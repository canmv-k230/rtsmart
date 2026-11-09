/* SPDX-License-Identifier: BSD-2-Clause */
#include "k230_pm_thermal_policy.h"
#include <string.h>

static bool elapsed(uint32_t now, uint32_t then, uint32_t interval)
{
    return (uint32_t)(now - then) >= interval;
}

void k230_thermal_config_default(struct k230_thermal_config *c, uint32_t hz)
{
    *c = (struct k230_thermal_config) {
        .warm_mc = 90000, .hot_mc = 95000, .min_mc = 100000, .critical_mc = 105000,
        /* One sensor code is ~300 m°C; the band must exceed ±1 code of noise. */
        .hysteresis_mc = 5000, .noise_mc = 1000,
        /* Throttling at WARM is not itself a reason to shut down. */
        .sustained_mc = 100000, .tick_hz = hz,
        /* Regulate one hysteresis band below WARM, so ordinary heavy load is
         * held by small profile steps before the first fixed ceiling. */
        .regulation = true, .target_mc = 85000, .control_ticks = 2 * hz,
        .raise_hold_ticks = 10 * hz,
        .restart_mc = 70000, .restart_hold_ticks = 60 * hz,
        .recovery_ticks = 5 * hz, .sustained_ticks = 30 * hz,
        .sustained_limit_ticks = 120 * hz,
        .fresh_ticks = (13 * hz + 1) / 2, .outage_ticks = 15 * hz,
        .enforcement_ticks = 2 * hz, .normal_ticks = 5 * hz, .idle_ticks = 10 * hz,
        .active_ticks = (hz + 3) / 4, .shutdown_on_sensor_fault = false
    };
}

bool k230_thermal_config_valid(const struct k230_thermal_config *c)
{
    return c && c->tick_hz > 0 &&
        c->warm_mc >= -40000 && c->warm_mc < c->hot_mc &&
        c->sustained_mc >= c->min_mc && c->sustained_mc < c->critical_mc &&
        c->restart_mc < c->warm_mc && c->restart_hold_ticks > 0 &&
        c->restart_hold_ticks < INT32_MAX &&
        c->hot_mc < c->min_mc && c->min_mc < c->critical_mc &&
        c->critical_mc <= K230_THERMAL_CRITICAL_MAX_MC && c->hysteresis_mc > 0 &&
        c->hysteresis_mc <= c->hot_mc - c->warm_mc &&
        c->hysteresis_mc <= c->min_mc - c->hot_mc &&
        c->noise_mc > 0 && c->noise_mc < c->min_mc - c->warm_mc &&
        c->active_ticks > 0 && c->active_ticks <= c->normal_ticks &&
        c->normal_ticks <= c->idle_ticks && c->idle_ticks < c->outage_ticks &&
        c->normal_ticks < c->fresh_ticks && c->fresh_ticks < c->outage_ticks &&
        c->outage_ticks < INT32_MAX && c->recovery_ticks > 0 &&
        c->recovery_ticks < INT32_MAX && c->sustained_ticks > 0 &&
        c->control_ticks > 0 && c->control_ticks < INT32_MAX &&
        c->raise_hold_ticks < INT32_MAX &&
        (!c->regulation || (c->target_mc - c->hysteresis_mc >= -40000 &&
            c->target_mc <= c->warm_mc - c->hysteresis_mc &&
            2 * c->noise_mc < c->hysteresis_mc)) &&
        c->sustained_limit_ticks >= c->sustained_ticks &&
        c->sustained_limit_ticks < INT32_MAX && c->enforcement_ticks > 0 &&
        c->enforcement_ticks < INT32_MAX;
}

uint32_t k230_thermal_fresh_ticks(const struct k230_thermal_config *c, uint32_t interval)
{
    if (!interval) interval = c->normal_ticks;
    /* Conversion/scheduling allowance follows the selected cadence. */
    return interval + c->fresh_ticks - c->normal_ticks;
}

uint32_t k230_thermal_outage_ticks(const struct k230_thermal_config *c, uint32_t interval)
{
    if (!interval) interval = c->normal_ticks;
    return interval + c->outage_ticks - c->normal_ticks;
}

uint32_t k230_thermal_poll_ticks(const struct k230_thermal_config *c,
    const struct k230_thermal_sample *s, uint32_t cpu_hz, uint32_t kpu_hz,
    bool rapid_rise)
{
    if (!s || !s->valid || s->error || s->temperature_mc >= c->min_mc)
        return c->active_ticks;
    if (rapid_rise || s->temperature_mc >= c->hot_mc)
        return c->tick_hz;
    /* The regulator needs one fresh observation per control period. */
    if (s->temperature_mc >= (c->regulation ? c->target_mc : c->warm_mc) - c->hysteresis_mc)
        return c->control_ticks < 2 * c->tick_hz ? c->control_ticks : 2 * c->tick_hz;
    return !cpu_hz || cpu_hz > 800000000U || kpu_hz > 400000000U ?
        c->normal_ticks : c->idle_ticks;
}

