/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2012-12-08     Bernard      fix the issue of _timevalue.tv_usec initialization,
 *                             which found by Rob <rdent@iinet.net.au>
 */

#include <rtdevice.h>
#include <time.h>
#include "clock_time.h"
#include "lwp.h"
#include "tick.h"

static struct timeval _timevalue;

static rt_tick_t clock_tick_max_safe(void)
{
    return (rt_tick_t)(RT_TICK_MAX / 2U - 1U);
}

static rt_tick_t clock_duration_to_tick(time_t second, long nsecond)
{
    rt_int64_t second_delta = second;
    rt_int64_t nsecond_delta = nsecond;
    rt_uint64_t tick;
    rt_uint64_t tick_max = clock_tick_max_safe();

    if (nsecond_delta < 0)
    {
        nsecond_delta += NANOSECOND_PER_SECOND;
        second_delta -= 1;
    }

    if (second_delta < 0)
    {
        return 0;
    }

    tick = (rt_uint64_t)second_delta * RT_TICK_PER_SECOND;
    tick += ((rt_uint64_t)nsecond_delta * RT_TICK_PER_SECOND) / NANOSECOND_PER_SECOND;

    if (tick > tick_max)
    {
        tick = tick_max;
    }

    return (rt_tick_t)tick;
}

static rt_tick_t clock_realtime_to_tick(const struct timespec *time)
{
    struct timespec tp;
    rt_int64_t second;
    rt_int64_t nsecond;

    clock_gettime(CLOCK_REALTIME, &tp);

    second = (rt_int64_t)time->tv_sec - (rt_int64_t)tp.tv_sec;
    nsecond = (rt_int64_t)time->tv_nsec - (rt_int64_t)tp.tv_nsec;

    return clock_duration_to_tick((time_t)second, (long)nsecond);
}

int clock_time_system_init()
{
    time_t time;
    rt_device_t device;

    time = 0;
    device = rt_device_find("rtc");
    if (device != RT_NULL)
    {
        /* get realtime seconds */
        rt_device_control(device, RT_DEVICE_CTRL_RTC_GET_TIME, &time);
    }

    /* use hardware timer for microsecond precision */
    uint64_t us = cpu_ticks_us();

    _timevalue.tv_usec = us % MICROSECOND_PER_SECOND;
    /*
     * The -1 pre-compensates for the fractional second carry that occurs in
     * clock_gettime(): there tv_nsec is computed as
     *   (_timevalue.tv_usec + us % 1e6) * 1000
     * which can exceed 1 second, triggering a +1 sec carry.  By subtracting
     * 1 here the two cancel out and the result matches the RTC epoch time.
     */
    _timevalue.tv_sec = time - us / MICROSECOND_PER_SECOND - 1;

    return 0;
}
INIT_COMPONENT_EXPORT(clock_time_system_init);

int clock_time_to_tick(const struct timespec *time)
{
    RT_ASSERT(time != RT_NULL);

    return (int)clock_realtime_to_tick(time);
}
RTM_EXPORT(clock_time_to_tick);

int clock_getres(clockid_t clockid, struct timespec *res)
{
    int ret = 0;

    if (res == RT_NULL)
    {
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }

    switch (clockid)
    {
    case CLOCK_REALTIME:
        res->tv_sec = 0;
        res->tv_nsec = 1000; /* microsecond resolution from hardware timer */
        break;

#ifdef RT_USING_CPUTIME
    case CLOCK_MONOTONIC:
    case CLOCK_CPUTIME_ID:
        res->tv_sec  = 0;
        res->tv_nsec = clock_cpu_getres();
        break;
#endif

    default:
        ret = -RT_ERROR;
        rt_set_errno(EINVAL);
        break;
    }

    return ret;
}
RTM_EXPORT(clock_getres);

