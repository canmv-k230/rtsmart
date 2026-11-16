#!/usr/bin/env python3
"""Exercise production GPIO IRQ functions with controlled SMP interleavings.

Run with python3 -B kernel/bsp/maix3/testcase/host/test_gpio_irq.py.
--source can select a saved driver to check that a regression fails before a fix.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

BSP = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=BSP / "drivers/interdrv/gpio/drv_gpio.c")
args = parser.parse_args()
driver = args.source.read_text()


def function(name):
    match = re.search(r"^[^\n;{}]*\b" + name + r"\([^\n]*\)\s*\{", driver, re.M)
    if not match:
        return ""
    brace = driver.index("{", match.start())
    depth = 1
    end = brace + 1
    while depth:
        depth += (driver[end] == "{") - (driver[end] == "}")
        end += 1
    return driver[match.start():end]


PRELUDE = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#define RT_USING_SMP
#define RT_CPUS_NR 2
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EBUSY 7
#define RT_EINVAL 10
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_NULL NULL
#define RT_NAME_MAX 16
#define GPIO_MAX_NUM 72
#define GPIO_IRQ_MAX_NUM 64
#define IRQN_GPIO0_INTERRUPT 32
#define GPIO_PE_RISING 0
#define GPIO_PE_FALLING 1
#define GPIO_PE_BOTH 2
#define GPIO_PE_HIGH 3
#define GPIO_PE_LOW 4
#define BIT(n) (1U << (n))
#define RT_ASSERT assert
#define rt_memcpy memcpy
#define rt_memset memset
#define rt_snprintf snprintf
#define LOG_E(...) ((void)0)
#define LOG_I(...) ((void)0)
#define SI_SIGIO 1
typedef long rt_base_t;
typedef unsigned long rt_size_t;
typedef int rt_err_t;
typedef int rt_bool_t;
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef uint32_t rt_uint32_t;
typedef uint64_t rt_uint64_t;
typedef unsigned long rt_tick_t;
typedef void *rt_device_t;
struct rt_spinlock { pthread_mutex_t mutex; };
struct rt_list_node { struct rt_list_node *next, *prev; };
struct rt_work { int unused; };
struct rt_workqueue { int unused; };
typedef int gpio_drive_mode_t;
typedef struct { rt_uint16_t pin, value; } gpio_cfg_t;
typedef struct { rt_uint16_t pin, mode, debounce_ms, signo; void *sigval; } gpio_irqcfg_t;
typedef struct { int si_code; void *si_ptr; } gpio_siginfo_t;
#define siginfo_t gpio_siginfo_t
static _Thread_local int cpu, nest;
static _Thread_local int current_pid = 123;
static atomic_int signals, callbacks, detached, waits, replacement_done;
static int block_callback, block_signal, block_eoi;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int entered, finish;
static void pause_at_gate(void) {
    pthread_mutex_lock(&gate_lock);
    entered = 1; pthread_cond_broadcast(&gate_cond);
    while (!finish) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
}
static void wait_for_gate(void) {
    pthread_mutex_lock(&gate_lock);
    while (!entered) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
}
static void release_gate(void) {
    pthread_mutex_lock(&gate_lock); finish = 1;
    pthread_cond_broadcast(&gate_cond); pthread_mutex_unlock(&gate_lock);
}
static rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock) {
    assert(pthread_mutex_lock(&lock->mutex) == 0); return 0;
}
static void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level) {
    (void)level; assert(pthread_mutex_unlock(&lock->mutex) == 0);
}
static int rt_hw_cpu_id(void) { return cpu; }
static rt_uint8_t rt_interrupt_get_nest(void) { return nest; }
static int lwp_getpid(void) { return current_pid; }
static rt_size_t lwp_get_from_user(void *dst, const void *src, rt_size_t size) {
    memcpy(dst, src, size); return size;
}
static rt_size_t lwp_put_to_user(void *dst, const void *src, rt_size_t size) {
    memcpy(dst, src, size); return size;
}
static rt_err_t kd_pin_mode(rt_base_t pin, rt_base_t mode) { abort(); }
static rt_err_t kd_pin_mode_get(rt_base_t pin, rt_base_t *mode) { abort(); }
static _Atomic uint64_t fake_now = 1000;
static void rt_thread_mdelay(int ms) {
    atomic_fetch_add(&waits, 1); atomic_fetch_add(&fake_now, ms); usleep(ms * 100);
}
static rt_tick_t rt_tick_get(void) { return atomic_load(&fake_now); }
static void rt_thread_delay_until(rt_tick_t *tick, rt_tick_t inc) {
    rt_thread_mdelay(inc); *tick = rt_tick_get();
}
static void rt_thread_yield(void) { atomic_fetch_add(&waits, 1); usleep(1000); }
static uint64_t cpu_ticks_ms(void) { return atomic_load(&fake_now); }
static void rt_list_init(struct rt_list_node *n) { n->next = n->prev = n; }
static int rt_list_isempty(struct rt_list_node *n) { return n->next == n; }
static void rt_list_insert_after(struct rt_list_node *a, struct rt_list_node *n) {
    n->next = a->next; n->prev = a; a->next->prev = n; a->next = n;
}
static void rt_list_remove(struct rt_list_node *n) {
    n->next->prev = n->prev; n->prev->next = n->next; rt_list_init(n);
}
#define rt_list_entry(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
#define rt_container_of(ptr,type,member) rt_list_entry(ptr,type,member)
static int rt_workqueue_submit_work(struct rt_workqueue *q, struct rt_work *w, int t) {
    (void)q; (void)w; (void)t; return 0;
}
static void lwp_kill_ext(int pid, int signo, siginfo_t *info) {
    assert(pid == 123 && signo == 10); (void)info;
    if (block_signal) pause_at_gate();
    atomic_fetch_add(&signals, 1);
}
void vicap_gpio_irq_process(void *arg) { (void)arg; }
'''

STUBS = r'''
typedef struct {
    struct rt_work work;
    struct rt_workqueue *queue;
    struct rt_spinlock lock;
    struct rt_list_node list;
} kd_gpio_work_t;
static struct {
    volatile kd_gpio_t *reg[2];
    kd_gpio_work_t debounce_work, irq_to_user_work;
    kd_gpio_irq_t irq_table[GPIO_IRQ_MAX_NUM];
} _gpio_inst;
static kd_gpio_t regs[2];
static pthread_mutex_t mmio_lock = PTHREAD_MUTEX_INITIALIZER;
static int plic_enabled, plic_claimed;
static uint64_t unmask_time[32];
rt_err_t kd_pin_attach_irq(rt_base_t, rt_uint32_t, void (*)(void*), void*);
rt_err_t kd_pin_detach_irq(rt_base_t);
rt_err_t kd_pin_irq_enable(rt_base_t, rt_uint32_t);
static void rt_hw_interrupt_mask(int v) { (void)v; plic_enabled = 0; }
static void rt_hw_interrupt_umask(int v) { (void)v; plic_enabled = 1; }
static void rt_hw_interrupt_install(int v, void (*fn)(int,void*), void *p, const char *n) {
    (void)v; (void)fn; (void)p; (void)n;
}
static uint32_t read32(volatile uint32_t *reg) {
    pthread_mutex_lock(&mmio_lock); uint32_t value = *reg;
    pthread_mutex_unlock(&mmio_lock); return value;
}
static void kd_pin_write_reg(rt_base_t pin, volatile uint32_t *reg, int bit, int value) {
    (void)pin;
    if (reg == &regs[0].porta_eoi && block_eoi) pause_at_gate();
    pthread_mutex_lock(&mmio_lock);
    if (reg == &regs[0].porta_eoi) {
        *reg = BIT(bit); regs[0].intstatus &= ~BIT(bit);
    } else { *reg = (*reg & ~BIT(bit)) | (value ? BIT(bit) : 0); }
    if (reg == &regs[0].intmask && !value) unmask_time[bit] = atomic_load(&fake_now);
    pthread_mutex_unlock(&mmio_lock);
}
static void raise_edge(void) {
    pthread_mutex_lock(&mmio_lock); regs[0].intstatus = 1; pthread_mutex_unlock(&mmio_lock);
}
static void callback(void *arg) {
    assert(arg == (void *)0x1234);
    assert(pthread_mutex_trylock(&_gpio_inst.irq_table[0].lock.mutex) == 0);
    pthread_mutex_unlock(&_gpio_inst.irq_table[0].lock.mutex);
    if (block_callback) pause_at_gate();
    atomic_fetch_add(&callbacks, 1);
}
static void edge_callback(void *arg) {
    (void)arg; atomic_fetch_add(&callbacks, 1);
    /* Another edge arrives while the callback is still running. */
    if (atomic_load(&callbacks) == 1) raise_edge();
}
static void self_callback(void *arg) {
    (void)arg; atomic_fetch_add(&callbacks, 1); assert(kd_pin_detach_irq(0) == 0);
}
static void fixture(void) {
    _gpio_inst.reg[0] = &regs[0]; _gpio_inst.reg[1] = &regs[1];
    for (int i = 0; i < GPIO_IRQ_MAX_NUM; i++)
        pthread_mutex_init(&_gpio_inst.irq_table[i].lock.mutex, NULL);
    pthread_mutex_init(&_gpio_inst.debounce_work.lock.mutex, NULL);
    pthread_mutex_init(&_gpio_inst.irq_to_user_work.lock.mutex, NULL);
    rt_list_init(&_gpio_inst.debounce_work.list);
    rt_list_init(&_gpio_inst.irq_to_user_work.list);
}
'''

TESTS = r'''
static void *dispatch(void *unused) {
    (void)unused; cpu = 0; nest = 1;
    kd_pin_irq_handler(32, NULL); nest = 0; return NULL;
}
static void *detach_thread(void *unused) {
    (void)unused; cpu = 1;
    assert(kd_pin_detach_irq(0) == 0); atomic_store(&detached, 1); return NULL;
}
static void *signal_thread(void *unused) {
    (void)unused; cpu = 0;
    kd_pin_irq_to_user_work(NULL, &_gpio_inst.irq_to_user_work); return NULL;
}
static void *replace_thread(void *unused) {
    (void)unused; cpu = 1;
    assert(kd_pin_attach_irq(0, GPIO_PE_RISING, callback, (void *)0x1234) == 0);
    assert(kd_pin_irq_enable(0, 1) == 0);
    raise_edge(); atomic_store(&replacement_done, 1); return NULL;
}
static void wait_until_waiting(void) {
    for (int i = 0; i < 1000 && !atomic_load(&waits); i++) usleep(1000);
    assert(atomic_load(&waits));
}
static void queue_signal(int mode) {
    assert(kd_pin_attach_irq(0, mode, kd_pin_irq_to_user_handler, NULL) == 0);
    _gpio_inst.irq_table[0].to_user.pid = 123;
    _gpio_inst.irq_table[0].to_user.cfg.mode = mode;
    _gpio_inst.irq_table[0].to_user.cfg.signo = 10;
    _gpio_inst.irq_table[0].to_user.cfg.debounce_ms = 100;
    raise_edge(); dispatch(NULL);
}
static void queued_cancel(int closing) {
    queue_signal(GPIO_PE_RISING);
    assert((closing ? kd_pin_close_wrap(NULL) : kd_pin_detach_irq(0)) == 0);
    kd_pin_irq_to_user_work(NULL, &_gpio_inst.irq_to_user_work);
    assert(atomic_load(&signals) == 0);
}
static void ioctl_detach_owner(int owner, int caller) {
    current_pid = owner;
    if (owner) {
        gpio_irqcfg_t cfg = { .pin = 0, .mode = GPIO_PE_HIGH,
                             .debounce_ms = 100, .signo = 10 };
        assert(kd_pin_control_wrap(NULL, KD_GPIO_IOCTL_SET_IRQ, &cfg) == RT_EOK);
    } else {
        assert(kd_pin_attach_irq(0, GPIO_PE_RISING, callback, (void *)0x1234) == RT_EOK);
    }
    gpio_cfg_t ctrl = { .pin = 0, .value = 1 };
    assert(kd_pin_control_wrap(NULL, KD_GPIO_IOCTL_CTRL_IRQ, &ctrl) == RT_EOK);
    kd_gpio_irq_t *irq = &_gpio_inst.irq_table[0];
    assert(irq->to_user.pid == owner);
    if (owner) {
        raise_edge(); dispatch(NULL);
        assert(irq->to_user.runing && irq->debounce.runing);
    }

    current_pid = caller;
    ctrl.value = 128;
    assert(kd_pin_control_wrap(NULL, KD_GPIO_IOCTL_CTRL_IRQ, &ctrl) == RT_EOK);
    if (caller && caller != owner) {
        /* A successful no-op must preserve both the callback and queued work. */
        assert(irq->to_user.pid == owner);
        assert(regs[0].inten & 1);
        if (owner) {
            assert(irq->hdr == kd_pin_irq_to_user_handler);
            assert(irq->to_user.cfg.signo == 10);
            assert(irq->to_user.runing && irq->debounce.runing);
            kd_pin_irq_to_user_work(NULL, &_gpio_inst.irq_to_user_work);
            kd_pin_irq_debounce_work(NULL, &_gpio_inst.debounce_work);
            assert(atomic_load(&signals) == 1 && !(regs[0].intmask & 1));
        } else {
            assert(irq->hdr == callback && irq->args == (void *)0x1234);
            assert(!(regs[0].intmask & 1));
            raise_edge(); dispatch(NULL);
            assert(atomic_load(&callbacks) == 1);
        }
    } else {
        assert(!irq->hdr && !irq->args && irq->to_user.pid == 0);
        assert(!(regs[0].inten & 1) && (regs[0].intmask & 1));
        assert(!irq->to_user.runing && !irq->debounce.runing);
        kd_pin_irq_to_user_work(NULL, &_gpio_inst.irq_to_user_work);
        kd_pin_irq_debounce_work(NULL, &_gpio_inst.debounce_work);
        raise_edge(); dispatch(NULL);
        assert(!atomic_load(&signals) && !atomic_load(&callbacks));
    }
}
static void debounce_cancel(void) {
    queue_signal(GPIO_PE_HIGH);
    assert(kd_pin_detach_irq(0) == 0);
    assert(kd_pin_attach_irq(0, GPIO_PE_RISING, callback, (void *)0x1234) == 0);
    kd_pin_irq_debounce_work(NULL, &_gpio_inst.debounce_work);
    kd_pin_irq_to_user_work(NULL, &_gpio_inst.irq_to_user_work);
    assert(regs[0].intmask == 1 && atomic_load(&signals) == 0);
}
static void active_wait(int notification) {
    pthread_t active, closer;
    if (notification) {
        queue_signal(GPIO_PE_RISING); block_signal = 1;
        pthread_create(&active, NULL, signal_thread, NULL);
    } else {
        assert(kd_pin_attach_irq(0, GPIO_PE_RISING, callback, (void *)0x1234) == 0);
        block_callback = 1; raise_edge(); pthread_create(&active, NULL, dispatch, NULL);
    }
    wait_for_gate(); pthread_create(&closer, NULL, detach_thread, NULL);
    wait_until_waiting();
    assert(!atomic_load(&detached));
    assert(kd_pin_attach_irq(0, GPIO_PE_RISING, callback, NULL) == -RT_EBUSY);
    release_gate(); pthread_join(active, NULL); pthread_join(closer, NULL);
    assert(atomic_load(&detached));
    assert(kd_pin_attach_irq(0, GPIO_PE_RISING, callback, (void *)0x1234) == 0);
}
static void orphan_eoi(void) {
    pthread_t isr, replacement;
    raise_edge(); block_eoi = 1;
    pthread_create(&isr, NULL, dispatch, NULL); wait_for_gate();
    pthread_create(&replacement, NULL, replace_thread, NULL);
    /* A replacement must wait until the old EOI has finished. The old driver
     * completes replacement here and subsequently erases its pending edge. */
    for (int i = 0; i < 100 && !atomic_load(&replacement_done); i++) usleep(1000);
    block_eoi = 0; release_gate();
    pthread_join(isr, NULL); pthread_join(replacement, NULL);
    assert(atomic_load(&replacement_done) && regs[0].intstatus == 1);
    dispatch(NULL); assert(atomic_load(&callbacks) == 1);
}
static void plic_dispatch(void) {
    if (!plic_enabled || plic_claimed) return;
    plic_claimed = 1; dispatch(NULL);
    /* PLIC completion for a disabled source is ignored. */
    if (plic_enabled) plic_claimed = 0;
}
static void self_reuse(void) {
    assert(kd_pin_attach_irq(0, GPIO_PE_RISING, self_callback, NULL) == 0);
    assert(kd_pin_irq_enable(0, 1) == 0); raise_edge(); plic_dispatch();
    assert(atomic_load(&callbacks) == 1);
    assert(kd_pin_attach_irq(0, GPIO_PE_RISING, callback, (void *)0x1234) == 0);
    assert(kd_pin_irq_enable(0, 1) == 0); raise_edge(); plic_dispatch();
    assert(atomic_load(&callbacks) == 2);
}
static void edge_during_callback(void) {
    assert(kd_pin_attach_irq(0, GPIO_PE_RISING, edge_callback, NULL) == 0);
    assert(kd_pin_irq_enable(0, 1) == 0); raise_edge(); dispatch(NULL);
    assert(atomic_load(&callbacks) == 1 && (regs[0].intstatus & 1));
    dispatch(NULL); assert(atomic_load(&callbacks) == 2 && !(regs[0].intstatus & 1));
}
static void queue_level(int pin, int debounce_ms) {
    kd_gpio_irq_t *irq = &_gpio_inst.irq_table[pin];
    assert(kd_pin_attach_irq(pin, GPIO_PE_HIGH, kd_pin_irq_to_user_handler, (void *)(long)pin) == 0);
    irq->to_user.pid = 123; irq->to_user.cfg.mode = GPIO_PE_HIGH;
    irq->to_user.cfg.signo = 10; irq->to_user.cfg.debounce_ms = debounce_ms;
    pthread_mutex_lock(&mmio_lock); regs[0].intstatus |= BIT(pin); pthread_mutex_unlock(&mmio_lock);
    kd_pin_irq_handler(IRQN_GPIO0_INTERRUPT + pin, NULL);
}
static void debounce_head_of_line(void) {
    cpu = 0; nest = 1;
    queue_level(0, 100); queue_level(1, 500); /* pin 1 becomes the list head */
    nest = 0;
    kd_pin_irq_debounce_work(NULL, &_gpio_inst.debounce_work);
    assert(unmask_time[0] == 1101 && unmask_time[1] == 1501);
    assert(!(regs[0].intmask & 3));
}
int main(int argc, char **argv) {
    assert(argc == 2); fixture();
    if (!strcmp(argv[1], "detach-cancels-queued-signal")) queued_cancel(0);
    else if (!strcmp(argv[1], "close-cancels-queued-signal")) queued_cancel(1);
    else if (!strcmp(argv[1], "ioctl-detach-preserves-other-owner")) ioctl_detach_owner(123, 456);
    else if (!strcmp(argv[1], "ioctl-detach-preserves-kernel-irq")) ioctl_detach_owner(0, 123);
    else if (!strcmp(argv[1], "ioctl-detach-own-irq")) ioctl_detach_owner(123, 123);
    else if (!strcmp(argv[1], "ioctl-kernel-detach-user-irq")) ioctl_detach_owner(123, 0);
    else if (!strcmp(argv[1], "ioctl-kernel-detach-kernel-irq")) ioctl_detach_owner(0, 0);
    else if (!strcmp(argv[1], "detach-cancels-debounce")) debounce_cancel();
    else if (!strcmp(argv[1], "detach-waits-for-callback")) active_wait(0);
    else if (!strcmp(argv[1], "detach-waits-for-active-signal")) active_wait(1);
    else if (!strcmp(argv[1], "orphan-eoi-preserves-new-edge")) orphan_eoi();
    else if (!strcmp(argv[1], "self-detach-allows-reuse")) self_reuse();
    else if (!strcmp(argv[1], "edge-during-callback-not-lost")) edge_during_callback();
    else if (!strcmp(argv[1], "debounce-does-not-wait-for-head")) debounce_head_of_line();
    else abort();
    return 0;
}
'''

structs = driver[driver.index("typedef struct _kd_gpio {"):driver.index("typedef struct _kd_gpio_mode {")]
functions = "\n\n".join(function(name) for name in (
    "kd_pin_irq_handler", "kd_pin_attach_irq", "kd_pin_detach_irq_locked",
    "kd_pin_irq_wait_tick", "kd_pin_irq_wait_idle", "kd_pin_detach_irq_for_owner", "kd_pin_detach_irq",
    "kd_pin_irq_enable", "kd_pin_irq_to_user_work", "kd_pin_irq_debounce_work",
    "kd_pin_irq_to_user_handler", "kd_pin_close_wrap", "kd_pin_control_wrap",
))
ioctls = "\n".join(re.findall(r"^#define KD_GPIO_IOCTL_.*", driver, re.M))
cases = ("detach-cancels-queued-signal", "close-cancels-queued-signal",
         "ioctl-detach-preserves-other-owner", "ioctl-detach-preserves-kernel-irq",
         "ioctl-detach-own-irq", "ioctl-kernel-detach-user-irq", "ioctl-kernel-detach-kernel-irq",
         "detach-cancels-debounce", "detach-waits-for-callback",
         "detach-waits-for-active-signal", "orphan-eoi-preserves-new-edge",
         "self-detach-allows-reuse", "edge-during-callback-not-lost",
         "debounce-does-not-wait-for-head")
failed = []
with tempfile.TemporaryDirectory(prefix="gpio-irq-test-") as tmp:
    source = Path(tmp) / "gpio_irq.c"
    binary = Path(tmp) / "gpio_irq"
    source.write_text(PRELUDE + structs + STUBS + ioctls + "\n" + functions + TESTS)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-pthread",
                    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
                    "-Wno-format", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(source), "-o", str(binary)], check=True)
    for case in cases:
        try:
            run = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=5)
            passed = run.returncode == 0
            detail = run.stdout + run.stderr
        except subprocess.TimeoutExpired:
            passed, detail = False, "timed out waiting for teardown\n"
        print(("PASS: " if passed else "FAIL: ") + case, flush=True)
        if not passed:
            failed.append(case)
            print(detail, end="", flush=True)
wait_test = subprocess.run([sys.executable, "-B", str(Path(__file__).with_name("test_gpio_irq_wait.py")),
                            "--source", str(args.source)])
raise SystemExit(bool(failed) or wait_test.returncode != 0)
