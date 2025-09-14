/* Copyright (c) 2025, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdint.h>
#include <sys/ioctl.h>

#include "rtdef.h"
#include "rtservice.h"
#include "rtthread.h"

#include "cache.h"
#include "tick.h"

#include "lwp.h"
#include "lwp_user_mm.h"

#include "comm/k_vb_comm.h"

#include "usbd_desc.h"

#define DBG_TAG "usbd_video"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#if defined(CHERRY_USB_DEVICE_FUNC_UVC)

///////////////////////////////////////////////////////////////////////////////
// Private Varibales //////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
static volatile bool tx_flag     = 0;
static volatile bool iso_tx_busy = false;

///////////////////////////////////////////////////////////////////////////////
// UVC Device Functions ///////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
#define USBD_VIDEO_MAX_BUFFER_CNT    (64)
#define USBD_VIDEO_THREAD_STACK_SIZE (8 * 1024)
#define USBD_VIDEO_THR_STREAM_FLAG   (0x01)

#define IOCTL_USBD_VIDEO_CREATE_POOL _IOW('v', 0x01, void*)
#define IOCTL_USBD_VIDEO_GET_BUFFER  _IOW('v', 0x02, void*)
#define IOCTL_USBD_VIDEO_PUT_BUFFER  _IOW('v', 0x03, void*)
#define IOCTL_USBD_VIDEO_CONFIGURE   _IOW('v', 0x04, void*)
#define IOCTL_USBD_VIDEO_STREAM_ON   _IOW('v', 0x05, void*)
#define IOCTL_USBD_VIDEO_STREAM_OFF  _IOW('v', 0x06, void*)
#define IOCTL_USBD_VIDEO_DEV_STATE   _IOR('v', 0x07, int*)

extern k_s32           vb_create_pool(k_u32* pool_id, k_u32 blk_cnt, k_u64 blk_size, const char* mmz_name, const char* buf_name,
                                      k_vb_remap_mode vb_remap_mode);
extern k_s32           vb_destroy_pool(k_u32 pool_id);
extern k_vb_blk_handle vb_get_blk_by_size_and_pool_id(k_u32 pool_id, k_u64 blk_size, k_u32 uid);
extern k_s32           vb_put_blk(k_u32 pool_id, k_u64 phys_addr);
extern k_u64           vb_blk_handle_to_phys(k_vb_blk_handle handle);
extern void*           vb_blk_handle_to_kern(k_vb_blk_handle handle);
extern k_s32           vb_user_sub(k_u32 pool_id, k_u64 phys_addr, k_u32 uid);
extern k_s32           vb_inquire_user_cnt(k_vb_blk_handle handle);

struct usbd_video_frame_t {
    uint32_t handle;
    uint32_t pool_id;
    uint64_t phys_addr;
    void*    k_addr;
    void*    u_addr; // we suppose only one user

    uint32_t data_length;

    rt_list_t frame, free, ready;
};

struct usbd_video_inst_t {
    struct rt_device device;

    struct rt_mutex mutex;
    struct rt_event event;
    rt_thread_t     thread;

    int      frame_rate;
    uint32_t buffer_cnt;
    uint32_t buffer_size;
    uint32_t buffer_pool_id;

    int stream_state; // 0: off, 1: on, 2: exit

    rt_list_t frame_list, free_list, ready_list;
};

///////////////////////////////////////////////////////////////////////////////
// IOCTL Functions ////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
struct usbd_video_create_pool_cfg_t {
    uint32_t buffer_size;
    uint32_t buffer_count;
};

struct usbd_video_buffer_wrap_t {
    void*    user_buffer;
    uint32_t buffer_size;
};

struct uvc_device_conf_t {
    int frame_rate;
};

static inline __attribute__((always_inline)) void _usbd_video_release_pool(struct usbd_video_inst_t* inst)
{
    struct usbd_video_frame_t *frame, *temp_frame;

    k_u32 pool_id = inst->buffer_pool_id;

    // remove all frames, and destroy pool
    rt_list_for_each_entry_safe(frame, temp_frame, &inst->frame_list, frame)
    {
        if (0x00 != vb_user_sub(frame->pool_id, frame->phys_addr, VB_UID_V_VI)) {
            LOG_E("Put vb block failed");
        }

        rt_list_remove(&frame->frame); /* unlink from list */
        rt_free(frame); /* release memory */
    }

    rt_list_init(&inst->free_list);
    rt_list_init(&inst->ready_list);

    if (VB_INVALID_POOLID != pool_id) {
        vb_destroy_pool(pool_id);
    }
    inst->buffer_pool_id = VB_INVALID_POOLID;
}

