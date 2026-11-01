#!/usr/bin/env python3
"""Exercise the regulator core with the production GPIO and K230 IO-bank drivers.

Covers voltage selection against rail limits and voltage tables, settling
waits, always-on rails, handle checks, configuration validation and failed
providers. For the IO-bank supply, every GPIO and pad write is checked so that
no pad in 1.8 V mode ever sees a 3.3 V supply. The header stubs used without
RT_USING_REGULATOR are compiled as well.
Run with python3 -B kernel/bsp/maix3/testcase/host/test_regulator.py.
"""
from pathlib import Path
import os
import subprocess
import tempfile

BSP = Path(__file__).resolve().parents[2]
REGULATOR = BSP / "drivers/extdrv/regulator"

STUBS = r"""
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t rt_tick_t;
struct rt_mutex { bool held; };
#define RT_NULL NULL
#define RT_EOK 0
#define RT_ERROR 1
#define RT_ETIMEOUT 2
#define RT_ENOMEM 5
#define RT_ENOSYS 6
#define RT_EIO 8
#define RT_EINVAL 10
#define RT_IPC_FLAG_PRIO 0
#define TIMER_CLK_FREQ 1000000
#define INIT_DEVICE_EXPORT_SEQ(fn, seq)
#define LOG_E(...) ((void)0)
#define rt_calloc calloc
#define rt_malloc malloc
#define rt_free free
#define rt_kprintf printf
typedef long rt_base_t;
static unsigned delays_ms;
static uint64_t now_us;
static inline uint64_t cpu_ticks(void) { return ++now_us; }
static inline rt_tick_t rt_tick_from_millisecond(unsigned ms) { return ms; }
static inline void rt_thread_mdelay(unsigned ms) { delays_ms += ms; }
static inline int rt_mutex_init(struct rt_mutex *m, const char *n, int f) { m->held = false; return 0; }
static inline int rt_mutex_take(struct rt_mutex *m, rt_tick_t t) { assert(!m->held); m->held = true; return 0; }
static inline int rt_mutex_release(struct rt_mutex *m) { assert(m->held); m->held = false; return 0; }

/* GPIO and IOMUX. The IO bank under test: select pin, polarity and pads. */
#define GPIO_DM_OUTPUT 0
#define GPIO_MAX_NUM 72
/* pin_level: what the pin carries; pin_dr: what it drives as an output. */
static int pin_level[72], pin_dr[72];
static bool pin_output[72];
static unsigned pin_glitches;   /* level changes caused by becoming an output */
static uint32_t pads[64];
static int bank_pin = -1, bank_first, bank_last;
static bool bank_high_is_1v8;
static unsigned bank_events;
static void bank_check(void)
{
    if (bank_pin < 0)
        return;
    bool supply_1v8 = (pin_level[bank_pin] != 0) == bank_high_is_1v8;
    for (int pad = bank_first; pad <= bank_last; ++pad)
        assert(supply_1v8 || !(pads[pad] & 0x200)); /* 1.8 V pad on 3.3 V supply */
    bank_events++;
}
static inline rt_base_t kd_pin_read(rt_base_t pin) { return pin_level[pin]; }
static inline int kd_pin_mode(rt_base_t pin, rt_base_t mode)
{
    pin_output[pin] = mode == GPIO_DM_OUTPUT;
    if (pin_output[pin] && pin_level[pin] != pin_dr[pin]) {
        pin_glitches++;
        pin_level[pin] = pin_dr[pin];
        bank_check();
    }
    return 0;
}
static inline int kd_pin_write(rt_base_t pin, rt_base_t value)
{
    assert(pin_output[pin]);
    pin_level[pin] = pin_dr[pin] = value ? 1 : 0;
    bank_check();
    return 0;
}
/* The output register is writable in either direction. */
static inline void kd_pin_set_dr(rt_base_t pin, int value)
{
    pin_dr[pin] = value ? 1 : 0;
    if (pin_output[pin]) {
        pin_level[pin] = pin_dr[pin];
        bank_check();
    }
}
typedef struct {
    union {
        struct {
            uint32_t st : 1, ds : 4, pd : 1, pu : 1, oe : 1, ie : 1, msc : 1, rsv : 1,
                io_sel : 3, rsv2 : 17, di : 1;
        } bit;
        uint32_t value;
    } u;
} fpioa_iomux_cfg_t;
static inline int drv_fpioa_get_pin_cfg(int pin, uint32_t *value) { *value = pads[pin]; return 0; }
static inline int drv_fpioa_set_pin_msc(int pin, bool v1p8)
{
    pads[pin] = v1p8 ? (pads[pin] | 0x200) : (pads[pin] & ~0x200U);
    bank_check();
    return 0;
}
"""

