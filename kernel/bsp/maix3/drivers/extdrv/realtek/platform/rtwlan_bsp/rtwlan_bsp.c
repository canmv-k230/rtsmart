#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtthread.h"
#include "rtdevice.h"
#include "drivers/sdio.h"
#include "card.h"
#include "wifi/wifi_conf.h"
#include "wifi/wifi_util.h"
#include "net_stack_intf.h"
#include "customer_rtos_service.h"

/* Realtek headers use DBG_INFO(...) as a vendor logger. Keep that macro
 * separate while rtdbg.h defines RT-Thread's numeric log-level constants. */
#pragma push_macro("DBG_INFO")
#undef DBG_INFO
#define DBG_TAG "realtek.wifi"
#define DBG_LVL 2
#include <rtdbg.h>
#pragma pop_macro("DBG_INFO")

static rt_int32_t realtek_probe(struct rt_mmcsd_card* card);
static rt_err_t wlan_get_mac(struct rt_wlan_device* wlan, rt_uint8_t mac[]);
static void wlan_log_tx_power(const char* ifname);

/* Settle time between stopping and restarting the chip when its efuse autoload
 * has to be retried. */
#ifndef REALTEK_AUTOLOAD_RETRY_DELAY_MS
#define REALTEK_AUTOLOAD_RETRY_DELAY_MS 100
#endif
#ifndef REALTEK_COUNTRY_CODE
#define REALTEK_COUNTRY_CODE "CN"
#endif
/* Largest received frame staged per interface: a full Ethernet frame with room
 * for a VLAN tag, rounded up to a cache line. */
#ifndef REALTEK_RX_FRAME_MAX
#define REALTEK_RX_FRAME_MAX 1600
#endif

/* Full fallback addresses embedded in the prebuilt vendor libraries.  The
 * shared 00:e0:4c prefix is also a valid Realtek OUI, so it is not enough to
 * identify an efuse autoload failure. */
#if defined(REALTEK_SDIO_RTL8189FTV)
static const rt_uint8_t realtek_default_mac[ETH_ALEN] =
    { 0x00, 0xe0, 0x4c, 0xb7, 0x43, 0x36 };
#elif defined(REALTEK_SDIO_RTL8733BS)
static const rt_uint8_t realtek_default_mac[ETH_ALEN] =
    { 0x00, 0xe0, 0x4c, 0x87, 0x00, 0x00 };
#endif

struct sdio_func* wifi_sdio_func;
struct rt_sdio_function* rtt_sdio_func;
static struct rt_wlan_device wlan_sta, wlan_ap;
static rt_bool_t wlan_sta_registered;
static rt_bool_t wlan_ap_registered;

void Set_WLAN_Power_On(void)
{
}

void Set_WLAN_Power_Off(void)
{
}

static rt_int32_t realtek_remove(struct rt_mmcsd_card* card)
{
    rt_int32_t ret = RT_EOK;
    rt_int32_t err;

    (void)card;

    if (wifi_sdio_func != NULL) {
        err = wifi_off();
        if (err < 0)
            return -RT_EIO;
    }

    if (wlan_ap_registered) {
        if (wlan_ap.mode != RT_WLAN_NONE)
            rt_wlan_set_mode(wlan_ap.device.parent.name, RT_WLAN_NONE);
        rt_wlan_dev_unregister(&wlan_ap);
        wlan_ap_registered = RT_FALSE;
    }

    if (wlan_sta_registered) {
        if (wlan_sta.mode != RT_WLAN_NONE)
            rt_wlan_set_mode(wlan_sta.device.parent.name, RT_WLAN_NONE);
        rt_wlan_dev_unregister(&wlan_sta);
        wlan_sta_registered = RT_FALSE;
    }

    if (rtt_sdio_func != NULL) {
        sdio_set_drvdata(rtt_sdio_func, NULL);
        err = sdio_disable_func(rtt_sdio_func);
        if (err != RT_EOK && ret == RT_EOK)
            ret = err;
    }

    rt_free(wifi_sdio_func);
    wifi_sdio_func = NULL;
    rtt_sdio_func = NULL;
    Set_WLAN_Power_Off();

    return ret;
}

