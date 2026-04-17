/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
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

#ifndef DRV_PMU_H__
#define DRV_PMU_H__

#include <stdint.h>
#include <time.h>

#define RT_DEVICE_CTRL_RTC_SET_CALLBACK     0x44
#define RT_DEVICE_CTRL_RTC_STOP_ALARM       0x45
#define RT_DEVICE_CTRL_RTC_STOP_TICK        0x46

#ifndef BIT
#define BIT(n)  (1U << (n))
#endif

typedef enum {
        RTC_INT_ALARM_YEAR = BIT(0),
        RTC_INT_ALARM_MONTH = BIT(1),
        RTC_INT_ALARM_DAY = BIT(2),
        RTC_INT_ALARM_WEEK = BIT(3),
        RTC_INT_ALARM_HOUR = BIT(4),
        RTC_INT_ALARM_MINUTE = BIT(5),
        RTC_INT_ALARM_SECOND = BIT(6),
        RTC_INT_TICK_YEAR = BIT(7),
        RTC_INT_TICK_MONTH,
        RTC_INT_TICK_DAY,
        RTC_INT_TICK_WEEK,
        RTC_INT_TICK_HOUR,
        RTC_INT_TICK_MINUTE,
        RTC_INT_TICK_SECOND,
        RTC_INT_TICK_S8,
        RTC_INT_TICK_S64,
} rtc_interrupt_mode_t;

struct kd_alarm_setup {
        uint32_t flag;
        struct tm tm;
};

int k230_pmu_rtc_get_time(time_t *time);
int k230_pmu_rtc_set_time(const time_t *time);
int k230_pmu_rtc_get_alarm(struct tm *tm);
int k230_pmu_rtc_set_alarm(const struct kd_alarm_setup *setup);
void k230_pmu_rtc_stop_alarm(void);
void k230_pmu_rtc_stop_tick(void);

#endif /* DRV_PMU_H__ */
