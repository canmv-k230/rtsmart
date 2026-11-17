#!/usr/bin/env python3
"""Exercise the UVC host glue (canmv_usb/usbh_uvc_thr.c) against a scripted camera.

usbh_uvc_thr.c is compiled unchanged with the real usbh_video.h, CherryUSB
error codes and RT-Thread headers; USB transfers, VB pools, dfs and lwp are
stubs. Covers the CherryUSB/RT-Thread to POSIX errno mapping, errno returned
by ioctls, a camera unplugged while streaming (the open fd turns dead and stays
dead after re-enumeration, poll wakes, the stream is stopped once under the I/O
lock, the cleared device slot is never read), bounded isochronous
packet counts, and selecting between two attached cameras.
Run with python3 -B kernel/bsp/maix3/testcase/host/test_uvc_host.py
[path/to/usbh_uvc_thr.c].
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

BSP = Path(__file__).resolve().parents[2]
KERNEL = BSP.parents[1]
RTT = KERNEL / "rt-thread"
USB = BSP / "components/CherryUSB"
CANMV_USB = BSP / "components/canmv_usb"
MPP = KERNEL.parents[1] / "mpp"

RTCONFIG = r"""
#pragma once
#define RT_NAME_MAX 20
#define RT_DEBUG
#define RT_ALIGN_SIZE 8
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 1000
#define RT_USING_DEVICE
#define RT_USING_DEVICE_OPS
#define RT_USING_POSIX
#define RT_USING_DFS
#define RT_USING_DFS_DEVFS
#define RT_USING_LIBC
#define RT_USING_CONSOLE
#define RT_USING_SYSTEM_WORKQUEUE
#define RT_USING_MUTEX
#define RT_USING_SEMAPHORE
#define RT_USING_HEAP
#define RT_USING_MUSL
#define ARCH_CPU_64BIT
"""

HEADERS = {
    "usbh_core.h": r"""
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <rtthread.h>
#include <rthw.h>
#include <ipc/workqueue.h>
#define CONFIG_USB_ALIGN_SIZE 64
#define CONFIG_USBHOST_MAX_VIDEO_CLASS 2
#define CONFIG_USBHOST_DEV_NAMELEN 16
#define CONFIG_USB_DBG_LEVEL USB_DBG_WARNING
#define CONFIG_USB_PRINTF(...) rt_kprintf(__VA_ARGS__)
#include "usb_util.h"
#include "usb_errno.h"
#include "usb_def.h"
#include "usb_log.h"
typedef void (*usbh_complete_callback_t)(void *arg, int nbytes);
struct usbh_iso_frame_packet {
    uint8_t *transfer_buffer;
    uint32_t transfer_buffer_length;
    uint32_t actual_length;
    int errorcode;
    uint32_t offset;
    uint32_t length;
};
struct usbh_interface { char devname[CONFIG_USBHOST_DEV_NAMELEN]; };
struct usbh_hubport {
    uint8_t speed;
    struct usb_device_descriptor device_desc;
    struct { struct usbh_interface intf[2]; } config;
    struct usb_setup_packet *setup;
};
struct usbh_urb {
    void *hcpriv;
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *ep;
    void *context;
    uint16_t interval;
    struct usb_setup_packet *setup;
    uint8_t *transfer_buffer;
    uint32_t transfer_buffer_length;
    uint32_t actual_length;
    uint32_t timeout;
    int errorcode;
    uint32_t num_of_iso_packets;
    usbh_complete_callback_t complete;
    void *arg;
    struct usbh_iso_frame_packet iso_packet[0];
};
int usbh_submit_urb(struct usbh_urb *urb);
int usbh_kill_urb(struct usbh_urb *urb);
int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup, uint8_t *buffer);
int usbh_get_string_desc(struct usbh_hubport *hport, uint8_t index, uint8_t *output, uint16_t output_len);
""",
    "dfs_poll.h": r"""
