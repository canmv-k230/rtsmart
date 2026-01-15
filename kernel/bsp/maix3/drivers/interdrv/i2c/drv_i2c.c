/*
 * Copyright (c) 2011-2022, Shanghai Real-Thread Electronic Technology Co.,Ltd
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-04-01     SummerGift   add i2c driver
 */

#include "drv_i2c.h"
#include "dfs_poll.h"
#include "ioremap.h"
#include "mmu.h"
#include "sysctl_clk.h"
#include <dfs_file.h>
#include <dfs_posix.h>
#include <riscv_io.h>
#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>
#include <stdlib.h>
#include "lwp_user_mm.h"

#define DBG_COLOR
#define DBG_LVL DBG_WARNING
#define DBG_TAG "i2c"
#include <rtdbg.h>

#if defined RT_USING_I2C0_SLAVE || defined RT_USING_I2C1_SLAVE || defined RT_USING_I2C2_SLAVE                          \
    || defined RT_USING_I2C3_SLAVE || defined RT_USING_I2C4_SLAVE
#ifndef RT_USING_I2C_SLAVE_EEPROM
#error RT_USING_I2C_SLAVE_EEPROM is required
#endif
#endif

#ifdef RT_USING_I2C_SLAVE_EEPROM

enum i2c_slave_event {
    I2C_SLAVE_READ_REQUESTED,
    I2C_SLAVE_WRITE_REQUESTED,
    I2C_SLAVE_READ_PROCESSED,
    I2C_SLAVE_WRITE_RECEIVED,
    I2C_SLAVE_STOP,
};

typedef void (*i2c_slave_cb)(void* ctx, enum i2c_slave_event event, rt_uint8_t* val);

struct i2c_slave_eeprom {
    struct rt_device device;
    uint32_t         slave_address;
    i2c_slave_cb     slave_callback;
    uint32_t         slave_status;
    int              poll_event;
    uint8_t*         buffer;
    uint32_t         buffer_size;
    uint8_t          ptr;
    uint8_t          flag_recv_ptr;
};

#endif

struct chip_i2c_bus {
    union {
        struct rt_i2c_bus_device parent;
#ifdef RT_USING_I2C_SLAVE_EEPROM
        struct i2c_slave_eeprom slave_device;
#endif
    };
    struct i2c_regs* regs;
    char* device_name;
    uint8_t index;
    uint8_t irq;
    uint8_t slave;
    struct rt_event event;
    uint32_t tx_abrt;
    struct rt_i2c_msg* msgs;
    uint32_t msgs_num;
    uint32_t msg_wr_idx;
    uint32_t buf_wr_idx;
    uint32_t msg_rd_idx;
    uint32_t buf_rd_idx;
};

static struct chip_i2c_bus i2c_buses[] = {
#ifdef RT_USING_I2C0
    {
        .index = 0,
#ifdef RT_USING_I2C0_SLAVE
        .slave       = RT_TRUE,
        .device_name = "i2c0_slave",
#else
        .slave       = RT_FALSE,
        .device_name = "i2c0",
#endif
    },
#endif
#ifdef RT_USING_I2C1
    {
        .index = 1,
#ifdef RT_USING_I2C1_SLAVE
        .slave       = RT_TRUE,
        .device_name = "i2c1_slave",
#else
        .slave       = RT_FALSE,
        .device_name = "i2c1",
#endif
    },
#endif
#ifdef RT_USING_I2C2
    {
        .index = 2,
#ifdef RT_USING_I2C2_SLAVE
        .slave       = RT_TRUE,
        .device_name = "i2c2_slave",
#else
        .slave       = RT_FALSE,
        .device_name = "i2c2",
#endif
    },
#endif
#ifdef RT_USING_I2C3
    {
        .index = 3,
#ifdef RT_USING_I2C3_SLAVE
        .slave       = RT_TRUE,
        .device_name = "i2c3_slave",
#else
        .slave       = RT_FALSE,
        .device_name = "i2c3",
#endif
    },
#endif
#ifdef RT_USING_I2C4
    {
        .index = 4,
#ifdef RT_USING_I2C4_SLAVE
        .slave       = RT_TRUE,
        .device_name = "i2c4_slave",
#else
        .slave       = RT_FALSE,
        .device_name = "i2c4",
#endif
    },
#endif
};

