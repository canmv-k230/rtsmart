#include "usbh_core.h"
#include "usbh_video.h"

#include <dfs_poll.h>
#include <dfs_posix.h>
#include <lwp.h>
#include <lwp_user_mm.h>
#include "tick.h"
#include "rvv_ops.h"

#define UVC_USE_RVV_COPY 1
#define UVC_PROFILE_WINDOW_MS 1000

#define UVC_URBS (5)
#define UVC_MAX_PACKETS (32)
#define MAX_UVC_BUFFER (32)

#if VB_VERSION
extern k_s32 vb_create_pool(k_u32 *pool_id, k_u32 blk_cnt, k_u64 blk_size,
                            const char *mmz_name, const char *buf_name,
                            k_vb_remap_mode vb_remap_mode);
extern k_s32 vb_destroy_pool(k_u32 pool_id);
extern k_vb_blk_handle vb_get_blk_by_size_and_pool_id(k_u32 pool_id, k_u64 blk_size, k_u32 uid);
extern k_s32 vb_put_blk(k_u32 pool_id, k_u64 phys_addr);
extern k_u64 vb_blk_handle_to_phys(k_vb_blk_handle handle);
extern k_u32 vb_blk_handle_to_pool_id(k_vb_blk_handle handle);
extern void *vb_blk_handle_to_kern(k_vb_blk_handle handle);
extern k_s32 vb_user_sub(k_u32 pool_id, k_u64 phys_addr, k_u32 uid);
extern k_s32 vb_inquire_user_cnt(k_vb_blk_handle handle);
#endif

struct uvc_buffer {
    struct uvc_frame buf;
    int state;
    struct rt_wqueue wait_queue;
    struct rt_list_node stream;
    struct rt_list_node irq;
    bool driver_use;
    uint64_t frame_copy_ns;
    uint64_t frame_copy_bytes;
    uint32_t frame_copy_count;
    /* for vb version */
    uint32_t handle;
    uint64_t blk_phys_addr;
    uint64_t phys_addr;
    uint32_t pool_id;
    void *blk_virt_addr;
    void *virt_addr;
};

struct uvc_queue {
    void *mem;
    int count;
    int buffer_size;

    struct rt_list_node app_queue;
    struct rt_list_node irq_queue;

    struct uvc_buffer buffer[MAX_UVC_BUFFER + 1];
    struct usbh_urb *urb[UVC_URBS];
    char *urb_buffer[UVC_URBS];
    uint64_t urb_dma[UVC_URBS];
    rt_bool_t disconnect;
    rt_bool_t streaming;
    rt_bool_t opened;
};

/* Values for bmHeaderInfo (Video and Still Image Payload Headers, 2.4.3.3) */
#define UVC_STREAM_EOH	(1 << 7)
#define UVC_STREAM_ERR	(1 << 6)
#define UVC_STREAM_STI	(1 << 5)
#define UVC_STREAM_RES	(1 << 4)
#define UVC_STREAM_SCR	(1 << 3)
#define UVC_STREAM_PTS	(1 << 2)
#define UVC_STREAM_EOF	(1 << 1)
#define UVC_STREAM_FID	(1 << 0)

static struct uvc_queue uvc_queue;
struct uvc_copy_window {
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_bytes;
    uint32_t count;
    uint64_t start_ms;
    uint64_t sample_ms;
    rt_bool_t valid;
};

struct uvc_copy_profiler {
    struct uvc_copy_window active;
    struct uvc_copy_window last;
};

struct uvc_frame_window {
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t total_bytes;
    uint64_t total_copies;
    uint32_t frame_count;
    uint64_t start_ms;
    uint64_t sample_ms;
    rt_bool_t valid;
};

struct uvc_frame_profiler {
    struct uvc_frame_window active;
    struct uvc_frame_window last;
};

struct uvc_copy_summary {
    uint64_t avg_ns;
    uint64_t max_ns;
    uint64_t avg_bytes;
    uint64_t throughput;
    uint64_t total_bytes;
    uint32_t count;
    uint64_t sample_ms;
    rt_bool_t valid;
};

struct uvc_frame_summary {
    uint64_t avg_ns;
    uint64_t max_ns;
    uint64_t avg_frame_bytes;
    uint64_t avg_copies;
    uint32_t frames;
    uint64_t sample_ms;
    rt_bool_t valid;
};

static struct uvc_copy_profiler uvc_copy_profiler;
static struct uvc_frame_profiler uvc_frame_profiler;
static volatile rt_bool_t uvc_profile_enabled = RT_FALSE;

static inline const char *uvc_copy_method_name(void)
{
#if UVC_USE_RVV_COPY
    return "rvv";
#else
    return "memcpy";
#endif
}

static void uvc_memcpy_bytes(void *dst, const void *src, uint32_t nbytes)
{
#if UVC_USE_RVV_COPY
    rvv_memcpy(dst, src, nbytes);
#else
    memcpy(dst, src, nbytes);
#endif
}

static void uvc_roll_copy_window(struct uvc_copy_profiler *profiler, uint64_t now_ms)
{
    rt_base_t level = rt_hw_interrupt_disable();

    profiler->last = profiler->active;
    profiler->last.sample_ms = now_ms - profiler->active.start_ms;
    profiler->last.valid = RT_TRUE;
    rt_memset(&profiler->active, 0, sizeof(profiler->active));
    profiler->active.start_ms = now_ms;

    rt_hw_interrupt_enable(level);
}

static void uvc_roll_frame_window(struct uvc_frame_profiler *profiler, uint64_t now_ms)
{
    rt_base_t level = rt_hw_interrupt_disable();

    profiler->last = profiler->active;
    profiler->last.sample_ms = now_ms - profiler->active.start_ms;
    profiler->last.valid = RT_TRUE;
    rt_memset(&profiler->active, 0, sizeof(profiler->active));
    profiler->active.start_ms = now_ms;

    rt_hw_interrupt_enable(level);
}

static void uvc_copy_stat_update(uint64_t delta_ns, uint32_t bytes)
{
    struct uvc_copy_window *active = &uvc_copy_profiler.active;
    uint64_t now_ms = cpu_ticks_ms();

    if (active->start_ms == 0) {
        active->start_ms = now_ms;
    }

    active->total_ns += delta_ns;
    active->total_bytes += bytes;
    active->count++;
    if (delta_ns > active->max_ns) {
        active->max_ns = delta_ns;
    }

    if ((now_ms - active->start_ms) >= UVC_PROFILE_WINDOW_MS && active->count) {
        uvc_roll_copy_window(&uvc_copy_profiler, now_ms);
    }
}

static void uvc_buffer_copy_stat_reset(struct uvc_buffer *uvc_buf)
{
    uvc_buf->frame_copy_ns = 0;
    uvc_buf->frame_copy_bytes = 0;
    uvc_buf->frame_copy_count = 0;
}

static void uvc_buffer_copy_stat_update(struct uvc_buffer *uvc_buf, uint64_t delta_ns, uint32_t bytes)
{
    uvc_buf->frame_copy_ns += delta_ns;
    uvc_buf->frame_copy_bytes += bytes;
    uvc_buf->frame_copy_count++;
}

static void uvc_frame_copy_stat_finish(struct uvc_buffer *uvc_buf)
{
    uint64_t now_ms = cpu_ticks_ms();
    struct uvc_frame_window *active = &uvc_frame_profiler.active;

    if (uvc_buf->frame_copy_count == 0) {
        uvc_buffer_copy_stat_reset(uvc_buf);
        return;
    }

    if (active->start_ms == 0) {
        active->start_ms = now_ms;
    }

    active->total_ns += uvc_buf->frame_copy_ns;
    active->total_bytes += uvc_buf->frame_copy_bytes;
    active->total_copies += uvc_buf->frame_copy_count;
    active->frame_count++;
    if (uvc_buf->frame_copy_ns > active->max_ns) {
        active->max_ns = uvc_buf->frame_copy_ns;
    }

    if ((now_ms - active->start_ms) >= UVC_PROFILE_WINDOW_MS && active->frame_count) {
        uvc_roll_frame_window(&uvc_frame_profiler, now_ms);
    }

    uvc_buffer_copy_stat_reset(uvc_buf);
}

static void uvc_copy_window_snapshot(struct uvc_copy_window *window)
{
    rt_base_t level = rt_hw_interrupt_disable();
    *window = uvc_copy_profiler.last;
    rt_hw_interrupt_enable(level);
}

static void uvc_frame_window_snapshot(struct uvc_frame_window *window)
{
    rt_base_t level = rt_hw_interrupt_disable();
    *window = uvc_frame_profiler.last;
    rt_hw_interrupt_enable(level);
}

