/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOARD_SDIO_CONFIG_H__
#define BOARD_SDIO_CONFIG_H__

#include <rtconfig.h>

#ifdef BSP_USING_WIFI_SDIO

#ifndef RT_USING_SDIO
#error "SDIO Wi-Fi requires RT_USING_SDIO."
#endif

#if !defined(BSP_WIFI_SDIO_HOST) || (BSP_WIFI_SDIO_HOST != 0 && BSP_WIFI_SDIO_HOST != 1)
#error "Select a valid SDIO Wi-Fi host: BSP_WIFI_SDIO_HOST_0 or BSP_WIFI_SDIO_HOST_1."
#elif BSP_WIFI_SDIO_HOST == 0
#ifndef RT_USING_SDIO0
#error "SDIO Wi-Fi on host 0 requires RT_USING_SDIO0."
#endif
#ifdef RT_SDIO0_EMMC
#error "SDIO0 cannot be shared by eMMC and Wi-Fi; select BSP_WIFI_SDIO_HOST_1."
#endif
#elif !defined(RT_USING_SDIO1)
#error "SDIO Wi-Fi on host 1 requires RT_USING_SDIO1."
#endif

#endif /* BSP_USING_WIFI_SDIO */

#endif /* BOARD_SDIO_CONFIG_H__ */
