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

#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include <rthw.h>
#include <rtdef.h>
#include <rtdevice.h>
#include <rtthread.h>

#include <ipc/workqueue.h>

#include <ioremap.h>
#include <lwp_user_mm.h>

#include "board.h"
#include "lwp_pid.h"
#include "tick.h"
#include <riscv_io.h>

#include "drv_fpioa.h"
#include "drv_gpio.h"

#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif

#define DBG_COLOR
#define DBG_TAG "gpio"
#include <rtdbg.h>

#define IRQN_GPIO0_INTERRUPT 32

#define BIT(n) (1U << (n))

#define write32(addr, value) writel(value, (volatile void*)(rt_uint64_t)(addr))
#define read32(addr)         readl((const volatile void*)(rt_uint64_t)(addr))

typedef struct _kd_gpio {
    struct {
        volatile uint32_t dr; // 0x00: Write Data register
        volatile uint32_t ddr; // 0x04: Data direction register
        volatile uint32_t ctl; // 0x08: Control register
    } port[4]; /* 0x00 - 0x2C: port control registers */

    volatile uint32_t inten; /* 0x30: interrupt enable register */
    volatile uint32_t intmask; /* 0x34: interrupt mask register */
    volatile uint32_t inttype_level; /* 0x38: interrupt type level register */
    volatile uint32_t int_polarity; /* 0x3C: interrupt polarity register */
    volatile uint32_t intstatus; /* 0x40: interrupt status register */
    volatile uint32_t raw_intstatus; /* 0x44: raw interrupt status register */
    volatile uint32_t debounce; /* 0x48: debounce register */
    volatile uint32_t porta_eoi; /* 0x4C: port a end of interrupt register */

    volatile uint32_t input[4]; /* 0x50-0x5C: porta/b/c/d input */
    volatile uint32_t ls_sync; /* 0x60: level sync register */
    volatile uint32_t id_code; /* 0x64: ID code register */
    volatile uint32_t int_bothedge; /* 0x68: interrupt both-edge register */
    volatile uint32_t gpio_comp_version; /* 0x6C: GPIO component version register */
    volatile uint32_t config2; /* 0x70: configuration register 2 */
    volatile uint32_t config1; /* 0x74: configuration register 1 */
} kd_gpio_t;

struct kd_gpio_irq_debounce_arg {
    rt_uint64_t tmo_ticks_ms;

    rt_uint16_t pin;
    rt_uint16_t runing;

    struct rt_list_node list;
};

struct kd_gpio_irq_to_user_arg {
    gpio_irqcfg_t cfg; // IRQ configuration

    rt_uint16_t runing;
    int         pid; // PID of the process that registered the IRQ

    struct rt_list_node list;
};

typedef struct _kd_gpio_irq {
    void (*hdr)(void* args);
    void* args;

    struct kd_gpio_irq_to_user_arg  to_user;
    struct kd_gpio_irq_debounce_arg debounce;
} kd_gpio_irq_t;

typedef struct _kd_gpio_mode {
    uint32_t mode;
    uint32_t iomux;

#define GPIO_OD_DIRECTION_INPUT  0
#define GPIO_OD_DIRECTION_OUTPUT 1
    uint32_t dir; // GPIO Direction (0: input, 1: output)
} kd_gpio_mode_t;

typedef struct _kd_gpio_work {
    struct rt_work       work;
    struct rt_workqueue* queue;

    rt_spinlock_t       lock;
    struct rt_list_node list;
} kd_gpio_work_t;

typedef struct _kd_gpio_inst {
    struct rt_device    dev;
    volatile kd_gpio_t* reg[2]; // 0: GPIO0, 1: GPIO1

    kd_gpio_work_t debounce_work;
    kd_gpio_work_t irq_to_user_work;

    kd_gpio_irq_t  irq_table[GPIO_IRQ_MAX_NUM]; // IRQ table
    kd_gpio_mode_t mode_table[GPIO_MAX_NUM];
} kd_gpio_inst_t;

static kd_gpio_inst_t _gpio_inst;

#define GPIO_BANK_COUNT 3

#ifdef RT_USING_SMP
static struct rt_spinlock g_gpio_bank_locks[GPIO_BANK_COUNT];
#else
static rt_uint32_t g_gpio_bank_locks[GPIO_BANK_COUNT];
#endif

static atomic_lock_t g_gpio_pin_state_locks[GPIO_MAX_NUM];

static inline int kd_gpio_pin_bank(rt_base_t pin)
{
    if (pin >= 64) {
        return 2;
    }

    return (pin >= 32) ? 1 : 0;
}

static inline rt_base_t kd_gpio_bank_lock(rt_base_t pin)
{
#ifdef RT_USING_SMP
    return rt_spin_lock_irqsave(&g_gpio_bank_locks[kd_gpio_pin_bank(pin)]);
#else
    rt_enter_critical();
    return 0;
#endif
}

static inline void kd_gpio_bank_unlock(rt_base_t pin, rt_base_t level)
{
#ifdef RT_USING_SMP
    rt_spin_unlock_irqrestore(&g_gpio_bank_locks[kd_gpio_pin_bank(pin)], level);
#else
    (void)pin;
    (void)level;
    rt_exit_critical();
#endif
}

static inline void kd_gpio_pin_state_lock(rt_base_t pin)
{
    atomic_lock_take(&g_gpio_pin_state_locks[pin]);
}

static inline void kd_gpio_pin_state_unlock(rt_base_t pin)
{
    atomic_lock_release(&g_gpio_pin_state_locks[pin]);
}

rt_err_t kd_pin_mode_get(rt_base_t pin, rt_base_t* mode)
{
    if (pin < 0 || pin >= GPIO_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return -RT_EINVAL;
    }

    kd_gpio_pin_state_lock(pin);
    *mode = _gpio_inst.mode_table[pin].mode;
    kd_gpio_pin_state_unlock(pin);

    return RT_EOK;
}

static inline __attribute__((always_inline)) void kd_pin_write_reg_raw(volatile uint32_t* reg, int pin, int val)
{
    uint32_t reg_val = read32(reg);
    reg_val &= ~BIT(pin);
    if (val) {
        reg_val |= BIT(pin);
    }
    write32(reg, reg_val);
}

