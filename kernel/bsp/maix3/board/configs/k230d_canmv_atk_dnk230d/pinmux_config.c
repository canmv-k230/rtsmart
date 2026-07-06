/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
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

#include "drv_fpioa.h"

#define VOL_BANK_IO0_1     BANK_VOL_1V8_MSC
#define VOL_BANK0_IO2_13   BANK_VOL_1V8_MSC
#define VOL_BANK1_IO14_25  BANK_VOL_3V3_MSC
#define VOL_BANK2_IO26_37  BANK_VOL_3V3_MSC
#define VOL_BANK3_IO38_49  BANK_VOL_3V3_MSC
#define VOL_BANK4_IO50_61  BANK_VOL_3V3_MSC
#define VOL_BANK5_IO62_63  BANK_VOL_1V8_MSC

/* clang-format off */
const board_pinmux_cfg_t board_pinmux_cfg[FPIOA_PIN_MAX_NUM] = {
    /* BOOT IO */
    [0 ] = PINMUX_CFG(0, VOL_BANK_IO0_1, 1, 0, 1, 0, 2, 0), // GPIO0
    [1 ] = PINMUX_CFG(1, VOL_BANK_IO0_1, 1, 0, 1, 0, 2, 0), // BOOT1

    /* BANK0 */
    [2 ] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 1, 0, 1, 0, 7, 1), // GPIO2
    [3 ] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 0, 1, 0, 1, 7, 1), // GPIO3
    [4 ] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 0, 1, 0, 1, 7, 1), // GPIO4
    [5 ] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 0, 1, 0, 0, 7, 1), // GPIO5
    [6 ] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 1, 0, 0, 0, 7, 1), // GPIO6
    [7 ] = PINMUX_CFG(2, VOL_BANK0_IO2_13, 1, 1, 1, 0, 7, 1), // IIC4_SCL
    [8 ] = PINMUX_CFG(2, VOL_BANK0_IO2_13, 1, 1, 1, 0, 7, 1), // IIC4_SDA
    [9 ] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO9
    [10] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 0, 1, 0, 1, 7, 1), // GPIO10
    [11] = PINMUX_CFG(3, VOL_BANK0_IO2_13, 1, 1, 1, 0, 7, 1), // IIC2_SCL
    [12] = PINMUX_CFG(3, VOL_BANK0_IO2_13, 1, 1, 1, 0, 7, 1), // IIC2_SDA
    [13] = PINMUX_CFG(1, VOL_BANK0_IO2_13, 0, 1, 0, 1, 4, 1), // M_CLK1

    /* BANK1 */
    [14] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO14
    [15] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO15
    [16] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO16
    [17] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO17
    [18] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO18
    [19] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO19
    [20] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO20
    [21] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO21
    [22] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO22
    [23] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO23
    [24] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO24
    [25] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 1, 1, 0, 0, 15, 1), // GPIO25

    /* BANK2 */
    [26] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 0, 1, 0, 0, 7, 1), // MMC1_CLK
    [27] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_CMD
    [28] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_D0
    [29] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_D1
    [30] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_D2
    [31] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_D3
    [32] = PINMUX_CFG(0, VOL_BANK2_IO26_37, 1, 0, 1, 0, 7, 1), // GPIO32
    [33] = PINMUX_CFG(0, VOL_BANK2_IO26_37, 0, 1, 1, 0, 7, 1), // GPIO33
    [34] = PINMUX_CFG(0, VOL_BANK2_IO26_37, 1, 0, 1, 0, 7, 1), // GPIO34
    [35] = PINMUX_CFG(0, VOL_BANK2_IO26_37, 1, 0, 1, 0, 7, 1), // GPIO35
    [36] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 1, 0, 7, 1), // IIC3_SCL
    [37] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 1, 0, 7, 1), // IIC3_SDA

    /* BANK3 */
    [38] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 0, 1, 0, 0, 7, 1), // UART0_TXD
    [39] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 1, 0, 0, 0, 7, 1), // UART0_RXD
    [40] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 0, 1, 0, 0, 7, 1), // UART1_TXD
    [41] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 1, 0, 0, 0, 7, 1), // UART1_RXD
    [42] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO42
    [43] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO43
    [44] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 0, 1, 0, 0, 7, 1), // UART2_TXD
    [45] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 1, 0, 0, 0, 7, 1), // UART2_RXD
    [46] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO46
    [47] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO47
    [48] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 0, 1, 0, 0, 7, 1), // UART4_TXD
    [49] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 1, 0, 0, 0, 7, 1), // UART4_RXD

    /* BANK4 */
    [50] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO50
    [51] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO51
    [52] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO52
    [53] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO53
    [54] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO54
    [55] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO55
    [56] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO56
    [57] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO57
    [58] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO58
    [59] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 1, 1, 0, 7, 1), // GPIO59
    [60] = PINMUX_CFG(1, VOL_BANK4_IO50_61, 0, 1, 0, 0, 7, 1), // PWM0
    [61] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 1, 1, 0, 7, 1), // GPIO61

    /* BANK5 */
    [62] = PINMUX_CFG(1, VOL_BANK5_IO62_63, 0, 1, 0, 1, 4, 1), // M_CLK2
    [63] = PINMUX_CFG(0, VOL_BANK5_IO62_63, 0, 1, 0, 0, 7, 1), // GPIO63

#if FPIOA_PIN_MAX_NUM > 64
    /* PMU IO */
    [64] = PMU_GPIO(GPIO64),
    [65] = PMU_GPIO(GPIO65),
    [66] = PMU_GPIO(GPIO66),
    [67] = PMU_GPIO(GPIO67),
    [68] = PMU_GPIO(GPIO68),
    [69] = PMU_GPIO(GPIO69),
    [70] = PMU_GPIO(GPIO70),
    [71] = PMU_GPIO(GPIO71),
#endif
};
/* clang-format on */

static inline __attribute__((always_inline)) void board_specific_pin_init_sequence() { }
