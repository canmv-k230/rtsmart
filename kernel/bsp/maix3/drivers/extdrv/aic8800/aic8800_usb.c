/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CherryUSB transport for the RT-Smart AIC8800 WLAN offload driver.
 */
#include "aic8800_wifi.h"

#include <rtlibc.h>

#define DBG_TAG "aic8800.usb"
#define DBG_LVL AIC8800_DBG_LVL
#include <rtdbg.h>

#define AIC8800_USB_STOP_TIMEOUT_MS     2000U
#define AIC8800_USB_RX_RETRY_FAST_MAX      3U
#define AIC8800_USB_RX_RETRY_SHORT_MAX    16U
#define AIC8800_USB_RX_RETRY_FAST_MS       1U
#define AIC8800_USB_RX_RETRY_SHORT_MS      5U
#define AIC8800_USB_RX_RETRY_LONG_MS      20U
#define AIC8800_USB_RX_SUBMIT_RETRIES      8U
#define AIC8800_USB_RX_WAIT_MS            100U
#define AIC8800_USB_TX_AGGREGATE_PREFIX     4U
#define AIC8800_USB_PID_MSC               0x5721
#define AIC8800_USB_VENDOR_ID_MSC_FACTORY 0x1111
#define AIC8800_USB_PID_MSC_FACTORY       0x1111
#define AIC8800_USB_MSC_SUBCLASS_SCSI       0x06
#define AIC8800_USB_MSC_PROTOCOL_BULK_ONLY  0x50
#define AIC8800_USB_MSC_REQUEST_GET_MAX_LUN 0xfe
#define AIC8800_USB_MODESWITCH_TIMEOUT_MS  3000U
#define AIC8800_USB_MODESWITCH_CBW_SIZE      31U
#define AIC8800_USB_MODESWITCH_CSW_SIZE      13U
#define AIC8800_USB_MODESWITCH_DATA_SIZE   2048U
#define AIC8800_USB_MODESWITCH_READY_TRIES    8U
#define AIC8800_USB_MODESWITCH_READY_WAIT_MS 50U
#define AIC8800_USB_MODESWITCH_SETTLE_MS    500U

/* A bulk OUT request that has not completed after this long is treated as
 * lost.  The transmit pool only has AIC8800_WIFI_DATA_TX_URBS slots, so a
 * single request the host controller never gives back drains the pool and
 * kills transmit for good: the slot semaphore is released only from the
 * completion callback.  Losing an outbound frame costs one retransmit;
 * leaving the endpoint wedged costs the link. */
#ifndef AIC8800_WIFI_TX_WATCHDOG_MS
#define AIC8800_WIFI_TX_WATCHDOG_MS       1000U
#endif
#define AIC8800_USB_TX_WATCHDOG_PERIOD_MS  250U

#define AIC8800_USB_DATA_RX_SLOT_COUNT       AIC8800_WIFI_DATA_RX_URBS
#define AIC8800_USB_MESSAGE_RX_SLOT_COUNT    AIC8800_WIFI_MESSAGE_RX_URBS
#define AIC8800_USB_RX_SPARES_PER_SLOT       AIC8800_WIFI_RX_SPARES_PER_SLOT

static struct aic8800_context g_aic8800_context;
static struct rt_workqueue *g_aic8800_attach_workqueue;
static struct rt_mutex g_aic8800_tx_mutex;
static struct rt_mutex g_aic8800_frame_mutex;
static struct rt_semaphore g_aic8800_tx_available;
static rt_bool_t g_aic8800_transport_ipc_initialized;
/* One forced port re-enumeration per attached fake-storage device. */
static rt_bool_t g_aic8800_modeswitch_rescanned;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX rt_uint8_t
    g_aic8800_modeswitch_cbw[AIC8800_USB_MODESWITCH_CBW_SIZE];

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX rt_uint8_t
    g_aic8800_modeswitch_csw[AIC8800_USB_MODESWITCH_CSW_SIZE];

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX rt_uint8_t
    g_aic8800_modeswitch_data[AIC8800_USB_MODESWITCH_DATA_SIZE];

#if defined(RT_USING_FINSH) && defined(AIC8800_WIFI_DEBUG_STATS)
#include <finsh.h>
#endif

static void aic8800_usb_stop_worker(struct aic8800_rx_worker *worker);
static void aic8800_usb_free_worker_buffer(
    struct aic8800_rx_worker *worker);
static void aic8800_usb_rx_complete(void *parameter, int length);
static void aic8800_usb_stop_tx(struct aic8800_tx_worker *worker);
static void aic8800_usb_free_tx_buffers(struct aic8800_tx_worker *worker);
static void aic8800_usb_tx_recovery_work(struct rt_work *work,
                                         void *work_data);
static void aic8800_usb_tx_watchdog_work(struct rt_work *work,
                                         void *work_data);
static void aic8800_usb_tx_watchdog(void *parameter);
static void aic8800_usb_tx_complete(void *parameter, int length);
static void aic8800_usb_attach_work(struct rt_work *work, void *work_data);
static rt_err_t aic8800_usb_transmit_data(
    struct aic8800_context *context, const void *data, rt_size_t length,
    const struct aic8800_tx_metadata *metadata, rt_size_t metadata_count,
    rt_bool_t *metadata_consumed);
static rt_bool_t aic8800_usb_supports_tx_aggregation(
    const struct aic8800_context *context);
static rt_err_t aic8800_usb_start_tx_queue(
    struct aic8800_context *context);
static void aic8800_usb_stop_tx_queue(struct aic8800_context *context);
static void aic8800_usb_free_tx_queue(struct aic8800_context *context);

#if defined(RT_USING_FINSH) && defined(AIC8800_WIFI_DEBUG_STATS)
static int aic8800_stat(int argc, char **argv)
{
    const struct aic8800_context *context = &g_aic8800_context;
    const struct aic8800_tx_worker *tx = &context->tx_worker;
    const struct aic8800_rx_worker *data = &context->data_worker;
    const struct aic8800_rx_worker *message = &context->message_worker;

    (void)argc;
    (void)argv;
    rt_kprintf("AIC state: transport=%d attached=%d station=%d\n",
               context->transport_connected, context->attached,
               context->station_connected);
    rt_kprintf("Station: vif=%u sta=%u port=%s pending=%d sets=%u errors=%u\n",
               context->vif_index, context->ap_station_index,
               context->station_control_port_open ? "open" : "closed",
               context->station_control_port_pending,
               (unsigned int)context->station_control_port_set_count,
               (unsigned int)context->station_control_port_error_count);
    rt_kprintf("TX window: full=%u ps-drops=%u ap-sta=%u held=%d\n",
               (unsigned int)context->tx_pending_full_count,
               (unsigned int)context->tx_power_save_drop_count,
               context->ap_stations[0].valid &&
                   context->ap_stations[0].firmware_index <
                       AIC8800_STATION_SLOTS ?
                   context->tx_pending[
                       context->ap_stations[0].firmware_index] : 0U,
               context->ap_stations[0].valid &&
                   context->ap_stations[0].firmware_index <
                       AIC8800_STATION_SLOTS ?
                   context->tx_pending_held[
                       context->ap_stations[0].firmware_index] : 0);
    rt_kprintf("TX credits (observed only): reported=%d updates=%u "
               "sta=%d bcmc=%d\n",
               context->tx_credits_tracked,
               (unsigned int)context->tx_credit_update_count,
               context->ap_station_index < AIC8800_STATION_SLOTS ?
                   context->tx_credits[context->ap_station_index] : -1,
               context->ap_broadcast_station_index <
                   AIC8800_STATION_SLOTS ?
                   context->tx_credits[
                       context->ap_broadcast_station_index] : -1);
    rt_kprintf("Channel ctx: station=%u ap=%u active=%u tracked=%d\n",
               (unsigned int)context->station_channel_index,
               (unsigned int)context->ap_channel_index,
               (unsigned int)context->active_channel_index,
               context->channel_context_tracked);
    rt_kprintf("NET TX: frames=%u errors=%u no-station=%u off-channel=%u "
               "arp=%u icmp=%u\n",
               (unsigned int)context->ethernet_tx_count,
               (unsigned int)context->ethernet_tx_error_count,
               (unsigned int)context->tx_no_station_count,
               (unsigned int)context->tx_off_channel_count,
               (unsigned int)context->arp_tx_count,
               (unsigned int)context->icmp_tx_count);
    rt_kprintf("NET RX: frames=%u errors=%u arp=%u icmp=%u\n",
               (unsigned int)context->ethernet_rx_count,
               (unsigned int)context->ethernet_rx_error_count,
               (unsigned int)context->arp_rx_count,
               (unsigned int)context->icmp_rx_count);
#ifdef AIC8800_WIFI_TCP_ACK_FILTER
    rt_kprintf("TCP ACK: suppressed=%u flushed=%u\n",
               (unsigned int)context->tcp_ack_suppressed,
               (unsigned int)context->tcp_ack_flushed);
#endif
    rt_kprintf("ICMP TX check: malformed=%u ip-csum=%u icmp-csum=%u\n",
               (unsigned int)context->icmp_tx_malformed_count,
               (unsigned int)context->icmp_tx_ip_checksum_error_count,
               (unsigned int)context->icmp_tx_checksum_error_count);
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
    rt_kprintf("USB TX: active=%d slots=%u pending=%u complete=%u errors=%u "
               "waits=%u timeouts=%u watchdog=%u reclaims=%u recoveries=%u "
               "max_burst=%u last=%d\n",
               tx->active, (unsigned int)tx->slot_count,
               (unsigned int)tx->pending,
               (unsigned int)tx->completion_count,
               (unsigned int)tx->error_count,
               (unsigned int)tx->wait_count,
               (unsigned int)tx->timeout_count,
               (unsigned int)tx->watchdog_count,
               (unsigned int)tx->reclaim_count,
               (unsigned int)tx->recovery_count,
               (unsigned int)tx->max_burst_count, tx->last_error);
    rt_kprintf("USB TX queue: enabled=%d aggregate=%d frames=%u aggregates=%u "
               "max=%u high=%u drops=%u errors=%u\n",
               context->usb_tx_queue_enabled,
               context->usb_tx_aggregation_enabled,
               (unsigned int)context->usb_tx_frame_count,
               (unsigned int)context->usb_tx_aggregate_count,
               (unsigned int)context->usb_tx_max_aggregate,
               (unsigned int)context->usb_tx_queue_high_water,
               (unsigned int)context->usb_tx_queue_drop_count,
               (unsigned int)context->usb_tx_error_count);
    rt_kprintf("USB RX data: slots=%u active=%d complete=%u rearm=%u "
               "complete_rearm=%u spare=%u/%u empty=%u errors=%u retries=%u "
               "backlog_high=%u assembly=%u overflow=%d\n",
               (unsigned int)data->slot_count, data->active,
               (unsigned int)data->completion_count,
               (unsigned int)data->rearm_count,
               (unsigned int)data->complete_rearm_count,
               (unsigned int)data->spare_available,
               (unsigned int)data->spare_count,
               (unsigned int)data->spare_empty_count,
               (unsigned int)data->error_count,
               (unsigned int)data->retry_count,
               (unsigned int)data->queue_high_water,
               (unsigned int)data->assembly_length,
               data->queue_overflow);
    rt_kprintf("USB RX msg: slots=%u active=%d complete=%u rearm=%u "
               "complete_rearm=%u spare=%u/%u empty=%u errors=%u retries=%u "
               "backlog_high=%u assembly=%u overflow=%d\n",
               (unsigned int)message->slot_count, message->active,
               (unsigned int)message->completion_count,
               (unsigned int)message->rearm_count,
               (unsigned int)message->complete_rearm_count,
               (unsigned int)message->spare_available,
               (unsigned int)message->spare_count,
               (unsigned int)message->spare_empty_count,
               (unsigned int)message->error_count,
               (unsigned int)message->retry_count,
               (unsigned int)message->queue_high_water,
               (unsigned int)message->assembly_length,
               message->queue_overflow);
    aic8800_core_print_rc_stats(&g_aic8800_context);
    return 0;
}
MSH_CMD_EXPORT(aic8800_stat, show AIC8800 USB and network counters);
#endif

#if AIC8800_WIFI_TX_TRACE_FRAMES && defined(RT_USING_FINSH)
static int aic8800_txtrace(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    aic8800_core_dump_tx_trace(&g_aic8800_context, "on demand");
    return 0;
}
MSH_CMD_EXPORT(aic8800_txtrace, dump the recent AIC8800 transmit descriptors);
#endif

static struct aic8800_context *aic8800_context_from_bus(
    struct rt_wlan_offload_bus *bus)
{
    return bus ? rt_wlan_offload_bus_get_driver_data(bus) : RT_NULL;
}

static rt_err_t aic8800_usb_result(int result)
{
    if (!result)
    {
        return RT_EOK;
    }
    if (aic8800_usb_is_timeout(result))
    {
        return -RT_ETIMEOUT;
    }
    if (result == -USB_ERR_NODEV || result == -USB_ERR_SHUTDOWN)
    {
        return -RT_EIO;
    }
    return -RT_EIO;
}

static const char *aic8800_usb_speed_name(rt_uint8_t speed)
{
    switch (speed)
    {
    case USB_SPEED_HIGH:
        return "high-speed";
    case USB_SPEED_FULL:
        return "full-speed";
    case USB_SPEED_LOW:
        return "low-speed";
    default:
        return "unknown-speed";
    }
}

static rt_bool_t aic8800_usb_object_is(const struct rt_object *object,
                                       enum rt_object_class_type type)
{
    return object && rt_object_get_type((rt_object_t)object) == type;
}

static rt_bool_t aic8800_usb_mutex_is_attached(const struct rt_mutex *mutex)
{
    return aic8800_usb_object_is(&mutex->parent.parent,
                                 RT_Object_Class_Mutex);
}

static void aic8800_usb_clear_tx_locks(struct aic8800_context *context)
{
    /* These locks live for the lifetime of the USB class driver.  A traffic
     * thread can be handed a mutex while disconnect is quiescing the bus, so
     * detaching and immediately reinitializing an embedded mutex corrupts
     * RT-Thread's intrusive object list on the next enumeration.
     */
    context->frame_mutex = RT_NULL;
    context->frame_mutex_initialized = RT_FALSE;
    context->tx_mutex = RT_NULL;
    context->tx_mutex_initialized = RT_FALSE;
}

static rt_uint32_t aic8800_usb_context_resource_mask(
    const struct aic8800_context *context)
{
    rt_uint32_t resources = 0;
    int index;

    if (aic8800_usb_mutex_is_attached(&context->bus.state_lock) ||
        aic8800_usb_mutex_is_attached(&context->bus.tx_lock))
    {
        resources |= 1U << 0;
    }
    if (aic8800_usb_mutex_is_attached(&context->commands.lock))
    {
        resources |= 1U << 1;
    }
    if (aic8800_usb_mutex_is_attached(&context->radio.command_lock) ||
        aic8800_usb_mutex_is_attached(&context->radio.data_lock) ||
        aic8800_usb_mutex_is_attached(&context->radio.operation_lock))
    {
        resources |= 1U << 2;
    }
    if (aic8800_usb_mutex_is_attached(&context->mgmt_confirmation_mutex) ||
        aic8800_usb_object_is(&context->mgmt_confirmation_timer.parent,
                              RT_Object_Class_Timer) ||
        aic8800_usb_mutex_is_attached(&context->rx_reorder_mutex) ||
        aic8800_usb_object_is(&context->rx_reorder_timer.parent,
                              RT_Object_Class_Timer) ||
        aic8800_usb_object_is(&context->tx_watchdog_timer.parent,
                              RT_Object_Class_Timer))
    {
        resources |= 1U << 3;
    }
    if (context->attached || context->bus_initialized ||
        context->attach_work_initialized ||
        context->tx_recovery_work_initialized ||
        context->tx_watchdog_work_initialized ||
        context->tx_watchdog_timer_initialized ||
        context->scan_work_initialized || context->scan_work_queued ||
        context->rx_reorder_slots || context->rx_reorder_work_initialized ||
        context->rx_reorder_work_queued ||
        context->radio.recovery_work_initialized ||
        context->radio.recovery_queued || context->radio.control
#ifdef RT_WLAN_OFFLOAD_EMBEDDED_WPA2
        || context->radio.supplicant
#endif
       )
    {
        resources |= 1U << 4;
    }
    if (context->tx_worker.active || context->tx_worker.pending ||
        context->tx_worker.slots || context->usb_tx_thread ||
        context->usb_tx_queue || context->usb_tx_pool ||
        context->usb_tx_aggregate_buffer)
    {
        resources |= 1U << 5;
    }
    if (context->data_worker.active || context->data_worker.thread ||
        context->data_worker.completed || context->data_worker.slots ||
        context->data_worker.buffers || context->data_worker.assembly ||
        context->message_worker.active || context->message_worker.thread ||
        context->message_worker.completed || context->message_worker.slots ||
        context->message_worker.buffers || context->message_worker.assembly)
    {
        resources |= 1U << 6;
    }
    for (index = 0; index < RT_WLAN_OFFLOAD_WLAN_VIF_COUNT; index++)
    {
        const struct rt_wlan_device *wlan =
            &context->radio.vifs[index].wlan;

        if (aic8800_usb_mutex_is_attached(&wlan->lock) ||
            aic8800_usb_object_is(&wlan->device.parent,
                                  RT_Object_Class_Device) ||
            wlan->device.ref_count || wlan->prot || wlan->netdev)
        {
            resources |= 1U << 7;
        }
    }
    return resources;
}

