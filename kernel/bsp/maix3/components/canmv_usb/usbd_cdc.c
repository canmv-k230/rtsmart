#include <lwp.h>
#include <lwp_user_mm.h>
#include <rtdevice.h>
#include <rtthread.h>

#include "usbd_desc.h"

#if defined(CHERRY_USB_DEVICE_FUNC_CDC) || defined (CHERRY_USB_DEVICE_FUNC_CDC_MTP)

#ifdef RT_SERIAL_USING_DMA

#include <dfs_posix.h>
#include <drivers/serial.h>

struct cdc_device {
    struct rt_serial_device serial;
    uint8_t                 busid;
    uint8_t                 in_ep;
    uint8_t                 out_ep;
    struct usbd_interface   intf_ctrl;
    struct usbd_interface   intf_data;
    int                     cdc_dtr;
    bool                    is_open;
};

#define CDC_MAX_MPS          USB_DEVICE_MAX_MPS
#define CDC_READ_BUFFER_SIZE (4096)

static struct cdc_device g_usbd_serial_cdc_acm;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_usbd_serial_cdc_acm_rx_buf[USB_ALIGN_UP(CDC_READ_BUFFER_SIZE, CONFIG_USB_ALIGN_SIZE)];

static rt_err_t cdc_configure(struct rt_serial_device* serial, struct serial_configure* cfg) { return RT_EOK; }

static rt_err_t cdc_control(struct rt_serial_device* serial, int cmd, void* arg)
{
    int                ret = RT_EOK;
    struct cdc_device* cdc;

    cdc = (struct cdc_device*)serial->parent.user_data;

    switch (cmd) {
    case RT_DEVICE_CTRL_CONFIG: {
        if (arg == (void*)RT_DEVICE_FLAG_DMA_RX) {
            cdc->is_open = RT_TRUE;
            cdc->cdc_dtr = 0;
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

    cdc = (struct cdc_device*)serial->parent.user_data;

    if (dir == RT_SERIAL_DMA_TX) {
        usbd_ep_start_write(cdc->busid, cdc->in_ep, buf, size);
        return size;
    }

    return 0;
}

void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    struct cdc_device* cdc;

    cdc = &g_usbd_serial_cdc_acm;
    if (cdc->is_open) {
        rt_serial_put_rxfifo(&cdc->serial, g_usbd_serial_cdc_acm_rx_buf, nbytes);
        rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_RX_DMADONE);
    }
    usbd_ep_start_read(cdc->busid, cdc->out_ep, g_usbd_serial_cdc_acm_rx_buf, CDC_READ_BUFFER_SIZE);
}

void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    struct cdc_device* cdc;

    if ((nbytes % CDC_MAX_MPS) == 0 && nbytes) {
        /* send zlp */
        usbd_ep_start_write(busid, ep, NULL, 0);
    } else {
        cdc = &g_usbd_serial_cdc_acm;
        rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_TX_DMADONE);
    }
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding* line_coding) { }

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding* line_coding)
{
    line_coding->dwDTERate   = 2000000;
    line_coding->bDataBits   = 8;
    line_coding->bParityType = 0;
    line_coding->bCharFormat = 0;
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    struct cdc_device* cdc;

    cdc          = &g_usbd_serial_cdc_acm;
    cdc->cdc_dtr = (int)dtr;
    rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_HOTPLUG);
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts) { }

void usbd_cdc_acm_send_break(uint8_t busid, uint8_t intf) { }

void canmv_usb_device_cdc_on_connected(void)
{
    struct cdc_device* cdc;

    cdc          = &g_usbd_serial_cdc_acm;
    cdc->cdc_dtr = 0;
    if (cdc->is_open) {
        rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_TX_DMADONE);
        rt_hw_serial_isr(&cdc->serial, RT_SERIAL_EVENT_HOTPLUG);
    }
    usbd_ep_start_read(cdc->busid, cdc->out_ep, g_usbd_serial_cdc_acm_rx_buf, CDC_READ_BUFFER_SIZE);
}

static const struct rt_uart_ops cdc_ops = { .configure = cdc_configure, .control = cdc_control, .dma_transmit = cdc_transmit };

