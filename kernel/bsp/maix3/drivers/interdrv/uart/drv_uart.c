/*
 * Copyright (c) 2019-2024
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include <rtdef.h>
#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>

#include "board.h"
#include "riscv_io.h"

#include <ioremap.h>
#include <lwp_user_mm.h>

#include "ipc/completion.h"

#include "sysctl_clk.h"

#include "drv_uart.h"
#include "math.h"

#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif

#define DBG_COLOR
#define DBG_TAG "uart"
#include <rtdbg.h>

/* Register offsets */
#define UART_RBR  0x00
#define UART_THR  0x00
#define UART_DLL  0x00
#define UART_DLH  0x04
#define UART_IER  0x04
#define UART_IIR  0x08
#define UART_FCR  0x08
#define UART_LCR  0x0c
#define UART_MCR  0x10
#define UART_LSR  0x14
#define UART_MSR  0x18
#define UART_SCH  0x1c
#define UART_USR  0x7c
#define UART_TFL  0x80
#define UART_RFL  0x84
#define UART_SRR  0x88
#define UART_HALT 0xa4
#define UART_DLF  0xc0

/* Bit definitions */
#define BIT(x) (1 << (x))

/* Line Status Register bits */
#define UART_LSR_RXFIFOE BIT(7)
#define UART_LSR_TEMT    BIT(6)
#define UART_LSR_THRE    BIT(5)
#define UART_LSR_BI      BIT(4)
#define UART_LSR_FE      BIT(3)
#define UART_LSR_PE      BIT(2)
#define UART_LSR_OE      BIT(1)
#define UART_LSR_DR      BIT(0)

/* Interrupt Enable Register bits */
#define UART_IER_RDI  BIT(0)
#define UART_IER_THRI BIT(1)
#define UART_IER_LSI  BIT(2)

#define UART_LCR_WLS_MSK 0x03 /* character length select mask */
#define UART_LCR_WLS_5   0x00 /* 5 bit character length */
#define UART_LCR_WLS_6   0x01 /* 6 bit character length */
#define UART_LCR_WLS_7   0x02 /* 7 bit character length */
#define UART_LCR_WLS_8   0x03 /* 8 bit character length */
#define UART_LCR_STB     0x04 /* # stop Bits, off=1, on=1.5 or 2) */
#define UART_LCR_PEN     0x08 /* Parity eneble */
#define UART_LCR_EPS     0x10 /* Even Parity Select */
#define UART_LCR_STKP    0x20 /* Stick Parity */
#define UART_LCR_SBRK    0x40 /* Set Break */
#define UART_LCR_BKSE    0x80 /* Bank select enable */
#define UART_LCR_DLAB    0x80 /* Divisor latch access bit */

/* Interrupt Identity Register bits */
#define UART_IIR_IID_MODEM_STAT     0x00
#define UART_IIR_IID_NO_INTR        0x01
#define UART_IIR_IID_THRE           0x02
#define UART_IIR_IID_RECV_DATA      0x04
#define UART_IIR_IID_RECV_LINE_STAT 0x06
#define UART_IIR_IID_BUSBSY         0x07
#define UART_IIR_IID_CHARTO         0x0C
#define UART_IIR_IID_MASK           0x0F

#define UART_SRR_UR  BIT(0) // UART reset
#define UART_SRR_RFR BIT(1) // RX FIFO reset
#define UART_SRR_XFR BIT(2) // TX FIFO reset

#define UART_DEFAULT_BAUDRATE 115200
#define UART_TIMEOUT_TICKS    (RT_TICK_PER_SECOND / 100) /* 10ms timeout */
#define MAX_BAUD_ERROR_PPM    20000 // 2% tolerance
#define DLF_SIZE              4

#define write32(addr, value) writel(value, addr)
#define read32(addr)         readl(addr)

typedef enum _uart_receive_trigger {
    UART_RECEIVE_FIFO_1,
    UART_RECEIVE_FIFO_8,
    UART_RECEIVE_FIFO_16,
    UART_RECEIVE_FIFO_30,
} uart_receive_trigger_t;

struct uart_inst {
    struct rt_serial_device device;

    void*    reg;
    uint32_t char_tmo_ticks;

    struct {
        uint32_t addr;
        uint32_t index;
        uint32_t irqno;
        uint32_t clock_en;
    } uart;
};

