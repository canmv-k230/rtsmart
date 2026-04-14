#pragma once

#include <stdint.h>

#include "usb_hid.h"

#ifdef CONFIG_USB_HS
#define CANMV_USB_HID_EP_SIZE 512
#define CANMV_USB_HID_EP_INTERVAL 4
#else
#define CANMV_USB_HID_EP_SIZE 64
#define CANMV_USB_HID_EP_INTERVAL 10
#endif

#define CANMV_USB_HID_REPORT_ID_OUT 0x01
#define CANMV_USB_HID_REPORT_ID_IN  0x02

#define CANMV_USB_HID_REPORT_DESC_SIZE 40
#define CANMV_USB_HID_DESCRIPTOR_SIZE  (9 + 9 + 7 + 7)
#define CANMV_USB_HID_PAYLOAD_SIZE     (CANMV_USB_HID_EP_SIZE - 1)

#define CANMV_USB_HID_DESCRIPTOR_INIT(intf, in_ep, out_ep)                                    \
    0x09, USB_DESCRIPTOR_TYPE_INTERFACE, (intf), 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,         \
    0x09, HID_DESCRIPTOR_TYPE_HID, 0x11, 0x01, 0x00, 0x01, 0x22,                              \
    WBVAL(CANMV_USB_HID_REPORT_DESC_SIZE),                                                     \
    0x07, USB_DESCRIPTOR_TYPE_ENDPOINT, (in_ep), 0x03, WBVAL(CANMV_USB_HID_EP_SIZE),          \
    CANMV_USB_HID_EP_INTERVAL,                                                                 \
    0x07, USB_DESCRIPTOR_TYPE_ENDPOINT, (out_ep), 0x03, WBVAL(CANMV_USB_HID_EP_SIZE),         \
    CANMV_USB_HID_EP_INTERVAL

#if defined(CHERRY_USB_DEVICE_FUNC_HID)

#define CANMV_USB_HID_INTERFACE_COUNT 1
#define CANMV_USB_HID_INTF_NUM        0x00
#define CANMV_USB_HID_IN_EP           0x81
#define CANMV_USB_HID_OUT_EP          0x01

#elif defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP)

#include "usb_mtp.h"
#include "usbd_desc_cdc_common.h"

#define MTP_INTF_NUM CANMV_USB_CDC_NEXT_INTERFACE

#ifdef CONFIG_ENABLE_DUAL_CDC_PORT
#define MTP_IN_EP            0x85
#define MTP_OUT_EP           0x05
#define MTP_INT_EP           0x86
#define CANMV_USB_HID_IN_EP  0x87
#define CANMV_USB_HID_OUT_EP 0x07
#else
#define MTP_IN_EP            0x83
#define MTP_OUT_EP           0x03
#define MTP_INT_EP           0x84
#define CANMV_USB_HID_IN_EP  0x85
#define CANMV_USB_HID_OUT_EP 0x05
#endif

#define CANMV_USB_HID_INTF_NUM        (MTP_INTF_NUM + 1)
#define CANMV_USB_HID_INTERFACE_COUNT (CANMV_USB_CDC_INTERFACE_COUNT + 2)

#endif
