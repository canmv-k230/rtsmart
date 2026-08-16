/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Typed ESP-Hosted-MCU services built from the generated RPC schema.
 */
#include "esp_hosted_rpc_api.h"
#include "esp_hosted_rpc_schema.h"
#include "esp_hosted_mcu_log.h"

#define DBG_TAG "esp_hosted.rpc.api"
#define DBG_LVL ESP_HOSTED_MCU_DBG_LVL
#include <rtdbg.h>

#define EH_WIFI_INIT_MAGIC 0x1f2f3f4f

static struct esp_hosted_rpc_api_callbacks g_callbacks;
static void *g_callback_argument;

static rt_err_t eh_call_checked(RpcId request_id,
                                const ProtobufCMessage *request,
                                int timeout_ms,
                                ProtobufCMessage **response)
{
    rt_err_t result;
    int status;

    result = rt_esp_hosted_rpc_call_message(request_id, request, response,
                                            timeout_ms);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_esp_hosted_rpc_response_status(*response, &status);
    if (result != RT_EOK)
    {
        LOG_W("request %u response has no status", request_id);
        rt_esp_hosted_rpc_free_message(*response);
        *response = RT_NULL;
        return -RT_ERROR;
    }
    if (status)
    {
        LOG_W("request %u failed: 0x%x", request_id, status);
        rt_esp_hosted_rpc_free_message(*response);
        *response = RT_NULL;
        return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_err_t eh_call_status(RpcId request_id,
                               const ProtobufCMessage *request,
                               int timeout_ms)
{
    ProtobufCMessage *response = RT_NULL;
    rt_err_t result = eh_call_checked(request_id, request, timeout_ms,
                                      &response);

    if (response)
    {
        rt_esp_hosted_rpc_free_message(response);
    }
    return result;
}

static rt_err_t eh_report_ap_record(
    const WifiApRecord *source, esp_hosted_wifi_scan_callback_t callback,
    void *argument)
{
    struct esp_hosted_wifi_ap_record record;
    size_t ssid_length;

    if (!source || source->bssid.len != sizeof(record.bssid))
    {
        return -RT_ERROR;
    }
    rt_memset(&record, 0, sizeof(record));
    rt_memcpy(record.bssid, source->bssid.data, sizeof(record.bssid));
    ssid_length = source->ssid.len;
    if (ssid_length && source->ssid.data[ssid_length - 1] == '\0')
    {
        ssid_length--;
    }
    record.ssid_length = ssid_length < ESP_HOSTED_WIFI_SSID_MAX_LENGTH ?
                         ssid_length : ESP_HOSTED_WIFI_SSID_MAX_LENGTH;
    if (record.ssid_length)
    {
        rt_memcpy(record.ssid, source->ssid.data, record.ssid_length);
    }
    record.ssid[record.ssid_length] = '\0';
    record.channel = source->primary;
    record.rssi = source->rssi;
    record.authmode = (enum esp_hosted_wifi_authmode)source->authmode;
    if (record.ssid_length && callback)
    {
        callback(&record, argument);
    }
    return RT_EOK;
}

void esp_hosted_rpc_api_init(const struct esp_hosted_rpc_api_callbacks *callbacks,
                             void *argument)
{
    if (callbacks)
    {
        g_callbacks = *callbacks;
    }
    else
    {
        rt_memset(&g_callbacks, 0, sizeof(g_callbacks));
    }
    g_callback_argument = argument;
}

void esp_hosted_rpc_api_handle_event(RpcId event_id,
                                     const ProtobufCMessage *message,
                                     void *argument)
{
    struct esp_hosted_rpc_api_event event;
    const ProtobufCBinaryData *mac = RT_NULL;

    (void)argument;
    if (!message)
    {
        return;
    }

    rt_memset(&event, 0, sizeof(event));
    switch (event_id)
    {
    case RPC_ID__Event_ESPInit:
        event.id = ESP_HOSTED_API_EVENT_INIT;
        event.data.reset_reason =
            ((RpcEventESPInit *)message)->cp_reset_reason;
        break;
    case RPC_ID__Event_StaConnected:
        event.id = ESP_HOSTED_API_EVENT_STA_CONNECTED;
        break;
    case RPC_ID__Event_StaDisconnected:
    {
        RpcEventStaDisconnected *disconnected =
            (RpcEventStaDisconnected *)message;

        event.id = ESP_HOSTED_API_EVENT_STA_DISCONNECTED;
        if (disconnected->sta_disconnected)
        {
            LOG_W("station disconnected: reason=%u rssi=%d",
                  (unsigned int)disconnected->sta_disconnected->reason,
                  (int)disconnected->sta_disconnected->rssi);
        }
        break;
    }
    case RPC_ID__Event_AP_StaConnected:
        event.id = ESP_HOSTED_API_EVENT_AP_STA_CONNECTED;
        mac = &((RpcEventAPStaConnected *)message)->mac;
        break;
    case RPC_ID__Event_AP_StaDisconnected:
        event.id = ESP_HOSTED_API_EVENT_AP_STA_DISCONNECTED;
        mac = &((RpcEventAPStaDisconnected *)message)->mac;
        break;
    case RPC_ID__Event_WifiEventNoArgs:
        event.id = ESP_HOSTED_API_EVENT_WIFI;
        event.data.wifi_event =
            ((RpcEventWifiEventNoArgs *)message)->event_id;
        break;
    case RPC_ID__Event_StaScanDone:
    {
        RpcEventStaScanDone *scan = (RpcEventStaScanDone *)message;

        event.id = ESP_HOSTED_API_EVENT_SCAN_DONE;
        if (scan->scan_done)
        {
            event.data.scan_count = scan->scan_done->number;
        }
        break;
    }
    default:
        return;
    }

    if (mac)
    {
        if (mac->len != sizeof(event.data.mac))
        {
            LOG_W("invalid AP station event %u", event_id);
            return;
        }
        rt_memcpy(event.data.mac, mac->data, sizeof(event.data.mac));
    }
    if (g_callbacks.event)
    {
        g_callbacks.event(&event, g_callback_argument);
    }
}

rt_err_t esp_hosted_rpc_wifi_init(uint8_t coprocessor_id)
{
    WifiInitConfig config = WIFI_INIT_CONFIG__INIT;
    RpcReqWifiInit request = RPC__REQ__WIFI_INIT__INIT;

    config.static_rx_buf_num = 10;
    config.dynamic_rx_buf_num = 32;
    config.tx_buf_type = 1;
    config.dynamic_tx_buf_num = 32;
    config.ampdu_rx_enable = 1;
    config.ampdu_tx_enable = 1;
    config.nvs_enable = 1;
    config.rx_ba_win = 6;
    config.beacon_max_len = 752;
    config.mgmt_sbuf_num = 32;
    config.sta_disconnected_pm = 1;
    config.espnow_max_encrypt_num = 7;
    config.magic = EH_WIFI_INIT_MAGIC;
    config.rx_mgmt_buf_type = 1;
    config.rx_mgmt_buf_num = 5;
    config.tx_hetb_queue_num = 1;

    switch (coprocessor_id)
    {
    case 0x02:
        config.static_rx_buf_num = 8;
        config.dynamic_rx_buf_num = 24;
        config.dynamic_tx_buf_num = 24;
        config.rx_ba_win = 16;
        break;
    case 0x05:
        config.rx_ba_win = 16;
        break;
    case 0x0c:
        config.static_rx_buf_num = 8;
        config.dynamic_rx_buf_num = 16;
        config.dynamic_tx_buf_num = 16;
        config.rx_ba_win = 8;
        break;
    case 0x17:
        config.dynamic_rx_buf_num = 64;
        config.rx_ba_win = 16;
        break;
    default:
        break;
    }

    request.cfg = &config;
    return eh_call_status(RPC_ID__Req_WifiInit, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_set_mode(uint8_t mode)
{
    RpcReqSetMode request = RPC__REQ__SET_MODE__INIT;

    if (mode > (ESP_HOSTED_WIFI_MODE_STA | ESP_HOSTED_WIFI_MODE_AP))
    {
        return -RT_EINVAL;
    }
    request.mode = mode;
    return eh_call_status(RPC_ID__Req_SetWifiMode, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_start(void)
{
    return eh_call_status(RPC_ID__Req_WifiStart, RT_NULL,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_connect(void)
{
    return eh_call_status(RPC_ID__Req_WifiConnect, RT_NULL,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_disconnect(void)
{
    return eh_call_status(RPC_ID__Req_WifiDisconnect, RT_NULL,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_set_sta_config(
    const struct esp_hosted_wifi_sta_config *config)
{
    static const uint8_t zero_bssid[6];
    WifiStaConfig station = WIFI_STA_CONFIG__INIT;
    WifiConfig wifi = WIFI_CONFIG__INIT;
    RpcReqWifiSetConfig request = RPC__REQ__WIFI_SET_CONFIG__INIT;

    if (!config || !config->ssid ||
        config->ssid_length > ESP_HOSTED_WIFI_SSID_MAX_LENGTH ||
        (config->password_length && !config->password))
    {
        return -RT_EINVAL;
    }
    station.ssid.data = (uint8_t *)config->ssid;
    station.ssid.len = config->ssid_length;
    station.password.data = (uint8_t *)config->password;
    station.password.len = config->password_length;
    if (config->bssid && rt_memcmp(config->bssid, zero_bssid, 6) != 0)
    {
        station.bssid_set = 1;
        station.bssid.data = (uint8_t *)config->bssid;
        station.bssid.len = 6;
    }
    station.channel = config->channel;
    wifi.u_case = WIFI_CONFIG__U_STA;
    wifi.sta = &station;
    request.iface = ESP_HOSTED_WIFI_IF_STA;
    request.cfg = &wifi;
    return eh_call_status(RPC_ID__Req_WifiSetConfig, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_set_ap_config(
    const struct esp_hosted_wifi_ap_config *config)
{
    WifiApConfig access_point = WIFI_AP_CONFIG__INIT;
    WifiConfig wifi = WIFI_CONFIG__INIT;
    RpcReqWifiSetConfig request = RPC__REQ__WIFI_SET_CONFIG__INIT;

    if (!config || !config->ssid ||
        config->ssid_length > ESP_HOSTED_WIFI_SSID_MAX_LENGTH ||
        (config->password_length && !config->password))
    {
        return -RT_EINVAL;
    }
    access_point.ssid.data = (uint8_t *)config->ssid;
    access_point.ssid.len = config->ssid_length;
    access_point.password.data = (uint8_t *)config->password;
    access_point.password.len = config->password_length;
    access_point.ssid_len = config->ssid_length;
    access_point.channel = config->channel ? config->channel : 1;
    access_point.authmode = config->authmode;
    access_point.ssid_hidden = config->hidden ? 1 : 0;
    access_point.max_connection = config->max_connections;
    access_point.beacon_interval = config->beacon_interval;
    wifi.u_case = WIFI_CONFIG__U_AP;
    wifi.ap = &access_point;
    request.iface = ESP_HOSTED_WIFI_IF_AP;
    request.cfg = &wifi;
    return eh_call_status(RPC_ID__Req_WifiSetConfig, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_ap_deauth(const uint8_t mac[6])
{
    RpcReqWifiApGetStaAid aid_request =
        RPC__REQ__WIFI_AP_GET_STA_AID__INIT;
    RpcReqWifiDeauthSta deauth_request = RPC__REQ__WIFI_DEAUTH_STA__INIT;
    RpcRespWifiApGetStaAid *aid_response;
    ProtobufCMessage *message;
    rt_err_t result;

    if (!mac)
    {
        return -RT_EINVAL;
    }
    aid_request.mac.data = (uint8_t *)mac;
    aid_request.mac.len = 6;
    result = eh_call_checked(RPC_ID__Req_WifiApGetStaAid,
                             &aid_request.base, ESP_HOSTED_RPC_TIMEOUT_MS,
                             &message);
    if (result != RT_EOK)
    {
        return result;
    }
    aid_response = (RpcRespWifiApGetStaAid *)message;
    if (!aid_response->aid || aid_response->aid > UINT16_MAX)
    {
        rt_esp_hosted_rpc_free_message(message);
        return -RT_ERROR;
    }
    deauth_request.aid = aid_response->aid;
    rt_esp_hosted_rpc_free_message(message);
    return eh_call_status(RPC_ID__Req_WifiDeauthSta,
                          &deauth_request.base, ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_scan(int maximum_records,
                                  esp_hosted_wifi_scan_callback_t callback,
                                  void *argument)
{
    RpcReqWifiScanStart start_request = RPC__REQ__WIFI_SCAN_START__INIT;
    RpcReqWifiScanGetApRecords records_request =
        RPC__REQ__WIFI_SCAN_GET_AP_RECORDS__INIT;
    RpcRespWifiScanGetApNum *count_response;
    RpcRespWifiScanGetApRecords *records_response;
    ProtobufCMessage *message;
    size_t count;
    size_t index;
    rt_err_t result;

    if (maximum_records < 0)
    {
        return -RT_EINVAL;
    }
    start_request.block = 1;
    result = eh_call_status(RPC_ID__Req_WifiScanStart,
                            &start_request.base, 30000);
    if (result != RT_EOK)
    {
        return result;
    }
    result = eh_call_checked(RPC_ID__Req_WifiScanGetApNum, RT_NULL,
                             ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result != RT_EOK)
    {
        return result;
    }
    count_response = (RpcRespWifiScanGetApNum *)message;
    count = count_response->number > 0 ? (size_t)count_response->number : 0;
    rt_esp_hosted_rpc_free_message(message);
    if (count > (size_t)maximum_records)
    {
        count = maximum_records;
    }
    if (!count)
    {
        return RT_EOK;
    }

    records_request.number = count;
    result = eh_call_checked(RPC_ID__Req_WifiScanGetApRecords,
                             &records_request.base, ESP_HOSTED_RPC_TIMEOUT_MS,
                             &message);
    if (result != RT_EOK)
    {
        return result;
    }
    records_response = (RpcRespWifiScanGetApRecords *)message;
    if (records_response->n_ap_records < count)
    {
        count = records_response->n_ap_records;
    }
    for (index = 0; index < count; index++)
    {
        result = eh_report_ap_record(records_response->ap_records[index],
                                     callback, argument);
        if (result != RT_EOK)
        {
            break;
        }
    }
    rt_esp_hosted_rpc_free_message(message);
    return result;
}

rt_err_t esp_hosted_rpc_wifi_scan_stop(void)
{
    return eh_call_status(RPC_ID__Req_WifiScanStop, RT_NULL,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_get_rssi(int *rssi)
{
    RpcRespWifiStaGetRssi *typed_response;
    ProtobufCMessage *response;
    rt_err_t result;

    if (!rssi)
    {
        return -RT_EINVAL;
    }
    result = eh_call_checked(RPC_ID__Req_WifiStaGetRssi, RT_NULL,
                             ESP_HOSTED_RPC_TIMEOUT_MS, &response);
    if (result == RT_EOK)
    {
        typed_response = (RpcRespWifiStaGetRssi *)response;
        *rssi = typed_response->rssi;
        rt_esp_hosted_rpc_free_message(response);
    }
    return result;
}

rt_err_t esp_hosted_rpc_wifi_set_power_save(int mode)
{
    RpcReqSetPs request = RPC__REQ__SET_PS__INIT;

    if (mode < 0 || mode > 2)
    {
        return -RT_EINVAL;
    }
    request.type = mode;
    return eh_call_status(RPC_ID__Req_WifiSetPs, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_get_power_save(int *mode)
{
    RpcRespGetPs *typed_response;
    ProtobufCMessage *response;
    rt_err_t result;

    if (!mode)
    {
        return -RT_EINVAL;
    }
    result = eh_call_checked(RPC_ID__Req_WifiGetPs, RT_NULL,
                             ESP_HOSTED_RPC_TIMEOUT_MS, &response);
    if (result == RT_EOK)
    {
        typed_response = (RpcRespGetPs *)response;
        *mode = typed_response->type;
        rt_esp_hosted_rpc_free_message(response);
    }
    return result;
}

rt_err_t esp_hosted_rpc_wifi_set_max_tx_power(int power)
{
    RpcReqWifiSetMaxTxPower request =
        RPC__REQ__WIFI_SET_MAX_TX_POWER__INIT;

    if (power < 8 || power > 84)
    {
        return -RT_EINVAL;
    }
    request.power = power;
    return eh_call_status(RPC_ID__Req_WifiSetMaxTxPower, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_get_max_tx_power(int *power)
{
    RpcRespWifiGetMaxTxPower *typed_response;
    ProtobufCMessage *response;
    rt_err_t result;

    if (!power)
    {
        return -RT_EINVAL;
    }
    result = eh_call_checked(RPC_ID__Req_WifiGetMaxTxPower, RT_NULL,
                             ESP_HOSTED_RPC_TIMEOUT_MS, &response);
    if (result == RT_EOK)
    {
        typed_response = (RpcRespWifiGetMaxTxPower *)response;
        *power = typed_response->power;
        rt_esp_hosted_rpc_free_message(response);
    }
    return result;
}

rt_err_t esp_hosted_rpc_wifi_set_channel(int channel)
{
    RpcReqWifiSetChannel request = RPC__REQ__WIFI_SET_CHANNEL__INIT;

    if (channel < 1 || channel > 196)
    {
        return -RT_EINVAL;
    }
    request.primary = channel;
    return eh_call_status(RPC_ID__Req_WifiSetChannel, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_get_channel(int *channel)
{
    RpcRespWifiGetChannel *typed_response;
    ProtobufCMessage *response;
    rt_err_t result;

    if (!channel)
    {
        return -RT_EINVAL;
    }
    result = eh_call_checked(RPC_ID__Req_WifiGetChannel, RT_NULL,
                             ESP_HOSTED_RPC_TIMEOUT_MS, &response);
    if (result == RT_EOK)
    {
        typed_response = (RpcRespWifiGetChannel *)response;
        *channel = typed_response->primary;
        rt_esp_hosted_rpc_free_message(response);
    }
    return result;
}

rt_err_t esp_hosted_rpc_wifi_get_mac(enum esp_hosted_wifi_interface interface,
                                     uint8_t mac[6])
{
    RpcReqGetMacAddress request = RPC__REQ__GET_MAC_ADDRESS__INIT;
    RpcRespGetMacAddress *typed_response;
    ProtobufCMessage *response;
    rt_err_t result;

    if ((interface != ESP_HOSTED_WIFI_IF_STA &&
         interface != ESP_HOSTED_WIFI_IF_AP) || !mac)
    {
        return -RT_EINVAL;
    }
    request.mode = interface;
    result = eh_call_checked(RPC_ID__Req_GetMACAddress, &request.base,
                             ESP_HOSTED_RPC_TIMEOUT_MS, &response);
    if (result != RT_EOK)
    {
        return result;
    }
    typed_response = (RpcRespGetMacAddress *)response;
    if (typed_response->mac.len != 6)
    {
        rt_esp_hosted_rpc_free_message(response);
        return -RT_ERROR;
    }
    rt_memcpy(mac, typed_response->mac.data, 6);
    rt_esp_hosted_rpc_free_message(response);
    return RT_EOK;
}

rt_err_t esp_hosted_rpc_wifi_set_mac(enum esp_hosted_wifi_interface interface,
                                     const uint8_t mac[6])
{
    RpcReqSetMacAddress request = RPC__REQ__SET_MAC_ADDRESS__INIT;

    if ((interface != ESP_HOSTED_WIFI_IF_STA &&
         interface != ESP_HOSTED_WIFI_IF_AP) || !mac)
    {
        return -RT_EINVAL;
    }
    request.mac.data = (uint8_t *)mac;
    request.mac.len = 6;
    request.mode = interface;
    return eh_call_status(RPC_ID__Req_SetMacAddress, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_set_country_code(const char code[2])
{
    RpcReqWifiSetCountryCode request =
        RPC__REQ__WIFI_SET_COUNTRY_CODE__INIT;

    if (!code)
    {
        return -RT_EINVAL;
    }
    request.country.data = (uint8_t *)code;
    request.country.len = 2;
    return eh_call_status(RPC_ID__Req_WifiSetCountryCode,
                          &request.base, ESP_HOSTED_RPC_TIMEOUT_MS);
}

rt_err_t esp_hosted_rpc_wifi_get_country_code(char code[3])
{
    RpcRespWifiGetCountryCode *typed_response;
    ProtobufCMessage *response;
    rt_err_t result;

    if (!code)
    {
        return -RT_EINVAL;
    }
    result = eh_call_checked(RPC_ID__Req_WifiGetCountryCode, RT_NULL,
                             ESP_HOSTED_RPC_TIMEOUT_MS, &response);
    if (result != RT_EOK)
    {
        return result;
    }
    typed_response = (RpcRespWifiGetCountryCode *)response;
    if (typed_response->country.len < 2)
    {
        rt_esp_hosted_rpc_free_message(response);
        return -RT_ERROR;
    }
    rt_memcpy(code, typed_response->country.data, 2);
    code[2] = '\0';
    rt_esp_hosted_rpc_free_message(response);
    return RT_EOK;
}

rt_err_t esp_hosted_rpc_feature_control(enum esp_hosted_feature_command command)
{
    RpcReqFeatureControl request = RPC__REQ__FEATURE_CONTROL__INIT;

    if (command < ESP_HOSTED_FEATURE_BT_INIT ||
        command > ESP_HOSTED_FEATURE_BT_DISABLE)
    {
        return -RT_EINVAL;
    }
    request.feature = RPC_FEATURE__Feature_Bluetooth;
    request.command = (RpcFeatureCommand)command;
    return eh_call_status(RPC_ID__Req_FeatureControl, &request.base,
                          ESP_HOSTED_RPC_TIMEOUT_MS);
}