static int dw_i2c_set_bus_speed(struct chip_i2c_bus* bus, rt_uint32_t speed)
{
    uint32_t         clock, period, lcnt, hcnt, cr;
    struct i2c_regs* i2c_base = bus->regs;

    clock  = sysctl_clk_get_leaf_freq(SYSCTL_CLK_I2C_0_CLK + bus->index);
    period = clock / speed;
    if (period < 22) {
        period = 22;
        // return -RT_EINVAL;
    }

    period = period - 20;
    lcnt   = period / 2;
    hcnt   = period - lcnt;

    if (readl(&i2c_base->ic_enable) & 1) {
        return -RT_EBUSY;
    }

    cr = readl(&i2c_base->ic_con) & (~IC_CON_SPD_MSK);
    if (speed >= I2C_MAX_SPEED) {
        cr |= IC_CON_SPD_HS;
        writel(hcnt, &i2c_base->ic_hs_scl_hcnt);
        writel(lcnt, &i2c_base->ic_hs_scl_lcnt);
    } else if (speed >= I2C_FAST_SPEED) {
        cr |= IC_CON_SPD_FS;
        writel(hcnt, &i2c_base->ic_fs_scl_hcnt);
        writel(lcnt, &i2c_base->ic_fs_scl_lcnt);
    } else {
        cr |= IC_CON_SPD_SS;
        writel(hcnt, &i2c_base->ic_ss_scl_hcnt);
        writel(lcnt, &i2c_base->ic_ss_scl_lcnt);
    }

    writel(0, &i2c_base->ic_sda_hold);
    writel(cr, &i2c_base->ic_con);

    return 0;
}

static void dw_i2c_isr(int irq, void* param)
{
    struct chip_i2c_bus* bus      = (struct chip_i2c_bus*)param;
    struct i2c_regs*     i2c_base = bus->regs;
    uint32_t             stat;

    stat = readl(&i2c_base->ic_intr_stat);
    if (stat & IC_TX_ABRT) {
        writel(0, &i2c_base->ic_intr_mask);
        bus->tx_abrt = readl(&i2c_base->ic_tx_abrt_source);
        rt_event_send(&bus->event, 1);
        return;
    }
    if (stat & IC_TX_EMPTY) {
        uint32_t spare = IC_FIFO_DEPTH - readl(&i2c_base->ic_txflr);
        uint32_t rx_spare = IC_FIFO_DEPTH - readl(&i2c_base->ic_rxflr);
        if (spare > rx_spare)
            spare = rx_spare;
        for (; bus->msg_wr_idx < bus->msgs_num;) {
            int i = bus->msg_wr_idx;
            int j = bus->buf_wr_idx;
            if (bus->msgs[i].flags & RT_I2C_RD) {
                for (; j < bus->msgs[i].len && spare; j++, spare--)
                    writel(DW_IC_DATA_CMD_READ, &i2c_base->ic_cmd_data);
            } else {
                uint8_t *buf = &bus->msgs[i].buf[j];
                for (; j < bus->msgs[i].len && spare; j++, spare--)
                    writel(*buf++, &i2c_base->ic_cmd_data);
            }
            bus->buf_wr_idx = j;
            if (bus->buf_wr_idx == bus->msgs[i].len) {
                bus->buf_wr_idx = 0;
                bus->msg_wr_idx++;
                if ((bus->msgs[i].flags & RT_I2C_STOP) || (bus->msg_wr_idx == bus->msgs_num)) {
                    writel(IC_TX_ABRT | IC_RX_FULL | IC_STOP_DET, &i2c_base->ic_intr_mask);
                    break;
                }
            }
            if (spare == 0)
                break;
        }
    }
    if (stat & (IC_RX_FULL | IC_STOP_DET)) {
        uint32_t valid = readl(&i2c_base->ic_rxflr);
        for (; bus->msg_rd_idx <= bus->msg_wr_idx && bus->msg_rd_idx < bus->msgs_num;) {
            int i = bus->msg_rd_idx;
            if ((bus->msgs[i].flags & RT_I2C_RD) == 0) {
                bus->msg_rd_idx++;
                continue;
            }
            int j = bus->buf_rd_idx;
            uint8_t *buf = &bus->msgs[i].buf[j];
            for (; j < bus->msgs[i].len && valid; j++, valid--)
                *buf++ = readl(&i2c_base->ic_cmd_data);
            bus->buf_rd_idx = j;
            if (bus->buf_rd_idx == bus->msgs[i].len) {
                bus->buf_rd_idx = 0;
                bus->msg_rd_idx++;
            }
            if (valid == 0)
                break;
        }
    }
    if (stat & IC_STOP_DET) {
        if (bus->msg_wr_idx == bus->msgs_num) {
            writel(0, &i2c_base->ic_intr_mask);
            bus->tx_abrt = 0;
            rt_event_send(&bus->event, 1);
        } else {
            writel(IC_TX_ABRT | IC_TX_EMPTY | IC_RX_FULL | IC_STOP_DET, &i2c_base->ic_intr_mask);
            readl(&i2c_base->ic_clr_stop_det);
        }
    }
}

