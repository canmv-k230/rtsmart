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

#define PMUTEX_NORMAL     0 /* Unable to recursion */
#define PMUTEX_RECURSIVE  1 /* Can be recursion */
#define PMUTEX_ERRORCHECK 2 /* This type of mutex provides error checking */

struct rt_pmutex
{
    union
    {
        rt_mutex_t kmutex;
        rt_sem_t ksem; /* use sem to emulate the mutex without recursive */
    } lock;

    struct lwp_avl_struct node;
    struct rt_object *custom_obj;
    rt_uint8_t type; /* pmutex type */
    rt_uint8_t destroying;
    rt_uint32_t operation_count;
};

static struct rt_mutex _pmutex_lock;
#ifdef RT_USING_SMP
static struct rt_spinlock _pmutex_tree_lock;
#else
static rt_spinlock_t _pmutex_tree_lock;
#endif

static int pmutex_system_init(void)
{
    rt_mutex_init(&_pmutex_lock, "pmtxLock", RT_IPC_FLAG_FIFO);
    rt_spin_lock_init(&_pmutex_tree_lock);
    return 0;
}
INIT_PREV_EXPORT(pmutex_system_init);

static void pmutex_release(struct rt_pmutex *pmutex)
{
    if (pmutex->type == PMUTEX_NORMAL)
    {
        rt_sem_delete(pmutex->lock.ksem);
    }
    else
    {
        rt_mutex_delete(pmutex->lock.kmutex);
    }

    rt_free(pmutex);
}

static void pmutex_tree_insert(struct rt_pmutex *pmutex)
{
    rt_base_t level = rt_spin_lock_irqsave(&_pmutex_tree_lock);

    lwp_avl_insert(&pmutex->node, (struct lwp_avl_struct **)pmutex->node.data);
    rt_spin_unlock_irqrestore(&_pmutex_tree_lock, level);
}

static void pmutex_tree_remove(struct rt_pmutex *pmutex)
{
    rt_base_t level = rt_spin_lock_irqsave(&_pmutex_tree_lock);

    if (pmutex->node.data != RT_NULL)
    {
        lwp_avl_remove(&pmutex->node, (struct lwp_avl_struct **)pmutex->node.data);
        pmutex->node.data = RT_NULL;
    }
    rt_spin_unlock_irqrestore(&_pmutex_tree_lock, level);
}

static int pmutex_exists(void *umutex, struct rt_lwp *lwp)
{
    struct lwp_avl_struct *node;
    int exists = 0;
    rt_base_t level;

    if (!lwp)
    {
        return 0;
    }

    level = rt_spin_lock_irqsave(&_pmutex_tree_lock);
    node = lwp_avl_find((avl_key_t)umutex, lwp->pmutex_search_head);
    if (node)
    {
        struct rt_pmutex *pmutex = rt_container_of(node, struct rt_pmutex, node);

        exists = !pmutex->destroying;
    }
    rt_spin_unlock_irqrestore(&_pmutex_tree_lock, level);
    return exists;
}

static struct rt_pmutex *pmutex_get(void *umutex, struct rt_lwp *lwp,
                                    rt_uint32_t *active_operations)
{
    struct rt_pmutex *pmutex = RT_NULL;
    struct lwp_avl_struct *node;
    rt_base_t level;

    if (!lwp)
    {
        return RT_NULL;
    }

    level = rt_spin_lock_irqsave(&_pmutex_tree_lock);
    node = lwp_avl_find((avl_key_t)umutex, lwp->pmutex_search_head);
    if (node)
    {
        pmutex = rt_container_of(node, struct rt_pmutex, node);
        if (pmutex->destroying)
        {
            pmutex = RT_NULL;
        }
        else
        {
            if (active_operations)
            {
                *active_operations = pmutex->operation_count;
            }
            pmutex->operation_count++;
        }
    }
    rt_spin_unlock_irqrestore(&_pmutex_tree_lock, level);
    return pmutex;
}