#define PRODUCT_RTL8189FTV (0xf179)
#define PRODUCT_RTL8733BS (0xb733)

static struct rt_sdio_device_id realtek_id[] = {
#if defined (REALTEK_SDIO_RTL8189FTV)
    { SDIO_ANY_FUNC_ID, 0x024c, PRODUCT_RTL8189FTV },
#elif defined (REALTEK_SDIO_RTL8733BS)
    { SDIO_ANY_FUNC_ID, 0x024c, PRODUCT_RTL8733BS },
#else
    ERROR
#endif
};

static struct rt_sdio_driver realtek_drv = {
    "realtek-wifi",
    realtek_probe,
    realtek_remove,
    realtek_id,
};

int realtek_init(void)
{
    rt_int32_t ret;

    Set_WLAN_Power_On();

    ret = sdio_register_driver(&realtek_drv);
    if (ret != RT_EOK && ret != -RT_EEMPTY) {
        LOG_E("SDIO driver registration failed: %d", ret);
        Set_WLAN_Power_Off();
        return ret;
    }
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(realtek_init);

void wlan_event_indication(rtw_event_indicate_t event, char* buf, int buf_len)
{
    if (event == WIFI_EVENT_DISCONNECT) {
        rt_wlan_dev_indicate_event_handle(&wlan_sta, RT_WLAN_DEV_EVT_DISCONNECT, NULL);
    } else if (event == WIFI_EVENT_STA_ASSOC || event == WIFI_EVENT_STA_DISASSOC) {
        struct rt_wlan_buff buff;
        struct rt_wlan_info wlan_info;
        memset(&wlan_info, 0, sizeof(wlan_info));
        buff.data = &wlan_info;
        buff.len = sizeof(struct rt_wlan_info);
        if (event == WIFI_EVENT_STA_ASSOC) {
            if (buf == NULL || buf_len < 10 + sizeof(wlan_info.bssid))
                return;
            memcpy(wlan_info.bssid, buf + 10, sizeof(wlan_info.bssid));
            rt_wlan_dev_indicate_event_handle(&wlan_ap, RT_WLAN_DEV_EVT_AP_ASSOCIATED, &buff);
        } else {
            if (buf == NULL || buf_len < sizeof(wlan_info.bssid))
                return;
            memcpy(wlan_info.bssid, buf, sizeof(wlan_info.bssid));
            rt_wlan_dev_indicate_event_handle(&wlan_ap, RT_WLAN_DEV_EVT_AP_DISASSOCIATED, &buff);
        }
    }
}

/* One staging buffer per interface instead of an allocation per frame.  Each
 * index is fed by its own library receive path and rt_wlan_dev_report_data()
 * has copied the frame out before this returns, so the buffer is free again by
 * the next call for that index.  At line rate this was a malloc/free pair for
 * every packet. */
static rt_uint8_t ethernetif_rx_frame[2][REALTEK_RX_FRAME_MAX]
    __attribute__((aligned(4)));

void ethernetif_recv(int idx, int total_len)
{
    struct eth_drv_sg sg_list;

    if ((idx != 0 && idx != 1) || total_len <= 0 || !rltk_wlan_running(idx))
        return;

    if (total_len > REALTEK_RX_FRAME_MAX) {
        /* Consume it anyway so the library does not keep re-offering it. */
        LOG_W("dropping %d byte frame on wlan%d, limit is %d",
              total_len, idx, REALTEK_RX_FRAME_MAX);
        total_len = REALTEK_RX_FRAME_MAX;
        sg_list.buf = (uintptr_t)ethernetif_rx_frame[idx];
        sg_list.len = total_len;
        rltk_wlan_recv(idx, &sg_list, 1);
        return;
    }

    sg_list.buf = (uintptr_t)ethernetif_rx_frame[idx];
    sg_list.len = total_len;

    rltk_wlan_recv(idx, &sg_list, 1);
    rt_wlan_dev_report_data(idx == 0 ? &wlan_sta : &wlan_ap,
                            (void*)sg_list.buf, total_len);
}

/* Nothing to do: the chip is powered, its firmware downloaded and both
 * interfaces created in realtek_probe() before either device is registered. */
static rt_err_t wlan_init(struct rt_wlan_device* wlan)
{
    (void)wlan;
    return RT_EOK;
}

/* The station and AP roles are separate rt_wlan devices here, each already bound
 * to its own library interface index, so there is no mode to switch. */
static rt_err_t wlan_mode(struct rt_wlan_device* wlan, rt_wlan_mode_t mode)
{
    (void)wlan;
    (void)mode;
    return RT_EOK;
}

static rtw_result_t scan_result_handler(rtw_scan_handler_result_t* malloced_scan_result)
{
    struct rt_wlan_buff buff;
    struct rt_wlan_info wlan_info;
    struct rt_wlan_device* wlan = malloced_scan_result->user_data;

    if (malloced_scan_result->scan_complete == RTW_TRUE) {
        rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_SCAN_DONE, NULL);
        return RTW_SUCCESS;
    }
    rtw_scan_result_t* record = &malloced_scan_result->ap_details;
    /* rtw_security_t and rtw_802_11_band_t are numerically identical to their
     * rt_wlan counterparts, so both carry straight across. */
    wlan_info.security = record->security;
    wlan_info.band = record->band;
    /* rtw_scan_result_t carries no rate, so leave it unreported rather than
     * inventing one; this is why the Mbps column of "wifi scan" reads 0. */
    wlan_info.datarate = 0;
    wlan_info.channel = record->channel;
    wlan_info.rssi = record->signal_strength;
    wlan_info.ssid.len = record->SSID.len;
    memcpy(wlan_info.ssid.val, record->SSID.val, sizeof(wlan_info.ssid.val));
    memcpy(wlan_info.bssid, record->BSSID.octet, sizeof(wlan_info.bssid));
    wlan_info.hidden = 0;    /* Not reported by the library either. */
    buff.data = &wlan_info;
    buff.len = sizeof(struct rt_wlan_info);
    rt_wlan_dev_indicate_event_handle(wlan, RT_WLAN_DEV_EVT_SCAN_REPORT, &buff);

    return RTW_SUCCESS;
}

