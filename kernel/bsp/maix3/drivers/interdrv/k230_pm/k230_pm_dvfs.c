/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdint.h>
#include "k230_pm.h"
#include "k230_pm_dvfs.h"
#include "k230_pm_pid.h"
#include "usage.h"

struct regulator_dev;
int regulator_set_voltage(struct regulator_dev *pdev, int vol_uv);

#define PROFILE_IS_UP(from, to) ((from) > (to))
#define K230_DVFS_MAX_PROFILES K230_PM_MAX_PROFILE_COUNT
#define K230_DVFS_THERMAL_DEFAULT_TEMP 85
#define K230_DVFS_SHUTDOWN_DEFAULT_TEMP 100
#define K230_DVFS_THERMAL_HYSTERESIS_TEMP 5
#define K230_DVFS_THERMAL_MIN_TEMP (-40)
#define K230_DVFS_THERMAL_MAX_TEMP 125
#define K230_DVFS_AUTO_THREAD_PRIORITY (RT_THREAD_PRIORITY_MAX - 8)
#define K230_DVFS_USAGE_THREAD_PRIORITY (K230_DVFS_AUTO_THREAD_PRIORITY - 1)
#define K230_DVFS_USAGE_DEFAULT_PRIORITY (RT_THREAD_PRIORITY_MAX - 2)
#define K230_DVFS_USAGE_DEFAULT_PERIOD 1000
#define K230_DVFS_FREQ_TOLERANCE_HZ 1000U

#define AUTO_GOVERNOR_POLL_TIME 100
#define AUTO_GOVERNOR_MIN_CHANGE_TIME 300
#define AUTO_GOVERNOR_TARGET_LOAD 70
#define AUTO_GOVERNOR_LOAD_DEADBAND 8
#define AUTO_GOVERNOR_UP_HOLD_COUNT 2
#define AUTO_GOVERNOR_DOWN_HOLD_COUNT 5
#define AUTO_PID_UP_THRESHOLD 20
#define AUTO_PID_DOWN_THRESHOLD (-20)
#define CPU_USAGE_INVALID 101

struct k230_dvfs_domain {
    const struct k230_dvfs_domain_desc *desc;
    struct rt_mutex mutex;
    uint32_t profile_lock[K230_DVFS_MAX_PROFILES];
    uint32_t clock_vote_count;
    uint32_t power_vote_count;
    int32_t manual_profile_idx;
    int32_t expect_profile_idx;
    int32_t actual_profile_idx;
    bool profile_valid;
    k230_pm_uapi_governor governor;
    int32_t thermal_protect_temp;
    int32_t thermal_protect_idx;
    bool thermal_over;
    struct regulator_dev *regulator;
};

static struct k230_dvfs_domain k230_dvfs_domains[K230_PM_DOMAIN_NR];
static int32_t k230_dvfs_thermal_shutdown = K230_DVFS_SHUTDOWN_DEFAULT_TEMP;
static bool k230_dvfs_domains_initialized;
static bool k230_dvfs_ready;

static bool auto_load_enable;
static bool auto_thread_started;
static uint32_t auto_up_hold_count;
static uint32_t auto_down_hold_count;
static struct k230_pm_pid_state auto_pid_state;
static rt_tick_t auto_last_change_tick;

static const struct k230_pm_pid_config auto_pid_config = {
    .target = AUTO_GOVERNOR_TARGET_LOAD,
    .deadband = AUTO_GOVERNOR_LOAD_DEADBAND,
    .kp = 4,
    .ki = 1,
    .kd = 2,
    .scale = 4,
    .integral_limit = 160,
};

static struct k230_dvfs_domain *k230_dvfs_domain_get(k230_pm_uapi_domain domain)
{
    if (domain < 0 || domain >= K230_PM_DOMAIN_NR)
        return RT_NULL;
    if (domain >= k230_dvfs_domain_desc_count)
        return RT_NULL;
    if (k230_dvfs_domains[domain].desc == RT_NULL)
        return RT_NULL;

    return &k230_dvfs_domains[domain];
}

static int k230_dvfs_domain_check(k230_pm_uapi_domain domain)
{
    if (domain < 0 || domain >= K230_PM_DOMAIN_NR)
        return K230_ERR_PM_ILLEGAL_PARAM;
    if (k230_dvfs_domain_get(domain) == RT_NULL)
        return K230_ERR_PM_UNEXIST;

    return 0;
}

static int32_t k230_dvfs_profile_index_transform(struct k230_dvfs_domain *domain,
    int32_t index)
{
    if (index < 0)
        index = domain->desc->step_count + index;

    if (index < 0)
        index = 0;
    else if (index >= domain->desc->step_count)
        index = domain->desc->step_count - 1;

    return index;
}

