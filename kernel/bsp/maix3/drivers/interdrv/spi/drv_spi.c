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

#include <stdint.h>
#include <string.h>

#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>

#include <drivers/spi.h>

#if defined(RT_USING_LWP) && defined(RT_USING_USERSPACE)
#include <lwp_user_mm.h>
#endif

#include "board.h"
#include "cache.h"
#include "drv_fpioa.h"
#include "drv_gpio.h"
#include "ioremap.h"
#include "riscv_io.h"
#include "rvv_ops.h"
#include "sysctl_clk.h"
#include "sysctl_rst.h"
#include "tick.h"

#include "drv_spi.h"

#define DBG_TAG "spi"
#ifdef RT_SPI_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#include <rtdbg.h>

#define IRQN_SPI0 146
#define IRQN_SPI1 155
#define IRQN_SPI2 164

#define BIT(n)                (1UL << (n))
#define DW_SPI_HARD_CS_MASK   0x7F
#define DW_SPI_MAX_CS         5
#define DW_SPI_PERF_MIN_BYTES 16U

typedef struct {
    const char*       name;
    uintptr_t         base;
    int               irq_base;
    sysctl_clk_node_e clk;
    uint8_t           idx;
    uint8_t           max_line;
    uint32_t          max_hz;
} dw_spi_hw_info_t;

typedef struct {
    struct rt_spi_bus       dev;
    void*                   base;
    const dw_spi_hw_info_t* hw;
    uint8_t                 rdse;
    uint8_t                 rdsd;
    uint32_t                ser;
    struct rt_event         event;
    void*                   dma_buf;
    void*                   pio_buf;
    rt_size_t               dma_buf_size;
    rt_size_t               pio_buf_size;
} dw_spi_bus_t;

typedef struct {
    void*     send_buf;
    void*     recv_buf;
    rt_size_t send_length;
    rt_size_t recv_length;
    uint8_t   cell_size;
} dw_spi_xfer_state_t;

typedef struct {
    dw_spi_xfer_state_t xfer;
    rt_size_t           total_frames;
    rt_size_t           add_frames;
    rt_size_t           send_total;
    rt_size_t           send_chunk;
    rt_size_t           send_done;
    rt_size_t           recv_chunk;
    rt_size_t           recv_done;
    void*               send_alloc;
    void*               recv_alloc;
    uint8_t             header_buf[12];
    uint8_t             cell_size;
    uint8_t             tmod;
    rt_bool_t           tx_copy_payload;
} dw_spi_standard_xfer_t;

static const uint8_t dw_spi_irq_offsets[] = { SSI_TXE, SSI_RXF, SSI_DONE, SSI_AXIE };

static const fpioa_func_t dw_spi_cs_funcs[][DW_SPI_MAX_CS] = {
    { OSPI_CS, FUNC_MAX, FUNC_MAX, FUNC_MAX, FUNC_MAX },
    { QSPI0_CS0, QSPI0_CS1, QSPI0_CS2, QSPI0_CS3, QSPI0_CS4 },
    { QSPI1_CS0, QSPI1_CS1, QSPI1_CS2, QSPI1_CS3, QSPI1_CS4 },
};

static uint32_t    dw_spi_get_ser(dw_spi_bus_t* spi_bus, struct rt_qspi_configuration* cfg);
static void        dw_spi_set_soft_cs(struct rt_qspi_configuration* cfg, rt_bool_t asserted);
static rt_uint32_t drv_spi_xfer_enhanced(dw_spi_bus_t* spi_bus, struct rt_qspi_configuration* cfg, struct rt_qspi_message* msg);
static rt_uint32_t drv_spi_xfer_standard(dw_spi_bus_t* spi_bus, struct rt_qspi_configuration* cfg, struct rt_qspi_message* msg);

static uint32_t dw_spi_find_ser_by_fpioa(dw_spi_bus_t* spi_bus, rt_bool_t mapped)
{
    uint8_t idx = spi_bus->hw->idx;

    if (idx >= sizeof(dw_spi_cs_funcs) / sizeof(dw_spi_cs_funcs[0])) {
        return 0;
    }

    for (uint8_t i = 0; i < DW_SPI_MAX_CS; i++) {
        fpioa_func_t func = dw_spi_cs_funcs[idx][i];
        int          pin;

        if (func == FUNC_MAX) {
            continue;
        }

        pin = drv_fpioa_find_pin_by_func(func);
        if ((mapped && pin >= 0) || (!mapped && pin < 0)) {
            return BIT(i);
        }
    }

    return 0;
}

static rt_err_t dw_spi_wait_tx_complete(dw_spi_reg_t* spi, uint32_t timeout_ms)
{
    uint64_t start_ms = cpu_ticks_ms();

    while ((readl(&spi->sr) & DW_SSI_SR_TX_DONE_MASK) != DW_SSI_SR_TX_DONE_VALUE) {
        if (cpu_ticks_ms() - start_ms >= timeout_ms) {
            return -RT_ETIMEOUT;
        }
    }

    return RT_EOK;
}

static void dw_spi_set_axi_addr(dw_spi_reg_t* spi, const void* buf)
{
    uint64_t addr = (uint64_t)(uintptr_t)buf;

    writel((uint32_t)addr, &spi->axiar0);
    writel((uint32_t)(addr >> 32), &spi->axiar1);
}

static uint32_t dw_spi_get_dma_atw(const void* buf, rt_size_t size)
{
    uintptr_t addr = (uintptr_t)buf;

    if (size >= 8 && (addr % 8) == 0 && (size % 8) == 0) {
        return 3;
    }
    if (size >= 4 && (addr % 4) == 0 && (size % 4) == 0) {
        return 2;
    }
    if (size >= 2 && (addr % 2) == 0 && (size % 2) == 0) {
        return 1;
    }

    return 0;
}

static uint32_t dw_spi_get_axi_burst_len(rt_size_t size, uint32_t atw)
{
    rt_size_t beat_bytes = (rt_size_t)1 << atw;
    rt_size_t beats;

    if (beat_bytes == 0) {
        return 0;
    }

    beats = (size + beat_bytes - 1) / beat_bytes;
    if (beats == 0) {
        return 0;
    }
    if (beats > 16) {
        beats = 16;
    }

    return (uint32_t)(beats - 1);
}

static rt_err_t dw_spi_get_xfer_frames(rt_size_t length_bytes, uint8_t cell_size, rt_size_t* frames)
{
    if (!frames || cell_size == 0) {
        return -RT_EINVAL;
    }
    if (length_bytes % cell_size) {
        return -RT_EINVAL;
    }

    *frames = length_bytes / cell_size;
    return RT_EOK;
}

static uint32_t dw_spi_make_txftlr(rt_size_t frames)
{
    rt_size_t irq_level;

    if (frames == 0) {
        return 0;
    }

    irq_level = frames > DW_SSI_TX_FIFO_THRESH ? DW_SSI_TX_FIFO_THRESH : frames - 1;

    /*
     * In the standard PIO path, software refills the FIFO on TXE interrupts.
     * If TXFTHR is left high and the CPU services one refill late, the FIFO can
     * drain completely and a short tail refill may never restart the shifter
     * because the databook requires the FIFO level to be greater than TXFTHR.
     * Keep the start threshold at zero here and use only the low TXE threshold
     * to control refill cadence.
     */
    return (uint32_t)irq_level;
}

