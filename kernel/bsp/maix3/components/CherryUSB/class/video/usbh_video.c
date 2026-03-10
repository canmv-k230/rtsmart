/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"
#include "usbh_video.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_video"
#include "usb_log.h"

#define DEV_FORMAT "video%d"

/* general descriptor field offsets */
#define DESC_bLength              0 /** Length offset */
#define DESC_bDescriptorType      1 /** Descriptor type offset */
#define DESC_bDescriptorSubType   2 /** Descriptor subtype offset */
#define DESC_bNumFormats          3 /** Descriptor numformat offset */
#define DESC_bNumFrameDescriptors 4 /** Descriptor numframe offset */
#define DESC_bFormatIndex         3 /** Descriptor format index offset */
#define DESC_bFrameIndex          3 /** Descriptor frame index offset */

/* interface descriptor field offsets */
#define INTF_DESC_bInterfaceNumber  2 /** Interface number offset */
#define INTF_DESC_bAlternateSetting 3 /** Alternate setting offset */

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_video_buf[128];

static const char *format_type[] = { "uncompressed", "mjpeg" };

struct usbh_guid_fourcc_map {
    uint8_t guid[16];
    uint32_t fourcc;
};

static const struct usbh_guid_fourcc_map g_guid_to_fourcc[] = {
    { .guid = { VIDEO_GUID_YUY2 }, .fourcc = USBH_VIDEO_FOURCC_YUY2 },
    { .guid = { VIDEO_GUID_UYVY }, .fourcc = USBH_VIDEO_FOURCC_UYVY },
    { .guid = { VIDEO_GUID_NV12 }, .fourcc = USBH_VIDEO_FOURCC_NV12 },
    { .guid = { VIDEO_GUID_I420 }, .fourcc = USBH_VIDEO_FOURCC_I420 },
    { .guid = { VIDEO_GUID_MJPEG }, .fourcc = USBH_VIDEO_FOURCC_MJPEG },
};

static uint32_t usbh_video_guid_to_fourcc(const uint8_t guid[16])
{
    for (uint32_t i = 0; i < sizeof(g_guid_to_fourcc) / sizeof(g_guid_to_fourcc[0]); i++) {
        if (memcmp(guid, g_guid_to_fourcc[i].guid, sizeof(g_guid_to_fourcc[i].guid)) == 0) {
            return g_guid_to_fourcc[i].fourcc;
        }
    }

    return 0;
}

static void usbh_video_fourcc_to_str(uint32_t fourcc, char str[5])
{
    str[0] = (char)(fourcc & 0xFF);
    str[1] = (char)((fourcc >> 8) & 0xFF);
    str[2] = (char)((fourcc >> 16) & 0xFF);
    str[3] = (char)((fourcc >> 24) & 0xFF);
    str[4] = '\0';
}

static struct usbh_video g_video_class[CONFIG_USBHOST_MAX_VIDEO_CLASS];
static uint32_t g_devinuse = 0;

static struct usbh_video *usbh_video_class_alloc(void)
{
    int devno;

    for (devno = 0; devno < CONFIG_USBHOST_MAX_VIDEO_CLASS; devno++) {
        if ((g_devinuse & (1 << devno)) == 0) {
            g_devinuse |= (1 << devno);
            usb_memset(&g_video_class[devno], 0, sizeof(struct usbh_video));
            g_video_class[devno].minor = devno;
            return &g_video_class[devno];
        }
    }
    return NULL;
}

static void usbh_video_class_free(struct usbh_video *video_class)
{
    int devno = video_class->minor;

    if (devno >= 0 && devno < 32) {
        g_devinuse &= ~(1 << devno);
    }
    usb_memset(video_class, 0, sizeof(struct usbh_video));
}

