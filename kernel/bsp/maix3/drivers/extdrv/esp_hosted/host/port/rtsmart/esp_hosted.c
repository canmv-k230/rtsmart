/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RT-Thread WLAN and Bluetooth adapter for ESP-Hosted-MCU.
 */
#include "esp_hosted.h"
#include "esp_hosted_control.h"
#include "esp_hosted_country.h"
#ifdef ESP_HOSTED_BLE
#include "esp_hosted_hci.h"
#endif
#include "esp_hosted_rpc_api.h"
#include "rpc_core.h"
#include "esp_hosted_transport.h"

#include <rtdevice.h>
#include <wlan_dev.h>
#include <wlan_mgnt.h>

#define DBG_TAG "esp.hosted"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define EH_WLAN_EVENT_QUEUE_DEPTH  8
#define EH_MQ_POOL_SIZE(depth, type) \
    ((depth) * (RT_ALIGN(sizeof(type), RT_ALIGN_SIZE) + sizeof(void *)))
#ifndef ESP_HOSTED_EVENT_THREAD_STACK_SIZE
#define ESP_HOSTED_EVENT_THREAD_STACK_SIZE 8192
#endif

struct eh_wlan_event
{
    uint8_t ap_interface;
    uint8_t event;
    uint8_t length;
    uint8_t data[sizeof(struct rt_wlan_info)];
};

struct eh_context
{
    struct rt_wlan_device sta;
    struct rt_wlan_device ap;
    struct rt_messagequeue wlan_event_queue;
    struct rt_mutex state_mutex;
    rt_thread_t event_thread;
    volatile rt_bool_t ready;
    volatile rt_bool_t config_pending;
    volatile rt_bool_t connecting;
    rt_bool_t private_config_sent;
    rt_bool_t rpc_init_seen;
    rt_bool_t wifi_initialized;
    rt_bool_t wifi_started;
#ifdef ESP_HOSTED_BLE
    rt_bool_t bt_supported;
    rt_bool_t bt_controller_initialized;
    rt_bool_t bt_controller_enabled;
#endif
    uint8_t wifi_mode;
    uint8_t coprocessor_id;
    uint32_t coprocessor_version;
    uint8_t wlan_event_pool[EH_MQ_POOL_SIZE(EH_WLAN_EVENT_QUEUE_DEPTH, struct eh_wlan_event)]
        __attribute__((aligned(RT_ALIGN_SIZE)));
};

static struct eh_context g_eh;
static rt_bool_t g_eh_initialized;
static rt_bool_t g_eh_initializing;

static rt_err_t eh_wlan_get_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[]);
static void eh_update_ready(void);

static rt_err_t eh_queue_frame(uint8_t interface, uint8_t flags,
                               const void *data, size_t length, rt_bool_t control)
{
    return esp_hosted_transport_send(interface, flags, data, length, control,
                                     RT_NULL, RT_NULL);
}

static rt_wlan_security_t eh_security_from_esp(
    enum esp_hosted_wifi_authmode authmode)
{
    switch (authmode)
    {
    case ESP_HOSTED_WIFI_AUTH_OPEN: return SECURITY_OPEN;
    case ESP_HOSTED_WIFI_AUTH_WEP: return SECURITY_WEP_PSK;
    case ESP_HOSTED_WIFI_AUTH_WPA_PSK: return SECURITY_WPA_AES_PSK;
    case ESP_HOSTED_WIFI_AUTH_WPA2_PSK: return SECURITY_WPA2_AES_PSK;
    case ESP_HOSTED_WIFI_AUTH_WPA_WPA2_PSK: return SECURITY_WPA_WPA2_MIXED_PSK;
    case ESP_HOSTED_WIFI_AUTH_WPA2_ENTERPRISE: return SECURITY_WPA2_AES_8021X;
    case ESP_HOSTED_WIFI_AUTH_WPA3_PSK: return SECURITY_WPA3_AES_PSK;
    case ESP_HOSTED_WIFI_AUTH_WPA2_WPA3_PSK: return SECURITY_WPA3_AES_PSK;
    default: return SECURITY_UNKNOWN;
    }
}

