/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
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
#include <rthw.h>
#include <ipc/completion.h>
#include <cache.h>
#include "ioremap.h"
#include "riscv_io.h"
#include "board.h"
#include "drv_pdma.h"

#define DBG_TAG "pdma"
#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#define DBG_COLOR
#include <rtdbg.h>

#define PDMA_CH0_IRQ 139
#define PDMA_CH1_IRQ 196

#define PDMA_BASE_ADDR 0x80804000
#define PDMA_IO_SIZE 0x200
#define PDMA_MAX_LINE_SIZE 0x3FFFFFFF
#define PDMA_CH_MASK ((1U << PDMA_CH_MAX) - 1U)
/* Preallocate enough 30-bit descriptors for any 32-bit transfer. */
#define PDMA_LLT_MAX_ENTRIES ((RT_UINT32_MAX / PDMA_MAX_LINE_SIZE) + 1U)
#define PDMA_COMPLETION_QUEUE_DEPTH PDMA_CH_MAX
#define PDMA_REQUEST_TIMEOUT_MS 1000
#define PDMA_SELFTEST_UART2_TX_ADDR (UART2_BASE_ADDR + 0x30U)
#define PDMA_SELFTEST_UART3_TX_ADDR (UART3_BASE_ADDR + 0x30U)
#define PDMA_UART_LOOPBACK_SIZE RT_CPU_CACHE_LINE_SZ
#define PDMA_SELFTEST_REARM_SIZE 32U
#define PDMA_SELFTEST_TIMEOUT_MS 1000
#define PDMA_UART_FCR_OFFSET 0x08U
#define PDMA_UART_FCR_FIFO_RESET 0x07U
#define PDMA_UART_FCR_RX_TRIGGER_16 0x87U

/* interrupt mask */
#define PDONE_INT 0x00000001
#define PITEM_INT 0x00000100
#define PPAUSE_INT 0x00010000
#define PTOUT_INT 0x01000000
#define PALL_INT 0x01010101

/* register structure */
typedef struct pdma_ch_reg {
    rt_uint32_t ch_ctl;
    rt_uint32_t ch_status;
    pdma_ch_cfg_t ch_cfg;
    rt_uint32_t ch_llt_saddr;
    rt_uint32_t reserved[4];
} pdma_ch_reg_t;

typedef struct pdma_reg {
    rt_uint32_t pdma_ch_en;
    rt_uint32_t dma_int_mask;
    rt_uint32_t dma_int_stat;
    rt_uint32_t reserved[5];
    pdma_ch_reg_t pdma_ch_reg[8];
    rt_uint32_t ch_peri_dev_sel[8];
} pdma_reg_t;

/* llt structure */
typedef struct pdma_llt {
    rt_uint32_t line_size : 30;
    rt_uint32_t pause : 1;
    rt_uint32_t node_intr : 1;
    rt_uint32_t src_addr;
    rt_uint32_t dst_addr;
    rt_uint32_t next_llt_addr;
} pdma_llt_t;

typedef struct {
    rt_uint32_t generation;
    rt_uint32_t status;
} pdma_completion_t;

typedef struct {
    pdma_reg_t* reg;
    struct rt_mutex lifecycle_lock;
#ifdef RT_USING_SMP
    struct rt_spinlock state_lock;
#else
    rt_spinlock_t state_lock;
#endif
    struct rt_event event;
    struct {
        int irq;
        char* name;
        void (*callback)(void* param);
        void* param;
        rt_bool_t configured;
        rt_bool_t running;
        rt_uint32_t generation;
        rt_uint32_t completion_head;
        rt_uint32_t completion_count;
        pdma_completion_t completions[PDMA_COMPLETION_QUEUE_DEPTH];
        rt_bool_t waiter_active;
        rt_uint32_t waiter_generation;
        rt_uint32_t waiter_status;
        rt_uint32_t callback_count;
        struct rt_completion callbacks_done;
        pdma_llt_t list[PDMA_LLT_MAX_ENTRIES] ALIGN(RT_ALIGN_SIZE);
    } chan[PDMA_CH_MAX];
} pdma_dev_t;
static pdma_dev_t pdma_dev;

static rt_bool_t pdma_chan_valid(int chan)
{
    return chan >= PDMA_CH_0 && chan < PDMA_CH_MAX;
}

static rt_bool_t pdma_chan_active(int chan)
{
    return (pdma_dev.reg->pdma_ch_en & (1U << chan)) != 0U;
}

static rt_uint32_t pdma_chan_int_mask(int chan)
{
    return PALL_INT << chan;
}

static void pdma_completion_reset(int chan)
{
    pdma_dev.chan[chan].completion_head = 0;
    pdma_dev.chan[chan].completion_count = 0;
}

static rt_bool_t pdma_completion_peek(int chan, rt_uint32_t* generation,
                                      rt_uint32_t* status)
{
    rt_uint32_t head;

    if (pdma_dev.chan[chan].completion_count == 0)
        return RT_FALSE;

    head = pdma_dev.chan[chan].completion_head;
    *generation = pdma_dev.chan[chan].completions[head].generation;
    *status = pdma_dev.chan[chan].completions[head].status;
    return RT_TRUE;
}

static rt_bool_t pdma_completion_push(int chan, rt_uint32_t generation,
                                      rt_uint32_t status)
{
    rt_uint32_t tail;

    if (pdma_dev.chan[chan].completion_count >=
        PDMA_COMPLETION_QUEUE_DEPTH)
        return RT_FALSE;

    tail = (pdma_dev.chan[chan].completion_head +
            pdma_dev.chan[chan].completion_count) %
           PDMA_COMPLETION_QUEUE_DEPTH;
    pdma_dev.chan[chan].completions[tail].generation = generation;
    pdma_dev.chan[chan].completions[tail].status = status;
    pdma_dev.chan[chan].completion_count++;
    return RT_TRUE;
}

