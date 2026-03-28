/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-01-30     armink       the first version
 */

#include <time.h>
#include <string.h>
#include <rtthread.h>

#include <drivers/rtc.h>

#include "tick.h"

#ifdef RT_USING_SOFT_RTC

/* 2018-01-30 14:44:50 = RTC_TIME_INIT(2018, 1, 30, 14, 44, 50)  */
#define RTC_TIME_INIT(year, month, day, hour, minute, second)        \
    {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = hour, .tm_min = minute, .tm_sec = second}

#ifndef SOFT_RTC_TIME_DEFAULT
#define SOFT_RTC_TIME_DEFAULT                    RTC_TIME_INIT(2018, 1, 1, 0, 0 ,0)
#endif

static struct rt_device soft_rtc_dev;
static uint64_t init_tick;
static time_t init_time;

static rt_err_t soft_rtc_control(rt_device_t dev, int cmd, void *args)
{
    time_t *time;
    struct tm time_temp;

    RT_ASSERT(dev != RT_NULL);
    memset(&time_temp, 0, sizeof(struct tm));

    switch (cmd)
    {
    case RT_DEVICE_CTRL_RTC_GET_TIME:
        time = (time_t *) args;
        *time = init_time + (cpu_ticks_ms() - init_tick) / 1000;
        break;

    case RT_DEVICE_CTRL_RTC_SET_TIME:
    {
        time = (time_t *) args;
        init_time = *time - (cpu_ticks_ms() - init_tick) / 1000;
        break;
    }
    }

    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops soft_rtc_ops = 
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    soft_rtc_control
};
#endif

int rt_soft_rtc_init(void)
{
    static rt_bool_t init_ok = RT_FALSE;
    struct tm time_new = SOFT_RTC_TIME_DEFAULT;

    if (init_ok)
    {
        return 0;
    }
    /* make sure only one 'rtc' device */
    RT_ASSERT(!rt_device_find("rtc"));

    init_tick = cpu_ticks_ms();
    init_time = mktime(&time_new);

    soft_rtc_dev.type    = RT_Device_Class_RTC;

    /* register rtc device */
#ifdef RT_USING_DEVICE_OPS
    soft_rtc_dev.ops     = &soft_rtc_ops;
#else
    soft_rtc_dev.init    = RT_NULL;
    soft_rtc_dev.open    = RT_NULL;
    soft_rtc_dev.close   = RT_NULL;
    soft_rtc_dev.read    = RT_NULL;
    soft_rtc_dev.write   = RT_NULL;
    soft_rtc_dev.control = soft_rtc_control;
#endif

    /* no private */
    soft_rtc_dev.user_data = RT_NULL;

    rt_device_register(&soft_rtc_dev, "rtc", RT_DEVICE_FLAG_RDWR);

    init_ok = RT_TRUE;

    return 0;
}
INIT_DEVICE_EXPORT(rt_soft_rtc_init);

#if 0 && defined (RT_USING_FINSH)
static void rtc_dump(int argc, char **argv)
{
    int count = 60;
    if (argc > 1)
        count = atoi(argv[1]);

    for (int i = 0; i < count; i++)
    {
        time_t now;
        struct tm *tm;
        soft_rtc_control(&soft_rtc_dev, RT_DEVICE_CTRL_RTC_GET_TIME, &now);
        tm = localtime(&now);
        rt_kprintf("[%3d] %04d-%02d-%02d %02d:%02d:%02d  ticks_ms=%ld\n",
            i, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec, cpu_ticks_ms());
        rt_thread_mdelay(1000);
    }
}
MSH_CMD_EXPORT(rtc_dump, dump RTC time every second. usage: rtc_dump [count]);
#endif

#endif /* RT_USING_SOFT_RTC */
