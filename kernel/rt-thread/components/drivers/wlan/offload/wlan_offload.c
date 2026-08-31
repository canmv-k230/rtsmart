/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <wlan_offload.h>

#define DBG_TAG "WLAN.offload"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifdef RT_WLAN_OFFLOAD_CONTROL
#include <wlan_offload_control.h>
#endif

#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
#include "wlan_offload_supplicant.h"
#endif

#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
#include "wlan_offload_hostapd.h"
#endif

#ifdef RT_WLAN_PROT_ENABLE
#include <wlan_prot.h>
#endif

#ifdef RT_WLAN_MANAGE_ENABLE
#include <wlan_mgnt.h>
#endif

#define WLAN_OFFLOAD_DEFAULT_FRAME_SIZE 1600
#define WLAN_OFFLOAD_VIF_STA_INDEX      0
#define WLAN_OFFLOAD_VIF_AP_INDEX       1

static struct rt_wlan_offload_vif *wlan_offload_vif_from_wlan(struct rt_wlan_device *wlan)
{
    /* wlan is deliberately the first member of rt_wlan_offload_vif. */
    return (struct rt_wlan_offload_vif *)wlan;
}

static int wlan_offload_vif_index(enum rt_wlan_offload_iftype iftype)
{
    if (iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION)
    {
        return WLAN_OFFLOAD_VIF_STA_INDEX;
    }
    if (iftype == RT_WLAN_OFFLOAD_IFTYPE_AP)
    {
        return WLAN_OFFLOAD_VIF_AP_INDEX;
    }
    return -1;
}

static struct rt_wlan_offload_vif *wlan_offload_get_vif_locked(
    struct rt_wlan_offload_radio *radio, enum rt_wlan_offload_iftype iftype)
{
    int index = wlan_offload_vif_index(iftype);

    if (index < 0 || !radio->vifs[index].registered)
    {
        return RT_NULL;
    }
    return &radio->vifs[index];
}

static rt_err_t wlan_offload_operation_enter(struct rt_wlan_offload_radio *radio)
{
    rt_err_t result;

    if (!radio || radio->state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&radio->command_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_mutex_release(&radio->command_lock);
        return result;
    }
    if (radio->state != RT_WLAN_OFFLOAD_STARTED)
    {
        rt_mutex_release(&radio->operation_lock);
        rt_mutex_release(&radio->command_lock);
        return -RT_EBUSY;
    }
    rt_mutex_release(&radio->operation_lock);
    return RT_EOK;
}

static void wlan_offload_operation_exit(struct rt_wlan_offload_radio *radio)
{
    rt_mutex_release(&radio->command_lock);
}

static rt_err_t wlan_offload_data_enter(struct rt_wlan_offload_vif *vif)
{
    struct rt_wlan_offload_radio *radio;
    rt_err_t result;

    if (!vif || !vif->radio)
    {
        return -RT_EINVAL;
    }
    radio = vif->radio;
    result = rt_mutex_take(&radio->data_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_mutex_release(&radio->data_lock);
        return result;
    }
    if (radio->state != RT_WLAN_OFFLOAD_STARTED || !vif->registered || !vif->enabled)
    {
        result = -RT_EBUSY;
    }
    rt_mutex_release(&radio->operation_lock);
    if (result != RT_EOK)
    {
        rt_mutex_release(&radio->data_lock);
    }
    return result;
}

static void wlan_offload_data_exit(struct rt_wlan_offload_radio *radio)
{
    rt_mutex_release(&radio->data_lock);
}

static rt_uint32_t wlan_offload_alloc_request_id_internal(
    struct rt_wlan_offload_radio *radio)
{
    rt_uint32_t request_id;

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    radio->request_sequence++;
    if (radio->request_sequence == 0)
    {
        radio->request_sequence++;
    }
    request_id = radio->request_sequence;
    rt_mutex_release(&radio->operation_lock);
    return request_id;
}

static rt_bool_t wlan_offload_mac_is_zero(const rt_uint8_t mac[6])
{
    static const rt_uint8_t zero[6] = {0};

    return rt_memcmp(mac, zero, sizeof(zero)) == 0;
}

#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
static void wlan_offload_bss_cache_clear_locked(
    struct rt_wlan_offload_radio *radio)
{
    rt_memset(radio->bss_cache, 0, sizeof(radio->bss_cache));
}

static rt_size_t wlan_offload_copy_security_ies(
    rt_uint8_t *destination, rt_size_t capacity,
    const rt_uint8_t *ies, rt_size_t ies_length)
{
    rt_size_t offset = 0;
    rt_size_t copied = 0;

    while (ies && offset + 2U <= ies_length)
    {
        rt_size_t element_length = 2U + ies[offset + 1U];
        rt_bool_t security_element;

        if (element_length > ies_length - offset)
        {
            break;
        }
        security_element = ies[offset] == 48 ||
            (ies[offset] == 221 && element_length >= 6U &&
             ies[offset + 2U] == 0x00 && ies[offset + 3U] == 0x50 &&
             ies[offset + 4U] == 0xf2 && ies[offset + 5U] == 0x01);
        if (security_element)
        {
            if (element_length > capacity - copied)
            {
                return 0;
            }
            rt_memcpy(destination + copied, ies + offset, element_length);
            copied += element_length;
        }
        offset += element_length;
    }
    return copied;
}

static void wlan_offload_bss_cache_update_locked(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_network *network)
{
    struct rt_wlan_offload_bss_cache_entry *entry = RT_NULL;
    struct rt_wlan_offload_bss_cache_entry *empty = RT_NULL;
    struct rt_wlan_offload_bss_cache_entry *weakest = RT_NULL;
    rt_size_t index;

    if (!radio || !network || network->ssid.len > RT_WLAN_SSID_MAX_LENGTH ||
        wlan_offload_mac_is_zero(network->bssid) ||
        !rt_wlan_offload_supplicant_supports(network->security))
    {
        return;
    }

    for (index = 0; index < RT_WLAN_OFFLOAD_BSS_CACHE_COUNT; index++)
    {
        struct rt_wlan_offload_bss_cache_entry *candidate =
            &radio->bss_cache[index];

        if (candidate->valid &&
            rt_memcmp(candidate->bssid, network->bssid,
                      sizeof(candidate->bssid)) == 0)
        {
            entry = candidate;
            break;
        }
        if (!candidate->valid)
        {
            if (!empty)
            {
                empty = candidate;
            }
            continue;
        }
        if (!weakest || candidate->rssi < weakest->rssi)
        {
            weakest = candidate;
        }
    }
    if (!entry)
    {
        entry = empty;
        if (!entry && weakest && network->rssi > weakest->rssi)
        {
            entry = weakest;
        }
    }
    if (!entry)
    {
        return;
    }

    rt_memset(entry, 0, sizeof(*entry));
    entry->valid = RT_TRUE;
    entry->ssid = network->ssid;
    rt_memcpy(entry->bssid, network->bssid, sizeof(entry->bssid));
    entry->channel = network->channel;
    entry->rssi = network->rssi;
    entry->security = network->security;
    entry->beacon_interval = network->beacon_interval;
    entry->capability = network->capability;
    if (network->ies)
    {
        entry->security_ies_length = (rt_uint16_t)wlan_offload_copy_security_ies(
            entry->security_ies, sizeof(entry->security_ies),
            network->ies, network->ies_length);
    }
    if (!entry->security_ies_length)
    {
        entry->valid = RT_FALSE;
    }
}

static rt_bool_t wlan_offload_bss_cache_copy_ies_locked(
    struct rt_wlan_offload_radio *radio, const rt_wlan_ssid_t *ssid,
    const rt_uint8_t bssid[6], rt_uint8_t *ies, rt_size_t *ies_length)
{
    const struct rt_wlan_offload_bss_cache_entry *best = RT_NULL;
    rt_bool_t requested_bssid;
    rt_size_t index;

    if (!radio || !ssid || !ies || !ies_length ||
        ssid->len > RT_WLAN_SSID_MAX_LENGTH)
    {
        return RT_FALSE;
    }
    requested_bssid = bssid && !wlan_offload_mac_is_zero(bssid);
    for (index = 0; index < RT_WLAN_OFFLOAD_BSS_CACHE_COUNT; index++)
    {
        const struct rt_wlan_offload_bss_cache_entry *candidate =
            &radio->bss_cache[index];

        if (!candidate->valid || candidate->security_ies_length == 0 ||
            candidate->ssid.len != ssid->len ||
            rt_memcmp(candidate->ssid.val, ssid->val, ssid->len) != 0 ||
            (requested_bssid && rt_memcmp(candidate->bssid, bssid, 6) != 0))
        {
            continue;
        }
        if (!best || candidate->rssi > best->rssi)
        {
            best = candidate;
        }
    }
    if (!best)
    {
        return RT_FALSE;
    }
    rt_memcpy(ies, best->security_ies, best->security_ies_length);
    *ies_length = best->security_ies_length;
    return RT_TRUE;
}
#endif

static rt_802_11_band_t wlan_offload_to_wlan_band(enum rt_wlan_offload_band_id band)
{
    if (band == RT_WLAN_OFFLOAD_BAND_2GHZ)
    {
        return RT_802_11_BAND_2_4GHZ;
    }
    if (band == RT_WLAN_OFFLOAD_BAND_5GHZ)
    {
        return RT_802_11_BAND_5GHZ;
    }
    return RT_802_11_BAND_UNKNOWN;
}

static rt_err_t wlan_offload_resolve_channel_for_band(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_band_id requested_band, int number,
    rt_bool_t allow_unspecified, rt_bool_t allow_no_ir,
    struct rt_wlan_offload_channel_definition *out)
{
    rt_size_t band_index;

    if (!radio || !out || number < 0 || number > 0xffff ||
        requested_band > RT_WLAN_OFFLOAD_BAND_MAX)
    {
        return -RT_EINVAL;
    }
    rt_memset(out, 0, sizeof(*out));
    out->band = RT_WLAN_OFFLOAD_BAND_MAX;
    out->width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
    if (!number)
    {
        return allow_unspecified ? RT_EOK : -RT_EINVAL;
    }

    for (band_index = 0; band_index < RT_WLAN_OFFLOAD_BAND_MAX; band_index++)
    {
        const struct rt_wlan_offload_supported_band *band = radio->bands[band_index];
        rt_size_t channel_index;

        if (requested_band != RT_WLAN_OFFLOAD_BAND_MAX &&
            band_index != (rt_size_t)requested_band)
        {
            continue;
        }
        if (!band)
        {
            continue;
        }
        for (channel_index = 0; channel_index < band->channel_count;
             channel_index++)
        {
            const struct rt_wlan_offload_channel *channel =
                &band->channels[channel_index];

            if (channel->number == number &&
                !(channel->flags & RT_WLAN_OFFLOAD_CHANNEL_DISABLED) &&
                (allow_no_ir ||
                 !(channel->flags & (RT_WLAN_OFFLOAD_CHANNEL_NO_IR |
                                     RT_WLAN_OFFLOAD_CHANNEL_RADAR))))
            {
                out->band = band->id;
                out->primary_channel = channel->number;
                out->primary_frequency_mhz = channel->center_frequency_mhz;
                out->center_frequency1_mhz = channel->center_frequency_mhz;
                return RT_EOK;
            }
        }
    }
    return -RT_EINVAL;
}

static rt_err_t wlan_offload_resolve_channel(
    struct rt_wlan_offload_radio *radio, int number,
    rt_bool_t allow_unspecified,
    struct rt_wlan_offload_channel_definition *out)
{
    return wlan_offload_resolve_channel_for_band(
        radio, RT_WLAN_OFFLOAD_BAND_MAX, number, allow_unspecified,
        RT_TRUE, out);
}

static rt_bool_t wlan_offload_ap_channel_supports_80mhz(
    const struct rt_wlan_offload_supported_band *band, rt_uint16_t number)
{
    rt_size_t index;

    if (!band)
    {
        return RT_FALSE;
    }
    for (index = 0; index < band->channel_count; index++)
    {
        const struct rt_wlan_offload_channel *channel = &band->channels[index];

        if (channel->number == number)
        {
            return !(channel->flags &
                     (RT_WLAN_OFFLOAD_CHANNEL_DISABLED |
                      RT_WLAN_OFFLOAD_CHANNEL_NO_IR |
                      RT_WLAN_OFFLOAD_CHANNEL_RADAR |
                      RT_WLAN_OFFLOAD_CHANNEL_NO_80MHZ));
        }
    }
    return RT_FALSE;
}

static void wlan_offload_select_compat_ap_width(
    struct rt_wlan_offload_radio *radio,
    struct rt_wlan_offload_channel_definition *definition)
{
    const struct rt_wlan_offload_supported_band *band;
    rt_uint16_t block_start;
    rt_uint16_t center_channel;
    rt_uint16_t index;

    if (!radio || !definition ||
        definition->band != RT_WLAN_OFFLOAD_BAND_5GHZ)
    {
        return;
    }
    band = radio->bands[RT_WLAN_OFFLOAD_BAND_5GHZ];
    if (!band || !(band->phy_capabilities & RT_WLAN_OFFLOAD_PHY_VHT))
    {
        return;
    }
    if (definition->primary_channel >= 36 &&
        definition->primary_channel <= 64)
    {
        block_start = (rt_uint16_t)(36U +
            ((definition->primary_channel - 36U) / 16U) * 16U);
    }
    else if (definition->primary_channel >= 100 &&
             definition->primary_channel <= 144)
    {
        block_start = (rt_uint16_t)(100U +
            ((definition->primary_channel - 100U) / 16U) * 16U);
    }
    else if (definition->primary_channel >= 149 &&
             definition->primary_channel <= 161)
    {
        block_start = 149U;
    }
    else
    {
        return;
    }
    if ((definition->primary_channel - block_start) % 4U)
    {
        return;
    }
    for (index = 0; index < 4U; index++)
    {
        if (!wlan_offload_ap_channel_supports_80mhz(
                band, (rt_uint16_t)(block_start + index * 4U)))
        {
            return;
        }
    }
    center_channel = (rt_uint16_t)(block_start + 6U);
    definition->width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80;
    definition->center_frequency1_mhz =
        (rt_uint16_t)(5000U + center_channel * 5U);
    definition->center_frequency2_mhz = 0;
}