void k230_thermal_policy_init(struct k230_thermal_policy *p, uint32_t now)
{
    memset(p, 0, sizeof(*p));
    p->health = K230_SENSOR_STARTING;
    p->required = K230_THERMAL_MIN;
    p->applied = -1;
    p->start_tick = now;
}

void k230_thermal_policy_shutdown(struct k230_thermal_policy *p,
    enum k230_thermal_reason reason)
{
    if (p->reason == K230_THERMAL_NONE)
        p->reason = reason;
    p->stage = K230_THERMAL_SHUTDOWN;
    p->required = K230_THERMAL_MIN;
}

void k230_thermal_policy_applied(struct k230_thermal_policy *p,
    int verified, bool at_minimum, int error)
{
    /* Read-back state, not the transition result, establishes protection. */
    p->last_actuation_error = error;
    p->at_minimum = at_minimum;
    if (verified < K230_THERMAL_NORMAL)
        p->applied = -1;
    else
        p->applied = verified > K230_THERMAL_MIN ? K230_THERMAL_MIN : verified;
    /* Close the old deadline immediately. The next observation may arrive
     * several seconds later and require a different, stricter ceiling. */
    if (p->applied >= (int)p->required)
        p->enforcing = false;
}

enum k230_thermal_stage k230_thermal_temperature_stage(
    const struct k230_thermal_config *c, int32_t temperature)
{
    if (temperature >= c->min_mc) return K230_THERMAL_MIN;
    if (temperature >= c->hot_mc) return K230_THERMAL_HOT;
    if (temperature >= c->warm_mc) return K230_THERMAL_WARM;
    return K230_THERMAL_NORMAL;
}

static int32_t recovery_threshold(const struct k230_thermal_config *c,
    enum k230_thermal_stage stage)
{
    if (stage == K230_THERMAL_MIN) return c->min_mc - c->hysteresis_mc;
    if (stage == K230_THERMAL_HOT) return c->hot_mc - c->hysteresis_mc;
    return c->warm_mc - c->hysteresis_mc;
}

