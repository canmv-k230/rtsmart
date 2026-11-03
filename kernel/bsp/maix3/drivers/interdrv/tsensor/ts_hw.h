/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __TS_HW_H__
#define __TS_HW_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * Bare K230 temperature sensor: registers and conversions only. No locks,
 * sleeps or RTOS objects; callers serialize access and choose how to wait,
 * so the thermal monitor and its interrupts-off shutdown path can use it
 * directly. The RT-Smart device (drv_ts.c) is a wrapper around it.
 */

#define TS_HW_DEFAULT_TRIM 8

/* The first conversion after EN takes 1305 clocks, later ones 256 clocks:
 * 13.05 ms and 2.56 ms at the default 100 kHz, 43.5 ms and 8.5 ms at the
 * sensor's 30 kHz minimum. */
#define TS_HW_FIRST_CONVERSION_MS 50
#define TS_HW_CONVERSION_MS       10

/* Maps the registers; repeated calls do nothing. */
int ts_hw_init(void);

/* The functions below do nothing, or fail, until ts_hw_init() succeeded. */

/* Leaves the sensor disabled: MODE, TRIM and TEST_EN may change only while
 * EN is low. Also clears a result left by the previous configuration. */
void ts_hw_configure(uint8_t trim, bool continuous);
void ts_hw_enable(void);
void ts_hw_disable(void);
bool ts_hw_enabled(void);
uint32_t ts_hw_ctrl(void);

/* Polls up to timeout_ms for a conversion latched since the last poll,
 * calling wait_1ms between polls. ts_hw_wait() and ts_hw_read_single()
 * return -RT_EINVAL for a NULL pointer or callback. */
int ts_hw_wait(uint16_t *raw, uint32_t timeout_ms, void (*wait_1ms)(void));
/* One conversion of a sensor configured for single mode: enable, wait for
 * the first conversion, disable. */
int ts_hw_read_single(int32_t *millicelsius, void (*wait_1ms)(void));

int32_t ts_hw_raw_to_mc(uint16_t raw);

#endif /* __TS_HW_H__ */
