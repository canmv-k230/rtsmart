/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <rthw.h>
#include "usb_config.h"
#include "usb_log.h"
#include <riscv_io.h>
#include <ioremap.h>

#include "k230_pm.h"

#ifdef ENABLE_CHERRY_USB
#define DEFAULT_USB_HCLK_FREQ_MHZ 200

uint32_t SystemCoreClock = (DEFAULT_USB_HCLK_FREQ_MHZ * 1000 * 1000);
uintptr_t g_usb_otg0_base = (uintptr_t)0x91500000UL;
uintptr_t g_usb_otg1_base = (uintptr_t)0x91540000UL;

#define USB_DMPULLDOWN0 	(1<<8)
#define USB_DPPULLDOWN0 	(1<<9)

// USB Host
#ifdef ENABLE_CHERRY_USB_HOST
#if defined(CHERRY_USB_K230_HOST_COMPDISTUNE) && (CHERRY_USB_K230_HOST_COMPDISTUNE >= 0)
#define K230_USB0_PHY_CTL0             0x915850b0UL
#define K230_USB_PHY_CTL_STRIDE        0x08U
#define K230_USB_PHY_COMPDISTUNE_SHIFT 3U
#define K230_USB_PHY_COMPDISTUNE_MASK  (7U << K230_USB_PHY_COMPDISTUNE_SHIFT)

static void k230_usb_host_phy_tune(unsigned int port)
{
    uintptr_t address = K230_USB0_PHY_CTL0 + port * K230_USB_PHY_CTL_STRIDE;
    volatile uint32_t *ctl0 = rt_ioremap((void *)address, sizeof(uint32_t));
    uint32_t before, desired, after;

    if (ctl0 == RT_NULL) {
        USB_LOG_ERR("ioremap usb%u PHY control register fail\n", port);
        return;
    }

    /* COMPDISTUNE controls host disconnect detection, not device bus resets. */
    before = readl(ctl0);
    desired = (before & ~K230_USB_PHY_COMPDISTUNE_MASK) |
              ((uint32_t)CHERRY_USB_K230_HOST_COMPDISTUNE << K230_USB_PHY_COMPDISTUNE_SHIFT);
    writel(desired, ctl0);
    after = readl(ctl0);
    rt_iounmap((void *)ctl0);

    if (after != desired) {
        USB_LOG_ERR("USB%u PHY CTL0 readback %08x, expected %08x\n", port, after, desired);
    }
    USB_LOG_INFO("usb host port=%u PHY override: COMPDISTUNE=%u ctl0=%08x->%08x\n",
                 port, (after & K230_USB_PHY_COMPDISTUNE_MASK) >> K230_USB_PHY_COMPDISTUNE_SHIFT,
                 before, after);
}
#else
#define k230_usb_host_phy_tune(port) ((void)0)
#endif

static void usb_hc_interrupt_cb(int irq, void *arg_pv)
{
    extern void USBH_IRQHandler(uint8_t busid);
    USBH_IRQHandler(0);
}

uint32_t usbh_get_dwc2_gccfg_conf(uint32_t reg_base)
{
    return 0;
}
#if defined (CHERRY_USB_HOST_USING_DEV0)
void usb_hc_low_level_init(void)
{
    if (k230_pm_get(K230_PM_DEV_USB0) != 0) {
        USB_LOG_ERR("pm get usb0 fail\n");
        return;
    }

    k230_usb_host_phy_tune(0);

    uint32_t *hs_reg = (uint32_t *)rt_ioremap((void *)(0x91585000 + 0x7C), 0x1000);
    if (hs_reg == RT_NULL) {
        USB_LOG_ERR("ioremap usb0 control register fail\n");
        (void)k230_pm_put(K230_PM_DEV_USB0);
        return;
    }
    uint32_t usb_ctl3 = *hs_reg;

    *hs_reg = usb_ctl3 | (USB_DMPULLDOWN0 | USB_DPPULLDOWN0);

    rt_iounmap(hs_reg);

    rt_hw_interrupt_install(173, usb_hc_interrupt_cb, NULL, "usbh");
    rt_hw_interrupt_umask(173);
}