static rt_bool_t pdma_completion_pop(int chan, rt_uint32_t generation)
{
    rt_uint32_t head;

    if (pdma_dev.chan[chan].completion_count == 0)
        return RT_FALSE;

    head = pdma_dev.chan[chan].completion_head;
    if (pdma_dev.chan[chan].completions[head].generation != generation)
        return RT_FALSE;

    pdma_dev.chan[chan].completion_head =
        (head + 1U) % PDMA_COMPLETION_QUEUE_DEPTH;
    pdma_dev.chan[chan].completion_count--;
    return RT_TRUE;
}

static rt_base_t pdma_spin_lock(void)
{
    return rt_spin_lock_irqsave(&pdma_dev.state_lock);
}

static void pdma_spin_unlock(rt_base_t level)
{
    rt_spin_unlock_irqrestore(&pdma_dev.state_lock, level);
}

static int pdma_lifecycle_lock(rt_int32_t timeout)
{
    rt_err_t ret;

    ret = rt_mutex_take(&pdma_dev.lifecycle_lock, timeout);
    if (ret == RT_EOK)
        return 0;
    if (ret == -RT_ETIMEOUT)
        return -RT_ETIMEOUT;

    return -RT_ERROR;
}

static void pdma_lifecycle_unlock(void)
{
    rt_mutex_release(&pdma_dev.lifecycle_lock);
}

static rt_int32_t pdma_wait_remaining(rt_tick_t started, rt_int32_t timeout)
{
    rt_tick_t elapsed;

    if (timeout <= 0)
        return timeout;

    elapsed = rt_tick_get() - started;
    return elapsed < (rt_tick_t)timeout
               ? timeout - (rt_int32_t)elapsed : 0;
}

int rt_dma_chan_request(char* name)
{
    char* chan_name;
    rt_base_t level;
    rt_uint32_t idle;
    int chan;
    int ret;

    chan_name = rt_strdup(name ? name : "anonymous");
    if (chan_name == RT_NULL)
        return -RT_ENOMEM;

    ret = pdma_lifecycle_lock(
        rt_tick_from_millisecond(PDMA_REQUEST_TIMEOUT_MS));
    if (ret != 0) {
        rt_free(chan_name);
        return ret;
    }

    level = pdma_spin_lock();
    idle = (~pdma_dev.reg->pdma_ch_en) & PDMA_CH_MASK;
    chan = __builtin_ffs(idle) - 1;
    if (pdma_chan_valid(chan)) {
        rt_uint32_t int_mask = pdma_chan_int_mask(chan);

        rt_hw_interrupt_mask(pdma_dev.chan[chan].irq);
        pdma_dev.reg->pdma_ch_en |= (1U << chan);
        pdma_dev.reg->dma_int_mask &= ~int_mask;
        pdma_dev.reg->dma_int_stat = int_mask;
        pdma_dev.chan[chan].name = chan_name;
        pdma_dev.chan[chan].callback = RT_NULL;
        pdma_dev.chan[chan].param = RT_NULL;
        pdma_dev.chan[chan].configured = RT_FALSE;
        pdma_dev.chan[chan].running = RT_FALSE;
        pdma_dev.chan[chan].generation = 0;
        pdma_completion_reset(chan);
        pdma_dev.chan[chan].waiter_active = RT_FALSE;
        pdma_dev.chan[chan].waiter_generation = 0;
        pdma_dev.chan[chan].waiter_status = 0;
        pdma_dev.chan[chan].callback_count = 0;
        chan_name = RT_NULL;
    } else {
        chan = -RT_EBUSY;
    }
    pdma_spin_unlock(level);

    if (chan >= 0) {
        rt_event_recv(&pdma_dev.event, pdma_chan_int_mask(chan),
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, RT_NULL);
    }
    pdma_lifecycle_unlock();

    if (chan < 0) {
        rt_free(chan_name);
        LOG_D("No idle pdma channel\n");
    }

    return chan;
}

int rt_dma_chan_release(int chan)
{
    rt_base_t level;
    rt_uint32_t int_mask;
    rt_bool_t wait_callbacks;
    char* name;
    int ret;

    if (!pdma_chan_valid(chan))
        return -RT_EINVAL;
    if (rt_interrupt_get_nest() != 0)
        return -RT_EBUSY;

    ret = pdma_lifecycle_lock(RT_WAITING_FOREVER);
    if (ret != 0)
        return ret;

    level = pdma_spin_lock();
    if (!pdma_chan_active(chan)) {
        pdma_spin_unlock(level);
        pdma_lifecycle_unlock();
        return -RT_EINVAL;
    }
    if (pdma_dev.chan[chan].waiter_active) {
        pdma_spin_unlock(level);
        pdma_lifecycle_unlock();
        return -RT_EBUSY;
    }

    int_mask = pdma_chan_int_mask(chan);
    rt_hw_interrupt_mask(pdma_dev.chan[chan].irq);
    pdma_dev.reg->pdma_ch_reg[chan].ch_ctl = 0x2;
    pdma_dev.reg->pdma_ch_en &= ~(1U << chan);
    pdma_dev.reg->dma_int_mask |= int_mask;
    pdma_dev.reg->dma_int_stat = int_mask;
    name = pdma_dev.chan[chan].name;
    pdma_dev.chan[chan].name = RT_NULL;
    pdma_dev.chan[chan].callback = RT_NULL;
    pdma_dev.chan[chan].param = RT_NULL;
    pdma_dev.chan[chan].configured = RT_FALSE;
    pdma_dev.chan[chan].running = RT_FALSE;
    pdma_completion_reset(chan);
    pdma_dev.chan[chan].waiter_status = 0;
    wait_callbacks = pdma_dev.chan[chan].callback_count != 0;
    if (wait_callbacks)
        rt_completion_init(&pdma_dev.chan[chan].callbacks_done);
    pdma_spin_unlock(level);

    rt_event_recv(&pdma_dev.event, int_mask,
        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, RT_NULL);
    if (wait_callbacks)
        rt_completion_wait(&pdma_dev.chan[chan].callbacks_done,
            RT_WAITING_FOREVER);
    pdma_lifecycle_unlock();
    rt_free(name);

    return 0;
}