static enum esp_hosted_wifi_authmode eh_authmode_from_security(
    rt_wlan_security_t security)
{
    if (security == SECURITY_OPEN)
    {
        return ESP_HOSTED_WIFI_AUTH_OPEN;
    }
    if (security & WPA3_SECURITY)
    {
        return ESP_HOSTED_WIFI_AUTH_WPA3_PSK;
    }
    if ((security & WPA_SECURITY) && (security & WPA2_SECURITY))
    {
        return ESP_HOSTED_WIFI_AUTH_WPA_WPA2_PSK;
    }
    if (security & WPA2_SECURITY)
    {
        return ESP_HOSTED_WIFI_AUTH_WPA2_PSK;
    }
    if (security & WPA_SECURITY)
    {
        return ESP_HOSTED_WIFI_AUTH_WPA_PSK;
    }
    if (security & WEP_ENABLED)
    {
        return ESP_HOSTED_WIFI_AUTH_WEP;
    }
    return ESP_HOSTED_WIFI_AUTH_OPEN;
}

static void eh_report_event(struct rt_wlan_device *device,
                            rt_wlan_dev_event_t event, void *data, int length)
{
    struct rt_wlan_buff buffer;

    if (data && length)
    {
        buffer.data = data;
        buffer.len = length;
        rt_wlan_dev_indicate_event_handle(device, event, &buffer);
    }
    else
    {
        rt_wlan_dev_indicate_event_handle(device, event, RT_NULL);
    }
}

static void eh_queue_wlan_event(rt_bool_t ap_interface,
                                rt_wlan_dev_event_t event,
                                const void *data, size_t length)
{
    struct eh_wlan_event wlan_event;

    rt_memset(&wlan_event, 0, sizeof(wlan_event));
    wlan_event.ap_interface = ap_interface;
    wlan_event.event = event;
    wlan_event.length = length < sizeof(wlan_event.data) ? length : sizeof(wlan_event.data);
    if (wlan_event.length)
    {
        rt_memcpy(wlan_event.data, data, wlan_event.length);
    }
    if (rt_mq_send(&g_eh.wlan_event_queue, &wlan_event, sizeof(wlan_event)) != RT_EOK)
    {
        LOG_W("WLAN event queue is full");
    }
}

static void eh_wlan_event_thread(void *argument)
{
    struct eh_wlan_event event;
    rt_err_t result;

    (void)argument;
    while (!g_eh.ready)
    {
        rt_thread_mdelay(100);
    }

    result = rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
    if (result != RT_EOK)
    {
        LOG_E("cannot enable station WLAN mode: %d", result);
    }
    else
    {
#ifdef RT_USING_NETDEV
        if (g_eh.sta.netdev)
        {
            LOG_I("station lwIP netdev registered");
        }
        else
        {
            LOG_E("station WLAN mode has no lwIP netdev");
        }
#else
        LOG_W("RT_USING_NETDEV is disabled; no station netdev was created");
#endif
    }

    result = rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP);
    if (result != RT_EOK)
    {
        LOG_E("cannot enable AP WLAN mode: %d", result);
    }
    else
    {
#ifdef RT_USING_NETDEV
        if (g_eh.ap.netdev)
        {
            LOG_I("AP lwIP netdev registered (SoftAP disabled)");
        }
        else
        {
            LOG_E("AP WLAN mode has no lwIP netdev");
        }
#else
        LOG_W("RT_USING_NETDEV is disabled; no AP netdev was created");
#endif
    }

    while (1)
    {
        if (rt_mq_recv(&g_eh.wlan_event_queue, &event, sizeof(event),
                       RT_WAITING_FOREVER) == RT_EOK)
        {
            eh_report_event(event.ap_interface ? &g_eh.ap : &g_eh.sta,
                            event.event,
                            event.length ? event.data : RT_NULL,
                            event.length);
        }
    }
}

static void eh_begin_new_session(void)
{
    g_eh.ready = RT_FALSE;
    g_eh.config_pending = RT_FALSE;
    g_eh.connecting = RT_FALSE;
    g_eh.private_config_sent = RT_FALSE;
    g_eh.rpc_init_seen = RT_FALSE;
    g_eh.wifi_initialized = RT_FALSE;
    g_eh.wifi_started = RT_FALSE;
#ifdef ESP_HOSTED_BLE
    g_eh.bt_supported = RT_FALSE;
    g_eh.bt_controller_initialized = RT_FALSE;
    g_eh.bt_controller_enabled = RT_FALSE;
#endif
    g_eh.wifi_mode = 0;
    esp_hosted_rpc_reset();
#ifdef ESP_HOSTED_BLE
    esp_hosted_hci_reset();
#endif
}

