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

#include "board.h"
#include "ioremap.h"
#include "sysctl_clk.h"
#include <riscv_io.h>

#if defined(RT_USING_MSH)
#include <finsh.h>
#endif

/* sys_ctl ts_controller: TSENW configures the sensor, TSENR holds the
 * conversion latched on each DATA_VALID rising edge. */
#define REG_TSENW 0x000
#define REG_TSENR 0x004

#define TSENW_EN         (1U << 0)
#define TSENW_CONTINUOUS (1U << 1)
#define TSENW_TRIM_POS   2
#define TSENW_TRIM_MASK  (0xFU << TSENW_TRIM_POS)
#define TSENW_TEST_EN    (1U << 6) /* analog test output; must stay 0 */

#define TSENR_DOUT_MASK  0xFFFU
#define TSENR_DOUT_VALID (1U << 12) /* cleared by reading TSENR */

/* The first conversion after EN takes 1305 clocks, later ones 256 clocks:
 * 13.05 ms and 2.56 ms at the default 100 kHz, 43.5 ms and 8.5 ms at the
 * sensor's 30 kHz minimum. */
#define TS_FIRST_CONVERSION_TIMEOUT_MS 50
#define TS_CONVERSION_TIMEOUT_MS       10
#define TS_LOCK_TIMEOUT_MS             500

static struct rt_mutex ts_mutex;
static void*           ts_base;
static uint8_t         ts_trim = 8;
static uint8_t         ts_mode = RT_DEVICE_TS_CTRL_MODE_CONTINUUOS;

static uint32_t ts_readl(uint32_t offset)
{
    return readl(ts_base + offset);
}

static void ts_writel(uint32_t value, uint32_t offset)
{
    writel(value, ts_base + offset);
}

static rt_bool_t ts_lock(void)
{
    if (rt_mutex_take(&ts_mutex, rt_tick_from_millisecond(TS_LOCK_TIMEOUT_MS)) == RT_EOK)
        return RT_TRUE;
    rt_kprintf("tsensor: lock timeout\n");
    return RT_FALSE;
}

/* Call with ts_mutex held. Leaves the sensor disabled: MODE, TRIM and
 * TEST_EN may change only while EN is low. */
static void ts_configure(void)
{
    uint32_t ctrl = ts_readl(REG_TSENW) & ~(TSENW_EN | TSENW_TEST_EN);

    ts_writel(ctrl, REG_TSENW);
    ctrl &= ~(TSENW_TRIM_MASK | TSENW_CONTINUOUS);
    ctrl |= (uint32_t)ts_trim << TSENW_TRIM_POS;
    if (ts_mode == RT_DEVICE_TS_CTRL_MODE_CONTINUUOS)
        ctrl |= TSENW_CONTINUOUS;
    ts_writel(ctrl, REG_TSENW);
    /* Clear a valid flag left by the previous configuration. */
    (void)ts_readl(REG_TSENR);
}

static void ts_enable(void)
{
    ts_configure();
    ts_writel(ts_readl(REG_TSENW) | TSENW_EN, REG_TSENW);
}

static void ts_disable(void)
{
    ts_writel(ts_readl(REG_TSENW) & ~TSENW_EN, REG_TSENW);
}

/* VALID clears on read, so test it and take DOUT from the same read. */
static int ts_wait_sample(uint16_t* raw, uint32_t timeout_ms)
{
    for (uint32_t waited = 0;; waited++) {
        uint32_t value = ts_readl(REG_TSENR);

        if (value & TSENR_DOUT_VALID) {
            *raw = value & TSENR_DOUT_MASK;
            return 0;
        }
        if (waited == timeout_ms)
            return -RT_ETIMEOUT;
        rt_thread_mdelay(1);
    }
}

