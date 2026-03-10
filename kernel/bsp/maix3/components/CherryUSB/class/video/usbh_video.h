/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USBH_VIDEO_H
#define USBH_VIDEO_H

#include "usb_video.h"

#define USBH_VIDEO_FORMAT_UNCOMPRESSED 0
#define USBH_VIDEO_FORMAT_MJPEG        1

#define USBH_VIDEO_FOURCC(a, b, c, d)                                           \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) |                   \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

#define USBH_VIDEO_FOURCC_YUY2  USBH_VIDEO_FOURCC('Y', 'U', 'Y', '2')
#define USBH_VIDEO_FOURCC_UYVY  USBH_VIDEO_FOURCC('U', 'Y', 'V', 'Y')
#define USBH_VIDEO_FOURCC_NV12  USBH_VIDEO_FOURCC('N', 'V', '1', '2')
#define USBH_VIDEO_FOURCC_I420  USBH_VIDEO_FOURCC('I', '4', '2', '0')
#define USBH_VIDEO_FOURCC_MJPEG USBH_VIDEO_FOURCC('M', 'J', 'P', 'G')

#define MAX_FORMAT_NUM (8)
#define MAX_FRAME_NUM (30)
#define MAX_INTERVAL_NUM (10)

struct usbh_video_frame {
    uint16_t wWidth;
    uint16_t wHeight;
    uint32_t dwDefaultFrameInterval;
    uint8_t  bFrameIntervalType;
    uint32_t dwFrameInterval[MAX_INTERVAL_NUM];
    uint32_t dwMaxVideoFrameBufferSize;
};

struct usbh_video_format {
    struct usbh_video_frame frame[MAX_FRAME_NUM];
    uint8_t format_type;
    uint8_t num_of_frames;
    uint8_t guidFormat[16];
    uint32_t fourcc;
};

struct usbh_videoframe {
    uint8_t *frame_buf;
    uint32_t frame_bufsize;
    uint32_t frame_format;
    uint32_t frame_size;
};

struct usbh_videostreaming {
    struct usbh_videoframe *frame;
    uint32_t frame_format;
    uint32_t bufoffset;
    uint16_t width;
    uint16_t height;
};

struct request_frame {
    uint16_t wWidth;
    uint16_t wHeight;
    uint32_t dwRequestFrameInterval;
    uint32_t dwMaxVideoFrameBufferSize;
};

struct usbh_video {
    struct rt_device device;
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *isoin;  /* ISO IN endpoint */
    struct usb_endpoint_descriptor *isoout; /* ISO OUT endpoint */
    struct usb_endpoint_descriptor *bulkin;  /* BULK IN endpoint */
    struct usb_endpoint_descriptor *bulkout;  /* BULK IN endpoint */

    struct {
        uint8_t header[256];
        unsigned int header_size;
        int skip_payload;
        uint32_t payload_size;
        uint32_t max_payload_size;
    } bulk;

    uint8_t ctrl_intf; /* interface number */
    uint8_t data_intf; /* interface number */
    uint8_t minor;
    struct video_probe_and_commit_controls probe;
    struct video_probe_and_commit_controls commit;
    uint16_t isoin_mps;
    uint16_t isoout_mps;
    bool is_opened;
    uint8_t current_format_type;
    uint32_t current_fourcc;
    struct request_frame current_frame;
    uint16_t bcdVDC;
    uint8_t num_of_intf_altsettings;
    uint8_t num_of_formats;
    struct usbh_video_format format[MAX_FORMAT_NUM];
};

#define VB_VERSION (1)
#define MAX_USB_STRING_LEN (64)

#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align)-1))
#define VDEC_ALIGN_SIZE (0x1000)

#include "comm/k_video_comm.h"
#include "comm/k_vb_comm.h"
#include "comm/k_vdec_comm.h"


struct uvc_fmtdesc {
    uint32_t index;
    uint32_t fourcc;
    uint8_t description[32];
};

struct uvc_format {
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t frameinterval;
};

struct uvc_requestbuffers {
    uint32_t count;
};

struct uvc_frame {
    uint32_t index;
    uint32_t length;
    uint32_t offset;
    uint32_t bytesused;
    char *userptr;
    union {
        k_video_frame_info v_info;
        k_vdec_stream v_stream;
    };
};

struct uvc_framedesc {
    uint32_t index;
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint32_t defaultframeinterval;
};

struct uvc_fpsdesc {
    uint32_t index;
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint32_t frameinterval;
};

struct usb_string {
    uint8_t index;
    char str[MAX_USB_STRING_LEN];
};

struct usb_index {
    uint8_t iManufacturer;      /* Index to manufacturer string */
    uint8_t iProduct;           /* Index to product string */
};

#define VIDIOC_ENUM_FMT         _IOWR('V', 1, struct uvc_fmtdesc)
#define VIDIOC_S_FMT            _IOWR('V', 2, struct uvc_format)
#define VIDIOC_REQBUFS          _IOWR('V', 3, struct uvc_requestbuffers)
#define VIDIOC_QUERYBUF         _IOWR('V',  4, struct uvc_frame)
#define VIDIOC_QBUF             _IOWR('V', 5, struct uvc_frame)
#define VIDIOC_DQBUF            _IOWR('V', 6, struct uvc_frame)
#define VIDIOC_BUFMMAP          _IOW('V', 7, struct dfs_mmap2_args)
#define VIDIOC_STREAMON         _IOW('V', 8, int)
#define VIDIOC_STREAMOFF        _IOW('V', 9, int)
#define VIDIOC_ENUM_FRAME       _IOWR('V', 10, struct uvc_framedesc)
#define VIDIOC_ENUM_INTERVAL    _IOWR('V', 11, struct uvc_fpsdesc)
#define VIDIOC_GET_STRING       _IOWR('V', 12, struct usb_string)
#define VIDIOC_GET_INDEX        _IOWR('V', 13, struct usb_index)

enum videobuf_state {
    VIDEOBUF_NEEDS_INIT = 0,
    VIDEOBUF_PREPARED   = 1,
    VIDEOBUF_QUEUED     = 2,
    VIDEOBUF_ACTIVE     = 3,
    VIDEOBUF_DONE       = 4,
    VIDEOBUF_ERROR      = 5,
    VIDEOBUF_IDLE       = 6,
};

#ifdef __cplusplus
extern "C" {
#endif

int usbh_video_get(struct usbh_video *video_class, uint8_t request, uint8_t intf, uint8_t entity_id, uint8_t cs, uint8_t *buf, uint16_t len);
int usbh_video_set(struct usbh_video *video_class, uint8_t request, uint8_t intf, uint8_t entity_id, uint8_t cs, uint8_t *buf, uint16_t len);

int usbh_video_open(struct usbh_video *video_class,
                    uint32_t fourcc,
                    uint16_t wWidth,
                    uint16_t wHeight,
                    uint8_t altsetting);
int usbh_video_close(struct usbh_video *video_class);

void usbh_video_list_info(struct usbh_video *video_class);

void usbh_video_run(struct usbh_video *video_class);
void usbh_video_stop(struct usbh_video *video_class);

#ifdef __cplusplus
}
#endif

#endif /* USBH_VIDEO_H */
