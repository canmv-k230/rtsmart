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

#include <rtdevice.h>
#include <rtthread.h>

#include <lwp_pid.h>

#include <lwp_user_mm.h>

#include <ioremap.h>
#include <riscv_io.h>

#include "board.h"
#include "drv_wdt.h"
#include "sysctl_clk.h"

#define DBG_TAG "wdt"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

#ifndef KD_WDT_ENABLE_PRETIMEOUT
#define KD_WDT_ENABLE_PRETIMEOUT (0)
#endif

#if KD_WDT_ENABLE_PRETIMEOUT
#include <ipc/workqueue.h>
#include <lwp_signal.h>
#endif

/* Driver constants and register definitions. */
#define IRQN_WDT1                     (92 + 16)
#define KD_WDT_FEED_THREAD_STACK_SIZE (1024 * 3)
#define KD_WDT_FEED_THREAD_PRIORITY   (25)
#define KD_WDT_FEED_THREAD_TIMESLICE  (200)
#define KD_WDT_FEED_IDLE_MS           (30)

#define KD_WDT_DEFAULT_TIMEOUT_SEC (20)

#define KD_WDT_MIN_CLOCK_DIV (1)
#define KD_WDT_MAX_CLOCK_DIV (64)

#define KD_WDT_NUM_TOPS      (16)
#define KD_WDT_FIX_TOP(_idx) (1U << (16 + (_idx)))
#define KD_WDT_RMOD_RESET    (1)
#define KD_WDT_RMOD_IRQ      (2)

#define KD_DEVICE_CTRL_WDT_START       _IOW('W', 1, int) /* start watchdog */
#define KD_DEVICE_CTRL_WDT_KEEPALIVE   _IOW('W', 2, int) /* refresh watchdog */
#define KD_DEVICE_CTRL_WDT_SET_TIMEOUT _IOW('W', 3, int) /* set timeout(in seconds) */
#define KD_DEVICE_CTRL_WDT_GET_TIMEOUT _IOW('W', 4, int) /* get timeout(in seconds) */

#if KD_WDT_ENABLE_PRETIMEOUT
#define KD_DEVICE_CTRL_WDT_GET_TIMELEFT   _IOW('W', 5, int) /* future: get the left time before reboot */
#define KD_DEVICE_CTRL_WDT_SET_PRETIMEOUT _IOW('W', 6, int) /* future: set pretimeout(in seconds) */
#endif

#if defined(__GNUC__) || defined(__clang__)
#define KD_WDT_UNUSED __attribute__((unused))
#else
#define KD_WDT_UNUSED
#endif

#define KD_WDT_REG_CR                   0x00U
#define KD_WDT_REG_TORR                 0x04U
#define KD_WDT_REG_CCVR                 0x08U
#define KD_WDT_REG_CRR                  0x0CU
#define KD_WDT_REG_STAT                 0x10U
#define KD_WDT_REG_EOI                  0x14U
#define KD_WDT_REG_COMP_PARAM_1         0xF4U
#define KD_WDT_CR_EN                    (1U << 0)
#define KD_WDT_CR_RMOD                  (1U << 1)
#define KD_WDT_TORR_TOP_MASK            0x0FU
#define KD_WDT_TORR_TOP_SHIFT           0U
#define KD_WDT_CRR_RESTART_KEY          0x76U
#define KD_WDT_COMP_PARAM_1_USE_FIX_TOP (1U << 6)

/*
 * There are sixteen fixed TOP values in the watchdog.
 * Each entry represents a timeout period of 2^(16 + idx) watchdog input clocks,
 * kd_wdt_timeouts_init() converts these cycle counts into sorted second/millisecond
 * values for later timeout selection.
 */
static const rt_uint32_t kd_wdt_fix_tops[KD_WDT_NUM_TOPS] = {
    KD_WDT_FIX_TOP(0),  KD_WDT_FIX_TOP(1),  KD_WDT_FIX_TOP(2),  KD_WDT_FIX_TOP(3),  KD_WDT_FIX_TOP(4),  KD_WDT_FIX_TOP(5),
    KD_WDT_FIX_TOP(6),  KD_WDT_FIX_TOP(7),  KD_WDT_FIX_TOP(8),  KD_WDT_FIX_TOP(9),  KD_WDT_FIX_TOP(10), KD_WDT_FIX_TOP(11),
    KD_WDT_FIX_TOP(12), KD_WDT_FIX_TOP(13), KD_WDT_FIX_TOP(14), KD_WDT_FIX_TOP(15),
};

