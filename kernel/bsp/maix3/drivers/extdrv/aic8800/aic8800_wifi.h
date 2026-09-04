/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __AIC8800_WIFI_H__
#define __AIC8800_WIFI_H__

#include <wlan_offload.h>
#include <wlan_offload_command.h>
#include <ipc/completion.h>
#include <ipc/workqueue.h>
#ifdef AIC8800_WIFI_TRANSPORT_USB
#include <usbh_core.h>
#endif
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
#include <drivers/sdio.h>
#endif

#include "aic8800_protocol.h"

#if defined(AIC8800_WIFI_LOG_LEVEL_DEBUG)
#define AIC8800_DBG_LVL DBG_LOG
#elif defined(AIC8800_WIFI_LOG_LEVEL_INFO)
#define AIC8800_DBG_LVL DBG_INFO
#elif defined(AIC8800_WIFI_LOG_LEVEL_WARNING)
#define AIC8800_DBG_LVL DBG_WARNING
#elif defined(AIC8800_WIFI_LOG_LEVEL_ERROR)
#define AIC8800_DBG_LVL DBG_ERROR
#else
#define AIC8800_DBG_LVL DBG_INFO
#endif

#ifdef AIC8800_WIFI_DEBUG_STATS
#define AIC8800_STAT(statement) ((void)(statement))
#else
#define AIC8800_STAT(statement) ((void)0)
#endif

#define AIC8800_USB_VENDOR_ID             0xa69c
#define AIC8800_USB_VENDOR_ID_V2          0x368b

#define AIC8800_USB_PID_AIC8800           0x8800
#define AIC8800_USB_PID_AIC8801           0x8801
#define AIC8800_USB_PID_AIC8800DC         0x88dc
#define AIC8800_USB_PID_AIC8800DW         0x88dd
#define AIC8800_USB_PID_AIC8800D80        0x8d80
#define AIC8800_USB_PID_AIC8800D81        0x8d81
/* Further D80/D81 runtime identities. The boot device is 0x8d80 for all of
 * them; which one comes back after the firmware download depends on the
 * module. The vendor Windows INF binds the same driver to every one:
 * "AIC8800D80 USB WiFi", "Wifi6 802.11ax USB Adapter", "Ugreen WIFI6". */
#define AIC8800_USB_PID_AIC8800D83        0x8d83
#define AIC8800_USB_PID_AIC8800D84        0x8d84
#define AIC8800_USB_PID_AIC8800D85        0x8d85
#define AIC8800_USB_PID_AIC8800D86        0x8d86
#define AIC8800_USB_PID_AIC8800D88        0x8d88
#define AIC8800_USB_PID_AIC8800D40        0x8d40
#define AIC8800_USB_PID_AIC8800D41        0x8d41
#define AIC8800_USB_PID_AIC8800D80X2      0x8d90
#define AIC8800_USB_PID_AIC8800D81X2      0x8d91
#define AIC8800_USB_PID_AIC8800D89X2      0x8d99

#define AIC8800_SDIO_VENDOR_AIC8801        0x5449
#define AIC8800_SDIO_PRODUCT_AIC8801       0x0145
#define AIC8800_SDIO_VENDOR_AIC8800DC       0xc8a1
#define AIC8800_SDIO_PRODUCT_AIC8800DC      0xc08d
#define AIC8800_SDIO_PRODUCT_AIC8800DC_MSG  0xc18d
#define AIC8800_SDIO_VENDOR_AIC8800D80     0xc8a1
#define AIC8800_SDIO_PRODUCT_AIC8800D80    0x0082

#define AIC8800_USB_HEADER_SIZE            4U
#define AIC8800_USB_RX_HEADER_SIZE        60U
#define AIC8800_USB_MAX_COMMAND_SIZE    1536U
#define AIC8800_ETHERNET_FRAME_MAX      1588U
#define AIC8800_USB_DMA_ALIGNMENT          64U
#ifndef AIC8800_WIFI_DATA_RX_URBS
#define AIC8800_WIFI_DATA_RX_URBS       20U
#endif
/* Extra DMA buffers per RX URB so giveback can keep the pipe armed
 * across a second high-speed refill before the worker returns the
 * first wave.  These are not URBs. */
#ifndef AIC8800_WIFI_RX_SPARES_PER_SLOT
#define AIC8800_WIFI_RX_SPARES_PER_SLOT  4U
#endif
#ifndef AIC8800_WIFI_MESSAGE_RX_URBS
#define AIC8800_WIFI_MESSAGE_RX_URBS    20U
#endif
#ifndef AIC8800_WIFI_DATA_TX_URBS
#define AIC8800_WIFI_DATA_TX_URBS         2U
#endif
#ifndef AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES
#define AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES 10U
#endif
#ifndef AIC8800_WIFI_USB_TX_AGGREGATE_SIZE
#define AIC8800_WIFI_USB_TX_AGGREGATE_SIZE 16384U
#endif
#ifndef AIC8800_WIFI_USB_TX_QUEUE_DEPTH
#define AIC8800_WIFI_USB_TX_QUEUE_DEPTH  64U
#endif
#define AIC8800_USB_TX_TOKEN_INDEX_MAX  256U
#ifndef AIC8800_WIFI_USB_TX_PRIORITY_RESERVE
#define AIC8800_WIFI_USB_TX_PRIORITY_RESERVE 4U
#endif
#ifndef AIC8800_WIFI_USB_TX_AGGREGATE_WAIT_MS
#define AIC8800_WIFI_USB_TX_AGGREGATE_WAIT_MS 1U
#endif
#ifndef AIC8800_WIFI_USB_TX_THREAD_STACK_SIZE
#define AIC8800_WIFI_USB_TX_THREAD_STACK_SIZE 4096U
#endif
#ifndef AIC8800_WIFI_USB_TX_THREAD_PRIORITY
#define AIC8800_WIFI_USB_TX_THREAD_PRIORITY 14U
#endif
/* The transmit queue thread runs above RT_SYSTEM_WORKQUEUE_PRIORITY, which is
 * where URB cancellation and endpoint recovery live.  Retrying a failing
 * endpoint without sleeping therefore starves the recovery that would clear the
 * fault, so back off for at least one tick between failures. */