static inline __attribute__((always_inline)) void kd_pin_write_reg(rt_base_t gpio_pin, volatile uint32_t* reg, int pin, int val)
{
    rt_base_t level = kd_gpio_bank_lock(gpio_pin);
    kd_pin_write_reg_raw(reg, pin, val);
    kd_gpio_bank_unlock(gpio_pin, level);
}

void kd_pin_set_ddr(rt_base_t pin, int value)
{
    /* Magic pin mapping: 0-31:gpio0, 32-63:gpio1[0], 64-71:gpio1[1] */
    volatile kd_gpio_t* gpio     = _gpio_inst.reg[pin >= 32];
    uint8_t             port_idx = (pin >= 64);
    uint8_t             port_pin = pin & 0x1F;

    /* Set GPIO direction */
    volatile uint32_t* ddr = &gpio->port[port_idx].ddr;
    kd_pin_write_reg(pin, ddr, port_pin, value);
}

uint32_t kd_pin_get_ddr(rt_base_t pin)
{
    /* Magic pin mapping: 0-31:gpio0, 32-63:gpio1[0], 64-71:gpio1[1] */
    volatile kd_gpio_t* gpio     = _gpio_inst.reg[pin >= 32];
    uint8_t             port_idx = (pin >= 64);
    uint8_t             port_pin = pin & 0x1F;

    /* Set GPIO direction */
    volatile uint32_t* ddr = &gpio->port[port_idx].ddr;

    /* Read pin state */
    uint32_t input_val = read32(ddr);
    return (input_val & BIT(port_pin)) ? 1 : 0;
}

uint32_t kd_pin_get_dr(rt_base_t pin)
{
    /* Magic pin mapping: 0-31:gpio0, 32-63:gpio1[0], 64-71:gpio1[1] */
    volatile kd_gpio_t* gpio      = _gpio_inst.reg[pin >= 32];
    uint8_t             port_idx  = (pin >= 64);
    uint8_t             port_pin  = pin & 0x1F;
    volatile uint32_t*  input_reg = &gpio->input[port_idx];

    /* Read pin state */
    uint32_t input_val = read32(input_reg);
    return (input_val & BIT(port_pin)) ? GPIO_PV_HIGH : GPIO_PV_LOW;
}

void kd_pin_set_dr(rt_base_t pin, int value)
{
    /* Magic pin mapping: 0-31:gpio0, 32-63:gpio1[0], 64-71:gpio1[1] */
    volatile kd_gpio_t* gpio     = _gpio_inst.reg[pin >= 32];
    uint8_t             port_idx = (pin >= 64);
    uint8_t             port_pin = pin & 0x1F;

    /* Set GPIO Ouput Value */
    volatile uint32_t* dr = &gpio->port[port_idx].dr;
    kd_pin_write_reg(pin, dr, port_pin, value);
}

rt_err_t kd_pin_mode(rt_base_t pin, rt_base_t mode)
{
    uint32_t dir = 0;

    fpioa_iomux_cfg_t curr_cfg, new_cfg;

    /* Parameter validation */
    if (pin >= GPIO_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d (max %d)", pin, GPIO_MAX_NUM - 1);
        return -RT_EINVAL;
    }

    if (0x00 != drv_fpioa_get_pin_cfg(pin, &curr_cfg.u.value)) {
        LOG_E("Failed to get pin configuration for pin %d", pin);
        return -RT_EINVAL;
    }
    new_cfg.u.value = curr_cfg.u.value;

    switch (mode) {
    case GPIO_DM_OUTPUT: {
        dir = 1; // Output

        new_cfg.u.bit.oe = 1; // Enable Output
        new_cfg.u.bit.ie = 1; // Enable Input

        new_cfg.u.bit.pu = 1; // Enable pull-up
        new_cfg.u.bit.pd = 1; // Enable pull-down
    } break;
    case GPIO_DM_INPUT: {
        dir = 0; // Input

        new_cfg.u.bit.oe = 0; // Enable Output
        new_cfg.u.bit.ie = 1; // Enable Input

        new_cfg.u.bit.pu = 0; // Disable pull-up
        new_cfg.u.bit.pd = 0; // Disable pull-down
    } break;
    case GPIO_DM_INPUT_PULLUP: {
        dir = 0; // Input

        new_cfg.u.bit.oe = 0; // Disable Output
        new_cfg.u.bit.ie = 1; // Enable Input

        new_cfg.u.bit.pu = 1; // Enable pull-up
        new_cfg.u.bit.pd = 0; // Disable pull-down
    } break;
    case GPIO_DM_INPUT_PULLDOWN: {
        dir = 0; // Input

        new_cfg.u.bit.oe = 0; // Disable Output
        new_cfg.u.bit.ie = 1; // Enable Input

        new_cfg.u.bit.pu = 0; // Disable pull-up
        new_cfg.u.bit.pd = 1; // Enable pull-down
    } break;
    case GPIO_DM_OUTPUT_OD: {
        dir = 1; // Output

        new_cfg.u.bit.oe = 1; // Enable Output
        new_cfg.u.bit.ie = 1; // Enable input

        new_cfg.u.bit.pu = 0; // Disable pull-up
        new_cfg.u.bit.pd = 0; // Disable pull-down
    } break;
    default:
        LOG_E("GPIO drive mode is not supported.");
        return -RT_EINVAL;
    }

    /* or remove this ? */
    if (64 <= pin) {
        new_cfg.u.bit.io_sel = 1; // Set to GPIO function
    } else {
        new_cfg.u.bit.io_sel = 0; // Set to GPIO function
    }
    new_cfg.u.bit.st = 1;

    volatile kd_gpio_t* gpio     = _gpio_inst.reg[pin >= 32];
    uint8_t             port_idx = (pin >= 64);
    uint8_t             port_pin = pin & 0x1F;
    volatile uint32_t*  ddr      = &gpio->port[port_idx].ddr;
    kd_gpio_mode_t*     pin_mode = &_gpio_inst.mode_table[pin];

    kd_gpio_pin_state_lock(pin);
    if (new_cfg.u.value != curr_cfg.u.value) {
        if (0x00 != drv_fpioa_set_pin_cfg(pin, new_cfg.u.value)) {
            kd_gpio_pin_state_unlock(pin);
            LOG_E("Failed to set pin configuration for pin %d", pin);
            return -RT_EINVAL;
        }
    }

    rt_base_t level = kd_gpio_bank_lock(pin);
    kd_pin_write_reg_raw(ddr, port_pin, dir);
    kd_gpio_bank_unlock(pin, level);

    pin_mode->mode  = mode;
    pin_mode->iomux = new_cfg.u.value;
    pin_mode->dir   = dir;
    kd_gpio_pin_state_unlock(pin);

    return RT_EOK;
}