/* Driver-owned runtime state and static configuration. */
struct kd_wdt_state {
    rt_thread_t feeder_thread;
    pid_t       owner_pid;
#if KD_WDT_ENABLE_PRETIMEOUT
    struct rt_work pretimeout_work;

    pid_t pending_pretimeout_pid;
    pid_t pretimeout_pid;
    char  reset_mode;
#endif
    rt_uint64_t timeout_sec;
    rt_bool_t   running;
    rt_bool_t   initialized;
#ifdef RT_USING_SMP
    rt_spinlock_t lock;
#else
    rt_uint32_t lock;
#endif
};

struct kd_wdt_config {
    const char* name;
    const char* feeder_name;
    rt_ubase_t  addr;
    rt_size_t   reg_size;
    int         irqno;
    int         clock_id;
};

struct kd_wdt_timeout {
    rt_uint32_t top_val;
    rt_uint32_t sec;
    rt_uint32_t msec;
};

struct kd_wdt_timeout_choice {
    rt_uint32_t div;
    rt_uint32_t top_val;
    rt_uint64_t timeout_us;
};

struct kd_wdt_inst {
    struct rt_device            device;
    void*                       reg;
    const struct kd_wdt_config* config;
    const rt_uint32_t*          tops;
    struct kd_wdt_timeout       timeouts[KD_WDT_NUM_TOPS];
    struct kd_wdt_state         state;
};

static const struct kd_wdt_config kd_wdt_config = {
    .name        = "watchdog1",
    .feeder_name = "wdt_feed1",
    .addr        = WDT1_BASE_ADDR,
    .reg_size    = WDT1_IO_SIZE,
    .irqno       = IRQN_WDT1,
    .clock_id    = SYSCTL_CLK_WDT_1_CLK,
};

static struct kd_wdt_inst kd_wdt_dev = {
    .config = &kd_wdt_config,
    .state = {
        .timeout_sec = KD_WDT_DEFAULT_TIMEOUT_SEC,
#if KD_WDT_ENABLE_PRETIMEOUT
        .reset_mode = KD_WDT_RMOD_RESET,
#endif
    },
};

static rt_err_t                     kd_wdt_feed_inst(struct kd_wdt_inst* inst);
static rt_err_t                     kd_wdt_release(struct kd_wdt_inst* inst);
static rt_err_t                     kd_wdt_timeouts_init(struct kd_wdt_inst* inst);
static rt_uint32_t                  kd_wdt_select_timeout_top(struct kd_wdt_inst* inst, rt_uint64_t timeout);
static const struct kd_wdt_timeout* kd_wdt_find_timeout(struct kd_wdt_inst* inst, rt_uint32_t top_val);
static inline rt_uint32_t           kd_wdt_timeout_factor(struct kd_wdt_inst* inst);

/* Basic device and MMIO helpers. */
static inline struct kd_wdt_inst* kd_wdt_from_device(rt_device_t dev) { return (struct kd_wdt_inst*)dev->user_data; }

static inline void* kd_wdt_reg_ptr(struct kd_wdt_inst* inst, rt_uint32_t reg) { return (rt_uint8_t*)inst->reg + reg; }

static inline rt_uint32_t kd_wdt_readl(struct kd_wdt_inst* inst, rt_uint32_t reg) { return readl(kd_wdt_reg_ptr(inst, reg)); }

static inline void kd_wdt_writel(struct kd_wdt_inst* inst, rt_uint32_t reg, rt_uint32_t value)
{
    writel(value, kd_wdt_reg_ptr(inst, reg));
}

static inline rt_uint32_t kd_wdt_get_root_clock(struct kd_wdt_inst* inst)
{
    sysctl_clk_node_e parent = sysctl_clk_get_leaf_parent(inst->config->clock_id);

    if (parent <= SYSCTL_CLK_ROOT_MAX) {
        return sysctl_boot_get_root_clk_freq(parent);
    }

    return sysctl_clk_get_leaf_freq(parent);
}

static rt_err_t kd_wdt_set_clock_div(struct kd_wdt_inst* inst, rt_uint32_t div)
{
    if (!sysctl_clk_set_leaf_div(inst->config->clock_id, 1, div)) {
        return -RT_ERROR;
    }

    return kd_wdt_timeouts_init(inst);
}

