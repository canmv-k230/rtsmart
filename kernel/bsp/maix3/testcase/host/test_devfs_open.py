#!/usr/bin/env python3
"""Check the errno that open() of a /dev node reports.

Compiles the production rt-thread/src/device.c, dfs/src/dfs_file.c,
dfs/filesystems/devfs/devfs.c and, as a driver whose fops->open wraps
rt_device_open(), drivers/bluetooth/bt_hci.c unmodified against RT-Thread
stubs and opens
devices through dfs_file_open(), on the first open of a path (devfs opens the
driver) and on later opens (dfs_file_open() calls the driver's fops->open
directly). The RT_E* codes and device flags are taken from rtdef.h.

- dfs_file_ops.open speaks POSIX: its errno reaches user space, -ENXIO
  included; codes that are not an errno (MPP K_ERR_*) become EIO.
- rt_device_ops open/init speak RT-Thread codes, which devfs translates;
  -RT_ENOSYS from the open op is still a successful open.
Run with python3 -B kernel/bsp/maix3/testcase/host/test_devfs_open.py.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

BSP = Path(__file__).resolve().parents[2]
RTT = BSP.parents[1] / "rt-thread"
DFS = RTT / "components/dfs"
DRIVERS = RTT / "components/drivers"


def rtdef_defines():
    """RT_E* codes and device flags, so the stubs cannot drift from rtdef.h."""
    text = (RTT / "include/rtdef.h").read_text()
    names = re.findall(r"^#define\s+(RT_E\w+|RT_DEVICE_O?FLAG_\w+)\s+(-?(?:0x)?[0-9a-fA-F]+)\b",
                       text, re.M)
    assert any(n == "RT_ENOSYS" for n, _ in names) and any(n == "RT_DEVICE_FLAG_ACTIVATED" for n, _ in names)
    return "".join(f"#define {name} {value}\n" for name, value in names)


RTCONFIG = r"""
#pragma once
#define RT_NAME_MAX 8
#define RT_USING_DEVICE
#define RT_USING_DEVICE_OPS
#define RT_USING_POSIX
#define RT_USING_DFS
#define RT_USING_DFS_DEVFS
#define RT_USING_BT_HCI
"""

STUBS = r"""
#pragma once
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "rtconfig.h"
#include "rtdef_values.h"

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
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_PRIO 0
#define rt_inline static inline
#define RTM_EXPORT(symbol)
#define RT_ASSERT(x) assert(x)
#define LOG_D(...) ((void)0)
#define LOG_I(...) ((void)0)
#define LOG_W(...) ((void)0)
#define LOG_E(...) ((void)0)
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
void rt_object_init(struct rt_object *object, enum rt_object_class_type type, const char *name);
void rt_object_detach(rt_object_t object);
static inline rt_uint8_t rt_object_get_type(rt_object_t object) { return object->type & ~RT_Object_Class_Static; }
static inline rt_bool_t rt_object_is_systemobject(rt_object_t object) { return 1; }

struct rt_wqueue { rt_uint32_t flag; rt_list_t waiting_list; };
typedef struct rt_wqueue rt_wqueue_t;
static inline void rt_wqueue_init(struct rt_wqueue *q) { q->flag = 0; rt_list_init(&q->waiting_list); }
static inline void rt_wqueue_wakeup(rt_wqueue_t *q, void *key) {}
struct rt_pollreq;
static inline void rt_poll_add(rt_wqueue_t *q, struct rt_pollreq *req) {}