# The board table overrides the weak default, so it is its own object.
BOARD = r"""
#include "regulator.h"
static int fail_get(struct regulator_dev *rdev, unsigned int *sel) { return -RT_EIO; }
static int fail_set(struct regulator_dev *rdev, unsigned int sel) { return -RT_EIO; }
static const struct regulator_ops fail_ops = { .set_selector = fail_set, .get_selector = fail_get };
static const struct regulator_driver fail_driver = {
    .name = "fail", .ops = &fail_ops, .min_uv = 0, .step_uv = 5000, .n_voltages = 401,
};

/* In-memory linear provider, 0 V to 2 V in 5 mV steps, starting at the rail
 * maximum: readback returns the last selector set. */
static int mem_init(struct regulator_dev *rdev)
{
    unsigned int *sel = malloc(sizeof(*sel));
    if (!sel)
        return -RT_ENOMEM;
    *sel = (unsigned int)(rdev->config->max_uv / 5000);
    rdev->bus = sel;
    return RT_EOK;
}
static int mem_enable(struct regulator_dev *rdev, bool enable) { return RT_EOK; }
static int mem_set(struct regulator_dev *rdev, unsigned int sel) { *(unsigned int *)rdev->bus = sel; return RT_EOK; }
static int mem_get(struct regulator_dev *rdev, unsigned int *sel) { *sel = *(unsigned int *)rdev->bus; return RT_EOK; }
static const struct regulator_ops mem_ops = {
    .init = mem_init, .set_enable = mem_enable, .set_selector = mem_set, .get_selector = mem_get,
};
static const struct regulator_driver mem_driver = {
    .name = "mem", .ops = &mem_ops, .min_uv = 0, .step_uv = 5000, .n_voltages = 401,
};

static const struct regulator_gpio_data vmmc_data = {
    .enable_pin = 5, .select_pin = -1, .volts = { 3300000 },
};
/* A card switch that is on at reset (pull-up): taken over without a pulse. */
static const struct regulator_gpio_data held_data = {
    .enable_pin = 8, .select_pin = -1, .volts = { 3300000 },
};
/* A load switch whose output ramps at 1 mV/us once enabled. */
static const struct regulator_gpio_data ramped_data = {
    .enable_pin = 9, .select_pin = -1, .volts = { 3300000 },
};
static const struct regulator_gpio_data vsel_data = {
    .enable_pin = -1, .select_pin = 6, .volts = { 3300000, 1800000 },
};
/* The external rail (pull-ups) of bank IO50-61: GPIO 7 high selects 1.8 V. */
static const struct regulator_gpio_data bank_rail_data = {
    .enable_pin = -1, .select_pin = 7, .volts = { 3300000, 1800000 },
};
static const struct regulator_k230_iobank_data bank_data = {
    .bank = K230_IOBANK_IO50_61, .supply = "bank_rail", .fall_us = 2000,
};
static const struct regulator_k230_iobank_data mixed_data = {
    .bank = K230_IOBANK_IO38_49,
};
static const struct regulator_k230_iobank_data orphan_data = {
    .bank = K230_IOBANK_IO2_13, .supply = "missing",
};

static const struct regulator_config table[] = {
    { .name = "a", .driver = &mem_driver, .min_uv = 600000, .max_uv = 900000,
      .settle_us = 1000, .always_on = true },
    { .name = "b", .driver = &mem_driver, .min_uv = 700000, .max_uv = 700000,
      .ramp_uv_per_us = 100 },
    { .name = "a", .driver = &mem_driver, .min_uv = 600000, .max_uv = 900000 },
    { .name = "range", .driver = &mem_driver, .min_uv = 600000, .max_uv = 3000000 },
    { .name = "fail", .driver = &fail_driver, .min_uv = 600000, .max_uv = 900000 },
    { .name = "vmmc", .driver = &regulator_gpio_driver, .data = &vmmc_data,
      .min_uv = 3300000, .max_uv = 3300000, .settle_us = 1000 },
    { .name = "held", .driver = &regulator_gpio_driver, .data = &held_data,
      .min_uv = 3300000, .max_uv = 3300000 },
    { .name = "ramped", .driver = &regulator_gpio_driver, .data = &ramped_data,
      .min_uv = 3300000, .max_uv = 3300000, .ramp_uv_per_us = 1000 },
    { .name = "vsel", .driver = &regulator_gpio_driver, .data = &vsel_data,
      .min_uv = 1800000, .max_uv = 3300000 },
    { .name = "bank_rail", .driver = &regulator_gpio_driver, .data = &bank_rail_data,
      .min_uv = 1800000, .max_uv = 3300000, .settle_us = 1000 },
    { .name = "bank", .driver = &regulator_k230_iobank_driver, .data = &bank_data,
      .min_uv = 1800000, .max_uv = 3300000 },
    { .name = "mixed", .driver = &regulator_k230_iobank_driver, .data = &mixed_data,
      .min_uv = 1800000, .max_uv = 3300000 },
    { .name = "orphan", .driver = &regulator_k230_iobank_driver, .data = &orphan_data,
      .min_uv = 1800000, .max_uv = 3300000 },
    { .name = "nodata", .driver = &regulator_gpio_driver, .min_uv = 0, .max_uv = 0 },
};

const struct regulator_config *board_regulator_configs(unsigned int *count)
{
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}
"""

