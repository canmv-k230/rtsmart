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
#include "io.h"
#include <ioremap.h>
#include "board.h"
#include "k230_pm.h"

#ifdef RT_USING_POSIX
#include <dfs_posix.h>
#include <dfs_poll.h>
#include <posix_termios.h>
#endif

#define GNNE_LOG_ENABLE
struct gnne_dev_handle {
    rt_wqueue_t *wait;
};


#ifdef  GNNE_LOG_ENABLE
#define gnne_log(fmt, ...)    rt_kprintf(fmt, ##__VA_ARGS__)
#else
#define gnne_log(fmt, ...)
#endif

#define gnne_info(s...)    do { \
    gnne_log("<gnne> "); \
    gnne_log(s); \
    gnne_log("\r\n"); \
} while (0)

#define gnne_err(s...) do { \
    gnne_log("<err>[%s:%d] ", __func__, __LINE__); \
    gnne_log(s); \
    gnne_log("\r\n"); \
} while (0)

#define GNNE_CMD_LOCK       0
#define GNNE_CMD_TRYLOCK    1
#define GNNE_CMD_UNLOCK     2
#define GNNE_LOCK_RELEASED  0x01U
#define IRQN_GNNE_INTERRUPT (16 + 173)

static struct rt_device g_gnne_device = {0};
static struct rt_event g_gnne_event = {0};
static struct rt_event g_gnne_lock_event = {0};
static struct rt_mutex g_gnne_lock_mutex = {0};
static struct rt_mutex g_gnne_pm_mutex = {0};
static struct gnne_dev_handle *g_gnne_lock_owner = RT_NULL;
static rt_uint32_t g_gnne_open_count;
static rt_bool_t g_gnne_pm_vote_held;
void *gnne_base_addr = RT_NULL;

#define KPU_DATA_SET_REG_ADDR     0x91301d8cU
#define KPU_DATA_BW_REG_ADDR      0x91301d90U
#define KPU_DATA_REG_PAGE_BASE    (KPU_DATA_SET_REG_ADDR & ~RT_MM_PAGE_MASK)
#define KPU_DATA_SET_REG_OFFSET   (KPU_DATA_SET_REG_ADDR - KPU_DATA_REG_PAGE_BASE)
#define KPU_DATA_BW_REG_OFFSET    (KPU_DATA_BW_REG_ADDR - KPU_DATA_REG_PAGE_BASE)
#define KPU_DATA_SET_ENABLE       0x1U
#define KPU_DATA_BW_VALUE         0x0a00U

static int gnne_setup_kpu_bandwidth(void)
{
#if 0
    volatile char *reg_page;
    volatile rt_uint32_t *reg_kpu_data_set;
    volatile rt_uint32_t *reg_kpu_data_bw;

    reg_page = (volatile char *)rt_ioremap((void *)KPU_DATA_REG_PAGE_BASE, RT_MM_PAGE_SIZE);
    if (reg_page == RT_NULL) {
        gnne_err("map kpu bandwidth register page 0x%08x failed", KPU_DATA_REG_PAGE_BASE);
        return -RT_ENOMEM;
    }

    reg_kpu_data_set = (volatile rt_uint32_t *)(reg_page + KPU_DATA_SET_REG_OFFSET);
    reg_kpu_data_bw = (volatile rt_uint32_t *)(reg_page + KPU_DATA_BW_REG_OFFSET);

    writel(KPU_DATA_SET_ENABLE, reg_kpu_data_set);
    writel(KPU_DATA_BW_VALUE, reg_kpu_data_bw);
    rt_iounmap((void *)reg_page);
#else
    writel(KPU_DATA_SET_ENABLE, (void *)KPU_DATA_SET_REG_ADDR);
    writel(KPU_DATA_BW_VALUE, (void *)KPU_DATA_BW_REG_ADDR);
#endif

    return RT_EOK;
}

static void gnne_clear_interrupt(void)
{
    volatile void *write_addr;

    if (gnne_base_addr == RT_NULL)
        return;

    write_addr = (void *)((char *)gnne_base_addr + 0x128);
    __iowmb();
    *(rt_uint64_t *)write_addr = 0x400000004;
}

