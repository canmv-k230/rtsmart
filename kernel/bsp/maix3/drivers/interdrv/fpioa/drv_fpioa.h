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

#pragma once

#include <stdint.h>

#define BANK_VOL_3V3_MSC 0
#define BANK_VOL_1V8_MSC 1

#define FPIOA_PIN_MAX_FUNCS    (5)
#define FPIOA_PIN_MAX_NUM      (64)
#define FPIOA_PIN_FUNC_ALT_NUM (4)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _fpioa_func {
    GPIO0,
    GPIO1,
    GPIO2,
    GPIO3,
    GPIO4,
    GPIO5,
    GPIO6,
    GPIO7,
    GPIO8,
    GPIO9,
    GPIO10,
    GPIO11,
    GPIO12,
    GPIO13,
    GPIO14,
    GPIO15,
    GPIO16,
    GPIO17,
    GPIO18,
    GPIO19,
    GPIO20,
    GPIO21,
    GPIO22,
    GPIO23,
    GPIO24,
    GPIO25,
    GPIO26,
    GPIO27,
    GPIO28,
    GPIO29,
    GPIO30,
    GPIO31,
    GPIO32,
    GPIO33,
    GPIO34,
    GPIO35,
    GPIO36,
    GPIO37,
    GPIO38,
    GPIO39,
    GPIO40,
    GPIO41,
    GPIO42,
    GPIO43,
    GPIO44,
    GPIO45,
    GPIO46,
    GPIO47,
    GPIO48,
    GPIO49,
    GPIO50,
    GPIO51,
    GPIO52,
    GPIO53,
    GPIO54,
    GPIO55,
    GPIO56,
    GPIO57,
    GPIO58,
    GPIO59,
    GPIO60,
    GPIO61,
    GPIO62,
    GPIO63,
    GPIO64,
    GPIO65,
    GPIO66,
    GPIO67,
    GPIO68,
    GPIO69,
    GPIO70,
    GPIO71,
    BOOT0,
    BOOT1,
    CI0,
    CI1,
    CI2,
    CI3,
    CO0,
    CO1,
    CO2,
    CO3,
    DI0,
    DI1,
    DI2,
    DI3,
    DO0,
    DO1,
    DO2,
    DO3,
    HSYNC0,
    HSYNC1,
    IIC0_SCL,
    IIC0_SDA,
    IIC1_SCL,
    IIC1_SDA,
    IIC2_SCL,
    IIC2_SDA,
    IIC3_SCL,
    IIC3_SDA,
    IIC4_SCL,
    IIC4_SDA,
    IIS_CLK,
    IIS_D_IN0_PDM_IN3,
    IIS_D_IN1_PDM_IN2,
    IIS_D_OUT0_PDM_IN1,
    IIS_D_OUT1_PDM_IN0,
    IIS_WS,
    JTAG_RST,
    JTAG_TCK,
    JTAG_TDI,
    JTAG_TDO,
    JTAG_TMS,
    M_CLK1,
    M_CLK2,
    M_CLK3,
    MMC1_CLK,
    MMC1_CMD,
    MMC1_D0,
    MMC1_D1,
    MMC1_D2,
    MMC1_D3,
    OSPI_CLK,
    OSPI_CS,
    OSPI_D0,
    OSPI_D1,
    OSPI_D2,
    OSPI_D3,
    OSPI_D4,
    OSPI_D5,
    OSPI_D6,
    OSPI_D7,
    OSPI_DQS,
    PDM_IN0,
    PDM_IN1,
    PDM_IN2,
    PDM_IN3,
    PULSE_CNTR0,
    PULSE_CNTR1,
    PULSE_CNTR2,
    PULSE_CNTR3,
    PULSE_CNTR4,
    PULSE_CNTR5,
    PWM0,
    PWM1,
    PWM2,
    PWM3,
    PWM4,
    PWM5,
    QSPI0_CLK,
    QSPI0_CS0,
    QSPI0_CS1,
    QSPI0_CS2,
    QSPI0_CS3,
    QSPI0_CS4,
    QSPI0_D0,
    QSPI0_D1,
    QSPI0_D2,
    QSPI0_D3,
    QSPI1_CLK,
    QSPI1_CS0,
    QSPI1_CS1,
    QSPI1_CS2,
    QSPI1_CS3,
    QSPI1_CS4,
    QSPI1_D0,
    QSPI1_D1,
    QSPI1_D2,
    QSPI1_D3,
    SPI2AXI_CK,
    SPI2AXI_CS,
    SPI2AXI_DI,
    SPI2AXI_DO,
    UART0_RXD,
    UART0_TXD,
    UART1_CTS,
    UART1_RTS,
    UART1_RXD,
    UART1_TXD,
    UART2_CTS,
    UART2_RTS,
    UART2_RXD,
    UART2_TXD,
    UART3_CTS,
    UART3_DE,
    UART3_RE,
    UART3_RTS,
    UART3_RXD,
    UART3_TXD,
    UART4_RXD,
    UART4_TXD,
    PDM_CLK,
    VSYNC0,
    VSYNC1,
    CTRL_IN_3D,
    CTRL_O1_3D,
    CTRL_O2_3D,
    PMU_INT0,
    PMU_INT1,
    PMU_INT2,
    PMU_INT3,
    PMU_INT4,
    PMU_INT5,
    PMU_OUT0,
    PMU_OUT1,
    TEST_PIN0,
    TEST_PIN1,
    TEST_PIN2,
    TEST_PIN3,
    TEST_PIN4,
    TEST_PIN5,
    TEST_PIN6,
    TEST_PIN7,
    TEST_PIN8,
    TEST_PIN9,
    TEST_PIN10,
    TEST_PIN11,
    TEST_PIN12,
    TEST_PIN13,
    TEST_PIN14,
    TEST_PIN15,
    TEST_PIN16,
    TEST_PIN17,
    TEST_PIN18,
    TEST_PIN19,
    TEST_PIN20,
    TEST_PIN21,
    TEST_PIN22,
    TEST_PIN23,
    TEST_PIN24,
    TEST_PIN25,
    TEST_PIN26,
    TEST_PIN27,
    TEST_PIN28,
    TEST_PIN29,
    TEST_PIN30,
    TEST_PIN31,
    FUNC_MAX,
} fpioa_func_t;