static rt_err_t wlan_scan(struct rt_wlan_device* wlan, struct rt_scan_info* scan_info)
{
    int ret;

    ret = wifi_scan_networks(scan_result_handler, wlan);

    return ret ? -RT_ERROR : 0;
}

static rt_err_t wlan_join(struct rt_wlan_device* wlan, struct rt_sta_info* sta_info)
{
    int ret;

    if (wifi_is_up(RTW_AP_INTERFACE)) {
        wifi_off();
        rt_thread_mdelay(20);
        rt_wlan_dev_indicate_event_handle(&wlan_ap, RT_WLAN_DEV_EVT_AP_STOP, NULL);
        if (wifi_on(RTW_MODE_STA) < 0) {
            LOG_E("STA interface restart failed");
            ret = -RT_EIO;
            goto out;
        }
    }

    ret = wifi_connect(sta_info->ssid.val, sta_info->security, sta_info->key.val,
        sta_info->ssid.len, sta_info->key.len, 0, NULL);
    if (ret == RTW_SUCCESS)
        wlan_log_tx_power(WLAN0_NAME);
out:
    rt_wlan_dev_indicate_event_handle(&wlan_sta, ret ? RT_WLAN_DEV_EVT_CONNECT_FAIL :
        RT_WLAN_DEV_EVT_CONNECT, NULL);

    return ret;
}

