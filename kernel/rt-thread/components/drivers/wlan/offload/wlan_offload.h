/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_WLAN_OFFLOAD_H__
#define __RT_WLAN_OFFLOAD_H__

#include <wlan_offload_bus.h>
#include <ipc/workqueue.h>
#include <wlan_dev.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_WLAN_OFFLOAD_API_VERSION             3

#define RT_WLAN_OFFLOAD_CAP_STA                 (1U << 0)
#define RT_WLAN_OFFLOAD_CAP_AP                  (1U << 1)
#define RT_WLAN_OFFLOAD_CAP_STA_AP_CONCURRENT   (1U << 2)
#define RT_WLAN_OFFLOAD_CAP_POWER_SAVE          (1U << 3)
#define RT_WLAN_OFFLOAD_CAP_MONITOR             (1U << 4)
#define RT_WLAN_OFFLOAD_CAP_EXTERNAL_SUPPLICANT (1U << 5)
#define RT_WLAN_OFFLOAD_CAP_HOTPLUG             (1U << 6)
#define RT_WLAN_OFFLOAD_CAP_SAE_OFFLOAD         (1U << 7)
#define RT_WLAN_OFFLOAD_CAP_4WAY_OFFLOAD        (1U << 8)
#define RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTH       (1U << 9)
#define RT_WLAN_OFFLOAD_CAP_EXTERNAL_AUTHENTICATOR (1U << 10)

#define RT_WLAN_OFFLOAD_PHY_11B                 (1U << 0)
#define RT_WLAN_OFFLOAD_PHY_11G                 (1U << 1)
#define RT_WLAN_OFFLOAD_PHY_11A                 (1U << 2)
#define RT_WLAN_OFFLOAD_PHY_HT                  (1U << 3)
#define RT_WLAN_OFFLOAD_PHY_VHT                 (1U << 4)
#define RT_WLAN_OFFLOAD_PHY_HE                  (1U << 5)
#define RT_WLAN_OFFLOAD_PHY_EHT                 (1U << 6)

#define RT_WLAN_OFFLOAD_CHANNEL_DISABLED        (1U << 0)
#define RT_WLAN_OFFLOAD_CHANNEL_NO_IR           (1U << 1)
#define RT_WLAN_OFFLOAD_CHANNEL_RADAR            (1U << 2)
#define RT_WLAN_OFFLOAD_CHANNEL_INDOOR_ONLY      (1U << 3)
#define RT_WLAN_OFFLOAD_CHANNEL_NO_HT40_PLUS     (1U << 4)
#define RT_WLAN_OFFLOAD_CHANNEL_NO_HT40_MINUS    (1U << 5)
#define RT_WLAN_OFFLOAD_CHANNEL_NO_80MHZ         (1U << 6)
#define RT_WLAN_OFFLOAD_CHANNEL_NO_160MHZ        (1U << 7)
#define RT_WLAN_OFFLOAD_CHANNEL_NO_320MHZ        (1U << 8)

#define RT_WLAN_OFFLOAD_REG_NO_IR                (1U << 0)
#define RT_WLAN_OFFLOAD_REG_DFS                  (1U << 1)
#define RT_WLAN_OFFLOAD_REG_INDOOR_ONLY          (1U << 2)
#define RT_WLAN_OFFLOAD_REG_NO_OUTDOOR           (1U << 3)

#define RT_WLAN_OFFLOAD_SCAN_PASSIVE             (1U << 0)
#define RT_WLAN_OFFLOAD_SCAN_RANDOM_MAC          (1U << 1)
#define RT_WLAN_OFFLOAD_SCAN_FLUSH_CACHE         (1U << 2)

#define RT_WLAN_OFFLOAD_IFTYPE_BIT(_type)        (1U << (_type))
#define RT_WLAN_OFFLOAD_MAX_KEY_LENGTH           64
#define RT_WLAN_OFFLOAD_MAX_SEQUENCE_LENGTH      16
#define RT_WLAN_OFFLOAD_WLAN_VIF_COUNT           2
#define RT_WLAN_OFFLOAD_VIF_STA_INDEX             0
#define RT_WLAN_OFFLOAD_VIF_AP_INDEX              1
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
#define RT_WLAN_OFFLOAD_BSS_CACHE_COUNT          64
#define RT_WLAN_OFFLOAD_BSS_SECURITY_IE_MAX_LENGTH 514
#endif