static inline __attribute__((always_inline)) rt_err_t _usbd_video_ioctl_create_pool(struct usbd_video_inst_t* inst, void* args)
{
    rt_err_t ret;

    k_u32 pool_id, pool_blk_size;

    struct usbd_video_create_pool_cfg_t cfg;

    struct usbd_video_frame_t* frame;

    rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);

    if (0x00 != inst->stream_state) {
        LOG_E("Stream is on, can not Create Pool");

        ret = RT_ERROR;
        goto _exit;
    }

    if (0x00 != LWP_GET_FROM_USER(&cfg, args, struct usbd_video_create_pool_cfg_t)) {
        ret = RT_ERROR;
        goto _exit;
    }

    if (VB_INVALID_POOLID != inst->buffer_pool_id) {
        LOG_W("Already Create Pool");

        if ((cfg.buffer_count == inst->buffer_cnt) && (cfg.buffer_size == inst->buffer_size)) {
            LOG_D("Same Buffer configure");

            ret = RT_EOK;
            goto _exit;
        } else {
            LOG_W("Recreate Buffer Pool");

            _usbd_video_release_pool(inst);
        }
    }

    if (cfg.buffer_count > USBD_VIDEO_MAX_BUFFER_CNT) {
        LOG_E("buffer count too large");

        ret = RT_ERROR;
        goto _exit;
    }

    if (!rt_list_isempty(&inst->frame_list)) {
        LOG_E("frame list not empty");

        ret = RT_ERROR;
        goto _exit;
    }

    if (!rt_list_isempty(&inst->free_list)) {
        LOG_E("free list not empty");

        ret = RT_ERROR;
        goto _exit;
    }

    if (!rt_list_isempty(&inst->ready_list)) {
        LOG_E("ready list not empty");

        ret = RT_ERROR;
        goto _exit;
    }

    // clear
    inst->buffer_cnt     = 0;
    inst->buffer_size    = 0;
    inst->buffer_pool_id = VB_INVALID_POOLID;

    pool_blk_size = VB_ALIGN_UP(cfg.buffer_size, 1024);

    if ((0x00 != vb_create_pool(&pool_id, cfg.buffer_count, pool_blk_size, NULL, "uvcd", VB_REMAP_MODE_NOCACHE))
        || (VB_INVALID_POOLID == pool_id)) {
        LOG_E("Can't create pool");

        ret = RT_ERROR;
        goto _exit;
    }

    for (int i = 0; i < cfg.buffer_count; i++) {
        if (NULL == (frame = malloc(sizeof(struct usbd_video_frame_t)))) {
            LOG_E("No memory");

            ret = RT_ENOMEM;
            goto _error;
        }

        if (VB_INVALID_HANDLE == (frame->handle = vb_get_blk_by_size_and_pool_id(pool_id, pool_blk_size, VB_UID_V_VI))) {
            LOG_E("No blk");

            rt_free(frame);

            ret = RT_ENOMEM;
            goto _error;
        }

        frame->pool_id   = pool_id;
        frame->phys_addr = vb_blk_handle_to_phys(frame->handle);
        frame->k_addr    = vb_blk_handle_to_kern(frame->handle);
        frame->u_addr    = NULL;

        LOG_D("phys_addr %p, k_addr %p\n", frame->phys_addr, frame->k_addr);

        rt_list_init(&frame->frame);
        rt_list_init(&frame->free);
        rt_list_init(&frame->ready);

        rt_list_insert_after(&inst->frame_list, &frame->frame);
        rt_list_insert_after(&inst->free_list, &frame->free);
    }

    inst->buffer_cnt     = cfg.buffer_count;
    inst->buffer_size    = cfg.buffer_size;
    inst->buffer_pool_id = pool_id;

    rt_mutex_release(&inst->mutex);

    return RT_EOK;

