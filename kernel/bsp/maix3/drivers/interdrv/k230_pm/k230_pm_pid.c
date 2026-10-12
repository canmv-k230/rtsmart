/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "k230_pm_pid.h"

static int32_t k230_pm_pid_clamp(int32_t val, int32_t min, int32_t max)
{
    if (val < min)
        return min;
    if (val > max)
        return max;

    return val;
}

void k230_pm_pid_reset(struct k230_pm_pid_state *state)
{
    if (!state)
        return;

    state->integral = 0;
    state->last_error = 0;
}

int32_t k230_pm_pid_calc(const struct k230_pm_pid_config *config,
    struct k230_pm_pid_state *state, int32_t input)
{
    int32_t error;
    int32_t derivative;
    int32_t scale;

    if (!config || !state)
        return 0;

    scale = config->scale > 0 ? config->scale : 1;
    error = input - config->target;
    if (error <= config->deadband && error >= -config->deadband) {
        error = 0;
        state->integral = 0;
    } else {
        state->integral = k230_pm_pid_clamp(state->integral + error,
            -config->integral_limit, config->integral_limit);
    }

    derivative = error - state->last_error;
    state->last_error = error;

    return (config->kp * error + config->ki * state->integral +
        config->kd * derivative) / scale;
}
