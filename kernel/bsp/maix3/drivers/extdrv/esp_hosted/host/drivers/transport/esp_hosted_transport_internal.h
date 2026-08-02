/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_HOSTED_TRANSPORT_INTERNAL_H
#define ESP_HOSTED_TRANSPORT_INTERNAL_H

#include "esp_hosted_transport.h"

#if !defined(ESP_HOSTED_TRANSPORT_SPI_FD) && \
    !defined(ESP_HOSTED_TRANSPORT_SPI_HD)
#define ESP_HOSTED_TRANSPORT_SPI_FD
#endif

#define EH_TRANSPORT_IF_MAX          8
#define EH_TRANSPORT_FLOW_CTRL_ON    1
#define EH_TRANSPORT_FLOW_CTRL_OFF   2
#define EH_TRANSPORT_CTRL_QUEUE_DEPTH 8
#ifdef ESP_HOSTED_BLE
#define EH_TRANSPORT_HCI_QUEUE_DEPTH 16
#endif
#define EH_TRANSPORT_DATA_QUEUE_DEPTH 16
#define EH_TRANSPORT_EVENT_WAKE      (1U << 0)
#define EH_TRANSPORT_INVALID_LOG_LIMIT 1
#define EH_TRANSPORT_MQ_POOL_SIZE(depth, type) \
    ((depth) * (RT_ALIGN(sizeof(type), RT_ALIGN_SIZE) + sizeof(void *)))

struct eh_transport;

struct eh_transport_tx_item
{
    uint8_t *frame;
    uint16_t length;
#ifdef ESP_HOSTED_BLE
    uint8_t hci;
#endif
    rt_bool_t control;
    esp_hosted_transport_tx_done_t done;
    void *done_argument;
};

struct eh_transport_ops
{
    const char *name;
    size_t frame_size;
    size_t tx_alignment;
    rt_err_t (*init)(struct eh_transport *transport);
    rt_err_t (*start)(struct eh_transport *transport);
    void (*run)(struct eh_transport *transport);
    rt_err_t (*set_slave_capabilities)(struct eh_transport *transport,
                                       uint8_t capabilities,
                                       uint32_t ext_capabilities);
};

struct eh_transport
{
    const struct eh_transport_ops *ops;
    struct esp_hosted_transport_callbacks callbacks;
    void *callback_argument;
    void *backend;
    struct rt_event event;
    struct rt_messagequeue ctrl_queue;
#ifdef ESP_HOSTED_BLE
    struct rt_messagequeue hci_queue;
#endif
    struct rt_messagequeue data_queue;
    rt_thread_t thread;
    volatile rt_bool_t tx_throttled;
    uint8_t invalid_rx_log_count;
    uint8_t ctrl_pool[EH_TRANSPORT_MQ_POOL_SIZE(EH_TRANSPORT_CTRL_QUEUE_DEPTH,
                                                struct eh_transport_tx_item)]
        __attribute__((aligned(RT_ALIGN_SIZE)));
#ifdef ESP_HOSTED_BLE
    uint8_t hci_pool[EH_TRANSPORT_MQ_POOL_SIZE(EH_TRANSPORT_HCI_QUEUE_DEPTH,
                                               struct eh_transport_tx_item)]
        __attribute__((aligned(RT_ALIGN_SIZE)));
#endif
    uint8_t data_pool[EH_TRANSPORT_MQ_POOL_SIZE(EH_TRANSPORT_DATA_QUEUE_DEPTH,
                                                struct eh_transport_tx_item)]
        __attribute__((aligned(RT_ALIGN_SIZE)));
};

extern const struct eh_transport_ops g_esp_hosted_spi_fd_ops;
extern const struct eh_transport_ops g_esp_hosted_spi_hd_ops;

rt_err_t eh_transport_wait(struct eh_transport *transport, rt_int32_t timeout);
void eh_transport_wake(struct eh_transport *transport);
rt_bool_t eh_transport_next_tx(struct eh_transport *transport,
                               struct eh_transport_tx_item *item);
void eh_transport_complete_tx(struct eh_transport_tx_item *item, rt_err_t result);
rt_bool_t eh_transport_deliver(struct eh_transport *transport,
                               const uint8_t *frame, size_t frame_length,
                               rt_bool_t *malformed);

#endif /* ESP_HOSTED_TRANSPORT_INTERNAL_H */