enum rt_wlan_offload_iftype
{
    RT_WLAN_OFFLOAD_IFTYPE_STATION = 0,
    RT_WLAN_OFFLOAD_IFTYPE_AP,
    RT_WLAN_OFFLOAD_IFTYPE_MONITOR,
    RT_WLAN_OFFLOAD_IFTYPE_P2P_CLIENT,
    RT_WLAN_OFFLOAD_IFTYPE_P2P_GO,
    RT_WLAN_OFFLOAD_IFTYPE_MAX,
};

enum rt_wlan_offload_band_id
{
    RT_WLAN_OFFLOAD_BAND_2GHZ = 0,
    RT_WLAN_OFFLOAD_BAND_5GHZ,
    RT_WLAN_OFFLOAD_BAND_6GHZ,
    RT_WLAN_OFFLOAD_BAND_MAX,
};

enum rt_wlan_offload_channel_width
{
    RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20_NOHT = 0,
    RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20,
    RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40,
    RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80,
    RT_WLAN_OFFLOAD_CHANNEL_WIDTH_80P80,
    RT_WLAN_OFFLOAD_CHANNEL_WIDTH_160,
    RT_WLAN_OFFLOAD_CHANNEL_WIDTH_320,
    RT_WLAN_OFFLOAD_CHANNEL_WIDTH_MAX,
};

enum rt_wlan_offload_state
{
    RT_WLAN_OFFLOAD_UNREGISTERED = 0,
    RT_WLAN_OFFLOAD_REGISTERED,
    RT_WLAN_OFFLOAD_STARTING,
    RT_WLAN_OFFLOAD_STARTED,
    RT_WLAN_OFFLOAD_OFFLINE,
    RT_WLAN_OFFLOAD_FAILED,
};

enum rt_wlan_offload_auth_type
{
    RT_WLAN_OFFLOAD_AUTH_OPEN = 0,
    RT_WLAN_OFFLOAD_AUTH_SHARED,
    RT_WLAN_OFFLOAD_AUTH_FT,
    RT_WLAN_OFFLOAD_AUTH_SAE,
    RT_WLAN_OFFLOAD_AUTH_AUTOMATIC,
};

enum rt_wlan_offload_cipher
{
    RT_WLAN_OFFLOAD_CIPHER_NONE = 0,
    RT_WLAN_OFFLOAD_CIPHER_WEP40,
    RT_WLAN_OFFLOAD_CIPHER_WEP104,
    RT_WLAN_OFFLOAD_CIPHER_TKIP,
    RT_WLAN_OFFLOAD_CIPHER_CCMP,
    RT_WLAN_OFFLOAD_CIPHER_CCMP_256,
    RT_WLAN_OFFLOAD_CIPHER_GCMP,
    RT_WLAN_OFFLOAD_CIPHER_GCMP_256,
    RT_WLAN_OFFLOAD_CIPHER_AES_CMAC,
};

enum rt_wlan_offload_dfs_region
{
    RT_WLAN_OFFLOAD_DFS_UNSET = 0,
    RT_WLAN_OFFLOAD_DFS_FCC,
    RT_WLAN_OFFLOAD_DFS_ETSI,
    RT_WLAN_OFFLOAD_DFS_JP,
};