static bool k230_dvfs_profile_index_valid(struct k230_dvfs_domain *domain,
    int32_t index)
{
    int32_t count = (int32_t)domain->desc->step_count;

    return index >= -count && index < count;
}

static bool k230_dvfs_thermal_temp_valid(int32_t temp)
{
    return temp >= K230_DVFS_THERMAL_MIN_TEMP &&
        temp <= K230_DVFS_THERMAL_MAX_TEMP;
}

static bool k230_dvfs_freq_match(uint32_t actual, uint32_t expected)
{
    uint32_t delta;

    delta = actual > expected ? actual - expected : expected - actual;
    return delta <= K230_DVFS_FREQ_TOLERANCE_HZ;
}

static bool k230_dvfs_read_hw_profile(struct k230_dvfs_domain *domain,
    int32_t *index)
{
    uint32_t actual_freq;
    uint32_t i;

    if (domain->desc->steps[0].leaf == SYSCTL_CLK_NODE_MAX) {
        *index = 0;
        return true;
    }

    actual_freq = sysctl_media_clk_get_leaf_freq(domain->desc->steps[0].leaf);
    for (i = 0; i < domain->desc->step_count; i++) {
        if (k230_dvfs_freq_match(actual_freq,
            (uint32_t)domain->desc->steps[i].freq)) {
            *index = (int32_t)i;
            return true;
        }
    }

    return false;
}

static int k230_dvfs_set_clock_step(struct k230_dvfs_domain *domain, int32_t index)
{
    const struct k230_dvfs_step *step;
    uint32_t actual_freq;
    int ret;

    index = k230_dvfs_profile_index_transform(domain, index);
    step = &domain->desc->steps[index];
    if (step->leaf == SYSCTL_CLK_NODE_MAX)
        return 0;

    ret = k230_clk_set_parent_div(step->leaf, step->parent,
        step->div_num, step->div_den);
    if (ret != 0) {
        rt_kprintf("k230_pm_dvfs: %s freq set failed: %d Hz\n",
            domain->desc->name, step->freq);
        return K230_ERR_PM_ILLEGAL_PARAM;
    }

    actual_freq = sysctl_media_clk_get_leaf_freq(step->leaf);
    if (!k230_dvfs_freq_match(actual_freq, (uint32_t)step->freq)) {
        rt_kprintf("k230_pm_dvfs: %s freq readback mismatch: target=%d Hz actual=%u Hz\n",
            domain->desc->name, step->freq, actual_freq);
        return K230_ERR_PM_ILLEGAL_PARAM;
    }

    rt_kprintf("k230_pm_dvfs: %s freq set: %d Hz\n",
        domain->desc->name, step->freq);
    return 0;
}

static void auto_governor_reset(void)
{
    auto_up_hold_count = 0;
    auto_down_hold_count = 0;
    k230_pm_pid_reset(&auto_pid_state);
    auto_last_change_tick = 0;
}

static bool auto_governor_can_change(void)
{
    rt_tick_t interval = rt_tick_from_millisecond(AUTO_GOVERNOR_MIN_CHANGE_TIME);

    return auto_last_change_tick == 0 ||
        rt_tick_get() - auto_last_change_tick >= interval;
}

static void auto_governor_log_change(struct k230_dvfs_domain *domain, int load,
    int32_t pid_score, int32_t from_idx, int32_t to_idx)
{
    rt_kprintf("k230_pm_dvfs: auto cpu freq: load=%d%% pid=%d profile %d->%d freq %d->%d Hz\n",
        load, pid_score, from_idx, to_idx,
        domain->desc->steps[from_idx].freq, domain->desc->steps[to_idx].freq);
}

static int32_t auto_governor_get_profile(struct k230_dvfs_domain *domain)
{
    int load;
    int32_t actual_idx;
    int32_t target_idx;
    int32_t pid_score;

    load = sys_cpu_usage(0);
    actual_idx = k230_dvfs_profile_index_transform(domain,
        domain->actual_profile_idx);

    if (load >= CPU_USAGE_INVALID)
        return actual_idx;

    pid_score = k230_pm_pid_calc(&auto_pid_config, &auto_pid_state, load);
    target_idx = actual_idx;

    if (pid_score > AUTO_PID_UP_THRESHOLD) {
        auto_down_hold_count = 0;
        if (actual_idx > 0 && ++auto_up_hold_count >=
            AUTO_GOVERNOR_UP_HOLD_COUNT && auto_governor_can_change())
            target_idx = actual_idx - 1;
    } else if (pid_score < AUTO_PID_DOWN_THRESHOLD) {
        auto_up_hold_count = 0;
        if (actual_idx < (int32_t)domain->desc->step_count - 1 &&
            ++auto_down_hold_count >= AUTO_GOVERNOR_DOWN_HOLD_COUNT &&
            auto_governor_can_change())
            target_idx = actual_idx + 1;
    } else {
        auto_up_hold_count = 0;
        auto_down_hold_count = 0;
    }

    if (target_idx != actual_idx) {
        auto_up_hold_count = 0;
        auto_down_hold_count = 0;
        auto_last_change_tick = rt_tick_get();
        auto_governor_log_change(domain, load, pid_score, actual_idx, target_idx);
    }

    return target_idx;
}

