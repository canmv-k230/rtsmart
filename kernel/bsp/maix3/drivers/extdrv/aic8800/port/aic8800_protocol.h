/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fixed-width messages used by the AIC8800 host/firmware interface.
 */
#ifndef __AIC8800_PROTOCOL_H__
#define __AIC8800_PROTOCOL_H__

#include <rtthread.h>

#define AIC_WIRE_TASK_MM                 0U
#define AIC_WIRE_TASK_SCANU              4U
#define AIC_WIRE_TASK_ME                 5U
#define AIC_WIRE_TASK_SM                 6U
#define AIC_WIRE_TASK_APM                7U
#define AIC_WIRE_DRIVER_TASK           100U

#define AIC_WIRE_MSG(_task, _index) \
    ((rt_uint16_t)(((_task) << 10) | (_index)))

#define AIC_MM_RESET_REQ          AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 0)
#define AIC_MM_RESET_CFM          AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 1)
#define AIC_MM_START_REQ          AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 2)
#define AIC_MM_START_CFM          AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 3)
#define AIC_MM_VERSION_REQ        AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 4)
#define AIC_MM_VERSION_CFM        AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 5)
#define AIC_MM_ADD_IF_REQ         AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 6)
#define AIC_MM_ADD_IF_CFM         AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 7)
#define AIC_MM_REMOVE_IF_REQ      AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 8)
#define AIC_MM_REMOVE_IF_CFM      AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 9)
#define AIC_MM_SET_FILTER_REQ     AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 14)
#define AIC_MM_SET_FILTER_CFM     AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 15)
#define AIC_MM_SET_CHANNEL_REQ    AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 16)
#define AIC_MM_SET_CHANNEL_CFM    AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 17)
#define AIC_MM_KEY_ADD_REQ        AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 36)
#define AIC_MM_KEY_ADD_CFM        AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 37)
#define AIC_MM_KEY_DEL_REQ        AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 38)
#define AIC_MM_KEY_DEL_CFM        AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 39)
/* The firmware serves one channel context at a time and announces the
 * transitions.  A VIF whose context is not the scheduled one must not be given
 * traffic; see aic_channel_context_active(). */
#define AIC_MM_CHANNEL_SWITCH_IND     AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 68)
#define AIC_MM_CHANNEL_PRE_SWITCH_IND AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 69)
/* An associated peer entering or leaving firmware-managed power save. */
#define AIC_MM_PS_CHANGE_IND      AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 73)
#define AIC_MM_SET_RF_CONFIG_REQ  AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 103)
#define AIC_MM_SET_RF_CONFIG_CFM  AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 104)
#define AIC_MM_SET_RF_CALIB_REQ   AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 105)
#define AIC_MM_SET_RF_CALIB_CFM   AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 106)
#define AIC_MM_GET_MAC_REQ        AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 115)
#define AIC_MM_GET_MAC_CFM        AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 116)
#define AIC_MM_SET_TXPWR_REQ      AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 119)
#define AIC_MM_SET_TXPWR_CFM      AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 120)
#define AIC_MM_SET_TXPWR_OFST_REQ AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 121)
#define AIC_MM_SET_TXPWR_OFST_CFM AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 122)
#define AIC_MM_SET_STACK_START_REQ AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 123)
#define AIC_MM_SET_STACK_START_CFM AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 124)
#define AIC_MM_GET_STA_INFO_REQ   AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 117)
#define AIC_MM_GET_STA_INFO_CFM   AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 118)
#define AIC_MM_GET_FW_VERSION_REQ  AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 128)
#define AIC_MM_GET_FW_VERSION_CFM  AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 129)
#define AIC_MM_SET_TXPWR_ADJ_REQ  AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 137)
#define AIC_MM_SET_TXPWR_ADJ_CFM  AIC_WIRE_MSG(AIC_WIRE_TASK_MM, 138)

/* AIC firmware ships with two incompatible MM_VERSION feature layouts.  The
 * full layout appears in the vendor USB driver, while current D80 firmware and
 * the SDIO driver use the compact layout.  Decode either layout into the
 * transport-independent AIC_FW_CAP_* masks before consuming it. */