static void* dw_spi_get_dma_buf(dw_spi_bus_t* spi_bus, rt_size_t size)
{
    void* buf;

    if (size > DW_SSI_DMA_BOUNCE_MAX_SIZE) {
        return NULL;
    }
    if (spi_bus->dma_buf && spi_bus->dma_buf_size >= size) {
        return spi_bus->dma_buf;
    }

    buf = rt_malloc_align(size, RT_CPU_CACHE_LINE_SZ);
    if (!buf) {
        return NULL;
    }

    if (spi_bus->dma_buf) {
        rt_free_align(spi_bus->dma_buf);
    }
    spi_bus->dma_buf      = buf;
    spi_bus->dma_buf_size = size;

    return buf;
}

static void* dw_spi_get_pio_buf(dw_spi_bus_t* spi_bus, rt_size_t size)
{
    void* buf;

    if (size > DW_SSI_DMA_BOUNCE_MAX_SIZE) {
        return NULL;
    }
    if (spi_bus->pio_buf && spi_bus->pio_buf_size >= size) {
        return spi_bus->pio_buf;
    }

    buf = rt_malloc_align(size, RT_CPU_CACHE_LINE_SZ);
    if (!buf) {
        return NULL;
    }

    if (spi_bus->pio_buf) {
        rt_free_align(spi_bus->pio_buf);
    }
    spi_bus->pio_buf      = buf;
    spi_bus->pio_buf_size = size;

    return buf;
}

static rt_bool_t dw_spi_buf_aligned(const void* buf, uint8_t cell_size)
{
    return cell_size <= 1 || ((uintptr_t)buf % cell_size) == 0;
}

static rt_bool_t dw_spi_is_user_buf(const void* buf, rt_size_t size)
{
#if defined(RT_USING_LWP) && defined(RT_USING_USERSPACE)
    if (buf == RT_NULL || size == 0 || lwp_self() == RT_NULL) {
        return RT_FALSE;
    }

    return lwp_user_accessable((void*)buf, size) ? RT_TRUE : RT_FALSE;
#else
    return RT_FALSE;
#endif
}

static rt_err_t dw_spi_copy_from_user_buf(void* dst, const void* src, rt_size_t size)
{
#if defined(RT_USING_LWP) && defined(RT_USING_USERSPACE)
    if (size != lwp_get_from_user(dst, (void*)src, size)) {
        return -RT_EINVAL;
    }
#else
    rt_memcpy(dst, src, size);
#endif

    return RT_EOK;
}

static rt_err_t dw_spi_copy_to_user_buf(void* dst, const void* src, rt_size_t size)
{
#if defined(RT_USING_LWP) && defined(RT_USING_USERSPACE)
    if (size != lwp_put_to_user(dst, (void*)src, size)) {
        return -RT_EINVAL;
    }
#else
    rt_memcpy(dst, src, size);
#endif

    return RT_EOK;
}

static rt_err_t dw_spi_prepare_user_xfer(struct rt_qspi_message* msg, void** tx_alloc, void** rx_alloc)
{
    rt_size_t len;

    *tx_alloc = RT_NULL;
    *rx_alloc = RT_NULL;
    len       = msg->parent.length;

    if (len == 0) {
        return RT_EOK;
    }

    if (dw_spi_is_user_buf(msg->parent.send_buf, len)) {
        *tx_alloc = rt_malloc_align(len, RT_CPU_CACHE_LINE_SZ);
        if (*tx_alloc == RT_NULL) {
            return -RT_ENOMEM;
        }
        if (dw_spi_copy_from_user_buf(*tx_alloc, msg->parent.send_buf, len) != RT_EOK) {
            return -RT_EINVAL;
        }
        msg->parent.send_buf = *tx_alloc;
    }

    if (dw_spi_is_user_buf(msg->parent.recv_buf, len)) {
        *rx_alloc = rt_malloc_align(len, RT_CPU_CACHE_LINE_SZ);
        if (*rx_alloc == RT_NULL) {
            return -RT_ENOMEM;
        }
        msg->parent.recv_buf = *rx_alloc;
    }

    return RT_EOK;
}

static rt_err_t dw_spi_finish_user_xfer(const struct rt_qspi_message* msg, void* user_recv_buf, rt_uint32_t length,
                                        void* rx_alloc)
{
    if (rx_alloc == RT_NULL || length == 0) {
        return RT_EOK;
    }

    if (length > msg->parent.length) {
        length = (rt_uint32_t)msg->parent.length;
    }

    return dw_spi_copy_to_user_buf(user_recv_buf, rx_alloc, length);
}

static rt_err_t dw_spi_fifo_push(dw_spi_xfer_state_t* xfer, dw_spi_reg_t* spi)
{
    uint8_t* buf = xfer->send_buf;

    if (!buf) {
        while (xfer->send_length && (readl(&spi->sr) & DW_SSI_SR_TFNF)) {
            writel(0, &spi->dr[0]);
            xfer->send_length--;
        }

        return RT_EOK;
    }

    if (xfer->cell_size == 1) {
        while (xfer->send_length && (readl(&spi->sr) & DW_SSI_SR_TFNF)) {
            writel(*(uint8_t*)buf, &spi->dr[0]);
            buf += 1;
            xfer->send_length--;
        }
    } else if (xfer->cell_size == 2) {
        while (xfer->send_length && (readl(&spi->sr) & DW_SSI_SR_TFNF)) {
            writel(*(uint16_t*)buf, &spi->dr[0]);
            buf += 2;
            xfer->send_length--;
        }
    } else if (xfer->cell_size == 4) {
        while (xfer->send_length && (readl(&spi->sr) & DW_SSI_SR_TFNF)) {
            writel(*(uint32_t*)buf, &spi->dr[0]);
            buf += 4;
            xfer->send_length--;
        }
    } else {
        return -RT_EINVAL;
    }

    xfer->send_buf = buf;

    return RT_EOK;
}

static rt_err_t dw_spi_fifo_pop(dw_spi_xfer_state_t* xfer, dw_spi_reg_t* spi)
{
    uint8_t* buf = xfer->recv_buf;

    if (!buf) {
        return -RT_EINVAL;
    }

    if (xfer->cell_size == 1) {
        while (xfer->recv_length && (readl(&spi->sr) & DW_SSI_SR_RFNE)) {
            *(uint8_t*)buf = readl(&spi->dr[0]);
            buf += 1;
            xfer->recv_length--;
        }
    } else if (xfer->cell_size == 2) {
        while (xfer->recv_length && (readl(&spi->sr) & DW_SSI_SR_RFNE)) {
            *(uint16_t*)buf = readl(&spi->dr[0]);
            buf += 2;
            xfer->recv_length--;
        }
    } else if (xfer->cell_size == 4) {
        while (xfer->recv_length && (readl(&spi->sr) & DW_SSI_SR_RFNE)) {
            *(uint32_t*)buf = readl(&spi->dr[0]);
            buf += 4;
            xfer->recv_length--;
        }
    } else {
        return -RT_EINVAL;
    }

    xfer->recv_buf = buf;
    return RT_EOK;
}

static rt_size_t dw_spi_limit_xfer_frames(rt_size_t frames)
{
    return frames > DW_SSI_MAX_XFER_FRAMES ? DW_SSI_MAX_XFER_FRAMES : frames;
}