static int k230_dvfs_get_expect_profile(struct k230_dvfs_domain *domain)
{
    if (domain->governor == K230_PM_GOVERNOR_PERFORMANCE)
        domain->expect_profile_idx = 0;
    else if (domain->governor == K230_PM_GOVERNOR_ENERGYSAVING)
        domain->expect_profile_idx = domain->desc->step_count - 1;
    else if (domain->governor == K230_PM_GOVERNOR_AUTO)
        domain->expect_profile_idx = auto_governor_get_profile(domain);

    return 0;
}

static int k230_dvfs_set_actual_profile_locked(struct k230_dvfs_domain *domain)
{
    int ret;
    int rollback_ret;
    int32_t from_idx_cvt;
    int32_t to_idx;
    int32_t to_idx_cvt;
    int32_t expect_profile_idx;
    int32_t expect_profile_idx_cvt;
    int32_t thermal_protect_idx;
    int32_t thermal_protect_idx_cvt;

    thermal_protect_idx = domain->thermal_protect_idx;
    expect_profile_idx = domain->expect_profile_idx;
    thermal_protect_idx_cvt = k230_dvfs_profile_index_transform(domain,
        thermal_protect_idx);
    expect_profile_idx_cvt = k230_dvfs_profile_index_transform(domain,
        expect_profile_idx);

    if (domain->thermal_over &&
        PROFILE_IS_UP(thermal_protect_idx_cvt, expect_profile_idx_cvt)) {
        to_idx = thermal_protect_idx;
        to_idx_cvt = thermal_protect_idx_cvt;
    } else {
        to_idx = expect_profile_idx;
        to_idx_cvt = expect_profile_idx_cvt;
    }

    if (!domain->profile_valid &&
        k230_dvfs_read_hw_profile(domain, &domain->actual_profile_idx))
        domain->profile_valid = true;

    from_idx_cvt = k230_dvfs_profile_index_transform(domain,
        domain->actual_profile_idx);
    if (domain->profile_valid && from_idx_cvt == to_idx_cvt) {
        return 0;
    }

    if (domain->regulator == RT_NULL) {
        ret = k230_dvfs_set_clock_step(domain, to_idx_cvt);
    } else if (!domain->profile_valid) {
        /* With an unknown boot frequency, raise to the maximum required
         * voltage before changing the clock, then settle at the target. */
        ret = regulator_set_voltage(domain->regulator,
            domain->desc->steps[0].volt);
        if (ret == 0) {
            rt_thread_mdelay(1);
            ret = k230_dvfs_set_clock_step(domain, to_idx_cvt);
        }
        if (ret == 0 && domain->desc->steps[0].volt !=
            domain->desc->steps[to_idx_cvt].volt) {
            ret = regulator_set_voltage(domain->regulator,
                domain->desc->steps[to_idx_cvt].volt);
        }
    } else if (PROFILE_IS_UP(from_idx_cvt, to_idx_cvt)) {
        if (domain->desc->steps[from_idx_cvt].volt ==
            domain->desc->steps[to_idx_cvt].volt) {
            ret = 0;
        } else {
            ret = regulator_set_voltage(domain->regulator,
                domain->desc->steps[to_idx_cvt].volt);
            rt_thread_mdelay(1);
        }
        if (ret == 0)
            ret = k230_dvfs_set_clock_step(domain, to_idx_cvt);
    } else {
        ret = k230_dvfs_set_clock_step(domain, to_idx_cvt);
        if (ret == 0 && domain->desc->steps[from_idx_cvt].volt !=
            domain->desc->steps[to_idx_cvt].volt) {
            ret = regulator_set_voltage(domain->regulator,
                domain->desc->steps[to_idx_cvt].volt);
            if (ret != 0) {
                rollback_ret = k230_dvfs_set_clock_step(domain, from_idx_cvt);
                if (rollback_ret != 0)
                    rt_kprintf("k230_pm_dvfs: %s clock rollback failed: %d\n",
                        domain->desc->name, rollback_ret);
            }
        }
    }

    if (ret == 0) {
        domain->actual_profile_idx = to_idx;
        domain->profile_valid = true;
    } else {
        domain->profile_valid = k230_dvfs_read_hw_profile(domain,
            &domain->actual_profile_idx);
        if (domain->profile_valid && domain->regulator != RT_NULL) {
            int32_t actual_idx_cvt = k230_dvfs_profile_index_transform(domain,
                domain->actual_profile_idx);

            rollback_ret = regulator_set_voltage(domain->regulator,
                domain->desc->steps[actual_idx_cvt].volt);
            if (rollback_ret != 0)
                rt_kprintf("k230_pm_dvfs: %s voltage reconcile failed: %d\n",
                    domain->desc->name, rollback_ret);
        }
    }

    return ret;
}

