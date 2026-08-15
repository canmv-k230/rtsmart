/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-08-13     tyx          the first version
 */

#include <rtthread.h>
#include <wlan_mgnt.h>
#include <wlan_cfg.h>
#include <wlan_prot.h>

#if defined(RT_WLAN_MANAGE_ENABLE) && defined(RT_WLAN_MSH_CMD_ENABLE)

struct wifi_cmd_des
{
    const char *cmd;
    int (*fun)(int argc, char *argv[]);
    rt_wlan_mode_t mode;
};

static int wifi_help(int argc, char *argv[]);
static int wifi_devices(int argc, char *argv[]);
static int wifi_scan(int argc, char *argv[]);
static int wifi_status(int argc, char *argv[]);
static int wifi_join(int argc, char *argv[]);
static int wifi_ap(int argc, char *argv[]);
static int wifi_list_sta(int argc, char *argv[]);
static int wifi_disconnect(int argc, char *argv[]);
static int wifi_ap_stop(int argc, char *argv[]);

#ifdef RT_WLAN_CMD_DEBUG
/* just for debug  */
static int wifi_debug(int argc, char *argv[]);
static int wifi_debug_save_cfg(int argc, char *argv[]);
static int wifi_debug_dump_cfg(int argc, char *argv[]);
static int wifi_debug_clear_cfg(int argc, char *argv[]);
static int wifi_debug_dump_prot(int argc, char *argv[]);
static int wifi_debug_set_mode(int argc, char *argv[]);
static int wifi_debug_set_prot(int argc, char *argv[]);
static int wifi_debug_set_autoconnect(int argc, char *argv[]);
#endif

/* cmd table */
static const struct wifi_cmd_des cmd_tab[] =
{
    {"help", wifi_help, RT_WLAN_NONE},
    {"devices", wifi_devices, RT_WLAN_NONE},
    {"scan", wifi_scan, RT_WLAN_STATION},
    {"join", wifi_join, RT_WLAN_STATION},
    {"ap", wifi_ap, RT_WLAN_AP},
    {"list_sta", wifi_list_sta, RT_WLAN_AP},
    {"disc", wifi_disconnect, RT_WLAN_STATION},
    {"ap_stop", wifi_ap_stop, RT_WLAN_AP},
    {"status", wifi_status, RT_WLAN_MODE_MAX},
    {"smartconfig", RT_NULL, RT_WLAN_STATION},
#ifdef RT_WLAN_CMD_DEBUG
    {"-d", wifi_debug, RT_WLAN_NONE},
#endif
};

#ifdef RT_WLAN_CMD_DEBUG
/* debug cmd table */
static const struct wifi_cmd_des debug_tab[] =
{
    {"save_cfg", wifi_debug_save_cfg, RT_WLAN_NONE},
    {"dump_cfg", wifi_debug_dump_cfg, RT_WLAN_NONE},
    {"clear_cfg", wifi_debug_clear_cfg, RT_WLAN_NONE},
    {"dump_prot", wifi_debug_dump_prot, RT_WLAN_NONE},
    {"mode", wifi_debug_set_mode, RT_WLAN_NONE},
    {"prot", wifi_debug_set_prot, RT_WLAN_NONE},
    {"auto", wifi_debug_set_autoconnect, RT_WLAN_NONE},
};
#endif

static int wifi_help(int argc, char *argv[])
{
    rt_kprintf("wifi\n");
    rt_kprintf("wifi help\n");
    rt_kprintf("wifi devices\n");
    rt_kprintf("wifi [-i DEVICE] scan [SSID]\n");
    rt_kprintf("wifi [-i DEVICE] join [SSID] [PASSWORD]\n");
    rt_kprintf("wifi [-i DEVICE] ap SSID [PASSWORD] [2g|5g] [CHANNEL]\n");
    rt_kprintf("wifi [-i DEVICE] list_sta\n");
    rt_kprintf("wifi [-i DEVICE] disc\n");
    rt_kprintf("wifi [-i DEVICE] ap_stop\n");
    rt_kprintf("wifi [-i DEVICE] status\n");
    rt_kprintf("wifi smartconfig\n");
    rt_kprintf("DEVICE: auto, usb, sdio, spi, WLAN device, or network interface\n");
#ifdef RT_WLAN_CMD_DEBUG
    rt_kprintf("wifi -d debug command\n");
#endif
    return 0;
}

