/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_wifi.h"

#ifdef ESP_HOSTED_WIFI_AUTO_START
#include <wlan_mgnt.h>
#endif
#ifdef ESP_HOSTED_WIFI_CONTROL
#include <wlan_offload_control.h>
#endif

#define DBG_TAG "esp_hosted.wifi"
#define DBG_LVL ESP_HOSTED_WIFI_DBG_LVL
#include <rtdbg.h>

#ifdef ESP_HOSTED_WIFI_NG
#define EHF_MAX_STATIONS 8U
#else
#define EHF_MAX_STATIONS 10U
#endif

static struct ehf_context g_ehf_context;

static const struct rt_wlan_offload_channel g_ehf_channels_2ghz[] = {
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 1, 2412, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 2, 2417, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 3, 2422, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 4, 2427, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 5, 2432, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 6, 2437, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 7, 2442, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 8, 2447, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 9, 2452, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 10, 2457, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 11, 2462, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 12, 2467, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_2GHZ, 13, 2472, 0, 20},
};

static const struct rt_wlan_offload_rate g_ehf_rates_2ghz[] = {
    {10, 0, 0}, {20, 1, 0}, {55, 2, 0}, {110, 3, 0},
    {60, 4, 0}, {90, 5, 0}, {120, 6, 0}, {180, 7, 0},
    {240, 8, 0}, {360, 9, 0}, {480, 10, 0}, {540, 11, 0},
};

static const struct rt_wlan_offload_supported_band g_ehf_band_2ghz = {
    .id = RT_WLAN_OFFLOAD_BAND_2GHZ,
    .phy_capabilities = RT_WLAN_OFFLOAD_PHY_11B | RT_WLAN_OFFLOAD_PHY_11G |
                        RT_WLAN_OFFLOAD_PHY_HT,
    .channels = g_ehf_channels_2ghz,
    .channel_count = sizeof(g_ehf_channels_2ghz) /
                     sizeof(g_ehf_channels_2ghz[0]),
    .rates = g_ehf_rates_2ghz,
    .rate_count = sizeof(g_ehf_rates_2ghz) / sizeof(g_ehf_rates_2ghz[0]),
    .max_spatial_streams = 1,
#ifdef ESP_HOSTED_WIFI_FG
    .max_channel_width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20,
#else
    .max_channel_width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40,
#endif
    .max_channel_width_set = RT_TRUE,
};

#ifdef ESP_HOSTED_WIFI_5GHZ
static const struct rt_wlan_offload_channel g_ehf_channels_5ghz[] = {
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 36, 5180, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 40, 5200, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 44, 5220, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 48, 5240, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 149, 5745, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 153, 5765, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 157, 5785, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 161, 5805, 0, 20},
    {RT_WLAN_OFFLOAD_BAND_5GHZ, 165, 5825, 0, 20},
};

static const struct rt_wlan_offload_supported_band g_ehf_band_5ghz = {
    .id = RT_WLAN_OFFLOAD_BAND_5GHZ,
    .phy_capabilities = RT_WLAN_OFFLOAD_PHY_11A | RT_WLAN_OFFLOAD_PHY_HT
#ifdef ESP_HOSTED_WIFI_NG
                        | RT_WLAN_OFFLOAD_PHY_VHT
#endif
                        ,
    .channels = g_ehf_channels_5ghz,
    .channel_count = sizeof(g_ehf_channels_5ghz) /
                     sizeof(g_ehf_channels_5ghz[0]),
    .rates = &g_ehf_rates_2ghz[4],
    .rate_count = 8,
    .max_spatial_streams = 1,
#ifdef ESP_HOSTED_WIFI_FG
    .max_channel_width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_20,
#else
    .max_channel_width = RT_WLAN_OFFLOAD_CHANNEL_WIDTH_40,
#endif
    .max_channel_width_set = RT_TRUE,
};
#endif

static const enum rt_wlan_offload_cipher g_ehf_ciphers[] = {
    RT_WLAN_OFFLOAD_CIPHER_WEP40,
    RT_WLAN_OFFLOAD_CIPHER_WEP104,
    RT_WLAN_OFFLOAD_CIPHER_TKIP,
    RT_WLAN_OFFLOAD_CIPHER_CCMP,
#ifdef ESP_HOSTED_WIFI_NG
    RT_WLAN_OFFLOAD_CIPHER_GCMP,
    RT_WLAN_OFFLOAD_CIPHER_GCMP_256,
    RT_WLAN_OFFLOAD_CIPHER_AES_CMAC,
#endif
};

