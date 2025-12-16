#include <stdbool.h>
#include <stdio.h>

#include "rtdef.h"
#include <rthw.h>
#include <rtthread.h>

#include <lwp.h>
#include <lwp_user_mm.h>

#include "sys/ioctl.h"
#include <netdb.h>
#include <netdev.h>
#include <netdev_ipaddr.h>

#include "dfs_file.h"

#ifdef CONFIG_ENABLE_NETWORK_RT_WLAN
#include <wlan_mgnt.h>
#endif // CONFIG_ENABLE_NETWORK_RT_WLAN

#define DBG_TAG "NetMgmtDev"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifdef CONFIG_ENABLE_NETWORK_RT_WLAN
// wlan basic
#define IOCTRL_WM_GET_AUTO_RECONNECT _IOWR('N', 0x00, void*)
#define IOCTRL_WM_SET_AUTO_RECONNECT _IOWR('N', 0x01, void*)

// wlan sta
#define IOCTRL_WM_STA_CONNECT      _IOWR('N', 0x10, void*)
#define IOCTRL_WM_STA_DISCONNECT   _IOWR('N', 0x11, void*)
#define IOCTRL_WM_STA_IS_CONNECTED _IOWR('N', 0x12, void*)
#define IOCTRL_WM_STA_GET_MAC      _IOWR('N', 0x13, void*)
#define IOCTRL_WM_STA_SET_MAC      _IOWR('N', 0x14, void*)
#define IOCTRL_WM_STA_GET_AP_INFO  _IOWR('N', 0x15, void*)
#define IOCTRL_WM_STA_GET_RSSI     _IOWR('N', 0x16, void*)
#define IOCTRL_WM_STA_SCAN         _IOWR('N', 0x17, void*)

// wlan ap
#define IOCTRL_WM_AP_START        _IOWR('N', 0x20, void*)
#define IOCTRL_WM_AP_STOP         _IOWR('N', 0x21, void*)
#define IOCTRL_WM_AP_IS_ACTIVE    _IOWR('N', 0x22, void*)
#define IOCTRL_WM_AP_GET_INFO     _IOWR('N', 0x23, void*)
#define IOCTRL_WM_AP_GET_STA_INFO _IOWR('N', 0x24, void*)
#define IOCTRL_WM_AP_DEAUTH_STA   _IOWR('N', 0x25, void*)
#define IOCTRL_WM_AP_GET_COUNTRY  _IOWR('N', 0x26, void*)
#define IOCTRL_WM_AP_SET_COUNTRY  _IOWR('N', 0x27, void*)
#endif // CONFIG_ENABLE_NETWORK_RT_WLAN

#ifdef CONFIG_ENABLE_NETWORK_RT_LAN_OVER_USB
// lan
#define IOCTRL_LAN_GET_ISCONNECTED _IOWR('N', 0x80, void*)
#define IOCTRL_LAN_GET_LINK_STATUS _IOWR('N', 0x81, void*)
#define IOCTRL_LAN_GET_MAC         _IOWR('N', 0x82, void*)
#define IOCTRL_LAN_SET_MAC         _IOWR('N', 0x83, void*)
#endif // CONFIG_ENABLE_NETWORK_RT_LAN_OVER_USB

// network util
#define IOCTRL_NET_IFCONFIG        _IOWR('N', 0x100, void*)
#define IOCTRL_NET_SET_DEV_DEFAULT _IOWR('N', 0x101, void*)
#define IOCTRL_NET_GET_DEV_DEFAULT _IOWR('N', 0x102, void*)
#define IOCTRL_NET_GET_DEV_LIST    _IOWR('N', 0x103, void*)
#define IOCTRL_NET_PROBE           _IOWR('N', 0x104, void*)

struct rt_net_mgmt_device {
    struct rt_device device;
    // struct rt_mutex lock;
};
static struct rt_net_mgmt_device net_mgmt_device;
static volatile int g_auto_reconnect = 1;

enum rt_netif_t {
    RT_NET_DEV_WLAN_STA    = 0,
    RT_NET_DEV_WLAN_AP     = 1,
    RT_NET_DEV_USB_RTL8152 = 2,
    RT_NET_DEV_USB_ECM     = 3,
};

/* WLAN CMD HANDLE ***********************************************************/
#ifdef CONFIG_ENABLE_NETWORK_RT_WLAN
struct rt_wlan_connect_config {
    int use_info;
    union {
        rt_wlan_ssid_t      ssid;
        struct rt_wlan_info info;
    };
    rt_wlan_key_t key;
};

