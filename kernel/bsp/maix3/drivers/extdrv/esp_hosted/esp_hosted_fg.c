/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_wifi.h"
#include "esp_hosted_country.h"
#include "esp_hosted_fg_protocol.h"

#include <esp_hosted_config.pb-c.h>
#include <wlan_mgnt.h>

#define DBG_TAG "esp_hosted.wifi.fg"
#define DBG_LVL ESP_HOSTED_WIFI_DBG_LVL
#include <rtdbg.h>

#define EHF_FG_COMMAND_TIMEOUT \
    rt_tick_from_millisecond(ESP_HOSTED_WIFI_COMMAND_TIMEOUT_MS)
#define EHF_FG_CONNECT_TIMEOUT_MARGIN_MS 250U
#define EHF_FG_CONNECT_TIMEOUT_MS \
    (RT_WLAN_CONNECT_WAIT_MS > EHF_FG_CONNECT_TIMEOUT_MARGIN_MS ? \
     RT_WLAN_CONNECT_WAIT_MS - EHF_FG_CONNECT_TIMEOUT_MARGIN_MS : \
     RT_WLAN_CONNECT_WAIT_MS)
#define EHF_FG_TLV_ENDPOINT_TYPE 1U
#define EHF_FG_TLV_DATA_TYPE     2U
#define EHF_FG_ENDPOINT_LENGTH   8U
#define EHF_FG_TLV_HEADER_LENGTH (3U + EHF_FG_ENDPOINT_LENGTH + 3U)

static const rt_uint8_t g_ehf_fg_response_endpoint[EHF_FG_ENDPOINT_LENGTH] = {
    'c', 't', 'r', 'l', 'R', 'e', 's', 'p',
};
static const rt_uint8_t g_ehf_fg_event_endpoint[EHF_FG_ENDPOINT_LENGTH] = {
    'c', 't', 'r', 'l', 'E', 'v', 'n', 't',
};

static rt_uint8_t ehf_fg_interface_from_iftype(enum rt_wlan_offload_iftype iftype)
{
    return iftype == RT_WLAN_OFFLOAD_IFTYPE_AP ? EHF_FG_AP_INTERFACE :
                                            EHF_FG_STA_INTERFACE;
}

static void ehf_fg_channel_definition(
    rt_uint32_t channel, struct rt_wlan_offload_channel_definition *definition)
{
    rt_memset(definition, 0, sizeof(*definition));
    definition->width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
    definition->primary_channel = channel;
    if (channel <= 14)
    {
        definition->band = RT_WLAN_OFFLOAD_BAND_2GHZ;
        definition->primary_frequency_mhz = channel == 14 ? 2484 :
                                            2407 + channel * 5;
    }
    else
    {
        definition->band = RT_WLAN_OFFLOAD_BAND_5GHZ;
        definition->primary_frequency_mhz = 5000 + channel * 5;
    }
    definition->center_frequency1_mhz = definition->primary_frequency_mhz;
}