static rt_err_t dw_spi_standard_get_add_frames(dw_spi_bus_t* spi_bus, struct rt_qspi_configuration* cfg,
                                               struct rt_qspi_message* msg, rt_size_t* add_frames)
{
    if (cfg->parent.data_width == 8) {
        if ((msg->instruction.size & 7) || (msg->address.size & 7) || (msg->dummy_cycles & 7)) {
            LOG_E("spi%d instruction %u, address %u, dummy_cycles %u invalid", spi_bus->hw->idx, msg->instruction.size,
                  msg->address.size, msg->dummy_cycles);
            LOG_E("instruction, address, dummy_cycles must be set to multiples of 8");
            return -RT_EINVAL;
        }
        if (msg->instruction.size > 32 || msg->address.size > 32 || msg->dummy_cycles > 32) {
            LOG_E("spi%d instruction %u, address %u, dummy_cycles %u invalid", spi_bus->hw->idx, msg->instruction.size,
                  msg->address.size, msg->dummy_cycles);
            LOG_E("instruction, address, dummy_cycles must be set to less than 32");
            return -RT_EINVAL;
        }

        *add_frames = (msg->instruction.size + msg->address.size + msg->dummy_cycles) / 8;
        return RT_EOK;
    }

    if (msg->instruction.size || msg->address.size || msg->dummy_cycles) {
        LOG_E("For data_width not equal 8, instruction, address, dummy_cycles must be set to zero");
        return -RT_EINVAL;
    }

    *add_frames = 0;
    return RT_EOK;
}

static void dw_spi_standard_fill_header(struct rt_qspi_message* msg, uint8_t* buf)
{
    for (int i = msg->instruction.size / 8; i; i--)
        *buf++ = msg->instruction.content >> ((i - 1) * 8);
    for (int i = msg->address.size / 8; i; i--)
        *buf++ = msg->address.content >> ((i - 1) * 8);
    for (int i = msg->dummy_cycles / 8; i; i--)
        *buf++ = 0xFF;
}

static rt_err_t dw_spi_standard_prepare_tx(dw_spi_bus_t* spi_bus, struct rt_qspi_message* msg, dw_spi_standard_xfer_t* sxfer)
{
    rt_bool_t payload_aligned = dw_spi_buf_aligned(msg->parent.send_buf, sxfer->cell_size);

    if (sxfer->tmod == SPI_TMOD_RO) {
        return RT_EOK;
    }

    if (sxfer->tmod == SPI_TMOD_TO) {
        sxfer->send_chunk = dw_spi_limit_xfer_frames(sxfer->total_frames + sxfer->add_frames);
    } else if (sxfer->tmod == SPI_TMOD_TR) {
        sxfer->send_chunk = dw_spi_limit_xfer_frames(sxfer->total_frames);
    } else {
        sxfer->send_chunk = sxfer->add_frames;
    }

    sxfer->send_total = sxfer->tmod == SPI_TMOD_TO ? sxfer->total_frames + sxfer->add_frames : sxfer->total_frames;
    if (sxfer->tmod == SPI_TMOD_EPROMREAD) {
        sxfer->send_total = sxfer->send_chunk;
    }

    if (sxfer->add_frames) {
        uint8_t* header = sxfer->tmod == SPI_TMOD_EPROMREAD ? sxfer->header_buf : NULL;

        if (sxfer->tmod == SPI_TMOD_TO) {
            sxfer->send_alloc = dw_spi_get_pio_buf(spi_bus, sxfer->send_chunk * sxfer->cell_size);
            if (sxfer->send_alloc == NULL) {
                return -RT_ENOMEM;
            }
            header                 = sxfer->send_alloc;
            sxfer->tx_copy_payload = !payload_aligned;
        }

        dw_spi_standard_fill_header(msg, header);
        sxfer->xfer.send_buf = sxfer->tmod == SPI_TMOD_EPROMREAD ? sxfer->header_buf : sxfer->send_alloc;
    } else if (payload_aligned) {
        sxfer->xfer.send_buf = (void*)msg->parent.send_buf;
    } else {
        sxfer->send_alloc = dw_spi_get_pio_buf(spi_bus, sxfer->send_chunk * sxfer->cell_size);
        if (sxfer->send_alloc == NULL) {
            return -RT_ENOMEM;
        }
        if (msg->parent.send_buf == NULL) {
            return -RT_EINVAL;
        }
        sxfer->tx_copy_payload = RT_TRUE;
        rvv_memcpy(sxfer->send_alloc, msg->parent.send_buf, sxfer->send_chunk * sxfer->cell_size);
        sxfer->xfer.send_buf = sxfer->send_alloc;
    }

    if (sxfer->tmod == SPI_TMOD_TO && sxfer->add_frames && sxfer->send_chunk > sxfer->add_frames) {
        if (msg->parent.send_buf == NULL) {
            return -RT_EINVAL;
        }
        rvv_memcpy((uint8_t*)sxfer->xfer.send_buf + sxfer->add_frames * sxfer->cell_size, msg->parent.send_buf,
                   (sxfer->send_chunk - sxfer->add_frames) * sxfer->cell_size);
    }

    sxfer->xfer.send_length = sxfer->send_chunk;
    return RT_EOK;
}

static rt_err_t dw_spi_standard_prepare_rx(dw_spi_bus_t* spi_bus, struct rt_qspi_message* msg, dw_spi_standard_xfer_t* sxfer)
{
    if (!msg->parent.recv_buf) {
        return RT_EOK;
    }

    sxfer->recv_chunk = dw_spi_limit_xfer_frames(sxfer->total_frames);
    if (dw_spi_buf_aligned(msg->parent.recv_buf, sxfer->cell_size)) {
        sxfer->xfer.recv_buf = msg->parent.recv_buf;
    } else {
        sxfer->recv_alloc = dw_spi_get_pio_buf(spi_bus, sxfer->recv_chunk * sxfer->cell_size);
        if (sxfer->recv_alloc == NULL) {
            return -RT_ENOMEM;
        }
        sxfer->xfer.recv_buf = sxfer->recv_alloc;
    }

    sxfer->xfer.recv_length = sxfer->recv_chunk;
    return RT_EOK;
}

static rt_bool_t dw_spi_standard_reload_tx(dw_spi_reg_t* spi, struct rt_qspi_message* msg, dw_spi_standard_xfer_t* sxfer)
{
    rt_size_t data_offset;

    sxfer->send_done += sxfer->send_chunk;
    if (sxfer->send_done >= sxfer->send_total || sxfer->tmod > SPI_TMOD_TO) {
        return RT_FALSE;
    }

    data_offset       = sxfer->send_done > sxfer->add_frames ? sxfer->send_done - sxfer->add_frames : 0;
    sxfer->send_chunk = dw_spi_limit_xfer_frames(sxfer->send_total - sxfer->send_done);
    if (sxfer->tx_copy_payload) {
        rvv_memcpy(sxfer->send_alloc, (const uint8_t*)msg->parent.send_buf + data_offset * sxfer->cell_size,
                   sxfer->send_chunk * sxfer->cell_size);
        sxfer->xfer.send_buf = sxfer->send_alloc;
    } else {
        sxfer->xfer.send_buf = (void*)((const uint8_t*)msg->parent.send_buf + data_offset * sxfer->cell_size);
    }
    sxfer->xfer.send_length = sxfer->send_chunk;
    writel(dw_spi_make_txftlr(sxfer->send_chunk), &spi->txftlr);

    return RT_TRUE;
}