static rt_err_t wlan_softap(struct rt_wlan_device* wlan, struct rt_ap_info* ap_info)
{
    int ret;
    uint8_t mac[ETH_ALEN];
    struct rt_wlan_buff buff = {NULL, 0};

    if (!wifi_is_up(RTW_STA_INTERFACE)) {
        LOG_W("STA interface is not running");
        ret = -RT_EIO;
        goto out;
    }

    if (!wifi_is_up(RTW_AP_INTERFACE)) {
        /* WLAN0 is kept for STA; add WLAN1 without restarting the driver. */
        if (wifi_on_coAP(RTW_MODE_STA_AP) < 0) {
            LOG_E("AP interface start failed");
            ret = -RT_EIO;
            goto out;
        }
    }

    ret = wifi_start_ap(ap_info->ssid.val, ap_info->security, ap_info->key.val,
                        ap_info->ssid.len, ap_info->key.len, ap_info->channel);
    if (ret == RTW_SUCCESS)
        wlan_log_tx_power(WLAN1_NAME);

    if (ret == RTW_SUCCESS && wlan_get_mac(&wlan_ap, mac) == RT_EOK) {
        buff.data = mac;
        buff.len = ETH_ALEN;
    }

out:
    rt_wlan_dev_indicate_event_handle(&wlan_ap,
        ret == RTW_SUCCESS ? RT_WLAN_DEV_EVT_AP_START : RT_WLAN_DEV_EVT_AP_STOP,
        buff.data != NULL ? &buff : NULL);

    return ret;
}

static rt_err_t wlan_disconnect(struct rt_wlan_device* wlan)
{
    int ret;

    ret = wifi_disconnect();
    if (ret == 0)
        rt_wlan_dev_indicate_event_handle(&wlan_sta, RT_WLAN_DEV_EVT_DISCONNECT, NULL);

    return ret;
}

static rt_err_t wlan_ap_stop(struct rt_wlan_device* wlan)
{
    if (wifi_is_up(RTW_AP_INTERFACE)) {
        if (wifi_off_coAP() < 0) {
            LOG_E("AP interface stop failed");
            return -RT_EIO;
        }
    }

    rt_wlan_dev_indicate_event_handle(&wlan_ap, RT_WLAN_DEV_EVT_AP_STOP, NULL);
    return RT_EOK;
}

static rt_err_t wlan_ap_deauth(struct rt_wlan_device* wlan, rt_uint8_t mac[])
{
    if (wlan != &wlan_ap || mac == NULL)
        return -RT_EINVAL;
    if (!wifi_is_up(RTW_AP_INTERFACE))
        return -RT_EIO;

    return wext_del_station(WLAN1_NAME, mac) < 0 ? -RT_EIO : RT_EOK;
}

static rt_err_t wlan_scan_stop(struct rt_wlan_device* wlan)
{
    (void)wlan;
    /* The library exposes no scan-abort entry point. */
    return -RT_ENOSYS;
}

static int wlan_get_rssi(struct rt_wlan_device* wlan)
{
    int32_t rssi;

    wifi_get_rssi(&rssi);

    return rssi;
}

static void wlan_log_tx_power(const char* ifname)
{
    uint8_t poweridx[20];
    uint8_t min[3] = {UINT8_MAX, UINT8_MAX, UINT8_MAX};
    uint8_t max[3] = {0, 0, 0};
    static const uint8_t first[3] = {0, 4, 12};
    static const uint8_t end[3] = {4, 12, 20};
    int group;
    int i;

    if (wext_get_tx_power(ifname, poweridx) < 0) {
        LOG_W("RF %s: unable to read TX power indices", ifname);
        return;
    }

    for (group = 0; group < 3; group++) {
        for (i = first[group]; i < end[group]; i++) {
            if (poweridx[i] < min[group])
                min[group] = poweridx[i];
            if (poweridx[i] > max[group])
                max[group] = poweridx[i];
        }
    }

    LOG_I("RF %s: TX scale 100%%, calibrated indices CCK %u-%u, OFDM %u-%u, MCS %u-%u",
          ifname, min[0], max[0], min[1], max[1], min[2], max[2]);
}