#ifndef AIC8800_WIFI_USB_TX_ERROR_BACKOFF_MS
#define AIC8800_WIFI_USB_TX_ERROR_BACKOFF_MS 2U
#endif
#ifndef AIC8800_WIFI_TX_BUFFER_SIZE
#define AIC8800_WIFI_TX_BUFFER_SIZE    2048U
#endif
/* Covers every station index the firmware may hand out, including the per-VIF
 * broadcast/multicast pseudo-stations, which sit above the real ones (33 has
 * been observed for VIF 1). */
#define AIC8800_STATION_SLOTS              40U
/* Diagnostic watermark for records outstanding to one associated SoftAP
 * client.  The transport queue/firmware flow control owns the actual bound;
 * this value must not make lwIP's direct transmit path discard a video burst. */
#define AIC8800_TX_PENDING_HIGH_WATER      64U
/* Matches the vendor's NX_TXQ_INITIAL_CREDITS. */
#define AIC8800_TX_INITIAL_CREDITS         64

#if defined(AIC8800_WIFI_USB_TX_AGGREGATION) && \
    AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES > 1U
#define AIC8800_WIFI_USB_TX_TRANSFER_SIZE \
    AIC8800_WIFI_USB_TX_AGGREGATE_SIZE
#else
#define AIC8800_WIFI_USB_TX_TRANSFER_SIZE AIC8800_WIFI_TX_BUFFER_SIZE
#endif
#ifndef AIC8800_WIFI_TX_WAIT_MS
#define AIC8800_WIFI_TX_WAIT_MS          100U
#endif
#ifndef AIC8800_WIFI_TX_QUEUE_WAIT_MS
#define AIC8800_WIFI_TX_QUEUE_WAIT_MS (AIC8800_WIFI_TX_WAIT_MS * 3U)
#endif
#ifndef AIC8800_WIFI_RX_RECOVERY_ERRORS
#define AIC8800_WIFI_RX_RECOVERY_ERRORS    64U
#endif
#ifndef AIC8800_WIFI_ATTACH_THREAD_STACK_SIZE
#define AIC8800_WIFI_ATTACH_THREAD_STACK_SIZE 12288U
#endif
#ifndef AIC8800_WIFI_ATTACH_THREAD_PRIORITY
#define AIC8800_WIFI_ATTACH_THREAD_PRIORITY   14U
#endif
#ifndef AIC8800_WIFI_SDIO_RX_BUFFER_SIZE
#define AIC8800_WIFI_SDIO_RX_BUFFER_SIZE   65536U
#endif
#define AIC8800_WIFI_SDIO_MAX_AMSDU_SIZE   11454U
#define AIC8800_WIFI_SDIO_MAX_RECORD_SIZE \
    ((AIC8800_WIFI_SDIO_MAX_AMSDU_SIZE + AIC8800_USB_RX_HEADER_SIZE + 3U) & \
     ~3U)
#ifndef AIC8800_WIFI_SDIO_RX_RECOVERY_ERRORS
#define AIC8800_WIFI_SDIO_RX_RECOVERY_ERRORS 64U
#endif
#ifndef AIC8800_WIFI_SDIO_RX_QUEUE_DEPTH
#define AIC8800_WIFI_SDIO_RX_QUEUE_DEPTH     256U
#endif
#ifndef AIC8800_WIFI_SDIO_RX_LARGE_QUEUE_DEPTH
#define AIC8800_WIFI_SDIO_RX_LARGE_QUEUE_DEPTH 64U
#endif
#ifndef AIC8800_WIFI_SDIO_DATA_THREAD_STACK_SIZE
#define AIC8800_WIFI_SDIO_DATA_THREAD_STACK_SIZE 8192U
#endif
#ifndef AIC8800_WIFI_SDIO_DATA_THREAD_PRIORITY
#define AIC8800_WIFI_SDIO_DATA_THREAD_PRIORITY   12U
#endif
#ifndef AIC8800_WIFI_SDIO_DATA_THREAD_BUDGET
#define AIC8800_WIFI_SDIO_DATA_THREAD_BUDGET    128U
#endif
#ifndef AIC8800_WIFI_SDIO_BUS_THREAD_PRIORITY
#define AIC8800_WIFI_SDIO_BUS_THREAD_PRIORITY    11U
#endif
#ifndef AIC8800_WIFI_SDIO_TX_AGGREGATE_FRAMES
#define AIC8800_WIFI_SDIO_TX_AGGREGATE_FRAMES 32U
#endif
#ifndef AIC8800_WIFI_SDIO_TX_QUEUE_DEPTH
#define AIC8800_WIFI_SDIO_TX_QUEUE_DEPTH     256U
#endif
#define AIC8800_WIFI_SDIO_TX_PRIORITY_RESERVE  4U
/* Zero: send whatever is already queued.  Vendor Linux never waits for a
 * second frame either - aicwf_sdio_send() flushes the aggregate as soon as
 * tx_pktcnt reaches one - and a blocking prefetch here caps un-queued traffic
 * at one frame per wait while holding tx_mutex. */
#ifndef AIC8800_WIFI_SDIO_TX_AGGREGATE_WAIT_MS
#define AIC8800_WIFI_SDIO_TX_AGGREGATE_WAIT_MS 0U
#endif
#ifndef AIC8800_WIFI_SDIO_TX_THREAD_STACK_SIZE
#define AIC8800_WIFI_SDIO_TX_THREAD_STACK_SIZE 4096U
#endif
#ifndef AIC8800_WIFI_SDIO_TX_THREAD_PRIORITY
#define AIC8800_WIFI_SDIO_TX_THREAD_PRIORITY   14U
#endif
#define AIC8800_USB_MAX_RECORD_SIZE \
    ((AIC_USB_LENGTH_MASK + AIC8800_USB_RX_HEADER_SIZE + 3U) & ~3U)
#define AIC8800_RX_REORDER_INLINE_SIZE \
    ((AIC8800_ETHERNET_FRAME_MAX + AIC8800_USB_RX_HEADER_SIZE + 64U + 3U) & \
     ~3U)