static void eh_handle_rpc_api_event(
    const struct esp_hosted_rpc_api_event *event, void *argument)
{
    struct rt_wlan_info info;

    (void)argument;
    switch (event->id)
    {
    case ESP_HOSTED_API_EVENT_INIT:
        if (g_eh.ready)
        {
            eh_begin_new_session();
        }
        g_eh.rpc_init_seen = RT_TRUE;
        LOG_I("coprocessor RPC initialized: reset-reason=%u",
              event->data.reset_reason);
        eh_update_ready();
        break;
    case ESP_HOSTED_API_EVENT_STA_CONNECTED:
        g_eh.connecting = RT_FALSE;
        eh_queue_wlan_event(RT_FALSE, RT_WLAN_DEV_EVT_CONNECT, RT_NULL, 0);
        break;
    case ESP_HOSTED_API_EVENT_STA_DISCONNECTED:
        if (g_eh.connecting)
        {
            g_eh.connecting = RT_FALSE;
            LOG_W("station connection failed; notifying RT-Thread WLAN");
            eh_queue_wlan_event(RT_FALSE, RT_WLAN_DEV_EVT_CONNECT_FAIL, RT_NULL, 0);
        }
        else
        {
            LOG_W("station disconnected; notifying RT-Thread WLAN");
            eh_queue_wlan_event(RT_FALSE, RT_WLAN_DEV_EVT_DISCONNECT, RT_NULL, 0);
        }
        break;
    case ESP_HOSTED_API_EVENT_AP_STA_CONNECTED:
    case ESP_HOSTED_API_EVENT_AP_STA_DISCONNECTED:
        INVALID_INFO(&info);
        rt_memcpy(info.bssid, event->data.mac, sizeof(info.bssid));
        eh_queue_wlan_event(RT_TRUE,
                            event->id == ESP_HOSTED_API_EVENT_AP_STA_CONNECTED ?
                            RT_WLAN_DEV_EVT_AP_ASSOCIATED :
                            RT_WLAN_DEV_EVT_AP_DISASSOCIATED,
                            &info, sizeof(info));
        break;
    case ESP_HOSTED_API_EVENT_WIFI:
        LOG_D("Wi-Fi event %d", event->data.wifi_event);
        break;
    case ESP_HOSTED_API_EVENT_SCAN_DONE:
        LOG_D("scan event: %d APs", event->data.scan_count);
        break;
    default:
        break;
    }
}

static void eh_update_ready(void)
{
    uint32_t version = g_eh.coprocessor_version;

    if (g_eh.ready || g_eh.config_pending || !g_eh.private_config_sent ||
        !g_eh.rpc_init_seen)
    {
        return;
    }
    g_eh.ready = RT_TRUE;
    esp_hosted_rpc_set_ready(RT_TRUE);
    LOG_I("transport ready: %s, chip=0x%02x firmware=%u.%u.%u",
          esp_hosted_transport_name(), g_eh.coprocessor_id,
          (unsigned int)((version >> 16) & 0xff),
          (unsigned int)((version >> 8) & 0xff),
          (unsigned int)(version & 0xff));
}

static void eh_control_configured(rt_err_t result, void *argument)
{
    (void)argument;
    g_eh.config_pending = RT_FALSE;
    if (result != RT_EOK)
    {
        g_eh.private_config_sent = RT_FALSE;
        LOG_E("failed to send coprocessor configuration: %d", result);
        return;
    }

    g_eh.private_config_sent = RT_TRUE;
    eh_update_ready();
}

static void eh_control_new_session(const struct esp_hosted_control_info *info,
                                   void *argument)
{
    (void)argument;
    /* Private and RPC init notifications can be delivered in either order.
     * Clear an established session, but preserve an RPC init notification
     * which has already arrived for the session currently starting. */
    if (g_eh.ready)
    {
        eh_begin_new_session();
    }
    g_eh.config_pending = RT_FALSE;
    g_eh.private_config_sent = RT_FALSE;
#ifdef ESP_HOSTED_BLE
    g_eh.bt_supported = info->bluetooth_supported;
    if (g_eh.bt_supported)
    {
        LOG_I("BLE controller transport available (%s)",
              info->bluetooth_dual_mode ? "dual-mode" : "BLE-only");
    }
    else
    {
        LOG_W("BLE HCI enabled but not advertised by coprocessor");
    }
#endif
    g_eh.coprocessor_id = info->coprocessor_id;
    g_eh.coprocessor_version = info->firmware_version;
    g_eh.config_pending = RT_TRUE;
}