static rt_bool_t dw_spi_standard_advance_rx(dw_spi_reg_t* spi, struct rt_qspi_message* msg, dw_spi_standard_xfer_t* sxfer)
{
    if (sxfer->recv_alloc) {
        rvv_memcpy((uint8_t*)msg->parent.recv_buf + sxfer->recv_done * sxfer->cell_size, sxfer->recv_alloc,
                   sxfer->recv_chunk * sxfer->cell_size);
    }

    sxfer->recv_done += sxfer->recv_chunk;
    if (sxfer->recv_done >= sxfer->total_frames) {
        return RT_FALSE;
    }

    sxfer->recv_chunk = dw_spi_limit_xfer_frames(sxfer->total_frames - sxfer->recv_done);
    sxfer->xfer.recv_buf
        = sxfer->recv_alloc ? sxfer->recv_alloc : (void*)((uint8_t*)msg->parent.recv_buf + sxfer->recv_done * sxfer->cell_size);
    sxfer->xfer.recv_length = sxfer->recv_chunk;
    writel(sxfer->recv_chunk > DW_SSI_RX_FIFO_THRESH ? DW_SSI_RX_FIFO_THRESH : sxfer->recv_chunk - 1, &spi->rxftlr);

    return RT_TRUE;
}

static rt_bool_t dw_spi_standard_has_tx_pending(const dw_spi_standard_xfer_t* sxfer)
{
    if (sxfer->tmod == SPI_TMOD_RO) {
        return RT_FALSE;
    }

    return sxfer->xfer.send_length || (sxfer->send_done + sxfer->send_chunk) < sxfer->send_total;
}

static uint32_t dw_spi_standard_get_imr(const dw_spi_standard_xfer_t* sxfer)
{
    uint32_t mask = 0;

    if (sxfer->tmod == SPI_TMOD_RO || sxfer->tmod == SPI_TMOD_TR || sxfer->tmod == SPI_TMOD_EPROMREAD) {
        mask |= DW_SSI_IMR_RXF;
    }
    if (dw_spi_standard_has_tx_pending(sxfer)) {
        mask |= DW_SSI_IMR_TXE;
    }

    return mask;
}

static void dw_spi_standard_restart_read(dw_spi_reg_t* spi, dw_spi_standard_xfer_t* sxfer)
{
    writel(readl(&spi->imr) | DW_SSI_IMR_RXF, &spi->imr);
    writel(0, &spi->ssienr);
    if (sxfer->tmod == SPI_TMOD_EPROMREAD) {
        sxfer->tmod = SPI_TMOD_RO;
        writel((readl(&spi->ctrlr0) & ~DW_SSI_CTRLR0_TMOD_MASK) | (sxfer->tmod << DW_SSI_CTRLR0_TMOD_SHIFT), &spi->ctrlr0);
        writel(0, &spi->txftlr);
    }
    writel(sxfer->recv_chunk - 1, &spi->ctrlr1);
    writel(1, &spi->ssienr);
    writel(0, &spi->dr[0]);
}

static rt_err_t dw_spi_standard_run_to(dw_spi_reg_t* spi, dw_spi_bus_t* spi_bus, struct rt_qspi_message* msg,
                                       dw_spi_standard_xfer_t* sxfer)
{
    rt_uint32_t event;
    rt_err_t    err;

    rt_event_control(&spi_bus->event, RT_IPC_CMD_RESET, 0);
    writel(1, &spi->ssienr);
    if (dw_spi_fifo_push(&sxfer->xfer, spi) != RT_EOK) {
        LOG_E("spi%d tx fifo prime error", spi_bus->hw->idx);
        return -RT_ERROR;
    }

    if (sxfer->xfer.send_length == 0 && sxfer->send_total == sxfer->send_chunk) {
        writel(spi_bus->ser, &spi->ser);
        if (dw_spi_wait_tx_complete(spi, 500) != RT_EOK) {
            LOG_E("spi%d standard tx drain timeout", spi_bus->hw->idx);
            rt_set_errno(-RT_ETIMEOUT);
            return -RT_ETIMEOUT;
        }
        return RT_EOK;
    }

    writel(dw_spi_standard_get_imr(sxfer), &spi->imr);
    writel(spi_bus->ser, &spi->ser);

    while (1) {
        err = rt_event_recv(&spi_bus->event, BIT(SSI_TXE), RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 500, &event);
        if (err == -RT_ETIMEOUT) {
            LOG_E("spi%d transfer data timeout", spi_bus->hw->idx);
            rt_set_errno(-RT_ETIMEOUT);
            return -RT_ETIMEOUT;
        }

        if (dw_spi_fifo_push(&sxfer->xfer, spi) != RT_EOK) {
            LOG_E("spi%d tx fifo push error", spi_bus->hw->idx);
            return -RT_ERROR;
        }

        if (sxfer->xfer.send_length == 0) {
            if (!dw_spi_standard_reload_tx(spi, msg, sxfer)) {
                if (dw_spi_wait_tx_complete(spi, 500) != RT_EOK) {
                    LOG_E("spi%d standard tx drain timeout", spi_bus->hw->idx);
                    rt_set_errno(-RT_ETIMEOUT);
                    return -RT_ETIMEOUT;
                }
                return RT_EOK;
            }
        }

        writel(dw_spi_standard_get_imr(sxfer), &spi->imr);
    }
}

static rt_err_t dw_spi_standard_run_tr(dw_spi_reg_t* spi, dw_spi_bus_t* spi_bus, struct rt_qspi_message* msg,
                                       dw_spi_standard_xfer_t* sxfer)
{
    rt_uint32_t event;
    rt_err_t    err;

    rt_event_control(&spi_bus->event, RT_IPC_CMD_RESET, 0);
    writel(spi_bus->ser, &spi->ser);
    writel(1, &spi->ssienr);
    if (dw_spi_fifo_push(&sxfer->xfer, spi) != RT_EOK) {
        LOG_E("spi%d tx fifo prime error", spi_bus->hw->idx);
        return -RT_ERROR;
    }
    writel(dw_spi_standard_get_imr(sxfer), &spi->imr);

    while (1) {
        err = rt_event_recv(&spi_bus->event, BIT(SSI_TXE) | BIT(SSI_RXF), RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 500, &event);
        if (err == -RT_ETIMEOUT) {
            LOG_E("spi%d transfer data timeout", spi_bus->hw->idx);
            rt_set_errno(-RT_ETIMEOUT);
            return -RT_ETIMEOUT;
        }

        if (event & BIT(SSI_TXE)) {
            if (dw_spi_fifo_push(&sxfer->xfer, spi) != RT_EOK) {
                LOG_E("spi%d tx fifo push error", spi_bus->hw->idx);
                return -RT_ERROR;
            }

            if (sxfer->xfer.send_length == 0) {
                (void)dw_spi_standard_reload_tx(spi, msg, sxfer);
            }
            writel(dw_spi_standard_get_imr(sxfer), &spi->imr);
        }

        if (event & BIT(SSI_RXF)) {
            if (dw_spi_fifo_pop(&sxfer->xfer, spi) != RT_EOK) {
                LOG_E("spi%d rx fifo pop error", spi_bus->hw->idx);
                return -RT_ERROR;
            }

            if (sxfer->xfer.recv_length == 0) {
                if (!dw_spi_standard_advance_rx(spi, msg, sxfer)) {
                    return RT_EOK;
                }
            } else if (sxfer->xfer.recv_length <= readl(&spi->rxftlr)) {
                writel(sxfer->xfer.recv_length - 1, &spi->rxftlr);
            }
            writel(dw_spi_standard_get_imr(sxfer), &spi->imr);
        }
    }
}

