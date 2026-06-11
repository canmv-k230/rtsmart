/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbd_core.h"
#include "usbd_adb.h"
#include "ipc/workqueue.h"

#define ADB_OUT_EP_IDX        0
#define ADB_IN_EP_IDX         1

#define ADB_STATE_READ_MSG    0
#define ADB_STATE_READ_DATA   1
#define ADB_STATE_WRITE_MSG   2
#define ADB_STATE_WRITE_DATA  3
#define ADB_STATE_AWRITE_MSG  4
#define ADB_STATE_AWRITE_DATA 5

#define MAX_PAYLOAD_V1        (4 * 1024)
#define MAX_PAYLOAD_V2        (256 * 1024)
#define MAX_PAYLOAD           MAX_PAYLOAD_V1
#define A_VERSION             0x01000000

#define A_SYNC                0x434e5953
#define A_CNXN                0x4e584e43
#define A_OPEN                0x4e45504f
#define A_OKAY                0x59414b4f
#define A_CLSE                0x45534c43
#define A_WRTE                0x45545257
#define A_AUTH                0x48545541

struct adb_msg {
    uint32_t command;     /* command identifier constant (A_CNXN, ...) */
    uint32_t arg0;        /* first argument                            */
    uint32_t arg1;        /* second argument                           */
    uint32_t data_length; /* length of payload (0 is allowed)          */
    uint32_t data_crc32;  /* crc32 of data payload                     */
    uint32_t magic;       /* command ^ 0xffffffff */
};

struct adb_packet {
    USB_MEM_ALIGNX struct adb_msg msg;
    USB_MEM_ALIGNX uint8_t payload[USB_ALIGN_UP(MAX_PAYLOAD, CONFIG_USB_ALIGN_SIZE)];
};

struct usbd_adb {
    uint8_t state;
    uint8_t common_state;
    uint8_t write_state;
    bool writable;
    bool is_open;
    uint32_t localid;
    uint32_t shell_remoteid;
    uint32_t file_remoteid;
    rt_sem_t w_sem;
    struct rt_work snd_okay;
    struct rt_work snd_clse;
} adb_client;

static struct usbd_endpoint adb_ep_data[2];

USB_NOCACHE_RAM_SECTION struct adb_packet tx_packet;
USB_NOCACHE_RAM_SECTION struct adb_packet rx_packet;
rt_bool_t use_adb_command;

static void adb_reset_state(void)
{
    extern void exit_adb_console(void);

    exit_adb_console();
    adb_client.state = 0;
    adb_client.common_state = ADB_STATE_READ_MSG;
    adb_client.write_state = 0;
    adb_client.writable = false;
    adb_client.is_open = false;
    adb_client.localid = 0;
    adb_client.shell_remoteid = 0;
    adb_client.file_remoteid = 0;

    if (adb_client.w_sem) {
        rt_sem_control(adb_client.w_sem, RT_IPC_CMD_RESET, (void *)1);
    }
}

static inline uint32_t adb_packet_checksum(struct adb_packet *packet)
{
    uint32_t sum = 0;
    uint32_t i;

    for (i = 0; i < packet->msg.data_length; ++i) {
        sum += (uint32_t)(packet->payload[i]);
    }

    return sum;
}

static uint32_t usbd_adb_get_remoteid(uint32_t localid)
{
    if (localid == ADB_SHELL_LOALID) {
        return adb_client.shell_remoteid;
    } else {
        return adb_client.file_remoteid;
    }
}

static void adb_send_msg(struct adb_packet *packet)
{
    adb_client.common_state = ADB_STATE_WRITE_MSG;

    packet->msg.data_crc32 = adb_packet_checksum(packet);
    packet->msg.magic = packet->msg.command ^ 0xffffffff;

    if (usbd_ep_start_write(0, adb_ep_data[ADB_IN_EP_IDX].ep_addr, (uint8_t *)&packet->msg, sizeof(struct adb_msg)) < 0) {
        adb_reset_state();
    }
}

static void adb_send_okay(struct adb_packet *packet, uint32_t localid)
{
    if (RT_EOK != rt_sem_trytake(adb_client.w_sem)) {
        if (RT_EOK != rt_work_submit(&adb_client.snd_okay, rt_tick_from_millisecond(5))) {
            USB_LOG_ERR("%s: failed to submit snd_okay work\n", __func__);
            return ;
        }
        return;
    }

    packet->msg.command = A_OKAY;
    packet->msg.arg0 = localid;
    packet->msg.arg1 = usbd_adb_get_remoteid(localid);
    packet->msg.data_length = 0;

    adb_send_msg(&tx_packet);
}

