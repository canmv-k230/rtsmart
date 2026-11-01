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

#include <stdlib.h>
#include <string.h>
#include <rtthread.h>
#include "regulator.h"
#define DBG_TAG  "regulator"
#ifdef RT_DEBUG
#define DBG_LVL          DBG_LOG
#else
#define DBG_LVL          DBG_WARNING
#endif
#define DBG_COLOR
#include <rtdbg.h>

#define REGULATOR_LOCK_MS 100

static struct regulator_dev *regulators;
static unsigned int regulator_count;

__attribute__((weak)) const struct regulator_config *board_regulator_configs(unsigned int *count)
{
    *count = 0;
    return RT_NULL;
}

/* Only handles returned by regulator_get() are accepted. */
static bool regulator_valid(const struct regulator_dev *reg)
{
    return reg && regulators && reg >= regulators && reg < regulators + regulator_count;
}

static unsigned int regulator_n_sel(const struct regulator_dev *reg)
{
    return reg->volt_table ? reg->n_table : reg->config->driver->n_voltages;
}

static int regulator_uv(const struct regulator_dev *reg, unsigned int sel)
{
    const struct regulator_driver *drv = reg->config->driver;

    return reg->volt_table ? reg->volt_table[sel] : drv->min_uv + (int)sel * drv->step_uv;
}

/* Lowest and highest output the provider supports. */
static void regulator_range(const struct regulator_dev *reg, int *lo, int *hi)
{
    unsigned int sel, n = regulator_n_sel(reg);

    *lo = *hi = regulator_uv(reg, 0);
    for (sel = 1; reg->volt_table && sel < n; ++sel) {
        if (reg->volt_table[sel] < *lo)
            *lo = reg->volt_table[sel];
        if (reg->volt_table[sel] > *hi)
            *hi = reg->volt_table[sel];
    }
    if (!reg->volt_table)
        *hi = regulator_uv(reg, n - 1);
}

/* Lowest supported voltage at or above min_uv, within max_uv and the rail limits. */
static int regulator_select(const struct regulator_dev *reg, int min_uv, int max_uv,
    unsigned int *sel)
{
    const struct regulator_config *cfg = reg->config;
    const struct regulator_driver *drv = cfg->driver;
    unsigned int s, best = 0, n = regulator_n_sel(reg);
    bool found = false;
    int64_t linear;

    if (min_uv < cfg->min_uv)
        min_uv = cfg->min_uv;
    if (max_uv > cfg->max_uv)
        max_uv = cfg->max_uv;
    if (min_uv > max_uv)
        return -RT_EINVAL;
    if (reg->volt_table) {
        for (s = 0; s < n; ++s) {
            int uv = reg->volt_table[s];
            if (uv >= min_uv && uv <= max_uv && (!found || uv < reg->volt_table[best])) {
                best = s;
                found = true;
            }
        }
        if (!found)
            return -RT_EINVAL;
        *sel = best;
        return RT_EOK;
    }
    linear = min_uv <= drv->min_uv ? 0 :
        ((int64_t)min_uv - drv->min_uv + drv->step_uv - 1) / drv->step_uv;
    if (linear >= n || regulator_uv(reg, (unsigned int)linear) > max_uv)
        return -RT_EINVAL;
    *sel = (unsigned int)linear;
    return RT_EOK;
}

/* Wait after the output rises from from_uv to selector sel_to. A known fall
 * needs none. An unknown start (from_uv < 0) may be anywhere in the
 * provider's range, so the rise is counted from its lowest voltage. */
static uint32_t regulator_settle_us(const struct regulator_dev *reg, int from_uv,
    unsigned int sel_to)
{
    const struct regulator_config *cfg = reg->config;
    int from = from_uv, hi, to = regulator_uv(reg, sel_to);
    uint32_t us = cfg->settle_us;

    if (from_uv < 0)
        regulator_range(reg, &from, &hi);
    else if (to <= from)
        return 0;
    if (cfg->ramp_uv_per_us && to > from)
        us += ((uint32_t)(to - from) + cfg->ramp_uv_per_us - 1) / cfg->ramp_uv_per_us;
    return us;
}

struct regulator_dev *regulator_get(const char *name)
{
    unsigned int i;

    if (!name)
        return RT_NULL;
    for (i = 0; i < regulator_count; ++i)
        if (!strcmp(regulators[i].config->name, name))
            return &regulators[i];
    return RT_NULL;
}

int regulator_status(struct regulator_dev *reg)
{
    return regulator_valid(reg) ? reg->error : -RT_EINVAL;
}

