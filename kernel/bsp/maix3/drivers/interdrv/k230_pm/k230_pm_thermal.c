/* SPDX-License-Identifier: BSD-2-Clause */
#include <rtthread.h>
#include <rthw.h>
#include <stdarg.h>
#include "k230_pm.h"
#include "k230_pm_dvfs.h"
#include "k230_pm_thermal.h"
#include "k230_pm_thermal_regulator.h"
#include "ts_hw.h"

#define SAMPLER_PRIORITY 8
#define CONTROLLER_PRIORITY 12
#define THERMAL_EVENT 1U
/* Same PLL rounding allowance as the DVFS readback check. */
#define THERMAL_FREQ_TOLERANCE_HZ 1000U
#define THERMAL_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Startup configuration, printed only at the verbose log level. */
#ifdef K230_PM_THERMAL_LOG_VERBOSE
#define thermal_log_config(...) rt_kprintf(__VA_ARGS__)
#else
#define thermal_log_config(...) do { if (0) rt_kprintf(__VA_ARGS__); } while (0)
#endif

static const enum k230_pm_dev_id thermal_accelerators[] = {
    K230_PM_DEV_AI, K230_PM_DEV_DPU, K230_PM_DEV_VPU
};

static struct rt_event thermal_event;
static struct rt_event sampler_event;
static rt_spinlock_t status_lock;
static rt_spinlock_t critical_lock;
static struct k230_thermal_reading retained_critical;
static bool retained_critical_pending;
static struct k230_thermal_policy published_policy;
static struct k230_thermal_clocks published_clocks[K230_THERMAL_DOMAINS];
static struct k230_thermal_regulator published_regulator;
static uint32_t published_caps[K230_THERMAL_DOMAINS];
static bool thermal_started;
static uint32_t sample_ticks;
static unsigned int reinit_requested;
static bool actuation_active;
static rt_tick_t actuation_start;
static bool sampling_urgent = true;
static bool sampling_restricted;
/* Written once by k230_pm_thermal_init() before either thread starts. */
static struct k230_thermal_stage_caps thermal_stage_caps;

static uint32_t thermal_stage_cap(enum k230_thermal_stage stage, enum k230_thermal_domain domain)
{
    return k230_thermal_stage_cap(&thermal_stage_caps, stage, domain);
}

static bool thermal_within(uint32_t actual, uint32_t cap)
{
    return cap == UINT32_MAX || actual <= cap + THERMAL_FREQ_TOLERANCE_HZ;
}

/* Thermal domains: DVFS domain, controlled clock and power domain (-1 for
 * always on). The dividers are always on; a rate matters only while its
 * power domain is. */
static const struct {
    const char *name;
    k230_pm_uapi_domain dvfs;
    sysctl_clk_node_e clk;
    int pd;
} thermal_domains[K230_THERMAL_DOMAINS] = {
    {"cpu", K230_PM_DOMAIN_CPU, SYSCTL_CLK_CPU1_CORE_CLK_ROOT, -1},
    {"kpu", K230_PM_DOMAIN_KPU, SYSCTL_CLK_AI_CORE_CLK, SYSCTL_PD_AI},
    {"vpu", K230_PM_DOMAIN_VPU, SYSCTL_CLK_VPU_CLK_ROOT, SYSCTL_PD_VPU},
};

/* Cut order: a heavier domain is lowered sooner (see the regulator). */
#ifndef K230_PM_THERMAL_WEIGHT_CPU
#define K230_PM_THERMAL_WEIGHT_CPU 1
#endif
#ifndef K230_PM_THERMAL_WEIGHT_KPU
#define K230_PM_THERMAL_WEIGHT_KPU 1
#endif
#ifndef K230_PM_THERMAL_WEIGHT_VPU
#define K230_PM_THERMAL_WEIGHT_VPU 1
#endif

static uint32_t thermal_hz(unsigned int d)
{
    return sysctl_media_clk_get_leaf_freq(thermal_domains[d].clk);
}

static bool thermal_powered(unsigned int d)
{
    return thermal_domains[d].pd < 0 ||
        k230_pd_hw_powered((sysctl_pwr_domain_e)thermal_domains[d].pd);
}

