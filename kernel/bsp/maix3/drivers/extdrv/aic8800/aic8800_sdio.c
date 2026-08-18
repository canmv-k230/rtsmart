/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RT-Smart SDIO transport for the AIC8801 and AIC8800D80 WLAN chips.
 */
#include "aic8800_wifi.h"

#include "tick.h"

#define DBG_TAG "aic8800.sdio"
#define DBG_LVL AIC8800_DBG_LVL
#include <rtdbg.h>

#define AIC8800_SDIO_FUNCTION                 1U
#define AIC8800_SDIO_BLOCK_SIZE             512U
#define AIC8800_SDIO_FLOW_BUFFER_SIZE       1536U
#define AIC8800_SDIO_EVENT_RX             (1U << 0)
#define AIC8800_SDIO_EVENT_STOP           (1U << 1)
#define AIC8800_SDIO_MAX_RX_BURST            32U
#define AIC8800_SDIO_POLL_DELAY_US           200U
#define AIC8800_SDIO_WAKE_RETRIES              50U
#define AIC8800_SDIO_DATA_CREDIT_RESERVE         2U

#define AIC8800_SDIO_LEGACY_BYTE_LENGTH      0x02U
#define AIC8800_SDIO_LEGACY_INTERRUPT        0x04U
#define AIC8800_SDIO_LEGACY_SLEEP            0x05U
#define AIC8800_SDIO_LEGACY_WAKEUP           0x09U
#define AIC8800_SDIO_LEGACY_FLOW_CONTROL     0x0aU
#define AIC8800_SDIO_LEGACY_REGISTER_BLOCK   0x0bU
#define AIC8800_SDIO_LEGACY_BYTE_MODE        0x11U
#define AIC8800_SDIO_LEGACY_BLOCK_COUNT      0x12U
#define AIC8800_SDIO_LEGACY_READ_FIFO        0x08U
#define AIC8800_SDIO_LEGACY_WRITE_FIFO       0x07U

#define AIC8800_SDIO_V3_INTERRUPT            0x00U
#define AIC8800_SDIO_V3_INTERRUPT_PENDING    0x01U
#define AIC8800_SDIO_V3_WAKEUP               0x02U
#define AIC8800_SDIO_V3_FLOW_CONTROL         0x03U
#define AIC8800_SDIO_V3_INTERRUPT_STATUS     0x04U
#define AIC8800_SDIO_V3_BYTE_LENGTH          0x05U
#define AIC8800_SDIO_V3_BYTE_MODE            0x07U
#define AIC8800_SDIO_V3_READ_FIFO            0x0fU
#define AIC8800_SDIO_V3_WRITE_FIFO           0x10U
#define AIC8800_SDIO_OTHER_INTERRUPT         0x80U
#define AIC8800_SDIO_V3_BYTE_MODE_STATUS     120U

#ifndef AIC8800_WIFI_SDIO_RX_BUFFER_SIZE
#define AIC8800_WIFI_SDIO_RX_BUFFER_SIZE   65536U
#endif
#ifndef AIC8800_WIFI_SDIO_FLOW_RETRIES
#define AIC8800_WIFI_SDIO_FLOW_RETRIES        50U
#endif
static struct aic8800_context g_aic8800_sdio_context;
static struct rt_mutex g_aic8800_sdio_tx_mutex;
static struct rt_mutex g_aic8800_sdio_frame_mutex;
static struct rt_workqueue *g_aic8800_sdio_attach_workqueue;
static rt_bool_t g_aic8800_sdio_ipc_initialized;

static rt_bool_t aic8800_sdio_should_log_error(rt_uint32_t count);

static rt_uint16_t aic8800_sdio_get_le16(const rt_uint8_t *data)
{
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8);
}

static void aic8800_sdio_put_length(rt_uint8_t *data, rt_uint16_t length)
{
    data[0] = (rt_uint8_t)length;
    data[1] = (data[1] & 0xf0U) | (rt_uint8_t)((length >> 8) & 0x0fU);
}

static rt_uint8_t aic8800_sdio_crc8(const rt_uint8_t *data, rt_size_t length)
{
    rt_uint8_t crc = 0;

    while (length--)
    {
        rt_uint8_t mask;

        for (mask = 0x80U; mask; mask >>= 1)
        {
            if (crc & 0x80U)
            {
                crc = (rt_uint8_t)((crc << 1) ^ 0x07U);
            }
            else
            {
                crc <<= 1;
            }
            if (*data & mask)
            {
                crc ^= 0x07U;
            }
        }
        data++;
    }
    return crc;
}

static rt_err_t aic8800_sdio_readb(struct aic8800_context *context,
                                    rt_uint32_t address,
                                    rt_uint8_t *value)
{
    rt_int32_t result;

    if (!context || !context->sdio_function || !value)
    {
        return -RT_EINVAL;
    }
    mmcsd_host_lock(context->sdio_card->host);
    *value = sdio_io_readb(context->sdio_function, address, &result);
    mmcsd_host_unlock(context->sdio_card->host);
    return result;
}

static rt_err_t aic8800_sdio_writeb(struct aic8800_context *context,
                                     rt_uint32_t address,
                                     rt_uint8_t value)
{
    rt_err_t result;

    if (!context || !context->sdio_function)
    {
        return -RT_EINVAL;
    }
    mmcsd_host_lock(context->sdio_card->host);
    result = sdio_io_writeb(context->sdio_function, address, value);
    mmcsd_host_unlock(context->sdio_card->host);
    return result;
}

static void aic8800_sdio_initialize_registers(
    struct aic8800_context *context)
{
    struct aic8800_sdio_registers *registers = &context->sdio_registers;

    rt_memset(registers, 0, sizeof(*registers));
    if (context->sdio_v3)
    {
        registers->byte_length = AIC8800_SDIO_V3_BYTE_LENGTH;
        registers->interrupt_config = AIC8800_SDIO_V3_INTERRUPT;
        registers->interrupt_pending = AIC8800_SDIO_V3_INTERRUPT_PENDING;
        registers->wakeup = AIC8800_SDIO_V3_WAKEUP;
        registers->flow_control = AIC8800_SDIO_V3_FLOW_CONTROL;
        registers->byte_mode_enable = AIC8800_SDIO_V3_BYTE_MODE;
        registers->interrupt_status = AIC8800_SDIO_V3_INTERRUPT_STATUS;
        registers->read_fifo = AIC8800_SDIO_V3_READ_FIFO;
        registers->write_fifo = AIC8800_SDIO_V3_WRITE_FIFO;
    }
    else
    {
        registers->byte_length = AIC8800_SDIO_LEGACY_BYTE_LENGTH;
        registers->interrupt_config = AIC8800_SDIO_LEGACY_INTERRUPT;
        registers->interrupt_pending = AIC8800_SDIO_LEGACY_SLEEP;
        registers->wakeup = AIC8800_SDIO_LEGACY_WAKEUP;
        registers->flow_control = AIC8800_SDIO_LEGACY_FLOW_CONTROL;
        registers->register_block = AIC8800_SDIO_LEGACY_REGISTER_BLOCK;
        registers->byte_mode_enable = AIC8800_SDIO_LEGACY_BYTE_MODE;
        registers->block_count = AIC8800_SDIO_LEGACY_BLOCK_COUNT;
        registers->read_fifo = AIC8800_SDIO_LEGACY_READ_FIFO;
        registers->write_fifo = AIC8800_SDIO_LEGACY_WRITE_FIFO;
    }
}

static rt_err_t aic8800_sdio_initialize_function(
    struct aic8800_context *context)
{
    struct rt_sdio_function *function = context->sdio_function;
    struct rt_sdio_function *function0 = context->sdio_card->sdio_function[0];
    rt_err_t result;

    mmcsd_host_lock(context->sdio_card->host);
    result = sdio_set_block_size(function, AIC8800_SDIO_BLOCK_SIZE);
    if (result == RT_EOK)
    {
        result = sdio_enable_func(function);
    }
    if (result == RT_EOK)
    {
        context->sdio_function_enabled = RT_TRUE;
    }
    if (result == RT_EOK && !context->sdio_v3)
    {
        cpu_ticks_delay_us(100U);
    }
    if (result == RT_EOK && context->sdio_v3)
    {
        result = function0 ? sdio_io_writeb(function0, 0xf2U, 0x7fU) :
                             -RT_EIO;
    }
    if (result == RT_EOK && !context->sdio_v3)
    {
        result = sdio_io_writeb(function,
                                context->sdio_registers.register_block, 1U);
    }
    if (result == RT_EOK)
    {
        result = sdio_io_writeb(function,
                                context->sdio_registers.byte_mode_enable, 1U);
    }
    mmcsd_host_unlock(context->sdio_card->host);
    return result;
}

