/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_wifi.h"
#include "esp_hosted_country.h"
#include "esp_hosted_ng_protocol.h"

#define DBG_TAG "esp_hosted.wifi.ng"
#define DBG_LVL ESP_HOSTED_WIFI_DBG_LVL
#include <rtdbg.h>

#define EHF_NG_COMMAND_TIMEOUT \
    rt_tick_from_millisecond(ESP_HOSTED_WIFI_COMMAND_TIMEOUT_MS)
#define EHF_NG_BEACON_FIXED_LENGTH 12U
#define EHF_NG_STA_FLAG_AUTHORIZED 1U
#define EHF_NG_MAX_AP_STATIONS     8U

static rt_uint8_t ehf_ng_interface_from_iftype(enum rt_wlan_offload_iftype iftype)
{
    return iftype == RT_WLAN_OFFLOAD_IFTYPE_AP ? EHF_NG_AP_INTERFACE :
                                            EHF_NG_STA_INTERFACE;
}

static void ehf_ng_channel_definition(
    rt_uint8_t channel, struct rt_wlan_offload_channel_definition *definition)
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

static rt_uint16_t ehf_ng_frame_checksum(const rt_uint8_t *frame,
                                         rt_size_t length)
{
    rt_uint16_t result = 0;
    rt_size_t index;

    for (index = 0; index < length; index++)
    {
        if (index != 8 && index != 9)
        {
            result += frame[index];
        }
    }
    return result;
}

static rt_err_t ehf_ng_send(struct ehf_context *context,
                            rt_uint8_t interface,
                            enum ehf_ng_packet_type packet_type,
                            const void *payload, rt_size_t payload_length)
{
    struct ehf_ng_transport_header *header;
    rt_uint8_t *frame;
    rt_size_t frame_length = sizeof(*header) + payload_length;
    rt_err_t result;

    if ((payload_length && !payload) || frame_length > context->bus->max_tx_size ||
        payload_length > 0xffffU)
    {
        return -RT_EINVAL;
    }
    frame = rt_calloc(1, frame_length);
    if (!frame)
    {
        return -RT_ENOMEM;
    }
    header = (struct ehf_ng_transport_header *)frame;
    header->interface_number = interface & 0x0f;
    header->packet_type = packet_type;
    ehf_put_le16(header->length, payload_length);
    ehf_put_le16(header->offset, sizeof(*header));
    if (payload_length)
    {
        rt_memcpy(frame + sizeof(*header), payload, payload_length);
    }
    if (context->checksum_enabled)
    {
        ehf_put_le16(header->checksum,
                     ehf_ng_frame_checksum(frame, frame_length));
    }
    result = packet_type == EHF_NG_PACKET_COMMAND_REQUEST ?
        rt_wlan_offload_bus_transmit_priority(context->bus,
                                         RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL,
                                         frame, frame_length) :
        rt_wlan_offload_bus_transmit(context->bus, frame, frame_length);
    rt_free(frame);
    return result;
}

static rt_err_t ehf_ng_command_push(
    struct rt_wlan_offload_command_manager *manager, rt_uint32_t token,
    rt_uint16_t command_id, const void *request, rt_size_t request_length,
    void *driver_data)
{
    struct ehf_context *context = driver_data;
    struct ehf_ng_command_header *header;
    rt_uint8_t *payload;
    rt_size_t payload_length = sizeof(*header) + request_length;
    rt_err_t result;

    (void)manager;
    (void)token;
    payload = rt_calloc(1, payload_length);
    if (!payload)
    {
        return -RT_ENOMEM;
    }
    header = (struct ehf_ng_command_header *)payload;
    header->command = command_id;
    if (request_length)
    {
        rt_memcpy(payload + sizeof(*header), request, request_length);
    }
    result = ehf_ng_send(context, context->command_interface,
                         EHF_NG_PACKET_COMMAND_REQUEST,
                         payload, payload_length);
    rt_free(payload);
    return result;
}

static rt_err_t ehf_ng_execute(struct ehf_context *context,
                               rt_uint8_t interface, rt_uint16_t command,
                               const void *request, rt_size_t request_length,
                               void *response, rt_size_t response_capacity,
                               rt_size_t *response_length)
{
    context->command_interface = interface;
    return rt_wlan_offload_command_execute(
        &context->commands, command, command, request, request_length,
        response, response_capacity, response_length,
        EHF_NG_COMMAND_TIMEOUT, RT_NULL);
}

static rt_err_t ehf_ng_start(struct rt_wlan_offload_radio *radio)
{
    struct ehf_context *context = ehf_context_from_radio(radio);

    rt_wlan_offload_command_manager_reset(&context->commands);
    return ehf_wait_for_boot(context);
}

static rt_err_t ehf_ng_stop(struct rt_wlan_offload_radio *radio)
{
    struct ehf_context *context = ehf_context_from_radio(radio);

    context->sta_enabled = RT_FALSE;
    context->ap_enabled = RT_FALSE;
    context->sta_connected = RT_FALSE;
    context->ap_started = RT_FALSE;
    context->scan_request_id = 0;
    context->auth_request_id = 0;
    context->assoc_request_id = 0;
    rt_memset(context->sta_bssid, 0, sizeof(context->sta_bssid));
    rt_wlan_offload_command_manager_fail(&context->commands, -RT_EIO);
    return RT_EOK;
}

static rt_err_t ehf_ng_change_interface(struct rt_wlan_offload_vif *vif,
                                         enum rt_wlan_offload_iftype iftype,
                                         rt_bool_t enabled)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    rt_uint8_t interface = ehf_ng_interface_from_iftype(iftype);
    rt_err_t result;

    result = ehf_ng_execute(context, interface,
                            enabled ? EHF_NG_CMD_INIT_INTERFACE :
                                      EHF_NG_CMD_DEINIT_INTERFACE,
                            RT_NULL, 0, RT_NULL, 0, RT_NULL);
    if (result == RT_EOK)
    {
        if (iftype == RT_WLAN_OFFLOAD_IFTYPE_AP)
        {
            context->ap_enabled = enabled;
            if (!enabled)
            {
                context->ap_started = RT_FALSE;
            }
        }
        else
        {
            context->sta_enabled = enabled;
            if (!enabled)
            {
                context->sta_connected = RT_FALSE;
                context->auth_request_id = 0;
                context->assoc_request_id = 0;
                rt_memset(context->sta_bssid, 0,
                          sizeof(context->sta_bssid));
            }
        }
    }
    return result;
}

static rt_err_t ehf_ng_scan(struct rt_wlan_offload_vif *vif,
                            const struct rt_wlan_offload_scan_request *request)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_scan_body body;
    rt_err_t result;

    rt_memset(&body, 0, sizeof(body));
    if (request->ssid_count)
    {
        rt_memcpy(body.ssid, request->ssids[0].value,
                  request->ssids[0].length);
    }
    if (request->channel_count)
    {
        body.channel = request->channels[0].primary_channel;
    }
    rt_memcpy(body.bssid, request->bssid, sizeof(body.bssid));
    ehf_put_le16(body.duration, request->duration_ms);
    context->scan_request_id = request->request_id;
    result = ehf_ng_execute(context, EHF_NG_STA_INTERFACE,
                            EHF_NG_CMD_SCAN, &body, sizeof(body),
                            RT_NULL, 0, RT_NULL);
    if (result != RT_EOK)
    {
        context->scan_request_id = 0;
    }
    return result;
}

