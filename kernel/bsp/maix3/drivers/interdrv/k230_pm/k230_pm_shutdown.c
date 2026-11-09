/* SPDX-License-Identifier: BSD-2-Clause */
#include <rtthread.h>
#include <rthw.h>
#include <stdarg.h>
#include "k230_pm.h"
#include "k230_pm_thermal.h"
#include "sysctl_clk.h"
#ifdef RT_USING_WDT
#include "wdt_hw.h"
#endif
#ifdef RT_USING_RTC_PMU
#include "drv_pmu.h"
#endif

/* Interrupts are off from here on: rt_kprintf may write to an ADB or telnet
 * console that blocks and reschedules, so print through the polled SBI
 * console. */
static void thermal_print(const char *fmt, ...)
{
    static char buf[128];
    va_list args;

    va_start(args, fmt);
    rt_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    rt_hw_console_output(buf);
}

/* Terminal shutdown: refresh WDT1 if its driver started it, never enable it.
 * The bare driver is ready only while the driver keeps WDT1 clocked. */
static void thermal_feed_watchdog(void)
{
#ifdef RT_USING_WDT
    if (wdt_hw_enabled())
        wdt_hw_restart();
#endif
}

#ifdef K230_PM_THERMAL_COOLDOWN_RESTART
#include "ts_hw.h"
#include "sysctl_boot.h"
#include "tick.h"

/* Restart only when cooling can be confirmed and a reboot can succeed: not
 * after a sensor outage, and not after a thermal initialization failure. */
static bool thermal_restart_allowed(unsigned int reason)
{
    return reason == K230_THERMAL_CRITICAL || reason == K230_THERMAL_SUSTAINED ||
        reason == K230_THERMAL_ENFORCEMENT;
}

/* Busy-wait 1 ms on the CPU timer, servicing the watchdog. */
static void thermal_shutdown_wait_1ms(void)
{
    uint64_t start = cpu_ticks();

    do
        thermal_feed_watchdog();
    while (cpu_ticks() - start < TIMER_CLK_FREQ / 1000);
}

/* The frozen sampler may have left the sensor mid-conversion: reconfigure,
 * then take one single-mode conversion. */
static int thermal_shutdown_read(int32_t *millicelsius)
{
    ts_hw_configure(TS_HW_DEFAULT_TRIM, false);
    return ts_hw_read_single(millicelsius, thermal_shutdown_wait_1ms);
}

/* Interrupts stay disabled: time comes from the CPU timer, the sensor is read
 * through the bare driver without sleeping, and the watchdogs are serviced
 * throughout. Any failed or warm reading restarts the cooldown hold. External
 * regulators keep the low voltages DVFS left them at across the reset; SPL
 * restores the boot voltages before the big core runs. */
static void thermal_cooldown_restart(void)
{
    struct k230_thermal_config config;
    uint64_t hold, last, since = 0;
    bool cool = false;

    k230_pm_thermal_config(&config);
    hold = (uint64_t)config.restart_hold_ticks * TIMER_CLK_FREQ / config.tick_hz;
    thermal_print("thermal: halt fallback, watchdogs held; physical power-off unconfirmed; restart after %u s below %d mC\n",
        (unsigned)(config.restart_hold_ticks / config.tick_hz), config.restart_mc);
    last = cpu_ticks();
    for (;;) {
        int32_t temperature;
        uint64_t now;
        thermal_feed_watchdog();
        now = cpu_ticks();
        if (now - last < TIMER_CLK_FREQ)
            continue;
        last = now;
        if (thermal_shutdown_read(&temperature) != RT_EOK ||
            temperature >= config.restart_mc) {
            cool = false;
        } else if (!cool) {
            cool = true;
            since = now;
        } else if (now - since >= hold) {
            thermal_print("thermal: cooled to %d mC, restarting\n", temperature);
            thermal_feed_watchdog();
            sysctl_boot_reset_soc();
        }
    }
}
#endif

/* Both CPU shutdown entry points enter the board hook with interrupts disabled. */
void rt_hw_board_shutdown(void)
{
    unsigned int still_on;

    if (!k230_pm_shutdown_terminal()) {
#ifdef RT_USING_RTC_PMU
        k230_pmu_shutdown();
#endif
        return;
    }

    /* Final best effort, even if a suspended actuator owns every PM mutex.
     * These raw helpers are register-only. Voltage is left unchanged; the
     * selected clocks are existing minimum profiles. No client resumes after
     * these writes, so resource-vote accounting is deliberately bypassed.
     * Switchable domains (camera, AI, video, 2D, display, little core) go
     * idle and power off while still clocked, as in k230_pm_put(); the
     * sampler's critical path reaches here without any earlier quiescence,
     * and a halt fallback otherwise leaves them powered. */
    still_on = k230_pm_thermal_quiesce_raw();
    (void)sysctl_media_clk_set_leaf_parent_div_raw(SYSCTL_CLK_CPU1_CORE_CLK_ROOT,
        SYSCTL_CLK_PLL0_OCLK, 1, 8);
    (void)sysctl_media_clk_set_leaf_parent_div_raw(SYSCTL_CLK_AI_CORE_CLK,
        SYSCTL_CLK_PLL3_OCLK_DIV2, 1, 8);
    sysctl_clk_set_leaf_en(SYSCTL_CLK_AI_CORE_CLK, false);
    sysctl_clk_set_leaf_en(SYSCTL_CLK_DPU_CLK, false);
    sysctl_clk_set_leaf_en(SYSCTL_CLK_VPU_CLK_ROOT, false);
    if (still_on)
        thermal_print("thermal: power domains still on mask=0x%x\n", still_on);
#ifdef K230_PM_THERMAL_SHUTDOWN_PMU
    thermal_feed_watchdog();
    thermal_print("thermal: requesting PMU power-off\n");
    if (k230_pmu_shutdown_thermal() != RT_EOK)
        thermal_print("thermal: PMU mapping unavailable\n");
#endif
#ifdef K230_PM_THERMAL_COOLDOWN_RESTART
    if (thermal_restart_allowed(k230_pm_shutdown_reason()))
        thermal_cooldown_restart();
#endif
    thermal_print("thermal: halt fallback, watchdogs held; physical power-off unconfirmed\n");
    for (;;) {
        thermal_feed_watchdog();
        /* IRQs remain disabled. WFI cannot be used: an enabled watchdog must
         * be serviced even when no interrupt can wake this hart. */
        __asm__ volatile("nop");
    }
}

void k230_pm_thermal_shutdown_enter(void)
{
    /* rt_hw_cpu_shutdown() prints through the selected RT-Thread console before
     * masking interrupts. ADB and telnet consoles can wait forever on their TX
     * mutexes, so the thermal path enters the board hook directly. */
    rt_hw_interrupt_disable();
    rt_hw_board_shutdown();

    /* A latched thermal board hook is terminal. Keep a defensive fallback in
     * case a future backend returns unexpectedly. */
    for (;;)
        __asm__ volatile("nop");
}
