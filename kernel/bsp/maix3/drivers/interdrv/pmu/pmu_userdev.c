#include <stdint.h>

#include "pmu_priv.h"

#ifdef RT_USING_DFS
#include <dfs_file.h>
#endif

#ifdef RT_USING_LWP
#include <lwp_signal.h>
#include <lwp_user_mm.h>
#endif

static void pmu_notify_reset_locked(struct pmu_notify_state *notify)
{
    notify->pid = 0;
    notify->signo = 0;
    notify->pending_events = 0;
    notify->registered = false;
    notify->ack_pending = false;
}

void pmu_notify_unregister_pid(struct pmu_dev *pmu, rt_int32_t pid)
{
    struct pmu_notify_state *notify = &pmu->notify;
    rt_base_t level;

    if (pid <= 0)
        return;

    level = rt_hw_interrupt_disable();
    if (notify->registered && (notify->pid == pid))
        pmu_notify_reset_locked(notify);
    rt_hw_interrupt_enable(level);
}

static rt_err_t pmu_notify_register(struct pmu_dev *pmu,
                    const struct pmu_notify_cfg *cfg)
{
    struct pmu_notify_state *notify = &pmu->notify;
    rt_base_t level;
    rt_int32_t pid;
    rt_int32_t signo;

    pid = cfg->pid;
    if (pid <= 0)
        pid = pmu_current_pid();
    if (pid <= 0)
        return -RT_EINVAL;

    signo = cfg->signo;
    if (signo <= 0)
        signo = PMU_NOTIFY_DEFAULT_SIGNO;

    level = rt_hw_interrupt_disable();
    notify->pid = pid;
    notify->signo = signo;
    notify->pending_events = 0;
    notify->registered = true;
    notify->ack_pending = false;
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

static rt_err_t pmu_notify_read(struct pmu_dev *pmu, struct pmu_event *event)
{
    struct pmu_notify_state *notify = &pmu->notify;
    rt_base_t level;
    rt_int32_t pid;

    pid = pmu_current_pid();

    level = rt_hw_interrupt_disable();
    if (!notify->registered || ((pid > 0) && (notify->pid != pid))) {
        rt_hw_interrupt_enable(level);
        return -RT_ERROR;
    }

    event->events = notify->pending_events;
    event->reserved = 0;
    notify->pending_events = 0;
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

rt_err_t pmu_notify_send(struct pmu_dev *pmu, rt_uint32_t events, bool wait_ack)
{
#ifndef RT_USING_LWP
    return -RT_ENOSYS;
#else
    struct pmu_notify_state *notify = &pmu->notify;
    rt_base_t level;
    rt_int32_t pid;
    rt_int32_t signo;
    int ret;

    level = rt_hw_interrupt_disable();
    if (!notify->registered) {
        rt_hw_interrupt_enable(level);
        return -RT_ENOSYS;
    }

    if (wait_ack && notify->ack_pending) {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }

    pid = notify->pid;
    signo = notify->signo;
    notify->pending_events |= events;
    if (wait_ack)
        notify->ack_pending = true;
    rt_hw_interrupt_enable(level);

    ret = lwp_kill(pid, signo);
    if (ret < 0) {
        level = rt_hw_interrupt_disable();
        if (notify->pid == pid)
            pmu_notify_reset_locked(notify);
        rt_hw_interrupt_enable(level);

        rt_kprintf("[pmu] userdev: signal pid=%d signo=%d failed (%d)\n",
               pid, signo, ret);
        return ret;
    }

    return RT_EOK;
#endif
}

static rt_err_t pmu_notify_shutdown_ack(struct pmu_dev *pmu,
                    const struct pmu_event *event)
{
    struct pmu_notify_state *notify = &pmu->notify;
    rt_base_t level;
    rt_int32_t pid;

    pid = pmu_current_pid();
    if (pid <= 0)
        return -RT_EINVAL;

    level = rt_hw_interrupt_disable();
    if (!notify->registered || (notify->pid != pid)) {
        rt_hw_interrupt_enable(level);
        return -RT_ERROR;
    }

    if (!notify->ack_pending) {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }

    if ((event != RT_NULL) &&
        ((event->events & PMU_EVENT_KEY_RELEASE) == 0U)) {
        rt_hw_interrupt_enable(level);
        return -RT_EINVAL;
    }

    notify->ack_pending = false;
    notify->pending_events = 0;
    pmu->pwrkey.pressed = false;
    pmu->pwrkey.long_press_seen = false;
    pmu->pwrkey.release_seen = false;
    pmu->pwrkey.user_notified = false;
    rt_hw_interrupt_enable(level);

    pmu_do_shutdown("userspace-ack");
    return RT_EOK;
}

static rt_err_t pmu_copy_from_user(void *dst, void *src, size_t size)
{
    if ((dst == RT_NULL) || (src == RT_NULL))
        return -RT_EINVAL;

#if defined(RT_USING_LWP) && defined(RT_USING_USERSPACE)
    if (lwp_get_from_user_ex(dst, src, size) != 0)
        return -RT_ERROR;
#else
    rt_memcpy(dst, src, size);
#endif

    return RT_EOK;
}

static rt_err_t pmu_copy_to_user(void *dst, void *src, size_t size)
{
    if ((dst == RT_NULL) || (src == RT_NULL))
        return -RT_EINVAL;

#if defined(RT_USING_LWP) && defined(RT_USING_USERSPACE)
    if (lwp_put_to_user_ex(dst, src, size) != 0)
        return -RT_ERROR;
#else
    rt_memcpy(dst, src, size);
#endif

    return RT_EOK;
}

#ifdef RT_USING_DFS
static int pmu_userdev_open(struct dfs_fd *file)
{
    (void)file;
    return RT_EOK;
}

static int pmu_userdev_close(struct dfs_fd *file)
{
    (void)file;
    pmu_notify_unregister_pid(pmu_get_dev(), pmu_current_pid());
    return RT_EOK;
}

static int pmu_userdev_ioctl(struct dfs_fd *file, int cmd, void *args)
{
    struct pmu_dev *pmu = pmu_get_dev();
    struct pmu_notify_cfg notify_cfg;
    struct pmu_event event;
    struct pmu_power_cycle_cfg cycle_cfg;
    rt_err_t ret = -RT_EINVAL;

    (void)file;

    switch (cmd) {
    case PMU_IOCTL_REGISTER_NOTIFY:
        if (args == RT_NULL)
            goto out;
        if (pmu_copy_from_user(&notify_cfg, args,
                       sizeof(notify_cfg)) != RT_EOK) {
            ret = -RT_ERROR;
            goto out;
        }
        ret = pmu_notify_register(pmu, &notify_cfg);
        goto out;

    case PMU_IOCTL_UNREGISTER_NOTIFY:
        pmu_notify_unregister_pid(pmu, pmu_current_pid());
        ret = RT_EOK;
        goto out;

    case PMU_IOCTL_GET_EVENT:
        if (args == RT_NULL)
            goto out;
        ret = pmu_notify_read(pmu, &event);
        if (ret != RT_EOK)
            goto out;
        ret = pmu_copy_to_user(args, &event, sizeof(event));
        goto out;

    case PMU_IOCTL_SHUTDOWN_ACK:
        if (args == RT_NULL)
            goto out;
        if (pmu_copy_from_user(&event, args, sizeof(event)) != RT_EOK)
            ret = -RT_ERROR;
        else
            ret = pmu_notify_shutdown_ack(pmu, &event);
        goto out;

    case PMU_IOCTL_SCHEDULE_POWER_CYCLE:
        if (args == RT_NULL)
            goto out;
        if (pmu_copy_from_user(&cycle_cfg, args,
                       sizeof(cycle_cfg)) != RT_EOK) {
            ret = -RT_ERROR;
            goto out;
        }
        ret = pmu_schedule_power_cycle(pmu, &cycle_cfg);
        goto out;

    case PMU_IOCTL_CANCEL_POWER_CYCLE:
        ret = pmu_cancel_power_cycle(pmu);
        goto out;

    default:
        goto out;
    }

out:
    return ret;
}

static const struct dfs_file_ops g_pmu_userdev_fops = {
    .open = pmu_userdev_open,
    .close = pmu_userdev_close,
    .ioctl = pmu_userdev_ioctl,
};
#endif

int pmu_init_userdev(struct pmu_dev *pmu)
{
    struct pmu_notify_state *notify = &pmu->notify;
    rt_err_t ret;

    if (notify->initialized)
        return RT_EOK;

#ifndef RT_USING_DFS
    return RT_EOK;
#else
    notify->device.type = RT_Device_Class_Miscellaneous;
    notify->device.user_data = pmu;

    ret = rt_device_register(&notify->device, PMU_USERDEV_NAME,
                 RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK) {
        rt_kprintf("[pmu] userdev: register %s failed (%d)\n",
               PMU_USERDEV_NAME, ret);
        return ret;
    }

    notify->device.fops = &g_pmu_userdev_fops;
    pmu_notify_reset_locked(notify);
    notify->initialized = true;
    return RT_EOK;
#endif
}