static void uvc_copy_window_summarize(const struct uvc_copy_window *window, struct uvc_copy_summary *summary)
{
    rt_memset(summary, 0, sizeof(*summary));
    if (!window->valid || !window->count || !window->sample_ms) {
        return;
    }

    summary->avg_ns = window->total_ns / window->count;
    summary->max_ns = window->max_ns;
    summary->avg_bytes = window->total_bytes / window->count;
    summary->throughput = (window->total_bytes * 1000ULL) / window->sample_ms;
    summary->total_bytes = window->total_bytes;
    summary->count = window->count;
    summary->sample_ms = window->sample_ms;
    summary->valid = RT_TRUE;
}

static void uvc_frame_window_summarize(const struct uvc_frame_window *window, struct uvc_frame_summary *summary)
{
    rt_memset(summary, 0, sizeof(*summary));
    if (!window->valid || !window->frame_count || !window->sample_ms) {
        return;
    }

    summary->avg_ns = window->total_ns / window->frame_count;
    summary->max_ns = window->max_ns;
    summary->avg_frame_bytes = window->total_bytes / window->frame_count;
    summary->avg_copies = window->total_copies / window->frame_count;
    summary->frames = window->frame_count;
    summary->sample_ms = window->sample_ms;
    summary->valid = RT_TRUE;
}

static void uvc_profile_reset(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    rt_memset(&uvc_copy_profiler, 0, sizeof(uvc_copy_profiler));
    rt_memset(&uvc_frame_profiler, 0, sizeof(uvc_frame_profiler));
    for (int i = 0; i < MAX_UVC_BUFFER + 1; i++) {
        uvc_buffer_copy_stat_reset(&uvc_queue.buffer[i]);
    }
    rt_hw_interrupt_enable(level);
}

static void uvc_profile_set_enabled(rt_bool_t enable)
{
    if (enable) {
        uvc_profile_reset();
        uvc_profile_enabled = RT_TRUE;
    } else {
        uvc_profile_enabled = RT_FALSE;
        uvc_profile_reset();
    }
}

static int uvc_profile_cmd(int argc, char **argv)
{
    if (argc == 1) {
        rt_kprintf("uvc host profile: %d\n", uvc_profile_enabled ? 1 : 0);
        return 0;
    }

    if (argc == 2) {
        if (argv[1][0] == '0' && argv[1][1] == '\0') {
            uvc_profile_set_enabled(RT_FALSE);
            rt_kprintf("uvc host profile: 0\n");
            return 0;
        }

        if (argv[1][0] == '1' && argv[1][1] == '\0') {
            uvc_profile_set_enabled(RT_TRUE);
            rt_kprintf("uvc host profile: 1\n");
            return 0;
        }
    }

    rt_kprintf("Usage: uvc_host_profile [0|1]\n");
    return -RT_ERROR;
}

static int uvc_profile_dump_cmd(int argc, char **argv)
{
    struct uvc_copy_window copy_window;
    struct uvc_frame_window frame_window;
    struct uvc_copy_summary copy_summary;
    struct uvc_frame_summary frame_summary;

    (void)argc;
    (void)argv;

    if (!uvc_profile_enabled) {
        rt_kprintf("uvc host profile: disabled\n");
        return 0;
    }

    uvc_copy_window_snapshot(&copy_window);
    uvc_copy_window_summarize(&copy_window, &copy_summary);
    if (!copy_summary.valid) {
        rt_kprintf("uvc %s copy: no completed %dms sample yet\n",
                   uvc_copy_method_name(), UVC_PROFILE_WINDOW_MS);
    } else {
        rt_kprintf("uvc %s copy: sample=%lums count=%u avg=%lu.%03luus max=%lu.%03luus "
                   "avg_bytes=%lu throughput=%luB/s bytes=%lu\n",
                   uvc_copy_method_name(),
                   (unsigned long)copy_summary.sample_ms,
                   copy_summary.count,
                   (unsigned long)(copy_summary.avg_ns / 1000),
                   (unsigned long)(copy_summary.avg_ns % 1000),
                   (unsigned long)(copy_summary.max_ns / 1000),
                   (unsigned long)(copy_summary.max_ns % 1000),
                   (unsigned long)copy_summary.avg_bytes,
                   (unsigned long)copy_summary.throughput,
                   (unsigned long)copy_summary.total_bytes);
    }

    uvc_frame_window_snapshot(&frame_window);
    uvc_frame_window_summarize(&frame_window, &frame_summary);
    if (!frame_summary.valid) {
        rt_kprintf("uvc frame copy: no completed %dms sample yet\n", UVC_PROFILE_WINDOW_MS);
    } else {
        rt_kprintf("uvc frame copy: sample=%lums frames=%u avg=%lu.%03luus max=%lu.%03luus "
                   "avg_copies=%lu avg_frame_bytes=%lu\n",
                   (unsigned long)frame_summary.sample_ms,
                   frame_summary.frames,
                   (unsigned long)(frame_summary.avg_ns / 1000),
                   (unsigned long)(frame_summary.avg_ns % 1000),
                   (unsigned long)(frame_summary.max_ns / 1000),
                   (unsigned long)(frame_summary.max_ns % 1000),
                   (unsigned long)frame_summary.avg_copies,
                   (unsigned long)frame_summary.avg_frame_bytes);
    }

    return 0;
}

static int uvc_profile_reset_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uvc_profile_reset();
    rt_kprintf("uvc host profile reset\n");
    return 0;
}

MSH_CMD_EXPORT_ALIAS(uvc_profile_dump_cmd, uvc_host_profile_dump, dump last completed uvc host copy profile window);
MSH_CMD_EXPORT_ALIAS(uvc_profile_reset_cmd, uvc_host_profile_reset, reset uvc host copy profile windows);
MSH_CMD_EXPORT_ALIAS(uvc_profile_cmd, uvc_host_profile, get or set uvc host copy profile state);

static inline void uvc_copy_payload(struct uvc_buffer *uvc_buf, void *dst, const void *src, uint32_t nbytes)
{
    if (nbytes == 0) {
        return;
    }

    if (!uvc_profile_enabled) {
        uvc_memcpy_bytes(dst, src, nbytes);
    } else {
        uint64_t start_ns = cpu_ticks_ns();
        uint64_t delta_ns;

        uvc_memcpy_bytes(dst, src, nbytes);
        delta_ns = cpu_ticks_ns() - start_ns;
        uvc_copy_stat_update(delta_ns, nbytes);
        uvc_buffer_copy_stat_update(uvc_buf, delta_ns, nbytes);
    }
}

static inline void uvc_buffer_profile_finish(struct uvc_buffer *uvc_buf)
{
    if (uvc_profile_enabled) {
        uvc_frame_copy_stat_finish(uvc_buf);
    }
}

static inline void uvc_buffer_profile_reset(struct uvc_buffer *uvc_buf)
{
    uvc_buffer_copy_stat_reset(uvc_buf);
}

static int usb_control_msg(struct usbh_hubport *hport, uint8_t request, uint8_t requesttype,
                           uint16_t value, uint16_t index, void *data, uint16_t size)
{
    struct usb_setup_packet *setup = hport->setup;

    setup->bmRequestType = requesttype;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = size;

    return usbh_control_transfer(hport, setup, data);
}

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define min(x,y) ({ \
                  typeof(x) _x = (x);	\
                  typeof(y) _y = (y);	\
                  (void) (&_x == &_y);	\
                  _x < _y ? _x : _y; })

#define max(x,y) ({ \
                  typeof(x) _x = (x);	\
                  typeof(y) _y = (y);	\
                  (void) (&_x == &_y);	\
                  _x > _y ? _x : _y; })

#define min_t(type, a, b) min(((type) a), ((type) b))
#define max_t(type, a, b) max(((type) a), ((type) b))

static uint32_t uvc_get_current_frame_size(const struct usbh_video *video)
{
    switch (video->current_fourcc) {
    case USBH_VIDEO_FOURCC_YUY2:
    case USBH_VIDEO_FOURCC_UYVY:
        return video->current_frame.wHeight * video->current_frame.wWidth * 2;
    case USBH_VIDEO_FOURCC_NV12:
    case USBH_VIDEO_FOURCC_I420:
        return (video->current_frame.wHeight * video->current_frame.wWidth * 3) / 2;
    default:
        return video->current_frame.dwMaxVideoFrameBufferSize;
    }
}

static const char *uvc_fourcc_to_name(uint32_t fourcc)
{
    switch (fourcc) {
    case USBH_VIDEO_FOURCC_YUY2:
        return "YUV 4:2:2 (YUY2)";
    case USBH_VIDEO_FOURCC_UYVY:
        return "YUV 4:2:2 (UYVY)";
    case USBH_VIDEO_FOURCC_NV12:
        return "YUV 4:2:0 (NV12)";
    case USBH_VIDEO_FOURCC_I420:
        return "YUV 4:2:0 (I420)";
    case USBH_VIDEO_FOURCC_MJPEG:
        return "MJPEG";
    default:
        return "UNKNOWN";
    }
}

