/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2016-12-28     Bernard      first version
 * 2018-03-09     Bernard      Add protection for pt->triggered.
 */
#include <stdint.h>

#include <rthw.h>
#include <rtdevice.h>
#include <rtthread.h>

#include <dfs.h>
#include <dfs_file.h>
#include <dfs_posix.h>
#include <dfs_poll.h>

#ifdef RT_USING_POSIX

struct rt_poll_node;

struct rt_poll_table
{
    rt_pollreq_t req;
    rt_uint32_t triggered; /* the waited thread whether triggered */
    rt_bool_t sleeping;    /* wakeups may resume only the poll sleep */
    rt_thread_t polling_thread;
    struct rt_poll_node *nodes;
    int error;
};

struct rt_poll_node
{
    struct rt_wqueue_node wqn;
    struct rt_poll_table *pt;
    struct rt_poll_node *next;
};

static int __wqueue_pollwake(struct rt_wqueue_node *wait, void *key)
{
    struct rt_poll_node *pn;

    if (key && !((rt_ubase_t)key & wait->key))
        return -1;

    pn = rt_container_of(wait, struct rt_poll_node, wqn);
    pn->pt->triggered = 1;

    /* Waitqueue callbacks run with interrupts disabled. Keep readiness even
     * during a scan, but do not resume a thread blocked on a backend mutex. */
    if (!pn->pt->sleeping)
        return -1;

    pn->pt->sleeping = RT_FALSE;
    pn->pt->polling_thread->error = RT_EOK;
    return __wqueue_default_wake(wait, key);
}

static void _poll_add(rt_wqueue_t *wq, rt_pollreq_t *req)
{
    struct rt_poll_table *pt;
    struct rt_poll_node *node;

    pt = rt_container_of(req, struct rt_poll_table, req);
    node = (struct rt_poll_node *)rt_malloc(sizeof(struct rt_poll_node));
    if (node == RT_NULL)
    {
        pt->error = -ENOMEM;
        return;
    }


    node->wqn.key = req->_key;
    rt_list_init(&(node->wqn.list));
    node->wqn.polling_thread = pt->polling_thread;
    node->wqn.wakeup = __wqueue_pollwake;
    node->next = pt->nodes;
    node->pt = pt;
    pt->nodes = node;
    rt_wqueue_add(wq, &node->wqn);
}

static void poll_table_init(struct rt_poll_table *pt)
{
    pt->req._proc = _poll_add;
    pt->triggered = 0;
    pt->sleeping = RT_FALSE;
    pt->nodes = RT_NULL;
    pt->error = 0;
    pt->polling_thread = rt_thread_self();
}

/* Return 0 for a wakeup, 1 for no event/timeout, or a negative POSIX errno. */
static int poll_wait_timeout(struct rt_poll_table *pt, int msec)
{
    rt_int32_t timeout;
    int ret = 0;
    struct rt_thread *thread;
    rt_base_t level;

    thread = pt->polling_thread;

    timeout = rt_tick_from_millisecond(msec);

    level = rt_hw_interrupt_disable();

    if (timeout != 0 && !pt->triggered)
    {
        /* LWP signal resume does not assign an errno. As in RT-Thread IPC,
         * a normal wakeup clears this; timer expiry supplies ETIMEOUT. */
        thread->error = -RT_EINTR;
        /* Interrupts remain disabled across suspend and sleeping publication.
         * Publishing earlier would allow a backend-mutex wait to be resumed. */
        if (rt_thread_suspend_with_flag(thread, RT_INTERRUPTIBLE) == RT_EOK)
        {
            pt->sleeping = RT_TRUE;
            if (timeout > 0)
            {
                rt_timer_control(&(thread->thread_timer),
                        RT_TIMER_CTRL_SET_TIME,
                        &timeout);
                rt_timer_start(&(thread->thread_timer));
            }

            rt_hw_interrupt_enable(level);

            rt_schedule();

            level = rt_hw_interrupt_disable();
            pt->sleeping = RT_FALSE;
        }

        if (thread->error == -RT_EINTR || thread->error == RT_EINTR)
            ret = -EINTR;
    }

    if (ret == 0)
        ret = !pt->triggered;
    rt_hw_interrupt_enable(level);

    return ret;
}