rt_err_t kd_pin_write(rt_base_t pin, rt_base_t value)
{
    /* Parameter validation */
    if (pin >= GPIO_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return -RT_EINVAL;
    }

    volatile kd_gpio_t* gpio     = _gpio_inst.reg[pin >= 32];
    uint8_t             port_idx = (pin >= 64);
    uint8_t             port_pin = pin & 0x1F;
    volatile uint32_t*  ddr      = &gpio->port[port_idx].ddr;
    volatile uint32_t*  dr       = &gpio->port[port_idx].dr;

    kd_gpio_pin_state_lock(pin);
    kd_gpio_mode_t* pin_mode = &_gpio_inst.mode_table[pin];
    uint32_t        mode     = pin_mode->mode;
    uint32_t        dir      = pin_mode->dir;

    if (GPIO_DM_OUTPUT_OD == mode) {
        if (dir == GPIO_OD_DIRECTION_INPUT) {
            uint32_t iomux = pin_mode->iomux;

            iomux |= BIT(7); // oe = 1
            if (0x00 != drv_fpioa_set_pin_cfg(pin, iomux)) {
                kd_gpio_pin_state_unlock(pin);
                LOG_E("Failed to set pin configuration for pin %d", pin);
                return -RT_EINVAL;
            }

            rt_base_t level = kd_gpio_bank_lock(pin);
            kd_pin_write_reg_raw(ddr, port_pin, 1);
            kd_gpio_bank_unlock(pin, level);
            pin_mode->dir = GPIO_OD_DIRECTION_OUTPUT;
            dir           = GPIO_OD_DIRECTION_OUTPUT;
        }
    } else if (0x00 == dir) {
        kd_gpio_pin_state_unlock(pin);
        LOG_E("Pin %d is input mode, not write it", pin);
        return -RT_EINVAL;
    }

    rt_base_t level = kd_gpio_bank_lock(pin);
    kd_pin_write_reg_raw(dr, port_pin, value);
    kd_gpio_bank_unlock(pin, level);
    kd_gpio_pin_state_unlock(pin);

    return RT_EOK;
}

rt_err_t kd_pin_read(rt_base_t pin)
{
    /* Parameter validation */
    if (pin >= GPIO_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return -RT_EINVAL;
    }

    volatile kd_gpio_t* gpio     = _gpio_inst.reg[pin >= 32];
    uint8_t             port_idx = (pin >= 64);
    uint8_t             port_pin = pin & 0x1F;
    volatile uint32_t*  ddr      = &gpio->port[port_idx].ddr;

    kd_gpio_pin_state_lock(pin);
    kd_gpio_mode_t* pin_mode = &_gpio_inst.mode_table[pin];
    uint32_t        mode     = pin_mode->mode;
    uint32_t        dir      = pin_mode->dir;

    if (GPIO_DM_OUTPUT_OD == mode) {
        if (dir == GPIO_OD_DIRECTION_OUTPUT) {
            uint32_t iomux = pin_mode->iomux;

            iomux &= ~BIT(7); // oe = 0
            if (0x00 != drv_fpioa_set_pin_cfg(pin, iomux)) {
                kd_gpio_pin_state_unlock(pin);
                LOG_E("Failed to set pin configuration for pin %d", pin);
                return -RT_EINVAL;
            }

            rt_base_t level = kd_gpio_bank_lock(pin);
            kd_pin_write_reg_raw(ddr, port_pin, 0);
            kd_gpio_bank_unlock(pin, level);
            pin_mode->dir = GPIO_OD_DIRECTION_INPUT;
        }
    }

    uint32_t value = kd_pin_get_dr(pin);
    kd_gpio_pin_state_unlock(pin);

    return value;
}

static void kd_pin_irq_handler(int vector, void* param)
{
    rt_base_t pin      = vector - IRQN_GPIO0_INTERRUPT;
    uint8_t   port_pin = pin & 0x1F;

    if (pin < 0 || pin >= GPIO_IRQ_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return;
    }

    volatile kd_gpio_t* gpio = _gpio_inst.reg[pin >= 32]; // Get the correct GPIO instance
    if (!gpio) {
        LOG_E("Invalid GPIO instance for pin %d", pin);
        return;
    }

    // Read the interrupt status
    uint32_t status = read32(&gpio->intstatus);
    if (status & BIT(port_pin)) {
        kd_gpio_irq_t* irq = &_gpio_inst.irq_table[pin];
        if (!irq->hdr) {
            LOG_E("No IRQ handler registered for pin %d", port_pin);
            return;
        }

        // Call the registered handler
        if (_gpio_inst.irq_table[pin].hdr) {
            _gpio_inst.irq_table[pin].hdr(_gpio_inst.irq_table[pin].args);
        }

        // Clear the interrupt status
        kd_pin_write_reg(pin, &gpio->porta_eoi, port_pin, 1);
    }
}