/* Device consoles can block on an unbounded TX mutex. Emergency diagnostics
 * use the direct SBI console before the interrupts-off board hook. */
static void thermal_polled_print(const char *fmt, ...)
{
    char buf[256];
    va_list args;

    va_start(args, fmt);
    rt_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    rt_hw_console_output(buf);
}

/* Preserve the first critical observation until the controller consumes it.
 * The immediate confirmation is allowed to update the ordinary reading cache. */
static void thermal_retain_critical(const struct k230_thermal_reading *sample)
{
    rt_base_t level = rt_spin_lock_irqsave(&critical_lock);

    if (!retained_critical_pending) {
        retained_critical = *sample;
        retained_critical_pending = true;
    }
    rt_spin_unlock_irqrestore(&critical_lock, level);
    rt_event_send(&thermal_event, THERMAL_EVENT);
}

static bool thermal_take_critical(struct k230_thermal_reading *sample)
{
    rt_base_t level = rt_spin_lock_irqsave(&critical_lock);
    bool pending = retained_critical_pending;

    if (pending) {
        *sample = retained_critical;
        retained_critical_pending = false;
    }
    rt_spin_unlock_irqrestore(&critical_lock, level);
    return pending;
}

static void thermal_shutdown(enum k230_thermal_reason reason, bool quiesce)
{
    struct k230_thermal_reading sample = {0};
    unsigned int i;
    bool first = k230_pm_shutdown_latch(reason);
#ifndef K230_PM_THERMAL_SHUTDOWN_NONE
    k230_pm_thermal_block_accelerators(true);
#endif
    if (first) {
        (void)k230_pm_thermal_reading(&sample);
        thermal_polled_print("thermal: shutdown request tick=%u reason=%u terminal=%u temp_mc=%d age_ticks=%u valid=%u error=%d cpu_hz=%u kpu_hz=%u vpu_hz=%u\n",
            (unsigned)rt_tick_get(), reason, k230_pm_shutdown_terminal(), sample.millicelsius,
            (unsigned)(rt_tick_get() - sample.tick), sample.valid, sample.error,
            thermal_hz(K230_THERMAL_CPU), thermal_hz(K230_THERMAL_KPU),
            thermal_hz(K230_THERMAL_VPU));
    }
#ifdef K230_PM_THERMAL_SHUTDOWN_NONE
    /* NONE is nonterminal: publish minimum caps before the controller catches
     * up, while leaving CPU and accelerator admission open. */
    k230_pm_thermal_set_limits(
        thermal_stage_cap(K230_THERMAL_MIN, K230_THERMAL_CPU),
        thermal_stage_cap(K230_THERMAL_MIN, K230_THERMAL_KPU),
        thermal_stage_cap(K230_THERMAL_MIN, K230_THERMAL_VPU));
    __atomic_store_n(&sampling_urgent, true, __ATOMIC_RELEASE);
    __atomic_store_n(&sampling_restricted, true, __ATOMIC_RELEASE);
    k230_pm_thermal_notify();
    return;
#endif
    if (quiesce) {
        __atomic_store_n(&actuation_start, rt_tick_get(), __ATOMIC_RELEASE);
        __atomic_store_n(&actuation_active, true, __ATOMIC_RELEASE);
        for (i = 0; i < THERMAL_ARRAY_SIZE(thermal_accelerators); ++i) {
            int ret = k230_pm_thermal_off(thermal_accelerators[i], true);
            if (ret)
                thermal_polled_print("thermal: quiesce device=%d failed=%d\n",
                    thermal_accelerators[i], ret);
        }
    }
    /* A terminal sampler path never waits for a PM, DVFS or regulator mutex. */
    k230_pm_thermal_shutdown_enter();
}

void k230_pm_thermal_notify(void)
{
    if (__atomic_load_n(&thermal_started, __ATOMIC_ACQUIRE)) {
        rt_event_send(&thermal_event, THERMAL_EVENT);
        rt_event_send(&sampler_event, THERMAL_EVENT);
    }
}

void k230_pm_thermal_clock_changed(void)
{
    /* A clock increase must shorten an already pending low-frequency wait.
     * Do not wake the controller for its own clock writes. */
    if (__atomic_load_n(&thermal_started, __ATOMIC_ACQUIRE))
        rt_event_send(&sampler_event, THERMAL_EVENT);
}

