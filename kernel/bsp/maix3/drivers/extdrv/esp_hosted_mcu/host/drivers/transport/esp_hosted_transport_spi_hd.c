/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_transport_spi.h"
#include "esp_hosted_mcu_log.h"
#include <tick.h>

#if defined(ESP_HOSTED_SPI_HD_STATS) && defined(RT_USING_FINSH) && \
    defined(FINSH_USING_MSH)
#include <finsh.h>
#endif

#ifdef ESP_HOSTED_TRANSPORT_SPI_HD

#define DBG_TAG "esp_hosted.spi.hd"
#define DBG_LVL ESP_HOSTED_MCU_DBG_LVL
#include <rtdbg.h>

#define EH_SPI_HD_CMD_WRBUF       0x01
#define EH_SPI_HD_CMD_RDBUF       0x02
#define EH_SPI_HD_CMD_WRDMA       0x03
#define EH_SPI_HD_CMD_RDDMA       0x04
#define EH_SPI_HD_CMD_WR_DONE     0x07
#define EH_SPI_HD_CMD_RD_DONE     0x08
#define EH_SPI_HD_CMD_INT_ACK     0x09
#define EH_SPI_HD_DUAL_MASK       0x50
#define EH_SPI_HD_QUAD_MASK       0xa0
#define EH_SPI_HD_REG_SLAVE_READY 0x00
#define EH_SPI_HD_REG_MAX_TX_LEN  0x04
#define EH_SPI_HD_REG_MAX_RX_LEN  0x08
#define EH_SPI_HD_REG_TX_LEN      0x0c
#define EH_SPI_HD_REG_RX_COUNT    0x10
#define EH_SPI_HD_REG_SLAVE_CTRL  0x14
#define EH_SPI_HD_SLAVE_READY     0xee
#define EH_SPI_HD_DATAPATH_ON     (1U << 0)
#define EH_SPI_HD_TX_LEN_MASK     0x00ffffffU
#define EH_SPI_HD_INT_MASK        (3U << 24)
#define EH_SPI_HD_START_THROTTLE  (1U << 24)
#define EH_SPI_HD_STOP_THROTTLE   (1U << 25)
#define EH_SPI_HD_REGISTER_POLLS  3
#define EH_SPI_HD_MAX_BURST       32
#define EH_SPI_HD_CREDIT_POLL_MS   1
#define EH_SPI_HD_DATA_QUEUE_SEND_WAIT_MS 50
#define EH_SPI_HD_READY_POLL_MS   100
#define EH_SPI_HD_START_WATCHDOG_MS 100
#define EH_SPI_HD_IRQ_WATCHDOG_MS 1000
#define EH_SPI_HD_HEALTH_FAILURES 2

#ifndef ESP_HOSTED_SPI_D2_PIN
#define ESP_HOSTED_SPI_D2_PIN (-1)
#endif
#ifndef ESP_HOSTED_SPI_D3_PIN
#define ESP_HOSTED_SPI_D3_PIN (-1)
#endif
#ifndef ESP_HOSTED_SPI_HD_POLL_INTERVAL_MS
#define ESP_HOSTED_SPI_HD_POLL_INTERVAL_MS 10
#endif
#ifndef ESP_HOSTED_SPI_HD_RESET_SETTLE_MS
#define ESP_HOSTED_SPI_HD_RESET_SETTLE_MS 1000
#endif
#ifndef ESP_HOSTED_SPI_HD_DMA_ALIGNMENT
#define ESP_HOSTED_SPI_HD_DMA_ALIGNMENT 4
#endif
#if ESP_HOSTED_SPI_HD_DMA_ALIGNMENT < 4 || \
    (ESP_HOSTED_SPI_HD_DMA_ALIGNMENT & \
     (ESP_HOSTED_SPI_HD_DMA_ALIGNMENT - 1)) != 0
#error "ESP_HOSTED_SPI_HD_DMA_ALIGNMENT must be a power of two of at least 4"
#endif

#if defined(ESP_HOSTED_SPI_HD_WIDTH_4)
#define EH_SPI_HD_MAX_DATA_LINES 4
#else
#define EH_SPI_HD_MAX_DATA_LINES 2
#endif

#ifdef ESP_HOSTED_SPI_HD_STATS
struct eh_spi_hd_stats
{
    uint64_t started_us;
    uint64_t spi_xfer_us;
    uint64_t tx_dma_us;
    uint64_t rx_dma_us;
    uint64_t tx_dma_max_us;
    uint64_t rx_dma_max_us;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t credit_stall_us;
    uint64_t credit_stall_max_us;
    uint64_t rx_error_backoff_us;
    uint32_t spi_xfers;
    uint32_t spi_errors;
    uint32_t tx_dma_xfers;
    uint32_t rx_dma_xfers;
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t tx_errors;
    uint32_t rx_errors;
    uint32_t credit_checks;
    uint32_t credit_empty;
    uint32_t credit_read_busy;
    uint32_t credit_stalls;
    uint32_t credit_stall_polls;
    uint32_t rx_error_backoffs;
};

#define EH_SPI_HD_STAT_INC(context, member) ((context)->stats.member++)
#define EH_SPI_HD_STAT_ADD(context, member, value) \
    ((context)->stats.member += (value))
#else
#define EH_SPI_HD_STAT_INC(context, member) do { } while (0)
#define EH_SPI_HD_STAT_ADD(context, member, value) do { } while (0)
#endif

