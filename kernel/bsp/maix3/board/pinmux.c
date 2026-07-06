/* Copyright (c) 2025, Canaan Bright Sight Co., Ltd
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
#include <stdint.h>
#include <stdio.h>

#include "rtconfig.h"
#include "rtdebug.h"
#include "rtdef.h"
#include "rtthread.h"

#include "drv_fpioa.h"
#include "drv_gpio.h"

static int pin_output(int pin, int val);

/* if func is FUNC_MAX, use cfg, else use func default */
typedef struct _board_pinmux_cfg_t {
    fpioa_func_t      func; /* function */
    fpioa_iomux_cfg_t cfg; /* pinmux configuration */
} board_pinmux_cfg_t;


#define PINMUX_CFG(_sel, _msc, _ie, _oe, _pu, _pd, _ds, _st) \
    { \
        .func = FUNC_MAX, \
        .cfg = { .u.bit = { .st = (_st), .ds = (_ds), .pd = (_pd), .pu = (_pu), .oe = (_oe), .ie = (_ie), .msc = (_msc), .io_sel = (_sel) } } \
    }

#define PMU_GPIO(_func) \
    { \
        .func = (_func), .cfg = { .u.value = 0 } \
    }

#define STRINGIFY(x) #x
#define TOSTRING(x)  STRINGIFY(x)

/* include configs/{CONFIG_BOARD}/pinmux_config.c */
#include TOSTRING(BOARD_CFG_FILE)

static int board_get_pin_bank_voltage(int pin)
{
    if (pin < 0 || pin >= FPIOA_PIN_MAX_NUM) {
        rt_kprintf("Invalid pin number: %d\n", pin);
        return -1;
    }

    switch (pin) {
    case 0 ... 1:
        return VOL_BANK_IO0_1;
    case 2 ... 13:
        return VOL_BANK0_IO2_13;
    case 14 ... 25:
        return VOL_BANK1_IO14_25;
    case 26 ... 37:
        return VOL_BANK2_IO26_37;
    case 38 ... 49:
        return VOL_BANK3_IO38_49;
    case 50 ... 61:
        return VOL_BANK4_IO50_61;
    case 62 ... 63:
        return VOL_BANK5_IO62_63;
    case 64 ... 71:
        return BANK_VOL_1V8_MSC; // fixed to 1.8V.
    default:
        rt_kprintf("Pin %d does not have a defined voltage bank\n", pin);
        return -1;
    }
}

int board_pinmux_init(void)
{
    int               bank_voltage;
    fpioa_iomux_cfg_t curr_cfg;

    if (sizeof(board_pinmux_cfg) / sizeof(board_pinmux_cfg[0]) != FPIOA_PIN_MAX_NUM) {
        rt_kprintf("Pinmux configuration size mismatch: expected %d, got %zu\n", FPIOA_PIN_MAX_NUM,
                   sizeof(board_pinmux_cfg) / sizeof(board_pinmux_cfg[0]));
        RT_ASSERT(sizeof(board_pinmux_cfg) / sizeof(board_pinmux_cfg[0]) == FPIOA_PIN_MAX_NUM);
        return -1;
    }

    /* set bank voltage */
    for (int i = 0; i < FPIOA_PIN_MAX_NUM; i++) {
        if (0 > (bank_voltage = board_get_pin_bank_voltage(i))) {
            rt_kprintf("Failed to get voltage for pin %d\n", i);
            RT_ASSERT(0);
            continue;
        }

        if (0x00 != drv_fpioa_get_pin_cfg(i, &curr_cfg.u.value)) {
            rt_kprintf("Failed to get pin %d configuration\n", i);
            RT_ASSERT(0);
            continue;
        }
        curr_cfg.u.bit.msc = bank_voltage & 0x01;
        if (0x00 != drv_fpioa_set_pin_cfg(i, curr_cfg.u.value)) {
            rt_kprintf("Failed to set pin %d configuration\n", i);
            RT_ASSERT(0);
            continue;
        }
    }

    for (int i = 0; i < FPIOA_PIN_MAX_NUM; i++) {
        if (board_pinmux_cfg[i].func == FUNC_MAX) {
            /* use cfg */
            if (0x00 != drv_fpioa_set_pin_cfg(i, board_pinmux_cfg[i].cfg.u.value)) {
                rt_kprintf("Failed to set pin %d configuration\n", i);
                RT_ASSERT(0);
                continue;
            }
        } else {
            /* use func */
            if (0x00 != drv_fpioa_set_pin_func(i, board_pinmux_cfg[i].func)) {
                rt_kprintf("Failed to set pin %d function %d\n", i, board_pinmux_cfg[i].func);
                RT_ASSERT(0);
                continue;
            }
        }
    }

    return 0;
}

static int pin_output(int pin, int val)
{
    int ret = 0;

    ret += kd_pin_mode(pin, GPIO_DM_OUTPUT);
    ret += kd_pin_write(pin, val);

    return ret;
}

#ifdef RT_BOARD_ENABLE_PIN_INIT_SEQUENCE
static int _board_specific_pin_init_sequence_wrap()
{
    board_specific_pin_init_sequence();

    return 0;
}
INIT_DEVICE_EXPORT_SEQ(_board_specific_pin_init_sequence_wrap, 1); // after kd_pin_init
#endif
