#!/usr/bin/env python3
"""Check that a file close and a first open serialize per path, not globally.

Compiles the production dfs/src/dfs.c, dfs/src/dfs_file.c and
dfs/filesystems/devfs/devfs.c unmodified against RT-Thread stubs backed by
POSIX threads, and drives them from several real threads. Mutexes are
recursive, as RT-Thread's are; semaphores count.

A thread "preempts" another at a chosen point: releasing the fnode lock
(dfs_fm_lock) hands the CPU to a waiting thread, as rt_mutex_release() does
when a thread of higher priority waits. The stubs run the armed job on a
worker thread and return once it has finished or blocked.

- Two closes of one path: exactly one is the backend's last close, and the
  fnode leaves the hash before it is freed.
- A close that sleeps in the backend does not hold up opens or closes of
  other paths.
- An open of a path whose last close or first open is running waits for it,
  then opens the path afresh; the backend never sees them overlap.
- A close waits for later opens in flight, so a failing one cannot take
  the backend's last close away; new later opens wait behind it.
- No waiter takes the wakeup of a waiter for another path.
- A later open's backend that closes its own path gets -EDEADLK; one that
  opens it again goes ahead, even while a close waits.
- A descriptor released after a retryable close failure keeps its hashed
  fnode, and the close is retried.
- close() of a descriptor another thread is still opening fails with EBADF;
  dup() of it and dup2() onto it are refused.
- Open flags cannot set DFS's own descriptor bits.
- dfs_file_is_open() (unlink, rename) waits out a running close.
- The last put of a descriptor closed through another reference does not
  retry a close.
- A non-last close on a descriptor that never goes through fd_free() (the
  stack descriptors of dfs_fs.c and netmgmt.c) still drops its reference.
- A retryable close failure keeps the reference; a terminal one does not.
- A backend that opens its own path from inside its close gets -EDEADLK.
- Later opens of one path run one at a time (drivers assume it).
- A queued close retried from fd_new() does not wait for a busy path.
Run with python3 -B kernel/bsp/maix3/testcase/host/test_dfs_close.py.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

BSP = Path(__file__).resolve().parents[2]
RTT = BSP.parents[1] / "rt-thread"
DFS = RTT / "components/dfs"

RTCONFIG = r"""
#pragma once
#define RT_NAME_MAX 8
#define RT_USING_DEVICE
#define RT_USING_DEVICE_OPS
#define RT_USING_POSIX
#define RT_USING_DFS
#define RT_USING_DFS_DEVFS
#define DFS_FILESYSTEMS_MAX 4
#define DFS_FILESYSTEM_TYPES_MAX 4
#define DFS_FD_MAX 16
#define DFS_PATH_MAX 256
"""

STUBS = r"""
#pragma once
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "rtconfig.h"

typedef long rt_base_t;
typedef unsigned long rt_ubase_t;
typedef rt_base_t rt_err_t;
typedef int8_t rt_int8_t;
typedef uint8_t rt_uint8_t;
typedef int16_t rt_int16_t;
typedef uint16_t rt_uint16_t;
typedef int32_t rt_int32_t;
typedef uint32_t rt_uint32_t;
typedef rt_ubase_t rt_size_t;
typedef rt_base_t rt_off_t;
typedef int rt_bool_t;
#define RT_NULL NULL
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_EOK 0
#define RT_ERROR 1
#define RT_ETIMEOUT 2
#define RT_EFULL 3
#define RT_EEMPTY 4
#define RT_ENOMEM 5
#define RT_ENOSYS 6
#define RT_EBUSY 7
#define RT_EIO 8
#define RT_EINTR 9
#define RT_EINVAL 10
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_PRIO 0
#define RT_IPC_FLAG_FIFO 0
#define RT_DEVICE_FLAG_RDWR 0x003
#define RT_DEVICE_FLAG_STANDALONE 0x008
#define RT_DEVICE_FLAG_ACTIVATED 0x010
#define RT_DEVICE_FLAG_REMOVABLE 0x004
#define RT_DEVICE_OFLAG_RDWR 0x003
#define RT_DEVICE_OFLAG_OPEN 0x008
#define rt_inline static inline
#define RTM_EXPORT(symbol)
#define INIT_PREV_EXPORT(fn)
#define MSH_CMD_EXPORT(...)
#define RT_ASSERT(x) assert(x)
#define LOG_D(...) ((void)0)
#define LOG_I(...) ((void)0)
#define LOG_W(...) ((void)0)
extern int test_log_errors;
#define LOG_E(...) ((void)test_log_errors++)
#ifndef DT_REG
#define DT_REG 0x08
#endif
#ifndef DT_DIR
#define DT_DIR 0x04
#define DT_FIFO 0x01
#define DT_CHR 0x02
#define DT_BLK 0x06
#endif

typedef struct rt_list_node { struct rt_list_node *next, *prev; } rt_list_t;
#define rt_container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define rt_list_entry(node, type, member) rt_container_of(node, type, member)
static inline void rt_list_init(rt_list_t *l) { l->next = l->prev = l; }
static inline void rt_list_insert_after(rt_list_t *l, rt_list_t *n)
{
    l->next->prev = n; n->next = l->next; l->next = n; n->prev = l;
}
static inline void rt_list_remove(rt_list_t *n)
{
    n->next->prev = n->prev; n->prev->next = n->next; n->next = n->prev = n;
}
static inline void rt_list_insert_before(rt_list_t *l, rt_list_t *n)
{
    l->prev->next = n; n->prev = l->prev; l->prev = n; n->next = l;
}
static inline int rt_list_isempty(const rt_list_t *l) { return l->next == l; }

enum rt_object_class_type { RT_Object_Class_Device = 0x09, RT_Object_Class_Static = 0x80 };
struct rt_object { char name[RT_NAME_MAX]; rt_uint8_t type; rt_uint8_t flag; rt_list_t list; };
typedef struct rt_object *rt_object_t;
struct rt_object_information { enum rt_object_class_type type; rt_list_t object_list; rt_size_t object_size; };
struct rt_object_information *rt_object_get_information(enum rt_object_class_type type);