static void pmutex_put(struct rt_pmutex *pmutex)
{
    int release = 0;
    rt_base_t level = rt_spin_lock_irqsave(&_pmutex_tree_lock);

    RT_ASSERT(pmutex->operation_count > 0);
    pmutex->operation_count--;
    if (pmutex->destroying && pmutex->operation_count == 0)
    {
        release = 1;
    }
    rt_spin_unlock_irqrestore(&_pmutex_tree_lock, level);

    if (release)
    {
        pmutex_release(pmutex);
    }
}

static int pmutex_is_locked(struct rt_pmutex *pmutex)
{
    int locked;
    rt_base_t level = rt_hw_interrupt_disable();

    if (pmutex->type == PMUTEX_NORMAL)
    {
        locked = pmutex->lock.ksem->value == 0;
    }
    else if (pmutex->type == PMUTEX_RECURSIVE || pmutex->type == PMUTEX_ERRORCHECK)
    {
        locked = pmutex->lock.kmutex->owner != RT_NULL;
    }
    else
    {
        locked = 1;
    }

    rt_hw_interrupt_enable(level);
    return locked;
}

static rt_err_t pmutex_destory(void *data)
{
    int release = 0;
    rt_base_t level;
    struct rt_pmutex *pmutex = (struct rt_pmutex *)data;

    if (pmutex)
    {
        level = rt_spin_lock_irqsave(&_pmutex_tree_lock);
        if (pmutex->node.data != RT_NULL)
        {
            lwp_avl_remove(&pmutex->node,
                           (struct lwp_avl_struct **)pmutex->node.data);
            pmutex->node.data = RT_NULL;
        }
        pmutex->destroying = 1;
        release = pmutex->operation_count == 0;
        rt_spin_unlock_irqrestore(&_pmutex_tree_lock, level);

        if (release)
        {
            pmutex_release(pmutex);
        }
    }
    return pmutex ? RT_EOK : -RT_ERROR;
}

static struct rt_pmutex* pmutex_create(void *umutex, struct rt_lwp *lwp)
{
    struct rt_pmutex *pmutex = RT_NULL;
    struct rt_object *obj = RT_NULL;
    rt_ubase_t type;

    if (!lwp)
    {
        return RT_NULL;
    }

    long *p = (long *)umutex;
    /* umutex[0] bit[0-1] saved mutex type */
    type = *p & 3;
    if (type != PMUTEX_NORMAL && type != PMUTEX_RECURSIVE && type != PMUTEX_ERRORCHECK)
    {
        return RT_NULL;
    }

    pmutex = (struct rt_pmutex *)rt_malloc(sizeof(struct rt_pmutex));
    if (!pmutex)
    {
        return RT_NULL;
    }
    pmutex->type = type;
    pmutex->destroying = 0;
    pmutex->operation_count = 0;

    if (type == PMUTEX_NORMAL)
    {
        pmutex->lock.ksem = rt_sem_create("pmutex", 1, RT_IPC_FLAG_PRIO);
        if (!pmutex->lock.ksem)
        {
            rt_free(pmutex);
            return RT_NULL;
        }
    }
    else
    {
        pmutex->lock.kmutex = rt_mutex_create("pmutex", RT_IPC_FLAG_PRIO);
        if (!pmutex->lock.kmutex)
        {
            rt_free(pmutex);
            return RT_NULL;
        }
    }

    obj = rt_custom_object_create("pmutex", (void *)pmutex, pmutex_destory);
    if (!obj)
    {
        if (pmutex->type == PMUTEX_NORMAL)
        {
            rt_sem_delete(pmutex->lock.ksem);
        }
        else
        {
            rt_mutex_delete(pmutex->lock.kmutex);
        }
        rt_free(pmutex);
        return RT_NULL;
    }
    pmutex->node.avl_key = (avl_key_t)umutex;
    pmutex->node.data = &lwp->pmutex_search_head;
    pmutex->custom_obj = obj;

    /* insert into pmutex head */
    pmutex_tree_insert(pmutex);
    return pmutex;
}

