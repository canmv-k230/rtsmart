/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_hosted.h"
#include "rpc_wrap.h"

#define DBG_TAG "esp.hosted.ota"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

esp_err_t esp_hosted_slave_ota_begin(void)
{
    LOG_D("Starting OTA on slave device");
    return rpc_ota_begin();
}

esp_err_t esp_hosted_slave_ota_write(uint8_t* ota_data, uint32_t ota_data_len)
{
    esp_err_t ret = ESP_OK;

    if (!ota_data || ota_data_len == 0) {
        LOG_E("Invalid OTA data parameters");
        return ESP_ERR_INVALID_ARG;
    }

    LOG_D("Writing %u bytes of OTA data", (unsigned int)ota_data_len);
    ret = rpc_ota_write(ota_data, ota_data_len);
    rt_thread_mdelay(10);
    return ret;
}

esp_err_t esp_hosted_slave_ota_end(void)
{
    LOG_D("Ending OTA on slave device");
    return rpc_ota_end();
}

esp_err_t esp_hosted_slave_ota_activate(void)
{
    LOG_D("Activating OTA on slave device");
    return rpc_ota_activate();
}

RTM_EXPORT(esp_hosted_slave_ota_begin);
RTM_EXPORT(esp_hosted_slave_ota_write);
RTM_EXPORT(esp_hosted_slave_ota_end);
RTM_EXPORT(esp_hosted_slave_ota_activate);
