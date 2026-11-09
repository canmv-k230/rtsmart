#!/usr/bin/env python3
"""Run the bare sensor driver, its RT-Smart device and the thermal reading
cache against a read-clear register model.

Run with python3 -B kernel/bsp/maix3/testcase/host/test_tsensor.py.
No board or cross compiler is required.
"""

from pathlib import Path
import os
import subprocess
import tempfile


DRIVERS = Path(__file__).resolve().parents[2] / "drivers/interdrv"

STUBS = r"""
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

typedef uint32_t rt_tick_t;
typedef int rt_base_t;
typedef int rt_spinlock_t;
typedef int rt_bool_t;
typedef int rt_err_t;
typedef uint16_t rt_uint16_t;
typedef size_t rt_size_t;
typedef long rt_off_t;
typedef void *rt_thread_t;
#define RT_NULL NULL
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_EOK 0
#define RT_ETIMEOUT 2
#define RT_EEMPTY 4
#define RT_ENOMEM 5
#define RT_ENOSYS 6
#define RT_EBUSY 7
#define RT_EINVAL 10
#define RT_IPC_FLAG_PRIO 0
#define RT_DEVICE_FLAG_RDWR 3
#define TS_BASE_ADDR 0x1000
#define TS_IO_SIZE 8
#define SYSCTL_CLK_TS_CLK 0
#define TIMER_CLK_FREQ 1000000
#define RT_TICK_PER_SECOND TEST_HZ
#define INIT_DEVICE_EXPORT(fn)
#define MSH_CMD_EXPORT(fn, desc)

struct rt_mutex { bool held; };
struct rt_device;
typedef struct rt_device *rt_device_t;
struct rt_device_ops {
    rt_err_t (*init)(rt_device_t);
    rt_err_t (*open)(rt_device_t, rt_uint16_t);
    rt_err_t (*close)(rt_device_t);
    rt_size_t (*read)(rt_device_t, rt_off_t, void *, rt_size_t);
    rt_size_t (*write)(rt_device_t, rt_off_t, const void *, rt_size_t);
    rt_err_t (*control)(rt_device_t, int, void *);
};
struct rt_device {
    const struct rt_device_ops *ops;
    rt_err_t (*open)(rt_device_t, rt_uint16_t);
    rt_err_t (*close)(rt_device_t);
    rt_size_t (*read)(rt_device_t, rt_off_t, void *, rt_size_t);
    rt_err_t (*control)(rt_device_t, int, void *);
};

static uint64_t now_us, next_conversion;
static uint32_t ctrl, data;
static uint16_t next_raw = 2000;
static unsigned first_us = 43500, period_us = 8534;
static unsigned shots, conversions, reads, writes, sleeps, kicks;
static bool no_conversion, terminal;
static rt_thread_t current_thread = (void *)1;

static rt_tick_t rt_tick_get(void) { return now_us * TEST_HZ / 1000000; }
static rt_tick_t rt_tick_from_millisecond(unsigned ms) {
    return ((uint64_t)ms * TEST_HZ + 999) / 1000;
}
static void rt_thread_mdelay(unsigned ms) {
    assert(!terminal);
    sleeps++;
    now_us += (uint64_t)rt_tick_from_millisecond(ms) * 1000000 / TEST_HZ;
}
static uint64_t cpu_ticks(void) { now_us += 100; return now_us; }
static rt_thread_t rt_thread_self(void) { return current_thread; }
static int rt_mutex_init(struct rt_mutex *m, const char *name, int flag) {
    m->held = false;
    return 0;
}
static int rt_mutex_take(struct rt_mutex *m, rt_tick_t timeout) {
    assert(!terminal);
    if (m->held) return -RT_ETIMEOUT;
    m->held = true;
    return 0;
}
static int rt_mutex_release(struct rt_mutex *m) {
    assert(m->held);
    m->held = false;
    return 0;
}
static void rt_spin_lock_init(rt_spinlock_t *lock) { *lock = 0; }
static int rt_spin_lock_irqsave(rt_spinlock_t *lock) {
    assert(!*lock);
    *lock = 1;
    return 0;
}
static void rt_spin_unlock_irqrestore(rt_spinlock_t *lock, int level) {
    assert(*lock);
    *lock = 0;
}
static int rt_device_register(rt_device_t dev, const char *name, unsigned flags) { return 0; }
static void *rt_ioremap(void *address, unsigned size) { return address; }
static unsigned sysctl_clk_get_leaf_freq(int clock) { return 30000; }
static void rt_kprintf(const char *fmt, ...) {}
#define rt_memcpy memcpy

/* TSENR's valid indication clears on read. Single mode converts once per
 * enable; continuous mode retains EN and produces later conversions. */
static uint32_t readl(const void *address) {
    reads++;
    if ((uintptr_t)address == TS_BASE_ADDR) return ctrl;
    assert((uintptr_t)address == TS_BASE_ADDR + 4);
    if ((ctrl & 1) && !no_conversion && now_us >= next_conversion) {
        data = (1U << 12) | next_raw++;
        conversions++;
        next_conversion = (ctrl & 2) ? now_us + period_us : UINT64_MAX;
    }
    uint32_t result = data;
    data &= ~(1U << 12);
    return result;
}
static void writel(uint32_t value, void *address) {
    assert((uintptr_t)address == TS_BASE_ADDR);
    writes++;
    /* MODE/TRIM must not change while the old configuration is enabled. */
    if (ctrl & 1) assert(((ctrl ^ value) & 0x3e) == 0);
    assert(!(value & (1U << 6)));
    if ((value & 1) && !(ctrl & 1)) {
        next_conversion = now_us + first_us;
        shots++;
    }
    ctrl = value;
}
"""

