/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_transport_internal.h"
#include "esp_hosted_mcu_log.h"

#define DBG_TAG "esp_hosted.transport"
#define DBG_LVL ESP_HOSTED_MCU_DBG_LVL
#include <rtdbg.h>

static struct eh_transport g_transport;

#ifdef ESP_HOSTED_BLE
#endif

static uint16_t eh_transport_get_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static rt_int32_t eh_transport_wait_remaining(rt_tick_t started,
                                              rt_int32_t timeout)
{
    rt_tick_t elapsed = rt_tick_get() - started;

    return elapsed < (rt_tick_t)timeout
               ? timeout - (rt_int32_t)elapsed : 0;
}

static rt_err_t eh_transport_wait_tx_ready(struct eh_transport *transport,
                                           rt_tick_t started,
                                           rt_int32_t timeout)
{
    while (transport->tx_throttled)
    {
        rt_uint32_t event;
        rt_int32_t remaining = eh_transport_wait_remaining(started, timeout);
        rt_err_t result;

        if (!remaining)
        {
            return -RT_EBUSY;
        }
        result = rt_event_recv(&transport->tx_event,
                               EH_TRANSPORT_EVENT_TX_READY,
                               RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                               remaining, &event);
        if (result != RT_EOK && transport->tx_throttled)
        {
            return result;
        }
    }
    return RT_EOK;
}

#ifdef ESP_HOSTED_SPI_HD_STATS
static void eh_transport_note_data_queue_wait(struct eh_transport *transport)
{
    rt_enter_critical();
    if (transport->data_queue.entry >= transport->data_queue.max_msgs)
    {
        transport->data_queue_waits++;
    }
    rt_exit_critical();
}

static void eh_transport_note_data_queue_depth(struct eh_transport *transport)
{
    rt_enter_critical();
    if (transport->data_queue.entry > transport->data_queue_high_watermark)
    {
        transport->data_queue_high_watermark = transport->data_queue.entry;
    }
    rt_exit_critical();
}

static void eh_transport_note_data_queue_result(struct eh_transport *transport,
                                                 rt_err_t result,
                                                 rt_bool_t bounded_wait)
{
    if (result == RT_EOK)
    {
        eh_transport_note_data_queue_depth(transport);
    }
    else if (bounded_wait)
    {
        rt_enter_critical();
        transport->data_queue_timeouts++;
        rt_exit_critical();
    }
}
#else
#define eh_transport_note_data_queue_wait(transport) do { } while (0)
#define eh_transport_note_data_queue_depth(transport) do { } while (0)
#define eh_transport_note_data_queue_result(transport, result, bounded_wait) \
    do { } while (0)
#endif

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

static void eh_transport_drain_queue(struct rt_messagequeue *queue)
{
    struct eh_transport_tx_item item;

    while (rt_mq_recv(queue, &item, sizeof(item), RT_WAITING_NO) == RT_EOK)
    {
        eh_transport_complete_tx(&item, -RT_ERROR);
    }
}

