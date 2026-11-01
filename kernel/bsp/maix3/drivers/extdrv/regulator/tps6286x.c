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

#include <rtthread.h>
#include <rtdevice.h>
#include "regulator.h"
#define DBG_TAG  "tps6286x"
#ifdef RT_DEBUG
#define DBG_LVL          DBG_LOG
#else
#define DBG_LVL          DBG_WARNING
#endif
#define DBG_COLOR
#include <rtdbg.h>

#define TPS6286X_VOUT1          0x01
#define TPS6286X_CONTROL        0x03
#define TPS6286X_CONTROL_SWEN   0x20

/* Bound bus ownership independently of unrelated I2C users. The K230
 * master transfer itself has a hardware deadline. */
static rt_size_t tps6286x_transfer(struct rt_i2c_bus_device *bus,
    struct rt_i2c_msg *msgs, rt_uint32_t count)
{
    rt_size_t ret;
    if (!bus || !bus->ops->master_xfer ||
        rt_mutex_take(&bus->lock, rt_tick_from_millisecond(100)) != RT_EOK)
        return 0;
    ret = bus->ops->master_xfer(bus, msgs, count);
    rt_mutex_release(&bus->lock);
    return ret;
}

static int tps6286x_write_reg(struct regulator_dev *rdev, uint8_t reg, uint8_t val)
{
    struct rt_i2c_msg msg;
    uint8_t buf[] = {reg, val};

    msg.addr  = rdev->config->addr;
    msg.flags = RT_I2C_WR;
    msg.buf   = buf;
    msg.len   = sizeof(buf);

    return tps6286x_transfer(rdev->bus, &msg, 1) == 1 ? RT_EOK : -RT_EIO;
}

static int tps6286x_read_reg(struct regulator_dev *rdev, uint8_t reg, uint8_t *val)
{
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = rdev->config->addr;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &reg;
    msgs[0].len   = 1;

    msgs[1].addr  = rdev->config->addr;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = val;
    msgs[1].len   = 1;

    return tps6286x_transfer(rdev->bus, msgs, 2) == 2 ? RT_EOK : -RT_EIO;
}

static int tps6286x_init(struct regulator_dev *rdev)
{
    struct rt_i2c_bus_device *bus;
    int ret;

    bus = (struct rt_i2c_bus_device *)rt_device_find(rdev->config->bus);
    if (bus == RT_NULL) {
        LOG_E("%s: can't find %s", rdev->config->name, rdev->config->bus);
        return -RT_ERROR;
    }
    ret = rt_device_open((rt_device_t)bus, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK) {
        LOG_E("%s: open %s failed: %d", rdev->config->name, rdev->config->bus, ret);
        return -RT_ERROR;
    }
    rdev->bus = bus;
    return RT_EOK;
}

static int tps6286x_set_enable(struct regulator_dev *rdev, bool enable)
{
    uint8_t val;

    if (tps6286x_read_reg(rdev, TPS6286X_CONTROL, &val) != RT_EOK)
        return -RT_EIO;
    val &= ~TPS6286X_CONTROL_SWEN;
    if (enable)
        val |= TPS6286X_CONTROL_SWEN;
    return tps6286x_write_reg(rdev, TPS6286X_CONTROL, val);
}

static int tps6286x_set_selector(struct regulator_dev *rdev, unsigned int sel)
{
    return tps6286x_write_reg(rdev, TPS6286X_VOUT1, (uint8_t)sel);
}

static int tps6286x_get_selector(struct regulator_dev *rdev, unsigned int *sel)
{
    uint8_t val;
    int ret = tps6286x_read_reg(rdev, TPS6286X_VOUT1, &val);

    if (ret == RT_EOK)
        *sel = val;
    return ret;
}

static const struct regulator_ops tps6286x_ops = {
    .init = tps6286x_init,
    .set_enable = tps6286x_set_enable,
    .set_selector = tps6286x_set_selector,
    .get_selector = tps6286x_get_selector,
};

/* VOUT1: 0.4 V to 1.675 V in 5 mV steps. */
const struct regulator_driver regulator_tps6286x_driver = {
    .name = "tps6286x",
    .ops = &tps6286x_ops,
    .min_uv = 400000,
    .step_uv = 5000,
    .n_voltages = 256,
};
