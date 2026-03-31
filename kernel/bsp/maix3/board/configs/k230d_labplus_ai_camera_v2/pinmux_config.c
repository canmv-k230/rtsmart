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

#include "drv_fpioa.h"

#define VOL_BANK_IO0_1    BANK_VOL_1V8_MSC
#define VOL_BANK0_IO2_13  BANK_VOL_3V3_MSC
#define VOL_BANK1_IO14_25 BANK_VOL_3V3_MSC
#define VOL_BANK2_IO26_37 BANK_VOL_3V3_MSC
#define VOL_BANK3_IO38_49 BANK_VOL_3V3_MSC
#define VOL_BANK4_IO50_61 BANK_VOL_3V3_MSC
#define VOL_BANK5_IO62_63 BANK_VOL_3V3_MSC

/* clang-format off */
#if 0
const fpioa_iomux_cfg_t board_pinmux_cfg[FPIOA_PIN_MAX_NUM] = {
    /* BOOT IO */
    [0] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK_IO0_1, .io_sel = 0 } }, // GPIO0
    [1] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK_IO0_1, .io_sel = 0 } }, // GPIO1

    /* BANK0 */
    [2] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO2
    [3] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 3 } }, // UART1_TXD
    [4] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 3 } }, // UART1_RXD
    [5] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 3 } }, // UART2_TXD
    [6] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 3 } }, // UART2_RXD
    [7] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 0, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO7
    [8] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO8
    [10] = { .u.bit = { .st = 1, .ds = 4, .pd = 0, .pu = 1, .oe = 0, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO10
    [11] = { .u.bit = { .st = 1, .ds = 4, .pd = 0, .pu = 1, .oe = 0, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO11
    [12] = { .u.bit = { .st = 1, .ds = 4, .pd = 0, .pu = 1, .oe = 0, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO12
    [13] = { .u.bit = { .st = 1, .ds = 4, .pd = 1, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 1 } }, // M_CLK1

    /* BANK1 */
    [14] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO14
    [15] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO15
    [16] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO16
    [17] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO17
    [18] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO18
    [19] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO19
    [20] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO20
    [21] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO21
    [22] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO22
    [23] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO23
    [24] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO24
    [25] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO25

    /* BANK2 */
    [26] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK2_IO26_37, .io_sel = 1 } }, // MMC1_CLK
    [27] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 1 } }, // MMC1_CMD
    [28] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 1 } }, // MMC1_D0
    [29] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 1 } }, // MMC1_D1
    [30] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 1 } }, // MMC1_D2
    [31] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 1 } }, // MMC1_D3
    [32] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO32
    [33] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO33
    [34] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO34
    [35] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO35
    [36] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 0, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO36
    [37] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 0, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO37

    /* BANK3 */
    [38] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 1 } }, // UART0_TXD
    [39] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 1 } }, // UART0_RXD
    [40] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO40
    [41] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO41
    [44] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO44
    [45] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO45
    [48] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 3 } }, // IIC0_SCL
    [49] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 3 } }, // IIC0_SDA

    /* BANK4 */
    [59] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // GPIO59
    [60] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // GPIO60
    [61] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // GPIO61

    /* BANK5 */
    [62] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 0, .msc = VOL_BANK5_IO62_63, .io_sel = 0 } }, // GPIO62
    [63] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK5_IO62_63, .io_sel = 0 } }, // GPIO63

    /* DROP PINS */
    [9] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // NC
    [42] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // NC
    [43] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // NC
    [46] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // NC
    [47] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // NC
    [50] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
    [51] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
    [52] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
    [53] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
    [54] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
    [55] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
    [56] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
    [57] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
    [58] = { .u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // NC
};
#endif