static rt_err_t _wlan_mgmt_cmd_basic_get_auto_reconnect(void* mgmt_dev, void* args)
{
    int auto_reconnect = (int)rt_wlan_get_autoreconnect_mode();

    lwp_put_to_user(args, &auto_reconnect, sizeof(int));

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_cmd_basic_set_auto_reconnect(void* mgmt_dev, void* args)
{
    int auto_reconnect = 1;

    lwp_get_from_user(&auto_reconnect, args, sizeof(int));

    rt_wlan_config_autoreconnect((rt_bool_t)(auto_reconnect & 0x01));
    g_auto_reconnect = auto_reconnect;

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_cmd_sta_connect(void* mgmt_dev, void* args)
{
    rt_err_t                      err = RT_EOK;
    struct rt_wlan_connect_config config;

    lwp_get_from_user(&config, args, sizeof(struct rt_wlan_connect_config));

    if (g_auto_reconnect) {
        rt_wlan_config_autoreconnect((rt_bool_t)(g_auto_reconnect & 0x01));
    }

    if (config.use_info) {
        err = rt_wlan_connect_adv(&config.info, (char*)config.key.val);
    } else {
        err = rt_wlan_connect((char*)config.ssid.val, (char*)config.key.val);
    }

    return err;
}

static rt_err_t _wlan_mgmt_cmd_sta_disconnect(void* mgmt_dev, void* args) { return rt_wlan_disconnect(); }

static rt_err_t _wlan_mgmt_cmd_sta_isconnected(void* mgmt_dev, void* args)
{
    int status = (int)rt_wlan_is_connected();

    if (status) {
        status = 0;

        struct rt_wlan_device* wlan_dev = (struct rt_wlan_device*)rt_device_find(RT_WLAN_DEVICE_STA_NAME);

        if (wlan_dev) {
            struct netdev* netdev = wlan_dev->netdev;
            if (netdev && netdev->ip_addr.addr) {
                status = 1;
            }
        }
    }

    lwp_put_to_user(args, &status, sizeof(int));

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_cmd_sta_get_mac(void* mgmt_dev, void* args)
{
    rt_err_t err = RT_EOK;
    uint8_t  mac[6];

    err = rt_wlan_get_mac(mac);
    lwp_put_to_user(args, mac, sizeof(mac));

    return err;
}

static rt_err_t _wlan_mgmt_cmd_sta_set_mac(void* mgmt_dev, void* args)
{
    uint8_t mac[6];

    lwp_get_from_user(&mac[0], args, sizeof(mac));

    return rt_wlan_set_mac(mac);
}

static rt_err_t _wlan_mgmt_cmd_sta_get_ap_info(void* mgmt_dev, void* args)
{
    rt_err_t            err = RT_EOK;
    struct rt_wlan_info info;

    err = rt_wlan_get_info(&info);

    lwp_put_to_user(args, &info, sizeof(struct rt_wlan_info));

    return err;
}

static rt_err_t _wlan_mgmt_cmd_sta_get_rssi(void* mgmt_dev, void* args)
{
    int rssi = rt_wlan_get_rssi();

    lwp_put_to_user(args, &rssi, sizeof(int));

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_cmd_sta_scan(void* mgmt_dev, void* args)
{
    struct rt_wlan_scan_result* result      = (struct rt_wlan_scan_result*)args;
    struct rt_wlan_scan_result* scan_result = RT_NULL;

    int32_t             result_num;
    struct rt_wlan_info info;

    lwp_get_from_user(&result_num, &result->num, sizeof(int32_t));
    lwp_get_from_user(&info, &result->info[0], sizeof(struct rt_wlan_info));

    /* clean scan result */
    rt_wlan_scan_result_clean();
    /* scan ap info */
    scan_result = rt_wlan_scan_with_info(&info);

    if (scan_result) {
        if (scan_result->num > result_num) {
            LOG_W("Scan result count(%d) bigger than user requset(%d)\n", scan_result->num, result_num);
            scan_result->num = result_num;
        }

        lwp_put_to_user(&result->num, &scan_result->num, sizeof(int32_t));

        for (int i = 0; i < scan_result->num; i++) {
            lwp_put_to_user(&result->info[i], &scan_result->info[i], sizeof(struct rt_wlan_info));
        }
        rt_wlan_scan_result_clean();

        return RT_EOK;
    }

    LOG_E("Can't scan ap\n");

    return -1;
}

static rt_err_t _wlan_mgmt_cmd_ap_start(void* mgmt_dev, void* args)
{
    rt_err_t                      err = RT_EOK;
    struct rt_wlan_connect_config config;

    lwp_get_from_user(&config, args, sizeof(struct rt_wlan_connect_config));

    if (g_auto_reconnect) {
        rt_wlan_config_autoreconnect(RT_FALSE);
    }

    if (config.use_info) {
        err = rt_wlan_start_ap_adv(&config.info, (char*)config.key.val);
    } else {
        err = rt_wlan_start_ap((char*)config.ssid.val, (char*)config.key.val);
    }

    return err;
}

static rt_err_t _wlan_mgmt_cmd_ap_stop(void* mgmt_dev, void* args) { return rt_wlan_ap_stop(); }

static rt_err_t _wlan_mgmt_cmd_ap_isactive(void* mgmt_dev, void* args)
{
    int active = (int)rt_wlan_ap_is_active();
    lwp_put_to_user(args, &active, sizeof(int));

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_cmd_ap_get_info(void* mgmt_dev, void* args)
{
    rt_err_t            err = RT_EOK;
    struct rt_wlan_info info;

    err = rt_wlan_ap_get_info(&info);

    lwp_put_to_user(args, &info, sizeof(struct rt_wlan_info));

    return err;
}

static rt_err_t _wlan_mgmt_cmd_ap_get_stations(void* mgmt_dev, void* args)
{
    struct rt_wlan_scan_result* result = (struct rt_wlan_scan_result*)args;
    int32_t                     result_num;

    int                  station_num = 0;
    struct rt_wlan_info* info        = NULL;

    station_num = rt_wlan_ap_get_sta_num();
    lwp_get_from_user(&result_num, &result->num, sizeof(int32_t));

    if (station_num > result_num) {
        LOG_W("Station count(%d) bigger than user requset(%d)\n", station_num, result_num);
        station_num = result_num;
    }

    info = malloc(sizeof(struct rt_wlan_info) * station_num);
    if (NULL == info) {
        LOG_E("No memory\n");
        return -1;
    }

    rt_wlan_ap_get_sta_info(info, station_num);

    lwp_put_to_user(&result->num, &station_num, sizeof(int32_t));
    lwp_put_to_user(result->info, info, sizeof(struct rt_wlan_info) * station_num);

    free(info);

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_cmd_ap_deauth_sta(void* mgmt_dev, void* args)
{
    uint8_t mac[6];

    lwp_get_from_user(mac, args, sizeof(mac));

    return rt_wlan_ap_deauth_sta(mac);
}

static rt_err_t _wlan_mgmt_cmd_ap_get_country(void* mgmt_dev, void* args)
{
    int country = (int)rt_wlan_ap_get_country();

    lwp_put_to_user(args, &country, sizeof(int));

    return RT_EOK;
}

static rt_err_t _wlan_mgmt_cmd_ap_set_country(void* mgmt_dev, void* args)
{
    int country = 0;

    lwp_get_from_user(&country, args, sizeof(int));

    return rt_wlan_ap_set_country((rt_country_code_t)country);
}
#endif // CONFIG_ENABLE_NETWORK_RT_WLAN

#ifdef CONFIG_ENABLE_NETWORK_RT_LAN_OVER_USB
/* LAN CMD HANDLE ************************************************************/
struct lan_status_cmd_wrap_t {
    enum rt_netif_t itf;

    int value;
};

struct lan_mac_cmd_wrap_t {
    enum rt_netif_t itf;

    uint8_t mac[6];
};

/**
 * @brief Get the network device name based on the interface enum.
 *
 * @param itf The network interface enum.
 * @return const char* The device name string, or NULL if not found.
 */
static const char* _get_netdev_name(enum rt_netif_t itf)
{
    switch (itf) {
    case RT_NET_DEV_USB_RTL8152:
        return CANMV_USB_HOST_NET_RTL8152_DEV_NAME;
    case RT_NET_DEV_USB_ECM:
        return CANMV_USB_HOST_NET_LTE_DEV_NAME;
    // Add other interface cases here in the future
    default:
        return NULL;
    }
}

static rt_err_t _lan_mgmt_cmd_get_isconnected(void* mgmt_dev, void* args)
{
    struct lan_status_cmd_wrap_t cfg;

    bool           connect_status = false;
    ip_addr_t      ip;
    rt_device_t    eth_dev  = NULL;
    struct netdev* netdev   = NULL;
    const char*    dev_name = NULL;

    // Get the interface type from the user
    if (0x00 != lwp_get_from_user_ex(&cfg, args, sizeof(struct lan_status_cmd_wrap_t))) {
        return RT_ERROR;
    }

    // Default to not connected
    cfg.value = 0;

    // Find the device name and handle
    dev_name = _get_netdev_name(cfg.itf);
    if (dev_name == NULL) {
        goto _exit;
    }
    eth_dev = rt_device_find(dev_name);
    if (eth_dev == NULL) {
        goto _exit;
    }

    // Check link status
    if (RT_EOK != rt_device_control(eth_dev, 0x1001 /* get connect status */, &connect_status) || (false == connect_status)) {
        goto _exit;
    }

    // Check for a valid IP address
    netdev = netdev_get_by_name(dev_name);
    if (netdev == NULL) {
        goto _exit;
    }
    memcpy(&ip, &netdev->ip_addr, sizeof(ip_addr_t));
    if (ip.addr == 0x00) {
        goto _exit;
    }

    // If all checks pass, it's connected
    cfg.value = 1;

_exit:
    // Return the result (0 or 1) to the user
    return lwp_put_to_user_ex(args, &cfg, sizeof(struct lan_status_cmd_wrap_t));
}

static rt_err_t _lan_mgmt_cmd_get_link_status(void* mgmt_dev, void* args)
{
    struct lan_status_cmd_wrap_t cfg;

    bool        connect_status;
    rt_device_t eth_dev  = NULL;
    const char* dev_name = NULL;

    // Get the interface type from the user
    if (0x00 != lwp_get_from_user_ex(&cfg, args, sizeof(struct lan_status_cmd_wrap_t))) {
        return RT_ERROR;
    }

    // Default to error/unknown status
    cfg.value = 0;

    dev_name = _get_netdev_name(cfg.itf);
    if (dev_name == NULL) {
        goto _exit;
    }
    eth_dev = rt_device_find(dev_name);
    if (eth_dev == NULL) {
        goto _exit;
    }

    if (RT_EOK == rt_device_control(eth_dev, 0x1001 /* get connect status */, &connect_status)) {
        if (connect_status) {
            cfg.value = 1; // Linked
        } else {
            cfg.value = 2; // Not Linked
        }
    }

_exit:
    // Return the status (0, 1, or 2) to the user
    return lwp_put_to_user_ex(args, &cfg, sizeof(struct lan_status_cmd_wrap_t));
}

static rt_err_t _lan_mgmt_cmd_get_mac(void* mgmt_dev, void* args)
{
    struct lan_mac_cmd_wrap_t cfg;

    rt_device_t eth_dev  = NULL;
    const char* dev_name = NULL;

    // Get the interface type from user space
    if (0x00 != lwp_get_from_user_ex(&cfg, args, sizeof(struct lan_mac_cmd_wrap_t))) {
        return RT_ERROR;
    }

    dev_name = _get_netdev_name(cfg.itf);
    if (dev_name == NULL) {
        return RT_ERROR;
    }
    eth_dev = rt_device_find(dev_name);
    if (eth_dev == NULL) {
        return RT_ERROR;
    }

    // Get the MAC and store it directly in our response struct
    if (RT_EOK != rt_device_control(eth_dev, 0x01 /* NIOCTL_GADDR */, &cfg.mac[0])) {
        return RT_ERROR;
    }

    // Send the struct containing the MAC back to the user
    return lwp_put_to_user_ex(args, &cfg, sizeof(struct lan_mac_cmd_wrap_t));
}

static rt_err_t _lan_mgmt_cmd_set_mac(void* mgmt_dev, void* args)
{
    struct lan_mac_cmd_wrap_t cfg;

    rt_device_t eth_dev  = NULL;
    const char* dev_name = NULL;

    // Get the struct containing interface type and new MAC from the user
    if (0x00 != lwp_get_from_user_ex(&cfg, args, sizeof(struct lan_mac_cmd_wrap_t))) {
        rt_kprintf("get mac struct failed\n");
        return RT_ERROR;
    }

    dev_name = _get_netdev_name(cfg.itf);
    if (dev_name == NULL) {
        return RT_ERROR;
    }
    eth_dev = rt_device_find(dev_name);
    if (eth_dev == NULL) {
        return RT_ERROR;
    }

    // Set the MAC address
    if (RT_EOK != rt_device_control(eth_dev, 0x1000 /* set mac */, &cfg.mac[0])) {
        return RT_ERROR;
    }

    return RT_EOK;
}
#endif // CONFIG_ENABLE_NETWORK_RT_LAN_OVER_USB

/* UTIL CMD HANDLE ***********************************************************/
static rt_err_t _net_mgmt_dev_cmd_net_ifconfig(void* mgmt_dev, void* args)
{
    struct ifconfig {
        enum rt_netif_t net_if; /* 0: sta, 1: ap, 2:... */
        uint16_t        func; /* 0: get ip info, 1: disable dhcp, set static ip, 2: enable dhcp */
        ip_addr_t       ip; /* IP address */
        ip_addr_t       gw; /* gateway */
        ip_addr_t       netmask; /* subnet mask */
        ip_addr_t       dns; /* DNS server */
    };
    struct ifconfig  ifconfig;
    struct ifconfig* out = (struct ifconfig*)args;

    struct rt_wlan_device* wlan_dev = NULL;
    struct netdev*         netdev   = NULL;

    lwp_get_from_user(&ifconfig, args, sizeof(struct ifconfig));

    if (RT_NET_DEV_WLAN_STA == ifconfig.net_if) { /* wlan station */
        wlan_dev = (struct rt_wlan_device*)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
        if (NULL == wlan_dev) {
            LOG_E("Can't find netif %s\n", RT_WLAN_DEVICE_STA_NAME);
            return -RT_ERROR;
        }
        netdev = wlan_dev->netdev;
    } else if (RT_NET_DEV_WLAN_AP == ifconfig.net_if) { /* wlan ap */
        wlan_dev = (struct rt_wlan_device*)rt_device_find(RT_WLAN_DEVICE_AP_NAME);
        if (NULL == wlan_dev) {
            LOG_E("Can't find netif %s\n", RT_WLAN_DEVICE_AP_NAME);
            return -RT_ERROR;
        }
        netdev = wlan_dev->netdev;
    }
#if defined(CONFIG_ENABLE_NETWORK_RT_LAN_OVER_USB)
    else if (RT_NET_DEV_USB_RTL8152 == ifconfig.net_if) { /* eth usb */
        /* eth rtl8152 device and netdev name is same */
        netdev = netdev_get_by_name(CANMV_USB_HOST_NET_RTL8152_DEV_NAME);
        if (NULL == netdev) {
            LOG_E("Can't find netif %s\n", CANMV_USB_HOST_NET_RTL8152_DEV_NAME);
            return -RT_ERROR;
        }
    } else if (RT_NET_DEV_USB_ECM == ifconfig.net_if) { /* lte usb */
        /* eth lte device and netdev name is same */
        netdev = netdev_get_by_name(CANMV_USB_HOST_NET_LTE_DEV_NAME);
        if (NULL == netdev) {
            LOG_E("Can't find netif %s\n", CANMV_USB_HOST_NET_LTE_DEV_NAME);
            return -RT_ERROR;
        }
    }
#endif
    else {
        LOG_E("Unsupport netif %d\n", ifconfig.net_if);
        return -RT_ERROR;
    }

    if (NULL == netdev) {
        LOG_E("Can't find netif %d\n", ifconfig.net_if);
        return -RT_ERROR;
    }

    if (0x00 == ifconfig.func) { /* get */
        lwp_put_to_user(&out->ip, &netdev->ip_addr, sizeof(ip_addr_t));
        lwp_put_to_user(&out->gw, &netdev->gw, sizeof(ip_addr_t));
        lwp_put_to_user(&out->netmask, &netdev->netmask, sizeof(ip_addr_t));
        lwp_put_to_user(&out->dns, &netdev->dns_servers[0], sizeof(ip_addr_t));
    } else if (0x01 == ifconfig.func) { /* set static ip */
        if (0x00 != netdev_dhcp_enabled(netdev, RT_FALSE)) {
            LOG_E("Set itf %s disable dhcp failed\n", netdev->name);
            return -RT_ERROR;
        }

        if (0x00 != netdev_set_ipaddr(netdev, &ifconfig.ip)) {
            LOG_E("Set itf %s ip failed\n", netdev->name);
            return -RT_ERROR;
        }
        if (0x00 != netdev_set_gw(netdev, &ifconfig.gw)) {
            LOG_E("Set itf %s gw failed\n", netdev->name);
            return -RT_ERROR;
        }
        if (0x00 != netdev_set_netmask(netdev, &ifconfig.netmask)) {
            LOG_E("Set itf %s netmask failed\n", netdev->name);
            return -RT_ERROR;
        }
        if (0x00 != netdev_set_dns_server(netdev, 1, &ifconfig.dns)) {
            LOG_E("Set itf %s dns failed\n", netdev->name);
            return -RT_ERROR;
        }
    } else if (0x02 == ifconfig.func) {
        if (0x00 != netdev_dhcp_enabled(netdev, RT_TRUE)) {
            LOG_E("Set itf %s enable dhcp failed\n", netdev->name);
            return -RT_ERROR;
        }
    }

    return RT_EOK;
}

// static rt_err_t _net_mgmt_dev_cmd_net_gethostbyname(void *mgmt_dev, void *args)
// {
//     struct result {
//         uint8_t ip[4];
//         int name_len; // max 256
//         char name[0];
//     };

//     uint64_t buffer[(sizeof(struct result) + 256) / sizeof(uint64_t) + 1];
//     struct result *request = (struct result *)args;
//     struct result *result = (struct result *)buffer;
//     struct hostent *host = NULL;

//     lwp_get_from_user(&result->name_len, &request->name_len, sizeof(int));
//     if(256 < result->name_len) {
//         LOG_E("Host name too long\n");
//         return -1;
//     }

//     lwp_get_from_user(&result->name[0], &request->name[0], result->name_len);
//     result->name[result->name_len] = '\0';
//     if(NULL == (host = gethostbyname(result->name))) {
//         LOG_W("get host failed1, %s\n", result->name);
//         return -2;
//     }

//     if(NULL == host->h_addr_list) {
//         LOG_W("get host failed2, %s\n", result->name);
//         return -2;
//     }

//     lwp_put_to_user(request->ip, host->h_addr_list[0], sizeof(request->ip));

//     return 0;
// }

static rt_err_t _net_mgmt_dev_cmd_set_default(void* mgmt_dev, void* args)
{
    char dev_name[32];

    struct netdev*         netdev   = NULL;
    struct rt_wlan_device* wlan_dev = NULL;

    lwp_get_from_user(&dev_name[0], args, sizeof(dev_name) - 1);
    dev_name[sizeof(dev_name) - 1] = '\0';

    if (0x00 == rt_strlen(dev_name)) {
        return -1;
    }

    netdev = netdev_get_by_name(dev_name);
    if (NULL == netdev) {
        LOG_E("Can't find netif %s\n", dev_name);
        return -RT_ERROR;
    }
    netdev_set_default(netdev);

    return 0;
}

static rt_err_t _net_mgmt_dev_cmd_get_default(void* mgmt_dev, void* args)
{
    extern struct netdev* netdev_default;

    char dev_name[32];

    if (NULL == netdev_default) {
        return -1;
    }

    rt_memcpy(dev_name, netdev_default->name, sizeof(netdev_default->name));

    if (sizeof(dev_name) != lwp_put_to_user(args, dev_name, sizeof(dev_name))) {
        return -2;
    }

    return 0;
}

static rt_err_t _net_mgmt_dev_cmd_get_dev_list(void* mgmt_dev, void* args)
{
#define NET_DEV_MAX_CNT 8

    rt_base_t      level;
    rt_slist_t*    node       = RT_NULL;
    struct netdev* netdev     = RT_NULL;
    int            netdev_cnt = 0;
    int            name_idx   = 0;

    char result_buffer[32 * NET_DEV_MAX_CNT], *pname = NULL; /* we max support 8 netdev */

    /* Check if netdev list exists */
    if (RT_NULL == netdev_list) {
        return -1;
    }

    if ((NULL == args) || !lwp_user_accessable(args, sizeof(result_buffer))) {
        LOG_E("user input buffer error\n");
        return -1;
    }

    name_idx = 0;
    rt_memset(result_buffer, 0, sizeof(result_buffer));

    level = rt_hw_interrupt_disable();
    for (node = &(netdev_list->list); node; node = rt_slist_next(node)) {
        netdev = rt_slist_entry(node, struct netdev, list);

        pname = &result_buffer[name_idx * 32];
        rt_memcpy(pname, netdev->name, RT_NAME_MAX);

        if ((++name_idx) > NET_DEV_MAX_CNT) {
            break;
        }
    }
    rt_hw_interrupt_enable(level);

    if (sizeof(result_buffer) != lwp_put_to_user(args, result_buffer, sizeof(result_buffer))) {
        return -1;
    }

    return 0;

#undef NET_DEV_MAX_CNT
}

static rt_err_t _net_mgmt_cmd_get_isactive(void* mgmt_dev, void* args)
{
    int         itf      = -1;
    int         isactive = 0;
    rt_device_t net_dev  = NULL;
    const char* dev_name;

    lwp_get_from_user(&itf, args, sizeof(itf));

    if (RT_NET_DEV_WLAN_STA == itf) { /* wlan station */
        net_dev = rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    } else if (RT_NET_DEV_WLAN_AP == itf) { /* wlan ap */
        net_dev = rt_device_find(RT_WLAN_DEVICE_AP_NAME);
    }
#ifdef CONFIG_ENABLE_NETWORK_RT_LAN_OVER_USB
    else if (RT_NET_DEV_USB_RTL8152 == itf) { /* eth usb */
        net_dev = rt_device_find(CANMV_USB_HOST_NET_RTL8152_DEV_NAME);
    } else if (RT_NET_DEV_USB_ECM == itf) { /* eth usb */
        net_dev = rt_device_find(CANMV_USB_HOST_NET_LTE_DEV_NAME);
    }
#endif

    isactive = (NULL == net_dev) ? 0 : 1;
    lwp_put_to_user(args, &isactive, sizeof(isactive));

    return RT_EOK;
}

struct rt_net_mgmt_device_cmd_handle {
    int cmd;
    rt_err_t (*func)(void* mgmt_dev, void* args);
};

static struct rt_net_mgmt_device_cmd_handle cmd_handles[] = {
#ifdef CONFIG_ENABLE_NETWORK_RT_WLAN
    // wlan basic
    {
        .cmd  = IOCTRL_WM_GET_AUTO_RECONNECT,
        .func = _wlan_mgmt_cmd_basic_get_auto_reconnect,
    },
    {
        .cmd  = IOCTRL_WM_SET_AUTO_RECONNECT,
        .func = _wlan_mgmt_cmd_basic_set_auto_reconnect,
    },

    // station
    {
        .cmd  = IOCTRL_WM_STA_CONNECT,
        .func = _wlan_mgmt_cmd_sta_connect,
    },
    {
        .cmd  = IOCTRL_WM_STA_DISCONNECT,
        .func = _wlan_mgmt_cmd_sta_disconnect,
    },
    {
        .cmd  = IOCTRL_WM_STA_IS_CONNECTED,
        .func = _wlan_mgmt_cmd_sta_isconnected,
    },
    {
        .cmd  = IOCTRL_WM_STA_GET_MAC,
        .func = _wlan_mgmt_cmd_sta_get_mac,
    },
    {
        .cmd  = IOCTRL_WM_STA_SET_MAC,
        .func = _wlan_mgmt_cmd_sta_set_mac,
    },
    {
        .cmd  = IOCTRL_WM_STA_GET_AP_INFO,
        .func = _wlan_mgmt_cmd_sta_get_ap_info,
    },
    {
        .cmd  = IOCTRL_WM_STA_GET_RSSI,
        .func = _wlan_mgmt_cmd_sta_get_rssi,
    },
    {
        .cmd  = IOCTRL_WM_STA_SCAN,
        .func = _wlan_mgmt_cmd_sta_scan,
    },

    // ap
    {
        .cmd  = IOCTRL_WM_AP_START,
        .func = _wlan_mgmt_cmd_ap_start,
    },
    {
        .cmd  = IOCTRL_WM_AP_STOP,
        .func = _wlan_mgmt_cmd_ap_stop,
    },
    {
        .cmd  = IOCTRL_WM_AP_IS_ACTIVE,
        .func = _wlan_mgmt_cmd_ap_isactive,
    },
    {
        .cmd  = IOCTRL_WM_AP_GET_INFO,
        .func = _wlan_mgmt_cmd_ap_get_info,
    },
    {
        .cmd  = IOCTRL_WM_AP_GET_STA_INFO,
        .func = _wlan_mgmt_cmd_ap_get_stations,
    },
    {
        .cmd  = IOCTRL_WM_AP_DEAUTH_STA,
        .func = _wlan_mgmt_cmd_ap_deauth_sta,
    },
    {
        .cmd  = IOCTRL_WM_AP_GET_COUNTRY,
        .func = _wlan_mgmt_cmd_ap_get_country,
    },
    {
        .cmd  = IOCTRL_WM_AP_SET_COUNTRY,
        .func = _wlan_mgmt_cmd_ap_set_country,
    },
#endif // CONFIG_ENABLE_NETWORK_RT_WLAN

#ifdef CONFIG_ENABLE_NETWORK_RT_LAN_OVER_USB
    // lan
    {
        .cmd  = IOCTRL_LAN_GET_ISCONNECTED,
        .func = _lan_mgmt_cmd_get_isconnected,
    },
    {
        .cmd  = IOCTRL_LAN_GET_LINK_STATUS,
        .func = _lan_mgmt_cmd_get_link_status,
    },
    {
        .cmd  = IOCTRL_LAN_GET_MAC,
        .func = _lan_mgmt_cmd_get_mac,
    },
    {
        .cmd  = IOCTRL_LAN_SET_MAC,
        .func = _lan_mgmt_cmd_set_mac,
    },
#endif // CONFIG_ENABLE_NETWORK_RT_LAN_OVER_USB

    // network
    {
        .cmd  = IOCTRL_NET_IFCONFIG,
        .func = _net_mgmt_dev_cmd_net_ifconfig,
    },
    {
        .cmd  = IOCTRL_NET_SET_DEV_DEFAULT,
        .func = _net_mgmt_dev_cmd_set_default,
    },
    {
        .cmd  = IOCTRL_NET_GET_DEV_DEFAULT,
        .func = _net_mgmt_dev_cmd_get_default,
    },
    {
        .cmd  = IOCTRL_NET_GET_DEV_LIST,
        .func = _net_mgmt_dev_cmd_get_dev_list,
    },
    {
        .cmd  = IOCTRL_NET_PROBE,
        .func = _net_mgmt_cmd_get_isactive,
    },
};

static rt_err_t _net_mgmt_dev_control(rt_device_t dev, int cmd, void* args)
{
    struct rt_net_mgmt_device* mgmt_dev     = (struct rt_net_mgmt_device*)dev;
    const size_t               handle_count = sizeof(cmd_handles) / sizeof(cmd_handles[0]);

    for (size_t i = 0; i < handle_count; i++) {
        if (cmd == cmd_handles[i].cmd) {
            return cmd_handles[i].func(mgmt_dev, args);
        }
    }

    LOG_E("Unsupport cmd 0x%x\n", cmd);

    return -RT_ERROR;
}

static rt_err_t _net_mgmt_dev_init(rt_device_t dev)
{
    (void)dev;
#ifdef CONFIG_ENABLE_NETWORK_RT_WLAN
    rt_wlan_config_autoreconnect(RT_TRUE);

    return (rt_err_t)rt_wlan_init();
#else
    return RT_EOK;
#endif
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops net_mgmt_dev_ops
    = { _net_mgmt_dev_init, RT_NULL, RT_NULL, RT_NULL, RT_NULL, _net_mgmt_dev_control };
#endif

static int net_mgmt_dev_init(void)
{
    rt_err_t         err        = RT_EOK;
    static rt_int8_t _init_flag = 0;

    /* Execute only once */
    if (_init_flag == 0) {
        rt_memset(&net_mgmt_device, 0, sizeof(struct rt_net_mgmt_device));

#ifdef RT_USING_DEVICE_OPS
        net_mgmt_device.device.ops = &net_mgmt_dev_ops;
#else
        net_mgmt_device.device.init    = _wlan_mgmt_init;
        net_mgmt_device.device.open    = RT_NULL;
        net_mgmt_device.device.close   = RT_NULL;
        net_mgmt_device.device.read    = RT_NULL;
        net_mgmt_device.device.write   = RT_NULL;
        net_mgmt_device.device.control = _rt_wlan_dev_control;
#endif
        net_mgmt_device.device.user_data = RT_NULL;
        net_mgmt_device.device.type      = RT_Device_Class_Miscellaneous;

        // rt_mutex_init(&net_mgmt_device.lock, "wlan_mgmt_dev", RT_IPC_FLAG_FIFO);

        if (RT_EOK != (err = rt_device_register(&net_mgmt_device.device, "netmgmt", RT_DEVICE_FLAG_RDWR))) {
            LOG_E("net_mgmt_device register failed, %d\n", errno);
        }
    }
    return 0;
}
INIT_APP_EXPORT(net_mgmt_dev_init);

void netdev_change_resolv_conf(int dns_cnt, const ip_addr_t* dns_servers)
{
    struct dfs_fd fd;
    char          content[64] = { 0 };
    int           content_len = 0;

    if (dns_cnt <= 0 || dns_servers == NULL) {
        LOG_E("No DNS servers provided.\n");
        return;
    }

    if (dfs_file_open(&fd, "/etc/resolv.conf", O_CREAT | O_TRUNC) != 0) {
        LOG_E("Open /etc/resolv.conf failed.\n");
        return;
    }

    rt_memset(content, 0x00, sizeof(content));
    if (dfs_file_write(&fd, content, sizeof(content)) != sizeof(content)) {
        LOG_E("Clear /etc/resolv.conf failed.\n");
    }
    dfs_file_lseek(&fd, SEEK_SET);

    for (int i = 0; i < dns_cnt; ++i) {
        char           ip[32] = { 0 };
        struct in_addr addr;
        addr.s_addr = dns_servers[i].addr;

        inet_ntoa_r(addr, ip, sizeof(ip));
        int len = rt_snprintf(content + content_len, sizeof(content) - content_len, "nameserver %s\n", ip);

        if (len < 0 || len >= (int)(sizeof(content) - content_len)) {
            LOG_E("DNS entry too long or truncated.\n");
            break;
        }

        content_len += len;
    }

    if (dfs_file_write(&fd, content, content_len) != content_len) {
        LOG_E("Write /etc/resolv.conf failed.\n");
    }

    dfs_file_close(&fd);
}

void netdev_generate_services_file(void)
{
    struct dfs_fd fd;

    static const char* default_services = "http        80/tcp\n"
                                          "https       443/tcp\n"
                                          "ftp         21/tcp\n"
                                          "ssh         22/tcp\n"
                                          "telnet      23/tcp\n"
                                          "smtp        25/tcp\n"
                                          "domain      53/udp\n"
                                          "ntp         123/udp\n";

    if (0x00 != dfs_file_open(&fd, "/etc/services", O_CREAT | O_TRUNC | O_WRONLY)) {
        rt_kprintf("Open /etc/services failed.\n");
        return;
    }

    int size = strlen(default_services);
    if (size != dfs_file_write(&fd, default_services, size)) {
        rt_kprintf("Write /etc/services failed.\n");
    }

    dfs_file_close(&fd);
}
