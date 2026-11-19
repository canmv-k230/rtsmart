#!/usr/bin/env python3
"""Exercise the real timer driver with controlled backend, IRQ and worker ordering."""
import os
from pathlib import Path
import subprocess
import tempfile

DRIVER = Path(__file__).resolve().parents[2] / "drivers/interdrv/hwtimer"

TEST = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "drv_timer.h"

#define RT_EOK 0
#define RT_ENOMEM 5
#define RT_NULL NULL
#define RT_WAITING_FOREVER -1
#define RT_DEVICE_OFLAG_RDWR 3
#define RT_IPC_FLAG_PRIO 1
#define RT_USING_HW_TIMER0
#define INIT_DEVICE_EXPORT(fn)
#define RT_ASSERT assert
#define LOG_E(...) ((void)0)
#define rt_memset memset
#define HW_TIMER_BASE_ADDR 0x1000
#define HW_TIMER_IO_SIZE 0x1000
#define SYSCTL_CLK_TIMER0_CLK 0
#define SYSCTL_CLK_PLL0_OCLK_DIV16 0
#define K230_PM_DEV_TIMER0 0
#define HWTIMER_CTRL_FREQ_SET 1
typedef int rt_err_t, rt_hwtimer_mode_t, sysctl_clk_node_e;
typedef long rt_base_t;
typedef unsigned rt_uint32_t;
typedef int32_t rt_int32_t;
typedef size_t rt_size_t;
enum k230_pm_dev_id { PM_TIMER };

struct dfs_file_ops;
typedef struct rt_device {
    void *user_data;
    const struct dfs_file_ops *fops;
    rt_err_t (*rx_indicate)(struct rt_device *, size_t);
    int open;
} *rt_device_t;
struct dfs_fnode { void *data; };
struct dfs_fd { struct dfs_fnode *fnode; };
struct dfs_file_ops {
    int (*open)(struct dfs_fd *);
    int (*close)(struct dfs_fd *);
    int (*ioctl)(struct dfs_fd *, int, void *);
    int (*read)(struct dfs_fd *, void *, size_t);
    int (*write)(struct dfs_fd *, const void *, size_t);
};
typedef struct rt_hwtimer {
    struct rt_device parent;
    unsigned freq;
    const struct rt_hwtimer_info *info;
    const struct rt_hwtimer_ops *ops;
} rt_hwtimer_t;
struct rt_hwtimer_info { unsigned maxfreq, minfreq, maxcnt, cntmode; };
#define HWTIMER_CNTMODE_DW 2
struct rt_hwtimer_ops {
    void (*init)(rt_hwtimer_t *, unsigned);
    int (*start)(rt_hwtimer_t *, unsigned, int);
    void (*stop)(rt_hwtimer_t *);
    unsigned (*count_get)(rt_hwtimer_t *);
    int (*control)(rt_hwtimer_t *, unsigned, void *);
};
struct rt_mutex { pthread_mutex_t lock; };
struct rt_work {
    void (*fn)(struct rt_work *, void *);
    void *arg;
    bool queued;
};
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int attempts, entered, work_inits;
static bool paused;
static int pid = 42, signals, last_pid, last_signo;
static void *last_value;
static kendryte_timer_t regs;
static struct dfs_fnode node;
static struct dfs_fd fd = { &node };

static void lock(pthread_mutex_t *m) { assert(!pthread_mutex_lock(m)); }
static void unlock(pthread_mutex_t *m) { assert(!pthread_mutex_unlock(m)); }
static void changed_wait(void) { assert(!pthread_cond_wait(&changed, &gate)); }
static void changed_signal(void) { assert(!pthread_cond_broadcast(&changed)); }

static int rt_mutex_init(struct rt_mutex *m, const char *name, int flags)
{
    assert(!pthread_mutex_init(&m->lock, NULL));
    return RT_EOK;
}
static int rt_mutex_take(struct rt_mutex *m, int timeout)
{
    lock(&gate);
    ++attempts;
    changed_signal();
    unlock(&gate);
    lock(&m->lock);
    return RT_EOK;
}
static void rt_mutex_release(struct rt_mutex *m) { unlock(&m->lock); }
static void enter_backend(void)
{
    lock(&gate);
    ++entered;
    changed_signal();
    while (paused)
        changed_wait();
    unlock(&gate);
}
static int devfs_device_open(rt_device_t d, int flags)
{
    enter_backend();
    if (d->open)
        return -EBUSY;
    d->open = 1;
    return 0;
}
static int rt_device_close(rt_device_t d)
{
    enter_backend();
    assert(d->open);
    ((rt_hwtimer_t *)d)->ops->init((rt_hwtimer_t *)d, 0);
    d->rx_indicate = NULL;
    d->open = 0;
    return 0;
}
static void rt_device_set_rx_indicate(rt_device_t d, int (*cb)(rt_device_t, size_t))
{
    d->rx_indicate = cb;
}
static void rt_device_hwtimer_isr(rt_hwtimer_t *t)
{
    if (t->parent.rx_indicate)
        t->parent.rx_indicate(&t->parent, 0);
}
static int rt_device_hwtimer_register(rt_hwtimer_t *t, const char *name, void *arg)
{
    t->parent.user_data = arg;
    node.data = &t->parent;
    return 0;
}
static void rt_work_init(struct rt_work *w, void (*fn)(struct rt_work *, void *), void *arg)
{
    ++work_inits;
    assert(!w->queued);
    w->fn = fn;
    w->arg = arg;
}
static void rt_work_submit(struct rt_work *w, int ticks) { w->queued = true; }
static void rt_work_cancel(struct rt_work *w) { w->queued = false; }
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) {}
static void rt_hw_interrupt_install(int irq, void (*fn)(int, void *), void *arg, const char *name) {}
static void rt_hw_interrupt_umask(int irq) {}
static void *rt_ioremap(void *addr, size_t size) { return &regs; }
static int k230_pm_get(enum k230_pm_dev_id id) { return 0; }
static int k230_pm_put(enum k230_pm_dev_id id) { return 0; }
static int k230_clk_set_parent_div(int clk, int parent, int a, int b) { return 0; }
static int lwp_getpid(void) { return pid; }
static size_t lwp_get_from_user(void *dst, void *src, size_t len)
{
    memcpy(dst, src, len);
    return len;
}
static size_t lwp_put_to_user(void *dst, const void *src, size_t len)
{
    memcpy(dst, src, len);
    return len;
}
static void lwp_kill_ext(int target, int signo, siginfo_t *info)
{
    ++signals;
    last_pid = target;
    last_signo = signo;
    last_value = info->si_ptr;
}
static int rt_device_control(rt_device_t d, int cmd, void *arg) { return 0; }
static int rt_device_read(rt_device_t d, int pos, void *buf, size_t size) { return 0; }
static int rt_device_write(rt_device_t d, int pos, const void *buf, size_t size) { return 0; }