_error:
    _usbd_video_release_pool(inst);

_exit:
    rt_mutex_release(&inst->mutex);

    return ret;
}

static inline __attribute__((always_inline)) rt_err_t _usbd_video_ioctl_get_buffer(struct usbd_video_inst_t* inst, void* args)
{
    rt_err_t ret = RT_EOK;

    struct usbd_video_buffer_wrap_t buffer;
    struct usbd_video_frame_t*      frame = NULL;

    rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);

    if (!rt_list_isempty(&inst->free_list)) {
        frame = rt_list_first_entry(&inst->free_list, struct usbd_video_frame_t, free);

        rt_list_remove(&frame->free);

        if (NULL != lwp_self()) {
            if (NULL == frame->u_addr) {
                frame->u_addr = lwp_map_user_phy(lwp_self(), RT_NULL, (void*)frame->phys_addr, inst->buffer_size, 0);
                if (NULL == frame->u_addr) {
                    LOG_E("mmap failed");
                    ret = RT_ERROR;

                    rt_list_insert_after(&inst->free_list, &frame->free);
                }
            }
            buffer.user_buffer = frame->u_addr;
        } else {
            buffer.user_buffer = frame->k_addr;
        }
        buffer.buffer_size = inst->buffer_size;

        LOG_D("user_buffer %p, u_addr %p, k_addr %p\n", buffer.user_buffer, frame->u_addr, frame->k_addr);
    } else {
        buffer.buffer_size = 0;
        buffer.user_buffer = 0;
    }

    if (LWP_PUT_TO_USER(args, &buffer, struct usbd_video_buffer_wrap_t)) {
        ret = RT_ERROR;
    }

    rt_mutex_release(&inst->mutex);

    return ret;
}

static inline __attribute__((always_inline)) rt_err_t _usbd_video_ioctl_put_buffer(struct usbd_video_inst_t* inst, void* args)
{
    rt_err_t ret = RT_EOK;

    struct usbd_video_buffer_wrap_t buffer;
    struct usbd_video_frame_t*      frame = NULL;

    rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);

    if (0x00 == LWP_GET_FROM_USER(&buffer, args, struct usbd_video_buffer_wrap_t)) {
        LOG_D("Received buffer with size: %d bytes\n", buffer.buffer_size);

        if (buffer.buffer_size > inst->buffer_size) {
            LOG_E("Invalid buffer size: %u (max: %u)", buffer.buffer_size, inst->buffer_size);
            buffer.buffer_size = 0;
        }

        rt_list_for_each_entry(frame, &inst->frame_list, frame)
        {
            if (NULL != lwp_self()) {
                if (frame->u_addr != buffer.user_buffer) {
                    continue;
                }
            } else {
                if (frame->k_addr == buffer.user_buffer) {
                    continue;
                }
            }

            frame->data_length = buffer.buffer_size;
            if (buffer.buffer_size) {
                rt_list_insert_after(&inst->ready_list, &frame->ready);
            } else {
                rt_list_insert_after(&inst->free_list, &frame->free);
            }
            break;
        }

        // wake up thread
        rt_event_send(&inst->event, USBD_VIDEO_THR_STREAM_FLAG);
    } else {
        ret = RT_ERROR;
    }

    rt_mutex_release(&inst->mutex);

    return ret;
}

static inline __attribute__((always_inline)) rt_err_t _usbd_video_ioctl_configure(struct usbd_video_inst_t* inst, void* args)
{
    struct uvc_device_conf_t cfg;

    if (0x00 != LWP_GET_FROM_USER(&cfg, args, struct uvc_device_conf_t)) {
        return RT_ERROR;
    }

    rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);

    inst->frame_rate = cfg.frame_rate;

    rt_mutex_release(&inst->mutex);

    return RT_EOK;
}

