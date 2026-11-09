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
#include "k230_pm_thermal.h"
#include "k230_pm_pid.h"
#include "usage.h"
#include "regulator.h"

#define PROFILE_IS_UP(from, to) ((from) > (to))
#define K230_DVFS_MAX_PROFILES K230_PM_MAX_PROFILE_COUNT
#define K230_DVFS_THERMAL_DEFAULT_TEMP 85
#define K230_DVFS_SHUTDOWN_MAX_TEMP 115
#ifdef K230_PM_THERMAL_PROTECT
/* The compiled thermal policy supplies the effective default limit. */
#define K230_DVFS_SHUTDOWN_DEFAULT_TEMP K230_DVFS_SHUTDOWN_MAX_TEMP
#else
#define K230_DVFS_SHUTDOWN_DEFAULT_TEMP 100
#endif
#define K230_DVFS_THERMAL_HYSTERESIS_TEMP 5
#define K230_DVFS_THERMAL_MIN_TEMP (-40)
#ifdef K230_PM_THERMAL_PROTECT
#define K230_DVFS_THERMAL_MAX_TEMP (K230_DVFS_SHUTDOWN_MAX_TEMP - 1)
#else
/* Nothing enforces these without thermal protection: keep the sensor range. */
#define K230_DVFS_THERMAL_MAX_TEMP 125
#endif
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
    bool voltage_valid;
    int verified_voltage;
    k230_pm_uapi_governor governor;
    int32_t thermal_protect_temp;
    int32_t thermal_protect_idx;
    bool thermal_over;
    bool thermal_recovering;
    bool thermal_restore_pending;
    rt_tick_t thermal_recovery_tick;
    struct k230_thermal_sample thermal_sample;
    struct regulator_dev *regulator;
    /* The board has this rail but its provider failed: transitions blocked. */
    bool regulator_failed;
};

static struct k230_dvfs_domain k230_dvfs_domains[K230_PM_DOMAIN_NR];
/* Runtime ceiling in whole degrees; the compiled policy may require less. */
static int32_t k230_dvfs_thermal_shutdown = K230_DVFS_SHUTDOWN_DEFAULT_TEMP;
static bool k230_dvfs_domains_initialized;
static bool k230_dvfs_ready;
static struct rt_mutex k230_dvfs_config_mutex;

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