#ifndef AIC8800_WIFI_RX_REORDER_FLOWS
#define AIC8800_WIFI_RX_REORDER_FLOWS      128U
#endif
#ifndef AIC8800_WIFI_RX_REORDER_SLOTS
#define AIC8800_WIFI_RX_REORDER_SLOTS      128U
#endif
#ifndef AIC8800_WIFI_RX_REORDER_WINDOW
#define AIC8800_WIFI_RX_REORDER_WINDOW      64U
#endif
#ifndef AIC8800_WIFI_RX_REORDER_TIMEOUT_MS
#define AIC8800_WIFI_RX_REORDER_TIMEOUT_MS  50U
#endif
#define AIC8800_RX_REORDER_SEQUENCE_MASK  0x0fffU
#define AIC8800_RX_REORDER_INVALID_SLOT   0xffffU

#define AIC8800_TCP_ACK_FLOW_COUNT             32U
#define AIC8800_TCP_ACK_FRAME_MAX             200U
#define AIC8800_TCP_ACK_SUPPRESS_LIMIT         10U
#define AIC8800_TCP_ACK_DELAY_MS                5U
#define AIC8800_TCP_ACK_FLOW_TIMEOUT_MS      4000U

#define AIC8800_INVALID_INDEX            0xffU
#define AIC8800_KEY_COUNT                   8U
#define AIC8800_HARDWARE_KEY_COUNT         24U
#define AIC8800_AP_STATION_COUNT            10U
#define AIC8800_PS_ID_COUNT                  2U
#define AIC8800_MGMT_CONFIRM_COUNT         16U
#define AIC8800_FILTER_PATTERN_MAX         64U
#define AIC8800_AP_BEACON_IES_MAX         512U

enum aic8800_transport
{
    AIC8800_TRANSPORT_USB = 0,
    AIC8800_TRANSPORT_SDIO,
};

#ifdef AIC8800_WIFI_TRANSPORT_USB
static inline rt_bool_t aic8800_usb_is_timeout(int result)
{
    /* The maix3 DWC2 port returns the native RT-Thread timeout value. */
    return result == -USB_ERR_TIMEOUT || result == -RT_ETIMEOUT;
}
#endif

struct aic8800_context;

/* Host-only ownership carried beside queued wire records. */
struct aic8800_tx_metadata
{
    rt_uint16_t station_generation;
    rt_uint16_t service_period_generation;
    rt_uint8_t station_index;
    rt_uint8_t vif_index;
    rt_uint8_t ps_id;
    rt_bool_t data_frame;
    rt_bool_t management;
    rt_bool_t accounted;
    rt_bool_t host_buffered;
    rt_bool_t service_period_reserved;
    rt_bool_t more_data;
    rt_bool_t eosp;
};

enum aic8800_tx_record_state
{
    AIC8800_TX_RECORD_READY = 0,
    AIC8800_TX_RECORD_DEFER,
    AIC8800_TX_RECORD_DROP,
};

#ifdef AIC8800_WIFI_TRANSPORT_USB
struct aic8800_rx_worker;
struct aic8800_tx_worker;

struct aic8800_tx_slot
{
    struct aic8800_tx_worker *worker;
    struct usbh_urb urb;
    rt_uint8_t *buffer;
    rt_size_t length;
    volatile rt_bool_t in_use;
    /* Armed immediately before usbh_submit_urb() and retired on giveback.
     * A host channel that stops completing leaves `submitted` set with
     * `submit_tick` frozen, which is the only evidence the watchdog has that
     * the request is stuck rather than merely slow. */
    volatile rt_bool_t submitted;
    /* Set while usbh_kill_urb() owns urb->reject.  Giveback may make the slot
     * otherwise look free before kill returns, so acquisition must also gate
     * on this state. */
    volatile rt_bool_t cancelling;
    volatile rt_tick_t submit_tick;
    struct aic8800_tx_metadata
        metadata[AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES];
    rt_uint8_t metadata_count;
};

struct aic8800_tx_worker
{
    struct aic8800_context *context;
    struct usb_endpoint_descriptor *endpoint;
    struct aic8800_tx_slot *slots;
    struct rt_completion stopped;
    rt_sem_t available;
    rt_size_t slot_count;
    rt_uint32_t pending;
    rt_uint32_t error_count;
    rt_uint32_t timeout_count;
    rt_uint32_t recovery_count;
    /* Counters exported by the on-demand transport statistics command. */
    rt_uint32_t completion_count;
    rt_uint64_t byte_count;
    rt_uint32_t wait_count;
    /* Watchdog cancellations and slots reclaimed without a giveback. */
    rt_uint32_t watchdog_count;
    rt_uint32_t reclaim_count;
    /* Consecutive submissions that acquired a slot without blocking. */
    rt_uint32_t burst_count;
    rt_uint32_t max_burst_count;
    rt_size_t next_slot;
    int last_error;
    volatile rt_bool_t active;
    rt_bool_t semaphore_initialized;
    rt_bool_t completion_initialized;
};

struct aic8800_usb_tx_record
{
    rt_uint64_t token;
    rt_uint16_t length;
    struct aic8800_tx_metadata metadata;
    rt_uint8_t data[AIC8800_WIFI_TX_BUFFER_SIZE];
};

/* Do not put a native pointer in the message queue.  A damaged queue payload
 * used to be consumed as a record address and was dereferenced immediately by
 * the TX worker.  Keep a checked, redundant token at opposite ends of one
 * RT_ALIGN_SIZE queue cell instead. */
struct aic8800_usb_tx_queue_entry
{
    rt_uint64_t token;
    rt_uint8_t reserved[48];
    rt_uint64_t token_check;
};

_Static_assert(sizeof(struct aic8800_usb_tx_queue_entry) == 64,
               "AIC USB TX queue entry must occupy one RT queue cell");
_Static_assert(AIC8800_WIFI_USB_TX_QUEUE_DEPTH <=
                   AIC8800_USB_TX_TOKEN_INDEX_MAX,
               "AIC USB TX token index is eight bits");

struct aic8800_rx_slot
{
    struct aic8800_rx_worker *worker;
    struct usbh_urb urb;
    rt_uint8_t *buffer;
    rt_size_t length;
    int result;
};

/* Posted from the URB giveback thread to the RX worker.  `slot` is set when
 * the worker must rearm or recover that request; it is NULL when giveback
 * already swapped in a spare and resubmitted. */
struct aic8800_rx_done
{
    struct aic8800_rx_slot *slot;
    rt_uint8_t *buffer;
    rt_size_t length;
    int result;
#ifdef AIC8800_WIFI_DEBUG_STATS
    /* Free-running microseconds at giveback, so the receive worker can
     * report how long a completed transfer waited to be processed. */
    rt_uint64_t queued_us;
#endif
};