static rt_err_t ehf_ng_disconnect(struct rt_wlan_offload_vif *vif,
                                  rt_uint32_t request_id,
                                  rt_uint16_t reason)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_disconnect_body body;

    (void)request_id;
    rt_memset(&body, 0, sizeof(body));
    ehf_put_le16(body.reason, reason);
    rt_memcpy(body.mac, context->sta_bssid, sizeof(body.mac));
    return ehf_ng_execute(context, EHF_NG_STA_INTERFACE,
                          EHF_NG_CMD_DISCONNECT, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_err_t ehf_ng_ap_security(rt_wlan_security_t security,
                                    rt_uint8_t *auth_mode,
                                    rt_uint8_t *pairwise_cipher,
                                    rt_uint8_t *pmf)
{
    if (!auth_mode || !pairwise_cipher || !pmf)
    {
        return -RT_EINVAL;
    }
    *pmf = 0;
    switch (security)
    {
    case SECURITY_OPEN:
        *auth_mode = 0;
        *pairwise_cipher = EHF_NG_CIPHER_NONE;
        return RT_EOK;
    case SECURITY_WEP_PSK:
    case SECURITY_WEP_SHARED:
        *auth_mode = 1;
        *pairwise_cipher = EHF_NG_CIPHER_WEP40;
        return RT_EOK;
    case SECURITY_WPA_TKIP_PSK:
        *auth_mode = 2;
        *pairwise_cipher = EHF_NG_CIPHER_TKIP;
        return RT_EOK;
    case SECURITY_WPA_AES_PSK:
        *auth_mode = 2;
        *pairwise_cipher = EHF_NG_CIPHER_CCMP;
        return RT_EOK;
    case SECURITY_WPA2_TKIP_PSK:
        *auth_mode = 3;
        *pairwise_cipher = EHF_NG_CIPHER_TKIP;
        return RT_EOK;
    case SECURITY_WPA2_AES_PSK:
        *auth_mode = 3;
        *pairwise_cipher = EHF_NG_CIPHER_CCMP;
        return RT_EOK;
    case SECURITY_WPA2_MIXED_PSK:
        *auth_mode = 3;
        *pairwise_cipher = EHF_NG_CIPHER_TKIP_CCMP;
        return RT_EOK;
    case SECURITY_WPA2_AES_CMAC:
        *auth_mode = 3;
        *pairwise_cipher = EHF_NG_CIPHER_CCMP;
        *pmf = 1;
        return RT_EOK;
    case SECURITY_WPA_WPA2_MIXED_PSK:
        *auth_mode = 4;
        *pairwise_cipher = EHF_NG_CIPHER_TKIP_CCMP;
        return RT_EOK;
    case SECURITY_WPA3_AES_PSK:
    case SECURITY_WPA3_SAE:
        *auth_mode = 6;
        *pairwise_cipher = EHF_NG_CIPHER_CCMP;
        *pmf = 3;
        return RT_EOK;
    case SECURITY_WPA2_WPA3_MIXED_PSK:
        *auth_mode = 7;
        *pairwise_cipher = EHF_NG_CIPHER_CCMP;
        *pmf = 1;
        return RT_EOK;
    default:
        return -RT_ENOSYS;
    }
}

static rt_err_t ehf_ng_set_ie(struct ehf_context *context, rt_uint8_t type,
                              const rt_uint8_t *data, rt_size_t length)
{
    struct ehf_ng_ie_body *body;
    rt_size_t body_length = sizeof(*body) + length;
    rt_err_t result;

    if (length > 0xffffU)
    {
        return -RT_EINVAL;
    }
    body = rt_calloc(1, body_length);
    if (!body)
    {
        return -RT_ENOMEM;
    }
    body->type = type;
    ehf_put_le16(body->length, length);
    if (length)
    {
        rt_memcpy(body->data, data, length);
    }
    result = ehf_ng_execute(context, EHF_NG_AP_INTERFACE,
                            EHF_NG_CMD_SET_IE, body, body_length,
                            RT_NULL, 0, RT_NULL);
    rt_free(body);
    return result;
}

static rt_err_t ehf_ng_start_ap(struct rt_wlan_offload_vif *vif,
                                const struct rt_wlan_offload_ap_settings *settings)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_ap_config_body body;
    struct rt_wlan_offload_event event;
    rt_uint8_t auth_mode;
    rt_uint8_t pairwise_cipher;
    rt_uint8_t pmf;
    rt_err_t result;

    result = ehf_ng_ap_security(settings->security, &auth_mode,
                                &pairwise_cipher, &pmf);
    if (result != RT_EOK)
    {
        return result;
    }
    if (settings->beacon_ies_length)
    {
        result = ehf_ng_set_ie(context, EHF_NG_IE_BEACON,
                               settings->beacon_ies,
                               settings->beacon_ies_length);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    rt_memset(&body, 0, sizeof(body));
    rt_memcpy(body.ssid, settings->ssid.val, settings->ssid.len);
    body.ssid_length = settings->ssid.len;
    body.channel = settings->channel.primary_channel;
    body.auth_mode = auth_mode;
    body.hidden = settings->hidden;
    body.max_connections = settings->max_stations ?
        settings->max_stations : EHF_NG_MAX_AP_STATIONS;
    if (body.max_connections > EHF_NG_MAX_AP_STATIONS)
    {
        body.max_connections = EHF_NG_MAX_AP_STATIONS;
    }
    body.pairwise_cipher = pairwise_cipher;
    body.pmf = pmf;
    body.privacy = settings->security != SECURITY_OPEN;
    ehf_put_le16(body.beacon_interval,
                 settings->beacon_interval ? settings->beacon_interval : 100);
    result = ehf_ng_execute(context, EHF_NG_AP_INTERFACE,
                            EHF_NG_CMD_AP_CONFIG, &body, sizeof(body),
                            RT_NULL, 0, RT_NULL);
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
    return rt_wlan_offload_report_event(&context->radio, &event);
}

static rt_err_t ehf_ng_stop_ap(struct rt_wlan_offload_vif *vif,
                               rt_uint32_t request_id)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_mode_body body;
    struct rt_wlan_offload_event event;
    rt_err_t result;

    if (context->sta_enabled)
    {
        rt_memset(&body, 0, sizeof(body));
        ehf_put_le16(body.mode, EHF_NG_WIFI_MODE_STA);
        result = ehf_ng_execute(context, EHF_NG_AP_INTERFACE,
                                EHF_NG_CMD_SET_MODE, &body, sizeof(body),
                                RT_NULL, 0, RT_NULL);
    }
    else
    {
        result = ehf_ng_execute(context, EHF_NG_AP_INTERFACE,
                                EHF_NG_CMD_DEINIT_INTERFACE,
                                RT_NULL, 0, RT_NULL, 0, RT_NULL);
    }
    if (result != RT_EOK)
    {
        return result;
    }
    context->ap_started = RT_FALSE;
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_AP_STOPPED;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
    event.request_id = request_id;
    event.status = RT_EOK;
    return rt_wlan_offload_report_event(&context->radio, &event);
}

