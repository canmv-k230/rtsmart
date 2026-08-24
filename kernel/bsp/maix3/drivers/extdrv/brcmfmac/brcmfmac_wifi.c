// SPDX-License-Identifier: ISC
/* RT-Smart WLAN offload binding for the brcmfmac BCDC firmware API. */
#include "brcmfmac.h"

#include <wlan_mgnt.h>
#ifdef RT_USING_PUFS
#include <pufs_rt.h>
#endif

#define DBG_TAG "brcmf.wifi"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define BRCMF_ESCAN_VERSION             1U
#define BRCMF_ESCAN_ACTION_START        1U
#define BRCMF_ESCAN_ACTION_ABORT        2U
#define BRCMF_BSS_TYPE_ANY              2
#define BRCMF_WPA3_AUTH_SAE_PSK         0x00040000U
#define BRCMF_DOT11_CAP_PRIVACY         0x0010U
#define BRCMF_IE_RSN                    48U
#define BRCMF_IE_VENDOR                 221U
#define BRCMF_WPA_OUI0                  0x00U
#define BRCMF_WPA_OUI1                  0x50U
#define BRCMF_WPA_OUI2                  0xf2U
#define BRCMF_WPA_OUI_TYPE              0x01U
#define BRCMF_RSN_OUI0                  0x00U
#define BRCMF_RSN_OUI1                  0x0fU
#define BRCMF_RSN_OUI2                  0xacU
#define BRCMF_SCAN_CIPHER_TKIP          0x01U
#define BRCMF_SCAN_CIPHER_CCMP          0x02U
#define BRCMF_SCAN_AKM_8021X            0x0001U
#define BRCMF_SCAN_AKM_PSK              0x0002U
#define BRCMF_SCAN_AKM_FT_8021X         0x0004U
#define BRCMF_SCAN_AKM_FT_PSK           0x0008U
#define BRCMF_SCAN_AKM_8021X_SHA256     0x0010U
#define BRCMF_SCAN_AKM_PSK_SHA256       0x0020U
#define BRCMF_SCAN_AKM_SAE              0x0040U
#define BRCMF_SCAN_AKM_FT_SAE           0x0080U
#define BRCMF_SCAN_AKM_SUITE_B          0x0100U
#define BRCMF_SCAN_AKM_SUITE_B_192      0x0200U
#define BRCMF_SCAN_AKM_OWE              0x0400U
#define BRCMF_MAX_SCAN_CHANNELS         64U
#define BRCMF_D11N_IO_TYPE              1U
#define BRCMF_D11N_CHSPEC_20            0x0b00U
#define BRCMF_D11N_CHSPEC_2G            0x2000U
#define BRCMF_D11N_CHSPEC_5G            0x1000U
#define BRCMF_D11AC_CHSPEC_20           0x1000U
#define BRCMF_D11AC_CHSPEC_5G           0xc000U
#define BRCMF_D11N_CHSPEC_BW_MASK       0x0c00U
#define BRCMF_D11AC_CHSPEC_BW_MASK      0x3800U
#define BRCMF_D11N_CHSPEC_BAND_MASK     0x3000U
#define BRCMF_D11AC_CHSPEC_BAND_MASK    0xc000U
#define BRCMF_CAP_BUFFER_SIZE             768U
#define BRCMF_CHANSPEC_BUFFER_SIZE        1536U
#define BRCMF_WL_CHAN_RADAR            (1U << 3)
#define BRCMF_WL_CHAN_PASSIVE          (1U << 5)
#define BRCMF_WL_CHAN_RESTRICTED       (1U << 6)
#define BRCMF_VNDR_IE_BEACON_FLAG      0x00000001U
#define BRCMF_VNDR_IE_HEADER_SIZE        12U
#define BRCMF_CLM_CHUNK_SIZE            1400U
#define BRCMF_CLM_FLAG_BEGIN            0x0002U
#define BRCMF_CLM_FLAG_END              0x0004U
#define BRCMF_CLM_HANDLER_VERSION       0x1000U
#define BRCMF_CLM_DOWNLOAD_TYPE         2U
#define BRCMF_JOIN_ACTIVE_TIME_MS       320U
#define BRCMF_JOIN_PASSIVE_TIME_MS      400U
#define BRCMF_JOIN_PROBE_INTERVAL_MS    20U
#define BRCMF_AP_INTERFACE_TIMEOUT_MS  1500U
#define BRCMF_PM_OFF                     0U
#define BRCMF_PM_FAST                    2U
#define BRCMF_REASON_DEAUTH_LEAVING      3U
#define BRCMF_REASON_CLASS3_NONASSOC     7U
#define BRCMF_CIS_TUPLE_BRCM            0x80U
#define BRCMF_CIS_TAG_MAC_ADDRESS       0x19U
#define BRCMF_ETH_P_IP                0x0800U
#define BRCMF_ETH_P_8021Q             0x8100U
#define BRCMF_ETH_P_8021AD            0x88a8U
#define BRCMF_ETH_P_IPV6              0x86ddU
#define BRCMF_ETH_P_EAPOL             0x888eU

struct brcmf_country
{
    char abbreviation[4];
    rt_int32_t revision;
    char code[4];
} BRCMF_PACKED;

static rt_country_code_t brcmf_country_from_alpha2(const char *alpha2)
{
    if (!alpha2 || rt_strlen(alpha2) != 2U)
    {
        return RT_COUNTRY_UNKNOWN;
    }
    if (alpha2[0] == 'C' && alpha2[1] == 'N')
    {
        return RT_COUNTRY_CHINA;
    }
    if (alpha2[0] == 'U' && alpha2[1] == 'S')
    {
        return RT_COUNTRY_UNITED_STATES;
    }
    if (alpha2[0] == 'U' && alpha2[1] == 'M')
    {
        return RT_COUNTRY_UNITED_STATES_MINOR_OUTLYING_ISLANDS;
    }
    if (alpha2[0] == 'X' && alpha2[1] == 'X')
    {
        return RT_COUNTRY_WORLD_WIDE_XX;
    }
    return RT_COUNTRY_UNKNOWN;
}

static rt_bool_t brcmf_mac_is_valid(const rt_uint8_t mac[BRCMF_ETH_ALEN])
{
    rt_bool_t all_zero = RT_TRUE;
    rt_bool_t all_ff = RT_TRUE;
    rt_size_t index;

    if (mac[0] & 1U)
    {
        return RT_FALSE;
    }
    for (index = 0; index < BRCMF_ETH_ALEN; index++)
    {
        all_zero = all_zero && mac[index] == 0U;
        all_ff = all_ff && mac[index] == 0xffU;
    }
    return !all_zero && !all_ff;
}

static rt_bool_t brcmf_mac_is_template(
    const rt_uint8_t mac[BRCMF_ETH_ALEN])
{
    static const rt_uint8_t template_mac[BRCMF_ETH_ALEN] = {
        0x00U, 0x90U, 0x4cU, 0xc5U, 0x12U, 0x38U
    };

    return !rt_memcmp(mac, template_mac, sizeof(template_mac));
}

static rt_bool_t brcmf_mac_from_cis(struct brcmf_context *context,
                                    rt_uint8_t mac[BRCMF_ETH_ALEN])
{
    struct rt_sdio_function_tuple *tuple = context->function1 ?
        context->function1->tuples : RT_NULL;

    for (; tuple; tuple = tuple->next)
    {
        if (tuple->code == BRCMF_CIS_TUPLE_BRCM &&
            tuple->size >= BRCMF_ETH_ALEN + 1U && tuple->data &&
            tuple->data[0] == BRCMF_CIS_TAG_MAC_ADDRESS)
        {
            rt_memcpy(mac, tuple->data + 1U, BRCMF_ETH_ALEN);
            if (brcmf_mac_is_valid(mac) && !brcmf_mac_is_template(mac))
            {
                return RT_TRUE;
            }
        }
    }
    return RT_FALSE;
}

#ifdef RT_USING_PUFS
static rt_bool_t brcmf_mac_from_uid(rt_uint8_t mac[BRCMF_ETH_ALEN])
{
    pufs_uid_st uid;
    rt_uint32_t hash = 2166136261U;
    rt_uint32_t mix = 0x9e3779b9U;
    rt_size_t index;

    if (pufs_get_uid(&uid) != SUCCESS)
    {
        return RT_FALSE;
    }
    for (index = 0; index < sizeof(uid.uid); index++)
    {
        hash = (hash ^ uid.uid[index]) * 16777619U;
        mix ^= uid.uid[index] + 0x9e3779b9U + (mix << 6) + (mix >> 2);
    }
    for (index = 0; index < BRCMF_ETH_ALEN; index++)
    {
        mac[index] = (rt_uint8_t)(
            (hash >> ((index & 3U) * 8U)) ^
            (mix >> (((index + 2U) & 3U) * 8U)));
        hash = hash * 16777619U + (rt_uint32_t)index;
        mix ^= hash + (mix << 6) + (mix >> 2);
    }
    mac[0] = (mac[0] & 0xfeU) | 0x02U;
    return brcmf_mac_is_valid(mac);
}
#endif

static void brcmf_mac_fallback(struct brcmf_context *context,
                               rt_uint8_t mac[BRCMF_ETH_ALEN])
{
    rt_uint64_t pointer = (rt_uint64_t)(rt_ubase_t)context;
    rt_uint32_t state = (rt_uint32_t)pointer ^
        (rt_uint32_t)(pointer >> 32) ^ context->chip.id ^
        context->chip.revision ^ (rt_uint32_t)rt_tick_get();
    rt_size_t index;

    if (!state)
    {
        state = 0x6d2b79f5U;
    }
    for (index = 0; index < BRCMF_ETH_ALEN; index++)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        mac[index] = (rt_uint8_t)state;
    }
    mac[0] = (mac[0] & 0xfeU) | 0x02U;
}

static rt_err_t brcmf_wifi_prepare_mac(struct brcmf_context *context)
{
    rt_uint8_t mac[BRCMF_ETH_ALEN];
    const char *source;
    rt_err_t result;

    if (brcmf_mac_is_valid(context->mac) &&
        !brcmf_mac_is_template(context->mac))
    {
        return RT_EOK;
    }
    if (brcmf_mac_from_cis(context, mac))
    {
        source = "SDIO CIS";
    }
#ifdef RT_USING_PUFS
    else if (brcmf_mac_from_uid(mac))
    {
        source = "SoC UID";
    }
#endif
    else
    {
        brcmf_mac_fallback(context, mac);
        source = "boot entropy";
    }

    result = brcmf_proto_iovar(context, context->sta_interface,
                               "cur_etheraddr", mac, sizeof(mac), RT_TRUE);
    if (result != RT_EOK)
    {
        LOG_E("could not replace invalid firmware MAC: %d", result);
        return result;
    }
    rt_memcpy(context->mac, mac, sizeof(context->mac));
    LOG_W("replaced firmware template MAC from %s with "
          "%02x:%02x:%02x:%02x:%02x:%02x", source,
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return RT_EOK;
}

static rt_err_t brcmf_wifi_program_country(
    struct brcmf_context *context, const char *alpha2, rt_int32_t revision,
    rt_country_code_t country)
{
    struct brcmf_country setting;
    rt_err_t result;

    if (!alpha2 || rt_strlen(alpha2) != 2U)
    {
        return -RT_EINVAL;
    }
    rt_memset(&setting, 0, sizeof(setting));
    rt_memcpy(setting.abbreviation, alpha2, 2U);
    rt_memcpy(setting.code, alpha2, 2U);
    brcmf_put_le32(&setting.revision, (rt_uint32_t)revision);
    result = brcmf_proto_command(context, context->sta_interface,
                                 BRCMF_C_SET_COUNTRY, &setting,
                                 sizeof(setting), RT_TRUE);
    if (result == RT_EOK)
    {
        context->country = country;
    }
    return result;
}

struct brcmf_channel_info
{
    rt_uint32_t hardware;
    rt_uint32_t target;
    rt_uint32_t scan;
} BRCMF_PACKED;

struct brcmf_download_data
{
    rt_uint16_t flags;
    rt_uint16_t type;
    rt_uint32_t length;
    rt_uint32_t crc;
    rt_uint8_t data[];
} BRCMF_PACKED;

static const struct rt_wlan_offload_channel g_brcmf_2ghz_channels[] = {
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 1, 2412, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 2, 2417, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 3, 2422, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 4, 2427, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 5, 2432, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 6, 2437, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 7, 2442, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 8, 2447, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 9, 2452, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 10, 2457, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 11, 2462, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 12, 2467, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 13, 2472, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 14, 2484, 0, 20},
};

static const struct rt_wlan_offload_channel g_brcmf_5ghz_channels[] = {
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 34, 5170, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 36, 5180, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 38, 5190, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 40, 5200, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 42, 5210, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 44, 5220, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 46, 5230, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 48, 5240, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 52, 5260, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 56, 5280, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 60, 5300, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 64, 5320, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 100, 5500, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 104, 5520, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 108, 5540, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 112, 5560, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 116, 5580, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 120, 5600, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 124, 5620, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 128, 5640, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 132, 5660, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 136, 5680, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 140, 5700, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 144, 5720, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 149, 5745, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 153, 5765, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 157, 5785, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 161, 5805, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 165, 5825, 0, 20},
};

_Static_assert(sizeof(g_brcmf_2ghz_channels) /
               sizeof(g_brcmf_2ghz_channels[0]) == BRCMF_2GHZ_CHANNEL_COUNT,
               "2 GHz channel table size mismatch");
_Static_assert(sizeof(g_brcmf_5ghz_channels) /
               sizeof(g_brcmf_5ghz_channels[0]) == BRCMF_5GHZ_CHANNEL_COUNT,
               "5 GHz channel table size mismatch");

static const struct rt_wlan_offload_rate g_brcmf_rates[] = {
    {10, 2, 0}, {20, 4, 0}, {55, 11, 0}, {110, 22, 0},
    {60, 12, 0}, {90, 18, 0}, {120, 24, 0}, {180, 36, 0},
    {240, 48, 0}, {360, 72, 0}, {480, 96, 0}, {540, 108, 0},
};

static const enum rt_wlan_offload_cipher g_brcmf_ciphers[] = {
    RT_WLAN_OFFLOAD_CIPHER_TKIP,
    RT_WLAN_OFFLOAD_CIPHER_CCMP,
};

static const struct rt_wlan_offload_iface_limit g_brcmf_limits[] = {
    {RT_WLAN_OFFLOAD_IFTYPE_BIT(RT_WLAN_OFFLOAD_IFTYPE_STATION) |
     RT_WLAN_OFFLOAD_IFTYPE_BIT(RT_WLAN_OFFLOAD_IFTYPE_AP), 2},
};

static const struct rt_wlan_offload_iface_combination g_brcmf_combinations[] = {
    {g_brcmf_limits, sizeof(g_brcmf_limits) / sizeof(g_brcmf_limits[0]),
     2, 1},
};

static rt_bool_t brcmf_chip_dual_band(rt_uint32_t chip);

