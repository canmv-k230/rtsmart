#include <lwp.h>
#include <lwp_user_mm.h>
#include <rtdevice.h>
#include <rtthread.h>

#include "usbd_desc.h"
#include "usbd_cdc.h"

#if defined(CHERRY_USB_DEVICE_FUNC_CDC) || defined (CHERRY_USB_DEVICE_FUNC_CDC_MTP) || defined (CHERRY_USB_DEVICE_FUNC_CDC_ADB) || defined (CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP)

#include "usbd_desc_cdc_common.h"

#define CDC_ACM_MAGIC_REBOOT_BAUDRATE  (300)
#define CDC_ACM_MAGIC_REBOOT_STOPBITS  (2)
#define CDC_ACM_MAGIC_REBOOT_PARITY    (3)
#define CDC_ACM_MAGIC_REBOOT_DATABITS  (5)
#define CDC_ACM_MAGIC_REBOOT_PORT      (0)

#ifdef RT_SERIAL_USING_DMA

#include <dfs_posix.h>
#include <drivers/serial.h>

struct cdc_device {
    struct rt_serial_device serial;
    uint8_t                 busid;
    uint8_t                 port_index;
    uint8_t                 in_ep;
    uint8_t                 out_ep;
    uint8_t                 int_ep;
    struct usbd_interface   intf_ctrl;
    struct usbd_interface   intf_data;
    struct cdc_line_coding  line_coding;
    const char             *dev_name;
    bool                    pending_magic_reset;
    bool                    last_dtr_state;
    bool                    last_rts_state;
    bool                    send_break_flag;
    int                     cdc_dtr;
    bool                    is_open;
};

#define CDC_MAX_MPS          USB_DEVICE_MAX_MPS
#define CDC_READ_BUFFER_SIZE (4096)

static struct cdc_device g_usbd_serial_cdc_acm[CANMV_USB_CDC_ACM_COUNT];
static const char *g_usbd_serial_cdc_acm_name[CANMV_USB_CDC_ACM_COUNT] = {
    "ttyGS0",
#ifdef CONFIG_ENABLE_DUAL_CDC_PORT
    "ttyGS1",
#endif
};
static const uint8_t g_usbd_serial_cdc_acm_in_ep[CANMV_USB_CDC_ACM_COUNT] = {
    CDC_IN_EP,
#ifdef CONFIG_ENABLE_DUAL_CDC_PORT
    CDC_IN_EP2,
#endif
};
static const uint8_t g_usbd_serial_cdc_acm_out_ep[CANMV_USB_CDC_ACM_COUNT] = {
    CDC_OUT_EP,
#ifdef CONFIG_ENABLE_DUAL_CDC_PORT
    CDC_OUT_EP2,
#endif
};
static const uint8_t g_usbd_serial_cdc_acm_int_ep[CANMV_USB_CDC_ACM_COUNT] = {
    CDC_INT_EP,
#ifdef CONFIG_ENABLE_DUAL_CDC_PORT
    CDC_INT_EP2,
#endif
};

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_usbd_serial_cdc_acm_rx_buf[CANMV_USB_CDC_ACM_COUNT][USB_ALIGN_UP(CDC_READ_BUFFER_SIZE, CONFIG_USB_ALIGN_SIZE)];

static uint32_t cdc_stop_bits_from_char_format(uint8_t char_format)
{
    return char_format == 2 ? STOP_BITS_2 : STOP_BITS_1;
}

static uint8_t cdc_char_format_from_stop_bits(uint32_t stop_bits)
{
    return stop_bits == STOP_BITS_2 ? 2 : 0;
}

static uint32_t cdc_parity_from_line_coding(uint8_t parity_type)
{
    switch (parity_type) {
    case 1:
        return PARITY_ODD;
    case 2:
        return PARITY_EVEN;
    default:
        return PARITY_NONE;
    }
}

static uint8_t cdc_line_parity_from_serial(uint32_t parity)
{
    switch (parity) {
    case PARITY_ODD:
        return 1;
    case PARITY_EVEN:
        return 2;
    default:
        return 0;
    }
}