enum rt_wlan_offload_event_type
{
    RT_WLAN_OFFLOAD_EVENT_RADIO_ONLINE = 0,
    RT_WLAN_OFFLOAD_EVENT_RADIO_OFFLINE,
    RT_WLAN_OFFLOAD_EVENT_SCAN_RESULT,
    RT_WLAN_OFFLOAD_EVENT_SCAN_DONE,
    RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT,
    RT_WLAN_OFFLOAD_EVENT_DISCONNECTED,
    RT_WLAN_OFFLOAD_EVENT_AP_STARTED,
    RT_WLAN_OFFLOAD_EVENT_AP_STOPPED,
    RT_WLAN_OFFLOAD_EVENT_NEW_STATION,
    RT_WLAN_OFFLOAD_EVENT_DEL_STATION,
    RT_WLAN_OFFLOAD_EVENT_AUTH_RX,
    RT_WLAN_OFFLOAD_EVENT_ASSOC_RX,
    RT_WLAN_OFFLOAD_EVENT_MGMT_RX,
    RT_WLAN_OFFLOAD_EVENT_MGMT_TX_STATUS,
    RT_WLAN_OFFLOAD_EVENT_EAPOL_RX,
    RT_WLAN_OFFLOAD_EVENT_REGULATORY_CHANGED,
    RT_WLAN_OFFLOAD_EVENT_EXTERNAL_AUTH_REQUIRED,
    RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR,
};

struct rt_wlan_offload_radio;
struct rt_wlan_offload_control;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
struct rt_wlan_offload_supplicant;
#endif
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
struct rt_wlan_offload_hostapd;
#endif

/* Runtime values reported by the device firmware, not build-time promises. */
struct rt_wlan_offload_firmware_info
{
    rt_uint32_t protocol_version;
    rt_uint32_t firmware_version;
    rt_uint32_t features;
    rt_uint16_t max_stations;
    rt_uint8_t max_vifs;
    rt_uint8_t max_channel_contexts;
};

struct rt_wlan_offload_channel
{
    enum rt_wlan_offload_band_id band;
    rt_uint16_t number;
    rt_uint16_t center_frequency_mhz;
    rt_uint32_t flags;
    rt_int8_t max_power_dbm;
};

struct rt_wlan_offload_channel_definition
{
    enum rt_wlan_offload_band_id band;
    enum rt_wlan_offload_channel_width width;
    rt_uint16_t primary_channel;
    rt_uint16_t primary_frequency_mhz;
    rt_uint16_t center_frequency1_mhz;
    rt_uint16_t center_frequency2_mhz;
};

struct rt_wlan_offload_rate
{
    rt_uint32_t bitrate_100kbps;
    rt_uint16_t hardware_value;
    rt_uint16_t flags;
};

struct rt_wlan_offload_supported_band
{
    enum rt_wlan_offload_band_id id;
    rt_uint32_t phy_capabilities;
    const struct rt_wlan_offload_channel *channels;
    rt_size_t channel_count;
    const struct rt_wlan_offload_rate *rates;
    rt_size_t rate_count;
    rt_uint8_t max_spatial_streams;
    /* Widest channel this radio can operate on this band.  Leave zero (which
     * is CHANNEL_WIDTH_20) only if the radio really is 20 MHz only; the rate
     * reporting treats an unset value as "no limit" via max_channel_width_set
     * so that existing drivers keep their behaviour. */
    enum rt_wlan_offload_channel_width max_channel_width;
    rt_bool_t max_channel_width_set;
};

struct rt_wlan_offload_iface_limit
{
    rt_uint32_t iftypes;
    rt_uint8_t maximum;
};

struct rt_wlan_offload_iface_combination
{
    const struct rt_wlan_offload_iface_limit *limits;
    rt_size_t limit_count;
    rt_uint8_t max_interfaces;
    rt_uint8_t num_different_channels;
};

struct rt_wlan_offload_regulatory_rule
{
    rt_uint32_t start_frequency_khz;
    rt_uint32_t end_frequency_khz;
    rt_uint32_t max_bandwidth_khz;
    rt_int16_t max_eirp_mbm;
    rt_uint32_t flags;
};

struct rt_wlan_offload_regulatory_domain
{
    char alpha2[2];
    enum rt_wlan_offload_dfs_region dfs_region;
    const struct rt_wlan_offload_regulatory_rule *rules;
    rt_size_t rule_count;
};

struct rt_wlan_offload_scan_ssid
{
    rt_uint8_t length;
    rt_uint8_t value[RT_WLAN_SSID_MAX_LENGTH];
};

