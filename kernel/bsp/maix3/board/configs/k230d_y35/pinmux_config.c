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

#define VOL_BANK_IO0_1    BANK_VOL_1V8_MSC
#define VOL_BANK0_IO2_13  BANK_VOL_3V3_MSC
#define VOL_BANK1_IO14_25 BANK_VOL_3V3_MSC
#define VOL_BANK2_IO26_37 BANK_VOL_3V3_MSC
#define VOL_BANK3_IO38_49 BANK_VOL_3V3_MSC
#define VOL_BANK4_IO50_61 BANK_VOL_3V3_MSC
#define VOL_BANK5_IO62_63 BANK_VOL_3V3_MSC

/* clang-format off */
const board_pinmux_cfg_t board_pinmux_cfg[FPIOA_PIN_MAX_NUM] = {
    /* BOOT IO */
    [0 ] = PINMUX_CFG(0, VOL_BANK_IO0_1, 0, 0, 0, 0, 0, 0), // GPIO0 (NC)
    [1 ] = PINMUX_CFG(0, VOL_BANK_IO0_1, 1, 1, 0, 0, 7, 1), // GPIO1

    /* BANK0: CAM3/I2C4 on IO7/8, sensor reset on IO10/11/12, MCLK1 on IO13 */
    [2 ] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 0, 0, 0, 0, 0, 0), // GPIO2 (NC)
    [3 ] = PINMUX_CFG(3, VOL_BANK0_IO2_13, 0, 1, 0, 0, 7, 0), // UART1_TXD
    [4 ] = PINMUX_CFG(3, VOL_BANK0_IO2_13, 1, 0, 0, 0, 0, 1), // UART1_RXD
    [5 ] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 0, 0, 0, 0, 0, 0), // GPIO5 (NC)
    [6 ] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 0, 0, 0, 0, 0, 0), // GPIO6 (NC)
    [7 ] = PINMUX_CFG(2, VOL_BANK0_IO2_13, 1, 1, 1, 0, 7, 1), // IIC4_SCL (CSI2 / CAM3)
    [8 ] = PINMUX_CFG(2, VOL_BANK0_IO2_13, 1, 1, 1, 0, 7, 1), // IIC4_SDA (CSI2 / CAM3)
    [9 ] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO9 (drop)
    [10] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 1, 1, 0, 0, 7, 1), // GPIO10 (CSI1 reset)
    [11] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 1, 1, 0, 0, 7, 1), // GPIO11 (CSI2 reset)
    [12] = PINMUX_CFG(0, VOL_BANK0_IO2_13, 1, 1, 0, 0, 7, 1), // GPIO12 (CSI0 reset)
    [13] = PINMUX_CFG(1, VOL_BANK0_IO2_13, 0, 1, 0, 0, 7, 0), // M_CLK1 (CSI0 / CAM1)

    /* BANK1 */
    [14] = PINMUX_CFG(1, VOL_BANK1_IO14_25, 0, 1, 1, 0, 7, 0), // OSPI_CS
    [15] = PINMUX_CFG(1, VOL_BANK1_IO14_25, 0, 1, 0, 0, 7, 0), // OSPI_CLK
    [16] = PINMUX_CFG(3, VOL_BANK1_IO14_25, 1, 1, 0, 0, 7, 1), // QSPI0_D0
    [17] = PINMUX_CFG(3, VOL_BANK1_IO14_25, 1, 1, 0, 0, 7, 1), // QSPI0_D1
    [18] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 0, 0, 0, 0, 0, 0), // GPIO18 (NC)
    [19] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 0, 0, 0, 0, 0, 0), // GPIO19 (NC)
    [20] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 0, 0, 0, 0, 0, 0), // GPIO20 (NC)
    [21] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 0, 0, 0, 0, 0, 0), // GPIO21 (NC)
    [22] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 0, 0, 0, 0, 0, 0), // GPIO22 (NC)
    [23] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 0, 0, 0, 0, 0, 0), // GPIO23 (LCD backlight)
    [24] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 0, 0, 0, 0, 0, 0), // GPIO24 (LCD reset)
    [25] = PINMUX_CFG(0, VOL_BANK1_IO14_25, 0, 0, 0, 0, 0, 0), // GPIO25 (NC)

    /* BANK2: SD/MMC1 + WiFi reset on IO35 */
    [26] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 0, 1, 0, 0, 7, 1), // MMC1_CLK (Wi-Fi SDIO)
    [27] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_CMD (Wi-Fi SDIO)
    [28] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_D0 (Wi-Fi SDIO)
    [29] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_D1 (Wi-Fi SDIO)
    [30] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_D2 (Wi-Fi SDIO)
    [31] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // MMC1_D3 (Wi-Fi SDIO)
    [32] = PINMUX_CFG(0, VOL_BANK2_IO26_37, 0, 0, 0, 0, 0, 0), // GPIO32 (NC)
    [33] = PINMUX_CFG(0, VOL_BANK2_IO26_37, 0, 0, 0, 0, 0, 0), // GPIO33 (NC)
    [34] = PINMUX_CFG(0, VOL_BANK2_IO26_37, 0, 0, 0, 0, 0, 0), // GPIO34 (NC)
    [35] = PINMUX_CFG(0, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // GPIO35 (RTL8189 reset)
    [36] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // IIC3_SCL
    [37] = PINMUX_CFG(1, VOL_BANK2_IO26_37, 1, 1, 0, 0, 7, 1), // IIC3_SDA

    /* BANK3: CAM2/I2C1 on IO40/41, CAM1/I2C0 on IO48/49, console UART2 on IO44/45 */
    [38] = PINMUX_CFG(0, VOL_BANK3_IO38_49, 0, 0, 0, 0, 0, 0), // GPIO38 (NC)
    [39] = PINMUX_CFG(0, VOL_BANK3_IO38_49, 1, 1, 0, 0, 7, 1), // GPIO39
    [40] = PINMUX_CFG(2, VOL_BANK3_IO38_49, 1, 1, 1, 0, 7, 1), // IIC1_SCL (CSI1 / CAM2)
    [41] = PINMUX_CFG(2, VOL_BANK3_IO38_49, 1, 1, 1, 0, 7, 1), // IIC1_SDA (CSI1 / CAM2)
    [42] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO42 (drop)
    [43] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO43 (drop)
    [44] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 0, 1, 0, 0, 7, 1), // UART2_TXD (console)
    [45] = PINMUX_CFG(1, VOL_BANK3_IO38_49, 1, 0, 0, 0, 7, 1), // UART2_RXD (console)
    [46] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO46 (drop)
    [47] = PINMUX_CFG(0, 0, 0, 0, 0, 0, 7, 1), // GPIO47 (drop)
    [48] = PINMUX_CFG(3, VOL_BANK3_IO38_49, 1, 1, 1, 0, 7, 1), // IIC0_SCL (CSI0 / CAM1)
    [49] = PINMUX_CFG(3, VOL_BANK3_IO38_49, 1, 1, 1, 0, 7, 1), // IIC0_SDA (CSI0 / CAM1)

    /* BANK4 */
    [50] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO50 (drop)
    [51] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO51 (drop)
    [52] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO52 (drop)
    [53] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO53 (drop)
    [54] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO54 (drop)
    [55] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO55 (drop)
    [56] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO56 (drop)
    [57] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO57 (drop)
    [58] = PINMUX_CFG(0, VOL_BANK4_IO50_61, 0, 0, 0, 0, 7, 1), // GPIO58 (drop)
    [59] = PINMUX_CFG(3, VOL_BANK4_IO50_61, 0, 1, 0, 0, 7, 0), // PWM5
    [60] = PINMUX_CFG(1, VOL_BANK4_IO50_61, 0, 1, 0, 0, 7, 0), // PWM0
    [61] = PINMUX_CFG(1, VOL_BANK4_IO50_61, 0, 1, 0, 0, 7, 0), // PWM1

    /* BANK5: camera MCLK2/3 */
    [62] = PINMUX_CFG(1, VOL_BANK5_IO62_63, 0, 1, 0, 0, 7, 0), // M_CLK2 (CSI1 / CAM2)
    [63] = PINMUX_CFG(1, VOL_BANK5_IO62_63, 0, 1, 0, 0, 7, 0), // M_CLK3 (CSI2 / CAM3)
};
/* clang-format on */

static inline __attribute__((always_inline)) void board_specific_pin_init_sequence() { }
