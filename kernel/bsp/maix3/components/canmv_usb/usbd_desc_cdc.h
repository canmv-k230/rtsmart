#pragma once

#include "usb_cdc.h"

/*!< endpoint address */
#define CDC_IN_EP  0x81
#define CDC_OUT_EP 0x01
#define CDC_INT_EP 0x82

#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

/*!< global descriptor */
static const uint8_t canmv_usb_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, CHERRY_USB_DEVICE_VID, CHERRY_USB_DEVICE_PID, 0x0100, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, USB_DEVICE_MAX_MPS, 0x02),
    ///////////////////////////////////////
    /// string0 descriptor
    ///////////////////////////////////////
    USB_LANGID_INIT(USBD_LANGID_STRING),
    ///////////////////////////////////////
    /// string1 descriptor
    ///////////////////////////////////////
    0x12, /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    'K',
    0x00, /* wcChar0 */
    'e',
    0x00, /* wcChar1 */
    'n',
    0x00, /* wcChar2 */
    'd',
    0x00, /* wcChar3 */
    'r',
    0x00, /* wcChar4 */
    'y',
    0x00, /* wcChar5 */
    't',
    0x00, /* wcChar6 */
    'e',
    0x00, /* wcChar7 */
    ///////////////////////////////////////
    /// string2 descriptor
    ///////////////////////////////////////
    0x0C, /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    'C',
    0x00, /* wcChar0 */
    'a',
    0x00, /* wcChar1 */
    'n',
    0x00, /* wcChar2 */
    'M',
    0x00, /* wcChar3 */
    'V',
    0x00, /* wcChar4 */
    ///////////////////////////////////////
    /// string3 descriptor
    ///////////////////////////////////////
    0x14, /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    '0',
    0x00, /* wcChar0 */
    '0',
    0x00, /* wcChar1 */
    '1',
    0x00, /* wcChar2 */
    '0',
    0x00, /* wcChar3 */
    '0',
    0x00, /* wcChar4 */
    '0',
    0x00, /* wcChar5 */
    '0',
    0x00, /* wcChar6 */
    '0',
    0x00, /* wcChar7 */
    '0',
    0x00, /* wcChar8 */
#ifdef CONFIG_USB_HS
    ///////////////////////////////////////
    /// device qualifier descriptor
    ///////////////////////////////////////
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0xEF,
    0x02,
    0x01,
    0x40,
    0x00,
    0x00,
#endif
    0x00,
};
