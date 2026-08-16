/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RT_ESP_HOSTED_RPC_INTERNAL_H
#define RT_ESP_HOSTED_RPC_INTERNAL_H

#include "esp_hosted_rpc.h"
#include "esp_hosted_rpc.pb-c.h"

typedef void (*esp_hosted_rpc_internal_event_callback_t)(
    RpcId event_id, const ProtobufCMessage *event, void *argument);

struct esp_hosted_rpc_callbacks
{
    esp_hosted_rpc_internal_event_callback_t event;
};

rt_err_t esp_hosted_rpc_init(const struct esp_hosted_rpc_callbacks *callbacks,
                             void *argument);
void esp_hosted_rpc_deinit(void);
void esp_hosted_rpc_reset(void);
void esp_hosted_rpc_set_ready(rt_bool_t ready);
void esp_hosted_rpc_receive(const uint8_t *data, size_t length, uint8_t flags);

rt_err_t esp_hosted_rpc_execute_message(
    RpcId request_id, const ProtobufCMessage *request, int timeout_ms,
    ProtobufCMessage **response);
ProtobufCAllocator *esp_hosted_rpc_allocator(void);

#endif /* RT_ESP_HOSTED_RPC_INTERNAL_H */