static rt_err_t dw_spi_standard_run_ro(dw_spi_reg_t* spi, dw_spi_bus_t* spi_bus, struct rt_qspi_message* msg,
                                       dw_spi_standard_xfer_t* sxfer)
{
    rt_uint32_t event;
    rt_err_t    err;

    (void)msg;

    rt_event_control(&spi_bus->event, RT_IPC_CMD_RESET, 0);
    writel(spi_bus->ser, &spi->ser);
    writel(1, &spi->ssienr);
    writel(dw_spi_standard_get_imr(sxfer), &spi->imr);
    writel(0, &spi->txftlr);
    writel(0, &spi->dr[0]);

    while (1) {
        err = rt_event_recv(&spi_bus->event, BIT(SSI_RXF), RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 500, &event);
        if (err == -RT_ETIMEOUT) {
            LOG_E("spi%d transfer data timeout", spi_bus->hw->idx);
            rt_set_errno(-RT_ETIMEOUT);
            return -RT_ETIMEOUT;
        }

        if (dw_spi_fifo_pop(&sxfer->xfer, spi) != RT_EOK) {
            LOG_E("spi%d rx fifo pop error", spi_bus->hw->idx);
            return -RT_ERROR;
        }

        if (sxfer->xfer.recv_length == 0) {
            if (!dw_spi_standard_advance_rx(spi, msg, sxfer)) {
                return RT_EOK;
            }
            dw_spi_standard_restart_read(spi, sxfer);
        } else {
            if (sxfer->xfer.recv_length <= readl(&spi->rxftlr)) {
                writel(sxfer->xfer.recv_length - 1, &spi->rxftlr);
            }
            writel(dw_spi_standard_get_imr(sxfer), &spi->imr);
        }
    }
}

static rt_err_t dw_spi_standard_run_epromread(dw_spi_reg_t* spi, dw_spi_bus_t* spi_bus, struct rt_qspi_message* msg,
                                              dw_spi_standard_xfer_t* sxfer)
{
    rt_uint32_t event;
    rt_err_t    err;

    rt_event_control(&spi_bus->event, RT_IPC_CMD_RESET, 0);
    writel(spi_bus->ser, &spi->ser);
    writel(1, &spi->ssienr);
    if (dw_spi_fifo_push(&sxfer->xfer, spi) != RT_EOK) {
        LOG_E("spi%d tx fifo prime error", spi_bus->hw->idx);
        return -RT_ERROR;
    }
    writel(dw_spi_standard_get_imr(sxfer), &spi->imr);

    while (1) {
        err = rt_event_recv(&spi_bus->event, BIT(SSI_TXE) | BIT(SSI_RXF), RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 500, &event);
        if (err == -RT_ETIMEOUT) {
            LOG_E("spi%d transfer data timeout", spi_bus->hw->idx);
            rt_set_errno(-RT_ETIMEOUT);
            return -RT_ETIMEOUT;
        }

        if (event & BIT(SSI_TXE)) {
            if (dw_spi_fifo_push(&sxfer->xfer, spi) != RT_EOK) {
                LOG_E("spi%d tx fifo push error", spi_bus->hw->idx);
                return -RT_ERROR;
            }
            writel(dw_spi_standard_get_imr(sxfer), &spi->imr);
        }

        if (event & BIT(SSI_RXF)) {
            if (dw_spi_fifo_pop(&sxfer->xfer, spi) != RT_EOK) {
                LOG_E("spi%d rx fifo pop error", spi_bus->hw->idx);
                return -RT_ERROR;
            }

            if (sxfer->xfer.recv_length == 0) {
                if (!dw_spi_standard_advance_rx(spi, msg, sxfer)) {
                    return RT_EOK;
                }
                dw_spi_standard_restart_read(spi, sxfer);
            } else {
                if (sxfer->xfer.recv_length <= readl(&spi->rxftlr)) {
                    writel(sxfer->xfer.recv_length - 1, &spi->rxftlr);
                }
                writel(dw_spi_standard_get_imr(sxfer), &spi->imr);
            }
        }
    }
}

static rt_err_t drv_spi_configure(struct rt_spi_device* device, struct rt_spi_configuration* configuration)
{
    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(configuration != RT_NULL);
    dw_spi_bus_t*                 spi_bus = (dw_spi_bus_t*)device->bus;
    dw_spi_reg_t*                 spi     = (dw_spi_reg_t*)spi_bus->base;
    struct rt_qspi_device*        dev     = (struct rt_qspi_device*)device;
    struct rt_qspi_configuration* cfg     = (struct rt_qspi_configuration*)configuration;
    uint8_t                       dfs, mode;
    uint32_t                      max_hz, spi_clock, div;

    if (cfg->qspi_dl_width > spi_bus->hw->max_line || cfg->qspi_dl_width == 0) {
        return -RT_EINVAL;
    }

    if (configuration->data_width < 4 || configuration->data_width > 32) {
        return -RT_EINVAL;
    }
    dfs = configuration->data_width - 1;

    max_hz = configuration->max_hz;
    if (max_hz > spi_bus->hw->max_hz) {
        max_hz = spi_bus->hw->max_hz;
    }
    spi_clock = sysctl_clk_get_leaf_freq(spi_bus->hw->clk);
    div       = spi_clock / max_hz;

    mode = configuration->mode & RT_SPI_MODE_3;

    if (configuration->soft_cs & 0x80) {
        uint8_t cs_pin = configuration->soft_cs & 0x7F;

        if (63 < cs_pin) {
            LOG_E("invalid cs pin %d", cs_pin);
        } else {
            kd_pin_mode(cs_pin, GPIO_DM_OUTPUT);
            kd_pin_write(cs_pin, configuration->mode & RT_SPI_CS_HIGH ? GPIO_PV_LOW : GPIO_PV_HIGH);
        }
    }

    writel(0, &spi->ssienr);
    writel(0, &spi->ser);
    writel(div, &spi->baudr);
    writel(spi_bus->rdse << 16 | spi_bus->rdsd, &spi->rx_sample_delay);
    writel(SSIC_AXI_BLW << 8, &spi->axiawlen);
    writel(SSIC_AXI_BLW << 8, &spi->axiarlen);
    writel((dfs) | (mode << 8), &spi->ctrlr0);
    spi_bus->ser = dw_spi_get_ser(spi_bus, cfg);

    rt_memcpy(&dev->config, cfg, sizeof(struct rt_qspi_configuration));

    return 0;
}

static rt_uint32_t drv_spi_xfer(struct rt_spi_device* device, struct rt_spi_message* message)
{
    dw_spi_bus_t*                 spi_bus       = (dw_spi_bus_t*)device->bus;
    struct rt_qspi_device*        dev           = (struct rt_qspi_device*)device;
    struct rt_qspi_message*       msg           = (struct rt_qspi_message*)message;
    struct rt_qspi_message        kmsg          = *msg;
    struct rt_qspi_configuration* cfg           = (struct rt_qspi_configuration*)&dev->config;
    void*                         tx_alloc      = RT_NULL;
    void*                         rx_alloc      = RT_NULL;
    void*                         user_recv_buf = msg->parent.recv_buf;
    rt_uint32_t                   length;

    if (dw_spi_prepare_user_xfer(&kmsg, &tx_alloc, &rx_alloc) != RT_EOK) {
        length = 0;
        goto cleanup;
    }

    if (kmsg.qspi_data_lines > 1) {
        length = drv_spi_xfer_enhanced(spi_bus, cfg, &kmsg);
    } else {
        length = drv_spi_xfer_standard(spi_bus, cfg, &kmsg);
    }

    if (dw_spi_finish_user_xfer(&kmsg, user_recv_buf, length, rx_alloc) != RT_EOK) {
        length = 0;
    }

cleanup:
    if (tx_alloc != RT_NULL) {
        rt_free_align(tx_alloc);
    }
    if (rx_alloc != RT_NULL) {
        rt_free_align(rx_alloc);
    }

    return length;
}

