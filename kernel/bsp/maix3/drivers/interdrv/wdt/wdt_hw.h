/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __WDT_HW_H__
#define __WDT_HW_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * Bare K230 WDT1 (DesignWare APB watchdog): registers only. No locks, sleeps
 * or RTOS objects; callers serialize access, so terminal shutdown can service
 * it with interrupts disabled. The RT-Smart device (drv_wdt.c) is a wrapper
 * around it.
 *
 * Register access needs WDT1's clock: call wdt_hw_init() only while holding
 * its PM reference, and wdt_hw_deinit() before releasing it. Until then every
 * function below does nothing or reports a disabled watchdog.
 */

int wdt_hw_init(void);
void wdt_hw_deinit(void);
bool wdt_hw_ready(void);

/* Enabling is sticky: only a reset disables the watchdog again. */
bool wdt_hw_enabled(void);
/* Restarts the counter first, so the full period applies. */
void wdt_hw_enable(void);
void wdt_hw_restart(void);

/* irq_mode: interrupt first, reset on a second timeout; else reset at once. */
void wdt_hw_set_response_mode(bool irq_mode);
/* TOP 0..15 selects a period of 2^(16 + top) watchdog clocks. */
void wdt_hw_set_top(uint32_t top);
uint32_t wdt_hw_get_top(void);
/* The TOP values are fixed by the hardware configuration. */
bool wdt_hw_uses_fix_top(void);

uint32_t wdt_hw_counter(void);
bool wdt_hw_irq_pending(void);
void wdt_hw_clear_irq(void);

#endif /* __WDT_HW_H__ */