static rt_err_t aic8800_sdio_runtime_wakeup(
    struct aic8800_context *context)
{
    rt_uint32_t retry;
    rt_err_t result;

    result = aic8800_sdio_writeb(context,
                                 context->sdio_registers.wakeup,
                                 context->sdio_v3 ? 0x11U : 1U);
    if (result != RT_EOK)
    {
        return result;
    }
    for (retry = 0; retry < AIC8800_SDIO_WAKE_RETRIES; retry++)
    {
        rt_uint8_t wake_request;

        result = aic8800_sdio_readb(context,
                                    context->sdio_registers.wakeup,
                                    &wake_request);
        if (result != RT_EOK)
        {
            return result;
        }
        if (!(wake_request & 1U))
        {
            return RT_EOK;
        }
        cpu_ticks_delay_us(AIC8800_SDIO_POLL_DELAY_US);
    }
    return -RT_ETIMEOUT;
}

static rt_err_t aic8800_sdio_receive_length_locked(
    struct aic8800_context *context, rt_size_t *length)
{
    const struct aic8800_sdio_registers *registers =
        &context->sdio_registers;
    rt_uint8_t status;
    rt_int32_t result;

    *length = 0;
    if (!context->sdio_v3)
    {
        rt_uint32_t retry;

        for (retry = 0; retry < AIC8800_WIFI_SDIO_FLOW_RETRIES; retry++)
        {
            status = sdio_io_readb(context->sdio_function,
                                   registers->block_count, &result);
            if (result != RT_EOK)
            {
                return result;
            }
            if (!(status & AIC8800_SDIO_OTHER_INTERRUPT))
            {
                break;
            }
            cpu_ticks_delay_us(AIC8800_SDIO_POLL_DELAY_US);
        }
        if (status & AIC8800_SDIO_OTHER_INTERRUPT)
        {
            return -RT_ETIMEOUT;
        }
        if (!status)
        {
            return -RT_EEMPTY;
        }
        if (status < 64U)
        {
            *length = (rt_size_t)status * AIC8800_SDIO_BLOCK_SIZE;
        }
        else
        {
            status = sdio_io_readb(context->sdio_function,
                                   registers->byte_length, &result);
            if (result != RT_EOK)
            {
                return result;
            }
            *length = (rt_size_t)status * 4U;
        }
        return *length ? RT_EOK : -RT_EEMPTY;
    }

    status = sdio_io_readb(context->sdio_function,
                           registers->interrupt_status, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!status)
    {
        return -RT_EEMPTY;
    }
    if (status & AIC8800_SDIO_OTHER_INTERRUPT)
    {
        rt_uint8_t pending = sdio_io_readb(
            context->sdio_function, registers->interrupt_pending, &result);

        if (result != RT_EOK)
        {
            return result;
        }
        result = sdio_io_writeb(context->sdio_function,
                                registers->interrupt_pending,
                                pending & (rt_uint8_t)~1U);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    if ((status | 0x08U) > AIC8800_SDIO_V3_BYTE_MODE_STATUS)
    {
        /* Values above 120 select function 2, which is the optional
         * Bluetooth channel and is intentionally not enabled here. */
        return -RT_EEMPTY;
    }
    if (status == AIC8800_SDIO_V3_BYTE_MODE_STATUS)
    {
        status = sdio_io_readb(context->sdio_function,
                               registers->byte_length, &result);
        if (result != RT_EOK)
        {
            return result;
        }
        *length = (rt_size_t)status * 4U;
    }
    else
    {
        *length = (rt_size_t)(status & 0x7fU) *
                  AIC8800_SDIO_BLOCK_SIZE;
    }
    return *length ? RT_EOK : -RT_EEMPTY;
}

static rt_err_t aic8800_sdio_receive_one(struct aic8800_context *context,
                                          void *data, rt_size_t capacity,
                                          rt_size_t *length)
{
    rt_size_t available;
    rt_err_t result;

    if (!context || !context->transport_connected || !data || !capacity ||
        !length)
    {
        return -RT_EINVAL;
    }
    *length = 0;
    mmcsd_host_lock(context->sdio_card->host);
    result = aic8800_sdio_receive_length_locked(context, &available);
    if (result == RT_EOK && available > capacity)
    {
        result = -RT_EFULL;
    }
    if (result == RT_EOK)
    {
        result = sdio_io_read_multi_fifo_b(
            context->sdio_function, context->sdio_registers.read_fifo,
            data, available);
    }
    mmcsd_host_unlock(context->sdio_card->host);
    if (result == RT_EOK)
    {
        *length = available;
    }
    return result;
}

static rt_err_t aic8800_sdio_read_flow_credits(
    struct aic8800_context *context, rt_uint8_t *credits)
{
    rt_err_t result;

    context->sdio_tx_flow_read_count++;
    result = aic8800_sdio_readb(
        context, context->sdio_registers.flow_control, credits);
    if (result == RT_EOK && !context->sdio_v3)
    {
        *credits &= 0x7fU;
    }
    return result;
}

static void aic8800_sdio_flow_retry_delay(rt_uint32_t retry)
{
    if (retry < 30U)
    {
        cpu_ticks_delay_us(AIC8800_SDIO_POLL_DELAY_US);
    }
    else if (retry < 40U)
    {
        rt_thread_mdelay(1);
    }
    else
    {
        rt_thread_mdelay(10);
    }
}

static rt_err_t aic8800_sdio_wait_for_message_credits(
    struct aic8800_context *context, rt_size_t transfer_length,
    rt_bool_t require_active)
{
    rt_uint32_t retry;

    for (retry = 0; retry < AIC8800_WIFI_SDIO_FLOW_RETRIES; retry++)
    {
        rt_uint8_t credits;
        rt_err_t result;

        if (!context->transport_connected ||
            (require_active && !context->sdio_active))
        {
            return -RT_EBUSY;
        }
        result = aic8800_sdio_read_flow_credits(context, &credits);
        if (result != RT_EOK)
        {
            return result;
        }
        if (credits && transfer_length <
                       (rt_size_t)credits * AIC8800_SDIO_FLOW_BUFFER_SIZE)
        {
            return RT_EOK;
        }
        aic8800_sdio_flow_retry_delay(retry);
    }
    return -RT_ETIMEOUT;
}

static rt_err_t aic8800_sdio_wait_for_data_credits(
    struct aic8800_context *context, rt_uint8_t *frame_limit)
{
    rt_uint32_t retry;

    /* Match the D80 reference driver's DATA_FLOW_CTRL_THRESH behavior. Keep
     * two firmware credits in reserve for control traffic and retain the
     * mutex on success so the returned data credits cannot be consumed by a
     * concurrent transfer before this aggregate is submitted. */
    for (retry = 0; retry < AIC8800_WIFI_SDIO_FLOW_RETRIES; retry++)
    {
        rt_uint8_t credits;
        rt_err_t result;

        result = rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            return result;
        }
        if (!context->transport_connected || !context->sdio_active)
        {
            rt_mutex_release(context->tx_mutex);
            return -RT_EBUSY;
        }
        credits = context->sdio_tx_available_credits;
        if (credits <= AIC8800_SDIO_DATA_CREDIT_RESERVE)
        {
            result = aic8800_sdio_read_flow_credits(context, &credits);
            if (result != RT_EOK)
            {
                context->sdio_tx_available_credits = 0;
                rt_mutex_release(context->tx_mutex);
                return result;
            }
            context->sdio_tx_available_credits = credits;
        }
        if (credits > AIC8800_SDIO_DATA_CREDIT_RESERVE)
        {
            if (retry)
            {
                context->sdio_tx_credit_wait_count++;
                context->sdio_tx_credit_retry_count += retry;
                if (retry > context->sdio_tx_credit_max_retries)
                {
                    context->sdio_tx_credit_max_retries =
                        (rt_uint8_t)retry;
                }
            }
            credits -= AIC8800_SDIO_DATA_CREDIT_RESERVE;
            *frame_limit = credits < AIC8800_WIFI_SDIO_TX_AGGREGATE_FRAMES ?
                           credits :
                           AIC8800_WIFI_SDIO_TX_AGGREGATE_FRAMES;
            return RT_EOK;
        }
        rt_mutex_release(context->tx_mutex);
        aic8800_sdio_flow_retry_delay(retry);
    }
    context->sdio_tx_credit_wait_count++;
    context->sdio_tx_credit_retry_count +=
        AIC8800_WIFI_SDIO_FLOW_RETRIES;
    context->sdio_tx_credit_timeout_count++;
    if (AIC8800_WIFI_SDIO_FLOW_RETRIES >
        context->sdio_tx_credit_max_retries)
    {
        context->sdio_tx_credit_max_retries =
            (rt_uint8_t)AIC8800_WIFI_SDIO_FLOW_RETRIES;
    }
    return -RT_ETIMEOUT;
}

