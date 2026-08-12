#ifndef __CANMV_USBH_NETDEV_H__
#define __CANMV_USBH_NETDEV_H__

#include <netif/ethernetif.h>
#include <netdev.h>

rt_err_t canmv_usbh_netdev_init(struct eth_device *device);

#endif
