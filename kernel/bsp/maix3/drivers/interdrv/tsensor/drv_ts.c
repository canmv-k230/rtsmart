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

#include "drv_ts.h"

#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>
#include "sysctl_clk.h"
#include "ts_hw.h"

/*
 * RT-Smart device for the temperature sensor, on top of the bare ts_hw
 * driver, which owns the registers.
 */

#define TS_LOCK_TIMEOUT_MS 500

static struct rt_mutex ts_mutex;
static bool ts_ready;
static uint8_t ts_trim = TS_HW_DEFAULT_TRIM;
static uint8_t ts_mode = RT_DEVICE_TS_CTRL_MODE_CONTINUUOS;

static int ts_lock(void)
{
    if (!ts_ready)
        return -RT_ENOSYS;
    return rt_mutex_take(&ts_mutex, rt_tick_from_millisecond(TS_LOCK_TIMEOUT_MS));
}

static int ts_check_mode(uint8_t mode)
{
    return mode == RT_DEVICE_TS_CTRL_MODE_SINGLE ||
        mode == RT_DEVICE_TS_CTRL_MODE_CONTINUUOS ? RT_EOK : -RT_EINVAL;
}

static void ts_sleep_1ms(void)
{
    rt_thread_mdelay(1);
}

/* Call with ts_mutex held. */
static int ts_read_raw(uint16_t *raw)
{
    bool continuous = ts_mode == RT_DEVICE_TS_CTRL_MODE_CONTINUUOS;

    /* A running continuous conversion has a sample pending or delivers one
     * within a conversion period; otherwise restart the sensor. */
    if (ts_hw_enabled() && ts_hw_wait(raw, TS_HW_CONVERSION_MS, ts_sleep_1ms) == RT_EOK)
        return RT_EOK;
    ts_hw_configure(ts_trim, continuous);
    ts_hw_enable();
    if (ts_hw_wait(raw, TS_HW_FIRST_CONVERSION_MS, ts_sleep_1ms) != RT_EOK) {
        ts_hw_disable();
        return -RT_ETIMEOUT;
    }
    if (!continuous) {
        /* Single mode converts once per enable. */
        ts_hw_disable();
        return RT_EOK;
    }
    /* The first conversion after enable may not have settled; use the next. */
    if (ts_hw_wait(raw, TS_HW_CONVERSION_MS, ts_sleep_1ms) != RT_EOK) {
        ts_hw_disable();
        return -RT_ETIMEOUT;
    }
    return RT_EOK;
}

int tsensor_init(void)
{
    int ret = ts_lock();

    if (ret != RT_EOK)
        return ret;
    ts_hw_configure(ts_trim, ts_mode == RT_DEVICE_TS_CTRL_MODE_CONTINUUOS);
    rt_mutex_release(&ts_mutex);
    return RT_EOK;
}

int tsensor_set_trim(uint8_t trim)
{
    int ret;

    if (trim > 15)
        return -RT_EINVAL;
    ret = ts_lock();
    if (ret != RT_EOK)
        return ret;
    if (trim != ts_trim) {
        ts_trim = trim;
        ts_hw_configure(ts_trim, ts_mode == RT_DEVICE_TS_CTRL_MODE_CONTINUUOS);
    }
    rt_mutex_release(&ts_mutex);
    return RT_EOK;
}

int tsensor_set_mode(uint8_t mode)
{
    int ret;

    if (ts_check_mode(mode) != RT_EOK)
        return -RT_EINVAL;
    ret = ts_lock();
    if (ret != RT_EOK)
        return ret;
    if (mode != ts_mode) {
        ts_mode = mode;
        ts_hw_configure(ts_trim, ts_mode == RT_DEVICE_TS_CTRL_MODE_CONTINUUOS);
    }
    rt_mutex_release(&ts_mutex);
    return RT_EOK;
}

/* raw_out and ctrl_out are optional diagnostics for chip_temp. */
static int ts_read(double *temp, uint16_t *raw_out, uint32_t *ctrl_out)
{
    static bool out_of_range_reported;
    uint16_t raw = 0;
    uint32_t ctrl;
    int32_t milli;
    int ret;

    if (temp == RT_NULL)
        return -RT_EINVAL;
    ret = ts_lock();
    if (ret != RT_EOK)
        return ret;
    ret = ts_read_raw(&raw);
    if (ret == RT_EOK) {
        ctrl = ts_hw_ctrl();
        milli = ts_hw_raw_to_mc(raw);
        /* Report the first out-of-range reading; return such readings as
         * they are. */
        if (milli < -40000 || milli > 125000) {
            if (!out_of_range_reported)
                rt_kprintf("tsensor: outside -40..125 C: raw=%u ctrl=0x%02x clock=%u Hz\n",
                    (unsigned)raw, (unsigned)ctrl,
                    (unsigned)sysctl_clk_get_leaf_freq(SYSCTL_CLK_TS_CLK));
            out_of_range_reported = true;
        } else {
            out_of_range_reported = false;
        }
        *temp = milli / 1000.0;
        if (raw_out != RT_NULL)
            *raw_out = raw;
        if (ctrl_out != RT_NULL)
            *ctrl_out = ctrl;
    }
    rt_mutex_release(&ts_mutex);
    return ret;
}