/* The prebuilt library has LPS compiled in (rtw_pm_set_lps and friends are
 * defined in it), so power save is live whatever this project's autoconf.h says.
 * Dozing between beacons costs tens of milliseconds per burst, so probe turns it
 * off and this op is the only way back on. */
static int wlan_powersave_level;

static rt_err_t wlan_set_powersave(struct rt_wlan_device* wlan, int level)
{
    int ret;

    (void)wlan;
    if (level > 0)
        ret = wifi_enable_powersave();
    else
        ret = wifi_disable_powersave();
    if (ret < 0)
        return -RT_EIO;

    wlan_powersave_level = level > 0 ? level : 0;
    return RT_EOK;
}

static int wlan_get_powersave(struct rt_wlan_device* wlan)
{
    (void)wlan;
    return wlan_powersave_level;
}

/* Not wired up on purpose.  wifi_set_promisc() delivers captured frames through
 * a callback rather than the normal receive path, so passing no callback would
 * either fault inside the library or enable a capture that goes nowhere - worse
 * than refusing.  Mapping it properly means forwarding that callback into
 * rt_wlan, and the two disagree on the frame format, so leave it unimplemented
 * until something needs it. */
static rt_err_t wlan_cfg_promisc(struct rt_wlan_device* wlan, rt_bool_t start)
{
    (void)wlan;
    (void)start;
    return -RT_ENOSYS;
}

static rt_err_t wlan_cfg_filter(struct rt_wlan_device* wlan, struct rt_wlan_filter* filter)
{
    (void)wlan;
    (void)filter;
    return -RT_ENOSYS;
}

static rt_err_t wlan_cfg_mgnt_filter(struct rt_wlan_device* wlan, rt_bool_t start)
{
    (void)wlan;
    (void)start;
    return -RT_ENOSYS;
}

static const char* wlan_ifname(struct rt_wlan_device* wlan)
{
    if (wlan == &wlan_sta)
        return WLAN0_NAME;
    if (wlan == &wlan_ap)
        return WLAN1_NAME;
    return NULL;
}

static rt_err_t wlan_set_channel(struct rt_wlan_device* wlan, int channel)
{
    const char* ifname = wlan_ifname(wlan);

    if (ifname == NULL || channel <= 0 || channel > UINT8_MAX)
        return -RT_EINVAL;

    return wext_set_channel(ifname, (uint8_t)channel) < 0 ?
           -RT_EIO : RT_EOK;
}

static int wlan_get_channel(struct rt_wlan_device* wlan)
{
    const char* ifname = wlan_ifname(wlan);
    uint8_t channel = 0;

    if (ifname == NULL)
        return -RT_EINVAL;
    if (wext_get_channel(ifname, &channel) < 0)
        return -RT_EIO;

    return channel;
}

/* The library takes a two-character country string, which does not map onto
 * RT-Thread's ~250-entry rt_country_code_t without a translation table nobody
 * needs yet.  The domain is set once at probe from REALTEK_COUNTRY_CODE. */
static rt_err_t wlan_set_country(struct rt_wlan_device* wlan, rt_country_code_t country_code)
{
    (void)wlan;
    (void)country_code;
    return -RT_ENOSYS;
}

static rt_country_code_t wlan_get_country(struct rt_wlan_device* wlan)
{
    (void)wlan;
    return RT_COUNTRY_UNKNOWN;
}

