/* SPDX-License-Identifier: BSD-2-Clause */
#include "k230_pm_thermal_regulator.h"
#include <string.h>

/* Control-model reference (not a source-code import): Linux
 * drivers/thermal/gov_step_wise.c. Stage ceilings if a DVFS table is unusable: */
static const uint32_t fallback_stage_caps[K230_THERMAL_DOMAINS][K230_THERMAL_MIN + 1] = {
    { UINT32_MAX, 800000000U, 400000000U, 200000000U },
    { UINT32_MAX, 400000000U, 200000000U, 100000000U },
    { UINT32_MAX, 600000000U, 400000000U, 200000000U },
};

/* Same PLL rounding allowance as the DVFS readback check. */
#define REGULATOR_FREQ_TOLERANCE_HZ 1000U

static bool elapsed(uint32_t now, uint32_t then, uint32_t interval)
{
    return (uint32_t)(now - then) >= interval;
}

int k230_thermal_regulator_init(struct k230_thermal_regulator *r,
    const uint32_t *const steps[K230_THERMAL_DOMAINS],
    const unsigned int counts[K230_THERMAL_DOMAINS],
    const unsigned int weights[K230_THERMAL_DOMAINS])
{
    unsigned int d, i, l, taken[K230_THERMAL_DOMAINS] = {0};
    memset(r, 0, sizeof(*r));
    for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
        if (!steps[d] || !counts[d] || counts[d] > K230_THERMAL_MAX_STEPS ||
            weights[d] > K230_THERMAL_MAX_WEIGHT)
            goto invalid;
        for (i = 0; i < counts[d]; ++i) {
            if (!steps[d][i] || (i && steps[d][i] >= steps[d][i - 1]))
                goto invalid;
            r->steps[d][i] = steps[d][i];
        }
        r->step_count[d] = counts[d];
    }
    /* Merge the domains' profile drops in order of their weighted fraction
     * of each range, (taken + 1) / ((count - 1) * weight). Ties go to the
     * heavier domain, then to the lower domain index. */
    for (l = 1;; ++l) {
        int best = -1;
        for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
            uint64_t mine, theirs;
            /* Weight 0: protection stages only, never regulated. */
            if (!weights[d] || taken[d] + 1 >= counts[d])
                continue;
            if (best < 0) {
                best = (int)d;
                continue;
            }
            mine = (uint64_t)(taken[d] + 1) * (counts[best] - 1) * weights[best];
            theirs = (uint64_t)(taken[best] + 1) * (counts[d] - 1) * weights[d];
            if (mine < theirs || (mine == theirs && weights[d] > weights[best]))
                best = (int)d;
        }
        if (best < 0)
            break;
        memcpy(r->index[l], r->index[l - 1], sizeof(r->index[l]));
        r->index[l][best] = (uint8_t)++taken[best];
        r->lowered[l] = (uint8_t)best;
    }
    r->levels = l;
    r->enabled = true;
    return 0;
invalid:
    memset(r, 0, sizeof(*r));
    return -1;
}

/* Lower to the first level that reduces a running clock below its current
 * read-back rate. Levels already satisfied by a protection ceiling or by a
 * slower governor request are passed over rather than accumulated. */
static void regulator_lower(struct k230_thermal_regulator *r,
    const bool powered[K230_THERMAL_DOMAINS], const uint32_t actual[K230_THERMAL_DOMAINS])
{
    unsigned int l;
    for (l = r->level + 1; l < r->levels; ++l) {
        unsigned int d = r->lowered[l];
        if (powered[d] && (uint64_t)r->steps[d][r->index[l][d]] +
            REGULATOR_FREQ_TOLERANCE_HZ < actual[d]) {
            r->level = l;
            return;
        }
    }
}

/* Raise one level that lifts a powered domain's ceiling. */
static void regulator_raise(struct k230_thermal_regulator *r,
    const bool powered[K230_THERMAL_DOMAINS])
{
    while (r->level > 0)
        if (powered[r->lowered[r->level--]])
            return;
}

