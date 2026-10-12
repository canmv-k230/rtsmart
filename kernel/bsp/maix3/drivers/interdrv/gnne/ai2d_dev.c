/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
 
#include <rtthread.h>
#include <rthw.h>
#include "k230_pm.h"

#ifdef RT_USING_POSIX
#include <dfs_posix.h>
#include <dfs_poll.h>
#include <posix_termios.h>
#endif

#define AI2D_LOG_ENABLE
struct ai_2d_dev_handle {
	rt_wqueue_t *wait;
};


#ifdef  AI2D_LOG_ENABLE
static int ai_2d_log(const char *fmt, ...)
{
    rt_kprintf(fmt);
    return 0;
}
#else
static int ai_2d_log(const char *fmt, ...)
{
    return 0;
}
#endif

#define ai_2d_info(s...)	do { \
	ai_2d_log("<ai_2d> "); \
	ai_2d_log(s); \
	ai_2d_log("\r\n"); \
} while (0)

#define ai_2d_err(s...) do { \
	ai_2d_log("<err>[%s:%d] ", __func__, __LINE__); \
	ai_2d_log(s); \
	ai_2d_log("\r\n"); \
} while (0)

#define IRQN_AI2D_INTERRUPT (16 + 175)

static struct rt_device g_ai_2d_device = {0};
static struct rt_event g_ai_2d_event = {0};
static struct rt_mutex g_ai_2d_pm_mutex = {0};
static rt_uint32_t g_ai_2d_open_count;
static rt_bool_t g_ai_2d_pm_vote_held;
extern void *gnne_base_addr;

static void ai_2d_clear_interrupt(void)
{
    volatile rt_uint32_t *write_addr;

    if (gnne_base_addr == RT_NULL)
        return;

    write_addr = (rt_uint32_t *)((char *)gnne_base_addr + 0xca0);
    write_addr[0] = 1;
    write_addr[1] = 0;
    write_addr[2] = 0;
    write_addr[3] = 0;
}

static int ai_2d_device_open(struct dfs_fd *file)
{
    struct ai_2d_dev_handle *handle;
    rt_device_t device;
    int ret;

    handle = rt_malloc(sizeof (struct ai_2d_dev_handle));
    if(handle == RT_NULL) {
        ai_2d_err("malloc failed\n");
        return -1;
    }

    ret = rt_mutex_take(&g_ai_2d_pm_mutex, RT_WAITING_FOREVER);
    if (ret != RT_EOK) {
        rt_free(handle);
        return -RT_ERROR;
    }

    if (g_ai_2d_open_count == 0) {
        if (g_ai_2d_pm_vote_held) {
            ret = k230_pm_put(K230_PM_DEV_AI);
            if (ret != 0) {
                rt_mutex_release(&g_ai_2d_pm_mutex);
                rt_free(handle);
                return ret;
            }
            g_ai_2d_pm_vote_held = RT_FALSE;
        }

        ret = k230_pm_get(K230_PM_DEV_AI);
        if (ret != 0) {
            rt_mutex_release(&g_ai_2d_pm_mutex);
            rt_free(handle);
            return ret;
        }
        g_ai_2d_pm_vote_held = RT_TRUE;
        ai_2d_clear_interrupt();
        rt_event_recv(&g_ai_2d_event, 0x01,
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, RT_NULL);
        rt_hw_interrupt_umask(IRQN_AI2D_INTERRUPT);
    }
    g_ai_2d_open_count++;
    rt_mutex_release(&g_ai_2d_pm_mutex);

    device = (rt_device_t)file->fnode->data;
    handle->wait = &device->wait_queue;
    file->data = (void*)handle;
    return RT_EOK;
}

