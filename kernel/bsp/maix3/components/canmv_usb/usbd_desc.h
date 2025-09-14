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

#ifdef CHERRY_USB_DEVICE_FUNC_CDC
#include "usbd_desc_cdc.h"
#endif

#ifdef CHERRY_USB_DEVICE_FUNC_CDC_MTP
#include "usbd_desc_cdc_mtp.h"
#endif

#ifdef CHERRY_USB_DEVICE_FUNC_UVC
#include "usbd_desc_uvc.h"
#endif

extern bool g_usb_device_connected;

extern void canmv_usb_device_cdc_on_connected(void);
extern void canmv_usb_device_cdc_init(void);

extern void canmv_usb_device_mtp_init(void);

extern void canmv_usb_device_uvc_on_connected(void);
extern void canmv_usb_device_uvc_init(void);

extern void board_usb_device_init(void* usb_base);