static rt_size_t aic8800_sdio_prepare_record(
    struct aic8800_context *context, rt_uint8_t *destination,
    rt_size_t capacity, const void *data, rt_size_t length)
{
    rt_size_t record_length;

    if (!context || !destination || !data ||
        length < AIC8800_USB_HEADER_SIZE)
    {
        return 0;
    }
    record_length = RT_ALIGN(length, 4U);
    if (record_length > capacity)
    {
        return 0;
    }
    rt_memset(destination, 0, record_length);
    rt_memcpy(destination, data, length);
    if ((destination[2] & 0x7fU) == AIC_USB_TYPE_DATA_TX)
    {
        rt_uint16_t payload_length;

        if (context->sdio_v3 && length >= 6U)
        {
            payload_length = aic8800_sdio_get_le16(destination + 4U);
            aic8800_sdio_put_length(
                destination,
                (rt_uint16_t)(sizeof(struct aic_wire_tx_host_descriptor) +
                              payload_length));
        }
        else
        {
            aic8800_sdio_put_length(
                destination, (rt_uint16_t)(record_length - 4U));
        }
    }
    destination[3] = context->sdio_v3 ?
                     aic8800_sdio_crc8(destination, 3U) : 0U;
    return record_length;
}

static rt_err_t aic8800_sdio_write_buffer(
    struct aic8800_context *context, rt_size_t record_length,
    rt_uint16_t frame_count)
{
    rt_size_t transfer_length = record_length;
    rt_err_t result;

    if (record_length % AIC8800_SDIO_BLOCK_SIZE)
    {
        transfer_length = RT_ALIGN(record_length + 4U,
                                   AIC8800_SDIO_BLOCK_SIZE);
    }
    if (transfer_length > context->sdio_tx_capacity)
    {
        return -RT_EFULL;
    }
    if (transfer_length > record_length)
    {
        rt_memset(context->sdio_tx_buffer + record_length, 0,
                  transfer_length - record_length);
    }
    mmcsd_host_lock(context->sdio_card->host);
    result = sdio_io_write_multi_fifo_b(
        context->sdio_function, context->sdio_registers.write_fifo,
        context->sdio_tx_buffer, transfer_length);
    mmcsd_host_unlock(context->sdio_card->host);
    if (result == RT_EOK)
    {
        context->sdio_tx_count++;
        context->sdio_tx_frame_count += frame_count;
        if (frame_count > 1U)
        {
            context->sdio_tx_aggregate_count++;
        }
        if (frame_count > context->sdio_tx_max_aggregate)
        {
            context->sdio_tx_max_aggregate = frame_count;
        }
    }
    return result;
}

static rt_err_t aic8800_sdio_transmit_direct(
    struct aic8800_context *context, const void *data, rt_size_t length,
    rt_bool_t require_active)
{
    rt_size_t record_length;
    rt_size_t transfer_length;
    rt_err_t result;

    if (!context || !context->transport_connected || !data ||
        length < AIC8800_USB_HEADER_SIZE ||
        length > AIC8800_WIFI_TX_BUFFER_SIZE ||
        (require_active && !context->sdio_active) ||
        !context->tx_mutex_initialized || !context->sdio_tx_buffer)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!context->transport_connected ||
        (require_active && !context->sdio_active))
    {
        result = -RT_EIO;
        goto done;
    }

    record_length = aic8800_sdio_prepare_record(
        context, context->sdio_tx_buffer, context->sdio_tx_capacity,
        data, length);
    if (!record_length)
    {
        result = -RT_EFULL;
        goto done;
    }
    transfer_length = record_length;
    if (record_length % AIC8800_SDIO_BLOCK_SIZE)
    {
        transfer_length = RT_ALIGN(record_length + 4U,
                                   AIC8800_SDIO_BLOCK_SIZE);
    }
    if (transfer_length > context->sdio_tx_capacity)
    {
        result = -RT_EFULL;
        goto done;
    }
    result = aic8800_sdio_wait_for_message_credits(
        context, transfer_length, require_active);
    if (result == RT_EOK)
    {
        result = aic8800_sdio_write_buffer(context, record_length, 1U);
    }
    /* Message traffic shares the firmware buffer pool. Force the data path
     * to refresh its cached count before building another aggregate. */
    context->sdio_tx_available_credits = 0;

done:
    if (result != RT_EOK && result != -RT_EBUSY)
    {
        context->sdio_tx_error_count++;
    }
    rt_mutex_release(context->tx_mutex);
    return result;
}

static rt_bool_t aic8800_sdio_requeue_tx_record(
    struct aic8800_context *context, struct aic8800_sdio_tx_record *record)
{
    return context && record && !context->sdio_tx_terminate &&
           context->sdio_active && context->sdio_tx_queue &&
           rt_mq_send(context->sdio_tx_queue, &record, sizeof(record)) ==
               RT_EOK;
}

static rt_err_t aic8800_sdio_transmit_aggregate(
    struct aic8800_context *context,
    struct aic8800_sdio_tx_record *first,
    struct aic8800_sdio_tx_record **carry)
{
    struct aic8800_sdio_tx_record *record = first;
    struct aic8800_sdio_tx_record *prefetched = RT_NULL;
    rt_size_t aggregate_length = 0;
    rt_uint8_t frame_limit = 0;
    rt_uint16_t frame_count = 0;
    rt_size_t deferred_count = 0;
    struct aic8800_tx_metadata
        metadata[AIC8800_WIFI_SDIO_TX_AGGREGATE_FRAMES];
    rt_err_t result;

    *carry = RT_NULL;
    if (context->sdio_active && !context->sdio_tx_terminate)
    {
        (void)rt_mq_recv(
            context->sdio_tx_queue, &prefetched, sizeof(prefetched),
            rt_tick_from_millisecond(
                AIC8800_WIFI_SDIO_TX_AGGREGATE_WAIT_MS));
    }
    result = aic8800_sdio_wait_for_data_credits(context, &frame_limit);
    if (result != RT_EOK)
    {
        aic8800_core_tx_complete(context, &record->metadata);
        rt_mp_free(record);
        *carry = prefetched;
        return result;
    }
    while (frame_count < frame_limit)
    {
        enum aic8800_tx_record_state state =
            aic8800_core_tx_metadata_state(context, &record->metadata);

        if (state != AIC8800_TX_RECORD_READY)
        {
            if (state != AIC8800_TX_RECORD_DEFER ||
                !aic8800_sdio_requeue_tx_record(context, record))
            {
                aic8800_core_tx_complete(context, &record->metadata);
                rt_mp_free(record);
            }
            record = RT_NULL;
            if (++deferred_count >= AIC8800_WIFI_SDIO_TX_QUEUE_DEPTH)
            {
                break;
            }
            if (prefetched)
            {
                record = prefetched;
                prefetched = RT_NULL;
            }
            else if (rt_mq_recv(context->sdio_tx_queue, &record,
                                sizeof(record), 0) != RT_EOK)
            {
                record = RT_NULL;
                break;
            }
            if (!record)
            {
                result = -RT_EBUSY;
                break;
            }
            continue;
        }
        rt_size_t prepared = aic8800_sdio_prepare_record(
            context, context->sdio_tx_buffer + aggregate_length,
            context->sdio_tx_capacity - aggregate_length,
            record->data, record->length);

        if (!prepared)
        {
            result = -RT_EFULL;
            goto done;
        }
        metadata[frame_count] = record->metadata;
        rt_mp_free(record);
        record = RT_NULL;
        aggregate_length += prepared;
        frame_count++;
        if (frame_count >= frame_limit)
        {
            break;
        }
        if (prefetched)
        {
            record = prefetched;
            prefetched = RT_NULL;
        }
        else if (rt_mq_recv(context->sdio_tx_queue, &record, sizeof(record),
                            0) != RT_EOK)
        {
            record = RT_NULL;
            break;
        }
        if (!record)
        {
            result = -RT_EBUSY;
            goto done;
        }
        if (!context->sdio_active || context->sdio_tx_terminate)
        {
            result = -RT_EBUSY;
            goto done;
        }
    }
    if (!frame_count)
    {
        result = RT_EOK;
    }
    else
    {
        result = context->sdio_active && !context->sdio_tx_terminate ?
                 aic8800_sdio_write_buffer(
                     context, aggregate_length, frame_count) : -RT_EBUSY;
    }
    if (result == RT_EOK)
    {
        if (context->sdio_tx_available_credits >= frame_count)
        {
            context->sdio_tx_available_credits -= frame_count;
        }
        else
        {
            context->sdio_tx_available_credits = 0;
        }
    }
    else
    {
        context->sdio_tx_available_credits = 0;
    }

done:
    if (record)
    {
        aic8800_core_tx_complete(context, &record->metadata);
        rt_mp_free(record);
    }
    for (rt_size_t index = 0; index < frame_count; index++)
    {
        aic8800_core_tx_complete(context, &metadata[index]);
    }
    *carry = prefetched;
    rt_mutex_release(context->tx_mutex);
    return result;
}