/* Byte ring, enough for bt_hci.c. */
struct rt_ringbuffer { rt_uint8_t *buf; rt_uint16_t size, len; };
static inline void rt_ringbuffer_init(struct rt_ringbuffer *rb, rt_uint8_t *pool, rt_int16_t size)
{
    rb->buf = pool; rb->size = size; rb->len = 0;
}
static inline void rt_ringbuffer_reset(struct rt_ringbuffer *rb) { rb->len = 0; }
static inline rt_size_t rt_ringbuffer_data_len(struct rt_ringbuffer *rb) { return rb->len; }
static inline rt_size_t rt_ringbuffer_space_len(struct rt_ringbuffer *rb) { return rb->size - rb->len; }
static inline rt_size_t rt_ringbuffer_put(struct rt_ringbuffer *rb, const rt_uint8_t *p, rt_uint16_t n)
{
    if (n > rb->size - rb->len) n = rb->size - rb->len;
    memcpy(rb->buf + rb->len, p, n); rb->len += n; return n;
}
static inline rt_size_t rt_ringbuffer_putchar(struct rt_ringbuffer *rb, const rt_uint8_t ch)
{
    return rt_ringbuffer_put(rb, &ch, 1);
}
static inline rt_size_t rt_ringbuffer_get(struct rt_ringbuffer *rb, rt_uint8_t *p, rt_uint16_t n)
{
    if (n > rb->len) n = rb->len;
    memcpy(p, rb->buf, n); memmove(rb->buf, rb->buf + n, rb->len - n); rb->len -= n; return n;
}
typedef int rt_spinlock_t;
static inline void rt_spin_lock_init(rt_spinlock_t *l) { *l = 0; }
static inline rt_base_t rt_spin_lock_irqsave(rt_spinlock_t *l) { assert(!*l); *l = 1; return 0; }
static inline void rt_spin_unlock_irqrestore(rt_spinlock_t *l, rt_base_t level) { assert(*l); *l = 0; }

enum rt_device_class_type {
    RT_Device_Class_Char = 0, RT_Device_Class_Block, RT_Device_Class_Pipe = 16,
    RT_Device_Class_Bus = 31,
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
    rt_err_t (*rx_indicate)(rt_device_t dev, rt_size_t size);
    rt_err_t (*tx_complete)(rt_device_t dev, void *buffer);
    const struct rt_device_ops *ops;
    const struct dfs_file_ops *fops;
    struct rt_wqueue wait_queue;
    void *user_data;
};
rt_device_t rt_device_find(const char *name);
rt_err_t rt_device_register(rt_device_t dev, const char *name, rt_uint16_t flags);
rt_err_t rt_device_open(rt_device_t dev, rt_uint16_t oflag);
rt_err_t rt_device_close(rt_device_t dev);
rt_size_t rt_device_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);
rt_size_t rt_device_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size);
rt_err_t rt_device_control(rt_device_t dev, int cmd, void *arg);
rt_err_t rt_device_set_rx_indicate(rt_device_t dev, rt_err_t (*rx_ind)(rt_device_t dev, rt_size_t size));

struct rt_mutex { int held; };
static inline rt_err_t rt_mutex_init(struct rt_mutex *m, const char *n, int f) { m->held = 0; return RT_EOK; }
static inline rt_err_t rt_mutex_take(struct rt_mutex *m, int t) { assert(!m->held); m->held = 1; return RT_EOK; }
static inline rt_err_t rt_mutex_release(struct rt_mutex *m) { assert(m->held); m->held = 0; return RT_EOK; }
static inline rt_err_t rt_mutex_detach(struct rt_mutex *m) { assert(!m->held); return RT_EOK; }
/* One thread: nothing ever waits for an fnode (test_dfs_close.py does). */
struct rt_semaphore { unsigned value; };
typedef struct rt_semaphore *rt_sem_t;
static inline rt_err_t rt_sem_init(struct rt_semaphore *s, const char *n, rt_uint32_t v, rt_uint8_t f)
{
    s->value = v; return RT_EOK;
}
static inline rt_err_t rt_sem_take(struct rt_semaphore *s, int t) { assert(s->value); s->value--; return RT_EOK; }
#define rt_sem_take_interruptible rt_sem_take
static inline rt_err_t rt_sem_release(struct rt_semaphore *s) { s->value++; return RT_EOK; }
static inline rt_err_t rt_sem_detach(struct rt_semaphore *s) { return RT_EOK; }
typedef struct rt_thread *rt_thread_t;
static inline rt_thread_t rt_thread_self(void) { return RT_NULL; }
static inline void rt_enter_critical(void) {}
static inline void rt_exit_critical(void) {}
static inline rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static inline void rt_hw_interrupt_enable(rt_base_t level) {}
extern rt_err_t test_errno;
static inline void rt_set_errno(rt_err_t e) { test_errno = e; }
static inline rt_err_t rt_get_errno(void) { return test_errno; }
#define rt_malloc malloc
#define rt_calloc calloc
#define rt_free free
#define rt_strdup strdup
#define rt_strcmp strcmp
#define rt_strncmp strncmp
#define rt_strncpy strncpy
#define rt_memset memset
#define rt_snprintf snprintf
#define rt_kprintf(...) ((void)0)
"""

ENV = r"""
#include <rtthread.h>
#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>

