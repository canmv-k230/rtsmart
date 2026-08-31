/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wlan_offload_supplicant.h"
#include "wlan_offload_crypto.h"
#include "wlan_offload_sae.h"

#define DBG_TAG "WLAN.wpa"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define WPA_HANDSHAKE_TIMEOUT_MS 8000
#define WPA_TKIP_COUNTERMEASURE_MS 60000

#define EAPOL_TYPE_KEY            3
#define EAPOL_KEY_DESCRIPTOR_RSN  2
#define EAPOL_KEY_DESCRIPTOR_WPA  254
#define EAPOL_KEY_FIXED_LENGTH    99
#define EAPOL_KEY_MIC_OFFSET      81
#define EAPOL_KEY_DATA_OFFSET     99

#define WPA_KEY_INFO_VERSION_MASK 0x0007
#define WPA_KEY_INFO_PAIRWISE     0x0008
#define WPA_KEY_INFO_INDEX_MASK   0x0030
#define WPA_KEY_INFO_INSTALL      0x0040
#define WPA_KEY_INFO_ACK          0x0080
#define WPA_KEY_INFO_MIC          0x0100
#define WPA_KEY_INFO_SECURE       0x0200
#define WPA_KEY_INFO_ERROR        0x0400
#define WPA_KEY_INFO_REQUEST      0x0800
#define WPA_KEY_INFO_ENCRYPTED    0x1000
#define WPA_KEY_INFO_VERSION_AKM  0x0000
#define WPA_KEY_INFO_VERSION_1    0x0001
#define WPA_KEY_INFO_VERSION_2    0x0002
#define WPA_KEY_INFO_VERSION_3    0x0003

#define WPA_PMK_LENGTH            32
#define WPA_PTK_LENGTH            64
#define WPA_KCK_LENGTH            16
#define WPA_KEK_OFFSET            16
#define WPA_TK_OFFSET             32
#define WPA_CCMP_KEY_LENGTH       16
#define WPA_TKIP_KEY_LENGTH       32
#define WPA_NONCE_LENGTH          32
#define WPA_REPLAY_LENGTH         8
#define WPA_GTK_MAX_LENGTH        32
#define WPA_IGTK_LENGTH           16

#define SAE_AKM_SUITE             0x000fac08UL
#define SAE_AUTH_ALGORITHM        3
#define SAE_AUTH_COMMIT           1
#define SAE_AUTH_CONFIRM          2
#define SAE_AUTH_HEADER_LENGTH    30
#define SAE_STATUS_SUCCESS        0
#define SAE_STATUS_UNSPECIFIED    1

enum wlan_offload_supplicant_state
{
    WLAN_OFFLOAD_SUPPLICANT_IDLE = 0,
    WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_EXTERNAL,
    WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_COMMIT,
    WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_CONFIRM,
    WLAN_OFFLOAD_SUPPLICANT_ASSOCIATING,
    WLAN_OFFLOAD_SUPPLICANT_WAIT_M1,
    WLAN_OFFLOAD_SUPPLICANT_WAIT_M3,
    WLAN_OFFLOAD_SUPPLICANT_INSTALLING,
    WLAN_OFFLOAD_SUPPLICANT_WAIT_G1,
    WLAN_OFFLOAD_SUPPLICANT_CONNECTED,
};

enum wlan_offload_wpa_protocol
{
    WLAN_OFFLOAD_WPA_PROTOCOL_WPA = 1,
    WLAN_OFFLOAD_WPA_PROTOCOL_RSN = 2,
};

struct wlan_offload_wpa_profile
{
    const char *name;
    enum wlan_offload_wpa_protocol protocol;
    enum rt_wlan_offload_cipher pairwise_cipher;
    enum rt_wlan_offload_cipher group_cipher;
    const rt_uint8_t *association_ie;
    rt_uint8_t association_ie_length;
    rt_bool_t sae;
    rt_bool_t sha256_akm;
    rt_bool_t mfp_required;
};

struct rt_wlan_offload_supplicant
{
    struct rt_wlan_offload_radio *radio;
    struct rt_mutex lock;
    rt_timer_t timer;
    enum wlan_offload_supplicant_state state;
    rt_uint32_t request_id;
    rt_wlan_ssid_t ssid;
    rt_uint8_t bssid[6];
    rt_uint8_t own_address[6];
    rt_uint8_t pmk[WPA_PMK_LENGTH];
    rt_uint8_t ptk[WPA_PTK_LENGTH];
    rt_uint8_t anonce[WPA_NONCE_LENGTH];
    rt_uint8_t snonce[WPA_NONCE_LENGTH];
    rt_uint8_t replay[WPA_REPLAY_LENGTH];
    rt_uint8_t eapol_version;
    rt_uint8_t descriptor_type;
    rt_uint16_t key_length;
    struct rt_wlan_offload_channel_definition channel;
    rt_uint8_t password[RT_WLAN_OFFLOAD_SAE_MAX_PASSWORD_LENGTH];
    rt_uint8_t password_length;
    struct rt_wlan_offload_sae sae;
    const struct wlan_offload_wpa_profile *profile;
    rt_tick_t tkip_mic_failure_tick;
    rt_tick_t tkip_countermeasure_tick;
    rt_bool_t tkip_mic_failure_valid;
    rt_bool_t tkip_countermeasures_active;
};

static const rt_uint8_t g_wpa2_psk_ccmp_rsn_ie[] = {
    0x30, 0x14,             /* RSN element, 20-byte body */
    0x01, 0x00,             /* version 1 */
    0x00, 0x0f, 0xac, 0x04, /* group cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* pairwise cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02, /* AKM: PSK */
    0x00, 0x00              /* RSN capabilities */
};

static const rt_uint8_t g_wpa2_psk_tkip_rsn_ie[] = {
    0x30, 0x14,
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02, /* group cipher: TKIP */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02, /* pairwise cipher: TKIP */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02,
    0x00, 0x00
};

static const rt_uint8_t g_wpa2_psk_tkip_ccmp_rsn_ie[] = {
    0x30, 0x14,
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* group cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02, /* pairwise cipher: TKIP */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02,
    0x00, 0x00
};

static const rt_uint8_t g_wpa2_psk_mixed_rsn_ie[] = {
    0x30, 0x14,
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02, /* group cipher: TKIP */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* pairwise cipher: CCMP */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02,
    0x00, 0x00
};

static const rt_uint8_t g_wpa_psk_tkip_ie[] = {
    0xdd, 0x16,
    0x00, 0x50, 0xf2, 0x01,
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x02, /* group cipher: TKIP */
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x02, /* pairwise cipher: TKIP */
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x02
};

static const rt_uint8_t g_wpa_psk_ccmp_ie[] = {
    0xdd, 0x16,
    0x00, 0x50, 0xf2, 0x01,
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x02, /* group cipher: TKIP */
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x04, /* pairwise cipher: CCMP */
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x02
};

static const rt_uint8_t g_wpa_psk_ccmp_group_ccmp_ie[] = {
    0xdd, 0x16,
    0x00, 0x50, 0xf2, 0x01,
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x04, /* group cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x04, /* pairwise cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x50, 0xf2, 0x02
};

static const rt_uint8_t g_wpa2_psk_ccmp_pmf_rsn_ie[] = {
    0x30, 0x1a,
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* group cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* pairwise cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x02, /* AKM: PSK */
    0x80, 0x00,             /* MFPC */
    0x00, 0x00,             /* PMKID count */
    0x00, 0x0f, 0xac, 0x06  /* group management cipher: BIP-CMAC-128 */
};

static const rt_uint8_t g_wpa2_psk_sha256_ccmp_rsn_ie[] = {
    0x30, 0x1a,
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* group cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* pairwise cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x06, /* AKM: PSK-SHA256 */
    0x80, 0x00,             /* MFPC */
    0x00, 0x00,             /* PMKID count */
    0x00, 0x0f, 0xac, 0x06  /* group management cipher: BIP-CMAC-128 */
};

static const rt_uint8_t g_wpa3_sae_ccmp_rsn_ie[] = {
    0x30, 0x1a,
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* group cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, /* pairwise cipher: CCMP-128 */
    0x01, 0x00,
    0x00, 0x0f, 0xac, 0x08, /* AKM: SAE */
    0xc0, 0x00,             /* MFPR and MFPC */
    0x00, 0x00,             /* PMKID count */
    0x00, 0x0f, 0xac, 0x06  /* group management cipher: BIP-CMAC-128 */
};

static const struct wlan_offload_wpa_profile g_wpa2_ccmp_profile = {
    "WPA2-PSK/CCMP", WLAN_OFFLOAD_WPA_PROTOCOL_RSN,
    RT_WLAN_OFFLOAD_CIPHER_CCMP, RT_WLAN_OFFLOAD_CIPHER_CCMP,
    g_wpa2_psk_ccmp_rsn_ie, sizeof(g_wpa2_psk_ccmp_rsn_ie), RT_FALSE,
    RT_FALSE, RT_FALSE
};

static const struct wlan_offload_wpa_profile g_wpa2_tkip_profile = {
    "WPA2-PSK/TKIP", WLAN_OFFLOAD_WPA_PROTOCOL_RSN,
    RT_WLAN_OFFLOAD_CIPHER_TKIP, RT_WLAN_OFFLOAD_CIPHER_TKIP,
    g_wpa2_psk_tkip_rsn_ie, sizeof(g_wpa2_psk_tkip_rsn_ie), RT_FALSE,
    RT_FALSE, RT_FALSE
};

static const struct wlan_offload_wpa_profile g_wpa2_tkip_ccmp_profile = {
    "WPA2-PSK/TKIP-CCMP(group)", WLAN_OFFLOAD_WPA_PROTOCOL_RSN,
    RT_WLAN_OFFLOAD_CIPHER_TKIP, RT_WLAN_OFFLOAD_CIPHER_CCMP,
    g_wpa2_psk_tkip_ccmp_rsn_ie, sizeof(g_wpa2_psk_tkip_ccmp_rsn_ie), RT_FALSE,
    RT_FALSE, RT_FALSE
};

static const struct wlan_offload_wpa_profile g_wpa2_mixed_profile = {
    "WPA2-PSK/CCMP+TKIP", WLAN_OFFLOAD_WPA_PROTOCOL_RSN,
    RT_WLAN_OFFLOAD_CIPHER_CCMP, RT_WLAN_OFFLOAD_CIPHER_TKIP,
    g_wpa2_psk_mixed_rsn_ie, sizeof(g_wpa2_psk_mixed_rsn_ie), RT_FALSE,
    RT_FALSE, RT_FALSE
};

static const struct wlan_offload_wpa_profile g_wpa_tkip_profile = {
    "WPA-PSK/TKIP", WLAN_OFFLOAD_WPA_PROTOCOL_WPA,
    RT_WLAN_OFFLOAD_CIPHER_TKIP, RT_WLAN_OFFLOAD_CIPHER_TKIP,
    g_wpa_psk_tkip_ie, sizeof(g_wpa_psk_tkip_ie), RT_FALSE, RT_FALSE,
    RT_FALSE
};

static const struct wlan_offload_wpa_profile g_wpa_ccmp_profile = {
    "WPA-PSK/CCMP", WLAN_OFFLOAD_WPA_PROTOCOL_WPA,
    RT_WLAN_OFFLOAD_CIPHER_CCMP, RT_WLAN_OFFLOAD_CIPHER_TKIP,
    g_wpa_psk_ccmp_ie, sizeof(g_wpa_psk_ccmp_ie), RT_FALSE, RT_FALSE,
    RT_FALSE
};

static const struct wlan_offload_wpa_profile g_wpa_ccmp_group_ccmp_profile = {
    "WPA-PSK/CCMP(group)-CCMP", WLAN_OFFLOAD_WPA_PROTOCOL_WPA,
    RT_WLAN_OFFLOAD_CIPHER_CCMP, RT_WLAN_OFFLOAD_CIPHER_CCMP,
    g_wpa_psk_ccmp_group_ccmp_ie,
    sizeof(g_wpa_psk_ccmp_group_ccmp_ie), RT_FALSE, RT_FALSE, RT_FALSE
};

