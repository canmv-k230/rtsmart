/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_wifi.h"

#include <drivers/sdio.h>

#define DBG_TAG "esp_hosted.wifi.sdio"
#define DBG_LVL ESP_HOSTED_WIFI_DBG_LVL
#include <rtdbg.h>

#define EHF_SDIO_FUNCTION             1
#define EHF_SDIO_BLOCK_SIZE           512U
#define EHF_SDIO_FIFO_END             0x1f800U
#define EHF_SDIO_ADDRESS_MASK         0x3ffU
#define EHF_SDIO_PACKET_LENGTH_REG    0x60U
#define EHF_SDIO_TOKEN_REG            0x44U
#define EHF_SDIO_INTERRUPT_STATUS_REG 0x58U
#define EHF_SDIO_INTERRUPT_CLEAR_REG  0xd4U
#define EHF_SDIO_SCRATCH7_REG         0x8cU
#define EHF_SDIO_RX_COUNTER_MAX       0x100000U
#define EHF_SDIO_RX_COUNTER_MASK      0xfffffU
#define EHF_SDIO_TX_COUNTER_MAX       0x1000U
#define EHF_SDIO_TX_COUNTER_MASK      0xfffU
#define EHF_SDIO_MAX_CREDITS          10U
#define EHF_SDIO_EVENT_WAKE           (1U << 0)
#define EHF_SDIO_EVENT_STOP           (1U << 1)
#define EHF_SDIO_MAX_RX_BURST         16
#define EHF_SDIO_STOP_TIMEOUT_MS      1000U
#define EHF_SDIO_MQ_POOL_SIZE         \
    (ESP_HOSTED_WIFI_TX_QUEUE_DEPTH * \
     (RT_ALIGN(sizeof(struct ehf_sdio_tx_item), RT_ALIGN_SIZE) + sizeof(void *)))

struct ehf_sdio_tx_item
{
    rt_uint8_t *data;
    rt_uint16_t length;
};

struct ehf_sdio_context
{
    struct rt_wlan_offload_bus bus;
    struct rt_sdio_function *function;
    struct rt_event event;
    struct rt_messagequeue tx_queue;
    struct rt_completion thread_stopped;
    struct rt_completion worker_idle;
    rt_thread_t thread;
    volatile rt_bool_t active;
    volatile rt_bool_t terminate;
    rt_bool_t thread_started;
    rt_bool_t rx_initialized;
    rt_uint32_t rx_byte_count;
    rt_uint32_t tx_buffer_count;
    rt_uint8_t mq_pool[EHF_SDIO_MQ_POOL_SIZE]
        __attribute__((aligned(RT_ALIGN_SIZE)));
};

static struct ehf_sdio_context g_ehf_sdio;

static rt_uint32_t ehf_sdio_read_register(struct ehf_sdio_context *context,
                                          rt_uint32_t address,
                                          rt_err_t *result)
{
    rt_int32_t error = RT_EOK;
    rt_uint32_t value;

    value = sdio_io_readl(context->function,
                          address & EHF_SDIO_ADDRESS_MASK, &error);
    if (result)
    {
        *result = error;
    }
    return value;
}

static rt_err_t ehf_sdio_write_register(struct ehf_sdio_context *context,
                                         rt_uint32_t address,
                                         rt_uint32_t value)
{
    return sdio_io_writel(context->function, value,
                          address & EHF_SDIO_ADDRESS_MASK);
}

static void ehf_sdio_irq(struct rt_sdio_function *function)
{
    struct ehf_sdio_context *context = sdio_get_drvdata(function);

    if (context)
    {
        rt_event_send(&context->event, EHF_SDIO_EVENT_WAKE);
    }
}