void usb_hc_low_level_deinit(void)
{
    rt_hw_interrupt_mask(173);
    (void)k230_pm_put(K230_PM_DEV_USB0);
}
#elif defined (CHERRY_USB_HOST_USING_DEV1)
void usb_hc_low_level_init(void)
{
    if (k230_pm_get(K230_PM_DEV_USB1) != 0) {
        USB_LOG_ERR("pm get usb1 fail\n");
        return;
    }

    k230_usb_host_phy_tune(1);

    uint32_t *hs_reg = (uint32_t *)rt_ioremap((void *)(0x91585000 + 0x9C), 0x1000);
    if (hs_reg == RT_NULL) {
        USB_LOG_ERR("ioremap usb1 control register fail\n");
        (void)k230_pm_put(K230_PM_DEV_USB1);
        return;
    }
    uint32_t usb_ctl3 = *hs_reg;

    *hs_reg = usb_ctl3 | (USB_DMPULLDOWN0 | USB_DPPULLDOWN0);

    rt_iounmap(hs_reg);

    rt_hw_interrupt_install(174, usb_hc_interrupt_cb, NULL, "usbh");
    rt_hw_interrupt_umask(174);
}

void usb_hc_low_level_deinit(void)
{
    rt_hw_interrupt_mask(174);
    (void)k230_pm_put(K230_PM_DEV_USB1);
}
#else
#error "Usb device select error"
#endif

#endif // ENABLE_CHERRY_USB_HOST

// USB Device
#if defined (ENABLE_CHERRY_USB_DEVICE) 
static void usb_dc_interrupt_cb(int irq, void *arg_pv)
{
    extern void USBD_IRQHandler(uint8_t busid);
    USBD_IRQHandler(0);
}
uint32_t usbd_get_dwc2_gccfg_conf(uint32_t reg_base)
{
    return 0;
}
#if defined (CHERRY_USB_DEVICE_USING_DEV0)
void usb_dc_low_level_init(void)
{
    if (k230_pm_get(K230_PM_DEV_USB0) != 0) {
        USB_LOG_ERR("pm get usb0 fail\n");
        return;
    }

    uint32_t *hs_reg = (uint32_t *)rt_ioremap((void *)(0x91585000 + 0x7C), 0x1000);
    if (hs_reg == RT_NULL) {
        USB_LOG_ERR("ioremap usb0 control register fail\n");
        (void)k230_pm_put(K230_PM_DEV_USB0);
        return;
    }
    *hs_reg = 0x37;
    rt_iounmap(hs_reg);

    rt_hw_interrupt_install(173, usb_dc_interrupt_cb, NULL, "usbd");
    rt_hw_interrupt_umask(173);
}

void usb_dc_low_level_deinit(void)
{
    rt_hw_interrupt_mask(173);
    (void)k230_pm_put(K230_PM_DEV_USB0);
}
#elif defined(CHERRY_USB_DEVICE_USING_DEV1)
void usb_dc_low_level_init(void)
{
    if (k230_pm_get(K230_PM_DEV_USB1) != 0) {
        USB_LOG_ERR("pm get usb1 fail\n");
        return;
    }

    uint32_t *hs_reg = (uint32_t *)rt_ioremap((void *)(0x91585000 + 0x9C), 0x1000);
    if (hs_reg == RT_NULL) {
        USB_LOG_ERR("ioremap usb1 control register fail\n");
        (void)k230_pm_put(K230_PM_DEV_USB1);
        return;
    }
    *hs_reg = 0x37;
    rt_iounmap(hs_reg);

    rt_hw_interrupt_install(174, usb_dc_interrupt_cb, NULL, "usbd");
    rt_hw_interrupt_umask(174);
}

void usb_dc_low_level_deinit(void)
{
    rt_hw_interrupt_mask(174);
    (void)k230_pm_put(K230_PM_DEV_USB1);
}
#else
#error "Usb device select error"
#endif 

#endif // ENABLE_CHERRY_USB_DEVICE

#endif // ENABLE_CHERRY_USB