/* Call with ts_mutex held. */
static int ts_read_raw(uint16_t* raw)
{
    /* A running continuous conversion has a sample pending or delivers one
     * within a conversion period; otherwise restart the sensor. */
    if ((ts_readl(REG_TSENW) & TSENW_EN) && ts_wait_sample(raw, TS_CONVERSION_TIMEOUT_MS) == 0)
        return 0;

    ts_enable();
    if (ts_wait_sample(raw, TS_FIRST_CONVERSION_TIMEOUT_MS) != 0) {
        ts_disable();
        return -RT_ETIMEOUT;
    }
    if (ts_mode == RT_DEVICE_TS_CTRL_MODE_SINGLE) {
        /* Single mode converts once per enable. */
        ts_disable();
        return 0;
    }
    /* The first conversion after enable may not have settled; use the next. */
    if (ts_wait_sample(raw, TS_CONVERSION_TIMEOUT_MS) != 0) {
        ts_disable();
        return -RT_ETIMEOUT;
    }
    return 0;
}

/* Calibration polynomial from the Kendryte SDK. */
static double ts_raw_to_celsius(uint16_t raw)
{
    double x = raw;

    return (((1.01472e-10 * x - 1.10063e-6) * x + 4.36150e-3) * x - 7.10128) * x + 3565.87;
}

static int ts_read(double* temp, uint16_t* raw_out, uint32_t* ctrl_out)
{
    static rt_bool_t out_of_range_reported;
    uint16_t         raw;
    uint32_t         ctrl;
    int              result;

    if (temp == RT_NULL || ts_base == RT_NULL)
        return -1;
    *temp = 0.0;
    if (!ts_lock())
        return -1;

    result = ts_read_raw(&raw);
    if (result == 0) {
        ctrl  = ts_readl(REG_TSENW);
        *temp = ts_raw_to_celsius(raw);
        if (raw_out != RT_NULL)
            *raw_out = raw;
        if (ctrl_out != RT_NULL)
            *ctrl_out = ctrl;

        /* Report out-of-range readings to thermal protection unchanged;
         * do not clamp them to a safe temperature. */
        if (*temp < -40.0 || *temp > 125.0) {
            if (!out_of_range_reported)
                rt_kprintf("tsensor: outside -40..125 C: raw=%u ctrl=0x%02x clock=%u Hz\n", (unsigned)raw,
                           (unsigned)ctrl, (unsigned)sysctl_clk_get_leaf_freq(SYSCTL_CLK_TS_CLK));
            out_of_range_reported = RT_TRUE;
        } else {
            out_of_range_reported = RT_FALSE;
        }
    }

    rt_mutex_release(&ts_mutex);
    return result == 0 ? 0 : -1;
}

void tsensor_init(void)
{
    if (ts_base == RT_NULL || !ts_lock())
        return;
    ts_configure();
    rt_mutex_release(&ts_mutex);
}

void tsensor_set_trim(uint8_t trim)
{
    if (ts_base == RT_NULL || !ts_lock())
        return;
    ts_trim = trim & 0xF;
    ts_configure();
    rt_mutex_release(&ts_mutex);
}

uint8_t tsensor_get_trim(void)
{
    return ts_trim;
}

void tsensor_set_mode(uint8_t mode)
{
    if (mode != RT_DEVICE_TS_CTRL_MODE_SINGLE && mode != RT_DEVICE_TS_CTRL_MODE_CONTINUUOS)
        return;
    if (ts_base == RT_NULL || !ts_lock())
        return;
    ts_mode = mode;
    ts_configure();
    rt_mutex_release(&ts_mutex);
}

uint8_t tsensor_get_mode(void)
{
    return ts_mode;
}

int tsensor_read_temp(double* temp)
{
    return ts_read(temp, RT_NULL, RT_NULL);
}

