#!/usr/bin/env python3
"""Run host regression tests without modifying the BSP configuration."""
import os
from pathlib import Path
import subprocess
import tempfile

TEST_DIR = Path(__file__).resolve().parent
MODULE_DIR = TEST_DIR.parent


def main():
    with tempfile.TemporaryDirectory(prefix="vo-debug-tests-") as temporary:
        build = Path(temporary)
        # The harness supplies RT-Thread/MMIO mocks before including the module.
        for header in ("rtthread.h", "rthw.h", "ioremap.h", "riscv_io.h", "riscv_mmu.h"):
            (build / header).write_text("")
        cc = os.environ.get("HOST_CC", "cc")
        common = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(build)]
        subprocess.run(common + ["-DRT_USING_VO_DEBUG", "-fsanitize=address,undefined", "-g",
                                str(TEST_DIR / "test_vo_debug.c"), "-o", str(build / "test")], check=True)
        subprocess.run([str(build / "test")], check=True)
        subprocess.run([str(build / "test"), "--interactive"], input=(
            "x" * 79 + "set osd0 1\nqjunk\nset osd0 1 extra\nexit extra\n"),
            text=True, check=True, timeout=10)
        subprocess.run([str(build / "test"), "--interactive"], input="\t \nexit\n",
                       text=True, check=True, timeout=10)
        # An unset guard must compile to an empty object, including shell aliases.
        subprocess.run(common + ["-c", str(MODULE_DIR / "vo_debug.c"),
                                 "-o", str(build / "disabled.o")], check=True)
        symbols = subprocess.check_output(["nm", str(build / "disabled.o")], text=True)
        assert "vo_debug" not in symbols, symbols
        print("Disabled source guard passed")


if __name__ == "__main__":
    main()
