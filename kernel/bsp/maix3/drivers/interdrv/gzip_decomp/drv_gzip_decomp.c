/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
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
#include <errno.h>
#include <rtthread.h>

#include <ioremap.h>
#include <lwp_user_mm.h>

#include "cache.h"
#include "riscv_io.h"

#include "sysctl_clk.h"
#include "sysctl_rst.h"
#include "k230_pm.h"

#include "drv_gzip_decomp.h"

#define DBG_LVL DBG_LOG
#define DBG_COLOR
#define DBG_TAG "gzip_decomp"
#include <rtdbg.h>

#ifndef PV_OFFSET
#define PV_OFFSET 0x0
#endif

/* Linked from the MPP kernel module. Hand-written mirrors of mpp
 * kernel/ext_inc/gsdma_ext.h and vb_ext.h: keep the signatures in sync.
 * vb_dma_buf_put(NULL) is a no-op. */
extern int  gsdma_sdma_reserve_channel_wait(uint8_t ch_num, int32_t timeout_ms);
extern int  gsdma_sdma_abort_channel(uint8_t ch_num);
extern void gsdma_sdma_release_channel(uint8_t ch_num);
extern int  gsdma_sdma_ch_submit(uint8_t ch_num, uint32_t ch_cfg_value, uint32_t llt_phys_addr,
                                 void (*callback)(void* arg), void* arg);
extern void* vb_dma_buf_get(uint64_t phys_addr, uint64_t len);
extern void  vb_dma_buf_put(void* buf);

/* ---- Hardware constants ---- */
#define UGZIP_BASE 0x80808000UL
#define UGZIP_SIZE 0x1C

#define ZIP_LINE_SIZE (128 * 1024) /* 128KB per SDMA LLI node */
#define ZIP_RD_CH     0 /* SDMA CH0: read (src DDR → SRAM) */
#define ZIP_WR_CH     1 /* SDMA CH1: write (SRAM → dst DDR) */

/* Read-channel ch_cfg: dec_en=1 (bit10) for gzip hardware handshake */
#define SDMA_CH_CFG_RD (0x1 << 10)
/* Write-channel ch_cfg: no special flags */
#define SDMA_CH_CFG_WR 0

/* Default timeout for decompression (ms) */
#define GZIP_DEFAULT_TIMEOUT_MS 5000

/* DECOMP_STATUS fields. Bits 11 (set when the length matched ISIZE, despite
 * its RTL name deflate_size_err) and 12 (header_err: the header differs from
 * the one template the engine checks, which ordinary streams do) say nothing
 * about failure. */
#define DECOMP_STATUS_CRC_OK    (0x1u << 10)
#define DECOMP_STATUS_STATE(s)  ((s) & 0xf)
#define DECOMP_STATE_IDLE       0
#define DECOMP_STATE_CRC_CHECK  8

/* While waiting, look at the engine this often. A good stream leaves IDLE as
 * soon as its first bytes arrive, passes CRC_CHECK at once and ends in IDLE
 * with crc_ok. A corrupt one stops in CRC_CHECK or drops back to IDLE without
 * crc_ok, an invalid first block within microseconds, long before the first
 * look; the write channel then never completes.
 *
 * GZIP_STALL_MS does not grow with the output. With backpressure_en the engine
 * waits for the write channel, so (as we read it; no RTL at hand) it is at
 * most the 4 SRAM_WR lines, 512 KB, ahead: at CRC_CHECK no more is left to
 * write, and 100 ms for that would mean under 5 MB/s. On k230-2 (2026-11-24),
 * 16 MiB outputs under four concurrent 16 MiB SDMA copies took 52-118 ms with
 * no false failure in 105 rounds, nor in some 1400 rounds of 1 MiB, and no
 * 10 ms look caught a good stream in IDLE without crc_ok. */
#define GZIP_POLL_MS            10
#define GZIP_STALL_MS           100

/* Driver result for a corrupt stream, beside the RT-Thread codes used
 * elsewhere. The ioctl reports it as EBADMSG: bad data, not worth a retry,
 * unlike EIO. */
