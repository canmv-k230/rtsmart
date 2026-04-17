#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "pmu_priv.h"

static volatile k230_rtc_regs_t *pmu_rtc_regs(struct pmu_dev *pmu)
{
    return (volatile k230_rtc_regs_t *)pmu_get_rtc_base(pmu);
}

static k230_rtc_int_ctrl_t *pmu_rtc_int_ctrl(struct pmu_dev *pmu)
{
    volatile k230_rtc_regs_t *rtc = pmu_rtc_regs(pmu);

    if (rtc == RT_NULL)
        return RT_NULL;

    return (k230_rtc_int_ctrl_t *)&rtc->int_ctrl;
}

static int pmu_rtc_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static void pmu_rtc_split_year(int year, int *year_h, int *year_l)
{
    int value;

    value = year % 100;
    if (value == 0) {
        *year_l = 100;
        *year_h = year / 100 - 1;
        return;
    }

    *year_l = value;
    *year_h = (year - value) / 100;
}

static void pmu_rtc_set_count(struct pmu_dev *pmu, unsigned int count)
{
    volatile k230_rtc_regs_t *rtc = pmu_rtc_regs(pmu);
    k230_rtc_count_t current_count;

    if (rtc == RT_NULL)
        return;

    rt_memset(&current_count, 0, sizeof(current_count));
    current_count.curr_count = count;
    current_count.sum_count = 32767;
    rtc->count = current_count;
    rt_thread_mdelay(1);
}

static void pmu_rtc_mark_write_complete(struct pmu_dev *pmu)
{
    k230_rtc_int_ctrl_t *int_ctrl = pmu_rtc_int_ctrl(pmu);

    if (int_ctrl == RT_NULL)
        return;

    int_ctrl->timer_w_en = 1;
    int_ctrl->timer_w_en = 0;
    int_ctrl->timer_r_en = 1;
}

static rt_bool_t pmu_rtc_is_tick_mode(rt_uint32_t flag)
{
    return flag >= RTC_INT_TICK_YEAR;
}