int rt_dma_chan_start(int chan)
{
    rt_base_t level;

    if (!pdma_chan_valid(chan))
        return -RT_EINVAL;

    level = pdma_spin_lock();
    if (!pdma_chan_active(chan) || !pdma_dev.chan[chan].configured ||
        pdma_dev.chan[chan].running) {
        pdma_spin_unlock(level);
        return -RT_EINVAL;
    }
    if (pdma_dev.chan[chan].completion_count >=
        PDMA_COMPLETION_QUEUE_DEPTH) {
        pdma_spin_unlock(level);
        return -RT_EBUSY;
    }

    pdma_dev.reg->pdma_ch_reg[chan].ch_ctl = 0x1;
    pdma_dev.chan[chan].generation++;
    if (pdma_dev.chan[chan].generation == 0)
        pdma_dev.chan[chan].generation++;
    pdma_dev.chan[chan].running = RT_TRUE;
    rt_hw_interrupt_umask(pdma_dev.chan[chan].irq);
    pdma_spin_unlock(level);

    return 0;
}

int rt_dma_chan_stop(int chan)
{
    rt_base_t level;

    if (!pdma_chan_valid(chan))
        return -RT_EINVAL;

    level = pdma_spin_lock();
    if (!pdma_chan_active(chan)) {
        pdma_spin_unlock(level);
        return -RT_EINVAL;
    }

    rt_hw_interrupt_mask(pdma_dev.chan[chan].irq);
    pdma_dev.reg->pdma_ch_reg[chan].ch_ctl = 0x2;
    pdma_dev.reg->dma_int_stat = pdma_chan_int_mask(chan);
    pdma_dev.chan[chan].running = RT_FALSE;
    pdma_completion_reset(chan);
    pdma_spin_unlock(level);

    return 0;
}

int rt_dma_chan_config(int chan, pdma_transfer_cfg_t* cfg)
{
    rt_base_t level;
    pdma_llt_t* list;
    rt_uint32_t list_num;

    if (!pdma_chan_valid(chan) || cfg == RT_NULL || cfg->length == 0 ||
        cfg->src_addr == RT_NULL || cfg->dst_addr == RT_NULL ||
        cfg->device < UART0_TX || cfg->device > PDM_IN)
        return -RT_EINVAL;

    level = pdma_spin_lock();
    if (!pdma_chan_active(chan) || pdma_dev.chan[chan].running) {
        pdma_spin_unlock(level);
        return -RT_EINVAL;
    }

    list_num = (cfg->length - 1) / PDMA_MAX_LINE_SIZE + 1;
    if (list_num > PDMA_LLT_MAX_ENTRIES) {
        pdma_spin_unlock(level);
        return -RT_EINVAL;
    }
    list = pdma_dev.chan[chan].list;

    for (rt_uint32_t i = 0; i < list_num; i++) {
        if (cfg->ch_cfg.ch_src_type == TX) {
            list[i].src_addr = (rt_uint64_t)cfg->src_addr +
                (rt_uint64_t)PDMA_MAX_LINE_SIZE * i;
            list[i].dst_addr = (rt_uint64_t)cfg->dst_addr;
        } else {
            list[i].src_addr = (rt_uint64_t)cfg->src_addr;
            list[i].dst_addr = (rt_uint64_t)cfg->dst_addr +
                (rt_uint64_t)PDMA_MAX_LINE_SIZE * i;
        }

        list[i].line_size = PDMA_MAX_LINE_SIZE;
        list[i].next_llt_addr = (rt_uint64_t)(list + i + 1);
        list[i].pause = 0;
        list[i].node_intr = 0;
    }
    list[list_num - 1].next_llt_addr = 0;
    list[list_num - 1].line_size = cfg->length -
        (rt_uint64_t)(list_num - 1) * PDMA_MAX_LINE_SIZE;

    rt_hw_cpu_dcache_clean((void*)list, sizeof(pdma_llt_t) * list_num);

    pdma_dev.reg->ch_peri_dev_sel[chan] = cfg->device;
    pdma_dev.reg->pdma_ch_reg[chan].ch_cfg = cfg->ch_cfg;
    pdma_dev.reg->pdma_ch_reg[chan].ch_llt_saddr = (rt_uint64_t)list;
    pdma_dev.chan[chan].configured = RT_TRUE;
    pdma_spin_unlock(level);

    return 0;
}

