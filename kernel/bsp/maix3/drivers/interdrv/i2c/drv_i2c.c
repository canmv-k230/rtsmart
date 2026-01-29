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

#include "ioremap.h"
#include "mmu.h"

#include <stdlib.h>
#include <string.h>

#include <rtdevice.h>
#include <rtthread.h>
#include "drv_fpioa.h"

#define DBG_TAG "i2c"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(ar) (sizeof(ar) / sizeof((ar)[0]))
#endif

/* Base address and IRQ mapping for maix3 I2C controllers */
#define DW_I2C_BASE_ADDR      0x91405000UL
#define DW_I2C_REG_STRIDE     0x1000UL
#define DW_I2C_IRQ_BASE       21

/* Enabled controllers on this SoC */
static struct dw_i2c_master k230_i2c_ctrls[] = {
#ifdef RT_USING_I2C0
    { .index = 0, .is_slave = I2C0_MODE },
#endif
#ifdef RT_USING_I2C1
    { .index = 1, .is_slave = I2C1_MODE },
#endif
#ifdef RT_USING_I2C2
    { .index = 2, .is_slave = I2C2_MODE },
#endif
#ifdef RT_USING_I2C3
    { .index = 3, .is_slave = I2C3_MODE },
#endif
#ifdef RT_USING_I2C4
    { .index = 4, .is_slave = I2C4_MODE },
#endif
};

int rt_hw_i2c_init(void)
{
    rt_err_t result;
    rt_uint32_t i;
    struct dw_i2c_master *i2c4 = RT_NULL;

    for (i = 0; i < (rt_uint32_t)ARRAY_SIZE(k230_i2c_ctrls); i++) {
        struct dw_i2c_master *master = &k230_i2c_ctrls[i];

        rt_snprintf(master->name, sizeof(master->name),
                    "i2c%u%s", master->index,
                    master->is_slave ? "_slave" : "");

        master->regs = (volatile struct dw_i2c_regs *)rt_ioremap(
            (void *)(DW_I2C_BASE_ADDR + DW_I2C_REG_STRIDE * master->index),
            0x1000);
        if (!master->regs) {
            LOG_E("i2c%d: rt_ioremap failed", master->index);
            continue;
        }

        master->irq = DW_I2C_IRQ_BASE + master->index;
        if (master->is_slave) {
            result = k230_i2c_slave_register(master);
            if (result != RT_EOK)
                continue;
        } else {
            master->bus.ops = &k230_i2c_bus_ops;

            result = k230_i2c_hw_init(master);
            if (result != RT_EOK) {
                LOG_E("i2c%d: init failed %d",
                      master->index, result);
                continue;
            }

            result = rt_i2c_bus_device_register(&master->bus,
                                                master->name);
            if (result != RT_EOK) {
                LOG_E("i2c%d: bus register failed %d",
                      master->index, result);
                continue;
            }
        }

        if (master->index == 4)
            i2c4 = master;
    }

#ifdef CONFIG_BOARD_K230_LABPLUS_1956
    /* Board-specific override: run i2c4 at standard speed */
    if (i2c4)
        k230_i2c_config_speed(i2c4, DW_I2C_SPEED_STANDARD);
#endif

    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_i2c_init);

static struct rt_i2c_bus_device *i2c_get_bus(const char *arg)
{
    char *end = RT_NULL;
    unsigned long id = strtoul(arg, &end, 0);

    if (end != arg && *end == '\0') {
        char name[16];

        rt_snprintf(name, sizeof(name), "i2c%lu", id);
        return rt_i2c_bus_device_find(name);
    }

    return rt_i2c_bus_device_find(arg);
}

static int i2c_bus_id_from_name(const char *name)
{
    unsigned long id;
    char *end = RT_NULL;

    if (!name)
        return -1;

    if (name[0] >= '0' && name[0] <= '9') {
        id = strtoul(name, &end, 0);
        if (end != name && *end == '\0')
            return (int)id;
    }

    if (strncmp(name, "i2c", 3) == 0) {
        id = strtoul(name + 3, &end, 0);
        if (end != name + 3)
            return (int)id;
    }

    return -1;
}

