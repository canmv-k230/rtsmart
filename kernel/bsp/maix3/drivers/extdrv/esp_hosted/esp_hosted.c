/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RT-Thread host port for the ESP-Hosted-MCU full-duplex SPI protocol.
 */
#include "esp_hosted.h"
#include "esp_hosted_country.h"
#include "esp_hosted_proto.h"

#include <board.h>
#include <drv_fpioa.h>
#include <drv_gpio.h>
#include <drivers/spi.h>
#include <rtdevice.h>
#include <rthw.h>
#include <wlan_dev.h>
#include <wlan_mgnt.h>

#define DBG_TAG "esp.hosted"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define EH_SPI_FRAME_SIZE          1600
#define EH_SPI_DATA_WIDTH          32
#define EH_SPI_WORD_SIZE           (EH_SPI_DATA_WIDTH / 8)
#define EH_SPI_FRAME_WORDS         (EH_SPI_FRAME_SIZE / EH_SPI_WORD_SIZE)
#define EH_EMPTY_RX_BACKOFF_MS     100
#define EH_INVALID_RX_LOG_LIMIT    1
#define EH_HEADER_SIZE             12
#define EH_MAX_PAYLOAD             (EH_SPI_FRAME_SIZE - EH_HEADER_SIZE)
#define EH_MAX_RPC_MESSAGE         8192
#define EH_RPC_TX_SIZE             1024
#define EH_CTRL_QUEUE_DEPTH        8
#define EH_DATA_QUEUE_DEPTH        16
#define EH_WLAN_EVENT_QUEUE_DEPTH  8
#define EH_MQ_POOL_SIZE(depth, type) \
    ((depth) * (RT_ALIGN(sizeof(type), RT_ALIGN_SIZE) + sizeof(void *)))
#define EH_EVENT_XFER              (1U << 0)
#define EH_RPC_TYPE_REQUEST        1
#define EH_RPC_TYPE_RESPONSE       2
#define EH_RPC_TYPE_EVENT          3
#define EH_IF_STA                  1
#define EH_IF_AP                   2
#define EH_IF_SERIAL               3
#define EH_IF_PRIV                 5
#define EH_IF_MAX                  8
#define EH_FLAG_MORE_FRAGMENT      (1U << 0)
#define EH_PRIV_EVENT_INIT         0x22
#define EH_PRIV_TAG_CAPABILITY     0x11
#define EH_PRIV_TAG_CHIP_ID        0x12
#define EH_PRIV_TAG_EXT_CAP        0x16
#define EH_PRIV_TAG_FW_VERSION     0x17
#define EH_HOST_CAPABILITIES       0x44
#define EH_HOST_CHIP_ID            0x45
#define EH_HOST_RAW_TP             0x46
#define EH_HOST_THROTTLE_HIGH      0x47
#define EH_HOST_THROTTLE_LOW       0x48
#define EH_FLOW_CTRL_ON            1
#define EH_FLOW_CTRL_OFF           2
#define EH_WIFI_MODE_STA           1
#define EH_WIFI_MODE_AP            2
#define EH_WIFI_IF_STA             0
#define EH_WIFI_IF_AP              1
#define EH_WIFI_INIT_MAGIC         0x1f2f3f4f

#if defined(ESP_HOSTED_SPI_BUS_SPI1)
#define EH_SPI_BUS_NAME            "spi1"
#define EH_SPI_CLK_FUNC            QSPI0_CLK
#define EH_SPI_D0_FUNC             QSPI0_D0
#define EH_SPI_D1_FUNC             QSPI0_D1
#elif defined(ESP_HOSTED_SPI_BUS_SPI2)
#define EH_SPI_BUS_NAME            "spi2"
#define EH_SPI_CLK_FUNC            QSPI1_CLK
#define EH_SPI_D0_FUNC             QSPI1_D0
#define EH_SPI_D1_FUNC             QSPI1_D1
#else /* default: spi0 */
#define EH_SPI_BUS_NAME            "spi0"
#define EH_SPI_CLK_FUNC            OSPI_CLK
#define EH_SPI_D0_FUNC             OSPI_D0
#define EH_SPI_D1_FUNC             OSPI_D1
#endif

#ifndef ESP_HOSTED_EVENT_THREAD_STACK_SIZE
#define ESP_HOSTED_EVENT_THREAD_STACK_SIZE 8192
#endif

enum eh_rpc_id
{
    EH_REQ_GET_MAC = 257,
    EH_REQ_SET_MAC = 258,
    EH_REQ_SET_MODE = 260,
    EH_REQ_SET_PS = 270,
    EH_REQ_GET_PS = 271,
    EH_REQ_WIFI_INIT = 278,
    EH_REQ_WIFI_START = 280,
    EH_REQ_WIFI_CONNECT = 282,
    EH_REQ_WIFI_DISCONNECT = 283,
    EH_REQ_WIFI_SET_CONFIG = 284,
    EH_REQ_SCAN_START = 286,
    EH_REQ_SCAN_STOP = 287,
    EH_REQ_SCAN_GET_NUM = 288,
    EH_REQ_SCAN_GET_RECORDS = 289,
    EH_REQ_WIFI_DEAUTH_STA = 293,
    EH_REQ_SET_CHANNEL = 301,
    EH_REQ_GET_CHANNEL = 302,
    EH_REQ_WIFI_AP_GET_STA_AID = 312,
    EH_REQ_SET_COUNTRY_CODE = 334,
    EH_REQ_GET_COUNTRY_CODE = 335,
    EH_REQ_GET_RSSI = 341,
    EH_EVENT_AP_CONNECTED = 771,
    EH_EVENT_AP_DISCONNECTED = 772,
    EH_EVENT_WIFI_NO_ARGS = 773,
    EH_EVENT_SCAN_DONE = 774,
    EH_EVENT_STA_CONNECTED = 775,
    EH_EVENT_STA_DISCONNECTED = 776,
};

struct eh_tx_item
{
    uint8_t interface;
    uint8_t flags;
    uint16_t length;
    uint8_t *data;
};

struct eh_wlan_event
{
    uint8_t ap_interface;
    uint8_t event;
    uint8_t length;
    uint8_t data[sizeof(struct rt_wlan_info)];
};

struct eh_context
{
    struct rt_qspi_device spi;
    struct rt_wlan_device sta;
    struct rt_wlan_device ap;
    struct rt_event xfer_event;
    struct rt_messagequeue ctrl_queue;
    struct rt_messagequeue data_queue;
    struct rt_messagequeue wlan_event_queue;
    struct rt_mutex rpc_mutex;
    struct rt_mutex state_mutex;
    struct rt_semaphore rpc_done;
    rt_thread_t thread;
    rt_thread_t event_thread;
    volatile rt_bool_t ready;
    volatile rt_bool_t config_pending;
    volatile rt_bool_t dummy_needed;
    volatile rt_bool_t tx_throttled;
    volatile rt_bool_t connecting;
    rt_bool_t boot_sync_pending;
    rt_bool_t boot_saw_handshake_inactive;
    uint8_t empty_rx_log_count;
    uint8_t invalid_rx_log_count;
    rt_bool_t wifi_initialized;
    rt_bool_t wifi_started;
    uint8_t wifi_mode;
    uint8_t coprocessor_id;
    uint32_t coprocessor_version;
    uint32_t rpc_uid;
    volatile uint32_t pending_uid;
    uint16_t response_id;
    size_t response_length;
    size_t serial_fragment_length;
    uint8_t response[EH_MAX_RPC_MESSAGE];
    uint8_t serial_fragment[EH_MAX_RPC_MESSAGE];
    uint8_t rpc_tx[EH_RPC_TX_SIZE];
    uint8_t rpc_tlv[EH_RPC_TX_SIZE + 12];
    uint8_t ctrl_pool[EH_MQ_POOL_SIZE(EH_CTRL_QUEUE_DEPTH, struct eh_tx_item)]
        __attribute__((aligned(RT_ALIGN_SIZE)));
    uint8_t data_pool[EH_MQ_POOL_SIZE(EH_DATA_QUEUE_DEPTH, struct eh_tx_item)]
        __attribute__((aligned(RT_ALIGN_SIZE)));
    uint8_t wlan_event_pool[EH_MQ_POOL_SIZE(EH_WLAN_EVENT_QUEUE_DEPTH, struct eh_wlan_event)]
        __attribute__((aligned(RT_ALIGN_SIZE)));
    uint8_t tx_frame[EH_SPI_FRAME_SIZE] __attribute__((aligned(64)));
    uint8_t rx_frame[EH_SPI_FRAME_SIZE] __attribute__((aligned(64)));
    uint32_t tx_words[EH_SPI_FRAME_WORDS] __attribute__((aligned(64)));
    uint32_t rx_words[EH_SPI_FRAME_WORDS] __attribute__((aligned(64)));
};

typedef int (*eh_rpc_parser_t)(uint16_t response_id, const uint8_t *data,
                               size_t length, void *argument);

static struct eh_context g_eh;

static rt_err_t eh_wlan_get_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[]);

static uint16_t eh_get_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void eh_put_le16(uint8_t *data, uint16_t value)
{
    data[0] = value & 0xff;
    data[1] = value >> 8;
}

static uint16_t eh_checksum(const uint8_t *data, size_t length)
{
    uint16_t result = 0;

    while (length--)
    {
        result += *data++;
    }
    return result;
}

static rt_bool_t eh_gpio_active(int pin, rt_bool_t active_low)
{
    if (pin < 0)
    {
        return RT_TRUE;
    }
    return kd_pin_read(pin) == (active_low ? GPIO_PV_LOW : GPIO_PV_HIGH);
}

static int eh_gpio_value(int pin)
{
    return pin < 0 ? -1 : kd_pin_read(pin);
}

static void eh_gpio_irq(void *argument)
{
    struct eh_context *context = argument;

    rt_event_send(&context->xfer_event, EH_EVENT_XFER);
}