static rt_bool_t pmu_rtc_is_valid_tick_mode(rt_uint32_t flag)
{
    switch (flag) {
    case RTC_INT_TICK_YEAR:
    case RTC_INT_TICK_MONTH:
    case RTC_INT_TICK_DAY:
    case RTC_INT_TICK_WEEK:
    case RTC_INT_TICK_HOUR:
    case RTC_INT_TICK_MINUTE:
    case RTC_INT_TICK_SECOND:
    case RTC_INT_TICK_S8:
    case RTC_INT_TICK_S64:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static rt_bool_t pmu_rtc_is_valid_alarm_mode(rt_uint32_t flag)
{
    if ((flag == 0U) || pmu_rtc_is_tick_mode(flag))
        return RT_FALSE;

    return (flag & ~PMU_RTC_ALARM_FLAG_MASK) == 0U;
}

static void pmu_rtc_configure_mode(struct pmu_dev *pmu,
                   rtc_interrupt_mode_t mode)
{
    k230_rtc_int_ctrl_t *int_ctrl = pmu_rtc_int_ctrl(pmu);

    if (int_ctrl == RT_NULL)
        return;

    if (mode < RTC_INT_TICK_YEAR) {
        int_ctrl->year_cmp = (mode & RTC_INT_ALARM_YEAR) != 0U;
        int_ctrl->month_cmp = (mode & RTC_INT_ALARM_MONTH) != 0U;
        int_ctrl->day_cmp = (mode & RTC_INT_ALARM_DAY) != 0U;
        int_ctrl->week_cmp = (mode & RTC_INT_ALARM_WEEK) != 0U;
        int_ctrl->hour_cmp = (mode & RTC_INT_ALARM_HOUR) != 0U;
        int_ctrl->minute_cmp = (mode & RTC_INT_ALARM_MINUTE) != 0U;
        int_ctrl->second_cmp = (mode & RTC_INT_ALARM_SECOND) != 0U;
        int_ctrl->alarm_en = 1;
        pmu_set_detect_en_locked(pmu, PMU_DET_RTC_ALARM, true);
        return;
    }

    switch (mode) {
    case RTC_INT_TICK_YEAR:
        int_ctrl->tick_sel = 0x8;
        break;
    case RTC_INT_TICK_MONTH:
        int_ctrl->tick_sel = 0x7;
        break;
    case RTC_INT_TICK_DAY:
        int_ctrl->tick_sel = 0x6;
        break;
    case RTC_INT_TICK_WEEK:
        int_ctrl->tick_sel = 0x5;
        break;
    case RTC_INT_TICK_HOUR:
        int_ctrl->tick_sel = 0x4;
        break;
    case RTC_INT_TICK_MINUTE:
        int_ctrl->tick_sel = 0x3;
        break;
    case RTC_INT_TICK_SECOND:
        int_ctrl->tick_sel = 0x2;
        break;
    case RTC_INT_TICK_S8:
        int_ctrl->tick_sel = 0x1;
        break;
    case RTC_INT_TICK_S64:
        int_ctrl->tick_sel = 0x0;
        break;
    default:
        return;
    }

    int_ctrl->tick_en = 1;
}

static void pmu_rtc_stop_alarm_locked(struct pmu_dev *pmu)
{
    k230_rtc_int_ctrl_t *int_ctrl = pmu_rtc_int_ctrl(pmu);

    if (int_ctrl == RT_NULL)
        return;

    int_ctrl->alarm_en = 0;
    pmu_set_detect_en_locked(pmu, PMU_DET_RTC_ALARM, false);
}

static void pmu_rtc_stop_tick_locked(struct pmu_dev *pmu)
{
    k230_rtc_int_ctrl_t *int_ctrl = pmu_rtc_int_ctrl(pmu);

    if (int_ctrl == RT_NULL)
        return;

    int_ctrl->tick_en = 0;
}

static void pmu_rtc_clear_alarm_locked(struct pmu_dev *pmu)
{
    k230_rtc_int_ctrl_t *int_ctrl = pmu_rtc_int_ctrl(pmu);

    if (int_ctrl == RT_NULL)
        return;

    int_ctrl->alarm_clr = 1;
}

static int pmu_rtc_write_time_tm(struct pmu_dev *pmu, const struct tm *tm)
{
    volatile k230_rtc_regs_t *rtc = pmu_rtc_regs(pmu);
    k230_rtc_int_ctrl_t *int_ctrl = pmu_rtc_int_ctrl(pmu);
    k230_rtc_date_t date;
    k230_rtc_time_t time_reg;
    int year;
    int year_l;
    int year_h;

    if ((rtc == RT_NULL) || (int_ctrl == RT_NULL) || (tm == RT_NULL))
        return -RT_ERROR;

    year = tm->tm_year + 1900;
    pmu_rtc_split_year(year, &year_h, &year_l);

    rt_memset(&date, 0, sizeof(date));
    rt_memset(&time_reg, 0, sizeof(time_reg));

    int_ctrl->timer_w_en = 1;

    date.year_h = year_h;
    date.year_l = year_l;
    date.month = tm->tm_mon + 1;
    date.day = tm->tm_mday;
    date.leap_year = pmu_rtc_is_leap_year(year);
    time_reg.week = tm->tm_wday;
    time_reg.hour = tm->tm_hour;
    time_reg.minute = tm->tm_min;
    time_reg.second = tm->tm_sec;

    rtc->date = date;
    rtc->time = time_reg;

    pmu_rtc_set_count(pmu, 0);
    rt_thread_mdelay(10);
    pmu_rtc_mark_write_complete(pmu);
    return RT_EOK;
}

static int pmu_rtc_read_time_locked(struct pmu_dev *pmu, time_t *time)
{
    volatile k230_rtc_regs_t *rtc = pmu_rtc_regs(pmu);
    k230_rtc_int_ctrl_t *int_ctrl = pmu_rtc_int_ctrl(pmu);
    k230_rtc_date_t date;
    k230_rtc_time_t time_reg;
    struct tm tm;

    if ((rtc == RT_NULL) || (int_ctrl == RT_NULL) || (time == RT_NULL))
        return -RT_ERROR;

    if (int_ctrl->timer_r_en == 0)
        int_ctrl->timer_r_en = 1;

    date = rtc->date;
    time_reg = rtc->time;

    rt_memset(&tm, 0, sizeof(tm));
    tm.tm_sec = time_reg.second;
    tm.tm_min = time_reg.minute;
    tm.tm_hour = time_reg.hour;
    tm.tm_mday = date.day;
    tm.tm_mon = date.month - 1;
    tm.tm_year = (date.year_h * 100 + date.year_l) - 1900;
    tm.tm_wday = time_reg.week;

    *time = timegm(&tm);
    return RT_EOK;
}

static int pmu_rtc_read_alarm_locked(struct pmu_dev *pmu, struct tm *tm)
{
    volatile k230_rtc_regs_t *rtc = pmu_rtc_regs(pmu);
    k230_rtc_alarm_date_t alarm_date;
    k230_rtc_alarm_time_t alarm_time;

    if ((rtc == RT_NULL) || (tm == RT_NULL))
        return -RT_ERROR;

    alarm_date = rtc->alarm_date;
    alarm_time = rtc->alarm_time;

    rt_memset(tm, 0, sizeof(*tm));
    tm->tm_year = (alarm_date.alarm_year_h * 100 +
               alarm_date.alarm_year_l) - 1900;
    tm->tm_mon = alarm_date.alarm_month - 1;
    tm->tm_mday = alarm_date.alarm_day;
    tm->tm_hour = alarm_time.alarm_hour;
    tm->tm_min = alarm_time.alarm_minute;
    tm->tm_sec = alarm_time.alarm_second;
    tm->tm_wday = alarm_time.alarm_week;

    return RT_EOK;
}

static int pmu_rtc_write_alarm_tm(struct pmu_dev *pmu, const struct tm *tm)
{
    volatile k230_rtc_regs_t *rtc = pmu_rtc_regs(pmu);
    k230_rtc_alarm_date_t alarm_date;
    k230_rtc_alarm_time_t alarm_time;
    int year;
    int year_l;
    int year_h;

    if ((rtc == RT_NULL) || (tm == RT_NULL))
        return -RT_ERROR;

    year = tm->tm_year + 1900;
    pmu_rtc_split_year(year, &year_h, &year_l);

    rt_memset(&alarm_date, 0, sizeof(alarm_date));
    rt_memset(&alarm_time, 0, sizeof(alarm_time));

    alarm_date.alarm_year_h = year_h;
    alarm_date.alarm_year_l = year_l;
    alarm_date.alarm_month = tm->tm_mon + 1;
    alarm_date.alarm_day = tm->tm_mday;
    alarm_time.alarm_hour = tm->tm_hour;
    alarm_time.alarm_minute = tm->tm_min;
    alarm_time.alarm_second = tm->tm_sec;
    alarm_time.alarm_week = tm->tm_wday;

    rtc->alarm_date = alarm_date;
    rtc->alarm_time = alarm_time;
    return RT_EOK;
}

static bool pmu_rtc_time_regs_valid(struct pmu_dev *pmu)
{
    volatile k230_rtc_regs_t *rtc = pmu_rtc_regs(pmu);
    k230_rtc_int_ctrl_t *int_ctrl = pmu_rtc_int_ctrl(pmu);
    k230_rtc_date_t date;
    k230_rtc_time_t time_reg;
    int year;

    if ((rtc == RT_NULL) || (int_ctrl == RT_NULL))
        return false;

    if (int_ctrl->timer_r_en == 0)
        int_ctrl->timer_r_en = 1;

    date = rtc->date;
    time_reg = rtc->time;
    year = date.year_h * 100 + date.year_l;

    if ((year < 1970) || (year > 2099))
        return false;
    if ((date.month < 1) || (date.month > 12))
        return false;
    if ((date.day < 1) || (date.day > 31))
        return false;
    if (time_reg.hour > 23)
        return false;
    if (time_reg.minute > 59)
        return false;
    if (time_reg.second > 59)
        return false;
    if (time_reg.week > 6)
        return false;

    return true;
}

static time_t pmu_rtc_default_time(void)
{
    struct tm tm;

    rt_memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2025 - 1900;
    tm.tm_mon = 1 - 1;
    tm.tm_mday = 1;
    tm.tm_wday = 5;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;

    return timegm(&tm);
}

static int pmu_rtc_enable_alarm_locked(struct pmu_dev *pmu, rt_uint32_t flag)
{
    pmu_rtc_clear_alarm_locked(pmu);
    pmu_rtc_configure_mode(pmu, flag);
    return RT_EOK;
}

static int pmu_rtc_program_alarm(struct pmu_dev *pmu, const struct tm *tm,
                 uint32_t flag)
{
    int ret;

    ret = pmu_rtc_write_alarm_tm(pmu, tm);
    if (ret != RT_EOK)
        return ret;

    return pmu_rtc_enable_alarm_locked(pmu, flag);
}

static void pmu_rtc_set_alarm_route_locked(struct pmu_dev *pmu, bool cpu_enable,
                       bool output_enable)
{
    pmu_update_bits_locked(pmu, PMU_INT0_TO_CPU_REGISTER,
                   PMU_IRQ_RTC_ALARM, cpu_enable);
    pmu_update_bits_locked(pmu, PMU_INT0_TO_CTL_REGISTER,
                   PMU_IRQ_RTC_ALARM, output_enable);
    pmu_update_bits_locked(pmu, PMU_INT1_TO_CTL_REGISTER,
                   PMU_IRQ_RTC_ALARM, output_enable);
}

static void pmu_rtc_set_alarm_route(struct pmu_dev *pmu, bool cpu_enable,
                    bool output_enable)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    pmu_rtc_set_alarm_route_locked(pmu, cpu_enable, output_enable);
    rt_hw_interrupt_enable(level);
}

static void pmu_rtc_stop_alarm_hw(struct pmu_dev *pmu)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    pmu_rtc_stop_alarm_locked(pmu);
    pmu_rtc_clear_alarm_locked(pmu);
    rt_hw_interrupt_enable(level);
}

