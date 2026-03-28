/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef DRV_GPIO_H__
#define DRV_GPIO_H__

#include "rtdef.h"

#define GPIO_IRQ_MAX_NUM (64)
#ifdef CONFIG_BOARD_NOT_SUPPORT_HW_RTC
#define GPIO_MAX_NUM     (64)
#else
#define GPIO_MAX_NUM     (64 + 8)
#endif

#define KD_GPIO_IRQ_DISABLE 0x00
#define KD_GPIO_IRQ_ENABLE  0x01

typedef enum _gpio_pin_edge {
    GPIO_PE_RISING  = 0,
    GPIO_PE_FALLING = 1,
    GPIO_PE_BOTH    = 2,
    GPIO_PE_HIGH    = 3,
    GPIO_PE_LOW     = 4,
} gpio_pin_edge_t;

typedef enum _gpio_drive_mode {
    GPIO_DM_OUTPUT         = 0,
    GPIO_DM_INPUT          = 1,
    GPIO_DM_INPUT_PULLUP   = 2,
    GPIO_DM_INPUT_PULLDOWN = 3,
    GPIO_DM_OUTPUT_OD      = 4,
} gpio_drive_mode_t;

typedef enum _gpio_pin_value { GPIO_PV_LOW, GPIO_PV_HIGH } gpio_pin_value_t;

typedef struct {
    rt_uint16_t pin;
    rt_uint16_t value;
} gpio_cfg_t;

typedef struct {
    rt_uint16_t pin;
    rt_uint16_t mode; // @ref gpio_pin_edge_t
    rt_uint16_t debounce_ms;
    rt_uint16_t signo;
    void*       sigval; // reuse as callback
} gpio_irqcfg_t;

int kd_pin_init(void);

rt_err_t kd_pin_attach_irq(rt_base_t pin, rt_uint32_t mode, void (*hdr)(void* args), void* args);
rt_err_t kd_pin_detach_irq(rt_base_t pin);

rt_err_t kd_pin_irq_enable(rt_base_t pin, rt_uint32_t enabled);

rt_err_t kd_pin_mode(rt_base_t pin, rt_base_t mode);
rt_err_t kd_pin_mode_get(rt_base_t pin, rt_base_t* mode);

rt_err_t kd_pin_write(rt_base_t pin, rt_base_t value);
rt_err_t kd_pin_read(rt_base_t pin);

/* fast api */

/* gpio output or input */
uint32_t kd_pin_get_ddr(rt_base_t pin);
/* set output or input */
void kd_pin_set_dr(rt_base_t pin, int value);

/* get gpio input value */
uint32_t kd_pin_get_dr(rt_base_t pin);
/* set gpio output value */
void kd_pin_set_dr(rt_base_t pin, int value);

#endif