static const char *wifi_mode_name(rt_wlan_mode_t mode)
{
    switch (mode)
    {
    case RT_WLAN_STATION:
        return "sta";
    case RT_WLAN_AP:
        return "ap";
    default:
        return "none";
    }
}

static const char *wifi_transport_name(rt_wlan_transport_t transport)
{
    switch (transport)
    {
    case RT_WLAN_TRANSPORT_USB:
        return "usb";
    case RT_WLAN_TRANSPORT_SDIO:
        return "sdio";
    case RT_WLAN_TRANSPORT_SPI:
        return "spi";
    default:
        return "unknown";
    }
}

static int wifi_devices(int argc, char *argv[])
{
    struct rt_wlan_device_info *devices;
    struct rt_wlan_device *selected_sta;
    struct rt_wlan_device *selected_ap;
    const char *netif_name;
    rt_size_t count;
    rt_size_t index;
    rt_size_t total;
    rt_bool_t selected;

    if (argc != 2)
    {
        return -1;
    }

    count = rt_wlan_dev_get_info(RT_NULL, 0);
    if (count == 0)
    {
        rt_kprintf("No Wi-Fi devices registered\n");
        return 0;
    }

    devices = rt_calloc(count, sizeof(*devices));
    if (devices == RT_NULL)
    {
        rt_kprintf("wifi: cannot allocate device list\n");
        return 0;
    }
    total = rt_wlan_dev_get_info(devices, count);
    if (total < count)
    {
        count = total;
    }

    selected_sta = rt_wlan_get_device(RT_WLAN_STATION);
    selected_ap = rt_wlan_get_device(RT_WLAN_AP);
    rt_kprintf("device               netif                role bus     state selected\n");
    rt_kprintf("-------------------- -------------------- ---- ------- ----- --------\n");
    for (index = 0; index < count; index++)
    {
        netif_name = devices[index].netif_name[0] ?
                     devices[index].netif_name : "-";
        selected = (selected_sta != RT_NULL &&
                    rt_strcmp(devices[index].device_name,
                              selected_sta->device.parent.name) == 0) ||
                   (selected_ap != RT_NULL &&
                    rt_strcmp(devices[index].device_name,
                              selected_ap->device.parent.name) == 0);
        rt_kprintf("%-20.20s %-20.20s %-4.4s %-7.7s %-5.5s %s\n",
                   devices[index].device_name, netif_name,
                   wifi_mode_name(devices[index].registered_mode),
                   wifi_transport_name(devices[index].transport),
                   wifi_mode_name(devices[index].mode),
                   selected ? "yes" : "no");
    }
    rt_free(devices);
    return 0;
}

static rt_bool_t wifi_parse_transport(const char *name,
                                      rt_wlan_transport_t *transport)
{
    if (rt_strcmp(name, "auto") == 0)
    {
        *transport = RT_WLAN_TRANSPORT_UNKNOWN;
    }
    else if (rt_strcmp(name, "usb") == 0)
    {
        *transport = RT_WLAN_TRANSPORT_USB;
    }
    else if (rt_strcmp(name, "sdio") == 0)
    {
        *transport = RT_WLAN_TRANSPORT_SDIO;
    }
    else if (rt_strcmp(name, "spi") == 0)
    {
        *transport = RT_WLAN_TRANSPORT_SPI;
    }
    else
    {
        return RT_FALSE;
    }
    return RT_TRUE;
}

static rt_err_t wifi_select_status_device(const char *selector)
{
    rt_wlan_transport_t transport;
    rt_wlan_mode_t mode;
    rt_err_t sta_result;
    rt_err_t ap_result;

    if (wifi_parse_transport(selector, &transport))
    {
        sta_result = rt_wlan_select_device(RT_WLAN_STATION, transport);
        ap_result = rt_wlan_select_device(RT_WLAN_AP, transport);
        return sta_result == RT_EOK || ap_result == RT_EOK ? RT_EOK : -RT_EIO;
    }

    mode = rt_wlan_get_mode(selector);
    if (mode == RT_WLAN_STATION || mode == RT_WLAN_AP)
    {
        return rt_wlan_set_mode(selector, mode);
    }

    sta_result = rt_wlan_set_mode(selector, RT_WLAN_STATION);
    if (sta_result == RT_EOK)
    {
        return RT_EOK;
    }
    return rt_wlan_set_mode(selector, RT_WLAN_AP);
}