int usbh_video_get(struct usbh_video *video_class, uint8_t request, uint8_t intf, uint8_t entity_id, uint8_t cs, uint8_t *buf, uint16_t len)
{
    struct usb_setup_packet *setup = video_class->hport->setup;
    int ret;
    uint8_t retry;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = request;
    setup->wValue = cs << 8;
    setup->wIndex = (entity_id << 8) | intf;
    setup->wLength = len;

    retry = 0;
    while (1) {
        ret = usbh_control_transfer(video_class->hport, setup, g_video_buf);
        if (ret > 0) {
            break;
        }
        retry++;

        if (retry == 3) {
            return ret;
        }
    }

    if (buf) {
        usb_memcpy(buf, g_video_buf, len);
    }

    return ret;
}

int usbh_video_set(struct usbh_video *video_class, uint8_t request, uint8_t intf, uint8_t entity_id, uint8_t cs, uint8_t *buf, uint16_t len)
{
    struct usb_setup_packet *setup = video_class->hport->setup;
    int ret;

    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = request;
    setup->wValue = cs << 8;
    setup->wIndex = (entity_id << 8) | intf;
    setup->wLength = len;

    usb_memcpy(g_video_buf, buf, len);

    ret = usbh_control_transfer(video_class->hport, setup, g_video_buf);
    usb_osal_msleep(50);
    return ret;
}

int usbh_videostreaming_get_cur_probe(struct usbh_video *video_class)
{
    return usbh_video_get(video_class, VIDEO_REQUEST_GET_CUR, video_class->data_intf, 0x00, VIDEO_VS_PROBE_CONTROL, (uint8_t *)&video_class->probe, 26);
}

int usbh_videostreaming_set_cur_probe(struct usbh_video *video_class, uint8_t formatindex, uint8_t frameindex)
{
    video_class->probe.bFormatIndex = formatindex;
    video_class->probe.bFrameIndex = frameindex;
    video_class->probe.dwMaxPayloadTransferSize = 0;
    video_class->probe.dwFrameInterval = video_class->current_frame.dwRequestFrameInterval;

    return usbh_video_set(video_class, VIDEO_REQUEST_SET_CUR, video_class->data_intf, 0x00, VIDEO_VS_PROBE_CONTROL, (uint8_t *)&video_class->probe, 26);
}

int usbh_videostreaming_set_cur_commit(struct usbh_video *video_class, uint8_t formatindex, uint8_t frameindex)
{
    usb_memcpy(&video_class->commit, &video_class->probe, sizeof(struct video_probe_and_commit_controls));
    video_class->commit.bFormatIndex = formatindex;
    video_class->commit.bFrameIndex = frameindex;
    video_class->commit.dwFrameInterval = video_class->current_frame.dwRequestFrameInterval;

    return usbh_video_set(video_class, VIDEO_REQUEST_SET_CUR, video_class->data_intf, 0x00, VIDEO_VS_COMMIT_CONTROL, (uint8_t *)&video_class->commit, 26);
}