static void thermal_check_shutdown(void)
{
    unsigned int reason = k230_pm_shutdown_reason();
    if (reason && k230_pm_shutdown_terminal())
        thermal_shutdown((enum k230_thermal_reason)reason, false);
}

/* Sampler only. A wedged actuator also stalls the controller's sustained-heat
 * and sensor-fault handling, but it is a thermal fault only once the latest
 * reading requires a ceiling the clocks exceed, or reaches the sustained
 * threshold. Without a fresh reading, minimum clocks are required. */
static void thermal_check_actuator(const struct k230_thermal_config *config,
    const struct k230_thermal_reading *sample, bool fresh)
{
    enum k230_thermal_stage required = K230_THERMAL_MIN;
    unsigned int d;
    if (!__atomic_load_n(&actuation_active, __ATOMIC_ACQUIRE) ||
        (rt_tick_t)(rt_tick_get() - __atomic_load_n(&actuation_start, __ATOMIC_ACQUIRE)) <
            config->enforcement_ticks)
        return;
    if (fresh) {
        if (sample->millicelsius >= config->sustained_mc)
            thermal_shutdown(K230_THERMAL_ENFORCEMENT, false);
        required = k230_thermal_temperature_stage(config, sample->millicelsius);
    }
    for (d = 0; d < K230_THERMAL_DOMAINS; ++d)
        if (thermal_powered(d) && !thermal_within(thermal_hz(d),
                thermal_stage_cap(required, (enum k230_thermal_domain)d)))
            thermal_shutdown(K230_THERMAL_ENFORCEMENT, false);
}

static void thermal_sleep_1ms(void)
{
    rt_thread_mdelay(1);
}

/* Sampler only: the monitor owns the sensor, in single mode. */
static int thermal_sensor_setup(void)
{
    int ret = ts_hw_init();

    if (ret == RT_EOK) {
        ts_hw_configure(TS_HW_DEFAULT_TRIM, false);
        k230_thermal_reading_invalidate();
    }
    return ret;
}

/* Sampler only. Publishes the result, or the error with earlier data kept. */
static int thermal_sensor_read(void)
{
    static bool out_of_range_reported;
    int32_t milli = 0;
    int ret = ts_hw_read_single(&milli, thermal_sleep_1ms);

    if (ret == RT_EOK) {
        /* Report the first out-of-range reading, and pass such readings to
         * the policy unchanged. */
        if (milli < -40000 || milli > 125000) {
            if (!out_of_range_reported)
                rt_kprintf("thermal: sensor reading outside -40..125 C: %d mC ctrl=0x%02x\n",
                    milli, (unsigned)ts_hw_ctrl());
            out_of_range_reported = true;
        } else {
            out_of_range_reported = false;
        }
    }
    k230_thermal_reading_publish(ret, milli);
    return ret;
}

static bool thermal_fresh_sample(struct k230_thermal_reading *sample)
{
    return k230_pm_thermal_reading(sample) == RT_EOK && sample->valid && !sample->stale;
}

static void thermal_wait_sample(const struct k230_thermal_config *config,
    const struct k230_thermal_reading *sample, bool fresh, rt_tick_t begin, bool rapid_rise,
    int error)
{
    const struct k230_thermal_sample observation = {
        .temperature_mc = sample->millicelsius, .valid = sample->valid,
        .error = error ? error : sample->error,
    };
    bool notify = true;
    for (;;) {
        int interval_ret = RT_EOK;
        rt_uint32_t events;
        rt_tick_t spent, period;
        thermal_check_shutdown();
        thermal_check_actuator(config, sample, fresh);
        period = k230_thermal_poll_ticks(config, &observation, thermal_hz(K230_THERMAL_CPU),
            thermal_powered(K230_THERMAL_KPU) ? thermal_hz(K230_THERMAL_KPU) : 0, rapid_rise);
        if (__atomic_load_n(&sampling_restricted, __ATOMIC_ACQUIRE) && period > 2 * config->tick_hz)
            period = 2 * config->tick_hz;
        if (__atomic_load_n(&sampling_urgent, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&actuation_active, __ATOMIC_ACQUIRE))
            period = config->active_ticks;
        __atomic_store_n(&sample_ticks, period, __ATOMIC_RELEASE);
        spent = rt_tick_get() - begin;
        if (spent < period && !observation.error && observation.valid)
            interval_ret = k230_thermal_reading_set_interval(period);
        /* Publish the expected cadence before the controller sees this sample. */
        if (notify) {
            rt_event_send(&thermal_event, THERMAL_EVENT);
            notify = false;
        }
        if (spent >= period || interval_ret != RT_EOK)
            return;
        /* Clock/configuration changes and actuation start/finish interrupt this
         * wait and recompute the due time. A shorter period that is already
         * overdue starts the next conversion at once. */
        rt_event_recv(&sampler_event, THERMAL_EVENT, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
            period - spent, &events);
    }
}