static rt_err_t wifi_select_command_device(const char *selector,
                                           rt_wlan_mode_t mode)
{
    rt_wlan_transport_t transport;

    if (mode == RT_WLAN_MODE_MAX)
    {
        return wifi_select_status_device(selector);
    }
    if (mode != RT_WLAN_STATION && mode != RT_WLAN_AP)
    {
        return -RT_EINVAL;
    }
    if (wifi_parse_transport(selector, &transport))
    {
        return rt_wlan_select_device(mode, transport);
    }
    return rt_wlan_set_mode(selector, mode);
}

static void wifi_print_selected_device(rt_wlan_mode_t mode)
{
    struct rt_wlan_device *device;
    const char *role;

    device = rt_wlan_get_device(mode);
    role = mode == RT_WLAN_STATION ? "STA" : "AP";
    if (device == RT_NULL)
    {
        rt_kprintf("Wi-Fi %s Device: none\n", role);
        return;
    }
    rt_kprintf("Wi-Fi %s Device: %s (%s)\n", role,
               device->device.parent.name,
               wifi_transport_name(device->transport));
}

static int wifi_status(int argc, char *argv[])
{
    int rssi;
    struct rt_wlan_info info;

    if (argc > 2)
        return -1;

    wifi_print_selected_device(RT_WLAN_STATION);
    if (rt_wlan_is_connected() == 1)
    {
        rssi = rt_wlan_get_rssi();
        rt_wlan_get_info(&info);
        rt_kprintf("Wi-Fi STA Info\n");
        rt_kprintf("SSID : %-.32s\n", &info.ssid.val[0]);
        rt_kprintf("MAC Addr: %02x:%02x:%02x:%02x:%02x:%02x\n", info.bssid[0],
                   info.bssid[1],
                   info.bssid[2],
                   info.bssid[3],
                   info.bssid[4],
                   info.bssid[5]);
        rt_kprintf("Channel: %d\n", info.channel);
        rt_kprintf("DataRate: %dMbps\n", info.datarate / 1000000);
        rt_kprintf("RSSI: %d\n", rssi);
    }
    else
    {
        rt_kprintf("wifi disconnected!\n");
    }

    wifi_print_selected_device(RT_WLAN_AP);
    if (rt_wlan_ap_is_active() == 1)
    {
        rt_wlan_ap_get_info(&info);
        rt_kprintf("Wi-Fi AP Info\n");
        rt_kprintf("SSID : %-.32s\n", &info.ssid.val[0]);
        rt_kprintf("MAC Addr: %02x:%02x:%02x:%02x:%02x:%02x\n", info.bssid[0],
                   info.bssid[1],
                   info.bssid[2],
                   info.bssid[3],
                   info.bssid[4],
                   info.bssid[5]);
        rt_kprintf("Channel: %d\n", info.channel);
        rt_kprintf("DataRate: %dMbps\n", info.datarate / 1000000);
        rt_kprintf("hidden: %s\n", info.hidden ? "Enable" : "Disable");
    }
    else
    {
        rt_kprintf("wifi ap not start!\n");
    }
    rt_kprintf("Auto Connect status:%s!\n", (rt_wlan_get_autoreconnect_mode() ? "Enable" : "Disable"));
    return 0;
}

static int wifi_scan(int argc, char *argv[])
{
    struct rt_wlan_scan_result *scan_result = RT_NULL;
    struct rt_wlan_info *info = RT_NULL;
    struct rt_wlan_info filter;

    if (argc > 3)
        return -1;

    if (argc == 3)
    {
        INVALID_INFO(&filter);
        SSID_SET(&filter, argv[2]);
        info = &filter;
    }

    /* clean scan result */
    rt_wlan_scan_result_clean();
    /* scan ap info */
    scan_result = rt_wlan_scan_with_info(info);
    if (scan_result)
    {
        int index, num;
        const char *security;

        num = scan_result->num;
        rt_kprintf("             SSID                      MAC                   security                 rssi chn Mbps\n");
        rt_kprintf("------------------------------- -----------------  ---------------------------- ---- --- ----\n");
        for (index = 0; index < num; index ++)
        {
            rt_kprintf("%-32.32s", &scan_result->info[index].ssid.val[0]);
            rt_kprintf("%02x:%02x:%02x:%02x:%02x:%02x  ",
                       scan_result->info[index].bssid[0],
                       scan_result->info[index].bssid[1],
                       scan_result->info[index].bssid[2],
                       scan_result->info[index].bssid[3],
                       scan_result->info[index].bssid[4],
                       scan_result->info[index].bssid[5]
                      );
            security = rt_wlan_security_name(
                scan_result->info[index].security);
            rt_kprintf("%-28.28s ", security);
            rt_kprintf("%-4d ", scan_result->info[index].rssi);
            rt_kprintf("%3d ", scan_result->info[index].channel);
            rt_kprintf("%4d\n", scan_result->info[index].datarate / 1000000);
        }
        rt_wlan_scan_result_clean();
    }
    else
    {
        rt_kprintf("wifi scan result is null\n");
    }
    return 0;
}