static struct kd_wdt_timeout_choice kd_wdt_choose_timeout(struct kd_wdt_inst* inst, rt_uint64_t timeout)
{
    rt_uint32_t                  div;
    rt_uint32_t                  idx;
    rt_uint32_t                  factor     = kd_wdt_timeout_factor(inst);
    rt_uint32_t                  root_hz    = kd_wdt_get_root_clock(inst);
    rt_uint64_t                  request_us = timeout * 1000000ULL;
    struct kd_wdt_timeout_choice best       = { 0 };
    struct kd_wdt_timeout_choice fallback   = { 0 };

    if (root_hz == 0) {
        return best;
    }

    for (div = KD_WDT_MIN_CLOCK_DIV; div <= KD_WDT_MAX_CLOCK_DIV; ++div) {
        for (idx = 0; idx < KD_WDT_NUM_TOPS; ++idx) {
            rt_uint64_t                  cycles     = inst->tops[idx];
            rt_uint64_t                  timeout_us = cycles * (rt_uint64_t)div * (rt_uint64_t)factor * 1000000ULL / root_hz;
            struct kd_wdt_timeout_choice current    = {
                   .div        = div,
                   .top_val    = idx,
                   .timeout_us = timeout_us,
            };

            if (timeout_us >= request_us) {
                if (best.timeout_us == 0 || timeout_us < best.timeout_us) {
                    best = current;
                }
            }

            if (fallback.timeout_us == 0 || timeout_us > fallback.timeout_us) {
                fallback = current;
            }
        }
    }

    if (best.timeout_us != 0) {
        return best;
    }

    return fallback;
}

static inline rt_uint32_t kd_wdt_get_clock(struct kd_wdt_inst* inst)
{
    return sysctl_clk_get_leaf_freq(inst->config->clock_id);
}

static inline rt_base_t kd_wdt_runtime_lock(struct kd_wdt_inst* inst)
{
#ifdef RT_USING_SMP
    return rt_spin_lock_irqsave(&inst->state.lock);
#else
    (void)inst;
    return rt_hw_interrupt_disable();
#endif
}

static inline void kd_wdt_runtime_unlock(struct kd_wdt_inst* inst, rt_base_t level)
{
#ifdef RT_USING_SMP
    rt_spin_unlock_irqrestore(&inst->state.lock, level);
#else
    (void)inst;
    rt_hw_interrupt_enable(level);
#endif
}

static inline rt_uint32_t kd_wdt_timeout_factor(struct kd_wdt_inst* inst)
{
#if KD_WDT_ENABLE_PRETIMEOUT
    return inst->state.reset_mode;
#else
    (void)inst;
    return KD_WDT_RMOD_RESET;
#endif
}

static inline rt_bool_t kd_wdt_hw_is_enabled(struct kd_wdt_inst* inst)
{
    return (kd_wdt_readl(inst, KD_WDT_REG_CR) & KD_WDT_CR_EN) != 0;
}

static inline void kd_wdt_hw_restart(struct kd_wdt_inst* inst) { kd_wdt_writel(inst, KD_WDT_REG_CRR, KD_WDT_CRR_RESTART_KEY); }

static void kd_wdt_hw_set_response_mode(struct kd_wdt_inst* inst, rt_bool_t irq_mode)
{
    rt_uint32_t ctrl = kd_wdt_readl(inst, KD_WDT_REG_CR);

    if (irq_mode) {
        ctrl |= KD_WDT_CR_RMOD;
    } else {
        ctrl &= ~KD_WDT_CR_RMOD;
    }

    kd_wdt_writel(inst, KD_WDT_REG_CR, ctrl);
}

static void kd_wdt_hw_enable(struct kd_wdt_inst* inst)
{
    rt_uint32_t ctrl = kd_wdt_readl(inst, KD_WDT_REG_CR);

    kd_wdt_hw_restart(inst);
    ctrl |= KD_WDT_CR_EN;
    kd_wdt_writel(inst, KD_WDT_REG_CR, ctrl);
}

static void kd_wdt_hw_set_timeout_top(struct kd_wdt_inst* inst, rt_uint32_t top_val)
{
    rt_uint32_t torr = (top_val & KD_WDT_TORR_TOP_MASK) << KD_WDT_TORR_TOP_SHIFT;

    kd_wdt_writel(inst, KD_WDT_REG_TORR, torr);
}

static inline rt_uint32_t kd_wdt_hw_get_timeout_top(struct kd_wdt_inst* inst)
{
    return (kd_wdt_readl(inst, KD_WDT_REG_TORR) >> KD_WDT_TORR_TOP_SHIFT) & KD_WDT_TORR_TOP_MASK;
}

