/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_wifi.h"

#include <drivers/spi.h>
#include <drv_fpioa.h>
#include <drv_gpio.h>

#define DBG_TAG "esp_hosted.wifi.spi"
#define DBG_LVL ESP_HOSTED_WIFI_DBG_LVL
#include <rtdbg.h>

#define EHF_SPI_EVENT_WAKE       (1U << 0)
#define EHF_SPI_EVENT_STOP       (1U << 1)
#define EHF_SPI_MAX_BURST        32
#define EHF_SPI_WORD_SIZE        4U
#define EHF_SPI_FRAME_WORDS      (EHF_SPI_FRAME_SIZE / EHF_SPI_WORD_SIZE)
#define EHF_SPI_DIAGNOSTIC_MS    1000U
#define EHF_SPI_TX_WAIT_MS       20U
#define EHF_SPI_STOP_TIMEOUT_MS  1000U
#define EHF_SPI_RX_BACKOFF_MS    10U
#define EHF_SPI_MQ_POOL_SIZE     \
    (ESP_HOSTED_WIFI_TX_QUEUE_DEPTH * \
     (RT_ALIGN(sizeof(struct ehf_spi_tx_item), RT_ALIGN_SIZE) + sizeof(void *)))

_Static_assert(EHF_SPI_FRAME_SIZE % EHF_SPI_WORD_SIZE == 0,
               "SPI frame must be aligned to the 32-bit controller width");

#ifdef ESP_HOSTED_WIFI_HANDSHAKE_ACTIVE_LOW
#define EHF_SPI_HANDSHAKE_POLARITY "active-low"
#else
#define EHF_SPI_HANDSHAKE_POLARITY "active-high"
#endif

#ifdef ESP_HOSTED_WIFI_DATA_READY_ACTIVE_LOW
#define EHF_SPI_DATA_READY_POLARITY "active-low"
#else
#define EHF_SPI_DATA_READY_POLARITY "active-high"
#endif

#ifdef ESP_HOSTED_WIFI_RESET_ACTIVE_LOW
#define EHF_SPI_RESET_POLARITY "active-low"
#else
#define EHF_SPI_RESET_POLARITY "active-high"
#endif

#if defined(ESP_HOSTED_WIFI_SPI_BUS_SPI1)
#define EHF_SPI_BUS_NAME "spi1"
#define EHF_SPI_CLK_FUNC QSPI0_CLK
#define EHF_SPI_D0_FUNC  QSPI0_D0
#define EHF_SPI_D1_FUNC  QSPI0_D1
#elif defined(ESP_HOSTED_WIFI_SPI_BUS_SPI2)
#define EHF_SPI_BUS_NAME "spi2"
#define EHF_SPI_CLK_FUNC QSPI1_CLK
#define EHF_SPI_D0_FUNC  QSPI1_D0
#define EHF_SPI_D1_FUNC  QSPI1_D1
#else
#define EHF_SPI_BUS_NAME "spi0"
#define EHF_SPI_CLK_FUNC OSPI_CLK
#define EHF_SPI_D0_FUNC  OSPI_D0
#define EHF_SPI_D1_FUNC  OSPI_D1
#endif

struct ehf_spi_tx_item
{
    rt_uint8_t *data;
    rt_uint16_t length;
};

struct ehf_spi_context
{
    struct rt_wlan_offload_bus bus;
    struct rt_qspi_device device;
    struct rt_event event;
    struct rt_messagequeue tx_queue;
    struct rt_completion thread_stopped;
    struct rt_completion worker_idle;
    rt_thread_t thread;
    volatile rt_bool_t active;
    volatile rt_bool_t terminate;
    rt_bool_t thread_started;
    rt_bool_t device_attached;
    rt_bool_t handshake_irq_attached;
    rt_bool_t data_ready_irq_attached;
    rt_bool_t boot_sync_pending;
    rt_bool_t saw_handshake_inactive;
    rt_bool_t first_transfer_seen;
    rt_bool_t first_rx_seen;
    rt_uint8_t empty_rx_streak;
    rt_tick_t last_diagnostic_tick;
    rt_uint8_t mq_pool[EHF_SPI_MQ_POOL_SIZE]
        __attribute__((aligned(RT_ALIGN_SIZE)));
    rt_uint8_t rx_frame[EHF_SPI_FRAME_SIZE] __attribute__((aligned(64)));
    rt_uint32_t tx_words[EHF_SPI_FRAME_WORDS] __attribute__((aligned(64)));
    rt_uint32_t rx_words[EHF_SPI_FRAME_WORDS] __attribute__((aligned(64)));
};