static int ehf_fg_hex_value(rt_uint8_t value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

static rt_err_t ehf_fg_mac_from_wire(const ProtobufCBinaryData *wire,
                                     rt_uint8_t mac[6])
{
    rt_size_t index;

    if (!wire || !wire->data)
    {
        return -RT_EINVAL;
    }
    if (wire->len == 6)
    {
        rt_memcpy(mac, wire->data, 6);
        return RT_EOK;
    }
    if (wire->len != 17)
    {
        return -RT_EINVAL;
    }
    for (index = 0; index < 6; index++)
    {
        int high = ehf_fg_hex_value(wire->data[index * 3]);
        int low = ehf_fg_hex_value(wire->data[index * 3 + 1]);

        if (high < 0 || low < 0 ||
            (index < 5 && wire->data[index * 3 + 2] != ':'))
        {
            return -RT_EINVAL;
        }
        mac[index] = (high << 4) | low;
    }
    return RT_EOK;
}

static void ehf_fg_mac_to_wire(const rt_uint8_t mac[6], char output[18])
{
    rt_snprintf(output, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static rt_uint16_t ehf_fg_frame_checksum(const rt_uint8_t *frame,
                                         rt_size_t length)
{
    rt_uint16_t result = 0;
    rt_size_t index;

    for (index = 0; index < length; index++)
    {
        if (index != 6 && index != 7)
        {
            result += frame[index];
        }
    }
    return result;
}

static rt_err_t ehf_fg_send_frame(struct ehf_context *context,
                                  rt_uint8_t interface, rt_uint8_t flags,
                                  rt_uint8_t private_type,
                                  rt_uint16_t sequence,
                                  const void *payload,
                                  rt_size_t payload_length,
                                  rt_bool_t control)
{
    struct ehf_fg_transport_header *header;
    rt_size_t frame_length = sizeof(*header) + payload_length;
    rt_uint8_t *frame;
    rt_err_t result;

    if ((payload_length && !payload) || payload_length > 0xffffU ||
        frame_length > context->bus->max_tx_size)
    {
        return -RT_EINVAL;
    }
    frame = rt_calloc(1, frame_length);
    if (!frame)
    {
        return -RT_ENOMEM;
    }
    header = (struct ehf_fg_transport_header *)frame;
    header->interface_number = interface & 0x0f;
    header->flags = flags;
    header->private_type = private_type;
    ehf_put_le16(header->length, payload_length);
    ehf_put_le16(header->offset, sizeof(*header));
    ehf_put_le16(header->sequence, sequence);
    if (payload_length)
    {
        rt_memcpy(frame + sizeof(*header), payload, payload_length);
    }
    if (context->checksum_enabled)
    {
        ehf_put_le16(header->checksum,
                     ehf_fg_frame_checksum(frame, frame_length));
    }
    result = control ?
        rt_wlan_offload_bus_transmit_priority(context->bus,
                                         RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL,
                                         frame, frame_length) :
        rt_wlan_offload_bus_transmit(context->bus, frame, frame_length);
    rt_free(frame);
    return result;
}

static rt_err_t ehf_fg_send_serial(struct ehf_context *context,
                                   const void *data, rt_size_t length)
{
    const rt_uint8_t *position = data;
    rt_uint16_t sequence;

    if (!data || !length)
    {
        return -RT_EINVAL;
    }
    context->tx_sequence++;
    if (!context->tx_sequence)
    {
        context->tx_sequence++;
    }
    sequence = context->tx_sequence;
    while (length)
    {
        rt_size_t fragment_length = length > EHF_FG_SERIAL_PAYLOAD ?
                                    EHF_FG_SERIAL_PAYLOAD : length;
        rt_uint8_t flags = length > fragment_length ?
                           EHF_FG_MORE_FRAGMENT : 0;
        rt_err_t result = ehf_fg_send_frame(
            context, EHF_FG_SERIAL_INTERFACE, flags, 0, sequence,
            position, fragment_length, RT_TRUE);

        if (result != RT_EOK)
        {
            return result;
        }
        position += fragment_length;
        length -= fragment_length;
    }
    return RT_EOK;
}

static rt_err_t ehf_fg_command_push(
    struct rt_wlan_offload_command_manager *manager, rt_uint32_t token,
    rt_uint16_t command_id, const void *request, rt_size_t request_length,
    void *driver_data)
{
    struct ehf_context *context = driver_data;
    CtrlMsg *message = (CtrlMsg *)request;
    rt_size_t protobuf_length;
    rt_size_t serial_length;
    rt_uint8_t *serial;
    rt_err_t result;

    (void)manager;
    if (!message || request_length != sizeof(*message))
    {
        return -RT_EINVAL;
    }
    message->msg_type = CTRL_MSG_TYPE__Req;
    message->msg_id = command_id;
    message->uid = token;
    message->req_resp_type = 0;
    protobuf_length = ctrl_msg__get_packed_size(message);
    if (!protobuf_length || protobuf_length > 0xffffU)
    {
        return -RT_EINVAL;
    }
    serial_length = EHF_FG_TLV_HEADER_LENGTH + protobuf_length;
    serial = rt_malloc(serial_length);
    if (!serial)
    {
        return -RT_ENOMEM;
    }
    serial[0] = EHF_FG_TLV_ENDPOINT_TYPE;
    ehf_put_le16(serial + 1, EHF_FG_ENDPOINT_LENGTH);
    rt_memcpy(serial + 3, g_ehf_fg_response_endpoint,
              EHF_FG_ENDPOINT_LENGTH);
    serial[3 + EHF_FG_ENDPOINT_LENGTH] = EHF_FG_TLV_DATA_TYPE;
    ehf_put_le16(serial + 4 + EHF_FG_ENDPOINT_LENGTH, protobuf_length);
    ctrl_msg__pack(message, serial + EHF_FG_TLV_HEADER_LENGTH);
    result = ehf_fg_send_serial(context, serial, serial_length);
    rt_free(serial);
    return result;
}

static rt_err_t ehf_fg_response_status(const CtrlMsg *message)
{
    if (!message)
    {
        return -RT_EINVAL;
    }
    switch (message->msg_id)
    {
    case CTRL_MSG_ID__Resp_GetMACAddress:
        return message->resp_get_mac_address &&
               !message->resp_get_mac_address->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_SetMacAddress:
        return message->resp_set_mac_address &&
               !message->resp_set_mac_address->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_SetWifiMode:
        return message->resp_set_wifi_mode &&
               !message->resp_set_wifi_mode->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_GetAPScanList:
        return message->resp_scan_ap_list &&
               !message->resp_scan_ap_list->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_GetAPConfig:
        return message->resp_get_ap_config &&
               !message->resp_get_ap_config->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_ConnectAP:
        return message->resp_connect_ap &&
               !message->resp_connect_ap->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_DisconnectAP:
        return message->resp_disconnect_ap &&
               !message->resp_disconnect_ap->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_StartSoftAP:
        return message->resp_start_softap &&
               !message->resp_start_softap->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_StopSoftAP:
        return message->resp_stop_softap &&
               !message->resp_stop_softap->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_SetPowerSaveMode:
        return message->resp_set_power_save_mode &&
               !message->resp_set_power_save_mode->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_GetPowerSaveMode:
        return message->resp_get_power_save_mode &&
               !message->resp_get_power_save_mode->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_SetCountryCode:
        return message->resp_set_country_code &&
               !message->resp_set_country_code->resp ? RT_EOK : -RT_ERROR;
    case CTRL_MSG_ID__Resp_GetCountryCode:
        return message->resp_get_country_code &&
               !message->resp_get_country_code->resp ? RT_EOK : -RT_ERROR;
    default:
        return RT_EOK;
    }
}

static rt_err_t ehf_fg_command(struct ehf_context *context,
                               CtrlMsgId request_id, CtrlMsgId response_id,
                               CtrlMsg *request, CtrlMsg **response)
{
    rt_uint8_t *reply;
    rt_size_t reply_length = 0;
    rt_err_t result;

    if (!response)
    {
        return -RT_EINVAL;
    }
    *response = RT_NULL;
    reply = rt_malloc(ESP_HOSTED_WIFI_CONTROL_BUFFER_SIZE);
    if (!reply)
    {
        return -RT_ENOMEM;
    }
    result = rt_wlan_offload_command_execute(
        &context->commands, request_id, response_id,
        request, sizeof(*request), reply,
        ESP_HOSTED_WIFI_CONTROL_BUFFER_SIZE, &reply_length,
        EHF_FG_COMMAND_TIMEOUT, RT_NULL);
    if (result != RT_EOK)
    {
        rt_free(reply);
        return result;
    }
    *response = ctrl_msg__unpack(RT_NULL, reply_length, reply);
    rt_free(reply);
    if (!*response || (*response)->msg_type != CTRL_MSG_TYPE__Resp ||
        (*response)->msg_id != response_id)
    {
        if (*response)
        {
            ctrl_msg__free_unpacked(*response, RT_NULL);
            *response = RT_NULL;
        }
        return -RT_EIO;
    }
    result = ehf_fg_response_status(*response);
    if (result != RT_EOK)
    {
        ctrl_msg__free_unpacked(*response, RT_NULL);
        *response = RT_NULL;
    }
    return result;
}

static void ehf_fg_report_connect_result(struct ehf_context *context,
                                         rt_uint32_t request_id,
                                         rt_err_t status,
                                         const rt_uint8_t bssid[6])
{
    struct rt_wlan_offload_event event;
    rt_err_t result;

    if (!request_id)
    {
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = request_id;
    event.status = status;
    if (bssid)
    {
        rt_memcpy(event.data.network.bssid, bssid,
                  sizeof(event.data.network.bssid));
    }
    result = rt_wlan_offload_report_event(&context->radio, &event);
    if (result != RT_EOK)
    {
        LOG_W("connect result request=%u status=%d rejected: %d",
              request_id, status, result);
    }
}

static void ehf_fg_connect_timeout(void *parameter)
{
    struct ehf_context *context = parameter;
    rt_uint32_t request_id;
    rt_uint16_t retries;

    if (!context ||
        rt_mutex_take(&context->radio.operation_lock,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return;
    }
    request_id = context->connect_request_id;
    retries = context->connect_retry_count;
    if (request_id)
    {
        context->connect_request_id = 0;
        context->connect_retry_count = 0;
        context->sta_connected = RT_FALSE;
        rt_memset(context->sta_bssid, 0, sizeof(context->sta_bssid));
    }
    rt_mutex_release(&context->radio.operation_lock);

    if (request_id)
    {
        LOG_W("station connection request=%u timed out after %u ms "
              "(%u disconnect events)", request_id,
              (unsigned int)EHF_FG_CONNECT_TIMEOUT_MS, retries);
        ehf_fg_report_connect_result(context, request_id, -RT_ETIMEOUT,
                                     RT_NULL);
    }
}

static void ehf_fg_connect_timer_init(struct ehf_context *context)
{
    if (context->connect_timer_initialized)
    {
        return;
    }
    rt_timer_init(&context->connect_timer, "ehf-conn",
                  ehf_fg_connect_timeout, context,
                  rt_tick_from_millisecond(EHF_FG_CONNECT_TIMEOUT_MS),
                  RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    context->connect_timer_initialized = RT_TRUE;
}

static void ehf_fg_connect_timer_deinit(struct ehf_context *context)
{
    if (!context->connect_timer_initialized)
    {
        return;
    }
    rt_timer_stop(&context->connect_timer);
    rt_timer_detach(&context->connect_timer);
    context->connect_timer_initialized = RT_FALSE;
}

static rt_err_t ehf_fg_start(struct rt_wlan_offload_radio *radio)
{
    struct ehf_context *context = ehf_context_from_radio(radio);
    rt_err_t result;

    rt_wlan_offload_command_manager_reset(&context->commands);
    result = ehf_wait_for_boot(context);
    if (result == RT_EOK)
    {
        ehf_fg_connect_timer_init(context);
    }
    return result;
}

static rt_err_t ehf_fg_stop(struct rt_wlan_offload_radio *radio)
{
    struct ehf_context *context = ehf_context_from_radio(radio);

    ehf_fg_connect_timer_deinit(context);
    context->sta_enabled = RT_FALSE;
    context->ap_enabled = RT_FALSE;
    context->sta_connected = RT_FALSE;
    context->ap_started = RT_FALSE;
    context->scan_request_id = 0;
    context->connect_request_id = 0;
    context->connect_retry_count = 0;
    rt_memset(context->sta_bssid, 0, sizeof(context->sta_bssid));
    rt_wlan_offload_command_manager_fail(&context->commands, -RT_EIO);
    return RT_EOK;
}

static rt_err_t ehf_fg_set_mode(struct ehf_context *context,
                                rt_bool_t sta, rt_bool_t ap)
{
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqSetMode payload = CTRL_MSG__REQ__SET_MODE__INIT;
    CtrlMsg *response;
    rt_err_t result;

    payload.mode = sta && ap ? CTRL__WIFI_MODE__APSTA :
                   sta ? CTRL__WIFI_MODE__STA :
                   ap ? CTRL__WIFI_MODE__AP : CTRL__WIFI_MODE__NONE;
    request.payload_case = CTRL_MSG__PAYLOAD_REQ_SET_WIFI_MODE;
    request.req_set_wifi_mode = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_SetWifiMode,
                            CTRL_MSG_ID__Resp_SetWifiMode,
                            &request, &response);
    if (result == RT_EOK)
    {
        ctrl_msg__free_unpacked(response, RT_NULL);
    }
    return result;
}

static rt_err_t ehf_fg_change_interface(struct rt_wlan_offload_vif *vif,
                                         enum rt_wlan_offload_iftype iftype,
                                         rt_bool_t enabled)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    rt_bool_t sta = context->sta_enabled;
    rt_bool_t ap = context->ap_enabled;
    rt_err_t result;

    if (iftype == RT_WLAN_OFFLOAD_IFTYPE_AP)
    {
        ap = enabled;
    }
    else
    {
        sta = enabled;
    }
    result = ehf_fg_set_mode(context, sta, ap);
    if (result == RT_EOK)
    {
        context->sta_enabled = sta;
        context->ap_enabled = ap;
        if (!sta)
        {
            if (context->connect_timer_initialized)
            {
                rt_timer_stop(&context->connect_timer);
            }
            context->sta_connected = RT_FALSE;
            context->connect_request_id = 0;
            context->connect_retry_count = 0;
            rt_memset(context->sta_bssid, 0,
                      sizeof(context->sta_bssid));
        }
        if (!ap)
        {
            context->ap_started = RT_FALSE;
        }
    }
    return result;
}

static rt_wlan_security_t ehf_fg_security_from_wire(CtrlWifiSecProt security)
{
    switch (security)
    {
    case CTRL__WIFI_SEC_PROT__Open: return SECURITY_OPEN;
    case CTRL__WIFI_SEC_PROT__WEP: return SECURITY_WEP_PSK;
    case CTRL__WIFI_SEC_PROT__WPA_PSK: return SECURITY_WPA_AES_PSK;
    case CTRL__WIFI_SEC_PROT__WPA2_PSK: return SECURITY_WPA2_AES_PSK;
    case CTRL__WIFI_SEC_PROT__WPA_WPA2_PSK:
        return SECURITY_WPA_WPA2_MIXED_PSK;
    case CTRL__WIFI_SEC_PROT__WPA3_PSK:
        return SECURITY_WPA3_SAE;
    case CTRL__WIFI_SEC_PROT__WPA2_WPA3_PSK:
        return SECURITY_WPA2_WPA3_MIXED_PSK;
    default: return SECURITY_UNKNOWN;
    }
}

static rt_err_t ehf_fg_security_to_wire(rt_wlan_security_t security,
                                        CtrlWifiSecProt *wire)
{
    if (!wire)
    {
        return -RT_EINVAL;
    }
    switch (security)
    {
    case SECURITY_OPEN:
        *wire = CTRL__WIFI_SEC_PROT__Open;
        return RT_EOK;
    case SECURITY_WEP_PSK:
    case SECURITY_WEP_SHARED:
        *wire = CTRL__WIFI_SEC_PROT__WEP;
        return RT_EOK;
    case SECURITY_WPA_TKIP_PSK:
    case SECURITY_WPA_AES_PSK:
        *wire = CTRL__WIFI_SEC_PROT__WPA_PSK;
        return RT_EOK;
    case SECURITY_WPA2_TKIP_PSK:
    case SECURITY_WPA2_AES_PSK:
    case SECURITY_WPA2_MIXED_PSK:
        *wire = CTRL__WIFI_SEC_PROT__WPA2_PSK;
        return RT_EOK;
    case SECURITY_WPA_WPA2_MIXED_PSK:
        *wire = CTRL__WIFI_SEC_PROT__WPA_WPA2_PSK;
        return RT_EOK;
    case SECURITY_WPA3_AES_PSK:
    case SECURITY_WPA3_SAE:
        *wire = CTRL__WIFI_SEC_PROT__WPA3_PSK;
        return RT_EOK;
    case SECURITY_WPA2_WPA3_MIXED_PSK:
        *wire = CTRL__WIFI_SEC_PROT__WPA2_WPA3_PSK;
        return RT_EOK;
    default:
        return -RT_ENOSYS;
    }
}

static rt_err_t ehf_fg_scan(struct rt_wlan_offload_vif *vif,
                            const struct rt_wlan_offload_scan_request *scan_request)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqScanResult payload = CTRL_MSG__REQ__SCAN_RESULT__INIT;
    CtrlMsg *response;
    CtrlMsgRespScanResult *scan;
    struct rt_wlan_offload_event event;
    rt_size_t index;
    rt_err_t result;

    request.payload_case = CTRL_MSG__PAYLOAD_REQ_SCAN_AP_LIST;
    request.req_scan_ap_list = &payload;
    context->scan_request_id = scan_request->request_id;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_GetAPScanList,
                            CTRL_MSG_ID__Resp_GetAPScanList,
                            &request, &response);
    if (result != RT_EOK)
    {
        context->scan_request_id = 0;
        return result;
    }

    scan = response->resp_scan_ap_list;
    for (index = 0; scan && index < scan->n_entries; index++)
    {
        const ScanResult *entry = scan->entries[index];

        if (!entry || !entry->chnl)
        {
            continue;
        }
        rt_memset(&event, 0, sizeof(event));
        event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_RESULT;
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
        event.request_id = scan_request->request_id;
        event.status = RT_EOK;
        event.data.network.ssid.len =
            entry->ssid.len <= RT_WLAN_SSID_MAX_LENGTH ?
            entry->ssid.len : RT_WLAN_SSID_MAX_LENGTH;
        if (entry->ssid.data)
        {
            rt_memcpy(event.data.network.ssid.val, entry->ssid.data,
                      event.data.network.ssid.len);
        }
        if (ehf_fg_mac_from_wire(&entry->bssid,
                                 event.data.network.bssid) != RT_EOK)
        {
            continue;
        }
        ehf_fg_channel_definition(entry->chnl,
                                  &event.data.network.channel);
        event.data.network.rssi = entry->rssi;
        event.data.network.security =
            ehf_fg_security_from_wire(entry->sec_prot);
        rt_wlan_offload_report_event(&context->radio, &event);
    }
    ctrl_msg__free_unpacked(response, RT_NULL);

    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_DONE;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = scan_request->request_id;
    event.status = RT_EOK;
    context->scan_request_id = 0;
    return rt_wlan_offload_report_event(&context->radio, &event);
}

static rt_bool_t ehf_fg_mac_is_zero(const rt_uint8_t mac[6])
{
    static const rt_uint8_t zero[6] = {0};

    return rt_memcmp(mac, zero, sizeof(zero)) == 0;
}

static rt_err_t ehf_fg_connect(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_connect_request *connect_request)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqConnectAP payload = CTRL_MSG__REQ__CONNECT_AP__INIT;
    CtrlMsg *response;
    char ssid[RT_WLAN_SSID_MAX_LENGTH + 1] = {0};
    char password[RT_WLAN_PASSWORD_MAX_LENGTH + 1] = {0};
    char bssid[18] = {0};
    CtrlWifiSecProt security;
    rt_err_t result;

    result = ehf_fg_security_to_wire(connect_request->security, &security);
    if (result != RT_EOK)
    {
        return result;
    }
    rt_memcpy(ssid, connect_request->ssid.val, connect_request->ssid.len);
    rt_memcpy(password, connect_request->key.val, connect_request->key.len);
    if (!ehf_fg_mac_is_zero(connect_request->bssid))
    {
        ehf_fg_mac_to_wire(connect_request->bssid, bssid);
    }
    payload.ssid = ssid;
    payload.pwd = password;
    payload.bssid = bssid;
    payload.is_wpa3_supported =
        security == CTRL__WIFI_SEC_PROT__WPA3_PSK ||
        security == CTRL__WIFI_SEC_PROT__WPA2_WPA3_PSK;
    payload.bw = CTRL__WIFI_BW__HT20;
    request.payload_case = CTRL_MSG__PAYLOAD_REQ_CONNECT_AP;
    request.req_connect_ap = &payload;
    rt_mutex_take(&context->radio.operation_lock, RT_WAITING_FOREVER);
    context->connect_request_id = connect_request->request_id;
    context->connect_retry_count = 0;
    context->sta_connected = RT_FALSE;
    rt_memset(context->sta_bssid, 0, sizeof(context->sta_bssid));
    rt_mutex_release(&context->radio.operation_lock);
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_ConnectAP,
                            CTRL_MSG_ID__Resp_ConnectAP,
                            &request, &response);
    if (result == RT_EOK)
    {
        rt_bool_t pending;

        ctrl_msg__free_unpacked(response, RT_NULL);
        rt_mutex_take(&context->radio.operation_lock, RT_WAITING_FOREVER);
        pending = context->connect_request_id == connect_request->request_id;
        rt_mutex_release(&context->radio.operation_lock);
        if (pending && context->connect_timer_initialized)
        {
            result = rt_timer_start(&context->connect_timer);
            if (result != RT_EOK)
            {
                rt_mutex_take(&context->radio.operation_lock,
                              RT_WAITING_FOREVER);
                if (context->connect_request_id ==
                    connect_request->request_id)
                {
                    context->connect_request_id = 0;
                }
                rt_mutex_release(&context->radio.operation_lock);
                return result;
            }
        }
        return RT_EOK;
    }
    rt_mutex_take(&context->radio.operation_lock, RT_WAITING_FOREVER);
    if (context->connect_request_id == connect_request->request_id)
    {
        context->connect_request_id = 0;
    }
    rt_mutex_release(&context->radio.operation_lock);
    return result;
}