static const struct rt_wlan_offload_iface_limit g_ehf_iface_limits[] = {
    {RT_WLAN_OFFLOAD_IFTYPE_BIT(RT_WLAN_OFFLOAD_IFTYPE_STATION), 1},
    {RT_WLAN_OFFLOAD_IFTYPE_BIT(RT_WLAN_OFFLOAD_IFTYPE_AP), 1},
};

static const struct rt_wlan_offload_iface_combination g_ehf_iface_combinations[] = {
    {
        .limits = g_ehf_iface_limits,
        .limit_count = sizeof(g_ehf_iface_limits) /
                       sizeof(g_ehf_iface_limits[0]),
        .max_interfaces = 2,
        .num_different_channels = 1,
    },
};

struct ehf_context *ehf_context_from_radio(struct rt_wlan_offload_radio *radio)
{
    return radio ? rt_wlan_offload_get_driver_data(radio) : RT_NULL;
}

struct ehf_context *ehf_context_from_vif(struct rt_wlan_offload_vif *vif)
{
    return vif ? ehf_context_from_radio(vif->radio) : RT_NULL;
}

static rt_err_t ehf_bus_receive(struct rt_wlan_offload_bus *bus, const void *data,
                                rt_size_t length, void *parameter)
{
    struct ehf_context *context = parameter;

    if (!context || context->bus != bus || !context->protocol ||
        !context->protocol->receive)
    {
        return -RT_EINVAL;
    }
    return context->protocol->receive(context, data, length);
}

static void ehf_bus_event(struct rt_wlan_offload_bus *bus,
                          enum rt_wlan_offload_bus_event event, rt_err_t status,
                          void *parameter)
{
    struct ehf_context *context = parameter;
    struct rt_wlan_offload_event report;

    if (!context || context->bus != bus ||
        (event != RT_WLAN_OFFLOAD_BUS_EVENT_ERROR &&
         event != RT_WLAN_OFFLOAD_BUS_EVENT_UNAVAILABLE))
    {
        return;
    }

    rt_wlan_offload_command_manager_fail(&context->commands,
                                    status == RT_EOK ? -RT_EIO : status);
    if (!context->booted)
    {
        rt_completion_done(&context->boot_completion);
    }
    if (context->radio.state == RT_WLAN_OFFLOAD_UNREGISTERED)
    {
        return;
    }
    rt_memset(&report, 0, sizeof(report));
    report.type = RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR;
    report.iftype = RT_WLAN_OFFLOAD_IFTYPE_MAX;
    report.status = status == RT_EOK ? -RT_EIO : status;
    report.data.firmware.reason = event;
    rt_wlan_offload_report_event(&context->radio, &report);
}

void ehf_bus_prepare(struct rt_wlan_offload_bus *bus)
{
    struct ehf_context *context = &g_ehf_context;

    if (!context->attached || context->bus != bus)
    {
        return;
    }
    context->booted = RT_FALSE;
    context->checksum_enabled = RT_FALSE;
    context->firmware_capabilities = 0;
    context->chip_id = 0;
    context->invalid_rx_log_count = 0;
    rt_memset(context->firmware_version, 0,
              sizeof(context->firmware_version));
    rt_completion_init(&context->boot_completion);
    rt_wlan_offload_command_manager_reset(&context->commands);
    if (context->protocol->reset)
    {
        context->protocol->reset(context);
    }
}

void ehf_boot_ready(struct ehf_context *context, rt_uint8_t capabilities,
                    rt_uint8_t chip_id, const rt_uint8_t version[8])
{
    struct rt_wlan_offload_firmware_info info;
    rt_bool_t first_boot;

    if (!context || !context->attached)
    {
        return;
    }
    first_boot = !context->booted;
    context->firmware_capabilities = capabilities;
    context->chip_id = chip_id;
    if (version)
    {
        rt_memcpy(context->firmware_version, version,
                  sizeof(context->firmware_version));
    }
#ifdef ESP_HOSTED_WIFI_CHECKSUM
    context->checksum_enabled = (capabilities & EHF_CAP_CHECKSUM) != 0;
#endif
    context->booted = RT_TRUE;

    if (first_boot)
    {
        LOG_I("valid %s boot event received from ESP",
              context->protocol->name);
    }

    rt_memset(&info, 0, sizeof(info));
    info.protocol_version =
#ifdef ESP_HOSTED_WIFI_FG
        1;
#else
        2;
#endif
    info.firmware_version = ((rt_uint32_t)context->firmware_version[3] << 24) |
                            ((rt_uint32_t)context->firmware_version[4] << 16) |
                            ((rt_uint32_t)context->firmware_version[5] << 8) |
                            context->firmware_version[6];
    info.features = capabilities;
    info.max_stations = EHF_MAX_STATIONS;
    info.max_vifs = 2;
    info.max_channel_contexts = 1;
    rt_wlan_offload_update_firmware_info(&context->radio, &info);
    rt_completion_done(&context->boot_completion);
}