static void thermal_sampler(void *arg)
{
    struct k230_thermal_config config;
    struct k230_thermal_reading previous = {0};
    bool owned = false;
    bool critical_confirmation_pending = false;
    int32_t critical_first_mc = 0;
    (void)arg;
    k230_pm_thermal_config(&config);
    while (1) {
        struct k230_thermal_reading sample = {0};
        rt_tick_t begin = rt_tick_get();
        int32_t critical;
        bool fresh;
        bool rapid_rise = false;
        int ret = -RT_EBUSY;

        thermal_check_shutdown();
        if (!owned) {
            ret = thermal_sensor_setup();
            owned = ret == RT_EOK;
        }
        /* Reapply the configuration without clearing the outage history. */
        if (owned && __atomic_exchange_n(&reinit_requested, false, __ATOMIC_ACQ_REL))
            ts_hw_configure(TS_HW_DEFAULT_TRIM, false);
        if (owned)
            ret = thermal_sensor_read();
        critical = k230_pm_dvfs_thermal_shutdown_mc();
        fresh = thermal_fresh_sample(&sample);
        if (critical_confirmation_pending && ret == RT_EOK && fresh) {
            critical_confirmation_pending = false;
            if (sample.millicelsius >= critical - config.noise_mc)
                thermal_shutdown(K230_THERMAL_CRITICAL, false);
            thermal_polled_print("thermal: critical sample not confirmed first_mc=%d second_mc=%d error=0\n",
                critical_first_mc, sample.millicelsius);
        }
        if (!critical_confirmation_pending && ret == RT_EOK && fresh &&
            sample.millicelsius >= critical) {
            int confirm_ret;
            bool confirm_fresh;

            /* One conversion cannot power the system off: confirm at once with
             * a second successful conversion. Retain the first observation so
             * it establishes MIN even when the cache is replaced meanwhile. */
            critical_first_mc = sample.millicelsius;
            critical_confirmation_pending = true;
            thermal_retain_critical(&sample);
            confirm_ret = owned ? thermal_sensor_read() : -RT_EBUSY;
            confirm_fresh = confirm_ret == RT_EOK && thermal_fresh_sample(&sample);
            if (confirm_fresh) {
                critical_confirmation_pending = false;
                if (sample.millicelsius >= critical - config.noise_mc)
                    thermal_shutdown(K230_THERMAL_CRITICAL, false);
            }
            thermal_polled_print("thermal: critical sample not confirmed first_mc=%d second_mc=%d error=%d\n",
                critical_first_mc, sample.millicelsius, confirm_ret);
        }
        if (ret == RT_EOK && fresh) {
            uint32_t dt = sample.tick - previous.tick;
            int64_t rise = (int64_t)sample.millicelsius - previous.millicelsius;
            rapid_rise = previous.valid && dt && rise > config.noise_mc &&
                rise * config.tick_hz >= (int64_t)dt * 1000; /* >= 1 C/s */
            previous = sample;
        }
        thermal_wait_sample(&config, &sample, fresh, begin, rapid_rise, ret);
    }
}

/* Only protection ceilings are verified; regulation is best effort. A domain
 * with its power removed cannot violate its programmed clock ceiling. */
static bool thermal_clocks_within_stage(
    const struct k230_thermal_clocks clocks[K230_THERMAL_DOMAINS],
    const bool powered[K230_THERMAL_DOMAINS], enum k230_thermal_stage required)
{
    unsigned int d;
    for (d = 0; d < K230_THERMAL_DOMAINS; ++d)
        if (powered[d] && (!clocks[d].actual_hz ||
            !thermal_within(clocks[d].actual_hz, thermal_stage_cap(required, d))))
            return false;
    return true;
}

