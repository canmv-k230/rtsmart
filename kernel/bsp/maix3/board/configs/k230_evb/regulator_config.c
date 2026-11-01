/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "regulator.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Limits span the DVFS voltage tables; widen them only with silicon data.
 * The 1 ms settle matches the allowance DVFS used before raising a clock. */
static const struct regulator_config board_regulators[] = {
    {
        .name = "regulator_cpu",
        .driver = &regulator_tps6286x_driver,
        .bus = "i2c4",
        .addr = 0x49,
        .min_uv = 620000,
        .max_uv = 800000,
        .settle_us = 1000,
        .always_on = true,
    },
    {
        .name = "regulator_kpu",
        .driver = &regulator_tps6286x_driver,
        .bus = "i2c1",
        .addr = 0x49,
        .min_uv = 800000,
        .max_uv = 800000,
        .settle_us = 1000,
        .always_on = true,
    },
};

const struct regulator_config *board_regulator_configs(unsigned int *count)
{
    *count = ARRAY_SIZE(board_regulators);
    return board_regulators;
}