static rt_err_t i2c_check_fpioa(const char *bus_name)
{
    int bus_id = i2c_bus_id_from_name(bus_name);
    int scl_pin;
    int sda_pin;
    static const fpioa_func_t i2c_scl_funcs[K230_I2C_MAX_NUM] = {
        IIC0_SCL, IIC1_SCL, IIC2_SCL, IIC3_SCL, IIC4_SCL
    };
    static const fpioa_func_t i2c_sda_funcs[K230_I2C_MAX_NUM] = {
        IIC0_SDA, IIC1_SDA, IIC2_SDA, IIC3_SDA, IIC4_SDA
    };

    if (bus_id < 0 || bus_id >= (int)K230_I2C_MAX_NUM) {
        rt_kprintf("invalid i2c bus: %s\n", bus_name ? bus_name : "NULL");
        return -RT_EINVAL;
    }

    scl_pin = drv_fpioa_find_pin_by_func(i2c_scl_funcs[bus_id]);
    sda_pin = drv_fpioa_find_pin_by_func(i2c_sda_funcs[bus_id]);
    if (scl_pin < 0 || sda_pin < 0) {
        rt_kprintf("i2c%d fpioa not configured (scl=%d sda=%d)\n",
                   bus_id, scl_pin, sda_pin);
        return -RT_EINVAL;
    }

    return RT_EOK;
}

static void i2c_transfer_help(void)
{
    rt_kprintf("Usage: i2c_transfer <bus> <desc> [data] [<desc> [data] ...]\n");
    rt_kprintf("  <bus> : i2c bus number or name (e.g. 0 or i2c0)\n");
    rt_kprintf("  <desc>: rN[@addr] or wN[@addr]\n");
    rt_kprintf("          N is length (1-65535). @addr optional if same as previous\n");
    rt_kprintf("  data  : wN must be followed by N byte values (0x00-0xFF)\n");
    rt_kprintf("Note: all messages must use the same I2C address on this platform.\n");
    rt_kprintf("Example: i2c_transfer 0 w1@0x50 0x64 r1\n");
}

static void i2cdetect_help(void)
{
    rt_kprintf("Usage: i2cdetect <bus> [start] [end]\n");
    rt_kprintf("  <bus>  : i2c bus number or name (e.g. 0 or i2c0)\n");
    rt_kprintf("  start/end: optional range, default 0x03 0x77\n");
    rt_kprintf("Note: scan uses a 1-byte read probe.\n");
}

static rt_err_t i2c_parse_desc(const char *desc,
                               rt_uint16_t *flags,
                               rt_uint16_t *len,
                               rt_uint16_t *addr,
                               rt_bool_t *addr_set)
{
    char dir;
    char *end = RT_NULL;
    char *addr_end = RT_NULL;
    unsigned long vlen;
    unsigned long vaddr;

    if (!desc || !desc[0])
        return -RT_EINVAL;

    dir = desc[0];
    if (dir != 'r' && dir != 'w')
        return -RT_EINVAL;

    vlen = strtoul(desc + 1, &end, 0);
    if (end == desc + 1 || vlen == 0 || vlen > 65535)
        return -RT_EINVAL;

    if (*end == '@') {
        vaddr = strtoul(end + 1, &addr_end, 0);
        if (addr_end == end + 1 || *addr_end != '\0' || vaddr > 0x7f)
            return -RT_EINVAL;
        *addr = (rt_uint16_t)vaddr;
        *addr_set = RT_TRUE;
    } else {
        if (*end != '\0')
            return -RT_EINVAL;
        *addr_set = RT_FALSE;
    }

    *flags = (dir == 'r') ? RT_I2C_RD : RT_I2C_WR;
    *len = (rt_uint16_t)vlen;

    return RT_EOK;
}

static void i2c_free_msgs(struct rt_i2c_msg *msgs, int msg_cnt)
{
    int i;

    if (!msgs)
        return;

    for (i = 0; i < msg_cnt; i++) {
        if (msgs[i].buf)
            rt_free(msgs[i].buf);
    }
    rt_free(msgs);
}

