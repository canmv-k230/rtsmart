/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_WLAN_OFFLOAD_CONTROL_PROTOCOL_H__
#define __RT_WLAN_OFFLOAD_CONTROL_PROTOCOL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTWO_CTRL_VERSION             2U
#define RTWO_CTRL_MAX_MESSAGE_SIZE    4096U
#define RTWO_CTRL_MAX_SSID_LENGTH     32U
#define RTWO_CTRL_MAX_SCAN_SSIDS      4U
#define RTWO_CTRL_MAX_SCAN_CHANNELS   64U
#define RTWO_CTRL_MAX_IE_LENGTH       1024U
#define RTWO_CTRL_MAX_FRAME_LENGTH    2304U
#define RTWO_CTRL_MAX_EAPOL_LENGTH    1600U
#define RTWO_CTRL_MAX_KEY_LENGTH      64U
#define RTWO_CTRL_MAX_SEQUENCE_LENGTH 16U
/* Largest IEEE 802.11 Key ID: 0-3 data, 4-5 IGTK, 6-7 BIGTK. */
#define RTWO_CTRL_MAX_KEY_INDEX       7U
#define RTWO_CTRL_MAX_DEVICE_NAME     24U

/* Private /dev/wlanctlN wire protocol. It is not an application header. */
#define RTWO_CTRL_FLAG_TRUNCATED       (1U << 0)
#define RTWO_CTRL_FLAG_OVERFLOW        (1U << 1)

#define RTWO_CTRL_CAP_STA                 (1U << 0)
#define RTWO_CTRL_CAP_AP                  (1U << 1)
#define RTWO_CTRL_CAP_STA_AP_CONCURRENT   (1U << 2)
#define RTWO_CTRL_CAP_POWER_SAVE          (1U << 3)
#define RTWO_CTRL_CAP_MONITOR             (1U << 4)
#define RTWO_CTRL_CAP_EXTERNAL_SUPPLICANT (1U << 5)
#define RTWO_CTRL_CAP_HOTPLUG             (1U << 6)
#define RTWO_CTRL_CAP_SAE_OFFLOAD         (1U << 7)
#define RTWO_CTRL_CAP_4WAY_OFFLOAD        (1U << 8)
#define RTWO_CTRL_CAP_EXTERNAL_AUTH       (1U << 9)

enum rtwo_ctrl_iftype
{
    RTWO_CTRL_IFTYPE_STATION = 0,
    RTWO_CTRL_IFTYPE_AP = 1,
};

enum rtwo_ctrl_message_type
{
    RTWO_CTRL_CMD_GET_INFO = 1,
    RTWO_CTRL_CMD_SET_INTERFACE,
    RTWO_CTRL_CMD_SCAN,
    RTWO_CTRL_CMD_ABORT_SCAN,
    RTWO_CTRL_CMD_AUTHENTICATE,
    RTWO_CTRL_CMD_ASSOCIATE,
    RTWO_CTRL_CMD_DISCONNECT,
    RTWO_CTRL_CMD_SET_KEY,
    RTWO_CTRL_CMD_DELETE_KEY,
    RTWO_CTRL_CMD_SET_DEFAULT_KEY,
    RTWO_CTRL_CMD_MGMT_TX,
    RTWO_CTRL_CMD_EAPOL_TX,
    RTWO_CTRL_CMD_EXTERNAL_AUTH_RESPONSE,
    /* Added after version 2 shipped. Older kernels answer -ENOSYS. */
    RTWO_CTRL_CMD_GET_NAMES,