static void cdc_sync_serial_config_from_line_coding(struct cdc_device *cdc, const struct cdc_line_coding *line_coding)
{
    if (!cdc || !line_coding) {
        return;
    }

    cdc->serial.config.baud_rate = line_coding->dwDTERate;
    cdc->serial.config.data_bits = line_coding->bDataBits;
    cdc->serial.config.stop_bits = cdc_stop_bits_from_char_format(line_coding->bCharFormat);
    cdc->serial.config.parity = cdc_parity_from_line_coding(line_coding->bParityType);
}

static void cdc_sync_line_coding_from_serial_config(struct cdc_device *cdc, const struct serial_configure *config)
{
    if (!cdc || !config) {
        return;
    }

    cdc->line_coding.dwDTERate = config->baud_rate;
    cdc->line_coding.bDataBits = config->data_bits;
    cdc->line_coding.bCharFormat = cdc_char_format_from_stop_bits(config->stop_bits);
    cdc->line_coding.bParityType = cdc_line_parity_from_serial(config->parity);
}

static struct cdc_device *cdc_find_by_intf(uint8_t intf_num)
{
    for (size_t i = 0; i < CANMV_USB_CDC_ACM_COUNT; i++) {
        struct cdc_device *cdc = &g_usbd_serial_cdc_acm[i];

        if (cdc->intf_ctrl.intf_num == intf_num || cdc->intf_data.intf_num == intf_num) {
            return cdc;
        }
    }

    return RT_NULL;
}

static void cdc_reset_link_state(struct cdc_device *cdc)
{
    if (!cdc) {
        return;
    }

    cdc->cdc_dtr = 0;
    cdc->last_dtr_state = false;
    cdc->last_rts_state = false;
    cdc->send_break_flag = false;
    cdc->pending_magic_reset = false;
}

static void cdc_complete_tx(struct cdc_device *cdc)
{
    if (!cdc || !cdc->serial.serial_tx) {
        return;
    }

    rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_TX_DMADONE);
}

static void cdc_start_read(struct cdc_device *cdc)
{
    if (!cdc) {
        USB_LOG_ERR("%s %d null cdc\n", __func__, __LINE__);
        return;
    }

    if (!g_usb_device_connected || !usb_device_is_configured(cdc->busid)) {
        return;
    }

    usbd_ep_start_read(cdc->busid, cdc->out_ep, g_usbd_serial_cdc_acm_rx_buf[cdc->port_index], CDC_READ_BUFFER_SIZE);
}

static rt_err_t cdc_configure(struct rt_serial_device *serial, struct serial_configure *cfg)
{
    if (cfg) {
        rt_memcpy(&serial->config, cfg, sizeof(serial->config));
    }

    return RT_EOK;
}

static rt_err_t cdc_control(struct rt_serial_device* serial, int cmd, void* arg)
{
    int                ret = RT_EOK;
    struct cdc_device* cdc;

    cdc = (struct cdc_device *)serial->parent.user_data;

    switch (cmd) {
    case RT_DEVICE_CTRL_CONFIG: {
        if (arg == (void*)RT_DEVICE_FLAG_DMA_RX) {
            cdc->is_open = RT_TRUE;
            cdc->cdc_dtr = 0;
            cdc->last_dtr_state = false;
            cdc->last_rts_state = false;
            cdc->send_break_flag = false;
        }
        break;
    }
    case RT_DEVICE_CTRL_CLOSE: {
        cdc->is_open = RT_FALSE;
        break;
    }
    case RT_DEVICE_CTRL_CLR_INT: {

    } break;
    /* Added*/
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
        cdc_sync_line_coding_from_serial_config(cdc, &serial->config);
    } break;
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
    } break;
    case UART_IOCTL_SEND_BREAK: {
        return RT_EOK;
    } break;
    case UART_IOCTL_GET_DTR: {
        if (!arg) {
            USB_LOG_ERR("arg is NULL");
            return -RT_EINVAL;
        }

        if (lwp_in_user_space(arg)) {
            if (sizeof(int) != lwp_put_to_user(arg, &cdc->cdc_dtr, sizeof(int))) {
                USB_LOG_ERR("lwp put error size\n");
                ret = -RT_EINVAL;
            }
        } else {
            *((int*)arg) = cdc->cdc_dtr;
        }
    } break;
    default:
        USB_LOG_ERR("%s: unsupport cmd %d\n", __func__, cmd);
        ret = RT_EINVAL;
        break;
    }

    return ret;
}

