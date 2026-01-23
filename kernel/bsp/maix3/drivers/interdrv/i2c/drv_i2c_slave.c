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
#include "drv_i2c.h"
#include "drv_i2c_core.h"

#include <riscv_io.h>
#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>

#include "dfs_poll.h"
#include <dfs_file.h>
#include <dfs_posix.h>
#include <stdlib.h>

#define DBG_TAG "i2c"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

static int eeprom_open(struct dfs_fd *file)
{
    struct i2c_slave_eeprom *eeprom = file->fnode->data;

    file->fnode->size = eeprom->buffer_size;
    return 0;
}

static int eeprom_close(struct dfs_fd *file)
{
    (void)file;
    return 0;
}

static int eeprom_read(struct dfs_fd *file, void *buffer, size_t size)
{
    struct i2c_slave_eeprom *eeprom = file->fnode->data;

    if (file->pos >= file->fnode->size)
        return 0;

    rt_uint32_t remain = file->fnode->size - file->pos;
    if (size > remain)
        size = remain;

    if (size && eeprom->buffer) {
        rt_memcpy(buffer, eeprom->buffer + file->pos, size);
    }

    file->pos += size;
    return (int)size;
}

static int eeprom_write(struct dfs_fd *file, const void *buffer, size_t size)
{
    struct i2c_slave_eeprom *eeprom = file->fnode->data;

    if (file->pos >= file->fnode->size)
        return 0;

    rt_uint32_t remain = file->fnode->size - file->pos;
    if (size > remain)
        size = remain;

    if (size && eeprom->buffer) {
        rt_memcpy(eeprom->buffer + file->pos, buffer, size);
    }

    file->pos += size;
    return (int)size;
}

static int eeprom_lseek(struct dfs_fd *file, off_t offset)
{
    if (offset < 0 || (rt_uint32_t)offset > file->fnode->size)
        return -1;

    file->pos = offset;
    return (int)offset;
}

static int eeprom_poll(struct dfs_fd *file, struct rt_pollreq *req)
{
    struct i2c_slave_eeprom *eeprom = file->fnode->data;
    rt_device_t device = &eeprom->device;
    int mask;

    rt_poll_add(&device->wait_queue, req);
    mask = eeprom->poll_event;
    eeprom->poll_event = 0;

    return mask;
}

static int eeprom_ioctl(struct dfs_fd *file, int cmd, void *args)
{
    struct i2c_slave_eeprom *eeprom = file->fnode->data;

    switch (cmd) {
    case I2C_SLAVE_IOCTL_SET_BUFFER_SIZE: {
        rt_uint32_t new_size;

        if (!args)
            return -RT_EINVAL;

        new_size = *(rt_uint32_t *)args;
        if (new_size == 0 || new_size > 4096) {
            LOG_E("slave set buffer size is illegal, size = %d\n", new_size);
            return -RT_EINVAL;
        }

        eeprom->buffer = rt_realloc(eeprom->buffer, new_size);
        if (!eeprom->buffer)
            return -RT_ENOMEM;

        eeprom->buffer_size = new_size;
        file->fnode->size = new_size;
        break;
    }
    case I2C_SLAVE_IOCTL_SET_ADDR: {
        struct dw_i2c_master *master;
        volatile struct dw_i2c_regs *regs;

        if (!args)
            return -RT_EINVAL;

        /* slave_dev is embedded in dw_i2c_master, so use container_of */
        master = rt_container_of(eeprom, struct dw_i2c_master, slave_dev);
        regs = master->regs;

        k230_i2c_ctrl_enable(master, RT_FALSE);
        eeprom->slave_address = *(rt_uint8_t *)args;
        writel(eeprom->slave_address, &regs->ic_sar.reg);
        k230_i2c_ctrl_enable(master, RT_TRUE);
        break;
    }
    default:
        return -RT_EINVAL;
    }

    return 0;
}

static const struct dfs_file_ops eeprom_fops = {
    .open  = eeprom_open,
    .close = eeprom_close,
    .read  = eeprom_read,
    .write = eeprom_write,
    .poll  = eeprom_poll,
    .lseek = eeprom_lseek,
    .ioctl = eeprom_ioctl,
};

static int i2c_slave_eeprom_register(struct i2c_slave_eeprom *dev, const char *name)
{
    int ret;

    ret = rt_device_register(&dev->device, name, RT_DEVICE_FLAG_RDWR);
    if (ret) {
        LOG_E("i2c slave: rt_device_register error %d\n", ret);
        return -1;
    }

    rt_wqueue_init(&dev->device.wait_queue);
    dev->device.fops = &eeprom_fops;
    dev->device.user_data = dev;

    if (!dev->buffer) {
        dev->buffer_size = 64;
        dev->buffer = rt_malloc(dev->buffer_size);
        if (dev->buffer)
            rt_memset(dev->buffer, 0x00, dev->buffer_size);
    }

    return 0;
}