static rt_err_t wlan_offload_validate_channel(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_channel_definition *definition,
    rt_bool_t allow_unspecified)
{
    const struct rt_wlan_offload_supported_band *band;
    rt_uint16_t center_offset;
    rt_size_t index;

    if (!radio || !definition ||
        (int)definition->width < RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20_NOHT ||
        definition->width >= RT_WLAN_OFFLOAD_CHANNEL_WIDTH_MAX)
    {
        return -RT_EINVAL;
    }
    if (!definition->primary_channel)
    {
        return allow_unspecified &&
               definition->band == RT_WLAN_OFFLOAD_BAND_MAX &&
               !definition->primary_frequency_mhz &&
               !definition->center_frequency1_mhz &&
               !definition->center_frequency2_mhz ? RT_EOK : -RT_EINVAL;
    }
    if ((int)definition->band < RT_WLAN_OFFLOAD_BAND_2GHZ ||
        definition->band >= RT_WLAN_OFFLOAD_BAND_MAX ||
        !definition->primary_frequency_mhz ||
        !definition->center_frequency1_mhz ||
        (definition->width == RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80 &&
         !definition->center_frequency2_mhz))
    {
        return -RT_EINVAL;
    }
    band = radio->bands[definition->band];
    if (!band)
    {
        return -RT_EINVAL;
    }
    for (index = 0; index < band->channel_count; index++)
    {
        const struct rt_wlan_offload_channel *channel = &band->channels[index];

        if (channel->number == definition->primary_channel &&
            channel->center_frequency_mhz ==
                definition->primary_frequency_mhz &&
            !(channel->flags & RT_WLAN_OFFLOAD_CHANNEL_DISABLED))
        {
            center_offset = definition->center_frequency1_mhz >
                            definition->primary_frequency_mhz ?
                            definition->center_frequency1_mhz -
                            definition->primary_frequency_mhz :
                            definition->primary_frequency_mhz -
                            definition->center_frequency1_mhz;
            switch (definition->width)
            {
            case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20_NOHT:
            case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20:
                return !center_offset &&
                       !definition->center_frequency2_mhz ?
                       RT_EOK : -RT_EINVAL;
            case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40:
                if (center_offset != 10 ||
                    definition->center_frequency2_mhz)
                {
                    return -RT_EINVAL;
                }
                if ((definition->center_frequency1_mhz >
                     definition->primary_frequency_mhz &&
                     (channel->flags & RT_WLAN_OFFLOAD_CHANNEL_NO_HT40_PLUS)) ||
                    (definition->center_frequency1_mhz <
                     definition->primary_frequency_mhz &&
                     (channel->flags & RT_WLAN_OFFLOAD_CHANNEL_NO_HT40_MINUS)))
                {
                    return -RT_EINVAL;
                }
                return RT_EOK;
            case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80:
                return !(channel->flags & RT_WLAN_OFFLOAD_CHANNEL_NO_80MHZ) &&
                       !definition->center_frequency2_mhz &&
                       (center_offset == 10 || center_offset == 30) ?
                       RT_EOK : -RT_EINVAL;
            case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80:
                return !(channel->flags & RT_WLAN_OFFLOAD_CHANNEL_NO_80MHZ) &&
                       (center_offset == 10 || center_offset == 30) &&
                       definition->center_frequency2_mhz !=
                           definition->center_frequency1_mhz ?
                       RT_EOK : -RT_EINVAL;
            case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_160:
                return !(channel->flags & RT_WLAN_OFFLOAD_CHANNEL_NO_160MHZ) &&
                       !definition->center_frequency2_mhz &&
                       (center_offset == 10 || center_offset == 30 ||
                        center_offset == 50 || center_offset == 70) ?
                       RT_EOK : -RT_EINVAL;
            case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_320:
                return !(channel->flags & RT_WLAN_OFFLOAD_CHANNEL_NO_320MHZ) &&
                       !definition->center_frequency2_mhz &&
                       center_offset >= 10 && center_offset <= 150 &&
                       center_offset % 20 == 10 ? RT_EOK : -RT_EINVAL;
            default:
                return -RT_EINVAL;
            }
        }
    }
    return -RT_EINVAL;
}

/*
 * Maximum PHY data rate advertised by a BSS, in bits per second.
 *
 * rt_wlan_info.datarate is a per-BSS property: rt_wlan_get_info() returns the
 * scan entry cached when the station joined, so the value derived here is what
 * both "wifi scan" and "wifi status" display. It is the link's maximum rate,
 * not the instantaneous rate picked by the firmware's rate control, which the
 * RT-Thread WLAN model provides no way to refresh.
 *
 * The rate is computed from first principles rather than a lookup table:
 *
 *     rate = N_SD * N_BPSCS * R * N_SS / T_sym
 *
 * where N_SD is the data-subcarrier count for the channel width, N_BPSCS the
 * bits per subcarrier per stream, R the coding rate, and T_sym the symbol time
 * (4.0 us, or 3.6 us with the short guard interval).
 */
static rt_uint32_t wlan_offload_data_subcarriers(
    enum rt_wlan_offload_channel_width width)
{
    switch (width)
    {
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40:
        return 108U;
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80:
        return 234U;
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80:
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_160:
        return 468U;
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_320:
        return 980U;
    default:
        return 52U;
    }
}

/* Bits per subcarrier and coding rate for MCS 0 through 9. */
static const struct
{
    rt_uint8_t bits;
    rt_uint8_t numerator;
    rt_uint8_t denominator;
} g_wlan_offload_mcs[10] = {
    {1, 1, 2}, {2, 1, 2}, {2, 3, 4}, {4, 1, 2}, {4, 3, 4},
    {6, 2, 3}, {6, 3, 4}, {6, 5, 6}, {8, 3, 4}, {8, 5, 6},
};

static rt_uint32_t wlan_offload_mcs_rate_kbps(
    enum rt_wlan_offload_channel_width width, rt_uint8_t mcs,
    rt_uint8_t streams, rt_bool_t short_guard)
{
    /* Symbol time in tenths of a microsecond. */
    rt_uint32_t symbol = short_guard ? 36U : 40U;
    rt_uint32_t subcarriers = wlan_offload_data_subcarriers(width);

    if (mcs >= sizeof(g_wlan_offload_mcs) / sizeof(g_wlan_offload_mcs[0]) ||
        !streams || streams > 8U)
    {
        return 0;
    }
    return (subcarriers * g_wlan_offload_mcs[mcs].bits *
            g_wlan_offload_mcs[mcs].numerator * streams * 10000U) /
           ((rt_uint32_t)g_wlan_offload_mcs[mcs].denominator * symbol);
}

static const rt_uint8_t *wlan_offload_find_ie(const rt_uint8_t *ies,
                                              rt_size_t length,
                                              rt_uint8_t id,
                                              rt_uint8_t *body_length)
{
    rt_size_t offset = 0;

    while (ies && offset + 2U <= length)
    {
        rt_uint8_t element_length = ies[offset + 1U];

        if (offset + 2U + element_length > length)
        {
            break;
        }
        if (ies[offset] == id)
        {
            *body_length = element_length;
            return ies + offset + 2U;
        }
        offset += 2U + element_length;
    }
    return RT_NULL;
}

static rt_uint32_t wlan_offload_legacy_rate_kbps(const rt_uint8_t *ies,
                                                 rt_size_t length)
{
    static const rt_uint8_t identifiers[2] = {1U, 50U};
    rt_uint32_t best = 0;
    rt_size_t which;

    for (which = 0; which < sizeof(identifiers); which++)
    {
        rt_uint8_t body_length = 0;
        const rt_uint8_t *body = wlan_offload_find_ie(ies, length,
                                                      identifiers[which],
                                                      &body_length);
        rt_size_t index;

        for (index = 0; body && index < body_length; index++)
        {
            /* Rates are in 500 kbit/s units and the top bit marks a basic
             * rate. Values above 54 Mbit/s are BSS membership selectors. */
            rt_uint32_t rate = (rt_uint32_t)(body[index] & 0x7fU) * 500U;

            if (rate <= 54000U && rate > best)
            {
                best = rate;
            }
        }
    }
    return best;
}

static rt_uint32_t wlan_offload_ht_rate_kbps(
    const rt_uint8_t *ies, rt_size_t length,
    enum rt_wlan_offload_channel_width width, rt_uint8_t max_streams)
{
    rt_uint8_t body_length = 0;
    const rt_uint8_t *body = wlan_offload_find_ie(ies, length, 45U,
                                                  &body_length);
    rt_uint16_t capabilities;
    rt_bool_t short_guard;
    rt_uint8_t highest = 0;
    rt_bool_t found = RT_FALSE;
    rt_size_t bit;

    /* HT Capabilities: two bytes of capability info, one byte of A-MPDU
     * parameters, then the sixteen-byte supported MCS set. */
    if (!body || body_length < 19U)
    {
        return 0;
    }
    capabilities = (rt_uint16_t)body[0] | ((rt_uint16_t)body[1] << 8);
    if (width < RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40 ||
        !(capabilities & (1U << 1)))
    {
        /* Either the BSS is 20 MHz or the peer does not support 20/40. */
        width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
        short_guard = (capabilities & (1U << 5)) != 0;
    }
    else
    {
        width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40;
        short_guard = (capabilities & (1U << 6)) != 0;
    }
    /* MCS 0 to 31 are the equal-modulation rates for one to four streams;
     * MCS 32 and above are duplicate and unequal-modulation modes.  Stop at
     * the streams this radio has, so a peer advertising more does not raise
     * the reported rate above anything the link can reach. */
    for (bit = 0; bit < 8U * (rt_size_t)max_streams && bit < 32U; bit++)
    {
        if (body[3U + bit / 8U] & (1U << (bit % 8U)))
        {
            highest = (rt_uint8_t)bit;
            found = RT_TRUE;
        }
    }
    if (!found)
    {
        return 0;
    }
    return wlan_offload_mcs_rate_kbps(width, (rt_uint8_t)(highest % 8U),
                                      (rt_uint8_t)(highest / 8U + 1U),
                                      short_guard);
}

static rt_uint32_t wlan_offload_vht_rate_kbps(
    const rt_uint8_t *ies, rt_size_t length,
    enum rt_wlan_offload_channel_width width, rt_uint8_t max_streams)
{
    rt_uint8_t body_length = 0;
    const rt_uint8_t *body = wlan_offload_find_ie(ies, length, 191U,
                                                  &body_length);
    rt_uint32_t capabilities;
    rt_uint16_t map;
    rt_bool_t short_guard;
    rt_uint8_t streams = 0;
    rt_uint8_t mcs = 0;
    rt_size_t stream;

    /* VHT Capabilities: four bytes of capability info then the eight-byte
     * supported VHT MCS and NSS set, whose first two bytes are the Rx map. */
    if (!body || body_length < 12U)
    {
        return 0;
    }
    capabilities = (rt_uint32_t)body[0] | ((rt_uint32_t)body[1] << 8) |
                   ((rt_uint32_t)body[2] << 16) | ((rt_uint32_t)body[3] << 24);
    map = (rt_uint16_t)body[4] | ((rt_uint16_t)body[5] << 8);
    if (width < RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80)
    {
        /* VHT is defined at 20 and 40 MHz too, and MCS 8 and 9 have no HT
         * equivalent, so these rates must not be left to the HT path.  Only
         * the guard interval comes from elsewhere: below 80 MHz a VHT BSS
         * uses the HT short-GI bits, not the VHT SGI-80/160 bits. */
        rt_uint8_t ht_length = 0;
        const rt_uint8_t *ht = wlan_offload_find_ie(ies, length, 45U,
                                                    &ht_length);
        rt_uint16_t ht_capabilities;

        if (!ht || ht_length < 2U)
        {
            return 0;
        }
        ht_capabilities = (rt_uint16_t)ht[0] | ((rt_uint16_t)ht[1] << 8);
        if (width < RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40 ||
            !(ht_capabilities & (1U << 1)))
        {
            width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
            short_guard = (ht_capabilities & (1U << 5)) != 0;
        }
        else
        {
            width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40;
            short_guard = (ht_capabilities & (1U << 6)) != 0;
        }
    }
    else
    {
        short_guard = width >= RT_WLAN_OFFLOAD_CHANNEL_WIDTH_160 ?
                      (capabilities & (1U << 6)) != 0 :
                      (capabilities & (1U << 5)) != 0;
    }
    /* Likewise bounded by the local stream count, not the peer's Rx map. */
    for (stream = 0; stream < 8U && stream < (rt_size_t)max_streams; stream++)
    {
        rt_uint8_t supported = (rt_uint8_t)((map >> (stream * 2U)) & 3U);

        if (supported == 3U)
        {
            continue;
        }
        streams = (rt_uint8_t)(stream + 1U);
        mcs = (rt_uint8_t)(7U + supported);
    }
    if (!streams)
    {
        return 0;
    }
    /* MCS 9 is not defined for a 20 MHz channel except at three and six
     * streams; an AP that advertises "MCS 0-9" still cannot use it there. */
    if (width == RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20 && mcs > 8U &&
        streams != 3U && streams != 6U)
    {
        mcs = 8U;
    }
    return wlan_offload_mcs_rate_kbps(width, mcs, streams, short_guard);
}

/* Operating width of the BSS, taken from the HT and VHT Operation elements.
 * The scan entry's channel width describes the channel the beacon was received
 * on, which is 20 MHz whenever the radio scanned at 20 MHz, so it understates
 * a 40 MHz or wider BSS.  The Operation elements carry what the BSS actually
 * runs, which is what the association ends up using. */
static enum rt_wlan_offload_channel_width wlan_offload_operating_width(
    const rt_uint8_t *ies, rt_size_t length,
    enum rt_wlan_offload_channel_width fallback)
{
    rt_uint8_t body_length = 0;
    const rt_uint8_t *body;
    enum rt_wlan_offload_channel_width width = fallback;

