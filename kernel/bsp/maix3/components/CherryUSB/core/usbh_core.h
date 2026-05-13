/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBH_CORE_H
#define USBH_CORE_H

#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "usb_config.h"
#include "usb_util.h"
#include "usb_errno.h"
#include "usb_def.h"
#include "usb_list.h"
#include "usb_log.h"
#include "usb_hc.h"
#include "usb_osal.h"
#include "usbh_hub.h"

#ifdef CHERRY_USB_HC_DRV_DWC2
#include <ipc/workqueue.h>
#include <rtthread.h>
#include <rthw.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define USB_CLASS_MATCH_VENDOR        0x0001
#define USB_CLASS_MATCH_PRODUCT       0x0002
#define USB_CLASS_MATCH_INTF_CLASS    0x0004
#define USB_CLASS_MATCH_INTF_SUBCLASS 0x0008
#define USB_CLASS_MATCH_INTF_PROTOCOL 0x0010

/*
 * urb->transfer_flags:
 *
 * Note: URB_DIR_IN/OUT is automatically set in usb_submit_urb().
 */
#define URB_SHORT_NOT_OK	0x0001	/* report short reads as errors */
#define URB_ISO_ASAP		0x0002	/* iso-only; use the first unexpired
                                     * slot in the schedule */
#define URB_NO_TRANSFER_DMA_MAP	0x0004	/* urb->transfer_dma valid on submit */
#define URB_ZERO_PACKET		0x0040	/* Finish bulk OUT with short packet */
#define URB_NO_INTERRUPT	0x0080	/* HINT: no non-error interrupt
                                     * needed */
#define URB_FREE_BUFFER		0x0100	/* Free transfer buffer with the URB */


#define CLASS_CONNECT(hport, i)    ((hport)->config.intf[i].class_driver->connect(hport, i))
#define CLASS_DISCONNECT(hport, i) ((hport)->config.intf[i].class_driver->disconnect(hport, i))

#ifdef __ARMCC_VERSION /* ARM C Compiler */
#define CLASS_INFO_DEFINE __attribute__((section("usbh_class_info"))) __USED __ALIGNED(1)
#elif defined(__GNUC__)
#define CLASS_INFO_DEFINE __attribute__((section(".usbh_class_info"))) __USED __ALIGNED(1)
#elif defined(__ICCARM__) || defined(__ICCRX__) || defined(__ICCRISCV__)
#pragma section = ".usbh_class_info"
#define CLASS_INFO_DEFINE __attribute__((section(".usbh_class_info"))) __USED __ALIGNED(1)
#endif

#define USBH_GET_URB_INTERVAL(interval, speed) (speed < USB_SPEED_HIGH ? interval: (1 << (interval - 1)))

#define USBH_EP_INIT(ep, ep_desc)                                            \
    do {                                                                     \
        ep = ep_desc;                                                        \
        USB_LOG_INFO("Ep=%02x Attr=%02u Mps=%d Interval=%02u Mult=%02u\r\n", \
                     ep_desc->bEndpointAddress,                              \
                     USB_GET_ENDPOINT_TYPE(ep_desc->bmAttributes),           \
                     USB_GET_MAXPACKETSIZE(ep_desc->wMaxPacketSize),         \
                     ep_desc->bInterval,                                     \
                     USB_GET_MULT(ep_desc->bmAttributes));                   \
    } while (0)

struct usbh_class_info {
    uint8_t match_flags; /* Used for product specific matches; range is inclusive */
    uint8_t class;       /* Base device class code */
    uint8_t subclass;    /* Sub-class, depends on base class. Eg. */
    uint8_t protocol;    /* Protocol, depends on base class. Eg. */
    uint16_t vid;        /* Vendor ID (for vendor/product specific devices) */
    uint16_t pid;        /* Product ID (for vendor/product specific devices) */
    const struct usbh_class_driver *class_driver;
};

struct usbh_hubport;
struct usbh_class_driver {
    const char *driver_name;
    int (*connect)(struct usbh_hubport *hport, uint8_t intf);
    int (*disconnect)(struct usbh_hubport *hport, uint8_t intf);
};

struct usbh_endpoint {
    struct usb_endpoint_descriptor ep_desc;
};

struct usbh_interface_altsetting {
    struct usb_interface_descriptor intf_desc;
    struct usbh_endpoint ep[CONFIG_USBHOST_MAX_ENDPOINTS];
};