static int pmu_rtc_set_alarm_time(struct pmu_dev *pmu, time_t alarm_time)
{
    struct tm *tm;
    rt_base_t level;
    int ret;

    if (!pmu->rtc.initialized)
        return -RT_ERROR;

    tm = gmtime(&alarm_time);
    if (tm == RT_NULL)
        return -RT_ERROR;

    level = rt_hw_interrupt_disable();
    pmu_rtc_stop_tick_locked(pmu);
    pmu_rtc_stop_alarm_locked(pmu);
    pmu_rtc_clear_alarm_locked(pmu);
    ret = pmu_rtc_program_alarm(pmu, tm, PMU_RTC_ALARM_ALL_FLAGS);
    rt_hw_interrupt_enable(level);

    return ret;
}

int pmu_cycle_prepare_wakeup_alarm(struct pmu_dev *pmu)
{
    time_t poweron_time;
    rt_base_t level;
    int ret;

    level = rt_hw_interrupt_disable();
    if (!pmu->cycle.active || !pmu->cycle.shutting_down) {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }

    poweron_time = pmu->cycle.poweron_time;
    pmu->cycle.active = false;
    rt_hw_interrupt_enable(level);

    ret = pmu_rtc_set_alarm_time(pmu, poweron_time);
    if (ret != RT_EOK) {
        level = rt_hw_interrupt_disable();
        pmu->cycle.active = true;
        rt_hw_interrupt_enable(level);
        return ret;
    }

    pmu_rtc_set_alarm_route(pmu, false, true);
    return RT_EOK;
}