static rt_bool_t aic8800_usb_reset_context(struct aic8800_context *context,
                                           const char *phase)
{
    rt_uint32_t resources = aic8800_usb_context_resource_mask(context);

    if (resources)
    {
        /* Never erase intrusive RT object links or storage still reachable by
         * a worker.  Keeping the context unavailable is preferable to
         * corrupting a global object list on the next enumeration. */
        LOG_E("AIC USB context is still live after %s (resources=0x%02x)",
              phase, (unsigned int)resources);
        context->hport = RT_NULL;
        return RT_FALSE;
    }
    rt_memset(context, 0, sizeof(*context));
    return RT_TRUE;
}

/* Retires both transmit maintenance jobs.  Order matters: stop the timer
 * before the work it feeds, otherwise a tick between the two calls can queue
 * work onto a structure that is about to be declared dead. */
static void aic8800_usb_cancel_tx_maintenance(
    struct aic8800_context *context)
{
    if (!context)
    {
        return;
    }
    if (context->tx_watchdog_timer_initialized)
    {
        rt_timer_stop(&context->tx_watchdog_timer);
        rt_timer_detach(&context->tx_watchdog_timer);
        context->tx_watchdog_timer_initialized = RT_FALSE;
    }
    if (context->tx_watchdog_work_initialized)
    {
        rt_work_cancel_sync(&context->tx_watchdog_work);
        context->tx_watchdog_work_initialized = RT_FALSE;
        context->tx_watchdog_work_queued = RT_FALSE;
    }
    if (context->tx_recovery_work_initialized)
    {
        rt_work_cancel_sync(&context->tx_recovery_work);
        context->tx_recovery_work_initialized = RT_FALSE;
    }
}

static rt_bool_t aic8800_usb_rx_completion_terminal(
    const struct aic8800_context *context, int result)
{
    if (!context || !context->transport_connected || !context->hport ||
        !context->hport->connected)
    {
        return RT_TRUE;
    }

    /* Cancellation and disconnect completions must not be submitted again.
     * All other host-controller errors, including EPROTO transaction errors,
     * are recoverable by posting a fresh receive request. */
    return result == -USB_ERR_NODEV || result == -USB_ERR_NOTCONN ||
           result == -USB_ERR_SHUTDOWN || result == -ENOENT ||
           result == -ECONNRESET || result == -ESHUTDOWN;
}

static rt_bool_t aic8800_usb_is_protocol_error(int result)
{
    return result == -71
#ifdef EPROTO
           || result == -EPROTO
#endif
           ;
}

static rt_bool_t aic8800_usb_is_stall(int result)
{
    return result == -USB_ERR_STALL || result == -32
#ifdef EPIPE
           || result == -EPIPE
#endif
           ;
}

/* Errors worth clearing the bulk OUT endpoint for.  A stall needs the halt
 * cleared before the device will accept anything again, and an I/O error such
 * as the descriptor-DMA AHB fault leaves the host channel in a state the
 * controller will not reuse.  Both are endpoint-local: neither is a reason to
 * tear the device down, and both are unrecoverable if left alone. */
static rt_bool_t aic8800_usb_tx_needs_recovery(int result)
{
    return aic8800_usb_is_stall(result) || result == -USB_ERR_IO ||
           result == -USB_ERR_TIMEOUT || result == -RT_ETIMEOUT;
}

static rt_bool_t aic8800_usb_rx_submit_retryable(int result)
{
    return result == -USB_ERR_BUSY || result == -USB_ERR_NOMEM ||
           result == -USB_ERR_TIMEOUT || result == -RT_ETIMEOUT ||
           aic8800_usb_is_protocol_error(result) ||
           aic8800_usb_is_stall(result);
}

static rt_uint32_t aic8800_usb_rx_retry_delay(
    const struct aic8800_rx_worker *worker)
{
    if (worker->consecutive_errors <= AIC8800_USB_RX_RETRY_FAST_MAX)
    {
        return AIC8800_USB_RX_RETRY_FAST_MS;
    }
    if (worker->consecutive_errors <= AIC8800_USB_RX_RETRY_SHORT_MAX)
    {
        return AIC8800_USB_RX_RETRY_SHORT_MS;
    }
    return AIC8800_USB_RX_RETRY_LONG_MS;
}

static int aic8800_usb_clear_halt(struct aic8800_context *context,
                                  rt_uint8_t endpoint)
{
    struct usb_setup_packet setup;

    if (!context || !context->hport || !context->hport->connected)
    {
        return -USB_ERR_NODEV;
    }

    rt_memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD |
                          USB_REQUEST_RECIPIENT_ENDPOINT;
    setup.bRequest = USB_REQUEST_CLEAR_FEATURE;
    setup.wValue = USB_FEATURE_ENDPOINT_HALT;
    setup.wIndex = endpoint;
    setup.wLength = 0;
    return usbh_control_transfer(context->hport, &setup, RT_NULL);
}

static void aic8800_usb_tx_recovery_work(struct rt_work *work,
                                         void *work_data)
{
    struct aic8800_context *context = work_data;
    struct aic8800_tx_worker *worker;
    rt_err_t result;

    (void)work;
    if (!context)
    {
        return;
    }
    worker = &context->tx_worker;
    if (!context->transport_connected || !worker->active ||
        !context->data_out || !context->hport ||
        !context->hport->connected ||
        !aic8800_usb_tx_needs_recovery(worker->last_error))
    {
        return;
    }
    result = rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return;
    }
    if (context->transport_connected && worker->active)
    {
        result = aic8800_usb_clear_halt(
            context, context->data_out->bEndpointAddress);
    }
    else
    {
        result = -RT_EIO;
    }
    rt_mutex_release(context->tx_mutex);
    if (result == RT_EOK)
    {
        worker->recovery_count++;
        LOG_I("bulk OUT ep 0x%02x recovered (attempts=%u)",
              context->data_out->bEndpointAddress,
              (unsigned int)worker->recovery_count);
    }
    else if (context->transport_connected)
    {
        LOG_E("bulk OUT ep 0x%02x recovery failed: %d",
              context->data_out->bEndpointAddress, result);
        rt_wlan_offload_bus_notify(&context->bus, RT_WLAN_OFFLOAD_BUS_EVENT_ERROR,
                              aic8800_usb_result(result));
    }
}

/* True once a slot has been in flight longer than the watchdog allows.  Read
 * with interrupts disabled by the caller: the completion callback retires both
 * fields together, and a torn read would either spare a stuck request for one
 * more period or cancel a healthy one. */
static rt_bool_t aic8800_usb_tx_slot_expired(
    const struct aic8800_tx_slot *slot, rt_tick_t now, rt_tick_t limit)
{
    if (!slot->in_use || !slot->submitted || slot->cancelling)
    {
        return RT_FALSE;
    }
    return (rt_tick_t)(now - slot->submit_tick) > limit;
}

/* Soft-timer context: only decide whether there is work to do.  Cancelling a
 * request blocks until the controller has released it, which must not happen
 * on the shared timer thread. */
static void aic8800_usb_tx_watchdog(void *parameter)
{
    struct aic8800_context *context = parameter;
    struct aic8800_tx_worker *worker;
    rt_tick_t now;
    rt_tick_t limit;
    rt_bool_t expired = RT_FALSE;
    rt_base_t level;

    if (!context || !context->tx_watchdog_work_initialized ||
        context->tx_watchdog_work_queued)
    {
        return;
    }
    worker = &context->tx_worker;
    if (!worker->active || !worker->slots || !worker->pending)
    {
        return;
    }
    now = rt_tick_get();
    limit = rt_tick_from_millisecond(AIC8800_WIFI_TX_WATCHDOG_MS);

    level = rt_hw_interrupt_disable();
    for (rt_size_t index = 0; index < worker->slot_count; index++)
    {
        if (aic8800_usb_tx_slot_expired(&worker->slots[index], now, limit))
        {
            expired = RT_TRUE;
            break;
        }
    }
    rt_hw_interrupt_enable(level);
    if (!expired)
    {
        return;
    }
    context->tx_watchdog_work_queued = RT_TRUE;
    if (rt_work_submit(&context->tx_watchdog_work, 0) != RT_EOK)
    {
        context->tx_watchdog_work_queued = RT_FALSE;
    }
}

static void aic8800_usb_tx_watchdog_work(struct rt_work *work,
                                         void *work_data)
{
    struct aic8800_context *context = work_data;
    struct aic8800_tx_worker *worker;
    rt_tick_t now;
    rt_tick_t limit;
    rt_bool_t recovered = RT_FALSE;

    (void)work;
    if (!context)
    {
        return;
    }
    context->tx_watchdog_work_queued = RT_FALSE;
    worker = &context->tx_worker;
    if (!context->transport_connected || !worker->active || !worker->slots)
    {
        return;
    }
    now = rt_tick_get();
    limit = rt_tick_from_millisecond(AIC8800_WIFI_TX_WATCHDOG_MS);

    for (rt_size_t index = 0; index < worker->slot_count; index++)
    {
        struct aic8800_tx_slot *slot = &worker->slots[index];
        rt_bool_t expired;
        rt_tick_t stuck_since;
        rt_base_t level;

        level = rt_hw_interrupt_disable();
        expired = aic8800_usb_tx_slot_expired(slot, now, limit);
        stuck_since = slot->submit_tick;
        if (expired)
        {
            /* Giveback retires in_use before usbh_kill_urb() drops
             * urb->reject. Keep this slot unavailable across that window. */
            slot->cancelling = RT_TRUE;
        }
        rt_hw_interrupt_enable(level);
        if (!expired)
        {
            continue;
        }
#ifdef AIC8800_WIFI_DEBUG_STATS
        AIC8800_STAT(worker->watchdog_count++);
        LOG_W("bulk OUT ep 0x%02x request stuck for %u ms; cancelling "
              "(watchdog=%u pending=%u)",
              worker->endpoint ? worker->endpoint->bEndpointAddress : 0,
              (unsigned int)((rt_tick_t)(now - stuck_since) * 1000U /
                             RT_TICK_PER_SECOND),
              (unsigned int)worker->watchdog_count,
              (unsigned int)worker->pending);
#else
        LOG_W("bulk OUT ep 0x%02x request stuck for %u ms; cancelling "
              "(pending=%u len=%u sta=%u mps-multiple=%d)",
              worker->endpoint ? worker->endpoint->bEndpointAddress : 0,
              (unsigned int)((rt_tick_t)(now - stuck_since) * 1000U /
                             RT_TICK_PER_SECOND),
              (unsigned int)worker->pending,
              (unsigned int)slot->length,
              slot->metadata_count ?
                  (unsigned int)slot->metadata[0].station_index :
                  (unsigned int)AIC8800_INVALID_INDEX,
              worker->endpoint &&
                  !(slot->length % USB_GET_MAXPACKETSIZE(
                        worker->endpoint->wMaxPacketSize)));
#endif
        /* Never hold tx_mutex here.  usbh_kill_urb() halts the channel and
         * then blocks until giveback has run, and giveback runs on the host
         * controller's bottom-half thread. */
        usbh_kill_urb(&slot->urb);
        /* Reclaim only while this is still the same stuck request.  Giveback
         * may have landed while the cancel was in flight and a producer may
         * already own the slot again; forcing a completion then would retire
         * a healthy transfer instead.  A reacquired slot is marked in use
         * before its tick is restamped, so the tick alone does not tell the
         * two apart: require the submitted flag as well, exactly as
         * aic8800_usb_tx_slot_expired() does. */
        level = rt_hw_interrupt_disable();
        expired = slot->in_use && slot->submitted &&
                  slot->submit_tick == stuck_since;
        slot->cancelling = RT_FALSE;
        rt_hw_interrupt_enable(level);
        if (expired)
        {
#ifdef AIC8800_WIFI_DEBUG_STATS
            AIC8800_STAT(worker->reclaim_count++);
            LOG_W("bulk OUT ep 0x%02x request not returned after cancel; "
                  "reclaiming slot (reclaims=%u)",
                  worker->endpoint ? worker->endpoint->bEndpointAddress : 0,
                  (unsigned int)worker->reclaim_count);
#else
            LOG_W("bulk OUT ep 0x%02x request not returned after cancel; "
                  "reclaiming slot",
                  worker->endpoint ? worker->endpoint->bEndpointAddress : 0);
#endif
            aic8800_usb_tx_complete(slot, -USB_ERR_TIMEOUT);
        }
        recovered = RT_TRUE;
    }
    if (!recovered)
    {
        return;
    }
#if AIC8800_WIFI_TX_TRACE_FRAMES
    /* First stuck request on this bus: dump what the firmware was last given.
     * Once only, so a wedged endpoint cannot flood the console. */
    if (!context->tx_trace_dumped)
    {
        context->tx_trace_dumped = RT_TRUE;
        aic8800_core_dump_tx_trace(context, "bulk OUT stuck");
    }
#endif
    /* The endpoint carried a request the controller could not finish, so its
     * halt and data toggle are both suspect.  Clear them before the reclaimed
     * slot is handed to the next frame.  A cancelled request gives back as
     * USB_ERR_SHUTDOWN, which on its own reads as an ordinary teardown, so
     * record why the transfer actually died before asking for recovery. */
    worker->last_error = -USB_ERR_TIMEOUT;
    if (context->tx_recovery_work_initialized)
    {
        rt_work_submit(&context->tx_recovery_work, 0);
    }
}

static rt_uint16_t aic8800_usb_get_length(const rt_uint8_t *data)
{
    return ((rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8)) &
           AIC_USB_LENGTH_MASK;
}

static rt_size_t aic8800_usb_align4(rt_size_t length)
{
    return (length + 3U) & ~(rt_size_t)3U;
}

static rt_err_t aic8800_usb_append_rx(struct aic8800_rx_worker *worker,
                                      const rt_uint8_t *data,
                                      rt_size_t length)
{
    if (!worker->assembly || length > worker->assembly_capacity -
                                      worker->assembly_length)
    {
        worker->assembly_length = 0;
        worker->padding_length = 0;
        return -RT_EFULL;
    }
    rt_memcpy(worker->assembly + worker->assembly_length, data, length);
    worker->assembly_length += length;
    return RT_EOK;
}

static rt_err_t aic8800_usb_decode_records(struct aic8800_rx_worker *worker,
                                           rt_uint8_t *buffer,
                                           rt_size_t *length,
                                           rt_size_t *padding_length)
{
    struct aic8800_context *context = worker->context;
    rt_err_t last_result = RT_EOK;

    /* Bulk transfers may end between a record and its protocol padding.  Keep
     * that padding length across URBs so the first bytes of the next URB are
     * never interpreted as a new AIC header. */
    if (*padding_length)
    {
        if (*length < *padding_length)
        {
            *padding_length -= *length;
            *length = 0;
            return RT_EOK;
        }
        *length -= *padding_length;
        if (*length)
        {
            rt_memmove(buffer, buffer + *padding_length, *length);
        }
        *padding_length = 0;
    }
    while (*length >= AIC8800_USB_HEADER_SIZE)
    {
        rt_uint16_t packet_length = aic8800_usb_get_length(buffer);
        rt_uint8_t type = buffer[2] & 0x7fU;
        rt_bool_t config_record =
            (type & AIC_USB_TYPE_CONFIG) == AIC_USB_TYPE_CONFIG;
        rt_size_t raw_length;
        rt_size_t wire_length;
        rt_size_t consumed;
        rt_err_t result;

        if (!packet_length && !type)
        {
            rt_memmove(buffer, buffer + 1, --(*length));
            continue;
        }
        raw_length = config_record ?
                     (rt_size_t)packet_length + AIC8800_USB_HEADER_SIZE :
                     (rt_size_t)packet_length + AIC8800_USB_RX_HEADER_SIZE;
        /* The AIC wire format aligns only configuration payloads.  RX data
         * records are exactly packet_length + RX_HWHRD_LEN bytes.  Rounding a
         * 193-byte data record to 196 bytes discards the first three bytes of
         * the next URB, which is why EAPOL message 1/4 disappeared after a
         * successful association.
         */
        wire_length = config_record ?
                      AIC8800_USB_HEADER_SIZE +
                          aic8800_usb_align4(packet_length) :
                      raw_length;
        if (raw_length < AIC8800_USB_HEADER_SIZE ||
            wire_length > AIC8800_USB_MAX_RECORD_SIZE)
        {
            *length = 0;
            *padding_length = 0;
            return -RT_EIO;
        }
        if (*length < raw_length)
        {
            break;
        }

        consumed = raw_length;
        if (wire_length != raw_length)
        {
            if (*length >= wire_length)
            {
                consumed = wire_length;
            }
            else
            {
                *padding_length = wire_length - raw_length;
            }
        }
        result = rt_wlan_offload_bus_rx(&context->bus, buffer, raw_length);
        if (result != RT_EOK && result != -RT_EEMPTY)
        {
            last_result = result;
        }
        *length -= consumed;
        if (*length)
        {
            rt_memmove(buffer, buffer + consumed, *length);
        }
        if (*padding_length)
        {
            break;
        }
    }
    return last_result;
}

static rt_err_t aic8800_usb_process_rx(struct aic8800_rx_worker *worker)
{
    return aic8800_usb_decode_records(worker, worker->assembly,
                                      &worker->assembly_length,
                                      &worker->padding_length);
}

