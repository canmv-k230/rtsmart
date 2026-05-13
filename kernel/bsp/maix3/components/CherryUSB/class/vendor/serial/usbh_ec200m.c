#include "usbh_ec200m.h"

#define DEV_FORMAT "ttyUSB%d"

#define EC200M_MAX_TTY_INTF (7)
#define EC200M_FIFO_SIZE (512)
#define ALIGN_SIZE (64)
#define INTF_DISCONNECT (0xffff)

static struct usbh_ec200m *g_ec200m[EC200M_MAX_TTY_INTF];
static uint8_t g_dev_inuse = 0;

static struct usbh_ec200m *get_usbh_ec200m(void)
{
    uint8_t devno;

    for (devno = 0; devno < EC200M_MAX_TTY_INTF; devno ++) {
        if ((g_dev_inuse & (1 << devno)) == 0) {
            g_dev_inuse |= (1 << devno);
            g_ec200m[devno] = rt_calloc(1, sizeof(struct usbh_ec200m));
            if (g_ec200m[devno] != NULL) {
                /* alloc ok ,assign devno */
                g_ec200m[devno]->minor = devno;
            }
            break;
        }
    }

    if (devno >= EC200M_MAX_TTY_INTF) {
        USB_LOG_ERR("too much interface = 0x%x\n", g_dev_inuse);
        return NULL;
    } else {
        return g_ec200m[devno];
    }
}

static void put_usbh_ec200m(struct usbh_ec200m *serial)
{
    uint8_t devno = serial->minor;

    if (devno < EC200M_MAX_TTY_INTF) {
        rt_free(g_ec200m[devno]);
        g_ec200m[devno] = RT_NULL;
        g_dev_inuse &= ~(1 << devno);
    }
}

static void usbh_ec200m_indat_callback(void *arg, int nbytes)
{
    struct usbh_ec200m *ec200m = (struct usbh_ec200m *)arg;
    rt_base_t level;

    if (nbytes > 0) {

        level = rt_hw_interrupt_disable();
        rt_serial_put_rxfifo(&ec200m->serial, ec200m->bulkin_buffer, nbytes);
        rt_hw_interrupt_enable(level);
        rt_hw_serial_isr(&ec200m->serial, RT_SERIAL_EVENT_RX_DMADONE);

        usbh_submit_urb(&ec200m->bulkin_urb);
    } else if (nbytes == -USB_ERR_NAK) {
        usbh_submit_urb(&ec200m->bulkin_urb);
    } else {
        USB_LOG_INFO("%s, ret = %d\n", __func__, nbytes);
    }
}

static int usbh_ec200m_bulk_in_transfer(struct usbh_ec200m *ec200m)
{
    int ret = 0;
    struct usbh_urb *urb = &ec200m->bulkin_urb;

    usbh_bulk_urb_fill(urb, ec200m->hport, ec200m->bulkin, ec200m->bulkin_buffer,
                       USB_GET_MAXPACKETSIZE(ec200m->bulkin->wMaxPacketSize),
                       0, (usbh_complete_callback_t)usbh_ec200m_indat_callback, ec200m);

    ret = usbh_submit_urb(urb);
    if (ret < 0) {
        rt_kprintf("submit bulkin urb fail, ret = %d, %s %d\n", ret, __func__, __LINE__);
    }

    return ret;
}

static int usbh_ec200m_bulk_out_transfer(struct usbh_ec200m *ec200m, uint8_t *buf,
                                         int32_t len, uint32_t timeout)
{
    int ret;
    struct usbh_urb *urb = &ec200m->bulkout_urb;

    if (len > ec200m->bulkout->wMaxPacketSize) {
        len = ec200m->bulkout->wMaxPacketSize;
        rt_kprintf("too big len\n");
    }

    usbh_bulk_urb_fill(urb, ec200m->hport, ec200m->bulkout, buf, len, timeout, NULL, NULL);
    ret = usbh_submit_urb(urb);
    if (ret < 0) {
        rt_kprintf("blukout write fail, ret = %d\n", ret);
    } else {
        ret = urb->actual_length;
    }

    return ret;
}

static rt_err_t ec200m_configure(struct rt_serial_device *serial, struct serial_configure* cfg)
{
    struct usbh_ec200m *ec200m;

    ec200m = (struct usbh_ec200m *)serial->parent.user_data;
    rt_sem_take(&(ec200m->wait_close), rt_tick_from_millisecond(1000));

    return RT_EOK;
}