bool pmu_cycle_owns_alarm(struct pmu_dev *pmu)
{
    return pmu->cycle.active || pmu->cycle.shutting_down;
}

static void pmu_cycle_save_routes(struct pmu_dev *pmu)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    pmu->cycle.saved_cpu_route = pmu_readl(pmu, PMU_INT0_TO_CPU_REGISTER);
    pmu->cycle.saved_out0_route = pmu_readl(pmu, PMU_INT0_TO_CTL_REGISTER);
    pmu->cycle.saved_out1_route = pmu_readl(pmu, PMU_INT1_TO_CTL_REGISTER);
    pmu->cycle.saved_routes_valid = true;
    rt_hw_interrupt_enable(level);
}

static void pmu_cycle_restore_routes(struct pmu_dev *pmu)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    if (pmu->cycle.saved_routes_valid) {
        pmu_writel(pmu, pmu->cycle.saved_cpu_route,
               PMU_INT0_TO_CPU_REGISTER);
        pmu_writel(pmu, pmu->cycle.saved_out0_route,
               PMU_INT0_TO_CTL_REGISTER);
        pmu_writel(pmu, pmu->cycle.saved_out1_route,
               PMU_INT1_TO_CTL_REGISTER);
        pmu->cycle.saved_routes_valid = false;
    }
    rt_hw_interrupt_enable(level);
}