static void drv_uart_irq_handler(int irq, void* param)
{
    struct uart_inst* inst = (struct uart_inst*)param;

    uint8_t iir = (uint8_t)(read32(inst->reg + UART_IIR) & UART_IIR_IID_MASK);
    uint8_t lsr = (uint8_t)read32(inst->reg + UART_LSR);

    /* Data available interrupt */
    if (iir == UART_IIR_IID_RECV_DATA || iir == UART_IIR_IID_CHARTO) {
        /* Check line status for errors first */
        if (lsr & (UART_LSR_FE | UART_LSR_PE | UART_LSR_OE)) {
            goto _on_error_drop_fifo;
            // rt_hw_serial_isr(&inst->device, RT_SERIAL_EVENT_RX_IND /* | RT_SERIAL_EVENT_RX_ERR */);
        } else {
            /* Normal data received */
            rt_hw_serial_isr(&inst->device, RT_SERIAL_EVENT_RX_IND);
        }
    }
    /* Receiver line status interrupt (errors) */
    else if (iir == UART_IIR_IID_RECV_LINE_STAT) {
        /* Clear line status interrupt by reading LSR */
        if (lsr & (UART_LSR_FE | UART_LSR_PE | UART_LSR_OE)) {
            goto _on_error_drop_fifo;
            // rt_hw_serial_isr(&inst->device, RT_SERIAL_EVENT_RX_IND /* | RT_SERIAL_EVENT_RX_ERR */);
        }
    }
    /* Busy detect interrupt */
    else if (iir == UART_IIR_IID_BUSBSY) {
        /* Clear busy state by reading USR */
        (void)read32(inst->reg + UART_USR);
    }

    return;

_on_error_drop_fifo:
    /* Drain FIFO on error */
    while (lsr & UART_LSR_DR) {
        (void)read32(inst->reg + UART_RBR);
        lsr = (uint8_t)read32(inst->reg + UART_LSR);
    }

    return;
}

static uint32_t drv_uart_calc_baud_divisor(struct uart_inst* inst, uint32_t baudrate)
{
    const unsigned int mode_x_div    = 16;
    const unsigned int bits_per_char = 10; // 8N1 format
    RT_ASSERT(inst);

    uint32_t clock     = sysctl_clk_get_leaf_freq(SYSCTL_CLK_UART_0_CLK + inst->uart.index);
    float    exact_div = (float)clock / (mode_x_div * baudrate);
    uint16_t int_div   = (uint16_t)exact_div;
    float    frac      = exact_div - int_div;

    // Compute fractional part
    uint8_t dlf = (uint8_t)(frac * (1 << DLF_SIZE) + 0.5f);
    if (dlf >= (1 << DLF_SIZE)) {
        dlf = 0;
        int_div += 1;
    }

    // Calculate actual baudrate and error
    float actual_baud   = (float)clock / (mode_x_div * (int_div + (float)dlf / (1 << DLF_SIZE)));
    float error_percent = 100.0f * fabsf((actual_baud - baudrate) / baudrate);

    // Store timeout in inst
    float char_time_s = (float)bits_per_char / actual_baud;

    uint32_t char_time_ticks = rt_tick_from_millisecond((uint32_t)(char_time_s * 1000.f + 0.5f)) * 4;
    if (char_time_ticks < 3) {
        char_time_ticks = 3;
    }
    inst->char_tmo_ticks = char_time_ticks;

    if (error_percent > 2.0f) {
        rt_kprintf("[UART] Baud mismatch: requested=%u, actual=%.2f, error=%.2f%%\n", baudrate, actual_baud, error_percent);
    }

    // Pack result as (dlf << 16) | (dlh << 8) | dll
    return ((uint32_t)dlf << 16) | ((int_div >> 8) << 8) | (int_div & 0xFF);
}

static void drv_uart_set_baudrate(struct uart_inst* inst, uint32_t baudrate)
{
    RT_ASSERT(inst);
    volatile void* uart_base = inst->reg;
    RT_ASSERT(uart_base);

    // Wait for idle
    while (!(read32(uart_base + UART_LSR) & UART_LSR_TEMT)) { }

    uint32_t packed = drv_uart_calc_baud_divisor(inst, baudrate);

    uint8_t dll = packed & 0xFF;
    uint8_t dlh = (packed >> 8) & 0xFF;
    uint8_t dlf = (packed >> 16) & ((1 << DLF_SIZE) - 1);

    // Enable DLAB
    uint32_t lcr = read32(uart_base + UART_LCR);
    write32(uart_base + UART_LCR, lcr | 0x80);

    // Set divisor registers
    write32(uart_base + UART_DLL, dll);
    write32(uart_base + UART_DLH, dlh);
    write32(uart_base + UART_DLF, dlf);

    /* Clear DLAB */
    write32(uart_base + UART_LCR, lcr & 0x7F);
}