static const struct wlan_offload_wpa_profile g_wpa2_ccmp_pmf_profile = {
    "WPA2-PSK/CCMP-PMF", WLAN_OFFLOAD_WPA_PROTOCOL_RSN,
    RT_WLAN_OFFLOAD_CIPHER_CCMP, RT_WLAN_OFFLOAD_CIPHER_CCMP,
    g_wpa2_psk_ccmp_pmf_rsn_ie,
    sizeof(g_wpa2_psk_ccmp_pmf_rsn_ie), RT_FALSE, RT_FALSE, RT_TRUE
};

static const struct wlan_offload_wpa_profile g_wpa2_sha256_ccmp_profile = {
    "WPA2-PSK-SHA256/CCMP", WLAN_OFFLOAD_WPA_PROTOCOL_RSN,
    RT_WLAN_OFFLOAD_CIPHER_CCMP, RT_WLAN_OFFLOAD_CIPHER_CCMP,
    g_wpa2_psk_sha256_ccmp_rsn_ie,
    sizeof(g_wpa2_psk_sha256_ccmp_rsn_ie), RT_FALSE, RT_TRUE, RT_TRUE
};

static const struct wlan_offload_wpa_profile g_wpa3_sae_profile = {
    "WPA3-SAE/CCMP", WLAN_OFFLOAD_WPA_PROTOCOL_RSN,
    RT_WLAN_OFFLOAD_CIPHER_CCMP, RT_WLAN_OFFLOAD_CIPHER_CCMP,
    g_wpa3_sae_ccmp_rsn_ie, sizeof(g_wpa3_sae_ccmp_rsn_ie), RT_TRUE,
    RT_TRUE, RT_TRUE
};

static rt_uint16_t supplicant_get_be16(const rt_uint8_t *data)
{
    return ((rt_uint16_t)data[0] << 8) | data[1];
}

static void supplicant_put_be16(rt_uint8_t *data, rt_uint16_t value)
{
    data[0] = (rt_uint8_t)(value >> 8);
    data[1] = (rt_uint8_t)value;
}

static rt_uint16_t supplicant_get_le16(const rt_uint8_t *data)
{
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8);
}

static void supplicant_put_le16(rt_uint8_t *data, rt_uint16_t value)
{
    data[0] = (rt_uint8_t)value;
    data[1] = (rt_uint8_t)(value >> 8);
}

static rt_bool_t supplicant_mac_is_zero(const rt_uint8_t mac[6])
{
    static const rt_uint8_t zero[6] = {0};

    return !mac || rt_memcmp(mac, zero, sizeof(zero)) == 0;
}

static const struct wlan_offload_wpa_profile *supplicant_profile(
    rt_wlan_security_t security)
{
    switch (security)
    {
    case SECURITY_WPA_TKIP_PSK:
        return &g_wpa_tkip_profile;
    case SECURITY_WPA_AES_PSK:
        return &g_wpa_ccmp_profile;
    case SECURITY_WPA2_TKIP_PSK:
        return &g_wpa2_tkip_profile;
    case SECURITY_WPA2_MIXED_PSK:
        return &g_wpa2_mixed_profile;
    case SECURITY_WPA2_AES_CMAC:
        return &g_wpa2_ccmp_pmf_profile;
    case SECURITY_WPA2_AES_PSK:
        return &g_wpa2_ccmp_profile;
    case SECURITY_WPA_WPA2_MIXED_PSK:
        /* Without the AP IEs the legacy WPA/TKIP leg is the only profile
         * whose group cipher is implied by the RT-Thread mixed-mode enum.
         * Normal scan-driven connects select the stronger RSN profile from
         * the advertised suites instead. */
        return &g_wpa_tkip_profile;
    case SECURITY_WPA2_AES_PSK_SHA256:
        return &g_wpa2_sha256_ccmp_profile;
    case SECURITY_WPA3_AES_PSK:
    case SECURITY_WPA3_SAE:
    case SECURITY_WPA2_WPA3_MIXED_PSK:
        return &g_wpa3_sae_profile;
    default:
        return RT_NULL;
    }
}

struct wlan_offload_ap_security
{
    rt_bool_t rsn;
    rt_bool_t wpa;
    rt_bool_t rsn_group_tkip;
    rt_bool_t rsn_group_ccmp;
    rt_bool_t rsn_pairwise_tkip;
    rt_bool_t rsn_pairwise_ccmp;
    rt_bool_t rsn_psk;
    rt_bool_t rsn_psk_sha256;
    rt_bool_t rsn_ft_psk;
    rt_bool_t rsn_sae;
    rt_bool_t rsn_ft_sae;
    rt_bool_t rsn_mfp_capable;
    rt_bool_t rsn_mfp_required;
    rt_bool_t rsn_group_mgmt_bip_cmac;
    rt_bool_t wpa_group_tkip;
    rt_bool_t wpa_group_ccmp;
    rt_bool_t wpa_pairwise_tkip;
    rt_bool_t wpa_pairwise_ccmp;
    rt_bool_t wpa_psk;
};

static rt_bool_t supplicant_suite_oui(const rt_uint8_t suite[4],
                                      rt_bool_t rsn)
{
    static const rt_uint8_t rsn_oui[3] = {0x00, 0x0f, 0xac};
    static const rt_uint8_t wpa_oui[3] = {0x00, 0x50, 0xf2};

    return rt_memcmp(suite, rsn ? rsn_oui : wpa_oui, 3) == 0;
}

static void supplicant_parse_cipher(struct wlan_offload_ap_security *info,
                                    const rt_uint8_t suite[4], rt_bool_t rsn,
                                    rt_bool_t group)
{
    rt_uint8_t type;

    if (!supplicant_suite_oui(suite, rsn))
    {
        return;
    }
    type = suite[3];
    if (group)
    {
        if (type == 2)
        {
            if (rsn)
            {
                info->rsn_group_tkip = RT_TRUE;
            }
            else
            {
                info->wpa_group_tkip = RT_TRUE;
            }
        }
        else if (type == 4)
        {
            if (rsn)
            {
                info->rsn_group_ccmp = RT_TRUE;
            }
            else
            {
                info->wpa_group_ccmp = RT_TRUE;
            }
        }
    }
    else if (type == 2)
    {
        if (rsn)
        {
            info->rsn_pairwise_tkip = RT_TRUE;
        }
        else
        {
            info->wpa_pairwise_tkip = RT_TRUE;
        }
    }
    else if (type == 4)
    {
        if (rsn)
        {
            info->rsn_pairwise_ccmp = RT_TRUE;
        }
        else
        {
            info->wpa_pairwise_ccmp = RT_TRUE;
        }
    }
}

static void supplicant_parse_rsn_akm(struct wlan_offload_ap_security *info,
                                     const rt_uint8_t suite[4])
{
    rt_uint8_t type;

    if (!supplicant_suite_oui(suite, RT_TRUE))
    {
        return;
    }
    type = suite[3];
    switch (type)
    {
    case 2:
        info->rsn_psk = RT_TRUE;
        break;
    case 4:
        info->rsn_ft_psk = RT_TRUE;
        break;
    case 6:
        info->rsn_psk_sha256 = RT_TRUE;
        break;
    case 8:
        info->rsn_sae = RT_TRUE;
        break;
    case 9:
        info->rsn_ft_sae = RT_TRUE;
        break;
    default:
        break;
    }
}

static rt_bool_t supplicant_parse_rsn_ie(struct wlan_offload_ap_security *info,
                                         const rt_uint8_t *body,
                                         rt_size_t length)
{
    struct wlan_offload_ap_security parsed;
    struct wlan_offload_ap_security *output = info;
    rt_size_t position = 0;
    rt_uint16_t count;
    rt_size_t index;

    if (!info || !body || length < 8U || supplicant_get_le16(body) != 1U)
    {
        return RT_FALSE;
    }
    parsed = *info;
    info = &parsed;
    position = 2U;
    supplicant_parse_cipher(info, body + position, RT_TRUE, RT_TRUE);
    position += 4U;
    if (position + 2U > length)
    {
        return RT_FALSE;
    }
    count = supplicant_get_le16(body + position);
    position += 2U;
    if (count > (length - position) / 4U)
    {
        return RT_FALSE;
    }
    for (index = 0; index < count; index++)
    {
        supplicant_parse_cipher(info, body + position, RT_TRUE, RT_FALSE);
        position += 4U;
    }
    if (position + 2U > length)
    {
        return RT_FALSE;
    }
    count = supplicant_get_le16(body + position);
    position += 2U;
    if (count > (length - position) / 4U)
    {
        return RT_FALSE;
    }
    for (index = 0; index < count; index++)
    {
        supplicant_parse_rsn_akm(info, body + position);
        position += 4U;
    }
    if (position + 2U <= length)
    {
        rt_uint16_t capabilities = supplicant_get_le16(body + position);

        info->rsn_mfp_required = (capabilities & 0x0040U) != 0;
        info->rsn_mfp_capable = (capabilities & 0x0080U) != 0;
        position += 2U;
    }
    if (position + 2U <= length)
    {
        count = supplicant_get_le16(body + position);
        position += 2U;
        if (count > (length - position) / 16U)
        {
            return RT_FALSE;
        }
        position += count * 16U;
    }
    if (position < length)
    {
        if (length - position < 4U)
        {
            return RT_FALSE;
        }
        info->rsn_group_mgmt_bip_cmac =
            supplicant_suite_oui(body + position, RT_TRUE) &&
            body[position + 3U] == 6;
    }
    else if (info->rsn_mfp_capable)
    {
        /* IEEE 802.11 defaults an omitted group-management suite to
         * BIP-CMAC-128 when management protection is negotiated. */
        info->rsn_group_mgmt_bip_cmac = RT_TRUE;
    }
    info->rsn = RT_TRUE;
    *output = parsed;
    return RT_TRUE;
}

static rt_bool_t supplicant_parse_wpa_ie(struct wlan_offload_ap_security *info,
                                         const rt_uint8_t *body,
                                         rt_size_t length)
{
    struct wlan_offload_ap_security parsed;
    struct wlan_offload_ap_security *output = info;
    rt_size_t position = 4U;
    rt_uint16_t count;
    rt_size_t index;

    if (!info || !body || length < 12U ||
        body[0] != 0x00 || body[1] != 0x50 || body[2] != 0xf2 ||
        body[3] != 0x01 || supplicant_get_le16(body + position) != 1U)
    {
        return RT_FALSE;
    }
    parsed = *info;
    info = &parsed;
    position += 2U;
    supplicant_parse_cipher(info, body + position, RT_FALSE, RT_TRUE);
    position += 4U;
    if (position + 2U > length)
    {
        return RT_FALSE;
    }
    count = supplicant_get_le16(body + position);
    position += 2U;
    if (count > (length - position) / 4U)
    {
        return RT_FALSE;
    }
    for (index = 0; index < count; index++)
    {
        supplicant_parse_cipher(info, body + position, RT_FALSE, RT_FALSE);
        position += 4U;
    }
    if (position + 2U > length)
    {
        return RT_FALSE;
    }
    count = supplicant_get_le16(body + position);
    position += 2U;
    if (count > (length - position) / 4U)
    {
        return RT_FALSE;
    }
    for (index = 0; index < count; index++)
    {
        if (supplicant_suite_oui(body + position, RT_FALSE) &&
            body[position + 3U] == 2)
        {
            info->wpa_psk = RT_TRUE;
        }
        position += 4U;
    }
    info->wpa = RT_TRUE;
    *output = parsed;
    return RT_TRUE;
}