void pmu_cycle_handle_shutdown_work(struct pmu_dev *pmu)
{
    int ret;

    ret = pmu_cycle_prepare_wakeup_alarm(pmu);
    if (ret == -RT_EBUSY)
        return;
    if (ret != RT_EOK) {
        rt_base_t level;

        level = rt_hw_interrupt_disable();
        pmu->cycle.shutting_down = false;
        rt_hw_interrupt_enable(level);
        pmu_cycle_restore_routes(pmu);
        rt_kprintf("[pmu] power cycle: arm wake alarm failed (%d)\n",
               ret);
        return;
    }

    pmu_do_shutdown("rtc-power-cycle");

    {
        rt_base_t level;

        level = rt_hw_interrupt_disable();
        pmu->cycle.shutting_down = false;
        rt_hw_interrupt_enable(level);
    }
    pmu_cycle_restore_routes(pmu);
}

rt_err_t pmu_schedule_power_cycle(struct pmu_dev *pmu,
                  const struct pmu_power_cycle_cfg *cfg)
{
    time_t now;
    rt_base_t level;
    bool busy;
    int ret;

    if ((cfg->shutdown_after_s < PMU_POWER_CYCLE_MIN_DELAY_S) ||
        (cfg->poweron_after_s < PMU_POWER_CYCLE_MIN_DELAY_S) ||
        (cfg->flags != 0U))
        return -RT_EINVAL;

    if (!pmu->rtc.initialized)
        return -RT_ERROR;

    ret = k230_pmu_rtc_get_time(&now);
    if (ret != RT_EOK)
        return ret;

    level = rt_hw_interrupt_disable();
    busy = pmu->cycle.active || pmu->cycle.shutting_down;
    rt_hw_interrupt_enable(level);
    if (busy)
        return -RT_EBUSY;

    pmu_cycle_save_routes(pmu);
    pmu_rtc_set_alarm_route(pmu, true, false);

    level = rt_hw_interrupt_disable();
    pmu->cycle.shutdown_time = now + cfg->shutdown_after_s;
    pmu->cycle.poweron_time = pmu->cycle.shutdown_time +
                  cfg->poweron_after_s;
    pmu->cycle.poweron_after_s = cfg->poweron_after_s;
    pmu->cycle.active = true;
    pmu->cycle.shutting_down = false;
    rt_hw_interrupt_enable(level);

    ret = pmu_rtc_set_alarm_time(pmu, pmu->cycle.shutdown_time);
    if (ret != RT_EOK) {
        level = rt_hw_interrupt_disable();
        pmu->cycle.active = false;
        pmu->cycle.shutting_down = false;
        rt_hw_interrupt_enable(level);
        pmu_cycle_restore_routes(pmu);
        return ret;
    }

    rt_kprintf("[pmu] power cycle: shutdown after %u s, wake after %u s\n",
           cfg->shutdown_after_s, cfg->poweron_after_s);
    return RT_EOK;
}