static void uvc_release_buffers(void)
{
#if VB_VERSION
    for (int i = 0; i < MAX_UVC_BUFFER + 1; i++) {
        if (uvc_queue.buffer[i].handle != VB_INVALID_HANDLE) {
            int ret = vb_user_sub(uvc_queue.buffer[i].pool_id,
                                  uvc_queue.buffer[i].blk_phys_addr,
                                  VB_UID_V_VI);
            if (ret != K_SUCCESS) {
                USB_LOG_ERR("put blk[%d] fail\n", i);
            }
            uvc_queue.buffer[i].handle = VB_INVALID_HANDLE;
        }
    }

    if (uvc_queue.buffer[0].pool_id != VB_INVALID_POOLID) {
        if (vb_destroy_pool(uvc_queue.buffer[0].pool_id) != 0) {
            USB_LOG_ERR("fail to destroyed pool %d: %s %d\n",
                        uvc_queue.buffer[0].pool_id, __func__, __LINE__);
        }
    }
#else
    if (uvc_queue.mem != RT_NULL) {
        rt_free_align(uvc_queue.mem);
    }
#endif

    for (int i = 0; i < MAX_UVC_BUFFER + 1; i++) {
        uvc_queue.buffer[i].pool_id = VB_INVALID_POOLID;
        uvc_queue.buffer[i].buf.userptr = RT_NULL;
        uvc_queue.buffer[i].buf.bytesused = 0;
        uvc_queue.buffer[i].buf.length = 0;
        uvc_queue.buffer[i].buf.offset = 0;
        uvc_queue.buffer[i].driver_use = false;
        uvc_queue.buffer[i].phys_addr = 0;
        uvc_queue.buffer[i].virt_addr = RT_NULL;
        uvc_queue.buffer[i].blk_phys_addr = 0;
        uvc_queue.buffer[i].blk_virt_addr = RT_NULL;
        uvc_queue.buffer[i].state = VIDEOBUF_IDLE;
    }

    uvc_queue.mem = RT_NULL;
    uvc_queue.count = 0;
    uvc_queue.buffer_size = 0;
    rt_list_init(&uvc_queue.app_queue);
    rt_list_init(&uvc_queue.irq_queue);
}

static struct usbh_video_format *uvc_find_format(struct usbh_video *video,
                                                 uint32_t fourcc)
{
    for (int i = 0; i < video->num_of_formats; i++) {
        if ((fourcc == 0) || (video->format[i].fourcc == fourcc)) {
            return &video->format[i];
        }
    }

    return RT_NULL;
}

static k_pixel_format uvc_fourcc_to_kpixel(uint32_t fourcc)
{
    switch (fourcc) {
    case USBH_VIDEO_FOURCC_YUY2:
        return PIXEL_FORMAT_YUYV_PACKAGE_422;
    case USBH_VIDEO_FOURCC_UYVY:
        return PIXEL_FORMAT_UYVY_PACKAGE_422;
    case USBH_VIDEO_FOURCC_NV12:
        return PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    case USBH_VIDEO_FOURCC_I420:
        return PIXEL_FORMAT_YVU_PLANAR_420;
    default:
        return PIXEL_FORMAT_BUTT;
    }
}

static struct uvc_buffer *get_uvc_buf()
{
    if (uvc_queue.count <= 0) {
        return RT_NULL;
    }

    struct uvc_buffer *_uvc_buf = &uvc_queue.buffer[uvc_queue.count];
    struct uvc_buffer *uvc_buf = _uvc_buf;

    rt_base_t level;
    level = rt_hw_interrupt_disable();

    if (uvc_buf->buf.bytesused != 0) {
        goto out;
    }

    if (!rt_list_isempty(&uvc_queue.irq_queue)) {
        uvc_buf = rt_list_first_entry(&uvc_queue.irq_queue, struct uvc_buffer, irq);
#if VB_VERSION
        if (vb_inquire_user_cnt(uvc_buf->handle) != 1) {
            uvc_buf = _uvc_buf;
        }
#endif
    }
#if 0
    else {
        static uint64_t timeout;

        /* print log per second */
        if ((int64_t)(timeout - cpu_ticks()) < 0) {
            timeout = cpu_ticks() + (1000 * 1000 * 1) * 27;
            rt_hw_interrupt_enable(level);
            USB_LOG_ERR("app process too slow\n");
            level = rt_hw_interrupt_disable();
        }
    }
#endif

out:
    rt_hw_interrupt_enable(level);
    return uvc_buf;
}

static void uvc_decode_iso(struct usbh_urb *urb, struct uvc_buffer *uvc_buf, struct usbh_video *video)
{
    static uint32_t drop_err_cnt = 0;
    static uint64_t drop_last_ms = 0;
    uint8_t *src;
    uint8_t *dst;
    int i;
    int len;
    int maxlen;
    int nbytes;

    for (i = 0; i < urb->num_of_iso_packets; i ++) {
        if (urb->iso_packet[i].errorcode < 0) {
            USB_LOG_DBG("lost (%d).\n", urb->iso_packet[i].errorcode);
            continue;
        }

        src = urb->transfer_buffer + urb->iso_packet[i].offset;
#if VB_VERSION
        dst = uvc_buf->virt_addr + uvc_buf->buf.bytesused;
#else
        dst = uvc_queue.mem + uvc_buf->buf.offset + uvc_buf->buf.bytesused;
#endif

        len = urb->iso_packet[i].actual_length;

        if (len < 2 || src[0] < 2 || src[0] > len) {
            continue;
        }

        if (src[1] & UVC_STREAM_ERR) {
            uint64_t now_ms = cpu_ticks_ms();

            drop_err_cnt++;
            if ((now_ms - drop_last_ms) >= 1000) {
                USB_LOG_ERR("Dropping payload (error bit set): %u in last second\n", drop_err_cnt);
                drop_last_ms = now_ms;
                drop_err_cnt = 0;
            }
            continue;
        }

        len -= src[0];

        maxlen = uvc_buf->buf.length - uvc_buf->buf.bytesused;
        nbytes = min(len, maxlen);

        uvc_copy_payload(uvc_buf, dst, src + src[0], nbytes);

        uvc_buf->buf.bytesused += nbytes;

        if (len > maxlen) {
            uvc_buf->state = VIDEOBUF_DONE;
            USB_LOG_ERR("Frame complete (overflow)\n");
        }

        if (src[1] & UVC_STREAM_EOF) {
            uvc_buf->state = VIDEOBUF_DONE;
        }

        if (uvc_buf->state == VIDEOBUF_DONE || uvc_buf->state == VIDEOBUF_ERROR) {
            if (!uvc_buf->driver_use) {

                if ((video->current_format_type == USBH_VIDEO_FORMAT_MJPEG) ||
                    (uvc_buf->buf.bytesused == uvc_buf->buf.length)) {
                    rt_base_t level;
#if 0
                    static uint64_t prev;
                    uint64_t diff = cpu_ticks_ms() - prev;
                    prev = cpu_ticks_ms();
                    USB_LOG_ERR("F = %02llu ms, L = %d\n",
                                (unsigned long long)diff, uvc_buf->buf.bytesused);
#endif

                    level = rt_hw_interrupt_disable();

                    rt_list_remove(&uvc_buf->irq);

                    rt_hw_interrupt_enable(level);
                    uvc_buffer_profile_finish(uvc_buf);
                    rt_wqueue_wakeup(&uvc_buf->wait_queue, 0);
                } else {
                    uvc_buf->state = VIDEOBUF_QUEUED;
                    uvc_buf->buf.bytesused = 0;
                    uvc_buffer_profile_reset(uvc_buf);
                }
            } else {
#if 0
                USB_LOG_ERR("LLEN = %d\n", uvc_buf->buf.bytesused);
#endif
                uvc_buffer_profile_finish(uvc_buf);
                uvc_buf->state = VIDEOBUF_QUEUED;
                uvc_buf->buf.bytesused = 0;
            }

            uvc_buf = get_uvc_buf();
        }
    }
}