#define AIC_MM_FULL_FEATURE_PS_BIT           6U
#define AIC_MM_FULL_FEATURE_DPSM_BIT         8U
#define AIC_MM_FULL_FEATURE_CHNL_CTXT_BIT   11U
#define AIC_MM_FULL_FEATURE_UMAC_BIT        15U
#define AIC_MM_FULL_FEATURE_VHT_BIT         16U
#define AIC_MM_FULL_FEATURE_BFMEE_BIT       17U
#define AIC_MM_FULL_FEATURE_MFP_BIT         20U
#define AIC_MM_FULL_FEATURE_MU_MIMO_RX_BIT  21U
#define AIC_MM_FULL_FEATURE_ANT_DIV_BIT     25U
#define AIC_MM_FULL_FEATURE_MON_DATA_BIT    29U
#define AIC_MM_FULL_FEATURE_HE_BIT          30U

#define AIC_MM_COMPACT_FEATURE_PS_BIT        2U
#define AIC_MM_COMPACT_FEATURE_UMAC_BIT      8U
#define AIC_MM_COMPACT_FEATURE_VHT_BIT       9U
#define AIC_MM_COMPACT_FEATURE_BFMEE_BIT    10U
#define AIC_MM_COMPACT_FEATURE_MFP_BIT      13U
#define AIC_MM_COMPACT_FEATURE_MU_MIMO_RX_BIT 14U
#define AIC_MM_COMPACT_FEATURE_ANT_DIV_BIT  18U
#define AIC_MM_COMPACT_FEATURE_MON_DATA_BIT 22U
#define AIC_MM_COMPACT_FEATURE_HE_BIT       23U

#define AIC_FW_CAP_PS                        (1UL << 0)
#define AIC_FW_CAP_DPSM                      (1UL << 1)
#define AIC_FW_CAP_CHNL_CTXT                 (1UL << 2)
#define AIC_FW_CAP_VHT                       (1UL << 3)
#define AIC_FW_CAP_MFP                       (1UL << 4)
#define AIC_FW_CAP_ANT_DIV                   (1UL << 5)
#define AIC_FW_CAP_MON_DATA                  (1UL << 6)
#define AIC_FW_CAP_HE                        (1UL << 7)
#define AIC_FW_CAP_BFMEE                     (1UL << 8)
#define AIC_FW_CAP_MU_MIMO_RX                (1UL << 9)

/* RX filter bits from the vendor NXMAC_ACCEPT_* register. */
#define AIC_RX_FILTER_MULTICAST       (1UL << 2)
#define AIC_RX_FILTER_BROADCAST       (1UL << 3)
#define AIC_RX_FILTER_OTHER_BSSID     (1UL << 4)
#define AIC_RX_FILTER_UNICAST         (1UL << 6)
#define AIC_RX_FILTER_MY_UNICAST      (1UL << 7)
#define AIC_RX_FILTER_PROBE_REQ       (1UL << 8)
#define AIC_RX_FILTER_PROBE_RESP      (1UL << 9)
#define AIC_RX_FILTER_BEACON          (1UL << 10)
#define AIC_RX_FILTER_OTHER_MGMT      (1UL << 15)
#define AIC_RX_FILTER_BAR             (1UL << 16)
#define AIC_RX_FILTER_BA              (1UL << 17)
#define AIC_RX_FILTER_PS_POLL         (1UL << 18)
#define AIC_RX_FILTER_OTHER_CONTROL   (1UL << 23)
#define AIC_RX_FILTER_DATA            (1UL << 24)
#define AIC_RX_FILTER_Q_DATA          (1UL << 26)
#define AIC_RX_FILTER_QOS_NULL        (1UL << 28)
#define AIC_RX_FILTER_OTHER_DATA      (1UL << 29)

#define AIC_RX_FILTER_FIXED (AIC_RX_FILTER_QOS_NULL | \
                             AIC_RX_FILTER_Q_DATA | \
                             AIC_RX_FILTER_DATA | \
                             AIC_RX_FILTER_OTHER_MGMT | \
                             AIC_RX_FILTER_MY_UNICAST | \
                             AIC_RX_FILTER_BROADCAST | \
                             AIC_RX_FILTER_BEACON | \
                             AIC_RX_FILTER_PROBE_RESP)
#define AIC_RX_FILTER_DEFAULT (AIC_RX_FILTER_FIXED | \
                               AIC_RX_FILTER_BA | \
                               AIC_RX_FILTER_BAR | \
                               AIC_RX_FILTER_OTHER_DATA | \
                               AIC_RX_FILTER_PROBE_REQ | \
                               AIC_RX_FILTER_PS_POLL)
