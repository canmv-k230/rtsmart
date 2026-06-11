/*
 * Copyright (c) 2025, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef USB_OTG_H
#define USB_OTG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USBOTG_MODE_HOST   0
#define USBOTG_MODE_DEVICE 1
#define USBOTG_MODE_OTG    2

int usb_otg_init(uint8_t busid);
int usb_otg_deinit(uint8_t busid);
uint8_t usb_otg_get_current_mode(uint8_t busid);
void usb_otg_irq_enable(uint8_t busid);

void USBOTG_IRQHandler(uint8_t busid);

#ifdef __cplusplus
}
#endif

#endif /* USB_OTG_H */
