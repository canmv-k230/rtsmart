/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-Hosted-MCU protobuf RPC core and RT-Smart serial adapter.
 */
#include "rpc_core.h"
#include "esp_hosted_rpc_compat.h"

#include <rthw.h>

#define DBG_TAG "esp.rpc"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define EH_FLAG_MORE_FRAGMENT     (1U << 0)
#define EH_RPC_TLV_HEADER_SIZE    12
#define EH_RPC_SERIAL_MAX_SIZE    8192
#define EH_RPC_TX_SIZE            (EH_RPC_SERIAL_MAX_SIZE - EH_RPC_TLV_HEADER_SIZE)
#define EH_RPC_RX_SIZE            (ESP_HOSTED_RPC_MAX_RESPONSE_SIZE + 32)
#define EH_RPC_EVENT_QUEUE_DEPTH  8
#define EH_MQ_POOL_SIZE(depth, type) \
    ((depth) * (RT_ALIGN(sizeof(type), RT_ALIGN_SIZE) + sizeof(void *)))

#ifndef ESP_HOSTED_EVENT_THREAD_STACK_SIZE
#define ESP_HOSTED_EVENT_THREAD_STACK_SIZE 8192
#endif

struct eh_rpc_event
{
    uint16_t id;
    size_t length;
    uint8_t *data;
};

struct eh_rpc_context
{
    struct esp_hosted_rpc_callbacks callbacks;
    void *callback_argument;
    struct rt_messagequeue event_queue;
    struct rt_mutex mutex;
    struct rt_semaphore done;
    rt_thread_t event_thread;
    volatile rt_bool_t ready;
    rt_bool_t initialized;
    uint32_t uid;
    volatile uint32_t pending_uid;
    uint16_t response_id;
    ProtobufCMessage *response_message;
    size_t fragment_length;
    uint8_t fragment[EH_RPC_RX_SIZE];
    uint8_t tx[EH_RPC_TX_SIZE];
    uint8_t tlv[EH_RPC_TX_SIZE + EH_RPC_TLV_HEADER_SIZE];
    uint8_t event_pool[EH_MQ_POOL_SIZE(EH_RPC_EVENT_QUEUE_DEPTH,
                                      struct eh_rpc_event)]
        __attribute__((aligned(RT_ALIGN_SIZE)));
};

static struct eh_rpc_context g_rpc;
static esp_hosted_rpc_event_callback_t g_observer_callback;
static void *g_observer_argument;

static void *eh_rpc_alloc(void *allocator_data, size_t size)
{
    (void)allocator_data;
    return esp_hosted_rpc_compat_alloc(size);
}

static void eh_rpc_free(void *allocator_data, void *pointer)
{
    (void)allocator_data;
    esp_hosted_rpc_compat_free(pointer);
}

static ProtobufCAllocator g_rpc_allocator = {
    .alloc = eh_rpc_alloc,
    .free = eh_rpc_free,
    .allocator_data = RT_NULL,
};

ProtobufCAllocator *esp_hosted_rpc_allocator(void)
{
    return &g_rpc_allocator;
}

static uint16_t eh_get_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void eh_put_le16(uint8_t *data, uint16_t value)
{
    data[0] = value & 0xff;
    data[1] = value >> 8;
}

static const ProtobufCFieldDescriptor *eh_payload_field(uint32_t id)
{
    const ProtobufCFieldDescriptor *field =
        protobuf_c_message_descriptor_get_field(&rpc__descriptor, id);

    if (!field || field->type != PROTOBUF_C_TYPE_MESSAGE || !field->descriptor)
    {
        return RT_NULL;
    }
    return field;
}

static ProtobufCMessage **eh_payload_slot(Rpc *rpc,
                                          const ProtobufCFieldDescriptor *field)
{
    return (ProtobufCMessage **)((uint8_t *)rpc + field->offset);
}

static esp_hosted_rpc_event_callback_t eh_get_observer(void **argument)
{
    esp_hosted_rpc_event_callback_t callback;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    callback = g_observer_callback;
    *argument = g_observer_argument;
    rt_hw_interrupt_enable(level);
    return callback;
}