#if KD_WDT_ENABLE_PRETIMEOUT
static inline rt_uint32_t kd_wdt_hw_get_counter(struct kd_wdt_inst* inst) { return kd_wdt_readl(inst, KD_WDT_REG_CCVR); }

static inline rt_bool_t kd_wdt_hw_irq_pending(struct kd_wdt_inst* inst)
{
    return (kd_wdt_readl(inst, KD_WDT_REG_STAT) & 0x1U) != 0;
}

static inline void kd_wdt_hw_clear_irq(struct kd_wdt_inst* inst) { (void)kd_wdt_readl(inst, KD_WDT_REG_EOI); }
#endif

static inline rt_bool_t kd_wdt_hw_uses_fix_top(struct kd_wdt_inst* inst)
{
    return (kd_wdt_readl(inst, KD_WDT_REG_COMP_PARAM_1) & KD_WDT_COMP_PARAM_1_USE_FIX_TOP) != 0;
}

/* Ownership and timeout bookkeeping helpers. */
static void kd_wdt_runtime_reset_locked(struct kd_wdt_inst* inst)
{
    inst->state.owner_pid = 0;
#if KD_WDT_ENABLE_PRETIMEOUT
    inst->state.pretimeout_pid = 0;
    inst->state.reset_mode     = KD_WDT_RMOD_RESET;
#endif
}

static void kd_wdt_apply_timeout_locked(struct kd_wdt_inst* inst, rt_uint32_t top_val)
{
    kd_wdt_hw_set_timeout_top(inst, top_val);

    /* TRM: a new TOP only takes effect after the next restart. */
    if (inst->state.running) {
        kd_wdt_hw_restart(inst);
    }
}

static void kd_wdt_switch_to_kernel_locked(struct kd_wdt_inst* inst)
{
    struct kd_wdt_timeout_choice choice;
    rt_uint32_t                  top_val;
    const struct kd_wdt_timeout* wdt_timeout;

    kd_wdt_runtime_reset_locked(inst);
    kd_wdt_hw_set_response_mode(inst, RT_FALSE);

    choice = kd_wdt_choose_timeout(inst, KD_WDT_DEFAULT_TIMEOUT_SEC);
    if (choice.div != 0 && kd_wdt_set_clock_div(inst, choice.div) == RT_EOK) {
        top_val = choice.top_val;
    } else {
        top_val = kd_wdt_select_timeout_top(inst, KD_WDT_DEFAULT_TIMEOUT_SEC);
    }

    kd_wdt_apply_timeout_locked(inst, top_val);

    wdt_timeout = kd_wdt_find_timeout(inst, top_val);
    if (wdt_timeout != RT_NULL) {
        inst->state.timeout_sec = wdt_timeout->sec * kd_wdt_timeout_factor(inst);
    } else {
        inst->state.timeout_sec = KD_WDT_DEFAULT_TIMEOUT_SEC;
    }
}

static void kd_wdt_claim_user_locked(struct kd_wdt_inst* inst, pid_t pid)
{
    inst->state.owner_pid = pid;
#if KD_WDT_ENABLE_PRETIMEOUT
    inst->state.pretimeout_pid = 0;
    inst->state.reset_mode     = KD_WDT_RMOD_RESET;
#endif
    kd_wdt_hw_set_response_mode(inst, RT_FALSE);

    if (inst->state.running) {
        kd_wdt_hw_restart(inst);
    }
}

static rt_err_t kd_wdt_claim_user(struct kd_wdt_inst* inst)
{
    pid_t     pid = lwp_getpid();
    rt_base_t level;

    if (pid == 0) {
        return RT_EOK;
    }

    level = kd_wdt_runtime_lock(inst);
    if (inst->state.owner_pid == 0) {
        kd_wdt_claim_user_locked(inst, pid);
    } else if (inst->state.owner_pid != pid) {
        kd_wdt_runtime_unlock(inst, level);
        return -RT_EBUSY;
    }
    kd_wdt_runtime_unlock(inst, level);

    return RT_EOK;
}

