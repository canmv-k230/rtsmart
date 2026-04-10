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
#include <rtthread.h>

#include <ioremap.h>
#include <lwp_user_mm.h>

#include "cache.h"
#include "riscv_io.h"

#include "sysctl_clk.h"
#include "sysctl_rst.h"

#include "drv_gzip_decomp.h"

#define DBG_LVL DBG_LOG
#define DBG_COLOR
#define DBG_TAG "gzip_decomp"
#include <rtdbg.h>

#ifndef PV_OFFSET
#define PV_OFFSET 0x0
#endif

/* ---- GSDMA exported functions (linked from MPP kernel module) ---- */
extern int  gsdma_sdma_reserve_channel(uint8_t ch_num);
extern void gsdma_sdma_release_channel(uint8_t ch_num);
extern int  gsdma_sdma_ch_submit(uint8_t ch_num, uint32_t ch_cfg_value, uint32_t llt_phys_addr, void (*callback)(void* arg),
                                 void* arg);
extern int  gsdma_sdma_ch_transfer(uint8_t ch_num, uint32_t src_phys, uint32_t dst_phys, uint32_t len, uint32_t ch_cfg_value,
                                   void (*callback)(void* arg), void* arg);

/* ---- Hardware constants ---- */
#define UGZIP_BASE 0x80808000UL
#define UGZIP_SIZE 0x10

#define ZIP_LINE_SIZE (128 * 1024) /* 128KB per SDMA LLI node */
#define ZIP_RD_CH     0 /* SDMA CH0: read (src DDR → SRAM) */
#define ZIP_WR_CH     1 /* SDMA CH1: write (SRAM → dst DDR) */

/* Read-channel ch_cfg: dec_en=1 (bit10) for gzip hardware handshake */
#define SDMA_CH_CFG_RD (0x1 << 10)
/* Write-channel ch_cfg: no special flags */
#define SDMA_CH_CFG_WR 0

/* Default timeout for decompression (ms) */
#define GZIP_DEFAULT_TIMEOUT_MS 5000

/* SRAM addresses used by UGZIP hardware for intermediate data.
 * UGZIP is hardwired to read compressed input from SRAM_RD and write
 * decompressed output to SRAM_WR. These are physical addresses — the SDMA
 * hardware accesses them directly, unaffected by RT-Smart's MMU.
 * SDMA shuttles data: user DDR ↔ SRAM ↔ UGZIP. */
#define SRAM_WR_ADDR 0x80200000UL /* UGZIP writes decompressed → SDMA reads from here */
#define SRAM_RD_ADDR 0x80280000UL /* SDMA writes compressed here → UGZIP reads */
#define SRAM_RD_BUFS 2 /* ping-pong for read channel */
#define SRAM_WR_BUFS 4 /* rotating for write channel */

/*
 * UGZIP register block at 0x80808000.
 * Data is fed via SDMA CH0 (read) and CH1 (write) with hardware handshake.
 */
struct ugzip_reg {
    volatile uint32_t decomp_start; /* 0x00 W1: bit0=start */
    volatile uint32_t gzip_src_size; /* 0x04 RW: bit31=enable | [30:0]=compressed size */
    volatile uint32_t dma_out_size; /* 0x08 RW: decompressed output size */
    volatile uint32_t decomp_intstat; /* 0x0C RO: bit10=crc_ok */
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
};

static struct gzip_decomp_dev g_gzip_decomp_dev;

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

