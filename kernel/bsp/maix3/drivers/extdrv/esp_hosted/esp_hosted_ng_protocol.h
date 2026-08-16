/*
 * ESP-Hosted-NG wire definitions derived from host/include/adapter.h in
 * ESP-Hosted commit 5acd9ba0eaf186cc340b8dc2e7a12993a4162b93.
 *
 * Copyright 2015-2024 Espressif Systems (Shanghai) PTE LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __ESP_HOSTED_NG_PROTOCOL_H__
#define __ESP_HOSTED_NG_PROTOCOL_H__

#include <rtthread.h>

#define EHF_NG_MAX_SSID_LENGTH 32U
#define EHF_NG_MAX_KEY_LENGTH  32U
#define EHF_NG_MAX_SEQ_LENGTH  10U

enum ehf_ng_interface
{
    EHF_NG_STA_INTERFACE = 0,
    EHF_NG_AP_INTERFACE,
    EHF_NG_HCI_INTERFACE,
    EHF_NG_INTERNAL_INTERFACE,
    EHF_NG_TEST_INTERFACE,
    EHF_NG_INTERFACE_MAX,
};

enum ehf_ng_packet_type
{
    EHF_NG_PACKET_DATA = 0,
    EHF_NG_PACKET_COMMAND_REQUEST,
    EHF_NG_PACKET_COMMAND_RESPONSE,
    EHF_NG_PACKET_EVENT,
    EHF_NG_PACKET_EAPOL,
};

enum ehf_ng_command_code
{
    EHF_NG_CMD_INIT_INTERFACE = 1,
    EHF_NG_CMD_SET_MAC = 2,
    EHF_NG_CMD_GET_MAC = 3,
    EHF_NG_CMD_SCAN = 4,
    EHF_NG_CMD_CONNECT = 5,
    EHF_NG_CMD_DISCONNECT = 6,
    EHF_NG_CMD_DEINIT_INTERFACE = 7,
    EHF_NG_CMD_ADD_KEY = 8,
    EHF_NG_CMD_DELETE_KEY = 9,
    EHF_NG_CMD_SET_DEFAULT_KEY = 10,
    EHF_NG_CMD_AUTH = 11,
    EHF_NG_CMD_ASSOC = 12,
    EHF_NG_CMD_GET_TX_POWER = 15,
    EHF_NG_CMD_SET_TX_POWER = 16,
    EHF_NG_CMD_GET_REG_DOMAIN = 17,
    EHF_NG_CMD_SET_REG_DOMAIN = 18,
    EHF_NG_CMD_SET_MODE = 22,
    EHF_NG_CMD_SET_IE = 23,
    EHF_NG_CMD_AP_CONFIG = 24,
    EHF_NG_CMD_MGMT_TX = 25,
    EHF_NG_CMD_AP_STATION = 26,
    EHF_NG_CMD_GET_RSSI = 27,
};

enum ehf_ng_command_status
{
    EHF_NG_RESPONSE_PENDING = 0,
    EHF_NG_RESPONSE_FAILED,
    EHF_NG_RESPONSE_SUCCESS,
    EHF_NG_RESPONSE_BUSY,
    EHF_NG_RESPONSE_UNSUPPORTED,
    EHF_NG_RESPONSE_INVALID,
};

enum ehf_ng_wifi_mode
{
    EHF_NG_WIFI_MODE_NONE,
    EHF_NG_WIFI_MODE_STA,
    EHF_NG_WIFI_MODE_AP,
    EHF_NG_WIFI_MODE_APSTA,
};

enum ehf_ng_event_code
{
    EHF_NG_EVENT_SCAN_RESULT = 1,
    EHF_NG_EVENT_STA_CONNECT,
    EHF_NG_EVENT_STA_DISCONNECT,
    EHF_NG_EVENT_AUTH_RX,
    EHF_NG_EVENT_ASSOC_RX,
    EHF_NG_EVENT_AP_MGMT_RX,
};

enum ehf_ng_ie_type
{
    EHF_NG_IE_BEACON = 0,
    EHF_NG_IE_PROBE_RESPONSE,
    EHF_NG_IE_ASSOC_RESPONSE,
    EHF_NG_IE_RSN,
    EHF_NG_IE_BEACON_PROBE_HEAD,
    EHF_NG_IE_BEACON_PROBE_TAIL,
};

enum ehf_ng_auth_type
{
    EHF_NG_AUTH_OPEN = 0,
    EHF_NG_AUTH_SHARED = 1,
    EHF_NG_AUTH_FT = 2,
    EHF_NG_AUTH_SAE = 3,
    EHF_NG_AUTH_AUTOMATIC = 8,
};

enum ehf_ng_ap_station_command
{
    EHF_NG_AP_STATION_ADD = 0,
    EHF_NG_AP_STATION_CHANGE,
    EHF_NG_AP_STATION_DELETE,
};

enum ehf_ng_cipher_type
{
    EHF_NG_CIPHER_NONE = 0,
    EHF_NG_CIPHER_WEP40,
    EHF_NG_CIPHER_WEP104,
    EHF_NG_CIPHER_TKIP,
    EHF_NG_CIPHER_CCMP,
    EHF_NG_CIPHER_TKIP_CCMP,
    EHF_NG_CIPHER_AES_CMAC,
    EHF_NG_CIPHER_SMS4,
    EHF_NG_CIPHER_GCMP,
    EHF_NG_CIPHER_GCMP_256,
};

enum ehf_ng_boot_tag
{
    EHF_NG_BOOT_CAPABILITY = 0,
    EHF_NG_BOOT_FIRMWARE_DATA,
    EHF_NG_BOOT_SPI_CLOCK,
    EHF_NG_BOOT_CHIP_ID,
    EHF_NG_BOOT_RAW_TEST,
    EHF_NG_BOOT_RX_BUFFER_SIZE,
};

struct ehf_ng_transport_header
{
    rt_uint8_t interface_number;
    rt_uint8_t flags;
    rt_uint8_t packet_type;
    rt_uint8_t reserved1;
    rt_uint8_t length[2];
    rt_uint8_t offset[2];
    rt_uint8_t checksum[2];
    rt_uint8_t reserved2;
    rt_uint8_t reserved3;
} __attribute__((packed));

struct ehf_ng_command_header
{
    rt_uint8_t command;
    rt_uint8_t status;
    rt_uint8_t length[2];
    rt_uint8_t sequence[2];
    rt_uint8_t reserved1;
    rt_uint8_t reserved2;
} __attribute__((packed));

struct ehf_ng_event_header
{
    rt_uint8_t event;
    rt_uint8_t status;
    rt_uint8_t length[2];
} __attribute__((packed));

struct ehf_ng_scan_body
{
    rt_uint8_t bssid[6];
    rt_uint8_t duration[2];
    char ssid[EHF_NG_MAX_SSID_LENGTH + 1];
    rt_uint8_t channel;
    rt_uint8_t padding[2];
} __attribute__((packed));

struct ehf_ng_mac_body
{
    rt_uint8_t mac[6];
    rt_uint8_t padding[2];
} __attribute__((packed));

struct ehf_ng_ie_body
{
    rt_uint8_t type;
    rt_uint8_t padding;
    rt_uint8_t length[2];
    rt_uint8_t data[];
} __attribute__((packed));

struct ehf_ng_ap_config_body
{
    rt_uint8_t ssid[32];
    rt_uint8_t ssid_length;
    rt_uint8_t channel;
    rt_uint8_t auth_mode;
    rt_uint8_t hidden;
    rt_uint8_t max_connections;
    rt_uint8_t pairwise_cipher;
    rt_uint8_t pmf;
    rt_uint8_t sae_pwe;
    rt_uint8_t beacon_interval[2];
    rt_uint8_t inactivity_timeout[2];
    rt_uint8_t privacy;
} __attribute__((packed));

struct ehf_ng_ap_station_body
{
    rt_uint8_t mac[6];
    rt_uint8_t command[2];
    rt_uint8_t flags_mask[4];
    rt_uint8_t flags_set[4];
    rt_uint8_t modify_mask[4];
    rt_uint8_t listen_interval[4];
    rt_uint8_t aid[2];
    rt_uint8_t extended_capabilities[6];
    rt_uint8_t supported_rates[12];
    rt_uint8_t ht_capabilities[28];
    rt_uint8_t vht_capabilities[14];
    rt_uint8_t padding1[2];
    rt_uint8_t he_capabilities[27];
    rt_uint8_t padding2;
} __attribute__((packed));

struct ehf_ng_auth_body
{
    rt_uint8_t bssid[6];
    rt_uint8_t channel;
    rt_uint8_t auth_type;
    char ssid[EHF_NG_MAX_SSID_LENGTH + 1];
    rt_uint8_t key_length;
    rt_uint8_t key[27];
    rt_uint8_t auth_data_length;
    rt_uint8_t padding[2];
    rt_uint8_t auth_data[];
} __attribute__((packed));

struct ehf_ng_assoc_body
{
    rt_uint8_t ie_length;
    rt_uint8_t padding[3];
    rt_uint8_t ies[];
} __attribute__((packed));

struct ehf_ng_disconnect_body
{
    rt_uint8_t reason[2];
    rt_uint8_t mac[6];
} __attribute__((packed));

struct ehf_ng_security_key
{
    rt_uint8_t algorithm[4];
    rt_uint8_t index[4];
    rt_uint8_t data[EHF_NG_MAX_KEY_LENGTH];
    rt_uint8_t length[4];
    rt_uint8_t mac[6];
    rt_uint8_t sequence[EHF_NG_MAX_SEQ_LENGTH];
    rt_uint8_t sequence_length[4];
    rt_uint8_t delete_key;
    rt_uint8_t set_current;
    rt_uint8_t padding[2];
} __attribute__((packed));

struct ehf_ng_mgmt_body
{
    rt_uint8_t channel;
    rt_uint8_t off_channel;
    rt_uint8_t wait[4];
    rt_uint8_t no_cck;
    rt_uint8_t dont_wait_for_ack;
    rt_uint8_t length[4];
    rt_uint8_t data[];
} __attribute__((packed));

struct ehf_ng_mode_body
{
    rt_uint8_t mode[2];
    rt_uint8_t padding[2];
} __attribute__((packed));

struct ehf_ng_reg_domain_body
{
    char country[4];
} __attribute__((packed));

struct ehf_ng_scan_event
{
    struct ehf_ng_event_header header;
    rt_uint8_t bssid[6];
    rt_uint8_t frame_type;
    rt_uint8_t channel;
    rt_uint8_t rssi[4];
    rt_uint8_t tsf[8];
    rt_uint8_t frame_length[2];
    rt_uint8_t padding[2];
    rt_uint8_t frame[];
} __attribute__((packed));

struct ehf_ng_auth_event
{
    struct ehf_ng_event_header header;
    rt_uint8_t bssid[6];
    rt_uint8_t frame_type;
    rt_uint8_t channel;
    rt_uint8_t rssi[4];
    rt_uint8_t tsf[8];
    rt_uint8_t frame_length[2];
    rt_uint8_t padding[2];
    rt_uint8_t frame[];
} __attribute__((packed));

struct ehf_ng_assoc_event
{
    struct ehf_ng_event_header header;
    rt_uint8_t bssid[6];
    rt_uint8_t frame_type;
    rt_uint8_t channel;
    char ssid[EHF_NG_MAX_SSID_LENGTH + 1];
    rt_uint8_t padding;
    rt_uint8_t frame_length[2];
    rt_uint8_t rssi[4];
    rt_uint8_t tsf[8];
    rt_uint8_t frame[];
} __attribute__((packed));

struct ehf_ng_mgmt_event
{
    struct ehf_ng_event_header header;
    rt_uint8_t noise_floor[4];
    rt_uint8_t rssi[4];
    rt_uint8_t channel[4];
    rt_uint8_t frame_length[4];
    rt_uint8_t frame[];
} __attribute__((packed));

struct ehf_ng_disconnect_event
{
    struct ehf_ng_event_header header;
    rt_uint8_t bssid[6];
    char ssid[EHF_NG_MAX_SSID_LENGTH + 1];
    rt_uint8_t reason;
} __attribute__((packed));

struct ehf_ng_boot_event
{
    struct ehf_ng_event_header header;
    rt_uint8_t length;
    rt_uint8_t padding[3];
    rt_uint8_t data[];
} __attribute__((packed));

#endif /* __ESP_HOSTED_NG_PROTOCOL_H__ */