int clock_gettime(clockid_t clockid, struct timespec *tp)
{
    int ret = 0;

    if (tp == RT_NULL)
    {
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }

    switch (clockid)
    {
    case CLOCK_REALTIME:
        {
            /* use hardware timer for microsecond precision */
            uint64_t us = cpu_ticks_us();

            tp->tv_sec  = _timevalue.tv_sec + us / MICROSECOND_PER_SECOND;
            tp->tv_nsec = (_timevalue.tv_usec + us % MICROSECOND_PER_SECOND) * 1000;
            if (tp->tv_nsec >= NANOSECOND_PER_SECOND)
            {
                tp->tv_sec += tp->tv_nsec / NANOSECOND_PER_SECOND;
                tp->tv_nsec %= NANOSECOND_PER_SECOND;
            }
        }
        break;

#ifdef RT_USING_CPUTIME
    case CLOCK_MONOTONIC:
    case CLOCK_CPUTIME_ID:
        {
            double unit = 0;
            uint64_t cpu_tick;

            unit = clock_cpu_getres();
            cpu_tick = clock_cpu_gettime();

            tp->tv_sec = ((uint64_t)(cpu_tick * unit)) / NANOSECOND_PER_SECOND;
            tp->tv_nsec = ((uint64_t)(cpu_tick * unit)) % NANOSECOND_PER_SECOND;
        }
        break;
#endif
    default:
        rt_set_errno(EINVAL);
        ret = -RT_ERROR;
    }

    return ret;
}
RTM_EXPORT(clock_gettime);

int clock_settime(clockid_t clockid, const struct timespec *tp)
{
    time_t second;
    rt_device_t device;

    if ((clockid != CLOCK_REALTIME) || (tp == RT_NULL))
    {
        rt_set_errno(EINVAL);

        return -RT_ERROR;
    }

    /* get second */
    second = tp->tv_sec;
    /* use hardware timer for microsecond precision */
    uint64_t us = cpu_ticks_us();

    /* update timevalue */
    _timevalue.tv_usec = MICROSECOND_PER_SECOND - (us % MICROSECOND_PER_SECOND);
    _timevalue.tv_sec = second - us / MICROSECOND_PER_SECOND - 1;

    /* update for RTC device */
    device = rt_device_find("rtc");
    if (device != RT_NULL)
    {
        /* set realtime seconds */
        rt_device_control(device, RT_DEVICE_CTRL_RTC_SET_TIME, &second);
    }
    else
        return -RT_ERROR;

    return 0;
}
RTM_EXPORT(clock_settime);