struct rt_wlan_offload_scan_request
{
    rt_uint32_t request_id;
    const struct rt_wlan_offload_scan_ssid *ssids;
    rt_size_t ssid_count;
    rt_uint8_t bssid[6];
    const struct rt_wlan_offload_channel_definition *channels;
    rt_size_t channel_count;
    rt_uint32_t flags;
    rt_uint16_t duration_ms;
    const rt_uint8_t *ies;
    rt_size_t ies_length;
};

struct rt_wlan_offload_connect_request
{
    rt_uint32_t request_id;
    rt_wlan_ssid_t ssid;
    rt_wlan_key_t key;
    rt_uint8_t bssid[6];
    struct rt_wlan_offload_channel_definition channel;
    rt_wlan_security_t security;
    const rt_uint8_t *ies;
    rt_size_t ies_length;
};

struct rt_wlan_offload_ap_settings
{
    rt_uint32_t request_id;
    rt_wlan_ssid_t ssid;
    rt_wlan_key_t key;
    struct rt_wlan_offload_channel_definition channel;
    rt_wlan_security_t security;
    rt_bool_t hidden;
    rt_uint16_t beacon_interval;
    rt_uint8_t max_stations;
    const rt_uint8_t *beacon_ies;
    rt_size_t beacon_ies_length;
};

struct rt_wlan_offload_key
{
    enum rt_wlan_offload_cipher cipher;
    rt_uint8_t index;
    rt_bool_t pairwise;
    rt_bool_t set_transmit;
    rt_uint8_t peer[6];
    rt_uint8_t key[RT_WLAN_OFFLOAD_MAX_KEY_LENGTH];
    rt_uint8_t key_length;
    rt_uint8_t sequence[RT_WLAN_OFFLOAD_MAX_SEQUENCE_LENGTH];
    rt_uint8_t sequence_length;
};

struct rt_wlan_offload_auth_request
{
    rt_uint32_t request_id;
    rt_wlan_ssid_t ssid;
    rt_uint8_t bssid[6];
    struct rt_wlan_offload_channel_definition channel;
    enum rt_wlan_offload_auth_type auth_type;
    const rt_uint8_t *auth_data;
    rt_size_t auth_data_length;
};

struct rt_wlan_offload_assoc_request
{
    rt_uint32_t request_id;
    rt_uint8_t bssid[6];
    const rt_uint8_t *ies;
    rt_size_t ies_length;
};

struct rt_wlan_offload_mgmt_frame
{
    rt_uint32_t request_id;
    struct rt_wlan_offload_channel_definition channel;
    rt_bool_t off_channel;
    rt_uint32_t wait_ms;
    rt_uint64_t cookie;
    const rt_uint8_t *data;
    rt_size_t length;
};

struct rt_wlan_offload_network
{
    rt_wlan_ssid_t ssid;
    rt_uint8_t bssid[6];
    struct rt_wlan_offload_channel_definition channel;
    rt_int16_t rssi;
    rt_wlan_security_t security;
    rt_uint16_t beacon_interval;
    rt_uint16_t capability;
    const rt_uint8_t *ies;
    rt_size_t ies_length;
};

#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
/* rt_wlan_info does not carry the WPA/RSN elements needed for association. */
struct rt_wlan_offload_bss_cache_entry
{
    rt_bool_t valid;
    rt_wlan_ssid_t ssid;
    rt_uint8_t bssid[6];
    struct rt_wlan_offload_channel_definition channel;
    rt_int16_t rssi;
    rt_wlan_security_t security;
    rt_uint16_t beacon_interval;
    rt_uint16_t capability;
    rt_uint16_t security_ies_length;
    rt_uint8_t security_ies[RT_WLAN_OFFLOAD_BSS_SECURITY_IE_MAX_LENGTH];
};
#endif

struct rt_wlan_offload_station
{
    rt_uint8_t mac[6];
    rt_int16_t rssi;
    rt_uint16_t aid;
};

struct rt_wlan_offload_station_parameters
{
    rt_uint8_t mac[6];
    rt_uint16_t aid;
    const rt_uint8_t *association_ies;
    rt_size_t association_ies_length;
};

