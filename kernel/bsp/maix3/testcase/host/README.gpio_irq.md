# GPIO IRQ teardown regressions

From the RT-Smart repository, run:

```sh
python3 -B kernel/bsp/maix3/testcase/host/test_gpio_irq.py
```

The host test compiles the production IRQ functions and data structures with
pthread locks and ASan/UBSan. It checks pending notification cancellation on
detach and close, cancellation of debounce before immediate reuse, waiting for
an active callback or signal delivery, reattachment racing with an orphan EOI,
reuse after ISR self-detach, an edge raised during the callback, and debounce
expiry for a pin queued behind a pin with a longer debounce. The production ioctl
path is also exercised: a userspace detach must preserve another process's or
the kernel's registration, including queued work, while the owner and kernel
callers can still detach and cancel pending work. `--source /path/to/drv_gpio.c` accepts a saved
driver for checking failures before a fix. The model accepts PLIC completion only
while its source is enabled, following the [PLIC completion rule](https://docs.riscv.org/reference/plic/plic-completion.html).

`gpio_irq_board_probe.c` and `gpio_irq_user_probe.c` exercise the real kernel and
userspace paths on the 01Studio K230. They use GPIO26, GPIO27 and GPIO28 with
internal pulls, without jumper wires. Use these pins only when the board has no
peripheral connected to them. The kernel probe restores their mode, pinmux and
direction. It includes 1,000 ordinary attach/interrupt/detach cycles, high/low
level disable and re-enable, duplicate attach, 100 ISR self-detach/reuse
cycles, and a second edge raised from inside the callback. A final 3 s case
sets GPIO27 to open-drain output and calls `kd_pin_write`/`kd_pin_read` on it
from a thread, while a 1 kHz hard timer enables a GPIO26 level interrupt whose
callback uses the same pin. In open-drain mode each call also updates the
pinmux, so this covers both the GPIO pin-state lock and the FPIOA lock. If either
lock leaves interrupts enabled, the board hangs in this case instead of printing
a failure, and needs a reset. The userspace probe includes another 1,000 cycles and raises its priority
above the signal worker to queue work immediately before detach or close.

To include the kernel test command temporarily, run from the SDK root with the
01Studio output already configured:

```sh
ln -s "$PWD/src/rtsmart/rtsmart/kernel/bsp/maix3/testcase/host/gpio_irq_board_probe.c" \
    src/rtsmart/rtsmart/kernel/bsp/maix3/drivers/interdrv/gpio/gpio_irq_board_probe.c
make -C output/k230_canmv_01studio_defconfig -j8
rm src/rtsmart/rtsmart/kernel/bsp/maix3/drivers/interdrv/gpio/gpio_irq_board_probe.c
```

Build the userspace probe from the SDK root, using its installed musl toolchain:

```sh
~/.kendryte/k230_toolchains/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-gcc \
    -std=gnu99 -Os -g -march=rv64imafdcv -mabi=lp64d -mcmodel=medany \
    src/rtsmart/rtsmart/kernel/bsp/maix3/testcase/host/gpio_irq_user_probe.c \
    -Tsrc/rtsmart/libs/mk/link.lds --static -Wl,--gc-sections \
    -o /tmp/gpio_irq_user_probe.elf
```

Flash the rebuilt image, upload the userspace ELF as
`/sdcard/gpio_irq_user_probe.elf`, and run these commands on `msh`:

```text
/sdcard/gpio_irq_user_probe.elf
gpio_irq_review
```

Both summaries must report `failures=0`. A notification already submitted to the
process before teardown cannot be retracted; these tests check that canceled
work does not submit a signal after detach or close returns. Detach cancels
pending debounce immediately, so a replacement can attach without waiting for
the old timeout. Thread callers may sleep while an active callback or signal
submission finishes. An ISR cannot wait for another invocation; it receives
`-RT_EBUSY` in that case, while its own callback can detach itself.

For IRQ ownership, build `gpio_irq_owner_probe.c` with the same userspace command
above, adding `-Isrc/rtsmart/libs/rtsmart_hal/components/k230_syscalls`, and upload
it as `/sdcard/gpio_irq_owner_probe.elf`. Running it without arguments performs
20 iterations with two real processes: the owner registers GPIO26 and receives
an interrupt, a child with a different PID opens `/dev/gpio`, requests detach
(`value=128`), verifies that replacement attach is still rejected, and exits.
The owner must receive another interrupt, then detach successfully with no
further signal. The summary must report `iterations=20` and `failures=0`.

To check kernel registrations, also link `gpio_irq_owner_board_probe.c` into
the GPIO driver group temporarily, using a symlink as for the kernel probe
above. Run:

```text
gpio_irq_owner_kernel start
/sdcard/gpio_irq_owner_probe.elf --kernel
gpio_irq_owner_kernel check
gpio_irq_owner_kernel stop
```

The user probe must report `failures=0`; the kernel check must print
`GPIO_OWNER_KERNEL PASS hits=1`, proving its callback still runs after userspace
detach and close. The stop command detaches from kernel context and restores
the saved GPIO26 configuration. Remove the probe symlinks, rebuild and restore
normal firmware after testing. These ownership probes also require GPIO26 to
have no attached peripheral.

Hardware tests use the normal single-core RT-Smart configuration. The controlled
SMP interleavings are covered by the host test.

The host command also runs `test_gpio_irq_wait.py`, which compiles the production
RT-Thread sleep, suspend and signal-check functions with both GPIO teardown wait
paths. It models a higher-priority detacher waiting for a signal worker or a
peer detacher. Both paths must block even with an unmasked signal pending, and
must preserve that signal for return to userspace. The old interruptible sleeps
fail both pending-signal cases.

For deterministic signal interruption tests on the board, build a temporary
firmware with `gpio_irq_signal_board_probe.c` linked into the GPIO driver group.
Immediately before the driver's `lwp_kill_ext(arg.pid, arg.cfg.signo, &info);`,
temporarily insert:

```c
extern void gpio_irq_signal_probe_before_notify(int pid, void *sigval);
gpio_irq_signal_probe_before_notify(arg.pid, arg.cfg.sigval);
```

After building the image, remove both the inserted gate and the probe symlink
from the driver directory. The gate is for test firmware only. Upload the
userspace probe built above, then run:

```text
/sdcard/gpio_irq_user_probe.elf --signal-wait
```

The three cases cover detach, close, and a second detacher waiting for an
existing teardown. The gate pauses an already claimed notification, wakes the
priority-10 userspace thread to start teardown, and then allows the real GPIO
signal to be posted while teardown is waiting. Each case must print
`GPIO_SIGNAL teardown-wait ... state=7`, deliver one signal, and finish. The final
`GPIO_USER SIGNAL_END` summary must report `failures=0`. Run this option only
with the gated firmware; its assertions check that each intended interleaving
actually occurred. Restore normal firmware after testing.