void k230_thermal_regulator_step(struct k230_thermal_regulator *r,
    const struct k230_thermal_config *c, const struct k230_thermal_sample *s,
    uint32_t now, const bool powered[K230_THERMAL_DOMAINS],
    const uint32_t actual[K230_THERMAL_DOMAINS])
{
    int32_t temperature, trend;
    if (!r->enabled)
        return;
    if (!c->regulation) {
        r->level = 0;
        return;
    }
    /* Failed, stale and repeated samples hold the level: missing data can
     * neither prove cooling nor justify further throttling. */
    if (!s || !s->valid || s->error ||
        elapsed(now, s->tick, k230_thermal_fresh_ticks(c, s->interval_ticks))) {
        r->below = false;  /* a gap cannot prove the die stayed cool */
        return;
    }
    if (r->seen && s->sequence == r->sequence)
        return;
    r->seen = true;
    r->sequence = s->sequence;
    /* One decision per control period, allowing a quarter period of jitter
     * when the sampler runs at the same cadence. */
    if (r->decided && !elapsed(s->tick, r->decision_tick,
        c->control_ticks - c->control_ticks / 4))
        return;
    temperature = s->temperature_mc;
    trend = r->decided ? temperature - r->decision_mc : 0;
    r->decided = true;
    r->decision_tick = s->tick;
    r->decision_mc = temperature;
    /* Above target, throttle unless already cooling beyond sensor noise.
     * Release only after readings have stayed below the band (and, inside
     * the switch-on margin, not heating) for raise_hold_ticks: on the EVB one
     * CPU cut (1188 -> 800 MHz) cooled the die 2.5 C within a period, and
     * raising on that reading toggled the clock every 2-10 s. A larger step
     * can cool past switch-on, so the hold applies there too. Once it has
     * passed below switch-on, recovery continues every period; inside the
     * band each further step waits again. */
    bool off = temperature < c->target_mc - c->hysteresis_mc;
    if (temperature > c->target_mc && trend > -c->noise_mc) {
        r->below = false;
        regulator_lower(r, powered, actual);
    } else if (off || (temperature < c->target_mc - 2 * c->noise_mc && trend < c->noise_mc)) {
        if (!r->below) {
            r->below = true;
            r->below_since = s->tick;
        } else if (elapsed(s->tick, r->below_since, c->raise_hold_ticks)) {
            regulator_raise(r, powered);
            if (!off)
                r->below_since = s->tick;
        }
    } else {
        r->below = false;
    }
}

uint32_t k230_thermal_regulator_cap(const struct k230_thermal_regulator *r,
    enum k230_thermal_domain d)
{
    unsigned int i = r->enabled ? r->index[r->level][d] : 0;
    return i ? r->steps[d][i] : UINT32_MAX;
}

int k230_thermal_stage_caps_init(struct k230_thermal_stage_caps *caps,
    const uint32_t *const steps[K230_THERMAL_DOMAINS],
    const unsigned int counts[K230_THERMAL_DOMAINS])
{
    unsigned int d, i, last;
    int ret = 0;
    for (d = 0; d < K230_THERMAL_DOMAINS; ++d) {
        bool valid = steps[d] && counts[d] && counts[d] <= K230_THERMAL_MAX_STEPS &&
            steps[d][0];
        for (i = 1; valid && i < counts[d]; ++i)
            valid = steps[d][i] && steps[d][i] < steps[d][i - 1];
        if (!valid) {
            memcpy(caps->hz[d], fallback_stage_caps[d], sizeof(caps->hz[d]));
            ret = -1;
            continue;
        }
        /* A short table reuses its minimum for the stricter stages. */
        last = counts[d] - 1;
        caps->hz[d][K230_THERMAL_NORMAL] = UINT32_MAX;
        caps->hz[d][K230_THERMAL_WARM] = steps[d][last < 1 ? last : 1];
        caps->hz[d][K230_THERMAL_HOT] = steps[d][last < 2 ? last : 2];
        caps->hz[d][K230_THERMAL_MIN] = steps[d][last];
    }
    return ret;
}

uint32_t k230_thermal_stage_cap(const struct k230_thermal_stage_caps *caps,
    enum k230_thermal_stage stage, enum k230_thermal_domain d)
{
    return caps->hz[d][stage > K230_THERMAL_MIN ? K230_THERMAL_MIN : stage];
}
