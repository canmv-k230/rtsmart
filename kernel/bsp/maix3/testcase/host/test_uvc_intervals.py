#!/usr/bin/env python3
"""Run production UVC descriptor parsing and interval ioctls under ASan/UBSan.

The connect function and ioctl cases are extracted unchanged; unrelated USB
transfers and kernel user copies are stubbed. Optional source paths reproduce
regressions against a saved version. No USB controller or camera is emulated.
The regex extractor requires the return type, function name and opening '(' on
one line; parameters may span lines but cannot contain semicolons. Its brace
scanner assumes braces in comments and strings do not affect function nesting.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

BSP = Path(__file__).resolve().parents[2]
VIDEO = BSP / "components/CherryUSB/class/video"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--video-source", type=Path, default=VIDEO / "usbh_video.c")
parser.add_argument("--uvc-source", type=Path, default=BSP / "components/canmv_usb/usbh_uvc_thr.c")
parser.add_argument("--case", default="all")
args = parser.parse_args()
video = args.video_source.read_text()
uvc = args.uvc_source.read_text()
header = (VIDEO / "usbh_video.h").read_text()


def function(source, name, optional=False):
    match = re.search(r"^[^\n;{}]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if match is None:
        if optional:
            return ""
        raise AssertionError(f"function {name} missing")
    start = source.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def structure(name):
    return re.search(r"^struct " + name + r" \{.*?^};", header, re.M | re.S)[0] + "\n"


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __PACKED __attribute__((packed))
#include "usb_video.h"
#define MAX_FORMAT_NUM 8
#define MAX_FRAME_NUM 30
#define MAX_INTERVAL_NUM 10
#define USBH_VIDEO_FORMAT_UNCOMPRESSED 0
#define USBH_VIDEO_FORMAT_MJPEG 1
#define USBH_VIDEO_FOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#define USBH_VIDEO_FOURCC_YUY2 USBH_VIDEO_FOURCC('Y','U','Y','2')
#define USBH_VIDEO_FOURCC_MJPEG USBH_VIDEO_FOURCC('M','J','P','G')
#define USB_LOG_ERR(...) ((void)0)
static unsigned warning_count;
#define USB_LOG_WRN(...) ((void)++warning_count)
#define USB_LOG_DBG(...) ((void)0)
#define USB_ERR_NOMEM 1
#define USB_DESCRIPTOR_TYPE_INTERFACE 4
#define USB_DESCRIPTOR_TYPE_ENDPOINT 5
#define CONFIG_USBHOST_DEV_NAMELEN 32
#define DEV_FORMAT "video%d"
#define RT_EOK 0
#define RT_NULL NULL
#define min(a,b) ((a)<(b)?(a):(b))
#define VIDIOC_ENUM_INTERVAL 1
#define VIDIOC_S_FMT 2
#define VIDIOC_ENUM_FRAME 3
#define DESC_bLength 0
#define DESC_bDescriptorType 1
#define DESC_bDescriptorSubType 2
#define DESC_bNumFormats 3
#define DESC_bNumFrameDescriptors 4
#define DESC_bFormatIndex 3
#define DESC_bFrameIndex 3
#define INTF_DESC_bInterfaceNumber 2
#define INTF_DESC_bAlternateSetting 3
struct rt_device { int unused; };
struct usbh_hubport {
    struct {
        struct { uint16_t wTotalLength; } config_desc;
        struct { unsigned altsetting_num; char devname[32]; void *priv; } intf[2];
    } config;
    uint8_t *raw_config_desc;
};
'''
STUBS = r'''
static struct usbh_video device;
static struct usbh_video *usbh_video_class_alloc(void) { memset(&device,0,sizeof(device)); return &device; }
static int usbh_video_close(struct usbh_video *v) { (void)v; return 0; }
static void usbh_video_run(struct usbh_video *v) { (void)v; }
static uint32_t usbh_video_guid_to_fourcc(const uint8_t *g) { (void)g; return USBH_VIDEO_FOURCC_YUY2; }
static int userspace;
static void *lwp_self(void) { return userspace ? &userspace : NULL; }
static void lwp_get_from_user(void *d, const void *s, size_t n) { memcpy(d,s,n); }
static void lwp_put_to_user(void *d, const void *s, size_t n) { memcpy(d,s,n); }
'''
TESTS = r'''
static void put32(uint8_t *p, uint32_t v) {
    for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
static void setup_frame(void) {
    memset(&device,0,sizeof(device));
    device.num_of_formats=1;
    device.format[0].fourcc=USBH_VIDEO_FOURCC_MJPEG;
    device.format[0].num_of_frames=1;
    struct usbh_video_frame *f=&device.format[0].frame[0];
    f->wWidth=640; f->wHeight=480;
    f->dwDefaultFrameInterval=400000;
    f->dwFrameInterval[0]=200000;
    f->dwFrameInterval[1]=1000000;
    f->dwFrameInterval[2]=100000;
}
static void continuous_enum(void) {
    setup_frame();
    struct uvc_fpsdesc d={.fourcc=USBH_VIDEO_FOURCC_MJPEG,.width=640,.height=480};
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==0);
    assert(d.frameinterval==400000);
    d.index=1;
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==-EINVAL);
    d.index=UINT32_MAX;
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==-EINVAL);
}
static void continuous_select(void) {
    setup_frame();
    uint32_t requests[]={200000,500000,1000000,550000,100000,1100000,0,UINT32_MAX};
    uint32_t expected[]={200000,500000,1000000,400000,400000,400000,400000,400000};
    for (unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);i++) {
        struct uvc_format f={640,480,USBH_VIDEO_FOURCC_MJPEG,requests[i]};
        assert(control(&device,VIDIOC_S_FMT,&f)==0);
        assert(f.frameinterval==expected[i]);
        assert(device.current_frame.dwRequestFrameInterval==expected[i]);
    }
    device.format[0].frame[0].dwFrameInterval[2]=0;
    struct uvc_format f={640,480,USBH_VIDEO_FOURCC_MJPEG,555555};
    assert(control(&device,VIDIOC_S_FMT,&f)==0 && f.frameinterval==555555);
}
static void missing_frame(void) {
    setup_frame();
    struct usbh_video_format *f=&device.format[0];
    f->num_of_frames=3;
    f->frame[2]=f->frame[0];
    f->frame[2].wWidth=1280;
    struct uvc_framedesc d={.fourcc=f->fourcc};
    assert(control(&device,VIDIOC_ENUM_FRAME,&d)==0 && d.width==640);
    d.index=1;
    assert(control(&device,VIDIOC_ENUM_FRAME,&d)==0 && d.width==1280);
    d.index=2;
    assert(control(&device,VIDIOC_ENUM_FRAME,&d)==-EINVAL);
    struct uvc_format fmt={1,1,f->fourcc,0};
    assert(control(&device,VIDIOC_S_FMT,&fmt)==0 && fmt.width==640 && fmt.height==480);
    memset(f->frame,0,sizeof(f->frame));
    d.index=0;
    assert(control(&device,VIDIOC_ENUM_FRAME,&d)==-EINVAL);
    assert(control(&device,VIDIOC_S_FMT,&fmt)==-EINVAL);
}
static void discrete(void) {
    setup_frame();
    struct usbh_video_frame *f=&device.format[0].frame[0];
    f->bFrameIntervalType=2;
    f->dwFrameInterval[0]=333333; f->dwFrameInterval[1]=666666;
    f->dwDefaultFrameInterval=333333;
    struct uvc_fpsdesc d={.fourcc=USBH_VIDEO_FOURCC_MJPEG,.width=640,.height=480};
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==0 && d.frameinterval==333333);
    d.index=1;
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==0 && d.frameinterval==666666);
    d.index=2;
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==-EINVAL);
    struct uvc_format fmt={640,480,USBH_VIDEO_FOURCC_MJPEG,666666};
    assert(control(&device,VIDIOC_S_FMT,&fmt)==0 && fmt.frameinterval==666666);
    f->bFrameIntervalType=255;
    d.index=MAX_INTERVAL_NUM;
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==-EINVAL);
}
static void descriptors(void) {
    /* Offset by one deliberately: USB descriptors need not be aligned. */
    uint8_t data[256]={0}, *p=data+1;
    struct usbh_hubport hp={0};
    hp.raw_config_desc=p;
    const uint8_t prefix[]={9,4,1,0,0,14,2,0,0, 4,36,1,1, 11,36,6,1,255,0,0,0,0,0,0};
    memcpy(p,prefix,sizeof(prefix));
    uint8_t *frame=p+sizeof(prefix);
    frame[0]=38; frame[1]=36; frame[2]=7; frame[3]=1;
    frame[5]=128; frame[6]=2; frame[7]=224; frame[8]=1;
    put32(frame+17,640*480*2); put32(frame+21,500000);
    put32(frame+26,200000); put32(frame+30,1000000); put32(frame+34,100000);
    hp.config.config_desc.wTotalLength=sizeof(prefix)+38;
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    struct usbh_video_frame *f=&device.format[0].frame[0];
    assert(f->wWidth==640 && f->wHeight==480);
    assert(f->bFrameIntervalType==0);
    assert(f->dwFrameInterval[0]==200000 && f->dwFrameInterval[1]==1000000 && f->dwFrameInterval[2]==100000);
    assert(device.format[0].num_of_frames==MAX_FRAME_NUM);
    /* More discrete entries than fit: count and storage must agree. */
    frame[0]=26+12*4; frame[25]=12;
    for (int i=0;i<12;i++) put32(frame+26+4*i,300000+100000*i);
    hp.config.config_desc.wTotalLength=sizeof(prefix)+frame[0];
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    f=&device.format[0].frame[0];
    assert(f->bFrameIntervalType==MAX_INTERVAL_NUM);
    assert(f->dwFrameInterval[9]==1200000);
    /* A default past the stored entries is the camera's own: keep it and
     * accept it without falling back. */
    put32(frame+21,1400000);
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    f=&device.format[0].frame[0];
    assert(f->dwDefaultFrameInterval==1400000);
    struct uvc_format fmt={640,480,USBH_VIDEO_FOURCC_MJPEG,1400000};
    assert(control(&device,VIDIOC_S_FMT,&fmt)==0 && fmt.frameinterval==1400000);
    /* Only a missing default is filled in. */
    put32(frame+21,0);
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    assert(device.format[0].frame[0].dwDefaultFrameInterval==300000);
    put32(frame+21,500000);
    /* Truncated discrete list and missing continuous data: default is finite. */
    frame[0]=30; frame[25]=12;
    hp.config.config_desc.wTotalLength=sizeof(prefix)+30;
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    assert(device.format[0].frame[0].bFrameIntervalType==1);
    frame[0]=26; frame[25]=0;
    hp.config.config_desc.wTotalLength=sizeof(prefix)+26;
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    assert(device.format[0].frame[0].bFrameIntervalType==1);
    assert(device.format[0].frame[0].dwFrameInterval[0]==500000);
    /* The uncompressed descriptor uses the same frame offsets. */
    p[sizeof(prefix)-11+2]=4; p[sizeof(prefix)-11]=27;
    memmove(frame+16,frame,26); frame+=16; frame[2]=5;
    hp.config.config_desc.wTotalLength=sizeof(prefix)+16+26;
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    assert(device.format[0].frame[0].wWidth==640);
}
static void select_index(uint8_t want_format, uint8_t want_frame) {
    uint8_t fi=0, fr=0;
    assert(usbh_video_select_frame(&device,USBH_VIDEO_FOURCC_MJPEG,640,480,&fi,&fr));
    assert(fi==want_format && fr==want_frame);
}
static void enum_intervals(const uint32_t *expected, unsigned count) {
    struct uvc_fpsdesc d={.fourcc=USBH_VIDEO_FOURCC_MJPEG,.width=640,.height=480};
    for (unsigned i=0;i<count;i++) {
        d.index=i;
        assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==0 && d.frameinterval==expected[i]);
    }
    d.index=count;
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&d)==-EINVAL);
}
static void set_format(uint32_t interval, uint32_t expected, uint8_t frame_index) {
    struct uvc_format f={640,480,USBH_VIDEO_FOURCC_MJPEG,interval};
    assert(control(&device,VIDIOC_S_FMT,&f)==0 && f.frameinterval==expected);
    assert(device.current_frame.bFormatIndex==1 && device.current_frame.bFrameIndex==frame_index);
    select_index(1,frame_index);
}
static void invalid_continuous(void) {
    uint8_t data[38]={0};
    data[0]=sizeof(data);
    data[5]=128; data[6]=2; data[7]=224; data[8]=1;
    put32(data+21,1);
    put32(data+26,1000000); put32(data+30,200000); put32(data+34,100000);
    setup_frame();
    struct usbh_video_frame *frame=&device.format[0].frame[0];
    usbh_video_parse_frame(frame,data);
    assert(!usbh_video_frame_usable(frame));
    struct uvc_framedesc fd={.fourcc=USBH_VIDEO_FOURCC_MJPEG};
    struct uvc_fpsdesc fps={.fourcc=USBH_VIDEO_FOURCC_MJPEG,.width=640,.height=480};
    struct uvc_format fmt={640,480,USBH_VIDEO_FOURCC_MJPEG,1};
    assert(control(&device,VIDIOC_ENUM_FRAME,&fd)==-EINVAL);
    assert(control(&device,VIDIOC_ENUM_INTERVAL,&fps)==-EINVAL);
    assert(control(&device,VIDIOC_S_FMT,&fmt)==-EINVAL);
    uint8_t fi=0, fr=0;
    assert(!usbh_video_select_frame(&device,fmt.fourcc,640,480,&fi,&fr));
    /* A complete range with a zero minimum is also malformed. */
    put32(data+26,0); put32(data+30,1000000);
    usbh_video_parse_frame(frame,data);
    assert(!usbh_video_frame_usable(frame));
    /* An equal min/max is a valid single-rate continuous descriptor. */
    put32(data+26,200000); put32(data+30,200000);
    usbh_video_parse_frame(frame,data);
    assert(usbh_video_frame_usable(frame) && frame->dwDefaultFrameInterval==200000);
    /* A valid descriptor of the same size survives an invalid earlier one. */
    device.format[0].num_of_frames=2;
    device.format[0].frame[1]=*frame;
    put32(data+26,1000000);
    usbh_video_parse_frame(frame,data);
    enum_intervals((const uint32_t[]){200000},1);
    set_format(1,200000,2);
}
static void empty_intervals(void) {
    setup_frame();
    struct usbh_video_frame *frame=&device.format[0].frame[0];
    frame->bFrameIntervalType=2;
    memset(frame->dwFrameInterval,0,sizeof(frame->dwFrameInterval));
    assert(uvc_frame_supports_interval(frame,400000));
    assert(!uvc_frame_supports_interval(frame,0));
    assert(!uvc_frame_supports_interval(frame,666666));
    set_format(0,400000,1);
    set_format(666666,400000,1);
    device.current_frame.bFormatIndex=device.current_frame.bFrameIndex=0;
    select_index(1,1);  /* fallback can use the nonzero default safely */
    device.format[0].num_of_frames=2;
    device.format[0].frame[1]=*frame;
    device.format[0].frame[1].bFrameIntervalType=1;
    device.format[0].frame[1].dwFrameInterval[0]=666666;
    device.format[0].frame[1].dwDefaultFrameInterval=666666;
    set_format(666666,666666,2);
    /* The parser repairs an empty discrete payload into a default-only list. */
    uint8_t data[34]={0};
    data[0]=sizeof(data); data[25]=2;
    data[5]=128; data[6]=2; data[7]=224; data[8]=1;
    put32(data+21,400000);
    usbh_video_parse_frame(frame,data);
    assert(frame->bFrameIntervalType==1 && frame->dwFrameInterval[0]==400000);
    put32(data+21,0);
    usbh_video_parse_frame(frame,data);
    assert(!usbh_video_frame_usable(frame));
}
static void malformed_length(void) {
    uint8_t data[11]={9,4,1,0,0,14,2,0,0,0,36};
    struct usbh_hubport hp={0};
    hp.raw_config_desc=data;
    hp.config.config_desc.wTotalLength=sizeof(data);
    const uint8_t lengths[]={0,1,3};
    for (unsigned i=0;i<sizeof(lengths);i++) {
        data[9]=lengths[i];
        unsigned before=warning_count;
        assert(usbh_video_ctrl_connect(&hp,0)==0);
        assert(warning_count==before+1);
    }
    /* Even a final byte without a descriptor type must be diagnosed. */
    hp.config.config_desc.wTotalLength=10;
    unsigned before=warning_count;
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    assert(warning_count==before+1);
    hp.config.config_desc.wTotalLength=9;
    before=warning_count;
    assert(usbh_video_ctrl_connect(&hp,0)==0);
    assert(warning_count==before);
}
static void duplicate_size(void) {
    /* Two descriptors of one size with different rates: the size is listed
     * once, its intervals are the union, and S_FMT and open use the
     * descriptor that supports the requested interval. */
    setup_frame();
    struct usbh_video_format *fmt=&device.format[0];
    fmt->num_of_frames=2;
    fmt->frame[0].bFrameIntervalType=1;
    fmt->frame[0].dwFrameInterval[0]=fmt->frame[0].dwDefaultFrameInterval=333333;
    fmt->frame[1]=fmt->frame[0];
    fmt->frame[1].dwFrameInterval[0]=fmt->frame[1].dwDefaultFrameInterval=666666;
    struct uvc_framedesc fd={.fourcc=USBH_VIDEO_FOURCC_MJPEG};
    assert(control(&device,VIDIOC_ENUM_FRAME,&fd)==0 && fd.width==640 && fd.defaultframeinterval==333333);
    fd.index=1;
    assert(control(&device,VIDIOC_ENUM_FRAME,&fd)==-EINVAL);
    enum_intervals((const uint32_t[]){333333,666666},2);
    set_format(333333,333333,1);
    set_format(666666,666666,2);
    set_format(0,333333,1);
    set_format(500000,333333,1);
    /* The stored descriptor is probed, not the first size match. */
    set_format(666666,666666,2);
    select_index(1,2);
    /* An unusable first slot of the same size is skipped everywhere. */
    fmt->frame[0].dwDefaultFrameInterval=0;
    memset(fmt->frame[0].dwFrameInterval,0,sizeof(fmt->frame[0].dwFrameInterval));
    fd.index=0;
    assert(control(&device,VIDIOC_ENUM_FRAME,&fd)==0 && fd.defaultframeinterval==666666);
    enum_intervals((const uint32_t[]){666666},1);
    set_format(0,666666,2);
    /* A stored index that no longer matches falls back to searching. */
    fmt->frame[0].dwFrameInterval[0]=fmt->frame[0].dwDefaultFrameInterval=333333;
    device.current_frame.bFrameIndex=MAX_FRAME_NUM;
    select_index(1,1);
    device.current_frame.bFormatIndex=0;
    fmt->frame[0].wWidth=0;
    select_index(1,2);
    uint8_t fi=0, fr=0;
    assert(!usbh_video_select_frame(&device,USBH_VIDEO_FOURCC_MJPEG,320,240,&fi,&fr));
    /* The layout of a real HDMI capture camera: the same list twice with
     * different defaults lists each interval once. */
    setup_frame();
    fmt->num_of_frames=2;
    fmt->frame[0].bFrameIntervalType=3;
    fmt->frame[0].dwFrameInterval[0]=166666;
    fmt->frame[0].dwFrameInterval[1]=333333;
    fmt->frame[0].dwFrameInterval[2]=666666;
    fmt->frame[0].dwDefaultFrameInterval=333333;
    fmt->frame[1]=fmt->frame[0];
    fmt->frame[1].dwDefaultFrameInterval=166666;
    enum_intervals((const uint32_t[]){166666,333333,666666},3);
    set_format(166666,166666,1);
    /* A continuous descriptor of the same size lists its default once. */
    fmt->frame[1].bFrameIntervalType=0;
    fmt->frame[1].dwFrameInterval[0]=100000;
    fmt->frame[1].dwFrameInterval[1]=2000000;
    fmt->frame[1].dwFrameInterval[2]=100000;
    fmt->frame[1].dwDefaultFrameInterval=400000;
    enum_intervals((const uint32_t[]){166666,333333,666666,400000},4);
    set_format(1500000,1500000,2);
}
int main(int argc, char **argv) {
    const char *name=argc>1?argv[1]:"all";
    for (userspace=0;userspace<=1;userspace++) {
        if (!strcmp(name,"all") || !strcmp(name,"continuous_enum")) continuous_enum();
        if (!strcmp(name,"all") || !strcmp(name,"continuous_select")) continuous_select();
        if (!strcmp(name,"all") || !strcmp(name,"discrete")) discrete();
        if (!strcmp(name,"all") || !strcmp(name,"missing_frame")) missing_frame();
        if (!strcmp(name,"all") || !strcmp(name,"duplicate_size")) duplicate_size();
        if (!strcmp(name,"all") || !strcmp(name,"descriptors")) descriptors();
        if (!strcmp(name,"all") || !strcmp(name,"invalid_continuous")) invalid_continuous();
        if (!strcmp(name,"all") || !strcmp(name,"empty_intervals")) empty_intervals();
        if (!strcmp(name,"all") || !strcmp(name,"malformed_length")) malformed_length();
    }
    puts("PASS: UVC intervals (kernel and user ioctl arguments)");
    return 0;
}
'''
structs = "".join(structure(n) for n in ["usbh_video_frame", "usbh_video_format", "request_frame", "usbh_video", "uvc_format", "uvc_fpsdesc", "uvc_framedesc"])
helpers = function(header, "usbh_video_frame_usable") + "".join(function(video,n,True) for n in ["usbh_video_get_le32", "usbh_video_parse_frame", "usbh_video_select_frame"])
cases = uvc[uvc.index("    case VIDIOC_ENUM_FRAME: {"):uvc.index("    case VIDIOC_REQBUFS: {")]
control = "static int control(struct usbh_video *video, int cmd, void *args) { int ret=0; switch(cmd) {\n" + cases + "} out: return ret; }\n"
source = PRELUDE + structs + STUBS + helpers + function(video,"usbh_video_ctrl_connect") + function(uvc,"uvc_find_format") + "".join(function(uvc,n,True) for n in ["uvc_frame_has_size", "uvc_find_frame", "uvc_frame_interval_count", "uvc_frame_interval", "uvc_interval_listed_before", "uvc_frame_supports_interval"]) + control + TESTS
with tempfile.TemporaryDirectory(prefix="uvc-intervals-") as tmpdir:
    tmp=Path(tmpdir)
    (tmp/"test.c").write_text(source)
    subprocess.run([os.environ.get("CC","cc"),"-std=gnu11","-g","-O1","-Wall","-Wextra","-Wno-sign-compare","-fsanitize=address,undefined","-fno-omit-frame-pointer","-fno-pie","-no-pie","-I",str(VIDEO),str(tmp/"test.c"),"-o",str(tmp/"test")],check=True)
    subprocess.run([str(tmp/"test"),args.case],check=True,env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1","UBSAN_OPTIONS":"halt_on_error=1"})