static void brcmf_wifi_initialize_bands(struct brcmf_context *context)
{
    rt_size_t index;
    rt_uint32_t width_flags = RT_WLAN_OFFLOAD_CHANNEL_DISABLED |
        RT_WLAN_OFFLOAD_CHANNEL_NO_HT40_PLUS |
        RT_WLAN_OFFLOAD_CHANNEL_NO_HT40_MINUS |
        RT_WLAN_OFFLOAD_CHANNEL_NO_80MHZ |
        RT_WLAN_OFFLOAD_CHANNEL_NO_160MHZ |
        RT_WLAN_OFFLOAD_CHANNEL_NO_320MHZ;

    rt_memcpy(context->channels_2ghz, g_brcmf_2ghz_channels,
              sizeof(context->channels_2ghz));
    rt_memcpy(context->channels_5ghz, g_brcmf_5ghz_channels,
              sizeof(context->channels_5ghz));
    for (index = 0; index < BRCMF_2GHZ_CHANNEL_COUNT; index++)
    {
        context->channels_2ghz[index].flags = width_flags;
    }
    for (index = 0; index < BRCMF_5GHZ_CHANNEL_COUNT; index++)
    {
        context->channels_5ghz[index].flags = width_flags;
    }

    rt_memset(&context->band_2ghz, 0, sizeof(context->band_2ghz));
    context->band_2ghz.id = RT_WLAN_OFFLOAD_BAND_2GHZ;
    context->band_2ghz.phy_capabilities = RT_WLAN_OFFLOAD_PHY_11B |
                                         RT_WLAN_OFFLOAD_PHY_11G |
                                         RT_WLAN_OFFLOAD_PHY_HT;
    context->band_2ghz.channels = context->channels_2ghz;
    context->band_2ghz.channel_count = BRCMF_2GHZ_CHANNEL_COUNT;
    context->band_2ghz.rates = g_brcmf_rates;
    context->band_2ghz.rate_count = sizeof(g_brcmf_rates) /
                                    sizeof(g_brcmf_rates[0]);
    context->band_2ghz.max_spatial_streams = 1U;
    context->band_2ghz.max_channel_width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
    context->band_2ghz.max_channel_width_set = RT_TRUE;

    rt_memset(&context->band_5ghz, 0, sizeof(context->band_5ghz));
    context->band_5ghz.id = RT_WLAN_OFFLOAD_BAND_5GHZ;
    context->band_5ghz.phy_capabilities = RT_WLAN_OFFLOAD_PHY_11A |
                                         RT_WLAN_OFFLOAD_PHY_HT |
                                         RT_WLAN_OFFLOAD_PHY_VHT;
    context->band_5ghz.channels = context->channels_5ghz;
    context->band_5ghz.channel_count = BRCMF_5GHZ_CHANNEL_COUNT;
    context->band_5ghz.rates = g_brcmf_rates + 4U;
    context->band_5ghz.rate_count = sizeof(g_brcmf_rates) /
                                    sizeof(g_brcmf_rates[0]) - 4U;
    context->band_5ghz.max_spatial_streams = 1U;
    context->band_5ghz.max_channel_width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
    context->band_5ghz.max_channel_width_set = RT_TRUE;
}

static struct brcmf_context *brcmf_vif_context(
    struct rt_wlan_offload_vif *vif)
{
    return rt_wlan_offload_get_driver_data(vif->radio);
}

static rt_uint8_t brcmf_interface_index(struct brcmf_context *context,
                                        enum rt_wlan_offload_iftype type)
{
    return type == RT_WLAN_OFFLOAD_IFTYPE_AP ? context->ap_interface :
                                               context->sta_interface;
}

static rt_bool_t brcmf_event_is_station(
    const struct brcmf_context *context,
    const struct brcmf_event_message *message)
{
    return message->interface_index == context->sta_interface &&
           message->bsscfg_index == 0U;
}

static rt_bool_t brcmf_event_is_ap(
    const struct brcmf_context *context,
    const struct brcmf_event_message *message)
{
    return context->ap_interface_created &&
           message->interface_index == context->ap_interface &&
           message->bsscfg_index == context->ap_bsscfg;
}

static rt_bool_t brcmf_valid_station_address(
    const rt_uint8_t address[BRCMF_ETH_ALEN])
{
    rt_uint8_t nonzero = 0;
    rt_size_t index;

    for (index = 0; index < BRCMF_ETH_ALEN; index++)
    {
        nonzero |= address[index];
    }
    return nonzero && !(address[0] & 1U);
}

static void brcmf_reset_ap_interface(struct brcmf_context *context)
{
    context->ap_interface_pending = RT_FALSE;
    context->ap_interface_created = RT_FALSE;
    context->ap_interface = 1U;
    /* Linux reserves bsscfg 1 for legacy P2P. */
    context->ap_bsscfg = 2U;
}

static void brcmf_update_ap_interface(struct brcmf_context *context,
                                      rt_uint8_t interface_index,
                                      rt_uint8_t bsscfg_index)
{
    if (interface_index >= BRCMF_MAX_IFS ||
        bsscfg_index >= BRCMF_MAX_VIFS)
    {
        LOG_W("ignore invalid AP interface mapping ifidx=%u bsscfg=%u",
              interface_index, bsscfg_index);
        return;
    }
    if (context->ap_interface != interface_index ||
        context->ap_bsscfg != bsscfg_index)
    {
        LOG_I("AP firmware interface ifidx=%u bsscfg=%u (was %u/%u)",
              interface_index, bsscfg_index, context->ap_interface,
              context->ap_bsscfg);
        context->ap_interface = interface_index;
        context->ap_bsscfg = bsscfg_index;
    }
    context->ap_interface_created = RT_TRUE;
    if (context->ap_interface_pending)
    {
        context->ap_interface_pending = RT_FALSE;
        rt_completion_done(&context->ap_interface_completion);
    }
}

static void brcmf_update_ap_address(
    struct brcmf_context *context,
    const rt_uint8_t address[BRCMF_ETH_ALEN])
{
    if (brcmf_mac_is_valid(address))
    {
        rt_memcpy(
            context->radio.vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX].address,
            address, BRCMF_ETH_ALEN);
    }
}

static void brcmf_derive_ap_address(
    const rt_uint8_t station[BRCMF_ETH_ALEN],
    rt_uint8_t ap[BRCMF_ETH_ALEN])
{
    rt_memcpy(ap, station, BRCMF_ETH_ALEN);
    ap[0] = (ap[0] | 0x02U) & 0xfeU;
    if (!rt_memcmp(ap, station, BRCMF_ETH_ALEN))
    {
        ap[BRCMF_ETH_ALEN - 1U] ^= 0x01U;
    }
}

static rt_err_t brcmf_ensure_distinct_ap_address(
    struct brcmf_context *context)
{
    struct rt_wlan_offload_vif *station =
        &context->radio.vifs[RT_WLAN_OFFLOAD_VIF_STA_INDEX];
    struct rt_wlan_offload_vif *ap =
        &context->radio.vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX];
    rt_uint8_t address[BRCMF_ETH_ALEN];
    rt_err_t result;

    rt_memset(address, 0, sizeof(address));
    result = brcmf_proto_iovar(context, context->ap_interface,
                               "cur_etheraddr", address, sizeof(address),
                               RT_FALSE);
    if (result != RT_EOK)
    {
        LOG_E("could not query AP MAC: %d", result);
        return result;
    }
    if (brcmf_mac_is_valid(address) &&
        rt_memcmp(address, station->address, BRCMF_ETH_ALEN))
    {
        rt_memcpy(ap->address, address, BRCMF_ETH_ALEN);
        return RT_EOK;
    }

    brcmf_derive_ap_address(station->address, address);
    result = brcmf_proto_iovar(context, context->ap_interface,
                               "cur_etheraddr", address, sizeof(address),
                               RT_TRUE);
    if (result != RT_EOK)
    {
        LOG_E("could not assign a distinct AP MAC: %d", result);
        return result;
    }

    rt_memset(address, 0, sizeof(address));
    result = brcmf_proto_iovar(context, context->ap_interface,
                               "cur_etheraddr", address, sizeof(address),
                               RT_FALSE);
    if (result != RT_EOK)
    {
        LOG_E("could not verify AP MAC: %d", result);
        return result;
    }
    if (!brcmf_mac_is_valid(address) ||
        !rt_memcmp(address, station->address, BRCMF_ETH_ALEN))
    {
        LOG_E("firmware kept the AP and station MAC addresses identical");
        return -RT_EIO;
    }

    rt_memcpy(ap->address, address, BRCMF_ETH_ALEN);
    LOG_I("assigned AP MAC %02x:%02x:%02x:%02x:%02x:%02x",
          address[0], address[1], address[2], address[3], address[4],
          address[5]);
    return RT_EOK;
}

static void brcmf_put_integer(rt_uint8_t data[4], rt_uint32_t value)
{
    brcmf_put_le32(data, value);
}

static rt_err_t brcmf_command_integer(struct brcmf_context *context,
                                      rt_uint8_t interface_index,
                                      rt_uint32_t command,
                                      rt_uint32_t value)
{
    rt_uint8_t data[4];

    brcmf_put_integer(data, value);
    return brcmf_proto_command(context, interface_index, command, data,
                               sizeof(data), RT_TRUE);
}

static void brcmf_configure_arp_nd_offload(
    struct brcmf_context *context, rt_bool_t enabled)
{
    static const char * const iovars[] = {"arp_ol", "arpoe", "ndoe"};
    rt_uint32_t values[] = {enabled ? 0x09U : 0U, enabled ? 1U : 0U,
                            enabled ? 1U : 0U};
    rt_size_t index;

    for (index = 0; index < sizeof(iovars) / sizeof(iovars[0]); index++)
    {
        rt_err_t result = brcmf_proto_iovar_int(
            context, context->sta_interface, iovars[index], &values[index],
            RT_TRUE);

        if (result != RT_EOK)
        {
            LOG_W("could not %s %s: %d", enabled ? "enable" : "disable",
                  iovars[index], result);
        }
    }
}

static rt_err_t brcmf_set_bss(struct brcmf_context *context,
                              rt_uint32_t bsscfg_index,
                              rt_bool_t enabled)
{
    struct brcmf_bss_enable setting;

    brcmf_put_le32(&setting.bsscfg_index, bsscfg_index);
    brcmf_put_le32(&setting.enable, enabled ? 1U : 0U);
    return brcmf_proto_iovar(context, context->ap_interface, "bss",
                             &setting, sizeof(setting), RT_TRUE);
}

static rt_uint16_t brcmf_chanspec(struct brcmf_context *context,
                                  rt_uint8_t channel)
{
    if (context->io_type == BRCMF_D11N_IO_TYPE)
    {
        return channel | BRCMF_D11N_CHSPEC_20 |
               (channel <= 14U ? BRCMF_D11N_CHSPEC_2G :
                                 BRCMF_D11N_CHSPEC_5G);
    }
    return channel | BRCMF_D11AC_CHSPEC_20 |
           (channel <= 14U ? 0U : BRCMF_D11AC_CHSPEC_5G);
}

static struct rt_wlan_offload_channel *brcmf_find_channel(
    struct brcmf_context *context, enum rt_wlan_offload_band_id band,
    rt_uint8_t number)
{
    struct rt_wlan_offload_channel *channels;
    rt_size_t count;
    rt_size_t index;

    if (band == RT_WLAN_OFFLOAD_BAND_2GHZ)
    {
        channels = context->channels_2ghz;
        count = BRCMF_2GHZ_CHANNEL_COUNT;
    }
    else if (band == RT_WLAN_OFFLOAD_BAND_5GHZ)
    {
        channels = context->channels_5ghz;
        count = BRCMF_5GHZ_CHANNEL_COUNT;
    }
    else
    {
        return RT_NULL;
    }
    for (index = 0; index < count; index++)
    {
        if (channels[index].number == number)
        {
            return &channels[index];
        }
    }
    return RT_NULL;
}

static rt_bool_t brcmf_decode_20mhz_chanspec(
    struct brcmf_context *context, rt_uint16_t chanspec,
    enum rt_wlan_offload_band_id *band, rt_uint8_t *channel)
{
    if (context->io_type == BRCMF_D11N_IO_TYPE)
    {
        if ((chanspec & BRCMF_D11N_CHSPEC_BW_MASK) != 0x0800U)
        {
            return RT_FALSE;
        }
        if ((chanspec & BRCMF_D11N_CHSPEC_BAND_MASK) ==
            BRCMF_D11N_CHSPEC_2G)
        {
            *band = RT_WLAN_OFFLOAD_BAND_2GHZ;
        }
        else if ((chanspec & BRCMF_D11N_CHSPEC_BAND_MASK) ==
                 BRCMF_D11N_CHSPEC_5G)
        {
            *band = RT_WLAN_OFFLOAD_BAND_5GHZ;
        }
        else
        {
            return RT_FALSE;
        }
    }
    else
    {
        if ((chanspec & BRCMF_D11AC_CHSPEC_BW_MASK) !=
            BRCMF_D11AC_CHSPEC_20)
        {
            return RT_FALSE;
        }
        *band = (chanspec & BRCMF_D11AC_CHSPEC_BAND_MASK) ==
                BRCMF_D11AC_CHSPEC_5G ? RT_WLAN_OFFLOAD_BAND_5GHZ :
                                       RT_WLAN_OFFLOAD_BAND_2GHZ;
    }
    *channel = (rt_uint8_t)chanspec;
    return *channel != 0U;
}

