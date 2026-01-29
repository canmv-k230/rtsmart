/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author        Notes
 * 2012-04-25     weety         first version
 * 2014-08-03     bernard       fix some compiling warning
 * 2021-04-20     RiceChen      added support for bus clock control
 */

#include <rtdevice.h>

#ifdef RT_USING_USERSPACE
#include <lwp_user_mm.h>
#endif

#define DBG_TAG               "I2C"
#ifdef RT_I2C_DEBUG
#define DBG_LVL               DBG_LOG
#else
#define DBG_LVL               DBG_INFO
#endif
#include <rtdbg.h>

static rt_size_t i2c_bus_device_read(rt_device_t dev,
                                     rt_off_t    pos,
                                     void       *buffer,
                                     rt_size_t   count)
{
    rt_uint16_t addr;
    rt_uint16_t flags;
    struct rt_i2c_bus_device *bus = (struct rt_i2c_bus_device *)dev->user_data;

    RT_ASSERT(bus != RT_NULL);
    RT_ASSERT(buffer != RT_NULL);

    LOG_D("I2C bus dev [%s] reading %u bytes.", dev->parent.name, count);

    addr = pos & 0xffff;
    flags = (pos >> 16) & 0xffff;

    return rt_i2c_master_recv(bus, addr, flags, (rt_uint8_t *)buffer, count);
}

static rt_size_t i2c_bus_device_write(rt_device_t dev,
                                      rt_off_t    pos,
                                      const void *buffer,
                                      rt_size_t   count)
{
    rt_uint16_t addr;
    rt_uint16_t flags;
    struct rt_i2c_bus_device *bus = (struct rt_i2c_bus_device *)dev->user_data;

    RT_ASSERT(bus != RT_NULL);
    RT_ASSERT(buffer != RT_NULL);

    LOG_D("I2C bus dev [%s] writing %u bytes.", dev->parent.name, count);

    addr = pos & 0xffff;
    flags = (pos >> 16) & 0xffff;

    return rt_i2c_master_send(bus, addr, flags, (const rt_uint8_t *)buffer, count);
}