static rt_err_t ehf_fg_disconnect(struct rt_wlan_offload_vif *vif,
                                  rt_uint32_t request_id,
                                  rt_uint16_t reason)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqGetStatus payload = CTRL_MSG__REQ__GET_STATUS__INIT;
    CtrlMsg *response;
    struct rt_wlan_offload_event event;
    rt_err_t result;

    request.payload_case = CTRL_MSG__PAYLOAD_REQ_DISCONNECT_AP;
    request.req_disconnect_ap = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_DisconnectAP,
                            CTRL_MSG_ID__Resp_DisconnectAP,
                            &request, &response);
    if (result != RT_EOK)
    {
        return result;
    }
    ctrl_msg__free_unpacked(response, RT_NULL);
    if (context->connect_timer_initialized)
    {
        rt_timer_stop(&context->connect_timer);
    }
    context->sta_connected = RT_FALSE;
    context->connect_request_id = 0;
    context->connect_retry_count = 0;
    rt_memset(context->sta_bssid, 0, sizeof(context->sta_bssid));
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_DISCONNECTED;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = request_id;
    event.status = RT_EOK;
    event.data.disconnected.reason = reason;
    event.data.disconnected.locally_generated = RT_TRUE;
    return rt_wlan_offload_report_event(&context->radio, &event);
}

