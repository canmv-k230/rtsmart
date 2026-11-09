/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef K230_PM_THERMAL_POLICY_H
#define K230_PM_THERMAL_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum k230_thermal_stage {
    K230_THERMAL_NORMAL, K230_THERMAL_WARM, K230_THERMAL_HOT,
    K230_THERMAL_MIN, K230_THERMAL_SHUTDOWN
};

enum k230_thermal_health {
    K230_SENSOR_STARTING, K230_SENSOR_OK, K230_SENSOR_TRANSIENT,
    K230_SENSOR_STALE, K230_SENSOR_FAULT, K230_SENSOR_RECOVERING
};

enum k230_thermal_reason {
    K230_THERMAL_NONE, K230_THERMAL_CRITICAL, K230_THERMAL_SUSTAINED,
    K230_THERMAL_ENFORCEMENT, K230_THERMAL_SENSOR_OUTAGE,
    K230_THERMAL_INIT_FAILURE
};

/* Highest selectable critical limit: the sensor is specified to 125 C with
 * +/-3 C untrimmed accuracy; keep a margin below the top of that range. */
#define K230_THERMAL_CRITICAL_MAX_MC 115000

/* Times are unsigned monotonic ticks; all intervals must be < 2^31 ticks. */
struct k230_thermal_config {
    /* Protection stages: each entry applies its fixed ceiling at once. */
    int32_t warm_mc, hot_mc, min_mc, critical_mc;
    int32_t hysteresis_mc, noise_mc;
    int32_t sustained_mc; /* Shutdown hold threshold after both clocks reach minimum. */
    /* Best-effort step-wise regulation toward target_mc, below the stages.
     * It starts at target_mc - hysteresis_mc and decides once per control_ticks. */
    bool regulation;
    int32_t target_mc;
    uint32_t control_ticks;
    /* Inside the band, a raise needs readings below it for this long; one
     * cut can cool the die past the band within a control period. */
    uint32_t raise_hold_ticks;
    uint32_t tick_hz;
    /* Halt fallback restarts after readings stay below restart_mc for
     * restart_hold_ticks (K230_PM_THERMAL_COOLDOWN_RESTART). */
    int32_t restart_mc;
    uint32_t restart_hold_ticks;
    /* sustained_ticks requires fresh heat and restarts on net cooling. The
     * overall limit survives acquisition gaps until a fresh reading below
     * sustained_mc - hysteresis_mc ends the overheating episode. */
    uint32_t recovery_ticks, sustained_ticks, sustained_limit_ticks;
    uint32_t fresh_ticks, outage_ticks;
    uint32_t enforcement_ticks, normal_ticks, idle_ticks, active_ticks;
    bool shutdown_on_sensor_fault;
};

struct k230_thermal_sample {
    int32_t temperature_mc;
    uint32_t tick, sequence;
    /* Expected interval after this acquisition; zero uses normal_ticks. */
    uint32_t interval_ticks;
    bool valid;
    int error;
};

struct k230_thermal_policy {
    enum k230_thermal_stage stage;
    enum k230_thermal_health health;
    enum k230_thermal_reason reason;
    enum k230_thermal_stage required;
    int applied; /* Stage whose protection ceilings have been verified, or -1. */
    bool at_minimum; /* Every powered thermal domain is at its minimum profile. */
    uint32_t start_tick, last_tick, sequence, last_interval_ticks;
    int32_t temperature_mc;
    bool seen_sample, sensor_restricted, fault, reinit_attempted;
    bool recovering, sensor_recovering, sustaining, overheating, enforcing, critical_pending;
    uint32_t critical_sequence;
    uint32_t recovery_since, sensor_recovery_since, sustained_since, enforcement_since;
    uint32_t overheat_since;
    int32_t cooling_reference_mc;
    int last_actuation_error;
};

struct k230_thermal_action {
    enum k230_thermal_stage ceiling;
    bool block_accelerators;
    bool reinitialize_sensor;
    bool shutdown;
};

/* Temperature takes priority over clocks: throttling must never lengthen a
 * hot chip's monitoring interval. kpu_hz is zero while its domain is off. */
uint32_t k230_thermal_poll_ticks(const struct k230_thermal_config *config,
    const struct k230_thermal_sample *sample, uint32_t cpu_hz, uint32_t kpu_hz,
    bool rapid_rise);
uint32_t k230_thermal_fresh_ticks(const struct k230_thermal_config *config,
    uint32_t interval_ticks);
uint32_t k230_thermal_outage_ticks(const struct k230_thermal_config *config,
    uint32_t interval_ticks);
/* Stage a temperature requires on entry, without recovery hysteresis. */
enum k230_thermal_stage k230_thermal_temperature_stage(
    const struct k230_thermal_config *config, int32_t temperature_mc);

void k230_thermal_config_default(struct k230_thermal_config *config, uint32_t tick_hz);
bool k230_thermal_config_valid(const struct k230_thermal_config *config);
void k230_thermal_policy_init(struct k230_thermal_policy *policy, uint32_t now);
struct k230_thermal_action k230_thermal_policy_step(struct k230_thermal_policy *policy,
    const struct k230_thermal_config *config, const struct k230_thermal_sample *sample,
    uint32_t now);
/* verified: stage whose protection ceilings the read-back clocks satisfy, or
 * -1 if enforcement failed. Regulation caps are never verified. at_minimum is
 * independent of the temperature stage. error is diagnostic only. */
void k230_thermal_policy_applied(struct k230_thermal_policy *policy,
    int verified, bool at_minimum, int error);
void k230_thermal_policy_shutdown(struct k230_thermal_policy *policy,
    enum k230_thermal_reason reason);

#endif