static rt_uint32_t drv_spi_xfer_enhanced(dw_spi_bus_t* spi_bus, struct rt_qspi_configuration* cfg, struct rt_qspi_message* msg)
{
    dw_spi_reg_t*       spi  = (dw_spi_reg_t*)spi_bus->base;
    dw_spi_xfer_state_t xfer = { 0 };

    uint8_t     spi_ff;
    uint8_t     trans_type  = 0;
    uint8_t     tmod        = msg->parent.recv_buf ? SPI_TMOD_RO : SPI_TMOD_TO;
    rt_size_t   total_bytes = msg->parent.length;
    rt_size_t   length      = 0;
    uint8_t     cell_size   = (cfg->parent.data_width + 7) >> 3;
    void*       dma_buf     = NULL;
    rt_uint32_t ret_length  = (rt_uint32_t)total_bytes;
    rt_err_t    err;

    err = dw_spi_get_xfer_frames(total_bytes, cell_size, &length);
    if (err != RT_EOK) {
        LOG_E("spi%d xfer length %u is not aligned to data width %u", spi_bus->hw->idx, (uint32_t)total_bytes,
              cfg->parent.data_width);
        return 0;
    }

    if (length > DW_SSI_MAX_XFER_FRAMES) {
        LOG_E("spi%d enhanced xfer length %u exceeds single transaction, caller must chunk", spi_bus->hw->idx,
              (uint32_t)total_bytes);
        return 0;
    }

    if (msg->qspi_data_lines > cfg->qspi_dl_width) {
        LOG_E("data line is invalid");
        return 0;
    }
    if (msg->qspi_data_lines == 2)
        spi_ff = SPI_FRF_DUAL_SPI;
    else if (msg->qspi_data_lines == 4)
        spi_ff = SPI_FRF_QUAD_SPI;
    else if (msg->qspi_data_lines == 8)
        spi_ff = SPI_FRF_OCT_SPI;
    else {
        LOG_E("data line is invalid");
        return 0;
    }
    if (cfg->parent.data_width & (msg->qspi_data_lines - 1)) {
        LOG_E("data line and data width do not match");
        return 0;
    }
    if (msg->instruction.size & 3 || msg->instruction.size > 16 || msg->instruction.size == 12) {
        LOG_E("instruction size is invalid");
        return 0;
    }
    if (msg->instruction.size && msg->instruction.qspi_lines != 1 && msg->instruction.qspi_lines != msg->qspi_data_lines) {
        LOG_E("instruction line is invalid");
        return 0;
    }
    if (msg->address.size & 3 || msg->address.size > 32) {
        LOG_E("address size is invalid");
        return 0;
    }
    if (msg->address.size && msg->address.qspi_lines != 1 && msg->address.qspi_lines != msg->qspi_data_lines) {
        LOG_E("address line is invalid");
        return 0;
    }
    if (msg->instruction.size + msg->address.size == 0 && length == 0) {
        LOG_E("enhanced xfer requires inst, addr, or data");
        return 0;
    }
    if (msg->instruction.size && msg->instruction.qspi_lines != 1) {
        trans_type = 2;
    }
    if (msg->address.size) {
        if (msg->address.qspi_lines != 1)
            trans_type = trans_type ? trans_type : 1;
        else if (trans_type != 0) {
            LOG_E("instruction or address line is invalid");
            return 0;
        }
    }
    if (msg->dummy_cycles > 31) {
        LOG_E("dummy cycle is invalid");
        return 0;
    }
    writel(readl(&spi->ctrlr0) & ~(DW_SSI_CTRLR0_SPI_FRF_MASK | DW_SSI_CTRLR0_TMOD_MASK), &spi->ctrlr0);

    if (length) {
        rt_size_t   txfthr     = length > (SSIC_TX_ABW / 2) ? (SSIC_TX_ABW / 2) : length - 1;
        rt_uint32_t event      = 0;
        uint32_t    inst_size  = msg->instruction.size;
        uint32_t    addr_size  = msg->address.size;
        uint32_t    inst_data  = msg->instruction.content;
        uint32_t    addr_data  = msg->address.content;
        rt_size_t   dma_bytes  = total_bytes;
        rt_size_t   dma_frames = length;
        const void* src_buf    = msg->parent.send_buf;
        void*       dst_buf    = msg->parent.recv_buf;
        uint32_t    spi_ctrlr0;
        rt_bool_t   dma_copy_back = RT_FALSE;

        spi_ctrlr0 = trans_type | DW_SSI_SPI_CTRLR0_ADDR_L(addr_size) | DW_SSI_SPI_CTRLR0_INST_L(inst_size)
            | DW_SSI_SPI_CTRLR0_WAIT_CYCLES(msg->dummy_cycles);

        writel(0, &spi->ssienr);
        writel(spi_ctrlr0, &spi->spi_ctrlr0);

        writel((readl(&spi->ctrlr0) & ~(DW_SSI_CTRLR0_SPI_FRF_MASK | DW_SSI_CTRLR0_TMOD_MASK))
                   | (spi_ff << DW_SSI_CTRLR0_SPI_FRF_SHIFT) | (tmod << DW_SSI_CTRLR0_TMOD_SHIFT),
               &spi->ctrlr0);
        writel((txfthr << 16) | (SSIC_TX_ABW / 2), &spi->txftlr);
        writel(SSIC_RX_ABW - 1, &spi->rxftlr);
        writel(dma_frames ? dma_frames - 1 : 0, &spi->ctrlr1);

        writel(DW_SSI_IMR_DONE | DW_SSI_IMR_AXIE, &spi->imr);
        writel(inst_data, &spi->spidr);
        writel(addr_data, &spi->spiar);

        if (tmod == SPI_TMOD_TO) {
            uint32_t atw;

            if (src_buf == NULL) {
                ret_length = 0;
                goto multi_cleanup;
            }

            if (dw_spi_buf_aligned(src_buf, cell_size)) {
                dma_buf = (void*)src_buf;
            } else {
                dma_buf = dw_spi_get_dma_buf(spi_bus, dma_bytes);
                if (dma_buf == NULL) {
                    LOG_E("spi%d alloc dma bounce buf failed", spi_bus->hw->idx);
                    ret_length = 0;
                    goto multi_cleanup;
                }
                rvv_memcpy(dma_buf, src_buf, dma_bytes);
            }

            atw = dw_spi_get_dma_atw(dma_buf, dma_bytes);
            writel(dw_spi_get_axi_burst_len(dma_bytes, atw) << 8, &spi->axiarlen);
            rt_hw_cpu_dcache_clean(dma_buf, dma_bytes);
        } else {
            uint32_t atw;

            if (dst_buf == NULL) {
                ret_length = 0;
                goto multi_cleanup;
            }

            if (dw_spi_buf_aligned(dst_buf, cell_size)) {
                dma_buf = (void*)dst_buf;
            } else {
                dma_buf = dw_spi_get_dma_buf(spi_bus, total_bytes);
                if (dma_buf == NULL) {
                    LOG_E("spi%d alloc dma bounce buf failed", spi_bus->hw->idx);
                    ret_length = 0;
                    goto multi_cleanup;
                }

                dma_copy_back = RT_TRUE;
            }

            atw = dw_spi_get_dma_atw(dma_buf, total_bytes);
            writel(dw_spi_get_axi_burst_len(total_bytes, atw) << 8, &spi->axiawlen);
            rt_hw_cpu_dcache_clean(dma_buf, total_bytes);
        }

        dw_spi_set_axi_addr(spi, dma_buf);
        writel((1U << 6) | (dw_spi_get_dma_atw(dma_buf, tmod == SPI_TMOD_TO ? dma_bytes : total_bytes) << 3)
                   | DW_SSI_DMACR_IDMAE,
               &spi->dmacr);
        rt_event_control(&spi_bus->event, RT_IPC_CMD_RESET, 0);

        /* cs take */
        if (msg->parent.cs_take) {
            dw_spi_set_soft_cs(cfg, RT_TRUE);
        }
        writel(spi_bus->ser, &spi->ser);

        writel(1, &spi->ssienr); /* start transfer */
        err = rt_event_recv(&spi_bus->event, BIT(SSI_DONE) | BIT(SSI_AXIE), RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 1000,
                            &event);
        if (err == -RT_ETIMEOUT) {
            LOG_E("spi%d enhanced dma timeout", spi_bus->hw->idx);
            ret_length = 0;
            goto multi_cleanup;
        }
        if (event & BIT(SSI_AXIE)) {
            LOG_E("spi%d dma error", spi_bus->hw->idx);
            ret_length = 0;
            goto multi_cleanup;
        }

        if (tmod == SPI_TMOD_RO) {
            rt_hw_cpu_dcache_invalidate(dma_buf, total_bytes);
            if (dma_copy_back) {
                rvv_memcpy(msg->parent.recv_buf, dma_buf, total_bytes);
            }
        }
    } else {
        uint32_t spi_ctrlr0;
        uint8_t  dfs_bits;
        uint32_t addr_left;

        writel(0, &spi->ssienr);

        spi_ctrlr0 = trans_type | DW_SSI_SPI_CTRLR0_ADDR_L(msg->address.size) | DW_SSI_SPI_CTRLR0_INST_L(msg->instruction.size)
            | DW_SSI_SPI_CTRLR0_WAIT_CYCLES(msg->dummy_cycles);
        writel(spi_ctrlr0, &spi->spi_ctrlr0);
        writel(readl(&spi->ctrlr0) | (spi_ff << DW_SSI_CTRLR0_SPI_FRF_SHIFT) | (SPI_TMOD_TO << DW_SSI_CTRLR0_TMOD_SHIFT),
               &spi->ctrlr0);
        writel(0, &spi->ctrlr1);
        writel(0, &spi->txftlr);
        writel(SSIC_RX_ABW - 1, &spi->rxftlr);
        writel(0, &spi->imr);
        writel(0, &spi->dmacr);

        /* cs take */
        if (msg->parent.cs_take) {
            dw_spi_set_soft_cs(cfg, RT_TRUE);
        }

        writel(spi_bus->ser, &spi->ser);
        writel(1, &spi->ssienr);

        dfs_bits = (uint8_t)(cell_size << 3);

        {
            uint32_t inst_left = msg->instruction.size;
            while (inst_left) {
                uint32_t take  = inst_left >= dfs_bits ? dfs_bits : inst_left;
                uint32_t shift = inst_left - take;
                uint32_t mask  = take >= 32 ? 0xFFFFFFFFu : ((1u << take) - 1u);

                writel((msg->instruction.content >> shift) & mask, &spi->dr[0]);
                inst_left -= take;
            }
        }

        addr_left = msg->address.size;
        while (addr_left) {
            uint32_t take  = addr_left >= dfs_bits ? dfs_bits : addr_left;
            uint32_t shift = addr_left - take;
            uint32_t mask  = take >= 32 ? 0xFFFFFFFFu : ((1u << take) - 1u);

            writel((msg->address.content >> shift) & mask, &spi->dr[0]);
            addr_left -= take;
        }

        if (dw_spi_wait_tx_complete(spi, 500) != RT_EOK) {
            LOG_E("spi%d enhanced inst/addr drain timeout", spi_bus->hw->idx);
            ret_length = 0;
        } else {
            ret_length = (msg->instruction.size + msg->address.size) / 8;
        }
    }
multi_cleanup:
    writel(0, &spi->imr);
    writel(0, &spi->dmacr);
    writel(0, &spi->ssienr);
    writel(0, &spi->ser);

    if (msg->parent.cs_release) {
        dw_spi_set_soft_cs(cfg, RT_FALSE);
    }

multi_exit:

    return ret_length;
}