static rt_bool_t supplicant_parse_ap_ies(const rt_uint8_t *ies,
                                         rt_size_t length,
                                         struct wlan_offload_ap_security *info)
{
    rt_size_t offset = 0;
    rt_bool_t parsed = RT_FALSE;
    rt_bool_t rsn_seen = RT_FALSE;
    rt_bool_t wpa_seen = RT_FALSE;

    if (!info)
    {
        return RT_FALSE;
    }
    rt_memset(info, 0, sizeof(*info));
    while (ies && offset + 2U <= length)
    {
        rt_uint8_t element_length = ies[offset + 1U];

        if (offset + 2U + element_length > length)
        {
            break;
        }
        /* Only the first RSN and the first WPA element are honoured.  The
         * per-element parsers accumulate cipher and AKM flags, so parsing a
         * duplicate would build the union of both advertisements - a suite set
         * no single AP ever offered.  Beacons are unauthenticated, so an
         * injected second element could otherwise steer profile selection with
         * suites the real AP does not implement. */
        if (ies[offset] == 48)
        {
            if (!rsn_seen)
            {
                rsn_seen = RT_TRUE;
                parsed |= supplicant_parse_rsn_ie(info, ies + offset + 2U,
                                                  element_length);
            }
        }
        else if (ies[offset] == 221 && element_length >= 4U &&
                 ies[offset + 2U] == 0x00 && ies[offset + 3U] == 0x50 &&
                 ies[offset + 4U] == 0xf2 && ies[offset + 5U] == 0x01)
        {
            if (!wpa_seen)
            {
                wpa_seen = RT_TRUE;
                parsed |= supplicant_parse_wpa_ie(info, ies + offset + 2U,
                                                  element_length);
            }
        }
        offset += 2U + element_length;
    }
    return parsed;
}

static const struct wlan_offload_wpa_profile *supplicant_rsn_sae_profile(
    const struct wlan_offload_ap_security *info)
{
    if (!info || !info->rsn_sae || !info->rsn_mfp_capable ||
        !info->rsn_group_mgmt_bip_cmac)
    {
        return RT_NULL;
    }
    return info->rsn_pairwise_ccmp && info->rsn_group_ccmp ?
           &g_wpa3_sae_profile : RT_NULL;
}

static const struct wlan_offload_wpa_profile *supplicant_rsn_sha256_profile(
    const struct wlan_offload_ap_security *info)
{
    if (!info || !info->rsn_psk_sha256 || !info->rsn_mfp_capable ||
        !info->rsn_group_mgmt_bip_cmac)
    {
        return RT_NULL;
    }
    return info->rsn_pairwise_ccmp && info->rsn_group_ccmp ?
           &g_wpa2_sha256_ccmp_profile : RT_NULL;
}

static const struct wlan_offload_wpa_profile *supplicant_rsn_profile(
    const struct wlan_offload_ap_security *info, rt_bool_t prefer_sae,
    rt_bool_t prefer_sha256, rt_bool_t require_mfp)
{
    const struct wlan_offload_wpa_profile *profile;

    if (!info || !info->rsn)
    {
        return RT_NULL;
    }
    if (prefer_sae)
    {
        return supplicant_rsn_sae_profile(info);
    }
    if (require_mfp)
    {
        return info->rsn_psk && info->rsn_pairwise_ccmp &&
               info->rsn_group_ccmp && info->rsn_mfp_capable &&
               info->rsn_group_mgmt_bip_cmac ?
               &g_wpa2_ccmp_pmf_profile : RT_NULL;
    }
    if (prefer_sha256)
    {
        profile = supplicant_rsn_sha256_profile(info);
        if (profile || !info->rsn_psk)
        {
            return profile;
        }
    }
    if (!info->rsn_psk)
    {
        profile = supplicant_rsn_sha256_profile(info);
        return profile ? profile : supplicant_rsn_sae_profile(info);
    }
    if (info->rsn_mfp_required)
    {
        return info->rsn_pairwise_ccmp && info->rsn_group_ccmp &&
               info->rsn_mfp_capable && info->rsn_group_mgmt_bip_cmac ?
               &g_wpa2_ccmp_pmf_profile : RT_NULL;
    }
    if (info->rsn_pairwise_ccmp)
    {
        return info->rsn_group_tkip ? &g_wpa2_mixed_profile :
               info->rsn_group_ccmp ? &g_wpa2_ccmp_profile : RT_NULL;
    }
    if (info->rsn_pairwise_tkip)
    {
        return info->rsn_group_tkip ? &g_wpa2_tkip_profile :
               info->rsn_group_ccmp ? &g_wpa2_tkip_ccmp_profile : RT_NULL;
    }
    return RT_NULL;
}

static const struct wlan_offload_wpa_profile *supplicant_wpa_profile(
    const struct wlan_offload_ap_security *info)
{
    if (!info || !info->wpa || !info->wpa_psk)
    {
        return RT_NULL;
    }
    if (info->wpa_pairwise_ccmp)
    {
        return info->wpa_group_ccmp ? &g_wpa_ccmp_group_ccmp_profile :
               info->wpa_group_tkip ? &g_wpa_ccmp_profile : RT_NULL;
    }
    return info->wpa_pairwise_tkip && info->wpa_group_tkip ?
           &g_wpa_tkip_profile : RT_NULL;
}

static const struct wlan_offload_wpa_profile *supplicant_profile_for_request(
    const struct rt_wlan_offload_connect_request *request,
    const rt_uint8_t *bss_ies, rt_size_t bss_ies_length)
{
    struct wlan_offload_ap_security info;
    const struct wlan_offload_wpa_profile *profile;
    rt_bool_t prefer_sae;
    rt_bool_t prefer_sha256;
    rt_bool_t require_mfp;
    rt_bool_t allow_rsn;
    rt_bool_t allow_wpa;

    if (!request || !bss_ies || !bss_ies_length ||
        !supplicant_parse_ap_ies(bss_ies, bss_ies_length, &info))
    {
        return request ? supplicant_profile(request->security) : RT_NULL;
    }
    prefer_sae = (((request->security & SAE_ENABLED) != 0) &&
                  ((request->security & WPA2_SECURITY) == 0)) ||
                 request->security == SECURITY_WPA3_AES_PSK;
    prefer_sha256 = request->security == SECURITY_WPA2_AES_PSK_SHA256;
    require_mfp = request->security == SECURITY_WPA2_AES_CMAC;
    allow_rsn = (request->security & (WPA2_SECURITY | WPA3_SECURITY)) != 0;
    allow_wpa = (request->security & WPA_SECURITY) != 0;
    if (allow_rsn)
    {
        profile = supplicant_rsn_profile(&info, prefer_sae, prefer_sha256,
                                         require_mfp);
        if (profile)
        {
            return profile;
        }
    }
    if (allow_wpa)
    {
        profile = supplicant_wpa_profile(&info);
        if (profile)
        {
            return profile;
        }
    }
    return RT_NULL;
}

static rt_wlan_security_t supplicant_security_for_profile(
    const struct wlan_offload_wpa_profile *profile)
{
    if (!profile)
    {
        return SECURITY_UNKNOWN;
    }
    if (profile->sae)
    {
        return SECURITY_WPA3_SAE;
    }
    if (profile->sha256_akm)
    {
        return SECURITY_WPA2_AES_PSK_SHA256;
    }
    if (profile->protocol == WLAN_OFFLOAD_WPA_PROTOCOL_WPA)
    {
        return profile->pairwise_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP ?
               SECURITY_WPA_TKIP_PSK : SECURITY_WPA_AES_PSK;
    }
    if (profile->pairwise_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP)
    {
        return SECURITY_WPA2_TKIP_PSK;
    }
    return profile->group_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP ?
           SECURITY_WPA2_MIXED_PSK : SECURITY_WPA2_AES_PSK;
}