static uint8_t drv_uart_get_fcr(struct uart_inst* inst)
{
    RT_ASSERT(inst);

    return (uint8_t)(0x07 | (UART_RECEIVE_FIFO_16 << 6));
}

static rt_err_t drv_uart_configure(struct rt_serial_device* serial, struct serial_configure* cfg)
{
    RT_ASSERT(cfg);

    struct uart_inst* inst = (struct uart_inst*)serial->parent.user_data;
    RT_ASSERT(inst);

    volatile void* uart_base = inst->reg;
    RT_ASSERT(uart_base);

    if (0x00 == inst->uart.clock_en) {
        inst->uart.clock_en = 1;

        sysctl_clk_set_leaf_div(SYSCTL_CLK_UART_0_CLK + inst->uart.index, 1, 1);
    }

#if 0
    while (!(read32(uart_base + UART_LSR) & UART_LSR_TEMT)) { }

    /* why must drain fifo */
    while (read32(uart_base + UART_LSR) & UART_LSR_DR) {
        (void)read32(uart_base + UART_RBR);
    }
    if (read32(uart_base + UART_USR) & BIT(0)) {
        rt_kprintf("UART busy, skip reconfig\n");
        return -RT_EBUSY;
    }
#else
    // Perform full UART reset (clears RX/TX FIFO, resets internal logic)
    write32(uart_base + UART_SRR, UART_SRR_UR | UART_SRR_RFR | UART_SRR_XFR);

    for (volatile int i = 0; i < 1000; i++) {
        asm volatile("nop");
    }
#endif

    write32(uart_base + UART_IER, 0x00);
    write32(uart_base + UART_MCR, BIT(1) | BIT(0));
    write32(uart_base + UART_FCR, 0x00);
    write32(uart_base + UART_FCR, drv_uart_get_fcr(inst));

    /* Configure line format */
    uint8_t lcr_value = (cfg->data_bits - 5); // Data bits: 5–8 → 0–3
    if (cfg->stop_bits != STOP_BITS_1) {
        lcr_value |= (1 << 2); // 2 stop bits
    }

    if (cfg->parity != PARITY_NONE) {
        lcr_value |= (1 << 3); // Parity enable
        if (cfg->parity == PARITY_EVEN) {
            lcr_value |= (1 << 4); // Even parity
        }
    }
    write32(uart_base + UART_LCR, lcr_value & 0x7F); // Apply line config

    drv_uart_set_baudrate(inst, cfg->baud_rate);

    if ((serial->serial_rx) && (serial->parent.open_flag & RT_DEVICE_FLAG_INT_RX)) {
        write32(uart_base + UART_IER, UART_IER_RDI | UART_IER_LSI);
    }

    return RT_EOK;
}