static rt_uint32_t drv_spi_xfer_standard(dw_spi_bus_t* spi_bus, struct rt_qspi_configuration* cfg, struct rt_qspi_message* msg)
{
    dw_spi_reg_t*          spi   = (dw_spi_reg_t*)spi_bus->base;
    dw_spi_standard_xfer_t sxfer = { 0 };
    rt_size_t              frame_chunk;
    rt_bool_t              has_send    = msg->parent.send_buf != NULL;
    rt_bool_t              has_recv    = msg->parent.recv_buf != NULL;
    rt_size_t              total_bytes = msg->parent.length;
    rt_uint32_t            ret_length  = (rt_uint32_t)total_bytes;
    rt_err_t               err;

    if (msg->parent.cs_take) {
        dw_spi_set_soft_cs(cfg, RT_TRUE);
    }

    sxfer.cell_size = (cfg->parent.data_width + 7) >> 3;
    err             = dw_spi_get_xfer_frames(total_bytes, sxfer.cell_size, &sxfer.total_frames);
    if (err != RT_EOK) {
        LOG_E("spi%d xfer length %u is not aligned to data width %u", spi_bus->hw->idx, (uint32_t)total_bytes,
              cfg->parent.data_width);
        ret_length = 0;
        goto single_exit;
    }

    err = dw_spi_standard_get_add_frames(spi_bus, cfg, msg, &sxfer.add_frames);
    if (err != RT_EOK) {
        ret_length = 0;
        goto single_exit;
    }

    if (!has_send && !has_recv && !msg->instruction.size && !msg->address.size && !msg->dummy_cycles) {
        goto single_exit;
    }

    if (has_send && has_recv)
        sxfer.tmod = SPI_TMOD_TR;
    else if (has_send)
        sxfer.tmod = SPI_TMOD_TO;
    else if (has_recv)
        sxfer.tmod = sxfer.add_frames ? SPI_TMOD_EPROMREAD : SPI_TMOD_RO;
    else if (sxfer.add_frames)
        sxfer.tmod = SPI_TMOD_TO;
    else
        sxfer.tmod = SPI_TMOD_RO;

    if (sxfer.tmod == SPI_TMOD_TR && sxfer.add_frames) {
        LOG_E("For read_write mode, instruction, address, dummy_cycles must be set to zero");
        ret_length = 0;
        goto single_exit;
    }
    frame_chunk = dw_spi_limit_xfer_frames(sxfer.total_frames);

    err = dw_spi_standard_prepare_tx(spi_bus, msg, &sxfer);
    if (err != RT_EOK) {
        if (err == -RT_ENOMEM)
            LOG_E("alloc tx bounce buffer error");
        else
            LOG_E("invalid tx message");
        ret_length = 0;
        goto single_exit;
    }

    err = dw_spi_standard_prepare_rx(spi_bus, msg, &sxfer);
    if (err != RT_EOK) {
        if (err == -RT_ENOMEM)
            LOG_E("alloc rx bounce buffer error");
        else
            LOG_E("invalid rx message");
        ret_length = 0;
        goto single_done;
    }

    if (sxfer.tmod == SPI_TMOD_TO) {
        frame_chunk = sxfer.send_chunk - sxfer.add_frames;
    }

    sxfer.xfer.cell_size = sxfer.cell_size;
    /* Standard transfers must not inherit enhanced instruction/address state. */
    writel(0, &spi->spi_ctrlr0);
    writel(0, &spi->spidr);
    writel(0, &spi->spiar);
    writel((readl(&spi->ctrlr0) & ~(DW_SSI_CTRLR0_SPI_FRF_MASK | DW_SSI_CTRLR0_TMOD_MASK))
               | (sxfer.tmod << DW_SSI_CTRLR0_TMOD_SHIFT),
           &spi->ctrlr0);

    writel(frame_chunk ? frame_chunk - 1 : 0, &spi->ctrlr1);
    writel(0, &spi->dmacr);

    writel(dw_spi_make_txftlr(sxfer.send_chunk), &spi->txftlr);
    writel(frame_chunk ? (frame_chunk > DW_SSI_RX_FIFO_THRESH ? DW_SSI_RX_FIFO_THRESH : frame_chunk - 1) : 0, &spi->rxftlr);
    writel(0, &spi->imr);

    switch (sxfer.tmod) {
    case SPI_TMOD_TO:
        err = dw_spi_standard_run_to(spi, spi_bus, msg, &sxfer);
        break;
    case SPI_TMOD_TR:
        err = dw_spi_standard_run_tr(spi, spi_bus, msg, &sxfer);
        break;
    case SPI_TMOD_RO:
        err = dw_spi_standard_run_ro(spi, spi_bus, msg, &sxfer);
        break;
    case SPI_TMOD_EPROMREAD:
        err = dw_spi_standard_run_epromread(spi, spi_bus, msg, &sxfer);
        break;
    default:
        err = -RT_EINVAL;
        break;
    }
    if (err != RT_EOK) {
        ret_length = 0;
    }
single_done:
    writel(0, &spi->ssienr);
    writel(0, &spi->ser);
single_exit:
    if (msg->parent.cs_release) {
        dw_spi_set_soft_cs(cfg, RT_FALSE);
    }
    return ret_length;
}