static int k230_dvfs_start_auto_thread(void);

static int k230_dvfs_set_domain_governor(struct k230_dvfs_domain *domain,
    k230_pm_uapi_governor governor)
{
    bool supports_auto = (domain->desc->caps & K230_DVFS_CAP_AUTO) != 0U;
    bool cpu_domain = domain == &k230_dvfs_domains[K230_PM_DOMAIN_CPU];
    bool entering_auto;
    bool leaving_auto;
    bool print_auto_enabled = false;
    int ret;

    if (governor != K230_PM_GOVERNOR_MANUAL &&
        governor != K230_PM_GOVERNOR_PERFORMANCE &&
        governor != K230_PM_GOVERNOR_ENERGYSAVING &&
        governor != K230_PM_GOVERNOR_AUTO)
        return K230_ERR_PM_ILLEGAL_PARAM;

    if (governor == K230_PM_GOVERNOR_AUTO && !supports_auto)
        return K230_ERR_PM_ILLEGAL_PARAM;

    rt_mutex_take(&domain->mutex, RT_WAITING_FOREVER);
    entering_auto = cpu_domain && governor == K230_PM_GOVERNOR_AUTO &&
        domain->governor != K230_PM_GOVERNOR_AUTO;
    leaving_auto = cpu_domain && governor != K230_PM_GOVERNOR_AUTO &&
        domain->governor == K230_PM_GOVERNOR_AUTO;

    if (entering_auto) {
        ret = k230_dvfs_start_auto_thread();
        if (ret != 0)
            goto fail;
        if (usage_set_worker_priority(K230_DVFS_USAGE_THREAD_PRIORITY) !=
            RT_EOK) {
            ret = K230_ERR_PM_NOTREADY;
            goto fail;
        }
        if (usage_set_period(AUTO_GOVERNOR_POLL_TIME) != RT_EOK) {
            (void)usage_set_worker_priority(
                K230_DVFS_USAGE_DEFAULT_PRIORITY);
            ret = K230_ERR_PM_NOTREADY;
            goto fail;
        }
    } else if (leaving_auto) {
        if (usage_set_period(K230_DVFS_USAGE_DEFAULT_PERIOD) != RT_EOK) {
            ret = K230_ERR_PM_NOTREADY;
            goto fail;
        }
        if (usage_set_worker_priority(K230_DVFS_USAGE_DEFAULT_PRIORITY) !=
            RT_EOK) {
            (void)usage_set_period(AUTO_GOVERNOR_POLL_TIME);
            ret = K230_ERR_PM_NOTREADY;
            goto fail;
        }
    }

    domain->governor = governor;

    if (cpu_domain) {
        auto_load_enable = governor == K230_PM_GOVERNOR_AUTO;
        auto_governor_reset();
        print_auto_enabled = governor == K230_PM_GOVERNOR_AUTO;
    }

    if (governor == K230_PM_GOVERNOR_MANUAL) {
        memset(domain->profile_lock, 0, sizeof(domain->profile_lock));
        domain->manual_profile_idx = domain->profile_valid ?
            domain->actual_profile_idx : domain->expect_profile_idx;
        domain->expect_profile_idx = domain->manual_profile_idx;
    }
    rt_mutex_release(&domain->mutex);

    if (print_auto_enabled)
        rt_kprintf("k230_pm_dvfs: auto cpu governor enabled: target=%d%% poll=%d ms min_step=%d ms\n",
            AUTO_GOVERNOR_TARGET_LOAD, AUTO_GOVERNOR_POLL_TIME,
            AUTO_GOVERNOR_MIN_CHANGE_TIME);

    return 0;

fail:
    rt_mutex_release(&domain->mutex);
    return ret;
}

static void auto_load_thread(void *arg)
{
    struct k230_dvfs_domain *cpu = &k230_dvfs_domains[K230_PM_DOMAIN_CPU];

    (void)arg;

    while (1) {
        rt_thread_mdelay(AUTO_GOVERNOR_POLL_TIME);

        rt_mutex_take(&cpu->mutex, RT_WAITING_FOREVER);
        if (auto_load_enable) {
            k230_dvfs_get_expect_profile(cpu);
            (void)k230_dvfs_set_actual_profile_locked(cpu);
        }
        rt_mutex_release(&cpu->mutex);
    }
}