struct rt_wlan_offload_event
{
    enum rt_wlan_offload_event_type type;
    enum rt_wlan_offload_iftype iftype;
    rt_uint32_t request_id;
    rt_err_t status;
    union
    {
        struct rt_wlan_offload_network network;
        struct rt_wlan_offload_station station;
        struct
        {
            rt_uint8_t bssid[6];
            rt_uint16_t reason;
            rt_bool_t locally_generated;
        } disconnected;
        struct
        {
            struct rt_wlan_offload_channel_definition channel;
            rt_int16_t rssi;
            const rt_uint8_t *data;
            rt_size_t length;
        } management;
        struct
        {
            rt_uint64_t cookie;
            rt_bool_t acknowledged;
            const rt_uint8_t *data;
            rt_size_t length;
        } tx_status;
        struct
        {
            rt_uint8_t source[6];
            rt_uint8_t destination[6];
            const rt_uint8_t *data;
            rt_size_t length;
        } eapol;
        struct
        {
            rt_country_code_t country;
            char alpha2[2];
        } regulatory;
        struct
        {
            rt_wlan_ssid_t ssid;
            rt_uint8_t bssid[6];
            rt_uint32_t akm_suite;
        } external_auth;
        struct
        {
            rt_uint32_t reason;
            const void *dump;
            rt_size_t dump_length;
        } firmware;
    } data;
};

struct rt_wlan_offload_vif
{
    /* Must remain first: the RT-Thread WLAN adapter casts wlan to vif. */
    struct rt_wlan_device wlan;
    struct rt_wlan_offload_radio *radio;
    enum rt_wlan_offload_iftype iftype;
    rt_bool_t registered;
    rt_bool_t enabled;
    rt_bool_t link_up;
    rt_bool_t promiscuous;
    rt_bool_t management_filter;
    rt_uint32_t pending_scan_id;
    rt_uint32_t pending_connect_id;
    rt_uint32_t pending_ap_id;
    rt_uint8_t address[6];
};

typedef void (*rt_wlan_offload_event_handler_t)(struct rt_wlan_offload_radio *radio,
                                           const struct rt_wlan_offload_event *event,
                                           void *parameter);

struct rt_wlan_offload_ops
{
    /*
     * Request storage is borrowed for the duration of each callback. The
     * driver must copy anything needed after the callback returns.
     */
    rt_err_t (*start)(struct rt_wlan_offload_radio *radio);
    rt_err_t (*stop)(struct rt_wlan_offload_radio *radio);
    rt_err_t (*change_interface)(struct rt_wlan_offload_vif *vif,
                                 enum rt_wlan_offload_iftype iftype,
                                 rt_bool_t enabled);
    rt_err_t (*scan)(struct rt_wlan_offload_vif *vif,
                     const struct rt_wlan_offload_scan_request *request);
    rt_err_t (*connect)(struct rt_wlan_offload_vif *vif,
                        const struct rt_wlan_offload_connect_request *request);
    rt_err_t (*disconnect)(struct rt_wlan_offload_vif *vif,
                           rt_uint32_t request_id, rt_uint16_t reason);
    rt_err_t (*start_ap)(struct rt_wlan_offload_vif *vif,
                         const struct rt_wlan_offload_ap_settings *settings);
    rt_err_t (*stop_ap)(struct rt_wlan_offload_vif *vif, rt_uint32_t request_id);
    rt_err_t (*del_station)(struct rt_wlan_offload_vif *vif,
                            rt_uint32_t request_id, const rt_uint8_t mac[6],
                            rt_uint16_t reason);
    rt_err_t (*add_station)(struct rt_wlan_offload_vif *vif,
                            rt_uint32_t request_id,
                            const struct rt_wlan_offload_station_parameters *station);
    rt_err_t (*set_station_authorized)(struct rt_wlan_offload_vif *vif,
                                       rt_uint32_t request_id,
                                       const rt_uint8_t mac[6],
                                       rt_bool_t authorized);
    rt_err_t (*abort_scan)(struct rt_wlan_offload_vif *vif, rt_uint32_t request_id);
    rt_err_t (*get_rssi)(struct rt_wlan_offload_vif *vif, int *rssi);
    rt_err_t (*set_power_save)(struct rt_wlan_offload_vif *vif, int level);
    rt_err_t (*get_power_save)(struct rt_wlan_offload_vif *vif, int *level);
    rt_err_t (*set_promiscuous)(struct rt_wlan_offload_vif *vif, rt_bool_t enabled);
    rt_err_t (*set_filter)(struct rt_wlan_offload_vif *vif,
                           struct rt_wlan_filter *filter);
    rt_err_t (*set_mgmt_filter)(struct rt_wlan_offload_vif *vif, rt_bool_t enabled);
    rt_err_t (*set_channel)(
        struct rt_wlan_offload_vif *vif,
        const struct rt_wlan_offload_channel_definition *channel);
    rt_err_t (*get_channel)(
        struct rt_wlan_offload_vif *vif,
        struct rt_wlan_offload_channel_definition *channel);
    rt_err_t (*set_regulatory)(struct rt_wlan_offload_radio *radio,
                               rt_country_code_t country);
    rt_err_t (*get_regulatory)(struct rt_wlan_offload_radio *radio,
                               rt_country_code_t *country);
    rt_err_t (*set_mac)(struct rt_wlan_offload_vif *vif, rt_uint8_t mac[6]);
    rt_err_t (*get_mac)(struct rt_wlan_offload_vif *vif, rt_uint8_t mac[6]);
    rt_err_t (*transmit)(struct rt_wlan_offload_vif *vif,
                         const void *data, int length);
    rt_err_t (*transmit_raw)(struct rt_wlan_offload_vif *vif,
                             const void *data, int length);