struct eh_spi_hd_context
{
    struct eh_spi_bus bus;
    rt_bool_t data_ready_irq_attached;
    uint8_t configured_width;
    uint8_t active_width;
    rt_bool_t bus_ready;
    rt_bool_t slave_ready_seen;
    rt_bool_t session_seen;
    rt_tick_t last_health_check;
    uint32_t tx_buffer_count;
    uint32_t tx_buffers_available;
    uint32_t rx_byte_count;
    uint32_t open_failures;
    uint32_t health_failures;
    uint32_t max_tx_length;
    uint32_t max_rx_length;
#ifdef ESP_HOSTED_SPI_HD_STATS
    struct eh_transport *transport;
    uint64_t credit_stall_started_us;
    struct eh_spi_hd_stats stats;
#endif
    uint8_t tx_frame[ESP_HOSTED_TRANSPORT_FRAME_SIZE] __attribute__((aligned(64)));
    uint8_t rx_frame[ESP_HOSTED_TRANSPORT_FRAME_SIZE] __attribute__((aligned(64)));
};

static struct eh_spi_hd_context g_spi_hd;

static rt_int32_t eh_spi_hd_ticks_until(rt_tick_t deadline)
{
    rt_int32_t remaining = (rt_int32_t)(deadline - rt_tick_get());

    return remaining > 0 ? remaining : 0;
}

static void eh_spi_hd_credit_stall_start(struct eh_spi_hd_context *context,
                                         rt_bool_t *credit_stalled)
{
#ifndef ESP_HOSTED_SPI_HD_STATS
    (void)context;
#endif
    if (*credit_stalled)
    {
        EH_SPI_HD_STAT_INC(context, credit_stall_polls);
        return;
    }
    *credit_stalled = RT_TRUE;
    EH_SPI_HD_STAT_INC(context, credit_stalls);
#ifdef ESP_HOSTED_SPI_HD_STATS
    context->credit_stall_started_us = cpu_ticks_us();
#endif
}

static void eh_spi_hd_credit_stall_end(struct eh_spi_hd_context *context,
                                       rt_bool_t *credit_stalled)
{
#ifndef ESP_HOSTED_SPI_HD_STATS
    (void)context;
#endif
    if (!*credit_stalled)
    {
        return;
    }
    *credit_stalled = RT_FALSE;
#ifdef ESP_HOSTED_SPI_HD_STATS
    if (context->credit_stall_started_us)
    {
        uint64_t elapsed_us = cpu_ticks_us() -
                              context->credit_stall_started_us;

        context->stats.credit_stall_us += elapsed_us;
        if (elapsed_us > context->stats.credit_stall_max_us)
        {
            context->stats.credit_stall_max_us = elapsed_us;
        }
        context->credit_stall_started_us = 0;
    }
#endif
}

static void eh_spi_hd_rx_error_backoff(struct eh_spi_hd_context *context)
{
#ifdef ESP_HOSTED_SPI_HD_STATS
    uint64_t started_us = cpu_ticks_us();
#else
    (void)context;
#endif

    rt_thread_mdelay(ESP_HOSTED_SPI_HD_POLL_INTERVAL_MS);
#ifdef ESP_HOSTED_SPI_HD_STATS
    context->stats.rx_error_backoffs++;
    context->stats.rx_error_backoff_us += cpu_ticks_us() - started_us;
#endif
}

static void eh_spi_hd_reset_data_path(struct eh_spi_hd_context *context)
{
    context->active_width = context->configured_width == 4
                                ? 2 : context->configured_width;
    context->bus_ready = RT_FALSE;
    context->slave_ready_seen = RT_FALSE;
    context->session_seen = RT_FALSE;
    context->last_health_check = rt_tick_get();
    context->tx_buffer_count = 0;
    context->tx_buffers_available = 0;
    context->rx_byte_count = 0;
    context->open_failures = 0;
    context->health_failures = 0;
    context->max_tx_length = 0;
    context->max_rx_length = 0;
}

static uint32_t eh_spi_hd_get_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void eh_spi_hd_put_le32(uint8_t *data, uint32_t value)
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static uint8_t eh_spi_hd_command(uint8_t opcode, uint8_t width)
{
    if (width == 4)
    {
        return opcode | EH_SPI_HD_QUAD_MASK;
    }
    if (width == 2)
    {
        return opcode | EH_SPI_HD_DUAL_MASK;
    }
    return opcode;
}

