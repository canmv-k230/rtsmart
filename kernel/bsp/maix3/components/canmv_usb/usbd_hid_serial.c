#include <lwp.h>
#include <lwp_user_mm.h>
#include <dfs_posix.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <stdlib.h>

#include "usbd_desc.h"

#if defined(CHERRY_USB_DEVICE_FUNC_HID) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP)

#include <drivers/serial.h>

#include "usbd_desc_hid_common.h"
#include "usbd_hid.h"
#include "usb_osal.h"

#ifdef RT_SERIAL_USING_DMA

struct hid_serial_device {
    struct rt_serial_device serial;
    uint8_t                 busid;
    uint8_t                 in_ep;
    uint8_t                 out_ep;
    struct usbd_interface   intf;
    const char             *dev_name;
    const rt_uint8_t       *tx_buf;
    rt_size_t               tx_remaining;
    rt_size_t               tx_chunk;
    bool                    tx_busy;
    bool                    is_open;
};

#define HID_SERIAL_READ_BUFFER_SIZE CANMV_USB_HID_EP_SIZE

#ifndef CHERRY_USB_DEVICE_FUNC_HID_DEVICE_NAME
#define CHERRY_USB_DEVICE_FUNC_HID_DEVICE_NAME "ttyHS0"
#endif

static struct hid_serial_device g_usbd_hid_serial = {
    .dev_name = CHERRY_USB_DEVICE_FUNC_HID_DEVICE_NAME,
};

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_usbd_hid_serial_rx_buf[USB_ALIGN_UP(HID_SERIAL_READ_BUFFER_SIZE, CONFIG_USB_ALIGN_SIZE)];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_usbd_hid_serial_tx_report[USB_ALIGN_UP(CANMV_USB_HID_EP_SIZE, CONFIG_USB_ALIGN_SIZE)];

static const uint8_t g_usbd_hid_serial_report_desc[CANMV_USB_HID_REPORT_DESC_SIZE] = {
    0x06, 0x00, 0xff,
    0x09, 0x01,
    0xa1, 0x01,
    0x85, CANMV_USB_HID_REPORT_ID_IN,
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xff, 0x00,
    0x75, 0x08,
    0x96, (CANMV_USB_HID_PAYLOAD_SIZE & 0xff), ((CANMV_USB_HID_PAYLOAD_SIZE >> 8) & 0xff),
    0x81, 0x02,
    0x85, CANMV_USB_HID_REPORT_ID_OUT,
    0x09, 0x03,
    0x15, 0x00,
    0x26, 0xff, 0x00,
    0x75, 0x08,
    0x96, (CANMV_USB_HID_PAYLOAD_SIZE & 0xff), ((CANMV_USB_HID_PAYLOAD_SIZE >> 8) & 0xff),
    0x91, 0x02,
    0xc0,
};

#ifdef CHERRY_USB_DEVICE_FUNC_HID_DEBUG

#define HID_SERIAL_DEBUG_READ_LEN     150

struct hid_serial_debug_read_args {
    char dev_path[32];
    int  read_len;
};

static void hid_serial_debug_read_thread(void *argument)
{
    struct hid_serial_debug_read_args *args = (struct hid_serial_debug_read_args *)argument;
    int fd;
    int len = 0;
    char buf[HID_SERIAL_DEBUG_READ_LEN];

    if (!args) {
        return;
    }

    fd = open(args->dev_path, O_RDONLY);
    if (fd < 0) {
        rt_kprintf("%s open %s fail\n", __func__, args->dev_path);
        goto exit_thread;
    }

    while (len < args->read_len) {
        int ret = read(fd, buf + len, args->read_len - len);

        if (ret <= 0) {
            break;
        }

        for (int i = 0; i < ret; i++) {
            rt_kprintf("%c", buf[len + i]);
        }
        rt_kprintf("\n");
        len += ret;
    }

    close(fd);

exit_thread:
    rt_free(args);
}

static void hid_serial_debug_read(const char *dev_path, int read_len)
{
    struct hid_serial_debug_read_args *args;
    usb_osal_thread_t thread;

    if (read_len <= 0 || read_len > HID_SERIAL_DEBUG_READ_LEN) {
        rt_kprintf("read length must be 1-%d\n", HID_SERIAL_DEBUG_READ_LEN);
        return;
    }

    args = rt_malloc(sizeof(*args));
    if (!args) {
        rt_kprintf("no memory for hid debug read\n");
        return;
    }

    rt_memset(args, 0, sizeof(*args));
    rt_strncpy(args->dev_path, dev_path, sizeof(args->dev_path) - 1);
    args->read_len = read_len;

    thread = usb_osal_thread_create("hid_rd", 4096, 15, hid_serial_debug_read_thread, args);
    if (thread == NULL) {
        rt_free(args);
        rt_kprintf("%s fail to create thread\n", __func__);
    }
}