static void eh_queue_event(uint16_t id, const ProtobufCMessage *message)
{
    struct eh_rpc_event event;
    void *argument;

    if (!eh_get_observer(&argument))
    {
        return;
    }
    (void)argument;
    rt_memset(&event, 0, sizeof(event));
    event.id = id;
    event.length = protobuf_c_message_get_packed_size(message);
    if (event.length)
    {
        event.data = esp_hosted_rpc_compat_alloc(event.length);
        if (!event.data)
        {
            LOG_W("cannot allocate event %u (%u bytes)", id,
                  (unsigned int)event.length);
            return;
        }
        if (protobuf_c_message_pack(message, event.data) != event.length)
        {
            esp_hosted_rpc_compat_free(event.data);
            return;
        }
    }
    if (rt_mq_send(&g_rpc.event_queue, &event, sizeof(event)) != RT_EOK)
    {
        LOG_W("event queue full; dropped event %u", id);
        esp_hosted_rpc_compat_free(event.data);
    }
}

static void eh_event_thread(void *argument)
{
    struct eh_rpc_event event;

    (void)argument;
    while (1)
    {
        if (rt_mq_recv(&g_rpc.event_queue, &event, sizeof(event),
                       RT_WAITING_FOREVER) == RT_EOK)
        {
            void *callback_argument;
            esp_hosted_rpc_event_callback_t callback =
                eh_get_observer(&callback_argument);

            if (callback)
            {
                callback((RpcId)event.id, event.data, event.length,
                         callback_argument);
            }
            esp_hosted_rpc_compat_free(event.data);
        }
    }
}

static void eh_handle_message(const uint8_t *data, size_t length)
{
    const ProtobufCFieldDescriptor *field;
    ProtobufCMessage *payload;
    Rpc *rpc;

    rpc = rpc__unpack(&g_rpc_allocator, length, data);
    if (!rpc)
    {
        LOG_W("cannot decode Rpc envelope");
        return;
    }
    field = eh_payload_field((uint32_t)rpc->msg_id);
    if (!field || rpc->payload_case != (Rpc__PayloadCase)rpc->msg_id)
    {
        LOG_W("invalid Rpc payload %u", rpc->msg_id);
        rpc__free_unpacked(rpc, &g_rpc_allocator);
        return;
    }
    payload = *eh_payload_slot(rpc, field);
    if (!payload || payload->descriptor != field->descriptor)
    {
        LOG_W("missing Rpc payload %u", rpc->msg_id);
        rpc__free_unpacked(rpc, &g_rpc_allocator);
        return;
    }

    if (rpc->msg_type == RPC_TYPE__Resp && rpc->uid == g_rpc.pending_uid)
    {
        if (g_rpc.response_message)
        {
            protobuf_c_message_free_unpacked(g_rpc.response_message,
                                              &g_rpc_allocator);
        }
        g_rpc.response_id = (uint16_t)rpc->msg_id;
        g_rpc.response_message = payload;
        *eh_payload_slot(rpc, field) = RT_NULL;
        rpc->payload_case = RPC__PAYLOAD__NOT_SET;
        rt_sem_release(&g_rpc.done);
    }
    else if (rpc->msg_type == RPC_TYPE__Event)
    {
        if (g_rpc.callbacks.event)
        {
            g_rpc.callbacks.event(rpc->msg_id, payload,
                                  g_rpc.callback_argument);
        }
        eh_queue_event((uint16_t)rpc->msg_id, payload);
    }
    rpc__free_unpacked(rpc, &g_rpc_allocator);
}

