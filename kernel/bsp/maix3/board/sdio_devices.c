/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>

#include "drv_gpio.h"
#include "drv_sdhci.h"
#include "sysctl_boot.h"

#if defined(RT_USING_SDIO0) || defined(RT_USING_SDIO1)

#define DBG_TAG "board.sdio"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static int board_sdio_boot_host(void)
{
    if (g_sysctl_boot_mode == SYSCTL_BOOT_EMMC)
    {
        return 0;
    }
    if (g_sysctl_boot_mode == SYSCTL_BOOT_SDCARD)
    {
        return 1;
    }
    return -1;
}

static int board_sdio_storage_init(void)
{
    int boot_host = board_sdio_boot_host();

    if (boot_host < 0)
    {
        LOG_I("boot mode %d does not use an SD/MMC controller",
              g_sysctl_boot_mode);
        return RT_EOK;
    }

#ifdef BSP_USING_WIFI_SDIO
    if (boot_host == BSP_WIFI_SDIO_HOST)
    {
        LOG_E("SDIO%d is assigned to both the boot disk and Wi-Fi",
              boot_host);
        return -RT_ERROR;
    }
#endif

    LOG_I("probing boot storage on SDIO%d", boot_host);
    kd_sdhci_change(boot_host);

#ifdef MOUNT_SECOND_CARD
    boot_host = boot_host == 0 ? 1 : 0;
    LOG_I("probing secondary storage on SDIO%d", boot_host);
    kd_sdhci_change(boot_host);
#endif

    return RT_EOK;
}
INIT_DEVICE_EXPORT_SEQ(board_sdio_storage_init, 210);

#ifdef BSP_USING_WIFI_SDIO

#define BOARD_SDIO_WIFI_THREAD_STACK_SIZE 2048U
#define BOARD_SDIO_WIFI_THREAD_PRIORITY   (RT_MMCSD_THREAD_PRIORITY + 1)

int board_sdio_wifi_prepare(void)
{
    int asserted = GPIO_PV_LOW;
    int released = GPIO_PV_HIGH;

    LOG_I("preparing SDIO Wi-Fi socket on host %d", BSP_WIFI_SDIO_HOST);

#ifndef BSP_WIFI_SDIO_RESET_ACTIVE_LOW
    asserted = GPIO_PV_HIGH;
    released = GPIO_PV_LOW;
#endif

#if BSP_WIFI_SDIO_REG_ON_PIN >= 0
    rt_err_t result = kd_pin_mode(BSP_WIFI_SDIO_REG_ON_PIN, GPIO_DM_OUTPUT);
    if (result != RT_EOK)
    {
        rt_kprintf("SDIO Wi-Fi: failed to configure reset GPIO %d: %d\n",
                   BSP_WIFI_SDIO_REG_ON_PIN, result);
        return result;
    }
    kd_pin_write(BSP_WIFI_SDIO_REG_ON_PIN, asserted);
    rt_thread_mdelay(BSP_WIFI_SDIO_RESET_PULSE_MS);
    kd_pin_write(BSP_WIFI_SDIO_REG_ON_PIN, released);
#elif defined(BSP_WIFI_SDIO_USE_SDIO0_RESET)
    kd_sdhci0_reset(asserted == GPIO_PV_HIGH);
    rt_thread_mdelay(BSP_WIFI_SDIO_RESET_PULSE_MS);
    kd_sdhci0_reset(released == GPIO_PV_HIGH);
#endif

    rt_thread_mdelay(BSP_WIFI_SDIO_POWER_UP_DELAY_MS);
    LOG_I("SDIO Wi-Fi socket ready on host %d", BSP_WIFI_SDIO_HOST);
    return RT_EOK;
}

static void board_sdio_wifi_probe(void *parameter)
{
    (void)parameter;

    if (board_sdio_wifi_prepare() != RT_EOK)
    {
        LOG_E("SDIO Wi-Fi socket preparation failed");
        return;
    }

    kd_sdhci_change(BSP_WIFI_SDIO_HOST);
}

static int board_sdio_wifi_init(void)
{
    rt_thread_t thread;
    int boot_host = board_sdio_boot_host();

    if (boot_host == BSP_WIFI_SDIO_HOST)
    {
        return RT_EOK;
    }

    thread = rt_thread_create("sdio_wifi", board_sdio_wifi_probe, RT_NULL,
                              BOARD_SDIO_WIFI_THREAD_STACK_SIZE,
                              BOARD_SDIO_WIFI_THREAD_PRIORITY, 20);
    if (!thread)
    {
        LOG_E("failed to create SDIO Wi-Fi probe thread");
        return -RT_ENOMEM;
    }

    return rt_thread_startup(thread);
}
INIT_COMPONENT_EXPORT_SEQ(board_sdio_wifi_init, 220);

#endif

#endif