static inline __attribute__((always_inline)) rt_err_t _usbd_video_ioctl_stream_on_off(struct usbd_video_inst_t* inst,
                                                                                      void*                     args)
{
    int on_off = (void*)0 == args ? 0 : 1;

    rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);

    inst->stream_state = on_off;

    rt_mutex_release(&inst->mutex);

    return RT_EOK;
}

static inline __attribute__((always_inline)) rt_err_t _usbd_video_ioctl_dev_state(struct usbd_video_inst_t* inst, void* args)
{
    int _state = tx_flag;

    if (0x00 == LWP_PUT_TO_USER(args, &_state, int)) {
        return RT_EOK;
    }

    return RT_ERROR;
}

static rt_err_t _usbd_video_dev_control(rt_device_t dev, int cmd, void* args)
{
    rt_err_t ret  = RT_ERROR;
    uint32_t _cmd = (uint32_t)cmd;

    struct usbd_video_inst_t* inst = (struct usbd_video_inst_t*)dev; // struct rt_device device is first

    switch (_cmd) {
    case IOCTL_USBD_VIDEO_CREATE_POOL: {
        ret = _usbd_video_ioctl_create_pool(inst, args);
    } break;
    case IOCTL_USBD_VIDEO_GET_BUFFER: {
        ret = _usbd_video_ioctl_get_buffer(inst, args);
    } break;
    case IOCTL_USBD_VIDEO_PUT_BUFFER: {
        ret = _usbd_video_ioctl_put_buffer(inst, args);
    } break;
    case IOCTL_USBD_VIDEO_CONFIGURE: {
        ret = _usbd_video_ioctl_configure(inst, args);
    } break;
    case IOCTL_USBD_VIDEO_STREAM_ON: {
        ret = _usbd_video_ioctl_stream_on_off(inst, (void*)1);
    } break;
    case IOCTL_USBD_VIDEO_STREAM_OFF: {
        ret = _usbd_video_ioctl_stream_on_off(inst, (void*)0);
    } break;
    case IOCTL_USBD_VIDEO_DEV_STATE: {
        ret = _usbd_video_ioctl_dev_state(inst, args);
    } break;
    default: {
        ret = RT_EINVAL;
        LOG_E("Unsupport cmd 0x%08X", _cmd);
    } break;
    }

    return ret;
}

/**
 * @brief Fills the 12-byte UVC payload header for a single packet.
 *
 * @param header_buffer Pointer to a 12-byte buffer where the header will be written.
 * @param packet_index The index of the current packet (0-based).
 * @param total_packets The total number of packets for the frame.
 * @param frame_counter A counter for the video frame number.
 * @param payload_size The size of the image data in this specific packet.
 */
static void video_payload_header_fill(uint8_t* header_buffer, uint32_t packet_index, uint32_t total_packets,
                                      uint32_t frame_counter, uint32_t payload_size)
{
    static uint32_t presentation_time = 0;

    header_buffer[0] = 0x0C; // bHeaderLength: 12 bytes
    header_buffer[1] = 0x00; // bmHeaderInfo: Clear all flags initially

    // Set Frame ID (FID) bit - alternates for each new frame
    header_buffer[1] |= (frame_counter & 0x01);

    // Set End of Frame (EOF) bit for the last packet
    if (packet_index == (total_packets - 1)) {
        header_buffer[1] |= (1 << 1); // Set EOF bit
    }

    // On the very first packet of a new frame, update presentation time
    if (packet_index == 0) {
        // 333333 units of 100ns = 33.3333 ms (~30 FPS)
        presentation_time += 333333;
    }

    // dwPresentationTime (4 bytes, little-endian)
    header_buffer[2] = (uint8_t)(presentation_time >> 0);
    header_buffer[3] = (uint8_t)(presentation_time >> 8);
    header_buffer[4] = (uint8_t)(presentation_time >> 16);
    header_buffer[5] = (uint8_t)(presentation_time >> 24);

    // dwSourceClockReference is optional, can be zero or a simple time source
    header_buffer[6]  = 0;
    header_buffer[7]  = 0;
    header_buffer[8]  = 0;
    header_buffer[9]  = 0;
    header_buffer[10] = 0;
    header_buffer[11] = 0;
}