/* Deliberately unsupported.  The SDK's wext_set_mac_address() is inside an
 * "#if 0" block, and the private command it would issue ("write_mac") lands on
 * the library's EX_WRITE_MAC handler, which programs the address into efuse -
 * one-time-programmable memory.  A routine rt_wlan_set_mac() call must not be
 * able to burn a fuse, so refuse instead.  The address comes from efuse at
 * power-on; see realtek_efuse_autoload_failed(). */
static rt_err_t wlan_set_mac(struct rt_wlan_device* wlan, rt_uint8_t mac[])
{
    (void)wlan;
    (void)mac;
    return -RT_ENOSYS;
}

static rt_err_t wlan_get_mac(struct rt_wlan_device* wlan, rt_uint8_t mac[])
{
    char addr[32];

    if (mac == NULL || wifi_get_mac_address(addr) != RTW_SUCCESS)
        return -RT_EIO;

    if (sscanf(addr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", mac, mac + 1,
               mac + 2, mac + 3, mac + 4, mac + 5) != ETH_ALEN)
        return -RT_EIO;

    if (wlan == &wlan_ap)
        mac[5] = mac[5] + 1;

    return RT_EOK;
}

static int wlan_recv(struct rt_wlan_device* wlan, void* buff, int len)
{
    (void)wlan;
    (void)buff;
    (void)len;

    return -RT_ENOSYS;
}

static int wlan_send(struct rt_wlan_device* wlan, void* buff, int len)
{
    struct eth_drv_sg sg_list;
    int idx;

    if (wlan == &wlan_sta)
        idx = 0;
    else if (wlan == &wlan_ap)
        idx = 1;
    else
        return -RT_EINVAL;

    if (buff == NULL || len <= 0)
        return -RT_EINVAL;
    if (!rltk_wlan_running(idx))
        return -RT_EIO;

    sg_list.buf = (uintptr_t)buff;
    sg_list.len = len;

    return rltk_wlan_send(idx, &sg_list, 1, len) == 0 ? RT_EOK : -RT_EIO;
}

static int wlan_send_raw_frame(struct rt_wlan_device* wlan, void* buff, int len)
{
    (void)wlan;
    (void)buff;
    (void)len;
    return -RT_ENOSYS;
}

static const struct rt_wlan_dev_ops ops = {
    .wlan_init = wlan_init,
    .wlan_mode = wlan_mode,
    .wlan_scan = wlan_scan,
    .wlan_join = wlan_join,
    .wlan_softap = wlan_softap,
    .wlan_disconnect = wlan_disconnect,
    .wlan_ap_stop = wlan_ap_stop,
    .wlan_ap_deauth = wlan_ap_deauth,
    .wlan_scan_stop = wlan_scan_stop,
    .wlan_get_rssi = wlan_get_rssi,
    .wlan_set_powersave = wlan_set_powersave,
    .wlan_get_powersave = wlan_get_powersave,
    .wlan_cfg_promisc = wlan_cfg_promisc,
    .wlan_cfg_filter = wlan_cfg_filter,
    .wlan_cfg_mgnt_filter = wlan_cfg_mgnt_filter,
    .wlan_set_channel = wlan_set_channel,
    .wlan_get_channel = wlan_get_channel,
    .wlan_set_country = wlan_set_country,
    .wlan_get_country = wlan_get_country,
    .wlan_set_mac = wlan_set_mac,
    .wlan_get_mac = wlan_get_mac,
    .wlan_recv = wlan_recv,
    .wlan_send = wlan_send,
    .wlan_send_raw_frame = wlan_send_raw_frame,
};

/* The chip loads its efuse into shadow registers during its own power-on
 * sequence, and that sequence only re-runs once the supply has actually dropped:
 * a warm reset, or a brief power cycle, can leave it skipped.  The driver then
 * substitutes defaults for the MAC, the crystal trim and the TX power tables
 * without reporting an error, after which the radio receives nothing at all and
 * a scan returns zero networks.
 *
 * The substituted MAC is the only part of that visible from here. */