static rt_uint8_t *aic8800_usb_take_spare(struct aic8800_rx_worker *worker)
{
    rt_uint8_t *buffer = RT_NULL;
    rt_base_t level;

    if (!worker)
    {
        return RT_NULL;
    }
    level = rt_hw_interrupt_disable();
    if (worker->spare_available)
    {
        worker->spare_available--;
        buffer = worker->spare[worker->spare_available];
        worker->spare[worker->spare_available] = RT_NULL;
    }
    rt_hw_interrupt_enable(level);
    return buffer;
}

static void aic8800_usb_give_spare(struct aic8800_rx_worker *worker,
                                   rt_uint8_t *buffer)
{
    rt_base_t level;

    if (!worker || !buffer || !worker->spare)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    if (worker->spare_available < worker->spare_count)
    {
        worker->spare[worker->spare_available++] = buffer;
        buffer = RT_NULL;
    }
    rt_hw_interrupt_enable(level);
    if (buffer)
    {
        /* Pool is already full; the extra buffer is still owned via
         * worker->buffers and will be freed on teardown. */
    }
}

static int aic8800_usb_submit_rx_slot(struct aic8800_rx_slot *slot)
{
    struct aic8800_rx_worker *worker;
    int result;

    if (!slot || !slot->worker || !slot->worker->context || !slot->buffer)
    {
        return -RT_EINVAL;
    }
    worker = slot->worker;
    if (!worker->active || !worker->context->transport_connected ||
        !worker->context->hport || !worker->context->hport->connected ||
        !worker->endpoint)
    {
        return -RT_EIO;
    }
    slot->length = 0;
    slot->result = 0;
    usbh_bulk_urb_fill(&slot->urb, worker->context->hport, worker->endpoint,
                       slot->buffer, AIC8800_WIFI_RX_BUFFER_SIZE, 0,
                       aic8800_usb_rx_complete, slot);
    result = usbh_submit_urb(&slot->urb);
    return result;
}

static void aic8800_usb_rx_complete(void *parameter, int length)
{
    struct aic8800_rx_slot *slot = parameter;
    struct aic8800_rx_worker *worker;
    struct aic8800_rx_done done;
    rt_uint8_t *spare;

    if (!slot || !slot->worker)
    {
        return;
    }
    worker = slot->worker;
    if (!worker->active || !worker->completed)
    {
        return;
    }

    done.slot = slot;
    done.buffer = slot->buffer;
    done.length = length > 0 ? (rt_size_t)length : 0;
    done.result = length < 0 ? length : 0;

    /* Giveback runs on the DWC2 low-priority workqueue (priority 1), ahead
     * of the RX worker.  Swap in a spare buffer and resubmit immediately so
     * the bulk IN pipe stays armed while the worker copies and decodes.
     * Errors stay on the original slot so the worker can recover it. */
    if (!done.result && worker->context &&
        worker->context->transport_connected && worker->context->hport &&
        worker->context->hport->connected && worker->endpoint)
    {
        spare = aic8800_usb_take_spare(worker);
        if (spare)
        {
            slot->buffer = spare;
            /* Re-check after the swap: stop_worker sets active false
             * then kills every slot URB.  Submitting past that point
             * would leave a live request after teardown. */
            if (worker->active && !aic8800_usb_submit_rx_slot(slot))
            {
                AIC8800_STAT(worker->complete_rearm_count++);
                done.slot = RT_NULL;
            }
            else
            {
                slot->buffer = done.buffer;
                aic8800_usb_give_spare(worker, spare);
            }
        }
        else
        {
            AIC8800_STAT(worker->spare_empty_count++);
        }
    }
    if (done.slot)
    {
        slot->result = done.result;
        slot->length = done.length;
    }

    if (rt_mq_send(worker->completed, &done, sizeof(done)) != RT_EOK)
    {
        if (!done.slot && done.buffer)
        {
            aic8800_usb_give_spare(worker, done.buffer);
        }
        worker->queue_overflow = RT_TRUE;
        worker->active = RT_FALSE;
    }
}

static rt_bool_t aic8800_usb_recover_rx_slot(
    struct aic8800_rx_worker *worker, struct aic8800_rx_slot *slot,
    int error, rt_bool_t completion_error)
{
    struct aic8800_context *context = worker->context;
    rt_uint32_t submit_attempts = 0;
    int result = error;

    if (completion_error &&
        aic8800_usb_rx_completion_terminal(context, error))
    {
        return RT_FALSE;
    }
    if (!completion_error && !aic8800_usb_rx_submit_retryable(error))
    {
        return RT_FALSE;
    }

    worker->assembly_length = 0;
    worker->padding_length = 0;
    worker->last_error = error;
    AIC8800_STAT(worker->error_count++);
    if (worker->consecutive_errors != UINT32_MAX)
    {
        worker->consecutive_errors++;
    }
    if (worker->consecutive_errors >=
        AIC8800_WIFI_RX_RECOVERY_ERRORS)
    {
        slot->result = error;
        LOG_E("bulk IN ep 0x%02x exceeded %u consecutive recovery errors",
              worker->endpoint->bEndpointAddress,
              (unsigned int)AIC8800_WIFI_RX_RECOVERY_ERRORS);
        return RT_FALSE;
    }

    /* EPROTO is a transaction error, not an endpoint halt.  Issuing a
     * synchronous CLEAR_FEATURE for it can block EP0 for the full control
     * timeout and starve both receive pipes.  A fresh URB is sufficient;
     * clear the endpoint only when the host reports an actual stall.
     */
    if (aic8800_usb_is_stall(error))
    {
        result = aic8800_usb_clear_halt(
            context, worker->endpoint->bEndpointAddress);
        if (result < 0)
        {
            LOG_W("failed to clear halt on bulk IN ep 0x%02x: %d",
                  worker->endpoint->bEndpointAddress, result);
        }
    }
    if (aic8800_log_throttle(worker->consecutive_errors))
    {
        LOG_W("bulk IN ep 0x%02x error %d; retrying (consecutive=%u)",
              worker->endpoint->bEndpointAddress, error,
              (unsigned int)worker->consecutive_errors);
    }

    while (worker->active && context->transport_connected && context->hport &&
           context->hport->connected)
    {
        rt_thread_mdelay(aic8800_usb_rx_retry_delay(worker));
        result = aic8800_usb_submit_rx_slot(slot);
        if (!result)
        {
            AIC8800_STAT(worker->retry_count++);
            return RT_TRUE;
        }
        if (!aic8800_usb_rx_submit_retryable(result))
        {
            slot->result = result;
            return RT_FALSE;
        }
        submit_attempts++;
        if (submit_attempts >= AIC8800_WIFI_RX_RECOVERY_ERRORS)
        {
            slot->result = result;
            LOG_E("bulk IN ep 0x%02x re-submit recovery exhausted (%d)",
                  worker->endpoint->bEndpointAddress, result);
            return RT_FALSE;
        }
        if (!(submit_attempts % AIC8800_USB_RX_SUBMIT_RETRIES))
        {
            /* Keep the slot alive while the controller recovers.  EPROTO is
             * retried with a fresh URB; only EPIPE/STALL requires an endpoint
             * halt clear. */
            if (aic8800_usb_is_stall(result))
            {
                if (aic8800_usb_clear_halt(
                        context, worker->endpoint->bEndpointAddress) < 0)
                {
                    LOG_W("bulk IN ep 0x%02x endpoint reset pending",
                          worker->endpoint->bEndpointAddress);
                }
                LOG_W("bulk IN ep 0x%02x transport recovery still pending (%d)",
                      worker->endpoint->bEndpointAddress, result);
                continue;
            }
            if (aic8800_usb_is_protocol_error(result))
            {
                LOG_W("bulk IN ep 0x%02x protocol recovery still pending (%d)",
                      worker->endpoint->bEndpointAddress, result);
                continue;
            }
            slot->result = result;
            return RT_FALSE;
        }
    }
    slot->result = result;
    return RT_FALSE;
}

static void aic8800_usb_note_rx_success(struct aic8800_rx_worker *worker)
{
    if (!worker->consecutive_errors)
    {
        return;
    }
    worker->recovery_count++;
    if (aic8800_log_throttle(worker->recovery_count))
    {
        LOG_I("bulk IN ep 0x%02x recovered after %u error(s); last=%d recoveries=%u",
              worker->endpoint->bEndpointAddress,
              (unsigned int)worker->consecutive_errors, worker->last_error,
              (unsigned int)worker->recovery_count);
    }
    worker->consecutive_errors = 0;
    worker->last_error = 0;
}

static void aic8800_usb_rx_thread(void *parameter)
{
    struct aic8800_rx_worker *worker = parameter;
    struct aic8800_context *context;

    if (!worker || !worker->context || !worker->completed)
    {
        return;
    }
    context = worker->context;
    while (worker->active && context->transport_connected)
    {
        struct aic8800_rx_done done;
        struct aic8800_rx_slot *slot;
        rt_bool_t completion_error = RT_TRUE;
        rt_err_t result;

        rt_memset(&done, 0, sizeof(done));
        result = rt_mq_recv(
            worker->completed, &done, sizeof(done),
            rt_tick_from_millisecond(AIC8800_USB_RX_WAIT_MS));
        if (result == -RT_ETIMEOUT)
        {
            continue;
        }
        if (!worker->active || !context->transport_connected)
        {
            if (!done.slot && done.buffer)
            {
                aic8800_usb_give_spare(worker, done.buffer);
            }
            break;
        }
        if (result != RT_EOK)
        {
            continue;
        }
#ifdef AIC8800_WIFI_DEBUG_STATS
        if (worker->completed->entry > worker->queue_high_water)
        {
            /* Completions left waiting behind this one: the endpoint is
             * outrunning this worker and its requests rearm late. */
            worker->queue_high_water = worker->completed->entry;
        }
#endif
        slot = done.slot;
        if (!done.result)
        {
            rt_size_t received_length = done.length;
            rt_err_t receive_result = RT_EOK;
            const rt_uint8_t *payload = done.buffer;

            aic8800_usb_note_rx_success(worker);
            AIC8800_STAT(worker->completion_count++);
            if (received_length && payload)
            {
                if (worker->completion_log_count < 16U)
                {
                    worker->completion_log_count++;
                    LOG_D("RX URB ep=0x%02x bytes=%u head=%02x %02x %02x %02x",
                          worker->endpoint->bEndpointAddress,
                          (unsigned int)received_length, payload[0],
                          payload[1], payload[2], payload[3]);
                }
                receive_result = aic8800_usb_append_rx(
                    worker, payload, received_length);
            }
            else
            {
                AIC8800_STAT(worker->zero_length_count++);
            }

            /* Giveback already resubmitted when done.slot is NULL.  Otherwise
             * this slot still owns the URB and must be rearmed here. */
            if (slot)
            {
                result = aic8800_usb_submit_rx_slot(slot);
                if (!result)
                {
                    AIC8800_STAT(worker->rearm_count++);
                }
                else
                {
                    slot->result = result;
                }
            }
            else
            {
                result = 0;
            }
            if (done.buffer && (!slot || done.buffer != slot->buffer))
            {
                aic8800_usb_give_spare(worker, done.buffer);
                done.buffer = RT_NULL;
            }

            if (receive_result == RT_EOK && received_length)
            {
                receive_result = aic8800_usb_process_rx(worker);
            }
            if (receive_result != RT_EOK &&
                receive_result != -RT_EEMPTY &&
                context->invalid_rx_log_count < 8)
            {
                context->invalid_rx_log_count++;
                LOG_W("discarded malformed USB frame on ep 0x%02x: %d",
                      worker->endpoint->bEndpointAddress, receive_result);
            }
            if (!result)
            {
                continue;
            }
            completion_error = RT_FALSE;
        }

        /* Disconnect cancellation is a normal terminal completion.  Reporting
         * it as a firmware failure races the hub disconnect callback with the
         * framework recovery worker and can restart RX while the USB context
         * is being destroyed.
         */
        if (!slot)
        {
            if (done.buffer)
            {
                aic8800_usb_give_spare(worker, done.buffer);
            }
            if (completion_error &&
                aic8800_usb_rx_completion_terminal(context, done.result))
            {
                break;
            }
            continue;
        }
        if (completion_error &&
            aic8800_usb_rx_completion_terminal(context, slot->result))
        {
            if (done.buffer && done.buffer != slot->buffer)
            {
                aic8800_usb_give_spare(worker, done.buffer);
            }
            break;
        }
        if (aic8800_usb_recover_rx_slot(worker, slot, slot->result,
                                        completion_error))
        {
            continue;
        }
        /* A retry can race the hub callback: the completion initially reports
         * EPROTO, then re-submission observes the disconnected port and
         * returns SHUTDOWN.  Treat that transition as normal teardown too. */
        if (aic8800_usb_rx_completion_terminal(context, slot->result))
        {
            break;
        }
        LOG_E("bulk IN ep 0x%02x stopped: %d",
              worker->endpoint->bEndpointAddress, slot->result);
        rt_wlan_offload_bus_notify(&context->bus, RT_WLAN_OFFLOAD_BUS_EVENT_ERROR,
                              aic8800_usb_result(slot->result));
        break;
    }
    worker->active = RT_FALSE;
    if (worker->queue_overflow && context->transport_connected)
    {
        LOG_E("USB RX completion queue overflow on ep 0x%02x",
              worker->endpoint ? worker->endpoint->bEndpointAddress : 0);
        rt_wlan_offload_bus_notify(&context->bus, RT_WLAN_OFFLOAD_BUS_EVENT_ERROR,
                              -RT_EFULL);
    }
    rt_completion_done(&worker->stopped);
}

static rt_err_t aic8800_usb_start_worker(
    struct aic8800_rx_worker *worker, struct aic8800_context *context,
    struct usb_endpoint_descriptor *endpoint, const char *name)
{
    rt_err_t result;