static struct ehf_spi_context g_ehf_spi;

static void ehf_spi_log_configuration(void)
{
    LOG_I("bus=%s device=%s mode=%d clock=%d Hz data-width=32",
          EHF_SPI_BUS_NAME, ESP_HOSTED_WIFI_SPI_DEVICE_NAME,
          ESP_HOSTED_WIFI_SPI_MODE, ESP_HOSTED_WIFI_SPI_MAX_HZ);
    LOG_I("pins: CS=%d CLK=%d MOSI=%d MISO=%d",
          ESP_HOSTED_WIFI_SPI_CS_PIN,
          ESP_HOSTED_WIFI_SPI_CLK_PIN,
          ESP_HOSTED_WIFI_SPI_D0_PIN,
          ESP_HOSTED_WIFI_SPI_D1_PIN);
    LOG_I("pins: HS=%d (%s) DR=%d (%s) RESET=%d (%s)",
          ESP_HOSTED_WIFI_HANDSHAKE_PIN,
          EHF_SPI_HANDSHAKE_POLARITY,
          ESP_HOSTED_WIFI_DATA_READY_PIN,
          EHF_SPI_DATA_READY_POLARITY,
          ESP_HOSTED_WIFI_RESET_PIN,
          EHF_SPI_RESET_POLARITY);
}

static rt_bool_t ehf_spi_pin_active(int pin, rt_bool_t active_low)
{
    return kd_pin_read(pin) == (active_low ? GPIO_PV_LOW : GPIO_PV_HIGH);
}

static rt_bool_t ehf_spi_handshake_active(void)
{
#ifdef ESP_HOSTED_WIFI_HANDSHAKE_ACTIVE_LOW
    return ehf_spi_pin_active(ESP_HOSTED_WIFI_HANDSHAKE_PIN, RT_TRUE);
#else
    return ehf_spi_pin_active(ESP_HOSTED_WIFI_HANDSHAKE_PIN, RT_FALSE);
#endif
}

static rt_bool_t ehf_spi_data_ready(void)
{
#ifdef ESP_HOSTED_WIFI_DATA_READY_ACTIVE_LOW
    return ehf_spi_pin_active(ESP_HOSTED_WIFI_DATA_READY_PIN, RT_TRUE);
#else
    return ehf_spi_pin_active(ESP_HOSTED_WIFI_DATA_READY_PIN, RT_FALSE);
#endif
}

static void ehf_spi_log_link_state(struct ehf_spi_context *context,
                                   const char *reason)
{
    int handshake_raw = kd_pin_read(ESP_HOSTED_WIFI_HANDSHAKE_PIN);
    int data_ready_raw = kd_pin_read(ESP_HOSTED_WIFI_DATA_READY_PIN);

    LOG_I("%s: HS=%d (%s) DR=%d (%s) sync=%s rx=%s",
          reason, handshake_raw,
          ehf_spi_handshake_active() ? "active" : "inactive",
          data_ready_raw, ehf_spi_data_ready() ? "active" : "inactive",
          context->boot_sync_pending ? "waiting" : "done",
          context->first_rx_seen ? "seen" : "none");
}

static rt_bool_t ehf_spi_diagnostic_due(struct ehf_spi_context *context)
{
    rt_tick_t now = rt_tick_get();
    rt_tick_t interval = rt_tick_from_millisecond(EHF_SPI_DIAGNOSTIC_MS);

    if ((rt_tick_t)(now - context->last_diagnostic_tick) < interval)
    {
        return RT_FALSE;
    }
    context->last_diagnostic_tick = now;
    return RT_TRUE;
}

static void ehf_spi_irq(void *parameter)
{
    struct ehf_spi_context *context = parameter;

    rt_event_send(&context->event, EHF_SPI_EVENT_WAKE);
}