static int hid_serial_debug_write(const char *dev_path, const char *data)
{
    int fd;
    int len;

    fd = open(dev_path, O_WRONLY);
    if (fd < 0) {
        rt_kprintf("open %s fail\n", dev_path);
        return -RT_ERROR;
    }

    len = rt_strlen(data);
    if (write(fd, data, len) != len) {
        rt_kprintf("write %s fail\n", dev_path);
        close(fd);
        return -RT_ERROR;
    }

    close(fd);
    return RT_EOK;
}

int cmd_hid_rd(int argc, char **argv)
{
    char dev_path[32];
    int read_len = HID_SERIAL_DEBUG_READ_LEN;

    rt_snprintf(dev_path, sizeof(dev_path), "/dev/%s", CHERRY_USB_DEVICE_FUNC_HID_DEVICE_NAME);

    if (argc > 3) {
        rt_kprintf("Usage: hid_rd [dev_path] [len]\n");
        rt_kprintf("Example: hid_rd /dev/%s 64\n", CHERRY_USB_DEVICE_FUNC_HID_DEVICE_NAME);
        return 0;
    }

    if (argc >= 2) {
        rt_strncpy(dev_path, argv[1], sizeof(dev_path) - 1);
    }

    if (argc == 3) {
        read_len = atoi(argv[2]);
    }

    hid_serial_debug_read(dev_path, read_len);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_hid_rd, hid_rd, read data from hid serial device);

int cmd_hid_wr(int argc, char **argv)
{
    char dev_path[32];
    const char *data;

    rt_snprintf(dev_path, sizeof(dev_path), "/dev/%s", CHERRY_USB_DEVICE_FUNC_HID_DEVICE_NAME);

    if (argc == 2) {
        data = argv[1];
    } else if (argc == 3) {
        rt_strncpy(dev_path, argv[1], sizeof(dev_path) - 1);
        data = argv[2];
    } else {
        rt_kprintf("Usage: hid_wr [dev_path] data\n");
        rt_kprintf("Example: hid_wr /dev/%s hello\n", CHERRY_USB_DEVICE_FUNC_HID_DEVICE_NAME);
        return 0;
    }

    hid_serial_debug_write(dev_path, data);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_hid_wr, hid_wr, write data to hid serial device);

#endif /* CHERRY_USB_DEVICE_FUNC_HID_DEBUG */

static void hid_serial_start_read(struct hid_serial_device *hid)
{
    if (!hid) {
        return;
    }

    usbd_ep_start_read(hid->busid, hid->out_ep, g_usbd_hid_serial_rx_buf, HID_SERIAL_READ_BUFFER_SIZE);
}

static void hid_serial_complete_tx(struct hid_serial_device *hid)
{
    hid->tx_busy = false;
    hid->tx_buf = RT_NULL;
    hid->tx_remaining = 0;
    hid->tx_chunk = 0;
    rt_hw_serial_isr(&hid->serial, RT_SERIAL_EVENT_TX_DMADONE);
}

static void hid_serial_start_next_write(struct hid_serial_device *hid)
{
    rt_size_t chunk;

    if (!hid || !hid->tx_busy) {
        return;
    }

    if (hid->tx_remaining == 0) {
        hid_serial_complete_tx(hid);
        return;
    }

    chunk = hid->tx_remaining;
    /* Reserve 1 byte for length in payload */
    if (chunk > (CANMV_USB_HID_PAYLOAD_SIZE - 1)) {
        chunk = CANMV_USB_HID_PAYLOAD_SIZE - 1;
    }

    g_usbd_hid_serial_tx_report[0] = CANMV_USB_HID_REPORT_ID_IN;
    g_usbd_hid_serial_tx_report[1] = (uint8_t)chunk; /* Length byte */
    rt_memset(&g_usbd_hid_serial_tx_report[2], 0, CANMV_USB_HID_PAYLOAD_SIZE - 1); /* Pad rest with 0 */
    rt_memcpy(&g_usbd_hid_serial_tx_report[2], hid->tx_buf, chunk);
    hid->tx_chunk = chunk;

    /* Always send complete Report Size (EP_SIZE) to avoid Windows hidclass short packet drops */
    if (usbd_ep_start_write(hid->busid, hid->in_ep, g_usbd_hid_serial_tx_report, CANMV_USB_HID_EP_SIZE) < 0) {
        hid_serial_complete_tx(hid);
    }
}