static void _usbd_video_thread_entry(void* args)
{
    uint32_t                   event;
    uint32_t                   frame_rate, frame_sleep_ms;
    struct usbd_video_frame_t* frame         = NULL;
    struct usbd_video_inst_t*  inst          = (struct usbd_video_inst_t*)args;
    static uint32_t            frame_counter = 0;

    // Allocate two packet buffers for ping-pong
    uint8_t* buffer = rt_malloc_align(MAX_PAYLOAD_SIZE * 2, RT_CPU_CACHE_LINE_SZ);
    RT_ASSERT(buffer);

    uint8_t* packet_buffer[2];
    packet_buffer[0] = &buffer[0];
    packet_buffer[1] = &buffer[MAX_PAYLOAD_SIZE];

    while (1) {
        if (RT_EOK
            != rt_event_recv(&inst->event, USBD_VIDEO_THR_STREAM_FLAG, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                             rt_tick_from_millisecond(100), &event)) {
            continue;
        }

        while (1) {
            rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);

            if (0x00 == inst->stream_state) {
                rt_mutex_release(&inst->mutex);
                break;
            }

            if (!rt_list_isempty(&inst->ready_list)) {
                frame = rt_list_first_entry(&inst->ready_list, struct usbd_video_frame_t, ready);
                rt_list_remove(&frame->ready);
            } else {
                rt_mutex_release(&inst->mutex);
                rt_thread_mdelay(2); // Wait briefly for a new frame
                break;
            }

            frame_rate = inst->frame_rate;

            rt_mutex_release(&inst->mutex);

            frame_sleep_ms = 1000 / frame_rate;

            if (10 > frame_sleep_ms) {
                frame_sleep_ms = 10;
            }
            if (100 < frame_sleep_ms) {
                frame_sleep_ms = 100;
            }

            uint64_t frame_send_time_ms = cpu_ticks_ms();

            if (frame && tx_flag) {
                uint8_t* input_data   = (uint8_t*)frame->k_addr;
                uint32_t input_len    = frame->data_length;
                uint32_t bytes_sent   = 0;
                uint32_t buffer_index = 0;

                // Max image data per packet (leaving space for 12-byte header)
                const uint32_t data_per_packet = MAX_PAYLOAD_SIZE - 12;
                const uint32_t total_packets   = (input_len + data_per_packet - 1) / data_per_packet;

                // Pre-fill the first packet
                uint32_t bytes_to_copy
                    = (bytes_sent + data_per_packet > input_len) ? (input_len - bytes_sent) : data_per_packet;
                video_payload_header_fill(packet_buffer[buffer_index], 0, total_packets, frame_counter, bytes_to_copy);
                memcpy(packet_buffer[buffer_index] + 12, input_data + bytes_sent, bytes_to_copy);
                bytes_sent += bytes_to_copy;

                // Loop through and send all packets
                for (uint32_t i = 0; i < total_packets; i++) {
                    if (!tx_flag) {
                        break;
                    }

                    // Wait for the previous packet to finish transmission
                    while (iso_tx_busy && tx_flag) {
                        // rt_thread_mdelay(1);
                        asm volatile("wfi");
                    }

                    // Start transmission of the current buffer
                    uint32_t total_packet_size = 12 + bytes_to_copy;
                    iso_tx_busy                = true; // Set busy flag before starting transmission
                    usbd_ep_start_write(USB_DEVICE_BUS_ID, VIDEO_IN_EP, packet_buffer[buffer_index], total_packet_size);

                    // Switch to the other buffer
                    buffer_index = 1 - buffer_index;

                    // Prepare the next packet's data and header
                    if (i < total_packets - 1) {
                        bytes_to_copy = (bytes_sent + data_per_packet > input_len) ? (input_len - bytes_sent) : data_per_packet;
                        video_payload_header_fill(packet_buffer[buffer_index], i + 1, total_packets, frame_counter,
                                                  bytes_to_copy);
                        memcpy(packet_buffer[buffer_index] + 12, input_data + bytes_sent, bytes_to_copy);
                        bytes_sent += bytes_to_copy;
                    }
                }

                // Wait for the last packet to finish
                // while (iso_tx_busy && tx_flag) {
                //     // rt_thread_mdelay(1);
                //     asm volatile("wfi");
                // }

                frame_counter++;
            }
            frame_send_time_ms = cpu_ticks_ms() - frame_send_time_ms;

            // Put the processed frame back into the free list
            rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);
            rt_list_insert_after(&inst->free_list, &frame->free);
            rt_mutex_release(&inst->mutex);

            if (frame_sleep_ms >= (uint32_t)frame_send_time_ms) {
                uint32_t sleep_ms = (frame_sleep_ms - frame_send_time_ms) - 1;

                if (10 > sleep_ms) {
                    sleep_ms = 10;
                }
                if (100 < sleep_ms) {
                    sleep_ms = 100;
                }

                if (sleep_ms) {
                    rt_thread_mdelay(sleep_ms);
                }
            }
        }
    }

    rt_kprintf("%s exit\n", __FUNCTION__);

    rt_free_align(buffer);
}