static void adb_send_close(struct adb_packet *packet, uint32_t localid, uint32_t remoteid)
{
    if (RT_EOK != rt_sem_trytake(adb_client.w_sem)) {
        if (RT_EOK != rt_work_submit(&adb_client.snd_clse, rt_tick_from_millisecond(5))) {
            USB_LOG_ERR("%s: failed to submit snd_clse work\n", __func__);
            return ;
        }
        return;
    }

    packet->msg.command = A_CLSE;
    packet->msg.arg0 = localid;
    packet->msg.arg1 = remoteid;
    packet->msg.data_length = 0;

    adb_send_msg(&tx_packet);
}

/*
 * Parse shell v2 payload format
 * Shell v2 payload may contain multiple sub-packets:
 *   sub-packet format: id(1 byte) + len(4 bytes little-endian) + data(len bytes)
 *
 * Example:
 *   Single char: 00 01 00 00 00 1b (id=0, len=1, data=0x1b)
 *   Multiple:    00 01 00 00 00 5b 00 01 00 00 00 41 (two sub-packets)
 *
 * This function extracts all data bytes and concatenates them into output buffer
 */
static uint32_t parse_shell_v2_payload(const uint8_t *payload, uint32_t payload_len, uint8_t *out_data, uint32_t out_max)
{
    uint32_t offset = 0;
    uint32_t total_data_len = 0;

    while (offset + 5 <= payload_len) {  /* Need at least 5 bytes for header */
        uint8_t id = payload[offset];
        uint32_t data_len = payload[offset + 1] |
                           (payload[offset + 2] << 8) |
                           (payload[offset + 3] << 16) |
                           (payload[offset + 4] << 24);

        offset += 5;  /* Skip header */

        /* Sanity check */
        if (offset + data_len > payload_len) {
            USB_LOG_ERR("[Shell V2] Invalid sub-packet: offset=%u data_len=%u payload_len=%u\r\n",
                        offset, data_len, payload_len);
            return 0;  /* Return error - invalid data */
        }

        /* Copy data to output buffer */
        if (total_data_len + data_len <= out_max) {
            memcpy(out_data + total_data_len, payload + offset, data_len);
            total_data_len += data_len;
        } else {
            USB_LOG_ERR("[Shell V2] Output buffer overflow\r\n");
            return 0;  /* Return error - buffer too small */
        }

        offset += data_len;  /* Skip data */
    }

    return total_data_len;
}