static rt_err_t brcmf_wifi_update_channels(struct brcmf_context *context)
{
    rt_uint8_t *buffer;
    rt_uint32_t count;
    rt_uint32_t enabled_2ghz = 0U;
    rt_uint32_t enabled_5ghz = 0U;
    rt_uint32_t index;
    rt_err_t result;

    brcmf_wifi_initialize_bands(context);
    context->radio.bands[RT_WLAN_OFFLOAD_BAND_2GHZ] =
        &context->band_2ghz;
    context->radio.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] =
        brcmf_chip_dual_band(context->chip.id) ? &context->band_5ghz : RT_NULL;
    buffer = rt_calloc(1, BRCMF_CHANSPEC_BUFFER_SIZE);
    if (!buffer)
    {
        return -RT_ENOMEM;
    }
    result = brcmf_proto_iovar(context, context->sta_interface, "chanspecs",
                               buffer, BRCMF_CHANSPEC_BUFFER_SIZE, RT_FALSE);
    if (result != RT_EOK)
    {
        rt_free(buffer);
        LOG_E("could not query firmware channels: %d", result);
        return result;
    }
    count = brcmf_get_le32(buffer);
    if (count > (BRCMF_CHANSPEC_BUFFER_SIZE - sizeof(rt_uint32_t)) /
                sizeof(rt_uint32_t))
    {
        rt_free(buffer);
        LOG_E("firmware returned invalid chanspec count %u", count);
        return -RT_EIO;
    }

    for (index = 0; index < count; index++)
    {
        rt_uint16_t chanspec = (rt_uint16_t)brcmf_get_le32(
            buffer + sizeof(rt_uint32_t) + index * sizeof(rt_uint32_t));
        enum rt_wlan_offload_band_id band;
        struct rt_wlan_offload_channel *entry;
        rt_uint8_t channel;

        if (!brcmf_decode_20mhz_chanspec(context, chanspec, &band, &channel) ||
            (band == RT_WLAN_OFFLOAD_BAND_5GHZ &&
             !context->radio.bands[RT_WLAN_OFFLOAD_BAND_5GHZ]))
        {
            continue;
        }
        entry = brcmf_find_channel(context, band, channel);
        if (!entry || !(entry->flags & RT_WLAN_OFFLOAD_CHANNEL_DISABLED))
        {
            continue;
        }
        entry->flags &= ~RT_WLAN_OFFLOAD_CHANNEL_DISABLED;
        if (band == RT_WLAN_OFFLOAD_BAND_2GHZ)
        {
            enabled_2ghz++;
        }
        else
        {
            enabled_5ghz++;
        }
    }
    rt_free(buffer);

    if (!enabled_2ghz)
    {
        LOG_E("firmware did not report any usable 2.4 GHz channel");
        return -RT_EIO;
    }
    for (index = 0; index < BRCMF_2GHZ_CHANNEL_COUNT +
                                BRCMF_5GHZ_CHANNEL_COUNT; index++)
    {
        struct rt_wlan_offload_channel *entry =
            index < BRCMF_2GHZ_CHANNEL_COUNT ?
            &context->channels_2ghz[index] :
            &context->channels_5ghz[index - BRCMF_2GHZ_CHANNEL_COUNT];
        rt_uint32_t information;

        if (entry->flags & RT_WLAN_OFFLOAD_CHANNEL_DISABLED)
        {
            continue;
        }
        information = brcmf_chanspec(context, (rt_uint8_t)entry->number);
        result = brcmf_proto_iovar_int(context, context->sta_interface,
                                       "per_chan_info", &information,
                                       RT_FALSE);
        if (result != RT_EOK)
        {
            continue;
        }
        if (information & BRCMF_WL_CHAN_RADAR)
        {
            entry->flags |= RT_WLAN_OFFLOAD_CHANNEL_RADAR |
                            RT_WLAN_OFFLOAD_CHANNEL_NO_IR;
        }
        if (information & (BRCMF_WL_CHAN_PASSIVE |
                           BRCMF_WL_CHAN_RESTRICTED))
        {
            entry->flags |= RT_WLAN_OFFLOAD_CHANNEL_NO_IR;
        }
    }
    if (!enabled_5ghz)
    {
        context->radio.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] = RT_NULL;
    }
    LOG_I("firmware channels: %u at 2.4 GHz, %u at 5 GHz (20 MHz)",
          enabled_2ghz, enabled_5ghz);
    return RT_EOK;
}

