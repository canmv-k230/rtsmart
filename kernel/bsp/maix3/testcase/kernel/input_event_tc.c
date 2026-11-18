#include <dfs_poll.h>
#include <dfs_posix.h>
#include <errno.h>
#include <rthw.h>
#include "rt_input_event.h"
#include "utest.h"

static struct rt_input_dev input = { .id = -1 };
static struct rt_semaphore done;
static const struct rt_input_info mouse = { .kind = RT_INPUT_KIND_MOUSE, .rel_bits = 3, .name = "test-mouse" };
static const struct rt_input_info touch = { .kind = RT_INPUT_KIND_TOUCH, .abs_bits = 3, .name = "test-touch" };

struct waiter {
    int fd;
    rt_bool_t poller;
    int result;
};
static struct waiter waiters[4];

static void wait_for_unplug(void *arg)
{
    struct waiter *w = arg;
    if (w->poller) {
        struct pollfd p = { .fd = w->fd, .events = POLLIN };
        w->result = poll(&p, 1, 5000) == 1 ? p.revents : 0;
    } else {
        struct input_event ev;
        w->result = read(w->fd, &ev, sizeof(ev)) < 0 ? rt_get_errno() : 0;
    }
    rt_sem_release(&done);
}

static rt_bool_t disconnect_waiters(int fd)
{
    unsigned started = 0, queued = 0;
    rt_sem_init(&done, "in-done", 0, RT_IPC_FLAG_FIFO);
    for (; started < sizeof(waiters) / sizeof(waiters[0]); ++started) {
        struct waiter *w = &waiters[started];
        rt_thread_t t;
        w->fd = fd;
        w->poller = started >= 2;
        w->result = 0;
        t = rt_thread_create("in-wait", wait_for_unplug, w, 4096, 15, 10);
        uassert_not_null(t);
        if (!t)
            break;
        rt_thread_startup(t);
    }
    /* Observe actual registrations instead of assuming a delay was enough. */
    for (int i = 0; i < 1000; ++i) {
        rt_base_t level = rt_hw_interrupt_disable();
        queued = rt_list_len(&input.parent.wait_queue.waiting_list);
        rt_hw_interrupt_enable(level);
        if (queued == started)
            break;
        rt_thread_mdelay(1);
    }
    uassert_int_equal(queued, 4);
    uassert_int_equal(rt_input_dev_disconnect(&input), RT_EOK);
    for (unsigned i = 0; i < started; ++i) {
        rt_err_t ret = rt_sem_take(&done, RT_TICK_PER_SECOND);
        uassert_int_equal(ret, RT_EOK);
        if (ret != RT_EOK) {
            /* Also release a stranded worker when testing a broken kernel. */
            rt_wqueue_wakeup_all(&input.parent.wait_queue, (void *)(POLLHUP | POLLERR));
            ret = rt_sem_take(&done, RT_TICK_PER_SECOND);
            uassert_int_equal(ret, RT_EOK);
            if (ret != RT_EOK)
                return RT_FALSE; /* Keep the fixture alive for this worker. */
        }
    }
    rt_sem_detach(&done);
    for (unsigned i = 0; i < started; ++i)
        uassert_int_equal(waiters[i].result, waiters[i].poller ? (POLLHUP | POLLERR) : -ENODEV);
    return RT_TRUE;
}

static void alternate_info(void *unused)
{
    for (int i = 0; i < 32; ++i) {
        rt_input_dev_disconnect(&input);
        rt_thread_delay(2);
        rt_input_dev_reconnect(&input, i % 2 ? &mouse : &touch);
        rt_thread_delay(2);
    }
    rt_sem_release(&done);
}

static void check_snapshots(int fd)
{
    struct rt_input_info info;
    unsigned seen_mouse = 0, seen_touch = 0, absent = 0, bad = 0;
    rt_thread_t t = rt_thread_create("in-cycle", alternate_info, RT_NULL, 4096, 15, 10);
    uassert_not_null(t);
    if (!t)
        return;
    rt_sem_init(&done, "in-cycle", 0, RT_IPC_FLAG_FIFO);
    rt_thread_startup(t);
    while (rt_sem_trytake(&done) != RT_EOK) {
        for (int i = 0; i < 32; ++i) {
            if (ioctl(fd, RT_INPUT_CTRL_GET_INFO, &info) < 0) {
                if (rt_get_errno() == -ENODEV)
                    absent++;
                else
                    bad++;
            } else if (!rt_memcmp(&info, &mouse, sizeof(info))) {
                seen_mouse++;
            } else if (!rt_memcmp(&info, &touch, sizeof(info))) {
                seen_touch++;
            } else {
                bad++;
            }
        }
        rt_thread_delay(1);
    }
    rt_sem_detach(&done);
    uassert_true(seen_mouse && seen_touch && absent);
    uassert_int_equal(bad, 0);
}