static int do_pollfd(struct pollfd *pollfd, struct dfs_fd *f, rt_pollreq_t *req)
{
    int mask = 0;
    int fd;

    fd = pollfd->fd;

    if (fd >= 0)
    {
        mask = POLLNVAL;

        if (f)
        {
            mask = POLLMASK_DEFAULT;
            if (f->fnode->fops->poll)
            {
                req->_key = pollfd->events | POLLERR | POLLHUP;

                mask = f->fnode->fops->poll(f, req);

                /* dealwith the device return error -1*/
                if (mask < 0)
                {
                    pollfd->revents = 0;
                    return mask;
                }
            }
            /* Mask out unneeded events. */
            mask &= pollfd->events | POLLERR | POLLHUP;
        }
    }
    pollfd->revents = mask;

    return mask;
}

static int poll_do(struct pollfd *fds, struct dfs_fd **files, nfds_t nfds,
                  struct rt_poll_table *pt, int msec)
{
    int num;
    int istimeout = 0;
    int n;
    struct pollfd *pf;
    int  ret = 0;

    if (msec == 0)
    {
        pt->req._proc = RT_NULL;
        istimeout = 1;
    }

    while (1)
    {
        pf = fds;
        num = 0;

        for (n = 0; n < nfds; n ++)
        {
            ret = do_pollfd(pf, files[n], &pt->req);
            if(ret < 0)
            {
                /*dealwith the device return error -1  */
                pt->req._proc = RT_NULL;
                return ret;
            }
            else if(ret > 0)
            {
                num ++;
                pt->req._proc = RT_NULL;
            }
            pf ++;
        }

        pt->req._proc = RT_NULL;

        if (num || istimeout)
            break;

        ret = poll_wait_timeout(pt, msec);
        if (ret < 0)
            return ret;
        if (ret)
            istimeout = 1;
    }

    return num;
}

static void poll_teardown(struct rt_poll_table *pt)
{
    struct rt_poll_node *node, *next;

    next = pt->nodes;
    while (next)
    {
        node = next;
        rt_wqueue_remove(&node->wqn);
        next = node->next;
        rt_free(node);
    }
}

int rt_poll_wait(int (*poll_fn)(void *, struct rt_pollreq *), void *data,
                 int events, int timeout)
{
    struct rt_poll_table table;
    rt_tick_t started = rt_tick_get();
    int result;

    poll_table_init(&table);
    table.req._key = events | POLLERR | POLLHUP;
    if (!timeout)
        table.req._proc = RT_NULL;

    for (;;)
    {
        rt_base_t level;
        int remaining = timeout;

        /* Clear before scanning: an event during the scan must prevent sleep. */
        level = rt_hw_interrupt_disable();
        table.triggered = 0;
        rt_hw_interrupt_enable(level);
        result = poll_fn(data, &table.req);
        table.req._proc = RT_NULL;
        if (result < 0)
            break;
        result &= table.req._key;
        if (result)
            break;
        if (table.error)
        {
            result = table.error;
            break;
        }
        if (timeout >= 0)
        {
            rt_uint64_t elapsed = (rt_uint64_t)(rt_tick_get() - started) *
                                  1000 / RT_TICK_PER_SECOND;
            if (elapsed >= (rt_uint32_t)timeout)
                break;
            remaining -= (int)elapsed;
        }
        result = poll_wait_timeout(&table, remaining);
        if (result < 0)
            break;
        /* Recheck readiness after timeout or a competing reader's wakeup. */
    }
    poll_teardown(&table);
    return result;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    int num;
    struct rt_poll_table table;
    struct dfs_fd **files = RT_NULL;
    nfds_t n;

    if (nfds)
    {
        files = rt_calloc(nfds, sizeof(*files));
        if (!files)
        {
            return -ENOMEM;
        }
        for (n = 0; n < nfds; n++)
        {
            files[n] = fd_get_ref(fds[n].fd);
        }
    }

    poll_table_init(&table);

    num = poll_do(fds, files, nfds, &table, timeout);

    poll_teardown(&table);
    /* Waitqueue nodes point into the backend; unlink them before final close. */
    for (n = 0; n < nfds; n++)
    {
        fd_put(files[n]);
    }
    rt_free(files);

    return num;
}

#endif 
