/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef K230_PM_THERMAL_REGULATOR_H
#define K230_PM_THERMAL_REGULATOR_H

#include "k230_pm_thermal_policy.h"

enum k230_thermal_domain {
    K230_THERMAL_CPU, K230_THERMAL_KPU, K230_THERMAL_VPU, K230_THERMAL_DOMAINS
};
#define K230_THERMAL_MAX_WEIGHT 8
#define K230_THERMAL_MAX_STEPS 16
#define K230_THERMAL_MAX_LEVELS (K230_THERMAL_DOMAINS * (K230_THERMAL_MAX_STEPS - 1) + 1)

/* Best-effort regulation below the protection stages, modelled on Linux's
 * step_wise governor. One ladder interleaves the domains' DVFS profiles by
 * their fraction of each profile range, scaled by a per-domain weight: level
 * 0 is unrestricted and every further level lowers one domain by one profile.
 * A heavier domain covers its range sooner, so it is cut first. Safety never depends on it:
 * protection ceilings, their verification and shutdown are independent. */
struct k230_thermal_regulator {
    uint32_t steps[K230_THERMAL_DOMAINS][K230_THERMAL_MAX_STEPS];
    unsigned int step_count[K230_THERMAL_DOMAINS];
    /* Profile index of each domain at each level; domain lowered entering it. */
    uint8_t index[K230_THERMAL_MAX_LEVELS][K230_THERMAL_DOMAINS];
    uint8_t lowered[K230_THERMAL_MAX_LEVELS];
    unsigned int levels, level;
    uint32_t sequence, decision_tick, below_since;
    int32_t decision_mc;
    bool enabled, seen, decided, below;
};

/* weights: 0..K230_THERMAL_MAX_WEIGHT per domain; equal weights interleave
 * by fraction of each range. Weight 0 leaves the domain out of the ladder,
 * so only the protection stages cap it. */
int k230_thermal_regulator_init(struct k230_thermal_regulator *regulator,
    const uint32_t *const steps[K230_THERMAL_DOMAINS],
    const unsigned int counts[K230_THERMAL_DOMAINS],
    const unsigned int weights[K230_THERMAL_DOMAINS]);
/* Decides at most once per control period, on a new fresh error-free sample;
 * missing data holds the level. Inside the band a raise also waits for
 * raise_hold_ticks of readings below it, so one cut that briefly cools the die
 * cannot be undone two seconds later; below switch-on it raises every period.
 * A step down skips levels that would not lower a running clock, so it cannot
 * wind up behind a protection ceiling or an idle governor. Both directions
 * skip levels that only move a powered-off domain. */
void k230_thermal_regulator_step(struct k230_thermal_regulator *regulator,
    const struct k230_thermal_config *config, const struct k230_thermal_sample *sample,
    uint32_t now, const bool powered[K230_THERMAL_DOMAINS],
    const uint32_t actual_hz[K230_THERMAL_DOMAINS]);
uint32_t k230_thermal_regulator_cap(const struct k230_thermal_regulator *regulator,
    enum k230_thermal_domain domain);

/* Protection ceilings derived from the same DVFS tables: WARM one profile
 * below maximum, HOT two below, MIN the minimum profile. */
struct k230_thermal_stage_caps {
    uint32_t hz[K230_THERMAL_DOMAINS][K230_THERMAL_MIN + 1];
};
/* Returns -1 and uses fixed fallback ceilings for any unusable table. */
int k230_thermal_stage_caps_init(struct k230_thermal_stage_caps *caps,
    const uint32_t *const steps[K230_THERMAL_DOMAINS],
    const unsigned int counts[K230_THERMAL_DOMAINS]);
uint32_t k230_thermal_stage_cap(const struct k230_thermal_stage_caps *caps,
    enum k230_thermal_stage stage, enum k230_thermal_domain domain);

#endif