static rt_bool_t realtek_efuse_autoload_failed(void)
{
    rt_uint8_t mac[ETH_ALEN] = {0};

    if (wlan_get_mac(&wlan_sta, mac) != RT_EOK)
        return RT_FALSE;    /* Cannot tell; assume the chip is fine. */

    return rt_memcmp(mac, realtek_default_mac,
                     sizeof(realtek_default_mac)) == 0;
}

/* Repeat the power-on sequence, which is usually enough to pick the efuse up.
 * Returns an error only when the chip could not be brought back up at all; a
 * radio still running on default calibration is reported and tolerated so the
 * interface at least registers. */
static rt_err_t realtek_recover_efuse_autoload(const char *model_name)
{
    if (!realtek_efuse_autoload_failed())
        return RT_EOK;

    LOG_W("%s efuse autoload did not run; retrying initialization", model_name);
    if (wifi_off() < 0) {
        LOG_E("%s could not be stopped for a retry", model_name);
        /* Vendor threads may still reference the SDIO function. Tell the core
         * to retain the card instead of freeing it after this probe fails. */
        return -RT_EBUSY;
    }
    rt_thread_mdelay(REALTEK_AUTOLOAD_RETRY_DELAY_MS);
    if (wifi_on(RTW_MODE_STA) < 0) {
        LOG_E("%s re-initialization failed", model_name);
        return -RT_EIO;
    }

    if (realtek_efuse_autoload_failed()) {
        /* Nothing further can be done from here: the part needs its reset or
         * power-enable line driven, and this board wires neither
         * (BSP_WIFI_SDIO_REG_ON_PIN is -1 and Set_WLAN_Power_On/Off() are
         * empty).  Say plainly why the radio will not hear anything. */
        LOG_E("%s is running on default calibration - scans will find nothing. "
              "A full power-off is needed, or the module's reset line has to be "
              "wired up and configured via BSP_WIFI_SDIO_REG_ON_PIN.",
              model_name);
    } else {
        LOG_I("%s efuse autoload recovered on retry", model_name);
    }
    return RT_EOK;
}

/* Establish the regulatory domain explicitly rather than inheriting whatever
 * default the driver picked.  The SDK offers a _WEAK wifi_set_country_code()
 * hook for this, but wifi_conf.h declares it weak, so an override defined here
 * would stay weak too and the linker would choose arbitrarily between the two;
 * call this from probe instead. */
static void realtek_apply_country(void)
{
    static char country_code[] = REALTEK_COUNTRY_CODE;
    uint8_t plan = 0;

    if (wext_set_country(WLAN0_NAME, (u8 *)country_code) < 0) {
        LOG_W("failed to apply country code %s", country_code);
        return;
    }
    if (wifi_get_channel_plan(&plan) == 0)
        LOG_I("country %s applied, channel plan 0x%02x", country_code, plan);
    else
        LOG_I("country %s applied", country_code);
}