static int thermal_verified_stage(
    const struct k230_thermal_clocks clocks[K230_THERMAL_DOMAINS],
    const bool powered[K230_THERMAL_DOMAINS], enum k230_thermal_stage required)
{
    return thermal_clocks_within_stage(clocks, powered, required) ? required : -1;
}

/* Each domain uses its own production DVFS table, highest first. */
static void thermal_dvfs_tables(uint32_t hz[K230_THERMAL_DOMAINS][K230_THERMAL_MAX_STEPS],
    unsigned int counts[K230_THERMAL_DOMAINS])
{
    unsigned int d, i;
    for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
        const struct k230_dvfs_domain_desc *desc = &k230_dvfs_domain_desc[thermal_domains[d].dvfs];
        counts[d] = desc->step_count;
        for (i = 0; i < K230_THERMAL_MAX_STEPS; ++i)
            hz[d][i] = i < desc->step_count ? (uint32_t)desc->steps[i].freq : 0;
    }
}

static void thermal_regulator_setup(struct k230_thermal_regulator *regulator)
{
    uint32_t hz[K230_THERMAL_DOMAINS][K230_THERMAL_MAX_STEPS];
    const uint32_t *steps[K230_THERMAL_DOMAINS] = { hz[0], hz[1], hz[2] };
    static const unsigned int weights[K230_THERMAL_DOMAINS] = {
        K230_PM_THERMAL_WEIGHT_CPU, K230_PM_THERMAL_WEIGHT_KPU, K230_PM_THERMAL_WEIGHT_VPU
    };
    unsigned int counts[K230_THERMAL_DOMAINS];
    thermal_dvfs_tables(hz, counts);
    thermal_log_config("thermal: cut weights cpu=%u kpu=%u vpu=%u\n", weights[0], weights[1], weights[2]);
    if (k230_thermal_regulator_init(regulator, steps, counts, weights))
        rt_kprintf("thermal: DVFS tables unusable; regulation disabled, protection stages remain\n");
    else if (regulator->levels <= 1)
        rt_kprintf("thermal: every cut weight is 0; regulation has nothing to lower\n");
}

static void thermal_stage_caps_setup(void)
{
    uint32_t hz[K230_THERMAL_DOMAINS][K230_THERMAL_MAX_STEPS];
    const uint32_t *steps[K230_THERMAL_DOMAINS] = { hz[0], hz[1], hz[2] };
    unsigned int counts[K230_THERMAL_DOMAINS], d;
    thermal_dvfs_tables(hz, counts);
    if (k230_thermal_stage_caps_init(&thermal_stage_caps, steps, counts))
        rt_kprintf("thermal: DVFS table unusable; fixed fallback stage ceilings\n");
    for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
        if (thermal_stage_cap(K230_THERMAL_MIN, (enum k230_thermal_domain)d) == UINT32_MAX)
            thermal_log_config("thermal: stage ceilings_mhz %s none (not throttled)\n",
                thermal_domains[d].name);
        else
            thermal_log_config("thermal: stage ceilings_mhz %s warm/hot/min=%u/%u/%u\n",
                thermal_domains[d].name,
                thermal_stage_cap(K230_THERMAL_WARM, (enum k230_thermal_domain)d) / 1000000U,
                thermal_stage_cap(K230_THERMAL_HOT, (enum k230_thermal_domain)d) / 1000000U,
                thermal_stage_cap(K230_THERMAL_MIN, (enum k230_thermal_domain)d) / 1000000U);
    }
}

