/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
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

#ifndef __DRV_REGULATOR_H__
#define __DRV_REGULATOR_H__

#include <stdbool.h>
#include <stdint.h>
#include <rtthread.h>

struct regulator_dev;

/* Provider operations on selectors of the driver's linear range. The core
 * converts voltages, rounds, enforces the rail limits and waits for settling. */
struct regulator_ops {
    int (*init)(struct regulator_dev *rdev);
    int (*set_enable)(struct regulator_dev *rdev, bool enable);
    int (*set_selector)(struct regulator_dev *rdev, unsigned int sel);
    int (*get_selector)(struct regulator_dev *rdev, unsigned int *sel);
};

struct regulator_driver {
    const char *name;
    const struct regulator_ops *ops;
    /* Linear range: uV = min_uv + sel * step_uv, for sel < n_voltages. A
     * driver whose voltages come from board data sets rdev->volt_table in
     * init instead. */
    int min_uv;
    int step_uv;
    unsigned int n_voltages;
};

/* One board rail: the chip that drives it, where, and what the SoC tolerates. */
struct regulator_config {
    const char *name;
    const struct regulator_driver *driver;
    const char *bus;
    uint16_t addr;
    /* Rail limits. A request window is intersected with them; when nothing
     * remains, the request fails. */
    int min_uv;
    int max_uv;
    /* After a rise: settle_us plus the rise at ramp_uv_per_us (0: none). */
    unsigned int ramp_uv_per_us;
    unsigned int settle_us;
    bool always_on;
    /* Driver-specific board data, e.g. struct regulator_gpio_data. */
    const void *data;
};

/* Provider view. Consumers only hold the pointer from regulator_get(). */
struct regulator_dev {
    const struct regulator_config *config;
    struct rt_mutex lock;
    void *bus;
    int error;          /* initialization result; 0 when usable */
    int sel;            /* last known selector, -1 when unknown */
    /* Voltage of each selector, when not the driver's linear range. */
    const int *volt_table;
    unsigned int n_table;
};

/* Provider drivers for board tables. */
extern const struct regulator_driver regulator_tps6286x_driver;
extern const struct regulator_driver regulator_gpio_driver;
extern const struct regulator_driver regulator_k230_iobank_driver;

/* Board data for regulator_gpio_driver: a switch or LDO with an optional
 * enable GPIO and an optional GPIO selecting between two voltages. */
struct regulator_gpio_data {
    int enable_pin;             /* -1: no enable control */
    bool enable_active_low;
    int select_pin;             /* -1: volts[0] only */
    int volts[2];               /* output with select_pin low, high */
};

/* Switchable K230 IO banks. IO0-1 and the PMU pads are fixed at 1.8 V. */
enum k230_iobank {
    K230_IOBANK_IO2_13,
    K230_IOBANK_IO14_25,
    K230_IOBANK_IO26_37,
    K230_IOBANK_IO38_49,
    K230_IOBANK_IO50_61,
    K230_IOBANK_IO62_63,
};

/* Board data for regulator_k230_iobank_driver: the voltage mode (MSC) of
 * every pad in one K230 IO bank, with the regulator that switches the bank's
 * external rail (I/O supply and/or pull-ups), e.g. a regulator_gpio_driver
 * rail listed earlier in the board table. A pad in 1.8 V mode must never see
 * 3.3 V: going down, the rail falls before the pads switch; going up, the
 * pads switch before the rail rises. */
struct regulator_k230_iobank_data {
    enum k230_iobank bank;
    const char *supply;         /* RT_NULL: no external rail to switch */
    unsigned int fall_us;       /* rail fall time to 1.8 V */
};

/* Board rails, from board/configs/<board>/regulator_config.c. */
const struct regulator_config *board_regulator_configs(unsigned int *count);

#ifdef RT_USING_REGULATOR
/* RT_NULL when the board has no such rail. A rail whose provider failed to
 * initialize is still returned; regulator_status() reports the failure. */
struct regulator_dev *regulator_get(const char *name);
int regulator_status(struct regulator_dev *reg);
int regulator_enable(struct regulator_dev *reg, bool enable);
/* Lowest supported voltage >= min_uv that is <= max_uv and within the rail. */
int regulator_set_voltage(struct regulator_dev *reg, int min_uv, int max_uv);
int regulator_get_voltage(struct regulator_dev *reg, int *uv);
#else
static inline struct regulator_dev *regulator_get(const char *name)
{
    (void)name;
    return RT_NULL;
}
static inline int regulator_status(struct regulator_dev *reg)
{
    (void)reg;
    return -RT_ENOSYS;
}
static inline int regulator_enable(struct regulator_dev *reg, bool enable)
{
    (void)reg;
    (void)enable;
    return -RT_ENOSYS;
}
static inline int regulator_set_voltage(struct regulator_dev *reg, int min_uv, int max_uv)
{
    (void)reg;
    (void)min_uv;
    (void)max_uv;
    return -RT_ENOSYS;
}
static inline int regulator_get_voltage(struct regulator_dev *reg, int *uv)
{
    (void)reg;
    (void)uv;
    return -RT_ENOSYS;
}
#endif

#endif /* __DRV_REGULATOR_H__ */
