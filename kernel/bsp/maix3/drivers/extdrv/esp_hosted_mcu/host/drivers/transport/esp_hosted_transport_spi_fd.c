/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_transport_spi.h"
#include "esp_hosted_mcu_log.h"

#ifdef ESP_HOSTED_TRANSPORT_SPI_FD

#define DBG_TAG "esp_hosted.spi.fd"
#define DBG_LVL ESP_HOSTED_MCU_DBG_LVL
#include <rtdbg.h>

#define EH_SPI_FD_DATA_WIDTH       32
#define EH_SPI_FD_WORD_SIZE        (EH_SPI_FD_DATA_WIDTH / 8)
#define EH_SPI_FD_FRAME_WORDS      (ESP_HOSTED_TRANSPORT_FRAME_SIZE / EH_SPI_FD_WORD_SIZE)
#define EH_SPI_FD_BACKOFF_MS       100
#define EH_SPI_FD_MAX_BURST        32

struct eh_spi_fd_context
{
    struct eh_spi_bus bus;
    rt_bool_t handshake_irq_attached;
    rt_bool_t data_ready_irq_attached;
    rt_bool_t dummy_needed;
    rt_bool_t boot_sync_pending;
    rt_bool_t boot_saw_handshake_inactive;
    uint8_t stuck_rx_log_count;
    uint8_t tx_frame[ESP_HOSTED_TRANSPORT_FRAME_SIZE] __attribute__((aligned(64)));
    uint8_t rx_frame[ESP_HOSTED_TRANSPORT_FRAME_SIZE] __attribute__((aligned(64)));
    uint32_t tx_words[EH_SPI_FD_FRAME_WORDS] __attribute__((aligned(64)));
    uint32_t rx_words[EH_SPI_FD_FRAME_WORDS] __attribute__((aligned(64)));
};

static struct eh_spi_fd_context g_spi_fd;

static rt_bool_t eh_spi_fd_handshake_active(void)
{
#ifdef ESP_HOSTED_HANDSHAKE_ACTIVE_LOW
    return eh_spi_gpio_active(ESP_HOSTED_HANDSHAKE_PIN, RT_TRUE);
#else
    return eh_spi_gpio_active(ESP_HOSTED_HANDSHAKE_PIN, RT_FALSE);
#endif
}

static rt_bool_t eh_spi_fd_data_ready(void)
{
#ifdef ESP_HOSTED_DATA_READY_ACTIVE_LOW
    return eh_spi_gpio_active(ESP_HOSTED_DATA_READY_PIN, RT_TRUE);
#else
    return eh_spi_gpio_active(ESP_HOSTED_DATA_READY_PIN, RT_FALSE);
#endif
}

static void eh_spi_fd_irq(void *argument)
{
    eh_transport_wake(argument, EH_TRANSPORT_EVENT_RX);
}

static rt_size_t eh_spi_fd_transfer(struct eh_spi_fd_context *context,
                                    const struct eh_transport_tx_item *item)
{
    struct rt_qspi_message message;
    rt_size_t index;
    rt_size_t result;

    rt_memset(context->tx_frame, 0, sizeof(context->tx_frame));
    if (item)
    {
        rt_memcpy(context->tx_frame, item->frame, item->length);
    }
    else
    {
        context->tx_frame[0] = EH_TRANSPORT_IF_MAX;
    }

    for (index = 0; index < EH_SPI_FD_FRAME_WORDS; index++)
    {
        const uint8_t *source = context->tx_frame + index * EH_SPI_FD_WORD_SIZE;

        context->tx_words[index] = ((uint32_t)source[0] << 24) |
                                   ((uint32_t)source[1] << 16) |
                                   ((uint32_t)source[2] << 8) |
                                   source[3];
    }

    rt_memset(&message, 0, sizeof(message));
    rt_memset(context->rx_words, 0, sizeof(context->rx_words));
    message.parent.send_buf = context->tx_words;
    message.parent.recv_buf = context->rx_words;
    message.parent.length = ESP_HOSTED_TRANSPORT_FRAME_SIZE;
    message.qspi_data_lines = 1;
    result = eh_spi_transfer(&context->bus, &message);
    if (result != ESP_HOSTED_TRANSPORT_FRAME_SIZE)
    {
        return result;
    }

    for (index = 0; index < EH_SPI_FD_FRAME_WORDS; index++)
    {
        uint32_t word = context->rx_words[index];
        uint8_t *destination = context->rx_frame + index * EH_SPI_FD_WORD_SIZE;

        destination[0] = word >> 24;
        destination[1] = word >> 16;
        destination[2] = word >> 8;
        destination[3] = word;
    }
    return result;
}

static rt_bool_t eh_spi_fd_rx_filled(struct eh_spi_fd_context *context,
                                     uint32_t value)
{
    rt_size_t index;

    for (index = 0; index < EH_SPI_FD_FRAME_WORDS; index++)
    {
        if (context->rx_words[index] != value)
        {
            return RT_FALSE;
        }
    }
    return RT_TRUE;
}