    /* HT Operation: primary channel, then the HT Operation Information field
     * whose first octet holds the secondary channel offset in bits 0-1 and the
     * STA channel width in bit 2. */
    body = wlan_offload_find_ie(ies, length, 61U, &body_length);
    if (body && body_length >= 2U)
    {
        rt_uint8_t offset = (rt_uint8_t)(body[1] & 0x03U);

        if ((body[1] & 0x04U) && (offset == 1U || offset == 3U))
        {
            width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40;
        }
        else
        {
            width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
        }
    }
    /* VHT Operation: channel width, then the two centre frequency segments.
     * Width 2 and 3 are deprecated spellings of 160 and 80+80; current APs
     * signal those with width 1 and a non-zero second segment. */
    body = wlan_offload_find_ie(ies, length, 192U, &body_length);
    if (body && body_length >= 3U)
    {
        if (body[0] == 1U)
        {
            rt_uint8_t seg0 = body[1];
            rt_uint8_t seg1 = body[2];
            rt_uint8_t delta = seg1 > seg0 ?
                               (rt_uint8_t)(seg1 - seg0) :
                               (rt_uint8_t)(seg0 - seg1);

            if (!seg1)
            {
                width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80;
            }
            else if (delta == 8U)
            {
                width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_160;
            }
            else if (delta > 8U)
            {
                width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80;
            }
            else
            {
                width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80;
            }
        }
        else if (body[0] == 2U)
        {
            width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_160;
        }
        else if (body[0] == 3U)
        {
            width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80;
        }
    }
    return width;
}

/* Spatial streams this radio can actually use on the network's band.  The IEs
 * describe what the peer offers, which is routinely more than the local radio
 * has: a 1x1 part associated with a two-stream AP would otherwise report the
 * AP's two-stream rate as if it were the link rate. */
static rt_uint8_t wlan_offload_local_streams(
    const struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_network *network)
{
    const struct rt_wlan_offload_supported_band *band;

    if (!radio || (int)network->channel.band < 0 ||
        network->channel.band >= RT_WLAN_OFFLOAD_BAND_MAX)
    {
        return 4U;
    }
    band = radio->bands[network->channel.band];
    if (!band || !band->max_spatial_streams)
    {
        /* Radio did not declare a limit; keep the peer's advertisement. */
        return 4U;
    }
    return band->max_spatial_streams > 4U ? 4U : band->max_spatial_streams;
}

static rt_uint32_t wlan_offload_network_datarate(
    const struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_network *network)
{
    const struct rt_wlan_offload_supported_band *band;
    enum rt_wlan_offload_channel_width width;
    rt_uint32_t best;
    rt_uint32_t candidate;
    rt_uint8_t max_streams;

    if (!network->ies || !network->ies_length)
    {
        return 0;
    }
    max_streams = wlan_offload_local_streams(radio, network);
    width = wlan_offload_operating_width(network->ies, network->ies_length,
                                         network->channel.width);
    /* Never report a width this radio cannot use: a 40 MHz part associated
     * with an 80 MHz AP runs at 40. */
    band = radio && (int)network->channel.band >= 0 &&
           network->channel.band < RT_WLAN_OFFLOAD_BAND_MAX ?
           radio->bands[network->channel.band] : RT_NULL;
    if (band && band->max_channel_width_set && width > band->max_channel_width)
    {
        width = band->max_channel_width;
    }
    best = wlan_offload_legacy_rate_kbps(network->ies, network->ies_length);
    candidate = wlan_offload_ht_rate_kbps(network->ies, network->ies_length,
                                          width, max_streams);
    if (candidate > best)
    {
        best = candidate;
    }
    candidate = wlan_offload_vht_rate_kbps(network->ies, network->ies_length,
                                           width, max_streams);
    if (candidate > best)
    {
        best = candidate;
    }
    /* rt_wlan_info.datarate is expressed in bits per second. */
    return best * 1000U;
}

static rt_err_t wlan_offload_network_to_wlan_info(
    struct rt_wlan_offload_radio *radio, const struct rt_wlan_offload_network *network,
    struct rt_wlan_info *info)
{
    if (!network || !info || network->ssid.len > RT_WLAN_SSID_MAX_LENGTH ||
        wlan_offload_validate_channel(radio, &network->channel, RT_FALSE) != RT_EOK ||
        (network->ies_length && !network->ies))
    {
        return -RT_EINVAL;
    }

    INVALID_INFO(info);
    info->security = network->security;
    info->band = wlan_offload_to_wlan_band(network->channel.band);
    info->channel = network->channel.primary_channel;
    info->rssi = network->rssi;
    info->ssid = network->ssid;
    info->datarate = wlan_offload_network_datarate(radio, network);
    rt_memcpy(info->bssid, network->bssid, sizeof(info->bssid));
    return RT_EOK;
}

static void wlan_offload_indicate_wlan(struct rt_wlan_offload_vif *vif,
                                  rt_wlan_dev_event_t event,
                                  const void *data, rt_size_t length)
{
    struct rt_wlan_buff buffer;

    if (data && length)
    {
        buffer.data = (void *)data;
        buffer.len = (rt_int32_t)length;
        rt_wlan_dev_indicate_event_handle(&vif->wlan, event, &buffer);
    }
    else
    {
        rt_wlan_dev_indicate_event_handle(&vif->wlan, event, RT_NULL);
    }
}

static rt_err_t wlan_offload_change_interface_submit(struct rt_wlan_offload_vif *vif,
                                                 rt_bool_t enabled)
{
    struct rt_wlan_offload_radio *radio = vif->radio;
    struct rt_wlan_offload_vif *other;
    rt_err_t result;

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (vif->enabled == enabled)
    {
        rt_mutex_release(&radio->operation_lock);
        return RT_EOK;
    }
    if (enabled && !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_STA_AP_CONCURRENT))
    {
        other = &radio->vifs[vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
                             WLAN_OFFLOAD_VIF_AP_INDEX : WLAN_OFFLOAD_VIF_STA_INDEX];
        if (other->registered && other->enabled)
        {
            rt_mutex_release(&radio->operation_lock);
            return -RT_EBUSY;
        }
    }
    rt_mutex_release(&radio->operation_lock);
    if (!radio->ops->change_interface)
    {
        return -RT_ENOSYS;
    }

    result = radio->ops->change_interface(vif, vif->iftype, enabled);
    if (result == RT_EOK)
    {
        rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
        vif->enabled = enabled;
        if (!enabled)
        {
            vif->link_up = RT_FALSE;
            vif->promiscuous = RT_FALSE;
            vif->management_filter = RT_FALSE;
            vif->pending_scan_id = 0;
            vif->pending_connect_id = 0;
            vif->pending_ap_id = 0;
        }
        rt_mutex_release(&radio->operation_lock);
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
        if (!enabled && vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION)
        {
            rt_wlan_offload_supplicant_cancel(radio);
        }
#endif
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
        if (!enabled && vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP)
        {
            rt_wlan_offload_hostapd_cancel(radio);
        }
#endif
    }
    return result;
}

static rt_err_t wlan_offload_scan_submit(
    struct rt_wlan_offload_vif *vif, const struct rt_wlan_offload_scan_request *request)
{
    struct rt_wlan_offload_radio *radio = vif->radio;
    rt_size_t index;
    rt_err_t result;

    if (vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION || !request ||
        !request->request_id ||
        (request->ssid_count && !request->ssids) ||
        request->ssid_count > radio->max_scan_ssids ||
        (request->channel_count && !request->channels) ||
        (request->ies_length && !request->ies) ||
        request->ies_length > radio->max_scan_ie_length)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (!vif->enabled || vif->pending_scan_id)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_EBUSY;
    }
    if (!radio->ops->scan)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_ENOSYS;
    }
    for (index = 0; index < request->ssid_count; index++)
    {
        if (request->ssids[index].length > RT_WLAN_SSID_MAX_LENGTH)
        {
            rt_mutex_release(&radio->operation_lock);
            return -RT_EINVAL;
        }
    }
    for (index = 0; index < request->channel_count; index++)
    {
        if (wlan_offload_validate_channel(radio, &request->channels[index],
                                     RT_FALSE) != RT_EOK)
        {
            rt_mutex_release(&radio->operation_lock);
            return -RT_EINVAL;
        }
    }

#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
    /* A new scan starts a new association-information snapshot. */
    wlan_offload_bss_cache_clear_locked(radio);
#endif
    vif->pending_scan_id = request->request_id;
    rt_mutex_release(&radio->operation_lock);
    result = radio->ops->scan(vif, request);
    if (result != RT_EOK)
    {
        rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
        if (vif->pending_scan_id == request->request_id)
        {
            vif->pending_scan_id = 0;
        }
        rt_mutex_release(&radio->operation_lock);
    }
    return result;
}

static rt_err_t wlan_offload_abort_scan_submit(struct rt_wlan_offload_vif *vif,
                                           rt_uint32_t request_id)
{
    struct rt_wlan_offload_radio *radio = vif->radio;

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (!request_id || vif->pending_scan_id != request_id)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_EINVAL;
    }
    rt_mutex_release(&radio->operation_lock);
    if (!radio->ops->abort_scan)
    {
        return -RT_ENOSYS;
    }
    return radio->ops->abort_scan(vif, request_id);
}

static rt_err_t wlan_offload_connect_submit(
    struct rt_wlan_offload_vif *vif, const struct rt_wlan_offload_connect_request *request)
{
    struct rt_wlan_offload_radio *radio = vif->radio;
    rt_err_t result;

    if (vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION || !request ||
        !request->request_id || request->ssid.len > RT_WLAN_SSID_MAX_LENGTH ||
        request->key.len > RT_WLAN_PASSWORD_MAX_LENGTH ||
        wlan_offload_validate_channel(radio, &request->channel, RT_TRUE) != RT_EOK ||
        (request->ies_length && !request->ies))
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (!vif->enabled || vif->pending_connect_id)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_EBUSY;
    }
    if (!radio->ops->connect)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_ENOSYS;
    }

    vif->pending_connect_id = request->request_id;
    rt_mutex_release(&radio->operation_lock);
    result = radio->ops->connect(vif, request);
    if (result != RT_EOK)
    {
        rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
        if (vif->pending_connect_id == request->request_id)
        {
            vif->pending_connect_id = 0;
        }
        rt_mutex_release(&radio->operation_lock);
    }
    return result;
}

static rt_err_t wlan_offload_disconnect_submit(struct rt_wlan_offload_vif *vif,
                                           rt_uint32_t request_id,
                                           rt_uint16_t reason)
{
    rt_bool_t enabled;

    if (vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION || !request_id)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&vif->radio->operation_lock, RT_WAITING_FOREVER);
    enabled = vif->enabled;
    rt_mutex_release(&vif->radio->operation_lock);
    if (!enabled)
    {
        return -RT_EBUSY;
    }
    if (!vif->radio->ops->disconnect)
    {
        return -RT_ENOSYS;
    }
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
    rt_wlan_offload_supplicant_cancel(vif->radio);
#endif
    return vif->radio->ops->disconnect(vif, request_id, reason);
}

static rt_err_t wlan_offload_start_ap_submit(
    struct rt_wlan_offload_vif *vif, const struct rt_wlan_offload_ap_settings *settings)
{
    struct rt_wlan_offload_radio *radio = vif->radio;
    rt_err_t result;

    if (vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_AP || !settings ||
        !settings->request_id || settings->ssid.len > RT_WLAN_SSID_MAX_LENGTH ||
        settings->key.len > RT_WLAN_PASSWORD_MAX_LENGTH ||
        wlan_offload_validate_channel(radio, &settings->channel, RT_FALSE) != RT_EOK ||
        (settings->max_stations && radio->firmware_info.max_stations &&
         settings->max_stations > radio->firmware_info.max_stations) ||
        (settings->beacon_ies_length && !settings->beacon_ies))
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (!vif->enabled || vif->pending_ap_id)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_EBUSY;
    }
    if (!radio->ops->start_ap)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_ENOSYS;
    }

    vif->pending_ap_id = settings->request_id;
    rt_mutex_release(&radio->operation_lock);
    result = radio->ops->start_ap(vif, settings);
    if (result != RT_EOK)
    {
        rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
        if (vif->pending_ap_id == settings->request_id)
        {
            vif->pending_ap_id = 0;
        }
        rt_mutex_release(&radio->operation_lock);
    }
    return result;
}

static rt_err_t wlan_offload_stop_ap_submit(struct rt_wlan_offload_vif *vif,
                                        rt_uint32_t request_id)
{
    struct rt_wlan_offload_radio *radio = vif->radio;
    rt_err_t result;

    if (vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_AP || !request_id)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (!vif->enabled || vif->pending_ap_id)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_EBUSY;
    }
    if (!radio->ops->stop_ap)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_ENOSYS;
    }

    vif->pending_ap_id = request_id;
    rt_mutex_release(&radio->operation_lock);
    result = radio->ops->stop_ap(vif, request_id);
    if (result != RT_EOK)
    {
        rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
        if (vif->pending_ap_id == request_id)
        {
            vif->pending_ap_id = 0;
        }
        rt_mutex_release(&radio->operation_lock);
    }
    return result;
}

