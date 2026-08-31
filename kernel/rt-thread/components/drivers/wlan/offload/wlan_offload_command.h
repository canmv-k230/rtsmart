/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_WLAN_OFFLOAD_COMMAND_H__
#define __RT_WLAN_OFFLOAD_COMMAND_H__

#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_WLAN_OFFLOAD_COMMAND_RESPONSE_ANY 0xffffU

enum rt_wlan_offload_command_manager_state
{
    RT_WLAN_OFFLOAD_COMMAND_MANAGER_UNINITIALIZED = 0,
    RT_WLAN_OFFLOAD_COMMAND_MANAGER_READY,
    RT_WLAN_OFFLOAD_COMMAND_MANAGER_FAILED,
};

struct rt_wlan_offload_command_manager;

/*
 * push() borrows request until it returns. It must encode token in protocols
 * which can have more than one command awaiting the same confirmation ID.
 */
typedef rt_err_t (*rt_wlan_offload_command_push_t)(
    struct rt_wlan_offload_command_manager *manager,
    rt_uint32_t token, rt_uint16_t command_id,
    const void *request, rt_size_t request_length,
    void *driver_data);

struct rt_wlan_offload_command_manager_config
{
    rt_uint16_t max_pending;
    rt_wlan_offload_command_push_t push;
    void *driver_data;
};

struct rt_wlan_offload_command_manager
{
    enum rt_wlan_offload_command_manager_state state;
    struct rt_mutex lock;
    rt_list_t pending;
    rt_uint32_t next_token;
    rt_uint16_t pending_count;
    rt_uint16_t max_pending;
    rt_uint16_t active_pushes;
    rt_bool_t deinitializing;
    struct rt_completion idle;
    rt_wlan_offload_command_push_t push;
    void *driver_data;
};

rt_err_t rt_wlan_offload_command_manager_init(
    struct rt_wlan_offload_command_manager *manager,
    const struct rt_wlan_offload_command_manager_config *config);
rt_err_t rt_wlan_offload_command_manager_deinit(
    struct rt_wlan_offload_command_manager *manager);

/*
 * Execute one firmware transaction. A confirmation copies at most
 * response_capacity bytes, while response_length reports the firmware's full
 * response size. The call returns -RT_EFULL when truncation occurred.
 */
rt_err_t rt_wlan_offload_command_execute(
    struct rt_wlan_offload_command_manager *manager,
    rt_uint16_t command_id, rt_uint16_t confirmation_id,
    const void *request, rt_size_t request_length,
    void *response, rt_size_t response_capacity,
    rt_size_t *response_length, rt_int32_t timeout,
    rt_uint32_t *token);

/* Called by the vendor protocol RX worker, never from a hard interrupt. */
rt_err_t rt_wlan_offload_command_complete(
    struct rt_wlan_offload_command_manager *manager,
    rt_uint32_t token, rt_uint16_t confirmation_id,
    rt_err_t status, const void *response, rt_size_t response_length);

/* Used by single-threaded transports which must service RX while push() is
 * executing from their receive callback. A zero token tests for any pending
 * transaction. */
rt_bool_t rt_wlan_offload_command_is_pending(
    struct rt_wlan_offload_command_manager *manager, rt_uint32_t token);

/* Fail and wake every pending transaction, then reject new commands. */
rt_err_t rt_wlan_offload_command_manager_fail(
    struct rt_wlan_offload_command_manager *manager, rt_err_t status);

/* Return a failed manager to READY after the vendor has recovered firmware. */
rt_err_t rt_wlan_offload_command_manager_reset(
    struct rt_wlan_offload_command_manager *manager);

void *rt_wlan_offload_command_get_driver_data(
    struct rt_wlan_offload_command_manager *manager);

#ifdef __cplusplus
}
#endif

#endif /* __RT_WLAN_OFFLOAD_COMMAND_H__ */