static int k230_dvfs_start_auto_thread(void)
{
    rt_thread_t tid;
    rt_err_t ret;

    if (auto_thread_started)
        return 0;

    tid = rt_thread_create("auto_load_thread", auto_load_thread,
        RT_NULL, 1024 * 10, K230_DVFS_AUTO_THREAD_PRIORITY,
        rt_tick_from_millisecond(10));
    if (tid == RT_NULL)
        return K230_ERR_PM_NOMEM;

    ret = rt_thread_startup(tid);
    if (ret != RT_EOK) {
        rt_thread_delete(tid);
        return K230_ERR_PM_NOTREADY;
    }
    auto_thread_started = true;
    return 0;
}

static int k230_dvfs_vote_locked_profile(struct k230_dvfs_domain *domain)
{
    uint32_t i;

    domain->expect_profile_idx = domain->manual_profile_idx;
    for (i = 0; i < domain->desc->step_count; i++) {
        if (domain->profile_lock[i]) {
            domain->expect_profile_idx = i;
            break;
        }
    }

    return 0;
}

static int k230_dvfs_clock_vote_dev(const struct k230_pm_dev_desc *desc,
    bool enable)
{
    unsigned int changed = 0;
    unsigned int i;
    int ret = 0;

    if (desc == RT_NULL)
        return K230_ERR_PM_UNEXIST;

    for (i = 0; i < desc->clock_count; i++) {
        if (desc->clocks[i] == SYSCTL_CLK_NODE_MAX)
            continue;
        ret = enable ? k230_clk_get(desc->clocks[i]) :
            k230_clk_put(desc->clocks[i]);
        if (ret != 0)
            goto rollback;
        changed++;
    }

    return 0;

rollback:
    while (changed > 0) {
        changed--;
        if (desc->clocks[changed] == SYSCTL_CLK_NODE_MAX)
            continue;
        if (enable)
            (void)k230_clk_put(desc->clocks[changed]);
        else
            (void)k230_clk_get(desc->clocks[changed]);
    }

    return K230_ERR_PM_BUSY;
}

int k230_pm_dvfs_get_stat(k230_pm_uapi_domain domain)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    const struct k230_pm_dev_desc *desc;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);
    if (dvfs->desc->pm_dev < 0)
        return 1;

    desc = k230_pm_dev_desc_find((enum k230_pm_dev_id)dvfs->desc->pm_dev);
    if (desc == RT_NULL)
        return K230_ERR_PM_UNEXIST;
    if (desc->pd >= 0)
        return k230_pd_hw_powered((sysctl_pwr_domain_e)desc->pd) ? 1 : 0;

    return 1;
}

int k230_pm_dvfs_get_profiles(k230_pm_uapi_domain domain, uint32_t *number,
    k230_pm_uapi_profile *profiles)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    uint32_t count;
    uint32_t i;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);
    if (number == RT_NULL)
        return K230_ERR_PM_NULL_PTR;

    count = *number;
    if (count == 0) {
        *number = dvfs->desc->step_count;
        return 0;
    }

    if (count > dvfs->desc->step_count)
        count = dvfs->desc->step_count;
    *number = count;

    if (profiles == RT_NULL)
        return K230_ERR_PM_NULL_PTR;

    for (i = 0; i < count; i++) {
        profiles[i].freq = dvfs->desc->steps[i].freq;
        profiles[i].volt = dvfs->desc->steps[i].volt;
    }

    return 0;
}

int k230_pm_dvfs_set_governor(k230_pm_uapi_domain domain,
    k230_pm_uapi_governor governor)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    int ret;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);

    ret = k230_dvfs_set_domain_governor(dvfs, governor);
    if (ret != 0)
        return ret;

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    ret = k230_dvfs_get_expect_profile(dvfs);
    if (ret == 0)
        ret = k230_dvfs_set_actual_profile_locked(dvfs);
    rt_mutex_release(&dvfs->mutex);

    return ret;
}

int k230_pm_dvfs_get_governor(k230_pm_uapi_domain domain,
    k230_pm_uapi_governor *governor)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);
    if (governor == RT_NULL)
        return K230_ERR_PM_NULL_PTR;

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    *governor = dvfs->governor;
    rt_mutex_release(&dvfs->mutex);
    return 0;
}