    /* Host-supplicant operations used by cfg80211-style firmware. */
    rt_err_t (*auth)(struct rt_wlan_offload_vif *vif,
                     const struct rt_wlan_offload_auth_request *request);
    rt_err_t (*assoc)(struct rt_wlan_offload_vif *vif,
                      const struct rt_wlan_offload_assoc_request *request);
    rt_err_t (*add_key)(struct rt_wlan_offload_vif *vif,
                        rt_uint32_t request_id,
                        const struct rt_wlan_offload_key *key);
    rt_err_t (*delete_key)(struct rt_wlan_offload_vif *vif,
                           rt_uint32_t request_id, rt_uint8_t index,
                           rt_bool_t pairwise, const rt_uint8_t peer[6]);
    rt_err_t (*set_default_key)(struct rt_wlan_offload_vif *vif,
                                rt_uint32_t request_id, rt_uint8_t index,
                                rt_bool_t unicast, rt_bool_t multicast);
    rt_err_t (*transmit_mgmt)(struct rt_wlan_offload_vif *vif,
                              const struct rt_wlan_offload_mgmt_frame *frame);
    rt_err_t (*external_auth_response)(struct rt_wlan_offload_vif *vif,
                                       rt_uint16_t status);
};

struct rt_wlan_offload_radio_config
{
    /* Metadata arrays remain owned by the driver until unregister returns. */
    rt_uint32_t api_version;
    /* Deprecated: STA/AP names are assigned by the WLAN core. */
    const char *station_name;
    const char *ap_name;
    /* Expose the userspace control device. The core assigns /dev/wlanctlN. */
    rt_bool_t control_device;
    /* Deprecated: overrides the assigned control device name. */
    const char *control_name;
    const struct rt_wlan_offload_ops *ops;
    struct rt_wlan_offload_bus *bus;
    rt_uint32_t capabilities;
    rt_size_t max_frame_size;
    const struct rt_wlan_offload_supported_band *bands[RT_WLAN_OFFLOAD_BAND_MAX];
    const enum rt_wlan_offload_cipher *cipher_suites;
    rt_size_t cipher_suite_count;
    const struct rt_wlan_offload_iface_combination *iface_combinations;
    rt_size_t iface_combination_count;
    const struct rt_wlan_offload_regulatory_domain *regulatory_domain;
    rt_uint8_t permanent_address[6];
    rt_uint8_t max_scan_ssids;
    rt_uint16_t max_scan_ie_length;
    struct rt_wlan_offload_firmware_info firmware_info;
    void *driver_data;
};