static rt_err_t ec200m_control(struct rt_serial_device *serial, int cmd, void* arg)
{
    int ret = RT_EOK;
    struct usbh_ec200m *ec200m;

    ec200m = (struct usbh_ec200m *)serial->parent.user_data;

    switch (cmd)
    {
    case RT_DEVICE_CTRL_CONFIG: {
        if (arg == (void *)RT_DEVICE_FLAG_DMA_RX) {
            usbh_ec200m_bulk_in_transfer(ec200m);
        }
        break;
    }
    case RT_DEVICE_CTRL_CLR_INT: {
        if (arg == (void *)RT_DEVICE_FLAG_DMA_RX) {
            if (ec200m->bulkin) {
                usbh_kill_urb(&ec200m->bulkin_urb);
            }
        } else if (arg == (void *)RT_DEVICE_FLAG_DMA_TX) {
            if (ec200m->bulkout) {
                usbh_kill_urb(&ec200m->bulkout_urb);
            }
        }
        break;
    }
    case RT_DEVICE_CTRL_CLOSE: {
        if (ec200m->intin) {
            usbh_kill_urb(&ec200m->intin_urb);
        }
        rt_sem_release(&(ec200m->wait_close));
        break;
    }
    default:
        USB_LOG_ERR("%s: unsupport cmd %d\n", __func__, cmd);
        ret = RT_EINVAL;
        break;
    }

    return ret;
}

static rt_size_t ec200m_transmit(struct rt_serial_device *serial, rt_uint8_t *buf, rt_size_t size, int dir)
{
    struct usbh_ec200m *ec200m;
    int len = 0;

    ec200m = (struct usbh_ec200m *)serial->parent.user_data;

    if (dir == RT_SERIAL_DMA_TX) {
        len = usbh_ec200m_bulk_out_transfer(ec200m, (uint8_t *)buf, size, 3000);
        rt_hw_serial_isr(&ec200m->serial, RT_SERIAL_EVENT_TX_DMADONE);
    }

    return len;
}

static const struct rt_uart_ops ec200m_ops =
{
    .configure = ec200m_configure,
    .control = ec200m_control,
    .dma_transmit = ec200m_transmit
};

static int usbh_ec200m_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usb_endpoint_descriptor *ep_desc;
    struct rt_device *device;
    int ret = 0;
    struct serial_configure config;

    struct usbh_ec200m *ec200m = get_usbh_ec200m();
    if (ec200m == NULL) {
        rt_kprintf("Fail to get ec200m\n");
        ret = -USB_ERR_RANGE;
        goto err_out;
    }

    ec200m->hport = hport;
    ec200m->intf = intf;

    hport->config.intf[intf].priv = ec200m;

    for (uint8_t i = 0;
         i < hport->config.intf[intf].altsetting[0].intf_desc.bNumEndpoints;
         i ++) {

        ep_desc = &hport->config.intf[intf].altsetting[0].ep[i].ep_desc;
        if (USB_GET_ENDPOINT_TYPE(ep_desc->bmAttributes) == USB_ENDPOINT_TYPE_INTERRUPT) {
            USBH_EP_INIT(ec200m->intin, ep_desc);
        } else {
            if (ep_desc->bEndpointAddress & 0x80) {
                USBH_EP_INIT(ec200m->bulkin, ep_desc);
            } else {
                USBH_EP_INIT(ec200m->bulkout, ep_desc);
            }
        }
    }

    if ((ec200m->bulkin == NULL) || (ec200m->bulkout == NULL)) {
        ret = -USB_ERR_INVAL;
        USB_LOG_ERR("ec200m intf %u missing bulk endpoints\n", intf);
        hport->config.intf[intf].priv = NULL;
        goto free_class;
    }

    ec200m->bulkin_buffer = rt_malloc_align(ec200m->bulkin->wMaxPacketSize, ALIGN_SIZE);
    if (ec200m->bulkin_buffer == NULL) {
        ret = -USB_ERR_NOMEM;
        rt_kprintf("Fail to alloc bulk in buffer\n");
        hport->config.intf[intf].priv = NULL;
        goto free_class;
    }

    snprintf(hport->config.intf[intf].devname, CONFIG_USBHOST_DEV_NAMELEN,
             DEV_FORMAT, ec200m->minor);

    config.bufsz = EC200M_FIFO_SIZE * 4;

    ec200m->serial.ops        = &ec200m_ops;
    ec200m->serial.config     = config;

    ret = rt_hw_serial_register(&ec200m->serial, hport->config.intf[intf].devname,
                                RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_DMA_TX, ec200m);
    if (ret) {
        rt_kprintf("ec200m device register fail\n");
        goto free_inbuffer;
    }

    rt_sem_init(&(ec200m->wait_close), "ec200m_wait_close", 1, RT_IPC_FLAG_FIFO);

    USB_LOG_INFO("register ec200m class:%s = 0x%x\r\n", hport->config.intf[intf].devname, ec200m);

    return 0;

