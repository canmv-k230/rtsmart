/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __K230_PM_UAPI_H__
#define __K230_PM_UAPI_H__

#include <stdint.h>
#include <stdbool.h>

#ifndef _IOC
#define _IOC(a, b, c, d) (((a) << 30) | ((b) << 8) | (c) | ((d) << 16))
#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U
#define _IO(a, b)       _IOC(_IOC_NONE, (a), (b), 0)
#define _IOW(a, b, c)   _IOC(_IOC_WRITE, (a), (b), sizeof(c))
#define _IOR(a, b, c)   _IOC(_IOC_READ, (a), (b), sizeof(c))
#define _IOWR(a, b, c)  _IOC(_IOC_READ | _IOC_WRITE, (a), (b), sizeof(c))
#endif

typedef enum {
    K230_PM_DOMAIN_CPU,
    K230_PM_DOMAIN_KPU,
    K230_PM_DOMAIN_DPU,
    K230_PM_DOMAIN_VPU,
    K230_PM_DOMAIN_DISPLAY,
    K230_PM_DOMAIN_MEDIA,
    K230_PM_DOMAIN_NR,
} k230_pm_uapi_domain;

typedef enum {
    K230_PM_GOVERNOR_MANUAL,
    K230_PM_GOVERNOR_PERFORMANCE,
    K230_PM_GOVERNOR_ENERGYSAVING,
    K230_PM_GOVERNOR_AUTO,
} k230_pm_uapi_governor;

typedef struct {
    int32_t freq;
    int32_t volt;
} k230_pm_uapi_profile;

#define K230_PM_MAX_PROFILE_COUNT 16U

struct k230_pm_uapi_domain_profiles {
    k230_pm_uapi_domain domain;
    uint32_t number;
    k230_pm_uapi_profile profiles[0];
};

struct k230_pm_uapi_domain_governor {
    k230_pm_uapi_domain domain;
    k230_pm_uapi_governor governor;
};

struct k230_pm_uapi_domain_profile {
    k230_pm_uapi_domain domain;
    int32_t index;
};

struct k230_pm_uapi_domain_thermal_protect {
    k230_pm_uapi_domain domain;
    int32_t temp; /* unit: C */
    int32_t index;
};

struct k230_pm_uapi_domain_bool {
    k230_pm_uapi_domain domain;
    bool enable;
};

enum {
    K230_IOC_NR_PM_RESERVED0,
    K230_IOC_NR_PM_RESERVED1,
    K230_IOC_NR_PM_GET_PROFILES,
    K230_IOC_NR_PM_GET_STAT,
    K230_IOC_NR_PM_SET_GOVERNOR,
    K230_IOC_NR_PM_GET_GOVERNOR,
    K230_IOC_NR_PM_SET_PROFILE,
    K230_IOC_NR_PM_GET_PROFILE,
    K230_IOC_NR_PM_SET_PROFILE_LOCK,
    K230_IOC_NR_PM_SET_PROFILE_UNLOCK,
    K230_IOC_NR_PM_SET_THERMAL_PROTECT,
    K230_IOC_NR_PM_GET_THERMAL_PROTECT,
    K230_IOC_NR_PM_SET_THERMAL_SHUTDOWN,
    K230_IOC_NR_PM_GET_THERMAL_SHUTDOWN,
    K230_IOC_NR_PM_SET_CLOCK,
    K230_IOC_NR_PM_SET_POWER,
};

#define K230_IOC_TYPE_PM 'p'

#define K230_IOC_PM_GET_PROFILES \
    _IOWR(K230_IOC_TYPE_PM, K230_IOC_NR_PM_GET_PROFILES, struct k230_pm_uapi_domain_profiles)
#define K230_IOC_PM_GET_STAT \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_GET_STAT, k230_pm_uapi_domain)
#define K230_IOC_PM_SET_GOVERNOR \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_SET_GOVERNOR, struct k230_pm_uapi_domain_governor)
#define K230_IOC_PM_GET_GOVERNOR \
    _IOWR(K230_IOC_TYPE_PM, K230_IOC_NR_PM_GET_GOVERNOR, struct k230_pm_uapi_domain_governor)
#define K230_IOC_PM_SET_PROFILE \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_SET_PROFILE, struct k230_pm_uapi_domain_profile)
#define K230_IOC_PM_GET_PROFILE \
    _IOWR(K230_IOC_TYPE_PM, K230_IOC_NR_PM_GET_PROFILE, struct k230_pm_uapi_domain_profile)
#define K230_IOC_PM_SET_PROFILE_LOCK \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_SET_PROFILE_LOCK, struct k230_pm_uapi_domain_profile)
#define K230_IOC_PM_SET_PROFILE_UNLOCK \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_SET_PROFILE_UNLOCK, struct k230_pm_uapi_domain_profile)
#define K230_IOC_PM_SET_THERMAL_PROTECT \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_SET_THERMAL_PROTECT, struct k230_pm_uapi_domain_thermal_protect)
#define K230_IOC_PM_GET_THERMAL_PROTECT \
    _IOWR(K230_IOC_TYPE_PM, K230_IOC_NR_PM_GET_THERMAL_PROTECT, struct k230_pm_uapi_domain_thermal_protect)
#define K230_IOC_PM_SET_THERMAL_SHUTDOWN \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_SET_THERMAL_SHUTDOWN, int32_t)
#define K230_IOC_PM_GET_THERMAL_SHUTDOWN \
    _IOWR(K230_IOC_TYPE_PM, K230_IOC_NR_PM_GET_THERMAL_SHUTDOWN, int32_t)
#define K230_IOC_PM_SET_CLOCK \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_SET_CLOCK, struct k230_pm_uapi_domain_bool)
#define K230_IOC_PM_SET_POWER \
    _IOW(K230_IOC_TYPE_PM, K230_IOC_NR_PM_SET_POWER, struct k230_pm_uapi_domain_bool)

#define K230_ERR_PM_ILLEGAL_PARAM 3
#define K230_ERR_PM_EXIST         4
#define K230_ERR_PM_UNEXIST       5
#define K230_ERR_PM_NULL_PTR      6
#define K230_ERR_PM_NOT_CONFIG    7
#define K230_ERR_PM_NOT_SUPPORT   8
#define K230_ERR_PM_NOT_PERM      9
#define K230_ERR_PM_NOMEM         12
#define K230_ERR_PM_NOTREADY      16
#define K230_ERR_PM_BADADDR       17
#define K230_ERR_PM_BUSY          18


#endif
