/*
 * Copyright (c) 2025, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbotg_core.h"
#include <stdio.h>

#ifdef ENABLE_CHERRY_USB_OTG

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbotg_core"
#include "usb_log.h"

#define CONFIG_USB_OTG_MAX_BUS CONFIG_USBHOST_MAX_BUS

struct usbotg_core_priv {
    uint8_t busid;
    uint32_t reg_base;
    bool usbh_initialized;
    bool usbd_initialized;
    usbotg_device_event_handler_t device_event_callback;
    uint8_t request_mode;
    usb_osal_sem_t change_sem;
    usb_osal_thread_t change_thread;
};

static struct usbotg_core_priv g_usbotg_core[CONFIG_USB_OTG_MAX_BUS];

extern void USBH_IRQHandler(uint8_t busid);
extern void USBD_IRQHandler(uint8_t busid);

static void usbotg_host_initialize(uint8_t busid)
{
    if (g_usbotg_core[busid].usbh_initialized) {
        return;
    }

    if (g_usbotg_core[busid].usbd_initialized) {
        usbd_deinitialize(busid);
        g_usbotg_core[busid].usbd_initialized = false;
    }

    USB_LOG_INFO("Switch to HOST mode\r\n");

    if (usbh_initialize(busid, g_usbotg_core[busid].reg_base) == 0) {
        g_usbotg_core[busid].usbh_initialized = true;
        usb_otg_irq_enable(busid);
    }
}

static void usbotg_device_initialize(uint8_t busid)
{
    if (g_usbotg_core[busid].usbd_initialized) {
        return;
    }

    if (g_usbotg_core[busid].usbh_initialized) {
        usbh_deinitialize(busid);
        g_usbotg_core[busid].usbh_initialized = false;
    }

    USB_LOG_INFO("Switch to DEVICE mode\r\n");

    if (usbd_initialize(busid, g_usbotg_core[busid].reg_base, g_usbotg_core[busid].device_event_callback) == 0) {
        g_usbotg_core[busid].usbd_initialized = true;
        usb_otg_irq_enable(busid);
    }
}

static void usbotg_rolechange_thread(void *argument)
{
    uint8_t busid = (uint8_t)(uintptr_t)argument;

    while (1) {
        if (usb_osal_sem_take(g_usbotg_core[busid].change_sem, USB_OSAL_WAITING_FOREVER) == 0) {
            if (g_usbotg_core[busid].request_mode == USBOTG_MODE_HOST) {
                usbotg_host_initialize(busid);
            } else if (g_usbotg_core[busid].request_mode == USBOTG_MODE_DEVICE) {
                usbotg_device_initialize(busid);
            }
        }
    }
}

int usbotg_initialize(uint8_t busid, uint32_t reg_base, usbotg_device_event_handler_t device_event_callback, uint8_t default_role)
{
    char thread_name[32] = { 0 };

    if (busid >= CONFIG_USB_OTG_MAX_BUS) {
        USB_LOG_ERR("bus overflow\r\n");
        return -1;
    }

    g_usbotg_core[busid].busid = busid;
    g_usbotg_core[busid].reg_base = reg_base;
    g_usbotg_core[busid].device_event_callback = device_event_callback;

    g_usbotg_core[busid].change_sem = usb_osal_sem_create(0);
    if (g_usbotg_core[busid].change_sem == NULL) {
        USB_LOG_ERR("Failed to create change_sem\r\n");
        return -1;
    }

    snprintf(thread_name, sizeof(thread_name), "usbotg%u", busid);
    g_usbotg_core[busid].change_thread = usb_osal_thread_create(thread_name, 4096, 10, usbotg_rolechange_thread, (void *)(uintptr_t)busid);
    if (g_usbotg_core[busid].change_thread == NULL) {
        USB_LOG_ERR("Failed to create usbotg thread\r\n");
        usb_osal_sem_delete(g_usbotg_core[busid].change_sem);
        g_usbotg_core[busid].change_sem = NULL;
        return -1;
    }

    if (usb_otg_init(busid) != 0) {
        USB_LOG_ERR("Failed to init usb otg\r\n");
        usb_osal_thread_delete(g_usbotg_core[busid].change_thread);
        usb_osal_sem_delete(g_usbotg_core[busid].change_sem);
        g_usbotg_core[busid].change_thread = NULL;
        g_usbotg_core[busid].change_sem = NULL;
        return -1;
    }

    if (default_role == USBOTG_MODE_OTG) {
        default_role = usb_otg_get_current_mode(busid);
    }
    usbotg_trigger_role_change(busid, default_role);
    return 0;
}

int usbotg_deinitialize(uint8_t busid)
{
    if (busid >= CONFIG_USB_OTG_MAX_BUS) {
        USB_LOG_ERR("bus overflow\r\n");
        return -1;
    }

    usb_otg_deinit(busid);

    if (g_usbotg_core[busid].usbd_initialized) {
        usbd_deinitialize(busid);
        g_usbotg_core[busid].usbd_initialized = false;
    }

    if (g_usbotg_core[busid].usbh_initialized) {
        usbh_deinitialize(busid);
        g_usbotg_core[busid].usbh_initialized = false;
    }

    if (g_usbotg_core[busid].change_thread) {
        usb_osal_thread_delete(g_usbotg_core[busid].change_thread);
        g_usbotg_core[busid].change_thread = NULL;
    }

    if (g_usbotg_core[busid].change_sem) {
        usb_osal_sem_delete(g_usbotg_core[busid].change_sem);
        g_usbotg_core[busid].change_sem = NULL;
    }

    return 0;
}

void usbotg_trigger_role_change(uint8_t busid, uint8_t mode)
{
    if (busid >= CONFIG_USB_OTG_MAX_BUS) {
        USB_LOG_ERR("bus overflow\r\n");
        return;
    }

    if (mode != USBOTG_MODE_HOST && mode != USBOTG_MODE_DEVICE) {
        return;
    }

    g_usbotg_core[busid].request_mode = mode;

    if (g_usbotg_core[busid].change_sem) {
        usb_osal_sem_give(g_usbotg_core[busid].change_sem);
    }
}

void USBOTG_IRQHandler(uint8_t busid)
{
    if (busid >= CONFIG_USB_OTG_MAX_BUS) {
        return;
    }

    if (g_usbotg_core[busid].usbh_initialized) {
        USBH_IRQHandler(busid);
    } else if (g_usbotg_core[busid].usbd_initialized) {
        USBD_IRQHandler(busid);
    }
}

#endif /* ENABLE_CHERRY_USB_OTG */