CASES = r"""

static int voltage(struct regulator_dev *reg)
{
    int uv = 0;
    assert(regulator_get_voltage(reg, &uv) == RT_EOK);
    return uv;
}

static void test_gpio_and_bank(void)
{
    struct regulator_dev *vmmc = regulator_get("vmmc");
    struct regulator_dev *vsel = regulator_get("vsel");
    struct regulator_dev *bank = regulator_get("bank");
    int pad;

    /* Pins are taken over at their current levels. */
    assert(regulator_status(vmmc) == RT_EOK && pin_output[5] && pin_level[5] == 0);
    assert(regulator_status(regulator_get("held")) == RT_EOK && pin_output[8] && pin_level[8] == 1);
    assert(pin_glitches == 0);
    assert(regulator_status(vsel) == RT_EOK && voltage(vsel) == 3300000);
    assert(regulator_status(regulator_get("nodata")) == -RT_EINVAL);

    /* Enable control waits for the output to settle. */
    delays_ms = 0;
    assert(regulator_enable(vmmc, true) == RT_EOK && pin_level[5] == 1 && delays_ms == 1);
    assert(regulator_enable(vmmc, false) == RT_EOK && pin_level[5] == 0);
    assert(voltage(vmmc) == 3300000);
    /* Enabling ramps from 0 V, not from the lowest selectable voltage. */
    delays_ms = 0;
    assert(regulator_enable(regulator_get("ramped"), true) == RT_EOK && delays_ms == 4);
    assert(regulator_set_voltage(vmmc, 1700000, 1950000) == -RT_EINVAL);

    /* A voltage table: the lowest entry within the window. */
    assert(regulator_set_voltage(vsel, 1700000, 1950000) == RT_EOK && pin_level[6] == 1);
    assert(voltage(vsel) == 1800000);
    assert(regulator_set_voltage(vsel, 1700000, 3600000) == RT_EOK && voltage(vsel) == 1800000);
    assert(regulator_set_voltage(vsel, 2700000, 3600000) == RT_EOK && pin_level[6] == 0);
    assert(regulator_set_voltage(vsel, 2500000, 2500000) == -RT_EINVAL);
    assert(regulator_enable(vsel, false) == -RT_ENOSYS);

    /* The IO bank and its rail follow the board pin table at registration. */
    assert(regulator_status(bank) == RT_EOK && voltage(bank) == 3300000);
    assert(pin_output[7] && pin_level[7] == 0);
    /* Pads that disagree are a board table error; so is a missing rail. */
    assert(regulator_status(regulator_get("mixed")) == -RT_EINVAL);
    assert(regulator_status(regulator_get("orphan")) == -RT_EINVAL);

    /* Down: the rail falls first, then every pad of the bank switches. */
    delays_ms = bank_events = 0;
    assert(regulator_set_voltage(bank, 1700000, 1950000) == RT_EOK);
    assert(pin_level[7] == 1 && delays_ms == 2 && bank_events == 13);
    for (pad = 50; pad <= 61; ++pad)
        assert(pads[pad] & 0x200);
    assert(voltage(bank) == 1800000);
    /* Up: the pads switch first, then the rail rises and settles. */
    delays_ms = bank_events = 0;
    assert(regulator_set_voltage(bank, 2700000, 3600000) == RT_EOK);
    assert(pin_level[7] == 0 && delays_ms == 1 && bank_events == 13);
    for (pad = 50; pad <= 61; ++pad)
        assert(!(pads[pad] & 0x200));
    assert(voltage(regulator_get("bank_rail")) == 3300000);
    assert(pads[49] == 0x1234 && pads[62] == 0x5678);  /* other banks untouched */
}

int main(void)
{
    struct regulator_dev *a, *b, *fail, foreign;

    /* Board state before the core starts: bank IO50-61 in 3.3 V mode, its
     * neighbours, and bank IO38-49 with pads in different modes. */
    bank_pin = 7; bank_first = 50; bank_last = 61; bank_high_is_1v8 = true;
    pads[49] = 0x1234;
    pads[62] = 0x5678;
    pads[41] = 0x200;
    pin_level[8] = 1;
    assert(regulator_core_init() == RT_EOK);
    a = regulator_get("a");
    b = regulator_get("b");
    fail = regulator_get("fail");
    assert(a && b && fail && !regulator_get("missing") && !regulator_get(NULL));

    /* Configuration errors and failed providers stay visible as failed rails. */
    assert(regulator_status(a) == RT_EOK && regulator_status(b) == RT_EOK);
    assert(regulator_status(&regulators[2]) == -RT_EINVAL); /* duplicate name */
    assert(regulator_status(&regulators[3]) == -RT_EINVAL); /* beyond the driver */
    assert(regulator_status(fail) == -RT_EIO);
    assert(regulator_set_voltage(fail, 700000, 700000) == -RT_EIO);

    /* The dummy starts at the rail maximum. */
    assert(voltage(a) == 900000 && voltage(b) == 700000);

    /* Lowest step at or above the minimum; a fall needs no wait. */
    delays_ms = 0;
    assert(regulator_set_voltage(a, 612345, 900000) == RT_EOK);
    assert(voltage(a) == 615000 && delays_ms == 0);
    /* A window is intersected with the rail limits. */
    assert(regulator_set_voltage(a, 500000, 650000) == RT_EOK && voltage(a) == 600000);
    /* Nothing left of the window: refused, the output is untouched. */
    assert(regulator_set_voltage(a, 500000, 550000) == -RT_EINVAL);
    assert(regulator_set_voltage(a, 950000, 1000000) == -RT_EINVAL);
    assert(regulator_set_voltage(a, 801000, 804000) == -RT_EINVAL);
    assert(regulator_set_voltage(a, 700000, 600000) == -RT_EINVAL);
    assert(voltage(a) == 600000);

    /* A rise waits for the settle time; unchanged output waits for nothing. */
    delays_ms = 0;
    assert(regulator_set_voltage(a, 800000, 800000) == RT_EOK && delays_ms == 1);
    delays_ms = 0;
    assert(regulator_set_voltage(a, 800000, 800000) == RT_EOK && delays_ms == 0);
    /* An unknown output gets the full allowance, even for the lowest voltage. */
    a->sel = -1;
    assert(regulator_set_voltage(a, 600000, 600000) == RT_EOK && delays_ms == 1);
    /* Slew: 700 mV from an unknown start (the driver's 0 V) at 100 uV/us. */
    delays_ms = 0;
    b->sel = -1;
    assert(regulator_set_voltage(b, 700000, 700000) == RT_EOK && delays_ms == 7);

    /* Always-on rails refuse to turn off. */
    assert(regulator_enable(a, false) == -RT_EINVAL);
    assert(regulator_enable(a, true) == RT_EOK);
    assert(regulator_enable(b, false) == RT_EOK);

    /* Only handles from regulator_get() are accepted. */
    foreign = *a;
    assert(regulator_set_voltage(&foreign, 700000, 700000) == -RT_EINVAL);
    assert(regulator_get_voltage(&foreign, &(int){0}) == -RT_EINVAL);
    assert(regulator_enable(NULL, true) == -RT_EINVAL);
    assert(regulator_status(&foreign) == -RT_EINVAL);

    test_gpio_and_bank();

    puts("PASS regulator core: selection, limits, tables, settling, always-on, handles, failures");
    puts("PASS GPIO and K230 IO-bank regulators: takeover, enable, tables, safe bank ordering");
    return 0;
}
"""

