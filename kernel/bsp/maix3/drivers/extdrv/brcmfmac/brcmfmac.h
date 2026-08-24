// SPDX-License-Identifier: ISC
/*
 * RT-Smart native brcmfmac port.
 *
 * Protocol definitions and driver organization are derived from Linux 6.6.36
 * brcmfmac, Copyright (c) 2010-2014 Broadcom Corporation.
 */
#ifndef __RTSMART_BRCMFMAC_H__
#define __RTSMART_BRCMFMAC_H__

#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/sdio.h>
#include <ipc/completion.h>
#include <wlan_offload.h>

#define BRCMF_SDIO_VENDOR_BROADCOM       0x02d0U
#define BRCMF_MAX_CORES                  32U
#define BRCMF_MAX_VIFS                    4U
#define BRCMF_MAX_IFS                    16U
#define BRCMF_MAX_COMMAND              8192U
#define BRCMF_MAX_FRAME                2048U
#define BRCMF_RX_BUFFER_SIZE          65536U
#define BRCMF_MAX_RX_GLOM_FRAMES        32U
#define BRCMF_EVENTING_MASK_LEN          18U
#define BRCMF_ETH_ALEN                    6U
#define BRCMF_SSID_MAX_LENGTH            32U
#define BRCMF_IFNAME_MAX_LENGTH          16U
#define BRCMF_WSEC_MAX_SAE_PASSWORD     128U
#define BRCMF_MAX_AP_IE_LENGTH           512U
#define BRCMF_2GHZ_CHANNEL_COUNT          14U
#define BRCMF_5GHZ_CHANNEL_COUNT          29U

#ifndef BRCMFMAC_FIRMWARE_PATH
#define BRCMFMAC_FIRMWARE_PATH "/bin/firmware/brcmfmac"
#endif
#ifndef BRCMFMAC_COUNTRY_CODE
#define BRCMFMAC_COUNTRY_CODE "CN"
#endif
#ifndef BRCMFMAC_COUNTRY_REVISION
#define BRCMFMAC_COUNTRY_REVISION 0
#endif
#ifndef BRCMFMAC_BCM43430A1_NVRAM
#define BRCMFMAC_BCM43430A1_NVRAM "nvram.txt"
#endif
#define BRCMFMAC_SDIO_F1_BLOCK_SIZE     64U
#define BRCMFMAC_SDIO_F2_BLOCK_SIZE    512U
#ifndef BRCMFMAC_INIT_THREAD_STACK_SIZE
#define BRCMFMAC_INIT_THREAD_STACK_SIZE 12288U
#endif
#ifndef BRCMFMAC_INIT_THREAD_PRIORITY
#define BRCMFMAC_INIT_THREAD_PRIORITY 14U
#endif
#ifndef BRCMFMAC_RX_THREAD_PRIORITY
#define BRCMFMAC_RX_THREAD_PRIORITY 11U
#endif
#ifndef BRCMFMAC_RX_THREAD_STACK_SIZE
#define BRCMFMAC_RX_THREAD_STACK_SIZE 8192U
#endif
#ifndef BRCMFMAC_TX_THREAD_PRIORITY
#define BRCMFMAC_TX_THREAD_PRIORITY 12U
#endif
#ifndef BRCMFMAC_TX_THREAD_STACK_SIZE
#define BRCMFMAC_TX_THREAD_STACK_SIZE 4096U
#endif
#if defined(RT_LWIP_TCPTHREAD_PRIORITY) && \
    BRCMFMAC_RX_THREAD_PRIORITY <= RT_LWIP_TCPTHREAD_PRIORITY