    RTWO_CTRL_EVENT_INFO = 0x8000,
    RTWO_CTRL_EVENT_RADIO_ONLINE,
    RTWO_CTRL_EVENT_RADIO_OFFLINE,
    RTWO_CTRL_EVENT_SCAN_RESULT,
    RTWO_CTRL_EVENT_SCAN_DONE,
    RTWO_CTRL_EVENT_CONNECT_RESULT,
    RTWO_CTRL_EVENT_DISCONNECTED,
    RTWO_CTRL_EVENT_AUTH_RX,
    RTWO_CTRL_EVENT_ASSOC_RX,
    RTWO_CTRL_EVENT_MGMT_RX,
    RTWO_CTRL_EVENT_MGMT_TX_STATUS,
    RTWO_CTRL_EVENT_EAPOL_RX,
    RTWO_CTRL_EVENT_REGULATORY_CHANGED,
    RTWO_CTRL_EVENT_FIRMWARE_ERROR,
    RTWO_CTRL_EVENT_EXTERNAL_AUTH_REQUIRED,
    RTWO_CTRL_EVENT_NAMES,
};

enum rtwo_ctrl_scan_flags
{
    RTWO_CTRL_SCAN_PASSIVE = 1U << 0,
    RTWO_CTRL_SCAN_RANDOM_MAC = 1U << 1,
    RTWO_CTRL_SCAN_FLUSH_CACHE = 1U << 2,
};

enum rtwo_ctrl_band
{
    RTWO_CTRL_BAND_2GHZ = 0,
    RTWO_CTRL_BAND_5GHZ,
    RTWO_CTRL_BAND_6GHZ,
    RTWO_CTRL_BAND_UNSPECIFIED = 0xff,
};

enum rtwo_ctrl_channel_width
{
    RTWO_CTRL_CHANNEL_WIDTH_20_NOHT = 0,
    RTWO_CTRL_CHANNEL_WIDTH_20,
    RTWO_CTRL_CHANNEL_WIDTH_40,
    RTWO_CTRL_CHANNEL_WIDTH_80,
    RTWO_CTRL_CHANNEL_WIDTH_80P80,
    RTWO_CTRL_CHANNEL_WIDTH_160,
    RTWO_CTRL_CHANNEL_WIDTH_320,
};

struct rtwo_ctrl_header
{
    uint16_t version;
    uint16_t type;
    uint32_t length;
    uint32_t request_id;
    int32_t status;
    uint8_t iftype;
    uint8_t flags;
    uint16_t reserved;
};

struct rtwo_ctrl_ssid
{
    uint8_t length;
    uint8_t value[RTWO_CTRL_MAX_SSID_LENGTH];
};

struct rtwo_ctrl_channel
{
    uint16_t primary_frequency_mhz;
    uint16_t center_frequency1_mhz;
    uint16_t center_frequency2_mhz;
    uint16_t primary_channel;
    uint8_t band;
    uint8_t width;
    uint8_t reserved[2];
};

struct rtwo_ctrl_info
{
    uint32_t capabilities;
    uint32_t phy_capabilities;
    uint32_t cipher_mask;
    uint32_t iftype_mask;
    uint32_t max_frame_size;
    uint32_t framework_api_version;
    uint32_t firmware_protocol_version;
    uint32_t firmware_version;
    uint32_t firmware_features;
    uint32_t firmware_generation;
    uint16_t max_scan_ie_length;
    uint16_t max_stations;
    uint8_t max_scan_ssids;
    uint8_t band_mask;
    uint8_t max_vifs;
    uint8_t max_channel_contexts;
    uint8_t address[6];
    uint8_t reserved[2];
};

/*
 * Device names the core assigned to this radio. An empty string means the
 * interface does not exist. radio_index is 0xff when no interface is
 * registered; otherwise control is "wlanctl<radio_index>" and the station and
 * AP devices are "phy<radio_index>-sta" and "phy<radio_index>-ap".
 */
struct rtwo_ctrl_names
{
    char control[RTWO_CTRL_MAX_DEVICE_NAME];
    char station[RTWO_CTRL_MAX_DEVICE_NAME];
    char ap[RTWO_CTRL_MAX_DEVICE_NAME];
    uint8_t radio_index;
    uint8_t reserved[3];
};

struct rtwo_ctrl_set_interface
{
    uint8_t enabled;
    uint8_t reserved[3];
};