static rt_err_t eh_spi_hd_hw_transfer(struct eh_spi_hd_context *context,
                                      uint8_t opcode, uint8_t address,
                                      const uint8_t *tx, uint8_t *rx,
                                      size_t length)
{
    struct rt_qspi_message message;
#ifdef ESP_HOSTED_SPI_HD_STATS
    uint64_t started_us;
    uint64_t elapsed_us;
#endif
    rt_size_t result;

    rt_memset(&message, 0, sizeof(message));
    message.parent.send_buf = tx;
    message.parent.recv_buf = rx;
    message.parent.length = length;
    message.instruction.content = eh_spi_hd_command(opcode,
                                                    context->active_width);
    message.instruction.size = 8;
    message.instruction.qspi_lines = 1;
    /* Espressif's SPI-HD device configuration applies eight-bit address and
     * dummy phases to every command, including DMA completion commands. */
    message.address.content = address;
    message.address.size = 8;
    message.address.qspi_lines = context->active_width;
    message.dummy_cycles = 8;
    message.qspi_data_lines = context->active_width;
    rt_set_errno(RT_EOK);
#ifdef ESP_HOSTED_SPI_HD_STATS
    started_us = cpu_ticks_us();
#endif
    result = eh_spi_transfer(&context->bus, &message);
#ifdef ESP_HOSTED_SPI_HD_STATS
    elapsed_us = cpu_ticks_us() - started_us;
    context->stats.spi_xfers++;
    context->stats.spi_xfer_us += elapsed_us;
    if (opcode == EH_SPI_HD_CMD_WRDMA)
    {
        context->stats.tx_dma_xfers++;
        context->stats.tx_dma_us += elapsed_us;
        if (elapsed_us > context->stats.tx_dma_max_us)
        {
            context->stats.tx_dma_max_us = elapsed_us;
        }
    }
    else if (opcode == EH_SPI_HD_CMD_RDDMA)
    {
        context->stats.rx_dma_xfers++;
        context->stats.rx_dma_us += elapsed_us;
        if (elapsed_us > context->stats.rx_dma_max_us)
        {
            context->stats.rx_dma_max_us = elapsed_us;
        }
    }
#endif
    if (length)
    {
        if (result == length)
        {
            return RT_EOK;
        }
        EH_SPI_HD_STAT_INC(context, spi_errors);
        return -RT_EIO;
    }
    if (rt_get_errno() == RT_EOK)
    {
        return RT_EOK;
    }
    EH_SPI_HD_STAT_INC(context, spi_errors);
    return -RT_EIO;
}

static rt_err_t eh_spi_hd_transfer(struct eh_spi_hd_context *context,
                                   uint8_t opcode, uint8_t address,
                                   const uint8_t *tx, uint8_t *rx,
                                   size_t length)
{
    return eh_spi_hd_hw_transfer(context, opcode, address, tx, rx, length);
}

static rt_err_t eh_spi_hd_read_reg_once(struct eh_spi_hd_context *context,
                                        uint8_t reg, uint32_t *value)
{
    uint8_t data[4] __attribute__((aligned(4)));
    rt_err_t result;

    result = eh_spi_hd_transfer(context, EH_SPI_HD_CMD_RDBUF, reg,
                                RT_NULL, data, sizeof(data));
    if (result == RT_EOK)
    {
        *value = eh_spi_hd_get_le32(data);
    }
    return result;
}

static rt_err_t eh_spi_hd_read_reg(struct eh_spi_hd_context *context,
                                   uint8_t reg, uint32_t *value,
                                   rt_bool_t stable)
{
    uint32_t current;
    uint32_t previous;
    int attempt;
    rt_err_t result;

    result = eh_spi_hd_read_reg_once(context, reg, &previous);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!stable)
    {
        *value = previous;
        return RT_EOK;
    }
    for (attempt = 0; attempt < EH_SPI_HD_REGISTER_POLLS; attempt++)
    {
        result = eh_spi_hd_read_reg_once(context, reg, &current);
        if (result != RT_EOK)
        {
            return result;
        }
        if (current == previous)
        {
            *value = current;
            return RT_EOK;
        }
        previous = current;
    }
    /* Counters such as TX_LEN and RX_COUNT may advance between every read
     * while traffic is active. That is transient, not a bus failure. */
    return -RT_EBUSY;
}

static rt_err_t eh_spi_hd_write_reg(struct eh_spi_hd_context *context,
                                    uint8_t reg, uint32_t value)
{
    uint8_t data[4] __attribute__((aligned(4)));

    eh_spi_hd_put_le32(data, value);
    return eh_spi_hd_transfer(context, EH_SPI_HD_CMD_WRBUF, reg,
                              data, RT_NULL, sizeof(data));
}

static rt_err_t eh_spi_hd_command_only(struct eh_spi_hd_context *context,
                                       uint8_t opcode)
{
    return eh_spi_hd_transfer(context, opcode, 0, RT_NULL, RT_NULL, 0);
}

static rt_err_t eh_spi_hd_read_dma(struct eh_spi_hd_context *context,
                                   uint8_t *data, size_t length)
{
    rt_err_t result;

    result = eh_spi_hd_transfer(context, EH_SPI_HD_CMD_RDDMA, 0,
                                RT_NULL, data, length);
    if (result == RT_EOK)
    {
        result = eh_spi_hd_command_only(context, EH_SPI_HD_CMD_RD_DONE);
    }
    return result;
}

static rt_err_t eh_spi_hd_write_dma(struct eh_spi_hd_context *context,
                                    const uint8_t *data, size_t length)
{
    size_t dma_length = RT_ALIGN(length, ESP_HOSTED_SPI_HD_DMA_ALIGNMENT);
    const uint8_t *dma_data = data;
    rt_err_t result;

    if (dma_length > context->max_rx_length ||
        dma_length > sizeof(context->tx_frame))
    {
        return -RT_EINVAL;
    }
    if (dma_length != length)
    {
        rt_memset(context->tx_frame, 0, dma_length);
        rt_memcpy(context->tx_frame, data, length);
        dma_data = context->tx_frame;
    }

    /* Match the upstream ESP host port: keep WRDMA atomic and pad its DMA
     * length while the transport header retains the actual frame length. */
    result = eh_spi_hd_transfer(context, EH_SPI_HD_CMD_WRDMA, 0,
                                dma_data, RT_NULL, dma_length);
    if (result == RT_EOK)
    {
        result = eh_spi_hd_command_only(context, EH_SPI_HD_CMD_WR_DONE);
    }
    return result;
}