static rt_err_t eh_queue_frame(uint8_t interface, uint8_t flags,
                               const void *data, size_t length, rt_bool_t control)
{
    struct eh_tx_item item;
    rt_err_t result;

    if (length > EH_MAX_PAYLOAD || (length && !data))
    {
        return -RT_EINVAL;
    }

    rt_memset(&item, 0, sizeof(item));
    item.interface = interface;
    item.flags = flags;
    item.length = length;
    if (length)
    {
        item.data = rt_malloc(length);
        if (!item.data)
        {
            return -RT_ENOMEM;
        }
        rt_memcpy(item.data, data, length);
    }

    result = rt_mq_send(control ? &g_eh.ctrl_queue : &g_eh.data_queue,
                        &item, sizeof(item));
    if (result != RT_EOK)
    {
        rt_free(item.data);
        return result;
    }
    rt_event_send(&g_eh.xfer_event, EH_EVENT_XFER);
    return RT_EOK;
}

static int eh_dequeue_frame(struct eh_tx_item *item)
{
    if (rt_mq_recv(&g_eh.ctrl_queue, item, sizeof(*item), RT_WAITING_NO) == RT_EOK)
    {
        return 1;
    }
    if (!g_eh.tx_throttled &&
        rt_mq_recv(&g_eh.data_queue, item, sizeof(*item), RT_WAITING_NO) == RT_EOK)
    {
        return 1;
    }
    return 0;
}

static void eh_build_frame(const struct eh_tx_item *item)
{
    uint16_t checksum;

    rt_memset(g_eh.tx_frame, 0, sizeof(g_eh.tx_frame));
    if (!item)
    {
        g_eh.tx_frame[0] = EH_IF_MAX;
        return;
    }

    g_eh.tx_frame[0] = item->interface & 0x0f;
    g_eh.tx_frame[1] = item->flags;
    eh_put_le16(g_eh.tx_frame + 2, item->length);
    eh_put_le16(g_eh.tx_frame + 4, EH_HEADER_SIZE);
    if (item->length)
    {
        rt_memcpy(g_eh.tx_frame + EH_HEADER_SIZE, item->data, item->length);
    }
#ifdef ESP_HOSTED_CHECKSUM
    checksum = eh_checksum(g_eh.tx_frame, EH_HEADER_SIZE + item->length);
    eh_put_le16(g_eh.tx_frame + 6, checksum);
#else
    (void)checksum;
#endif
}

static rt_wlan_security_t eh_security_from_esp(int authmode)
{
    switch (authmode)
    {
    case 0: return SECURITY_OPEN;
    case 1: return SECURITY_WEP_PSK;
    case 2: return SECURITY_WPA_AES_PSK;
    case 3: return SECURITY_WPA2_AES_PSK;
    case 4: return SECURITY_WPA_WPA2_MIXED_PSK;
    case 5: return SECURITY_WPA2_AES_8021X;
    case 6: return SECURITY_WPA3_AES_PSK;
    case 7: return SECURITY_WPA3_AES_PSK;
    default: return SECURITY_UNKNOWN;
    }
}

static int eh_authmode_from_security(rt_wlan_security_t security)
{
    if (security == SECURITY_OPEN)
    {
        return 0;
    }
    if (security & WPA3_SECURITY)
    {
        return 6;
    }
    if ((security & WPA_SECURITY) && (security & WPA2_SECURITY))
    {
        return 4;
    }
    if (security & WPA2_SECURITY)
    {
        return 3;
    }
    if (security & WPA_SECURITY)
    {
        return 2;
    }
    if (security & WEP_ENABLED)
    {
        return 1;
    }
    return 0;
}

static void eh_report_event(struct rt_wlan_device *device,
                            rt_wlan_dev_event_t event, void *data, int length)
{
    struct rt_wlan_buff buffer;

    if (data && length)
    {
        buffer.data = data;
        buffer.len = length;
        rt_wlan_dev_indicate_event_handle(device, event, &buffer);
    }
    else
    {
        rt_wlan_dev_indicate_event_handle(device, event, RT_NULL);
    }
}

static void eh_queue_wlan_event(rt_bool_t ap_interface,
                                rt_wlan_dev_event_t event,
                                const void *data, size_t length)
{
    struct eh_wlan_event wlan_event;

    rt_memset(&wlan_event, 0, sizeof(wlan_event));
    wlan_event.ap_interface = ap_interface;
    wlan_event.event = event;
    wlan_event.length = length < sizeof(wlan_event.data) ? length : sizeof(wlan_event.data);
    if (wlan_event.length)
    {
        rt_memcpy(wlan_event.data, data, wlan_event.length);
    }
    if (rt_mq_send(&g_eh.wlan_event_queue, &wlan_event, sizeof(wlan_event)) != RT_EOK)
    {
        LOG_W("WLAN event queue is full");
    }
}

static void eh_wlan_event_thread(void *argument)
{
    struct eh_wlan_event event;
    rt_err_t result;

    (void)argument;
    while (!g_eh.ready)
    {
        rt_thread_mdelay(100);
    }

    result = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
    if (result != RT_EOK)
    {
        LOG_E("cannot enable station WLAN mode: %d", result);
    }
    else
    {
#ifdef RT_USING_NETDEV
        if (g_eh.sta.netdev)
        {
            LOG_I("station lwIP netdev registered");
        }
        else
        {
            LOG_E("station WLAN mode has no lwIP netdev");
        }
#else
        LOG_W("RT_USING_NETDEV is disabled; no station netdev was created");
#endif
    }

    result = rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP);
    if (result != RT_EOK)
    {
        LOG_E("cannot enable AP WLAN mode: %d", result);
    }
    else
    {
#ifdef RT_USING_NETDEV
        if (g_eh.ap.netdev)
        {
            LOG_I("AP lwIP netdev registered");
        }
        else
        {
            LOG_E("AP WLAN mode has no lwIP netdev");
        }
#else
        LOG_W("RT_USING_NETDEV is disabled; no AP netdev was created");
#endif
    }

    while (1)
    {
        if (rt_mq_recv(&g_eh.wlan_event_queue, &event, sizeof(event),
                       RT_WAITING_FOREVER) == RT_EOK)
        {
            eh_report_event(event.ap_interface ? &g_eh.ap : &g_eh.sta,
                            event.event,
                            event.length ? event.data : RT_NULL,
                            event.length);
        }
    }
}

static void eh_handle_rpc_event(uint16_t id, const uint8_t *data, size_t length)
{
    struct eh_pb_field field;
    struct eh_pb_field nested;
    struct rt_wlan_info info;

    switch (id)
    {
    case EH_EVENT_STA_CONNECTED:
        g_eh.connecting = RT_FALSE;
        eh_queue_wlan_event(RT_FALSE, RT_WLAN_DEV_EVT_CONNECT, RT_NULL, 0);
        break;
    case EH_EVENT_STA_DISCONNECTED:
        if (g_eh.connecting)
        {
            g_eh.connecting = RT_FALSE;
            eh_queue_wlan_event(RT_FALSE, RT_WLAN_DEV_EVT_CONNECT_FAIL, RT_NULL, 0);
        }
        else
        {
            eh_queue_wlan_event(RT_FALSE, RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL, 0);
        }
        break;
    case EH_EVENT_AP_CONNECTED:
    case EH_EVENT_AP_DISCONNECTED:
        INVALID_INFO(&info);
        if (eh_pb_find(data, length, 2, &field) <= 0 || field.wire_type != 2 ||
            field.length != sizeof(info.bssid))
        {
            LOG_W("invalid SoftAP station event %u", id);
            break;
        }
        rt_memcpy(info.bssid, field.data, sizeof(info.bssid));
        eh_queue_wlan_event(RT_TRUE,
                            id == EH_EVENT_AP_CONNECTED ? RT_WLAN_DEV_EVT_AP_ASSOCIATED
                                                        : RT_WLAN_DEV_EVT_AP_DISASSOCIATED,
                            &info, sizeof(info));
        break;
    case EH_EVENT_WIFI_NO_ARGS:
        if (eh_pb_find(data, length, 2, &field) > 0 && field.wire_type == 0)
        {
            LOG_D("Wi-Fi event %d", (int)field.value);
        }
        break;
    case EH_EVENT_SCAN_DONE:
        if (eh_pb_find(data, length, 2, &field) > 0 && field.wire_type == 2 &&
            eh_pb_find(field.data, field.length, 2, &nested) > 0)
        {
            LOG_D("scan event: %d APs", (int)nested.value);
        }
        break;
    default:
        LOG_D("ignored RPC event %u", id);
        break;
    }
}

static void eh_handle_rpc_message(const uint8_t *data, size_t length)
{
    struct eh_pb_reader reader;
    struct eh_pb_field field;
    const uint8_t *payload = RT_NULL;
    size_t payload_length = 0;
    uint32_t type = 0;
    uint32_t id = 0;
    uint32_t uid = 0;
    int result;

    eh_pb_reader_init(&reader, data, length);
    while ((result = eh_pb_next(&reader, &field)) > 0)
    {
        if (field.number == 1 && field.wire_type == 0)
        {
            type = field.value;
        }
        else if (field.number == 2 && field.wire_type == 0)
        {
            id = field.value;
        }
        else if (field.number == 3 && field.wire_type == 0)
        {
            uid = field.value;
        }
        else if (field.wire_type == 2)
        {
            payload = field.data;
            payload_length = field.length;
        }
    }
    if (result < 0 || !id || !payload)
    {
        LOG_W("invalid RPC message");
        return;
    }

    if (type == EH_RPC_TYPE_RESPONSE && uid == g_eh.pending_uid)
    {
        if (payload_length > sizeof(g_eh.response))
        {
            payload_length = sizeof(g_eh.response);
        }
        rt_memcpy(g_eh.response, payload, payload_length);
        g_eh.response_length = payload_length;
        g_eh.response_id = id;
        rt_sem_release(&g_eh.rpc_done);
    }
    else if (type == EH_RPC_TYPE_EVENT)
    {
        eh_handle_rpc_event(id, payload, payload_length);
    }
}

