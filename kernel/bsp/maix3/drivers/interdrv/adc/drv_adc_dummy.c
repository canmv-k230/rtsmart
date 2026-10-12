/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "rtdef.h"
#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/adc.h>

static struct rt_adc_device k230_adc_dummy_device;

static rt_err_t k230_adc_dummy_enabled(struct rt_adc_device *device,
                                       rt_uint32_t channel,
                                       rt_bool_t enabled)
{
    (void)device;
    (void)channel;
    (void)enabled;

    return -RT_ENOSYS;
}

static rt_err_t k230_adc_dummy_convert(struct rt_adc_device *device,
                                       rt_uint32_t channel,
                                       rt_uint32_t *value)
{
    (void)device;
    (void)channel;

    if (value != RT_NULL)
        *value = 0;

    return -RT_ENOSYS;
}

static const struct rt_adc_ops k230_adc_dummy_ops = {
    .enabled = k230_adc_dummy_enabled,
    .convert = k230_adc_dummy_convert,
};

rt_bool_t k230_adc_acodec_hp_detect_enabled(void)
{
    return RT_FALSE;
}

rt_uint32_t k230_adc_acodec_hp_detect_channel(void)
{
    return -1U;
}

static int k230_adc_dummy_init(void)
{
    return rt_hw_adc_register(&k230_adc_dummy_device, "adc",
                              &k230_adc_dummy_ops, RT_NULL);
}
INIT_DEVICE_EXPORT(k230_adc_dummy_init);