static void eh_transport_receive(void *argument, uint8_t interface,
                                 uint8_t flags, const uint8_t *payload,
                                 size_t length)
{
    (void)argument;
    switch (interface)
    {
    case ESP_HOSTED_TRANSPORT_IF_STA:
        rt_wlan_dev_report_data(&g_eh.sta, (void *)payload, length);
        break;
    case ESP_HOSTED_TRANSPORT_IF_AP:
        rt_wlan_dev_report_data(&g_eh.ap, (void *)payload, length);
        break;
    case ESP_HOSTED_TRANSPORT_IF_SERIAL:
        esp_hosted_rpc_receive(payload, length, flags);
        break;
#ifdef ESP_HOSTED_BLE
    case ESP_HOSTED_TRANSPORT_IF_HCI:
        esp_hosted_hci_receive(payload, length);
        break;
#endif
    case ESP_HOSTED_TRANSPORT_IF_PRIVATE:
        esp_hosted_control_receive(payload, length);
        break;
    default:
        LOG_D("ignored interface %u", interface);
        break;
    }
}

static rt_err_t eh_wait_ready(void)
{
    rt_tick_t start = rt_tick_get();
    rt_tick_t timeout = rt_tick_from_millisecond(ESP_HOSTED_RPC_TIMEOUT_MS);

    while (!g_eh.ready)
    {
        if ((rt_tick_get() - start) >= timeout)
        {
            LOG_E("%s startup timeout: private-config=%d rpc-init=%d",
                  esp_hosted_transport_name(), g_eh.private_config_sent,
                  g_eh.rpc_init_seen);
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(10);
    }
    return RT_EOK;
}

#ifdef ESP_HOSTED_BLE
static rt_err_t eh_bt_feature_control(uint8_t command)
{
    return esp_hosted_rpc_feature_control(
        (enum esp_hosted_feature_command)command);
}
#endif

static rt_err_t eh_wifi_init_locked(void)
{
    rt_err_t result;

    if (g_eh.wifi_initialized)
    {
        return RT_EOK;
    }

    result = esp_hosted_rpc_wifi_init(g_eh.coprocessor_id);
    if (result == RT_EOK)
    {
        g_eh.wifi_initialized = RT_TRUE;
    }
    return result;
}

static rt_err_t eh_set_mode_locked(uint8_t mode)
{
    rt_err_t result;

    if (mode == g_eh.wifi_mode)
    {
        return RT_EOK;
    }
    result = esp_hosted_rpc_wifi_set_mode(mode);
    if (result == RT_EOK)
    {
        g_eh.wifi_mode = mode;
    }
    return result;
}

static rt_err_t eh_start_locked(void)
{
    rt_err_t result;

    if (g_eh.wifi_started)
    {
        return RT_EOK;
    }
    result = esp_hosted_rpc_wifi_start();
    if (result == RT_EOK)
    {
        g_eh.wifi_started = RT_TRUE;
    }
    return result;
}

static rt_err_t eh_wlan_init(struct rt_wlan_device *wlan)
{
    rt_err_t result;

    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    rt_mutex_release(&g_eh.state_mutex);
    if (result == RT_EOK)
    {
        eh_report_event(wlan, RT_WLAN_DEV_EVT_INIT_DONE, RT_NULL, 0);
    }
    return result;
}

static rt_err_t eh_wlan_mode(struct rt_wlan_device *wlan, rt_wlan_mode_t mode)
{
    uint8_t new_mode;
    rt_err_t result;

    if ((wlan == &g_eh.sta && mode != RT_WLAN_STATION && mode != RT_WLAN_NONE) ||
        (wlan == &g_eh.ap && mode != RT_WLAN_AP && mode != RT_WLAN_NONE))
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    if (result == RT_EOK)
    {
        uint8_t interface_mode = wlan == &g_eh.sta ?
                                 ESP_HOSTED_WIFI_MODE_STA :
                                 ESP_HOSTED_WIFI_MODE_AP;

        /* RT-Thread selects AP mode while attaching the AP netdev. Keep the
         * coprocessor AP disabled until wlan_softap supplies its configuration. */
        if (wlan != &g_eh.ap || mode == RT_WLAN_NONE)
        {
            new_mode = mode == RT_WLAN_NONE ? (g_eh.wifi_mode & ~interface_mode)
                                            : (g_eh.wifi_mode | interface_mode);
            result = eh_set_mode_locked(new_mode);
        }
    }
    rt_mutex_release(&g_eh.state_mutex);
    return result;
}

static rt_err_t eh_wlan_join(struct rt_wlan_device *wlan, struct rt_sta_info *info)
{
    struct esp_hosted_wifi_sta_config config;
    rt_err_t result;

    (void)wlan;
    config.ssid = info->ssid.val;
    config.ssid_length = info->ssid.len;
    config.password = info->key.val;
    config.password_length = info->key.len;
    config.bssid = info->bssid;
    config.channel = info->channel;

    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    if (result == RT_EOK)
    {
        result = eh_set_mode_locked(g_eh.wifi_mode |
                                    ESP_HOSTED_WIFI_MODE_STA);
    }
    if (result == RT_EOK)
    {
        result = esp_hosted_rpc_wifi_set_sta_config(&config);
    }
    if (result == RT_EOK)
    {
        result = eh_start_locked();
    }
    if (result == RT_EOK)
    {
        g_eh.connecting = RT_TRUE;
        result = esp_hosted_rpc_wifi_connect();
    }
    if (result != RT_EOK)
    {
        g_eh.connecting = RT_FALSE;
    }
    rt_mutex_release(&g_eh.state_mutex);
    return result;
}

static rt_err_t eh_wlan_softap(struct rt_wlan_device *wlan, struct rt_ap_info *info)
{
    struct esp_hosted_wifi_ap_config config;
    uint8_t mac[6];
    rt_err_t result;

    (void)wlan;
    config.ssid = info->ssid.val;
    config.ssid_length = info->ssid.len;
    config.password = info->key.val;
    config.password_length = info->key.len;
    config.channel = info->channel;
    config.authmode = eh_authmode_from_security(info->security);
    config.hidden = info->hidden;
    config.max_connections = 4;
    config.beacon_interval = 100;

    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    if (result == RT_EOK)
    {
        result = eh_set_mode_locked(g_eh.wifi_mode |
                                    ESP_HOSTED_WIFI_MODE_AP);
    }
    if (result == RT_EOK)
    {
        result = esp_hosted_rpc_wifi_set_ap_config(&config);
    }
    if (result == RT_EOK)
    {
        result = eh_start_locked();
    }
    rt_mutex_release(&g_eh.state_mutex);
    if (result == RT_EOK)
    {
        rt_err_t mac_result = esp_hosted_rpc_wifi_get_mac(
            ESP_HOSTED_WIFI_IF_AP, mac);

        if (mac_result == RT_EOK)
        {
            eh_report_event(&g_eh.ap, RT_WLAN_DEV_EVT_AP_START, mac, sizeof(mac));
        }
        else
        {
            LOG_W("cannot read SoftAP MAC address: %d", mac_result);
            eh_report_event(&g_eh.ap, RT_WLAN_DEV_EVT_AP_START, RT_NULL, 0);
        }
    }
    return result;
}

static rt_err_t eh_wlan_disconnect(struct rt_wlan_device *wlan)
{
    (void)wlan;
    g_eh.connecting = RT_FALSE;
    return esp_hosted_rpc_wifi_disconnect();
}

static rt_err_t eh_wlan_ap_stop(struct rt_wlan_device *wlan)
{
    rt_err_t result;

    (void)wlan;
    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_set_mode_locked(g_eh.wifi_mode &
                                ~ESP_HOSTED_WIFI_MODE_AP);
    rt_mutex_release(&g_eh.state_mutex);
    if (result == RT_EOK)
    {
        eh_report_event(&g_eh.ap, RT_WLAN_DEV_EVT_AP_STOP, RT_NULL, 0);
    }
    return result;
}

static rt_err_t eh_wlan_ap_deauth(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    if (wlan != &g_eh.ap || !mac)
    {
        return -RT_EINVAL;
    }
    return esp_hosted_rpc_wifi_ap_deauth(mac);
}

static void eh_report_scan_record(
    const struct esp_hosted_wifi_ap_record *record, void *argument)
{
    struct rt_wlan_info info;

    (void)argument;
    INVALID_INFO(&info);
    info.datarate = 0;
    info.hidden = 0;
    rt_memcpy(info.bssid, record->bssid, sizeof(info.bssid));
    info.ssid.len = record->ssid_length < RT_WLAN_SSID_MAX_LENGTH ?
                    record->ssid_length : RT_WLAN_SSID_MAX_LENGTH;
    rt_memcpy(info.ssid.val, record->ssid, info.ssid.len);
    info.ssid.val[info.ssid.len] = '\0';
    info.channel = record->channel;
    info.band = info.channel <= 14 ? RT_802_11_BAND_2_4GHZ :
                                    RT_802_11_BAND_5GHZ;
    info.rssi = record->rssi;
    info.security = eh_security_from_esp(record->authmode);
    eh_report_event(&g_eh.sta, RT_WLAN_DEV_EVT_SCAN_REPORT, &info, sizeof(info));
}

static rt_err_t eh_wlan_scan(struct rt_wlan_device *wlan, struct rt_scan_info *scan_info)
{
    rt_err_t result;

    (void)wlan;
    (void)scan_info;
    rt_mutex_take(&g_eh.state_mutex, RT_WAITING_FOREVER);
    result = eh_wifi_init_locked();
    if (result == RT_EOK)
    {
        result = eh_set_mode_locked(g_eh.wifi_mode |
                                    ESP_HOSTED_WIFI_MODE_STA);
    }
    if (result == RT_EOK)
    {
        result = eh_start_locked();
    }
    rt_mutex_release(&g_eh.state_mutex);
    if (result != RT_EOK)
    {
        return result;
    }

    result = esp_hosted_rpc_wifi_scan(ESP_HOSTED_MAX_SCAN_RESULTS,
                                      eh_report_scan_record, RT_NULL);
    eh_report_event(&g_eh.sta, RT_WLAN_DEV_EVT_SCAN_DONE, RT_NULL, 0);
    return result;
}

static rt_err_t eh_wlan_scan_stop(struct rt_wlan_device *wlan)
{
    (void)wlan;
    return esp_hosted_rpc_wifi_scan_stop();
}

static int eh_wlan_get_rssi(struct rt_wlan_device *wlan)
{
    int rssi = -127;

    (void)wlan;
    esp_hosted_rpc_wifi_get_rssi(&rssi);
    return rssi;
}

static rt_err_t eh_wlan_set_powersave(struct rt_wlan_device *wlan, int level)
{
    (void)wlan;
    if (level < 0 || level > 2)
    {
        return -RT_EINVAL;
    }
    return esp_hosted_rpc_wifi_set_power_save(level);
}

static int eh_wlan_get_powersave(struct rt_wlan_device *wlan)
{
    int level = -1;

    (void)wlan;
    esp_hosted_rpc_wifi_get_power_save(&level);
    return level;
}

static rt_err_t eh_wlan_set_channel(struct rt_wlan_device *wlan, int channel)
{
    (void)wlan;
    if (channel < 1 || channel > 196)
    {
        return -RT_EINVAL;
    }
    return esp_hosted_rpc_wifi_set_channel(channel);
}

static int eh_wlan_get_channel(struct rt_wlan_device *wlan)
{
    int channel = -1;

    (void)wlan;
    esp_hosted_rpc_wifi_get_channel(&channel);
    return channel;
}

static rt_err_t eh_wlan_get_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    return esp_hosted_rpc_wifi_get_mac(
        wlan == &g_eh.sta ? ESP_HOSTED_WIFI_IF_STA : ESP_HOSTED_WIFI_IF_AP,
        mac);
}