free_inbuffer:
    rt_free_align(ec200m->bulkin_buffer);
    hport->config.intf[intf].priv = NULL;
free_class:
    put_usbh_ec200m(ec200m);
err_out:

    return ret;
}

static int usbh_ec200m_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    int ret = 0;

    struct usbh_ec200m *ec200m = (struct usbh_ec200m *)hport->config.intf[intf].priv;

    if (ec200m) {
        /* Double kill urb have no side effect */
        if (ec200m->bulkin) {
            usbh_kill_urb(&ec200m->bulkin_urb);
        }

        if (ec200m->bulkout) {
            usbh_kill_urb(&ec200m->bulkout_urb);
        }

        if (ec200m->intin) {
            usbh_kill_urb(&ec200m->intin_urb);
        }

        rt_hw_serial_isr(&ec200m->serial, RT_SERIAL_EVENT_DISCONNECT);

        ret = rt_sem_take(&(ec200m->wait_close), rt_tick_from_millisecond(1000));
        if (ret != RT_EOK) {
            USB_LOG_ERR("wait ec200m_close fail ret = %d\n", ret);
        }

        rt_sem_detach(&(ec200m->wait_close));
        rt_free_align(ec200m->bulkin_buffer);
        rt_hw_serial_unregister(&ec200m->serial);
        put_usbh_ec200m(ec200m);

        if (hport->config.intf[intf].devname[0] != '\0') {
            USB_LOG_INFO("unregister ec200m class:%s = 0x%x\r\n", hport->config.intf[intf].devname, ec200m);
        }
    }

    return ret;
}

const struct usbh_class_driver ec200m_class_driver = {
    .driver_name = "ec200m",
    .connect = usbh_ec200m_connect,
    .disconnect = usbh_ec200m_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info ec200m_class_info = {
    .match_flags = USB_CLASS_MATCH_VENDOR | USB_CLASS_MATCH_PRODUCT | USB_CLASS_MATCH_INTF_CLASS,
    .class = 0xff,
    .subclass = 0xff,
    .protocol = 0xff,
    .vid = 0x2c7c,
    .pid = 0x6002,
    .class_driver = &ec200m_class_driver
};

CLASS_INFO_DEFINE const struct usbh_class_info ec200a_class_info = {
    .match_flags = USB_CLASS_MATCH_VENDOR | USB_CLASS_MATCH_PRODUCT | USB_CLASS_MATCH_INTF_CLASS,
    .class = 0xff,
    .subclass = 0xff,
    .protocol = 0xff,
    .vid = 0x2c7c,
    .pid = 0x6005,
    .class_driver = &ec200m_class_driver
};

CLASS_INFO_DEFINE const struct usbh_class_info ml307b_class_info = {
    .match_flags = USB_CLASS_MATCH_VENDOR | USB_CLASS_MATCH_PRODUCT | USB_CLASS_MATCH_INTF_CLASS,
    .class = 0xff,
    .subclass = 0xff,
    .protocol = 0xff,
    .vid = 0x2ecc,
    .pid = 0x3012,
    .class_driver = &ec200m_class_driver
};

CLASS_INFO_DEFINE const struct usbh_class_info simcom_a7680c_class_info = {
    .match_flags = USB_CLASS_MATCH_VENDOR | USB_CLASS_MATCH_PRODUCT | USB_CLASS_MATCH_INTF_CLASS,
    .class = 0xff,
    .subclass = 0xff,
    .protocol = 0xff,
    .vid = 0x1e0e,
    .pid = 0x9011,
    .class_driver = &ec200m_class_driver
};