int rt_dma_chan_done(int chan, int timeout)
{
    rt_base_t level;
    rt_tick_t started;
    rt_int32_t remaining;
    int ret = 0;
    rt_err_t err;
    rt_uint32_t event = 0;
    rt_uint32_t generation;
    rt_uint32_t status;
    rt_bool_t terminal = RT_FALSE;
    rt_bool_t quiesce = RT_FALSE;

    if (!pdma_chan_valid(chan))
        return -RT_EINVAL;

    level = pdma_spin_lock();
    if (!pdma_chan_active(chan) ||
        (!pdma_dev.chan[chan].running &&
         pdma_dev.chan[chan].completion_count == 0) ||
        pdma_dev.chan[chan].waiter_active) {
        pdma_spin_unlock(level);
        return -RT_EINVAL;
    }

    if (!pdma_completion_peek(chan, &generation, &status)) {
        generation = pdma_dev.chan[chan].generation;
        status = 0;
    }
    pdma_dev.chan[chan].waiter_active = RT_TRUE;
    pdma_dev.chan[chan].waiter_generation = generation;
    pdma_dev.chan[chan].waiter_status = status;
    pdma_spin_unlock(level);

    started = rt_tick_get();
    remaining = timeout;
    err = RT_EOK;

    if (status & ((PDONE_INT | PTOUT_INT) << chan))
        rt_event_recv(&pdma_dev.event, pdma_chan_int_mask(chan),
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, RT_NULL);

    while (!(status & ((PDONE_INT | PTOUT_INT) << chan))) {
        event = 0;
        err = rt_event_recv(&pdma_dev.event, pdma_chan_int_mask(chan),
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, remaining, &event);
        if (err != RT_EOK)
            break;

        level = pdma_spin_lock();
        if (pdma_dev.chan[chan].waiter_active &&
            pdma_dev.chan[chan].waiter_generation == generation)
            status = pdma_dev.chan[chan].waiter_status;
        pdma_spin_unlock(level);

        if (status & ((PDONE_INT | PTOUT_INT) << chan))
            break;

        if (event & (PITEM_INT << chan)) {
            LOG_D("pdma ch%d node init", chan);
            ret = 1;
            break;
        }
        if (event & (PPAUSE_INT << chan)) {
            LOG_W("pdma ch%d pause", chan);
            ret = 2;
            break;
        }

        remaining = pdma_wait_remaining(started, timeout);
        if (timeout >= 0 && remaining == 0) {
            err = -RT_ETIMEOUT;
            break;
        }
    }

    if (!(status & ((PDONE_INT | PTOUT_INT) << chan))) {
        level = pdma_spin_lock();
        if (pdma_dev.chan[chan].waiter_active &&
            pdma_dev.chan[chan].waiter_generation == generation)
            status = pdma_dev.chan[chan].waiter_status;
        pdma_spin_unlock(level);
    }

    if (status & (PDONE_INT << chan)) {
        terminal = RT_TRUE;
    } else if (status & (PTOUT_INT << chan)) {
        LOG_E("pdma ch%d timeout", chan);
        ret = -RT_ETIMEOUT;
        terminal = RT_TRUE;
        quiesce = RT_TRUE;
    } else if (err == -RT_ETIMEOUT) {
        LOG_E("pdma ch%d transfer timeout", chan);
        ret = -RT_ETIMEOUT;
        terminal = RT_TRUE;
        quiesce = RT_TRUE;
    } else if (err != RT_EOK) {
        ret = -RT_ERROR;
        terminal = RT_TRUE;
        quiesce = RT_TRUE;
    }

    level = pdma_spin_lock();
    if (pdma_dev.chan[chan].waiter_active &&
        pdma_dev.chan[chan].waiter_generation == generation) {
        if (pdma_chan_active(chan) && quiesce &&
            pdma_dev.chan[chan].generation == generation) {
            rt_hw_interrupt_mask(pdma_dev.chan[chan].irq);
            pdma_dev.reg->pdma_ch_reg[chan].ch_ctl = 0x2;
            pdma_dev.reg->dma_int_stat = pdma_chan_int_mask(chan);
        }
        if (terminal && pdma_dev.chan[chan].generation == generation) {
            pdma_dev.chan[chan].running = RT_FALSE;
        }
        if (terminal)
            (void)pdma_completion_pop(chan, generation);
        pdma_dev.chan[chan].waiter_active = RT_FALSE;
        pdma_dev.chan[chan].waiter_status = 0;
    }
    pdma_spin_unlock(level);

    if (quiesce) {
        rt_event_recv(&pdma_dev.event, pdma_chan_int_mask(chan),
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, RT_NULL);
    }

    return ret;
}

int rt_dma_chan_callback(int chan, void (*callback)(void* param), void* param)
{
    rt_base_t level;

    if (!pdma_chan_valid(chan))
        return -RT_EINVAL;

    level = pdma_spin_lock();
    if (!pdma_chan_active(chan)) {
        pdma_spin_unlock(level);
        return -RT_EINVAL;
    }
    if (pdma_dev.chan[chan].running ||
        pdma_dev.chan[chan].callback_count != 0) {
        pdma_spin_unlock(level);
        return -RT_EBUSY;
    }

    pdma_dev.chan[chan].callback = callback;
    pdma_dev.chan[chan].param = param;
    pdma_spin_unlock(level);

    return 0;
}

static void pdma_irq(int irq, void* param)
{
    rt_base_t level;
    rt_uint32_t stat;
    rt_bool_t active = RT_FALSE;
    rt_bool_t terminal = RT_FALSE;
    rt_bool_t callbacks_done = RT_FALSE;
    void (*callback)(void* param) = RT_NULL;
    void* callback_param = RT_NULL;
    int ch = (rt_uint64_t)param;

    level = pdma_spin_lock();
    stat = pdma_dev.reg->dma_int_stat & pdma_chan_int_mask(ch);
    if (stat) {
        pdma_dev.reg->dma_int_stat = stat;
        active = pdma_chan_active(ch);
        if (active) {
            terminal = (stat & ((PDONE_INT | PTOUT_INT) << ch)) != 0U;
            if (terminal) {
                /* Completion callbacks may immediately configure the next transfer. */
                pdma_dev.chan[ch].running = RT_FALSE;
                if (!pdma_completion_push(ch,
                        pdma_dev.chan[ch].generation, stat))
                    RT_ASSERT(0);
                if (pdma_dev.chan[ch].waiter_active &&
                    pdma_dev.chan[ch].waiter_generation ==
                        pdma_dev.chan[ch].generation)
                    pdma_dev.chan[ch].waiter_status = stat;
                callback = pdma_dev.chan[ch].callback;
                callback_param = pdma_dev.chan[ch].param;
                if (callback)
                    pdma_dev.chan[ch].callback_count++;
            }
        }
    }
    pdma_spin_unlock(level);

    if (stat && active) {
        if (terminal && callback) {
            callback(callback_param);
            level = pdma_spin_lock();
            RT_ASSERT(pdma_dev.chan[ch].callback_count != 0);
            pdma_dev.chan[ch].callback_count--;
            callbacks_done = pdma_dev.chan[ch].callback_count == 0;
            pdma_spin_unlock(level);
            if (callbacks_done)
                rt_completion_done(&pdma_dev.chan[ch].callbacks_done);
        }
        rt_event_send(&pdma_dev.event, stat);
    }
}