static rt_err_t wlan_offload_wlan_init(struct rt_wlan_device *wlan)
{
    struct rt_wlan_offload_radio *radio = wlan_offload_vif_from_wlan(wlan)->radio;
    rt_err_t result;
    rt_bool_t retry = RT_FALSE;
    int index;

    result = rt_mutex_take(&radio->command_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_mutex_release(&radio->command_lock);
        return result;
    }
    if (radio->state == RT_WLAN_OFFLOAD_STARTED || radio->state == RT_WLAN_OFFLOAD_OFFLINE)
    {
        result = RT_EOK;
        goto exit;
    }
    if (radio->state == RT_WLAN_OFFLOAD_FAILED)
    {
        retry = RT_TRUE;
    }
    else if (radio->state != RT_WLAN_OFFLOAD_REGISTERED)
    {
        result = -RT_EBUSY;
        goto exit;
    }

    radio->state = RT_WLAN_OFFLOAD_STARTING;
    /* A failed USB/firmware generation may leave the framework VIF marked
     * enabled even though the driver's stop path removed it.  Start each
     * new generation with a clean interface state so the next mode request
     * actually calls change_interface(). */
    for (index = 0; index < RT_WLAN_OFFLOAD_WLAN_VIF_COUNT; index++)
    {
        struct rt_wlan_offload_vif *vif = &radio->vifs[index];

        vif->enabled = RT_FALSE;
        vif->link_up = RT_FALSE;
        vif->promiscuous = RT_FALSE;
        vif->management_filter = RT_FALSE;
        vif->pending_scan_id = 0;
        vif->pending_connect_id = 0;
        vif->pending_ap_id = 0;
    }
    rt_mutex_release(&radio->operation_lock);

    if (retry)
    {
        if (radio->ops->stop)
        {
            radio->ops->stop(radio);
        }
        if (radio->bus)
        {
            rt_wlan_offload_bus_stop(radio->bus);
        }
    }
    if (radio->bus)
    {
        result = rt_wlan_offload_bus_start(radio->bus);
        if (result != RT_EOK)
        {
            rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
            radio->state = RT_WLAN_OFFLOAD_FAILED;
            goto exit;
        }
    }
    result = radio->ops->start ? radio->ops->start(radio) : RT_EOK;
    if (result != RT_EOK)
    {
        if (radio->bus)
        {
            rt_wlan_offload_bus_stop(radio->bus);
        }
        rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
        radio->state = RT_WLAN_OFFLOAD_FAILED;
        goto exit;
    }

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (radio->state == RT_WLAN_OFFLOAD_STARTING)
    {
        radio->state = RT_WLAN_OFFLOAD_STARTED;
        radio->firmware_generation++;
        if (!radio->firmware_generation)
        {
            radio->firmware_generation++;
        }
    }
    else
    {
        result = -RT_EIO;
    }

exit:
    rt_mutex_release(&radio->operation_lock);
    rt_mutex_release(&radio->command_lock);
    return result;
}

static rt_err_t wlan_offload_wlan_mode(struct rt_wlan_device *wlan,
                                  rt_wlan_mode_t mode)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_bool_t enabled;
    rt_err_t result;

    if ((vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION &&
         mode != RT_WLAN_STATION && mode != RT_WLAN_NONE) ||
        (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP &&
         mode != RT_WLAN_AP && mode != RT_WLAN_NONE))
    {
        return -RT_EINVAL;
    }
    enabled = mode != RT_WLAN_NONE;
    result = wlan_offload_operation_enter(vif->radio);
    if (result == RT_EOK)
    {
        result = wlan_offload_change_interface_submit(vif, enabled);
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static void wlan_offload_recovery_work(struct rt_work *work, void *work_data)
{
    struct rt_wlan_offload_radio *radio = work_data;
    rt_bool_t restore_station = RT_FALSE;
    rt_bool_t restore_ap = RT_FALSE;
    struct rt_wlan_offload_vif *init_vif = RT_NULL;
    rt_err_t result = -RT_EINVAL;
    int index;

    (void)work;
    if (!radio || !radio->recovery_work_initialized)
    {
        return;
    }

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (radio->state != RT_WLAN_OFFLOAD_FAILED)
    {
        radio->recovery_queued = RT_FALSE;
        rt_mutex_release(&radio->operation_lock);
        return;
    }
    for (index = 0; index < RT_WLAN_OFFLOAD_WLAN_VIF_COUNT; index++)
    {
        struct rt_wlan_offload_vif *vif = &radio->vifs[index];

        if (!vif->registered)
        {
            continue;
        }
        if (!init_vif)
        {
            init_vif = vif;
        }
        if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION &&
            vif->wlan.mode == RT_WLAN_STATION)
        {
            restore_station = RT_TRUE;
        }
        else if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP &&
                 vif->wlan.mode == RT_WLAN_AP)
        {
            restore_ap = RT_TRUE;
        }
    }
    rt_mutex_release(&radio->operation_lock);

    if (init_vif)
    {
        result = wlan_offload_wlan_init(&init_vif->wlan);
    }
    if (result == RT_EOK && restore_station)
    {
        result = wlan_offload_wlan_mode(
            &radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].wlan, RT_WLAN_STATION);
    }
    if (result == RT_EOK && restore_ap)
    {
        result = wlan_offload_wlan_mode(
            &radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].wlan, RT_WLAN_AP);
    }

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    radio->recovery_queued = RT_FALSE;
    rt_mutex_release(&radio->operation_lock);
    if (result == RT_EOK)
    {
        LOG_I("WLAN offload firmware recovery completed");
    }
    else
    {
        LOG_E("WLAN offload firmware recovery failed: %d", result);
    }
}

static rt_err_t wlan_offload_wlan_scan(struct rt_wlan_device *wlan,
                                  struct rt_scan_info *scan_info)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    struct rt_wlan_offload_scan_request request;
    struct rt_wlan_offload_scan_ssid ssid;
    struct rt_wlan_offload_channel_definition channel;
    struct rt_wlan_offload_channel_definition *band_channels = RT_NULL;
    rt_err_t result;

    rt_memset(&request, 0, sizeof(request));
    rt_memset(&ssid, 0, sizeof(ssid));
    rt_memset(&channel, 0, sizeof(channel));
    result = wlan_offload_operation_enter(vif->radio);
    if (result != RT_EOK)
    {
        return result;
    }

    request.request_id = wlan_offload_alloc_request_id_internal(vif->radio);
    if (scan_info)
    {
        if (scan_info->ssid.len > RT_WLAN_SSID_MAX_LENGTH)
        {
            result = -RT_EINVAL;
            goto exit;
        }
        if (scan_info->ssid.len)
        {
            ssid.length = scan_info->ssid.len;
            rt_memcpy(ssid.value, scan_info->ssid.val, ssid.length);
            request.ssids = &ssid;
            request.ssid_count = 1;
        }
        rt_memcpy(request.bssid, scan_info->bssid, sizeof(request.bssid));
        if (scan_info->band_locked &&
            scan_info->band != RT_802_11_BAND_2_4GHZ &&
            scan_info->band != RT_802_11_BAND_5GHZ)
        {
            result = -RT_EINVAL;
            goto exit;
        }
        if (scan_info->channel_min > 0 &&
            scan_info->channel_min == scan_info->channel_max)
        {
            enum rt_wlan_offload_band_id band = RT_WLAN_OFFLOAD_BAND_MAX;

            if (scan_info->band_locked &&
                scan_info->band == RT_802_11_BAND_2_4GHZ)
            {
                band = RT_WLAN_OFFLOAD_BAND_2GHZ;
            }
            else if (scan_info->band_locked &&
                     scan_info->band == RT_802_11_BAND_5GHZ)
            {
                band = RT_WLAN_OFFLOAD_BAND_5GHZ;
            }
            result = wlan_offload_resolve_channel_for_band(
                vif->radio, band, scan_info->channel_min, RT_FALSE, RT_TRUE,
                &channel);
            if (result != RT_EOK)
            {
                goto exit;
            }
            request.channels = &channel;
            request.channel_count = 1;
        }
        else if (scan_info->band_locked)
        {
            enum rt_wlan_offload_band_id band_id =
                scan_info->band == RT_802_11_BAND_2_4GHZ ?
                    RT_WLAN_OFFLOAD_BAND_2GHZ : RT_WLAN_OFFLOAD_BAND_5GHZ;
            const struct rt_wlan_offload_supported_band *band =
                vif->radio->bands[band_id];
            rt_size_t index;

            if (!band || !band->channel_count)
            {
                result = -RT_EINVAL;
                goto exit;
            }
            band_channels = rt_calloc(
                band->channel_count, sizeof(*band_channels));
            if (!band_channels)
            {
                result = -RT_ENOMEM;
                goto exit;
            }
            for (index = 0; index < band->channel_count; index++)
            {
                const struct rt_wlan_offload_channel *configured =
                    &band->channels[index];
                struct rt_wlan_offload_channel_definition *definition;

                if (configured->flags & RT_WLAN_OFFLOAD_CHANNEL_DISABLED)
                {
                    continue;
                }
                definition = &band_channels[request.channel_count++];
                definition->band = band_id;
                definition->width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
                definition->primary_channel = configured->number;
                definition->primary_frequency_mhz =
                    configured->center_frequency_mhz;
                definition->center_frequency1_mhz =
                    configured->center_frequency_mhz;
            }
            if (!request.channel_count)
            {
                result = -RT_EINVAL;
                goto exit;
            }
            request.channels = band_channels;
        }
        if (scan_info->passive)
        {
            request.flags |= RT_WLAN_OFFLOAD_SCAN_PASSIVE;
        }
    }
    result = wlan_offload_scan_submit(vif, &request);

exit:
    rt_free(band_channels);
    wlan_offload_operation_exit(vif->radio);
    return result;
}

static rt_err_t wlan_offload_wlan_stop_scan(struct rt_wlan_device *wlan)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_uint32_t request_id;
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        rt_mutex_take(&vif->radio->operation_lock, RT_WAITING_FOREVER);
        request_id = vif->pending_scan_id;
        rt_mutex_release(&vif->radio->operation_lock);
        result = wlan_offload_abort_scan_submit(vif, request_id);
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static rt_err_t wlan_offload_wlan_connect(struct rt_wlan_device *wlan,
                                     struct rt_sta_info *sta_info)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    struct rt_wlan_offload_connect_request request;
    enum rt_wlan_offload_band_id band;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
    rt_uint8_t bss_ies[RT_WLAN_OFFLOAD_BSS_SECURITY_IE_MAX_LENGTH];
    rt_size_t bss_ies_length = 0;
    rt_bool_t use_embedded_supplicant;
#endif
    rt_err_t result;

    if (!sta_info)
    {
        return -RT_EINVAL;
    }
    rt_memset(&request, 0, sizeof(request));
    request.ssid = sta_info->ssid;
    request.key = sta_info->key;
    rt_memcpy(request.bssid, sta_info->bssid, sizeof(request.bssid));
    if (sta_info->band == RT_802_11_BAND_2_4GHZ)
    {
        band = RT_WLAN_OFFLOAD_BAND_2GHZ;
    }
    else if (sta_info->band == RT_802_11_BAND_5GHZ)
    {
        band = RT_WLAN_OFFLOAD_BAND_5GHZ;
    }
    else if (sta_info->band == RT_802_11_BAND_UNKNOWN)
    {
        band = RT_WLAN_OFFLOAD_BAND_MAX;
    }
    else
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_resolve_channel_for_band(
        vif->radio, band, sta_info->channel, RT_TRUE, RT_TRUE,
        &request.channel);
    if (result != RT_EOK)
    {
        return result;
    }
    request.security = sta_info->security;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
    use_embedded_supplicant =
        (vif->radio->capabilities &
         RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT) &&
        !(vif->radio->capabilities & RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD) &&
        rt_wlan_offload_supplicant_supports(request.security);
#endif

    result = wlan_offload_operation_enter(vif->radio);
    if (result == RT_EOK)
    {
        request.request_id = wlan_offload_alloc_request_id_internal(vif->radio);
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
        if (use_embedded_supplicant)
        {
            rt_mutex_take(&vif->radio->operation_lock, RT_WAITING_FOREVER);
            (void)wlan_offload_bss_cache_copy_ies_locked(
                vif->radio, &request.ssid, request.bssid,
                bss_ies, &bss_ies_length);
            rt_mutex_release(&vif->radio->operation_lock);
            result = rt_wlan_offload_supplicant_prepare(
                vif->radio, &request, bss_ies, bss_ies_length);
        }
#endif
        if (result == RT_EOK)
        {
            result = wlan_offload_connect_submit(vif, &request);
        }
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
        if (result != RT_EOK && use_embedded_supplicant)
        {
            rt_wlan_offload_supplicant_cancel(vif->radio);
        }
#endif
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static rt_err_t wlan_offload_wlan_disconnect(struct rt_wlan_device *wlan)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        result = wlan_offload_disconnect_submit(
            vif, wlan_offload_alloc_request_id_internal(vif->radio), 0);
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static rt_err_t wlan_offload_wlan_start_ap(struct rt_wlan_device *wlan,
                                      struct rt_ap_info *ap_info)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    struct rt_wlan_offload_ap_settings settings;
    enum rt_wlan_offload_band_id band;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    rt_bool_t use_embedded_hostapd;
#endif
    rt_err_t result;

    if (!ap_info)
    {
        return -RT_EINVAL;
    }
    rt_memset(&settings, 0, sizeof(settings));
    settings.ssid = ap_info->ssid;
    settings.key = ap_info->key;
    if (ap_info->band == RT_802_11_BAND_2_4GHZ)
    {
        band = RT_WLAN_OFFLOAD_BAND_2GHZ;
    }
    else if (ap_info->band == RT_802_11_BAND_5GHZ)
    {
        band = RT_WLAN_OFFLOAD_BAND_5GHZ;
    }
    else if (ap_info->band == RT_802_11_BAND_UNKNOWN)
    {
        band = RT_WLAN_OFFLOAD_BAND_MAX;
    }
    else
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_resolve_channel_for_band(
        vif->radio, band, ap_info->channel, RT_FALSE, RT_FALSE,
        &settings.channel);
    if (result != RT_EOK)
    {
        return result;
    }
    wlan_offload_select_compat_ap_width(vif->radio, &settings.channel);
    settings.security = ap_info->security;
    settings.hidden = ap_info->hidden;
    settings.request_id = wlan_offload_alloc_request_id_internal(vif->radio);
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    use_embedded_hostapd =
        (vif->radio->capabilities &
         RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR) &&
        !(vif->radio->capabilities & RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD);
    if (use_embedded_hostapd)
    {
        result = rt_wlan_offload_hostapd_prepare(vif->radio, &settings);
        if (result != RT_EOK)
        {
            return result;
        }
    }
#endif

    result = wlan_offload_operation_enter(vif->radio);
    if (result == RT_EOK)
    {
        result = wlan_offload_start_ap_submit(vif, &settings);
        wlan_offload_operation_exit(vif->radio);
    }
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    if (result != RT_EOK && use_embedded_hostapd)
    {
        rt_wlan_offload_hostapd_cancel(vif->radio);
    }
#endif
    return result;
}