int tsensor_read_temp(double *temp)
{
    return ts_read(temp, RT_NULL, RT_NULL);
}

uint8_t tsensor_get_trim(void)
{
    return ts_trim;
}

uint8_t tsensor_get_mode(void)
{
    return ts_mode;
}

#ifdef DRV_TS_USE_RT_DEVICE
static rt_err_t ts_device_open(rt_device_t dev, rt_uint16_t oflag)
{
    /* RT-device owns client references; opening does not touch registers. */
    return ts_ready ? RT_EOK : -RT_ENOSYS;
}

static rt_err_t ts_device_close(rt_device_t dev)
{
    /* rt_device_close() calls this only when the last reference is dropped;
     * the last client stops a continuous conversion. */
    int ret = ts_lock();

    if (ret != RT_EOK)
        return ret;
    ts_hw_disable();
    rt_mutex_release(&ts_mutex);
    return RT_EOK;
}

static rt_size_t ts_device_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    double temp;

    /* The caller's buffer need not be aligned for a double. */
    if (buffer == RT_NULL || size != sizeof(temp) || tsensor_read_temp(&temp) != RT_EOK)
        return 0;
    rt_memcpy(buffer, &temp, sizeof(temp));
    return sizeof(temp);
}

static rt_err_t ts_device_control(rt_device_t dev, int cmd, void *args)
{
    if (args == RT_NULL)
        return -RT_EINVAL;
    switch (cmd) {
    case RT_DEVICE_TS_CTRL_SET_MODE:
        return tsensor_set_mode(*(uint8_t *)args);
    case RT_DEVICE_TS_CTRL_SET_TRIM:
        return tsensor_set_trim(*(uint8_t *)args);
    case RT_DEVICE_TS_CTRL_GET_MODE:
        *(uint8_t *)args = tsensor_get_mode();
        return RT_EOK;
    case RT_DEVICE_TS_CTRL_GET_TRIM:
        *(uint8_t *)args = tsensor_get_trim();
        return RT_EOK;
    default:
        return -RT_EINVAL;
    }
}

static struct rt_device ts_device;
#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops ts_ops = {
    .open    = ts_device_open,
    .close   = ts_device_close,
    .read    = ts_device_read,
    .control = ts_device_control,
};
#endif
#endif

static int rt_hw_ts_init(void)
{
    int ret = ts_hw_init();

    if (ret != RT_EOK)
        return ret;
    ret = rt_mutex_init(&ts_mutex, "dev_ts", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
        return ret;
    /* Stop whatever earlier boot stages left running. */
    ts_hw_configure(ts_trim, ts_mode == RT_DEVICE_TS_CTRL_MODE_CONTINUUOS);
    ts_ready = true;
#ifdef DRV_TS_USE_RT_DEVICE
#ifdef RT_USING_DEVICE_OPS
    ts_device.ops = &ts_ops;
#else
    ts_device.open = ts_device_open;
    ts_device.close = ts_device_close;
    ts_device.read = ts_device_read;
    ts_device.control = ts_device_control;
#endif
    ret = rt_device_register(&ts_device, "ts", RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK)
        return ret;
#endif
    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_ts_init);

#ifdef RT_USING_MSH
static void chip_temp(void)
{
    double temp;
    int32_t milli;
    uint32_t magnitude;
    uint16_t raw;
    uint32_t ctrl;
    int ret = ts_read(&temp, &raw, &ctrl);

    if (ret != RT_EOK) {
        rt_kprintf("chip temperature unavailable: %d\n", ret);
        return;
    }
    milli = (int32_t)(temp * 1000.0 + (temp < 0.0 ? -0.5 : 0.5));
    magnitude = milli < 0 ? (uint32_t)-milli : (uint32_t)milli;
    rt_kprintf("chip temperature: %s%u.%03u C\n", milli < 0 ? "-" : "",
        magnitude / 1000, magnitude % 1000);
    rt_kprintf("tsensor: raw=%u (0x%03x) ctrl=0x%02x trim=%u mode=%s clock=%u Hz\n",
        (unsigned)raw, (unsigned)raw, (unsigned)ctrl, (unsigned)ts_trim,
        ts_mode == RT_DEVICE_TS_CTRL_MODE_CONTINUUOS ? "continuous" : "single",
        (unsigned)sysctl_clk_get_leaf_freq(SYSCTL_CLK_TS_CLK));
}
MSH_CMD_EXPORT(chip_temp, dump chip temperature);
#endif