#pragma once
#include <rtthread.h>
#include <ipc/waitqueue.h>
#include <poll.h>
struct rt_pollreq { int key; };
void rt_poll_add(rt_wqueue_t *wq, struct rt_pollreq *req);
""",
    "dfs_posix.h": r"""
#pragma once
#include <fcntl.h>
struct dirent;
struct rt_pollreq;
struct dfs_fnode { void *data; };
struct dfs_fd { uint32_t flags; struct dfs_fnode *fnode; void *data; };
struct dfs_file_ops {
    int (*open)(struct dfs_fd *fd);
    int (*close)(struct dfs_fd *fd);
    int (*ioctl)(struct dfs_fd *fd, int cmd, void *args);
    int (*read)(struct dfs_fd *fd, void *buf, size_t count);
    int (*write)(struct dfs_fd *fd, const void *buf, size_t count);
    int (*flush)(struct dfs_fd *fd);
    int (*lseek)(struct dfs_fd *fd, off_t offset);
    int (*getdents)(struct dfs_fd *fd, struct dirent *dirp, uint32_t count);
    int (*poll)(struct dfs_fd *fd, struct rt_pollreq *req);
};
struct dfs_mmap2_args { void *addr; size_t length; int prot; int flags; off_t pgoffset; void *ret; };
""",
    "lwp.h": "#pragma once\nvoid *lwp_self(void);\n",
    "lwp_user_mm.h": r"""
#pragma once
#include <stddef.h>
size_t lwp_get_from_user(void *dst, void *src, size_t size);
size_t lwp_put_to_user(void *dst, void *src, size_t size);
void *lwp_map_user_phy(void *lwp, void *map_va, void *map_pa, size_t map_size, int cached);
int lwp_unmap_user_phy(void *lwp, void *va);
""",
    "tick.h": r"""
#pragma once
#include <stdint.h>
uint64_t cpu_ticks_ms(void);
uint64_t cpu_ticks_us(void);
uint64_t cpu_ticks_ns(void);
""",
    "rvv_ops.h": "#pragma once\n#include <string.h>\n#define rvv_memcpy memcpy\n",
    "cache.h": r"""
#pragma once
typedef struct { int unused; } rt_mmu_info;
void rt_hw_cpu_dcache_clean(void *addr, int size);
void *rt_hw_mmu_v2p(rt_mmu_info *mmu_info, void *v_addr);
""",
}

MODEL = r"""
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/asan_interface.h>
#include "usbh_core.h"
#include "usbh_video.h"
#include "dfs_poll.h"
#include "dfs_posix.h"
#include "cache.h"
#include "vb_ext.h"


rt_mmu_info mmu_info;

/* ---- RT-Thread ---- */
static int irq_off, mutex_held;
static int other_holds;   /* another thread holds io_mutex (a long fops call) */
static int other_holds_long; /* ...longer than any timed wait, but it ends */
static char klog[8192];

rt_base_t rt_hw_interrupt_disable(void) { return irq_off++; }
void rt_hw_interrupt_enable(rt_base_t level) { irq_off = (int)level; }
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag) { mutex->hold = 0; return RT_EOK; }
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time)
{
    assert(!irq_off);
    if (other_holds) {
        if (time == 0)
            return -RT_ETIMEOUT;
        assert(other_holds_long);   /* would block behind the holder */
        if (time != RT_WAITING_FOREVER)
            return -RT_ETIMEOUT;
        other_holds = other_holds_long = 0;     /* the holder finished */
    }
    mutex->hold++;
    mutex_held++;
    return RT_EOK;
}
rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
    assert(mutex->hold > 0);
    mutex->hold--;
    mutex_held--;
    return RT_EOK;
}
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms) { return ms; }
void rt_kprintf(const char *fmt, ...)
{
    va_list ap;
    size_t len = strlen(klog);

    va_start(ap, fmt);
    vsnprintf(klog + len, sizeof(klog) - len, fmt, ap);
    va_end(ap);
}
void rt_assert_handler(const char *ex, const char *func, rt_size_t line)
{
    fprintf(stderr, "RT_ASSERT(%s) %s:%lu\n", ex, func, (unsigned long)line);
    abort();
}
void *rt_memset(void *s, int c, rt_ubase_t count) { return memset(s, c, count); }
void *rt_memcpy(void *dst, const void *src, rt_ubase_t count) { return memcpy(dst, src, count); }
void *rt_calloc(rt_size_t count, rt_size_t size) { assert(!irq_off); return calloc(count, size); }
void rt_free(void *ptr) { free(ptr); }
void *rt_malloc_align(rt_size_t size, rt_size_t align) { assert(!irq_off); return aligned_alloc(align, size); }
void rt_free_align(void *ptr) { free(ptr); }