static rt_err_t ehf_ng_del_station(struct rt_wlan_offload_vif *vif,
                                   rt_uint32_t request_id,
                                   const rt_uint8_t mac[6],
                                   rt_uint16_t reason)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_disconnect_body body;

    (void)request_id;
    rt_memset(&body, 0, sizeof(body));
    ehf_put_le16(body.reason, reason);
    if (mac)
    {
        rt_memcpy(body.mac, mac, sizeof(body.mac));
    }
    return ehf_ng_execute(context, EHF_NG_AP_INTERFACE,
                          EHF_NG_CMD_DISCONNECT, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_bool_t ehf_ng_find_ie(const rt_uint8_t *ies, rt_size_t length,
                                rt_uint8_t id, const rt_uint8_t **body,
                                rt_size_t *body_length)
{
    rt_size_t offset = 0;

    while (ies && offset + 2U <= length)
    {
        rt_size_t size = ies[offset + 1U];

        if (offset + 2U + size > length)
        {
            return RT_FALSE;
        }
        if (ies[offset] == id)
        {
            if (body)
            {
                *body = ies + offset + 2U;
            }
            if (body_length)
            {
                *body_length = size;
            }
            return RT_TRUE;
        }
        offset += 2U + size;
    }
    return RT_FALSE;
}

static rt_bool_t ehf_ng_find_extension_ie(
    const rt_uint8_t *ies, rt_size_t length, rt_uint8_t extension_id,
    const rt_uint8_t **body, rt_size_t *body_length)
{
    rt_size_t offset = 0;

    while (ies && offset + 3U <= length)
    {
        rt_size_t size = ies[offset + 1U];

        if (offset + 2U + size > length)
        {
            return RT_FALSE;
        }
        if (ies[offset] == 255U && size >= 1U &&
            ies[offset + 2U] == extension_id)
        {
            if (body)
            {
                *body = ies + offset + 3U;
            }
            if (body_length)
            {
                *body_length = size - 1U;
            }
            return RT_TRUE;
        }
        offset += 2U + size;
    }
    return RT_FALSE;
}

static void ehf_ng_copy_supported_rates(
    rt_uint8_t output[12], const rt_uint8_t *ies, rt_size_t ies_length)
{
    const rt_uint8_t *body;
    rt_size_t body_length;
    rt_size_t copied = 0;
    const rt_uint8_t ids[] = {1U, 50U};
    rt_size_t index;

    for (index = 0; index < sizeof(ids) / sizeof(ids[0]); index++)
    {
        rt_size_t count;

        if (!ehf_ng_find_ie(ies, ies_length, ids[index], &body,
                            &body_length))
        {
            continue;
        }
        count = body_length;
        if (count > 10U - copied)
        {
            count = 10U - copied;
        }
        rt_memcpy(output + 2U + copied, body, count);
        copied += count;
        if (copied == 10U)
        {
            break;
        }
    }
    if (copied)
    {
        output[0] = 1U;
        output[1] = (rt_uint8_t)copied;
    }
}

static rt_err_t ehf_ng_add_station(
    struct rt_wlan_offload_vif *vif, rt_uint32_t request_id,
    const struct rt_wlan_offload_station_parameters *station)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_ap_station_body body;
    const rt_uint8_t *capability;
    rt_size_t capability_length;

    (void)request_id;
    if (!context || !station || !context->ap_started)
    {
        return -RT_EINVAL;
    }
    rt_memset(&body, 0, sizeof(body));
    rt_memcpy(body.mac, station->mac, sizeof(body.mac));
    ehf_put_le16(body.command, EHF_NG_AP_STATION_ADD);
    ehf_put_le16(body.aid, station->aid);
    ehf_ng_copy_supported_rates(body.supported_rates,
                                station->association_ies,
                                station->association_ies_length);

    if (ehf_ng_find_ie(station->association_ies,
                       station->association_ies_length, 127U,
                       &capability, &capability_length))
    {
        if (capability_length > sizeof(body.extended_capabilities))
        {
            capability_length = sizeof(body.extended_capabilities);
        }
        rt_memcpy(body.extended_capabilities, capability, capability_length);
    }
    if (ehf_ng_find_ie(station->association_ies,
                       station->association_ies_length, 45U,
                       &capability, &capability_length) &&
        capability_length >= 26U)
    {
        body.ht_capabilities[0] = 45U;
        body.ht_capabilities[1] = 26U;
        rt_memcpy(body.ht_capabilities + 2U, capability, 26U);
    }
    if (ehf_ng_find_ie(station->association_ies,
                       station->association_ies_length, 191U,
                       &capability, &capability_length) &&
        capability_length >= 12U)
    {
        body.vht_capabilities[0] = 191U;
        body.vht_capabilities[1] = 12U;
        rt_memcpy(body.vht_capabilities + 2U, capability, 12U);
    }
    if (ehf_ng_find_extension_ie(
            station->association_ies, station->association_ies_length,
            35U, &capability, &capability_length) &&
        capability_length >= 24U)
    {
        body.he_capabilities[0] = 255U;
        body.he_capabilities[1] = 25U;
        body.he_capabilities[2] = 35U;
        rt_memcpy(body.he_capabilities + 3U, capability, 24U);
    }
    return ehf_ng_execute(context, EHF_NG_AP_INTERFACE,
                          EHF_NG_CMD_AP_STATION, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_err_t ehf_ng_set_station_authorized(
    struct rt_wlan_offload_vif *vif, rt_uint32_t request_id,
    const rt_uint8_t mac[6], rt_bool_t authorized)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_ap_station_body body;
    rt_uint32_t authorized_flag = 1UL << EHF_NG_STA_FLAG_AUTHORIZED;

    (void)request_id;
    if (!context || !mac || !context->ap_started)
    {
        return -RT_EINVAL;
    }
    rt_memset(&body, 0, sizeof(body));
    rt_memcpy(body.mac, mac, sizeof(body.mac));
    ehf_put_le16(body.command, EHF_NG_AP_STATION_CHANGE);
    ehf_put_le32(body.flags_mask, authorized_flag);
    if (authorized)
    {
        ehf_put_le32(body.flags_set, authorized_flag);
    }
    return ehf_ng_execute(context, EHF_NG_AP_INTERFACE,
                          EHF_NG_CMD_AP_STATION, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_err_t ehf_ng_get_rssi(struct rt_wlan_offload_vif *vif, int *rssi)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    rt_uint8_t response[4] = {0};
    rt_size_t response_length = 0;
    rt_err_t result;

    result = ehf_ng_execute(context, EHF_NG_STA_INTERFACE,
                            EHF_NG_CMD_GET_RSSI, RT_NULL, 0,
                            response, sizeof(response), &response_length);
    if (result == RT_EOK && response_length >= 1)
    {
        *rssi = (rt_int8_t)response[0];
    }
    else if (result == RT_EOK)
    {
        result = -RT_EIO;
    }
    return result;
}

static rt_err_t ehf_ng_set_regulatory(struct rt_wlan_offload_radio *radio,
                                      rt_country_code_t country)
{
    struct ehf_context *context = ehf_context_from_radio(radio);
    struct ehf_ng_reg_domain_body body;
    const char *alpha2 = ehf_country_code_from_rt(country);

    if (!alpha2)
    {
        return -RT_EINVAL;
    }
    rt_memset(&body, 0, sizeof(body));
    body.country[0] = alpha2[0];
    body.country[1] = alpha2[1];
    return ehf_ng_execute(context, EHF_NG_STA_INTERFACE,
                          EHF_NG_CMD_SET_REG_DOMAIN, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_err_t ehf_ng_get_regulatory(struct rt_wlan_offload_radio *radio,
                                      rt_country_code_t *country)
{
    struct ehf_context *context = ehf_context_from_radio(radio);
    struct ehf_ng_reg_domain_body response;
    rt_size_t response_length = 0;
    rt_err_t result;

    result = ehf_ng_execute(context, EHF_NG_STA_INTERFACE,
                            EHF_NG_CMD_GET_REG_DOMAIN, RT_NULL, 0,
                            &response, sizeof(response), &response_length);
    if (result == RT_EOK && response_length >= sizeof(response))
    {
        *country = ehf_country_code_to_rt(
            (const rt_uint8_t *)response.country, 2);
        if (*country == RT_COUNTRY_UNKNOWN)
        {
            result = -RT_EINVAL;
        }
    }
    else if (result == RT_EOK)
    {
        result = -RT_EIO;
    }
    return result;
}

static rt_err_t ehf_ng_set_mac(struct rt_wlan_offload_vif *vif, rt_uint8_t mac[6])
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_mac_body body;

    rt_memset(&body, 0, sizeof(body));
    rt_memcpy(body.mac, mac, sizeof(body.mac));
    return ehf_ng_execute(context,
                          ehf_ng_interface_from_iftype(vif->iftype),
                          EHF_NG_CMD_SET_MAC, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_err_t ehf_ng_get_mac(struct rt_wlan_offload_vif *vif, rt_uint8_t mac[6])
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_mac_body response;
    rt_size_t response_length = 0;
    rt_err_t result;

    result = ehf_ng_execute(context,
                            ehf_ng_interface_from_iftype(vif->iftype),
                            EHF_NG_CMD_GET_MAC, RT_NULL, 0,
                            &response, sizeof(response), &response_length);
    if (result == RT_EOK && response_length >= 6)
    {
        rt_memcpy(mac, response.mac, 6);
        rt_memcpy(vif->address, response.mac, 6);
    }
    else if (result == RT_EOK)
    {
        result = -RT_EIO;
    }
    return result;
}

static rt_err_t ehf_ng_transmit(struct rt_wlan_offload_vif *vif,
                                const void *data, int length)
{
    struct ehf_context *context = ehf_context_from_vif(vif);

    if (length <= 0)
    {
        return -RT_EINVAL;
    }
    return ehf_ng_send(context, ehf_ng_interface_from_iftype(vif->iftype),
                       EHF_NG_PACKET_DATA, data, length);
}

static rt_err_t ehf_ng_auth_type_to_wire(
    enum rt_wlan_offload_auth_type auth_type, rt_uint8_t *wire)
{
    if (!wire)
    {
        return -RT_EINVAL;
    }
    switch (auth_type)
    {
    case RT_WLAN_OFFLOAD_AUTH_OPEN: *wire = EHF_NG_AUTH_OPEN; return RT_EOK;
    case RT_WLAN_OFFLOAD_AUTH_SHARED: *wire = EHF_NG_AUTH_SHARED; return RT_EOK;
    case RT_WLAN_OFFLOAD_AUTH_FT: *wire = EHF_NG_AUTH_FT; return RT_EOK;
    case RT_WLAN_OFFLOAD_AUTH_SAE: *wire = EHF_NG_AUTH_SAE; return RT_EOK;
    case RT_WLAN_OFFLOAD_AUTH_AUTOMATIC: *wire = EHF_NG_AUTH_AUTOMATIC; return RT_EOK;
    default: return -RT_ENOSYS;
    }
}

static rt_err_t ehf_ng_auth(struct rt_wlan_offload_vif *vif,
                            const struct rt_wlan_offload_auth_request *request)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_auth_body *body;
    rt_size_t body_length = sizeof(*body) + request->auth_data_length;
    rt_uint8_t auth_type;
    rt_err_t result;

    result = ehf_ng_auth_type_to_wire(request->auth_type, &auth_type);
    if (result != RT_EOK)
    {
        return result;
    }
    if (request->auth_data_length > 0xffU)
    {
        return -RT_EINVAL;
    }
    body = rt_calloc(1, body_length);
    if (!body)
    {
        return -RT_ENOMEM;
    }
    rt_memcpy(body->bssid, request->bssid, sizeof(body->bssid));
    body->channel = request->channel.primary_channel;
    body->auth_type = auth_type;
    rt_memcpy(body->ssid, request->ssid.val, request->ssid.len);
    body->auth_data_length = request->auth_data_length;
    if (request->auth_data_length)
    {
        rt_memcpy(body->auth_data, request->auth_data,
                  request->auth_data_length);
    }
    context->auth_request_id = request->request_id;
    result = ehf_ng_execute(context, EHF_NG_STA_INTERFACE,
                            EHF_NG_CMD_AUTH, body, body_length,
                            RT_NULL, 0, RT_NULL);
    rt_free(body);
    if (result != RT_EOK)
    {
        context->auth_request_id = 0;
    }
    return result;
}

static rt_err_t ehf_ng_assoc(struct rt_wlan_offload_vif *vif,
                             const struct rt_wlan_offload_assoc_request *request)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_assoc_body *body;
    rt_size_t body_length = sizeof(*body) + request->ies_length;
    rt_err_t result;

    if (request->ies_length > 0xffU)
    {
        return -RT_EINVAL;
    }
    body = rt_calloc(1, body_length);
    if (!body)
    {
        return -RT_ENOMEM;
    }
    body->ie_length = request->ies_length;
    if (request->ies_length)
    {
        rt_memcpy(body->ies, request->ies, request->ies_length);
    }
    context->assoc_request_id = request->request_id;
    result = ehf_ng_execute(context, EHF_NG_STA_INTERFACE,
                            EHF_NG_CMD_ASSOC, body, body_length,
                            RT_NULL, 0, RT_NULL);
    rt_free(body);
    if (result != RT_EOK)
    {
        context->assoc_request_id = 0;
    }
    return result;
}

static rt_uint32_t ehf_ng_key_algorithm(enum rt_wlan_offload_cipher cipher)
{
    switch (cipher)
    {
    case RT_WLAN_OFFLOAD_CIPHER_WEP40: return 1;
    case RT_WLAN_OFFLOAD_CIPHER_TKIP: return 2;
    case RT_WLAN_OFFLOAD_CIPHER_CCMP: return 3;
    case RT_WLAN_OFFLOAD_CIPHER_WEP104: return 5;
    case RT_WLAN_OFFLOAD_CIPHER_AES_CMAC: return 7;
    case RT_WLAN_OFFLOAD_CIPHER_GCMP:
    case RT_WLAN_OFFLOAD_CIPHER_GCMP_256: return 9;
    default: return 0;
    }
}

static void ehf_ng_fill_key(struct ehf_ng_security_key *output,
                            const struct rt_wlan_offload_key *key)
{
    rt_memset(output, 0, sizeof(*output));
    ehf_put_le32(output->algorithm, ehf_ng_key_algorithm(key->cipher));
    ehf_put_le32(output->index, key->index);
    ehf_put_le32(output->length, key->key_length);
    ehf_put_le32(output->sequence_length, key->sequence_length);
    rt_memcpy(output->data, key->key, key->key_length);
    rt_memcpy(output->sequence, key->sequence, key->sequence_length);
    if (key->pairwise)
    {
        rt_memcpy(output->mac, key->peer, 6);
    }

    if (key->cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP && key->key_length >= 32)
    {
        rt_uint8_t mic[8];

        rt_memcpy(mic, &output->data[16], sizeof(mic));
        rt_memcpy(&output->data[16], &output->data[24], sizeof(mic));
        rt_memcpy(&output->data[24], mic, sizeof(mic));
    }
}

static rt_err_t ehf_ng_add_key(struct rt_wlan_offload_vif *vif,
                               rt_uint32_t request_id,
                               const struct rt_wlan_offload_key *key)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_security_key body;

    (void)request_id;
    if (!ehf_ng_key_algorithm(key->cipher) ||
        key->key_length > sizeof(body.data) ||
        key->sequence_length > sizeof(body.sequence))
    {
        return -RT_EINVAL;
    }
    ehf_ng_fill_key(&body, key);
    return ehf_ng_execute(context,
                          ehf_ng_interface_from_iftype(vif->iftype),
                          EHF_NG_CMD_ADD_KEY, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_err_t ehf_ng_delete_key(struct rt_wlan_offload_vif *vif,
                                  rt_uint32_t request_id, rt_uint8_t index,
                                  rt_bool_t pairwise,
                                  const rt_uint8_t peer[6])
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_security_key body;

    (void)request_id;
    rt_memset(&body, 0, sizeof(body));
    ehf_put_le32(body.index, index);
    if (pairwise && peer)
    {
        rt_memcpy(body.mac, peer, 6);
    }
    body.delete_key = 1;
    return ehf_ng_execute(context,
                          ehf_ng_interface_from_iftype(vif->iftype),
                          EHF_NG_CMD_DELETE_KEY, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_err_t ehf_ng_set_default_key(struct rt_wlan_offload_vif *vif,
                                       rt_uint32_t request_id,
                                       rt_uint8_t index, rt_bool_t unicast,
                                       rt_bool_t multicast)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_security_key body;

    (void)request_id;
    (void)unicast;
    (void)multicast;
    rt_memset(&body, 0, sizeof(body));
    ehf_put_le32(body.index, index);
    body.set_current = 1;
    return ehf_ng_execute(context,
                          ehf_ng_interface_from_iftype(vif->iftype),
                          EHF_NG_CMD_SET_DEFAULT_KEY, &body, sizeof(body),
                          RT_NULL, 0, RT_NULL);
}

static rt_err_t ehf_ng_transmit_mgmt(
    struct rt_wlan_offload_vif *vif, const struct rt_wlan_offload_mgmt_frame *frame)
{
    struct ehf_context *context = ehf_context_from_vif(vif);
    struct ehf_ng_mgmt_body *body;
    struct rt_wlan_offload_event event;
    rt_size_t body_length = sizeof(*body) + frame->length;
    rt_err_t result;

    body = rt_calloc(1, body_length);
    if (!body)
    {
        return -RT_ENOMEM;
    }
    body->channel = frame->channel.primary_channel;
    body->off_channel = frame->off_channel;
    ehf_put_le32(body->wait, frame->wait_ms);
    ehf_put_le32(body->length, frame->length);
    rt_memcpy(body->data, frame->data, frame->length);
    result = ehf_ng_execute(context,
                            ehf_ng_interface_from_iftype(vif->iftype),
                            EHF_NG_CMD_MGMT_TX, body, body_length,
                            RT_NULL, 0, RT_NULL);
    rt_free(body);

    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_MGMT_TX_STATUS;
    event.iftype = vif->iftype;
    event.request_id = frame->request_id;
    event.status = RT_EOK;
    event.data.tx_status.cookie = frame->cookie;
    event.data.tx_status.acknowledged = result == RT_EOK;
    event.data.tx_status.data = frame->data;
    event.data.tx_status.length = frame->length;
    rt_wlan_offload_report_event(&context->radio, &event);
    return result == RT_EOK ? RT_EOK : result;
}

static rt_err_t ehf_ng_response_status(rt_uint8_t status)
{
    switch (status)
    {
    case EHF_NG_RESPONSE_SUCCESS: return RT_EOK;
    case EHF_NG_RESPONSE_BUSY: return -RT_EBUSY;
    case EHF_NG_RESPONSE_UNSUPPORTED: return -RT_ENOSYS;
    case EHF_NG_RESPONSE_INVALID: return -RT_EINVAL;
    default: return -RT_ERROR;
    }
}

static void ehf_ng_process_response(struct ehf_context *context,
                                    const rt_uint8_t *payload,
                                    rt_size_t length)
{
    const struct ehf_ng_command_header *header;
    rt_size_t response_length;

    if (length < sizeof(*header))
    {
        return;
    }
    header = (const struct ehf_ng_command_header *)payload;
    response_length = ehf_get_le16(header->length);
    if (response_length > length - sizeof(*header))
    {
        LOG_W("truncated command response %u: %u > %u",
              header->command, (unsigned int)response_length,
              (unsigned int)(length - sizeof(*header)));
        return;
    }
    rt_wlan_offload_command_complete(
        &context->commands, 0, header->command,
        ehf_ng_response_status(header->status),
        payload + sizeof(*header), response_length);
}

struct ehf_ng_security_suites
{
    rt_bool_t group_tkip;
    rt_bool_t group_aes;
    rt_bool_t tkip;
    rt_bool_t aes;
    rt_bool_t psk;
    rt_bool_t psk_sha256;
    rt_bool_t ft_psk;
    rt_bool_t psk_sha384;
    rt_bool_t ft_psk_sha384;
    rt_bool_t sae;
    rt_bool_t ft_sae;
    rt_bool_t sae_ext_key;
    rt_bool_t ft_sae_ext_key;
    rt_bool_t enterprise;
    rt_bool_t enterprise_sha256;
    rt_bool_t enterprise_sha384;
    rt_bool_t ft_enterprise;
    rt_bool_t suite_b;
    rt_bool_t suite_b_192;
    rt_bool_t ft_enterprise_sha384;
    rt_bool_t fils_sha256;
    rt_bool_t fils_sha384;
    rt_bool_t ft_fils_sha256;
    rt_bool_t ft_fils_sha384;
    rt_bool_t owe;
    rt_bool_t dpp;
    rt_bool_t cckm;
};

static void ehf_ng_parse_akm_suite(struct ehf_ng_security_suites *suites,
                                   const rt_uint8_t suite[4], rt_bool_t rsn)
{
    static const rt_uint8_t rsn_oui[3] = {0x00, 0x0f, 0xac};
    static const rt_uint8_t wpa_oui[3] = {0x00, 0x50, 0xf2};
    static const rt_uint8_t wfa_oui[3] = {0x50, 0x6f, 0x9a};
    static const rt_uint8_t cisco_oui[3] = {0x00, 0x40, 0x96};
    rt_uint8_t type;

    if (!suites || !suite)
    {
        return;
    }
    type = suite[3];
    if (!rsn && rt_memcmp(suite, wpa_oui, sizeof(wpa_oui)) == 0)
    {
        suites->enterprise |= type == 1;
        suites->psk |= type == 2;
        return;
    }
    if (rsn && rt_memcmp(suite, wfa_oui, sizeof(wfa_oui)) == 0)
    {
        suites->dpp |= type == 2;
        return;
    }
    if (rsn && rt_memcmp(suite, cisco_oui, sizeof(cisco_oui)) == 0)
    {
        suites->cckm |= type == 0;
        return;
    }
    if (!rsn || rt_memcmp(suite, rsn_oui, sizeof(rsn_oui)) != 0)
    {
        return;
    }

    switch (type)
    {
    case 1: suites->enterprise = RT_TRUE; break;
    case 2: suites->psk = RT_TRUE; break;
    case 3: suites->ft_enterprise = RT_TRUE; break;
    case 4: suites->ft_psk = RT_TRUE; break;
    case 5: suites->enterprise_sha256 = RT_TRUE; break;
    case 6: suites->psk_sha256 = RT_TRUE; break;
    case 8: suites->sae = RT_TRUE; break;
    case 9: suites->ft_sae = RT_TRUE; break;
    case 11: suites->suite_b = RT_TRUE; break;
    case 12: suites->suite_b_192 = RT_TRUE; break;
    case 13:
    case 22: suites->ft_enterprise_sha384 = RT_TRUE; break;
    case 14: suites->fils_sha256 = RT_TRUE; break;
    case 15: suites->fils_sha384 = RT_TRUE; break;
    case 16: suites->ft_fils_sha256 = RT_TRUE; break;
    case 17: suites->ft_fils_sha384 = RT_TRUE; break;
    case 18: suites->owe = RT_TRUE; break;
    case 19: suites->ft_psk_sha384 = RT_TRUE; break;
    case 20: suites->psk_sha384 = RT_TRUE; break;
    case 23: suites->enterprise_sha384 = RT_TRUE; break;
    case 24: suites->sae_ext_key = RT_TRUE; break;
    case 25: suites->ft_sae_ext_key = RT_TRUE; break;
    default: break;
    }
}

static rt_wlan_security_t ehf_ng_security_from_ies(rt_uint16_t capability,
                                                   const rt_uint8_t *ies,
                                                   rt_size_t length)
{
    struct ehf_ng_security_suites rsn = {0}, wpa = {0};
    rt_size_t offset = 0;
    rt_bool_t security_ie = RT_FALSE;
    rt_bool_t owe_transition = RT_FALSE;
    rt_bool_t osen = RT_FALSE;
    rt_bool_t wapi_psk = RT_FALSE;
    rt_bool_t wapi_cert = RT_FALSE;

    if (length && !ies)
    {
        return SECURITY_UNKNOWN;
    }
    while (offset + 2U <= length)
    {
        rt_uint8_t id = ies[offset];
        rt_uint8_t ie_length = ies[offset + 1];

        if (offset + 2U + ie_length > length)
        {
            break;
        }
        if (id == 221 && ie_length >= 4U &&
            ies[offset + 2] == 0x50 && ies[offset + 3] == 0x6f &&
            ies[offset + 4] == 0x9a)
        {
            owe_transition |= ies[offset + 5] == 0x1c;
            if (ies[offset + 5] == 0x12)
            {
                osen = RT_TRUE;
                security_ie = RT_TRUE;
            }
        }
        if (id == 68)
        {
            const rt_uint8_t *body = ies + offset + 2;
            rt_uint16_t count;
            rt_size_t position = 4U;
            rt_size_t index;

            security_ie = RT_TRUE;
            if (ie_length < 8U || ehf_get_le16(body) != 1U)
            {
                offset += 2U + ie_length;
                continue;
            }
            count = ehf_get_le16(body + 2);
            for (index = 0; index < count && position + 4U <= ie_length;
                 index++, position += 4U)
            {
                if (body[position] == 0x00 && body[position + 1] == 0x14 &&
                    body[position + 2] == 0x72)
                {
                    wapi_cert |= body[position + 3] == 1;
                    wapi_psk |= body[position + 3] == 2;
                }
            }
        }
        if (id == 48 ||
            (id == 221 && ie_length >= 6 &&
             ies[offset + 2] == 0x00 && ies[offset + 3] == 0x50 &&
             ies[offset + 4] == 0xf2 && ies[offset + 5] == 0x01))
        {
            security_ie = RT_TRUE;
            struct ehf_ng_security_suites parsed = {0};
            struct ehf_ng_security_suites *suites = id == 48 ? &rsn : &wpa;
            const rt_uint8_t *body = ies + offset + 2;
            const rt_uint8_t *oui = id == 48 ?
                (const rt_uint8_t *)"\x00\x0f\xac" :
                (const rt_uint8_t *)"\x00\x50\xf2";
            rt_size_t body_length = ie_length;
            rt_size_t position = id == 48 ? 0U : 4U;
            rt_uint16_t count;
            rt_size_t index;

            if (body_length < position + 8U ||
                ehf_get_le16(body + position) != 1U)
            {
                offset += 2U + ie_length;
                continue;
            }
            if (rt_memcmp(body + position + 2U, oui, 3) == 0)
            {
                parsed.group_tkip = body[position + 5U] == 2;
                parsed.group_aes = body[position + 5U] == 4 ||
                                   body[position + 5U] == 8 ||
                                   body[position + 5U] == 9 ||
                                   body[position + 5U] == 10;
            }
            position += 2U + 4U; /* version and group cipher */
            count = ehf_get_le16(body + position);
            position += 2U;
            for (index = 0; index < count && position + 4U <= body_length;
                 index++, position += 4U)
            {
                if (rt_memcmp(body + position, oui, 3) == 0)
                {
                    rt_uint8_t suite = body[position + 3];

                    parsed.tkip |= suite == 2 ||
                                   (suite == 0 && parsed.group_tkip);
                    /* RT-Thread's legacy security enum has one AES bit.  Map
                     * CCMP-128, GCMP-128, GCMP-256 and CCMP-256 to it. */
                    parsed.aes |= suite == 4 || suite == 8 ||
                                  suite == 9 || suite == 10 ||
                                  (suite == 0 && parsed.group_aes);
                }
            }
            if (index != count || position + 2U > body_length)
            {
                offset += 2U + ie_length;
                continue;
            }
            count = ehf_get_le16(body + position);
            position += 2U;
            for (index = 0; index < count && position + 4U <= body_length;
                 index++, position += 4U)
            {
                ehf_ng_parse_akm_suite(&parsed, body + position, id == 48);
            }
            if (index != count)
            {
                offset += 2U + ie_length;
                continue;
            }
            *suites = parsed;
        }
        offset += 2U + ie_length;
    }
    if (!(capability & 0x0010U))
    {
        return owe_transition ? SECURITY_OWE_TRANSITION : SECURITY_OPEN;
    }
    if (wapi_psk || wapi_cert)
    {
        return wapi_psk ? SECURITY_WAPI_PSK : SECURITY_WAPI_CERT;
    }
    if (osen)
    {
        return SECURITY_OSEN;
    }
    if (rsn.aes)
    {
        rt_bool_t psk = rsn.psk || rsn.psk_sha256 || rsn.ft_psk;
        rt_bool_t sae = rsn.sae || rsn.ft_sae || rsn.sae_ext_key ||
                        rsn.ft_sae_ext_key;

        if (psk && sae)
        {
            return SECURITY_WPA2_WPA3_MIXED_PSK;
        }
        if ((rsn.enterprise || rsn.enterprise_sha256) &&
            (rsn.suite_b || rsn.suite_b_192 ||
             rsn.enterprise_sha384))
        {
            return SECURITY_WPA2_WPA3_MIXED_8021X;
        }
        if (rsn.ft_sae_ext_key)
        {
            return SECURITY_FT_WPA3_SAE_EXT_KEY;
        }
        if (rsn.sae_ext_key)
        {
            return SECURITY_WPA3_SAE_EXT_KEY;
        }
        if (rsn.ft_sae)
        {
            return SECURITY_FT_WPA3_SAE;
        }
        if (rsn.sae)
        {
            return SECURITY_WPA3_SAE;
        }
        if (rsn.ft_enterprise_sha384)
        {
            return SECURITY_FT_WPA3_8021X_SHA384;
        }
        if (rsn.suite_b_192)
        {
            return SECURITY_WPA3_192BIT_8021X;
        }
        if (rsn.suite_b || rsn.enterprise_sha384)
        {
            return SECURITY_WPA3_AES_8021X;
        }
        if (rsn.ft_fils_sha384)
        {
            return SECURITY_FT_FILS_SHA384;
        }
        if (rsn.ft_fils_sha256)
        {
            return SECURITY_FT_FILS_SHA256;
        }
        if (rsn.fils_sha384)
        {
            return SECURITY_FILS_SHA384;
        }
        if (rsn.fils_sha256)
        {
            return SECURITY_FILS_SHA256;
        }
        if (rsn.owe)
        {
            return owe_transition ? SECURITY_OWE_TRANSITION : SECURITY_OWE;
        }
        if (rsn.dpp)
        {
            return SECURITY_DPP;
        }
        if (rsn.cckm)
        {
            return SECURITY_CCKM;
        }
        if (rsn.ft_psk)
        {
            return SECURITY_FT_WPA2_AES_PSK;
        }
        if (rsn.ft_psk_sha384)
        {
            return SECURITY_FT_WPA3_AES_PSK_SHA384;
        }
        if (rsn.psk_sha384)
        {
            return SECURITY_WPA3_AES_PSK_SHA384;
        }
        if (rsn.psk_sha256)
        {
            return SECURITY_WPA2_AES_PSK_SHA256;
        }
        if (rsn.ft_enterprise)
        {
            return SECURITY_FT_WPA2_AES_8021X;
        }
        if (rsn.enterprise_sha256)
        {
            return SECURITY_WPA2_AES_8021X_SHA256;
        }
    }
    if (rsn.psk && wpa.psk)
    {
        return SECURITY_WPA_WPA2_MIXED_PSK;
    }
    if (rsn.enterprise && wpa.enterprise)
    {
        return SECURITY_WPA_WPA2_MIXED_8021X;
    }
    if (rsn.psk)
    {
        if (rsn.aes && (rsn.tkip || rsn.group_tkip))
        {
            return SECURITY_WPA2_MIXED_PSK;
        }
        if (rsn.aes)
        {
            return SECURITY_WPA2_AES_PSK;
        }
        if (rsn.tkip)
        {
            return SECURITY_WPA2_TKIP_PSK;
        }
    }
    if (rsn.enterprise)
    {
        if (rsn.aes)
        {
            return SECURITY_WPA2_AES_8021X;
        }
        if (rsn.tkip)
        {
            return SECURITY_WPA2_TKIP_8021X;
        }
    }
    if (wpa.psk)
    {
        return wpa.aes ? SECURITY_WPA_AES_PSK :
               wpa.tkip ? SECURITY_WPA_TKIP_PSK : SECURITY_UNKNOWN;
    }
    if (wpa.enterprise)
    {
        if (wpa.aes)
        {
            return SECURITY_WPA_AES_8021X;
        }
        if (wpa.tkip)
        {
            return SECURITY_WPA_TKIP_8021X;
        }
    }
    /* A protected BSS without an RSN/WPA IE is legacy WEP.  Do not label a
     * malformed or unsupported WPA/RSN element as WEP, however. */
    return security_ie ? SECURITY_UNKNOWN : SECURITY_WEP_PSK;
}

static void ehf_ng_process_scan_event(struct ehf_context *context,
                                      const rt_uint8_t *payload,
                                      rt_size_t length)
{
    const struct ehf_ng_scan_event *scan;
    struct rt_wlan_offload_event event;
    const rt_uint8_t *ies;
    rt_size_t ies_length;
    rt_size_t offset;
    rt_uint16_t frame_length;

    if (length < sizeof(*scan) || !context->scan_request_id)
    {
        return;
    }
    scan = (const struct ehf_ng_scan_event *)payload;
    if (!scan->header.status)
    {
        rt_uint32_t request_id = context->scan_request_id;

        context->scan_request_id = 0;
        rt_memset(&event, 0, sizeof(event));
        event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_DONE;
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
        event.request_id = request_id;
        event.status = RT_EOK;
        rt_wlan_offload_report_event(&context->radio, &event);
        return;
    }
    frame_length = ehf_get_le16(scan->frame_length);
    if (sizeof(*scan) + frame_length > length ||
        frame_length < EHF_NG_BEACON_FIXED_LENGTH)
    {
        return;
    }
    ies = scan->frame + EHF_NG_BEACON_FIXED_LENGTH;
    ies_length = frame_length - EHF_NG_BEACON_FIXED_LENGTH;

    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_RESULT;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = context->scan_request_id;
    event.status = RT_EOK;
    rt_memcpy(event.data.network.bssid, scan->bssid, 6);
    ehf_ng_channel_definition(scan->channel, &event.data.network.channel);
    event.data.network.rssi = (rt_int32_t)ehf_get_le32(scan->rssi);
    event.data.network.beacon_interval = ehf_get_le16(scan->frame + 8);
    event.data.network.capability = ehf_get_le16(scan->frame + 10);
    event.data.network.security = ehf_ng_security_from_ies(
        event.data.network.capability, ies, ies_length);
    event.data.network.ies = ies;
    event.data.network.ies_length = ies_length;
    for (offset = 0; offset + 2 <= ies_length; )
    {
        rt_uint8_t ie_length = ies[offset + 1];

        if (offset + 2U + ie_length > ies_length)
        {
            break;
        }
        if (ies[offset] == 0)
        {
            event.data.network.ssid.len =
                ie_length <= RT_WLAN_SSID_MAX_LENGTH ? ie_length :
                                                        RT_WLAN_SSID_MAX_LENGTH;
            rt_memcpy(event.data.network.ssid.val, ies + offset + 2,
                      event.data.network.ssid.len);
            break;
        }
        offset += 2U + ie_length;
    }
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void ehf_ng_report_management(struct ehf_context *context,
                                     enum rt_wlan_offload_event_type type,
                                     enum rt_wlan_offload_iftype iftype,
                                     rt_uint32_t request_id,
                                     rt_uint8_t channel, rt_int32_t rssi,
                                     const rt_uint8_t *frame,
                                     rt_size_t frame_length)
{
    struct rt_wlan_offload_event event;

    if (!channel || !frame || !frame_length)
    {
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = type;
    event.iftype = iftype;
    event.request_id = request_id;
    event.status = RT_EOK;
    ehf_ng_channel_definition(channel, &event.data.management.channel);
    event.data.management.rssi = rssi;
    event.data.management.data = frame;
    event.data.management.length = frame_length;
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void ehf_ng_process_event(struct ehf_context *context,
                                 rt_uint8_t interface,
                                 const rt_uint8_t *payload,
                                 rt_size_t length)
{
    const struct ehf_ng_event_header *header;

    if (length < sizeof(*header))
    {
        return;
    }
    header = (const struct ehf_ng_event_header *)payload;
    if (ehf_get_le16(header->length) > length - sizeof(*header))
    {
        LOG_W("truncated event %u", header->event);
        return;
    }
    length = sizeof(*header) + ehf_get_le16(header->length);
    switch (header->event)
    {
    case EHF_NG_EVENT_SCAN_RESULT:
        ehf_ng_process_scan_event(context, payload, length);
        break;

    case EHF_NG_EVENT_AUTH_RX:
        if (length >= sizeof(struct ehf_ng_auth_event))
        {
            const struct ehf_ng_auth_event *event =
                (const struct ehf_ng_auth_event *)payload;
            rt_size_t frame_length = ehf_get_le16(event->frame_length);

            if (sizeof(*event) + frame_length <= length)
            {
                rt_uint32_t request_id = context->auth_request_id;

                context->auth_request_id = 0;
                ehf_ng_report_management(
                    context, RT_WLAN_OFFLOAD_EVENT_AUTH_RX,
                    RT_WLAN_OFFLOAD_IFTYPE_STATION, request_id,
                    event->channel, (rt_int32_t)ehf_get_le32(event->rssi),
                    event->frame, frame_length);
            }
        }
        break;

    case EHF_NG_EVENT_ASSOC_RX:
        if (length >= sizeof(struct ehf_ng_assoc_event))
        {
            const struct ehf_ng_assoc_event *event =
                (const struct ehf_ng_assoc_event *)payload;
            rt_size_t frame_length = ehf_get_le16(event->frame_length);

            if (sizeof(*event) + frame_length <= length)
            {
                struct rt_wlan_offload_event connected_event;
                rt_uint32_t request_id = context->assoc_request_id;

                context->assoc_request_id = 0;
                context->sta_connected = header->status == 0;
                if (context->sta_connected)
                {
                    rt_memcpy(context->sta_bssid, event->bssid,
                              sizeof(context->sta_bssid));
                }
                else
                {
                    rt_memset(context->sta_bssid, 0,
                              sizeof(context->sta_bssid));
                }
                ehf_ng_report_management(
                    context, RT_WLAN_OFFLOAD_EVENT_ASSOC_RX,
                    RT_WLAN_OFFLOAD_IFTYPE_STATION, request_id,
                    event->channel, (rt_int32_t)ehf_get_le32(event->rssi),
                    event->frame, frame_length);
                if (request_id)
                {
                    rt_memset(&connected_event, 0,
                              sizeof(connected_event));
                    connected_event.type = RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT;
                    connected_event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
                    connected_event.request_id = request_id;
                    connected_event.status = header->status ?
                                             -RT_ERROR : RT_EOK;
                    rt_memcpy(connected_event.data.network.bssid,
                              event->bssid, 6);
                    rt_wlan_offload_report_event(&context->radio,
                                            &connected_event);
                }
            }
        }
        break;

    case EHF_NG_EVENT_STA_DISCONNECT:
        if (length >= sizeof(struct ehf_ng_disconnect_event))
        {
            const struct ehf_ng_disconnect_event *disconnected =
                (const struct ehf_ng_disconnect_event *)payload;
            struct rt_wlan_offload_event event;

            context->sta_connected = RT_FALSE;
            context->auth_request_id = 0;
            context->assoc_request_id = 0;
            rt_memset(context->sta_bssid, 0, sizeof(context->sta_bssid));
            rt_memset(&event, 0, sizeof(event));
            event.type = RT_WLAN_OFFLOAD_EVENT_DISCONNECTED;
            event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
            event.status = RT_EOK;
            event.data.disconnected.reason = disconnected->reason;
            rt_memcpy(event.data.disconnected.bssid,
                      disconnected->bssid, 6);
            rt_wlan_offload_report_event(&context->radio, &event);
        }
        break;

    case EHF_NG_EVENT_AP_MGMT_RX:
        if (length >= sizeof(struct ehf_ng_mgmt_event))
        {
            const struct ehf_ng_mgmt_event *event =
                (const struct ehf_ng_mgmt_event *)payload;
            rt_size_t frame_length = ehf_get_le32(event->frame_length);

            if (sizeof(*event) + frame_length <= length)
            {
                ehf_ng_report_management(
                    context, RT_WLAN_OFFLOAD_EVENT_MGMT_RX,
                    interface == EHF_NG_AP_INTERFACE ?
                        RT_WLAN_OFFLOAD_IFTYPE_AP : RT_WLAN_OFFLOAD_IFTYPE_STATION,
                    0, ehf_get_le32(event->channel),
                    (rt_int32_t)ehf_get_le32(event->rssi),
                    event->frame, frame_length);
            }
        }
        break;

    default:
        break;
    }
}

static void ehf_ng_process_boot(struct ehf_context *context,
                                const rt_uint8_t *payload,
                                rt_size_t length)
{
    const struct ehf_ng_boot_event *boot;
    rt_uint8_t capabilities = 0;
    rt_uint8_t chip_id = 0;
    rt_uint8_t version[8] = {0};
    const rt_uint8_t *position;
    rt_size_t remaining;

    if (length < sizeof(*boot))
    {
        return;
    }
    boot = (const struct ehf_ng_boot_event *)payload;
    if (boot->header.event != 1 || boot->header.status ||
        sizeof(*boot) + boot->length > length)
    {
        return;
    }
    position = boot->data;
    remaining = boot->length;
    while (remaining >= 2)
    {
        rt_uint8_t tag = position[0];
        rt_uint8_t tag_length = position[1];

        if ((rt_size_t)tag_length + 2 > remaining)
        {
            break;
        }
        if (tag == EHF_NG_BOOT_CAPABILITY && tag_length >= 1)
        {
            capabilities = position[2];
        }
        else if (tag == EHF_NG_BOOT_FIRMWARE_DATA && tag_length >= 8)
        {
            rt_memcpy(version, position + 2, sizeof(version));
        }
        else if (tag == EHF_NG_BOOT_CHIP_ID && tag_length >= 1)
        {
            chip_id = position[2];
        }
        else if (tag == EHF_NG_BOOT_RX_BUFFER_SIZE && tag_length >= 4)
        {
            context->sdio_token_size = ehf_get_le32(position + 2);
        }
        position += tag_length + 2;
        remaining -= tag_length + 2;
    }
    ehf_boot_ready(context, capabilities, chip_id, version);
}

static void ehf_ng_process_frame(struct ehf_context *context,
                                 const struct ehf_ng_transport_header *header,
                                 const rt_uint8_t *payload,
                                 rt_size_t payload_length)
{
    rt_uint8_t interface = header->interface_number & 0x0f;

    if ((interface == EHF_NG_STA_INTERFACE ||
         interface == EHF_NG_AP_INTERFACE) &&
        (header->packet_type == EHF_NG_PACKET_DATA ||
         header->packet_type == EHF_NG_PACKET_EAPOL))
    {
        rt_wlan_offload_rx(&context->radio,
                      interface == EHF_NG_AP_INTERFACE ?
                          RT_WLAN_OFFLOAD_IFTYPE_AP : RT_WLAN_OFFLOAD_IFTYPE_STATION,
                      payload, payload_length);
    }
    else if (header->packet_type == EHF_NG_PACKET_COMMAND_RESPONSE)
    {
        ehf_ng_process_response(context, payload, payload_length);
    }
    else if (header->packet_type == EHF_NG_PACKET_EVENT &&
             interface == EHF_NG_INTERNAL_INTERFACE)
    {
        ehf_ng_process_boot(context, payload, payload_length);
    }
    else if (header->packet_type == EHF_NG_PACKET_EVENT)
    {
        ehf_ng_process_event(context, interface, payload, payload_length);
    }
}

static rt_err_t ehf_ng_receive(struct ehf_context *context, const void *data,
                               rt_size_t length)
{
    const rt_uint8_t *bytes = data;
    rt_size_t position = 0;
    rt_bool_t processed = RT_FALSE;
    rt_bool_t malformed = RT_FALSE;

    if (!context || !data || length < sizeof(struct ehf_ng_transport_header))
    {
        return -RT_EINVAL;
    }

    while (position + sizeof(struct ehf_ng_transport_header) <= length)
    {
        const struct ehf_ng_transport_header *header =
            (const struct ehf_ng_transport_header *)(bytes + position);
        rt_uint16_t payload_length = ehf_get_le16(header->length);
        rt_uint16_t payload_offset = ehf_get_le16(header->offset);
        rt_size_t frame_length;

        if (!payload_length && !payload_offset)
        {
            break;
        }
        if ((header->interface_number & 0x0f) >= EHF_NG_INTERFACE_MAX ||
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
                ehf_ng_frame_checksum(bytes + position, frame_length))
        {
            if (!context->invalid_rx_log_count)
            {
                LOG_W("dropping frame with invalid checksum: received=%u "
                      "calculated=%u",
                      ehf_get_le16(header->checksum),
                      ehf_ng_frame_checksum(bytes + position, frame_length));
            }
            context->invalid_rx_log_count = 1;
            malformed = RT_TRUE;
        }
        else
        {
            ehf_ng_process_frame(context, header,
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

static void ehf_ng_reset(struct ehf_context *context)
{
    context->scan_request_id = 0;
    context->connect_request_id = 0;
    context->auth_request_id = 0;
    context->assoc_request_id = 0;
    rt_memset(context->sta_bssid, 0, sizeof(context->sta_bssid));
    context->sdio_token_size = ESP_HOSTED_WIFI_SDIO_TOKEN_SIZE;
}

static const struct rt_wlan_offload_ops g_ehf_ng_wlan_offload_ops = {
    .start = ehf_ng_start,
    .stop = ehf_ng_stop,
    .change_interface = ehf_ng_change_interface,
    .scan = ehf_ng_scan,
    .disconnect = ehf_ng_disconnect,
    .start_ap = ehf_ng_start_ap,
    .stop_ap = ehf_ng_stop_ap,
    .del_station = ehf_ng_del_station,
    .add_station = ehf_ng_add_station,
    .set_station_authorized = ehf_ng_set_station_authorized,
    .get_rssi = ehf_ng_get_rssi,
    .set_regulatory = ehf_ng_set_regulatory,
    .get_regulatory = ehf_ng_get_regulatory,
    .set_mac = ehf_ng_set_mac,
    .get_mac = ehf_ng_get_mac,
    .transmit = ehf_ng_transmit,
    .auth = ehf_ng_auth,
    .assoc = ehf_ng_assoc,
    .add_key = ehf_ng_add_key,
    .delete_key = ehf_ng_delete_key,
    .set_default_key = ehf_ng_set_default_key,
    .transmit_mgmt = ehf_ng_transmit_mgmt,
};

const struct ehf_protocol_ops g_ehf_ng_protocol = {
    .name = "ESP-Hosted-NG",
    .capabilities = RT_WLAN_OFFLOAD_CAP_STA | RT_WLAN_OFFLOAD_CAP_AP |
                    RT_WLAN_OFFLOAD_CAP_STA_AP_CONCURRENT |
                    RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT |
                    RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR,
    .max_pending_commands = 1,
    .wlan_offload_ops = &g_ehf_ng_wlan_offload_ops,
    .command_push = ehf_ng_command_push,
    .receive = ehf_ng_receive,
    .reset = ehf_ng_reset,
};