int k230_pm_dvfs_set_profile(k230_pm_uapi_domain domain, int32_t index)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    int32_t old_manual_profile_idx;
    int32_t old_expect_profile_idx;
    int ret;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    if (dvfs->governor != K230_PM_GOVERNOR_MANUAL) {
        rt_mutex_release(&dvfs->mutex);
        return K230_ERR_PM_NOT_SUPPORT;
    }

    index = k230_dvfs_profile_index_transform(dvfs, index);
    old_manual_profile_idx = dvfs->manual_profile_idx;
    old_expect_profile_idx = dvfs->expect_profile_idx;
    dvfs->manual_profile_idx = index;
    k230_dvfs_vote_locked_profile(dvfs);
    ret = k230_dvfs_set_actual_profile_locked(dvfs);
    if (ret != 0) {
        dvfs->manual_profile_idx = old_manual_profile_idx;
        dvfs->expect_profile_idx = old_expect_profile_idx;
    }
    rt_mutex_release(&dvfs->mutex);

    return ret;
}

int k230_pm_dvfs_get_profile(k230_pm_uapi_domain domain, int32_t *index)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);
    if (index == RT_NULL)
        return K230_ERR_PM_NULL_PTR;

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    if (!dvfs->profile_valid &&
        k230_dvfs_read_hw_profile(dvfs, &dvfs->actual_profile_idx))
        dvfs->profile_valid = true;
    if (!dvfs->profile_valid) {
        rt_mutex_release(&dvfs->mutex);
        return K230_ERR_PM_NOTREADY;
    }
    *index = k230_dvfs_profile_index_transform(dvfs, dvfs->actual_profile_idx);
    rt_mutex_release(&dvfs->mutex);
    return 0;
}

int k230_pm_dvfs_set_profile_lock(k230_pm_uapi_domain domain, int32_t index)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    int32_t old_expect_profile_idx;
    int ret;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    if (dvfs->governor != K230_PM_GOVERNOR_MANUAL) {
        rt_mutex_release(&dvfs->mutex);
        return K230_ERR_PM_NOT_SUPPORT;
    }

    index = k230_dvfs_profile_index_transform(dvfs, index);
    if (dvfs->profile_lock[index] >= UINT32_MAX) {
        rt_mutex_release(&dvfs->mutex);
        return K230_ERR_PM_BUSY;
    }

    old_expect_profile_idx = dvfs->expect_profile_idx;
    dvfs->profile_lock[index]++;
    k230_dvfs_vote_locked_profile(dvfs);

    ret = k230_dvfs_set_actual_profile_locked(dvfs);
    if (ret != 0) {
        dvfs->profile_lock[index]--;
        dvfs->expect_profile_idx = old_expect_profile_idx;
    }
    rt_mutex_release(&dvfs->mutex);
    return ret;
}

int k230_pm_dvfs_set_profile_unlock(k230_pm_uapi_domain domain, int32_t index)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    int32_t old_expect_profile_idx;
    int ret;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    if (dvfs->governor != K230_PM_GOVERNOR_MANUAL) {
        rt_mutex_release(&dvfs->mutex);
        return K230_ERR_PM_NOT_SUPPORT;
    }

    index = k230_dvfs_profile_index_transform(dvfs, index);
    if (dvfs->profile_lock[index] == 0) {
        rt_mutex_release(&dvfs->mutex);
        return K230_ERR_PM_BUSY;
    }

    old_expect_profile_idx = dvfs->expect_profile_idx;
    dvfs->profile_lock[index]--;
    k230_dvfs_vote_locked_profile(dvfs);

    ret = k230_dvfs_set_actual_profile_locked(dvfs);
    if (ret != 0) {
        dvfs->profile_lock[index]++;
        dvfs->expect_profile_idx = old_expect_profile_idx;
    }
    rt_mutex_release(&dvfs->mutex);
    return ret;
}

int k230_pm_dvfs_set_thermal_protect(k230_pm_uapi_domain domain,
    int32_t temp, int32_t index)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    int ret;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);
    if ((dvfs->desc->caps & K230_DVFS_CAP_THERMAL) == 0U)
        return K230_ERR_PM_NOT_SUPPORT;
    if (!k230_dvfs_thermal_temp_valid(temp) ||
        !k230_dvfs_profile_index_valid(dvfs, index))
        return K230_ERR_PM_ILLEGAL_PARAM;

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    dvfs->thermal_protect_temp = temp;
    dvfs->thermal_protect_idx = index;

    ret = k230_dvfs_set_actual_profile_locked(dvfs);
    rt_mutex_release(&dvfs->mutex);
    return ret;
}

int k230_pm_dvfs_get_thermal_protect(k230_pm_uapi_domain domain,
    int32_t *temp, int32_t *index)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);
    if ((dvfs->desc->caps & K230_DVFS_CAP_THERMAL) == 0U)
        return K230_ERR_PM_NOT_SUPPORT;
    if (temp == RT_NULL || index == RT_NULL)
        return K230_ERR_PM_NULL_PTR;

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    *temp = dvfs->thermal_protect_temp;
    *index = dvfs->thermal_protect_idx;
    rt_mutex_release(&dvfs->mutex);
    return 0;
}