static rt_err_t eh_wlan_set_mac(struct rt_wlan_device *wlan, rt_uint8_t mac[])
{
    return esp_hosted_rpc_wifi_set_mac(
        wlan == &g_eh.sta ? ESP_HOSTED_WIFI_IF_STA : ESP_HOSTED_WIFI_IF_AP,
        mac);
}

static int eh_wlan_send(struct rt_wlan_device *wlan, void *buffer, int length)
{
    rt_err_t result;

    if (!g_eh.ready || !esp_hosted_transport_can_send_data() || length <= 0 ||
        (size_t)length > esp_hosted_transport_max_payload())
    {
        return -RT_EBUSY;
    }
    result = eh_queue_frame(
        wlan == &g_eh.sta ? ESP_HOSTED_TRANSPORT_IF_STA :
                            ESP_HOSTED_TRANSPORT_IF_AP,
        0, buffer, length, RT_FALSE);
    /* rt_wlan_dev_ops::wlan_send uses rt_err_t semantics. Returning the frame
     * length makes lwIP treat an accepted packet as a link-layer failure. */
    return result;
}

static rt_err_t eh_wlan_set_country(struct rt_wlan_device *wlan,
                                    rt_country_code_t country)
{
    const char *country_code;

    (void)wlan;
    country_code = eh_country_code_from_rt(country);
    if (!country_code)
    {
        return -RT_EINVAL;
    }

    return esp_hosted_rpc_wifi_set_country_code(country_code);
}