static rt_err_t _usbd_video_dev_init(rt_device_t dev)
{
    (void)dev;

    return RT_EOK;
}

static rt_err_t _usbd_video_dev_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;

    return RT_EOK;
}

static rt_err_t _usbd_video_dev_close(rt_device_t dev)
{
    struct usbd_video_frame_t *frame = NULL, *temp_frame = NULL;
    struct usbd_video_inst_t*  inst  = (struct usbd_video_inst_t*)dev;
    int                        count = 0;

    void* addresses_to_unmap[USBD_VIDEO_MAX_BUFFER_CNT] = { 0 };

    rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);
    inst->stream_state = 0;
    rt_event_send(&inst->event, USBD_VIDEO_THR_STREAM_FLAG);
    rt_mutex_release(&inst->mutex);
    rt_thread_mdelay(10);

    rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);

    if (NULL != lwp_self()) {
        rt_list_for_each_entry(frame, &inst->frame_list, frame)
        {
            if (NULL != frame->u_addr) {
                addresses_to_unmap[count++] = frame->u_addr;
                frame->u_addr               = NULL;
            }
        }
    }
    rt_mutex_release(&inst->mutex);

    for (int i = 0; i < count; i++) {
        lwp_munmap(addresses_to_unmap[i]);
    }

    rt_mutex_take(&inst->mutex, RT_WAITING_FOREVER);

    rt_list_for_each_entry_safe(frame, temp_frame, &inst->frame_list, frame)
    {
        vb_user_sub(inst->buffer_pool_id, frame->phys_addr, VB_UID_V_VI);

        rt_list_remove(&frame->frame);
        rt_list_remove(&frame->free);
        rt_list_remove(&frame->ready);
    }

    if (VB_INVALID_POOLID != inst->buffer_pool_id) {
        vb_destroy_pool(inst->buffer_pool_id);
        inst->buffer_pool_id = VB_INVALID_POOLID;
    }

    rt_mutex_release(&inst->mutex);

    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops usbd_video_dev_ops = {
    _usbd_video_dev_init, // rt_err_t  (*init)   (rt_device_t dev);
    _usbd_video_dev_open, // rt_err_t  (*open)   (rt_device_t dev, rt_uint16_t oflag);
    _usbd_video_dev_close, // rt_err_t  (*close)  (rt_device_t dev);
    RT_NULL, // rt_size_t (*read)   (rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);
    RT_NULL, // rt_size_t (*write)  (rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size);
    _usbd_video_dev_control, // rt_err_t  (*control)(rt_device_t dev, int cmd, void *args);
};
#endif