static rt_err_t ehf_fg_start_ap(struct rt_wlan_offload_vif *vif,
                                const struct rt_wlan_offload_ap_settings *settings)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqStartSoftAP payload = CTRL_MSG__REQ__START_SOFT_AP__INIT;
    CtrlMsg *response;
    struct rt_wlan_offload_event event;
    char ssid[RT_WLAN_SSID_MAX_LENGTH + 1] = {0};
    char password[RT_WLAN_PASSWORD_MAX_LENGTH + 1] = {0};
    CtrlWifiSecProt security;
    rt_err_t result;

    result = ehf_fg_security_to_wire(settings->security, &security);
    if (result != RT_EOK)
    {
        return result;
    }
    rt_memcpy(ssid, settings->ssid.val, settings->ssid.len);
    rt_memcpy(password, settings->key.val, settings->key.len);
    payload.ssid = ssid;
    payload.pwd = password;
    payload.chnl = settings->channel.primary_channel;
    payload.sec_prot = security;
    payload.max_conn = settings->max_stations ? settings->max_stations : 4;
    payload.ssid_hidden = settings->hidden;
    payload.bw = CTRL__WIFI_BW__HT20;
    request.payload_case = CTRL_MSG__PAYLOAD_REQ_START_SOFTAP;
    request.req_start_softap = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_StartSoftAP,
                            CTRL_MSG_ID__Resp_StartSoftAP,
                            &request, &response);
    if (result != RT_EOK)
    {
        return result;
    }

    context->ap_started = RT_TRUE;
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_AP_STARTED;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
    event.request_id = settings->request_id;
    event.status = RT_EOK;
    if (response->resp_start_softap)
    {
        ehf_fg_mac_from_wire(&response->resp_start_softap->mac,
                             event.data.network.bssid);
    }
    ctrl_msg__free_unpacked(response, RT_NULL);
    return rt_wlan_offload_report_event(&context->radio, &event);
}