static void uvc_decode_bulk(struct usbh_urb *urb, struct uvc_buffer *uvc_buf, struct usbh_video *video)
{
    static uint32_t drop_err_cnt = 0;
    static uint64_t drop_last_ms = 0;
    int len, ret = 0;
    uint8_t *mem;
    uint8_t *dst;
    int maxlen;
    int nbytes;

    if (urb->actual_length == 0) {
        USB_LOG_ERR("zero len\n");
        return;
    }

    mem = urb->transfer_buffer;
    len = urb->actual_length;
    video->bulk.payload_size += len;

    if (video->bulk.header_size == 0 && !video->bulk.skip_payload) {
        if (len < 2 || mem[0] < 2 || mem[0] > len) {
            ret = -RT_EINVAL;
            goto check;
        }

        if (mem[1] & UVC_STREAM_ERR) {
            uint64_t now_ms = cpu_ticks_ms();

            drop_err_cnt++;
            if ((now_ms - drop_last_ms) >= 1000) {
                USB_LOG_ERR("Dropping bulk payload (error bit set): %u in last second\n", drop_err_cnt);
                drop_last_ms = now_ms;
                drop_err_cnt = 0;
            }
            ret = -RT_EIO;
            goto check;
        }

        ret = mem[0];
check:
        if (ret < 0) {
            video->bulk.skip_payload = 1;
        } else {
            memcpy(video->bulk.header, mem, ret);
            video->bulk.header_size = ret;

            mem += ret;
            len -= ret;
        }
    }

    if (!video->bulk.skip_payload) {
#if VB_VERSION
        dst = uvc_buf->virt_addr + uvc_buf->buf.bytesused;
#else
        dst = uvc_queue.mem + uvc_buf->buf.offset + uvc_buf->buf.bytesused;
#endif
        maxlen = uvc_buf->buf.length - uvc_buf->buf.bytesused;
        nbytes = min(len, maxlen);
        uvc_copy_payload(uvc_buf, dst, mem, nbytes);
        uvc_buf->buf.bytesused += nbytes;
        if (len > maxlen) {
            uvc_buf->state = VIDEOBUF_DONE;
            USB_LOG_ERR("Frame complete (over_flow)\n");
        }
    }

    if (urb->actual_length < urb->transfer_buffer_length ||
        video->bulk.payload_size >= video->commit.dwMaxPayloadTransferSize) {
        if (!video->bulk.skip_payload) {
            if ((video->bulk.header[1] & UVC_STREAM_EOF) && (uvc_buf->buf.bytesused != 0)) {
                uvc_buf->state = VIDEOBUF_DONE;
            }

            if (uvc_buf->state == VIDEOBUF_DONE || uvc_buf->state == VIDEOBUF_ERROR) {
                if (!uvc_buf->driver_use) {
                    rt_base_t level;
#if 0
                    static uint64_t prev;
                    uint64_t diff = cpu_ticks_ms() - prev;
                    prev = cpu_ticks_ms();
                    USB_LOG_ERR("F = %02llu ms, L = %d\n",
                                (unsigned long long)diff, uvc_buf->buf.bytesused);
#endif
                    level = rt_hw_interrupt_disable();

                    rt_list_remove(&uvc_buf->irq);

                    rt_hw_interrupt_enable(level);
                    uvc_buffer_profile_finish(uvc_buf);
                    rt_wqueue_wakeup(&uvc_buf->wait_queue, 0);
                } else {
#if 0
                    USB_LOG_ERR("LLEN = %d\n", uvc_buf->buf.bytesused);
#endif
                    uvc_buffer_profile_finish(uvc_buf);
                    uvc_buf->state = VIDEOBUF_QUEUED;
                    uvc_buf->buf.bytesused = 0;
                }

            }
        }
        video->bulk.header_size = 0;
        video->bulk.skip_payload = 0;
        video->bulk.payload_size = 0;
    }
}

/* todo: support nesting? & get_uvc_buf need spin_lock ? */
static void uvc_video_complete(void *arg, int nb)
{
    int ret;
    struct uvc_buffer *uvc_buf;
    struct usbh_urb *urb = (struct usbh_urb *)arg;
    struct usbh_video *video_class = (struct usbh_video *)urb->context;

    if (!video_class->is_opened) {
        return;
    }

    if (urb->errorcode != 0) {
        if (urb->errorcode == -USB_ERR_SHUTDOWN) {
            return;
        }
        USB_LOG_DBG("Non-zero status (%d) in video completion handler, try resubmit.\n",
                    urb->errorcode);
        goto requeue;
    }

    if (uvc_queue.count <= 0 || uvc_queue.buffer_size <= 0) {
        goto requeue;
    }

    uvc_buf = get_uvc_buf();
    if (!uvc_buf) {
        goto requeue;
    }

    if (video_class->num_of_intf_altsettings > 1) {
        uvc_decode_iso(urb, uvc_buf, video_class);
    } else {
        uvc_decode_bulk(urb, uvc_buf, video_class);
    }

requeue:
    if ((ret = usbh_submit_urb(urb)) < 0) {
        USB_LOG_ERR("Failed to resubmit video URB (%d).\n", ret);
    }
}

static int uvc_init_urbs(struct usbh_video *video_class)
{
    uint16_t psize;
    uint32_t size;
    int npackets;
    int i;
    int j;
    int ret = 0;

    struct usbh_urb *urb;

    psize = (video_class->num_of_intf_altsettings > 1) ? video_class->isoin_mps
        : (video_class->bulkin->wMaxPacketSize & USB_MAXPACKETSIZE_MASK);

    size  = (video_class->num_of_intf_altsettings > 1) ? video_class->commit.dwMaxVideoFrameSize
        : video_class->commit.dwMaxPayloadTransferSize;

    npackets = DIV_ROUND_UP(size, psize);
    USB_LOG_DBG("b4 npackets = %d, size = %d, psize = %d\n", npackets, size, psize);
    if (npackets > UVC_MAX_PACKETS)
        npackets = UVC_MAX_PACKETS;

    size = psize * npackets;
    USB_LOG_DBG("af npackets = %d, size = %d, psize = %d\n", npackets, size, psize);

    for (i = 0; i < UVC_URBS; ++i) {
        /* todo: */
#if 0
        uvc_queue.urb_buffer[i] = dma_alloc_coherent(size, size, &uvc_queue.urb_dma[i], size);
#else
        uvc_queue.urb_buffer[i] = rt_malloc_align(USB_ALIGN_UP(size, CONFIG_USB_ALIGN_SIZE), CONFIG_USB_ALIGN_SIZE);
#endif

        uvc_queue.urb[i] =
            (struct usbh_urb *)rt_calloc(1, sizeof(struct usbh_urb) +
                                         (npackets * sizeof(struct usbh_iso_frame_packet)));

        if (!uvc_queue.urb_buffer[i] || !uvc_queue.urb[i])
        {
            ret = -ENOMEM;
            goto out;
        }

    }

    for (i = 0; i < UVC_URBS; ++i) {
        urb = uvc_queue.urb[i];

        urb->transfer_buffer = uvc_queue.urb_buffer[i];
        urb->transfer_buffer_length = size;
        urb->hport = video_class->hport;
        urb->setup = NULL;
        urb->arg = urb;
        urb->timeout = 0;
        urb->context = (void *)video_class;
        urb->complete = uvc_video_complete;

        if (video_class->num_of_intf_altsettings > 1) {
            urb->num_of_iso_packets = npackets;
            urb->ep = video_class->isoin;
            urb->interval = USBH_GET_URB_INTERVAL(video_class->isoin->bInterval,
                                                  video_class->hport->speed);
            for (j = 0; j < npackets; ++j) {
                urb->iso_packet[j].offset = j * psize;
                urb->iso_packet[j].length = psize;
            }
        } else {
            urb->ep = video_class->bulkin;
        }
    }

    return ret;

out:
    for (; i >= 0; i--) {
        if (uvc_queue.urb_buffer[i]) {
            rt_free_align(uvc_queue.urb_buffer[i]);
            uvc_queue.urb_buffer[i] = RT_NULL;
        }

        if (uvc_queue.urb[i]) {
            rt_free(uvc_queue.urb[i]);
            uvc_queue.urb[i] = RT_NULL;
        }
    }

    return ret;
}

static void uvc_uninit_urbs(void)
{
    int i;

    for (i = 0; i < UVC_URBS; i++) {
        if (uvc_queue.urb_buffer[i]) {
            rt_free_align(uvc_queue.urb_buffer[i]);
            uvc_queue.urb_buffer[i] = RT_NULL;
        }

        if (uvc_queue.urb[i]) {
            rt_free(uvc_queue.urb[i]);
            uvc_queue.urb[i] = RT_NULL;
        }
    }
}

static void usbh_video_off(struct usbh_video *video_class);

