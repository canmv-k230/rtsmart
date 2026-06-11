#include <rtthread.h>
#include <rthw.h>
#include <stdbool.h>
#include <stdint.h>
#include <ioremap.h>
#include <riscv_io.h>

#include "drv_gpio.h"
#include "sysctl_rst.h"
#include "usb_config.h"
#include "usb_log.h"
#include "usb_otg.h"
#include "usbotg_core.h"

#if defined(ENABLE_CHERRY_USB) && defined(ENABLE_CHERRY_USB_OTG)

#define DEFAULT_USB_HCLK_FREQ_MHZ 200

struct usbh_bus;

uint32_t SystemCoreClock = (DEFAULT_USB_HCLK_FREQ_MHZ * 1000 * 1000);
uintptr_t g_usb_otg0_base = (uintptr_t)0x91500000UL;
uintptr_t g_usb_otg1_base = (uintptr_t)0x91540000UL;

#define K230_USB0_CTRL3_ADDR 0x9158507cUL
#define K230_USB1_CTRL3_ADDR 0x9158509cUL
#define K230_USB0_IRQ        173
#define K230_USB1_IRQ        174

#define USB_IDPULLUP0   (1 << 4)
#define USB_DMPULLDOWN0 (1 << 8)
#define USB_DPPULLDOWN0 (1 << 9)
#define USB_ID_STATUS   (1 << 3)

#define USB_OTG_POLL_INTERVAL_MS 20

#ifndef CHERRY_USB_OTG_USING_DEV
#define CHERRY_USB_OTG_USING_DEV 0
#endif

#ifndef CHERRY_USB_OTG_DEBOUNCE_MS
#define CHERRY_USB_OTG_DEBOUNCE_MS 100
#endif

static uintptr_t kendryte_usb_ctrl3_addr(uint8_t dev)
{
    return dev ? K230_USB1_CTRL3_ADDR : K230_USB0_CTRL3_ADDR;
}

static int kendryte_usb_irq(uint8_t dev)
{
    return dev ? K230_USB1_IRQ : K230_USB0_IRQ;
}

static int kendryte_usb_reset(uint8_t dev)
{
    if (!sysctl_reset(dev ? SYSCTL_RESET_USB1 : SYSCTL_RESET_USB0)) {
        USB_LOG_ERR("reset usb%u fail\n", dev);
        return -1;
    }

    return 0;
}

static uint32_t kendryte_usb_read_ctrl3(uint8_t dev)
{
    volatile uint32_t *hs_reg;
    uint32_t val;

    hs_reg = (volatile uint32_t *)rt_ioremap((void *)kendryte_usb_ctrl3_addr(dev), 0x1000);
    val = *hs_reg;
    rt_iounmap((void *)hs_reg);
    return val;
}

static void kendryte_usb_write_ctrl3(uint8_t dev, uint32_t val)
{
    volatile uint32_t *hs_reg;

    hs_reg = (volatile uint32_t *)rt_ioremap((void *)kendryte_usb_ctrl3_addr(dev), 0x1000);
    *hs_reg = val;
    rt_iounmap((void *)hs_reg);
}

static uint8_t kendryte_usb_mode_from_ctrl3(uint32_t val)
{
    return (val & USB_ID_STATUS) ? USBOTG_MODE_DEVICE : USBOTG_MODE_HOST;
}

static void kendryte_usb_host_power(bool enable)
{
#ifdef CANMV_USB_PWR_PIN
    int usb_host_pin = CANMV_USB_PWR_PIN;

    if (usb_host_pin >= 0) {
        kd_pin_mode(usb_host_pin, GPIO_DM_OUTPUT);
        kd_pin_write(usb_host_pin, enable ? CANMV_USB_PWR_PIN_VALID_VAL : !CANMV_USB_PWR_PIN_VALID_VAL);
    }
#else
    (void)enable;
#endif
}

static void kendryte_usb_config_host(uint8_t dev)
{
    uint32_t usb_ctl3 = kendryte_usb_read_ctrl3(dev) | USB_IDPULLUP0;

    kendryte_usb_write_ctrl3(dev, usb_ctl3 | USB_DMPULLDOWN0 | USB_DPPULLDOWN0);
    kendryte_usb_host_power(true);
}

static void kendryte_usb_config_device(uint8_t dev)
{
    kendryte_usb_host_power(false);
    kendryte_usb_write_ctrl3(dev, 0x37);
}

static void kendryte_usb_irq_mask(uint8_t dev)
{
    rt_hw_interrupt_mask(kendryte_usb_irq(dev));
}

static void kendryte_usb_irq_install(uint8_t dev, void (*handler)(int irq, void *arg), const char *name)
{
    int irq = kendryte_usb_irq(dev);

    rt_hw_interrupt_mask(irq);
    rt_hw_interrupt_install(irq, handler, NULL, name);
}

void usb_otg_irq_enable(uint8_t busid)
{
    USBOTG_IRQHandler(busid);
    rt_hw_interrupt_umask(kendryte_usb_irq(CHERRY_USB_OTG_USING_DEV));
}

struct kendryte_usb_otg {
    bool initialized;
    bool running;
    uint8_t dev;
    uint8_t last_mode;
    volatile uint32_t *id_reg;
    usb_osal_thread_t monitor_thread;
};

static struct kendryte_usb_otg g_kendryte_usb_otg;

