/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Radio capability and calibration setup for the RT-Smart AIC8800 driver.
 */
#include "aic8800_wifi.h"
#include "aic8800_radio_dc.h"

#include <dfs_posix.h>
#include <string.h>

#define DBG_TAG "aic8800.radio"
#define DBG_LVL AIC8800_DBG_LVL
#include <rtdbg.h>

#ifndef AIC8800_WIFI_COUNTRY_CODE
#define AIC8800_WIFI_COUNTRY_CODE "CN"
#endif

#define AIC_HT_CAP_LDPC                 (1U << 0)
#define AIC_HT_CAP_SUP_WIDTH_20_40      (1U << 1)
#define AIC_HT_CAP_SHORT_GI_20          (1U << 5)
#define AIC_HT_CAP_SHORT_GI_40          (1U << 6)
#define AIC_HT_CAP_RX_STBC_1            (1U << 8)
#define AIC_HT_CAP_MAX_AMSDU            (1U << 11)
#define AIC_HT_AMPDU_MAX_64K             3U
#define AIC_HT_AMPDU_DENSITY_16_US       7U
#define AIC_HT_MCS_TX_DEFINED            1U
#define AIC_PHY_BANDWIDTH_20_MHZ          0U
#define AIC_PHY_BANDWIDTH_40_MHZ          1U
#define AIC_PHY_BANDWIDTH_80_MHZ          2U
#define AIC_PHY_FEATURE_BANDWIDTH_MASK     0x03000000UL
#define AIC_PHY_FEATURE_BANDWIDTH_SHIFT    24U
#define AIC_VHT_CAP_MAX_MPDU_11454         2U
#define AIC_VHT_CAP_RX_LDPC              (1UL << 4)
#define AIC_VHT_CAP_SHORT_GI_80          (1UL << 5)
#define AIC_VHT_CAP_RX_STBC_1            (1UL << 8)
#define AIC_VHT_CAP_SU_BEAMFORMEE        (1UL << 12)
#define AIC_VHT_CAP_BEAMFORMEE_STS_4     (3UL << 13)
#define AIC_VHT_CAP_MU_BEAMFORMEE        (1UL << 20)
#define AIC_VHT_CAP_MAX_AMPDU_EXPONENT   (7UL << 23)
#define AIC_TX_LIFETIME_MS             1000U

/* Defaults produced by rwnx_set_he_capa() in the vendor Linux driver for a
 * one-stream D81 radio with 40/80 MHz enabled. */
static const rt_uint8_t g_aic_he_mac_capability[6] = {
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
};

static const rt_uint8_t g_aic_he_phy_capability[11] = {
    0x06, 0xe0, 0x2b, 0x58, 0x0d, 0xc0,
    0xcf, 0x04, 0x02, 0x30, 0x00,
};

static const rt_uint8_t g_aic_he_ppe_thresholds[25] = {
    0x38, 0x1c, 0xc7, 0x01,
};

static const struct aic_wire_tx_power_v2 g_aic_tx_power_v2 = {
    .enable = 1,
    .legacy_2ghz = {
        20, 20, 20, 20, 20, 20, 20, 20, 18, 18, 16, 16,
    },
    .ht_vht_2ghz = {20, 20, 20, 20, 18, 18, 16, 16, 16, 16},
    .he_2ghz = {
        20, 20, 20, 20, 18, 18, 16, 16, 16, 16, 15, 15,
    },
};

static const struct aic_wire_tx_power_v3 g_aic_tx_power_v3 = {
    .enable = 1,
    .legacy_2ghz = {
        20, 20, 20, 20, 20, 20, 20, 20, 18, 18, 16, 16,
    },
    .ht_vht_2ghz = {20, 20, 20, 20, 18, 18, 16, 16, 16, 16},
    .he_2ghz = {
        20, 20, 20, 20, 18, 18, 16, 16, 16, 16, 15, 15,
    },
    .legacy_5ghz = {
        -128, -128, -128, -128, 20, 20, 20, 20, 18, 18, 16, 16,
    },
    .ht_vht_5ghz = {20, 20, 20, 20, 18, 18, 16, 16, 16, 15},
    .he_5ghz = {
        20, 20, 20, 20, 18, 18, 16, 16, 16, 15, 14, 14,
    },
};

static const struct aic_wire_tx_power_v4 g_aic_tx_power_v4 = {
    .enable = 1,
    .legacy_2ghz = {
        20, 20, 20, 20, 20, 20, 20, 20, 18, 18, 16, 16,
    },
    .ht_vht_2ghz = {20, 20, 20, 20, 18, 18, 16, 16, 16, 16},
    .he_2ghz = {
        20, 20, 20, 20, 18, 18, 16, 16, 16, 16, 15, 15,
    },
    .legacy_5ghz = {20, 20, 20, 20, 18, 18, 16, 16},
    .ht_vht_5ghz = {20, 20, 20, 20, 18, 18, 16, 16, 16, 15},
    .he_5ghz = {
        20, 20, 20, 20, 18, 18, 16, 16, 16, 15, 14, 14,
    },
};

/* The legacy AIC8801 command uses these defaults when no module userconfig
 * is available.  They match the Linux driver nvram_info defaults. */
static const struct aic_wire_tx_power_index g_aic_tx_power_index = {
    .enable = 1,
    .dsss = 9,
    .ofdm_low_2ghz = 8,
    .ofdm_64qam_2ghz = 8,
    .ofdm_256qam_2ghz = 8,
    .ofdm_1024qam_2ghz = 8,
    .ofdm_low_5ghz = 11,
    .ofdm_64qam_5ghz = 10,
    .ofdm_256qam_5ghz = 9,
    .ofdm_1024qam_5ghz = 9,
};

static const struct aic_wire_tx_power_offset g_aic_tx_power_offset = {
    .enable = 1,
};

static const char *const g_aic_legacy_2ghz_keys[] = {
    "lvl_11b_11ag_1m_2g4", "lvl_11b_11ag_2m_2g4",
    "lvl_11b_11ag_5m5_2g4", "lvl_11b_11ag_11m_2g4",
    "lvl_11b_11ag_6m_2g4", "lvl_11b_11ag_9m_2g4",
    "lvl_11b_11ag_12m_2g4", "lvl_11b_11ag_18m_2g4",
    "lvl_11b_11ag_24m_2g4", "lvl_11b_11ag_36m_2g4",
    "lvl_11b_11ag_48m_2g4", "lvl_11b_11ag_54m_2g4",
};

static const char *const g_aic_ht_2ghz_keys[] = {
    "lvl_11n_11ac_mcs0_2g4", "lvl_11n_11ac_mcs1_2g4",
    "lvl_11n_11ac_mcs2_2g4", "lvl_11n_11ac_mcs3_2g4",
    "lvl_11n_11ac_mcs4_2g4", "lvl_11n_11ac_mcs5_2g4",
    "lvl_11n_11ac_mcs6_2g4", "lvl_11n_11ac_mcs7_2g4",
    "lvl_11n_11ac_mcs8_2g4", "lvl_11n_11ac_mcs9_2g4",
};

static const char *const g_aic_he_2ghz_keys[] = {
    "lvl_11ax_mcs0_2g4", "lvl_11ax_mcs1_2g4",
    "lvl_11ax_mcs2_2g4", "lvl_11ax_mcs3_2g4",
    "lvl_11ax_mcs4_2g4", "lvl_11ax_mcs5_2g4",
    "lvl_11ax_mcs6_2g4", "lvl_11ax_mcs7_2g4",
    "lvl_11ax_mcs8_2g4", "lvl_11ax_mcs9_2g4",
    "lvl_11ax_mcs10_2g4", "lvl_11ax_mcs11_2g4",
};

static const char *const g_aic_legacy_5ghz_keys[] = {
    "lvl_11a_6m_5g", "lvl_11a_9m_5g", "lvl_11a_12m_5g",
    "lvl_11a_18m_5g", "lvl_11a_24m_5g", "lvl_11a_36m_5g",
    "lvl_11a_48m_5g", "lvl_11a_54m_5g",
};