void usbd_adb_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    USB_LOG_DBG("[OUT] state=%d, nbytes=%d\r\n", adb_client.common_state, nbytes);

    if (adb_client.common_state == ADB_STATE_READ_MSG) {
        if (nbytes == 0) {
            /* Zero-Length Packet (ZLP) from USB - ignore and restart read */
            USB_LOG_DBG("[OUT] ZLP received, restarting read\r\n");
            if (usbd_ep_start_read(busid, adb_ep_data[ADB_OUT_EP_IDX].ep_addr, (uint8_t *)&rx_packet.msg, sizeof(struct adb_msg)) < 0) {
                adb_reset_state();
            }
            return;
        }

        if (nbytes != sizeof(struct adb_msg)) {
            USB_LOG_ERR("invalid adb msg size:%d\r\n", (unsigned int)nbytes);
            /* Recover state machine - restart message read */
            adb_client.common_state = ADB_STATE_READ_MSG;
            if (usbd_ep_start_read(busid, adb_ep_data[ADB_OUT_EP_IDX].ep_addr,
                                   (uint8_t *)&rx_packet.msg, sizeof(struct adb_msg)) < 0) {
                adb_reset_state();
            }
            return;
        }

        USB_LOG_DBG("command:%x arg0:%x arg1:%x len:%d\r\n",
                     rx_packet.msg.command,
                     rx_packet.msg.arg0,
                     rx_packet.msg.arg1,
                     rx_packet.msg.data_length);

        if (rx_packet.msg.data_length) {
            /* setup next out ep read transfer */
            adb_client.common_state = ADB_STATE_READ_DATA;
            if (usbd_ep_start_read(busid, adb_ep_data[ADB_OUT_EP_IDX].ep_addr, rx_packet.payload, rx_packet.msg.data_length) < 0) {
                adb_reset_state();
            }
        } else {
            if (rx_packet.msg.command == A_CLSE) {
                adb_client.writable = false;
                extern void exit_adb_console(void);
                exit_adb_console();
                rt_sem_control(adb_client.w_sem, RT_IPC_CMD_RESET, (void *)1);
            }
            adb_client.common_state = ADB_STATE_READ_MSG;
            /* setup first out ep read transfer */
            if (usbd_ep_start_read(busid, adb_ep_data[ADB_OUT_EP_IDX].ep_addr, (uint8_t *)&rx_packet.msg, sizeof(struct adb_msg)) < 0) {
                adb_reset_state();
            }
        }
    } else if (adb_client.common_state == ADB_STATE_READ_DATA) {
        switch (rx_packet.msg.command) {
            case A_SYNC:

                break;
            case A_CNXN: /* CONNECT(version, maxdata, "system-id-string") */
                char *support_feature = "device::"
                                        "ro.product.name=cherryadb;"
                                        "ro.product.model=cherrysh;"
                                        "ro.product.device=cherryadb;"
                                        "features=cmd,shell_v2";

                tx_packet.msg.command = A_CNXN;
                tx_packet.msg.arg0 = A_VERSION;
                tx_packet.msg.arg1 = MAX_PAYLOAD;
                tx_packet.msg.data_length = strlen(support_feature);
                memcpy(tx_packet.payload, support_feature, strlen(support_feature));

                adb_send_msg(&tx_packet);

                adb_client.writable = false;
                break;
            case A_OPEN: /* OPEN(local-id, 0, "destination") */
                rx_packet.payload[rx_packet.msg.data_length] = '\0';
                adb_client.is_open = true;

                extern void adb_enter();
                if (strncmp((const char *)rx_packet.payload, "shell,", 6) == 0) {
                    adb_client.localid = ADB_SHELL_LOALID;
                    adb_client.shell_remoteid = rx_packet.msg.arg0;
                    adb_enter();
                    adb_send_okay(&tx_packet, ADB_SHELL_LOALID);

                    USB_LOG_INFO("Open shell service, remoteid:%x\r\n", rx_packet.msg.arg0);
                } else if (strncmp((const char *)rx_packet.payload, "sync:", 5) == 0) {
                    adb_client.localid = ADB_FILE_LOALID;
                    adb_client.file_remoteid = rx_packet.msg.arg0;
                    adb_send_okay(&tx_packet, ADB_FILE_LOALID);
                    USB_LOG_INFO("Open file service, remoteid:%x\r\n", rx_packet.msg.arg0);
                }
                break;
            case A_OKAY:
                break;
            case A_CLSE:

                break;
            case A_WRTE: /* WRITE(local-id, remote-id, "data") */
                USB_LOG_DBG("[OUT] A_WRTE: shell_rid=%x file_rid=%x arg0=%x arg1=%x\r\n",
                            adb_client.shell_remoteid, adb_client.file_remoteid,
                            rx_packet.msg.arg0, rx_packet.msg.arg1);
                if ((rx_packet.msg.arg0 == adb_client.shell_remoteid) && (rx_packet.msg.arg1 == ADB_SHELL_LOALID)) {
                    if (adb_client.is_open) {
                        adb_client.is_open = false;
                        rx_packet.payload[1] = 0x1;
                        rx_packet.payload[1] = 0x1;
                        rx_packet.payload[2] = 0x0;
                        rx_packet.payload[3] = 0x0;
                        rx_packet.payload[4] = 0x0;
                        rx_packet.payload[5] = '\n';
                        rx_packet.msg.data_length = 6;
                    }

                    adb_send_okay(&tx_packet, rx_packet.msg.arg1);
                } else if ((rx_packet.msg.arg0 == adb_client.file_remoteid) && (rx_packet.msg.arg1 == ADB_FILE_LOALID)) {
                    USB_LOG_DBG("[OUT] File A_WRTE, sending OKAY (data_len=%d)\r\n", rx_packet.msg.data_length);
                    /* Flow control: only send OKAY if buffers available */
                    if (usbd_adb_can_send_file_okay()) {
                        adb_send_okay(&tx_packet, rx_packet.msg.arg1);
                    } else {
                        /* Buffer full - don't send OKAY, PC will stop sending */
                        adb_client.common_state = ADB_STATE_READ_MSG;
                        if (usbd_ep_start_read(busid, adb_ep_data[ADB_OUT_EP_IDX].ep_addr, (uint8_t *)&rx_packet.msg, sizeof(struct adb_msg)) < 0) {
                            adb_reset_state();
                        }
                    }
                } else {
                    adb_send_close(&tx_packet, 0, rx_packet.msg.arg0);
                }
                break;
            case A_AUTH:

                break;

            default:
                break;
        }
    }
}