static void i2c_slave_eeprom_callback(void *ctx,
                                      enum i2c_slave_event event,
                                      rt_uint8_t *val)
{
    struct i2c_slave_eeprom *eeprom = ctx;

    switch (event) {
    case I2C_SLAVE_WRITE_RECEIVED:
        if (eeprom->flag_recv_ptr) {
            /* write data byte into buffer */
            if (eeprom->buffer && eeprom->buffer_size) {
                if (eeprom->ptr >= eeprom->buffer_size)
                    eeprom->ptr = 0;
                eeprom->buffer[eeprom->ptr++] = *val;
                eeprom->poll_event |= POLLIN;
                rt_wqueue_wakeup(&eeprom->device.wait_queue, (void *)POLLIN);
            }
        } else {
            /* first byte is treated as memory offset */
            eeprom->flag_recv_ptr = RT_TRUE;
            eeprom->ptr = *val;
        }
        break;
    case I2C_SLAVE_READ_PROCESSED:
        /* previous byte was acknowledged on the bus; advance offset */
        eeprom->ptr++;
        /* fallthrough */
    case I2C_SLAVE_READ_REQUESTED:
        if (eeprom->buffer && eeprom->buffer_size) {
            if (eeprom->ptr >= eeprom->buffer_size)
                eeprom->ptr = 0;
            *val = eeprom->buffer[eeprom->ptr];
        }
        /*
         * Do not increment ptr again here; the byte may still be
         * discarded if the master NACKs it.
         */
        break;
    case I2C_SLAVE_STOP:
    case I2C_SLAVE_WRITE_REQUESTED:
        /* reset state when a new transaction starts or stops */
        eeprom->ptr = 0;
        eeprom->flag_recv_ptr = RT_FALSE;
        break;
    default:
        break;
    }
}

static rt_uint32_t dw_i2c_read_clear_intrbits_slave(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    rt_uint32_t dummy;
    rt_uint32_t stat = readl(&regs->ic_intr_stat.reg);
    struct dw_ic_intr_t intr;

    intr.reg = stat;

    if (intr.bits.TX_ABRT) {
        dummy = readl(&regs->ic_tx_abrt_source.reg);
        LOG_E("i2c slave: tx_abrt_source=%08x\n", dummy);
        dummy = readl(&regs->ic_clr_tx_abrt.reg);
    }
    if (intr.bits.RX_UNDER)
        dummy = readl(&regs->ic_clr_rx_under.reg);
    if (intr.bits.RX_OVER)
        dummy = readl(&regs->ic_clr_rx_over.reg);
    if (intr.bits.TX_OVER)
        dummy = readl(&regs->ic_clr_tx_over.reg);
    if (intr.bits.RX_DONE)
        dummy = readl(&regs->ic_clr_rx_done.reg);
    if (intr.bits.ACTIVITY)
        dummy = readl(&regs->ic_clr_activity.reg);
    if (intr.bits.STOP_DET)
        dummy = readl(&regs->ic_clr_stop_det.reg);
    if (intr.bits.START_DET)
        dummy = readl(&regs->ic_clr_start_det.reg);
    if (intr.bits.GEN_CALL)
        dummy = readl(&regs->ic_clr_gen_call.reg);

    return stat;
}