static void thermal_controller(void *arg)
{
    struct k230_thermal_policy policy;
    struct k230_thermal_config config;
    struct k230_thermal_clocks clocks[K230_THERMAL_DOMAINS] = {{0}};
    struct k230_thermal_regulator regulator;
    /* DVFS applied its default profiles without a ceiling before startup. */
    uint32_t applied_caps[K230_THERMAL_DOMAINS] = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    /* A normal boot stays at NORMAL and logs nothing; a hot start logs its stage. */
    enum k230_thermal_stage logged_stage = K230_THERMAL_NORMAL;
    enum k230_thermal_health logged_health = K230_SENSOR_STARTING;
    rt_tick_t fault_log_tick = rt_tick_get();
#ifdef K230_PM_THERMAL_TRACE
    uint32_t traced_sequence = 0;
#endif
    (void)arg;
    k230_pm_thermal_config(&config);
    k230_thermal_policy_init(&policy, rt_tick_get());
    thermal_regulator_setup(&regulator);

    while (1) {
        struct k230_thermal_reading snapshot = {0};
        struct k230_thermal_sample sample = {0};
        struct k230_thermal_action action;
        rt_uint32_t events;
        rt_base_t level;
        int rets[K230_THERMAL_DOMAINS], verified, ret = 0;
        uint32_t caps[K230_THERMAL_DOMAINS], actual[K230_THERMAL_DOMAINS];
        bool powered[K230_THERMAL_DOMAINS];
        bool at_minimum;
        bool retained_observation;
        unsigned int d, i;

        thermal_check_shutdown();
        (void)k230_pm_thermal_reading(&snapshot);
        retained_observation = thermal_take_critical(&snapshot);
        sample.temperature_mc = snapshot.millicelsius;
        sample.tick = snapshot.tick;
        sample.sequence = snapshot.sequence;
        sample.interval_ticks = snapshot.interval_ticks;
        sample.valid = snapshot.valid;
        sample.error = snapshot.error;
        /* Runtime settings can tighten, but never override the compiled ceiling. */
        config.critical_mc = k230_pm_dvfs_thermal_shutdown_mc();
        action = k230_thermal_policy_step(&policy, &config, &sample, rt_tick_get());
        /* A sampler-side NONE request returns instead of terminating. Reflect
         * its latched reason in the controller and retain minimum protection. */
        if (k230_pm_shutdown_reason() && !action.shutdown) {
            k230_thermal_policy_shutdown(&policy,
                (enum k230_thermal_reason)k230_pm_shutdown_reason());
            action.shutdown = true;
            action.block_accelerators = true;
            action.ceiling = K230_THERMAL_MIN;
        }
        if (action.shutdown && !k230_pm_shutdown_reason())
            thermal_shutdown(policy.reason, true);
        if (action.reinitialize_sensor)
            __atomic_store_n(&reinit_requested, true, __ATOMIC_RELEASE);
        if (action.block_accelerators)
            k230_pm_thermal_block_accelerators(true);

        for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
            actual[d] = thermal_hz(d);
            powered[d] = thermal_powered(d);
        }
        k230_thermal_regulator_step(&regulator, &config, &sample, rt_tick_get(),
            powered, actual);
        /* The protection ceiling bounds every cap; regulation only lowers it. */
        for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
            uint32_t stage_cap = thermal_stage_cap(action.ceiling, d);
            uint32_t regulated = k230_thermal_regulator_cap(&regulator, d);
            caps[d] = regulated < stage_cap ? regulated : stage_cap;
        }
        k230_pm_thermal_set_limits(caps[K230_THERMAL_CPU], caps[K230_THERMAL_KPU],
            caps[K230_THERMAL_VPU]);
        __atomic_store_n(&actuation_start, rt_tick_get(), __ATOMIC_RELEASE);
        __atomic_store_n(&actuation_active, true, __ATOMIC_RELEASE);
        rt_event_send(&sampler_event, THERMAL_EVENT);
        for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
            thermal_check_shutdown();
            rets[d] = k230_pm_dvfs_thermal_apply(thermal_domains[d].dvfs, &sample,
                caps[d] > applied_caps[d], &clocks[d]);
        }
        thermal_check_shutdown();
        if (policy.fault) {
            /* Release idle domains, but leave domains that clients still
             * reference powered: their drivers may be accessing registers.
             * NONE also permits a later client to power the domain back up. */
            for (i = 0; i < THERMAL_ARRAY_SIZE(thermal_accelerators); ++i) {
                int off_ret = k230_pm_thermal_off(thermal_accelerators[i], false);
                if (off_ret && !ret)
                    ret = off_ret;
                thermal_check_shutdown();
            }
        }
        __atomic_store_n(&actuation_active, false, __ATOMIC_RELEASE);
        for (d = K230_THERMAL_DOMAINS; d-- > 0;)
            if (rets[d])
                ret = rets[d];
        for (d = 0; d < K230_THERMAL_DOMAINS; ++d)
            powered[d] = thermal_powered(d);
        verified = thermal_verified_stage(clocks, powered, action.ceiling);
        at_minimum = verified >= 0 &&
            thermal_clocks_within_stage(clocks, powered, K230_THERMAL_MIN);
        k230_thermal_policy_applied(&policy, verified, at_minimum, ret);
        for (d = 0; d < K230_THERMAL_DOMAINS; ++d)
            if (!rets[d] && clocks[d].actual_hz && thermal_within(clocks[d].actual_hz, caps[d]))
                applied_caps[d] = caps[d];
        __atomic_store_n(&sampling_urgent,
            policy.health != K230_SENSOR_OK || verified < (int)action.ceiling,
            __ATOMIC_RELEASE);
        {
            bool restricted = policy.stage != K230_THERMAL_NORMAL;
            for (d = 0; d < K230_THERMAL_DOMAINS; ++d)
                restricted = restricted || caps[d] != UINT32_MAX;
            __atomic_store_n(&sampling_restricted, restricted, __ATOMIC_RELEASE);
        }
        rt_event_send(&sampler_event, THERMAL_EVENT);
        if (verified >= (int)action.ceiling)
            k230_pm_thermal_block_accelerators(action.block_accelerators);
        {
            bool health_changed = policy.health != logged_health;
            /* Entering or leaving a sensor fault; starting up is not one. */
            bool show = (health_changed &&
                    (policy.health > K230_SENSOR_OK || logged_health > K230_SENSOR_OK)) ||
                (policy.fault && (rt_tick_t)(rt_tick_get() - fault_log_tick) >=
                    rt_tick_from_millisecond(10000));
#ifndef K230_PM_THERMAL_LOG_ERRORS
            show = show || policy.stage != logged_stage;
#endif
#ifdef K230_PM_THERMAL_LOG_VERBOSE
            show = show || health_changed;
#endif
            if (show) {
                rt_kprintf("thermal: tick=%u stage=%u health=%u temp_mc=%d seq=%u ceiling=%u verified=%d cpu_hz=%u kpu_hz=%u vpu_hz=%u sensor_error=%d actuation_error=%d\n",
                    (unsigned)rt_tick_get(), policy.stage, policy.health, policy.temperature_mc,
                    policy.sequence, action.ceiling, verified, clocks[K230_THERMAL_CPU].actual_hz,
                    clocks[K230_THERMAL_KPU].actual_hz, clocks[K230_THERMAL_VPU].actual_hz,
                    sample.error, ret);
                fault_log_tick = rt_tick_get();
            }
            logged_stage = policy.stage;
            logged_health = policy.health;
        }
