/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_HOSTED_HCI_H
#define ESP_HOSTED_HCI_H

#include <stddef.h>
#include <stdint.h>
#include <rtthread.h>

#ifdef ESP_HOSTED_BLE
rt_err_t esp_hosted_hci_init(void);
void esp_hosted_hci_deinit(void);
void esp_hosted_hci_receive(const uint8_t *data, size_t length);
void esp_hosted_hci_reset(void);
#else
static inline rt_err_t esp_hosted_hci_init(void)
{
    return RT_EOK;
}

static inline void esp_hosted_hci_deinit(void)
{
}

static inline void esp_hosted_hci_receive(const uint8_t *data, size_t length)
{
    (void)data;
    (void)length;
}

static inline void esp_hosted_hci_reset(void)
{
}
#endif

#endif /* ESP_HOSTED_HCI_H */