struct aic8800_rx_worker
{
    struct aic8800_context *context;
    struct usb_endpoint_descriptor *endpoint;
    struct rt_completion stopped;
    rt_thread_t thread;
    rt_mq_t completed;
    struct aic8800_rx_slot *slots;
    rt_size_t slot_count;
    /* Master list of every DMA buffer owned by this worker (slot buffers
     * plus the spare pool).  Teardown frees this list and ignores whichever
     * pointers happen to sit in the completion queue. */
    rt_uint8_t **buffers;
    rt_size_t buffer_count;
    rt_uint8_t **spare;
    rt_size_t spare_count;
    rt_size_t spare_available;
    rt_uint8_t *assembly;
    rt_size_t assembly_length;
    rt_size_t assembly_capacity;
    rt_size_t padding_length;
    rt_uint32_t recovery_count;
    rt_uint32_t consecutive_errors;
    /* Set by the first successful transfer on this endpoint.  Until then a
     * failure is not the routine mid-association deafness the error log
     * demotes, so it must stay visible.  Not a statistic: the demotion has to
     * behave the same way in builds without AIC8800_WIFI_DEBUG_STATS. */
    rt_bool_t ever_completed;
#ifdef AIC8800_WIFI_DEBUG_STATS
    rt_uint32_t spare_empty_count;
    rt_uint32_t complete_rearm_count;
    rt_uint32_t completion_count;
    rt_uint32_t zero_length_count;
    rt_uint32_t rearm_count;
    rt_uint32_t error_count;
    rt_uint32_t retry_count;
    /* Completions still queued when this worker picked one up. */
    rt_uint16_t queue_high_water;
    /* Microseconds a completed transfer spent between giveback and this
     * worker picking it up.  A large figure means the endpoint delivered on
     * time and the delay is on this side; a small one means the frames were
     * late arriving and the driver is not what held them. */
    rt_uint32_t dispatch_max_us;
    rt_uint32_t dispatch_last_us;
#endif
    int last_error;
    rt_uint8_t completion_log_count;
    volatile rt_bool_t active;
    volatile rt_bool_t queue_overflow;
    const char *name;
};
#endif

struct aic8800_rx_reorder_slot
{
    rt_uint16_t next;
    rt_uint16_t sequence;
    rt_uint16_t length;
    rt_tick_t queued_at;
    rt_bool_t used;
    rt_uint8_t *external_data;
    rt_uint8_t data[AIC8800_RX_REORDER_INLINE_SIZE];
};

struct aic8800_rx_reorder_flow
{
    rt_uint16_t head;
    rt_uint16_t expected;
    rt_tick_t last_seen;
    rt_uint8_t vif_index;
    rt_uint8_t station_index;
    rt_uint8_t tid;
    rt_bool_t valid;
    rt_bool_t initialized;
};

#ifdef AIC8800_WIFI_TCP_ACK_FILTER
struct aic8800_tcp_ack_flow
{
    rt_uint32_t source_address;
    rt_uint32_t destination_address;
    rt_uint32_t acknowledgement;
    rt_tick_t last_seen;
    rt_uint16_t source_port;
    rt_uint16_t destination_port;
    rt_uint16_t window;
    rt_uint16_t length;
    enum rt_wlan_offload_iftype iftype;
    rt_uint8_t suppressed;
    rt_bool_t valid;
    rt_bool_t acknowledgement_valid;
    rt_bool_t pending;
    rt_bool_t quick_ack;
    rt_uint8_t data[AIC8800_TCP_ACK_FRAME_MAX];
};
#endif

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
struct aic8800_sdio_registers
{
    rt_uint8_t byte_length;
    rt_uint8_t interrupt_config;
    rt_uint8_t interrupt_pending;
    rt_uint8_t wakeup;
    rt_uint8_t flow_control;
    rt_uint8_t register_block;
    rt_uint8_t byte_mode_enable;
    rt_uint8_t block_count;
    rt_uint8_t interrupt_status;
    rt_uint8_t read_fifo;
    rt_uint8_t write_fifo;
};
#endif

/* Parsed from the vendor userconfig text file.  The defaults are the
 * conservative values used when the optional file is absent. */
struct aic8800_radio_config_state
{
    struct aic_wire_tx_power_index tx_power_index;
    struct aic_wire_tx_power_offset tx_power_offset;
    struct aic_wire_tx_power_v2 tx_power_v2;
    struct aic_wire_tx_power_v3 tx_power_v3;
    struct aic_wire_tx_power_v4 tx_power_v4;
    struct aic_wire_tx_power_adjust tx_power_adjust;
    struct aic_wire_tx_power_offset_2x tx_power_offset_2x;
    struct aic_wire_tx_power_offset_2x_v2 tx_power_offset_2x_v2;
    rt_uint8_t loss_enabled_2ghz;
    rt_int8_t loss_value_2ghz;
    rt_uint8_t loss_enabled_5ghz;
    rt_int8_t loss_value_5ghz;
    rt_uint8_t crystal_enabled;
    rt_uint8_t crystal_capacitance;
    rt_uint8_t crystal_capacitance_fine;
    rt_int8_t channel_power_2ghz[14];
    rt_int8_t channel_power_5ghz[28];
    rt_uint32_t channel_valid_2ghz;
    rt_uint32_t channel_valid_5ghz;
    rt_bool_t powerlimit_loaded;
    char country_code[3];
    rt_bool_t loaded;
};

struct aic8800_mgmt_confirmation
{
    rt_bool_t used;
    rt_uint32_t index;
    rt_uint32_t request_id;
    rt_uint64_t cookie;
    rt_uint8_t *frame;
    rt_size_t frame_length;
    rt_tick_t deadline;
    enum rt_wlan_offload_iftype iftype;
};

struct aic8800_hardware_key
{
    rt_bool_t valid;
    enum rt_wlan_offload_iftype iftype;
    rt_uint8_t index;
    rt_bool_t pairwise;
    rt_uint8_t peer[6];
    rt_uint8_t hardware_index;
};

