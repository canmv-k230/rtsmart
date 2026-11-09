#!/usr/bin/env python3
"""Exercise the terminal thermal shutdown and cooldown restart.

Run with python3 -B kernel/bsp/maix3/testcase/host/test_thermal_restart.py.
The production k230_pm_shutdown.c runs against a fake CPU timer, a scripted
temperature sensor and a watchdog. Interrupts are off in this path, so any
RTOS wait asserts. Boot voltages are SPL's job and not touched here. No board
or cross compiler is needed.
"""

from pathlib import Path
import os
import subprocess
import tempfile


PM = Path(__file__).resolve().parents[2] / "drivers/interdrv/k230_pm"

STUBS = r"""
#pragma once
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define RT_EOK 0
#define RT_ETIMEOUT 2
#define TIMER_CLK_FREQ 1000000
#define SYSCTL_CLK_CPU1_CORE_CLK_ROOT 10
#define SYSCTL_CLK_AI_CORE_CLK 11
#define SYSCTL_CLK_DPU_CLK 12
#define SYSCTL_CLK_VPU_CLK_ROOT 13
#define SYSCTL_CLK_PLL0_OCLK 14
#define SYSCTL_CLK_PLL3_OCLK_DIV2 15
enum k230_thermal_reason {
    K230_THERMAL_NONE, K230_THERMAL_CRITICAL, K230_THERMAL_SUSTAINED,
    K230_THERMAL_ENFORCEMENT, K230_THERMAL_SENSOR_OUTAGE, K230_THERMAL_INIT_FAILURE
};
struct k230_thermal_config { uint32_t tick_hz, restart_hold_ticks; int restart_mc; };

/* Outcomes of one shutdown: it returned, reset the SoC, halted for the whole
 * observation window, or the PMU removed power. */
enum { RETURNED, RESET, HALTED, POWERED_OFF };
#define READ_FAILS INT32_MIN
#define WINDOW_US 8000000

static unsigned reason, kicks, samples, script_len;
static int32_t script[8];
static uint64_t now_us, reset_at_us;
static bool pmu_poweroff;
static jmp_buf escape;

static uint64_t cpu_ticks(void) { now_us += 100; return now_us; }
static void rt_thread_mdelay(unsigned ms) { assert(!"no RTOS waits in terminal shutdown"); }
static unsigned k230_pm_shutdown_reason(void) { return reason; }
static bool k230_pm_shutdown_terminal(void) {
#ifdef K230_PM_THERMAL_SHUTDOWN_NONE
    return false;
#else
    return reason != K230_THERMAL_NONE;
#endif
}
static unsigned k230_pm_thermal_quiesce_raw(void) { return 0; }
static int sysctl_media_clk_set_leaf_parent_div_raw(int leaf, int parent, int n, int d) {
    /* Clocks drop to their minimum profiles; voltage is left alone. */
    assert(n == 1 && d == 8); return 0;
}
static void sysctl_clk_set_leaf_en(int leaf, bool on) { assert(!on); }
static void k230_pm_thermal_config(struct k230_thermal_config *c) {
    *c = (struct k230_thermal_config){ 1000, 2000, 70000 }; /* 2 s below 70 C */
}
static void rt_kprintf(const char *fmt, ...) {}
static int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    return vsnprintf(buf, size, fmt, args);
}
static void rt_hw_console_output(const char *str) { (void)str; }
static int rt_hw_interrupt_disable(void) { return 0; }

/* WDT1 started by its driver; each refresh counts and moves time. */
#define RT_USING_WDT
static bool wdt_hw_enabled(void) { return true; }
static void wdt_hw_restart(void) {
    kicks++;
    now_us += 10;
    if (now_us > WINDOW_US) longjmp(escape, HALTED);
}

#define TS_HW_DEFAULT_TRIM 8
static void ts_hw_configure(uint8_t trim, bool continuous) { assert(!continuous); }
static int ts_hw_read_single(int32_t *t, void (*wait_1ms)(void)) {
    int32_t value = script[samples < script_len ? samples : script_len - 1];
    samples++;
    wait_1ms();
    if (value == READ_FAILS) return -RT_ETIMEOUT;
    *t = value;
    return RT_EOK;
}

static void sysctl_boot_reset_soc(void) { reset_at_us = now_us; longjmp(escape, RESET); }
static int k230_pmu_shutdown_thermal(void) {
    if (pmu_poweroff) longjmp(escape, POWERED_OFF);
    return 0; /* request accepted but rails stay on: exercise the fallback */
}
static void k230_pmu_shutdown(void) {}
"""

