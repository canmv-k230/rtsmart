/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_transport_internal.h"

#define DBG_TAG "esp.transport"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static struct eh_transport g_transport;

#ifdef ESP_HOSTED_BLE
#endif

static uint16_t eh_transport_get_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void eh_transport_put_le16(uint8_t *data, uint16_t value)
{
    data[0] = value & 0xff;
    data[1] = value >> 8;
}

static uint16_t eh_transport_checksum(const uint8_t *data, size_t length,
                                      rt_bool_t skip_checksum)
{
    uint16_t result = 0;
    size_t index;

    for (index = 0; index < length; index++)
    {
        if (!skip_checksum || index < 6 || index > 7)
        {
            result += data[index];
        }
    }
    return result;
}

static void eh_transport_thread(void *argument)
{
    struct eh_transport *transport = argument;

    transport->ops->run(transport);
}

rt_err_t esp_hosted_transport_init(const struct esp_hosted_transport_callbacks *callbacks,
                                   void *argument)
{
    rt_err_t result;

    if (!callbacks || !callbacks->receive)
    {
        return -RT_EINVAL;
    }

    rt_memset(&g_transport, 0, sizeof(g_transport));
#if defined(ESP_HOSTED_TRANSPORT_SPI_HD)
    g_transport.ops = &g_esp_hosted_spi_hd_ops;
#else
    g_transport.ops = &g_esp_hosted_spi_fd_ops;
#endif
    g_transport.callbacks = *callbacks;
    g_transport.callback_argument = argument;

    result = rt_event_init(&g_transport.event, "eh-xfer", RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mq_init(&g_transport.ctrl_queue, "eh-ctrl", g_transport.ctrl_pool,
                        sizeof(struct eh_transport_tx_item),
                        sizeof(g_transport.ctrl_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
#ifdef ESP_HOSTED_BLE
    result = rt_mq_init(&g_transport.hci_queue, "eh-hci", g_transport.hci_pool,
                        sizeof(struct eh_transport_tx_item),
                        sizeof(g_transport.hci_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
#endif
    result = rt_mq_init(&g_transport.data_queue, "eh-data", g_transport.data_pool,
                        sizeof(struct eh_transport_tx_item),
                        sizeof(g_transport.data_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = g_transport.ops->init(&g_transport);
    if (result != RT_EOK)
    {
        return result;
    }

    g_transport.thread = rt_thread_create("esp-hosted", eh_transport_thread,
                                          &g_transport,
                                          ESP_HOSTED_THREAD_STACK_SIZE,
                                          ESP_HOSTED_THREAD_PRIORITY, 20);
    return g_transport.thread ? RT_EOK : -RT_ENOMEM;
}

rt_err_t esp_hosted_transport_start(void)
{
    rt_err_t result;

    if (!g_transport.ops || !g_transport.thread)
    {
        return -RT_EINVAL;
    }
    result = g_transport.ops->start(&g_transport);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_thread_startup(g_transport.thread);
    if (result == RT_EOK)
    {
        eh_transport_wake(&g_transport);
    }
    return result;
}

rt_err_t esp_hosted_transport_send(uint8_t interface, uint8_t flags,
                                   const void *data, size_t length,
                                   rt_bool_t control,
                                   esp_hosted_transport_tx_done_t done,
                                   void *done_argument)
{
    struct eh_transport_tx_item item;
    struct rt_messagequeue *queue;
    size_t frame_length;
    size_t wire_length;
    rt_err_t result;

    if (!g_transport.ops || interface >= EH_TRANSPORT_IF_MAX ||
        length > esp_hosted_transport_max_payload() || (length && !data))
    {
        return -RT_EINVAL;
    }
    if (!control && g_transport.tx_throttled)
    {
        return -RT_EBUSY;
    }

    frame_length = ESP_HOSTED_TRANSPORT_HEADER_SIZE + length;
    wire_length = RT_ALIGN(frame_length, g_transport.ops->tx_alignment);
    if (wire_length > g_transport.ops->frame_size)
    {
        return -RT_EINVAL;
    }
    rt_memset(&item, 0, sizeof(item));
    item.frame = rt_calloc(1, wire_length);
    if (!item.frame)
    {
        return -RT_ENOMEM;
    }
    item.length = wire_length;
    item.control = control;
    item.done = done;
    item.done_argument = done_argument;
    item.frame[0] = interface & 0x0f;
    item.frame[1] = flags;
    eh_transport_put_le16(item.frame + 2, length);
    eh_transport_put_le16(item.frame + 4, ESP_HOSTED_TRANSPORT_HEADER_SIZE);
    if (length)
    {
        rt_memcpy(item.frame + ESP_HOSTED_TRANSPORT_HEADER_SIZE, data, length);
    }
#ifdef ESP_HOSTED_CHECKSUM
    eh_transport_put_le16(item.frame + 6,
                          eh_transport_checksum(item.frame, frame_length, RT_FALSE));
#endif

    queue = control ? &g_transport.ctrl_queue : &g_transport.data_queue;
    result = rt_mq_send(queue, &item, sizeof(item));
    if (result != RT_EOK)
    {
        rt_free(item.frame);
        return result;
    }
    eh_transport_wake(&g_transport);
    return RT_EOK;
}

#ifdef ESP_HOSTED_BLE
rt_err_t esp_hosted_transport_send_hci(uint8_t packet_type,
                                       const void *data, size_t length,
                                       esp_hosted_transport_tx_done_t done,
                                       void *done_argument)
{
    struct eh_transport_tx_item item;
    size_t frame_length;
    size_t wire_length;
    rt_err_t result;

    if (!g_transport.ops || packet_type < 1 || packet_type > 5 ||
        length > esp_hosted_transport_max_payload() || (length && !data))
    {
        return -RT_EINVAL;
    }

    frame_length = ESP_HOSTED_TRANSPORT_HEADER_SIZE + length;
    wire_length = RT_ALIGN(frame_length, g_transport.ops->tx_alignment);
    if (wire_length > g_transport.ops->frame_size)
    {
        return -RT_EINVAL;
    }
    rt_memset(&item, 0, sizeof(item));
    item.frame = rt_calloc(1, wire_length);
    if (!item.frame)
    {
        return -RT_ENOMEM;
    }
    item.length = wire_length;
    item.hci = RT_TRUE;
    item.control = RT_TRUE;
    item.done = done;
    item.done_argument = done_argument;
    item.frame[0] = ESP_HOSTED_TRANSPORT_IF_HCI;
    eh_transport_put_le16(item.frame + 2, length);
    eh_transport_put_le16(item.frame + 4, ESP_HOSTED_TRANSPORT_HEADER_SIZE);
    item.frame[11] = packet_type;
    if (length)
    {
        rt_memcpy(item.frame + ESP_HOSTED_TRANSPORT_HEADER_SIZE, data, length);
    }
#ifdef ESP_HOSTED_CHECKSUM
    eh_transport_put_le16(item.frame + 6,
                          eh_transport_checksum(item.frame, frame_length, RT_FALSE));
#endif

    result = rt_mq_send(&g_transport.hci_queue, &item, sizeof(item));
    if (result != RT_EOK)
    {
        rt_free(item.frame);
        return result;
    }
    eh_transport_wake(&g_transport);
    return RT_EOK;
}

void esp_hosted_transport_flush_hci(void)
{
    struct eh_transport_tx_item item;

    while (rt_mq_recv(&g_transport.hci_queue, &item, sizeof(item),
                      RT_WAITING_NO) == RT_EOK)
    {
        eh_transport_complete_tx(&item, -RT_ERROR);
    }
}
#endif

rt_err_t esp_hosted_transport_set_slave_capabilities(uint8_t capabilities,
                                                     uint32_t ext_capabilities)
{
    if (!g_transport.ops || !g_transport.ops->set_slave_capabilities)
    {
        return RT_EOK;
    }
    return g_transport.ops->set_slave_capabilities(&g_transport, capabilities,
                                                   ext_capabilities);
}

rt_bool_t esp_hosted_transport_can_send_data(void)
{
    return !g_transport.tx_throttled;
}

size_t esp_hosted_transport_max_payload(void)
{
    if (!g_transport.ops || g_transport.ops->frame_size < ESP_HOSTED_TRANSPORT_HEADER_SIZE)
    {
        return 0;
    }
    return g_transport.ops->frame_size - ESP_HOSTED_TRANSPORT_HEADER_SIZE;
}

const char *esp_hosted_transport_name(void)
{
    return g_transport.ops ? g_transport.ops->name : "uninitialized";
}

rt_err_t eh_transport_wait(struct eh_transport *transport, rt_int32_t timeout)
{
    rt_uint32_t event;

    return rt_event_recv(&transport->event, EH_TRANSPORT_EVENT_WAKE,
                         RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                         timeout, &event);
}

void eh_transport_wake(struct eh_transport *transport)
{
    rt_event_send(&transport->event, EH_TRANSPORT_EVENT_WAKE);
}

rt_bool_t eh_transport_next_tx(struct eh_transport *transport,
                               struct eh_transport_tx_item *item)
{
    if (rt_mq_recv(&transport->ctrl_queue, item, sizeof(*item),
                   RT_WAITING_NO) == RT_EOK)
    {
        return RT_TRUE;
    }
#ifdef ESP_HOSTED_BLE
    if (rt_mq_recv(&transport->hci_queue, item, sizeof(*item),
                   RT_WAITING_NO) == RT_EOK)
    {
        return RT_TRUE;
    }
#endif
    if (!transport->tx_throttled &&
        rt_mq_recv(&transport->data_queue, item, sizeof(*item),
                   RT_WAITING_NO) == RT_EOK)
    {
        return RT_TRUE;
    }
    return RT_FALSE;
}

void eh_transport_complete_tx(struct eh_transport_tx_item *item, rt_err_t result)
{
    if (item->done)
    {
        item->done(item->done_argument, result);
    }
    rt_free(item->frame);
    rt_memset(item, 0, sizeof(*item));
}

rt_bool_t eh_transport_deliver(struct eh_transport *transport,
                               const uint8_t *frame, size_t frame_length,
                               rt_bool_t *malformed)
{
    uint16_t length;
    uint16_t offset;
    uint16_t received_checksum;
    uint16_t calculated_checksum;
    uint8_t interface;
    uint8_t flow_control;

    if (malformed)
    {
        *malformed = RT_FALSE;
    }
    if (!frame || frame_length < ESP_HOSTED_TRANSPORT_HEADER_SIZE)
    {
        if (malformed)
        {
            *malformed = RT_TRUE;
        }
        return RT_FALSE;
    }

    interface = frame[0] & 0x0f;
    length = eh_transport_get_le16(frame + 2);
    offset = eh_transport_get_le16(frame + 4);
    received_checksum = eh_transport_get_le16(frame + 6);
    flow_control = frame[10] & 0x03;
    if (flow_control == EH_TRANSPORT_FLOW_CTRL_ON)
    {
        transport->tx_throttled = RT_TRUE;
    }
    else if (flow_control == EH_TRANSPORT_FLOW_CTRL_OFF)
    {
        transport->tx_throttled = RT_FALSE;
    }

    if (!length)
    {
        if (interface != EH_TRANSPORT_IF_MAX)
        {
            if (transport->invalid_rx_log_count < EH_TRANSPORT_INVALID_LOG_LIMIT)
            {
                LOG_W("invalid empty frame: interface=%u", interface);
                transport->invalid_rx_log_count++;
            }
            if (malformed)
            {
                *malformed = RT_TRUE;
            }
        }
        else
        {
            transport->invalid_rx_log_count = 0;
        }
        return RT_FALSE;
    }

    if (offset != ESP_HOSTED_TRANSPORT_HEADER_SIZE ||
        length > esp_hosted_transport_max_payload() ||
        offset + length > frame_length)
    {
        if (transport->invalid_rx_log_count < EH_TRANSPORT_INVALID_LOG_LIMIT)
        {
            LOG_W("invalid frame: interface=%u length=%u offset=%u available=%u",
                  interface, length, offset, (unsigned int)frame_length);
            transport->invalid_rx_log_count++;
        }
        if (malformed)
        {
            *malformed = RT_TRUE;
        }
        return RT_FALSE;
    }
#ifdef ESP_HOSTED_CHECKSUM
    calculated_checksum = eh_transport_checksum(frame, offset + length, RT_TRUE);
    if (calculated_checksum != received_checksum)
    {
        if (transport->invalid_rx_log_count < EH_TRANSPORT_INVALID_LOG_LIMIT)
        {
            LOG_W("checksum mismatch: received=%u calculated=%u",
                  received_checksum, calculated_checksum);
            transport->invalid_rx_log_count++;
        }
        if (malformed)
        {
            *malformed = RT_TRUE;
        }
        return RT_FALSE;
    }
#else
    (void)received_checksum;
    (void)calculated_checksum;
#endif

    transport->callbacks.receive(transport->callback_argument, interface,
                                 frame[1], frame + offset, length);
    transport->invalid_rx_log_count = 0;
    return RT_TRUE;
}