static rt_err_t wlan_offload_wlan_stop_ap(struct rt_wlan_device *wlan)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        result = wlan_offload_stop_ap_submit(
            vif, wlan_offload_alloc_request_id_internal(vif->radio));
        wlan_offload_operation_exit(vif->radio);
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
        if (result == RT_EOK)
        {
            rt_wlan_offload_hostapd_cancel(vif->radio);
        }
#endif
    }
    return result;
}

static rt_err_t wlan_offload_wlan_deauth_station(struct rt_wlan_device *wlan,
                                             rt_uint8_t mac[6])
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result;

    if (!mac)
    {
        return -RT_EINVAL;
    }
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    if (vif->radio->capabilities &
        RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR)
    {
        return rt_wlan_offload_hostapd_deauth(vif->radio, mac, 0);
    }
#endif
    result = wlan_offload_operation_enter(vif->radio);
    if (result == RT_EOK)
    {
        result = vif->radio->ops->del_station ?
                 vif->radio->ops->del_station(
                     vif, wlan_offload_alloc_request_id_internal(vif->radio),
                     mac, 0) : -RT_ENOSYS;
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static int wlan_offload_wlan_get_rssi(struct rt_wlan_device *wlan)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    int rssi = 0;
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        result = vif->radio->ops->get_rssi ?
                 vif->radio->ops->get_rssi(vif, &rssi) : -RT_ENOSYS;
        wlan_offload_operation_exit(vif->radio);
    }
    if (result != RT_EOK)
    {
        rt_set_errno(result);
    }
    return rssi;
}

static rt_err_t wlan_offload_wlan_set_power_save(struct rt_wlan_device *wlan,
                                             int level)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        result = vif->radio->ops->set_power_save ?
                 vif->radio->ops->set_power_save(vif, level) : -RT_ENOSYS;
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static int wlan_offload_wlan_get_power_save(struct rt_wlan_device *wlan)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    int level = -1;
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        result = vif->radio->ops->get_power_save ?
                 vif->radio->ops->get_power_save(vif, &level) : -RT_ENOSYS;
        wlan_offload_operation_exit(vif->radio);
    }
    if (result != RT_EOK)
    {
        rt_set_errno(result);
    }
    return level;
}

static rt_err_t wlan_offload_wlan_set_promiscuous(struct rt_wlan_device *wlan,
                                              rt_bool_t enabled)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        result = vif->radio->ops->set_promiscuous ?
                 vif->radio->ops->set_promiscuous(vif, enabled) : -RT_ENOSYS;
        if (result == RT_EOK)
        {
            vif->promiscuous = enabled;
        }
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static rt_err_t wlan_offload_wlan_set_filter(struct rt_wlan_device *wlan,
                                        struct rt_wlan_filter *filter)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result;

    if (!filter)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(vif->radio);
    if (result == RT_EOK)
    {
        result = vif->radio->ops->set_filter ?
                 vif->radio->ops->set_filter(vif, filter) : -RT_ENOSYS;
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static rt_err_t wlan_offload_wlan_set_mgmt_filter(struct rt_wlan_device *wlan,
                                              rt_bool_t enabled)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        result = vif->radio->ops->set_mgmt_filter ?
                 vif->radio->ops->set_mgmt_filter(vif, enabled) : -RT_ENOSYS;
        if (result == RT_EOK)
        {
            vif->management_filter = enabled;
        }
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static rt_err_t wlan_offload_wlan_set_channel(struct rt_wlan_device *wlan,
                                          int channel)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    struct rt_wlan_offload_channel_definition definition;
    rt_err_t result;

    result = wlan_offload_resolve_channel(vif->radio, channel, RT_FALSE,
                                     &definition);
    if (result != RT_EOK)
    {
        return result;
    }
    result = wlan_offload_operation_enter(vif->radio);

    if (result == RT_EOK)
    {
        result = vif->radio->ops->set_channel ?
                 vif->radio->ops->set_channel(vif, &definition) : -RT_ENOSYS;
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static int wlan_offload_wlan_get_channel(struct rt_wlan_device *wlan)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    struct rt_wlan_offload_channel_definition definition;
    int channel = -1;
    rt_err_t result = wlan_offload_operation_enter(vif->radio);

    rt_memset(&definition, 0, sizeof(definition));
    if (result == RT_EOK)
    {
        result = vif->radio->ops->get_channel ?
                 vif->radio->ops->get_channel(vif, &definition) : -RT_ENOSYS;
        if (result == RT_EOK &&
            wlan_offload_validate_channel(vif->radio, &definition,
                                     RT_FALSE) == RT_EOK)
        {
            channel = definition.primary_channel;
        }
        else if (result == RT_EOK)
        {
            result = -RT_EINVAL;
        }
        wlan_offload_operation_exit(vif->radio);
    }
    if (result != RT_EOK)
    {
        rt_set_errno(result);
    }
    return channel;
}

static rt_err_t wlan_offload_wlan_set_country(struct rt_wlan_device *wlan,
                                          rt_country_code_t country)
{
    struct rt_wlan_offload_radio *radio = wlan_offload_vif_from_wlan(wlan)->radio;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result == RT_EOK)
    {
        result = radio->ops->set_regulatory ?
                 radio->ops->set_regulatory(radio, country) : -RT_ENOSYS;
        wlan_offload_operation_exit(radio);
    }
    return result;
}

static rt_country_code_t wlan_offload_wlan_get_country(struct rt_wlan_device *wlan)
{
    struct rt_wlan_offload_radio *radio = wlan_offload_vif_from_wlan(wlan)->radio;
    rt_country_code_t country = RT_COUNTRY_UNKNOWN;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result == RT_EOK)
    {
        result = radio->ops->get_regulatory ?
                 radio->ops->get_regulatory(radio, &country) : -RT_ENOSYS;
        wlan_offload_operation_exit(radio);
    }
    if (result != RT_EOK)
    {
        rt_set_errno(result);
    }
    return country;
}

static rt_err_t wlan_offload_wlan_set_mac(struct rt_wlan_device *wlan,
                                     rt_uint8_t mac[6])
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result;

    if (!mac)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(vif->radio);
    if (result == RT_EOK)
    {
        result = vif->radio->ops->set_mac ?
                 vif->radio->ops->set_mac(vif, mac) : -RT_ENOSYS;
        if (result == RT_EOK)
        {
            rt_memcpy(vif->address, mac, sizeof(vif->address));
        }
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static rt_err_t wlan_offload_wlan_get_mac(struct rt_wlan_device *wlan,
                                     rt_uint8_t mac[6])
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result;

    if (!mac)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(vif->radio);
    if (result == RT_EOK)
    {
        if (vif->radio->ops->get_mac)
        {
            result = vif->radio->ops->get_mac(vif, mac);
            if (result == RT_EOK)
            {
                rt_memcpy(vif->address, mac, sizeof(vif->address));
            }
        }
        else
        {
            rt_memcpy(mac, vif->address, sizeof(vif->address));
            result = RT_EOK;
        }
        wlan_offload_operation_exit(vif->radio);
    }
    return result;
}

static int wlan_offload_wlan_transmit(struct rt_wlan_device *wlan,
                                 void *data, int length)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result;

    if (!data || length <= 0 ||
        (rt_size_t)length > vif->radio->max_frame_size)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_data_enter(vif);
    if (result == RT_EOK)
    {
        result = vif->radio->ops->transmit ?
                 vif->radio->ops->transmit(vif, data, length) : -RT_ENOSYS;
        wlan_offload_data_exit(vif->radio);
    }
    return result;
}

static int wlan_offload_wlan_transmit_raw(struct rt_wlan_device *wlan,
                                     void *data, int length)
{
    struct rt_wlan_offload_vif *vif = wlan_offload_vif_from_wlan(wlan);
    rt_err_t result;

    if (!data || length <= 0 ||
        (rt_size_t)length > vif->radio->max_frame_size)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_data_enter(vif);
    if (result == RT_EOK)
    {
        result = vif->radio->ops->transmit_raw ?
                 vif->radio->ops->transmit_raw(vif, data, length) : -RT_ENOSYS;
        wlan_offload_data_exit(vif->radio);
    }
    return result;
}

static const struct rt_wlan_dev_ops wlan_offload_wlan_ops =
{
    .wlan_init = wlan_offload_wlan_init,
    .wlan_mode = wlan_offload_wlan_mode,
    .wlan_scan = wlan_offload_wlan_scan,
    .wlan_join = wlan_offload_wlan_connect,
    .wlan_softap = wlan_offload_wlan_start_ap,
    .wlan_disconnect = wlan_offload_wlan_disconnect,
    .wlan_ap_stop = wlan_offload_wlan_stop_ap,
    .wlan_ap_deauth = wlan_offload_wlan_deauth_station,
    .wlan_scan_stop = wlan_offload_wlan_stop_scan,
    .wlan_get_rssi = wlan_offload_wlan_get_rssi,
    .wlan_set_powersave = wlan_offload_wlan_set_power_save,
    .wlan_get_powersave = wlan_offload_wlan_get_power_save,
    .wlan_cfg_promisc = wlan_offload_wlan_set_promiscuous,
    .wlan_cfg_filter = wlan_offload_wlan_set_filter,
    .wlan_cfg_mgnt_filter = wlan_offload_wlan_set_mgmt_filter,
    .wlan_set_channel = wlan_offload_wlan_set_channel,
    .wlan_get_channel = wlan_offload_wlan_get_channel,
    .wlan_set_country = wlan_offload_wlan_set_country,
    .wlan_get_country = wlan_offload_wlan_get_country,
    .wlan_set_mac = wlan_offload_wlan_set_mac,
    .wlan_get_mac = wlan_offload_wlan_get_mac,
    .wlan_recv = RT_NULL,
    .wlan_send = wlan_offload_wlan_transmit,
    .wlan_send_raw_frame = wlan_offload_wlan_transmit_raw,
};

static rt_err_t wlan_offload_validate_metadata(
    const struct rt_wlan_offload_radio_config *config)
{
    rt_bool_t has_band = RT_FALSE;
    rt_uint8_t required_vifs = 1;
    rt_size_t index;

    if ((config->cipher_suite_count && !config->cipher_suites) ||
        (config->iface_combination_count && !config->iface_combinations) ||
        (config->regulatory_domain && config->regulatory_domain->rule_count &&
         !config->regulatory_domain->rules))
    {
        return -RT_EINVAL;
    }

    for (index = 0; index < config->cipher_suite_count; index++)
    {
        if ((int)config->cipher_suites[index] < RT_WLAN_OFFLOAD_CIPHER_NONE ||
            config->cipher_suites[index] > RT_WLAN_OFFLOAD_CIPHER_AES_CMAC)
        {
            return -RT_EINVAL;
        }
    }

    for (index = 0; index < RT_WLAN_OFFLOAD_BAND_MAX; index++)
    {
        const struct rt_wlan_offload_supported_band *band = config->bands[index];

        if (!band)
        {
            continue;
        }
        has_band = RT_TRUE;
        if (band->id != (enum rt_wlan_offload_band_id)index ||
            !band->channel_count || !band->channels ||
            (band->rate_count && !band->rates))
        {
            return -RT_EINVAL;
        }
        {
            rt_size_t channel_index;

            for (channel_index = 0;
                 channel_index < band->channel_count; channel_index++)
            {
                if (band->channels[channel_index].band != band->id ||
                    !band->channels[channel_index].number ||
                    !band->channels[channel_index].center_frequency_mhz)
                {
                    return -RT_EINVAL;
                }
            }
        }
    }
    if (!has_band)
    {
        return -RT_EINVAL;
    }

    if ((config->capabilities & RT_WLAN_OFFLOAD_CAP_STA_AP_CONCURRENT) &&
        (config->capabilities & RT_WLAN_OFFLOAD_CAP_STA) &&
        (config->capabilities & RT_WLAN_OFFLOAD_CAP_AP))
    {
        required_vifs = 2;
    }
    if (config->firmware_info.max_vifs &&
        config->firmware_info.max_vifs < required_vifs)
    {
        return -RT_EINVAL;
    }

    for (index = 0; index < config->iface_combination_count; index++)
    {
        const struct rt_wlan_offload_iface_combination *combination =
            &config->iface_combinations[index];

        if (!combination->limit_count || !combination->limits ||
            !combination->max_interfaces ||
            !combination->num_different_channels ||
            (config->firmware_info.max_vifs &&
             combination->max_interfaces > config->firmware_info.max_vifs) ||
            (config->firmware_info.max_channel_contexts &&
             combination->num_different_channels >
                 config->firmware_info.max_channel_contexts))
        {
            return -RT_EINVAL;
        }
        {
            rt_size_t limit_index;

            for (limit_index = 0;
                 limit_index < combination->limit_count; limit_index++)
            {
                if (!combination->limits[limit_index].iftypes ||
                    !combination->limits[limit_index].maximum)
                {
                    return -RT_EINVAL;
                }
            }
        }
    }
    return RT_EOK;
}

static rt_wlan_transport_t wlan_offload_wlan_transport(
    const struct rt_wlan_offload_bus *bus)
{
    if (!bus)
    {
        return RT_WLAN_TRANSPORT_UNKNOWN;
    }
    switch (bus->type)
    {
    case RT_WLAN_OFFLOAD_BUS_USB:
        return RT_WLAN_TRANSPORT_USB;
    case RT_WLAN_OFFLOAD_BUS_SDIO:
        return RT_WLAN_TRANSPORT_SDIO;
    case RT_WLAN_OFFLOAD_BUS_SPI:
        return RT_WLAN_TRANSPORT_SPI;
    default:
        return RT_WLAN_TRANSPORT_UNKNOWN;
    }
}