static rt_err_t ehf_fg_stop_ap(struct rt_wlan_offload_vif *vif,
                               rt_uint32_t request_id)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqGetStatus payload = CTRL_MSG__REQ__GET_STATUS__INIT;
    CtrlMsg *response;
    struct rt_wlan_offload_event event;
    rt_err_t result;

    request.payload_case = CTRL_MSG__PAYLOAD_REQ_STOP_SOFTAP;
    request.req_stop_softap = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_StopSoftAP,
                            CTRL_MSG_ID__Resp_StopSoftAP,
                            &request, &response);
    if (result != RT_EOK)
    {
        return result;
    }
    ctrl_msg__free_unpacked(response, RT_NULL);
    context->ap_started = RT_FALSE;
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_AP_STOPPED;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
    event.request_id = request_id;
    event.status = RT_EOK;
    return rt_wlan_offload_report_event(&context->radio, &event);
}

static rt_err_t ehf_fg_get_rssi(struct rt_wlan_offload_vif *vif, int *rssi)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqGetAPConfig payload = CTRL_MSG__REQ__GET_APCONFIG__INIT;
    CtrlMsg *response;
    rt_err_t result;

    request.payload_case = CTRL_MSG__PAYLOAD_REQ_GET_AP_CONFIG;
    request.req_get_ap_config = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_GetAPConfig,
                            CTRL_MSG_ID__Resp_GetAPConfig,
                            &request, &response);
    if (result == RT_EOK)
    {
        *rssi = response->resp_get_ap_config->rssi;
        ctrl_msg__free_unpacked(response, RT_NULL);
    }
    return result;
}

static rt_err_t ehf_fg_set_power_save(struct rt_wlan_offload_vif *vif, int level)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqSetMode payload = CTRL_MSG__REQ__SET_MODE__INIT;
    CtrlMsg *response;
    rt_err_t result;

    if (level < CTRL__WIFI_POWER_SAVE__NO_PS ||
        level >= CTRL__WIFI_POWER_SAVE__PS_Invalid)
    {
        return -RT_EINVAL;
    }
    payload.mode = level;
    request.payload_case = CTRL_MSG__PAYLOAD_REQ_SET_POWER_SAVE_MODE;
    request.req_set_power_save_mode = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_SetPowerSaveMode,
                            CTRL_MSG_ID__Resp_SetPowerSaveMode,
                            &request, &response);
    if (result == RT_EOK)
    {
        ctrl_msg__free_unpacked(response, RT_NULL);
    }
    return result;
}

static rt_err_t ehf_fg_get_power_save(struct rt_wlan_offload_vif *vif, int *level)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqGetMode payload = CTRL_MSG__REQ__GET_MODE__INIT;
    CtrlMsg *response;
    rt_err_t result;

    request.payload_case = CTRL_MSG__PAYLOAD_REQ_GET_POWER_SAVE_MODE;
    request.req_get_power_save_mode = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_GetPowerSaveMode,
                            CTRL_MSG_ID__Resp_GetPowerSaveMode,
                            &request, &response);
    if (result == RT_EOK)
    {
        *level = response->resp_get_power_save_mode->mode;
        ctrl_msg__free_unpacked(response, RT_NULL);
    }
    return result;
}

static rt_err_t ehf_fg_set_regulatory(struct rt_wlan_offload_radio *radio,
                                      rt_country_code_t country)
{
    struct ehf_context *context = ehf_context_from_radio(radio);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqSetCountryCode payload =
        CTRL_MSG__REQ__SET_COUNTRY_CODE__INIT;
    CtrlMsg *response;
    const char *alpha2 = ehf_country_code_from_rt(country);
    rt_err_t result;

    if (!alpha2)
    {
        return -RT_EINVAL;
    }
    payload.country.data = (rt_uint8_t *)alpha2;
    payload.country.len = 2;
    payload.ieee80211d_enabled = 1;
    request.payload_case = CTRL_MSG__PAYLOAD_REQ_SET_COUNTRY_CODE;
    request.req_set_country_code = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_SetCountryCode,
                            CTRL_MSG_ID__Resp_SetCountryCode,
                            &request, &response);
    if (result == RT_EOK)
    {
        ctrl_msg__free_unpacked(response, RT_NULL);
    }
    return result;
}

static rt_err_t ehf_fg_get_regulatory(struct rt_wlan_offload_radio *radio,
                                      rt_country_code_t *country)
{
    struct ehf_context *context = ehf_context_from_radio(radio);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqGetCountryCode payload =
        CTRL_MSG__REQ__GET_COUNTRY_CODE__INIT;
    CtrlMsg *response;
    rt_err_t result;

    request.payload_case = CTRL_MSG__PAYLOAD_REQ_GET_COUNTRY_CODE;
    request.req_get_country_code = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_GetCountryCode,
                            CTRL_MSG_ID__Resp_GetCountryCode,
                            &request, &response);
    if (result == RT_EOK)
    {
        CtrlMsgRespGetCountryCode *wire = response->resp_get_country_code;

        if (!wire->country.data || wire->country.len < 2)
        {
            result = -RT_EIO;
        }
        else
        {
            *country = ehf_country_code_to_rt(wire->country.data,
                                              wire->country.len);
            if (*country == RT_COUNTRY_UNKNOWN)
            {
                result = -RT_EINVAL;
            }
        }
        ctrl_msg__free_unpacked(response, RT_NULL);
    }
    return result;
}