static int32_t k230_dvfs_mc_to_c_floor(int32_t temp_mc)
{
    return temp_mc / 1000 - (temp_mc % 1000 < 0);
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

static int k230_dvfs_safe_voltage(struct k230_dvfs_domain *domain, int voltage)
{
    int actual = 0;
    int ret;
    if (k230_pm_shutdown_terminal())
        return K230_ERR_PM_BUSY;
    domain->voltage_valid = false;
    /* The regulator waits for a rise to settle before returning. */
    ret = regulator_set_voltage(domain->regulator, voltage, domain->desc->steps[0].volt);
    if (k230_pm_shutdown_terminal())
        return K230_ERR_PM_BUSY;
    /* A successful write alone does not establish a known-safe voltage. */
    if (ret == 0)
        ret = regulator_get_voltage(domain->regulator, &actual);
    if (k230_pm_shutdown_terminal())
        return K230_ERR_PM_BUSY;
    domain->voltage_valid = ret == 0 && actual >= voltage &&
        actual <= domain->desc->steps[0].volt;
    if (domain->voltage_valid)
        domain->verified_voltage = actual;
    return domain->voltage_valid ? 0 : K230_ERR_PM_NOTREADY;
}

static int32_t k230_dvfs_limit_index(struct k230_dvfs_domain *domain)
{
    uint32_t limit = k230_pm_thermal_limit(domain->desc->steps[0].leaf);
    int32_t i;
    for (i = 0; i < (int32_t)domain->desc->step_count; i++)
        if ((uint32_t)domain->desc->steps[i].freq <= limit)
            return i;
    return domain->desc->step_count - 1;
}

static int k230_dvfs_apply_locked(struct k230_dvfs_domain *domain, bool may_raise)
{
    int ret = 0;
    int32_t from, to, limit;
    bool known;

    if (k230_pm_shutdown_terminal())
        return K230_ERR_PM_BUSY;
    if (domain->regulator_failed)
        return K230_ERR_PM_NOTREADY;
    to = k230_dvfs_profile_index_transform(domain, domain->expect_profile_idx);
    limit = k230_dvfs_limit_index(domain);
    if (domain->thermal_over) {
        int32_t extra = k230_dvfs_profile_index_transform(domain, domain->thermal_protect_idx);
        if (extra > limit)
            limit = extra;
    }
    if (to < limit)
        to = limit;

    /* Core power-up enforcement and external clock callers can change the
     * divider. Never trust a previously cached profile as clock readback. */
    known = k230_dvfs_read_hw_profile(domain, &domain->actual_profile_idx);
    domain->profile_valid = known;
    from = k230_dvfs_profile_index_transform(domain, domain->actual_profile_idx);
    if (!may_raise && known && from > to)
        to = from;
    if (!known && !may_raise) {
        uint32_t actual_hz = sysctl_media_clk_get_leaf_freq(domain->desc->steps[0].leaf);
        /* A valid lower boot/custom rate need not match a profile. A thermal
         * ceiling must not raise it merely to obtain a profile index. */
        if (actual_hz && actual_hz <= (uint32_t)domain->desc->steps[to].freq) {
            if (domain->regulator && (!domain->voltage_valid ||
                domain->verified_voltage < domain->desc->steps[to].volt))
                return k230_dvfs_safe_voltage(domain, domain->desc->steps[0].volt);
            return 0;
        }
    }
    if (known && from == to) {
        if (domain->regulator && (!domain->voltage_valid ||
            domain->verified_voltage < domain->desc->steps[to].volt))
            return k230_dvfs_safe_voltage(domain, domain->desc->steps[to].volt);
        return 0;
    }

    if (domain->regulator == RT_NULL) {
        ret = k230_dvfs_set_clock_step(domain, to);
    } else if (!known) {
        ret = k230_dvfs_safe_voltage(domain, domain->desc->steps[0].volt);
        if (ret == 0)
            ret = k230_dvfs_set_clock_step(domain, to);
        /* Maximum voltage is safe for this profile. A subsequent transition
         * may reduce it once the clock state is known. */
    } else if (PROFILE_IS_UP(from, to)) {
        ret = k230_dvfs_safe_voltage(domain, domain->desc->steps[to].volt);
        if (ret == 0) {
            if (k230_pm_shutdown_terminal())
                ret = K230_ERR_PM_BUSY;
            else
                ret = k230_dvfs_set_clock_step(domain, to);
        }
    } else {
        ret = k230_dvfs_set_clock_step(domain, to);
        if (ret == 0 && domain->desc->steps[from].volt != domain->desc->steps[to].volt) {
            ret = k230_dvfs_safe_voltage(domain, domain->desc->steps[to].volt);
            if (ret != 0) {
                /* Keep the lower clock. Restore and verify the known-safe
                 * voltage, never the previous hotter clock. */
                ret = k230_dvfs_safe_voltage(domain, domain->desc->steps[from].volt);
            }
        }
    }
    domain->profile_valid = k230_dvfs_read_hw_profile(domain, &domain->actual_profile_idx);
    if (!domain->profile_valid || domain->actual_profile_idx < limit)
        return K230_ERR_PM_NOTREADY;
    return ret;
}

static int k230_dvfs_set_actual_profile_locked(struct k230_dvfs_domain *domain)
{
    return k230_dvfs_apply_locked(domain, true);
}

/* Match the sensor's cadence plus conversion/scheduling allowance. Samples
 * arrive only from the thermal controller. */
static rt_tick_t k230_dvfs_sample_fresh_ticks(uint32_t interval_ticks)
{
#ifdef K230_PM_THERMAL_PROTECT
    struct k230_thermal_config config;
    k230_pm_thermal_config(&config);
    return k230_thermal_fresh_ticks(&config, interval_ticks);
#else
    (void)interval_ticks;
    return rt_tick_from_millisecond(6500);
#endif
}

static rt_tick_t k230_dvfs_recovery_ticks(void)
{
#ifdef K230_PM_THERMAL_PROTECT
    struct k230_thermal_config config;
    k230_pm_thermal_config(&config);
    return config.recovery_ticks;
#else
    return rt_tick_from_millisecond(5000);
#endif
}

static void k230_dvfs_extra_limit(struct k230_dvfs_domain *dvfs,
    const struct k230_thermal_sample *sample)
{
    rt_tick_t fresh = k230_dvfs_sample_fresh_ticks(sample ? sample->interval_ticks : 0);
    rt_tick_t gap = k230_dvfs_sample_fresh_ticks(dvfs->thermal_sample.interval_ticks);
    rt_tick_t recovery = k230_dvfs_recovery_ticks();
    bool new_sample = sample && sample->valid &&
        (!dvfs->thermal_sample.valid || sample->sequence != dvfs->thermal_sample.sequence);
    if (gap < fresh)
        gap = fresh;
    if (!sample || !sample->valid || sample->error ||
        (rt_tick_t)(rt_tick_get() - sample->tick) >= fresh) {
        dvfs->thermal_recovering = false;
        return;
    }
    if (sample->temperature_mc >= dvfs->thermal_protect_temp * 1000) {
        dvfs->thermal_over = true;
        dvfs->thermal_recovering = false;
    } else if (sample->temperature_mc >=
        (dvfs->thermal_protect_temp - K230_DVFS_THERMAL_HYSTERESIS_TEMP) * 1000) {
        dvfs->thermal_recovering = false;
    } else if (new_sample && dvfs->thermal_over) {
        if (!dvfs->thermal_recovering ||
            (rt_tick_t)(sample->tick - dvfs->thermal_sample.tick) >= gap) {
            dvfs->thermal_recovering = true;
            dvfs->thermal_recovery_tick = sample->tick;
        } else if ((rt_tick_t)(sample->tick - dvfs->thermal_recovery_tick) >= recovery) {
            dvfs->thermal_over = false;
            dvfs->thermal_recovering = false;
        }
    }
    dvfs->thermal_sample = *sample;
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
        /* Keep the requested profile, not the running one: a thermal
         * ceiling may be holding the clock lower and must not become the
         * manual choice once it lifts. */
        domain->manual_profile_idx = domain->expect_profile_idx;
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
    if (!k230_dvfs_thermal_temp_valid(temp) || !k230_dvfs_profile_index_valid(dvfs, index))
        return K230_ERR_PM_ILLEGAL_PARAM;
    if (rt_mutex_take(&k230_dvfs_config_mutex, rt_tick_from_millisecond(100)) != RT_EOK)
        return K230_ERR_PM_BUSY;
    if (temp * 1000 >= k230_pm_dvfs_thermal_shutdown_mc()) {
        rt_mutex_release(&k230_dvfs_config_mutex);
        return K230_ERR_PM_ILLEGAL_PARAM;
    }
    if (rt_mutex_take(&dvfs->mutex, rt_tick_from_millisecond(100)) != RT_EOK) {
        rt_mutex_release(&k230_dvfs_config_mutex);
        return K230_ERR_PM_BUSY;
    }
    dvfs->thermal_protect_temp = temp;
    dvfs->thermal_protect_idx = index;
    dvfs->thermal_recovering = false;
    rt_mutex_release(&k230_dvfs_config_mutex);
    k230_dvfs_extra_limit(dvfs, &dvfs->thermal_sample);
    ret = k230_dvfs_set_actual_profile_locked(dvfs);
    rt_mutex_release(&dvfs->mutex);
    k230_pm_thermal_notify();
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
#ifdef K230_PM_THERMAL_PROTECT
    struct k230_thermal_config config;
    uint32_t i;
    if (temp < K230_DVFS_THERMAL_MIN_TEMP || temp > K230_DVFS_SHUTDOWN_MAX_TEMP)
        return K230_ERR_PM_ILLEGAL_PARAM;
    k230_pm_thermal_config(&config);
    if (!k230_thermal_config_valid(&config) ||
        temp * 1000 <= config.sustained_mc || temp * 1000 > config.critical_mc)
        return K230_ERR_PM_ILLEGAL_PARAM;
    if (rt_mutex_take(&k230_dvfs_config_mutex, rt_tick_from_millisecond(100)) != RT_EOK)
        return K230_ERR_PM_BUSY;
    for (i = 0; i < k230_dvfs_domain_desc_count; ++i) {
        const struct k230_dvfs_domain *dvfs = &k230_dvfs_domains[i];
        if (dvfs->desc && (dvfs->desc->caps & K230_DVFS_CAP_THERMAL) &&
            dvfs->thermal_protect_temp >= temp) {
            rt_mutex_release(&k230_dvfs_config_mutex);
            return K230_ERR_PM_ILLEGAL_PARAM;
        }
    }
    __atomic_store_n(&k230_dvfs_thermal_shutdown, temp, __ATOMIC_RELEASE);
    rt_mutex_release(&k230_dvfs_config_mutex);
    k230_pm_thermal_notify();
    return 0;
#else
    /* Stored for /dev/pm only; nothing acts on it without thermal protection. */
    if (!k230_dvfs_thermal_temp_valid(temp))
        return K230_ERR_PM_ILLEGAL_PARAM;
    __atomic_store_n(&k230_dvfs_thermal_shutdown, temp, __ATOMIC_RELEASE);
    return 0;
#endif
}

int k230_pm_dvfs_get_thermal_shutdown(int32_t *temp)
{
    if (temp == RT_NULL)
        return K230_ERR_PM_NULL_PTR;

    *temp = k230_pm_dvfs_thermal_shutdown_temp();
    return 0;
}

int32_t k230_pm_dvfs_thermal_shutdown_temp(void)
{
    return k230_dvfs_mc_to_c_floor(k230_pm_dvfs_thermal_shutdown_mc());
}

int32_t k230_pm_dvfs_thermal_shutdown_mc(void)
{
    int32_t limit = __atomic_load_n(&k230_dvfs_thermal_shutdown, __ATOMIC_ACQUIRE) * 1000;
#ifdef K230_PM_THERMAL_PROTECT
    struct k230_thermal_config config;
    k230_pm_thermal_config(&config);
    if (config.critical_mc < limit)
        limit = config.critical_mc;
#endif
    return limit;
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

int k230_pm_dvfs_thermal_apply(k230_pm_uapi_domain domain,
    const struct k230_thermal_sample *sample, bool may_raise,
    struct k230_thermal_clocks *status)
{
    struct k230_dvfs_domain *dvfs = k230_dvfs_domain_get(domain);
    int ret;
    if (dvfs == RT_NULL || status == RT_NULL)
        return K230_ERR_PM_ILLEGAL_PARAM;
    status->requested_hz = k230_pm_thermal_limit(dvfs->desc->steps[0].leaf);
    status->actual_hz = sysctl_media_clk_get_leaf_freq(dvfs->desc->steps[0].leaf);
    if (rt_mutex_take(&dvfs->mutex, rt_tick_from_millisecond(100)) != RT_EOK) {
        status->error = K230_ERR_PM_BUSY;
        return status->error;
    }
    {
        bool was_over = dvfs->thermal_over;
        k230_dvfs_extra_limit(dvfs, sample);
        if (dvfs->thermal_over)
            dvfs->thermal_restore_pending = false;
        else if (was_over)
            dvfs->thermal_restore_pending = true;
        int32_t limit = k230_dvfs_limit_index(dvfs);
        if (dvfs->thermal_over) {
            int32_t extra = k230_dvfs_profile_index_transform(dvfs, dvfs->thermal_protect_idx);
            if (extra > limit) limit = extra;
        }
        status->requested_hz = dvfs->desc->steps[limit].freq;
        /* Clearing the extra cap is an edge, but restoring the saved request
         * may need retries even when the system ceiling never changes. */
        ret = k230_dvfs_apply_locked(dvfs, may_raise || dvfs->thermal_restore_pending);
    }
    status->actual_hz = sysctl_media_clk_get_leaf_freq(dvfs->desc->steps[0].leaf);
    if (ret == 0 && status->actual_hz > status->requested_hz +
        (status->requested_hz == UINT32_MAX ? 0 : K230_DVFS_FREQ_TOLERANCE_HZ))
        ret = K230_ERR_PM_NOTREADY;
    status->error = ret;
    if (ret == 0)
        dvfs->thermal_restore_pending = false;
    rt_mutex_release(&dvfs->mutex);
    return ret;
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
            dvfs->regulator ? "yes" : dvfs->regulator_failed ? "failed" : "no");
        for (j = 0; j < dvfs->desc->step_count; j++) {
            if (dvfs->profile_lock[j])
                rt_kprintf("%u:%u ", j, dvfs->profile_lock[j]);
        }
        rt_kprintf("%s\n", dvfs->thermal_over ? "hot" : "");
    }
}

