/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021/01/02     bernard      the first version
 */

#include <rtthread.h>
#include <lwp.h>
#ifdef RT_USING_USERSPACE
#include <lwp_user_mm.h>
#endif
#include "clock_time.h"

struct rt_futex
{
    int *uaddr;
    rt_list_t waiting_thread;
    struct lwp_avl_struct node;
    struct rt_object *custom_obj;
};

static struct rt_mutex _futex_lock;

static rt_tick_t futex_timeout_to_tick(const struct timespec *timeout)
{
    rt_int64_t second_delta;
    rt_int64_t nsecond_delta;
    rt_uint64_t tick;
    rt_uint64_t tick_max = RT_TICK_MAX / 2U - 1U;

    if (timeout == RT_NULL)
    {
        return 0;
    }

    second_delta = timeout->tv_sec;
    nsecond_delta = timeout->tv_nsec;

    if (nsecond_delta < 0)
    {
        nsecond_delta += NANOSECOND_PER_SECOND;
        second_delta -= 1;
    }

    if (second_delta < 0)
    {
        return 0;
    }

    tick = (rt_uint64_t)second_delta * RT_TICK_PER_SECOND;
    if (nsecond_delta > 0)
    {
        /* A relative futex timeout must not expire before its deadline. */
        tick += ((rt_uint64_t)nsecond_delta * RT_TICK_PER_SECOND
                 + NANOSECOND_PER_SECOND - 1) / NANOSECOND_PER_SECOND;
    }

    if (tick > tick_max)
    {
        tick = tick_max;
    }

    return (rt_tick_t)tick;
}

static int futex_system_init(void)
{
    rt_mutex_init(&_futex_lock, "futexList", RT_IPC_FLAG_FIFO);
    return 0;
}
INIT_PREV_EXPORT(futex_system_init);

rt_err_t futex_destory(void *data)
{
    rt_err_t ret = -1;
    rt_base_t level;
    struct rt_futex *futex = (struct rt_futex *)data;

    if (futex)
    {
        level = rt_hw_interrupt_disable();
        /* remove futex from futext avl */
        lwp_avl_remove(&futex->node, (struct lwp_avl_struct **)futex->node.data);
        rt_hw_interrupt_enable(level);

        /* release object */
        rt_free(futex);
        ret = 0;
    }
    return ret;
}

struct rt_futex *futex_create(int *uaddr, struct rt_lwp *lwp)
{
    struct rt_futex *futex = RT_NULL;
    struct rt_object *obj = RT_NULL;

    if (!lwp)
    {
        return RT_NULL;
    }
    futex = (struct rt_futex *)rt_malloc(sizeof(struct rt_futex));
    if (!futex)
    {
        return RT_NULL;
    }
    obj = rt_custom_object_create("futex", (void *)futex, futex_destory);
    if (!obj)
    {
        rt_free(futex);
        return RT_NULL;
    }

    futex->uaddr = uaddr;
    futex->node.avl_key = (avl_key_t)uaddr;
    futex->node.data = &lwp->address_search_head;
    futex->custom_obj = obj;
    rt_list_init(&(futex->waiting_thread));

    /* insert into futex head */
    lwp_avl_insert(&futex->node, &lwp->address_search_head);
    return futex;
}

static struct rt_futex *futex_get(void *uaddr, struct rt_lwp *lwp)
{
    struct rt_futex *futex = RT_NULL;
    struct lwp_avl_struct *node = RT_NULL;

    node = lwp_avl_find((avl_key_t)uaddr, lwp->address_search_head);
    if (!node)
    {
        return RT_NULL;
    }
    futex = rt_container_of(node, struct rt_futex, node);
    return futex;
}

int futex_wait(struct rt_futex *futex, int value, const struct timespec *timeout)
{
    rt_base_t level = 0;
    rt_err_t ret = -RT_EINTR;

    if (*(futex->uaddr) == value)
    {
        rt_thread_t thread = rt_thread_self();

        level = rt_hw_interrupt_disable();
        ret = rt_thread_suspend_with_flag(thread, RT_INTERRUPTIBLE);

        if (ret < 0)
        {
            rt_mutex_release(&_futex_lock);
            rt_hw_interrupt_enable(level);
            rt_set_errno(EINTR);
            return -EINTR;
        }

        /* add into waiting thread list */
        rt_list_insert_before(&(futex->waiting_thread), &(thread->tlist));

        /* with timeout */
        if (timeout)
        {
            rt_tick_t time = futex_timeout_to_tick(timeout);

            /* start the timer of thread */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &time);
            rt_timer_start(&(thread->thread_timer));
        }
        rt_mutex_release(&_futex_lock);
        rt_hw_interrupt_enable(level);

        /* do schedule */
        rt_schedule();

        ret = thread->error;
        if (ret == -RT_ETIMEOUT)
        {
            rt_set_errno(ETIMEDOUT);
            ret = -ETIMEDOUT;
        }
        else if (ret == -RT_EINTR)
        {
            rt_set_errno(EINTR);
            ret = -EINTR;
        }
        else if (ret != RT_EOK)
        {
            rt_set_errno(EAGAIN);
            ret = -EAGAIN;
        }
    }
    else
    {
        rt_mutex_release(&_futex_lock);
        ret = -EAGAIN;
        rt_set_errno(EAGAIN);
    }

    return ret;
}

