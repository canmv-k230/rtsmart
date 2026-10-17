#!/usr/bin/env python3
"""Local driver integration checks; these are not a hardware acceptance test."""
import hashlib
import json
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DRIVER = HERE.parents[1] / "drv_i2c.c"
SYSCTL = DRIVER.parent.parent / "sysctl"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, help="Optional JSON report path")
options = parser.parse_args()
compiler = shutil.which("clang") or shutil.which("cc")
if compiler is None:
    raise SystemExit("No host C compiler")

variants = {
    "v3p0_master": ["-DCONFIG_BOARD_K230_CANMV_V3P0", "-DI2C1_MODE=0"],
    "other_board": ["-DI2C1_MODE=0"],
    "v3p0_smp": ["-DCONFIG_BOARD_K230_CANMV_V3P0", "-DRT_USING_SMP", "-DI2C1_MODE=0"],
    "v3p0_slave": ["-DCONFIG_BOARD_K230_CANMV_V3P0", "-DI2C1_MODE=1"],
}
scenarios = ["normal", "rmu_missing", "pclk_off", "core_off", "mapping_failed",
             "reset_api_false", "init_failed_once", "repeated_init"]
result = {
    "scope": "Host integration of actual drv_i2c.c with actual I2C/sysctl headers and RTOS mocks",
    "driver_sha256": hashlib.sha256(DRIVER.read_bytes()).hexdigest(),
    "compiler": compiler,
    "variants": {},
    "hardware_tested": False,
}
with tempfile.TemporaryDirectory(prefix="k230-i2c-reset-host-") as directory:
    for name, defines in variants.items():
        executable = Path(directory) / name
        args = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-but-set-variable", "-fsanitize=undefined",
                "-fno-sanitize-recover=all", "-DRT_USING_I2C0", "-DRT_USING_I2C1",
                "-DI2C0_MODE=0", f'-DCANDIDATE_SOURCE="{DRIVER}"',
                "-I", str(HERE),
                "-I", str(SYSCTL / "sysctl_clock"),
                "-I", str(SYSCTL / "sysctl_reset"), *defines,
                str(HERE / "boot_reset_test.c"), "-o", str(executable)]
        subprocess.run(args, check=True, capture_output=True, text=True)
        for scenario in scenarios:
            completed = subprocess.run([str(executable), scenario], check=True,
                                       capture_output=True, text=True)
            if completed.stdout.strip() != "PASS":
                raise RuntimeError((name, scenario, completed.stdout))
        result["variants"][name] = {"scenarios": scenarios, "passed": len(scenarios)}
result["passed"] = 32
if options.output:
    options.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