rt_err_t test_errno;
/* dfs.c's descriptor lock (dfs_file_open() hides a descriptor while it
 * opens); one thread here. */
void dfs_fd_lock(void) {}
void dfs_fd_unlock(void) {}
static struct rt_object_information devices = {
    RT_Object_Class_Device, { &devices.object_list, &devices.object_list }, 0,
};
struct rt_object_information *rt_object_get_information(enum rt_object_class_type type)
{
    return type == RT_Object_Class_Device ? &devices : RT_NULL;
}
void rt_object_init(struct rt_object *object, enum rt_object_class_type type, const char *name)
{
    strncpy(object->name, name, RT_NAME_MAX);
    object->type = type | RT_Object_Class_Static;
    rt_list_insert_after(&devices.object_list, &object->list);
}
void rt_object_detach(rt_object_t object) { rt_list_remove(&object->list); }

/* One file system: devfs on /dev. */
static struct dfs_filesystem devfs;
int dfs_register(const struct dfs_filesystem_ops *ops)
{
    devfs.path = "/dev";
    devfs.ops = ops;
    return 0;
}
struct dfs_filesystem *dfs_filesystem_lookup(const char *path)
{
    return strncmp(path, "/dev", 4) == 0 ? &devfs : RT_NULL;
}
char *dfs_normalize_path(const char *directory, const char *filename) { return strdup(filename); }
const char *dfs_subdir(const char *directory, const char *filename)
{
    const char *dir = filename + strlen(directory);
    return *dir ? dir : RT_NULL;
}
"""

CASES = r"""
#include <rtthread.h>
#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>
#include "devfs.h"
#include <drivers/bt_hci.h>

/* An MPP code: K_DEF_ERR(K_ID_VB, K_ERR_LEVEL_ERROR, K_ERR_NOMEM). */
#define K_ERR_VB_NOMEM ((int)(0x80000000U | 0x20000000U | (6U << 16) | (2U << 13) | 12U))

static unsigned failures;
#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* A POSIX-speaking dfs_file_ops driver: open returns fops_result, and counts
 * how many opens it accepted. */
static int fops_result, fops_opens;
static int posix_open(struct dfs_fd *fd) { if (fops_result >= 0) fops_opens++; return fops_result; }
static int posix_close(struct dfs_fd *fd) { fops_opens--; return 0; }
static const struct dfs_file_ops posix_fops = { .open = posix_open, .close = posix_close };

/* rt_device_ops drivers: init and open return what the test sets. */
static rt_err_t init_result, open_result;
static rt_err_t rt_init(rt_device_t dev) { return init_result; }
static rt_err_t rt_open(rt_device_t dev, rt_uint16_t oflag) { return open_result; }
static rt_err_t rt_close(rt_device_t dev) { return RT_EOK; }
static const struct rt_device_ops rt_ops = { .init = rt_init, .open = rt_open, .close = rt_close };
static const struct rt_device_ops no_open_ops = { .close = rt_close };

static struct rt_device posix_dev, rt_dev, no_open_dev;

static struct dfs_fd *open_dev_flags(const char *path, int flags, int *result)
{
    struct dfs_fd *fd = calloc(1, sizeof(*fd));
    fd->ref_count = 1;
    *result = dfs_file_open(fd, path, flags);
    if (*result != 0) {
        CHECK(fd->fnode == RT_NULL);
        free(fd);
        return RT_NULL;
    }
    return fd;
}

