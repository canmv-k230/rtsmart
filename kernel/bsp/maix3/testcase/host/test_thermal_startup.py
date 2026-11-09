#!/usr/bin/env python3
"""Exercise thermal startup with production controller, policy, tables and DVFS.

DVFS initialization applies the default profiles without waiting for a reading;
the host fixture then schedules the sampler/controller passes and records every
clock/voltage write. Test both fixed rails and configured regulators with UBSan.
Run with python3 -B kernel/bsp/maix3/testcase/host/test_thermal_startup.py.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

BSP = Path(__file__).resolve().parents[2]
PM = BSP / "drivers/interdrv/k230_pm"

STUBS = r"""
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
typedef uint32_t rt_tick_t;
typedef uint32_t rt_uint32_t;
typedef int rt_base_t;
typedef int rt_spinlock_t;
struct rt_mutex { bool held; };
struct rt_event { unsigned events; };
struct rt_thread { void (*entry)(void *); };
typedef struct rt_thread *rt_thread_t;
#define RT_NULL NULL
#define RT_EOK 0
#define RT_ETIMEOUT 2
#define RT_EEMPTY 4
#define RT_ENOSYS 6
#define RT_EBUSY 7
#define RT_EINVAL 10
#define RT_WAITING_FOREVER (-1)
#define RT_TICK_PER_SECOND 1000
#define RT_IPC_FLAG_PRIO 0
#define RT_EVENT_FLAG_OR 1
#define RT_EVENT_FLAG_CLEAR 2
#define RT_THREAD_PRIORITY_MAX 32
#define rt_kprintf(...) do { assert(!device_console_locked); } while (0)
typedef enum {
    SYSCTL_CLK_CPU1_CORE_CLK_ROOT, SYSCTL_CLK_AI_CORE_CLK,
    SYSCTL_CLK_VPU_CLK_ROOT, SYSCTL_CLK_CPU1_CORE_CLK_SRC,
    SYSCTL_CLK_DPU_CLK, SYSCTL_CLK_PLL0_OCLK, SYSCTL_CLK_PLL0_OCLK_DIV2,
    SYSCTL_CLK_PLL1_OCLK_DIV2, SYSCTL_CLK_PLL1_OCLK_DIV4,
    SYSCTL_CLK_PLL3_OCLK_DIV2, SYSCTL_CLK_NODE_MAX
} sysctl_clk_node_e;
typedef enum { SYSCTL_PD_AI, SYSCTL_PD_DPU, SYSCTL_PD_VPU } sysctl_pwr_domain_e;
typedef int sysctl_reset_e;
struct regulator_dev { int uv; };
static struct regulator_dev rails[2];
static uint32_t now, boot_tick, rates[SYSCTL_CLK_NODE_MAX];
static unsigned clock_writes, voltage_writes, sampler_calls, sample_calls;
static unsigned polled_console_writes;
static unsigned scenario, boot_mode;
static bool in_sampler, in_controller, device_console_locked;
static jmp_buf controller_exit, sampler_exit, shutdown_exit;
static rt_tick_t rt_tick_get(void) { return now; }
static rt_tick_t rt_tick_from_millisecond(unsigned ms) { return ms; }
static void rt_thread_mdelay(unsigned ms) { now += ms; }
static int rt_mutex_init(struct rt_mutex *m, const char *name, int flags) { m->held = false; return 0; }
static int rt_mutex_take(struct rt_mutex *m, int timeout) { assert(!m->held); m->held = true; return 0; }
static int rt_mutex_release(struct rt_mutex *m) { assert(m->held); m->held = false; return 0; }
static void rt_spin_lock_init(rt_spinlock_t *lock) {}
static rt_base_t rt_spin_lock_irqsave(rt_spinlock_t *lock) { return 0; }
static void rt_spin_unlock_irqrestore(rt_spinlock_t *lock, rt_base_t level) {}
static int rt_event_init(struct rt_event *event, const char *name, int flags) { return 0; }
static int rt_event_send(struct rt_event *event, unsigned bits) { event->events |= bits; return 0; }
static int rt_event_recv(struct rt_event *, unsigned, int, rt_tick_t, unsigned *);
static rt_thread_t rt_thread_create(const char *name, void (*entry)(void *), void *arg,
    unsigned stack, unsigned priority, unsigned slice)
{
    static struct rt_thread threads[2];
    unsigned index = strcmp(name, "thermal") == 0;
    threads[index].entry = entry;
    return &threads[index];
}
static int rt_thread_startup(rt_thread_t thread);
static int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    return vsnprintf(buf, size, fmt, args);
}
static void rt_hw_console_output(const char *str) { (void)str; polled_console_writes++; }
static void k230_pm_thermal_shutdown_enter(void) {
#ifdef K230_PM_THERMAL_SHUTDOWN_NONE
    assert(!"NONE backend entered terminal shutdown");
#else
    longjmp(shutdown_exit, 1);
#endif
}
/* Board rails exist only in the regulated configurations. */
static struct regulator_dev *regulator_get(const char *name) {
    if (!TEST_RAILS || !name) return NULL;
    if (!strcmp(name, "regulator_cpu")) return &rails[0];
    if (!strcmp(name, "regulator_kpu")) return &rails[1];
    return NULL;
}
uint32_t sysctl_media_clk_get_leaf_freq(sysctl_clk_node_e node) {
    return node < SYSCTL_CLK_NODE_MAX ? rates[node] : 0;
}
static int regulator_status(struct regulator_dev *rail) { return 0; }
int regulator_set_voltage(struct regulator_dev *, int, int);
int regulator_get_voltage(struct regulator_dev *, int *);
"""

MODEL = r"""
static const sysctl_clk_node_e leaves[] = {
    SYSCTL_CLK_CPU1_CORE_CLK_ROOT, SYSCTL_CLK_AI_CORE_CLK, SYSCTL_CLK_VPU_CLK_ROOT
};
static const uint32_t boot_rates[][3] = {
    {1600000000, 800000000, 800000000},
    {1188000000, 400000000, 400000000},
    {777000000, 350000000, 350000000}, /* valid boot rates outside the table */
    {200000000, 100000000, 200000000},
};
static void assert_default_profiles(void)
{
    for (unsigned d = 0; d < 3; ++d)
        assert(rates[leaves[d]] == boot_rates[0][d]);
}
int k230_clk_set_parent_div(sysctl_clk_node_e node, sysctl_clk_node_e parent,
    uint32_t numerator, uint32_t denominator)
{
    if (!thermal_rate_allowed(node, parent, numerator, denominator))
        return -1;
    uint32_t hz = (uint64_t)rates[parent] * numerator / denominator;
    /* Raising a clock requires the corresponding rail to be ready first. */
    for (unsigned d = 0; d < k230_dvfs_domain_desc_count; ++d) {
        const struct k230_dvfs_domain_desc *desc = &k230_dvfs_domain_desc[d];
        struct regulator_dev *rail = regulator_get(desc->regulator_name);
        if (desc->steps[0].leaf != node || !rail) continue;
        for (unsigned i = 0; i < desc->step_count; ++i)
            if (abs(desc->steps[i].freq - (int)hz) <= 1000)
                assert(rail->uv >= desc->steps[i].volt);
    }
    rates[node] = hz;
    ++clock_writes;
    return 0;
}
int regulator_set_voltage(struct regulator_dev *rail, int min_uv, int max_uv)
{
    assert(min_uv <= max_uv);
    rail->uv = min_uv;
    ++voltage_writes;
    return 0;
}
int regulator_get_voltage(struct regulator_dev *rail, int *uv) { *uv = rail->uv; return 0; }
bool k230_pd_hw_powered(sysctl_pwr_domain_e pd) { return true; }
int k230_pm_thermal_off(enum k230_pm_dev_id dev, bool force) { return 0; }
/* The bare sensor driver; the thermal reading cache is the real one. */
int ts_hw_init(void) { return scenario == 4 ? -RT_ENOSYS : 0; }
void ts_hw_configure(uint8_t trim, bool continuous) {}
uint32_t ts_hw_ctrl(void) { return 0; }
int ts_hw_read_single(int32_t *millicelsius, void (*wait_1ms)(void))
{
    ++sample_calls;
    now += 20;
    if (scenario == 3 || ((scenario == 6 || scenario == 8) && sample_calls == 2))
        return -RT_ETIMEOUT;
    *millicelsius = scenario == 1 ? 51000 : scenario == 2 ? 54000 :
        (scenario == 5 || ((scenario == 6 || scenario == 7 || scenario == 8) && sample_calls == 1) ||
        (scenario == 8 && sample_calls == 3)) ?
        55000 : 36000;
    return 0;
}
"""

CASES = r"""
static unsigned controller_passes, init_clock_writes, init_voltage_writes;
static uint32_t restricted_elapsed;
static struct k230_thermal_policy after_policy;
static uint32_t after_rates[3];
static bool after_blocked;
/* Threads are driven from main(); initialization must not depend on them. */
static int rt_thread_startup(rt_thread_t thread) { return 0; }
static void run_sampler(void)
{
    in_sampler = true;
    if (!setjmp(sampler_exit)) thermal_sampler(NULL);
    in_sampler = false;
}
static int rt_event_recv(struct rt_event *event, unsigned mask, int flags,
    rt_tick_t timeout, unsigned *received)
{
    if (in_sampler) longjmp(sampler_exit, 1);
    /* Only the monitor threads wait; DVFS initialization never blocks. */
    assert(in_controller && timeout > 0);
    if (!controller_passes++) {
        /* No reading yet: the default profiles stand, nothing is blocked. */
        assert(published_policy.required == K230_THERMAL_NORMAL);
        assert(!k230_pm_thermal_accelerators_blocked());
        assert(clock_writes == init_clock_writes && voltage_writes == init_voltage_writes);
        assert_default_profiles();
        run_sampler();
        return 0;
    }
    if ((scenario == 3 || scenario == 4) && published_policy.required != K230_THERMAL_MIN) {
        /* A sensor that never reports restricts only once its reading is stale. */
        assert(!k230_pm_thermal_accelerators_blocked());
        assert_default_profiles();
        now += timeout;
        run_sampler();
        return 0;
    }
    restricted_elapsed = now - boot_tick;
    after_policy = published_policy;
    after_blocked = k230_pm_thermal_accelerators_blocked();
    for (unsigned d = 0; d < 3; ++d) after_rates[d] = rates[leaves[d]];
    longjmp(controller_exit, 1);
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    scenario = atoi(argv[1]); boot_mode = atoi(argv[2]);
    boot_tick = now = atoi(argv[3]) ? UINT32_MAX - 30 : 80;
    rails[0].uv = boot_mode == 3 ? 620000 : boot_mode == 1 ? 750000 : 800000;
    rails[1].uv = 800000;
    for (unsigned d = 0; d < 3; ++d) rates[leaves[d]] = boot_rates[boot_mode][d];
    rates[SYSCTL_CLK_PLL0_OCLK] = 1600000000;
    rates[SYSCTL_CLK_PLL0_OCLK_DIV2] = 800000000;
    rates[SYSCTL_CLK_PLL1_OCLK_DIV2] = 1188000000;
    rates[SYSCTL_CLK_PLL1_OCLK_DIV4] = 594000000;
    rates[SYSCTL_CLK_PLL3_OCLK_DIV2] = 800000000;
    rates[SYSCTL_CLK_DPU_CLK] = 594000000;
#ifdef K230_PM_THERMAL_PROTECT
#ifndef K230_PM_THERMAL_SHUTDOWN_NONE
    if (setjmp(shutdown_exit)) {
        /* A critical first reading is confirmed at once, then powers off. */
        assert((scenario == 5 || scenario == 8) &&
            k230_pm_shutdown_reason() == K230_THERMAL_CRITICAL);
        assert(sample_calls == (scenario == 5 ? 2U : 3U));
        assert(device_console_locked && polled_console_writes > 0);
        return 0;
    }
#endif
#endif
    /* Default profiles apply at once, before any reading. */
    assert(k230_pm_dvfs_init() == 0);
    assert(k230_dvfs_ready && !sample_calls);
    assert_default_profiles();
#ifdef K230_PM_THERMAL_PROTECT
    assert(k230_dvfs_domain_desc[K230_PM_DOMAIN_VPU].caps & K230_DVFS_CAP_THERMAL);
    {
        struct k230_thermal_clocks clocks[3] = {
            {.actual_hz = 200000000}, {.actual_hz = 0}, {.actual_hz = 0}
        };
        bool powered[3] = {true, false, false};
        struct k230_thermal_config config;
        assert(thermal_verified_stage(clocks, powered, K230_THERMAL_MIN) ==
            K230_THERMAL_MIN);
        assert(thermal_clocks_within_stage(clocks, powered, K230_THERMAL_MIN));
        powered[K230_THERMAL_KPU] = true;
        assert(thermal_verified_stage(clocks, powered, K230_THERMAL_MIN) < 0);
        k230_pm_thermal_config(&config);
        assert(k230_dvfs_recovery_ticks() == config.recovery_ticks);
    }
    assert(!k230_pm_thermal_accelerators_blocked());
#ifdef K230_PM_THERMAL_SHUTDOWN_NONE
    k230_pm_thermal_block_accelerators(true);
    assert(!k230_pm_thermal_accelerators_blocked());
    k230_pm_thermal_block_accelerators(false);
#endif
    for (unsigned d = 0; d < 3; ++d)
        assert(k230_pm_thermal_limit(leaves[d]) == UINT32_MAX);
    init_clock_writes = clock_writes;
    init_voltage_writes = voltage_writes;
    /* A terminal critical path must bypass a blocked device console. NONE
     * keeps scheduling, so ordinary controller diagnostics stay available. */
#ifdef K230_PM_THERMAL_SHUTDOWN_NONE
    device_console_locked = false;
#else
    device_console_locked = scenario == 5 || scenario == 8;
#endif
    in_controller = true;
    if (!setjmp(controller_exit)) thermal_controller(NULL);
    in_controller = false;
#ifdef K230_PM_THERMAL_SHUTDOWN_NONE
    assert(controller_passes >= 2);
#else
    assert(scenario != 5 && scenario != 8 && controller_passes >= 2);
#endif
    unsigned stage = scenario == 1 ? K230_THERMAL_HOT :
        scenario >= 2 ? K230_THERMAL_MIN : K230_THERMAL_NORMAL;
#ifdef K230_PM_THERMAL_SHUTDOWN_NONE
    assert(after_policy.required == stage);
    if (scenario == 5 || scenario == 8) {
        assert(after_policy.stage == K230_THERMAL_SHUTDOWN);
        assert(!after_blocked);
        assert(k230_pm_shutdown_reason() == K230_THERMAL_CRITICAL);
        assert(polled_console_writes > 0);
    } else {
        assert(!after_blocked);
    }
#else
    assert(after_policy.required == stage && !after_blocked);
#endif
    if (scenario == 3 || scenario == 4) {
        struct k230_thermal_config config;
        k230_pm_thermal_config(&config);
        uint32_t stale = k230_thermal_fresh_ticks(&config, 0);
        assert(restricted_elapsed >= stale && restricted_elapsed <= stale + config.active_ticks + 20);
    }
    if (scenario == 6 || scenario == 7) {
        /* A failed or cool confirmation cannot erase the first observation:
         * it is applied as MIN and requires normal staged recovery. */
        assert(sample_calls >= 2);
        assert(after_policy.stage == K230_THERMAL_MIN);
        assert(k230_pm_shutdown_reason() == K230_THERMAL_NONE);
    }
    for (unsigned d = 0; d < 3; ++d) {
        uint32_t cap = thermal_stage_cap(stage, d);
        uint32_t expected = cap == UINT32_MAX ? boot_rates[0][d] : cap;
#ifdef K230_PM_THERMAL_REGULATION
        /* Regulation may cut further; protection must still bound every clock. */
        if (stage == K230_THERMAL_HOT) {
            assert(after_rates[d] > 0 && after_rates[d] <= expected + 1000);
            continue;
        }
#endif
        assert(abs((int)after_rates[d] - (int)expected) <= 1000);
    }
#endif
    return 0;
}
"""


def function(source, name):
    """Compile original function bodies unchanged, omitting unrelated governors."""
    match = re.search(r"^[\w *]+\b" + re.escape(name) + r"\([^;]+?\n\{", source, re.M)
    assert match, name
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end] + "\n"


def main():
    with tempfile.TemporaryDirectory(prefix="k230-startup-test-") as directory:
        build = Path(directory)
        (build / "stubs.h").write_text(STUBS)
        for name in ("rtthread.h", "rtdevice.h", "rthw.h", "rtdef.h", "sysctl_clk.h", "sysctl_pwr.h", "sysctl_rst.h"):
            (build / name).write_text('#include "stubs.h"\n')
        for header in PM.glob("*.h"):
            (build / header.name).write_text(header.read_text())
        (build / "ts_hw.h").write_text((BSP / "drivers/interdrv/tsensor/ts_hw.h").read_text())
        core = (PM / "k230_pm_core.c").read_text()
        core = core[core.index("static unsigned int thermal_shutdown_reason;"):core.index("static bool thermal_accelerator_pd(")] + function(core, "thermal_rate_allowed")
        tables = (PM / "k230_pm_tables.c").read_text()
        tables = tables[tables.index("#define DVFS_STEP("):tables.index("#define PM_DEV(")]
        dvfs = (PM / "k230_pm_dvfs.c").read_text()
        subset = dvfs[dvfs.index("#define PROFILE_IS_UP("):dvfs.index("static bool auto_load_enable;")]
        for name in ("k230_dvfs_domain_get", "k230_dvfs_profile_index_transform", "k230_dvfs_mc_to_c_floor",
                     "k230_dvfs_freq_match", "k230_dvfs_read_hw_profile", "k230_dvfs_set_clock_step",
                     "k230_dvfs_safe_voltage", "k230_dvfs_limit_index", "k230_dvfs_apply_locked",
                     "k230_dvfs_set_actual_profile_locked", "k230_dvfs_sample_fresh_ticks",
                     "k230_dvfs_recovery_ticks", "k230_dvfs_extra_limit",
                     "k230_pm_dvfs_thermal_shutdown_mc", "k230_pm_dvfs_thermal_apply", "k230_pm_dvfs_init"):
            subset += function(dvfs, name)
        source = build / "test.c"
        source.write_text('#include "stubs.h"\n#include "k230_pm.h"\n#include "k230_pm_dvfs.h"\n'
                          '#include "k230_pm_thermal.h"\n#include "ts_hw.h"\n'
                          + core + tables + MODEL + subset
                          + '\n#ifdef K230_PM_THERMAL_PROTECT\n#include "k230_pm_thermal_reading.c"\n'
                          '#include "k230_pm_thermal.c"\n#endif\n'
                          + CASES)
        for unit in ("k230_pm_thermal_reading.c", "k230_pm_thermal.c"):
            (build / unit).write_text((PM / unit).read_text())
        # No-protection build excludes the test scheduler and thermal-only cases.
        variants = [(protect, regulation, real, False)
                    for protect, regulation in ((True, True), (True, False), (False, False))
                    for real in (False, True)]
        variants.append((True, True, False, True))
        for protect, regulation, real, none in variants:
            flags = ["-DK230_PM_THERMAL_CRITICAL_C=55", "-DK230_PM_THERMAL_TARGET_C=45"]
            if protect:
                flags.append("-DK230_PM_THERMAL_PROTECT")
            if regulation:
                flags.append("-DK230_PM_THERMAL_REGULATION")
            if none:
                flags.append("-DK230_PM_THERMAL_SHUTDOWN_NONE")
            flags.append(f"-DTEST_RAILS={int(real)}")
            binary = build / "test"
            current = source.read_text()
            if not protect:
                current = current.replace(CASES, "#ifdef K230_PM_THERMAL_PROTECT\n" +
                    CASES[:CASES.index("int main(")] + "#endif\n" + CASES[CASES.index("int main("):])
            (build / "compile.c").write_text(current)
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-unused-variable",
                "-Wno-unused-but-set-variable", "-Wno-sign-compare", "-fsanitize=undefined",
                "-fno-sanitize-recover=all", *flags, "-I", str(build), str(build / "compile.c"),
                str(PM / "k230_pm_thermal_policy.c"), str(PM / "k230_pm_thermal_regulator.c"),
                "-o", str(binary)], check=True)
            for scenario in range(9 if protect else 1):
                for boot in range(4):
                    for wrap in (0, 1):
                        subprocess.run([str(binary), str(scenario), str(boot), str(wrap)], check=True, timeout=5)
            print(f"PASS startup: protection={protect} regulation={regulation} "
                  f"regulators={real} shutdown_none={none}", flush=True)


if __name__ == "__main__":
    main()