    if (!endpoint)
    {
        return RT_EOK;
    }
    rt_completion_init(&worker->stopped);
    worker->context = context;
    worker->endpoint = endpoint;
    worker->name = name;
    worker->assembly_length = 0;
    worker->padding_length = 0;
    AIC8800_STAT(worker->completion_count = 0);
    AIC8800_STAT(worker->zero_length_count = 0);
    AIC8800_STAT(worker->rearm_count = 0);
    AIC8800_STAT(worker->error_count = 0);
    AIC8800_STAT(worker->retry_count = 0);
    worker->recovery_count = 0;
    worker->consecutive_errors = 0;
    AIC8800_STAT(worker->queue_high_water = 0);
    worker->last_error = 0;
    worker->completion_log_count = 0;
    worker->queue_overflow = RT_FALSE;
    AIC8800_STAT(worker->complete_rearm_count = 0);
    AIC8800_STAT(worker->spare_empty_count = 0);
    /* Rebuild the spare list from every allocated buffer that is not
     * currently bound to a slot, so a previous run cannot leave the
     * pool empty after leftovers were only partially drained. */
    worker->spare_available = 0;
    if (worker->spare && worker->buffers)
    {
        for (rt_size_t index = 0; index < worker->buffer_count; index++)
        {
            rt_bool_t in_slot = RT_FALSE;

            for (rt_size_t slot_index = 0; slot_index < worker->slot_count;
                 slot_index++)
            {
                if (worker->slots[slot_index].buffer == worker->buffers[index])
                {
                    in_slot = RT_TRUE;
                    break;
                }
            }
            if (!in_slot && worker->spare_available < worker->spare_count)
            {
                worker->spare[worker->spare_available++] =
                    worker->buffers[index];
            }
        }
    }
    /* Complete-path rearm can post several refill waves before the
     * worker returns the first, so the queue must hold every spare. */
    worker->completed = rt_mq_create(name, sizeof(struct aic8800_rx_done),
                                     worker->slot_count + worker->spare_count,
                                     RT_IPC_FLAG_FIFO);
    if (!worker->completed)
    {
        return -RT_ENOMEM;
    }
    worker->active = RT_TRUE;
    worker->thread = rt_thread_create(
        name, aic8800_usb_rx_thread, worker,
        AIC8800_WIFI_RX_THREAD_STACK_SIZE,
        AIC8800_WIFI_RX_THREAD_PRIORITY, 10);
    if (!worker->thread)
    {
        worker->active = RT_FALSE;
        rt_mq_delete(worker->completed);
        worker->completed = RT_NULL;
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(worker->thread);
    if (result != RT_EOK)
    {
        rt_thread_delete(worker->thread);
        worker->thread = RT_NULL;
        worker->active = RT_FALSE;
        rt_mq_delete(worker->completed);
        worker->completed = RT_NULL;
        return result;
    }
    for (rt_size_t index = 0; index < worker->slot_count; index++)
    {
        result = aic8800_usb_result(
            aic8800_usb_submit_rx_slot(&worker->slots[index]));
        if (result != RT_EOK)
        {
            aic8800_usb_stop_worker(worker);
            return result;
        }
    }
    return RT_EOK;
}

static void aic8800_usb_stop_worker(struct aic8800_rx_worker *worker)
{
    if (!worker->thread)
    {
        worker->active = RT_FALSE;
        if (worker->completed)
        {
            struct aic8800_rx_done leftover;

            while (rt_mq_recv(worker->completed, &leftover, sizeof(leftover),
                              RT_WAITING_NO) == RT_EOK)
            {
                if (leftover.buffer &&
                    (!leftover.slot || leftover.buffer != leftover.slot->buffer))
                {
                    aic8800_usb_give_spare(worker, leftover.buffer);
                }
            }
            rt_mq_delete(worker->completed);
            worker->completed = RT_NULL;
        }
        return;
    }
    worker->active = RT_FALSE;
    for (rt_size_t index = 0; index < worker->slot_count; index++)
    {
        /* Kill also waits for a giveback which was already queued by the HCD
         * and therefore no longer has hcpriv. */
        usbh_kill_urb(&worker->slots[index].urb);
    }
    if (rt_completion_wait(
            &worker->stopped,
            rt_tick_from_millisecond(AIC8800_USB_STOP_TIMEOUT_MS)) != RT_EOK)
    {
        LOG_E("RX thread %s did not stop after USB cancellation; deleting it",
              worker->name);
        rt_thread_delete(worker->thread);
    }
    worker->thread = RT_NULL;
#ifdef AIC8800_WIFI_DEBUG_STATS
    LOG_I("RX ep=0x%02x stopped: completions=%u zero=%u rearms=%u complete_rearm=%u spare_empty=%u errors=%u retries=%u recoveries=%u backlog_high=%u pending=%u",
          worker->endpoint ? worker->endpoint->bEndpointAddress : 0,
          (unsigned int)worker->completion_count,
          (unsigned int)worker->zero_length_count,
          (unsigned int)worker->rearm_count,
          (unsigned int)worker->complete_rearm_count,
          (unsigned int)worker->spare_empty_count,
          (unsigned int)worker->error_count,
          (unsigned int)worker->retry_count,
          (unsigned int)worker->recovery_count,
          (unsigned int)worker->queue_high_water,
          (unsigned int)worker->assembly_length);
#endif
    if (worker->completed)
    {
        struct aic8800_rx_done leftover;

        while (rt_mq_recv(worker->completed, &leftover, sizeof(leftover),
                          RT_WAITING_NO) == RT_EOK)
        {
            if (leftover.buffer &&
                (!leftover.slot || leftover.buffer != leftover.slot->buffer))
            {
                aic8800_usb_give_spare(worker, leftover.buffer);
            }
        }
        rt_mq_delete(worker->completed);
        worker->completed = RT_NULL;
    }
    worker->assembly_length = 0;
    worker->padding_length = 0;
}

static void aic8800_usb_tx_complete(void *parameter, int length)
{
    struct aic8800_tx_slot *slot = parameter;
    struct aic8800_tx_worker *worker;
    rt_size_t expected;
    rt_base_t level;
    rt_bool_t failed;

    if (!slot || !slot->worker)
    {
        return;
    }
    worker = slot->worker;
    expected = slot->length;
    failed = length < 0 || (rt_size_t)length != expected;

    level = rt_hw_interrupt_disable();
    if (!slot->in_use)
    {
        rt_hw_interrupt_enable(level);
        return;
    }
    if (failed)
    {
        worker->error_count++;
        worker->last_error = length < 0 ? length : -RT_EIO;
    }
    else
    {
        AIC8800_STAT(worker->completion_count++);
        AIC8800_STAT(worker->byte_count += (rt_uint64_t)length);
    }
    if (worker->pending)
    {
        worker->pending--;
    }
    slot->length = 0;
    slot->submitted = RT_FALSE;
    slot->in_use = RT_FALSE;
    rt_hw_interrupt_enable(level);

    if (worker->context)
    {
        for (rt_size_t index = 0; index < slot->metadata_count; index++)
        {
            aic8800_core_tx_complete(
                worker->context, &slot->metadata[index]);
        }
    }
    slot->metadata_count = 0;

    if (worker->semaphore_initialized)
    {
        /* Read once: teardown may clear the field between the test and the
         * release. */
        rt_sem_t available = worker->available;

        if (available)
        {
            rt_sem_release(available);
        }
    }
    if (!worker->active && !worker->pending)
    {
        rt_completion_done(&worker->stopped);
    }
    if (failed && worker->active &&
        aic8800_log_throttle(worker->error_count))
    {
        LOG_W("bulk OUT ep 0x%02x completion failed: %d (errors=%u)",
              worker->endpoint ? worker->endpoint->bEndpointAddress : 0,
              worker->last_error, (unsigned int)worker->error_count);
    }
    if (failed && worker->active && worker->context &&
        worker->context->tx_recovery_work_initialized)
    {
        rt_work_submit(&worker->context->tx_recovery_work, 0);
    }
}

static rt_err_t aic8800_usb_start_tx(struct aic8800_tx_worker *worker,
                                     struct aic8800_context *context)
{
    if (!worker || !context || !context->data_out || !worker->slots ||
        !worker->slot_count || !worker->semaphore_initialized)
    {
        return -RT_EIO;
    }
    worker->context = context;
    worker->endpoint = context->data_out;
    worker->pending = 0;
    AIC8800_STAT(worker->completion_count = 0);
    AIC8800_STAT(worker->byte_count = 0);
    worker->error_count = 0;
    AIC8800_STAT(worker->wait_count = 0);
    worker->timeout_count = 0;
    worker->recovery_count = 0;
    AIC8800_STAT(worker->watchdog_count = 0);
    AIC8800_STAT(worker->reclaim_count = 0);
    AIC8800_STAT(worker->burst_count = 0);
    AIC8800_STAT(worker->max_burst_count = 0);
    worker->next_slot = 0;
    worker->last_error = 0;
    rt_completion_init(&worker->stopped);
    worker->completion_initialized = RT_TRUE;
    for (rt_size_t index = 0; index < worker->slot_count; index++)
    {
        worker->slots[index].length = 0;
        worker->slots[index].submitted = RT_FALSE;
        worker->slots[index].cancelling = RT_FALSE;
        worker->slots[index].submit_tick = 0;
        worker->slots[index].in_use = RT_FALSE;
        worker->slots[index].metadata_count = 0;
    }
    rt_sem_control(worker->available, RT_IPC_CMD_RESET,
                   (void *)worker->slot_count);
    worker->active = RT_TRUE;
    return RT_EOK;
}

static void aic8800_usb_stop_tx(struct aic8800_tx_worker *worker)
{
    rt_base_t level;
    rt_bool_t active;
    rt_bool_t pending;

    if (!worker || !worker->slots)
    {
        return;
    }
    level = rt_hw_interrupt_disable();
    active = worker->active;
    worker->active = RT_FALSE;
    pending = worker->pending != 0;
    rt_hw_interrupt_enable(level);
    if (!active && !pending)
    {
        return;
    }
    if (worker->completion_initialized && !pending)
    {
        rt_completion_done(&worker->stopped);
    }
    for (rt_size_t index = 0; index < worker->slot_count; index++)
    {
        if (worker->slots[index].in_use)
        {
            usbh_kill_urb(&worker->slots[index].urb);
        }
    }
    if (worker->completion_initialized && pending && rt_completion_wait(
                       &worker->stopped,
                       rt_tick_from_millisecond(AIC8800_USB_STOP_TIMEOUT_MS)) !=
                       RT_EOK)
    {
        LOG_E("TX ep=0x%02x did not drain after USB cancellation",
              worker->endpoint ? worker->endpoint->bEndpointAddress : 0);
        level = rt_hw_interrupt_disable();
        for (rt_size_t index = 0; index < worker->slot_count; index++)
        {
            worker->slots[index].length = 0;
            worker->slots[index].submitted = RT_FALSE;
            worker->slots[index].cancelling = RT_FALSE;
            worker->slots[index].in_use = RT_FALSE;
        }
        worker->pending = 0;
        rt_hw_interrupt_enable(level);
        if (worker->context)
        {
            for (rt_size_t index = 0; index < worker->slot_count; index++)
            {
                for (rt_size_t item = 0;
                     item < worker->slots[index].metadata_count; item++)
                {
                    aic8800_core_tx_complete(
                        worker->context,
                        &worker->slots[index].metadata[item]);
                }
                worker->slots[index].metadata_count = 0;
            }
        }
    }
    if (worker->semaphore_initialized)
    {
        /* bus_stop() and disconnect() can both reach this point; read the
         * semaphore once in case the other path has already retired it. */
        rt_sem_t available = worker->available;

        if (available)
        {
            rt_sem_control(available, RT_IPC_CMD_RESET,
                           (void *)worker->slot_count);
        }
    }
#ifdef AIC8800_WIFI_DEBUG_STATS
    LOG_I("TX ep=0x%02x stopped: completions=%u bytes=%llu errors=%u waits=%u timeouts=%u watchdog=%u reclaims=%u max_burst=%u pending=%u",
          worker->endpoint ? worker->endpoint->bEndpointAddress : 0,
          (unsigned int)worker->completion_count,
          (unsigned long long)worker->byte_count,
          (unsigned int)worker->error_count,
          (unsigned int)worker->wait_count,
          (unsigned int)worker->timeout_count,
          (unsigned int)worker->watchdog_count,
          (unsigned int)worker->reclaim_count,
          (unsigned int)worker->max_burst_count,
          (unsigned int)worker->pending);
#endif
    worker->pending = 0;
}

static rt_err_t aic8800_usb_bus_start(struct rt_wlan_offload_bus *bus)
{
    struct aic8800_context *context = aic8800_context_from_bus(bus);
    rt_err_t result;

    if (!context || !context->transport_connected)
    {
        return -RT_EIO;
    }
    context->invalid_rx_log_count = 0;
    /* Anything the previous session had in flight is gone with it. */
    aic8800_core_tx_pending_reset(context);
#if AIC8800_WIFI_TX_TRACE_FRAMES
    context->tx_trace_dumped = RT_FALSE;
#endif
    result = aic8800_usb_start_tx(&context->tx_worker, context);
    if (result != RT_EOK)
    {
        return result;
    }
    result = aic8800_usb_start_tx_queue(context);
    if (result != RT_EOK)
    {
        aic8800_usb_stop_tx(&context->tx_worker);
        return result;
    }
    result = aic8800_usb_start_worker(&context->data_worker, context,
                                      context->data_in, "aic-rx");
    if (result != RT_EOK)
    {
        aic8800_usb_stop_tx_queue(context);
        aic8800_usb_stop_tx(&context->tx_worker);
        return result;
    }
    result = aic8800_usb_start_worker(&context->message_worker, context,
                                      context->message_in, "aic-msg");
    if (result != RT_EOK)
    {
        aic8800_usb_stop_worker(&context->data_worker);
        aic8800_usb_stop_tx_queue(context);
        aic8800_usb_stop_tx(&context->tx_worker);
        return result;
    }
    LOG_I("USB bus ready: %s RX ep=0x%02x/%u mps=%u TX ep=0x%02x/%u "
          "mps=%u buffer=%u queue=%u aggregate=%u/%u%s",
          aic8800_usb_speed_name(context->hport->speed),
          context->data_in->bEndpointAddress,
          (unsigned int)context->data_worker.slot_count,
          (unsigned int)USB_GET_MAXPACKETSIZE(
              context->data_in->wMaxPacketSize),
          context->data_out->bEndpointAddress,
          (unsigned int)context->tx_worker.slot_count,
          (unsigned int)USB_GET_MAXPACKETSIZE(
              context->data_out->wMaxPacketSize),
          (unsigned int)AIC8800_WIFI_TX_BUFFER_SIZE,
          context->usb_tx_queue_enabled ?
              (unsigned int)AIC8800_WIFI_USB_TX_QUEUE_DEPTH : 0U,
          context->usb_tx_aggregation_enabled ?
              (unsigned int)AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES : 1U,
          context->usb_tx_aggregation_enabled ?
              (unsigned int)AIC8800_WIFI_USB_TX_AGGREGATE_SIZE :
              (unsigned int)AIC8800_WIFI_TX_BUFFER_SIZE,
          context->message_in ? ", message endpoint active" : "");
    if (context->message_in)
    {
        LOG_D("USB message RX ep=0x%02x count=%u",
              context->message_in->bEndpointAddress,
              (unsigned int)context->message_worker.slot_count);
    }
    return RT_EOK;
}

static rt_err_t aic8800_usb_bus_stop(struct rt_wlan_offload_bus *bus)
{
    struct aic8800_context *context = aic8800_context_from_bus(bus);

    if (!context)
    {
        return -RT_EINVAL;
    }
    aic8800_usb_stop_tx_queue(context);
    aic8800_usb_stop_tx(&context->tx_worker);
    aic8800_usb_stop_worker(&context->message_worker);
    aic8800_usb_stop_worker(&context->data_worker);
    return RT_EOK;
}

static rt_err_t aic8800_usb_transmit_endpoint(
    struct aic8800_context *context,
    struct usb_endpoint_descriptor *endpoint,
    const void *data, rt_size_t length)
{
    rt_err_t lock_result;
    rt_err_t transfer_result;
    int result;

    if (!context || !endpoint || !data || !length)
    {
        return -RT_EINVAL;
    }
    if (!context->transport_connected || !context->tx_mutex_initialized)
    {
        return -RT_EIO;
    }
    lock_result = rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER);
    if (lock_result != RT_EOK)
    {
        return lock_result;
    }
    if (!context->transport_connected || !context->hport ||
        !context->hport->connected)
    {
        rt_mutex_release(context->tx_mutex);
        return -RT_EIO;
    }
    if (endpoint != context->data_out && endpoint != context->message_out)
    {
        /* Endpoint descriptors are replaced during boot/runtime transition.
         * Do not submit a stale descriptor captured before tx_mutex was held. */
        rt_mutex_release(context->tx_mutex);
        return -RT_EINVAL;
    }
    usbh_bulk_urb_fill(&context->tx_urb, context->hport, endpoint,
                       (rt_uint8_t *)data, length,
                       AIC8800_WIFI_COMMAND_TIMEOUT_MS, RT_NULL, RT_NULL);
    context->tx_urb.transfer_flags = URB_ZERO_PACKET;
    result = usbh_submit_urb(&context->tx_urb);
    if (result)
    {
        LOG_E("bulk OUT ep 0x%02x failed: %d",
              endpoint->bEndpointAddress, result);
    }
    transfer_result = aic8800_usb_result(result);
    if (transfer_result == RT_EOK &&
        context->tx_urb.actual_length != length)
    {
        LOG_E("short bulk OUT ep 0x%02x: %u/%u",
              endpoint->bEndpointAddress,
              (unsigned int)context->tx_urb.actual_length,
              (unsigned int)length);
        transfer_result = -RT_EIO;
    }
    rt_mutex_release(context->tx_mutex);
    return transfer_result;
}

static struct aic8800_tx_slot *aic8800_usb_acquire_tx_slot(
    struct aic8800_tx_worker *worker, rt_err_t *error)
{
    struct aic8800_tx_slot *slot = RT_NULL;
    rt_sem_t available;
    rt_size_t start;
    rt_err_t result;
    rt_base_t level;

    if (!worker || !error || !worker->active ||
        !worker->semaphore_initialized)
    {
        if (error)
        {
            *error = -RT_EIO;
        }
        return RT_NULL;
    }
    /* Capture the semaphore once.  Teardown clears worker->available while a
     * producer may already have passed the checks above; the object itself is
     * a class-driver global that outlives every enumeration, so a captured
     * pointer stays valid. */
    available = worker->available;
    if (!available)
    {
        *error = -RT_EIO;
        return RT_NULL;
    }
    result = rt_sem_take(available, RT_WAITING_NO);
    if (result == RT_EOK)
    {
#ifdef AIC8800_WIFI_DEBUG_STATS
        AIC8800_STAT(worker->burst_count++);
        if (worker->burst_count > worker->max_burst_count)
        {
            worker->max_burst_count = worker->burst_count;
        }
#endif
    }
    else
    {
        AIC8800_STAT(worker->burst_count = 0);
        AIC8800_STAT(worker->wait_count++);
        result = rt_sem_take(
            available, rt_tick_from_millisecond(AIC8800_WIFI_TX_WAIT_MS));
    }
    if (result != RT_EOK)
    {
        worker->timeout_count++;
        *error = result == -RT_ETIMEOUT ? -RT_EFULL : result;
        return RT_NULL;
    }

    level = rt_hw_interrupt_disable();
    if (worker->active)
    {
        start = worker->next_slot;
        for (rt_size_t offset = 0; offset < worker->slot_count; offset++)
        {
            rt_size_t index = start + offset;

            if (index >= worker->slot_count)
            {
                index -= worker->slot_count;
            }
            if (!worker->slots[index].in_use &&
                !worker->slots[index].cancelling)
            {
                slot = &worker->slots[index];
                slot->in_use = RT_TRUE;
                slot->submitted = RT_FALSE;
                worker->pending++;
                worker->next_slot = index + 1U;
                if (worker->next_slot == worker->slot_count)
                {
                    worker->next_slot = 0;
                }
                break;
            }
        }
    }
    rt_hw_interrupt_enable(level);
    if (!slot)
    {
        rt_sem_release(available);
        *error = worker->active ? -RT_EBUSY : -RT_EIO;
        return RT_NULL;
    }
    *error = RT_EOK;
    return slot;
}

static void aic8800_usb_release_unsubmitted_tx_slot(
    struct aic8800_tx_slot *slot)
{
    struct aic8800_tx_worker *worker;
    rt_base_t level;