static int gzip_decomp_do_gunzip(struct gzip_decomp_dev* dev, uint64_t src_phys, uint32_t src_len, uint64_t dst_phys,
                                 uint32_t dst_len, int32_t timeout_ms)
{
    struct sdma_llt *   rd_llt = RT_NULL, *wr_llt = RT_NULL;
    struct rt_semaphore wr_done_sem;
    uint32_t            rd_count = 0, wr_count = 0;
    uint32_t            decomp_intstat;
    int                 ret;

    if (!src_phys || !src_len || !dst_phys || !dst_len)
        return -RT_EINVAL;

    rt_mutex_take(&dev->mutex, RT_WAITING_FOREVER);

    LOG_D("gunzip: src=0x%08x len=%u dst=0x%08x len=%u", (uint32_t)src_phys, src_len, (uint32_t)dst_phys, dst_len);

    /* 1. Enable clocks and reset decompress block */
    sysctl_clk_set_leaf_en(SYSCTL_CLK_SHRM_SRC, true);
    sysctl_clk_set_leaf_en(SYSCTL_CLK_DECOMPRESS_ACLK_GATE, true);

    if (!sysctl_reset(SYSCTL_RESET_DECOMPRESS)) {
        LOG_E("sysctl_reset(DECOMPRESS) failed");
        ret = -RT_ERROR;
        goto out;
    }
    sysctl_clk_set_leaf_en(SYSCTL_CLK_DECOMPRESS_ACLK_GATE, true);

    /* 2. Reserve SDMA CH0 (read) and CH1 (write) */
    if (gsdma_sdma_reserve_channel(ZIP_RD_CH) != 0) {
        LOG_E("Failed to reserve SDMA CH%d", ZIP_RD_CH);
        ret = -RT_EBUSY;
        goto out;
    }
    if (gsdma_sdma_reserve_channel(ZIP_WR_CH) != 0) {
        LOG_E("Failed to reserve SDMA CH%d", ZIP_WR_CH);
        gsdma_sdma_release_channel(ZIP_RD_CH);
        ret = -RT_EBUSY;
        goto out;
    }

    /* 3. Flush source data cache */
    rt_hw_cpu_dcache_clean((void*)(uintptr_t)(src_phys - PV_OFFSET), src_len);

    /* 4. Build SDMA LLI chains */
    rd_llt = build_rd_llt((uint32_t)src_phys, src_len, &rd_count);
    if (!rd_llt) {
        ret = -RT_ENOMEM;
        goto out_release;
    }
    wr_llt = build_wr_llt((uint32_t)dst_phys, dst_len, &wr_count);
    if (!wr_llt) {
        ret = -RT_ENOMEM;
        goto out_release;
    }

    LOG_D("rd_llt cnt=%u wr_llt cnt=%u", rd_count, wr_count);

    /* 5. Init completion semaphore for write channel */
    rt_sem_init(&wr_done_sem, "gz_wr", 0, RT_IPC_FLAG_PRIO);

    /* 6. Prepare UGZIP: set src_size with bit31 to reset internal state */
    writel(0x80000000, &dev->reg->gzip_src_size);

    /* 7. Submit SDMA channels via GSDMA driver workflow:
     *    CH0 (read): src DDR → SRAM_RD, dec_en=1 for gzip handshake
     *    CH1 (write): SRAM_WR → dst DDR, callback releases semaphore */
    gsdma_sdma_ch_submit(ZIP_RD_CH, SDMA_CH_CFG_RD, (uint32_t)((uint64_t)rd_llt + PV_OFFSET), gzip_rd_done_callback, RT_NULL);
    gsdma_sdma_ch_submit(ZIP_WR_CH, SDMA_CH_CFG_WR, (uint32_t)((uint64_t)wr_llt + PV_OFFSET), gzip_wr_done_callback,
                         &wr_done_sem);

    /* 8. Configure UGZIP and start decompression */
    writel(src_len | (0x1u << 31), &dev->reg->gzip_src_size);
    writel(dst_len, &dev->reg->dma_out_size);
    writel(0x3, &dev->reg->decomp_start);

    /* 9. Wait for write channel completion via IRQ callback */
    ret = rt_sem_take(&wr_done_sem, rt_tick_from_millisecond(timeout_ms > 0 ? timeout_ms : GZIP_DEFAULT_TIMEOUT_MS));

    if (ret == RT_EOK) {
        decomp_intstat = readl(&dev->reg->decomp_intstat);
        if (decomp_intstat & (0x1 << 10)) {
            LOG_D("CRC OK: decomp_intstat=0x%08x", decomp_intstat);
            ret = RT_EOK;
        } else {
            LOG_E("CRC error: decomp_intstat=0x%08x", decomp_intstat);
            ret = -RT_ERROR;
        }
    } else {
        LOG_E("Timeout: decomp_intstat=0x%08x", readl(&dev->reg->decomp_intstat));
        ret = -RT_ETIMEOUT;
    }

    rt_sem_detach(&wr_done_sem);

    /* 10. Invalidate destination cache */
    rt_hw_cpu_dcache_invalidate((void*)(uintptr_t)(dst_phys - PV_OFFSET), dst_len);

    /* 11. Clean up */
    writel(0, &dev->reg->gzip_src_size);

    if (ret != RT_EOK)
        sysctl_reset(SYSCTL_RESET_DECOMPRESS);

out_release:
    gsdma_sdma_release_channel(ZIP_RD_CH);
    gsdma_sdma_release_channel(ZIP_WR_CH);

    if (wr_llt)
        rt_free_align(wr_llt);
    if (rd_llt)
        rt_free_align(rd_llt);

out:
    rt_mutex_release(&dev->mutex);
    return ret;
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
            return -RT_ERROR;
        }
        ret = gzip_decomp_do_gunzip(ddev, user_args.src_phys, user_args.src_len, user_args.dst_phys, user_args.dst_len,
                                    user_args.timeout_ms);
        break;
    default:
        ret = -RT_EINVAL;
        break;
    }
    return ret;
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
