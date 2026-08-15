/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_WLAN_OFFLOAD_CONTROL_H__
#define __RT_WLAN_OFFLOAD_CONTROL_H__

#include <wlan_offload.h>

/* A NULL name assigns wlanctlN from the radio's phy index. */
rt_err_t rt_wlan_offload_control_register(struct rt_wlan_offload_radio *radio,
                                     const char *name);
rt_err_t rt_wlan_offload_control_get_name(
    const struct rt_wlan_offload_radio *radio, char *name, rt_size_t size);
rt_err_t rt_wlan_offload_control_unregister(struct rt_wlan_offload_radio *radio);
void rt_wlan_offload_control_report_event(struct rt_wlan_offload_radio *radio,
                                     const struct rt_wlan_offload_event *event);

#endif /* __RT_WLAN_OFFLOAD_CONTROL_H__ */