int k230_pm_dvfs_set_thermal_shutdown(int32_t temp)
{
    if (!k230_dvfs_thermal_temp_valid(temp))
        return K230_ERR_PM_ILLEGAL_PARAM;

    k230_dvfs_thermal_shutdown = temp;
    return 0;
}

int k230_pm_dvfs_get_thermal_shutdown(int32_t *temp)
{
    if (temp == RT_NULL)
        return K230_ERR_PM_NULL_PTR;

    *temp = k230_dvfs_thermal_shutdown;
    return 0;
}

int32_t k230_pm_dvfs_thermal_shutdown_temp(void)
{
    return k230_dvfs_thermal_shutdown;
}

int k230_pm_dvfs_set_clock(k230_pm_uapi_domain domain, bool enable)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    const struct k230_pm_dev_desc *desc;
    int ret;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);
    if (dvfs->desc->pm_dev < 0)
        return K230_ERR_PM_NOT_SUPPORT;

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    if (enable) {
        if (dvfs->clock_vote_count == UINT32_MAX) {
            rt_mutex_release(&dvfs->mutex);
            return K230_ERR_PM_BUSY;
        }
    } else if (dvfs->clock_vote_count == 0) {
        rt_mutex_release(&dvfs->mutex);
        return K230_ERR_PM_BUSY;
    }

    desc = k230_pm_dev_desc_find((enum k230_pm_dev_id)dvfs->desc->pm_dev);
    ret = k230_dvfs_clock_vote_dev(desc, enable);
    if (ret == 0) {
        if (enable)
            dvfs->clock_vote_count++;
        else
            dvfs->clock_vote_count--;
    }
    rt_mutex_release(&dvfs->mutex);
    return ret;
}

int k230_pm_dvfs_set_power(k230_pm_uapi_domain domain, bool enable)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    const struct k230_pm_dev_desc *desc;
    int ret;

    if (dvfs == RT_NULL)
        return k230_dvfs_domain_check(domain);
    if (dvfs->desc->pm_dev < 0)
        return K230_ERR_PM_NOT_SUPPORT;

    desc = k230_pm_dev_desc_find((enum k230_pm_dev_id)dvfs->desc->pm_dev);
    if (desc == RT_NULL || desc->pd < 0)
        return K230_ERR_PM_NOT_SUPPORT;

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    if (enable) {
        if (dvfs->power_vote_count == UINT32_MAX) {
            rt_mutex_release(&dvfs->mutex);
            return K230_ERR_PM_BUSY;
        }

        ret = k230_pd_get((sysctl_pwr_domain_e)desc->pd);
        if (ret == 0) {
            ret = k230_dvfs_set_actual_profile_locked(dvfs);
            if (ret == 0)
                dvfs->power_vote_count++;
            else
                (void)k230_pd_put((sysctl_pwr_domain_e)desc->pd);
        }
    } else {
        if (dvfs->power_vote_count == 0) {
            rt_mutex_release(&dvfs->mutex);
            return K230_ERR_PM_BUSY;
        }

        ret = k230_pd_put((sysctl_pwr_domain_e)desc->pd);
        if (ret == 0) {
            dvfs->power_vote_count--;
            if (!k230_pd_hw_powered((sysctl_pwr_domain_e)desc->pd))
                dvfs->profile_valid = false;
        }
    }
    rt_mutex_release(&dvfs->mutex);

    return ret == 0 ? 0 : K230_ERR_PM_BUSY;
}

void k230_pm_dvfs_thermal_update(k230_pm_uapi_domain domain, int32_t temp)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    bool thermal_over;

    if (dvfs == RT_NULL)
        return;
    if ((dvfs->desc->caps & K230_DVFS_CAP_THERMAL) == 0U)
        return;

    rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
    if (dvfs->thermal_over)
        thermal_over = temp > dvfs->thermal_protect_temp -
            K230_DVFS_THERMAL_HYSTERESIS_TEMP;
    else
        thermal_over = temp > dvfs->thermal_protect_temp;

    if (thermal_over != dvfs->thermal_over) {
        if (thermal_over)
            rt_kprintf("k230_pm_dvfs: domain %s thermal protect: temp=%d C limit=%d C profile=%d\n",
                dvfs->desc->name, temp, dvfs->thermal_protect_temp,
                dvfs->thermal_protect_idx);
        else
            rt_kprintf("k230_pm_dvfs: domain %s thermal protect clear: temp=%d C limit=%d C\n",
                dvfs->desc->name, temp, dvfs->thermal_protect_temp);
    }

    dvfs->thermal_over = thermal_over;
    (void)k230_dvfs_set_actual_profile_locked(dvfs);
    rt_mutex_release(&dvfs->mutex);
}