int regulator_enable(struct regulator_dev *reg, bool enable)
{
    const struct regulator_ops *ops;
    int ret;

    if (!regulator_valid(reg))
        return -RT_EINVAL;
    if (reg->error)
        return reg->error;
    if (!enable && reg->config->always_on)
        return -RT_EINVAL;
    ops = reg->config->driver->ops;
    if (!ops->set_enable)
        return -RT_ENOSYS;
    if (rt_mutex_take(&reg->lock, rt_tick_from_millisecond(REGULATOR_LOCK_MS)) != RT_EOK)
        return -RT_ETIMEOUT;
    ret = ops->set_enable(reg, enable);
    /* Enabling ramps the output from 0 V. */
    if (!ret && enable && reg->sel >= 0) {
        uint32_t us = regulator_settle_us(reg, 0, (unsigned int)reg->sel);
        if (us)
            rt_thread_mdelay((us + 999) / 1000);
    }
    rt_mutex_release(&reg->lock);
    return ret;
}

int regulator_set_voltage(struct regulator_dev *reg, int min_uv, int max_uv)
{
    unsigned int sel;
    uint32_t us;
    int ret;

    if (!regulator_valid(reg))
        return -RT_EINVAL;
    if (reg->error)
        return reg->error;
    ret = regulator_select(reg, min_uv, max_uv, &sel);
    if (ret)
        return ret;
    if (rt_mutex_take(&reg->lock, rt_tick_from_millisecond(REGULATOR_LOCK_MS)) != RT_EOK)
        return -RT_ETIMEOUT;
    us = regulator_settle_us(reg, reg->sel >= 0 ?
        regulator_uv(reg, (unsigned int)reg->sel) : -1, sel);
    ret = reg->config->driver->ops->set_selector(reg, sel);
    /* A failed write leaves the output unknown. */
    reg->sel = ret ? -1 : (int)sel;
    if (!ret && us)
        rt_thread_mdelay((us + 999) / 1000);
    rt_mutex_release(&reg->lock);
    return ret;
}

int regulator_get_voltage(struct regulator_dev *reg, int *uv)
{
    unsigned int sel;
    int ret;

    if (!regulator_valid(reg) || !uv)
        return -RT_EINVAL;
    if (reg->error)
        return reg->error;
    if (rt_mutex_take(&reg->lock, rt_tick_from_millisecond(REGULATOR_LOCK_MS)) != RT_EOK)
        return -RT_ETIMEOUT;
    ret = reg->config->driver->ops->get_selector(reg, &sel);
    if (!ret && sel >= regulator_n_sel(reg))
        ret = -RT_EIO;
    reg->sel = ret ? -1 : (int)sel;
    rt_mutex_release(&reg->lock);
    if (!ret)
        *uv = regulator_uv(reg, sel);
    return ret;
}

/* Static checks; the voltage range is known only after the driver's init. */
static int regulator_check_config(const struct regulator_config *cfg, unsigned int index,
    const struct regulator_config *configs)
{
    const struct regulator_driver *drv = cfg->driver;
    unsigned int i;

    if (!cfg->name || !drv || !drv->ops || !drv->ops->set_selector ||
        !drv->ops->get_selector || cfg->min_uv > cfg->max_uv)
        return -RT_EINVAL;
    for (i = 0; i < index; ++i)
        if (configs[i].name && !strcmp(configs[i].name, cfg->name))
            return -RT_EINVAL;
    return RT_EOK;
}

static int regulator_check_range(const struct regulator_dev *reg)
{
    const struct regulator_driver *drv = reg->config->driver;
    int lo, hi;

    if (reg->volt_table ? !reg->n_table : (drv->step_uv <= 0 || !drv->n_voltages))
        return -RT_EINVAL;
    regulator_range(reg, &lo, &hi);
    return reg->config->min_uv < lo || reg->config->max_uv > hi ? -RT_EINVAL : RT_EOK;
}

static int regulator_core_init(void)
{
    const struct regulator_config *configs;
    struct regulator_dev *regs;
    unsigned int count = 0, i;

    configs = board_regulator_configs(&count);
    if (!configs || !count)
        return RT_EOK;
    regs = rt_calloc(count, sizeof(*regs));
    if (!regs)
        return -RT_ENOMEM;
    /* Rails are published one by one, so a driver's init can look up a
     * supply listed earlier in the table. */
    regulators = regs;
    for (i = 0; i < count; ++i) {
        struct regulator_dev *reg = &regs[i];
        const struct regulator_config *cfg = &configs[i];
        unsigned int sel;

        reg->config = cfg;
        reg->sel = -1;
        reg->error = regulator_check_config(cfg, i, configs);
        if (!reg->error && rt_mutex_init(&reg->lock, cfg->name, RT_IPC_FLAG_PRIO) != RT_EOK)
            reg->error = -RT_ERROR;
        if (!reg->error && cfg->driver->ops->init)
            reg->error = cfg->driver->ops->init(reg);
        if (!reg->error)
            reg->error = regulator_check_range(reg);
        /* Reading the output proves the provider responds. */
        if (!reg->error) {
            reg->error = cfg->driver->ops->get_selector(reg, &sel);
            if (!reg->error && sel >= regulator_n_sel(reg))
                reg->error = -RT_EIO;
            if (!reg->error)
                reg->sel = (int)sel;
        }
        if (reg->error)
            LOG_E("%s: init failed: %d", cfg->name ? cfg->name : "?", reg->error);
        regulator_count = i + 1;
    }
    return RT_EOK;
}
/* Init sections run in name order: after I2C (3), GPIO (3.010) and the board
 * pin table (3.1), which sets the IO bank modes the k230_iobank provider
 * reads; before the SD/MMC hosts (3.200), which look up their supplies. */