    if (!slot || !slot->worker)
    {
        return;
    }
    worker = slot->worker;
    level = rt_hw_interrupt_disable();
    if (!slot->in_use)
    {
        rt_hw_interrupt_enable(level);
        return;
    }
    if (worker->pending)
    {
        worker->pending--;
    }
    slot->length = 0;
    slot->submitted = RT_FALSE;
    slot->in_use = RT_FALSE;
    slot->metadata_count = 0;
    rt_hw_interrupt_enable(level);
    if (worker->semaphore_initialized)
    {
        rt_sem_t available = worker->available;

        if (available)
        {
            rt_sem_release(available);
        }
    }
    if (!worker->active && !worker->pending)
    {
        rt_completion_done(&worker->stopped);
    }
}

static rt_err_t aic8800_usb_transmit_data(
    struct aic8800_context *context, const void *data, rt_size_t length,
    const struct aic8800_tx_metadata *metadata, rt_size_t metadata_count,
    rt_bool_t *metadata_consumed)
{
    struct aic8800_tx_worker *worker;
    struct aic8800_tx_slot *slot;
    rt_err_t result;
    rt_err_t lock_result;
    int submit_result;

    if (metadata_consumed)
    {
        *metadata_consumed = RT_FALSE;
    }
    if (!context || !data || !length ||
        metadata_count > AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES ||
        (metadata_count && !metadata) ||
        length > AIC8800_WIFI_USB_TX_TRANSFER_SIZE)
    {
        return -RT_EINVAL;
    }
    worker = &context->tx_worker;
    if (!context->transport_connected || !worker->active)
    {
        return -RT_EIO;
    }
    slot = aic8800_usb_acquire_tx_slot(worker, &result);
    if (!slot)
    {
        if (result == -RT_EFULL &&
            aic8800_log_throttle(worker->timeout_count))
        {
#ifdef AIC8800_WIFI_DEBUG_STATS
            LOG_W("bulk OUT queue full: waits=%u timeouts=%u pending=%u",
                  (unsigned int)worker->wait_count,
                  (unsigned int)worker->timeout_count,
                  (unsigned int)worker->pending);
#else
            LOG_W("bulk OUT queue full: timeouts=%u pending=%u",
                  (unsigned int)worker->timeout_count,
                  (unsigned int)worker->pending);
#endif
        }
        return result;
    }

    rt_memcpy(slot->buffer, data, length);
    slot->length = length;
    slot->metadata_count = 0;
    if (!context->tx_mutex_initialized)
    {
        aic8800_usb_tx_complete(slot, -USB_ERR_SHUTDOWN);
        return -RT_EIO;
    }
    lock_result = rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER);
    if (lock_result != RT_EOK)
    {
        aic8800_usb_tx_complete(slot, -USB_ERR_SHUTDOWN);
        return lock_result;
    }
    if (!context->transport_connected || !worker->active || !context->hport ||
        !context->hport->connected || !worker->endpoint ||
        worker->endpoint != context->data_out)
    {
        rt_mutex_release(context->tx_mutex);
        aic8800_usb_tx_complete(slot, -USB_ERR_SHUTDOWN);
        return -RT_EIO;
    }
    for (rt_size_t index = 0; index < metadata_count; index++)
    {
        if (aic8800_core_tx_metadata_state(context, &metadata[index]) !=
            AIC8800_TX_RECORD_READY)
        {
            rt_mutex_release(context->tx_mutex);
            aic8800_usb_release_unsubmitted_tx_slot(slot);
            return -RT_EBUSY;
        }
    }
    if (metadata_count)
    {
        rt_memcpy(slot->metadata, metadata,
                  metadata_count * sizeof(slot->metadata[0]));
        slot->metadata_count = (rt_uint8_t)metadata_count;
    }
    usbh_bulk_urb_fill(&slot->urb, context->hport, worker->endpoint,
                       slot->buffer, length, 0,
                       aic8800_usb_tx_complete, slot);
    slot->urb.transfer_flags = URB_ZERO_PACKET;
    /* Arm the watchdog before handing the request to the controller: once
     * usbh_submit_urb() returns, the completion may already have run. */
    slot->submit_tick = rt_tick_get();
    slot->submitted = RT_TRUE;
    submit_result = usbh_submit_urb(&slot->urb);
    rt_mutex_release(context->tx_mutex);
    if (submit_result)
    {
        rt_bool_t caller_owns;
        rt_base_t level = rt_hw_interrupt_disable();

        caller_owns = slot->in_use;
        if (caller_owns)
        {
            slot->metadata_count = 0;
        }
        rt_hw_interrupt_enable(level);
        if (caller_owns)
        {
            aic8800_usb_tx_complete(slot, submit_result);
        }
        else if (metadata_consumed)
        {
            *metadata_consumed = RT_TRUE;
        }
        return aic8800_usb_result(submit_result);
    }
    if (metadata_consumed)
    {
        *metadata_consumed = RT_TRUE;
    }
    return RT_EOK;
}

