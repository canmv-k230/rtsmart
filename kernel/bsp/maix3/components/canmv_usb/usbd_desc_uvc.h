#pragma once

#include "usbd_video.h"

#define VIDEO_IN_EP 0x81

#define MAX_PAYLOAD_SIZE  1024 // for high speed with one transcations every one micro frame
#define VIDEO_PACKET_SIZE (unsigned int)(((MAX_PAYLOAD_SIZE / 1)) | (0x00 << 11))

// #define MAX_PAYLOAD_SIZE  2048 // for high speed with two transcations every one micro frame
// #define VIDEO_PACKET_SIZE (unsigned int)(((MAX_PAYLOAD_SIZE / 2)) | (0x01 << 11))

// #define MAX_PAYLOAD_SIZE  3072 // for high speed with three transcations every one micro frame
// #define VIDEO_PACKET_SIZE (unsigned int)(((MAX_PAYLOAD_SIZE / 3)) | (0x02 << 11))

#define CAM_WIDTH  (unsigned int)(USB_DEVICE_UVC_IMG_WIDTH)
#define CAM_HEIGHT (unsigned int)(USB_DEVICE_UVC_IMG_HEIGHT)
#define CAM_FPS    (USB_DEVICE_UVC_IMG_FPS)

#define INTERVAL       (unsigned long)(10000000 / CAM_FPS)
#define MIN_BIT_RATE   (unsigned long)(CAM_WIDTH * CAM_HEIGHT * 16 * CAM_FPS) // 16 bit
#define MAX_BIT_RATE   (unsigned long)(CAM_WIDTH * CAM_HEIGHT * 16 * CAM_FPS)
#define MAX_FRAME_SIZE (unsigned long)(CAM_WIDTH * CAM_HEIGHT * 2)

#define USB_VIDEO_DESC_SIZ (unsigned long)(9 + 8 + 9 + 13 + 18 + 9 + 12 + 9 + 14 + 11 + 30 + 9 + 7 + 6 - 1)

#define VC_TERMINAL_SIZ (unsigned int)(13 + 18 + 12 + 9 - 1)
#define VS_HEADER_SIZ   (unsigned int)(13 + 1 + 11 + 30 + 6)

/*!< global descriptor */
static const uint8_t canmv_usb_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xef, 0x02, 0x01, CHERRY_USB_DEVICE_VID, CHERRY_USB_DEVICE_PID, 0x0001, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_VIDEO_DESC_SIZ, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    VIDEO_VC_DESCRIPTOR_INIT(0x00, 0, 0x0100, VC_TERMINAL_SIZ, 48000000, 0x02),
    VIDEO_VS_DESCRIPTOR_INIT(0x01, 0x00, 0x00),
    VIDEO_VS_HEADER_DESCRIPTOR_INIT(0x01, VS_HEADER_SIZ, VIDEO_IN_EP, 0x00),
    VIDEO_VS_FORMAT_MJPEG_DESCRIPTOR_INIT(0x01, 0x01),
    VIDEO_VS_FRAME_MJPEG_DESCRIPTOR_INIT(0x01, CAM_WIDTH, CAM_HEIGHT, MIN_BIT_RATE, MAX_BIT_RATE, MAX_FRAME_SIZE,
                                         DBVAL(INTERVAL), 0x01, DBVAL(INTERVAL)),
    /* Color Matching Descriptor */
    0x06,        // bLength
    0x24,        // CS_INTERFACE
    0x0D,        // VS_COLORFORMAT subtype
    0x01,        // bColorPrimaries      (1 = BT.709, 2 = sRGB)
    0x01,        // bTransferCharacteristics
    0x04,         // bMatrixCoefficients
    VIDEO_VS_DESCRIPTOR_INIT(0x01, 0x01, 0x01),
    /* 1.2.2.2 Standard VideoStream Isochronous Video Data Endpoint Descriptor */
    0x07, /* bLength */
    USB_DESCRIPTOR_TYPE_ENDPOINT, /* bDescriptorType: ENDPOINT */
    0x81, /* bEndpointAddress: IN endpoint 2 */
    0x01, /* bmAttributes: Isochronous transfer type. Asynchronous synchronization type. */
    WBVAL(VIDEO_PACKET_SIZE), /* wMaxPacketSize */
    0x01, /* bInterval: One frame interval */

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