static rt_wqueue_t *polled_wq;
static int poll_wakeups;
void rt_poll_add(rt_wqueue_t *wq, struct rt_pollreq *req) { polled_wq = wq; }
void rt_wqueue_wakeup(rt_wqueue_t *queue, void *key)
{
    /* Wakes only the first waiter: not enough on unplug. */
}
void rt_wqueue_wakeup_all(rt_wqueue_t *queue, void *key)
{
    if (queue == polled_wq)
        poll_wakeups++;
}

static struct rt_device *registered;
static struct rt_device *registry[2];
rt_err_t rt_device_register(rt_device_t dev, const char *name, rt_uint16_t flags)
{
    unsigned int index = name[strlen(name) - 1] - '0';
    assert(index < 2 && registry[index] == RT_NULL);
    registry[index] = dev;
    registered = dev;
    return RT_EOK;
}
rt_err_t rt_device_unregister(rt_device_t dev)
{
    assert(registry[0] == dev || registry[1] == dev);
    for (int i = 0; i < 2; i++)
        if (registry[i] == dev) registry[i] = RT_NULL;
    registered = registry[0] ? registry[0] : registry[1];
    return RT_EOK;
}
rt_err_t rt_device_control(rt_device_t dev, int cmd, void *arg)
{
    assert(dev == registry[0] || dev == registry[1]);
    return dev->ops->control(dev, cmd, arg);
}

void rt_hw_cpu_dcache_clean(void *addr, int size) {}
void *rt_hw_mmu_v2p(rt_mmu_info *info, void *v_addr) { return v_addr; }
uint64_t cpu_ticks_ms(void) { return 1; }
uint64_t cpu_ticks_us(void) { return 1; }
uint64_t cpu_ticks_ns(void) { return 1; }

/* Kernel callers: lwp_self() == NULL, args are kernel pointers. */
void *lwp_self(void) { return NULL; }
size_t lwp_get_from_user(void *dst, void *src, size_t size) { memcpy(dst, src, size); return size; }
size_t lwp_put_to_user(void *dst, void *src, size_t size) { memcpy(dst, src, size); return size; }
void *lwp_map_user_phy(void *lwp, void *va, void *pa, size_t size, int cached) { return pa; }
int lwp_unmap_user_phy(void *lwp, void *va) { return 0; }

/* ---- system workqueue: run by hand ---- */
static struct rt_work *pending_work;
void rt_work_init(struct rt_work *work, void (*fn)(struct rt_work *work, void *data), void *data)
{
    work->work_func = fn;
    work->work_data = data;
}
rt_err_t rt_work_submit(struct rt_work *work, rt_tick_t ticks)
{
    assert(ticks > 0);
    pending_work = work;
    return RT_EOK;
}
static int run_work(void)
{
    struct rt_work *work = pending_work;

    if (work == NULL)
        return 0;
    pending_work = NULL;
    assert(!mutex_held);
    work->work_func(work, work->work_data);
    return 1;
}