rt_err_t usbd_serial_register(struct cdc_device* cdc, void* data)
{
    int                     ret;
    struct serial_configure config;

    config.bufsz = CDC_READ_BUFFER_SIZE * 4;

    cdc->serial.ops    = &cdc_ops;
    cdc->serial.config = config;

    ret = rt_hw_serial_register(&cdc->serial, "ttyUSB", RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_DMA_TX,
                                cdc);

    return ret;
}

void canmv_usb_device_cdc_init(void)
{
    struct cdc_device* cdc;
    uint8_t            busid = USB_DEVICE_BUS_ID;
    uint8_t            in_ep = CDC_IN_EP, out_ep = CDC_OUT_EP;

    struct usbd_endpoint cdc_out_ep = { .ep_addr = out_ep, .ep_cb = usbd_cdc_acm_bulk_out };

    struct usbd_endpoint cdc_in_ep = { .ep_addr = in_ep, .ep_cb = usbd_cdc_acm_bulk_in };

    cdc = &g_usbd_serial_cdc_acm;

    cdc->is_open = RT_FALSE;
    cdc->busid   = busid;
    cdc->in_ep   = in_ep;
    cdc->out_ep  = out_ep;

    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &cdc->intf_ctrl));
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &cdc->intf_data));
    usbd_add_endpoint(busid, &cdc_out_ep);
    usbd_add_endpoint(busid, &cdc_in_ep);

    if (usbd_serial_register(cdc, NULL) != RT_EOK) {
        USB_LOG_ERR("Failed to register usb_serial device\n");
    }
}

#else

#include "dfs_file.h"
#include "dfs_poll.h"
#include "ipc/waitqueue.h"

#define CDC_MAX_MPS USB_DEVICE_MAX_MPS

/*****************************************************************************/
static int      cdc_poll_flag;
static uint32_t actual_read;

static USB_MEM_ALIGNX uint8_t usb_read_buffer[4096];

static struct rt_device     cdc_device;
static struct rt_semaphore  cdc_read_sem, cdc_write_sem;
static struct rt_completion cdc_write_done;
static int                  cdc_dtr = 0;

static int cdc_open(struct dfs_fd* fd) { return 0; }

static int cdc_close(struct dfs_fd* fd) { return 0; }

static int cdc_read(struct dfs_fd* fd, void* buf, size_t count)
{
    int      read_count = -1;
    rt_err_t error      = RT_ERROR;

    if (RT_EOK == (error = rt_sem_take(&cdc_read_sem, rt_tick_from_millisecond(100)))) {
        read_count  = actual_read;
        actual_read = 0;

        if (0 < read_count) {
            memcpy(buf, usb_read_buffer, read_count);
        }
    } else {
        if (actual_read) {
            USB_LOG_WRN("read %d but not copy\n", actual_read);
        }

        if ((-RT_ETIMEOUT) == error) {
            read_count = 0;

            USB_LOG_WRN("read timeout\n");
        }
    }

    usbd_ep_start_read(USB_DEVICE_BUS_ID, CDC_OUT_EP, usb_read_buffer, sizeof(usb_read_buffer));

    return read_count;
}

static int cdc_write(struct dfs_fd* fd, const void* buf, size_t count)
{
    rt_err_t   error   = RT_ERROR;
    rt_int32_t timeout = RT_WAITING_FOREVER;

    if (count == 0 || (false == g_usb_device_connected)) {
        return -1; /* error */
    }

    if (RT_EOK != (error = rt_sem_take(&cdc_write_sem, timeout))) {
        if (error == -RT_ETIMEOUT) {
            rt_kprintf("sem timeout len = %d\n", count);
        } else {
            rt_kprintf("sem error %d\n", error);
        }
    } else {
        usbd_ep_start_write(USB_DEVICE_BUS_ID, CDC_IN_EP, buf, count);
        if (RT_EOK != (error = rt_completion_wait(&cdc_write_done, timeout))) {
            if (error == -RT_ETIMEOUT) {
                rt_kprintf("completion timeout len = %d\n", count);
            } else {
                rt_kprintf("completion error %d\n", error);
            }
        } else {
            return count;
        }
    }

    return 0;
}