#define AIC_RX_FILTER_PROMISCUOUS (AIC_RX_FILTER_DEFAULT | \
                                   AIC_RX_FILTER_OTHER_BSSID | \
                                   AIC_RX_FILTER_UNICAST | \
                                   AIC_RX_FILTER_MULTICAST | \
                                   AIC_RX_FILTER_OTHER_CONTROL)

#define AIC_SCANU_START_REQ       AIC_WIRE_MSG(AIC_WIRE_TASK_SCANU, 0)
#define AIC_SCANU_START_CFM       AIC_WIRE_MSG(AIC_WIRE_TASK_SCANU, 1)
#define AIC_SCANU_RESULT_IND      AIC_WIRE_MSG(AIC_WIRE_TASK_SCANU, 4)
#define AIC_SCANU_VENDOR_IE_REQ   AIC_WIRE_MSG(AIC_WIRE_TASK_SCANU, 7)
#define AIC_SCANU_VENDOR_IE_CFM   AIC_WIRE_MSG(AIC_WIRE_TASK_SCANU, 8)
#define AIC_SCANU_START_ACCEPTED  AIC_WIRE_MSG(AIC_WIRE_TASK_SCANU, 9)
#define AIC_SCANU_CANCEL_REQ      AIC_WIRE_MSG(AIC_WIRE_TASK_SCANU, 10)
#define AIC_SCANU_CANCEL_CFM      AIC_WIRE_MSG(AIC_WIRE_TASK_SCANU, 11)

#define AIC_ME_CONFIG_REQ         AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 0)
#define AIC_ME_CONFIG_CFM         AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 1)
#define AIC_ME_CHAN_CONFIG_REQ    AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 2)
#define AIC_ME_CHAN_CONFIG_CFM    AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 3)
#define AIC_ME_CONTROL_PORT_REQ   AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 4)
#define AIC_ME_CONTROL_PORT_CFM   AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 5)
#define AIC_ME_STA_ADD_REQ        AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 7)
#define AIC_ME_STA_ADD_CFM        AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 8)
#define AIC_ME_STA_DEL_REQ        AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 9)
#define AIC_ME_STA_DEL_CFM        AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 10)
/* Firmware-reported transmit credit offsets.  This generation's vendor driver
 * leaves enforcement disabled, so the values are collected for diagnostics. */
#define AIC_ME_TX_CREDITS_UPDATE_IND AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 11)
#define AIC_ME_RC_STATS_REQ       AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 14)
#define AIC_ME_RC_STATS_CFM       AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 15)
#define AIC_ME_SET_PS_MODE_REQ    AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 19)
#define AIC_ME_SET_PS_MODE_CFM    AIC_WIRE_MSG(AIC_WIRE_TASK_ME, 20)

#define AIC_ME_PS_MODE_OFF        0U
#define AIC_ME_PS_MODE_ON         1U
#define AIC_ME_PS_MODE_ON_DYN     2U

#define AIC_SM_CONNECT_REQ        AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 0)
#define AIC_SM_CONNECT_CFM        AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 1)
#define AIC_SM_CONNECT_IND        AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 2)
#define AIC_SM_DISCONNECT_REQ     AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 3)
#define AIC_SM_DISCONNECT_CFM     AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 4)
#define AIC_SM_DISCONNECT_IND     AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 5)
#define AIC_SM_EXTERNAL_AUTH_REQUIRED_IND \
                                    AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 6)
#define AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP \
                                    AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 7)
#define AIC_SM_EXTERNAL_AUTH_REQUIRED_RSP_CFM \
                                    AIC_WIRE_MSG(AIC_WIRE_TASK_SM, 12)

#define AIC_APM_START_REQ         AIC_WIRE_MSG(AIC_WIRE_TASK_APM, 0)
#define AIC_APM_START_CFM         AIC_WIRE_MSG(AIC_WIRE_TASK_APM, 1)
#define AIC_APM_STOP_REQ          AIC_WIRE_MSG(AIC_WIRE_TASK_APM, 2)
#define AIC_APM_STOP_CFM          AIC_WIRE_MSG(AIC_WIRE_TASK_APM, 3)
#define AIC_APM_SET_BEACON_IE_REQ AIC_WIRE_MSG(AIC_WIRE_TASK_APM, 8)
#define AIC_APM_SET_BEACON_IE_CFM AIC_WIRE_MSG(AIC_WIRE_TASK_APM, 9)

