/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "aic8800_wifi.h"

#include <wlan_offload_control.h>
#ifdef AIC8800_WIFI_AUTO_START
#include <wlan_mgnt.h>
#endif
#ifdef AIC8800_WIFI_DEBUG_STATS
#include <finsh.h>
#endif

#define DBG_TAG "aic8800.wifi"
#define DBG_LVL AIC8800_DBG_LVL
#include <rtdbg.h>

#define AIC_RX_FLAG_AMSDU                  (1UL << 0)
#define AIC_RX_FLAG_80211_MPDU             (1UL << 1)
#define AIC_RX_FLAG_NEEDS_REORDER           (1UL << 5)
#define AIC_TX_FLAG_MORE_DATA              (1U << 2)
#define AIC_TX_FLAG_MANAGEMENT             (1U << 3)
#define AIC_TX_FLAG_EOSP                   (1U << 9)
#define AIC_TX_STATUS_DESCRIPTOR_REQUEST    (1UL << 31)
#define AIC_TX_STATUS_ACKNOWLEDGED           (1UL << 3)
#define AIC_ETHERTYPE_ARP                     0x0806U
#define AIC_ETHERTYPE_EAPOL                   0x888eU
#define AIC_ETHERTYPE_IPV4                    0x0800U
#define AIC_ETHERTYPE_IPV6                    0x86ddU
#define AIC_ETHERTYPE_VLAN                    0x8100U
#define AIC_ETHERTYPE_VLAN_PROVIDER           0x88a8U
#define AIC_IP_PROTOCOL_TCP                         6U
#define AIC_IP_PROTOCOL_UDP                        17U
#define AIC_IPV6_NEXT_HEADER_ICMP                  58U
#define AIC_TCP_FLAG_SYN                         0x02U
#define AIC_TCP_FLAG_RST                         0x04U
#define AIC_DHCP_SERVER_PORT                       67U
#define AIC_DHCP_CLIENT_PORT                       68U
#define AIC_DHCPV6_CLIENT_PORT                    546U
#define AIC_DHCPV6_SERVER_PORT                    547U
#define AIC_TX_ACCESS_CATEGORY_BACKGROUND          0U
#define AIC_TX_ACCESS_CATEGORY_BEST_EFFORT        1U
#define AIC_TX_ACCESS_CATEGORY_VIDEO              2U
#define AIC_TX_ACCESS_CATEGORY_VOICE              3U
#define AIC_TX_TID_BEST_EFFORT                    0U
#define AIC_TX_TID_NON_QOS                     0xffU
#define AIC_PS_ID_LEGACY                         0U
#define AIC_PS_ID_UAPSD                          1U
#define AIC_UAPSD_TIDS_VO          ((1U << 6) | (1U << 7))
#define AIC_UAPSD_TIDS_VI          ((1U << 4) | (1U << 5))
#define AIC_UAPSD_TIDS_BK          ((1U << 1) | (1U << 2))
#define AIC_UAPSD_TIDS_BE          ((1U << 0) | (1U << 3))

#define AIC_CONNECTION_CONTROL_PORT_HOST   (1UL << 0)
#define AIC_CONNECTION_CONTROL_PORT_NO_ENC (1UL << 1)
#define AIC_CONNECTION_DISABLE_HT          (1UL << 2)
#define AIC_CONNECTION_WPA                 (1UL << 3)
#define AIC_CONNECTION_MFP                 (1UL << 4)
#define AIC_UAPSD_QUEUE_VO                  (1U << 0)
#define AIC_UAPSD_TIMEOUT_MS                300U
#define AIC_LP_CLOCK_ACCURACY_PPM           20U
#define AIC_DC_RUNTIME_RX_GAIN_ADDRESS      0x4033b300U
#define AIC_DC_RUNTIME_RX_GAIN_MASK         0xffU
#define AIC_DC_RUNTIME_RX_GAIN_VALUE        0x0eU

#define AIC_SCAN_CHANNEL_COUNT     AIC_WIRE_SCAN_CHANNEL_COUNT
#define AIC_SCAN_SSID_COUNT        AIC_WIRE_SCAN_SSID_COUNT
#define AIC_SCAN_IE_MAX            AIC_WIRE_SCAN_IE_MAX
#define AIC_SCAN_REQUEST_SIZE      sizeof(struct aic_wire_scanu_start_req)
#define AIC_SCAN_FOLLOWUP_RETRY_MS                         20U
#define AIC_SCAN_FOLLOWUP_MAX_RETRIES                      50U
#define AIC_SCAN_RESULT_DRAIN_RETRY_MS                     10U
#define AIC_SCAN_RESULT_DRAIN_MAX_RETRIES                  20U
/* Associations below 80 MHz on an 80 MHz-or-wider AP before the driver stops
 * believing the modem's 80 MHz claim.  See aic_update_connected_channel(). */
#define AIC_BANDWIDTH_80_FAILURES                           2U
#define AIC_CHANNEL_DEFAULT_POWER_DBM                      20
#define AIC_TRAFFIC_IND_RETRY_MS                           20U
#define AIC_PS_SP_INTERRUPTED                             0xffU
#define AIC_CONNECT_REQUEST_SIZE   sizeof(struct aic_wire_sm_connect_req)
#define AIC_ME_CONFIG_SIZE         sizeof(struct aic_wire_me_config_req)
#define AIC_ME_CHANNEL_CONFIG_SIZE \
    sizeof(struct aic_wire_me_channel_config_req)
#define AIC_TX_DESCRIPTOR_SIZE     sizeof(struct aic_wire_tx_host_descriptor)

/* struct sm_connect_ind is not packed.  With the vendor WLAN_OFFLOAD ABI
 * (u8_l/bool_l are one byte), the negotiated channel fields are laid out as
 * follows after the 800-byte association IE buffer. */
#define AIC_SM_CONNECT_IND_BAND_OFFSET       822U
#define AIC_SM_CONNECT_IND_FREQUENCY_OFFSET  824U
#define AIC_SM_CONNECT_IND_WIDTH_OFFSET      826U
#define AIC_SM_CONNECT_IND_CENTER1_OFFSET    828U
#define AIC_SM_CONNECT_IND_CENTER2_OFFSET    832U
#define AIC_SM_CONNECT_IND_CHANNEL_END       836U
/* sm_connect_ind: status(2) bssid(6) roamed(1) vif_idx(1) ap_idx(1) ch_idx(1) */
#define AIC_SM_CONNECT_IND_STATION_OFFSET     10U
#define AIC_SM_CONNECT_IND_CHANNEL_INDEX_OFFSET 11U
#define AIC_SM_CONNECT_IND_QOS_OFFSET         12U
#define AIC_SM_CONNECT_IND_ACM_OFFSET         13U
#define AIC_SM_CONNECT_IND_REQ_IE_LEN_OFFSET  14U
#define AIC_SM_CONNECT_IND_RSP_IE_LEN_OFFSET  16U
#define AIC_SM_CONNECT_IND_IE_BUFFER_OFFSET   20U
#define AIC_SM_CONNECT_IND_IE_BUFFER_SIZE    800U
#define AIC_WLAN_EID_HT_OPERATION             61U
#define AIC_WLAN_EID_VHT_OPERATION           192U

static void aic_scan_work(struct rt_work *work, void *work_data);
static void aic_ap_rechannel_work(struct rt_work *work, void *work_data);
static void aic_traffic_work(struct rt_work *work, void *work_data);
static void aic_station_loss_work(struct rt_work *work, void *work_data);
static void aic_set_traffic_status(struct aic8800_context *context,
                                   rt_uint8_t station_index,
                                   rt_uint8_t ps_id,
                                   rt_bool_t available);
static void aic_report_scan_done_status(struct aic8800_context *context,
                                        rt_err_t status,
                                        rt_bool_t result_count_valid,
                                        rt_uint16_t result_count);
static rt_err_t aic_confirmation_status(const rt_uint8_t *confirmation,
                                        rt_size_t length);
static rt_bool_t aic_device_supports_vht(
    const struct aic8800_context *context);
static rt_bool_t aic_device_supports_5ghz(
    const struct aic8800_context *context);
static rt_bool_t aic_channel_allowed(
    const struct aic8800_context *context, enum rt_wlan_offload_band_id band,
    rt_uint16_t channel);
static void aic_refresh_channel_metadata(struct aic8800_context *context);
static rt_err_t aic_send_me_config(struct aic8800_context *context);
static rt_uint16_t aic8800_protocol_version(
    const struct aic8800_context *context);
static void aic_clear_hardware_keys(struct aic8800_context *context,
                                    rt_bool_t notify_firmware);
static void aic_cancel_mgmt_confirmations(struct aic8800_context *context,
                                          rt_err_t status);
static rt_err_t aic_del_station(struct rt_wlan_offload_vif *vif,
                                rt_uint32_t request_id,
                                const rt_uint8_t mac[6], rt_uint16_t reason);
static rt_err_t aic_del_station_internal(
    struct rt_wlan_offload_vif *vif, rt_uint32_t request_id,
    const rt_uint8_t mac[6], rt_uint16_t reason,
    rt_bool_t firmware_station_lost);
static rt_err_t aic_add_key(struct rt_wlan_offload_vif *vif,
                            rt_uint32_t request_id,
                            const struct rt_wlan_offload_key *key);
static rt_err_t aic_delete_key(struct rt_wlan_offload_vif *vif,
                               rt_uint32_t request_id, rt_uint8_t index,
                               rt_bool_t pairwise, const rt_uint8_t peer[6]);
static rt_err_t aic_set_control_port(struct aic8800_context *context,
                                     rt_uint8_t station_index,
                                     rt_bool_t open);
static struct aic8800_ap_station *aic_find_ap_station(
    struct aic8800_context *context, const rt_uint8_t address[6]);
static struct aic8800_hardware_key *aic_find_hardware_key(
    struct aic8800_context *context, enum rt_wlan_offload_iftype iftype,
    rt_uint8_t index, rt_bool_t pairwise, const rt_uint8_t peer[6],
    rt_bool_t allocate);
static rt_err_t aic_start_ap_firmware(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_ap_settings *settings);
static rt_err_t aic_stop_ap_firmware(struct aic8800_context *context);
static void aic_clear_saved_ap(struct aic8800_context *context);
static void aic_schedule_ap_resume(struct aic8800_context *context,
                                   rt_bool_t use_station_channel);
static rt_err_t aic_deliver_ethernet(struct aic8800_context *context,
                                     const rt_uint8_t *record,
                                     rt_size_t length);
static void aic_rx_reorder_reset(struct aic8800_context *context);
static rt_err_t aic_rx_reorder_init(struct aic8800_context *context);
static void aic_rx_reorder_deinit(struct aic8800_context *context);
#ifdef AIC8800_WIFI_TCP_ACK_FILTER
static void aic_tcp_ack_reset(struct aic8800_context *context);
static rt_err_t aic_transmit_frame(struct aic8800_context *context,
                                   struct rt_wlan_offload_vif *vif,
                                   const rt_uint8_t *data, rt_size_t length,
                                   rt_bool_t filter_tcp_ack,
                                   rt_bool_t management,
                                   rt_uint32_t status_descriptor);
#else
#define aic_tcp_ack_reset(context) ((void)(context))
#endif
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
static rt_err_t aic_sdio_data_queue_init(struct aic8800_context *context);
static void aic_sdio_data_queue_deinit(struct aic8800_context *context);
#endif

static rt_bool_t aic_firmware_uses_compact_features(
    const struct aic8800_context *context, rt_uint32_t raw_features)
{
    rt_bool_t compact_umac =
        (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_UMAC_BIT)) != 0;
    rt_bool_t full_umac =
        (raw_features & (1UL << AIC_MM_FULL_FEATURE_UMAC_BIT)) != 0;
    rt_bool_t compact_he =
        (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_HE_BIT)) != 0;
    rt_bool_t full_he =
        (raw_features & (1UL << AIC_MM_FULL_FEATURE_HE_BIT)) != 0;

    /* UMAC must be present in FullMAC firmware, so its shifted position is the
     * strongest layout discriminator.  HE resolves firmware that reports an
     * incomplete UMAC feature set. */
    if (compact_umac != full_umac)
    {
        return compact_umac;
    }
    if (compact_he != full_he)
    {
        return compact_he;
    }
    return context && context->transport == AIC8800_TRANSPORT_SDIO;
}

static rt_uint32_t aic_decode_firmware_features(
    const struct aic8800_context *context, rt_uint32_t raw_features,
    rt_bool_t *compact_map)
{
    rt_uint32_t features = 0;
    rt_bool_t compact = aic_firmware_uses_compact_features(
        context, raw_features);

    if (compact)
    {
        if (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_PS_BIT))
        {
            features |= AIC_FW_CAP_PS;
        }
        if (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_VHT_BIT))
        {
            features |= AIC_FW_CAP_VHT;
        }
        if (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_BFMEE_BIT))
        {
            features |= AIC_FW_CAP_BFMEE;
        }
        if (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_MFP_BIT))
        {
            features |= AIC_FW_CAP_MFP;
        }
        if (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_MU_MIMO_RX_BIT))
        {
            features |= AIC_FW_CAP_MU_MIMO_RX;
        }
        if (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_ANT_DIV_BIT))
        {
            features |= AIC_FW_CAP_ANT_DIV;
        }
        if (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_MON_DATA_BIT))
        {
            features |= AIC_FW_CAP_MON_DATA;
        }
        if (raw_features & (1UL << AIC_MM_COMPACT_FEATURE_HE_BIT))
        {
            features |= AIC_FW_CAP_HE;
        }
    }
    else
    {
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_PS_BIT))
        {
            features |= AIC_FW_CAP_PS;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_DPSM_BIT))
        {
            features |= AIC_FW_CAP_DPSM;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_CHNL_CTXT_BIT))
        {
            features |= AIC_FW_CAP_CHNL_CTXT;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_VHT_BIT))
        {
            features |= AIC_FW_CAP_VHT;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_BFMEE_BIT))
        {
            features |= AIC_FW_CAP_BFMEE;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_MFP_BIT))
        {
            features |= AIC_FW_CAP_MFP;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_MU_MIMO_RX_BIT))
        {
            features |= AIC_FW_CAP_MU_MIMO_RX;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_ANT_DIV_BIT))
        {
            features |= AIC_FW_CAP_ANT_DIV;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_MON_DATA_BIT))
        {
            features |= AIC_FW_CAP_MON_DATA;
        }
        if (raw_features & (1UL << AIC_MM_FULL_FEATURE_HE_BIT))
        {
            features |= AIC_FW_CAP_HE;
        }
    }
    if (compact_map)
    {
        *compact_map = compact;
    }
    return features;
}

static rt_int8_t aic_hex_digit(rt_uint8_t value)
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

/* RT-Thread's WLAN shell passes WEP keys as text.  Accept both the Linux
 * convention of five/thirteen byte ASCII keys and the ten/twenty-six byte
 * hexadecimal form used by the existing WLAN tools. */
static rt_bool_t aic_decode_wep_key(const rt_wlan_key_t *input,
                                    rt_uint8_t output[13],
                                    rt_size_t *output_length)
{
    rt_size_t index;
    rt_size_t decoded_length;

    if (!input || !output || !output_length)
    {
        return RT_FALSE;
    }
    if (input->len == 5U || input->len == 13U)
    {
        rt_memcpy(output, input->val, input->len);
        *output_length = input->len;
        return RT_TRUE;
    }
    if (input->len != 10U && input->len != 26U)
    {
        return RT_FALSE;
    }
    decoded_length = input->len / 2U;
    for (index = 0; index < decoded_length; index++)
    {
        rt_int8_t high = aic_hex_digit(input->val[index * 2U]);
        rt_int8_t low = aic_hex_digit(input->val[index * 2U + 1U]);

        if (high < 0 || low < 0)
        {
            return RT_FALSE;
        }
        output[index] = (rt_uint8_t)((high << 4) | low);
    }
    *output_length = decoded_length;
    return RT_TRUE;
}

static rt_bool_t aic_security_supported(rt_wlan_security_t security)
{
    switch (security)
    {
    case SECURITY_OPEN:
    case SECURITY_WEP_PSK:
    case SECURITY_WEP_SHARED:
    case SECURITY_WPA_TKIP_PSK:
    case SECURITY_WPA_AES_PSK:
    case SECURITY_WPA2_TKIP_PSK:
    case SECURITY_WPA2_AES_PSK:
    case SECURITY_WPA2_MIXED_PSK:
    case SECURITY_WPA2_AES_CMAC:
    case SECURITY_WPA_WPA2_MIXED_PSK:
    case SECURITY_WPA2_AES_PSK_SHA256:
    case SECURITY_WPA3_AES_PSK:
    case SECURITY_WPA3_SAE:
    case SECURITY_WPA2_WPA3_MIXED_PSK:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static rt_bool_t aic_security_uses_host_supplicant(
    rt_wlan_security_t security)
{
    if (security == SECURITY_OPEN || security == SECURITY_UNKNOWN ||
        (security & (WEP_ENABLED | WAPI_ENABLED)))
    {
        return RT_FALSE;
    }
    return (security & (WPA_SECURITY | WPA2_SECURITY | WPA3_SECURITY |
                        OWE_ENABLED | FT_ENABLED | SHA256_ENABLED |
                        SHA384_ENABLED | FILS_ENABLED | DPP_ENABLED |
                        OSEN_ENABLED | CCKM_ENABLED |
                        IEEE_8021X_ENABLED)) != 0;
}

static const struct rt_wlan_offload_channel g_aic8800_channels_2ghz[] = {
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
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 12, 2467, RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 13, 2472, RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 14, 2484, RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
};

static const struct rt_wlan_offload_channel g_aic8800_channels_5ghz[] = {
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 36, 5180, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 40, 5200, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 44, 5220, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 48, 5240, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 52, 5260, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 56, 5280, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 60, 5300, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 64, 5320, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 100, 5500, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 104, 5520, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 108, 5540, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 112, 5560, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 116, 5580, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 120, 5600, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 124, 5620, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 128, 5640, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 132, 5660, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 136, 5680, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 140, 5700, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 144, 5720, RT_WLAN_OFFLOAD_CHANNEL_RADAR | RT_WLAN_OFFLOAD_CHANNEL_NO_IR, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 149, 5745, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 153, 5765, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 157, 5785, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 161, 5805, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 165, 5825, 0, 20},
};

static const struct rt_wlan_offload_rate g_aic8800_rates_2ghz[] = {
    {10, 0, 0}, {20, 1, 0}, {55, 2, 0}, {110, 3, 0},
    {60, 4, 0}, {90, 5, 0}, {120, 6, 0}, {180, 7, 0},
    {240, 8, 0}, {360, 9, 0}, {480, 10, 0}, {540, 11, 0},
};

static const struct rt_wlan_offload_supported_band g_aic8800_band_2ghz = {
    .id = RT_WLAN_OFFLOAD_BAND_2GHZ,
    .phy_capabilities = RT_WLAN_OFFLOAD_PHY_11B | RT_WLAN_OFFLOAD_PHY_11G |
                        RT_WLAN_OFFLOAD_PHY_HT,
    .channels = g_aic8800_channels_2ghz,
    .channel_count = sizeof(g_aic8800_channels_2ghz) /
                     sizeof(g_aic8800_channels_2ghz[0]),
    .rates = g_aic8800_rates_2ghz,
    .rate_count = sizeof(g_aic8800_rates_2ghz) /
                  sizeof(g_aic8800_rates_2ghz[0]),
    .max_spatial_streams = 1,
    /* 2.4 GHz operation is limited to HT20/HT40. */
    .max_channel_width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40,
    .max_channel_width_set = RT_TRUE,
};

static const struct rt_wlan_offload_supported_band g_aic8800_band_5ghz = {
    .id = RT_WLAN_OFFLOAD_BAND_5GHZ,
    .phy_capabilities = RT_WLAN_OFFLOAD_PHY_11A | RT_WLAN_OFFLOAD_PHY_HT,
    .channels = g_aic8800_channels_5ghz,
    .channel_count = sizeof(g_aic8800_channels_5ghz) /
                     sizeof(g_aic8800_channels_5ghz[0]),
    .rates = &g_aic8800_rates_2ghz[4],
    .rate_count = 8,
    .max_spatial_streams = 1,
    /* Conservative until MM_VERSION identifies an 80 MHz-capable product. */
    .max_channel_width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40,
    .max_channel_width_set = RT_TRUE,
};

static const enum rt_wlan_offload_cipher g_aic8800_ciphers[] = {
    RT_WLAN_OFFLOAD_CIPHER_WEP40,
    RT_WLAN_OFFLOAD_CIPHER_WEP104,
    RT_WLAN_OFFLOAD_CIPHER_TKIP,
    RT_WLAN_OFFLOAD_CIPHER_CCMP,
    RT_WLAN_OFFLOAD_CIPHER_AES_CMAC,
};

static const struct rt_wlan_offload_iface_limit g_aic8800_iface_limits[] = {
    {RT_WLAN_OFFLOAD_IFTYPE_BIT(RT_WLAN_OFFLOAD_IFTYPE_STATION), 1},
    {RT_WLAN_OFFLOAD_IFTYPE_BIT(RT_WLAN_OFFLOAD_IFTYPE_AP), 1},
};

static const struct rt_wlan_offload_iface_combination g_aic8800_iface_combinations[] = {
    {
        .limits = g_aic8800_iface_limits,
        .limit_count = 2,
        .max_interfaces = 2,
        .num_different_channels = 1,
    },
};

static rt_uint16_t aic_get_le16(const void *data)
{
    const rt_uint8_t *bytes = data;
    return (rt_uint16_t)bytes[0] | ((rt_uint16_t)bytes[1] << 8);
}

static rt_uint16_t aic_record_payload_length(
    const struct aic8800_context *context, const void *record)
{
    rt_uint16_t length = aic_get_le16(record);

    /* SDIO firmware aggregates A-MSDUs larger than 4 KiB and its vendor
     * driver consumes the complete 16-bit length. USB reserves the upper
     * nibble of this field and therefore keeps the 12-bit wire limit. */
    return context && context->transport == AIC8800_TRANSPORT_SDIO ?
           length : (length & AIC_USB_LENGTH_MASK);
}

static rt_uint32_t aic_get_le32(const void *data)
{
    const rt_uint8_t *bytes = data;
    return (rt_uint32_t)bytes[0] | ((rt_uint32_t)bytes[1] << 8) |
           ((rt_uint32_t)bytes[2] << 16) | ((rt_uint32_t)bytes[3] << 24);
}

#ifdef AIC8800_WIFI_DEBUG_STATS
static rt_uint16_t aic_network_checksum(const rt_uint8_t *data,
                                        rt_size_t length)
{
    rt_uint32_t sum = 0;

    while (length >= 2U)
    {
        sum += ((rt_uint16_t)data[0] << 8) | data[1];
        data += 2;
        length -= 2U;
    }
    if (length)
    {
        sum += (rt_uint16_t)data[0] << 8;
    }
    while (sum >> 16)
    {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (rt_uint16_t)~sum;
}

static void aic_validate_icmp_frame(struct aic8800_context *context,
                                    const rt_uint8_t *frame,
                                    rt_size_t length)
{
    const rt_uint8_t *ip;
    rt_size_t header_length;
    rt_size_t total_length;

    if (!context || !frame || length < 14U + 20U)
    {
        if (context)
        {
            AIC8800_STAT(context->icmp_tx_malformed_count++);
        }
        return;
    }
    ip = frame + 14U;
    header_length = (rt_size_t)(ip[0] & 0x0fU) * 4U;
    total_length = ((rt_size_t)ip[2] << 8) | ip[3];
    if ((ip[0] >> 4) != 4U || header_length < 20U ||
        total_length < header_length + 4U ||
        total_length > length - 14U || ip[9] != 1U)
    {
        AIC8800_STAT(context->icmp_tx_malformed_count++);
        return;
    }
    if (aic_network_checksum(ip, header_length) != 0U)
    {
        AIC8800_STAT(context->icmp_tx_ip_checksum_error_count++);
    }
    if (aic_network_checksum(ip + header_length,
                             total_length - header_length) != 0U)
    {
        AIC8800_STAT(context->icmp_tx_checksum_error_count++);
    }
}
#endif

static void aic_put_le16(void *data, rt_uint16_t value)
{
    rt_uint8_t *bytes = data;
    bytes[0] = (rt_uint8_t)value;
    bytes[1] = (rt_uint8_t)(value >> 8);
}

static void aic_put_le32(void *data, rt_uint32_t value)
{
    rt_uint8_t *bytes = data;
    bytes[0] = (rt_uint8_t)value;
    bytes[1] = (rt_uint8_t)(value >> 8);
    bytes[2] = (rt_uint8_t)(value >> 16);
    bytes[3] = (rt_uint8_t)(value >> 24);
}

static rt_size_t aic_align4(rt_size_t value)
{
    return (value + 3U) & ~(rt_size_t)3U;
}

static rt_bool_t aic_mac_is_zero(const rt_uint8_t address[6])
{
    return !address || !(address[0] | address[1] | address[2] |
             address[3] | address[4] | address[5]);
}

static rt_bool_t aic_mac_valid(const rt_uint8_t address[6])
{
    return address && !(address[0] & 1U) && !aic_mac_is_zero(address);
}

static rt_uint16_t aic_frequency_to_channel(rt_uint16_t frequency)
{
    if (frequency == 2484)
    {
        return 14;
    }
    if (frequency >= 2412 && frequency <= 2472)
    {
        return (frequency - 2407) / 5;
    }
    if (frequency >= 5000 && frequency <= 5900)
    {
        return (frequency - 5000) / 5;
    }
    return 0;
}

static void aic_channel_definition(rt_uint16_t frequency,
                                   struct rt_wlan_offload_channel_definition *channel)
{
    rt_memset(channel, 0, sizeof(*channel));
    channel->band = frequency < 5000 ? RT_WLAN_OFFLOAD_BAND_2GHZ :
                                       RT_WLAN_OFFLOAD_BAND_5GHZ;
    channel->width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
    channel->primary_channel = aic_frequency_to_channel(frequency);
    channel->primary_frequency_mhz = frequency;
    channel->center_frequency1_mhz = frequency;
}

static rt_bool_t aic_channel_definition_equal(
    const struct rt_wlan_offload_channel_definition *left,
    const struct rt_wlan_offload_channel_definition *right)
{
    if (!left || !right || left->band != right->band ||
        left->width != right->width ||
        left->primary_channel != right->primary_channel ||
        left->primary_frequency_mhz != right->primary_frequency_mhz ||
        left->center_frequency1_mhz != right->center_frequency1_mhz ||
        left->center_frequency2_mhz != right->center_frequency2_mhz)
    {
        return RT_FALSE;
    }
    return RT_TRUE;
}

static rt_bool_t aic_channel_definition_same_primary(
    const struct rt_wlan_offload_channel_definition *left,
    const struct rt_wlan_offload_channel_definition *right)
{
    return left && right && left->band == right->band &&
           left->primary_channel == right->primary_channel &&
           left->primary_frequency_mhz == right->primary_frequency_mhz;
}

/* Firmware transmit credits: recorded, never enforced.
 *
 * The firmware does emit AIC_ME_TX_CREDITS_UPDATE_IND, carrying a signed offset
 * per RA/TID queue, and the message is decoded here so the values can be
 * inspected.  Do not gate transmit on them.  Vendor Linux of this generation
 * disables the whole mechanism - the decrement on push, the replenish from the
 * transmit confirmation, and the replenish from this very indication are all
 * compiled out (`#if 0` in rwnx_tx.c around txq->credits-- and cfm.credits, and
 * a commented-out `txq->credits += update` in rwnx_txq_credit_update).  Gating
 * on a counter the firmware does not maintain simply starves the link: doing so
 * drained the station VIF to zero and killed all routed traffic. */
static void aic_tx_credit_reset(struct aic8800_context *context,
                                rt_uint8_t station_index)
{
    rt_base_t level;

    if (!context || station_index >= AIC8800_STATION_SLOTS)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    context->tx_credits[station_index] = AIC8800_TX_INITIAL_CREDITS;
    rt_hw_interrupt_enable(level);
}

static void aic_tx_credit_update(struct aic8800_context *context,
                                 rt_uint8_t station_index, rt_int8_t offset)
{
    rt_base_t level;
    rt_int32_t credits;

    if (!context || station_index >= AIC8800_STATION_SLOTS)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    context->tx_credits_tracked = RT_TRUE;
    credits = (rt_int32_t)context->tx_credits[station_index] + offset;
    if (credits > AIC8800_TX_INITIAL_CREDITS)
    {
        credits = AIC8800_TX_INITIAL_CREDITS;
    }
    else if (credits < 0)
    {
        credits = 0;
    }
    context->tx_credits[station_index] = (rt_int16_t)credits;
    rt_hw_interrupt_enable(level);
}

/* Per-station transmit window.  A generation identifies one lifetime of a
 * firmware station slot, so a late completion can never release a slot owned by
 * a client which reused the same firmware index. */
static void aic_station_state_set(struct aic8800_context *context,
                                  rt_uint8_t station_index,
                                  rt_bool_t present, rt_bool_t power_save,
                                  rt_uint16_t uapsd_tids,
                                  rt_bool_t complete_station_add)
{
    rt_base_t level;
    rt_size_t ps_id;

    if (!context || station_index >= AIC8800_STATION_SLOTS)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    if (complete_station_add)
    {
        if (context->station_add_pending)
        {
            power_save = power_save ||
                         context->sta_add_power_save[station_index];
        }
        context->station_add_pending = RT_FALSE;
        rt_memset(context->sta_add_power_save, 0,
                  sizeof(context->sta_add_power_save));
    }
    context->sta_generation[station_index]++;
    if (!context->sta_generation[station_index])
    {
        context->sta_generation[station_index]++;
    }
    context->sta_present[station_index] = present;
    context->sta_power_save[station_index] = present && power_save;
    context->sta_uapsd_tids[station_index] = present ? uapsd_tids : 0;
    context->tx_pending[station_index] = 0;
    for (ps_id = 0; ps_id < AIC8800_PS_ID_COUNT; ps_id++)
    {
        context->sta_buffered[station_index][ps_id] = 0;
        context->sta_service_period_generation[station_index][ps_id]++;
        if (!context->sta_service_period_generation[station_index][ps_id])
        {
            context->sta_service_period_generation[station_index][ps_id]++;
        }
        context->sta_service_period_remaining[station_index][ps_id] = 0;
        context->sta_service_period_reserved[station_index][ps_id] = 0;
        context->sta_traffic_available[station_index][ps_id] = RT_FALSE;
        context->sta_traffic_reported[station_index][ps_id] = RT_FALSE;
        context->sta_traffic_dirty[station_index][ps_id] = RT_FALSE;
    }
    rt_hw_interrupt_enable(level);
}

static void aic_station_add_begin(struct aic8800_context *context)
{
    rt_base_t level;

    if (!context)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    rt_memset(context->sta_add_power_save, 0,
              sizeof(context->sta_add_power_save));
    context->station_add_pending = RT_TRUE;
    rt_hw_interrupt_enable(level);
}

static void aic_station_add_cancel(struct aic8800_context *context)
{
    rt_base_t level;

    if (!context)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    context->station_add_pending = RT_FALSE;
    rt_memset(context->sta_add_power_save, 0,
              sizeof(context->sta_add_power_save));
    rt_hw_interrupt_enable(level);
}

static rt_bool_t aic_station_state_snapshot(
    struct aic8800_context *context, rt_uint8_t station_index,
    rt_uint16_t *generation, rt_bool_t *power_save)
{
    rt_bool_t present;
    rt_base_t level;

    if (!context || station_index >= AIC8800_STATION_SLOTS)
    {
        return RT_FALSE;
    }
    level = rt_hw_interrupt_disable();
    present = context->sta_present[station_index];
    if (generation)
    {
        *generation = context->sta_generation[station_index];
    }
    if (power_save)
    {
        *power_save = context->sta_power_save[station_index];
    }
    rt_hw_interrupt_enable(level);
    return present;
}

static rt_bool_t aic_station_state_matches(
    struct aic8800_context *context, rt_uint8_t station_index,
    rt_uint16_t generation, rt_bool_t *power_save)
{
    rt_bool_t matches;
    rt_base_t level;

    if (!context || station_index >= AIC8800_STATION_SLOTS)
    {
        return RT_FALSE;
    }
    level = rt_hw_interrupt_disable();
    matches = context->sta_present[station_index] &&
              context->sta_generation[station_index] == generation;
    if (power_save)
    {
        *power_save = context->sta_power_save[station_index];
    }
    rt_hw_interrupt_enable(level);
    return matches;
}

/* Every transport record with accounted metadata must reach this function
 * exactly once, whether it was submitted, rejected by a worker, or drained
 * during reset/teardown.  USB cancellation/watchdog recovery and both USB and
 * SDIO queue cleanup paths preserve that contract.  The generation check
 * below makes a late completion harmless after a station slot is reused. */
void aic8800_core_tx_complete(
    struct aic8800_context *context,
    const struct aic8800_tx_metadata *metadata)
{
    rt_uint8_t station_index;
    rt_uint8_t ps_id;
    rt_bool_t traffic_empty = RT_FALSE;
    rt_base_t level;

    if (!context || !metadata || !metadata->accounted)
    {
        return;
    }
    station_index = metadata->station_index;
    ps_id = metadata->ps_id;
    if (station_index >= AIC8800_STATION_SLOTS ||
        ps_id >= AIC8800_PS_ID_COUNT)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    if (context->sta_generation[station_index] ==
            metadata->station_generation &&
        context->tx_pending[station_index])
    {
        context->tx_pending[station_index]--;
        if (metadata->host_buffered &&
            context->sta_buffered[station_index][ps_id])
        {
            context->sta_buffered[station_index][ps_id]--;
            if (metadata->service_period_reserved &&
                metadata->service_period_generation ==
                    context->sta_service_period_generation[station_index]
                                                          [ps_id] &&
                context->sta_service_period_reserved[station_index][ps_id])
            {
                context->sta_service_period_reserved[station_index][ps_id]--;
            }
            traffic_empty =
                context->sta_buffered[station_index][ps_id] == 0;
        }
    }
    rt_hw_interrupt_enable(level);
    if (traffic_empty)
    {
        aic_set_traffic_status(context, station_index, ps_id, RT_FALSE);
    }
}

void aic8800_core_tx_pending_reset(struct aic8800_context *context)
{
    rt_base_t level;

    if (!context)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    rt_memset(context->tx_pending, 0, sizeof(context->tx_pending));
    rt_memset(context->sta_present, 0, sizeof(context->sta_present));
    context->station_add_pending = RT_FALSE;
    rt_memset(context->sta_add_power_save, 0,
              sizeof(context->sta_add_power_save));
    rt_memset(context->sta_power_save, 0, sizeof(context->sta_power_save));
    rt_memset(context->sta_uapsd_tids, 0,
              sizeof(context->sta_uapsd_tids));
    rt_memset(context->sta_buffered, 0, sizeof(context->sta_buffered));
    rt_memset(context->sta_service_period_remaining, 0,
              sizeof(context->sta_service_period_remaining));
    rt_memset(context->sta_service_period_reserved, 0,
              sizeof(context->sta_service_period_reserved));
    rt_memset(context->sta_traffic_available, 0,
              sizeof(context->sta_traffic_available));
    rt_memset(context->sta_traffic_reported, 0,
              sizeof(context->sta_traffic_reported));
    rt_memset(context->sta_traffic_dirty, 0,
              sizeof(context->sta_traffic_dirty));
    for (rt_size_t index = 0; index < AIC8800_STATION_SLOTS; index++)
    {
        context->sta_generation[index]++;
        if (!context->sta_generation[index])
        {
            context->sta_generation[index]++;
        }
        for (rt_size_t ps_id = 0; ps_id < AIC8800_PS_ID_COUNT; ps_id++)
        {
            context->sta_service_period_generation[index][ps_id]++;
            if (!context->sta_service_period_generation[index][ps_id])
            {
                context->sta_service_period_generation[index][ps_id]++;
            }
        }
    }
    rt_hw_interrupt_enable(level);
}

static rt_err_t aic_tx_pending_acquire(struct aic8800_context *context,
                                       rt_uint8_t station_index,
                                       rt_uint16_t generation,
                                       rt_uint8_t ps_id)
{
    rt_base_t level;
    rt_bool_t watermark_reached = RT_FALSE;
    rt_uint32_t watermark_events = 0;
    rt_uint16_t pending = 0;

    if (!context || station_index >= AIC8800_STATION_SLOTS || !generation ||
        ps_id >= AIC8800_PS_ID_COUNT)
    {
        return -RT_EINVAL;
    }
    level = rt_hw_interrupt_disable();
    if (!context->sta_present[station_index] ||
        context->sta_generation[station_index] != generation)
    {
        rt_hw_interrupt_enable(level);
        return -RT_EBUSY;
    }
    if (context->tx_pending[station_index] >=
        AIC8800_TX_PENDING_HIGH_WATER)
    {
        context->tx_pending_watermark_count++;
        watermark_reached = RT_TRUE;
        watermark_events = context->tx_pending_watermark_count;
        /* Do not turn this watermark into a drop gate.  RTP packetizers emit
         * a keyframe as a tight burst, while the SDIO/USB worker drains the
         * transport asynchronously.  The old early return discarded the tail
         * of every burst once 64 records were pending, so the browser never
         * received a complete decodable keyframe.  Transport memory remains
         * bounded by the fixed TX record pools: USB uses
         * AIC8800_WIFI_USB_TX_QUEUE_DEPTH records (64 in the maix3 config) and
         * SDIO uses AIC8800_WIFI_SDIO_TX_QUEUE_DEPTH records (256).  Their
         * producers apply a bounded wait and return an error when a pool is
         * exhausted; USB URB and SDIO aggregate capacities are also fixed. */
    }
    context->tx_pending[station_index]++;
    context->sta_buffered[station_index][ps_id]++;
    pending = context->tx_pending[station_index];
    rt_hw_interrupt_enable(level);
    if (watermark_reached && aic8800_log_throttle(watermark_events))
    {
        LOG_D("station %u pending watermark reached (pending=%u events=%u); queueing",
              (unsigned int)station_index,
              (unsigned int)pending,
              (unsigned int)watermark_events);
    }
    return RT_EOK;
}

/* Channel-context index owned by a VIF, or AIC8800_INVALID_INDEX when the VIF
 * is not attached to one. */
static rt_uint8_t aic_vif_channel_index(const struct aic8800_context *context,
                                        enum rt_wlan_offload_iftype iftype)
{
    if (!context)
    {
        return AIC8800_INVALID_INDEX;
    }
    return iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
           context->station_channel_index : context->ap_channel_index;
}

/* The LMAC serves one channel context at a time.  Vendor Linux mirrors this by
 * keeping every VIF's transmit queues stopped unless its context is the
 * scheduled one (RWNX_TXQ_STOP_CHAN, driven by MM_CHANNEL_PRE_SWITCH_IND and
 * MM_CHANNEL_SWITCH_IND).  Handing the firmware a frame for an unscheduled
 * context is what wedges its transmit path.
 *
 * A firmware serving a single context never announces a switch, so gating
 * only starts once the first indication arrives.  While a switch is in
 * progress no context is on air and nothing may be sent.  A VIF that has not
 * been told its context yet is left ungated: association and the EAPOL
 * exchange run before the context index is known. */
static rt_bool_t aic_channel_context_active(
    const struct aic8800_context *context,
    enum rt_wlan_offload_iftype iftype)
{
    rt_uint8_t index;

    if (!context || !context->channel_context_tracked)
    {
        return RT_TRUE;
    }
    if (context->active_channel_index == AIC8800_INVALID_INDEX)
    {
        return RT_FALSE;
    }
    index = aic_vif_channel_index(context, iftype);
    return index == AIC8800_INVALID_INDEX ||
           index == context->active_channel_index;
}

void aic8800_core_tx_metadata_init(
    struct aic8800_context *context, const void *data, rt_size_t length,
    struct aic8800_tx_metadata *metadata)
{
    const rt_uint8_t *record = data;
    const struct aic_wire_tx_host_descriptor *descriptor;
    rt_base_t level;

    if (!metadata)
    {
        return;
    }
    rt_memset(metadata, 0, sizeof(*metadata));
    metadata->station_index = AIC8800_INVALID_INDEX;
    metadata->vif_index = AIC8800_INVALID_INDEX;
    metadata->ps_id = AIC_PS_ID_LEGACY;
    if (!context || !record ||
        length < AIC8800_USB_HEADER_SIZE + AIC_TX_DESCRIPTOR_SIZE ||
        (record[2] & 0x7fU) != AIC_USB_TYPE_DATA_TX)
    {
        return;
    }
    descriptor = (const struct aic_wire_tx_host_descriptor *)(
        record + AIC8800_USB_HEADER_SIZE);
    metadata->data_frame = RT_TRUE;
    metadata->station_index = descriptor->station_index;
    metadata->vif_index = descriptor->vif_index;
    metadata->management =
        (aic_get_le16(&descriptor->flags) & AIC_TX_FLAG_MANAGEMENT) != 0;
    if (metadata->management ||
        descriptor->vif_index != context->ap_vif_index ||
        descriptor->station_index >= AIC8800_STATION_SLOTS ||
        (descriptor->destination.array[0] & 1U))
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    if (context->sta_present[descriptor->station_index])
    {
        metadata->accounted = RT_TRUE;
        metadata->host_buffered = RT_TRUE;
        metadata->station_generation =
            context->sta_generation[descriptor->station_index];
        if (descriptor->tid < 16U &&
            (context->sta_uapsd_tids[descriptor->station_index] &
             (1U << descriptor->tid)))
        {
            metadata->ps_id = AIC_PS_ID_UAPSD;
        }
    }
    rt_hw_interrupt_enable(level);
}

enum aic8800_tx_record_state aic8800_core_tx_metadata_state(
    struct aic8800_context *context,
    struct aic8800_tx_metadata *metadata)
{
    rt_bool_t valid;
    rt_base_t level;

    if (!context || !metadata)
    {
        return AIC8800_TX_RECORD_DROP;
    }
    if (!metadata->data_frame || metadata->management)
    {
        return AIC8800_TX_RECORD_READY;
    }
    if (metadata->vif_index == context->vif_index)
    {
        if (!context->station_enabled || !context->station_connected ||
            metadata->station_index != context->ap_station_index)
        {
            return AIC8800_TX_RECORD_DROP;
        }
        return aic_channel_context_active(
                   context, RT_WLAN_OFFLOAD_IFTYPE_STATION) ?
               AIC8800_TX_RECORD_READY : AIC8800_TX_RECORD_DEFER;
    }
    if (metadata->vif_index != context->ap_vif_index ||
        !context->ap_enabled || !context->ap_started)
    {
        return AIC8800_TX_RECORD_DROP;
    }
    if (!aic_channel_context_active(context, RT_WLAN_OFFLOAD_IFTYPE_AP))
    {
        return AIC8800_TX_RECORD_DEFER;
    }
    if (!metadata->accounted)
    {
        return metadata->station_index == context->ap_broadcast_station_index ?
               AIC8800_TX_RECORD_READY : AIC8800_TX_RECORD_DROP;
    }
    if (metadata->station_index >= AIC8800_STATION_SLOTS)
    {
        return AIC8800_TX_RECORD_DROP;
    }
    level = rt_hw_interrupt_disable();
    valid = context->sta_present[metadata->station_index] &&
            context->sta_generation[metadata->station_index] ==
                metadata->station_generation;
    if (metadata->ps_id >= AIC8800_PS_ID_COUNT)
    {
        valid = RT_FALSE;
    }
    if (valid && context->sta_power_save[metadata->station_index])
    {
        if (metadata->service_period_reserved &&
            metadata->service_period_generation ==
                context->sta_service_period_generation[
                    metadata->station_index][metadata->ps_id])
        {
            rt_hw_interrupt_enable(level);
            return AIC8800_TX_RECORD_READY;
        }
        metadata->service_period_reserved = RT_FALSE;
        metadata->more_data = RT_FALSE;
        metadata->eosp = RT_FALSE;
        if (!context->sta_service_period_remaining[metadata->station_index]
                                                   [metadata->ps_id])
        {
            rt_hw_interrupt_enable(level);
            return AIC8800_TX_RECORD_DEFER;
        }
        context->sta_service_period_remaining[metadata->station_index]
                                                     [metadata->ps_id]--;
        context->sta_service_period_reserved[metadata->station_index]
                                                    [metadata->ps_id]++;
        metadata->service_period_generation =
            context->sta_service_period_generation[metadata->station_index]
                                                  [metadata->ps_id];
        metadata->service_period_reserved = RT_TRUE;
        metadata->eosp = metadata->ps_id == AIC_PS_ID_UAPSD &&
            !context->sta_service_period_remaining[metadata->station_index]
                                                   [metadata->ps_id];
    }
    else if (valid)
    {
        metadata->service_period_reserved = RT_FALSE;
        metadata->more_data = RT_FALSE;
        metadata->eosp = RT_FALSE;
    }
    rt_hw_interrupt_enable(level);
    return valid ? AIC8800_TX_RECORD_READY : AIC8800_TX_RECORD_DROP;
}

enum aic8800_tx_record_state aic8800_core_tx_metadata_apply(
    struct aic8800_context *context, struct aic8800_tx_metadata *metadata,
    void *data, rt_size_t length)
{
    rt_uint8_t *record = data;
    struct aic_wire_tx_host_descriptor *descriptor;
    rt_bool_t traffic_empty = RT_FALSE;
    rt_bool_t power_save = RT_FALSE;
    rt_bool_t service_period_active = RT_FALSE;
    rt_uint16_t flags;
    rt_base_t level;

    if (!context || !metadata || !record)
    {
        return AIC8800_TX_RECORD_DROP;
    }
    if (length < AIC8800_USB_HEADER_SIZE + AIC_TX_DESCRIPTOR_SIZE ||
        (record[2] & 0x7fU) != AIC_USB_TYPE_DATA_TX)
    {
        return AIC8800_TX_RECORD_READY;
    }
    if (metadata->accounted && metadata->host_buffered &&
        metadata->station_index < AIC8800_STATION_SLOTS &&
        metadata->ps_id < AIC8800_PS_ID_COUNT)
    {
        level = rt_hw_interrupt_disable();
        if (!context->sta_present[metadata->station_index] ||
            context->sta_generation[metadata->station_index] !=
                metadata->station_generation ||
            !context->sta_buffered[metadata->station_index][metadata->ps_id])
        {
            rt_hw_interrupt_enable(level);
            return AIC8800_TX_RECORD_DROP;
        }
        power_save = context->sta_power_save[metadata->station_index];
        service_period_active = metadata->service_period_reserved &&
            metadata->service_period_generation ==
                context->sta_service_period_generation[
                    metadata->station_index][metadata->ps_id] &&
            context->sta_service_period_reserved[
                metadata->station_index][metadata->ps_id] != 0;
        if (power_save && !service_period_active)
        {
            rt_hw_interrupt_enable(level);
            return AIC8800_TX_RECORD_DEFER;
        }
        if (service_period_active)
        {
            context->sta_service_period_reserved[
                metadata->station_index][metadata->ps_id]--;
        }
        context->sta_buffered[metadata->station_index][metadata->ps_id]--;
        metadata->host_buffered = RT_FALSE;
        metadata->more_data = power_save &&
            context->sta_buffered[metadata->station_index]
                                 [metadata->ps_id] != 0;
        metadata->eosp = metadata->eosp && power_save &&
                         service_period_active;
        traffic_empty =
            context->sta_buffered[metadata->station_index]
                                 [metadata->ps_id] == 0;
        rt_hw_interrupt_enable(level);
    }

    descriptor = (struct aic_wire_tx_host_descriptor *)(
        record + AIC8800_USB_HEADER_SIZE);
    flags = aic_get_le16(&descriptor->flags);
    flags &= ~(AIC_TX_FLAG_MORE_DATA | AIC_TX_FLAG_EOSP);
    if (metadata->more_data)
    {
        flags |= AIC_TX_FLAG_MORE_DATA;
    }
    if (metadata->eosp)
    {
        flags |= AIC_TX_FLAG_EOSP;
    }
    aic_put_le16(&descriptor->flags, flags);
    if (traffic_empty)
    {
        aic_set_traffic_status(context, metadata->station_index,
                               metadata->ps_id, RT_FALSE);
    }
    return AIC8800_TX_RECORD_READY;
}

void aic8800_core_tx_metadata_restore(
    struct aic8800_context *context, struct aic8800_tx_metadata *metadata)
{
    rt_bool_t report = RT_FALSE;
    rt_base_t level;

    if (!context || !metadata || !metadata->accounted ||
        metadata->host_buffered ||
        metadata->station_index >= AIC8800_STATION_SLOTS ||
        metadata->ps_id >= AIC8800_PS_ID_COUNT)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    if (context->sta_present[metadata->station_index] &&
        context->sta_generation[metadata->station_index] ==
            metadata->station_generation)
    {
        context->sta_buffered[metadata->station_index][metadata->ps_id]++;
        if (metadata->service_period_reserved &&
            metadata->service_period_generation ==
                context->sta_service_period_generation[
                    metadata->station_index][metadata->ps_id])
        {
            context->sta_service_period_reserved[
                metadata->station_index][metadata->ps_id]++;
        }
        metadata->host_buffered = RT_TRUE;
        metadata->more_data = RT_FALSE;
        report = context->sta_power_save[metadata->station_index];
    }
    rt_hw_interrupt_enable(level);
    if (report)
    {
        aic_set_traffic_status(context, metadata->station_index,
                               metadata->ps_id, RT_TRUE);
    }
}

static void aic_channel_context_report(struct aic8800_context *context,
                                       const char *reason)
{
    if (!context)
    {
        return;
    }
    LOG_I("channel contexts (%s): station=%u ap=%u active=%u",
          reason ? reason : "update",
          (unsigned int)context->station_channel_index,
          (unsigned int)context->ap_channel_index,
          (unsigned int)context->active_channel_index);
    if (context->station_connected && context->ap_started &&
        context->station_channel_index != AIC8800_INVALID_INDEX &&
        context->ap_channel_index != AIC8800_INVALID_INDEX &&
        context->station_channel_index != context->ap_channel_index)
    {
        /* One radio, two contexts: the firmware has to time-share the air and
         * every frame for the unscheduled VIF has to be held back. */
        LOG_W("concurrent VIFs are on different channel contexts "
              "(station=%u ap=%u); transmit will be gated",
              (unsigned int)context->station_channel_index,
              (unsigned int)context->ap_channel_index);
    }
}

static const rt_uint8_t *aic_connect_response_ies(
    const rt_uint8_t *parameter, rt_size_t length, rt_size_t *ies_length)
{
    rt_uint16_t request_length;
    rt_uint16_t response_length;

    if (!parameter || !ies_length ||
        length < AIC_SM_CONNECT_IND_IE_BUFFER_OFFSET)
    {
        return RT_NULL;
    }
    request_length = aic_get_le16(
        parameter + AIC_SM_CONNECT_IND_REQ_IE_LEN_OFFSET);
    response_length = aic_get_le16(
        parameter + AIC_SM_CONNECT_IND_RSP_IE_LEN_OFFSET);
    if (request_length > AIC_SM_CONNECT_IND_IE_BUFFER_SIZE ||
        response_length > AIC_SM_CONNECT_IND_IE_BUFFER_SIZE - request_length ||
        AIC_SM_CONNECT_IND_IE_BUFFER_OFFSET + request_length +
            response_length > length)
    {
        return RT_NULL;
    }
    *ies_length = response_length;
    return parameter + AIC_SM_CONNECT_IND_IE_BUFFER_OFFSET + request_length;
}

static const rt_uint8_t *aic_find_connect_ie(
    const rt_uint8_t *ies, rt_size_t ies_length, rt_uint8_t element_id,
    rt_uint8_t *element_length)
{
    rt_size_t offset = 0;

    while (ies && offset + 2U <= ies_length)
    {
        rt_size_t current_length = ies[offset + 1U];

        if (offset + 2U + current_length > ies_length)
        {
            break;
        }
        if (ies[offset] == element_id)
        {
            if (element_length)
            {
                *element_length = (rt_uint8_t)current_length;
            }
            return ies + offset + 2U;
        }
        offset += 2U + current_length;
    }
    return RT_NULL;
}

static const rt_uint8_t *aic_connect_response_ie(
    const rt_uint8_t *parameter, rt_size_t length, rt_uint8_t element_id,
    rt_uint8_t *element_length)
{
    const rt_uint8_t *ies;
    rt_size_t ies_length = 0;

    ies = aic_connect_response_ies(parameter, length, &ies_length);
    return aic_find_connect_ie(ies, ies_length, element_id, element_length);
}

/* Returns the AP's operating bandwidth in MHz, or 0 when no HT or VHT
 * operation element is present.
 *
 * VHT Operation signals 160 MHz and 80+80 MHz two different ways.  Besides the
 * deprecated width codes 2 and 3, there is the backward-compatible form where
 * the width code stays 1 and the second centre-frequency segment is non-zero:
 * a segment separation of 8 channels means 160 MHz, and more than 16 means
 * 80+80 MHz.  Reading the width code alone reports a 160 MHz AP as 80 MHz,
 * which is what the field says but not what the AP is doing. */
static rt_uint32_t aic_log_ap_channel_operation(const rt_uint8_t *parameter,
                                                rt_size_t length)
{
    const rt_uint8_t *ht;
    const rt_uint8_t *vht;
    rt_uint8_t ht_length = 0;
    rt_uint8_t vht_length = 0;
    rt_uint8_t secondary = 0;
    rt_uint8_t ht_width = 0;
    rt_uint8_t vht_width = 0;
    rt_uint8_t center0 = 0;
    rt_uint8_t center1 = 0;
    rt_uint32_t bandwidth = 0;

    ht = aic_connect_response_ie(parameter, length,
                                 AIC_WLAN_EID_HT_OPERATION, &ht_length);
    vht = aic_connect_response_ie(parameter, length,
                                  AIC_WLAN_EID_VHT_OPERATION, &vht_length);
    if (ht && ht_length >= 2U)
    {
        secondary = ht[1] & 0x03U;
        ht_width = (ht[1] & 0x04U) ? 40U : 20U;
        bandwidth = ht_width;
    }
    if (vht && vht_length >= 3U)
    {
        vht_width = vht[0];
        center0 = vht[1];
        center1 = vht[2];
        switch (vht_width)
        {
        case 0U:
            /* 20 or 40 MHz; the HT operation element above already said which. */
            break;
        case 1U:
            if (center1)
            {
                int separation = (int)center1 - (int)center0;

                if (separation < 0)
                {
                    separation = -separation;
                }
                bandwidth = separation == 8 || separation > 16 ? 160U : 80U;
            }
            else
            {
                bandwidth = 80U;
            }
            break;
        case 2U:
        case 3U:
            bandwidth = 160U;
            break;
        default:
            break;
        }
    }
    if (ht || vht)
    {
        LOG_I("AP operation: HT=%u MHz secondary=%u VHT-code=%u center0=%u center1=%u operating=%u MHz",
              (unsigned int)ht_width, (unsigned int)secondary,
              (unsigned int)vht_width, (unsigned int)center0,
              (unsigned int)center1, (unsigned int)bandwidth);
    }
    return bandwidth;
}

/* Firmware channel width code to MHz.  80+80 MHz occupies 160 MHz of spectrum
 * in two segments; report it as 160 so width comparisons stay meaningful. */
static rt_uint32_t aic_channel_width_mhz(rt_uint8_t width)
{
    switch (width)
    {
    case 1U: return 40U;
    case 2U: return 80U;
    case 3U: return 160U;
    case 4U: return 160U;
    default: return 20U;
    }
}

static void aic_update_connected_channel(
    struct aic8800_context *context, const rt_uint8_t *parameter,
    rt_size_t length)
{
    struct rt_wlan_offload_channel_definition channel;
    rt_uint16_t frequency;
    rt_uint32_t center1;
    rt_uint32_t center2;
    rt_uint8_t band;
    rt_uint8_t width;
    rt_uint16_t channel_number;
    rt_uint32_t negotiated;
    rt_uint32_t offered;

    if (!context || !parameter || length < AIC_SM_CONNECT_IND_CHANNEL_END)
    {
        return;
    }
    band = parameter[AIC_SM_CONNECT_IND_BAND_OFFSET];
    frequency = aic_get_le16(parameter + AIC_SM_CONNECT_IND_FREQUENCY_OFFSET);
    width = parameter[AIC_SM_CONNECT_IND_WIDTH_OFFSET];
    center1 = aic_get_le32(parameter + AIC_SM_CONNECT_IND_CENTER1_OFFSET);
    center2 = aic_get_le32(parameter + AIC_SM_CONNECT_IND_CENTER2_OFFSET);
    if (band > 1U || !frequency)
    {
        return;
    }
    channel_number = aic_frequency_to_channel(frequency);
    if (!channel_number || !aic_channel_allowed(
            context, (enum rt_wlan_offload_band_id)band, channel_number))
    {
        return;
    }
    rt_memset(&channel, 0, sizeof(channel));
    channel.band = (enum rt_wlan_offload_band_id)band;
    channel.primary_channel = channel_number;
    channel.primary_frequency_mhz = frequency;
    channel.center_frequency1_mhz = center1 <= 0xffffU ?
                                    (rt_uint16_t)center1 : frequency;
    channel.center_frequency2_mhz = center2 <= 0xffffU ?
                                    (rt_uint16_t)center2 : 0;
    switch (width)
    {
    case 1U:
        channel.width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40;
        break;
    case 2U:
        channel.width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80;
        break;
    case 3U:
        channel.width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_160;
        break;
    case 4U:
        channel.width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80;
        break;
    default:
        channel.width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20;
        break;
    }
    if (!channel.center_frequency1_mhz)
    {
        channel.center_frequency1_mhz = frequency;
    }
    context->current_channel = channel;
    context->current_channel_valid = RT_TRUE;
    negotiated = aic_channel_width_mhz(width);
    LOG_I("association channel: primary=%u frequency=%u MHz width=%u MHz center1=%u center2=%u",
          (unsigned int)channel.primary_channel,
          (unsigned int)channel.primary_frequency_mhz,
          (unsigned int)negotiated,
          (unsigned int)channel.center_frequency1_mhz,
          (unsigned int)channel.center_frequency2_mhz);
    offered = aic_log_ap_channel_operation(parameter, length);

    /* Tell an AIC8800D40 apart from an AIC8800D80 at run time.  Both enumerate
     * as a69c:8d81, load the same firmware family and report an 80 MHz modem
     * in MM_VERSION, so nothing at startup separates a part that does
     * 20/40/80 MHz at 600.4 Mbps from one that does 20/40 MHz at 286.8 Mbps.
     * The association itself does: the firmware picks the operating width from
     * the AP's operation elements and its own modem, so settling for 40 MHz
     * where the AP offered 80 or more is a modem that cannot do 80.
     *
     * Wait for a second such association before believing it.  The costs are
     * asymmetric - capping a real D80 at 40 MHz throws away half its
     * throughput, while letting a D40 advertise a width it never uses costs
     * little - so the evidence has to be repeated, and any association that
     * does reach 80 MHz proves the modem outright and clears the count. */
    if (channel.band == RT_WLAN_OFFLOAD_BAND_5GHZ && offered >= 80U &&
        !context->bandwidth_80_rejected &&
        aic8800_radio_supports_80mhz(context))
    {
        if (negotiated >= 80U)
        {
            context->bandwidth_80_failures = 0;
        }
        else if (++context->bandwidth_80_failures >= AIC_BANDWIDTH_80_FAILURES)
        {
            LOG_W("modem claims 80 MHz but associated at %u MHz to a %u MHz AP "
                  "%u times; advertising %u MHz from the next association",
                  (unsigned int)negotiated, (unsigned int)offered,
                  (unsigned int)context->bandwidth_80_failures,
                  (unsigned int)negotiated);
            context->bandwidth_80_rejected = RT_TRUE;
            context->me_config_stale = RT_TRUE;
            aic_refresh_channel_metadata(context);
        }
        else
        {
            LOG_I("associated at %u MHz to a %u MHz AP; another like this "
                  "drops the 80 MHz claim",
                  (unsigned int)negotiated, (unsigned int)offered);
        }
    }
}

/* mac_chan_flags: bit0 CHAN_NO_IR, bit1 CHAN_DISABLED, bit2 CHAN_RADAR.
 *
 * Only CHAN_DISABLED is derived from the regulatory table.  Vendor Linux builds
 * with CONFIG_RADAR_OR_IR_DETECT=n, so its get_chan_flags() is a no-op and both
 * ME_CHAN_CONFIG_REQ and SCANU_START_REQ always carry flags=0: the firmware owns
 * DFS and no-initiate-radiation policy itself.  Advertising CHAN_NO_IR from the
 * host turns those channels beacon-only, which loses APs during a scan. */
static rt_uint8_t aic_channel_flags(rt_uint32_t flags)
{
    return (flags & RT_WLAN_OFFLOAD_CHANNEL_DISABLED) ? 2U : 0U;
}

static void aic_encode_channel(rt_uint8_t *destination,
                               const struct rt_wlan_offload_channel_definition *channel,
                               rt_int8_t power)
{
    aic_put_le16(destination, channel->primary_frequency_mhz);
    destination[2] = channel->band == RT_WLAN_OFFLOAD_BAND_5GHZ ? 1 : 0;
    destination[3] = 0;
    destination[4] = (rt_uint8_t)power;
    destination[5] = 0;
}

static const struct rt_wlan_offload_channel *aic_channel_metadata(
    const struct aic8800_context *context,
    enum rt_wlan_offload_band_id band_id, rt_uint16_t channel_number)
{
    const struct rt_wlan_offload_supported_band *band;
    rt_size_t index;

    if (!context || band_id >= RT_WLAN_OFFLOAD_BAND_MAX)
    {
        return RT_NULL;
    }
    band = context->radio.bands[band_id];
    if (!band)
    {
        return RT_NULL;
    }
    for (index = 0; index < band->channel_count; index++)
    {
        if (band->channels[index].number == channel_number)
        {
            return &band->channels[index];
        }
    }
    return RT_NULL;
}

static rt_err_t aic_command_push(
    struct rt_wlan_offload_command_manager *manager, rt_uint32_t token,
    rt_uint16_t command_id, const void *request, rt_size_t request_length,
    void *driver_data)
{
    struct aic8800_context *context = driver_data;
    rt_uint8_t *frame;
    rt_size_t native_length;
    rt_size_t frame_length;
    rt_err_t result;

    (void)manager;
    (void)token;
    /* The command header is 16 bytes and the USB length field is 12 bits.
     * Check before doing the additions so a malformed size cannot wrap and
     * pass the frame-size test. */
    if (!context || (request_length && !request) ||
        request_length > AIC8800_USB_MAX_COMMAND_SIZE - 16U)
    {
        return -RT_EINVAL;
    }
    native_length = 8U + request_length;
    frame_length = native_length + 8U;
    frame = rt_malloc_align(frame_length, AIC8800_USB_DMA_ALIGNMENT);
    if (!frame)
    {
        return -RT_ENOMEM;
    }
    rt_memset(frame, 0, frame_length);
    aic_put_le16(frame, (rt_uint16_t)(native_length + 4U));
    frame[2] = AIC_USB_TYPE_COMMAND;
    aic_put_le16(frame + 8, command_id);
    aic_put_le16(frame + 10, command_id >> 10);
    aic_put_le16(frame + 12, AIC_WIRE_DRIVER_TASK);
    aic_put_le16(frame + 14, (rt_uint16_t)request_length);
    if (request_length)
    {
        rt_memcpy(frame + 16, request, request_length);
    }
    if (context->command_tx_log_count < 16U)
    {
        context->command_tx_log_count++;
        LOG_D("firmware TX 0x%04x, parameter=%u, transport=%u",
              command_id,
              (unsigned int)request_length, (unsigned int)frame_length);
    }
    result = rt_wlan_offload_bus_transmit_priority(
        &context->bus, RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL, frame,
        frame_length);
    rt_free_align(frame);
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    /* EAPOL key installation is initiated by the SDIO RX thread. Keep
     * servicing the multiplexed FIFO until its synchronous command completes. */
    if (result == RT_EOK &&
        context->transport == AIC8800_TRANSPORT_SDIO &&
        rt_thread_self() == context->sdio_thread)
    {
        result = aic8800_sdio_pump_command(
            context, manager, token, AIC8800_WIFI_COMMAND_TIMEOUT_MS);
    }
#endif
    return result;
}

static void aic_clear_hardware_keys(struct aic8800_context *context,
                                    rt_bool_t notify_firmware)
{
    rt_size_t index;

    if (!context)
    {
        return;
    }
    /* The AIC firmware discards station keys when the station entry is
     * removed.  Do not send an untracked MM_KEY_DEL_REQ from the RX worker:
     * its confirmation could otherwise satisfy a later synchronous command
     * because the vendor protocol has no host token. */
    (void)notify_firmware;
    for (index = 0; index < AIC8800_HARDWARE_KEY_COUNT; index++)
    {
        if (!context->hardware_keys[index].valid)
        {
            continue;
        }
        rt_memset(&context->hardware_keys[index], 0,
                  sizeof(context->hardware_keys[index]));
    }
}

static void aic_clear_vif_hardware_keys(
    struct aic8800_context *context, enum rt_wlan_offload_iftype iftype)
{
    rt_size_t index;

    if (!context)
    {
        return;
    }
    for (index = 0; index < AIC8800_HARDWARE_KEY_COUNT; index++)
    {
        if (context->hardware_keys[index].valid &&
            context->hardware_keys[index].iftype == iftype)
        {
            rt_memset(&context->hardware_keys[index], 0,
                      sizeof(context->hardware_keys[index]));
        }
    }
}

rt_err_t aic8800_protocol_command(struct aic8800_context *context,
                                rt_uint16_t request_id,
                                rt_uint16_t confirmation_id,
                                const void *request,
                                rt_size_t request_length,
                                void *response,
                                rt_size_t response_capacity,
                                rt_size_t *response_length)
{
    rt_err_t result;

    if (!context)
    {
        return -RT_EINVAL;
    }
    if (!context->command_gate_initialized)
    {
        return -RT_EIO;
    }
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO &&
        rt_thread_self() == context->sdio_thread)
    {
        while (RT_TRUE)
        {
            result = rt_sem_take(&context->command_gate, RT_WAITING_NO);
            if (result == RT_EOK)
            {
                break;
            }
            result = aic8800_sdio_pump_command(
                context, &context->commands, 0,
                AIC8800_WIFI_COMMAND_TIMEOUT_MS);
            if (result != RT_EOK)
            {
                return result;
            }
            /* The completed command's owner must run before it can release
             * the gate. A bounded wait avoids starving that lower-priority
             * thread, while the next iteration still services a command if
             * another waiter acquired the gate first. */
            result = rt_sem_take(&context->command_gate, 1);
            if (result == RT_EOK)
            {
                break;
            }
            if (result != -RT_ETIMEOUT)
            {
                return result;
            }
        }
    }
    else
#endif
    {
        result = rt_sem_take(&context->command_gate, RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    result = rt_wlan_offload_command_execute(
        &context->commands, request_id, confirmation_id,
        request, request_length, response, response_capacity, response_length,
        rt_tick_from_millisecond(AIC8800_WIFI_COMMAND_TIMEOUT_MS), RT_NULL);
    rt_sem_release(&context->command_gate);
    return result;
}

static rt_err_t aic_execute(struct aic8800_context *context,
                            rt_uint16_t request_id,
                            rt_uint16_t confirmation_id,
                            const void *request, rt_size_t request_length,
                            void *response, rt_size_t response_capacity,
                            rt_size_t *response_length)
{
    return aic8800_protocol_command(context, request_id, confirmation_id,
                                    request, request_length, response,
                                    response_capacity, response_length);
}

static rt_err_t aic_runtime_mem_mask_write(
    struct aic8800_context *context, rt_uint32_t address, rt_uint32_t mask,
    rt_uint32_t value)
{
    rt_uint8_t request[12];

    aic_put_le32(request, address);
    aic_put_le32(request + 4, mask);
    aic_put_le32(request + 8, value);
    return aic_execute(context, AIC_DBG_MEM_MASK_WRITE_REQ,
                       AIC_DBG_MEM_MASK_WRITE_CFM, request, sizeof(request),
                       RT_NULL, 0, RT_NULL);
}

static rt_err_t aic_configure_runtime_rx_gain(struct aic8800_context *context)
{
    if (!context)
    {
        return -RT_EINVAL;
    }
    if (context->product_id != AIC8800_USB_PID_AIC8800DC &&
        context->product_id != AIC8800_USB_PID_AIC8800DW)
    {
        return RT_EOK;
    }
    return aic_runtime_mem_mask_write(
        context, AIC_DC_RUNTIME_RX_GAIN_ADDRESS, AIC_DC_RUNTIME_RX_GAIN_MASK,
        AIC_DC_RUNTIME_RX_GAIN_VALUE);
}

static rt_err_t aic_configure_coexistence(struct aic8800_context *context)
{
    struct aic_wire_mm_set_coex_req request;

    if (!context)
    {
        return -RT_EINVAL;
    }
    rt_memset(&request, 0, sizeof(request));
    request.bt_on = 1;
    request.enable_nullcts = 1;
    return aic_execute(context, AIC_MM_SET_COEX_REQ, AIC_MM_SET_COEX_CFM,
                       &request, sizeof(request), RT_NULL, 0, RT_NULL);
}

static void aic_schedule_traffic_work(struct aic8800_context *context,
                                      rt_tick_t delay)
{
    rt_bool_t submit = RT_FALSE;
    rt_base_t level;

    if (!context)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    if (context->traffic_work_initialized &&
        !context->traffic_work_queued && context->attached)
    {
        context->traffic_work_queued = RT_TRUE;
        submit = RT_TRUE;
    }
    rt_hw_interrupt_enable(level);
    if (submit && rt_work_submit(&context->traffic_work, delay) != RT_EOK)
    {
        level = rt_hw_interrupt_disable();
        context->traffic_work_queued = RT_FALSE;
        rt_hw_interrupt_enable(level);
    }
}

static void aic_set_traffic_status(struct aic8800_context *context,
                                   rt_uint8_t station_index,
                                   rt_uint8_t ps_id,
                                   rt_bool_t available)
{
    rt_bool_t dirty = RT_FALSE;
    rt_base_t level;

    if (!context || station_index >= AIC8800_STATION_SLOTS ||
        ps_id >= AIC8800_PS_ID_COUNT)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    if (!context->sta_present[station_index])
    {
        context->sta_traffic_available[station_index][ps_id] = RT_FALSE;
        context->sta_traffic_reported[station_index][ps_id] = RT_FALSE;
        context->sta_traffic_dirty[station_index][ps_id] = RT_FALSE;
    }
    else
    {
        context->sta_traffic_available[station_index][ps_id] = available;
        dirty =
            context->sta_traffic_reported[station_index][ps_id] != available;
        context->sta_traffic_dirty[station_index][ps_id] = dirty;
    }
    rt_hw_interrupt_enable(level);
    if (dirty)
    {
        aic_schedule_traffic_work(context, 0);
    }
}

static void aic_traffic_work(struct rt_work *work, void *work_data)
{
    struct aic8800_context *context = work_data;
    rt_bool_t retry = RT_FALSE;
    rt_base_t level;
    rt_size_t index;

    (void)work;
    if (!context)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    context->traffic_work_queued = RT_FALSE;
    rt_hw_interrupt_enable(level);

    for (index = 0; index < AIC8800_STATION_SLOTS; index++)
    {
        for (rt_size_t ps_id = 0; ps_id < AIC8800_PS_ID_COUNT; ps_id++)
        {
            struct aic_wire_me_traffic_ind_req request;
            rt_uint16_t generation;
            rt_bool_t available;
            rt_bool_t dirty;
            rt_err_t result;

            level = rt_hw_interrupt_disable();
            dirty = context->sta_present[index] &&
                    context->sta_traffic_dirty[index][ps_id];
            generation = context->sta_generation[index];
            available = context->sta_traffic_available[index][ps_id];
            rt_hw_interrupt_enable(level);
            if (!dirty)
            {
                continue;
            }

            request.station_index = (rt_uint8_t)index;
            request.tx_available = available ? 1U : 0U;
            request.uapsd = ps_id == AIC_PS_ID_UAPSD;
            result = aic_execute(context, AIC_ME_TRAFFIC_IND_REQ,
                                 AIC_ME_TRAFFIC_IND_CFM, &request,
                                 sizeof(request), RT_NULL, 0, RT_NULL);

            level = rt_hw_interrupt_disable();
            if (!context->sta_present[index] ||
                context->sta_generation[index] != generation)
            {
                context->sta_traffic_available[index][ps_id] = RT_FALSE;
                context->sta_traffic_reported[index][ps_id] = RT_FALSE;
                context->sta_traffic_dirty[index][ps_id] = RT_FALSE;
            }
            else
            {
                if (result == RT_EOK)
                {
                    context->sta_traffic_reported[index][ps_id] = available;
                }
                context->sta_traffic_dirty[index][ps_id] =
                    context->sta_traffic_available[index][ps_id] !=
                        context->sta_traffic_reported[index][ps_id];
                retry = retry || context->sta_traffic_dirty[index][ps_id];
            }
            rt_hw_interrupt_enable(level);
        }
    }
    if (retry && context->lmac_started)
    {
        aic_schedule_traffic_work(
            context, rt_tick_from_millisecond(AIC_TRAFFIC_IND_RETRY_MS));
    }
}

static rt_err_t aic_submit_scan_wire(struct aic8800_context *context,
                                     const struct aic_wire_scanu_start_req
                                         *request)
{
    return aic_execute(context, AIC_SCANU_START_REQ,
                       AIC_SCANU_START_ACCEPTED, request,
                       AIC_SCAN_REQUEST_SIZE, RT_NULL, 0, RT_NULL);
}

static rt_err_t aic_confirmation_status(const rt_uint8_t *confirmation,
                                        rt_size_t length)
{
    if (length && !confirmation)
    {
        return -RT_EINVAL;
    }
    return length && confirmation[0] ? -RT_ERROR : RT_EOK;
}

struct aic_security_suites
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

static void aic_parse_akm_suite(struct aic_security_suites *suites,
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

static rt_wlan_security_t aic_security_from_ies(rt_uint16_t capability,
                                                const rt_uint8_t *ies,
                                                rt_size_t length)
{
    struct aic_security_suites rsn = {0}, wpa = {0};
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
            if (ie_length < 8U || aic_get_le16(body) != 1U)
            {
                offset += 2U + ie_length;
                continue;
            }
            count = aic_get_le16(body + 2);
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
            struct aic_security_suites parsed = {0};
            struct aic_security_suites *suites = id == 48 ? &rsn : &wpa;
            const rt_uint8_t *body = ies + offset + 2;
            const rt_uint8_t *oui = id == 48 ?
                (const rt_uint8_t *)"\x00\x0f\xac" :
                (const rt_uint8_t *)"\x00\x50\xf2";
            rt_size_t body_length = ie_length;
            rt_size_t position = id == 48 ? 0U : 4U;
            rt_uint16_t count;
            rt_size_t index;

            if (body_length < position + 8U ||
                aic_get_le16(body + position) != 1U)
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
            count = aic_get_le16(body + position);
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
            count = aic_get_le16(body + position);
            position += 2U;
            for (index = 0; index < count && position + 4U <= body_length;
                 index++, position += 4U)
            {
                aic_parse_akm_suite(&parsed, body + position, id == 48);
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

static rt_bool_t aic_rsn_ies_enable_mfp(const rt_uint8_t *ies,
                                        rt_size_t length)
{
    rt_size_t offset = 0;

    while (ies && offset + 2U <= length)
    {
        rt_uint8_t ie_length = ies[offset + 1U];
        const rt_uint8_t *body;
        rt_size_t position;
        rt_uint16_t count;

        if (offset + 2U + ie_length > length)
        {
            break;
        }
        if (ies[offset] != 48 || ie_length < 8U)
        {
            offset += 2U + ie_length;
            continue;
        }
        body = ies + offset + 2U;
        if (aic_get_le16(body) != 1U)
        {
            offset += 2U + ie_length;
            continue;
        }
        position = 2U + 4U;
        if (position + 2U > ie_length)
        {
            break;
        }
        count = aic_get_le16(body + position);
        position += 2U;
        if (count > (ie_length - position) / 4U)
        {
            break;
        }
        position += count * 4U;
        if (position + 2U > ie_length)
        {
            break;
        }
        count = aic_get_le16(body + position);
        position += 2U;
        if (count > (ie_length - position) / 4U)
        {
            break;
        }
        position += count * 4U;
        if (position + 2U <= ie_length &&
            (aic_get_le16(body + position) & 0x00c0U))
        {
            return RT_TRUE;
        }
        offset += 2U + ie_length;
    }
    return RT_FALSE;
}

static void aic_report_scan_result(struct aic8800_context *context,
                                   const rt_uint8_t *parameter,
                                   rt_size_t length)
{
    struct rt_wlan_offload_event event;
    const rt_uint8_t *frame;
    const rt_uint8_t *ies;
    rt_size_t frame_length;
    rt_size_t ies_length;
    rt_size_t offset;

    if (!context || !parameter || length < 12)
    {
        if (context && context->invalid_rx_log_count < 8U)
        {
            context->invalid_rx_log_count++;
            LOG_W("ignored malformed scan result: length=%u",
                  (unsigned int)length);
        }
        return;
    }
    /* A well-formed record can still arrive after SCANU_START_CFM cleared the
     * request, or while the radio is down.  That is a lost result, not a
     * protocol error: keep it out of the malformed counter so the warning above
     * keeps pointing at real parsing problems. */
    if (!context->scan_request_id || !context->attached ||
        context->radio.state != RT_WLAN_OFFLOAD_STARTED)
    {
        context->scan_late_result_count++;
        if (aic8800_log_throttle(context->scan_late_result_count))
        {
            LOG_D("dropped scan result with no scan outstanding: length=%u "
                  "(total=%u)",
                  (unsigned int)length,
                  (unsigned int)context->scan_late_result_count);
        }
        return;
    }
    frame_length = aic_get_le16(parameter);
    if (frame_length < 36 || frame_length > length - 12U)
    {
        if (context->invalid_rx_log_count < 8U)
        {
            context->invalid_rx_log_count++;
            LOG_W("ignored malformed scan result: length=%u frame=%u",
                  (unsigned int)length, (unsigned int)frame_length);
        }
        return;
    }
    frame = parameter + 12;
    ies = frame + 36;
    ies_length = frame_length - 36U;

    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_RESULT;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = context->scan_request_id;
    event.status = RT_EOK;
    rt_memcpy(event.data.network.bssid, frame + 16, 6);
    aic_channel_definition(aic_get_le16(parameter + 4),
                           &event.data.network.channel);
    if (event.data.network.channel.band == RT_WLAN_OFFLOAD_BAND_2GHZ)
    {
        context->scan_results_2ghz++;
    }
    else if (event.data.network.channel.band == RT_WLAN_OFFLOAD_BAND_5GHZ)
    {
        context->scan_results_5ghz++;
    }
    event.data.network.rssi = (rt_int8_t)parameter[9];
    event.data.network.beacon_interval = aic_get_le16(frame + 32);
    event.data.network.capability = aic_get_le16(frame + 34);
    event.data.network.security = aic_security_from_ies(
        event.data.network.capability, ies, ies_length);
    event.data.network.ies = ies;
    event.data.network.ies_length = ies_length;
    for (offset = 0; offset + 2U <= ies_length; )
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

static void aic_report_scan_done_status(struct aic8800_context *context,
                                        rt_err_t status,
                                        rt_bool_t result_count_valid,
                                        rt_uint16_t result_count)
{
    struct rt_wlan_offload_event event;
    rt_uint32_t request_id;

    if (!context)
    {
        return;
    }
    request_id = context->scan_request_id;

    if (!request_id)
    {
        return;
    }
    context->scan_request_id = 0;
    context->scan_completion_pending = RT_FALSE;
    context->scan_completion_retry_count = 0;
    context->scan_result_count_valid = RT_FALSE;
    context->scan_followup_pending = RT_FALSE;
    context->scan_followup_retry_count = 0;
    context->scan_work_queued = RT_FALSE;
    if (!context->attached || context->radio.state != RT_WLAN_OFFLOAD_STARTED)
    {
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_SCAN_DONE;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = request_id;
    event.status = status;
    if (result_count_valid)
    {
        LOG_I("scan complete: status=%d firmware=%u 2.4GHz=%u 5GHz=%u",
              status, (unsigned int)result_count,
              context->scan_results_2ghz, context->scan_results_5ghz);
    }
    else
    {
        LOG_I("scan complete: status=%d firmware=? 2.4GHz=%u 5GHz=%u",
              status, context->scan_results_2ghz,
              context->scan_results_5ghz);
    }
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void aic_handle_scan_done(struct aic8800_context *context,
                                 const rt_uint8_t *parameter,
                                 rt_size_t length)
{
    rt_err_t status;
    rt_uint16_t result_count = 0;
    rt_bool_t result_count_valid = RT_FALSE;

    if (!context)
    {
        return;
    }
    if (!context->scan_request_id || context->scan_completion_pending)
    {
        return;
    }
    if (!parameter || length < 2U)
    {
        aic_report_scan_done_status(context, -RT_EIO, RT_FALSE, 0);
        return;
    }
    status = parameter[1] ? -RT_ERROR : RT_EOK;
    if (length >= 3U)
    {
        result_count = parameter[2];
        result_count_valid = RT_TRUE;
    }

    if (result_count_valid && context->scan_result_count_valid)
    {
        context->scan_expected_results = (rt_uint16_t)(
            context->scan_expected_results + result_count);
    }
    else
    {
        context->scan_result_count_valid = RT_FALSE;
    }

    if (status == RT_EOK && context->scan_followup_pending)
    {
        context->scan_followup_retry_count = 0;
        context->scan_work_queued = RT_TRUE;
        status = rt_work_submit(&context->scan_work, 0);
        if (status == RT_EOK)
        {
            return;
        }
        context->scan_work_queued = RT_FALSE;
        LOG_E("cannot queue 5 GHz scan: %d", status);
    }
    if (status != RT_EOK)
    {
        aic_report_scan_done_status(
            context, status, context->scan_result_count_valid,
            context->scan_expected_results);
        return;
    }
    context->scan_completion_status = status;
    context->scan_completion_retry_count = 0;
    context->scan_completion_pending = RT_TRUE;
    context->scan_work_queued = RT_TRUE;
    status = rt_work_submit(
        &context->scan_work,
        rt_tick_from_millisecond(AIC_SCAN_RESULT_DRAIN_RETRY_MS));
    if (status != RT_EOK)
    {
        context->scan_work_queued = RT_FALSE;
        LOG_W("cannot defer scan completion for late results: %d", status);
        aic_report_scan_done_status(
            context, RT_EOK, context->scan_result_count_valid,
            context->scan_expected_results);
    }
}

static void aic_scan_work(struct rt_work *work, void *work_data)
{
    struct aic8800_context *context = work_data;
    rt_uint16_t received;
    rt_err_t retry_result;
    rt_err_t result;

    (void)work;
    if (!context)
    {
        return;
    }
    context->scan_work_queued = RT_FALSE;
    if (!context->scan_request_id)
    {
        return;
    }
    if (context->scan_followup_pending)
    {
        if (!context->transport_connected || !context->attached ||
            context->radio.state != RT_WLAN_OFFLOAD_STARTED)
        {
            return;
        }
        context->scan_followup_pending = RT_FALSE;
        LOG_D("continuing scan on 5 GHz (%u channels)",
              context->scan_followup.channel_count);
        result = aic_submit_scan_wire(context, &context->scan_followup);
        if (result == -RT_EFULL &&
            context->scan_followup_retry_count <
                AIC_SCAN_FOLLOWUP_MAX_RETRIES &&
            context->scan_request_id && context->transport_connected &&
            context->attached &&
            context->radio.state == RT_WLAN_OFFLOAD_STARTED)
        {
            context->scan_followup_retry_count++;
            context->scan_followup_pending = RT_TRUE;
            context->scan_work_queued = RT_TRUE;
            retry_result = rt_work_submit(
                &context->scan_work,
                rt_tick_from_millisecond(AIC_SCAN_FOLLOWUP_RETRY_MS));
            if (retry_result == RT_EOK)
            {
                return;
            }
            context->scan_work_queued = RT_FALSE;
            context->scan_followup_pending = RT_FALSE;
            result = retry_result;
        }
        if (result != RT_EOK)
        {
            LOG_E("5 GHz scan start failed: %d", result);
            aic_report_scan_done_status(context, result, RT_FALSE, 0);
        }
        return;
    }
    if (!context->scan_completion_pending)
    {
        return;
    }
    received = context->scan_results_2ghz + context->scan_results_5ghz;
    if (context->scan_result_count_valid &&
        received < context->scan_expected_results &&
        context->scan_completion_retry_count <
            AIC_SCAN_RESULT_DRAIN_MAX_RETRIES &&
        context->transport_connected && context->attached &&
        context->radio.state == RT_WLAN_OFFLOAD_STARTED)
    {
        context->scan_completion_retry_count++;
        context->scan_work_queued = RT_TRUE;
        result = rt_work_submit(
            &context->scan_work,
            rt_tick_from_millisecond(AIC_SCAN_RESULT_DRAIN_RETRY_MS));
        if (result == RT_EOK)
        {
            return;
        }
        context->scan_work_queued = RT_FALSE;
        LOG_W("cannot continue scan result drain: %d", result);
    }
    if (context->scan_result_count_valid &&
        received < context->scan_expected_results)
    {
        LOG_W("scan result drain timed out: firmware=%u received=%u",
              context->scan_expected_results, received);
    }
    aic_report_scan_done_status(
        context, context->scan_completion_status,
        context->scan_result_count_valid, context->scan_expected_results);
}

static void aic_report_connect(struct aic8800_context *context,
                               const rt_uint8_t *parameter,
                               rt_size_t length)
{
    struct rt_wlan_offload_event event;
    rt_uint16_t status;
    rt_uint8_t old_bssid[6];
    rt_bool_t was_connected;
    rt_bool_t roamed;

    if (!context || !parameter ||
        length <= AIC_SM_CONNECT_IND_ACM_OFFSET)
    {
        return;
    }
    aic_rx_reorder_reset(context);
    aic_tcp_ack_reset(context);
    was_connected = context->station_connected;
    roamed = parameter[8] != 0;
    rt_memcpy(old_bssid, context->bssid, sizeof(old_bssid));
    status = aic_get_le16(parameter);
    context->station_connected = status == 0;
    context->station_control_port_open = RT_FALSE;
    if (context->wep_enabled)
    {
        context->wep_auth_error = status == 13U;
    }
    if (!status)
    {
        rt_memcpy(context->bssid, parameter + 2, 6);
        context->ap_station_index =
            parameter[AIC_SM_CONNECT_IND_STATION_OFFSET];
        context->station_qos =
            parameter[AIC_SM_CONNECT_IND_QOS_OFFSET] ? RT_TRUE : RT_FALSE;
        context->station_acm = parameter[AIC_SM_CONNECT_IND_ACM_OFFSET];
        aic_tx_credit_reset(context, context->ap_station_index);
        context->station_channel_index =
            parameter[AIC_SM_CONNECT_IND_CHANNEL_INDEX_OFFSET];
        context->rssi = 0;
        aic_update_connected_channel(context, parameter, length);
        aic_channel_context_report(context, "station associated");
    }
    else
    {
        LOG_W("association failed: IEEE status=%u", (unsigned int)status);
        if (was_connected)
        {
            context->station_interface_recycle_pending = RT_TRUE;
        }
        context->ap_station_index = AIC8800_INVALID_INDEX;
        context->station_qos = RT_FALSE;
        context->station_acm = 0;
        context->station_channel_index = AIC8800_INVALID_INDEX;
        context->current_channel_valid = RT_FALSE;
        context->rssi = 0;
        rt_memset(context->bssid, 0, sizeof(context->bssid));
        rt_memset(&context->auth, 0, sizeof(context->auth));
        aic_clear_vif_hardware_keys(context,
                                    RT_WLAN_OFFLOAD_IFTYPE_STATION);
    }
    if (status)
    {
        context->station_control_port_pending = RT_FALSE;
    }
    if (context->ap_paused_for_station &&
        (!context->station_control_port_pending || status))
    {
        aic_schedule_ap_resume(context, status == 0);
    }

    /* A reconnect/roam indication can arrive without a pending connect
     * request.  Keep the cached link state current, but only complete a
     * request which this host actually submitted. */
    if (!context->connect_request_id)
    {
        if (status && (roamed || was_connected) && context->attached &&
            context->radio.state == RT_WLAN_OFFLOAD_STARTED)
        {
            rt_memset(&event, 0, sizeof(event));
            event.type = RT_WLAN_OFFLOAD_EVENT_DISCONNECTED;
            event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
            event.status = RT_EOK;
            event.data.disconnected.reason = 0;
            event.data.disconnected.locally_generated = RT_FALSE;
            rt_memcpy(event.data.disconnected.bssid, old_bssid, 6);
            (void)rt_wlan_offload_report_event(&context->radio, &event);
        }
        return;
    }
    if (!context->attached || context->radio.state != RT_WLAN_OFFLOAD_STARTED)
    {
        context->connect_request_id = 0;
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = context->connect_request_id;
    event.status = status ? -RT_ERROR : RT_EOK;
    if (!status)
    {
        rt_memcpy(event.data.network.bssid, context->bssid, 6);
    }
    context->connect_request_id = 0;
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void aic_report_disconnect(struct aic8800_context *context,
                                  const rt_uint8_t *parameter,
                                  rt_size_t length)
{
    struct rt_wlan_offload_event event;
    rt_bool_t was_connected;

    if (!context || !parameter || length < 2)
    {
        return;
    }
    aic_rx_reorder_reset(context);
    aic_tcp_ack_reset(context);
    was_connected = context->station_connected;
    context->station_connected = RT_FALSE;
    if (was_connected)
    {
        /* This firmware allocates a new peer index after a reconnect but can
         * leave the old peer's BlockAck state attached to the VIF.  Recreate
         * the station VIF before the next association so TX aggregation does
         * not remain at one MPDU per A-MPDU. */
        context->station_interface_recycle_pending = RT_TRUE;
    }
    context->station_control_port_open = RT_FALSE;
    context->ap_station_index = AIC8800_INVALID_INDEX;
    context->station_qos = RT_FALSE;
    context->station_acm = 0;
    context->station_channel_index = AIC8800_INVALID_INDEX;
    context->connect_request_id = 0;
    context->station_control_port_pending = RT_FALSE;
    context->current_channel_valid = RT_FALSE;
    context->rssi = 0;
    rt_memset(&context->auth, 0, sizeof(context->auth));
    aic_clear_vif_hardware_keys(context, RT_WLAN_OFFLOAD_IFTYPE_STATION);
    if (context->ap_paused_for_station)
    {
        aic_schedule_ap_resume(context, RT_FALSE);
    }
    if (!context->attached || context->radio.state != RT_WLAN_OFFLOAD_STARTED)
    {
        context->disconnect_request_id = 0;
        rt_memset(context->bssid, 0, sizeof(context->bssid));
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_DISCONNECTED;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    event.request_id = context->disconnect_request_id;
    event.status = RT_EOK;
    event.data.disconnected.reason = aic_get_le16(parameter);
    event.data.disconnected.locally_generated =
        context->disconnect_request_id != 0;
    rt_memcpy(event.data.disconnected.bssid, context->bssid, 6);
    context->disconnect_request_id = 0;
    rt_memset(context->bssid, 0, sizeof(context->bssid));
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void aic_report_firmware_fault(struct aic8800_context *context,
                                      rt_uint16_t id,
                                      const rt_uint8_t *parameter,
                                      rt_size_t length)
{
    struct rt_wlan_offload_event event;
    const rt_uint8_t *info = RT_NULL;
    rt_size_t info_length = 0;
    rt_size_t log_length;

    if (!context)
    {
        return;
    }
    if (parameter && length >= sizeof(rt_uint32_t))
    {
        info_length = aic_get_le32(parameter);
        if (info_length > length - sizeof(rt_uint32_t))
        {
            info_length = length - sizeof(rt_uint32_t);
        }
        if (info_length > sizeof(((struct aic_wire_mm_firmware_fault_ind *)0)->info))
        {
            info_length =
                sizeof(((struct aic_wire_mm_firmware_fault_ind *)0)->info);
        }
        info = parameter + sizeof(rt_uint32_t);
    }
    log_length = id == AIC_MM_FW_PANIC_IND && info_length > 36U ?
                 36U : info_length;
    LOG_E("firmware %s (message=0x%04x length=%u): %.*s",
          id == AIC_MM_FW_PANIC_IND ? "panic" : "assert", id,
          (unsigned int)info_length, (int)log_length,
          info ? (const char *)info : "");

    rt_wlan_offload_command_manager_fail(&context->commands, -RT_EIO);
    aic_cancel_mgmt_confirmations(context, -RT_EIO);
    if (!context->attached ||
        context->radio.state == RT_WLAN_OFFLOAD_UNREGISTERED ||
        context->radio.state == RT_WLAN_OFFLOAD_FAILED)
    {
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_MAX;
    event.status = -RT_EIO;
    event.data.firmware.reason = id;
    event.data.firmware.dump = info;
    event.data.firmware.dump_length = info_length;
    (void)rt_wlan_offload_report_event(&context->radio, &event);
}

static void aic_report_tkip_mic_failure(
    struct aic8800_context *context,
    const struct aic_wire_me_tkip_mic_failure_ind *indication)
{
    struct rt_wlan_offload_event event;

    if (!context || !indication || !context->attached ||
        context->radio.state != RT_WLAN_OFFLOAD_STARTED)
    {
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_TKIP_MIC_FAILURE;
    if (indication->vif_index == context->vif_index)
    {
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
    }
    else if (indication->vif_index == context->ap_vif_index)
    {
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
    }
    else
    {
        LOG_W("TKIP MIC failure for unknown VIF %u",
              (unsigned int)indication->vif_index);
        return;
    }
    event.status = RT_EOK;
    rt_memcpy(event.data.mic_failure.source, indication->address, 6);
    rt_memcpy(event.data.mic_failure.tsc, indication->tsc, 8);
    event.data.mic_failure.key_index = indication->key_index;
    event.data.mic_failure.group = indication->group_addressed != 0;
    (void)rt_wlan_offload_report_event(&context->radio, &event);
}

static void aic_schedule_station_loss(
    struct aic8800_context *context,
    const struct aic_wire_mm_apm_staloss_ind *indication)
{
    rt_size_t free_index = AIC8800_AP_STATION_COUNT;
    rt_bool_t submit = RT_FALSE;
    rt_base_t level;

    if (!context || !indication)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    for (rt_size_t index = 0; index < AIC8800_AP_STATION_COUNT; index++)
    {
        if (context->station_loss[index].pending &&
            rt_memcmp(context->station_loss[index].address,
                      indication->address, 6) == 0)
        {
            free_index = index;
            break;
        }
        if (!context->station_loss[index].pending &&
            free_index == AIC8800_AP_STATION_COUNT)
        {
            free_index = index;
        }
    }
    if (free_index < AIC8800_AP_STATION_COUNT)
    {
        struct aic8800_station_loss *loss =
            &context->station_loss[free_index];

        loss->pending = RT_TRUE;
        loss->vif_index = indication->vif_index;
        loss->station_index = indication->station_index;
        rt_memcpy(loss->address, indication->address, 6);
        if (context->station_loss_work_initialized &&
            !context->station_loss_work_queued && context->attached)
        {
            context->station_loss_work_queued = RT_TRUE;
            submit = RT_TRUE;
        }
    }
    rt_hw_interrupt_enable(level);
    if (submit && rt_work_submit(&context->station_loss_work, 0) != RT_EOK)
    {
        level = rt_hw_interrupt_disable();
        context->station_loss_work_queued = RT_FALSE;
        if (free_index < AIC8800_AP_STATION_COUNT)
        {
            context->station_loss[free_index].pending = RT_FALSE;
        }
        rt_hw_interrupt_enable(level);
        LOG_E("cannot queue AP station-loss handling");
    }
    else if (free_index == AIC8800_AP_STATION_COUNT)
    {
        LOG_W("AP station-loss queue full");
    }
}

static void aic_station_loss_work(struct rt_work *work, void *work_data)
{
    struct aic8800_context *context = work_data;

    (void)work;
    while (context)
    {
        struct aic8800_station_loss loss;
        struct rt_wlan_offload_event event;
        rt_uint16_t aid = 0;
        rt_bool_t found = RT_FALSE;
        rt_base_t level;
        rt_err_t result;

        rt_memset(&loss, 0, sizeof(loss));
        level = rt_hw_interrupt_disable();
        for (rt_size_t index = 0; index < AIC8800_AP_STATION_COUNT; index++)
        {
            if (context->station_loss[index].pending)
            {
                loss = context->station_loss[index];
                context->station_loss[index].pending = RT_FALSE;
                break;
            }
        }
        if (!loss.pending)
        {
            context->station_loss_work_queued = RT_FALSE;
            rt_hw_interrupt_enable(level);
            return;
        }
        rt_hw_interrupt_enable(level);

        if (!context->attached || !context->ap_started ||
            loss.vif_index != context->ap_vif_index)
        {
            continue;
        }
        if (rt_mutex_take(context->frame_mutex, RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }
        for (rt_size_t index = 0; index < AIC8800_AP_STATION_COUNT; index++)
        {
            const struct aic8800_ap_station *station =
                &context->ap_stations[index];

            if (station->valid &&
                station->firmware_index == loss.station_index &&
                rt_memcmp(station->address, loss.address, 6) == 0)
            {
                aid = station->aid;
                found = RT_TRUE;
                break;
            }
        }
        rt_mutex_release(context->frame_mutex);
        if (!found)
        {
            LOG_D("ignored stale AP station-loss indication for index %u",
                  (unsigned int)loss.station_index);
            continue;
        }
        result = aic_del_station_internal(
            &context->radio.vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX], 0,
            loss.address, 4U, RT_TRUE);
        if (result != RT_EOK)
        {
            LOG_W("firmware station-loss cleanup failed for "
                  "%02x:%02x:%02x:%02x:%02x:%02x",
                  loss.address[0], loss.address[1], loss.address[2],
                  loss.address[3], loss.address[4], loss.address[5]);
        }
        rt_memset(&event, 0, sizeof(event));
        event.type = RT_WLAN_OFFLOAD_EVENT_DEL_STATION;
        event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
        event.status = RT_EOK;
        rt_memcpy(event.data.station.mac, loss.address, 6);
        event.data.station.aid = aid;
        (void)rt_wlan_offload_report_event(&context->radio, &event);
    }
}

static rt_bool_t aic_command_is_confirmation(rt_uint16_t id)
{
    switch (id)
    {
    case AIC_MM_RESET_CFM:
    case AIC_MM_START_CFM:
    case AIC_MM_VERSION_CFM:
    case AIC_MM_ADD_IF_CFM:
    case AIC_MM_REMOVE_IF_CFM:
    case AIC_MM_SET_FILTER_CFM:
    case AIC_MM_SET_CHANNEL_CFM:
    case AIC_MM_KEY_ADD_CFM:
    case AIC_MM_KEY_DEL_CFM:
    case AIC_MM_SET_RF_CONFIG_CFM:
    case AIC_MM_SET_RF_CALIB_CFM:
    case AIC_MM_GET_MAC_CFM:
    case AIC_MM_SET_TXPWR_CFM:
    case AIC_MM_SET_TXPWR_OFST_CFM:
    case AIC_MM_SET_STACK_START_CFM:
    case AIC_MM_GET_STA_INFO_CFM:
    case AIC_MM_GET_FW_VERSION_CFM:
    case AIC_MM_SET_TXPWR_ADJ_CFM:
    case AIC_MM_SET_COEX_CFM:
    case AIC_DBG_MEM_MASK_WRITE_CFM:
    case AIC_SCANU_START_CFM:
    case AIC_SCANU_START_ACCEPTED:
    case AIC_SCANU_VENDOR_IE_CFM:
    case AIC_SCANU_CANCEL_CFM:
    case AIC_ME_CONFIG_CFM:
    case AIC_ME_CHAN_CONFIG_CFM:
    case AIC_ME_CONTROL_PORT_CFM:
    case AIC_ME_STA_ADD_CFM:
    case AIC_ME_STA_DEL_CFM:
    case AIC_ME_TRAFFIC_IND_CFM:
    case AIC_ME_RC_STATS_CFM:
    case AIC_ME_SET_PS_MODE_CFM:
    case AIC_SM_CONNECT_CFM:
    case AIC_SM_DISCONNECT_CFM:
    case AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP_CFM:
    case AIC_APM_START_CFM:
    case AIC_APM_STOP_CFM:
    case AIC_APM_SET_BEACON_IE_CFM:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static rt_err_t aic_handle_command(struct aic8800_context *context,
                                   const rt_uint8_t *frame,
                                   rt_size_t length)
{
    rt_uint16_t id;
    rt_uint16_t parameter_length;
    const rt_uint8_t *parameter;

    if (!context || !frame || length < 16)
    {
        return -RT_EIO;
    }
    id = aic_get_le16(frame + 4);
    parameter_length = aic_get_le16(frame + 10);
    if ((rt_size_t)parameter_length > length - 16U)
    {
        return -RT_EIO;
    }
    parameter = frame + 16;
    if (context->command_rx_log_count < 16U)
    {
        context->command_rx_log_count++;
        LOG_D("firmware RX 0x%04x, parameter=%u, transport=%u",
              id, (unsigned int)parameter_length,
              (unsigned int)length);
    }
    switch (id)
    {
    case AIC_SCANU_RESULT_IND:
        aic_report_scan_result(context, parameter, parameter_length);
        break;
    case AIC_SCANU_START_CFM:
        aic_handle_scan_done(context, parameter, parameter_length);
        break;
    case AIC_MM_APM_STALOSS_IND:
        if (parameter_length >=
            sizeof(struct aic_wire_mm_apm_staloss_ind))
        {
            aic_schedule_station_loss(
                context,
                (const struct aic_wire_mm_apm_staloss_ind *)parameter);
        }
        else
        {
            LOG_E("short AP station-loss indication: %u",
                  (unsigned int)parameter_length);
        }
        break;
    case AIC_MM_FW_PANIC_IND:
    case AIC_MM_FW_ASSERT_IND:
        aic_report_firmware_fault(context, id, parameter, parameter_length);
        break;
    case AIC_MM_PS_CHANGE_IND:
        if (parameter_length >= sizeof(struct aic_wire_mm_ps_change_ind))
        {
            const struct aic_wire_mm_ps_change_ind *indication =
                (const struct aic_wire_mm_ps_change_ind *)parameter;

            if (indication->station_index < AIC8800_STATION_SLOTS)
            {
                rt_bool_t buffered[AIC8800_PS_ID_COUNT] = {RT_FALSE};
                rt_base_t level = rt_hw_interrupt_disable();

                if (context->sta_present[indication->station_index])
                {
                    rt_bool_t power_save =
                        indication->power_save ? RT_TRUE : RT_FALSE;

                    if (context->sta_power_save[indication->station_index] !=
                        power_save)
                    {
                        for (rt_size_t ps_id = 0;
                             ps_id < AIC8800_PS_ID_COUNT; ps_id++)
                        {
                            context->sta_service_period_generation[
                                indication->station_index][ps_id]++;
                            if (!context->sta_service_period_generation[
                                    indication->station_index][ps_id])
                            {
                                context->sta_service_period_generation[
                                    indication->station_index][ps_id]++;
                            }
                            context->sta_service_period_remaining[
                                indication->station_index][ps_id] = 0;
                            context->sta_service_period_reserved[
                                indication->station_index][ps_id] = 0;
                        }
                    }
                    context->sta_power_save[indication->station_index] =
                        power_save;
                    for (rt_size_t ps_id = 0;
                         ps_id < AIC8800_PS_ID_COUNT; ps_id++)
                    {
                        buffered[ps_id] = context->sta_buffered[
                            indication->station_index][ps_id] != 0;
                    }
                }
                else if (context->station_add_pending)
                {
                    context->sta_add_power_save[indication->station_index] =
                        indication->power_save ? RT_TRUE : RT_FALSE;
                }
                rt_hw_interrupt_enable(level);
                for (rt_size_t ps_id = 0;
                     ps_id < AIC8800_PS_ID_COUNT; ps_id++)
                {
                    aic_set_traffic_status(
                        context, indication->station_index, (rt_uint8_t)ps_id,
                        indication->power_save && buffered[ps_id]);
                }
                LOG_D("station %u power save %s",
                      (unsigned int)indication->station_index,
                      indication->power_save ? "on" : "off");
            }
        }
        break;
    case AIC_MM_TRAFFIC_REQ_IND:
        if (parameter_length >= sizeof(struct aic_wire_mm_traffic_req_ind))
        {
            const struct aic_wire_mm_traffic_req_ind *indication =
                (const struct aic_wire_mm_traffic_req_ind *)parameter;

            if (indication->station_index < AIC8800_STATION_SLOTS)
            {
                rt_uint16_t granted = 0;
                rt_uint8_t ps_id = indication->uapsd ?
                    AIC_PS_ID_UAPSD : AIC_PS_ID_LEGACY;
                rt_bool_t interrupted = RT_FALSE;
                rt_base_t level = rt_hw_interrupt_disable();

                if (context->sta_present[indication->station_index] &&
                    context->sta_power_save[indication->station_index])
                {
                    if (indication->uapsd &&
                        indication->packet_count == AIC_PS_SP_INTERRUPTED)
                    {
                        context->sta_service_period_remaining[
                            indication->station_index][ps_id] = 0;
                        context->sta_service_period_reserved[
                            indication->station_index][ps_id] = 0;
                        context->sta_service_period_generation[
                            indication->station_index][ps_id]++;
                        if (!context->sta_service_period_generation[
                                indication->station_index][ps_id])
                        {
                            context->sta_service_period_generation[
                                indication->station_index][ps_id]++;
                        }
                        interrupted = RT_TRUE;
                    }
                    else if (!context->sta_service_period_remaining[
                                  indication->station_index][ps_id] &&
                             !context->sta_service_period_reserved[
                                  indication->station_index][ps_id])
                    {
                        rt_uint16_t pending = context->sta_buffered[
                            indication->station_index][ps_id];

                        granted = indication->packet_count ?
                            indication->packet_count : pending;
                        if (granted > pending)
                        {
                            granted = pending;
                        }
                        if (granted)
                        {
                            context->sta_service_period_generation[
                                indication->station_index][ps_id]++;
                            if (!context->sta_service_period_generation[
                                    indication->station_index][ps_id])
                            {
                                context->sta_service_period_generation[
                                    indication->station_index][ps_id]++;
                            }
                            context->sta_service_period_remaining[
                                indication->station_index][ps_id] = granted;
                        }
                    }
                }
                rt_hw_interrupt_enable(level);
                LOG_D("station %u %s service period: requested=%u granted=%u",
                      (unsigned int)indication->station_index,
                      indication->uapsd ? "U-APSD" : "legacy",
                      (unsigned int)indication->packet_count,
                      interrupted ? 0U : (unsigned int)granted);
            }
        }
        break;
    case AIC_MM_CHANNEL_PRE_SWITCH_IND:
        /* The context named here is about to leave the air.  Vendor Linux
         * stops every VIF bound to it before the switch completes. */
        if (parameter_length >=
            sizeof(struct aic_wire_mm_channel_pre_switch_ind))
        {
            const struct aic_wire_mm_channel_pre_switch_ind *indication =
                (const struct aic_wire_mm_channel_pre_switch_ind *)parameter;

            context->channel_context_tracked = RT_TRUE;
            context->active_channel_index = AIC8800_INVALID_INDEX;
            LOG_D("channel context %u pre-switch",
                  (unsigned int)indication->channel_index);
        }
        break;
    case AIC_MM_CHANNEL_SWITCH_IND:
        if (parameter_length >=
            sizeof(struct aic_wire_mm_channel_switch_ind))
        {
            const struct aic_wire_mm_channel_switch_ind *indication =
                (const struct aic_wire_mm_channel_switch_ind *)parameter;

            /* A remain-on-channel switch parks the radio on a scan/offchannel
             * context that belongs to no VIF; leaving the active index unset
             * keeps both VIFs gated until the radio comes back. */
            context->channel_context_tracked = RT_TRUE;
            context->active_channel_index = indication->remain_on_channel ?
                AIC8800_INVALID_INDEX : indication->channel_index;
            LOG_D("channel context %u active (roc=%u)",
                  (unsigned int)indication->channel_index,
                  (unsigned int)indication->remain_on_channel);
        }
        break;
    case AIC_ME_TX_CREDITS_UPDATE_IND:
        if (parameter_length >=
            sizeof(struct aic_wire_me_tx_credits_update_ind))
        {
            const struct aic_wire_me_tx_credits_update_ind *indication =
                (const struct aic_wire_me_tx_credits_update_ind *)parameter;

            aic_tx_credit_update(context, indication->station_index,
                                 indication->credits);
            context->tx_credit_update_count++;
            LOG_D("station %u tid %u credits %+d",
                  (unsigned int)indication->station_index,
                  (unsigned int)indication->tid, (int)indication->credits);
        }
        break;
    case AIC_ME_TKIP_MIC_FAILURE_IND:
        if (parameter_length >=
            sizeof(struct aic_wire_me_tkip_mic_failure_ind))
        {
            aic_report_tkip_mic_failure(
                context,
                (const struct aic_wire_me_tkip_mic_failure_ind *)parameter);
        }
        else
        {
            LOG_E("short TKIP MIC failure indication: %u",
                  (unsigned int)parameter_length);
        }
        break;
    case AIC_SM_CONNECT_IND:
        aic_report_connect(context, parameter, parameter_length);
        break;
    case AIC_SM_DISCONNECT_IND:
        aic_report_disconnect(context, parameter, parameter_length);
        break;
    case AIC_SM_EXTERNAL_AUTH_REQUIRED_IND:
        if (parameter_length >=
            sizeof(struct aic_wire_sm_external_auth_required_ind))
        {
            const struct aic_wire_sm_external_auth_required_ind *indication =
                (const struct aic_wire_sm_external_auth_required_ind *)parameter;
            struct rt_wlan_offload_event event;

            rt_memset(&event, 0, sizeof(event));
            event.type = RT_WLAN_OFFLOAD_EVENT_EXTERNAL_AUTH_REQUIRED;
            event.iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
            event.request_id = context->connect_request_id;
            event.status = RT_EOK;
            event.data.external_auth.ssid.len = indication->ssid.length;
            if (event.data.external_auth.ssid.len > RT_WLAN_SSID_MAX_LENGTH)
            {
                event.data.external_auth.ssid.len = RT_WLAN_SSID_MAX_LENGTH;
            }
            rt_memcpy(event.data.external_auth.ssid.val,
                      indication->ssid.array,
                      event.data.external_auth.ssid.len);
            rt_memcpy(event.data.external_auth.bssid,
                      indication->bssid.array, 6);
            event.data.external_auth.akm_suite = indication->akm;
            if (context->connect_request_id && context->attached &&
                context->radio.state == RT_WLAN_OFFLOAD_STARTED)
            {
                rt_wlan_offload_report_event(&context->radio, &event);
            }
        }
        else
        {
            LOG_E("short external-auth indication: %u",
                  (unsigned int)parameter_length);
        }
        break;
    default:
        break;
    }
    /* Indications and unsolicited firmware diagnostics share the command
     * transport.  Completing a pending transaction for every ID can wake a
     * caller with an unrelated event, especially after a timeout. */
    if (aic_command_is_confirmation(id))
    {
        rt_wlan_offload_command_complete(&context->commands, 0, id, RT_EOK,
                                    parameter, parameter_length);
    }
    return RT_EOK;
}

static rt_err_t aic_report_management(struct aic8800_context *context,
                                      const rt_uint8_t *record,
                                      rt_size_t length)
{
    struct rt_wlan_offload_event event;
    rt_uint16_t frame_length;
    rt_uint16_t frequency;
    enum rt_wlan_offload_iftype iftype;

    if (!context || !record)
    {
        return -RT_EINVAL;
    }
    if (!context->attached || context->radio.state != RT_WLAN_OFFLOAD_STARTED)
    {
        return RT_EOK;
    }
    if (length < AIC8800_USB_RX_HEADER_SIZE)
    {
        return -RT_EIO;
    }
    frame_length = aic_record_payload_length(context, record);
    if (frame_length < 24U ||
        frame_length > length - AIC8800_USB_RX_HEADER_SIZE)
    {
        return -RT_EIO;
    }
    frequency = aic_get_le16(record + 42);
    if (!aic_frequency_to_channel(frequency))
    {
        return -RT_EINVAL;
    }
    iftype = record[49] == context->ap_vif_index ?
             RT_WLAN_OFFLOAD_IFTYPE_AP : RT_WLAN_OFFLOAD_IFTYPE_STATION;
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_MGMT_RX;
    event.iftype = iftype;
    event.status = RT_EOK;
    aic_channel_definition(frequency, &event.data.management.channel);
    event.data.management.rssi = (rt_int8_t)record[14];
    event.data.management.data = record + AIC8800_USB_RX_HEADER_SIZE;
    event.data.management.length = frame_length;
    return rt_wlan_offload_report_event(&context->radio, &event);
}

/* RT-Thread exposes a byte pattern/mask filter while the AIC firmware only
 * exposes coarse MAC RX filter bits.  Keep the firmware filter at its normal
 * data-safe setting and apply the precise rule to the Ethernet frame in the
 * host.  The rule is copied by aic_set_filter(), so callers may release their
 * buffers as soon as the control request returns. */
static rt_bool_t aic_filter_accept(struct aic8800_context *context,
                                   const rt_uint8_t *frame,
                                   rt_size_t length)
{
    rt_bool_t enabled;
    rt_filter_rule_t rule;
    rt_uint16_t offset;
    rt_uint16_t pattern_length;
    rt_uint8_t mask[AIC8800_FILTER_PATTERN_MAX];
    rt_uint8_t pattern[AIC8800_FILTER_PATTERN_MAX];
    rt_size_t index;
    rt_bool_t matched = RT_FALSE;

    if (!context || !frame)
    {
        return RT_FALSE;
    }
    if (!context->attached)
    {
        /* The USB RX worker can finish a packet while detach is tearing
         * down the radio.  Do not deliver it after unregister. */
        return RT_FALSE;
    }
    if (!context->filter_enabled)
    {
        /* No rule is installed, so there is nothing to copy under the state
         * lock.  rt_wlan_offload_rx() takes the same mutex immediately after
         * this call; skipping it here halves the per-frame lock traffic on
         * the receive path, which no filter can affect anyway.  A rule being
         * installed concurrently takes effect from the next frame. */
        return RT_TRUE;
    }
    if (rt_mutex_take(&context->radio.operation_lock,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        /* Never drop traffic because the state lock is temporarily
         * unavailable.  The caller will still validate the frame normally. */
        return RT_TRUE;
    }
    enabled = context->filter_enabled;
    rule = context->filter_rule;
    offset = context->filter_offset;
    pattern_length = context->filter_length;
    if (enabled && pattern_length <= AIC8800_FILTER_PATTERN_MAX)
    {
        rt_memcpy(mask, context->filter_mask, pattern_length);
        rt_memcpy(pattern, context->filter_pattern, pattern_length);
    }
    rt_mutex_release(&context->radio.operation_lock);

    if (!enabled || !pattern_length ||
        pattern_length > AIC8800_FILTER_PATTERN_MAX)
    {
        return RT_TRUE;
    }
    if (offset > length || pattern_length > length - offset)
    {
        return rule == RT_NEGATIVE_MATCHING;
    }
    for (index = 0; index < pattern_length; index++)
    {
        if ((frame[offset + index] & mask[index]) !=
            (pattern[index] & mask[index]))
        {
            break;
        }
    }
    matched = index == pattern_length;
    return rule == RT_POSITIVE_MATCHING ? matched : !matched;
}

#ifdef AIC8800_WIFI_TCP_ACK_FILTER
struct aic_tcp_packet
{
    rt_uint32_t source_address;
    rt_uint32_t destination_address;
    rt_uint32_t sequence;
    rt_uint32_t acknowledgement;
    rt_uint16_t source_port;
    rt_uint16_t destination_port;
    rt_uint16_t window;
    rt_uint16_t payload_length;
    rt_uint8_t flags;
};

static rt_uint16_t aic_tcp_get_be16(const rt_uint8_t *data)
{
    return ((rt_uint16_t)data[0] << 8) | data[1];
}

static rt_uint32_t aic_tcp_get_be32(const rt_uint8_t *data)
{
    return ((rt_uint32_t)data[0] << 24) |
           ((rt_uint32_t)data[1] << 16) |
           ((rt_uint32_t)data[2] << 8) | data[3];
}

static rt_bool_t aic_tcp_parse(const rt_uint8_t *ethernet,
                               rt_size_t length,
                               struct aic_tcp_packet *packet)
{
    const rt_uint8_t *ip;
    const rt_uint8_t *tcp;
    rt_uint16_t fragment;
    rt_uint16_t ip_length;
    rt_size_t ip_header_length;
    rt_size_t tcp_header_length;

    if (!ethernet || !packet || length < 14U + 20U + 20U ||
        aic_tcp_get_be16(ethernet + 12U) != 0x0800U)
    {
        return RT_FALSE;
    }
    ip = ethernet + 14U;
    ip_header_length = (rt_size_t)(ip[0] & 0x0fU) * 4U;
    ip_length = aic_tcp_get_be16(ip + 2U);
    fragment = aic_tcp_get_be16(ip + 6U);
    if ((ip[0] >> 4) != 4U || ip_header_length < 20U ||
        ip_length < ip_header_length + 20U ||
        ip_length > length - 14U || ip[9] != 6U ||
        (fragment & 0x3fffU) != 0U)
    {
        return RT_FALSE;
    }
    tcp = ip + ip_header_length;
    tcp_header_length = (rt_size_t)(tcp[12] >> 4) * 4U;
    if (tcp_header_length < 20U ||
        tcp_header_length > ip_length - ip_header_length)
    {
        return RT_FALSE;
    }
    packet->source_address = aic_tcp_get_be32(ip + 12U);
    packet->destination_address = aic_tcp_get_be32(ip + 16U);
    packet->source_port = aic_tcp_get_be16(tcp);
    packet->destination_port = aic_tcp_get_be16(tcp + 2U);
    packet->sequence = aic_tcp_get_be32(tcp + 4U);
    packet->acknowledgement = aic_tcp_get_be32(tcp + 8U);
    packet->flags = tcp[13];
    packet->window = aic_tcp_get_be16(tcp + 14U);
    packet->payload_length = (rt_uint16_t)(
        ip_length - ip_header_length - tcp_header_length);
    return RT_TRUE;
}

static rt_bool_t aic_tcp_sequence_after(rt_uint32_t sequence,
                                        rt_uint32_t previous)
{
    return (rt_int32_t)(sequence - previous) > 0;
}

static struct aic8800_tcp_ack_flow *aic_tcp_ack_find_locked(
    struct aic8800_context *context,
    const struct aic_tcp_packet *packet,
    enum rt_wlan_offload_iftype iftype, rt_bool_t allocate)
{
    struct aic8800_tcp_ack_flow *available = RT_NULL;
    rt_tick_t oldest_age = 0;
    rt_tick_t now = rt_tick_get();
    rt_tick_t timeout = rt_tick_from_millisecond(
        AIC8800_TCP_ACK_FLOW_TIMEOUT_MS);
    rt_size_t index;

    for (index = 0; index < AIC8800_TCP_ACK_FLOW_COUNT; index++)
    {
        struct aic8800_tcp_ack_flow *flow =
            &context->tcp_ack_flows[index];

        if (flow->valid && !flow->pending &&
            (rt_tick_t)(now - flow->last_seen) >= timeout)
        {
            rt_memset(flow, 0, sizeof(*flow));
        }
        if (flow->valid && flow->iftype == iftype &&
            flow->source_address == packet->source_address &&
            flow->destination_address == packet->destination_address &&
            flow->source_port == packet->source_port &&
            flow->destination_port == packet->destination_port)
        {
            flow->last_seen = now;
            return flow;
        }
        if (!flow->valid)
        {
            if (!available || available->valid)
            {
                available = flow;
            }
        }
        else if (!flow->pending)
        {
            rt_tick_t age = now - flow->last_seen;

            if (!available || (available->valid && age > oldest_age))
            {
                available = flow;
                oldest_age = age;
            }
        }
    }
    if (!allocate || !available)
    {
        return RT_NULL;
    }
    rt_memset(available, 0, sizeof(*available));
    available->valid = RT_TRUE;
    available->iftype = iftype;
    available->source_address = packet->source_address;
    available->destination_address = packet->destination_address;
    available->source_port = packet->source_port;
    available->destination_port = packet->destination_port;
    available->last_seen = now;
    return available;
}

static rt_err_t aic_tcp_ack_arm_locked(struct aic8800_context *context)
{
    rt_err_t result;

    if (context->tcp_ack_timer_armed)
    {
        return RT_EOK;
    }
    context->tcp_ack_timer_armed = RT_TRUE;
    result = rt_timer_start(&context->tcp_ack_timer);
    if (result != RT_EOK)
    {
        context->tcp_ack_timer_armed = RT_FALSE;
    }
    return result;
}

static rt_bool_t aic_tcp_ack_filter(
    struct aic8800_context *context, struct rt_wlan_offload_vif *vif,
    const rt_uint8_t *ethernet, rt_size_t length)
{
    struct aic8800_tcp_ack_flow *flow;
    struct aic_tcp_packet packet;
    rt_bool_t consume = RT_FALSE;

    if (!context || !vif || length > AIC8800_TCP_ACK_FRAME_MAX ||
        !aic_tcp_parse(ethernet, length, &packet) ||
        packet.payload_length != 0U || (packet.flags & 0x3fU) != 0x10U ||
        !context->tcp_ack_initialized ||
        rt_mutex_take(&context->tcp_ack_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return RT_FALSE;
    }
    if (!context->tcp_ack_initialized)
    {
        rt_mutex_release(&context->tcp_ack_mutex);
        return RT_FALSE;
    }
    flow = aic_tcp_ack_find_locked(context, &packet, vif->iftype, RT_TRUE);
    if (!flow)
    {
        rt_mutex_release(&context->tcp_ack_mutex);
        return RT_FALSE;
    }
    if (!flow->acknowledgement_valid)
    {
        flow->acknowledgement = packet.acknowledgement;
        flow->window = packet.window;
        flow->acknowledgement_valid = RT_TRUE;
    }
    else if (!aic_tcp_sequence_after(packet.acknowledgement,
                                     flow->acknowledgement))
    {
        flow->last_seen = rt_tick_get();
    }
    else if (flow->quick_ack || flow->window != packet.window)
    {
        flow->acknowledgement = packet.acknowledgement;
        flow->window = packet.window;
        flow->pending = RT_FALSE;
        flow->suppressed = 0;
        flow->quick_ack = RT_FALSE;
    }
    else
    {
        flow->acknowledgement = packet.acknowledgement;
        flow->window = packet.window;
        flow->suppressed++;
        if (flow->suppressed >= AIC8800_TCP_ACK_SUPPRESS_LIMIT)
        {
            flow->pending = RT_FALSE;
            flow->suppressed = 0;
            AIC8800_STAT(context->tcp_ack_flushed++);
        }
        else
        {
            flow->length = (rt_uint16_t)length;
            rt_memcpy(flow->data, ethernet, length);
            flow->pending = RT_TRUE;
            if (aic_tcp_ack_arm_locked(context) == RT_EOK)
            {
                AIC8800_STAT(context->tcp_ack_suppressed++);
                consume = RT_TRUE;
            }
            else
            {
                flow->pending = RT_FALSE;
                flow->suppressed = 0;
            }
        }
    }
    rt_mutex_release(&context->tcp_ack_mutex);
    return consume;
}

static void aic_tcp_ack_note_rx(
    struct aic8800_context *context, enum rt_wlan_offload_iftype iftype,
    const rt_uint8_t *ethernet, rt_size_t length)
{
    struct aic_tcp_packet packet;
    rt_size_t index;

    if (!context || !context->tcp_ack_initialized ||
        !aic_tcp_parse(ethernet, length, &packet) ||
        !(packet.flags & 0x08U) || !packet.payload_length ||
        rt_mutex_take(&context->tcp_ack_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return;
    }
    for (index = 0; index < AIC8800_TCP_ACK_FLOW_COUNT; index++)
    {
        struct aic8800_tcp_ack_flow *flow =
            &context->tcp_ack_flows[index];

        if (flow->valid && flow->iftype == iftype &&
            flow->source_address == packet.destination_address &&
            flow->destination_address == packet.source_address &&
            flow->source_port == packet.destination_port &&
            flow->destination_port == packet.source_port)
        {
            flow->quick_ack = RT_TRUE;
            break;
        }
    }
    rt_mutex_release(&context->tcp_ack_mutex);
}

static void aic_tcp_ack_flush_work(struct rt_work *work, void *work_data)
{
    struct aic8800_context *context = work_data;
    rt_size_t index;

    (void)work;
    for (index = 0; context && context->tcp_ack_initialized &&
                    index < AIC8800_TCP_ACK_FLOW_COUNT; index++)
    {
        struct rt_wlan_offload_vif *vif = RT_NULL;
        rt_uint8_t ethernet[AIC8800_TCP_ACK_FRAME_MAX];
        rt_size_t length = 0;

        if (rt_mutex_take(&context->tcp_ack_mutex,
                          RT_WAITING_FOREVER) != RT_EOK)
        {
            break;
        }
        if (context->tcp_ack_initialized &&
            context->tcp_ack_flows[index].pending)
        {
            struct aic8800_tcp_ack_flow *flow =
                &context->tcp_ack_flows[index];
            rt_uint8_t vif_index =
                flow->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
                RT_WLAN_OFFLOAD_VIF_STA_INDEX :
                RT_WLAN_OFFLOAD_VIF_AP_INDEX;

            length = flow->length;
            rt_memcpy(ethernet, flow->data, length);
            vif = &context->radio.vifs[vif_index];
            flow->pending = RT_FALSE;
            flow->suppressed = 0;
            AIC8800_STAT(context->tcp_ack_flushed++);
        }
        rt_mutex_release(&context->tcp_ack_mutex);
        if (vif && length)
        {
            (void)aic_transmit_frame(context, vif, ethernet, length,
                                     RT_FALSE, RT_FALSE, 0);
        }
    }
    if (context)
    {
        rt_base_t level = rt_hw_interrupt_disable();

        context->tcp_ack_work_queued = RT_FALSE;
        rt_hw_interrupt_enable(level);
        if (context->tcp_ack_initialized &&
            rt_mutex_take(&context->tcp_ack_mutex,
                          RT_WAITING_FOREVER) == RT_EOK)
        {
            for (index = 0; index < AIC8800_TCP_ACK_FLOW_COUNT; index++)
            {
                if (context->tcp_ack_flows[index].pending)
                {
                    (void)aic_tcp_ack_arm_locked(context);
                    break;
                }
            }
            rt_mutex_release(&context->tcp_ack_mutex);
        }
    }
}

static void aic_tcp_ack_timeout(void *parameter)
{
    struct aic8800_context *context = parameter;
    rt_base_t level;
    rt_bool_t submit = RT_FALSE;

    if (!context)
    {
        return;
    }
    if (context->tcp_ack_mutex_initialized &&
        rt_mutex_take(&context->tcp_ack_mutex,
                      RT_WAITING_FOREVER) == RT_EOK)
    {
        context->tcp_ack_timer_armed = RT_FALSE;
        rt_mutex_release(&context->tcp_ack_mutex);
    }
    level = rt_hw_interrupt_disable();
    if (context->tcp_ack_initialized && context->tcp_ack_work_initialized &&
        !context->tcp_ack_work_queued)
    {
        context->tcp_ack_work_queued = RT_TRUE;
        submit = RT_TRUE;
    }
    rt_hw_interrupt_enable(level);
    if (submit && rt_work_submit(&context->tcp_ack_work, 0) != RT_EOK)
    {
        context->tcp_ack_work_queued = RT_FALSE;
    }
}

static void aic_tcp_ack_reset(struct aic8800_context *context)
{
    if (!context || !context->tcp_ack_initialized ||
        rt_mutex_take(&context->tcp_ack_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return;
    }
    rt_memset(context->tcp_ack_flows, 0, sizeof(context->tcp_ack_flows));
    rt_mutex_release(&context->tcp_ack_mutex);
}

static void aic_tcp_ack_deinit(struct aic8800_context *context)
{
    if (!context)
    {
        return;
    }
    context->tcp_ack_initialized = RT_FALSE;
    if (context->tcp_ack_timer_initialized)
    {
        rt_timer_stop(&context->tcp_ack_timer);
        rt_timer_detach(&context->tcp_ack_timer);
        context->tcp_ack_timer_initialized = RT_FALSE;
        context->tcp_ack_timer_armed = RT_FALSE;
    }
    if (context->tcp_ack_work_initialized)
    {
        rt_work_cancel_sync(&context->tcp_ack_work);
        context->tcp_ack_work_initialized = RT_FALSE;
        context->tcp_ack_work_queued = RT_FALSE;
    }
    if (context->tcp_ack_mutex_initialized)
    {
        rt_mutex_detach(&context->tcp_ack_mutex);
        context->tcp_ack_mutex_initialized = RT_FALSE;
    }
#ifdef AIC8800_WIFI_DEBUG_STATS
    LOG_I("TCP ACK filter: suppressed=%u flushed=%u",
          (unsigned int)context->tcp_ack_suppressed,
          (unsigned int)context->tcp_ack_flushed);
#endif
}

static rt_err_t aic_tcp_ack_init(struct aic8800_context *context)
{
    rt_tick_t period = rt_tick_from_millisecond(AIC8800_TCP_ACK_DELAY_MS);
    rt_err_t result;

    if (!period)
    {
        period = 1;
    }
    result = rt_mutex_init(&context->tcp_ack_mutex, "aic-ack",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    context->tcp_ack_mutex_initialized = RT_TRUE;
    rt_work_init(&context->tcp_ack_work, aic_tcp_ack_flush_work, context);
    context->tcp_ack_work_initialized = RT_TRUE;
    rt_timer_init(&context->tcp_ack_timer, "aic-ack",
                  aic_tcp_ack_timeout, context, period,
                  RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    context->tcp_ack_timer_initialized = RT_TRUE;
    context->tcp_ack_initialized = RT_TRUE;
    return RT_EOK;
}
#endif

static rt_err_t aic_submit_ethernet(struct aic8800_context *context,
                                    enum rt_wlan_offload_iftype iftype,
                                    const rt_uint8_t *ethernet,
                                    rt_size_t length)
{
    rt_err_t result;
#ifdef AIC8800_WIFI_DEBUG_STATS
    rt_uint16_t ethertype;
#endif

#ifdef AIC8800_WIFI_TCP_ACK_FILTER
    aic_tcp_ack_note_rx(context, iftype, ethernet, length);
#endif
    if (!aic_filter_accept(context, ethernet, length))
    {
        return RT_EOK;
    }
#ifdef AIC8800_WIFI_DEBUG_STATS
    AIC8800_STAT(context->ethernet_rx_count++);
    if (length >= 14U)
    {
        ethertype = ((rt_uint16_t)ethernet[12] << 8) | ethernet[13];
        if (ethertype == 0x0806U)
        {
            AIC8800_STAT(context->arp_rx_count++);
        }
        else if (ethertype == 0x0800U && length >= 24U &&
                 ethernet[23] == 1U)
        {
            AIC8800_STAT(context->icmp_rx_count++);
        }
    }
#endif
    result = rt_wlan_offload_rx(&context->radio, iftype, ethernet, length);
    if (result != RT_EOK && result != -RT_EEMPTY)
    {
        AIC8800_STAT(context->ethernet_rx_error_count++);
    }
    /* Authentication and network-stack failures belong to the frame
     * consumer; the SDIO/USB record itself was received successfully. */
    return result == -RT_EEMPTY ? -RT_EEMPTY : RT_EOK;
}

static rt_size_t aic_amsdu_payload_offset(const rt_uint8_t *frame,
                                          rt_size_t frame_length,
                                          rt_size_t header_length)
{
    static const rt_uint8_t iv_lengths[] = {0, 8, 4, 18};
    rt_size_t index;

    for (index = 0; index < sizeof(iv_lengths); index++)
    {
        rt_size_t offset = header_length + iv_lengths[index];
        rt_size_t subframe_payload_length;

        if (offset + 22U > frame_length)
        {
            continue;
        }
        subframe_payload_length =
            ((rt_size_t)frame[offset + 12U] << 8) | frame[offset + 13U];
        if (subframe_payload_length >= 8U &&
            14U + subframe_payload_length <= frame_length - offset &&
            frame[offset + 14U] == 0xaa &&
            frame[offset + 15U] == 0xaa &&
            frame[offset + 16U] == 0x03 &&
            frame[offset + 17U] == 0x00 &&
            frame[offset + 18U] == 0x00 &&
            frame[offset + 19U] == 0x00)
        {
            return offset;
        }
    }
    return 0;
}

static rt_err_t aic_deliver_amsdu(struct aic8800_context *context,
                                  enum rt_wlan_offload_iftype iftype,
                                  const rt_uint8_t *frame,
                                  rt_size_t frame_length,
                                  rt_size_t header_length)
{
    rt_size_t offset;
    const rt_uint8_t *cursor;
    rt_size_t remaining;
    rt_uint8_t ethernet[AIC8800_ETHERNET_FRAME_MAX];
    rt_bool_t delivered = RT_FALSE;

    offset = aic_amsdu_payload_offset(frame, frame_length, header_length);
    if (!offset)
    {
        return -RT_EEMPTY;
    }
    cursor = frame + offset;
    remaining = frame_length - offset;

    while (remaining >= 14U)
    {
        rt_size_t payload_length =
            ((rt_size_t)cursor[12] << 8) | cursor[13];
        rt_size_t subframe_length = 14U + payload_length;
        rt_size_t ethernet_length;
        rt_size_t consumed;

        if (payload_length < 8U || subframe_length > remaining)
        {
            return -RT_EIO;
        }
        if (cursor[14] == 0xaa && cursor[15] == 0xaa &&
            cursor[16] == 0x03 && cursor[17] == 0x00 &&
            cursor[18] == 0x00 && cursor[19] == 0x00)
        {
            ethernet_length = 14U + payload_length - 8U;
            if (ethernet_length > sizeof(ethernet))
            {
                return -RT_EFULL;
            }
            rt_memcpy(ethernet, cursor, 12U);
            rt_memcpy(ethernet + 12U, cursor + 20U, 2U);
            rt_memcpy(ethernet + 14U, cursor + 22U,
                      payload_length - 8U);
        }
        else
        {
            /* Preserve non-SNAP 802.3 MSDUs, including their length field. */
            ethernet_length = subframe_length;
            if (ethernet_length > sizeof(ethernet))
            {
                return -RT_EFULL;
            }
            rt_memcpy(ethernet, cursor, ethernet_length);
        }
        (void)aic_submit_ethernet(context, iftype, ethernet,
                                  ethernet_length);
        AIC8800_STAT(context->rx_amsdu_subframe_count++);
        delivered = RT_TRUE;

        if (subframe_length == remaining)
        {
            remaining = 0;
            break;
        }
        consumed = aic_align4(subframe_length);
        if (consumed > remaining)
        {
            return -RT_EIO;
        }
        cursor += consumed;
        remaining -= consumed;
    }
    return delivered && remaining == 0 ? RT_EOK : -RT_EIO;
}

static rt_err_t aic_deliver_ethernet(struct aic8800_context *context,
                                     const rt_uint8_t *record,
                                     rt_size_t length)
{
    static const rt_uint8_t iv_lengths[] = {0, 8, 4, 18};
    const rt_uint8_t *frame;
    rt_uint16_t frame_length;
    rt_uint8_t destination[6];
    rt_uint8_t source[6];
    rt_size_t header_length;
    rt_size_t llc_offset = 0;
    rt_size_t payload_length;
    rt_uint8_t ethernet[AIC8800_ETHERNET_FRAME_MAX];
    rt_uint8_t ds;
    rt_size_t index;
    rt_size_t qos_offset = 0;
    rt_uint32_t rx_flags;
    rt_bool_t amsdu = RT_FALSE;
    enum rt_wlan_offload_iftype iftype;

    if (!context || !record || !context->attached ||
        context->radio.state != RT_WLAN_OFFLOAD_STARTED)
    {
        return RT_EOK;
    }
    frame = record + AIC8800_USB_RX_HEADER_SIZE;
    frame_length = aic_record_payload_length(context, record);
    if (length < AIC8800_USB_RX_HEADER_SIZE || frame_length < 24 ||
        frame_length > length - AIC8800_USB_RX_HEADER_SIZE)
    {
        AIC8800_STAT(context->rx_invalid_data_count++);
        return -RT_EIO;
    }
    if ((frame[0] & 0x0cU) != 0x08U)
    {
        AIC8800_STAT(context->rx_invalid_data_count++);
        return -RT_EEMPTY;
    }
    AIC8800_STAT(context->rx_data_record_count++);
    ds = frame[1] & 3U;
    header_length = ds == 3 ? 30U : 24U;
    if (frame[0] & 0x80U)
    {
        qos_offset = header_length;
        header_length += 2U;
        AIC8800_STAT(context->rx_qos_record_count++);
    }
    if (frame[1] & 0x80U)
    {
        header_length += 4U;
    }
    if (frame_length < header_length)
    {
        AIC8800_STAT(context->rx_invalid_data_count++);
        return -RT_EIO;
    }
    switch (ds)
    {
    case 0:
        rt_memcpy(destination, frame + 4, 6);
        rt_memcpy(source, frame + 10, 6);
        break;
    case 1:
        rt_memcpy(destination, frame + 16, 6);
        rt_memcpy(source, frame + 10, 6);
        break;
    case 2:
        rt_memcpy(destination, frame + 4, 6);
        rt_memcpy(source, frame + 16, 6);
        break;
    default:
        rt_memcpy(destination, frame + 16, 6);
        rt_memcpy(source, frame + 24, 6);
        break;
    }
    rx_flags = aic_get_le32(record + 48);
    /* Both USB and SDIO firmware deliver received A-MSDUs as raw 802.11
     * aggregates. Firmware revisions disagree on whether flags_is_amsdu is
     * populated, so accept the descriptor or QoS indication only after the
     * first subframe has passed structural validation. */
    amsdu = qos_offset &&
            ((rx_flags & AIC_RX_FLAG_AMSDU) || (frame[qos_offset] & 0x80U)) &&
            aic_amsdu_payload_offset(frame, frame_length, header_length);
    iftype = record[49] == context->ap_vif_index ?
             RT_WLAN_OFFLOAD_IFTYPE_AP : RT_WLAN_OFFLOAD_IFTYPE_STATION;
    if (iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION)
    {
        context->rssi = (rt_int8_t)record[14];
    }
    if (amsdu)
    {
        AIC8800_STAT(context->rx_amsdu_record_count++);
        return aic_deliver_amsdu(context, iftype, frame, frame_length,
                                 header_length);
    }
    if (frame_length < header_length + 8U)
    {
        AIC8800_STAT(context->rx_invalid_data_count++);
        return -RT_EIO;
    }
    for (index = 0; index < sizeof(iv_lengths); index++)
    {
        rt_size_t candidate = header_length + iv_lengths[index];

        if (candidate + 8U <= frame_length &&
            frame[candidate] == 0xaa && frame[candidate + 1] == 0xaa &&
            frame[candidate + 2] == 0x03 && frame[candidate + 3] == 0x00 &&
            frame[candidate + 4] == 0x00 && frame[candidate + 5] == 0x00)
        {
            llc_offset = candidate;
            break;
        }
    }
    if (!llc_offset)
    {
        AIC8800_STAT(context->rx_no_llc_count++);
        return -RT_EEMPTY;
    }
    payload_length = frame_length - llc_offset - 8U;
    if (payload_length > sizeof(ethernet) - 14U)
    {
        AIC8800_STAT(context->rx_invalid_data_count++);
        return -RT_EFULL;
    }
    rt_memcpy(ethernet, destination, 6);
    rt_memcpy(ethernet + 6, source, 6);
    rt_memcpy(ethernet + 12, frame + llc_offset + 6, 2);
    rt_memcpy(ethernet + 14, frame + llc_offset + 8, payload_length);
    return aic_submit_ethernet(context, iftype, ethernet,
                               14U + payload_length);
}

struct aic_rx_reorder_metadata
{
    rt_uint16_t sequence;
    rt_uint8_t vif_index;
    rt_uint8_t station_index;
    rt_uint8_t tid;
};

static rt_bool_t aic_rx_reorder_metadata(
    struct aic8800_context *context, const rt_uint8_t *record,
    rt_size_t length, struct aic_rx_reorder_metadata *metadata)
{
    const rt_uint8_t *frame;
    rt_uint16_t frame_length;
    rt_size_t qos_offset;

    if (!context || !record || !metadata ||
        length < AIC8800_USB_RX_HEADER_SIZE + 26U)
    {
        return RT_FALSE;
    }
    frame = record + AIC8800_USB_RX_HEADER_SIZE;
    frame_length = aic_record_payload_length(context, record);
    if (frame_length < 26U ||
        frame_length > length - AIC8800_USB_RX_HEADER_SIZE ||
        (frame[0] & 0x0cU) != 0x08U || !(frame[0] & 0x80U))
    {
        return RT_FALSE;
    }
    qos_offset = (frame[1] & 3U) == 3U ? 30U : 24U;
    if (qos_offset + 2U > frame_length)
    {
        return RT_FALSE;
    }
    metadata->sequence =
        (rt_uint16_t)(((frame[22] >> 4) | ((rt_uint16_t)frame[23] << 4)) &
                      AIC8800_RX_REORDER_SEQUENCE_MASK);
    metadata->vif_index = record[49];
    metadata->station_index = record[50];
    metadata->tid = frame[qos_offset] & 0x0fU;
    return RT_TRUE;
}

static rt_bool_t aic_rx_reorder_should_bypass(
    struct aic8800_context *context, const rt_uint8_t *record,
    rt_size_t length)
{
    static const rt_uint8_t iv_lengths[] = {0, 8, 4, 18};
    const rt_uint8_t *frame;
    rt_uint16_t frame_length;
    rt_size_t header_length;
    rt_size_t qos_offset;
    rt_size_t index;
    rt_uint8_t ds;

    if (!context || !record ||
        length < AIC8800_USB_RX_HEADER_SIZE + 26U)
    {
        return RT_FALSE;
    }
    frame = record + AIC8800_USB_RX_HEADER_SIZE;
    frame_length = aic_record_payload_length(context, record);
    if (frame_length < 26U ||
        frame_length > length - AIC8800_USB_RX_HEADER_SIZE ||
        (frame[0] & 0x0cU) != 0x08U || !(frame[0] & 0x80U))
    {
        return RT_FALSE;
    }
    ds = frame[1] & 3U;
    header_length = ds == 3U ? 30U : 24U;
    qos_offset = header_length;
    header_length += 2U;
    if (frame[1] & 0x80U)
    {
        header_length += 4U;
    }
    if (frame_length < header_length)
    {
        return RT_FALSE;
    }

    if (frame[qos_offset] & 0x80U)
    {
        const rt_uint8_t *cursor = RT_NULL;
        rt_size_t remaining = 0;

        for (index = 0; index < sizeof(iv_lengths); index++)
        {
            rt_size_t offset = header_length + iv_lengths[index];

            if (offset + 22U <= frame_length &&
                frame[offset + 14U] == 0xaa &&
                frame[offset + 15U] == 0xaa &&
                frame[offset + 16U] == 0x03 &&
                frame[offset + 17U] == 0x00 &&
                frame[offset + 18U] == 0x00 &&
                frame[offset + 19U] == 0x00)
            {
                cursor = frame + offset;
                remaining = frame_length - offset;
                break;
            }
        }
        while (cursor && remaining >= 14U)
        {
            rt_size_t payload_length =
                ((rt_size_t)cursor[12] << 8) | cursor[13];
            rt_size_t subframe_length = 14U + payload_length;
            rt_size_t consumed;

            if (payload_length < 8U || subframe_length > remaining)
            {
                break;
            }
            if ((cursor[0] & 1U) ||
                (cursor[14] == 0xaa && cursor[15] == 0xaa &&
                 cursor[16] == 0x03 && cursor[17] == 0x00 &&
                 cursor[18] == 0x00 && cursor[19] == 0x00 &&
                 cursor[20] == (rt_uint8_t)(AIC_ETHERTYPE_EAPOL >> 8) &&
                 cursor[21] == (rt_uint8_t)AIC_ETHERTYPE_EAPOL))
            {
                return RT_TRUE;
            }
            if (subframe_length == remaining)
            {
                break;
            }
            consumed = aic_align4(subframe_length);
            if (consumed > remaining)
            {
                break;
            }
            cursor += consumed;
            remaining -= consumed;
        }
        return RT_FALSE;
    }

    {
        const rt_uint8_t *destination;

        if (ds == 1U || ds == 3U)
        {
            destination = frame + 16U;
        }
        else
        {
            destination = frame + 4U;
        }
        if (destination[0] & 1U)
        {
            return RT_TRUE;
        }
    }
    for (index = 0; index < sizeof(iv_lengths); index++)
    {
        rt_size_t offset = header_length + iv_lengths[index];

        if (offset + 8U <= frame_length &&
            frame[offset] == 0xaa && frame[offset + 1U] == 0xaa &&
            frame[offset + 2U] == 0x03 && frame[offset + 3U] == 0x00 &&
            frame[offset + 4U] == 0x00 && frame[offset + 5U] == 0x00)
        {
            return frame[offset + 6U] ==
                       (rt_uint8_t)(AIC_ETHERTYPE_EAPOL >> 8) &&
                   frame[offset + 7U] == (rt_uint8_t)AIC_ETHERTYPE_EAPOL;
        }
    }
    return RT_FALSE;
}

static rt_uint16_t aic_rx_reorder_distance(rt_uint16_t sequence,
                                            rt_uint16_t expected)
{
    return (sequence - expected) & AIC8800_RX_REORDER_SEQUENCE_MASK;
}

static void aic_rx_reorder_release_slot(
    struct aic8800_rx_reorder_slot *slot)
{
    if (slot->external_data)
    {
        rt_free(slot->external_data);
        slot->external_data = RT_NULL;
    }
    slot->used = RT_FALSE;
    slot->next = AIC8800_RX_REORDER_INVALID_SLOT;
    slot->length = 0;
}

static rt_err_t aic_rx_reorder_deliver_head_locked(
    struct aic8800_context *context, struct aic8800_rx_reorder_flow *flow)
{
    struct aic8800_rx_reorder_slot *slot;
    rt_uint16_t index;
    rt_err_t result;

    if (!flow || flow->head == AIC8800_RX_REORDER_INVALID_SLOT)
    {
        return -RT_EEMPTY;
    }
    index = flow->head;
    slot = &context->rx_reorder_slots[index];
    flow->head = slot->next;
    flow->expected = (slot->sequence + 1U) &
                     AIC8800_RX_REORDER_SEQUENCE_MASK;
    result = aic_deliver_ethernet(
        context, slot->external_data ? slot->external_data : slot->data,
        slot->length);
    aic_rx_reorder_release_slot(slot);
    if (context->rx_reorder_pending)
    {
        context->rx_reorder_pending--;
    }
    AIC8800_STAT(context->rx_reorder_delivered++);
    return result;
}

static rt_err_t aic_rx_reorder_drain_locked(
    struct aic8800_context *context, struct aic8800_rx_reorder_flow *flow)
{
    rt_err_t last_result = RT_EOK;

    while (flow->head != AIC8800_RX_REORDER_INVALID_SLOT)
    {
        struct aic8800_rx_reorder_slot *slot =
            &context->rx_reorder_slots[flow->head];
        rt_err_t result;

        if (slot->sequence != flow->expected)
        {
            break;
        }
        result = aic_rx_reorder_deliver_head_locked(context, flow);
        if (result != RT_EOK && result != -RT_EEMPTY)
        {
            last_result = result;
        }
    }
    return last_result;
}

static void aic_rx_reorder_flush_flow_locked(
    struct aic8800_context *context, struct aic8800_rx_reorder_flow *flow)
{
    while (flow && flow->head != AIC8800_RX_REORDER_INVALID_SLOT)
    {
        (void)aic_rx_reorder_deliver_head_locked(context, flow);
    }
}

static void aic_rx_reorder_discard_flow_locked(
    struct aic8800_context *context, struct aic8800_rx_reorder_flow *flow)
{
    while (flow && flow->head != AIC8800_RX_REORDER_INVALID_SLOT)
    {
        struct aic8800_rx_reorder_slot *slot =
            &context->rx_reorder_slots[flow->head];

        flow->head = slot->next;
        aic_rx_reorder_release_slot(slot);
        if (context->rx_reorder_pending)
        {
            context->rx_reorder_pending--;
        }
        AIC8800_STAT(context->rx_reorder_drops++);
    }
}

static struct aic8800_rx_reorder_flow *aic_rx_reorder_find_flow_locked(
    struct aic8800_context *context,
    const struct aic_rx_reorder_metadata *metadata, rt_bool_t create)
{
    struct aic8800_rx_reorder_flow *available = RT_NULL;
    rt_bool_t available_is_empty = RT_FALSE;
    rt_tick_t oldest_age = 0;
    rt_tick_t now = rt_tick_get();
    rt_size_t index;

    for (index = 0; index < AIC8800_WIFI_RX_REORDER_FLOWS; index++)
    {
        struct aic8800_rx_reorder_flow *flow =
            &context->rx_reorder_flows[index];

        if (flow->valid && flow->vif_index == metadata->vif_index &&
            flow->station_index == metadata->station_index &&
            flow->tid == metadata->tid)
        {
            flow->last_seen = now;
            return flow;
        }
        if (!flow->valid)
        {
            if (!available_is_empty)
            {
                available = flow;
                available_is_empty = RT_TRUE;
            }
        }
        else if (!available_is_empty &&
                 flow->head == AIC8800_RX_REORDER_INVALID_SLOT)
        {
            available = flow;
            available_is_empty = RT_TRUE;
        }
        else if (!available_is_empty)
        {
            rt_tick_t age = now - flow->last_seen;

            if (!available || age >= oldest_age)
            {
                available = flow;
                oldest_age = age;
            }
        }
    }
    if (!create || !available)
    {
        return RT_NULL;
    }
    if (available->valid)
    {
        /* Reusing a flow must not release packets across an unresolved gap.
         * Drop the stale queue and let upper-layer retransmission recover. */
        aic_rx_reorder_discard_flow_locked(context, available);
    }
    rt_memset(available, 0, sizeof(*available));
    available->valid = RT_TRUE;
    available->vif_index = metadata->vif_index;
    available->station_index = metadata->station_index;
    available->tid = metadata->tid;
    available->head = AIC8800_RX_REORDER_INVALID_SLOT;
    available->last_seen = now;
    return available;
}

static struct aic8800_rx_reorder_slot *aic_rx_reorder_alloc_slot_locked(
    struct aic8800_context *context, rt_uint16_t *slot_index)
{
    rt_size_t index;

    for (index = 0; index < AIC8800_WIFI_RX_REORDER_SLOTS; index++)
    {
        if (!context->rx_reorder_slots[index].used)
        {
            struct aic8800_rx_reorder_slot *slot =
                &context->rx_reorder_slots[index];

            slot->used = RT_TRUE;
            slot->next = AIC8800_RX_REORDER_INVALID_SLOT;
            *slot_index = (rt_uint16_t)index;
            return slot;
        }
    }
    return RT_NULL;
}

static rt_err_t aic_rx_reorder_queue_locked(
    struct aic8800_context *context, struct aic8800_rx_reorder_flow *flow,
    const struct aic_rx_reorder_metadata *metadata,
    const rt_uint8_t *record, rt_size_t length)
{
    struct aic8800_rx_reorder_slot *slot;
    rt_uint16_t *link = &flow->head;
    rt_uint16_t slot_index;

    while (*link != AIC8800_RX_REORDER_INVALID_SLOT)
    {
        struct aic8800_rx_reorder_slot *queued =
            &context->rx_reorder_slots[*link];
        rt_uint16_t queued_distance;
        rt_uint16_t new_distance;

        if (queued->sequence == metadata->sequence)
        {
            AIC8800_STAT(context->rx_reorder_duplicates++);
            return -RT_EEMPTY;
        }
        queued_distance = aic_rx_reorder_distance(queued->sequence,
                                                   flow->expected);
        new_distance = aic_rx_reorder_distance(metadata->sequence,
                                                flow->expected);
        if (new_distance < queued_distance)
        {
            break;
        }
        link = &queued->next;
    }
    slot = aic_rx_reorder_alloc_slot_locked(context, &slot_index);
    if (!slot)
    {
        AIC8800_STAT(context->rx_reorder_drops++);
        return -RT_EEMPTY;
    }
    slot->sequence = metadata->sequence;
    slot->length = (rt_uint16_t)length;
    slot->queued_at = rt_tick_get();
    if (length > sizeof(slot->data))
    {
        slot->external_data = rt_malloc(length);
        if (!slot->external_data)
        {
            aic_rx_reorder_release_slot(slot);
            AIC8800_STAT(context->rx_reorder_drops++);
            return -RT_EEMPTY;
        }
        rt_memcpy(slot->external_data, record, length);
    }
    else
    {
        rt_memcpy(slot->data, record, length);
    }
    slot->next = *link;
    *link = slot_index;
    context->rx_reorder_pending++;
    AIC8800_STAT(context->rx_reorder_queued++);
    return RT_EOK;
}

static rt_err_t aic_rx_reorder_receive(
    struct aic8800_context *context, const rt_uint8_t *record,
    rt_size_t length, rt_bool_t needs_reorder,
    const struct aic_rx_reorder_metadata *metadata)
{
    struct aic8800_rx_reorder_flow *flow;
    rt_uint16_t distance;
    rt_err_t result = RT_EOK;

    if (!context->rx_reorder_initialized ||
        rt_mutex_take(&context->rx_reorder_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return aic_deliver_ethernet(context, record, length);
    }
    if (!context->rx_reorder_initialized)
    {
        rt_mutex_release(&context->rx_reorder_mutex);
        return aic_deliver_ethernet(context, record, length);
    }
    flow = aic_rx_reorder_find_flow_locked(context, metadata, needs_reorder);
    if (!needs_reorder)
    {
        if (flow)
        {
            aic_rx_reorder_flush_flow_locked(context, flow);
            flow->initialized = RT_FALSE;
        }
        rt_mutex_release(&context->rx_reorder_mutex);
        return aic_deliver_ethernet(context, record, length);
    }
    if (!flow)
    {
        AIC8800_STAT(context->rx_reorder_drops++);
        rt_mutex_release(&context->rx_reorder_mutex);
        return aic_deliver_ethernet(context, record, length);
    }
    if (!flow->initialized)
    {
        flow->expected = metadata->sequence;
        flow->initialized = RT_TRUE;
    }
    distance = aic_rx_reorder_distance(metadata->sequence, flow->expected);
    if (!distance)
    {
        result = aic_deliver_ethernet(context, record, length);
        flow->expected = (flow->expected + 1U) &
                         AIC8800_RX_REORDER_SEQUENCE_MASK;
        (void)aic_rx_reorder_drain_locked(context, flow);
    }
    else if (distance < 0x0800U)
    {
        if (distance >= AIC8800_WIFI_RX_REORDER_WINDOW)
        {
            aic_rx_reorder_flush_flow_locked(context, flow);
            flow->expected = metadata->sequence;
            result = aic_deliver_ethernet(context, record, length);
            flow->expected = (flow->expected + 1U) &
                             AIC8800_RX_REORDER_SEQUENCE_MASK;
        }
        else
        {
            result = aic_rx_reorder_queue_locked(context, flow, metadata,
                                                 record, length);
        }
    }
    else
    {
        AIC8800_STAT(context->rx_reorder_duplicates++);
        result = -RT_EEMPTY;
    }
    rt_mutex_release(&context->rx_reorder_mutex);
    return result;
}

static void aic_rx_reorder_timeout_work(struct rt_work *work,
                                        void *work_data)
{
    struct aic8800_context *context = work_data;
    rt_tick_t timeout = rt_tick_from_millisecond(
        AIC8800_WIFI_RX_REORDER_TIMEOUT_MS);
    rt_tick_t now = rt_tick_get();
    rt_size_t index;

    (void)work;
    if (context && context->rx_reorder_initialized &&
        rt_mutex_take(&context->rx_reorder_mutex,
                      RT_WAITING_FOREVER) == RT_EOK)
    {
        if (context->rx_reorder_initialized)
        {
            for (index = 0; index < AIC8800_WIFI_RX_REORDER_FLOWS; index++)
            {
                struct aic8800_rx_reorder_flow *flow =
                    &context->rx_reorder_flows[index];

                if (flow->valid &&
                    flow->head != AIC8800_RX_REORDER_INVALID_SLOT)
                {
                    struct aic8800_rx_reorder_slot *slot =
                        &context->rx_reorder_slots[flow->head];

                    if ((rt_tick_t)(now - slot->queued_at) >= timeout)
                    {
                        flow->expected = slot->sequence;
                        (void)aic_rx_reorder_deliver_head_locked(context, flow);
                        (void)aic_rx_reorder_drain_locked(context, flow);
                        AIC8800_STAT(context->rx_reorder_timeouts++);
                    }
                }
            }
        }
        rt_mutex_release(&context->rx_reorder_mutex);
    }
    if (context)
    {
        context->rx_reorder_work_queued = RT_FALSE;
    }
}

static void aic_rx_reorder_timeout(void *parameter)
{
    struct aic8800_context *context = parameter;
    rt_base_t level;
    rt_bool_t submit = RT_FALSE;

    if (!context)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    if (context->rx_reorder_initialized &&
        context->rx_reorder_work_initialized &&
        !context->rx_reorder_work_queued)
    {
        context->rx_reorder_work_queued = RT_TRUE;
        submit = RT_TRUE;
    }
    rt_hw_interrupt_enable(level);
    if (submit && rt_work_submit(&context->rx_reorder_work, 0) != RT_EOK)
    {
        context->rx_reorder_work_queued = RT_FALSE;
    }
}

static void aic_rx_reorder_reset(struct aic8800_context *context)
{
    rt_size_t index;

    if (!context || !context->rx_reorder_initialized ||
        rt_mutex_take(&context->rx_reorder_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return;
    }
    for (index = 0; index < AIC8800_WIFI_RX_REORDER_SLOTS; index++)
    {
        aic_rx_reorder_release_slot(&context->rx_reorder_slots[index]);
    }
    context->rx_reorder_pending = 0;
    rt_memset(context->rx_reorder_flows, 0,
              sizeof(context->rx_reorder_flows));
    for (index = 0; index < AIC8800_WIFI_RX_REORDER_FLOWS; index++)
    {
        context->rx_reorder_flows[index].head =
            AIC8800_RX_REORDER_INVALID_SLOT;
    }
    rt_mutex_release(&context->rx_reorder_mutex);
}

static rt_err_t aic_rx_reorder_init(struct aic8800_context *context)
{
    rt_tick_t period;
    rt_err_t result;
    rt_size_t index;

    context->rx_reorder_slots = rt_calloc(
        AIC8800_WIFI_RX_REORDER_SLOTS,
        sizeof(*context->rx_reorder_slots));
    if (!context->rx_reorder_slots)
    {
        return -RT_ENOMEM;
    }
    result = rt_mutex_init(&context->rx_reorder_mutex, "aic-rxo",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_free(context->rx_reorder_slots);
        context->rx_reorder_slots = RT_NULL;
        return result;
    }
    context->rx_reorder_mutex_initialized = RT_TRUE;
    for (index = 0; index < AIC8800_WIFI_RX_REORDER_FLOWS; index++)
    {
        context->rx_reorder_flows[index].head =
            AIC8800_RX_REORDER_INVALID_SLOT;
    }
    for (index = 0; index < AIC8800_WIFI_RX_REORDER_SLOTS; index++)
    {
        context->rx_reorder_slots[index].next =
            AIC8800_RX_REORDER_INVALID_SLOT;
    }
    rt_work_init(&context->rx_reorder_work,
                 aic_rx_reorder_timeout_work, context);
    context->rx_reorder_work_initialized = RT_TRUE;
    period = rt_tick_from_millisecond(
        AIC8800_WIFI_RX_REORDER_TIMEOUT_MS / 2U);
    if (!period)
    {
        period = 1;
    }
    rt_timer_init(&context->rx_reorder_timer, "aic-rxo",
                  aic_rx_reorder_timeout, context, period,
                  RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    context->rx_reorder_timer_initialized = RT_TRUE;
    context->rx_reorder_initialized = RT_TRUE;
    result = rt_timer_start(&context->rx_reorder_timer);
    if (result != RT_EOK)
    {
        aic_rx_reorder_deinit(context);
        return result;
    }
    return RT_EOK;
}

static void aic_rx_reorder_deinit(struct aic8800_context *context)
{
    if (!context)
    {
        return;
    }
    if (context->rx_reorder_timer_initialized)
    {
        rt_timer_stop(&context->rx_reorder_timer);
        rt_timer_detach(&context->rx_reorder_timer);
        context->rx_reorder_timer_initialized = RT_FALSE;
    }
    if (context->rx_reorder_work_initialized)
    {
        rt_work_cancel_sync(&context->rx_reorder_work);
        context->rx_reorder_work_initialized = RT_FALSE;
        context->rx_reorder_work_queued = RT_FALSE;
    }
    context->rx_reorder_initialized = RT_FALSE;
    if (context->rx_reorder_mutex_initialized)
    {
        rt_mutex_detach(&context->rx_reorder_mutex);
        context->rx_reorder_mutex_initialized = RT_FALSE;
    }
    if (context->rx_reorder_slots)
    {
#ifdef AIC8800_WIFI_DEBUG_STATS
        LOG_I("RX reorder: pending=%u queued=%u delivered=%u timeout=%u duplicate=%u drops=%u",
              (unsigned int)context->rx_reorder_pending,
              (unsigned int)context->rx_reorder_queued,
              (unsigned int)context->rx_reorder_delivered,
              (unsigned int)context->rx_reorder_timeouts,
              (unsigned int)context->rx_reorder_duplicates,
              (unsigned int)context->rx_reorder_drops);
#endif
        rt_free(context->rx_reorder_slots);
        context->rx_reorder_slots = RT_NULL;
    }
}

static rt_err_t aic_process_data(struct aic8800_context *context,
                                 const rt_uint8_t *record,
                                 rt_size_t length)
{
    struct aic_rx_reorder_metadata metadata;
    rt_uint32_t flags;

    if (!context || !record || length < AIC8800_USB_RX_HEADER_SIZE)
    {
        return -RT_EIO;
    }
    flags = aic_get_le32(record + 48);
    if (flags & AIC_RX_FLAG_80211_MPDU)
    {
        return aic_report_management(context, record, length);
    }
    /* Firmware normally delivers in-order QoS frames without setting
     * NEEDS_REORDER. Avoid a mutex and a flow-table walk on every packet in
     * that common path. A pending gap still takes the slow path so queued
     * frames are flushed before the new packet is delivered. */
    if (!(flags & AIC_RX_FLAG_NEEDS_REORDER) &&
        !context->rx_reorder_pending)
    {
        return aic_deliver_ethernet(context, record, length);
    }
    if (aic_rx_reorder_metadata(context, record, length, &metadata))
    {
        rt_bool_t needs_reorder =
            (flags & AIC_RX_FLAG_NEEDS_REORDER) != 0;

        if (needs_reorder &&
            aic_rx_reorder_should_bypass(context, record, length))
        {
            return aic_deliver_ethernet(context, record, length);
        }
        return aic_rx_reorder_receive(
            context, record, length, needs_reorder, &metadata);
    }
    return aic_deliver_ethernet(context, record, length);
}

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
static void aic_sdio_data_queue_note_drop(
    struct aic8800_context *context, rt_uint16_t queued,
    rt_uint16_t normal_free, rt_uint16_t large_free)
{
    rt_uint32_t drops = ++context->sdio_data_drop_count;

    if (drops <= 4U || (drops & (drops - 1U)) == 0U)
    {
        LOG_W("SDIO RX backlog drop: drops=%u queued=%u/%u "
              "normal-free=%u large-free=%u",
              (unsigned int)drops,
              (unsigned int)queued,
              (unsigned int)AIC8800_WIFI_SDIO_RX_QUEUE_DEPTH,
              (unsigned int)normal_free,
              (unsigned int)large_free);
    }
}

static rt_err_t aic_sdio_data_queue_push(
    struct aic8800_context *context, const rt_uint8_t *record,
    rt_size_t length)
{
    struct aic8800_sdio_rx_record *queued = RT_NULL;
    rt_mp_t pool = RT_NULL;
    rt_uint16_t queue_entries = 0;
    rt_uint16_t normal_free = 0;
    rt_uint16_t large_free = 0;
    rt_err_t result;

    if (length > AIC8800_WIFI_SDIO_MAX_RECORD_SIZE)
    {
        return -RT_EFULL;
    }
    if (context->sdio_data_queue_stopping)
    {
        return RT_EOK;
    }
    if (!context->sdio_data_queue_mutex_initialized ||
        rt_mutex_take(&context->sdio_data_queue_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return RT_EOK;
    }
    if (context->sdio_data_queue_active && context->sdio_data_queue)
    {
        pool = length <= AIC8800_USB_MAX_RECORD_SIZE ?
               context->sdio_data_pool : context->sdio_data_large_pool;
        if (pool)
        {
            queued = rt_mp_alloc(pool, 0);
        }
    }
    if (!queued)
    {
        result = context->sdio_data_queue_active ? -RT_EFULL : -RT_EBUSY;
    }
    else
    {
        queued->length = (rt_uint16_t)length;
        rt_memcpy(queued->data, record, length);
        result = rt_mq_send(context->sdio_data_queue, &queued,
                            sizeof(queued));
        if (result == RT_EOK)
        {
            rt_uint16_t queued_count = context->sdio_data_queue->entry;

            context->sdio_data_queued_count++;
            if (queued_count > context->sdio_data_queue_high_water)
            {
                context->sdio_data_queue_high_water = queued_count;
            }
        }
        else
        {
            rt_mp_free(queued);
        }
    }
    if (result == -RT_EFULL)
    {
        queue_entries = context->sdio_data_queue ?
                        context->sdio_data_queue->entry : 0;
        normal_free = context->sdio_data_pool ?
                      context->sdio_data_pool->block_free_count : 0;
        large_free = context->sdio_data_large_pool ?
                     context->sdio_data_large_pool->block_free_count : 0;
    }
    rt_mutex_release(&context->sdio_data_queue_mutex);
    if (result == -RT_EFULL)
    {
        aic_sdio_data_queue_note_drop(
            context, queue_entries, normal_free, large_free);
    }
    /* Queue pressure is a packet drop, not an SDIO protocol failure. */
    return RT_EOK;
}

static void aic_sdio_data_worker(void *parameter)
{
    struct aic8800_context *context = parameter;
    rt_uint32_t processed = 0;

    while (RT_TRUE)
    {
        struct aic8800_sdio_rx_record *record = RT_NULL;
        rt_err_t result = rt_mq_recv(
            context->sdio_data_queue, &record, sizeof(record),
            RT_WAITING_FOREVER);

        if (result != RT_EOK)
        {
            if (context->sdio_data_terminate)
            {
                break;
            }
            continue;
        }
        if (context->sdio_data_terminate)
        {
            if (record)
            {
                rt_mp_free(record);
            }
            break;
        }
        if (record)
        {
            (void)aic_process_data(context, record->data, record->length);
            context->sdio_data_processed_count++;
            rt_mp_free(record);
            processed++;
            if (processed >= AIC8800_WIFI_SDIO_DATA_THREAD_BUDGET)
            {
                processed = 0;
                if (context->sdio_data_queue &&
                    context->sdio_data_queue->entry)
                {
                    rt_thread_delay(1);
                }
            }
        }
    }
    rt_completion_done(&context->sdio_data_thread_stopped);
}

static void aic_sdio_data_queue_reset(struct aic8800_context *context)
{
    struct aic8800_sdio_rx_record *record;

    if (!context->sdio_data_queue)
    {
        return;
    }
    while (rt_mq_recv(context->sdio_data_queue, &record, sizeof(record), 0) ==
           RT_EOK)
    {
        if (record)
        {
            rt_mp_free(record);
        }
    }
    rt_mq_control(context->sdio_data_queue, RT_IPC_CMD_RESET, RT_NULL);
}

static void aic_sdio_data_queue_deinit(struct aic8800_context *context)
{
    struct aic8800_sdio_rx_record *wake = RT_NULL;

    if (!context)
    {
        return;
    }
    context->sdio_data_queue_stopping = RT_TRUE;
    if (context->sdio_data_queue_mutex_initialized &&
        rt_mutex_take(&context->sdio_data_queue_mutex,
                      RT_WAITING_FOREVER) == RT_EOK)
    {
        context->sdio_data_queue_active = RT_FALSE;
        rt_mutex_release(&context->sdio_data_queue_mutex);
    }
    else
    {
        context->sdio_data_queue_active = RT_FALSE;
    }
    if (context->sdio_data_thread)
    {
        if (!context->sdio_data_thread_started)
        {
            rt_thread_delete(context->sdio_data_thread);
        }
        else
        {
            context->sdio_data_terminate = RT_TRUE;
            /* A full queue already makes the worker runnable. */
            (void)rt_mq_send(context->sdio_data_queue, &wake, sizeof(wake));
            rt_completion_wait(&context->sdio_data_thread_stopped,
                               RT_WAITING_FOREVER);
        }
        context->sdio_data_thread = RT_NULL;
        context->sdio_data_thread_started = RT_FALSE;
    }
    aic_sdio_data_queue_reset(context);
    if (context->sdio_data_queue)
    {
        rt_mq_delete(context->sdio_data_queue);
        context->sdio_data_queue = RT_NULL;
    }
    if (context->sdio_data_pool)
    {
        rt_mp_delete(context->sdio_data_pool);
        context->sdio_data_pool = RT_NULL;
    }
    if (context->sdio_data_large_pool)
    {
        rt_mp_delete(context->sdio_data_large_pool);
        context->sdio_data_large_pool = RT_NULL;
    }
    if (context->sdio_data_queue_mutex_initialized)
    {
        rt_mutex_detach(&context->sdio_data_queue_mutex);
        context->sdio_data_queue_mutex_initialized = RT_FALSE;
    }
    if (context->sdio_data_queue_initialized)
    {
        LOG_I("SDIO RX queue: queued=%u processed=%u drops=%u high-water=%u/%u",
              (unsigned int)context->sdio_data_queued_count,
              (unsigned int)context->sdio_data_processed_count,
              (unsigned int)context->sdio_data_drop_count,
              (unsigned int)context->sdio_data_queue_high_water,
              (unsigned int)AIC8800_WIFI_SDIO_RX_QUEUE_DEPTH);
    }
    context->sdio_data_queue_initialized = RT_FALSE;
}

static rt_err_t aic_sdio_data_queue_init(struct aic8800_context *context)
{
    rt_err_t result;

    context->sdio_data_queue_stopping = RT_FALSE;
    context->sdio_data_terminate = RT_FALSE;
    result = rt_mutex_init(&context->sdio_data_queue_mutex, "aic-srq",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    context->sdio_data_queue_mutex_initialized = RT_TRUE;
    context->sdio_data_pool = rt_mp_create(
        "aic-srp", AIC8800_WIFI_SDIO_RX_QUEUE_DEPTH,
        sizeof(struct aic8800_sdio_rx_record) + AIC8800_USB_MAX_RECORD_SIZE);
    context->sdio_data_large_pool = rt_mp_create(
        "aic-srl", AIC8800_WIFI_SDIO_RX_LARGE_QUEUE_DEPTH,
        sizeof(struct aic8800_sdio_rx_record) +
            AIC8800_WIFI_SDIO_MAX_RECORD_SIZE);
    context->sdio_data_queue = rt_mq_create(
        "aic-srx", sizeof(struct aic8800_sdio_rx_record *),
        AIC8800_WIFI_SDIO_RX_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    if (!context->sdio_data_pool || !context->sdio_data_large_pool ||
        !context->sdio_data_queue)
    {
        result = -RT_ENOMEM;
        goto fail;
    }
    rt_completion_init(&context->sdio_data_thread_stopped);
    context->sdio_data_thread = rt_thread_create(
        "aic-srx", aic_sdio_data_worker, context,
        AIC8800_WIFI_SDIO_DATA_THREAD_STACK_SIZE,
        AIC8800_WIFI_SDIO_DATA_THREAD_PRIORITY, 10U);
    if (!context->sdio_data_thread)
    {
        result = -RT_ENOMEM;
        goto fail;
    }
    context->sdio_data_queue_initialized = RT_TRUE;
    context->sdio_data_queue_active = RT_TRUE;
    result = rt_thread_startup(context->sdio_data_thread);
    if (result != RT_EOK)
    {
        goto fail;
    }
    context->sdio_data_thread_started = RT_TRUE;
    return RT_EOK;

fail:
    aic_sdio_data_queue_deinit(context);
    return result;
}
#endif

static rt_err_t aic_handle_data(struct aic8800_context *context,
                                const rt_uint8_t *record,
                                rt_size_t length)
{
    rt_uint32_t flags;
    rt_size_t max_record_size = AIC8800_USB_MAX_RECORD_SIZE;

    if (!context || !record || length < AIC8800_USB_RX_HEADER_SIZE)
    {
        return -RT_EIO;
    }
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        max_record_size = AIC8800_WIFI_SDIO_MAX_RECORD_SIZE;
    }
#endif
    if (length > max_record_size)
    {
        return -RT_EFULL;
    }
    flags = aic_get_le32(record + 48);
    if (flags & AIC_RX_FLAG_80211_MPDU)
    {
        return aic_process_data(context, record, length);
    }
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO &&
        (context->sdio_data_queue_initialized ||
         context->sdio_data_queue_stopping))
    {
        return aic_sdio_data_queue_push(context, record, length);
    }
#endif
    return aic_process_data(context, record, length);
}

static void aic_report_mgmt_confirmation(
    struct aic8800_context *context,
    struct aic8800_mgmt_confirmation *confirmation,
    rt_err_t status, rt_bool_t acknowledged)
{
    struct rt_wlan_offload_event event;

    if (!context || !confirmation || !confirmation->frame)
    {
        return;
    }
    if (!context->attached ||
        context->radio.state != RT_WLAN_OFFLOAD_STARTED)
    {
        rt_free(confirmation->frame);
        confirmation->frame = RT_NULL;
        return;
    }
    rt_memset(&event, 0, sizeof(event));
    event.type = RT_WLAN_OFFLOAD_EVENT_MGMT_TX_STATUS;
    event.iftype = confirmation->iftype;
    event.request_id = confirmation->request_id;
    event.status = status;
    event.data.tx_status.cookie = confirmation->cookie;
    event.data.tx_status.acknowledged = acknowledged;
    event.data.tx_status.data = confirmation->frame;
    event.data.tx_status.length = confirmation->frame_length;
    rt_wlan_offload_report_event(&context->radio, &event);
    rt_free(confirmation->frame);
    confirmation->frame = RT_NULL;
}

static rt_bool_t aic_take_mgmt_confirmation(
    struct aic8800_context *context, rt_uint32_t index,
    struct aic8800_mgmt_confirmation *confirmation)
{
    rt_bool_t found = RT_FALSE;
    rt_size_t slot;

    if (!context || !confirmation ||
        !context->mgmt_confirmation_mutex_initialized ||
        rt_mutex_take(&context->mgmt_confirmation_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        return RT_FALSE;
    }
    for (slot = 0; slot < AIC8800_MGMT_CONFIRM_COUNT; slot++)
    {
        if (context->mgmt_confirmations[slot].used &&
            context->mgmt_confirmations[slot].index == index)
        {
            *confirmation = context->mgmt_confirmations[slot];
            rt_memset(&context->mgmt_confirmations[slot], 0,
                      sizeof(context->mgmt_confirmations[slot]));
            found = RT_TRUE;
            break;
        }
    }
    rt_mutex_release(&context->mgmt_confirmation_mutex);
    return found;
}

static rt_err_t aic_allocate_mgmt_confirmation(
    struct aic8800_context *context,
    enum rt_wlan_offload_iftype iftype,
    const struct rt_wlan_offload_mgmt_frame *request, rt_uint32_t *index)
{
    rt_uint8_t *frame;
    rt_size_t slot;

    if (!context || !request || !index || !request->data || !request->length ||
        request->length > AIC8800_ETHERNET_FRAME_MAX)
    {
        return -RT_EINVAL;
    }
    frame = rt_malloc(request->length);
    if (!frame)
    {
        return -RT_ENOMEM;
    }
    rt_memcpy(frame, request->data, request->length);
    if (!context->mgmt_confirmation_mutex_initialized ||
        rt_mutex_take(&context->mgmt_confirmation_mutex,
                      RT_WAITING_FOREVER) != RT_EOK)
    {
        rt_free(frame);
        return -RT_EIO;
    }
    for (slot = 0; slot < AIC8800_MGMT_CONFIRM_COUNT; slot++)
    {
        struct aic8800_mgmt_confirmation *confirmation =
            &context->mgmt_confirmations[slot];

        if (confirmation->used)
        {
            continue;
        }
        confirmation->used = RT_TRUE;
        confirmation->index = context->next_mgmt_confirmation++ &
                              0x3fffffffU;
        confirmation->request_id = request->request_id;
        confirmation->cookie = request->cookie;
        confirmation->frame = frame;
        confirmation->frame_length = request->length;
        confirmation->deadline = rt_tick_get() + rt_tick_from_millisecond(
            AIC8800_WIFI_MGMT_CONFIRM_TIMEOUT_MS);
        confirmation->iftype = iftype;
        *index = confirmation->index;
        rt_mutex_release(&context->mgmt_confirmation_mutex);
        return RT_EOK;
    }
    rt_mutex_release(&context->mgmt_confirmation_mutex);
    rt_free(frame);
    return -RT_EFULL;
}

static void aic_cancel_mgmt_confirmations(struct aic8800_context *context,
                                          rt_err_t status)
{
    struct aic8800_mgmt_confirmation confirmation;
    rt_size_t slot;

    if (!context->mgmt_confirmation_mutex_initialized)
    {
        return;
    }
    for (slot = 0; slot < AIC8800_MGMT_CONFIRM_COUNT; slot++)
    {
        rt_bool_t found = RT_FALSE;

        rt_memset(&confirmation, 0, sizeof(confirmation));
        if (rt_mutex_take(&context->mgmt_confirmation_mutex,
                          RT_WAITING_FOREVER) != RT_EOK)
        {
            return;
        }
        if (context->mgmt_confirmations[slot].used)
        {
            confirmation = context->mgmt_confirmations[slot];
            rt_memset(&context->mgmt_confirmations[slot], 0,
                      sizeof(context->mgmt_confirmations[slot]));
            found = RT_TRUE;
        }
        rt_mutex_release(&context->mgmt_confirmation_mutex);
        if (found)
        {
            aic_report_mgmt_confirmation(context, &confirmation,
                                         status, RT_FALSE);
        }
    }
}

static void aic_mgmt_confirmation_timeout(void *parameter)
{
    struct aic8800_context *context = parameter;
    rt_tick_t now = rt_tick_get();
    rt_size_t slot;

    if (!context || !context->mgmt_confirmation_mutex_initialized)
    {
        return;
    }
    for (slot = 0; slot < AIC8800_MGMT_CONFIRM_COUNT; slot++)
    {
        struct aic8800_mgmt_confirmation confirmation;
        rt_bool_t expired = RT_FALSE;

        rt_memset(&confirmation, 0, sizeof(confirmation));
        if (rt_mutex_take(&context->mgmt_confirmation_mutex,
                          RT_WAITING_FOREVER) != RT_EOK)
        {
            return;
        }
        if (context->mgmt_confirmations[slot].used &&
            (rt_int32_t)(now - context->mgmt_confirmations[slot].deadline) >= 0)
        {
            confirmation = context->mgmt_confirmations[slot];
            rt_memset(&context->mgmt_confirmations[slot], 0,
                      sizeof(context->mgmt_confirmations[slot]));
            expired = RT_TRUE;
        }
        rt_mutex_release(&context->mgmt_confirmation_mutex);
        if (expired)
        {
            aic_report_mgmt_confirmation(context, &confirmation,
                                         -RT_ETIMEOUT, RT_FALSE);
        }
    }
}

static rt_err_t aic_handle_data_confirmation(
    struct aic8800_context *context, const rt_uint8_t *record,
    rt_size_t length)
{
    struct aic8800_mgmt_confirmation confirmation;
    rt_uint32_t status;
    rt_uint32_t index;

    if (!context || !record || length < 12U ||
        aic_record_payload_length(context, record) != 8U)
    {
        return -RT_EIO;
    }
    status = aic_get_le32(record + 4);
    index = aic_get_le32(record + 8);
    rt_memset(&confirmation, 0, sizeof(confirmation));
    if (!aic_take_mgmt_confirmation(context, index, &confirmation))
    {
        if (context->invalid_rx_log_count < 8)
        {
            context->invalid_rx_log_count++;
            LOG_W("unexpected TX confirmation index %u", index);
        }
        return RT_EOK;
    }
    aic_report_mgmt_confirmation(
        context, &confirmation, RT_EOK,
        (status & AIC_TX_STATUS_ACKNOWLEDGED) != 0);
    return RT_EOK;
}

rt_err_t aic8800_core_receive(struct rt_wlan_offload_bus *bus, const void *data,
                              rt_size_t length, void *parameter)
{
    struct aic8800_context *context = parameter;
    const rt_uint8_t *cursor;
    rt_size_t remaining;
    rt_err_t last_result = -RT_EEMPTY;

    if (!context || bus != &context->bus || !data)
    {
        return -RT_EINVAL;
    }
    cursor = data;
    remaining = length;
    while (remaining >= AIC8800_USB_HEADER_SIZE)
    {
        rt_uint16_t packet_length =
            aic_record_payload_length(context, cursor);
        rt_uint8_t type = cursor[2] & 0x7fU;
        rt_size_t raw_length;
        rt_size_t record_length;

        if (!packet_length && !type)
        {
            break;
        }
        if ((type & AIC_USB_TYPE_CONFIG) != AIC_USB_TYPE_CONFIG)
        {
            raw_length = (rt_size_t)packet_length +
                         AIC8800_USB_RX_HEADER_SIZE;
        }
        else
        {
            raw_length = (rt_size_t)packet_length + 4U;
        }
        record_length = aic_align4(raw_length);
        if (raw_length < AIC8800_USB_HEADER_SIZE || raw_length > remaining)
        {
            return -RT_EIO;
        }
        if (record_length > remaining)
        {
            record_length = raw_length;
        }

        if ((type & AIC_USB_TYPE_CONFIG) != AIC_USB_TYPE_CONFIG)
        {
            last_result = aic_handle_data(context, cursor, raw_length);
        }
        else if (type == AIC_USB_TYPE_COMMAND)
        {
            last_result = aic_handle_command(context, cursor, raw_length);
        }
        else if (type == AIC_USB_TYPE_DATA_CONFIRM)
        {
            last_result = aic_handle_data_confirmation(
                context, cursor, raw_length);
        }
        else if (type == AIC_USB_TYPE_PRINT)
        {
            rt_size_t print_length = raw_length > 4U ? raw_length - 4U : 0;

            LOG_D("firmware: %.*s", (int)print_length, cursor + 4);
            last_result = RT_EOK;
        }
        else
        {
            if (context->invalid_rx_log_count < 8U)
            {
                context->invalid_rx_log_count++;
                LOG_W("ignored unsupported firmware record type 0x%02x",
                      type);
            }
            last_result = RT_EOK;
        }
        cursor += record_length;
        remaining -= record_length;
    }
    return last_result;
}

static void aic_bus_event(struct rt_wlan_offload_bus *bus,
                          enum rt_wlan_offload_bus_event event, rt_err_t status,
                          void *parameter)
{
    struct aic8800_context *context = parameter;
    struct rt_wlan_offload_event report;

    if (!context || bus != &context->bus ||
        (event != RT_WLAN_OFFLOAD_BUS_EVENT_ERROR &&
         event != RT_WLAN_OFFLOAD_BUS_EVENT_UNAVAILABLE))
    {
        return;
    }
    rt_wlan_offload_command_manager_fail(&context->commands,
                                    status == RT_EOK ? -RT_EIO : status);
    aic_cancel_mgmt_confirmations(
        context, status == RT_EOK ? -RT_EIO : status);
    if (!context->attached || context->radio.state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        return;
    }
    rt_memset(&report, 0, sizeof(report));
    report.type = RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR;
    report.iftype = RT_WLAN_OFFLOAD_IFTYPE_MAX;
    report.status = status == RT_EOK ? -RT_EIO : status;
    report.data.firmware.reason = event;
    rt_wlan_offload_report_event(&context->radio, &report);
}

static struct aic8800_context *aic_context_from_radio(
    struct rt_wlan_offload_radio *radio)
{
    return radio ? rt_wlan_offload_get_driver_data(radio) : RT_NULL;
}

static struct aic8800_context *aic_context_from_vif(
    struct rt_wlan_offload_vif *vif)
{
    return vif ? aic_context_from_radio(vif->radio) : RT_NULL;
}

static rt_err_t aic_configure_channels(struct aic8800_context *context)
{
    rt_uint8_t channels[AIC_ME_CHANNEL_CONFIG_SIZE];
    rt_size_t channel_count_2ghz = 0;
    rt_size_t channel_count_5ghz = 0;
    rt_size_t index;
    rt_err_t result;

    rt_memset(channels, 0, sizeof(channels));
    for (index = 0; index < context->band_2ghz.channel_count; index++)
    {
        const struct rt_wlan_offload_channel *channel =
            &context->channels_2ghz[index];
        rt_uint8_t *wire;

        if (!aic8800_radio_channel_allowed(context, channel->band,
                                           channel->number))
        {
            continue;
        }
        wire = channels + channel_count_2ghz * 6U;
        aic_put_le16(wire, channel->center_frequency_mhz);
        wire[2] = 0;
        wire[3] = aic_channel_flags(channel->flags);
        wire[4] = (rt_uint8_t)aic8800_radio_channel_fw_power(
            context, channel->band, channel->number,
            AIC_CHANNEL_DEFAULT_POWER_DBM);
        channel_count_2ghz++;
    }
    channels[252] = (rt_uint8_t)channel_count_2ghz;
#ifdef AIC8800_WIFI_5GHZ
    if (context->radio.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] &&
        (!context->firmware_capabilities_valid ||
         context->firmware_supports_5ghz))
    {
        for (index = 0; index < context->band_5ghz.channel_count; index++)
        {
            const struct rt_wlan_offload_channel *channel =
                &context->channels_5ghz[index];
            rt_uint8_t *wire;

            if (!aic8800_radio_channel_allowed(context, channel->band,
                                               channel->number))
            {
                continue;
            }
            wire = channels + (14U + channel_count_5ghz) * 6U;
            aic_put_le16(wire, channel->center_frequency_mhz);
            wire[2] = 1;
            wire[3] = aic_channel_flags(channel->flags);
            wire[4] = (rt_uint8_t)aic8800_radio_channel_fw_power(
                context, channel->band, channel->number,
                AIC_CHANNEL_DEFAULT_POWER_DBM);
            channel_count_5ghz++;
        }
        channels[253] = (rt_uint8_t)channel_count_5ghz;
    }
#endif
    result = aic_execute(context, AIC_ME_CHAN_CONFIG_REQ,
                         AIC_ME_CHAN_CONFIG_CFM, channels, sizeof(channels),
                         RT_NULL, 0, RT_NULL);
    if (result != RT_EOK)
    {
        LOG_E("channel configuration failed: %d", result);
    }
    return result;
}

static rt_err_t aic_configure_firmware(struct aic8800_context *context)
{
    struct aic_wire_mm_set_stack_start_req stack_request;
    struct aic_wire_mm_set_stack_start_cfm stack_response;
    struct aic_wire_mm_get_fw_version_cfm runtime_version;
    rt_uint8_t runtime_version_request = 0;
    rt_uint8_t version[28];
    rt_uint8_t mac_response[6];
    rt_uint8_t get_mac[4];
    struct rt_wlan_offload_firmware_info info;
    rt_size_t response_length = 0;
    rt_uint32_t raw_features;
    rt_bool_t disable_5ghz_profile;
    rt_err_t result;

    rt_memset(&stack_request, 0, sizeof(stack_request));
    stack_request.start = 1;
    /* Keep the vendor-info request consistent with Linux.  DC/DW firmware
     * has no 5 GHz vendor profile, and AIC8801 follows the build option. */
    disable_5ghz_profile =
        context->product_id == AIC8800_USB_PID_AIC8800DC ||
        context->product_id == AIC8800_USB_PID_AIC8800DW;
#ifndef AIC8800_WIFI_5GHZ
    if (context->product_id == AIC8800_USB_PID_AIC8801)
    {
        disable_5ghz_profile = RT_TRUE;
    }
#endif
    if (disable_5ghz_profile)
    {
        stack_request.vendor_info = 0;
    }
    else
    {
        stack_request.vendor_info = 1U << 5;
    }
    rt_memset(&stack_response, 0, sizeof(stack_response));
    response_length = 0;
    result = aic_execute(context, AIC_MM_SET_STACK_START_REQ,
                         AIC_MM_SET_STACK_START_CFM, &stack_request,
                         sizeof(stack_request), &stack_response,
                         sizeof(stack_response), &response_length);
    if (result != RT_EOK || response_length < sizeof(stack_response))
    {
        LOG_E("firmware stack start failed: %d, length=%u", result,
              (unsigned int)response_length);
        return result == RT_EOK ? -RT_EIO : result;
    }
    LOG_I("firmware stack started: 5GHz=%u vendor=0x%02x",
          stack_response.supports_5ghz, stack_response.vendor_info);
    context->firmware_supports_5ghz = stack_response.supports_5ghz != 0;
    context->firmware_capabilities_valid = RT_TRUE;

    rt_memset(&runtime_version, 0, sizeof(runtime_version));
    response_length = 0;
    result = aic_execute(context, AIC_MM_GET_FW_VERSION_REQ,
                         AIC_MM_GET_FW_VERSION_CFM,
                         &runtime_version_request,
                         sizeof(runtime_version_request), &runtime_version,
                         sizeof(runtime_version), &response_length);
    if (result != RT_EOK)
    {
        /* This optional query does not gate firmware initialization. */
        LOG_W("runtime firmware version query failed: %d; continuing",
              result);
    }
    else if (response_length > 1U && runtime_version.length)
    {
        rt_size_t version_length = runtime_version.length;

        if (version_length > sizeof(runtime_version.version))
        {
            version_length = sizeof(runtime_version.version);
        }
        if (version_length > response_length - 1U)
        {
            version_length = response_length - 1U;
        }
        LOG_I("runtime firmware: %.*s", (int)version_length,
              runtime_version.version);
    }

    result = aic8800_radio_initialize(context);
    if (result != RT_EOK)
    {
        LOG_E("RF initialization failed: %d", result);
        return result;
    }

    rt_memset(get_mac, 0, sizeof(get_mac));
    aic_put_le32(get_mac, 1);
    rt_memset(mac_response, 0, sizeof(mac_response));
    response_length = 0;
    result = aic_execute(context, AIC_MM_GET_MAC_REQ, AIC_MM_GET_MAC_CFM,
                         get_mac, sizeof(get_mac), mac_response,
                         sizeof(mac_response), &response_length);
    if (result == RT_EOK && response_length >= 6 &&
        aic_mac_valid(mac_response))
    {
        rt_memcpy(context->address, mac_response, 6);
    }
    else
    {
        LOG_E("firmware MAC query failed (%d), length=%u",
              result, (unsigned int)response_length);
        return result == RT_EOK ? -RT_EIO : result;
    }
    rt_memcpy(context->radio.permanent_address, context->address, 6);
    rt_memcpy(context->radio.vifs[0].address, context->address, 6);
    rt_memcpy(context->radio.vifs[1].address, context->address, 6);
    context->radio.vifs[1].address[0] ^= 0x02U;

    result = aic_execute(context, AIC_MM_RESET_REQ, AIC_MM_RESET_CFM,
                         RT_NULL, 0, RT_NULL, 0, RT_NULL);
    if (result != RT_EOK)
    {
        LOG_E("firmware reset failed: %d", result);
        return result;
    }
    rt_memset(version, 0, sizeof(version));
    response_length = 0;
    result = aic_execute(context, AIC_MM_VERSION_REQ, AIC_MM_VERSION_CFM,
                         RT_NULL, 0, version, sizeof(version),
                         &response_length);
    if (result != RT_EOK ||
        response_length < sizeof(struct aic_wire_mm_version_cfm))
    {
        LOG_E("firmware version query failed: %d, length=%u", result,
              (unsigned int)response_length);
        return result == RT_EOK ? -RT_EIO : result;
    }
    raw_features = aic_get_le32(version + 20);
    context->firmware_phy_features = aic_get_le32(version + 12);
    context->firmware_features = aic_decode_firmware_features(
        context, raw_features, &context->firmware_feature_map_compact);
#if defined(AIC8800_WIFI_POWER_SAVE)
    if (context->firmware_features & AIC_FW_CAP_PS)
    {
        context->radio.capabilities |= RT_WLAN_OFFLOAD_CAP_POWER_SAVE;
    }
    else
#endif
    {
        context->radio.capabilities &= ~RT_WLAN_OFFLOAD_CAP_POWER_SAVE;
    }
    /* The radio is registered before MM_VERSION is available.  Refresh the
     * advertised PHY capabilities now that the firmware feature bitmap is
     * authoritative. */
    aic_refresh_channel_metadata(context);

    result = aic_send_me_config(context);
    if (result != RT_EOK)
    {
        return result;
    }

    result = aic_configure_channels(context);
    if (result != RT_EOK)
    {
        return result;
    }

    rt_memset(&info, 0, sizeof(info));
    info.protocol_version = aic8800_protocol_version(context);
    info.firmware_version = aic_get_le32(version);
    info.features = raw_features;
    info.max_stations = aic_get_le16(version + 24);
    info.max_vifs = version[26];
    info.max_channel_contexts = 1;
    result = rt_wlan_offload_update_firmware_info(&context->radio, &info);
    if (result != RT_EOK)
    {
        LOG_E("firmware limits rejected by WLAN offload: %d", result);
        return result;
    }
    LOG_I("firmware ready: version=0x%08x features=0x%08x MAC=%02x:%02x:%02x:%02x:%02x:%02x",
          info.firmware_version, info.features,
          context->address[0], context->address[1], context->address[2],
          context->address[3], context->address[4], context->address[5]);
    return RT_EOK;
}

static rt_err_t aic_wlan_offload_start(struct rt_wlan_offload_radio *radio)
{
    struct aic8800_context *context = aic_context_from_radio(radio);
    rt_err_t result;

    if (!context || !context->transport_connected)
    {
        return -RT_EIO;
    }
    context->firmware_capabilities_valid = RT_FALSE;
    context->firmware_supports_5ghz = RT_FALSE;
    context->firmware_features = 0;
    context->firmware_phy_features = 0;
    context->firmware_feature_map_compact = RT_FALSE;
    context->power_save_level = 0;
    context->current_channel_valid = RT_FALSE;
    result = rt_wlan_offload_command_manager_reset(&context->commands);
    if (result != RT_EOK)
    {
        return result;
    }
    result = aic_configure_firmware(context);
    if (result != RT_EOK)
    {
        context->firmware_capabilities_valid = RT_FALSE;
        context->firmware_supports_5ghz = RT_FALSE;
        context->firmware_features = 0;
        context->firmware_phy_features = 0;
        context->firmware_feature_map_compact = RT_FALSE;
        context->power_save_level = 0;
        context->radio.capabilities &= ~RT_WLAN_OFFLOAD_CAP_POWER_SAVE;
    }
    return result;
}

static rt_err_t aic_wlan_offload_stop(struct rt_wlan_offload_radio *radio)
{
    struct aic8800_context *context = aic_context_from_radio(radio);

    if (!context)
    {
        return -RT_EINVAL;
    }
    rt_wlan_offload_command_manager_fail(&context->commands, -RT_EIO);
    aic_cancel_mgmt_confirmations(context, -RT_EIO);
    aic_rx_reorder_reset(context);
    aic_tcp_ack_reset(context);
    context->scan_completion_pending = RT_FALSE;
    context->scan_followup_pending = RT_FALSE;
    if (context->scan_work_initialized)
    {
        rt_work_cancel_sync(&context->scan_work);
        context->scan_work_queued = RT_FALSE;
    }
    if (context->ap_rechannel_work_initialized)
    {
        rt_work_cancel_sync(&context->ap_rechannel_work);
        context->ap_rechannel_work_queued = RT_FALSE;
    }
    if (context->traffic_work_initialized)
    {
        rt_work_cancel_sync(&context->traffic_work);
        context->traffic_work_queued = RT_FALSE;
    }
    if (context->station_loss_work_initialized)
    {
        rt_work_cancel_sync(&context->station_loss_work);
        context->station_loss_work_queued = RT_FALSE;
        rt_memset(context->station_loss, 0,
                  sizeof(context->station_loss));
    }
    context->lmac_started = RT_FALSE;
    context->firmware_capabilities_valid = RT_FALSE;
    context->firmware_supports_5ghz = RT_FALSE;
    context->firmware_features = 0;
    context->firmware_phy_features = 0;
    context->firmware_feature_map_compact = RT_FALSE;
    context->radio.capabilities &= ~RT_WLAN_OFFLOAD_CAP_POWER_SAVE;
    context->station_enabled = RT_FALSE;
    context->station_connected = RT_FALSE;
    context->station_qos = RT_FALSE;
    context->station_acm = 0;
    context->station_interface_recycle_pending = RT_FALSE;
    context->ap_enabled = RT_FALSE;
    context->ap_started = RT_FALSE;
    context->ap_paused_for_station = RT_FALSE;
    context->ap_resume_on_station_channel = RT_FALSE;
    context->promiscuous_enabled = RT_FALSE;
    context->filter_enabled = RT_FALSE;
    context->filter_offset = 0;
    context->filter_length = 0;
    context->power_save_level = 0;
    context->current_channel_valid = RT_FALSE;
    context->rssi = 0;
    rt_memset(context->bssid, 0, sizeof(context->bssid));
    rt_memset(&context->auth, 0, sizeof(context->auth));
    context->scan_request_id = 0;
    context->connect_request_id = 0;
    context->disconnect_request_id = 0;
    context->wep_enabled = RT_FALSE;
    context->wep_auth_error = RT_FALSE;
    context->station_control_port_pending = RT_FALSE;
    context->wep_last_auth_type = RT_WLAN_OFFLOAD_AUTH_OPEN;
    context->vif_index = AIC8800_INVALID_INDEX;
    context->ap_station_index = AIC8800_INVALID_INDEX;
    context->ap_vif_index = AIC8800_INVALID_INDEX;
    context->ap_broadcast_station_index = AIC8800_INVALID_INDEX;
    context->station_channel_index = AIC8800_INVALID_INDEX;
    context->ap_channel_index = AIC8800_INVALID_INDEX;
    context->active_channel_index = AIC8800_INVALID_INDEX;
    context->channel_context_tracked = RT_FALSE;
    aic8800_core_tx_pending_reset(context);
    rt_memset(context->ap_stations, 0, sizeof(context->ap_stations));
    aic_clear_saved_ap(context);
    aic_clear_hardware_keys(context, RT_FALSE);
    return RT_EOK;
}

static rt_err_t aic_change_interface(struct rt_wlan_offload_vif *vif,
                                     enum rt_wlan_offload_iftype iftype,
                                     rt_bool_t enabled)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_uint8_t request[10];
    rt_uint8_t confirmation[2];
    rt_size_t confirmation_length = 0;
    rt_err_t result;
    rt_bool_t *interface_enabled;
    rt_uint8_t *interface_index;

    if (!context || !vif ||
        (iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION &&
         iftype != RT_WLAN_OFFLOAD_IFTYPE_AP))
    {
        return -RT_ENOSYS;
    }
    interface_enabled = iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
                        &context->station_enabled : &context->ap_enabled;
    interface_index = iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
                      &context->vif_index : &context->ap_vif_index;
    if (*interface_enabled == enabled)
    {
        return RT_EOK;
    }
    if (!enabled)
    {
        rt_uint8_t remove_request = *interface_index;

        result = aic_execute(context, AIC_MM_REMOVE_IF_REQ,
                             AIC_MM_REMOVE_IF_CFM, &remove_request,
                             sizeof(remove_request), RT_NULL, 0, RT_NULL);
        if (result == RT_EOK)
        {
            aic_rx_reorder_reset(context);
            aic_tcp_ack_reset(context);
            *interface_enabled = RT_FALSE;
            *interface_index = AIC8800_INVALID_INDEX;
            aic_clear_vif_hardware_keys(context, iftype);
            if (iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION)
            {
                context->station_connected = RT_FALSE;
                context->station_interface_recycle_pending = RT_FALSE;
                context->promiscuous_enabled = RT_FALSE;
                context->filter_enabled = RT_FALSE;
                context->filter_offset = 0;
                context->filter_length = 0;
                context->ap_station_index = AIC8800_INVALID_INDEX;
                context->station_channel_index = AIC8800_INVALID_INDEX;
                context->scan_request_id = 0;
                context->scan_completion_pending = RT_FALSE;
                context->scan_followup_pending = RT_FALSE;
                context->connect_request_id = 0;
                context->disconnect_request_id = 0;
                context->wep_enabled = RT_FALSE;
                context->wep_auth_error = RT_FALSE;
                context->station_control_port_pending = RT_FALSE;
                context->wep_last_auth_type = RT_WLAN_OFFLOAD_AUTH_OPEN;
                context->current_channel_valid = RT_FALSE;
                context->rssi = 0;
                rt_memset(context->bssid, 0, sizeof(context->bssid));
                rt_memset(&context->auth, 0, sizeof(context->auth));
                if (context->ap_paused_for_station)
                {
                    aic_schedule_ap_resume(context, RT_FALSE);
                }
            }
            else
            {
                context->ap_started = RT_FALSE;
                context->ap_paused_for_station = RT_FALSE;
                context->ap_resume_on_station_channel = RT_FALSE;
                context->ap_broadcast_station_index = AIC8800_INVALID_INDEX;
                context->ap_channel_index = AIC8800_INVALID_INDEX;
                aic8800_core_tx_pending_reset(context);
                rt_memset(context->ap_stations, 0,
                          sizeof(context->ap_stations));
                aic_clear_saved_ap(context);
            }
        }
        return result;
    }

    if (!context->lmac_started)
    {
        struct aic_wire_mm_start_req start_request;

        rt_memset(&start_request, 0, sizeof(start_request));
        aic_put_le32(&start_request.uapsd_timeout, AIC_UAPSD_TIMEOUT_MS);
        aic_put_le16(&start_request.lp_clock_accuracy,
                     AIC_LP_CLOCK_ACCURACY_PPM);
        result = aic_execute(context, AIC_MM_START_REQ, AIC_MM_START_CFM,
                             &start_request, sizeof(start_request),
                             RT_NULL, 0, RT_NULL);
        if (result != RT_EOK)
        {
            return result;
        }
        result = aic_configure_runtime_rx_gain(context);
        if (result != RT_EOK)
        {
            LOG_E("runtime RX gain setup failed: %d", result);
            return result;
        }
        result = aic_configure_coexistence(context);
        if (result != RT_EOK)
        {
            LOG_W("Wi-Fi/Bluetooth coexistence setup failed: %d", result);
        }
        context->lmac_started = RT_TRUE;
    }

    rt_memset(request, 0, sizeof(request));
    request[0] = iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ? 0 : 2;
    rt_memcpy(request + 2, vif->address, 6);
    rt_memset(&confirmation, 0, sizeof(confirmation));
    result = aic_execute(context, AIC_MM_ADD_IF_REQ, AIC_MM_ADD_IF_CFM,
                         request, sizeof(request), confirmation,
                         sizeof(confirmation), &confirmation_length);
    if (result != RT_EOK)
    {
        return result;
    }
    if (confirmation_length < 2U)
    {
        return -RT_EIO;
    }
    result = aic_confirmation_status(confirmation, confirmation_length);
    if (result == RT_EOK && confirmation_length >= 2)
    {
        *interface_index = confirmation[1];
        *interface_enabled = RT_TRUE;
        LOG_I("%s VIF %u enabled",
              iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ? "station" : "AP",
              *interface_index);
    }
    return result;
}

/* ME_CONFIG carries the PHY capabilities the firmware puts into the
 * association request, so it has to be resent whenever those change. */
static rt_err_t aic_send_me_config(struct aic8800_context *context)
{
    struct aic_wire_me_config_req me_config;
    rt_err_t result;

    aic8800_radio_prepare(context, &me_config);
    context->power_save_level = me_config.power_save_enabled ? 1 : 0;
    result = aic_execute(context, AIC_ME_CONFIG_REQ, AIC_ME_CONFIG_CFM,
                         &me_config, sizeof(me_config), RT_NULL, 0, RT_NULL);
    if (result != RT_EOK)
    {
        LOG_E("ME configuration failed: %d", result);
        return result;
    }
    context->me_config_stale = RT_FALSE;
    return RT_EOK;
}

/* Republish the advertised capabilities before associating when a previous
 * association proved one of them wrong.  This runs on the caller's thread, not
 * a receive worker, so the synchronous command is safe here. */
static rt_err_t aic_apply_pending_me_config(struct aic8800_context *context)
{
    if (!context->me_config_stale || !context->lmac_started)
    {
        return RT_EOK;
    }
    LOG_I("republishing PHY capabilities before associating");
    return aic_send_me_config(context);
}

static rt_err_t aic_recycle_station_interface(
    struct rt_wlan_offload_vif *vif)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_err_t result;

    if (!context || !vif ||
        vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_STATION)
    {
        return -RT_EINVAL;
    }
    if (!context->station_interface_recycle_pending)
    {
        return RT_EOK;
    }
    if (context->station_connected || context->connect_request_id ||
        context->scan_request_id)
    {
        return -RT_EBUSY;
    }

    if (context->station_enabled)
    {
        result = aic_change_interface(vif,
                                      RT_WLAN_OFFLOAD_IFTYPE_STATION,
                                      RT_FALSE);
        if (result != RT_EOK)
        {
            context->station_interface_recycle_pending = RT_TRUE;
            LOG_E("station VIF recycle remove failed: %d", result);
            return result;
        }
    }
    result = aic_change_interface(vif, RT_WLAN_OFFLOAD_IFTYPE_STATION,
                                  RT_TRUE);
    if (result != RT_EOK)
    {
        context->station_interface_recycle_pending = RT_TRUE;
        LOG_E("station VIF recycle add failed: %d", result);
        return result;
    }
    context->station_interface_recycle_pending = RT_FALSE;
    LOG_I("station VIF recycled after disconnect");
    return RT_EOK;
}

static rt_err_t aic_set_scan_ies(struct aic8800_context *context,
                                 const struct rt_wlan_offload_scan_request *request)
{
    rt_uint8_t vendor_request[260];
    rt_uint8_t confirmation[1];
    rt_size_t confirmation_length = 0;
    rt_err_t result;

    if (!request->ies_length)
    {
        return RT_EOK;
    }
    if (request->ies_length > AIC_SCAN_IE_MAX)
    {
        return -RT_EINVAL;
    }
    if (!request->ies)
    {
        return -RT_EINVAL;
    }
    rt_memset(vendor_request, 0, sizeof(vendor_request));
    aic_put_le16(vendor_request, request->ies_length);
    vendor_request[2] = context->vif_index;
    rt_memcpy(vendor_request + 3, request->ies, request->ies_length);
    result = aic_execute(context, AIC_SCANU_VENDOR_IE_REQ,
                         AIC_SCANU_VENDOR_IE_CFM, vendor_request,
                         sizeof(vendor_request), confirmation,
                         sizeof(confirmation), &confirmation_length);
    return result == RT_EOK ?
           aic_confirmation_status(confirmation, confirmation_length) : result;
}

static void aic_prepare_scan_common(
    struct aic8800_context *context,
    const struct rt_wlan_offload_scan_request *request,
    struct aic_wire_scanu_start_req *wire)
{
    rt_uint32_t duration_ms = request->duration_ms;
    rt_size_t index;

    rt_memset(wire, 0, sizeof(*wire));
    for (index = 0; index < request->ssid_count; index++)
    {
        wire->ssids[index].length = request->ssids[index].length;
        rt_memcpy(wire->ssids[index].array, request->ssids[index].value,
                  wire->ssids[index].length);
    }
    rt_memset(&wire->bssid, 0xff, sizeof(wire->bssid));
    if (!aic_mac_is_zero(request->bssid))
    {
        rt_memcpy(&wire->bssid, request->bssid, sizeof(wire->bssid));
    }
    wire->vif_index = context->vif_index;
    wire->ssid_count = (rt_uint8_t)request->ssid_count;
    if (!(request->flags & RT_WLAN_OFFLOAD_SCAN_PASSIVE))
    {
        /* Include a wildcard entry so active scans send broadcast probes.
         * Retain named entries as well so hidden networks remain reachable. */
        if (!wire->ssid_count)
        {
            wire->ssid_count = 1;
        }
        else if (wire->ssid_count < AIC_SCAN_SSID_COUNT)
        {
            rt_bool_t wildcard = RT_FALSE;

            for (index = 0; index < wire->ssid_count; index++)
            {
                wildcard = wildcard || !wire->ssids[index].length;
            }
            if (!wildcard)
            {
                wire->ssids[wire->ssid_count].length = 0;
                wire->ssid_count++;
            }
        }
    }
    /* Leave duration at zero unless the caller asked for a specific dwell.
     * Both vendor Linux drivers always send zero here - the SDIO one spells it
     * out ("req->duration = 0") and the USB one never assigns the field - which
     * leaves the LMAC scan module scheduling its own per-channel dwell. A
     * host-supplied value overrides that schedule and makes the firmware report
     * a different partial subset of the visible BSSes on every pass, so
     * directed scans issued by a join frequently return nothing at all. */
    aic_put_le32(&wire->duration_us, duration_ms * 1000U);
}

static rt_err_t aic_scan(struct rt_wlan_offload_vif *vif,
                         const struct rt_wlan_offload_scan_request *request)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic_wire_scanu_start_req scan_request;
    rt_size_t count_2ghz = 0;
    rt_size_t count_5ghz = 0;
    rt_size_t index;
    rt_err_t result;

    if (!context || !request || !context->station_enabled ||
        request->ssid_count > AIC_SCAN_SSID_COUNT ||
        request->channel_count > AIC_SCAN_CHANNEL_COUNT ||
        (request->ssid_count && !request->ssids) ||
        (request->channel_count && !request->channels) ||
        (request->ies_length && !request->ies))
    {
        return -RT_EINVAL;
    }
    if (context->scan_request_id)
    {
        return -RT_EBUSY;
    }
    for (index = 0; index < request->ssid_count; index++)
    {
        if (request->ssids[index].length > RT_WLAN_SSID_MAX_LENGTH)
        {
            return -RT_EINVAL;
        }
    }
    result = aic_set_scan_ies(context, request);
    if (result != RT_EOK)
    {
        return result;
    }
    aic_prepare_scan_common(context, request, &scan_request);
    aic_prepare_scan_common(context, request, &context->scan_followup);
    if (request->channel_count)
    {
        for (index = 0; index < request->channel_count; index++)
        {
            const struct rt_wlan_offload_channel_definition *channel =
                &request->channels[index];
            const struct rt_wlan_offload_channel *configured;
            rt_uint8_t *wire;
            rt_uint16_t channel_number = channel->primary_channel ?
                channel->primary_channel :
                aic_frequency_to_channel(channel->primary_frequency_mhz);

            if (channel->band != RT_WLAN_OFFLOAD_BAND_2GHZ &&
                channel->band != RT_WLAN_OFFLOAD_BAND_5GHZ)
            {
                return -RT_EINVAL;
            }
            if (!aic_channel_allowed(context, channel->band, channel_number))
            {
                return -RT_EINVAL;
            }
            configured = aic_channel_metadata(
                context, channel->band, channel_number);
            if (!configured ||
                (configured->flags & RT_WLAN_OFFLOAD_CHANNEL_DISABLED))
            {
                return -RT_EINVAL;
            }

            if (channel->band == RT_WLAN_OFFLOAD_BAND_2GHZ)
            {
                wire = (rt_uint8_t *)&scan_request.channels[count_2ghz++];
            }
            else
            {
                wire = (rt_uint8_t *)&context->scan_followup.channels[
                    count_5ghz++];
            }
            aic_encode_channel(
                wire, channel,
                aic8800_radio_channel_fw_power(
                    context, configured->band, configured->number,
                    AIC_CHANNEL_DEFAULT_POWER_DBM));
            wire[3] = aic_channel_flags(configured->flags);
            if (request->flags & RT_WLAN_OFFLOAD_SCAN_PASSIVE)
            {
                wire[3] |= 1U;
            }
        }
    }
    else
    {
        for (index = 0; index < context->band_2ghz.channel_count; index++)
        {
            const struct rt_wlan_offload_channel *configured =
                &context->channels_2ghz[index];
            struct rt_wlan_offload_channel_definition channel;
            rt_uint8_t *wire =
                (rt_uint8_t *)&scan_request.channels[count_2ghz];

            if (!aic8800_radio_channel_allowed(
                    context, configured->band, configured->number))
            {
                continue;
            }

            aic_channel_definition(
                configured->center_frequency_mhz,
                &channel);

            aic_encode_channel(wire,
                               &channel,
                               aic8800_radio_channel_fw_power(
                                   context, configured->band,
                                   configured->number,
                                   AIC_CHANNEL_DEFAULT_POWER_DBM));
            wire[3] = aic_channel_flags(configured->flags);
            if (request->flags & RT_WLAN_OFFLOAD_SCAN_PASSIVE)
            {
                wire[3] |= 1U;
            }
            count_2ghz++;
        }
#ifdef AIC8800_WIFI_5GHZ
        if (context->radio.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] &&
            (!context->firmware_capabilities_valid ||
             context->firmware_supports_5ghz))
        {
            for (index = 0; index < context->band_5ghz.channel_count; index++)
            {
                const struct rt_wlan_offload_channel *configured =
                    &context->channels_5ghz[index];
                struct rt_wlan_offload_channel_definition channel;
                rt_uint8_t *wire =
                    (rt_uint8_t *)&context->scan_followup.channels[
                        count_5ghz];

                if (!aic8800_radio_channel_allowed(
                        context, configured->band, configured->number))
                {
                    continue;
                }

                aic_channel_definition(
                    configured->center_frequency_mhz,
                    &channel);
                aic_encode_channel(wire,
                                   &channel,
                                   aic8800_radio_channel_fw_power(
                                       context, configured->band,
                                       configured->number,
                                       AIC_CHANNEL_DEFAULT_POWER_DBM));
                wire[3] = aic_channel_flags(configured->flags);
                if (request->flags & RT_WLAN_OFFLOAD_SCAN_PASSIVE)
                {
                    wire[3] |= 1U;
                }
                count_5ghz++;
            }
        }
#endif
    }
    if (!count_2ghz && !count_5ghz)
    {
        return -RT_EINVAL;
    }
    if (!count_2ghz)
    {
        scan_request = context->scan_followup;
        scan_request.channel_count = (rt_uint8_t)count_5ghz;
        count_5ghz = 0;
    }
    else
    {
        scan_request.channel_count = (rt_uint8_t)count_2ghz;
    }
    context->scan_followup.channel_count = (rt_uint8_t)count_5ghz;

    context->scan_request_id = request->request_id;
    context->scan_results_2ghz = 0;
    context->scan_results_5ghz = 0;
    context->scan_completion_pending = RT_FALSE;
    context->scan_expected_results = 0;
    context->scan_result_count_valid = RT_TRUE;
    context->scan_completion_retry_count = 0;
    context->scan_followup_pending = count_5ghz != 0;
    context->scan_followup_retry_count = 0;
    context->scan_work_queued = RT_FALSE;
    if (aic_get_le32(&scan_request.duration_us))
    {
        LOG_I("starting scan: first=%u channels followup-5GHz=%u channels "
              "dwell=%u ms",
              scan_request.channel_count,
              context->scan_followup.channel_count,
              (unsigned int)(aic_get_le32(&scan_request.duration_us) / 1000U));
    }
    else
    {
        LOG_I("starting scan: first=%u channels followup-5GHz=%u channels "
              "dwell=firmware",
              scan_request.channel_count,
              context->scan_followup.channel_count);
    }
    result = aic_submit_scan_wire(context, &scan_request);
    if (result != RT_EOK)
    {
        context->scan_request_id = 0;
        context->scan_followup_pending = RT_FALSE;
        return result;
    }
    return RT_EOK;
}

static rt_err_t aic_abort_scan(struct rt_wlan_offload_vif *vif,
                               rt_uint32_t request_id)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_uint8_t confirmation[1];
    rt_size_t confirmation_length = 0;
    rt_err_t result;
    rt_err_t completion_status;

    if (!context || !request_id || context->scan_request_id != request_id)
    {
        return -RT_EINVAL;
    }
    context->scan_completion_pending = RT_FALSE;
    context->scan_followup_pending = RT_FALSE;
    if (context->scan_work_initialized)
    {
        rt_work_cancel_sync(&context->scan_work);
        context->scan_work_queued = RT_FALSE;
    }
    result = aic_execute(context, AIC_SCANU_CANCEL_REQ,
                         AIC_SCANU_CANCEL_CFM, RT_NULL, 0, confirmation,
                         sizeof(confirmation), &confirmation_length);
    completion_status = result;
    if (result == RT_EOK)
    {
        completion_status = aic_confirmation_status(confirmation,
                                                    confirmation_length);
    }
    if (context->scan_request_id)
    {
        aic_report_scan_done_status(
            context, completion_status, RT_FALSE, 0);
    }
    return completion_status;
}

static rt_uint8_t aic_auth_type(enum rt_wlan_offload_auth_type auth_type)
{
    switch (auth_type)
    {
    case RT_WLAN_OFFLOAD_AUTH_SHARED: return 1;
    case RT_WLAN_OFFLOAD_AUTH_FT: return 2;
    case RT_WLAN_OFFLOAD_AUTH_SAE: return 3;
    default: return 0;
    }
}

static rt_err_t aic_connect_submit(
    struct aic8800_context *context, rt_uint32_t request_id,
    const rt_wlan_ssid_t *ssid, const rt_uint8_t bssid[6],
    const struct rt_wlan_offload_channel_definition *channel,
    enum rt_wlan_offload_auth_type auth_type, rt_wlan_security_t security,
    const rt_uint8_t *ies, rt_size_t ies_length, rt_bool_t host_supplicant)
{
    rt_uint8_t request[AIC_CONNECT_REQUEST_SIZE];
    rt_uint8_t confirmation[1];
    rt_uint32_t flags = 0;
    rt_size_t confirmation_length = 0;
    rt_bool_t rechannel_ap = RT_FALSE;
    rt_err_t result;

    if (!context || !ssid || ssid->len > RT_WLAN_SSID_MAX_LENGTH ||
        ies_length > 256 || (ies_length && !ies))
    {
        return -RT_EINVAL;
    }
    if (context->ap_started)
    {
        if (channel && channel->primary_frequency_mhz &&
            !aic_channel_definition_same_primary(channel,
                                                 &context->ap_channel))
        {
            LOG_I("station channel %u differs from AP channel %u; pausing AP",
                  channel->primary_channel,
                  context->ap_channel.primary_channel);
            rechannel_ap = RT_TRUE;
        }
        if (!channel || !channel->primary_frequency_mhz)
        {
            channel = &context->ap_channel;
        }
    }
    if (security == SECURITY_UNKNOWN)
    {
        return -RT_EINVAL;
    }
    if (host_supplicant)
    {
        if (!aic_security_uses_host_supplicant(security))
        {
            return -RT_ENOSYS;
        }
    }
    else if (!aic_security_supported(security))
    {
        return -RT_ENOSYS;
    }
    rt_memset(request, 0, sizeof(request));
    request[0] = ssid->len;
    rt_memcpy(request + 1, ssid->val, ssid->len);
    rt_memset(request + 34, 0xff, 6);
    if (bssid && !aic_mac_is_zero(bssid))
    {
        rt_memcpy(request + 34, bssid, 6);
    }
    if (channel && channel->primary_frequency_mhz)
    {
        rt_uint16_t channel_number = channel->primary_channel ?
            channel->primary_channel :
            aic_frequency_to_channel(channel->primary_frequency_mhz);

        if (!aic_channel_allowed(context, channel->band, channel_number))
        {
            return -RT_EINVAL;
        }
        aic_encode_channel(
            request + 40, channel,
            aic8800_radio_channel_fw_power(context, channel->band,
                                           channel_number,
                                           AIC_CHANNEL_DEFAULT_POWER_DBM));
    }
    else
    {
        aic_put_le16(request + 40, 0xffff);
    }
    if (host_supplicant)
    {
        flags |= AIC_CONNECTION_CONTROL_PORT_HOST |
                 AIC_CONNECTION_CONTROL_PORT_NO_ENC;
    }
    if (aic_security_uses_host_supplicant(security))
    {
        flags |= AIC_CONNECTION_WPA;
    }
    if (security & (WEP_ENABLED | TKIP_ENABLED))
    {
        flags |= AIC_CONNECTION_DISABLE_HT;
    }
    if ((security & (WPA3_SECURITY | OWE_ENABLED | DPP_ENABLED)) ||
        aic_rsn_ies_enable_mfp(ies, ies_length))
    {
        if (context->firmware_capabilities_valid &&
            !(context->firmware_features & AIC_FW_CAP_MFP))
        {
            return -RT_ENOSYS;
        }
        flags |= AIC_CONNECTION_MFP;
    }
    aic_put_le32(request + 48, flags);
    request[52] = 0x88;
    request[53] = 0x8e;
    aic_put_le16(request + 54, (rt_uint16_t)ies_length);
    /* Match the vendor Linux defaults. A zero listen interval lets the
     * firmware derive the value, while one U-APSD voice queue is its normal
     * station policy. */
    aic_put_le16(request + 56, 0);
    request[59] = aic_auth_type(auth_type);
    request[60] = AIC_UAPSD_QUEUE_VO;
    request[61] = context->vif_index;
    if (ies_length)
    {
        rt_memcpy(request + 64, ies, ies_length);
    }

    if (rechannel_ap)
    {
        result = aic_stop_ap_firmware(context);
        if (result != RT_EOK)
        {
            LOG_E("cannot pause AP for station connection: %d", result);
            return result;
        }
        context->ap_paused_for_station = RT_TRUE;
        context->ap_resume_on_station_channel = RT_TRUE;
    }

    context->current_channel_valid = RT_FALSE;
    context->connect_request_id = request_id;
    context->station_control_port_pending = host_supplicant;
    context->station_control_port_open = RT_FALSE;
    rt_memset(confirmation, 0, sizeof(confirmation));
    result = aic_execute(context, AIC_SM_CONNECT_REQ, AIC_SM_CONNECT_CFM,
                         request, sizeof(request), confirmation,
                         sizeof(confirmation), &confirmation_length);
    if (result != RT_EOK ||
        aic_confirmation_status(confirmation, confirmation_length) != RT_EOK)
    {
        LOG_E("connect request failed: transport=%d firmware=%u security=0x%08x",
              result,
              confirmation_length ? (unsigned int)confirmation[0] : 0xffU,
              (unsigned int)security);
        context->connect_request_id = 0;
        context->station_control_port_pending = RT_FALSE;
        context->current_channel_valid = RT_FALSE;
        if (context->ap_paused_for_station)
        {
            aic_schedule_ap_resume(context, RT_FALSE);
        }
        return result != RT_EOK ? result : -RT_ERROR;
    }
    if (channel && channel->primary_frequency_mhz)
    {
        context->current_channel = *channel;
        context->current_channel_valid = RT_TRUE;
    }
    return RT_EOK;
}

static rt_err_t aic_connect(struct rt_wlan_offload_vif *vif,
                            const struct rt_wlan_offload_connect_request *request)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_bool_t protected_network;
    rt_bool_t wep_network;
    struct rt_wlan_offload_key wep_key;
    rt_uint8_t wep_value[13];
    rt_size_t wep_length;
    enum rt_wlan_offload_auth_type auth_type;
    rt_err_t result;

    if (!context || !request)
    {
        return -RT_EINVAL;
    }
    result = aic_recycle_station_interface(vif);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!context->station_enabled)
    {
        return -RT_EINVAL;
    }
    result = aic_apply_pending_me_config(context);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!aic_security_supported(request->security))
    {
        return -RT_ENOSYS;
    }
    if (request->security == SECURITY_OPEN && request->key.len)
    {
        return -RT_EINVAL;
    }
    protected_network = request->security != SECURITY_OPEN;
    wep_network = (request->security & WEP_ENABLED) &&
                  !(request->security & (WPA_SECURITY | WPA2_SECURITY |
                                         WPA3_SECURITY));
    auth_type = request->security & WPA3_SECURITY ?
                RT_WLAN_OFFLOAD_AUTH_SAE :
                request->security & SHARED_ENABLED ?
                RT_WLAN_OFFLOAD_AUTH_SHARED : RT_WLAN_OFFLOAD_AUTH_OPEN;
    if ((request->security & (WPA_SECURITY | WPA2_SECURITY |
                              WPA3_SECURITY)) && request->key.len &&
        !request->ies_length)
    {
        LOG_W("protected connection is missing an RSN association element");
        return -RT_ENOSYS;
    }
    if (wep_network)
    {
        if (!(request->security & SHARED_ENABLED))
        {
            if (!context->wep_enabled)
            {
                auth_type = RT_WLAN_OFFLOAD_AUTH_SHARED;
            }
            else if (context->wep_auth_error)
            {
                auth_type = context->wep_last_auth_type ==
                            RT_WLAN_OFFLOAD_AUTH_SHARED ?
                            RT_WLAN_OFFLOAD_AUTH_OPEN :
                            RT_WLAN_OFFLOAD_AUTH_SHARED;
            }
            else
            {
                auth_type = context->wep_last_auth_type;
            }
        }
        context->wep_enabled = RT_TRUE;
        context->wep_auth_error = RT_FALSE;
        context->wep_last_auth_type = auth_type;
        LOG_I("WEP authentication using %s system",
              auth_type == RT_WLAN_OFFLOAD_AUTH_SHARED ? "shared-key" :
                                                         "open");
        /* WEP is handled by the firmware.  Install the default shared key
         * before starting association; the embedded WPA supplicant must not
         * be used for this legacy cipher. */
        if (!aic_decode_wep_key(&request->key, wep_value, &wep_length))
        {
            return -RT_EINVAL;
        }
        rt_memset(&wep_key, 0, sizeof(wep_key));
        wep_key.cipher = wep_length == 5U ? RT_WLAN_OFFLOAD_CIPHER_WEP40 :
                                                  RT_WLAN_OFFLOAD_CIPHER_WEP104;
        wep_key.index = 0;
        wep_key.set_transmit = RT_TRUE;
        wep_key.key_length = wep_length;
        rt_memcpy(wep_key.key, wep_value, wep_length);
        result = aic_add_key(vif, 0, &wep_key);
        rt_memset(wep_value, 0, sizeof(wep_value));
        if (result != RT_EOK)
        {
            return result;
        }
    }
    else
    {
        context->wep_enabled = RT_FALSE;
        context->wep_auth_error = RT_FALSE;
        context->wep_last_auth_type = RT_WLAN_OFFLOAD_AUTH_OPEN;
    }
    result = aic_connect_submit(
        context, request->request_id, &request->ssid, request->bssid,
        &request->channel, auth_type,
        request->security,
        request->ies, request->ies_length, protected_network && !wep_network);
    if (result != RT_EOK && wep_network)
    {
        /* A failed association must not leave the default WEP key armed for
         * the next request.  The firmware key delete is best effort; the
         * host bookkeeping is cleared by aic_delete_key(). */
        (void)aic_delete_key(vif, 0, 0, RT_FALSE, RT_NULL);
    }
    return result;
}

static rt_err_t aic_external_auth_response(struct rt_wlan_offload_vif *vif,
                                            rt_uint16_t status)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic_wire_sm_external_auth_required_rsp response;

    if (!context || !context->station_enabled)
    {
        return -RT_EINVAL;
    }
    rt_memset(&response, 0, sizeof(response));
    response.vif_index = context->vif_index;
    response.status = status;
    /* This can be called from the firmware indication worker. Queue the
     * response without waiting on that same worker for its confirmation. */
    return aic_command_push(&context->commands, 0,
                            AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP,
                            &response, sizeof(response), context);
}

static rt_err_t aic_auth(struct rt_wlan_offload_vif *vif,
                         const struct rt_wlan_offload_auth_request *request)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_err_t result;

    if (!context || !request)
    {
        return -RT_EINVAL;
    }
    result = aic_recycle_station_interface(vif);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!context->station_enabled)
    {
        return -RT_EINVAL;
    }
    if (request->auth_data_length && !request->auth_data)
    {
        return -RT_EINVAL;
    }
    if (request->auth_data_length)
    {
        /* AIC firmware performs SAE itself through the external-auth path;
         * raw authentication frames are not part of its station ABI. */
        return -RT_ENOSYS;
    }
    rt_memset(&context->auth, 0, sizeof(context->auth));
    context->auth.valid = RT_TRUE;
    context->auth.ssid = request->ssid;
    rt_memcpy(context->auth.bssid, request->bssid, 6);
    context->auth.channel = request->channel;
    context->auth.auth_type = request->auth_type;
    return RT_EOK;
}

static rt_err_t aic_assoc(struct rt_wlan_offload_vif *vif,
                          const struct rt_wlan_offload_assoc_request *request)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_wlan_security_t security;

    if (!context || !request || !context->station_enabled ||
        !context->auth.valid)
    {
        return -RT_EINVAL;
    }
    if (request->ies_length > sizeof(((struct aic_wire_sm_connect_req *)0)->ie_buffer) ||
        (request->ies_length && !request->ies))
    {
        return -RT_EINVAL;
    }
    security = request->ies_length ?
        aic_security_from_ies(0x0010, request->ies, request->ies_length) :
        SECURITY_OPEN;
    if (security == SECURITY_UNKNOWN || (security & WEP_ENABLED))
    {
        return -RT_ENOSYS;
    }
    return aic_connect_submit(
        context, request->request_id, &context->auth.ssid, request->bssid,
        &context->auth.channel, context->auth.auth_type, security,
        request->ies, request->ies_length,
        aic_security_uses_host_supplicant(security));
}

static rt_err_t aic_disconnect(struct rt_wlan_offload_vif *vif,
                               rt_uint32_t request_id, rt_uint16_t reason)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_uint8_t request[4];
    rt_err_t result;

    if (!context || !context->station_enabled)
    {
        return -RT_EINVAL;
    }
    rt_memset(request, 0, sizeof(request));
    aic_put_le16(request, reason ? reason : 3);
    request[2] = context->vif_index;
    context->disconnect_request_id = request_id;
    result = aic_execute(context, AIC_SM_DISCONNECT_REQ,
                         AIC_SM_DISCONNECT_CFM, request, sizeof(request),
                         RT_NULL, 0, RT_NULL);
    if (result != RT_EOK)
    {
        context->disconnect_request_id = 0;
        if (context->ap_paused_for_station)
        {
            context->station_control_port_pending = RT_FALSE;
            aic_schedule_ap_resume(context, RT_FALSE);
        }
    }
    else
    {
        context->connect_request_id = 0;
    }
    return result;
}

/* Advances *offset past the appended element.  Returning an error instead of a
 * zero offset keeps a truncated element from resetting the write position to
 * the start of the beacon and corrupting its fixed header. */
static rt_err_t aic_beacon_add_ie(rt_uint8_t *beacon, rt_size_t capacity,
                                  rt_size_t *offset, rt_uint8_t id,
                                  const void *data, rt_size_t length)
{
    rt_size_t position;

    if (!beacon || !offset)
    {
        return -RT_EINVAL;
    }
    position = *offset;
    if (length > 255U || position + 2U + length > capacity)
    {
        return -RT_EFULL;
    }
    beacon[position++] = id;
    beacon[position++] = (rt_uint8_t)length;
    if (length)
    {
        rt_memcpy(beacon + position, data, length);
    }
    *offset = position + length;
    return RT_EOK;
}

static rt_err_t aic_build_beacon(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_ap_settings *settings,
    struct aic_wire_apm_set_beacon_ie_req *request,
    rt_uint16_t *tim_offset, rt_uint8_t *tim_length)
{
    static const rt_uint8_t rates_2ghz[] =
        {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    static const rt_uint8_t rates_5ghz[] =
        {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
    static const rt_uint8_t extended_rates[] = {0x30, 0x48, 0x60, 0x6c};
    static const rt_uint8_t tim[] = {0, 1, 0, 0};
    static const rt_uint8_t erp[] = {0};
    static const rt_uint8_t vht_capability[] = {
        0x32, 0x01, 0x80, 0x03, /* MPDU 11454, LDPC, SGI80, RX STBC, AMPDU x7 */
        0xfe, 0xff, 0x86, 0x01, /* RX: one stream, MCS 0-9, 390 Mbps */
        0xfe, 0xff, 0x86, 0x01  /* TX: one stream, MCS 0-9, 390 Mbps */
    };
    static const rt_uint8_t rsn[] = {
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
    };
    static const rt_uint8_t wmm[] = {
        0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x80, 0x00,
        0x03, 0xa4, 0x00, 0x00, 0x27, 0xa4, 0x00, 0x00,
        0x42, 0x43, 0x5e, 0x00, 0x62, 0x32, 0x2f, 0x00
    };
    rt_uint8_t ht_cap[26] = {0};
    rt_uint8_t ht_operation[22] = {0};
    rt_uint8_t vht_operation[5] = {0};
    rt_uint8_t channel;
    rt_uint16_t capability;
    const rt_uint8_t *rates;
    rt_size_t offset = 36U;
    rt_size_t capacity;
    rt_err_t result;

    if (!vif || !settings || !request || !tim_offset || !tim_length ||
        settings->ssid.len > RT_WLAN_SSID_MAX_LENGTH)
    {
        return -RT_EINVAL;
    }
    capacity = sizeof(request->beacon);
    rt_memset(request, 0, sizeof(*request));
    request->vif_index = vif->radio ?
        ((struct aic8800_context *)vif->radio->driver_data)->ap_vif_index : 0;
    request->beacon[0] = 0x80;
    rt_memset(request->beacon + 4, 0xff, 6);
    rt_memcpy(request->beacon + 10, vif->address, 6);
    rt_memcpy(request->beacon + 16, vif->address, 6);
    aic_put_le16(request->beacon + 32,
                 settings->beacon_interval ? settings->beacon_interval : 100U);
    capability = settings->security == SECURITY_OPEN ? 0x0401U : 0x0411U;
    aic_put_le16(request->beacon + 34, capability);
    result = aic_beacon_add_ie(request->beacon, capacity, &offset, 0,
        settings->hidden ? RT_NULL : settings->ssid.val,
        settings->hidden ? 0 : settings->ssid.len);
    if (result != RT_EOK)
    {
        return result;
    }
    rates = settings->channel.band == RT_WLAN_OFFLOAD_BAND_5GHZ ?
            rates_5ghz : rates_2ghz;
    result = aic_beacon_add_ie(request->beacon, capacity, &offset, 1,
                              rates, 8);
    if (result != RT_EOK)
    {
        return result;
    }
    channel = (rt_uint8_t)(settings->channel.primary_channel ?
        settings->channel.primary_channel :
        aic_frequency_to_channel(settings->channel.primary_frequency_mhz));
    if (settings->channel.band == RT_WLAN_OFFLOAD_BAND_2GHZ)
    {
        result = aic_beacon_add_ie(request->beacon, capacity, &offset, 3,
                                  &channel, 1);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    *tim_offset = (rt_uint16_t)offset;
    result = aic_beacon_add_ie(request->beacon, capacity, &offset, 5,
                              tim, sizeof(tim));
    if (result != RT_EOK)
    {
        return result;
    }
    *tim_length = 6;
    if (settings->channel.band == RT_WLAN_OFFLOAD_BAND_2GHZ)
    {
        result = aic_beacon_add_ie(request->beacon, capacity, &offset, 42,
                                  erp, sizeof(erp));
        if (result == RT_EOK)
        {
            result = aic_beacon_add_ie(request->beacon, capacity, &offset, 50,
                                      extended_rates,
                                      sizeof(extended_rates));
        }
        if (result != RT_EOK)
        {
            return result;
        }
    }
    ht_cap[0] = 0x63;
    ht_cap[1] = 0x09;
    ht_cap[2] = 0x1f;
    ht_cap[3] = 0xff;
    result = aic_beacon_add_ie(request->beacon, capacity, &offset, 45,
                              ht_cap, sizeof(ht_cap));
    if (result != RT_EOK)
    {
        return result;
    }
    ht_operation[0] = channel;
    if (settings->channel.width == RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80)
    {
        rt_int16_t center_offset =
            (rt_int16_t)settings->channel.center_frequency1_mhz -
            (rt_int16_t)settings->channel.primary_frequency_mhz;

        ht_operation[1] = (center_offset == 30 || center_offset == -10) ?
                          0x05U : 0x07U;
    }
    result = aic_beacon_add_ie(request->beacon, capacity, &offset, 61,
                              ht_operation, sizeof(ht_operation));
    if (result != RT_EOK)
    {
        return result;
    }
    if (settings->channel.width == RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80)
    {
        vht_operation[0] = 1;
        vht_operation[1] = (rt_uint8_t)aic_frequency_to_channel(
            settings->channel.center_frequency1_mhz);
        vht_operation[3] = 0xfc;
        vht_operation[4] = 0xff;
        result = aic_beacon_add_ie(request->beacon, capacity, &offset, 191,
                                  vht_capability, sizeof(vht_capability));
        if (result == RT_EOK)
        {
            result = aic_beacon_add_ie(request->beacon, capacity, &offset, 192,
                                      vht_operation, sizeof(vht_operation));
        }
        if (result != RT_EOK)
        {
            return result;
        }
    }
    result = aic_beacon_add_ie(request->beacon, capacity, &offset, 221,
                              wmm, sizeof(wmm));
    if (result != RT_EOK)
    {
        return result;
    }
    if (settings->security != SECURITY_OPEN && !settings->beacon_ies_length)
    {
        result = aic_beacon_add_ie(request->beacon, capacity, &offset, 48,
                                  rsn, sizeof(rsn));
        if (result != RT_EOK)
        {
            return result;
        }
    }
    if (settings->beacon_ies_length)
    {
        if (!settings->beacon_ies ||
            settings->beacon_ies_length > capacity - offset)
        {
            return -RT_EFULL;
        }
        rt_memcpy(request->beacon + offset, settings->beacon_ies,
                  settings->beacon_ies_length);
        offset += settings->beacon_ies_length;
    }
    if (offset <= 36U || offset > capacity)
    {
        return -RT_EFULL;
    }
    request->beacon_length = (rt_uint16_t)offset;
    return RT_EOK;
}

static void aic_report_ap_state(struct aic8800_context *context,
                                rt_uint32_t request_id, rt_bool_t started,
                                rt_err_t status)
{
    struct rt_wlan_offload_event event;

    rt_memset(&event, 0, sizeof(event));
    event.type = started ? RT_WLAN_OFFLOAD_EVENT_AP_STARTED :
                           RT_WLAN_OFFLOAD_EVENT_AP_STOPPED;
    event.iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
    event.request_id = request_id;
    event.status = status;
    if (started)
    {
        rt_memcpy(event.data.network.bssid,
                  context->radio.vifs[1].address, 6);
    }
    rt_wlan_offload_report_event(&context->radio, &event);
}

static void aic_clear_saved_ap(struct aic8800_context *context)
{
    if (!context)
    {
        return;
    }
    context->ap_settings_valid = RT_FALSE;
    rt_memset(&context->ap_settings, 0, sizeof(context->ap_settings));
    rt_memset(context->ap_beacon_ies, 0, sizeof(context->ap_beacon_ies));
}

static rt_err_t aic_save_ap(
    struct aic8800_context *context,
    const struct rt_wlan_offload_ap_settings *settings)
{
    if (!context || !settings ||
        settings->beacon_ies_length > sizeof(context->ap_beacon_ies) ||
        (settings->beacon_ies_length && !settings->beacon_ies))
    {
        return -RT_EINVAL;
    }
    context->ap_settings = *settings;
    rt_memset(&context->ap_settings.key, 0,
              sizeof(context->ap_settings.key));
    if (settings->beacon_ies_length)
    {
        rt_memcpy(context->ap_beacon_ies, settings->beacon_ies,
                  settings->beacon_ies_length);
        context->ap_settings.beacon_ies = context->ap_beacon_ies;
    }
    else
    {
        context->ap_settings.beacon_ies = RT_NULL;
    }
    context->ap_settings_valid = RT_TRUE;
    return RT_EOK;
}

static rt_err_t aic_start_ap_firmware(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_ap_settings *settings)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic_wire_apm_set_beacon_ie_req beacon;
    struct aic_wire_apm_start_req request;
    struct aic_wire_apm_start_cfm confirmation;
    struct rt_wlan_offload_ap_settings concurrent;
    rt_size_t confirmation_length = 0;
    rt_uint16_t tim_offset;
    rt_uint8_t tim_length;
    rt_uint16_t channel_number;
    rt_uint16_t bandwidth_mhz;
    rt_err_t result;

    if (!context || !settings || !context->ap_enabled || context->ap_started ||
        (settings->security != SECURITY_OPEN &&
         settings->security != SECURITY_WPA2_AES_PSK))
    {
        return -RT_EINVAL;
    }
    channel_number = settings->channel.primary_channel ?
        settings->channel.primary_channel :
        aic_frequency_to_channel(settings->channel.primary_frequency_mhz);
    if (!channel_number || !aic_channel_allowed(context, settings->channel.band,
                                                 channel_number))
    {
        return -RT_EINVAL;
    }
    if (context->station_connected)
    {
        if (!aic_channel_definition_same_primary(&context->current_channel,
                                                 &settings->channel))
        {
            return -RT_EBUSY;
        }
        /* One radio can hold exactly one channel context.  The caller only
         * carries the station's band and primary channel over to the AP - the
         * width and centre frequencies are re-derived from scratch and get
         * upgraded to the widest the band allows, so a station linked at
         * 40 MHz ends up starting the AP at 80 MHz on the same primary.  The
         * firmware then has to serve two incompatible channel definitions on
         * one PHY; it survives beaconing but wedges once both VIFs carry data,
         * taking every USB endpoint down with it.  Adopt the station's live
         * definition instead, before the beacon is built, so the HT/VHT
         * operation elements advertise the bandwidth actually in use. */
        if (context->current_channel_valid &&
            !aic_channel_definition_equal(&context->current_channel,
                                          &settings->channel))
        {
            concurrent = *settings;
            concurrent.channel = context->current_channel;
            settings = &concurrent;
            LOG_I("concurrent AP follows station channel definition: "
                  "primary=%u center1=%u center2=%u",
                  (unsigned int)settings->channel.primary_channel,
                  (unsigned int)settings->channel.center_frequency1_mhz,
                  (unsigned int)settings->channel.center_frequency2_mhz);
        }
    }
    result = aic_build_beacon(vif, settings, &beacon, &tim_offset,
                              &tim_length);
    if (result != RT_EOK) return result;
    result = aic_execute(context, AIC_APM_SET_BEACON_IE_REQ,
                         AIC_APM_SET_BEACON_IE_CFM, &beacon, sizeof(beacon),
                         RT_NULL, 0, RT_NULL);
    if (result != RT_EOK) return result;

    rt_memset(&request, 0, sizeof(request));
    if (settings->channel.band == RT_WLAN_OFFLOAD_BAND_5GHZ)
    {
        request.basic_rates.length = 3;
        request.basic_rates.array[0] = 0x8c;
        request.basic_rates.array[1] = 0x98;
        request.basic_rates.array[2] = 0xb0;
    }
    else
    {
        request.basic_rates.length = 4;
        request.basic_rates.array[0] = 0x82;
        request.basic_rates.array[1] = 0x84;
        request.basic_rates.array[2] = 0x8b;
        request.basic_rates.array[3] = 0x96;
    }
    aic_encode_channel((rt_uint8_t *)&request.channel, &settings->channel,
        aic8800_radio_channel_fw_power(context, settings->channel.band,
                                       channel_number,
                                       AIC_CHANNEL_DEFAULT_POWER_DBM));
    request.center_frequency1 = settings->channel.center_frequency1_mhz ?
        settings->channel.center_frequency1_mhz :
        settings->channel.primary_frequency_mhz;
    request.center_frequency2 = settings->channel.center_frequency2_mhz;
    switch (settings->channel.width)
    {
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20_NOHT:
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20:
        request.channel_width = 0;
        bandwidth_mhz = 20;
        break;
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40:
        request.channel_width = 1;
        bandwidth_mhz = 40;
        break;
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80:
        request.channel_width = 2;
        bandwidth_mhz = 80;
        break;
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_160:
    case RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80:
        /* APM_START_REQ can carry these, but aic_build_beacon() only emits a
         * VHT operation element for 20/40/80 MHz.  Starting here would put a
         * beacon describing a narrower channel on a wider one, so refuse
         * instead.  The advertised band maximum is 80 MHz, so the framework
         * should never ask; a direct rt_wlan_start_ap_with_channel() can. */
        LOG_E("SoftAP at %u MHz is not supported; the beacon builder covers "
              "20, 40 and 80 MHz only",
              (unsigned int)aic_channel_width_mhz(
                  settings->channel.width ==
                          RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80 ? 4U : 3U));
        return -RT_ENOSYS;
    default:
        return -RT_ENOSYS;
    }
    request.beacon_length = beacon.beacon_length;
    request.tim_offset = tim_offset;
    request.tim_length = tim_length;
    request.beacon_interval = settings->beacon_interval ?
                              settings->beacon_interval : 100U;
    request.vif_index = context->ap_vif_index;
    if (settings->security != SECURITY_OPEN)
    {
        request.flags = AIC_CONNECTION_CONTROL_PORT_HOST |
                        AIC_CONNECTION_CONTROL_PORT_NO_ENC |
                        AIC_CONNECTION_WPA;
        request.control_port_ethertype = 0x8e88U;
    }
    rt_memset(&confirmation, 0, sizeof(confirmation));
    result = aic_execute(context, AIC_APM_START_REQ, AIC_APM_START_CFM,
                         &request, sizeof(request), &confirmation,
                         sizeof(confirmation), &confirmation_length);
    if (result == RT_EOK &&
        (confirmation_length < sizeof(confirmation) || confirmation.status ||
         confirmation.vif_index != context->ap_vif_index ||
         confirmation.broadcast_station_index == AIC8800_INVALID_INDEX))
    {
        result = -RT_ERROR;
    }
    if (result == RT_EOK)
    {
        context->ap_started = RT_TRUE;
        context->ap_broadcast_station_index =
            confirmation.broadcast_station_index;
        context->ap_channel_index = confirmation.channel_index;
        aic_tx_credit_reset(context, context->ap_broadcast_station_index);
        context->ap_channel = settings->channel;
        LOG_I("AP started on channel %u, width=%u MHz (VIF=%u BCMC=%u CH=%u)",
              channel_number, (unsigned int)bandwidth_mhz,
              context->ap_vif_index, context->ap_broadcast_station_index,
              context->ap_channel_index);
        aic_channel_context_report(context, "AP started");
    }
    return result;
}

static rt_err_t aic_start_ap(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_ap_settings *settings)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_err_t result;

    if (!context || !settings || context->ap_started ||
        context->ap_paused_for_station ||
        settings->beacon_ies_length > sizeof(context->ap_beacon_ies) ||
        (settings->beacon_ies_length && !settings->beacon_ies))
    {
        return -RT_EINVAL;
    }
    result = aic_start_ap_firmware(vif, settings);
    if (result == RT_EOK)
    {
        result = aic_save_ap(context, settings);
        if (result == RT_EOK)
        {
            /* aic_start_ap_firmware() may have narrowed the channel to match a
             * live station link.  Save what the radio actually runs, or a
             * later resume would restore the mismatched definition. */
            context->ap_settings.channel = context->ap_channel;
        }
        else
        {
            (void)aic_stop_ap_firmware(context);
        }
    }
    if (result != RT_EOK)
    {
        aic_clear_saved_ap(context);
    }
    aic_report_ap_state(context, settings->request_id, RT_TRUE, result);
    return result;
}

static rt_err_t aic_stop_ap_firmware(struct aic8800_context *context)
{
    rt_uint8_t request;
    rt_err_t result;

    if (!context || !context->ap_enabled || !context->ap_started)
    {
        return -RT_EINVAL;
    }
    request = context->ap_vif_index;
    result = aic_execute(context, AIC_APM_STOP_REQ, AIC_APM_STOP_CFM,
                         &request, sizeof(request), RT_NULL, 0, RT_NULL);
    if (result == RT_EOK)
    {
        aic_rx_reorder_reset(context);
        aic_tcp_ack_reset(context);
        context->ap_started = RT_FALSE;
        context->ap_broadcast_station_index = AIC8800_INVALID_INDEX;
        context->ap_channel_index = AIC8800_INVALID_INDEX;
        aic8800_core_tx_pending_reset(context);
        rt_memset(context->ap_stations, 0, sizeof(context->ap_stations));
        aic_clear_vif_hardware_keys(context, RT_WLAN_OFFLOAD_IFTYPE_AP);
    }
    return result;
}

static rt_err_t aic_stop_ap(struct rt_wlan_offload_vif *vif,
                            rt_uint32_t request_id)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_err_t result;

    if (!context || !context->ap_enabled ||
        (!context->ap_started && !context->ap_paused_for_station))
    {
        return -RT_EINVAL;
    }
    if (context->ap_paused_for_station &&
        context->ap_rechannel_work_initialized)
    {
        rt_work_cancel_sync(&context->ap_rechannel_work);
        context->ap_rechannel_work_queued = RT_FALSE;
    }
    result = context->ap_started ? aic_stop_ap_firmware(context) : RT_EOK;
    if (result == RT_EOK)
    {
        context->ap_paused_for_station = RT_FALSE;
        context->ap_resume_on_station_channel = RT_FALSE;
        aic_clear_saved_ap(context);
    }
    aic_report_ap_state(context, request_id, RT_FALSE, result);
    return result;
}

static void aic_schedule_ap_resume(struct aic8800_context *context,
                                   rt_bool_t use_station_channel)
{
    rt_err_t result;

    if (!context || !context->ap_paused_for_station ||
        !context->ap_rechannel_work_initialized)
    {
        return;
    }
    context->ap_resume_on_station_channel = use_station_channel;
    if (context->ap_rechannel_work_queued)
    {
        return;
    }
    context->ap_rechannel_work_queued = RT_TRUE;
    result = rt_work_submit(&context->ap_rechannel_work, 0);
    if (result != RT_EOK)
    {
        context->ap_rechannel_work_queued = RT_FALSE;
        context->ap_paused_for_station = RT_FALSE;
        context->ap_resume_on_station_channel = RT_FALSE;
        aic_clear_saved_ap(context);
        LOG_E("cannot queue AP channel migration: %d", result);
        aic_report_ap_state(context, 0, RT_FALSE, result);
    }
}

static void aic_ap_rechannel_work(struct rt_work *work, void *work_data)
{
    struct aic8800_context *context = work_data;
    struct rt_wlan_offload_ap_settings settings;
    rt_err_t result;

    (void)work;
    if (!context)
    {
        return;
    }
    context->ap_rechannel_work_queued = RT_FALSE;
    if (!context->ap_paused_for_station || !context->ap_settings_valid)
    {
        return;
    }
    if (!context->attached || !context->transport_connected ||
        context->radio.state != RT_WLAN_OFFLOAD_STARTED ||
        !context->ap_enabled)
    {
        context->ap_paused_for_station = RT_FALSE;
        context->ap_resume_on_station_channel = RT_FALSE;
        aic_clear_saved_ap(context);
        return;
    }

    settings = context->ap_settings;
    if (context->ap_resume_on_station_channel &&
        context->station_connected && context->current_channel_valid)
    {
        settings.channel = context->current_channel;
    }
    result = aic_start_ap_firmware(
        &context->radio.vifs[1], &settings);
    if (result == RT_EOK)
    {
        result = rt_wlan_offload_ap_channel_changed(&context->radio,
                                                    &settings.channel);
    }
    if (result == RT_EOK)
    {
        context->ap_settings.channel = settings.channel;
        context->ap_paused_for_station = RT_FALSE;
        context->ap_resume_on_station_channel = RT_FALSE;
        LOG_I("AP resumed on station channel %u, width=%u",
              settings.channel.primary_channel,
              (unsigned int)settings.channel.width);
        return;
    }

    if (context->ap_started)
    {
        (void)aic_stop_ap_firmware(context);
    }
    context->ap_paused_for_station = RT_FALSE;
    context->ap_resume_on_station_channel = RT_FALSE;
    aic_clear_saved_ap(context);
    LOG_E("AP channel migration failed: %d", result);
    aic_report_ap_state(context, 0, RT_FALSE, result);
}

static rt_bool_t aic_find_ie(const rt_uint8_t *ies, rt_size_t length,
                             rt_uint8_t id, const rt_uint8_t **body,
                             rt_size_t *body_length)
{
    rt_size_t offset = 0;

    while (ies && offset + 2U <= length)
    {
        rt_size_t size = ies[offset + 1U];
        if (offset + 2U + size > length) return RT_FALSE;
        if (ies[offset] == id)
        {
            if (body) *body = ies + offset + 2U;
            if (body_length) *body_length = size;
            return RT_TRUE;
        }
        offset += 2U + size;
    }
    return RT_FALSE;
}

static rt_bool_t aic_find_extension_ie(
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

static rt_err_t aic_add_station(
    struct rt_wlan_offload_vif *vif, rt_uint32_t request_id,
    const struct rt_wlan_offload_station_parameters *station)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic_wire_me_sta_add_req request;
    struct aic_wire_me_sta_add_cfm confirmation;
    struct aic8800_ap_station *entry = RT_NULL;
    const rt_uint8_t *body;
    rt_size_t length;
    rt_size_t index;
    rt_size_t response_length = 0;
    rt_uint16_t uapsd_tids = 0;
    rt_err_t result;

    (void)request_id;
    if (!context || !station || !context->ap_started) return -RT_EINVAL;
    if (aic_find_ap_station(context, station->mac)) return -RT_EBUSY;
    for (index = 0; index < AIC8800_AP_STATION_COUNT; index++)
    {
        if (!context->ap_stations[index].valid)
        {
            entry = &context->ap_stations[index];
            break;
        }
    }
    if (!entry) return -RT_EFULL;
    rt_memset(&request, 0, sizeof(request));
    rt_memcpy(&request.address, station->mac, 6);
    if (aic_find_ie(station->association_ies,
                    station->association_ies_length, 1, &body, &length))
    {
        request.rates.length = length > 12U ? 12U : length;
        rt_memcpy(request.rates.array, body, request.rates.length);
    }
    if (request.rates.length < 12U &&
        aic_find_ie(station->association_ies,
                    station->association_ies_length, 50, &body, &length))
    {
        rt_size_t room = 12U - request.rates.length;
        if (length > room) length = room;
        rt_memcpy(request.rates.array + request.rates.length, body, length);
        request.rates.length += length;
    }
    if (aic_find_ie(station->association_ies,
                    station->association_ies_length, 45, &body, &length) &&
        length >= 26U)
    {
        request.flags |= 1UL << 1;
        request.ht.capability = aic_get_le16(body);
        request.ht.ampdu_parameters = body[2];
        rt_memcpy(request.ht.mcs_rate, body + 3, 16);
        request.ht.extended_capability = aic_get_le16(body + 19);
        request.ht.beamforming_capability = aic_get_le32(body + 21);
        request.ht.antenna_selection_capability = body[25];
    }
    if (aic_find_ie(station->association_ies,
                    station->association_ies_length, 191, &body, &length) &&
        length >= 12U)
    {
        request.flags |= 1UL << 2;
        request.vht.capability = aic_get_le32(body);
        request.vht.rx_mcs_map = aic_get_le16(body + 4);
        request.vht.rx_highest = aic_get_le16(body + 6);
        request.vht.tx_mcs_map = aic_get_le16(body + 8);
        request.vht.tx_highest = aic_get_le16(body + 10);
    }
    if (aic_find_extension_ie(
            station->association_ies, station->association_ies_length,
            35U, &body, &length) && length >= 21U)
    {
        rt_size_t mcs_offset = 17U;

        request.flags |= 1UL << 5;
        rt_memcpy(request.he.mac_capability, body, 6U);
        rt_memcpy(request.he.phy_capability, body + 6U, 11U);
        request.he.mcs.rx_mcs_80 = aic_get_le16(body + mcs_offset);
        request.he.mcs.tx_mcs_80 = aic_get_le16(body + mcs_offset + 2U);
        request.he.mcs.rx_mcs_160 = 0xffffU;
        request.he.mcs.tx_mcs_160 = 0xffffU;
        request.he.mcs.rx_mcs_80p80 = 0xffffU;
        request.he.mcs.tx_mcs_80p80 = 0xffffU;
        mcs_offset += 4U;
        if ((request.he.phy_capability[0] & 0x08U) &&
            length >= mcs_offset + 4U)
        {
            request.he.mcs.rx_mcs_160 = aic_get_le16(body + mcs_offset);
            request.he.mcs.tx_mcs_160 =
                aic_get_le16(body + mcs_offset + 2U);
            mcs_offset += 4U;
        }
        if ((request.he.phy_capability[0] & 0x10U) &&
            length >= mcs_offset + 4U)
        {
            request.he.mcs.rx_mcs_80p80 = aic_get_le16(body + mcs_offset);
            request.he.mcs.tx_mcs_80p80 =
                aic_get_le16(body + mcs_offset + 2U);
        }
    }
    for (index = 0; index + 8U <= station->association_ies_length; )
    {
        rt_size_t size = station->association_ies[index + 1U];
        if (index + 2U + size > station->association_ies_length) break;
        if (station->association_ies[index] == 221 && size >= 7U &&
            rt_memcmp(station->association_ies + index + 2,
                      "\x00\x50\xf2\x02", 4) == 0)
        {
            rt_uint8_t qos_info = station->association_ies[index + 8U];

            request.flags |= 1UL;
            /* Linux passes the WMM station QoS Info fields through in
             * ME_STA_ADD_REQ.  Firmware needs these in addition to the QoS
             * capability bit to schedule U-APSD service periods correctly. */
            request.uapsd_queues = qos_info & 0x0fU;
            request.max_sp_length = ((qos_info >> 5U) & 0x03U) * 2U;
            if (qos_info & (1U << 0)) uapsd_tids |= AIC_UAPSD_TIDS_VO;
            if (qos_info & (1U << 1)) uapsd_tids |= AIC_UAPSD_TIDS_VI;
            if (qos_info & (1U << 2)) uapsd_tids |= AIC_UAPSD_TIDS_BK;
            if (qos_info & (1U << 3)) uapsd_tids |= AIC_UAPSD_TIDS_BE;
            break;
        }
        index += 2U + size;
    }
    request.aid = station->aid;
    request.vif_index = context->ap_vif_index;
    rt_memset(&confirmation, 0, sizeof(confirmation));
    aic_station_add_begin(context);
    result = aic_execute(context, AIC_ME_STA_ADD_REQ, AIC_ME_STA_ADD_CFM,
                         &request, sizeof(request), &confirmation,
                         sizeof(confirmation), &response_length);
    if (result != RT_EOK)
    {
        aic_station_add_cancel(context);
        return result;
    }
    if (response_length < 2U || confirmation.status ||
        confirmation.station_index >= AIC8800_STATION_SLOTS)
    {
        aic_station_add_cancel(context);
        return -RT_ERROR;
    }
    result = rt_mutex_take(context->frame_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        aic_station_add_cancel(context);
        return result;
    }
    entry->aid = station->aid;
    entry->firmware_index = confirmation.station_index;
    entry->qos = (request.flags & 1UL) != 0;
    entry->acm = 0;
    entry->uapsd_tids = uapsd_tids;
    rt_memcpy(entry->address, station->mac, 6);
    entry->valid = RT_TRUE;
    aic_tx_credit_reset(context, entry->firmware_index);
    aic_station_state_set(
        context, entry->firmware_index, RT_TRUE,
        response_length >= 3U && confirmation.power_state, uapsd_tids,
        RT_TRUE);
    rt_mutex_release(context->frame_mutex);
    return RT_EOK;
}

static rt_err_t aic_del_station(struct rt_wlan_offload_vif *vif,
                                rt_uint32_t request_id,
                                const rt_uint8_t mac[6], rt_uint16_t reason)
{
    return aic_del_station_internal(vif, request_id, mac, reason, RT_FALSE);
}

static rt_err_t aic_del_station_internal(
    struct rt_wlan_offload_vif *vif, rt_uint32_t request_id,
    const rt_uint8_t mac[6], rt_uint16_t reason,
    rt_bool_t firmware_station_lost)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic8800_ap_station *entry;
    struct aic_wire_me_sta_del_req request;
    rt_bool_t power_save = RT_FALSE;
    rt_err_t result;

    (void)request_id;
    (void)reason;
    if (!context || !mac || !context->ap_started) return -RT_EINVAL;
    entry = aic_find_ap_station(context, mac);
    if (!entry) return RT_EOK;
    rt_memset(&request, 0, sizeof(request));
    request.station_index = entry->firmware_index;
    request.tdls_station = 0;
    result = rt_mutex_take(context->frame_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!context->tx_mutex_initialized)
    {
        rt_mutex_release(context->frame_mutex);
        return -RT_EIO;
    }
    result = rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_mutex_release(context->frame_mutex);
        return result;
    }
    (void)aic_station_state_snapshot(
        context, entry->firmware_index, RT_NULL, &power_save);
    aic_station_state_set(
        context, entry->firmware_index, RT_FALSE, RT_FALSE, 0, RT_FALSE);
    rt_mutex_release(context->tx_mutex);
    rt_mutex_release(context->frame_mutex);
    result = aic_execute(context, AIC_ME_STA_DEL_REQ, AIC_ME_STA_DEL_CFM,
                         &request, sizeof(request), RT_NULL, 0, RT_NULL);
    if (result == RT_EOK || firmware_station_lost)
    {
        struct aic8800_hardware_key *key;
        while ((key = aic_find_hardware_key(
                    context, RT_WLAN_OFFLOAD_IFTYPE_AP, 0, RT_TRUE,
                    mac, RT_FALSE)) != RT_NULL)
        {
            rt_memset(key, 0, sizeof(*key));
        }
        if (rt_mutex_take(context->frame_mutex, RT_WAITING_FOREVER) == RT_EOK)
        {
            rt_memset(entry, 0, sizeof(*entry));
            rt_mutex_release(context->frame_mutex);
        }
        else
        {
            rt_memset(entry, 0, sizeof(*entry));
        }
    }
    else
    {
        /* The firmware kept the entry.  Resume it with a fresh generation;
         * records queued before the failed delete remain invalid. */
        if (rt_mutex_take(context->frame_mutex, RT_WAITING_FOREVER) == RT_EOK)
        {
            if (context->tx_mutex_initialized &&
                rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER) == RT_EOK)
            {
                aic_station_state_set(
                    context, entry->firmware_index, RT_TRUE, power_save,
                    entry->uapsd_tids, RT_FALSE);
                rt_mutex_release(context->tx_mutex);
            }
            rt_mutex_release(context->frame_mutex);
        }
    }
    return result;
}

static rt_err_t aic_set_station_authorized(
    struct rt_wlan_offload_vif *vif, rt_uint32_t request_id,
    const rt_uint8_t mac[6], rt_bool_t authorized)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic8800_ap_station *entry;
    rt_err_t result;

    (void)request_id;
    if (!context || !mac)
    {
        return -RT_EINVAL;
    }
    if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION)
    {
        if (!context->station_connected ||
            context->ap_station_index == AIC8800_INVALID_INDEX)
        {
            return -RT_EBUSY;
        }
        if (rt_memcmp(mac, context->bssid, 6) != 0)
        {
            return -RT_EINVAL;
        }
        result = aic_set_control_port(context, context->ap_station_index,
                                      authorized);
        AIC8800_STAT(context->station_control_port_set_count++);
        if (result == RT_EOK)
        {
            context->station_control_port_open = authorized;
            LOG_I("station controlled port %s (STA=%u)",
                  authorized ? "opened" : "closed",
                  context->ap_station_index);
            if (authorized)
            {
                context->station_control_port_pending = RT_FALSE;
                if (context->ap_paused_for_station)
                {
                    aic_schedule_ap_resume(context, RT_TRUE);
                }
            }
        }
        else
        {
            AIC8800_STAT(context->station_control_port_error_count++);
            LOG_E("station controlled port update failed: %d (STA=%u)",
                  result, context->ap_station_index);
        }
        return result;
    }
    if (vif->iftype != RT_WLAN_OFFLOAD_IFTYPE_AP || !context->ap_started)
    {
        return -RT_EINVAL;
    }
    entry = aic_find_ap_station(context, mac);
    if (!entry) return -RT_EINVAL;
    result = aic_set_control_port(context, entry->firmware_index, authorized);
    if (result == RT_EOK) entry->authorized = authorized;
    return result;
}

static int aic_cipher_suite(enum rt_wlan_offload_cipher cipher)
{
    switch (cipher)
    {
    case RT_WLAN_OFFLOAD_CIPHER_WEP40: return 0;
    case RT_WLAN_OFFLOAD_CIPHER_TKIP: return 1;
    case RT_WLAN_OFFLOAD_CIPHER_CCMP: return 2;
    case RT_WLAN_OFFLOAD_CIPHER_WEP104: return 3;
    case RT_WLAN_OFFLOAD_CIPHER_AES_CMAC: return 5;
    default: return -1;
    }
}

static rt_bool_t aic_cipher_key_length_valid(enum rt_wlan_offload_cipher cipher,
                                             rt_size_t length)
{
    switch (cipher)
    {
    case RT_WLAN_OFFLOAD_CIPHER_WEP40:
        return length == 5U;
    case RT_WLAN_OFFLOAD_CIPHER_WEP104:
        return length == 13U;
    case RT_WLAN_OFFLOAD_CIPHER_TKIP:
        return length == 32U;
    case RT_WLAN_OFFLOAD_CIPHER_CCMP:
    case RT_WLAN_OFFLOAD_CIPHER_AES_CMAC:
        return length == 16U;
    default:
        return RT_FALSE;
    }
}

static rt_err_t aic_set_control_port(struct aic8800_context *context,
                                     rt_uint8_t station_index,
                                     rt_bool_t open)
{
    rt_uint8_t request[2];

    if (station_index == AIC8800_INVALID_INDEX)
    {
        return -RT_EINVAL;
    }
    request[0] = station_index;
    request[1] = open;
    return aic_execute(context, AIC_ME_CONTROL_PORT_REQ,
                       AIC_ME_CONTROL_PORT_CFM, request, sizeof(request),
                       RT_NULL, 0, RT_NULL);
}

static struct aic8800_hardware_key *aic_find_hardware_key(
    struct aic8800_context *context, enum rt_wlan_offload_iftype iftype,
    rt_uint8_t index, rt_bool_t pairwise, const rt_uint8_t peer[6],
    rt_bool_t allocate)
{
    struct aic8800_hardware_key *free_key = RT_NULL;
    rt_size_t slot;

    for (slot = 0; slot < AIC8800_HARDWARE_KEY_COUNT; slot++)
    {
        struct aic8800_hardware_key *entry = &context->hardware_keys[slot];

        if (!entry->valid)
        {
            if (!free_key) free_key = entry;
            continue;
        }
        if (entry->iftype == iftype && entry->index == index &&
            entry->pairwise == pairwise &&
            (!pairwise || !peer || rt_memcmp(entry->peer, peer, 6) == 0))
        {
            return entry;
        }
    }
    return allocate ? free_key : RT_NULL;
}

static rt_err_t aic_add_key(struct rt_wlan_offload_vif *vif,
                            rt_uint32_t request_id,
                            const struct rt_wlan_offload_key *key)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_uint8_t request[44];
    struct aic_wire_mm_key_add_cfm confirmation;
    rt_size_t confirmation_length = 0;
    rt_uint8_t hardware_index;
    rt_uint8_t station_index = AIC8800_INVALID_INDEX;
    rt_uint8_t vif_index;
    struct aic8800_hardware_key *entry;
    struct aic8800_ap_station *ap_station;
    int cipher;
    rt_err_t result;

    (void)request_id;
    if (!context || !vif || !key ||
        (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
         !context->station_enabled : !context->ap_enabled) ||
        key->index >= AIC8800_KEY_COUNT ||
        key->key_length > 32 ||
        !aic_cipher_key_length_valid(key->cipher, key->key_length))
    {
        return -RT_EINVAL;
    }
    vif_index = vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
                context->vif_index : context->ap_vif_index;
    if (key->pairwise && vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION)
    {
        station_index = context->ap_station_index;
    }
    else if (key->pairwise)
    {
        ap_station = aic_find_ap_station(context, key->peer);
        station_index = ap_station ? ap_station->firmware_index :
                                     AIC8800_INVALID_INDEX;
    }
    if (key->pairwise && station_index == AIC8800_INVALID_INDEX)
    {
        return -RT_EBUSY;
    }
    cipher = aic_cipher_suite(key->cipher);
    if (cipher < 0)
    {
        return -RT_ENOSYS;
    }
    /* The LMAC keeps a separate hardware index.  Replace an existing key
     * explicitly so reconnects and WEP key rotation do not exhaust the key
     * table. */
    entry = aic_find_hardware_key(context, vif->iftype, key->index,
                                  key->pairwise, key->peer, RT_TRUE);
    if (!entry)
    {
        return -RT_EFULL;
    }
    if (entry->valid)
    {
        hardware_index = entry->hardware_index;
        result = aic_execute(context, AIC_MM_KEY_DEL_REQ,
                             AIC_MM_KEY_DEL_CFM, &hardware_index,
                             sizeof(hardware_index), RT_NULL, 0, RT_NULL);
        if (result != RT_EOK)
        {
            return result;
        }
        rt_memset(entry, 0, sizeof(*entry));
    }
    rt_memset(request, 0, sizeof(request));
    request[0] = key->index;
    request[1] = key->pairwise ? station_index :
                                 AIC8800_INVALID_INDEX;
    request[4] = key->key_length;
    rt_memcpy(request + 8, key->key, key->key_length);
    request[40] = (rt_uint8_t)cipher;
    request[41] = vif_index;
    request[43] = key->pairwise;
    rt_memset(&confirmation, 0, sizeof(confirmation));
    result = aic_execute(context, AIC_MM_KEY_ADD_REQ, AIC_MM_KEY_ADD_CFM,
                         request, sizeof(request), &confirmation,
                         sizeof(confirmation), &confirmation_length);
    if (result != RT_EOK)
    {
        return result;
    }
    if (confirmation_length < 2U)
    {
        return -RT_EIO;
    }
    result = aic_confirmation_status(
        (const rt_uint8_t *)&confirmation, confirmation_length);
    if (result == RT_EOK && confirmation_length >= 2)
    {
        entry->valid = RT_TRUE;
        entry->iftype = vif->iftype;
        entry->index = key->index;
        entry->pairwise = key->pairwise;
        rt_memcpy(entry->peer, key->peer, 6);
        entry->hardware_index = confirmation.hardware_key_index;
    }
    return result;
}

static rt_err_t aic_delete_key(struct rt_wlan_offload_vif *vif,
                               rt_uint32_t request_id, rt_uint8_t index,
                               rt_bool_t pairwise,
                               const rt_uint8_t peer[6])
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_uint8_t hardware_index;
    struct aic8800_hardware_key *entry;
    rt_err_t result;

    (void)request_id;
    if (!context || !vif || index >= AIC8800_KEY_COUNT)
    {
        return -RT_EINVAL;
    }
    /* Vendor Linux treats deleting an already absent key as a no-op.  WPA
     * teardown commonly repeats this operation after a disconnect. */
    entry = aic_find_hardware_key(context, vif->iftype, index, pairwise,
                                  peer, RT_FALSE);
    if (!entry)
    {
        return RT_EOK;
    }
    if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
        !context->station_enabled : !context->ap_enabled)
    {
        rt_memset(entry, 0, sizeof(*entry));
        return RT_EOK;
    }
    hardware_index = entry->hardware_index;
    result = aic_execute(context, AIC_MM_KEY_DEL_REQ, AIC_MM_KEY_DEL_CFM,
                         &hardware_index, sizeof(hardware_index),
                         RT_NULL, 0, RT_NULL);
    if (result == RT_EOK)
    {
        rt_memset(entry, 0, sizeof(*entry));
    }
    return result;
}

static rt_err_t aic_set_default_key(struct rt_wlan_offload_vif *vif,
                                    rt_uint32_t request_id, rt_uint8_t index,
                                    rt_bool_t unicast, rt_bool_t multicast)
{
    struct aic8800_context *context = aic_context_from_vif(vif);

    (void)request_id;
    if (!context || !vif ||
        (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
         !context->station_enabled : !context->ap_enabled) ||
        index >= AIC8800_KEY_COUNT || (!unicast && !multicast))
    {
        return -RT_EINVAL;
    }
    return RT_EOK;
}

static struct aic8800_ap_station *aic_find_ap_station(
    struct aic8800_context *context, const rt_uint8_t address[6])
{
    rt_size_t index;

    if (!context || !address)
    {
        return RT_NULL;
    }
    for (index = 0; index < AIC8800_AP_STATION_COUNT; index++)
    {
        if (context->ap_stations[index].valid &&
            rt_memcmp(context->ap_stations[index].address, address, 6) == 0)
        {
            return &context->ap_stations[index];
        }
    }
    return RT_NULL;
}

static rt_uint8_t aic_tx_access_category(rt_uint8_t tid)
{
    static const rt_uint8_t categories[8] =
    {
        AIC_TX_ACCESS_CATEGORY_BEST_EFFORT,
        AIC_TX_ACCESS_CATEGORY_BACKGROUND,
        AIC_TX_ACCESS_CATEGORY_BACKGROUND,
        AIC_TX_ACCESS_CATEGORY_BEST_EFFORT,
        AIC_TX_ACCESS_CATEGORY_VIDEO,
        AIC_TX_ACCESS_CATEGORY_VIDEO,
        AIC_TX_ACCESS_CATEGORY_VOICE,
        AIC_TX_ACCESS_CATEGORY_VOICE,
    };

    return categories[tid & 7U];
}

static rt_uint8_t aic_classify_8021d(const rt_uint8_t *frame,
                                     rt_size_t length)
{
    rt_uint16_t ethertype;
    rt_size_t network_offset = 14U;

    if (!frame || length < network_offset)
    {
        return AIC_TX_TID_BEST_EFFORT;
    }
    ethertype = ((rt_uint16_t)frame[12] << 8) | frame[13];
    if ((ethertype == AIC_ETHERTYPE_VLAN ||
         ethertype == AIC_ETHERTYPE_VLAN_PROVIDER) && length >= 18U)
    {
        return (frame[14] >> 5) & 7U;
    }
    if (ethertype == AIC_ETHERTYPE_IPV4 && length >= network_offset + 2U)
    {
        return (frame[network_offset + 1U] >> 5) & 7U;
    }
    if (ethertype == AIC_ETHERTYPE_IPV6 && length >= network_offset + 2U)
    {
        rt_uint8_t traffic_class =
            (rt_uint8_t)((frame[network_offset] << 4) |
                         (frame[network_offset + 1U] >> 4));

        return (traffic_class >> 5) & 7U;
    }
    return AIC_TX_TID_BEST_EFFORT;
}

static rt_uint16_t aic_frame_get_be16(const rt_uint8_t *data)
{
    return ((rt_uint16_t)data[0] << 8) | data[1];
}

/* The reference Linux driver can keep tens of bulk OUT requests in flight.
 * This port deliberately uses a small request pool to coexist with Bluetooth
 * and the fixed DWC2 request pool, so bootstrap traffic must not sit behind a
 * saturated data queue.  Only packets which are safe to move ahead of normal
 * stream data are selected here. */
static rt_bool_t aic_is_latency_control_frame(const rt_uint8_t *frame,
                                              rt_size_t length)
{
    const rt_uint8_t *network;
    const rt_uint8_t *transport;
    rt_uint16_t ethertype;
    rt_uint16_t source_port;
    rt_uint16_t destination_port;
    rt_size_t network_offset = 14U;
    rt_size_t transport_offset;
    rt_size_t ip_header_length;
    rt_uint8_t protocol;

    if (!frame || length < network_offset)
    {
        return RT_FALSE;
    }
    ethertype = aic_frame_get_be16(frame + 12U);
    for (rt_size_t tags = 0;
         tags < 2U && (ethertype == AIC_ETHERTYPE_VLAN ||
                      ethertype == AIC_ETHERTYPE_VLAN_PROVIDER);
         tags++)
    {
        if (length < network_offset + 4U)
        {
            return RT_FALSE;
        }
        ethertype = aic_frame_get_be16(frame + network_offset + 2U);
        network_offset += 4U;
    }
    if (ethertype == AIC_ETHERTYPE_EAPOL || ethertype == AIC_ETHERTYPE_ARP)
    {
        return RT_TRUE;
    }
    if (ethertype == AIC_ETHERTYPE_IPV4)
    {
        if (length < network_offset + 20U)
        {
            return RT_FALSE;
        }
        network = frame + network_offset;
        ip_header_length = (rt_size_t)(network[0] & 0x0fU) * 4U;
        if ((network[0] >> 4) != 4U || ip_header_length < 20U ||
            length < network_offset + ip_header_length ||
            (aic_frame_get_be16(network + 6U) & 0x1fffU) != 0U)
        {
            return RT_FALSE;
        }
        protocol = network[9];
        transport_offset = network_offset + ip_header_length;
    }
    else if (ethertype == AIC_ETHERTYPE_IPV6)
    {
        if (length < network_offset + 40U ||
            (frame[network_offset] >> 4) != 6U)
        {
            return RT_FALSE;
        }
        network = frame + network_offset;
        protocol = network[6];
        transport_offset = network_offset + 40U;
        if (protocol == AIC_IPV6_NEXT_HEADER_ICMP &&
            length > transport_offset)
        {
            rt_uint8_t type = frame[transport_offset];

            return type >= 133U && type <= 136U;
        }
    }
    else
    {
        return RT_FALSE;
    }

    transport = frame + transport_offset;
    if (protocol == AIC_IP_PROTOCOL_TCP &&
        length >= transport_offset + 20U)
    {
        return (transport[13] & (AIC_TCP_FLAG_SYN | AIC_TCP_FLAG_RST)) != 0U;
    }
    if (protocol != AIC_IP_PROTOCOL_UDP ||
        length < transport_offset + 8U)
    {
        return RT_FALSE;
    }
    source_port = aic_frame_get_be16(transport);
    destination_port = aic_frame_get_be16(transport + 2U);
    if ((source_port == AIC_DHCP_SERVER_PORT ||
         source_port == AIC_DHCP_CLIENT_PORT) &&
        (destination_port == AIC_DHCP_SERVER_PORT ||
         destination_port == AIC_DHCP_CLIENT_PORT))
    {
        return RT_TRUE;
    }
    return (source_port == AIC_DHCPV6_CLIENT_PORT ||
            source_port == AIC_DHCPV6_SERVER_PORT) &&
           (destination_port == AIC_DHCPV6_CLIENT_PORT ||
            destination_port == AIC_DHCPV6_SERVER_PORT);
}

static rt_uint8_t aic_downgrade_tid(rt_uint8_t tid, rt_uint8_t acm)
{
    static const rt_uint8_t downgrade_tid[4] = {2U, 3U, 5U, 7U};
    rt_uint8_t category = aic_tx_access_category(tid);

    while (acm & (1U << category))
    {
        if (category == AIC_TX_ACCESS_CATEGORY_BACKGROUND)
        {
            return 1U;
        }
        category--;
        tid = downgrade_tid[category];
    }
    return tid;
}

static rt_err_t aic_transmit_frame(struct aic8800_context *context,
                                   struct rt_wlan_offload_vif *vif,
                                   const rt_uint8_t *data, rt_size_t length,
                                   rt_bool_t filter_tcp_ack,
                                   rt_bool_t management,
                                   rt_uint32_t status_descriptor)
{
    rt_size_t payload_length;
    rt_size_t raw_length;
    rt_size_t total_length;
    rt_uint8_t *frame;
    struct aic_wire_tx_host_descriptor *descriptor;
    rt_err_t lock_result;
    rt_err_t result;
    rt_uint8_t station_index = AIC8800_INVALID_INDEX;
    rt_uint16_t station_generation = 0;
    rt_bool_t group_addressed = RT_FALSE;
    rt_bool_t accounted = RT_FALSE;
    rt_bool_t power_save = RT_FALSE;
    rt_bool_t peer_qos = RT_FALSE;
    rt_uint8_t peer_acm = 0;
    rt_uint16_t peer_uapsd_tids = 0;
    rt_uint8_t tid = AIC_TX_TID_NON_QOS;
    rt_uint8_t ps_id = AIC_PS_ID_LEGACY;
    rt_bool_t high_priority;
    struct aic8800_tx_metadata accounting_metadata;
#ifdef AIC8800_WIFI_DEBUG_STATS
    rt_bool_t icmp = RT_FALSE;
    rt_uint16_t ethertype;
#endif
    struct aic8800_ap_station *station = RT_NULL;

    rt_memset(&accounting_metadata, 0, sizeof(accounting_metadata));
    accounting_metadata.station_index = AIC8800_INVALID_INDEX;

    if (!context || !vif || !data || !length ||
        (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
         !context->station_enabled : !context->ap_started))
    {
        return -RT_EINVAL;
    }
    if ((management && length < 24) || (!management && length < 14) ||
        length > AIC8800_ETHERNET_FRAME_MAX)
    {
        return -RT_EINVAL;
    }
#ifdef AIC8800_WIFI_TCP_ACK_FILTER
    if (!management && filter_tcp_ack &&
        aic_tcp_ack_filter(context, vif, data, length))
    {
        return RT_EOK;
    }
#else
    (void)filter_tcp_ack;
#endif
#ifdef AIC8800_WIFI_DEBUG_STATS
    if (!management)
    {
        ethertype = ((rt_uint16_t)data[12] << 8) | data[13];
        icmp = ethertype == 0x0800U && length >= 24U && data[23] == 1U;
    }
#endif
    payload_length = management ? length : length - 14U;
    raw_length = AIC8800_USB_HEADER_SIZE + AIC_TX_DESCRIPTOR_SIZE +
                 payload_length;
    /* The Linux AIC USB driver submits ordinary data records at their exact
     * length and pads records that request a firmware TX confirmation. SDIO
     * performs its required four-byte padding in
     * aic8800_sdio_prepare_record(), after this transport-neutral frame has
     * been built. Advertising padding as USB payload changes the firmware
     * record boundary for non-word-sized Ethernet packets. */
    total_length = status_descriptor ? aic_align4(raw_length) : raw_length;
    if (total_length > context->bus.max_tx_size ||
        total_length > AIC_USB_LENGTH_MASK ||
        total_length > sizeof(context->tx_frame))
    {
        return -RT_EFULL;
    }
    if (!context->frame_mutex_initialized)
    {
        return -RT_EIO;
    }

    /* Resolve the peer before taking frame_mutex.  The station generation is
     * rechecked under that mutex before the record is queued. */
    if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION)
    {
        station_index = context->ap_station_index;
        /* Match the vendor fullmac path: QoS peers use best-effort TID 0;
         * non-QoS peers use the legacy station queue. Firmware owns block
         * acknowledgement state for QoS traffic. */
        if (!management)
        {
            tid = context->station_qos ? AIC_TX_TID_BEST_EFFORT :
                                         AIC_TX_TID_NON_QOS;
        }
    }
    else
    {
        const rt_uint8_t *destination = management ? data + 4 : data;

        station = aic_find_ap_station(context, destination);
        if (station)
        {
            station_index = station->firmware_index;
            peer_qos = station->qos;
            peer_acm = station->acm;
            peer_uapsd_tids = station->uapsd_tids;
            if (!aic_station_state_snapshot(
                    context, station_index, &station_generation, &power_save))
            {
                station_index = AIC8800_INVALID_INDEX;
            }
        }
        else if (!management && (destination[0] & 1U))
        {
            /* Group-addressed traffic goes to the VIF's BC/MC pseudo-station,
             * which is not a QoS peer: it has no block-ack agreement and no
             * per-TID queue, so the frame has to be described as non-QoS.
             * Vendor Linux keeps the BC/MC queue on the best-effort hardware
             * queue as well - it defines a separate broadcast queue but never
             * selects it, so a frame handed over on that queue is accepted by
             * the firmware and then never transmitted. */
            station_index = context->ap_broadcast_station_index;
            group_addressed = RT_TRUE;
        }
    }

    /* Management frames may legitimately carry the "no peer" index: the
     * firmware transmits them off the VIF.  A data frame cannot - the firmware
     * indexes its station table with this value, so 0xff walks off the end of
     * the array and wedges the transmit path, after which the device stops
     * draining bulk OUT entirely.  This is the state right after a SoftAP
     * client leaves: aic_del_station() has already dropped the station entry
     * while lwIP still holds the departed MAC in its ARP cache, so NAT keeps
     * forwarding inbound packets to a peer the firmware no longer knows. */
    if (!management && station_index == AIC8800_INVALID_INDEX)
    {
        context->tx_no_station_count++;
        if (aic8800_log_throttle(context->tx_no_station_count))
        {
            LOG_D("dropping frame for unknown peer "
                  "%02x:%02x:%02x:%02x:%02x:%02x (drops=%u)",
                  data[0], data[1], data[2], data[3], data[4], data[5],
                  (unsigned int)context->tx_no_station_count);
        }
        /* Report success: the frame is discarded exactly like an Ethernet
         * driver drops a packet for an unreachable peer, and this return value
         * is handed straight back to lwIP by wlan_offload_wlan_transmit(). */
        return RT_EOK;
    }

    if (!management && vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP &&
        !group_addressed && peer_qos)
    {
        tid = aic_downgrade_tid(aic_classify_8021d(data, length), peer_acm);
        if (peer_uapsd_tids & (1U << tid))
        {
            ps_id = AIC_PS_ID_UAPSD;
        }
    }

    /* The transport worker owns channel-context backpressure.  Queue new
     * records while this VIF is off-channel so lwIP does not discard them. */
    if (!management && !aic_channel_context_active(context, vif->iftype))
    {
        context->tx_off_channel_count++;
        if (aic8800_log_throttle(context->tx_off_channel_count))
        {
            LOG_D("queueing %s frame for inactive channel context %u "
                  "(active=%u queued=%u)",
                  vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
                      "station" : "AP",
                  (unsigned int)aic_vif_channel_index(context, vif->iftype),
                  (unsigned int)context->active_channel_index,
                  (unsigned int)context->tx_off_channel_count);
        }
    }

    /* Account frames outstanding for an associated SoftAP client.  The
     * transport worker applies bounded backpressure outside this direct lwIP
     * transmit path. */
    if (!management && vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP &&
        !group_addressed)
    {
        result = aic_tx_pending_acquire(
            context, station_index, station_generation, ps_id);
        if (result != RT_EOK)
        {
            /* A station deleted or reused during transmit is a link-layer
             * drop. Returning an error makes lwIP retry the same frame even
             * though its destination is no longer valid. */
            return RT_EOK;
        }
        accounted = RT_TRUE;
        accounting_metadata.accounted = RT_TRUE;
        accounting_metadata.host_buffered = RT_TRUE;
        accounting_metadata.station_index = station_index;
        accounting_metadata.station_generation = station_generation;
        accounting_metadata.ps_id = ps_id;
        if (power_save)
        {
            context->tx_power_save_buffered_count++;
            aic_set_traffic_status(context, station_index, ps_id, RT_TRUE);
        }
    }

    lock_result = rt_mutex_take(context->frame_mutex, RT_WAITING_FOREVER);
    if (lock_result != RT_EOK)
    {
        if (accounted)
        {
            aic8800_core_tx_complete(context, &accounting_metadata);
        }
        return lock_result;
    }

    /* Station deletion is serialized with this final check and transport
     * enqueue.  Channel indications can still arrive while a record waits in
     * the transport, so both workers repeat the channel-context check. */
    if (accounted)
    {
        const rt_uint8_t *destination = management ? data + 4 : data;

        station = aic_find_ap_station(context, destination);
        if (!station || station->firmware_index != station_index ||
            !aic_station_state_matches(
                context, station_index, station_generation, &power_save))
        {
            aic8800_core_tx_complete(context, &accounting_metadata);
            rt_mutex_release(context->frame_mutex);
            return RT_EOK;
        }
        if (power_save)
        {
            aic_set_traffic_status(context, station_index, ps_id, RT_TRUE);
        }
    }
    else if (!management &&
             ((vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION &&
               (!context->station_connected ||
                station_index != context->ap_station_index)) ||
              (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP &&
               station_index != context->ap_broadcast_station_index)))
    {
        rt_mutex_release(context->frame_mutex);
        return RT_EOK;
    }
    frame = context->tx_frame;
#ifdef AIC8800_WIFI_DEBUG_STATS
    if (icmp)
    {
        aic_validate_icmp_frame(context, data, length);
    }
#endif
    rt_memset(frame, 0, AIC8800_USB_HEADER_SIZE + AIC_TX_DESCRIPTOR_SIZE);
    aic_put_le16(frame, (rt_uint16_t)total_length);
    frame[1] &= 0x0fU;
    frame[2] = AIC_USB_TYPE_DATA_TX;
    descriptor = (struct aic_wire_tx_host_descriptor *)(
        frame + AIC8800_USB_HEADER_SIZE);
    aic_put_le16(&descriptor->packet_length,
                 (rt_uint16_t)payload_length);
    aic_put_le32(&descriptor->status_descriptor, status_descriptor);
    descriptor->access_category = management ?
        AIC_TX_ACCESS_CATEGORY_VOICE :
        (tid == AIC_TX_TID_NON_QOS ? AIC_TX_ACCESS_CATEGORY_BEST_EFFORT :
                                     aic_tx_access_category(tid));
    descriptor->tid = tid;
    descriptor->vif_index =
        vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
        context->vif_index : context->ap_vif_index;
    descriptor->station_index = station_index;
    if (management)
    {
        aic_put_le16(&descriptor->flags, AIC_TX_FLAG_MANAGEMENT);
        if (length >= 16)
        {
            rt_memcpy(descriptor->destination.array, data + 4, 6);
            rt_memcpy(descriptor->source.array, data + 10, 6);
        }
        rt_memcpy(frame + AIC8800_USB_HEADER_SIZE + AIC_TX_DESCRIPTOR_SIZE,
                  data, length);
    }
    else
    {
        rt_memcpy(descriptor->destination.array, data, 6);
        rt_memcpy(descriptor->source.array, data + 6, 6);
        rt_memcpy(&descriptor->ethertype, data + 12, 2);
        rt_memcpy(frame + AIC8800_USB_HEADER_SIZE + AIC_TX_DESCRIPTOR_SIZE,
                  data + 14, payload_length);
    }
    if (total_length > raw_length)
    {
        rt_memset(frame + raw_length, 0, total_length - raw_length);
    }
    high_priority = management ||
                    (!management &&
                     aic_is_latency_control_frame(data, length));
    if (!management)
    {
        AIC8800_STAT(context->ethernet_tx_count++);
#ifdef AIC8800_WIFI_DEBUG_STATS
        if (ethertype == 0x0806U)
        {
            AIC8800_STAT(context->arp_tx_count++);
        }
        else if (icmp)
        {
            AIC8800_STAT(context->icmp_tx_count++);
        }
#endif
    }
    result = rt_wlan_offload_bus_transmit_priority(
        &context->bus, high_priority ? RT_WLAN_OFFLOAD_BUS_PRIORITY_HIGH :
                                      RT_WLAN_OFFLOAD_BUS_PRIORITY_NORMAL,
        frame, total_length);
    if (!management && result != RT_EOK)
    {
        AIC8800_STAT(context->ethernet_tx_error_count++);
        /* The frame never reached the transport, so no completion will arrive
         * to release its slot in the station's window. */
        if (accounted)
        {
            aic8800_core_tx_complete(context, &accounting_metadata);
        }
    }
    rt_mutex_release(context->frame_mutex);
    return result;
}

static rt_err_t aic_transmit(struct rt_wlan_offload_vif *vif,
                             const void *data, int length)
{
    return length > 0 ?
           aic_transmit_frame(aic_context_from_vif(vif), vif, data, length,
                              RT_TRUE, RT_FALSE, 0) : -RT_EINVAL;
}

static rt_err_t aic_transmit_raw(struct rt_wlan_offload_vif *vif,
                                 const void *data, int length)
{
    return length > 0 ?
           aic_transmit_frame(aic_context_from_vif(vif), vif, data, length,
                              RT_FALSE, RT_TRUE, 0) : -RT_EINVAL;
}

static rt_err_t aic_transmit_mgmt(struct rt_wlan_offload_vif *vif,
                                  const struct rt_wlan_offload_mgmt_frame *request)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic8800_mgmt_confirmation confirmation;
    rt_uint32_t index;
    rt_err_t result;

    if (!context || !request || !request->data || request->length < 24U ||
        request->length > AIC8800_ETHERNET_FRAME_MAX)
    {
        return -RT_EINVAL;
    }
    if (request->off_channel)
    {
        return -RT_ENOSYS;
    }
    if (request->channel.primary_frequency_mhz)
    {
        const struct rt_wlan_offload_channel_definition *active = RT_NULL;

        if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP && context->ap_started)
        {
            active = &context->ap_channel;
        }
        else if (context->current_channel_valid)
        {
            active = &context->current_channel;
        }
        else if (context->auth.valid)
        {
            active = &context->auth.channel;
        }
        if (!active ||
            active->band != request->channel.band ||
            active->primary_frequency_mhz !=
                request->channel.primary_frequency_mhz)
        {
            return -RT_EBUSY;
        }
    }
    result = aic_allocate_mgmt_confirmation(context, vif->iftype, request,
                                              &index);
    if (result != RT_EOK)
    {
        return result;
    }
    result = aic_transmit_frame(context, vif, request->data, request->length,
                                RT_FALSE, RT_TRUE,
                                AIC_TX_STATUS_DESCRIPTOR_REQUEST | index);
    if (result != RT_EOK)
    {
        rt_memset(&confirmation, 0, sizeof(confirmation));
        if (aic_take_mgmt_confirmation(context, index, &confirmation))
        {
            aic_report_mgmt_confirmation(context, &confirmation,
                                         result, RT_FALSE);
        }
    }
    return result;
}

static const char *aic_rate_format_name(rt_uint32_t format)
{
    static const char *const names[] = {
        "legacy", "legacy-dup", "HT-MF", "HT-GF",
        "VHT", "HE-SU", "HE-MU", "HE-ER"
    };

    return format < sizeof(names) / sizeof(names[0]) ? names[format] :
                                                        "unknown";
}

static void aic_log_station_rate(
    const struct aic_wire_mm_get_sta_info_cfm *response)
{
    rt_uint32_t rate = response->rate_info;
    rt_uint32_t format = (rate >> 11) & 0x07U;
    rt_uint32_t bandwidth = 20U << ((rate >> 7) & 0x03U);
    rt_uint32_t gi = (rate >> 9) & 0x03U;
    rt_uint32_t mcs;
    rt_uint32_t nss;

    if (format == 2U || format == 3U)
    {
        mcs = rate & 0x07U;
        nss = ((rate >> 3) & 0x03U) + 1U;
    }
    else if (format >= 4U)
    {
        mcs = rate & 0x0fU;
        nss = ((rate >> 4) & 0x07U) + 1U;
    }
    else
    {
        mcs = rate & 0x7fU;
        nss = 1U;
    }
    LOG_I("link: RSSI=%d TX=%s NSS%u MCS/index=%u bandwidth=%u MHz GI=%u failed=%u raw=0x%08x",
          response->rssi, aic_rate_format_name(format),
          (unsigned int)nss, (unsigned int)mcs,
          (unsigned int)bandwidth, (unsigned int)gi,
          (unsigned int)response->tx_failed, (unsigned int)rate);
}

#ifdef AIC8800_WIFI_DEBUG_STATS
static void aic_stat_print_rate(rt_uint16_t rate_config)
{
    rt_uint32_t format = (rate_config >> 11) & 0x07U;
    rt_uint32_t bandwidth = 20U << ((rate_config >> 7) & 0x03U);
    rt_uint32_t gi = (rate_config >> 9) & 0x03U;
    rt_uint32_t mcs;
    rt_uint32_t nss;

    if (format == 2U || format == 3U)
    {
        mcs = rate_config & 0x07U;
        nss = ((rate_config >> 3) & 0x03U) + 1U;
    }
    else if (format >= 4U)
    {
        mcs = rate_config & 0x0fU;
        nss = ((rate_config >> 4) & 0x07U) + 1U;
    }
    else
    {
        mcs = rate_config & 0x7fU;
        nss = 1U;
    }
    rt_kprintf("%s NSS%u MCS/index=%u BW=%u GI=%u",
               aic_rate_format_name(format), (unsigned int)nss,
               (unsigned int)mcs, (unsigned int)bandwidth,
               (unsigned int)gi);
}

static void aic_stat_print_rc(struct aic8800_context *context)
{
    struct aic_wire_me_rc_stats_req request;
    struct aic_wire_me_rc_stats_cfm response;
    rt_size_t response_length = 0;
    rt_uint16_t sample_count;
    rt_err_t result;

    if (!context->attached || !context->station_connected ||
        context->ap_station_index == AIC8800_INVALID_INDEX)
    {
        return;
    }
    request.station_index = context->ap_station_index;
    rt_memset(&response, 0, sizeof(response));
    result = aic_execute(context, AIC_ME_RC_STATS_REQ,
                         AIC_ME_RC_STATS_CFM, &request, sizeof(request),
                         &response, sizeof(response), &response_length);
    if (result != RT_EOK || response_length < sizeof(response))
    {
        rt_kprintf("RC: unavailable result=%d length=%u\n", result,
                   (unsigned int)response_length);
        return;
    }
    sample_count = aic_get_le16(&response.sample_count);
    if (sample_count > AIC_WIRE_RC_SAMPLE_COUNT)
    {
        sample_count = AIC_WIRE_RC_SAMPLE_COUNT;
    }
    rt_kprintf("RC: samples=%u MPDUs=%u AMPDUs=%u average=%u.%u "
               "retry=%u wait=%u chain=%u/%u/%u/%u\n",
               (unsigned int)sample_count,
               (unsigned int)aic_get_le16(&response.ampdu_length),
               (unsigned int)aic_get_le16(&response.ampdu_packets),
               (unsigned int)(aic_get_le32(&response.average_ampdu_length) >>
                              16),
               (unsigned int)(((aic_get_le32(
                                    &response.average_ampdu_length) * 10U) >>
                               16) % 10U),
               (unsigned int)response.software_retry_step,
               (unsigned int)response.sample_wait,
               (unsigned int)aic_get_le16(&response.retry_step_index[0]),
               (unsigned int)aic_get_le16(&response.retry_step_index[1]),
               (unsigned int)aic_get_le16(&response.retry_step_index[2]),
               (unsigned int)aic_get_le16(&response.retry_step_index[3]));
    for (rt_uint16_t index = 0; index < sample_count; index++)
    {
        const struct aic_wire_rc_rate_stats *rate = &response.rates[index];
        rt_uint32_t probability =
            (((rt_uint32_t)aic_get_le16(&rate->probability) * 1000U) >> 16) +
            1U;
        rt_uint32_t throughput =
            aic_get_le32(&response.throughput[index]) / 10U;

        rt_kprintf("RC[%u]%c%c%c: ", (unsigned int)index,
                   aic_get_le16(&response.retry_step_index[0]) == index ?
                       'T' : ' ',
                   aic_get_le16(&response.retry_step_index[1]) == index ?
                       't' : ' ',
                   aic_get_le16(&response.retry_step_index[2]) == index ?
                       'P' : ' ');
        aic_stat_print_rate(aic_get_le16(&rate->rate_config));
        rt_kprintf(" tp=%u.%u prob=%u.%u%% ok=%u/%u skipped=%u\n",
                   (unsigned int)(throughput / 10U),
                   (unsigned int)(throughput % 10U),
                   (unsigned int)(probability / 10U),
                   (unsigned int)(probability % 10U),
                   (unsigned int)aic_get_le16(&rate->success),
                   (unsigned int)aic_get_le16(&rate->attempts),
                   (unsigned int)rate->detail.sample.sample_skipped);
    }
}

static void aic_stat_print_common(const char *transport,
                                  struct aic8800_context *context)
{
    int station_credit = -1;
    unsigned int station_pending = 0;

    if (context->ap_station_index < AIC8800_STATION_SLOTS)
    {
        station_credit = context->tx_credits[context->ap_station_index];
        station_pending = context->tx_pending[context->ap_station_index];
    }
    rt_kprintf("[%s] connected=%d attached=%d station=%d ap=%d "
               "vif=%u sta=%u port=%s\n",
               transport, context->transport_connected, context->attached,
               context->station_connected, context->ap_started,
               context->vif_index, context->ap_station_index,
               context->station_control_port_open ? "open" : "closed");
    rt_kprintf("TX state: pending=%u watermark-events=%u ps-buffered=%u credits=%d "
               "updates=%u no-station=%u off-channel=%u\n",
               station_pending,
               (unsigned int)context->tx_pending_watermark_count,
               (unsigned int)context->tx_power_save_buffered_count,
               station_credit, (unsigned int)context->tx_credit_update_count,
               (unsigned int)context->tx_no_station_count,
               (unsigned int)context->tx_off_channel_count);
    rt_kprintf("NET TX/RX: frames=%u/%u errors=%u/%u ARP=%u/%u ICMP=%u/%u\n",
               (unsigned int)context->ethernet_tx_count,
               (unsigned int)context->ethernet_rx_count,
               (unsigned int)context->ethernet_tx_error_count,
               (unsigned int)context->ethernet_rx_error_count,
               (unsigned int)context->arp_tx_count,
               (unsigned int)context->arp_rx_count,
               (unsigned int)context->icmp_tx_count,
               (unsigned int)context->icmp_rx_count);
    rt_kprintf("RX decode: data=%u qos=%u amsdu=%u subframes=%u "
               "no-llc=%u invalid=%u\n",
               (unsigned int)context->rx_data_record_count,
               (unsigned int)context->rx_qos_record_count,
               (unsigned int)context->rx_amsdu_record_count,
               (unsigned int)context->rx_amsdu_subframe_count,
               (unsigned int)context->rx_no_llc_count,
               (unsigned int)context->rx_invalid_data_count);
    rt_kprintf("RX reorder: pending=%u queued=%u delivered=%u timeout=%u "
               "duplicate=%u drops=%u\n",
               (unsigned int)context->rx_reorder_pending,
               (unsigned int)context->rx_reorder_queued,
               (unsigned int)context->rx_reorder_delivered,
               (unsigned int)context->rx_reorder_timeouts,
               (unsigned int)context->rx_reorder_duplicates,
               (unsigned int)context->rx_reorder_drops);
#ifdef AIC8800_WIFI_TCP_ACK_FILTER
    rt_kprintf("TCP ACK: suppressed=%u flushed=%u\n",
               (unsigned int)context->tcp_ack_suppressed,
               (unsigned int)context->tcp_ack_flushed);
#endif
}

#ifdef AIC8800_WIFI_TRANSPORT_USB
static void aic_stat_print_usb(struct aic8800_context *context)
{
    const struct aic8800_tx_worker *tx = &context->tx_worker;
    const struct aic8800_rx_worker *data = &context->data_worker;
    const struct aic8800_rx_worker *message = &context->message_worker;

    aic_stat_print_common("USB", context);
    rt_kprintf("USB TX queue: queued=%u depth=%u high=%u frames=%u "
               "aggregates=%u max=%u drops=%u defer=%u worker-drop=%u "
               "submit-busy=%u errors=%u\n",
               context->usb_tx_queue ?
                   (unsigned int)context->usb_tx_queue->entry : 0U,
               (unsigned int)AIC8800_WIFI_USB_TX_QUEUE_DEPTH,
               (unsigned int)context->usb_tx_queue_high_water,
               (unsigned int)context->usb_tx_frame_count,
               (unsigned int)context->usb_tx_aggregate_count,
               (unsigned int)context->usb_tx_max_aggregate,
               (unsigned int)context->usb_tx_queue_drop_count,
               (unsigned int)context->usb_tx_record_defer_count,
               (unsigned int)context->usb_tx_record_drop_count,
               (unsigned int)context->usb_tx_submit_busy_count,
               (unsigned int)context->usb_tx_error_count);
    rt_kprintf("USB TX URB: active=%d slots=%u pending=%u complete=%u "
               "bytes=%llu waits=%u timeouts=%u watchdog=%u reclaims=%u "
               "recoveries=%u max-burst=%u errors=%u last=%d\n",
               tx->active, (unsigned int)tx->slot_count,
               (unsigned int)tx->pending,
               (unsigned int)tx->completion_count,
               (unsigned long long)tx->byte_count,
               (unsigned int)tx->wait_count,
               (unsigned int)tx->timeout_count,
               (unsigned int)tx->watchdog_count,
               (unsigned int)tx->reclaim_count,
               (unsigned int)tx->recovery_count,
               (unsigned int)tx->max_burst_count,
               (unsigned int)tx->error_count, tx->last_error);
    rt_kprintf("USB RX data/msg: complete=%u/%u errors=%u/%u retries=%u/%u "
               "recoveries=%u/%u backlog-high=%u/%u overflow=%d/%d\n",
               (unsigned int)data->completion_count,
               (unsigned int)message->completion_count,
               (unsigned int)data->error_count,
               (unsigned int)message->error_count,
               (unsigned int)data->retry_count,
               (unsigned int)message->retry_count,
               (unsigned int)data->recovery_count,
               (unsigned int)message->recovery_count,
               (unsigned int)data->queue_high_water,
               (unsigned int)message->queue_high_water,
               data->queue_overflow, message->queue_overflow);
    rt_kprintf("USB RX dispatch: data max=%u us last=%u us "
               "msg max=%u us last=%u us\n",
               (unsigned int)data->dispatch_max_us,
               (unsigned int)data->dispatch_last_us,
               (unsigned int)message->dispatch_max_us,
               (unsigned int)message->dispatch_last_us);
    aic_stat_print_rc(context);
}
#endif

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
static void aic_stat_print_sdio(struct aic8800_context *context)
{
    unsigned int clock_khz = 0;
    unsigned int width = 0;
    unsigned int timing = 0;

    if (context->sdio_card)
    {
        clock_khz = context->sdio_card->host->io_cfg.clock / 1000U;
        width = context->sdio_card->host->io_cfg.bus_width ==
                    MMCSD_BUS_WIDTH_4 ? 4U : 1U;
        timing = context->sdio_card->host->io_cfg.timing;
    }
    aic_stat_print_common("SDIO", context);
    rt_kprintf("SDIO bus: active=%d clock=%u kHz width=%u timing=%u\n",
               context->sdio_active, clock_khz, width, timing);
    rt_kprintf("SDIO TX: transfers=%u frames=%u aggregates=%u max=%u "
               "queued=%u high=%u drops=%u errors=%u defer=%u "
               "worker-drop=%u\n",
               (unsigned int)context->sdio_tx_count,
               (unsigned int)context->sdio_tx_frame_count,
               (unsigned int)context->sdio_tx_aggregate_count,
               (unsigned int)context->sdio_tx_max_aggregate,
               context->sdio_tx_queue ?
                   (unsigned int)context->sdio_tx_queue->entry : 0U,
               (unsigned int)context->sdio_tx_queue_high_water,
               (unsigned int)context->sdio_tx_queue_drop_count,
               (unsigned int)context->sdio_tx_error_count,
               (unsigned int)context->sdio_tx_record_defer_count,
               (unsigned int)context->sdio_tx_record_drop_count);
    rt_kprintf("SDIO flow: credits=%u reads=%u waits=%u retries=%u "
               "max-retry=%u timeouts=%u fallbacks=%u wait-ms=%u "
               "granted=%u\n",
               (unsigned int)context->sdio_tx_available_credits,
               (unsigned int)context->sdio_tx_flow_read_count,
               (unsigned int)context->sdio_tx_credit_wait_count,
               (unsigned int)context->sdio_tx_credit_retry_count,
               (unsigned int)context->sdio_tx_credit_max_retries,
               (unsigned int)context->sdio_tx_credit_timeout_count,
               (unsigned int)context->sdio_tx_credit_fallback_count,
               (unsigned int)(context->sdio_tx_credit_wait_ticks * 1000U /
                              RT_TICK_PER_SECOND),
               (unsigned int)context->sdio_tx_credit_grant_frames);
    rt_kprintf("SDIO RX: transfers=%u I/O-errors=%u protocol-drops=%u "
               "queued=%u processed=%u drops=%u queue-high=%u\n",
               (unsigned int)context->sdio_rx_count,
               (unsigned int)context->sdio_error_count,
               (unsigned int)context->sdio_protocol_drop_count,
               (unsigned int)context->sdio_data_queued_count,
               (unsigned int)context->sdio_data_processed_count,
               (unsigned int)context->sdio_data_drop_count,
               (unsigned int)context->sdio_data_queue_high_water);
    aic_stat_print_rc(context);
}
#endif

static int aic8800_stat(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        rt_kprintf("usage: aic8800_stat\n");
        return -RT_EINVAL;
    }
#ifdef AIC8800_WIFI_TRANSPORT_USB
    aic_stat_print_usb(aic8800_usb_stat_context());
#endif
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    aic_stat_print_sdio(aic8800_sdio_stat_context());
#endif
    return 0;
}
MSH_CMD_EXPORT(aic8800_stat, show AIC8800 USB and SDIO statistics);
#endif /* AIC8800_WIFI_DEBUG_STATS */

static rt_err_t aic_get_rssi(struct rt_wlan_offload_vif *vif, int *rssi)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic_wire_mm_get_sta_info_compat_req request;
    struct aic_wire_mm_get_sta_info_cfm response;
    rt_size_t request_length;
    rt_size_t response_length = 0;
    rt_err_t result;

    if (!context || !rssi || !context->station_connected)
    {
        return -RT_EINVAL;
    }
    if (context->ap_station_index == AIC8800_INVALID_INDEX)
    {
        return -RT_EIO;
    }
    request.station_index = context->ap_station_index;
    request.pattern[0] = 's';
    request.pattern[1] = 't';
    request.pattern[2] = 'a';
    request_length =
        context->product_id == AIC8800_USB_PID_AIC8800D80X2 ||
        context->product_id == AIC8800_USB_PID_AIC8800D81X2 ||
        context->product_id == AIC8800_USB_PID_AIC8800D89X2 ?
            sizeof(struct aic_wire_mm_get_sta_info_req) : sizeof(request);
    rt_memset(&response, 0, sizeof(response));
    result = aic_execute(context, AIC_MM_GET_STA_INFO_REQ,
                         AIC_MM_GET_STA_INFO_CFM, &request, request_length,
                         &response, sizeof(response), &response_length);
    if (result != RT_EOK)
    {
        return result;
    }
    /* V5 returns the 32-byte statistics extension. Older compatible firmware
     * may stop after the original RSSI fields at byte 12. */
    if (response_length < 12U)
    {
        return -RT_EIO;
    }
    context->rssi = response.rssi;
    *rssi = context->rssi;
    aic_log_station_rate(&response);
    return RT_EOK;
}

static rt_err_t aic_set_rx_filter(struct aic8800_context *context,
                                  rt_bool_t promiscuous)
{
    struct aic_wire_mm_set_filter_req request;
    rt_err_t result;

    if (!context || (!context->station_enabled && !context->ap_enabled) ||
        !context->lmac_started)
    {
        return -RT_EBUSY;
    }
    request.filter = promiscuous ? AIC_RX_FILTER_PROMISCUOUS :
                                   AIC_RX_FILTER_DEFAULT;
    result = aic_execute(context, AIC_MM_SET_FILTER_REQ,
                         AIC_MM_SET_FILTER_CFM, &request, sizeof(request),
                         RT_NULL, 0, RT_NULL);
    if (result == RT_EOK)
    {
        context->promiscuous_enabled = promiscuous;
    }
    return result;
}

static rt_err_t aic_set_promiscuous(struct rt_wlan_offload_vif *vif,
                                    rt_bool_t enabled)
{
    return aic_set_rx_filter(aic_context_from_vif(vif), enabled);
}

static rt_err_t aic_set_filter(struct rt_wlan_offload_vif *vif,
                               struct rt_wlan_filter *filter)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_err_t result;

    if (!context || !filter)
    {
        return -RT_EINVAL;
    }
    if (!filter->enable)
    {
        result = rt_mutex_take(&context->radio.operation_lock,
                               RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            return result;
        }
        context->filter_enabled = RT_FALSE;
        context->filter_offset = 0;
        context->filter_length = 0;
        rt_memset(context->filter_mask, 0, sizeof(context->filter_mask));
        rt_memset(context->filter_pattern, 0,
                  sizeof(context->filter_pattern));
        rt_mutex_release(&context->radio.operation_lock);
        return RT_EOK;
    }
    if (filter->rule != RT_POSITIVE_MATCHING &&
        filter->rule != RT_NEGATIVE_MATCHING)
    {
        return -RT_EINVAL;
    }
    if (!filter->patt.mask_size ||
        filter->patt.mask_size > AIC8800_FILTER_PATTERN_MAX ||
        !filter->patt.mask || !filter->patt.pattern ||
        filter->patt.offset > AIC8800_ETHERNET_FRAME_MAX ||
        filter->patt.mask_size > AIC8800_ETHERNET_FRAME_MAX -
                                  filter->patt.offset)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&context->radio.operation_lock,
                           RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    context->filter_rule = filter->rule;
    context->filter_offset = filter->patt.offset;
    context->filter_length = filter->patt.mask_size;
    rt_memcpy(context->filter_mask, filter->patt.mask,
              filter->patt.mask_size);
    rt_memcpy(context->filter_pattern, filter->patt.pattern,
              filter->patt.mask_size);
    context->filter_enabled = RT_TRUE;
    rt_mutex_release(&context->radio.operation_lock);
    return RT_EOK;
}

static rt_err_t aic_set_mgmt_filter(struct rt_wlan_offload_vif *vif,
                                    rt_bool_t enabled)
{
    struct aic8800_context *context = aic_context_from_vif(vif);

    (void)enabled;
    if (!context ||
        (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
         !context->station_enabled : !context->ap_enabled) ||
        !context->lmac_started)
    {
        return -RT_EBUSY;
    }
    /* Management frames are already reported by the firmware's RX path;
     * the generic WLAN offload layer gates delivery to the registered callback. */
    return RT_EOK;
}

static rt_err_t aic_set_power_save(struct rt_wlan_offload_vif *vif, int level)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    struct aic_wire_me_set_ps_mode_req request;
    rt_err_t result;

    if (!context || level < 0 || level > 1)
    {
        return -RT_EINVAL;
    }
#ifndef AIC8800_WIFI_POWER_SAVE
    return -RT_ENOSYS;
#endif
    if (!context->firmware_capabilities_valid)
    {
        return -RT_EIO;
    }
    if (!(context->firmware_features & AIC_FW_CAP_PS))
    {
        return -RT_ENOSYS;
    }
    request.state = level ? AIC_ME_PS_MODE_ON : AIC_ME_PS_MODE_OFF;
    result = aic_execute(context, AIC_ME_SET_PS_MODE_REQ,
                         AIC_ME_SET_PS_MODE_CFM, &request, sizeof(request),
                         RT_NULL, 0, RT_NULL);
    if (result == RT_EOK)
    {
        context->power_save_level = level ? 1 : 0;
    }
    return result;
}

static rt_err_t aic_get_power_save(struct rt_wlan_offload_vif *vif, int *level)
{
    struct aic8800_context *context = aic_context_from_vif(vif);

    if (!context || !level)
    {
        return -RT_EINVAL;
    }
    if (!context->firmware_capabilities_valid)
    {
        return -RT_EIO;
    }
    if (!(context->firmware_features & AIC_FW_CAP_PS))
    {
        return -RT_ENOSYS;
    }
    *level = context->power_save_level ? 1 : 0;
    return RT_EOK;
}

static rt_err_t aic_set_channel(
    struct rt_wlan_offload_vif *vif,
    const struct rt_wlan_offload_channel_definition *channel)
{
    struct aic8800_context *context = aic_context_from_vif(vif);
    rt_uint16_t channel_number;

    if (!context || !vif || !channel ||
        (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
         !context->station_enabled : !context->ap_enabled) ||
        !context->lmac_started)
    {
        return -RT_EINVAL;
    }
    channel_number = channel->primary_channel ? channel->primary_channel :
                     aic_frequency_to_channel(channel->primary_frequency_mhz);
    if (!channel_number || !aic_channel_allowed(context, channel->band,
                                                channel_number))
    {
        return -RT_EINVAL;
    }
    if (context->station_connected || context->ap_started)
    {
        return -RT_EBUSY;
    }
    /* The AIC WLAN offload firmware exposes MM_SET_CHANNEL only for the
     * secondary RF chain.  The primary chain is selected by SM_CONNECT and
     * the Linux vendor driver deliberately treats a primary set-channel as a
     * no-op.  Do not send a request for an unsupported primary chain. */
    if (context->current_channel_valid &&
        aic_channel_definition_equal(&context->current_channel, channel))
    {
        return RT_EOK;
    }
    return -RT_ENOSYS;
}

static rt_err_t aic_get_channel(
    struct rt_wlan_offload_vif *vif,
    struct rt_wlan_offload_channel_definition *channel)
{
    struct aic8800_context *context = aic_context_from_vif(vif);

    if (!context || !channel)
    {
        return -RT_EINVAL;
    }
    if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP && context->ap_started)
    {
        *channel = context->ap_channel;
        return RT_EOK;
    }
    if (!context->current_channel_valid)
    {
        return -RT_EINVAL;
    }
    *channel = context->current_channel;
    return RT_EOK;
}

static rt_err_t aic_set_mac(struct rt_wlan_offload_vif *vif,
                            rt_uint8_t address[6])
{
    struct aic8800_context *context = aic_context_from_vif(vif);

    if (!context || !aic_mac_valid(address))
    {
        return -RT_EINVAL;
    }
    if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ?
        context->station_enabled : context->ap_enabled)
    {
        return -RT_EBUSY;
    }
    rt_memcpy(vif->address, address, 6);
    if (vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION)
    {
        rt_memcpy(context->address, address, 6);
    }
    return RT_EOK;
}

static rt_err_t aic_get_mac(struct rt_wlan_offload_vif *vif,
                            rt_uint8_t address[6])
{
    struct aic8800_context *context = aic_context_from_vif(vif);

    if (!context || !address)
    {
        return -RT_EINVAL;
    }
    rt_memcpy(address, vif->address, 6);
    return RT_EOK;
}

static rt_err_t aic_set_regulatory(struct rt_wlan_offload_radio *radio,
                                   rt_country_code_t country)
{
    struct aic8800_context *context = aic_context_from_radio(radio);
    struct aic8800_radio_config_state previous_config;
    rt_country_code_t previous_country;
    rt_err_t result;

    if (!context)
    {
        return -RT_EINVAL;
    }
    previous_config = context->radio_config;
    previous_country = context->country;
    result = aic8800_radio_set_regulatory(context, country);

    if (result == RT_EOK)
    {
        aic_refresh_channel_metadata(context);
        if (context->lmac_started)
        {
            result = aic_configure_channels(context);
            if (result != RT_EOK)
            {
                context->radio_config = previous_config;
                context->country = previous_country;
                aic_refresh_channel_metadata(context);
            }
        }
    }
    return result;
}

static rt_err_t aic_get_regulatory(struct rt_wlan_offload_radio *radio,
                                   rt_country_code_t *country)
{
    return aic8800_radio_get_regulatory(aic_context_from_radio(radio),
                                         country);
}

static const struct rt_wlan_offload_ops g_aic8800_wifi_ops = {
    .start = aic_wlan_offload_start,
    .stop = aic_wlan_offload_stop,
    .change_interface = aic_change_interface,
    .scan = aic_scan,
    .connect = aic_connect,
    .disconnect = aic_disconnect,
    .start_ap = aic_start_ap,
    .stop_ap = aic_stop_ap,
    .del_station = aic_del_station,
    .add_station = aic_add_station,
    .set_station_authorized = aic_set_station_authorized,
    .abort_scan = aic_abort_scan,
    .get_rssi = aic_get_rssi,
    .set_power_save = aic_set_power_save,
    .get_power_save = aic_get_power_save,
    .set_promiscuous = aic_set_promiscuous,
    .set_filter = aic_set_filter,
    .set_mgmt_filter = aic_set_mgmt_filter,
    .set_channel = aic_set_channel,
    .get_channel = aic_get_channel,
    .set_mac = aic_set_mac,
    .get_mac = aic_get_mac,
    .set_regulatory = aic_set_regulatory,
    .get_regulatory = aic_get_regulatory,
    .transmit = aic_transmit,
    .transmit_raw = aic_transmit_raw,
    .auth = aic_auth,
    .assoc = aic_assoc,
    .add_key = aic_add_key,
    .delete_key = aic_delete_key,
    .set_default_key = aic_set_default_key,
    .transmit_mgmt = aic_transmit_mgmt,
    .external_auth_response = aic_external_auth_response,
};

static rt_bool_t aic_device_supports_5ghz(
    const struct aic8800_context *context)
{
    if (!context)
    {
        return RT_FALSE;
    }
    if (context->firmware_capabilities_valid &&
        !context->firmware_supports_5ghz)
    {
        return RT_FALSE;
    }
    return context->product_id != AIC8800_USB_PID_AIC8801 &&
           context->product_id != AIC8800_USB_PID_AIC8800DC;
}

static rt_bool_t aic_channel_allowed(
    const struct aic8800_context *context, enum rt_wlan_offload_band_id band,
    rt_uint16_t channel)
{
    if (!context || (band == RT_WLAN_OFFLOAD_BAND_5GHZ &&
                     !aic_device_supports_5ghz(context)))
    {
        return RT_FALSE;
    }
    return aic8800_radio_channel_allowed(context, band, channel);
}

static rt_bool_t aic_device_supports_vht(
    const struct aic8800_context *context)
{
    rt_bool_t product_support;

    if (!context || !context->firmware_capabilities_valid)
    {
        return RT_FALSE;
    }
    switch (context->product_id)
    {
    case AIC8800_USB_PID_AIC8800D80:
    case AIC8800_USB_PID_AIC8800D81:
    case AIC8800_USB_PID_AIC8800D40:
    case AIC8800_USB_PID_AIC8800D41:
    case AIC8800_USB_PID_AIC8800D80X2:
    case AIC8800_USB_PID_AIC8800D81X2:
    case AIC8800_USB_PID_AIC8800D89X2:
        product_support = RT_TRUE;
        break;
    default:
        product_support = RT_FALSE;
        break;
    }
    if (!product_support || !(context->firmware_features & AIC_FW_CAP_VHT))
    {
        return RT_FALSE;
    }
    return RT_TRUE;
}

static void aic_refresh_channel_metadata(struct aic8800_context *context)
{
    rt_size_t index;

    /* VHT and HE are independent firmware feature bits.  Do not use a
     * combined static band description: some revisions advertise HE without
     * VHT, and the advertised capabilities must follow that negotiation. */
    context->band_2ghz = g_aic8800_band_2ghz;
    if (aic_device_supports_vht(context))
    {
        context->band_2ghz.phy_capabilities |= RT_WLAN_OFFLOAD_PHY_VHT;
    }
    if (context->firmware_capabilities_valid &&
        (context->firmware_features & AIC_FW_CAP_HE))
    {
        context->band_2ghz.phy_capabilities |= RT_WLAN_OFFLOAD_PHY_HE;
    }
    rt_memcpy(context->channels_2ghz, g_aic8800_channels_2ghz,
              sizeof(context->channels_2ghz));
    for (index = 0; index < sizeof(context->channels_2ghz) /
                           sizeof(context->channels_2ghz[0]); index++)
    {
        struct rt_wlan_offload_channel *channel = &context->channels_2ghz[index];

        if (!aic8800_radio_channel_allowed(context, channel->band,
                                           channel->number))
        {
            channel->flags |= RT_WLAN_OFFLOAD_CHANNEL_DISABLED;
            channel->max_power_dbm = 0;
        }
        else
        {
            if (context->country == RT_COUNTRY_CHINA &&
                channel->number >= 12 && channel->number <= 13)
            {
                channel->flags &= ~RT_WLAN_OFFLOAD_CHANNEL_NO_IR;
            }
            channel->max_power_dbm = aic8800_radio_channel_power(
                context, channel->band, channel->number,
                channel->max_power_dbm);
        }
    }
    context->band_2ghz.channels = context->channels_2ghz;
    context->band_2ghz.channel_count =
        sizeof(context->channels_2ghz) / sizeof(context->channels_2ghz[0]);

#ifdef AIC8800_WIFI_5GHZ
    context->band_5ghz = g_aic8800_band_5ghz;
    if (aic8800_radio_supports_80mhz(context))
    {
        context->band_5ghz.max_channel_width =
            RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80;
    }
    if (aic_device_supports_vht(context))
    {
        context->band_5ghz.phy_capabilities |= RT_WLAN_OFFLOAD_PHY_VHT;
    }
    if (context->firmware_capabilities_valid &&
        (context->firmware_features & AIC_FW_CAP_HE))
    {
        context->band_5ghz.phy_capabilities |= RT_WLAN_OFFLOAD_PHY_HE;
    }
    rt_memcpy(context->channels_5ghz, g_aic8800_channels_5ghz,
              sizeof(context->channels_5ghz));
    for (index = 0; index < sizeof(context->channels_5ghz) /
                           sizeof(context->channels_5ghz[0]); index++)
    {
        struct rt_wlan_offload_channel *channel = &context->channels_5ghz[index];

        if (!aic8800_radio_channel_allowed(context, channel->band,
                                           channel->number))
        {
            channel->flags |= RT_WLAN_OFFLOAD_CHANNEL_DISABLED;
            channel->max_power_dbm = 0;
        }
        else
        {
            channel->max_power_dbm = aic8800_radio_channel_power(
                context, channel->band, channel->number,
                channel->max_power_dbm);
        }
    }
    context->band_5ghz.channels = context->channels_5ghz;
    context->band_5ghz.channel_count =
        sizeof(context->channels_5ghz) / sizeof(context->channels_5ghz[0]);
    if (aic_device_supports_5ghz(context))
    {
        context->radio.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] = &context->band_5ghz;
    }
    else
    {
        context->radio.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] = RT_NULL;
    }
#endif
}

static void aic8800_core_cancel_work(struct aic8800_context *context)
{
    if (context->traffic_work_initialized)
    {
        rt_work_cancel_sync(&context->traffic_work);
        context->traffic_work_initialized = RT_FALSE;
        context->traffic_work_queued = RT_FALSE;
    }
    if (context->station_loss_work_initialized)
    {
        rt_work_cancel_sync(&context->station_loss_work);
        context->station_loss_work_initialized = RT_FALSE;
        context->station_loss_work_queued = RT_FALSE;
        rt_memset(context->station_loss, 0,
                  sizeof(context->station_loss));
    }
    if (context->scan_work_initialized)
    {
        rt_work_cancel_sync(&context->scan_work);
        context->scan_work_initialized = RT_FALSE;
        context->scan_work_queued = RT_FALSE;
    }
    if (context->ap_rechannel_work_initialized)
    {
        rt_work_cancel_sync(&context->ap_rechannel_work);
        context->ap_rechannel_work_initialized = RT_FALSE;
        context->ap_rechannel_work_queued = RT_FALSE;
    }
}

static void aic_command_gate_deinit(struct aic8800_context *context)
{
    if (context && context->command_gate_initialized)
    {
        rt_sem_detach(&context->command_gate);
        context->command_gate_initialized = RT_FALSE;
    }
}

static const char *aic8800_model_name(
    const struct aic8800_context *context)
{
    switch (context->product_id)
    {
    case AIC8800_USB_PID_AIC8801: return "aic8801";
    case AIC8800_USB_PID_AIC8800DC: return "aic8800dc";
    case AIC8800_USB_PID_AIC8800DW: return "aic8800dw";
    case AIC8800_USB_PID_AIC8800D80: return "aic8800d80";
    case AIC8800_USB_PID_AIC8800D81: return "aic8800d81";
    case AIC8800_USB_PID_AIC8800D83: return "aic8800d83";
    case AIC8800_USB_PID_AIC8800D84: return "aic8800d84";
    case AIC8800_USB_PID_AIC8800D85: return "aic8800d85";
    case AIC8800_USB_PID_AIC8800D86: return "aic8800d86";
    case AIC8800_USB_PID_AIC8800D88: return "aic8800d88";
    case AIC8800_USB_PID_AIC8800D40: return "aic8800d40";
    case AIC8800_USB_PID_AIC8800D41: return "aic8800d41";
    case AIC8800_USB_PID_AIC8800D80X2: return "aic8800d80x2";
    case AIC8800_USB_PID_AIC8800D81X2: return "aic8800d81x2";
    case AIC8800_USB_PID_AIC8800D89X2: return "aic8800d89x2";
    default: return "aic8800";
    }
}

static const char *aic8800_transport_name(
    const struct aic8800_context *context)
{
    return context->transport == AIC8800_TRANSPORT_SDIO ? "SDIO" : "USB";
}

static rt_uint16_t aic8800_protocol_version(
    const struct aic8800_context *context)
{
    return context && context->transport == AIC8800_TRANSPORT_SDIO ?
           AIC_WIRE_MSG_API_VERSION_SDIO : AIC_WIRE_MSG_API_VERSION_USB;
}

rt_err_t aic8800_core_attach(struct aic8800_context *context)
{
    struct rt_wlan_offload_command_manager_config command_config;
    struct rt_wlan_offload_radio_config radio_config;
    rt_err_t result;

    if (!context || context->attached)
    {
        return -RT_EINVAL;
    }
    if (!context->firmware_runtime_ready)
    {
        return -RT_EIO;
    }
    result = aic8800_radio_load_config(context);
    if (result != RT_EOK)
    {
        aic8800_core_cancel_work(context);
        return result;
    }
    aic_refresh_channel_metadata(context);
    rt_work_init(&context->scan_work, aic_scan_work, context);
    context->scan_work_initialized = RT_TRUE;
    rt_work_init(&context->station_loss_work,
                 aic_station_loss_work, context);
    context->station_loss_work_initialized = RT_TRUE;
    context->station_loss_work_queued = RT_FALSE;
    rt_memset(context->station_loss, 0, sizeof(context->station_loss));
    rt_work_init(&context->ap_rechannel_work, aic_ap_rechannel_work, context);
    context->ap_rechannel_work_initialized = RT_TRUE;
    result = rt_mutex_init(&context->mgmt_confirmation_mutex, "aic-cfm",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        aic8800_core_cancel_work(context);
        return result;
    }
    context->mgmt_confirmation_mutex_initialized = RT_TRUE;
    result = rt_sem_init(&context->command_gate, "aic-cmd", 1,
                         RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&context->mgmt_confirmation_mutex);
        context->mgmt_confirmation_mutex_initialized = RT_FALSE;
        aic8800_core_cancel_work(context);
        return result;
    }
    context->command_gate_initialized = RT_TRUE;
    rt_memset(&command_config, 0, sizeof(command_config));
    command_config.max_pending = 1;
    command_config.push = aic_command_push;
    command_config.driver_data = context;
    result = rt_wlan_offload_command_manager_init(&context->commands,
                                             &command_config);
    if (result != RT_EOK)
    {
        aic_command_gate_deinit(context);
        rt_mutex_detach(&context->mgmt_confirmation_mutex);
        context->mgmt_confirmation_mutex_initialized = RT_FALSE;
        aic8800_core_cancel_work(context);
        return result;
    }
    rt_wlan_offload_bus_set_callbacks(&context->bus, aic8800_core_receive,
                                 aic_bus_event, context);

    rt_memset(&radio_config, 0, sizeof(radio_config));
    radio_config.api_version = RT_WLAN_OFFLOAD_API_VERSION;
    radio_config.model_name = aic8800_model_name(context);
    radio_config.control_device = RT_TRUE;
    radio_config.ops = &g_aic8800_wifi_ops;
    radio_config.bus = &context->bus;
    radio_config.capabilities = RT_WLAN_OFFLOAD_CAP_STA |
                                RT_WLAN_OFFLOAD_CAP_AP |
                                RT_WLAN_OFFLOAD_CAP_STA_AP_CONCURRENT |
                                RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT |
                                RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR |
                                RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTH |
                                RT_WLAN_OFFLOAD_CAP_HOTPLUG;
    radio_config.max_frame_size = AIC8800_ETHERNET_FRAME_MAX;
    radio_config.bands[RT_WLAN_OFFLOAD_BAND_2GHZ] = &context->band_2ghz;
#ifdef AIC8800_WIFI_5GHZ
    if (aic_device_supports_5ghz(context))
    {
        radio_config.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] = &context->band_5ghz;
    }
#endif
    radio_config.cipher_suites = g_aic8800_ciphers;
    radio_config.cipher_suite_count = sizeof(g_aic8800_ciphers) /
                                      sizeof(g_aic8800_ciphers[0]);
    radio_config.iface_combinations = g_aic8800_iface_combinations;
    radio_config.iface_combination_count =
        sizeof(g_aic8800_iface_combinations) /
        sizeof(g_aic8800_iface_combinations[0]);
    radio_config.max_scan_ssids = AIC_SCAN_SSID_COUNT;
    radio_config.max_scan_ie_length = AIC_SCAN_IE_MAX;
    radio_config.firmware_info.protocol_version =
        aic8800_protocol_version(context);
    radio_config.firmware_info.max_stations = 10;
    radio_config.firmware_info.max_vifs = 2;
    radio_config.firmware_info.max_channel_contexts = 1;
    radio_config.driver_data = context;
    result = rt_wlan_offload_register_radio(&context->radio, &radio_config);
    if (result != RT_EOK)
    {
        rt_wlan_offload_bus_set_callbacks(&context->bus, RT_NULL, RT_NULL, RT_NULL);
        rt_wlan_offload_command_manager_deinit(&context->commands);
        aic_command_gate_deinit(context);
        rt_mutex_detach(&context->mgmt_confirmation_mutex);
        context->mgmt_confirmation_mutex_initialized = RT_FALSE;
        aic8800_core_cancel_work(context);
        return result;
    }
    result = aic_rx_reorder_init(context);
    if (result != RT_EOK)
    {
        rt_wlan_offload_unregister_radio(&context->radio);
        rt_wlan_offload_bus_set_callbacks(&context->bus, RT_NULL, RT_NULL,
                                          RT_NULL);
        rt_wlan_offload_command_manager_deinit(&context->commands);
        aic_command_gate_deinit(context);
        rt_mutex_detach(&context->mgmt_confirmation_mutex);
        context->mgmt_confirmation_mutex_initialized = RT_FALSE;
        aic8800_core_cancel_work(context);
        return result;
    }
#ifdef AIC8800_WIFI_TCP_ACK_FILTER
    result = aic_tcp_ack_init(context);
    if (result != RT_EOK)
    {
        aic_rx_reorder_deinit(context);
        rt_wlan_offload_unregister_radio(&context->radio);
        rt_wlan_offload_bus_set_callbacks(&context->bus, RT_NULL, RT_NULL,
                                          RT_NULL);
        rt_wlan_offload_command_manager_deinit(&context->commands);
        aic_command_gate_deinit(context);
        rt_mutex_detach(&context->mgmt_confirmation_mutex);
        context->mgmt_confirmation_mutex_initialized = RT_FALSE;
        aic8800_core_cancel_work(context);
        return result;
    }
#endif
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        result = aic_sdio_data_queue_init(context);
        if (result != RT_EOK)
        {
#ifdef AIC8800_WIFI_TCP_ACK_FILTER
            aic_tcp_ack_deinit(context);
#endif
            aic_rx_reorder_deinit(context);
            rt_wlan_offload_unregister_radio(&context->radio);
            rt_wlan_offload_bus_set_callbacks(&context->bus, RT_NULL,
                                              RT_NULL, RT_NULL);
            rt_wlan_offload_command_manager_deinit(&context->commands);
            aic_command_gate_deinit(context);
            rt_mutex_detach(&context->mgmt_confirmation_mutex);
            context->mgmt_confirmation_mutex_initialized = RT_FALSE;
            aic8800_core_cancel_work(context);
            return result;
        }
    }
#endif
    {
        rt_tick_t period = rt_tick_from_millisecond(
            AIC8800_WIFI_MGMT_CONFIRM_TIMEOUT_MS / 4U);

        if (!period)
        {
            period = 1;
        }
        rt_timer_init(&context->mgmt_confirmation_timer, "aic-cfm",
                      aic_mgmt_confirmation_timeout, context, period,
                      RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
        context->mgmt_confirmation_timer_initialized = RT_TRUE;
        result = rt_timer_start(&context->mgmt_confirmation_timer);
        if (result != RT_EOK)
        {
            rt_timer_detach(&context->mgmt_confirmation_timer);
            context->mgmt_confirmation_timer_initialized = RT_FALSE;
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
            if (context->transport == AIC8800_TRANSPORT_SDIO)
            {
                aic_sdio_data_queue_deinit(context);
            }
#endif
#ifdef AIC8800_WIFI_TCP_ACK_FILTER
            aic_tcp_ack_deinit(context);
#endif
            aic_rx_reorder_deinit(context);
            rt_wlan_offload_unregister_radio(&context->radio);
            rt_wlan_offload_bus_set_callbacks(&context->bus, RT_NULL, RT_NULL,
                                         RT_NULL);
            rt_wlan_offload_command_manager_deinit(&context->commands);
            aic_command_gate_deinit(context);
            rt_mutex_detach(&context->mgmt_confirmation_mutex);
            context->mgmt_confirmation_mutex_initialized = RT_FALSE;
            aic8800_core_cancel_work(context);
            return result;
        }
    }
    rt_work_init(&context->traffic_work, aic_traffic_work, context);
    context->traffic_work_initialized = RT_TRUE;
    context->traffic_work_queued = RT_FALSE;
    context->attached = RT_TRUE;
    {
        char control[RT_NAME_MAX];

        if (rt_wlan_offload_control_get_name(&context->radio, control,
                                             sizeof(control)) != RT_EOK)
        {
            rt_strncpy(control, "none", sizeof(control));
        }
        LOG_I("registered AIC8800 WLAN offload over %s; control=/dev/%s",
              aic8800_transport_name(context), control);
    }
#ifdef AIC8800_WIFI_AUTO_START
    result = rt_wlan_set_mode(
        context->radio.vifs[RT_WLAN_OFFLOAD_VIF_STA_INDEX].wlan.device.parent.name,
        RT_WLAN_STATION);
    if (result != RT_EOK)
    {
        LOG_W("automatic station initialization failed: %d", result);
    }
    result = rt_wlan_set_mode(
        context->radio.vifs[RT_WLAN_OFFLOAD_VIF_AP_INDEX].wlan.device.parent.name,
        RT_WLAN_AP);
    if (result != RT_EOK)
    {
        LOG_W("automatic AP initialization failed: %d", result);
    }
#endif
    return RT_EOK;
}

rt_err_t aic8800_core_detach(struct aic8800_context *context)
{
    rt_err_t result;

    if (!context || !context->attached)
    {
        return context ? RT_EOK : -RT_EINVAL;
    }
    aic8800_core_cancel_work(context);
    if (context->mgmt_confirmation_timer_initialized)
    {
        rt_timer_stop(&context->mgmt_confirmation_timer);
        rt_timer_detach(&context->mgmt_confirmation_timer);
        context->mgmt_confirmation_timer_initialized = RT_FALSE;
    }
    aic_cancel_mgmt_confirmations(context, -RT_EIO);
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        aic_sdio_data_queue_deinit(context);
    }
#endif
    aic_rx_reorder_reset(context);
    aic_tcp_ack_reset(context);
#ifdef AIC8800_WIFI_TCP_ACK_FILTER
    aic_tcp_ack_deinit(context);
#endif
    result = rt_wlan_offload_unregister_radio(&context->radio);
    if (result != RT_EOK)
    {
        LOG_E("WLAN offload radio teardown failed: %d", result);
        return result;
    }
    aic_rx_reorder_deinit(context);
    rt_wlan_offload_bus_set_callbacks(&context->bus, RT_NULL, RT_NULL, RT_NULL);
    result = rt_wlan_offload_command_manager_deinit(&context->commands);
    if (result != RT_EOK)
    {
        LOG_E("command manager teardown failed: %d", result);
        return result;
    }
    aic_command_gate_deinit(context);
    if (context->mgmt_confirmation_mutex_initialized)
    {
        result = rt_mutex_detach(&context->mgmt_confirmation_mutex);
        if (result != RT_EOK)
        {
            LOG_E("management confirmation teardown failed: %d", result);
            return result;
        }
        context->mgmt_confirmation_mutex_initialized = RT_FALSE;
    }
    context->attached = RT_FALSE;
    return RT_EOK;
}