struct aic8800_ap_station
{
    rt_bool_t valid;
    rt_bool_t authorized;
    rt_bool_t qos;
    rt_uint8_t acm;
    rt_uint8_t address[6];
    rt_uint16_t aid;
    rt_uint16_t uapsd_tids;
    rt_uint8_t firmware_index;
};

struct aic8800_station_loss
{
    rt_bool_t pending;
    rt_uint8_t vif_index;
    rt_uint8_t station_index;
    rt_uint8_t address[6];
};

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
struct aic8800_sdio_tx_record
{
    rt_uint16_t length;
    rt_uint8_t priority;
    struct aic8800_tx_metadata metadata;
    rt_uint8_t data[AIC8800_WIFI_TX_BUFFER_SIZE];
};

struct aic8800_sdio_rx_record
{
    rt_uint16_t length;
    rt_uint8_t data[];
};
#endif

struct aic8800_auth_cache
{
    rt_bool_t valid;
    rt_wlan_ssid_t ssid;
    rt_uint8_t bssid[6];
    struct rt_wlan_offload_channel_definition channel;
    enum rt_wlan_offload_auth_type auth_type;
};

struct aic8800_context
{
    struct rt_wlan_offload_radio radio;
    struct rt_wlan_offload_bus bus;
    struct rt_wlan_offload_command_manager commands;
    struct rt_semaphore command_gate;

    enum aic8800_transport transport;
    rt_uint16_t vendor_id;
    rt_uint16_t product_id;
    rt_uint8_t chip_id;
    rt_uint8_t chip_sub_id;
    rt_uint8_t chip_mcu_id;
    rt_bool_t chip_revision_valid;
    rt_mutex_t tx_mutex;
    rt_mutex_t frame_mutex;
    rt_uint8_t tx_frame[AIC8800_WIFI_TX_BUFFER_SIZE];

    struct rt_mutex rx_reorder_mutex;
    struct rt_timer rx_reorder_timer;
    struct rt_work rx_reorder_work;
    struct aic8800_rx_reorder_flow
        rx_reorder_flows[AIC8800_WIFI_RX_REORDER_FLOWS];
    struct aic8800_rx_reorder_slot *rx_reorder_slots;
    volatile rt_uint16_t rx_reorder_pending;
    rt_bool_t rx_reorder_mutex_initialized;
    rt_bool_t rx_reorder_timer_initialized;
    rt_bool_t rx_reorder_work_initialized;
    volatile rt_bool_t rx_reorder_work_queued;
    rt_bool_t rx_reorder_initialized;

#ifdef AIC8800_WIFI_TCP_ACK_FILTER
    struct rt_mutex tcp_ack_mutex;
    struct rt_timer tcp_ack_timer;
    struct rt_work tcp_ack_work;
    struct aic8800_tcp_ack_flow
        tcp_ack_flows[AIC8800_TCP_ACK_FLOW_COUNT];
    rt_bool_t tcp_ack_mutex_initialized;
    rt_bool_t tcp_ack_timer_initialized;
    rt_bool_t tcp_ack_timer_armed;
    rt_bool_t tcp_ack_work_initialized;
    volatile rt_bool_t tcp_ack_work_queued;
    rt_bool_t tcp_ack_initialized;
#endif

#ifdef AIC8800_WIFI_TRANSPORT_USB
    struct usbh_hubport *hport;
    rt_uint8_t interface_number;
    struct usb_endpoint_descriptor *data_in;
    struct usb_endpoint_descriptor *data_out;
    struct usb_endpoint_descriptor *message_in;
    struct usb_endpoint_descriptor *message_out;
    struct usbh_urb tx_urb;
    struct rt_work attach_work;
    struct rt_completion attach_done;
    rt_err_t attach_result;
    struct rt_work tx_recovery_work;
    struct rt_timer tx_watchdog_timer;
    struct rt_work tx_watchdog_work;
    struct aic8800_tx_worker tx_worker;
    struct aic8800_rx_worker data_worker;
    struct aic8800_rx_worker message_worker;
    struct rt_completion usb_tx_thread_stopped;
    rt_thread_t usb_tx_thread;
    rt_mq_t usb_tx_queue;
    rt_mp_t usb_tx_pool;
    rt_uint8_t *usb_tx_aggregate_buffer;
    rt_uint32_t usb_tx_queue_drop_count;
    rt_uint32_t usb_tx_error_count;
    /* Records the transmit worker requeued or discarded after re-checking the
     * peer, and frames the bulk OUT stage could not place.  All three used to be
     * silent, so a frame lwIP believed it had sent could vanish with no counter
     * anywhere - which is exactly what makes a receiver-side loss figure
     * impossible to attribute between the host and the air. */
    rt_uint32_t usb_tx_record_defer_count;
    rt_uint32_t usb_tx_record_drop_count;
    rt_uint32_t usb_tx_submit_busy_count;
    rt_uint32_t usb_tx_queue_token_recovery_count;
    rt_uint32_t usb_tx_queue_token_error_count;
    rt_uint32_t usb_tx_queue_record_error_count;
    rt_uint32_t usb_tx_pool_rebuild_count;
    rt_uint32_t usb_tx_queue_orphan_reclaim_count;
    rt_uint32_t usb_tx_token_sequence;
    volatile rt_bool_t usb_tx_queue_recovery_pending;
    rt_bool_t usb_tx_thread_started;
    volatile rt_bool_t usb_tx_queue_enabled;
    rt_bool_t usb_tx_aggregation_enabled;
    volatile rt_bool_t usb_tx_terminate;
#endif

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    struct rt_mmcsd_card *sdio_card;
    struct rt_sdio_function *sdio_function;
    struct rt_sdio_function *sdio_message_function;
    struct aic8800_sdio_registers sdio_registers;
    struct rt_work sdio_attach_work;
    struct rt_event sdio_event;
    struct rt_completion sdio_thread_stopped;
    struct rt_completion sdio_tx_thread_stopped;
    struct rt_completion sdio_data_thread_stopped;
    struct rt_mutex sdio_tx_queue_mutex;
    struct rt_mutex sdio_data_queue_mutex;
    rt_thread_t sdio_thread;
    rt_thread_t sdio_tx_thread;
    rt_thread_t sdio_data_thread;
    rt_mq_t sdio_tx_queue;
    rt_mp_t sdio_tx_pool;
    rt_mq_t sdio_data_queue;
    rt_mp_t sdio_data_pool;
    rt_mp_t sdio_data_large_pool;
    rt_uint8_t *sdio_rx_buffer;
    rt_uint8_t *sdio_command_rx_buffer;
    rt_uint8_t *sdio_tx_buffer;
    rt_size_t sdio_tx_capacity;
    rt_uint16_t sdio_vendor_id;
    rt_uint16_t sdio_product_id;
    rt_uint32_t sdio_rx_count;
    rt_uint32_t sdio_tx_count;
    rt_uint32_t sdio_tx_frame_count;
    rt_uint32_t sdio_tx_aggregate_count;
    rt_uint32_t sdio_tx_queue_drop_count;
    rt_uint32_t sdio_tx_error_count;
    rt_uint32_t sdio_tx_flow_read_count;
    rt_uint32_t sdio_tx_credit_wait_count;
    rt_uint32_t sdio_tx_credit_retry_count;
    rt_uint32_t sdio_tx_credit_timeout_count;
    rt_uint32_t sdio_tx_credit_fallback_count;
    /* Time spent waiting for firmware credits and frames granted. */
    rt_uint32_t sdio_tx_credit_wait_ticks;
    rt_uint32_t sdio_tx_credit_grant_frames;
    /* Records the transmit worker discarded or requeued after re-checking the
     * peer against aic8800_core_tx_metadata_state().  Both paths used to be
     * silent, which hides an EAPOL or data frame that lwIP believes was sent. */
    rt_uint32_t sdio_tx_record_defer_count;
    rt_uint32_t sdio_tx_record_drop_count;
    rt_uint16_t sdio_tx_max_aggregate;
    rt_uint16_t sdio_tx_queue_high_water;
    rt_uint8_t sdio_tx_available_credits;
    rt_uint8_t sdio_tx_credit_max_retries;
    rt_uint32_t sdio_error_count;
    rt_uint32_t sdio_protocol_drop_count;
    rt_uint32_t sdio_consecutive_errors;
    rt_uint32_t sdio_data_queued_count;
    rt_uint32_t sdio_data_processed_count;
    rt_uint32_t sdio_data_drop_count;
    rt_uint16_t sdio_data_queue_high_water;
    rt_err_t sdio_attach_result;
    rt_bool_t sdio_v3;
    rt_bool_t sdio_attach_work_initialized;
    rt_bool_t sdio_function_enabled;
    rt_bool_t sdio_message_function_enabled;
    rt_bool_t sdio_event_initialized;
    rt_bool_t sdio_thread_started;
    rt_bool_t sdio_tx_thread_started;
    rt_bool_t sdio_data_thread_started;
    rt_bool_t sdio_tx_queue_mutex_initialized;
    rt_bool_t sdio_data_queue_mutex_initialized;
    rt_bool_t sdio_data_queue_initialized;
    rt_bool_t sdio_irq_attached;
    rt_bool_t sdio_recovery_reported;
    volatile rt_bool_t sdio_active;
    volatile rt_bool_t sdio_irq_source_masked;
    volatile rt_bool_t sdio_terminate;
    volatile rt_bool_t sdio_tx_terminate;
    volatile rt_bool_t sdio_data_queue_active;
    volatile rt_bool_t sdio_data_queue_stopping;
    volatile rt_bool_t sdio_data_terminate;
#endif

