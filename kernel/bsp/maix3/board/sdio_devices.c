/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>

#ifdef MOUNT_SECOND_CARD
#include <dfs_fs.h>
#endif

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

#define BOARD_SDIO_CD_MOUNT_TIMEOUT_MS     2000U
#define BOARD_SDIO_SECOND_CARD_MOUNT_POINT "/ext_data"

#ifdef MOUNT_SECOND_CARD
int board_sdio_cd_mount(int host)
{
    const char *device_name;
    const char *mounted_path;
    rt_device_t device;
    int result;

    result = kd_sdhci_wait_card(
        host, rt_tick_from_millisecond(BOARD_SDIO_CD_MOUNT_TIMEOUT_MS));
    if (result != MMCSD_HOST_PLUGED)
    {
        LOG_E("secondary card detection failed on SDIO%d: %d",
              host, result);
        return result;
    }

    device_name = host == 0 ? "sd00" : "sd10";
    device = rt_device_find(device_name);
    if (!device)
    {
        LOG_E("secondary card device %s was not registered", device_name);
        return -RT_ERROR;
    }

    mounted_path = dfs_filesystem_get_mounted_path(device);
    if (mounted_path)
    {
        if (rt_strcmp(mounted_path, BOARD_SDIO_SECOND_CARD_MOUNT_POINT) != 0)
        {
            LOG_W("secondary card device %s is already mounted on %s",
                  device_name, mounted_path);
            return -RT_EBUSY;
        }
        return RT_EOK;
    }

    result = dfs_mount(device_name, BOARD_SDIO_SECOND_CARD_MOUNT_POINT,
                       "elm", 0, RT_NULL);
    if (result != RT_EOK)
    {
        device = rt_device_find(device_name);
        if (!device)
        {
            return result;
        }
        mounted_path = dfs_filesystem_get_mounted_path(device);
        if (mounted_path &&
            rt_strcmp(mounted_path,
                      BOARD_SDIO_SECOND_CARD_MOUNT_POINT) == 0)
        {
            return RT_EOK;
        }

        LOG_E("failed to mount secondary card %s on %s: %d",
              device_name, BOARD_SDIO_SECOND_CARD_MOUNT_POINT,
              rt_get_errno());
        return result;
    }

    LOG_I("mounted secondary card %s on %s", device_name,
          BOARD_SDIO_SECOND_CARD_MOUNT_POINT);
    return RT_EOK;
}
#endif

#if defined(MOUNT_SECOND_CARD) && defined(SECOND_CARD_CD_GPIO) && \
    SECOND_CARD_CD_GPIO >= 0

#define BOARD_SDIO_CD_DEBOUNCE_MS       30U
#define BOARD_SDIO_CD_WORKER_STACK_SIZE 8192U
#define BOARD_SDIO_CD_WORKER_PRIORITY   (RT_MMCSD_THREAD_PRIORITY + 1)
#define BOARD_SDIO_CD_EVENT_COUNT       8U

static struct rt_timer board_sdio_cd_timer;
static struct rt_mailbox board_sdio_cd_events;
static rt_ubase_t board_sdio_cd_event_pool[BOARD_SDIO_CD_EVENT_COUNT];
static rt_thread_t board_sdio_cd_worker;
static int board_sdio_cd_host = -1;
static rt_bool_t board_sdio_cd_present;
static rt_bool_t board_sdio_cd_reprobe_required;

static int board_sdio_cd_get_present(void)
{
    rt_err_t level = kd_pin_read(SECOND_CARD_CD_GPIO);

    if (level < 0)
    {
        return level;
    }

#ifdef SECOND_CARD_CD_ACTIVE_LOW
    return level == GPIO_PV_LOW;
#else
    return level == GPIO_PV_HIGH;
#endif
}

