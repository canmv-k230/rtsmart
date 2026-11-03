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
#endif
}
UTEST_TC_EXPORT(testcase, "testcases.kernel.ts_tc", RT_NULL, RT_NULL, 100);
