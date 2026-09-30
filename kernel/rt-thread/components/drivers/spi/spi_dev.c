/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 */

#include "rtdef.h"
#include <rtthread.h>
#include <drivers/spi.h>

#ifdef RT_USING_USERSPACE
#include <lwp_user_mm.h>

static rt_bool_t _spi_user_buffer_valid(const void *buffer, rt_size_t length)
{
    if (buffer == RT_NULL || length == 0 || lwp_self() == RT_NULL)
    {
        return RT_TRUE;
    }

    return lwp_user_accessable((void *)buffer, length) ? RT_TRUE : RT_FALSE;
}

static rt_bool_t _spi_user_message_valid(const struct rt_spi_message *message)
{
    return _spi_user_buffer_valid(message->send_buf, message->length) &&
           _spi_user_buffer_valid(message->recv_buf, message->length);
}

static rt_err_t _spi_user_message_prepare(struct rt_spi_message *message,
                                          void **send_alloc,
                                          void **recv_alloc,
                                          void **user_recv_buf)
{
    *send_alloc = RT_NULL;
    *recv_alloc = RT_NULL;
    *user_recv_buf = message->recv_buf;

    if (lwp_self() == RT_NULL || message->length == 0)
    {
        return RT_EOK;
    }
    if (!_spi_user_message_valid(message))
    {
        return -RT_EINVAL;
    }

    if (message->send_buf != RT_NULL)
    {
        *send_alloc = rt_malloc(message->length);
        if (*send_alloc == RT_NULL)
        {
            return -RT_ENOMEM;
        }
        if (lwp_get_from_user(*send_alloc, (void *)message->send_buf,
                              message->length) != message->length)
        {
            return -RT_EINVAL;
        }
        message->send_buf = *send_alloc;
    }

    if (message->recv_buf != RT_NULL)
    {
        *recv_alloc = rt_malloc(message->length);
        if (*recv_alloc == RT_NULL)
        {
            return -RT_ENOMEM;
        }
        message->recv_buf = *recv_alloc;
    }

    return RT_EOK;
}

static rt_err_t _spi_user_message_finish(const struct rt_spi_message *message,
                                         void *user_recv_buf,
                                         void *recv_alloc,
                                         rt_size_t transferred)
{
    if (recv_alloc == RT_NULL || transferred == 0)
    {
        return RT_EOK;
    }
    if (transferred > message->length)
    {
        transferred = message->length;
    }

    return lwp_put_to_user(user_recv_buf, recv_alloc, transferred) == transferred ?
           RT_EOK : -RT_EINVAL;
}

static void _spi_user_message_release(void *send_alloc, void *recv_alloc)
{
    if (send_alloc != RT_NULL)
    {
        rt_free(send_alloc);
    }
    if (recv_alloc != RT_NULL)
    {
        rt_free(recv_alloc);
    }
}
#endif

static rt_size_t _spidev_transfer(struct rt_spi_device *device,
                                  const void *send_buf,
                                  void *recv_buf,
                                  rt_size_t size)
{
    if (device->bus->mode & RT_SPI_BUS_MODE_QSPI)
    {
        struct rt_qspi_message message;

        rt_memset(&message, 0, sizeof(message));
        message.parent.send_buf = send_buf;
        message.parent.recv_buf = recv_buf;
        message.parent.length = size;
        message.parent.cs_take = 1;
        message.parent.cs_release = 1;
        message.qspi_data_lines = 1;

        return rt_qspi_transfer_message((struct rt_qspi_device *)device,
                                        &message);
    }

    return rt_spi_transfer(device, send_buf, recv_buf, size);
}

/* SPI bus device interface, compatible with RT-Thread 0.3.x/1.0.x */
static rt_size_t _spi_bus_device_read(rt_device_t dev,
                                      rt_off_t    pos,
                                      void       *buffer,
                                      rt_size_t   size)
{
    struct rt_spi_bus *bus;

    bus = (struct rt_spi_bus *)dev;
    RT_ASSERT(bus != RT_NULL);
    RT_ASSERT(bus->parent.user_data != RT_NULL);

    if (bus->mode & RT_SPI_BUS_MODE_QSPI)
        return 0;

    return rt_spi_transfer(bus->parent.user_data, RT_NULL, buffer, size);
}

static rt_size_t _spi_bus_device_write(rt_device_t dev,
                                       rt_off_t    pos,
                                       const void *buffer,
                                       rt_size_t   size)
{
    struct rt_spi_bus *bus;

    bus = (struct rt_spi_bus *)dev;
    RT_ASSERT(bus != RT_NULL);
    RT_ASSERT(bus->parent.user_data != RT_NULL);

    if (bus->mode & RT_SPI_BUS_MODE_QSPI)
        return 0;

    return rt_spi_transfer(bus->parent.user_data, buffer, RT_NULL, size);
}