static int cdc_ioctl(struct dfs_fd* fd, int cmd, void* args)
{
    int ret = RT_EOK;

    switch (cmd) {
    case UART_IOCTL_GET_DTR:
        if (sizeof(int) != lwp_put_to_user(args, &cdc_dtr, sizeof(int))) {
            USB_LOG_ERR("lwp put error size\n");
            ret = -RT_EINVAL;
        }
        break;
    default:
        USB_LOG_ERR("Unsupport cmd %d in %s\n", cmd, __func__);
        ret = -RT_ERROR;
        break;
    }

    return ret;
}

static int cdc_poll(struct dfs_fd* fd, struct rt_pollreq* req)
{
    rt_poll_add(&cdc_device.wait_queue, req);
    int tmp       = cdc_poll_flag;
    cdc_poll_flag = 0;
    return tmp;
}

static const struct dfs_file_ops cdc_ops
    = { .open = cdc_open, .close = cdc_close, .read = cdc_read, .write = cdc_write, .ioctl = cdc_ioctl, .poll = cdc_poll };

static void cdc_device_init(void)
{
    rt_err_t err = rt_device_register(&cdc_device, "ttyUSB", RT_DEVICE_OFLAG_RDWR);
    if (err != RT_EOK) {
        rt_kprintf("[usb] register device error\n");
        return;
    }
    cdc_device.fops = &cdc_ops;
    rt_sem_init(&cdc_read_sem, "cdc_read", 0, RT_IPC_FLAG_FIFO);
    rt_sem_init(&cdc_write_sem, "cdc_write", 1, RT_IPC_FLAG_FIFO);
    rt_completion_init(&cdc_write_done);
}

/*****************************************************************************/
static void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    actual_read = nbytes;
    cdc_poll_flag |= POLLIN;
    rt_wqueue_wakeup(&cdc_device.wait_queue, (void*)POLLIN);
    rt_sem_release(&cdc_read_sem);
}

static void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (nbytes && (0x00 == (nbytes % CDC_MAX_MPS))) {
        usbd_ep_start_write(USB_DEVICE_BUS_ID, CDC_IN_EP, NULL, 0);
    } else {
        rt_completion_done(&cdc_write_done);
        rt_sem_release(&cdc_write_sem);
    }
}

static struct usbd_interface usbd_cdc_intf;
static struct usbd_interface usbd_cdc_data_unuse;

static struct usbd_endpoint cdc_out_ep = { .ep_addr = CDC_OUT_EP, .ep_cb = usbd_cdc_acm_bulk_out };

static struct usbd_endpoint cdc_in_ep = { .ep_addr = CDC_IN_EP, .ep_cb = usbd_cdc_acm_bulk_in };

void canmv_usb_device_cdc_on_connected(void)
{
    actual_read = -1;
    rt_sem_control(&cdc_read_sem, RT_IPC_CMD_RESET, (void*)1);
    rt_sem_control(&cdc_write_sem, RT_IPC_CMD_RESET, (void*)1);
    // TODO
    rt_completion_done(&cdc_write_done);
    rt_completion_init(&cdc_write_done);

    cdc_dtr = 0;
    usbd_ep_start_read(USB_DEVICE_BUS_ID, CDC_OUT_EP, usb_read_buffer, sizeof(usb_read_buffer));
}

void canmv_usb_device_cdc_init(void)
{
    usbd_cdc_acm_init_intf(USB_DEVICE_BUS_ID, &usbd_cdc_intf);
    usbd_add_interface(USB_DEVICE_BUS_ID, &usbd_cdc_intf);
    usbd_add_interface(USB_DEVICE_BUS_ID, &usbd_cdc_data_unuse);
    usbd_add_endpoint(USB_DEVICE_BUS_ID, &cdc_out_ep);
    usbd_add_endpoint(USB_DEVICE_BUS_ID, &cdc_in_ep);

    cdc_device_init();
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding* line_coding) { }

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding* line_coding)
{
    line_coding->dwDTERate   = 2000000;
    line_coding->bDataBits   = 8;
    line_coding->bParityType = 0;
    line_coding->bCharFormat = 0;
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    cdc_poll_flag |= POLLERR;

    cdc_dtr = (int)dtr;
    rt_wqueue_wakeup(&cdc_device.wait_queue, (void*)POLLERR);
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts) { }

void usbd_cdc_acm_send_break(uint8_t busid, uint8_t intf) { }
#endif

#endif