static void eh_handle_serial(const uint8_t *data, size_t length, uint8_t flags)
{
    const uint8_t *rpc_data;
    size_t rpc_length;
    size_t fixed_length = 12;

    if (length > sizeof(g_eh.serial_fragment) - g_eh.serial_fragment_length)
    {
        LOG_W("RPC fragment overflow");
        g_eh.serial_fragment_length = 0;
        return;
    }
    rt_memcpy(g_eh.serial_fragment + g_eh.serial_fragment_length, data, length);
    g_eh.serial_fragment_length += length;
    if (flags & EH_FLAG_MORE_FRAGMENT)
    {
        return;
    }

    data = g_eh.serial_fragment;
    length = g_eh.serial_fragment_length;
    g_eh.serial_fragment_length = 0;
    if (length < fixed_length || data[0] != 0x01 || eh_get_le16(data + 1) != 6 ||
        (rt_memcmp(data + 3, "RPCRsp", 6) != 0 && rt_memcmp(data + 3, "RPCEvt", 6) != 0) ||
        data[9] != 0x02)
    {
        LOG_W("invalid RPC TLV");
        return;
    }
    rpc_length = eh_get_le16(data + 10);
    if (rpc_length > length - fixed_length)
    {
        LOG_W("truncated RPC TLV");
        return;
    }
    rpc_data = data + fixed_length;
    eh_handle_rpc_message(rpc_data, rpc_length);
}

static rt_err_t eh_send_private_config(uint8_t chip_id)
{
    uint8_t event[17] = {
        EH_PRIV_EVENT_INIT, 15,
        EH_HOST_CAPABILITIES, 1, 0,
        EH_HOST_CHIP_ID, 1, 0,
        EH_HOST_RAW_TP, 1, 0,
        EH_HOST_THROTTLE_HIGH, 1, 0,
        EH_HOST_THROTTLE_LOW, 1, 0,
    };

    event[7] = chip_id;
    return eh_queue_frame(EH_IF_PRIV, 0, event, sizeof(event), RT_TRUE);
}

