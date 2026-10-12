/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_file.h>
#include <lwp_user_mm.h>
#include "k230_pm_dvfs.h"

#define COPY_FROM_USER(dst, src, size) \
    ((size) != lwp_get_from_user((dst), (src), (size)))
#define COPY_TO_USER(dst, src, size) \
    ((size) != lwp_put_to_user((dst), (src), (size)))

struct k230_pm_uapi_dev {
    struct rt_device parent;
    struct rt_mutex mutex;
};

static struct k230_pm_uapi_dev pm_device;

static int k230_pm_uapi_get_stat(void *args)
{
    k230_pm_uapi_domain domain;

    if (COPY_FROM_USER(&domain, args, sizeof(domain)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return k230_pm_dvfs_get_stat(domain);
}

static int k230_pm_uapi_get_profiles(void *args)
{
    struct k230_pm_uapi_domain_profiles profiles;
    k230_pm_uapi_profile copy_profiles[K230_PM_MAX_PROFILE_COUNT];
    uint32_t requested_number;
    uint32_t number;
    int ret;

    if (COPY_FROM_USER(&profiles, args, sizeof(profiles)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    requested_number = profiles.number;
    number = requested_number;
    if (number > K230_PM_MAX_PROFILE_COUNT)
        number = K230_PM_MAX_PROFILE_COUNT;

    ret = k230_pm_dvfs_get_profiles(profiles.domain, &number, copy_profiles);
    if (ret == 0) {
        profiles.number = number;
        if (COPY_TO_USER(args, &profiles, sizeof(profiles)))
            ret = K230_ERR_PM_ILLEGAL_PARAM;
        else if (requested_number != 0 && number != 0 && COPY_TO_USER(
            ((struct k230_pm_uapi_domain_profiles *)args)->profiles,
            copy_profiles, sizeof(copy_profiles[0]) * number))
            ret = K230_ERR_PM_ILLEGAL_PARAM;
    }

    return ret;
}

static int k230_pm_uapi_set_governor(void *args)
{
    struct k230_pm_uapi_domain_governor governor;

    if (COPY_FROM_USER(&governor, args, sizeof(governor)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return k230_pm_dvfs_set_governor(governor.domain, governor.governor);
}

static int k230_pm_uapi_get_governor(void *args)
{
    struct k230_pm_uapi_domain_governor governor;
    int ret;

    if (COPY_FROM_USER(&governor, args, sizeof(governor)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    ret = k230_pm_dvfs_get_governor(governor.domain, &governor.governor);
    if (ret != 0)
        return ret;

    if (COPY_TO_USER(args, &governor, sizeof(governor)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return 0;
}

static int k230_pm_uapi_set_profile(void *args)
{
    struct k230_pm_uapi_domain_profile profile;
    int ret;

    if (COPY_FROM_USER(&profile, args, sizeof(profile)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    ret = k230_pm_dvfs_set_profile(profile.domain, profile.index);
    if (ret != 0)
        return ret;

    if (COPY_TO_USER(args, &profile, sizeof(profile)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return 0;
}

static int k230_pm_uapi_get_profile(void *args)
{
    struct k230_pm_uapi_domain_profile profile;
    int ret;

    if (COPY_FROM_USER(&profile, args, sizeof(profile)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    ret = k230_pm_dvfs_get_profile(profile.domain, &profile.index);
    if (ret != 0)
        return ret;

    if (COPY_TO_USER(args, &profile, sizeof(profile)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return 0;
}

static int k230_pm_uapi_set_profile_lock(void *args)
{
    struct k230_pm_uapi_domain_profile profile;

    if (COPY_FROM_USER(&profile, args, sizeof(profile)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return k230_pm_dvfs_set_profile_lock(profile.domain, profile.index);
}

static int k230_pm_uapi_set_profile_unlock(void *args)
{
    struct k230_pm_uapi_domain_profile profile;

    if (COPY_FROM_USER(&profile, args, sizeof(profile)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return k230_pm_dvfs_set_profile_unlock(profile.domain, profile.index);
}

static int k230_pm_uapi_set_thermal_protect(void *args)
{
    struct k230_pm_uapi_domain_thermal_protect thermal;

    if (COPY_FROM_USER(&thermal, args, sizeof(thermal)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return k230_pm_dvfs_set_thermal_protect(thermal.domain, thermal.temp,
        thermal.index);
}

static int k230_pm_uapi_get_thermal_protect(void *args)
{
    struct k230_pm_uapi_domain_thermal_protect thermal;
    int ret;

    if (COPY_FROM_USER(&thermal, args, sizeof(thermal)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    ret = k230_pm_dvfs_get_thermal_protect(thermal.domain, &thermal.temp,
        &thermal.index);
    if (ret != 0)
        return ret;

    if (COPY_TO_USER(args, &thermal, sizeof(thermal)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return 0;
}

static int k230_pm_uapi_set_thermal_shutdown(void *args)
{
    int32_t temp;

    if (COPY_FROM_USER(&temp, args, sizeof(temp)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return k230_pm_dvfs_set_thermal_shutdown(temp);
}

static int k230_pm_uapi_get_thermal_shutdown(void *args)
{
    int32_t temp;
    int ret;

    if (COPY_FROM_USER(&temp, args, sizeof(temp)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    ret = k230_pm_dvfs_get_thermal_shutdown(&temp);
    if (ret != 0)
        return ret;

    if (COPY_TO_USER(args, &temp, sizeof(temp)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return 0;
}

static int k230_pm_uapi_set_clock(void *args)
{
    struct k230_pm_uapi_domain_bool enable;

    if (COPY_FROM_USER(&enable, args, sizeof(enable)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return k230_pm_dvfs_set_clock(enable.domain, enable.enable);
}

static int k230_pm_uapi_set_power(void *args)
{
    struct k230_pm_uapi_domain_bool enable;

    if (COPY_FROM_USER(&enable, args, sizeof(enable)))
        return K230_ERR_PM_ILLEGAL_PARAM;

    return k230_pm_dvfs_set_power(enable.domain, enable.enable);
}

static int k230_pm_uapi_open(struct dfs_fd *file)
{
    (void)file;
    return 0;
}

static int k230_pm_uapi_close(struct dfs_fd *file)
{
    (void)file;
    return 0;
}

static int k230_pm_uapi_ioctl(struct dfs_fd *file, int cmd, void *args)
{
    int ret;

    (void)file;

    rt_mutex_take(&pm_device.mutex, RT_WAITING_FOREVER);
    switch (cmd) {
    case K230_IOC_PM_GET_PROFILES:
        ret = k230_pm_uapi_get_profiles(args);
        break;
    case K230_IOC_PM_GET_STAT:
        ret = k230_pm_uapi_get_stat(args);
        break;
    case K230_IOC_PM_SET_GOVERNOR:
        ret = k230_pm_uapi_set_governor(args);
        break;
    case K230_IOC_PM_GET_GOVERNOR:
        ret = k230_pm_uapi_get_governor(args);
        break;
    case K230_IOC_PM_SET_PROFILE:
        ret = k230_pm_uapi_set_profile(args);
        break;
    case K230_IOC_PM_GET_PROFILE:
        ret = k230_pm_uapi_get_profile(args);
        break;
    case K230_IOC_PM_SET_PROFILE_LOCK:
        ret = k230_pm_uapi_set_profile_lock(args);
        break;
    case K230_IOC_PM_SET_PROFILE_UNLOCK:
        ret = k230_pm_uapi_set_profile_unlock(args);
        break;
    case K230_IOC_PM_SET_THERMAL_PROTECT:
        ret = k230_pm_uapi_set_thermal_protect(args);
        break;
    case K230_IOC_PM_GET_THERMAL_PROTECT:
        ret = k230_pm_uapi_get_thermal_protect(args);
        break;
    case K230_IOC_PM_SET_THERMAL_SHUTDOWN:
        ret = k230_pm_uapi_set_thermal_shutdown(args);
        break;
    case K230_IOC_PM_GET_THERMAL_SHUTDOWN:
        ret = k230_pm_uapi_get_thermal_shutdown(args);
        break;
    case K230_IOC_PM_SET_CLOCK:
        ret = k230_pm_uapi_set_clock(args);
        break;
    case K230_IOC_PM_SET_POWER:
        ret = k230_pm_uapi_set_power(args);
        break;
    default:
        ret = -EINVAL;
        break;
    }
    rt_mutex_release(&pm_device.mutex);

    return ret;
}

static const struct dfs_file_ops k230_pm_uapi_fops = {
    .open = k230_pm_uapi_open,
    .close = k230_pm_uapi_close,
    .ioctl = k230_pm_uapi_ioctl,
};

static int k230_pm_dvfs_uapi_init(void)
{
    int ret;

    ret = k230_pm_dvfs_init();
    if (ret != 0)
        return ret;

    ret = rt_mutex_init(&pm_device.mutex, "pm_mutex", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK) {
        return ret;
    }

    ret = rt_device_register(&pm_device.parent, "pm", RT_DEVICE_FLAG_RDWR);
    if (ret != 0) {
        rt_kprintf("k230_pm_dvfs: /dev/pm register failed: %d\n", ret);
        rt_mutex_detach(&pm_device.mutex);
        return ret;
    }
    pm_device.parent.fops = &k230_pm_uapi_fops;

    return 0;
}
INIT_APP_EXPORT(k230_pm_dvfs_uapi_init);