rt_err_t pmu_cancel_power_cycle(struct pmu_dev *pmu)
{
    rt_base_t level;
    bool was_active;

    level = rt_hw_interrupt_disable();
    was_active = pmu->cycle.active || pmu->cycle.shutting_down;
    pmu->cycle.active = false;
    pmu->cycle.shutting_down = false;
    rt_hw_interrupt_enable(level);

    if (was_active) {
        pmu_rtc_stop_alarm_hw(pmu);
        pmu_cycle_restore_routes(pmu);
    }

    return RT_EOK;
}

void pmu_rtc_handle_irq(struct pmu_dev *pmu, uint32_t status)
{
    if (status & PMU_IRQ_RTC_ALARM) {
        if (pmu->cycle.active && !pmu->cycle.shutting_down) {
            pmu->cycle.shutting_down = true;
            if (pmu->rtc.base != RT_NULL) {
                pmu_rtc_stop_alarm_locked(pmu);
                pmu_rtc_clear_alarm_locked(pmu);
            }
            pmu_post_work(pmu, PMU_WORK_CYCLE_SHUTDOWN);
            status &= ~PMU_IRQ_RTC_ALARM;
        }
    }

    if ((status & PMU_IRQ_RTC_MASK) == 0U)
        return;

    if (status & PMU_IRQ_RTC_ALARM) {
        pmu_rtc_stop_alarm_locked(pmu);
        pmu_rtc_clear_alarm_locked(pmu);
    }

    if (pmu->rtc.callback != RT_NULL)
        pmu->rtc.callback();
}

static void pmu_rtc_init_wakeup_state(struct pmu_dev *pmu)
{
    rt_base_t level;
    uint32_t status;

    status = pmu_readl(pmu, PMU_INT_STATE_REG);
    if ((status & (PMU_INT_STATE_RTC_ALARM_INPUT_MASK |
               PMU_IRQ_RTC_ALARM)) == 0U)
        return;

    level = rt_hw_interrupt_disable();
    pmu_update_bits_locked(pmu, PMU_INT0_TO_CPU_REGISTER,
                   PMU_IRQ_RTC_ALARM, false);
    pmu_rtc_stop_alarm_locked(pmu);
    pmu_rtc_clear_alarm_locked(pmu);
    rt_hw_interrupt_enable(level);
}

static int pmu_rtc_init_hardware(struct pmu_dev *pmu)
{
    rt_base_t level;
    int ret;
    uint32_t rtc_irq_mask;
    uint32_t rtc_det_mask;

    ret = pmu_ensure_access(pmu);
    if (ret != RT_EOK)
        return ret;

    if (pmu_get_rtc_base(pmu) == RT_NULL)
        return -RT_ERROR;

    if (pmu->rtc.initialized)
        return RT_EOK;

    rtc_irq_mask = PMU_IRQ_RTC_ALARM | PMU_IRQ_RTC_TICK;
    rtc_det_mask = PMU_DET_RTC_ALARM | PMU_DET_RTC_TICK;

    level = rt_hw_interrupt_disable();
    pmu_update_bits_locked(pmu, PMU_INT0_TO_CPU_REGISTER, rtc_irq_mask, true);
    pmu_update_bits_locked(pmu, PMU_INT0_TO_CTL_REGISTER, rtc_irq_mask, false);
    pmu_update_bits_locked(pmu, PMU_INT1_TO_CTL_REGISTER, rtc_irq_mask, false);
    pmu_update_bits_locked(pmu, PMU_INT_DETECT_EN, rtc_det_mask, true);
    pmu_rtc_stop_tick_locked(pmu);
    pmu->rtc.initialized = true;
    rt_hw_interrupt_enable(level);

    pmu_rtc_init_wakeup_state(pmu);
    return RT_EOK;
}

