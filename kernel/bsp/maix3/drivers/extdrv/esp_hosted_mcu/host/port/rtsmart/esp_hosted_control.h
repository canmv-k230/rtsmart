/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RT_ESP_HOSTED_CONTROL_H
#define RT_ESP_HOSTED_CONTROL_H

#include <rtthread.h>

struct esp_hosted_control_info
{
    uint8_t coprocessor_id;
    uint32_t firmware_version;
    rt_bool_t bluetooth_supported;
    rt_bool_t bluetooth_dual_mode;
};

struct esp_hosted_control_callbacks
{
    void (*new_session)(const struct esp_hosted_control_info *info,
                        void *argument);
    void (*configured)(rt_err_t result, void *argument);
};

void esp_hosted_control_init(
    const struct esp_hosted_control_callbacks *callbacks, void *argument);
void esp_hosted_control_receive(const uint8_t *data, size_t length);

#endif /* RT_ESP_HOSTED_CONTROL_H */