static rt_err_t _spi_bus_device_control(rt_device_t dev,
                                        int         cmd,
                                        void       *args)
{
    struct rt_spi_bus *bus;
    rt_err_t ret = -RT_EINVAL;
#ifdef RT_USING_USERSPACE
    struct rt_qspi_configuration qspi_cfg;
    struct rt_spi_configuration spi_cfg;
    struct rt_qspi_message qspi_msg;
    struct rt_spi_message spi_msg;
    void *send_alloc;
    void *recv_alloc;
    void *user_recv_buf;
#endif

    bus = (struct rt_spi_bus *)dev;
    RT_ASSERT(bus != RT_NULL);
    RT_ASSERT(bus->parent.user_data != RT_NULL);

    switch (cmd)
    {
        case RT_SPI_DEV_CTRL_CONFIG:
#ifdef RT_USING_USERSPACE
            if (args == RT_NULL)
            {
                return -RT_EINVAL;
            }
            if (bus->mode & RT_SPI_BUS_MODE_QSPI)
            {
                if (LWP_GET_FROM_USER(&qspi_cfg, args, struct rt_qspi_configuration) != 0)
                {
                    return -RT_EINVAL;
                }
                ret = rt_qspi_configure(bus->parent.user_data, &qspi_cfg);
            }
            else
            {
                if (LWP_GET_FROM_USER(&spi_cfg, args, struct rt_spi_configuration) != 0)
                {
                    return -RT_EINVAL;
                }
                ret = rt_spi_configure(bus->parent.user_data, &spi_cfg);
            }
#else
            if (bus->mode & RT_SPI_BUS_MODE_QSPI)
            {
                ret = rt_qspi_configure(bus->parent.user_data, args);
            }
            else
            {
                ret = rt_spi_configure(bus->parent.user_data, args);
            }
#endif
            break;
        case RT_SPI_DEV_CTRL_RW:
#ifdef RT_USING_USERSPACE
            if (args == RT_NULL)
            {
                return -RT_EINVAL;
            }
            if (bus->mode & RT_SPI_BUS_MODE_QSPI)
            {
                if (LWP_GET_FROM_USER(&qspi_msg, args, struct rt_qspi_message) != 0)
                {
                    return -RT_EINVAL;
                }
                if (qspi_msg.parent.next != RT_NULL)
                {
                    return -RT_EINVAL;
                }
                ret = _spi_user_message_prepare(&qspi_msg.parent, &send_alloc,
                                                &recv_alloc, &user_recv_buf);
                if (ret != RT_EOK)
                {
                    _spi_user_message_release(send_alloc, recv_alloc);
                    return ret;
                }
                ret = rt_qspi_transfer_message(bus->parent.user_data, &qspi_msg);
                if (ret > 0 && _spi_user_message_finish(&qspi_msg.parent,
                                                        user_recv_buf, recv_alloc,
                                                        ret) != RT_EOK)
                {
                    ret = -RT_EINVAL;
                }
                _spi_user_message_release(send_alloc, recv_alloc);
            }
            else
            {
                if (LWP_GET_FROM_USER(&spi_msg, args, struct rt_spi_message) != 0)
                {
                    return -RT_EINVAL;
                }
                if (spi_msg.next != RT_NULL)
                {
                    return -RT_EINVAL;
                }
                ret = _spi_user_message_prepare(&spi_msg, &send_alloc,
                                                &recv_alloc, &user_recv_buf);
                if (ret != RT_EOK)
                {
                    _spi_user_message_release(send_alloc, recv_alloc);
                    return ret;
                }
                if(RT_NULL != rt_spi_transfer_message(bus->parent.user_data, &spi_msg))
                {
                    ret = RT_ERROR;
                }
                else
                {
                    ret = RT_EOK;
                    if (_spi_user_message_finish(&spi_msg, user_recv_buf,
                                                 recv_alloc, spi_msg.length) != RT_EOK)
                    {
                        ret = -RT_EINVAL;
                    }
                }
                _spi_user_message_release(send_alloc, recv_alloc);
            }
#else
            if (bus->mode & RT_SPI_BUS_MODE_QSPI)
            {
                ret = rt_qspi_transfer_message(bus->parent.user_data, args);
            }
            else
            {
                if(RT_NULL != rt_spi_transfer_message(bus->parent.user_data, args))
                {
                    ret = RT_ERROR;
                }
                else
                {
                    ret = RT_EOK;
                }
            }
#endif
            break;
        default:
            break;
    }

    return ret;
}