#define AIC_USB_TYPE_DATA                  0x00U
#define AIC_USB_TYPE_DATA_TX               0x01U
#define AIC_USB_TYPE_CONFIG                0x10U
#define AIC_USB_TYPE_COMMAND               0x11U
#define AIC_USB_TYPE_DATA_CONFIRM          0x12U
#define AIC_USB_TYPE_PRINT                 0x13U
#define AIC_USB_LENGTH_MASK              0x0fffU

#define AIC_WIRE_E2A_PATTERN       0xaddeDe2aUL
#define AIC_WIRE_MSG_API_VERSION             15U
#define AIC_WIRE_SCAN_CHANNEL_COUNT          42U
#define AIC_WIRE_SCAN_SSID_COUNT              3U
#define AIC_WIRE_SCAN_IE_MAX                200U

struct aic_wire_mac_addr
{
    rt_uint16_t array[3];
};

struct aic_wire_mac_ssid
{
    rt_uint8_t length;
    rt_uint8_t array[32];
};

struct aic_wire_mac_channel
{
    rt_uint16_t frequency;
    rt_uint8_t band;
    rt_uint8_t flags;
    rt_int8_t tx_power;
};

struct aic_wire_ht_capability
{
    rt_uint16_t capability;
    rt_uint8_t ampdu_parameters;
    rt_uint8_t mcs_rate[16];
    rt_uint16_t extended_capability;
    rt_uint32_t beamforming_capability;
    rt_uint8_t antenna_selection_capability;
};

struct aic_wire_vht_capability
{
    rt_uint32_t capability;
    rt_uint16_t rx_mcs_map;
    rt_uint16_t rx_highest;
    rt_uint16_t tx_mcs_map;
    rt_uint16_t tx_highest;
};

struct aic_wire_he_mcs_support
{
    rt_uint16_t rx_mcs_80;
    rt_uint16_t tx_mcs_80;
    rt_uint16_t rx_mcs_160;
    rt_uint16_t tx_mcs_160;
    rt_uint16_t rx_mcs_80p80;
    rt_uint16_t tx_mcs_80p80;
};

struct aic_wire_he_capability
{
    rt_uint8_t mac_capability[6];
    rt_uint8_t phy_capability[11];
    struct aic_wire_he_mcs_support mcs;
    rt_uint8_t ppe_thresholds[25];
};

struct aic_wire_mm_start_req
{
    rt_uint32_t phy_parameters[16];
    rt_uint32_t uapsd_timeout;
    rt_uint16_t lp_clock_accuracy;
};

struct aic_wire_mm_version_cfm
{
    rt_uint32_t lmac_version;
    rt_uint32_t mac_version1;
    rt_uint32_t mac_version2;
    rt_uint32_t phy_version1;
    rt_uint32_t phy_version2;
    rt_uint32_t features;
    rt_uint16_t max_stations;
    rt_uint8_t max_vifs;
};

struct aic_wire_mm_set_stack_start_req
{
    rt_uint8_t start;
    rt_uint8_t efuse_valid;
    rt_uint8_t vendor_info;
    rt_uint8_t firmware_trace_redirect;
};

struct aic_wire_mm_set_stack_start_cfm
{
    rt_uint8_t supports_5ghz;
    rt_uint8_t vendor_info;
};

struct aic_wire_mm_get_sta_info_req
{
    rt_uint8_t station_index;
};

struct aic_wire_mm_set_filter_req
{
    rt_uint32_t filter;
};

struct aic_wire_mm_get_sta_info_cfm
{
    rt_uint32_t rate_info;
    rt_uint32_t tx_failed;
    rt_int8_t rssi;
    rt_uint8_t reserved[3];
};

/* The operating-channel ABI is shared by MM_SET_CHANNEL and several ME
 * messages in the vendor firmware.  Keep the field order byte-for-byte
 * compatible with struct mac_chan_op. */
struct aic_wire_mac_chan_op
{
    rt_uint8_t band;
    rt_uint8_t type;
    rt_uint16_t primary_frequency;
    rt_uint16_t center_frequency1;
    rt_uint16_t center_frequency2;
    rt_int8_t tx_power;
    rt_uint8_t flags;
};

struct aic_wire_mm_set_channel_req
{
    struct aic_wire_mac_chan_op channel;
    rt_uint8_t index;
};

struct aic_wire_mm_set_channel_cfm
{
    rt_uint8_t radio_index;
    rt_int8_t power;
};