#if defined(RT_USING_MSH) && defined(RT_PDMA_ENABLE_BUILTIN_TESTS)
static int pdma_test_expect(rt_bool_t condition, const char* name,
                            int* passed, int* failed)
{
    if (condition) {
        (*passed)++;
        rt_kprintf("[PASS] %s\n", name);
        return 0;
    }

    (*failed)++;
    rt_kprintf("[FAIL] %s\n", name);
    return -RT_ERROR;
}

static int pdma_test_uart2_tx(void)
{
    pdma_transfer_cfg_t cfg = {0};
    rt_device_t uart;
    rt_uint8_t* data;
    int chan = -1;
    int ret;

    uart = rt_device_find("uart2");
    if (uart == RT_NULL)
        return -RT_ENOSYS;

    ret = rt_device_open(uart, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK)
        return ret;

    data = rt_malloc(32);
    if (data == RT_NULL) {
        ret = -RT_ENOMEM;
        goto out_close;
    }
    for (rt_size_t i = 0; i < 32; i++)
        data[i] = (rt_uint8_t)('A' + (i % 26));
    rt_hw_cpu_dcache_clean(data, 32);

    chan = rt_dma_chan_request("pdma_uart2_tx");
    if (chan < 0) {
        ret = chan;
        goto out_free;
    }

    cfg.device = UART2_TX;
    cfg.src_addr = data;
    cfg.dst_addr = (void*)PDMA_SELFTEST_UART2_TX_ADDR;
    cfg.length = 32;
    cfg.ch_cfg.ch_src_type = TX;
    cfg.ch_cfg.ch_dev_hsize = PSBYTE1;
    cfg.ch_cfg.ch_dat_endian = PDEFAULT;
    cfg.ch_cfg.ch_dev_blen = PBURST_LEN_1;
    cfg.ch_cfg.ch_priority = 7;
    cfg.ch_cfg.ch_dev_tout = 0xfff;

    ret = rt_dma_chan_config(chan, &cfg);
    if (ret == 0)
        ret = rt_dma_chan_start(chan);
    if (ret == 0) {
        rt_thread_mdelay(10);
        ret = rt_dma_chan_done(chan,
            rt_tick_from_millisecond(PDMA_SELFTEST_TIMEOUT_MS));
    }

    if (ret != 0)
        (void)rt_dma_chan_stop(chan);
    if (rt_dma_chan_release(chan) != 0 && ret == 0)
        ret = -RT_ERROR;

out_free:
    rt_free(data);
out_close:
    if (rt_device_close(uart) != RT_EOK && ret == 0)
        ret = -RT_ERROR;

    return ret;
}

static int pdma_test_timeout_recovery(void)
{
    pdma_transfer_cfg_t cfg = {0};
    rt_device_t uart;
    rt_uint8_t* data;
    int chan = -1;
    int ret;

    uart = rt_device_find("uart2");
    if (uart == RT_NULL)
        return -RT_ENOSYS;

    ret = rt_device_open(uart, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK)
        return ret;

    data = rt_malloc(32);
    if (data == RT_NULL) {
        ret = -RT_ENOMEM;
        goto out_close;
    }

    chan = rt_dma_chan_request("pdma_timeout");
    if (chan < 0) {
        ret = chan;
        goto out_free;
    }

    cfg.device = UART2_RX;
    cfg.src_addr = (void*)UART2_BASE_ADDR;
    cfg.dst_addr = data;
    cfg.length = 32;
    cfg.ch_cfg.ch_src_type = RX;
    cfg.ch_cfg.ch_dev_hsize = PSBYTE1;
    cfg.ch_cfg.ch_dat_endian = PDEFAULT;
    cfg.ch_cfg.ch_dev_blen = PBURST_LEN_1;
    cfg.ch_cfg.ch_priority = 7;
    cfg.ch_cfg.ch_dev_tout = 0xfff;

    ret = rt_dma_chan_config(chan, &cfg);
    if (ret == 0)
        ret = rt_dma_chan_start(chan);
    if (ret == 0)
        ret = rt_dma_chan_done(chan, 0);
    if (ret != -RT_ETIMEOUT) {
        if (ret == 0)
            ret = -RT_ERROR;
        goto out_release;
    }

    for (rt_size_t i = 0; i < 32; i++)
        data[i] = (rt_uint8_t)('0' + (i % 10));
    rt_hw_cpu_dcache_clean(data, 32);

    cfg.device = UART2_TX;
    cfg.src_addr = data;
    cfg.dst_addr = (void*)PDMA_SELFTEST_UART2_TX_ADDR;
    cfg.ch_cfg.ch_src_type = TX;

    ret = rt_dma_chan_config(chan, &cfg);
    if (ret == 0)
        ret = rt_dma_chan_start(chan);
    if (ret == 0) {
        rt_thread_mdelay(10);
        ret = rt_dma_chan_done(chan,
            rt_tick_from_millisecond(PDMA_SELFTEST_TIMEOUT_MS));
    }

out_release:
    if (ret != 0)
        (void)rt_dma_chan_stop(chan);
    if (rt_dma_chan_release(chan) != 0 && ret == 0)
        ret = -RT_ERROR;
out_free:
    rt_free(data);
out_close:
    if (rt_device_close(uart) != RT_EOK && ret == 0)
        ret = -RT_ERROR;

    return ret;
}

static void pdma_test_callback(void* param)
{
    rt_sem_release((rt_sem_t)param);
}