static int gnne_device_lock(struct gnne_dev_handle *handle, rt_bool_t wait)
{
    rt_uint32_t event;
    rt_err_t ret;

    if (handle == RT_NULL)
        return -RT_EINVAL;

    while (1) {
        ret = rt_mutex_take(&g_gnne_lock_mutex, RT_WAITING_FOREVER);
        if (ret != RT_EOK)
            return -RT_ERROR;

        if (g_gnne_lock_owner == RT_NULL) {
            g_gnne_lock_owner = handle;
            rt_mutex_release(&g_gnne_lock_mutex);
            return 0;
        }

        if (g_gnne_lock_owner == handle) {
            rt_mutex_release(&g_gnne_lock_mutex);
            return 0;
        }

        rt_mutex_release(&g_gnne_lock_mutex);
        if (!wait)
            return -RT_EBUSY;

        ret = rt_event_recv(&g_gnne_lock_event, GNNE_LOCK_RELEASED,
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &event);
        if (ret != RT_EOK)
            return -RT_ERROR;
    }
}

static int gnne_device_unlock(struct gnne_dev_handle *handle)
{
    rt_bool_t released = RT_FALSE;
    rt_err_t ret;

    if (handle == RT_NULL)
        return -RT_EINVAL;

    ret = rt_mutex_take(&g_gnne_lock_mutex, RT_WAITING_FOREVER);
    if (ret != RT_EOK)
        return -RT_ERROR;

    if (g_gnne_lock_owner == handle) {
        g_gnne_lock_owner = RT_NULL;
        released = RT_TRUE;
    }
    rt_mutex_release(&g_gnne_lock_mutex);

    if (released)
        rt_event_send(&g_gnne_lock_event, GNNE_LOCK_RELEASED);

    return 0;
}

static int gnne_device_open(struct dfs_fd *file)
{
    struct gnne_dev_handle *handle;
    rt_device_t device;
    int ret;

    handle = rt_malloc(sizeof(struct gnne_dev_handle));
    if(handle == RT_NULL) {
        gnne_err("malloc failed\n");
        return -1;
    }

    ret = rt_mutex_take(&g_gnne_pm_mutex, RT_WAITING_FOREVER);
    if (ret != RT_EOK) {
        rt_free(handle);
        return -RT_ERROR;
    }

    if (g_gnne_open_count == 0) {
        if (g_gnne_pm_vote_held) {
            ret = k230_pm_put(K230_PM_DEV_AI);
            if (ret != 0) {
                rt_mutex_release(&g_gnne_pm_mutex);
                rt_free(handle);
                return ret;
            }
            g_gnne_pm_vote_held = RT_FALSE;
        }

        ret = k230_pm_get(K230_PM_DEV_AI);
        if (ret != 0) {
            rt_mutex_release(&g_gnne_pm_mutex);
            rt_free(handle);
            return ret;
        }
        g_gnne_pm_vote_held = RT_TRUE;

        ret = gnne_setup_kpu_bandwidth();
        if (ret != RT_EOK) {
            if (k230_pm_put(K230_PM_DEV_AI) == 0)
                g_gnne_pm_vote_held = RT_FALSE;
            rt_mutex_release(&g_gnne_pm_mutex);
            rt_free(handle);
            return ret;
        }
        gnne_clear_interrupt();
        rt_event_recv(&g_gnne_event, 0x01,
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, RT_NULL);
        rt_hw_interrupt_umask(IRQN_GNNE_INTERRUPT);
    }
    g_gnne_open_count++;
    rt_mutex_release(&g_gnne_pm_mutex);

    device = (rt_device_t)file->fnode->data;
    handle->wait = &device->wait_queue;
    file->data = (void*)handle;
    return RT_EOK;
}

static int gnne_device_close(struct dfs_fd *file)
{
    struct gnne_dev_handle *handle;
    int ret;
    int pm_ret;

    handle = (struct gnne_dev_handle *)file->data;
    if(handle == RT_NULL) {
        gnne_err("try to close a invalid handle");
        return -RT_EINVAL;
    }
    gnne_device_unlock(handle);

    ret = rt_mutex_take(&g_gnne_pm_mutex, RT_WAITING_FOREVER);
    if (ret != RT_EOK) {
        rt_free(handle);
        file->data = RT_NULL;
        return -RT_ERROR;
    }

    if (g_gnne_open_count == 0) {
        rt_mutex_release(&g_gnne_pm_mutex);
        rt_free(handle);
        file->data = RT_NULL;
        return -RT_EINVAL;
    }

    g_gnne_open_count--;
    if (g_gnne_open_count == 0) {
        rt_hw_interrupt_mask(IRQN_GNNE_INTERRUPT);
        gnne_clear_interrupt();
        pm_ret = k230_pm_put(K230_PM_DEV_AI);
        if (pm_ret == 0)
            g_gnne_pm_vote_held = RT_FALSE;
    } else {
        pm_ret = 0;
    }
    rt_mutex_release(&g_gnne_pm_mutex);

    rt_free(handle);
    file->data = RT_NULL;
    return pm_ret == 0 ? RT_EOK : pm_ret;
}

