#!/usr/bin/env python3
"""Check derived thermal stages and policy behavior using production sources.

Run with python3 -B kernel/bsp/maix3/testcase/host/test_thermal_config.py.
Includes the 55/45 C configuration, fractional steps, minimum spacing and
regulation disabled. No board or cross compiler is required.
"""

from pathlib import Path
import os
import subprocess
import sys
import tempfile


BSP = Path(__file__).resolve().parents[2]
PM = BSP / "drivers/interdrv/k230_pm"

CASES = r"""
#include <assert.h>
#include "k230_pm_thermal.h"

static struct k230_thermal_action observe(struct k230_thermal_policy *p,
    const struct k230_thermal_config *c, struct k230_thermal_sample *s,
    int32_t temperature, uint32_t now)
{
    s->valid = true;
    s->temperature_mc = temperature;
    s->sequence++;
    s->tick = now;
    return k230_thermal_policy_step(p, c, s, now);
}

int main(void)
{
    struct k230_thermal_config c;
    struct k230_thermal_policy p;
    struct k230_thermal_sample s = {0};
    k230_pm_thermal_config(&c);
    assert(k230_thermal_config_valid(&c));
    assert(c.target_mc == EXPECT_TARGET);
    assert(c.critical_mc == EXPECT_CRITICAL);
    assert(c.warm_mc == EXPECT_WARM);
    assert(c.hot_mc == EXPECT_HOT);
    assert(c.min_mc == EXPECT_MIN && c.sustained_mc == EXPECT_MIN);
    assert(c.hysteresis_mc == EXPECT_HYSTERESIS);
    assert(c.noise_mc == EXPECT_NOISE);
    assert(c.restart_mc == EXPECT_RESTART);
    assert(c.regulation == EXPECT_REGULATION);

    /* Entry thresholds work with fractional degrees and with regulation off. */
    k230_thermal_policy_init(&p, 0);
    assert(observe(&p, &c, &s, c.warm_mc - 1, 0).ceiling == K230_THERMAL_NORMAL);
    assert(observe(&p, &c, &s, c.warm_mc, 1).ceiling == K230_THERMAL_WARM);
    assert(observe(&p, &c, &s, c.hot_mc, 2).ceiling == K230_THERMAL_HOT);
    assert(observe(&p, &c, &s, c.min_mc, 3).ceiling == K230_THERMAL_MIN);
    k230_thermal_policy_applied(&p, K230_THERMAL_MIN, true, 0);

    /* Crossing the recovery band still requires the complete cooling hold. */
    assert(observe(&p, &c, &s, c.min_mc - c.hysteresis_mc, 4).ceiling == K230_THERMAL_MIN);
    assert(observe(&p, &c, &s, c.min_mc - c.hysteresis_mc - 1, 5).ceiling == K230_THERMAL_MIN);
    assert(observe(&p, &c, &s, c.min_mc - c.hysteresis_mc - 1,
        5 + c.recovery_ticks - 1).ceiling == K230_THERMAL_MIN);
    assert(observe(&p, &c, &s, c.min_mc - c.hysteresis_mc - 1,
        5 + c.recovery_ticks).ceiling == K230_THERMAL_HOT);

    /* Critical still needs a second fresh observation inside the noise band. */
    k230_thermal_policy_init(&p, 0);
    assert(!observe(&p, &c, &s, c.critical_mc, 0).shutdown);
    s.error = -1;
    s.sequence++;
    s.tick = 1;
    assert(!k230_thermal_policy_step(&p, &c, &s, 1).shutdown);
    s.error = 0;
    assert(observe(&p, &c, &s, c.critical_mc - c.noise_mc, 2).shutdown);
    assert(p.reason == K230_THERMAL_CRITICAL);
    k230_thermal_policy_init(&p, 0);
    assert(!observe(&p, &c, &s, c.critical_mc, 0).shutdown);
    assert(!observe(&p, &c, &s, c.critical_mc - c.noise_mc - 1, 1).shutdown);
    return 0;
}
"""