static struct dfs_fd *open_dev(const char *path, int *result)
{
    return open_dev_flags(path, O_RDWR, result);
}

static void close_dev(struct dfs_fd *fd)
{
    CHECK(dfs_file_close(fd) == 0);
    /* fd_free()'s share of the fnode reference. */
    if (fd->fnode)
        fd->fnode->ref_count--;
    free(fd);
}

static int open_once(const char *path)
{
    int result;
    struct dfs_fd *fd = open_dev(path, &result);
    if (fd)
        close_dev(fd);
    return result;
}

static void test_fops_first_open(void)
{
    /* POSIX errors from the driver reach user space unchanged. */
    fops_result = -ENODEV;
    CHECK(open_once("/dev/posix") == -ENODEV);
    fops_result = -EBUSY;
    CHECK(open_once("/dev/posix") == -EBUSY);
    fops_result = -ETIMEDOUT;
    CHECK(open_once("/dev/posix") == -ETIMEDOUT);
    /* -ENXIO is -RT_ENOSYS's number, but fops->open speaks POSIX: a failure. */
    fops_result = -ENXIO;
    CHECK(open_once("/dev/posix") == -ENXIO);
    CHECK(fops_opens == 0);
    /* Not an errno: user space would take it for a successful return. */
    fops_result = K_ERR_VB_NOMEM;
    CHECK(open_once("/dev/posix") == -EIO);
    fops_result = 0;
    CHECK(open_once("/dev/posix") == 0);
    CHECK(fops_opens == 0);
}

static void test_fops_cached_open(void)
{
    int result;
    struct dfs_fd *first;

    fops_result = 0;
    first = open_dev("/dev/posix", &result);
    CHECK(first && result == 0);

    /* Later opens of the open path call fops->open directly: same rules. */
    fops_result = -EBUSY;
    CHECK(open_dev("/dev/posix", &result) == RT_NULL && result == -EBUSY);
    fops_result = -ENXIO;
    CHECK(open_dev("/dev/posix", &result) == RT_NULL && result == -ENXIO);
    fops_result = K_ERR_VB_NOMEM;
    CHECK(open_dev("/dev/posix", &result) == RT_NULL && result == -EIO);
    CHECK(first->fnode->ref_count == 1);

    fops_result = 0;
    struct dfs_fd *second = open_dev("/dev/posix", &result);
    CHECK(second && result == 0 && fops_opens == 2);
    close_dev(second);
    close_dev(first);
    CHECK(fops_opens == 0);
}

static void test_rt_device_open(void)
{
    /* No open op: rt_device_open() succeeds. */
    CHECK(open_once("/dev/noopen") == 0);
    CHECK(no_open_dev.ref_count == 0);

    /* -RT_ENOSYS from the open op: rt_device_open() counts the device as
     * opened, so the open succeeds and close drops that reference. */
    init_result = RT_EOK;
    open_result = -RT_ENOSYS;
    int result;
    struct dfs_fd *fd = open_dev("/dev/rtdev", &result);
    CHECK(fd && result == 0 && rt_dev.ref_count == 1);
    if (fd)
        close_dev(fd);
    CHECK(rt_dev.ref_count == 0);

    /* RT-Thread codes are translated: their numbers mean other errnos. */
    open_result = -RT_EBUSY;     /* -E2BIG's number */
    CHECK(open_once("/dev/rtdev") == -EBUSY);
    open_result = -RT_ENOMEM;    /* -EIO's number */
    CHECK(open_once("/dev/rtdev") == -ENOMEM);
    open_result = -RT_ETIMEOUT;  /* -ENOENT's number */
    CHECK(open_once("/dev/rtdev") == -ETIMEDOUT);
    open_result = -RT_EINVAL;    /* -ECHILD's number */
    CHECK(open_once("/dev/rtdev") == -EINVAL);
    open_result = -RT_ERROR;     /* -EPERM's number */
    CHECK(open_once("/dev/rtdev") == -EIO);
    /* A POSIX errno past RT-Thread's range passes; anything else is EIO. */
    open_result = -ENODEV;
    CHECK(open_once("/dev/rtdev") == -ENODEV);
    open_result = K_ERR_VB_NOMEM;
    CHECK(open_once("/dev/rtdev") == -EIO);
    open_result = RT_ERROR;
    CHECK(open_once("/dev/rtdev") == -EIO);
    CHECK(rt_dev.ref_count == 0);

    /* -RT_ENOSYS from init is a failure: no reference, device inactive. */
    rt_dev.flag &= ~RT_DEVICE_FLAG_ACTIVATED;
    init_result = -RT_ENOSYS;
    open_result = RT_EOK;
    CHECK(open_once("/dev/rtdev") == -ENODEV);
    CHECK(rt_dev.ref_count == 0 && !(rt_dev.flag & RT_DEVICE_FLAG_ACTIVATED));
    init_result = RT_EOK;
    CHECK(open_once("/dev/rtdev") == 0);
    CHECK(rt_dev.ref_count == 0);
}