static void aic8800_sdio_tx_worker(void *parameter)
{
    struct aic8800_context *context = parameter;
    struct aic8800_sdio_tx_record *record = RT_NULL;
    struct aic8800_sdio_tx_record *carry = RT_NULL;

    while (!context->sdio_tx_terminate)
    {
        rt_err_t result;

        record = carry;
        carry = RT_NULL;
        if (record)
        {
            result = RT_EOK;
        }
        else
        {
            result = rt_mq_recv(context->sdio_tx_queue, &record,
                                sizeof(record), RT_WAITING_FOREVER);
        }

        if (context->sdio_tx_terminate)
        {
            if (result == RT_EOK && record)
            {
                aic8800_core_tx_complete(context, &record->metadata);
                rt_mp_free(record);
            }
            break;
        }
        if (result != RT_EOK || !context->sdio_active)
        {
            if (result == RT_EOK && record)
            {
                aic8800_core_tx_complete(context, &record->metadata);
                rt_mp_free(record);
            }
            continue;
        }
        {
            enum aic8800_tx_record_state state =
                aic8800_core_tx_metadata_state(context, &record->metadata);

            if (state == AIC8800_TX_RECORD_DEFER &&
                aic8800_sdio_requeue_tx_record(context, record))
            {
                rt_thread_mdelay(1);
                continue;
            }
            if (state != AIC8800_TX_RECORD_READY)
            {
                aic8800_core_tx_complete(context, &record->metadata);
                rt_mp_free(record);
                continue;
            }
        }
        result = aic8800_sdio_transmit_aggregate(context, record, &carry);
        if (result != RT_EOK && result != -RT_EBUSY)
        {
            context->sdio_tx_error_count++;
            if (aic8800_sdio_should_log_error(
                    context->sdio_tx_error_count))
            {
                LOG_W("SDIO transmit aggregate failed: %d (errors=%u)",
                      result,
                      (unsigned int)context->sdio_tx_error_count);
            }
        }
    }
    if (carry)
    {
        aic8800_core_tx_complete(context, &carry->metadata);
        rt_mp_free(carry);
    }
    rt_completion_done(&context->sdio_tx_thread_stopped);
}

static void aic8800_sdio_reset_tx_queue_locked(
    struct aic8800_context *context)
{
    struct aic8800_sdio_tx_record *record;

    if (!context->sdio_tx_queue)
    {
        return;
    }
    while (rt_mq_recv(context->sdio_tx_queue, &record, sizeof(record), 0) ==
           RT_EOK)
    {
        if (record)
        {
            aic8800_core_tx_complete(context, &record->metadata);
            rt_mp_free(record);
        }
    }
    rt_mq_control(context->sdio_tx_queue, RT_IPC_CMD_RESET, RT_NULL);
}