struct rt_wqueue { rt_uint32_t flag; rt_list_t waiting_list; };
typedef struct rt_wqueue rt_wqueue_t;
struct rt_pollreq;
static inline void rt_poll_add(rt_wqueue_t *q, struct rt_pollreq *req) {}

enum rt_device_class_type {
    RT_Device_Class_Char = 0, RT_Device_Class_Block, RT_Device_Class_Pipe = 16, RT_Device_Class_Bus = 31,
};
typedef struct rt_device *rt_device_t;
struct rt_device_ops {
    rt_err_t (*init)(rt_device_t dev);
    rt_err_t (*open)(rt_device_t dev, rt_uint16_t oflag);
    rt_err_t (*close)(rt_device_t dev);
    rt_size_t (*read)(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);
    rt_size_t (*write)(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size);
    rt_err_t (*control)(rt_device_t dev, int cmd, void *args);
};
struct dfs_file_ops;
struct rt_device {
    struct rt_object parent;
    enum rt_device_class_type type;
    rt_uint16_t flag;
    rt_uint16_t open_flag;
    rt_uint8_t ref_count;
    rt_uint8_t device_id;
    const struct rt_device_ops *ops;
    const struct dfs_file_ops *fops;
    struct rt_wqueue wait_queue;
    void *user_data;
};
rt_device_t rt_device_find(const char *name);
rt_err_t rt_device_open(rt_device_t dev, rt_uint16_t oflag);
rt_err_t rt_device_close(rt_device_t dev);
rt_size_t rt_device_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);
rt_size_t rt_device_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size);
rt_err_t rt_device_control(rt_device_t dev, int cmd, void *arg);

/* Threads are pthreads; mutexes are recursive, as rt_mutex is. */
typedef struct rt_thread *rt_thread_t;
static inline rt_thread_t rt_thread_self(void) { return (rt_thread_t)(uintptr_t)pthread_self(); }
struct rt_mutex { pthread_mutex_t m; const char *name; };
typedef struct rt_mutex *rt_mutex_t;
rt_err_t rt_mutex_init(struct rt_mutex *m, const char *name, int flag);
rt_err_t rt_mutex_take(struct rt_mutex *m, rt_int32_t t);
rt_err_t rt_mutex_release(struct rt_mutex *m);
rt_err_t rt_mutex_detach(struct rt_mutex *m);
struct rt_semaphore { pthread_mutex_t m; pthread_cond_t c; unsigned value; };
typedef struct rt_semaphore *rt_sem_t;
rt_err_t rt_sem_init(struct rt_semaphore *s, const char *name, rt_uint32_t value, rt_uint8_t flag);
rt_err_t rt_sem_take(struct rt_semaphore *s, rt_int32_t t);
#define rt_sem_take_interruptible rt_sem_take
rt_err_t rt_sem_release(struct rt_semaphore *s);
rt_err_t rt_sem_detach(struct rt_semaphore *s);
static inline void rt_enter_critical(void) {}
static inline void rt_exit_critical(void) {}
rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t level);
void rt_set_errno(rt_err_t e);
rt_err_t rt_get_errno(void);
#define rt_malloc malloc
#define rt_calloc calloc
#define rt_realloc realloc
#define rt_free free
#define rt_strdup strdup
#define rt_strcmp strcmp
#define rt_strncmp strncmp
#define rt_strncpy strncpy
#define rt_strlen strlen
#define rt_memset memset
#define rt_memcpy memcpy
#define rt_snprintf snprintf
#define rt_kprintf(...) ((void)0)
"""

ENV = r"""
#include <rtthread.h>
#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>

static __thread rt_err_t thread_errno;
int test_log_errors;
void rt_set_errno(rt_err_t e) { thread_errno = e; }
rt_err_t rt_get_errno(void) { return thread_errno; }

/* Interrupts off: one global recursive lock. */
static pthread_mutex_t irq_lock;
static __thread int irq_depth;
rt_base_t rt_hw_interrupt_disable(void)
{
    if (irq_depth++ == 0) pthread_mutex_lock(&irq_lock);
    return 0;
}
void rt_hw_interrupt_enable(rt_base_t level)
{
    if (--irq_depth == 0) pthread_mutex_unlock(&irq_lock);
}

/* --- Scheduling model ------------------------------------------------ */

/* A job runs on the worker thread when the armed lock is released, and the
 * releasing thread continues once the job has finished or blocked. */
static pthread_mutex_t sched_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sched_cond = PTHREAD_COND_INITIALIZER;
static const char *armed_lock;
static void (*armed_job)(void);
static void (*worker_job)(void);
static int worker_busy, worker_blocked;
static pthread_t worker_thread;
static __thread int is_worker;

static void note_blocking(int delta)
{
    if (!is_worker)
        return;
    pthread_mutex_lock(&sched_lock);
    worker_blocked += delta;
    pthread_cond_broadcast(&sched_cond);
    pthread_mutex_unlock(&sched_lock);
}

static void *worker_main(void *arg)
{
    is_worker = 1;
    pthread_mutex_lock(&sched_lock);
    for (;;)
    {
        while (!worker_job)
            pthread_cond_wait(&sched_cond, &sched_lock);
        void (*job)(void) = worker_job;
        pthread_mutex_unlock(&sched_lock);
        job();
        pthread_mutex_lock(&sched_lock);
        worker_job = NULL;
        worker_busy = 0;
        pthread_cond_broadcast(&sched_cond);
    }
    return NULL;
}

void test_sched_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&irq_lock, &a);
    pthread_create(&worker_thread, NULL, worker_main, NULL);
}

/* Preempt to @job the next time a mutex named @lock is released. */
void test_preempt_on_release(const char *lock, void (*job)(void))
{
    pthread_mutex_lock(&sched_lock);
    armed_lock = lock;
    armed_job = job;
    pthread_mutex_unlock(&sched_lock);
}

/* Run @job on the worker; return once it has finished or blocked. */
void test_run_on_worker(void (*job)(void))
{
    pthread_mutex_lock(&sched_lock);
    assert(!worker_busy);
    worker_busy = 1;
    worker_blocked = 0;
    worker_job = job;
    pthread_cond_broadcast(&sched_cond);
    while (worker_busy && !worker_blocked)
        pthread_cond_wait(&sched_cond, &sched_lock);
    pthread_mutex_unlock(&sched_lock);
}