static rt_bool_t eh_spi_hd_data_ready(void)
{
#ifdef ESP_HOSTED_DATA_READY_ACTIVE_LOW
    return eh_spi_gpio_active(ESP_HOSTED_DATA_READY_PIN, RT_TRUE);
#else
    return eh_spi_gpio_active(ESP_HOSTED_DATA_READY_PIN, RT_FALSE);
#endif
}

static void eh_spi_hd_irq(void *argument)
{
    eh_transport_wake(argument, EH_TRANSPORT_EVENT_RX);
}

static rt_err_t eh_spi_hd_init(struct eh_transport *transport)
{
    const struct eh_spi_pin pins[] = {
        { "clock", ESP_HOSTED_SPI_CLK_PIN },
        { "D0", ESP_HOSTED_SPI_D0_PIN },
        { "D1", EH_SPI_HD_MAX_DATA_LINES >= 2 ? ESP_HOSTED_SPI_D1_PIN : -1 },
        { "D2", EH_SPI_HD_MAX_DATA_LINES == 4 ? ESP_HOSTED_SPI_D2_PIN : -1 },
        { "D3", EH_SPI_HD_MAX_DATA_LINES == 4 ? ESP_HOSTED_SPI_D3_PIN : -1 },
        { "chip-select", ESP_HOSTED_SPI_CS_PIN },
        { "data-ready", ESP_HOSTED_DATA_READY_PIN },
        { "reset", ESP_HOSTED_RESET_PIN },
    };
    rt_err_t result;

    rt_memset(&g_spi_hd, 0, sizeof(g_spi_hd));
#ifdef ESP_HOSTED_SPI_HD_STATS
    g_spi_hd.stats.started_us = cpu_ticks_us();
#endif
    transport->backend = &g_spi_hd;
#ifdef ESP_HOSTED_SPI_HD_STATS
    g_spi_hd.transport = transport;
#endif
    g_spi_hd.configured_width = EH_SPI_HD_MAX_DATA_LINES;
    g_spi_hd.active_width = EH_SPI_HD_MAX_DATA_LINES == 4
                                ? 2 : EH_SPI_HD_MAX_DATA_LINES;

    result = eh_spi_validate_pins(pins, sizeof(pins) / sizeof(pins[0]));
    if (result != RT_EOK)
    {
        return result;
    }
    result = eh_spi_init(&g_spi_hd.bus, 8,
                         g_spi_hd.configured_width, RT_FALSE);
    if (result != RT_EOK)
    {
        return result;
    }
#ifdef ESP_HOSTED_DATA_READY_ACTIVE_LOW
    result = eh_spi_configure_input_irq(ESP_HOSTED_DATA_READY_PIN, RT_TRUE,
                                        eh_spi_hd_irq, transport);
#else
    result = eh_spi_configure_input_irq(ESP_HOSTED_DATA_READY_PIN, RT_FALSE,
                                        eh_spi_hd_irq, transport);
#endif
    if (result != RT_EOK)
    {
        LOG_E("cannot configure data-ready GPIO: %d", result);
    }
    else if (ESP_HOSTED_DATA_READY_PIN >= 0)
    {
        g_spi_hd.data_ready_irq_attached = RT_TRUE;
    }
    return result;
}

static rt_err_t eh_spi_hd_start(struct eh_transport *transport)
{
    struct eh_spi_hd_context *context = transport->backend;

    LOG_I("SPI: bus=%s mode=%d freq=%d Hz half-duplex, max-width=%u",
          eh_spi_bus_name(), ESP_HOSTED_SPI_MODE,
          ESP_HOSTED_SPI_MAX_HZ, context->configured_width);
    LOG_I("GPIOs: CLK:%d D0:%d D1:%d D2:%d D3:%d CS:%d DR:%d RESET:%d",
          ESP_HOSTED_SPI_CLK_PIN, ESP_HOSTED_SPI_D0_PIN,
          context->configured_width >= 2 ? ESP_HOSTED_SPI_D1_PIN : -1,
          context->configured_width == 4 ? ESP_HOSTED_SPI_D2_PIN : -1,
          context->configured_width == 4 ? ESP_HOSTED_SPI_D3_PIN : -1,
          ESP_HOSTED_SPI_CS_PIN, ESP_HOSTED_DATA_READY_PIN,
          ESP_HOSTED_RESET_PIN);
    eh_spi_reset_coprocessor(&context->bus);
    eh_spi_hd_reset_data_path(context);
    if (ESP_HOSTED_RESET_PIN < 0)
    {
        LOG_W("reset GPIO disabled; host and coprocessor must be reset together");
    }
    LOG_I("waiting for SPI-HD coprocessor ready register");
    return RT_EOK;
}