#error "brcmfmac RX priority must be lower than lwIP (numerically greater)"
#endif
#if BRCMFMAC_TX_THREAD_PRIORITY <= BRCMFMAC_RX_THREAD_PRIORITY
#error "brcmfmac TX priority must be lower than RX (numerically greater)"
#endif
#if BRCMFMAC_INIT_THREAD_PRIORITY <= BRCMFMAC_TX_THREAD_PRIORITY
#error "brcmfmac init priority must be lower than TX (numerically greater)"
#endif
#ifndef BRCMFMAC_TX_QUEUE_DEPTH
#define BRCMFMAC_TX_QUEUE_DEPTH 256U
#endif
#ifndef BRCMFMAC_TX_QUEUE_WAIT_MS
#define BRCMFMAC_TX_QUEUE_WAIT_MS 1000U
#endif
#ifndef BRCMFMAC_TX_GLOM_FRAMES
#define BRCMFMAC_TX_GLOM_FRAMES 32U
#endif
#ifndef BRCMFMAC_TX_GLOM_WAIT_MS
#define BRCMFMAC_TX_GLOM_WAIT_MS 1U
#endif
#ifndef BRCMFMAC_TX_RETRIES
#define BRCMFMAC_TX_RETRIES 2U
#endif
#ifndef BRCMFMAC_SDIO_WATCHDOG_MS
#define BRCMFMAC_SDIO_WATCHDOG_MS 10U
#endif
#ifndef BRCMFMAC_TX_CREDIT_TIMEOUT_MS
#define BRCMFMAC_TX_CREDIT_TIMEOUT_MS 2000U
#endif
#ifndef BRCMFMAC_TX_CREDIT_POLL_MS
#define BRCMFMAC_TX_CREDIT_POLL_MS 2U
#endif
#ifndef BRCMFMAC_CONTROL_TIMEOUT_MS
#define BRCMFMAC_CONTROL_TIMEOUT_MS 5000U
#endif
#ifndef BRCMFMAC_CONNECT_TIMEOUT_MS
#define BRCMFMAC_CONNECT_TIMEOUT_MS 9000U
#endif
#define BRCMF_PACKED __attribute__((packed))

static inline rt_uint16_t brcmf_get_le16(const void *pointer)
{
    const rt_uint8_t *data = pointer;
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8);
}

static inline rt_uint32_t brcmf_get_le32(const void *pointer)
{
    const rt_uint8_t *data = pointer;
    return (rt_uint32_t)data[0] | ((rt_uint32_t)data[1] << 8) |
           ((rt_uint32_t)data[2] << 16) | ((rt_uint32_t)data[3] << 24);
}

static inline rt_uint16_t brcmf_get_be16(const void *pointer)
{
    const rt_uint8_t *data = pointer;
    return ((rt_uint16_t)data[0] << 8) | data[1];
}

static inline rt_uint32_t brcmf_get_be32(const void *pointer)
{
    const rt_uint8_t *data = pointer;
    return ((rt_uint32_t)data[0] << 24) | ((rt_uint32_t)data[1] << 16) |
           ((rt_uint32_t)data[2] << 8) | data[3];
}

static inline void brcmf_put_le16(void *pointer, rt_uint16_t value)
{
    rt_uint8_t *data = pointer;
    data[0] = value;
    data[1] = value >> 8;
}