#define GZIP_ECORRUPT           1000

/* SRAM addresses used by UGZIP hardware for intermediate data.
 * UGZIP is hardwired to read compressed input from SRAM_RD and write
 * decompressed output to SRAM_WR. These are physical addresses — the SDMA
 * hardware accesses them directly, unaffected by RT-Smart's MMU.
 * SDMA shuttles data: user DDR ↔ SRAM ↔ UGZIP.
 *
 * The 768KB from 0x80200000 to 0x802BFFFF (banks mem0/1/2/8 out, mem9/10 in)
 * belong to the shared SRAM GNNE/KPU uses. While decompression is enabled
 * the KPU is locked out of them, and decompression overwrites whatever the
 * KPU kept there; nothing in hardware or this driver arbitrates. Use gzip
 * only while the KPU does not use that SRAM, e.g. to unpack a model before
 * it runs. */
#define SRAM_WR_ADDR 0x80200000UL /* UGZIP writes decompressed → SDMA reads from here */
#define SRAM_RD_ADDR 0x80280000UL /* SDMA writes compressed here → UGZIP reads */
#define SRAM_RD_BUFS 2 /* ping-pong for read channel */
#define SRAM_WR_BUFS 4 /* rotating for write channel */

/*
 * UGZIP register block at 0x80808000.
 * Data is fed via SDMA CH0 (read) and CH1 (write) with hardware handshake.
 */
struct ugzip_reg {
    volatile uint32_t decomp_start; /* 0x00: bit0=start, bit1=backpressure_en */
    volatile uint32_t gzip_src_size; /* 0x04 RW: bit31=enable | [30:0]=compressed size */
    volatile uint32_t dma_out_size; /* 0x08 RW: decompressed output size */
    volatile uint32_t decomp_status; /* 0x0C RO: bit10=crc_ok, [3:0]=engine state */
    volatile uint32_t decomp_int_status; /* 0x10: never set in this SoC, poll 0x0C */
    volatile uint32_t gzip_dma_hw; /* 0x14 RW: DMA handshake enable */
    volatile uint32_t gzip_div; /* 0x18 RW: clock divider pattern */
};

/*
 * SDMA LLI entry (24 bytes) — matches GSDMA hardware layout.
 */
struct sdma_llt {
    uint32_t reserved_0 : 28;
    uint32_t dimension : 1;
    uint32_t pause : 1;
    uint32_t node_intr : 1;
    uint32_t reserved_1 : 1;

    uint32_t src_addr;
    uint32_t line_size;

    uint32_t line_num : 16;
    uint32_t line_space : 16;

    uint32_t dst_addr;
    uint32_t next_llt_addr;
};

struct gzip_decomp_dev {
    struct rt_device  dev;
    struct rt_mutex   mutex;
    struct ugzip_reg* reg;
    /* Keep these alive after a failed SDMA STOP. The next ioctl retries
     * cleanup under mutex before it can submit another transfer. */
    struct {
        struct sdma_llt *rd_llt, *wr_llt;
        void *src_pin, *dst_pin;
        int rd_reserved, wr_reserved, pm_held;
    } held;
};

static struct gzip_decomp_dev g_gzip_decomp_dev;