#ifdef K230_PM_THERMAL_TRACE
        if (sample.valid && sample.sequence != traced_sequence) {
            traced_sequence = sample.sequence;
            rt_kprintf("thermal: trace tick=%u temp_mc=%d seq=%u stage=%u ceiling=%u verified=%d level=%u/%u cap_cpu_hz=%u cap_kpu_hz=%u cap_vpu_hz=%u cpu_hz=%u kpu_hz=%u vpu_hz=%u\n",
                (unsigned)sample.tick, sample.temperature_mc, sample.sequence,
                policy.stage, action.ceiling, verified, regulator.level,
                regulator.levels ? regulator.levels - 1 : 0,
                caps[K230_THERMAL_CPU], caps[K230_THERMAL_KPU], caps[K230_THERMAL_VPU],
                clocks[K230_THERMAL_CPU].actual_hz, clocks[K230_THERMAL_KPU].actual_hz,
                clocks[K230_THERMAL_VPU].actual_hz);
        }
#endif
        level = rt_spin_lock_irqsave(&status_lock);
        published_policy = policy;
        published_regulator = regulator;
        for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
            published_clocks[d] = clocks[d];
            published_caps[d] = caps[d];
        }
        rt_spin_unlock_irqrestore(&status_lock, level);
        /* Apply the retained critical observation completely, then consume the
         * latest cache state without waiting for another sampler event. */
        if (retained_observation)
            continue;
        /* Healthy operation is sample-driven. A deadline still detects a dead
         * sampler; failed writes and unmet protection retry without waiting
         * for a new conversion. There is no fixed actuation loop. */
        {
            uint32_t age = rt_tick_get() - sample.tick;
            uint32_t budget = k230_thermal_fresh_ticks(&config, sample.interval_ticks);
            uint32_t timeout = budget > age ? budget - age : config.active_ticks;
            if (ret || policy.health != K230_SENSOR_OK || verified < (int)action.ceiling)
                timeout = config.active_ticks;
            rt_event_recv(&thermal_event, THERMAL_EVENT, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                timeout, &events);
        }
    }
}

