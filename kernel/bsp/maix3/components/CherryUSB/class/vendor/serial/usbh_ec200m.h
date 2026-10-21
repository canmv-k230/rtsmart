#ifndef __USBH_EC200M_H__
#define __USBH_EC200M_H__

#include <rthw.h>
#include <rtthread.h>
#include "usbh_core.h"
#include <drivers/serial.h>

#ifdef __cplusplus
extern "C" {
#endif

struct usbh_ec200m {
    struct rt_serial_device serial;
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *intin;
    struct usb_endpoint_descriptor *bulkin;
    struct usb_endpoint_descriptor *bulkout;
    struct usbh_urb bulkout_urb;
    struct usbh_urb bulkin_urb;
    struct usbh_urb intin_urb; /* not surpport now */
    rt_uint8_t *bulkin_buffer;
    struct rt_semaphore wait_close;

    rt_uint16_t bufsz;

    rt_uint32_t rx_err_cnt; /* consecutive bulk-in errors (reset on data/NAK) */

    uint8_t intf;
    uint8_t minor;
};

#ifdef __cplusplus
}
#endif

#endif