CASES = r"""
static void sleep_1ms(void) { rt_thread_mdelay(1); }
/* Terminal waits: no scheduler, the watchdog is serviced. */
static void busy_1ms(void) { kicks++; now_us += 1000; }

int main(void)
{
    double temp;
    int32_t milli;
    char unaligned[sizeof(double) + 1];
    (void)unaligned;

    /* The bare driver does nothing before its registers are mapped. */
    ts_hw_configure(3, true);
    ts_hw_enable();
    assert(writes == 0 && reads == 0 && !ts_hw_enabled());
    assert(ts_hw_read_single(&milli, sleep_1ms) == -RT_ENOSYS);
    assert(tsensor_read_temp(NULL) == -RT_EINVAL);
    assert(tsensor_read_temp(&temp) == -RT_ENOSYS);

    /* Bootloader left another mode/trim, test output, and a stale sample. */
    ctrl = 1 | 2 | (3 << 2) | (1 << 6);
    data = (1 << 12) | 4095;
    assert(rt_hw_ts_init() == 0);
    assert(tsensor_set_mode(0xff) == -RT_EINVAL);
    assert(tsensor_set_trim(16) == -RT_EINVAL);

#ifndef K230_PM_THERMAL_PROTECT
    /* The device stops what boot left running. Continuous startup discards
     * the first conversion; further reads keep the sensor running; single
     * mode starts one conversion per read. */
    assert(!(ctrl & 1) && !(ctrl & (1 << 6)));
    assert(tsensor_read_temp(&temp) == 0);
    assert(shots == 1 && conversions == 2 && (ctrl & 1));
    assert(tsensor_read_temp(&temp) == 0);
    assert(shots == 1 && conversions == 3);
    assert(tsensor_set_mode(RT_DEVICE_TS_CTRL_MODE_SINGLE) == 0);
    assert(tsensor_read_temp(&temp) == 0);
    assert(shots == 2 && conversions == 4 && !(ctrl & 1));
    assert(tsensor_set_trim(7) == 0 && tsensor_get_trim() == 7);
    assert(((ctrl >> 2) & 15) == 7);
    assert(tsensor_set_mode(RT_DEVICE_TS_CTRL_MODE_CONTINUUOS) == 0);
    assert(tsensor_read_temp(&temp) == 0);
    assert(shots == 3 && conversions == 6 && (ctrl & 1));
    /* An out-of-range conversion is returned unclamped. */
    next_raw = 0;
    assert(tsensor_set_mode(RT_DEVICE_TS_CTRL_MODE_SINGLE) == 0);
    assert(tsensor_read_temp(&temp) == 0 && temp > 125.0);
    no_conversion = true;
    assert(tsensor_read_temp(&temp) == -RT_ETIMEOUT && !(ctrl & 1));
    no_conversion = false;
    next_raw = 2000;
#ifdef DRV_TS_USE_RT_DEVICE
    assert(ts_device_open(&ts_device, 0) == 0);
    assert(ts_device_read(&ts_device, 0, unaligned + 1, sizeof(double)) == sizeof(double));
    memcpy(&temp, unaligned + 1, sizeof(temp));
    assert(temp == ts_hw_raw_to_mc(2000) / 1000.0);
    assert(ts_device_read(&ts_device, 0, NULL, sizeof(double)) == 0);
    assert(ts_device_close(&ts_device) == 0 && !(ctrl & 1));
#endif
#else
    /* The monitor owns the sensor: the device leaves it alone, keeps the
     * monitor's configuration, and has no reading until the monitor runs. */
    assert(writes == 0);
    assert(tsensor_get_mode() == RT_DEVICE_TS_CTRL_MODE_SINGLE);
    assert(tsensor_get_trim() == TS_HW_DEFAULT_TRIM);
    assert(tsensor_set_mode(RT_DEVICE_TS_CTRL_MODE_CONTINUUOS) == -RT_EBUSY);
    assert(tsensor_set_mode(RT_DEVICE_TS_CTRL_MODE_SINGLE) == 0);
    assert(tsensor_set_trim(7) == -RT_EBUSY);
    assert(tsensor_set_trim(TS_HW_DEFAULT_TRIM) == 0);
    assert(tsensor_read_temp(&temp) == -RT_ENOSYS);
    k230_thermal_reading_init();
    assert(tsensor_read_temp(&temp) == -RT_EEMPTY);
    assert(writes == 0);

    /* Monitor side: single mode, one conversion per read, left disabled. */
    struct k230_thermal_reading before, after;
    ts_hw_configure(TS_HW_DEFAULT_TRIM, false);
    assert(!(ctrl & 1) && !(ctrl & 2) && !(ctrl & (1 << 6)) && ((ctrl >> 2) & 15) == 8);
    uint16_t expected_raw = next_raw;
    assert(ts_hw_read_single(&milli, sleep_1ms) == 0);
    assert(shots == 1 && !(ctrl & 1) && milli == ts_hw_raw_to_mc(expected_raw));
    k230_thermal_reading_publish(0, milli);
    assert(k230_pm_thermal_reading(&before) == 0 && before.valid && !before.stale);
    assert(before.millicelsius == milli && before.sequence == 1);

    /* Readers take the monitor's reading without touching registers. */
    unsigned old_reads = reads, old_writes = writes;
    assert(tsensor_read_temp(&temp) == 0 && temp == milli / 1000.0);
#ifdef DRV_TS_USE_RT_DEVICE
    assert(ts_device_open(&ts_device, 0) == 0);
    assert(ts_device_read(&ts_device, 0, unaligned + 1, sizeof(double)) == sizeof(double));
    memcpy(&temp, unaligned + 1, sizeof(temp));
    assert(temp == milli / 1000.0);
    assert(ts_device_close(&ts_device) == 0);
#endif
    assert(reads == old_reads && writes == old_writes);

    /* Freshness follows the announced interval, which never refreshes data. */
    assert(k230_thermal_reading_set_interval(rt_tick_from_millisecond(10000)) == 0);
    now_us += 10000000;
    assert(tsensor_read_temp(&temp) == 0);
    now_us += 1500000;
    assert(tsensor_read_temp(&temp) == -RT_ETIMEOUT);
    assert(k230_thermal_reading_set_interval(rt_tick_from_millisecond(10000)) == -RT_ETIMEOUT);
    assert(k230_pm_thermal_reading(&after) == 0 && after.stale && after.tick == before.tick);

    /* A failed conversion keeps the earlier data and its history. */
    assert(ts_hw_read_single(&milli, sleep_1ms) == 0);
    k230_thermal_reading_publish(0, milli);
    assert(k230_pm_thermal_reading(&before) == 0 && before.sequence == 2);
    no_conversion = true;
    assert(ts_hw_read_single(&milli, sleep_1ms) == -RT_ETIMEOUT && !(ctrl & 1));
    k230_thermal_reading_publish(-RT_ETIMEOUT, 0);
    assert(k230_pm_thermal_reading(&after) == 0);
    assert(after.sequence == before.sequence && after.tick == before.tick);
    assert(after.valid && after.error == -RT_ETIMEOUT);
    assert(tsensor_read_temp(&temp) == -RT_ETIMEOUT);
    no_conversion = false;
    assert(ts_hw_read_single(&milli, sleep_1ms) == 0);
    k230_thermal_reading_publish(0, milli);
    assert(k230_pm_thermal_reading(&after) == 0 && after.sequence == before.sequence + 1 && !after.error);

    /* A reconfiguration invalidates the data, not the history. */
    k230_thermal_reading_invalidate();
    assert(tsensor_read_temp(&temp) == -RT_EEMPTY);
    assert(k230_pm_thermal_reading(&before) == 0 && before.sequence == after.sequence);

    /* An out-of-range conversion reaches readers unclamped. */
    next_raw = 0;
    assert(ts_hw_read_single(&milli, sleep_1ms) == 0 && milli > 125000);
    k230_thermal_reading_publish(0, milli);
    assert(tsensor_read_temp(&temp) == 0 && temp > 125.0);
#endif

    /* The bare read used by terminal shutdown: no scheduler or mutex, the
     * watchdog serviced, bounded by the first-conversion deadline. */
    terminal = true;
    ts_hw_configure(TS_HW_DEFAULT_TRIM, false);
    unsigned old_sleeps = sleeps;
    assert(ts_hw_read_single(&milli, busy_1ms) == 0);
    assert(kicks > 0 && sleeps == old_sleeps && !(ctrl & 1));
    no_conversion = true;
    uint64_t start = now_us;
    assert(ts_hw_read_single(&milli, busy_1ms) == -RT_ETIMEOUT);
    assert(now_us - start <= (TS_HW_FIRST_CONVERSION_MS + 1) * 1100 && !(ctrl & 1));
    terminal = false;
    puts("PASS bare sensor, device wrapper, thermal reading cache and terminal read");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="k230-tsensor-test-") as directory:
        build = Path(directory)
        (build / "stubs.h").write_text(STUBS)
        for name in ("rtdef.h", "rtdevice.h", "rthw.h", "rtthread.h", "board.h",
                     "ioremap.h", "sysctl_clk.h", "riscv_io.h", "tick.h", "finsh.h"):
            (build / name).write_text('#include "stubs.h"\n')
        for header in ("tsensor/drv_ts.h", "tsensor/ts_hw.h",
                       "k230_pm/k230_pm_thermal.h", "k230_pm/k230_pm_thermal_policy.h"):
            path = DRIVERS / header
            (build / path.name).write_text(path.read_text())
        sources = ["tsensor/ts_hw.c", "tsensor/drv_ts.c"]
        for hz in (100, 1000):
            for thermal in (False, True):
                units = sources + (["k230_pm/k230_pm_thermal_reading.c"] if thermal else [])
                source = build / "test.c"
                source.write_text("".join(f'#include "{DRIVERS / unit}"\n' for unit in units) + CASES)
                for device in ("none", "legacy", "ops"):
                    flags = [f"-DTEST_HZ={hz}", "-DRT_USING_MSH"]
                    if thermal:
                        flags.append("-DK230_PM_THERMAL_PROTECT")
                    if device != "none":
                        flags.append("-DDRV_TS_USE_RT_DEVICE")
                    if device == "ops":
                        flags.append("-DRT_USING_DEVICE_OPS")
                    binary = build / "test"
                    subprocess.run([
                        os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        *flags, "-I", str(build), str(source), "-o", str(binary),
                    ], check=True)
                    print(f"tick_hz={hz} thermal={thermal} device={device}", flush=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