void k230_pm_dvfs_dump(void)
{
    uint32_t i;

    rt_kprintf("%-8s %-13s %-6s %-6s %-6s %-6s %-8s %-8s %-10s %s\n",
        "domain", "governor", "expect", "actual", "cvote", "pvote",
        "limit_C", "tprof", "regulator", "locks");
    for (i = 0; i < k230_dvfs_domain_desc_count; i++) {
        struct k230_dvfs_domain *dvfs = &k230_dvfs_domains[i];
        uint32_t j;

        if (dvfs->desc == RT_NULL)
            continue;

        rt_kprintf("%-8s %-13d %-6d %-6d %-6u %-6u %-8d %-8d %-10s ",
            dvfs->desc->name, dvfs->governor,
            k230_dvfs_profile_index_transform(dvfs, dvfs->expect_profile_idx),
            k230_dvfs_profile_index_transform(dvfs, dvfs->actual_profile_idx),
            dvfs->clock_vote_count, dvfs->power_vote_count,
            dvfs->thermal_protect_temp, dvfs->thermal_protect_idx,
            dvfs->regulator ? "yes" : "no");
        for (j = 0; j < dvfs->desc->step_count; j++) {
            if (dvfs->profile_lock[j])
                rt_kprintf("%u:%u ", j, dvfs->profile_lock[j]);
        }
        rt_kprintf("%s\n", dvfs->thermal_over ? "hot" : "");
    }
}

int k230_pm_dvfs_init(void)
{
    uint32_t i;
    int ret;

    if (k230_dvfs_ready)
        return 0;

    if (!k230_dvfs_domains_initialized) {
        for (i = 0; i < k230_dvfs_domain_desc_count; i++) {
            const struct k230_dvfs_domain_desc *desc =
                &k230_dvfs_domain_desc[i];
            struct k230_dvfs_domain *dvfs = &k230_dvfs_domains[i];

            rt_mutex_init(&dvfs->mutex, "dvfs", RT_IPC_FLAG_PRIO);

            /* Leave a bad domain unregistered (desc == NULL) so lookups
             * report it as missing, but keep the others usable. */
            if (desc->step_count == 0 ||
                desc->step_count > K230_DVFS_MAX_PROFILES) {
                rt_kprintf("k230_pm_dvfs: %s has invalid step count %u (max %u), skipped\n",
                    desc->name, desc->step_count, K230_DVFS_MAX_PROFILES);
                continue;
            }

            dvfs->desc = desc;
            dvfs->actual_profile_idx = 0;
            dvfs->profile_valid = k230_dvfs_read_hw_profile(dvfs,
                &dvfs->actual_profile_idx);
            dvfs->manual_profile_idx = 0;
            dvfs->expect_profile_idx = dvfs->manual_profile_idx;
            dvfs->governor = K230_PM_GOVERNOR_MANUAL;
            dvfs->thermal_protect_temp = K230_DVFS_THERMAL_DEFAULT_TEMP;
            dvfs->thermal_protect_idx = dvfs->desc->step_count - 1;

            if (dvfs->desc->regulator_name) {
                dvfs->regulator = (struct regulator_dev *)
                    rt_device_find(dvfs->desc->regulator_name);
                if (dvfs->regulator == RT_NULL)
                    rt_kprintf("k230_pm_dvfs: can't find %s device, skip voltage scaling\n",
                        dvfs->desc->regulator_name);
            }
        }

        k230_dvfs_thermal_shutdown = K230_DVFS_SHUTDOWN_DEFAULT_TEMP;
        k230_dvfs_domains_initialized = true;
    }

    for (i = 0; i < k230_dvfs_domain_desc_count; i++) {
        struct k230_dvfs_domain *dvfs = &k230_dvfs_domains[i];

        if (dvfs->desc == RT_NULL)
            continue;

        /* Best effort: a failed default apply must not take down PM init. */
        rt_mutex_take(&dvfs->mutex, RT_WAITING_FOREVER);
        ret = k230_dvfs_set_actual_profile_locked(dvfs);
        rt_mutex_release(&dvfs->mutex);
        if (ret != 0)
            rt_kprintf("k230_pm_dvfs: %s default profile apply failed: %d\n",
                dvfs->desc->name, ret);
    }

    ret = k230_pm_thermal_init();
    if (ret != 0)
        return ret;

    k230_dvfs_ready = true;
    return 0;
}

__attribute__((weak)) int regulator_set_voltage(struct regulator_dev *pdev, int vol_uv)
{
    (void)pdev;
    (void)vol_uv;
    return -1;
}