    rt_bool_t attached;
    rt_bool_t bus_initialized;
    rt_bool_t tx_mutex_initialized;
    rt_bool_t frame_mutex_initialized;
#ifdef AIC8800_WIFI_TRANSPORT_USB
    rt_bool_t attach_work_initialized;
    rt_bool_t tx_recovery_work_initialized;
    rt_bool_t tx_watchdog_timer_initialized;
    rt_bool_t tx_watchdog_work_initialized;
    volatile rt_bool_t tx_watchdog_work_queued;
#endif
    rt_bool_t transport_connected;
    rt_bool_t firmware_transition;
    rt_bool_t firmware_runtime_ready;
    rt_bool_t lmac_started;
    rt_uint32_t firmware_features; /* Normalized AIC_FW_CAP_* masks. */
    rt_uint32_t firmware_phy_features; /* MM_VERSION PHY feature register. */
    rt_bool_t firmware_feature_map_compact;
    rt_bool_t firmware_supports_5ghz;
    rt_bool_t firmware_capabilities_valid;
    rt_country_code_t country;
    rt_int32_t power_save_level;
    rt_bool_t station_enabled;
    rt_bool_t station_connected;
    rt_bool_t station_qos;
    rt_uint8_t station_acm;
    rt_bool_t station_interface_recycle_pending;
    rt_bool_t ap_enabled;
    rt_bool_t ap_started;
    rt_bool_t promiscuous_enabled;
    rt_bool_t filter_enabled;
    rt_filter_rule_t filter_rule;
    rt_uint16_t filter_offset;
    rt_uint16_t filter_length;
    rt_uint8_t filter_mask[AIC8800_FILTER_PATTERN_MAX];
    rt_uint8_t filter_pattern[AIC8800_FILTER_PATTERN_MAX];
    struct rt_wlan_offload_channel_definition current_channel;
    rt_bool_t current_channel_valid;
    rt_uint8_t vif_index;
    rt_uint8_t ap_station_index;
    rt_uint8_t ap_vif_index;
    rt_uint8_t ap_broadcast_station_index;
    /* LMAC channel-context indices.  The firmware serves one context at a
     * time and announces switches; a VIF whose context is not the scheduled
     * one must not be handed traffic. */
    rt_uint8_t station_channel_index;
    rt_uint8_t ap_channel_index;
    rt_uint8_t active_channel_index;
    /* Set once the firmware announces a switch.  Until then it is serving a
     * single context and never reports one, so transmit must not be gated. */
    rt_bool_t channel_context_tracked;
    /* MM_VERSION reports the modem's maximum channel width, and on some parts
     * it overstates it: an AIC8800D40 answers 80 MHz behind the same USB
     * product ID as a D80.  An association that settles for 40 MHz on an AP
     * offering 80 MHz or more proves the report wrong, because a genuine
     * 80 MHz station would have taken 80.  Latch that and stop advertising a
     * width the firmware will not use, so the AP stops rating its downlink
     * for one. */
    rt_bool_t bandwidth_80_rejected;
    /* Consecutive 5 GHz associations that settled below 80 MHz on an AP
     * offering 80 MHz or more.  Any association that does reach 80 MHz clears
     * this, so a D80 having one odd association is not mistaken for a D40. */
    rt_uint8_t bandwidth_80_failures;
    /* Set when the advertised PHY capabilities changed after ME_CONFIG was
     * last sent; the next connect re-sends it before associating. */
    rt_bool_t me_config_stale;
    rt_uint32_t tx_off_channel_count;
    struct rt_wlan_offload_channel_definition ap_channel;
    rt_uint8_t address[6];
    rt_uint8_t bssid[6];
    rt_int16_t rssi;