static rt_size_t cdc_transmit(struct rt_serial_device* serial, rt_uint8_t* buf, rt_size_t size, int dir)
{
    struct cdc_device* cdc;

    cdc = (struct cdc_device *)serial->parent.user_data;

    if (dir == RT_SERIAL_DMA_TX) {
        int ret;

        if (!g_usb_device_connected || !usb_device_is_configured(cdc->busid) || !cdc->cdc_dtr) {
            cdc_complete_tx(cdc);
            return size;
        }

        ret = usbd_ep_start_write(cdc->busid, cdc->in_ep, buf, size);
        if (ret < 0) {
            cdc_complete_tx(cdc);
        }
        return size;
    }

    return 0;
}

static void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes, void *arg)
{
    struct cdc_device *cdc = (struct cdc_device *)arg;

    (void)busid;
    (void)ep;

    if (g_usb_device_connected && usb_device_is_configured(busid) && cdc && cdc->is_open) {
        rt_serial_put_rxfifo(&cdc->serial, g_usbd_serial_cdc_acm_rx_buf[cdc->port_index], nbytes);
        rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_RX_DMADONE);
    }
    cdc_start_read(cdc);
}

static void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes, void *arg)
{
    struct cdc_device *cdc = (struct cdc_device *)arg;

    if (!g_usb_device_connected || !usb_device_is_configured(busid)) {
        cdc_complete_tx(cdc);
    } else if ((nbytes % CDC_MAX_MPS) == 0 && nbytes) {
        /* send zlp */
        if (usbd_ep_start_write(busid, ep, NULL, 0) < 0) {
            cdc_complete_tx(cdc);
        }
    } else if (cdc) {
        cdc_complete_tx(cdc);
    }
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    struct cdc_device *cdc;

    (void)busid;

    cdc = cdc_find_by_intf(intf);
    if (!cdc) {
        return;
    }

    cdc->cdc_dtr = (int)dtr;
    rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_HOTPLUG);

    cdc->last_dtr_state = dtr;
}

void canmv_usb_device_cdc_on_connected(void)
{
    for (size_t i = 0; i < CANMV_USB_CDC_ACM_COUNT; i++) {
        struct cdc_device *cdc = &g_usbd_serial_cdc_acm[i];

        cdc_reset_link_state(cdc);

        if (cdc->is_open) {
            cdc_complete_tx(cdc);
            rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_HOTPLUG);
        }

        cdc_start_read(cdc);
    }
}

void canmv_usb_device_cdc_on_disconnected(void)
{
    for (size_t i = 0; i < CANMV_USB_CDC_ACM_COUNT; i++) {
        struct cdc_device *cdc = &g_usbd_serial_cdc_acm[i];

        cdc_reset_link_state(cdc);

        if (cdc->is_open) {
            cdc_complete_tx(cdc);
            rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_DISCONNECT);
        }
    }
}

static const struct rt_uart_ops cdc_ops = { .configure = cdc_configure, .control = cdc_control, .dma_transmit = cdc_transmit };

static rt_err_t usbd_serial_register(struct cdc_device *cdc)
{
    int ret;
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    config.baud_rate = BAUD_RATE_2000000;
    config.bufsz = CDC_READ_BUFFER_SIZE * 4;

    cdc->serial.ops    = &cdc_ops;
    cdc->serial.config = config;
    cdc_sync_line_coding_from_serial_config(cdc, &config);

    ret = rt_hw_serial_register(&cdc->serial, cdc->dev_name, RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_DMA_TX, cdc);

    return ret;
}

