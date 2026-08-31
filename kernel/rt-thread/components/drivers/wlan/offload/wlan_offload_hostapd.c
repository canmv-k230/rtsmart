/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wlan_offload_hostapd.h"
#include "wlan_offload_crypto.h"

#define DBG_TAG "WLAN.hostapd"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define HOSTAPD_MAX_STATIONS              10U
#define HOSTAPD_RETRY_INTERVAL_MS       1000U
#define HOSTAPD_MAX_RETRIES                3U
#define HOSTAPD_EAPOL_MAX                192U

#define WLAN_FC_ASSOC_REQ             0x0000U
#define WLAN_FC_REASSOC_REQ           0x0020U
#define WLAN_FC_ASSOC_RESP            0x0010U
#define WLAN_FC_REASSOC_RESP          0x0030U
#define WLAN_FC_AUTH                  0x00b0U
#define WLAN_FC_DISASSOC              0x00a0U
#define WLAN_FC_DEAUTH                0x00c0U
#define WLAN_AUTH_OPEN                     0U
#define WLAN_STATUS_SUCCESS                 0U
#define WLAN_STATUS_UNSPECIFIED_FAILURE     1U
#define WLAN_STATUS_NOT_SUPPORTED_AUTH_ALG 13U
#define WLAN_REASON_PREV_AUTH_NOT_VALID     2U
#define WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT 15U

#define EAPOL_TYPE_KEY                     3U
#define EAPOL_KEY_DESCRIPTOR_RSN           2U
#define EAPOL_KEY_FIXED_LENGTH            99U
#define EAPOL_KEY_MIC_OFFSET              81U
#define EAPOL_KEY_DATA_OFFSET             99U
#define WPA_KEY_INFO_VERSION_MASK      0x0007U
#define WPA_KEY_INFO_VERSION_2         0x0002U
#define WPA_KEY_INFO_PAIRWISE          0x0008U
#define WPA_KEY_INFO_INSTALL           0x0040U
#define WPA_KEY_INFO_ACK               0x0080U
#define WPA_KEY_INFO_MIC               0x0100U
#define WPA_KEY_INFO_SECURE            0x0200U
#define WPA_KEY_INFO_ERROR             0x0400U
#define WPA_KEY_INFO_REQUEST           0x0800U
#define WPA_KEY_INFO_ENCRYPTED         0x1000U

#define WPA_PMK_LENGTH                    32U
#define WPA_PTK_LENGTH                    48U
#define WPA_KCK_LENGTH                    16U
#define WPA_KEK_OFFSET                    16U
#define WPA_TK_OFFSET                     32U
#define WPA_NONCE_LENGTH                  32U
#define WPA_GTK_LENGTH                    16U
/* The pairwise temporal key happens to be the same size as the CCMP
 * group key, but it is a different key; keep the lengths separate so a
 * future group-cipher change cannot silently resize the pairwise key. */
#define WPA_TK_LENGTH                     16U

enum wlan_offload_hostapd_station_state
{
    HOSTAPD_STATION_FREE = 0,
    HOSTAPD_STATION_AUTHENTICATED,
    HOSTAPD_STATION_WAIT_M2,
    HOSTAPD_STATION_WAIT_M4,
    HOSTAPD_STATION_AUTHORIZED,
};

struct wlan_offload_hostapd_station
{
    struct rt_wlan_offload_hostapd *hostapd;
    enum wlan_offload_hostapd_station_state state;
    rt_uint8_t mac[6];
    rt_uint16_t aid;
    rt_bool_t firmware_added;
    rt_bool_t reported;
    rt_uint8_t anonce[WPA_NONCE_LENGTH];
    rt_uint8_t snonce[WPA_NONCE_LENGTH];
    rt_uint8_t ptk[WPA_PTK_LENGTH];
    rt_uint8_t replay[8];
    rt_uint8_t last_eapol[HOSTAPD_EAPOL_MAX];
    rt_size_t last_eapol_length;
    rt_tick_t retry_deadline;
    rt_uint8_t retries;
};

struct rt_wlan_offload_hostapd
{
    struct rt_wlan_offload_radio *radio;
    struct rt_mutex lock;
    rt_timer_t timer;
    rt_bool_t prepared;
    rt_bool_t active;
    rt_bool_t secure;
    rt_bool_t gtk_installed;
    rt_uint32_t request_id;
    rt_uint64_t replay_counter;
    rt_uint8_t max_stations;
    rt_wlan_ssid_t ssid;
    struct rt_wlan_offload_channel_definition channel;
    rt_uint8_t address[6];
    rt_uint8_t pmk[WPA_PMK_LENGTH];
    rt_uint8_t gtk[WPA_GTK_LENGTH];
    struct wlan_offload_hostapd_station stations[HOSTAPD_MAX_STATIONS];
};

static const rt_uint8_t g_hostapd_rsn_ie[] = {
    0x30, 0x14, 0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* group cipher: CCMP-128 */
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, /* pairwise: CCMP-128 */
    0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, /* AKM: PSK */
    0x00, 0x00
};

static rt_uint16_t hostapd_get_le16(const rt_uint8_t *data)
{
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8);
}

static rt_uint16_t hostapd_get_be16(const rt_uint8_t *data)
{
    return ((rt_uint16_t)data[0] << 8) | data[1];
}

static void hostapd_put_le16(rt_uint8_t *data, rt_uint16_t value)
{
    data[0] = (rt_uint8_t)value;
    data[1] = (rt_uint8_t)(value >> 8);
}

static rt_uint8_t hostapd_frequency_to_channel(rt_uint16_t frequency)
{
    if (frequency >= 5000U && frequency <= 5900U)
    {
        return (rt_uint8_t)((frequency - 5000U) / 5U);
    }
    return 0;
}

static void hostapd_put_be16(rt_uint8_t *data, rt_uint16_t value)
{
    data[0] = (rt_uint8_t)(value >> 8);
    data[1] = (rt_uint8_t)value;
}

static rt_bool_t hostapd_mac_equal(const rt_uint8_t left[6],
                                   const rt_uint8_t right[6])
{
    return left && right && rt_memcmp(left, right, 6) == 0;
}

static rt_bool_t hostapd_mac_is_broadcast(const rt_uint8_t address[6])
{
    static const rt_uint8_t broadcast[6] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    return hostapd_mac_equal(address, broadcast);
}