static int dw_i2c_init(struct chip_i2c_bus* bus)
{
    struct i2c_regs* i2c_base = bus->regs;

    writel(0, &i2c_base->ic_enable);
    writel(0, &i2c_base->ic_intr_mask);
    readl(&i2c_base->ic_clr_intr);
    writel(IC_CON_SD | IC_CON_RE | IC_CON_SPD_FS | IC_CON_MM | DW_IC_CON_STOP_DET_IFADDRESSED | DW_IC_CON_TX_EMPTY_CTRL, &i2c_base->ic_con);
    writel(IC_FIFO_DEPTH / 2 - 1, &i2c_base->ic_rx_tl);
    writel(IC_FIFO_DEPTH / 2 - 1, &i2c_base->ic_tx_tl);
    dw_i2c_set_bus_speed(bus, I2C_FAST_SPEED);

    rt_event_init(&bus->event, "i2c_event", RT_IPC_FLAG_PRIO);

    rt_hw_interrupt_install(bus->irq, dw_i2c_isr, bus, bus->device_name);
    rt_hw_interrupt_umask(bus->irq);

    return 0;
}

static int dw_i2c_xfer(struct chip_i2c_bus* bus)
{
    struct i2c_regs* i2c_base = bus->regs;
    int ret;
    uint32_t value;

    value = 1000;
    while (readl(&i2c_base->ic_enable_status) & IC_ENABLE_0B) {
        if (value-- == 0) {
            LOG_E("i2c xfer busy");
            return -RT_EBUSY;
        }
    }

    bus->msg_wr_idx = 0;
    bus->buf_wr_idx = 0;
    bus->msg_rd_idx = 0;
    bus->buf_rd_idx = 0;
    value = bus->msgs[0].addr & 0x3FFUL;
    if (bus->msgs[0].flags & RT_I2C_ADDR_10BIT)
        value |= DW_IC_TAR_10BITADDR_MASTER;
    writel(value, &i2c_base->ic_tar);
    readl(&i2c_base->ic_clr_intr);
    writel(IC_TX_ABRT | IC_TX_EMPTY | IC_RX_FULL | IC_STOP_DET, &i2c_base->ic_intr_mask);
    writel(1, &i2c_base->ic_enable);

    ret = rt_event_recv(&bus->event, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 1000, 0);
    if (ret == RT_EOK) {
        if (bus->tx_abrt) {
            LOG_D("i2c xfer abort: 0x%x", bus->tx_abrt);
            ret = -RT_EIO;
        }
    } else if (ret == -RT_ETIMEOUT) {
        LOG_E("i2c xfer timeout, slv 0x%02X", value);
    } else {
        LOG_E("i2c xfer error: %d", ret);
    }

    writel(0, &i2c_base->ic_enable);

    return ret;
}

#define copy_from_user(dst, src, size)                                      \
    (size != lwp_get_from_user(dst, src, size))
#define copy_to_user(dst, src, size)                                        \
    (size != lwp_put_to_user(dst, src, size))