static rt_err_t kd_wdt_timeouts_init(struct kd_wdt_inst* inst)
{
    rt_uint32_t           wdt_clk;
    rt_uint32_t           i, t;
    rt_uint64_t           msec;
    struct kd_wdt_timeout tout, *dst, tmp;

    if (inst->tops == RT_NULL) {
        return -RT_EINVAL;
    }

    wdt_clk = kd_wdt_get_clock(inst);
    if (wdt_clk == 0) {
        return -RT_ERROR;
    }

    for (i = 0; i < KD_WDT_NUM_TOPS; ++i) {
        tout.top_val = i;
        tout.sec     = inst->tops[i] / wdt_clk;
        msec         = (rt_uint64_t)inst->tops[i] * (rt_uint64_t)1000L;
        msec         = msec / wdt_clk;
        tout.msec    = msec - (rt_uint64_t)tout.sec * (rt_uint64_t)1000L;

        for (t = 0; t < i; ++t) {
            dst = &inst->timeouts[t];
            if (tout.sec > dst->sec || (tout.sec == dst->sec && tout.msec >= dst->msec)) {
                continue;
            }

            tmp  = *dst;
            *dst = tout;
            tout = tmp;
        }

        inst->timeouts[i] = tout;

        LOG_D("WDT TOP%d: %u cycles, %u.%03u seconds\n", i, inst->tops[i], tout.sec, tout.msec);
    }

    return RT_EOK;
}

static rt_uint32_t kd_wdt_select_timeout_top(struct kd_wdt_inst* inst, rt_uint64_t timeout)
{
    rt_uint32_t i;
    rt_uint32_t factor = kd_wdt_timeout_factor(inst);
    rt_uint32_t time   = (timeout + factor - 1) / factor;

    for (i = 0; i < KD_WDT_NUM_TOPS; ++i) {
        if (inst->timeouts[i].sec >= time) {
            return inst->timeouts[i].top_val;
        }
    }

    return inst->timeouts[KD_WDT_NUM_TOPS - 1].top_val;
}

static const struct kd_wdt_timeout* kd_wdt_find_timeout(struct kd_wdt_inst* inst, rt_uint32_t top_val)
{
    rt_uint32_t i;

    for (i = 0; i < KD_WDT_NUM_TOPS; ++i) {
        if (inst->timeouts[i].top_val == top_val) {
            return &inst->timeouts[i];
        }
    }

    return RT_NULL;
}

/* Kernel feeder thread management. */
#if KD_WDT_ENABLE_PRETIMEOUT
static void kd_wdt_pretimeout_work(struct rt_work* work, void* work_data)
{
    struct kd_wdt_inst* inst = work_data;
    pid_t               pid;
    rt_base_t           level;

    (void)work;

    level                              = kd_wdt_runtime_lock(inst);
    pid                                = inst->state.pending_pretimeout_pid;
    inst->state.pending_pretimeout_pid = 0;
    kd_wdt_runtime_unlock(inst, level);

    if (pid > 0) {
        lwp_kill(pid, SIGUSR2);
    }
}
#endif

static void kd_wdt_feeder_entry(void* parameter)
{
    struct kd_wdt_inst*  inst  = parameter;
    struct kd_wdt_state* state = &inst->state;

    while (1) {
        rt_uint64_t delay_ms    = KD_WDT_FEED_IDLE_MS;
        rt_bool_t   should_feed = RT_FALSE;
        rt_base_t   level;

        level = kd_wdt_runtime_lock(inst);
        if (state->running && state->owner_pid == 0) {
            should_feed = RT_TRUE;
            delay_ms    = (state->timeout_sec * 1000ULL) / 2;
            if (delay_ms < KD_WDT_FEED_IDLE_MS) {
                delay_ms = KD_WDT_FEED_IDLE_MS;
            }
        }
        kd_wdt_runtime_unlock(inst, level);

        if (should_feed) {
            kd_wdt_feed_inst(inst);

            LOG_D("Watchdog fed by kernel thread, timeout is %u seconds\n", state->timeout_sec);
        }

        rt_thread_mdelay((rt_int32_t)delay_ms);
    }
}

static rt_err_t kd_wdt_runtime_init(struct kd_wdt_inst* inst)
{
    if (inst->state.initialized) {
        return RT_EOK;
    }

#ifdef RT_USING_SMP
    rt_spin_lock_init(&inst->state.lock);
#endif

#if KD_WDT_ENABLE_PRETIMEOUT
    rt_work_init(&inst->state.pretimeout_work, kd_wdt_pretimeout_work, inst);
#endif

    inst->state.feeder_thread
        = rt_thread_create(inst->config->feeder_name, kd_wdt_feeder_entry, inst, KD_WDT_FEED_THREAD_STACK_SIZE,
                           KD_WDT_FEED_THREAD_PRIORITY, KD_WDT_FEED_THREAD_TIMESLICE);
    if (inst->state.feeder_thread == RT_NULL) {
        return -RT_ENOMEM;
    }

    rt_thread_startup(inst->state.feeder_thread);
    inst->state.initialized = RT_TRUE;

    return RT_EOK;
}

