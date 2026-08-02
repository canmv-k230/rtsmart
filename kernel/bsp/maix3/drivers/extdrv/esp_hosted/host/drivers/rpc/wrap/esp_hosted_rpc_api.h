/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RT_ESP_HOSTED_RPC_API_H
#define RT_ESP_HOSTED_RPC_API_H

#include "esp_hosted_rpc_schema.h"

#include <rtthread.h>

#define ESP_HOSTED_WIFI_SSID_MAX_LENGTH 32

enum esp_hosted_wifi_interface
{
    ESP_HOSTED_WIFI_IF_STA = 0,
    ESP_HOSTED_WIFI_IF_AP = 1,
};

enum esp_hosted_wifi_mode
{
    ESP_HOSTED_WIFI_MODE_STA = 1,
    ESP_HOSTED_WIFI_MODE_AP = 2,
};

enum esp_hosted_wifi_authmode
{
    ESP_HOSTED_WIFI_AUTH_OPEN = 0,
    ESP_HOSTED_WIFI_AUTH_WEP = 1,
    ESP_HOSTED_WIFI_AUTH_WPA_PSK = 2,
    ESP_HOSTED_WIFI_AUTH_WPA2_PSK = 3,
    ESP_HOSTED_WIFI_AUTH_WPA_WPA2_PSK = 4,
    ESP_HOSTED_WIFI_AUTH_WPA2_ENTERPRISE = 5,
    ESP_HOSTED_WIFI_AUTH_WPA3_PSK = 6,
    ESP_HOSTED_WIFI_AUTH_WPA2_WPA3_PSK = 7,
};

enum esp_hosted_feature_command
{
    ESP_HOSTED_FEATURE_BT_INIT = 1,
    ESP_HOSTED_FEATURE_BT_DEINIT = 2,
    ESP_HOSTED_FEATURE_BT_ENABLE = 3,
    ESP_HOSTED_FEATURE_BT_DISABLE = 4,
};

enum esp_hosted_rpc_api_event_id
{
    ESP_HOSTED_API_EVENT_INIT,
    ESP_HOSTED_API_EVENT_STA_CONNECTED,
    ESP_HOSTED_API_EVENT_STA_DISCONNECTED,
    ESP_HOSTED_API_EVENT_AP_STA_CONNECTED,
    ESP_HOSTED_API_EVENT_AP_STA_DISCONNECTED,
    ESP_HOSTED_API_EVENT_WIFI,
    ESP_HOSTED_API_EVENT_SCAN_DONE,
};

struct esp_hosted_rpc_api_event
{
    enum esp_hosted_rpc_api_event_id id;
    union
    {
        uint32_t reset_reason;
        uint8_t mac[6];
        int wifi_event;
        int scan_count;
    } data;
};

struct esp_hosted_rpc_api_callbacks
{
    void (*event)(const struct esp_hosted_rpc_api_event *event, void *argument);
};

struct esp_hosted_wifi_sta_config
{
    const uint8_t *ssid;
    size_t ssid_length;
    const uint8_t *password;
    size_t password_length;
    const uint8_t *bssid;
    int channel;
};

struct esp_hosted_wifi_ap_config
{
    const uint8_t *ssid;
    size_t ssid_length;
    const uint8_t *password;
    size_t password_length;
    int channel;
    enum esp_hosted_wifi_authmode authmode;
    rt_bool_t hidden;
    int max_connections;
    int beacon_interval;
};

struct esp_hosted_wifi_ap_record
{
    uint8_t bssid[6];
    uint8_t ssid[ESP_HOSTED_WIFI_SSID_MAX_LENGTH + 1];
    size_t ssid_length;
    int channel;
    int rssi;
    enum esp_hosted_wifi_authmode authmode;
};

typedef void (*esp_hosted_wifi_scan_callback_t)(
    const struct esp_hosted_wifi_ap_record *record, void *argument);

void esp_hosted_rpc_api_init(const struct esp_hosted_rpc_api_callbacks *callbacks,
                             void *argument);
void esp_hosted_rpc_api_handle_event(RpcId event_id,
                                     const ProtobufCMessage *message,
                                     void *argument);

rt_err_t esp_hosted_rpc_wifi_init(uint8_t coprocessor_id);
rt_err_t esp_hosted_rpc_wifi_set_mode(uint8_t mode);
rt_err_t esp_hosted_rpc_wifi_start(void);
rt_err_t esp_hosted_rpc_wifi_connect(void);
rt_err_t esp_hosted_rpc_wifi_disconnect(void);
rt_err_t esp_hosted_rpc_wifi_set_sta_config(
    const struct esp_hosted_wifi_sta_config *config);
rt_err_t esp_hosted_rpc_wifi_set_ap_config(
    const struct esp_hosted_wifi_ap_config *config);
rt_err_t esp_hosted_rpc_wifi_ap_deauth(const uint8_t mac[6]);
rt_err_t esp_hosted_rpc_wifi_scan(int maximum_records,
                                  esp_hosted_wifi_scan_callback_t callback,
                                  void *argument);
rt_err_t esp_hosted_rpc_wifi_scan_stop(void);
rt_err_t esp_hosted_rpc_wifi_get_rssi(int *rssi);
rt_err_t esp_hosted_rpc_wifi_set_power_save(int mode);
rt_err_t esp_hosted_rpc_wifi_get_power_save(int *mode);
rt_err_t esp_hosted_rpc_wifi_set_channel(int channel);
rt_err_t esp_hosted_rpc_wifi_get_channel(int *channel);
rt_err_t esp_hosted_rpc_wifi_get_mac(enum esp_hosted_wifi_interface interface,
                                     uint8_t mac[6]);
rt_err_t esp_hosted_rpc_wifi_set_mac(enum esp_hosted_wifi_interface interface,
                                     const uint8_t mac[6]);
rt_err_t esp_hosted_rpc_wifi_set_country_code(const char code[2]);
rt_err_t esp_hosted_rpc_wifi_get_country_code(char code[3]);
rt_err_t esp_hosted_rpc_feature_control(enum esp_hosted_feature_command command);

#endif /* RT_ESP_HOSTED_RPC_API_H */