/* fops->open callbacks that wrap rt_device_open() use devfs_device_open(). */
static struct rt_device standalone_dev;
static int wrapper_open(struct dfs_fd *fd)
{
    return devfs_device_open((rt_device_t)fd->fnode->data, RT_DEVICE_OFLAG_RDWR);
}
static int wrapper_close(struct dfs_fd *fd)
{
    rt_device_close((rt_device_t)fd->fnode->data);
    return 0;
}
static const struct dfs_file_ops wrapper_fops = { .open = wrapper_open, .close = wrapper_close };

static void test_device_open_helper(void)
{
    int result;
    struct dfs_fd *first;

    standalone_dev.ops = &no_open_ops;
    CHECK(rt_device_register(&standalone_dev, "timer", RT_DEVICE_FLAG_RDWR |
                             RT_DEVICE_FLAG_STANDALONE) == RT_EOK);
    standalone_dev.fops = &wrapper_fops;
    first = open_dev("/dev/timer", &result);
    CHECK(first && result == 0 && standalone_dev.ref_count == 1);
    /* rt_device_open() refuses a second open of a standalone device with
     * -RT_EBUSY, which user space must see as EBUSY, not E2BIG. */
    CHECK(open_dev("/dev/timer", &result) == RT_NULL && result == -EBUSY);
    close_dev(first);
    CHECK(standalone_dev.ref_count == 0);

    rt_dev.flag &= ~RT_DEVICE_FLAG_ACTIVATED;
    init_result = RT_EOK;
    open_result = -RT_ENOSYS;
    CHECK(devfs_device_open(&rt_dev, RT_DEVICE_OFLAG_RDWR) == 0 && rt_dev.ref_count == 1);
    CHECK(rt_device_close(&rt_dev) == RT_EOK);
    open_result = -RT_EBUSY;
    CHECK(devfs_device_open(&rt_dev, RT_DEVICE_OFLAG_RDWR) == -EBUSY && rt_dev.ref_count == 0);
}

/* bt_hci.c: a real fops->open around rt_device_open() of a standalone
 * device whose open op reports RT_E* codes. */
static rt_err_t hci_result;
static rt_err_t hci_open(struct rt_bt_hci_device *hci) { return hci_result; }
static rt_err_t hci_send(struct rt_bt_hci_device *hci, rt_uint8_t type, const rt_uint8_t *data,
                         rt_size_t length) { return RT_EOK; }
static const struct rt_bt_hci_ops hci_ops = { .open = hci_open, .send = hci_send };
static struct rt_bt_hci_device hci;
static rt_uint8_t hci_rx[64];