static int _pthread_mutex_init(void *umutex)
{
    struct rt_lwp *lwp = RT_NULL;
    struct rt_pmutex *pmutex = RT_NULL;
    rt_err_t lock_ret = 0;

    /* umutex union is 6 x (void *) */
    if (!lwp_user_accessable(umutex, sizeof(void *) * 6))
    {
        rt_set_errno(EINVAL);
        return -EINVAL;
    }

    lock_ret = rt_mutex_take_interruptible(&_pmutex_lock, RT_WAITING_FOREVER);
    if (lock_ret != RT_EOK)
    {
        rt_set_errno(EAGAIN);
        return -EAGAIN;
    }

    lwp = lwp_self();
    if (!pmutex_exists(umutex, lwp))
    {
        /* create a pmutex according to this umutex */
        pmutex = pmutex_create(umutex, lwp);
        if (pmutex == RT_NULL)
        {
            rt_mutex_release(&_pmutex_lock);
            rt_set_errno(ENOMEM);
            return -ENOMEM;
        }
        if (lwp_user_object_add(lwp, pmutex->custom_obj) != 0)
        {
            pmutex_tree_remove(pmutex);
            rt_custom_object_destroy(pmutex->custom_obj);
            rt_mutex_release(&_pmutex_lock);
            rt_set_errno(ENOMEM);
            return -ENOMEM;
        }
    }
    /* The lock path can race with another first-use initialization. Keep an
     * existing kernel lock intact instead of resetting a live mutex. */

    rt_mutex_release(&_pmutex_lock);

    return 0;
}

static int _pthread_mutex_lock_timeout(void *umutex, struct timespec *timeout)
{
    struct rt_lwp *lwp = RT_NULL;
    struct rt_pmutex *pmutex = RT_NULL;
    rt_err_t lock_ret = 0;
    int ret = 0;
    rt_int32_t time = RT_WAITING_FOREVER;
    register rt_base_t temp;

    if (timeout)
    {
        if (!lwp_user_accessable((void *)timeout, sizeof(struct timespec)))
        {
            rt_set_errno(EINVAL);
            return -EINVAL;
        }
        time = clock_time_to_tick(timeout);
    }

    lock_ret = rt_mutex_take_interruptible(&_pmutex_lock, RT_WAITING_FOREVER);
    if (lock_ret != RT_EOK)
    {
        rt_set_errno(EINTR);
        return -EINTR;
    }

    lwp = lwp_self();
    pmutex = pmutex_get(umutex, lwp, RT_NULL);
    if (pmutex == RT_NULL)
    {
        rt_mutex_release(&_pmutex_lock);
        rt_set_errno(EINVAL);
        return -ENOMEM;  /* umutex not recored in kernel */
    }
    rt_mutex_release(&_pmutex_lock);

    switch (pmutex->type)
    {
    case PMUTEX_NORMAL:
        lock_ret = rt_sem_take_interruptible(pmutex->lock.ksem, time);
        break;
    case PMUTEX_RECURSIVE:
        lock_ret = rt_mutex_take_interruptible(pmutex->lock.kmutex, time);
        break;
    case PMUTEX_ERRORCHECK:
        temp = rt_hw_interrupt_disable();
        if (pmutex->lock.kmutex->owner == rt_thread_self())
        {
            /* enable interrupt */
            rt_hw_interrupt_enable(temp);
            rt_set_errno(EDEADLK);
            ret = -EDEADLK;
            goto out;
        }
        lock_ret = rt_mutex_take_interruptible(pmutex->lock.kmutex, time);
        rt_hw_interrupt_enable(temp);
        break;
    default: /* unknown type */
        rt_set_errno(EINVAL);
        ret = -EINVAL;
        goto out;
    }

    if (lock_ret != RT_EOK)
    {
        if (lock_ret == -RT_ETIMEOUT)
        {
            if (time == 0) /* timeout is 0, means try lock failed */
            {
                rt_set_errno(EBUSY);
                ret = -EBUSY;
            }
            else
            {
                rt_set_errno(ETIMEDOUT);
                ret = -ETIMEDOUT;
            }
        }
        else if (lock_ret == -RT_EINTR)
        {
            rt_set_errno(EINTR);
            ret = -EINTR;
        }
        else
        {
            rt_set_errno(EAGAIN);
            ret = -EAGAIN;
        }
    }
out:
    pmutex_put(pmutex);
    return ret;
}

