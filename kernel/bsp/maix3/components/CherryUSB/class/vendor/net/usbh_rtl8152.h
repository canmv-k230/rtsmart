/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBH_RTL8152_H
#define USBH_RTL8152_H

#include "lwip/netif.h"
#include "lwip/pbuf.h"

#define RTL8152_MAX_TX 4
#define RTL8152_MAX_RX 4

struct usbh_rtl8152;
struct rtl8152_link_recovery;

/* Chip policy. USB transport, packet framing and the RT-Thread netdev are
 * shared; select this immutable table once when the adapter is attached. */
struct rtl8152_chip_ops {
    const char *name;
    uint8_t rx_urb_count;
    void (*init)(struct usbh_rtl8152 *tp);
    void (*hw_phy_cfg)(struct usbh_rtl8152 *tp);
    /* Order MAC preparation and the final PHY reset for this chip. */
    int (*start)(struct usbh_rtl8152 *tp);
    void (*set_eee)(struct usbh_rtl8152 *tp, bool enable);
    void (*set_aldps)(struct usbh_rtl8152 *tp, bool enable);
    int (*set_speed)(struct usbh_rtl8152 *tp, uint8_t autoneg,
                     uint16_t speed, uint8_t duplex);
    int (*get_mac)(struct usbh_rtl8152 *tp, uint8_t *mac);
    int (*set_mac)(struct usbh_rtl8152 *tp, unsigned char *mac);
    int (*enable)(struct usbh_rtl8152 *tp);
    void (*disable)(struct usbh_rtl8152 *tp);
    void (*up)(struct usbh_rtl8152 *tp);
    void (*down)(struct usbh_rtl8152 *tp);
    int (*recover_initial_link)(struct usbh_rtl8152 *tp,
                                struct rtl8152_link_recovery *state);
};

struct usbh_rtl8152 {
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *bulkin;  /* Bulk IN endpoint */
    struct usb_endpoint_descriptor *bulkout; /* Bulk OUT endpoint */
    struct usb_endpoint_descriptor *intin;   /* INTR IN endpoint  */
    struct usbh_urb bulkout_urb[RTL8152_MAX_TX];
    struct usbh_urb bulkin_urb[RTL8152_MAX_RX];
    struct usbh_urb intin_urb;

    uint8_t intf;
    uint8_t rx_urb_count;

    uint8_t mac[6];
    bool connect_status;
    bool stop_requested;
    bool rx_thread_running;
    bool plug;
    int io_error;
    uint32_t speed[2];

    ip_addr_t ipaddr;
    ip_addr_t netmask;
    ip_addr_t gateway;

    uint8_t version;
    uint8_t eee_adv;
    uint8_t eee_en;
    uint8_t supports_gmii;
    uint16_t min_mtu;
    uint16_t max_mtu;
    uint16_t ocp_base;
    uint32_t saved_wolopts;
    uint32_t rx_buf_sz;
    bool phy_reset_pending;
    const struct rtl8152_chip_ops *chip_ops;
};

#ifdef __cplusplus
extern "C" {
#endif

int usbh_rtl8152_run(struct usbh_rtl8152 *rtl8152_class);
void usbh_rtl8152_stop(struct usbh_rtl8152 *rtl8152_class);
void usbh_rtl8152_link_changed(struct usbh_rtl8152 *rtl8152_class, int state);

void usbh_rtl8152_rx_thread(void *argument);
err_t usbh_rtl8152_linkoutput(struct netif *netif, struct pbuf *p);

#ifdef __cplusplus
}
#endif

#endif /* USBH_RTL8152_H */
