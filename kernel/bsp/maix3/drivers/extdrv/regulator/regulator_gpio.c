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
#include "drv_gpio.h"

struct regulator_gpio_state {
    const struct regulator_gpio_data *data;
    unsigned int sel;
};

/* Take over a pin at its current level, so registering does not glitch it:
 * the output register holds that level before the pin starts driving. */
static int regulator_gpio_claim(int pin)
{
    rt_base_t level;

    if (pin >= GPIO_MAX_NUM)
        return -RT_EINVAL;
    level = kd_pin_read(pin);
    if (level < 0)
        return -RT_EIO;
    level = level ? 1 : 0;
    kd_pin_set_dr(pin, (int)level);
    if (kd_pin_mode(pin, GPIO_DM_OUTPUT) != RT_EOK)
        return -RT_EIO;
    return level;
}

static int regulator_gpio_init(struct regulator_dev *rdev)
{
    const struct regulator_gpio_data *data = rdev->config->data;
    struct regulator_gpio_state *state;
    int level = 0;

    if (!data)
        return -RT_EINVAL;
    state = rt_malloc(sizeof(*state));
    if (!state)
        return -RT_ENOMEM;
    state->data = data;
    if (data->enable_pin >= 0 && regulator_gpio_claim(data->enable_pin) < 0)
        level = -RT_EIO;
    if (level == 0 && data->select_pin >= 0)
        level = regulator_gpio_claim(data->select_pin);
    if (level < 0) {
        rt_free(state);
        return level;
    }
    state->sel = (unsigned int)level;
    rdev->bus = state;
    rdev->volt_table = data->volts;
    rdev->n_table = data->select_pin >= 0 ? 2 : 1;
    return RT_EOK;
}

static int regulator_gpio_set_enable(struct regulator_dev *rdev, bool enable)
{
    const struct regulator_gpio_data *data = ((struct regulator_gpio_state *)rdev->bus)->data;

    if (data->enable_pin < 0)
        return enable ? RT_EOK : -RT_ENOSYS;
    return kd_pin_write(data->enable_pin, enable != data->enable_active_low) == RT_EOK ?
        RT_EOK : -RT_EIO;
}

static int regulator_gpio_set_selector(struct regulator_dev *rdev, unsigned int sel)
{
    struct regulator_gpio_state *state = rdev->bus;

    if (state->data->select_pin >= 0 &&
        kd_pin_write(state->data->select_pin, sel ? 1 : 0) != RT_EOK)
        return -RT_EIO;
    state->sel = sel;
    return RT_EOK;
}

static int regulator_gpio_get_selector(struct regulator_dev *rdev, unsigned int *sel)
{
    *sel = ((struct regulator_gpio_state *)rdev->bus)->sel;
    return RT_EOK;
}

static const struct regulator_ops regulator_gpio_ops = {
    .init = regulator_gpio_init,
    .set_enable = regulator_gpio_set_enable,
    .set_selector = regulator_gpio_set_selector,
    .get_selector = regulator_gpio_get_selector,
};

/* Voltages come from struct regulator_gpio_data. */
const struct regulator_driver regulator_gpio_driver = {
    .name = "gpio",
    .ops = &regulator_gpio_ops,
};