int clock_nanosleep(clockid_t clockid, int flags, const struct timespec *rqtp, struct timespec *rmtp)
{
    if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec >= NANOSECOND_PER_SECOND)
    {
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }
    switch (clockid)
    {
    case CLOCK_REALTIME:
    {
        rt_tick_t tick, tick_old = rt_tick_get();
        if ((flags & TIMER_ABSTIME) == TIMER_ABSTIME)
        {
            tick = clock_realtime_to_tick(rqtp);
        }
        else
        {
            tick = clock_duration_to_tick(rqtp->tv_sec, rqtp->tv_nsec);
        }

        rt_thread_delay(tick);

        if (rt_get_errno() == -RT_EINTR)
        {
            rt_tick_t tick_now = rt_tick_get();
            if ((tick_now - tick_old) < tick)
            {
                if (rmtp)
                {
                    tick = tick - (tick_now - tick_old);
                    /* get the passed time */
                    rmtp->tv_sec = tick / RT_TICK_PER_SECOND;
                    rmtp->tv_nsec = (tick % RT_TICK_PER_SECOND) * (NANOSECOND_PER_SECOND / RT_TICK_PER_SECOND);
                }
                rt_set_errno(EINTR);
                return -RT_ERROR;
            }
            else
                rt_set_errno(0);
        }
    }
    break;

#ifdef RT_USING_CPUTIME
    case CLOCK_MONOTONIC:
    case CLOCK_CPUTIME_ID:
    {
        uint64_t cpu_tick, cpu_tick_old;
        struct timespec tp;
        cpu_tick_old = clock_cpu_gettime();
        rt_tick_t tick;
        float unit = clock_cpu_getres();

        cpu_tick = ((uint64_t)rqtp->tv_sec * NANOSECOND_PER_SECOND + rqtp->tv_nsec) / unit;
        if ((flags & TIMER_ABSTIME) == TIMER_ABSTIME)
        {
            clock_gettime(clockid, &tp);
            tick = clock_duration_to_tick(rqtp->tv_sec - tp.tv_sec, rqtp->tv_nsec - tp.tv_nsec);
            cpu_tick = cpu_tick < cpu_tick_old ? 0 : cpu_tick - cpu_tick_old;
        }
        else
        {
            tick = clock_duration_to_tick(rqtp->tv_sec, rqtp->tv_nsec);
        }

        rt_thread_delay(tick);

        if (rt_get_errno() == -RT_EINTR)
        {
            uint64_t rmtp_cpu_tick = clock_cpu_gettime();
            if ((rmtp_cpu_tick - cpu_tick_old) < cpu_tick)
            {
                if (rmtp)
                {
                    rt_uint64_t rmtp_nsec;

                    rmtp_cpu_tick = cpu_tick - (rmtp_cpu_tick - cpu_tick_old);
                    rmtp_nsec = (rt_uint64_t)(rmtp_cpu_tick * unit);
                    rmtp->tv_sec = rmtp_nsec / NANOSECOND_PER_SECOND;
                    rmtp->tv_nsec = rmtp_nsec % NANOSECOND_PER_SECOND;
                }
                rt_set_errno(EINTR);
                return -RT_ERROR;
            }
            else
                rt_set_errno(0);
        }
        else
            while (clock_cpu_gettime() - cpu_tick_old < cpu_tick);
    }
    break;
#endif
    default:
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }

    return 0;
}
RTM_EXPORT(clock_nanosleep);

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec >= NANOSECOND_PER_SECOND)
    {
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }
#ifdef RT_USING_CPUTIME
    uint64_t cpu_tick, cpu_tick_old;
    cpu_tick_old = clock_cpu_gettime();
    rt_tick_t tick;
    float unit = clock_cpu_getres();

    cpu_tick = ((uint64_t)rqtp->tv_sec * NANOSECOND_PER_SECOND + rqtp->tv_nsec) / unit;
    tick = clock_duration_to_tick(rqtp->tv_sec, rqtp->tv_nsec);
    rt_thread_delay(tick);

    if (rt_get_errno() == -RT_EINTR)
    {
        uint64_t rmtp_cpu_tick = clock_cpu_gettime();
        if ((rmtp_cpu_tick - cpu_tick_old) < cpu_tick)
        {
            if (rmtp)
            {
                rt_uint64_t rmtp_nsec;

                rmtp_cpu_tick = cpu_tick - (rmtp_cpu_tick - cpu_tick_old);
                rmtp_nsec = (rt_uint64_t)(rmtp_cpu_tick * unit);
                rmtp->tv_sec = rmtp_nsec / NANOSECOND_PER_SECOND;
                rmtp->tv_nsec = rmtp_nsec % NANOSECOND_PER_SECOND;
            }
            rt_set_errno(EINTR);
            return -RT_ERROR;
        }
        else
            rt_set_errno(0);
    }
    else
        while (clock_cpu_gettime() - cpu_tick_old < cpu_tick);
#else
    rt_tick_t tick, tick_old = rt_tick_get();
    tick = clock_duration_to_tick(rqtp->tv_sec, rqtp->tv_nsec);
    rt_thread_delay(tick);

    if (rt_get_errno() == -RT_EINTR)
    {
        rt_tick_t tick_now = rt_tick_get();
        if ((tick_now - tick_old) < tick)
        {
            if (rmtp)
            {
                tick = tick_old + tick - tick_now;
                /* get the passed time */
                rmtp->tv_sec = tick / RT_TICK_PER_SECOND;
                rmtp->tv_nsec = (tick % RT_TICK_PER_SECOND) * (NANOSECOND_PER_SECOND / RT_TICK_PER_SECOND);
            }
            rt_set_errno(EINTR);
            return -RT_ERROR;
        }
        else
            rt_set_errno(0);
    }
#endif
    return 0;
}
RTM_EXPORT(nanosleep);