static rt_int8_t hostapd_hex_digit(rt_uint8_t value)
{
    if (value >= '0' && value <= '9') return (rt_int8_t)(value - '0');
    if (value >= 'a' && value <= 'f') return (rt_int8_t)(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return (rt_int8_t)(value - 'A' + 10);
    return -1;
}

static rt_bool_t hostapd_decode_raw_psk(const rt_wlan_key_t *key,
                                        rt_uint8_t pmk[WPA_PMK_LENGTH])
{
    rt_size_t index;

    if (!key || key->len != 64U)
    {
        return RT_FALSE;
    }
    for (index = 0; index < WPA_PMK_LENGTH; index++)
    {
        rt_int8_t high = hostapd_hex_digit(key->val[index * 2U]);
        rt_int8_t low = hostapd_hex_digit(key->val[index * 2U + 1U]);

        if (high < 0 || low < 0)
        {
            rt_wlan_offload_crypto_zero(pmk, WPA_PMK_LENGTH);
            return RT_FALSE;
        }
        pmk[index] = (rt_uint8_t)((high << 4) | low);
    }
    return RT_TRUE;
}

static rt_err_t hostapd_random(rt_uint8_t *data, rt_size_t length)
{
    rt_device_t random = rt_device_find("hwrng");
    rt_size_t received = 0;
    rt_err_t result;

    if (!random)
    {
        return -RT_ENOSYS;
    }
    result = rt_device_open(random, RT_DEVICE_OFLAG_RDONLY);
    if (result != RT_EOK)
    {
        return result;
    }
    while (received < length)
    {
        rt_size_t count = rt_device_read(random, 0, data + received,
                                         length - received);
        if (!count)
        {
            result = -RT_EIO;
            break;
        }
        received += count;
    }
    rt_device_close(random);
    return result;
}

static void hostapd_station_clear(struct wlan_offload_hostapd_station *station)
{
    struct rt_wlan_offload_hostapd *hostapd;

    if (!station)
    {
        return;
    }
    hostapd = station->hostapd;
    rt_wlan_offload_crypto_zero(station, sizeof(*station));
    station->hostapd = hostapd;
}

static void hostapd_clear_locked(struct rt_wlan_offload_hostapd *hostapd)
{
    rt_size_t index;

    hostapd->prepared = RT_FALSE;
    hostapd->active = RT_FALSE;
    hostapd->secure = RT_FALSE;
    hostapd->gtk_installed = RT_FALSE;
    hostapd->request_id = 0;
    hostapd->replay_counter = 0;
    rt_memset(&hostapd->ssid, 0, sizeof(hostapd->ssid));
    rt_memset(&hostapd->channel, 0, sizeof(hostapd->channel));
    rt_memset(hostapd->address, 0, sizeof(hostapd->address));
    rt_wlan_offload_crypto_zero(hostapd->pmk, sizeof(hostapd->pmk));
    rt_wlan_offload_crypto_zero(hostapd->gtk, sizeof(hostapd->gtk));
    for (index = 0; index < HOSTAPD_MAX_STATIONS; index++)
    {
        hostapd_station_clear(&hostapd->stations[index]);
    }
    hostapd->max_stations = 0;
}

static struct rt_wlan_offload_hostapd *hostapd_lock_from_radio(
    struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_hostapd *hostapd;

    if (!radio)
    {
        return RT_NULL;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    hostapd = radio->hostapd;
    if (hostapd)
    {
        rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    }
    rt_mutex_release(&radio->operation_lock);
    return hostapd;
}

static struct wlan_offload_hostapd_station *hostapd_find_station_locked(
    struct rt_wlan_offload_hostapd *hostapd, const rt_uint8_t mac[6])
{
    rt_size_t index;

    for (index = 0; index < hostapd->max_stations; index++)
    {
        if (hostapd->stations[index].state != HOSTAPD_STATION_FREE &&
            hostapd_mac_equal(hostapd->stations[index].mac, mac))
        {
            return &hostapd->stations[index];
        }
    }
    return RT_NULL;
}

static struct wlan_offload_hostapd_station *hostapd_allocate_station_locked(
    struct rt_wlan_offload_hostapd *hostapd, const rt_uint8_t mac[6])
{
    rt_size_t index;

    for (index = 0; index < hostapd->max_stations; index++)
    {
        if (hostapd->stations[index].state == HOSTAPD_STATION_FREE)
        {
            struct wlan_offload_hostapd_station *station =
                &hostapd->stations[index];

            station->state = HOSTAPD_STATION_AUTHENTICATED;
            station->aid = (rt_uint16_t)(index + 1U);
            rt_memcpy(station->mac, mac, 6);
            return station;
        }
    }
    return RT_NULL;
}

static void hostapd_next_replay_locked(
    struct rt_wlan_offload_hostapd *hostapd, rt_uint8_t replay[8])
{
    int index;

    hostapd->replay_counter++;
    if (!hostapd->replay_counter)
    {
        hostapd->replay_counter++;
    }
    for (index = 7; index >= 0; index--)
    {
        replay[index] = (rt_uint8_t)(hostapd->replay_counter >>
                                     ((7 - index) * 8));
    }
}

static void hostapd_derive_ptk(struct rt_wlan_offload_hostapd *hostapd,
                               struct wlan_offload_hostapd_station *station)
{
    rt_uint8_t context[76];
    const rt_uint8_t *first;
    const rt_uint8_t *second;

    if (rt_memcmp(hostapd->address, station->mac, 6) < 0)
    {
        first = hostapd->address;
        second = station->mac;
    }
    else
    {
        first = station->mac;
        second = hostapd->address;
    }
    rt_memcpy(context, first, 6);
    rt_memcpy(context + 6, second, 6);
    if (rt_memcmp(station->anonce, station->snonce,
                  WPA_NONCE_LENGTH) < 0)
    {
        first = station->anonce;
        second = station->snonce;
    }
    else
    {
        first = station->snonce;
        second = station->anonce;
    }
    rt_memcpy(context + 12, first, WPA_NONCE_LENGTH);
    rt_memcpy(context + 44, second, WPA_NONCE_LENGTH);
    rt_wlan_offload_wpa_prf(hostapd->pmk, sizeof(hostapd->pmk),
                       "Pairwise key expansion", context, sizeof(context),
                       station->ptk, sizeof(station->ptk));
    rt_wlan_offload_crypto_zero(context, sizeof(context));
}

static rt_bool_t hostapd_verify_mic(
    const struct wlan_offload_hostapd_station *station,
    const rt_uint8_t *frame, rt_size_t length)
{
    rt_uint8_t copy[HOSTAPD_EAPOL_MAX];
    rt_uint8_t received[WPA_KCK_LENGTH];
    rt_uint8_t digest[20];
    rt_bool_t valid;

    if (!station || !frame || length > sizeof(copy) ||
        length < EAPOL_KEY_FIXED_LENGTH)
    {
        return RT_FALSE;
    }
    rt_memcpy(copy, frame, length);
    rt_memcpy(received, copy + EAPOL_KEY_MIC_OFFSET, sizeof(received));
    rt_memset(copy + EAPOL_KEY_MIC_OFFSET, 0, sizeof(received));
    rt_wlan_offload_hmac_sha1(station->ptk, WPA_KCK_LENGTH,
                         copy, length, digest);
    valid = rt_wlan_offload_crypto_equal(received, digest, sizeof(received));
    rt_wlan_offload_crypto_zero(copy, sizeof(copy));
    rt_wlan_offload_crypto_zero(received, sizeof(received));
    rt_wlan_offload_crypto_zero(digest, sizeof(digest));
    return valid;
}

static rt_bool_t hostapd_rsn_ie_valid(const rt_uint8_t *ies,
                                      rt_size_t length)
{
    rt_size_t offset = 0;

    while (ies && offset + 2U <= length)
    {
        rt_uint8_t id = ies[offset];
        rt_uint8_t element_length = ies[offset + 1U];
        const rt_uint8_t *body = ies + offset + 2U;
        rt_size_t position;
        rt_uint16_t count;
        rt_size_t index;
        rt_bool_t pairwise_ccmp = RT_FALSE;
        rt_bool_t psk = RT_FALSE;

        if (offset + 2U + element_length > length)
        {
            return RT_FALSE;
        }
        if (id != 48U)
        {
            offset += 2U + element_length;
            continue;
        }
        if (element_length < 18U || hostapd_get_le16(body) != 1U ||
            rt_memcmp(body + 2U, "\x00\x0f\xac\x04", 4) != 0)
        {
            return RT_FALSE;
        }
        position = 6U;
        count = hostapd_get_le16(body + position);
        position += 2U;
        for (index = 0; index < count && position + 4U <= element_length;
             index++, position += 4U)
        {
            pairwise_ccmp |=
                rt_memcmp(body + position, "\x00\x0f\xac\x04", 4) == 0;
        }
        if (index != count || position + 2U > element_length)
        {
            return RT_FALSE;
        }
        count = hostapd_get_le16(body + position);
        position += 2U;
        for (index = 0; index < count && position + 4U <= element_length;
             index++, position += 4U)
        {
            psk |= rt_memcmp(body + position, "\x00\x0f\xac\x02", 4) == 0;
        }
        return index == count && pairwise_ccmp && psk;
    }
    return RT_FALSE;
}

static rt_err_t hostapd_make_key_frame(
    struct wlan_offload_hostapd_station *station, rt_uint16_t key_info,
    const rt_uint8_t *nonce, const rt_uint8_t *key_data,
    rt_size_t key_data_length, rt_uint8_t *output, rt_size_t *output_length)
{
    rt_uint8_t digest[20];
    rt_size_t length = EAPOL_KEY_FIXED_LENGTH + key_data_length;

    if (!station || !output || !output_length || *output_length < length ||
        length > HOSTAPD_EAPOL_MAX)
    {
        return -RT_EINVAL;
    }
    rt_memset(output, 0, length);
    output[0] = 2;
    output[1] = EAPOL_TYPE_KEY;
    hostapd_put_be16(output + 2, (rt_uint16_t)(length - 4U));
    output[4] = EAPOL_KEY_DESCRIPTOR_RSN;
    hostapd_put_be16(output + 5, key_info);
    /* Key Length carries the pairwise cipher's key length. */
    hostapd_put_be16(output + 7, WPA_TK_LENGTH);
    rt_memcpy(output + 9, station->replay, sizeof(station->replay));
    if (nonce)
    {
        rt_memcpy(output + 17, nonce, WPA_NONCE_LENGTH);
    }
    hostapd_put_be16(output + 97, (rt_uint16_t)key_data_length);
    if (key_data_length)
    {
        rt_memcpy(output + EAPOL_KEY_DATA_OFFSET, key_data, key_data_length);
    }
    if (key_info & WPA_KEY_INFO_MIC)
    {
        rt_wlan_offload_hmac_sha1(station->ptk, WPA_KCK_LENGTH,
                             output, length, digest);
        rt_memcpy(output + EAPOL_KEY_MIC_OFFSET, digest, WPA_KCK_LENGTH);
        rt_wlan_offload_crypto_zero(digest, sizeof(digest));
    }
    *output_length = length;
    return RT_EOK;
}

static rt_err_t hostapd_build_message1_locked(
    struct rt_wlan_offload_hostapd *hostapd,
    struct wlan_offload_hostapd_station *station)
{
    rt_size_t length = sizeof(station->last_eapol);

    hostapd_next_replay_locked(hostapd, station->replay);
    if (hostapd_random(station->anonce, sizeof(station->anonce)) != RT_EOK)
    {
        return -RT_EIO;
    }
    if (hostapd_make_key_frame(
            station, WPA_KEY_INFO_VERSION_2 | WPA_KEY_INFO_PAIRWISE |
                     WPA_KEY_INFO_ACK,
            station->anonce, RT_NULL, 0, station->last_eapol,
            &length) != RT_EOK)
    {
        return -RT_ERROR;
    }
    station->last_eapol_length = length;
    station->state = HOSTAPD_STATION_WAIT_M2;
    station->retries = 0;
    station->retry_deadline = rt_tick_get() +
        rt_tick_from_millisecond(HOSTAPD_RETRY_INTERVAL_MS);
    return RT_EOK;
}

static rt_err_t hostapd_build_message3_locked(
    struct rt_wlan_offload_hostapd *hostapd,
    struct wlan_offload_hostapd_station *station)
{
    rt_uint8_t plain[48];
    rt_uint8_t wrapped[56];
    rt_size_t wrapped_length = sizeof(wrapped);
    rt_size_t length = sizeof(station->last_eapol);

    rt_memset(plain, 0, sizeof(plain));
    rt_memcpy(plain, g_hostapd_rsn_ie, sizeof(g_hostapd_rsn_ie));
    plain[22] = 0xdd;
    plain[23] = 22;
    plain[24] = 0x00;
    plain[25] = 0x0f;
    plain[26] = 0xac;
    plain[27] = 0x01; /* GTK KDE */
    plain[28] = 0x01; /* key index 1; stations receive group traffic */
    plain[29] = 0x00;
    rt_memcpy(plain + 30, hostapd->gtk, WPA_GTK_LENGTH);
    plain[46] = 0xdd; /* key-data padding */
    plain[47] = 0x00;
    if (rt_wlan_offload_aes_wrap(station->ptk + WPA_KEK_OFFSET,
                            plain, sizeof(plain), wrapped,
                            &wrapped_length) != 0)
    {
        return -RT_ERROR;
    }
    hostapd_next_replay_locked(hostapd, station->replay);
    if (hostapd_make_key_frame(
            station, WPA_KEY_INFO_VERSION_2 | WPA_KEY_INFO_PAIRWISE |
                     WPA_KEY_INFO_INSTALL | WPA_KEY_INFO_ACK |
                     WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE |
                     WPA_KEY_INFO_ENCRYPTED,
            station->anonce, wrapped, wrapped_length,
            station->last_eapol, &length) != RT_EOK)
    {
        rt_wlan_offload_crypto_zero(plain, sizeof(plain));
        rt_wlan_offload_crypto_zero(wrapped, sizeof(wrapped));
        return -RT_ERROR;
    }
    station->last_eapol_length = length;
    station->state = HOSTAPD_STATION_WAIT_M4;
    station->retries = 0;
    station->retry_deadline = rt_tick_get() +
        rt_tick_from_millisecond(HOSTAPD_RETRY_INTERVAL_MS);
    rt_wlan_offload_crypto_zero(plain, sizeof(plain));
    rt_wlan_offload_crypto_zero(wrapped, sizeof(wrapped));
    return RT_EOK;
}

static rt_err_t hostapd_send_eapol(
    struct rt_wlan_offload_hostapd *hostapd,
    const struct wlan_offload_hostapd_station *station)
{
    return rt_wlan_offload_transmit_eapol(
        hostapd->radio, RT_WLAN_OFFLOAD_IFTYPE_AP, station->mac,
        station->last_eapol, station->last_eapol_length);
}

static rt_err_t hostapd_send_management(
    struct rt_wlan_offload_hostapd *hostapd, const rt_uint8_t *frame,
    rt_size_t length)
{
    struct rt_wlan_offload_mgmt_frame request;

    rt_memset(&request, 0, sizeof(request));
    request.request_id = rt_wlan_offload_alloc_request_id(hostapd->radio);
    request.channel = hostapd->channel;
    request.cookie = request.request_id;
    request.data = frame;
    request.length = length;
    return request.request_id ? rt_wlan_offload_transmit_mgmt(
        hostapd->radio, RT_WLAN_OFFLOAD_IFTYPE_AP, &request) : -RT_EIO;
}

static void hostapd_report_station(struct rt_wlan_offload_hostapd *hostapd,
                                   const rt_uint8_t mac[6], rt_uint16_t aid,
                                   rt_bool_t connected)
{
    struct rt_wlan_offload_event event;

    rt_memset(&event, 0, sizeof(event));
    event.type = connected ? RT_WLAN_OFFLOAD_EVENT_NEW_STATION :
                             RT_WLAN_OFFLOAD_EVENT_DEL_STATION;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
    event.status = RT_EOK;
    rt_memcpy(event.data.station.mac, mac, 6);
    event.data.station.aid = aid;
    (void)rt_wlan_offload_report_event(hostapd->radio, &event);
}

static rt_err_t hostapd_remove_station(
    struct rt_wlan_offload_hostapd *hostapd,
    struct wlan_offload_hostapd_station *station, rt_uint16_t reason,
    rt_bool_t transmit_deauth)
{
    rt_uint8_t mac[6];
    rt_uint16_t aid;
    rt_bool_t firmware_added;
    rt_bool_t reported;
    rt_err_t result = RT_EOK;

    rt_memcpy(mac, station->mac, sizeof(mac));
    aid = station->aid;
    firmware_added = station->firmware_added;
    reported = station->reported;
    if (transmit_deauth)
    {
        rt_uint8_t frame[26] = {0};

        hostapd_put_le16(frame, WLAN_FC_DEAUTH);
        rt_memcpy(frame + 4, mac, 6);
        rt_memcpy(frame + 10, hostapd->address, 6);
        rt_memcpy(frame + 16, hostapd->address, 6);
        hostapd_put_le16(frame + 24, reason);
        (void)hostapd_send_management(hostapd, frame, sizeof(frame));
    }
    if (firmware_added)
    {
        rt_uint32_t request_id = rt_wlan_offload_alloc_request_id(hostapd->radio);

        if (request_id)
        {
            result = rt_wlan_offload_del_station(
                hostapd->radio, RT_WLAN_OFFLOAD_IFTYPE_AP, request_id,
                mac, reason);
        }
    }
    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    hostapd_station_clear(station);
    rt_mutex_release(&hostapd->lock);
    if (reported)
    {
        hostapd_report_station(hostapd, mac, aid, RT_FALSE);
    }
    return result;
}

static void hostapd_timeout(void *parameter)
{
    struct rt_wlan_offload_hostapd *hostapd = parameter;
    struct
    {
        rt_uint8_t mac[6];
        rt_uint8_t eapol[HOSTAPD_EAPOL_MAX];
        rt_size_t length;
    } retry[HOSTAPD_MAX_STATIONS];
    rt_uint8_t expired[HOSTAPD_MAX_STATIONS][6];
    rt_size_t retry_count = 0;
    rt_size_t expired_count = 0;
    rt_size_t index;
    rt_tick_t now = rt_tick_get();

    if (!hostapd)
    {
        return;
    }
    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    if (hostapd->active)
    {
        for (index = 0; index < HOSTAPD_MAX_STATIONS; index++)
        {
            struct wlan_offload_hostapd_station *station =
                &hostapd->stations[index];

            if ((station->state != HOSTAPD_STATION_WAIT_M2 &&
                 station->state != HOSTAPD_STATION_WAIT_M4) ||
                (rt_int32_t)(now - station->retry_deadline) < 0)
            {
                continue;
            }
            if (station->retries < HOSTAPD_MAX_RETRIES)
            {
                station->retries++;
                station->retry_deadline = now +
                    rt_tick_from_millisecond(HOSTAPD_RETRY_INTERVAL_MS);
                rt_memcpy(retry[retry_count].mac, station->mac, 6);
                retry[retry_count].length = station->last_eapol_length;
                rt_memcpy(retry[retry_count].eapol, station->last_eapol,
                          station->last_eapol_length);
                retry_count++;
            }
            else
            {
                rt_memcpy(expired[expired_count++], station->mac, 6);
            }
        }
    }
    rt_mutex_release(&hostapd->lock);
    for (index = 0; index < retry_count; index++)
    {
        (void)rt_wlan_offload_transmit_eapol(
            hostapd->radio, RT_WLAN_OFFLOAD_IFTYPE_AP, retry[index].mac,
            retry[index].eapol, retry[index].length);
    }
    for (index = 0; index < expired_count; index++)
    {
        struct wlan_offload_hostapd_station *station;

        rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
        station = hostapd_find_station_locked(hostapd, expired[index]);
        if (!station ||
            (station->state != HOSTAPD_STATION_WAIT_M2 &&
             station->state != HOSTAPD_STATION_WAIT_M4) ||
            station->retries < HOSTAPD_MAX_RETRIES ||
            (rt_int32_t)(now - station->retry_deadline) < 0)
        {
            rt_mutex_release(&hostapd->lock);
            continue;
        }
        rt_mutex_release(&hostapd->lock);
        LOG_W("station %02x:%02x:%02x:%02x:%02x:%02x WPA timeout",
              expired[index][0], expired[index][1], expired[index][2],
              expired[index][3], expired[index][4], expired[index][5]);
        (void)hostapd_remove_station(hostapd, station,
            WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT, RT_TRUE);
    }
}

static struct rt_wlan_offload_hostapd *hostapd_get_or_create(
    struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_hostapd *hostapd;
    rt_size_t index;

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    hostapd = radio->hostapd;
    rt_mutex_release(&radio->operation_lock);
    if (hostapd)
    {
        return hostapd;
    }
    hostapd = rt_calloc(1, sizeof(*hostapd));
    if (!hostapd)
    {
        return RT_NULL;
    }
    hostapd->radio = radio;
    if (rt_mutex_init(&hostapd->lock, "fmhostap", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_free(hostapd);
        return RT_NULL;
    }
    for (index = 0; index < HOSTAPD_MAX_STATIONS; index++)
    {
        hostapd->stations[index].hostapd = hostapd;
    }
    hostapd->timer = rt_timer_create(
        "fmhostap", hostapd_timeout, hostapd,
        rt_tick_from_millisecond(500),
        RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    if (!hostapd->timer)
    {
        rt_mutex_detach(&hostapd->lock);
        rt_free(hostapd);
        return RT_NULL;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (!radio->hostapd)
    {
        radio->hostapd = hostapd;
        hostapd = RT_NULL;
    }
    rt_mutex_release(&radio->operation_lock);
    if (hostapd)
    {
        rt_timer_delete(hostapd->timer);
        rt_mutex_detach(&hostapd->lock);
        rt_free(hostapd);
    }
    return radio->hostapd;
}

rt_bool_t rt_wlan_offload_hostapd_supports(rt_wlan_security_t security)
{
    return security == SECURITY_OPEN || security == SECURITY_WPA2_AES_PSK;
}

rt_err_t rt_wlan_offload_hostapd_prepare(
    struct rt_wlan_offload_radio *radio,
    struct rt_wlan_offload_ap_settings *settings)
{
    struct rt_wlan_offload_hostapd *hostapd;
    struct rt_wlan_offload_vif *vif;
    rt_uint8_t pmk[WPA_PMK_LENGTH] = {0};
    rt_uint8_t gtk[WPA_GTK_LENGTH] = {0};
    rt_bool_t secure;
    rt_bool_t raw_psk = RT_FALSE;
    rt_uint8_t max_stations;
    rt_err_t result = RT_EOK;

    if (!radio || !settings || !settings->request_id ||
        !settings->ssid.len || settings->ssid.len > RT_WLAN_SSID_MAX_LENGTH ||
        !rt_wlan_offload_hostapd_supports(settings->security) ||
        !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR))
    {
        return -RT_ENOSYS;
    }
    secure = settings->security == SECURITY_WPA2_AES_PSK;
    max_stations = settings->max_stations ? settings->max_stations :
                                            HOSTAPD_MAX_STATIONS;
    if (radio->firmware_info.max_stations &&
        max_stations > radio->firmware_info.max_stations)
    {
        max_stations = (rt_uint8_t)radio->firmware_info.max_stations;
    }
    if (max_stations > HOSTAPD_MAX_STATIONS)
    {
        max_stations = HOSTAPD_MAX_STATIONS;
    }
    if (!max_stations)
    {
        return -RT_EFULL;
    }
    if ((!secure && settings->key.len) ||
        (secure && (settings->key.len < 8U || settings->key.len > 64U)))
    {
        return -RT_EINVAL;
    }
    if (secure)
    {
        raw_psk = hostapd_decode_raw_psk(&settings->key, pmk);
        if (!raw_psk && settings->key.len == 64U)
        {
            return -RT_EINVAL;
        }
        if (!raw_psk && rt_wlan_offload_pbkdf2_sha1(
                settings->key.val, settings->key.len,
                settings->ssid.val, settings->ssid.len, pmk) != 0)
        {
            return -RT_ERROR;
        }
        result = hostapd_random(gtk, sizeof(gtk));
        if (result != RT_EOK)
        {
            rt_wlan_offload_crypto_zero(pmk, sizeof(pmk));
            return result;
        }
    }
    hostapd = hostapd_get_or_create(radio);
    if (!hostapd)
    {
        rt_wlan_offload_crypto_zero(pmk, sizeof(pmk));
        rt_wlan_offload_crypto_zero(gtk, sizeof(gtk));
        return -RT_ENOMEM;
    }
    vif = rt_wlan_offload_get_vif(radio, RT_WLAN_OFFLOAD_IFTYPE_AP);
    if (!vif)
    {
        rt_wlan_offload_crypto_zero(pmk, sizeof(pmk));
        rt_wlan_offload_crypto_zero(gtk, sizeof(gtk));
        return -RT_EINVAL;
    }
    rt_timer_stop(hostapd->timer);
    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    hostapd_clear_locked(hostapd);
    hostapd->prepared = RT_TRUE;
    hostapd->secure = secure;
    hostapd->max_stations = max_stations;
    hostapd->request_id = settings->request_id;
    hostapd->ssid = settings->ssid;
    hostapd->channel = settings->channel;
    rt_memcpy(hostapd->address, vif->address, 6);
    rt_memcpy(hostapd->pmk, pmk, sizeof(pmk));
    rt_memcpy(hostapd->gtk, gtk, sizeof(gtk));
    rt_mutex_release(&hostapd->lock);
    if (secure)
    {
        settings->beacon_ies = g_hostapd_rsn_ie;
        settings->beacon_ies_length = sizeof(g_hostapd_rsn_ie);
    }
    rt_wlan_offload_crypto_zero(pmk, sizeof(pmk));
    rt_wlan_offload_crypto_zero(gtk, sizeof(gtk));
    return RT_EOK;
}

void rt_wlan_offload_hostapd_cancel(struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_hostapd *hostapd = hostapd_lock_from_radio(radio);

    if (!hostapd)
    {
        return;
    }
    rt_timer_stop(hostapd->timer);
    hostapd_clear_locked(hostapd);
    rt_mutex_release(&hostapd->lock);
}

void rt_wlan_offload_hostapd_deinit(struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_hostapd *hostapd;

    if (!radio)
    {
        return;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    hostapd = radio->hostapd;
    radio->hostapd = RT_NULL;
    rt_mutex_release(&radio->operation_lock);
    if (!hostapd)
    {
        return;
    }
    rt_timer_stop(hostapd->timer);
    rt_timer_delete(hostapd->timer);
    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    hostapd_clear_locked(hostapd);
    rt_mutex_release(&hostapd->lock);
    rt_mutex_detach(&hostapd->lock);
    rt_free(hostapd);
}

static rt_err_t hostapd_send_auth_response(
    struct rt_wlan_offload_hostapd *hostapd, const rt_uint8_t station[6],
    rt_uint16_t algorithm, rt_uint16_t transaction, rt_uint16_t status)
{
    rt_uint8_t frame[30] = {0};

    hostapd_put_le16(frame, WLAN_FC_AUTH);
    rt_memcpy(frame + 4, station, 6);
    rt_memcpy(frame + 10, hostapd->address, 6);
    rt_memcpy(frame + 16, hostapd->address, 6);
    hostapd_put_le16(frame + 24, algorithm);
    hostapd_put_le16(frame + 26, transaction);
    hostapd_put_le16(frame + 28, status);
    return hostapd_send_management(hostapd, frame, sizeof(frame));
}

static rt_err_t hostapd_handle_auth(
    struct rt_wlan_offload_hostapd *hostapd, const rt_uint8_t *frame,
    rt_size_t length)
{
    struct wlan_offload_hostapd_station *station;
    rt_uint16_t algorithm;
    rt_uint16_t transaction;
    rt_uint16_t status = WLAN_STATUS_SUCCESS;

    if (length < 30U)
    {
        return -RT_EINVAL;
    }
    algorithm = hostapd_get_le16(frame + 24);
    transaction = hostapd_get_le16(frame + 26);
    if (algorithm != WLAN_AUTH_OPEN)
    {
        status = WLAN_STATUS_NOT_SUPPORTED_AUTH_ALG;
    }
    else if (transaction != 1U)
    {
        status = WLAN_STATUS_UNSPECIFIED_FAILURE;
    }
    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    station = hostapd_find_station_locked(hostapd, frame + 10);
    if (status == WLAN_STATUS_SUCCESS && station &&
        station->state != HOSTAPD_STATION_AUTHENTICATED)
    {
        rt_mutex_release(&hostapd->lock);
        (void)hostapd_remove_station(
            hostapd, station, WLAN_REASON_PREV_AUTH_NOT_VALID, RT_FALSE);
        rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
        station = RT_NULL;
    }
    if (status == WLAN_STATUS_SUCCESS && !station)
    {
        station = hostapd_allocate_station_locked(hostapd, frame + 10);
        if (!station)
        {
            status = WLAN_STATUS_UNSPECIFIED_FAILURE;
        }
    }
    rt_mutex_release(&hostapd->lock);
    return hostapd_send_auth_response(hostapd, frame + 10, algorithm,
                                      2U, status);
}

static rt_bool_t hostapd_find_ie(const rt_uint8_t *ies, rt_size_t length,
                                 rt_uint8_t id, const rt_uint8_t **body,
                                 rt_size_t *body_length)
{
    rt_size_t offset = 0;

    while (ies && offset + 2U <= length)
    {
        rt_size_t element_length = ies[offset + 1U];

        if (offset + 2U + element_length > length)
        {
            return RT_FALSE;
        }
        if (ies[offset] == id)
        {
            if (body) *body = ies + offset + 2U;
            if (body_length) *body_length = element_length;
            return RT_TRUE;
        }
        offset += 2U + element_length;
    }
    return RT_FALSE;
}

static rt_err_t hostapd_send_assoc_response(
    struct rt_wlan_offload_hostapd *hostapd, const rt_uint8_t station[6],
    rt_uint16_t aid, rt_uint16_t status, rt_bool_t reassociation)
{
    static const rt_uint8_t rates_2ghz[] = {
        0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
        0x32, 0x04, 0x30, 0x48, 0x60, 0x6c
    };
    static const rt_uint8_t rates_5ghz[] = {
        0x01, 0x08, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c
    };
    static const rt_uint8_t wmm[] = {
        0xdd, 0x18, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x80, 0x00,
        0x03, 0xa4, 0x00, 0x00, 0x27, 0xa4, 0x00, 0x00,
        0x42, 0x43, 0x5e, 0x00, 0x62, 0x32, 0x2f, 0x00
    };
    static const rt_uint8_t vht_capability[] = {
        0xbf, 0x0c,
        0x32, 0x01, 0x80, 0x03,
        0xfe, 0xff, 0x86, 0x01,
        0xfe, 0xff, 0x86, 0x01
    };
    rt_uint8_t ht_cap[28] = {0x2d, 0x1a, 0x63, 0x09, 0x1f, 0xff};
    rt_uint8_t ht_operation[24] = {0x3d, 0x16};
    rt_uint8_t vht_operation[7] = {0xc0, 0x05};
    rt_uint8_t frame[192] = {0};
    rt_size_t length = 30U;

    hostapd_put_le16(frame, reassociation ? WLAN_FC_REASSOC_RESP :
                                           WLAN_FC_ASSOC_RESP);
    rt_memcpy(frame + 4, station, 6);
    rt_memcpy(frame + 10, hostapd->address, 6);
    rt_memcpy(frame + 16, hostapd->address, 6);
    hostapd_put_le16(frame + 24, hostapd->secure ? 0x0411U : 0x0401U);
    hostapd_put_le16(frame + 26, status);
    hostapd_put_le16(frame + 28, status == WLAN_STATUS_SUCCESS ?
                     (rt_uint16_t)(aid | 0xc000U) : 0U);
    if (hostapd->channel.band == RT_WLAN_OFFLOAD_BAND_5GHZ)
    {
        rt_memcpy(frame + length, rates_5ghz, sizeof(rates_5ghz));
        length += sizeof(rates_5ghz);
    }
    else
    {
        rt_memcpy(frame + length, rates_2ghz, sizeof(rates_2ghz));
        length += sizeof(rates_2ghz);
    }
    ht_operation[2] = (rt_uint8_t)hostapd->channel.primary_channel;
    if (hostapd->channel.width == RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80)
    {
        rt_int16_t center_offset =
            (rt_int16_t)hostapd->channel.center_frequency1_mhz -
            (rt_int16_t)hostapd->channel.primary_frequency_mhz;

        ht_operation[3] = (center_offset == 30 || center_offset == -10) ?
                          0x05U : 0x07U;
    }
    rt_memcpy(frame + length, ht_cap, sizeof(ht_cap));
    length += sizeof(ht_cap);
    rt_memcpy(frame + length, ht_operation, sizeof(ht_operation));
    length += sizeof(ht_operation);
    if (hostapd->channel.width == RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80)
    {
        vht_operation[2] = 1;
        vht_operation[3] = hostapd_frequency_to_channel(
            hostapd->channel.center_frequency1_mhz);
        vht_operation[5] = 0xfc;
        vht_operation[6] = 0xff;
        rt_memcpy(frame + length, vht_capability, sizeof(vht_capability));
        length += sizeof(vht_capability);
        rt_memcpy(frame + length, vht_operation, sizeof(vht_operation));
        length += sizeof(vht_operation);
    }
    rt_memcpy(frame + length, wmm, sizeof(wmm));
    length += sizeof(wmm);
    if (status == WLAN_STATUS_SUCCESS && hostapd->secure)
    {
        rt_memcpy(frame + length, g_hostapd_rsn_ie,
                  sizeof(g_hostapd_rsn_ie));
        length += sizeof(g_hostapd_rsn_ie);
    }
    return hostapd_send_management(hostapd, frame, length);
}

static rt_err_t hostapd_install_gtk(struct rt_wlan_offload_hostapd *hostapd)
{
    struct rt_wlan_offload_key key;
    rt_uint32_t request_id;
    rt_err_t result;

    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    if (hostapd->gtk_installed)
    {
        rt_mutex_release(&hostapd->lock);
        return RT_EOK;
    }
    rt_memset(&key, 0, sizeof(key));
    key.cipher = RT_WLAN_OFFLOAD_CIPHER_CCMP;
    key.index = 1;
    key.set_transmit = RT_TRUE;
    key.key_length = WPA_GTK_LENGTH;
    rt_memcpy(key.key, hostapd->gtk, WPA_GTK_LENGTH);
    rt_mutex_release(&hostapd->lock);
    request_id = rt_wlan_offload_alloc_request_id(hostapd->radio);
    result = request_id ? rt_wlan_offload_add_key(
        hostapd->radio, RT_WLAN_OFFLOAD_IFTYPE_AP, request_id, &key) :
        -RT_EIO;
    if (result == RT_EOK)
    {
        rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
        hostapd->gtk_installed = RT_TRUE;
        rt_mutex_release(&hostapd->lock);
    }
    rt_wlan_offload_crypto_zero(&key, sizeof(key));
    return result;
}

static rt_err_t hostapd_handle_assoc(
    struct rt_wlan_offload_hostapd *hostapd, const rt_uint8_t *frame,
    rt_size_t length, rt_bool_t reassociation)
{
    struct wlan_offload_hostapd_station *station;
    struct rt_wlan_offload_station_parameters parameters;
    const rt_uint8_t *ies;
    const rt_uint8_t *ssid;
    rt_size_t ies_length;
    rt_size_t ssid_length;
    rt_uint16_t status = WLAN_STATUS_SUCCESS;
    rt_uint16_t aid = 0;
    rt_uint32_t request_id;
    rt_err_t result;
    rt_uint8_t mac[6];
    rt_uint8_t eapol[HOSTAPD_EAPOL_MAX];
    rt_size_t eapol_length = 0;

    if (length < (reassociation ? 34U : 28U))
    {
        return -RT_EINVAL;
    }
    ies = frame + (reassociation ? 34U : 28U);
    ies_length = length - (reassociation ? 34U : 28U);
    if (!hostapd_find_ie(ies, ies_length, 0, &ssid, &ssid_length) ||
        ssid_length != hostapd->ssid.len ||
        rt_memcmp(ssid, hostapd->ssid.val, ssid_length) != 0 ||
        (hostapd->secure && !hostapd_rsn_ie_valid(ies, ies_length)))
    {
        status = WLAN_STATUS_UNSPECIFIED_FAILURE;
    }
    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    station = hostapd_find_station_locked(hostapd, frame + 10);
    if (!station || station->state != HOSTAPD_STATION_AUTHENTICATED)
    {
        status = WLAN_STATUS_UNSPECIFIED_FAILURE;
    }
    if (station)
    {
        aid = station->aid;
    }
    rt_mutex_release(&hostapd->lock);
    if (status != WLAN_STATUS_SUCCESS)
    {
        return hostapd_send_assoc_response(hostapd, frame + 10, aid, status,
                                            reassociation);
    }
    rt_memcpy(mac, frame + 10, 6);
    rt_memset(&parameters, 0, sizeof(parameters));
    rt_memcpy(parameters.mac, mac, 6);
    parameters.aid = aid;
    parameters.association_ies = ies;
    parameters.association_ies_length = ies_length;
    request_id = rt_wlan_offload_alloc_request_id(hostapd->radio);
    result = request_id ? rt_wlan_offload_add_station(
        hostapd->radio, RT_WLAN_OFFLOAD_IFTYPE_AP, request_id, &parameters) :
        -RT_EIO;
    if (result != RT_EOK)
    {
        (void)hostapd_send_assoc_response(
            hostapd, frame + 10, aid, WLAN_STATUS_UNSPECIFIED_FAILURE,
            reassociation);
        return result;
    }
    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    station = hostapd_find_station_locked(hostapd, mac);
    if (!station)
    {
        rt_mutex_release(&hostapd->lock);
        return -RT_EIO;
    }
    station->firmware_added = RT_TRUE;
    if (hostapd->secure)
    {
        result = hostapd_build_message1_locked(hostapd, station);
        if (result == RT_EOK)
        {
            /* Snapshot message 1 while the slot is still known to belong to
             * this station.  Everything below runs with the lock dropped, and
             * an application thread calling rt_wlan_offload_hostapd_deauth()
             * or _deinit() can clear and recycle the slot in that window. */
            eapol_length = station->last_eapol_length;
            rt_memcpy(eapol, station->last_eapol, eapol_length);
        }
    }
    rt_mutex_release(&hostapd->lock);
    if (result == RT_EOK)
    {
        result = hostapd_send_assoc_response(
            hostapd, mac, aid, WLAN_STATUS_SUCCESS, reassociation);
    }
    if (result == RT_EOK && hostapd->secure)
    {
        result = rt_wlan_offload_transmit_eapol(
            hostapd->radio, RT_WLAN_OFFLOAD_IFTYPE_AP, mac, eapol,
            eapol_length);
    }
    if (result == RT_EOK && !hostapd->secure)
    {
        request_id = rt_wlan_offload_alloc_request_id(hostapd->radio);
        result = request_id ? rt_wlan_offload_set_station_authorized(
            hostapd->radio, RT_WLAN_OFFLOAD_IFTYPE_AP, request_id,
            mac, RT_TRUE) : -RT_EIO;
    }
    /* Re-resolve the station under the lock rather than reusing the pointer
     * captured above: the slot may have been freed, or reassigned to a
     * different peer, while the lock was dropped. */
    rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
    station = hostapd_find_station_locked(hostapd, mac);
    if (!station)
    {
        rt_mutex_release(&hostapd->lock);
        return result == RT_EOK ? -RT_EIO : result;
    }
    if (result == RT_EOK && !hostapd->secure)
    {
        station->state = HOSTAPD_STATION_AUTHORIZED;
        station->reported = RT_TRUE;
        rt_mutex_release(&hostapd->lock);
        hostapd_report_station(hostapd, mac, aid, RT_TRUE);
    }
    else if (result != RT_EOK)
    {
        rt_mutex_release(&hostapd->lock);
        (void)hostapd_remove_station(
            hostapd, station, WLAN_REASON_PREV_AUTH_NOT_VALID, RT_TRUE);
    }
    else
    {
        rt_mutex_release(&hostapd->lock);
    }
    return result;
}

static rt_bool_t hostapd_handle_management(
    struct rt_wlan_offload_hostapd *hostapd,
    const struct rt_wlan_offload_event *event)
{
    const rt_uint8_t *frame = event->data.management.data;
    rt_size_t length = event->data.management.length;
    rt_uint16_t frame_control;
    rt_uint16_t type;
    struct wlan_offload_hostapd_station *station;

    if (!frame || length < 24U ||
        (!hostapd_mac_equal(frame + 4, hostapd->address) &&
         !hostapd_mac_is_broadcast(frame + 4)))
    {
        return RT_FALSE;
    }
    frame_control = hostapd_get_le16(frame);
    type = frame_control & 0x00fcU;
    if (type == WLAN_FC_AUTH)
    {
        (void)hostapd_handle_auth(hostapd, frame, length);
        return RT_TRUE;
    }
    if (type == WLAN_FC_ASSOC_REQ || type == WLAN_FC_REASSOC_REQ)
    {
        (void)hostapd_handle_assoc(hostapd, frame, length,
                                   type == WLAN_FC_REASSOC_REQ);
        return RT_TRUE;
    }
    if (type == WLAN_FC_DEAUTH || type == WLAN_FC_DISASSOC)
    {
        rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
        station = hostapd_find_station_locked(hostapd, frame + 10);
        rt_mutex_release(&hostapd->lock);
        if (station)
        {
            (void)hostapd_remove_station(
                hostapd, station,
                length >= 26U ? hostapd_get_le16(frame + 24) :
                                WLAN_REASON_PREV_AUTH_NOT_VALID,
                RT_FALSE);
        }
        return RT_TRUE;
    }
    return RT_FALSE;
}

rt_err_t rt_wlan_offload_hostapd_deauth(
    struct rt_wlan_offload_radio *radio, const rt_uint8_t mac[6],
    rt_uint16_t reason)
{
    struct rt_wlan_offload_hostapd *hostapd;
    struct wlan_offload_hostapd_station *station;

    if (!radio || !mac)
    {
        return -RT_EINVAL;
    }
    hostapd = hostapd_lock_from_radio(radio);
    if (!hostapd || !hostapd->active)
    {
        if (hostapd) rt_mutex_release(&hostapd->lock);
        return -RT_EBUSY;
    }
    station = hostapd_find_station_locked(hostapd, mac);
    rt_mutex_release(&hostapd->lock);
    if (!station)
    {
        return -RT_EINVAL;
    }
    return hostapd_remove_station(
        hostapd, station, reason ? reason : WLAN_REASON_PREV_AUTH_NOT_VALID,
        RT_TRUE);
}

rt_err_t rt_wlan_offload_hostapd_channel_changed(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_channel_definition *channel)
{
    struct rt_wlan_offload_hostapd *hostapd;
    struct
    {
        rt_uint8_t mac[6];
        rt_uint16_t aid;
    } disconnected[HOSTAPD_MAX_STATIONS];
    rt_size_t disconnected_count = 0;
    rt_size_t index;

    if (!radio || !channel || !channel->primary_channel ||
        !channel->primary_frequency_mhz)
    {
        return -RT_EINVAL;
    }
    hostapd = hostapd_lock_from_radio(radio);
    if (!hostapd || !hostapd->active)
    {
        if (hostapd) rt_mutex_release(&hostapd->lock);
        return -RT_EBUSY;
    }
    hostapd->channel = *channel;
    hostapd->gtk_installed = RT_FALSE;
    for (index = 0; index < HOSTAPD_MAX_STATIONS; index++)
    {
        struct wlan_offload_hostapd_station *station =
            &hostapd->stations[index];

        if (station->reported)
        {
            rt_memcpy(disconnected[disconnected_count].mac, station->mac, 6);
            disconnected[disconnected_count].aid = station->aid;
            disconnected_count++;
        }
        hostapd_station_clear(station);
    }
    rt_mutex_release(&hostapd->lock);

    for (index = 0; index < disconnected_count; index++)
    {
        hostapd_report_station(hostapd, disconnected[index].mac,
                               disconnected[index].aid, RT_FALSE);
    }
    return RT_EOK;
}

rt_bool_t rt_wlan_offload_hostapd_filter_event(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_event *event)
{
    struct rt_wlan_offload_hostapd *hostapd;
    rt_bool_t consume = RT_FALSE;

    if (!radio || !event)
    {
        return RT_FALSE;
    }
    hostapd = hostapd_lock_from_radio(radio);
    if (!hostapd)
    {
        return RT_FALSE;
    }
    if (event->type == RT_WLAN_OFFLOAD_EVENT_AP_STARTED &&
        hostapd->prepared &&
        (!hostapd->request_id || event->request_id == hostapd->request_id))
    {
        if (event->status == RT_EOK)
        {
            hostapd->active = RT_TRUE;
            rt_timer_start(hostapd->timer);
            LOG_I("embedded %s SoftAP started",
                  hostapd->secure ? "WPA2-PSK" : "open");
        }
        else
        {
            rt_timer_stop(hostapd->timer);
            hostapd_clear_locked(hostapd);
        }
    }
    else if (event->type == RT_WLAN_OFFLOAD_EVENT_AP_STOPPED ||
             event->type == RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR ||
             event->type == RT_WLAN_OFFLOAD_EVENT_RADIO_OFFLINE)
    {
        rt_timer_stop(hostapd->timer);
        hostapd_clear_locked(hostapd);
    }
    else if (event->type == RT_WLAN_OFFLOAD_EVENT_DEL_STATION &&
             event->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP && hostapd->active)
    {
        struct wlan_offload_hostapd_station *station =
            hostapd_find_station_locked(hostapd, event->data.station.mac);

        if (station)
        {
            hostapd_station_clear(station);
        }
    }
    else if (event->type == RT_WLAN_OFFLOAD_EVENT_MGMT_RX &&
             event->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP && hostapd->active)
    {
        rt_mutex_release(&hostapd->lock);
        return hostapd_handle_management(hostapd, event);
    }
    rt_mutex_release(&hostapd->lock);
    return consume;
}

rt_bool_t rt_wlan_offload_hostapd_handle_eapol(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    const rt_uint8_t source[6], const rt_uint8_t destination[6],
    const rt_uint8_t *data, rt_size_t length, rt_err_t *result)
{
    struct rt_wlan_offload_hostapd *hostapd;
    struct wlan_offload_hostapd_station *station;
    rt_uint16_t body_length;
    rt_uint16_t key_info;
    rt_uint16_t key_data_length;
    rt_bool_t message2;
    rt_bool_t message4;
    rt_bool_t install_gtk = RT_FALSE;
    rt_bool_t install_pairwise = RT_FALSE;
    rt_bool_t authorize_station = RT_FALSE;
    rt_err_t status = RT_EOK;

    if (result) *result = RT_EOK;
    if (!radio || iftype != RT_WLAN_OFFLOAD_IFTYPE_AP || !source ||
        !destination || !data || length < EAPOL_KEY_FIXED_LENGTH ||
        data[1] != EAPOL_TYPE_KEY || data[4] != EAPOL_KEY_DESCRIPTOR_RSN)
    {
        return RT_FALSE;
    }
    body_length = hostapd_get_be16(data + 2);
    key_data_length = hostapd_get_be16(data + 97);
    if ((rt_size_t)body_length + 4U > length || body_length < 95U ||
        key_data_length > (rt_size_t)body_length - 95U)
    {
        return RT_FALSE;
    }
    length = (rt_size_t)body_length + 4U;
    key_info = hostapd_get_be16(data + 5);
    if ((key_info & WPA_KEY_INFO_VERSION_MASK) != WPA_KEY_INFO_VERSION_2 ||
        (key_info & (WPA_KEY_INFO_ACK | WPA_KEY_INFO_ERROR |
                     WPA_KEY_INFO_REQUEST)))
    {
        return RT_FALSE;
    }
    message2 = (key_info & (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_MIC |
                            WPA_KEY_INFO_SECURE)) ==
               (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_MIC);
    message4 = (key_info & (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_MIC |
                            WPA_KEY_INFO_SECURE)) ==
               (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_MIC |
                WPA_KEY_INFO_SECURE);
    hostapd = hostapd_lock_from_radio(radio);
    if (!hostapd || !hostapd->active || !hostapd->secure ||
        !hostapd_mac_equal(destination, hostapd->address))
    {
        if (hostapd) rt_mutex_release(&hostapd->lock);
        return RT_FALSE;
    }
    station = hostapd_find_station_locked(hostapd, source);
    if (!station)
    {
        rt_mutex_release(&hostapd->lock);
        return RT_FALSE;
    }
    if (message2 && station->state == HOSTAPD_STATION_WAIT_M2 &&
        rt_memcmp(data + 9, station->replay, sizeof(station->replay)) == 0 &&
        key_data_length && hostapd_rsn_ie_valid(
            data + EAPOL_KEY_DATA_OFFSET, key_data_length))
    {
        rt_memcpy(station->snonce, data + 17, sizeof(station->snonce));
        hostapd_derive_ptk(hostapd, station);
        if (!hostapd_verify_mic(station, data, length))
        {
            /* The replay counter this frame has to echo was sent in the
             * clear in message 1, so anyone who can observe and inject can
             * produce a message 2 that reaches this point with an arbitrary
             * SNonce; only the MIC distinguishes it from the real station.
             * Discard it and let the retry timer resend message 1 instead of
             * removing the station, which would turn one spoofed frame into
             * a forced disconnect. Retrying is safe because each message 2
             * re-derives the PTK from the SNonce it carries, so the values
             * this frame left behind are overwritten by the genuine one. */
            rt_mutex_release(&hostapd->lock);
            LOG_W("discarding message 2/4 with an invalid MIC from "
                  "%02x:%02x:%02x:%02x:%02x:%02x",
                  source[0], source[1], source[2],
                  source[3], source[4], source[5]);
            return RT_TRUE;
        }
        install_gtk = !hostapd->gtk_installed;
        install_pairwise = RT_TRUE;
    }
    else if (message4 && station->state == HOSTAPD_STATION_WAIT_M4 &&
             rt_memcmp(data + 9, station->replay,
                       sizeof(station->replay)) == 0 &&
             hostapd_verify_mic(station, data, length))
    {
        authorize_station = RT_TRUE;
    }
    else
    {
        rt_mutex_release(&hostapd->lock);
        return RT_TRUE;
    }
    rt_mutex_release(&hostapd->lock);

    if (status == RT_EOK && install_gtk)
    {
        status = hostapd_install_gtk(hostapd);
    }
    if (status == RT_EOK && install_pairwise)
    {
        struct rt_wlan_offload_key key;
        rt_uint32_t request_id;

        rt_memset(&key, 0, sizeof(key));
        key.cipher = RT_WLAN_OFFLOAD_CIPHER_CCMP;
        key.pairwise = RT_TRUE;
        key.set_transmit = RT_TRUE;
        rt_memcpy(key.peer, station->mac, 6);
        key.key_length = WPA_TK_LENGTH;
        rt_memcpy(key.key, station->ptk + WPA_TK_OFFSET, WPA_TK_LENGTH);
        request_id = rt_wlan_offload_alloc_request_id(radio);
        status = request_id ? rt_wlan_offload_add_key(
            radio, RT_WLAN_OFFLOAD_IFTYPE_AP, request_id, &key) : -RT_EIO;
        rt_wlan_offload_crypto_zero(&key, sizeof(key));
    }
    if (status == RT_EOK && message2)
    {
        rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
        status = hostapd_build_message3_locked(hostapd, station);
        rt_mutex_release(&hostapd->lock);
        if (status == RT_EOK)
        {
            status = hostapd_send_eapol(hostapd, station);
        }
    }
    if (status == RT_EOK && authorize_station)
    {
        rt_uint32_t request_id = rt_wlan_offload_alloc_request_id(radio);

        status = request_id ? rt_wlan_offload_set_station_authorized(
            radio, RT_WLAN_OFFLOAD_IFTYPE_AP, request_id,
            station->mac, RT_TRUE) : -RT_EIO;
        if (status == RT_EOK)
        {
            rt_mutex_take(&hostapd->lock, RT_WAITING_FOREVER);
            station->state = HOSTAPD_STATION_AUTHORIZED;
            station->reported = RT_TRUE;
            station->last_eapol_length = 0;
            rt_mutex_release(&hostapd->lock);
            hostapd_report_station(hostapd, station->mac,
                                   station->aid, RT_TRUE);
            LOG_I("station %02x:%02x:%02x:%02x:%02x:%02x authorized",
                  station->mac[0], station->mac[1], station->mac[2],
                  station->mac[3], station->mac[4], station->mac[5]);
        }
    }
    if (status != RT_EOK)
    {
        (void)hostapd_remove_station(
            hostapd, station, WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT, RT_TRUE);
    }
    if (result) *result = status;
    return RT_TRUE;
}