static void aic8800_sdio_reset_tx_queue(struct aic8800_context *context)
{
    if (!context->sdio_tx_queue_mutex_initialized ||
        rt_mutex_take(&context->sdio_tx_queue_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return;
    }
    aic8800_sdio_reset_tx_queue_locked(context);
    rt_mutex_release(&context->sdio_tx_queue_mutex);
}

rt_err_t aic8800_sdio_firmware_transmit(struct aic8800_context *context,
                                         const void *data, rt_size_t length)
{
    return aic8800_sdio_transmit_direct(
        context, data, length, RT_FALSE);
}

rt_err_t aic8800_sdio_firmware_receive(struct aic8800_context *context,
                                        void *data, rt_size_t capacity,
                                        rt_size_t *length,
                                        rt_uint32_t timeout_ms)
{
    rt_tick_t timeout;
    rt_tick_t start;

    if (!context || !length)
    {
        return -RT_EINVAL;
    }
    start = rt_tick_get();
    timeout = rt_tick_from_millisecond(timeout_ms);
    do
    {
        rt_err_t result = aic8800_sdio_receive_one(
            context, data, capacity, length);

        if (result != -RT_EEMPTY)
        {
            return result;
        }
        cpu_ticks_delay_us(AIC8800_SDIO_POLL_DELAY_US);
    }
    while (context->transport_connected &&
           (rt_tick_t)(rt_tick_get() - start) < timeout);
    return context->transport_connected ? -RT_ETIMEOUT : -RT_EIO;
}

rt_err_t aic8800_sdio_pump_command(
    struct aic8800_context *context,
    struct rt_wlan_offload_command_manager *manager,
    rt_uint32_t token, rt_uint32_t timeout_ms)
{
    rt_tick_t timeout;
    rt_tick_t start;

    if (!context || !manager || !token || !timeout_ms ||
        rt_thread_self() != context->sdio_thread ||
        !context->sdio_command_rx_buffer)
    {
        return -RT_EINVAL;
    }
    start = rt_tick_get();
    timeout = rt_tick_from_millisecond(timeout_ms);
    while (context->transport_connected && context->sdio_active &&
           rt_wlan_offload_command_is_pending(manager, token))
    {
        rt_size_t length = 0;
        rt_err_t result = aic8800_sdio_receive_one(
            context, context->sdio_command_rx_buffer,
            AIC8800_WIFI_SDIO_RX_BUFFER_SIZE, &length);

        if (result == -RT_EEMPTY)
        {
            if ((rt_tick_t)(rt_tick_get() - start) >= timeout)
            {
                return -RT_ETIMEOUT;
            }
            cpu_ticks_delay_us(AIC8800_SDIO_POLL_DELAY_US);
            continue;
        }
        if (result != RT_EOK)
        {
            return result;
        }
        context->sdio_rx_count++;
        result = rt_wlan_offload_bus_rx(
            &context->bus, context->sdio_command_rx_buffer, length);
        if (result != RT_EOK && result != -RT_EEMPTY)
        {
            return result;
        }
        if ((rt_tick_t)(rt_tick_get() - start) >= timeout &&
            rt_wlan_offload_command_is_pending(manager, token))
        {
            return -RT_ETIMEOUT;
        }
    }
    return rt_wlan_offload_command_is_pending(manager, token) ?
           -RT_ETIMEOUT : RT_EOK;
}

static rt_err_t aic8800_sdio_set_irq_source_locked(
    struct aic8800_context *context, rt_bool_t enabled)
{
    rt_err_t result;

    if (!context || !context->sdio_function)
    {
        return -RT_EINVAL;
    }
    result = sdio_io_writeb(
        context->sdio_function, context->sdio_registers.interrupt_config,
        enabled ? 0x07U : 0U);
    if (result == RT_EOK)
    {
        context->sdio_irq_source_masked = !enabled;
    }
    return result;
}

static rt_err_t aic8800_sdio_set_irq_source(
    struct aic8800_context *context, rt_bool_t enabled)
{
    rt_err_t result;

    if (!context || !context->sdio_card)
    {
        return -RT_EINVAL;
    }
    mmcsd_host_lock(context->sdio_card->host);
    result = aic8800_sdio_set_irq_source_locked(context, enabled);
    mmcsd_host_unlock(context->sdio_card->host);
    return result;
}

static void aic8800_sdio_irq(struct rt_sdio_function *function)
{
    struct aic8800_context *context = sdio_get_drvdata(function);

    if (context && context->sdio_event_initialized)
    {
        rt_err_t result = RT_EOK;

        /* The generic priority-4 SDIO IRQ thread re-enables the host IRQ as
         * soon as this callback returns. Mask the level-triggered device
         * source until the deferred FIFO worker has drained it. */
        if (!context->sdio_irq_source_masked)
        {
            result = aic8800_sdio_set_irq_source_locked(context, RT_FALSE);
        }
        rt_event_send(&context->sdio_event, AIC8800_SDIO_EVENT_RX);
        if (result != RT_EOK)
        {
            context->sdio_error_count++;
            if (aic8800_sdio_should_log_error(context->sdio_error_count))
            {
                LOG_W("failed to mask SDIO interrupt source: %d", result);
            }
        }
    }
}

static rt_bool_t aic8800_sdio_should_log_error(rt_uint32_t count)
{
    return count <= 4U || (count & (count - 1U)) == 0U;
}

static void aic8800_sdio_note_transport_error(
    struct aic8800_context *context, rt_err_t result)
{
    context->sdio_error_count++;
    context->sdio_consecutive_errors++;
    if (aic8800_sdio_should_log_error(context->sdio_error_count))
    {
        LOG_W("SDIO receive failed: %d (consecutive=%u total=%u)",
              result,
              (unsigned int)context->sdio_consecutive_errors,
              (unsigned int)context->sdio_error_count);
    }
    if (context->sdio_consecutive_errors <
            AIC8800_WIFI_SDIO_RX_RECOVERY_ERRORS ||
        context->sdio_recovery_reported)
    {
        return;
    }

    /* Stop consuming the asserted interrupt before reporting failure. This
     * prevents one broken transfer from queuing an unbounded recovery loop. */
    context->sdio_recovery_reported = RT_TRUE;
    context->sdio_active = RT_FALSE;
    LOG_E("SDIO receive recovery threshold reached: %u errors",
          (unsigned int)context->sdio_consecutive_errors);
    rt_wlan_offload_bus_notify(
        &context->bus, RT_WLAN_OFFLOAD_BUS_EVENT_ERROR, result);
}

static void aic8800_sdio_note_protocol_drop(
    struct aic8800_context *context, rt_err_t result)
{
    context->sdio_protocol_drop_count++;
    if (aic8800_sdio_should_log_error(context->sdio_protocol_drop_count))
    {
        LOG_W("discarded malformed SDIO aggregate: %d (drops=%u)",
              result, (unsigned int)context->sdio_protocol_drop_count);
    }
}

static void aic8800_sdio_worker(void *parameter)
{
    struct aic8800_context *context = parameter;

    while (!context->sdio_terminate)
    {
        rt_uint32_t events = 0;
        rt_uint32_t count;
        rt_bool_t drained = RT_FALSE;

        rt_event_recv(&context->sdio_event,
                      AIC8800_SDIO_EVENT_RX | AIC8800_SDIO_EVENT_STOP,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &events);
        if (context->sdio_terminate)
        {
            break;
        }
        if (!context->sdio_active)
        {
            continue;
        }
        for (count = 0; count < AIC8800_SDIO_MAX_RX_BURST; count++)
        {
            rt_size_t length = 0;
            rt_err_t result = aic8800_sdio_receive_one(
                context, context->sdio_rx_buffer,
                AIC8800_WIFI_SDIO_RX_BUFFER_SIZE, &length);

            if (result == -RT_EEMPTY)
            {
                context->sdio_consecutive_errors = 0;
                drained = RT_TRUE;
                break;
            }
            if (result != RT_EOK)
            {
                aic8800_sdio_note_transport_error(context, result);
                if (context->sdio_active)
                {
                    /* The controller interrupt may remain asserted without a
                     * new edge after a failed transaction. Retry from this
                     * worker instead of depending on another IRQ. */
                    rt_thread_mdelay(1);
                    rt_event_send(&context->sdio_event,
                                  AIC8800_SDIO_EVENT_RX);
                }
                break;
            }
            context->sdio_consecutive_errors = 0;
            context->sdio_rx_count++;
            result = rt_wlan_offload_bus_rx(&context->bus,
                                             context->sdio_rx_buffer, length);
            if (result != RT_EOK && result != -RT_EEMPTY)
            {
                /* The FIFO transfer completed. A malformed firmware record is
                 * not an SDIO controller failure and must not restart the bus. */
                aic8800_sdio_note_protocol_drop(context, result);
            }
        }
        if (count == AIC8800_SDIO_MAX_RX_BURST && context->sdio_active)
        {
            /* A level-triggered card interrupt may not generate another edge
             * while the FIFO remains non-empty. Requeue the drain and give
             * the protocol worker one tick to consume the records just read. */
            rt_event_send(&context->sdio_event, AIC8800_SDIO_EVENT_RX);
            rt_thread_delay(1);
        }
        else if (drained && context->sdio_active &&
                 context->sdio_irq_source_masked)
        {
            rt_err_t result = aic8800_sdio_set_irq_source(context, RT_TRUE);

            if (result != RT_EOK)
            {
                aic8800_sdio_note_transport_error(context, result);
                if (context->sdio_active)
                {
                    rt_event_send(&context->sdio_event,
                                  AIC8800_SDIO_EVENT_RX);
                    rt_thread_delay(1);
                }
            }
        }
    }
    rt_completion_done(&context->sdio_thread_stopped);
}

static rt_err_t aic8800_sdio_attach_irq(struct aic8800_context *context)
{
    struct rt_sdio_function *function0 =
        context->sdio_card->sdio_function[0];
    rt_err_t result;

    mmcsd_host_lock(context->sdio_card->host);
    result = sdio_attach_irq(context->sdio_function, aic8800_sdio_irq);
    if (result == RT_EOK && context->sdio_v3)
    {
        result = function0 ? sdio_io_writeb(
                                 function0, SDIO_REG_CCCR_INT_EN, 0x07U) :
                             -RT_EIO;
        if (result != RT_EOK)
        {
            sdio_detach_irq(context->sdio_function);
        }
    }
    mmcsd_host_unlock(context->sdio_card->host);
    if (result == RT_EOK)
    {
        context->sdio_irq_attached = RT_TRUE;
        /* sdio_attach_irq() installs the common SDIO IRQ thread but does not
         * unmask the K230 host controller's card-interrupt source. */
        context->sdio_card->host->ops->enable_sdio_irq(
            context->sdio_card->host, 1);
    }
    return result;
}

static void aic8800_sdio_detach_irq(struct aic8800_context *context)
{
    struct rt_sdio_function *function0;

    if (!context->sdio_irq_attached)
    {
        return;
    }
    function0 = context->sdio_card->sdio_function[0];
    mmcsd_host_lock(context->sdio_card->host);
    sdio_detach_irq(context->sdio_function);
    if (context->sdio_v3 && function0)
    {
        sdio_io_writeb(function0, SDIO_REG_CCCR_INT_EN, 0U);
    }
    mmcsd_host_unlock(context->sdio_card->host);
    context->sdio_irq_attached = RT_FALSE;
    context->sdio_irq_source_masked = RT_FALSE;
}

static rt_err_t aic8800_sdio_bus_start(struct rt_wlan_offload_bus *bus)
{
    struct aic8800_context *context =
        rt_wlan_offload_bus_get_driver_data(bus);
    rt_err_t result;

    if (!context || !context->transport_connected ||
        !context->sdio_thread_started ||
        !context->sdio_tx_thread_started || !context->sdio_tx_queue)
    {
        return -RT_EIO;
    }
    context->invalid_rx_log_count = 0;
    context->sdio_consecutive_errors = 0;
    context->sdio_recovery_reported = RT_FALSE;
    aic8800_core_tx_pending_reset(context);
    result = aic8800_sdio_set_irq_source(context, RT_FALSE);
    if (result != RT_EOK)
    {
        return result;
    }
    result = aic8800_sdio_attach_irq(context);
    if (result != RT_EOK)
    {
        return result;
    }
    aic8800_sdio_reset_tx_queue(context);
    context->sdio_tx_available_credits = 0;
    context->sdio_active = RT_TRUE;
    result = aic8800_sdio_set_irq_source(context, RT_TRUE);
    if (result != RT_EOK)
    {
        context->sdio_active = RT_FALSE;
        aic8800_sdio_detach_irq(context);
        return result;
    }
    rt_event_send(&context->sdio_event, AIC8800_SDIO_EVENT_RX);
    LOG_I("SDIO bus started on function %u: clock=%u kHz width=%u timing=%u protocol=%s aggregate=%u wait=%u ms",
          context->sdio_function->num,
          (unsigned int)(context->sdio_card->host->io_cfg.clock / 1000U),
          context->sdio_card->host->io_cfg.bus_width == MMCSD_BUS_WIDTH_4 ?
              4U : 1U,
          context->sdio_card->host->io_cfg.timing,
          context->sdio_v3 ? "v3" : "legacy",
          (unsigned int)AIC8800_WIFI_SDIO_TX_AGGREGATE_FRAMES,
          (unsigned int)AIC8800_WIFI_SDIO_TX_AGGREGATE_WAIT_MS);
    return RT_EOK;
}

static rt_err_t aic8800_sdio_bus_stop(struct rt_wlan_offload_bus *bus)
{
    struct aic8800_context *context =
        rt_wlan_offload_bus_get_driver_data(bus);
    rt_err_t result = RT_EOK;

    if (!context)
    {
        return -RT_EINVAL;
    }
    context->sdio_active = RT_FALSE;
    context->sdio_tx_available_credits = 0;
    aic8800_sdio_reset_tx_queue(context);
    /* Wait until an aggregate already removed from the queue has stopped
     * using the SDIO function and shared transmit buffer. */
    if (context->tx_mutex_initialized)
    {
        rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER);
        rt_mutex_release(context->tx_mutex);
    }
    if (context->transport_connected && context->sdio_function)
    {
        result = aic8800_sdio_set_irq_source(context, RT_FALSE);
    }
    aic8800_sdio_detach_irq(context);
    LOG_I("SDIO bus stopped: RX=%u TX transfers=%u frames=%u aggregates=%u "
          "max_aggregate=%u queue_high_water=%u queue_drops=%u "
          "flow_reads=%u credit_waits=%u credit_retries=%u "
          "credit_max_retry=%u credit_timeouts=%u TX errors=%u "
          "RX I/O errors=%u protocol drops=%u",
          (unsigned int)context->sdio_rx_count,
          (unsigned int)context->sdio_tx_count,
          (unsigned int)context->sdio_tx_frame_count,
          (unsigned int)context->sdio_tx_aggregate_count,
          (unsigned int)context->sdio_tx_max_aggregate,
          (unsigned int)context->sdio_tx_queue_high_water,
          (unsigned int)context->sdio_tx_queue_drop_count,
          (unsigned int)context->sdio_tx_flow_read_count,
          (unsigned int)context->sdio_tx_credit_wait_count,
          (unsigned int)context->sdio_tx_credit_retry_count,
          (unsigned int)context->sdio_tx_credit_max_retries,
          (unsigned int)context->sdio_tx_credit_timeout_count,
          (unsigned int)context->sdio_tx_error_count,
          (unsigned int)context->sdio_error_count,
          (unsigned int)context->sdio_protocol_drop_count);
    return result;
}