/* Active watchdog operations. */
static rt_err_t kd_wdt_feed_inst(struct kd_wdt_inst* inst)
{
    kd_wdt_hw_restart(inst);

    return RT_EOK;
}

rt_err_t kd_wdt_feed(void)
{
    struct kd_wdt_inst* inst = &kd_wdt_dev;
    rt_base_t           level;

    if (!inst->state.initialized || inst->reg == RT_NULL) {
        return -RT_ERROR;
    }

    level = kd_wdt_runtime_lock(inst);
    kd_wdt_feed_inst(inst);
    kd_wdt_runtime_unlock(inst, level);

    return RT_EOK;
}

static rt_err_t kd_wdt_enable(struct kd_wdt_inst* inst)
{
    rt_base_t level;

    level = kd_wdt_runtime_lock(inst);
    kd_wdt_hw_enable(inst);
    inst->state.running = RT_TRUE;
    kd_wdt_runtime_unlock(inst, level);

    return RT_EOK;
}

static rt_err_t kd_wdt_set_timeout(struct kd_wdt_inst* inst, rt_uint64_t timeout)
{
    const struct kd_wdt_timeout* wdt_timeout;
    struct kd_wdt_timeout_choice choice;
    rt_base_t                    level;
    rt_err_t                     ret;

    level  = kd_wdt_runtime_lock(inst);
    choice = kd_wdt_choose_timeout(inst, timeout);
    if (choice.div == 0) {
        kd_wdt_runtime_unlock(inst, level);
        return -RT_ERROR;
    }

    ret = kd_wdt_set_clock_div(inst, choice.div);
    if (ret != RT_EOK) {
        kd_wdt_runtime_unlock(inst, level);
        return ret;
    }

    kd_wdt_apply_timeout_locked(inst, choice.top_val);
    wdt_timeout = kd_wdt_find_timeout(inst, choice.top_val);
    if (wdt_timeout != RT_NULL) {
        inst->state.timeout_sec = wdt_timeout->sec * kd_wdt_timeout_factor(inst);
    } else {
        inst->state.timeout_sec = timeout;
    }
    kd_wdt_runtime_unlock(inst, level);

    return RT_EOK;
}

static rt_err_t kd_wdt_get_timeout(struct kd_wdt_inst* inst, void* timeout)
{
    const struct kd_wdt_timeout* wdt_timeout;
    rt_uint32_t                  top_val = kd_wdt_hw_get_timeout_top(inst);
    rt_uint32_t                  factor;
    rt_base_t                    level;

    level       = kd_wdt_runtime_lock(inst);
    factor      = kd_wdt_timeout_factor(inst);
    wdt_timeout = kd_wdt_find_timeout(inst, top_val);
    kd_wdt_runtime_unlock(inst, level);

    if (wdt_timeout == RT_NULL) {
        return -RT_EINVAL;
    }

    *((rt_uint64_t*)timeout) = wdt_timeout->sec * factor;

    return RT_EOK;
}

/* Future capability hooks kept out of the active ioctl surface for now. */
#if KD_WDT_ENABLE_PRETIMEOUT
static rt_err_t KD_WDT_UNUSED kd_wdt_set_pretimeout(struct kd_wdt_inst* inst, rt_uint64_t timeout)
{
    pid_t       pid = lwp_getpid();
    rt_uint64_t current_timeout;
    rt_base_t   level;

    level = kd_wdt_runtime_lock(inst);
    if (timeout) {
        if (pid == 0 && inst->state.owner_pid == 0) {
            kd_wdt_runtime_unlock(inst, level);
            return -RT_EINVAL;
        }

        inst->state.pretimeout_pid = inst->state.owner_pid ? inst->state.owner_pid : pid;
        inst->state.reset_mode     = KD_WDT_RMOD_IRQ;
        kd_wdt_hw_set_response_mode(inst, RT_TRUE);
    } else {
        inst->state.pretimeout_pid = 0;
        inst->state.reset_mode     = KD_WDT_RMOD_RESET;
        kd_wdt_hw_set_response_mode(inst, RT_FALSE);
    }
    current_timeout = inst->state.timeout_sec;
    kd_wdt_runtime_unlock(inst, level);

    return kd_wdt_set_timeout(inst, current_timeout);
}