rt_err_t ehf_wait_for_boot(struct ehf_context *context)
{
    rt_uint8_t required_capability;
    rt_err_t result;

    if (!context)
    {
        return -RT_EINVAL;
    }
    if (!context->booted)
    {
        LOG_I("waiting up to %d ms for a valid %s boot event",
              ESP_HOSTED_WIFI_BOOT_TIMEOUT_MS,
              context->protocol->name);
        result = rt_completion_wait(
            &context->boot_completion,
            rt_tick_from_millisecond(ESP_HOSTED_WIFI_BOOT_TIMEOUT_MS));
        if (result != RT_EOK)
        {
            LOG_E("%s link not established: boot event timed out after %d ms",
                  context->protocol->name,
                  ESP_HOSTED_WIFI_BOOT_TIMEOUT_MS);
            return result;
        }
        if (!context->booted)
        {
            return -RT_EIO;
        }
    }
    required_capability = context->bus->type == RT_WLAN_OFFLOAD_BUS_SDIO ?
                          EHF_CAP_SDIO : EHF_CAP_SPI;
    if (!(context->firmware_capabilities & required_capability))
    {
        LOG_E("firmware capabilities 0x%02x do not support selected transport",
              context->firmware_capabilities);
        return -RT_ENOSYS;
    }
    LOG_I("%s ready: chip=%u version=%u.%u.%u.%u capabilities=0x%02x",
          context->protocol->name, context->chip_id,
          context->firmware_version[3], context->firmware_version[4],
          context->firmware_version[5], context->firmware_version[6],
          context->firmware_capabilities);
    return RT_EOK;
}

#ifdef ESP_HOSTED_WIFI_AUTO_START
static void ehf_auto_start_worker(void *parameter)
{
    struct ehf_context *context = parameter;
    rt_err_t result;

    if (!context->auto_start_cancelled && context->attached)
    {
        result = rt_wlan_set_mode(
            context->radio.vifs[RT_WLAN_OFFLOAD_VIF_STA_INDEX].wlan.device.parent.name,
            RT_WLAN_STATION);
        if (!context->auto_start_cancelled)
        {
            if (result == RT_EOK)
            {
                LOG_I("station interface initialized automatically");
            }
            else
            {
                LOG_W("automatic station initialization failed: %d; "
                      "the WLAN offload control device remains available",
                      result);
            }
        }
    }
    rt_completion_done(&context->auto_start_completion);
}