rt_err_t kd_pin_attach_irq(rt_base_t pin, rt_uint32_t mode, void (*hdr)(void* args), void* args)
{
    char    intr_name[RT_NAME_MAX];
    uint8_t port_pin = pin & 0x1F;

    if (pin < 0 || pin >= GPIO_IRQ_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return -RT_EINVAL;
    }

    if (hdr == NULL) {
        LOG_E("IRQ handler cannot be NULL for pin %d", pin);
        return -RT_EINVAL;
    }
    extern void vicap_gpio_irq_process(void* irq_info);

    if (hdr == vicap_gpio_irq_process) {
        LOG_I("reg vicap_gpio_irq_process\n");
    }

    if (mode != GPIO_PE_RISING && mode != GPIO_PE_FALLING && mode != GPIO_PE_BOTH && mode != GPIO_PE_HIGH
        && mode != GPIO_PE_LOW) {
        LOG_E("Unsupported IRQ mode %d for pin %d", mode, pin);
        return -RT_EINVAL;
    }

    rt_hw_interrupt_mask(IRQN_GPIO0_INTERRUPT + pin);

    kd_gpio_irq_t* irq = &_gpio_inst.irq_table[pin];
    if (irq->hdr) {
        LOG_E("IRQ handler already attached for pin %d", pin);
        return -RT_EBUSY;
    }

    irq->hdr  = hdr;
    irq->args = args;

    rt_memset(&irq->to_user, 0, sizeof(struct kd_gpio_irq_to_user_arg));
    rt_memset(&irq->debounce, 0, sizeof(struct kd_gpio_irq_debounce_arg));

    volatile kd_gpio_t* gpio = _gpio_inst.reg[pin >= 32]; // Get the correct GPIO instance
    if (!gpio) {
        LOG_E("Invalid GPIO instance for pin %d", pin);
        return -RT_EINVAL;
    }
    // Disable interrupt during configuration
    kd_pin_write_reg(pin, &gpio->inten, port_pin, 0);

    switch (mode) {
    case GPIO_PE_RISING: {
        kd_pin_write_reg(pin, &gpio->inttype_level, port_pin, 1); // Set to rising edge
        kd_pin_write_reg(pin, &gpio->int_polarity, port_pin, 1); // Set polarity to high
        kd_pin_write_reg(pin, &gpio->int_bothedge, port_pin, 0); // Set to both edges
    } break;
    case GPIO_PE_FALLING: {
        kd_pin_write_reg(pin, &gpio->inttype_level, port_pin, 1); // Set to falling edge
        kd_pin_write_reg(pin, &gpio->int_polarity, port_pin, 0); // Set polarity to low
        kd_pin_write_reg(pin, &gpio->int_bothedge, port_pin, 0); // Set to both edges
    } break;
    case GPIO_PE_BOTH: {
        kd_pin_write_reg(pin, &gpio->int_bothedge, port_pin, 1); // Set to both edges
    } break;
    case GPIO_PE_HIGH: {
        kd_pin_write_reg(pin, &gpio->inttype_level, port_pin, 0); // Set to level high
        kd_pin_write_reg(pin, &gpio->int_polarity, port_pin, 1); // Set polarity to high
        kd_pin_write_reg(pin, &gpio->int_bothedge, port_pin, 0); // Set to both edges
    } break;
    case GPIO_PE_LOW: {
        kd_pin_write_reg(pin, &gpio->inttype_level, port_pin, 0); // Set to level low
        kd_pin_write_reg(pin, &gpio->int_polarity, port_pin, 0); // Set polarity to low
        kd_pin_write_reg(pin, &gpio->int_bothedge, port_pin, 0); // Set to both edges
    } break;
    default:
        LOG_E("Unsupported IRQ mode %d for pin %d", mode, port_pin);
        return -RT_EINVAL;
    }

    rt_snprintf(intr_name, sizeof(intr_name), "pin%d", pin);
    rt_hw_interrupt_install(IRQN_GPIO0_INTERRUPT + pin, kd_pin_irq_handler, RT_NULL, intr_name);

    return RT_EOK;
}

rt_err_t kd_pin_detach_irq(rt_base_t pin)
{
    uint8_t port_pin = pin & 0x1F;

    if (pin < 0 || pin >= GPIO_IRQ_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return -RT_EINVAL;
    }
    kd_pin_irq_enable(pin, 0);

    kd_gpio_irq_t* irq = &_gpio_inst.irq_table[pin];

    irq->hdr  = NULL;
    irq->args = NULL;

    rt_memset(&irq->to_user, 0, sizeof(struct kd_gpio_irq_to_user_arg));
    rt_memset(&irq->debounce, 0, sizeof(struct kd_gpio_irq_debounce_arg));

    return RT_EOK;
}

rt_err_t kd_pin_irq_enable(rt_base_t pin, rt_uint32_t enabled)
{
    uint8_t port_pin = pin & 0x1F;

    if (pin < 0 || pin >= GPIO_IRQ_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return -RT_EINVAL;
    }

    kd_gpio_irq_t* irq = &_gpio_inst.irq_table[pin];
    if (!irq->hdr) {
        LOG_E("No IRQ handler registered for pin %d", pin);
        return -RT_EINVAL;
    }

    volatile kd_gpio_t* gpio = _gpio_inst.reg[pin >= 32]; // Get the correct GPIO instance
    if (!gpio) {
        LOG_E("Invalid GPIO instance for pin %d", pin);
        return -RT_EINVAL;
    }

    if (enabled) {
        rt_hw_interrupt_umask(IRQN_GPIO0_INTERRUPT + pin);

        // Enable the interrupt
        kd_pin_write_reg(pin, &gpio->inten, port_pin, 1); // Enable interrupt for the pin
        kd_pin_write_reg(pin, &gpio->intmask, port_pin, 0); // Unmask the interrupt
    } else {
        rt_hw_interrupt_mask(IRQN_GPIO0_INTERRUPT + pin);

        // Disable the interrupt
        kd_pin_write_reg(pin, &gpio->inten, port_pin, 0); // Enable interrupt for the pin
        kd_pin_write_reg(pin, &gpio->intmask, port_pin, 1); // Mask the interrupt
    }

    return RT_EOK;
}