static int usbh_video_on(struct usbh_video *video_class)
{
    int ret;

    ret = usbh_video_open(video_class, video_class->current_fourcc,
                          video_class->current_frame.wWidth,
                          video_class->current_frame.wHeight, -1);
    if (ret < 0) {
        goto out;
    }

    USB_LOG_DBG("video params:\n");
    USB_LOG_DBG("bmHint                   = %d\n", video_class->commit.hintUnion1.bmHint);
    USB_LOG_DBG("bFormatIndex             = %d\n", video_class->commit.bFormatIndex);
    USB_LOG_DBG("bFrameIndex              = %d\n", video_class->commit.bFrameIndex);
    USB_LOG_DBG("dwFrameInterval          = %d\n", video_class->commit.dwFrameInterval);
    USB_LOG_DBG("wKeyFrameRate            = %d\n", video_class->commit.wKeyFrameRate);
    USB_LOG_DBG("wPFrameRate              = %d\n", video_class->commit.wPFrameRate);
    USB_LOG_DBG("wCompQuality             = %d\n", video_class->commit.wCompQuality);
    USB_LOG_DBG("wCompWindowSize          = %d\n", video_class->commit.wCompWindowSize);
    USB_LOG_DBG("wDelay                   = %d\n", video_class->commit.wDelay);
    USB_LOG_DBG("dwMaxVideoFrameSize      = %d\n", video_class->commit.dwMaxVideoFrameSize);
    USB_LOG_DBG("dwMaxPayloadTransferSize = %d\n", video_class->commit.dwMaxPayloadTransferSize);
    USB_LOG_DBG("dwClockFrequency         = %d\n", video_class->commit.dwClockFrequency);
    USB_LOG_DBG("bmFramingInfo            = %d\n", video_class->commit.bmFramingInfo);
    USB_LOG_DBG("bPreferedVersion         = %d\n", video_class->commit.bPreferedVersion);
    USB_LOG_DBG("bMinVersion              = %d\n", video_class->commit.bMinVersion);
    USB_LOG_DBG("bMaxVersion              = %d\n", video_class->commit.bMaxVersion);
    USB_LOG_DBG("wMaxPacketSize           = %d\n", video_class->isoin_mps);

#if 0
    if (0)
    {
        uint8_t exposure_auto = 0x01; // 设置为手动模式
        ret = usbh_video_set(video_class, VIDEO_REQUEST_SET_CUR, video_class->ctrl_intf, 0x1,
                             VIDEO_CT_AE_MODE_CONTROL, &exposure_auto, 1);
        USB_LOG_ERR("AE ret = %d\n", ret);
    }
#endif

	video_class->bulk.header_size = 0;
	video_class->bulk.skip_payload = 0;
	video_class->bulk.payload_size = 0;

    ret = uvc_init_urbs(video_class);
    if (ret) {
        USB_LOG_ERR("%s err: ret = %d\n", __func__, ret);
        usbh_video_off(video_class);
        goto out;
    }

    for (int i = 0; i < UVC_URBS; ++i) {
        if ((ret = usbh_submit_urb(uvc_queue.urb[i])) < 0) {
            USB_LOG_ERR("%s: Failed to submit URB %u (%d).\n", __func__, i, ret);
            usbh_video_off(video_class);
            return ret;
        }
    }

    uvc_queue.streaming = RT_TRUE;

out:
    return ret;
}

static void usbh_video_off(struct usbh_video *video_class)
{
    rt_bool_t need_close = video_class->is_opened;

    /* Mark stream as stopping first to block decode/resubmit in URB completion. */
    video_class->is_opened = false;
    uvc_queue.streaming = RT_FALSE;

    for (int i = 0; i < UVC_URBS; i++) {
        if (uvc_queue.urb[i] != RT_NULL) {
            usbh_kill_urb(uvc_queue.urb[i]);
        }
    }
    uvc_uninit_urbs();

    if (need_close) {
        /* todo: check return val */
        usbh_video_close(video_class);
    }
}

static struct rt_mutex io_mutex;

int uvc_iomutex_init(void)
{
    return rt_mutex_init(&io_mutex, "io_mutex", RT_IPC_FLAG_PRIO);
}

static inline rt_err_t io_lock(void)
{
    rt_err_t ret;

    ret = rt_mutex_take(&io_mutex, rt_tick_from_millisecond(10000));
    if (ret != RT_EOK) {
        if (ret == -RT_ETIMEOUT) {
            USB_LOG_ERR("uvc take iolock timeout\n");
        } else {
            RT_ASSERT(0);
        }
    }

    return ret;
}

static inline void io_unlock(void)
{
    rt_mutex_release(&io_mutex);
}

INIT_COMPONENT_EXPORT(uvc_iomutex_init);

static int uvc_fops_open(struct dfs_fd *fd)
{
    if (io_lock() != RT_EOK) {
        return -RT_ERROR;
    }

    if (uvc_queue.opened) {
        io_unlock();
        return -RT_EBUSY;
    }

    uvc_queue.opened = RT_TRUE;
    uvc_queue.disconnect = RT_FALSE;
    uvc_queue.streaming = RT_FALSE;
    uvc_queue.count = 0;
    uvc_queue.buffer_size = 0;
    uvc_profile_reset();
    rt_list_init(&uvc_queue.app_queue);
    rt_list_init(&uvc_queue.irq_queue);

    for (int i = 0; i < MAX_UVC_BUFFER + 1; i++) {
        uvc_queue.buffer[i].handle = VB_INVALID_HANDLE;
        uvc_queue.buffer[i].pool_id = VB_INVALID_POOLID;
        uvc_queue.buffer[i].blk_phys_addr = 0;
        uvc_queue.buffer[i].blk_virt_addr = RT_NULL;
        uvc_queue.buffer[i].phys_addr = 0;
        uvc_queue.buffer[i].virt_addr = RT_NULL;
        uvc_queue.buffer[i].buf.userptr = RT_NULL;
        uvc_queue.buffer[i].buf.bytesused = 0;
        uvc_queue.buffer[i].state = VIDEOBUF_IDLE;
        uvc_queue.buffer[i].driver_use = false;
        uvc_buffer_profile_reset(&uvc_queue.buffer[i]);
    }

    io_unlock();
    return 0;
}

static int uvc_fops_close(struct dfs_fd *fd)
{
    if (io_lock() != RT_EOK) {
        return -RT_ERROR;
    }

    rt_device_t device = (rt_device_t)fd->fnode->data;
    struct usbh_video *video = (struct usbh_video *)device;
    if (video) {
        usbh_video_off(video);
    }

    if (lwp_self() != NULL) {
        for (int i = 0; i < uvc_queue.count; i ++) {
            void *va = (void *)uvc_queue.buffer[i].buf.userptr;
            if (va != RT_NULL) {
                lwp_unmap_user_phy(lwp_self(), va);
                uvc_queue.buffer[i].buf.userptr = 0;
                USB_LOG_DBG("unmap buffer\n");
            }
        }
    }

    uvc_release_buffers();
    uvc_queue.opened = RT_FALSE;
    uvc_queue.disconnect = RT_FALSE;
    io_unlock();

    return 0;
}

static int uvc_fops_ioctl(struct dfs_fd *fd, int cmd, void *args)
{
    int ret;
    rt_device_t device;

    if (io_lock() != RT_EOK) {
        return -RT_ERROR;
    }
    if (uvc_queue.disconnect) {
        ret = -ENODEV;
        goto out;
    }
    device = (rt_device_t)fd->fnode->data;
    ret = rt_device_control(device, cmd, args);

out:
    io_unlock();

    return ret;
}

static int uvc_fops_read(struct dfs_fd *fd, void *buf, size_t count)
{
    return 0;
}

static int uvc_fops_write(struct dfs_fd *fd, const void *buf, size_t count)
{
    return 0;
}

static int uvc_fops_poll(struct dfs_fd *fd, struct rt_pollreq *req)
{
    int mask = 0;
    int flags = 0;
    rt_base_t level;
    rt_device_t device;
    struct uvc_buffer *uvc_buf;

    device = (rt_device_t)fd->fnode->data;
    RT_ASSERT(device != RT_NULL);

    if (io_lock() != RT_EOK) {
        return POLLERR;
    }
    if (uvc_queue.disconnect) {
        mask |= POLLERR;
        goto done;
    }

    level = rt_hw_interrupt_disable();

    if (rt_list_isempty(&uvc_queue.app_queue)) {
        mask |= POLLERR;
        rt_hw_interrupt_enable(level);
        goto done;
    }

    uvc_buf = rt_list_first_entry(&uvc_queue.app_queue, struct uvc_buffer, stream);
    rt_hw_interrupt_enable(level);

    /* only support POLLIN */
    flags = fd->flags & O_ACCMODE;
    if (flags == O_RDONLY || flags == O_RDWR) {

        rt_poll_add(&(uvc_buf->wait_queue), req);

        level = rt_hw_interrupt_disable();
        if (uvc_buf->state == VIDEOBUF_DONE) {
            mask |= POLLIN;
        } else if (uvc_buf->state == VIDEOBUF_ERROR) {
            mask |= POLLERR;
        }
        rt_hw_interrupt_enable(level);
    }

done:

    io_unlock();

    return mask;
}

const static struct dfs_file_ops uvc_fops =
{
    uvc_fops_open,
    uvc_fops_close,
    uvc_fops_ioctl,     /* ioctl */
    uvc_fops_read,
    uvc_fops_write,
    RT_NULL,            /* flush */
    RT_NULL,            /* lseek */
    RT_NULL,            /* getdents */
    uvc_fops_poll,      /* poll */
};