static uint32_t gzip_get_le32(const uint8_t* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* Every SDMA node moves a whole ZIP_LINE_SIZE line, so the engine may touch
 * up to the next line boundary past len. */
static uint64_t gzip_dma_span(uint32_t len)
{
    return ((uint64_t)len + ZIP_LINE_SIZE - 1) & ~((uint64_t)ZIP_LINE_SIZE - 1);
}

static int gzip_dma_range_valid(uint64_t phys, uint32_t len)
{
    uint64_t span = gzip_dma_span(len);

    return span != 0 && phys <= UINT32_MAX && phys + span - 1 <= UINT32_MAX;
}

static int gzip_validate_stream(uint64_t src_phys, uint32_t src_len, uint64_t dst_phys, uint32_t dst_len)
{
    const uint8_t* src;

    if (!src_phys || src_len < 18 || !dst_phys || !dst_len)
        return -RT_EINVAL;
    if (!gzip_dma_range_valid(src_phys, src_len) || !gzip_dma_range_valid(dst_phys, dst_len))
        return -RT_EINVAL;

    src = (const uint8_t*)(uintptr_t)(src_phys - PV_OFFSET);
    rt_hw_cpu_dcache_clean((void*)src, src_len);

    /* User space normalizes the private 0x09 marker and optional fields
     * before issuing the ioctl. Validate the exact stream shape expected by
     * hardware, including output size, before any DMA can touch dst_phys. */
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 0x08 || src[3] != 0)
        return -RT_EINVAL;
    if (gzip_get_le32(src + src_len - 4) != dst_len)
        return -RT_EINVAL;

    return RT_EOK;
}

/* Callback for SDMA read channel — no-op, just prevents ch_submit from blocking */
static void gzip_rd_done_callback(void* arg) { (void)arg; }

/* Callback for SDMA write channel completion — releases semaphore */
static void gzip_wr_done_callback(void* arg)
{
    struct rt_semaphore* sem = (struct rt_semaphore*)arg;
    rt_sem_release(sem);
}

/*
 * Build SDMA LLI chain for the read channel.
 * src_phys (user DDR) → SRAM_RD_ADDR ping-pong (2 × 128KB).
 */
static struct sdma_llt* build_rd_llt(uint32_t src_phys, uint32_t src_len, uint32_t* out_count)
{
    struct sdma_llt* llt;
    uint32_t         count, i;
    uint64_t         phys;

    count = (src_len + ZIP_LINE_SIZE - 1) / ZIP_LINE_SIZE;
    if (count == 0)
        count = 1;

    llt = rt_malloc_align(count * sizeof(struct sdma_llt), 64);
    if (!llt)
        return RT_NULL;

    rt_memset(llt, 0, count * sizeof(struct sdma_llt));
    phys = (uint64_t)llt + PV_OFFSET;

    for (i = 0; i < count; i++) {
        llt[i].dimension     = 0; /* 1D */
        llt[i].src_addr      = src_phys + i * ZIP_LINE_SIZE;
        llt[i].dst_addr      = (uint32_t)(SRAM_RD_ADDR + (i % SRAM_RD_BUFS) * ZIP_LINE_SIZE);
        llt[i].line_size     = ZIP_LINE_SIZE;
        llt[i].next_llt_addr = (i + 1 < count) ? (uint32_t)(phys + (i + 1) * sizeof(struct sdma_llt)) : 0;
    }

    rt_hw_cpu_dcache_clean(llt, count * sizeof(struct sdma_llt));
    *out_count = count;
    return llt;
}

/*
 * Build SDMA LLI chain for the write channel.
 * SRAM_WR_ADDR rotating (4 × 128KB) → dst_phys (user DDR).
 */
static struct sdma_llt* build_wr_llt(uint32_t dst_phys, uint32_t dst_len, uint32_t* out_count)
{
    struct sdma_llt* llt;
    uint32_t         count, i;
    uint64_t         phys;

    count = (dst_len + ZIP_LINE_SIZE - 1) / ZIP_LINE_SIZE;
    if (count == 0)
        count = 1;

    llt = rt_malloc_align(count * sizeof(struct sdma_llt), 64);
    if (!llt)
        return RT_NULL;

    rt_memset(llt, 0, count * sizeof(struct sdma_llt));
    phys = (uint64_t)llt + PV_OFFSET;

    for (i = 0; i < count; i++) {
        llt[i].dimension     = 0; /* 1D */
        llt[i].src_addr      = (uint32_t)(SRAM_WR_ADDR + (i % SRAM_WR_BUFS) * ZIP_LINE_SIZE);
        llt[i].dst_addr      = dst_phys + i * ZIP_LINE_SIZE;
        llt[i].line_size     = ZIP_LINE_SIZE;
        llt[i].next_llt_addr = (i + 1 < count) ? (uint32_t)(phys + (i + 1) * sizeof(struct sdma_llt)) : 0;
    }

