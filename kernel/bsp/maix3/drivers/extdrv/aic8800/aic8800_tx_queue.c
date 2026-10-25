/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "aic8800_tx_queue.h"
#include <rthw.h>

#define AIC8800_TX_QUEUE_WAKE 1U
#define AIC8800_TX_QUEUE_CHECK 0x69c7f4a1U
#define AIC8800_TX_QUEUE_STOP_CHECK 0x80000000U

struct aic8800_tx_queue
{
    struct rt_event ready;
    rt_uint16_t head;
    rt_uint16_t count;
    rt_uint32_t state_check;
    rt_bool_t faulted;
    rt_bool_t stopped;
    /* Keep the event's IPC links and ring state out of the payload cache lines. */
    ALIGN(64) struct aic8800_usb_tx_queue_entry
        entries[AIC8800_WIFI_USB_TX_QUEUE_DEPTH];
};

static rt_uint32_t aic8800_tx_queue_check(const struct aic8800_tx_queue *queue)
{
    return AIC8800_TX_QUEUE_CHECK ^ queue->head ^
           ((rt_uint32_t)queue->count << 16) ^
           (queue->stopped ? AIC8800_TX_QUEUE_STOP_CHECK : 0U);
}

static rt_bool_t aic8800_tx_queue_valid(const struct aic8800_tx_queue *queue)
{
    return !queue->faulted && queue->head < AIC8800_WIFI_USB_TX_QUEUE_DEPTH &&
           queue->count <= AIC8800_WIFI_USB_TX_QUEUE_DEPTH &&
           (queue->stopped == RT_FALSE || queue->stopped == RT_TRUE) &&
           queue->state_check == aic8800_tx_queue_check(queue);
}

struct aic8800_tx_queue *aic8800_tx_queue_create(void)
{
    struct aic8800_tx_queue *queue = rt_malloc_align(sizeof(*queue), 64);

    if (!queue)
        return RT_NULL;
    rt_memset(queue, 0, sizeof(*queue));
    queue->state_check = aic8800_tx_queue_check(queue);
    if (rt_event_init(&queue->ready, "aic-utxq", RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        rt_free_align(queue);
        return RT_NULL;
    }
    return queue;
}

void aic8800_tx_queue_delete(struct aic8800_tx_queue *queue)
{
    if (queue)
    {
        rt_event_detach(&queue->ready);
        rt_free_align(queue);
    }
}

void aic8800_tx_queue_reset(struct aic8800_tx_queue *queue)
{
    if (!queue)
        return;
    /* Never walk old cells or indices: the caller reclaims orphaned records
     * from the separately owned TX pool when the ring cannot be drained. */
    queue->head = 0;
    queue->count = 0;
    queue->faulted = RT_FALSE;
    queue->stopped = RT_FALSE;
    queue->state_check = aic8800_tx_queue_check(queue);
    rt_event_control(&queue->ready, RT_IPC_CMD_RESET, RT_NULL);
}

void aic8800_tx_queue_stop(struct aic8800_tx_queue *queue)
{
    rt_base_t level;

    if (!queue)
        return;
    level = rt_hw_interrupt_disable();
    /* Do not legitimize corrupt state when publishing a normal stop. */
    if (!aic8800_tx_queue_valid(queue))
        queue->faulted = RT_TRUE;
    queue->stopped = RT_TRUE;
    queue->state_check = aic8800_tx_queue_check(queue);
    rt_hw_interrupt_enable(level);
    /* Out-of-band wakeup also works for a full or damaged ring. */
    rt_event_send(&queue->ready, AIC8800_TX_QUEUE_WAKE);
}

rt_size_t aic8800_tx_queue_count(struct aic8800_tx_queue *queue)
{
    rt_size_t count;
    rt_base_t level;

    if (!queue)
        return 0;
    level = rt_hw_interrupt_disable();
    count = aic8800_tx_queue_valid(queue) ? queue->count : 0;
    rt_hw_interrupt_enable(level);
    return count;
}

rt_err_t aic8800_tx_queue_send(struct aic8800_tx_queue *queue,
                              const struct aic8800_usb_tx_queue_entry *entry,
                              rt_bool_t urgent)
{
    rt_size_t index;
    rt_base_t level;

    if (!queue || !entry)
        return -RT_EINVAL;
    level = rt_hw_interrupt_disable();
    if (!aic8800_tx_queue_valid(queue))
    {
        queue->faulted = RT_TRUE;
        rt_hw_interrupt_enable(level);
        rt_event_send(&queue->ready, AIC8800_TX_QUEUE_WAKE);
        return -RT_EIO;
    }
    if (queue->stopped)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EINTR;
    }
    if (queue->count == AIC8800_WIFI_USB_TX_QUEUE_DEPTH)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EFULL;
    }
    if (urgent)
    {
        queue->head = queue->head ? queue->head - 1U :
                                   AIC8800_WIFI_USB_TX_QUEUE_DEPTH - 1U;
        index = queue->head;
    }
    else
        index = (queue->head + queue->count) % AIC8800_WIFI_USB_TX_QUEUE_DEPTH;

    /* Serialize the copy with publication, including worker-side requeues. */
    queue->entries[index] = *entry;
    queue->count++;
    queue->state_check = aic8800_tx_queue_check(queue);
    rt_hw_interrupt_enable(level);
    rt_event_send(&queue->ready, AIC8800_TX_QUEUE_WAKE);
    return RT_EOK;
}

rt_err_t aic8800_tx_queue_recv(struct aic8800_tx_queue *queue,
                              struct aic8800_usb_tx_queue_entry *entry,
                              rt_int32_t timeout)
{
    rt_tick_t start = rt_tick_get();

    if (!queue || !entry)
        return -RT_EINVAL;
    for (;;)
    {
        rt_uint32_t received;
        rt_int32_t remaining = timeout;
        rt_err_t result;
        rt_base_t level = rt_hw_interrupt_disable();

        if (!aic8800_tx_queue_valid(queue))
        {
            queue->faulted = RT_TRUE;
            rt_hw_interrupt_enable(level);
            return -RT_EIO;
        }
        if (queue->count)
        {
            *entry = queue->entries[queue->head];
            queue->head = (queue->head + 1U) % AIC8800_WIFI_USB_TX_QUEUE_DEPTH;
            queue->count--;
            queue->state_check = aic8800_tx_queue_check(queue);
            rt_hw_interrupt_enable(level);
            return RT_EOK;
        }
        if (queue->stopped)
        {
            rt_hw_interrupt_enable(level);
            return -RT_EINTR;
        }
        rt_hw_interrupt_enable(level);
        if (!timeout)
            return -RT_ETIMEOUT;
        if (timeout > 0)
        {
            rt_tick_t elapsed = rt_tick_get() - start;

            if (elapsed >= (rt_tick_t)timeout)
                return -RT_ETIMEOUT;
            remaining -= elapsed;
        }
        /* Events coalesce: a stale wakeup is harmless, and a send between
         * the empty check and this wait cannot be lost. */
        result = rt_event_recv(&queue->ready, AIC8800_TX_QUEUE_WAKE,
                               RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                               remaining, &received);
        if (result != RT_EOK)
            return result;
    }
}