int usbh_video_open(struct usbh_video *video_class,
                    uint32_t fourcc,
                    uint16_t wWidth,
                    uint16_t wHeight,
                    uint8_t altsetting)
{
    struct usb_setup_packet *setup = video_class->hport->setup;
    struct usb_endpoint_descriptor *ep_desc;
    uint8_t mult;
    uint16_t mps;
    int ret;
    bool found = false;
    uint8_t formatidx = 0;
    uint8_t frameidx = 0;
    uint8_t step;

    if (video_class->is_opened) {
        return 0;
    }

    for (uint8_t i = 0; i < video_class->num_of_formats; i++) {
        if ((fourcc == 0) || (fourcc == video_class->format[i].fourcc)) {
            formatidx = i + 1;
            for (uint8_t j = 0; j < video_class->format[i].num_of_frames; j++) {
                if ((wWidth == video_class->format[i].frame[j].wWidth) &&
                    (wHeight == video_class->format[i].frame[j].wHeight)) {
                    frameidx = j + 1;
                    found = true;
                    goto found;
                }
            }
        }
    }

found:
    if (found == false) {
        return -USB_ERR_NODEV;
    }

    if (altsetting != 0xFF && altsetting > (video_class->num_of_intf_altsettings - 1)) {
        return -USB_ERR_INVAL;
    }

    /* Open video step:
     * Get CUR request (probe)
     * Set CUR request (probe)
     * Get CUR request (probe)
     * Get MAX request (probe)
     * Get MIN request (probe)
     * Get CUR request (probe)
     * Set CUR request (commit)
     *    
    */
    step = 0;
    ret = usbh_videostreaming_get_cur_probe(video_class);
    if (ret < 0) {
        goto errout;
    }

    step = 1;
    ret = usbh_videostreaming_set_cur_probe(video_class, formatidx, frameidx);
    if (ret < 0) {
        goto errout;
    }

    step = 2;
    ret = usbh_videostreaming_get_cur_probe(video_class);
    if (ret < 0) {
        goto errout;
    }

    step = 3;
    ret = usbh_video_get(video_class, VIDEO_REQUEST_GET_MAX, video_class->data_intf, 0x00, VIDEO_VS_PROBE_CONTROL, NULL, 26);
    if (ret < 0) {
        goto errout;
    }

    step = 4;
    ret = usbh_video_get(video_class, VIDEO_REQUEST_GET_MIN, video_class->data_intf, 0x00, VIDEO_VS_PROBE_CONTROL, NULL, 26);
    if (ret < 0) {
        goto errout;
    }

    step = 5;
    ret = usbh_videostreaming_set_cur_probe(video_class, formatidx, frameidx);
    if (ret < 0) {
        goto errout;
    }

    step = 6;
    ret = usbh_videostreaming_get_cur_probe(video_class);
    if (ret < 0) {
        goto errout;
    }

    step = 7;
    ret = usbh_videostreaming_set_cur_commit(video_class, formatidx, frameidx);
    if (ret < 0) {
        goto errout;
    }

    if (altsetting == 0xFF)
    {
        unsigned int bandwidth, psize, i;

        if (video_class->num_of_intf_altsettings > 1) {
            /* Isochronous endpoint, select the alternate setting. */
            bandwidth = video_class->commit.dwMaxPayloadTransferSize;

            if (bandwidth == 0) {
                USB_LOG_RAW("requested null bandwidth, defaulting to lowest.\n");
                bandwidth = 1;
            }

            for (i = 0; i < video_class->num_of_intf_altsettings; ++i) {
                ep_desc = &video_class->hport->config.intf[video_class->data_intf].altsetting[i].ep[0].ep_desc;

                /* Check if the bandwidth is high enough. */
                psize = ep_desc->wMaxPacketSize;
                psize = (psize & 0x07ff) * (1 + ((psize >> 11) & 3));
                if (psize >= bandwidth)
                    break;
            }

            if (i >= video_class->num_of_intf_altsettings) {
                USB_LOG_RAW("Can't find suitable altsetting\n");
                return -USB_ERR_RANGE;
            }

            altsetting = i;
        } else {
            altsetting = 0;
            ep_desc = &video_class->hport->config.intf[video_class->data_intf].altsetting[altsetting].ep[0].ep_desc;
            USB_LOG_RAW("uvc which use bulk ep\n");

            if (ep_desc->bEndpointAddress & 0x80) {
                USBH_EP_INIT(video_class->bulkin, ep_desc);
            } else {
                USBH_EP_INIT(video_class->bulkout, ep_desc);
            }

            goto out;
        }
    }

    step = 8;
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = USB_REQUEST_SET_INTERFACE;
    setup->wValue = altsetting;
    setup->wIndex = video_class->data_intf;
    setup->wLength = 0;

    ret = usbh_control_transfer(video_class->hport, setup, NULL);
    if (ret < 0) {
        goto errout;
    }

    ep_desc = &video_class->hport->config.intf[video_class->data_intf].altsetting[altsetting].ep[0].ep_desc;
    mult = (ep_desc->wMaxPacketSize & USB_MAXPACKETSIZE_ADDITIONAL_TRANSCATION_MASK) >> USB_MAXPACKETSIZE_ADDITIONAL_TRANSCATION_SHIFT;
    mps = ep_desc->wMaxPacketSize & USB_MAXPACKETSIZE_MASK;
    if (ep_desc->bEndpointAddress & 0x80) {
        video_class->isoin_mps = mps * (mult + 1);
        USBH_EP_INIT(video_class->isoin, ep_desc);
    } else {
        video_class->isoout_mps = mps * (mult + 1);
        USBH_EP_INIT(video_class->isoout, ep_desc);
    }

    USB_LOG_INFO("Open video and select formatidx:%u, frameidx:%u, altsetting:%u\r\n", formatidx, frameidx, altsetting);
    USB_LOG_INFO("mps = %d, mult = %d, video_class->isoin_mps = %d, video_class->isoout_mps = %d\r\n",
                 mps, mult, video_class->isoin_mps, video_class->isoout_mps);
out:
    video_class->is_opened = true;
    video_class->current_format_type = video_class->format[formatidx - 1].format_type;
    video_class->current_fourcc = video_class->format[formatidx - 1].fourcc;
    return ret;

errout:
    video_class->is_opened = false;
    USB_LOG_ERR("Fail to open video in step %u\r\n", step);
    return ret;
}