struct usbh_interface {
    char devname[CONFIG_USBHOST_DEV_NAMELEN];
    struct usbh_class_driver *class_driver;
    void *priv;
    struct usbh_interface_altsetting altsetting[CONFIG_USBHOST_MAX_INTF_ALTSETTINGS];
    uint8_t altsetting_num;
};

struct usbh_configuration {
    struct usb_configuration_descriptor config_desc;
    struct usbh_interface intf[CONFIG_USBHOST_MAX_INTERFACES];
};

#ifdef CHERRY_USB_HC_DRV_DWC2
struct usb_tt_clear {
    struct usb_dlist_node clear_list;
    unsigned tt;
    uint16_t devinfo;
    struct usbh_hcd *hcd;
    struct usb_host_endpoint *ep;
};

struct usb_tt {
    struct usbh_hubport *hub;
    int multi;  /* true means one TT per port */
    unsigned think_time;    /* think time in ns */

#ifdef RT_USING_SMP
    struct rt_spinlock lock;
#else
    rt_spinlock_t lock;
#endif
    struct usb_dlist_node clear_list;    /* of usb_tt_clear */
    struct rt_work clear_work;
    void *hcpriv;   /* HCD private data */
};
#endif

struct usbh_hubport {
    bool connected;   /* True: device connected; false: disconnected */
    uint8_t port;     /* Hub port index */
    uint8_t dev_addr; /* device address */
    uint8_t speed;    /* device speed */
    struct usb_device_descriptor device_desc;
    struct usbh_configuration config;
    const char *iManufacturer;
    const char *iProduct;
    const char *iSerialNumber;
    uint8_t *raw_config_desc;
    struct usb_setup_packet *setup;
    struct usbh_hub *parent;
    struct usbh_bus *bus;
#ifdef CHERRY_USB_HC_DRV_DWC2
    struct usb_tt *tt;
    int maxchild;
    int ttport;
    struct usb_host_endpoint hep_in[16];
    struct usb_host_endpoint hep_out[16];
#endif
#ifdef CONFIG_USBHOST_XHCI
    uint32_t protocol; /* port protocol, for xhci, some ports are USB2.0, others are USB3.0 */
#endif
    struct usb_endpoint_descriptor ep0;
    struct usbh_urb ep0_urb;
    usb_osal_mutex_t mutex;
};

struct usbh_hub {
    usb_slist_t list;
    bool connected;
    bool is_roothub;
    uint8_t index;
    uint8_t hub_addr;
#ifdef CHERRY_USB_HC_DRV_DWC2
    struct usb_tt tt;
#endif
    struct usb_hub_descriptor hub_desc;
    struct usbh_hubport child[CONFIG_USBHOST_MAX_EHPORTS];
    struct usbh_hubport *parent;
    struct usbh_bus *bus;
    struct usb_endpoint_descriptor *intin;
    struct usbh_urb intin_urb;
    uint8_t *int_buffer;
    struct usb_osal_timer *int_timer;
};

struct usbh_devaddr_map {
    /**
     * alloctab[0]:addr from 0~31
     * alloctab[1]:addr from 32~63
     * alloctab[2]:addr from 64~95
     * alloctab[3]:addr from 96~127
     *
     */
    uint8_t next;         /* Next device address */
    uint32_t alloctab[4]; /* Bit allocation table */
};

struct usbh_hcd {
    uint32_t reg_base;
    uint8_t hcd_id;
    uint8_t roothub_intbuf[1];
    struct usbh_hub roothub;
#ifdef CHERRY_USB_HC_DRV_DWC2
    struct usbh_hubport root_hub;
    void *hcd_priv;
#endif
};

struct usbh_bus {
    usb_slist_t list;
    uint8_t busid;
    struct usbh_hcd hcd;
    struct usbh_devaddr_map devgen;
    usb_osal_thread_t hub_thread;
    usb_osal_mq_t hub_mq;
    usb_slist_t hub_list;
#ifdef CHERRY_USB_HC_DRV_DWC2
    int bandwidth_allocated;	/* on this bus: how much of the time
                                 * reserved for periodic (intr/iso)
                                 * requests is used, on average?
                                 * Units: microseconds/frame.
                                 * Limits: Full/low speed reserve 90%,
                                 * while high speed reserves 80%.
                                 */
    int bandwidth_int_reqs;		/* number of Interrupt requests */
    int bandwidth_isoc_reqs;	/* number of Isoc. requests */
#endif
};