/* ---- VB: referenced blocks keep their pool busy until released ---- */
#define BLKS 40
#define POOLS 8
static void *blk[BLKS];
static int blk_refs[BLKS];
static k_u32 blk_pool[BLKS];
static k_bool pool_live[POOLS];
static int pools;   /* pools whose memory is still allocated */
k_s32 vb_create_pool(k_u32 *pool_id, k_u32 blk_cnt, k_u64 blk_size, const char *mmz_name,
                     const char *buf_name, k_vb_remap_mode mode)
{
    for (k_u32 id = 0; id < POOLS; id++) {
        if (!pool_live[id]) {
            pool_live[id] = K_TRUE;
            pools++;
            *pool_id = id;
            return 0;
        }
    }
    return K_FAILED;
}
static void pool_free_if_idle(k_u32 pool_id)
{
    for (int i = 0; i < BLKS; i++)
        if (blk[i] && blk_pool[i] == pool_id)
            return;
    pool_live[pool_id] = K_FALSE;
    pools--;
}
k_s32 vb_destroy_pool(k_u32 pool_id)
{
    assert(mutex_held && !irq_off);
    if (pool_id >= POOLS || !pool_live[pool_id])
        return K_ERR_VB_UNEXIST;
    for (int i = 0; i < BLKS; i++) {
        if (blk[i] && blk_pool[i] == pool_id)
            return K_ERR_VB_BUSY;
    }
    pool_free_if_idle(pool_id);
    return 0;
}
k_vb_blk_handle vb_get_blk_by_size_and_pool_id(k_u32 pool_id, k_u64 blk_size, k_u32 uid)
{
    for (int i = 0; i < BLKS; i++) {
        if (blk[i] == NULL) {
            blk[i] = aligned_alloc(0x1000, (blk_size + 0xfff) & ~0xfffULL);
            blk_refs[i] = 1;
            blk_pool[i] = pool_id;
            return i;
        }
    }
    return VB_INVALID_HANDLE;
}
static void blk_unref(int i)
{
    if (--blk_refs[i] == 0) {
        free(blk[i]);
        blk[i] = NULL;
    }
}
k_u64 vb_blk_handle_to_phys(k_vb_blk_handle handle) { return (k_u64)(uintptr_t)blk[handle]; }
void *vb_blk_handle_to_kern(k_vb_blk_handle handle) { return blk[handle]; }
k_u32 vb_blk_handle_to_pool_id(k_vb_blk_handle handle) { return blk_pool[handle]; }
k_s32 vb_inquire_user_cnt(k_vb_blk_handle handle) { return 1; }
k_s32 vb_user_sub(k_u32 pool_id, k_u64 phys_addr, k_u32 uid)
{
    for (int i = 0; i < BLKS; i++) {
        if (blk[i] && (k_u64)(uintptr_t)blk[i] == phys_addr) {
            blk_unref(i);
            return K_SUCCESS;
        }
    }
    assert(0);
    return -1;
}
k_s32 vb_put_blk(k_u32 pool_id, k_u64 phys_addr) { return 0; }
/* ---- CherryUSB (usbh_video.c and the host controller) ---- */
static struct usb_endpoint_descriptor iso_ep = {
    .bLength = 7, .bEndpointAddress = 0x81, .bmAttributes = 0x05,
    .wMaxPacketSize = 0x1400, .bInterval = 1,   /* 1024 x 3 */
};
static int open_ret, submit_ret, submits, kills, closes, submitted;
static struct usbh_urb *last_urb;