int usbh_video_close(struct usbh_video *video_class)
{
    struct usb_setup_packet *setup = video_class->hport->setup;
    int ret = 0;

    USB_LOG_INFO("Close video device\r\n");

    video_class->is_opened = false;

    if (video_class->isoin) {
        video_class->isoin = NULL;
    }

    if (video_class->isoout) {
        video_class->isoout = NULL;
    }

    setup->bmRequestType = USB_REQUEST_DIR_OUT |
        USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = USB_REQUEST_SET_INTERFACE;
    setup->wValue = 0;
    setup->wIndex = video_class->data_intf;
    setup->wLength = 0;

    ret = usbh_control_transfer(video_class->hport, setup, NULL);
    if (ret < 0) {
        return ret;
    }
    return ret;
}

void usbh_video_list_info(struct usbh_video *video_class)
{
    struct usb_endpoint_descriptor *ep_desc;
    uint8_t mult;
    uint16_t mps;
    char fourcc_str[5];

    USB_LOG_INFO("============= Video device information ===================\r\n");
    USB_LOG_RAW("bcdVDC:%04x\r\n", video_class->bcdVDC);
    USB_LOG_RAW("Num of altsettings:%u\r\n", video_class->num_of_intf_altsettings);

    for (uint8_t i = 0; i < video_class->num_of_intf_altsettings; i++) {
        if (i == 0) {
            USB_LOG_RAW("Ingore altsetting 0\r\n");
            continue;
        }
        ep_desc = &video_class->hport->config.intf[video_class->data_intf].altsetting[i].ep[0].ep_desc;

        mult = (ep_desc->wMaxPacketSize & USB_MAXPACKETSIZE_ADDITIONAL_TRANSCATION_MASK) >> USB_MAXPACKETSIZE_ADDITIONAL_TRANSCATION_SHIFT;
        mps = ep_desc->wMaxPacketSize & USB_MAXPACKETSIZE_MASK;

        USB_LOG_RAW("Altsetting:%u, Ep=%02x Attr=%02u Mps=%d Interval=%02u Mult=%02u\r\n",
                     i,
                     ep_desc->bEndpointAddress,
                     ep_desc->bmAttributes,
                     mps,
                     ep_desc->bInterval,
                     mult);
    }

    USB_LOG_RAW("bNumFormats:%u\r\n", video_class->num_of_formats);
    for (uint8_t i = 0; i < video_class->num_of_formats; i++) {
        const char *type_name = (video_class->format[i].format_type < 2)
            ? format_type[video_class->format[i].format_type]
            : "unknown";
        usbh_video_fourcc_to_str(video_class->format[i].fourcc, fourcc_str);
        USB_LOG_RAW("  FormatIndex:%u\r\n", i + 1);
        USB_LOG_RAW("  FormatType:%s\r\n", type_name);
        USB_LOG_RAW("  FourCC:%s (0x%08x)\r\n", fourcc_str, video_class->format[i].fourcc);
        USB_LOG_RAW("  bNumFrames:%u\r\n", video_class->format[i].num_of_frames);
        USB_LOG_RAW("  Resolution:\r\n");
        for (uint8_t j = 0; j < video_class->format[i].num_of_frames; j++) {
            USB_LOG_RAW("      FrameIndex:%u\r\n", j + 1);
            USB_LOG_RAW("      wWidth: %4d, wHeight: %4d, dwDefaultFrameInterval: %d\r\n",
                         video_class->format[i].frame[j].wWidth,
                         video_class->format[i].frame[j].wHeight,
                         video_class->format[i].frame[j].dwDefaultFrameInterval);
        }
    }

    USB_LOG_INFO("============= Video device information ===================\r\n");
}