static rt_bool_t brcmf_capability_present(const char *capabilities,
                                          const char *wanted)
{
    rt_size_t wanted_length = rt_strlen(wanted);
    const char *current = capabilities;

    while (*current)
    {
        const char *start;
        rt_size_t length;

        while (*current == ' ')
        {
            current++;
        }
        start = current;
        while (*current && *current != ' ')
        {
            current++;
        }
        length = (rt_size_t)(current - start);
        if (length == wanted_length && !rt_memcmp(start, wanted, length))
        {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static void brcmf_wifi_update_features(
    struct brcmf_context *context, struct rt_wlan_offload_firmware_info *info)
{
    char capabilities[BRCMF_CAP_BUFFER_SIZE + 1U];
    rt_uint32_t sup_wpa = 0U;
    rt_uint32_t maxassoc = 0U;
    rt_bool_t firmware_handshake = RT_FALSE;
    rt_err_t result;

    context->radio.capabilities &= ~(RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD |
                                     RT_WLAN_OFFLOAD_CAP_SAE_OFFLOAD);
    context->sae_supported = RT_FALSE;
    rt_memset(capabilities, 0, sizeof(capabilities));
    result = brcmf_proto_iovar(context, context->sta_interface, "cap",
                               capabilities, BRCMF_CAP_BUFFER_SIZE, RT_FALSE);
    if (result == RT_EOK)
    {
        capabilities[BRCMF_CAP_BUFFER_SIZE] = '\0';
        firmware_handshake = brcmf_capability_present(capabilities, "idauth");
        context->sae_supported = brcmf_capability_present(capabilities, "sae");
    }
    else
    {
        LOG_W("could not query firmware capabilities: %d", result);
    }

    result = brcmf_proto_iovar_int(context, context->sta_interface,
                                   "sup_wpa", &sup_wpa, RT_FALSE);
    if (result == RT_EOK)
    {
        firmware_handshake = RT_TRUE;
    }
    if (firmware_handshake)
    {
        context->radio.capabilities |= RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD;
    }
    if (context->sae_supported)
    {
        context->radio.capabilities |= RT_WLAN_OFFLOAD_CAP_SAE_OFFLOAD |
                                       RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD;
    }

    result = brcmf_proto_iovar_int(context, context->sta_interface,
                                   "maxassoc", &maxassoc, RT_FALSE);
    if (result == RT_EOK && maxassoc <= 0xffffU)
    {
        info->max_stations = (rt_uint16_t)maxassoc;
    }
    info->features = context->radio.capabilities &
        (RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD | RT_WLAN_OFFLOAD_CAP_SAE_OFFLOAD);
}

static rt_err_t brcmf_load_clm(struct brcmf_context *context)
{
    struct brcmf_download_data *download;
    rt_uint8_t *blob;
    rt_size_t blob_length;
    rt_size_t offset = 0;
    rt_uint16_t flags = BRCMF_CLM_FLAG_BEGIN;
    rt_err_t result;

    if (!context->mapping->clm)
    {
        return RT_EOK;
    }
    result = brcmf_firmware_load(context->mapping->clm, &blob,
                                 &blob_length, RT_FALSE);
    if (result == -RT_ENOSYS)
    {
        LOG_W("CLM blob %s is not installed; channels may be limited",
              context->mapping->clm);
        return RT_EOK;
    }
    if (result != RT_EOK)
    {
        return result;
    }
    download = rt_malloc(sizeof(*download) + BRCMF_CLM_CHUNK_SIZE);
    if (!download)
    {
        rt_free(blob);
        return -RT_ENOMEM;
    }
    while (offset < blob_length && result == RT_EOK)
    {
        rt_size_t chunk = blob_length - offset;

        if (chunk > BRCMF_CLM_CHUNK_SIZE)
        {
            chunk = BRCMF_CLM_CHUNK_SIZE;
        }
        if (offset + chunk == blob_length)
        {
            flags |= BRCMF_CLM_FLAG_END;
        }
        brcmf_put_le16(&download->flags,
                       flags | BRCMF_CLM_HANDLER_VERSION);
        brcmf_put_le16(&download->type, BRCMF_CLM_DOWNLOAD_TYPE);
        brcmf_put_le32(&download->length, chunk);
        brcmf_put_le32(&download->crc, 0);
        rt_memcpy(download->data, blob + offset, chunk);
        result = brcmf_proto_iovar(
            context, context->sta_interface, "clmload", download,
            sizeof(*download) + chunk, RT_TRUE);
        flags &= ~BRCMF_CLM_FLAG_BEGIN;
        offset += chunk;
    }
    if (result != RT_EOK)
    {
        rt_uint32_t status = 0;

        (void)brcmf_proto_iovar_int(context, context->sta_interface,
                                    "clmload_status", &status, RT_FALSE);
        LOG_E("CLM download failed: %d (status %u)", result, status);
    }
    rt_free(download);
    rt_free(blob);
    return result;
}

static rt_bool_t brcmf_chip_dual_band(rt_uint32_t chip)
{
    switch (chip)
    {
    case 0x4339:
    case 0x4345:
    case 43454:
    case 0x4354:
    case 0x4356:
    case 0x4359:
    case 0x4373:
    case 43752:
    case 43012:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static rt_bool_t brcmf_scan_suite_oui(const rt_uint8_t suite[4],
                                       rt_bool_t rsn)
{
    return suite[0] == (rsn ? BRCMF_RSN_OUI0 : BRCMF_WPA_OUI0) &&
           suite[1] == (rsn ? BRCMF_RSN_OUI1 : BRCMF_WPA_OUI1) &&
           suite[2] == (rsn ? BRCMF_RSN_OUI2 : BRCMF_WPA_OUI2);
}

static rt_uint8_t brcmf_scan_cipher(const rt_uint8_t suite[4], rt_bool_t rsn)
{
    if (!brcmf_scan_suite_oui(suite, rsn))
    {
        return 0;
    }
    if (suite[3] == 2U)
    {
        return BRCMF_SCAN_CIPHER_TKIP;
    }
    if (suite[3] == 4U)
    {
        return BRCMF_SCAN_CIPHER_CCMP;
    }
    return 0;
}

static rt_wlan_security_t brcmf_scan_rsn_security(const rt_uint8_t *body,
                                                   rt_size_t length)
{
    rt_uint32_t akm = 0;
    rt_uint8_t group_cipher;
    rt_uint8_t pairwise_cipher = 0;
    rt_size_t position;
    rt_uint16_t count;
    rt_size_t index;

    if (!body || length < 8U || brcmf_get_le16(body) != 1U)
    {
        return SECURITY_UNKNOWN;
    }
    position = 2U;
    group_cipher = brcmf_scan_cipher(body + position, RT_TRUE);
    position += 4U;
    count = brcmf_get_le16(body + position);
    position += 2U;
    if (!count || count > (length - position) / 4U)
    {
        return SECURITY_UNKNOWN;
    }
    for (index = 0; index < count; index++, position += 4U)
    {
        pairwise_cipher |= brcmf_scan_cipher(body + position, RT_TRUE);
    }
    if (position + 2U > length)
    {
        return SECURITY_UNKNOWN;
    }
    count = brcmf_get_le16(body + position);
    position += 2U;
    if (!count || count > (length - position) / 4U)
    {
        return SECURITY_UNKNOWN;
    }
    for (index = 0; index < count; index++, position += 4U)
    {
        const rt_uint8_t *suite = body + position;

        if (!brcmf_scan_suite_oui(suite, RT_TRUE))
        {
            continue;
        }
        switch (suite[3])
        {
        case 1U:
            akm |= BRCMF_SCAN_AKM_8021X;
            break;
        case 2U:
            akm |= BRCMF_SCAN_AKM_PSK;
            break;
        case 3U:
            akm |= BRCMF_SCAN_AKM_FT_8021X;
            break;
        case 4U:
            akm |= BRCMF_SCAN_AKM_FT_PSK;
            break;
        case 5U:
            akm |= BRCMF_SCAN_AKM_8021X_SHA256;
            break;
        case 6U:
            akm |= BRCMF_SCAN_AKM_PSK_SHA256;
            break;
        case 8U:
            akm |= BRCMF_SCAN_AKM_SAE;
            break;
        case 9U:
            akm |= BRCMF_SCAN_AKM_FT_SAE;
            break;
        case 11U:
            akm |= BRCMF_SCAN_AKM_SUITE_B;
            break;
        case 12U:
            akm |= BRCMF_SCAN_AKM_SUITE_B_192;
            break;
        case 18U:
            akm |= BRCMF_SCAN_AKM_OWE;
            break;
        default:
            break;
        }
    }
    if (!(pairwise_cipher & (BRCMF_SCAN_CIPHER_TKIP |
                             BRCMF_SCAN_CIPHER_CCMP)) || !akm)
    {
        return SECURITY_UNKNOWN;
    }
    if (akm & BRCMF_SCAN_AKM_OWE)
    {
        return pairwise_cipher & BRCMF_SCAN_CIPHER_CCMP ? SECURITY_OWE :
                                                          SECURITY_UNKNOWN;
    }
    if ((akm & BRCMF_SCAN_AKM_SAE) &&
        (akm & (BRCMF_SCAN_AKM_PSK | BRCMF_SCAN_AKM_PSK_SHA256 |
                BRCMF_SCAN_AKM_FT_PSK)))
    {
        return pairwise_cipher & BRCMF_SCAN_CIPHER_CCMP ?
               SECURITY_WPA2_WPA3_MIXED_PSK : SECURITY_UNKNOWN;
    }
    if (akm & BRCMF_SCAN_AKM_SAE)
    {
        return pairwise_cipher & BRCMF_SCAN_CIPHER_CCMP ? SECURITY_WPA3_SAE :
                                                          SECURITY_UNKNOWN;
    }
    if (akm & BRCMF_SCAN_AKM_FT_SAE)
    {
        return pairwise_cipher & BRCMF_SCAN_CIPHER_CCMP ?
               SECURITY_FT_WPA3_SAE : SECURITY_UNKNOWN;
    }
    if (akm & BRCMF_SCAN_AKM_SUITE_B_192)
    {
        return SECURITY_WPA3_192BIT_8021X;
    }
    if (akm & BRCMF_SCAN_AKM_SUITE_B)
    {
        return SECURITY_WPA3_AES_8021X;
    }
    if ((akm & BRCMF_SCAN_AKM_FT_PSK) && !(akm & BRCMF_SCAN_AKM_PSK))
    {
        return pairwise_cipher & BRCMF_SCAN_CIPHER_CCMP ?
               SECURITY_FT_WPA2_AES_PSK : SECURITY_UNKNOWN;
    }
    if ((akm & BRCMF_SCAN_AKM_PSK_SHA256) &&
        !(akm & BRCMF_SCAN_AKM_PSK))
    {
        return pairwise_cipher & BRCMF_SCAN_CIPHER_CCMP ?
               SECURITY_WPA2_AES_PSK_SHA256 : SECURITY_UNKNOWN;
    }
    if (akm & BRCMF_SCAN_AKM_PSK)
    {
        if ((pairwise_cipher | group_cipher) ==
            (BRCMF_SCAN_CIPHER_TKIP | BRCMF_SCAN_CIPHER_CCMP))
        {
            return SECURITY_WPA2_MIXED_PSK;
        }
        return pairwise_cipher == BRCMF_SCAN_CIPHER_TKIP ?
               SECURITY_WPA2_TKIP_PSK : SECURITY_WPA2_AES_PSK;
    }
    if (akm & BRCMF_SCAN_AKM_FT_8021X)
    {
        return SECURITY_FT_WPA2_AES_8021X;
    }
    if (akm & BRCMF_SCAN_AKM_8021X_SHA256)
    {
        return SECURITY_WPA2_AES_8021X_SHA256;
    }
    if (akm & BRCMF_SCAN_AKM_8021X)
    {
        return pairwise_cipher == BRCMF_SCAN_CIPHER_TKIP ?
               SECURITY_WPA2_TKIP_8021X : SECURITY_WPA2_AES_8021X;
    }
    return SECURITY_UNKNOWN;
}

static rt_wlan_security_t brcmf_scan_wpa_security(const rt_uint8_t *body,
                                                   rt_size_t length)
{
    rt_uint8_t pairwise_cipher = 0;
    rt_bool_t psk = RT_FALSE;
    rt_bool_t enterprise = RT_FALSE;
    rt_size_t position = 4U;
    rt_uint16_t count;
    rt_size_t index;

    if (!body || length < 12U ||
        body[0] != BRCMF_WPA_OUI0 || body[1] != BRCMF_WPA_OUI1 ||
        body[2] != BRCMF_WPA_OUI2 || body[3] != BRCMF_WPA_OUI_TYPE ||
        brcmf_get_le16(body + position) != 1U)
    {
        return SECURITY_UNKNOWN;
    }
    position += 2U + 4U;
    if (position + 2U > length)
    {
        return SECURITY_UNKNOWN;
    }
    count = brcmf_get_le16(body + position);
    position += 2U;
    if (!count || count > (length - position) / 4U)
    {
        return SECURITY_UNKNOWN;
    }
    for (index = 0; index < count; index++, position += 4U)
    {
        pairwise_cipher |= brcmf_scan_cipher(body + position, RT_FALSE);
    }
    if (position + 2U > length)
    {
        return SECURITY_UNKNOWN;
    }
    count = brcmf_get_le16(body + position);
    position += 2U;
    if (!count || count > (length - position) / 4U)
    {
        return SECURITY_UNKNOWN;
    }
    for (index = 0; index < count; index++, position += 4U)
    {
        const rt_uint8_t *suite = body + position;

        if (!brcmf_scan_suite_oui(suite, RT_FALSE))
        {
            continue;
        }
        psk |= suite[3] == 2U;
        enterprise |= suite[3] == 1U;
    }
    if (!pairwise_cipher || (!psk && !enterprise))
    {
        return SECURITY_UNKNOWN;
    }
    if (psk)
    {
        return pairwise_cipher == BRCMF_SCAN_CIPHER_TKIP ?
               SECURITY_WPA_TKIP_PSK : SECURITY_WPA_AES_PSK;
    }
    return pairwise_cipher == BRCMF_SCAN_CIPHER_TKIP ?
           SECURITY_WPA_TKIP_8021X : SECURITY_WPA_AES_8021X;
}

static rt_wlan_security_t brcmf_scan_security(
    const rt_uint8_t *ies, rt_size_t length, rt_uint16_t capability)
{
    rt_wlan_security_t rsn_security = SECURITY_UNKNOWN;
    rt_wlan_security_t wpa_security = SECURITY_UNKNOWN;
    rt_size_t offset = 0;
    rt_bool_t rsn_seen = RT_FALSE;
    rt_bool_t wpa_seen = RT_FALSE;

    while (ies && offset + 2U <= length)
    {
        rt_uint8_t id = ies[offset];
        rt_uint8_t element_length = ies[offset + 1U];

        if (offset + 2U + element_length > length)
        {
            break;
        }
        if (id == BRCMF_IE_RSN && !rsn_seen)
        {
            rsn_seen = RT_TRUE;
            rsn_security = brcmf_scan_rsn_security(
                ies + offset + 2U, element_length);
        }
        else if (id == BRCMF_IE_VENDOR && element_length >= 4U &&
                 ies[offset + 2U] == BRCMF_WPA_OUI0 &&
                 ies[offset + 3U] == BRCMF_WPA_OUI1 &&
                 ies[offset + 4U] == BRCMF_WPA_OUI2 &&
                 ies[offset + 5U] == BRCMF_WPA_OUI_TYPE && !wpa_seen)
        {
            wpa_seen = RT_TRUE;
            wpa_security = brcmf_scan_wpa_security(
                ies + offset + 2U, element_length);
        }
        offset += 2U + element_length;
    }
    if (rsn_security != SECURITY_UNKNOWN &&
        wpa_security != SECURITY_UNKNOWN)
    {
        if (rsn_security & WPA3_SECURITY)
        {
            return rsn_security;
        }
        return (rsn_security & IEEE_8021X_ENABLED) ||
               (wpa_security & IEEE_8021X_ENABLED) ?
               SECURITY_WPA_WPA2_MIXED_8021X :
               SECURITY_WPA_WPA2_MIXED_PSK;
    }
    if (rsn_security != SECURITY_UNKNOWN)
    {
        return rsn_security;
    }
    if (wpa_security != SECURITY_UNKNOWN)
    {
        return wpa_security;
    }
    if (rsn_seen || wpa_seen)
    {
        return SECURITY_UNKNOWN;
    }
    return capability & BRCMF_DOT11_CAP_PRIVACY ? SECURITY_WEP_PSK :
                                                  SECURITY_OPEN;
}

static void brcmf_report_connect(struct brcmf_context *context,
                                 rt_err_t status)
{
    struct rt_wlan_offload_event event;

    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = context->connect_request;
    event.status = status;
    context->connect_request = 0;
    context->connect_started = 0;
    context->connect_assoc_seen = RT_FALSE;
    context->connect_psk_seen = RT_FALSE;
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void brcmf_finish_connect(struct brcmf_context *context,
                                 rt_err_t status)
{
    rt_err_t result;

    if (!context->connect_request)
    {
        return;
    }
    if (context->ap_resume_work_queued)
    {
        return;
    }
    if (!context->ap_suspended_for_connect)
    {
        brcmf_report_connect(context, status);
        return;
    }

    context->deferred_connect_status = status;
    context->ap_resume_work_queued = RT_TRUE;
    result = rt_work_submit(&context->ap_resume_work, 0);
    if (result != RT_EOK)
    {
        context->ap_resume_work_queued = RT_FALSE;
        LOG_E("could not defer AP resume after station join: %d", result);
    }
    /* Report station completion before AP restart commands can exceed the
     * WLAN management wait timeout. AP recovery continues independently. */
    brcmf_report_connect(context, status);
}

void brcmf_wifi_watchdog(struct brcmf_context *context)
{
    rt_err_t result;

    if (!context || !context->connect_request ||
        rt_tick_get() - context->connect_started <
            rt_tick_from_millisecond(BRCMFMAC_CONNECT_TIMEOUT_MS))
    {
        return;
    }

    LOG_E("connection event timeout for request %u",
          (unsigned int)context->connect_request);
    if (context->connect_cleanup_work_initialized &&
        !context->connect_cleanup_work_queued)
    {
        context->connect_cleanup_work_queued = RT_TRUE;
        result = rt_work_submit(&context->connect_cleanup_work, 0);
        if (result != RT_EOK)
        {
            context->connect_cleanup_work_queued = RT_FALSE;
            LOG_E("could not defer timed-out connection cleanup: %d", result);
        }
    }
    brcmf_finish_connect(context, -RT_ETIMEOUT);
}

static void brcmf_report_ap_started(struct brcmf_context *context,
                                    rt_err_t status)
{
    struct rt_wlan_offload_event event;

    if (!context->ap_request)
    {
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_AP_STARTED;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
    event.request_id = context->ap_request;
    event.status = status;
    rt_memcpy(event.data.network.bssid,
              context->radio.vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX].address,
              BRCMF_ETH_ALEN);
    context->ap_request = 0;
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void brcmf_report_scan_result(struct brcmf_context *context,
                                     const rt_uint8_t *data,
                                     rt_size_t length)
{
    const struct brcmf_escan_result *result =
        (const struct brcmf_escan_result *)data;
    const struct brcmf_bss_info *bss;
    struct rt_wlan_offload_event event;
    rt_uint32_t bss_length;
    rt_uint16_t ie_offset;
    rt_uint32_t ie_length;
    rt_uint8_t channel;

    if (length < sizeof(*result))
    {
        LOG_W("short escan result: %u", (unsigned int)length);
        return;
    }
    if (brcmf_get_le16(&result->bss_count) != 1U ||
        brcmf_get_le16(&result->sync_id) !=
            (context->scan_request & 0xffffU))
    {
        LOG_W("invalid escan result count=%u sync=%u expected=%u",
              (unsigned int)brcmf_get_le16(&result->bss_count),
              (unsigned int)brcmf_get_le16(&result->sync_id),
              context->scan_request & 0xffffU);
        return;
    }
    bss = &result->bss;
    bss_length = brcmf_get_le32(&bss->length);
    ie_offset = brcmf_get_le16(&bss->ie_offset);
    ie_length = brcmf_get_le32(&bss->ie_length);
    if (bss_length < sizeof(*bss) || bss_length > length -
        ((const rt_uint8_t *)bss - data) || ie_offset > bss_length ||
        ie_length > bss_length - ie_offset ||
        bss->ssid_length > BRCMF_SSID_MAX_LENGTH)
    {
        LOG_W("invalid BSS v%u len=%u avail=%u ie=%u+%u ssid=%u",
              brcmf_get_le32(&bss->version), bss_length,
              (unsigned int)(length - ((const rt_uint8_t *)bss - data)),
              (unsigned int)ie_offset, ie_length,
              (unsigned int)bss->ssid_length);
        return;
    }
    channel = bss->control_channel ? bss->control_channel :
                                     (brcmf_get_le16(&bss->chanspec) & 0xffU);
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_RESULT;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = context->scan_request;
    event.status = RT_EOK;
    event.data.network.ssid.len = bss->ssid_length;
    rt_memcpy(event.data.network.ssid.val, bss->ssid, bss->ssid_length);
    rt_memcpy(event.data.network.bssid, bss->bssid, BRCMF_ETH_ALEN);
    event.data.network.channel.primary_channel = channel;
    event.data.network.channel.width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
    if (channel <= 14U)
    {
        event.data.network.channel.band = RT_WLAN_OFFLOAD_BAND_2GHZ;
        event.data.network.channel.primary_frequency_mhz =
            channel == 14U ? 2484U : 2407U + 5U * channel;
    }
    else
    {
        event.data.network.channel.band = RT_WLAN_OFFLOAD_BAND_5GHZ;
        event.data.network.channel.primary_frequency_mhz = 5000U + 5U * channel;
    }
    event.data.network.channel.center_frequency1_mhz =
        event.data.network.channel.primary_frequency_mhz;
    event.data.network.rssi = (rt_int16_t)brcmf_get_le16(&bss->rssi);
    event.data.network.beacon_interval =
        brcmf_get_le16(&bss->beacon_period);
    event.data.network.capability = brcmf_get_le16(&bss->capability);
    event.data.network.ies = (const rt_uint8_t *)bss + ie_offset;
    event.data.network.ies_length = ie_length;
    event.data.network.security = brcmf_scan_security(
        event.data.network.ies, ie_length, event.data.network.capability);
    rt_wlan_offload_report_event(&context->radio, &event);
}

void brcmf_wifi_handle_event(struct brcmf_context *context,
                             const struct brcmf_event_packet *packet,
                             rt_size_t length)
{
    const struct brcmf_event_message *message = &packet->message;
    rt_uint32_t type = brcmf_get_be32(&message->event_type);
    rt_uint32_t status = brcmf_get_be32(&message->status);
    rt_uint32_t reason = brcmf_get_be32(&message->reason);
    rt_uint32_t data_length = brcmf_get_be32(&message->data_length);
    rt_uint16_t flags = brcmf_get_be16(&message->flags);
    rt_size_t fixed = (const rt_uint8_t *)packet->data -
                      (const rt_uint8_t *)packet;
    struct rt_wlan_offload_event event;

    if (length < fixed || data_length > length - fixed)
    {
        return;
    }
    switch (type)
    {
    case BRCMF_E_ESCAN_RESULT:
        if (status == BRCMF_E_STATUS_PARTIAL)
        {
            brcmf_report_scan_result(context, packet->data, data_length);
        }
        else if (context->scan_active)
        {
            rt_memset(&event, 0, sizeof(event));
            event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_DONE;
            event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
            event.request_id = context->scan_request;
            event.status = status == BRCMF_E_STATUS_SUCCESS ? RT_EOK :
                                                              -RT_ERROR;
            context->scan_active = RT_FALSE;
            rt_wlan_offload_report_event(&context->radio, &event);
        }
        break;
    case BRCMF_E_IF:
        if (data_length >= sizeof(struct brcmf_interface_event))
        {
            const struct brcmf_interface_event *interface_event =
                (const struct brcmf_interface_event *)packet->data;

            if (!(interface_event->flags & BRCMF_E_IF_FLAG_NOIF) &&
                interface_event->role == BRCMF_E_IF_ROLE_AP &&
                (interface_event->action == BRCMF_E_IF_ADD ||
                 interface_event->action == BRCMF_E_IF_CHANGE))
            {
                /* Publish the firmware-selected MAC before waking the AP
                 * creation waiter, which validates it against the STA MAC. */
                brcmf_update_ap_address(context, message->address);
                brcmf_update_ap_interface(
                    context, interface_event->interface_index,
                    interface_event->bsscfg_index);
            }
            else if (interface_event->role == BRCMF_E_IF_ROLE_AP &&
                     interface_event->action == BRCMF_E_IF_DEL)
            {
                context->ap_interface_created = RT_FALSE;
                if (context->ap_interface_pending)
                {
                    context->ap_interface_pending = RT_FALSE;
                    rt_completion_done(&context->ap_interface_completion);
                }
            }
        }
        break;
    case BRCMF_E_SET_SSID:
        if (brcmf_event_is_ap(context, message))
        {
            break;
        }
        if (context->connect_request &&
            brcmf_event_is_station(context, message))
        {
            if (status != BRCMF_E_STATUS_SUCCESS)
            {
                LOG_E("association failed: status %u reason %u auth %u "
                      "ifidx=%u bsscfg=%u",
                      status, reason,
                      brcmf_get_be32(&message->auth_type),
                      message->interface_index, message->bsscfg_index);
                brcmf_finish_connect(context, -RT_ERROR);
            }
            else
            {
                LOG_I("associated with %02x:%02x:%02x:%02x:%02x:%02x",
                      message->address[0], message->address[1],
                      message->address[2], message->address[3],
                      message->address[4], message->address[5]);
                context->connect_assoc_seen = RT_TRUE;
                if (!context->connect_secure || context->connect_psk_seen)
                {
                    brcmf_finish_connect(context, RT_EOK);
                }
            }
        }
        break;
    case BRCMF_E_PSK_SUP:
        if (context->connect_request &&
            brcmf_event_is_station(context, message))
        {
            if (status == BRCMF_E_STATUS_FWSUP_COMPLETED)
            {
                context->connect_psk_seen = RT_TRUE;
                if (context->connect_assoc_seen)
                {
                    brcmf_finish_connect(context, RT_EOK);
                }
            }
            else
            {
                LOG_E("firmware supplicant failed: status %u reason %u",
                      status, reason);
                brcmf_finish_connect(context, -RT_ERROR);
            }
        }
        break;
    case BRCMF_E_AUTH:
    case BRCMF_E_ASSOC:
    case BRCMF_E_REASSOC:
    case BRCMF_E_PRUNE:
        if (context->connect_request &&
            brcmf_event_is_station(context, message) &&
            status != BRCMF_E_STATUS_SUCCESS &&
            status != BRCMF_E_STATUS_UNSOLICITED &&
            status != BRCMF_E_STATUS_ATTEMPT)
        {
            LOG_W("connect event %u failed: status %u reason %u auth %u "
                  "ifidx=%u bsscfg=%u",
                  type, status, reason,
                  brcmf_get_be32(&message->auth_type),
                  message->interface_index, message->bsscfg_index);
        }
        break;
    case BRCMF_E_LINK:
        if ((flags & BRCMF_EVENT_MSG_LINK) &&
            status != BRCMF_E_STATUS_NO_NETWORKS)
        {
            break;
        }
        if (!brcmf_event_is_station(context, message))
        {
            break;
        }
        if (context->connect_request)
        {
            LOG_E("connection failed by LINK event: status %u reason %u",
                  status, reason);
            brcmf_finish_connect(context, -RT_ERROR);
            break;
        }
        rt_memset(&event, 0, sizeof(event));
        event.type = RT_WLAN_OFFLOAD_EVENT_DISCONNECTED;
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
        event.status = RT_EOK;
        event.data.disconnected.reason = 0;
        rt_memcpy(event.data.disconnected.bssid, message->address,
                  BRCMF_ETH_ALEN);
        rt_wlan_offload_report_event(&context->radio, &event);
        break;
    case BRCMF_E_DEAUTH:
    case BRCMF_E_DEAUTH_IND:
    case BRCMF_E_DISASSOC_IND:
        rt_memset(&event, 0, sizeof(event));
        if (brcmf_event_is_ap(context, message))
        {
            if (!brcmf_valid_station_address(message->address) ||
                (type == BRCMF_E_DEAUTH &&
                 reason == BRCMF_REASON_CLASS3_NONASSOC))
            {
                break;
            }
            LOG_W("AP station %02x:%02x:%02x:%02x:%02x:%02x left "
                  "event=%u status=%u reason=%u",
                  message->address[0], message->address[1],
                  message->address[2], message->address[3],
                  message->address[4], message->address[5],
                  type, status, reason);
            event.type = RT_WLAN_OFFLOAD_EVENT_DEL_STATION;
            event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
            rt_memcpy(event.data.station.mac, message->address,
                      BRCMF_ETH_ALEN);
            rt_wlan_offload_report_event(&context->radio, &event);
            break;
        }
        if (!brcmf_event_is_station(context, message))
        {
            break;
        }
        if (context->connect_request)
        {
            LOG_E("connection terminated by event %u: status %u reason %u",
                  type, status, reason);
            brcmf_finish_connect(context, -RT_ERROR);
            break;
        }
        event.type = RT_WLAN_OFFLOAD_EVENT_DISCONNECTED;
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
        event.status = RT_EOK;
        event.data.disconnected.reason = reason;
        event.data.disconnected.locally_generated = type == BRCMF_E_DEAUTH;
        rt_memcpy(event.data.disconnected.bssid, message->address,
                  BRCMF_ETH_ALEN);
        rt_wlan_offload_report_event(&context->radio, &event);
        break;
    case BRCMF_E_AP_STARTED:
        brcmf_update_ap_interface(context, message->interface_index,
                                  message->bsscfg_index);
        brcmf_report_ap_started(
            context,
            status == BRCMF_E_STATUS_SUCCESS ? RT_EOK : -RT_ERROR);
        break;
    case BRCMF_E_ASSOC_IND:
    case BRCMF_E_REASSOC_IND:
        brcmf_update_ap_interface(context, message->interface_index,
                                  message->bsscfg_index);
        if (reason != BRCMF_E_STATUS_SUCCESS)
        {
            LOG_W("AP association rejected for "
                  "%02x:%02x:%02x:%02x:%02x:%02x: "
                  "event=%u status=%u reason=%u",
                  message->address[0], message->address[1],
                  message->address[2], message->address[3],
                  message->address[4], message->address[5],
                  type, status, reason);
            break;
        }
        rt_memset(&event, 0, sizeof(event));
        event.type = RT_WLAN_OFFLOAD_EVENT_NEW_STATION;
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
        rt_memcpy(event.data.station.mac, message->address, BRCMF_ETH_ALEN);
        rt_wlan_offload_report_event(&context->radio, &event);
        break;
    default:
        break;
    }
}

static rt_err_t brcmf_wifi_start(struct rt_wlan_offload_radio *radio)
{
    struct brcmf_context *context = rt_wlan_offload_get_driver_data(radio);
    struct rt_wlan_offload_firmware_info info;
    rt_uint8_t io_type[4] = {0};
    rt_uint32_t value;
    rt_err_t tuning_result;
    rt_err_t result;

    brcmf_reset_ap_interface(context);
    result = brcmf_proto_start(context);

    if (result == RT_EOK)
    {
        result = brcmf_wifi_prepare_mac(context);
    }
    if (result == RT_EOK)
    {
        result = brcmf_load_clm(context);
    }
    if (result == RT_EOK)
    {
        result = brcmf_wifi_program_country(
            context, BRCMFMAC_COUNTRY_CODE, BRCMFMAC_COUNTRY_REVISION,
            brcmf_country_from_alpha2(BRCMFMAC_COUNTRY_CODE));
    }
    if (result == RT_EOK)
    {
        result = brcmf_proto_command(context, context->sta_interface,
                                     BRCMF_C_GET_VERSION, io_type,
                                     sizeof(io_type), RT_FALSE);
    }
    if (result == RT_EOK)
    {
        context->io_type = brcmf_get_le32(io_type);
        if (context->io_type != 1U && context->io_type != 2U)
        {
            result = -RT_EIO;
        }
    }
    if (result == RT_EOK)
    {
        result = brcmf_command_integer(context, context->sta_interface,
                                       BRCMF_C_UP, 0U);
    }
    if (result == RT_EOK)
    {
        /* BCM43430A1 exposes one RF path. The legacy board driver
         * selected antenna 0 instead of leaving diversity state to firmware. */
        tuning_result = brcmf_command_integer(
            context, context->sta_interface, BRCMF_C_SET_ANTDIV, 0U);
        if (tuning_result != RT_EOK)
        {
            LOG_W("could not select BCM43430A1 antenna 0: %d", tuning_result);
        }
    }
    if (result == RT_EOK)
    {
        /* Linux enables frameburst once for the primary firmware
         * interface, before either STA or AP mode is configured. */
        result = brcmf_command_integer(context, context->sta_interface,
                                       BRCMF_C_SET_FAKEFRAG, 1U);
    }
    if (result == RT_EOK)
    {
        result = brcmf_wifi_update_channels(context);
    }
    if (result != RT_EOK)
    {
        return result;
    }
    rt_memcpy(radio->permanent_address, context->mac, BRCMF_ETH_ALEN);
    rt_memcpy(radio->vifs[RT_WLAN_OFFLOAD_VIF_STA_INDEX].address,
              context->mac, BRCMF_ETH_ALEN);
    brcmf_derive_ap_address(
        radio->vifs[RT_WLAN_OFFLOAD_VIF_STA_INDEX].address,
        radio->vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX].address);
    rt_memset(&info, 0, sizeof(info));
    info.protocol_version = 4;
    info.max_vifs = 2;
    info.max_channel_contexts = 1;
    brcmf_wifi_update_features(context, &info);
    return rt_wlan_offload_update_firmware_info(radio, &info);
}

static rt_err_t brcmf_wifi_stop(struct rt_wlan_offload_radio *radio)
{
    struct brcmf_context *context = rt_wlan_offload_get_driver_data(radio);

    if (context->ap_resume_work_initialized)
    {
        (void)rt_work_cancel_sync(&context->ap_resume_work);
        context->ap_resume_work_queued = RT_FALSE;
        context->ap_suspended_for_connect = RT_FALSE;
    }
    if (context->connect_cleanup_work_initialized)
    {
        (void)rt_work_cancel_sync(&context->connect_cleanup_work);
        context->connect_cleanup_work_queued = RT_FALSE;
    }
    context->scan_active = RT_FALSE;
    context->connect_request = 0;
    context->connect_started = 0;
    context->ap_request = 0;
    brcmf_reset_ap_interface(context);
    return rt_wlan_offload_set_radio_online(radio, RT_FALSE);
}

static rt_err_t brcmf_wifi_start_ap(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_ap_settings *settings);
static rt_err_t brcmf_wifi_start_ap_apsta(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_ap_settings *settings);
static rt_err_t brcmf_wifi_suspend_ap_for_connect(
    struct brcmf_context *context,
    const struct rt_wlan_offload_channel_definition *channel);
static rt_err_t brcmf_wifi_resume_suspended_ap(
    struct brcmf_context *context, rt_err_t connect_status);
static void brcmf_wifi_resume_ap_work(struct rt_work *work,
                                      void *work_data);
static rt_err_t brcmf_wifi_get_channel(
    struct rt_wlan_offload_vif *vif,
    struct rt_wlan_offload_channel_definition *channel);

static rt_err_t brcmf_wifi_set_station_mode(struct brcmf_context *context)
{
    rt_uint32_t value = 1U;
    rt_err_t result;

    if (context->ap_interface_created)
    {
        /* A virtual AP already implies that the primary BSS is in STA mode.
         * Do not take the radio down again when scan/connect reselects STA. */
        return RT_EOK;
    }
    result = brcmf_command_integer(context, context->sta_interface,
                                   BRCMF_C_DOWN, 1U);
    if (result == RT_EOK)
    {
        result = brcmf_proto_iovar_int(
            context, context->sta_interface, "apsta", &value, RT_TRUE);
    }
    if (result == RT_EOK)
    {
        result = brcmf_command_integer(context, context->sta_interface,
                                       BRCMF_C_UP, 1U);
    }
    if (result == RT_EOK)
    {
        result = brcmf_command_integer(context, context->sta_interface,
                                       BRCMF_C_SET_INFRA, 1U);
    }
    return result;
}

static rt_err_t brcmf_wifi_change_interface(
    struct rt_wlan_offload_vif *vif, enum rt_wlan_offload_iftype type,
    rt_bool_t enabled)
{
    struct brcmf_context *context = brcmf_vif_context(vif);

    if (type == RT_WLAN_OFFLOAD_IFTYPE_STATION)
    {
        if (!enabled)
        {
            return RT_EOK;
        }
        return brcmf_wifi_set_station_mode(context);
    }
    if (type == RT_WLAN_OFFLOAD_IFTYPE_AP)
    {
        struct rt_wlan_offload_ap_settings settings;

        if (!enabled)
        {
            if (!context->ap_interface_created)
            {
                return RT_EOK;
            }
            return brcmf_set_bss(context, context->ap_bsscfg, RT_FALSE);
        }
        if (!context->ap_settings_valid ||
            context->ap_settings_generation ==
                context->radio.firmware_generation)
        {
            return RT_EOK;
        }

        /* Firmware recovery recreates interface mode first. Replay the AP
         * configuration here so the AP_STARTED event also restarts DHCP/NAT. */
        settings = context->ap_settings;
        settings.request_id = rt_wlan_offload_alloc_request_id(&context->radio);
        if (!settings.request_id)
        {
            return -RT_EIO;
        }
        return brcmf_wifi_start_ap(vif, &settings);
    }
    return -RT_ENOSYS;
}

static rt_err_t brcmf_wifi_scan(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_scan_request *request)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    struct brcmf_escan_params *params;
    rt_size_t params_length;
    rt_size_t index;
    rt_err_t result;

    if (context->scan_active ||
        request->ssid_count > 1U)
    {
        return -RT_EBUSY;
    }
    if (request->channel_count > BRCMF_MAX_SCAN_CHANNELS)
    {
        return -RT_EINVAL;
    }
    params_length = sizeof(*params) - sizeof(params->params.channels) +
                    request->channel_count * sizeof(params->params.channels[0]);
    params_length = RT_ALIGN(params_length, 4U);
    params = rt_calloc(1, params_length);
    if (!params)
    {
        return -RT_ENOMEM;
    }
    brcmf_put_le32(&params->version, BRCMF_ESCAN_VERSION);
    brcmf_put_le16(&params->action, BRCMF_ESCAN_ACTION_START);
    brcmf_put_le16(&params->sync_id, request->request_id & 0xffffU);
    rt_memset(params->params.bssid, 0xff, BRCMF_ETH_ALEN);
    params->params.bss_type = BRCMF_BSS_TYPE_ANY;
    params->params.scan_type = request->flags & RT_WLAN_OFFLOAD_SCAN_PASSIVE ?
                               1U : 0U;
    brcmf_put_le32(&params->params.probes, 0xffffffffU);
    brcmf_put_le32(&params->params.active_time,
                   request->duration_ms ? request->duration_ms : 0xffffffffU);
    brcmf_put_le32(&params->params.passive_time,
                   request->duration_ms ? request->duration_ms : 0xffffffffU);
    brcmf_put_le32(&params->params.home_time, 0xffffffffU);
    if (request->ssid_count)
    {
        brcmf_put_le32(&params->params.ssid.length,
                       request->ssids[0].length);
        rt_memcpy(params->params.ssid.value, request->ssids[0].value,
                  request->ssids[0].length);
    }
    brcmf_put_le32(&params->params.channel_count, request->channel_count);
    for (index = 0; index < request->channel_count; index++)
    {
        brcmf_put_le16(
            &params->params.channels[index],
            brcmf_chanspec(context, request->channels[index].primary_channel));
    }
    for (index = 0; index < BRCMF_ETH_ALEN; index++)
    {
        if (request->bssid[index])
        {
            rt_memcpy(params->params.bssid, request->bssid, BRCMF_ETH_ALEN);
            break;
        }
    }
    context->scan_request = request->request_id;
    context->scan_active = RT_TRUE;
    result = brcmf_proto_iovar(context, context->sta_interface, "escan",
                               params, params_length, RT_TRUE);
    rt_free(params);
    if (result != RT_EOK)
    {
        context->scan_active = RT_FALSE;
        LOG_E("escan request failed: %d", result);
    }
    return result;
}

static rt_err_t brcmf_wifi_abort_scan(struct rt_wlan_offload_vif *vif,
                                      rt_uint32_t request_id)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    rt_uint8_t buffer[RT_ALIGN(sizeof(struct brcmf_escan_params), 4U)];
    struct brcmf_escan_params *params =
        (struct brcmf_escan_params *)buffer;

    if (!context->scan_active || context->scan_request != request_id)
    {
        return -RT_EINVAL;
    }
    rt_memset(buffer, 0, sizeof(buffer));
    brcmf_put_le32(&params->version, BRCMF_ESCAN_VERSION);
    brcmf_put_le16(&params->action, BRCMF_ESCAN_ACTION_ABORT);
    brcmf_put_le16(&params->sync_id, request_id & 0xffffU);
    brcmf_put_le32(&params->params.channel_count, 1U);
    brcmf_put_le16(params->params.channels, 0xffffU);
    {
        struct rt_wlan_offload_event event;
        rt_err_t result = brcmf_proto_iovar(
            context, context->sta_interface, "escan", params, sizeof(buffer),
            RT_TRUE);

        if (result != RT_EOK)
        {
            return result;
        }
        context->scan_active = RT_FALSE;
        context->scan_request = 0U;
        rt_memset(&event, 0, sizeof(event));
        event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_DONE;
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
        event.request_id = request_id;
        event.status = -RT_EINTR;
        rt_wlan_offload_report_event(&context->radio, &event);
    }
    return RT_EOK;
}

static rt_err_t brcmf_wifi_security(struct brcmf_context *context,
                                    rt_uint8_t interface_index,
                                    rt_uint32_t bsscfg,
                                    rt_wlan_security_t security,
                                    const rt_wlan_key_t *key,
                                    rt_bool_t station)
{
    rt_uint32_t auth = 0;
    rt_uint32_t wsec = 0;
    rt_uint32_t wpa_auth = BRCMF_WPA_AUTH_DISABLED;
    rt_uint32_t sup_wpa = 0;
    rt_bool_t legacy_psk = RT_FALSE;
    rt_bool_t sae = RT_FALSE;
    rt_uint8_t target_interface;
    struct brcmf_wsec_sae_password sae_password;
    struct brcmf_wsec_pmk pmk;
    rt_err_t result;

    switch (security)
    {
    case SECURITY_OPEN:
        break;
    case SECURITY_WPA_TKIP_PSK:
        wsec = BRCMF_WSEC_TKIP;
        wpa_auth = BRCMF_WPA_AUTH_PSK;
        sup_wpa = 1;
        legacy_psk = RT_TRUE;
        break;
    case SECURITY_WPA_AES_PSK:
        wsec = BRCMF_WSEC_AES;
        wpa_auth = BRCMF_WPA_AUTH_PSK;
        sup_wpa = 1;
        legacy_psk = RT_TRUE;
        break;
    case SECURITY_WPA2_TKIP_PSK:
        wsec = BRCMF_WSEC_TKIP;
        wpa_auth = BRCMF_WPA2_AUTH_PSK;
        sup_wpa = 1;
        legacy_psk = RT_TRUE;
        break;
    case SECURITY_WPA2_AES_PSK:
    case SECURITY_WPA2_AES_CMAC:
        wsec = BRCMF_WSEC_AES;
        wpa_auth = BRCMF_WPA2_AUTH_PSK;
        sup_wpa = 1;
        legacy_psk = RT_TRUE;
        break;
    case SECURITY_WPA2_MIXED_PSK:
        wsec = BRCMF_WSEC_TKIP | BRCMF_WSEC_AES;
        wpa_auth = BRCMF_WPA2_AUTH_PSK;
        sup_wpa = 1;
        legacy_psk = RT_TRUE;
        break;
    case SECURITY_WPA_WPA2_MIXED_PSK:
        wsec = BRCMF_WSEC_TKIP | BRCMF_WSEC_AES;
        wpa_auth = BRCMF_WPA_AUTH_PSK | BRCMF_WPA2_AUTH_PSK;
        sup_wpa = 1;
        legacy_psk = RT_TRUE;
        break;
    case SECURITY_WPA2_AES_PSK_SHA256:
        wsec = BRCMF_WSEC_AES;
        wpa_auth = BRCMF_WPA2_AUTH_PSK_SHA256;
        sup_wpa = 1;
        legacy_psk = RT_TRUE;
        break;
    case SECURITY_WPA3_SAE:
        wsec = BRCMF_WSEC_AES;
        wpa_auth = BRCMF_WPA3_AUTH_SAE_PSK;
        sup_wpa = 1;
        sae = RT_TRUE;
        break;
    case SECURITY_WPA2_WPA3_MIXED_PSK:
        wsec = BRCMF_WSEC_AES;
        wpa_auth = BRCMF_WPA2_AUTH_PSK;
        sup_wpa = 1;
        legacy_psk = RT_TRUE;
        if (context->sae_supported)
        {
            wpa_auth |= BRCMF_WPA3_AUTH_SAE_PSK;
            sae = RT_TRUE;
        }
        break;
    default:
        return -RT_ENOSYS;
    }

    if (sae && !context->sae_supported)
    {
        return -RT_ENOSYS;
    }
    if ((legacy_psk && (!key || key->len < 8U ||
                        key->len > sizeof(pmk.key))) ||
        (sae && (!key || !key->len ||
                 key->len > sizeof(sae_password.key))))
    {
        return -RT_EINVAL;
    }

    result = brcmf_proto_bsscfg_iovar(context, interface_index, bsscfg,
                                       "auth", &auth, sizeof(auth), RT_TRUE);
    if (result == RT_EOK)
    {
        result = brcmf_proto_bsscfg_iovar(context, interface_index, bsscfg,
                                           "wsec", &wsec, sizeof(wsec),
                                           RT_TRUE);
    }
    if (result == RT_EOK)
    {
        result = brcmf_proto_bsscfg_iovar(context, interface_index, bsscfg,
                                           "wpa_auth", &wpa_auth,
                                           sizeof(wpa_auth), RT_TRUE);
    }
    if (result == RT_EOK && station)
    {
        result = brcmf_proto_bsscfg_iovar(context, interface_index, bsscfg,
                                           "sup_wpa", &sup_wpa,
                                           sizeof(sup_wpa), RT_TRUE);
    }

    target_interface = station ? interface_index : context->ap_interface;
    if (result == RT_EOK && sae)
    {
        rt_memset(&sae_password, 0, sizeof(sae_password));
        brcmf_put_le16(&sae_password.key_length, key->len);
        rt_memcpy(sae_password.key, key->val, key->len);
        result = brcmf_proto_iovar(context, target_interface,
                                   "sae_password", &sae_password,
                                   sizeof(sae_password), RT_TRUE);
    }
    if (result == RT_EOK && legacy_psk)
    {
        rt_memset(&pmk, 0, sizeof(pmk));
        brcmf_put_le16(&pmk.key_length, key->len);
        brcmf_put_le16(&pmk.flags, BRCMF_WSEC_PASSPHRASE);
        rt_memcpy(pmk.key, key->val, key->len);
        if (!station && context->chip.id == 43430U)
        {
            /* The BCM43430 AP firmware needs time to apply wpa_auth. */
            rt_thread_mdelay(2);
        }
        result = brcmf_proto_command(context, target_interface,
                                     BRCMF_C_SET_WSEC_PMK, &pmk,
                                     sizeof(pmk), RT_TRUE);
    }
    return result;
}

static rt_err_t brcmf_wifi_connect(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_connect_request *request)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    struct brcmf_ext_join_params ext_join;
    struct brcmf_join_params join;
    rt_size_t ext_join_length;
    rt_size_t join_length;
    rt_err_t power_result;
    rt_err_t result;

    if (!request->ssid.len || request->ssid.len > BRCMF_SSID_MAX_LENGTH)
    {
        return -RT_EINVAL;
    }
    if (context->ap_resume_work_queued ||
        context->connect_cleanup_work_queued ||
        context->ap_suspended_for_connect)
    {
        return -RT_EBUSY;
    }
    /* Remove an existing AP interface before reinitializing the primary BSS.
     * This makes AP-first connection use the reliable station-first firmware
     * sequence; the AP is recreated after the join completes. */
    result = brcmf_wifi_suspend_ap_for_connect(context, &request->channel);
    if (result != RT_EOK)
    {
        return result;
    }
    result = brcmf_wifi_set_station_mode(context);
    if (result != RT_EOK)
    {
        power_result = brcmf_wifi_resume_suspended_ap(
            context, -RT_ERROR);
        if (power_result != RT_EOK)
        {
            LOG_E("could not restore AP after station mode failure: %d",
                  power_result);
        }
        return result;
    }
    if (result == RT_EOK)
    {
        power_result = brcmf_command_integer(
            context, context->sta_interface, BRCMF_C_SET_PM,
            context->station_power_save ? BRCMF_PM_FAST : BRCMF_PM_OFF);
        if (power_result != RT_EOK)
        {
            LOG_W("could not apply station power-save mode: %d", power_result);
        }
    }
    if (result == RT_EOK)
    {
        result = brcmf_command_integer(context, context->sta_interface,
                                       BRCMF_C_SET_AUTH, 0);
    }
    if (result == RT_EOK)
    {
        result = brcmf_wifi_security(context, context->sta_interface, 0,
                                     request->security, &request->key,
                                     RT_TRUE);
    }
    if (result != RT_EOK)
    {
        power_result = brcmf_wifi_resume_suspended_ap(
            context, -RT_ERROR);
        if (power_result != RT_EOK)
        {
            LOG_E("could not restore AP after station setup failure: %d",
                  power_result);
        }
        return result;
    }
    rt_memset(&ext_join, 0, sizeof(ext_join));
    brcmf_put_le32(&ext_join.ssid.length, request->ssid.len);
    rt_memcpy(ext_join.ssid.value, request->ssid.val, request->ssid.len);
    ext_join.scan.scan_type = 0xffU;
    brcmf_put_le32(&ext_join.scan.probes, 0xffffffffU);
    brcmf_put_le32(&ext_join.scan.active_time, 0xffffffffU);
    brcmf_put_le32(&ext_join.scan.passive_time, 0xffffffffU);
    brcmf_put_le32(&ext_join.scan.home_time, 0xffffffffU);
    rt_memset(ext_join.assoc.bssid, 0xff, BRCMF_ETH_ALEN);
    if (request->bssid[0] || request->bssid[1] || request->bssid[2] ||
        request->bssid[3] || request->bssid[4] || request->bssid[5])
    {
        rt_memcpy(ext_join.assoc.bssid, request->bssid, BRCMF_ETH_ALEN);
    }
    ext_join_length = offsetof(struct brcmf_ext_join_params, assoc) +
                      offsetof(struct brcmf_assoc_params, chanspec);
    if (request->channel.primary_channel)
    {
        brcmf_put_le32(&ext_join.scan.probes,
                       BRCMF_JOIN_ACTIVE_TIME_MS /
                       BRCMF_JOIN_PROBE_INTERVAL_MS);
        brcmf_put_le32(&ext_join.scan.active_time,
                       BRCMF_JOIN_ACTIVE_TIME_MS);
        brcmf_put_le32(&ext_join.scan.passive_time,
                       BRCMF_JOIN_PASSIVE_TIME_MS);
        brcmf_put_le32(&ext_join.assoc.chanspec_count, 1U);
        brcmf_put_le16(ext_join.assoc.chanspec,
                       brcmf_chanspec(
                           context, request->channel.primary_channel));
        ext_join_length += sizeof(ext_join.assoc.chanspec[0]);
    }
    context->connect_request = request->request_id;
    context->connect_started = rt_tick_get();
    context->connect_secure = request->security != SECURITY_OPEN;
    context->connect_assoc_seen = RT_FALSE;
    context->connect_psk_seen = RT_FALSE;
    result = brcmf_proto_bsscfg_iovar(
        context, context->sta_interface, 0, "join", &ext_join,
        ext_join_length, RT_TRUE);
    if (result != RT_EOK)
    {
        LOG_W("join iovar failed (%d), falling back to SET_SSID", result);
        rt_memset(&join, 0, sizeof(join));
        rt_memcpy(&join.ssid, &ext_join.ssid, sizeof(join.ssid));
        rt_memcpy(&join.assoc, &ext_join.assoc, sizeof(join.assoc));
        join_length = sizeof(join.ssid);
        if (request->channel.primary_channel)
        {
            join_length += sizeof(join.assoc);
        }
        result = brcmf_proto_command(context, context->sta_interface,
                                     BRCMF_C_SET_SSID, &join, join_length,
                                     RT_TRUE);
    }
    if (result != RT_EOK)
    {
        context->connect_request = 0;
        context->connect_started = 0;
        power_result = brcmf_wifi_resume_suspended_ap(
            context, -RT_ERROR);
        if (power_result != RT_EOK)
        {
            LOG_E("could not restore AP after join command failure: %d",
                  power_result);
        }
    }
    return result;
}

static rt_err_t brcmf_wifi_disconnect(struct rt_wlan_offload_vif *vif,
                                      rt_uint32_t request_id,
                                      rt_uint16_t reason)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    struct brcmf_scb_value scb;

    (void)request_id;
    rt_memset(&scb, 0, sizeof(scb));
    brcmf_put_le32(&scb.value, reason);
    rt_memset(scb.address, 0xff, BRCMF_ETH_ALEN);
    return brcmf_proto_command(context, context->sta_interface,
                               BRCMF_C_DISASSOC, &scb, sizeof(scb), RT_TRUE);
}

static void brcmf_wifi_connect_cleanup_work(struct rt_work *work,
                                              void *work_data)
{
    struct brcmf_context *context = work_data;
    struct rt_wlan_offload_vif *station;
    rt_err_t result;

    (void)work;
    if (!context->tearing_down && context->firmware_running)
    {
        station = &context->radio.vifs[RT_WLAN_OFFLOAD_VIF_STA_INDEX];
        result = brcmf_wifi_disconnect(station, 0,
                                       BRCMF_REASON_DEAUTH_LEAVING);
        if (result != RT_EOK)
        {
            LOG_W("could not disassociate timed-out connection: %d", result);
        }
    }
    context->connect_cleanup_work_queued = RT_FALSE;
}

static rt_bool_t brcmf_wifi_skip_vendor_ie(const rt_uint8_t *ie)
{
    return ie[1] >= 4U && ie[2] == 0x00U && ie[3] == 0x50U &&
           ie[4] == 0xf2U && (ie[5] == 1U || ie[5] == 2U);
}

static rt_err_t brcmf_wifi_program_vendor_ies(
    struct brcmf_context *context, rt_uint8_t interface_index,
    rt_uint32_t bsscfg, const rt_uint8_t *ies, rt_size_t length,
    rt_bool_t add)
{
    rt_size_t offset = 0U;

    while (offset < length)
    {
        const rt_uint8_t *ie = ies + offset;
        rt_size_t ie_length;
        rt_uint8_t *command;
        rt_err_t result;

        if (length - offset < 2U || ie[1] > length - offset - 2U)
        {
            return -RT_EINVAL;
        }
        ie_length = (rt_size_t)ie[1] + 2U;
        offset += ie_length;
        if (ie[0] != 221U || ie[1] < 4U || brcmf_wifi_skip_vendor_ie(ie))
        {
            continue;
        }

        command = rt_calloc(1U, BRCMF_VNDR_IE_HEADER_SIZE + ie_length);
        if (!command)
        {
            return -RT_ENOMEM;
        }
        rt_memcpy(command, add ? "add" : "del", 4U);
        brcmf_put_le32(command + 4U, 1U);
        brcmf_put_le32(command + 8U, BRCMF_VNDR_IE_BEACON_FLAG);
        rt_memcpy(command + BRCMF_VNDR_IE_HEADER_SIZE, ie, ie_length);
        result = brcmf_proto_bsscfg_iovar(
            context, interface_index, bsscfg, "vndr_ie", command,
            BRCMF_VNDR_IE_HEADER_SIZE + ie_length, RT_TRUE);
        rt_free(command);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    return RT_EOK;
}

static rt_err_t brcmf_wifi_apply_ap_options(
    struct brcmf_context *context,
    const struct rt_wlan_offload_ap_settings *settings)
{
    rt_uint32_t maxassoc = settings->max_stations ?
        settings->max_stations : context->radio.firmware_info.max_stations;
    rt_err_t result;

    if (maxassoc)
    {
        result = brcmf_proto_iovar_int(context, context->ap_interface,
                                       "maxassoc", &maxassoc, RT_TRUE);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    if (context->ap_settings_valid &&
        context->ap_settings_generation == context->radio.firmware_generation &&
        context->ap_settings.beacon_ies_length)
    {
        result = brcmf_wifi_program_vendor_ies(
            context, context->ap_interface, context->ap_bsscfg,
            context->ap_settings.beacon_ies,
            context->ap_settings.beacon_ies_length, RT_FALSE);
        if (result != RT_EOK)
        {
            LOG_W("could not remove old AP vendor IEs: %d", result);
        }
    }
    if (!settings->beacon_ies_length)
    {
        return RT_EOK;
    }
    return brcmf_wifi_program_vendor_ies(
        context, context->ap_interface, context->ap_bsscfg,
        settings->beacon_ies, settings->beacon_ies_length, RT_TRUE);
}


static void brcmf_wifi_save_ap_settings(
    struct brcmf_context *context,
    const struct rt_wlan_offload_ap_settings *settings)
{
    context->ap_settings = *settings;
    if (settings->beacon_ies_length)
    {
        rt_memmove(context->ap_beacon_ies, settings->beacon_ies,
                   settings->beacon_ies_length);
        context->ap_settings.beacon_ies = context->ap_beacon_ies;
    }
    else
    {
        context->ap_settings.beacon_ies = RT_NULL;
    }
    context->ap_settings_generation = context->radio.firmware_generation;
    context->ap_settings_valid = RT_TRUE;
}

static rt_err_t brcmf_wifi_start_ap_apsta(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_ap_settings *settings)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    struct brcmf_ssid ssid;
    rt_uint32_t value;
    rt_bool_t wait_for_interface;
    rt_err_t tuning_result;
    rt_err_t result;

    brcmf_configure_arp_nd_offload(context, RT_FALSE);

    value = 0;
    tuning_result = brcmf_proto_iovar_int(
        context, context->sta_interface, "mpc", &value, RT_TRUE);
    if (tuning_result != RT_EOK)
    {
        LOG_W("could not disable MPC for APSTA: %d", tuning_result);
    }
    rt_memset(&ssid, 0, sizeof(ssid));
    brcmf_put_le32(&ssid.length, settings->ssid.len);
    rt_memcpy(ssid.value, settings->ssid.val, settings->ssid.len);
    wait_for_interface = !context->ap_interface_created;
    if (wait_for_interface)
    {
        rt_completion_init(&context->ap_interface_completion);
        context->ap_interface_pending = RT_TRUE;
    }
    result = brcmf_proto_bsscfg_iovar(
        context, context->sta_interface, context->ap_bsscfg,
        "ssid", &ssid, sizeof(ssid), RT_TRUE);
    if (result == RT_EOK && wait_for_interface)
    {
        result = rt_completion_wait(
            &context->ap_interface_completion,
            rt_tick_from_millisecond(BRCMF_AP_INTERFACE_TIMEOUT_MS));
        context->ap_interface_pending = RT_FALSE;
        if (result != RT_EOK || !context->ap_interface_created)
        {
            LOG_E("firmware did not confirm AP interface creation: %d",
                  result);
            result = result == RT_EOK ? -RT_EIO : result;
        }
    }
    else if (result != RT_EOK)
    {
        context->ap_interface_pending = RT_FALSE;
    }
    if (result == RT_EOK)
    {
        result = brcmf_ensure_distinct_ap_address(context);
    }
    if (result == RT_EOK)
    {
        result = brcmf_command_integer(context, context->ap_interface,
                                       BRCMF_C_SET_INFRA, 1U);
    }
    if (result == RT_EOK)
    {
        result = brcmf_command_integer(context, context->ap_interface,
                                       BRCMF_C_SET_AP, 1U);
    }
    if (result == RT_EOK && settings->beacon_interval)
    {
        result = brcmf_command_integer(context, context->ap_interface,
                                       BRCMF_C_SET_BCNPRD,
                                       settings->beacon_interval);
    }
    if (result == RT_EOK)
    {
        value = brcmf_chanspec(
            context, settings->channel.primary_channel);
        tuning_result = brcmf_proto_iovar_int(
            context, context->ap_interface, "chanspec", &value, RT_TRUE);
        if (tuning_result != RT_EOK)
        {
            LOG_W("could not set AP chanspec, using legacy channel: %d",
                  tuning_result);
            result = brcmf_command_integer(
                context, context->ap_interface, BRCMF_C_SET_CHANNEL,
                settings->channel.primary_channel);
        }
    }
    if (result == RT_EOK)
    {
        result = brcmf_wifi_security(context, context->ap_interface,
                                     context->ap_bsscfg,
                                     settings->security, &settings->key,
                                     RT_FALSE);
    }
    value = settings->hidden ? 1U : 0U;
    if (result == RT_EOK)
    {
        result = brcmf_proto_bsscfg_iovar(
            context, context->ap_interface, context->ap_bsscfg,
            "closednet", &value, sizeof(value), RT_TRUE);
    }
    if (result == RT_EOK)
    {
        value = 0U;
        tuning_result = brcmf_proto_bsscfg_iovar(
            context, context->ap_interface, context->ap_bsscfg,
            "wme_bss_disable", &value, sizeof(value), RT_TRUE);
        if (tuning_result != RT_EOK)
        {
            LOG_W("could not enable WME on AP: %d", tuning_result);
        }
    }
    if (result == RT_EOK)
    {
        result = brcmf_wifi_apply_ap_options(context, settings);
    }
    if (result == RT_EOK)
    {
        result = brcmf_command_integer(context, context->ap_interface,
                                       BRCMF_C_UP, 1U);
    }
    if (result == RT_EOK)
    {
        context->ap_request = settings->request_id;
        result = brcmf_set_bss(context, context->ap_bsscfg, RT_TRUE);
        if (result != RT_EOK)
        {
            context->ap_request = 0;
        }
        else
        {
            brcmf_wifi_save_ap_settings(context, settings);
            brcmf_report_ap_started(context, RT_EOK);
        }
    }
    if (result != RT_EOK)
    {
        value = 1U;
        tuning_result = brcmf_proto_iovar_int(
            context, context->sta_interface, "mpc", &value, RT_TRUE);
        if (tuning_result != RT_EOK)
        {
            LOG_W("could not restore MPC after AP failure: %d",
                  tuning_result);
        }
        brcmf_configure_arp_nd_offload(context, RT_TRUE);
    }
    return result;
}

static rt_err_t brcmf_wifi_suspend_ap_for_connect(
    struct brcmf_context *context,
    const struct rt_wlan_offload_channel_definition *channel)
{
    struct rt_wlan_offload_vif *ap =
        &context->radio.vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX];
    rt_err_t restore_result;
    rt_err_t result;

    if (!context->ap_interface_created || !context->ap_settings_valid)
    {
        return RT_EOK;
    }
    if (context->ap_suspended_for_connect)
    {
        return -RT_EBUSY;
    }

    context->suspended_ap_settings = context->ap_settings;
    context->suspended_ap_settings.request_id = 0U;
    rt_memset(&context->connect_channel, 0,
              sizeof(context->connect_channel));
    if (channel)
    {
        context->connect_channel = *channel;
    }
    result = brcmf_set_bss(context, context->ap_bsscfg, RT_FALSE);
    if (result != RT_EOK)
    {
        LOG_E("could not suspend AP before station join: %d", result);
        return result;
    }
    context->ap_suspended_for_connect = RT_TRUE;

    /* BCM43430 can leave a disabled secondary BSS attached to its old
     * channel. Remove it completely so the primary BSS follows the same
     * station-mode initialization path as a station-first connection. */
    rt_completion_init(&context->ap_interface_completion);
    context->ap_interface_pending = RT_TRUE;
    result = brcmf_proto_bsscfg_iovar(
        context, context->ap_interface, context->ap_bsscfg,
        "interface_remove", RT_NULL, 0U, RT_TRUE);
    if (result == RT_EOK)
    {
        result = rt_completion_wait(
            &context->ap_interface_completion,
            rt_tick_from_millisecond(BRCMF_AP_INTERFACE_TIMEOUT_MS));
    }
    context->ap_interface_pending = RT_FALSE;
    if (result != RT_EOK || context->ap_interface_created)
    {
        LOG_E("could not remove suspended AP interface: %d", result);
        if (context->ap_interface_created)
        {
            restore_result = brcmf_set_bss(
                context, context->ap_bsscfg, RT_TRUE);
        }
        else
        {
            restore_result = brcmf_wifi_start_ap_apsta(
                ap, &context->suspended_ap_settings);
        }
        context->ap_suspended_for_connect = RT_FALSE;
        if (restore_result != RT_EOK)
        {
            LOG_E("could not restore AP after interface removal failure: %d",
                  restore_result);
        }
        return result == RT_EOK ? -RT_EIO : result;
    }
    LOG_I("suspended and removed concurrent AP on channel %u for station join",
          context->suspended_ap_settings.channel.primary_channel);
    return RT_EOK;
}

static rt_err_t brcmf_wifi_resume_suspended_ap(
    struct brcmf_context *context, rt_err_t connect_status)
{
    struct rt_wlan_offload_ap_settings settings;
    struct rt_wlan_offload_channel_definition joined_channel;
    struct rt_wlan_offload_vif *ap =
        &context->radio.vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX];
    struct rt_wlan_offload_vif *station =
        &context->radio.vifs[RT_WLAN_OFFLOAD_VIF_STA_INDEX];
    rt_uint16_t old_channel;
    rt_err_t notify_result;
    rt_err_t result;

    if (!context->ap_suspended_for_connect)
    {
        return RT_EOK;
    }
    if (!context->ap_settings_valid)
    {
        context->ap_suspended_for_connect = RT_FALSE;
        return RT_EOK;
    }

    settings = context->suspended_ap_settings;
    old_channel = settings.channel.primary_channel;
    if (connect_status == RT_EOK)
    {
        rt_memset(&joined_channel, 0, sizeof(joined_channel));
        result = brcmf_wifi_get_channel(station, &joined_channel);
        if (result != RT_EOK || !joined_channel.primary_channel)
        {
            joined_channel = context->connect_channel;
        }
        if (joined_channel.primary_channel)
        {
            settings.channel = joined_channel;
        }
    }

    result = brcmf_wifi_start_ap_apsta(ap, &settings);
    context->ap_suspended_for_connect = RT_FALSE;
    rt_memset(&context->connect_channel, 0,
              sizeof(context->connect_channel));
    if (result != RT_EOK)
    {
        return result;
    }
    if (settings.channel.primary_channel != old_channel)
    {
        notify_result = rt_wlan_offload_ap_channel_changed(
            &context->radio, &settings.channel);
        if (notify_result != RT_EOK)
        {
            LOG_W("AP resumed on channel %u but management update failed: %d",
                  settings.channel.primary_channel, notify_result);
        }
    }
    LOG_I("resumed concurrent AP on channel %u after station join %s",
          settings.channel.primary_channel,
          connect_status == RT_EOK ? "success" : "failure");
    return RT_EOK;
}

static void brcmf_wifi_resume_ap_work(struct rt_work *work,
                                      void *work_data)
{
    struct brcmf_context *context = work_data;
    rt_err_t connect_status;
    rt_err_t result;

    (void)work;
    if (context->tearing_down)
    {
        context->ap_suspended_for_connect = RT_FALSE;
        context->ap_resume_work_queued = RT_FALSE;
        return;
    }

    connect_status = context->deferred_connect_status;
    result = brcmf_wifi_resume_suspended_ap(context, connect_status);
    if (result != RT_EOK)
    {
        LOG_E("could not resume AP after station join %s: %d",
              connect_status == RT_EOK ? "success" : "failure", result);
    }
    context->ap_resume_work_queued = RT_FALSE;
}

static rt_err_t brcmf_wifi_start_ap(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_ap_settings *settings)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    rt_err_t result;

    if (!settings->ssid.len || settings->ssid.len > BRCMF_SSID_MAX_LENGTH ||
        !settings->channel.primary_channel ||
        settings->beacon_ies_length > BRCMF_MAX_AP_IE_LENGTH ||
        (settings->beacon_ies_length && !settings->beacon_ies))
    {
        return -RT_EINVAL;
    }

    /* BCM43430 cannot reliably convert a running primary AP BSS back into
     * the APSTA station role. Establish APSTA before creating the AP so an
     * AP-first station join uses the same firmware state as STA-first. */
    result = brcmf_wifi_set_station_mode(context);
    if (result != RT_EOK)
    {
        return result;
    }
    return brcmf_wifi_start_ap_apsta(vif, settings);
}