/* irq handler for usrapps */
static void kd_pin_irq_to_user_work(struct rt_work* work, void* work_data)
{
    rt_base_t level;

    struct kd_gpio_irq_to_user_arg arg, *_arg = NULL;

    kd_gpio_work_t* gpio_work = (kd_gpio_work_t*)work_data;
    if (NULL == gpio_work) {
        LOG_E("Invalid work data\n");
        return;
    }

    level = rt_spin_lock_irqsave(&gpio_work->lock);
    while (!rt_list_isempty(&gpio_work->list)) {
        int runging = 0;

        _arg = rt_list_entry(gpio_work->list.next, struct kd_gpio_irq_to_user_arg, list);
        rt_memcpy(&arg, _arg, sizeof(struct kd_gpio_irq_to_user_arg));

        if ((0 < _arg->pid) && (_arg->runing)) {
            runging = 1;
        }

        rt_spin_unlock_irqrestore(&gpio_work->lock, level);

        if (runging) {
            siginfo_t info;
            rt_memset(&info, 0, sizeof(info));

            info.si_code = SI_SIGIO;
            info.si_ptr  = arg.cfg.sigval;

            lwp_kill_ext(arg.pid, arg.cfg.signo, &info);
        }

        level = rt_spin_lock_irqsave(&gpio_work->lock);

        _arg->runing = 0;
        rt_list_remove(&_arg->list);
    }
    rt_spin_unlock_irqrestore(&gpio_work->lock, level);
}

static void kd_pin_irq_debounce_work(struct rt_work* work, void* work_data)
{
    rt_base_t level;

    struct kd_gpio_irq_debounce_arg arg, *_arg = NULL;

    kd_gpio_work_t* gpio_work = (kd_gpio_work_t*)work_data;
    if (NULL == gpio_work) {
        LOG_E("Invalid work data\n");
        return;
    }

    level = rt_spin_lock_irqsave(&gpio_work->lock);
    while (!rt_list_isempty(&gpio_work->list)) {
        int runing = 0;

        _arg = rt_list_entry(gpio_work->list.next, struct kd_gpio_irq_debounce_arg, list);
        rt_memcpy(&arg, _arg, sizeof(struct kd_gpio_irq_debounce_arg));

        runing = _arg->runing;

        rt_spin_unlock_irqrestore(&gpio_work->lock, level);

        int         del_node  = 0;
        rt_uint64_t ticks_now = cpu_ticks_ms();

        if (runing && (ticks_now > arg.tmo_ticks_ms)) {
            rt_uint16_t pin = arg.pin;

            volatile kd_gpio_t* gpio = _gpio_inst.reg[pin >= 32]; // Get the correct GPIO instance
            kd_pin_write_reg(pin, &gpio->intmask, pin & 0x1F, 0); // Unmask the interrupt

            del_node = 1;
        } else {
            rt_thread_mdelay(1);
        }

        level = rt_spin_lock_irqsave(&gpio_work->lock);
        if (del_node) {
            _arg->runing = 0;
            rt_list_remove(&_arg->list);
        }
    }
    rt_spin_unlock_irqrestore(&gpio_work->lock, level);
}

static void kd_pin_irq_to_user_handler(void* arg)
{
    rt_base_t pin      = (rt_base_t)arg;
    uint8_t   port_pin = pin & 0x1F;

    if (pin < 0 || pin >= GPIO_IRQ_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return;
    }

    kd_gpio_irq_t* irq  = &_gpio_inst.irq_table[pin];
    rt_uint16_t    mode = irq->to_user.cfg.mode;

    if ((GPIO_PE_HIGH == mode) || (GPIO_PE_LOW == mode)) {
        volatile kd_gpio_t* gpio = _gpio_inst.reg[pin >= 32]; // Get the correct GPIO instance
        if (!gpio) {
            LOG_E("Invalid GPIO instance for pin %d", pin);
            return;
        }
        kd_pin_write_reg(pin, &gpio->intmask, port_pin, 1); // Mask the interrupt

        struct kd_gpio_irq_debounce_arg* debounce = &irq->debounce;
        if (0x00 == debounce->runing) {
            debounce->runing = 1;
            debounce->pin    = pin;

            rt_uint16_t debounce_ms = irq->to_user.cfg.debounce_ms;
            debounce->tmo_ticks_ms  = (uint64_t)debounce_ms + cpu_ticks_ms();

            rt_list_insert_after(&_gpio_inst.debounce_work.list, &debounce->list);
            rt_workqueue_submit_work(_gpio_inst.debounce_work.queue, &_gpio_inst.debounce_work.work, 1);
        }
    }

    struct kd_gpio_irq_to_user_arg* to_user = &irq->to_user;
    if (0x00 == to_user->runing) {
        to_user->runing = 1;

        rt_list_insert_after(&_gpio_inst.irq_to_user_work.list, &to_user->list);
        rt_workqueue_submit_work(_gpio_inst.irq_to_user_work.queue, &_gpio_inst.irq_to_user_work.work, 0);
    }
}

/* Wrap to rt-thread device */
static rt_err_t kd_pin_open_wrap(rt_device_t dev, rt_uint16_t oflag) { return RT_EOK; }

static rt_err_t kd_pin_close_wrap(rt_device_t dev)
{
    int pid = lwp_getpid();

    if (0x00 != pid) {
        /* close from lwp process */
        for (int i = 0; i < GPIO_IRQ_MAX_NUM; i++) {
            kd_gpio_irq_t* irq = &_gpio_inst.irq_table[i];

            if (irq->to_user.pid == pid) {
                kd_pin_detach_irq(i);
            }
        }
    }

    return RT_EOK;
}

static rt_size_t kd_pin_read_wrap(rt_device_t dev, rt_off_t pos, void* buffer, rt_size_t size)
{
    int     pin = (int)pos;
    uint8_t value;

    if (pin < 0 || pin >= GPIO_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return 0;
    }

    if (0x01 != size) {
        LOG_E("Invalid GPIO write size %d", size);
        return 0;
    }

    value = kd_pin_read(pin);

    *((uint8_t*)buffer) = value & 0x01;

    return 1;
}

static rt_size_t kd_pin_write_wrap(rt_device_t dev, rt_off_t pos, const void* buffer, rt_size_t size)
{
    int     pin = (int)pos;
    uint8_t value;

    if (pin < 0 || pin >= GPIO_MAX_NUM) {
        LOG_E("Invalid GPIO pin %d", pin);
        return 0;
    }

    if (0x01 != size) {
        LOG_E("Invalid GPIO read size %d", size);
        return 0;
    }

    value = ((uint8_t*)buffer)[0];

    if (RT_EOK != kd_pin_write(pin, value)) {
        LOG_E("Write pin %d failed\n", pin);
        return 0;
    }

    return 1;
}