typedef struct _fpioa_iomux_cfg {
    union {
        struct {
            uint32_t st : 1; // bit 0    输入施密特触发器控制使能
            uint32_t ds : 4; // bit 1-4  驱动电流控制
            uint32_t pd : 1; // bit 5    下拉使能
            uint32_t pu : 1; // bit 6    上拉使能
            uint32_t oe : 1; // bit 7    输出使能
            uint32_t ie : 1; // bit 8    输入使能
            uint32_t msc : 1; // bit 9    电压选择 !!! be careful !!!
            uint32_t rsv_bit10 : 1; // bit 10
            uint32_t io_sel : 3; // bit 11-13 复用功能选择
            uint32_t rsv_bit14_30 : 17; // bit 14-30
            uint32_t di : 1; // bit 31   当前PAD输入到芯片内部的数据(即PAD的C端)
        } bit;
        uint32_t value;
    } u;
} fpioa_iomux_cfg_t;

int drv_fpioa_get_pin_cfg(int pin, uint32_t* value);
int drv_fpioa_set_pin_cfg(int pin, uint32_t value);

int drv_fpioa_get_pin_func(int pin, fpioa_func_t* func);
int drv_fpioa_set_pin_func(int pin, fpioa_func_t func);

/* Get all pins that a specific function can be assigned to */
int drv_fpioa_func_available_pins(fpioa_func_t func, int pins[FPIOA_PIN_FUNC_ALT_NUM]);