int futex_wake(struct rt_futex *futex, int number)
{
    int woken = 0;
    rt_base_t level = rt_hw_interrupt_disable();
    while (!rt_list_isempty(&(futex->waiting_thread)) && number)
    {
        rt_thread_t thread;

        thread = rt_list_entry(futex->waiting_thread.next, struct rt_thread, tlist);
        /* remove from waiting list */
        rt_list_remove(&(thread->tlist));

        thread->error = RT_EOK;
        /* resume the suspended thread */
        rt_thread_resume(thread);
        woken++;

        number--;
    }
    rt_mutex_release(&_futex_lock);
    rt_hw_interrupt_enable(level);

    /* do schedule */
    if (woken)
    {
        rt_schedule();
    }

    return woken;
}

int sys_futex(int *uaddr, int op, int val, const struct timespec *timeout,
              int *uaddr2, int val3)
{
    struct rt_lwp *lwp = RT_NULL;
    struct rt_futex *futex = RT_NULL;
    int command = op & ~FUTEX_PRIVATE_FLAG;
    int ret = 0;
    rt_err_t lock_ret = 0;

    if (!lwp_user_accessable(uaddr, sizeof(int)))
    {
        rt_set_errno(EINVAL);
        return -EINVAL;
    }

    if (command == FUTEX_WAIT && timeout)
    {
        if (!lwp_user_accessable((void *)timeout, sizeof(struct timespec)))
        {
            rt_set_errno(EINVAL);
            return -EINVAL;
        }
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= NANOSECOND_PER_SECOND)
        {
            rt_set_errno(EINVAL);
            return -EINVAL;
        }
    }
    else if (command == FUTEX_REQUEUE)
    {
        /*
         * musl uses (wake_count, requeue_count) == (0, 1) to hand a private
         * condition-variable barrier to its mutex. RT-Smart mutexes wait on
         * pmutex objects, so that one barrier waiter must be woken directly.
         * Reject all other forms instead of pretending to provide the Linux
         * FUTEX_REQUEUE ABI without moving waiters to uaddr2.
         */
        if (!(op & FUTEX_PRIVATE_FLAG) ||
            val != 0 || (rt_base_t)timeout != 1)
        {
            rt_set_errno(ENOSYS);
            return -ENOSYS;
        }
        if (uaddr2 == uaddr)
        {
            rt_set_errno(EINVAL);
            return -EINVAL;
        }
        if (!lwp_user_accessable(uaddr2, sizeof(int)))
        {
            rt_set_errno(EFAULT);
            return -EFAULT;
        }
    }
    lock_ret = rt_mutex_take_interruptible(&_futex_lock, RT_WAITING_FOREVER);
    if (lock_ret != RT_EOK)
    {
        rt_set_errno(EINTR);
        return -EINTR;
    }

    lwp = lwp_self();
    futex = futex_get(uaddr, lwp);

    if (command == FUTEX_WAIT && futex == RT_NULL)
    {
        /* create a futex according to this uaddr */
        futex = futex_create(uaddr, lwp);
        if (futex == RT_NULL)
        {
            rt_mutex_release(&_futex_lock);
            rt_set_errno(ENOMEM);
            return -ENOMEM;
        }
        if (lwp_user_object_add(lwp, futex->custom_obj) != 0)
        {
            rt_custom_object_destroy(futex->custom_obj);
            rt_mutex_release(&_futex_lock);
            rt_set_errno(ENOMEM);
            return -ENOMEM;
        }
    }

    switch (command)
    {
    case FUTEX_WAIT:
        ret = futex_wait(futex, val, timeout);
        /* _futex_lock is released by futex_wait */
        break;

    case FUTEX_WAKE:
        if (futex == RT_NULL)
        {
            rt_mutex_release(&_futex_lock);
        }
        else
        {
            ret = futex_wake(futex, val);
            /* _futex_lock is released by futex_wake */
        }
        break;

    case FUTEX_REQUEUE:
        if (futex == RT_NULL)
        {
            rt_mutex_release(&_futex_lock);
        }
        else
        {
            ret = futex_wake(futex, 1);
            /* _futex_lock is released by futex_wake */
        }
        break;

    default:
        rt_mutex_release(&_futex_lock);
        rt_set_errno(ENOSYS);
        ret = -ENOSYS;
        break;
    }

    return ret;
}
