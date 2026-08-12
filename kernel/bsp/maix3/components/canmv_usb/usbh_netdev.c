#include <rtdevice.h>
#include <rtthread.h>

#include "usbh_netdev.h"

#define DBG_TAG "usb.net"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define USB_NETDEV_NAME_COUNT 10

rt_err_t canmv_usbh_netdev_init(struct eth_device *device)
{
    char name[RT_NAME_MAX];
    rt_err_t result;
    rt_uint16_t flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    int index;

#if LWIP_IGMP
    flags |= NETIF_FLAG_IGMP;
#endif

    if (!device)
    {
        return -RT_EINVAL;
    }

    for (index = 0; index < USB_NETDEV_NAME_COUNT; index++)
    {
        struct netdev *netdev;

        rt_snprintf(name, sizeof(name), "eth%d", index);
        if (rt_device_find(name) || netdev_get_by_name(name))
        {
            continue;
        }
        result = eth_device_init_with_flag(device, name, flags);
        if (result != RT_EOK)
        {
            /* A concurrent probe may have claimed this slot. */
            if (rt_device_find(name) || netdev_get_by_name(name))
            {
                continue;
            }
            LOG_E("cannot register USB network interface %s: %d", name,
                  result);
            return result;
        }
        netdev = netdev_get_by_name(name);
        if (!netdev)
        {
            LOG_E("USB network interface %s has no netdev", name);
            return -RT_EIO;
        }
        netdev_set_type(netdev, NETDEV_TYPE_LAN);
        LOG_I("registered LAN interface as %s", name);
        return RT_EOK;
    }

    LOG_E("no free USB network interface name");
    return -RT_EFULL;
}