#define KD_GPIO_IOCTL_SET_MODE _IOW('G', 0, gpio_cfg_t*)
#define KD_GPIO_IOCTL_GET_MODE _IOR('G', 1, gpio_cfg_t*)

#define KD_GPIO_IOCTL_SET_IRQ _IOW('G', 2, gpio_irqcfg_t*)
#define KD_GPIO_IOCTL_GET_IRQ _IOR('G', 3, gpio_irqcfg_t*)

#define KD_GPIO_IOCTL_CTRL_IRQ _IOWR('G', 4, gpio_cfg_t*)

static rt_err_t kd_pin_control_wrap(rt_device_t dev, int cmd, void* args)
{
    int pid = lwp_getpid();

    switch (cmd) {
    case KD_GPIO_IOCTL_SET_MODE: {
        gpio_cfg_t cfg;

        if (0x00 != pid) {
            lwp_get_from_user(&cfg, args, sizeof(cfg));
        } else {
            rt_memcpy(&cfg, args, sizeof(cfg));
        }
        int pin = cfg.pin;
        if (pin < 0 || pin >= GPIO_MAX_NUM) {
            LOG_E("Invalid GPIO pin %d", pin);
            return -RT_EINVAL;
        }

        if (RT_EOK != kd_pin_mode(cfg.pin, cfg.value)) {
            LOG_E("set pin mode failed\n");
            return -RT_ERROR;
        }
    } break;
    case KD_GPIO_IOCTL_GET_MODE: {
        gpio_cfg_t cfg;

        if (0x00 != pid) {
            lwp_get_from_user(&cfg, args, sizeof(cfg));
        } else {
            rt_memcpy(&cfg, args, sizeof(cfg));
        }
        int pin = cfg.pin;
        if (pin < 0 || pin >= GPIO_MAX_NUM) {
            LOG_E("Invalid GPIO pin %d", pin);
            return -RT_EINVAL;
        }

        rt_base_t mode;
        if (RT_EOK != kd_pin_mode_get(cfg.pin, &mode)) {
            LOG_E("get pin mode failed\n");
            return -RT_ERROR;
        }
        cfg.value = (gpio_drive_mode_t)mode;

        if (0x00 != pid) {
            lwp_put_to_user(args, &cfg, sizeof(cfg));
        } else {
            rt_memcpy(args, &cfg, sizeof(cfg));
        }
    } break;
    case KD_GPIO_IOCTL_SET_IRQ: {
        gpio_irqcfg_t irq_cfg;

        void (*hdr)(void* args);

        if (0x00 != pid) {
            lwp_get_from_user(&irq_cfg, args, sizeof(irq_cfg));

            hdr = kd_pin_irq_to_user_handler;
        } else {
            rt_memcpy(&irq_cfg, args, sizeof(irq_cfg));

            hdr = irq_cfg.sigval; // reuse
        }
        int pin = irq_cfg.pin;
        if (pin < 0 || pin >= GPIO_IRQ_MAX_NUM) {
            LOG_E("Invalid GPIO pin %d", pin);
            return -RT_EINVAL;
        }
        kd_gpio_irq_t* irq = &_gpio_inst.irq_table[pin];

        if (RT_EOK != kd_pin_attach_irq(pin, irq_cfg.mode, hdr, (void*)(long)pin)) {
            LOG_E("attach pin irq failed\n");
            return -RT_ERROR;
        }

        irq->to_user.pid = pid;
        rt_memcpy(&irq->to_user.cfg, &irq_cfg, sizeof(irq->to_user.cfg));
    } break;
    case KD_GPIO_IOCTL_GET_IRQ: {
        gpio_irqcfg_t irq_cfg;

        if (0x00 != pid) {
            lwp_get_from_user(&irq_cfg, args, sizeof(irq_cfg));
        } else {
            rt_memcpy(&irq_cfg, args, sizeof(irq_cfg));
        }

        int pin = irq_cfg.pin;
        if (pin < 0 || pin >= GPIO_IRQ_MAX_NUM) {
            LOG_E("Invalid GPIO pin %d", pin);
            return -RT_EINVAL;
        }
        kd_gpio_irq_t* irq = &_gpio_inst.irq_table[pin];

        if (0x00 != pid) {
            lwp_put_to_user(args, &irq->to_user.cfg, sizeof(irq_cfg));
        } else {
            rt_memcpy(args, &irq->to_user.cfg, sizeof(irq_cfg));
        }
    } break;
    case KD_GPIO_IOCTL_CTRL_IRQ: {
        gpio_cfg_t cfg;

        if (0x00 != pid) {
            lwp_get_from_user(&cfg, args, sizeof(cfg));
        } else {
            rt_memcpy(&cfg, args, sizeof(cfg));
        }
        int pin = cfg.pin;
        if (pin < 0 || pin >= GPIO_IRQ_MAX_NUM) {
            LOG_E("Invalid GPIO pin %d", pin);
            return -RT_EINVAL;
        }

        int enable = cfg.value & 0x01;
        int detach = cfg.value & (1 << 7);

        if ((0x00 == enable) && detach) {
            if (RT_EOK != kd_pin_detach_irq(pin)) {
                LOG_E("control pin%d detach irq failed\n", pin);
                return -RT_ERROR;
            }
        } else {
            if (kd_pin_irq_enable(pin, enable)) {
                LOG_E("control pin%d irq failed\n", pin);
                return -RT_ERROR;
            }
        }
    } break;
    default: {
        LOG_E("Unsupport cmd 0x%08X\n", cmd);
        return -RT_ERROR;
    } break;
    }

    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops serial_ops = {
    // .init    = kd_pin_init_wrap,
    .open    = kd_pin_open_wrap,
    .close   = kd_pin_close_wrap,
    .read    = kd_pin_read_wrap,
    .write   = kd_pin_write_wrap,
    .control = kd_pin_control_wrap
};
#endif

int kd_pin_init(void)
{
    struct rt_device* device;

    static int _gpio_inited = 0;
    if (_gpio_inited) {
        LOG_W("GPIO already initialized");
        return RT_EOK;
    }
    _gpio_inited = 1;

    if (rt_device_find("gpio")) {
        LOG_E("GPIO device already registered");
        return -RT_ERROR;
    }

    // Map GPIO registers
    void* reg0 = rt_ioremap_nocache((void*)GPIO0_BASE_ADDR, GPIO0_IO_SIZE);
    void* reg1 = rt_ioremap_nocache((void*)GPIO1_BASE_ADDR, GPIO1_IO_SIZE);
    if (!reg0 || !reg1) {
        LOG_E("GPIO ioremap failed");
        return -RT_ERROR;
    }
    // Initialize GPIO device
    _gpio_inst.reg[0] = (kd_gpio_t*)reg0;
    _gpio_inst.reg[1] = (kd_gpio_t*)reg1;

#ifdef RT_USING_SMP
    for (int i = 0; i < GPIO_BANK_COUNT; i++) {
        rt_spin_lock_init(&g_gpio_bank_locks[i]);
    }
#endif

    for (int i = 0; i < GPIO_MAX_NUM; i++) {
        atoic_lock_init(&g_gpio_pin_state_locks[i]);
    }

    for (int i = 0; i < GPIO_IRQ_MAX_NUM; i++) {
        kd_gpio_irq_t* irq = &_gpio_inst.irq_table[i];

        irq->hdr  = NULL;
        irq->args = NULL;
        rt_memset(&irq->to_user, 0, sizeof(struct kd_gpio_irq_to_user_arg));
        rt_memset(&irq->debounce, 0, sizeof(struct kd_gpio_irq_debounce_arg));

        rt_hw_interrupt_mask(IRQN_GPIO0_INTERRUPT + i);
    }

    _gpio_inst.debounce_work.queue = rt_workqueue_create("gpio_debounce", 20480, RT_SYSTEM_WORKQUEUE_PRIORITY - 2);
    if (!_gpio_inst.debounce_work.queue) {
        LOG_E("Create debounce workqueue failed");
        goto _error1;
    }
    rt_work_init(&_gpio_inst.debounce_work.work, kd_pin_irq_debounce_work, &_gpio_inst.debounce_work);
    rt_spin_lock_init(_gpio_inst.debounce_work.lock);
    rt_list_init(&_gpio_inst.debounce_work.list);

    _gpio_inst.irq_to_user_work.queue = rt_workqueue_create("gpio_irq_to_user", 20480, RT_SYSTEM_WORKQUEUE_PRIORITY - 1);
    if (!_gpio_inst.irq_to_user_work.queue) {
        LOG_E("Create irq_to_user workqueue failed");
        goto _error2;
    }
    rt_work_init(&_gpio_inst.irq_to_user_work.work, kd_pin_irq_to_user_work, &_gpio_inst.irq_to_user_work);
    rt_spin_lock_init(_gpio_inst.irq_to_user_work.lock);
    rt_list_init(&_gpio_inst.irq_to_user_work.list);

    device = &_gpio_inst.dev;
    rt_memset(device, 0, sizeof(struct rt_device));
    device->type = RT_Device_Class_Char;

#ifdef RT_USING_DEVICE_OPS
    device->ops = &serial_ops;
#else
    device->init    = NULL; // kd_pin_init;
    device->open    = kd_pin_open;
    device->close   = kd_pin_close;
    device->read    = kd_pin_read;
    device->write   = kd_pin_write;
    device->control = kd_pin_control;
#endif
    device->user_data = &_gpio_inst;

    // Register the GPIO device
    rt_device_register(device, "gpio", RT_DEVICE_FLAG_RDWR);

    return RT_EOK;

_error2:
    if (_gpio_inst.irq_to_user_work.queue) {
        rt_workqueue_destroy(_gpio_inst.irq_to_user_work.queue);
    }
_error1:
    if (_gpio_inst.debounce_work.queue) {
        rt_workqueue_destroy(_gpio_inst.debounce_work.queue);
    }

    _gpio_inited = 0;

    return -RT_ERROR;
}
INIT_DEVICE_EXPORT(kd_pin_init);

#if defined(RT_USING_MSH) && defined(RT_GPIO_ENABLE_BUILTIN_CMD) // Enable GPIO command in MSH
#include <msh.h>

#include <finsh.h> // For MSH_CMD_EXPORT

static uint64_t last_intr_tick = 0;

static void gpio_irq_demo(void* args)
{
    uint64_t current_tick = cpu_ticks();

    uint64_t tick_diff = current_tick - last_intr_tick;
    uint64_t us_diff   = (tick_diff * 1000000) / TIMER_CLK_FREQ;

    rt_kprintf(">> GPIO IRQ! pin=%d | Time since last: %ld us (ticks: %ld)\n", args, us_diff, tick_diff);

    last_intr_tick = current_tick;
}

static int gpio_parse_int_arg(const char* text, const char* arg_name, int* value)
{
    char* end = RT_NULL;
    long  parsed;

    if ((RT_NULL == text) || ('\0' == text[0])) {
        rt_kprintf("Invalid %s: empty input\n", arg_name);
        return -RT_EINVAL;
    }

    parsed = strtol(text, &end, 10);
    if ((end == text) || ('\0' != *end)) {
        rt_kprintf("Invalid %s: %s\n", arg_name, text);
        return -RT_EINVAL;
    }

    if ((parsed < INT_MIN) || (parsed > INT_MAX)) {
        rt_kprintf("Invalid %s: out of range (%s)\n", arg_name, text);
        return -RT_EINVAL;
    }

    *value = (int)parsed;
    return RT_EOK;
}

static void gpio_print_usage(void)
{
    rt_kprintf("Usage:\n");
    rt_kprintf("  gpio mode <pin> <mode>\n");
    rt_kprintf("  gpio write <pin> <value>\n");
    rt_kprintf("  gpio read <pin>\n");
    rt_kprintf("  gpio pulse <pin> <high_ms> <low_ms> [count]\n");
    rt_kprintf("  gpio irq <pin> <mode>\n");
    rt_kprintf("  gpio irq_enable <pin>\n");
    rt_kprintf("  gpio irq_disable <pin>\n");
    rt_kprintf("\nGPIO output values:\n");
    rt_kprintf("  0: low\n");
    rt_kprintf("  1: high\n");
    rt_kprintf("\nGPIO drive modes:\n");
    rt_kprintf("  0: output\n");
    rt_kprintf("  1: input\n");
    rt_kprintf("  2: input_pullup\n");
    rt_kprintf("  3: input_pulldown\n");
    rt_kprintf("  4: output_open_drain\n");
    rt_kprintf("\nGPIO IRQ modes:\n");
    rt_kprintf("  0: rising_edge\n");
    rt_kprintf("  1: falling_edge\n");
    rt_kprintf("  2: both_edges\n");
    rt_kprintf("  3: level_high\n");
    rt_kprintf("  4: level_low\n");
}

static void gpio_run_pulse(int pin, int high_ms, int low_ms, int count)
{
    if (pin < 0 || pin >= GPIO_MAX_NUM) {
        rt_kprintf("Invalid pin %d\n", pin);
        return;
    }

    if (high_ms < 0 || low_ms < 0) {
        rt_kprintf("Pulse timing must be >= 0 ms\n");
        return;
    }

    if (count <= 0) {
        rt_kprintf("Pulse count must be > 0\n");
        return;
    }

    if ((0 == high_ms) && (0 == low_ms)) {
        rt_kprintf("At least one pulse phase must be > 0 ms\n");
        return;
    }

    if (kd_pin_mode(pin, GPIO_DM_OUTPUT) != RT_EOK) {
        rt_kprintf("Failed to set pin %d to output mode\n", pin);
        return;
    }

    if (kd_pin_write(pin, GPIO_PV_LOW) != RT_EOK) {
        rt_kprintf("Failed to drive pin %d low before pulse test\n", pin);
        return;
    }

    rt_kprintf("Pulse test on pin %d: high=%d ms low=%d ms count=%d\n", pin, high_ms, low_ms, count);
    for (int i = 0; i < count; i++) {
        if (kd_pin_write(pin, GPIO_PV_HIGH) != RT_EOK) {
            rt_kprintf("Pulse stopped: failed to drive pin %d high at cycle %d\n", pin, i + 1);
            break;
        }
        if (high_ms > 0) {
            rt_thread_mdelay(high_ms);
        }

        if (kd_pin_write(pin, GPIO_PV_LOW) != RT_EOK) {
            rt_kprintf("Pulse stopped: failed to drive pin %d low at cycle %d\n", pin, i + 1);
            break;
        }
        if (low_ms > 0) {
            rt_thread_mdelay(low_ms);
        }
    }
}

static void do_gpio(int argc, char** argv)
{
    int pin;

    if (argc < 3) {
        gpio_print_usage();
        return;
    }

    const char* cmd = argv[1];

    if (gpio_parse_int_arg(argv[2], "pin", &pin) != RT_EOK) {
        gpio_print_usage();
        return;
    }

    if (!strcmp(cmd, "mode") && argc == 4) {
        int mode;

        if (gpio_parse_int_arg(argv[3], "mode", &mode) != RT_EOK) {
            return;
        }
        if (kd_pin_mode(pin, mode) == RT_EOK)
            rt_kprintf("Set pin %d to mode %d OK\n", pin, mode);
        else
            rt_kprintf("Failed to set mode\n");
    } else if (!strcmp(cmd, "write") && argc == 4) {
        int value;

        if (gpio_parse_int_arg(argv[3], "value", &value) != RT_EOK) {
            return;
        }
        if (kd_pin_write(pin, value) == RT_EOK)
            rt_kprintf("Wrote %d to pin %d\n", value, pin);
        else
            rt_kprintf("Write failed\n");
    } else if (!strcmp(cmd, "read")) {
        int val = kd_pin_read(pin);
        if (val >= 0)
            rt_kprintf("Read from pin %d: %d\n", pin, val);
        else
            rt_kprintf("Read failed\n");
    } else if (!strcmp(cmd, "pulse") && (argc == 5 || argc == 6)) {
        int high_ms;
        int low_ms;
        int count = 1;

        if (gpio_parse_int_arg(argv[3], "high_ms", &high_ms) != RT_EOK
            || gpio_parse_int_arg(argv[4], "low_ms", &low_ms) != RT_EOK) {
            return;
        }

        if ((argc == 6) && (gpio_parse_int_arg(argv[5], "count", &count) != RT_EOK)) {
            return;
        }

        gpio_run_pulse(pin, high_ms, low_ms, count);
    } else if (!strcmp(cmd, "irq") && argc == 4) {
        int mode;

        if (gpio_parse_int_arg(argv[3], "irq_mode", &mode) != RT_EOK) {
            return;
        }
        if (kd_pin_attach_irq(pin, mode, gpio_irq_demo, (void*)(long)pin) == RT_EOK && kd_pin_irq_enable(pin, 1) == RT_EOK) {
            rt_kprintf("IRQ attached and enabled on pin %d (mode %d)\n", pin, mode);
        } else {
            rt_kprintf("Failed to attach/enable IRQ\n");
        }
    } else if (!strcmp(cmd, "irq_enable")) {
        if (kd_pin_irq_enable(pin, 1) == RT_EOK)
            rt_kprintf("IRQ enabled on pin %d\n", pin);
        else
            rt_kprintf("Failed to enable IRQ on pin %d\n", pin);
    } else if (!strcmp(cmd, "irq_disable")) {
        if (kd_pin_detach_irq(pin) == RT_EOK)
            rt_kprintf("IRQ disabled on pin %d\n", pin);
        else
            rt_kprintf("Failed to disable IRQ on pin %d\n", pin);
    } else {
        rt_kprintf("Invalid command\n");
        gpio_print_usage();
    }
}

MSH_CMD_EXPORT_ALIAS(do_gpio, gpio,
                     "GPIO command:\n"
                     "  mode <pin> <mode>\n"
                     "  write <pin> <value>\n"
                     "  read <pin>\n"
                     "  pulse <pin> <high_ms> <low_ms> [count]\n"
                     "  irq <pin> <mode>\n"
                     "  irq_enable <pin>\n"
                     "  irq_disable <pin>\n"
                     "  value: 0=low 1=high\n"
                     "  mode: 0=output 1=input 2=input_pullup 3=input_pulldown 4=output_open_drain\n"
                     "  irq_mode: 0=rising 1=falling 2=both 3=high 4=low");

#endif // end of RT_USING_MSH