static rt_err_t ehf_sdio_receive_one(struct ehf_sdio_context *context)
{
    rt_uint32_t current;
    rt_uint32_t available;
    rt_uint32_t aligned_length;
    rt_uint8_t *buffer;
    rt_err_t result;

    current = ehf_sdio_read_register(context,
                                     EHF_SDIO_PACKET_LENGTH_REG, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    current &= EHF_SDIO_RX_COUNTER_MASK;
    available = (current + EHF_SDIO_RX_COUNTER_MAX -
                 context->rx_byte_count) % EHF_SDIO_RX_COUNTER_MAX;
    if (!available)
    {
        return -RT_EEMPTY;
    }
    if (available > ESP_HOSTED_WIFI_SDIO_RX_BUFFER_SIZE)
    {
        if (current <= ESP_HOSTED_WIFI_SDIO_RX_BUFFER_SIZE)
        {
            LOG_W("RX counter reset from %u to %u", context->rx_byte_count,
                  current);
            context->rx_byte_count = 0;
            available = current;
        }
    }
    if (available > ESP_HOSTED_WIFI_SDIO_RX_BUFFER_SIZE)
    {
        LOG_E("RX aggregate %u exceeds configured maximum %u", available,
              ESP_HOSTED_WIFI_SDIO_RX_BUFFER_SIZE);
        return -RT_EFULL;
    }

    aligned_length = RT_ALIGN(available, EHF_TRANSPORT_ALIGNMENT);
    buffer = rt_malloc(aligned_length);
    if (!buffer)
    {
        return -RT_ENOMEM;
    }
    result = sdio_io_read_multi_incr_b(
        context->function, EHF_SDIO_FIFO_END - available,
        buffer, aligned_length);
    if (result == RT_EOK)
    {
        context->rx_byte_count =
            (context->rx_byte_count + available) % EHF_SDIO_RX_COUNTER_MAX;
        rt_wlan_offload_bus_rx(&context->bus, buffer, available);
    }
    rt_free(buffer);
    return result;
}

static rt_uint32_t ehf_sdio_available_credits(
    struct ehf_sdio_context *context, rt_err_t *result)
{
    rt_uint32_t token = ehf_sdio_read_register(context, EHF_SDIO_TOKEN_REG,
                                                result);

    token = (token >> 16) & EHF_SDIO_TX_COUNTER_MASK;
    return (token + EHF_SDIO_TX_COUNTER_MAX - context->tx_buffer_count) %
           EHF_SDIO_TX_COUNTER_MAX;
}

static rt_err_t ehf_sdio_send_one(struct ehf_sdio_context *context,
                                  const struct ehf_sdio_tx_item *item)
{
    struct ehf_context *radio_context = context->bus.callback_parameter;
    rt_uint32_t token_size;
    rt_uint32_t needed;
    rt_uint32_t available;
    rt_uint32_t write_length;
    rt_uint8_t *buffer;
    rt_err_t result;

    token_size = radio_context && radio_context->sdio_token_size ?
                 radio_context->sdio_token_size :
                 ESP_HOSTED_WIFI_SDIO_TOKEN_SIZE;
    needed = (item->length + token_size - 1) / token_size;
    available = ehf_sdio_available_credits(context, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    if (available < needed)
    {
        return -RT_EBUSY;
    }

    write_length = RT_ALIGN(item->length, EHF_SDIO_BLOCK_SIZE);
    buffer = rt_calloc(1, write_length);
    if (!buffer)
    {
        return -RT_ENOMEM;
    }
    rt_memcpy(buffer, item->data, item->length);
    result = sdio_io_write_multi_incr_b(
        context->function, EHF_SDIO_FIFO_END - write_length,
        buffer, write_length);
    rt_free(buffer);
    if (result == RT_EOK)
    {
        context->tx_buffer_count =
            (context->tx_buffer_count + needed) % EHF_SDIO_TX_COUNTER_MAX;
    }
    return result;
}

static void ehf_sdio_worker(void *parameter)
{
    struct ehf_sdio_context *context = parameter;
    struct ehf_sdio_tx_item pending;
    rt_bool_t have_pending = RT_FALSE;

    rt_memset(&pending, 0, sizeof(pending));
    while (!context->terminate)
    {
        rt_uint32_t events;
        rt_uint32_t interrupt_status;
        rt_err_t result;
        int count;

        rt_event_recv(&context->event,
                      EHF_SDIO_EVENT_WAKE | EHF_SDIO_EVENT_STOP,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      rt_tick_from_millisecond(10), &events);
        if (!context->active)
        {
            if (have_pending)
            {
                rt_free(pending.data);
                have_pending = RT_FALSE;
            }
            rt_completion_done(&context->worker_idle);
            continue;
        }

        interrupt_status = ehf_sdio_read_register(
            context, EHF_SDIO_INTERRUPT_STATUS_REG, &result);
        if (result == RT_EOK && interrupt_status)
        {
            ehf_sdio_write_register(context,
                                    EHF_SDIO_INTERRUPT_CLEAR_REG,
                                    interrupt_status);
        }
        for (count = 0; count < EHF_SDIO_MAX_RX_BURST; count++)
        {
            result = ehf_sdio_receive_one(context);
            if (result == -RT_EEMPTY)
            {
                break;
            }
            if (result != RT_EOK)
            {
                rt_wlan_offload_bus_notify(&context->bus,
                                      RT_WLAN_OFFLOAD_BUS_EVENT_ERROR, result);
                break;
            }
        }

        if (!have_pending &&
            rt_mq_recv(&context->tx_queue, &pending, sizeof(pending), 0) ==
                RT_EOK)
        {
            have_pending = RT_TRUE;
        }
        if (have_pending)
        {
            result = ehf_sdio_send_one(context, &pending);
            if (result == RT_EOK)
            {
                rt_free(pending.data);
                have_pending = RT_FALSE;
            }
            else if (result != -RT_EBUSY)
            {
                rt_free(pending.data);
                have_pending = RT_FALSE;
                rt_wlan_offload_bus_notify(&context->bus,
                                      RT_WLAN_OFFLOAD_BUS_EVENT_ERROR, result);
            }
        }
    }
    if (have_pending)
    {
        rt_free(pending.data);
    }
    rt_completion_done(&context->thread_stopped);
}

static void ehf_sdio_stop_worker(struct ehf_sdio_context *context)
{
    if (!context->thread)
    {
        return;
    }
    if (!context->thread_started)
    {
        rt_thread_delete(context->thread);
    }
    else
    {
        context->terminate = RT_TRUE;
        rt_event_send(&context->event, EHF_SDIO_EVENT_STOP);
        rt_completion_wait(&context->thread_stopped, RT_WAITING_FOREVER);
    }
    context->thread = RT_NULL;
    context->thread_started = RT_FALSE;
}

static void ehf_sdio_drain_queue(struct ehf_sdio_context *context)
{
    struct ehf_sdio_tx_item item;

    while (rt_mq_recv(&context->tx_queue, &item, sizeof(item), 0) == RT_EOK)
    {
        rt_free(item.data);
    }
}

static rt_err_t ehf_sdio_bus_start(struct rt_wlan_offload_bus *bus)
{
    struct ehf_sdio_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    rt_uint32_t current;
    rt_uint32_t token;
    rt_err_t result;

    ehf_bus_prepare(bus);
    ehf_sdio_drain_queue(context);
    current = ehf_sdio_read_register(context, EHF_SDIO_PACKET_LENGTH_REG,
                                     &result) & EHF_SDIO_RX_COUNTER_MASK;
    if (result != RT_EOK)
    {
        return result;
    }
    if (!context->rx_initialized)
    {
        context->rx_byte_count =
            current <= ESP_HOSTED_WIFI_SDIO_RX_BUFFER_SIZE ? 0 : current;
        context->rx_initialized = RT_TRUE;
    }
    token = ehf_sdio_read_register(context, EHF_SDIO_TOKEN_REG, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    token = (token >> 16) & EHF_SDIO_TX_COUNTER_MASK;
    context->tx_buffer_count = token >= EHF_SDIO_MAX_CREDITS ?
                               token - EHF_SDIO_MAX_CREDITS : 0;
    context->active = RT_TRUE;
    result = sdio_io_writeb(context->function, EHF_SDIO_SCRATCH7_REG, 1);
    if (result != RT_EOK)
    {
        context->active = RT_FALSE;
        return result;
    }
    rt_event_send(&context->event, EHF_SDIO_EVENT_WAKE);
    return RT_EOK;
}

static rt_err_t ehf_sdio_bus_stop(struct rt_wlan_offload_bus *bus)
{
    struct ehf_sdio_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    rt_err_t result;

    if (context->thread && context->thread_started && !context->terminate)
    {
        rt_completion_init(&context->worker_idle);
    }
    context->active = RT_FALSE;
    rt_event_send(&context->event, EHF_SDIO_EVENT_STOP);
    if (context->thread && context->thread_started && !context->terminate)
    {
        result = rt_completion_wait(
            &context->worker_idle,
            rt_tick_from_millisecond(EHF_SDIO_STOP_TIMEOUT_MS));
        if (result != RT_EOK)
        {
            LOG_E("worker did not stop within %u ms", EHF_SDIO_STOP_TIMEOUT_MS);
            return result;
        }
    }
    ehf_sdio_drain_queue(context);
    return RT_EOK;
}

static rt_err_t ehf_sdio_bus_transmit(struct rt_wlan_offload_bus *bus,
                                      const void *data, rt_size_t length)
{
    struct ehf_sdio_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    struct ehf_sdio_tx_item item;
    rt_err_t result;

    if (!context->active || !data || !length ||
        length > ESP_HOSTED_WIFI_SDIO_RX_BUFFER_SIZE || length > 0xffffU)
    {
        return -RT_EINVAL;
    }
    item.data = rt_malloc(length);
    if (!item.data)
    {
        return -RT_ENOMEM;
    }
    rt_memcpy(item.data, data, length);
    item.length = length;
    result = rt_mq_send(&context->tx_queue, &item, sizeof(item));
    if (result != RT_EOK)
    {
        rt_free(item.data);
        return result;
    }
    rt_event_send(&context->event, EHF_SDIO_EVENT_WAKE);
    return RT_EOK;
}

static const struct rt_wlan_offload_bus_ops g_ehf_sdio_bus_ops = {
    .start = ehf_sdio_bus_start,
    .stop = ehf_sdio_bus_stop,
    .transmit = ehf_sdio_bus_transmit,
};

static rt_int32_t ehf_sdio_probe(struct rt_mmcsd_card *card)
{
    struct ehf_sdio_context *context = &g_ehf_sdio;
    struct rt_wlan_offload_bus_config bus_config;
    rt_err_t result;

    if (!card || card->sdio_function_num < EHF_SDIO_FUNCTION ||
        !card->sdio_function[EHF_SDIO_FUNCTION] || context->function)
    {
        return -RT_EINVAL;
    }
    rt_memset(context, 0, sizeof(*context));
    context->function = card->sdio_function[EHF_SDIO_FUNCTION];
    sdio_set_drvdata(context->function, context);
    result = sdio_enable_func(context->function);
    if (result != RT_EOK)
    {
        goto fail;
    }
    result = sdio_set_block_size(context->function, EHF_SDIO_BLOCK_SIZE);
    if (result != RT_EOK)
    {
        goto disable;
    }
    result = rt_event_init(&context->event, "ehf-sdio", RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        goto disable;
    }
    result = rt_mq_init(&context->tx_queue, "ehf-tx", context->mq_pool,
                        sizeof(struct ehf_sdio_tx_item),
                        sizeof(context->mq_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        goto detach_event;
    }
    rt_completion_init(&context->thread_stopped);
    rt_completion_init(&context->worker_idle);
    result = sdio_attach_irq(context->function, ehf_sdio_irq);
    if (result != RT_EOK)
    {
        goto detach_queue;
    }
    context->thread = rt_thread_create(
        "ehf-sdio", ehf_sdio_worker, context,
        ESP_HOSTED_WIFI_THREAD_STACK_SIZE,
        ESP_HOSTED_WIFI_THREAD_PRIORITY, 10);
    if (!context->thread)
    {
        result = -RT_ENOMEM;
        goto detach_irq;
    }
    result = rt_thread_startup(context->thread);
    if (result != RT_EOK)
    {
        goto stop_worker;
    }
    context->thread_started = RT_TRUE;

    rt_memset(&bus_config, 0, sizeof(bus_config));
    bus_config.type = RT_WLAN_OFFLOAD_BUS_SDIO;
    bus_config.ops = &g_ehf_sdio_bus_ops;
    bus_config.capabilities = RT_WLAN_OFFLOAD_BUS_CAP_PACKET |
                              RT_WLAN_OFFLOAD_BUS_CAP_HOTPLUG;
    bus_config.max_tx_size = ESP_HOSTED_WIFI_SDIO_RX_BUFFER_SIZE;
    bus_config.max_rx_size = ESP_HOSTED_WIFI_SDIO_RX_BUFFER_SIZE;
    bus_config.alignment = EHF_TRANSPORT_ALIGNMENT;
    bus_config.driver_data = context;
    result = rt_wlan_offload_bus_init(&context->bus, &bus_config);
    if (result != RT_EOK)
    {
        goto stop_worker;
    }
    result = ehf_attach_bus(&context->bus);
    if (result != RT_EOK)
    {
        rt_wlan_offload_bus_deinit(&context->bus);
        goto stop_worker;
    }
    LOG_I("bound SDIO %04x:%04x", card->cis.manufacturer,
          context->function->product);
    return RT_EOK;

stop_worker:
    ehf_sdio_stop_worker(context);
detach_irq:
    sdio_detach_irq(context->function);
detach_queue:
    rt_mq_detach(&context->tx_queue);
detach_event:
    rt_event_detach(&context->event);
disable:
    sdio_disable_func(context->function);
fail:
    sdio_set_drvdata(context->function, RT_NULL);
    context->function = RT_NULL;
    return result;
}

static rt_int32_t ehf_sdio_remove(struct rt_mmcsd_card *card)
{
    struct ehf_sdio_context *context = &g_ehf_sdio;
    struct rt_sdio_function *function;

    (void)card;
    if (!context->function)
    {
        return RT_EOK;
    }
    function = context->function;
    context->active = RT_FALSE;
    rt_wlan_offload_bus_notify(&context->bus,
                          RT_WLAN_OFFLOAD_BUS_EVENT_UNAVAILABLE, -RT_EIO);
    sdio_detach_irq(function);
    ehf_sdio_stop_worker(context);
    ehf_detach_bus(&context->bus);
    sdio_disable_func(function);
    sdio_set_drvdata(function, RT_NULL);
    rt_wlan_offload_bus_deinit(&context->bus);
    ehf_sdio_drain_queue(context);
    rt_mq_detach(&context->tx_queue);
    rt_event_detach(&context->event);
    rt_memset(context, 0, sizeof(*context));
    return RT_EOK;
}

static struct rt_sdio_device_id g_ehf_sdio_id = {
    SDIO_ANY_FUNC_ID,
    ESP_HOSTED_WIFI_SDIO_MANUFACTURER,
    ESP_HOSTED_WIFI_SDIO_PRODUCT,
};

static struct rt_sdio_driver g_ehf_sdio_driver = {
    "esp-hosted-wifi",
    ehf_sdio_probe,
    ehf_sdio_remove,
    &g_ehf_sdio_id,
};

rt_err_t ehf_sdio_driver_init(void)
{
    rt_err_t result = sdio_register_driver(&g_ehf_sdio_driver);

    return result == -RT_EEMPTY ? RT_EOK : result;
}
