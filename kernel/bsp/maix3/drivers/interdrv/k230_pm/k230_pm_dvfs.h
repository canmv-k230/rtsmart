/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __K230_PM_DVFS_H__
#define __K230_PM_DVFS_H__

#include <stdbool.h>
#include <stdint.h>
#include "k230_pm_uapi.h"

int k230_pm_dvfs_init(void);
int k230_pm_dvfs_get_stat(k230_pm_uapi_domain domain);
int k230_pm_dvfs_get_profiles(k230_pm_uapi_domain domain, uint32_t *number,
    k230_pm_uapi_profile *profiles);
int k230_pm_dvfs_set_governor(k230_pm_uapi_domain domain,
    k230_pm_uapi_governor governor);
int k230_pm_dvfs_get_governor(k230_pm_uapi_domain domain,
    k230_pm_uapi_governor *governor);
int k230_pm_dvfs_set_profile(k230_pm_uapi_domain domain, int32_t index);
int k230_pm_dvfs_get_profile(k230_pm_uapi_domain domain, int32_t *index);
int k230_pm_dvfs_set_profile_lock(k230_pm_uapi_domain domain, int32_t index);
int k230_pm_dvfs_set_profile_unlock(k230_pm_uapi_domain domain, int32_t index);
int k230_pm_dvfs_set_thermal_protect(k230_pm_uapi_domain domain,
    int32_t temp, int32_t index);
int k230_pm_dvfs_get_thermal_protect(k230_pm_uapi_domain domain,
    int32_t *temp, int32_t *index);
int k230_pm_dvfs_set_thermal_shutdown(int32_t temp);
int k230_pm_dvfs_get_thermal_shutdown(int32_t *temp);
int k230_pm_dvfs_set_clock(k230_pm_uapi_domain domain, bool enable);
int k230_pm_dvfs_set_power(k230_pm_uapi_domain domain, bool enable);
void k230_pm_dvfs_thermal_update(k230_pm_uapi_domain domain, int32_t temp);
int32_t k230_pm_dvfs_thermal_shutdown_temp(void);
void k230_pm_dvfs_dump(void);

#ifdef K230_PM_THERMAL_PROTECT
int k230_pm_thermal_init(void);
#else
static inline int k230_pm_thermal_init(void)
{
    return 0;
}
#endif

#endif
