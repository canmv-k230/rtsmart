/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <rtthread.h>
#include "k230_pm.h"
#include "k230_pm_dvfs.h"
#include "drv_ts.h"

#define THERMAL_DETECT_POLL_TIME 5000
#define THERMAL_SENSOR_MAX_FAILS 3
#define THERMAL_THREAD_PRIORITY (RT_THREAD_PRIORITY_MAX - 8)

static bool thermal_thread_started;

static int32_t k230_pm_thermal_get_temperature(void)
{
    double temp = 0.0f;
    static int32_t last_temp;
    static uint32_t fail_count;
    static bool last_temp_valid;

    if (tsensor_read_temp(&temp) != 0) {
        fail_count++;
        if (last_temp_valid && fail_count < THERMAL_SENSOR_MAX_FAILS)
            return last_temp;

        return k230_pm_dvfs_thermal_shutdown_temp() + 1;
    }

    fail_count = 0;
    last_temp = (int32_t)temp;
    last_temp_valid = true;
    return last_temp;
}

static void k230_pm_thermal_shutdown(void)
{
    (void)k230_pm_emergency_off(K230_PM_DEV_AI);
    (void)k230_pm_emergency_off(K230_PM_DEV_DPU);
    (void)k230_pm_emergency_off(K230_PM_DEV_VPU);
}

static void k230_pm_thermal_thread(void *arg)
{
    (void)arg;

    tsensor_init();

    while (1) {
        int32_t temp;

        temp = k230_pm_thermal_get_temperature();

        k230_pm_dvfs_thermal_update(K230_PM_DOMAIN_CPU, temp);
        k230_pm_dvfs_thermal_update(K230_PM_DOMAIN_KPU, temp);

        if (temp > k230_pm_dvfs_thermal_shutdown_temp())
            k230_pm_thermal_shutdown();

        rt_thread_mdelay(THERMAL_DETECT_POLL_TIME);
    }
}

int k230_pm_thermal_init(void)
{
    rt_thread_t tid;
    rt_err_t ret;

    if (thermal_thread_started)
        return 0;

    tid = rt_thread_create("thermal_detect_thread", k230_pm_thermal_thread,
        RT_NULL, 1024 * 10, THERMAL_THREAD_PRIORITY,
        rt_tick_from_millisecond(10));
    if (tid == RT_NULL)
        return K230_ERR_PM_NOMEM;

    ret = rt_thread_startup(tid);
    if (ret != RT_EOK) {
        rt_thread_delete(tid);
        return K230_ERR_PM_NOTREADY;
    }
    thermal_thread_started = true;
    return 0;
}
