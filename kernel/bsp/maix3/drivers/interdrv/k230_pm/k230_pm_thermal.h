/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef K230_PM_THERMAL_H
#define K230_PM_THERMAL_H

#ifdef K230_PM_THERMAL_PROTECT
#include <rtthread.h>
#include "k230_pm_thermal_policy.h"

/* The policy stays free of RT-Thread configuration; board build options are
 * applied here so the thermal controller and DVFS see the same thresholds. */
static inline void k230_pm_thermal_config(struct k230_thermal_config *config)
{
    int32_t step;

    k230_thermal_config_default(config, RT_TICK_PER_SECOND);
#ifndef K230_PM_THERMAL_REGULATION
    config->regulation = false;
#endif
#ifdef K230_PM_THERMAL_CRITICAL_C
#if K230_PM_THERMAL_CRITICAL_C < 50 || K230_PM_THERMAL_CRITICAL_C * 1000 > K230_THERMAL_CRITICAL_MAX_MC
#error "K230_PM_THERMAL_CRITICAL_C must be 50..115"
#endif
    config->critical_mc = K230_PM_THERMAL_CRITICAL_C * 1000;
    config->target_mc = config->critical_mc - 20000;
#endif
#ifdef K230_PM_THERMAL_TARGET_C
#if K230_PM_THERMAL_TARGET_C < 30 || K230_PM_THERMAL_TARGET_C > 95
#error "K230_PM_THERMAL_TARGET_C must be 30..95"
#endif
#if defined(K230_PM_THERMAL_CRITICAL_C) && \
    K230_PM_THERMAL_TARGET_C > K230_PM_THERMAL_CRITICAL_C - 8
#error "K230_PM_THERMAL_TARGET_C must be at least 8 C below K230_PM_THERMAL_CRITICAL_C for 2 C minimum stage spacing"
#endif
    config->target_mc = K230_PM_THERMAL_TARGET_C * 1000;
#endif
    /* Fit four bands between target and critical, retaining fractional C.
     * Cap at the usual 5 C so lowering the regulation target alone does not
     * move protection stages farther from critical. The 8 C minimum gap
     * keeps each band at least 2 C wide, above sensor-code noise. */
    step = (config->critical_mc - config->target_mc) / 4;
    if (step > 5000)
        step = 5000;
    config->min_mc = config->sustained_mc = config->critical_mc - step;
    config->hot_mc = config->critical_mc - 2 * step;
    config->warm_mc = config->critical_mc - 3 * step;
    config->hysteresis_mc = step;
    /* The regulator requires a noise band strictly below half the hysteresis.
     * At the 2 C minimum step use 0.9 C, still above +/- one sensor code. */
    if (2 * config->noise_mc >= step)
        config->noise_mc = step / 2 - 100;
#ifdef K230_PM_THERMAL_SHUTDOWN_ON_SENSOR_FAULT
    config->shutdown_on_sensor_fault = true;
#endif
    /* Lowered thresholds use the same policy. Restart below the regulation
     * target as well as below 70 C, so cooling cannot restart into WARM. */
    if (config->restart_mc > config->target_mc)
        config->restart_mc = config->target_mc;
}

void k230_pm_thermal_notify(void);
void k230_pm_thermal_clock_changed(void);
#if defined(RT_USING_MSH) && defined(K230_PM_CMD)
#include "k230_pm_thermal_regulator.h"

struct k230_thermal_status {
    struct k230_thermal_policy policy;
    struct {
        const char *name;
        uint32_t cap_hz, requested_hz, actual_hz;
        int error;
        bool powered;
    } domains[K230_THERMAL_DOMAINS];
    bool regulator_enabled;
    unsigned int regulator_level, regulator_levels;
    int32_t regulator_decision_mc;
    uint32_t sample_ticks;
};

/* Copy published controller state under its lock. Power state and sampling
 * cadence are read separately. False before startup or for a null output. */
bool k230_pm_thermal_get_status(struct k230_thermal_status *status);
#endif

/* Default sensor cadence plus conversion/scheduling allowance. Each reading
 * carries its actual cadence, so slower polling cannot pass off old data. */
#define K230_THERMAL_READING_NORMAL_MS 5000U
#define K230_THERMAL_READING_FRESH_MS  6500U

/* The thermal monitor owns the temperature sensor. */
struct k230_thermal_reading {
    int32_t millicelsius;
    rt_tick_t tick;             /* acquisition time */
    uint32_t sequence;          /* successful acquisitions */
    rt_tick_t interval_ticks;   /* expected time to the next one; 0: default */
    int error;                  /* last acquisition; earlier data stays */
    bool valid;
    bool stale;                 /* computed when read */
};

/* Latest reading, also served by /dev/ts. -RT_ENOSYS before the monitor
 * starts. */
int k230_pm_thermal_reading(struct k230_thermal_reading *reading);

/* Monitor side, k230_pm_thermal_reading.c. */
void k230_thermal_reading_init(void);
void k230_thermal_reading_publish(int error, int32_t millicelsius);
void k230_thermal_reading_invalidate(void);
int k230_thermal_reading_set_interval(rt_tick_t interval);
#else
static inline void k230_pm_thermal_notify(void) {}
static inline void k230_pm_thermal_clock_changed(void) {}
#endif

#endif
