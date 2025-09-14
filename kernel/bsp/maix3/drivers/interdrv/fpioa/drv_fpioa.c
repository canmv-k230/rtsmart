/* Copyright (c) 2024, Canaan Bright Sight Co., Ltd
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
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "rtthread.h"

#include <ioremap.h>

#include "riscv_io.h"

#include "drv_fpioa.h"

#define write32(addr, value) writel(value, (volatile void*)(rt_uint64_t)(addr))
#define read32(addr)         readl((const volatile void*)(rt_uint64_t)(addr))

#pragma pack(1)

static const fpioa_func_cfg_t g_func_describ_array[] = {
    { BOOT0, "BOOT0", 0x10F },
    { BOOT1, "BOOT1", 0x10F },
    { CI0, "RESV", 0x10F },
    { CI1, "RESV", 0x10F },
    { CI2, "RESV", 0x10F },
    { CI3, "RESV", 0x10F },
    { CO0, "RESV", 0x08E },
    { CO1, "RESV", 0x08E },
    { CO2, "RESV", 0x08E },
    { CO3, "RESV", 0x08E },
    { DI0, "RESV", 0x10F },
    { DI1, "RESV", 0x10F },
    { DI2, "RESV", 0x10F },
    { DI3, "RESV", 0x10F },
    { DO0, "RESV", 0x08E },
    { DO1, "RESV", 0x08E },
    { DO2, "RESV", 0x08E },
    { DO3, "RESV", 0x08E },
    { HSYNC0, "HSYNC0", 0x08E },
    { HSYNC1, "HSYNC1", 0x08E },
    { IIC0_SCL, "IIC0_SCL", 0x18F },
    { IIC0_SDA, "IIC0_SDA", 0x18F },
    { IIC1_SCL, "IIC1_SCL", 0x18F },
    { IIC1_SDA, "IIC1_SDA", 0x18F },
    { IIC2_SCL, "IIC2_SCL", 0x18F },
    { IIC2_SDA, "IIC2_SDA", 0x18F },
    { IIC3_SCL, "IIC3_SCL", 0x18F },
    { IIC3_SDA, "IIC3_SDA", 0x18F },
    { IIC4_SCL, "IIC4_SCL", 0x18F },
    { IIC4_SDA, "IIC4_SDA", 0x18F },
    { IIS_CLK, "IIS_CLK", 0x08E },
    { IIS_D_IN0_PDM_IN3, "IIS_D_IN0_PDM_IN3", 0x10F },
    { IIS_D_IN1_PDM_IN2, "IIS_D_IN1_PDM_IN2", 0x10F },
    { IIS_D_OUT0_PDM_IN1, "IIS_D_OUT0_PDM_IN1", 0x08E },
    { IIS_D_OUT1_PDM_IN0, "IIS_D_OUT1_PDM_IN0", 0x08E },
    { IIS_WS, "IIS_WS", 0x08E },
    { JTAG_RST, "JTAG_RST", 0x14F },
    { JTAG_TCK, "JTAG_TCK", 0x12F },
    { JTAG_TDI, "JTAG_TDI", 0x12F },
    { JTAG_TDO, "JTAG_TDO", 0x08E },
    { JTAG_TMS, "JTAG_TMS", 0x12F },
    { M_CLK1, "M_CLK1", 0x08E },
    { M_CLK2, "M_CLK2", 0x08E },
    { M_CLK3, "M_CLK3", 0x08E },
    { MMC1_CLK, "MMC1_CLK", 0x08E },
    { MMC1_CMD, "MMC1_CMD", 0x1CF },
    { MMC1_D0, "MMC1_D0", 0x1CF },
    { MMC1_D1, "MMC1_D1", 0x1CF },
    { MMC1_D2, "MMC1_D2", 0x1CF },
    { MMC1_D3, "MMC1_D3", 0x1CF },
    { OSPI_CLK, "OSPI_CLK", 0x08E },
    { OSPI_CS, "OSPI_CS", 0x0CE },
    { OSPI_D0, "OSPI_D0", 0x18F },
    { OSPI_D1, "OSPI_D1", 0x18F },
    { OSPI_D2, "OSPI_D2", 0x18F },
    { OSPI_D3, "OSPI_D3", 0x18F },
    { OSPI_D4, "OSPI_D4", 0x18F },
    { OSPI_D5, "OSPI_D5", 0x18F },
    { OSPI_D6, "OSPI_D6", 0x18F },
    { OSPI_D7, "OSPI_D7", 0x18F },
    { OSPI_DQS, "OSPI_DQS", 0x10F },
    { PDM_IN0, "PDM_IN0", 0x10F },
    { PDM_IN1, "PDM_IN1", 0x10F },
    { PDM_IN2, "PDM_IN2", 0x10F },
    { PDM_IN3, "PDM_IN3", 0x10F },
    { PULSE_CNTR0, "PULSE_CNTR0", 0x10F },
    { PULSE_CNTR1, "PULSE_CNTR1", 0x10F },
    { PULSE_CNTR2, "PULSE_CNTR2", 0x10F },
    { PULSE_CNTR3, "PULSE_CNTR3", 0x10F },
    { PULSE_CNTR4, "PULSE_CNTR4", 0x10F },
    { PULSE_CNTR5, "PULSE_CNTR5", 0x10F },
    { PWM0, "PWM0", 0x08E },
    { PWM1, "PWM1", 0x08E },
    { PWM2, "PWM2", 0x08E },
    { PWM3, "PWM3", 0x08E },
    { PWM4, "PWM4", 0x08E },
    { PWM5, "PWM5", 0x08E },
    { QSPI0_CLK, "QSPI0_CLK", 0x08E },
    { QSPI0_CS0, "QSPI0_CS0", 0x0CE },
    { QSPI0_CS1, "QSPI0_CS1", 0x0CE },
    { QSPI0_CS2, "QSPI0_CS2", 0x0CE },
    { QSPI0_CS3, "QSPI0_CS3", 0x0CE },
    { QSPI0_CS4, "QSPI0_CS4", 0x0CE },
    { QSPI0_D0, "QSPI0_D0", 0x18F },
    { QSPI0_D1, "QSPI0_D1", 0x18F },
    { QSPI0_D2, "QSPI0_D2", 0x18F },
    { QSPI0_D3, "QSPI0_D3", 0x18F },
    { QSPI1_CLK, "QSPI1_CLK", 0x08E },
    { QSPI1_CS0, "QSPI1_CS0", 0x0CE },
    { QSPI1_CS1, "QSPI1_CS1", 0x0CE },
    { QSPI1_CS2, "QSPI1_CS2", 0x0CE },
    { QSPI1_CS3, "QSPI1_CS3", 0x0CE },
    { QSPI1_CS4, "QSPI1_CS4", 0x0CE },
    { QSPI1_D0, "QSPI1_D0", 0x18F },
    { QSPI1_D1, "QSPI1_D1", 0x18F },
    { QSPI1_D2, "QSPI1_D2", 0x18F },
    { QSPI1_D3, "QSPI1_D3", 0x18F },
    { SPI2AXI_CK, "SPI2AXI_CK", 0x10F },
    { SPI2AXI_CS, "SPI2AXI_CS", 0x14F },
    { SPI2AXI_DI, "SPI2AXI_DI", 0x10F },
    { SPI2AXI_DO, "SPI2AXI_DO", 0x08E },
    { UART0_RXD, "UART0_RXD", 0x10F },
    { UART0_TXD, "UART0_TXD", 0x08E },
    { UART1_CTS, "UART1_CTS", 0x10F },
    { UART1_RTS, "UART1_RTS", 0x08E },
    { UART1_RXD, "UART1_RXD", 0x10F },
    { UART1_TXD, "UART1_TXD", 0x08E },
    { UART2_CTS, "UART2_CTS", 0x10F },
    { UART2_RTS, "UART2_RTS", 0x08E },
    { UART2_RXD, "UART2_RXD", 0x10F },
    { UART2_TXD, "UART2_TXD", 0x08E },
    { UART3_CTS, "UART3_CTS", 0x10F },
    { UART3_DE, "UART3_DE", 0x08E },
    { UART3_RE, "UART3_RE", 0x08E },
    { UART3_RTS, "UART3_RTS", 0x08E },
    { UART3_RXD, "UART3_RXD", 0x10F },
    { UART3_TXD, "UART3_TXD", 0x08E },
    { UART4_RXD, "UART4_RXD", 0x10F },
    { UART4_TXD, "UART4_TXD", 0x08E },
    { PDM_CLK, "PDM_CLK", 0x08E },
    { VSYNC0, "VSYNC0", 0x08E },
    { VSYNC1, "VSYNC1", 0x08E },
    { CTRL_IN_3D, "CTRL_IN_3D", 0x10F },
    { CTRL_O1_3D, "CTRL_O1_3D", 0x08E },
    { CTRL_O2_3D, "CTRL_O2_3D", 0x08E },
};

static const uint8_t g_pin_func_array[][FPIOA_PIN_MAX_FUNCS] = {
    { GPIO0, BOOT0, TEST_PIN0, FUNC_MAX, FUNC_MAX },
    { GPIO1, BOOT1, TEST_PIN1, FUNC_MAX, FUNC_MAX },
    { GPIO2, JTAG_TCK, PULSE_CNTR0, TEST_PIN2, FUNC_MAX },
    { GPIO3, JTAG_TDI, PULSE_CNTR1, UART1_TXD, TEST_PIN0 },
    { GPIO4, JTAG_TDO, PULSE_CNTR2, UART1_RXD, TEST_PIN1 },
    { GPIO5, JTAG_TMS, PULSE_CNTR3, UART2_TXD, TEST_PIN2 },
    { GPIO6, JTAG_RST, PULSE_CNTR4, UART2_RXD, TEST_PIN3 },
    { GPIO7, PWM2, IIC4_SCL, TEST_PIN3, DI0 },
    { GPIO8, PWM3, IIC4_SDA, TEST_PIN4, DI1 },
    { GPIO9, PWM4, UART1_TXD, IIC1_SCL, DI2 },
    { GPIO10, CTRL_IN_3D, UART1_RXD, IIC1_SDA, DI3 },
    { GPIO11, CTRL_O1_3D, UART2_TXD, IIC2_SCL, DO0 },
    { GPIO12, CTRL_O2_3D, UART2_RXD, IIC2_SDA, DO1 },
    { GPIO13, M_CLK1, DO2, FUNC_MAX, FUNC_MAX },
    { GPIO14, OSPI_CS, TEST_PIN5, QSPI0_CS0, DO3 },
    { GPIO15, OSPI_CLK, TEST_PIN6, QSPI0_CLK, CO3 },
    { GPIO16, OSPI_D0, QSPI1_CS4, QSPI0_D0, CO2 },
    { GPIO17, OSPI_D1, QSPI1_CS3, QSPI0_D1, CO1 },
    { GPIO18, OSPI_D2, QSPI1_CS2, QSPI0_D2, CO0 },
    { GPIO19, OSPI_D3, QSPI1_CS1, QSPI0_D3, TEST_PIN4 },
    { GPIO20, OSPI_D4, QSPI1_CS0, PULSE_CNTR0, TEST_PIN5 },
    { GPIO21, OSPI_D5, QSPI1_CLK, PULSE_CNTR1, TEST_PIN6 },
    { GPIO22, OSPI_D6, QSPI1_D0, PULSE_CNTR2, TEST_PIN7 },
    { GPIO23, OSPI_D7, QSPI1_D1, PULSE_CNTR3, TEST_PIN8 },
    { GPIO24, OSPI_DQS, QSPI1_D2, PULSE_CNTR4, TEST_PIN9 },
    { GPIO25, PWM5, QSPI1_D3, PULSE_CNTR5, TEST_PIN10 },
    { GPIO26, MMC1_CLK, TEST_PIN7, PDM_CLK, FUNC_MAX },
    { GPIO27, MMC1_CMD, PULSE_CNTR5, PDM_IN0, CI0 },
    { GPIO28, MMC1_D0, UART3_TXD, PDM_IN1, CI1 },
    { GPIO29, MMC1_D1, UART3_RXD, CTRL_IN_3D, CI2 },
    { GPIO30, MMC1_D2, UART3_RTS, CTRL_O1_3D, CI3 },
    { GPIO31, MMC1_D3, UART3_CTS, CTRL_O2_3D, TEST_PIN11 },
    { GPIO32, IIC0_SCL, IIS_CLK, UART3_TXD, TEST_PIN12 },
    { GPIO33, IIC0_SDA, IIS_WS, UART3_RXD, TEST_PIN13 },
    { GPIO34, IIC1_SCL, IIS_D_IN0_PDM_IN3, UART3_RTS, FUNC_MAX },
    { GPIO35, IIC1_SDA, IIS_D_OUT0_PDM_IN1, UART3_CTS, FUNC_MAX },
    { GPIO36, IIC3_SCL, IIS_D_IN1_PDM_IN2, UART4_TXD, FUNC_MAX },
    { GPIO37, IIC3_SDA, IIS_D_OUT1_PDM_IN0, UART4_RXD, FUNC_MAX },
    { GPIO38, UART0_TXD, TEST_PIN8, QSPI1_CS0, HSYNC0 },
    { GPIO39, UART0_RXD, TEST_PIN9, QSPI1_CLK, VSYNC0 },
    { GPIO40, UART1_TXD, IIC1_SCL, QSPI1_D0, TEST_PIN18 },
    { GPIO41, UART1_RXD, IIC1_SDA, QSPI1_D1, TEST_PIN19 },
    { GPIO42, UART1_RTS, PWM0, QSPI1_D2, TEST_PIN20 },
    { GPIO43, UART1_CTS, PWM1, QSPI1_D3, TEST_PIN21 },
    { GPIO44, UART2_TXD, IIC3_SCL, TEST_PIN10, SPI2AXI_CK },
    { GPIO45, UART2_RXD, IIC3_SDA, TEST_PIN11, SPI2AXI_CS },
    { GPIO46, UART2_RTS, PWM2, IIC4_SCL, TEST_PIN22 },
    { GPIO47, UART2_CTS, PWM3, IIC4_SDA, TEST_PIN23 },
    { GPIO48, UART4_TXD, TEST_PIN12, IIC0_SCL, SPI2AXI_DI },
    { GPIO49, UART4_RXD, TEST_PIN13, IIC0_SDA, SPI2AXI_DO },
    { GPIO50, UART3_TXD, IIC2_SCL, QSPI0_CS4, TEST_PIN24 },
    { GPIO51, UART3_RXD, IIC2_SDA, QSPI0_CS3, TEST_PIN25 },
    { GPIO52, UART3_RTS, PWM4, IIC3_SCL, TEST_PIN26 },
    { GPIO53, UART3_CTS, PWM5, IIC3_SDA, FUNC_MAX },
    { GPIO54, QSPI0_CS0, MMC1_CMD, PWM0, TEST_PIN27 },
    { GPIO55, QSPI0_CLK, MMC1_CLK, PWM1, TEST_PIN28 },
    { GPIO56, QSPI0_D0, MMC1_D0, PWM2, TEST_PIN29 },
    { GPIO57, QSPI0_D1, MMC1_D1, PWM3, TEST_PIN30 },
    { GPIO58, QSPI0_D2, MMC1_D2, PWM4, TEST_PIN31 },
    { GPIO59, QSPI0_D3, MMC1_D3, PWM5, FUNC_MAX },
    { GPIO60, PWM0, IIC0_SCL, QSPI0_CS2, HSYNC1 },
    { GPIO61, PWM1, IIC0_SDA, QSPI0_CS1, VSYNC1 },
    { GPIO62, M_CLK2, UART3_DE, TEST_PIN14, FUNC_MAX },
    { GPIO63, M_CLK3, UART3_RE, TEST_PIN15, FUNC_MAX },
};

#pragma pack()

static volatile uint32_t* get_fpioa_reg(void)
{
    static volatile uint32_t* fpioa_reg = NULL;

    if (NULL == fpioa_reg) {
        fpioa_reg = (uint32_t*)rt_ioremap_nocache((void*)0x91105000, 0x1000UL);
    }

    return fpioa_reg;
}

int drv_fpioa_get_pin_cfg(int pin, uint32_t* value)
{
    if (pin < 0 || pin >= 72) {
        rt_kprintf("[hal_fpioa]: Invalid pin %d (max=%d)\n", pin, 72 - 1);
        return -1;
    }

    volatile uint32_t* fpioa_reg = get_fpioa_reg();
    if (fpioa_reg == NULL) {
        return -1;
    }

    *value = read32(&fpioa_reg[pin]);

    return 0;
}

int drv_fpioa_set_pin_cfg(int pin, uint32_t value)
{
    if (pin < 0 || pin >= 72) {
        rt_kprintf("[hal_fpioa]: Invalid pin %d (max=%d)\n", pin, 72 - 1);
        return -1;
    }

    volatile uint32_t* fpioa_reg = get_fpioa_reg();
    if (fpioa_reg == NULL) {
        return -1;
    }

    uint32_t reg_value = read32(&fpioa_reg[pin]);
    reg_value = (reg_value & 0x200) | value;
    write32(&fpioa_reg[pin], reg_value);

    return 0;
}

const fpioa_func_cfg_t* drv_fpioa_get_func_cfg(fpioa_func_t func)
{
    static fpioa_func_cfg_t gpio_dft_cfg = { .func = GPIO0, .cfg = 0x18f, .name = "GPIO0" };

    if (GPIO63 >= func) {
        return &gpio_dft_cfg;
    }

    for (size_t i = 0; i < sizeof(g_func_describ_array) / sizeof(g_func_describ_array[0]); i++) {
        if (g_func_describ_array[i].func == func) {
            return &g_func_describ_array[i];
        }
    }

    return NULL;
}

int drv_fpioa_get_pin_func(int pin, fpioa_func_t* func)
{
    int               sel;
    fpioa_iomux_cfg_t cfg;
    const uint8_t*    avail_func = NULL;

    if (FPIOA_PIN_MAX_NUM <= pin) {
        rt_kprintf("[hal_fpioa]: invalid pin %d\n", pin);
        return -1;
    }

    if (0x00 != drv_fpioa_get_pin_cfg(pin, &cfg.u.value)) {
        rt_kprintf("[hal_fpioa]: get pin cfg failed.\n");
        return -1;
    }

    sel = cfg.u.bit.io_sel;
    if (FPIOA_PIN_MAX_FUNCS <= sel) {
        rt_kprintf("[hal_fpioa]: invalid pin sel.\n");
        return -1;
    }

    avail_func = &g_pin_func_array[pin][0];

    if (func) {
        *func = avail_func[sel];
    }

    return 0;
}

int drv_fpioa_set_pin_func(int pin, fpioa_func_t func)
{
    int found    = 0;
    int func_sel = 0;

    fpioa_iomux_cfg_t       cfg, cfg_tmp;
    const fpioa_func_cfg_t* func_cfg   = NULL;
    const uint8_t*          avail_func = NULL;

    int          alt_pins_cnt = 0;
    int          alt_pins[FPIOA_PIN_FUNC_ALT_NUM];
    fpioa_func_t alt_pin_curr_func;

    if (FPIOA_PIN_MAX_NUM <= pin) {
        return -1;
    }

    avail_func = &g_pin_func_array[pin][0];
    for (int i = 0; i < FPIOA_PIN_MAX_FUNCS; i++) {
        if (func == avail_func[i]) {
            found    = 1;
            func_sel = i;
            break;
        }
    }

    /* if set pin to GPIO, we not check too strictly */
    if((GPIO63 >= func) && (GPIO0 <= func)) {
        found    = 1;
        func_sel = 0;
    }

    if (0x00 == found) {
        rt_kprintf("[hal_fpioa]: pin %d unsupport func %d\n", pin, func);
        return -1;
    }

    if (0 < (alt_pins_cnt = drv_fpioa_func_available_pins(func, &alt_pins[0]))) {
        for (int i = 0; i < alt_pins_cnt; i++) {
            if (0 == drv_fpioa_get_pin_func(alt_pins[i], &alt_pin_curr_func)) {
                if (alt_pin_curr_func == func) {
                    if (0x00 != drv_fpioa_get_pin_cfg(alt_pins[i], &cfg_tmp.u.value)) {
                        rt_kprintf("[hal_fpioa]: get pin %d cfg failed.\n", alt_pins[i]);
                        continue;
                    }
                    cfg.u.value      = 0;
                    cfg.u.bit.io_sel = 0;
                    cfg.u.bit.msc    = cfg_tmp.u.bit.msc;

                    drv_fpioa_set_pin_cfg(alt_pins[i], cfg.u.value);
                }
            }
        }
    }

    if (NULL == (func_cfg = drv_fpioa_get_func_cfg(func))) {
        rt_kprintf("[hal_fpioa]: unsupport func %d\n", func);
        return -1;
    }
    cfg.u.value      = func_cfg->cfg;
    cfg.u.bit.io_sel = func_sel;

    if (0x00 != drv_fpioa_get_pin_cfg(pin, &cfg_tmp.u.value)) {
        rt_kprintf("[hal_fpioa]: get pin cfg %d failed\n", pin);
        return -1;
    }
    cfg.u.bit.msc = cfg_tmp.u.bit.msc;

    return drv_fpioa_set_pin_cfg(pin, cfg.u.value);
}