static rt_err_t aic8800_sdio_bus_transmit_priority(
    struct rt_wlan_offload_bus *bus,
    enum rt_wlan_offload_bus_priority priority,
    const void *data, rt_size_t length)
{
    struct aic8800_context *context =
        rt_wlan_offload_bus_get_driver_data(bus);
    struct aic8800_sdio_tx_record *record = RT_NULL;
    rt_tick_t wait_started = 0;
    rt_tick_t wait_timeout = 0;
    rt_err_t result;

    if (!context || !data || length < AIC8800_USB_HEADER_SIZE ||
        length > bus->max_tx_size)
    {
        return -RT_EINVAL;
    }
    if (priority == RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL)
    {
        return aic8800_sdio_transmit_direct(context, data, length, RT_TRUE);
    }
    if (!context->sdio_tx_queue_mutex_initialized)
    {
        return -RT_EIO;
    }
    result = rt_mutex_take(&context->sdio_tx_queue_mutex,
                           RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!context->sdio_active || context->sdio_tx_terminate ||
        !context->sdio_tx_queue || !context->sdio_tx_pool)
    {
        rt_mutex_release(&context->sdio_tx_queue_mutex);
        return -RT_EBUSY;
    }

    if (priority == RT_WLAN_OFFLOAD_BUS_PRIORITY_NORMAL)
    {
        wait_started = rt_tick_get();
        wait_timeout = rt_tick_from_millisecond(AIC8800_WIFI_TX_WAIT_MS);
    }
    for (;;)
    {
        record = rt_mp_alloc(context->sdio_tx_pool, 0);
        if (record && priority == RT_WLAN_OFFLOAD_BUS_PRIORITY_NORMAL &&
            context->sdio_tx_pool->block_free_count <
                AIC8800_WIFI_SDIO_TX_PRIORITY_RESERVE)
        {
            rt_mp_free(record);
            record = RT_NULL;
        }
        if (record || priority != RT_WLAN_OFFLOAD_BUS_PRIORITY_NORMAL ||
            (rt_tick_t)(rt_tick_get() - wait_started) >= wait_timeout)
        {
            break;
        }

        /* The worker does not take this mutex while returning pool blocks. */
        rt_thread_mdelay(1);
    }
    if (!record)
    {
        result = -RT_EFULL;
    }
    else
    {
        rt_uint16_t queue_depth;

        record->length = (rt_uint16_t)length;
        record->priority = (rt_uint8_t)priority;
        aic8800_core_tx_metadata_init(
            context, data, length, &record->metadata);
        rt_memcpy(record->data, data, length);
        result = priority == RT_WLAN_OFFLOAD_BUS_PRIORITY_HIGH ?
                 rt_mq_urgent(context->sdio_tx_queue, &record,
                              sizeof(record)) :
                 rt_mq_send(context->sdio_tx_queue, &record, sizeof(record));
        queue_depth = context->sdio_tx_queue->entry;
        if (result == RT_EOK &&
            queue_depth > context->sdio_tx_queue_high_water)
        {
            context->sdio_tx_queue_high_water = queue_depth;
        }
    }
    if (result != RT_EOK)
    {
        if (record)
        {
            rt_mp_free(record);
        }
        context->sdio_tx_queue_drop_count++;
        if (aic8800_sdio_should_log_error(
                context->sdio_tx_queue_drop_count))
        {
            LOG_W("SDIO transmit queue full: result=%d drops=%u depth=%u "
                  "high=%u credits=%u credit_waits=%u max_retry=%u",
                  result,
                  (unsigned int)context->sdio_tx_queue_drop_count,
                  context->sdio_tx_queue ?
                      (unsigned int)context->sdio_tx_queue->entry : 0U,
                  (unsigned int)context->sdio_tx_queue_high_water,
                  (unsigned int)context->sdio_tx_available_credits,
                  (unsigned int)context->sdio_tx_credit_wait_count,
                  (unsigned int)context->sdio_tx_credit_max_retries);
        }
    }
    rt_mutex_release(&context->sdio_tx_queue_mutex);
    return result;
}

static rt_err_t aic8800_sdio_bus_transmit(
    struct rt_wlan_offload_bus *bus, const void *data, rt_size_t length)
{
    return aic8800_sdio_bus_transmit_priority(
        bus, RT_WLAN_OFFLOAD_BUS_PRIORITY_NORMAL, data, length);
}

static const struct rt_wlan_offload_bus_ops g_aic8800_sdio_bus_ops = {
    .start = aic8800_sdio_bus_start,
    .stop = aic8800_sdio_bus_stop,
    .transmit = aic8800_sdio_bus_transmit,
    .transmit_priority = aic8800_sdio_bus_transmit_priority,
};

static void aic8800_sdio_stop_worker(struct aic8800_context *context)
{
    if (context->sdio_tx_thread)
    {
        if (!context->sdio_tx_thread_started)
        {
            rt_thread_delete(context->sdio_tx_thread);
        }
        else
        {
            context->sdio_tx_terminate = RT_TRUE;
            aic8800_sdio_reset_tx_queue(context);
            rt_completion_wait(&context->sdio_tx_thread_stopped,
                               RT_WAITING_FOREVER);
        }
        context->sdio_tx_thread = RT_NULL;
        context->sdio_tx_thread_started = RT_FALSE;
    }
    if (!context->sdio_thread)
    {
        return;
    }
    if (!context->sdio_thread_started)
    {
        rt_thread_delete(context->sdio_thread);
    }
    else
    {
        context->sdio_terminate = RT_TRUE;
        rt_event_send(&context->sdio_event, AIC8800_SDIO_EVENT_STOP);
        rt_completion_wait(&context->sdio_thread_stopped,
                           RT_WAITING_FOREVER);
    }
    context->sdio_thread = RT_NULL;
    context->sdio_thread_started = RT_FALSE;
}