int usbh_video_open(struct usbh_video *video, uint32_t fourcc, uint16_t w, uint16_t h, uint8_t alt)
{
    assert(mutex_held);
    if (open_ret < 0)
        return open_ret;
    video->is_opened = true;
    video->current_altsetting = 1;
    video->isoin = &iso_ep;
    video->isoin_mps = 1024 * 3;
    return 0;
}
int usbh_video_close(struct usbh_video *video)
{
    /* Stopping a stream must be serialized with the other stop paths. */
    assert(mutex_held);
    video->is_opened = false;
    closes++;
    return 0;
}
int usbh_submit_urb(struct usbh_urb *urb)
{
    if (submit_ret < 0 && submits == 2)
        return submit_ret;
    submits++;
    submitted++;
    last_urb = urb;
    return 0;
}
int usbh_kill_urb(struct usbh_urb *urb)
{
    assert(mutex_held);
    kills++;
    return 0;
}
int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup, uint8_t *buffer) { return 0; }
int usbh_get_string_desc(struct usbh_hubport *hport, uint8_t index, uint8_t *output, uint16_t len) { return -1; }

/* ---- scenario ---- */
static struct usbh_video video;   /* the g_video_class slot */
static struct usbh_hubport hport;
static struct usb_setup_packet setup;
static struct dfs_fnode fnode;

/* usbh_video_ctrl_connect(): a fresh slot, then usbh_video_run(). */
static void connect(void)
{
    memset(&video, 0, sizeof(video));
    hport.setup = &setup;
    hport.speed = USB_SPEED_HIGH;
    strcpy(hport.config.intf[0].devname, "video0");
    video.hport = &hport;
    video.ctrl_intf = 0;
    video.data_intf = 1;
    video.num_of_intf_altsettings = 2;
    video.num_of_formats = 1;
    video.format[0].fourcc = USBH_VIDEO_FOURCC_MJPEG;
    video.format[0].format_type = USBH_VIDEO_FORMAT_MJPEG;
    video.format[0].num_of_frames = 1;
    video.format[0].frame[0].wWidth = 640;
    video.format[0].frame[0].wHeight = 480;
    video.format[0].frame[0].dwDefaultFrameInterval = 333333;
    video.format[0].frame[0].bFrameIntervalType = 1;
    video.format[0].frame[0].dwFrameInterval[0] = 333333;
    video.format[0].frame[0].dwMaxVideoFrameBufferSize = 64 * 1024;
    video.isoin = &iso_ep;
    usbh_video_run(&video);
    assert(registered == &video.device);
    fnode.data = registered;
}

/* usbh_video_ctrl_disconnect(): usbh_video_stop(), then usbh_video_class_free(). */
static void disconnect(void)
{
    usbh_video_stop(&video);
    assert(registry[0] == RT_NULL && mutex_held == 0);
    memset(&video, 0, sizeof(video));
    /* Nothing may read the cleared slot until it is reused. */
    ASAN_POISON_MEMORY_REGION(&video, sizeof(video));
}

static const struct dfs_file_ops *fops;

static int xopen(struct dfs_fd *fd)
{
    fd->flags = O_RDWR;
    fd->fnode = &fnode;
    return fops->open(fd);
}

static void start(struct dfs_fd *fd, uint32_t frame_size)
{
    struct uvc_format fmt = { .width = 640, .height = 480, .fourcc = USBH_VIDEO_FOURCC_MJPEG };
    struct uvc_requestbuffers req = { .count = 3 };

    assert(fops->ioctl(fd, VIDIOC_S_FMT, &fmt) == 0);
    assert(fops->ioctl(fd, VIDIOC_REQBUFS, &req) == 0);
    for (uint32_t i = 0; i < req.count; i++) {
        struct uvc_frame f = { .index = i };
        assert(fops->ioctl(fd, VIDIOC_QBUF, &f) == 0);
    }
    video.commit.dwMaxVideoFrameSize = frame_size;
    video.commit.dwMaxPayloadTransferSize = 3072;
}