#include "drv_timer.c"

struct operation { int (*call)(struct dfs_fd *); int result; };
static void *run(void *arg)
{
    struct operation *op = arg;
    op->result = op->call(&fd);
    return NULL;
}

static void overlap(int (*first)(struct dfs_fd *), int second_result)
{
    pthread_t a, b;
    struct operation one = { first, -1 }, two = { timer_fops_open, -1 };
    lock(&gate);
    paused = true;
    int initial_entered = entered, initial_attempts = attempts;
    assert(!pthread_create(&a, NULL, run, &one));
    while (entered == initial_entered)
        changed_wait();
    assert(!pthread_create(&b, NULL, run, &two));
    while (attempts < initial_attempts + 2 && entered == initial_entered + 1)
        changed_wait();
    assert(entered == initial_entered + 1);
    paused = false;
    changed_signal();
    unlock(&gate);
    assert(!pthread_join(a, NULL));
    assert(!pthread_join(b, NULL));
    assert(one.result == 0 && two.result == second_result);
}

static void irq_tests(rt_device_t device)
{
    hwtimer_irqcfg_t cfg = { 1, SIGUSR1, &regs };
    assert(!device->rx_indicate);
    assert(!timer_fops_ioctl(&fd, HWTIMER_CTRL_IRQ_SET, &cfg));
    kd_hwtimer_isr(0, device);
#ifdef RT_TIMER_IRQ_METHOD_WORKQUEUE
    struct kd_timer_Type *state = device->user_data;
    struct rt_work *work = &state->send_sig_work;
    assert(work->queued && signals == 0);
    work->queued = false;  /* The worker dequeued the old expiry but has not run. */
    assert(!timer_fops_close(&fd));
    work->fn(work, work->arg);
    assert(signals == 0);
    assert(!device->rx_indicate);

    assert(!timer_fops_open(&fd));
    assert(!timer_fops_ioctl(&fd, HWTIMER_CTRL_IRQ_SET, &cfg));
    kd_hwtimer_isr(0, device);
    assert(work->queued);
    assert(!timer_fops_close(&fd));
    assert(!work->queued);

    assert(!timer_fops_open(&fd));
    assert(!timer_fops_ioctl(&fd, HWTIMER_CTRL_IRQ_SET, &cfg));
    kd_hwtimer_isr(0, device);
    assert(work->queued);
    pid++;
    cfg.signo = SIGUSR2;
    assert(!timer_fops_ioctl(&fd, HWTIMER_CTRL_IRQ_SET, &cfg));
    assert(!work->queued);
    work->fn(work, work->arg);
    assert(signals == 0);
    kd_hwtimer_isr(0, device);
    assert(work->queued);
    work->queued = false;
    work->fn(work, work->arg);
    assert(signals == 1 && last_pid == pid && last_signo == SIGUSR2 && last_value == &regs);
    kd_hwtimer_isr(0, device);
    cfg.enable = 0;
    assert(!timer_fops_ioctl(&fd, HWTIMER_CTRL_IRQ_SET, &cfg));
    assert(!work->queued && !device->rx_indicate && work_inits == 1);
    work->fn(work, work->arg);
    assert(signals == 1);
#else
    assert(signals == 1 && last_pid == pid && last_signo == SIGUSR1 && last_value == &regs);
#endif
    assert(!timer_fops_close(&fd));
    assert(!device->rx_indicate);
}

int main(void)
{
    assert(rt_hw_timer_init() == 0);
    overlap(timer_fops_open, -EBUSY);
    overlap(timer_fops_close, 0);
    irq_tests(node.data);
    puts("PASS: timer lifecycle and signal isolation");
}
'''

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    for header in (
        "rtdevice", "rthw", "rtthread", "dfs_posix", "devfs", "lwp_user_mm",
        "ioremap", "riscv_io", "sysctl_clk", "sysctl_rst", "k230_pm", "board", "rtdbg",
    ):
        (root / f"{header}.h").touch()
    source = root / "timer_test.c"
    source.write_text(TEST)
    for mode in ("WORKQUEUE", "DIRECT"):
        program = root / mode.lower()
        defines = ["-DRT_TIMER_IRQ_METHOD_DIRECT"] if mode == "DIRECT" else []
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=gnu11", "-pthread", "-g",
             "-fsanitize=undefined", "-fno-sanitize-recover=all",
             *defines, "-I", str(root), "-I", str(DRIVER),
             str(source), "-o", str(program)],
            check=True,
        )
        print(f"{mode}:", flush=True)
        subprocess.run([str(program)], check=True, timeout=10)