static void usbd_uvc_device_init(void)
{
    rt_err_t err = RT_EOK;

    static rt_int8_t                _init_flag = 0;
    static struct usbd_video_inst_t usbd_video_dev;

    if (_init_flag == 0) {
        rt_memset(&usbd_video_dev, 0, sizeof(struct usbd_video_inst_t));
#ifdef RT_USING_DEVICE_OPS
        usbd_video_dev.device.ops = &usbd_video_dev_ops;
#else
        usbd_video_dev.device.init    = _usbd_video_dev_init;
        usbd_video_dev.device.open    = RT_NULL;
        usbd_video_dev.device.close   = RT_NULL;
        usbd_video_dev.device.read    = RT_NULL;
        usbd_video_dev.device.write   = RT_NULL;
        usbd_video_dev.device.control = _usbd_video_dev_control;
#endif
        usbd_video_dev.device.user_data = NULL;
        usbd_video_dev.device.type      = RT_Device_Class_Miscellaneous;

        usbd_video_dev.frame_rate     = 30; // default 30 fps
        usbd_video_dev.buffer_cnt     = 5;
        usbd_video_dev.buffer_size    = 1024 * 1024; // 1MiB
        usbd_video_dev.buffer_pool_id = VB_INVALID_POOLID;

        usbd_video_dev.stream_state = 0; // not stream on

        rt_list_init(&usbd_video_dev.frame_list);
        rt_list_init(&usbd_video_dev.free_list);
        rt_list_init(&usbd_video_dev.ready_list);

        rt_event_init(&usbd_video_dev.event, "usbd_video_dev", RT_IPC_FLAG_FIFO);
        rt_mutex_init(&usbd_video_dev.mutex, "usbd_video_dev", RT_IPC_FLAG_FIFO);

        usbd_video_dev.thread = rt_thread_create("usbd_video_dev", _usbd_video_thread_entry, &usbd_video_dev,
                                                 USBD_VIDEO_THREAD_STACK_SIZE, 20, 10);
        if (NULL == usbd_video_dev.thread) {
            LOG_E("Create thread failed");
            return;
        }

        if (RT_EOK
            != (err = rt_device_register(&usbd_video_dev.device, "video", RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_STANDALONE))) {
            LOG_E("usbd_video_dev register failed, %d", errno);
            return;
        }

        rt_thread_startup(usbd_video_dev.thread);
    }
}

///////////////////////////////////////////////////////////////////////////////
// USB Stack Functions ////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void usbd_video_open(uint8_t busid, uint8_t intf)
{
    tx_flag     = 1;
    iso_tx_busy = false;

    USB_LOG_RAW("OPEN\r\n");
}

void usbd_video_close(uint8_t busid, uint8_t intf)
{
    tx_flag     = 0;
    iso_tx_busy = false;

    USB_LOG_RAW("CLOSE\r\n");
}

void canmv_usb_device_uvc_on_connected(void)
{
    tx_flag     = 0;
    iso_tx_busy = false;
}

static void usbd_video_iso_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    // USB_LOG_RAW("actual in len:%d\r\n", nbytes);
    iso_tx_busy = false;
}

static struct usbd_interface intf0, intf1;

static struct usbd_endpoint video_in_ep = {
    .ep_cb   = usbd_video_iso_callback,
    .ep_addr = VIDEO_IN_EP,
};

void canmv_usb_device_uvc_init(void)
{
    rt_thread_t tid;
    uint8_t     busid = USB_DEVICE_BUS_ID;

    usbd_add_interface(busid, usbd_video_init_intf(busid, &intf0, INTERVAL, MAX_FRAME_SIZE, MAX_PAYLOAD_SIZE));
    usbd_add_interface(busid, usbd_video_init_intf(busid, &intf1, INTERVAL, MAX_FRAME_SIZE, MAX_PAYLOAD_SIZE));
    usbd_add_endpoint(busid, &video_in_ep);

    usbd_uvc_device_init();
}

#endif
