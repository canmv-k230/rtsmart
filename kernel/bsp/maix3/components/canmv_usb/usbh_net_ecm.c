#include "netif/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "rtdef.h"

#if LWIP_DHCP
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#endif

#include <rtdevice.h>
#include <netif/ethernetif.h>
#include <netdev.h>

#include "usbh_core.h"
#include "usbh_cdc_ecm.h"

struct netif g_cdc_ecm_netif;

static struct eth_device cdc_ecm_dev;
static bool cdc_ecm_netdev_inited;

static rt_err_t rt_usbh_cdc_ecm_control(rt_device_t dev, int cmd, void *args)
{
    struct usbh_cdc_ecm *cdc_ecm_class = (struct usbh_cdc_ecm *)dev->user_data;

    if (cdc_ecm_class == RT_NULL || cdc_ecm_class->stop_requested) {
        return -RT_ERROR;
    }

    switch (cmd) {
        case NIOCTL_GADDR:

            /* get mac address */
            if (args)
                rt_memcpy(args, cdc_ecm_class->mac, 6);
            else
                return -RT_ERROR;

            break;
        case 0x1000: // set mac
            // can we write mac address?

            break;
        case 0x1001: // get connect_status
            *(bool *)args = cdc_ecm_class->connect_status;
            break;

        default:
            break;
    }

    return RT_EOK;
}

static rt_err_t rt_usbh_cdc_ecm_eth_tx(rt_device_t dev, struct pbuf *p)
{
    return usbh_cdc_ecm_linkoutput(NULL, p);
}

const static struct rt_device_ops net_ecm_device_ops =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    rt_usbh_cdc_ecm_control
};

void usbh_cdc_ecm_run(struct usbh_cdc_ecm *cdc_ecm_class)
{
    cdc_ecm_class->stop_requested = false;

    if (!cdc_ecm_netdev_inited) {
        usb_memset(&cdc_ecm_dev, 0, sizeof(struct eth_device));

        cdc_ecm_dev.parent.ops = &net_ecm_device_ops;
        cdc_ecm_dev.eth_rx = NULL;
        cdc_ecm_dev.eth_tx = rt_usbh_cdc_ecm_eth_tx;
        cdc_ecm_dev.parent.user_data = cdc_ecm_class;

        eth_device_init(&cdc_ecm_dev, CANMV_USB_HOST_NET_LTE_DEV_NAME);
        cdc_ecm_netdev_inited = true;
    } else {
        cdc_ecm_dev.parent.user_data = cdc_ecm_class;
    }

    if (cdc_ecm_dev.netif) {
        rt_memcpy(cdc_ecm_dev.netif->hwaddr, cdc_ecm_class->mac,
                  sizeof(cdc_ecm_class->mac));
    }

    eth_device_linkchange(&cdc_ecm_dev, RT_FALSE);

    cdc_ecm_class->rx_thread_running = true;
    usb_osal_thread_create("usbh_cdc_ecm_rx", 4096, CONFIG_USBHOST_PSC_PRIO + 1, usbh_cdc_ecm_rx_thread, cdc_ecm_dev.netif);
}

void usbh_cdc_ecm_stop(struct usbh_cdc_ecm *cdc_ecm_class)
{
    cdc_ecm_class->stop_requested = true;
    eth_device_linkchange(&cdc_ecm_dev, RT_FALSE);
    cdc_ecm_dev.parent.user_data = RT_NULL;

    while (cdc_ecm_class->rx_thread_running) {
        usb_osal_msleep(10);
    }
}

void usbh_cdc_ecm_link_changed(struct usbh_cdc_ecm *cdc_ecm_class, bool state)
{
    (void)cdc_ecm_class;
    eth_device_linkchange(&cdc_ecm_dev, state ? RT_TRUE : RT_FALSE);
}