static rt_err_t ehf_fg_set_mac(struct rt_wlan_offload_vif *vif, rt_uint8_t mac[6])
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqSetMacAddress payload =
        CTRL_MSG__REQ__SET_MAC_ADDRESS__INIT;
    CtrlMsg *response;
    char wire_mac[18];
    rt_err_t result;

    ehf_fg_mac_to_wire(mac, wire_mac);
    payload.mac.data = (rt_uint8_t *)wire_mac;
    payload.mac.len = 17;
    payload.mode = vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP ?
                   CTRL__WIFI_MODE__AP : CTRL__WIFI_MODE__STA;
    request.payload_case = CTRL_MSG__PAYLOAD_REQ_SET_MAC_ADDRESS;
    request.req_set_mac_address = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_SetMacAddress,
                            CTRL_MSG_ID__Resp_SetMacAddress,
                            &request, &response);
    if (result == RT_EOK)
    {
        rt_memcpy(vif->address, mac, 6);
        ctrl_msg__free_unpacked(response, RT_NULL);
    }
    return result;
}

static rt_err_t ehf_fg_get_mac(struct rt_wlan_offload_vif *vif, rt_uint8_t mac[6])
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    CtrlMsg request = CTRL_MSG__INIT;
    CtrlMsgReqGetMacAddress payload =
        CTRL_MSG__REQ__GET_MAC_ADDRESS__INIT;
    CtrlMsg *response;
    rt_err_t result;

    payload.mode = vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP ?
                   CTRL__WIFI_MODE__AP : CTRL__WIFI_MODE__STA;
    request.payload_case = CTRL_MSG__PAYLOAD_REQ_GET_MAC_ADDRESS;
    request.req_get_mac_address = &payload;
    result = ehf_fg_command(context, CTRL_MSG_ID__Req_GetMACAddress,
                            CTRL_MSG_ID__Resp_GetMACAddress,
                            &request, &response);
    if (result == RT_EOK)
    {
        CtrlMsgRespGetMacAddress *wire = response->resp_get_mac_address;

        if (ehf_fg_mac_from_wire(&wire->mac, mac) != RT_EOK)
        {
            result = -RT_EIO;
        }
        else
        {
            rt_memcpy(vif->address, mac, 6);
        }
        ctrl_msg__free_unpacked(response, RT_NULL);
    }
    return result;
}

static rt_err_t ehf_fg_transmit(struct rt_wlan_offload_vif *vif,
                                const void *data, int length)
{
    struct ehf_context *context = ehf_context_from_vif(vif);

    if (length <= 0)
    {
        return -RT_EINVAL;
    }
    context->tx_sequence++;
    return ehf_fg_send_frame(
        context, ehf_fg_interface_from_iftype(vif->iftype), 0, 0,
        context->tx_sequence, data, length, RT_FALSE);
}