static rt_country_code_t eh_get_country(struct rt_wlan_device *wlan)
{
    char country_code[3];

    (void)wlan;
    if (esp_hosted_rpc_wifi_get_country_code(country_code) != RT_EOK)
    {
        return RT_COUNTRY_UNKNOWN;
    }
    return eh_country_code_to_rt((const uint8_t *)country_code, 2);
}

static const struct rt_wlan_dev_ops g_eh_wlan_ops = {
    .wlan_init = eh_wlan_init,
    .wlan_mode = eh_wlan_mode,
    .wlan_scan = eh_wlan_scan,
    .wlan_join = eh_wlan_join,
    .wlan_softap = eh_wlan_softap,
    .wlan_disconnect = eh_wlan_disconnect,
    .wlan_ap_stop = eh_wlan_ap_stop,
    .wlan_ap_deauth = eh_wlan_ap_deauth,
    .wlan_scan_stop = eh_wlan_scan_stop,
    .wlan_get_rssi = eh_wlan_get_rssi,
    .wlan_set_powersave = eh_wlan_set_powersave,
    .wlan_get_powersave = eh_wlan_get_powersave,
    .wlan_cfg_promisc = RT_NULL,
    .wlan_cfg_filter = RT_NULL,
    .wlan_cfg_mgnt_filter = RT_NULL,
    .wlan_set_channel = eh_wlan_set_channel,
    .wlan_get_channel = eh_wlan_get_channel,
    .wlan_set_country = eh_wlan_set_country,
    .wlan_get_country = eh_get_country,
    .wlan_set_mac = eh_wlan_set_mac,
    .wlan_get_mac = eh_wlan_get_mac,
    .wlan_recv = RT_NULL,
    .wlan_send = eh_wlan_send,
    .wlan_send_raw_frame = RT_NULL,
};

