#pragma once

#include <stdbool.h>

#include "rtthread.h"

#include "usb_osal.h"

#if defined(ENABLE_CHERRY_USB_DEVICE)
#include "usbd_core.h"
#endif

#if defined(ENABLE_CHERRY_USB_HOST)
#include "usbh_core.h"
#endif

#define USB_DEVICE_BUS_ID  0x00
#define USBD_MAX_POWER     500
#define USBD_LANGID_STRING 1033

// OpenMV Cam
#ifndef CHERRY_USB_DEVICE_VID
#define CHERRY_USB_DEVICE_VID 0x1209
#endif

#ifndef CHERRY_USB_DEVICE_PID
#define CHERRY_USB_DEVICE_PID 0xABD1
#endif

#ifdef CONFIG_USB_HS
#define USB_DEVICE_MAX_MPS 512
#else
#define USB_DEVICE_MAX_MPS 64
#endif

#define USB_DEVICE_FS_MAX_MPS 64

#define CANMV_USB_CONFIG_DESCRIPTOR_INIT(descriptor_type, wTotalLength, bNumInterfaces,                   \
                                         bConfigurationValue, bmAttributes, bMaxPower)                    \
    0x09, descriptor_type, WBVAL(wTotalLength), bNumInterfaces, bConfigurationValue, 0x00, bmAttributes, \
        USB_CONFIG_POWER_MA(bMaxPower)

#define CANMV_USB_MTP_DESCRIPTOR_INIT(bFirstInterface, out_ep, in_ep, int_ep, str_idx, max_packet_size) \
    USB_INTERFACE_DESCRIPTOR_INIT(bFirstInterface, 0x00, 0x03, USB_MTP_CLASS, USB_MTP_SUB_CLASS,        \
                                  USB_MTP_PROTOCOL, str_idx),                                           \
    USB_ENDPOINT_DESCRIPTOR_INIT(out_ep, USB_ENDPOINT_TYPE_BULK, max_packet_size, 0x00),                \
    USB_ENDPOINT_DESCRIPTOR_INIT(in_ep, USB_ENDPOINT_TYPE_BULK, max_packet_size, 0x00),                 \
    USB_ENDPOINT_DESCRIPTOR_INIT(int_ep, USB_ENDPOINT_TYPE_INTERRUPT, 0x1c, 0x06)

#ifdef CHERRY_USB_DEVICE_FUNC_CDC
#include "usbd_desc_cdc.h"
#endif

#ifdef CHERRY_USB_DEVICE_FUNC_HID
#include "usbd_desc_hid.h"
#endif

#ifdef CHERRY_USB_DEVICE_FUNC_CDC_MTP
#include "usbd_desc_cdc_mtp.h"
#endif

#ifdef CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP
#include "usbd_desc_hid_cdc_mtp.h"
#endif

#ifdef CHERRY_USB_DEVICE_FUNC_CDC_ADB
#include "usbd_desc_cdc_adb.h"
#endif

#ifdef CHERRY_USB_DEVICE_FUNC_ADB
#include "usbd_desc_adb.h"
#endif

#ifdef CHERRY_USB_DEVICE_FUNC_UVC
#include "usbd_desc_uvc.h"
#endif

extern bool g_usb_device_connected;

extern void canmv_usb_device_cdc_on_connected(void);
extern void canmv_usb_device_cdc_on_disconnected(void);
extern void canmv_usb_device_cdc_init(void);

extern void canmv_usb_device_hid_on_connected(void);
extern void canmv_usb_device_hid_on_disconnected(void);
extern void canmv_usb_device_hid_init(void);

extern void canmv_usb_device_mtp_init(void);

extern void canmv_usb_device_uvc_on_connected(void);
extern void canmv_usb_device_uvc_on_disconnected(void);
extern void canmv_usb_device_uvc_init(void);

extern void board_usb_device_event_handler(uint8_t busid, uint8_t event);
extern void board_usb_device_register(void);
extern void board_usb_device_init(void* usb_base);

extern void canmv_usb_device_adb_init(void);