/* Wait up to @ms for the worker's job to finish. */
int test_worker_wait(int ms)
{
    struct timespec ts;
    int done;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&sched_lock);
    while (worker_busy)
        if (pthread_cond_timedwait(&sched_cond, &sched_lock, &ts))
            break;
    done = !worker_busy;
    pthread_mutex_unlock(&sched_lock);
    return done;
}

rt_err_t rt_mutex_init(struct rt_mutex *m, const char *name, int flag)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a);
    m->name = name;
    return RT_EOK;
}
rt_err_t rt_mutex_take(struct rt_mutex *m, rt_int32_t t)
{
    if (pthread_mutex_trylock(&m->m) == 0)
        return RT_EOK;
    note_blocking(1);
    pthread_mutex_lock(&m->m);
    note_blocking(-1);
    return RT_EOK;
}
rt_err_t rt_mutex_release(struct rt_mutex *m)
{
    void (*job)(void) = NULL;

    pthread_mutex_unlock(&m->m);
    pthread_mutex_lock(&sched_lock);
    if (armed_lock && m->name && strcmp(armed_lock, m->name) == 0 && !is_worker)
    {
        job = armed_job;
        armed_lock = NULL;
        armed_job = NULL;
    }
    pthread_mutex_unlock(&sched_lock);
    if (job)
        test_run_on_worker(job);
    return RT_EOK;
}
rt_err_t rt_mutex_detach(struct rt_mutex *m) { return RT_EOK; }

rt_err_t rt_sem_init(struct rt_semaphore *s, const char *name, rt_uint32_t value, rt_uint8_t flag)
{
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->value = value;
    return RT_EOK;
}
rt_err_t rt_sem_take(struct rt_semaphore *s, rt_int32_t t)
{
    pthread_mutex_lock(&s->m);
    if (!s->value)
    {
        note_blocking(1);
        while (!s->value)
            pthread_cond_wait(&s->c, &s->m);
        note_blocking(-1);
    }
    s->value--;
    pthread_mutex_unlock(&s->m);
    return RT_EOK;
}
rt_err_t rt_sem_detach(struct rt_semaphore *s)
{
    pthread_mutex_destroy(&s->m);
    pthread_cond_destroy(&s->c);
    return RT_EOK;
}
rt_err_t rt_sem_release(struct rt_semaphore *s)
{
    pthread_mutex_lock(&s->m);
    s->value++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return RT_EOK;
}

/* --- Devices and file systems ---------------------------------------- */

static struct rt_object_information devices = {
    RT_Object_Class_Device, { &devices.object_list, &devices.object_list }, 0,
};
struct rt_object_information *rt_object_get_information(enum rt_object_class_type type)
{
    return type == RT_Object_Class_Device ? &devices : RT_NULL;
}
void test_register_device(struct rt_device *dev, const char *name)
{
    strncpy(dev->parent.name, name, RT_NAME_MAX);
    dev->parent.type = RT_Object_Class_Device;
    rt_list_insert_after(&devices.object_list, &dev->parent.list);
}
rt_device_t rt_device_find(const char *name)
{
    rt_list_t *n;
    for (n = devices.object_list.next; n != &devices.object_list; n = n->next)
    {
        struct rt_object *o = rt_list_entry(n, struct rt_object, list);
        if (strncmp(o->name, name, RT_NAME_MAX) == 0)
            return (rt_device_t)o;
    }
    return RT_NULL;
}
/* Only dfs_file_ops drivers are used: devfs never calls these. */
rt_err_t rt_device_open(rt_device_t dev, rt_uint16_t oflag) { abort(); }
rt_err_t rt_device_close(rt_device_t dev) { abort(); }
rt_size_t rt_device_read(rt_device_t dev, rt_off_t pos, void *b, rt_size_t s) { abort(); }
rt_size_t rt_device_write(rt_device_t dev, rt_off_t pos, const void *b, rt_size_t s) { abort(); }
rt_err_t rt_device_control(rt_device_t dev, int cmd, void *arg) { abort(); }

/* devfs on /dev, mounted by dfs_init(). */
static struct dfs_filesystem devfs;
int dfs_register(const struct dfs_filesystem_ops *ops)
{
    devfs.path = "/dev";
    devfs.ops = ops;
    return 0;
}
int dfs_mount(const char *device_name, const char *path, const char *filesystemtype,
              unsigned long rwflag, const void *data)
{
    return 0;
}
int dfs_unmount(const char *specialfile) { return 0; }
struct dfs_filesystem *dfs_filesystem_lookup(const char *path)
{
    return strncmp(path, "/dev", 4) == 0 ? &devfs : RT_NULL;
}
"""

CASES = r"""
#include <rtthread.h>
#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>
#include <unistd.h>

#ifndef DFS_F_OPENING               /* a dfs tree from before it */
#define DFS_F_OPENING 0
#endif

void test_sched_init(void);
void test_preempt_on_release(const char *lock, void (*job)(void));
void test_run_on_worker(void (*job)(void));
int test_worker_wait(int ms);
void test_register_device(struct rt_device *dev, const char *name);
int dfs_init(void);

static unsigned failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* A driver whose behaviour each case sets. It records first opens and last
 * closes, as backends see them (fnode->ref_count), and checks that an open
 * and a close of the path never run at the same time when either is the
 * backend's first or last. */
struct drv {
    struct rt_device dev;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int opens, first_opens, closes, last_closes;
    int in_last_close, in_first_open, overlap;
    int gate_close, gate_open;      /* block in close / first open until cleared */
    int gate_later_open;            /* block in a later open until cleared */
    int in_gate;
    int close_result, close_terminal, open_result;
    const char *reopen_path;        /* open this path from inside close */
    int reopen_result;
    struct dfs_fd *close_in_open;   /* close this from inside a later open */
    int close_in_open_result;
    struct dfs_fd *open_in_open;    /* open the path again into this, from a later open */
    int open_in_open_result;
    int in_later_open, later_overlap; /* later opens of the path running at once */
};
static struct drv slow, other;