static int wifi_join(int argc, char *argv[])
{
    const char *ssid = RT_NULL;
    const char *key = RT_NULL;
    struct rt_wlan_cfg_info cfg_info;
    rt_err_t result;

    rt_memset(&cfg_info, 0, sizeof(cfg_info));
    if (argc ==  2)
    {
#ifdef RT_WLAN_CFG_ENABLE
        /* get info to connect */
        if (rt_wlan_cfg_read_index(&cfg_info, 0) == 1)
        {
            ssid = (char *)(&cfg_info.info.ssid.val[0]);
            if (cfg_info.key.len)
                key = (char *)(&cfg_info.key.val[0]);
        }
        else
#endif
        {
            rt_kprintf("not find connect info\n");
        }
    }
    else if (argc == 3)
    {
        /* ssid */
        ssid = argv[2];
    }
    else if (argc == 4)
    {
        ssid = argv[2];
        /* password */
        key = argv[3];
    }
    else
    {
        return -1;
    }
    result = rt_wlan_connect(ssid, key);
    if (result != RT_EOK)
    {
        rt_kprintf("wifi join failed: %d\n", result);
    }
    return result;
}

static rt_bool_t wifi_ap_parse_band(const char *name,
                                    rt_802_11_band_t *band,
                                    int *default_channel)
{
    if (rt_strcmp(name, "2g") == 0 ||
        rt_strcmp(name, "2.4") == 0 ||
        rt_strcmp(name, "2.4g") == 0 ||
        rt_strcmp(name, "2.4ghz") == 0)
    {
        *band = RT_802_11_BAND_2_4GHZ;
        *default_channel = 6;
        return RT_TRUE;
    }
    if (rt_strcmp(name, "5g") == 0 ||
        rt_strcmp(name, "5.8") == 0 ||
        rt_strcmp(name, "5.8g") == 0 ||
        rt_strcmp(name, "5ghz") == 0 ||
        rt_strcmp(name, "5.8ghz") == 0)
    {
        *band = RT_802_11_BAND_5GHZ;
        *default_channel = 149;
        return RT_TRUE;
    }
    return RT_FALSE;
}

static rt_bool_t wifi_ap_parse_channel(const char *text, int *channel)
{
    int value = 0;

    if (!text[0])
    {
        return RT_FALSE;
    }
    while (*text)
    {
        if (*text < '0' || *text > '9' || value > 3276)
        {
            return RT_FALSE;
        }
        value = value * 10 + (*text++ - '0');
    }
    if (value <= 0 || value > 0x7fff)
    {
        return RT_FALSE;
    }
    *channel = value;
    return RT_TRUE;
}

static int wifi_ap(int argc, char *argv[])
{
    const char *ssid = RT_NULL;
    const char *key = RT_NULL;
    rt_802_11_band_t band = RT_802_11_BAND_2_4GHZ;
    int channel = 6;
    rt_bool_t channel_selected = RT_FALSE;
    int result;

    if (argc < 3 || argc > 6)
    {
        return -1;
    }
    ssid = argv[2];

    if (argc == 4)
    {
        if (wifi_ap_parse_band(argv[3], &band, &channel))
        {
            channel_selected = RT_TRUE;
        }
        else
        {
            key = argv[3];
        }
    }
    else if (argc == 5)
    {
        channel_selected = RT_TRUE;
        if (wifi_ap_parse_band(argv[3], &band, &channel))
        {
            if (!wifi_ap_parse_channel(argv[4], &channel))
            {
                rt_kprintf("invalid channel\n");
                return -RT_EINVAL;
            }
        }
        else
        {
            key = argv[3];
            if (!wifi_ap_parse_band(argv[4], &band, &channel))
            {
                rt_kprintf("invalid band: use 2g or 5g\n");
                return -RT_EINVAL;
            }
        }
    }
    else if (argc == 6)
    {
        channel_selected = RT_TRUE;
        key = argv[3];
        if (!wifi_ap_parse_band(argv[4], &band, &channel))
        {
            rt_kprintf("invalid band: use 2g or 5g\n");
            return -RT_EINVAL;
        }
        if (!wifi_ap_parse_channel(argv[5], &channel))
        {
            rt_kprintf("invalid channel\n");
            return -RT_EINVAL;
        }
    }

    if (channel_selected)
    {
        result = rt_wlan_start_ap_with_channel(ssid, key, band, channel);
    }
    else
    {
        result = rt_wlan_start_ap(ssid, key);
    }
    if (result != RT_EOK)
    {
        rt_kprintf("wifi ap start failed: %d\n", result);
    }
    return result;
}