static rt_size_t chip_i2c_master_xfer(struct rt_i2c_bus_device* bus, struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    struct chip_i2c_bus* i2c_bus = (struct chip_i2c_bus*)bus;

    if (num == 0 || msgs == 0) {
        LOG_E("i2c xfer num is zero or msgs is NULL");
        return -RT_EINVAL;
    }

    if (lwp_self() && lwp_in_user_space((char *)msgs)) {
        rt_uint8_t **user_ptr;
        struct rt_i2c_msg **_msgs = &i2c_bus->msgs;

        *_msgs = rt_malloc(sizeof(struct rt_i2c_msg) * num);
        if (!(*_msgs)) {
            LOG_E("i2c xfer malloc fail");
            return -RT_ENOMEM;
        }

        if (0 != copy_from_user(*_msgs, msgs, sizeof(struct rt_i2c_msg) * num)) {
            LOG_E("i2c xfer copy msg fail");
            rt_free(*_msgs);
            return -RT_ENOMEM;
        }

        user_ptr = rt_malloc(sizeof(rt_uint8_t) * num);
        if (!user_ptr) {
            LOG_E("i2c xfer malloc fail");
            rt_free(*_msgs);
            return -RT_ENOMEM;
        }

        for (int i = 0; i < num; i++) {
            user_ptr[i] = (*_msgs)[i].buf;
            (*_msgs)[i].buf = rt_malloc((*_msgs)[i].len);
            if (!(*_msgs)[i].buf) {
                LOG_E("i2c xfer malloc fail");
                for (int j = 0; j < i; j++)
                    rt_free((*_msgs)[j].buf);
                rt_free(*_msgs);
                rt_free(user_ptr);
                return -RT_ENOMEM;
            }
            if (((*_msgs)[i].flags & RT_I2C_RD) == 0) {
                if (0 != copy_from_user((*_msgs)[i].buf, user_ptr[i], (*_msgs)[i].len)) {
                    LOG_E("i2c xfer get user buf fail");
                }
            }
        }
        i2c_bus->msgs_num = num;

        if (dw_i2c_xfer(i2c_bus)) {
            for (int i = 0; i < num; i++)
                rt_free((*_msgs)[i].buf);
            rt_free(*_msgs);
            rt_free(user_ptr);
            return -RT_EIO;
        }

        for (int i = 0; i < num; i++) {
            if ((*_msgs)[i].flags & RT_I2C_RD) {
                if (0 != copy_to_user(user_ptr[i], (*_msgs)[i].buf, (*_msgs)[i].len)) {
                    LOG_E("i2c xfer put user buf fail");
                }
            }
            rt_free((*_msgs)[i].buf);
        }
        rt_free(*_msgs);
        rt_free(user_ptr);
    } else {
        i2c_bus->msgs = msgs;
        i2c_bus->msgs_num = num;

        if (dw_i2c_xfer(i2c_bus))
            return -RT_EIO;
    }

    return num;
}

static rt_err_t chip_i2c_control(struct rt_i2c_bus_device* bus, rt_uint32_t cmd, rt_uint32_t arg)
{
    rt_err_t             ret     = -RT_EINVAL;
    struct chip_i2c_bus* i2c_bus = (struct chip_i2c_bus*)bus;

    switch (cmd) {
    case RT_I2C_DEV_CTRL_CLK:
        ret = dw_i2c_set_bus_speed(i2c_bus, arg);
        break;
    default:
        break;
    }

    return ret;
}

static const struct rt_i2c_bus_device_ops chip_i2c_ops = {
    .master_xfer     = chip_i2c_master_xfer,
    .i2c_bus_control = chip_i2c_control,
};

#ifdef RT_USING_I2C_SLAVE_EEPROM

static int eeprom_open(struct dfs_fd* file)
{
    struct i2c_slave_eeprom* eeprom = file->fnode->data;
    file->fnode->size               = eeprom->buffer_size;

    return 0;
}

static int eeprom_close(struct dfs_fd* file) { return 0; }

static int eeprom_read(struct dfs_fd* file, void* buffer, size_t size)
{
    struct i2c_slave_eeprom* eeprom = file->fnode->data;

    if (file->pos >= file->fnode->size)
        return 0;

    uint32_t lack = file->fnode->size - file->pos;
    if (size > lack)
        size = lack;
    memcpy(buffer, eeprom->buffer + file->pos, size);
    file->pos += size;

    return size;
}