int k230_pmu_rtc_get_time(time_t *time)
{
    struct pmu_dev *pmu = pmu_get_dev();

    if (time == RT_NULL)
        return -RT_EINVAL;

    if (!pmu->rtc.initialized)
        return -RT_ERROR;

    return pmu_rtc_read_time_locked(pmu, time);
}

int k230_pmu_rtc_set_time(const time_t *time)
{
    struct pmu_dev *pmu = pmu_get_dev();
    struct tm *tm;
    rt_base_t level;
    int ret;

    if (time == RT_NULL)
        return -RT_EINVAL;

    if (!pmu->rtc.initialized)
        return -RT_ERROR;

    tm = gmtime(time);
    if (tm == RT_NULL)
        return -RT_ERROR;

    level = rt_hw_interrupt_disable();
    ret = pmu_rtc_write_time_tm(pmu, tm);
    rt_hw_interrupt_enable(level);

    return ret;
}

int k230_pmu_rtc_get_alarm(struct tm *tm)
{
    struct pmu_dev *pmu = pmu_get_dev();

    if (tm == RT_NULL)
        return -RT_EINVAL;

    if (!pmu->rtc.initialized)
        return -RT_ERROR;

    return pmu_rtc_read_alarm_locked(pmu, tm);
}

int k230_pmu_rtc_set_alarm(const struct kd_alarm_setup *setup)
{
    struct pmu_dev *pmu = pmu_get_dev();
    rt_base_t level;
    int ret;

    if (setup == RT_NULL)
        return -RT_EINVAL;

    if (!pmu->rtc.initialized)
        return -RT_ERROR;

    if (pmu_rtc_is_tick_mode(setup->flag)) {
        if (!pmu_rtc_is_valid_tick_mode(setup->flag))
            return -RT_EINVAL;
        if (pmu_cycle_owns_alarm(pmu))
            return -RT_EBUSY;

        level = rt_hw_interrupt_disable();
        pmu_rtc_stop_alarm_locked(pmu);
        pmu_rtc_clear_alarm_locked(pmu);
        pmu_rtc_configure_mode(pmu, setup->flag);
        rt_hw_interrupt_enable(level);
        return RT_EOK;
    }

    if (!pmu_rtc_is_valid_alarm_mode(setup->flag))
        return -RT_EINVAL;

    if (pmu_cycle_owns_alarm(pmu))
        return -RT_EBUSY;

    level = rt_hw_interrupt_disable();
    pmu_rtc_set_alarm_route_locked(pmu, true, false);
    pmu_rtc_stop_tick_locked(pmu);
    pmu_rtc_stop_alarm_locked(pmu);
    pmu_rtc_clear_alarm_locked(pmu);
    ret = pmu_rtc_program_alarm(pmu, &setup->tm, setup->flag);
    rt_hw_interrupt_enable(level);

    return ret;
}

void k230_pmu_rtc_stop_alarm(void)
{
    struct pmu_dev *pmu = pmu_get_dev();

    if (pmu_cycle_owns_alarm(pmu))
        return;

    if (!pmu->rtc.initialized)
        return;

    pmu_rtc_stop_alarm_hw(pmu);
}

