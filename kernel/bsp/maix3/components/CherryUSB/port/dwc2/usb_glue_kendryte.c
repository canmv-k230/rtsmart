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

#include "sysctl_rst.h"

#ifdef ENABLE_CHERRY_USB
#define DEFAULT_USB_HCLK_FREQ_MHZ 200

uint32_t SystemCoreClock = (DEFAULT_USB_HCLK_FREQ_MHZ * 1000 * 1000);
uintptr_t g_usb_otg0_base = (uintptr_t)0x91500000UL;
uintptr_t g_usb_otg1_base = (uintptr_t)0x91540000UL;

#define SYSCTL_USB_DONE_SHIFT (28)
#define SYSCTL_USB_DONE_MASK (0xF << SYSCTL_USB_DONE_SHIFT)

#define SYSCTL_USB_RESET_SHIFT (0)
#define SYSCTL_USB_RESET_MASK (0x3 << SYSCTL_USB_RESET_SHIFT)

// static void sysctl_reset_hw_done(volatile uint32_t *reset_reg, uint8_t reset_bit, uint8_t done_bit)
// {
//     uint32_t val;
//     rt_base_t level;
//     uint32_t done = ((1 << done_bit) | (1 << (done_bit + 2)));
//     uint32_t reset = (1 << reset_bit);

//     level = rt_hw_interrupt_disable();

//     /* clear done bit */
//     val = readl(reset_reg);
//     val &= ~(SYSCTL_USB_DONE_MASK | SYSCTL_USB_RESET_MASK);
//     val |= done;
//     writel(val, reset_reg);

//     /* set reset bit */
//     val = readl(reset_reg);
//     val &= ~(SYSCTL_USB_DONE_MASK | SYSCTL_USB_RESET_MASK);
//     val |= reset;
//     writel(val, reset_reg);

//     rt_hw_interrupt_enable(level);

//     /* wait done bit */
//    while ((readl(reset_reg) & done) != done);
// }

#define USB_IDPULLUP0 		(1<<4)
#define USB_DMPULLDOWN0 	(1<<8)
#define USB_DPPULLDOWN0 	(1<<9)

// USB Host
#ifdef ENABLE_CHERRY_USB_HOST
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
    // sysctl_reset_hw_done((volatile uint32_t *)0x9110103c, 0, 28);
    if (!sysctl_reset(SYSCTL_RESET_USB0)) {
        USB_LOG_ERR("reset usb0 fail\n");
    }

    uint32_t *hs_reg = (uint32_t *)rt_ioremap((void *)(0x91585000 + 0x7C), 0x1000);
    uint32_t usb_ctl3 = *hs_reg;

    *hs_reg = usb_ctl3 | (USB_DMPULLDOWN0 | USB_DPPULLDOWN0);

    rt_iounmap(hs_reg);

    rt_hw_interrupt_install(173, usb_hc_interrupt_cb, NULL, "usbh");
    rt_hw_interrupt_umask(173);
}

void usb_hc_low_level_deinit(void)
{
    rt_hw_interrupt_mask(173);
}
#elif defined (CHERRY_USB_HOST_USING_DEV1)
void usb_hc_low_level_init(void)
{
    // sysctl_reset_hw_done((volatile uint32_t *)0x9110103c, 1, 29);
    if (!sysctl_reset(SYSCTL_RESET_USB1)) {
        USB_LOG_ERR("reset usb1 fail\n");
    }

    uint32_t *hs_reg = (uint32_t *)rt_ioremap((void *)(0x91585000 + 0x9C), 0x1000);
    uint32_t usb_ctl3 = *hs_reg;

    *hs_reg = usb_ctl3 | (USB_DMPULLDOWN0 | USB_DPPULLDOWN0);

    rt_iounmap(hs_reg);

    rt_hw_interrupt_install(174, usb_hc_interrupt_cb, NULL, "usbh");
    rt_hw_interrupt_umask(174);
}

void usb_hc_low_level_deinit(void)
{
    rt_hw_interrupt_mask(174);
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
    // sysctl_reset_hw_done((volatile uint32_t *)0x9110103c, 0, 28);
    if (!sysctl_reset(SYSCTL_RESET_USB0)) {
        USB_LOG_ERR("reset usb0 fail\n");
    }

    uint32_t *hs_reg = (uint32_t *)rt_ioremap((void *)(0x91585000 + 0x7C), 0x1000);
    *hs_reg = 0x37;
    rt_iounmap(hs_reg);

    rt_hw_interrupt_install(173, usb_dc_interrupt_cb, NULL, "usbd");
    rt_hw_interrupt_umask(173);
}

void usb_dc_low_level_deinit(void)
{
    rt_hw_interrupt_mask(173);
}
#elif defined(CHERRY_USB_DEVICE_USING_DEV1)
void usb_dc_low_level_init(void)
{
    // sysctl_reset_hw_done((volatile uint32_t *)0x9110103c, 1, 29);
    if (!sysctl_reset(SYSCTL_RESET_USB1)) {
        USB_LOG_ERR("reset usb1 fail\n");
    }

    uint32_t *hs_reg = (uint32_t *)rt_ioremap((void *)(0x91585000 + 0x9C), 0x1000);
    *hs_reg = 0x37;
    rt_iounmap(hs_reg);

    rt_hw_interrupt_install(174, usb_dc_interrupt_cb, NULL, "usbd");
    rt_hw_interrupt_umask(174);
}

void usb_dc_low_level_deinit(void)
{
    rt_hw_interrupt_mask(174);
}
#else
#error "Usb device select error"
#endif 

#endif // ENABLE_CHERRY_USB_DEVICE

#endif // ENABLE_CHERRY_USB