static struct drv *drv_of(struct dfs_fd *fd)
{
    return (struct drv *)fd->fnode->data;
}

static void gate(struct drv *d, int *flag)
{
    pthread_mutex_lock(&d->lock);
    d->in_gate++;
    pthread_cond_broadcast(&d->cond);
    while (*flag)
        pthread_cond_wait(&d->cond, &d->lock);
    d->in_gate--;
    pthread_mutex_unlock(&d->lock);
}

static void open_gate(struct drv *d, int *flag)
{
    pthread_mutex_lock(&d->lock);
    *flag = 0;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

static int drv_open(struct dfs_fd *fd)
{
    struct drv *d = drv_of(fd);
    int first = fd->fnode->ref_count == 1;
    int result = d->open_result;    /* decided on entry */

    if (first)
    {
        if (d->in_last_close)
            d->overlap++;
        d->in_first_open = 1;
    }
    if (first && d->gate_open)
        gate(d, &d->gate_open);
    if (!first)
    {
        pthread_mutex_lock(&d->lock);
        if (d->in_later_open)
            d->later_overlap++;
        d->in_later_open++;
        pthread_mutex_unlock(&d->lock);
    }
    if (!first && d->gate_later_open)
        gate(d, &d->gate_later_open);
    if (!first && d->open_in_open)
    {
        struct dfs_fd *nested = d->open_in_open;

        d->open_in_open = NULL;
        d->open_in_open_result = dfs_file_open(nested, "/dev/slow", O_RDWR);
    }
    if (!first && d->close_in_open)
    {
        struct dfs_fd *victim = d->close_in_open;

        d->close_in_open = NULL;
        d->close_in_open_result = dfs_file_close(victim);
    }
    if (first)
        d->in_first_open = 0;
    else
    {
        pthread_mutex_lock(&d->lock);
        d->in_later_open--;
        pthread_mutex_unlock(&d->lock);
    }
    if (result < 0)
        return result;
    d->opens++;
    d->first_opens += first;
    return 0;
}

static int drv_close(struct dfs_fd *fd)
{
    struct drv *d = drv_of(fd);
    int last = fd->fnode->ref_count == 1;

    if (last)
    {
        if (d->in_first_open)
            d->overlap++;
        d->in_last_close = 1;
    }
    if (d->gate_close)
        gate(d, &d->gate_close);
    if (d->reopen_path)
    {
        struct dfs_fd nested;
        const char *path = d->reopen_path;

        d->reopen_path = NULL;
        memset(&nested, 0, sizeof(nested));
        nested.ref_count = 1;
        d->reopen_result = dfs_file_open(&nested, path, O_RDWR);
        if (d->reopen_result == 0)
            dfs_file_close(&nested);
    }
    if (last)
        d->in_last_close = 0;
    if (d->close_result < 0)
    {
        if (d->close_terminal)
            fd->flags |= DFS_F_CLOSE_TERMINAL;
        return d->close_result;
    }
    d->closes++;
    d->last_closes += last;
    return 0;
}

static const struct dfs_file_ops drv_fops = { .open = drv_open, .close = drv_close };

/* Wait until a thread sits in @d's gate. */
static void wait_in_gate(struct drv *d)
{
    pthread_mutex_lock(&d->lock);
    while (!d->in_gate)
        pthread_cond_wait(&d->cond, &d->lock);
    pthread_mutex_unlock(&d->lock);
}

static int open_fd(const char *path)
{
    int fd = fd_new();
    struct dfs_fd *d = fd_get(fd);
    int result;

    assert(fd >= 0 && d);
    result = dfs_file_open(d, path, O_RDWR);
    if (result < 0)
    {
        fd_release(fd);
        return result;
    }
    return fd;
}

static int close_fd(int fd)
{
    return fdt_fd_close(dfs_fdtable_get(), fd, -1);
}

/* Run @fn on a new thread; whether it finished within @ms. The thread is
 * joined later by thread_join(). */
struct timed { pthread_t t; void (*fn)(void); int done; pthread_mutex_t m; pthread_cond_t c; };
static void *timed_main(void *arg)
{
    struct timed *t = arg;
    t->fn();
    pthread_mutex_lock(&t->m);
    t->done = 1;
    pthread_cond_broadcast(&t->c);
    pthread_mutex_unlock(&t->m);
    return NULL;
}
static int thread_run(struct timed *t, void (*fn)(void), int ms)
{
    struct timespec ts;
    int done;

    t->fn = fn;
    t->done = 0;
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    pthread_create(&t->t, NULL, timed_main, t);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&t->m);
    while (!t->done)
        if (pthread_cond_timedwait(&t->c, &t->m, &ts))
            break;
    done = t->done;
    pthread_mutex_unlock(&t->m);
    return done;
}
static void thread_join(struct timed *t) { pthread_join(t->t, NULL); }

static int path_is_open(const char *path)
{
    return dfs_file_is_open(path);
}

/* --- Two closes of one path ------------------------------------------ */

static int race_fd;
static void close_race_fd(void) { CHECK(close_fd(race_fd) == 0); }