static int eeprom_write(struct dfs_fd* file, const void* buffer, size_t size)
{
    struct i2c_slave_eeprom* eeprom = file->fnode->data;

    if (file->pos >= file->fnode->size)
        return 0;

    uint32_t lack = file->fnode->size - file->pos;
    if (size > lack)
        size = lack;
    memcpy(eeprom->buffer + file->pos, buffer, size);
    file->pos += size;

    return size;
}

static int eeprom_lseek(struct dfs_fd* file, off_t offset)
{
    struct i2c_slave_eeprom* eeprom = file->fnode->data;

    if (offset < file->fnode->size) {
        file->pos = offset;
        return offset;
    } else {
        return -1;
    }
}

static int eeprom_poll(struct dfs_fd* file, struct rt_pollreq* req)
{
    struct i2c_slave_eeprom* eeprom = file->fnode->data;

    rt_device_t device = file->fnode->data;
    rt_poll_add(&device->wait_queue, req);
    int mask           = eeprom->poll_event;
    eeprom->poll_event = 0;
    return mask;
}

static int eeprom_ioctl(struct dfs_fd* file, int cmd, void* args)
{
    struct i2c_slave_eeprom* eeprom = file->fnode->data;

    switch (cmd) {
    case I2C_SLAVE_IOCTL_SET_BUFFER_SIZE:
        if (args == NULL) {
            LOG_E("[%s] cmd:%d args is null\n", __func__, cmd);
            return -RT_EINVAL;
        }
        size_t size = *((uint32_t*)args);
        if ((size > 4096) || (0 == size)) {
            LOG_E("slave set buffer size is illegal, size = %d\n", size);
            return -RT_EINVAL;
        }
        eeprom->buffer = rt_realloc(eeprom->buffer, size);
        if (!eeprom->buffer) {
            LOG_E("slave buffer malloc fail\n");
            return -RT_ENOMEM;
        }
        eeprom->buffer_size = size;
        file->fnode->size = size;
        break;
    case I2C_SLAVE_IOCTL_SET_ADDR:
        if (args == NULL) {
            LOG_E("[%s] cmd:%d args is null\n", __func__, cmd);
            return -RT_EINVAL;
        }
        struct chip_i2c_bus* bus      = (struct chip_i2c_bus*)eeprom;
        struct i2c_regs*     i2c_base = bus->regs;
        writel(0, &i2c_base->ic_enable);
        eeprom->slave_address = *((uint8_t*)args);
        writel(eeprom->slave_address, &i2c_base->ic_sar);
        writel(1, &i2c_base->ic_enable);
        break;
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

int i2c_slave_eeprom_register(struct i2c_slave_eeprom* dev, const char* name)
{
    int ret;

    ret = rt_device_register(&dev->device, name, RT_DEVICE_FLAG_RDWR);
    if (ret) {
        LOG_E("%s: rt_device_register error %d\n", __func__, ret);
        return -1;
    }
    rt_wqueue_init(&dev->device.wait_queue);
    dev->device.fops      = &eeprom_fops;
    dev->device.user_data = dev;
    if (dev->buffer == NULL) {
        dev->buffer_size = 64;
        dev->buffer      = rt_malloc(dev->buffer_size);
        memset(dev->buffer, 0xa5, dev->buffer_size);
    }

    return 0;
}

static void i2c_slave_eeprom_callback(void* ctx, enum i2c_slave_event event, rt_uint8_t* val)
{
    struct i2c_slave_eeprom* eeprom = ctx;

    switch (event) {
    case I2C_SLAVE_WRITE_RECEIVED:
        if (eeprom->flag_recv_ptr) {
            // write data
            if (eeprom->buffer != NULL) {
                if (eeprom->ptr >= eeprom->buffer_size)
                    eeprom->ptr = 0;
                eeprom->buffer[eeprom->ptr++] = *val;
                eeprom->poll_event |= POLLIN;
                rt_wqueue_wakeup(&eeprom->device.wait_queue, (void*)POLLIN);
            }
        } else {
            // recv addr byte
            eeprom->flag_recv_ptr = RT_TRUE;
            eeprom->ptr           = *val;
        }
        break;
    case I2C_SLAVE_READ_PROCESSED:
        /* The previous byte made it to the bus, get next one */
        eeprom->ptr++;
    case I2C_SLAVE_READ_REQUESTED:
        if (eeprom->buffer != NULL) {
            if (eeprom->ptr >= eeprom->buffer_size)
                eeprom->ptr = 0;
            *val = eeprom->buffer[eeprom->ptr];
        }
        /*
         * Do not increment buffer_idx here, because we don't know if
         * this byte will be actually used. Read Linux I2C slave docs
         * for details.
         */
        break;
    case I2C_SLAVE_STOP:
    case I2C_SLAVE_WRITE_REQUESTED:
        eeprom->ptr           = 0;
        eeprom->flag_recv_ptr = RT_FALSE;
        break;

    default:
        break;
    }
}

static rt_uint32_t i2c_dw_read_clear_intrbits_slave(struct i2c_regs* i2c_base)
{
    rt_uint32_t dummy;
    rt_uint32_t stat = readl(&i2c_base->ic_intr_stat);

    if (stat & DW_IC_INTR_TX_ABRT) {
        // error, read reason
        dummy = readl(&i2c_base->ic_tx_abrt_source);
        LOG_E("tx_abrt_source: %08x\n", dummy);
        dummy = readl(&i2c_base->ic_clr_tx_abrt);
    }
    if (stat & DW_IC_INTR_RX_UNDER)
        dummy = readl(&i2c_base->ic_clr_rx_under);
    if (stat & DW_IC_INTR_RX_OVER)
        dummy = readl(&i2c_base->ic_clr_rx_over);
    if (stat & DW_IC_INTR_TX_OVER)
        dummy = readl(&i2c_base->ic_clr_tx_over);
    if (stat & DW_IC_INTR_RX_DONE)
        dummy = readl(&i2c_base->ic_clr_rx_done);
    if (stat & DW_IC_INTR_ACTIVITY)
        dummy = readl(&i2c_base->ic_clr_activity);
    if (stat & DW_IC_INTR_STOP_DET)
        dummy = readl(&i2c_base->ic_clr_stop_det);
    if (stat & DW_IC_INTR_START_DET)
        dummy = readl(&i2c_base->ic_clr_start_det);
    if (stat & DW_IC_INTR_GEN_CALL)
        dummy = readl(&i2c_base->ic_clr_gen_call);

    return stat;
}

#define STATUS_ACTIVE            BIT(0)
#define STATUS_WRITE_IN_PROGRESS BIT(1)
#define STATUS_READ_IN_PROGRESS  BIT(2)

static void dw_i2c_slave_isr(int irq, void* param)
{
    struct chip_i2c_bus*     bus      = param;
    struct i2c_regs*         i2c_base = bus->regs;
    struct i2c_slave_eeprom* slave    = &bus->slave_device;
    rt_uint32_t              raw_stat, stat, enabled, tmp;
    rt_uint8_t               val = 0, slave_activity;

    enabled        = readl(&i2c_base->ic_enable);
    raw_stat       = readl(&i2c_base->ic_raw_intr_stat);
    tmp            = readl(&i2c_base->ic_status);
    slave_activity = ((tmp & DW_IC_STATUS_SLAVE_ACTIVITY) >> 6);

    if (!enabled || !(raw_stat & ~DW_IC_INTR_ACTIVITY))
        return;

    stat = i2c_dw_read_clear_intrbits_slave(i2c_base);

    // rt_kprintf("i2c_slave_isr enabled(%u) slave_activity(%u) raw_stat(%08x) stat(%08x)\n", enabled, slave_activity,
    // raw_stat, stat);

    if (stat & DW_IC_INTR_RX_FULL) {
        if (!(slave->slave_status & STATUS_WRITE_IN_PROGRESS)) {
            slave->slave_status |= STATUS_WRITE_IN_PROGRESS;
            slave->slave_status &= ~STATUS_READ_IN_PROGRESS;
            slave->slave_callback(&bus->slave_device, I2C_SLAVE_WRITE_REQUESTED, &val);
        }

        do {
            tmp = readl(&i2c_base->ic_cmd_data);
            if (tmp & DW_IC_DATA_CMD_FIRST_DATA_BYTE)
                slave->slave_callback(&bus->slave_device, I2C_SLAVE_WRITE_REQUESTED, &val);
            val = tmp;
            slave->slave_callback(&bus->slave_device, I2C_SLAVE_WRITE_RECEIVED, &val);
            tmp = readl(&i2c_base->ic_status);
        } while (tmp & DW_IC_STATUS_RFNE);
    }

    if (stat & DW_IC_INTR_RD_REQ) {
        if (slave_activity) {
            tmp = readl(&i2c_base->ic_clr_rd_req);

            if (!(slave->slave_status & STATUS_READ_IN_PROGRESS)) {
                slave->slave_callback(&bus->slave_device, I2C_SLAVE_READ_REQUESTED, &val);
                slave->slave_status |= STATUS_READ_IN_PROGRESS;
                slave->slave_status &= ~STATUS_WRITE_IN_PROGRESS;
            } else {
                slave->slave_callback(&bus->slave_device, I2C_SLAVE_READ_PROCESSED, &val);
            }
            writel(val, &i2c_base->ic_cmd_data);
        }
    }

    if (stat & DW_IC_INTR_STOP_DET)
        slave->slave_callback(&bus->slave_device, I2C_SLAVE_STOP, &val);
}

static void dw_i2c_slave_init(struct chip_i2c_bus* bus)
{
    struct i2c_regs* i2c_base      = bus->regs;
    rt_uint8_t       slave_address = bus->slave_device.slave_address;
    int              irq           = bus->irq;

    writel(0, &i2c_base->ic_enable);
    bus->slave_device.slave_status &= ~STATUS_ACTIVE;

    if (!bus->slave_device.slave_callback) {
        LOG_E("i2c device %s has no slave callback\n", bus->device_name);
        return;
    }

    bus->slave_device.slave_status = 0;
    writel(0, &i2c_base->ic_tx_tl);
    writel(0, &i2c_base->ic_rx_tl);
    writel(DW_IC_CON_RX_FIFO_FULL_HLD_CTRL | DW_IC_CON_RESTART_EN | DW_IC_CON_STOP_DET_IFADDRESSED, &i2c_base->ic_con);
    writel(DW_IC_INTR_SLAVE_MASK, &i2c_base->ic_intr_mask);
    writel(slave_address, &i2c_base->ic_sar);
    rt_hw_interrupt_install(irq, dw_i2c_slave_isr, bus, bus->device_name);

    bus->slave_device.slave_status |= STATUS_ACTIVE;
    writel(1, &i2c_base->ic_enable);

    rt_hw_interrupt_umask(irq);
}
#endif

int rt_hw_i2c_init(void)
{
    for (int i = 0; i < sizeof(i2c_buses) / sizeof(i2c_buses[0]); i++) {
        i2c_buses[i].regs = (struct i2c_regs*)rt_ioremap((void*)0x91405000U + 0x1000 * i2c_buses[i].index, 0x1000);
        i2c_buses[i].irq  = 21 + i2c_buses[i].index;
        i2c_buses[i].parent.ops = &chip_i2c_ops;
        if (i2c_buses[i].slave) {
#ifdef RT_USING_I2C_SLAVE_EEPROM
            i2c_buses[i].slave_device.slave_address  = 0x20 + i2c_buses[i].index;
            i2c_buses[i].slave_device.slave_callback = i2c_slave_eeprom_callback, dw_i2c_slave_init(&i2c_buses[i]);
            i2c_slave_eeprom_register(&i2c_buses[i].slave_device, i2c_buses[i].device_name);
#endif
        } else {
            dw_i2c_init(&i2c_buses[i]);
            rt_i2c_bus_device_register(&i2c_buses[i].parent, i2c_buses[i].device_name);
        }
    }
#ifdef CONFIG_BOARD_K230_LABPLUS_1956
    dw_i2c_set_bus_speed(&i2c_buses[4], I2C_STANDARD_SPEED);
#endif

    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_i2c_init);

void xxd(int argc, char* argv[])
{
    if (argc != 2) {
        rt_kprintf("Usage: xxd file\n");
        return;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        rt_kprintf("Can't open %s\n", argv[1]);
        return;
    }
    char buffer[16];
    int  size = 0;
    do {
        size = read(fd, buffer, sizeof(buffer));
        for (unsigned i = 0; i < size; i++) {
            rt_kprintf("%02x ", buffer[i]);
        }
        rt_kprintf("\n");
    } while (size != 0);
    close(fd);
    return;
}
MSH_CMD_EXPORT(xxd, make a hexdump or do the reverse.)