/* Get all pins that a specific function can be assigned to */
int drv_fpioa_func_available_pins(fpioa_func_t func, int pins[FPIOA_PIN_FUNC_ALT_NUM])
{
    int pin_cnt = 0;

    for (size_t i = 0; i < sizeof(g_pin_func_array) / sizeof(g_pin_func_array[0]); i++) {
        const uint8_t* pin_funcs = &g_pin_func_array[i][0];

        for (int j = 0; j < FPIOA_PIN_MAX_FUNCS; j++) {
            if (func == pin_funcs[j]) {
                pins[pin_cnt++] = i;

                if (FPIOA_PIN_FUNC_ALT_NUM <= pin_cnt) {
                    rt_kprintf("[hal_fpioa]: too many pins\n");
                    return pin_cnt;
                }
                break;
            }
        }
    }

    return pin_cnt;
}

/* Get all functions that can be assigned to a specific pin */
int drv_fpioa_pin_supported_funcs(int pin, fpioa_func_t funcs[FPIOA_PIN_MAX_FUNCS])
{
    if (sizeof(g_pin_func_array) / sizeof(g_pin_func_array[0]) <= pin) {
        return -1;
    }

    const uint8_t* avail_func = &g_pin_func_array[pin][0];

    for (int i = 0; i < FPIOA_PIN_MAX_FUNCS; i++) {
        funcs[i] = avail_func[i];
    }

    return 0;
}