static rt_err_t uvc_open(rt_device_t dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

rt_err_t uvc_close(rt_device_t dev)
{
    return RT_EOK;
}

static rt_size_t uvc_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    return RT_EOK;
}

static rt_size_t uvc_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    return RT_EOK;
}

struct uvc_format_desc {
    uint32_t fourcc;
    const char *name;
};

static struct uvc_format_desc uvc_fmts[] = {
    {
        .fourcc = USBH_VIDEO_FOURCC_YUY2,
        .name = "YUV 4:2:2 (YUY2)",
    },
    {
        .fourcc = USBH_VIDEO_FOURCC_UYVY,
        .name = "YUV 4:2:2 (UYVY)",
    },
    {
        .fourcc = USBH_VIDEO_FOURCC_NV12,
        .name = "YUV 4:2:0 (NV12)",
    },
    {
        .fourcc = USBH_VIDEO_FOURCC_I420,
        .name = "YUV 4:2:0 (I420)",
    },
    {
        .fourcc = USBH_VIDEO_FOURCC_MJPEG,
        .name = "MJPEG",
    },
};

#include "cache.h"
extern rt_mmu_info mmu_info;

static rt_err_t uvc_control(rt_device_t dev, int cmd, void *args)
{
    struct usbh_video *video = (struct usbh_video *)dev;
    int ret = RT_EOK;

    switch (cmd) {
    case VIDIOC_GET_INDEX: {
        struct usb_index local_index;
        struct usb_index *index;

        if (lwp_self() == NULL) {
            index = (struct usb_index *)args;
        } else {
            index = &local_index;
            lwp_get_from_user(index, args, sizeof(*index));
        }

        index->iManufacturer = video->hport->device_desc.iManufacturer;
        index->iProduct = video->hport->device_desc.iProduct;

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, index, sizeof(*index));
        }
        break;
    }
    case VIDIOC_GET_STRING: {
        struct usb_string local_string;
        struct usb_string *string;
        const char *init_name = "usb video device";

        if (lwp_self() == NULL) {
            string = (struct usb_string *)args;
        } else {
            string = &local_string;
            lwp_get_from_user(string, args, sizeof(*string));
        }

        memset(string->str, '\0', sizeof(string->str));

        ret = usbh_get_string_desc(video->hport, string->index, string->str, sizeof(string->str));
        if (ret < 0) {
            memcpy(string->str, init_name, strlen(init_name) + 1);
        } else {
            string->str[MAX_USB_STRING_LEN - 1] = '\0';
        }

        ret = RT_EOK;

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, string, sizeof(*string));
        }
        break;
    }
    case VIDIOC_ENUM_FMT: {
        int i;
        struct uvc_fmtdesc local_fmt;
        struct uvc_fmtdesc *fmt;
        int idx;

        if (lwp_self() == NULL) {
            fmt = (struct uvc_fmtdesc *)args;
        } else {
            fmt = &local_fmt;
            lwp_get_from_user(fmt, args, sizeof(*fmt));
        }

        idx = fmt->index;

        if (idx >= video->num_of_formats) {
            ret = -RT_EINVAL;
            goto out;
        }

        fmt->fourcc = video->format[idx].fourcc;

        for (i = 0; i < sizeof(uvc_fmts) / sizeof(uvc_fmts[0]); i++) {
            if (uvc_fmts[i].fourcc == fmt->fourcc) {
                break;
            }
        }

        memset(fmt->description, 0, sizeof(fmt->description));
        if (i < sizeof(uvc_fmts) / sizeof(uvc_fmts[0])) {
            strncpy((char *)fmt->description, uvc_fmts[i].name, sizeof(fmt->description) - 1);
        } else {
            strncpy((char *)fmt->description, uvc_fourcc_to_name(fmt->fourcc),
                    sizeof(fmt->description) - 1);
        }
        fmt->description[sizeof(fmt->description) - 1] = '\0';

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, fmt, sizeof(*fmt));
        }
        break;
    }
    case VIDIOC_ENUM_FRAME: {
        struct uvc_framedesc local_frame;
        struct uvc_framedesc *frame_desc;
        struct usbh_video_format *format = NULL;

        if (lwp_self() == NULL) {
            frame_desc = (struct uvc_framedesc *)args;
        } else {
            frame_desc = &local_frame;
            lwp_get_from_user(frame_desc, args, sizeof(*frame_desc));
        }

        format = uvc_find_format(video, frame_desc->fourcc);

        if (format == NULL) {
            USB_LOG_ERR("unmatch video format fourcc: %s %d\n", __func__, __LINE__);
            ret = -RT_EINVAL;
            goto out;
        }

        if (frame_desc->index >= format->num_of_frames) {
            ret = -RT_EINVAL;
            goto out;
        }

        frame_desc->width = format->frame[frame_desc->index].wWidth;
        frame_desc->height = format->frame[frame_desc->index].wHeight;
        frame_desc->defaultframeinterval = format->frame[frame_desc->index].dwDefaultFrameInterval;
        frame_desc->fourcc = format->fourcc;

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, frame_desc, sizeof(*frame_desc));
        }

        break;
    }
    case VIDIOC_ENUM_INTERVAL: {
        int i;
        struct uvc_fpsdesc local_fps;
        struct uvc_fpsdesc *fps_desc;
        struct usbh_video_format *format = NULL;
        struct usbh_video_frame *frame = NULL;

        if (lwp_self() == NULL) {
            fps_desc = (struct uvc_fpsdesc *)args;
        } else {
            fps_desc = &local_fps;
            lwp_get_from_user(fps_desc, args, sizeof(*fps_desc));
        }

        format = uvc_find_format(video, fps_desc->fourcc);

        if (format == NULL) {
            USB_LOG_ERR("unmatch video format fourcc: %s %d\n", __func__, __LINE__);
            ret = -RT_EINVAL;
            goto out;
        }

        for (i = 0; i < format->num_of_frames; i++) {
            if ((fps_desc->width == format->frame[i].wWidth) &&
                (fps_desc->height == format->frame[i].wHeight))
                frame = &format->frame[i];
        }

        if (frame == NULL) {
            USB_LOG_ERR("unmatch frame size: %ux%u\n",
                        fps_desc->width, fps_desc->height);
            ret = -RT_EINVAL;
            goto out;
        }

        if (frame->bFrameIntervalType) {
            if (fps_desc->index >= frame->bFrameIntervalType) {
                ret = -RT_EINVAL;
                goto out;
            }

            fps_desc->frameinterval = frame->dwFrameInterval[fps_desc->index];
            fps_desc->fourcc = format->fourcc;

        } else {
            USB_LOG_ERR("have no interval field ?: %s %d\n", __func__, __LINE__);
        }

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, fps_desc, sizeof(*fps_desc));
        }
        break;
    }
    case VIDIOC_S_FMT: {
        struct uvc_format local_fmt;
        struct uvc_format *fmt;
        uint16_t rw, rh;
        unsigned int d, maxd;
        int i;
        struct usbh_video_format *uvc_format = RT_NULL;
        struct usbh_video_frame *frame = RT_NULL;

        if (lwp_self() == NULL) {
            fmt = (struct uvc_format *)args;
        } else  {
            fmt = &local_fmt;
            lwp_get_from_user(fmt, args, sizeof(*fmt));
        }

        uvc_format = uvc_find_format(video, fmt->fourcc);
        if (uvc_format == RT_NULL) {
            USB_LOG_ERR("Unsupport video format fourcc: %s %d\n", __func__, __LINE__);
            ret = -RT_EINVAL;
            goto out;
        }

        /* Find the closest image size. The distance between image sizes is
         * the size in pixels of the non-overlapping regions between the
         * requested size and the frame-specified size.
         */
        rw = fmt->width;
        rh = fmt->height;
        maxd = (unsigned int)-1;

        for (i = 0; i < uvc_format->num_of_frames; ++i) {
            uint16_t w = uvc_format->frame[i].wWidth;
            uint16_t h = uvc_format->frame[i].wHeight;

            d = min(w, rw) * min(h, rh);
            d = w*h + rw*rh - 2*d;
            if (d < maxd) {
                maxd = d;
                frame = &uvc_format->frame[i];
            }

            if (maxd == 0)
                break;
        }

        if (frame == NULL) {
            USB_LOG_ERR("Unsupported size %ux%u.\n", fmt->width, fmt->height);
            ret = -RT_EINVAL;
            goto out;
        }

        for (i = 0; i < frame->bFrameIntervalType; i++) {
            if (fmt->frameinterval == frame->dwFrameInterval[i])
                break;
        }

        if (i >= frame->bFrameIntervalType) {
            USB_LOG_ERR("Unsupported interval %d in (%ux%u). use default interval = %d\n",
                        fmt->frameinterval, fmt->width, fmt->height, frame->dwDefaultFrameInterval);
            fmt->frameinterval = frame->dwDefaultFrameInterval;
        }

        fmt->width = frame->wWidth;
        fmt->height = frame->wHeight;
        fmt->fourcc = uvc_format->fourcc;

        video->current_format_type = uvc_format->format_type;
        video->current_fourcc = fmt->fourcc;
        video->current_frame.wWidth = frame->wWidth;
        video->current_frame.wHeight = frame->wHeight;
        video->current_frame.dwRequestFrameInterval = fmt->frameinterval;
        video->current_frame.dwMaxVideoFrameBufferSize = frame->dwMaxVideoFrameBufferSize;

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, fmt, sizeof(*fmt));
        }

        break;
    }
    case VIDIOC_REQBUFS: {
        int i = 0;
        struct uvc_requestbuffers local_request;
        struct uvc_requestbuffers *request;
        uint32_t buffer_size = video->current_frame.dwMaxVideoFrameBufferSize;

        if (lwp_self() == NULL) {
            request = (struct uvc_requestbuffers *)args;
        } else {
            request = &local_request;
            lwp_get_from_user(request, args, sizeof(*request));
        }

        if (uvc_queue.streaming || video->is_opened) {
            USB_LOG_ERR("reqbufs while stream is running\n");
            ret = -RT_EBUSY;
            goto out;
        }

        buffer_size = uvc_get_current_frame_size(video);
        buffer_size = USB_ALIGN_UP(buffer_size, CONFIG_USB_ALIGN_SIZE);

        USB_LOG_DBG("buffer_size = %d, dwMaxVideoFrameBufferSize = %d, count = %d\n",
                   buffer_size, video->current_frame.dwMaxVideoFrameBufferSize, request->count);
        if (request->count > MAX_UVC_BUFFER) {
            USB_LOG_ERR("Too many buffer required: %s %d\n", __func__, __LINE__);
            ret = -RT_EINVAL;
            goto out;
        }

        if (request->count == 0) {
            uvc_release_buffers();
            if (lwp_self() != NULL) {
                lwp_put_to_user(args, request, sizeof(*request));
            }
            break;
        }

        uvc_release_buffers();

#if VB_VERSION
        k_u32 pool_id;
        int alloc_size = buffer_size + VDEC_ALIGN_SIZE;

        USB_LOG_DBG("alloc_size = 0x%x\n", alloc_size);
        if (vb_create_pool(&pool_id, request->count + 1, alloc_size,
                           RT_NULL, "uvc", VB_REMAP_MODE_CACHED) != 0) {
            USB_LOG_ERR("Can't create pool: %s %d\n", __func__, __LINE__);
            ret = -RT_ENOMEM;
            goto release;
        }
        uvc_queue.buffer[0].pool_id = pool_id;
#else
        uvc_queue.mem = rt_malloc_align(buffer_size * (request->count + 1), CONFIG_USB_ALIGN_SIZE);
        if (uvc_queue.mem == RT_NULL) {
            USB_LOG_ERR("Can't alloc mem: %s %d\n", __func__, __LINE__);
            ret = -RT_ENOMEM;
            goto out;
        }
#endif

        rt_list_init(&uvc_queue.app_queue);
        rt_list_init(&uvc_queue.irq_queue);

        for (i = 0; i < request->count + 1; i ++) {
            uvc_queue.buffer[i].buf.index = i;
            uvc_queue.buffer[i].buf.length = buffer_size;
            uvc_queue.buffer[i].buf.offset = i * buffer_size;
            uvc_queue.buffer[i].buf.bytesused = 0;
            uvc_queue.buffer[i].state = VIDEOBUF_IDLE;
            uvc_queue.buffer[i].driver_use = false;
            uvc_buffer_profile_reset(&uvc_queue.buffer[i]);
            rt_wqueue_init(&uvc_queue.buffer[i].wait_queue);

#if VB_VERSION
            uvc_queue.buffer[i].handle =
                vb_get_blk_by_size_and_pool_id(pool_id, alloc_size, VB_UID_V_VI);
            if (uvc_queue.buffer[i].handle == VB_INVALID_HANDLE) {
                USB_LOG_ERR("Can't get buffer from pool: %s %d\n", __func__, __LINE__);
                ret = -RT_ENOMEM;
                goto release;
            }
            uvc_queue.buffer[i].blk_phys_addr =
                vb_blk_handle_to_phys(uvc_queue.buffer[i].handle);
            uvc_queue.buffer[i].blk_virt_addr =
                vb_blk_handle_to_kern(uvc_queue.buffer[i].handle);
            uvc_queue.buffer[i].phys_addr =
                ALIGN_UP(uvc_queue.buffer[i].blk_phys_addr, VDEC_ALIGN_SIZE);
            uvc_queue.buffer[i].virt_addr =
                (void *)ALIGN_UP((k_u64)uvc_queue.buffer[i].blk_virt_addr, VDEC_ALIGN_SIZE);
            uvc_queue.buffer[i].pool_id =
                vb_blk_handle_to_pool_id(uvc_queue.buffer[i].handle);
            USB_LOG_DBG("phys[%d] = 0x%lx, virt[%d] = 0x%p, pool_id = %d\n",
                       i, uvc_queue.buffer[i].phys_addr, i,
                       uvc_queue.buffer[i].virt_addr, uvc_queue.buffer[i].pool_id);
#endif
        }
        uvc_queue.buffer[i - 1].driver_use = true;

        uvc_queue.count = request->count;
        uvc_queue.buffer_size = buffer_size;

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, request, sizeof(*request));
        }
        break;