/* Get all functions that can be assigned to a specific pin */
int drv_fpioa_pin_supported_funcs(int pin, fpioa_func_t funcs[FPIOA_PIN_MAX_FUNCS]);

int drv_fpioa_get_func_name(fpioa_func_t func, char* buf, size_t buf_size);

int drv_fpioa_get_pin_func_name(int pin, char* buf, size_t buf_size);
int drv_fpioa_get_pin_alt_func_names(int pin, char* buf, size_t buf_size);

int drv_fpioa_find_pin_by_func(fpioa_func_t func);
int drv_fpioa_is_func_supported_by_pin(int pin, fpioa_func_t func);

/* for fpioa configure generator. */
typedef struct _fpioa_func_cfg {
    fpioa_func_t func;
    const char*  name;
    uint32_t     cfg;
} fpioa_func_cfg_t;

const fpioa_func_cfg_t* drv_fpioa_get_func_cfg(fpioa_func_t func);

#define DRV_FPIOA_SET_TEMPLATE(item)                                                                                   \
    static inline __attribute__((__always_inline__)) int drv_fpioa_set_pin_##item(int pin, int value)                  \
    {                                                                                                                  \
        fpioa_iomux_cfg_t cfg;                                                                                         \
        if (drv_fpioa_get_pin_cfg(pin, &cfg.u.value) != 0) {                                                           \
            return -1;                                                                                                 \
        }                                                                                                              \
        if (cfg.u.bit.item != value) {                                                                                 \
            cfg.u.bit.item = value;                                                                                    \
            return drv_fpioa_set_pin_cfg(pin, cfg.u.value);                                                            \
        }                                                                                                              \
        return 0;                                                                                                      \
    }

#define DRV_FPIOA_GET_TEMPLATE(item)                                                                                   \
    static inline __attribute__((__always_inline__)) int drv_fpioa_get_pin_##item(int pin)                             \
    {                                                                                                                  \
        fpioa_iomux_cfg_t cfg;                                                                                         \
        if (drv_fpioa_get_pin_cfg(pin, &cfg.u.value) != 0) {                                                           \
            return -1;                                                                                                 \
        }                                                                                                              \
        return cfg.u.bit.item;                                                                                         \
    }

// int drv_fpioa_set_pin_st(int pin, int value);
DRV_FPIOA_SET_TEMPLATE(st)
// int drv_fpioa_get_pin_st(int pin);
DRV_FPIOA_GET_TEMPLATE(st)

// int drv_fpioa_set_pin_ds(int pin, int value);
DRV_FPIOA_SET_TEMPLATE(ds)
// int drv_fpioa_get_pin_ds(int pin);
DRV_FPIOA_GET_TEMPLATE(ds)

// int drv_fpioa_set_pin_pd(int pin, int value);
DRV_FPIOA_SET_TEMPLATE(pd)
// int drv_fpioa_get_pin_pd(int pin);
DRV_FPIOA_GET_TEMPLATE(pd)

// int drv_fpioa_set_pin_pu(int pin, int value);
DRV_FPIOA_SET_TEMPLATE(pu)
// int drv_fpioa_get_pin_pu(int pin);
DRV_FPIOA_GET_TEMPLATE(pu)

// int drv_fpioa_set_pin_oe(int pin, int value);
DRV_FPIOA_SET_TEMPLATE(oe)
// int drv_fpioa_get_pin_oe(int pin);
DRV_FPIOA_GET_TEMPLATE(oe)

// int drv_fpioa_set_pin_ie(int pin, int value);
DRV_FPIOA_SET_TEMPLATE(ie)
// int drv_fpioa_get_pin_ie(int pin);
DRV_FPIOA_GET_TEMPLATE(ie)

#undef DRV_FPIOA_SET_TEMPLATE
#undef DRV_FPIOA_GET_TEMPLATE

#ifdef __cplusplus
}
#endif