void k230_pmu_rtc_stop_tick(void)
{
    struct pmu_dev *pmu = pmu_get_dev();
    rt_base_t level;

    if (!pmu->rtc.initialized)
        return;

    level = rt_hw_interrupt_disable();
    pmu_rtc_stop_tick_locked(pmu);
    rt_hw_interrupt_enable(level);
}

static rt_err_t pmu_rtc_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;
    return RT_EOK;
}

static rt_err_t pmu_rtc_close(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

static rt_size_t pmu_rtc_read(rt_device_t dev, rt_off_t pos, void *buffer,
                  rt_size_t size)
{
    (void)dev;
    (void)pos;
    (void)buffer;
    (void)size;
    return RT_ERROR;
}

static rt_size_t pmu_rtc_write(rt_device_t dev, rt_off_t pos,
                   const void *buffer, rt_size_t size)
{
    (void)dev;
    (void)pos;
    (void)buffer;
    (void)size;
    return RT_ERROR;
}

static rt_err_t pmu_rtc_control(rt_device_t dev, int cmd, void *args)
{
    struct pmu_dev *pmu = pmu_get_dev();

    RT_ASSERT(dev != RT_NULL);

    switch (cmd) {
    case RT_DEVICE_CTRL_RTC_GET_TIME:
        return k230_pmu_rtc_get_time((time_t *)args);
    case RT_DEVICE_CTRL_RTC_SET_TIME:
        return k230_pmu_rtc_set_time((const time_t *)args);
    case RT_DEVICE_CTRL_RTC_GET_ALARM:
        return k230_pmu_rtc_get_alarm((struct tm *)args);
    case RT_DEVICE_CTRL_RTC_SET_ALARM:
        return k230_pmu_rtc_set_alarm((const struct kd_alarm_setup *)args);
    case RT_DEVICE_CTRL_RTC_STOP_ALARM:
        k230_pmu_rtc_stop_alarm();
        return RT_EOK;
    case RT_DEVICE_CTRL_RTC_STOP_TICK:
        k230_pmu_rtc_stop_tick();
        return RT_EOK;
    case RT_DEVICE_CTRL_RTC_SET_CALLBACK:
        pmu->rtc.callback = args;
        return RT_EOK;
    default:
        return RT_EINVAL;
    }
}

static const struct rt_device_ops g_pmu_rtc_ops = {
    .init = RT_NULL,
    .open = pmu_rtc_open,
    .close = pmu_rtc_close,
    .read = pmu_rtc_read,
    .write = pmu_rtc_write,
    .control = pmu_rtc_control,
};

static int pmu_rtc_register_device(struct pmu_dev *pmu)
{
    int ret;

    if (pmu->rtc.device_registered)
        return RT_EOK;

    pmu->rtc.device.type = RT_Device_Class_RTC;
    pmu->rtc.device.ops = &g_pmu_rtc_ops;
    pmu->rtc.device.user_data = pmu;

    ret = rt_device_register(&pmu->rtc.device, "rtc",
                 RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK)
        return ret;

    pmu->rtc.device_registered = true;
    return RT_EOK;
}

int pmu_init_rtc(struct pmu_dev *pmu)
{
    time_t default_time;
    int ret;

    ret = pmu_rtc_init_hardware(pmu);
    if (ret != RT_EOK) {
        rt_kprintf("[rtc] init failed: RTC access is unavailable\n");
        return ret;
    }

    ret = pmu_rtc_register_device(pmu);
    if (ret != RT_EOK) {
        rt_kprintf("[rtc] register device failed: %d\n", ret);
        return ret;
    }

#ifndef RT_FASTBOOT
    rt_kprintf("rtc driver register OK\n");
#endif

    if (!pmu_rtc_time_regs_valid(pmu)) {
        default_time = pmu_rtc_default_time();
        ret = k230_pmu_rtc_set_time(&default_time);
        if (ret != RT_EOK)
            return ret;
    }

    return RT_EOK;
}