static rt_err_t eh_spi_hd_open_data_path(struct eh_spi_hd_context *context)
{
    uint32_t value;
    rt_err_t result;

    result = eh_spi_hd_read_reg(context, EH_SPI_HD_REG_SLAVE_READY,
                                &value, RT_TRUE);
    if (result != RT_EOK)
    {
        return -RT_EBUSY;
    }
    if (value != EH_SPI_HD_SLAVE_READY)
    {
        return -RT_EBUSY;
    }
    if (!context->slave_ready_seen)
    {
        rt_int32_t elapsed_ms = eh_spi_reset_elapsed_ms(&context->bus);

        context->slave_ready_seen = RT_TRUE;
        if (elapsed_ms >= 0)
        {
            LOG_I("SPI-HD register ready after %d ms: READY=%02x",
                  elapsed_ms, value);
        }
        else
        {
            LOG_I("SPI-HD register ready: READY=%02x", value);
        }
    }
    result = eh_spi_hd_read_reg(context, EH_SPI_HD_REG_MAX_TX_LEN,
                                &context->max_tx_length, RT_TRUE);
    if (result == RT_EOK)
    {
        result = eh_spi_hd_read_reg(context, EH_SPI_HD_REG_MAX_RX_LEN,
                                    &context->max_rx_length, RT_TRUE);
    }
    if (result != RT_EOK ||
        context->max_tx_length < ESP_HOSTED_TRANSPORT_HEADER_SIZE ||
        context->max_rx_length < ESP_HOSTED_TRANSPORT_HEADER_SIZE ||
        context->max_tx_length > ESP_HOSTED_TRANSPORT_FRAME_SIZE ||
        context->max_rx_length > ESP_HOSTED_TRANSPORT_FRAME_SIZE)
    {
        LOG_E("invalid SPI-HD buffer sizes: TX=%u RX=%u",
              context->max_tx_length, context->max_rx_length);
        return -RT_EIO;
    }
    result = eh_spi_hd_write_reg(context, EH_SPI_HD_REG_SLAVE_CTRL,
                                 EH_SPI_HD_DATAPATH_ON);
    if (result != RT_EOK)
    {
        return result;
    }
    result = eh_spi_hd_read_reg(context, EH_SPI_HD_REG_SLAVE_CTRL,
                                &value, RT_TRUE);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!(value & EH_SPI_HD_DATAPATH_ON))
    {
        context->open_failures++;
        if (context->open_failures == 1 ||
            !(context->open_failures % 50))
        {
            LOG_W("SPI-HD control write not accepted: CTRL=%08x", value);
        }
        return -RT_EBUSY;
    }

    context->bus_ready = RT_TRUE;
    context->last_health_check = rt_tick_get();
    context->open_failures = 0;
    context->tx_buffer_count = 0;
    context->tx_buffers_available = 0;
    context->rx_byte_count = 0;
    LOG_I("SPI-HD data path open: bootstrap-width=%u max-width=%u TX=%u RX=%u",
          context->active_width, context->configured_width,
          context->max_tx_length, context->max_rx_length);
    return RT_EOK;
}

static rt_bool_t eh_spi_hd_data_path_alive(struct eh_spi_hd_context *context)
{
    uint32_t ready;
    uint32_t control;

    if (eh_spi_hd_read_reg(context, EH_SPI_HD_REG_SLAVE_READY,
                           &ready, RT_TRUE) != RT_EOK ||
        eh_spi_hd_read_reg(context, EH_SPI_HD_REG_SLAVE_CTRL,
                           &control, RT_TRUE) != RT_EOK)
    {
        return RT_FALSE;
    }
    return ready == EH_SPI_HD_SLAVE_READY &&
           (control & EH_SPI_HD_DATAPATH_ON);
}

static rt_err_t eh_spi_hd_receive(struct eh_transport *transport,
                                  rt_bool_t *received)
{
    struct eh_spi_hd_context *context = transport->backend;
    uint32_t current;
    uint32_t interrupt_flags;
    uint32_t length;
    rt_bool_t malformed;
    rt_err_t result;

    *received = RT_FALSE;
    result = eh_spi_hd_read_reg(context, EH_SPI_HD_REG_TX_LEN,
                                &current, RT_TRUE);
    if (result != RT_EOK)
    {
        return result;
    }
    result = eh_spi_hd_command_only(context, EH_SPI_HD_CMD_INT_ACK);
    if (result != RT_EOK)
    {
        return result;
    }

    interrupt_flags = current & EH_SPI_HD_INT_MASK;
    if (interrupt_flags & EH_SPI_HD_START_THROTTLE)
    {
        eh_transport_set_tx_throttled(transport, RT_TRUE);
    }
    if (interrupt_flags & EH_SPI_HD_STOP_THROTTLE)
    {
        eh_transport_set_tx_throttled(transport, RT_FALSE);
    }
    current &= EH_SPI_HD_TX_LEN_MASK;
    length = (current - context->rx_byte_count) & EH_SPI_HD_TX_LEN_MASK;
    if (!length)
    {
        return RT_EOK;
    }
    if (length > context->max_tx_length ||
        length > sizeof(context->rx_frame))
    {
        LOG_E("invalid SPI-HD receive length %u (current=%06x consumed=%06x)",
              length, current, context->rx_byte_count);
        return -RT_EIO;
    }

    rt_memset(context->rx_frame, 0, sizeof(context->rx_frame));
    result = eh_spi_hd_read_dma(context, context->rx_frame, length);
    if (result != RT_EOK)
    {
        return result;
    }
    context->rx_byte_count = (context->rx_byte_count + length) &
                             EH_SPI_HD_TX_LEN_MASK;
    EH_SPI_HD_STAT_INC(context, rx_frames);
    EH_SPI_HD_STAT_ADD(context, rx_bytes, length);
    *received = RT_TRUE;
    eh_transport_deliver(transport, context->rx_frame, length, &malformed);
    return malformed ? -RT_EIO : RT_EOK;
}

static rt_err_t eh_spi_hd_transmit(struct eh_transport *transport,
                                   struct eh_transport_tx_item *item)
{
    struct eh_spi_hd_context *context = transport->backend;
    uint32_t slave_count;
    uint32_t available;
    rt_err_t result;

