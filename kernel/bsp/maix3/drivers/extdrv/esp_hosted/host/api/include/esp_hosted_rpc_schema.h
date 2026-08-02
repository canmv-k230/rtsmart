/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RT_ESP_HOSTED_RPC_SCHEMA_H
#define RT_ESP_HOSTED_RPC_SCHEMA_H

#include "esp_hosted_rpc.h"
#include "esp_hosted_rpc.pb-c.h"

typedef void (*esp_hosted_rpc_message_event_callback_t)(
    RpcId event_id, const ProtobufCMessage *event, void *argument);

/* Return NULL when the ID is only reserved in RpcId and has no payload
 * message in the current ESP-Hosted schema. */
const ProtobufCMessageDescriptor *
rt_esp_hosted_rpc_request_descriptor(RpcId request_id);
const ProtobufCMessageDescriptor *
rt_esp_hosted_rpc_response_descriptor(RpcId request_id);
const ProtobufCMessageDescriptor *
rt_esp_hosted_rpc_event_descriptor(RpcId event_id);

/* The request must use the generated message type associated with request_id.
 * A NULL request encodes a message containing only protobuf default values.
 * On success, *response is allocated and must be released with the free API. */
rt_err_t rt_esp_hosted_rpc_call_message(RpcId request_id,
                                        const ProtobufCMessage *request,
                                        ProtobufCMessage **response,
                                        int timeout_ms);
rt_err_t rt_esp_hosted_rpc_decode_event(RpcId event_id,
                                        const void *payload,
                                        size_t payload_length,
                                        ProtobufCMessage **event);
void rt_esp_hosted_rpc_free_message(ProtobufCMessage *message);

/* Extract the standard ESP status member named "resp". */
rt_err_t rt_esp_hosted_rpc_response_status(const ProtobufCMessage *response,
                                           int *status);

/* The raw and decoded event registration APIs share one application observer;
 * registering either one replaces the other. The event is callback-scoped. */
rt_err_t rt_esp_hosted_rpc_set_message_event_callback(
    esp_hosted_rpc_message_event_callback_t callback, void *argument);

#endif /* RT_ESP_HOSTED_RPC_SCHEMA_H */