static inline bool usbh_intf_slot_valid(const struct usbh_hubport *hport, uint8_t intf)
{
    return intf < CONFIG_USBHOST_MAX_INTERFACES &&
           hport->config.intf[intf].altsetting_num > 0;
}

static inline void usbh_control_urb_fill(struct usbh_urb *urb,
                                         struct usbh_hubport *hport,
                                         struct usb_setup_packet *setup,
                                         uint8_t *transfer_buffer,
                                         uint32_t transfer_buffer_length,
                                         uint32_t timeout,
                                         usbh_complete_callback_t complete,
                                         void *arg)
{
    urb->hport = hport;
    urb->ep = &hport->ep0;
    urb->setup = setup;
    urb->transfer_buffer = transfer_buffer;
    urb->transfer_buffer_length = transfer_buffer_length;
    urb->timeout = timeout;
    urb->complete = complete;
    urb->arg = arg;
}

static inline void usbh_bulk_urb_fill(struct usbh_urb *urb,
                                      struct usbh_hubport *hport,
                                      struct usb_endpoint_descriptor *ep,
                                      uint8_t *transfer_buffer,
                                      uint32_t transfer_buffer_length,
                                      uint32_t timeout,
                                      usbh_complete_callback_t complete,
                                      void *arg)
{
    urb->hport = hport;
    urb->ep = ep;
    urb->setup = NULL;
    urb->transfer_buffer = transfer_buffer;
    urb->transfer_buffer_length = transfer_buffer_length;
    urb->timeout = timeout;
    urb->complete = complete;
    urb->arg = arg;
}

static inline void usbh_int_urb_fill(struct usbh_urb *urb,
                                     struct usbh_hubport *hport,
                                     struct usb_endpoint_descriptor *ep,
                                     uint8_t *transfer_buffer,
                                     uint32_t transfer_buffer_length,
                                     uint32_t timeout,
                                     usbh_complete_callback_t complete,
                                     void *arg)
{
    urb->hport = hport;
    urb->ep = ep;
    urb->setup = NULL;
    urb->transfer_buffer = transfer_buffer;
    urb->transfer_buffer_length = transfer_buffer_length;
    urb->timeout = timeout;
    urb->complete = complete;
    urb->arg = arg;
    urb->interval = USBH_GET_URB_INTERVAL(ep->bInterval, hport->speed);
}

extern struct usbh_bus g_usbhost_bus[];
#ifdef USBH_IRQHandler
#error USBH_IRQHandler is obsolete, please call USBH_IRQHandler(xxx) in your irq
#endif

/**
 * @brief Submit an control transfer to an endpoint.
 * This is a blocking method; this method will not return until the transfer has completed.
 * Default timeout is 500ms.
 *
 * @param pipe The control endpoint to send/receive the control request.
 * @param setup Setup packet to be sent.
 * @param buffer buffer used for sending the request and for returning any responses.
 * @return On success will return 0, and others indicate fail.
 */
int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup, uint8_t *buffer);

/**
 * @brief Retrieves a USB string descriptor from a specific hub port.
 * 
 * This function is responsible for retrieving the USB string descriptor
 * with the specified index from the USB device connected to the given hub port.
 * The retrieved descriptor is stored in the output buffer provided.
 * 
 * @param hport Pointer to the USB hub port structure.
 * @param index Index of the string descriptor to retrieve.
 * @param output Pointer to the buffer where the retrieved descriptor will be stored.
 * @return On success will return 0, and others indicate fail.
 */
int usbh_get_string_desc(struct usbh_hubport *hport, uint8_t index, uint8_t *output, uint32_t output_len);

/**
 * @brief Sets the alternate setting for a USB interface on a specific hub port.
 * 
 * This function is responsible for setting the alternate setting of the
 * specified USB interface on the USB device connected to the given hub port.
 * The interface and alternate setting are identified by the respective parameters.
 * 
 * @param hport Pointer to the USB hub port structure.
 * @param intf Interface number to set the alternate setting for.
 * @param altsetting Alternate setting value to set for the interface.
 * @return On success will return 0, and others indicate fail.
 */
int usbh_set_interface(struct usbh_hubport *hport, uint8_t intf, uint8_t altsetting);

int usbh_initialize(uint8_t busid, uint32_t reg_base);
int usbh_deinitialize(uint8_t busid);
void *usbh_find_class_instance(const char *devname);

int lsusb(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* USBH_CORE_H */