static rt_err_t ehf_spi_validate_pins(void)
{
    const int pins[] = {
        ESP_HOSTED_WIFI_SPI_CS_PIN,
        ESP_HOSTED_WIFI_SPI_CLK_PIN,
        ESP_HOSTED_WIFI_SPI_D0_PIN,
        ESP_HOSTED_WIFI_SPI_D1_PIN,
        ESP_HOSTED_WIFI_HANDSHAKE_PIN,
        ESP_HOSTED_WIFI_DATA_READY_PIN,
        ESP_HOSTED_WIFI_RESET_PIN,
    };
    rt_size_t first;
    rt_size_t second;

    for (first = 0; first < sizeof(pins) / sizeof(pins[0]); first++)
    {
        if (pins[first] < 0)
        {
            continue;
        }
        for (second = first + 1; second < sizeof(pins) / sizeof(pins[0]);
             second++)
        {
            if (pins[first] == pins[second])
            {
                LOG_E("GPIO %d is assigned to two ESP-Hosted signals",
                      pins[first]);
                return -RT_EINVAL;
            }
        }
    }
    return RT_EOK;
}

static rt_err_t ehf_spi_configure_gpio_irq(int pin, rt_bool_t active_low)
{
    rt_err_t result;

    kd_pin_mode(pin, active_low ? GPIO_DM_INPUT_PULLUP :
                                  GPIO_DM_INPUT_PULLDOWN);
    result = kd_pin_attach_irq(pin, active_low ? GPIO_PE_FALLING :
                                                GPIO_PE_RISING,
                               ehf_spi_irq, &g_ehf_spi);
    if (result == RT_EOK)
    {
        kd_pin_irq_enable(pin, RT_TRUE);
    }
    return result;
}

static rt_err_t ehf_spi_hardware_init(struct ehf_spi_context *context)
{
    struct rt_qspi_configuration configuration;
    rt_err_t result;

    result = ehf_spi_validate_pins();
    if (result != RT_EOK)
    {
        return result;
    }
    if (drv_fpioa_set_pin_func(ESP_HOSTED_WIFI_SPI_CLK_PIN,
                               EHF_SPI_CLK_FUNC) != 0 ||
        drv_fpioa_set_pin_func(ESP_HOSTED_WIFI_SPI_D0_PIN,
                               EHF_SPI_D0_FUNC) != 0 ||
        drv_fpioa_set_pin_func(ESP_HOSTED_WIFI_SPI_D1_PIN,
                               EHF_SPI_D1_FUNC) != 0)
    {
        LOG_E("cannot route pins to %s", EHF_SPI_BUS_NAME);
        return -RT_EINVAL;
    }
    if (ESP_HOSTED_WIFI_SPI_CS_PIN >= 0)
    {
        kd_pin_mode(ESP_HOSTED_WIFI_SPI_CS_PIN, GPIO_DM_OUTPUT);
        kd_pin_write(ESP_HOSTED_WIFI_SPI_CS_PIN, GPIO_PV_HIGH);
    }

    result = rt_spi_bus_attach_device(
        &context->device.parent, ESP_HOSTED_WIFI_SPI_DEVICE_NAME,
        EHF_SPI_BUS_NAME, RT_NULL);
    if (result != RT_EOK)
    {
        LOG_E("cannot attach %s to %s: %d",
              ESP_HOSTED_WIFI_SPI_DEVICE_NAME, EHF_SPI_BUS_NAME, result);
        return result;
    }
    context->device_attached = RT_TRUE;

    rt_memset(&configuration, 0, sizeof(configuration));
    configuration.parent.mode = RT_SPI_MSB;
    switch (ESP_HOSTED_WIFI_SPI_MODE)
    {
    case 1: configuration.parent.mode |= RT_SPI_MODE_1; break;
    case 2: configuration.parent.mode |= RT_SPI_MODE_2; break;
    default: configuration.parent.mode |= RT_SPI_MODE_3; break;
    }
    if (ESP_HOSTED_WIFI_SPI_CS_PIN >= 0)
    {
        configuration.parent.soft_cs =
            0x80 | ESP_HOSTED_WIFI_SPI_CS_PIN;
    }
    configuration.parent.data_width = 32;
    configuration.parent.max_hz = ESP_HOSTED_WIFI_SPI_MAX_HZ;
    configuration.qspi_dl_width = 1;
    result = rt_qspi_configure(&context->device, &configuration);
    if (result != RT_EOK)
    {
        return result;
    }

#ifdef ESP_HOSTED_WIFI_HANDSHAKE_ACTIVE_LOW
    result = ehf_spi_configure_gpio_irq(
        ESP_HOSTED_WIFI_HANDSHAKE_PIN, RT_TRUE);
#else
    result = ehf_spi_configure_gpio_irq(
        ESP_HOSTED_WIFI_HANDSHAKE_PIN, RT_FALSE);
#endif
    if (result != RT_EOK)
    {
        return result;
    }
    context->handshake_irq_attached = RT_TRUE;
#ifdef ESP_HOSTED_WIFI_DATA_READY_ACTIVE_LOW
    result = ehf_spi_configure_gpio_irq(
        ESP_HOSTED_WIFI_DATA_READY_PIN, RT_TRUE);
#else
    result = ehf_spi_configure_gpio_irq(
        ESP_HOSTED_WIFI_DATA_READY_PIN, RT_FALSE);
#endif
    if (result == RT_EOK)
    {
        context->data_ready_irq_attached = RT_TRUE;
    }
    return result;
}

