/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __ESP_HOSTED_WIFI_H__
#define __ESP_HOSTED_WIFI_H__

#include <wlan_offload.h>
#include <wlan_offload_command.h>
#include <ipc/completion.h>

#include "esp_hosted_protocol.h"

#if defined(ESP_HOSTED_WIFI_LOG_LEVEL_DEBUG)
#define ESP_HOSTED_WIFI_DBG_LVL DBG_LOG
#elif defined(ESP_HOSTED_WIFI_LOG_LEVEL_INFO)
#define ESP_HOSTED_WIFI_DBG_LVL DBG_INFO
#elif defined(ESP_HOSTED_WIFI_LOG_LEVEL_WARNING)
#define ESP_HOSTED_WIFI_DBG_LVL DBG_WARNING
#elif defined(ESP_HOSTED_WIFI_LOG_LEVEL_ERROR)
#define ESP_HOSTED_WIFI_DBG_LVL DBG_ERROR
#else
#define ESP_HOSTED_WIFI_DBG_LVL DBG_INFO
#endif

#ifndef ESP_HOSTED_WIFI_SDIO_TOKEN_SIZE
#define ESP_HOSTED_WIFI_SDIO_TOKEN_SIZE 15872
#endif

struct ehf_context;

struct ehf_protocol_ops
{
    const char *name;
    rt_uint32_t capabilities;
    rt_uint16_t max_pending_commands;
    const struct rt_wlan_offload_ops *wlan_offload_ops;
    rt_wlan_offload_command_push_t command_push;
    rt_err_t (*receive)(struct ehf_context *context, const void *data,
                        rt_size_t length);
    void (*reset)(struct ehf_context *context);
};

struct ehf_context
{
    struct rt_wlan_offload_radio radio;
    struct rt_wlan_offload_bus *bus;
    struct rt_wlan_offload_command_manager commands;
    struct rt_completion boot_completion;
#ifdef ESP_HOSTED_WIFI_AUTO_START
    struct rt_completion auto_start_completion;
    rt_thread_t auto_start_thread;
    volatile rt_bool_t auto_start_cancelled;
#endif
    const struct ehf_protocol_ops *protocol;
    rt_bool_t attached;
    rt_bool_t booted;
    rt_bool_t checksum_enabled;
    rt_bool_t sta_enabled;
    rt_bool_t ap_enabled;
    rt_bool_t sta_connected;
    rt_bool_t ap_started;
    rt_uint8_t firmware_capabilities;
    rt_uint8_t chip_id;
    rt_uint8_t firmware_version[8];
    rt_uint32_t sdio_token_size;
    rt_uint16_t tx_sequence;
    rt_uint32_t scan_request_id;
    rt_uint32_t connect_request_id;
#ifdef ESP_HOSTED_WIFI_FG
    struct rt_timer connect_timer;
    rt_bool_t connect_timer_initialized;
    rt_uint16_t connect_retry_count;
#endif
    rt_uint32_t auth_request_id;
    rt_uint32_t assoc_request_id;
    rt_uint8_t command_interface;
    rt_uint8_t sta_bssid[6];
    rt_uint8_t *reassembly;
    rt_size_t reassembly_length;
    rt_uint16_t reassembly_sequence;
    rt_bool_t reassembly_active;
    rt_uint8_t invalid_rx_log_count;
};

#ifdef ESP_HOSTED_WIFI_FG
extern const struct ehf_protocol_ops g_ehf_fg_protocol;
#else
extern const struct ehf_protocol_ops g_ehf_ng_protocol;
#endif

struct ehf_context *ehf_context_from_radio(struct rt_wlan_offload_radio *radio);
struct ehf_context *ehf_context_from_vif(struct rt_wlan_offload_vif *vif);

rt_err_t ehf_attach_bus(struct rt_wlan_offload_bus *bus);
void ehf_detach_bus(struct rt_wlan_offload_bus *bus);
void ehf_bus_prepare(struct rt_wlan_offload_bus *bus);
void ehf_boot_ready(struct ehf_context *context, rt_uint8_t capabilities,
                    rt_uint8_t chip_id, const rt_uint8_t version[8]);
rt_err_t ehf_wait_for_boot(struct ehf_context *context);

rt_err_t ehf_spi_driver_init(void);
rt_err_t ehf_sdio_driver_init(void);

#endif /* __ESP_HOSTED_WIFI_H__ */