struct aic_wire_me_set_ps_mode_req
{
    rt_uint8_t state;
};

struct aic_wire_mm_ps_change_ind
{
    rt_uint8_t station_index;
    rt_uint8_t power_save;      /* 0: awake, 1: sleeping */
};

struct aic_wire_mm_channel_switch_ind
{
    rt_uint8_t channel_index;
    rt_uint8_t remain_on_channel;
    rt_uint8_t vif_index;
    rt_uint8_t remain_on_channel_tdls;
};

struct aic_wire_mm_channel_pre_switch_ind
{
    rt_uint8_t channel_index;
};

struct aic_wire_mm_get_fw_version_cfm
{
    rt_uint8_t length;
    rt_uint8_t version[63];
};

struct aic_wire_mm_add_if_req
{
    rt_uint8_t type;
    struct aic_wire_mac_addr address;
    rt_uint8_t p2p;
};

struct aic_wire_security_key
{
    rt_uint8_t length;
    rt_uint32_t array[8];
};

struct aic_wire_mm_key_add_req
{
    rt_uint8_t key_index;
    rt_uint8_t station_index;
    struct aic_wire_security_key key;
    rt_uint8_t cipher;
    rt_uint8_t vif_index;
    rt_uint8_t spp;
    rt_uint8_t pairwise;
};

struct aic_wire_mac_rateset
{
    rt_uint8_t length;
    rt_uint8_t array[12];
};

struct aic_wire_me_sta_add_req
{
    struct aic_wire_mac_addr address;
    struct aic_wire_mac_rateset rates;
    struct aic_wire_ht_capability ht;
    struct aic_wire_vht_capability vht;
    struct aic_wire_he_capability he;
    rt_uint32_t flags;
    rt_uint16_t aid;
    rt_uint8_t uapsd_queues;
    rt_uint8_t max_sp_length;
    rt_uint8_t opmode;
    rt_uint8_t vif_index;
    rt_uint8_t tdls_station;
    rt_uint8_t tdls_initiator;
    rt_uint8_t tdls_channel_switch;
};

struct aic_wire_me_sta_add_cfm
{
    rt_uint8_t station_index;
    rt_uint8_t status;
    rt_uint8_t power_state;
};

struct aic_wire_me_sta_del_req
{
    rt_uint8_t station_index;
    rt_uint8_t tdls_station;
};

struct aic_wire_apm_start_req
{
    struct aic_wire_mac_rateset basic_rates;
    struct aic_wire_mac_channel channel;
    rt_uint32_t center_frequency1;
    rt_uint32_t center_frequency2;
    rt_uint8_t channel_width;
    rt_uint32_t beacon_address;
    rt_uint16_t beacon_length;
    rt_uint16_t tim_offset;
    rt_uint16_t beacon_interval;
    rt_uint32_t flags;
    rt_uint16_t control_port_ethertype;
    rt_uint8_t tim_length;
    rt_uint8_t vif_index;
};

struct aic_wire_apm_start_cfm
{
    rt_uint8_t status;
    rt_uint8_t vif_index;
    rt_uint8_t channel_index;
    rt_uint8_t broadcast_station_index;
};

struct aic_wire_apm_set_beacon_ie_req
{
    rt_uint8_t vif_index;
    rt_uint16_t beacon_length;
    rt_uint8_t beacon[512];
};

struct aic_wire_me_config_req
{
    struct aic_wire_ht_capability ht;
    struct aic_wire_vht_capability vht;
    struct aic_wire_he_capability he;
    rt_uint16_t tx_lifetime;
    rt_uint8_t max_bandwidth;
    rt_uint8_t ht_supported;
    rt_uint8_t vht_supported;
    rt_uint8_t he_supported;
    rt_uint8_t he_uplink_enabled;
    rt_uint8_t power_save_enabled;
    rt_uint8_t antenna_diversity_enabled;
    rt_uint8_t dynamic_power_save;
};

#define AIC_WIRE_RC_SAMPLE_COUNT 10U
#define AIC_WIRE_RC_HE_SAMPLE_INDEX AIC_WIRE_RC_SAMPLE_COUNT

struct aic_wire_me_tx_credits_update_ind
{
    rt_uint8_t station_index;
    rt_uint8_t tid;
    rt_int8_t credits;          /* Offset to apply, may be negative. */
};

struct aic_wire_me_rc_stats_req
{
    rt_uint8_t station_index;
};