static rt_err_t hid_serial_configure(struct rt_serial_device *serial, struct serial_configure *cfg)
{
    if (cfg) {
        rt_memcpy(&serial->config, cfg, sizeof(serial->config));
    }

    return RT_EOK;
}

static rt_err_t hid_serial_control(struct rt_serial_device *serial, int cmd, void *arg)
{
    int ret = RT_EOK;
    struct hid_serial_device *hid;

    hid = (struct hid_serial_device *)serial->parent.user_data;

    switch (cmd) {
    case RT_DEVICE_CTRL_CONFIG: {
        if (arg == (void *)RT_DEVICE_FLAG_DMA_RX) {
            hid->is_open = RT_TRUE;
        }
        break;
    }
    case RT_DEVICE_CTRL_CLOSE: {
        hid->is_open = RT_FALSE;
        hid->tx_busy = false;
        hid->tx_buf = RT_NULL;
        hid->tx_remaining = 0;
        hid->tx_chunk = 0;
        break;
    }
    case RT_DEVICE_CTRL_CLR_INT: {
        break;
    }
    case UART_IOCTL_SET_CONFIG: {
        struct serial_configure config;

        if (!arg) {
            USB_LOG_ERR("arg is NULL");
            return -RT_EINVAL;
        }

        if (lwp_in_user_space(arg)) {
            if (sizeof(config) != lwp_get_from_user(&config, arg, sizeof(config))) {
                USB_LOG_ERR("lwp get error size");
                return -RT_EINVAL;
            }
        } else {
            rt_memcpy(&config, arg, sizeof(config));
        }

        if (config.bufsz == 0) {
            config.bufsz = serial->config.bufsz;
        }

        rt_memcpy(&serial->config, &config, sizeof(serial->config));
        break;
    }
    case UART_IOCTL_GET_CONFIG: {
        if (!arg) {
            USB_LOG_ERR("arg is NULL");
            return -RT_EINVAL;
        }

        if (lwp_in_user_space(arg)) {
            if (sizeof(serial->config) != lwp_put_to_user(arg, &serial->config, sizeof(serial->config))) {
                USB_LOG_ERR("lwp put error size");
                return -RT_EINVAL;
            }
        } else {
            rt_memcpy(arg, &serial->config, sizeof(serial->config));
        }
        break;
    }
    case UART_IOCTL_SEND_BREAK: {
        return RT_EOK;
    }
    case UART_IOCTL_GET_DTR: {
        int dtr = (g_usb_device_connected && hid->is_open) ? 1 : 0;

        if (!arg) {
            USB_LOG_ERR("arg is NULL");
            return -RT_EINVAL;
        }

        if (lwp_in_user_space(arg)) {
            if (sizeof(int) != lwp_put_to_user(arg, &dtr, sizeof(int))) {
                USB_LOG_ERR("lwp put error size\n");
                ret = -RT_EINVAL;
            }
        } else {
            *((int *)arg) = dtr;
        }
        break;
    }
    default:
        USB_LOG_ERR("%s: unsupport cmd %d\n", __func__, cmd);
        ret = RT_EINVAL;
        break;
    }

    return ret;
}

static rt_size_t hid_serial_transmit(struct rt_serial_device *serial, rt_uint8_t *buf, rt_size_t size, int dir)
{
    struct hid_serial_device *hid;

    hid = (struct hid_serial_device *)serial->parent.user_data;

    if (dir != RT_SERIAL_DMA_TX || size == 0) {
        return 0;
    }

    if (hid->tx_busy) {
        return 0;
    }

    hid->tx_busy = true;
    hid->tx_buf = buf;
    hid->tx_remaining = size;
    hid->tx_chunk = 0;
    hid_serial_start_next_write(hid);

    return size;
}