struct rtwo_ctrl_scan_request
{
    uint32_t flags;
    uint16_t duration_ms;
    uint8_t ssid_count;
    uint8_t channel_count;
    uint16_t ies_length;
    uint8_t bssid[6];
    struct rtwo_ctrl_ssid ssids[RTWO_CTRL_MAX_SCAN_SSIDS];
    struct rtwo_ctrl_channel channels[RTWO_CTRL_MAX_SCAN_CHANNELS];
    uint8_t ies[RTWO_CTRL_MAX_IE_LENGTH];
};

struct rtwo_ctrl_auth_request
{
    struct rtwo_ctrl_ssid ssid;
    uint8_t bssid[6];
    struct rtwo_ctrl_channel channel;
    uint8_t auth_type;
    uint8_t reserved[3];
    uint16_t data_length;
    uint8_t data[RTWO_CTRL_MAX_IE_LENGTH];
};

struct rtwo_ctrl_assoc_request
{
    uint8_t bssid[6];
    uint16_t ies_length;
    uint8_t ies[RTWO_CTRL_MAX_IE_LENGTH];
};

struct rtwo_ctrl_disconnect_request
{
    uint16_t reason;
    uint16_t reserved;
};

struct rtwo_ctrl_key_request
{
    uint32_t cipher;
    uint8_t index;
    uint8_t pairwise;
    uint8_t set_transmit;
    uint8_t key_length;
    uint8_t peer[6];
    uint8_t sequence_length;
    uint8_t reserved;
    uint8_t key[RTWO_CTRL_MAX_KEY_LENGTH];
    uint8_t sequence[RTWO_CTRL_MAX_SEQUENCE_LENGTH];
};

struct rtwo_ctrl_delete_key_request
{
    uint8_t index;
    uint8_t pairwise;
    uint8_t peer[6];
};

struct rtwo_ctrl_default_key_request
{
    uint8_t index;
    uint8_t unicast;
    uint8_t multicast;
    uint8_t reserved;
};

struct rtwo_ctrl_mgmt_frame
{
    struct rtwo_ctrl_channel channel;
    uint8_t off_channel;
    uint8_t reserved[3];
    uint32_t wait_ms;
    uint64_t cookie;
    uint16_t data_length;
    uint8_t data[RTWO_CTRL_MAX_FRAME_LENGTH];
};

struct rtwo_ctrl_eapol_frame
{
    uint8_t source[6];
    uint8_t destination[6];
    uint16_t data_length;
    uint8_t data[RTWO_CTRL_MAX_EAPOL_LENGTH];
};

struct rtwo_ctrl_network
{
    struct rtwo_ctrl_ssid ssid;
    uint8_t bssid[6];
    struct rtwo_ctrl_channel channel;
    int16_t rssi;
    uint32_t security;
    uint16_t beacon_interval;
    uint16_t capability;
    uint16_t ies_length;
    uint8_t ies[RTWO_CTRL_MAX_IE_LENGTH];
};

struct rtwo_ctrl_disconnected
{
    uint8_t bssid[6];
    uint16_t reason;
    uint8_t locally_generated;
    uint8_t reserved;
};

struct rtwo_ctrl_rx_frame
{
    struct rtwo_ctrl_channel channel;
    int16_t rssi;
    uint16_t data_length;
    uint8_t data[RTWO_CTRL_MAX_FRAME_LENGTH];
};

struct rtwo_ctrl_tx_status
{
    uint64_t cookie;
    uint8_t acknowledged;
    uint8_t reserved;
    uint16_t data_length;
    uint8_t data[RTWO_CTRL_MAX_FRAME_LENGTH];
};

struct rtwo_ctrl_firmware_error
{
    uint32_t reason;
    uint16_t dump_length;
    uint8_t dump[RTWO_CTRL_MAX_IE_LENGTH];
};

struct rtwo_ctrl_external_auth
{
    struct rtwo_ctrl_ssid ssid;
    uint8_t bssid[6];
    uint8_t reserved;
    uint32_t akm_suite;
};

struct rtwo_ctrl_external_auth_response
{
    uint16_t status;
    uint16_t reserved;
};

#ifdef __cplusplus
}
#endif

#endif /* __RT_WLAN_OFFLOAD_CONTROL_PROTOCOL_H__ */