static void ehf_spi_hardware_deinit(struct ehf_spi_context *context)
{
    if (context->data_ready_irq_attached)
    {
        kd_pin_irq_enable(ESP_HOSTED_WIFI_DATA_READY_PIN, RT_FALSE);
        kd_pin_detach_irq(ESP_HOSTED_WIFI_DATA_READY_PIN);
        context->data_ready_irq_attached = RT_FALSE;
    }
    if (context->handshake_irq_attached)
    {
        kd_pin_irq_enable(ESP_HOSTED_WIFI_HANDSHAKE_PIN, RT_FALSE);
        kd_pin_detach_irq(ESP_HOSTED_WIFI_HANDSHAKE_PIN);
        context->handshake_irq_attached = RT_FALSE;
    }
    if (context->device_attached)
    {
        rt_device_unregister(&context->device.parent.parent);
        context->device_attached = RT_FALSE;
    }
}

static void ehf_spi_reset_target(void)
{
    int asserted;
    int released;

    if (ESP_HOSTED_WIFI_RESET_PIN < 0)
    {
        return;
    }
#ifdef ESP_HOSTED_WIFI_RESET_ACTIVE_LOW
    asserted = GPIO_PV_LOW;
#else
    asserted = GPIO_PV_HIGH;
#endif
    released = asserted == GPIO_PV_LOW ? GPIO_PV_HIGH : GPIO_PV_LOW;
    kd_pin_mode(ESP_HOSTED_WIFI_RESET_PIN, GPIO_DM_OUTPUT);
    kd_pin_write(ESP_HOSTED_WIFI_RESET_PIN, released);
    rt_thread_mdelay(10);
    kd_pin_write(ESP_HOSTED_WIFI_RESET_PIN, asserted);
    rt_thread_mdelay(ESP_HOSTED_WIFI_RESET_PULSE_MS);
    kd_pin_write(ESP_HOSTED_WIFI_RESET_PIN, released);
}

static rt_size_t ehf_spi_exchange(struct ehf_spi_context *context,
                                  const struct ehf_spi_tx_item *item)
{
    struct rt_qspi_message message;
    rt_size_t index;
    rt_size_t result;

    rt_memset(context->tx_words, 0, sizeof(context->tx_words));
    if (item)
    {
        rt_size_t full_words = item->length / EHF_SPI_WORD_SIZE;
        rt_size_t remaining = item->length % EHF_SPI_WORD_SIZE;

        for (index = 0; index < full_words; index++)
        {
            const rt_uint8_t *source = item->data +
                                       index * EHF_SPI_WORD_SIZE;

            context->tx_words[index] = ((rt_uint32_t)source[0] << 24) |
                                       ((rt_uint32_t)source[1] << 16) |
                                       ((rt_uint32_t)source[2] << 8) |
                                       source[3];
        }
        if (remaining)
        {
            const rt_uint8_t *source = item->data +
                                       full_words * EHF_SPI_WORD_SIZE;
            rt_uint32_t word = 0;

            for (index = 0; index < remaining; index++)
            {
                word |= (rt_uint32_t)source[index] << (24U - index * 8U);
            }
            context->tx_words[full_words] = word;
        }
    }
    else
    {
        context->tx_words[0] = 0xff000000U;
    }

    rt_memset(&message, 0, sizeof(message));
    message.parent.send_buf = context->tx_words;
    message.parent.recv_buf = context->rx_words;
    message.parent.length = EHF_SPI_FRAME_SIZE;
    message.parent.cs_take = ESP_HOSTED_WIFI_SPI_CS_PIN >= 0;
    message.parent.cs_release = ESP_HOSTED_WIFI_SPI_CS_PIN >= 0;
    message.qspi_data_lines = 1;
    result = rt_qspi_transfer_message(&context->device, &message);
    if (result != EHF_SPI_FRAME_SIZE)
    {
        return result;
    }

    return result;
}