CASES = r"""
static int run(unsigned why, unsigned n, const int32_t *temps) {
    reason = why;
    kicks = samples = 0;
    now_us = reset_at_us = 0;
    script_len = n;
    memcpy(script, temps, n * sizeof(*temps));
    int outcome = setjmp(escape);
    if (!outcome) {
        rt_hw_board_shutdown();
        return RETURNED;
    }
    return outcome;
}

int main(void) {
    static const int32_t cool[] = { 60000 };

    /* A normal shutdown request is not a thermal one. */
    assert(run(K230_THERMAL_NONE, 1, cool) == RETURNED && !samples);

#ifdef K230_PM_THERMAL_SHUTDOWN_NONE
    /* NONE leaves every thermal reason nonterminal. The controller, tested in
     * test_thermal_startup.py, retains minimum caps without blocking work. */
    for (unsigned why = K230_THERMAL_CRITICAL; why <= K230_THERMAL_INIT_FAILURE; ++why)
        assert(run(why, 1, cool) == RETURNED && !samples && !kicks);
    puts("PASS thermal shutdown: none backend remains nonterminal");
    return 0;
#endif

#ifdef K230_PM_THERMAL_SHUTDOWN_PMU
    pmu_poweroff = true;
    assert(run(K230_THERMAL_CRITICAL, 1, cool) == POWERED_OFF && !samples);
    pmu_poweroff = false;
#endif

#ifdef K230_PM_THERMAL_COOLDOWN_RESTART
    static const int32_t warm_in_hold[] = { 60000, 75000, 60000 };
    static const int32_t at_threshold[] = { 70000 };
    static const int32_t failing[] = { READ_FAILS };
    static const int32_t fail_in_hold[] = { 60000, READ_FAILS, 60000 };

    /* Heat shutdowns restart after the hold: one reading per second, the
     * first cool one starts the hold, the reset follows 2 s later. */
    for (unsigned why = K230_THERMAL_CRITICAL; why <= K230_THERMAL_ENFORCEMENT; ++why) {
        assert(run(why, 1, cool) == RESET);
        assert(samples == 3 && kicks > 0);
        assert(reset_at_us >= 3000000 && reset_at_us < 3100000);
    }
    /* A warm or failed reading restarts the hold. */
    assert(run(K230_THERMAL_CRITICAL, 3, warm_in_hold) == RESET && samples == 5);
    assert(reset_at_us >= 5000000 && reset_at_us < 5100000);
    assert(run(K230_THERMAL_CRITICAL, 3, fail_in_hold) == RESET && samples == 5);
    /* The threshold itself is not cool, and failing reads never restart. */
    assert(run(K230_THERMAL_CRITICAL, 1, at_threshold) == HALTED && samples >= 7);
    assert(run(K230_THERMAL_CRITICAL, 1, failing) == HALTED && samples >= 7);
#else
    assert(run(K230_THERMAL_CRITICAL, 1, cool) == HALTED && !samples && kicks > 0);
#endif
    /* Cooling cannot be confirmed after these: halt without reading. */
    assert(run(K230_THERMAL_SENSOR_OUTAGE, 1, cool) == HALTED && !samples && kicks > 0);
    assert(run(K230_THERMAL_INIT_FAILURE, 1, cool) == HALTED && !samples && kicks > 0);
    puts("PASS thermal shutdown: cooldown hold, restart reasons, halt and PMU backend");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="k230-restart-test-") as directory:
        build = Path(directory)
        (build / "stubs.h").write_text(STUBS)
        for name in ("rtthread.h", "rthw.h", "k230_pm.h", "k230_pm_thermal.h", "sysctl_clk.h",
                     "wdt_hw.h", "drv_pmu.h", "ts_hw.h", "sysctl_boot.h", "tick.h"):
            (build / name).write_text('#include "stubs.h"\n')
        # A copy, so its includes resolve to the stubs, not the real headers.
        (build / "k230_pm_shutdown.c").write_text((PM / "k230_pm_shutdown.c").read_text())
        source = build / "test.c"
        source.write_text('#include "k230_pm_shutdown.c"\n' + CASES)
        variants = [(False, False, True)]
        variants += [(restart, pmu, False) for restart in (True, False) for pmu in (False, True)]
        for restart, pmu, none in variants:
            flags = []
            if restart:
                flags.append("-DK230_PM_THERMAL_COOLDOWN_RESTART")
            if pmu:
                flags += ["-DK230_PM_THERMAL_SHUTDOWN_PMU", "-DRT_USING_RTC_PMU"]
            if none:
                flags.append("-DK230_PM_THERMAL_SHUTDOWN_NONE")
            binary = build / "test"
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-unused-parameter",
                "-fsanitize=undefined", "-fno-sanitize-recover=all", *flags,
                "-I", str(build), str(source), "-o", str(binary),
            ], check=True)
            print(f"cooldown_restart={restart} pmu={pmu} none={none}", flush=True)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