static void test_bt_hci(void)
{
    int result;
    struct dfs_fd *first;

    CHECK(rt_bt_hci_register(&hci, "hci0", &hci_ops, hci_rx, sizeof(hci_rx), RT_NULL) == RT_EOK);

    /* The transport's -RT_EIO (-ENOEXEC's number) on first and later opens. */
    hci_result = -RT_EIO;
    CHECK(open_once("/dev/hci0") == -EIO);
    CHECK(hci.parent.rx_indicate == RT_NULL && hci.parent.ref_count == 0);
    /* bt_hci_open() turns the transport's -RT_ENOSYS into a failure. */
    hci_result = -RT_ENOSYS;
    CHECK(open_once("/dev/hci0") == -EIO);
    /* O_ACCMODE 3 is not an access mode. */
    CHECK(open_dev_flags("/dev/hci0", O_ACCMODE, &result) == RT_NULL && result == -EINVAL);

    hci_result = RT_EOK;
    first = open_dev("/dev/hci0", &result);
    CHECK(first && result == 0 && hci.parent.ref_count == 1);
    /* Standalone: a second open is -RT_EBUSY (-E2BIG's number). */
    CHECK(open_dev("/dev/hci0", &result) == RT_NULL && result == -EBUSY);
    /* The refused open leaves the first file's rx hook alone. */
    CHECK(hci.parent.rx_indicate != RT_NULL);
    close_dev(first);
    CHECK(hci.parent.rx_indicate == RT_NULL);

    /* A writer gets no rx hook. */
    first = open_dev_flags("/dev/hci0", O_WRONLY, &result);
    CHECK(first && result == 0 && hci.parent.rx_indicate == RT_NULL);
    close_dev(first);
    CHECK(hci.parent.ref_count == 0);
}

int main(void)
{
    dfs_fnode_mgr_init();
    devfs_init();

    CHECK(rt_device_register(&posix_dev, "posix", RT_DEVICE_FLAG_RDWR) == RT_EOK);
    posix_dev.fops = &posix_fops;
    CHECK(rt_device_register(&rt_dev, "rtdev", RT_DEVICE_FLAG_RDWR) == RT_EOK);
    rt_dev.ops = &rt_ops;
    CHECK(rt_device_register(&no_open_dev, "noopen", RT_DEVICE_FLAG_RDWR) == RT_EOK);
    no_open_dev.ops = &no_open_ops;

    test_fops_first_open();
    test_fops_cached_open();
    test_rt_device_open();
    test_device_open_helper();
    test_bt_hci();
    if (failures) {
        fprintf(stderr, "%u check(s) failed\n", failures);
        return 1;
    }
    puts("PASS devfs open: fops POSIX errno on first and later opens, K_ERR_* as EIO");
    puts("PASS devfs open: rt_device_open() codes translated, -RT_ENOSYS open op still opens");
    puts("PASS devfs_device_open(): standalone second open is EBUSY");
    puts("PASS bt_hci fops->open: RT_E* from rt_device_open() as errno, EINVAL for a bad mode");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="k230-devfs-test-") as directory:
        build = Path(directory)
        (build / "rtconfig.h").write_text(RTCONFIG)
        (build / "rtdef_values.h").write_text(rtdef_defines())
        (build / "stubs.h").write_text(STUBS)
        for name in ("rtthread.h", "rthw.h", "rtdbg.h", "ipc/ringbuffer.h"):
            (build / name).parent.mkdir(exist_ok=True)
            (build / name).write_text('#include "stubs.h"\n')
        (build / "rtdevice.h").write_text('#include "stubs.h"\n#include <drivers/bt_hci.h>\n')
        (build / "dfs_poll.h").write_text('#include "stubs.h"\n')
        (build / "dfs_posix.h").write_text('#include <dfs_file.h>\n')
        (build / "env.c").write_text(ENV)
        (build / "test.c").write_text(CASES)
        sources = [RTT / "src/device.c", DFS / "src/dfs_file.c",
                   DFS / "filesystems/devfs/devfs.c", DRIVERS / "bluetooth/bt_hci.c",
                   build / "env.c", build / "test.c"]
        cc = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Werror",
              "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
              "-fsanitize=undefined,address", "-fno-sanitize-recover=all",
              "-I", str(build), "-I", str(DFS / "include"), "-I", str(DFS / "filesystems/devfs"),
              "-I", str(DRIVERS / "include")]
        binary = build / "test"
        subprocess.run([*cc, *map(str, sources), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