static void rtthread_timer_wrapper(void *timerobj)
{
    struct timer_obj *timer;
    siginfo_t info;

    timer = (struct timer_obj *)timerobj;

    rt_memset(&info, 0, sizeof(info));
    info.si_code = SI_TIMER;
    info.si_ptr = timer->val.sival_ptr;
    lwp_kill_ext(timer->pid, timer->sigev_signo, &info);

    if (timer->reload == 0U)
    {
        timer->status = NOT_ACTIVE;
    }

    // if (timer->sigev_notify_function != RT_NULL)
    // {
    //     (timer->sigev_notify_function)(timer->val);
    // }

    timer->reload = clock_duration_to_tick(timer->interval.tv_sec, timer->interval.tv_nsec);
    rt_timer_control(&timer->timer, RT_TIMER_CTRL_SET_TIME, &(timer->reload));
}

int timer_create(clockid_t clockid, struct sigevent *evp, timer_t *timerid)
{
    static int num = 0;
    struct timer_obj *timer;
    char timername[RT_NAME_MAX] = {0};

    if (clockid > CLOCK_TAI || evp->sigev_notify != SIGEV_NONE && evp->sigev_notify != SIGEV_SIGNAL)
    {
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }

    timer = rt_malloc(sizeof(struct timer_obj));
    if (timer == RT_NULL)
    {
        rt_set_errno(ENOMEM);
        return -RT_ENOMEM;
    }

    rt_snprintf(timername, RT_NAME_MAX, "psx_tm%02d", num++);
    num %= 100;
    timer->sigev_signo = evp->sigev_signo;
    timer->pid = lwp_self()->pid;
    timer->sigev_notify_function = evp->sigev_notify_function;
    timer->val = evp->sigev_value;
    timer->interval.tv_sec = 0;
    timer->interval.tv_nsec = 0;
    timer->reload = 0U;
    timer->status = NOT_ACTIVE;

    if (evp->sigev_notify == SIGEV_NONE)
    {
        rt_timer_init(&timer->timer, timername, RT_NULL, RT_NULL, 0, RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    }
    else
    {
        rt_timer_init(&timer->timer, timername, rtthread_timer_wrapper, timer, 0, RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    }

    *timerid = (timer_t)((uintptr_t)timer >> 1);

    return RT_EOK;
}
RTM_EXPORT(timer_create);

int timer_delete(timer_t timerid)
{
    struct timer_obj *timer = (struct timer_obj *)((uintptr_t)timerid << 1);

    if (timer == RT_NULL || rt_object_get_type(&timer->timer.parent) != RT_Object_Class_Timer)
    {
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }

    if (timer->status == ACTIVE)
    {
        timer->status = NOT_ACTIVE;
        rt_timer_stop(&timer->timer);
    }
    rt_timer_detach(&timer->timer);
    rt_free(timer);
    return RT_EOK;
}
RTM_EXPORT(timer_delete);

int timer_getoverrun(timer_t timerid)
{
    struct timer_obj *timer = (struct timer_obj *)((uintptr_t)timerid << 1);
    rt_set_errno(ENOSYS);
    return -RT_ERROR;
}

int timer_gettime(timer_t timerid, struct itimerspec *its)
{
    struct timer_obj *timer = (struct timer_obj *)((uintptr_t)timerid << 1);
    rt_tick_t remaining;
    rt_uint32_t seconds, nanoseconds;

    if (timer == NULL || rt_object_get_type(&timer->timer.parent) != RT_Object_Class_Timer)
    {
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }

    if (its == NULL)
    {
        rt_set_errno(EFAULT);
        return -RT_ERROR;
    }

    if (timer->status == ACTIVE)
    {
        rt_tick_t remain_tick;

        rt_timer_control(&timer->timer, RT_TIMER_CTRL_GET_REMAIN_TIME, &remain_tick);

        /* 'remain_tick' is minimum-unit in the RT-Thread' timer,
         * so the seconds, nanoseconds will be calculated by 'remain_tick'.
         */
        remaining = remain_tick - rt_tick_get();

        /* calculate 'second' */
        seconds = remaining / RT_TICK_PER_SECOND;

        /* calculate 'nanosecond';  To avoid lost of accuracy, because "RT_TICK_PER_SECOND" maybe 100, 1000, 1024 and so on.
         *
         *        remain_tick                  millisecond                                 remain_tick * MILLISECOND_PER_SECOND
         *  ------------------------- = --------------------------  --->  millisecond = -------------------------------------------
         *    RT_TICK_PER_SECOND          MILLISECOND_PER_SECOND                                RT_TICK_PER_SECOND
         *
         *                    remain_tick * MILLISECOND_PER_SECOND                          remain_tick * MILLISECOND_PER_SECOND * MICROSECOND_PER_SECOND
         *   millisecond = ----------------------------------------  ---> nanosecond = -------------------------------------------------------------------
         *                         RT_TICK_PER_SECOND                                                           RT_TICK_PER_SECOND
         *
         */
        nanoseconds = (((remaining % RT_TICK_PER_SECOND) * MILLISECOND_PER_SECOND) * MICROSECOND_PER_SECOND) / RT_TICK_PER_SECOND;

        its->it_value.tv_sec = (rt_int32_t)seconds;
        its->it_value.tv_nsec = (rt_int32_t)nanoseconds;
    }
    else
    {
        /* Timer is disarmed */
        its->it_value.tv_sec = 0;
        its->it_value.tv_nsec = 0;
    }

    /* The interval last set by timer_settime() */
    its->it_interval = timer->interval;
    return RT_EOK;
}
RTM_EXPORT(timer_gettime);

int timer_settime(timer_t timerid, int flags, const struct itimerspec *value,
                  struct itimerspec *ovalue)
{
    struct timer_obj *timer = (struct timer_obj *)((uintptr_t)timerid << 1);
    if (timer == NULL ||
        rt_object_get_type(&timer->timer.parent) != RT_Object_Class_Timer ||
        value->it_interval.tv_nsec < 0 ||
        value->it_interval.tv_nsec >= NANOSECOND_PER_SECOND ||
        value->it_interval.tv_sec < 0 ||
        value->it_value.tv_nsec < 0 ||
        value->it_value.tv_nsec >= NANOSECOND_PER_SECOND ||
        value->it_value.tv_sec < 0)
    {
        rt_set_errno(EINVAL);
        return -RT_ERROR;
    }

    /*  Save time to expire and old reload value. */
    if (ovalue != NULL)
    {
        timer_gettime(timerid, ovalue);
    }

    /* Stop the timer if the value is 0 */
    if ((value->it_value.tv_sec == 0) && (value->it_value.tv_nsec == 0))
    {
        if (timer->status == ACTIVE)
        {
            rt_timer_stop(&timer->timer);
        }

        timer->status = NOT_ACTIVE;
        return RT_EOK;
    }
    if ((flags & TIMER_ABSTIME) == TIMER_ABSTIME)
    {
        timer->reload = clock_realtime_to_tick(&value->it_value);
    }
    else
    {
        timer->reload = clock_duration_to_tick(value->it_value.tv_sec, value->it_value.tv_nsec);
    }
    timer->interval.tv_sec = value->it_interval.tv_sec;
    timer->interval.tv_nsec = value->it_interval.tv_nsec;
    timer->value.tv_sec = value->it_value.tv_sec;
    timer->value.tv_nsec = value->it_value.tv_nsec;

    if (timer->status == ACTIVE)
    {
        rt_timer_stop(&timer->timer);
    }

    timer->status = ACTIVE;

    if ((value->it_interval.tv_sec == 0) && (value->it_interval.tv_nsec == 0))
        rt_timer_control(&timer->timer, RT_TIMER_CTRL_SET_ONESHOT, RT_NULL);
    else
        rt_timer_control(&timer->timer, RT_TIMER_CTRL_SET_PERIODIC, RT_NULL);

    rt_timer_control(&timer->timer, RT_TIMER_CTRL_SET_TIME, &(timer->reload));
    rt_timer_start(&timer->timer);

    return RT_EOK;
}
RTM_EXPORT(timer_settime);
