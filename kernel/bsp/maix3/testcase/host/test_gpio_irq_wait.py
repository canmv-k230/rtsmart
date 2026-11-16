#!/usr/bin/env python3
"""Check GPIO teardown with the production RT-Thread sleep and signal checks."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

BSP = Path(__file__).resolve().parents[2]
RTT = BSP.parents[1] / "rt-thread"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=BSP / "drivers/interdrv/gpio/drv_gpio.c")
args = parser.parse_args()


def function(source, name):
    match = re.search(r"^[^\n;{}]*\b" + name + r"\([^\n]*\)\s*\{", source, re.M)
    if not match:
        return ""
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define RT_USING_LWP
#define rt_inline static inline
#define RT_ASSERT assert
#define RT_NULL NULL
#define RT_FALSE 0
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EBUSY 7
#define RT_EINTR 9
#define RT_EINVAL 10
#define RT_ETIMEOUT 2
#define RT_INTERRUPTIBLE 0
#define RT_KILLABLE 1
#define RT_UNINTERRUPTIBLE 2
#define RT_THREAD_READY 1
#define RT_THREAD_RUNNING 3
#define RT_THREAD_STAT_MASK 7
#define RT_Object_Class_Thread 1
#define RT_TIMER_CTRL_SET_TIME 1
#define RT_DEBUG_THREAD 0
#define RT_DEBUG_LOG(a,b)
#define RT_OBJECT_HOOK_CALL(a,b)
#define LOG_E(...)
#define GPIO_IRQ_MAX_NUM 1
#define SIGKILL 9
typedef long rt_base_t;
typedef unsigned long rt_tick_t;
typedef int32_t rt_int32_t;
typedef uint8_t rt_uint8_t;
typedef int rt_err_t;
typedef int rt_bool_t;
typedef void *rt_object_t;
typedef struct { unsigned long sig[1]; } lwp_sigset_t;
struct rt_thread {
    int stat, error, thread_timer;
    lwp_sigset_t signal, signal_mask;
};
typedef struct rt_thread *rt_thread_t;
typedef struct {
    int lock;
    unsigned in_flight, notify_in_flight;
    rt_bool_t detaching;
    rt_uint8_t active_nest[1];
    struct { int pid; } to_user;
} kd_gpio_irq_t;
static struct { kd_gpio_irq_t irq_table[1]; } _gpio_inst;
static struct rt_thread detacher = { .stat = RT_THREAD_RUNNING };
static int sleep_attempts, schedules, suspend_state;
static rt_tick_t now;
static rt_thread_t rt_thread_self(void) {
    /* Model strict RT priority: the lower priority signal worker can run
     * only after this detacher actually blocks. Bound an old-driver spin. */
    assert(++sleep_attempts < 1000 && "pending signal prevents teardown from blocking");
    return &detacher;
}
static int rt_object_get_type(rt_object_t p) { (void)p; return RT_Object_Class_Thread; }
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t l) { (void)l; }
static rt_base_t rt_spin_lock_irqsave(int *p) { (void)p; return 0; }
static void rt_spin_unlock_irqrestore(int *p, rt_base_t l) { (void)p; (void)l; }
static rt_uint8_t rt_interrupt_get_nest(void) { return 0; }
static int lwp_sigismember(lwp_sigset_t *s, int sig) { return !!(s->sig[0] & (1UL << (sig-1))); }
static void rt_schedule_remove_thread(rt_thread_t t) { (void)t; }
static void rt_thread_set_suspend_state(rt_thread_t t, int flag) { suspend_state = flag; t->stat = 4; }
static void rt_timer_stop(int *t) { (void)t; }
static void rt_timer_control(int *t, int cmd, rt_tick_t *tick) { (void)t; (void)cmd; (void)tick; }
static void rt_timer_start(int *t) { (void)t; }
static rt_tick_t rt_tick_from_millisecond(int ms) { return ms; }
static rt_tick_t rt_tick_get(void) { return now; }
static void rt_schedule(void) {
    assert(detacher.stat == 4);
    assert(suspend_state == RT_UNINTERRUPTIBLE || !detacher.signal.sig[0]);
    schedules++;
    /* Finish the worker/peer detacher, then wake this caller at its timeout. */
    _gpio_inst.irq_table[0].notify_in_flight = 0;
    _gpio_inst.irq_table[0].detaching = 0;
    detacher.stat = RT_THREAD_RUNNING;
    now++;
}
static void kd_pin_detach_irq_locked(rt_base_t pin, kd_gpio_irq_t *irq) { (void)pin; (void)irq; }
'''

MAIN = r'''
int main(int argc, char **argv) {
    assert(argc == 3);
    int pending = atoi(argv[1]), peer = atoi(argv[2]);
    _gpio_inst.irq_table[0].detaching = peer;
    _gpio_inst.irq_table[0].notify_in_flight = 1;
    detacher.signal.sig[0] = pending ? 1UL << 9 : 0;
    assert(kd_pin_detach_irq(0) == RT_EOK);
    assert(schedules == 1);
    assert(!_gpio_inst.irq_table[0].notify_in_flight);
    assert(!_gpio_inst.irq_table[0].detaching);
    /* Kernel teardown must leave the signal pending for return to userspace. */
    assert(detacher.signal.sig[0] == (pending ? 1UL << 9 : 0));
    return 0;
}
'''

signal_source = (RTT / "components/lwp/lwp_signal.c").read_text()
thread_source = (RTT / "src/thread.c").read_text()
driver = args.source.read_text()
functions = "\n".join(
    [function(signal_source, name) for name in ("lwp_thread_signal_check", "lwp_suspend_sigcheck")]
    + [function(thread_source, name) for name in (
        "rt_thread_suspend_with_flag", "rt_thread_sleep", "rt_thread_mdelay", "rt_thread_delay_until")]
    + [function(driver, name) for name in (
        "kd_pin_irq_wait_tick", "kd_pin_irq_wait_idle", "kd_pin_detach_irq_for_owner", "kd_pin_detach_irq")]
)
failed = False
with tempfile.TemporaryDirectory(prefix="gpio-irq-wait-") as tmp:
    source, binary = Path(tmp) / "wait.c", Path(tmp) / "wait"
    source.write_text(PRELUDE + functions + MAIN)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                    "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter",
                    "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
    for pending in (0, 1):
        for peer in (0, 1):
            name = f"{'concurrent-detach' if peer else 'active-notification'}-{'pending-signal' if pending else 'no-signal'}"
            run = subprocess.run([str(binary), str(pending), str(peer)], capture_output=True, text=True, timeout=5)
            print(("PASS: " if run.returncode == 0 else "FAIL: ") + name, flush=True)
            if run.returncode:
                failed = True
                print(run.stdout + run.stderr, end="", flush=True)
raise SystemExit(failed)