    rt_hw_cpu_dcache_clean(llt, count * sizeof(struct sdma_llt));
    *out_count = count;
    return llt;
}

/* The milliseconds left of a budget of ticks that began at start, 0 once it
 * is spent. */
static int32_t gzip_ms_left(rt_tick_t start, rt_tick_t budget)
{
    rt_tick_t used = rt_tick_get() - start;

    if (used >= budget)
        return 0;
    return (int32_t)(((uint64_t)(budget - used) * 1000 + RT_TICK_PER_SECOND - 1) / RT_TICK_PER_SECOND);
}

static int gzip_wait_done(struct gzip_decomp_dev* dev, struct rt_semaphore* wr_done_sem, rt_tick_t start,
                          rt_tick_t budget)
{
    rt_tick_t engine_start = rt_tick_get();
    rt_tick_t crc_since = 0;
    int       left_idle = 0;
    uint32_t  status;

    for (;;) {
        if (rt_sem_take(wr_done_sem, rt_tick_from_millisecond(GZIP_POLL_MS)) == RT_EOK) {
            status = readl(&dev->reg->decomp_status);
            if (status & DECOMP_STATUS_CRC_OK) {
                LOG_D("CRC OK: decomp_status=0x%08x", status);
                return RT_EOK;
            }
            LOG_E("CRC mismatch: decomp_status=0x%08x", status);
            return -GZIP_ECORRUPT;
        }

        status = readl(&dev->reg->decomp_status);
        switch (DECOMP_STATUS_STATE(status)) {
        case DECOMP_STATE_IDLE:
            crc_since = 0;
            /* IDLE without a good CRC after it ran, or still IDLE long after
             * the start (it stopped before the first look): the output will
             * never complete. */
            if (!(status & DECOMP_STATUS_CRC_OK) &&
                (left_idle || rt_tick_get() - engine_start >= rt_tick_from_millisecond(GZIP_STALL_MS))) {
                LOG_E("Corrupt stream: decomp_status=0x%08x", status);
                return -GZIP_ECORRUPT;
            }
            break;
        case DECOMP_STATE_CRC_CHECK:
            left_idle = 1;
            if (!crc_since) {
                crc_since = rt_tick_get() | 1;
            } else if (rt_tick_get() - crc_since >= rt_tick_from_millisecond(GZIP_STALL_MS)) {
                LOG_E("Corrupt stream, stuck in CRC check: decomp_status=0x%08x", status);
                return -GZIP_ECORRUPT;
            }
            break;
        default:
            left_idle = 1;
            crc_since = 0;
            break;
        }

        if (rt_tick_get() - start >= budget) {
            LOG_E("Timeout: decomp_status=0x%08x", status);
            return -RT_ETIMEOUT;
        }
    }
}

/* Called with dev->mutex held. No descriptor or pin may be released until
 * both channels acknowledge STOP; resetting shared GSDMA would abort other
 * clients as well. A permanently stuck channel retains its resources. */