static void aic8800_sdio_free_transport(struct aic8800_context *context)
{
    rt_bool_t tx_queue_locked = RT_FALSE;

    if (context->sdio_rx_buffer)
    {
        rt_free_align(context->sdio_rx_buffer);
        context->sdio_rx_buffer = RT_NULL;
    }
    if (context->sdio_command_rx_buffer)
    {
        rt_free_align(context->sdio_command_rx_buffer);
        context->sdio_command_rx_buffer = RT_NULL;
    }
    if (context->sdio_tx_buffer)
    {
        rt_free_align(context->sdio_tx_buffer);
        context->sdio_tx_buffer = RT_NULL;
    }
    if (context->sdio_tx_queue_mutex_initialized &&
        rt_mutex_take(&context->sdio_tx_queue_mutex,
                      RT_WAITING_FOREVER) == RT_EOK)
    {
        tx_queue_locked = RT_TRUE;
    }
    if (context->sdio_tx_queue)
    {
        aic8800_sdio_reset_tx_queue_locked(context);
        rt_mq_delete(context->sdio_tx_queue);
        context->sdio_tx_queue = RT_NULL;
    }
    if (context->sdio_tx_pool)
    {
        rt_mp_delete(context->sdio_tx_pool);
        context->sdio_tx_pool = RT_NULL;
    }
    if (tx_queue_locked)
    {
        rt_mutex_release(&context->sdio_tx_queue_mutex);
    }
    if (context->sdio_tx_queue_mutex_initialized)
    {
        rt_mutex_detach(&context->sdio_tx_queue_mutex);
        context->sdio_tx_queue_mutex_initialized = RT_FALSE;
    }
    if (context->sdio_event_initialized)
    {
        rt_event_detach(&context->sdio_event);
        context->sdio_event_initialized = RT_FALSE;
    }
}

static rt_err_t aic8800_sdio_prepare_transport(
    struct aic8800_context *context)
{
    rt_err_t result;