INIT_DEVICE_EXPORT_SEQ(regulator_core_init, 100);

#if defined(RT_USING_MSH) && defined(RT_REGULATOR_ENABLE_BUILTIN_CMD)
static void regulator_show(struct regulator_dev *reg)
{
    const struct regulator_config *cfg = reg->config;
    int uv = 0, ret = regulator_get_voltage(reg, &uv);

    rt_kprintf("%-16s %-10s %s/0x%02x limits=%d..%d uV%s ", cfg->name,
        cfg->driver ? cfg->driver->name : "?", cfg->bus ? cfg->bus : "-", cfg->addr,
        cfg->min_uv, cfg->max_uv, cfg->always_on ? " always-on" : "");
    if (ret)
        rt_kprintf("error=%d\n", ret);
    else
        rt_kprintf("now=%d uV\n", uv);
}

static int regulator_cmd(int argc, char **argv)
{
    struct regulator_dev *reg;
    int value, ret;

    if (argc < 2) {
        for (unsigned int i = 0; i < regulator_count; ++i)
            regulator_show(&regulators[i]);
        rt_kprintf("\nUsage:\t%s name op(e|s|g|t) [data]\n"
            "\te 0|1                      : enable\n"
            "\ts uV                       : set exactly, within the rail limits\n"
            "\tg                          : get\n"
            "\tt start stop step delay_ms : sweep\n",
            argv[0]);
        return 0;
    }
    reg = regulator_get(argv[1]);
    if (!reg) {
        rt_kprintf("Can't find regulator %s\n", argv[1]);
        return -1;
    }
    if (argc < 3) {
        regulator_show(reg);
        return 0;
    }

    switch (argv[2][0]) {
    case 'e':
        if (argc < 4) {
            rt_kprintf("please input 0|1\n");
            return -1;
        }
        ret = regulator_enable(reg, strtol(argv[3], 0, 0) != 0);
        break;
    case 's':
        if (argc < 4) {
            rt_kprintf("please input voltage\n");
            return -1;
        }
        value = strtol(argv[3], 0, 0);
        if (value < reg->config->min_uv || value > reg->config->max_uv) {
            rt_kprintf("%d uV is outside the %s limits %d..%d uV\n", value,
                reg->config->name, reg->config->min_uv, reg->config->max_uv);
            return -1;
        }
        ret = regulator_set_voltage(reg, value, value);
        break;
    case 'g':
        ret = regulator_get_voltage(reg, &value);
        if (!ret)
            rt_kprintf("current voltage: %d uV\n", value);
        break;
    case 't': {
        int start, stop, step, delay;

        if (argc < 7) {
            rt_kprintf("please input start_volt stop_volt step_volt step_delay\n");
            return -1;
        }
        start = strtol(argv[3], 0, 0);
        stop = strtol(argv[4], 0, 0);
        step = strtol(argv[5], 0, 0);
        delay = strtol(argv[6], 0, 0);
        if (step <= 0) {
            rt_kprintf("step_volt must be positive\n");
            return -1;
        }
        ret = 0;
        for (int volt = start; start > stop ? volt >= stop : volt <= stop;
            volt += start > stop ? -step : step) {
            /* Stops at the first step outside the rail limits. */
            ret = regulator_set_voltage(reg, volt, volt);
            if (!ret)
                ret = regulator_get_voltage(reg, &value);
            if (ret)
                break;
            rt_kprintf("current voltage: %d uV\n", value);
            rt_thread_mdelay(delay);
        }
        break;
    }
    default:
        rt_kprintf("invalid op '%c'\n", argv[2][0]);
        return -1;
    }

    if (ret != 0) {
        rt_kprintf("op '%c' fail: %d\n", argv[2][0], ret);
        return -1;
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(regulator_cmd, regulator, list and control board regulators);
#endif