    rt_uint32_t scan_request_id;
    struct rt_work scan_work;
    struct rt_work ap_rechannel_work;
    struct aic_wire_scanu_start_req scan_followup;
    rt_uint16_t scan_results_2ghz;
    rt_uint16_t scan_results_5ghz;
    /* SCANU_RESULT_IND records that arrived with no scan request outstanding. */
    rt_uint32_t scan_late_result_count;
    rt_err_t scan_completion_status;
    rt_uint16_t scan_expected_results;
    rt_uint8_t scan_completion_retry_count;
    rt_uint8_t scan_followup_retry_count;
    rt_bool_t scan_work_initialized;
    rt_bool_t scan_work_queued;
    rt_bool_t scan_completion_pending;
    rt_bool_t scan_result_count_valid;
    rt_bool_t scan_followup_pending;
    rt_bool_t ap_rechannel_work_initialized;
    rt_bool_t ap_rechannel_work_queued;
    rt_bool_t ap_paused_for_station;
    rt_bool_t ap_resume_on_station_channel;
    rt_bool_t ap_settings_valid;
    struct rt_wlan_offload_ap_settings ap_settings;
    rt_uint8_t ap_beacon_ies[AIC8800_AP_BEACON_IES_MAX];
    rt_uint32_t connect_request_id;
    rt_uint32_t disconnect_request_id;
    rt_bool_t wep_enabled;
    rt_bool_t wep_auth_error;
    rt_bool_t station_control_port_pending;
    rt_bool_t station_control_port_open;
    enum rt_wlan_offload_auth_type wep_last_auth_type;
    struct aic8800_auth_cache auth;
    struct aic8800_hardware_key hardware_keys[AIC8800_HARDWARE_KEY_COUNT];
    struct aic8800_ap_station ap_stations[AIC8800_AP_STATION_COUNT];
    struct rt_mutex mgmt_confirmation_mutex;
    struct rt_timer mgmt_confirmation_timer;
    struct aic8800_mgmt_confirmation
        mgmt_confirmations[AIC8800_MGMT_CONFIRM_COUNT];
    rt_uint32_t next_mgmt_confirmation;
    rt_bool_t mgmt_confirmation_mutex_initialized;
    rt_bool_t mgmt_confirmation_timer_initialized;
    /* Transport accounting used by error and shutdown diagnostics. */
    rt_uint32_t usb_tx_frame_count;
    rt_uint32_t usb_tx_aggregate_count;
    rt_uint16_t usb_tx_max_aggregate;
    rt_uint16_t usb_tx_queue_high_water;
#ifdef AIC8800_WIFI_DEBUG_STATS
    rt_uint32_t rx_reorder_queued;
    rt_uint32_t rx_reorder_delivered;
    rt_uint32_t rx_reorder_timeouts;
    rt_uint32_t rx_reorder_duplicates;
    rt_uint32_t rx_reorder_drops;
    rt_uint32_t tcp_ack_suppressed;
    rt_uint32_t tcp_ack_flushed;
    rt_uint32_t station_control_port_set_count;
    rt_uint32_t station_control_port_error_count;
    rt_uint32_t ethernet_tx_count;
    rt_uint32_t ethernet_tx_error_count;
    rt_uint32_t ethernet_rx_count;
    rt_uint32_t ethernet_rx_error_count;
    rt_uint32_t arp_tx_count;
    rt_uint32_t arp_rx_count;
    rt_uint32_t icmp_tx_count;
    rt_uint32_t icmp_rx_count;
    rt_uint32_t icmp_tx_malformed_count;
    rt_uint32_t icmp_tx_ip_checksum_error_count;
    rt_uint32_t icmp_tx_checksum_error_count;
    rt_uint32_t rx_data_record_count;
    rt_uint32_t rx_qos_record_count;
    rt_uint32_t rx_amsdu_record_count;
    rt_uint32_t rx_amsdu_subframe_count;
    rt_uint32_t rx_no_llc_count;
    rt_uint32_t rx_invalid_data_count;
#endif
    /* Frames handed over for a peer the firmware has no station entry for.
     * Always built: it also gates the rate-limited drop log. */
    rt_uint32_t tx_no_station_count;
    /* Per-station firmware transmit credits.  Only enforced once the firmware
     * has actually reported one, so a build that never sends them keeps
     * working. */
    rt_int16_t tx_credits[AIC8800_STATION_SLOTS];
    /* Frames handed to the transport for a station and not yet completed. */
    rt_uint16_t tx_pending[AIC8800_STATION_SLOTS];
    rt_uint16_t sta_generation[AIC8800_STATION_SLOTS];
    rt_bool_t sta_present[AIC8800_STATION_SLOTS];
    /* PS changes can arrive before ME_STA_ADD_CFM makes the station present. */
    rt_bool_t station_add_pending;
    rt_bool_t sta_add_power_save[AIC8800_STATION_SLOTS];
    /* Observed peer power-save state.  Ordinary downlink records are held out
     * of the firmware data endpoint while a peer sleeps. */
    rt_bool_t sta_power_save[AIC8800_STATION_SLOTS];
    rt_uint16_t sta_uapsd_tids[AIC8800_STATION_SLOTS];
    rt_uint16_t sta_buffered[AIC8800_STATION_SLOTS][AIC8800_PS_ID_COUNT];
    rt_uint16_t sta_service_period_generation[AIC8800_STATION_SLOTS]
                                                     [AIC8800_PS_ID_COUNT];
    rt_uint16_t sta_service_period_remaining[AIC8800_STATION_SLOTS]
                                                    [AIC8800_PS_ID_COUNT];
    rt_uint16_t sta_service_period_reserved[AIC8800_STATION_SLOTS]
                                                   [AIC8800_PS_ID_COUNT];
    rt_bool_t sta_traffic_available[AIC8800_STATION_SLOTS]
                                           [AIC8800_PS_ID_COUNT];
    rt_bool_t sta_traffic_reported[AIC8800_STATION_SLOTS]
                                          [AIC8800_PS_ID_COUNT];
    rt_bool_t sta_traffic_dirty[AIC8800_STATION_SLOTS]
                                       [AIC8800_PS_ID_COUNT];
    struct rt_work traffic_work;
    struct rt_work station_loss_work;
    struct aic8800_station_loss station_loss[AIC8800_AP_STATION_COUNT];
    rt_bool_t command_gate_initialized;
    rt_bool_t traffic_work_initialized;
    volatile rt_bool_t traffic_work_queued;
    rt_bool_t station_loss_work_initialized;
    volatile rt_bool_t station_loss_work_queued;
    /* Frames accepted while this station was already at the diagnostic
     * pending watermark. */
    rt_uint32_t tx_pending_watermark_count;
    rt_uint32_t tx_power_save_buffered_count;
    rt_bool_t tx_credits_tracked;
    rt_uint32_t tx_credit_update_count;
    rt_uint8_t invalid_rx_log_count;
    rt_uint8_t command_tx_log_count;
    rt_uint8_t command_rx_log_count;
    struct rt_wlan_offload_channel channels_2ghz[14];
    struct rt_wlan_offload_channel channels_5ghz[25];
    struct rt_wlan_offload_supported_band band_2ghz;
    struct rt_wlan_offload_supported_band band_5ghz;
    struct aic8800_radio_config_state radio_config;
};