static rt_err_t eh_spi_fd_init(struct eh_transport *transport)
{
    const struct eh_spi_pin pins[] = {
        { "clock", ESP_HOSTED_SPI_CLK_PIN },
        { "D0/MOSI", ESP_HOSTED_SPI_D0_PIN },
        { "D1/MISO", ESP_HOSTED_SPI_D1_PIN },
        { "chip-select", ESP_HOSTED_SPI_CS_PIN },
        { "handshake", ESP_HOSTED_HANDSHAKE_PIN },
        { "data-ready", ESP_HOSTED_DATA_READY_PIN },
        { "reset", ESP_HOSTED_RESET_PIN },
    };
    rt_err_t result;

    rt_memset(&g_spi_fd, 0, sizeof(g_spi_fd));
    transport->backend = &g_spi_fd;
    if (ESP_HOSTED_HANDSHAKE_PIN < 0 || ESP_HOSTED_DATA_READY_PIN < 0)
    {
        LOG_E("full-duplex SPI requires handshake and data-ready GPIOs");
        return -RT_EINVAL;
    }
    result = eh_spi_validate_pins(pins, sizeof(pins) / sizeof(pins[0]));
    if (result != RT_EOK)
    {
        return result;
    }
    result = eh_spi_init(&g_spi_fd.bus, EH_SPI_FD_DATA_WIDTH, 1, RT_TRUE);
    if (result != RT_EOK)
    {
        return result;
    }
#ifdef ESP_HOSTED_HANDSHAKE_ACTIVE_LOW
    result = eh_spi_configure_input_irq(ESP_HOSTED_HANDSHAKE_PIN, RT_TRUE,
                                        eh_spi_fd_irq, transport);
#else
    result = eh_spi_configure_input_irq(ESP_HOSTED_HANDSHAKE_PIN, RT_FALSE,
                                        eh_spi_fd_irq, transport);
#endif
    if (result != RT_EOK)
    {
        LOG_E("cannot configure handshake GPIO: %d", result);
        return result;
    }
    g_spi_fd.handshake_irq_attached = RT_TRUE;
#ifdef ESP_HOSTED_DATA_READY_ACTIVE_LOW
    result = eh_spi_configure_input_irq(ESP_HOSTED_DATA_READY_PIN, RT_TRUE,
                                        eh_spi_fd_irq, transport);
#else
    result = eh_spi_configure_input_irq(ESP_HOSTED_DATA_READY_PIN, RT_FALSE,
                                        eh_spi_fd_irq, transport);
#endif
    if (result != RT_EOK)
    {
        LOG_E("cannot configure data-ready GPIO: %d", result);
        return result;
    }
    g_spi_fd.data_ready_irq_attached = RT_TRUE;
    return RT_EOK;
}

static rt_err_t eh_spi_fd_start(struct eh_transport *transport)
{
    struct eh_spi_fd_context *context = transport->backend;

    context->boot_sync_pending = ESP_HOSTED_RESET_PIN >= 0 &&
                                 ESP_HOSTED_HANDSHAKE_PIN >= 0;
    context->boot_saw_handshake_inactive = RT_FALSE;

    LOG_I("SPI: bus=%s mode=%d freq=%d Hz full-duplex",
          eh_spi_bus_name(), ESP_HOSTED_SPI_MODE, ESP_HOSTED_SPI_MAX_HZ);
    LOG_I("GPIOs: CLK:%d MOSI:%d MISO:%d CS:%d HS:%d DR:%d RESET:%d",
          ESP_HOSTED_SPI_CLK_PIN, ESP_HOSTED_SPI_D0_PIN,
          ESP_HOSTED_SPI_D1_PIN, ESP_HOSTED_SPI_CS_PIN,
          ESP_HOSTED_HANDSHAKE_PIN, ESP_HOSTED_DATA_READY_PIN,
          ESP_HOSTED_RESET_PIN);
    eh_spi_reset_coprocessor(&context->bus);
    if (ESP_HOSTED_RESET_PIN < 0)
    {
        LOG_W("reset GPIO disabled; host and coprocessor must be reset together");
    }
    LOG_I("waiting for transport handshake");
    return RT_EOK;
}

