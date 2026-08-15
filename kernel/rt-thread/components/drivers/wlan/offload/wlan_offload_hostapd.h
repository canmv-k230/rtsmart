/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_WLAN_OFFLOAD_HOSTAPD_H__
#define __RT_WLAN_OFFLOAD_HOSTAPD_H__

#include <wlan_offload.h>

rt_bool_t rt_wlan_offload_hostapd_supports(rt_wlan_security_t security);
rt_err_t rt_wlan_offload_hostapd_prepare(
    struct rt_wlan_offload_radio *radio,
    struct rt_wlan_offload_ap_settings *settings);
void rt_wlan_offload_hostapd_cancel(struct rt_wlan_offload_radio *radio);
void rt_wlan_offload_hostapd_deinit(struct rt_wlan_offload_radio *radio);
rt_err_t rt_wlan_offload_hostapd_deauth(
    struct rt_wlan_offload_radio *radio, const rt_uint8_t mac[6],
    rt_uint16_t reason);
rt_err_t rt_wlan_offload_hostapd_channel_changed(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_channel_definition *channel);
rt_bool_t rt_wlan_offload_hostapd_filter_event(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_event *event);
rt_bool_t rt_wlan_offload_hostapd_handle_eapol(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    const rt_uint8_t source[6], const rt_uint8_t destination[6],
    const rt_uint8_t *data, rt_size_t length, rt_err_t *result);

#endif /* __RT_WLAN_OFFLOAD_HOSTAPD_H__ */