static rt_err_t drv_uart_control(struct rt_serial_device* serial, int cmd, void* arg)
{
    struct uart_inst* inst = (struct uart_inst*)serial->parent.user_data;
    RT_ASSERT(inst);

    volatile void* uart_base = inst->reg;
    RT_ASSERT(uart_base);

    switch (cmd) {
    case RT_DEVICE_CTRL_CONFIG: {
#ifdef RT_SERIAL_USING_DMA
        if (arg == (void*)RT_DEVICE_FLAG_DMA_RX) {
        } else if (arg == (void*)RT_DEVICE_FLAG_DMA_TX) {
        } else
#endif
        {
            LOG_E("unknown arg 0x%x", (uint64_t)arg);
        }
    } break;
    case RT_DEVICE_CTRL_CLOSE: {
        // Perform full UART reset (clears RX/TX FIFO, resets internal logic)
        write32(uart_base + UART_SRR, UART_SRR_UR | UART_SRR_RFR | UART_SRR_XFR);

        for (volatile int i = 0; i < 1000; i++) {
            asm volatile("nop");
        }
        write32(uart_base + UART_IER, 0x00);
    } break;
    case RT_DEVICE_CTRL_CLR_INT: {
        if (arg == (void*)RT_DEVICE_FLAG_INT_RX) {
            write32(uart_base + UART_IER, read32(uart_base + UART_IER) & ~(UART_IER_RDI | UART_IER_LSI));
        } else if (arg == (void*)RT_DEVICE_FLAG_INT_TX) {
            LOG_E("not support int_tx");
        } else
#ifdef RT_SERIAL_USING_DMA
            if (arg == (void*)RT_DEVICE_FLAG_DMA_RX) {
        } else if (arg == (void*)RT_DEVICE_FLAG_DMA_TX) {
        } else
#endif
        {
            LOG_E("unknown arg 0x%x", (uint64_t)arg);
        }
    } break;
    case RT_DEVICE_CTRL_SET_INT: {
        if (arg == (void*)RT_DEVICE_FLAG_INT_RX) {
            write32(uart_base + UART_IER, read32(uart_base + UART_IER) | (UART_IER_RDI | UART_IER_LSI));
        } else if (arg == (void*)RT_DEVICE_FLAG_INT_TX) {
            LOG_E("not support int_tx");
        } else {
            LOG_E("unknown arg 0x%x", (uint64_t)arg);
        }
    } break;
    /* Added */
    case RT_FIOMMAP2: {
        struct dfs_mmap2_args* mmap2 = (struct dfs_mmap2_args*)arg;
        if (mmap2 && mmap2->length <= 0x400) {
            mmap2->ret = lwp_map_user_phy(lwp_self(), NULL, inst->reg, mmap2->length, 0);
        }
    } break;
    case UART_IOCTL_SET_CONFIG: {
        if (arg) {
            struct serial_configure _config;

            if (lwp_in_user_space(arg)) {
                if (sizeof(_config) != lwp_get_from_user(&_config, arg, sizeof(_config))) {
                    LOG_E("lwp get error size");
                    return -RT_EINVAL;
                }
            } else {
                rt_memcpy(&_config, arg, sizeof(_config));
            }

            if (0x00 == _config.bufsz) {
                // Use default buffer size if not specified
                _config.bufsz = serial->config.bufsz;
            } else {
                if (_config.bufsz != serial->config.bufsz && serial->parent.ref_count) {
                    /*can not change buffer size*/
                    LOG_W("Can not change uart%d configure", inst->uart.index);

                    return RT_EBUSY;
                }
            }

            /* set serial configure */
            rt_memcpy(&serial->config, &_config, sizeof(serial->config));

            if (serial->parent.ref_count) {
                /* serial device has been opened, to configure it */
                drv_uart_configure(serial, (struct serial_configure*)&serial->config);
            }
        }
    } break;
    case UART_IOCTL_GET_CONFIG: {
        if (!arg) {
            LOG_W("arg is NULL");
            return -RT_EINVAL;
        }

        if (lwp_in_user_space(arg)) {
            if (sizeof(serial->config) != lwp_put_to_user(arg, &serial->config, sizeof(serial->config))) {
                LOG_E("lwp put error size");
                return -RT_EINVAL;
            }
        } else {
            rt_memcpy(arg, &serial->config, sizeof(serial->config));
        }
    } break;
    case UART_IOCTL_SEND_BREAK: {
        // Wait for TX to become idle
        while (!(read32(uart_base + UART_LSR) & UART_LSR_TEMT)) { }

        // Send break
        uint32_t lcr = read32(uart_base + UART_LCR);
        write32(uart_base + UART_LCR, lcr | UART_LCR_SBRK);
        rt_thread_mdelay(5); // send break for ~1ms

        // Clear break
        write32(uart_base + UART_LCR, lcr & ~UART_LCR_SBRK);
    } break;
    case UART_IOCTL_GET_DTR: {
        if (!arg) {
            LOG_W("arg is NULL");
            return -RT_EINVAL;
        }

        if (lwp_in_user_space(arg)) {
            int dtr = 1;

            if (sizeof(int) != lwp_put_to_user(arg, &dtr, sizeof(int))) {
                LOG_E("lwp put error size");
                return -RT_EINVAL;
            }
        } else {
            *((int*)arg) = 1; // DTR is always set
        }
    } break;
    default:
        LOG_E("Invalid cmd 0x%x\n", cmd);
        return -RT_EINVAL;
    }

    return RT_EOK;
}