static rt_int32_t realtek_probe(struct rt_mmcsd_card* card)
{
    const char *model_name;
    rt_int32_t ret;

    if (card == NULL || card->sdio_function[0] == NULL ||
        card->sdio_function[1] == NULL)
        return -RT_EINVAL;
    if (wifi_sdio_func != NULL || rtt_sdio_func != NULL)
        return -RT_EBUSY;

    wifi_sdio_func = rt_calloc(1, sizeof(struct sdio_func));
    if (wifi_sdio_func == NULL)
        return -RT_ENOMEM;

    rtt_sdio_func = card->sdio_function[1];
    sdio_set_drvdata(rtt_sdio_func, wifi_sdio_func);

    ret = sdio_enable_func(rtt_sdio_func);
    if (ret != RT_EOK)
        goto fail;

    ret = sdio_set_block_size(rtt_sdio_func, 512);
    if (ret != RT_EOK)
        goto fail_disable_sdio;

    wifi_sdio_func->max_blksize = rtt_sdio_func->max_blk_size;
    wifi_sdio_func->cur_blksize = rtt_sdio_func->cur_blk_size;
    if (!wifi_sdio_func->max_blksize)
    {
        wifi_sdio_func->max_blksize = wifi_sdio_func->cur_blksize;
        LOG_W("function CIS reported zero max block size; using configured size %u",
              wifi_sdio_func->max_blksize);
    }
    wifi_sdio_func->enable_timeout = rtt_sdio_func->enable_timeout_val;
    wifi_sdio_func->num = rtt_sdio_func->num;
    wifi_sdio_func->vendor = rtt_sdio_func->manufacturer;
    wifi_sdio_func->device = rtt_sdio_func->product;
    wifi_sdio_func->num_info = 0;
    model_name = PRODUCT_RTL8189FTV == wifi_sdio_func->device ?
                 "rtl8189ftv" : "rtl8733bs";

    if (wifi_on(RTW_MODE_STA) < 0) {
        LOG_E("%s initialization failed", model_name);
        ret = -RT_EIO;
        goto fail_disable_sdio;
    }

    ret = realtek_recover_efuse_autoload(model_name);
    if (ret == -RT_EBUSY)
        return ret;
    if (ret != RT_EOK)
        goto fail_disable_sdio;

    realtek_apply_country();

    /* Start with power save off: the library dozes between beacons otherwise,
     * which costs far more throughput than it saves here.  Applications can ask
     * for it back through rt_wlan_set_powersave(). */
    if (wifi_disable_powersave() < 0)
        LOG_W("%s could not disable power save", model_name);

    ret = rt_wlan_dev_register_auto(&wlan_sta, model_name, RT_WLAN_STATION,
                                    RT_WLAN_TRANSPORT_SDIO, &ops, NULL);
    if (ret != RT_EOK)
        goto fail_wifi;
    wlan_sta_registered = RT_TRUE;

    ret = rt_wlan_dev_register_auto(&wlan_ap, model_name, RT_WLAN_AP,
                                    RT_WLAN_TRANSPORT_SDIO, &ops, NULL);
    if (ret != RT_EOK)
        goto fail_wifi;
    wlan_ap_registered = RT_TRUE;

    ret = rt_wlan_set_mode(wlan_sta.device.parent.name, RT_WLAN_STATION);
    if (ret != RT_EOK)
        goto fail_wifi;

    ret = rt_wlan_set_mode(wlan_ap.device.parent.name, RT_WLAN_AP);
    if (ret != RT_EOK)
        goto fail_wifi;

    return RT_EOK;

fail_wifi:
    if (wifi_off() < 0) {
        LOG_E("cleanup failed after probe error %d", ret);
        /* Keep the backing card and function alive while the vendor driver is
         * still running; sdio_register_card() recognizes this condition. */
        return -RT_EBUSY;
    }
    if (wlan_ap_registered) {
        if (wlan_ap.mode != RT_WLAN_NONE)
            rt_wlan_set_mode(wlan_ap.device.parent.name, RT_WLAN_NONE);
        rt_wlan_dev_unregister(&wlan_ap);
        wlan_ap_registered = RT_FALSE;
    }
    if (wlan_sta_registered) {
        if (wlan_sta.mode != RT_WLAN_NONE)
            rt_wlan_set_mode(wlan_sta.device.parent.name, RT_WLAN_NONE);
        rt_wlan_dev_unregister(&wlan_sta);
        wlan_sta_registered = RT_FALSE;
    }
fail_disable_sdio:
    sdio_disable_func(rtt_sdio_func);
fail:
    sdio_set_drvdata(rtt_sdio_func, NULL);
    rt_free(wifi_sdio_func);
    wifi_sdio_func = NULL;
    rtt_sdio_func = NULL;
    return ret;
}