void usbd_adb_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    (void)nbytes;

    USB_LOG_DBG("[IN] state=[%d,%d], cmd=[%x,%x]\r\n",
                adb_client.common_state, adb_client.write_state, rx_packet.msg.command, tx_packet.msg.command);

    if (adb_client.common_state == ADB_STATE_WRITE_MSG) {
        if (tx_packet.msg.data_length) {
            adb_client.common_state = ADB_STATE_WRITE_DATA;
            if (usbd_ep_start_write(busid, adb_ep_data[ADB_IN_EP_IDX].ep_addr, tx_packet.payload, tx_packet.msg.data_length) < 0) {
                adb_reset_state();
            }
        } else {
            if (rx_packet.msg.command == A_WRTE) {
                adb_client.writable = true;
                USB_LOG_DBG("[IN] A_WRTE done, localid=%d\r\n", adb_client.localid);
                if (adb_client.localid == ADB_SHELL_LOALID) {
                    /* Parse shell v2 payload and extract actual data */
                    static uint8_t shell_data[MAX_PAYLOAD];  /* Static to save stack */
                    uint32_t data_len = parse_shell_v2_payload(rx_packet.payload,
                                                               rx_packet.msg.data_length,
                                                               shell_data,
                                                               sizeof(shell_data));
                    if (data_len > 0) {
                        usbd_adb_notify_shell_read(shell_data, data_len);
                    }
                } else if (adb_client.localid == ADB_FILE_LOALID) {
                    usbd_adb_notify_file_read(rx_packet.payload, rx_packet.msg.data_length);
                }
            } else if ((rx_packet.msg.command == A_OPEN) && (rx_packet.msg.data_length != 34)) {
                if (adb_client.localid == ADB_SHELL_LOALID) {
                    usbd_adb_notify_shell_read(rx_packet.payload + 33, rx_packet.msg.data_length - 33);
                    use_adb_command = RT_TRUE;
                }
            }

            USB_LOG_DBG("[IN] -> READ_MSG, start_read\r\n");
            adb_client.common_state = ADB_STATE_READ_MSG;
            /* setup first out ep read transfer */
            if (usbd_ep_start_read(busid, adb_ep_data[ADB_OUT_EP_IDX].ep_addr, (uint8_t *)&rx_packet.msg, sizeof(struct adb_msg)) < 0) {
                adb_reset_state();
            }

            if ((tx_packet.msg.command == A_OKAY) || (tx_packet.msg.command == A_CLSE)) {
                rt_sem_release(adb_client.w_sem);
            }
        }
    } else if (adb_client.common_state == ADB_STATE_WRITE_DATA) {
        adb_client.common_state = ADB_STATE_READ_MSG;
        /* setup first out ep read transfer */
        if (usbd_ep_start_read(busid, adb_ep_data[ADB_OUT_EP_IDX].ep_addr, (uint8_t *)&rx_packet.msg, sizeof(struct adb_msg)) < 0) {
            adb_reset_state();
        }
    } else if (adb_client.write_state == ADB_STATE_AWRITE_MSG) {
        if (tx_packet.msg.data_length) {
            adb_client.write_state = ADB_STATE_AWRITE_DATA;
            if (usbd_ep_start_write(busid, adb_ep_data[ADB_IN_EP_IDX].ep_addr, tx_packet.payload, tx_packet.msg.data_length) < 0) {
                adb_reset_state();
            }
        } else {
        }
    } else if (adb_client.write_state == ADB_STATE_AWRITE_DATA) {
        rt_sem_release(adb_client.w_sem);
    }
}

void snd_okay_work(struct rt_work* work, void* work_data)
{
    adb_send_okay(&tx_packet, adb_client.localid);
}

void snd_clse_work(struct rt_work* work, void* work_data)
{
    adb_send_close(&tx_packet, 0, adb_client.shell_remoteid);
}

void adb_notify_handler(uint8_t busid, uint8_t event, void *arg)
{
    (void)arg;

    switch (event) {
        case USBD_EVENT_INIT:
            break;
        case USBD_EVENT_DEINIT:
        case USBD_EVENT_RESET:
            adb_reset_state();
            break;
        case USBD_EVENT_CONFIGURED:
            adb_client.common_state = ADB_STATE_READ_MSG;
            /* setup first out ep read transfer */
            if (usbd_ep_start_read(busid, adb_ep_data[ADB_OUT_EP_IDX].ep_addr, (uint8_t *)&rx_packet.msg, sizeof(struct adb_msg)) < 0) {
                adb_reset_state();
            }
            break;

        default:
            break;
    }
}