static int drv_uart_putc(struct rt_serial_device* serial, char c)
{
    struct uart_inst* inst = (struct uart_inst*)serial->parent.user_data;
    RT_ASSERT(inst);

    volatile void* uart_base = inst->reg;
    RT_ASSERT(uart_base);

    // Determine timeout in ticks
    rt_tick_t timeout_ticks = (inst->char_tmo_ticks != 0) ? inst->char_tmo_ticks : UART_TIMEOUT_TICKS;

    rt_tick_t start = rt_tick_get();

    // Wait for transmit holding register to become empty
    while (!(read32(uart_base + UART_LSR) & UART_LSR_THRE)) {
        if (rt_tick_get() - start > timeout_ticks) {
            if ((read32(uart_base + UART_LSR) & UART_LSR_THRE)) {
                break;
            }

            /* Do not log through the UART from its putc callback. If this is
             * the console UART, rt_kprintf() re-enters this function and can
             * recurse until the stack is exhausted while TX remains stuck. */
            return -RT_ETIMEOUT;
        }
    }

    // Write character
    write32(uart_base + UART_THR, c);

    return 1;
}

static int drv_uart_getc(struct rt_serial_device* serial)
{
    struct uart_inst* inst = (struct uart_inst*)serial->parent.user_data;
    RT_ASSERT(inst);

    volatile void* uart_base = inst->reg;
    RT_ASSERT(uart_base);

    if (!(read32(uart_base + UART_LSR) & UART_LSR_DR)) {
        return -1;
    }

    return (int)read32(uart_base + UART_RBR);
}

static const struct rt_uart_ops uart_ops = {
    .configure    = drv_uart_configure,
    .control      = drv_uart_control,
    .putc         = drv_uart_putc,
    .getc         = drv_uart_getc,
    .dma_transmit = NULL,
};

/* Hardware instances */
static struct uart_inst uart_devs[] = {
#if defined(RT_SERIAL_ENABLE_UART0)
    { .uart = { .addr = UART0_BASE_ADDR, .index = 0, .irqno = 0x10, .clock_en = 0 } },
#endif
#if defined(RT_SERIAL_ENABLE_UART1)
    { .uart = { .addr = UART1_BASE_ADDR, .index = 1, .irqno = 0x11, .clock_en = 0 } },
#endif
#if defined(RT_SERIAL_ENABLE_UART2)
    { .uart = { .addr = UART2_BASE_ADDR, .index = 2, .irqno = 0x12, .clock_en = 0 } },
#endif
#if defined(RT_SERIAL_ENABLE_UART3)
    { .uart = { .addr = UART3_BASE_ADDR, .index = 3, .irqno = 0x13, .clock_en = 0 } },
#endif
#if defined(RT_SERIAL_ENABLE_UART4)
    { .uart = { .addr = UART4_BASE_ADDR, .index = 4, .irqno = 0x14, .clock_en = 0 } },
#endif
};

int rt_hw_uart_init(void)
{
    char                    dev_name[RT_NAME_MAX];
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    static int uart_inited_flag = 0;
    if (uart_inited_flag) {
        return 0;
    }

    for (size_t i = 0; i < sizeof(uart_devs) / sizeof(uart_devs[0]); i++) {
        struct uart_inst* inst = &uart_devs[i];

        inst->reg = rt_ioremap((void*)(uint64_t)inst->uart.addr, 0x1000);
        if (!inst->reg) {
            LOG_E("ioremap failed for UART%d", inst->uart.index);
            continue;
        }

        inst->device.ops          = &uart_ops;
        inst->device.config       = config;
        inst->device.config.bufsz = RT_SERIAL_RECV_BUF_SIZE;

#ifdef RT_USING_CONSOLE
        if (CONFIG_RTT_CONSOLE_ID == inst->uart.index) {
            if (CONFIG_RTT_CONSOLE_BAUD != inst->device.config.baud_rate) {
                inst->device.config.baud_rate = CONFIG_RTT_CONSOLE_BAUD;
            }
        }
#endif /* RT_USING_CONSOLE */

        rt_snprintf(dev_name, sizeof(dev_name), "uart%d", inst->uart.index);
        rt_hw_interrupt_install(inst->uart.irqno, drv_uart_irq_handler, inst, dev_name);
        rt_hw_interrupt_umask(inst->uart.irqno);

        rt_hw_serial_register(&inst->device, dev_name, RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX, inst);
    }

#ifdef RT_USING_CONSOLE
    rt_snprintf(dev_name, sizeof(dev_name), "uart%d", CONFIG_RTT_CONSOLE_ID);

    rt_console_set_device(dev_name);
#endif /* RT_USING_CONSOLE */

    uart_inited_flag = 1;

    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_uart_init);