rt_bool_t rt_esp_hosted_is_ready(void)
{
    return g_eh.ready;
}

#ifdef ESP_HOSTED_BLE
rt_bool_t rt_esp_hosted_bt_supported(void)
{
    return g_eh.bt_supported;
}

rt_err_t rt_esp_hosted_bt_controller_start(void)
{
    rt_err_t result;

    result = eh_wait_ready();
    if (result != RT_EOK)
    {
        return result;
    }
    if (!g_eh.bt_supported)
    {
        return -RT_ENOSYS;
    }
    if (!g_eh.bt_controller_initialized)
    {
        result = eh_bt_feature_control(ESP_HOSTED_FEATURE_BT_INIT);
        if (result != RT_EOK)
        {
            LOG_E("cannot initialize Bluetooth controller: %d", result);
            return result;
        }
        g_eh.bt_controller_initialized = RT_TRUE;
    }
    if (!g_eh.bt_controller_enabled)
    {
        result = eh_bt_feature_control(ESP_HOSTED_FEATURE_BT_ENABLE);
        if (result != RT_EOK)
        {
            rt_err_t cleanup_result;

            LOG_E("cannot enable Bluetooth controller: %d", result);
            cleanup_result = eh_bt_feature_control(ESP_HOSTED_FEATURE_BT_DEINIT);
            if (cleanup_result == RT_EOK)
            {
                g_eh.bt_controller_initialized = RT_FALSE;
            }
            else
            {
                LOG_W("cannot roll back Bluetooth controller initialization: %d",
                      cleanup_result);
            }
            return result;
        }
        g_eh.bt_controller_enabled = RT_TRUE;
    }
    LOG_I("Bluetooth controller initialized and enabled");
    return RT_EOK;
}