#if defined(RT_USING_MSH) && defined(K230_PM_CMD)
bool k230_pm_thermal_get_status(struct k230_thermal_status *status)
{
    rt_base_t level;
    unsigned int d;

    if (status == RT_NULL || !__atomic_load_n(&thermal_started, __ATOMIC_ACQUIRE))
        return false;

    level = rt_spin_lock_irqsave(&status_lock);
    status->policy = published_policy;
    status->regulator_enabled = published_regulator.enabled;
    status->regulator_level = published_regulator.level;
    status->regulator_levels = published_regulator.levels;
    status->regulator_decision_mc = published_regulator.decision_mc;
    for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
        status->domains[d].cap_hz = published_caps[d];
        status->domains[d].requested_hz = published_clocks[d].requested_hz;
        status->domains[d].actual_hz = published_clocks[d].actual_hz;
        status->domains[d].error = published_clocks[d].error;
    }
    rt_spin_unlock_irqrestore(&status_lock, level);
    for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
        status->domains[d].name = thermal_domains[d].name;
        status->domains[d].powered = thermal_powered(d);
    }
    status->sample_ticks = __atomic_load_n(&sample_ticks, __ATOMIC_ACQUIRE);
    return true;
}
#endif /* RT_USING_MSH && K230_PM_CMD */

int k230_pm_thermal_init(void)
{
    rt_thread_t sampler, controller;
    struct k230_thermal_config config;
    if (thermal_started)
        return 0;
    /* Readers get "no reading yet" from now on, not "no monitor". */
    k230_thermal_reading_init();
    k230_pm_thermal_config(&config);
    thermal_log_config("thermal: thresholds_mc warm=%d hot=%d min=%d sustained=%d critical=%d\n",
        config.warm_mc, config.hot_mc, config.min_mc, config.sustained_mc,
        k230_pm_dvfs_thermal_shutdown_mc());
    thermal_log_config("thermal: regulation=%s target_mc=%d switch_on_mc=%d control_ticks=%u\n",
        config.regulation ? "step_wise" : "off", config.target_mc,
        config.target_mc - config.hysteresis_mc, config.control_ticks);
    thermal_stage_caps_setup();
    if (!k230_thermal_config_valid(&config)) {
        rt_kprintf("thermal: invalid threshold/timing configuration\n");
        goto failed;
    }
    rt_spin_lock_init(&status_lock);
    rt_spin_lock_init(&critical_lock);
    if (rt_event_init(&thermal_event, "thermal", RT_IPC_FLAG_PRIO) != RT_EOK ||
        rt_event_init(&sampler_event, "ts_wake", RT_IPC_FLAG_PRIO) != RT_EOK)
        goto failed;
    sample_ticks = rt_tick_from_millisecond(250);
    k230_thermal_policy_init(&published_policy, rt_tick_get());
    sampler = rt_thread_create("ts_sample", thermal_sampler, RT_NULL, 4096,
        SAMPLER_PRIORITY, 10);
    controller = rt_thread_create("thermal", thermal_controller, RT_NULL, 6144,
        CONTROLLER_PRIORITY, 10);
    if (!sampler || !controller)
        goto failed;
    __atomic_store_n(&thermal_started, true, __ATOMIC_RELEASE);
    if (rt_thread_startup(sampler) != RT_EOK || rt_thread_startup(controller) != RT_EOK)
        goto failed;
    return 0;
failed:
    thermal_shutdown(K230_THERMAL_INIT_FAILURE, false);
    return K230_ERR_PM_NOTREADY;
}