static inline void brcmf_put_le32(void *pointer, rt_uint32_t value)
{
    rt_uint8_t *data = pointer;
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

enum brcmf_bus_channel
{
    BRCMF_BUS_CHANNEL_CONTROL = 0,
    BRCMF_BUS_CHANNEL_EVENT = 1,
    BRCMF_BUS_CHANNEL_DATA = 2,
};

enum brcmf_core_id
{
    BRCMF_CORE_CHIPCOMMON = 0x800,
    BRCMF_CORE_80211 = 0x812,
    BRCMF_CORE_INTERNAL_MEM = 0x80e,
    BRCMF_CORE_SDIO_DEV = 0x829,
    BRCMF_CORE_ARM_CM3 = 0x82a,
    BRCMF_CORE_ARM_CR4 = 0x83e,
    BRCMF_CORE_ARM_CA7 = 0x847,
    BRCMF_CORE_SYS_MEM = 0x849,
    BRCMF_CORE_PMU = 0x827,
    BRCMF_CORE_GCI = 0x840,
};

struct brcmf_core
{
    rt_uint16_t id;
    rt_uint8_t revision;
    rt_uint32_t base;
    rt_uint32_t wrapbase;
};

struct brcmf_chip
{
    rt_uint32_t id;
    rt_uint32_t revision;
    rt_uint32_t enum_base;
    rt_uint32_t rambase;
    rt_uint32_t ramsize;
    rt_uint32_t srsize;
    rt_uint32_t cc_caps;
    rt_uint32_t cc_caps_ext;
    rt_uint32_t pmu_caps;
    rt_uint32_t pmu_revision;
    struct brcmf_core cores[BRCMF_MAX_CORES];
    rt_uint8_t core_count;
};

struct brcmf_firmware_mapping
{
    rt_uint16_t vendor;
    rt_uint16_t device;
    rt_uint32_t chip;
    rt_uint32_t revision_mask;
    const char *model;
    const char *firmware;
    const char *nvram;
    const char *clm;
};

struct brcmf_context
{
    struct rt_wlan_offload_radio radio;
    struct rt_wlan_offload_bus bus;
    struct rt_mmcsd_card *card;
    struct rt_sdio_function *function1;
    struct rt_sdio_function *function2;
    const struct brcmf_firmware_mapping *mapping;
    struct brcmf_chip chip;
    struct rt_mutex io_mutex;
    struct rt_mutex command_mutex;
    struct rt_completion command_completion;
    struct rt_completion worker_stopped;
    struct rt_completion tx_worker_stopped;
    struct rt_completion ap_interface_completion;
    struct rt_work ap_resume_work;
    struct rt_work connect_cleanup_work;
    struct rt_work auto_start_work;
    rt_sem_t rx_sem;
    rt_sem_t tx_sem;
    rt_mp_t tx_pool;
    rt_mq_t tx_queue;
    rt_thread_t rx_thread;
    rt_thread_t tx_thread;
    rt_uint8_t *rx_buffer;
    rt_uint8_t *tx_buffer;
    rt_uint8_t *command_buffer;
    rt_size_t command_length;
    rt_err_t command_status;
    rt_uint32_t command_request;
    rt_uint32_t scan_request;
    volatile rt_uint32_t connect_request;
    rt_uint32_t ap_request;
    volatile rt_tick_t connect_started;
    rt_uint32_t ap_settings_generation;
    rt_uint32_t backplane_window;
    volatile rt_uint8_t tx_sequence;
    volatile rt_uint8_t tx_max;
    volatile rt_uint8_t tx_flow_control;
    rt_uint8_t rx_sequence;
    rt_uint8_t rx_glom_count;
    rt_uint16_t rx_next_length;
    rt_uint16_t rx_glom_length[BRCMF_MAX_RX_GLOM_FRAMES];
    rt_uint16_t f2_block_size;
    rt_uint32_t tx_transfer_count;
    rt_uint32_t tx_frame_count;
    rt_uint32_t tx_aggregate_count;
    rt_uint32_t tx_error_count;
    rt_uint32_t tx_retry_count;
    rt_uint32_t tx_drop_count;
    rt_uint32_t tx_credit_wait_count;
    rt_uint32_t tx_credit_stall_count;
    rt_uint32_t tx_invalid_credit_count;
    rt_uint32_t tx_flow_control_count;
    rt_uint32_t rx_recovery_count;
    rt_uint32_t rx_retry_count;
    rt_uint32_t rx_watchdog_count;
    rt_uint32_t rx_empty_poll_count;
    rt_uint32_t tx_recovery_count;
    rt_uint32_t hostmail_count;
    rt_uint32_t irq_f1_count;
    rt_uint32_t irq_f2_count;
    rt_uint32_t data_tx_sta_count;
    rt_uint32_t data_tx_ap_count;
    rt_uint32_t data_rx_sta_count;
    rt_uint32_t data_rx_ap_count;
    rt_uint32_t data_rx_unknown_count;
    rt_uint32_t data_rx_drop_count;
    rt_uint16_t tx_queue_high_water;
    rt_uint8_t tx_max_aggregate;
    rt_uint8_t mac[BRCMF_ETH_ALEN];
    rt_uint8_t ap_beacon_ies[BRCMF_MAX_AP_IE_LENGTH];
    struct rt_wlan_offload_ap_settings ap_settings;
    struct rt_wlan_offload_ap_settings suspended_ap_settings;
    struct rt_wlan_offload_channel_definition connect_channel;
    struct rt_wlan_offload_channel channels_2ghz[BRCMF_2GHZ_CHANNEL_COUNT];
    struct rt_wlan_offload_channel channels_5ghz[BRCMF_5GHZ_CHANNEL_COUNT];
    struct rt_wlan_offload_supported_band band_2ghz;
    struct rt_wlan_offload_supported_band band_5ghz;
    rt_uint8_t sta_interface;
    rt_uint8_t ap_interface;
    rt_uint8_t ap_bsscfg;
    rt_uint8_t io_type;
    rt_country_code_t country;
    rt_bool_t scan_active;
    rt_bool_t connect_assoc_seen;
    rt_bool_t connect_psk_seen;
    rt_bool_t connect_secure;
    rt_bool_t station_power_save;
    rt_bool_t sae_supported;
    rt_bool_t ap_settings_valid;
    rt_bool_t ap_resume_work_initialized;
    rt_bool_t connect_cleanup_work_initialized;
    rt_bool_t auto_start_work_initialized;
    rt_bool_t bus_initialized;
    rt_bool_t radio_registered;
    rt_bool_t tearing_down;
    volatile rt_bool_t rx_pending;
    volatile rt_bool_t rx_skip;
    rt_bool_t firmware_running;
    rt_bool_t irq_f1_attached;
    rt_bool_t irq_f2_attached;
    volatile rt_bool_t irq_deferred;
    rt_bool_t worker_started;
    rt_bool_t tx_worker_started;
    rt_bool_t tx_glom;
    volatile rt_bool_t tx_flow_control_state;
    volatile rt_bool_t control_waiting;
    volatile rt_bool_t ap_interface_pending;
    volatile rt_bool_t ap_interface_created;
    volatile rt_bool_t ap_suspended_for_connect;
    volatile rt_bool_t ap_resume_work_queued;
    volatile rt_bool_t connect_cleanup_work_queued;
    volatile rt_bool_t auto_start_work_queued;
    volatile rt_err_t deferred_connect_status;
    volatile rt_bool_t worker_running;
    volatile rt_bool_t tx_worker_running;
};

struct brcmf_bus_record
{
    rt_uint8_t channel;
    rt_uint8_t interface_index;
    rt_uint16_t length;
    rt_uint8_t payload[];
} BRCMF_PACKED;

struct brcmf_bcdc_dcmd
{
    rt_uint32_t command;
    rt_uint32_t length;
    rt_uint32_t flags;
    rt_uint32_t status;
    rt_uint8_t data[];
} BRCMF_PACKED;

struct brcmf_bcdc_header
{
    rt_uint8_t flags;
    rt_uint8_t priority;
    rt_uint8_t flags2;
    rt_uint8_t data_offset;
} BRCMF_PACKED;

struct brcmf_eth_header
{
    rt_uint8_t destination[BRCMF_ETH_ALEN];
    rt_uint8_t source[BRCMF_ETH_ALEN];
    rt_uint16_t type;
} BRCMF_PACKED;

struct brcmf_event_vendor_header
{
    rt_uint16_t subtype;
    rt_uint16_t length;
    rt_uint8_t version;
    rt_uint8_t oui[3];
    rt_uint16_t user_subtype;
} BRCMF_PACKED;

struct brcmf_event_message
{
    rt_uint16_t version;
    rt_uint16_t flags;
    rt_uint32_t event_type;
    rt_uint32_t status;
    rt_uint32_t reason;
    rt_uint32_t auth_type;
    rt_uint32_t data_length;
    rt_uint8_t address[BRCMF_ETH_ALEN];
    char interface_name[BRCMF_IFNAME_MAX_LENGTH];
    rt_uint8_t interface_index;
    rt_uint8_t bsscfg_index;
} BRCMF_PACKED;

struct brcmf_event_packet
{
    struct brcmf_eth_header ethernet;
    struct brcmf_event_vendor_header vendor;
    struct brcmf_event_message message;
    rt_uint8_t data[];
} BRCMF_PACKED;

struct brcmf_interface_event
{
    rt_uint8_t interface_index;
    rt_uint8_t action;
    rt_uint8_t flags;
    rt_uint8_t bsscfg_index;
    rt_uint8_t role;
} BRCMF_PACKED;

struct brcmf_ssid
{
    rt_uint32_t length;
    rt_uint8_t value[BRCMF_SSID_MAX_LENGTH];
} BRCMF_PACKED;

struct brcmf_scan_params
{
    struct brcmf_ssid ssid;
    rt_uint8_t bssid[BRCMF_ETH_ALEN];
    rt_int8_t bss_type;
    rt_uint8_t scan_type;
    rt_int32_t probes;
    rt_int32_t active_time;
    rt_int32_t passive_time;
    rt_int32_t home_time;
    rt_uint32_t channel_count;
    rt_uint16_t channels[1];
} BRCMF_PACKED;

struct brcmf_escan_params
{
    rt_uint32_t version;
    rt_uint16_t action;
    rt_uint16_t sync_id;
    struct brcmf_scan_params params;
} BRCMF_PACKED;

struct brcmf_bss_info
{
    rt_uint32_t version;
    rt_uint32_t length;
    rt_uint8_t bssid[BRCMF_ETH_ALEN];
    rt_uint16_t beacon_period;
    rt_uint16_t capability;
    rt_uint8_t ssid_length;
    rt_uint8_t ssid[BRCMF_SSID_MAX_LENGTH];
    rt_uint8_t beacon_flags;
    rt_uint32_t rates_count;
    rt_uint8_t rates[16];
    rt_uint16_t chanspec;
    rt_uint16_t atim_window;
    rt_uint8_t dtim_period;
    rt_uint8_t access_network;
    rt_int16_t rssi;
    rt_int8_t noise;
    rt_uint8_t n_capability;
    rt_uint8_t reserved1[2];
    rt_uint32_t nbss_capability;
    rt_uint8_t control_channel;
    rt_uint8_t reserved_control[3];
    rt_uint32_t reserved;
    rt_uint8_t flags;
    rt_uint8_t reserved2[3];
    rt_uint8_t basic_mcs[16];
    rt_uint16_t ie_offset;
    rt_uint16_t reserved3;
    rt_uint32_t ie_length;
    rt_int16_t snr;
} BRCMF_PACKED;

_Static_assert(offsetof(struct brcmf_bss_info, rates_count) == 52U,
               "brcmf BSS rates offset mismatch");
_Static_assert(offsetof(struct brcmf_bss_info, rssi) == 78U,
               "brcmf BSS RSSI offset mismatch");
_Static_assert(offsetof(struct brcmf_bss_info, ie_offset) == 116U,
               "brcmf BSS IE offset mismatch");
_Static_assert(offsetof(struct brcmf_bss_info, ie_length) == 120U,
               "brcmf BSS IE length offset mismatch");
_Static_assert(sizeof(struct brcmf_bss_info) == 126U,
               "brcmf BSS fixed size mismatch");

struct brcmf_escan_result
{
    rt_uint32_t buffer_length;
    rt_uint32_t version;
    rt_uint16_t sync_id;
    rt_uint16_t bss_count;
    struct brcmf_bss_info bss;
} BRCMF_PACKED;

struct brcmf_assoc_params
{
    rt_uint8_t bssid[BRCMF_ETH_ALEN];
    rt_uint16_t padding;
    rt_uint32_t chanspec_count;
    rt_uint16_t chanspec[1];
} BRCMF_PACKED;

struct brcmf_join_scan_params
{
    rt_uint8_t scan_type;
    rt_uint8_t padding[3];
    rt_int32_t probes;
    rt_int32_t active_time;
    rt_int32_t passive_time;
    rt_int32_t home_time;
} BRCMF_PACKED;

struct brcmf_join_params
{
    struct brcmf_ssid ssid;
    struct brcmf_assoc_params assoc;
} BRCMF_PACKED;

struct brcmf_ext_join_params
{
    struct brcmf_ssid ssid;
    struct brcmf_join_scan_params scan;
    struct brcmf_assoc_params assoc;
} BRCMF_PACKED;

struct brcmf_wsec_pmk
{
    rt_uint16_t key_length;
    rt_uint16_t flags;
    rt_uint8_t key[BRCMF_WSEC_MAX_SAE_PASSWORD];
} BRCMF_PACKED;

struct brcmf_wsec_sae_password
{
    rt_uint16_t key_length;
    rt_uint8_t key[BRCMF_WSEC_MAX_SAE_PASSWORD];
} BRCMF_PACKED;

struct brcmf_scb_value
{
    rt_uint32_t value;
    rt_uint8_t address[BRCMF_ETH_ALEN];
} BRCMF_PACKED;

struct brcmf_bss_enable
{
    rt_uint32_t bsscfg_index;
    rt_uint32_t enable;
} BRCMF_PACKED;

enum brcmf_event_code
{
    BRCMF_E_SET_SSID = 0,
    BRCMF_E_AUTH = 3,
    BRCMF_E_DEAUTH = 5,
    BRCMF_E_DEAUTH_IND = 6,
    BRCMF_E_ASSOC = 7,
    BRCMF_E_ASSOC_IND = 8,
    BRCMF_E_REASSOC = 9,
    BRCMF_E_REASSOC_IND = 10,
    BRCMF_E_DISASSOC_IND = 12,
    BRCMF_E_LINK = 16,
    BRCMF_E_PRUNE = 23,
    BRCMF_E_PSK_SUP = 46,
    BRCMF_E_IF = 54,
    BRCMF_E_AP_STARTED = 64,
    BRCMF_E_ESCAN_RESULT = 69,
};

#define BRCMF_E_STATUS_SUCCESS          0U
#define BRCMF_E_STATUS_NO_NETWORKS      3U
#define BRCMF_E_STATUS_UNSOLICITED      6U
#define BRCMF_E_STATUS_ATTEMPT          7U
#define BRCMF_E_STATUS_PARTIAL          8U
#define BRCMF_E_STATUS_FWSUP_COMPLETED  6U
#define BRCMF_EVENT_MSG_LINK            1U
#define BRCMF_E_IF_ADD                  1U
#define BRCMF_E_IF_DEL                  2U
#define BRCMF_E_IF_CHANGE               3U
#define BRCMF_E_IF_FLAG_NOIF            1U
#define BRCMF_E_IF_ROLE_STA             0U
#define BRCMF_E_IF_ROLE_AP              1U

#define BRCMF_C_GET_VERSION              1U
#define BRCMF_C_UP                       2U
#define BRCMF_C_DOWN                     3U
#define BRCMF_C_SET_PROMISC             10U
#define BRCMF_C_GET_RATE                12U
#define BRCMF_C_SET_INFRA               20U
#define BRCMF_C_SET_AUTH                22U
#define BRCMF_C_GET_BSSID               23U
#define BRCMF_C_SET_SSID                26U
#define BRCMF_C_GET_CHANNEL             29U
#define BRCMF_C_SET_CHANNEL             30U
#define BRCMF_C_DISASSOC                52U
#define BRCMF_C_SET_ANTDIV              64U
#define BRCMF_C_SET_BCNPRD              76U
#define BRCMF_C_SET_DTIMPRD             78U
#define BRCMF_C_SET_COUNTRY             84U
#define BRCMF_C_GET_PM                  85U
#define BRCMF_C_SET_PM                  86U
#define BRCMF_C_SET_GMODE              110U
#define BRCMF_C_GET_AP                 117U
#define BRCMF_C_SET_AP                 118U
#define BRCMF_C_GET_RSSI               127U
#define BRCMF_C_SET_WSEC               134U
#define BRCMF_C_SET_SCB_TIMEOUT        158U
#define BRCMF_C_SET_WPA_AUTH           165U
#define BRCMF_C_SCB_DEAUTH_REASON      201U
#define BRCMF_C_GET_FAKEFRAG           218U
#define BRCMF_C_SET_FAKEFRAG           219U
#define BRCMF_C_GET_VAR                262U
#define BRCMF_C_SET_VAR                263U
#define BRCMF_C_SET_WSEC_PMK           268U

#define BRCMF_WSEC_WEP                  0x0001U
#define BRCMF_WSEC_TKIP                 0x0002U
#define BRCMF_WSEC_AES                  0x0004U
#define BRCMF_WPA_AUTH_DISABLED         0x0000U
#define BRCMF_WPA_AUTH_PSK              0x0004U
#define BRCMF_WPA2_AUTH_PSK             0x0080U
#define BRCMF_WPA2_AUTH_PSK_SHA256      0x8000U
#define BRCMF_WSEC_PASSPHRASE           0x0001U

const struct brcmf_firmware_mapping *brcmf_firmware_find(
    rt_uint16_t vendor, rt_uint16_t device, rt_uint32_t chip,
    rt_uint32_t revision);
rt_err_t brcmf_chip_attach(struct brcmf_context *context);
struct brcmf_core *brcmf_chip_get_core(struct brcmf_chip *chip,
                                       rt_uint16_t id);
rt_err_t brcmf_chip_set_passive(struct brcmf_context *context);
rt_err_t brcmf_chip_set_active(struct brcmf_context *context,
                               rt_uint32_t reset_vector);

rt_err_t brcmf_sdio_backplane_read(struct brcmf_context *context,
                                   rt_uint32_t address, void *data,
                                   rt_size_t length);
rt_err_t brcmf_sdio_backplane_write(struct brcmf_context *context,
                                    rt_uint32_t address, const void *data,
                                    rt_size_t length);
rt_uint32_t brcmf_sdio_read32(struct brcmf_context *context,
                              rt_uint32_t address, rt_err_t *error);
rt_err_t brcmf_sdio_write32(struct brcmf_context *context,
                            rt_uint32_t address, rt_uint32_t value);
rt_err_t brcmf_firmware_load(const char *name, rt_uint8_t **data,
                             rt_size_t *length, rt_bool_t required);

rt_err_t brcmf_proto_start(struct brcmf_context *context);
void brcmf_proto_stop(struct brcmf_context *context);
rt_err_t brcmf_proto_receive(struct rt_wlan_offload_bus *bus,
                             const void *data, rt_size_t length,
                             void *parameter);
rt_err_t brcmf_proto_command(struct brcmf_context *context,
                             rt_uint8_t interface_index, rt_uint32_t command,
                             void *data, rt_size_t length, rt_bool_t set);
rt_err_t brcmf_proto_iovar(struct brcmf_context *context,
                           rt_uint8_t interface_index, const char *name,
                           void *data, rt_size_t length, rt_bool_t set);
rt_err_t brcmf_proto_iovar_int(struct brcmf_context *context,
                               rt_uint8_t interface_index, const char *name,
                               rt_uint32_t *value, rt_bool_t set);
rt_err_t brcmf_proto_bsscfg_iovar(struct brcmf_context *context,
                                  rt_uint8_t interface_index,
                                  rt_uint32_t bsscfg_index, const char *name,
                                  void *data, rt_size_t length, rt_bool_t set);

rt_err_t brcmf_wifi_attach(struct brcmf_context *context);
#ifdef RT_WLAN_MANAGE_ENABLE
void brcmf_wifi_auto_start(struct brcmf_context *context);
#endif
rt_err_t brcmf_wifi_detach(struct brcmf_context *context);
void brcmf_wifi_watchdog(struct brcmf_context *context);
void brcmf_wifi_handle_event(struct brcmf_context *context,
                             const struct brcmf_event_packet *packet,
                             rt_size_t length);

#endif /* __RTSMART_BRCMFMAC_H__ */