#if VB_VERSION
release:
        uvc_release_buffers();
        break;
#endif
    }
    case VIDIOC_QUERYBUF: {
        struct uvc_frame local_query_buf;
        struct uvc_frame *query_buf;

        if (lwp_self() == NULL) {
            query_buf = (struct uvc_frame *)args;
        } else {
            query_buf = &local_query_buf;
            lwp_get_from_user(query_buf, args, sizeof(*query_buf));
        }

        if (query_buf->index >= uvc_queue.count) {
            USB_LOG_ERR("index over range: %s %d\n", __func__, __LINE__);
            ret = -RT_EINVAL;
            goto out;
        }

        memcpy(query_buf, &uvc_queue.buffer[query_buf->index].buf, sizeof(struct uvc_frame));

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, query_buf, sizeof(*query_buf));
        }

        break;
    }

    case VIDIOC_BUFMMAP: {
        int i;
        void *addr;
        struct uvc_buffer *buffer;
        struct dfs_mmap2_args mmap_local;
        struct dfs_mmap2_args *mmap;
        void *pa, *va;

        if (lwp_self() == NULL) {
            mmap = (struct dfs_mmap2_args *)args;
        } else {
            mmap = &mmap_local;
            lwp_get_from_user(mmap, args, sizeof(*mmap));
        }

        for (i = 0; i < uvc_queue.count; i ++) {
            buffer = &uvc_queue.buffer[i];
            if (buffer->buf.offset == mmap->pgoffset) {
                break;
            }
        }

        if ((i == uvc_queue.count) || (mmap->length != uvc_queue.buffer_size)) {
            USB_LOG_ERR("video buf mismatch: %s %d\n", __func__, __LINE__);
            ret = -RT_ERROR;
            goto out;
        }

#if !VB_VERSION
        va = uvc_queue.mem + buffer->buf.offset;
        pa = rt_hw_mmu_v2p(&mmu_info, va);
#else
        va = buffer->virt_addr;
        pa = (void *)buffer->phys_addr;
#endif

        if (lwp_self() != NULL) {
            addr = lwp_map_user_phy(lwp_self(), RT_NULL, pa, mmap->length, 0);
            if (addr == 0) {
                USB_LOG_ERR("video mmap fail: %s %d\n", __func__, __LINE__);
                ret = -RT_ERROR;
                goto out;
            }
            mmap->addr = addr;
            buffer->buf.userptr = (char *)addr;
            lwp_put_to_user(args, mmap, sizeof(*mmap));
        } else {
            mmap->addr = va;
        }

        break;
    }
    case VIDIOC_QBUF: {
        struct uvc_frame local_q_buf;
        struct uvc_frame *q_buf;
        struct uvc_buffer *uvc_buf;

        if (lwp_self() == NULL) {
            q_buf = (struct uvc_frame *)args;
        } else {
            q_buf = &local_q_buf;
            lwp_get_from_user(q_buf, args, sizeof(*q_buf));
        }

        if (q_buf->index >= uvc_queue.count) {
            USB_LOG_ERR("index over range: %s %d\n", __func__, __LINE__);
            ret = -RT_EINVAL;
            goto out;
        }

        uvc_buf = &uvc_queue.buffer[q_buf->index];
        if (uvc_buf->state != VIDEOBUF_IDLE) {
            USB_LOG_ERR("mismatch state: %s %d\n", __func__, __LINE__);
            ret = -RT_EINVAL;
            goto out;
        }

        rt_base_t level;
        level = rt_hw_interrupt_disable();

        uvc_buf->state = VIDEOBUF_QUEUED;
        uvc_buf->buf.bytesused = 0;

        rt_list_insert_before(&uvc_queue.app_queue, &uvc_buf->stream);
        rt_list_insert_before(&uvc_queue.irq_queue, &uvc_buf->irq);

        rt_hw_interrupt_enable(level);
#if 0
        if (uvc_buf->buf.index == 0)
        {
            USB_LOG_ERR("q(put) ref = %d\n", vb_inquire_user_cnt(uvc_buf->handle));
        }
#endif

        /* todo: no need */
        if (lwp_self() != NULL) {
            lwp_put_to_user(args, q_buf, sizeof(*q_buf));
        }

        break;
    }
    case VIDIOC_DQBUF: {
        void *va;
        rt_base_t level;
        struct uvc_frame local_dq_buf;
        struct uvc_frame *dq_buf;
        struct uvc_buffer *uvc_buf;

        if (lwp_self() == NULL) {
            dq_buf = (struct uvc_frame *)args;
        } else {
            dq_buf = &local_dq_buf;
            lwp_get_from_user(dq_buf, args, sizeof(*dq_buf));
        }

        level = rt_hw_interrupt_disable();

        if (rt_list_isempty(&uvc_queue.app_queue)) {
            USB_LOG_ERR("queue is empty: %s %d\n", __func__, __LINE__);
            ret = -RT_EINVAL;
            rt_hw_interrupt_enable(level);
            goto out;
        }

        uvc_buf = rt_list_first_entry(&uvc_queue.app_queue, struct uvc_buffer, stream);
        rt_hw_interrupt_enable(level);

        switch (uvc_buf->state) {
        case VIDEOBUF_DONE:
            uvc_buf->state = VIDEOBUF_IDLE;
            break;
        case VIDEOBUF_ERROR:
        case VIDEOBUF_IDLE:
        case VIDEOBUF_QUEUED:
        case VIDEOBUF_ACTIVE:
        default:
            ret = -RT_EINVAL;
            goto out;
        }

        memset(dq_buf, 0x0, sizeof(struct uvc_frame));
        memcpy(dq_buf, &uvc_buf->buf, sizeof(struct uvc_frame));

#if VB_VERSION
        if (video->current_format_type == USBH_VIDEO_FORMAT_UNCOMPRESSED) {
            k_pixel_format pixel_fmt = uvc_fourcc_to_kpixel(video->current_fourcc);
            uint32_t width = video->current_frame.wWidth;
            uint32_t height = video->current_frame.wHeight;
            if (pixel_fmt == PIXEL_FORMAT_BUTT) {
                USB_LOG_ERR("unsupport uncompressed fourcc: 0x%08x\n", video->current_fourcc);
                pixel_fmt = PIXEL_FORMAT_YUYV_PACKAGE_422;
            }
            dq_buf->v_info.v_frame.width = width;
            dq_buf->v_info.v_frame.height = height;

            if (pixel_fmt == PIXEL_FORMAT_YUYV_PACKAGE_422 ||
                pixel_fmt == PIXEL_FORMAT_UYVY_PACKAGE_422) {
                dq_buf->v_info.v_frame.stride[0] = width * 2;
            } else if (pixel_fmt == PIXEL_FORMAT_YUV_SEMIPLANAR_420) {
                dq_buf->v_info.v_frame.stride[0] = width;
                dq_buf->v_info.v_frame.stride[1] = width;
                dq_buf->v_info.v_frame.phys_addr[1] = uvc_buf->phys_addr + (width * height);
                dq_buf->v_info.v_frame.virt_addr[1] = (uint64_t)uvc_buf->virt_addr + (width * height);
            } else if (pixel_fmt == PIXEL_FORMAT_YVU_PLANAR_420) {
                dq_buf->v_info.v_frame.stride[0] = width;
                dq_buf->v_info.v_frame.stride[1] = width / 2;
                dq_buf->v_info.v_frame.stride[2] = width / 2;
                dq_buf->v_info.v_frame.phys_addr[1] = uvc_buf->phys_addr + (width * height);
                dq_buf->v_info.v_frame.phys_addr[2] = dq_buf->v_info.v_frame.phys_addr[1] + (width * height / 4);
                dq_buf->v_info.v_frame.virt_addr[1] = (uint64_t)uvc_buf->virt_addr + (width * height);
                dq_buf->v_info.v_frame.virt_addr[2] = dq_buf->v_info.v_frame.virt_addr[1] + (width * height / 4);
            } else {
                dq_buf->v_info.v_frame.stride[0] = width;
            }
            dq_buf->v_info.v_frame.pixel_format = pixel_fmt;
            dq_buf->v_info.v_frame.phys_addr[0] = uvc_buf->phys_addr;
            dq_buf->v_info.v_frame.virt_addr[0] = (uint64_t)uvc_buf->virt_addr;

            dq_buf->v_info.mod_id = K_ID_VO;
            dq_buf->v_info.pool_id = uvc_buf->pool_id;
        } else if (video->current_format_type == USBH_VIDEO_FORMAT_MJPEG) {
            dq_buf->v_stream.len = uvc_buf->buf.bytesused;
            dq_buf->v_stream.phy_addr = uvc_buf->phys_addr;
            /* todo */
            dq_buf->v_stream.end_of_stream = 0;

            dq_buf->v_stream.pts = cpu_ticks_us();
        } else {
            USB_LOG_ERR("Unsupport format %s %d\n", __func__, __LINE__);
        }

        va = uvc_buf->virt_addr;
#else
        va = uvc_queue.mem + uvc_buf->buf.offset;
        dq_buf->v_stream.len = dq_buf->bytesused;
#endif

        rt_hw_cpu_dcache_clean((void *)va, dq_buf->bytesused ? dq_buf->bytesused : dq_buf->length);

        level = rt_hw_interrupt_disable();
        rt_list_remove(&uvc_buf->stream);
        rt_hw_interrupt_enable(level);

#if 0
        if (uvc_buf->buf.index == 0)
        {
            USB_LOG_ERR("dq(get) ref = %d\n", vb_inquire_user_cnt(uvc_buf->handle));
        }
#endif

        if (lwp_self() != NULL) {
            lwp_put_to_user(args, dq_buf, sizeof(*dq_buf));
        }

        break;
    }
    case VIDIOC_STREAMON: {
        if (uvc_queue.streaming) {
            ret = -RT_EBUSY;
            break;
        }
        if ((uvc_queue.count <= 0) || (uvc_queue.buffer_size <= 0) ||
            rt_list_isempty(&uvc_queue.irq_queue)) {
            USB_LOG_ERR("stream on without prepared/qbuf buffers\n");
            ret = -RT_EINVAL;
            break;
        }
        ret = usbh_video_on(video);
        break;
    }
    case VIDIOC_STREAMOFF:
        usbh_video_off(video);
        break;
    default:
        USB_LOG_ERR("Unsupport cmd %s\n", __func__);
        ret = -RT_EINVAL;
    }