struct rt_wlan_offload_radio
{
    struct rt_wlan_offload_vif vifs[RT_WLAN_OFFLOAD_WLAN_VIF_COUNT];
    const struct rt_wlan_offload_ops *ops;
    struct rt_wlan_offload_bus *bus;
    rt_uint32_t capabilities;
    rt_size_t max_frame_size;
    const struct rt_wlan_offload_supported_band *bands[RT_WLAN_OFFLOAD_BAND_MAX];
    const enum rt_wlan_offload_cipher *cipher_suites;
    rt_size_t cipher_suite_count;
    const struct rt_wlan_offload_iface_combination *iface_combinations;
    rt_size_t iface_combination_count;
    const struct rt_wlan_offload_regulatory_domain *regulatory_domain;
    rt_uint8_t permanent_address[6];
    rt_uint8_t max_scan_ssids;
    rt_uint16_t max_scan_ie_length;
    struct rt_wlan_offload_firmware_info firmware_info;
    rt_uint32_t firmware_generation;
    void *driver_data;
    struct rt_mutex command_lock;
    struct rt_mutex data_lock;
    struct rt_mutex operation_lock;
    enum rt_wlan_offload_state state;
    rt_uint32_t request_sequence;
    rt_wlan_offload_event_handler_t event_handler;
    void *event_parameter;
    struct rt_wlan_offload_control *control;
    struct rt_work recovery_work;
    rt_bool_t recovery_work_initialized;
    rt_bool_t recovery_queued;
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
    struct rt_wlan_offload_bss_cache_entry
        bss_cache[RT_WLAN_OFFLOAD_BSS_CACHE_COUNT];
    struct rt_wlan_offload_supplicant *supplicant;
#endif
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_HOSTAPD
    struct rt_wlan_offload_hostapd *hostapd;
#endif
};

rt_err_t rt_wlan_offload_register_radio(struct rt_wlan_offload_radio *radio,
                                   const struct rt_wlan_offload_radio_config *config);
rt_err_t rt_wlan_offload_unregister_radio(struct rt_wlan_offload_radio *radio);
rt_err_t rt_wlan_offload_set_radio_online(struct rt_wlan_offload_radio *radio,
                                     rt_bool_t online);
rt_uint32_t rt_wlan_offload_alloc_request_id(struct rt_wlan_offload_radio *radio);
rt_err_t rt_wlan_offload_update_firmware_info(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_firmware_info *info);
rt_err_t rt_wlan_offload_get_firmware_info(
    struct rt_wlan_offload_radio *radio,
    struct rt_wlan_offload_firmware_info *info,
    rt_uint32_t *generation);

struct rt_wlan_offload_vif *rt_wlan_offload_get_vif(struct rt_wlan_offload_radio *radio,
                                          enum rt_wlan_offload_iftype iftype);
struct rt_wlan_device *rt_wlan_offload_get_wlan(struct rt_wlan_offload_radio *radio,
                                           enum rt_wlan_offload_iftype iftype);
void *rt_wlan_offload_get_driver_data(struct rt_wlan_offload_radio *radio);

/* RX and event payload storage is borrowed for the duration of each call. */
rt_err_t rt_wlan_offload_rx(struct rt_wlan_offload_radio *radio,
                       enum rt_wlan_offload_iftype iftype,
                       const void *data, int length);
rt_err_t rt_wlan_offload_report_event(struct rt_wlan_offload_radio *radio,
                                 const struct rt_wlan_offload_event *event);
void rt_wlan_offload_set_event_handler(struct rt_wlan_offload_radio *radio,
                                  rt_wlan_offload_event_handler_t handler,
                                  void *parameter);

rt_err_t rt_wlan_offload_change_interface(struct rt_wlan_offload_radio *radio,
                                      enum rt_wlan_offload_iftype iftype,
                                      rt_bool_t enabled);