static void eh_spi_fd_run(struct eh_transport *transport)
{
    struct eh_spi_fd_context *context = transport->backend;
    struct eh_transport_tx_item pending_item;
    rt_bool_t have_pending_item = RT_FALSE;
    rt_bool_t waiting_for_handshake = RT_FALSE;

    rt_memset(&pending_item, 0, sizeof(pending_item));

    while (1)
    {
        int transactions;

        eh_transport_wait(transport, EH_TRANSPORT_EVENT_ALL,
                          rt_tick_from_millisecond(10));
        for (transactions = 0; transactions < EH_SPI_FD_MAX_BURST; transactions++)
        {
            struct eh_transport_tx_item *item_pointer;
            rt_bool_t data_ready;
            rt_bool_t malformed = RT_FALSE;
            rt_bool_t rx_processed = RT_FALSE;
            rt_bool_t stuck_rx;

            if (context->boot_sync_pending)
            {
                if (!eh_spi_fd_handshake_active())
                {
                    context->boot_saw_handshake_inactive = RT_TRUE;
                    break;
                }
                if (!context->boot_saw_handshake_inactive)
                {
                    break;
                }
                context->boot_sync_pending = RT_FALSE;
                {
                    rt_int32_t elapsed_ms =
                        eh_spi_reset_elapsed_ms(&context->bus);

                    LOG_I("coprocessor boot ready after %d ms: HS=%d DR=%d",
                          elapsed_ms,
                          eh_spi_gpio_value(ESP_HOSTED_HANDSHAKE_PIN),
                          eh_spi_gpio_value(ESP_HOSTED_DATA_READY_PIN));
                }
            }

            data_ready = ESP_HOSTED_DATA_READY_PIN < 0
                             ? RT_TRUE : eh_spi_fd_data_ready();
            if (!have_pending_item)
            {
                have_pending_item = eh_transport_next_tx(transport,
                                                          &pending_item);
            }
            if (!have_pending_item && !data_ready && !context->dummy_needed)
            {
                break;
            }
            if (!eh_spi_fd_handshake_active())
            {
                if (!waiting_for_handshake)
                {
                    LOG_D("waiting for handshake: HS=%d DR=%d",
                          eh_spi_gpio_value(ESP_HOSTED_HANDSHAKE_PIN),
                          eh_spi_gpio_value(ESP_HOSTED_DATA_READY_PIN));
                    waiting_for_handshake = RT_TRUE;
                }
                break;
            }
            if (waiting_for_handshake)
            {
                waiting_for_handshake = RT_FALSE;
            }

            item_pointer = have_pending_item ? &pending_item : RT_NULL;
            context->dummy_needed = RT_FALSE;
            if (eh_spi_fd_transfer(context, item_pointer) !=
                ESP_HOSTED_TRANSPORT_FRAME_SIZE)
            {
                LOG_E("SPI full-duplex transfer failed");
                if (have_pending_item)
                {
                    eh_transport_complete_tx(&pending_item, -RT_EIO);
                    have_pending_item = RT_FALSE;
                }
                break;
            }

            stuck_rx = eh_spi_fd_rx_filled(context, 0) ||
                       eh_spi_fd_rx_filled(context, UINT32_MAX);
            if (stuck_rx)
            {
                if (context->stuck_rx_log_count < EH_TRANSPORT_INVALID_LOG_LIMIT)
                {
                    LOG_W("RX stuck at 0x%02x; check %s D1/MISO wiring and coprocessor power",
                          context->rx_words[0] ? 0xff : 0x00,
                          eh_spi_bus_name());
                    context->stuck_rx_log_count++;
                }
                malformed = RT_TRUE;
            }
            else
            {
                rx_processed = eh_transport_deliver(transport, context->rx_frame,
                                                     sizeof(context->rx_frame),
                                                     &malformed);
            }
            if (have_pending_item)
            {
                eh_transport_complete_tx(&pending_item, RT_EOK);
                have_pending_item = RT_FALSE;
            }
            if (item_pointer || rx_processed)
            {
                context->dummy_needed = RT_TRUE;
            }
            if (malformed)
            {
                rt_thread_mdelay(EH_SPI_FD_BACKOFF_MS);
                break;
            }
            if (ESP_HOSTED_DATA_READY_PIN < 0)
            {
                break;
            }
        }
    }
}

static void eh_spi_fd_deinit(struct eh_transport *transport)
{
    struct eh_spi_fd_context *context = transport->backend;

    if (!context)
    {
        return;
    }
    if (context->data_ready_irq_attached)
    {
        eh_spi_deconfigure_input_irq(ESP_HOSTED_DATA_READY_PIN);
    }
    if (context->handshake_irq_attached)
    {
        eh_spi_deconfigure_input_irq(ESP_HOSTED_HANDSHAKE_PIN);
    }
    eh_spi_deinit(&context->bus);
    rt_memset(context, 0, sizeof(*context));
    transport->backend = RT_NULL;
}

static rt_err_t eh_spi_fd_set_capabilities(struct eh_transport *transport,
                                           uint8_t capabilities,
                                           uint32_t ext_capabilities)
{
    (void)transport;
    (void)ext_capabilities;
    if (!(capabilities & (1U << 5)))
    {
        LOG_E("coprocessor did not advertise full-duplex SPI WLAN");
        return -RT_ENOSYS;
    }
    return RT_EOK;
}

const struct eh_transport_ops g_esp_hosted_spi_fd_ops = {
    .name = "SPI full-duplex",
    .frame_size = ESP_HOSTED_TRANSPORT_FRAME_SIZE,
    .tx_alignment = 1,
    .data_queue_send_wait_ms = 0,
    .init = eh_spi_fd_init,
    .deinit = eh_spi_fd_deinit,
    .start = eh_spi_fd_start,
    .run = eh_spi_fd_run,
    .set_slave_capabilities = eh_spi_fd_set_capabilities,
};

#endif /* ESP_HOSTED_TRANSPORT_SPI_FD */