rt_bool_t rt_wlan_offload_supplicant_supports(rt_wlan_security_t security)
{
    if (supplicant_profile(security))
    {
        return RT_TRUE;
    }
    switch (security)
    {
    case SECURITY_FT_WPA2_AES_PSK:
    case SECURITY_WPA3_AES_PSK_SHA384:
    case SECURITY_FT_WPA3_AES_PSK_SHA384:
    case SECURITY_FT_WPA3_SAE:
    case SECURITY_WPA3_SAE_EXT_KEY:
    case SECURITY_FT_WPA3_SAE_EXT_KEY:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static rt_uint8_t supplicant_cipher_key_length(enum rt_wlan_offload_cipher cipher)
{
    return cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP ? WPA_TKIP_KEY_LENGTH :
                                              WPA_CCMP_KEY_LENGTH;
}

static rt_int8_t supplicant_hex_digit(rt_uint8_t value)
{
    if (value >= '0' && value <= '9')
    {
        return (rt_int8_t)(value - '0');
    }
    if (value >= 'a' && value <= 'f')
    {
        return (rt_int8_t)(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F')
    {
        return (rt_int8_t)(value - 'A' + 10);
    }
    return -1;
}

static rt_bool_t supplicant_decode_raw_psk(const rt_wlan_key_t *key,
                                           rt_uint8_t pmk[WPA_PMK_LENGTH])
{
    rt_size_t index;

    if (!key || !pmk || key->len != 64U)
    {
        return RT_FALSE;
    }
    for (index = 0; index < WPA_PMK_LENGTH; index++)
    {
        rt_int8_t high = supplicant_hex_digit(key->val[index * 2U]);
        rt_int8_t low = supplicant_hex_digit(key->val[index * 2U + 1U]);

        if (high < 0 || low < 0)
        {
            return RT_FALSE;
        }
        pmk[index] = (rt_uint8_t)((high << 4) | low);
    }
    return RT_TRUE;
}

static rt_uint16_t supplicant_cipher_descriptor_version(
    const struct wlan_offload_wpa_profile *profile)
{
    if (profile->sae)
    {
        return WPA_KEY_INFO_VERSION_AKM;
    }
    if (profile->sha256_akm)
    {
        return WPA_KEY_INFO_VERSION_3;
    }
    return profile->pairwise_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP ?
           WPA_KEY_INFO_VERSION_1 : WPA_KEY_INFO_VERSION_2;
}

static int supplicant_replay_compare(const rt_uint8_t left[8],
                                     const rt_uint8_t right[8])
{
    return rt_memcmp(left, right, 8);
}

static void supplicant_clear_locked(struct rt_wlan_offload_supplicant *supplicant)
{
    supplicant->state = WLAN_OFFLOAD_SUPPLICANT_IDLE;
    supplicant->request_id = 0;
    supplicant->eapol_version = 0;
    supplicant->descriptor_type = 0;
    supplicant->key_length = 0;
    supplicant->password_length = 0;
    supplicant->profile = RT_NULL;
    rt_memset(supplicant->bssid, 0, sizeof(supplicant->bssid));
    rt_memset(&supplicant->ssid, 0, sizeof(supplicant->ssid));
    rt_memset(supplicant->own_address, 0, sizeof(supplicant->own_address));
    rt_wlan_offload_crypto_zero(supplicant->pmk, sizeof(supplicant->pmk));
    rt_wlan_offload_crypto_zero(supplicant->ptk, sizeof(supplicant->ptk));
    rt_wlan_offload_crypto_zero(supplicant->anonce, sizeof(supplicant->anonce));
    rt_wlan_offload_crypto_zero(supplicant->snonce, sizeof(supplicant->snonce));
    rt_wlan_offload_crypto_zero(supplicant->replay, sizeof(supplicant->replay));
    rt_wlan_offload_crypto_zero(supplicant->password,
                           sizeof(supplicant->password));
    rt_wlan_offload_sae_clear(&supplicant->sae);
    rt_memset(&supplicant->channel, 0, sizeof(supplicant->channel));
}

static struct rt_wlan_offload_supplicant *supplicant_lock_from_radio(
    struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_supplicant *supplicant;

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    supplicant = radio->supplicant;
    if (supplicant)
    {
        rt_mutex_take(&supplicant->lock, RT_WAITING_FOREVER);
    }
    rt_mutex_release(&radio->operation_lock);
    return supplicant;
}

static rt_err_t supplicant_random(rt_uint8_t *data, rt_size_t length)
{
    rt_device_t random = rt_device_find("hwrng");
    rt_size_t received = 0;
    rt_err_t result = RT_EOK;

    if (!random)
    {
        LOG_E("hardware random device hwrng is unavailable");
        return -RT_ENOSYS;
    }

    result = rt_device_open(random, RT_DEVICE_OFLAG_RDONLY);
    if (result != RT_EOK)
    {
        LOG_E("hardware random open failed: %d", result);
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
    if (result != RT_EOK)
    {
        LOG_E("hardware random read failed: %u/%u bytes",
              (unsigned int)received, (unsigned int)length);
    }
    return result;
}

static int supplicant_sae_random(void *context, rt_uint8_t *data,
                                 size_t length)
{
    (void)context;
    return supplicant_random(data, length) == RT_EOK ? 0 : -1;
}

static void supplicant_report_result(struct rt_wlan_offload_radio *radio,
                                     rt_uint32_t request_id,
                                     rt_err_t status,
                                     const rt_uint8_t bssid[6])
{
    struct rt_wlan_offload_event event;

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
        rt_memcpy(event.data.network.bssid, bssid, 6);
    }
    rt_wlan_offload_report_event(radio, &event);
}

static void supplicant_disconnect_failed_link(struct rt_wlan_offload_radio *radio,
                                              rt_uint16_t reason)
{
    rt_uint32_t request_id = rt_wlan_offload_alloc_request_id(radio);

    if (request_id)
    {
        rt_wlan_offload_disconnect(radio, RT_WLAN_OFFLOAD_IFTYPE_STATION,
                              request_id, reason);
    }
}

static void supplicant_fail(struct rt_wlan_offload_radio *radio, rt_err_t status)
{
    struct rt_wlan_offload_supplicant *supplicant;
    rt_uint32_t request_id;
    rt_uint8_t bssid[6];

    supplicant = supplicant_lock_from_radio(radio);
    if (!supplicant)
    {
        return;
    }
    request_id = supplicant->request_id;
    rt_memcpy(bssid, supplicant->bssid, sizeof(bssid));
    supplicant_clear_locked(supplicant);
    rt_timer_stop(supplicant->timer);
    rt_mutex_release(&supplicant->lock);
    supplicant_report_result(radio, request_id, status, bssid);
    supplicant_disconnect_failed_link(radio, 15U);
}

static void supplicant_reject_external(struct rt_wlan_offload_radio *radio,
                                       rt_err_t status)
{
    struct rt_wlan_offload_supplicant *supplicant;
    rt_uint32_t request_id;
    rt_uint8_t bssid[6];

    supplicant = supplicant_lock_from_radio(radio);
    if (!supplicant)
    {
        return;
    }
    request_id = supplicant->request_id;
    rt_memcpy(bssid, supplicant->bssid, sizeof(bssid));
    supplicant_clear_locked(supplicant);
    rt_timer_stop(supplicant->timer);
    rt_mutex_release(&supplicant->lock);
    supplicant_report_result(radio, request_id, status, bssid);
}

static void supplicant_timeout(void *parameter)
{
    struct rt_wlan_offload_radio *radio = parameter;
    struct rt_wlan_offload_supplicant *supplicant;
    rt_uint32_t request_id = 0;
    rt_uint8_t bssid[6] = {0};

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    supplicant = radio->supplicant;
    if (supplicant)
    {
        rt_mutex_take(&supplicant->lock, RT_WAITING_FOREVER);
        if (supplicant->state != WLAN_OFFLOAD_SUPPLICANT_IDLE &&
            supplicant->state != WLAN_OFFLOAD_SUPPLICANT_CONNECTED)
        {
            request_id = supplicant->request_id;
            rt_memcpy(bssid, supplicant->bssid, sizeof(bssid));
            supplicant_clear_locked(supplicant);
        }
        rt_mutex_release(&supplicant->lock);
    }
    rt_mutex_release(&radio->operation_lock);

    if (request_id)
    {
        LOG_W("WPA authentication timed out");
        supplicant_report_result(radio, request_id, -RT_ETIMEOUT, bssid);
        supplicant_disconnect_failed_link(radio, 15U);
    }
}

static struct rt_wlan_offload_supplicant *supplicant_get_or_create(
    struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_supplicant *supplicant;

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    supplicant = radio->supplicant;
    rt_mutex_release(&radio->operation_lock);
    if (supplicant)
    {
        return supplicant;
    }

    supplicant = rt_calloc(1, sizeof(*supplicant));
    if (!supplicant)
    {
        return RT_NULL;
    }
    supplicant->radio = radio;
    if (rt_mutex_init(&supplicant->lock, "fmwpa", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        rt_free(supplicant);
        return RT_NULL;
    }
    supplicant->timer = rt_timer_create(
        "fmwpa", supplicant_timeout, radio,
        rt_tick_from_millisecond(WPA_HANDSHAKE_TIMEOUT_MS),
        RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (!supplicant->timer)
    {
        rt_mutex_detach(&supplicant->lock);
        rt_free(supplicant);
        return RT_NULL;
    }

    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    if (!radio->supplicant)
    {
        radio->supplicant = supplicant;
        supplicant = RT_NULL;
    }
    rt_mutex_release(&radio->operation_lock);
    if (supplicant)
    {
        rt_timer_delete(supplicant->timer);
        rt_mutex_detach(&supplicant->lock);
        rt_free(supplicant);
    }
    return radio->supplicant;
}

rt_err_t rt_wlan_offload_supplicant_prepare(
    struct rt_wlan_offload_radio *radio,
    struct rt_wlan_offload_connect_request *request,
    const rt_uint8_t *bss_ies, rt_size_t bss_ies_length)
{
    struct rt_wlan_offload_supplicant *supplicant;
    const struct wlan_offload_wpa_profile *profile;
    rt_uint8_t raw_pmk[WPA_PMK_LENGTH];
    rt_bool_t raw_psk;
    rt_err_t result;

    profile = request ? supplicant_profile_for_request(
        request, bss_ies, bss_ies_length) : RT_NULL;
    rt_memset(raw_pmk, 0, sizeof(raw_pmk));
    raw_psk = request && profile && !profile->sae &&
              supplicant_decode_raw_psk(&request->key, raw_pmk);
    if (!radio || !request || !profile ||
        (profile && profile->sae ?
         (!request->key.len ||
          request->key.len > RT_WLAN_OFFLOAD_SAE_MAX_PASSWORD_LENGTH) :
         ((!raw_psk) && (request->key.len < 8 || request->key.len > 63))) ||
        !request->ssid.len ||
        !(radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT) ||
        !radio->ops ||
        !radio->ops->add_key || !radio->ops->transmit ||
        (profile && profile->sae &&
         (!(radio->capabilities & RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTH) ||
          !radio->ops->transmit_mgmt ||
          !radio->ops->external_auth_response)))
    {
        LOG_W("embedded connection does not support security 0x%08x",
              request ? (unsigned int)request->security : 0U);
        rt_wlan_offload_crypto_zero(raw_pmk, sizeof(raw_pmk));
        return -RT_ENOSYS;
    }
    /* The legacy driver callback validates this enum before sending the
     * association command.  Once the AP IE has selected a supported profile,
     * expose that concrete profile rather than an advertised FT/SHA256
     * variant which the embedded handshake intentionally falls back from. */
    request->security = supplicant_security_for_profile(profile);
    supplicant = supplicant_get_or_create(radio);
    if (!supplicant)
    {
        rt_wlan_offload_crypto_zero(raw_pmk, sizeof(raw_pmk));
        return -RT_ENOMEM;
    }

    rt_mutex_take(&supplicant->lock, RT_WAITING_FOREVER);
    if (supplicant->tkip_countermeasures_active)
    {
        rt_tick_t elapsed = rt_tick_get() -
            supplicant->tkip_countermeasure_tick;
        rt_bool_t tkip = profile->pairwise_cipher ==
                             RT_WLAN_OFFLOAD_CIPHER_TKIP ||
                         profile->group_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP;

        if (elapsed < rt_tick_from_millisecond(WPA_TKIP_COUNTERMEASURE_MS) &&
            tkip)
        {
            rt_wlan_offload_crypto_zero(raw_pmk, sizeof(raw_pmk));
            rt_mutex_release(&supplicant->lock);
            LOG_W("TKIP association blocked during MIC countermeasures");
            return -RT_EBUSY;
        }
        if (elapsed >= rt_tick_from_millisecond(
                           WPA_TKIP_COUNTERMEASURE_MS))
        {
            supplicant->tkip_countermeasures_active = RT_FALSE;
        }
    }
    if (supplicant->state != WLAN_OFFLOAD_SUPPLICANT_IDLE)
    {
        rt_wlan_offload_crypto_zero(raw_pmk, sizeof(raw_pmk));
        rt_mutex_release(&supplicant->lock);
        return -RT_EBUSY;
    }
    result = supplicant_random(supplicant->snonce,
                               sizeof(supplicant->snonce));
    if (result == RT_EOK && !profile->sae)
    {
        if (raw_psk)
        {
            rt_memcpy(supplicant->pmk, raw_pmk, sizeof(raw_pmk));
        }
        else if (rt_wlan_offload_pbkdf2_sha1(request->key.val, request->key.len,
                                        request->ssid.val, request->ssid.len,
                                        supplicant->pmk) != 0)
        {
            result = -RT_ERROR;
        }
    }
    rt_wlan_offload_crypto_zero(raw_pmk, sizeof(raw_pmk));
    if (result != RT_EOK)
    {
        supplicant_clear_locked(supplicant);
        rt_mutex_release(&supplicant->lock);
        return result;
    }

    supplicant->request_id = request->request_id;
    supplicant->state = profile->sae ?
        WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_EXTERNAL :
        WLAN_OFFLOAD_SUPPLICANT_ASSOCIATING;
    supplicant->profile = profile;
    supplicant->ssid = request->ssid;
    rt_memcpy(supplicant->bssid, request->bssid, sizeof(supplicant->bssid));
    rt_memcpy(supplicant->own_address,
              radio->vifs[0].address, sizeof(supplicant->own_address));
    supplicant->channel = request->channel;
    if (profile->sae)
    {
        supplicant->password_length = request->key.len;
        rt_memcpy(supplicant->password, request->key.val, request->key.len);
    }
    request->ies = profile->association_ie;
    request->ies_length = profile->association_ie_length;
    rt_timer_stop(supplicant->timer);
    result = rt_timer_start(supplicant->timer);
    if (result != RT_EOK)
    {
        supplicant_clear_locked(supplicant);
    }
    rt_mutex_release(&supplicant->lock);
    if (result == RT_EOK)
    {
        LOG_I("embedded %s handshake prepared", profile->name);
    }
    return result;
}

void rt_wlan_offload_supplicant_cancel(struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_supplicant *supplicant;

    if (!radio)
    {
        return;
    }
    supplicant = supplicant_lock_from_radio(radio);
    if (!supplicant)
    {
        return;
    }
    rt_timer_stop(supplicant->timer);
    supplicant_clear_locked(supplicant);
    rt_mutex_release(&supplicant->lock);
}

void rt_wlan_offload_supplicant_deinit(struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_supplicant *supplicant;

    if (!radio)
    {
        return;
    }
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    supplicant = radio->supplicant;
    radio->supplicant = RT_NULL;
    if (supplicant)
    {
        rt_timer_stop(supplicant->timer);
        rt_timer_delete(supplicant->timer);
    }
    rt_mutex_release(&radio->operation_lock);
    if (supplicant)
    {
        rt_mutex_take(&supplicant->lock, RT_WAITING_FOREVER);
        supplicant_clear_locked(supplicant);
        /* The radio pointer is already cleared and the timer is gone.  Detach
         * while holding the mutex so a previously queued user cannot acquire
         * it between release and destruction. */
        rt_mutex_detach(&supplicant->lock);
        rt_free(supplicant);
    }
}

static rt_err_t supplicant_send_sae_auth(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_channel_definition *channel,
    const rt_uint8_t own_address[6], const rt_uint8_t bssid[6],
    rt_uint16_t transaction, const rt_uint8_t *payload,
    rt_size_t payload_length)
{
    struct rt_wlan_offload_mgmt_frame request;
    rt_uint8_t *frame;
    rt_err_t result;

    frame = rt_calloc(1, SAE_AUTH_HEADER_LENGTH + payload_length);
    if (!frame)
    {
        return -RT_ENOMEM;
    }
    frame[0] = 0xb0; /* IEEE 802.11 authentication management frame */
    rt_memcpy(frame + 4, bssid, 6);
    rt_memcpy(frame + 10, own_address, 6);
    rt_memcpy(frame + 16, bssid, 6);
    supplicant_put_le16(frame + 24, SAE_AUTH_ALGORITHM);
    supplicant_put_le16(frame + 26, transaction);
    supplicant_put_le16(frame + 28, SAE_STATUS_SUCCESS);
    if (payload_length)
    {
        rt_memcpy(frame + SAE_AUTH_HEADER_LENGTH, payload, payload_length);
    }
    rt_memset(&request, 0, sizeof(request));
    request.request_id = rt_wlan_offload_alloc_request_id(radio);
    request.channel = *channel;
    request.wait_ms = 1000;
    request.data = frame;
    request.length = SAE_AUTH_HEADER_LENGTH + payload_length;
    result = request.request_id ?
        rt_wlan_offload_transmit_mgmt(radio, RT_WLAN_OFFLOAD_IFTYPE_STATION,
                                 &request) : -RT_EBUSY;
    rt_wlan_offload_crypto_zero(frame, request.length);
    rt_free(frame);
    return result;
}

static rt_bool_t supplicant_handle_external_auth(
    struct rt_wlan_offload_radio *radio, const struct rt_wlan_offload_event *event)
{
    struct rt_wlan_offload_supplicant *supplicant;
    struct rt_wlan_offload_channel_definition channel;
    rt_uint8_t own_address[6];
    rt_uint8_t bssid[6];
    rt_uint8_t commit[RT_WLAN_OFFLOAD_SAE_COMMIT_LENGTH];
    rt_err_t status = RT_EOK;
    rt_bool_t consume = RT_FALSE;

    supplicant = supplicant_lock_from_radio(radio);
    if (!supplicant)
    {
        return RT_FALSE;
    }
    if (supplicant->state == WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_EXTERNAL &&
        supplicant->profile && supplicant->profile->sae &&
        event->request_id == supplicant->request_id)
    {
        consume = RT_TRUE;
        if (event->data.external_auth.akm_suite != SAE_AKM_SUITE ||
            event->data.external_auth.ssid.len != supplicant->ssid.len ||
            rt_memcmp(event->data.external_auth.ssid.val,
                      supplicant->ssid.val, supplicant->ssid.len) != 0 ||
            (!supplicant_mac_is_zero(supplicant->bssid) &&
             rt_memcmp(event->data.external_auth.bssid,
                       supplicant->bssid, 6) != 0))
        {
            status = -RT_EINVAL;
        }
        else
        {
            rt_memcpy(supplicant->bssid,
                      event->data.external_auth.bssid, 6);
            status = rt_wlan_offload_sae_prepare(
                &supplicant->sae, supplicant->own_address,
                supplicant->bssid, supplicant->password,
                supplicant->password_length, supplicant_sae_random,
                RT_NULL) == 0 ? RT_EOK : -RT_ERROR;
        }
        if (status == RT_EOK &&
            rt_wlan_offload_sae_write_commit(&supplicant->sae, commit) != 0)
        {
            status = -RT_ERROR;
        }
        if (status == RT_EOK)
        {
            channel = supplicant->channel;
            rt_memcpy(own_address, supplicant->own_address, 6);
            rt_memcpy(bssid, supplicant->bssid, 6);
            supplicant->state = WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_COMMIT;
        }
    }
    rt_mutex_release(&supplicant->lock);
    if (!consume)
    {
        return RT_FALSE;
    }
    if (status == RT_EOK)
    {
        status = supplicant_send_sae_auth(
            radio, &channel, own_address, bssid, SAE_AUTH_COMMIT,
            commit, sizeof(commit));
    }
    rt_wlan_offload_crypto_zero(commit, sizeof(commit));
    if (status != RT_EOK)
    {
        rt_wlan_offload_external_auth_response(
            radio, RT_WLAN_OFFLOAD_IFTYPE_STATION, SAE_STATUS_UNSPECIFIED);
        supplicant_reject_external(radio, status);
    }
    else
    {
        LOG_I("SAE commit sent");
    }
    return RT_TRUE;
}

static rt_bool_t supplicant_handle_sae_management(
    struct rt_wlan_offload_radio *radio, const struct rt_wlan_offload_event *event)
{
    const rt_uint8_t *frame = event->data.management.data;
    rt_size_t length = event->data.management.length;
    struct rt_wlan_offload_supplicant *supplicant;
    struct rt_wlan_offload_channel_definition channel;
    rt_uint8_t own_address[6];
    rt_uint8_t bssid[6];
    rt_uint8_t confirm[RT_WLAN_OFFLOAD_SAE_CONFIRM_LENGTH];
    rt_uint16_t transaction;
    rt_uint16_t auth_status;
    rt_bool_t send_confirm = RT_FALSE;
    rt_bool_t finish_auth = RT_FALSE;
    rt_bool_t consume = RT_FALSE;
    rt_err_t status = RT_EOK;

    if (!frame || length < SAE_AUTH_HEADER_LENGTH ||
        (frame[0] & 0xfcU) != 0xb0U ||
        supplicant_get_le16(frame + 24) != SAE_AUTH_ALGORITHM)
    {
        return RT_FALSE;
    }
    transaction = supplicant_get_le16(frame + 26);
    auth_status = supplicant_get_le16(frame + 28);
    supplicant = supplicant_lock_from_radio(radio);
    if (!supplicant)
    {
        return RT_FALSE;
    }
    if (supplicant->profile && supplicant->profile->sae &&
        rt_memcmp(frame + 4, supplicant->own_address, 6) == 0 &&
        rt_memcmp(frame + 10, supplicant->bssid, 6) == 0)
    {
        if (transaction == SAE_AUTH_COMMIT &&
            supplicant->state == WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_COMMIT)
        {
            consume = RT_TRUE;
            if (auth_status != SAE_STATUS_SUCCESS ||
                rt_wlan_offload_sae_process_commit(
                    &supplicant->sae, frame + SAE_AUTH_HEADER_LENGTH,
                    length - SAE_AUTH_HEADER_LENGTH) != 0 ||
                rt_wlan_offload_sae_write_confirm(&supplicant->sae, confirm) != 0)
            {
                status = -RT_ERROR;
            }
            else
            {
                supplicant->state = WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_CONFIRM;
                channel = event->data.management.channel;
                rt_memcpy(own_address, supplicant->own_address, 6);
                rt_memcpy(bssid, supplicant->bssid, 6);
                send_confirm = RT_TRUE;
            }
        }
        else if (transaction == SAE_AUTH_CONFIRM &&
                 supplicant->state == WLAN_OFFLOAD_SUPPLICANT_SAE_WAIT_CONFIRM)
        {
            consume = RT_TRUE;
            if (auth_status != SAE_STATUS_SUCCESS ||
                rt_wlan_offload_sae_check_confirm(
                    &supplicant->sae, frame + SAE_AUTH_HEADER_LENGTH,
                    length - SAE_AUTH_HEADER_LENGTH) != 0)
            {
                status = -RT_ERROR;
            }
            else
            {
                rt_memcpy(supplicant->pmk, supplicant->sae.pmk,
                          sizeof(supplicant->pmk));
                supplicant->state = WLAN_OFFLOAD_SUPPLICANT_ASSOCIATING;
                finish_auth = RT_TRUE;
            }
        }
    }
    rt_mutex_release(&supplicant->lock);
    if (!consume)
    {
        return RT_FALSE;
    }
    if (status == RT_EOK && send_confirm)
    {
        status = supplicant_send_sae_auth(
            radio, &channel, own_address, bssid, SAE_AUTH_CONFIRM,
            confirm, sizeof(confirm));
        if (status == RT_EOK)
        {
            LOG_I("SAE confirm sent");
        }
    }
    if (status == RT_EOK && finish_auth)
    {
        status = rt_wlan_offload_external_auth_response(
            radio, RT_WLAN_OFFLOAD_IFTYPE_STATION, SAE_STATUS_SUCCESS);
        if (status == RT_EOK)
        {
            LOG_I("SAE authentication complete");
        }
    }
    rt_wlan_offload_crypto_zero(confirm, sizeof(confirm));
    if (status != RT_EOK)
    {
        rt_wlan_offload_external_auth_response(
            radio, RT_WLAN_OFFLOAD_IFTYPE_STATION, SAE_STATUS_UNSPECIFIED);
        supplicant_fail(radio, status);
    }
    return RT_TRUE;
}

rt_bool_t rt_wlan_offload_supplicant_filter_event(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_event *event)
{
    struct rt_wlan_offload_supplicant *supplicant;
    rt_bool_t consume = RT_FALSE;
    rt_bool_t mic_disconnect = RT_FALSE;

    if (!radio || !event)
    {
        return RT_FALSE;
    }
    if (event->type == RT_WLAN_OFFLOAD_EVENT_EXTERNAL_AUTH_REQUIRED)
    {
        return supplicant_handle_external_auth(radio, event);
    }
    if (event->type == RT_WLAN_OFFLOAD_EVENT_MGMT_RX)
    {
        return supplicant_handle_sae_management(radio, event);
    }
    supplicant = supplicant_lock_from_radio(radio);
    if (!supplicant)
    {
        return RT_FALSE;
    }
    if (event->type == RT_WLAN_OFFLOAD_EVENT_TKIP_MIC_FAILURE &&
        event->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION &&
        supplicant->profile &&
        (supplicant->profile->pairwise_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP ||
         supplicant->profile->group_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP) &&
        supplicant->state == WLAN_OFFLOAD_SUPPLICANT_CONNECTED)
    {
        rt_tick_t now = rt_tick_get();
        rt_tick_t window =
            rt_tick_from_millisecond(WPA_TKIP_COUNTERMEASURE_MS);

        if (supplicant->tkip_mic_failure_valid &&
            now - supplicant->tkip_mic_failure_tick < window)
        {
            supplicant->tkip_mic_failure_valid = RT_FALSE;
            supplicant->tkip_countermeasures_active = RT_TRUE;
            supplicant->tkip_countermeasure_tick = now;
            rt_timer_stop(supplicant->timer);
            supplicant_clear_locked(supplicant);
            mic_disconnect = RT_TRUE;
            LOG_E("second TKIP MIC failure; countermeasures enabled");
        }
        else
        {
            supplicant->tkip_mic_failure_tick = now;
            supplicant->tkip_mic_failure_valid = RT_TRUE;
            LOG_W("TKIP MIC failure detected");
        }
    }
    else if (event->type == RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT &&
        event->request_id == supplicant->request_id &&
        (supplicant->state == WLAN_OFFLOAD_SUPPLICANT_ASSOCIATING ||
         supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_M1 ||
         supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_M3))
    {
        if (event->status == RT_EOK)
        {
            if (!supplicant_mac_is_zero(event->data.network.bssid))
            {
                rt_memcpy(supplicant->bssid,
                          event->data.network.bssid, 6);
            }
            if (supplicant->state == WLAN_OFFLOAD_SUPPLICANT_ASSOCIATING)
            {
                supplicant->state = WLAN_OFFLOAD_SUPPLICANT_WAIT_M1;
                LOG_I("%s association complete; waiting for message 1/4",
                      supplicant->profile->name);
            }
            consume = RT_TRUE;
        }
        else
        {
            rt_timer_stop(supplicant->timer);
            supplicant_clear_locked(supplicant);
        }
    }
    else if (event->type == RT_WLAN_OFFLOAD_EVENT_DISCONNECTED ||
             event->type == RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR ||
             event->type == RT_WLAN_OFFLOAD_EVENT_RADIO_OFFLINE)
    {
        rt_timer_stop(supplicant->timer);
        supplicant_clear_locked(supplicant);
    }
    rt_mutex_release(&supplicant->lock);
    if (mic_disconnect)
    {
        supplicant_disconnect_failed_link(radio, 14U);
    }
    return consume;
}

static void supplicant_derive_ptk(struct rt_wlan_offload_supplicant *supplicant)
{
    rt_uint8_t context[76];
    const rt_uint8_t *first;
    const rt_uint8_t *second;

    if (rt_memcmp(supplicant->bssid, supplicant->own_address, 6) < 0)
    {
        first = supplicant->bssid;
        second = supplicant->own_address;
    }
    else
    {
        first = supplicant->own_address;
        second = supplicant->bssid;
    }
    rt_memcpy(context, first, 6);
    rt_memcpy(context + 6, second, 6);
    if (rt_memcmp(supplicant->anonce, supplicant->snonce,
                  WPA_NONCE_LENGTH) < 0)
    {
        first = supplicant->anonce;
        second = supplicant->snonce;
    }
    else
    {
        first = supplicant->snonce;
        second = supplicant->anonce;
    }
    rt_memcpy(context + 12, first, WPA_NONCE_LENGTH);
    rt_memcpy(context + 44, second, WPA_NONCE_LENGTH);
    if (supplicant->profile->sha256_akm)
    {
        rt_wlan_offload_sha256_prf(supplicant->pmk, sizeof(supplicant->pmk),
                              "Pairwise key expansion", context,
                              sizeof(context), supplicant->ptk, 48);
    }
    else
    {
        rt_wlan_offload_wpa_prf(supplicant->pmk, sizeof(supplicant->pmk),
                           "Pairwise key expansion", context,
                           sizeof(context), supplicant->ptk,
                           sizeof(supplicant->ptk));
    }
    rt_wlan_offload_crypto_zero(context, sizeof(context));
}

static rt_err_t supplicant_make_key_frame(
    struct rt_wlan_offload_supplicant *supplicant,
    rt_uint16_t key_info, rt_uint16_t key_length,
    const rt_uint8_t replay[8],
    const rt_uint8_t *nonce,
    const rt_uint8_t *key_data,
    rt_size_t key_data_length,
    rt_uint8_t **output,
    rt_size_t *output_length)
{
    rt_uint8_t digest[20];
    rt_uint8_t *frame;
    rt_size_t length = EAPOL_KEY_FIXED_LENGTH + key_data_length;

    frame = rt_calloc(1, length);
    if (!frame)
    {
        return -RT_ENOMEM;
    }
    frame[0] = supplicant->eapol_version;
    frame[1] = EAPOL_TYPE_KEY;
    supplicant_put_be16(frame + 2, (rt_uint16_t)(length - 4U));
    frame[4] = supplicant->descriptor_type;
    supplicant_put_be16(frame + 5, key_info);
    supplicant_put_be16(frame + 7, key_length);
    rt_memcpy(frame + 9, replay, 8);
    if (nonce)
    {
        rt_memcpy(frame + 17, nonce, WPA_NONCE_LENGTH);
    }
    supplicant_put_be16(frame + 97, (rt_uint16_t)key_data_length);
    if (key_data_length)
    {
        rt_memcpy(frame + EAPOL_KEY_DATA_OFFSET, key_data, key_data_length);
    }
    if (key_info & WPA_KEY_INFO_MIC)
    {
        if (supplicant->profile->sha256_akm)
        {
            if (rt_wlan_offload_aes_cmac(supplicant->ptk, frame, length,
                                    digest) != 0)
            {
                rt_wlan_offload_crypto_zero(frame, length);
                rt_free(frame);
                return -RT_ERROR;
            }
        }
        else if ((key_info & WPA_KEY_INFO_VERSION_MASK) ==
                 WPA_KEY_INFO_VERSION_1)
        {
            rt_wlan_offload_hmac_md5(supplicant->ptk, WPA_KCK_LENGTH,
                                frame, length, digest);
        }
        else
        {
            rt_wlan_offload_hmac_sha1(supplicant->ptk, WPA_KCK_LENGTH,
                                 frame, length, digest);
        }
        rt_memcpy(frame + EAPOL_KEY_MIC_OFFSET, digest, WPA_KCK_LENGTH);
        rt_wlan_offload_crypto_zero(digest, sizeof(digest));
    }
    *output = frame;
    *output_length = length;
    return RT_EOK;
}

static rt_bool_t supplicant_verify_mic(
    struct rt_wlan_offload_supplicant *supplicant,
    const rt_uint8_t *frame,
    rt_size_t length)
{
    rt_uint8_t received[WPA_KCK_LENGTH];
    rt_uint8_t digest[20];
    rt_uint8_t *copy = rt_malloc(length);
    rt_bool_t valid;

    if (!copy)
    {
        return RT_FALSE;
    }
    rt_memcpy(copy, frame, length);
    rt_memcpy(received, copy + EAPOL_KEY_MIC_OFFSET, sizeof(received));
    rt_memset(copy + EAPOL_KEY_MIC_OFFSET, 0, WPA_KCK_LENGTH);
    if (supplicant->profile->sha256_akm)
    {
        if (rt_wlan_offload_aes_cmac(supplicant->ptk, copy, length, digest) != 0)
        {
            rt_wlan_offload_crypto_zero(received, sizeof(received));
            rt_wlan_offload_crypto_zero(copy, length);
            rt_free(copy);
            return RT_FALSE;
        }
    }
    else if ((supplicant_get_be16(frame + 5) & WPA_KEY_INFO_VERSION_MASK) ==
             WPA_KEY_INFO_VERSION_1)
    {
        rt_wlan_offload_hmac_md5(supplicant->ptk, WPA_KCK_LENGTH,
                            copy, length, digest);
    }
    else
    {
        rt_wlan_offload_hmac_sha1(supplicant->ptk, WPA_KCK_LENGTH,
                             copy, length, digest);
    }
    valid = rt_wlan_offload_crypto_equal(received, digest, sizeof(received));
    rt_wlan_offload_crypto_zero(received, sizeof(received));
    rt_wlan_offload_crypto_zero(digest, sizeof(digest));
    rt_wlan_offload_crypto_zero(copy, length);
    rt_free(copy);
    return valid;
}

static rt_err_t supplicant_decrypt_key_data(
    struct rt_wlan_offload_supplicant *supplicant,
    const rt_uint8_t *frame,
    rt_size_t frame_length,
    rt_uint8_t **plain,
    rt_size_t *plain_length)
{
    rt_uint16_t key_data_length = supplicant_get_be16(frame + 97);
    rt_uint16_t version =
        supplicant_get_be16(frame + 5) & WPA_KEY_INFO_VERSION_MASK;
    rt_uint8_t encryption_key[32];
    rt_err_t result = -RT_ERROR;

    *plain = RT_NULL;
    *plain_length = 0;
    if (!key_data_length || key_data_length >
        frame_length - EAPOL_KEY_DATA_OFFSET)
    {
        return key_data_length ? -RT_EIO : RT_EOK;
    }
    *plain = rt_malloc(key_data_length);
    if (!*plain)
    {
        return -RT_ENOMEM;
    }
    if (version == WPA_KEY_INFO_VERSION_1)
    {
        rt_memcpy(*plain, frame + EAPOL_KEY_DATA_OFFSET, key_data_length);
        rt_memcpy(encryption_key, frame + 49, 16);
        rt_memcpy(encryption_key + 16,
                  supplicant->ptk + WPA_KEK_OFFSET, 16);
        if (rt_wlan_offload_rc4_skip(encryption_key, sizeof(encryption_key), 256,
                               *plain, key_data_length) == 0)
        {
            *plain_length = key_data_length;
            result = RT_EOK;
        }
        rt_wlan_offload_crypto_zero(encryption_key, sizeof(encryption_key));
    }
    else if ((version == WPA_KEY_INFO_VERSION_2 ||
              version == WPA_KEY_INFO_VERSION_3 ||
              (version == WPA_KEY_INFO_VERSION_AKM &&
               supplicant->profile->sae)) &&
             key_data_length >= 24U &&
             !(key_data_length & 7U))
    {
        *plain_length = key_data_length - 8U;
        if (rt_wlan_offload_aes_unwrap(supplicant->ptk + WPA_KEK_OFFSET,
                                  frame + EAPOL_KEY_DATA_OFFSET,
                                  key_data_length, *plain,
                                  plain_length) == 0)
        {
            result = RT_EOK;
        }
    }
    if (result != RT_EOK)
    {
        rt_wlan_offload_crypto_zero(*plain, key_data_length);
        rt_free(*plain);
        *plain = RT_NULL;
        *plain_length = 0;
    }
    return result;
}

static rt_err_t supplicant_extract_gtk(
    struct rt_wlan_offload_supplicant *supplicant,
    const rt_uint8_t *frame,
    rt_size_t frame_length,
    rt_uint8_t *gtk,
    rt_uint8_t *gtk_length,
    rt_uint8_t *gtk_index,
    rt_uint8_t *igtk,
    rt_uint8_t *igtk_length,
    rt_uint8_t *igtk_index,
    rt_uint8_t igtk_sequence[6])
{
    rt_uint16_t key_data_length = supplicant_get_be16(frame + 97);
    rt_uint16_t key_info = supplicant_get_be16(frame + 5);
    rt_uint16_t key_length = supplicant_get_be16(frame + 7);
    rt_uint8_t *plain = RT_NULL;
    rt_size_t plain_length = 0;
    rt_size_t offset = 0;
    rt_err_t result = -RT_EEMPTY;

    *gtk_length = 0;
    *igtk_length = 0;
    if (!key_data_length)
    {
        return RT_EOK;
    }
    if (supplicant->profile->protocol == WLAN_OFFLOAD_WPA_PROTOCOL_RSN &&
        !(key_info & WPA_KEY_INFO_ENCRYPTED))
    {
        return -RT_EIO;
    }
    result = supplicant_decrypt_key_data(supplicant, frame, frame_length,
                                         &plain, &plain_length);
    if (result != RT_EOK)
    {
        return result;
    }
    if (supplicant->profile->protocol == WLAN_OFFLOAD_WPA_PROTOCOL_WPA)
    {
        rt_uint8_t expected = supplicant_cipher_key_length(
            supplicant->profile->group_cipher);

        if (key_length != expected || key_length > plain_length)
        {
            result = -RT_EIO;
            goto exit;
        }
        *gtk_index = (rt_uint8_t)((key_info & WPA_KEY_INFO_INDEX_MASK) >> 4);
        *gtk_length = (rt_uint8_t)key_length;
        rt_memcpy(gtk, plain, key_length);
        result = RT_EOK;
        goto exit;
    }
    result = -RT_EEMPTY;
    while (offset + 2U <= plain_length)
    {
        rt_uint8_t element = plain[offset];
        rt_uint8_t element_length = plain[offset + 1U];

        if (element == 0 && element_length == 0)
        {
            break;
        }
        if (offset + 2U + element_length > plain_length)
        {
            result = -RT_EIO;
            break;
        }
        if (element == 0xdd && element_length >= 6U + WPA_CCMP_KEY_LENGTH &&
            plain[offset + 2] == 0x00 && plain[offset + 3] == 0x0f &&
            plain[offset + 4] == 0xac && plain[offset + 5] == 0x01)
        {
            rt_size_t length = element_length - 6U;

            if (length > WPA_GTK_MAX_LENGTH)
            {
                result = -RT_EIO;
                break;
            }
            *gtk_index = plain[offset + 6] & 3U;
            *gtk_length = (rt_uint8_t)length;
            rt_memcpy(gtk, plain + offset + 8, length);
            result = RT_EOK;
        }
        else if (element == 0xdd &&
                 element_length == 4U + 2U + 6U + WPA_IGTK_LENGTH &&
                 plain[offset + 2] == 0x00 && plain[offset + 3] == 0x0f &&
                 plain[offset + 4] == 0xac && plain[offset + 5] == 0x09)
        {
            rt_uint16_t key_id = supplicant_get_le16(plain + offset + 6);

            if (key_id < 4U || key_id > 5U)
            {
                result = -RT_EIO;
                break;
            }
            *igtk_index = (rt_uint8_t)key_id;
            rt_memcpy(igtk_sequence, plain + offset + 8, 6);
            rt_memcpy(igtk, plain + offset + 14, WPA_IGTK_LENGTH);
            *igtk_length = WPA_IGTK_LENGTH;
            result = RT_EOK;
        }
        offset += 2U + element_length;
    }

exit:
    rt_wlan_offload_crypto_zero(plain, key_data_length);
    rt_free(plain);
    return result;
}

static rt_err_t supplicant_install_key(
    struct rt_wlan_offload_radio *radio,
    rt_bool_t pairwise,
    enum rt_wlan_offload_cipher cipher,
    rt_uint8_t index,
    const rt_uint8_t *key,
    rt_uint8_t key_length,
    const rt_uint8_t *sequence,
    const rt_uint8_t *peer)
{
    struct rt_wlan_offload_key wlan_offload_key;
    rt_uint32_t request_id;
    rt_err_t result;

    rt_memset(&wlan_offload_key, 0, sizeof(wlan_offload_key));
    wlan_offload_key.cipher = cipher;
    wlan_offload_key.index = index;
    wlan_offload_key.pairwise = pairwise;
    wlan_offload_key.set_transmit = RT_TRUE;
    wlan_offload_key.key_length = key_length;
    rt_memcpy(wlan_offload_key.key, key, key_length);
    if (pairwise)
    {
        rt_memcpy(wlan_offload_key.peer, peer, 6);
    }
    else if (sequence)
    {
        wlan_offload_key.sequence_length = 6;
        rt_memcpy(wlan_offload_key.sequence, sequence, 6);
    }
    request_id = rt_wlan_offload_alloc_request_id(radio);
    if (!request_id)
    {
        result = -RT_EBUSY;
    }
    else
    {
        result = rt_wlan_offload_add_key(radio,
                                    RT_WLAN_OFFLOAD_IFTYPE_STATION,
                                    request_id, &wlan_offload_key);
    }
    rt_wlan_offload_crypto_zero(&wlan_offload_key, sizeof(wlan_offload_key));
    return result;
}

static rt_err_t supplicant_set_authorized(
    struct rt_wlan_offload_radio *radio, const rt_uint8_t bssid[6],
    rt_bool_t authorized)
{
    rt_uint32_t request_id = rt_wlan_offload_alloc_request_id(radio);

    return request_id ? rt_wlan_offload_set_station_authorized(
                            radio, RT_WLAN_OFFLOAD_IFTYPE_STATION,
                            request_id, bssid, authorized) : -RT_EBUSY;
}

static rt_err_t supplicant_handle_message1(
    struct rt_wlan_offload_supplicant *supplicant,
    const rt_uint8_t *frame,
    rt_size_t length,
    rt_uint8_t **response,
    rt_size_t *response_length)
{
    rt_uint16_t key_info = supplicant_get_be16(frame + 5);
    rt_err_t result;

    (void)length;
    rt_memcpy(supplicant->replay, frame + 9, WPA_REPLAY_LENGTH);
    rt_memcpy(supplicant->anonce, frame + 17, WPA_NONCE_LENGTH);
    supplicant->eapol_version = frame[0];
    supplicant->descriptor_type = frame[4];
    supplicant->key_length = supplicant_get_be16(frame + 7);
    supplicant_derive_ptk(supplicant);
    result = supplicant_make_key_frame(
        supplicant,
        (key_info & WPA_KEY_INFO_VERSION_MASK) |
        WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_MIC,
        supplicant->key_length,
        supplicant->replay, supplicant->snonce,
        supplicant->profile->association_ie,
        supplicant->profile->association_ie_length,
        response, response_length);
    if (result == RT_EOK)
    {
        supplicant->state = WLAN_OFFLOAD_SUPPLICANT_WAIT_M3;
    }
    return result;
}

static rt_err_t supplicant_handle_message3(
    struct rt_wlan_offload_supplicant *supplicant,
    const rt_uint8_t *frame,
    rt_size_t length,
    rt_uint8_t gtk[WPA_GTK_MAX_LENGTH],
    rt_uint8_t *gtk_length,
    rt_uint8_t *gtk_index,
    rt_uint8_t igtk[WPA_IGTK_LENGTH],
    rt_uint8_t *igtk_length,
    rt_uint8_t *igtk_index,
    rt_uint8_t igtk_sequence[6],
    rt_uint8_t **response,
    rt_size_t *response_length)
{
    rt_uint16_t key_info = supplicant_get_be16(frame + 5);
    rt_err_t result;

    if (!supplicant_verify_mic(supplicant, frame, length))
    {
        LOG_W("WPA message 3 MIC mismatch");
        return -RT_ERROR;
    }
    result = RT_EOK;
    if (supplicant->profile->protocol == WLAN_OFFLOAD_WPA_PROTOCOL_RSN)
    {
        result = supplicant_extract_gtk(supplicant, frame, length, gtk,
                                        gtk_length, gtk_index, igtk,
                                        igtk_length, igtk_index,
                                        igtk_sequence);
        if (result != RT_EOK && result != -RT_EEMPTY)
        {
            return result;
        }
        if (supplicant->profile->mfp_required && !*igtk_length)
        {
            return -RT_EIO;
        }
    }
    result = supplicant_make_key_frame(
        supplicant,
        (key_info & WPA_KEY_INFO_VERSION_MASK) |
        WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_MIC |
        (supplicant->profile->protocol == WLAN_OFFLOAD_WPA_PROTOCOL_RSN ?
         WPA_KEY_INFO_SECURE : 0),
        supplicant->key_length,
        frame + 9, RT_NULL, RT_NULL, 0, response, response_length);
    if (result == RT_EOK)
    {
        rt_memcpy(supplicant->replay, frame + 9, WPA_REPLAY_LENGTH);
        supplicant->state = WLAN_OFFLOAD_SUPPLICANT_INSTALLING;
    }
    return result;
}

static rt_err_t supplicant_handle_group_message1(
    struct rt_wlan_offload_supplicant *supplicant,
    const rt_uint8_t *frame,
    rt_size_t length,
    rt_uint8_t gtk[WPA_GTK_MAX_LENGTH],
    rt_uint8_t *gtk_length,
    rt_uint8_t *gtk_index,
    rt_uint8_t igtk[WPA_IGTK_LENGTH],
    rt_uint8_t *igtk_length,
    rt_uint8_t *igtk_index,
    rt_uint8_t igtk_sequence[6],
    rt_uint8_t **response,
    rt_size_t *response_length)
{
    rt_uint16_t key_info = supplicant_get_be16(frame + 5);
    rt_err_t result;

    if (!supplicant_verify_mic(supplicant, frame, length))
    {
        return -RT_ERROR;
    }
    result = supplicant_extract_gtk(supplicant, frame, length, gtk,
                                    gtk_length, gtk_index, igtk,
                                    igtk_length, igtk_index,
                                    igtk_sequence);
    if (result != RT_EOK || !*gtk_length)
    {
        return result == RT_EOK ? -RT_EIO : result;
    }
    return supplicant_make_key_frame(
        supplicant,
        (key_info & WPA_KEY_INFO_VERSION_MASK) |
        (supplicant->profile->protocol == WLAN_OFFLOAD_WPA_PROTOCOL_WPA ?
         key_info & WPA_KEY_INFO_INDEX_MASK : 0) |
        WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE,
        supplicant_get_be16(frame + 7),
        frame + 9, RT_NULL, RT_NULL, 0, response, response_length);
}

rt_bool_t rt_wlan_offload_supplicant_handle_eapol(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    const rt_uint8_t source[6],
    const rt_uint8_t destination[6],
    const rt_uint8_t *data,
    rt_size_t length,
    rt_err_t *result)
{
    struct rt_wlan_offload_supplicant *supplicant;
    rt_uint16_t key_info;
    rt_uint16_t body_length;
    rt_uint16_t key_data_length;
    rt_uint8_t gtk[WPA_GTK_MAX_LENGTH] = {0};
    rt_uint8_t igtk[WPA_IGTK_LENGTH] = {0};
    rt_uint8_t pairwise_key[WPA_TKIP_KEY_LENGTH] = {0};
    rt_uint8_t pairwise_key_length = 0;
    rt_uint8_t gtk_length = 0;
    rt_uint8_t gtk_index = 0;
    rt_uint8_t igtk_length = 0;
    rt_uint8_t igtk_index = 0;
    rt_uint8_t key_rsc[6] = {0};
    rt_uint8_t igtk_sequence[6] = {0};
    rt_uint8_t *response = RT_NULL;
    rt_size_t response_length = 0;
    rt_bool_t install_pairwise = RT_FALSE;
    rt_bool_t complete = RT_FALSE;
    rt_bool_t group_rekey = RT_FALSE;
    rt_bool_t initial_group = RT_FALSE;
    rt_bool_t wait_group = RT_FALSE;
    enum rt_wlan_offload_cipher pairwise_cipher = RT_WLAN_OFFLOAD_CIPHER_NONE;
    enum rt_wlan_offload_cipher group_cipher = RT_WLAN_OFFLOAD_CIPHER_NONE;
    enum wlan_offload_wpa_protocol protocol = WLAN_OFFLOAD_WPA_PROTOCOL_RSN;
    rt_uint32_t connect_request_id = 0;
    rt_uint8_t bssid[6] = {0};
    rt_err_t status = RT_EOK;

    if (result)
    {
        *result = RT_EOK;
    }
    if (!radio || iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION || !source ||
        !destination || !data ||
        length < EAPOL_KEY_FIXED_LENGTH || data[1] != EAPOL_TYPE_KEY ||
        (data[4] != EAPOL_KEY_DESCRIPTOR_RSN &&
         data[4] != EAPOL_KEY_DESCRIPTOR_WPA))
    {
        return RT_FALSE;
    }
    body_length = supplicant_get_be16(data + 2);
    key_data_length = supplicant_get_be16(data + 97);
    if ((rt_size_t)body_length + 4U > length || body_length < 95U ||
        key_data_length > (rt_size_t)body_length - 95U)
    {
        return RT_FALSE;
    }
    length = (rt_size_t)body_length + 4U;
    key_info = supplicant_get_be16(data + 5);
    if (((key_info & WPA_KEY_INFO_VERSION_MASK) != WPA_KEY_INFO_VERSION_AKM &&
         (key_info & WPA_KEY_INFO_VERSION_MASK) != WPA_KEY_INFO_VERSION_1 &&
         (key_info & WPA_KEY_INFO_VERSION_MASK) != WPA_KEY_INFO_VERSION_2 &&
         (key_info & WPA_KEY_INFO_VERSION_MASK) != WPA_KEY_INFO_VERSION_3) ||
        (key_info & (WPA_KEY_INFO_ERROR | WPA_KEY_INFO_REQUEST)))
    {
        return RT_FALSE;
    }

    supplicant = supplicant_lock_from_radio(radio);
    if (!supplicant)
    {
        return RT_FALSE;
    }
    if (supplicant->state == WLAN_OFFLOAD_SUPPLICANT_IDLE)
    {
        rt_mutex_release(&supplicant->lock);
        return RT_FALSE;
    }
    if (!supplicant->profile ||
        data[4] != (supplicant->profile->protocol == WLAN_OFFLOAD_WPA_PROTOCOL_RSN ?
                   EAPOL_KEY_DESCRIPTOR_RSN : EAPOL_KEY_DESCRIPTOR_WPA) ||
        ((key_info & WPA_KEY_INFO_VERSION_MASK) !=
             supplicant_cipher_descriptor_version(supplicant->profile) &&
         !(!(key_info & WPA_KEY_INFO_PAIRWISE) &&
           supplicant->profile->pairwise_cipher == RT_WLAN_OFFLOAD_CIPHER_CCMP &&
           supplicant->profile->group_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP &&
           (key_info & WPA_KEY_INFO_VERSION_MASK) ==
               WPA_KEY_INFO_VERSION_1)))
    {
        rt_mutex_release(&supplicant->lock);
        return RT_FALSE;
    }
    pairwise_cipher = supplicant->profile->pairwise_cipher;
    group_cipher = supplicant->profile->group_cipher;
    protocol = supplicant->profile->protocol;
    if (!supplicant_mac_is_zero(supplicant->bssid) &&
        rt_memcmp(source, supplicant->bssid, 6) != 0)
    {
        rt_mutex_release(&supplicant->lock);
        return RT_FALSE;
    }
    if (rt_memcmp(destination, supplicant->own_address, 6) != 0)
    {
        rt_mutex_release(&supplicant->lock);
        return RT_FALSE;
    }
    if (supplicant_mac_is_zero(supplicant->bssid))
    {
        rt_memcpy(supplicant->bssid, source, 6);
    }

    if ((key_info & (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_ACK |
                     WPA_KEY_INFO_MIC)) ==
        (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_ACK) &&
        (supplicant->state == WLAN_OFFLOAD_SUPPLICANT_ASSOCIATING ||
         supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_M1 ||
         (supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_M3 &&
          supplicant_replay_compare(data + 9, supplicant->replay) >= 0)))
    {
        status = supplicant_handle_message1(supplicant, data, length,
                                             &response, &response_length);
        if (status == RT_EOK)
        {
            LOG_I("received %s message 1/4; sending message 2/4",
                  supplicant->profile->name);
        }
    }
    else if ((key_info & (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_ACK |
                          WPA_KEY_INFO_MIC)) ==
             (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_ACK |
              WPA_KEY_INFO_MIC) &&
             supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_M3 &&
             supplicant_replay_compare(data + 9, supplicant->replay) >= 0)
    {
        rt_memcpy(key_rsc, data + 65, sizeof(key_rsc));
        status = supplicant_handle_message3(supplicant, data, length,
                                             gtk, &gtk_length, &gtk_index,
                                             igtk, &igtk_length, &igtk_index,
                                             igtk_sequence,
                                             &response, &response_length);
        install_pairwise = status == RT_EOK;
        if (status == RT_EOK)
        {
            pairwise_key_length = supplicant_cipher_key_length(
                pairwise_cipher);
            rt_memcpy(pairwise_key, supplicant->ptk + WPA_TK_OFFSET,
                      pairwise_key_length);
            if (pairwise_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP &&
                pairwise_key_length == WPA_TKIP_KEY_LENGTH)
            {
                rt_uint8_t mic[8];

                rt_memcpy(mic, pairwise_key + 16, sizeof(mic));
                rt_memcpy(pairwise_key + 16, pairwise_key + 24,
                          sizeof(mic));
                rt_memcpy(pairwise_key + 24, mic, sizeof(mic));
                rt_wlan_offload_crypto_zero(mic, sizeof(mic));
            }
            rt_memcpy(bssid, supplicant->bssid, sizeof(bssid));
            rt_timer_stop(supplicant->timer);
            LOG_I("received %s message 3/4; installing keys",
                  supplicant->profile->name);
        }
    }
    else if ((key_info & (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_ACK |
                          WPA_KEY_INFO_MIC)) ==
             (WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_ACK |
              WPA_KEY_INFO_MIC) &&
             (supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_G1 ||
              supplicant->state == WLAN_OFFLOAD_SUPPLICANT_CONNECTED) &&
             supplicant_replay_compare(data + 9, supplicant->replay) == 0 &&
             supplicant_verify_mic(supplicant, data, length))
    {
        status = supplicant_make_key_frame(
            supplicant,
            (key_info & WPA_KEY_INFO_VERSION_MASK) |
            WPA_KEY_INFO_PAIRWISE | WPA_KEY_INFO_MIC |
            (protocol == WLAN_OFFLOAD_WPA_PROTOCOL_RSN ?
             WPA_KEY_INFO_SECURE : 0),
            supplicant_get_be16(data + 7),
            data + 9, RT_NULL, RT_NULL, 0, &response, &response_length);
    }
    /* Group message 1 must carry a strictly greater replay counter. The
     * authenticator increments the counter for every EAPOL-Key frame it
     * sends, so an equal counter can only be a replay of a frame this
     * station already accepted. Its MIC still verifies, so accepting it
     * would reinstall the same GTK and reset the group replay counter in
     * the firmware, letting an attacker resend captured broadcast and
     * multicast frames. The pairwise branches above are already safe: a
     * replayed message 3 no longer matches WAIT_M3 and is answered by the
     * preceding branch without reinstalling the PTK. */
    else if (!(key_info & WPA_KEY_INFO_PAIRWISE) &&
             (key_info & (WPA_KEY_INFO_ACK | WPA_KEY_INFO_MIC |
                          WPA_KEY_INFO_SECURE)) ==
             (WPA_KEY_INFO_ACK | WPA_KEY_INFO_MIC |
              WPA_KEY_INFO_SECURE) &&
             (supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_G1 ||
              supplicant->state == WLAN_OFFLOAD_SUPPLICANT_CONNECTED) &&
             supplicant_replay_compare(data + 9, supplicant->replay) > 0)
    {
        initial_group =
            supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_G1;
        rt_memcpy(bssid, supplicant->bssid, sizeof(bssid));
        rt_memcpy(key_rsc, data + 65, sizeof(key_rsc));
        status = supplicant_handle_group_message1(
            supplicant, data, length, gtk, &gtk_length, &gtk_index,
            igtk, &igtk_length, &igtk_index, igtk_sequence,
            &response, &response_length);
        group_rekey = status == RT_EOK;
    }
    else
    {
        rt_mutex_release(&supplicant->lock);
        return RT_TRUE;
    }
    rt_mutex_release(&supplicant->lock);

    if (status == RT_EOK && gtk_length)
    {
        if (group_cipher == RT_WLAN_OFFLOAD_CIPHER_TKIP &&
            gtk_length == WPA_TKIP_KEY_LENGTH)
        {
            rt_uint8_t mic[8];

            rt_memcpy(mic, gtk + 16, sizeof(mic));
            rt_memcpy(gtk + 16, gtk + 24, sizeof(mic));
            rt_memcpy(gtk + 24, mic, sizeof(mic));
            rt_wlan_offload_crypto_zero(mic, sizeof(mic));
        }
        status = supplicant_install_key(
                                        radio, RT_FALSE,
                                        group_cipher,
                                        gtk_index,
                                        gtk, gtk_length, key_rsc, RT_NULL);
        if (status == RT_EOK)
        {
            rt_uint32_t request_id =
                rt_wlan_offload_alloc_request_id(radio);

            status = request_id ?
                rt_wlan_offload_set_default_key(radio,
                                           RT_WLAN_OFFLOAD_IFTYPE_STATION,
                                           request_id, gtk_index,
                                           RT_FALSE, RT_TRUE) : -RT_EBUSY;
        }
    }
    if (status == RT_EOK && install_pairwise)
    {
        status = supplicant_install_key(
                                        radio, RT_TRUE,
                                        pairwise_cipher, 0,
                                        pairwise_key,
                                        pairwise_key_length, RT_NULL,
                                        bssid);
    }
    if (status == RT_EOK && igtk_length)
    {
        status = supplicant_install_key(
            radio, RT_FALSE, RT_WLAN_OFFLOAD_CIPHER_AES_CMAC,
            igtk_index, igtk, igtk_length, igtk_sequence, RT_NULL);
    }
    if (status == RT_EOK && response)
    {
        status = rt_wlan_offload_transmit_eapol(
            radio, RT_WLAN_OFFLOAD_IFTYPE_STATION,
            source, response, response_length);
    }
    if (status == RT_EOK &&
        ((install_pairwise && protocol == WLAN_OFFLOAD_WPA_PROTOCOL_RSN) ||
         (group_rekey && initial_group)))
    {
        status = supplicant_set_authorized(radio, bssid, RT_TRUE);
    }
    if (status == RT_EOK && install_pairwise)
    {
        supplicant = supplicant_lock_from_radio(radio);
        if (supplicant &&
            supplicant->state == WLAN_OFFLOAD_SUPPLICANT_INSTALLING)
        {
            rt_memcpy(supplicant->replay, data + 9, WPA_REPLAY_LENGTH);
            if (protocol == WLAN_OFFLOAD_WPA_PROTOCOL_WPA)
            {
                supplicant->state = WLAN_OFFLOAD_SUPPLICANT_WAIT_G1;
                rt_timer_stop(supplicant->timer);
                status = rt_timer_start(supplicant->timer);
                wait_group = status == RT_EOK;
            }
            else
            {
                supplicant->state = WLAN_OFFLOAD_SUPPLICANT_CONNECTED;
                connect_request_id = supplicant->request_id;
                rt_memcpy(bssid, supplicant->bssid, sizeof(bssid));
                supplicant->request_id = 0;
                rt_timer_stop(supplicant->timer);
                complete = RT_TRUE;
            }
        }
        if (supplicant)
        {
            rt_mutex_release(&supplicant->lock);
        }
    }
    else if (status == RT_EOK && group_rekey)
    {
        supplicant = supplicant_lock_from_radio(radio);
        if (supplicant)
        {
            rt_memcpy(supplicant->replay, data + 9, WPA_REPLAY_LENGTH);
            if (initial_group &&
                supplicant->state == WLAN_OFFLOAD_SUPPLICANT_WAIT_G1)
            {
                supplicant->state = WLAN_OFFLOAD_SUPPLICANT_CONNECTED;
                connect_request_id = supplicant->request_id;
                rt_memcpy(bssid, supplicant->bssid, sizeof(bssid));
                supplicant->request_id = 0;
                rt_timer_stop(supplicant->timer);
                complete = RT_TRUE;
            }
            rt_mutex_release(&supplicant->lock);
        }
    }

    if (response)
    {
        rt_wlan_offload_crypto_zero(response, response_length);
        rt_free(response);
    }
    rt_wlan_offload_crypto_zero(gtk, sizeof(gtk));
    rt_wlan_offload_crypto_zero(igtk, sizeof(igtk));
    rt_wlan_offload_crypto_zero(pairwise_key, sizeof(pairwise_key));
    if (status != RT_EOK && (install_pairwise || initial_group))
    {
        LOG_E("WPA key installation/message 4 failed: %d", status);
        supplicant_fail(radio, status);
    }
    else if (wait_group)
    {
        LOG_I("WPA pairwise handshake complete; waiting for group key");
    }
    else if (complete)
    {
        LOG_I("WPA four-way handshake complete");
        supplicant_report_result(radio, connect_request_id,
                                 RT_EOK, bssid);
    }
    if (result)
    {
        *result = status;
    }
    return RT_TRUE;
}