rt_err_t esp_hosted_transport_deinit(void)
{
    if (g_transport.thread_started)
    {
        return -RT_EBUSY;
    }
    if (g_transport.thread)
    {
        rt_thread_delete(g_transport.thread);
        g_transport.thread = RT_NULL;
    }
    if (g_transport.backend_initialized && g_transport.ops &&
        g_transport.ops->deinit)
    {
        g_transport.ops->deinit(&g_transport);
        g_transport.backend_initialized = RT_FALSE;
    }
    if (g_transport.data_queue_initialized)
    {
        eh_transport_drain_queue(&g_transport.data_queue);
        rt_mq_detach(&g_transport.data_queue);
    }
#ifdef ESP_HOSTED_BLE
    if (g_transport.hci_queue_initialized)
    {
        eh_transport_drain_queue(&g_transport.hci_queue);
        rt_mq_detach(&g_transport.hci_queue);
    }
#endif
    if (g_transport.ctrl_queue_initialized)
    {
        eh_transport_drain_queue(&g_transport.ctrl_queue);
        rt_mq_detach(&g_transport.ctrl_queue);
    }
    if (g_transport.tx_event_initialized)
    {
        rt_event_detach(&g_transport.tx_event);
    }
    if (g_transport.event_initialized)
    {
        rt_event_detach(&g_transport.event);
    }
    rt_memset(&g_transport, 0, sizeof(g_transport));
    return RT_EOK;
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
        goto fail;
    }
    g_transport.event_initialized = RT_TRUE;
    result = rt_event_init(&g_transport.tx_event, "eh-tx", RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        goto fail;
    }
    g_transport.tx_event_initialized = RT_TRUE;
    result = rt_mq_init(&g_transport.ctrl_queue, "eh-ctrl", g_transport.ctrl_pool,
                        sizeof(struct eh_transport_tx_item),
                        sizeof(g_transport.ctrl_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        goto fail;
    }
    g_transport.ctrl_queue_initialized = RT_TRUE;
#ifdef ESP_HOSTED_BLE
    result = rt_mq_init(&g_transport.hci_queue, "eh-hci", g_transport.hci_pool,
                        sizeof(struct eh_transport_tx_item),
                        sizeof(g_transport.hci_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        goto fail;
    }
    g_transport.hci_queue_initialized = RT_TRUE;
#endif
    result = rt_mq_init(&g_transport.data_queue, "eh-data", g_transport.data_pool,
                        sizeof(struct eh_transport_tx_item),
                        sizeof(g_transport.data_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        goto fail;
    }
    g_transport.data_queue_initialized = RT_TRUE;
    result = g_transport.ops->init(&g_transport);
    if (result != RT_EOK)
    {
        if (g_transport.ops->deinit && g_transport.backend)
        {
            g_transport.ops->deinit(&g_transport);
        }
        goto fail;
    }
    g_transport.backend_initialized = RT_TRUE;

    g_transport.thread = rt_thread_create("esp-hosted", eh_transport_thread,
                                          &g_transport,
                                          ESP_HOSTED_THREAD_STACK_SIZE,
                                          ESP_HOSTED_THREAD_PRIORITY, 20);
    if (!g_transport.thread)
    {
        result = -RT_ENOMEM;
        goto fail;
    }
    return RT_EOK;

fail:
    (void)esp_hosted_transport_deinit();
    return result;
}

rt_err_t esp_hosted_transport_start(void)
{
    rt_err_t result;

    if (!g_transport.ops || !g_transport.thread || g_transport.thread_started)
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
        g_transport.thread_started = RT_TRUE;
        eh_transport_wake(&g_transport, EH_TRANSPORT_EVENT_ALL);
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
    rt_tick_t wait_started = 0;
    rt_int32_t wait_ticks = 0;
    rt_err_t result;

    if (!g_transport.ops || interface >= EH_TRANSPORT_IF_MAX ||
        length > esp_hosted_transport_max_payload() || (length && !data))
    {
        return -RT_EINVAL;
    }
    if (!control)
    {
        wait_ticks = rt_tick_from_millisecond(
            g_transport.ops->data_queue_send_wait_ms);
        wait_started = rt_tick_get();
        result = eh_transport_wait_tx_ready(&g_transport, wait_started,
                                            wait_ticks);
        if (result != RT_EOK)
        {
            return result;
        }
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
    if (control)
    {
        result = rt_mq_send(queue, &item, sizeof(item));
    }
    else
    {
        if (wait_ticks)
        {
            eh_transport_note_data_queue_wait(&g_transport);
            result = rt_mq_send_wait(
                queue, &item, sizeof(item),
                eh_transport_wait_remaining(wait_started, wait_ticks));
        }
        else
        {
            result = rt_mq_send(queue, &item, sizeof(item));
        }
        eh_transport_note_data_queue_result(&g_transport, result,
                                            wait_ticks != 0);
    }
    if (result != RT_EOK)
    {
        rt_free(item.frame);
        return result;
    }
    eh_transport_wake(&g_transport, control ? EH_TRANSPORT_EVENT_CONTROL :
                                             EH_TRANSPORT_EVENT_DATA);
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
    eh_transport_wake(&g_transport, EH_TRANSPORT_EVENT_CONTROL);
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

rt_err_t eh_transport_wait(struct eh_transport *transport, rt_uint32_t events,
                           rt_int32_t timeout)
{
    rt_uint32_t event;

    return rt_event_recv(&transport->event, events,
                         RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                         timeout, &event);
}

void eh_transport_wake(struct eh_transport *transport, rt_uint32_t events)
{
    rt_event_send(&transport->event, events);
}

void eh_transport_set_tx_throttled(struct eh_transport *transport,
                                   rt_bool_t throttled)
{
    rt_bool_t was_throttled = transport->tx_throttled;

    transport->tx_throttled = throttled;
    if (was_throttled && !throttled)
    {
        rt_event_send(&transport->tx_event, EH_TRANSPORT_EVENT_TX_READY);
    }
}

rt_bool_t eh_transport_next_control(struct eh_transport *transport,
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
    return RT_FALSE;
}

rt_bool_t eh_transport_next_tx(struct eh_transport *transport,
                               struct eh_transport_tx_item *item)
{
    if (eh_transport_next_control(transport, item))
    {
        return RT_TRUE;
    }
    eh_transport_note_data_queue_depth(transport);
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

    /* Full-duplex SPI dummy transactions use a zeroed header except for the
     * ESP_MAX_IF/if_num sentinel, so they have no offset or checksum to
     * validate. Check the complete sentinel before accepting flow control. */
    if (!length && interface == EH_TRANSPORT_IF_MAX)
    {
        if (frame[0] != (0xf0U | EH_TRANSPORT_IF_MAX) || frame[1] ||
            offset || received_checksum || frame[8] || frame[9] ||
            (frame[10] & 0xfcU) || frame[11])
        {
            if (transport->invalid_rx_log_count < EH_TRANSPORT_INVALID_LOG_LIMIT)
            {
                LOG_W("invalid empty frame sentinel: 0x%02x", frame[0]);
                transport->invalid_rx_log_count++;
            }
            if (malformed)
            {
                *malformed = RT_TRUE;
            }
            return RT_FALSE;
        }
        if (flow_control == EH_TRANSPORT_FLOW_CTRL_ON)
        {
            eh_transport_set_tx_throttled(transport, RT_TRUE);
        }
        else if (flow_control == EH_TRANSPORT_FLOW_CTRL_OFF)
        {
            eh_transport_set_tx_throttled(transport, RT_FALSE);
        }
        transport->invalid_rx_log_count = 0;
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

    /* Flow-control-only packets have no payload. Validate their header and
     * checksum before allowing them to change persistent TX state. */
    if (flow_control == EH_TRANSPORT_FLOW_CTRL_ON)
    {
        eh_transport_set_tx_throttled(transport, RT_TRUE);
    }
    else if (flow_control == EH_TRANSPORT_FLOW_CTRL_OFF)
    {
        eh_transport_set_tx_throttled(transport, RT_FALSE);
    }
    if (!length)
    {
        if (flow_control != EH_TRANSPORT_FLOW_CTRL_ON &&
            flow_control != EH_TRANSPORT_FLOW_CTRL_OFF)
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

    transport->callbacks.receive(transport->callback_argument, interface,
                                 frame[1], frame + offset, length);
    transport->invalid_rx_log_count = 0;
    return RT_TRUE;
}