static int wifi_list_sta(int argc, char *argv[])
{
    struct rt_wlan_info *sta_info;
    int num, i;

    if (argc > 2)
        return -1;
    num = rt_wlan_ap_get_sta_num();
    sta_info = rt_malloc(sizeof(struct rt_wlan_info) * num);
    if (sta_info == RT_NULL)
    {
        rt_kprintf("num:%d\n", num);
        return 0;
    }
    rt_wlan_ap_get_sta_info(sta_info, num);
    rt_kprintf("num:%d\n", num);
    for (i = 0; i < num; i++)
    {
        rt_kprintf("sta mac  %02x:%02x:%02x:%02x:%02x:%02x\n",
                   sta_info[i].bssid[0], sta_info[i].bssid[1], sta_info[i].bssid[2],
                   sta_info[i].bssid[3], sta_info[i].bssid[4], sta_info[i].bssid[5]);
    }
    rt_free(sta_info);
    return 0;
}

static int wifi_disconnect(int argc, char *argv[])
{
    if (argc != 2)
    {
        return -1;
    }

    rt_wlan_disconnect();
    return 0;
}

static int wifi_ap_stop(int argc, char *argv[])
{
    if (argc != 2)
    {
        return -1;
    }

    rt_wlan_ap_stop();
    return 0;
}

#ifdef RT_WLAN_CMD_DEBUG
/* just for debug */
static int wifi_debug_help(int argc, char *argv[])
{
    rt_kprintf("save_cfg ssid [password]\n");
    rt_kprintf("dump_cfg\n");
    rt_kprintf("clear_cfg\n");
    rt_kprintf("dump_prot\n");
    rt_kprintf("mode sta/ap dev_name\n");
    rt_kprintf("prot lwip dev_name\n");
    rt_kprintf("auto enable/disable\n");
    return 0;
}

static int wifi_debug_save_cfg(int argc, char *argv[])
{
    struct rt_wlan_cfg_info cfg_info;
    int len;
    char *ssid = RT_NULL, *password = RT_NULL;

    rt_memset(&cfg_info, 0, sizeof(cfg_info));
    INVALID_INFO(&cfg_info.info);
    if (argc == 2)
    {
        ssid = argv[1];
    }
    else if (argc == 3)
    {
        ssid = argv[1];
        password = argv[2];
    }
    else
    {
        return -1;
    }

    if (ssid != RT_NULL)
    {
        len = rt_strlen(ssid);
        if (len > RT_WLAN_SSID_MAX_LENGTH)
        {
            rt_kprintf("ssid is to long");
            return 0;
        }
        rt_memcpy(&cfg_info.info.ssid.val[0], ssid, len);
        cfg_info.info.ssid.len = len;
    }

    if (password != RT_NULL)
    {
        len = rt_strlen(password);
        if (len > RT_WLAN_PASSWORD_MAX_LENGTH)
        {
            rt_kprintf("password is to long");
            return 0;
        }
        rt_memcpy(&cfg_info.key.val[0], password, len);
        cfg_info.key.len = len;
    }
#ifdef RT_WLAN_CFG_ENABLE
    rt_wlan_cfg_save(&cfg_info);
#endif
    return 0;
}

static int wifi_debug_dump_cfg(int argc, char *argv[])
{
    if (argc == 1)
    {
#ifdef RT_WLAN_CFG_ENABLE
        rt_wlan_cfg_dump();
#endif
    }
    else
    {
        return -1;
    }
    return 0;
}

static int wifi_debug_clear_cfg(int argc, char *argv[])
{
    if (argc == 1)
    {
#ifdef RT_WLAN_CFG_ENABLE
        rt_wlan_cfg_delete_all();
        rt_wlan_cfg_cache_save();
#endif
    }
    else
    {
        return -1;
    }
    return 0;
}