rt_err_t rt_wlan_offload_register_radio(struct rt_wlan_offload_radio *radio,
                                   const struct rt_wlan_offload_radio_config *config)
{
    struct rt_wlan_offload_vif *vif;
    rt_bool_t control_requested;
    rt_err_t result;

    if (!radio || !config || config->api_version != RT_WLAN_OFFLOAD_API_VERSION ||
        !config->model_name || !config->model_name[0] ||
        !config->ops || !config->ops->transmit ||
        !config->ops->change_interface ||
        !(config->capabilities & (RT_WLAN_OFFLOAD_CAP_STA | RT_WLAN_OFFLOAD_CAP_AP)) ||
        ((config->capabilities & RT_WLAN_OFFLOAD_CAP_STA) &&
         !config->ops->scan) ||
        ((config->capabilities & RT_WLAN_OFFLOAD_CAP_AP) &&
         (!config->ops->start_ap || !config->ops->stop_ap)) ||
        ((config->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT) &&
         (!(config->capabilities & RT_WLAN_OFFLOAD_CAP_STA) ||
          !config->ops->auth || !config->ops->assoc ||
          !config->ops->disconnect || !config->ops->add_key ||
          !config->ops->delete_key || !config->ops->set_default_key ||
          !config->ops->transmit_mgmt)) ||
        ((config->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTH) &&
         (!(config->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT) ||
          !config->ops->external_auth_response)) ||
        ((config->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR) &&
         (!(config->capabilities & RT_WLAN_OFFLOAD_CAP_AP) ||
          !config->ops->add_station || !config->ops->del_station ||
          !config->ops->set_station_authorized || !config->ops->add_key ||
          !config->ops->delete_key || !config->ops->transmit_mgmt)))
    {
        return -RT_EINVAL;
    }
    /* A name alone still requests the device, for drivers predating the flag. */
    control_requested = config->control_device || config->control_name != RT_NULL;
#ifdef RT_WLAN_OFFLOAD_CONTROL
    if ((config->control_name && !config->control_name[0]) ||
        ((config->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT) &&
         !control_requested))
    {
        return -RT_EINVAL;
    }
#else
    if (control_requested ||
        (config->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT))
    {
        return -RT_EINVAL;
    }
#endif
    if (config->bus && config->bus->state == RT_WLAN_OFFLOAD_BUS_UNINITIALIZED)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_validate_metadata(config);
    if (result != RT_EOK)
    {
        return result;
    }

    rt_memset(radio, 0, sizeof(*radio));
    radio->ops = config->ops;
    radio->bus = config->bus;
    radio->capabilities = config->capabilities;
    radio->max_frame_size = config->max_frame_size ?
                            config->max_frame_size : WLAN_OFFLOAD_DEFAULT_FRAME_SIZE;
    rt_memcpy(radio->bands, config->bands, sizeof(radio->bands));
    radio->cipher_suites = config->cipher_suites;
    radio->cipher_suite_count = config->cipher_suite_count;
    radio->iface_combinations = config->iface_combinations;
    radio->iface_combination_count = config->iface_combination_count;
    radio->regulatory_domain = config->regulatory_domain;
    rt_memcpy(radio->permanent_address, config->permanent_address,
              sizeof(radio->permanent_address));
    radio->max_scan_ssids = config->max_scan_ssids ?
                            config->max_scan_ssids : 1;
    radio->max_scan_ie_length = config->max_scan_ie_length;
    radio->firmware_info = config->firmware_info;
    radio->driver_data = config->driver_data;
    result = rt_mutex_init(&radio->command_lock, "wo-cmd", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mutex_init(&radio->data_lock, "wo-data", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&radio->command_lock);
        return result;
    }
    result = rt_mutex_init(&radio->operation_lock, "wlan_offload", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&radio->data_lock);
        rt_mutex_detach(&radio->command_lock);
        return result;
    }

    if (config->capabilities & RT_WLAN_OFFLOAD_CAP_STA)
    {
        vif = &radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX];
        vif->radio = radio;
        vif->iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
        rt_memcpy(vif->address, radio->permanent_address,
                  sizeof(vif->address));
        result = rt_wlan_dev_register_auto(
            &vif->wlan, config->model_name, RT_WLAN_STATION,
            wlan_offload_wlan_transport(config->bus),
            &wlan_offload_wlan_ops, vif);
        if (result != RT_EOK)
        {
            rt_mutex_detach(&radio->operation_lock);
            rt_mutex_detach(&radio->data_lock);
            rt_mutex_detach(&radio->command_lock);
            return result;
        }
        vif->wlan.flags |= RT_WLAN_FLAG_DIRECT_TX;
        vif->registered = RT_TRUE;
    }

    if (config->capabilities & RT_WLAN_OFFLOAD_CAP_AP)
    {
        vif = &radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX];
        vif->radio = radio;
        vif->iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
        rt_memcpy(vif->address, radio->permanent_address,
                  sizeof(vif->address));
        result = rt_wlan_dev_register_auto(
            &vif->wlan, config->model_name, RT_WLAN_AP,
            wlan_offload_wlan_transport(config->bus),
            &wlan_offload_wlan_ops, vif);
        if (result != RT_EOK)
        {
            vif = &radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX];
            if (vif->registered)
            {
                rt_wlan_dev_unregister(&vif->wlan);
                vif->registered = RT_FALSE;
            }
            rt_mutex_detach(&radio->operation_lock);
            rt_mutex_detach(&radio->data_lock);
            rt_mutex_detach(&radio->command_lock);
            return result;
        }
        vif->wlan.flags |= RT_WLAN_FLAG_DIRECT_TX;
        vif->registered = RT_TRUE;
    }

    radio->state = RT_WLAN_OFFLOAD_REGISTERED;
#ifdef RT_WLAN_OFFLOAD_CONTROL
    if (control_requested)
    {
        result = rt_wlan_offload_control_register(radio, config->control_name);
        if (result != RT_EOK)
        {
            int index;

            radio->state = RT_WLAN_OFFLOAD_UNREGISTERED;
            for (index = 0; index < RT_WLAN_OFFLOAD_WLAN_VIF_COUNT; index++)
            {
                vif = &radio->vifs[index];
                if (vif->registered)
                {
                    rt_wlan_dev_unregister(&vif->wlan);
                    vif->registered = RT_FALSE;
                }
            }
            rt_mutex_detach(&radio->operation_lock);
            rt_mutex_detach(&radio->data_lock);
            rt_mutex_detach(&radio->command_lock);
            return result;
        }
    }
#endif
    rt_work_init(&radio->recovery_work, wlan_offload_recovery_work, radio);
    radio->recovery_work_initialized = RT_TRUE;
    return RT_EOK;
}

rt_err_t rt_wlan_offload_unregister_radio(struct rt_wlan_offload_radio *radio)
{
    rt_bool_t unregister_vif[RT_WLAN_OFFLOAD_WLAN_VIF_COUNT] = {RT_FALSE};
    rt_bool_t sta_link;
    rt_bool_t ap_link;
    rt_bool_t stop_driver;
    rt_err_t result;
    int index;

    if (!radio || radio->state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&radio->command_lock, RT_WAITING_FOREVER);
    rt_mutex_take(&radio->data_lock, RT_WAITING_FOREVER);
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (radio->state == RT_WLAN_OFFLOAD_STARTING)
    {
        rt_mutex_release(&radio->operation_lock);
        rt_mutex_release(&radio->data_lock);
        rt_mutex_release(&radio->command_lock);
        return -RT_EBUSY;
    }
    stop_driver = radio->state == RT_WLAN_OFFLOAD_STARTED ||
                  radio->state == RT_WLAN_OFFLOAD_OFFLINE ||
                  radio->state == RT_WLAN_OFFLOAD_FAILED;
    sta_link = radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].link_up;
    ap_link = radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].link_up;
    radio->state = RT_WLAN_OFFLOAD_UNREGISTERED;
    for (index = 0; index < RT_WLAN_OFFLOAD_WLAN_VIF_COUNT; index++)
    {
        unregister_vif[index] = radio->vifs[index].registered;
        radio->vifs[index].registered = RT_FALSE;
        radio->vifs[index].link_up = RT_FALSE;
    }
    rt_mutex_release(&radio->operation_lock);
    rt_mutex_release(&radio->data_lock);
    rt_mutex_release(&radio->command_lock);

    /* Clear WLAN management and network state while its event handlers and
     * protocol attachment are still present. */
    if (sta_link && unregister_vif[WLAN_OFFLOAD_VIF_STA_INDEX])
    {
        wlan_offload_indicate_wlan(&radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX],
                              RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL, 0);
    }
    if (ap_link && unregister_vif[WLAN_OFFLOAD_VIF_AP_INDEX])
    {
        wlan_offload_indicate_wlan(&radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX],
                              RT_WLAN_DEV_EVT_AP_STOP, RT_NULL, 0);
    }

    if (radio->recovery_work_initialized)
    {
        rt_work_cancel_sync(&radio->recovery_work);
        radio->recovery_work_initialized = RT_FALSE;
        radio->recovery_queued = RT_FALSE;
    }

    if (stop_driver && radio->ops->stop)
    {
        radio->ops->stop(radio);
    }
    if (radio->bus)
    {
        rt_wlan_offload_bus_stop(radio->bus);
    }
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
    rt_wlan_offload_supplicant_deinit(radio);
#endif
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    rt_wlan_offload_hostapd_deinit(radio);
#endif
#ifdef RT_WLAN_OFFLOAD_CONTROL
    rt_wlan_offload_control_unregister(radio);
#endif

    /* Protocol and device teardown call into lwIP, SAL and device callbacks.
     * Do not hold the radio locks across those external subsystems.  The
     * UNREGISTERED state and cleared VIF flags already reject new operations. */
    for (index = 0; index < RT_WLAN_OFFLOAD_WLAN_VIF_COUNT; index++)
    {
        struct rt_wlan_offload_vif *vif = &radio->vifs[index];

        if (!unregister_vif[index])
        {
            continue;
        }
#ifdef RT_WLAN_MANAGE_ENABLE
        rt_wlan_mgnt_unregister_device(&vif->wlan);
#endif
#ifdef RT_WLAN_PROT_ENABLE
        if (vif->wlan.prot)
        {
            rt_wlan_prot_detach_dev(&vif->wlan);
        }
#endif
        result = rt_wlan_dev_unregister(&vif->wlan);
        if (result != RT_EOK)
        {
            return result;
        }
    }

    /* Drain operations that entered before the state transition, then detach
     * each mutex while it is still owned.  A release followed by detach has a
     * window in which a packet thread can acquire an object that is about to
     * be zeroed and reused on USB re-enumeration. */
    result = rt_mutex_take(&radio->command_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mutex_take(&radio->data_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_mutex_release(&radio->command_lock);
        return result;
    }
    result = rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_mutex_release(&radio->data_lock);
        rt_mutex_release(&radio->command_lock);
        return result;
    }
    result = rt_mutex_detach(&radio->operation_lock);
    if (result != RT_EOK)
    {
        rt_mutex_release(&radio->data_lock);
        rt_mutex_release(&radio->command_lock);
        return result;
    }
    result = rt_mutex_detach(&radio->data_lock);
    if (result != RT_EOK)
    {
        rt_mutex_release(&radio->command_lock);
        return result;
    }
    return rt_mutex_detach(&radio->command_lock);
}

static rt_err_t wlan_offload_set_radio_online_internal(
    struct rt_wlan_offload_radio *radio, rt_bool_t online,
    const struct rt_wlan_offload_event *event)
{
    rt_wlan_offload_event_handler_t handler;
    void *parameter;
    rt_bool_t sta_link = RT_FALSE;
    rt_bool_t ap_link = RT_FALSE;

#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
    if (!online)
    {
        rt_wlan_offload_supplicant_cancel(radio);
    }
#endif
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    if (!online)
    {
        rt_wlan_offload_hostapd_cancel(radio);
    }
#endif

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (online)
    {
        if (radio->state == RT_WLAN_OFFLOAD_STARTED)
        {
            rt_mutex_release(&radio->operation_lock);
            return RT_EOK;
        }
        if (radio->state != RT_WLAN_OFFLOAD_OFFLINE)
        {
            rt_mutex_release(&radio->operation_lock);
            return -RT_EBUSY;
        }
        radio->state = RT_WLAN_OFFLOAD_STARTED;
        radio->firmware_generation++;
        if (!radio->firmware_generation)
        {
            radio->firmware_generation++;
        }
    }
    else
    {
        if (radio->state == RT_WLAN_OFFLOAD_OFFLINE)
        {
            rt_mutex_release(&radio->operation_lock);
            return RT_EOK;
        }
        if (radio->state != RT_WLAN_OFFLOAD_STARTED)
        {
            rt_mutex_release(&radio->operation_lock);
            return -RT_EBUSY;
        }
        radio->state = RT_WLAN_OFFLOAD_OFFLINE;
        sta_link = radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].link_up;
        ap_link = radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].link_up;
        radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].link_up = RT_FALSE;
        radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].link_up = RT_FALSE;
        radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].pending_scan_id = 0;
        radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].pending_connect_id = 0;
        radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].pending_ap_id = 0;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
        wlan_offload_bss_cache_clear_locked(radio);
#endif
    }
    handler = radio->event_handler;
    parameter = radio->event_parameter;
    rt_mutex_release(&radio->operation_lock);

    if (sta_link && radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].registered)
    {
        wlan_offload_indicate_wlan(&radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX],
                              RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL, 0);
    }
    if (ap_link && radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].registered)
    {
        wlan_offload_indicate_wlan(&radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX],
                              RT_WLAN_DEV_EVT_AP_STOP, RT_NULL, 0);
    }
    if (handler)
    {
        handler(radio, event, parameter);
    }
#ifdef RT_WLAN_OFFLOAD_CONTROL
    rt_wlan_offload_control_report_event(radio, event);
#endif
    return RT_EOK;
}