static int usbh_video_ctrl_connect(struct usbh_hubport *hport, uint8_t intf)
{
    int ret;
    uint8_t cur_iface = 0xff;
    // uint8_t cur_alt_setting = 0xff;
    uint8_t frame_index = 0xff;
    uint8_t format_index = 0xff;
    uint8_t num_of_frames = 0xff;
    uint8_t num_of_interval = 0xff;
    uint8_t *p;

    struct usbh_video *video_class = usbh_video_class_alloc();
    if (video_class == NULL) {
        USB_LOG_ERR("Fail to alloc video_class\r\n");
        return -USB_ERR_NOMEM;
    }

    video_class->hport = hport;
    video_class->ctrl_intf = intf;
    video_class->data_intf = intf + 1;
    video_class->num_of_intf_altsettings = hport->config.intf[intf + 1].altsetting_num;

    hport->config.intf[intf].priv = video_class;

    ret = usbh_video_close(video_class);
    if (ret < 0) {
        USB_LOG_ERR("Fail to close video device\r\n");
        return ret;
    }

    p = hport->raw_config_desc;
    while (p[DESC_bLength]) {
        switch (p[DESC_bDescriptorType]) {
            case USB_DESCRIPTOR_TYPE_INTERFACE:
                cur_iface = p[INTF_DESC_bInterfaceNumber];
                //cur_alt_setting = p[INTF_DESC_bAlternateSetting];
                break;
            case USB_DESCRIPTOR_TYPE_ENDPOINT:
                //ep_desc = (struct usb_endpoint_descriptor *)p;
                break;
            case VIDEO_CS_INTERFACE_DESCRIPTOR_TYPE:
                if (cur_iface == video_class->ctrl_intf) {
                    switch (p[DESC_bDescriptorSubType]) {
                        case VIDEO_VC_HEADER_DESCRIPTOR_SUBTYPE:
                            video_class->bcdVDC = ((uint16_t)p[4] << 8) | (uint16_t)p[3];
                            break;
                        case VIDEO_VC_INPUT_TERMINAL_DESCRIPTOR_SUBTYPE:
                            break;
                        case VIDEO_VC_OUTPUT_TERMINAL_DESCRIPTOR_SUBTYPE:
                            break;
                        case VIDEO_VC_PROCESSING_UNIT_DESCRIPTOR_SUBTYPE:
                            break;

                        default:
                            break;
                    }
                } else if (cur_iface == video_class->data_intf) {
                    switch (p[DESC_bDescriptorSubType]) {
                        case VIDEO_VS_INPUT_HEADER_DESCRIPTOR_SUBTYPE:
                            video_class->num_of_formats = p[DESC_bNumFormats];
                            if (video_class->num_of_formats > MAX_FORMAT_NUM) {
                                USB_LOG_WRN("Too many formats %u, truncate to %u\r\n",
                                            video_class->num_of_formats, MAX_FORMAT_NUM);
                                video_class->num_of_formats = MAX_FORMAT_NUM;
                            }
                            break;
                        case VIDEO_VS_FORMAT_UNCOMPRESSED_DESCRIPTOR_SUBTYPE:
                            format_index = p[DESC_bFormatIndex];
                            num_of_frames = p[DESC_bNumFrameDescriptors];
                            if ((format_index == 0) || (format_index > video_class->num_of_formats)) {
                                USB_LOG_WRN("Ignore out-of-range format_index=%u\r\n", format_index);
                                break;
                            }

                            video_class->format[format_index - 1].num_of_frames = num_of_frames;
                            video_class->format[format_index - 1].format_type = USBH_VIDEO_FORMAT_UNCOMPRESSED;
                            memcpy(video_class->format[format_index - 1].guidFormat, ((struct video_cs_if_vs_format_uncompressed_descriptor *)p)->guidFormat, 16);
                            video_class->format[format_index - 1].fourcc =
                                usbh_video_guid_to_fourcc(video_class->format[format_index - 1].guidFormat);
                            if (video_class->format[format_index - 1].fourcc == 0) {
                                video_class->format[format_index - 1].fourcc = USBH_VIDEO_FOURCC(
                                    video_class->format[format_index - 1].guidFormat[0],
                                    video_class->format[format_index - 1].guidFormat[1],
                                    video_class->format[format_index - 1].guidFormat[2],
                                    video_class->format[format_index - 1].guidFormat[3]);
                            }
                            break;
                        case VIDEO_VS_FORMAT_MJPEG_DESCRIPTOR_SUBTYPE:
                            format_index = p[DESC_bFormatIndex];
                            num_of_frames = p[DESC_bNumFrameDescriptors];
                            if ((format_index == 0) || (format_index > video_class->num_of_formats)) {
                                USB_LOG_WRN("Ignore out-of-range format_index=%u\r\n", format_index);
                                break;
                            }

                            video_class->format[format_index - 1].num_of_frames = num_of_frames;
                            video_class->format[format_index - 1].format_type = USBH_VIDEO_FORMAT_MJPEG;
                            memcpy(video_class->format[format_index - 1].guidFormat,
                                   (uint8_t[16]) { VIDEO_GUID_MJPEG }, 16);
                            video_class->format[format_index - 1].fourcc = USBH_VIDEO_FOURCC_MJPEG;
                            break;
                        case VIDEO_VS_FRAME_UNCOMPRESSED_DESCRIPTOR_SUBTYPE:
                            frame_index = p[DESC_bFrameIndex];
                            if ((format_index == 0) || (format_index > video_class->num_of_formats) ||
                                (frame_index == 0) || (frame_index > MAX_FRAME_NUM)) {
                                USB_LOG_WRN("Ignore frame desc format=%u frame=%u\r\n",
                                            format_index, frame_index);
                                break;
                            }

                            video_class->format[format_index - 1].frame[frame_index - 1].wWidth = ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->wWidth;
                            video_class->format[format_index - 1].frame[frame_index - 1].wHeight = ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->wHeight;
                            video_class->format[format_index - 1].frame[frame_index - 1].dwDefaultFrameInterval = ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->dwDefaultFrameInterval;
                            video_class->format[format_index - 1].frame[frame_index - 1].dwMaxVideoFrameBufferSize = ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->dwMaxVideoFrameBufferSize;
                            num_of_interval = video_class->format[format_index - 1].frame[frame_index - 1].bFrameIntervalType = ((struct video_cs_if_vs_frame_uncompressed_descriptor *)p)->bFrameIntervalType;
                            for (int i = 0; i < num_of_interval && i < MAX_INTERVAL_NUM; i ++) {
                                video_class->format[format_index - 1].frame[frame_index - 1].dwFrameInterval[i] = *(uint32_t *)(p + 26 + 4*i);
                            }
                            break;
                        case VIDEO_VS_FRAME_MJPEG_DESCRIPTOR_SUBTYPE:
                            frame_index = p[DESC_bFrameIndex];
                            if ((format_index == 0) || (format_index > video_class->num_of_formats) ||
                                (frame_index == 0) || (frame_index > MAX_FRAME_NUM)) {
                                USB_LOG_WRN("Ignore frame desc format=%u frame=%u\r\n",
                                            format_index, frame_index);
                                break;
                            }

                            video_class->format[format_index - 1].frame[frame_index - 1].wWidth = ((struct video_cs_if_vs_frame_mjpeg_descriptor *)p)->wWidth;
                            video_class->format[format_index - 1].frame[frame_index - 1].wHeight = ((struct video_cs_if_vs_frame_mjpeg_descriptor *)p)->wHeight;
                            video_class->format[format_index - 1].frame[frame_index - 1].dwDefaultFrameInterval = ((struct video_cs_if_vs_frame_mjpeg_descriptor *)p)->dwDefaultFrameInterval;
                            video_class->format[format_index - 1].frame[frame_index - 1].dwMaxVideoFrameBufferSize = ((struct video_cs_if_vs_frame_mjpeg_descriptor *)p)->dwMaxVideoFrameBufferSize;
                            num_of_interval = video_class->format[format_index - 1].frame[frame_index - 1].bFrameIntervalType = ((struct video_cs_if_vs_frame_mjpeg_descriptor *)p)->bFrameIntervalType;
                            for (int i = 0; i < num_of_interval && i < MAX_INTERVAL_NUM; i ++) {
                                video_class->format[format_index - 1].frame[frame_index - 1].dwFrameInterval[i] = *(uint32_t *)(p + 26 + 4*i);
                            }
                            break;
                        default:
                            break;
                    }
                }

                break;

            default:
                break;
        }
        /* skip to next descriptor */
        p += p[DESC_bLength];
    }

    usbh_video_list_info(video_class);

    snprintf(hport->config.intf[intf].devname, CONFIG_USBHOST_DEV_NAMELEN, DEV_FORMAT, video_class->minor);

    USB_LOG_INFO("Register Video Class:%s\r\n", hport->config.intf[intf].devname);

    usbh_video_run(video_class);
    return ret;
}