struct aic_wire_rc_rate_stats
{
    rt_uint16_t attempts;
    rt_uint16_t success;
    rt_uint16_t probability;
    rt_uint16_t rate_config;
    union
    {
        struct
        {
            rt_uint8_t sample_skipped;
            rt_uint8_t old_probability_available;
            rt_uint8_t rate_allowed;
        } sample;
        rt_uint16_t ru_and_length;
    } detail;
};

struct aic_wire_me_rc_stats_cfm
{
    rt_uint8_t station_index;
    rt_uint16_t sample_count;
    rt_uint16_t ampdu_length;
    rt_uint16_t ampdu_packets;
    rt_uint32_t average_ampdu_length;
    rt_uint8_t software_retry_step;
    rt_uint8_t sample_wait;
    rt_uint16_t retry_step_index[4];
    struct aic_wire_rc_rate_stats
        rates[AIC_WIRE_RC_SAMPLE_COUNT + 1U];
    rt_uint32_t throughput[AIC_WIRE_RC_SAMPLE_COUNT + 1U];
};

struct aic_wire_me_channel_config_req
{
    struct aic_wire_mac_channel channels_2ghz[14];
    struct aic_wire_mac_channel channels_5ghz[28];
    rt_uint8_t count_2ghz;
    rt_uint8_t count_5ghz;
};

struct aic_wire_scanu_start_req
{
    struct aic_wire_mac_channel channels[AIC_WIRE_SCAN_CHANNEL_COUNT];
    struct aic_wire_mac_ssid ssids[AIC_WIRE_SCAN_SSID_COUNT];
    struct aic_wire_mac_addr bssid;
    rt_uint32_t additional_ies;
    rt_uint16_t additional_ie_length;
    rt_uint8_t vif_index;
    rt_uint8_t channel_count;
    rt_uint8_t ssid_count;
    rt_uint8_t no_cck;
    rt_uint32_t duration_us;
};

struct aic_wire_sm_connect_req
{
    struct aic_wire_mac_ssid ssid;
    struct aic_wire_mac_addr bssid;
    struct aic_wire_mac_channel channel;
    rt_uint32_t flags;
    rt_uint16_t control_port_ethertype;
    rt_uint16_t ie_length;
    rt_uint16_t listen_interval;
    rt_uint8_t dont_wait_bcmc;
    rt_uint8_t auth_type;
    rt_uint8_t uapsd_queues;
    rt_uint8_t vif_index;
    rt_uint32_t ie_buffer[64];
};

struct aic_wire_sm_external_auth_required_ind
{
    rt_uint8_t vif_index;
    struct aic_wire_mac_ssid ssid;
    struct aic_wire_mac_addr bssid;
    rt_uint32_t akm;
};

struct aic_wire_sm_external_auth_required_rsp
{
    rt_uint8_t vif_index;
    rt_uint8_t reserved;
    rt_uint16_t status;
};

struct aic_wire_tx_host_descriptor
{
    rt_uint16_t packet_length;
    rt_uint16_t extended_flags;
    rt_uint32_t status_descriptor;
    struct aic_wire_mac_addr destination;
    struct aic_wire_mac_addr source;
    rt_uint16_t ethertype;
    rt_uint8_t access_category;
    rt_uint8_t tid;
    rt_uint8_t vif_index;
    rt_uint8_t station_index;
    rt_uint16_t flags;
};

struct aic_wire_tx_power_index
{
    rt_int8_t enable;
    rt_int8_t dsss;
    rt_int8_t ofdm_low_2ghz;
    rt_int8_t ofdm_64qam_2ghz;
    rt_int8_t ofdm_256qam_2ghz;
    rt_int8_t ofdm_1024qam_2ghz;
    rt_int8_t ofdm_low_5ghz;
    rt_int8_t ofdm_64qam_5ghz;
    rt_int8_t ofdm_256qam_5ghz;
    rt_int8_t ofdm_1024qam_5ghz;
};

struct aic_wire_tx_power_v2
{
    rt_uint8_t enable;
    rt_int8_t legacy_2ghz[12];
    rt_int8_t ht_vht_2ghz[10];
    rt_int8_t he_2ghz[12];
};

struct aic_wire_tx_power_v3
{
    rt_uint8_t enable;
    rt_int8_t legacy_2ghz[12];
    rt_int8_t ht_vht_2ghz[10];
    rt_int8_t he_2ghz[12];
    rt_int8_t legacy_5ghz[12];
    rt_int8_t ht_vht_5ghz[10];
    rt_int8_t he_5ghz[12];
};