/* Rate limit for repeating conditions: the first few, then powers of two. */
rt_inline rt_bool_t aic8800_log_throttle(rt_uint32_t count)
{
    return count <= 4U || !(count & (count - 1U));
}

/* Called by the transport when a transmitted frame completes, so the
 * per-station in-flight count can be released. */
void aic8800_core_tx_metadata_init(
    struct aic8800_context *context, const void *data, rt_size_t length,
    struct aic8800_tx_metadata *metadata);
enum aic8800_tx_record_state aic8800_core_tx_metadata_state(
    struct aic8800_context *context,
    struct aic8800_tx_metadata *metadata);
enum aic8800_tx_record_state aic8800_core_tx_metadata_apply(
    struct aic8800_context *context, struct aic8800_tx_metadata *metadata,
    void *data, rt_size_t length);
void aic8800_core_tx_metadata_restore(
    struct aic8800_context *context, struct aic8800_tx_metadata *metadata);
void aic8800_core_tx_complete(
    struct aic8800_context *context,
    const struct aic8800_tx_metadata *metadata);
void aic8800_core_tx_pending_reset(struct aic8800_context *context);
rt_err_t aic8800_core_attach(struct aic8800_context *context);
rt_err_t aic8800_core_detach(struct aic8800_context *context);
rt_err_t aic8800_core_receive(struct rt_wlan_offload_bus *bus, const void *data,
                              rt_size_t length, void *parameter);
#ifdef AIC8800_WIFI_DEBUG_STATS
#ifdef AIC8800_WIFI_TRANSPORT_USB
struct aic8800_context *aic8800_usb_stat_context(void);
#endif
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
struct aic8800_context *aic8800_sdio_stat_context(void);
#endif
#endif

rt_err_t aic8800_protocol_command(struct aic8800_context *context,
                                rt_uint16_t request_id,
                                rt_uint16_t confirmation_id,
                                const void *request,
                                rt_size_t request_length,
                                void *response,
                                rt_size_t response_capacity,
                                rt_size_t *response_length);
rt_err_t aic8800_radio_initialize(struct aic8800_context *context);
rt_err_t aic8800_radio_load_config(struct aic8800_context *context);
rt_bool_t aic8800_radio_supports_80mhz(
    const struct aic8800_context *context);
rt_bool_t aic8800_radio_channel_allowed(
    const struct aic8800_context *context,
    enum rt_wlan_offload_band_id band, rt_uint16_t channel);
rt_int8_t aic8800_radio_channel_power(
    const struct aic8800_context *context,
    enum rt_wlan_offload_band_id band, rt_uint16_t channel,
    rt_int8_t default_power);
rt_int8_t aic8800_radio_channel_fw_power(
    const struct aic8800_context *context,
    enum rt_wlan_offload_band_id band, rt_uint16_t channel,
    rt_int8_t default_power);
rt_err_t aic8800_radio_set_regulatory(struct aic8800_context *context,
                                       rt_country_code_t country);
rt_err_t aic8800_radio_get_regulatory(const struct aic8800_context *context,
                                       rt_country_code_t *country);
void aic8800_radio_prepare(struct aic8800_context *context,
                               struct aic_wire_me_config_req *request);

rt_err_t aic8800_firmware_probe(struct aic8800_context *context,
                                rt_bool_t *attach_runtime);
void aic8800_firmware_disconnected(struct aic8800_context *context);
int aic8800_firmware_open_file(const struct aic8800_context *context,
                               const char *directory, const char *name,
                               char *path, rt_size_t path_capacity);
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
rt_err_t aic8800_firmware_wait_available(struct aic8800_context *context,
                                          rt_uint32_t timeout_ms);
#endif

#ifdef AIC8800_WIFI_TRANSPORT_USB
rt_err_t aic8800_usb_driver_init(void);
#endif

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
rt_err_t aic8800_sdio_driver_init(void);
rt_err_t aic8800_sdio_firmware_transmit(struct aic8800_context *context,
                                         const void *data, rt_size_t length);
rt_err_t aic8800_sdio_firmware_receive(struct aic8800_context *context,
                                        void *data, rt_size_t capacity,
                                        rt_size_t *length,
                                        rt_uint32_t timeout_ms);
rt_err_t aic8800_sdio_pump_command(
    struct aic8800_context *context,
    struct rt_wlan_offload_command_manager *manager,
    rt_uint32_t token, rt_uint32_t timeout_ms);
#endif

#if defined(AIC8800_WIFI_TRANSPORT_USB) && defined(AIC8800_WIFI_BLE)
rt_err_t aic8800_btusb_driver_init(void);
#endif

#endif /* __AIC8800_WIFI_H__ */