static void ehf_spi_unpack_rx(struct ehf_spi_context *context)
{
    rt_size_t index;

    for (index = 0; index < EHF_SPI_FRAME_WORDS; index++)
    {
        rt_uint32_t word = context->rx_words[index];
        rt_uint8_t *destination = context->rx_frame +
                                  index * EHF_SPI_WORD_SIZE;

        destination[0] = word >> 24;
        destination[1] = word >> 16;
        destination[2] = word >> 8;
        destination[3] = word;
    }
}

static rt_bool_t ehf_spi_rx_is_constant(struct ehf_spi_context *context,
                                        rt_uint32_t value)
{
    rt_size_t index;

    for (index = 0; index < EHF_SPI_FRAME_WORDS; index++)
    {
        if (context->rx_words[index] != value)
        {
            return RT_FALSE;
        }
    }
    return RT_TRUE;
}

static void ehf_spi_worker(void *parameter)
{
    struct ehf_spi_context *context = parameter;
    struct ehf_spi_tx_item pending;
    rt_bool_t have_pending = RT_FALSE;

    rt_memset(&pending, 0, sizeof(pending));
    while (!context->terminate)
    {
        rt_uint32_t events;
        int burst;

        rt_event_recv(&context->event,
                      EHF_SPI_EVENT_WAKE | EHF_SPI_EVENT_STOP,
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

        for (burst = 0; burst < EHF_SPI_MAX_BURST && context->active; burst++)
        {
            rt_bool_t data_ready;
            rt_bool_t sent_frame;
            rt_bool_t rx_available;
            rt_err_t rx_result = -RT_EEMPTY;
            rt_size_t transferred;

            if (!context->first_rx_seen && ehf_spi_diagnostic_due(context))
            {
                ehf_spi_log_link_state(context, "waiting for ESP boot data");
            }

            if (context->boot_sync_pending)
            {
                if (!ehf_spi_handshake_active())
                {
                    if (!context->saw_handshake_inactive)
                    {
                        LOG_I("ESP handshake inactive after reset");
                    }
                    context->saw_handshake_inactive = RT_TRUE;
                    break;
                }
                if (!context->saw_handshake_inactive)
                {
                    break;
                }
                context->boot_sync_pending = RT_FALSE;
                LOG_I("ESP handshake active; SPI transport synchronized");
            }

            if (!have_pending &&
                rt_mq_recv(&context->tx_queue, &pending, sizeof(pending), 0) ==
                    RT_EOK)
            {
                have_pending = RT_TRUE;
            }
            data_ready = ehf_spi_data_ready();
            /* DR and the TX queue are the only reasons to clock a 1600-byte
             * transaction. An extra transaction after every frame wastes up
             * to half of the SPI bandwidth. */
            if (!have_pending && !data_ready)
            {
                context->empty_rx_streak = 0;
                break;
            }
            if (!ehf_spi_handshake_active())
            {
                break;
            }
            sent_frame = have_pending;
            transferred = ehf_spi_exchange(
                context, have_pending ? &pending : RT_NULL);
            if (transferred != EHF_SPI_FRAME_SIZE)
            {
                LOG_E("full-duplex transfer failed: %u/%u bytes",
                      (unsigned int)transferred,
                      (unsigned int)EHF_SPI_FRAME_SIZE);
                if (have_pending)
                {
                    rt_free(pending.data);
                    have_pending = RT_FALSE;
                }
                rt_wlan_offload_bus_notify(&context->bus,
                                      RT_WLAN_OFFLOAD_BUS_EVENT_ERROR, -RT_EIO);
                rt_thread_mdelay(100);
                break;
            }
            if (!context->first_transfer_seen)
            {
                context->first_transfer_seen = RT_TRUE;
                LOG_I("first SPI transfer completed");
            }
            if (have_pending)
            {
                rt_free(pending.data);
                have_pending = RT_FALSE;
            }
            rx_available = !ehf_spi_rx_is_constant(context, 0) &&
                           !ehf_spi_rx_is_constant(context, 0xffffffffU);
            if (rx_available)
            {
                ehf_spi_unpack_rx(context);
                rx_result = rt_wlan_offload_bus_rx(&context->bus,
                                              context->rx_frame,
                                              sizeof(context->rx_frame));
                context->empty_rx_streak = 0;
                if (rx_result == RT_EOK && !context->first_rx_seen)
                {
                    context->first_rx_seen = RT_TRUE;
                    LOG_D("ESP data received: %02x %02x %02x %02x",
                          context->rx_frame[0], context->rx_frame[1],
                          context->rx_frame[2], context->rx_frame[3]);
                }
            }
            if (rx_available && rx_result != RT_EOK &&
                rx_result != -RT_EEMPTY)
            {
                rt_thread_mdelay(EHF_SPI_RX_BACKOFF_MS);
                break;
            }
            if (!sent_frame && rx_result != RT_EOK)
            {
                /* FG can announce data while one prequeued dummy is ahead of
                 * it. Retry that case immediately once, then back off a stuck
                 * DR signal so the worker cannot spin at 100% CPU. */
                if (context->empty_rx_streak != 0xffU)
                {
                    context->empty_rx_streak++;
                }
                if (context->empty_rx_streak > 1U)
                {
                    rt_thread_mdelay(EHF_SPI_RX_BACKOFF_MS);
                }
                break;
            }
        }
    }
    if (have_pending)
    {
        rt_free(pending.data);
    }
    rt_completion_done(&context->thread_stopped);
}

static void ehf_spi_stop_worker(struct ehf_spi_context *context)
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
        rt_event_send(&context->event, EHF_SPI_EVENT_STOP);
        rt_completion_wait(&context->thread_stopped, RT_WAITING_FOREVER);
    }
    context->thread = RT_NULL;
    context->thread_started = RT_FALSE;
}