    if (!context->tx_buffers_available)
    {
        EH_SPI_HD_STAT_INC(context, credit_checks);
        result = eh_spi_hd_read_reg(context, EH_SPI_HD_REG_RX_COUNT,
                                    &slave_count, RT_TRUE);
        if (result != RT_EOK)
        {
            if (result == -RT_EBUSY)
            {
                EH_SPI_HD_STAT_INC(context, credit_read_busy);
            }
            return result;
        }
        available = slave_count - context->tx_buffer_count;
        if (!available)
        {
            EH_SPI_HD_STAT_INC(context, credit_empty);
            return -RT_EBUSY;
        }
        context->tx_buffers_available = available;
    }
    if (item->length > context->max_rx_length)
    {
        return -RT_EINVAL;
    }
    result = eh_spi_hd_write_dma(context, item->frame, item->length);
    if (result == RT_EOK)
    {
        context->tx_buffer_count++;
        context->tx_buffers_available--;
        EH_SPI_HD_STAT_INC(context, tx_frames);
        EH_SPI_HD_STAT_ADD(context, tx_bytes, item->length);
    }
    return result;
}

static void eh_spi_hd_run(struct eh_transport *transport)
{
    struct eh_spi_hd_context *context = transport->backend;
    struct eh_transport_tx_item pending_item;
    struct eh_transport_tx_item pending_control_item;
    rt_bool_t have_pending_item = RT_FALSE;
    rt_bool_t have_pending_control_item = RT_FALSE;
    rt_bool_t credit_stalled = RT_FALSE;
    rt_tick_t credit_poll_deadline = 0;
    rt_int32_t elapsed_ms = eh_spi_reset_elapsed_ms(&context->bus);

    rt_memset(&pending_item, 0, sizeof(pending_item));
    rt_memset(&pending_control_item, 0, sizeof(pending_control_item));

    if (elapsed_ms >= 0 && elapsed_ms < ESP_HOSTED_SPI_HD_RESET_SETTLE_MS)
    {
        rt_int32_t remaining_ms = ESP_HOSTED_SPI_HD_RESET_SETTLE_MS - elapsed_ms;

        LOG_I("waiting %d ms for ESP reset to settle", remaining_ms);
        rt_thread_mdelay(remaining_ms);
    }

    while (1)
    {
        int transactions;
        rt_int32_t timeout;
        rt_uint32_t wait_events = EH_TRANSPORT_EVENT_ALL;
        rt_bool_t credit_poll_due = RT_TRUE;

        if (!context->bus_ready)
        {
            /* Keep probes rate-limited even when upper layers queue traffic
             * while the coprocessor is still booting. */
            rt_thread_mdelay(EH_SPI_HD_READY_POLL_MS);
            eh_spi_hd_open_data_path(context);
            continue;
        }

        if (!context->session_seen)
        {
            if ((rt_tick_t)(rt_tick_get() - context->last_health_check) >=
                rt_tick_from_millisecond(EH_SPI_HD_START_WATCHDOG_MS))
            {
                context->last_health_check = rt_tick_get();
                if (!eh_spi_hd_data_path_alive(context))
                {
                    context->health_failures++;
                    if (context->health_failures >= EH_SPI_HD_HEALTH_FAILURES)
                    {
                        LOG_W("SPI-HD data path lost; reopening");
                        eh_transport_set_tx_throttled(transport, RT_FALSE);
                        eh_spi_hd_credit_stall_end(context,
                                                   &credit_stalled);
                        transport->invalid_rx_log_count = 0;
                        eh_spi_hd_reset_data_path(context);
                    }
                    else
                    {
                        rt_thread_mdelay(EH_SPI_HD_START_WATCHDOG_MS);
                    }
                    continue;
                }
                context->health_failures = 0;
            }
        }

        if (credit_stalled)
        {
            /* RX_COUNT is level-polled. A full ESP receive queue does not
             * generate a data-ready edge when a credit returns. Ignore new
             * data queue wakeups until the next poll deadline, but still
             * service receive interrupts and priority traffic immediately. */
            timeout = eh_spi_hd_ticks_until(credit_poll_deadline);
            wait_events = EH_TRANSPORT_EVENT_CONTROL |
                          EH_TRANSPORT_EVENT_RX;
            credit_poll_due = timeout == 0;
        }
        else if (have_pending_item)
        {
            timeout = RT_WAITING_NO;
        }
        else if (ESP_HOSTED_DATA_READY_PIN < 0)
        {
            timeout = rt_tick_from_millisecond(ESP_HOSTED_SPI_HD_POLL_INTERVAL_MS);
        }
        else if (eh_spi_hd_data_ready())
        {
            timeout = RT_WAITING_NO;
        }
        else
        {
            /* Data Ready is level-signalled. The periodic wake also recovers
             * if its first edge arrived while the transport was opening. */
            timeout = rt_tick_from_millisecond(EH_SPI_HD_IRQ_WATCHDOG_MS);
        }
        if (!context->session_seen &&
            timeout > rt_tick_from_millisecond(EH_SPI_HD_START_WATCHDOG_MS))
        {
            timeout = rt_tick_from_millisecond(EH_SPI_HD_START_WATCHDOG_MS);
        }
        eh_transport_wait(transport, wait_events, timeout);
        if (credit_stalled)
        {
            credit_poll_due =
                eh_spi_hd_ticks_until(credit_poll_deadline) == 0;
        }
        for (transactions = 0; transactions < EH_SPI_HD_MAX_BURST; transactions++)
        {
            rt_bool_t data_ready = ESP_HOSTED_DATA_READY_PIN < 0 ||
                                   eh_spi_hd_data_ready();
            rt_bool_t progress = RT_FALSE;
            rt_err_t result;

            if (data_ready)
            {
                rt_bool_t received;

                result = eh_spi_hd_receive(transport, &received);
                if (result != RT_EOK && result != -RT_EBUSY)
                {
                    EH_SPI_HD_STAT_INC(context, rx_errors);
                    LOG_W("SPI-HD receive failed: %d", result);
                    eh_spi_hd_rx_error_backoff(context);
                    break;
                }
                progress = received;
            }

            /* Keep a blocked data frame local, but allow control and RPC
             * traffic to bypass it. Requeueing the data frame is racy when
             * producers have filled the data queue. */
            if (!have_pending_control_item && have_pending_item &&
                !pending_item.control)
            {
                have_pending_control_item = eh_transport_next_control(
                    transport, &pending_control_item);
            }
            if (!have_pending_item && !have_pending_control_item)
            {
                have_pending_item = eh_transport_next_tx(transport,
                                                          &pending_item);
                if (!have_pending_item)
                {
                    eh_spi_hd_credit_stall_end(context, &credit_stalled);
                }
            }
            if (have_pending_control_item || have_pending_item)
            {
                struct eh_transport_tx_item *item = have_pending_control_item
                                                        ? &pending_control_item
                                                        : &pending_item;
                rt_bool_t sending_control = item->control;

                if (credit_stalled && !credit_poll_due &&
                    eh_spi_hd_ticks_until(credit_poll_deadline) == 0)
                {
                    credit_poll_due = RT_TRUE;
                }
                if (!sending_control && credit_stalled && !credit_poll_due)
                {
                    if (!progress)
                    {
                        break;
                    }
                    continue;
                }
                result = eh_spi_hd_transmit(transport, item);
                if (result == -RT_EBUSY)
                {
                    eh_spi_hd_credit_stall_start(context, &credit_stalled);
                    credit_poll_deadline = rt_tick_get() +
                        rt_tick_from_millisecond(EH_SPI_HD_CREDIT_POLL_MS);
                    break;
                }
                eh_transport_complete_tx(item, result);
                if (have_pending_control_item)
                {
                    have_pending_control_item = RT_FALSE;
                }
                else
                {
                    have_pending_item = RT_FALSE;
                }
                if (!sending_control || !have_pending_item)
                {
                    eh_spi_hd_credit_stall_end(context, &credit_stalled);
                }
                else if (result == RT_EOK &&
                         context->tx_buffers_available)
                {
                    eh_spi_hd_credit_stall_end(context, &credit_stalled);
                }
                else if (credit_stalled)
                {
                    credit_poll_deadline = rt_tick_get() +
                        rt_tick_from_millisecond(EH_SPI_HD_CREDIT_POLL_MS);
                }
                if (result != RT_EOK)
                {
                    EH_SPI_HD_STAT_INC(context, tx_errors);
                    LOG_E("SPI-HD transmit failed: %d", result);
                    break;
                }
                progress = RT_TRUE;
            }
            if (!progress || ESP_HOSTED_DATA_READY_PIN < 0)
            {
                break;
            }
        }
    }
}