rt_err_t rt_wlan_offload_set_radio_online(struct rt_wlan_offload_radio *radio,
                                     rt_bool_t online)
{
    struct rt_wlan_offload_event event;

    if (!radio)
    {
        return -RT_EINVAL;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = online ? RT_WLAN_OFFLOAD_EVENT_RADIO_ONLINE :
                          RT_WLAN_OFFLOAD_EVENT_RADIO_OFFLINE;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_MAX;
    event.status = RT_EOK;
    return wlan_offload_set_radio_online_internal(radio, online, &event);
}

rt_uint32_t rt_wlan_offload_alloc_request_id(struct rt_wlan_offload_radio *radio)
{
    rt_uint32_t request_id;

    if (!radio || radio->state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        return 0;
    }
    request_id = wlan_offload_alloc_request_id_internal(radio);
    return request_id;
}

rt_err_t rt_wlan_offload_update_firmware_info(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_firmware_info *info)
{
    rt_uint8_t required_vifs = 1;
    rt_size_t index;

    if (!radio || !info || radio->state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        return -RT_EINVAL;
    }
    if ((radio->capabilities & RT_WLAN_OFFLOAD_CAP_STA_AP_CONCURRENT) &&
        (radio->capabilities & RT_WLAN_OFFLOAD_CAP_STA) &&
        (radio->capabilities & RT_WLAN_OFFLOAD_CAP_AP))
    {
        required_vifs = 2;
    }
    if (info->max_vifs && info->max_vifs < required_vifs)
    {
        return -RT_EINVAL;
    }
    for (index = 0; index < radio->iface_combination_count; index++)
    {
        const struct rt_wlan_offload_iface_combination *combination =
            &radio->iface_combinations[index];

        if ((info->max_vifs &&
             combination->max_interfaces > info->max_vifs) ||
            (info->max_channel_contexts &&
             combination->num_different_channels >
                 info->max_channel_contexts))
        {
            return -RT_EINVAL;
        }
    }

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    radio->firmware_info = *info;
    rt_mutex_release(&radio->operation_lock);
    return RT_EOK;
}

rt_err_t rt_wlan_offload_get_firmware_info(
    struct rt_wlan_offload_radio *radio,
    struct rt_wlan_offload_firmware_info *info,
    rt_uint32_t *generation)
{
    if (!radio || !info || radio->state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    *info = radio->firmware_info;
    if (generation)
    {
        *generation = radio->firmware_generation;
    }
    rt_mutex_release(&radio->operation_lock);
    return RT_EOK;
}

struct rt_wlan_offload_vif *rt_wlan_offload_get_vif(struct rt_wlan_offload_radio *radio,
                                          enum rt_wlan_offload_iftype iftype)
{
    int index;

    if (!radio)
    {
        return RT_NULL;
    }
    index = wlan_offload_vif_index(iftype);
    if (index < 0 || !radio->vifs[index].registered)
    {
        return RT_NULL;
    }
    return &radio->vifs[index];
}

struct rt_wlan_device *rt_wlan_offload_get_wlan(struct rt_wlan_offload_radio *radio,
                                           enum rt_wlan_offload_iftype iftype)
{
    struct rt_wlan_offload_vif *vif = rt_wlan_offload_get_vif(radio, iftype);

    return vif ? &vif->wlan : RT_NULL;
}

void *rt_wlan_offload_get_driver_data(struct rt_wlan_offload_radio *radio)
{
    return radio ? radio->driver_data : RT_NULL;
}

rt_err_t rt_wlan_offload_rx(struct rt_wlan_offload_radio *radio,
                       enum rt_wlan_offload_iftype iftype,
                       const void *data, int length)
{
    const rt_uint8_t *frame = data;
    struct rt_wlan_offload_vif *vif;
    rt_bool_t promiscuous;
    rt_err_t result;

    if (!radio || !data || length <= 0 ||
        (rt_size_t)length > radio->max_frame_size)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    if (radio->state != RT_WLAN_OFFLOAD_STARTED)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_EBUSY;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || !vif->enabled)
    {
        rt_mutex_release(&radio->operation_lock);
        return -RT_EBUSY;
    }
    promiscuous = vif->promiscuous;
    rt_mutex_release(&radio->operation_lock);

    if (promiscuous)
    {
        rt_wlan_dev_promisc_handler(&vif->wlan, (void *)data, length);
    }
    if ((radio->capabilities & (RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT |
                                RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR)) &&
        length >= 14 && frame[12] == 0x88 && frame[13] == 0x8e)
    {
        struct rt_wlan_offload_event event;
#if defined(RT_WLAN_OFFLOAD_EMBEDDED_WPA2) || \
    defined(RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD)
        rt_err_t supplicant_result;
#endif

        rt_memset(&event, 0, sizeof(event));
        event.type = RT_WLAN_OFFLOAD_EVENT_EAPOL_RX;
        event.iftype = iftype;
        event.status = RT_EOK;
        rt_memcpy(event.data.eapol.destination, frame, 6);
        rt_memcpy(event.data.eapol.source, frame + 6, 6);
        event.data.eapol.data = frame + 14;
        event.data.eapol.length = length - 14;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
        if (rt_wlan_offload_hostapd_handle_eapol(
                radio, iftype, event.data.eapol.source,
                event.data.eapol.destination, event.data.eapol.data,
                event.data.eapol.length, &supplicant_result))
        {
            return supplicant_result;
        }
#endif
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
        if (rt_wlan_offload_supplicant_handle_eapol(
                radio, iftype, event.data.eapol.source,
                event.data.eapol.destination, event.data.eapol.data,
                event.data.eapol.length, &supplicant_result))
        {
            return supplicant_result;
        }
#endif
        return rt_wlan_offload_report_event(radio, &event);
    }
    return rt_wlan_dev_report_data(&vif->wlan, (void *)data, length);
}

void rt_wlan_offload_set_event_handler(struct rt_wlan_offload_radio *radio,
                                  rt_wlan_offload_event_handler_t handler,
                                  void *parameter)
{
    if (!radio || radio->state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        return;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    radio->event_handler = handler;
    radio->event_parameter = parameter;
    rt_mutex_release(&radio->operation_lock);
}

rt_err_t rt_wlan_offload_report_event(struct rt_wlan_offload_radio *radio,
                                 const struct rt_wlan_offload_event *event)
{
    struct rt_wlan_offload_vif *vif = RT_NULL;
    struct rt_wlan_info info;
    rt_wlan_offload_event_handler_t handler;
    void *parameter;
    rt_wlan_dev_event_t wlan_event = RT_WLAN_DEV_EVT_MAX;
    const void *wlan_data = RT_NULL;
    rt_size_t wlan_length = 0;
    rt_err_t result = RT_EOK;
    rt_bool_t firmware_sta_disconnect = RT_FALSE;
    rt_bool_t firmware_ap_stop = RT_FALSE;
    rt_bool_t management_filter = RT_FALSE;
    rt_bool_t schedule_recovery = RT_FALSE;

    if (!radio || !event || (int)event->type < 0 ||
        event->type > RT_WLAN_OFFLOAD_EVENT_TKIP_MIC_FAILURE)
    {
        return -RT_EINVAL;
    }
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    if (rt_wlan_offload_hostapd_filter_event(radio, event))
    {
        return RT_EOK;
    }
#endif
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
    if (rt_wlan_offload_supplicant_filter_event(radio, event))
    {
        return RT_EOK;
    }
#endif
    if (event->type == RT_WLAN_OFFLOAD_EVENT_RADIO_ONLINE ||
        event->type == RT_WLAN_OFFLOAD_EVENT_RADIO_OFFLINE)
    {
        return wlan_offload_set_radio_online_internal(
            radio, event->type == RT_WLAN_OFFLOAD_EVENT_RADIO_ONLINE, event);
    }

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (radio->state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        result = -RT_EBUSY;
        goto exit;
    }
    if (event->type != RT_WLAN_OFFLOAD_EVENT_REGULATORY_CHANGED &&
        event->type != RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR)
    {
        if (radio->state != RT_WLAN_OFFLOAD_STARTED)
        {
            result = -RT_EBUSY;
            goto exit;
        }
        vif = wlan_offload_get_vif_locked(radio, event->iftype);
        if (!vif)
        {
            result = -RT_EINVAL;
            goto exit;
        }
        if (((event->type == RT_WLAN_OFFLOAD_EVENT_SCAN_RESULT ||
              event->type == RT_WLAN_OFFLOAD_EVENT_SCAN_DONE ||
              event->type == RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT ||
              event->type == RT_WLAN_OFFLOAD_EVENT_DISCONNECTED ||
              event->type == RT_WLAN_OFFLOAD_EVENT_AUTH_RX ||
              event->type == RT_WLAN_OFFLOAD_EVENT_ASSOC_RX ||
              event->type == RT_WLAN_OFFLOAD_EVENT_EXTERNAL_AUTH_REQUIRED) &&
             vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION) ||
            ((event->type == RT_WLAN_OFFLOAD_EVENT_AP_STARTED ||
              event->type == RT_WLAN_OFFLOAD_EVENT_AP_STOPPED ||
              event->type == RT_WLAN_OFFLOAD_EVENT_NEW_STATION ||
              event->type == RT_WLAN_OFFLOAD_EVENT_DEL_STATION) &&
             vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_AP))
        {
            result = -RT_EINVAL;
            goto exit;
        }
    }

    switch (event->type)
    {
    case RT_WLAN_OFFLOAD_EVENT_SCAN_RESULT:
        if (!event->request_id || event->request_id != vif->pending_scan_id)
        {
            result = -RT_EINVAL;
            break;
        }
        result = wlan_offload_network_to_wlan_info(radio, &event->data.network,
                                              &info);
        if (result == RT_EOK)
        {
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
            wlan_offload_bss_cache_update_locked(radio,
                                                  &event->data.network);
#endif
            wlan_event = RT_WLAN_DEV_EVT_SCAN_REPORT;
            wlan_data = &info;
            wlan_length = sizeof(info);
        }
        break;

    case RT_WLAN_OFFLOAD_EVENT_SCAN_DONE:
        if (!event->request_id || event->request_id != vif->pending_scan_id)
        {
            result = -RT_EINVAL;
            break;
        }
        vif->pending_scan_id = 0;
        wlan_event = RT_WLAN_DEV_EVT_SCAN_DONE;
        break;

    case RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT:
        if (!event->request_id || event->request_id != vif->pending_connect_id)
        {
            result = -RT_EINVAL;
            break;
        }
        vif->pending_connect_id = 0;
        vif->link_up = event->status == RT_EOK;
        wlan_event = event->status == RT_EOK ?
                     RT_WLAN_DEV_EVT_CONNECT : RT_WLAN_DEV_EVT_CONNECT_FAIL;
        break;

    case RT_WLAN_OFFLOAD_EVENT_DISCONNECTED:
        vif->pending_connect_id = 0;
        vif->link_up = RT_FALSE;
        wlan_event = RT_WLAN_DEV_EVT_DISCONNECT;
        break;

    case RT_WLAN_OFFLOAD_EVENT_AP_STARTED:
        if (vif->pending_ap_id &&
            event->request_id != vif->pending_ap_id)
        {
            result = -RT_EINVAL;
            break;
        }
        vif->pending_ap_id = 0;
        vif->link_up = event->status == RT_EOK;
        if (event->status == RT_EOK)
        {
            wlan_event = RT_WLAN_DEV_EVT_AP_START;
            wlan_data = wlan_offload_mac_is_zero(event->data.network.bssid) ?
                        radio->permanent_address : event->data.network.bssid;
            wlan_length = 6;
        }
        break;

    case RT_WLAN_OFFLOAD_EVENT_AP_STOPPED:
        if (vif->pending_ap_id &&
            event->request_id != vif->pending_ap_id)
        {
            result = -RT_EINVAL;
            break;
        }
        vif->pending_ap_id = 0;
        vif->link_up = RT_FALSE;
        wlan_event = RT_WLAN_DEV_EVT_AP_STOP;
        break;

    case RT_WLAN_OFFLOAD_EVENT_NEW_STATION:
    case RT_WLAN_OFFLOAD_EVENT_DEL_STATION:
        INVALID_INFO(&info);
        info.rssi = event->data.station.rssi;
        rt_memcpy(info.bssid, event->data.station.mac, sizeof(info.bssid));
        wlan_event = event->type == RT_WLAN_OFFLOAD_EVENT_NEW_STATION ?
                     RT_WLAN_DEV_EVT_AP_ASSOCIATED :
                     RT_WLAN_DEV_EVT_AP_DISASSOCIATED;
        wlan_data = &info;
        wlan_length = sizeof(info);
        break;

    case RT_WLAN_OFFLOAD_EVENT_AUTH_RX:
    case RT_WLAN_OFFLOAD_EVENT_ASSOC_RX:
    case RT_WLAN_OFFLOAD_EVENT_MGMT_RX:
        if ((event->data.management.length &&
             !event->data.management.data) ||
            wlan_offload_validate_channel(radio,
                                     &event->data.management.channel,
                                     RT_FALSE) != RT_EOK)
        {
            result = -RT_EINVAL;
        }
        else if (event->type == RT_WLAN_OFFLOAD_EVENT_MGMT_RX &&
                 vif->management_filter)
        {
            management_filter = RT_TRUE;
        }
        break;

    case RT_WLAN_OFFLOAD_EVENT_MGMT_TX_STATUS:
        if (event->data.tx_status.length && !event->data.tx_status.data)
        {
            result = -RT_EINVAL;
        }
        break;

    case RT_WLAN_OFFLOAD_EVENT_EAPOL_RX:
        if (!event->data.eapol.length || !event->data.eapol.data)
        {
            result = -RT_EINVAL;
        }
        break;

    case RT_WLAN_OFFLOAD_EVENT_REGULATORY_CHANGED:
        break;

    case RT_WLAN_OFFLOAD_EVENT_EXTERNAL_AUTH_REQUIRED:
        if (!event->request_id || !event->data.external_auth.ssid.len ||
            event->data.external_auth.ssid.len > RT_WLAN_SSID_MAX_LENGTH ||
            wlan_offload_mac_is_zero(event->data.external_auth.bssid))
        {
            result = -RT_EINVAL;
        }
        break;

    case RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR:
        if (event->data.firmware.dump_length && !event->data.firmware.dump)
        {
            result = -RT_EINVAL;
            break;
        }
        if (radio->state == RT_WLAN_OFFLOAD_STARTING)
        {
            /* Do not overwrite STARTING.  rt_wlan_offload_unregister_radio()
             * refuses teardown in that state precisely because registration is
             * still in flight; moving to FAILED here would admit teardown and
             * let it race the start path.  The starting thread reports the
             * failure through its own return value instead. */
            result = -RT_EBUSY;
            break;
        }
        radio->state = RT_WLAN_OFFLOAD_FAILED;
        if (radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].link_up)
        {
            radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].link_up = RT_FALSE;
            firmware_sta_disconnect = RT_TRUE;
        }
        if (radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].link_up)
        {
            radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].link_up = RT_FALSE;
            firmware_ap_stop = RT_TRUE;
        }
        radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].pending_scan_id = 0;
        radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].pending_connect_id = 0;
        radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].pending_ap_id = 0;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
        wlan_offload_bss_cache_clear_locked(radio);