static void two_cameras(void)
{
    static struct usbh_video second;
    static struct usbh_hubport second_hport;
    struct dfs_fnode second_node;
    struct dfs_fd first_fd = {0}, second_fd = {0};
    struct uvc_fmtdesc desc = {0};

    connect();
    second = video;
    second_hport = hport;
    second.hport = &second_hport;
    second.minor = 1;
    strcpy(second_hport.config.intf[0].devname, "video1");
    usbh_video_run(&second);
    assert(registry[0] == &video.device && registry[1] == &second.device);
    second_node.data = &second.device;
    second_fd.fnode = &second_node;
    second_fd.flags = O_RDWR;
    fops = video.device.fops;

    assert(xopen(&first_fd) == 0);
    assert(fops->open(&second_fd) == -EBUSY);
    kills = closes = 0;
    usbh_video_stop(&second);  /* an unused camera disappears */
    assert(kills == 0 && closes == 0);
    assert(fops->ioctl(&first_fd, VIDIOC_ENUM_FMT, &desc) == 0);
    usbh_video_run(&second);
    assert(fops->close(&first_fd) == 0);

    assert(fops->open(&second_fd) == 0);
    start(&second_fd, 64000);
    second.commit.dwMaxVideoFrameSize = 64000;
    second.commit.dwMaxPayloadTransferSize = 3072;
    assert(fops->ioctl(&second_fd, VIDIOC_STREAMON, NULL) == 0);
    kills = closes = 0;
    disconnect();             /* removing video0 must not stop video1 */
    assert(kills == 0 && closes == 0);
    assert(fops->ioctl(&second_fd, VIDIOC_ENUM_FMT, &desc) == 0);
    assert(fops->close(&second_fd) == 0 && kills == 5 && closes == 1 && pools == 0);
    usbh_video_stop(&second);
    assert(registry[0] == NULL && registry[1] == NULL);
    ASAN_UNPOISON_MEMORY_REGION(&video, sizeof(video));
}

static void errno_mapping(void)
{
    static const struct { int in, out; } usb[] = {
        { -USB_ERR_NOMEM, -ENOMEM }, { -USB_ERR_INVAL, -EINVAL }, { -USB_ERR_NODEV, -ENODEV },
        { -USB_ERR_NOTCONN, -ENODEV }, { -USB_ERR_NOTSUPP, -EOPNOTSUPP }, { -USB_ERR_BUSY, -EBUSY },
        { -USB_ERR_RANGE, -ENOSPC }, { -USB_ERR_STALL, -EPIPE }, { -USB_ERR_BABBLE, -EOVERFLOW },
        { -USB_ERR_NAK, -EAGAIN }, { -USB_ERR_DT, -EPROTO }, { -USB_ERR_IO, -EIO },
        { -USB_ERR_SHUTDOWN, -ESHUTDOWN }, { -USB_ERR_TIMEOUT, -ETIMEDOUT }, { -99, -EIO },
        { 0, 0 }, { 26, 26 },
    };
    for (size_t i = 0; i < sizeof(usb) / sizeof(usb[0]); i++)
        assert(uvc_usb_to_errno(usb[i].in) == usb[i].out);
    /* The reported bug: a USB I/O error used to reach user space as ENOMEM. */
    assert(-USB_ERR_IO == -ENOMEM && uvc_usb_to_errno(-USB_ERR_IO) == -EIO);
}