static const char *const g_aic_ht_5ghz_keys[] = {
    "lvl_11n_11ac_mcs0_5g", "lvl_11n_11ac_mcs1_5g",
    "lvl_11n_11ac_mcs2_5g", "lvl_11n_11ac_mcs3_5g",
    "lvl_11n_11ac_mcs4_5g", "lvl_11n_11ac_mcs5_5g",
    "lvl_11n_11ac_mcs6_5g", "lvl_11n_11ac_mcs7_5g",
    "lvl_11n_11ac_mcs8_5g", "lvl_11n_11ac_mcs9_5g",
};

static const char *const g_aic_he_5ghz_keys[] = {
    "lvl_11ax_mcs0_5g", "lvl_11ax_mcs1_5g",
    "lvl_11ax_mcs2_5g", "lvl_11ax_mcs3_5g",
    "lvl_11ax_mcs4_5g", "lvl_11ax_mcs5_5g",
    "lvl_11ax_mcs6_5g", "lvl_11ax_mcs7_5g",
    "lvl_11ax_mcs8_5g", "lvl_11ax_mcs9_5g",
    "lvl_11ax_mcs10_5g", "lvl_11ax_mcs11_5g",
};

static const rt_uint16_t g_aic_powerlimit_5ghz_channels[25] = {
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120,
    124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165,
};

static rt_bool_t aic_radio_parse_number(const char *text, rt_int32_t *value);
static rt_int8_t aic_radio_clamp_power(rt_int32_t value);

static void aic_radio_set_country_code(char destination[3], const char *source)
{
    destination[0] = 'C';
    destination[1] = 'N';
    destination[2] = '\0';
    if (source && source[0] && source[1])
    {
        char first = source[0] >= 'a' && source[0] <= 'z' ?
                     source[0] - ('a' - 'A') : source[0];
        char second = source[1] >= 'a' && source[1] <= 'z' ?
                      source[1] - ('a' - 'A') : source[1];

        if ((first == 'C' && second == 'N') ||
            (first == 'U' && second == 'S'))
        {
            destination[0] = first;
            destination[1] = second;
        }
    }
}

static rt_country_code_t aic_radio_country_from_code(const char code[3])
{
    if (code && code[0] == 'C' && code[1] == 'N')
    {
        return RT_COUNTRY_CHINA;
    }
    if (code && code[0] == 'U' && code[1] == 'S')
    {
        return RT_COUNTRY_UNITED_STATES;
    }
    return RT_COUNTRY_UNKNOWN;
}

static void aic_radio_init_powerlimit(
    struct aic8800_radio_config_state *config)
{
    rt_size_t index;

    for (index = 0; index < sizeof(config->channel_power_2ghz); index++)
    {
        config->channel_power_2ghz[index] = 20;
    }
    for (index = 0; index < sizeof(config->channel_power_5ghz); index++)
    {
        config->channel_power_5ghz[index] = 20;
    }
    config->channel_valid_2ghz = (1U << 14) - 1U;
    config->channel_valid_5ghz = (1U << 25) - 1U;
    config->powerlimit_loaded = RT_FALSE;
}

static char *aic_radio_next_token(char **cursor)
{
    char *start;

    if (!cursor || !*cursor)
    {
        return RT_NULL;
    }
    start = *cursor;
    while (*start == ' ' || *start == '\t' || *start == '\r')
    {
        start++;
    }
    if (!*start || *start == '#')
    {
        *cursor = start;
        return RT_NULL;
    }
    *cursor = start;
    while (**cursor && **cursor != ' ' && **cursor != '\t' &&
           **cursor != '\r')
    {
        (*cursor)++;
    }
    if (**cursor)
    {
        **cursor = '\0';
        (*cursor)++;
    }
    return start;
}

static rt_bool_t aic_radio_parse_power_token(const char *token,
                                             rt_int8_t *power,
                                             rt_bool_t *available)
{
    rt_int32_t parsed;

    if (!token || !power || !available)
    {
        return RT_FALSE;
    }
    if (!strcmp(token, "NA") || !strcmp(token, "na"))
    {
        *power = -128;
        *available = RT_FALSE;
        return RT_TRUE;
    }
    if (!aic_radio_parse_number(token, &parsed))
    {
        return RT_FALSE;
    }
    *power = aic_radio_clamp_power(parsed);
    *available = RT_TRUE;
    return RT_TRUE;
}

