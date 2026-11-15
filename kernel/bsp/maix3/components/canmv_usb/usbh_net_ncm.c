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
#include "usbh_cdc_ncm.h"
#include "usbh_netdev.h"

#define DBG_TAG "usb.ncm"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

struct netif g_cdc_ncm_netif;

static struct eth_device cdc_ncm_dev;
static bool cdc_ncm_netdev_inited;

static rt_err_t rt_usbh_cdc_ncm_control(rt_device_t dev, int cmd, void *args)
{
    struct usbh_cdc_ncm *cdc_ncm_class = (struct usbh_cdc_ncm *)dev->user_data;

    if (cdc_ncm_class == RT_NULL || cdc_ncm_class->stop_requested) {
        return -RT_ERROR;
    }

    switch (cmd) {
        case NIOCTL_GADDR:

            /* get mac address */
            if (args)
                rt_memcpy(args, cdc_ncm_class->mac, 6);
            else
                return -RT_ERROR;

            break;
        case 0x1000: // set mac
            // can we write mac address?

            break;
        case 0x1001: // get connect_status
            *(bool *)args = cdc_ncm_class->connect_status;
            break;

        default:
            break;
    }

    return RT_EOK;
}

static rt_err_t rt_usbh_cdc_ncm_eth_tx(rt_device_t dev, struct pbuf *p)
{
    return usbh_cdc_ncm_linkoutput(NULL, p);
}

const static struct rt_device_ops net_ncm_device_ops =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    rt_usbh_cdc_ncm_control
};

void usbh_cdc_ncm_run(struct usbh_cdc_ncm *cdc_ncm_class)
{
    cdc_ncm_class->stop_requested = false;

    if (!cdc_ncm_netdev_inited) {
        usb_memset(&cdc_ncm_dev, 0, sizeof(struct eth_device));

        cdc_ncm_dev.parent.ops = &net_ncm_device_ops;
        cdc_ncm_dev.eth_rx = NULL;
        cdc_ncm_dev.eth_tx = rt_usbh_cdc_ncm_eth_tx;
        cdc_ncm_dev.parent.user_data = cdc_ncm_class;

        if (canmv_usbh_netdev_init(&cdc_ncm_dev) != RT_EOK) {
            cdc_ncm_dev.parent.user_data = RT_NULL;
            return;
        }
        cdc_ncm_netdev_inited = true;
    } else {
        cdc_ncm_dev.parent.user_data = cdc_ncm_class;
    }

    if (cdc_ncm_dev.netif) {
        rt_memcpy(cdc_ncm_dev.netif->hwaddr, cdc_ncm_class->mac,
                  sizeof(cdc_ncm_class->mac));
    }

    eth_device_linkchange(&cdc_ncm_dev, RT_FALSE);

    cdc_ncm_class->rx_thread_running = true;
    if (usb_osal_thread_create("usbh_cdc_ncm_rx", 4096,
                               CONFIG_USBHOST_PSC_PRIO + 1,
                               usbh_cdc_ncm_rx_thread,
                               cdc_ncm_dev.netif) == NULL) {
        cdc_ncm_class->rx_thread_running = false;
        cdc_ncm_dev.parent.user_data = RT_NULL;
        LOG_E("failed to start CDC NCM receive thread");
        return;
    }

    LOG_I("CDC NCM attached as %s, MAC %02x:%02x:%02x:%02x:%02x:%02x",
          cdc_ncm_dev.parent.parent.name,
          cdc_ncm_class->mac[0], cdc_ncm_class->mac[1],
          cdc_ncm_class->mac[2], cdc_ncm_class->mac[3],
          cdc_ncm_class->mac[4], cdc_ncm_class->mac[5]);
}

void usbh_cdc_ncm_stop(struct usbh_cdc_ncm *cdc_ncm_class)
{
    cdc_ncm_class->stop_requested = true;
    eth_device_linkchange(&cdc_ncm_dev, RT_FALSE);
    cdc_ncm_dev.parent.user_data = RT_NULL;

    while (cdc_ncm_class->rx_thread_running) {
        usb_osal_msleep(10);
    }

    LOG_I("CDC NCM detached from %s", cdc_ncm_dev.parent.parent.name);
}

void usbh_cdc_ncm_link_changed(struct usbh_cdc_ncm *cdc_ncm_class, bool state)
{
    (void)cdc_ncm_class;
    eth_device_linkchange(&cdc_ncm_dev, state ? RT_TRUE : RT_FALSE);
}
