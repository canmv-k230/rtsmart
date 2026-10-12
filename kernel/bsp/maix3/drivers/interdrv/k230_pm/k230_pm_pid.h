/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __K230_PM_PID_H__
#define __K230_PM_PID_H__

#include <stdint.h>

struct k230_pm_pid_config {
    int32_t target;
    int32_t deadband;
    int32_t kp;
    int32_t ki;
    int32_t kd;
    int32_t scale;
    int32_t integral_limit;
};

struct k230_pm_pid_state {
    int32_t integral;
    int32_t last_error;
};

void k230_pm_pid_reset(struct k230_pm_pid_state *state);
int32_t k230_pm_pid_calc(const struct k230_pm_pid_config *config,
    struct k230_pm_pid_state *state, int32_t input);

#endif