static rt_err_t KD_WDT_UNUSED kd_wdt_get_timeleft(struct kd_wdt_inst* inst, void* timeout)
{
    rt_uint32_t wdt_clk;
    rt_uint64_t counter_sec;
    rt_uint64_t current_period_sec;
    rt_bool_t   irq_pending;
    char        reset_mode;
    rt_base_t   level;

    wdt_clk = kd_wdt_get_clock(inst);
    if (wdt_clk == 0) {
        return -RT_ERROR;
    }

    counter_sec = ((rt_uint64_t)kd_wdt_hw_get_counter(inst) + wdt_clk - 1) / wdt_clk;

    level              = kd_wdt_runtime_lock(inst);
    reset_mode         = inst->state.reset_mode;
    current_period_sec = inst->state.timeout_sec / reset_mode;
    irq_pending        = kd_wdt_hw_irq_pending(inst);
    kd_wdt_runtime_unlock(inst, level);

    if (!irq_pending && reset_mode == KD_WDT_RMOD_IRQ) {
        counter_sec += current_period_sec;
    }

    *((rt_uint64_t*)timeout) = counter_sec;

    return RT_EOK;
}
#endif

/* Device entry points. */
static rt_err_t kd_wdt_init(struct kd_wdt_inst* inst)
{
    rt_err_t  ret;
    rt_base_t level;

    ret = kd_wdt_runtime_init(inst);
    if (ret != RT_EOK) {
        return ret;
    }

    kd_wdt_hw_set_response_mode(inst, RT_FALSE);

    inst->tops = kd_wdt_hw_uses_fix_top(inst) ? kd_wdt_fix_tops : RT_NULL;
    ret        = kd_wdt_timeouts_init(inst);
    if (ret != RT_EOK) {
        LOG_E("watchdog timeout table init failed %d", ret);
        return ret;
    }

    if (!inst->timeouts[KD_WDT_NUM_TOPS - 1].sec) {
        LOG_E("No any valid Timeout period detected");
        return -RT_EINVAL;
    }

    level = kd_wdt_runtime_lock(inst);

    inst->state.running = RT_FALSE;
    kd_wdt_switch_to_kernel_locked(inst);
    kd_wdt_hw_enable(inst);
    inst->state.running = RT_TRUE;

    kd_wdt_runtime_unlock(inst, level);

    return RT_EOK;
}

static rt_err_t kd_wdt_release(struct kd_wdt_inst* inst)
{
    pid_t     pid = lwp_getpid();
    rt_base_t level;

    level = kd_wdt_runtime_lock(inst);
    if (pid == 0 || inst->state.owner_pid == pid) {
        kd_wdt_switch_to_kernel_locked(inst);
    }
    kd_wdt_runtime_unlock(inst, level);

    return RT_EOK;
}

static rt_err_t kd_wdt_dev_close(rt_device_t dev) { return kd_wdt_release(kd_wdt_from_device(dev)); }

static rt_err_t kd_wdt_dev_control(rt_device_t dev, int cmd, void* args)
{
    struct kd_wdt_inst* inst = kd_wdt_from_device(dev);
    rt_uint32_t         timeout_in;
    rt_uint64_t         timeout_out;
    rt_err_t            ret;

    RT_ASSERT(dev != NULL);

    switch (cmd) {
    case KD_DEVICE_CTRL_WDT_START:
        if (kd_wdt_claim_user(inst) != RT_EOK) {
            return -RT_EBUSY;
        }
        return kd_wdt_enable(inst);
    case KD_DEVICE_CTRL_WDT_KEEPALIVE:
        if (kd_wdt_claim_user(inst) != RT_EOK) {
            return -RT_EBUSY;
        }
        return kd_wdt_feed_inst(inst);
    case KD_DEVICE_CTRL_WDT_SET_TIMEOUT:
        if (args == RT_NULL) {
            return -RT_EINVAL;
        }
        if (lwp_get_from_user_ex(&timeout_in, args, sizeof(timeout_in)) != 0) {
            return -EFAULT;
        }
        if (kd_wdt_claim_user(inst) != RT_EOK) {
            return -RT_EBUSY;
        }
        return kd_wdt_set_timeout(inst, timeout_in);
    case KD_DEVICE_CTRL_WDT_GET_TIMEOUT:
        ret = kd_wdt_get_timeout(inst, &timeout_out);
        if (ret != RT_EOK) {
            return ret;
        }
        if (args == RT_NULL) {
            return -RT_EINVAL;
        }
        if (lwp_put_to_user_ex(args, &timeout_out, sizeof(timeout_out)) != 0) {
            return -EFAULT;
        }
        return RT_EOK;
#if KD_WDT_ENABLE_PRETIMEOUT
    case KD_DEVICE_CTRL_WDT_SET_PRETIMEOUT:
        if (args == RT_NULL) {
            return -RT_EINVAL;
        }
        if (lwp_get_from_user_ex(&timeout_in, args, sizeof(timeout_in)) != 0) {
            return -EFAULT;
        }
        if (kd_wdt_claim_user(inst) != RT_EOK) {
            return -RT_EBUSY;
        }
        return kd_wdt_set_pretimeout(inst, timeout_in);
    case KD_DEVICE_CTRL_WDT_GET_TIMELEFT:
        ret = kd_wdt_get_timeleft(inst, &timeout_out);
        if (ret != RT_EOK) {
            return ret;
        }
        if (args == RT_NULL) {
            return -RT_EINVAL;
        }
        if (lwp_put_to_user_ex(args, &timeout_out, sizeof(timeout_out)) != 0) {
            return -EFAULT;
        }
        return RT_EOK;
#endif
    default:
        return -RT_EINVAL;
    }
}

