/* Copyright 2020 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "utest.h"
#include "drv_ts.h"
#ifdef K230_PM_THERMAL_PROTECT
#include "k230_pm_thermal.h"
#endif

#if defined(DRV_TS_USE_RT_DEVICE) && defined(K230_PM_THERMAL_PROTECT)
/* The monitor keeps sampling: wait for a fresh reading after the given one.
 * An idle monitor samples every 10 s, so allow 15 s. */
static bool ts_new_reading(uint32_t sequence)
{
    struct k230_thermal_reading reading;

    for (int waited = 0; waited < 15000; waited += 100) {
        if (k230_pm_thermal_reading(&reading) == RT_EOK && reading.sequence != sequence &&
            reading.valid && !reading.stale)
            return true;
        rt_thread_mdelay(100);
    }
    return false;
}

static struct rt_semaphore readers_done;
static int reader_errors[2];
static volatile bool reader_finished[2];
static void ts_reader(void *argument)
{
    unsigned index = (unsigned)(rt_ubase_t)argument;
    rt_device_t dev = rt_device_find("ts");
    for (unsigned i = 0; i < 16; ++i) {
        double temp;
        if (!dev || rt_device_open(dev, RT_DEVICE_OFLAG_RDWR) != RT_EOK) {
            reader_errors[index]++;
            break;
        }
        if (rt_device_read(dev, 0, &temp, sizeof(temp)) != sizeof(temp))
            reader_errors[index]++;
        if (rt_device_close(dev) != RT_EOK)
            reader_errors[index]++;
        rt_thread_mdelay(100);
    }
    reader_finished[index] = true;
    rt_sem_release(&readers_done);
}

static void test_ts_concurrent_readers(void)
{
    struct k230_thermal_reading before;
    rt_thread_t readers[2];
    unsigned started = 0;
    rt_sem_init(&readers_done, "tsread", 0, RT_IPC_FLAG_PRIO);
    k230_pm_thermal_reading(&before);
    for (unsigned i = 0; i < 2; ++i) {
        reader_errors[i] = 0;
        reader_finished[i] = false;
        readers[i] = rt_thread_create("ts_client", ts_reader,
            (void *)(rt_ubase_t)i, 2048, 16, 10);
        uassert_true(readers[i] != RT_NULL);
        if (readers[i] && rt_thread_startup(readers[i]) == RT_EOK)
            started++;
    }
    for (unsigned i = 0; i < started; ++i)
        uassert_int_equal(rt_sem_take(&readers_done, rt_tick_from_millisecond(5000)), RT_EOK);
    for (unsigned i = 0; i < 2; ++i) {
        if (readers[i] && !reader_finished[i])
            rt_thread_delete(readers[i]);
    }
    uassert_int_equal(reader_errors[0] + reader_errors[1], 0);
    uassert_true(ts_new_reading(before.sequence));
    rt_sem_detach(&readers_done);
}
#endif

#ifdef DRV_TS_USE_RT_DEVICE
static void test_ts_read(void)
{
    rt_device_t dev = rt_device_find("ts");
    double temp;
    uint8_t mode, trim;
    uassert_true(dev != RT_NULL);
    if (!dev) return;
    uassert_int_equal(rt_device_open(dev, RT_DEVICE_OFLAG_RDWR), RT_EOK);
    uassert_int_equal(rt_device_control(dev, RT_DEVICE_TS_CTRL_GET_MODE, &mode), RT_EOK);
    uassert_int_equal(rt_device_control(dev, RT_DEVICE_TS_CTRL_GET_TRIM, &trim), RT_EOK);
    uassert_int_equal(rt_device_control(dev, RT_DEVICE_TS_CTRL_SET_MODE, RT_NULL), -RT_EINVAL);
    uassert_int_equal(rt_device_control(dev, 0x7fff, &mode), -RT_EINVAL);
#ifdef K230_PM_THERMAL_PROTECT
    {
        struct k230_thermal_reading before;
        uint8_t other_mode = RT_DEVICE_TS_CTRL_MODE_CONTINUUOS;
        uint8_t other_trim = (trim + 1) & 15;
        uassert_int_equal(mode, RT_DEVICE_TS_CTRL_MODE_SINGLE);
        uassert_int_equal(rt_device_control(dev, RT_DEVICE_TS_CTRL_SET_MODE, &mode), RT_EOK);
        uassert_int_equal(rt_device_control(dev, RT_DEVICE_TS_CTRL_SET_MODE, &other_mode), -RT_EBUSY);
        uassert_int_equal(rt_device_control(dev, RT_DEVICE_TS_CTRL_SET_TRIM, &other_trim), -RT_EBUSY);
        /* Startup cache readiness is intentionally asynchronous. */
        rt_thread_mdelay(1500);
        uassert_int_equal(k230_pm_thermal_reading(&before), RT_EOK);
        for (int i = 0; i < 10; ++i) {
            uassert_int_equal(rt_device_read(dev, 0, &temp, sizeof(temp)), sizeof(temp));
            uassert_int_equal(rt_device_close(dev), RT_EOK);
            uassert_int_equal(rt_device_open(dev, RT_DEVICE_OFLAG_RDWR), RT_EOK);
            rt_thread_mdelay(150);
        }
        uassert_true(ts_new_reading(before.sequence));
    }
#else
    {
        const uint8_t modes[] = { RT_DEVICE_TS_CTRL_MODE_CONTINUUOS,
            RT_DEVICE_TS_CTRL_MODE_SINGLE, RT_DEVICE_TS_CTRL_MODE_CONTINUUOS };
        for (unsigned i = 0; i < sizeof(modes); ++i) {
            uint8_t setting = modes[i];
            uassert_int_equal(rt_device_control(dev, RT_DEVICE_TS_CTRL_SET_MODE, &setting), RT_EOK);
            for (int j = 0; j < 3; ++j)
                uassert_int_equal(rt_device_read(dev, 0, &temp, sizeof(temp)), sizeof(temp));
        }
        uassert_int_equal(rt_device_control(dev, RT_DEVICE_TS_CTRL_SET_MODE, &mode), RT_EOK);
    }
#endif
    uassert_int_equal(rt_device_close(dev), RT_EOK);
}
#endif

static void testcase(void)
{
    uassert_int_equal(tsensor_read_temp(RT_NULL), -RT_EINVAL);
    uassert_int_equal(tsensor_set_mode(0xff), -RT_EINVAL);
    uassert_int_equal(tsensor_set_trim(16), -RT_EINVAL);
#ifdef DRV_TS_USE_RT_DEVICE
    UTEST_UNIT_RUN(test_ts_read);
#ifdef K230_PM_THERMAL_PROTECT
    UTEST_UNIT_RUN(test_ts_concurrent_readers);
#endif
#endif
}
UTEST_TC_EXPORT(testcase, "testcases.kernel.ts_tc", RT_NULL, RT_NULL, 100);