    result = rt_mutex_init(&context->sdio_tx_queue_mutex, "aic-stq",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    context->sdio_tx_queue_mutex_initialized = RT_TRUE;
    context->sdio_tx_capacity = RT_ALIGN(
        (rt_size_t)AIC8800_WIFI_TX_BUFFER_SIZE *
            AIC8800_WIFI_SDIO_TX_AGGREGATE_FRAMES + 4U,
        AIC8800_SDIO_BLOCK_SIZE);
    context->sdio_rx_buffer = rt_malloc_align(
        AIC8800_WIFI_SDIO_RX_BUFFER_SIZE, AIC8800_USB_DMA_ALIGNMENT);
    context->sdio_command_rx_buffer = rt_malloc_align(
        AIC8800_WIFI_SDIO_RX_BUFFER_SIZE, AIC8800_USB_DMA_ALIGNMENT);
    context->sdio_tx_buffer = rt_malloc_align(
        context->sdio_tx_capacity, AIC8800_USB_DMA_ALIGNMENT);
    context->sdio_tx_pool = rt_mp_create(
        "aic-stp", AIC8800_WIFI_SDIO_TX_QUEUE_DEPTH,
        sizeof(struct aic8800_sdio_tx_record));
    context->sdio_tx_queue = rt_mq_create(
        "aic-stx", sizeof(struct aic8800_sdio_tx_record *),
        AIC8800_WIFI_SDIO_TX_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    if (!context->sdio_rx_buffer || !context->sdio_command_rx_buffer ||
        !context->sdio_tx_buffer || !context->sdio_tx_pool ||
        !context->sdio_tx_queue)
    {
        aic8800_sdio_free_transport(context);
        return -RT_ENOMEM;
    }
    result = rt_event_init(&context->sdio_event, "aic-sdio",
                           RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        aic8800_sdio_free_transport(context);
        return result;
    }
    context->sdio_event_initialized = RT_TRUE;
    rt_completion_init(&context->sdio_thread_stopped);
    rt_completion_init(&context->sdio_tx_thread_stopped);
    context->sdio_thread = rt_thread_create(
        "aic-sdio", aic8800_sdio_worker, context,
        AIC8800_WIFI_RX_THREAD_STACK_SIZE,
        AIC8800_WIFI_SDIO_BUS_THREAD_PRIORITY, 10U);
    if (!context->sdio_thread)
    {
        aic8800_sdio_free_transport(context);
        return -RT_ENOMEM;
    }
    context->sdio_tx_thread = rt_thread_create(
        "aic-stx", aic8800_sdio_tx_worker, context,
        AIC8800_WIFI_SDIO_TX_THREAD_STACK_SIZE,
        AIC8800_WIFI_SDIO_TX_THREAD_PRIORITY, 10U);
    if (!context->sdio_tx_thread)
    {
        aic8800_sdio_stop_worker(context);
        aic8800_sdio_free_transport(context);
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(context->sdio_thread);
    if (result != RT_EOK)
    {
        aic8800_sdio_stop_worker(context);
        aic8800_sdio_free_transport(context);
        return result;
    }
    context->sdio_thread_started = RT_TRUE;
    result = rt_thread_startup(context->sdio_tx_thread);
    if (result != RT_EOK)
    {
        aic8800_sdio_stop_worker(context);
        aic8800_sdio_free_transport(context);
        return result;
    }
    context->sdio_tx_thread_started = RT_TRUE;
    return RT_EOK;
}

static void aic8800_sdio_attach_work(struct rt_work *work, void *work_data)
{
    struct aic8800_context *context = work_data;
    struct rt_wlan_offload_bus_config bus_config;
    rt_bool_t attach_runtime = RT_FALSE;
    rt_err_t result = -RT_EIO;

    (void)work;
    if (!context || !context->transport_connected ||
        !context->sdio_card || !context->sdio_function)
    {
        return;
    }

    result = aic8800_firmware_wait_available(
        context, AIC8800_WIFI_COMMAND_TIMEOUT_MS);
    if (result != RT_EOK)
    {
        goto fail;
    }
    result = aic8800_sdio_initialize_function(context);
    if (result != RT_EOK)
    {
        LOG_E("SDIO function initialization failed: %d", result);
        goto fail;
    }
    result = aic8800_sdio_prepare_transport(context);
    if (result != RT_EOK)
    {
        LOG_E("SDIO transport initialization failed: %d", result);
        goto fail;
    }
    result = aic8800_firmware_probe(context, &attach_runtime);
    if (result != RT_EOK || !attach_runtime)
    {
        if (result == RT_EOK)
        {
            result = -RT_EIO;
        }
        LOG_E("SDIO firmware initialization failed: %d", result);
        goto fail;
    }
    if (!context->transport_connected)
    {
        result = -RT_EIO;
        goto fail;
    }
    result = aic8800_sdio_runtime_wakeup(context);
    if (result != RT_EOK)
    {
        LOG_E("SDIO runtime wake failed: %d", result);
        goto fail;
    }
    rt_memset(&bus_config, 0, sizeof(bus_config));
    bus_config.type = RT_WLAN_OFFLOAD_BUS_SDIO;
    bus_config.ops = &g_aic8800_sdio_bus_ops;
    bus_config.capabilities = RT_WLAN_OFFLOAD_BUS_CAP_PACKET |
                              RT_WLAN_OFFLOAD_BUS_CAP_HOTPLUG |
                              RT_WLAN_OFFLOAD_BUS_CAP_TX_PRIORITY |
                              RT_WLAN_OFFLOAD_BUS_CAP_DMA;
    bus_config.max_tx_size = AIC8800_WIFI_TX_BUFFER_SIZE;
    bus_config.max_rx_size = AIC8800_WIFI_SDIO_RX_BUFFER_SIZE;
    bus_config.alignment = 4U;
    bus_config.driver_data = context;
    result = rt_wlan_offload_bus_init(&context->bus, &bus_config);
    if (result != RT_EOK)
    {
        LOG_E("WLAN offload bus initialization failed: %d", result);
        goto fail;
    }
    context->bus_initialized = RT_TRUE;

    result = aic8800_core_attach(context);
    if (result != RT_EOK)
    {
        rt_err_t cleanup_result;

        LOG_E("WLAN offload radio attachment failed: %d", result);
        cleanup_result = rt_wlan_offload_bus_deinit(&context->bus);
        if (cleanup_result == RT_EOK)
        {
            context->bus_initialized = RT_FALSE;
        }
        else
        {
            LOG_E("WLAN offload bus rollback failed: %d", cleanup_result);
        }
        goto fail;
    }

    context->sdio_attach_result = RT_EOK;
    LOG_I("attached AIC SDIO device %04x:%04x on function %u",
          context->sdio_vendor_id, context->sdio_product_id,
          context->sdio_function->num);
    return;

fail:
    context->sdio_attach_result = result;
    context->sdio_active = RT_FALSE;
    aic8800_sdio_detach_irq(context);
    context->transport_connected = RT_FALSE;
    aic8800_firmware_disconnected(context);
    aic8800_sdio_stop_worker(context);
    aic8800_sdio_free_transport(context);
    if (context->sdio_function_enabled)
    {
        mmcsd_host_lock(context->sdio_card->host);
        sdio_disable_func(context->sdio_function);
        mmcsd_host_unlock(context->sdio_card->host);
        context->sdio_function_enabled = RT_FALSE;
    }
}

static rt_int32_t aic8800_sdio_probe(struct rt_mmcsd_card *card)
{
    struct aic8800_context *context = &g_aic8800_sdio_context;
    struct rt_sdio_function *function;
    rt_err_t result;

    if (!card || card->sdio_function_num < AIC8800_SDIO_FUNCTION ||
        !card->sdio_function[AIC8800_SDIO_FUNCTION] ||
        context->sdio_function || !g_aic8800_sdio_ipc_initialized)
    {
        return -RT_EINVAL;
    }
    function = card->sdio_function[AIC8800_SDIO_FUNCTION];
    rt_memset(context, 0, sizeof(*context));
    context->transport = AIC8800_TRANSPORT_SDIO;
    context->sdio_card = card;
    context->sdio_function = function;
    context->sdio_vendor_id = card->cis.manufacturer;
    context->sdio_product_id = function->product;
    context->vendor_id = AIC8800_USB_VENDOR_ID;
    if (context->sdio_vendor_id == AIC8800_SDIO_VENDOR_AIC8801 &&
        context->sdio_product_id == AIC8800_SDIO_PRODUCT_AIC8801)
    {
        context->product_id = AIC8800_USB_PID_AIC8800;
    }
    else if (context->sdio_vendor_id == AIC8800_SDIO_VENDOR_AIC8800D80 &&
             context->sdio_product_id == AIC8800_SDIO_PRODUCT_AIC8800D80)
    {
        context->product_id = AIC8800_USB_PID_AIC8800D80;
        context->sdio_v3 = RT_TRUE;
    }
    else
    {
        rt_memset(context, 0, sizeof(*context));
        return -RT_ENOSYS;
    }
    context->vif_index = AIC8800_INVALID_INDEX;
    context->ap_station_index = AIC8800_INVALID_INDEX;
    context->ap_vif_index = AIC8800_INVALID_INDEX;
    context->ap_broadcast_station_index = AIC8800_INVALID_INDEX;
    context->tx_mutex = &g_aic8800_sdio_tx_mutex;
    context->tx_mutex_initialized = RT_TRUE;
    context->frame_mutex = &g_aic8800_sdio_frame_mutex;
    context->frame_mutex_initialized = RT_TRUE;
    aic8800_sdio_initialize_registers(context);
    sdio_set_drvdata(function, context);

    context->transport_connected = RT_TRUE;
    context->sdio_attach_result = -RT_EIO;
    rt_work_init(&context->sdio_attach_work, aic8800_sdio_attach_work,
                 context);
    context->sdio_attach_work_initialized = RT_TRUE;
    result = rt_workqueue_dowork(g_aic8800_sdio_attach_workqueue,
                                 &context->sdio_attach_work);
    if (result != RT_EOK)
    {
        LOG_E("failed to queue SDIO attach work: %d", result);
        context->sdio_attach_work_initialized = RT_FALSE;
        goto queue_failed;
    }

    /* Card discovery holds the mmcsd host lock while invoking probe.  Defer
     * function setup and firmware loading until after discovery releases it. */
    LOG_I("queued AIC SDIO device %04x:%04x on function %u",
          context->sdio_vendor_id, context->sdio_product_id, function->num);
    return RT_EOK;

queue_failed:
    context->transport_connected = RT_FALSE;
    aic8800_firmware_disconnected(context);
    sdio_set_drvdata(function, RT_NULL);
    rt_memset(context, 0, sizeof(*context));
    return result;
}

static rt_int32_t aic8800_sdio_remove(struct rt_mmcsd_card *card)
{
    struct aic8800_context *context = &g_aic8800_sdio_context;
    struct rt_sdio_function *function;
    rt_err_t result;

    if (!context->sdio_function || context->sdio_card != card)
    {
        return RT_EOK;
    }
    function = context->sdio_function;
    if (context->sdio_attach_work_initialized)
    {
        rt_workqueue_cancel_work_sync(g_aic8800_sdio_attach_workqueue,
                                      &context->sdio_attach_work);
        context->sdio_attach_work_initialized = RT_FALSE;
    }
    if (context->attached)
    {
        result = aic8800_core_detach(context);
        if (result != RT_EOK)
        {
            LOG_E("WLAN offload radio teardown failed: %d", result);
        }
    }
    context->sdio_active = RT_FALSE;
    aic8800_sdio_detach_irq(context);
    context->transport_connected = RT_FALSE;
    aic8800_sdio_stop_worker(context);
    if (context->bus_initialized)
    {
        result = rt_wlan_offload_bus_deinit(&context->bus);
        if (result != RT_EOK)
        {
            LOG_E("WLAN offload bus teardown failed: %d", result);
        }
        else
        {
            context->bus_initialized = RT_FALSE;
        }
    }
    aic8800_firmware_disconnected(context);
    aic8800_sdio_free_transport(context);
    if (context->sdio_function_enabled)
    {
        mmcsd_host_lock(card->host);
        sdio_disable_func(function);
        mmcsd_host_unlock(card->host);
    }
    sdio_set_drvdata(function, RT_NULL);
    LOG_I("detached AIC SDIO device %04x:%04x",
          context->sdio_vendor_id, context->sdio_product_id);
    rt_memset(context, 0, sizeof(*context));
    return RT_EOK;
}

static struct rt_sdio_device_id g_aic8801_sdio_id = {
    SDIO_ANY_FUNC_ID, AIC8800_SDIO_VENDOR_AIC8801,
    AIC8800_SDIO_PRODUCT_AIC8801,
};

static struct rt_sdio_device_id g_aic8800d80_sdio_id = {
    SDIO_ANY_FUNC_ID, AIC8800_SDIO_VENDOR_AIC8800D80,
    AIC8800_SDIO_PRODUCT_AIC8800D80,
};

static struct rt_sdio_driver g_aic8801_sdio_driver = {
    "aic8801-wifi", aic8800_sdio_probe, aic8800_sdio_remove,
    &g_aic8801_sdio_id,
};

static struct rt_sdio_driver g_aic8800d80_sdio_driver = {
    "aic8800d80-wifi", aic8800_sdio_probe, aic8800_sdio_remove,
    &g_aic8800d80_sdio_id,
};

rt_err_t aic8800_sdio_driver_init(void)
{
    rt_err_t result;

    if (!g_aic8800_sdio_ipc_initialized)
    {
        result = rt_mutex_init(&g_aic8800_sdio_tx_mutex, "aic-tx",
                               RT_IPC_FLAG_PRIO);
        if (result != RT_EOK)
        {
            return result;
        }
        result = rt_mutex_init(&g_aic8800_sdio_frame_mutex, "aic-frm",
                               RT_IPC_FLAG_PRIO);
        if (result != RT_EOK)
        {
            rt_mutex_detach(&g_aic8800_sdio_tx_mutex);
            return result;
        }
        g_aic8800_sdio_ipc_initialized = RT_TRUE;
    }
    if (!g_aic8800_sdio_attach_workqueue)
    {
        g_aic8800_sdio_attach_workqueue = rt_workqueue_create(
            "aic-sdio-init", AIC8800_WIFI_ATTACH_THREAD_STACK_SIZE,
            AIC8800_WIFI_ATTACH_THREAD_PRIORITY);
        if (!g_aic8800_sdio_attach_workqueue)
        {
            return -RT_ENOMEM;
        }
    }

    result = sdio_register_driver(&g_aic8801_sdio_driver);
    if (result != RT_EOK && result != -RT_EEMPTY)
    {
        return result;
    }
    result = sdio_register_driver(&g_aic8800d80_sdio_driver);
    if (result != RT_EOK && result != -RT_EEMPTY)
    {
        sdio_unregister_driver(&g_aic8801_sdio_driver);
        return result;
    }
    return RT_EOK;
}

static int aic8800_sdio_component_init(void)
{
    return aic8800_sdio_driver_init();
}
INIT_COMPONENT_EXPORT(aic8800_sdio_component_init);