static void eh_spi_hd_deinit(struct eh_transport *transport)
{
    struct eh_spi_hd_context *context = transport->backend;

    if (!context)
    {
        return;
    }
    if (context->data_ready_irq_attached)
    {
        eh_spi_deconfigure_input_irq(ESP_HOSTED_DATA_READY_PIN);
    }
    eh_spi_deinit(&context->bus);
    rt_memset(context, 0, sizeof(*context));
    transport->backend = RT_NULL;
}

static rt_err_t eh_spi_hd_set_capabilities(struct eh_transport *transport,
                                           uint8_t capabilities,
                                           uint32_t ext_capabilities)
{
    struct eh_spi_hd_context *context = transport->backend;
    uint8_t selected_width;

    (void)capabilities;
    if (!(ext_capabilities & (1U << 4)))
    {
        LOG_E("coprocessor did not advertise WLAN over SPI-HD");
        return -RT_ENOSYS;
    }
    if (context->configured_width == 4 && (ext_capabilities & (1U << 1)))
    {
        selected_width = 4;
    }
    else if (ext_capabilities & ((1U << 0) | (1U << 1)))
    {
        selected_width = 2;
    }
    else
    {
        LOG_E("no common SPI-HD data width (host max %u, slave caps 0x%08x)",
              context->configured_width, ext_capabilities);
        return -RT_ENOSYS;
    }

    context->active_width = selected_width;
    context->session_seen = RT_TRUE;
    LOG_I("SPI-HD negotiated %u data lines, DMA alignment=%u",
          selected_width, ESP_HOSTED_SPI_HD_DMA_ALIGNMENT);
    return RT_EOK;
}

const struct eh_transport_ops g_esp_hosted_spi_hd_ops = {
    .name = "SPI half-duplex",
    .frame_size = ESP_HOSTED_TRANSPORT_FRAME_SIZE,
    .tx_alignment = ESP_HOSTED_SPI_HD_DMA_ALIGNMENT,
    .data_queue_send_wait_ms = EH_SPI_HD_DATA_QUEUE_SEND_WAIT_MS,
    .init = eh_spi_hd_init,
    .deinit = eh_spi_hd_deinit,
    .start = eh_spi_hd_start,
    .run = eh_spi_hd_run,
    .set_slave_capabilities = eh_spi_hd_set_capabilities,
};

#if defined(ESP_HOSTED_SPI_HD_STATS) && defined(RT_USING_FINSH) && \
    defined(FINSH_USING_MSH)