def check_kconfig():
    sys.path.insert(0, str(BSP.parent.parent / "rt-thread/tools"))
    import kconfiglib
    from defconfig import set_kconfig_env_defaults, set_user_app_dir_env

    os.chdir(BSP)
    set_kconfig_env_defaults()
    set_user_app_dir_env(".config", "Kconfig")
    config = kconfiglib.Kconfig("Kconfig", warn=False)
    boards = sorted((BSP / "configs").glob("*defconfig"))
    backend_names = (
        "K230_PM_THERMAL_SHUTDOWN_NONE",
        "K230_PM_THERMAL_SHUTDOWN_HALT",
        "K230_PM_THERMAL_SHUTDOWN_PMU",
    )

    def check_backend(label):
        selected = [name for name in backend_names if config.syms[name].str_value == "y"]
        explicit = [name for name in backend_names if config.syms[name].user_value == 2]
        assert len(selected) == 1, (label, selected)
        if explicit:
            assert selected == explicit, (label, selected, explicit)
        else:
            assert selected == ["K230_PM_THERMAL_SHUTDOWN_NONE"], label
        return selected[0]

    for board in boards:
        config.load_config(str(board))
        if config.syms["K230_PM_THERMAL_PROTECT"].str_value != "y":
            continue
        if check_backend(board) == "K230_PM_THERMAL_SHUTDOWN_NONE":
            assert config.syms["K230_PM_THERMAL_COOLDOWN_RESTART"].str_value == "n", board
        critical = int(config.syms["K230_PM_THERMAL_CRITICAL_C"].str_value)
        target = int(config.syms["K230_PM_THERMAL_TARGET_C"].str_value)
        assert 50 <= critical <= 115 and 30 <= target <= min(95, critical - 8), board
        config.syms["K230_PM_THERMAL_REGULATION"].set_value("n")
        assert int(config.syms["K230_PM_THERMAL_TARGET_C"].str_value) == target
    config.load_config(str(BSP / "configs/k230_evb_defconfig"))
    assert config.syms["K230_PM_THERMAL_SHUTDOWN_NONE"].str_value == "y"
    config.syms["K230_PM_THERMAL_SHUTDOWN_HALT"].set_value("y")
    assert check_backend("explicit HALT override") == "K230_PM_THERMAL_SHUTDOWN_HALT"
    for critical in range(50, 116):
        config.syms["K230_PM_THERMAL_CRITICAL_C"].set_value(str(critical))
        config.syms["K230_PM_THERMAL_TARGET_C"].unset_value()
        assert int(config.syms["K230_PM_THERMAL_TARGET_C"].str_value) == (critical - 20) // 5 * 5
    print(f"PASS {len(boards)} board configurations, target defaults and regulation-off retention", flush=True)


def main():
    check_kconfig()
    # Critical/target C, then expected WARM/HOT/MIN/hysteresis/noise/restart mC.
    configurations = (
        (55, 45, 47500, 50000, 52500, 2500, 1000, 45000),
        (55, 47, 49000, 51000, 53000, 2000, 900, 47000),
        (56, 45, 47750, 50500, 53250, 2750, 1000, 45000),
        (54, 45, 47250, 49500, 51750, 2250, 1000, 45000),
        (50, 30, 35000, 40000, 45000, 5000, 1000, 30000),
        (75, 55, 60000, 65000, 70000, 5000, 1000, 55000),
        (105, 85, 90000, 95000, 100000, 5000, 1000, 70000),
        (105, 60, 90000, 95000, 100000, 5000, 1000, 60000),
        (115, 95, 100000, 105000, 110000, 5000, 1000, 70000),
    )
    with tempfile.TemporaryDirectory(prefix="k230-thermal-config-") as directory:
        build = Path(directory)
        (build / "rtthread.h").write_text("#include <stdint.h>\ntypedef uint32_t rt_tick_t;\n"
                                          "#define RT_TICK_PER_SECOND 1000\n")
        source, binary = build / "test.c", build / "test"
        source.write_text(CASES)
        base = [os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-fno-sanitize-recover=all", "-DK230_PM_THERMAL_PROTECT",
                "-I", str(build), "-I", str(PM), str(source),
                str(PM / "k230_pm_thermal_policy.c"), "-o", str(binary)]
        for critical, target, *expected in configurations:
            flags = [f"-DK230_PM_THERMAL_CRITICAL_C={critical}", f"-DK230_PM_THERMAL_TARGET_C={target}",
                     f"-DEXPECT_CRITICAL={critical * 1000}", f"-DEXPECT_TARGET={target * 1000}"]
            names = ("WARM", "HOT", "MIN", "HYSTERESIS", "NOISE", "RESTART")
            flags += [f"-DEXPECT_{name}={value}" for name, value in zip(names, expected)]
            for regulation in (False, True):
                mode = [f"-DEXPECT_REGULATION={int(regulation)}"]
                if regulation:
                    mode.append("-DK230_PM_THERMAL_REGULATION")
                subprocess.run(base + flags + mode, check=True)
                subprocess.run([str(binary)], check=True)
        # Compile only the header for rejected settings; don't let test fixture
        # definitions be the reason compilation fails.
        source.write_text('#include "k230_pm_thermal.h"\nint main(void) { return 0; }\n')
        for critical, target in ((55, 48), (55, 55), (55, 85), (49, 30), (116, 95), (105, 29), (105, 96)):
            result = subprocess.run(base + [f"-DK230_PM_THERMAL_CRITICAL_C={critical}",
                                           f"-DK230_PM_THERMAL_TARGET_C={target}"], capture_output=True)
            assert result.returncode and b"#error" in result.stderr, result.stderr
    print("PASS derived stages, recovery holds and critical confirmation with regulation on/off")
    print("PASS rejected gaps below 8 C and out-of-range settings")


if __name__ == "__main__":
    main()
