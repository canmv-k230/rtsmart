/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Complete generated-schema access for ESP-Hosted-MCU RPC messages.
 */
#include "rpc_slave_if.h"
#include "rpc_core.h"

#include <rthw.h>

#define DBG_TAG "esp.rpc.schema"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static esp_hosted_rpc_message_event_callback_t g_event_callback;
static void *g_event_argument;

static const ProtobufCMessageDescriptor *eh_payload_descriptor(uint32_t id)
{
    const ProtobufCFieldDescriptor *field =
        protobuf_c_message_descriptor_get_field(&rpc__descriptor, id);

    if (!field || field->type != PROTOBUF_C_TYPE_MESSAGE || !field->descriptor)
    {
        return RT_NULL;
    }
    return field->descriptor;
}

const ProtobufCMessageDescriptor *
rt_esp_hosted_rpc_request_descriptor(RpcId request_id)
{
    if (request_id <= RPC_ID__Req_Base || request_id >= RPC_ID__Req_Max)
    {
        return RT_NULL;
    }
    return eh_payload_descriptor((uint32_t)request_id);
}

const ProtobufCMessageDescriptor *
rt_esp_hosted_rpc_response_descriptor(RpcId request_id)
{
    if (request_id <= RPC_ID__Req_Base || request_id >= RPC_ID__Req_Max)
    {
        return RT_NULL;
    }
    return eh_payload_descriptor(ESP_HOSTED_RPC_RESPONSE_ID(request_id));
}

const ProtobufCMessageDescriptor *
rt_esp_hosted_rpc_event_descriptor(RpcId event_id)
{
    if (event_id <= RPC_ID__Event_Base || event_id >= RPC_ID__Event_Max)
    {
        return RT_NULL;
    }
    return eh_payload_descriptor((uint32_t)event_id);
}

rt_err_t rt_esp_hosted_rpc_call_message(RpcId request_id,
                                        const ProtobufCMessage *request,
                                        ProtobufCMessage **response,
                                        int timeout_ms)
{
    const ProtobufCMessageDescriptor *request_descriptor;
    const ProtobufCMessageDescriptor *response_descriptor;
    ProtobufCMessage *default_request = RT_NULL;
    ProtobufCAllocator *allocator = esp_hosted_rpc_allocator();
    rt_err_t result;

    if (!response)
    {
        return -RT_EINVAL;
    }
    *response = RT_NULL;
    request_descriptor = rt_esp_hosted_rpc_request_descriptor(request_id);
    response_descriptor = rt_esp_hosted_rpc_response_descriptor(request_id);
    if (!request_descriptor || !response_descriptor)
    {
        return -RT_ENOSYS;
    }
    if (request && request->descriptor != request_descriptor)
    {
        return -RT_EINVAL;
    }
    if (!request)
    {
        default_request = allocator->alloc(
            allocator->allocator_data, request_descriptor->sizeof_message);
        if (!default_request)
        {
            return -RT_ENOMEM;
        }
        rt_memset(default_request, 0, request_descriptor->sizeof_message);
        request_descriptor->message_init(default_request);
        request = default_request;
    }

    if (timeout_ms <= 0)
    {
        timeout_ms = ESP_HOSTED_RPC_TIMEOUT_MS;
    }
    result = esp_hosted_rpc_execute_message(request_id, request, timeout_ms,
                                            response);
    if (default_request)
    {
        protobuf_c_message_free_unpacked(default_request, allocator);
    }
    return result;
}

void rt_esp_hosted_rpc_free_message(ProtobufCMessage *message)
{
    protobuf_c_message_free_unpacked(message, esp_hosted_rpc_allocator());
}

rt_err_t rt_esp_hosted_rpc_decode_event(RpcId event_id,
                                        const void *payload,
                                        size_t payload_length,
                                        ProtobufCMessage **event)
{
    const ProtobufCMessageDescriptor *descriptor;

    if (!event || (payload_length && !payload))
    {
        return -RT_EINVAL;
    }
    *event = RT_NULL;
    descriptor = rt_esp_hosted_rpc_event_descriptor(event_id);
    if (!descriptor)
    {
        return -RT_ENOSYS;
    }
    *event = protobuf_c_message_unpack(descriptor, esp_hosted_rpc_allocator(),
                                       payload_length, payload);
    return *event ? RT_EOK : -RT_ERROR;
}

rt_err_t rt_esp_hosted_rpc_response_status(const ProtobufCMessage *response,
                                           int *status)
{
    const ProtobufCFieldDescriptor *field;

    if (!response || !response->descriptor || !status)
    {
        return -RT_EINVAL;
    }
    field = protobuf_c_message_descriptor_get_field_by_name(
        response->descriptor, "resp");
    if (!field || (field->type != PROTOBUF_C_TYPE_INT32 &&
                   field->type != PROTOBUF_C_TYPE_SINT32 &&
                   field->type != PROTOBUF_C_TYPE_SFIXED32))
    {
        return -RT_EINVAL;
    }
    *status = *(const int32_t *)((const uint8_t *)response + field->offset);
    return RT_EOK;
}

static void eh_schema_event(RpcId event_id,
                            const uint8_t *payload, size_t payload_length,
                            void *argument)
{
    esp_hosted_rpc_message_event_callback_t callback;
    ProtobufCMessage *event;
    void *callback_argument;
    rt_base_t level;

    (void)argument;
    if (rt_esp_hosted_rpc_decode_event(event_id, payload, payload_length,
                                       &event) != RT_EOK)
    {
        LOG_W("cannot decode event %u", event_id);
        return;
    }
    level = rt_hw_interrupt_disable();
    callback = g_event_callback;
    callback_argument = g_event_argument;
    rt_hw_interrupt_enable(level);
    if (callback)
    {
        callback(event_id, event, callback_argument);
    }
    protobuf_c_message_free_unpacked(event, esp_hosted_rpc_allocator());
}

rt_err_t rt_esp_hosted_rpc_set_message_event_callback(
    esp_hosted_rpc_message_event_callback_t callback, void *argument)
{
    rt_base_t level = rt_hw_interrupt_disable();

    g_event_callback = callback;
    g_event_argument = callback ? argument : RT_NULL;
    rt_hw_interrupt_enable(level);
    return rt_esp_hosted_rpc_set_event_callback(
        callback ? eh_schema_event : RT_NULL, RT_NULL);
}

RTM_EXPORT(rt_esp_hosted_rpc_request_descriptor);
RTM_EXPORT(rt_esp_hosted_rpc_response_descriptor);
RTM_EXPORT(rt_esp_hosted_rpc_event_descriptor);
RTM_EXPORT(rt_esp_hosted_rpc_call_message);
RTM_EXPORT(rt_esp_hosted_rpc_decode_event);
RTM_EXPORT(rt_esp_hosted_rpc_free_message);
RTM_EXPORT(rt_esp_hosted_rpc_response_status);
RTM_EXPORT(rt_esp_hosted_rpc_set_message_event_callback);