static rt_err_t eh_wait_ready(void)
{
    rt_tick_t start = rt_tick_get();
    rt_tick_t timeout = rt_tick_from_millisecond(ESP_HOSTED_RPC_TIMEOUT_MS);

    while (!g_rpc.ready)
    {
        if ((rt_tick_get() - start) >= timeout)
        {
            LOG_E("startup timeout");
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(10);
    }
    return RT_EOK;
}

static ProtobufCMessage *eh_decode_request(
    RpcId request_id, const void *data, size_t length)
{
    const ProtobufCFieldDescriptor *field =
        eh_payload_field((uint32_t)request_id);
    const ProtobufCMessageDescriptor *descriptor;
    ProtobufCMessage *request;

    if (!field)
    {
        return RT_NULL;
    }
    descriptor = field->descriptor;
    if (length)
    {
        return protobuf_c_message_unpack(descriptor, &g_rpc_allocator, length,
                                         data);
    }
    request = esp_hosted_rpc_compat_alloc(descriptor->sizeof_message);
    if (request)
    {
        rt_memset(request, 0, descriptor->sizeof_message);
        descriptor->message_init(request);
    }
    return request;
}

static rt_err_t eh_response_status(const ProtobufCMessage *response,
                                   int *status)
{
    const ProtobufCFieldDescriptor *field;

    field = protobuf_c_message_descriptor_get_field_by_name(
        response->descriptor, "resp");
    if (!field || (field->type != PROTOBUF_C_TYPE_INT32 &&
                   field->type != PROTOBUF_C_TYPE_SINT32 &&
                   field->type != PROTOBUF_C_TYPE_SFIXED32))
    {
        return -RT_ERROR;
    }
    *status = *(const int32_t *)((const uint8_t *)response + field->offset);
    return RT_EOK;
}

rt_err_t esp_hosted_rpc_init(const struct esp_hosted_rpc_callbacks *callbacks,
                             void *argument)
{
    rt_err_t result;

    if (!callbacks)
    {
        return -RT_EINVAL;
    }
    rt_memset(&g_rpc, 0, sizeof(g_rpc));
    g_rpc.callbacks = *callbacks;
    g_rpc.callback_argument = argument;

    result = rt_mutex_init(&g_rpc.mutex, "eh-rpc", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_sem_init(&g_rpc.done, "eh-done", 0, RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mq_init(&g_rpc.event_queue, "eh-rpcev", g_rpc.event_pool,
                        sizeof(struct eh_rpc_event), sizeof(g_rpc.event_pool),
                        RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    g_rpc.event_thread =
        rt_thread_create("esp-rpc", eh_event_thread, &g_rpc,
                         ESP_HOSTED_EVENT_THREAD_STACK_SIZE,
                         ESP_HOSTED_THREAD_PRIORITY, 20);
    if (!g_rpc.event_thread)
    {
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(g_rpc.event_thread);
    if (result == RT_EOK)
    {
        g_rpc.initialized = RT_TRUE;
    }
    return result;
}

void esp_hosted_rpc_reset(void)
{
    g_rpc.ready = RT_FALSE;
    g_rpc.fragment_length = 0;
    if (g_rpc.initialized && g_rpc.pending_uid)
    {
        g_rpc.response_id = 0;
        rt_sem_release(&g_rpc.done);
    }
}

void esp_hosted_rpc_set_ready(rt_bool_t ready)
{
    g_rpc.ready = ready;
}

void esp_hosted_rpc_receive(const uint8_t *data, size_t length, uint8_t flags)
{
    const uint8_t *rpc_data;
    size_t rpc_length;

    if (!g_rpc.initialized)
    {
        return;
    }
    if (length > sizeof(g_rpc.fragment) - g_rpc.fragment_length)
    {
        LOG_W("fragment overflow");
        g_rpc.fragment_length = 0;
        return;
    }
    rt_memcpy(g_rpc.fragment + g_rpc.fragment_length, data, length);
    g_rpc.fragment_length += length;
    if (flags & EH_FLAG_MORE_FRAGMENT)
    {
        return;
    }

    data = g_rpc.fragment;
    length = g_rpc.fragment_length;
    g_rpc.fragment_length = 0;
    if (length < EH_RPC_TLV_HEADER_SIZE || data[0] != 0x01 ||
        eh_get_le16(data + 1) != 6 ||
        (rt_memcmp(data + 3, "RPCRsp", 6) != 0 &&
         rt_memcmp(data + 3, "RPCEvt", 6) != 0) ||
        data[9] != 0x02)
    {
        LOG_W("invalid TLV");
        return;
    }
    rpc_length = eh_get_le16(data + 10);
    if (rpc_length > length - EH_RPC_TLV_HEADER_SIZE)
    {
        LOG_W("truncated TLV");
        return;
    }
    rpc_data = data + EH_RPC_TLV_HEADER_SIZE;
    eh_handle_message(rpc_data, rpc_length);
}

rt_err_t esp_hosted_rpc_execute_message(
    RpcId request_id, const ProtobufCMessage *request, int timeout_ms,
    ProtobufCMessage **response)
{
    const ProtobufCFieldDescriptor *request_field;
    const ProtobufCFieldDescriptor *response_field;
    uint16_t expected_response_id = ESP_HOSTED_RPC_RESPONSE_ID(request_id);
    Rpc envelope = RPC__INIT;
    size_t serial_length;
    size_t transport_payload;
    size_t offset;
    uint32_t uid;
    rt_err_t result;

    if (!request || !response)
    {
        return -RT_EINVAL;
    }
    *response = RT_NULL;
    request_field = eh_payload_field((uint32_t)request_id);
    response_field = eh_payload_field(expected_response_id);
    if (!request_field || !response_field)
    {
        return -RT_ENOSYS;
    }
    if (request->descriptor != request_field->descriptor)
    {
        return -RT_EINVAL;
    }
    result = eh_wait_ready();
    if (result != RT_EOK)
    {
        return result;
    }
    rt_mutex_take(&g_rpc.mutex, RT_WAITING_FOREVER);
    while (rt_sem_trytake(&g_rpc.done) == RT_EOK)
    {
    }
    if (g_rpc.response_message)
    {
        protobuf_c_message_free_unpacked(g_rpc.response_message,
                                          &g_rpc_allocator);
        g_rpc.response_message = RT_NULL;
    }

    uid = ++g_rpc.uid;
    if (!uid)
    {
        /* UID 0 means no pending request; skip it after wraparound. */
        uid = ++g_rpc.uid;
    }
    envelope.msg_type = RPC_TYPE__Req;
    envelope.msg_id = (RpcId)request_id;
    envelope.uid = uid;
    envelope.payload_case = (Rpc__PayloadCase)request_id;
    *eh_payload_slot(&envelope, request_field) = (ProtobufCMessage *)request;
    serial_length = rpc__get_packed_size(&envelope);
    if (!serial_length || serial_length > sizeof(g_rpc.tx))
    {
        result = -RT_EFULL;
        goto exit;
    }
    if (rpc__pack(&envelope, g_rpc.tx) != serial_length)
    {
        result = -RT_ERROR;
        goto exit;
    }

    g_rpc.tlv[0] = 0x01;
    eh_put_le16(g_rpc.tlv + 1, 6);
    rt_memcpy(g_rpc.tlv + 3, "RPCRsp", 6);
    g_rpc.tlv[9] = 0x02;
    eh_put_le16(g_rpc.tlv + 10, serial_length);
    rt_memcpy(g_rpc.tlv + EH_RPC_TLV_HEADER_SIZE, g_rpc.tx, serial_length);
    serial_length += EH_RPC_TLV_HEADER_SIZE;
    g_rpc.response_id = 0;
    g_rpc.pending_uid = uid;
    transport_payload = esp_hosted_rpc_compat_max_payload();
    if (!transport_payload)
    {
        result = -RT_ERROR;
        g_rpc.pending_uid = 0;
        goto exit;
    }
    for (offset = 0; offset < serial_length;)
    {
        size_t fragment_length = serial_length - offset;
        uint8_t fragment_flags = 0;

        if (fragment_length > transport_payload)
        {
            fragment_length = transport_payload;
            fragment_flags = EH_FLAG_MORE_FRAGMENT;
        }
        result = esp_hosted_rpc_compat_send(fragment_flags,
                                            g_rpc.tlv + offset,
                                            fragment_length);
        if (result != RT_EOK)
        {
            LOG_E("cannot queue request %u fragment %u: %d", request_id,
                  (unsigned int)offset, result);
            g_rpc.pending_uid = 0;
            goto exit;
        }
        offset += fragment_length;
    }
    result = rt_sem_take(&g_rpc.done, rt_tick_from_millisecond(timeout_ms));
    g_rpc.pending_uid = 0;
    if (result != RT_EOK)
    {
        LOG_W("request %u timed out", request_id);
        result = -RT_ETIMEOUT;
        goto exit;
    }
    if (g_rpc.response_id != expected_response_id || !g_rpc.response_message ||
        g_rpc.response_message->descriptor != response_field->descriptor)
    {
        LOG_W("request %u received invalid response %u", request_id,
              g_rpc.response_id);
        result = -RT_ERROR;
        goto exit;
    }
    *response = g_rpc.response_message;
    g_rpc.response_message = RT_NULL;

exit:
    if (result != RT_EOK && g_rpc.response_message)
    {
        protobuf_c_message_free_unpacked(g_rpc.response_message,
                                          &g_rpc_allocator);
        g_rpc.response_message = RT_NULL;
    }
    rt_mutex_release(&g_rpc.mutex);
    return result;
}

rt_err_t rt_esp_hosted_rpc_call(RpcId request_id, const void *request,
                                size_t request_length, void *response,
                                size_t response_capacity,
                                size_t *response_length, int timeout_ms)
{
    ProtobufCMessage *request_message;
    ProtobufCMessage *response_message = RT_NULL;
    size_t packed_length;
    rt_err_t result;

    if (response_length)
    {
        *response_length = 0;
    }
    if (request_id <= RPC_ID__Req_Base || request_id >= RPC_ID__Req_Max ||
        request_length > ESP_HOSTED_RPC_MAX_REQUEST_SIZE ||
        (request_length && !request) || (!response && response_capacity))
    {
        return -RT_EINVAL;
    }
    request_message = eh_decode_request(request_id, request, request_length);
    if (!request_message)
    {
        return eh_payload_field((uint32_t)request_id) ? -RT_ERROR : -RT_ENOSYS;
    }
    if (timeout_ms <= 0)
    {
        timeout_ms = ESP_HOSTED_RPC_TIMEOUT_MS;
    }
    result = esp_hosted_rpc_execute_message(request_id, request_message,
                                            timeout_ms, &response_message);
    protobuf_c_message_free_unpacked(request_message, &g_rpc_allocator);
    if (result != RT_EOK)
    {
        return result;
    }
    packed_length = protobuf_c_message_get_packed_size(response_message);
    if (response_length)
    {
        *response_length = packed_length;
    }
    if (response && packed_length > response_capacity)
    {
        result = -RT_EFULL;
    }
    else if (response && packed_length &&
             protobuf_c_message_pack(response_message, response) != packed_length)
    {
        result = -RT_ERROR;
    }
    protobuf_c_message_free_unpacked(response_message, &g_rpc_allocator);
    return result;
}

rt_err_t rt_esp_hosted_rpc_call_status(RpcId request_id, const void *request,
                                       size_t request_length, int timeout_ms)
{
    ProtobufCMessage *request_message;
    ProtobufCMessage *response_message = RT_NULL;
    int status;
    rt_err_t result;

    if (request_id <= RPC_ID__Req_Base || request_id >= RPC_ID__Req_Max ||
        request_length > ESP_HOSTED_RPC_MAX_REQUEST_SIZE ||
        (request_length && !request))
    {
        return -RT_EINVAL;
    }
    request_message = eh_decode_request(request_id, request, request_length);
    if (!request_message)
    {
        return eh_payload_field((uint32_t)request_id) ? -RT_ERROR : -RT_ENOSYS;
    }
    if (timeout_ms <= 0)
    {
        timeout_ms = ESP_HOSTED_RPC_TIMEOUT_MS;
    }
    result = esp_hosted_rpc_execute_message(request_id, request_message,
                                            timeout_ms, &response_message);
    protobuf_c_message_free_unpacked(request_message, &g_rpc_allocator);
    if (result == RT_EOK)
    {
        result = eh_response_status(response_message, &status);
        if (result == RT_EOK && status)
        {
            LOG_W("request %u failed: 0x%x", request_id, status);
            result = -RT_ERROR;
        }
    }
    if (response_message)
    {
        protobuf_c_message_free_unpacked(response_message, &g_rpc_allocator);
    }
    return result;
}

rt_err_t rt_esp_hosted_rpc_set_event_callback(
    esp_hosted_rpc_event_callback_t callback, void *argument)
{
    rt_base_t level = rt_hw_interrupt_disable();

    g_observer_callback = callback;
    g_observer_argument = callback ? argument : RT_NULL;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

RTM_EXPORT(rt_esp_hosted_rpc_call);
RTM_EXPORT(rt_esp_hosted_rpc_call_status);
RTM_EXPORT(rt_esp_hosted_rpc_set_event_callback);