static void eh_handle_private(const uint8_t *data, size_t length)
{
    size_t offset = 2;
    uint8_t chip_id = 0xff;
    uint8_t capabilities = 0;
    uint32_t version = 0;
    rt_err_t result;

    if (length < 2 || data[0] != EH_PRIV_EVENT_INIT || data[1] > length - 2)
    {
        return;
    }

    while (offset + 2 <= length)
    {
        uint8_t tag = data[offset];
        uint8_t item_length = data[offset + 1];
        const uint8_t *value = data + offset + 2;

        if (item_length > length - offset - 2)
        {
            return;
        }
        if (tag == EH_PRIV_TAG_CHIP_ID && item_length == 1)
        {
            chip_id = value[0];
        }
        else if (tag == EH_PRIV_TAG_CAPABILITY && item_length == 1)
        {
            capabilities = value[0];
        }
        else if (tag == EH_PRIV_TAG_FW_VERSION && item_length == 4)
        {
            version = (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
                      ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
        }
        else if (tag == EH_PRIV_TAG_EXT_CAP)
        {
            /* Extended capabilities are transport-specific and informational here. */
        }
        offset += item_length + 2;
    }

    if (chip_id == 0xff)
    {
        LOG_E("ESP-Hosted init event has no coprocessor ID");
        return;
    }
    if (!(capabilities & (1U << 5)))
    {
        LOG_W("coprocessor did not advertise full-duplex SPI WLAN");
    }
    g_eh.coprocessor_id = chip_id;
    g_eh.coprocessor_version = version;
    g_eh.config_pending = RT_TRUE;
    result = eh_send_private_config(chip_id);
    if (result != RT_EOK)
    {
        g_eh.config_pending = RT_FALSE;
        LOG_E("failed to queue coprocessor configuration: %d", result);
    }
}

static rt_bool_t eh_process_rx_frame(rt_bool_t *malformed)
{
    uint16_t length = eh_get_le16(g_eh.rx_frame + 2);
    uint16_t offset = eh_get_le16(g_eh.rx_frame + 4);
    uint16_t received_checksum = eh_get_le16(g_eh.rx_frame + 6);
    uint16_t calculated_checksum;
    uint8_t interface = g_eh.rx_frame[0] & 0x0f;
    uint8_t flow_control = g_eh.rx_frame[10] & 0x03;
    const uint8_t *payload;

    if (malformed)
    {
        *malformed = RT_FALSE;
    }

    if (flow_control == EH_FLOW_CTRL_ON)
    {
        g_eh.tx_throttled = RT_TRUE;
    }
    else if (flow_control == EH_FLOW_CTRL_OFF)
    {
        g_eh.tx_throttled = RT_FALSE;
    }
    if (!length)
    {
        /* ESP_MAX_IF is the normal dummy frame used when the coprocessor has
         * no payload for this full-duplex transaction. */
        if (interface != EH_IF_MAX && g_eh.empty_rx_log_count < EH_INVALID_RX_LOG_LIMIT)
        {
            LOG_W("invalid empty RX header: interface=%u mode=%d HS=%d DR=%d; check D1/MISO wiring",
                  interface, ESP_HOSTED_SPI_MODE,
                  eh_gpio_value(ESP_HOSTED_HANDSHAKE_PIN),
                  eh_gpio_value(ESP_HOSTED_DATA_READY_PIN));
            g_eh.empty_rx_log_count++;
        }
        if (interface != EH_IF_MAX && malformed)
        {
            *malformed = RT_TRUE;
        }
        else if (interface == EH_IF_MAX)
        {
            g_eh.invalid_rx_log_count = 0;
        }
        return RT_FALSE;
    }
    LOG_D("RX frame: interface=%u length=%u offset=%u", interface, length, offset);
    if (length > EH_MAX_PAYLOAD || offset != EH_HEADER_SIZE)
    {
        if (g_eh.invalid_rx_log_count < EH_INVALID_RX_LOG_LIMIT)
        {
            LOG_W("invalid frame: interface=%u length=%u offset=%u", interface, length, offset);
            g_eh.invalid_rx_log_count++;
        }
        if (malformed)
        {
            *malformed = RT_TRUE;
        }
        return RT_FALSE;
    }
#ifdef ESP_HOSTED_CHECKSUM
    g_eh.rx_frame[6] = 0;
    g_eh.rx_frame[7] = 0;
    calculated_checksum = eh_checksum(g_eh.rx_frame, offset + length);
    if (calculated_checksum != received_checksum)
    {
        if (g_eh.invalid_rx_log_count < EH_INVALID_RX_LOG_LIMIT)
        {
            LOG_W("checksum mismatch: received=%u calculated=%u",
                  received_checksum, calculated_checksum);
            g_eh.invalid_rx_log_count++;
        }
        if (malformed)
        {
            *malformed = RT_TRUE;
        }
        return RT_FALSE;
    }
#else
    (void)received_checksum;
    (void)calculated_checksum;
#endif

    payload = g_eh.rx_frame + offset;
    switch (interface)
    {
    case EH_IF_STA:
        rt_wlan_dev_report_data(&g_eh.sta, (void *)payload, length);
        break;
    case EH_IF_AP:
        rt_wlan_dev_report_data(&g_eh.ap, (void *)payload, length);
        break;
    case EH_IF_SERIAL:
        eh_handle_serial(payload, length, g_eh.rx_frame[1]);
        break;
    case EH_IF_PRIV:
        eh_handle_private(payload, length);
        break;
    default:
        LOG_D("ignored interface %u", interface);
        break;
    }
    g_eh.invalid_rx_log_count = 0;
    return RT_TRUE;
}

static rt_size_t eh_spi_transfer_frame(void)
{
    struct rt_qspi_message message;
    rt_size_t result;
    rt_size_t index;

    /* The controller shifts the most-significant bit of each 32-bit FIFO
     * entry first. Pack explicitly so the byte stream on the wire remains
     * identical to an 8-bit SPI transfer on this little-endian CPU. */
    for (index = 0; index < EH_SPI_FRAME_WORDS; index++)
    {
        const uint8_t *source = g_eh.tx_frame + index * EH_SPI_WORD_SIZE;

        g_eh.tx_words[index] = ((uint32_t)source[0] << 24) |
                               ((uint32_t)source[1] << 16) |
                               ((uint32_t)source[2] << 8) |
                               source[3];
    }

    rt_memset(&message, 0, sizeof(message));
    rt_memset(g_eh.rx_words, 0, sizeof(g_eh.rx_words));
    message.parent.send_buf = g_eh.tx_words;
    message.parent.recv_buf = g_eh.rx_words;
    message.parent.length = EH_SPI_FRAME_SIZE;
    message.parent.cs_take = ESP_HOSTED_SPI_CS_PIN >= 0;
    message.parent.cs_release = ESP_HOSTED_SPI_CS_PIN >= 0;
    message.qspi_data_lines = 1;

    result = rt_qspi_transfer_message(&g_eh.spi, &message);
    if (result != EH_SPI_FRAME_SIZE)
    {
        return result;
    }

    for (index = 0; index < EH_SPI_FRAME_WORDS; index++)
    {
        uint32_t word = g_eh.rx_words[index];
        uint8_t *destination = g_eh.rx_frame + index * EH_SPI_WORD_SIZE;

        destination[0] = word >> 24;
        destination[1] = word >> 16;
        destination[2] = word >> 8;
        destination[3] = word;
    }
    return result;
}

static rt_bool_t eh_rx_frame_is_filled(uint32_t value)
{
    rt_size_t index;

    for (index = 0; index < EH_SPI_FRAME_WORDS; index++)
    {
        if (g_eh.rx_words[index] != value)
        {
            return RT_FALSE;
        }
    }
    return RT_TRUE;
}

static void eh_transfer_thread(void *argument)
{
    struct eh_tx_item item;
    struct eh_tx_item *item_pointer;
    rt_uint32_t event;
    int transactions;
    rt_bool_t waiting_for_handshake = RT_FALSE;

    (void)argument;
    while (1)
    {
        rt_event_recv(&g_eh.xfer_event, EH_EVENT_XFER,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      rt_tick_from_millisecond(10), &event);

        for (transactions = 0; transactions < 32; transactions++)
        {
            rt_bool_t data_ready;
            rt_bool_t have_item;
            rt_bool_t stuck_rx = RT_FALSE;
            rt_bool_t malformed_rx = RT_FALSE;
            rt_bool_t rx_processed;
            rt_bool_t transfer_ok = RT_FALSE;
            rt_bool_t transport_config;

            if (g_eh.boot_sync_pending)
            {
                rt_bool_t handshake_active = eh_gpio_active(ESP_HOSTED_HANDSHAKE_PIN,
#ifdef ESP_HOSTED_HANDSHAKE_ACTIVE_LOW
                                                             RT_TRUE
#else
                                                             RT_FALSE
#endif
                                                             );

                if (!handshake_active)
                {
                    g_eh.boot_saw_handshake_inactive = RT_TRUE;
                    break;
                }
                if (!g_eh.boot_saw_handshake_inactive)
                {
                    break;
                }
                g_eh.boot_sync_pending = RT_FALSE;
                LOG_I("coprocessor SPI handshake ready: HS=%d DR=%d",
                      eh_gpio_value(ESP_HOSTED_HANDSHAKE_PIN),
                      eh_gpio_value(ESP_HOSTED_DATA_READY_PIN));
            }

            if (ESP_HOSTED_DATA_READY_PIN < 0)
            {
                /* Without DR, poll one transaction per worker wake-up. */
                data_ready = RT_TRUE;
            }
            else
            {
                data_ready = eh_gpio_active(ESP_HOSTED_DATA_READY_PIN,
#ifdef ESP_HOSTED_DATA_READY_ACTIVE_LOW
                                            RT_TRUE
#else
                                            RT_FALSE
#endif
                                            );
            }
            have_item = eh_dequeue_frame(&item);
            transport_config = have_item && g_eh.config_pending &&
                               item.interface == EH_IF_PRIV && item.length == 17 &&
                               item.data && item.data[0] == EH_PRIV_EVENT_INIT;

            if (!have_item && !data_ready && !g_eh.dummy_needed)
            {
                break;
            }
            if (!eh_gpio_active(ESP_HOSTED_HANDSHAKE_PIN,
#ifdef ESP_HOSTED_HANDSHAKE_ACTIVE_LOW
                                RT_TRUE
#else
                                RT_FALSE
#endif
                                ))
            {
                if (!waiting_for_handshake)
                {
                    LOG_D("waiting for handshake: HS=%d DR=%d",
                          eh_gpio_value(ESP_HOSTED_HANDSHAKE_PIN),
                          eh_gpio_value(ESP_HOSTED_DATA_READY_PIN));
                    waiting_for_handshake = RT_TRUE;
                }
                if (have_item)
                {
                    rt_mq_urgent(&g_eh.ctrl_queue, &item, sizeof(item));
                }
                break;
            }
            if (waiting_for_handshake)
            {
                LOG_D("handshake asserted: HS=%d DR=%d",
                      eh_gpio_value(ESP_HOSTED_HANDSHAKE_PIN),
                      eh_gpio_value(ESP_HOSTED_DATA_READY_PIN));
                waiting_for_handshake = RT_FALSE;
            }

            item_pointer = have_item ? &item : RT_NULL;
            g_eh.dummy_needed = RT_FALSE;
            eh_build_frame(item_pointer);
            rt_memset(g_eh.rx_frame, 0, sizeof(g_eh.rx_frame));
            if (eh_spi_transfer_frame() != EH_SPI_FRAME_SIZE)
            {
                LOG_E("SPI transfer failed");
            }
            else
            {
                transfer_ok = RT_TRUE;
                stuck_rx = eh_rx_frame_is_filled(0) ||
                           eh_rx_frame_is_filled(UINT32_MAX);
                if (stuck_rx)
                {
                    if (g_eh.empty_rx_log_count < EH_INVALID_RX_LOG_LIMIT)
                    {
                        LOG_W("SPI RX stuck at 0x%02x; check %s D1/MISO wiring and coprocessor power",
                              g_eh.rx_words[0] ? 0xff : 0x00, EH_SPI_BUS_NAME);
                        g_eh.empty_rx_log_count++;
                    }
                    malformed_rx = RT_TRUE;
                    rx_processed = RT_FALSE;
                }
                else
                {
                    /* SPI is full duplex. A valid ESP frame may arrive while
                     * the host is transmitting, so always process RX. */
                    rx_processed = eh_process_rx_frame(&malformed_rx);
                }
                if (have_item || rx_processed)
                {
                    g_eh.dummy_needed = RT_TRUE;
                }
            }
            if (transfer_ok && transport_config)
            {
                uint32_t version = g_eh.coprocessor_version;

                g_eh.config_pending = RT_FALSE;
                g_eh.ready = RT_TRUE;
                LOG_I("transport ready: chip=0x%02x firmware=%u.%u.%u",
                      g_eh.coprocessor_id,
                      (unsigned int)((version >> 16) & 0xff),
                      (unsigned int)((version >> 8) & 0xff),
                      (unsigned int)(version & 0xff));
            }
            if (have_item)
            {
                rt_free(item.data);
            }
            if (malformed_rx)
            {
                /* A disconnected or unsynchronised slave can leave DR high.
                 * Do not spin continuously on the same invalid bus state. */
                rt_thread_mdelay(EH_EMPTY_RX_BACKOFF_MS);
                break;
            }
            if (ESP_HOSTED_DATA_READY_PIN < 0)
            {
                break;
            }
        }
    }
}

static rt_err_t eh_wait_ready(void)
{
    rt_tick_t start = rt_tick_get();
    rt_tick_t timeout = rt_tick_from_millisecond(ESP_HOSTED_RPC_TIMEOUT_MS);

    while (!g_eh.ready)
    {
        if ((rt_tick_get() - start) >= timeout)
        {
            LOG_E("transport handshake timeout: HS=%d DR=%d",
                  eh_gpio_value(ESP_HOSTED_HANDSHAKE_PIN),
                  eh_gpio_value(ESP_HOSTED_DATA_READY_PIN));
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(10);
    }
    return RT_EOK;
}

static int eh_get_response_status(const uint8_t *data, size_t length,
                                  uint32_t field_number, int *status)
{
    struct eh_pb_field field;
    int result;

    /* In proto3, a successful int32 response status is zero and is normally
     * omitted from the encoded submessage. */
    *status = 0;
    result = eh_pb_find(data, length, field_number, &field);
    if (result == 0)
    {
        return 0;
    }
    if (result < 0 || field.wire_type != 0)
    {
        return -1;
    }
    *status = eh_pb_field_int32(&field);
    return 0;
}

static int eh_parse_status(uint16_t response_id, const uint8_t *data,
                           size_t length, void *argument)
{
    (void)response_id;
    return eh_get_response_status(data, length, 1, argument);
}

static rt_err_t eh_rpc_execute(uint16_t request_id, const uint8_t *request,
                               size_t request_length, int timeout_ms,
                               eh_rpc_parser_t parser, void *argument)
{
    struct eh_pb_writer writer;
    uint16_t expected_response_id = request_id + 0x100;
    uint32_t uid;
    rt_err_t result;
    int parse_result = 0;

    result = eh_wait_ready();
    if (result != RT_EOK)
    {
        return result;
    }
    rt_mutex_take(&g_eh.rpc_mutex, RT_WAITING_FOREVER);
    while (rt_sem_trytake(&g_eh.rpc_done) == RT_EOK)
    {
    }

    uid = ++g_eh.rpc_uid;
    eh_pb_writer_init(&writer, g_eh.rpc_tx, sizeof(g_eh.rpc_tx));
    eh_pb_put_varint(&writer, 1, EH_RPC_TYPE_REQUEST);
    eh_pb_put_varint(&writer, 2, request_id);
    eh_pb_put_varint(&writer, 3, uid);
    eh_pb_put_bytes(&writer, request_id, request, request_length);
    if (writer.error || writer.length > sizeof(g_eh.rpc_tlv) - 12)
    {
        result = -RT_EINVAL;
        goto exit;
    }

    g_eh.rpc_tlv[0] = 0x01;
    eh_put_le16(g_eh.rpc_tlv + 1, 6);
    rt_memcpy(g_eh.rpc_tlv + 3, "RPCRsp", 6);
    g_eh.rpc_tlv[9] = 0x02;
    eh_put_le16(g_eh.rpc_tlv + 10, writer.length);
    rt_memcpy(g_eh.rpc_tlv + 12, g_eh.rpc_tx, writer.length);
    g_eh.response_length = 0;
    g_eh.response_id = 0;
    g_eh.pending_uid = uid;
    result = eh_queue_frame(EH_IF_SERIAL, 0, g_eh.rpc_tlv,
                            writer.length + 12, RT_TRUE);
    if (result != RT_EOK)
    {
        LOG_E("cannot queue RPC %u: %d (control queue %u/%u)", request_id,
              result, (unsigned int)g_eh.ctrl_queue.entry,
              (unsigned int)g_eh.ctrl_queue.max_msgs);
        g_eh.pending_uid = 0;
        goto exit;
    }
    result = rt_sem_take(&g_eh.rpc_done, rt_tick_from_millisecond(timeout_ms));
    g_eh.pending_uid = 0;
    if (result != RT_EOK)
    {
        LOG_W("RPC %u timed out", request_id);
        result = -RT_ETIMEOUT;
        goto exit;
    }
    if (g_eh.response_id != expected_response_id)
    {
        LOG_W("RPC %u received unexpected response %u (expected %u)",
              request_id, g_eh.response_id, expected_response_id);
        result = -RT_ERROR;
        goto exit;
    }
    if (parser)
    {
        parse_result = parser(g_eh.response_id, g_eh.response,
                              g_eh.response_length, argument);
        if (parse_result)
        {
            LOG_W("RPC %u response %u is malformed", request_id,
                  g_eh.response_id);
            result = -RT_ERROR;
        }
    }

exit:
    rt_mutex_release(&g_eh.rpc_mutex);
    return result;
}

static rt_err_t eh_rpc_status(uint16_t request_id, const uint8_t *request,
                              size_t request_length, int timeout_ms)
{
    int status = -1;
    rt_err_t result = eh_rpc_execute(request_id, request, request_length,
                                     timeout_ms, eh_parse_status, &status);

    if (result == RT_EOK && status != 0)
    {
        LOG_W("RPC %u failed: 0x%x", request_id, status);
        result = -RT_ERROR;
    }
    return result;
}

static rt_err_t eh_rpc_empty(uint16_t request_id)
{
    return eh_rpc_status(request_id, RT_NULL, 0, ESP_HOSTED_RPC_TIMEOUT_MS);
}

static rt_err_t eh_wifi_init_locked(void)
{
    uint8_t config[256];
    uint8_t request[280];
    struct eh_pb_writer config_writer;
    struct eh_pb_writer request_writer;
    rt_err_t result;
    int static_rx_buffers = 10;
    int dynamic_rx_buffers = 32;
    int dynamic_tx_buffers = 32;
    int rx_ba_window = 6;

    if (g_eh.wifi_initialized)
    {
        return RT_EOK;
    }

    switch (g_eh.coprocessor_id)
    {
    case 0x02: /* ESP32-S2 */
        static_rx_buffers = 8;
        dynamic_rx_buffers = 24;
        dynamic_tx_buffers = 24;
        rx_ba_window = 16;
        break;
    case 0x05: /* ESP32-C3 */
        rx_ba_window = 16;
        break;
    case 0x0c: /* ESP32-C2 */
        static_rx_buffers = 8;
        dynamic_rx_buffers = 16;
        dynamic_tx_buffers = 16;
        rx_ba_window = 8;
        break;
    case 0x17: /* ESP32-C5 */
        dynamic_rx_buffers = 64;
        rx_ba_window = 16;
        break;
    default:
        break;
    }

    eh_pb_writer_init(&config_writer, config, sizeof(config));
    eh_pb_put_varint(&config_writer, 1, static_rx_buffers);
    eh_pb_put_varint(&config_writer, 2, dynamic_rx_buffers);
    eh_pb_put_varint(&config_writer, 3, 1);   /* dynamic TX buffers */
    eh_pb_put_varint(&config_writer, 5, dynamic_tx_buffers);
    eh_pb_put_varint(&config_writer, 8, 1);   /* AMPDU RX */
    eh_pb_put_varint(&config_writer, 9, 1);   /* AMPDU TX */
    eh_pb_put_varint(&config_writer, 11, 1);  /* NVS */
    eh_pb_put_varint(&config_writer, 13, rx_ba_window);
    eh_pb_put_varint(&config_writer, 15, 752);
    eh_pb_put_varint(&config_writer, 16, 32);
    eh_pb_put_varint(&config_writer, 18, 1);  /* disconnected power management */
    eh_pb_put_varint(&config_writer, 19, 7);  /* ESP-NOW encrypted peers */
    eh_pb_put_varint(&config_writer, 20, EH_WIFI_INIT_MAGIC);
    eh_pb_put_varint(&config_writer, 21, 1);  /* dynamic management RX buffers */
    eh_pb_put_varint(&config_writer, 22, 5);
    eh_pb_put_varint(&config_writer, 23, 1);
    if (config_writer.error)
    {
        return -RT_ERROR;
    }

    eh_pb_writer_init(&request_writer, request, sizeof(request));
    eh_pb_put_bytes(&request_writer, 1, config, config_writer.length);
    result = eh_rpc_status(EH_REQ_WIFI_INIT, request, request_writer.length,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
    if (result == RT_EOK)
    {
        g_eh.wifi_initialized = RT_TRUE;
    }
    return result;
}

static rt_err_t eh_set_mode_locked(uint8_t mode)
{
    uint8_t request[16];
    struct eh_pb_writer writer;
    rt_err_t result;

    if (mode == g_eh.wifi_mode)
    {
        return RT_EOK;
    }
    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_varint(&writer, 1, mode);
    result = eh_rpc_status(EH_REQ_SET_MODE, request, writer.length,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
    if (result == RT_EOK)
    {
        g_eh.wifi_mode = mode;
    }
    return result;
}

static rt_err_t eh_start_locked(void)
{
    rt_err_t result;

    if (g_eh.wifi_started)
    {
        return RT_EOK;
    }
    result = eh_rpc_empty(EH_REQ_WIFI_START);
    if (result == RT_EOK)
    {
        g_eh.wifi_started = RT_TRUE;
    }
    return result;
}

static rt_err_t eh_wlan_init(struct rt_wlan_device *wlan)
{
    rt_err_t result;

    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    rt_mutex_release(&g_eh.state_mutex);
    if (result == RT_EOK)
    {
        eh_report_event(wlan, RT_WLAN_DEV_EVT_INIT_DONE, RT_NULL, 0);
    }
    return result;
}

static rt_err_t eh_wlan_mode(struct rt_wlan_device *wlan, rt_wlan_mode_t mode)
{
    uint8_t new_mode;
    rt_err_t result;

    if ((wlan == &g_eh.sta && mode != RT_WLAN_STATION && mode != RT_WLAN_NONE) ||
        (wlan == &g_eh.ap && mode != RT_WLAN_AP && mode != RT_WLAN_NONE))
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    if (result == RT_EOK)
    {
        uint8_t interface_mode = wlan == &g_eh.sta ? EH_WIFI_MODE_STA : EH_WIFI_MODE_AP;

        new_mode = mode == RT_WLAN_NONE ? (g_eh.wifi_mode & ~interface_mode)
                                        : (g_eh.wifi_mode | interface_mode);
        result = eh_set_mode_locked(new_mode);
    }
    rt_mutex_release(&g_eh.state_mutex);
    return result;
}

static rt_err_t eh_wlan_join(struct rt_wlan_device *wlan, struct rt_sta_info *info)
{
    uint8_t sta_config[256];
    uint8_t wifi_config[280];
    uint8_t request[320];
    struct eh_pb_writer sta_writer;
    struct eh_pb_writer config_writer;
    struct eh_pb_writer request_writer;
    rt_err_t result;

    (void)wlan;
    eh_pb_writer_init(&sta_writer, sta_config, sizeof(sta_config));
    eh_pb_put_bytes(&sta_writer, 1, info->ssid.val, info->ssid.len);
    eh_pb_put_bytes(&sta_writer, 2, info->key.val, info->key.len);
    if (rt_memcmp(info->bssid, "\0\0\0\0\0\0", 6) != 0)
    {
        eh_pb_put_varint(&sta_writer, 4, 1);
        eh_pb_put_bytes(&sta_writer, 5, info->bssid, 6);
    }
    if (info->channel)
    {
        eh_pb_put_varint(&sta_writer, 6, info->channel);
    }
    eh_pb_writer_init(&config_writer, wifi_config, sizeof(wifi_config));
    eh_pb_put_bytes(&config_writer, 2, sta_config, sta_writer.length);
    eh_pb_writer_init(&request_writer, request, sizeof(request));
    eh_pb_put_varint(&request_writer, 1, EH_WIFI_IF_STA);
    eh_pb_put_bytes(&request_writer, 2, wifi_config, config_writer.length);
    if (sta_writer.error || config_writer.error || request_writer.error)
    {
        return -RT_EINVAL;
    }

    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    if (result == RT_EOK)
    {
        result = eh_set_mode_locked(g_eh.wifi_mode | EH_WIFI_MODE_STA);
    }
    if (result == RT_EOK)
    {
        result = eh_rpc_status(EH_REQ_WIFI_SET_CONFIG, request, request_writer.length,
                               ESP_HOSTED_RPC_TIMEOUT_MS);
    }
    if (result == RT_EOK)
    {
        result = eh_start_locked();
    }
    if (result == RT_EOK)
    {
        g_eh.connecting = RT_TRUE;
        result = eh_rpc_empty(EH_REQ_WIFI_CONNECT);
    }
    if (result != RT_EOK)
    {
        g_eh.connecting = RT_FALSE;
    }
    rt_mutex_release(&g_eh.state_mutex);
    return result;
}

static rt_err_t eh_wlan_softap(struct rt_wlan_device *wlan, struct rt_ap_info *info)
{
    uint8_t ap_config[256];
    uint8_t wifi_config[280];
    uint8_t request[320];
    struct eh_pb_writer ap_writer;
    struct eh_pb_writer config_writer;
    struct eh_pb_writer request_writer;
    uint8_t mac[6];
    rt_err_t result;

    (void)wlan;
    eh_pb_writer_init(&ap_writer, ap_config, sizeof(ap_config));
    eh_pb_put_bytes(&ap_writer, 1, info->ssid.val, info->ssid.len);
    eh_pb_put_bytes(&ap_writer, 2, info->key.val, info->key.len);
    eh_pb_put_varint(&ap_writer, 3, info->ssid.len);
    eh_pb_put_varint(&ap_writer, 4, info->channel ? info->channel : 1);
    eh_pb_put_int32(&ap_writer, 5, eh_authmode_from_security(info->security));
    eh_pb_put_varint(&ap_writer, 6, info->hidden ? 1 : 0);
    eh_pb_put_varint(&ap_writer, 7, 4);
    eh_pb_put_varint(&ap_writer, 8, 100);
    eh_pb_writer_init(&config_writer, wifi_config, sizeof(wifi_config));
    eh_pb_put_bytes(&config_writer, 1, ap_config, ap_writer.length);
    eh_pb_writer_init(&request_writer, request, sizeof(request));
    eh_pb_put_varint(&request_writer, 1, EH_WIFI_IF_AP);
    eh_pb_put_bytes(&request_writer, 2, wifi_config, config_writer.length);
    if (ap_writer.error || config_writer.error || request_writer.error)
    {
        return -RT_EINVAL;
    }

    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    if (result == RT_EOK)
    {
        result = eh_set_mode_locked(g_eh.wifi_mode | EH_WIFI_MODE_AP);
    }
    if (result == RT_EOK)
    {
        result = eh_rpc_status(EH_REQ_WIFI_SET_CONFIG, request, request_writer.length,
                               ESP_HOSTED_RPC_TIMEOUT_MS);
    }
    if (result == RT_EOK)
    {
        result = eh_start_locked();
    }
    rt_mutex_release(&g_eh.state_mutex);
    if (result == RT_EOK)
    {
        rt_err_t mac_result = eh_wlan_get_mac(&g_eh.ap, mac);

        if (mac_result == RT_EOK)
        {
            eh_report_event(&g_eh.ap, RT_WLAN_DEV_EVT_AP_START, mac, sizeof(mac));
        }
        else
        {
            LOG_W("cannot read SoftAP MAC address: %d", mac_result);
            eh_report_event(&g_eh.ap, RT_WLAN_DEV_EVT_AP_START, RT_NULL, 0);
        }
    }
    return result;
}

static rt_err_t eh_wlan_disconnect(struct rt_wlan_device *wlan)
{
    (void)wlan;
    g_eh.connecting = RT_FALSE;
    return eh_rpc_empty(EH_REQ_WIFI_DISCONNECT);
}

static rt_err_t eh_wlan_ap_stop(struct rt_wlan_device *wlan)
{
    rt_err_t result;

    (void)wlan;
    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_set_mode_locked(g_eh.wifi_mode & ~EH_WIFI_MODE_AP);
    rt_mutex_release(&g_eh.state_mutex);
    if (result == RT_EOK)
    {
        eh_report_event(&g_eh.ap, RT_WLAN_DEV_EVT_AP_STOP, RT_NULL, 0);
    }
    return result;
}

static int eh_parse_sta_aid(uint16_t response_id, const uint8_t *data,
                            size_t length, void *argument)
{
    struct eh_pb_field field;
    int status;

    (void)response_id;
    if (eh_get_response_status(data, length, 1, &status) || status ||
        eh_pb_find(data, length, 2, &field) <= 0 || field.wire_type != 0 ||
        field.value == 0 || field.value > UINT16_MAX)
    {
        return -1;
    }
    *(uint16_t *)argument = (uint16_t)field.value;
    return 0;
}

static rt_err_t eh_wlan_ap_deauth(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    uint8_t request[32];
    struct eh_pb_writer writer;
    uint16_t aid = 0;
    rt_err_t result;

    if (wlan != &g_eh.ap || !mac)
    {
        return -RT_EINVAL;
    }

    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_bytes(&writer, 1, mac, 6);
    if (writer.error)
    {
        return -RT_EINVAL;
    }
    result = eh_rpc_execute(EH_REQ_WIFI_AP_GET_STA_AID, request, writer.length,
                            ESP_HOSTED_RPC_TIMEOUT_MS, eh_parse_sta_aid, &aid);
    if (result != RT_EOK)
    {
        return result;
    }

    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_varint(&writer, 1, aid);
    if (writer.error)
    {
        return -RT_EINVAL;
    }
    return eh_rpc_status(EH_REQ_WIFI_DEAUTH_STA, request, writer.length,
                         ESP_HOSTED_RPC_TIMEOUT_MS);
}

static int eh_parse_scan_number(uint16_t response_id, const uint8_t *data,
                                size_t length, void *argument)
{
    struct eh_pb_field field;
    int status;
    int result;

    (void)response_id;
    if (eh_get_response_status(data, length, 1, &status) || status)
    {
        return -1;
    }
    *(int *)argument = 0;
    result = eh_pb_find(data, length, 2, &field);
    if (result == 0)
    {
        return 0;
    }
    if (result < 0 || field.wire_type != 0)
    {
        return -1;
    }
    *(int *)argument = field.value;
    return 0;
}

static int eh_parse_ap_record(const uint8_t *data, size_t length)
{
    struct eh_pb_reader reader;
    struct eh_pb_field field;
    struct rt_wlan_info info;
    int result;

    INVALID_INFO(&info);
    info.datarate = 0;
    info.hidden = 0;
    /* WIFI_AUTH_OPEN is zero, so proto3 omits authmode for open APs. */
    info.security = SECURITY_OPEN;
    eh_pb_reader_init(&reader, data, length);
    while ((result = eh_pb_next(&reader, &field)) > 0)
    {
        if (field.number == 1 && field.wire_type == 2)
        {
            if (field.length != sizeof(info.bssid))
            {
                return -1;
            }
            rt_memcpy(info.bssid, field.data, sizeof(info.bssid));
        }
        else if (field.number == 2 && field.wire_type == 2)
        {
            size_t ssid_length = field.length;

            /* ESP-Hosted's RPC_COPY_STR includes the terminating NUL in the
             * protobuf bytes field. RT-Thread stores the character count, so
             * keeping that NUL in ssid.len prevents cache lookup during join. */
            if (ssid_length && field.data[ssid_length - 1] == '\0')
            {
                ssid_length--;
            }
            info.ssid.len = ssid_length < RT_WLAN_SSID_MAX_LENGTH ?
                            ssid_length : RT_WLAN_SSID_MAX_LENGTH;
            rt_memcpy(info.ssid.val, field.data, info.ssid.len);
            info.ssid.val[info.ssid.len] = '\0';
        }
        else if (field.number == 3 && field.wire_type == 0)
        {
            info.channel = field.value;
            info.band = info.channel <= 14 ? RT_802_11_BAND_2_4GHZ : RT_802_11_BAND_5GHZ;
        }
        else if (field.number == 5 && field.wire_type == 0)
        {
            info.rssi = eh_pb_field_int32(&field);
        }
        else if (field.number == 6 && field.wire_type == 0)
        {
            info.security = eh_security_from_esp(eh_pb_field_int32(&field));
        }
    }
    if (result >= 0 && info.ssid.len)
    {
        eh_report_event(&g_eh.sta, RT_WLAN_DEV_EVT_SCAN_REPORT, &info, sizeof(info));
    }
    return result < 0 ? -1 : 0;
}

static int eh_parse_scan_records(uint16_t response_id, const uint8_t *data,
                                 size_t length, void *argument)
{
    struct eh_pb_reader reader;
    struct eh_pb_field field;
    int status;
    int result;

    (void)response_id;
    (void)argument;
    if (eh_get_response_status(data, length, 1, &status) || status)
    {
        return -1;
    }
    eh_pb_reader_init(&reader, data, length);
    while ((result = eh_pb_next(&reader, &field)) > 0)
    {
        if (field.number == 3 && field.wire_type == 2)
        {
            if (eh_parse_ap_record(field.data, field.length))
            {
                return -1;
            }
        }
    }
    return result < 0 ? -1 : 0;
}

static rt_err_t eh_wlan_scan(struct rt_wlan_device *wlan, struct rt_scan_info *scan_info)
{
    uint8_t request[16];
    struct eh_pb_writer writer;
    int count = 0;
    rt_err_t result;

    (void)wlan;
    (void)scan_info;
    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    if (result == RT_EOK)
    {
        result = eh_set_mode_locked(g_eh.wifi_mode | EH_WIFI_MODE_STA);
    }
    if (result == RT_EOK)
    {
        result = eh_start_locked();
    }
    rt_mutex_release(&g_eh.state_mutex);
    if (result != RT_EOK)
    {
        return result;
    }

    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_varint(&writer, 2, 1); /* blocking scan */
    result = eh_rpc_status(EH_REQ_SCAN_START, request, writer.length, 30000);
    if (result == RT_EOK)
    {
        result = eh_rpc_execute(EH_REQ_SCAN_GET_NUM, RT_NULL, 0,
                                ESP_HOSTED_RPC_TIMEOUT_MS,
                                eh_parse_scan_number, &count);
    }
    if (count > ESP_HOSTED_MAX_SCAN_RESULTS)
    {
        count = ESP_HOSTED_MAX_SCAN_RESULTS;
    }
    if (result == RT_EOK && count > 0)
    {
        eh_pb_writer_init(&writer, request, sizeof(request));
        eh_pb_put_varint(&writer, 1, count);
        result = eh_rpc_execute(EH_REQ_SCAN_GET_RECORDS, request, writer.length,
                                ESP_HOSTED_RPC_TIMEOUT_MS,
                                eh_parse_scan_records, RT_NULL);
    }
    eh_report_event(&g_eh.sta, RT_WLAN_DEV_EVT_SCAN_DONE, RT_NULL, 0);
    return result;
}

static rt_err_t eh_wlan_scan_stop(struct rt_wlan_device *wlan)
{
    (void)wlan;
    return eh_rpc_empty(EH_REQ_SCAN_STOP);
}

static int eh_parse_rssi(uint16_t response_id, const uint8_t *data,
                         size_t length, void *argument)
{
    struct eh_pb_field field;
    int status;
    int result;

    (void)response_id;
    if (eh_get_response_status(data, length, 1, &status) || status)
    {
        return -1;
    }
    *(int *)argument = 0;
    result = eh_pb_find(data, length, 2, &field);
    if (result == 0)
    {
        return 0;
    }
    if (result < 0 || field.wire_type != 0)
    {
        return -1;
    }
    *(int *)argument = eh_pb_field_int32(&field);
    return 0;
}

static int eh_wlan_get_rssi(struct rt_wlan_device *wlan)
{
    int rssi = -127;

    (void)wlan;
    eh_rpc_execute(EH_REQ_GET_RSSI, RT_NULL, 0, ESP_HOSTED_RPC_TIMEOUT_MS,
                   eh_parse_rssi, &rssi);
    return rssi;
}

static rt_err_t eh_wlan_set_powersave(struct rt_wlan_device *wlan, int level)
{
    uint8_t request[16];
    struct eh_pb_writer writer;

    (void)wlan;
    if (level < 0 || level > 2)
    {
        return -RT_EINVAL;
    }
    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_varint(&writer, 1, level);
    return eh_rpc_status(EH_REQ_SET_PS, request, writer.length,
                         ESP_HOSTED_RPC_TIMEOUT_MS);
}

static int eh_wlan_get_powersave(struct rt_wlan_device *wlan)
{
    int level = -1;

    (void)wlan;
    eh_rpc_execute(EH_REQ_GET_PS, RT_NULL, 0, ESP_HOSTED_RPC_TIMEOUT_MS,
                   eh_parse_rssi, &level);
    return level;
}

static rt_err_t eh_wlan_set_channel(struct rt_wlan_device *wlan, int channel)
{
    uint8_t request[16];
    struct eh_pb_writer writer;

    (void)wlan;
    if (channel < 1 || channel > 196)
    {
        return -RT_EINVAL;
    }
    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_varint(&writer, 1, channel);
    return eh_rpc_status(EH_REQ_SET_CHANNEL, request, writer.length,
                         ESP_HOSTED_RPC_TIMEOUT_MS);
}

static int eh_parse_channel(uint16_t response_id, const uint8_t *data,
                            size_t length, void *argument)
{
    return eh_parse_rssi(response_id, data, length, argument);
}

static int eh_wlan_get_channel(struct rt_wlan_device *wlan)
{
    int channel = -1;

    (void)wlan;
    eh_rpc_execute(EH_REQ_GET_CHANNEL, RT_NULL, 0, ESP_HOSTED_RPC_TIMEOUT_MS,
                   eh_parse_channel, &channel);
    return channel;
}

static int eh_parse_mac(uint16_t response_id, const uint8_t *data,
                        size_t length, void *argument)
{
    struct eh_pb_field field;
    int status;

    (void)response_id;
    if (eh_get_response_status(data, length, 2, &status) || status ||
        eh_pb_find(data, length, 1, &field) <= 0 ||
        field.wire_type != 2 || field.length != 6)
    {
        return -1;
    }
    rt_memcpy(argument, field.data, 6);
    return 0;
}

static rt_err_t eh_wlan_get_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    uint8_t request[16];
    struct eh_pb_writer writer;

    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_varint(&writer, 1, wlan == &g_eh.sta ? EH_WIFI_IF_STA : EH_WIFI_IF_AP);
    return eh_rpc_execute(EH_REQ_GET_MAC, request, writer.length,
                          ESP_HOSTED_RPC_TIMEOUT_MS, eh_parse_mac, mac);
}

static rt_err_t eh_wlan_set_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    uint8_t request[32];
    struct eh_pb_writer writer;

    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_bytes(&writer, 1, mac, 6);
    eh_pb_put_varint(&writer, 2, wlan == &g_eh.sta ? EH_WIFI_IF_STA : EH_WIFI_IF_AP);
    return eh_rpc_status(EH_REQ_SET_MAC, request, writer.length,
                         ESP_HOSTED_RPC_TIMEOUT_MS);
}

static int eh_wlan_send(struct rt_wlan_device *wlan, void *buffer, int length)
{
    rt_err_t result;

    if (!g_eh.ready || g_eh.tx_throttled || length <= 0 || length > EH_MAX_PAYLOAD)
    {
        return -RT_EBUSY;
    }
    result = eh_queue_frame(wlan == &g_eh.sta ? EH_IF_STA : EH_IF_AP,
                            0, buffer, length, RT_FALSE);
    /* rt_wlan_dev_ops::wlan_send uses rt_err_t semantics. Returning the frame
     * length makes lwIP treat an accepted packet as a link-layer failure. */
    return result;
}

static rt_err_t eh_wlan_set_country(struct rt_wlan_device *wlan,
                                    rt_country_code_t country)
{
    uint8_t request[16];
    struct eh_pb_writer writer;
    const char *country_code;

    (void)wlan;
    country_code = eh_country_code_from_rt(country);
    if (!country_code)
    {
        return -RT_EINVAL;
    }

    eh_pb_writer_init(&writer, request, sizeof(request));
    eh_pb_put_bytes(&writer, 1, country_code, 2);
    eh_pb_put_varint(&writer, 2, 0); /* Keep the configured country fixed. */
    if (writer.error)
    {
        return -RT_EINVAL;
    }
    return eh_rpc_status(EH_REQ_SET_COUNTRY_CODE, request, writer.length,
                         ESP_HOSTED_RPC_TIMEOUT_MS);
}

static int eh_parse_country_code(uint16_t response_id, const uint8_t *data,
                                 size_t length, void *argument)
{
    struct eh_pb_field field;
    int status;

    (void)response_id;
    if (eh_get_response_status(data, length, 1, &status) || status ||
        eh_pb_find(data, length, 2, &field) <= 0 || field.wire_type != 2 ||
        field.length < 2)
    {
        return -1;
    }
    *(rt_country_code_t *)argument = eh_country_code_to_rt(field.data, field.length);
    return *(rt_country_code_t *)argument == RT_COUNTRY_UNKNOWN ? -1 : 0;
}

static rt_country_code_t eh_get_country(struct rt_wlan_device *wlan)
{
    rt_country_code_t country = RT_COUNTRY_UNKNOWN;

    (void)wlan;
    if (eh_rpc_execute(EH_REQ_GET_COUNTRY_CODE, RT_NULL, 0,
                       ESP_HOSTED_RPC_TIMEOUT_MS, eh_parse_country_code,
                       &country) != RT_EOK)
    {
        return RT_COUNTRY_UNKNOWN;
    }
    return country;
}

static const struct rt_wlan_dev_ops g_eh_wlan_ops = {
    .wlan_init = eh_wlan_init,
    .wlan_mode = eh_wlan_mode,
    .wlan_scan = eh_wlan_scan,
    .wlan_join = eh_wlan_join,
    .wlan_softap = eh_wlan_softap,
    .wlan_disconnect = eh_wlan_disconnect,
    .wlan_ap_stop = eh_wlan_ap_stop,
    .wlan_ap_deauth = eh_wlan_ap_deauth,
    .wlan_scan_stop = eh_wlan_scan_stop,
    .wlan_get_rssi = eh_wlan_get_rssi,
    .wlan_set_powersave = eh_wlan_set_powersave,
    .wlan_get_powersave = eh_wlan_get_powersave,
    .wlan_cfg_promisc = RT_NULL,
    .wlan_cfg_filter = RT_NULL,
    .wlan_cfg_mgnt_filter = RT_NULL,
    .wlan_set_channel = eh_wlan_set_channel,
    .wlan_get_channel = eh_wlan_get_channel,
    .wlan_set_country = eh_wlan_set_country,
    .wlan_get_country = eh_get_country,
    .wlan_set_mac = eh_wlan_set_mac,
    .wlan_get_mac = eh_wlan_get_mac,
    .wlan_recv = RT_NULL,
    .wlan_send = eh_wlan_send,
    .wlan_send_raw_frame = RT_NULL,
};

rt_bool_t rt_esp_hosted_is_ready(void)
{
    return g_eh.ready;
}

static void eh_reset_coprocessor(void)
{
#ifdef ESP_HOSTED_RESET_ACTIVE_LOW
    int reset_value = GPIO_PV_LOW;
#else
    int reset_value = GPIO_PV_HIGH;
#endif
    int run_value = reset_value == GPIO_PV_LOW ? GPIO_PV_HIGH : GPIO_PV_LOW;

    if (ESP_HOSTED_RESET_PIN < 0)
    {
        return;
    }
    kd_pin_mode(ESP_HOSTED_RESET_PIN, GPIO_DM_OUTPUT);
    kd_pin_write(ESP_HOSTED_RESET_PIN, run_value);
    rt_thread_mdelay(10);
    kd_pin_write(ESP_HOSTED_RESET_PIN, reset_value);
    rt_thread_mdelay(10);
    kd_pin_write(ESP_HOSTED_RESET_PIN, run_value);
}

struct eh_pin_assignment
{
    const char *name;
    int pin;
};

static rt_err_t eh_validate_pin_config(void)
{
    const struct eh_pin_assignment pins[] = {
        { "clock", ESP_HOSTED_SPI_CLK_PIN },
        { "D0", ESP_HOSTED_SPI_D0_PIN },
        { "D1", ESP_HOSTED_SPI_D1_PIN },
        { "chip-select", ESP_HOSTED_SPI_CS_PIN },
        { "handshake", ESP_HOSTED_HANDSHAKE_PIN },
        { "data-ready", ESP_HOSTED_DATA_READY_PIN },
        { "reset", ESP_HOSTED_RESET_PIN },
    };
    rt_size_t first;
    rt_size_t second;

    for (first = 0; first < sizeof(pins) / sizeof(pins[0]); first++)
    {
        if (pins[first].pin < 0)
        {
            continue;
        }
        for (second = first + 1; second < sizeof(pins) / sizeof(pins[0]); second++)
        {
            if (pins[first].pin == pins[second].pin)
            {
                LOG_E("GPIO %d assigned to both %s and %s", pins[first].pin,
                      pins[first].name, pins[second].name);
                return -RT_EINVAL;
            }
        }
    }
    return RT_EOK;
}

static rt_err_t eh_configure_spi_pins(void)
{
    int pins[2] = {
        ESP_HOSTED_SPI_D0_PIN,
        ESP_HOSTED_SPI_D1_PIN,
    };
    const fpioa_func_t functions[2] = {
        EH_SPI_D0_FUNC,
        EH_SPI_D1_FUNC,
    };
    int index;

    if (ESP_HOSTED_SPI_CLK_PIN >= 0 &&
        drv_fpioa_set_pin_func(ESP_HOSTED_SPI_CLK_PIN, EH_SPI_CLK_FUNC) != 0)
    {
        LOG_E("GPIO %d cannot provide %s clock", ESP_HOSTED_SPI_CLK_PIN,
              EH_SPI_BUS_NAME);
        return -RT_EINVAL;
    }
    for (index = 0; index < 2; index++)
    {
        if (pins[index] >= 0 &&
            drv_fpioa_set_pin_func(pins[index], functions[index]) != 0)
        {
            LOG_E("GPIO %d cannot provide %s D%d", pins[index],
                  EH_SPI_BUS_NAME, index);
            return -RT_EINVAL;
        }
    }
    if ((pins[0] >= 0 &&
         (drv_fpioa_set_pin_oe(pins[0], 1) != 0 ||
          drv_fpioa_set_pin_ie(pins[0], 0) != 0)) ||
        (pins[1] >= 0 &&
         (drv_fpioa_set_pin_oe(pins[1], 0) != 0 ||
          drv_fpioa_set_pin_ie(pins[1], 1) != 0)))
    {
        LOG_E("cannot configure single-line SPI data direction");
        return -RT_ERROR;
    }

    return RT_EOK;
}

int rt_hw_esp_hosted_init(void)
{
    struct rt_qspi_configuration config;
    rt_err_t result;

    result = eh_validate_pin_config();
    if (result != RT_EOK)
    {
        return result;
    }

    rt_memset(&g_eh, 0, sizeof(g_eh));
    rt_event_init(&g_eh.xfer_event, "eh-xfer", RT_IPC_FLAG_FIFO);
    rt_mutex_init(&g_eh.rpc_mutex, "eh-rpc", RT_IPC_FLAG_PRIO);
    rt_mutex_init(&g_eh.state_mutex, "eh-state", RT_IPC_FLAG_PRIO);
    rt_sem_init(&g_eh.rpc_done, "eh-done", 0, RT_IPC_FLAG_PRIO);
    result = rt_mq_init(&g_eh.ctrl_queue, "eh-ctrl", g_eh.ctrl_pool,
                        sizeof(struct eh_tx_item), sizeof(g_eh.ctrl_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mq_init(&g_eh.data_queue, "eh-data", g_eh.data_pool,
                        sizeof(struct eh_tx_item), sizeof(g_eh.data_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mq_init(&g_eh.wlan_event_queue, "eh-event", g_eh.wlan_event_pool,
                        sizeof(struct eh_wlan_event), sizeof(g_eh.wlan_event_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }

    result = eh_configure_spi_pins();
    if (result != RT_EOK)
    {
        return result;
    }

    if (ESP_HOSTED_SPI_CS_PIN >= 0)
    {
        kd_pin_mode(ESP_HOSTED_SPI_CS_PIN, GPIO_DM_OUTPUT);
        kd_pin_write(ESP_HOSTED_SPI_CS_PIN, GPIO_PV_HIGH);
    }

    result = rt_spi_bus_attach_device(&g_eh.spi.parent, ESP_HOSTED_SPI_DEVICE_NAME,
                                      EH_SPI_BUS_NAME, RT_NULL);
    if (result != RT_EOK)
    {
        LOG_E("cannot attach to %s", EH_SPI_BUS_NAME);
        return result;
    }

    rt_memset(&config, 0, sizeof(config));
    config.parent.mode = RT_SPI_MSB;
    switch (ESP_HOSTED_SPI_MODE)
    {
    case 0: config.parent.mode |= RT_SPI_MODE_0; break;
    case 1: config.parent.mode |= RT_SPI_MODE_1; break;
    case 2: config.parent.mode |= RT_SPI_MODE_2; break;
    default: config.parent.mode |= RT_SPI_MODE_3; break;
    }
    if (ESP_HOSTED_SPI_CS_PIN >= 0)
    {
        config.parent.soft_cs = 0x80 | ESP_HOSTED_SPI_CS_PIN;
    }
    config.parent.data_width = EH_SPI_DATA_WIDTH;
    config.parent.max_hz = ESP_HOSTED_SPI_MAX_HZ;
    config.qspi_dl_width = 1;
    result = rt_qspi_configure(&g_eh.spi, &config);
    if (result != RT_EOK)
    {
        LOG_E("cannot configure SPI: %d", result);
        return result;
    }
    if (ESP_HOSTED_HANDSHAKE_PIN >= 0)
    {
        kd_pin_mode(ESP_HOSTED_HANDSHAKE_PIN,
#ifdef ESP_HOSTED_HANDSHAKE_ACTIVE_LOW
                    GPIO_DM_INPUT_PULLUP
#else
                    GPIO_DM_INPUT_PULLDOWN
#endif
                    );
    }
    if (ESP_HOSTED_DATA_READY_PIN >= 0)
    {
        kd_pin_mode(ESP_HOSTED_DATA_READY_PIN,
#ifdef ESP_HOSTED_DATA_READY_ACTIVE_LOW
                    GPIO_DM_INPUT_PULLUP
#else
                    GPIO_DM_INPUT_PULLDOWN
#endif
                    );
    }
    if (ESP_HOSTED_HANDSHAKE_PIN >= 0)
    {
        result = kd_pin_attach_irq(ESP_HOSTED_HANDSHAKE_PIN,
#ifdef ESP_HOSTED_HANDSHAKE_ACTIVE_LOW
                                   PIN_IRQ_MODE_FALLING
#else
                                   PIN_IRQ_MODE_RISING
#endif
                                   , eh_gpio_irq, &g_eh);
        if (result != RT_EOK)
        {
            LOG_E("cannot attach handshake IRQ: %d", result);
            return result;
        }
        kd_pin_irq_enable(ESP_HOSTED_HANDSHAKE_PIN, RT_TRUE);
    }
    else
    {
        LOG_I("handshake GPIO disabled");
    }
    if (ESP_HOSTED_DATA_READY_PIN >= 0)
    {
        result = kd_pin_attach_irq(ESP_HOSTED_DATA_READY_PIN,
#ifdef ESP_HOSTED_DATA_READY_ACTIVE_LOW
                                   PIN_IRQ_MODE_FALLING
#else
                                   PIN_IRQ_MODE_RISING
#endif
                                   , eh_gpio_irq, &g_eh);
        if (result != RT_EOK)
        {
            LOG_E("cannot attach data-ready IRQ: %d", result);
            return result;
        }
        kd_pin_irq_enable(ESP_HOSTED_DATA_READY_PIN, RT_TRUE);
    }
    else
    {
        LOG_I("data-ready GPIO disabled; polling transport");
    }

    result = rt_wlan_dev_register(&g_eh.ap, RT_WLAN_DEVICE_AP_NAME,
                                  &g_eh_wlan_ops, RT_WLAN_FLAG_AP_ONLY, &g_eh);
    if (result != RT_EOK)
    {
        LOG_E("cannot register AP WLAN device: %d", result);
        return result;
    }
    result = rt_wlan_dev_register(&g_eh.sta, RT_WLAN_DEVICE_STA_NAME,
                                  &g_eh_wlan_ops, RT_WLAN_FLAG_STA_ONLY, &g_eh);
    if (result != RT_EOK)
    {
        LOG_E("cannot register station WLAN device: %d", result);
        return result;
    }

    g_eh.thread = rt_thread_create("esp-hosted", eh_transfer_thread, &g_eh,
                                   ESP_HOSTED_THREAD_STACK_SIZE,
                                   ESP_HOSTED_THREAD_PRIORITY, 20);
    if (!g_eh.thread)
    {
        return -RT_ENOMEM;
    }
    g_eh.event_thread = rt_thread_create("esp-event", eh_wlan_event_thread, &g_eh,
                                         ESP_HOSTED_EVENT_THREAD_STACK_SIZE,
                                         ESP_HOSTED_THREAD_PRIORITY, 20);
    if (!g_eh.event_thread)
    {
        return -RT_ENOMEM;
    }

    /* The transport worker observes the reset-time inactive-to-active
     * handshake transition asynchronously before it clocks SPI. */
    g_eh.boot_sync_pending = ESP_HOSTED_RESET_PIN >= 0 &&
                             ESP_HOSTED_HANDSHAKE_PIN >= 0;
    g_eh.boot_saw_handshake_inactive = RT_FALSE;

    LOG_I("SPI: bus=%s mode=%d freq=%d Hz full-duplex",
          EH_SPI_BUS_NAME, ESP_HOSTED_SPI_MODE, ESP_HOSTED_SPI_MAX_HZ);
    LOG_I("GPIOs: CLK:%d MOSI:%d MISO:%d CS:%d HS:%d DR:%d RESET:%d",
          ESP_HOSTED_SPI_CLK_PIN, ESP_HOSTED_SPI_D0_PIN,
          ESP_HOSTED_SPI_D1_PIN, ESP_HOSTED_SPI_CS_PIN,
          ESP_HOSTED_HANDSHAKE_PIN, ESP_HOSTED_DATA_READY_PIN,
          ESP_HOSTED_RESET_PIN);

    eh_reset_coprocessor();
    if (ESP_HOSTED_RESET_PIN < 0)
    {
        LOG_W("reset GPIO disabled; host and coprocessor must be reset together");
    }
    else if (ESP_HOSTED_HANDSHAKE_PIN < 0)
    {
        LOG_W("handshake GPIO disabled; reset completion cannot be verified");
    }
    LOG_D("GPIO levels: HS:%d DR:%d RESET:%d",
          eh_gpio_value(ESP_HOSTED_HANDSHAKE_PIN),
          eh_gpio_value(ESP_HOSTED_DATA_READY_PIN),
          eh_gpio_value(ESP_HOSTED_RESET_PIN));
    LOG_I("waiting for transport handshake");

    rt_thread_startup(g_eh.thread);
    rt_thread_startup(g_eh.event_thread);
    rt_event_send(&g_eh.xfer_event, EH_EVENT_XFER);
    return RT_EOK;
}
INIT_APP_EXPORT(rt_hw_esp_hosted_init);