struct usbd_interface *usbd_adb_init_intf(uint8_t busid, struct usbd_interface *intf, uint8_t in_ep, uint8_t out_ep)
{
    (void)busid;

    intf->class_interface_handler = NULL;
    intf->class_endpoint_handler = NULL;
    intf->vendor_handler = NULL;
    intf->notify_handler = adb_notify_handler;

    adb_ep_data[ADB_OUT_EP_IDX].ep_addr = out_ep;
    adb_ep_data[ADB_OUT_EP_IDX].ep_cb = usbd_adb_bulk_out;
    adb_ep_data[ADB_IN_EP_IDX].ep_addr = in_ep;
    adb_ep_data[ADB_IN_EP_IDX].ep_cb = usbd_adb_bulk_in;

    usbd_add_endpoint(busid, &adb_ep_data[ADB_OUT_EP_IDX]);
    usbd_add_endpoint(busid, &adb_ep_data[ADB_IN_EP_IDX]);
    rt_work_init(&adb_client.snd_okay, snd_okay_work, &adb_client);
    rt_work_init(&adb_client.snd_clse, snd_clse_work, &adb_client);

    adb_client.w_sem = rt_sem_create("w_sem", 1, RT_IPC_FLAG_FIFO);
    if (adb_client.w_sem == RT_NULL) {
        rt_kprintf("[ADB] Failed to create WX semaphore\n");
        return NULL;
    }

    return intf;
}

bool usbd_adb_can_write(void)
{
    return adb_client.writable;
}

int usbd_adb_write(uint32_t localid, const uint8_t *data, uint32_t len)
{
    struct adb_packet *packet;

    if (RT_EOK != rt_sem_take(adb_client.w_sem, rt_tick_from_millisecond(3000))) {
        USB_LOG_ERR("%s: failed to take w_sem\n", __func__);
        return -1;
    }

    packet = &tx_packet;
    packet->msg.command = A_WRTE;
    packet->msg.arg0 = localid;
    packet->msg.arg1 = usbd_adb_get_remoteid(localid);

    if (localid == ADB_FILE_LOALID) {
        /* Check buffer overflow for file channel */
        if (len > MAX_PAYLOAD) {
            USB_LOG_ERR("File data too large: %u (max %u)\r\n", len, MAX_PAYLOAD);
            return -1;
        }
        packet->msg.data_length = len;
        memcpy(packet->payload, data, len);
    } else if (localid == ADB_SHELL_LOALID) {
        /* shell v2 - check buffer overflow (5 byte header + data) */
        if (len + 5 > MAX_PAYLOAD) {
            USB_LOG_ERR("Shell data too large: %u (max %u)\r\n", len, MAX_PAYLOAD - 5);
            return -1;
        }

        uint8_t *payload_ptr;
        packet->msg.data_length = len + 5;
        payload_ptr = packet->payload;
        *payload_ptr = (uint8_t)0x1;
        payload_ptr++;

        *payload_ptr++ = (uint8_t)(len & 0xFF);
        *payload_ptr++ = (uint8_t)((len >> 8) & 0xFF);
        *payload_ptr++ = (uint8_t)((len >> 16) & 0xFF);
        *payload_ptr++ = (uint8_t)((len >> 24) & 0xFF);
        memcpy(payload_ptr, data, len);
    }

    packet->msg.data_crc32 = adb_packet_checksum(packet);
    packet->msg.magic = packet->msg.command ^ 0xffffffff;

    adb_client.write_state = ADB_STATE_AWRITE_MSG;
    if (usbd_ep_start_write(0, adb_ep_data[ADB_IN_EP_IDX].ep_addr, (uint8_t *)&packet->msg, sizeof(struct adb_msg)) < 0) {
        adb_reset_state();
        return -1;
    }
    return 0;
}

void usbd_adb_close(uint32_t localid)
{
    adb_send_close(&tx_packet, 0, usbd_adb_get_remoteid(localid));
}

void usbd_adb_send_file_okay(void)
{
    /* Send transport OKAY for file channel (called from worker thread) */
    adb_send_okay(&tx_packet, ADB_FILE_LOALID);
}

RT_WEAK rt_bool_t is_use_adb_console()
{
    return RT_FALSE;
}

RT_WEAK int adb_exit(void)
{
    return 0;
}