static int gzip_decomp_release_resources(struct gzip_decomp_dev* dev)
{
    int rd_ret = 0, wr_ret = 0;

    if (dev->held.wr_reserved)
        wr_ret = gsdma_sdma_abort_channel(ZIP_WR_CH);
    if (dev->held.rd_reserved)
        rd_ret = gsdma_sdma_abort_channel(ZIP_RD_CH);
    if (rd_ret != 0 || wr_ret != 0) {
        LOG_E("SDMA did not stop; retaining gzip buffers and descriptors");
        return -RT_EBUSY;
    }

    /* Read channel first: its dec_en keeps both channels in handshake mode,
     * and releasing a channel may start a queued SDMA transfer on it. */
    if (dev->held.rd_reserved)
        gsdma_sdma_release_channel(ZIP_RD_CH);
    if (dev->held.wr_reserved)
        gsdma_sdma_release_channel(ZIP_WR_CH);
    dev->held.rd_reserved = dev->held.wr_reserved = 0;
    if (dev->held.wr_llt)
        rt_free_align(dev->held.wr_llt);
    if (dev->held.rd_llt)
        rt_free_align(dev->held.rd_llt);
    dev->held.rd_llt = dev->held.wr_llt = RT_NULL;
    vb_dma_buf_put(dev->held.dst_pin);
    vb_dma_buf_put(dev->held.src_pin);
    dev->held.src_pin = dev->held.dst_pin = RT_NULL;
    if (dev->held.pm_held) {
        int ret = k230_pm_put(K230_PM_DEV_GZIP);
        if (ret != 0)
            return k230_pm_rt_errno(ret); /* PM codes overlap RT-Thread's */
        dev->held.pm_held = 0;
    }
    return RT_EOK;
}

static int gzip_decomp_run(struct gzip_decomp_dev* dev, uint64_t src_phys, uint32_t src_len, uint64_t dst_phys,
                           uint32_t dst_len, int32_t timeout_ms)
{
    struct rt_semaphore wr_done_sem;
    uint32_t            rd_count = 0, wr_count = 0;
    /* One budget for the whole request: both channel waits and the
     * decompression share it. */
    rt_tick_t           budget = rt_tick_from_millisecond(timeout_ms > 0 ? timeout_ms : GZIP_DEFAULT_TIMEOUT_MS);
    rt_tick_t           start;
    int                 sem_initialized = 0;
    int                 ret, cleanup_ret;

    ret = gzip_validate_stream(src_phys, src_len, dst_phys, dst_len);
    if (ret != RT_EOK)
        goto out;

    LOG_D("gunzip: src=0x%08x len=%u dst=0x%08x len=%u", (uint32_t)src_phys, src_len, (uint32_t)dst_phys, dst_len);

    /* 1. Enable clocks and reset decompress block */
    ret = k230_pm_get(K230_PM_DEV_GZIP);
    if (ret != 0) {
        LOG_E("pm get gzip failed: %d", ret);
        ret = -RT_ERROR;
        goto out;
    }
    dev->held.pm_held = 1;

    /* 2. Reserve SDMA CH0 (read) and CH1 (write). Ordinary SDMA transfers
     * may be running on them: let those finish, scheduling no new ones. */
    start = rt_tick_get();
    if (gsdma_sdma_reserve_channel_wait(ZIP_RD_CH, gzip_ms_left(start, budget)) != 0) {
        LOG_E("Failed to reserve SDMA CH%d", ZIP_RD_CH);
        ret = -RT_EBUSY;
        goto out;
    }
    dev->held.rd_reserved = 1;
    if (gsdma_sdma_reserve_channel_wait(ZIP_WR_CH, gzip_ms_left(start, budget)) != 0) {
        LOG_E("Failed to reserve SDMA CH%d", ZIP_WR_CH);
        ret = -RT_EBUSY;
        goto out_release;
    }
    dev->held.wr_reserved = 1;
    if (rt_tick_get() != start)
        LOG_D("waited %u ticks for SDMA CH%d/CH%d", (unsigned)(rt_tick_get() - start), ZIP_RD_CH, ZIP_WR_CH);

    /* 3. Build SDMA LLI chains */
    dev->held.rd_llt = build_rd_llt((uint32_t)src_phys, src_len, &rd_count);
    if (!dev->held.rd_llt) {
        ret = -RT_ENOMEM;
        goto out_release;
    }
    dev->held.wr_llt = build_wr_llt((uint32_t)dst_phys, dst_len, &wr_count);
    if (!dev->held.wr_llt) {
        ret = -RT_ENOMEM;
        goto out_release;
    }

    LOG_D("rd_llt cnt=%u wr_llt cnt=%u", rd_count, wr_count);

    /* 4. Init completion semaphore for write channel */
    if (rt_sem_init(&wr_done_sem, "gz_wr", 0, RT_IPC_FLAG_PRIO) != RT_EOK) {
        ret = -RT_ERROR;
        goto out_release;
    }
    sem_initialized = 1;

    /* 5. The write channel overwrites whole lines from dst_phys on. Write
     * back dirty cache lines there first, so none is evicted over the output
     * while the DMA runs; they are invalidated once it is done. */
    rt_hw_cpu_dcache_clean((void*)(uintptr_t)(dst_phys - PV_OFFSET), gzip_dma_span(dst_len));

    /* Prepare UGZIP: set src_size with bit31 to reset internal state */
    writel(0x80000000, &dev->reg->gzip_src_size);

    /* 6. Submit SDMA channels via GSDMA driver workflow:
     *    CH0 (read): src DDR → SRAM_RD, dec_en=1 for gzip handshake
     *    CH1 (write): SRAM_WR → dst DDR, callback releases semaphore */
    if (gsdma_sdma_ch_submit(ZIP_RD_CH, SDMA_CH_CFG_RD, (uint32_t)((uint64_t)dev->held.rd_llt + PV_OFFSET),
                             gzip_rd_done_callback, RT_NULL) != 0 ||
        gsdma_sdma_ch_submit(ZIP_WR_CH, SDMA_CH_CFG_WR, (uint32_t)((uint64_t)dev->held.wr_llt + PV_OFFSET),
                             gzip_wr_done_callback, &wr_done_sem) != 0) {
        LOG_E("Failed to submit SDMA channels");
        ret = -RT_ERROR;
        goto out_reset;
    }

    /* 7. Configure UGZIP and start decompression */
    writel(src_len | (0x1u << 31), &dev->reg->gzip_src_size);
    writel(dst_len, &dev->reg->dma_out_size);
    writel(0x3, &dev->reg->decomp_start);

    /* 8. Wait for write channel completion via IRQ callback, giving up early
     * once the engine shows the stream is corrupt. */
    ret = gzip_wait_done(dev, &wr_done_sem, start, budget);

    /* 9. Invalidate destination cache */
    rt_hw_cpu_dcache_invalidate((void*)(uintptr_t)(dst_phys - PV_OFFSET), dst_len);

    /* 10. Stop the engine before aborting DMA on an error. */
out_reset:
    writel(0, &dev->reg->gzip_src_size);

    if (ret != RT_EOK)
        sysctl_reset(SYSCTL_RESET_DECOMPRESS);

out_release:
out:
    /* Abort removes callbacks even if STOP fails, so the stack semaphore
     * can be detached. Descriptors, pins and clocks stay owned by dev. */
    cleanup_ret = gzip_decomp_release_resources(dev);
    if (sem_initialized)
        rt_sem_detach(&wr_done_sem);
    return cleanup_ret != RT_EOK ? cleanup_ret : ret;
}