static void board_sdio_cd_process(int present)
{
    int current;
    int result;

    /* A queued insertion can be stale if another debounced removal arrived
     * while the worker was busy. Removal events are always processed so a
     * physical power cycle invalidates the old card state. */
    if (present && board_sdio_cd_get_present() != RT_TRUE)
    {
        return;
    }

    if (!present)
    {
        board_sdio_cd_reprobe_required = RT_TRUE;
    }

    result = kd_sdhci_wait_card(
        board_sdio_cd_host,
        rt_tick_from_millisecond(BOARD_SDIO_CD_MOUNT_TIMEOUT_MS));
    if (result != MMCSD_HOST_PLUGED && result != MMCSD_HOST_UNPLUGED)
    {
        LOG_E("failed to read SDIO%d card state: %d",
              board_sdio_cd_host, result);
        return;
    }

    current = result == MMCSD_HOST_PLUGED;

    if (!present)
    {
        if (current)
        {
            kd_sdhci_change(board_sdio_cd_host);
            result = kd_sdhci_wait_card(
                board_sdio_cd_host,
                rt_tick_from_millisecond(BOARD_SDIO_CD_MOUNT_TIMEOUT_MS));
            if (result != MMCSD_HOST_UNPLUGED)
            {
                LOG_E("failed to remove secondary card on SDIO%d: %d",
                      board_sdio_cd_host, result);
                return;
            }
        }

        board_sdio_cd_reprobe_required = RT_FALSE;
        return;
    }

    /* If the preceding removal failed, the registered card describes media
     * that has physically gone away. Remove it before probing this insertion. */
    if (board_sdio_cd_reprobe_required && current)
    {
        kd_sdhci_change(board_sdio_cd_host);
        result = kd_sdhci_wait_card(
            board_sdio_cd_host,
            rt_tick_from_millisecond(BOARD_SDIO_CD_MOUNT_TIMEOUT_MS));
        if (result != MMCSD_HOST_UNPLUGED)
        {
            LOG_E("failed to retire stale card on SDIO%d: %d",
                  board_sdio_cd_host, result);
            return;
        }
        current = RT_FALSE;
    }

    if (!current)
    {
        board_sdio_cd_reprobe_required = RT_FALSE;
        kd_sdhci_change(board_sdio_cd_host);
        result = kd_sdhci_wait_card(
            board_sdio_cd_host,
            rt_tick_from_millisecond(BOARD_SDIO_CD_MOUNT_TIMEOUT_MS));
        if (result != MMCSD_HOST_PLUGED)
        {
            LOG_E("failed to probe secondary card on SDIO%d: %d",
                  board_sdio_cd_host, result);
            return;
        }
    }

    if (board_sdio_cd_get_present() == RT_TRUE)
    {
        board_sdio_cd_mount(board_sdio_cd_host);
    }
}

static void board_sdio_cd_worker_entry(void *parameter)
{
    rt_ubase_t event;

    (void)parameter;

    while (RT_TRUE)
    {
        if (rt_mb_recv(&board_sdio_cd_events, &event,
                       RT_WAITING_FOREVER) == RT_EOK)
        {
            board_sdio_cd_process(event == 2U);
        }
    }
}

static void board_sdio_cd_debounce(void *parameter)
{
    int present;

    (void)parameter;

    present = board_sdio_cd_get_present();
    if (present < 0)
    {
        LOG_E("failed to read secondary card-detect GPIO %d: %d",
              SECOND_CARD_CD_GPIO, present);
        return;
    }

    if (board_sdio_cd_present == (rt_bool_t)present)
    {
        return;
    }

    board_sdio_cd_present = present;
    LOG_I("secondary card %s on SDIO%d",
          present ? "inserted" : "removed", board_sdio_cd_host);
    if (rt_mb_send(&board_sdio_cd_events, present ? 2U : 1U) != RT_EOK)
    {
        LOG_W("secondary card-detect event queue is full");
    }
}

static void board_sdio_cd_irq(void *parameter)
{
    (void)parameter;

    rt_timer_start(&board_sdio_cd_timer);
}