const board_pinmux_cfg_t board_pinmux_cfg[FPIOA_PIN_MAX_NUM] = {
    /* BOOT IO */
    [0] = { .func = GPIO0, .cfg = { .u.value = 0 } }, // GPIO0
    [1] = { .func = GPIO1, .cfg = { .u.value = 0 } }, // GPIO1

    /* BANK0 */
    [2] = { .func = GPIO2, .cfg = { .u.value = 0 } }, // GPIO2
    [3] = { .func = UART1_TXD, .cfg = { .u.value = 0 } }, // UART1_TXD
    [4] = { .func = UART1_RXD, .cfg = { .u.value = 0 } }, // UART1_RXD
    [5] = { .func = UART2_TXD, .cfg = { .u.value = 0 } }, // UART2_TXD
    [6] = { .func = UART2_RXD, .cfg = { .u.value = 0 } }, // UART2_RXD
    [7] = { .func = GPIO7, .cfg = { .u.value = 0 } }, // GPIO7
    [8] = { .func = GPIO8, .cfg = { .u.value = 0 } }, // GPIO8
    [10] = { .func = GPIO10, .cfg = { .u.value = 0 } }, // GPIO10
    [11] = { .func = GPIO11, .cfg = { .u.value = 0 } }, // GPIO11
    [12] = { .func = GPIO12, .cfg = { .u.value = 0 } }, // GPIO12
    [13] = { .func = M_CLK1, .cfg = { .u.value = 0 } }, // M_CLK1

    /* BANK1 */
    [14] = { .func = GPIO14, .cfg = { .u.value = 0 } }, // GPIO14
    [15] = { .func = GPIO15, .cfg = { .u.value = 0 } }, // GPIO15
    [16] = { .func = GPIO16, .cfg = { .u.value = 0 } }, // GPIO16
    [17] = { .func = GPIO17, .cfg = { .u.value = 0 } }, // GPIO17
    [18] = { .func = GPIO18, .cfg = { .u.value = 0 } }, // GPIO18
    [19] = { .func = GPIO19, .cfg = { .u.value = 0 } }, // GPIO19
    [20] = { .func = GPIO20, .cfg = { .u.value = 0 } }, // GPIO20
    [21] = { .func = GPIO21, .cfg = { .u.value = 0 } }, // GPIO21
    [22] = { .func = GPIO22, .cfg = { .u.value = 0 } }, // GPIO22
    [23] = { .func = GPIO23, .cfg = { .u.value = 0 } }, // GPIO23
    [24] = { .func = GPIO24, .cfg = { .u.value = 0 } }, // GPIO24
    [25] = { .func = GPIO25, .cfg = { .u.value = 0 } }, // GPIO25

    /* BANK2 */
    [26] = { .func = MMC1_CLK, .cfg = { .u.value = 0 } }, // MMC1_CLK
    [27] = { .func = MMC1_CMD, .cfg = { .u.value = 0 } }, // MMC1_CMD
    [28] = { .func = MMC1_D0, .cfg = { .u.value = 0 } }, // MMC1_D0
    [29] = { .func = MMC1_D1, .cfg = { .u.value = 0 } }, // MMC1_D1
    [30] = { .func = MMC1_D2, .cfg = { .u.value = 0 } }, // MMC1_D2
    [31] = { .func = MMC1_D3, .cfg = { .u.value = 0 } }, // MMC1_D3
    [32] = { .func = GPIO32, .cfg = { .u.value = 0 } }, // GPIO32
    [33] = { .func = GPIO33, .cfg = { .u.value = 0 } }, // GPIO33
    [34] = { .func = GPIO34, .cfg = { .u.value = 0 } }, // GPIO34
    [35] = { .func = GPIO35, .cfg = { .u.value = 0 } }, // GPIO35
    [36] = { .func = GPIO36, .cfg = { .u.value = 0 } }, // GPIO36
    [37] = { .func = GPIO37, .cfg = { .u.value = 0 } }, // GPIO37

    /* BANK3 */
    [38] = { .func = UART0_TXD, .cfg = { .u.value = 0 } }, // UART0_TXD
    [39] = { .func = UART0_RXD, .cfg = { .u.value = 0 } }, // UART0_RXD
    [40] = { .func = GPIO40, .cfg = { .u.value = 0 } }, // GPIO40
    [41] = { .func = GPIO41, .cfg = { .u.value = 0 } }, // GPIO41
    [44] = { .func = GPIO44, .cfg = { .u.value = 0 } }, // GPIO44
    [45] = { .func = GPIO45, .cfg = { .u.value = 0 } }, // GPIO45
    [48] = { .func = IIC0_SCL, .cfg = { .u.value = 0 } }, // IIC0_SCL
    [49] = { .func = IIC0_SDA, .cfg = { .u.value = 0 } }, // IIC0_SDA

    /* BANK4 */
    [59] = { .func = GPIO59, .cfg = { .u.value = 0 } }, // GPIO59
    [60] = { .func = GPIO60, .cfg = { .u.value = 0 } }, // GPIO60
    [61] = { .func = GPIO61, .cfg = { .u.value = 0 } }, // GPIO61

    /* BANK5 */
    [62] = { .func = GPIO62, .cfg = { .u.value = 0 } }, // GPIO62
    [63] = { .func = GPIO63, .cfg = { .u.value = 0 } }, // GPIO63

    /* DROP PINS */
    [9]  = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } } }, // NC
    [42] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } } }, // NC
    [43] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } } }, // NC
    [46] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } } }, // NC
    [47] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } } }, // NC
    [50] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
    [51] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
    [52] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
    [53] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
    [54] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
    [55] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
    [56] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
    [57] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
    [58] = { .func = FUNC_MAX, .cfg = {.u.bit = { .st = 0, .ds = 0, .pd = 0, .pu = 0, .oe = 0, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } } }, // NC
};
/* clang-format on */

static inline __attribute__((always_inline)) void board_specific_pin_init_sequence() { }