DISABLED = r"""
#include "regulator.h"
int main(void)
{
    assert(!regulator_get("regulator_cpu"));
    assert(regulator_set_voltage(NULL, 800000, 800000) == -RT_ENOSYS);
    assert(regulator_get_voltage(NULL, &(int){0}) == -RT_ENOSYS);
    puts("PASS regulator stubs without RT_USING_REGULATOR");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="k230-regulator-test-") as directory:
        build = Path(directory)
        (build / "stubs.h").write_text(STUBS)
        for name in ("rtthread.h", "rtdevice.h", "rtdbg.h", "tick.h", "drv_gpio.h", "drv_fpioa.h"):
            (build / name).write_text('#include "stubs.h"\n')
        sources = ("regulator.c", "regulator_gpio.c", "regulator_k230_iobank.c")
        for name in ("regulator.h", *sources):
            (build / name).write_text((REGULATOR / name).read_text())
        (build / "test.c").write_text("".join(f'#include "{name}"\n' for name in sources) + CASES)
        (build / "board.c").write_text(BOARD)
        (build / "disabled.c").write_text(DISABLED)
        cc = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
              "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
              "-fsanitize=undefined,address", "-fno-sanitize-recover=all", "-I", str(build)]
        enabled = ["-DRT_USING_REGULATOR", "-DRT_USING_MSH", "-DRT_REGULATOR_ENABLE_BUILTIN_CMD",
                   "-DMSH_CMD_EXPORT_ALIAS(...)="]
        for name, flags, extra in (("test", enabled, [str(build / "board.c")]),
                                   ("disabled", [], [])):
            binary = build / name
            subprocess.run([*cc, *flags, str(build / f"{name}.c"), *extra, "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