static int pdma_test_uart2_callback_rearm(void)
{
    pdma_transfer_cfg_t cfg = {0};
    struct rt_semaphore done;
    rt_device_t uart;
    rt_uint8_t* data;
    rt_bool_t sem_initialized = RT_FALSE;
    int chan = -1;
    int ret;

    uart = rt_device_find("uart2");
    if (uart == RT_NULL)
        return -RT_ENOSYS;

    ret = rt_device_open(uart, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK)
        return ret;

    data = rt_malloc(32);
    if (data == RT_NULL) {
        ret = -RT_ENOMEM;
        goto out_close;
    }
    for (rt_size_t i = 0; i < 32; i++)
        data[i] = (rt_uint8_t)('a' + (i % 26));
    rt_hw_cpu_dcache_clean(data, 32);

    ret = rt_sem_init(&done, "pdmacb", 0, RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
        goto out_free;
    sem_initialized = RT_TRUE;

    chan = rt_dma_chan_request("pdma_uart2_cb");
    if (chan < 0) {
        ret = chan;
        goto out_sem;
    }

    cfg.device = UART2_TX;
    cfg.src_addr = data;
    cfg.dst_addr = (void*)PDMA_SELFTEST_UART2_TX_ADDR;
    cfg.length = 32;
    cfg.ch_cfg.ch_src_type = TX;
    cfg.ch_cfg.ch_dev_hsize = PSBYTE1;
    cfg.ch_cfg.ch_dat_endian = PDEFAULT;
    cfg.ch_cfg.ch_dev_blen = PBURST_LEN_1;
    cfg.ch_cfg.ch_priority = 7;
    cfg.ch_cfg.ch_dev_tout = 0xfff;

    ret = rt_dma_chan_callback(chan, pdma_test_callback, &done);
    for (int transfer = 0; ret == 0 && transfer < 2; transfer++) {
        ret = rt_dma_chan_config(chan, &cfg);
        if (ret == 0)
            ret = rt_dma_chan_start(chan);
        if (ret == 0 && rt_sem_take(&done,
                rt_tick_from_millisecond(PDMA_SELFTEST_TIMEOUT_MS)) != RT_EOK)
            ret = -RT_ETIMEOUT;
    }

    if (ret != 0)
        (void)rt_dma_chan_stop(chan);
    if (rt_dma_chan_release(chan) != 0 && ret == 0)
        ret = -RT_ERROR;

out_sem:
    if (sem_initialized)
        rt_sem_detach(&done);
out_free:
    rt_free(data);
out_close:
    if (rt_device_close(uart) != RT_EOK && ret == 0)
        ret = -RT_ERROR;

    return ret;
}

struct pdma_test_done_rearm_context {
    int chan;
    pdma_transfer_cfg_t* cfg;
    volatile int callbacks;
    volatile int rearm_result;
};

static void pdma_test_done_rearm_callback(void* param)
{
    struct pdma_test_done_rearm_context* context = param;

    context->callbacks++;
    if (context->callbacks != 1)
        return;

    context->rearm_result = rt_dma_chan_config(context->chan, context->cfg);
    if (context->rearm_result == 0)
        context->rearm_result = rt_dma_chan_start(context->chan);
}

static int pdma_test_done_callback_rearm(void)
{
    pdma_transfer_cfg_t cfg = {0};
    struct pdma_test_done_rearm_context context = {0};
    rt_device_t uart;
    rt_uint8_t* data;
    int chan = -1;
    int ret;

    uart = rt_device_find("uart2");
    if (uart == RT_NULL)
        return -RT_ENOSYS;

    ret = rt_device_open(uart, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK)
        return ret;

    data = rt_malloc(PDMA_SELFTEST_REARM_SIZE);
    if (data == RT_NULL) {
        ret = -RT_ENOMEM;
        goto out_close;
    }
    for (rt_size_t i = 0; i < PDMA_SELFTEST_REARM_SIZE; i++)
        data[i] = (rt_uint8_t)('A' + (i % 26));
    rt_hw_cpu_dcache_clean(data, PDMA_SELFTEST_REARM_SIZE);

    chan = rt_dma_chan_request("pdma_done_rearm");
    if (chan < 0) {
        ret = chan;
        goto out_free;
    }

    cfg.device = UART2_TX;
    cfg.src_addr = data;
    cfg.dst_addr = (void*)PDMA_SELFTEST_UART2_TX_ADDR;
    cfg.length = PDMA_SELFTEST_REARM_SIZE;
    cfg.ch_cfg.ch_src_type = TX;
    cfg.ch_cfg.ch_dev_hsize = PSBYTE1;
    cfg.ch_cfg.ch_dat_endian = PDEFAULT;
    cfg.ch_cfg.ch_dev_blen = PBURST_LEN_1;
    cfg.ch_cfg.ch_priority = 7;
    cfg.ch_cfg.ch_dev_tout = 0xfff;

    context.chan = chan;
    context.cfg = &cfg;
    context.rearm_result = -RT_ERROR;

    ret = rt_dma_chan_callback(chan, pdma_test_done_rearm_callback,
        &context);
    if (ret == 0)
        ret = rt_dma_chan_config(chan, &cfg);
    if (ret == 0)
        ret = rt_dma_chan_start(chan);
    for (int waited = 0;
         ret == 0 && context.callbacks < 2 &&
             waited < PDMA_SELFTEST_TIMEOUT_MS;
         waited++) {
        if (context.callbacks != 0 && context.rearm_result != 0) {
            ret = context.rearm_result;
            break;
        }
        rt_thread_mdelay(1);
    }
    if (ret == 0 && context.callbacks != 2)
        ret = -RT_ETIMEOUT;
    if (ret == 0)
        ret = rt_dma_chan_done(chan,
            rt_tick_from_millisecond(PDMA_SELFTEST_TIMEOUT_MS));
    if (ret == 0 && context.rearm_result != 0)
        ret = context.rearm_result;
    if (ret == 0)
        ret = rt_dma_chan_done(chan,
            rt_tick_from_millisecond(PDMA_SELFTEST_TIMEOUT_MS));
    if (ret == 0 && context.callbacks != 2)
        ret = -RT_ERROR;

    if (ret != 0)
        (void)rt_dma_chan_stop(chan);
    if (rt_dma_chan_release(chan) != 0 && ret == 0)
        ret = -RT_ERROR;

out_free:
    rt_free(data);
out_close:
    if (rt_device_close(uart) != RT_EOK && ret == 0)
        ret = -RT_ERROR;

    return ret;
}

static int pdma_uart3_loopback(void)
{
    pdma_transfer_cfg_t rx_cfg = {0};
    pdma_transfer_cfg_t tx_cfg = {0};
    rt_device_t uart;
    void* uart_reg = RT_NULL;
    rt_uint8_t* tx_data = RT_NULL;
    rt_uint8_t* rx_data = RT_NULL;
    int rx_chan = -1;
    int tx_chan = -1;
    int matched = 0;
    int ret;

    uart = rt_device_find("uart3");
    if (uart == RT_NULL) {
        ret = -RT_ENOSYS;
        goto out_result;
    }

    ret = rt_device_open(uart, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK)
        goto out_result;

    uart_reg = rt_ioremap((void*)UART3_BASE_ADDR, 0x1000);
    if (uart_reg == RT_NULL) {
        ret = -RT_ENOMEM;
        goto out_buffers;
    }

    tx_data = rt_malloc_align(PDMA_UART_LOOPBACK_SIZE,
        RT_CPU_CACHE_LINE_SZ);
    rx_data = rt_malloc_align(PDMA_UART_LOOPBACK_SIZE,
        RT_CPU_CACHE_LINE_SZ);
    if (tx_data == RT_NULL || rx_data == RT_NULL) {
        ret = -RT_ENOMEM;
        goto out_buffers;
    }

    for (rt_size_t i = 0; i < PDMA_UART_LOOPBACK_SIZE; i++)
        tx_data[i] = (rt_uint8_t)((i * 37U + 11U) & 0xffU);
    rt_memset(rx_data, 0, PDMA_UART_LOOPBACK_SIZE);
    rt_hw_cpu_dcache_clean(tx_data, PDMA_UART_LOOPBACK_SIZE);
    rt_hw_cpu_dcache_clean(rx_data, PDMA_UART_LOOPBACK_SIZE);

    rx_chan = rt_dma_chan_request("pdma_uart3_rx");
    if (rx_chan < 0) {
        ret = rx_chan;
        goto out_buffers;
    }
    tx_chan = rt_dma_chan_request("pdma_uart3_tx");
    if (tx_chan < 0) {
        ret = tx_chan;
        goto out_channels;
    }

    rx_cfg.device = UART3_RX;
    rx_cfg.src_addr = (void*)UART3_BASE_ADDR;
    rx_cfg.dst_addr = rx_data;
    rx_cfg.length = PDMA_UART_LOOPBACK_SIZE;
    rx_cfg.ch_cfg.ch_src_type = RX;
    rx_cfg.ch_cfg.ch_dev_hsize = PSBYTE1;
    rx_cfg.ch_cfg.ch_dat_endian = PDEFAULT;
    rx_cfg.ch_cfg.ch_dev_blen = PBURST_LEN_1;
    rx_cfg.ch_cfg.ch_priority = 7;
    rx_cfg.ch_cfg.ch_dev_tout = 0xfff;

    tx_cfg = rx_cfg;
    tx_cfg.device = UART3_TX;
    tx_cfg.src_addr = tx_data;
    tx_cfg.dst_addr = (void*)PDMA_SELFTEST_UART3_TX_ADDR;
    tx_cfg.ch_cfg.ch_src_type = TX;

    /* A one-byte trigger lets a finite RX DMA consume the final FIFO bytes. */
    writel(PDMA_UART_FCR_FIFO_RESET,
        (rt_uint8_t*)uart_reg + PDMA_UART_FCR_OFFSET);

    ret = rt_dma_chan_config(rx_chan, &rx_cfg);
    if (ret == 0)
        ret = rt_dma_chan_config(tx_chan, &tx_cfg);
    if (ret == 0)
        ret = rt_dma_chan_start(rx_chan);
    if (ret == 0)
        ret = rt_dma_chan_start(tx_chan);
    if (ret == 0)
        ret = rt_dma_chan_done(tx_chan,
            rt_tick_from_millisecond(PDMA_SELFTEST_TIMEOUT_MS));
    if (ret == 0)
        ret = rt_dma_chan_done(rx_chan,
            rt_tick_from_millisecond(PDMA_SELFTEST_TIMEOUT_MS));
    rt_hw_cpu_dcache_invalidate(rx_data, PDMA_UART_LOOPBACK_SIZE);
    while (matched < PDMA_UART_LOOPBACK_SIZE &&
           tx_data[matched] == rx_data[matched])
        matched++;
    if (ret == 0 && matched != PDMA_UART_LOOPBACK_SIZE)
        ret = -RT_ERROR;

out_channels:
    if (tx_chan >= 0 && rt_dma_chan_release(tx_chan) != 0 && ret == 0)
        ret = -RT_ERROR;
    if (rx_chan >= 0 && rt_dma_chan_release(rx_chan) != 0 && ret == 0)
        ret = -RT_ERROR;
out_buffers:
    if (uart_reg != RT_NULL) {
        writel(PDMA_UART_FCR_RX_TRIGGER_16,
            (rt_uint8_t*)uart_reg + PDMA_UART_FCR_OFFSET);
        rt_iounmap(uart_reg);
    }
    if (rx_data != RT_NULL)
        rt_free_align(rx_data);
    if (tx_data != RT_NULL)
        rt_free_align(tx_data);
    if (rt_device_close(uart) != RT_EOK && ret == 0)
        ret = -RT_ERROR;
out_result:
    rt_kprintf("PDMA_UART3_LOOPBACK_RESULT %s bytes=%d matched=%d ret=%d\n",
        ret == 0 ? "PASS" : "FAIL", PDMA_UART_LOOPBACK_SIZE, matched,
        ret);
    return ret;
}
MSH_CMD_EXPORT(pdma_uart3_loopback, run UART3 TX-RX PDMA loopback test);

static int pdma_selftest(void)
{
    pdma_transfer_cfg_t cfg = {0};
    rt_uint8_t byte = 0x5a;
    int channels[PDMA_CH_MAX];
    int passed = 0;
    int failed = 0;
    int count = 0;
    int chan;
    int cycle;

    pdma_test_expect(rt_dma_chan_release(-1) == -RT_EINVAL,
        "PDMA rejects negative release", &passed, &failed);
    pdma_test_expect(rt_dma_chan_start(PDMA_CH_MAX) == -RT_EINVAL,
        "PDMA rejects out-of-range start", &passed, &failed);
    pdma_test_expect(rt_dma_chan_done(-1, 0) == -RT_EINVAL,
        "PDMA rejects out-of-range completion", &passed, &failed);

    for (count = 0; count < PDMA_CH_MAX; count++) {
        channels[count] = rt_dma_chan_request("pdma_test");
        if (channels[count] < 0)
            break;
    }
    pdma_test_expect(count == PDMA_CH_MAX,
        "PDMA allocates all eight channels", &passed, &failed);
    if (count == PDMA_CH_MAX) {
        chan = rt_dma_chan_request("pdma_exhausted");
        pdma_test_expect(chan == -RT_EBUSY,
            "PDMA channel exhaustion is bounded", &passed, &failed);

        chan = channels[0];
        pdma_test_expect(rt_dma_chan_start(chan) == -RT_EINVAL,
            "PDMA rejects unconfigured start", &passed, &failed);
        pdma_test_expect(rt_dma_chan_config(chan, RT_NULL) == -RT_EINVAL,
            "PDMA rejects null configuration", &passed, &failed);
        pdma_test_expect(rt_dma_chan_config(chan, &cfg) == -RT_EINVAL,
            "PDMA rejects zero-length configuration", &passed, &failed);

        cfg.device = UART2_TX;
        cfg.src_addr = &byte;
        cfg.dst_addr = (void*)UART2_BASE_ADDR;
        cfg.length = sizeof(byte);
        cfg.ch_cfg.ch_src_type = TX;
        cfg.ch_cfg.ch_dev_hsize = PSBYTE1;
        cfg.ch_cfg.ch_dat_endian = PDEFAULT;
        cfg.ch_cfg.ch_dev_blen = PBURST_LEN_1;
        cfg.ch_cfg.ch_priority = 1;
        cfg.ch_cfg.ch_dev_tout = 0xfff;
        pdma_test_expect(rt_dma_chan_config(chan, &cfg) == 0,
            "PDMA accepts a valid descriptor", &passed, &failed);
        pdma_test_expect(rt_dma_chan_done(chan, 0) == -RT_EINVAL,
            "PDMA rejects completion before start", &passed, &failed);
        pdma_test_expect(rt_dma_chan_stop(chan) == 0,
            "PDMA cancels a prepared request", &passed, &failed);
    }

    while (count > 0) {
        count--;
        pdma_test_expect(rt_dma_chan_release(channels[count]) == 0,
            "PDMA releases allocated channel", &passed, &failed);
    }

    pdma_test_expect(pdma_test_uart2_tx() == 0,
        "PDMA UART2 transfer completes", &passed, &failed);
    pdma_test_expect(pdma_test_timeout_recovery() == 0,
        "PDMA timeout quiesces and recovers", &passed, &failed);
    pdma_test_expect(pdma_test_uart2_callback_rearm() == 0,
        "PDMA callback transfer rearms", &passed, &failed);
    pdma_test_expect(pdma_test_done_callback_rearm() == 0,
        "PDMA completion survives callback rearm", &passed, &failed);

    for (cycle = 0; cycle < 100; cycle++) {
        chan = rt_dma_chan_request("pdma_recovery");
        if (chan < 0 || rt_dma_chan_release(chan) != 0)
            break;
    }
    pdma_test_expect(cycle == 100,
        "PDMA 100 request/release recovery cycles", &passed, &failed);

    rt_kprintf("PDMA_SELFTEST_RESULT passed=%d failed=%d total=%d\n",
        passed, failed, passed + failed);
    return failed ? -RT_ERROR : RT_EOK;
}
MSH_CMD_EXPORT(pdma_selftest, run non-destructive PDMA lifecycle tests);
#endif

int rt_hw_pdma_device_init(void)
{
    rt_err_t ret;

    pdma_dev.reg = rt_ioremap((void*)PDMA_BASE_ADDR, PDMA_IO_SIZE);

    if (RT_NULL == pdma_dev.reg) {
        LOG_E("pdma module ioremap error!\n");
        return -RT_ERROR;
    }

    ret = rt_mutex_init(&pdma_dev.lifecycle_lock, "pdma",
        RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
        goto err_iounmap;

    rt_spin_lock_init(&pdma_dev.state_lock);

    ret = rt_event_init(&pdma_dev.event, "pdma_event", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
        goto err_mutex_detach;

    pdma_dev.chan[0].irq = PDMA_CH0_IRQ;
    for (int i = 1; i < 8; i++)
        pdma_dev.chan[i].irq = PDMA_CH1_IRQ - 1 + i;

    for (int i = 0; i < 8; i++) {
        rt_completion_init(&pdma_dev.chan[i].callbacks_done);
        rt_hw_interrupt_install(pdma_dev.chan[i].irq, pdma_irq, (void*)(rt_uint64_t)i, "pdma");
        rt_hw_interrupt_mask(pdma_dev.chan[i].irq);
    }

    return RT_EOK;

err_mutex_detach:
    rt_mutex_detach(&pdma_dev.lifecycle_lock);
err_iounmap:
    rt_iounmap(pdma_dev.reg);
    pdma_dev.reg = RT_NULL;
    return ret;
}
INIT_PREV_EXPORT(rt_hw_pdma_device_init);
