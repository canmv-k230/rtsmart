/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RT_ESP_HOSTED_RPC_H
#define RT_ESP_HOSTED_RPC_H

#include "esp_hosted_rpc.pb-c.h"

#include <rtthread.h>

#define ESP_HOSTED_RPC_MAX_REQUEST_SIZE  8160
#define ESP_HOSTED_RPC_MAX_RESPONSE_SIZE 8192
#define ESP_HOSTED_RPC_RESPONSE_ID(request_id) \
    ((RpcId)((request_id) + 0x100U))

typedef void (*esp_hosted_rpc_event_callback_t)(RpcId event_id,
                                                const uint8_t *payload,
                                                size_t payload_length,
                                                void *argument);

/* request and response are serialized inner messages from the generated
 * ESP-Hosted schema. response_length is always updated when non-NULL. */
rt_err_t rt_esp_hosted_rpc_call(RpcId request_id, const void *request,
                                size_t request_length, void *response,
                                size_t response_capacity,
                                size_t *response_length, int timeout_ms);
rt_err_t rt_esp_hosted_rpc_call_status(RpcId request_id, const void *request,
                                       size_t request_length, int timeout_ms);

/* One observer receives every RPC event after the RT-Smart WLAN wrapper.
 * The callback runs on a dedicated thread and payload is callback-scoped. */
rt_err_t rt_esp_hosted_rpc_set_event_callback(
    esp_hosted_rpc_event_callback_t callback, void *argument);

#endif /* RT_ESP_HOSTED_RPC_H */