static rt_bool_t aic_radio_parse_powerlimit_line(
    struct aic8800_radio_config_state *config, char *line)
{
    char *cursor = line;
    char *channel_token;
    char *first_value;
    char *second_value;
    rt_int32_t channel_number;
    rt_int8_t power;
    rt_bool_t available;
    rt_size_t index;
    rt_bool_t use_second_column = config->country_code[0] == 'U' &&
                                  config->country_code[1] == 'S';

    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')
    {
        cursor++;
    }
    if (!*cursor || *cursor == '#')
    {
        return RT_FALSE;
    }
    channel_token = aic_radio_next_token(&cursor);
    if (!channel_token ||
        (channel_token[0] != 'C' && channel_token[0] != 'c') ||
        (channel_token[1] != 'H' && channel_token[1] != 'h') ||
        !aic_radio_parse_number(channel_token + 2, &channel_number))
    {
        return RT_FALSE;
    }
    first_value = aic_radio_next_token(&cursor);
    second_value = aic_radio_next_token(&cursor);
    if (!first_value || !second_value)
    {
        return RT_FALSE;
    }
    if (!aic_radio_parse_power_token(use_second_column ? second_value :
                                     first_value, &power, &available))
    {
        return RT_FALSE;
    }
    if (channel_number >= 1 && channel_number <= 14)
    {
        index = (rt_size_t)channel_number - 1U;
        config->channel_power_2ghz[index] = power;
        if (available)
        {
            config->channel_valid_2ghz |= 1U << index;
        }
        else
        {
            config->channel_valid_2ghz &= ~(1U << index);
        }
        return RT_TRUE;
    }
    for (index = 0; index < sizeof(g_aic_powerlimit_5ghz_channels) /
                           sizeof(g_aic_powerlimit_5ghz_channels[0]); index++)
    {
        if (g_aic_powerlimit_5ghz_channels[index] == channel_number)
        {
            config->channel_power_5ghz[index] = power;
            if (available)
            {
                config->channel_valid_5ghz |= 1U << index;
            }
            else
            {
                config->channel_valid_5ghz &= ~(1U << index);
            }
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static void aic_radio_load_powerlimit(
    struct aic8800_context *context, struct aic8800_radio_config_state *config,
    const char *directory, const char *filename)
{
    char path[256];
    char line[192];
    rt_size_t line_length = 0;
    rt_size_t parsed_lines = 0;
    int descriptor;
    char character;
    ssize_t length;

    descriptor = aic8800_firmware_open_file(
        context, directory, filename, path, sizeof(path));
    if (descriptor < 0)
    {
        config->powerlimit_loaded = RT_TRUE;
        LOG_W("power-limit file not found: %s; using 20 dBm defaults", path);
        return;
    }
    config->channel_valid_2ghz = 0;
    config->channel_valid_5ghz = 0;
    while ((length = read(descriptor, &character, 1)) == 1)
    {
        if (character == '\n')
        {
            line[line_length] = '\0';
            if (aic_radio_parse_powerlimit_line(config, line))
            {
                parsed_lines++;
            }
            line_length = 0;
        }
        else if (line_length + 1U < sizeof(line))
        {
            line[line_length++] = character;
        }
        else
        {
            line_length = 0;
        }
    }
    if (line_length)
    {
        line[line_length] = '\0';
        if (aic_radio_parse_powerlimit_line(config, line))
        {
            parsed_lines++;
        }
    }
    close(descriptor);
    if (length < 0)
    {
        LOG_W("power-limit read failed: %s", path);
    }
    if (!parsed_lines)
    {
        aic_radio_init_powerlimit(config);
        LOG_W("power-limit file has no usable rows: %s", path);
    }
    else
    {
        config->powerlimit_loaded = RT_TRUE;
        LOG_I("loaded power limits: %s country=%s rows=%u", path,
              config->country_code, (unsigned int)parsed_lines);
    }
    (void)context;
}

static rt_err_t aic_radio_reload_powerlimit(struct aic8800_context *context)
{
    const char *directory;
    const char *filename;
    struct aic8800_radio_config_state *config;

    if (!context)
    {
        return -RT_EINVAL;
    }
    config = &context->radio_config;
    if (context->product_id == AIC8800_USB_PID_AIC8800D80X2 ||
        context->product_id == AIC8800_USB_PID_AIC8800D81X2 ||
        context->product_id == AIC8800_USB_PID_AIC8800D89X2)
    {
        directory = "aic8800D80X2";
        filename = "aic_powerlimit_8800d80x2.txt";
    }
    else if (context->product_id == AIC8800_USB_PID_AIC8800D80 ||
             context->product_id == AIC8800_USB_PID_AIC8800D81 ||
             context->product_id == AIC8800_USB_PID_AIC8800D40 ||
             context->product_id == AIC8800_USB_PID_AIC8800D41)
    {
        directory = "aic8800D80";
        filename = "aic_powerlimit_8800d80.txt";
    }
    else if (context->product_id == AIC8800_USB_PID_AIC8801)
    {
        /* AIC8801 has no Linux power-limit file.  Its regulatory limits are
         * provided by the legacy firmware defaults/regulatory database. */
        aic_radio_init_powerlimit(config);
        return RT_EOK;
    }
    else if (context->product_id == AIC8800_USB_PID_AIC8800DC ||
             context->product_id == AIC8800_USB_PID_AIC8800DW)
    {
        directory = "aic8800DC";
        filename = context->product_id == AIC8800_USB_PID_AIC8800DC ?
                   "aic_powerlimit_8800dc.txt" :
                   "aic_powerlimit_8800dw.txt";
    }
    else
    {
        return -RT_ENOSYS;
    }
    aic_radio_init_powerlimit(config);
    aic_radio_load_powerlimit(context, config, directory, filename);
    return RT_EOK;
}

static rt_bool_t aic_radio_parse_number(const char *text, rt_int32_t *value)
{
    rt_int32_t sign = 1;
    rt_int32_t result = 0;
    rt_bool_t have_digit = RT_FALSE;

    if (!text || !value)
    {
        return RT_FALSE;
    }
    while (*text == ' ' || *text == '\t')
    {
        text++;
    }
    if (*text == '+' || *text == '-')
    {
        sign = *text++ == '-' ? -1 : 1;
    }
    while (*text >= '0' && *text <= '9')
    {
        have_digit = RT_TRUE;
        if (result > 100000000)
        {
            return RT_FALSE;
        }
        result = result * 10 + (*text++ - '0');
    }
    while (*text == ' ' || *text == '\t' || *text == '\r')
    {
        text++;
    }
    if (*text || !have_digit)
    {
        return RT_FALSE;
    }
    *value = sign * result;
    return RT_TRUE;
}

static rt_int8_t aic_radio_clamp_power(rt_int32_t value)
{
    if (value < -128)
    {
        value = -128;
    }
    else if (value > 127)
    {
        value = 127;
    }
    return (rt_int8_t)value;
}

static rt_int8_t aic_radio_add_power(rt_int8_t value, rt_int8_t offset)
{
    /* -128 is the vendor sentinel for an unavailable rate. */
    if (value == -128)
    {
        return value;
    }
    return aic_radio_clamp_power((rt_int32_t)value + offset);
}

static rt_bool_t aic_radio_set_power_key(
    struct aic8800_radio_config_state *config, const char *key,
    rt_int8_t value)
{
    rt_size_t index;

    for (index = 0; index < sizeof(g_aic_legacy_2ghz_keys) /
                         sizeof(g_aic_legacy_2ghz_keys[0]); index++)
    {
        if (!strcmp(key, g_aic_legacy_2ghz_keys[index]))
        {
            config->tx_power_v2.legacy_2ghz[index] = value;
            config->tx_power_v3.legacy_2ghz[index] = value;
            config->tx_power_v4.legacy_2ghz[index] = value;
            return RT_TRUE;
        }
    }
    for (index = 0; index < sizeof(g_aic_ht_2ghz_keys) /
                         sizeof(g_aic_ht_2ghz_keys[0]); index++)
    {
        if (!strcmp(key, g_aic_ht_2ghz_keys[index]))
        {
            config->tx_power_v2.ht_vht_2ghz[index] = value;
            config->tx_power_v3.ht_vht_2ghz[index] = value;
            config->tx_power_v4.ht_vht_2ghz[index] = value;
            return RT_TRUE;
        }
    }
    for (index = 0; index < sizeof(g_aic_he_2ghz_keys) /
                         sizeof(g_aic_he_2ghz_keys[0]); index++)
    {
        if (!strcmp(key, g_aic_he_2ghz_keys[index]))
        {
            config->tx_power_v2.he_2ghz[index] = value;
            config->tx_power_v3.he_2ghz[index] = value;
            config->tx_power_v4.he_2ghz[index] = value;
            return RT_TRUE;
        }
    }
    for (index = 0; index < sizeof(g_aic_legacy_5ghz_keys) /
                         sizeof(g_aic_legacy_5ghz_keys[0]); index++)
    {
        if (!strcmp(key, g_aic_legacy_5ghz_keys[index]))
        {
            /* The v3 wire format retains four reserved entries before the
             * 6 Mbps 5 GHz legacy rate; v4 starts at 6 Mbps. */
            config->tx_power_v3.legacy_5ghz[index + 4U] = value;
            if (index < sizeof(config->tx_power_v4.legacy_5ghz))
            {
                config->tx_power_v4.legacy_5ghz[index] = value;
            }
            return RT_TRUE;
        }
    }
    for (index = 0; index < sizeof(g_aic_ht_5ghz_keys) /
                         sizeof(g_aic_ht_5ghz_keys[0]); index++)
    {
        if (!strcmp(key, g_aic_ht_5ghz_keys[index]))
        {
            config->tx_power_v3.ht_vht_5ghz[index] = value;
            config->tx_power_v4.ht_vht_5ghz[index] = value;
            return RT_TRUE;
        }
    }
    for (index = 0; index < sizeof(g_aic_he_5ghz_keys) /
                         sizeof(g_aic_he_5ghz_keys[0]); index++)
    {
        if (!strcmp(key, g_aic_he_5ghz_keys[index]))
        {
            config->tx_power_v3.he_5ghz[index] = value;
            config->tx_power_v4.he_5ghz[index] = value;
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static rt_bool_t aic_radio_set_offset_key(
    struct aic8800_radio_config_state *config, const char *key,
    rt_int8_t value)
{
    static const char *const groups_2ghz[] = {"1_4", "5_9", "10_13"};
    static const char *const groups_5ghz[] = {
        "42", "58", "106", "122", "138", "155"};
    rt_size_t group;
    char candidate[64];

    for (group = 0; group < 3; group++)
    {
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_2g4_11b_chan_%s", groups_2ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x.offsets_2ghz[0][group] = value;
            config->tx_power_offset_2x_v2.offsets_2ghz_ant0[group][0] = value;
            config->tx_power_offset_2x_v2.offsets_2ghz_ant1[group][0] = value;
            return RT_TRUE;
        }
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_2g4_ofdm_highrate_chan_%s", groups_2ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x.offsets_2ghz[1][group] = value;
            config->tx_power_offset_2x_v2.offsets_2ghz_ant0[group][1] = value;
            config->tx_power_offset_2x_v2.offsets_2ghz_ant1[group][1] = value;
            return RT_TRUE;
        }
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_2g4_ofdm_lowrate_chan_%s", groups_2ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x.offsets_2ghz[2][group] = value;
            return RT_TRUE;
        }
    }
    for (group = 0; group < 6; group++)
    {
        const char *suffix = groups_5ghz[group];

        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_5g_ofdm_lowrate_chan_%s", suffix);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x.offsets_5ghz[0][group] = value;
            return RT_TRUE;
        }
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_5g_ofdm_highrate_chan_%s", suffix);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x.offsets_5ghz[1][group] = value;
            config->tx_power_offset_2x_v2.offsets_5ghz_ant0[group][0] = value;
            config->tx_power_offset_2x_v2.offsets_5ghz_ant1[group][0] = value;
            return RT_TRUE;
        }
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_5g_ofdm_midrate_chan_%s", suffix);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x.offsets_5ghz[2][group] = value;
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static rt_bool_t aic_radio_set_offset_v2_key(
    struct aic8800_radio_config_state *config, const char *key,
    rt_int8_t value)
{
    static const char *const groups_2ghz[] = {"1_4", "5_9", "10_13"};
    static const char *const groups_5ghz[] = {
        "42", "58", "106", "122", "138", "155"};
    rt_size_t group;
    char candidate[80];

    for (group = 0; group < 3; group++)
    {
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_2g4_ant0_11b_chan_%s", groups_2ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x_v2.offsets_2ghz_ant0[group][0] = value;
            return RT_TRUE;
        }
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_2g4_ant1_11b_chan_%s", groups_2ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x_v2.offsets_2ghz_ant1[group][0] = value;
            return RT_TRUE;
        }
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_2g4_ant0_ofdm_highrate_chan_%s", groups_2ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x_v2.offsets_2ghz_ant0[group][1] = value;
            return RT_TRUE;
        }
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_2g4_ant1_ofdm_highrate_chan_%s", groups_2ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x_v2.offsets_2ghz_ant1[group][1] = value;
            return RT_TRUE;
        }
    }
    for (group = 0; group < 6; group++)
    {
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_5g_ant0_ofdm_highrate_chan_%s", groups_5ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x_v2.offsets_5ghz_ant0[group][0] = value;
            return RT_TRUE;
        }
        rt_snprintf(candidate, sizeof(candidate),
                    "ofst_5g_ant1_ofdm_highrate_chan_%s", groups_5ghz[group]);
        if (!strcmp(key, candidate))
        {
            config->tx_power_offset_2x_v2.offsets_5ghz_ant1[group][0] = value;
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static rt_bool_t aic_radio_set_legacy_key(
    struct aic8800_radio_config_state *config, const char *key,
    rt_int8_t value, rt_int32_t parsed)
{
    const char *name = key;

    /* SDIO AIC8801 files select the active module with a prefix.  The Linux
     * driver uses module0 for the Wi-Fi path; module1 belongs to the other
     * module and must not overwrite the Wi-Fi command. */
    if (!strncmp(name, "module0_", 8))
    {
        name += 8;
    }
    else if (!strncmp(name, "module1_", 8))
    {
        return RT_FALSE;
    }

    if (!strcmp(name, "dsss"))
    {
        config->tx_power_index.dsss = value;
    }
    else if (!strcmp(name, "ofdmlowrate_2g4"))
    {
        config->tx_power_index.ofdm_low_2ghz = value;
    }
    else if (!strcmp(name, "ofdm64qam_2g4"))
    {
        config->tx_power_index.ofdm_64qam_2ghz = value;
    }
    else if (!strcmp(name, "ofdm256qam_2g4"))
    {
        config->tx_power_index.ofdm_256qam_2ghz = value;
    }
    else if (!strcmp(name, "ofdm1024qam_2g4"))
    {
        config->tx_power_index.ofdm_1024qam_2ghz = value;
    }
    else if (!strcmp(name, "ofdmlowrate_5g"))
    {
        config->tx_power_index.ofdm_low_5ghz = value;
    }
    else if (!strcmp(name, "ofdm64qam_5g"))
    {
        config->tx_power_index.ofdm_64qam_5ghz = value;
    }
    else if (!strcmp(name, "ofdm256qam_5g"))
    {
        config->tx_power_index.ofdm_256qam_5ghz = value;
    }
    else if (!strcmp(name, "ofdm1024qam_5g"))
    {
        config->tx_power_index.ofdm_1024qam_5ghz = value;
    }
    else if (!strcmp(name, "ofst_chan_1_4"))
    {
        config->tx_power_offset.channels_1_4 = value;
    }
    else if (!strcmp(name, "ofst_chan_5_9"))
    {
        config->tx_power_offset.channels_5_9 = value;
    }
    else if (!strcmp(name, "ofst_chan_10_13"))
    {
        config->tx_power_offset.channels_10_13 = value;
    }
    else if (!strcmp(name, "ofst_chan_36_64"))
    {
        config->tx_power_offset.channels_36_64 = value;
    }
    else if (!strcmp(name, "ofst_chan_100_120"))
    {
        config->tx_power_offset.channels_100_120 = value;
    }
    else if (!strcmp(name, "ofst_chan_122_140"))
    {
        config->tx_power_offset.channels_122_140 = value;
    }
    else if (!strcmp(name, "ofst_chan_142_165"))
    {
        config->tx_power_offset.channels_142_165 = value;
    }
    else if (!strcmp(name, "enable"))
    {
        config->tx_power_index.enable = parsed != 0;
    }
    else if (!strcmp(name, "ofst_enable"))
    {
        config->tx_power_offset.enable = parsed != 0;
    }
    else if (!strcmp(name, "xtal_enable"))
    {
        config->crystal_enabled = parsed != 0;
    }
    else if (!strcmp(name, "xtal_cap"))
    {
        config->crystal_capacitance = (rt_uint8_t)value;
    }
    else if (!strcmp(name, "xtal_cap_fine"))
    {
        config->crystal_capacitance_fine = (rt_uint8_t)value;
    }
    else
    {
        return RT_FALSE;
    }
    return RT_TRUE;
}

static void aic_radio_apply_loss(struct aic8800_radio_config_state *config)
{
    rt_size_t index;

    if (!config->loss_enabled || !config->loss_value)
    {
        return;
    }
    for (index = 0; index < sizeof(config->tx_power_v3.legacy_2ghz); index++)
    {
        config->tx_power_v2.legacy_2ghz[index] = aic_radio_add_power(
            config->tx_power_v2.legacy_2ghz[index], config->loss_value);
        config->tx_power_v3.legacy_2ghz[index] = aic_radio_add_power(
            config->tx_power_v3.legacy_2ghz[index], config->loss_value);
        config->tx_power_v4.legacy_2ghz[index] = aic_radio_add_power(
            config->tx_power_v4.legacy_2ghz[index], config->loss_value);
    }
    for (index = 0; index < sizeof(config->tx_power_v3.ht_vht_2ghz); index++)
    {
        config->tx_power_v2.ht_vht_2ghz[index] = aic_radio_add_power(
            config->tx_power_v2.ht_vht_2ghz[index], config->loss_value);
        config->tx_power_v3.ht_vht_2ghz[index] = aic_radio_add_power(
            config->tx_power_v3.ht_vht_2ghz[index], config->loss_value);
        config->tx_power_v4.ht_vht_2ghz[index] = aic_radio_add_power(
            config->tx_power_v4.ht_vht_2ghz[index], config->loss_value);
    }
    for (index = 0; index < sizeof(config->tx_power_v3.he_2ghz); index++)
    {
        config->tx_power_v2.he_2ghz[index] = aic_radio_add_power(
            config->tx_power_v2.he_2ghz[index], config->loss_value);
        config->tx_power_v3.he_2ghz[index] = aic_radio_add_power(
            config->tx_power_v3.he_2ghz[index], config->loss_value);
        config->tx_power_v4.he_2ghz[index] = aic_radio_add_power(
            config->tx_power_v4.he_2ghz[index], config->loss_value);
    }
    for (index = 0; index < sizeof(config->tx_power_v3.legacy_5ghz); index++)
    {
        if (config->tx_power_v3.legacy_5ghz[index] != -128)
        {
            config->tx_power_v3.legacy_5ghz[index] = aic_radio_add_power(
                config->tx_power_v3.legacy_5ghz[index], config->loss_value);
        }
        if (index < sizeof(config->tx_power_v4.legacy_5ghz))
        {
            if (config->tx_power_v4.legacy_5ghz[index] != -128)
            {
                config->tx_power_v4.legacy_5ghz[index] = aic_radio_add_power(
                    config->tx_power_v4.legacy_5ghz[index],
                    config->loss_value);
            }
        }
    }
    for (index = 0; index < sizeof(config->tx_power_v3.ht_vht_5ghz); index++)
    {
        config->tx_power_v3.ht_vht_5ghz[index] = aic_radio_add_power(
            config->tx_power_v3.ht_vht_5ghz[index], config->loss_value);
        config->tx_power_v4.ht_vht_5ghz[index] = aic_radio_add_power(
            config->tx_power_v4.ht_vht_5ghz[index], config->loss_value);
    }
    for (index = 0; index < sizeof(config->tx_power_v3.he_5ghz); index++)
    {
        config->tx_power_v3.he_5ghz[index] = aic_radio_add_power(
            config->tx_power_v3.he_5ghz[index], config->loss_value);
        config->tx_power_v4.he_5ghz[index] = aic_radio_add_power(
            config->tx_power_v4.he_5ghz[index], config->loss_value);
    }
}

static void aic_radio_parse_line(struct aic8800_radio_config_state *config,
                                 char *line)
{
    char *key;
    char *value;
    char *end;
    rt_int32_t parsed;
    rt_int8_t power;

    while (*line == ' ' || *line == '\t')
    {
        line++;
    }
    if (!*line || *line == '#')
    {
        return;
    }
    key = line;
    value = strchr(line, '=');
    if (!value)
    {
        return;
    }
    *value++ = '\0';
    end = key + strlen(key);
    while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
    {
        *--end = '\0';
    }
    while (*value == ' ' || *value == '\t')
    {
        value++;
    }
    if (!strcmp(value, "NA") || !strcmp(value, "na"))
    {
        return;
    }
    if (!aic_radio_parse_number(value, &parsed))
    {
        return;
    }
    power = aic_radio_clamp_power(parsed);
    if (aic_radio_set_power_key(config, key, power) ||
        aic_radio_set_offset_key(config, key, power) ||
        aic_radio_set_offset_v2_key(config, key, power))
    {
        return;
    }
    if (!strcmp(key, "enable"))
    {
        config->tx_power_v2.enable = parsed != 0;
        config->tx_power_v3.enable = parsed != 0;
        config->tx_power_v4.enable = parsed != 0;
        config->tx_power_index.enable = parsed != 0;
    }
    else if (!strcmp(key, "lvl_adj_enable"))
    {
        config->tx_power_adjust.enable = parsed != 0;
    }
    else if (!strcmp(key, "lvl_adj_2g4_chan_1_4"))
    {
        config->tx_power_adjust.adjustment_2ghz[0] = power;
    }
    else if (!strcmp(key, "lvl_adj_2g4_chan_5_9"))
    {
        config->tx_power_adjust.adjustment_2ghz[1] = power;
    }
    else if (!strcmp(key, "lvl_adj_2g4_chan_10_13"))
    {
        config->tx_power_adjust.adjustment_2ghz[2] = power;
    }
    else if (!strncmp(key, "lvl_adj_5g_chan_", 16))
    {
        static const char *const group_names[] = {
            "42", "58", "106", "122", "138", "155"};
        rt_size_t index;

        for (index = 0; index < sizeof(group_names) /
                             sizeof(group_names[0]); index++)
        {
            char candidate[32];

            rt_snprintf(candidate, sizeof(candidate), "lvl_adj_5g_chan_%s",
                        group_names[index]);
            if (!strcmp(key, candidate))
            {
                config->tx_power_adjust.adjustment_5ghz[index] = power;
                break;
            }
        }
    }
    else if (!strcmp(key, "loss_enable"))
    {
        config->loss_enabled = parsed != 0;
    }
    else if (!strcmp(key, "loss_value"))
    {
        config->loss_value = power;
    }
    else if (!strcmp(key, "ofst_enable"))
    {
        config->tx_power_offset_2x.enable = parsed != 0;
        config->tx_power_offset_2x_v2.enable = parsed != 0;
        config->tx_power_offset.enable = parsed != 0;
    }
    else if (!strcmp(key, "xtal_enable"))
    {
        config->crystal_enabled = parsed != 0;
    }
    else if (!strcmp(key, "xtal_cap"))
    {
        config->crystal_capacitance = (rt_uint8_t)power;
    }
    else if (!strcmp(key, "xtal_cap_fine"))
    {
        config->crystal_capacitance_fine = (rt_uint8_t)power;
    }
    else
    {
        aic_radio_set_legacy_key(config, key, power, parsed);
    }
}

rt_err_t aic8800_radio_load_config(struct aic8800_context *context)
{
    struct aic8800_radio_config_state *config;
    const char *directory;
    const char *filename;
    const char *powerlimit_filename;
    char path[256];
    char line[192];
    rt_size_t line_length = 0;
    int descriptor;
    char character;
    ssize_t length;

    if (!context)
    {
        return -RT_EINVAL;
    }
    config = &context->radio_config;
    rt_memset(config, 0, sizeof(*config));
    config->tx_power_index = g_aic_tx_power_index;
    config->tx_power_offset = g_aic_tx_power_offset;
    config->tx_power_v2 = g_aic_tx_power_v2;
    config->tx_power_v3 = g_aic_tx_power_v3;
    config->tx_power_v4 = g_aic_tx_power_v4;
    config->loss_enabled = RT_TRUE;
    config->loss_value = 0;
    aic_radio_set_country_code(config->country_code,
                               AIC8800_WIFI_COUNTRY_CODE);
    aic_radio_init_powerlimit(config);
    if (context->product_id == AIC8800_USB_PID_AIC8800D80X2 ||
        context->product_id == AIC8800_USB_PID_AIC8800D81X2 ||
        context->product_id == AIC8800_USB_PID_AIC8800D89X2)
    {
        directory = "aic8800D80X2";
        filename = "aic_userconfig_8800d80x2.txt";
        powerlimit_filename = "aic_powerlimit_8800d80x2.txt";
    }
    else if (context->product_id == AIC8800_USB_PID_AIC8800D80 ||
             context->product_id == AIC8800_USB_PID_AIC8800D81 ||
             context->product_id == AIC8800_USB_PID_AIC8800D40 ||
             context->product_id == AIC8800_USB_PID_AIC8800D41)
    {
        directory = "aic8800D80";
        filename = "aic_userconfig_8800d80.txt";
        powerlimit_filename = "aic_powerlimit_8800d80.txt";
    }
    else if (context->product_id == AIC8800_USB_PID_AIC8801)
    {
        if (context->transport == AIC8800_TRANSPORT_SDIO)
        {
            directory = "aic8800";
            filename = "aic_userconfig.txt";
            powerlimit_filename = RT_NULL;
        }
        else
        {
            /* The Linux USB driver keeps the legacy nvram defaults and does
             * not parse the SDIO module-prefixed userconfig on USB. */
            config->loaded = RT_TRUE;
            context->country = aic_radio_country_from_code(
                config->country_code);
            return RT_EOK;
        }
    }
    else if (context->product_id == AIC8800_USB_PID_AIC8800DC ||
             context->product_id == AIC8800_USB_PID_AIC8800DW)
    {
        directory = "aic8800DC";
        if (context->product_id == AIC8800_USB_PID_AIC8800DC)
        {
            filename = "aic_userconfig_8800dc.txt";
            powerlimit_filename = "aic_powerlimit_8800dc.txt";
        }
        else
        {
            filename = "aic_userconfig_8800dw.txt";
            powerlimit_filename = "aic_powerlimit_8800dw.txt";
        }
    }
    else
    {
        config->loaded = RT_TRUE;
        context->country = aic_radio_country_from_code(config->country_code);
        return RT_EOK;
    }
    if (powerlimit_filename)
    {
        aic_radio_load_powerlimit(context, config, directory,
                                  powerlimit_filename);
    }
    descriptor = aic8800_firmware_open_file(
        context, directory, filename, path, sizeof(path));
    if (descriptor < 0)
    {
        LOG_W("userconfig not found: %s; using built-in RF defaults", path);
        config->loaded = RT_TRUE;
        context->country = aic_radio_country_from_code(config->country_code);
        return RT_EOK;
    }
    while ((length = read(descriptor, &character, 1)) == 1)
    {
        if (character == '\n')
        {
            line[line_length] = '\0';
            aic_radio_parse_line(config, line);
            line_length = 0;
        }
        else if (line_length + 1U < sizeof(line))
        {
            line[line_length++] = character;
        }
        else
        {
            line_length = 0;
        }
    }
    if (line_length)
    {
        line[line_length] = '\0';
        aic_radio_parse_line(config, line);
    }
    close(descriptor);
    if (length < 0)
    {
        LOG_W("userconfig read failed: %s", path);
    }
    aic_radio_apply_loss(config);
    config->loaded = RT_TRUE;
    context->country = aic_radio_country_from_code(config->country_code);
    LOG_I("loaded RF userconfig: %s country=%s (offset=%u adjust=%u xtal=%u)",
          path, config->country_code, config->tx_power_offset_2x.enable,
          config->tx_power_adjust.enable,
          config->crystal_enabled);
    return RT_EOK;
}

static int aic_radio_powerlimit_index_5ghz(rt_uint16_t channel)
{
    rt_size_t index;

    for (index = 0; index < sizeof(g_aic_powerlimit_5ghz_channels) /
                           sizeof(g_aic_powerlimit_5ghz_channels[0]); index++)
    {
        if (g_aic_powerlimit_5ghz_channels[index] == channel)
        {
            return (int)index;
        }
    }
    return -1;
}

rt_bool_t aic8800_radio_channel_allowed(
    const struct aic8800_context *context, enum rt_wlan_offload_band_id band,
    rt_uint16_t channel)
{
    int index;

    if (!context || !context->radio_config.powerlimit_loaded)
    {
        return RT_TRUE;
    }
    if (band == RT_WLAN_OFFLOAD_BAND_2GHZ)
    {
        if (channel < 1 || channel > 14)
        {
            return RT_FALSE;
        }
        return (context->radio_config.channel_valid_2ghz &
                (1U << (channel - 1U))) != 0;
    }
    if (band != RT_WLAN_OFFLOAD_BAND_5GHZ)
    {
        return RT_FALSE;
    }
    index = aic_radio_powerlimit_index_5ghz(channel);
    return index >= 0 &&
           (context->radio_config.channel_valid_5ghz & (1U << index)) != 0;
}

rt_int8_t aic8800_radio_channel_power(
    const struct aic8800_context *context, enum rt_wlan_offload_band_id band,
    rt_uint16_t channel, rt_int8_t default_power)
{
    int index;

    if (!context || !context->radio_config.powerlimit_loaded)
    {
        return default_power;
    }
    if (band == RT_WLAN_OFFLOAD_BAND_2GHZ && channel >= 1 && channel <= 14)
    {
        return context->radio_config.channel_power_2ghz[channel - 1U];
    }
    if (band == RT_WLAN_OFFLOAD_BAND_5GHZ)
    {
        index = aic_radio_powerlimit_index_5ghz(channel);
        if (index >= 0)
        {
            return context->radio_config.channel_power_5ghz[index];
        }
    }
    return default_power;
}

rt_err_t aic8800_radio_set_regulatory(struct aic8800_context *context,
                                       rt_country_code_t country)
{
    char code[3];
    rt_country_code_t supported;

    if (!context)
    {
        return -RT_EINVAL;
    }
    if (country == RT_COUNTRY_CHINA)
    {
        code[0] = 'C';
        code[1] = 'N';
    }
    else if (country == RT_COUNTRY_UNITED_STATES ||
             country == RT_COUNTRY_UNITED_STATES_REV4 ||
             country == RT_COUNTRY_UNITED_STATES_NO_DFS)
    {
        code[0] = 'U';
        code[1] = 'S';
    }
    else
    {
        return -RT_ENOSYS;
    }
    code[2] = '\0';
    if (context->station_connected || context->connect_request_id ||
        context->scan_request_id)
    {
        return -RT_EBUSY;
    }
    {
        struct aic8800_radio_config_state previous = context->radio_config;

        rt_memcpy(context->radio_config.country_code, code, sizeof(code));
        if (aic_radio_reload_powerlimit(context) != RT_EOK)
        {
            context->radio_config = previous;
            return -RT_EIO;
        }
    }
    supported = aic_radio_country_from_code(code);
    context->country = supported;
    return RT_EOK;
}

rt_err_t aic8800_radio_get_regulatory(const struct aic8800_context *context,
                                       rt_country_code_t *country)
{
    if (!context || !country)
    {
        return -RT_EINVAL;
    }
    *country = context->country;
    return RT_EOK;
}

static void aic_wire_put_le16(void *destination, rt_uint16_t value)
{
    rt_uint8_t *bytes = destination;

    bytes[0] = (rt_uint8_t)value;
    bytes[1] = (rt_uint8_t)(value >> 8);
}

static void aic_wire_put_le32(void *destination, rt_uint32_t value)
{
    rt_uint8_t *bytes = destination;

    bytes[0] = (rt_uint8_t)value;
    bytes[1] = (rt_uint8_t)(value >> 8);
    bytes[2] = (rt_uint8_t)(value >> 16);
    bytes[3] = (rt_uint8_t)(value >> 24);
}

static rt_err_t aic_wire_send(struct aic8800_context *context,
                                rt_uint16_t request_id,
                                rt_uint16_t confirmation_id,
                                const void *request,
                                rt_size_t request_length)
{
    rt_err_t result = aic8800_protocol_command(
        context, request_id, confirmation_id, request, request_length,
        RT_NULL, 0, RT_NULL);

    if (result != RT_EOK)
    {
        LOG_E("firmware command 0x%04x failed: %d", request_id, result);
    }
    return result;
}

/* Linux sends the complete transport-specific union: SDIO ends at v3/2x,
 * while USB includes the later v4/2x-v2 members. */
static rt_size_t aic_wire_tx_power_v3_request_length(
    const struct aic8800_context *context)
{
    return context->transport == AIC8800_TRANSPORT_SDIO ?
           sizeof(struct aic_wire_tx_power_v3) :
           sizeof(struct aic_wire_mm_set_tx_power_req);
}

static rt_size_t aic_wire_tx_power_offset_request_length(
    const struct aic8800_context *context)
{
    return context->transport == AIC8800_TRANSPORT_SDIO ?
           sizeof(struct aic_wire_tx_power_offset_2x) :
           sizeof(struct aic_wire_mm_set_tx_power_offset_req);
}

static rt_err_t aic_wire_send_tx_power_index(
    struct aic8800_context *context)
{
    struct aic_wire_mm_set_tx_power_req request;

    rt_memset(&request, 0, sizeof(request));
    request.configuration.index = context->radio_config.tx_power_index;
    if (!request.configuration.index.enable)
    {
        return RT_EOK;
    }
    return aic_wire_send(context, AIC_MM_SET_TXPWR_REQ,
                         AIC_MM_SET_TXPWR_CFM,
                         &request.configuration.index,
                         sizeof(request.configuration.index));
}

static rt_err_t aic_wire_send_tx_power_offset(
    struct aic8800_context *context)
{
    struct aic_wire_mm_set_tx_power_offset_req request;

    rt_memset(&request, 0, sizeof(request));
    request.configuration.offset = context->radio_config.tx_power_offset;
    if (!request.configuration.offset.enable)
    {
        return RT_EOK;
    }
    return aic_wire_send(context, AIC_MM_SET_TXPWR_OFST_REQ,
                         AIC_MM_SET_TXPWR_OFST_CFM,
                         &request.configuration,
                         aic_wire_tx_power_offset_request_length(context));
}

static rt_err_t aic_wire_send_tx_power_v2(
    struct aic8800_context *context)
{
    const struct aic_wire_tx_power_v2 *power =
        &context->radio_config.tx_power_v2;
    struct aic_wire_mm_set_tx_power_req request;

    if (!power->enable)
    {
        return RT_EOK;
    }
    rt_memset(&request, 0, sizeof(request));
    if (context->chip_sub_id == 0)
    {
        request.configuration.index.enable = power->enable;
        request.configuration.index.dsss = power->legacy_2ghz[3];
        request.configuration.index.ofdm_low_2ghz = power->he_2ghz[4];
        request.configuration.index.ofdm_64qam_2ghz = power->he_2ghz[7];
        request.configuration.index.ofdm_256qam_2ghz = power->he_2ghz[9];
        request.configuration.index.ofdm_1024qam_2ghz = power->he_2ghz[11];
        request.configuration.index.ofdm_low_5ghz = 13;
        request.configuration.index.ofdm_64qam_5ghz = 13;
        request.configuration.index.ofdm_256qam_5ghz = 13;
        request.configuration.index.ofdm_1024qam_5ghz = 13;
    }
    else
    {
        request.configuration.v2 = *power;
    }
    return aic_wire_send(context, AIC_MM_SET_TXPWR_REQ,
                         AIC_MM_SET_TXPWR_CFM,
                         &request.configuration, sizeof(request));
}

static rt_err_t aic_wire_send_tx_power_v3(
    struct aic8800_context *context)
{
    struct aic_wire_mm_set_tx_power_req request;

    rt_memset(&request, 0, sizeof(request));
    request.configuration.v3 = context->radio_config.tx_power_v3;
    if (!request.configuration.v3.enable)
    {
        return RT_EOK;
    }
    return aic_wire_send(context, AIC_MM_SET_TXPWR_REQ,
                         AIC_MM_SET_TXPWR_CFM,
                         &request.configuration,
                         aic_wire_tx_power_v3_request_length(context));
}

static rt_err_t aic_wire_send_tx_power_v4(
    struct aic8800_context *context)
{
    struct aic_wire_mm_set_tx_power_req request;

    rt_memset(&request, 0, sizeof(request));
    request.configuration.v4 = context->radio_config.tx_power_v4;
    if (!request.configuration.v4.enable)
    {
        return RT_EOK;
    }
    return aic_wire_send(context, AIC_MM_SET_TXPWR_REQ,
                         AIC_MM_SET_TXPWR_CFM,
                         &request.configuration.v4,
                         sizeof(request.configuration.v4));
}

static rt_err_t aic_wire_send_tx_power_adjust(
    struct aic8800_context *context)
{
    struct aic_wire_mm_set_tx_power_adjust_req request;

    rt_memset(&request, 0, sizeof(request));
    request.adjustment = context->radio_config.tx_power_adjust;
    if (!request.adjustment.enable)
    {
        return RT_EOK;
    }
    return aic_wire_send(context, AIC_MM_SET_TXPWR_ADJ_REQ,
                         AIC_MM_SET_TXPWR_ADJ_CFM,
                         &request, sizeof(request));
}

static rt_err_t aic_wire_send_tx_power_offset_2x(
    struct aic8800_context *context)
{
    struct aic_wire_mm_set_tx_power_offset_req request;

    rt_memset(&request, 0, sizeof(request));
    request.configuration.offset_2x =
        context->radio_config.tx_power_offset_2x;
    if (!request.configuration.offset_2x.enable)
    {
        return RT_EOK;
    }
    return aic_wire_send(context, AIC_MM_SET_TXPWR_OFST_REQ,
                         AIC_MM_SET_TXPWR_OFST_CFM,
                         &request.configuration,
                         aic_wire_tx_power_offset_request_length(context));
}

static rt_err_t aic_wire_send_tx_power_offset_2x_v2(
    struct aic8800_context *context)
{
    struct aic_wire_mm_set_tx_power_offset_req request;

    rt_memset(&request, 0, sizeof(request));
    request.configuration.offset_2x_v2 =
        context->radio_config.tx_power_offset_2x_v2;
    if (!request.configuration.offset_2x_v2.enable)
    {
        return RT_EOK;
    }
    return aic_wire_send(context, AIC_MM_SET_TXPWR_OFST_REQ,
                         AIC_MM_SET_TXPWR_OFST_CFM,
                         &request.configuration.offset_2x_v2,
                         sizeof(request.configuration.offset_2x_v2));
}

static rt_err_t aic_wire_send_rf_config(
    struct aic8800_context *context, rt_uint8_t table_offset,
    rt_uint8_t table_selector, const rt_uint32_t *table,
    rt_size_t word_count)
{
    struct aic_wire_mm_set_rf_config_req request;
    rt_size_t index;

    if (!table || word_count > sizeof(request.data) / sizeof(request.data[0]))
    {
        return -RT_EINVAL;
    }
    rt_memset(&request, 0, sizeof(request));
    request.table_selector = table_selector;
    request.table_offset = table_offset;
    request.table_count = 16;
    for (index = 0; index < word_count; index++)
    {
        aic_wire_put_le32(&request.data[index], table[index]);
    }
    return aic_wire_send(context, AIC_MM_SET_RF_CONFIG_REQ,
                         AIC_MM_SET_RF_CONFIG_CFM,
                         &request, sizeof(request));
}

static rt_err_t aic_wire_send_rf_calibration(
    struct aic8800_context *context, rt_uint32_t calibration_2ghz,
    rt_uint32_t calibration_5ghz)
{
    struct aic_wire_mm_set_rf_calibration_req request;

    rt_memset(&request, 0, sizeof(request));
    aic_wire_put_le32(&request.calibration_2ghz, calibration_2ghz);
    aic_wire_put_le32(&request.calibration_5ghz, calibration_5ghz);
    aic_wire_put_le32(&request.alpha, 0x0c34c008U);
    aic_wire_put_le32(&request.bluetooth_enabled, 0);
    aic_wire_put_le32(&request.bluetooth_parameter, 0x00264203U);
    if (context->radio_config.crystal_enabled)
    {
        request.crystal_capacitance =
            context->radio_config.crystal_capacitance;
        request.crystal_capacitance_fine =
            context->radio_config.crystal_capacitance_fine;
    }
    /* Some runtime revisions acknowledge calibration with an empty CFM.
     * The vendor driver waits for the CFM but does not consume its optional
     * gain-address payload. */
    return aic_wire_send(context, AIC_MM_SET_RF_CALIB_REQ,
                         AIC_MM_SET_RF_CALIB_CFM,
                         &request, sizeof(request));
}

rt_err_t aic8800_radio_initialize(struct aic8800_context *context)
{
    rt_err_t result;

    if (!context)
    {
        return -RT_EINVAL;
    }
    if (!context->radio_config.loaded)
    {
        result = aic8800_radio_load_config(context);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    switch (context->product_id)
    {
    case AIC8800_USB_PID_AIC8801:
        result = aic_wire_send_tx_power_index(context);
        if (result == RT_EOK)
        {
            result = aic_wire_send_tx_power_offset(context);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_rf_calibration(context, 0xbfU, 0x3fU);
        }
        break;

    case AIC8800_USB_PID_AIC8800D81:
    case AIC8800_USB_PID_AIC8800D41:
        result = aic_wire_send_tx_power_v3(context);
        if (result == RT_EOK)
        {
            result = aic_wire_send_tx_power_adjust(context);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_tx_power_offset_2x(context);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_rf_calibration(context, 0x0f8fU,
                                                     0x0f0fU);
        }
        break;

    case AIC8800_USB_PID_AIC8800D81X2:
    case AIC8800_USB_PID_AIC8800D89X2:
        result = aic_wire_send_tx_power_v4(context);
        if (result == RT_EOK)
        {
            result = aic_wire_send_tx_power_adjust(context);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_tx_power_offset_2x_v2(context);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_rf_calibration(context, 0x0f8fU,
                                                     0x0f0fU);
        }
        break;

    case AIC8800_USB_PID_AIC8800DC:
    case AIC8800_USB_PID_AIC8800DW:
    {
        const rt_uint32_t *tx_gain;
        const rt_uint32_t *tx_gain_dsss;

        if ((context->chip_id & 0xc0U) == 0xc0U)
        {
            tx_gain = g_aic8800dc_tx_gain_24ghz_h;
            tx_gain_dsss = g_aic8800dc_tx_gain_24ghz_dsss_h;
        }
        else
        {
            tx_gain = g_aic8800dc_tx_gain_24ghz;
            tx_gain_dsss = g_aic8800dc_tx_gain_24ghz_dsss;
        }
        result = aic_wire_send_tx_power_v2(context);
        if (result == RT_EOK)
        {
            result = aic_wire_send_tx_power_offset(context);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_rf_config(context, 0, 1, tx_gain, 32);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_rf_config(context, 16, 1,
                                             tx_gain_dsss, 32);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_rf_config(
                context, 0, 0, g_aic8800dc_rx_gain_24ghz_20mhz, 64);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_rf_config(
                context, 32, 0, g_aic8800dc_rx_gain_24ghz_40mhz, 64);
        }
        if (result == RT_EOK)
        {
            result = aic_wire_send_rf_calibration(context, 0x0f8fU, 0);
        }
        break;
    }

    default:
        LOG_E("unsupported AIC runtime product 0x%04x", context->product_id);
        return -RT_ENOSYS;
    }
    if (result == RT_EOK)
    {
        LOG_I("RF initialized for product 0x%04x", context->product_id);
    }
    return result;
}

static rt_bool_t aic8800_radio_supports_80mhz(
    const struct aic8800_context *context)
{
    if (!context)
    {
        return RT_FALSE;
    }
#ifdef AIC8800_WIFI_USB_LIMIT_40MHZ
    if (context->transport == AIC8800_TRANSPORT_USB)
    {
        return RT_FALSE;
    }
#endif
    /* MM_VERSION reports the modem's actual maximum channel width. The
     * vendor driver has the same MDM_CHBW check, although its downstream
     * dynamic-parameter path bypasses it and assumes D81 means 80 MHz. */
    if (((context->firmware_phy_features & AIC_PHY_FEATURE_BANDWIDTH_MASK) >>
         AIC_PHY_FEATURE_BANDWIDTH_SHIFT) < AIC_PHY_BANDWIDTH_80_MHZ)
    {
        return RT_FALSE;
    }
    /* The vendor SDIO driver starts with use_80 disabled globally, then
     * explicitly enables use_80 and SGI80 for D80 hardware before sending
     * ME_CONFIG_REQ.  Identify that hardware from its stable SDIO IDs since
     * RT-Smart maps the loaded D80 firmware to the D81 runtime product ID. */
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
        return context->sdio_vendor_id == AIC8800_SDIO_VENDOR_AIC8800D80 &&
               context->sdio_product_id == AIC8800_SDIO_PRODUCT_AIC8800D80;
#else
        return RT_FALSE;
#endif
    }
    switch (context->product_id)
    {
    case AIC8800_USB_PID_AIC8800D81:
    case AIC8800_USB_PID_AIC8800D81X2:
    case AIC8800_USB_PID_AIC8800D89X2:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

void aic8800_radio_prepare(struct aic8800_context *context,
                           struct aic_wire_me_config_req *request)
{
    rt_uint16_t ht_capability = AIC_HT_CAP_LDPC |
                                AIC_HT_CAP_SUP_WIDTH_20_40 |
                                AIC_HT_CAP_SHORT_GI_20 |
                                AIC_HT_CAP_SHORT_GI_40 |
                                AIC_HT_CAP_RX_STBC_1 |
                                AIC_HT_CAP_MAX_AMSDU;

    rt_uint32_t features = context ? context->firmware_features : 0;
    rt_bool_t vht_supported = (features & AIC_FW_CAP_VHT) != 0;
    rt_bool_t he_supported = (features & AIC_FW_CAP_HE) != 0;
    rt_bool_t bandwidth_80;
    rt_uint16_t vht_highest;

    RT_ASSERT(request != RT_NULL);
    bandwidth_80 = (vht_supported || he_supported) &&
                   aic8800_radio_supports_80mhz(context);
    vht_highest = bandwidth_80 ? 390U : 180U;
    rt_memset(request, 0, sizeof(*request));
    aic_wire_put_le16(&request->ht.capability, ht_capability);
    request->ht.ampdu_parameters =
        AIC_HT_AMPDU_MAX_64K | (AIC_HT_AMPDU_DENSITY_16_US << 2);
    request->ht.mcs_rate[0] = 0xff;
    request->ht.mcs_rate[4] = 0x01; /* MCS32 for 20/40 MHz operation. */
    aic_wire_put_le16(&request->ht.mcs_rate[10], 150U);
    request->ht.mcs_rate[12] = AIC_HT_MCS_TX_DEFINED;
    aic_wire_put_le16(&request->tx_lifetime, AIC_TX_LIFETIME_MS);
    request->max_bandwidth = bandwidth_80 ? AIC_PHY_BANDWIDTH_80_MHZ :
                             AIC_PHY_BANDWIDTH_40_MHZ;
    request->ht_supported = 1;
    if (vht_supported)
    {
        /* One spatial stream, MCS 0-9 up to the configured width.  The
         * remaining VHT spatial streams are unsupported. */
        request->vht.capability = AIC_VHT_CAP_MAX_MPDU_11454 |
                                   AIC_VHT_CAP_RX_LDPC |
                                   AIC_VHT_CAP_RX_STBC_1 |
                                   AIC_VHT_CAP_MAX_AMPDU_EXPONENT;
        if (bandwidth_80)
        {
            request->vht.capability |= AIC_VHT_CAP_SHORT_GI_80;
        }
        if (features & AIC_FW_CAP_BFMEE)
        {
            request->vht.capability |= AIC_VHT_CAP_SU_BEAMFORMEE |
                                       AIC_VHT_CAP_BEAMFORMEE_STS_4;
        }
        if (features & AIC_FW_CAP_MU_MIMO_RX)
        {
            request->vht.capability |= AIC_VHT_CAP_MU_BEAMFORMEE;
        }
        aic_wire_put_le16(&request->vht.rx_mcs_map, 0xfffEU);
        aic_wire_put_le16(&request->vht.tx_mcs_map, 0xfffEU);
        aic_wire_put_le16(&request->vht.rx_highest, vht_highest);
        aic_wire_put_le16(&request->vht.tx_highest, vht_highest);
        request->vht_supported = 1;
    }
    if (he_supported)
    {
        /* Bits are two per spatial stream. This driver configures one NSS,
         * so stream 1 is supported and streams 2-8 must remain 3 (not
         * supported). The vendor template starts at 0xfffa, but its dynamic
         * parameter pass rewrites that value to these one-stream maps. */
        rt_uint16_t mcs_80 = 0xfffdU;

        rt_memcpy(request->he.mac_capability, g_aic_he_mac_capability,
                  sizeof(g_aic_he_mac_capability));
        rt_memcpy(request->he.phy_capability, g_aic_he_phy_capability,
                  sizeof(g_aic_he_phy_capability));
        rt_memcpy(request->he.ppe_thresholds, g_aic_he_ppe_thresholds,
                  sizeof(g_aic_he_ppe_thresholds));
        if (!bandwidth_80)
        {
            request->he.ppe_thresholds[0] &= (rt_uint8_t)~0x20U;
            request->he.ppe_thresholds[2] &= (rt_uint8_t)~0xc0U;
            request->he.ppe_thresholds[3] &= (rt_uint8_t)~0x01U;
        }
        if (context &&
            (context->product_id == AIC8800_USB_PID_AIC8800D81 ||
             context->product_id == AIC8800_USB_PID_AIC8800D81X2 ||
             context->product_id == AIC8800_USB_PID_AIC8800D89X2 ||
             (context->transport == AIC8800_TRANSPORT_SDIO &&
              context->product_id == AIC8800_USB_PID_AIC8800D80)))
        {
            mcs_80 = 0xfffeU;
        }
        if (context &&
            (context->product_id == AIC8800_USB_PID_AIC8800D81X2 ||
             context->product_id == AIC8800_USB_PID_AIC8800D89X2))
        {
            request->he.phy_capability[2] |= 0x04U;
        }
        aic_wire_put_le16(&request->he.mcs.rx_mcs_80, mcs_80);
        aic_wire_put_le16(&request->he.mcs.tx_mcs_80, mcs_80);
        aic_wire_put_le16(&request->he.mcs.rx_mcs_160, 0xffffU);
        aic_wire_put_le16(&request->he.mcs.tx_mcs_160, 0xffffU);
        aic_wire_put_le16(&request->he.mcs.rx_mcs_80p80, 0xffffU);
        aic_wire_put_le16(&request->he.mcs.tx_mcs_80p80, 0xffffU);
        request->he_supported = 1;
    }
#ifdef AIC8800_WIFI_POWER_SAVE
    request->power_save_enabled = (features & AIC_FW_CAP_PS) != 0;
#else
    /* Keep the radio awake. ME_CONFIG ps_on is the only host bit that
     * actually arms firmware TIM/DTIM sleep: the vendor never sends
     * ME_SET_PS_MODE after join, and neither do we. */
    request->power_save_enabled = 0;
#endif
    request->dynamic_power_save = 0;
    request->antenna_diversity_enabled =
        (features & AIC_FW_CAP_ANT_DIV) != 0;
    LOG_I("ME capabilities: feature-map=%s PHY=0x%08x HT40=1 VHT=%u HE=%u max-bw=%u MHz BFMEE=%u MU-RX=%u PS=%u DPSM=%u",
          context && context->firmware_feature_map_compact ? "compact" : "full",
          (unsigned int)(context ? context->firmware_phy_features : 0),
          (unsigned int)vht_supported, (unsigned int)he_supported,
          (unsigned int)(20U << request->max_bandwidth),
          (unsigned int)((features & AIC_FW_CAP_BFMEE) != 0),
          (unsigned int)((features & AIC_FW_CAP_MU_MIMO_RX) != 0),
          (unsigned int)request->power_save_enabled,
          (unsigned int)request->dynamic_power_save);
}