static rt_err_t brcmf_wifi_stop_ap(struct rt_wlan_offload_vif *vif,
                                   rt_uint32_t request_id)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    struct rt_wlan_offload_event event;
    rt_uint32_t value = 1U;
    rt_err_t tuning_result;
    rt_err_t result;

    if (context->ap_settings_valid &&
        context->ap_settings_generation == context->radio.firmware_generation &&
        context->ap_settings.beacon_ies_length)
    {
        tuning_result = brcmf_wifi_program_vendor_ies(
            context, context->ap_interface, context->ap_bsscfg,
            context->ap_settings.beacon_ies,
            context->ap_settings.beacon_ies_length, RT_FALSE);
        if (tuning_result != RT_EOK)
        {
            LOG_W("could not remove AP vendor IEs on stop: %d",
                  tuning_result);
        }
    }
    result = brcmf_set_bss(context, context->ap_bsscfg, RT_FALSE);
    tuning_result = brcmf_proto_iovar_int(
        context, context->sta_interface, "mpc", &value, RT_TRUE);
    if (tuning_result != RT_EOK)
    {
        LOG_W("could not restore MPC after AP stop: %d", tuning_result);
    }
    brcmf_configure_arp_nd_offload(context, RT_TRUE);
    if (result == RT_EOK)
    {
        context->ap_settings_valid = RT_FALSE;
        rt_memset(&event, 0, sizeof(event));
        event.type = RT_WLAN_OFFLOAD_EVENT_AP_STOPPED;
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
        event.request_id = request_id;
        event.status = RT_EOK;
        rt_wlan_offload_report_event(&context->radio, &event);
    }
    return result;
}