#if KD_WDT_ENABLE_PRETIMEOUT
static void kd_wdt_irq_handler(int irq, void* param)
{
    rt_base_t level;

    pid_t pretimeout_pid = 0;

    struct kd_wdt_inst* inst = param;

    (void)irq;

    level = kd_wdt_runtime_lock(inst);
    if (!kd_wdt_hw_irq_pending(inst)) {
        kd_wdt_runtime_unlock(inst, level);
        return;
    }

    pretimeout_pid = inst->state.pretimeout_pid;
    kd_wdt_runtime_reset_locked(inst);
    kd_wdt_hw_set_response_mode(inst, RT_FALSE);
    kd_wdt_hw_clear_irq(inst);
    kd_wdt_runtime_unlock(inst, level);

    rt_kprintf("watchdog pretimeout\n");

    if (pretimeout_pid) {
        level = kd_wdt_runtime_lock(inst);

        inst->state.pending_pretimeout_pid = pretimeout_pid;
        kd_wdt_runtime_unlock(inst, level);

        (void)rt_work_submit(&inst->state.pretimeout_work, 0);
    }
}
#endif

/* Device registration and startup. */
#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops kd_wdt_ops = {
    .init    = RT_NULL,
    .open    = RT_NULL,
    .close   = kd_wdt_dev_close,
    .read    = RT_NULL,
    .write   = RT_NULL,
    .control = kd_wdt_dev_control,
};
#endif

int rt_hw_wdt_init(void)
{
    rt_err_t            ret             = RT_EOK;
    static int          wdt_inited_flag = 0;
    struct kd_wdt_inst* inst            = &kd_wdt_dev;

    if (wdt_inited_flag) {
        return 0;
    }

    inst->reg = rt_ioremap((void*)inst->config->addr, inst->config->reg_size);
    if (inst->reg == RT_NULL) {
        LOG_E("ioremap failed for %s", inst->config->name);
        return -RT_ENOMEM;
    }

    inst->device.type        = RT_Device_Class_WDT;
    inst->device.rx_indicate = RT_NULL;
    inst->device.tx_complete = RT_NULL;
    inst->device.user_data   = inst;

#ifdef RT_USING_DEVICE_OPS
    inst->device.ops = &kd_wdt_ops;
#else
    inst->device.init    = RT_NULL;
    inst->device.open    = RT_NULL;
    inst->device.close   = kd_wdt_dev_close;
    inst->device.read    = RT_NULL;
    inst->device.write   = RT_NULL;
    inst->device.control = kd_wdt_dev_control;
#endif

    ret = rt_device_register(&inst->device, inst->config->name, RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_STANDALONE);
    if (ret != RT_EOK) {
        LOG_E("register %s failed %d", inst->config->name, ret);
        return ret;
    }

#if KD_WDT_ENABLE_PRETIMEOUT
    rt_hw_interrupt_install(inst->config->irqno, kd_wdt_irq_handler, inst, inst->config->name);
    rt_hw_interrupt_umask(inst->config->irqno);
#endif

    ret = kd_wdt_init(inst);
    if (ret != RT_EOK) {
        LOG_E("init %s failed %d", inst->config->name, ret);
        return ret;
    }

    inst->device.flag |= RT_DEVICE_FLAG_ACTIVATED;

    wdt_inited_flag = 1;

    return ret;
}
INIT_DEVICE_EXPORT(rt_hw_wdt_init);