/*
 * The reserved channels take raw descriptors that GSDMA cannot check, so the
 * user buffer rule is applied here: both physical ranges must lie in an
 * in-use VB block or a user-allocated MMZ buffer, padded to the whole 128KB
 * nodes the DMA reads and writes.
 * They stay pinned until both channels stop, including a failed STOP; the
 * kernel also reads the source header, so pin before validating it.
 */
static int gzip_decomp_do_gunzip(struct gzip_decomp_dev* dev, uint64_t src_phys, uint32_t src_len, uint64_t dst_phys,
                                 uint32_t dst_len, int32_t timeout_ms)
{
    uint64_t src_span = gzip_dma_span(src_len);
    uint64_t dst_span = gzip_dma_span(dst_len);
    void*    src_pin;
    void*    dst_pin;
    int      ret;

    if (!src_span || !dst_span)
        return -RT_EINVAL;

    rt_mutex_take(&dev->mutex, RT_WAITING_FOREVER);
    ret = gzip_decomp_release_resources(dev);
    if (ret != RT_EOK)
        goto out;

    src_pin = vb_dma_buf_get(src_phys, src_span);
    dst_pin = src_pin ? vb_dma_buf_get(dst_phys, dst_span) : RT_NULL;
    if (!dst_pin) {
        vb_dma_buf_put(src_pin);
        LOG_E("invalid user buffers: src=0x%lx+%lu dst=0x%lx+%lu", (unsigned long)src_phys, (unsigned long)src_span,
              (unsigned long)dst_phys, (unsigned long)dst_span);
        ret = -RT_EINVAL;
        goto out;
    }

    dev->held.src_pin = src_pin;
    dev->held.dst_pin = dst_pin;
    ret = gzip_decomp_run(dev, src_phys, src_len, dst_phys, dst_len, timeout_ms);

out:
    rt_mutex_release(&dev->mutex);
    return ret;
}