static void ehf_spi_drain_queue(struct ehf_spi_context *context)
{
    struct ehf_spi_tx_item item;

    while (rt_mq_recv(&context->tx_queue, &item, sizeof(item), 0) == RT_EOK)
    {
        rt_free(item.data);
    }
}

static rt_err_t ehf_spi_bus_start(struct rt_wlan_offload_bus *bus)
{
    struct ehf_spi_context *context = rt_wlan_offload_bus_get_driver_data(bus);

    ehf_bus_prepare(bus);
    ehf_spi_drain_queue(context);
    context->boot_sync_pending = ESP_HOSTED_WIFI_RESET_PIN >= 0;
    context->saw_handshake_inactive = RT_FALSE;
    context->first_transfer_seen = RT_FALSE;
    context->first_rx_seen = RT_FALSE;
    context->empty_rx_streak = 0;
    context->last_diagnostic_tick = rt_tick_get();
    LOG_I("starting ESP link and resetting target");
    ehf_spi_reset_target();
    ehf_spi_log_link_state(context, "after reset");
    context->active = RT_TRUE;
    rt_event_send(&context->event, EHF_SPI_EVENT_WAKE);
    return RT_EOK;
}

static rt_err_t ehf_spi_bus_stop(struct rt_wlan_offload_bus *bus)
{
    struct ehf_spi_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    rt_err_t result;

    if (context->thread && context->thread_started && !context->terminate)
    {
        rt_completion_init(&context->worker_idle);
    }
    context->active = RT_FALSE;
    rt_event_send(&context->event, EHF_SPI_EVENT_STOP);
    if (context->thread && context->thread_started && !context->terminate)
    {
        result = rt_completion_wait(
            &context->worker_idle,
            rt_tick_from_millisecond(EHF_SPI_STOP_TIMEOUT_MS));
        if (result != RT_EOK)
        {
            LOG_E("worker did not stop within %u ms", EHF_SPI_STOP_TIMEOUT_MS);
            return result;
        }
    }
    ehf_spi_drain_queue(context);
    return RT_EOK;
}