static int wifi_debug_dump_prot(int argc, char *argv[])
{
    if (argc == 1)
    {
        rt_wlan_prot_dump();
    }
    else
    {
        return -1;
    }
    return 0;
}

static int wifi_debug_set_mode(int argc, char *argv[])
{
    rt_wlan_mode_t mode;

    if (argc != 3)
        return -1;

    if (rt_strcmp("sta", argv[1]) == 0)
    {
        mode = RT_WLAN_STATION;
    }
    else if (rt_strcmp("ap", argv[1]) == 0)
    {
        mode = RT_WLAN_AP;
    }
    else if (rt_strcmp("none", argv[1]) == 0)
    {
        mode = RT_WLAN_NONE;
    }
    else
        return -1;

    rt_wlan_set_mode(argv[2], mode);
    return 0;
}

static int wifi_debug_set_prot(int argc, char *argv[])
{
    if (argc != 3)
    {
        return -1;
    }

    rt_wlan_prot_attach(argv[2], argv[1]);
    return 0;
}

static int wifi_debug_set_autoconnect(int argc, char *argv[])
{
    if (argc == 2)
    {
        if (rt_strcmp(argv[1], "enable") == 0)
            rt_wlan_config_autoreconnect(RT_TRUE);
        else if (rt_strcmp(argv[1], "disable") == 0)
            rt_wlan_config_autoreconnect(RT_FALSE);
    }
    else
    {
        return -1;
    }
    return 0;
}

static int wifi_debug(int argc, char *argv[])
{
    int i, result = 0;
    const struct wifi_cmd_des *run_cmd = RT_NULL;

    if (argc < 3)
    {
        wifi_debug_help(0, RT_NULL);
        return 0;
    }

    for (i = 0; i < sizeof(debug_tab) / sizeof(debug_tab[0]); i++)
    {
        if (rt_strcmp(debug_tab[i].cmd, argv[2]) == 0)
        {
            run_cmd = &debug_tab[i];
            break;
        }
    }

    if (run_cmd == RT_NULL)
    {
        wifi_debug_help(0, RT_NULL);
        return 0;
    }

    if (run_cmd->fun != RT_NULL)
    {
        result = run_cmd->fun(argc - 2, &argv[2]);
    }

    if (result)
    {
        wifi_debug_help(argc - 2, &argv[2]);
    }
    return 0;
}
#endif

static int wifi_msh(int argc, char *argv[])
{
    int i, command_index = 1, command_argc, result = 0;
    const char *selector = RT_NULL;
    const struct wifi_cmd_des *run_cmd = RT_NULL;
    char *command_argv[argc];

    if (argc == 1)
    {
        wifi_help(argc, argv);
        return 0;
    }

    if (rt_strcmp(argv[1], "-i") == 0)
    {
        if (argc < 4)
        {
            wifi_help(argc, argv);
            return 0;
        }
        selector = argv[2];
        command_index = 3;
    }

    /* find fun */
    for (i = 0; i < sizeof(cmd_tab) / sizeof(cmd_tab[0]); i++)
    {
        if (rt_strcmp(cmd_tab[i].cmd, argv[command_index]) == 0)
        {
            run_cmd = &cmd_tab[i];
            break;
        }
    }

    /* not find fun, print help */
    if (run_cmd == RT_NULL)
    {
        wifi_help(argc, argv);
        return 0;
    }

    if (selector != RT_NULL)
    {
        result = wifi_select_command_device(selector, run_cmd->mode);
        if (result != RT_EOK)
        {
            rt_kprintf("wifi: cannot select device %s for %s: %d\n",
                       selector, run_cmd->cmd, result);
            return 0;
        }

        command_argv[0] = argv[0];
        command_argc = 1;
        for (i = command_index; i < argc; i++)
        {
            command_argv[command_argc++] = argv[i];
        }
    }
    else
    {
        command_argc = argc;
        for (i = 0; i < argc; i++)
        {
            command_argv[i] = argv[i];
        }
    }

    /* run fun */
    if (run_cmd->fun != RT_NULL)
    {
        result = run_cmd->fun(command_argc, command_argv);
    }

    if (result)
    {
        wifi_help(command_argc, command_argv);
    }
    return 0;
}

#if defined(RT_USING_FINSH)
FINSH_FUNCTION_EXPORT_ALIAS(wifi_msh, __cmd_wifi, wifi command.);
#endif

#endif