static int _pthread_mutex_unlock(void *umutex)
{
    struct rt_lwp *lwp = RT_NULL;
    struct rt_pmutex *pmutex = RT_NULL;
    rt_err_t lock_ret = 0;
    int ret = 0;
    rt_base_t level;

    lock_ret = rt_mutex_take_interruptible(&_pmutex_lock, RT_WAITING_FOREVER);
    if (lock_ret != RT_EOK)
    {
        rt_set_errno(EAGAIN);
        return -EAGAIN;
    }

    lwp = lwp_self();
    pmutex = pmutex_get(umutex, lwp, RT_NULL);
    if (pmutex == RT_NULL)
    {
        rt_mutex_release(&_pmutex_lock);
        rt_set_errno(EPERM);
        return -EPERM;//unlock static mutex of unlock state
    }
    rt_mutex_release(&_pmutex_lock);

    switch (pmutex->type)
    {
    case PMUTEX_NORMAL:
        level = rt_hw_interrupt_disable();
        if(pmutex->lock.ksem->value >=1)
        {
            rt_hw_interrupt_enable(level);
            rt_set_errno(EPERM);
            ret = -EPERM;
            goto out;
        }
        rt_hw_interrupt_enable(level);
        lock_ret = rt_sem_release(pmutex->lock.ksem);
        break;
    case PMUTEX_RECURSIVE:
    case PMUTEX_ERRORCHECK:
        lock_ret = rt_mutex_release(pmutex->lock.kmutex);
        break;
    default: /* unknown type */
        rt_set_errno(EINVAL);
        ret = -EINVAL;
        goto out;
    }

    if (lock_ret != RT_EOK)
    {
        rt_set_errno(EPERM);
        ret = -EPERM;
    }
out:
    pmutex_put(pmutex);
    return ret;
}

static int _pthread_mutex_destroy(void *umutex)
{
    struct rt_lwp *lwp = RT_NULL;
    struct rt_pmutex *pmutex = RT_NULL;
    rt_uint32_t active_operations;
    rt_err_t lock_ret = 0;

    lock_ret = rt_mutex_take_interruptible(&_pmutex_lock, RT_WAITING_FOREVER);
    if (lock_ret != RT_EOK)
    {
        rt_set_errno(EAGAIN);
        return -EAGAIN;
    }

    lwp = lwp_self();
    pmutex = pmutex_get(umutex, lwp, &active_operations);
    if (pmutex == RT_NULL)
    {
        rt_mutex_release(&_pmutex_lock);
        rt_set_errno(EINVAL);
        return -EINVAL;
    }

    if (active_operations != 0 || pmutex_is_locked(pmutex))
    {
        pmutex_put(pmutex);
        rt_mutex_release(&_pmutex_lock);
        rt_set_errno(EBUSY);
        return -EBUSY;
    }

    pmutex_tree_remove(pmutex);
    lock_ret = lwp_user_object_delete(lwp, pmutex->custom_obj);

    pmutex_put(pmutex);
    rt_mutex_release(&_pmutex_lock);

    if (lock_ret != RT_EOK)
    {
        rt_set_errno(EINVAL);
        return -EINVAL;
    }

    return 0;
}

int sys_pmutex(void *umutex, int op, void *arg)
{
    int ret = -EINVAL;

    switch (op)
    {
        case PMUTEX_INIT:
            ret = _pthread_mutex_init(umutex);
            break;
        case PMUTEX_LOCK:
            ret = _pthread_mutex_lock_timeout(umutex, (struct timespec*)arg);
            if (ret == -ENOMEM)
            {
                /* lock not init, try init it and lock again. */
                ret = _pthread_mutex_init(umutex);
                if (ret == 0)
                {
                    ret = _pthread_mutex_lock_timeout(umutex, (struct timespec*)arg);
                }
            }
            break;
        case PMUTEX_UNLOCK:
            ret = _pthread_mutex_unlock(umutex);
            break;
        case PMUTEX_DESTROY:
            ret = _pthread_mutex_destroy(umutex);
            break;
        default:
            rt_set_errno(EINVAL);
            break;
    }

    /* PMUTEX is the userspace pthread interface, whose operations return
     * positive errno values rather than the kernel's raw negative errno. */
    return ret < 0 ? -ret : ret;
}
