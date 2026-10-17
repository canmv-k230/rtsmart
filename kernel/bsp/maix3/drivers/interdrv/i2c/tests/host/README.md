# I2C1 boot reset host checks

Run from the RT-Smart repository root with Python 3 and Clang (or `cc`):

```sh
python3 kernel/bsp/maix3/drivers/interdrv/i2c/tests/host/run_host_checks.py
```

The runner compiles the actual `drv_i2c.c` and the public I2C/sysctl headers with
minimal RTOS mocks, warnings as errors, and UndefinedBehaviorSanitizer. Each
scenario starts a fresh process to isolate boot-lifetime state. Binaries are
created in a temporary directory and removed. Use `--output <path>` for an
optional JSON report.

The four compile variants cover V3P0 master, another board, V3P0 SMP, and V3P0
slave. Eight scenarios cover normal initialization, missing RMU mapping, either
clock gate disabled, failed I2C1 mapping, a false reset API return, failed
hardware initialization followed by another attempt, and repeated
initialization. Assertions verify the reset target, reset ordering before I2C1
hardware initialization/registration, and at most one reset per native boot.
I2C0 remains part of every initialization sequence and must not be reset.

These are software integration checks with mocks, not target compilation,
register behavior, electrical timing, concurrent SMP execution, or hardware
acceptance. The reset API's existing one-tick implementation is not simulated
or independently validated by these checks. No tests are included by the BSP
`SConscript`.