void canmv_usb_device_cdc_init(void)
{
    uint8_t busid = USB_DEVICE_BUS_ID;

    for (size_t i = 0; i < CANMV_USB_CDC_ACM_COUNT; i++) {
        struct cdc_device *cdc = &g_usbd_serial_cdc_acm[i];
        struct usbd_endpoint cdc_out_ep = {
            .ep_addr = g_usbd_serial_cdc_acm_out_ep[i],
            .ep_cb_ex = usbd_cdc_acm_bulk_out,
            .ep_arg = cdc,
        };
        struct usbd_endpoint cdc_in_ep = {
            .ep_addr = g_usbd_serial_cdc_acm_in_ep[i],
            .ep_cb_ex = usbd_cdc_acm_bulk_in,
            .ep_arg = cdc,
        };

        cdc->is_open = RT_FALSE;
        cdc->busid = busid;
        cdc->port_index = i;
        cdc->in_ep = g_usbd_serial_cdc_acm_in_ep[i];
        cdc->out_ep = g_usbd_serial_cdc_acm_out_ep[i];
        cdc->int_ep = g_usbd_serial_cdc_acm_int_ep[i];
        cdc->dev_name = g_usbd_serial_cdc_acm_name[i];
        cdc->line_coding.dwDTERate = BAUD_RATE_2000000;
        cdc->line_coding.bCharFormat = 0;
        cdc->line_coding.bParityType = 0;
        cdc->line_coding.bDataBits = 8;
        cdc_reset_link_state(cdc);

        usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &cdc->intf_ctrl));
        usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &cdc->intf_data));
        usbd_add_endpoint(busid, &cdc_out_ep);
        usbd_add_endpoint(busid, &cdc_in_ep);

        if (usbd_serial_register(cdc) != RT_EOK) {
            USB_LOG_ERR("Failed to register usb serial device %s\n", cdc->dev_name);
        }
    }
}

#else
#error "RT_SERIAL_USING_DMA is not defined!"
#endif

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding)
{
    struct cdc_device *cdc;

    (void)busid;

    if (!line_coding) {
        return;
    }

    cdc = cdc_find_by_intf(intf);
    if (!cdc) {
        return;
    }

    cdc->last_dtr_state = false;
    cdc->last_rts_state = false;
    cdc->send_break_flag = false;
    cdc->pending_magic_reset = false;
    usb_memcpy((void *)&cdc->line_coding, line_coding, sizeof(cdc->line_coding));
    cdc_sync_serial_config_from_line_coding(cdc, line_coding);

    if (cdc->port_index != CDC_ACM_MAGIC_REBOOT_PORT) {
        return;
    }

    if (CDC_ACM_MAGIC_REBOOT_BAUDRATE != line_coding->dwDTERate) {
        return;
    }

    if (CDC_ACM_MAGIC_REBOOT_STOPBITS != line_coding->bCharFormat) {
        return;
    }

    if (CDC_ACM_MAGIC_REBOOT_PARITY != line_coding->bParityType) {
        return;
    }

    if (CDC_ACM_MAGIC_REBOOT_DATABITS != line_coding->bDataBits) {
        return;
    }

    cdc->pending_magic_reset = true;
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding* line_coding)
{
    struct cdc_device *cdc;

    (void)busid;

    if (!line_coding) {
        return;
    }

    cdc = cdc_find_by_intf(intf);
    if (!cdc) {
        line_coding->dwDTERate = BAUD_RATE_2000000;
        line_coding->bCharFormat = 0;
        line_coding->bParityType = 0;
        line_coding->bDataBits = 8;
        return;
    }

    usb_memcpy((void *)line_coding, &cdc->line_coding, sizeof(cdc->line_coding));
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts)
{
    struct cdc_device *cdc;

    (void)busid;

    cdc = cdc_find_by_intf(intf);
    if (!cdc) {
        return;
    }

    cdc->last_rts_state = rts;

    if (cdc->port_index != CDC_ACM_MAGIC_REBOOT_PORT) {
        return;
    }

    if (!cdc->pending_magic_reset) {
        return;
    }

    if (!cdc->last_dtr_state) {
        return;
    }

    if (!cdc->send_break_flag) {
        return;
    }

    if (!rts) {
        return;
    }

    extern void reboot_to_upgrade(void);
    reboot_to_upgrade();
}

void usbd_cdc_acm_send_break(uint8_t busid, uint8_t intf)
{
    struct cdc_device *cdc;

    (void)busid;

    cdc = cdc_find_by_intf(intf);
    if (!cdc) {
        return;
    }

    cdc->send_break_flag = true;
}

#endif
