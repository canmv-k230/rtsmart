/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USB_HC_H
#define USB_HC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*usbh_complete_callback_t)(void *arg, int nbytes);

struct usbh_bus;

/**
 * @brief USB Iso Configuration.
 *
 * Structure containing the USB Iso configuration.
 */
struct usbh_iso_frame_packet {
    uint8_t *transfer_buffer;
    uint32_t transfer_buffer_length;
    uint32_t actual_length;
    int errorcode;
    uint32_t offset;
    uint32_t length;
};

#ifdef CHERRY_USB_HC_DRV_DWC2
struct usb_host_endpoint {
    void *hcpriv;
};
#endif

/**
 * @brief USB Urb Configuration.
 *
 * Structure containing the USB Urb configuration.
 */

struct usbh_urb {
    void *hcpriv;
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *ep;
#ifdef CHERRY_USB_HC_DRV_DWC2
    struct usb_host_endpoint *hep;
    struct usb_dlist_node urb_list;
    uint32_t pipe;
    int error_count;
    volatile int use_count;
    volatile int reject;
    void *context;
#endif
    uint8_t data_toggle;
#ifdef CHERRY_USB_HC_DRV_DWC2
    uint16_t interval;
#else
    uint8_t interval;
#endif
    struct usb_setup_packet *setup;
    uint8_t *transfer_buffer;
    uint32_t transfer_buffer_length;
    int transfer_flags;
    uint32_t actual_length;
    uint32_t timeout;
    int errorcode;
    uint32_t num_of_iso_packets;
    uint32_t start_frame;
    usbh_complete_callback_t complete;
    void *arg;
#if defined(__ICCARM__) || defined(__ICCRISCV__) || defined(__ICCRX__)
    struct usbh_iso_frame_packet *iso_packet;
#else
    struct usbh_iso_frame_packet iso_packet[0];
#endif
};

/**
 * @brief usb host controller hardware init.
 *
 * @return On success will return 0, and others indicate fail.
 */
int usb_hc_init(struct usbh_bus *bus);

/**
 * @brief usb host controller hardware deinit.
 *
 * @return On success will return 0, and others indicate fail.
 */
int usb_hc_deinit(struct usbh_bus *bus);

/**
 * @brief Get frame number.
 *
 * @return frame number.
 */
uint16_t usbh_get_frame_number(struct usbh_bus *bus);
/**
 * @brief control roothub.
 *
 * @param setup setup request buffer.
 * @param buf buf for reading response or write data.
 * @return On success will return 0, and others indicate fail.
 */
int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf);

/**
 * @brief Submit a usb transfer request to an endpoint.
 *
 * If timeout is not zero, this function will be in poll transfer mode,
 * otherwise will be in async transfer mode.
 *
 * @param urb Usb request block.
 * @return  On success will return 0, and others indicate fail.
 */
int usbh_submit_urb(struct usbh_urb *urb);

/**
 * @brief Cancel a transfer request.
 *
 * This function will call When calls usbh_submit_urb and return -USB_ERR_TIMEOUT or -USB_ERR_SHUTDOWN.
 *
 * @param urb Usb request block.
 * @return  On success will return 0, and others indicate fail.
 */
int usbh_kill_urb(struct usbh_urb *urb);

#ifdef CHERRY_USB_HC_DRV_DWC2
/* Stop submissions and kill all endpoint URBs before calling. Waits for the
 * hardware channel to retire, then resets the host toggle and PING state.
 * Keep submissions stopped until the device's halt has also been cleared. */
int usbh_dwc2_reset_endpoint(struct usbh_hubport *hport, uint8_t endpoint);
#endif

#ifdef __cplusplus
}
#endif

#endif /* USB_HC_H */