struct aic_wire_tx_power_v4
{
    rt_uint8_t enable;
    rt_int8_t legacy_2ghz[12];
    rt_int8_t ht_vht_2ghz[10];
    rt_int8_t he_2ghz[12];
    rt_int8_t legacy_5ghz[8];
    rt_int8_t ht_vht_5ghz[10];
    rt_int8_t he_5ghz[12];
    rt_int8_t legacy_6ghz[8];
    rt_int8_t ht_vht_6ghz[10];
    rt_int8_t he_6ghz[12];
};

struct aic_wire_mm_set_tx_power_req
{
    union
    {
        struct aic_wire_tx_power_index index;
        struct aic_wire_tx_power_v2 v2;
        struct aic_wire_tx_power_v3 v3;
        struct aic_wire_tx_power_v4 v4;
    } configuration;
};

struct aic_wire_tx_power_offset
{
    rt_int8_t enable;
    rt_int8_t channels_1_4;
    rt_int8_t channels_5_9;
    rt_int8_t channels_10_13;
    rt_int8_t channels_36_64;
    rt_int8_t channels_100_120;
    rt_int8_t channels_122_140;
    rt_int8_t channels_142_165;
};

struct aic_wire_tx_power_offset_2x
{
    rt_uint8_t enable;
    rt_int8_t offsets_2ghz[3][3];
    rt_int8_t offsets_5ghz[3][6];
};

struct aic_wire_tx_power_offset_2x_v2
{
    rt_uint8_t enable;
    rt_uint8_t flags;
    rt_int8_t offsets_2ghz_ant0[3][3];
    rt_int8_t offsets_2ghz_ant1[3][3];
    rt_int8_t offsets_5ghz_ant0[6][3];
    rt_int8_t offsets_5ghz_ant1[6][3];
    rt_int8_t offsets_6ghz_ant0[15];
    rt_int8_t offsets_6ghz_ant1[15];
};

struct aic_wire_mm_set_tx_power_offset_req
{
    union
    {
        struct aic_wire_tx_power_offset offset;
        struct aic_wire_tx_power_offset_2x offset_2x;
        struct aic_wire_tx_power_offset_2x_v2 offset_2x_v2;
    } configuration;
};

struct aic_wire_tx_power_adjust
{
    rt_uint8_t enable;
    rt_int8_t adjustment_2ghz[3];
    rt_int8_t adjustment_5ghz[6];
};

struct aic_wire_mm_set_tx_power_adjust_req
{
    struct aic_wire_tx_power_adjust adjustment;
};

struct aic_wire_mm_set_rf_config_req
{
    rt_uint8_t table_selector;
    rt_uint8_t table_offset;
    rt_uint8_t table_count;
    rt_uint8_t default_page;
    rt_uint32_t data[64];
};

struct aic_wire_mm_set_rf_calibration_req
{
    rt_uint32_t calibration_2ghz;
    rt_uint32_t calibration_5ghz;
    rt_uint32_t alpha;
    rt_uint32_t bluetooth_enabled;
    rt_uint32_t bluetooth_parameter;
    rt_uint8_t crystal_capacitance;
    rt_uint8_t crystal_capacitance_fine;
};

struct aic_wire_mm_set_rf_calibration_cfm
{
    rt_uint32_t rx_gain_2ghz_address;
    rt_uint32_t rx_gain_5ghz_address;
    rt_uint32_t tx_gain_2ghz_address;
    rt_uint32_t tx_gain_5ghz_address;
};

_Static_assert(sizeof(struct aic_wire_mac_channel) == 6,
               "AIC channel ABI changed");
_Static_assert(sizeof(struct aic_wire_ht_capability) == 32,
               "AIC HT capability ABI changed");
_Static_assert(sizeof(struct aic_wire_he_capability) == 56,
               "AIC HE capability ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_start_req) == 72,
               "AIC start request ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_version_cfm) == 28,
               "AIC version confirmation ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_set_stack_start_req) == 4,
               "AIC stack-start request ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_set_stack_start_cfm) == 2,
               "AIC stack-start confirmation ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_get_sta_info_req) == 1,
               "AIC station-info request ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_set_filter_req) == 4,
               "AIC RX-filter request ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_get_sta_info_cfm) == 12,
               "AIC station-info confirmation ABI changed");