#endif
        if (radio->recovery_work_initialized && !radio->recovery_queued)
        {
            radio->recovery_queued = RT_TRUE;
            schedule_recovery = RT_TRUE;
        }
        break;

    case RT_WLAN_OFFLOAD_EVENT_TKIP_MIC_FAILURE:
        break;

    default:
        result = -RT_EINVAL;
        break;
    }

    if (result != RT_EOK)
    {
        goto exit;
    }
    handler = radio->event_handler;
    parameter = radio->event_parameter;
    rt_mutex_release(&radio->operation_lock);

    if (schedule_recovery &&
        rt_work_submit(&radio->recovery_work,
                       rt_tick_from_millisecond(100)) != RT_EOK)
    {
        rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
        radio->recovery_queued = RT_FALSE;
        rt_mutex_release(&radio->operation_lock);
        LOG_E("cannot queue WLAN offload firmware recovery");
    }

    if (firmware_sta_disconnect &&
        radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX].registered)
    {
        wlan_offload_indicate_wlan(&radio->vifs[WLAN_OFFLOAD_VIF_STA_INDEX],
                              RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL, 0);
    }
    if (firmware_ap_stop && radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX].registered)
    {
        wlan_offload_indicate_wlan(&radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX],
                              RT_WLAN_DEV_EVT_AP_STOP, RT_NULL, 0);
    }
    if (wlan_event != RT_WLAN_DEV_EVT_MAX)
    {
        wlan_offload_indicate_wlan(vif, wlan_event, wlan_data, wlan_length);
    }
    if (management_filter)
    {
        rt_wlan_dev_mgnt_filter_handler(
            &vif->wlan, (void *)event->data.management.data,
            (int)event->data.management.length);
    }
    if (handler)
    {
        handler(radio, event, parameter);
    }
#ifdef RT_WLAN_OFFLOAD_CONTROL
    rt_wlan_offload_control_report_event(radio, event);
#endif
    return RT_EOK;

exit:
    rt_mutex_release(&radio->operation_lock);
    return result;
}

rt_err_t rt_wlan_offload_change_interface(struct rt_wlan_offload_radio *radio,
                                      enum rt_wlan_offload_iftype iftype,
                                      rt_bool_t enabled)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    result = vif ? wlan_offload_change_interface_submit(vif, enabled) : -RT_EINVAL;
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_scan(struct rt_wlan_offload_radio *radio,
                         enum rt_wlan_offload_iftype iftype,
                         const struct rt_wlan_offload_scan_request *request)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    result = vif ? wlan_offload_scan_submit(vif, request) : -RT_EINVAL;
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_abort_scan(struct rt_wlan_offload_radio *radio,
                               enum rt_wlan_offload_iftype iftype,
                               rt_uint32_t request_id)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    result = vif ? wlan_offload_abort_scan_submit(vif, request_id) : -RT_EINVAL;
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_connect(struct rt_wlan_offload_radio *radio,
                            enum rt_wlan_offload_iftype iftype,
                            const struct rt_wlan_offload_connect_request *request)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    result = vif ? wlan_offload_connect_submit(vif, request) : -RT_EINVAL;
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_disconnect(struct rt_wlan_offload_radio *radio,
                               enum rt_wlan_offload_iftype iftype,
                               rt_uint32_t request_id, rt_uint16_t reason)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    result = vif ? wlan_offload_disconnect_submit(vif, request_id, reason) :
                   -RT_EINVAL;
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_start_ap(struct rt_wlan_offload_radio *radio,
                             enum rt_wlan_offload_iftype iftype,
                             const struct rt_wlan_offload_ap_settings *settings)
{
    struct rt_wlan_offload_vif *vif;
    const struct rt_wlan_offload_ap_settings *effective = settings;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    struct rt_wlan_offload_ap_settings local;
    rt_bool_t use_embedded_hostapd = RT_FALSE;
#endif
    rt_err_t result;

#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    if (radio && settings &&
        (radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR) &&
        !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD))
    {
        local = *settings;
        result = rt_wlan_offload_hostapd_prepare(radio, &local);
        if (result != RT_EOK)
        {
            return result;
        }
        effective = &local;
        use_embedded_hostapd = RT_TRUE;
    }
#endif
    result = wlan_offload_operation_enter(radio);

    if (result != RT_EOK)
    {
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
        if (use_embedded_hostapd) rt_wlan_offload_hostapd_cancel(radio);
#endif
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    result = vif ? wlan_offload_start_ap_submit(vif, effective) : -RT_EINVAL;
    wlan_offload_operation_exit(radio);
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    if (result != RT_EOK && use_embedded_hostapd)
    {
        rt_wlan_offload_hostapd_cancel(radio);
    }
#endif
    return result;
}

rt_err_t rt_wlan_offload_stop_ap(struct rt_wlan_offload_radio *radio,
                            enum rt_wlan_offload_iftype iftype,
                            rt_uint32_t request_id)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    result = vif ? wlan_offload_stop_ap_submit(vif, request_id) : -RT_EINVAL;
    wlan_offload_operation_exit(radio);
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    if (result == RT_EOK &&
        (radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR))
    {
        rt_wlan_offload_hostapd_cancel(radio);
    }
#endif
    return result;
}

rt_err_t rt_wlan_offload_ap_channel_changed(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_channel_definition *channel)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result = RT_EOK;

    if (!radio || !channel ||
        wlan_offload_validate_channel(radio, channel, RT_FALSE) != RT_EOK)
    {
        return -RT_EINVAL;
    }
    vif = &radio->vifs[WLAN_OFFLOAD_VIF_AP_INDEX];
    if (radio->state != RT_WLAN_OFFLOAD_STARTED || !vif->enabled ||
        !vif->link_up)
    {
        return -RT_EBUSY;
    }
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    if ((radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR) &&
        !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD))
    {
        result = rt_wlan_offload_hostapd_channel_changed(radio, channel);
    }
#endif
#ifdef RT_WLAN_MANAGE_ENABLE
    if (result == RT_EOK)
    {
        rt_802_11_band_t band =
            channel->band == RT_WLAN_OFFLOAD_BAND_5GHZ ?
            RT_802_11_BAND_5GHZ : RT_802_11_BAND_2_4GHZ;

        (void)rt_wlan_ap_update_channel(band, channel->primary_channel);
    }
#endif
    return result;
}

rt_err_t rt_wlan_offload_del_station(struct rt_wlan_offload_radio *radio,
                                enum rt_wlan_offload_iftype iftype,
                                rt_uint32_t request_id,
                                const rt_uint8_t mac[6], rt_uint16_t reason)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!mac || !request_id)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_AP)
    {
        result = -RT_EINVAL;
    }
    else
    {
        result = radio->ops->del_station ?
                 radio->ops->del_station(vif, request_id, mac, reason) :
                 -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_add_station(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    rt_uint32_t request_id,
    const struct rt_wlan_offload_station_parameters *station)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!station || !request_id || wlan_offload_mac_is_zero(station->mac) ||
        !station->aid ||
        (station->association_ies_length && !station->association_ies))
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_AP ||
        !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR))
    {
        result = -RT_EINVAL;
    }
    else
    {
        result = radio->ops->add_station ?
                 radio->ops->add_station(vif, request_id, station) :
                 -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_set_station_authorized(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    rt_uint32_t request_id, const rt_uint8_t mac[6], rt_bool_t authorized)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!mac || !request_id || wlan_offload_mac_is_zero(mac))
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif ||
        !((vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION &&
           (radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT)) ||
          (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP &&
           (radio->capabilities &
            RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR))))
    {
        result = -RT_EINVAL;
    }
    else
    {
        /* Some external-supplicant transports authorize the station as part
         * of pairwise-key installation. Drivers with a separate controlled
         * port operation expose this callback and are completed explicitly. */
        result = radio->ops->set_station_authorized ?
                 radio->ops->set_station_authorized(
                     vif, request_id, mac, authorized) :
                 vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
                 RT_EOK : -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_auth(struct rt_wlan_offload_radio *radio,
                         enum rt_wlan_offload_iftype iftype,
                         const struct rt_wlan_offload_auth_request *request)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!request || !request->request_id ||
        request->ssid.len > RT_WLAN_SSID_MAX_LENGTH ||
        (radio && wlan_offload_validate_channel(radio, &request->channel,
                                           RT_FALSE) != RT_EOK) ||
        (request->auth_data_length && !request->auth_data))
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION ||
        !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT))
    {
        result = -RT_EINVAL;
    }
    else
    {
        result = radio->ops->auth ? radio->ops->auth(vif, request) : -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_assoc(struct rt_wlan_offload_radio *radio,
                          enum rt_wlan_offload_iftype iftype,
                          const struct rt_wlan_offload_assoc_request *request)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!request || !request->request_id ||
        (request->ies_length && !request->ies))
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION ||
        !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT))
    {
        result = -RT_EINVAL;
    }
    else
    {
        rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
        if (!vif->enabled || vif->pending_connect_id)
        {
            rt_mutex_release(&radio->operation_lock);
            result = -RT_EBUSY;
            wlan_offload_operation_exit(radio);
            return result;
        }
        vif->pending_connect_id = request->request_id;
        rt_mutex_release(&radio->operation_lock);
        result = radio->ops->assoc ? radio->ops->assoc(vif, request) : -RT_ENOSYS;
        if (result != RT_EOK)
        {
            rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
            if (vif->pending_connect_id == request->request_id)
            {
                vif->pending_connect_id = 0;
            }
            rt_mutex_release(&radio->operation_lock);
        }
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_add_key(struct rt_wlan_offload_radio *radio,
                            enum rt_wlan_offload_iftype iftype,
                            rt_uint32_t request_id,
                            const struct rt_wlan_offload_key *key)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!request_id || !key || !key->key_length ||
        key->key_length > RT_WLAN_OFFLOAD_MAX_KEY_LENGTH ||
        key->sequence_length > RT_WLAN_OFFLOAD_MAX_SEQUENCE_LENGTH)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || !(radio->capabilities &
                  (RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT |
                   RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR)))
    {
        result = -RT_EINVAL;
    }
    else
    {
        result = radio->ops->add_key ?
                 radio->ops->add_key(vif, request_id, key) : -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_delete_key(struct rt_wlan_offload_radio *radio,
                               enum rt_wlan_offload_iftype iftype,
                               rt_uint32_t request_id, rt_uint8_t index,
                               rt_bool_t pairwise, const rt_uint8_t peer[6])
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!request_id)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || !(radio->capabilities &
                  (RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT |
                   RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR)))
    {
        result = -RT_EINVAL;
    }
    else
    {
        result = radio->ops->delete_key ?
                 radio->ops->delete_key(vif, request_id, index,
                                        pairwise, peer) : -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_set_default_key(struct rt_wlan_offload_radio *radio,
                                    enum rt_wlan_offload_iftype iftype,
                                    rt_uint32_t request_id, rt_uint8_t index,
                                    rt_bool_t unicast, rt_bool_t multicast)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!request_id)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || !(radio->capabilities &
                  (RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT |
                   RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR)))
    {
        result = -RT_EINVAL;
    }
    else
    {
        result = radio->ops->set_default_key ?
                 radio->ops->set_default_key(vif, request_id, index,
                                             unicast, multicast) : -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_transmit_mgmt(struct rt_wlan_offload_radio *radio,
                                  enum rt_wlan_offload_iftype iftype,
                                  const struct rt_wlan_offload_mgmt_frame *frame)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result;

    if (!frame || !frame->request_id || !frame->data || !frame->length ||
        (radio && (frame->length > radio->max_frame_size ||
                   wlan_offload_validate_channel(radio, &frame->channel,
                                            RT_FALSE) != RT_EOK)))
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_operation_enter(radio);
    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || !(radio->capabilities &
                  (RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT |
                   RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR)))
    {
        result = -RT_EINVAL;
    }
    else
    {
        result = radio->ops->transmit_mgmt ?
                 radio->ops->transmit_mgmt(vif, frame) : -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_external_auth_response(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    rt_uint16_t status)
{
    struct rt_wlan_offload_vif *vif;
    rt_err_t result = wlan_offload_operation_enter(radio);

    if (result != RT_EOK)
    {
        return result;
    }
    vif = wlan_offload_get_vif_locked(radio, iftype);
    if (!vif || !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT))
    {
        result = -RT_EINVAL;
    }
    else
    {
        result = radio->ops->external_auth_response ?
                 radio->ops->external_auth_response(vif, status) : -RT_ENOSYS;
    }
    wlan_offload_operation_exit(radio);
    return result;
}

rt_err_t rt_wlan_offload_transmit_eapol(struct rt_wlan_offload_radio *radio,
                                   enum rt_wlan_offload_iftype iftype,
                                   const rt_uint8_t destination[6],
                                   const void *data, rt_size_t length)
{
    struct rt_wlan_offload_vif *vif;
    rt_uint8_t *frame;
    rt_err_t result;

    if (!radio || !destination || !data || !length ||
        length + 14 > radio->max_frame_size)
    {
        return -RT_EINVAL;
    }
    frame = rt_malloc(length + 14);
    if (!frame)
    {
        return -RT_ENOMEM;
    }
    vif = rt_wlan_offload_get_vif(radio, iftype);
    if (!vif || !(radio->capabilities &
                  (RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT |
                   RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR)))
    {
        rt_free(frame);
        return -RT_EINVAL;
    }
    result = wlan_offload_data_enter(vif);
    if (result != RT_EOK)
    {
        rt_free(frame);
        return result;
    }
    rt_memcpy(frame, destination, 6);
    rt_memcpy(frame + 6, vif->address, 6);
    frame[12] = 0x88;
    frame[13] = 0x8e;
    rt_memcpy(frame + 14, data, length);
    result = radio->ops->transmit(vif, frame, length + 14);
    wlan_offload_data_exit(radio);
    rt_free(frame);
    return result;
}
