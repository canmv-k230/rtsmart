/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <rtthread.h>
#include <rthw.h>
#include "k230_pm_thermal.h"

/* The sensor's latest reading. Only the thermal sampler publishes; readers
 * (the controller, /dev/ts) take a copy. */
static rt_spinlock_t reading_lock;
static struct k230_thermal_reading reading;
static bool reading_ready;

static rt_tick_t reading_fresh_ticks(const struct k230_thermal_reading *r)
{
    return r->interval_ticks ? r->interval_ticks +
        rt_tick_from_millisecond(K230_THERMAL_READING_FRESH_MS - K230_THERMAL_READING_NORMAL_MS) :
        rt_tick_from_millisecond(K230_THERMAL_READING_FRESH_MS);
}

void k230_thermal_reading_init(void)
{
    rt_spin_lock_init(&reading_lock);
    reading.error = -RT_EEMPTY;
    __atomic_store_n(&reading_ready, true, __ATOMIC_RELEASE);
}

void k230_thermal_reading_publish(int error, int32_t millicelsius)
{
    rt_base_t level = rt_spin_lock_irqsave(&reading_lock);

    reading.error = error;
    if (error == RT_EOK) {
        reading.millicelsius = millicelsius;
        reading.tick = rt_tick_get();
        reading.sequence++;
        reading.interval_ticks = 0;
        reading.valid = true;
    }
    rt_spin_unlock_irqrestore(&reading_lock, level);
}

/* A sensor reconfiguration invalidates the data, not its history. */
void k230_thermal_reading_invalidate(void)
{
    rt_base_t level = rt_spin_lock_irqsave(&reading_lock);

    reading.valid = false;
    reading.error = -RT_EEMPTY;
    rt_spin_unlock_irqrestore(&reading_lock, level);
}

int k230_thermal_reading_set_interval(rt_tick_t interval)
{
    rt_base_t level;

    if (!interval || interval > rt_tick_from_millisecond(10000))
        return -RT_EINVAL;
    level = rt_spin_lock_irqsave(&reading_lock);
    if (!reading.valid || reading.error ||
        (rt_tick_t)(rt_tick_get() - reading.tick) >= reading_fresh_ticks(&reading)) {
        rt_spin_unlock_irqrestore(&reading_lock, level);
        return -RT_ETIMEOUT;
    }
    /* Updating the schedule never refreshes acquisition time or sequence. */
    reading.interval_ticks = interval;
    rt_spin_unlock_irqrestore(&reading_lock, level);
    return RT_EOK;
}

int k230_pm_thermal_reading(struct k230_thermal_reading *out)
{
    rt_base_t level;

    if (out == RT_NULL)
        return -RT_EINVAL;
    if (!__atomic_load_n(&reading_ready, __ATOMIC_ACQUIRE))
        return -RT_ENOSYS;
    level = rt_spin_lock_irqsave(&reading_lock);
    *out = reading;
    rt_spin_unlock_irqrestore(&reading_lock, level);
    out->stale = !out->valid || (rt_tick_t)(rt_tick_get() - out->tick) >= reading_fresh_ticks(out);
    return RT_EOK;
}