int main(void)
{
    struct dfs_fd fd = { 0 }, fd2 = { 0 };
    struct rt_pollreq req;
    struct uvc_fmtdesc fmtdesc = { .index = 5 };

    errno_mapping();
    assert(uvc_iomutex_init() == RT_EOK);
    two_cameras();
    connect();
    fops = video.device.fops;

    /* RT-Thread codes no longer leak: an out-of-range index is EINVAL (22), not 10. */
    assert(xopen(&fd) == 0);
    assert(xopen(&fd2) == -EBUSY);
    assert(fops->ioctl(&fd, VIDIOC_ENUM_FMT, &fmtdesc) == -EINVAL);

    /* Interval 0 means "default": silently use it; a bad one is still reported. */
    {
        struct uvc_format fmt = { .width = 640, .height = 480, .fourcc = USBH_VIDEO_FOURCC_MJPEG };

        klog[0] = 0;
        assert(fops->ioctl(&fd, VIDIOC_S_FMT, &fmt) == 0 && fmt.frameinterval == 333333);
        assert(strstr(klog, "Unsupported interval") == NULL);
        fmt.frameinterval = 12345;
        assert(fops->ioctl(&fd, VIDIOC_S_FMT, &fmt) == 0 && fmt.frameinterval == 333333);
        assert(strstr(klog, "Unsupported interval") != NULL);
    }

    /* A USB I/O error starting the stream reaches user space as EIO. */
    start(&fd, 64 * 1024);
    open_ret = -USB_ERR_IO;
    assert(fops->ioctl(&fd, VIDIOC_STREAMON, NULL) == -EIO);
    open_ret = -USB_ERR_RANGE;
    assert(fops->ioctl(&fd, VIDIOC_STREAMON, NULL) == -ENOSPC);
    open_ret = 0;

    /* No periodic bandwidth at submit: ENOSPC, submitted URBs killed, nothing leaks. */
    submit_ret = -USB_ERR_RANGE;
    submits = kills = closes = 0;
    assert(fops->ioctl(&fd, VIDIOC_STREAMON, NULL) == -ENOSPC);
    assert(kills == 5 && closes == 1);
    submit_ret = 0;

    /* DDMA tracks packet indexes directly: non-power-of-two counts work. */
    klog[0] = 0;
    submits = 0;
    video.commit.dwMaxVideoFrameSize = 40000;
    assert(fops->ioctl(&fd, VIDIOC_STREAMON, NULL) == 0 && submits == 5);
    assert(last_urb->num_of_iso_packets == 14 && last_urb->transfer_buffer_length == 14 * 3072);
    /* One stream-on line with what was negotiated. */
    assert(strstr(klog, "stream on: MJPEG 640x480") && strstr(klog, "alt 1 ep 0x81 mps 1024x3"));
    assert(fops->ioctl(&fd, VIDIOC_STREAMOFF, NULL) == 0);
    kills = closes = 0;
    assert(fops->ioctl(&fd, VIDIOC_STREAMOFF, NULL) == 0 && kills == 0 && closes == 0);
    video.commit.dwMaxVideoFrameSize = 1 << 20;
    assert(fops->ioctl(&fd, VIDIOC_STREAMON, NULL) == 0);
    assert(last_urb->num_of_iso_packets == 32);

    /* Streaming, an app waits in select(): no frame yet. */
    polled_wq = NULL;
    poll_wakeups = 0;
    assert(fops->poll(&fd, &req) == 0 && polled_wq != NULL);

    /* Unplug: the stream stops once, under the lock; the waiter is woken. */
    kills = closes = 0;
    disconnect();
    assert(kills == 5 && closes == 1 && poll_wakeups == 1);

    /* The fd is dead: errors right away, and the cleared slot is not read. */
    assert(fops->poll(&fd, &req) == (POLLERR | POLLHUP));
    assert(fops->ioctl(&fd, VIDIOC_DQBUF, &(struct uvc_frame){ 0 }) == -ENODEV);
    assert(fops->read(&fd, NULL, 0) == -ENODEV);
    assert(xopen(&fd2) == -EBUSY);

    /* The app closes it: buffers are released, the gone stream is not stopped again. */
    kills = closes = 0;
    assert(fops->close(&fd) == 0 && kills == 0 && closes == 0 && pools == 0);

    /* An open racing the unplug (devfs found the device just before) is ENODEV. */
    assert(xopen(&fd2) == -ENODEV);

    /* Re-enumeration into the same slot while an old fd is still open. */
    ASAN_UNPOISON_MEMORY_REGION(&video, sizeof(video));
    connect();
    assert(xopen(&fd) == 0);
    start(&fd, 1 << 20);
    assert(fops->ioctl(&fd, VIDIOC_STREAMON, NULL) == 0);
    disconnect();
    ASAN_UNPOISON_MEMORY_REGION(&video, sizeof(video));
    connect();
    /* The old fd does not reach the new camera, and keeps the slot busy. */
    assert(fops->ioctl(&fd, VIDIOC_ENUM_FMT, &(struct uvc_fmtdesc){ 0 }) == -ENODEV);
    assert(fops->poll(&fd, &req) == (POLLERR | POLLHUP));
    assert(xopen(&fd2) == -EBUSY);
    /* Closing it must not stop the new camera's stream or touch its URBs. */
    kills = closes = 0;
    assert(fops->close(&fd) == 0 && kills == 0 && closes == 0 && pools == 0);

    /* The next run works without a reset. */
    assert(xopen(&fd2) == 0);
    assert(fops->ioctl(&fd2, VIDIOC_ENUM_FMT, &(struct uvc_fmtdesc){ 0 }) == 0);
    start(&fd2, 1 << 20);
    assert(fops->ioctl(&fd2, VIDIOC_STREAMON, NULL) == 0);
    kills = closes = 0;
    assert(fops->close(&fd2) == 0 && kills == 5 && closes == 1 && pools == 0);

    /* Another thread's fops call holds io_mutex longer than a timed wait
     * (a slow control transfer). Close waits for it and succeeds: a failed
     * close would leave the fd to DFS, which closes it again later from
     * another thread, unmapping the buffers from the wrong process. */
    assert(xopen(&fd) == 0);
    start(&fd, 1 << 20);
    assert(fops->ioctl(&fd, VIDIOC_STREAMON, NULL) == 0);
    kills = closes = 0;
    other_holds = other_holds_long = 1;
    assert(fops->close(&fd) == 0);
    assert(!other_holds && kills == 5 && closes == 1 && pools == 0);
    assert(xopen(&fd) == 0);
    assert(fops->close(&fd) == 0);

    /* Unplug with nothing open does not touch the stream state. */
    kills = closes = 0;
    disconnect();
    assert(kills == 0 && closes == 0);

    puts("PASS UVC host: errno mapping, ioctl errno, unplug while streaming, stale fd, iso packets, two-camera selection");
    return 0;
}
"""


def main():
    source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else CANMV_USB / "usbh_uvc_thr.c"
    with tempfile.TemporaryDirectory(prefix="k230-uvc-host-test-") as directory:
        build = Path(directory)
        (build / "rtconfig.h").write_text(RTCONFIG)
        # MPP's k_type.h includes this generated, git-ignored header; the
        # build directory comes first on the include path.
        (build / "k_autoconf_comm.h").write_text("/* generated by the SDK build */\n")
        # Use the production interval helper with the scripted host types.
        core = (USB / "core/usbh_core.h").read_text()
        interval = core[core.index("static inline uint32_t usbh_get_urb_interval("):
                        core.index("#define USBH_EP_INIT")]
        for name, text in HEADERS.items():
            if name == "usbh_core.h":
                text += interval
            (build / name).write_text(text)
        # Keep private driver functions visible without exporting test hooks.
        (build / "test.c").write_text(f'#include "{source}"\n' + MODEL)
        cc = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Werror",
              "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
              "-Wno-pointer-sign", "-Wno-builtin-declaration-mismatch",
              "-Wno-zero-length-bounds", "-Wno-array-bounds", "-fsanitize=undefined,address",
              "-fno-sanitize-recover=all",
              "-I", str(build), "-I", str(CANMV_USB), "-I", str(USB / "class/video"),
              "-I", str(USB / "common"), "-I", str(RTT / "include"),
              "-I", str(RTT / "components/drivers/include"),
              "-I", str(MPP / "include"), "-I", str(MPP / "include/comm"),
              "-I", str(MPP / "kernel/ext_inc")]
        binary = build / "test"
        subprocess.run([*cc, str(build / "test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
