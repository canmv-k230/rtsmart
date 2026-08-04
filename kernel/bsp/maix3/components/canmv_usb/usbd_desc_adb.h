#pragma once

#include "usb_cdc.h"
#include "usbd_adb.h"

/*!< endpoint address */
#define ADB_IN_EP  0x81
#define ADB_OUT_EP 0x01

#define ADB_DESCRIPTOR_LEN (9 + 7 + 7)

#ifdef CONFIG_USB_HS
#define WINUSB_MAX_MPS 512
#else
#define WINUSB_MAX_MPS 64
#endif

#define ADB_INTF_NUM (0x0)

#define USB_CONFIG_SIZE (9 + ADB_DESCRIPTOR_LEN)

#define CANMV_USB_ADB_CONFIG_DESCRIPTOR_INIT(descriptor_type, max_packet_size)                    \
    CANMV_USB_CONFIG_DESCRIPTOR_INIT(descriptor_type, USB_CONFIG_SIZE, 0x01, 0x01,                \
                                     USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),                     \
    ADB_DESCRIPTOR_INIT(ADB_INTF_NUM, ADB_IN_EP, ADB_OUT_EP, max_packet_size)

/*!< global descriptor */
static const uint8_t canmv_usb_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, CHERRY_USB_DEVICE_VID, CHERRY_USB_DEVICE_PID, 0x0100, 0x01),
    CANMV_USB_ADB_CONFIG_DESCRIPTOR_INIT(USB_DESCRIPTOR_TYPE_CONFIGURATION, WINUSB_MAX_MPS),
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
    0x00,
    0x00,
    0x00,
    0x40,
    0x01,
    0x00,
    CANMV_USB_ADB_CONFIG_DESCRIPTOR_INIT(USB_DESCRIPTOR_TYPE_OTHER_SPEED, USB_DEVICE_FS_MAX_MPS),
#endif
    0x00,
};

#undef CANMV_USB_ADB_CONFIG_DESCRIPTOR_INIT