static rt_err_t brcmf_wifi_del_station(struct rt_wlan_offload_vif *vif,
                                       rt_uint32_t request_id,
                                       const rt_uint8_t mac[6],
                                       rt_uint16_t reason)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    struct brcmf_scb_value scb;

    (void)request_id;
    brcmf_put_le32(&scb.value, reason);
    rt_memcpy(scb.address, mac, BRCMF_ETH_ALEN);
    return brcmf_proto_command(context, context->ap_interface,
                               BRCMF_C_SCB_DEAUTH_REASON, &scb,
                               sizeof(scb), RT_TRUE);
}

static rt_err_t brcmf_wifi_get_rssi(struct rt_wlan_offload_vif *vif,
                                    int *rssi)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    rt_uint8_t data[4] = {0};
    rt_err_t result = brcmf_proto_command(
        context, brcmf_interface_index(context, vif->iftype),
        BRCMF_C_GET_RSSI, data, sizeof(data), RT_FALSE);

    if (result == RT_EOK)
    {
        *rssi = (rt_int32_t)brcmf_get_le32(data);
    }
    return result;
}

static rt_err_t brcmf_wifi_set_power_save(struct rt_wlan_offload_vif *vif,
                                          int level)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    rt_err_t result = brcmf_command_integer(
        context, context->sta_interface, BRCMF_C_SET_PM,
        level ? BRCMF_PM_FAST : BRCMF_PM_OFF);

    if (result == RT_EOK)
    {
        context->station_power_save = level ? RT_TRUE : RT_FALSE;
    }
    return result;
}