static void spi_irq(int vector, void* param)
{
    dw_spi_bus_t* spi_bus = param;
    dw_spi_reg_t* spi     = (dw_spi_reg_t*)spi_bus->base;
    uint32_t      status  = readl(&spi->isr);
    uint32_t      event   = 0;

    vector -= spi_bus->hw->irq_base;
    if (vector < 0 || vector >= DW_SSI_IRQ_BANK_STRIDE) {
        return;
    }

    if (!(status & (DW_SSI_IMR_TXE | DW_SSI_IMR_RXF | DW_SSI_IMR_DONE | DW_SSI_IMR_AXIE))) {
        return;
    }

    if (status & DW_SSI_IMR_TXE) {
        writel(readl(&spi->imr) & ~DW_SSI_IMR_TXE, &spi->imr);
        event |= BIT(SSI_TXE);
    }
    if (status & DW_SSI_IMR_RXF) {
        writel(readl(&spi->imr) & ~DW_SSI_IMR_RXF, &spi->imr);
        event |= BIT(SSI_RXF);
    }
    if (status & DW_SSI_IMR_DONE) {
        readl(&spi->donecr);
        event |= BIT(SSI_DONE);
    }
    if (status & DW_SSI_IMR_AXIE) {
        readl(&spi->axiecr);
        event |= BIT(SSI_AXIE);
    }

    if (event) {
        rt_event_send(&spi_bus->event, event);
    }
}

static struct rt_spi_ops spi_ops = { drv_spi_configure, drv_spi_xfer };

static rt_err_t dw_spi_register_bus(dw_spi_bus_t* spi_bus, const dw_spi_hw_info_t* info, uint8_t rdsd)
{
    rt_err_t ret;

    spi_bus->base = rt_ioremap((void*)info->base, SPI_OPI_IO_SIZE);
    spi_bus->hw   = info;
    spi_bus->rdse = 0;
    spi_bus->rdsd = rdsd;

    ret = rt_qspi_bus_register(&spi_bus->dev, info->name, &spi_ops);
    if (ret) {
        LOG_E("%s register fail", info->name);
        return ret;
    }

    rt_event_init(&spi_bus->event, info->name, RT_IPC_FLAG_PRIO);
    for (rt_size_t i = 0; i < sizeof(dw_spi_irq_offsets) / sizeof(dw_spi_irq_offsets[0]); i++) {
        int irq = info->irq_base + dw_spi_irq_offsets[i];

        rt_hw_interrupt_install(irq, spi_irq, spi_bus, info->name);
        rt_hw_interrupt_umask(irq);
    }

    return RT_EOK;
}

static const dw_spi_hw_info_t dw_spi_hw_info[] = {
    { "spi0", SPI_OPI_BASE_ADDR, IRQN_SPI0, SYSCTL_CLK_SSI_0_CLK, 0, 8, 200000000 },
    { "spi1", SPI_QOPI_BASE_ADDR, IRQN_SPI1, SYSCTL_CLK_SSI_1_CLK, 1, 4, 100000000 },
    { "spi2", SPI_QOPI_BASE_ADDR + SPI_OPI_IO_SIZE, IRQN_SPI2, SYSCTL_CLK_SSI_2_CLK, 2, 4, 100000000 },
};

static uint32_t dw_spi_get_ser(dw_spi_bus_t* spi_bus, struct rt_qspi_configuration* cfg)
{
    uint32_t ser = cfg->parent.hard_cs & DW_SPI_HARD_CS_MASK;

    if (ser)
        return ser;

    if (cfg->parent.soft_cs & 0x80) {
        ser = dw_spi_find_ser_by_fpioa(spi_bus, RT_FALSE);
        if (ser)
            return ser;
    }

    ser = dw_spi_find_ser_by_fpioa(spi_bus, RT_TRUE);
    return ser ? ser : BIT(0);
}

static void dw_spi_set_soft_cs(struct rt_qspi_configuration* cfg, rt_bool_t asserted)
{
    uint8_t   cs_pin;
    rt_bool_t active_high;

    if (!(cfg->parent.soft_cs & 0x80))
        return;

    cs_pin = cfg->parent.soft_cs & 0x7F;
    if (cs_pin > 63) {
        LOG_E("invalid cs pin %d", cs_pin);
        return;
    }

    active_high = !!(cfg->parent.mode & RT_SPI_CS_HIGH);
    kd_pin_write(cs_pin, asserted == active_high ? GPIO_PV_HIGH : GPIO_PV_LOW);
}

int rt_hw_spi_bus_init(void)
{
    rt_err_t ret;
#ifdef RT_USING_SPI0
    static dw_spi_bus_t spi_bus0;
    ret = dw_spi_register_bus(&spi_bus0, &dw_spi_hw_info[0], RT_SPI0_RXSD);
    if (ret)
        return ret;
#endif
#ifdef RT_USING_SPI1
    static dw_spi_bus_t spi_bus1;
    ret = dw_spi_register_bus(&spi_bus1, &dw_spi_hw_info[1], RT_SPI1_RXSD);
    if (ret)
        return ret;
#endif
#ifdef RT_USING_SPI2
    static dw_spi_bus_t spi_bus2;
    ret = dw_spi_register_bus(&spi_bus2, &dw_spi_hw_info[2], RT_SPI2_RXSD);
    if (ret)
        return ret;
#endif
    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_spi_bus_init);