static void ehf_fg_report_station_event(struct ehf_context *context,
                                        rt_bool_t connected,
                                        const ProtobufCBinaryData *mac,
                                        rt_uint32_t aid,
                                        rt_uint32_t reason)
{
    struct rt_wlan_offload_event event;

    rt_memset(&event, 0, sizeof(event));
    if (ehf_fg_mac_from_wire(mac, event.data.station.mac) != RT_EOK)
    {
        return;
    }
    event.type = connected ? RT_WLAN_OFFLOAD_EVENT_NEW_STATION :
                             RT_WLAN_OFFLOAD_EVENT_DEL_STATION;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
    event.status = RT_EOK;
    event.data.station.aid = aid;
    (void)reason;
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void ehf_fg_process_event_message(struct ehf_context *context,
                                         const CtrlMsg *message)
{
    struct rt_wlan_offload_event event;

    switch (message->msg_id)
    {
    case CTRL_MSG_ID__Event_StationDisconnectFromAP:
        if (message->event_station_disconnect_from_ap)
        {
            const CtrlMsgEventStationDisconnectFromAP *wire =
                message->event_station_disconnect_from_ap;
            rt_uint32_t request_id;
            rt_bool_t was_connected;
            rt_uint16_t retries = 0;

            rt_mutex_take(&context->radio.operation_lock,
                          RT_WAITING_FOREVER);
            request_id = context->connect_request_id;
            was_connected = context->sta_connected;
            context->sta_connected = RT_FALSE;
            rt_memset(context->sta_bssid, 0,
                      sizeof(context->sta_bssid));
            if (request_id)
            {
                context->connect_retry_count++;
                retries = context->connect_retry_count;
            }
            rt_mutex_release(&context->radio.operation_lock);

            if (request_id)
            {
                LOG_I("station association retry request=%u reason=%u "
                      "attempt=%u", request_id, wire->reason, retries);
                break;
            }
            if (was_connected)
            {
                rt_memset(&event, 0, sizeof(event));
                event.type = RT_WLAN_OFFLOAD_EVENT_DISCONNECTED;
                event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
                event.status = wire->resp ? -RT_ERROR : RT_EOK;
                event.data.disconnected.reason = wire->reason;
                ehf_fg_mac_from_wire(&wire->bssid,
                                     event.data.disconnected.bssid);
                rt_wlan_offload_report_event(&context->radio, &event);
            }
        }
        break;

    case CTRL_MSG_ID__Event_StationConnectedToAP:
        if (message->event_station_connected_to_ap)
        {
            const CtrlMsgEventStationConnectedToAP *wire =
                message->event_station_connected_to_ap;
            rt_bool_t connected = !wire->resp;
            rt_bool_t was_connected;
            rt_uint32_t request_id;
            rt_uint16_t retries;
            rt_uint8_t bssid[6] = {0};

            if (connected)
            {
                ehf_fg_mac_from_wire(&wire->bssid, bssid);
            }
            if (context->connect_timer_initialized)
            {
                rt_timer_stop(&context->connect_timer);
            }
            rt_mutex_take(&context->radio.operation_lock,
                          RT_WAITING_FOREVER);
            request_id = context->connect_request_id;
            retries = context->connect_retry_count;
            was_connected = context->sta_connected;
            context->connect_request_id = 0;
            context->connect_retry_count = 0;
            context->sta_connected = connected;
            if (connected && !ehf_fg_mac_is_zero(bssid))
            {
                rt_memcpy(context->sta_bssid, bssid,
                          sizeof(context->sta_bssid));
            }
            else
            {
                rt_memset(context->sta_bssid, 0,
                          sizeof(context->sta_bssid));
            }
            rt_mutex_release(&context->radio.operation_lock);

            if (request_id)
            {
                if (connected)
                {
                    LOG_I("station connected request=%u after %u retries "
                          "bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                          request_id, retries, bssid[0], bssid[1], bssid[2],
                          bssid[3], bssid[4], bssid[5]);
                }
                else
                {
                    LOG_W("station connection request=%u rejected: %d",
                          request_id, wire->resp);
                }
                ehf_fg_report_connect_result(
                    context, request_id,
                    connected ? RT_EOK : -RT_ERROR, bssid);
            }
            else if (connected && !was_connected)
            {
                LOG_I("station connected without a pending host request "
                      "bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                      bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
                      bssid[5]);
            }
        }
        break;

    case CTRL_MSG_ID__Event_StationConnectedToESPSoftAP:
        if (message->event_station_connected_to_esp_softap)
        {
            const CtrlMsgEventStationConnectedToESPSoftAP *wire =
                message->event_station_connected_to_esp_softap;

            ehf_fg_report_station_event(context, RT_TRUE, &wire->mac,
                                        wire->aid, 0);
        }
        break;

    case CTRL_MSG_ID__Event_StationDisconnectFromESPSoftAP:
        if (message->event_station_disconnect_from_esp_softap)
        {
            const CtrlMsgEventStationDisconnectFromESPSoftAP *wire =
                message->event_station_disconnect_from_esp_softap;

            ehf_fg_report_station_event(context, RT_FALSE, &wire->mac,
                                        wire->aid, wire->reason);
        }
        break;

    default:
        break;
    }
}

static void ehf_fg_process_protobuf(struct ehf_context *context,
                                    const rt_uint8_t *data,
                                    rt_size_t length)
{
    CtrlMsg *message = ctrl_msg__unpack(RT_NULL, length, data);

    if (!message)
    {
        LOG_W("invalid control protobuf (%u bytes)", length);
        return;
    }
    if (message->msg_type == CTRL_MSG_TYPE__Resp)
    {
        rt_wlan_offload_command_complete(&context->commands,
                                    message->uid > 0 ? message->uid : 0,
                                    message->msg_id, RT_EOK,
                                    data, length);
    }
    else if (message->msg_type == CTRL_MSG_TYPE__Event)
    {
        ehf_fg_process_event_message(context, message);
    }
    ctrl_msg__free_unpacked(message, RT_NULL);
}

static void ehf_fg_process_serial_tlv(struct ehf_context *context,
                                      const rt_uint8_t *data,
                                      rt_size_t length)
{
    rt_uint16_t endpoint_length;
    rt_uint16_t protobuf_length;
    const rt_uint8_t *endpoint;
    rt_size_t data_header;

    if (length < EHF_FG_TLV_HEADER_LENGTH ||
        data[0] != EHF_FG_TLV_ENDPOINT_TYPE)
    {
        return;
    }
    endpoint_length = ehf_get_le16(data + 1);
    if (endpoint_length != EHF_FG_ENDPOINT_LENGTH ||
        3U + endpoint_length + 3U > length)
    {
        return;
    }
    endpoint = data + 3;
    if (rt_memcmp(endpoint, g_ehf_fg_response_endpoint,
                  EHF_FG_ENDPOINT_LENGTH) != 0 &&
        rt_memcmp(endpoint, g_ehf_fg_event_endpoint,
                  EHF_FG_ENDPOINT_LENGTH) != 0)
    {
        return;
    }
    data_header = 3U + endpoint_length;
    if (data[data_header] != EHF_FG_TLV_DATA_TYPE)
    {
        return;
    }
    protobuf_length = ehf_get_le16(data + data_header + 1);
    if (data_header + 3U + protobuf_length > length)
    {
        return;
    }
    ehf_fg_process_protobuf(context, data + data_header + 3,
                            protobuf_length);
}

static void ehf_fg_process_serial(struct ehf_context *context,
                                  const rt_uint8_t *data,
                                  rt_size_t length, rt_uint8_t flags,
                                  rt_uint16_t sequence)
{
    rt_uint8_t *resized;
    rt_bool_t more = (flags & EHF_FG_MORE_FRAGMENT) != 0;

    if (!more && !context->reassembly_active)
    {
        ehf_fg_process_serial_tlv(context, data, length);
        return;
    }
    if (context->reassembly_active &&
        sequence != context->reassembly_sequence)
    {
        rt_free(context->reassembly);
        context->reassembly = RT_NULL;
        context->reassembly_length = 0;
        context->reassembly_active = RT_FALSE;
    }
    if (context->reassembly_length + length >
        ESP_HOSTED_WIFI_CONTROL_BUFFER_SIZE)
    {
        rt_free(context->reassembly);
        context->reassembly = RT_NULL;
        context->reassembly_length = 0;
        context->reassembly_active = RT_FALSE;
        return;
    }
    resized = rt_realloc(context->reassembly,
                         context->reassembly_length + length);
    if (!resized)
    {
        rt_free(context->reassembly);
        context->reassembly = RT_NULL;
        context->reassembly_length = 0;
        context->reassembly_active = RT_FALSE;
        return;
    }
    context->reassembly = resized;
    rt_memcpy(context->reassembly + context->reassembly_length,
              data, length);
    context->reassembly_length += length;
    context->reassembly_sequence = sequence;
    context->reassembly_active = more;
    if (!more)
    {
        ehf_fg_process_serial_tlv(context, context->reassembly,
                                  context->reassembly_length);
        rt_free(context->reassembly);
        context->reassembly = RT_NULL;
        context->reassembly_length = 0;
    }
}

static void ehf_fg_process_boot(struct ehf_context *context,
                                const rt_uint8_t *data, rt_size_t length)
{
    const struct ehf_fg_private_event *event;
    rt_uint8_t capabilities = 0;
    rt_uint8_t chip_id = 0;
    rt_uint8_t version[8] = {0};
    const rt_uint8_t *position;
    rt_size_t remaining;

    if (length < sizeof(*event))
    {
        return;
    }
    event = (const struct ehf_fg_private_event *)data;
    if (event->event_type != 0 ||
        sizeof(*event) + event->event_length > length)
    {
        return;
    }
    position = event->data;
    remaining = event->event_length;
    while (remaining >= 2)
    {
        rt_uint8_t tag = position[0];
        rt_uint8_t tag_length = position[1];

        if ((rt_size_t)tag_length + 2 > remaining)
        {
            break;
        }
        if (tag == EHF_FG_PRIVATE_CAPABILITY && tag_length >= 1)
        {
            capabilities = position[2];
        }
        else if (tag == EHF_FG_PRIVATE_CHIP_ID && tag_length >= 1)
        {
            chip_id = position[2];
        }
        else if (tag == EHF_FG_PRIVATE_FIRMWARE_DATA && tag_length >= 8)
        {
            rt_memcpy(version, position + 2, sizeof(version));
        }
        else if (tag == EHF_FG_PRIVATE_RX_BUFFER_CONFIG &&
                 tag_length >= 5 && position[2] == 0 && position[6])
        {
            context->sdio_token_size = position[6] * 512U;
        }
        position += tag_length + 2;
        remaining -= tag_length + 2;
    }
    ehf_boot_ready(context, capabilities, chip_id, version);
}

static void ehf_fg_process_frame(struct ehf_context *context,
                                 const struct ehf_fg_transport_header *header,
                                 const rt_uint8_t *payload,
                                 rt_size_t payload_length)
{
    rt_uint8_t interface = header->interface_number & 0x0f;

    if (interface == EHF_FG_STA_INTERFACE ||
        interface == EHF_FG_AP_INTERFACE)
    {
        rt_wlan_offload_rx(&context->radio,
                      interface == EHF_FG_AP_INTERFACE ?
                          RT_WLAN_OFFLOAD_IFTYPE_AP : RT_WLAN_OFFLOAD_IFTYPE_STATION,
                      payload, payload_length);
    }
    else if (interface == EHF_FG_SERIAL_INTERFACE)
    {
        ehf_fg_process_serial(context, payload, payload_length,
                              header->flags,
                              ehf_get_le16(header->sequence));
    }
    else if (interface == EHF_FG_PRIVATE_INTERFACE &&
             header->private_type == EHF_FG_PRIVATE_EVENT)
    {
        ehf_fg_process_boot(context, payload, payload_length);
    }
}

static rt_err_t ehf_fg_receive(struct ehf_context *context, const void *data,
                               rt_size_t length)
{
    const rt_uint8_t *bytes = data;
    rt_size_t position = 0;
    rt_bool_t processed = RT_FALSE;
    rt_bool_t malformed = RT_FALSE;

    if (!context || !data || length < sizeof(struct ehf_fg_transport_header))
    {
        return -RT_EINVAL;
    }

    while (position + sizeof(struct ehf_fg_transport_header) <= length)
    {
        const struct ehf_fg_transport_header *header =
            (const struct ehf_fg_transport_header *)(bytes + position);
        rt_uint16_t payload_length = ehf_get_le16(header->length);
        rt_uint16_t payload_offset = ehf_get_le16(header->offset);
        rt_size_t frame_length;

        if (!payload_length && !payload_offset)
        {
            break;
        }
        if ((header->interface_number & 0x0f) >= EHF_FG_INTERFACE_MAX ||
            payload_offset < sizeof(*header) || payload_offset > 16U)
        {
            if (!context->invalid_rx_log_count)
            {
                LOG_W("dropping malformed frame: interface=%u length=%u "
                      "offset=%u available=%u",
                      header->interface_number & 0x0f, payload_length,
                      payload_offset, (unsigned int)(length - position));
            }
            context->invalid_rx_log_count = 1;
            malformed = RT_TRUE;
            break;
        }
        frame_length = payload_offset + payload_length;
        if (frame_length > length - position)
        {
            if (!context->invalid_rx_log_count)
            {
                LOG_W("dropping oversized frame: length=%u offset=%u "
                      "available=%u",
                      payload_length, payload_offset,
                      (unsigned int)(length - position));
            }
            context->invalid_rx_log_count = 1;
            malformed = RT_TRUE;
            break;
        }
        if (context->checksum_enabled &&
            ehf_get_le16(header->checksum) !=
                ehf_fg_frame_checksum(bytes + position, frame_length))
        {
            if (!context->invalid_rx_log_count)
            {
                LOG_W("dropping frame with invalid checksum: received=%u "
                      "calculated=%u",
                      ehf_get_le16(header->checksum),
                      ehf_fg_frame_checksum(bytes + position, frame_length));
            }
            context->invalid_rx_log_count = 1;
            malformed = RT_TRUE;
        }
        else
        {
            ehf_fg_process_frame(context, header,
                                 bytes + position + payload_offset,
                                 payload_length);
            context->invalid_rx_log_count = 0;
            processed = RT_TRUE;
        }
        position += RT_ALIGN(frame_length, EHF_TRANSPORT_ALIGNMENT);
        if (context->bus->type == RT_WLAN_OFFLOAD_BUS_SPI)
        {
            break;
        }
    }
    if (malformed)
    {
        return -RT_EIO;
    }
    return processed ? RT_EOK : -RT_EEMPTY;
}

static void ehf_fg_reset(struct ehf_context *context)
{
    if (context->connect_timer_initialized)
    {
        rt_timer_stop(&context->connect_timer);
    }
    if (context->reassembly)
    {
        rt_free(context->reassembly);
    }
    context->reassembly = RT_NULL;
    context->reassembly_length = 0;
    context->reassembly_active = RT_FALSE;
    context->scan_request_id = 0;
    context->connect_request_id = 0;
    context->connect_retry_count = 0;
    rt_memset(context->sta_bssid, 0, sizeof(context->sta_bssid));
    context->sdio_token_size = ESP_HOSTED_WIFI_SDIO_TOKEN_SIZE;
}

static const struct rt_wlan_offload_ops g_ehf_fg_wlan_offload_ops = {
    .start = ehf_fg_start,
    .stop = ehf_fg_stop,
    .change_interface = ehf_fg_change_interface,
    .scan = ehf_fg_scan,
    .connect = ehf_fg_connect,
    .disconnect = ehf_fg_disconnect,
    .start_ap = ehf_fg_start_ap,
    .stop_ap = ehf_fg_stop_ap,
    .get_rssi = ehf_fg_get_rssi,
    .set_power_save = ehf_fg_set_power_save,
    .get_power_save = ehf_fg_get_power_save,
    .set_regulatory = ehf_fg_set_regulatory,
    .get_regulatory = ehf_fg_get_regulatory,
    .set_mac = ehf_fg_set_mac,
    .get_mac = ehf_fg_get_mac,
    .transmit = ehf_fg_transmit,
};

const struct ehf_protocol_ops g_ehf_fg_protocol = {
    .name = "ESP-Hosted-FG",
    .capabilities = RT_WLAN_OFFLOAD_CAP_STA | RT_WLAN_OFFLOAD_CAP_AP |
                    RT_WLAN_OFFLOAD_CAP_STA_AP_CONCURRENT |
                    RT_WLAN_OFFLOAD_CAP_POWER_SAVE |
                    RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD |
                    RT_WLAN_OFFLOAD_CAP_SAE_OFFLOAD,
    .max_pending_commands = 1,
    .wlan_offload_ops = &g_ehf_fg_wlan_offload_ops,
    .command_push = ehf_fg_command_push,
    .receive = ehf_fg_receive,
    .reset = ehf_fg_reset,
};
