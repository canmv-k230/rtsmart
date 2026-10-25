/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __AIC8800_TX_QUEUE_H__
#define __AIC8800_TX_QUEUE_H__

#include <rtthread.h>

#ifdef RT_USING_SMP
#error "AIC8800 USB transport requires a single-core RT-Thread build"
#endif

#ifndef AIC8800_WIFI_USB_TX_QUEUE_DEPTH
#define AIC8800_WIFI_USB_TX_QUEUE_DEPTH 64U
#endif

/* Payload corruption must never turn a queue cell into a native list link. */
struct aic8800_usb_tx_queue_entry
{
    rt_uint64_t token;
    rt_uint8_t reserved[48];
    rt_uint64_t token_check;
};

_Static_assert(sizeof(struct aic8800_usb_tx_queue_entry) == 64,
               "AIC USB TX queue entries must be 64 bytes");
_Static_assert(AIC8800_WIFI_USB_TX_QUEUE_DEPTH > 0 &&
                   AIC8800_WIFI_USB_TX_QUEUE_DEPTH <= 256,
               "AIC USB TX queue depth must fit the token index");

struct aic8800_tx_queue;

/* Multiple senders, one consumer; synchronization follows this port's
 * single-core interrupt critical sections. */
struct aic8800_tx_queue *aic8800_tx_queue_create(void);
/* Reset/delete require the producer barrier and a quiescent consumer. */
void aic8800_tx_queue_delete(struct aic8800_tx_queue *queue);
void aic8800_tx_queue_reset(struct aic8800_tx_queue *queue);
void aic8800_tx_queue_stop(struct aic8800_tx_queue *queue);
rt_size_t aic8800_tx_queue_count(struct aic8800_tx_queue *queue);
rt_err_t aic8800_tx_queue_send(struct aic8800_tx_queue *queue,
                              const struct aic8800_usb_tx_queue_entry *entry,
                              rt_bool_t urgent);
rt_err_t aic8800_tx_queue_recv(struct aic8800_tx_queue *queue,
                              struct aic8800_usb_tx_queue_entry *entry,
                              rt_int32_t timeout);

#endif
