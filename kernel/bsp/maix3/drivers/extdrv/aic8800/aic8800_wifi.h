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
#define AIC8800_SDIO_VENDOR_AIC8800D80     0xc8a1
#define AIC8800_SDIO_PRODUCT_AIC8800D80    0x0082

#define AIC8800_USB_HEADER_SIZE            4U
#define AIC8800_USB_RX_HEADER_SIZE        60U
#define AIC8800_USB_MAX_COMMAND_SIZE    1536U
#define AIC8800_ETHERNET_FRAME_MAX      1588U
#define AIC8800_USB_DMA_ALIGNMENT          64U
#ifndef AIC8800_WIFI_DATA_RX_URBS
#define AIC8800_WIFI_DATA_RX_URBS        5U
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
#ifndef AIC8800_WIFI_USB_TX_AGGREGATE_WAIT_MS
#define AIC8800_WIFI_USB_TX_AGGREGATE_WAIT_MS 1U
#endif
#ifndef AIC8800_WIFI_USB_TX_THREAD_STACK_SIZE
#define AIC8800_WIFI_USB_TX_THREAD_STACK_SIZE 4096U
#endif
#ifndef AIC8800_WIFI_USB_TX_THREAD_PRIORITY
#define AIC8800_WIFI_USB_TX_THREAD_PRIORITY 14U
#endif
#ifndef AIC8800_WIFI_TX_BUFFER_SIZE
#define AIC8800_WIFI_TX_BUFFER_SIZE    2048U
#endif
#if AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES > 1U
#define AIC8800_WIFI_USB_TX_TRANSFER_SIZE \
    AIC8800_WIFI_USB_TX_AGGREGATE_SIZE
#else
#define AIC8800_WIFI_USB_TX_TRANSFER_SIZE AIC8800_WIFI_TX_BUFFER_SIZE
#endif
#ifndef AIC8800_WIFI_TX_WAIT_MS
#define AIC8800_WIFI_TX_WAIT_MS          100U
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
#ifndef AIC8800_WIFI_SDIO_TX_AGGREGATE_WAIT_MS
#define AIC8800_WIFI_SDIO_TX_AGGREGATE_WAIT_MS 1U
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
    volatile rt_tick_t submit_tick;
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
    rt_uint32_t completion_count;
    rt_uint64_t byte_count;
    rt_uint32_t error_count;
    rt_uint32_t wait_count;
    rt_uint32_t timeout_count;
    rt_uint32_t recovery_count;
    /* Requests the watchdog had to cancel because the host controller never
     * gave them back, and of those, the ones whose giveback still did not
     * arrive so the slot had to be reclaimed by hand. */
    rt_uint32_t watchdog_count;
    rt_uint32_t reclaim_count;
    /* Frames submitted without blocking on a free request.  A large burst
     * means the transmit worker held the CPU across that many frames, which
     * starves any lower-priority receive worker. */
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
    rt_uint16_t length;
    rt_uint8_t data[AIC8800_WIFI_TX_BUFFER_SIZE];
};

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
    rt_uint32_t spare_empty_count;
    rt_uint32_t complete_rearm_count;
    rt_uint8_t *assembly;
    rt_size_t assembly_length;
    rt_size_t assembly_capacity;
    rt_size_t padding_length;
    rt_uint32_t completion_count;
    rt_uint32_t zero_length_count;
    rt_uint32_t rearm_count;
    rt_uint32_t error_count;
    rt_uint32_t retry_count;
    rt_uint32_t recovery_count;
    rt_uint32_t consecutive_errors;
    /* Completions still queued when this worker picked one up.  A non-zero
     * value means the worker is not keeping up with the endpoint and its
     * requests are being rearmed late. */
    rt_uint16_t queue_high_water;
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
    rt_uint8_t loss_enabled;
    rt_int8_t loss_value;
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
    rt_uint8_t address[6];
    rt_uint16_t aid;
    rt_uint8_t firmware_index;
};

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
struct aic8800_sdio_tx_record
{
    rt_uint16_t length;
    rt_uint8_t priority;
    rt_uint8_t reserved;
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
    rt_uint32_t rx_reorder_queued;
    rt_uint32_t rx_reorder_delivered;
    rt_uint32_t rx_reorder_timeouts;
    rt_uint32_t rx_reorder_duplicates;
    rt_uint32_t rx_reorder_drops;
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
    rt_uint32_t tcp_ack_suppressed;
    rt_uint32_t tcp_ack_flushed;
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
    rt_uint32_t usb_tx_frame_count;
    rt_uint32_t usb_tx_aggregate_count;
    rt_uint32_t usb_tx_queue_drop_count;
    rt_uint32_t usb_tx_error_count;
    rt_uint16_t usb_tx_max_aggregate;
    rt_uint16_t usb_tx_queue_high_water;
    rt_bool_t usb_tx_thread_started;
    rt_bool_t usb_tx_queue_enabled;
    rt_bool_t usb_tx_aggregation_enabled;
    volatile rt_bool_t usb_tx_terminate;
#endif

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    struct rt_mmcsd_card *sdio_card;
    struct rt_sdio_function *sdio_function;
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
    rt_uint8_t scan_followup_retry_count;
    rt_bool_t scan_work_initialized;
    rt_bool_t scan_work_queued;
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
    rt_uint32_t station_control_port_set_count;
    rt_uint32_t station_control_port_error_count;
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
    rt_uint8_t invalid_rx_log_count;
    rt_uint8_t command_tx_log_count;
    rt_uint8_t command_rx_log_count;
    struct rt_wlan_offload_channel channels_2ghz[14];
    struct rt_wlan_offload_channel channels_5ghz[25];
    struct rt_wlan_offload_supported_band band_2ghz;
    struct rt_wlan_offload_supported_band band_5ghz;
    struct aic8800_radio_config_state radio_config;
};

rt_err_t aic8800_core_attach(struct aic8800_context *context);
rt_err_t aic8800_core_detach(struct aic8800_context *context);
rt_err_t aic8800_core_receive(struct rt_wlan_offload_bus *bus, const void *data,
                              rt_size_t length, void *parameter);
void aic8800_core_print_rc_stats(struct aic8800_context *context);

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
rt_bool_t aic8800_radio_channel_allowed(
    const struct aic8800_context *context,
    enum rt_wlan_offload_band_id band, rt_uint16_t channel);
rt_int8_t aic8800_radio_channel_power(
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