rt_err_t rt_esp_hosted_bt_controller_stop(void)
{
    rt_err_t result = RT_EOK;
    rt_err_t deinit_result;

    if (g_eh.bt_controller_enabled)
    {
        result = eh_bt_feature_control(ESP_HOSTED_FEATURE_BT_DISABLE);
        if (result == RT_EOK)
        {
            g_eh.bt_controller_enabled = RT_FALSE;
        }
        else
        {
            LOG_W("cannot disable Bluetooth controller: %d", result);
        }
    }
    if (g_eh.bt_controller_initialized && !g_eh.bt_controller_enabled)
    {
        deinit_result = eh_bt_feature_control(ESP_HOSTED_FEATURE_BT_DEINIT);
        if (deinit_result == RT_EOK)
        {
            g_eh.bt_controller_initialized = RT_FALSE;
        }
        else
        {
            LOG_W("cannot deinitialize Bluetooth controller: %d", deinit_result);
            if (result == RT_EOK)
            {
                result = deinit_result;
            }
        }
    }
    return result;
}
#endif

int rt_hw_esp_hosted_init(void)
{
    const struct esp_hosted_transport_callbacks transport_callbacks = {
        .receive = eh_transport_receive,
    };
    const struct esp_hosted_rpc_callbacks rpc_callbacks = {
        .event = esp_hosted_rpc_api_handle_event,
    };
    const struct esp_hosted_rpc_api_callbacks api_callbacks = {
        .event = eh_handle_rpc_api_event,
    };
    const struct esp_hosted_control_callbacks control_callbacks = {
        .new_session = eh_control_new_session,
        .configured = eh_control_configured,
    };
    rt_err_t result;

    if (g_eh_initialized)
    {
        return RT_EOK;
    }
    if (g_eh_initializing)
    {
        return -RT_EBUSY;
    }
    g_eh_initializing = RT_TRUE;
    rt_memset(&g_eh, 0, sizeof(g_eh));
    result = rt_mutex_init(&g_eh.state_mutex, "eh-state", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        g_eh_initializing = RT_FALSE;
        return result;
    }
    result = rt_mq_init(&g_eh.wlan_event_queue, "eh-event", g_eh.wlan_event_pool,
                        sizeof(struct eh_wlan_event), sizeof(g_eh.wlan_event_pool), RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        g_eh_initializing = RT_FALSE;
        return result;
    }

    result = esp_hosted_transport_init(&transport_callbacks, &g_eh);
    if (result != RT_EOK)
    {
        LOG_E("cannot initialize %s transport: %d",
              esp_hosted_transport_name(), result);
        g_eh_initializing = RT_FALSE;
        return result;
    }
    result = esp_hosted_rpc_init(&rpc_callbacks, &g_eh);
    if (result != RT_EOK)
    {
        LOG_E("cannot initialize RPC protocol: %d", result);
        g_eh_initializing = RT_FALSE;
        return result;
    }
    esp_hosted_rpc_api_init(&api_callbacks, &g_eh);
    esp_hosted_control_init(&control_callbacks, &g_eh);

    result = rt_wlan_dev_register(&g_eh.ap, RT_WLAN_DEVICE_AP_NAME,
                                  &g_eh_wlan_ops, RT_WLAN_FLAG_AP_ONLY, &g_eh);
    if (result != RT_EOK)
    {
        LOG_E("cannot register AP WLAN device: %d", result);
        g_eh_initializing = RT_FALSE;
        return result;
    }
    result = rt_wlan_dev_register(&g_eh.sta, RT_WLAN_DEVICE_STA_NAME,
                                  &g_eh_wlan_ops, RT_WLAN_FLAG_STA_ONLY, &g_eh);
    if (result != RT_EOK)
    {
        LOG_E("cannot register station WLAN device: %d", result);
        g_eh_initializing = RT_FALSE;
        return result;
    }

#ifdef ESP_HOSTED_BLE
    result = esp_hosted_hci_init();
    if (result != RT_EOK)
    {
        LOG_E("cannot register Bluetooth HCI device: %d", result);
        g_eh_initializing = RT_FALSE;
        return result;
    }
#endif

    g_eh.event_thread = rt_thread_create("esp-event", eh_wlan_event_thread, &g_eh,
                                         ESP_HOSTED_EVENT_THREAD_STACK_SIZE,
                                         ESP_HOSTED_THREAD_PRIORITY, 20);
    if (!g_eh.event_thread)
    {
        g_eh_initializing = RT_FALSE;
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(g_eh.event_thread);
    if (result != RT_EOK)
    {
        g_eh_initializing = RT_FALSE;
        return result;
    }
    result = esp_hosted_transport_start();
    g_eh_initializing = RT_FALSE;
    if (result == RT_EOK)
    {
        g_eh_initialized = RT_TRUE;
    }
    return result;
}
INIT_APP_EXPORT(rt_hw_esp_hosted_init);