static rt_err_t ehf_schedule_auto_start(struct ehf_context *context)
{
    rt_err_t result;

    rt_completion_init(&context->auto_start_completion);
    context->auto_start_cancelled = RT_FALSE;
    context->auto_start_thread = rt_thread_create(
        "ehf-probe", ehf_auto_start_worker, context,
        ESP_HOSTED_WIFI_THREAD_STACK_SIZE,
        ESP_HOSTED_WIFI_THREAD_PRIORITY, 10);
    if (!context->auto_start_thread)
    {
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(context->auto_start_thread);
    if (result != RT_EOK)
    {
        rt_thread_delete(context->auto_start_thread);
        context->auto_start_thread = RT_NULL;
    }
    return result;
}
#endif

rt_err_t ehf_attach_bus(struct rt_wlan_offload_bus *bus)
{
    struct ehf_context *context = &g_ehf_context;
    struct rt_wlan_offload_command_manager_config command_config;
    struct rt_wlan_offload_radio_config radio_config;
    rt_err_t result;

    if (!bus || context->attached)
    {
        return -RT_EBUSY;
    }
    rt_memset(context, 0, sizeof(*context));
#ifdef ESP_HOSTED_WIFI_FG
    context->protocol = &g_ehf_fg_protocol;
#else
    context->protocol = &g_ehf_ng_protocol;
#endif
    context->bus = bus;
    rt_completion_init(&context->boot_completion);

    rt_memset(&command_config, 0, sizeof(command_config));
    command_config.max_pending = context->protocol->max_pending_commands;
    command_config.push = context->protocol->command_push;
    command_config.driver_data = context;
    result = rt_wlan_offload_command_manager_init(&context->commands,
                                             &command_config);
    if (result != RT_EOK)
    {
        return result;
    }

    rt_wlan_offload_bus_set_callbacks(bus, ehf_bus_receive, ehf_bus_event, context);
    rt_memset(&radio_config, 0, sizeof(radio_config));
    radio_config.api_version = RT_WLAN_OFFLOAD_API_VERSION;
#ifdef ESP_HOSTED_WIFI_FG
    radio_config.model_name = "esp-hosted-fg";
#else
    radio_config.model_name = "esp-hosted-ng";
#endif
#ifdef ESP_HOSTED_WIFI_CONTROL
    radio_config.control_device = RT_TRUE;
#endif
    radio_config.ops = context->protocol->wlan_offload_ops;
    radio_config.bus = bus;
    radio_config.capabilities = context->protocol->capabilities;
    if (bus->capabilities & RT_WLAN_OFFLOAD_BUS_CAP_HOTPLUG)
    {
        radio_config.capabilities |= RT_WLAN_OFFLOAD_CAP_HOTPLUG;
    }
    radio_config.max_frame_size = EHF_ETHERNET_FRAME_MAX;
    radio_config.bands[RT_WLAN_OFFLOAD_BAND_2GHZ] = &g_ehf_band_2ghz;
#ifdef ESP_HOSTED_WIFI_5GHZ
    radio_config.bands[RT_WLAN_OFFLOAD_BAND_5GHZ] = &g_ehf_band_5ghz;
#endif
    radio_config.cipher_suites = g_ehf_ciphers;
    radio_config.cipher_suite_count = sizeof(g_ehf_ciphers) /
                                      sizeof(g_ehf_ciphers[0]);
    radio_config.iface_combinations = g_ehf_iface_combinations;
    radio_config.iface_combination_count =
        sizeof(g_ehf_iface_combinations) /
        sizeof(g_ehf_iface_combinations[0]);
    radio_config.max_scan_ssids = 1;
    radio_config.max_scan_ie_length = 0;
    radio_config.firmware_info.max_stations = EHF_MAX_STATIONS;
    radio_config.firmware_info.max_vifs = 2;
    radio_config.firmware_info.max_channel_contexts = 1;
    radio_config.driver_data = context;
    result = rt_wlan_offload_register_radio(&context->radio, &radio_config);
    if (result != RT_EOK)
    {
        rt_wlan_offload_bus_set_callbacks(bus, RT_NULL, RT_NULL, RT_NULL);
        rt_wlan_offload_command_manager_deinit(&context->commands);
        rt_memset(context, 0, sizeof(*context));
        return result;
    }
    context->attached = RT_TRUE;
    LOG_I("registered %s over %s", context->protocol->name,
          bus->type == RT_WLAN_OFFLOAD_BUS_SDIO ? "SDIO" : "SPI");
#ifdef ESP_HOSTED_WIFI_CONTROL
    {
        char control_name[RT_NAME_MAX + 1];

        if (rt_wlan_offload_control_get_name(
                &context->radio, control_name, sizeof(control_name)) == RT_EOK)
        {
            LOG_I("userspace control device: /dev/%s", control_name);
        }
    }
#endif
#ifdef ESP_HOSTED_WIFI_AUTO_START
    result = ehf_schedule_auto_start(context);
    if (result == RT_EOK)
    {
        LOG_I("station interface initialization scheduled");
    }
    else
    {
        LOG_W("cannot schedule automatic station initialization: %d; "
              "the WLAN offload control device remains available", result);
    }
#else
    LOG_I("ESP link probe starts when the WLAN device is initialized");
#endif
    return RT_EOK;
}

void ehf_detach_bus(struct rt_wlan_offload_bus *bus)
{
    struct ehf_context *context = &g_ehf_context;

    if (!context->attached || context->bus != bus)
    {
        return;
    }
#ifdef ESP_HOSTED_WIFI_AUTO_START
    if (context->auto_start_thread)
    {
        context->auto_start_cancelled = RT_TRUE;
        rt_completion_done(&context->boot_completion);
        rt_completion_wait(&context->auto_start_completion,
                           RT_WAITING_FOREVER);
        context->auto_start_thread = RT_NULL;
    }
#endif
    rt_wlan_offload_unregister_radio(&context->radio);
    rt_wlan_offload_bus_set_callbacks(bus, RT_NULL, RT_NULL, RT_NULL);
    rt_wlan_offload_command_manager_deinit(&context->commands);
    if (context->reassembly)
    {
        rt_free(context->reassembly);
    }
    rt_memset(context, 0, sizeof(*context));
}

static int ehf_driver_init(void)
{
#ifdef ESP_HOSTED_WIFI_TRANSPORT_SPI
    return ehf_spi_driver_init();
#else
    return ehf_sdio_driver_init();
#endif
}
INIT_COMPONENT_EXPORT(ehf_driver_init);