struct k230_thermal_action k230_thermal_policy_step(struct k230_thermal_policy *p,
    const struct k230_thermal_config *c, const struct k230_thermal_sample *s,
    uint32_t now)
{
    struct k230_thermal_action a = {0};
    bool fresh, observation, gap;
    uint32_t last, fresh_ticks, gap_ticks, outage_ticks;

    if (!k230_thermal_config_valid(c)) {
        k230_thermal_policy_shutdown(p, K230_THERMAL_INIT_FAILURE);
        a.ceiling = K230_THERMAL_MIN;
        a.block_accelerators = a.shutdown = true;
        return a;
    }
    if (p->stage == K230_THERMAL_SHUTDOWN)
        goto result;

    fresh_ticks = k230_thermal_fresh_ticks(c, s ? s->interval_ticks : 0);
    outage_ticks = k230_thermal_outage_ticks(c, s ? s->interval_ticks : 0);
    gap_ticks = k230_thermal_fresh_ticks(c, p->last_interval_ticks);
    if (gap_ticks < fresh_ticks) gap_ticks = fresh_ticks;
    fresh = s && s->valid && !elapsed(now, s->tick, fresh_ticks);
    observation = fresh && (!p->seen_sample || s->sequence != p->sequence);
    gap = observation && p->seen_sample &&
        elapsed(s->tick, p->last_tick, gap_ticks);

    /* One conversion cannot power the system off. A second fresh observation
     * within the noise band of the limit confirms it; a lower one clears it.
     * Meanwhile the reading already requires minimum clocks. A configuration
     * tightening marks the latest fresh sample and the next observation decides. */
    if (fresh && !s->error && (s->temperature_mc >= c->critical_mc || (p->critical_pending &&
        s->temperature_mc >= c->critical_mc - c->noise_mc))) {
        if (p->critical_pending && s->sequence != p->critical_sequence) {
            p->temperature_mc = s->temperature_mc;
            p->last_tick = s->tick;
            k230_thermal_policy_shutdown(p, K230_THERMAL_CRITICAL);
            goto result;
        }
        if (!p->critical_pending) {
            p->critical_pending = true;
            p->critical_sequence = s->sequence;
        }
    } else if (observation && !s->error) {
        p->critical_pending = false;
    }

    /* A late observation is current data and imposes no restriction; a stale
     * period seen in real time does (below). It cannot prove continuous
     * cooling or continuous heat across the gap, so those windows restart. */
    if (gap)
        p->recovering = p->sensor_recovering = p->sustaining = false;
    if (observation) {
        p->seen_sample = true;
        p->last_tick = s->tick;
        p->sequence = s->sequence;
        p->temperature_mc = s->temperature_mc;
        p->reinit_attempted = false;
    }
    if (s && s->valid && s->sequence == p->sequence)
        p->last_interval_ticks = s->interval_ticks;
    last = p->seen_sample ? p->last_tick : p->start_tick;

    if (!fresh) {
        p->recovering = p->sensor_recovering = p->sustaining = false;
        if (elapsed(now, last, fresh_ticks)) {
            p->sensor_restricted = true;
            p->health = K230_SENSOR_STALE;
            if (!p->reinit_attempted) {
                a.reinitialize_sensor = true;
                p->reinit_attempted = true;
            }
        }
        if (elapsed(now, last, outage_ticks)) {
            p->fault = true;
            p->health = K230_SENSOR_FAULT;
            if (c->shutdown_on_sensor_fault) {
                k230_thermal_policy_shutdown(p, K230_THERMAL_SENSOR_OUTAGE);
                goto result;
            }
        }
    } else if (s->error) {
        p->recovering = p->sensor_recovering = p->sustaining = false;
        p->health = p->fault ? K230_SENSOR_FAULT : K230_SENSOR_TRANSIENT;
    } else if (observation) {
        if (p->sensor_restricted) {
            p->health = K230_SENSOR_RECOVERING;
            if (!p->sensor_recovering) {
                p->sensor_recovering = true;
                p->sensor_recovery_since = s->tick;
            } else if (elapsed(s->tick, p->sensor_recovery_since, c->recovery_ticks)) {
                p->sensor_restricted = p->fault = p->sensor_recovering = false;
                p->health = K230_SENSOR_OK;
            }
        } else {
            p->health = K230_SENSOR_OK;
        }
    }

    if (fresh) {
        enum k230_thermal_stage target = k230_thermal_temperature_stage(c, s->temperature_mc);
        if (target > p->stage) {
            p->stage = target;
            p->recovering = false;
        } else if (observation && !s->error && !p->sensor_restricted &&
            p->stage > K230_THERMAL_NORMAL &&
            s->temperature_mc < recovery_threshold(c, p->stage)) {
            if (!p->recovering) {
                p->recovering = true;
                p->recovery_since = s->tick;
            } else if (elapsed(s->tick, p->recovery_since, c->recovery_ticks)) {
                p->stage--;
                p->recovering = false;
            }
        } else if (observation) {
            p->recovering = false;
        }
    }

    if (observation && !s->error) {
        /* An acquisition failure cannot prove cooling. Keep the overall heat
         * budget separate from the uninterrupted, fresh-observation window;
         * hysteresis also prevents noise near MIN from clearing the budget. */
        if (s->temperature_mc < c->sustained_mc - c->hysteresis_mc)
            p->overheating = false;
        if (s->temperature_mc < c->sustained_mc || !p->at_minimum) {
            p->sustaining = false;
        } else if (!p->sustaining) {
            p->sustaining = true;
            p->sustained_since = s->tick;
            p->cooling_reference_mc = s->temperature_mc;
        } else {
            /* Net cooling beyond the noise band restarts the window. The
             * reference only moves down, so noise around a steady or rising
             * temperature cannot restart it, and the total-time limit bounds
             * how long slow cooling may extend it. */
            if (s->temperature_mc < p->cooling_reference_mc - c->noise_mc) {
                p->sustained_since = s->tick;
                p->cooling_reference_mc = s->temperature_mc;
            }
            if (elapsed(s->tick, p->sustained_since, c->sustained_ticks))
                k230_thermal_policy_shutdown(p, K230_THERMAL_SUSTAINED);
        }
        if (p->sustaining) {
            if (!p->overheating) {
                p->overheating = true;
                p->overheat_since = s->tick;
            } else if (elapsed(s->tick, p->overheat_since, c->sustained_limit_ticks)) {
                k230_thermal_policy_shutdown(p, K230_THERMAL_SUSTAINED);
            }
        }
    }

result:
    a.shutdown = p->stage == K230_THERMAL_SHUTDOWN;
    /* Before the first reading the boot clocks stand; a sensor that never
     * reports becomes stale, then a fault, measured from start_tick. */
    a.block_accelerators = a.shutdown || p->fault;
    a.ceiling = (a.shutdown || p->sensor_restricted || p->critical_pending) ?
        K230_THERMAL_MIN : p->stage;
    p->required = a.ceiling;
    /* NORMAL imposes no ceiling, so there is nothing to enforce: a failed
     * write or unreadable regulator on a cool chip is not a thermal fault. */
    if (!a.shutdown && a.ceiling > K230_THERMAL_NORMAL && p->applied < (int)a.ceiling) {
        if (!p->enforcing) {
            p->enforcing = true;
            p->enforcement_since = now;
        } else if (elapsed(now, p->enforcement_since, c->enforcement_ticks)) {
            k230_thermal_policy_shutdown(p, K230_THERMAL_ENFORCEMENT);
            a.shutdown = a.block_accelerators = true;
            a.ceiling = K230_THERMAL_MIN;
        }
    } else {
        p->enforcing = false;
    }
    return a;
}