static rt_err_t brcmf_wifi_get_power_save(struct rt_wlan_offload_vif *vif,
                                          int *level)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    rt_uint8_t data[4] = {0};
    rt_err_t result = brcmf_proto_command(
        context, context->sta_interface, BRCMF_C_GET_PM,
        data, sizeof(data), RT_FALSE);

    if (result == RT_EOK)
    {
        *level = brcmf_get_le32(data);
        context->station_power_save = *level != BRCMF_PM_OFF;
    }
    return result;
}

static rt_err_t brcmf_wifi_set_promiscuous(
    struct rt_wlan_offload_vif *vif, rt_bool_t enabled)
{
    struct brcmf_context *context = brcmf_vif_context(vif);

    return brcmf_command_integer(
        context, brcmf_interface_index(context, vif->iftype),
        BRCMF_C_SET_PROMISC, enabled ? 1U : 0U);
}

static rt_err_t brcmf_wifi_set_channel(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_channel_definition *channel)
{
    struct brcmf_context *context = brcmf_vif_context(vif);

    return brcmf_command_integer(
        context, brcmf_interface_index(context, vif->iftype),
        BRCMF_C_SET_CHANNEL, channel->primary_channel);
}

static rt_err_t brcmf_wifi_get_channel(
    struct rt_wlan_offload_vif *vif,
    struct rt_wlan_offload_channel_definition *channel)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    struct brcmf_channel_info info;
    rt_uint32_t number;
    rt_err_t result;

    rt_memset(&info, 0, sizeof(info));
    result = brcmf_proto_command(
        context, brcmf_interface_index(context, vif->iftype),
        BRCMF_C_GET_CHANNEL, &info, sizeof(info), RT_FALSE);
    if (result != RT_EOK)
    {
        return result;
    }
    number = brcmf_get_le32(&info.target);
    rt_memset(channel, 0, sizeof(*channel));
    channel->primary_channel = number;
    channel->width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
    channel->band = number <= 14U ? RT_WLAN_OFFLOAD_BAND_2GHZ :
                                   RT_WLAN_OFFLOAD_BAND_5GHZ;
    channel->primary_frequency_mhz = number <= 14U ?
        (number == 14U ? 2484U : 2407U + 5U * number) :
        5000U + 5U * number;
    channel->center_frequency1_mhz = channel->primary_frequency_mhz;
    return RT_EOK;
}