rt_err_t  _spi_bus_device_open(rt_device_t dev, rt_uint16_t oflag)
{
    rt_err_t res;
    struct rt_spi_bus *bus;

    bus = (struct rt_spi_bus*)dev;
    rt_mutex_take(&(bus->lock), RT_WAITING_FOREVER);
    if (bus->parent.user_data == NULL)
    {
        char dev_name[32];
        rt_snprintf(dev_name, sizeof(dev_name), "%s_dev", bus->parent.parent.name);

        if (bus->mode & RT_SPI_BUS_MODE_QSPI)
        {
            struct rt_qspi_device* qspi_device = rt_malloc(sizeof(struct rt_qspi_device));
            struct rt_qspi_configuration cfg = {
                .parent.mode = 0,
                .parent.reserved = 0,
                .parent.data_width = 0,
                .parent.max_hz = 0,
                .ddr_mode = 0,
                .medium_size = 0,
                .qspi_dl_width = 1,
            };
            if (qspi_device == RT_NULL)
            {
                rt_kprintf("no memory, alloc %s failed\n", dev_name);
                goto exit;
            }
            rt_memset(qspi_device, 0, sizeof(struct rt_qspi_device));
            rt_memcpy(&qspi_device->config, &cfg, sizeof(struct rt_qspi_configuration));
            res = rt_spi_bus_attach_device(&qspi_device->parent, dev_name, bus->parent.parent.name, RT_NULL);
            if (res != RT_EOK)
            {
                rt_free(qspi_device);
                rt_kprintf("%s attach  failed\n", dev_name);
                goto exit;
            }
            bus->parent.user_data = qspi_device;
        } else {
            struct rt_spi_configuration cfg = {
                .mode = 0,
                .reserved = 0,
                .data_width = 8,
                .max_hz = 1000000,
            };
            struct rt_spi_device* spi_device = rt_malloc(sizeof(struct rt_spi_device));
            if (spi_device == RT_NULL)
            {
                rt_kprintf("no memory, alloc %s failed\n", dev_name);
                goto exit;
            }
            rt_memset(spi_device, 0, sizeof(struct rt_spi_device));
            rt_memcpy(&spi_device->config, &cfg, sizeof(struct rt_spi_configuration));
            res = rt_spi_bus_attach_device(spi_device, dev_name, bus->parent.parent.name, RT_NULL);
            if (res != RT_EOK)
            {
                rt_free(spi_device);
                rt_kprintf("%s attach  failed\n", dev_name);
                goto exit;
            }
            bus->parent.user_data = spi_device;
        }
    }
exit:
    rt_mutex_release(&(bus->lock));
    return 0;
}

rt_err_t  _spi_bus_device_close(rt_device_t dev)
{
    rt_err_t res;
    struct rt_spi_bus *bus = (struct rt_spi_bus *)dev;

    res = rt_device_unregister(&((struct rt_spi_device*)bus->parent.user_data)->parent);
    if (res != RT_EOK)
    {
        rt_kprintf("device unregister failed!\n");
        return res;
    }
    rt_free(bus->parent.user_data);
    bus->parent.user_data = NULL;

    return 0;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops spi_bus_ops = 
{
    RT_NULL,
    _spi_bus_device_open,
    _spi_bus_device_close,
    _spi_bus_device_read,
    _spi_bus_device_write,
    _spi_bus_device_control
};
#endif

rt_err_t rt_spi_bus_device_init(struct rt_spi_bus *bus, const char *name)
{
    struct rt_device *device;
    RT_ASSERT(bus != RT_NULL);

    device = &bus->parent;

    /* set device type */
    device->type    = RT_Device_Class_SPIBUS;
    /* initialize device interface */
#ifdef RT_USING_DEVICE_OPS
    device->ops     = &spi_bus_ops;
#else
    device->init    = RT_NULL;
    device->open    = RT_NULL;
    device->close   = RT_NULL;
    device->read    = _spi_bus_device_read;
    device->write   = _spi_bus_device_write;
    device->control = _spi_bus_device_control;
#endif

    /* register to device manager */
    return rt_device_register(device, name, RT_DEVICE_FLAG_RDWR);
}

/* SPI Dev device interface, compatible with RT-Thread 0.3.x/1.0.x */
static rt_size_t _spidev_device_read(rt_device_t dev,
                                     rt_off_t    pos,
                                     void       *buffer,
                                     rt_size_t   size)
{
    struct rt_spi_device *device;

    device = (struct rt_spi_device *)dev;
    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(device->bus != RT_NULL);

    return _spidev_transfer(device, RT_NULL, buffer, size);
}

static rt_size_t _spidev_device_write(rt_device_t dev,
                                      rt_off_t    pos,
                                      const void *buffer,
                                      rt_size_t   size)
{
    struct rt_spi_device *device;

    device = (struct rt_spi_device *)dev;
    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(device->bus != RT_NULL);

    return _spidev_transfer(device, buffer, RT_NULL, size);
}

static rt_err_t _spidev_device_control(rt_device_t dev,
                                       int         cmd,
                                       void       *args)
{
    switch (cmd)
    {
    case 0: /* set device */
        break;
    case 1: 
        break;
    }

    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops spi_device_ops = 
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    _spidev_device_read,
    _spidev_device_write,
    _spidev_device_control
};
#endif

rt_err_t rt_spidev_device_init(struct rt_spi_device *dev, const char *name)
{
    struct rt_device *device;
    RT_ASSERT(dev != RT_NULL);

    device = &(dev->parent);

    /* set device type */
    device->type    = RT_Device_Class_SPIDevice;
#ifdef RT_USING_DEVICE_OPS
    device->ops     = &spi_device_ops;
#else
    device->init    = RT_NULL;
    device->open    = RT_NULL;
    device->close   = RT_NULL;
    device->read    = _spidev_device_read;
    device->write   = _spidev_device_write;
    device->control = _spidev_device_control;
#endif

    /* register to device manager */
    return rt_device_register(device, name, RT_DEVICE_FLAG_RDWR);
}