out:
    return ret;
}

const static struct rt_device_ops uvc_ops =
{
    RT_NULL,
    uvc_open,
    uvc_close,
    uvc_read,
    uvc_write,
    uvc_control
};

void usbh_video_run(struct usbh_video *video_class)
{
    int ret;
    struct rt_device *device;
    const char *dev_name = video_class->hport->config.intf[video_class->ctrl_intf].devname;

    if (io_lock() != RT_EOK) {
        USB_LOG_ERR("set disconnect flag fail in %s\n", __func__);
        return;
    }
    uvc_queue.disconnect = RT_FALSE;
    io_unlock();
    device = &(video_class->device);
    device->type = RT_Device_Class_Char;
    device->rx_indicate = RT_NULL;
    device->tx_complete = RT_NULL;
    device->ops = &uvc_ops;

    ret = rt_device_register(device, dev_name, RT_DEVICE_FLAG_RDWR);
    if (ret) {
        USB_LOG_ERR("%s register fail\n", dev_name);
    }

    device->fops = &uvc_fops;

    USB_LOG_DBG("register %s\n", dev_name);

}

void usbh_video_stop(struct usbh_video *video_class)
{
    if (io_lock() == RT_EOK) {
        uvc_queue.disconnect = RT_TRUE;
        io_unlock();
    } else {
        USB_LOG_ERR("set disconnect flag fail in %s\n", __func__);
    }

    usbh_video_off(video_class);

    for (int i = 0; i < uvc_queue.count; i++) {
        uvc_queue.buffer[i].state = VIDEOBUF_ERROR;
        rt_wqueue_wakeup(&uvc_queue.buffer[i].wait_queue, 0);
    }

    rt_device_unregister(&video_class->device);
}