static rt_bool_t aic8800_usb_supports_tx_aggregation(
    const struct aic8800_context *context)
{
    if (!context)
    {
        return RT_FALSE;
    }
    switch (context->product_id)
    {
    case AIC8800_USB_PID_AIC8800DC:
    case AIC8800_USB_PID_AIC8800DW:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static void aic8800_usb_reset_tx_queue(struct aic8800_context *context)
{
    struct aic8800_usb_tx_record *record;

    if (!context || !context->usb_tx_queue)
    {
        return;
    }
    while (rt_mq_recv(context->usb_tx_queue, &record, sizeof(record), 0) ==
           RT_EOK)
    {
        if (record)
        {
            aic8800_core_tx_complete(context, &record->metadata);
            rt_mp_free(record);
        }
    }
    rt_mq_control(context->usb_tx_queue, RT_IPC_CMD_RESET, RT_NULL);
}

static rt_bool_t aic8800_usb_requeue_tx_record(
    struct aic8800_context *context, struct aic8800_usb_tx_record *record)
{
    return context && record && !context->usb_tx_terminate &&
           context->usb_tx_queue &&
           rt_mq_send(context->usb_tx_queue, &record, sizeof(record)) ==
               RT_EOK;
}

static rt_size_t aic8800_usb_append_tx_aggregate(
    rt_uint8_t *destination, rt_size_t capacity,
    const struct aic8800_usb_tx_record *record)
{
    rt_size_t wire_length;

    if (!destination || !record ||
        record->length < AIC8800_USB_HEADER_SIZE)
    {
        return 0;
    }
    wire_length = aic8800_usb_align4(
        AIC8800_USB_TX_AGGREGATE_PREFIX + record->length);
    if (wire_length > capacity)
    {
        return 0;
    }
    rt_memset(destination, 0, wire_length);
    /* The vendor aggregate format prefixes a normal four-byte AIC record
     * with two copies of that record's 12-bit length. */
    destination[0] = (rt_uint8_t)record->length;
    destination[1] = (rt_uint8_t)(record->length >> 8) & 0x0fU;
    destination[2] = destination[0];
    destination[3] = destination[1];
    rt_memcpy(destination + AIC8800_USB_TX_AGGREGATE_PREFIX,
              record->data, record->length);
    return wire_length;
}

static rt_err_t aic8800_usb_submit_tx_transfer(
    struct aic8800_context *context, const void *data, rt_size_t length,
    const struct aic8800_tx_metadata *metadata, rt_size_t metadata_count,
    rt_bool_t *metadata_consumed)
{
    return aic8800_usb_transmit_data(
        context, data, length, metadata, metadata_count, metadata_consumed);
}

static void aic8800_usb_tx_queue_worker(void *parameter)
{
    struct aic8800_context *context = parameter;
    struct aic8800_usb_tx_record *carry = RT_NULL;

    while (!context->usb_tx_terminate)
    {
        struct aic8800_usb_tx_record *records[
            AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES];
        struct aic8800_tx_metadata metadata[
            AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES];
        rt_size_t wire_lengths[AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES];
        rt_size_t aggregate_length = 0;
        rt_size_t count = 0;
        rt_size_t scanned = 0;
        rt_size_t max_records = context->usb_tx_aggregation_enabled ?
            AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES : 1U;
        rt_bool_t metadata_consumed = RT_FALSE;
        rt_err_t result;

        if (carry)
        {
            records[count] = carry;
            carry = RT_NULL;
        }
        else
        {
            result = rt_mq_recv(context->usb_tx_queue, &records[count],
                                sizeof(records[count]), RT_WAITING_FOREVER);
            if (result != RT_EOK)
            {
                continue;
            }
        }
        if (context->usb_tx_terminate)
        {
            if (records[count])
            {
                aic8800_core_tx_complete(
                    context, &records[count]->metadata);
                rt_mp_free(records[count]);
            }
            break;
        }
        if (!records[count])
        {
            continue;
        }
        {
            enum aic8800_tx_record_state state =
                aic8800_core_tx_metadata_state(
                    context, &records[count]->metadata);

            if (state == AIC8800_TX_RECORD_DEFER &&
                aic8800_usb_requeue_tx_record(context, records[count]))
            {
                rt_thread_mdelay(1);
                continue;
            }
            if (state != AIC8800_TX_RECORD_READY)
            {
                aic8800_core_tx_complete(
                    context, &records[count]->metadata);
                rt_mp_free(records[count]);
                continue;
            }
        }
        wire_lengths[count] = aic8800_usb_align4(
            AIC8800_USB_TX_AGGREGATE_PREFIX + records[count]->length);
        aggregate_length = wire_lengths[count];
        count++;

        while (count < max_records &&
               scanned++ < AIC8800_WIFI_USB_TX_QUEUE_DEPTH)
        {
            struct aic8800_usb_tx_record *next = RT_NULL;
            rt_int32_t timeout = count == 1U ?
                (rt_int32_t)rt_tick_from_millisecond(
                    AIC8800_WIFI_USB_TX_AGGREGATE_WAIT_MS) : 0;
            rt_size_t wire_length;

            if (rt_mq_recv(context->usb_tx_queue, &next, sizeof(next),
                           timeout) != RT_EOK)
            {
                break;
            }
            if (!next)
            {
                if (context->usb_tx_terminate)
                {
                    break;
                }
                continue;
            }
            {
                enum aic8800_tx_record_state state =
                    aic8800_core_tx_metadata_state(
                        context, &next->metadata);

                if (state == AIC8800_TX_RECORD_DEFER &&
                    aic8800_usb_requeue_tx_record(context, next))
                {
                    continue;
                }
                if (state != AIC8800_TX_RECORD_READY)
                {
                    aic8800_core_tx_complete(context, &next->metadata);
                    rt_mp_free(next);
                    continue;
                }
            }
            wire_length = aic8800_usb_align4(
                AIC8800_USB_TX_AGGREGATE_PREFIX + next->length);
            if (aggregate_length + wire_length >
                AIC8800_WIFI_USB_TX_AGGREGATE_SIZE)
            {
                carry = next;
                break;
            }
            records[count] = next;
            wire_lengths[count] = wire_length;
            aggregate_length += wire_length;
            count++;
        }

        if (context->usb_tx_terminate)
        {
            for (rt_size_t index = 0; index < count; index++)
            {
                aic8800_core_tx_complete(
                    context, &records[index]->metadata);
                rt_mp_free(records[index]);
            }
            break;
        }
        {
            rt_size_t valid_count = 0;

            for (rt_size_t index = 0; index < count; index++)
            {
                enum aic8800_tx_record_state state =
                    aic8800_core_tx_metadata_state(
                        context, &records[index]->metadata);

                if (state == AIC8800_TX_RECORD_READY)
                {
                    records[valid_count++] = records[index];
                }
                else if (state == AIC8800_TX_RECORD_DEFER &&
                         aic8800_usb_requeue_tx_record(
                             context, records[index]))
                {
                    /* Ownership remains with the queue. */
                }
                else
                {
                    aic8800_core_tx_complete(
                        context, &records[index]->metadata);
                    rt_mp_free(records[index]);
                }
            }
            count = valid_count;
        }
        if (!count)
        {
            continue;
        }
        aggregate_length = 0;
        for (rt_size_t index = 0; index < count; index++)
        {
            wire_lengths[index] = aic8800_usb_align4(
                AIC8800_USB_TX_AGGREGATE_PREFIX + records[index]->length);
            aggregate_length += wire_lengths[index];
            metadata[index] = records[index]->metadata;
        }
        if (!context->usb_tx_aggregation_enabled)
        {
            RT_ASSERT(count == 1U);
            result = aic8800_usb_submit_tx_transfer(
                context, records[0]->data, records[0]->length,
                metadata, count, &metadata_consumed);
        }
        else
        {
            rt_size_t offset = 0;

            /* CONFIG_USB_TX_AGGR in the vendor driver wraps every transfer,
             * including a one-record transfer, in the eight-byte aggregate
             * header. D80/D80X2 firmware uses ordinary records instead. */
            for (rt_size_t index = 0; index < count; index++)
            {
                rt_size_t prepared = aic8800_usb_append_tx_aggregate(
                    context->usb_tx_aggregate_buffer + offset,
                    AIC8800_WIFI_USB_TX_AGGREGATE_SIZE - offset,
                    records[index]);

                RT_ASSERT(prepared == wire_lengths[index]);
                offset += prepared;
            }
            result = aic8800_usb_submit_tx_transfer(
                context, context->usb_tx_aggregate_buffer, offset,
                metadata, count, &metadata_consumed);
        }
        if (result == RT_EOK)
        {
            AIC8800_STAT(context->usb_tx_frame_count += count);
            if (context->usb_tx_aggregation_enabled)
            {
                AIC8800_STAT(context->usb_tx_aggregate_count++);
            }
#ifdef AIC8800_WIFI_DEBUG_STATS
            if (count > context->usb_tx_max_aggregate)
            {
                context->usb_tx_max_aggregate = (rt_uint16_t)count;
            }
#endif
        }
        else if (!context->usb_tx_terminate && result != -RT_EBUSY)
        {
            context->usb_tx_error_count++;
            if (aic8800_log_throttle(context->usb_tx_error_count))
            {
                LOG_W("USB transmit worker failed: %d (errors=%u)",
                      result, (unsigned int)context->usb_tx_error_count);
            }
            /* This thread outranks the system workqueue, which is where URB
             * cancellation and endpoint recovery run, and usbh_kill_urb()
             * holds urb->reject across a block.  Draining the queue at full
             * speed into a failing endpoint therefore keeps the watchdog and
             * recovery work off the CPU forever, and every retry lands on a
             * request still marked for cancellation.  Sleep so the recovery
             * that would clear the fault can actually make progress. */
            rt_thread_mdelay(AIC8800_WIFI_USB_TX_ERROR_BACKOFF_MS);
        }
        else if (!context->usb_tx_terminate)
        {
            rt_thread_mdelay(1);
        }
        if (result != RT_EOK && !metadata_consumed)
        {
            for (rt_size_t index = 0; index < count; index++)
            {
                if (aic8800_usb_requeue_tx_record(
                        context, records[index]))
                {
                    records[index] = RT_NULL;
                }
                else
                {
                    aic8800_core_tx_complete(
                        context, &records[index]->metadata);
                }
            }
        }
        for (rt_size_t index = 0; index < count; index++)
        {
            if (records[index])
            {
                rt_mp_free(records[index]);
            }
        }
    }
    if (carry)
    {
        aic8800_core_tx_complete(context, &carry->metadata);
        rt_mp_free(carry);
    }
    rt_completion_done(&context->usb_tx_thread_stopped);
}

static rt_err_t aic8800_usb_start_tx_queue(
    struct aic8800_context *context)
{
    rt_err_t result;

    if (!context)
    {
        return -RT_EINVAL;
    }
    AIC8800_STAT(context->usb_tx_frame_count = 0);
    AIC8800_STAT(context->usb_tx_aggregate_count = 0);
    context->usb_tx_queue_drop_count = 0;
    context->usb_tx_error_count = 0;
    AIC8800_STAT(context->usb_tx_max_aggregate = 0);
    AIC8800_STAT(context->usb_tx_queue_high_water = 0);
    context->usb_tx_queue_enabled = RT_FALSE;
    context->usb_tx_aggregation_enabled =
        aic8800_usb_supports_tx_aggregation(context) &&
        AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES > 1U;
    if (!context->usb_tx_queue || !context->usb_tx_pool ||
        (context->usb_tx_aggregation_enabled &&
         !context->usb_tx_aggregate_buffer))
    {
        context->usb_tx_aggregation_enabled = RT_FALSE;
        return -RT_EIO;
    }
    aic8800_usb_reset_tx_queue(context);
    context->usb_tx_terminate = RT_FALSE;
    context->usb_tx_queue_enabled = RT_TRUE;
    rt_completion_init(&context->usb_tx_thread_stopped);
    context->usb_tx_thread = rt_thread_create(
        "aic-utx", aic8800_usb_tx_queue_worker, context,
        AIC8800_WIFI_USB_TX_THREAD_STACK_SIZE,
        AIC8800_WIFI_USB_TX_THREAD_PRIORITY, 10U);
    if (!context->usb_tx_thread)
    {
        context->usb_tx_queue_enabled = RT_FALSE;
        context->usb_tx_aggregation_enabled = RT_FALSE;
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(context->usb_tx_thread);
    if (result != RT_EOK)
    {
        rt_thread_delete(context->usb_tx_thread);
        context->usb_tx_thread = RT_NULL;
        context->usb_tx_queue_enabled = RT_FALSE;
        context->usb_tx_aggregation_enabled = RT_FALSE;
        return result;
    }
    context->usb_tx_thread_started = RT_TRUE;
    return RT_EOK;
}

static void aic8800_usb_stop_tx_queue(struct aic8800_context *context)
{
    struct aic8800_usb_tx_record *stop = RT_NULL;

    if (!context)
    {
        return;
    }
    context->usb_tx_queue_enabled = RT_FALSE;
    context->usb_tx_terminate = RT_TRUE;
    if (context->usb_tx_queue)
    {
        (void)rt_mq_urgent(context->usb_tx_queue, &stop, sizeof(stop));
    }
    if (context->usb_tx_thread)
    {
        if (context->usb_tx_thread_started)
        {
            rt_completion_wait(&context->usb_tx_thread_stopped,
                               RT_WAITING_FOREVER);
        }
        else
        {
            rt_thread_delete(context->usb_tx_thread);
        }
    }
    context->usb_tx_thread = RT_NULL;
    context->usb_tx_thread_started = RT_FALSE;
    context->usb_tx_aggregation_enabled = RT_FALSE;
    aic8800_usb_reset_tx_queue(context);
}

static rt_err_t aic8800_usb_queue_transmit(
    struct aic8800_context *context, const void *data, rt_size_t length,
    rt_bool_t urgent)
{
    struct aic8800_usb_tx_record *record;
    rt_err_t result;

    if (!context || !data || length < AIC8800_USB_HEADER_SIZE ||
        length > AIC8800_WIFI_TX_BUFFER_SIZE ||
        !context->usb_tx_queue_enabled || !context->usb_tx_pool ||
        !context->usb_tx_queue)
    {
        return -RT_EINVAL;
    }
    record = rt_mp_alloc(
        context->usb_tx_pool,
        rt_tick_from_millisecond(AIC8800_WIFI_TX_WAIT_MS));
    if (!record)
    {
        result = -RT_EFULL;
        goto failed;
    }
    record->length = (rt_uint16_t)length;
    aic8800_core_tx_metadata_init(
        context, data, length, &record->metadata);
    rt_memcpy(record->data, data, length);
    result = urgent ?
             rt_mq_urgent(context->usb_tx_queue, &record, sizeof(record)) :
             rt_mq_send(context->usb_tx_queue, &record, sizeof(record));
    if (result != RT_EOK)
    {
        rt_mp_free(record);
        goto failed;
    }
#ifdef AIC8800_WIFI_DEBUG_STATS
    if (context->usb_tx_queue->entry > context->usb_tx_queue_high_water)
    {
        context->usb_tx_queue_high_water = context->usb_tx_queue->entry;
    }
#endif
    return RT_EOK;

failed:
    context->usb_tx_queue_drop_count++;
    if (aic8800_log_throttle(context->usb_tx_queue_drop_count))
    {
#ifdef AIC8800_WIFI_DEBUG_STATS
        LOG_W("USB transmit queue full: result=%d drops=%u depth=%u high=%u",
              result, (unsigned int)context->usb_tx_queue_drop_count,
              context->usb_tx_queue ?
                  (unsigned int)context->usb_tx_queue->entry : 0U,
              (unsigned int)context->usb_tx_queue_high_water);
#else
        LOG_W("USB transmit queue full: result=%d drops=%u depth=%u",
              result, (unsigned int)context->usb_tx_queue_drop_count,
              context->usb_tx_queue ?
                  (unsigned int)context->usb_tx_queue->entry : 0U);
#endif
    }
    return result;
}

static rt_err_t aic8800_usb_bus_transmit(struct rt_wlan_offload_bus *bus,
                                         const void *data, rt_size_t length)
{
    struct aic8800_context *context = aic8800_context_from_bus(bus);
    struct aic8800_tx_metadata metadata;
    rt_bool_t metadata_consumed;
    rt_err_t result;

    if (!context || length > bus->max_tx_size)
    {
        return -RT_EINVAL;
    }
    if (context->usb_tx_queue_enabled)
    {
        return aic8800_usb_queue_transmit(context, data, length, RT_FALSE);
    }
    aic8800_core_tx_metadata_init(context, data, length, &metadata);
    result = aic8800_usb_transmit_data(
        context, data, length, &metadata, 1U, &metadata_consumed);
    return result != RT_EOK && metadata_consumed ? RT_EOK : result;
}

static rt_err_t aic8800_usb_bus_transmit_priority(
    struct rt_wlan_offload_bus *bus, enum rt_wlan_offload_bus_priority priority,
    const void *data, rt_size_t length)
{
    struct aic8800_context *context = aic8800_context_from_bus(bus);
    struct aic8800_tx_metadata metadata;
    rt_bool_t metadata_consumed;
    rt_err_t result;

    if (!context || length > bus->max_tx_size)
    {
        return -RT_EINVAL;
    }
    if (priority == RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL)
    {
        struct usb_endpoint_descriptor *endpoint =
            context->message_out ? context->message_out : context->data_out;

        return aic8800_usb_transmit_endpoint(context, endpoint, data, length);
    }
    /* Keep every data/management record in the host queue.  High-priority
     * records go to its head, while DC/DW aggregation is applied by the same
     * worker before submission. */
    if (context->usb_tx_queue_enabled)
    {
        return aic8800_usb_queue_transmit(
            context, data, length,
            priority == RT_WLAN_OFFLOAD_BUS_PRIORITY_HIGH);
    }
    aic8800_core_tx_metadata_init(context, data, length, &metadata);
    result = aic8800_usb_transmit_data(
        context, data, length, &metadata, 1U, &metadata_consumed);
    return result != RT_EOK && metadata_consumed ? RT_EOK : result;
}

static const struct rt_wlan_offload_bus_ops g_aic8800_usb_bus_ops = {
    .start = aic8800_usb_bus_start,
    .stop = aic8800_usb_bus_stop,
    .transmit = aic8800_usb_bus_transmit,
    .transmit_priority = aic8800_usb_bus_transmit_priority,
};

static rt_err_t aic8800_usb_allocate_worker_buffer(
    struct aic8800_rx_worker *worker, rt_size_t slot_count)
{
    rt_size_t index;

    worker->slots = rt_calloc(slot_count, sizeof(*worker->slots));
    if (!worker->slots)
    {
        return -RT_ENOMEM;
    }
    worker->slot_count = slot_count;
    /* Several spare DMA buffers per URB so giveback can keep the pipe
     * armed across a second high-speed refill before the worker returns
     * the first wave.  These are not extra URBs. */
    worker->spare_count = slot_count * AIC8800_USB_RX_SPARES_PER_SLOT;
    worker->buffer_count = slot_count + worker->spare_count;
    worker->buffers = rt_calloc(worker->buffer_count, sizeof(*worker->buffers));
    worker->spare = rt_calloc(worker->spare_count, sizeof(*worker->spare));
    if (!worker->buffers || !worker->spare)
    {
        aic8800_usb_free_worker_buffer(worker);
        return -RT_ENOMEM;
    }
    for (index = 0; index < worker->buffer_count; index++)
    {
        worker->buffers[index] = rt_malloc_align(
            AIC8800_WIFI_RX_BUFFER_SIZE, AIC8800_USB_DMA_ALIGNMENT);
        if (!worker->buffers[index])
        {
            aic8800_usb_free_worker_buffer(worker);
            return -RT_ENOMEM;
        }
    }
    for (index = 0; index < slot_count; index++)
    {
        worker->slots[index].worker = worker;
        worker->slots[index].buffer = worker->buffers[index];
    }
    for (index = 0; index < worker->spare_count; index++)
    {
        worker->spare[index] = worker->buffers[slot_count + index];
    }
    worker->spare_available = worker->spare_count;
    worker->assembly_capacity = AIC8800_WIFI_RX_BUFFER_SIZE +
                                AIC8800_USB_MAX_RECORD_SIZE;
    worker->assembly = rt_malloc(worker->assembly_capacity);
    if (!worker->assembly)
    {
        aic8800_usb_free_worker_buffer(worker);
        return -RT_ENOMEM;
    }
    return RT_EOK;
}

static void aic8800_usb_free_worker_buffer(struct aic8800_rx_worker *worker)
{
    rt_size_t index;

    rt_free(worker->assembly);
    worker->assembly = RT_NULL;
    worker->assembly_length = 0;
    worker->assembly_capacity = 0;
    if (worker->buffers)
    {
        for (index = 0; index < worker->buffer_count; index++)
        {
            if (worker->buffers[index])
            {
                rt_free_align(worker->buffers[index]);
                worker->buffers[index] = RT_NULL;
            }
        }
        rt_free(worker->buffers);
        worker->buffers = RT_NULL;
    }
    worker->buffer_count = 0;
    rt_free(worker->spare);
    worker->spare = RT_NULL;
    worker->spare_count = 0;
    worker->spare_available = 0;
    if (worker->slots)
    {
        for (index = 0; index < worker->slot_count; index++)
        {
            worker->slots[index].buffer = RT_NULL;
        }
        rt_free(worker->slots);
        worker->slots = RT_NULL;
    }
    worker->slot_count = 0;
    worker->padding_length = 0;
}

static rt_err_t aic8800_usb_allocate_tx_buffers(
    struct aic8800_tx_worker *worker, rt_size_t slot_count,
    rt_size_t transfer_size)
{
    worker->slots = rt_calloc(slot_count, sizeof(*worker->slots));
    if (!worker->slots)
    {
        return -RT_ENOMEM;
    }
    worker->slot_count = slot_count;
    if (!g_aic8800_transport_ipc_initialized)
    {
        aic8800_usb_free_tx_buffers(worker);
        return -RT_EIO;
    }
    worker->available = &g_aic8800_tx_available;
    rt_sem_control(worker->available, RT_IPC_CMD_RESET, (void *)slot_count);
    worker->semaphore_initialized = RT_TRUE;
    for (rt_size_t index = 0; index < slot_count; index++)
    {
        worker->slots[index].worker = worker;
        worker->slots[index].buffer = rt_malloc_align(
            transfer_size, AIC8800_USB_DMA_ALIGNMENT);
        if (!worker->slots[index].buffer)
        {
            aic8800_usb_free_tx_buffers(worker);
            return -RT_ENOMEM;
        }
    }
    return RT_EOK;
}

static void aic8800_usb_free_tx_buffers(struct aic8800_tx_worker *worker)
{
    if (!worker)
    {
        return;
    }
    worker->active = RT_FALSE;
    if (worker->slots)
    {
        for (rt_size_t index = 0; index < worker->slot_count; index++)
        {
            if (worker->context)
            {
                for (rt_size_t item = 0;
                     item < worker->slots[index].metadata_count; item++)
                {
                    aic8800_core_tx_complete(
                        worker->context,
                        &worker->slots[index].metadata[item]);
                }
            }
            worker->slots[index].metadata_count = 0;
            if (worker->slots[index].buffer)
            {
                rt_free_align(worker->slots[index].buffer);
                worker->slots[index].buffer = RT_NULL;
            }
        }
        rt_free(worker->slots);
        worker->slots = RT_NULL;
    }
    /* Retire the flag before the pointer so a producer or completion which is
     * already past its check cannot observe a NULL semaphore. */
    worker->semaphore_initialized = RT_FALSE;
    worker->available = RT_NULL;
    worker->slot_count = 0;
    worker->pending = 0;
}

static rt_err_t aic8800_usb_allocate_tx_queue(
    struct aic8800_context *context, rt_bool_t allocate_aggregate_buffer)
{
    if (!context)
    {
        return -RT_EINVAL;
    }
    context->usb_tx_pool = rt_mp_create(
        "aic-utxp", AIC8800_WIFI_USB_TX_QUEUE_DEPTH,
        sizeof(struct aic8800_usb_tx_record));
    context->usb_tx_queue = rt_mq_create(
        "aic-utxq", sizeof(struct aic8800_usb_tx_record *),
        AIC8800_WIFI_USB_TX_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    if (allocate_aggregate_buffer)
    {
        context->usb_tx_aggregate_buffer = rt_malloc_align(
            AIC8800_WIFI_USB_TX_AGGREGATE_SIZE,
            AIC8800_USB_DMA_ALIGNMENT);
    }
    if (!context->usb_tx_pool || !context->usb_tx_queue ||
        (allocate_aggregate_buffer && !context->usb_tx_aggregate_buffer))
    {
        aic8800_usb_free_tx_queue(context);
        return -RT_ENOMEM;
    }
    return RT_EOK;
}

static void aic8800_usb_free_tx_queue(struct aic8800_context *context)
{
    if (!context)
    {
        return;
    }
    aic8800_usb_stop_tx_queue(context);
    if (context->usb_tx_queue)
    {
        rt_mq_delete(context->usb_tx_queue);
        context->usb_tx_queue = RT_NULL;
    }
    if (context->usb_tx_pool)
    {
        rt_mp_delete(context->usb_tx_pool);
        context->usb_tx_pool = RT_NULL;
    }
    if (context->usb_tx_aggregate_buffer)
    {
        rt_free_align(context->usb_tx_aggregate_buffer);
        context->usb_tx_aggregate_buffer = RT_NULL;
    }
}

static rt_err_t aic8800_usb_allocate_buffers(struct aic8800_context *context)
{
    rt_bool_t aggregate = aic8800_usb_supports_tx_aggregation(context) &&
                          AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES > 1U;
    rt_size_t transfer_size = aggregate ?
        AIC8800_WIFI_USB_TX_TRANSFER_SIZE : AIC8800_WIFI_TX_BUFFER_SIZE;
    rt_err_t result = aic8800_usb_allocate_tx_buffers(
        &context->tx_worker, AIC8800_WIFI_DATA_TX_URBS, transfer_size);

    if (result != RT_EOK)
    {
        return result;
    }
    result = aic8800_usb_allocate_tx_queue(context, aggregate);
    if (result != RT_EOK)
    {
        aic8800_usb_free_tx_buffers(&context->tx_worker);
        return result;
    }
    result = aic8800_usb_allocate_worker_buffer(
        &context->data_worker, AIC8800_USB_DATA_RX_SLOT_COUNT);

    if (result != RT_EOK)
    {
        aic8800_usb_free_tx_queue(context);
        aic8800_usb_free_tx_buffers(&context->tx_worker);
        return result;
    }
    if (context->message_in)
    {
        result = aic8800_usb_allocate_worker_buffer(
            &context->message_worker, AIC8800_USB_MESSAGE_RX_SLOT_COUNT);
        if (result != RT_EOK)
        {
            aic8800_usb_free_worker_buffer(&context->data_worker);
            aic8800_usb_free_tx_queue(context);
            aic8800_usb_free_tx_buffers(&context->tx_worker);
            return result;
        }
    }
    return RT_EOK;
}

static void aic8800_usb_free_buffers(struct aic8800_context *context)
{
    aic8800_usb_free_worker_buffer(&context->message_worker);
    aic8800_usb_free_worker_buffer(&context->data_worker);
    aic8800_usb_free_tx_queue(context);
    aic8800_usb_free_tx_buffers(&context->tx_worker);
}

static void aic8800_usb_attach_work(struct rt_work *work, void *work_data)
{
    struct aic8800_context *context = work_data;
    struct rt_wlan_offload_bus_config bus_config;
    rt_bool_t attach_runtime = RT_FALSE;
    rt_err_t result = -RT_EIO;

    (void)work;
    if (!context)
    {
        return;
    }
    if (!context->transport_connected || !context->hport ||
        !context->hport->connected)
    {
        goto done;
    }

    result = aic8800_firmware_probe(context, &attach_runtime);
    if (result != RT_EOK)
    {
        LOG_E("firmware probe for %04x:%04x failed: %d",
              context->vendor_id, context->product_id, result);
        goto done;
    }
    if (!context->transport_connected || !context->hport ||
        !context->hport->connected)
    {
        result = -RT_EIO;
        goto done;
    }
    if (!attach_runtime)
    {
        context->firmware_transition = RT_TRUE;
        rt_snprintf(context->hport->config.intf[context->interface_number].devname,
                    CONFIG_USBHOST_DEV_NAMELEN, "aic-loader");
        LOG_I("waiting for AIC firmware re-enumeration from %04x:%04x",
              context->vendor_id, context->product_id);
        result = RT_EOK;
        goto done;
    }

    rt_memset(&bus_config, 0, sizeof(bus_config));
    bus_config.type = RT_WLAN_OFFLOAD_BUS_USB;
    bus_config.ops = &g_aic8800_usb_bus_ops;
    bus_config.capabilities = RT_WLAN_OFFLOAD_BUS_CAP_PACKET |
                              RT_WLAN_OFFLOAD_BUS_CAP_FULL_DUPLEX |
                              RT_WLAN_OFFLOAD_BUS_CAP_HOTPLUG |
                              RT_WLAN_OFFLOAD_BUS_CAP_TX_PRIORITY |
                              RT_WLAN_OFFLOAD_BUS_CAP_DMA;
    bus_config.max_tx_size = AIC8800_WIFI_TX_BUFFER_SIZE;
    bus_config.max_rx_size = AIC8800_WIFI_RX_BUFFER_SIZE;
    bus_config.alignment = 4;
    bus_config.driver_data = context;
    result = rt_wlan_offload_bus_init(&context->bus, &bus_config);
    if (result != RT_EOK)
    {
        LOG_E("WLAN offload bus initialization failed: %d", result);
        goto done;
    }
    context->bus_initialized = RT_TRUE;

    result = aic8800_core_attach(context);
    if (result != RT_EOK)
    {
        rt_err_t cleanup_result;

        LOG_E("WLAN offload radio attachment failed: %d", result);
        cleanup_result = rt_wlan_offload_bus_deinit(&context->bus);
        if (cleanup_result == RT_EOK)
        {
            context->bus_initialized = RT_FALSE;
        }
        else
        {
            LOG_E("WLAN offload bus rollback failed: %d", cleanup_result);
        }
        goto done;
    }

    LOG_I("attached AIC runtime device %04x:%04x, data=%02x/%02x%s",
          context->vendor_id, context->product_id,
          context->data_in->bEndpointAddress,
          context->data_out->bEndpointAddress,
          context->message_in ? ", dedicated message endpoints" : "");

done:
    context->attach_result = result;
    rt_completion_done(&context->attach_done);
}

static int aic8800_usb_connect(struct usbh_hubport *hport, rt_uint8_t intf)
{
    struct aic8800_context *context = &g_aic8800_context;
    struct usbh_interface_altsetting *setting;
    rt_uint32_t resources;
    rt_err_t result;
    rt_uint8_t index;

    if (!hport || context->transport_connected)
    {
        return -USB_ERR_BUSY;
    }
    resources = aic8800_usb_context_resource_mask(context);
    if (resources)
    {
        LOG_E("refusing USB reconnect while the previous context is still live (resources=0x%02x)",
              (unsigned int)resources);
        return -USB_ERR_BUSY;
    }
    /* An AIC device arriving means any preceding mode switch did its job, so
     * the next fake-storage device gets its forced re-enumeration back. */
    g_aic8800_modeswitch_rescanned = RT_FALSE;
    rt_memset(context, 0, sizeof(*context));
    context->transport = AIC8800_TRANSPORT_USB;
    context->hport = hport;
    context->interface_number = intf;
    context->vendor_id = hport->device_desc.idVendor;
    context->product_id = hport->device_desc.idProduct;
    context->vif_index = AIC8800_INVALID_INDEX;
    context->ap_station_index = AIC8800_INVALID_INDEX;
    context->ap_vif_index = AIC8800_INVALID_INDEX;
    context->ap_broadcast_station_index = AIC8800_INVALID_INDEX;

    setting = &hport->config.intf[intf].altsetting[0];
    for (index = 0; index < setting->intf_desc.bNumEndpoints; index++)
    {
        struct usb_endpoint_descriptor *endpoint =
            &setting->ep[index].ep_desc;

        if (USB_GET_ENDPOINT_TYPE(endpoint->bmAttributes) !=
            USB_ENDPOINT_TYPE_BULK)
        {
            continue;
        }
        if (endpoint->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK)
        {
            if (!context->data_in)
            {
                USBH_EP_INIT(context->data_in, endpoint);
            }
            else if (!context->message_in)
            {
                USBH_EP_INIT(context->message_in, endpoint);
            }
        }
        else if (!context->data_out)
        {
            USBH_EP_INIT(context->data_out, endpoint);
        }
        else if (!context->message_out)
        {
            USBH_EP_INIT(context->message_out, endpoint);
        }
    }
    if (!context->data_in || !context->data_out)
    {
        LOG_E("%04x:%04x interface %u has no bulk data endpoint pair",
              context->vendor_id, context->product_id, intf);
        rt_memset(context, 0, sizeof(*context));
        return -USB_ERR_NODEV;
    }

    if (!g_aic8800_transport_ipc_initialized)
    {
        rt_memset(context, 0, sizeof(*context));
        return -USB_ERR_NODEV;
    }
    context->tx_mutex = &g_aic8800_tx_mutex;
    context->tx_mutex_initialized = RT_TRUE;
    context->frame_mutex = &g_aic8800_frame_mutex;
    context->frame_mutex_initialized = RT_TRUE;
    rt_work_init(&context->tx_recovery_work,
                 aic8800_usb_tx_recovery_work, context);
    context->tx_recovery_work_initialized = RT_TRUE;
    rt_work_init(&context->tx_watchdog_work,
                 aic8800_usb_tx_watchdog_work, context);
    context->tx_watchdog_work_initialized = RT_TRUE;
    context->tx_watchdog_work_queued = RT_FALSE;
    rt_timer_init(&context->tx_watchdog_timer, "aic-txw",
                  aic8800_usb_tx_watchdog, context,
                  rt_tick_from_millisecond(
                      AIC8800_USB_TX_WATCHDOG_PERIOD_MS),
                  RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    context->tx_watchdog_timer_initialized = RT_TRUE;
    if (rt_timer_start(&context->tx_watchdog_timer) != RT_EOK)
    {
        LOG_E("transmit watchdog timer failed to start");
        aic8800_usb_cancel_tx_maintenance(context);
        aic8800_usb_clear_tx_locks(context);
        aic8800_usb_reset_context(context, "watchdog start failure");
        return -USB_ERR_NOMEM;
    }

    result = aic8800_usb_allocate_buffers(context);
    if (result != RT_EOK)
    {
        aic8800_usb_cancel_tx_maintenance(context);
        aic8800_usb_clear_tx_locks(context);
        aic8800_usb_reset_context(context, "buffer allocation failure");
        return -USB_ERR_NOMEM;
    }

    context->transport_connected = RT_TRUE;
    hport->config.intf[intf].priv = context;
    rt_snprintf(hport->config.intf[intf].devname,
                CONFIG_USBHOST_DEV_NAMELEN, "aic-wlan");
    rt_completion_init(&context->attach_done);
    context->attach_result = -RT_EIO;
    rt_work_init(&context->attach_work, aic8800_usb_attach_work, context);
    context->attach_work_initialized = RT_TRUE;
    result = rt_workqueue_dowork(g_aic8800_attach_workqueue,
                                 &context->attach_work);
    if (result != RT_EOK)
    {
        LOG_E("failed to queue AIC attach work: %d", result);
        hport->config.intf[intf].priv = RT_NULL;
        context->transport_connected = RT_FALSE;
        context->attach_work_initialized = RT_FALSE;
        aic8800_usb_cancel_tx_maintenance(context);
        aic8800_usb_free_buffers(context);
        aic8800_usb_clear_tx_locks(context);
        aic8800_usb_reset_context(context, "attach queue failure");
        return -USB_ERR_NOMEM;
    }

    /* Keep the class probe serialized with hub enumeration while running the
     * firmware and WLAN offload initialization on a stack sized for the driver. */
    result = rt_completion_wait(&context->attach_done, RT_WAITING_FOREVER);
    /* completion is raised by the callback itself.  Join the work item before
     * this context can be cleared or reused by a fast re-enumeration. */
    rt_workqueue_cancel_work_sync(g_aic8800_attach_workqueue,
                                  &context->attach_work);
    context->attach_work_initialized = RT_FALSE;
    if (result == RT_EOK)
    {
        result = context->attach_result;
    }
    if (result != RT_EOK)
    {
        hport->config.intf[intf].priv = RT_NULL;
        context->transport_connected = RT_FALSE;
        if (context->bus_initialized)
        {
            result = aic8800_core_detach(context);
            if (result == RT_EOK)
            {
                result = rt_wlan_offload_bus_deinit(&context->bus);
                if (result == RT_EOK)
                {
                    context->bus_initialized = RT_FALSE;
                }
            }
        }
        aic8800_firmware_disconnected(context);
        aic8800_usb_cancel_tx_maintenance(context);
        aic8800_usb_free_buffers(context);
        aic8800_usb_clear_tx_locks(context);
        aic8800_usb_reset_context(context, "failed attach");
        return -USB_ERR_INVAL;
    }
    return 0;
}

static int aic8800_usb_disconnect(struct usbh_hubport *hport, rt_uint8_t intf)
{
    struct aic8800_context *context;
    rt_uint16_t vendor_id;
    rt_uint16_t product_id;
    rt_err_t result;

    if (!hport)
    {
        return 0;
    }
    context = hport->config.intf[intf].priv;
    if (!context)
    {
        return 0;
    }

    vendor_id = context->vendor_id;
    product_id = context->product_id;

    context->transport_connected = RT_FALSE;
    if (context->attach_work_initialized)
    {
        rt_workqueue_cancel_work_sync(g_aic8800_attach_workqueue,
                                      &context->attach_work);
        context->attach_work_initialized = RT_FALSE;
    }
    /* Quiesce asynchronous TX before cancelling recovery work.  This closes
     * the race where a late completion queues new work after cancellation. */
    aic8800_usb_stop_tx(&context->tx_worker);
    aic8800_usb_cancel_tx_maintenance(context);
    usbh_kill_urb(&context->tx_urb);
    if (context->tx_mutex_initialized &&
        rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        rt_mutex_release(context->tx_mutex);
    }
    if (context->frame_mutex_initialized &&
        rt_mutex_take(context->frame_mutex, RT_WAITING_FOREVER) == RT_EOK)
    {
        rt_mutex_release(context->frame_mutex);
    }
    if (context->bus_initialized)
    {
        result = aic8800_core_detach(context);
        if (result == RT_EOK)
        {
            result = rt_wlan_offload_bus_deinit(&context->bus);
            if (result == RT_EOK)
            {
                context->bus_initialized = RT_FALSE;
            }
            else
            {
                LOG_E("WLAN offload bus teardown failed: %d", result);
            }
        }
    }
    aic8800_firmware_disconnected(context);
    aic8800_usb_free_buffers(context);
    aic8800_usb_clear_tx_locks(context);
    hport->config.intf[intf].priv = RT_NULL;
    LOG_I("detached AIC USB device %04x:%04x",
          vendor_id, product_id);
    aic8800_usb_reset_context(context, "disconnect");
    return 0;
}

static rt_bool_t aic8800_usb_modeswitch_disconnected(
    const struct usbh_hubport *hport, int result)
{
    return !hport->connected || result == -USB_ERR_NODEV ||
           result == -USB_ERR_NOTCONN || result == -USB_ERR_SHUTDOWN ||
           result == -ENOENT || result == -ECONNRESET ||
           result == -ESHUTDOWN;
}

/* The USB string descriptors identify a rebranded module far better than the
 * placeholder IDs some of them ship with, and they are the first thing worth
 * seeing when a switch does not take. */
static void aic8800_usb_log_device_strings(struct usbh_hubport *hport)
{
    static const struct
    {
        rt_uint8_t index;
        const char *name;
    } strings[] = {
        {USB_STRING_MFC_INDEX, "manufacturer"},
        {USB_STRING_PRODUCT_INDEX, "product"},
        {USB_STRING_SERIAL_INDEX, "serial"},
    };
    rt_size_t index;

    for (index = 0; index < sizeof(strings) / sizeof(strings[0]); index++)
    {
        int result;

        /* usbh_get_string_desc() unpacks UTF-16 in place and neither
         * terminates the result nor reports its length, so the buffer has to
         * be cleared first or whatever the previous command left behind is
         * printed as part of the string. */
        rt_memset(g_aic8800_modeswitch_data, 0,
                  AIC8800_USB_MODESWITCH_DATA_SIZE);
        result = usbh_get_string_desc(
            hport, strings[index].index, g_aic8800_modeswitch_data,
            AIC8800_USB_MODESWITCH_DATA_SIZE - 1U);
        if (result >= 0)
        {
            LOG_I("USB %s: '%s'", strings[index].name,
                  (const char *)g_aic8800_modeswitch_data);
        }
    }
}

/* Run one bulk-only transport command.  Returns 1 when the device dropped off
 * the bus, which for a mode switch is the outcome we want; 0 when the command
 * completed, with the CSW status in *status; a negative error otherwise. */
static int aic8800_usb_modeswitch_command(
    struct usbh_hubport *hport,
    struct usb_endpoint_descriptor *bulk_out,
    struct usb_endpoint_descriptor *bulk_in,
    const rt_uint8_t *cdb, rt_uint8_t cdb_length,
    rt_uint32_t data_length, rt_uint8_t *status)
{
    static rt_uint32_t tag = 0x12345678U;
    struct usbh_urb urb;
    int result;

    if (cdb_length > 16U || data_length > AIC8800_USB_MODESWITCH_DATA_SIZE)
    {
        return -USB_ERR_INVAL;
    }
    tag++;
    rt_memset(g_aic8800_modeswitch_cbw, 0, AIC8800_USB_MODESWITCH_CBW_SIZE);
    g_aic8800_modeswitch_cbw[0] = 0x55;
    g_aic8800_modeswitch_cbw[1] = 0x53;
    g_aic8800_modeswitch_cbw[2] = 0x42;
    g_aic8800_modeswitch_cbw[3] = 0x43;
    g_aic8800_modeswitch_cbw[4] = (rt_uint8_t)tag;
    g_aic8800_modeswitch_cbw[5] = (rt_uint8_t)(tag >> 8);
    g_aic8800_modeswitch_cbw[6] = (rt_uint8_t)(tag >> 16);
    g_aic8800_modeswitch_cbw[7] = (rt_uint8_t)(tag >> 24);
    g_aic8800_modeswitch_cbw[8] = (rt_uint8_t)data_length;
    g_aic8800_modeswitch_cbw[9] = (rt_uint8_t)(data_length >> 8);
    /* bmCBWFlags: every command used here reads, none writes. */
    g_aic8800_modeswitch_cbw[12] = data_length ? 0x80U : 0x00U;
    g_aic8800_modeswitch_cbw[13] = 0;
    g_aic8800_modeswitch_cbw[14] = cdb_length;
    rt_memcpy(&g_aic8800_modeswitch_cbw[15], cdb, cdb_length);

    rt_memset(&urb, 0, sizeof(urb));
    usbh_bulk_urb_fill(&urb, hport, bulk_out, g_aic8800_modeswitch_cbw,
                       AIC8800_USB_MODESWITCH_CBW_SIZE,
                       AIC8800_USB_MODESWITCH_TIMEOUT_MS, RT_NULL, RT_NULL);
    result = usbh_submit_urb(&urb);
    if (result)
    {
        return aic8800_usb_modeswitch_disconnected(hport, result) ?
               1 : result;
    }
    if (urb.actual_length != AIC8800_USB_MODESWITCH_CBW_SIZE)
    {
        return -USB_ERR_IO;
    }

    if (data_length)
    {
        rt_memset(g_aic8800_modeswitch_data, 0, data_length);
        rt_memset(&urb, 0, sizeof(urb));
        usbh_bulk_urb_fill(&urb, hport, bulk_in, g_aic8800_modeswitch_data,
                           data_length,
                           AIC8800_USB_MODESWITCH_TIMEOUT_MS,
                           RT_NULL, RT_NULL);
        result = usbh_submit_urb(&urb);
        if (result && aic8800_usb_modeswitch_disconnected(hport, result))
        {
            return 1;
        }
        /* A stalled or short data phase still leaves the CSW to collect. */
    }

    rt_memset(g_aic8800_modeswitch_csw, 0, AIC8800_USB_MODESWITCH_CSW_SIZE);
    rt_memset(&urb, 0, sizeof(urb));
    usbh_bulk_urb_fill(&urb, hport, bulk_in, g_aic8800_modeswitch_csw,
                       AIC8800_USB_MODESWITCH_CSW_SIZE,
                       AIC8800_USB_MODESWITCH_TIMEOUT_MS, RT_NULL, RT_NULL);
    result = usbh_submit_urb(&urb);
    if (result)
    {
        /* The eject is expected to tear the link down before or during the
         * CSW, so treat an IO error the same as a clean disconnect. */
        return (aic8800_usb_modeswitch_disconnected(hport, result) ||
                result == -USB_ERR_IO) ? 1 : result;
    }
    if (urb.actual_length != AIC8800_USB_MODESWITCH_CSW_SIZE ||
        g_aic8800_modeswitch_csw[0] != 0x55 ||
        g_aic8800_modeswitch_csw[1] != 0x53 ||
        g_aic8800_modeswitch_csw[2] != 0x42 ||
        g_aic8800_modeswitch_csw[3] != 0x53 ||
        rt_memcmp(&g_aic8800_modeswitch_csw[4],
                  &g_aic8800_modeswitch_cbw[4], 4) != 0)
    {
        return -USB_ERR_IO;
    }
    if (status)
    {
        *status = g_aic8800_modeswitch_csw[12];
    }
    return 0;
}

static int aic8800_usb_modeswitch_connect(struct usbh_hubport *hport,
                                           rt_uint8_t intf)
{
    struct usbh_interface_altsetting *setting;
    struct usb_setup_packet *setup;
    struct usb_endpoint_descriptor *bulk_in = RT_NULL;
    struct usb_endpoint_descriptor *bulk_out = RT_NULL;
    int result;
    rt_uint8_t index;

    if (!hport || !hport->connected)
    {
        return -USB_ERR_NODEV;
    }
    setting = &hport->config.intf[intf].altsetting[0];

    /* Some AIC fake-storage firmware does not service a BOT command until
     * the host has completed the standard MSC class initialization. */
    setup = hport->setup;
    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS |
                           USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = AIC8800_USB_MSC_REQUEST_GET_MAX_LUN;
    setup->wValue = 0;
    setup->wIndex = setting->intf_desc.bInterfaceNumber;
    setup->wLength = 1;
    result = usbh_control_transfer(hport, setup, g_aic8800_modeswitch_csw);
    if (result < 0 && result != -USB_ERR_STALL)
    {
        LOG_E("AIC8800FC GET_MAX_LUN failed: %d", result);
        return result;
    }

    for (index = 0; index < setting->intf_desc.bNumEndpoints; index++)
    {
        struct usb_endpoint_descriptor *endpoint =
            &setting->ep[index].ep_desc;

        if (USB_GET_ENDPOINT_TYPE(endpoint->bmAttributes) !=
            USB_ENDPOINT_TYPE_BULK)
        {
            continue;
        }
        if (endpoint->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK)
        {
            bulk_in = endpoint;
        }
        else
        {
            bulk_out = endpoint;
        }
    }
    if (!bulk_in || !bulk_out)
    {
        LOG_E("%04x:%04x MSC interface %u lacks bulk endpoints",
              hport->device_desc.idVendor, hport->device_desc.idProduct,
              intf);
        return -USB_ERR_NODEV;
    }

    LOG_I("attempting MSC mode switch on %04x:%04x",
          hport->device_desc.idVendor, hport->device_desc.idProduct);
    aic8800_usb_log_device_strings(hport);

    /* The vendor tooling ejects through the operating system's storage
     * stack, so the device only ever sees START STOP UNIT after the medium
     * has been probed.  Some fake-storage firmware accepts a bare eject with
     * a good status and then does nothing at all; walking the same path a
     * real host takes is what makes it act.  Everything before the eject is
     * best effort - a device that refuses these still gets ejected. */
    {
        static const rt_uint8_t inquiry[6] =
            {0x12, 0x00, 0x00, 0x00, 0x24, 0x00};
        static const rt_uint8_t test_unit_ready[6] =
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        static const rt_uint8_t allow_removal[6] =
            {0x1e, 0x00, 0x00, 0x00, 0x00, 0x00};
        static const rt_uint8_t read_capacity[10] =
            {0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        static const rt_uint8_t start_stop_eject[6] =
            {0x1b, 0x00, 0x00, 0x00, 0x02, 0x00};
        static const rt_uint8_t start_stop_eject_immed[6] =
            {0x1b, 0x01, 0x00, 0x00, 0x02, 0x00};
        rt_uint8_t read_block[10] =
            {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
        rt_uint32_t block_size = 0;
        rt_uint8_t status = 0xffU;
        rt_uint8_t attempt;

        result = aic8800_usb_modeswitch_command(
            hport, bulk_out, bulk_in, inquiry, sizeof(inquiry), 36U,
            &status);
        if (result == 1)
        {
            goto switched;
        }
        if (result == 0 && status == 0)
        {
            LOG_I("MSC medium: '%.8s' '%.16s' rev '%.4s' type 0x%02x",
                  &g_aic8800_modeswitch_data[8],
                  &g_aic8800_modeswitch_data[16],
                  &g_aic8800_modeswitch_data[32],
                  g_aic8800_modeswitch_data[0] & 0x1fU);
        }
        else
        {
            LOG_D("MSC INQUIRY unavailable (%d, status %u)", result, status);
        }

        for (attempt = 0; attempt < AIC8800_USB_MODESWITCH_READY_TRIES;
             attempt++)
        {
            status = 0xffU;
            result = aic8800_usb_modeswitch_command(
                hport, bulk_out, bulk_in, test_unit_ready,
                sizeof(test_unit_ready), 0U, &status);
            if (result == 1)
            {
                goto switched;
            }
            if (result == 0 && status == 0)
            {
                break;
            }
            rt_thread_mdelay(AIC8800_USB_MODESWITCH_READY_WAIT_MS);
        }
        LOG_D("MSC ready after %u attempt(s)", (unsigned int)attempt + 1U);

        /* Read the capacity and the first block. Fake-storage firmware that
         * gates the switch on the medium actually having been mounted needs
         * to see this; a host that only ejects never gets that far. */
        status = 0xffU;
        result = aic8800_usb_modeswitch_command(
            hport, bulk_out, bulk_in, read_capacity, sizeof(read_capacity),
            8U, &status);
        if (result == 1)
        {
            goto switched;
        }
        if (result == 0 && status == 0)
        {
            block_size = ((rt_uint32_t)g_aic8800_modeswitch_data[4] << 24) |
                         ((rt_uint32_t)g_aic8800_modeswitch_data[5] << 16) |
                         ((rt_uint32_t)g_aic8800_modeswitch_data[6] << 8) |
                         (rt_uint32_t)g_aic8800_modeswitch_data[7];
            LOG_D("MSC capacity: last LBA %u block %u",
                  (unsigned int)(
                      ((rt_uint32_t)g_aic8800_modeswitch_data[0] << 24) |
                      ((rt_uint32_t)g_aic8800_modeswitch_data[1] << 16) |
                      ((rt_uint32_t)g_aic8800_modeswitch_data[2] << 8) |
                      (rt_uint32_t)g_aic8800_modeswitch_data[3]),
                  (unsigned int)block_size);
        }
        if (block_size && block_size <= AIC8800_USB_MODESWITCH_DATA_SIZE)
        {
            result = aic8800_usb_modeswitch_command(
                hport, bulk_out, bulk_in, read_block, sizeof(read_block),
                block_size, RT_NULL);
            if (result == 1)
            {
                goto switched;
            }
        }

        /* Some modules switch on a private command rather than an eject.
         * Usb_Driver.dll in the vendor Windows package exposes exactly two,
         * both 16-byte CDBs with opcode 0xfd and the sub-command in the last
         * byte, and the routine in AicWifiService.exe that calls them reads
         * five identification bytes with 0xf3 ("GetHippo"), compares them
         * against an expected string, and only then issues the data-less
         * 0xf2 ("Set_CS1_0").  That routine never ejects, and on this
         * hardware the eject leaves the medium gone and the private command
         * channel refusing everything afterwards - so once the device has
         * identified itself, follow the vendor path and stop. */
        {
            static const rt_uint8_t vendor_identify[16] =
                {0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf3};
            static const rt_uint8_t vendor_switch[16] =
                {0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf2};

            status = 0xffU;
            result = aic8800_usb_modeswitch_command(
                hport, bulk_out, bulk_in, vendor_identify,
                sizeof(vendor_identify), 5U, &status);
            if (result == 1)
            {
                goto switched;
            }
            if (result == 0 && status == 0)
            {
                LOG_I("vendor identify: %02x %02x %02x %02x %02x '%.5s'",
                      g_aic8800_modeswitch_data[0],
                      g_aic8800_modeswitch_data[1],
                      g_aic8800_modeswitch_data[2],
                      g_aic8800_modeswitch_data[3],
                      g_aic8800_modeswitch_data[4],
                      (const char *)g_aic8800_modeswitch_data);

                status = 0xffU;
                result = aic8800_usb_modeswitch_command(
                    hport, bulk_out, bulk_in, vendor_switch,
                    sizeof(vendor_switch), 0U, &status);
                if (result == 1)
                {
                    goto switched;
                }
                if (result != 0 || status != 0)
                {
                    LOG_E("vendor switch command refused: result %d status %u",
                          result, status);
                    return -USB_ERR_IO;
                }
                /* Accepted. The vendor service closes the handle and forces a
                 * bus rescan after this, so give the module a moment to act
                 * on it before disturbing the port. */
                rt_thread_mdelay(AIC8800_USB_MODESWITCH_SETTLE_MS);
                goto rescan;
            }
            LOG_D("vendor identify unsupported (%d, status %u)",
                  result, status);
            if (hport->device_desc.idVendor ==
                AIC8800_USB_VENDOR_ID_MSC_FACTORY)
            {
                /* 1111:1111 is a shared placeholder rather than an AIC
                 * identity, and the private command is the only positive
                 * proof this is the right module.  Without it, leave the
                 * device completely alone: ejecting a stranger's storage is
                 * worse than not switching ours. */
                LOG_W("%04x:%04x did not answer the AIC vendor command; "
                      "leaving it untouched",
                      hport->device_desc.idVendor,
                      hport->device_desc.idProduct);
                return 0;
            }
        }

        result = aic8800_usb_modeswitch_command(
            hport, bulk_out, bulk_in, allow_removal, sizeof(allow_removal),
            0U, RT_NULL);
        if (result == 1)
        {
            goto switched;
        }

        result = aic8800_usb_modeswitch_command(
            hport, bulk_out, bulk_in, start_stop_eject,
            sizeof(start_stop_eject), 0U, &status);
        if (result == 1)
        {
            goto switched;
        }
        if (result < 0)
        {
            LOG_E("mode-switch eject failed: %d", result);
            return result;
        }
        if (status != 0)
        {
            LOG_E("device rejected the eject: CSW status %u", status);
            return -USB_ERR_IO;
        }

        /* Still here: retry once with IMMED set, which is the form some
         * firmware acts on. */
        rt_thread_mdelay(AIC8800_USB_MODESWITCH_READY_WAIT_MS);
        result = aic8800_usb_modeswitch_command(
            hport, bulk_out, bulk_in, start_stop_eject_immed,
            sizeof(start_stop_eject_immed), 0U, &status);
        if (result == 1)
        {
            goto switched;
        }
        LOG_D("MSC immediate eject: result %d status %u", result, status);
    }

rescan:
    /* Every command was accepted and the device is still here, so it may be
     * waiting for the bus to be cycled: the vendor service follows its work
     * with a "devcon rescan".  Failing the probe makes the hub release the
     * port and drive a fresh reset, which is the same stimulus.  Do it once
     * only - the hub retries three times, and repeating the whole sequence
     * on a module that has already accepted it just walks the device into a
     * state where it answers nothing. */
    if (g_aic8800_modeswitch_rescanned)
    {
        LOG_W("device stayed in MSC mode after the vendor switch sequence");
        return 0;
    }
    g_aic8800_modeswitch_rescanned = RT_TRUE;
    LOG_I("mode-switch commands accepted; forcing port re-enumeration");
    return -USB_ERR_NODEV;

switched:
    g_aic8800_modeswitch_rescanned = RT_FALSE;
    LOG_I("device switched; waiting for USB re-enumeration");
    return 0;
}

static int aic8800_usb_modeswitch_disconnect(struct usbh_hubport *hport,
                                              rt_uint8_t intf)
{
    (void)hport;
    (void)intf;
    /* The device is leaving, so the next one to arrive gets a fresh attempt
     * at the single forced re-enumeration. */
    g_aic8800_modeswitch_rescanned = RT_FALSE;
    return 0;
}

static const struct usbh_class_driver g_aic8800_modeswitch_class_driver = {
    .driver_name = "aic8800-modeswitch",
    .connect = aic8800_usb_modeswitch_connect,
    .disconnect = aic8800_usb_modeswitch_disconnect,
};

static const struct usbh_class_driver g_aic8800_usb_class_driver = {
    .driver_name = "aic8800-wlan_offload",
    .connect = aic8800_usb_connect,
    .disconnect = aic8800_usb_disconnect,
};

#define AIC8800_USB_CLASS_INFO(_name, _vid, _pid)                          \
    CLASS_INFO_DEFINE const struct usbh_class_info _name = {               \
        .match_flags = USB_CLASS_MATCH_VENDOR | USB_CLASS_MATCH_PRODUCT |  \
                       USB_CLASS_MATCH_INTF_CLASS |                         \
                       USB_CLASS_MATCH_INTF_SUBCLASS |                      \
                       USB_CLASS_MATCH_INTF_PROTOCOL,                       \
        .class = 0xff,                                                      \
        .subclass = 0xff,                                                   \
        .protocol = 0xff,                                                   \
        .vid = (_vid),                                                      \
        .pid = (_pid),                                                      \
        .class_driver = &g_aic8800_usb_class_driver,                        \
    }

AIC8800_USB_CLASS_INFO(g_aic8801_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8801);
AIC8800_USB_CLASS_INFO(g_aic8800_boot_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800);
AIC8800_USB_CLASS_INFO(g_aic8800dc_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800DC);
AIC8800_USB_CLASS_INFO(g_aic8800dw_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800DW);
AIC8800_USB_CLASS_INFO(g_aic8800d81_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D81);
AIC8800_USB_CLASS_INFO(g_aic8800d80_boot_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D80);
/* Rebranded D80/D81 runtime identities. All boot as 0x8d80 and take the same
 * firmware; only the identity the runtime reports back differs. Matching on
 * the vendor-specific interface descriptor keeps the Bluetooth interfaces of
 * the composite variants out of the way. */
AIC8800_USB_CLASS_INFO(g_aic8800d83_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D83);
AIC8800_USB_CLASS_INFO(g_aic8800d84_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D84);
AIC8800_USB_CLASS_INFO(g_aic8800d85_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D85);
AIC8800_USB_CLASS_INFO(g_aic8800d86_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D86);
AIC8800_USB_CLASS_INFO(g_aic8800d88_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D88);
AIC8800_USB_CLASS_INFO(g_aic8800d81_v2_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D81);
AIC8800_USB_CLASS_INFO(g_aic8800d83_v2_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D83);
AIC8800_USB_CLASS_INFO(g_aic8800d84_v2_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D84);
AIC8800_USB_CLASS_INFO(g_aic8800d85_v2_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D85);
AIC8800_USB_CLASS_INFO(g_aic8800d86_v2_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D86);
AIC8800_USB_CLASS_INFO(g_aic8800d88_v2_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D88);
AIC8800_USB_CLASS_INFO(g_aic8800d41_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D41);
AIC8800_USB_CLASS_INFO(g_aic8800d40_boot_class_info,
                       AIC8800_USB_VENDOR_ID,
                       AIC8800_USB_PID_AIC8800D40);
AIC8800_USB_CLASS_INFO(g_aic8800d81x2_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D81X2);
AIC8800_USB_CLASS_INFO(g_aic8800d89x2_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D89X2);
AIC8800_USB_CLASS_INFO(g_aic8800d80x2_boot_class_info,
                       AIC8800_USB_VENDOR_ID_V2,
                       AIC8800_USB_PID_AIC8800D80X2);

#define AIC8800_USB_MODESWITCH_CLASS_INFO(_name, _vid, _pid)              \
    CLASS_INFO_DEFINE const struct usbh_class_info _name = {              \
        .match_flags = USB_CLASS_MATCH_VENDOR | USB_CLASS_MATCH_PRODUCT | \
                       USB_CLASS_MATCH_INTF_CLASS |                        \
                       USB_CLASS_MATCH_INTF_SUBCLASS |                     \
                       USB_CLASS_MATCH_INTF_PROTOCOL,                      \
        .class = USB_DEVICE_CLASS_MASS_STORAGE,                            \
        .subclass = AIC8800_USB_MSC_SUBCLASS_SCSI,                         \
        .protocol = AIC8800_USB_MSC_PROTOCOL_BULK_ONLY,                    \
        .vid = (_vid),                                                     \
        .pid = (_pid),                                                     \
        .class_driver = &g_aic8800_modeswitch_class_driver,                \
    }

AIC8800_USB_MODESWITCH_CLASS_INFO(g_aic8800_modeswitch_class_info,
                                  AIC8800_USB_VENDOR_ID,
                                  AIC8800_USB_PID_MSC);
#ifdef AIC8800_WIFI_USB_MODESWITCH_PLACEHOLDER_ID
/* 0x1111:0x1111 is not an AIC identity: 0x1111 is not AIC's USB-IF vendor ID
 * (0xa69c is), and no vendor INF in either Windows driver package binds it.
 * It is the placeholder that unprogrammed and no-name devices ship with, so
 * claiming it here takes the mass-storage interface away from any such
 * device - including ordinary USB sticks - and ejects it. Opt in only while
 * bringing up a module known to use it. */
AIC8800_USB_MODESWITCH_CLASS_INFO(g_aic8800_factory_modeswitch_class_info,
                                  AIC8800_USB_VENDOR_ID_MSC_FACTORY,
                                  AIC8800_USB_PID_MSC_FACTORY);
#endif

rt_err_t aic8800_usb_driver_init(void)
{
    rt_err_t result;

    if (!g_aic8800_transport_ipc_initialized)
    {
        result = rt_mutex_init(&g_aic8800_tx_mutex, "aic-tx",
                               RT_IPC_FLAG_PRIO);
        if (result != RT_EOK)
        {
            return result;
        }
        result = rt_mutex_init(&g_aic8800_frame_mutex, "aic-frm",
                               RT_IPC_FLAG_PRIO);
        if (result != RT_EOK)
        {
            rt_mutex_detach(&g_aic8800_tx_mutex);
            return result;
        }
        result = rt_sem_init(&g_aic8800_tx_available, "aic-txf",
                             AIC8800_WIFI_DATA_TX_URBS,
                             RT_IPC_FLAG_FIFO);
        if (result != RT_EOK)
        {
            rt_mutex_detach(&g_aic8800_frame_mutex);
            rt_mutex_detach(&g_aic8800_tx_mutex);
            return result;
        }
        g_aic8800_transport_ipc_initialized = RT_TRUE;
    }
    if (g_aic8800_attach_workqueue)
    {
        return RT_EOK;
    }
    g_aic8800_attach_workqueue = rt_workqueue_create(
        "aic-attach", AIC8800_WIFI_ATTACH_THREAD_STACK_SIZE,
        AIC8800_WIFI_ATTACH_THREAD_PRIORITY);
    if (!g_aic8800_attach_workqueue)
    {
        return -RT_ENOMEM;
    }
    return RT_EOK;
}

static int aic8800_usb_component_init(void)
{
    rt_err_t result;

#ifdef AIC8800_WIFI_BLE
    result = aic8800_btusb_driver_init();
    if (result != RT_EOK)
    {
        return result;
    }
#endif
    result = aic8800_usb_driver_init();
    return result;
}
INIT_COMPONENT_EXPORT(aic8800_usb_component_init);