static uint64_t eh_spi_hd_rate_kbps(uint64_t bytes, uint64_t elapsed_us)
{
    return elapsed_us ? (bytes * 8000U) / elapsed_us : 0;
}

static void esp_spi_hd_stats(int argc, char **argv)
{
    struct eh_spi_hd_stats stats;
    uint32_t queue_waits = 0;
    uint32_t queue_timeouts = 0;
    uint16_t queue_high_watermark = 0;
    uint16_t queue_capacity = 0;
    uint64_t now_us = cpu_ticks_us();
    uint64_t elapsed_us;
    double utilization;

    if (argc > 1 && !rt_strcmp(argv[1], "reset"))
    {
        rt_enter_critical();
        rt_memset(&g_spi_hd.stats, 0, sizeof(g_spi_hd.stats));
        g_spi_hd.stats.started_us = now_us;
        if (g_spi_hd.credit_stall_started_us)
        {
            g_spi_hd.credit_stall_started_us = now_us;
            g_spi_hd.stats.credit_stalls = 1;
        }
        if (g_spi_hd.transport)
        {
            g_spi_hd.transport->data_queue_waits = 0;
            g_spi_hd.transport->data_queue_timeouts = 0;
            g_spi_hd.transport->data_queue_high_watermark =
                g_spi_hd.transport->data_queue.entry;
        }
        rt_exit_critical();
        rt_kprintf("SPI-HD statistics reset\n");
        return;
    }
    if (argc > 1)
    {
        rt_kprintf("Usage: esp_spi_hd_stats [reset]\n");
        return;
    }

    rt_enter_critical();
    stats = g_spi_hd.stats;
    if (g_spi_hd.transport)
    {
        queue_waits = g_spi_hd.transport->data_queue_waits;
        queue_timeouts = g_spi_hd.transport->data_queue_timeouts;
        queue_high_watermark =
            g_spi_hd.transport->data_queue_high_watermark;
        if (g_spi_hd.transport->data_queue.entry > queue_high_watermark)
        {
            queue_high_watermark = g_spi_hd.transport->data_queue.entry;
        }
        queue_capacity = g_spi_hd.transport->data_queue.max_msgs;
    }
    if (g_spi_hd.credit_stall_started_us)
    {
        uint64_t active_stall_us = now_us -
                                   g_spi_hd.credit_stall_started_us;

        stats.credit_stall_us += active_stall_us;
        if (active_stall_us > stats.credit_stall_max_us)
        {
            stats.credit_stall_max_us = active_stall_us;
        }
    }
    rt_exit_critical();
    if (!stats.started_us)
    {
        rt_kprintf("SPI-HD transport is not initialized\n");
        return;
    }

    elapsed_us = now_us - stats.started_us;
    utilization = elapsed_us
                    ? ((double)stats.spi_xfer_us * 100.0) / elapsed_us : 0.0;
    rt_kprintf("SPI-HD statistics over %llu ms\n",
               (unsigned long long)(elapsed_us / 1000U));
    rt_kprintf(" TX: frames=%u wire_bytes=%llu rate=%llu kbps errors=%u\n",
               stats.tx_frames, (unsigned long long)stats.tx_bytes,
               (unsigned long long)eh_spi_hd_rate_kbps(stats.tx_bytes,
                                                       elapsed_us),
               stats.tx_errors);
    rt_kprintf(" RX: frames=%u wire_bytes=%llu rate=%llu kbps errors=%u\n",
               stats.rx_frames, (unsigned long long)stats.rx_bytes,
               (unsigned long long)eh_spi_hd_rate_kbps(stats.rx_bytes,
                                                       elapsed_us),
               stats.rx_errors);
    rt_kprintf(" SPI: xfers=%u errors=%u wall=%llu us utilization=%.1f%%\n",
               stats.spi_xfers, stats.spi_errors,
               (unsigned long long)stats.spi_xfer_us, utilization);
    rt_kprintf(" DMA TX: xfers=%u average=%llu us max=%llu us\n",
               stats.tx_dma_xfers,
               (unsigned long long)(stats.tx_dma_xfers
                                        ? stats.tx_dma_us / stats.tx_dma_xfers : 0),
               (unsigned long long)stats.tx_dma_max_us);
    rt_kprintf(" DMA RX: xfers=%u average=%llu us max=%llu us\n",
               stats.rx_dma_xfers,
               (unsigned long long)(stats.rx_dma_xfers
                                        ? stats.rx_dma_us / stats.rx_dma_xfers : 0),
               (unsigned long long)stats.rx_dma_max_us);
    rt_kprintf(" Credits: checks=%u empty=%u unstable=%u\n",
               stats.credit_checks, stats.credit_empty,
               stats.credit_read_busy);
    rt_kprintf(" Credit stalls: episodes=%u polls=%u total=%llu us max=%llu us\n",
               stats.credit_stalls, stats.credit_stall_polls,
               (unsigned long long)stats.credit_stall_us,
               (unsigned long long)stats.credit_stall_max_us);
    rt_kprintf(" Data queue: high-water=%u/%u waits=%u timeouts=%u\n",
               queue_high_watermark, queue_capacity, queue_waits,
               queue_timeouts);
    rt_kprintf(" Delays: RX error backoffs=%u/%llu us\n",
               stats.rx_error_backoffs,
               (unsigned long long)stats.rx_error_backoff_us);
}
MSH_CMD_EXPORT(esp_spi_hd_stats, show or reset SPI-HD transfer statistics);
#endif

#endif /* ESP_HOSTED_TRANSPORT_SPI_HD */
