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
const struct st_iomux_reg_t board_pinmux_cfg[K230_PIN_COUNT] = {
    /* BOOT IO */
    [0] = { .u.bit = { .st = 0, .ds = 2, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK_IO0_1, .io_sel = 1 } }, // BOOT0
    [1] = { .u.bit = { .st = 0, .ds = 2, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK_IO0_1, .io_sel = 1 } }, // BOOT1

    /* BANK0 */
    [2] = { .u.bit = { .st = 1, .ds = 4, .pd = 1, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 1 } }, // JTAG_TCK
    [3] = { .u.bit = { .st = 0, .ds = 4, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 1 } }, // JTAG_TDI
    [4] = { .u.bit = { .st = 0, .ds = 4, .pd = 0, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 1 } }, // JTAG_TDO
    [5] = { .u.bit = { .st = 0, .ds = 4, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 1 } }, // JTAG_TMS
    [6] = { .u.bit = { .st = 0, .ds = 4, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 1 } }, // JTAG_RST
    [7] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 2 } }, // IIC4_SCL
    [8] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 2 } }, // IIC4_SDA
    [9] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO9
    [10] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO10
    [11] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO11
    [12] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK0_IO2_13, .io_sel = 0 } }, // GPIO12
    [13] = { .u.bit = { .st = 1, .ds = 4, .pd = 1, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK0_IO2_13, .io_sel = 1 } }, // M_CLK1

    /* BANK1 */
    [14] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO14
    [15] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO15
    [16] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO16
    [17] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO17
    [18] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO18
    [19] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO19
    [20] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO20
    [21] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO21
    [22] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO22
    [23] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO23
    [24] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO24
    [25] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK1_IO14_25, .io_sel = 0 } }, // GPIO25

    /* BANK2 */
    [26] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO26
    [27] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO27
    [28] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO28
    [29] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO29
    [30] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO30
    [31] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO31
    [32] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO32
    [33] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO33
    [34] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO34
    [35] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 0 } }, // GPIO35
    [36] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 1 } }, // IIC3_SCL
    [37] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK2_IO26_37, .io_sel = 1 } }, // IIC3_SDA

    /* BANK3 */
    [38] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK3_IO38_49, .io_sel = 1 } }, // UART0_TXD
    [39] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 1 } }, // UART0_RXD
    [40] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 2 } }, // IIC1_SCL
    [41] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 2 } }, // IIC1_SDA
    [42] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO42
    [43] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO43
    [44] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO44
    [45] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO45
    [46] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO46
    [47] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 0 } }, // GPIO47
    [48] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 3 } }, // IIC0_SCL
    [49] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK3_IO38_49, .io_sel = 3 } }, // IIC0_SDA

    /* BANK4 */
    [50] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 1 } }, // UART3_TXD
    [51] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 0, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 1 } }, // UART3_RXD
    [52] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // GPIO52
    [53] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // GPIO53
    [54] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 2 } }, // MMC1_CMD
    [55] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 0, .msc = VOL_BANK4_IO50_61, .io_sel = 2 } }, // MMC1_CLK
    [56] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 2 } }, // MMC1_D0
    [57] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 2 } }, // MMC1_D1
    [58] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 2 } }, // MMC1_D2
    [59] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 2 } }, // MMC1_D3
    [60] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // GPIO60
    [61] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 1, .oe = 1, .ie = 1, .msc = VOL_BANK4_IO50_61, .io_sel = 0 } }, // GPIO61

    /* BANK5 */
    [62] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK5_IO62_63, .io_sel = 0 } }, // GPIO62
    [63] = { .u.bit = { .st = 1, .ds = 7, .pd = 0, .pu = 0, .oe = 1, .ie = 1, .msc = VOL_BANK5_IO62_63, .io_sel = 0 } }, // GPIO63
};
/* clang-format on */

static inline __attribute__((always_inline)) void board_specific_pin_init_sequence()
{
    
}