rt_err_t rt_wlan_offload_scan(struct rt_wlan_offload_radio *radio,
                         enum rt_wlan_offload_iftype iftype,
                         const struct rt_wlan_offload_scan_request *request);
rt_err_t rt_wlan_offload_abort_scan(struct rt_wlan_offload_radio *radio,
                               enum rt_wlan_offload_iftype iftype,
                               rt_uint32_t request_id);
rt_err_t rt_wlan_offload_connect(struct rt_wlan_offload_radio *radio,
                            enum rt_wlan_offload_iftype iftype,
                            const struct rt_wlan_offload_connect_request *request);
rt_err_t rt_wlan_offload_disconnect(struct rt_wlan_offload_radio *radio,
                               enum rt_wlan_offload_iftype iftype,
                               rt_uint32_t request_id, rt_uint16_t reason);
rt_err_t rt_wlan_offload_start_ap(struct rt_wlan_offload_radio *radio,
                             enum rt_wlan_offload_iftype iftype,
                             const struct rt_wlan_offload_ap_settings *settings);
rt_err_t rt_wlan_offload_stop_ap(struct rt_wlan_offload_radio *radio,
                            enum rt_wlan_offload_iftype iftype,
                            rt_uint32_t request_id);
rt_err_t rt_wlan_offload_ap_channel_changed(
    struct rt_wlan_offload_radio *radio,
    const struct rt_wlan_offload_channel_definition *channel);
rt_err_t rt_wlan_offload_del_station(struct rt_wlan_offload_radio *radio,
                                enum rt_wlan_offload_iftype iftype,
                                rt_uint32_t request_id,
                                const rt_uint8_t mac[6], rt_uint16_t reason);
rt_err_t rt_wlan_offload_add_station(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    rt_uint32_t request_id,
    const struct rt_wlan_offload_station_parameters *station);
rt_err_t rt_wlan_offload_set_station_authorized(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    rt_uint32_t request_id, const rt_uint8_t mac[6], rt_bool_t authorized);

rt_err_t rt_wlan_offload_auth(struct rt_wlan_offload_radio *radio,
                         enum rt_wlan_offload_iftype iftype,
                         const struct rt_wlan_offload_auth_request *request);
rt_err_t rt_wlan_offload_assoc(struct rt_wlan_offload_radio *radio,
                          enum rt_wlan_offload_iftype iftype,
                          const struct rt_wlan_offload_assoc_request *request);
rt_err_t rt_wlan_offload_add_key(struct rt_wlan_offload_radio *radio,
                            enum rt_wlan_offload_iftype iftype,
                            rt_uint32_t request_id,
                            const struct rt_wlan_offload_key *key);
rt_err_t rt_wlan_offload_delete_key(struct rt_wlan_offload_radio *radio,
                               enum rt_wlan_offload_iftype iftype,
                               rt_uint32_t request_id, rt_uint8_t index,
                               rt_bool_t pairwise, const rt_uint8_t peer[6]);
rt_err_t rt_wlan_offload_set_default_key(struct rt_wlan_offload_radio *radio,
                                    enum rt_wlan_offload_iftype iftype,
                                    rt_uint32_t request_id, rt_uint8_t index,
                                    rt_bool_t unicast, rt_bool_t multicast);
rt_err_t rt_wlan_offload_transmit_mgmt(struct rt_wlan_offload_radio *radio,
                                  enum rt_wlan_offload_iftype iftype,
                                  const struct rt_wlan_offload_mgmt_frame *frame);
rt_err_t rt_wlan_offload_external_auth_response(
    struct rt_wlan_offload_radio *radio,
    enum rt_wlan_offload_iftype iftype,
    rt_uint16_t status);
rt_err_t rt_wlan_offload_transmit_eapol(struct rt_wlan_offload_radio *radio,
                                   enum rt_wlan_offload_iftype iftype,
                                   const rt_uint8_t destination[6],
                                   const void *data, rt_size_t length);

#ifdef __cplusplus
}
#endif

#endif /* __RT_WLAN_OFFLOAD_H__ */