static int ai_2d_device_close(struct dfs_fd *file)
{
    struct ai_2d_dev_handle *handle;
    int ret;
    int pm_ret;

    handle = (struct ai_2d_dev_handle *)file->data;
    if(handle == RT_NULL) {
        ai_2d_err("try to close a invalid handle");
		return -RT_EINVAL;
    }

    ret = rt_mutex_take(&g_ai_2d_pm_mutex, RT_WAITING_FOREVER);
    if (ret != RT_EOK) {
        rt_free(handle);
        file->data = RT_NULL;
        return -RT_ERROR;
    }

    if (g_ai_2d_open_count == 0) {
        rt_mutex_release(&g_ai_2d_pm_mutex);
        rt_free(handle);
        file->data = RT_NULL;
        return -RT_EINVAL;
    }

    g_ai_2d_open_count--;
    if (g_ai_2d_open_count == 0) {
        rt_hw_interrupt_mask(IRQN_AI2D_INTERRUPT);
        ai_2d_clear_interrupt();
        pm_ret = k230_pm_put(K230_PM_DEV_AI);
        if (pm_ret == 0)
            g_ai_2d_pm_vote_held = RT_FALSE;
    } else {
        pm_ret = 0;
    }
    rt_mutex_release(&g_ai_2d_pm_mutex);

    rt_free(handle);
    file->data = RT_NULL;
    return pm_ret == 0 ? RT_EOK : pm_ret;
}

static int ai_2d_device_ioctl(struct dfs_fd *file, int cmd, void *args)
{
    (void)file;
    (void)cmd;
    (void)args;

    return -RT_EINVAL;
}

int ai_2d_device_poll(struct dfs_fd *file, struct rt_pollreq *req)
{
    struct ai_2d_dev_handle *handle;
    rt_uint32_t event;

    handle = (struct ai_2d_dev_handle*)file->data;
    if (!handle) {
        ai_2d_err("ai_2d_dev_handle NULL!");
        return -EINVAL;
    }
    rt_poll_add(handle->wait, req);
    if (rt_event_recv(&g_ai_2d_event, 0x01,
        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, &event) == RT_EOK)
        return POLLIN;

    return 0;
}

static const struct dfs_file_ops ai_2d_input_fops = {
    .open = ai_2d_device_open,
    .close = ai_2d_device_close,
    .ioctl = ai_2d_device_ioctl,
    .poll = ai_2d_device_poll,
};

static void irq_callback(int irq, void *data)
{
    rt_wqueue_t *wait = (rt_wqueue_t *)data;

    if(gnne_base_addr == RT_NULL) {
        ai_2d_err("ai2d interrupts while the hardware is not yet initialized\n");
        return;
    }
    ai_2d_clear_interrupt();
	rt_wqueue_wakeup(wait, (void*)POLLIN);
    rt_event_send(&g_ai_2d_event, 0x1);
}

int ai_2d_device_init(void)
{
    int ret = 0;
    rt_device_t ai_2d_device = &g_ai_2d_device;

    ret = rt_event_init(&g_ai_2d_event, "ai_2d_event", RT_IPC_FLAG_PRIO);
    if (ret) {
        ai_2d_err("event init failed\n");
        return -ENOMEM;
    }

    ret = rt_mutex_init(&g_ai_2d_pm_mutex, "ai2d_pm", RT_IPC_FLAG_PRIO);
    if (ret) {
        ai_2d_err("pm mutex init failed\n");
        rt_event_detach(&g_ai_2d_event);
        return -ENOMEM;
    }

    ret = rt_device_register(ai_2d_device, "ai_2d_device", RT_DEVICE_FLAG_RDWR);
    if(ret) {
        ai_2d_err("ai_2d_device register fail\n");
        rt_mutex_detach(&g_ai_2d_pm_mutex);
        rt_event_detach(&g_ai_2d_event);
        return ret;
    }

    rt_wqueue_init(&ai_2d_device->wait_queue);
    rt_hw_interrupt_install(IRQN_AI2D_INTERRUPT, irq_callback, &ai_2d_device->wait_queue, "ai_2d_irq");
    rt_hw_interrupt_mask(IRQN_AI2D_INTERRUPT);

    ai_2d_device->fops = &ai_2d_input_fops;

#ifndef RT_FASTBOOT
    if(!ret)
        ai_2d_info("%s OK\n", __func__);
#endif
    return ret;
}