static void i2c_transfer(int argc, char **argv)
{
    struct rt_i2c_bus_device *bus;
    struct rt_i2c_msg *msgs = RT_NULL;
    rt_uint16_t last_addr = 0;
    rt_bool_t have_addr = RT_FALSE;
    int max_msgs;
    int msg_cnt = 0;
    int arg_idx = 2;
    int i;
    int ret;

    if (argc < 3) {
        i2c_transfer_help();
        return;
    }

    bus = i2c_get_bus(argv[1]);
    if (!bus) {
        rt_kprintf("i2c bus not found: %s\n", argv[1]);
        return;
    }
    if (i2c_check_fpioa(argv[1]) != RT_EOK)
        return;

    max_msgs = argc - 2;
    msgs = (struct rt_i2c_msg *)rt_calloc(max_msgs, sizeof(*msgs));
    if (!msgs) {
        rt_kprintf("no memory for i2c messages\n");
        return;
    }

    while (arg_idx < argc) {
        const char *desc = argv[arg_idx++];
        rt_uint16_t flags = 0;
        rt_uint16_t len = 0;
        rt_uint16_t addr = 0;
        rt_bool_t addr_set = RT_FALSE;
        struct rt_i2c_msg *msg;

        if (i2c_parse_desc(desc, &flags, &len, &addr, &addr_set) != RT_EOK) {
            rt_kprintf("invalid desc: %s\n", desc);
            i2c_transfer_help();
            goto out;
        }

        if (addr_set) {
            if (have_addr && addr != last_addr) {
                rt_kprintf("all messages must use the same address\n");
                goto out;
            }
            last_addr = addr;
            have_addr = RT_TRUE;
        } else if (!have_addr) {
            rt_kprintf("address missing in first message\n");
            goto out;
        }

        if (msg_cnt >= max_msgs) {
            rt_kprintf("too many messages\n");
            goto out;
        }

        msg = &msgs[msg_cnt];
        msg->addr = last_addr;
        msg->flags = flags;
        msg->len = len;
        msg->buf = (rt_uint8_t *)rt_malloc(len);
        if (!msg->buf) {
            rt_kprintf("no memory for message buffer\n");
            goto out;
        }

        msg_cnt++;

        if (!(flags & RT_I2C_RD)) {
            for (i = 0; i < len; i++) {
                unsigned long val;
                char *end = RT_NULL;

                if (arg_idx >= argc) {
                    rt_kprintf("missing data for write message\n");
                    goto out;
                }
                val = strtoul(argv[arg_idx++], &end, 0);
                if (end == argv[arg_idx - 1] || *end != '\0' || val > 0xFF) {
                    rt_kprintf("invalid data byte: %s\n", argv[arg_idx - 1]);
                    goto out;
                }
                msg->buf[i] = (rt_uint8_t)val;
            }
        }
    }

    ret = (int)rt_i2c_transfer(bus, msgs, msg_cnt);
    if (ret != msg_cnt) {
        rt_kprintf("i2c_transfer failed: %d\n", ret);
        goto out;
    }

    for (i = 0; i < msg_cnt; i++) {
        int j;

        if (!(msgs[i].flags & RT_I2C_RD))
            continue;

        rt_kprintf("read[%d]:", i);
        for (j = 0; j < msgs[i].len; j++) {
            rt_kprintf(" 0x%02x", msgs[i].buf[j]);
        }
        rt_kprintf("\n");
    }

out:
    i2c_free_msgs(msgs, msg_cnt);
}
MSH_CMD_EXPORT(i2c_transfer, i2c transfer tool)

static rt_bool_t i2c_probe_addr(struct rt_i2c_bus_device *bus, rt_uint16_t addr)
{
    rt_uint8_t data = 0;
    struct rt_i2c_msg msg;
    int ret;

    msg.addr = addr;
    msg.flags = RT_I2C_RD;
    msg.len = 1;
    msg.buf = &data;

    ret = (int)rt_i2c_transfer(bus, &msg, 1);
    return (ret == 1);
}

static void i2cdetect(int argc, char **argv)
{
    struct rt_i2c_bus_device *bus;
    rt_uint32_t start = 0x03;
    rt_uint32_t end = 0x77;
    int row, col;

    if (argc < 2 || argc > 4) {
        i2cdetect_help();
        return;
    }

    bus = i2c_get_bus(argv[1]);
    if (!bus) {
        rt_kprintf("i2c bus not found: %s\n", argv[1]);
        return;
    }
    if (i2c_check_fpioa(argv[1]) != RT_EOK)
        return;

    if (argc >= 3) {
        char *endptr = RT_NULL;
        start = strtoul(argv[2], &endptr, 0);
        if (endptr == argv[2] || *endptr != '\0') {
            i2cdetect_help();
            return;
        }
    }

    if (argc >= 4) {
        char *endptr = RT_NULL;
        end = strtoul(argv[3], &endptr, 0);
        if (endptr == argv[3] || *endptr != '\0') {
            i2cdetect_help();
            return;
        }
    }

    if (start > 0x7f || end > 0x7f || start > end) {
        rt_kprintf("invalid scan range\n");
        return;
    }

    rt_kprintf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (row = 0; row < 0x80; row += 16) {
        rt_kprintf("%02x: ", row);
        for (col = 0; col < 16; col++) {
            rt_uint16_t addr = (rt_uint16_t)(row + col);

            if (addr < start || addr > end) {
                rt_kprintf("   ");
                continue;
            }

            if (i2c_probe_addr(bus, addr))
                rt_kprintf("%02x ", addr);
            else
                rt_kprintf("-- ");
        }
        rt_kprintf("\n");
    }
}
MSH_CMD_EXPORT(i2cdetect, i2c scan tool)
