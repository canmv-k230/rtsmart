/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 */

#include <rthw.h>
#include <rtthread.h>
#include "lwp_mm.h"

static struct rt_mutex mm_lock;

static int lwp_mm_system_init(void)
{
    rt_mutex_init(&mm_lock, "mm_lock", RT_IPC_FLAG_FIFO);
    return 0;
}
INIT_BOARD_EXPORT(lwp_mm_system_init);

void rt_mm_lock(void)
{
    if (rt_thread_self())
    {
        rt_mutex_take(&mm_lock, RT_WAITING_FOREVER);
    }
}

void rt_mm_unlock(void)
{
    if (rt_thread_self())
    {
        rt_mutex_release(&mm_lock);
    }
}