static rt_err_t i2c_bus_device_control(rt_device_t dev,
                                       int         cmd,
                                       void       *args)
{
    rt_err_t ret;
    struct rt_i2c_priv_data *priv_data;
    struct rt_i2c_bus_device *bus = (struct rt_i2c_bus_device *)dev->user_data;
    rt_uint32_t bus_clock;

    RT_ASSERT(bus != RT_NULL);

    switch (cmd)
    {
    /* set 7-bit addr mode */
    case RT_I2C_DEV_CTRL_7BIT:
        bus->flags &= ~RT_I2C_ADDR_10BIT;
        break;
    /* set 10-bit addr mode */
    case RT_I2C_DEV_CTRL_10BIT:
        bus->flags |= RT_I2C_ADDR_10BIT;
        break;
    case RT_I2C_DEV_CTRL_TIMEOUT:
#ifdef RT_USING_USERSPACE
        if (args == RT_NULL)
        {
            return -RT_EINVAL;
        }
        if (LWP_GET_FROM_USER(&bus->timeout, args, rt_uint32_t) != 0)
        {
            return -RT_EINVAL;
        }
#else
        bus->timeout = *(rt_uint32_t *)args;
#endif
        break;
    case RT_I2C_DEV_CTRL_RW:
    {
#ifdef RT_USING_USERSPACE
        struct rt_i2c_priv_data user_priv;
        struct rt_i2c_msg *kmsgs = RT_NULL;
        rt_uint8_t **user_bufs = RT_NULL;
        rt_size_t i;

        if (args == RT_NULL)
        {
            return -RT_EINVAL;
        }

        /* copy rt_i2c_priv_data from user/kernel space */
        if (LWP_GET_FROM_USER(&user_priv, args, struct rt_i2c_priv_data) != 0)
        {
            return -RT_EINVAL;
        }

        if (user_priv.msgs == RT_NULL || user_priv.number == 0)
        {
            return -RT_EINVAL;
        }

        /* allocate kernel-side message array */
        kmsgs = (struct rt_i2c_msg *)rt_calloc(user_priv.number, sizeof(struct rt_i2c_msg));
        if (kmsgs == RT_NULL)
        {
            return -RT_ENOMEM;
        }

        /* keep original user buffers so we can copy data back */
        user_bufs = (rt_uint8_t **)rt_calloc(user_priv.number, sizeof(rt_uint8_t *));
        if (user_bufs == RT_NULL)
        {
            rt_free(kmsgs);
            return -RT_ENOMEM;
        }

        /* copy each rt_i2c_msg from user to kernel */
        for (i = 0; i < user_priv.number; i++)
        {
            if (LWP_GET_FROM_USER(&kmsgs[i],
                                  &user_priv.msgs[i],
                                  struct rt_i2c_msg) != 0)
            {
                ret = -RT_EINVAL;
                goto __i2c_rw_cleanup;
            }

            user_bufs[i] = kmsgs[i].buf;
            if (kmsgs[i].len == 0)
            {
                kmsgs[i].buf = RT_NULL;
                continue;
            }

            if (kmsgs[i].buf == RT_NULL)
            {
                ret = -RT_EINVAL;
                goto __i2c_rw_cleanup;
            }

            kmsgs[i].buf = (rt_uint8_t *)rt_malloc(kmsgs[i].len);
            if (kmsgs[i].buf == RT_NULL)
            {
                ret = -RT_ENOMEM;
                goto __i2c_rw_cleanup;
            }

            /* for write messages, copy data from user buffer */
            if (!(kmsgs[i].flags & RT_I2C_RD))
            {
                if (lwp_get_from_user(kmsgs[i].buf,
                                      user_bufs[i],
                                      kmsgs[i].len) != kmsgs[i].len)
                {
                    ret = -RT_EINVAL;
                    goto __i2c_rw_cleanup;
                }
            }
        }

        /* perform transfer using kernel buffers */
        ret = rt_i2c_transfer(bus, kmsgs, user_priv.number);
        if (ret < 0)
        {
            ret = -RT_EIO;
            goto __i2c_rw_cleanup;
        }

        /* copy read data back to user buffers */
        for (i = 0; i < user_priv.number; i++)
        {
            if ((kmsgs[i].flags & RT_I2C_RD) &&
                (kmsgs[i].len > 0) &&
                (user_bufs[i] != RT_NULL))
            {
                if (lwp_put_to_user(user_bufs[i],
                                    kmsgs[i].buf,
                                    kmsgs[i].len) != kmsgs[i].len)
                {
                    ret = -RT_EINVAL;
                    goto __i2c_rw_cleanup;
                }
            }
        }

        ret = RT_EOK;

__i2c_rw_cleanup:
        if (kmsgs != RT_NULL)
        {
            for (i = 0; i < user_priv.number; i++)
            {
                if (kmsgs[i].buf != RT_NULL && kmsgs[i].len > 0)
                {
                    rt_free(kmsgs[i].buf);
                }
            }
            rt_free(kmsgs);
        }
        if (user_bufs != RT_NULL)
        {
            rt_free(user_bufs);
        }

        if (ret < 0)
        {
            return ret;
        }
#else
        priv_data = (struct rt_i2c_priv_data *)args;
        ret = rt_i2c_transfer(bus, priv_data->msgs, priv_data->number);
        if (ret < 0)
        {
            return -RT_EIO;
        }
#endif
        break;
    }
    case RT_I2C_DEV_CTRL_CLK:
        if (args == RT_NULL)
        {
            return -RT_EINVAL;
        }
#ifdef RT_USING_USERSPACE
        if (LWP_GET_FROM_USER(&bus_clock, args, rt_uint32_t) != 0)
        {
            return -RT_EINVAL;
        }
#else
        bus_clock = *(rt_uint32_t *)args;
#endif
        ret = rt_i2c_control(bus, cmd, bus_clock);
        if (ret < 0)
        {
            return -RT_EIO;
        }
        break;
    default:
        break;
    }

    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops i2c_ops =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    i2c_bus_device_read,
    i2c_bus_device_write,
    i2c_bus_device_control
};
#endif

rt_err_t rt_i2c_bus_device_device_init(struct rt_i2c_bus_device *bus,
                                       const char               *name)
{
    struct rt_device *device;
    RT_ASSERT(bus != RT_NULL);

    device = &bus->parent;

    device->user_data = bus;

    /* set device type */
    device->type    = RT_Device_Class_I2CBUS;
    /* initialize device interface */
#ifdef RT_USING_DEVICE_OPS
    device->ops     = &i2c_ops;
#else
    device->init    = RT_NULL;
    device->open    = RT_NULL;
    device->close   = RT_NULL;
    device->read    = i2c_bus_device_read;
    device->write   = i2c_bus_device_write;
    device->control = i2c_bus_device_control;
#endif

    /* register to device manager */
    rt_device_register(device, name, RT_DEVICE_FLAG_RDWR);

    return RT_EOK;
}
