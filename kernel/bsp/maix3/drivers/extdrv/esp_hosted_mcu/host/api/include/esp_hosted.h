/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RT_ESP_HOSTED_H
#define RT_ESP_HOSTED_H

#include "esp_hosted_api_types.h"
#include "esp_hosted_ota.h"
#include "esp_hosted_rpc.h"
#include "esp_hosted_wifi_remote.h"

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

int rt_hw_esp_hosted_init(void);
rt_bool_t rt_esp_hosted_is_ready(void);

int esp_hosted_init(void);
int esp_hosted_deinit(void);
int esp_hosted_connect_to_slave(void);
esp_err_t esp_hosted_get_coprocessor_fwversion(
    esp_hosted_coprocessor_fwver_t *ver_info);
esp_err_t esp_hosted_get_cp_info(uint32_t *cp_chip_id, char *cp_target_name,
                                 size_t cp_target_name_len);
esp_err_t esp_hosted_bt_controller_init(void);
esp_err_t esp_hosted_bt_controller_deinit(bool mem_release);
esp_err_t esp_hosted_bt_controller_enable(void);
esp_err_t esp_hosted_bt_controller_disable(void);
esp_err_t esp_hosted_iface_mac_addr_set(uint8_t *mac, size_t mac_len,
                                        esp_mac_type_t type);
esp_err_t esp_hosted_iface_mac_addr_get(uint8_t *mac, size_t mac_len,
                                        esp_mac_type_t type);
size_t esp_hosted_iface_mac_addr_len_get(esp_mac_type_t type);
esp_err_t esp_hosted_get_coprocessor_app_desc(
    esp_hosted_app_desc_t *app_desc);
esp_err_t esp_hosted_configure_heartbeat(bool enable, int duration_sec);
#ifdef ESP_HOSTED_BLE
rt_bool_t rt_esp_hosted_bt_supported(void);
rt_err_t rt_esp_hosted_bt_controller_start(void);
rt_err_t rt_esp_hosted_bt_controller_stop(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* RT_ESP_HOSTED_H */
