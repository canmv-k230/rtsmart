/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

/** prevent recursive inclusion **/
#ifndef __ESP_HOSTED_API_TYPES_H__
#define __ESP_HOSTED_API_TYPES_H__

#include "esp_hosted_rpc.pb-c.h"

#include <rtthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint32_t major1;
	uint32_t minor1;
	uint32_t patch1;
	int32_t revision;
	int32_t prerelease;
	int32_t build;
} esp_hosted_coprocessor_fwver_t;

/* The upstream API uses ESP-IDF Wi-Fi types. RT-Smart keeps the same API
 * function names but represents structured arguments with the generated
 * protobuf types, avoiding a private copy of the ESP-IDF ABI. Pointers inside
 * a structured getter result remain valid until the next call to that same
 * getter. Callers should copy data they need to retain. */
typedef rt_err_t esp_err_t;
typedef int32_t wifi_interface_t;
typedef int32_t wifi_mode_t;
typedef int32_t wifi_ps_type_t;
typedef int32_t wifi_storage_t;
typedef int32_t wifi_bandwidth_t;
typedef int32_t wifi_second_chan_t;
typedef int32_t wifi_phy_mode_t;
typedef int32_t esp_mac_type_t;

typedef WifiInitConfig wifi_init_config_t;
typedef WifiConfig wifi_config_t;
typedef WifiScanConfig wifi_scan_config_t;
typedef WifiScanDefaultParams wifi_scan_default_params_t;
typedef WifiApRecord wifi_ap_record_t;
typedef WifiCountry wifi_country_t;
typedef WifiStaList wifi_sta_list_t;

enum
{
    WIFI_IF_STA = 0,
    WIFI_IF_AP = 1,
};

enum
{
    WIFI_MODE_NULL = 0,
    WIFI_MODE_STA = 1,
    WIFI_MODE_AP = 2,
    WIFI_MODE_APSTA = 3,
};

enum
{
    ESP_MAC_WIFI_STA = 0,
    ESP_MAC_WIFI_SOFTAP,
    ESP_MAC_BT,
    ESP_MAC_ETH,
    ESP_MAC_IEEE802154,
    ESP_MAC_BASE,
    ESP_MAC_EFUSE_FACTORY,
    ESP_MAC_EFUSE_CUSTOM,
    ESP_MAC_EFUSE_EXT,
};

#define ESP_OK                RT_EOK
#define ESP_FAIL              (-RT_ERROR)
#define ESP_ERR_INVALID_ARG   (-RT_EINVAL)
#define ESP_ERR_NOT_SUPPORTED (-RT_ENOSYS)

#define ESP_HOSTED_APP_DESC_MAGIC_WORD 0xABCD5432U

typedef struct
{
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full;
    uint16_t max_efuse_blk_rev_full;
    uint8_t mmu_page_size;
    uint8_t reserv3[3];
    uint32_t reserv2[18];
} esp_hosted_app_desc_t;

#ifdef __cplusplus
}
#endif

#endif
