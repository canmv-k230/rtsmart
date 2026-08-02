/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RT-Smart port of the ESP-Hosted-MCU RPC wrapper.  The public wrapper keeps
 * the upstream function split while the transport boundary uses generated
 * protobuf-c messages directly.
 */
#include "rpc_wrap.h"

#include "esp_hosted_rpc_schema.h"

#include <limits.h>
#include <string.h>

#define DBG_TAG "esp.rpc.wrap"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define RPC_SCAN_TIMEOUT_MS 30000
#define RPC_OTA_TIMEOUT_MS  15000

enum rpc_response_cache_id
{
    RPC_CACHE_WIFI_CONFIG,
    RPC_CACHE_SCAN_PARAMS,
    RPC_CACHE_SCAN_RECORD,
    RPC_CACHE_SCAN_RECORDS,
    RPC_CACHE_AP_INFO,
    RPC_CACHE_COUNTRY,
    RPC_CACHE_STA_LIST,
    RPC_CACHE_COUNT,
};

static ProtobufCMessage *g_response_cache[RPC_CACHE_COUNT];

static esp_err_t rpc_call_checked(RpcId request_id,
                                  const ProtobufCMessage *request,
                                  int timeout_ms,
                                  ProtobufCMessage **response)
{
    esp_err_t result;
    int remote_status;

    if (!response)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *response = RT_NULL;
    result = rt_esp_hosted_rpc_call_message(request_id, request, response,
                                            timeout_ms);
    if (result != ESP_OK)
    {
        return result;
    }
    result = rt_esp_hosted_rpc_response_status(*response, &remote_status);
    if (result != ESP_OK)
    {
        rt_esp_hosted_rpc_free_message(*response);
        *response = RT_NULL;
        return result;
    }
    if (remote_status != ESP_OK)
    {
        rt_esp_hosted_rpc_free_message(*response);
        *response = RT_NULL;
        return remote_status;
    }
    return ESP_OK;
}

static esp_err_t rpc_call_status(RpcId request_id,
                                 const ProtobufCMessage *request,
                                 int timeout_ms)
{
    ProtobufCMessage *response = RT_NULL;
    esp_err_t result = rpc_call_checked(request_id, request, timeout_ms,
                                        &response);

    if (response)
    {
        rt_esp_hosted_rpc_free_message(response);
    }
    return result;
}

static void rpc_cache_response(enum rpc_response_cache_id id,
                               ProtobufCMessage *response)
{
    if (g_response_cache[id])
    {
        rt_esp_hosted_rpc_free_message(g_response_cache[id]);
    }
    g_response_cache[id] = response;
}

static void rpc_copy_binary(void *destination, size_t destination_size,
                            const ProtobufCBinaryData *source)
{
    size_t length;

    if (!destination || !destination_size)
    {
        return;
    }
    rt_memset(destination, 0, destination_size);
    if (!source || !source->data)
    {
        return;
    }
    length = source->len < destination_size ? source->len : destination_size;
    rt_memcpy(destination, source->data, length);
}