int k230_pm_dvfs_prepare_reset(void)
{
    int ret = 0;
    unsigned int i;

    if (!k230_dvfs_domains_initialized || rt_interrupt_get_nest() || !rt_thread_self())
        return K230_ERR_PM_NOTREADY;
    for (i = 0; i < k230_dvfs_domain_desc_count; ++i) {
        struct k230_dvfs_domain *domain = &k230_dvfs_domains[i];
        int volt, actual = 0, r;

        if (!domain->desc || !domain->regulator || !domain->desc->step_count)
            continue;
        volt = domain->desc->steps[0].volt;
        /* Held until the reset, so no governor or thermal update lowers it. */
        if (rt_mutex_take(&domain->mutex, rt_tick_from_millisecond(100)) != RT_EOK) {
            ret = K230_ERR_PM_BUSY;
            continue;
        }
        /* Waits for the rise to settle before returning. */
        r = regulator_set_voltage(domain->regulator, volt, volt);
        if (r == 0)
            r = regulator_get_voltage(domain->regulator, &actual);
        if (r != 0 || actual < volt) {
            rt_kprintf("k230_pm_dvfs: %s boot voltage %d uV not restored: %d\n",
                domain->desc->name, volt, r);
            ret = K230_ERR_PM_NOTREADY;
        }
    }
    return ret;
}