int drv_fpioa_get_func_name(fpioa_func_t func, char* buf, size_t buf_size)
{
    const fpioa_func_cfg_t* func_cfg = NULL;

    if (!buf || (0x00 == buf_size)) {
        return -1;
    }

    if (GPIO63 >= func) {
        snprintf(buf, buf_size, "GPIO%d", func);
        return 0;
    }

    if (TEST_PIN0 <= func) {
        strncpy(buf, "RESV", buf_size);
        return 0;
    }

    func_cfg = drv_fpioa_get_func_cfg(func);
    if (func_cfg && func_cfg->name) {
        strncpy(buf, func_cfg->name, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return 0;
    }

    return -1;
}

int drv_fpioa_get_pin_func_name(int pin, char* buf, size_t buf_size)
{
    fpioa_func_t func;

    if (0x00 != drv_fpioa_get_pin_func(pin, &func)) {
        return -1;
    }

    return drv_fpioa_get_func_name(func, buf, buf_size);
}

int drv_fpioa_get_pin_alt_func_names(int pin, char* buf, size_t buf_size)
{
    int          pos = 0;
    char         func_name[32];
    fpioa_func_t funcs[FPIOA_PIN_MAX_FUNCS];

    if (0x00 != drv_fpioa_pin_supported_funcs(pin, &funcs[0])) {
        return -1;
    }

    for (int i = 0; i < FPIOA_PIN_MAX_FUNCS; i++) {
        if (0x00 != drv_fpioa_get_func_name(funcs[i], &func_name[0], sizeof(func_name))) {
            return -1;
        }

        pos += snprintf(buf + pos, buf_size - pos, "%s%s", func_name, (i == (FPIOA_PIN_MAX_FUNCS - 1)) ? "" : "/");
    }

    return 0;
}

int drv_fpioa_find_pin_by_func(fpioa_func_t func)
{
    fpioa_func_t curr_func;

    if ((GPIO0 >= func) || (FUNC_MAX <= func)) {
        return -1;
    }

    for (int i = 0; i < FPIOA_PIN_MAX_NUM; i++) {
        if (0x00 != drv_fpioa_get_pin_func(i, &curr_func)) {
            return -1;
        }

        if (curr_func == func) {
            return i;
        }
    }

    return -1;
}

int drv_fpioa_is_func_supported_by_pin(int pin, fpioa_func_t func)
{
    fpioa_func_t funcs[FPIOA_PIN_MAX_FUNCS];

    if (drv_fpioa_pin_supported_funcs(pin, funcs) != 0) {
        return 0;
    }

    for (int i = 0; i < FPIOA_PIN_MAX_FUNCS; i++) {
        if (funcs[i] == func) {
            return 1;
        }
    }

    return 0;
}

#if defined(RT_USING_MSH) && defined(RT_FPIOA_ENABLE_BUILTIN_CMD)
#include <msh.h>

static void fpioa_show_pin_info(int pin)
{
    char         func_name[32];
    char         alt_funcs[256] = { 0 };
    fpioa_func_t func;
    uint32_t     value;
    int          pos = 0;

    if (drv_fpioa_get_pin_func(pin, &func) != 0) {
        rt_kprintf("Failed to get pin %d function\n", pin);
        return;
    }

    if (drv_fpioa_get_pin_func_name(pin, func_name, sizeof(func_name)) != 0) {
        rt_kprintf("Failed to get pin %d function name\n", pin);
        return;
    }

    if (drv_fpioa_get_pin_cfg(pin, &value) != 0) {
        rt_kprintf("Failed to get pin %d configuration\n", pin);
        return;
    }

    // Build alternate functions string with both names and values
    fpioa_func_t funcs[FPIOA_PIN_MAX_FUNCS];
    if (drv_fpioa_pin_supported_funcs(pin, funcs) == 0) {
        for (int i = 0; i < FPIOA_PIN_MAX_FUNCS; i++) {
            char name[32];
            if (funcs[i] == FUNC_MAX) {
                strcpy(name, "RESV");
            } else if (drv_fpioa_get_func_name(funcs[i], name, sizeof(name)) != 0) {
                snprintf(name, sizeof(name), "UNKNOWN(%d)", funcs[i]);
            }

            pos += snprintf(alt_funcs + pos, sizeof(alt_funcs) - pos, "%s%s(%d)", (i == 0) ? "" : "/", name, funcs[i]);
        }
    } else {
        strcpy(alt_funcs, "N/A");
    }

    rt_kprintf("Pin %d:\n", pin);
    rt_kprintf("  Current function: %s (%d)\n", func_name, func);
    rt_kprintf("  Alternate functions: %s\n", alt_funcs);
    rt_kprintf("  Configuration value: 0x%08X\n", value);
    rt_kprintf("  Details:\n");
    rt_kprintf("    st (Schmitt trigger): %d\n", drv_fpioa_get_pin_st(pin));
    rt_kprintf("    ds (Drive strength): %d\n", drv_fpioa_get_pin_ds(pin));
    rt_kprintf("    pd (Pull-down): %d\n", drv_fpioa_get_pin_pd(pin));
    rt_kprintf("    pu (Pull-up): %d\n", drv_fpioa_get_pin_pu(pin));
    rt_kprintf("    oe (Output enable): %d\n", drv_fpioa_get_pin_oe(pin));
    rt_kprintf("    ie (Input enable): %d\n", drv_fpioa_get_pin_ie(pin));
    rt_kprintf("    msc (Voltage select): %d\n", (value >> 9) & 0x1);
    rt_kprintf("    io_sel (Function select): %d\n", (value >> 11) & 0x7);
}

static void fpioa_list_all_pins(void)
{
    for (int pin = 0; pin < FPIOA_PIN_MAX_NUM; pin++) {
        fpioa_func_t func;
        char         func_name[32];

        if (drv_fpioa_get_pin_func(pin, &func) == 0 && drv_fpioa_get_pin_func_name(pin, func_name, sizeof(func_name)) == 0) {
            rt_kprintf("Pin %2d: %s (%d)\n", pin, func_name, func);
        }
    }
}

static void fpioa_set_pin_func(int pin, fpioa_func_t func)
{
    const fpioa_func_cfg_t* func_cfg = drv_fpioa_get_func_cfg(func);
    if (!func_cfg) {
        rt_kprintf("Invalid function %d\n", func);
        return;
    }

    if (drv_fpioa_set_pin_func(pin, func) == 0) {
        char func_name[32];
        drv_fpioa_get_func_name(func, func_name, sizeof(func_name));
        rt_kprintf("Set pin %d to %s (%d) successfully\n", pin, func_name, func);
    } else {
        rt_kprintf("Failed to set pin %d to function %d\n", pin, func);
    }
}

static void fpioa_set_pin_attr(int pin, const char* attr, int value)
{
    int result = -1;

    if (!strcmp(attr, "ds")) {
        result = drv_fpioa_set_pin_ds(pin, value);
    } else if (!strcmp(attr, "pu")) {
        result = drv_fpioa_set_pin_pu(pin, value);
    } else if (!strcmp(attr, "pd")) {
        result = drv_fpioa_set_pin_pd(pin, value);
    } else if (!strcmp(attr, "ie")) {
        result = drv_fpioa_set_pin_ie(pin, value);
    } else if (!strcmp(attr, "oe")) {
        result = drv_fpioa_set_pin_oe(pin, value);
    } else if (!strcmp(attr, "st")) {
        result = drv_fpioa_set_pin_st(pin, value);
    } else if (!strcmp(attr, "msc")) {
        // Special handling for msc (voltage select)
        uint32_t cfg;
        if (drv_fpioa_get_pin_cfg(pin, &cfg) == 0) {
            if (value) {
                cfg |= (1 << 9); // Set bit 9
            } else {
                cfg &= ~(1 << 9); // Clear bit 9
            }
            result = drv_fpioa_set_pin_cfg(pin, cfg);
        }
    } else {
        rt_kprintf("Unknown attribute: %s\n", attr);
        return;
    }

    if (result == 0) {
        rt_kprintf("Set pin %d %s to %d successfully\n", pin, attr, value);
    } else {
        rt_kprintf("Failed to set pin %d %s to %d\n", pin, attr, value);
    }
}

static void fpioa_cmd(int argc, char** argv)
{
    if (argc < 2) {
        rt_kprintf("FPIOA Pin Configuration Tool\n");
        rt_kprintf("Usage:\n");
        rt_kprintf("fpioa show <pin>            - Show pin information\n");
        rt_kprintf("fpioa list                  - List all pins and their functions\n");
        rt_kprintf("fpioa set <pin> <func>      - Set pin function\n");
        rt_kprintf("fpioa get <pin>             - Get pin function\n");
        rt_kprintf("fpioa attr <pin> <attr> <val> - Set pin attribute\n");
        rt_kprintf("\nAttributes:\n");
        rt_kprintf("  ds  - Drive strength (0-15)\n");
        rt_kprintf("  pu  - Pull-up (0/1)\n");
        rt_kprintf("  pd  - Pull-down (0/1)\n");
        rt_kprintf("  ie  - Input enable (0/1)\n");
        rt_kprintf("  oe  - Output enable (0/1)\n");
        rt_kprintf("  st  - Schmitt trigger (0/1)\n");
        rt_kprintf("  msc - Voltage select (0=3.3V, 1=1.8V)\n");
        return;
    }

    if (!strcmp(argv[1], "show")) {
        if (argc < 3) {
            rt_kprintf("Usage: fpioa show <pin>\n");
            return;
        }
        int pin = atoi(argv[2]);
        fpioa_show_pin_info(pin);
    } else if (!strcmp(argv[1], "list")) {
        fpioa_list_all_pins();
    } else if (!strcmp(argv[1], "set")) {
        if (argc < 4) {
            rt_kprintf("Usage: fpioa set <pin> <func>\n");
            return;
        }
        int pin  = atoi(argv[2]);
        int func = atoi(argv[3]);
        fpioa_set_pin_func(pin, func);
    } else if (!strcmp(argv[1], "get")) {
        if (argc < 3) {
            rt_kprintf("Usage: fpioa get <pin>\n");
            return;
        }
        int          pin = atoi(argv[2]);
        fpioa_func_t func;
        if (drv_fpioa_get_pin_func(pin, &func) == 0) {
            char func_name[32];
            drv_fpioa_get_func_name(func, func_name, sizeof(func_name));
            rt_kprintf("Pin %d function: %s (%d)\n", pin, func_name, func);
        } else {
            rt_kprintf("Failed to get pin %d function\n", pin);
        }
    } else if (!strcmp(argv[1], "attr")) {
        if (argc < 5) {
            rt_kprintf("Usage: fpioa attr <pin> <attribute> <value>\n");
            rt_kprintf("Available attributes: ds, pu, pd, ie, oe, st, msc\n");
            return;
        }
        int         pin   = atoi(argv[2]);
        const char* attr  = argv[3];
        int         value = atoi(argv[4]);
        fpioa_set_pin_attr(pin, attr, value);
    } else {
        rt_kprintf("Unknown command: %s\n", argv[1]);
    }
}

MSH_CMD_EXPORT_ALIAS(fpioa_cmd, fpioa, FPIOA pin configuration tool);

#endif