static rt_err_t brcmf_wifi_set_regulatory(
    struct rt_wlan_offload_radio *radio, rt_country_code_t country)
{
    struct brcmf_context *context = rt_wlan_offload_get_driver_data(radio);
    const char *alpha2;
    rt_int32_t revision = -1;

    switch (country)
    {
    case RT_COUNTRY_CHINA:
        alpha2 = "CN";
        break;
    case RT_COUNTRY_UNITED_STATES:
        alpha2 = "US";
        break;
    case RT_COUNTRY_UNITED_STATES_REV4:
        alpha2 = "US";
        revision = 4;
        break;
    case RT_COUNTRY_UNITED_STATES_MINOR_OUTLYING_ISLANDS:
        alpha2 = "UM";
        break;
    case RT_COUNTRY_WORLD_WIDE_XX:
        alpha2 = "XX";
        break;
    default:
        return -RT_ENOSYS;
    }
    return brcmf_wifi_program_country(context, alpha2, revision, country);
}

static rt_err_t brcmf_wifi_get_regulatory(
    struct rt_wlan_offload_radio *radio, rt_country_code_t *country)
{
    struct brcmf_context *context = rt_wlan_offload_get_driver_data(radio);

    if (!country)
    {
        return -RT_EINVAL;
    }
    *country = context->country;
    return RT_EOK;
}

static rt_err_t brcmf_wifi_set_mac(struct rt_wlan_offload_vif *vif,
                                   rt_uint8_t mac[6])
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    struct rt_wlan_offload_vif *other;
    rt_err_t result;

    if (!mac || !brcmf_mac_is_valid(mac))
    {
        return -RT_EINVAL;
    }
    other = &context->radio.vifs[
        vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP ?
        RT_WLAN_OFFLOAD_VIF_STA_INDEX : RT_WLAN_OFFLOAD_VIF_AP_INDEX];
    if ((vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP ||
         context->ap_interface_created) &&
        !rt_memcmp(mac, other->address, BRCMF_ETH_ALEN))
    {
        return -RT_EINVAL;
    }

    result = brcmf_proto_iovar(
        context, brcmf_interface_index(context, vif->iftype),
        "cur_etheraddr", mac, BRCMF_ETH_ALEN, RT_TRUE);

    if (result == RT_EOK)
    {
        rt_memcpy(vif->address, mac, BRCMF_ETH_ALEN);
    }
    return result;
}

static rt_err_t brcmf_wifi_get_mac(struct rt_wlan_offload_vif *vif,
                                   rt_uint8_t mac[6])
{
    rt_memcpy(mac, vif->address, BRCMF_ETH_ALEN);
    return RT_EOK;
}

static rt_uint8_t brcmf_wifi_classify_frame(const void *data, int length)
{
    const rt_uint8_t *frame = data;
    rt_size_t network_offset = sizeof(struct brcmf_eth_header);
    rt_uint16_t ethertype;
    rt_uint8_t priority = 0;

    if (length < (int)network_offset)
    {
        return 0;
    }
    ethertype = brcmf_get_be16(frame + 12U);
    if ((ethertype == BRCMF_ETH_P_8021Q ||
         ethertype == BRCMF_ETH_P_8021AD) && length >= 18)
    {
        priority = frame[14] >> 5;
        ethertype = brcmf_get_be16(frame + 16U);
        network_offset = 18U;
    }
    if (ethertype == BRCMF_ETH_P_EAPOL)
    {
        return 7U;
    }
    if (priority)
    {
        return priority;
    }
    if (ethertype == BRCMF_ETH_P_IP &&
        length >= (int)(network_offset + 2U))
    {
        return frame[network_offset + 1U] >> 5;
    }
    if (ethertype == BRCMF_ETH_P_IPV6 &&
        length >= (int)(network_offset + 2U))
    {
        rt_uint8_t traffic_class =
            (rt_uint8_t)((frame[network_offset] & 0x0fU) << 4) |
            (frame[network_offset + 1U] >> 4);

        return traffic_class >> 5;
    }
    return 0;
}

static rt_err_t brcmf_wifi_transmit(struct rt_wlan_offload_vif *vif,
                                    const void *data, int length)
{
    struct brcmf_context *context = brcmf_vif_context(vif);
    rt_uint8_t prefix[sizeof(struct brcmf_bus_record) +
                      sizeof(struct brcmf_bcdc_header)];
    struct brcmf_bus_record *record = (struct brcmf_bus_record *)prefix;
    struct brcmf_bcdc_header *header;
    struct rt_wlan_offload_bus_iovec vectors[2];
    rt_err_t result;

    if (!data || length <= 0 || length > BRCMF_MAX_FRAME)
    {
        return -RT_EINVAL;
    }
    record->channel = BRCMF_BUS_CHANNEL_DATA;
    record->interface_index = brcmf_interface_index(context, vif->iftype);
    record->length = sizeof(*header) + length;
    header = (struct brcmf_bcdc_header *)record->payload;
    header->flags = 2U << 4;
    header->priority = brcmf_wifi_classify_frame(data, length);
    header->flags2 = record->interface_index;
    header->data_offset = 0;
    vectors[0].data = prefix;
    vectors[0].length = sizeof(prefix);
    vectors[1].data = data;
    vectors[1].length = length;
    result = rt_wlan_offload_bus_transmitv(
        &context->bus, RT_WLAN_OFFLOAD_BUS_PRIORITY_NORMAL, vectors, 2);
    if (result == -RT_EFULL)
    {
        /* The transport already waited for its bounded queue to drain. If that
         * timeout expires, consume the overflow as a normal link-layer drop;
         * propagating it makes lwIP retry this same frame in a tight loop. */
        return RT_EOK;
    }
    if (result == RT_EOK)
    {
        if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP)
        {
            context->data_tx_ap_count++;
        }
        else
        {
            context->data_tx_sta_count++;
        }
    }
    return result;
}

static const struct rt_wlan_offload_ops g_brcmf_wifi_ops = {
    .start = brcmf_wifi_start,
    .stop = brcmf_wifi_stop,
    .change_interface = brcmf_wifi_change_interface,
    .scan = brcmf_wifi_scan,
    .connect = brcmf_wifi_connect,
    .disconnect = brcmf_wifi_disconnect,
    .start_ap = brcmf_wifi_start_ap,
    .stop_ap = brcmf_wifi_stop_ap,
    .del_station = brcmf_wifi_del_station,
    .abort_scan = brcmf_wifi_abort_scan,
    .get_rssi = brcmf_wifi_get_rssi,
    .set_power_save = brcmf_wifi_set_power_save,
    .get_power_save = brcmf_wifi_get_power_save,
    .set_promiscuous = brcmf_wifi_set_promiscuous,
    .set_channel = brcmf_wifi_set_channel,
    .get_channel = brcmf_wifi_get_channel,
    .set_regulatory = brcmf_wifi_set_regulatory,
    .get_regulatory = brcmf_wifi_get_regulatory,
    .set_mac = brcmf_wifi_set_mac,
    .get_mac = brcmf_wifi_get_mac,
    .transmit = brcmf_wifi_transmit,
};


static void brcmf_wifi_bus_event(struct rt_wlan_offload_bus *bus,
                                 enum rt_wlan_offload_bus_event event,
                                 rt_err_t status, void *parameter)
{
    struct brcmf_context *context = parameter;
    struct rt_wlan_offload_event report;

    if (!context || bus != &context->bus ||
        (event != RT_WLAN_OFFLOAD_BUS_EVENT_ERROR &&
         event != RT_WLAN_OFFLOAD_BUS_EVENT_UNAVAILABLE))
    {
        return;
    }

    status = status == RT_EOK ? -RT_EIO : status;
    context->firmware_running = RT_FALSE;
    context->command_status = status;
    rt_completion_done(&context->command_completion);
    if (context->tx_sem)
    {
        rt_sem_release(context->tx_sem);
    }
    if (context->rx_sem)
    {
        rt_sem_release(context->rx_sem);
    }
    if (!context->radio_registered)
    {
        return;
    }

    rt_memset(&report, 0, sizeof(report));
    report.type = RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR;
    report.iftype = RT_WLAN_OFFLOAD_IFTYPE_MAX;
    report.status = status;
    report.data.firmware.reason = event;
    rt_wlan_offload_report_event(&context->radio, &report);
}

rt_err_t brcmf_wifi_attach(struct brcmf_context *context)
{
    struct rt_wlan_offload_radio_config config;
    rt_uint16_t product = context->function1->product;
    rt_err_t result;

    context->sta_interface = 0;
    context->ap_interface = 1;
    context->country = RT_COUNTRY_UNKNOWN;
    /* Linux reserves bsscfg 1 for legacy P2P. */
    context->ap_bsscfg = 2;
    context->mac[0] = 0x02;
    context->mac[1] = 0x43;
    context->mac[2] = context->chip.id >> 8;
    context->mac[3] = context->chip.id;
    context->mac[4] = product >> 8;
    context->mac[5] = product;
    rt_wlan_offload_bus_set_callbacks(&context->bus, brcmf_proto_receive,
                                       brcmf_wifi_bus_event, context);
    brcmf_wifi_initialize_bands(context);
    rt_memset(&config, 0, sizeof(config));
    config.api_version = RT_WLAN_OFFLOAD_API_VERSION;
    config.model_name = context->mapping->model;
    config.control_device = RT_TRUE;
    config.ops = &g_brcmf_wifi_ops;
    config.bus = &context->bus;
    config.capabilities = RT_WLAN_OFFLOAD_CAP_STA |
                          RT_WLAN_OFFLOAD_CAP_AP |
                          RT_WLAN_OFFLOAD_CAP_STA_AP_CONCURRENT |
                          RT_WLAN_OFFLOAD_CAP_POWER_SAVE |
                          RT_WLAN_OFFLOAD_CAP_HOTPLUG;
    config.max_frame_size = BRCMF_MAX_FRAME;
    config.bands[RT_WLAN_OFFLOAD_BAND_2GHZ] = &context->band_2ghz;
    if (brcmf_chip_dual_band(context->chip.id))
    {
        config.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] = &context->band_5ghz;
    }
    config.cipher_suites = g_brcmf_ciphers;
    config.cipher_suite_count = sizeof(g_brcmf_ciphers) /
                                sizeof(g_brcmf_ciphers[0]);
    config.iface_combinations = g_brcmf_combinations;
    config.iface_combination_count = sizeof(g_brcmf_combinations) /
                                     sizeof(g_brcmf_combinations[0]);
    rt_memcpy(config.permanent_address, context->mac, BRCMF_ETH_ALEN);
    config.max_scan_ssids = 1;
    config.firmware_info.max_stations = 0;
    config.firmware_info.max_vifs = 2;
    config.firmware_info.max_channel_contexts = 1;
    config.driver_data = context;
    rt_work_init(&context->ap_resume_work, brcmf_wifi_resume_ap_work, context);
    context->ap_resume_work_initialized = RT_TRUE;
    rt_work_init(&context->connect_cleanup_work,
                 brcmf_wifi_connect_cleanup_work, context);
    context->connect_cleanup_work_initialized = RT_TRUE;
    result = rt_wlan_offload_register_radio(&context->radio, &config);
    context->radio_registered = result == RT_EOK;
    if (result != RT_EOK)
    {
        context->ap_resume_work_initialized = RT_FALSE;
        context->connect_cleanup_work_initialized = RT_FALSE;
    }
    return result;
}

#ifdef RT_WLAN_MANAGE_ENABLE
void brcmf_wifi_auto_start(struct brcmf_context *context)
{
    rt_err_t result;

    result = rt_wlan_set_mode(
        context->radio.vifs[RT_WLAN_OFFLOAD_VIF_STA_INDEX]
            .wlan.device.parent.name,
        RT_WLAN_STATION);
    if (result != RT_EOK)
    {
        LOG_W("automatic station initialization failed: %d", result);
    }

    result = rt_wlan_set_mode(
        context->radio.vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX]
            .wlan.device.parent.name,
        RT_WLAN_AP);
    if (result != RT_EOK)
    {
        LOG_W("automatic AP initialization failed: %d", result);
    }
}
#endif

rt_err_t brcmf_wifi_detach(struct brcmf_context *context)
{
    rt_err_t result;

    if (!context || !context->radio_registered)
    {
        return RT_EOK;
    }
    if (context->ap_resume_work_initialized)
    {
        (void)rt_work_cancel_sync(&context->ap_resume_work);
        context->ap_resume_work_queued = RT_FALSE;
        context->ap_suspended_for_connect = RT_FALSE;
    }
    if (context->connect_cleanup_work_initialized)
    {
        (void)rt_work_cancel_sync(&context->connect_cleanup_work);
        context->connect_cleanup_work_queued = RT_FALSE;
    }
    result = rt_wlan_offload_unregister_radio(&context->radio);
    if (result == RT_EOK)
    {
        context->radio_registered = RT_FALSE;
        context->ap_resume_work_initialized = RT_FALSE;
        context->connect_cleanup_work_initialized = RT_FALSE;
        rt_wlan_offload_bus_set_callbacks(&context->bus, RT_NULL, RT_NULL,
                                           RT_NULL);
    }
    return result;
}