_Static_assert(sizeof(struct aic_wire_mac_chan_op) == 10,
               "AIC operating-channel ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_set_channel_req) == 12,
               "AIC set-channel request ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_set_channel_cfm) == 2,
               "AIC set-channel confirmation ABI changed");
_Static_assert(sizeof(struct aic_wire_me_set_ps_mode_req) == 1,
               "AIC power-save request ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_get_fw_version_cfm) == 64,
               "AIC firmware-version confirmation ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_add_if_req) == 10,
               "AIC add-interface ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_key_add_req) == 44,
               "AIC key request ABI changed");
_Static_assert(sizeof(struct aic_wire_mac_rateset) == 13,
               "AIC rate-set ABI changed");
_Static_assert(sizeof(struct aic_wire_me_sta_add_req) == 136,
               "AIC station-add ABI changed");
_Static_assert(sizeof(struct aic_wire_me_sta_del_req) == 2,
               "AIC station-delete ABI changed");
_Static_assert(sizeof(struct aic_wire_apm_start_req) == 52,
               "AIC AP-start ABI changed");
_Static_assert(sizeof(struct aic_wire_apm_start_cfm) == 4,
               "AIC AP-start confirmation ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_ps_change_ind) == 2,
               "AIC power-save indication ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_channel_switch_ind) == 4,
               "AIC channel-switch indication ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_channel_pre_switch_ind) == 1,
               "AIC channel-pre-switch indication ABI changed");
_Static_assert(sizeof(struct aic_wire_apm_set_beacon_ie_req) == 516,
               "AIC beacon-upload ABI changed");
_Static_assert(sizeof(struct aic_wire_me_config_req) == 112,
               "AIC ME configuration ABI changed");
_Static_assert(sizeof(struct aic_wire_me_tx_credits_update_ind) == 3,
               "AIC transmit credit indication ABI changed");
_Static_assert(sizeof(struct aic_wire_me_rc_stats_req) == 1,
               "AIC RC statistics request ABI changed");
_Static_assert(sizeof(struct aic_wire_rc_rate_stats) == 12,
               "AIC RC rate statistics ABI changed");
_Static_assert(sizeof(struct aic_wire_me_rc_stats_cfm) == 200,
               "AIC RC statistics confirmation ABI changed");
_Static_assert(sizeof(struct aic_wire_me_channel_config_req) == 254,
               "AIC channel configuration ABI changed");
_Static_assert(sizeof(struct aic_wire_scanu_start_req) == 376,
               "AIC scan request ABI changed");
_Static_assert(sizeof(struct aic_wire_sm_connect_req) == 320,
               "AIC connect request ABI changed");
_Static_assert(sizeof(struct aic_wire_sm_external_auth_required_ind) == 44,
               "AIC external-auth indication ABI changed");
_Static_assert(sizeof(struct aic_wire_sm_external_auth_required_rsp) == 4,
               "AIC external-auth response ABI changed");
_Static_assert(sizeof(struct aic_wire_tx_host_descriptor) == 28,
               "AIC TX descriptor ABI changed");
_Static_assert(sizeof(struct aic_wire_tx_power_index) == 10,
               "AIC TX-power index ABI changed");
_Static_assert(sizeof(struct aic_wire_tx_power_v2) == 35,
               "AIC TX-power v2 ABI changed");
_Static_assert(sizeof(struct aic_wire_tx_power_v3) == 69,
               "AIC TX-power v3 ABI changed");
_Static_assert(sizeof(struct aic_wire_tx_power_v4) == 95,
               "AIC TX-power v4 ABI changed");
_Static_assert(sizeof(struct aic_wire_tx_power_offset) == 8,
               "AIC TX-power offset ABI changed");
_Static_assert(sizeof(struct aic_wire_tx_power_offset_2x) == 28,
               "AIC TX-power 2x offset ABI changed");
_Static_assert(sizeof(struct aic_wire_tx_power_offset_2x_v2) == 86,
               "AIC TX-power 2x v2 offset ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_set_tx_power_adjust_req) == 10,
               "AIC TX-power adjustment ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_set_rf_config_req) == 260,
               "AIC RF-config request ABI changed");
_Static_assert(sizeof(struct aic_wire_mm_set_rf_calibration_req) == 24,
               "AIC RF calibration ABI changed");

#endif /* __AIC8800_PROTOCOL_H__ */