static rt_err_t ehf_spi_bus_transmit(struct rt_wlan_offload_bus *bus,
                                     const void *data, rt_size_t length)
{
    struct ehf_spi_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    struct ehf_spi_tx_item item;
    rt_err_t result;

    if (!context->active || !data || !length || length > EHF_SPI_FRAME_SIZE)
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
    /* Backpressure the network stack briefly instead of dropping a TCP burst
     * when its congestion window is larger than the transport queue. */
    result = rt_mq_send_wait(&context->tx_queue, &item, sizeof(item),
                             rt_tick_from_millisecond(EHF_SPI_TX_WAIT_MS));
    if (result != RT_EOK)
    {
        rt_free(item.data);
        return result;
    }
    rt_event_send(&context->event, EHF_SPI_EVENT_WAKE);
    return RT_EOK;
}

static rt_err_t ehf_spi_bus_reset(struct rt_wlan_offload_bus *bus)
{
    (void)bus;
    ehf_spi_reset_target();
    return RT_EOK;
}

static const struct rt_wlan_offload_bus_ops g_ehf_spi_bus_ops = {
    .start = ehf_spi_bus_start,
    .stop = ehf_spi_bus_stop,
    .transmit = ehf_spi_bus_transmit,
    .reset = ehf_spi_bus_reset,
};

rt_err_t ehf_spi_driver_init(void)
{
    struct ehf_spi_context *context = &g_ehf_spi;
    struct rt_wlan_offload_bus_config bus_config;
    rt_err_t result;

    rt_memset(context, 0, sizeof(*context));
    result = rt_event_init(&context->event, "ehf-spi", RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mq_init(&context->tx_queue, "ehf-tx", context->mq_pool,
                        sizeof(struct ehf_spi_tx_item),
                        sizeof(context->mq_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        goto detach_event;
    }
    rt_completion_init(&context->thread_stopped);
    rt_completion_init(&context->worker_idle);
    result = ehf_spi_hardware_init(context);
    if (result != RT_EOK)
    {
        goto cleanup_hardware;
    }
    ehf_spi_log_configuration();

    context->thread = rt_thread_create(
        "ehf-spi", ehf_spi_worker, context,
        ESP_HOSTED_WIFI_THREAD_STACK_SIZE,
        ESP_HOSTED_WIFI_THREAD_PRIORITY, 10);
    if (!context->thread)
    {
        result = -RT_ENOMEM;
        goto cleanup_hardware;
    }
    result = rt_thread_startup(context->thread);
    if (result != RT_EOK)
    {
        goto stop_worker;
    }
    context->thread_started = RT_TRUE;

    rt_memset(&bus_config, 0, sizeof(bus_config));
    bus_config.type = RT_WLAN_OFFLOAD_BUS_SPI;
    bus_config.ops = &g_ehf_spi_bus_ops;
    bus_config.capabilities = RT_WLAN_OFFLOAD_BUS_CAP_PACKET |
                              RT_WLAN_OFFLOAD_BUS_CAP_FULL_DUPLEX;
    bus_config.max_tx_size = EHF_SPI_FRAME_SIZE;
    bus_config.max_rx_size = EHF_SPI_FRAME_SIZE;
    bus_config.alignment = EHF_TRANSPORT_ALIGNMENT;
    bus_config.driver_data = context;
    result = rt_wlan_offload_bus_init(&context->bus, &bus_config);
    if (result != RT_EOK)
    {
        goto stop_worker;
    }
    result = ehf_attach_bus(&context->bus);
    if (result == RT_EOK)
    {
        return RT_EOK;
    }
    rt_wlan_offload_bus_deinit(&context->bus);

stop_worker:
    ehf_spi_stop_worker(context);
cleanup_hardware:
    ehf_spi_hardware_deinit(context);
    rt_mq_detach(&context->tx_queue);
detach_event:
    rt_event_detach(&context->event);
    return result;
}