static rt_err_t board_sdio_cd_init(int host)
{
    rt_err_t result;
    int current;
    int present;

#ifdef SECOND_CARD_CD_ACTIVE_LOW
    result = kd_pin_mode(SECOND_CARD_CD_GPIO, GPIO_DM_INPUT_PULLUP);
#else
    result = kd_pin_mode(SECOND_CARD_CD_GPIO, GPIO_DM_INPUT_PULLDOWN);
#endif
    if (result != RT_EOK)
    {
        LOG_E("failed to configure secondary card-detect GPIO %d: %d",
              SECOND_CARD_CD_GPIO, result);
        return result;
    }

    present = board_sdio_cd_get_present();
    if (present < 0)
    {
        LOG_E("failed to read secondary card-detect GPIO %d: %d",
              SECOND_CARD_CD_GPIO, present);
        return present;
    }

    board_sdio_cd_host = host;
    board_sdio_cd_present = present;
    rt_timer_init(&board_sdio_cd_timer, "sd_cd", board_sdio_cd_debounce,
                  RT_NULL,
                  rt_tick_from_millisecond(BOARD_SDIO_CD_DEBOUNCE_MS),
                  RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    result = rt_mb_init(&board_sdio_cd_events, "sd_cd_evt",
                        board_sdio_cd_event_pool,
                        BOARD_SDIO_CD_EVENT_COUNT, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        rt_timer_detach(&board_sdio_cd_timer);
        return result;
    }

    board_sdio_cd_worker = rt_thread_create(
        "sd_cd", board_sdio_cd_worker_entry, RT_NULL,
        BOARD_SDIO_CD_WORKER_STACK_SIZE, BOARD_SDIO_CD_WORKER_PRIORITY, 20);
    if (!board_sdio_cd_worker)
    {
        rt_mb_detach(&board_sdio_cd_events);
        rt_timer_detach(&board_sdio_cd_timer);
        return -RT_ENOMEM;
    }

    result = rt_thread_startup(board_sdio_cd_worker);
    if (result != RT_EOK)
    {
        rt_thread_delete(board_sdio_cd_worker);
        board_sdio_cd_worker = RT_NULL;
        rt_mb_detach(&board_sdio_cd_events);
        rt_timer_detach(&board_sdio_cd_timer);
        return result;
    }

    result = kd_pin_attach_irq(SECOND_CARD_CD_GPIO, GPIO_PE_BOTH,
                               board_sdio_cd_irq, RT_NULL);
    if (result != RT_EOK)
    {
        LOG_E("failed to attach secondary card-detect GPIO %d IRQ: %d",
              SECOND_CARD_CD_GPIO, result);
        rt_thread_delete(board_sdio_cd_worker);
        board_sdio_cd_worker = RT_NULL;
        rt_mb_detach(&board_sdio_cd_events);
        rt_timer_detach(&board_sdio_cd_timer);
        return result;
    }

    result = kd_pin_irq_enable(SECOND_CARD_CD_GPIO, KD_GPIO_IRQ_ENABLE);
    if (result != RT_EOK)
    {
        LOG_E("failed to enable secondary card-detect GPIO %d IRQ: %d",
              SECOND_CARD_CD_GPIO, result);
        kd_pin_detach_irq(SECOND_CARD_CD_GPIO);
        rt_thread_delete(board_sdio_cd_worker);
        board_sdio_cd_worker = RT_NULL;
        rt_mb_detach(&board_sdio_cd_events);
        rt_timer_detach(&board_sdio_cd_timer);
        return result;
    }

    LOG_I("secondary card-detect GPIO %d enabled (%s)",
          SECOND_CARD_CD_GPIO, present ? "inserted" : "empty");

    current = board_sdio_cd_get_present();
    if (current < 0)
    {
        LOG_E("failed to verify secondary card-detect GPIO %d: %d",
              SECOND_CARD_CD_GPIO, current);
    }
    else if (present != current)
    {
        rt_timer_start(&board_sdio_cd_timer);
    }

    return RT_EOK;
}

#endif

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
#if defined(SECOND_CARD_CD_GPIO) && SECOND_CARD_CD_GPIO >= 0
    {
        rt_err_t result = board_sdio_cd_init(boot_host);

        if (result != RT_EOK)
        {
            return result;
        }
        if (!board_sdio_cd_present)
        {
            return RT_EOK;
        }
    }
#endif
    LOG_I("probing secondary storage on SDIO%d", boot_host);
    kd_sdhci_change(boot_host);
#endif

    return RT_EOK;
}
INIT_DEVICE_EXPORT_SEQ(board_sdio_storage_init, 210);

#ifdef BSP_USING_WIFI_SDIO

#define BOARD_SDIO_WIFI_THREAD_STACK_SIZE 8192U
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