int k230_pm_dvfs_init(void)
{
    uint32_t i;
    int ret;

    if (k230_dvfs_ready)
        return 0;

    if (!k230_dvfs_domains_initialized) {
        int32_t thermal_default = K230_DVFS_THERMAL_DEFAULT_TEMP;
#ifdef K230_PM_THERMAL_PROTECT
        struct k230_thermal_config config;
        k230_pm_thermal_config(&config);
        thermal_default = k230_dvfs_mc_to_c_floor(config.min_mc);
#endif
        if (rt_mutex_init(&k230_dvfs_config_mutex, "thermcfg", RT_IPC_FLAG_PRIO) != RT_EOK)
            return K230_ERR_PM_NOTREADY;
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
            dvfs->thermal_protect_temp = thermal_default;
            dvfs->thermal_protect_idx = dvfs->desc->step_count - 1;
#ifdef K230_PM_THERMAL_PROTECT
            /* The system monitor walks the profiles. An implicit per-domain
             * minimum would bypass that ramp; explicit user caps still apply. */
            dvfs->thermal_protect_idx = 0;
#endif

            /* No board rail: frequency-only. A failed one blocks transitions. */
            dvfs->regulator = regulator_get(dvfs->desc->regulator_name);
            if (dvfs->regulator && regulator_status(dvfs->regulator) != 0) {
                rt_kprintf("k230_pm_dvfs: regulator %s failed (%d); transitions blocked\n",
                    dvfs->desc->regulator_name, regulator_status(dvfs->regulator));
                dvfs->regulator = RT_NULL;
                dvfs->regulator_failed = true;
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

    /* Does not wait for a reading: the first one lowers clocks if needed. */
    ret = k230_pm_thermal_init();
    if (ret != 0)
        return ret;

    k230_dvfs_ready = true;
    return 0;
}