/* The ioctl result becomes the caller's errno, so it must be POSIX. The
 * driver works in RT-Thread codes (and GZIP_ECORRUPT), which mean other things
 * as errno values: -RT_EBUSY is 7, E2BIG. */
static int gzip_decomp_posix_errno(int ret)
{
    switch (ret) {
    case RT_EOK:
        return 0;
    case -RT_EBUSY:
        return -EBUSY;
    case -RT_EINVAL:
        return -EINVAL;
    case -RT_ENOMEM:
        return -ENOMEM;
    case -RT_ETIMEOUT:
        return -ETIMEDOUT;
    case -GZIP_ECORRUPT:
        return -EBADMSG;
    default:
        return -EIO;
    }
}

static rt_err_t gzip_decomp_control(rt_device_t dev, int cmd, void* args)
{
    struct gzip_decomp_dev*    ddev = rt_container_of(dev, struct gzip_decomp_dev, dev);
    struct rt_gzip_decomp_args user_args;
    int                        ret;

    switch (cmd) {
    case RT_GZIP_DECOMP_GUNZIP:
        if (lwp_get_from_user_ex(&user_args, args, sizeof(user_args)) != 0) {
            LOG_E("Failed to copy args from user");
            return -EFAULT;
        }
        ret = gzip_decomp_do_gunzip(ddev, user_args.src_phys, user_args.src_len, user_args.dst_phys, user_args.dst_len,
                                    user_args.timeout_ms);
        return gzip_decomp_posix_errno(ret);
    default:
        return -ENOTTY;
    }
}

static rt_err_t gzip_decomp_open(rt_device_t dev, rt_uint16_t oflag) { return RT_EOK; }

static rt_err_t gzip_decomp_close(rt_device_t dev) { return RT_EOK; }

static const struct rt_device_ops gzip_decomp_ops = {
    .init    = RT_NULL,
    .open    = gzip_decomp_open,
    .close   = gzip_decomp_close,
    .read    = RT_NULL,
    .write   = RT_NULL,
    .control = gzip_decomp_control,
};

int rt_hw_gzip_decomp_init(void)
{
    struct gzip_decomp_dev* dev = &g_gzip_decomp_dev;

    /* ioremap UGZIP registers */
    dev->reg = (struct ugzip_reg*)rt_ioremap((void*)UGZIP_BASE, UGZIP_SIZE);
    if (!dev->reg) {
        LOG_E("Failed to ioremap UGZIP at 0x%08lx", UGZIP_BASE);
        return -RT_ERROR;
    }

    rt_mutex_init(&dev->mutex, "decomp", RT_IPC_FLAG_PRIO);

    if (RT_EOK != rt_device_register(&dev->dev, "gzip_decomp", RT_DEVICE_FLAG_RDWR)) {
        LOG_E("gzip_decomp device register failed");
        rt_iounmap(dev->reg);
        return -RT_ERROR;
    }

    dev->dev.ops = &gzip_decomp_ops;

    LOG_I("K230 UGZIP driver initialized (base=0x%08lx, SRAM rd=0x%08lx wr=0x%08lx)", UGZIP_BASE, SRAM_RD_ADDR, SRAM_WR_ADDR);

    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_gzip_decomp_init);
