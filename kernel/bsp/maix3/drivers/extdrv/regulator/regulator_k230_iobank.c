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

#include <rtthread.h>
#include "regulator.h"
#include "drv_fpioa.h"

#define IOBANK_SEL_3V3 0U
#define IOBANK_SEL_1V8 1U

static const int iobank_volts[] = { 3300000, 1800000 };

/* Pads of each switchable bank; the whole bank changes together. */
static const struct {
    int first, last;
} iobank_pads[] = {
    [K230_IOBANK_IO2_13]  = { 2, 13 },
    [K230_IOBANK_IO14_25] = { 14, 25 },
    [K230_IOBANK_IO26_37] = { 26, 37 },
    [K230_IOBANK_IO38_49] = { 38, 49 },
    [K230_IOBANK_IO50_61] = { 50, 61 },
    [K230_IOBANK_IO62_63] = { 62, 63 },
};

struct regulator_iobank_state {
    const struct regulator_k230_iobank_data *data;
    struct regulator_dev *supply;
    unsigned int sel;
};

static int iobank_pad_1v8(int pad, bool *v1p8)
{
    fpioa_iomux_cfg_t cfg;

    if (drv_fpioa_get_pin_cfg(pad, &cfg.u.value))
        return -RT_EIO;
    *v1p8 = cfg.u.bit.msc != 0;
    return RT_EOK;
}

static int iobank_set_pads(enum k230_iobank bank, bool v1p8)
{
    int pad;

    for (pad = iobank_pads[bank].first; pad <= iobank_pads[bank].last; ++pad)
        if (drv_fpioa_set_pin_msc(pad, v1p8))
            return -RT_EIO;
    return RT_EOK;
}

/* The rail regulator rounds up within its limits; ask for the exact level. */
static int iobank_set_supply(struct regulator_iobank_state *state, unsigned int sel)
{
    if (!state->supply)
        return RT_EOK;
    return regulator_set_voltage(state->supply, iobank_volts[sel], iobank_volts[sel]);
}

static int iobank_init(struct regulator_dev *rdev)
{
    const struct regulator_k230_iobank_data *data = rdev->config->data;
    struct regulator_iobank_state *state;
    struct regulator_dev *supply = RT_NULL;
    bool first, v1p8;
    int pad, ret;

    if (!data || (unsigned int)data->bank >= sizeof(iobank_pads) / sizeof(iobank_pads[0]))
        return -RT_EINVAL;
    if (data->supply) {
        supply = regulator_get(data->supply);
        if (!supply)
            return -RT_EINVAL;
        if (regulator_status(supply))
            return regulator_status(supply);
    }
    /* The board pin table set the whole bank to one mode; pads that
     * disagree are a board table error. */
    if (iobank_pad_1v8(iobank_pads[data->bank].first, &first))
        return -RT_EIO;
    for (pad = iobank_pads[data->bank].first + 1; pad <= iobank_pads[data->bank].last; ++pad) {
        if (iobank_pad_1v8(pad, &v1p8))
            return -RT_EIO;
        if (v1p8 != first)
            return -RT_EINVAL;
    }
    state = rt_malloc(sizeof(*state));
    if (!state)
        return -RT_ENOMEM;
    state->data = data;
    state->supply = supply;
    state->sel = first ? IOBANK_SEL_1V8 : IOBANK_SEL_3V3;
    /* The rail follows the pads. */
    ret = iobank_set_supply(state, state->sel);
    if (ret) {
        rt_free(state);
        return ret;
    }
    rdev->bus = state;
    rdev->volt_table = iobank_volts;
    rdev->n_table = sizeof(iobank_volts) / sizeof(iobank_volts[0]);
    return RT_EOK;
}

static int iobank_set_selector(struct regulator_dev *rdev, unsigned int sel)
{
    struct regulator_iobank_state *state = rdev->bus;
    const struct regulator_k230_iobank_data *data = state->data;
    int ret;

    if (sel == IOBANK_SEL_1V8) {
        ret = iobank_set_supply(state, sel);
        if (ret)
            return ret;
        /* sel follows the rail, which is now at 1.8 V. If the pads then
         * fail to switch, those left in 3.3 V mode are safe on it. */
        state->sel = sel;
        if (state->supply && data->fall_us)
            rt_thread_mdelay((data->fall_us + 999) / 1000);
        return iobank_set_pads(data->bank, true);
    }
    ret = iobank_set_pads(data->bank, false);
    if (ret)
        return ret;
    ret = iobank_set_supply(state, sel);
    if (!ret)
        state->sel = sel;
    return ret;
}

static int iobank_get_selector(struct regulator_dev *rdev, unsigned int *sel)
{
    *sel = ((struct regulator_iobank_state *)rdev->bus)->sel;
    return RT_EOK;
}

static const struct regulator_ops iobank_ops = {
    .init = iobank_init,
    .set_selector = iobank_set_selector,
    .get_selector = iobank_get_selector,
};

/* 3.3 V (selector 0) or 1.8 V (selector 1). */
const struct regulator_driver regulator_k230_iobank_driver = {
    .name = "k230_iobank",
    .ops = &iobank_ops,
};