#if defined(DRV_TS_USE_RT_DEVICE)
static rt_size_t ts_device_read(rt_device_t dev, rt_off_t pos, void* buffer, rt_size_t size)
{
    double temp;

    if (buffer == RT_NULL || size != sizeof(temp) || tsensor_read_temp(&temp) != 0)
        return 0;
    rt_memcpy(buffer, &temp, sizeof(temp));
    return sizeof(temp);
}

static rt_err_t ts_device_control(rt_device_t dev, int cmd, void* args)
{
    rt_uint8_t* value = args;

    if (value == RT_NULL)
        return -RT_EINVAL;

    switch (cmd) {
    case RT_DEVICE_TS_CTRL_SET_MODE:
        if (*value != RT_DEVICE_TS_CTRL_MODE_SINGLE && *value != RT_DEVICE_TS_CTRL_MODE_CONTINUUOS)
            return -RT_EINVAL;
        tsensor_set_mode(*value);
        return RT_EOK;
    case RT_DEVICE_TS_CTRL_GET_MODE:
        *value = tsensor_get_mode();
        return RT_EOK;
    case RT_DEVICE_TS_CTRL_SET_TRIM:
        if (*value > 0xF)
            return -RT_EINVAL;
        tsensor_set_trim(*value);
        return RT_EOK;
    case RT_DEVICE_TS_CTRL_GET_TRIM:
        *value = tsensor_get_trim();
        return RT_EOK;
    default:
        return -RT_EINVAL;
    }
}

static struct rt_device ts_device;

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops ts_ops = {
    .read    = ts_device_read,
    .control = ts_device_control,
};
#endif

static int register_ts_device(void)
{
#ifdef RT_USING_DEVICE_OPS
    ts_device.ops = &ts_ops;
#else
    ts_device.read    = ts_device_read;
    ts_device.control = ts_device_control;
#endif

    if (rt_device_register(&ts_device, "ts", RT_DEVICE_FLAG_RDWR) != RT_EOK) {
        rt_kprintf("ts device register fail\n");
        return -1;
    }
    return 0;
}
#endif // DRV_TS_USE_RT_DEVICE

static int rt_hw_ts_init(void)
{
    ts_base = rt_ioremap((void*)TS_BASE_ADDR, TS_IO_SIZE);
    if (ts_base == RT_NULL) {
        rt_kprintf("ts ioremap error\n");
        return -1;
    }

    if (rt_mutex_init(&ts_mutex, "dev_ts", RT_IPC_FLAG_PRIO) != RT_EOK) {
        rt_kprintf("ts init mutex error\n");
        return -3;
    }

    tsensor_init();

#if defined(DRV_TS_USE_RT_DEVICE)
    if (register_ts_device() != 0)
        return -2;
#endif

    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_ts_init);

#if defined(RT_USING_MSH)
static void chip_temp(void)
{
    double   temp;
    uint16_t raw;
    uint32_t ctrl;
    int32_t  milli;
    uint32_t magnitude;

    if (ts_read(&temp, &raw, &ctrl) != 0) {
        rt_kprintf("read chip temperature failed\n");
        return;
    }

    milli     = (int32_t)(temp * 1000.0 + (temp < 0.0 ? -0.5 : 0.5));
    magnitude = milli < 0 ? (uint32_t)-milli : (uint32_t)milli;
    rt_kprintf("chip temperature: %s%u.%03u C\n", milli < 0 ? "-" : "", magnitude / 1000, magnitude % 1000);
    rt_kprintf("tsensor: raw=%u (0x%03x) ctrl=0x%02x trim=%u mode=%s clock=%u Hz\n", (unsigned)raw, (unsigned)raw,
               (unsigned)ctrl, (unsigned)((ctrl & TSENW_TRIM_MASK) >> TSENW_TRIM_POS),
               (ctrl & TSENW_CONTINUOUS) ? "continuous" : "single",
               (unsigned)sysctl_clk_get_leaf_freq(SYSCTL_CLK_TS_CLK));
}
MSH_CMD_EXPORT(chip_temp, dump chip temperature);
#endif