static void dw_i2c_slave_isr(int irq, void *param)
{
    struct dw_i2c_master *master = param;
    volatile struct dw_i2c_regs *regs = master->regs;
    struct i2c_slave_eeprom *slave = &master->slave_dev;
    rt_uint32_t raw_stat, stat;
    struct dw_ic_intr_t raw_intr, intr;
    struct dw_ic_enable_t en;
    struct dw_ic_status_t status;
    struct dw_ic_data_cmd_t data;
    rt_uint32_t tmp;
    rt_uint8_t val = 0;
    rt_uint8_t slave_activity;

    en.reg = readl(&regs->ic_enable.reg);
    raw_stat = readl(&regs->ic_raw_intr_stat.reg);
    raw_intr.reg = raw_stat;
    status.reg = readl(&regs->ic_status.reg);
    slave_activity = (rt_uint8_t)status.bits.SLV_ACTIVITY;

    if (!en.bits.ENABLE)
        return;

    /* Ignore pure ACTIVITY interrupts. */
    raw_intr.bits.ACTIVITY = 0;
    if (raw_intr.reg == 0U)
        return;

    stat = dw_i2c_read_clear_intrbits_slave(master);
    intr.reg = stat;

    if (intr.bits.RX_FULL) {
        if (!(slave->slave_status & DW_SLAVE_STATUS_WRITE_IN_PROGRESS)) {
            slave->slave_status |= DW_SLAVE_STATUS_WRITE_IN_PROGRESS;
            slave->slave_status &= ~DW_SLAVE_STATUS_READ_IN_PROGRESS;
            slave->slave_callback(slave, I2C_SLAVE_WRITE_REQUESTED, &val);
        }

        do {
            data.reg = readl(&regs->ic_cmd_data.reg);
            if (data.bits.FIRST_DATA_BYTE)
                slave->slave_callback(slave, I2C_SLAVE_WRITE_REQUESTED, &val);
            val = (rt_uint8_t)data.bits.DAT;
            slave->slave_callback(slave, I2C_SLAVE_WRITE_RECEIVED, &val);
            status.reg = readl(&regs->ic_status.reg);
            tmp = status.reg;
        } while (status.bits.RFNE);
    }

    if (intr.bits.RD_REQ) {
        if (slave_activity) {
            tmp = readl(&regs->ic_clr_rd_req.reg);

            if (!(slave->slave_status & DW_SLAVE_STATUS_READ_IN_PROGRESS)) {
                slave->slave_callback(slave, I2C_SLAVE_READ_REQUESTED, &val);
                slave->slave_status |= DW_SLAVE_STATUS_READ_IN_PROGRESS;
                slave->slave_status &= ~DW_SLAVE_STATUS_WRITE_IN_PROGRESS;
            } else {
                slave->slave_callback(slave, I2C_SLAVE_READ_PROCESSED, &val);
            }

            data.reg = 0;
            data.bits.DAT = val;
            writel(data.reg, &regs->ic_cmd_data.reg);
        }
    }

    if (intr.bits.STOP_DET)
        slave->slave_callback(slave, I2C_SLAVE_STOP, &val);
}

static void dw_i2c_slave_init(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct i2c_slave_eeprom *slave = &master->slave_dev;
    struct dw_ic_con_t con;

    k230_i2c_ctrl_enable(master, RT_FALSE);
    slave->slave_status &= ~DW_SLAVE_STATUS_ACTIVE;

    if (!slave->slave_callback) {
        LOG_E("i2c%d: no slave callback\n", master->index);
        return;
    }

    slave->slave_status = 0;

    writel(0, &regs->ic_tx_tl.reg);
    writel(0, &regs->ic_rx_tl.reg);

    /*
     * Configure controller for pure slave mode, mirroring the legacy
     * driver:
     *  - MASTER_MODE = 0
     *  - SLAVE_DISABLE = 0 (enable slave)
     *  - RX_FIFO_FULL_HLD_CTRL = 1
     *  - IC_RESTART_EN = 1
     *  - STOP_DET_IFADDRESSED = 1
     *
     * All other bits, including SPEED, are left at 0.
     */
    con.reg = 0;
    con.bits.MASTER_MODE           = 0;
    con.bits.IC_SLAVE_DISABLE      = 0;
    con.bits.RX_FIFO_FULL_HLD_CTRL = 1;
    con.bits.IC_RESTART_EN         = 1;
    con.bits.STOP_DET_IFADDRESSED  = 1;
    writel(con.reg, &regs->ic_con.reg);

    /* RX_FULL, TX_ABRT, STOP_DET, RX_UNDER, RD_REQ */
    {
        struct dw_ic_intr_t mask;

        mask.reg = 0;
        mask.bits.RX_FULL  = 1;
        mask.bits.TX_ABRT  = 1;
        mask.bits.STOP_DET = 1;
        mask.bits.RX_UNDER = 1;
        mask.bits.RD_REQ   = 1;
        writel(mask.reg, &regs->ic_intr_mask.reg);
    }
    writel(slave->slave_address, &regs->ic_sar.reg);

    rt_hw_interrupt_install(master->irq, dw_i2c_slave_isr,
                            master, master->name);

    slave->slave_status |= DW_SLAVE_STATUS_ACTIVE;
    k230_i2c_ctrl_enable(master, RT_TRUE);

    rt_hw_interrupt_umask(master->irq);
}

rt_err_t k230_i2c_slave_register(struct dw_i2c_master *master)
{
    rt_err_t result;

    /* Slave EEPROM mode for this controller */
    master->slave_dev.slave_address = 0x20 + master->index;
    master->slave_dev.slave_callback = i2c_slave_eeprom_callback;

    dw_i2c_slave_init(master);
    if (!(master->slave_dev.slave_status & DW_SLAVE_STATUS_ACTIVE))
        return -RT_ERROR;

    result = i2c_slave_eeprom_register(&master->slave_dev,
                                       master->name);
    if (result != RT_EOK) {
        LOG_E("i2c%d: slave register failed %d",
              master->index, result);
        return result;
    }

    return RT_EOK;
}
