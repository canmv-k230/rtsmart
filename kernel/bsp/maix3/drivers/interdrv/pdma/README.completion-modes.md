# Explicit PDMA completion ownership

This change is based on `canmv-k230/rtsmart` main at
`6d7e08e6710591cb428a8c20e816c0e57bb041fe`.

## Problem

The driver queues every terminal interrupt in an eight-entry completion FIFO.
Only `rt_dma_chan_done()` consumes that FIFO. A callback-only client that starts
its next transfer in the IRQ callback never consumes it: after eight terminal
interrupts, the next `rt_dma_chan_start()` returns `-RT_EBUSY`. In the matching
MPP audio library, callbacks rearm DMA and do not call `done()`; the error is
not propagated from the rearm path, and audio eventually stalls.

Do not infer completion ownership from whether a waiter is currently active.
A client may consume results after its callbacks have already returned. The
existing built-in callback-and-late-done test intentionally uses that pattern.

## API and migration

| Registration mode | Callback | Completion FIFO | `done()` |
| --- | --- | --- | --- |
| `PDMA_COMPLETION_WAIT` | Must be null | Enabled | Consumes oldest result |
| `PDMA_COMPLETION_CALLBACK_ONLY` | Must be non-null | Disabled | `-RT_EINVAL` |
| `PDMA_COMPLETION_HYBRID` | Must be non-null | Enabled | Consumes oldest result |

Newly allocated channels default to WAIT. Use
`rt_dma_chan_callback_ex(chan, callback, param, mode)` to choose explicitly.
Registration refuses running transfers, active waiters/callbacks, and queued
unconsumed completions with `-RT_EBUSY`; it never discards queued results.

The old `rt_dma_chan_callback()` symbol and signature remain, but a non-null
callback now selects CALLBACK_ONLY. A null callback selects WAIT. **This is a
semantic change for callback-plus-`done()` callers**, including delayed consumers.
They must migrate to HYBRID:

```c
/* Callback-only client: no done() calls. */
rt_dma_chan_callback(chan, callback, param);

/* Callback plus synchronous or later completion consumption. */
rt_dma_chan_callback_ex(chan, callback, param, PDMA_COMPLETION_HYBRID);
/* ... start transfers, receive callbacks ... */
ret = rt_dma_chan_done(chan, timeout);
```

Keeping existing function signatures and `pdma_transfer_cfg_t` layout does not
guarantee compatibility with every previously linked binary. The audited MPP
library (`9f5301afe0bf3f4056827c8335f9e4478fa95043`) contains three PDMA consumers
(AI PDM, AI I2S, AO I2S), all callback-only. The matching linked runtime has no
external `done()` callers. Unknown libraries need their own audit. Maintainers
may instead preserve HYBRID as the legacy default and migrate/rebuild MPP to use
the new CALLBACK_ONLY API; that requires a coordinated audio-library update
and is not the policy implemented by this proposal.

Callbacks report a terminal IRQ, which can be a hardware timeout, rather than
unconditionally reporting success. `rt_dma_chan_get_completion_snapshot()` is a
non-consuming, IRQ-safe view of the latest terminal generation/raw IRQ bits and
a saturating lifetime hardware-timeout counter. A new completion can replace
the last result; this snapshot is not a FIFO or full event history. It survives
stop/mode changes and resets on a new channel allocation. A software `done()`
timeout is still reported by its return value, not this counter.

## Implementation boundaries

The existing lifecycle mutex, state spinlock, in-flight callback release waits,
single waiter, generation tracking, and descriptor construction are retained.
Only WAIT/HYBRID enqueue completions and publish waiter events. The IRQ records
whether to notify waiters before calling user code, so CALLBACK_ONLY events
cannot leak into a later mode after IRQ/callback dispatch has completed
and the channel is subsequently re-registered.

IRQ masks use unsigned constants: channel 7 timeout/PALL shifts otherwise touch
bit 31 and invoke signed-shift undefined behavior. The built-in callback test
now performs 64 transfers instead of two, crossing the old queue limit.
The built-in delayed-done test is explicitly registered as HYBRID.

## Host regression tests

Requirements: Python 3 and Clang on a Unix host. Run from the repository root:

```sh
python3 kernel/bsp/maix3/drivers/interdrv/pdma/tests/host/run_host_tests.py --mutations
```

Add `--report <new-path.json>` for a JSON report. An existing report is never
overwritten. `--asan` is optional and its results are distinct from UBSan.
The tests compile the actual driver with deterministic RTOS/MMIO shims; there
is no second implementation of PDMA. They never access a board or serial port.

18 cases cover 1,000 callback-only transfers, late HYBRID consumption, full FIFO
rejection/recovery, generation/timeout interactions, channel reuse, registration
busy states, terminal snapshots, timeout saturation, all eight IRQ bit layouts,
non-terminal interrupts, descriptor chains and in-flight callback release.
Four variants run: plain, UBSan, SMP-conditional compilation, and built-in-test
conditional compilation. Seven injected defects must compile successfully and
then be rejected by behavior checks. Compilation errors do not count as a
mutation detected.

The SMP variant uses sequential shims; it is not SMP concurrency validation.
The built-in-test variant compiles its UART path but never invokes UART I/O.
Large-length descriptor tests do not transfer large buffers. Host shims cannot
validate target MMIO ordering, W1C behavior, cache coherence or DMA bus traffic.

## Bounded target evidence and remaining review

The same two final C/H files were linked into a K230 CanMV V3P0 non-SMP runtime.
The audited audio library completed three AO cycles (800, 125, 125 frames),
three AI cycles (120 frames each), and three mixed camera/display+AI/AO cycles
(total 60,084 ms, 1,509 AI and AO frames each). Native cleanup was checked.
No claim is made for long-term soak, all board profiles, target SMP concurrency,
all unknown linked consumers, or runtime stack high-water measurements.
That integration also contained independent camera and AO-exit fixes; it does
not establish that this PDMA change fixes every audio or camera symptom alone.

This proposal is suitable for upstream design review, not unconditional merge
approval. Review the legacy API migration policy and test real target boundary
conditions (IRQ versus timeout, callback versus release, stop/restart, concurrent
channels) before broad deployment.