esp_err_t rpc_wifi_init(const wifi_init_config_t *arg)
{
    RpcReqWifiInit request = RPC__REQ__WIFI_INIT__INIT;

    if (!arg)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.cfg = (WifiInitConfig *)arg;
    return rpc_call_status(RPC_ID__Req_WifiInit, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_deinit(void)
{
    return rpc_call_status(RPC_ID__Req_WifiDeinit, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_set_mode(wifi_mode_t mode)
{
    RpcReqSetMode request = RPC__REQ__SET_MODE__INIT;

    request.mode = mode;
    return rpc_call_status(RPC_ID__Req_SetWifiMode, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_mode(wifi_mode_t *mode)
{
    ProtobufCMessage *message;
    RpcRespGetMode *response;
    esp_err_t result;

    if (!mode)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_GetWifiMode, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespGetMode *)message;
        *mode = response->mode;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_start(void)
{
    return rpc_call_status(RPC_ID__Req_WifiStart, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_stop(void)
{
    return rpc_call_status(RPC_ID__Req_WifiStop, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_connect(void)
{
    return rpc_call_status(RPC_ID__Req_WifiConnect, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_disconnect(void)
{
    return rpc_call_status(RPC_ID__Req_WifiDisconnect, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf)
{
    RpcReqWifiSetConfig request = RPC__REQ__WIFI_SET_CONFIG__INIT;

    if (!conf)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.iface = interface;
    request.cfg = conf;
    return rpc_call_status(RPC_ID__Req_WifiSetConfig, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_config(wifi_interface_t interface, wifi_config_t *conf)
{
    RpcReqWifiGetConfig request = RPC__REQ__WIFI_GET_CONFIG__INIT;
    ProtobufCMessage *message;
    RpcRespWifiGetConfig *response;
    esp_err_t result;

    if (!conf)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.iface = interface;
    result = rpc_call_checked(RPC_ID__Req_WifiGetConfig, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiGetConfig *)message;
        if (!response->cfg)
        {
            rt_esp_hosted_rpc_free_message(message);
            return ESP_FAIL;
        }
        *conf = *response->cfg;
        rpc_cache_response(RPC_CACHE_WIFI_CONFIG, message);
    }
    return result;
}

esp_err_t rpc_wifi_get_mac(wifi_interface_t mode, uint8_t mac[6])
{
    RpcReqGetMacAddress request = RPC__REQ__GET_MAC_ADDRESS__INIT;
    ProtobufCMessage *message;
    RpcRespGetMacAddress *response;
    esp_err_t result;

    if (!mac)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.mode = mode;
    result = rpc_call_checked(RPC_ID__Req_GetMACAddress, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespGetMacAddress *)message;
        if (response->mac.len != 6 || !response->mac.data)
        {
            result = ESP_FAIL;
        }
        else
        {
            rt_memcpy(mac, response->mac.data, 6);
        }
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_set_mac(wifi_interface_t mode, const uint8_t mac[6])
{
    RpcReqSetMacAddress request = RPC__REQ__SET_MAC_ADDRESS__INIT;

    if (!mac)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.mode = mode;
    request.mac.data = (uint8_t *)mac;
    request.mac.len = 6;
    return rpc_call_status(RPC_ID__Req_SetMacAddress, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_set_scan_parameters(
    const wifi_scan_default_params_t *config)
{
    RpcReqWifiScanParams request = RPC__REQ__WIFI_SCAN_PARAMS__INIT;

    request.cmd = RPC_CMD__Set;
    request.config = (WifiScanDefaultParams *)config;
    request.is_config_null = config == RT_NULL;
    return rpc_call_status(RPC_ID__Req_WifiScanParams, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_scan_parameters(wifi_scan_default_params_t *config)
{
    RpcReqWifiScanParams request = RPC__REQ__WIFI_SCAN_PARAMS__INIT;
    ProtobufCMessage *message;
    RpcRespWifiScanParams *response;
    esp_err_t result;

    if (!config)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.cmd = RPC_CMD__Get;
    request.is_config_null = 1;
    result = rpc_call_checked(RPC_ID__Req_WifiScanParams, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiScanParams *)message;
        if (!response->config)
        {
            rt_esp_hosted_rpc_free_message(message);
            return ESP_FAIL;
        }
        *config = *response->config;
        rpc_cache_response(RPC_CACHE_SCAN_PARAMS, message);
    }
    return result;
}

esp_err_t rpc_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    RpcReqWifiScanStart request = RPC__REQ__WIFI_SCAN_START__INIT;

    request.config = (WifiScanConfig *)config;
    request.config_set = config != RT_NULL;
    request.block = block;
    return rpc_call_status(RPC_ID__Req_WifiScanStart, &request.base,
                           block ? RPC_SCAN_TIMEOUT_MS
                                 : ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_scan_stop(void)
{
    return rpc_call_status(RPC_ID__Req_WifiScanStop, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_scan_get_ap_num(uint16_t *number)
{
    ProtobufCMessage *message;
    RpcRespWifiScanGetApNum *response;
    esp_err_t result;

    if (!number)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiScanGetApNum, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiScanGetApNum *)message;
        if (response->number < 0 || response->number > UINT16_MAX)
        {
            result = ESP_FAIL;
        }
        else
        {
            *number = response->number;
        }
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_scan_get_ap_record(wifi_ap_record_t *ap_record)
{
    ProtobufCMessage *message;
    RpcRespWifiScanGetApRecord *response;
    esp_err_t result;

    if (!ap_record)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiScanGetApRecord, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiScanGetApRecord *)message;
        if (!response->ap_record)
        {
            rt_esp_hosted_rpc_free_message(message);
            return ESP_FAIL;
        }
        *ap_record = *response->ap_record;
        rpc_cache_response(RPC_CACHE_SCAN_RECORD, message);
    }
    return result;
}

esp_err_t rpc_wifi_scan_get_ap_records(uint16_t *number,
                                       wifi_ap_record_t *ap_records)
{
    RpcReqWifiScanGetApRecords request =
        RPC__REQ__WIFI_SCAN_GET_AP_RECORDS__INIT;
    ProtobufCMessage *message;
    RpcRespWifiScanGetApRecords *response;
    size_t capacity;
    size_t count;
    size_t index;
    esp_err_t result;

    if (!number || !*number || !ap_records)
    {
        return ESP_ERR_INVALID_ARG;
    }
    capacity = *number;
    request.number = capacity;
    result = rpc_call_checked(RPC_ID__Req_WifiScanGetApRecords, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiScanGetApRecords *)message;
        if (response->n_ap_records && !response->ap_records)
        {
            rt_esp_hosted_rpc_free_message(message);
            return ESP_FAIL;
        }
        count = response->n_ap_records;
        if (response->number >= 0 && count > (size_t)response->number)
        {
            count = response->number;
        }
        if (count > capacity)
        {
            count = capacity;
        }
        for (index = 0; index < count; index++)
        {
            if (!response->ap_records[index])
            {
                break;
            }
            ap_records[index] = *response->ap_records[index];
        }
        *number = index;
        rpc_cache_response(RPC_CACHE_SCAN_RECORDS, message);
    }
    return result;
}

esp_err_t rpc_wifi_clear_ap_list(void)
{
    return rpc_call_status(RPC_ID__Req_WifiClearApList, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_restore(void)
{
    return rpc_call_status(RPC_ID__Req_WifiRestore, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_clear_fast_connect(void)
{
    return rpc_call_status(RPC_ID__Req_WifiClearFastConnect, RT_NULL,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_deauth_sta(uint16_t aid)
{
    RpcReqWifiDeauthSta request = RPC__REQ__WIFI_DEAUTH_STA__INIT;

    request.aid = aid;
    return rpc_call_status(RPC_ID__Req_WifiDeauthSta, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
    ProtobufCMessage *message;
    RpcRespWifiStaGetApInfo *response;
    esp_err_t result;

    if (!ap_info)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiStaGetApInfo, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiStaGetApInfo *)message;
        if (!response->ap_record)
        {
            rt_esp_hosted_rpc_free_message(message);
            return ESP_FAIL;
        }
        *ap_info = *response->ap_record;
        rpc_cache_response(RPC_CACHE_AP_INFO, message);
    }
    return result;
}

esp_err_t rpc_wifi_set_ps(wifi_ps_type_t type)
{
    RpcReqSetPs request = RPC__REQ__SET_PS__INIT;

    request.type = type;
    return rpc_call_status(RPC_ID__Req_WifiSetPs, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_ps(wifi_ps_type_t *type)
{
    ProtobufCMessage *message;
    RpcRespGetPs *response;
    esp_err_t result;

    if (!type)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiGetPs, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespGetPs *)message;
        *type = response->type;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_set_storage(wifi_storage_t storage)
{
    RpcReqWifiSetStorage request = RPC__REQ__WIFI_SET_STORAGE__INIT;

    request.storage = storage;
    return rpc_call_status(RPC_ID__Req_WifiSetStorage, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_set_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t bw)
{
    RpcReqWifiSetBandwidth request = RPC__REQ__WIFI_SET_BANDWIDTH__INIT;

    request.ifx = ifx;
    request.bw = bw;
    return rpc_call_status(RPC_ID__Req_WifiSetBandwidth, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t *bw)
{
    RpcReqWifiGetBandwidth request = RPC__REQ__WIFI_GET_BANDWIDTH__INIT;
    ProtobufCMessage *message;
    RpcRespWifiGetBandwidth *response;
    esp_err_t result;

    if (!bw)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.ifx = ifx;
    result = rpc_call_checked(RPC_ID__Req_WifiGetBandwidth, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiGetBandwidth *)message;
        *bw = response->bw;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_set_channel(uint8_t primary, wifi_second_chan_t second)
{
    RpcReqWifiSetChannel request = RPC__REQ__WIFI_SET_CHANNEL__INIT;

    request.primary = primary;
    request.second = second;
    return rpc_call_status(RPC_ID__Req_WifiSetChannel, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second)
{
    ProtobufCMessage *message;
    RpcRespWifiGetChannel *response;
    esp_err_t result;

    if (!primary || !second)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiGetChannel, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiGetChannel *)message;
        *primary = response->primary;
        *second = response->second;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_set_country_code(const char *country,
                                    bool ieee80211d_enabled)
{
    RpcReqWifiSetCountryCode request =
        RPC__REQ__WIFI_SET_COUNTRY_CODE__INIT;

    if (!country)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.country.data = (uint8_t *)country;
    request.country.len = 3;
    request.ieee80211d_enabled = ieee80211d_enabled;
    return rpc_call_status(RPC_ID__Req_WifiSetCountryCode, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_country_code(char *country)
{
    ProtobufCMessage *message;
    RpcRespWifiGetCountryCode *response;
    esp_err_t result;

    if (!country)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiGetCountryCode, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiGetCountryCode *)message;
        rpc_copy_binary(country, 3, &response->country);
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_set_country(const wifi_country_t *country)
{
    RpcReqWifiSetCountry request = RPC__REQ__WIFI_SET_COUNTRY__INIT;

    if (!country)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.country = (WifiCountry *)country;
    return rpc_call_status(RPC_ID__Req_WifiSetCountry, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_country(wifi_country_t *country)
{
    ProtobufCMessage *message;
    RpcRespWifiGetCountry *response;
    esp_err_t result;

    if (!country)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiGetCountry, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiGetCountry *)message;
        if (!response->country)
        {
            rt_esp_hosted_rpc_free_message(message);
            return ESP_FAIL;
        }
        *country = *response->country;
        rpc_cache_response(RPC_CACHE_COUNTRY, message);
    }
    return result;
}

esp_err_t rpc_wifi_ap_get_sta_list(wifi_sta_list_t *sta)
{
    ProtobufCMessage *message;
    RpcRespWifiApGetStaList *response;
    esp_err_t result;

    if (!sta)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiApGetStaList, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiApGetStaList *)message;
        if (!response->sta_list)
        {
            rt_esp_hosted_rpc_free_message(message);
            return ESP_FAIL;
        }
        *sta = *response->sta_list;
        rpc_cache_response(RPC_CACHE_STA_LIST, message);
    }
    return result;
}

esp_err_t rpc_wifi_ap_get_sta_aid(const uint8_t mac[6], uint16_t *aid)
{
    RpcReqWifiApGetStaAid request = RPC__REQ__WIFI_AP_GET_STA_AID__INIT;
    ProtobufCMessage *message;
    RpcRespWifiApGetStaAid *response;
    esp_err_t result;

    if (!mac || !aid)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.mac.data = (uint8_t *)mac;
    request.mac.len = 6;
    result = rpc_call_checked(RPC_ID__Req_WifiApGetStaAid, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiApGetStaAid *)message;
        if (response->aid > UINT16_MAX)
        {
            result = ESP_FAIL;
        }
        else
        {
            *aid = response->aid;
        }
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_sta_get_rssi(int *rssi)
{
    ProtobufCMessage *message;
    RpcRespWifiStaGetRssi *response;
    esp_err_t result;

    if (!rssi)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiStaGetRssi, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiStaGetRssi *)message;
        *rssi = response->rssi;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_set_protocol(wifi_interface_t ifx,
                                uint8_t protocol_bitmap)
{
    RpcReqWifiSetProtocol request = RPC__REQ__WIFI_SET_PROTOCOL__INIT;

    request.ifx = ifx;
    request.protocol_bitmap = protocol_bitmap;
    return rpc_call_status(RPC_ID__Req_WifiSetProtocol, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_protocol(wifi_interface_t ifx,
                                uint8_t *protocol_bitmap)
{
    RpcReqWifiGetProtocol request = RPC__REQ__WIFI_GET_PROTOCOL__INIT;
    ProtobufCMessage *message;
    RpcRespWifiGetProtocol *response;
    esp_err_t result;

    if (!protocol_bitmap)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.ifx = ifx;
    result = rpc_call_checked(RPC_ID__Req_WifiGetProtocol, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiGetProtocol *)message;
        *protocol_bitmap = response->protocol_bitmap;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_set_max_tx_power(int8_t power)
{
    RpcReqWifiSetMaxTxPower request =
        RPC__REQ__WIFI_SET_MAX_TX_POWER__INIT;

    request.power = power;
    return rpc_call_status(RPC_ID__Req_WifiSetMaxTxPower, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_max_tx_power(int8_t *power)
{
    ProtobufCMessage *message;
    RpcRespWifiGetMaxTxPower *response;
    esp_err_t result;

    if (!power)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiGetMaxTxPower, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiGetMaxTxPower *)message;
        *power = response->power;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_sta_get_negotiated_phymode(wifi_phy_mode_t *phymode)
{
    ProtobufCMessage *message;
    RpcRespWifiStaGetNegotiatedPhymode *response;
    esp_err_t result;

    if (!phymode)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiStaGetNegotiatedPhymode,
                              RT_NULL, ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiStaGetNegotiatedPhymode *)message;
        *phymode = response->phymode;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_sta_get_aid(uint16_t *aid)
{
    ProtobufCMessage *message;
    RpcRespWifiStaGetAid *response;
    esp_err_t result;

    if (!aid)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_WifiStaGetAid, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiStaGetAid *)message;
        if (response->aid > UINT16_MAX)
        {
            result = ESP_FAIL;
        }
        else
        {
            *aid = response->aid;
        }
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_set_inactive_time(wifi_interface_t ifx, uint16_t sec)
{
    RpcReqWifiSetInactiveTime request =
        RPC__REQ__WIFI_SET_INACTIVE_TIME__INIT;

    request.ifx = ifx;
    request.sec = sec;
    return rpc_call_status(RPC_ID__Req_WifiSetInactiveTime, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_wifi_get_inactive_time(wifi_interface_t ifx, uint16_t *sec)
{
    RpcReqWifiGetInactiveTime request =
        RPC__REQ__WIFI_GET_INACTIVE_TIME__INIT;
    ProtobufCMessage *message;
    RpcRespWifiGetInactiveTime *response;
    esp_err_t result;

    if (!sec)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.ifx = ifx;
    result = rpc_call_checked(RPC_ID__Req_WifiGetInactiveTime, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespWifiGetInactiveTime *)message;
        if (response->sec > UINT16_MAX)
        {
            result = ESP_FAIL;
        }
        else
        {
            *sec = response->sec;
        }
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_wifi_disable_pmf_config(wifi_interface_t ifx)
{
    RpcReqWifiDisablePmfConfig request =
        RPC__REQ__WIFI_DISABLE_PMF_CONFIG__INIT;

    request.ifx = ifx;
    return rpc_call_status(RPC_ID__Req_WifiDisablePmfConfig, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

static esp_err_t rpc_get_fw_response(
    RpcRespGetCoprocessorFwVersion **fw_response)
{
    ProtobufCMessage *message;
    esp_err_t result;

    result = rpc_call_checked(RPC_ID__Req_GetCoprocessorFwVersion, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        *fw_response = (RpcRespGetCoprocessorFwVersion *)message;
    }
    return result;
}

esp_err_t rpc_get_coprocessor_fwversion(
    esp_hosted_coprocessor_fwver_t *ver_info)
{
    RpcRespGetCoprocessorFwVersion *response;
    esp_err_t result;

    if (!ver_info)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_get_fw_response(&response);
    if (result == ESP_OK)
    {
        ver_info->major1 = response->major1;
        ver_info->minor1 = response->minor1;
        ver_info->patch1 = response->patch1;
        ver_info->revision = response->revision;
        ver_info->prerelease = response->prerelease;
        ver_info->build = response->build;
        rt_esp_hosted_rpc_free_message(&response->base);
    }
    return result;
}

esp_err_t rpc_get_cp_info(uint32_t *cp_chip_id, char *cp_target_name,
                          size_t cp_target_name_len)
{
    RpcRespGetCoprocessorFwVersion *response;
    size_t length;
    esp_err_t result;

    if (!cp_chip_id || !cp_target_name || !cp_target_name_len)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_get_fw_response(&response);
    if (result == ESP_OK)
    {
        *cp_chip_id = response->chip_id;
        length = response->idf_target.len;
        if (length && !response->idf_target.data)
        {
            rt_esp_hosted_rpc_free_message(&response->base);
            return ESP_FAIL;
        }
        if (length && response->idf_target.data[length - 1] == '\0')
        {
            length--;
        }
        if (length >= cp_target_name_len)
        {
            length = cp_target_name_len - 1;
        }
        if (length)
        {
            rt_memcpy(cp_target_name, response->idf_target.data, length);
        }
        cp_target_name[length] = '\0';
        rt_esp_hosted_rpc_free_message(&response->base);
    }
    return result;
}

static esp_err_t rpc_feature_control(RpcFeatureCommand command,
                                     RpcFeatureOption option)
{
    RpcReqFeatureControl request = RPC__REQ__FEATURE_CONTROL__INIT;

    request.feature = RPC_FEATURE__Feature_Bluetooth;
    request.command = command;
    request.option = option;
    return rpc_call_status(RPC_ID__Req_FeatureControl, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_bt_controller_init(void)
{
    return rpc_feature_control(RPC_FEATURE_COMMAND__Feature_Command_BT_Init,
                               RPC_FEATURE_OPTION__Feature_Option_None);
}

esp_err_t rpc_bt_controller_deinit(bool mem_release)
{
    return rpc_feature_control(
        RPC_FEATURE_COMMAND__Feature_Command_BT_Deinit,
        mem_release
            ? RPC_FEATURE_OPTION__Feature_Option_BT_Deinit_Release_Memory
            : RPC_FEATURE_OPTION__Feature_Option_None);
}

esp_err_t rpc_bt_controller_enable(void)
{
    return rpc_feature_control(RPC_FEATURE_COMMAND__Feature_Command_BT_Enable,
                               RPC_FEATURE_OPTION__Feature_Option_None);
}

esp_err_t rpc_bt_controller_disable(void)
{
    return rpc_feature_control(RPC_FEATURE_COMMAND__Feature_Command_BT_Disable,
                               RPC_FEATURE_OPTION__Feature_Option_None);
}

esp_err_t rpc_iface_mac_addr_set_get(bool set, uint8_t *mac, size_t mac_len,
                                     esp_mac_type_t type)
{
    RpcReqIfaceMacAddrSetGet request =
        RPC__REQ__IFACE_MAC_ADDR_SET_GET__INIT;
    ProtobufCMessage *message;
    RpcRespIfaceMacAddrSetGet *response;
    esp_err_t result;

    if (!mac || !mac_len)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.set = set;
    request.type = type;
    if (set)
    {
        request.mac.data = mac;
        request.mac.len = mac_len;
    }
    result = rpc_call_checked(RPC_ID__Req_IfaceMacAddrSetGet, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespIfaceMacAddrSetGet *)message;
        if (!set)
        {
            if (!response->mac.data || response->mac.len > mac_len)
            {
                result = ESP_FAIL;
            }
            else
            {
                rt_memcpy(mac, response->mac.data, response->mac.len);
            }
        }
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_iface_mac_addr_len_get(size_t *len, esp_mac_type_t type)
{
    RpcReqIfaceMacAddrLenGet request =
        RPC__REQ__IFACE_MAC_ADDR_LEN_GET__INIT;
    ProtobufCMessage *message;
    RpcRespIfaceMacAddrLenGet *response;
    esp_err_t result;

    if (!len)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.type = type;
    result = rpc_call_checked(RPC_ID__Req_IfaceMacAddrLenGet, &request.base,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespIfaceMacAddrLenGet *)message;
        *len = response->len;
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_iface_get_coprocessor_app_desc(esp_hosted_app_desc_t *app_desc)
{
    ProtobufCMessage *message;
    RpcRespAppGetDesc *response;
    EspAppDesc *source;
    esp_err_t result;

    if (!app_desc)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = rpc_call_checked(RPC_ID__Req_AppGetDesc, RT_NULL,
                              ESP_HOSTED_RPC_TIMEOUT_MS, &message);
    if (result == ESP_OK)
    {
        response = (RpcRespAppGetDesc *)message;
        source = response->app_desc;
        if (!source)
        {
            rt_esp_hosted_rpc_free_message(message);
            return ESP_FAIL;
        }
        rt_memset(app_desc, 0, sizeof(*app_desc));
        app_desc->magic_word = source->magic_word;
        app_desc->secure_version = source->secure_version;
        rpc_copy_binary(app_desc->reserv1, sizeof(app_desc->reserv1),
                        &source->reserv1);
        rpc_copy_binary(app_desc->version, sizeof(app_desc->version),
                        &source->version);
        rpc_copy_binary(app_desc->project_name, sizeof(app_desc->project_name),
                        &source->project_name);
        rpc_copy_binary(app_desc->time, sizeof(app_desc->time), &source->time);
        rpc_copy_binary(app_desc->date, sizeof(app_desc->date), &source->date);
        rpc_copy_binary(app_desc->idf_ver, sizeof(app_desc->idf_ver),
                        &source->idf_ver);
        rpc_copy_binary(app_desc->app_elf_sha256,
                        sizeof(app_desc->app_elf_sha256),
                        &source->app_elf_sha256);
        app_desc->min_efuse_blk_rev_full = source->min_efuse_blk_rev_full;
        app_desc->max_efuse_blk_rev_full = source->max_efuse_blk_rev_full;
        app_desc->mmu_page_size = source->mmu_page_size;
        rpc_copy_binary(app_desc->reserv3, sizeof(app_desc->reserv3),
                        &source->reserv3);
        rpc_copy_binary(app_desc->reserv2, sizeof(app_desc->reserv2),
                        &source->reserv2);
        rt_esp_hosted_rpc_free_message(message);
    }
    return result;
}

esp_err_t rpc_iface_configure_heartbeat(bool enable, int duration_sec)
{
    RpcReqConfigHeartbeat request = RPC__REQ__CONFIG_HEARTBEAT__INIT;

    if (duration_sec < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.enable = enable;
    request.duration = duration_sec;
    return rpc_call_status(RPC_ID__Req_ConfigHeartbeat, &request.base,
                           ESP_HOSTED_RPC_TIMEOUT_MS);
}

esp_err_t rpc_ota_begin(void)
{
    return rpc_call_status(RPC_ID__Req_OTABegin, RT_NULL,
                           RPC_OTA_TIMEOUT_MS);
}

esp_err_t rpc_ota_write(uint8_t *ota_data, uint32_t ota_data_len)
{
    RpcReqOTAWrite request = RPC__REQ__OTAWRITE__INIT;

    if (!ota_data || !ota_data_len)
    {
        return ESP_ERR_INVALID_ARG;
    }
    request.ota_data.data = ota_data;
    request.ota_data.len = ota_data_len;
    return rpc_call_status(RPC_ID__Req_OTAWrite, &request.base,
                           RPC_OTA_TIMEOUT_MS);
}

esp_err_t rpc_ota_end(void)
{
    return rpc_call_status(RPC_ID__Req_OTAEnd, RT_NULL, RPC_OTA_TIMEOUT_MS);
}

esp_err_t rpc_ota_activate(void)
{
    return rpc_call_status(RPC_ID__Req_OTAActivate, RT_NULL,
                           RPC_OTA_TIMEOUT_MS);
}
