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
#ifndef __DRV_I2C_CORE_H__
#define __DRV_I2C_CORE_H__

#include <rtdef.h>
#include <rtdevice.h>

#include "drv_i2c.h"

/*
 * Core per-controller state shared between master and slave paths, plus the
 * optional EEPROM-style slave helper structures. This header is private to
 * the BSP I2C driver; it is not exposed to user space.
 */

enum i2c_slave_event {
    I2C_SLAVE_READ_REQUESTED,
    I2C_SLAVE_WRITE_REQUESTED,
    I2C_SLAVE_READ_PROCESSED,
    I2C_SLAVE_WRITE_RECEIVED,
    I2C_SLAVE_STOP,
};

typedef void (*i2c_slave_cb)(void *ctx, enum i2c_slave_event event, rt_uint8_t *val);

struct i2c_slave_eeprom {
    struct rt_device device;
    rt_uint32_t      slave_address;
    i2c_slave_cb     slave_callback;
    rt_uint32_t      slave_status;
    int              poll_event;
    rt_uint8_t      *buffer;
    rt_uint32_t      buffer_size;
    rt_uint8_t       ptr;
    rt_uint8_t       flag_recv_ptr;
};

/* Per-slave status bits stored in slave_status */
#define DW_SLAVE_STATUS_ACTIVE            (1U << 0)
#define DW_SLAVE_STATUS_WRITE_IN_PROGRESS (1U << 1)
#define DW_SLAVE_STATUS_READ_IN_PROGRESS  (1U << 2)

struct dw_i2c_master {
    struct rt_i2c_bus_device bus;         /* RT-Thread I2C bus device (master mode) */
    volatile struct dw_i2c_regs *regs;    /* mapped registers */
    char name[16];                        /* device or bus name */
    rt_uint8_t index;                     /* controller index */
    rt_uint8_t irq;                       /* interrupt number */
    rt_uint8_t is_slave;                  /* non-zero when used in slave-eeprom mode */
    struct i2c_slave_eeprom slave_dev;

    rt_uint32_t tx_fifo_depth;
    rt_uint32_t rx_fifo_depth;

    struct rt_event event;                /* transfer completion */

    struct rt_i2c_msg *msgs;
    rt_uint32_t msgs_num;

    rt_uint32_t msg_wr_idx;
    rt_uint32_t buf_wr_idx;
    rt_uint32_t msg_rd_idx;
    rt_uint32_t buf_rd_idx;
    rt_int32_t lst_rd_idx;

    rt_uint32_t rx_pending;
    rt_uint32_t abrt_src;
    rt_uint32_t err_state;
    rt_uint32_t last_speed;
    rt_uint32_t status;
};

void k230_i2c_ctrl_enable(struct dw_i2c_master *master, rt_bool_t enable);
rt_err_t k230_i2c_recover_bus(struct dw_i2c_master *master);
rt_err_t k230_i2c_hw_init(struct dw_i2c_master *master);
rt_err_t k230_i2c_config_speed(struct dw_i2c_master *master, rt_uint32_t speed_hz);
extern const struct rt_i2c_bus_device_ops k230_i2c_bus_ops;

rt_err_t k230_i2c_slave_register(struct dw_i2c_master *master);

#endif /* __DRV_I2C_CORE_H__ */