static void input_lifecycle(void)
{
    struct rt_input_info info;
    struct input_event ev[2];
    char path[RT_NAME_MAX + 6];
    int fd, second;
    rt_err_t ret;
    uassert_int_equal(input.id, -1);
    if (input.id >= 0)
        return;
    ret = rt_input_dev_register(&input, &mouse);
    uassert_int_equal(ret, RT_EOK);
    if (ret != RT_EOK)
        return;
    rt_snprintf(path, sizeof(path), "/dev/input/event%d", input.id);
    fd = open(path, O_RDONLY);
    uassert_true(fd >= 0);
    if (fd >= 0) {
        uassert_int_equal(rt_input_dev_unregister(&input), -RT_EBUSY);
        uassert_int_equal(ioctl(fd, RT_INPUT_CTRL_GET_INFO, &info), 0);
        uassert_buf_equal(&info, &mouse, sizeof(info));
        uassert_int_equal(rt_device_control(&input.parent, RT_INPUT_CTRL_GET_INFO, RT_NULL), -EINVAL);
        uassert_int_equal(ioctl(fd, RT_INPUT_CTRL_GET_INFO, RT_NULL), -1);
        uassert_int_equal(rt_get_errno(), -EINVAL);
        uassert_int_equal(ioctl(fd, RT_INPUT_CTRL_GET_INFO + 1, &info), -1);
        uassert_int_equal(rt_get_errno(), -EINVAL);
        if (!disconnect_waiters(fd))
            return;
        uassert_int_equal(rt_device_control(&input.parent, RT_INPUT_CTRL_GET_INFO, &info), -ENODEV);
        uassert_int_equal(ioctl(fd, RT_INPUT_CTRL_GET_INFO, &info), -1);
        uassert_int_equal(rt_get_errno(), -ENODEV);
        second = open(path, O_RDONLY);
        uassert_int_equal(second, -1);
        if (second >= 0)
            close(second);
        uassert_int_equal(rt_input_dev_unregister(&input), -RT_EBUSY);
        uassert_int_equal(rt_input_dev_reconnect(&input, &touch), RT_EOK);
        uassert_int_equal(ioctl(fd, RT_INPUT_CTRL_GET_INFO, &info), 0);
        uassert_buf_equal(&info, &touch, sizeof(info));
        uassert_int_equal(rt_input_report(&input, EV_ABS, ABS_X, 42), RT_EOK);
        uassert_int_equal(rt_input_sync(&input), RT_EOK);
        struct pollfd p = { .fd = fd, .events = POLLIN };
        uassert_int_equal(poll(&p, 1, 0), 1);
        uassert_int_equal(p.revents, POLLIN);
        if (p.revents & POLLIN) {
            uassert_int_equal(read(fd, ev, sizeof(ev)), sizeof(ev));
            uassert_int_equal(ev[0].value, 42);
            uassert_int_equal(ev[1].type, EV_SYN);
        }
        check_snapshots(fd);
        second = open(path, O_WRONLY);
        uassert_true(second >= 0);
        uassert_int_equal(close(fd), 0);
        if (second >= 0) {
            uassert_int_equal(rt_input_dev_unregister(&input), -RT_EBUSY);
            uassert_int_equal(close(second), 0);
        }
    }
    uassert_int_equal(rt_input_dev_unregister(&input), RT_EOK);
    uassert_int_equal(input.id, -1);
    uassert_null(rt_device_find(path + sizeof("/dev/") - 1));
}

static void testcase(void)
{
    UTEST_UNIT_RUN(input_lifecycle);
}
UTEST_TC_EXPORT(testcase, "usb.input.lifecycle", RT_NULL, RT_NULL, 15);