static int gnne_device_ioctl(struct dfs_fd *file, int cmd, void *args)
{
    struct gnne_dev_handle *handle;
    int ret;

    (void)args;
    handle = (struct gnne_dev_handle *)file->data;
    if (handle == RT_NULL)
        return -RT_EINVAL;

    switch (cmd) {
    case GNNE_CMD_LOCK:
        return gnne_device_lock(handle, RT_TRUE);
    case GNNE_CMD_UNLOCK:
        return gnne_device_unlock(handle);
    case GNNE_CMD_TRYLOCK:
        ret = gnne_device_lock(handle, RT_FALSE);
        return (ret == 0) ? 0 : -1;
    default:
        return -RT_EINVAL;
    }
}

int gnne_device_poll(struct dfs_fd *file, struct rt_pollreq *req)
{
    struct gnne_dev_handle *handle;
    rt_uint32_t event;

    handle = (struct gnne_dev_handle*)file->data;
    if (!handle) {
        gnne_err("gnne_dev_handle NULL!");
        return -EINVAL;
    }
    rt_poll_add(handle->wait, req);
    if (rt_event_recv(&g_gnne_event, 0x01,
        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, &event) == RT_EOK)
        return POLLIN;

    return 0;
}

static const struct dfs_file_ops gnne_input_fops = {
    .open = gnne_device_open,
    .close = gnne_device_close,
    .ioctl = gnne_device_ioctl,
    .poll = gnne_device_poll,
};

static void irq_callback(int irq, void *data)
{
    rt_wqueue_t *wait = (rt_wqueue_t *)data;

    if(gnne_base_addr == RT_NULL) {
        gnne_err("gnne interrupts while the hardware is not yet initialized\n");
        return;
    }

    gnne_clear_interrupt();
    rt_wqueue_wakeup(wait, (void*)POLLIN);
    rt_event_send(&g_gnne_event, 0x1);
}

int gnne_device_init(void)
{
    int ret = 0;
    rt_device_t gnne_device = &g_gnne_device;

    ret = rt_event_init(&g_gnne_event, "gnne_event", RT_IPC_FLAG_PRIO);
    if (ret) {
        gnne_err("event init failed\n");
        return -ENOMEM;
    }

    ret = rt_event_init(&g_gnne_lock_event, "gnne_lkev", RT_IPC_FLAG_PRIO);
    if (ret) {
        gnne_err("lock event init failed\n");
        rt_event_detach(&g_gnne_event);
        return -ENOMEM;
    }

    ret = rt_mutex_init(&g_gnne_lock_mutex, "gnne_lk", RT_IPC_FLAG_PRIO);
    if (ret) {
        gnne_err("lock mutex init failed\n");
        rt_event_detach(&g_gnne_lock_event);
        rt_event_detach(&g_gnne_event);
        return -ENOMEM;
    }

    ret = rt_mutex_init(&g_gnne_pm_mutex, "gnne_pm", RT_IPC_FLAG_PRIO);
    if (ret) {
        gnne_err("pm mutex init failed\n");
        rt_mutex_detach(&g_gnne_lock_mutex);
        rt_event_detach(&g_gnne_lock_event);
        rt_event_detach(&g_gnne_event);
        return -ENOMEM;
    }

    ret = rt_device_register(gnne_device, "gnne_device", RT_DEVICE_FLAG_RDWR);
    if(ret) {
        gnne_err("gnne_device register fail\n");
        goto err_ipc_detach;
    }


    /*rt_ioremap maps the size of at least one page*/
    gnne_base_addr = rt_ioremap((void *)KPU_BASE_ADDR, (KPU_IO_SIZE + FFT_IO_SIZE + AI2D_IO_SIZE));
    if(gnne_base_addr == RT_NULL) {
        gnne_err("gnne ioremap error\n");
        ret = -RT_ENOMEM;
        goto err_device_unregister;
    }

    rt_wqueue_init(&gnne_device->wait_queue);
    rt_hw_interrupt_install(IRQN_GNNE_INTERRUPT, irq_callback, &gnne_device->wait_queue, "gnne_irq");
    rt_hw_interrupt_mask(IRQN_GNNE_INTERRUPT);

    gnne_device->fops = &gnne_input_fops;

#ifndef RT_FASTBOOT
    gnne_info("%s done.\n", __func__);
#endif

    return ret;

err_device_unregister:
    rt_device_unregister(gnne_device);
err_ipc_detach:
    rt_mutex_detach(&g_gnne_pm_mutex);
    rt_mutex_detach(&g_gnne_lock_mutex);
    rt_event_detach(&g_gnne_lock_event);
    rt_event_detach(&g_gnne_event);
    return ret;
}
