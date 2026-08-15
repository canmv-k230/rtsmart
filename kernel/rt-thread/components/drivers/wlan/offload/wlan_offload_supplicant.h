/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_WLAN_OFFLOAD_SUPPLICANT_H__
#define __RT_WLAN_OFFLOAD_SUPPLICANT_H__

#include <wlan_offload.h>

rt_bool_t rt_wlan_offload_supplicant_supports(rt_wlan_security_t security);
rt_err_t rt_wlan_offload_supplicant_prepare(
    struct rt_wlan_offload_radio *radio,
    struct rt_wlan_offload_connect_request *request,
    const rt_uint8_t *bss_ies, rt_size_t bss_ies_length);
void rt_wlan_offload_supplicant_cancel(struct rt_wlan_offload_radio *radio);
void rt_wlan_offload_supplicant_deinit(struct rt_wlan_offload_radio *radio);
rt_bool_t rt_wlan_offload_supplicant_filter_event(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_event *event);
rt_bool_t rt_wlan_offload_supplicant_handle_eapol(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    const rt_uint8_t source[6],
    const rt_uint8_t destination[6],
    const rt_uint8_t *data,
    rt_size_t length,
    rt_err_t *result);

#endif /* __RT_WLAN_OFFLOAD_SUPPLICANT_H__ */