static void usb_otg_monitor_thread(void *argument)
{
    uint8_t busid = (uint8_t)(uintptr_t)argument;
    uint8_t sample = g_kendryte_usb_otg.last_mode;
    uint8_t stable_mode = sample;
    uint32_t stable_time = 0;

    while (g_kendryte_usb_otg.running) {
        usb_osal_msleep(USB_OTG_POLL_INTERVAL_MS);

        sample = kendryte_usb_mode_from_ctrl3(*g_kendryte_usb_otg.id_reg);
        if (sample == stable_mode) {
            stable_time = 0;
            continue;
        }

        if (sample == g_kendryte_usb_otg.last_mode) {
            stable_time += USB_OTG_POLL_INTERVAL_MS;
        } else {
            g_kendryte_usb_otg.last_mode = sample;
            stable_time = 0;
        }

        if (stable_time >= CHERRY_USB_OTG_DEBOUNCE_MS) {
            stable_mode = sample;
            stable_time = 0;
            USB_LOG_WRN("USB OTG ID change: %s\r\n", stable_mode == USBOTG_MODE_HOST ? "host" : "device");
            usbotg_trigger_role_change(busid, stable_mode);
        }
    }
}

int usb_otg_init(uint8_t busid)
{
    (void)busid;

    if (g_kendryte_usb_otg.initialized) {
        return 0;
    }

    g_kendryte_usb_otg.dev = CHERRY_USB_OTG_USING_DEV;
    g_kendryte_usb_otg.id_reg = (volatile uint32_t *)rt_ioremap((void *)kendryte_usb_ctrl3_addr(g_kendryte_usb_otg.dev), 0x1000);
    if (g_kendryte_usb_otg.id_reg == NULL) {
        USB_LOG_ERR("map usb%u id reg failed\r\n", g_kendryte_usb_otg.dev);
        return -1;
    }

    g_kendryte_usb_otg.last_mode = kendryte_usb_mode_from_ctrl3(*g_kendryte_usb_otg.id_reg);
    g_kendryte_usb_otg.running = true;
    g_kendryte_usb_otg.monitor_thread = usb_osal_thread_create("usb_id", 2048, 11, usb_otg_monitor_thread, (void *)(uintptr_t)busid);
    if (g_kendryte_usb_otg.monitor_thread == NULL) {
        g_kendryte_usb_otg.running = false;
        rt_iounmap((void *)g_kendryte_usb_otg.id_reg);
        g_kendryte_usb_otg.id_reg = NULL;
        return -1;
    }

    g_kendryte_usb_otg.initialized = true;
    return 0;
}

int usb_otg_deinit(uint8_t busid)
{
    (void)busid;

    if (!g_kendryte_usb_otg.initialized) {
        return 0;
    }

    g_kendryte_usb_otg.running = false;
    if (g_kendryte_usb_otg.monitor_thread) {
        usb_osal_thread_delete(g_kendryte_usb_otg.monitor_thread);
        g_kendryte_usb_otg.monitor_thread = NULL;
    }
    if (g_kendryte_usb_otg.id_reg) {
        rt_iounmap((void *)g_kendryte_usb_otg.id_reg);
        g_kendryte_usb_otg.id_reg = NULL;
    }
    g_kendryte_usb_otg.initialized = false;
    return 0;
}

uint8_t usb_otg_get_current_mode(uint8_t busid)
{
    (void)busid;

    if (g_kendryte_usb_otg.id_reg) {
        return kendryte_usb_mode_from_ctrl3(*g_kendryte_usb_otg.id_reg);
    }

    return kendryte_usb_mode_from_ctrl3(kendryte_usb_read_ctrl3(CHERRY_USB_OTG_USING_DEV));
}

#ifdef ENABLE_CHERRY_USB_HOST
static void usb_hc_interrupt_cb(int irq, void *arg_pv)
{
    (void)irq;
    (void)arg_pv;
    USBOTG_IRQHandler(0);
}

uint32_t usbh_get_dwc2_gccfg_conf(uint32_t reg_base)
{
    (void)reg_base;
    return 0;
}

void usb_hc_low_level_init(struct usbh_bus *bus)
{
    uint8_t dev = CHERRY_USB_OTG_USING_DEV;

    (void)bus;
    if (kendryte_usb_reset(dev) != 0) {
        return;
    }

    kendryte_usb_config_host(dev);
    kendryte_usb_irq_install(dev, usb_hc_interrupt_cb, "usbh");
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
    (void)bus;
    kendryte_usb_irq_mask(CHERRY_USB_OTG_USING_DEV);
    kendryte_usb_host_power(false);
}
#endif /* ENABLE_CHERRY_USB_HOST */

#if defined(ENABLE_CHERRY_USB_DEVICE)
static void usb_dc_interrupt_cb(int irq, void *arg_pv)
{
    (void)irq;
    (void)arg_pv;
    USBOTG_IRQHandler(0);
}

uint32_t usbd_get_dwc2_gccfg_conf(uint32_t reg_base)
{
    (void)reg_base;
    return 0;
}

void usb_dc_low_level_init(void)
{
    uint8_t dev = CHERRY_USB_OTG_USING_DEV;

    if (kendryte_usb_reset(dev) != 0) {
        return;
    }

    kendryte_usb_config_device(dev);
    kendryte_usb_irq_install(dev, usb_dc_interrupt_cb, "usbd");
}

void usb_dc_low_level_deinit(void)
{
    kendryte_usb_irq_mask(CHERRY_USB_OTG_USING_DEV);
}
#endif /* ENABLE_CHERRY_USB_DEVICE */

#endif /* ENABLE_CHERRY_USB && ENABLE_CHERRY_USB_OTG */