static void usbd_hid_serial_out(uint8_t busid, uint8_t ep, uint32_t nbytes, void *arg)
{
    struct hid_serial_device *hid = (struct hid_serial_device *)arg;
    const uint8_t *payload = g_usbd_hid_serial_rx_buf;
    uint32_t payload_len = 0;

    (void)busid;
    (void)ep;

    /* HID report format for our custom serial: 
     * Byte 0: Report ID (OUT/IN)
     * Byte 1: Valid Length (payload size)
     * Byte 2..N: Data payload
     */
    if (nbytes >= 2 &&
        (g_usbd_hid_serial_rx_buf[0] == CANMV_USB_HID_REPORT_ID_OUT ||
         g_usbd_hid_serial_rx_buf[0] == CANMV_USB_HID_REPORT_ID_IN)) {
        
        payload_len = g_usbd_hid_serial_rx_buf[1]; /* Read custom valid length */
        payload = &g_usbd_hid_serial_rx_buf[2];
        
        /* Bound check the length against packet size */
        if (payload_len > nbytes - 2) {
            payload_len = nbytes - 2;
        }
    }

    if (hid && hid->is_open && payload_len > 0) {
        rt_serial_put_rxfifo(&hid->serial, payload, payload_len);
        rt_hw_serial_isr(&hid->serial, RT_SERIAL_EVENT_RX_DMADONE);
    }

    hid_serial_start_read(hid);
}

static void usbd_hid_serial_in(uint8_t busid, uint8_t ep, uint32_t nbytes, void *arg)
{
    struct hid_serial_device *hid = (struct hid_serial_device *)arg;

    (void)busid;
    (void)ep;
    (void)nbytes;

    if (!hid || !hid->tx_busy) {
        return;
    }

    hid->tx_buf += hid->tx_chunk;
    hid->tx_remaining -= hid->tx_chunk;
    hid->tx_chunk = 0;

    if (hid->tx_remaining == 0) {
        hid_serial_complete_tx(hid);
        return;
    }

    hid_serial_start_next_write(hid);
}

void canmv_usb_device_hid_on_connected(void)
{
    struct hid_serial_device *hid = &g_usbd_hid_serial;

    hid->tx_busy = false;
    hid->tx_buf = RT_NULL;
    hid->tx_remaining = 0;
    hid->tx_chunk = 0;

    if (hid->is_open) {
        rt_hw_serial_isr(&hid->serial, RT_SERIAL_EVENT_TX_DMADONE);
        rt_hw_serial_isr(&hid->serial, RT_SERIAL_EVENT_HOTPLUG);
    }

    hid_serial_start_read(hid);
}

static const struct rt_uart_ops hid_serial_ops = {
    .configure = hid_serial_configure,
    .control = hid_serial_control,
    .dma_transmit = hid_serial_transmit,
};

static rt_err_t usbd_hid_serial_register(struct hid_serial_device *hid)
{
    int ret;
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    config.baud_rate = BAUD_RATE_2000000;
    config.bufsz = HID_SERIAL_READ_BUFFER_SIZE * 4;

    hid->serial.ops = &hid_serial_ops;
    hid->serial.config = config;

    ret = rt_hw_serial_register(&hid->serial, hid->dev_name,
                                RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_DMA_TX,
                                hid);

    return ret;
}

void canmv_usb_device_hid_init(void)
{
    struct hid_serial_device *hid = &g_usbd_hid_serial;
    struct usbd_endpoint hid_out_ep = {
        .ep_addr = CANMV_USB_HID_OUT_EP,
        .ep_cb_ex = usbd_hid_serial_out,
        .ep_arg = hid,
    };
    struct usbd_endpoint hid_in_ep = {
        .ep_addr = CANMV_USB_HID_IN_EP,
        .ep_cb_ex = usbd_hid_serial_in,
        .ep_arg = hid,
    };

    hid->is_open = RT_FALSE;
    hid->tx_busy = false;
    hid->tx_buf = RT_NULL;
    hid->tx_remaining = 0;
    hid->tx_chunk = 0;
    hid->busid = USB_DEVICE_BUS_ID;
    hid->in_ep = CANMV_USB_HID_IN_EP;
    hid->out_ep = CANMV_USB_HID_OUT_EP;

    usbd_add_interface(hid->busid,
                       usbd_hid_init_intf(hid->busid, &hid->intf,
                                          g_usbd_hid_serial_report_desc,
                                          sizeof(g_usbd_hid_serial_report_desc)));
    usbd_add_endpoint(hid->busid, &hid_out_ep);
    usbd_add_endpoint(hid->busid, &hid_in_ep);

    if (usbd_hid_serial_register(hid) != RT_EOK) {
        USB_LOG_ERR("Failed to register usb hid serial device %s\n", hid->dev_name);
    }
}

#else
#error "RT_SERIAL_USING_DMA is not defined!"
#endif

#endif
