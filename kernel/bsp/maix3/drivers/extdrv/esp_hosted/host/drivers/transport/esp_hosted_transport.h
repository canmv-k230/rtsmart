/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_HOSTED_TRANSPORT_H
#define ESP_HOSTED_TRANSPORT_H

#include <rtthread.h>

#define ESP_HOSTED_TRANSPORT_FRAME_SIZE  1600
#define ESP_HOSTED_TRANSPORT_HEADER_SIZE 12

enum esp_hosted_transport_interface
{
    ESP_HOSTED_TRANSPORT_IF_STA = 1,
    ESP_HOSTED_TRANSPORT_IF_AP = 2,
    ESP_HOSTED_TRANSPORT_IF_SERIAL = 3,
    ESP_HOSTED_TRANSPORT_IF_HCI = 4,
    ESP_HOSTED_TRANSPORT_IF_PRIVATE = 5,
};

typedef void (*esp_hosted_transport_rx_t)(void *argument, uint8_t interface,
                                          uint8_t flags, const uint8_t *data,
                                          size_t length);
typedef void (*esp_hosted_transport_tx_done_t)(void *argument, rt_err_t result);

struct esp_hosted_transport_callbacks
{
    esp_hosted_transport_rx_t receive;
};

rt_err_t esp_hosted_transport_init(const struct esp_hosted_transport_callbacks *callbacks,
                                   void *argument);
rt_err_t esp_hosted_transport_start(void);
rt_err_t esp_hosted_transport_send(uint8_t interface, uint8_t flags,
                                   const void *data, size_t length,
                                   rt_bool_t control,
                                   esp_hosted_transport_tx_done_t done,
                                   void *done_argument);
#ifdef ESP_HOSTED_BLE
rt_err_t esp_hosted_transport_send_hci(uint8_t packet_type,
                                       const void *data, size_t length,
                                       esp_hosted_transport_tx_done_t done,
                                       void *done_argument);
void esp_hosted_transport_flush_hci(void);
#endif
rt_err_t esp_hosted_transport_set_slave_capabilities(uint8_t capabilities,
                                                     uint32_t ext_capabilities);
rt_bool_t esp_hosted_transport_can_send_data(void);
size_t esp_hosted_transport_max_payload(void);
const char *esp_hosted_transport_name(void);

#endif /* ESP_HOSTED_TRANSPORT_H */