static int usbh_video_ctrl_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    int ret = 0;

    struct usbh_video *video_class = (struct usbh_video *)hport->config.intf[intf].priv;

    if (video_class) {
        if (video_class->isoin) {
        }

        if (video_class->isoout) {
        }

        if (hport->config.intf[intf].devname[0] != '\0') {
            USB_LOG_INFO("Unregister Video Class:%s\r\n", hport->config.intf[intf].devname);
            usbh_video_stop(video_class);
        }

        usbh_video_class_free(video_class);
    }

    return ret;
}

static int usbh_video_streaming_connect(struct usbh_hubport *hport, uint8_t intf)
{
    USB_LOG_INFO("%s %d intf = %d\n", __func__, __LINE__, intf);

    return 0;
}

static int usbh_video_streaming_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    USB_LOG_INFO("%s %d\n", __func__, __LINE__);

    return 0;
}

__WEAK void usbh_video_run(struct usbh_video *video_class)
{
}

__WEAK void usbh_video_stop(struct usbh_video *video_class)
{
}

const struct usbh_class_driver video_ctrl_class_driver = {
    .driver_name = "video_ctrl",
    .connect = usbh_video_ctrl_connect,
    .disconnect = usbh_video_ctrl_disconnect
};

const struct usbh_class_driver video_streaming_class_driver = {
    .driver_name = "video_streaming",
    .connect = usbh_video_streaming_connect,
    .disconnect = usbh_video_streaming_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info video_ctrl_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
    .class = USB_DEVICE_CLASS_VIDEO,
    .subclass = VIDEO_SC_VIDEOCONTROL,
    .protocol = VIDEO_PC_PROTOCOL_UNDEFINED,
    .vid = 0x00,
    .pid = 0x00,
    .class_driver = &video_ctrl_class_driver
};

CLASS_INFO_DEFINE const struct usbh_class_info video_streaming_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
    .class = USB_DEVICE_CLASS_VIDEO,
    .subclass = VIDEO_SC_VIDEOSTREAMING,
    .protocol = VIDEO_PC_PROTOCOL_UNDEFINED,
    .vid = 0x00,
    .pid = 0x00,
    .class_driver = &video_streaming_class_driver
};