static void test_two_closes_of_one_path(void)
{
    int a = open_fd("/dev/slow"), b = open_fd("/dev/slow");

    CHECK(a >= 0 && b >= 0 && slow.first_opens == 1);
    /* B's close preempts A's when A releases the fnode lock. */
    race_fd = b;
    test_preempt_on_release("dfs_mgr", close_race_fd);
    CHECK(close_fd(a) == 0);
    CHECK(test_worker_wait(2000));
    CHECK(slow.closes == 2);
    CHECK(slow.last_closes == 1);
    /* Freed only once out of the hash: a lookup must not reach it. */
    CHECK(!path_is_open("/dev/slow"));
    a = open_fd("/dev/slow");
    CHECK(a >= 0 && close_fd(a) == 0);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- A sleeping close and other paths --------------------------------- */

static int slow_fd, other_fd, worker_result;
static void close_slow_fd(void) { worker_result = close_fd(slow_fd); }
static void open_other(void) { other_fd = open_fd("/dev/other"); }
static void close_other(void) { worker_result = close_fd(other_fd); }

static void test_sleeping_close_leaves_other_paths_alone(void)
{
    int keep = open_fd("/dev/other");
    pthread_t t;

    slow_fd = open_fd("/dev/slow");
    CHECK(slow_fd >= 0 && keep >= 0);
    slow.gate_close = 1;
    /* The close sleeps in the backend on another thread. */
    pthread_create(&t, NULL, (void *(*)(void *))close_slow_fd, NULL);
    wait_in_gate(&slow);

    /* An open and a close of another path go ahead meanwhile. */
    test_run_on_worker(open_other);
    CHECK(test_worker_wait(500));
    CHECK(other_fd >= 0);
    if (test_worker_wait(0) && other_fd >= 0)
    {
        test_run_on_worker(close_other);
        CHECK(test_worker_wait(500));
        CHECK(worker_result == 0);
    }

    open_gate(&slow, &slow.gate_close);
    pthread_join(t, NULL);
    CHECK(test_worker_wait(2000));
    CHECK(worker_result == 0);
    CHECK(slow.last_closes == 1);
    CHECK(close_fd(keep) == 0);
    CHECK(other.last_closes == 1 && other.first_opens == 1 && other.opens == 2);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
    memset(&other.opens, 0, sizeof(other) - offsetof(struct drv, opens));
}

/* --- Same path: open during the last close, open during a first open -- */

static int reopen_fd;
static void reopen_slow(void) { reopen_fd = open_fd("/dev/slow"); }

static void test_open_waits_for_last_close(void)
{
    pthread_t t;

    slow_fd = open_fd("/dev/slow");
    slow.gate_close = 1;
    pthread_create(&t, NULL, (void *(*)(void *))close_slow_fd, NULL);
    wait_in_gate(&slow);

    test_run_on_worker(reopen_slow);
    /* Not while the last close runs. */
    CHECK(!test_worker_wait(200));
    open_gate(&slow, &slow.gate_close);
    pthread_join(t, NULL);
    CHECK(test_worker_wait(2000));
    CHECK(worker_result == 0 && reopen_fd >= 0);
    CHECK(slow.overlap == 0);
    /* The reopen was a first open, after the last close. */
    CHECK(slow.last_closes == 1 && slow.first_opens == 2);
    CHECK(close_fd(reopen_fd) == 0);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

static int first_fd;
static void first_open_slow(void) { first_fd = open_fd("/dev/slow"); }

static void test_open_waits_for_first_open(void)
{
    pthread_t t;

    slow.gate_open = 1;
    pthread_create(&t, NULL, (void *(*)(void *))first_open_slow, NULL);
    wait_in_gate(&slow);
    /* A second open of the path waits for the first to finish; one of
     * another path does not. */
    test_run_on_worker(reopen_slow);
    CHECK(!test_worker_wait(200));
    other_fd = -1;
    struct timed u;
    int other_done = thread_run(&u, open_other, 500);
    CHECK(other_done);

    open_gate(&slow, &slow.gate_open);
    thread_join(&u);
    CHECK(other_fd >= 0);
    CHECK(close_fd(other_fd) == 0);
    pthread_join(t, NULL);
    CHECK(test_worker_wait(2000));
    CHECK(first_fd >= 0 && reopen_fd >= 0);
    CHECK(slow.first_opens == 1 && slow.opens == 2);
    CHECK(close_fd(first_fd) == 0 && close_fd(reopen_fd) == 0);
    CHECK(slow.last_closes == 1);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
    memset(&other.opens, 0, sizeof(other) - offsetof(struct drv, opens));
}

static void test_failed_first_open_wakes_waiter(void)
{
    pthread_t t;

    slow.gate_open = 1;
    slow.open_result = -EIO;
    pthread_create(&t, NULL, (void *(*)(void *))first_open_slow, NULL);
    wait_in_gate(&slow);
    test_run_on_worker(reopen_slow);
    CHECK(!test_worker_wait(200));
    /* The first open fails; the waiter opens the path afresh, and so fails
     * too unless the driver changed its mind. */
    slow.open_result = 0;
    open_gate(&slow, &slow.gate_open);
    pthread_join(t, NULL);
    CHECK(test_worker_wait(2000));
    CHECK(first_fd == -EIO || reopen_fd == -EIO);
    CHECK(first_fd >= 0 || reopen_fd >= 0);
    CHECK(slow.first_opens == 1);
    CHECK(close_fd(first_fd >= 0 ? first_fd : reopen_fd) == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

static int later_fd;
static void later_open_slow(void) { later_fd = open_fd("/dev/slow"); }

static void test_close_waits_for_later_open(void)
{
    pthread_t t;

    slow_fd = open_fd("/dev/slow");
    /* A later open is in flight, and will fail. */
    slow.gate_later_open = 1;
    slow.open_result = -EIO;
    pthread_create(&t, NULL, (void *(*)(void *))later_open_slow, NULL);
    wait_in_gate(&slow);
    slow.open_result = 0;
    /* The close would not be the backend's last while that open counts;
     * it waits for the open. */
    test_run_on_worker(close_slow_fd);
    CHECK(!test_worker_wait(200));
    open_gate(&slow, &slow.gate_later_open);
    pthread_join(t, NULL);
    CHECK(test_worker_wait(2000));
    CHECK(later_fd == -EIO && worker_result == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- Waking: no waiter takes another's wakeup --------------------------- */

static int stolen_other_fd, waiter_fd;
static void close_other_fd(void) { worker_result = close_fd(other_fd); }
static void open_slow_waiter(void) { waiter_fd = open_fd("/dev/slow"); }
/* Runs while the waiter for /dev/slow is between queueing and sleeping: the
 * close of /dev/slow ends, then a waiter for /dev/other (still closing)
 * queues and sleeps. */
static pthread_t slow_closer;
static void finish_slow_then_wait_for_other(void)
{
    open_gate(&slow, &slow.gate_close);
    pthread_join(slow_closer, NULL);
    stolen_other_fd = open_fd("/dev/other");
}

static void test_wakeup_not_taken_by_other_path(void)
{
    pthread_t other_closer;
    struct timed a;
    int done;

    other_fd = open_fd("/dev/other");
    other.gate_close = 1;
    pthread_create(&other_closer, NULL, (void *(*)(void *))close_other_fd, NULL);
    wait_in_gate(&other);
    slow_fd = open_fd("/dev/slow");
    slow.gate_close = 1;
    pthread_create(&slow_closer, NULL, (void *(*)(void *))close_slow_fd, NULL);
    wait_in_gate(&slow);

    test_preempt_on_release("dfs_mgr", finish_slow_then_wait_for_other);
    done = thread_run(&a, open_slow_waiter, 1000);
    /* /dev/slow is free: its waiter must not sleep until /dev/other is. */
    CHECK(done);
    open_gate(&other, &other.gate_close);
    pthread_join(other_closer, NULL);
    thread_join(&a);
    CHECK(test_worker_wait(2000));
    CHECK(waiter_fd >= 0 && stolen_other_fd >= 0);
    CHECK(close_fd(waiter_fd) == 0 && close_fd(stolen_other_fd) == 0);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
    memset(&other.opens, 0, sizeof(other) - offsetof(struct drv, opens));
}

/* --- A close waiting for opens is not starved by new ones --------------- */

static int keep_fd, o1_fd, o2_fd;
static void open_o1(void) { o1_fd = open_fd("/dev/slow"); }
static void open_o2(void) { o2_fd = open_fd("/dev/slow"); }
static void close_keep(void) { worker_result = close_fd(keep_fd); }

static void test_close_not_starved_by_opens(void)
{
    pthread_t o1;
    struct timed c, o2;

    keep_fd = open_fd("/dev/slow");
    slow.gate_later_open = 1;
    pthread_create(&o1, NULL, (void *(*)(void *))open_o1, NULL);
    wait_in_gate(&slow);
    CHECK(!thread_run(&c, close_keep, 100));        /* waits for o1 */
    /* A new open waits behind the close instead of joining o1. */
    CHECK(!thread_run(&o2, open_o2, 200));
    CHECK(slow.in_gate == 1);
    open_gate(&slow, &slow.gate_later_open);
    pthread_join(o1, NULL);
    thread_join(&c);
    thread_join(&o2);
    CHECK(worker_result == 0 && o1_fd >= 0 && o2_fd >= 0);
    CHECK(close_fd(o1_fd) == 0 && close_fd(o2_fd) == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- A backend's later open that reaches a close of its own path -------- */

static struct dfs_fd victim;
static int later_open_fd;
static void later_open_closing_victim(void) { later_open_fd = open_fd("/dev/slow"); }

static void test_close_inside_later_open(void)
{
    int first = open_fd("/dev/slow");
    struct timed t;

    memset(&victim, 0, sizeof(victim));
    victim.ref_count = 1;
    CHECK(dfs_file_open(&victim, "/dev/slow", O_RDWR) == 0);
    /* The later open's backend closes another descriptor of the path (as a
     * deferred close retried by fd_new() would): it would wait for its own
     * open. */
    slow.close_in_open = &victim;
    CHECK(thread_run(&t, later_open_closing_victim, 1000));
    thread_join(&t);
    CHECK(slow.close_in_open_result == -EDEADLK);
    CHECK(later_open_fd >= 0);
    CHECK(dfs_file_close(&victim) == 0);
    CHECK(close_fd(later_open_fd) == 0 && close_fd(first) == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* A later open's backend that opens its own path again while a close waits
 * for it: the nested open goes ahead (the close waits for the outer open
 * anyway) instead of failing with -EDEADLK. */
static struct dfs_fd nested_fd;

static void test_nested_open_while_close_waits(void)
{
    pthread_t o1;
    struct timed c;

    keep_fd = open_fd("/dev/slow");
    memset(&nested_fd, 0, sizeof(nested_fd));
    nested_fd.ref_count = 1;
    slow.gate_later_open = 1;
    slow.open_in_open = &nested_fd;
    pthread_create(&o1, NULL, (void *(*)(void *))open_o1, NULL);
    wait_in_gate(&slow);
    CHECK(!thread_run(&c, close_keep, 100));        /* waits for o1 */
    open_gate(&slow, &slow.gate_later_open);
    pthread_join(o1, NULL);
    thread_join(&c);
    CHECK(slow.open_in_open_result == 0 && o1_fd >= 0 && worker_result == 0);
    if (slow.open_in_open_result == 0)
        CHECK(dfs_file_close(&nested_fd) == 0);
    CHECK(close_fd(o1_fd) == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* close() of a descriptor number another thread is still opening: it used to
 * reach the descriptor with the fnode set and wait for the first open, which
 * then failed and freed the fnode under it. The number is not valid until the
 * open is done, so the close fails with EBADF. */
static int slot_fd, slot_open_result, slot_close_result;
static void open_into_slot(void)
{
    struct dfs_fd *d = fd_get(slot_fd);

    slot_open_result = dfs_file_open(d, "/dev/slow", O_RDWR);
    if (slot_open_result < 0)
        fd_release(slot_fd);
}
static void close_slot(void) { slot_close_result = close_fd(slot_fd); }

static void test_close_of_descriptor_being_opened(void)
{
    pthread_t t;
    struct timed c;
    int done;

    slot_fd = fd_new();
    slow.gate_open = 1;
    slow.open_result = -EIO;
    pthread_create(&t, NULL, (void *(*)(void *))open_into_slot, NULL);
    wait_in_gate(&slow);
    done = thread_run(&c, close_slot, 300);
    CHECK(done && slot_close_result == -EBADF);
    open_gate(&slow, &slow.gate_open);
    pthread_join(t, NULL);
    thread_join(&c);
    slow.open_result = 0;
    CHECK(slot_open_result == -EIO && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* dup2() onto, or dup() of, a descriptor another thread is still opening:
 * dup2 used to take it as the descriptor to replace and close it under the
 * open, and dup to hand out an alias before the open was done. Both are
 * refused, as is either on a slot fd_new() allocated whose open has not
 * started (no fnode yet). */
int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
static int dup2_result, dup_result;
static void dup2_onto_slot(void) { dup2_result = sys_dup2(other_fd, slot_fd); }

static void test_dup_of_descriptor_being_opened(void)
{
    pthread_t t;
    struct timed c;
    int fresh = fd_new();

    other_fd = open_fd("/dev/other");
    CHECK(sys_dup2(other_fd, fresh) < 0);    /* no fnode yet */
    CHECK(sys_dup(fresh) < 0);
    fd_release(fresh);

    slot_fd = fd_new();
    slow.gate_open = 1;
    slow.open_result = -EIO;
    pthread_create(&t, NULL, (void *(*)(void *))open_into_slot, NULL);
    wait_in_gate(&slow);
    CHECK(thread_run(&c, dup2_onto_slot, 300));
    CHECK(dup2_result < 0);
    dup_result = sys_dup(slot_fd);
    CHECK(dup_result < 0);
    open_gate(&slow, &slow.gate_open);
    pthread_join(t, NULL);
    thread_join(&c);
    slow.open_result = 0;
    CHECK(slot_open_result == -EIO && !path_is_open("/dev/slow"));
    CHECK(close_fd(other_fd) == 0);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
    memset(&other.opens, 0, sizeof(other) - offsetof(struct drv, opens));
}

/* Open flags from user space carrying DFS's own bits: DFS_F_CLOSING made
 * the descriptor unreachable, so it could never be closed. */
static void test_user_flags_cannot_set_internal_bits(void)
{
    int fd = fd_new();
    struct dfs_fd *d = fd_get(fd);

    CHECK(dfs_file_open(d, "/dev/slow", O_RDWR | DFS_F_CLOSING | DFS_F_CLOSE_TERMINAL) == 0);
    CHECK(!(d->flags & (DFS_F_CLOSING | DFS_F_CLOSE_TERMINAL | DFS_F_OPENING)));
    CHECK(close_fd(fd) == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- unlink()/rename() ask whether a path is open ------------------------ */

static int is_open_result;
static void ask_is_open(void) { is_open_result = dfs_file_is_open("/dev/slow"); }

static void test_is_open_waits_for_close(void)
{
    pthread_t t;

    slow_fd = open_fd("/dev/slow");
    slow.gate_close = 1;
    pthread_create(&t, NULL, (void *(*)(void *))close_slow_fd, NULL);
    wait_in_gate(&slow);
    /* Neither open nor closed yet: it waits, as it did on the fnode lock,
     * instead of reporting the path open (EBUSY to unlink). */
    test_run_on_worker(ask_is_open);
    CHECK(!test_worker_wait(200));
    open_gate(&slow, &slow.gate_close);
    pthread_join(t, NULL);
    CHECK(test_worker_wait(2000));
    CHECK(is_open_result == 0);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- A descriptor closed through another reference ---------------------- */

static void test_put_of_closed_descriptor(void)
{
    int fd = fd_new();
    struct dfs_fd *d = fd_get(fd);
    int errors = test_log_errors;

    /* closedir() and mkdir() close a descriptor that a dup() or an I/O pin
     * may still reference; its last put has nothing to close. It used to
     * fail with ENXIO and be retried, and logged, forever. */
    CHECK(d && d->fnode == NULL);
    d->ref_count++;
    fdt_fd_release(dfs_fdtable_get(), fd);
    fd_put(d);
    CHECK(test_log_errors == errors);
    fd = fd_new();
    CHECK(fd >= 0 && test_log_errors == errors);
    fd_release(fd);
}

/* closedir(), mkdir() and opendir() release the descriptor even when its
 * close failed and can be retried: the fnode stays hashed. fd_free() used to
 * free it anyway, and the next lookup of the path read freed memory. */
static void test_release_after_retryable_close(void)
{
    int fd = fd_new();
    struct dfs_fd *d = fd_get(fd);

    CHECK(dfs_file_open(d, "/dev/slow", O_RDWR) == 0);
    slow.close_result = -EAGAIN;
    CHECK(dfs_file_close(d) == -EAGAIN);
    fd_release(fd);
    CHECK(path_is_open("/dev/slow"));
    slow.close_result = 0;
    fd = fd_new();                  /* retries the close */
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    fd_release(fd);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- Later opens of one path run one at a time ------------------------- */

/* Drivers' open callbacks (tty_open(), rt_device_open()'s ref_count) assume
 * opens of their path do not overlap. */
static int later_a, later_b;
static void later_open_a(void) { later_a = open_fd("/dev/slow"); }
static void later_open_b(void) { later_b = open_fd("/dev/slow"); }

static void test_later_opens_serialized(void)
{
    struct timed a, b;
    int first = open_fd("/dev/slow");

    slow.gate_later_open = 1;
    CHECK(!thread_run(&a, later_open_a, 100));
    wait_in_gate(&slow);
    CHECK(!thread_run(&b, later_open_b, 200));      /* waits for a */
    open_gate(&slow, &slow.gate_later_open);
    thread_join(&a);
    thread_join(&b);
    CHECK(later_a >= 0 && later_b >= 0 && slow.later_overlap == 0);
    CHECK(close_fd(later_a) == 0 && close_fd(later_b) == 0 && close_fd(first) == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- A queued close retry does not wait for a busy path ---------------- */

/* fd_new() retries queued closes; one whose path has an open in flight is
 * requeued instead of stalling an open of an unrelated path. */
static int retry_new_fd;
static void new_unrelated_fd(void) { retry_new_fd = fd_new(); }

static void test_retry_skips_busy_path(void)
{
    struct timed opener, alloc;
    int fd = fd_new();
    struct dfs_fd *d = fd_get(fd);

    CHECK(dfs_file_open(d, "/dev/slow", O_RDWR) == 0);
    slow.close_result = -EAGAIN;
    CHECK(dfs_file_close(d) == -EAGAIN);
    fd_release(fd);                 /* queued for a retry */
    /* The opener's own fd_new() retries the close too: keep it failing
     * until the opener sits in a later open of the path. */
    slow.gate_later_open = 1;
    CHECK(!thread_run(&opener, later_open_a, 100));
    wait_in_gate(&slow);
    slow.close_result = 0;
    CHECK(thread_run(&alloc, new_unrelated_fd, 1000));  /* does not wait */
    thread_join(&alloc);
    CHECK(retry_new_fd >= 0 && slow.closes == 0);
    fd_release(retry_new_fd);
    open_gate(&slow, &slow.gate_later_open);
    thread_join(&opener);
    CHECK(later_a >= 0);
    fd = fd_new();                  /* the retry goes through now */
    CHECK(slow.closes == 1 && slow.last_closes == 0);
    fd_release(fd);
    CHECK(close_fd(later_a) == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- Descriptors that never reach fd_free() ---------------------------- */

static void test_stack_descriptor_close(void)
{
    int fd = open_fd("/dev/slow");
    struct dfs_fd stack;

    /* dfs_fs.c and netmgmt.c open and close a struct dfs_fd of their own. */
    memset(&stack, 0, sizeof(stack));
    stack.ref_count = 1;
    CHECK(dfs_file_open(&stack, "/dev/slow", O_RDWR) == 0);
    CHECK(dfs_file_close(&stack) == 0);
    CHECK(close_fd(fd) == 0);
    /* The table descriptor's close is the last one. */
    CHECK(slow.last_closes == 1);
    CHECK(!path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- Close failures ---------------------------------------------------- */

static void test_close_failures(void)
{
    int a = open_fd("/dev/slow"), b = open_fd("/dev/slow");

    /* Retryable: the descriptor and its reference stay, and the path is
     * not left busy. */
    slow.close_result = -EAGAIN;
    CHECK(close_fd(a) == -EAGAIN);
    slow.close_result = 0;
    CHECK(close_fd(b) == 0);
    CHECK(slow.last_closes == 0);
    CHECK(close_fd(a) == 0);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));

    /* Terminal: the reference goes with it. */
    a = open_fd("/dev/slow");
    b = open_fd("/dev/slow");
    slow.close_result = -EIO;
    slow.close_terminal = 1;
    CHECK(close_fd(a) == -EIO);
    slow.close_result = 0;
    slow.close_terminal = 0;
    CHECK(close_fd(b) == 0);
    CHECK(slow.last_closes == 2 && !path_is_open("/dev/slow"));
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
}

/* --- A close that opens its own path ------------------------------------ */

static void test_own_path_from_close(void)
{
    int fd = open_fd("/dev/slow");

    slow.reopen_path = "/dev/slow";
    CHECK(close_fd(fd) == 0);
    CHECK(slow.reopen_result == -EDEADLK);
    CHECK(slow.last_closes == 1 && !path_is_open("/dev/slow"));

    /* Another path is fine. */
    fd = open_fd("/dev/slow");
    slow.reopen_path = "/dev/other";
    CHECK(close_fd(fd) == 0);
    CHECK(slow.reopen_result == 0);
    memset(&slow.opens, 0, sizeof(slow) - offsetof(struct drv, opens));
    memset(&other.opens, 0, sizeof(other) - offsetof(struct drv, opens));
}

static void setup(struct drv *d, const char *name)
{
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    d->dev.fops = &drv_fops;
    test_register_device(&d->dev, name);
}

int main(int argc, char **argv)
{
    test_sched_init();
    dfs_init();
    setup(&slow, "slow");
    setup(&other, "other");

    const char *only = argc > 1 ? argv[1] : NULL;
#define RUN(name) do { if (!only || strcmp(only, #name) == 0) { \
        unsigned before = failures; test_##name(); \
        printf("%s %s\n", failures == before ? "PASS" : "FAIL", #name); \
        fflush(stdout); } } while (0)
    RUN(two_closes_of_one_path);
    RUN(sleeping_close_leaves_other_paths_alone);
    RUN(open_waits_for_last_close);
    RUN(open_waits_for_first_open);
    RUN(failed_first_open_wakes_waiter);
    RUN(close_waits_for_later_open);
    RUN(wakeup_not_taken_by_other_path);
    RUN(close_not_starved_by_opens);
    RUN(close_inside_later_open);
    RUN(nested_open_while_close_waits);
    RUN(release_after_retryable_close);
    RUN(close_of_descriptor_being_opened);
    RUN(dup_of_descriptor_being_opened);
    RUN(user_flags_cannot_set_internal_bits);
    RUN(is_open_waits_for_close);
    RUN(put_of_closed_descriptor);
    RUN(stack_descriptor_close);
    RUN(close_failures);
    RUN(own_path_from_close);
    RUN(later_opens_serialized);
    RUN(retry_skips_busy_path);
    if (failures) {
        fprintf(stderr, "%u check(s) failed\n", failures);
        _exit(1);
    }
    _exit(0);
}
"""


def build(directory, dfs_root=DFS):
    build_dir = Path(directory)
    (build_dir / "rtconfig.h").write_text(RTCONFIG)
    (build_dir / "stubs.h").write_text(STUBS)
    for name in ("rtthread.h", "rthw.h", "rtdbg.h", "rtdevice.h", "dfs_poll.h"):
        (build_dir / name).write_text('#include "stubs.h"\n')
    (build_dir / "dfs_posix.h").write_text('#include <dfs_file.h>\n')
    (build_dir / "env.c").write_text(ENV)
    (build_dir / "test.c").write_text(CASES)
    sources = [dfs_root / "src/dfs.c", dfs_root / "src/dfs_file.c",
               dfs_root / "filesystems/devfs/devfs.c", build_dir / "env.c", build_dir / "test.c"]
    cc = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Werror", "-g",
          "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
          "-fsanitize=undefined,address", "-fno-sanitize-recover=all", "-pthread",
          "-I", str(build_dir), "-I", str(dfs_root / "include"),
          "-I", str(dfs_root / "filesystems/devfs")]
    binary = build_dir / "test"
    subprocess.run([*cc, *map(str, sources), "-o", str(binary)], check=True)
    return binary


def main():
    # An optional argument names another dfs tree (for example a copy of the
    # code before a fix); a second one runs a single case.
    dfs_root = Path(sys.argv[1]) if len(sys.argv) > 1 else DFS
    only = sys.argv[2:3]
    with tempfile.TemporaryDirectory(prefix="k230-dfs-close-test-") as directory:
        binary = build(directory, dfs_root)
        result = subprocess.run([str(binary), *only], timeout=60)
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()
