/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * AIC manufacturing RF-test command device. The firmware ABI follows the
 * vendor driver's DBG_RFTEST_CMD_REQ/CFM messages; userspace gets a bounded
 * binary interface instead of the Linux driver's private netdev ioctl.
 */
#include "aic8800_wifi.h"
#include "aic8800_rf_test.h"

#include <dfs_file.h>
#include <errno.h>
#include <lwp_user_mm.h>

#define DBG_TAG "aic8800.rf"
#define DBG_LVL AIC8800_DBG_LVL
#include <rtdbg.h>

#define AIC8800_RF_DEVICE_PREFIX "aicrf"

_Static_assert(AIC8800_RF_ARGUMENT_SIZE == AIC_RFTEST_ARGUMENT_SIZE &&
               AIC8800_RF_RESULT_SIZE == AIC_RFTEST_RESULT_SIZE,
               "RF-test userspace and firmware ABI sizes differ");
_Static_assert(sizeof(struct aic8800_rf_command) == 176,
               "RF-test ioctl ABI changed");

struct aic8800_rf_test_device
{
    struct rt_device device;
    struct aic8800_context *context;
    struct rt_mutex lock;
    struct rt_semaphore idle;
    rt_uint16_t active_operations;
    rt_bool_t opened;
    rt_bool_t registered;
    rt_bool_t unregistering;
    rt_bool_t destroying;
};

static struct aic8800_rf_test_device *aic_rf_device_from_fd(
    struct dfs_fd *fd)
{
    struct rt_device *device;

    if (!fd || !fd->fnode)
    {
        return RT_NULL;
    }
    device = (struct rt_device *)fd->fnode->data;
    return device ? device->user_data : RT_NULL;
}

static void aic_rf_put_le32(rt_uint8_t *destination, rt_uint32_t value)
{
    destination[0] = (rt_uint8_t)value;
    destination[1] = (rt_uint8_t)(value >> 8);
    destination[2] = (rt_uint8_t)(value >> 16);
    destination[3] = (rt_uint8_t)(value >> 24);
}

static void aic_rf_device_destroy(struct aic8800_rf_test_device *device)
{
    RT_ASSERT(device && !device->registered && !device->opened &&
              !device->active_operations);
    rt_sem_detach(&device->idle);
    rt_mutex_detach(&device->lock);
    rt_free(device);
}

static int aic_rf_chip_type(const struct aic8800_context *context,
                            rt_uint32_t *chip_type)
{
    switch (context->product_id)
    {
    case AIC8800_USB_PID_AIC8800:
    case AIC8800_USB_PID_AIC8801:
        *chip_type = AIC8800_RF_CHIP_AIC8800D;
        return 0;
    case AIC8800_USB_PID_AIC8800DC:
    case AIC8800_USB_PID_AIC8800DW:
        *chip_type = AIC8800_RF_CHIP_AIC8800DCDW;
        return 0;
    case AIC8800_USB_PID_AIC8800D80:
    case AIC8800_USB_PID_AIC8800D81:
    case AIC8800_USB_PID_AIC8800D83:
    case AIC8800_USB_PID_AIC8800D84:
    case AIC8800_USB_PID_AIC8800D85:
    case AIC8800_USB_PID_AIC8800D86:
    case AIC8800_USB_PID_AIC8800D88:
    case AIC8800_USB_PID_AIC8800D40:
    case AIC8800_USB_PID_AIC8800D41:
        *chip_type = AIC8800_RF_CHIP_AIC8800D80;
        return 0;
    case AIC8800_USB_PID_AIC8800D80X2:
    case AIC8800_USB_PID_AIC8800D81X2:
    case AIC8800_USB_PID_AIC8800D89X2:
        *chip_type = AIC8800_RF_CHIP_AIC8800D80X2;
        return 0;
    default:
        return -ENODEV;
    }
}

static int aic_rf_validate_command(const struct aic8800_context *context,
                                   const struct aic8800_rf_command *request)
{
    if (request->command == AIC8800_RF_SET_POWER)
    {
        if (request->argument_length != 1U ||
            (context->product_id != AIC8800_USB_PID_AIC8801 &&
             request->arguments[0] > 0x1eU))
        {
            return -EINVAL;
        }
    }
    if (request->command != AIC8800_RF_RDWR_PWRIDX)
    {
        return 0;
    }
    if (context->product_id == AIC8800_USB_PID_AIC8801)
    {
        return request->argument_length == 0U ||
               request->argument_length == 3U ? 0 : -EINVAL;
    }
    return request->argument_length == 0U ||
           request->argument_length == 4U ||
           request->argument_length == 12U ||
           request->argument_length == 14U ? 0 : -EINVAL;
}

static int aic_rf_rt_error_to_errno(rt_err_t error)
{
    switch (error)
    {
    case RT_EOK: return 0;
    case -RT_ETIMEOUT: return -ETIMEDOUT;
    case -RT_EFULL: return -ENOSPC;
    case -RT_EEMPTY: return -EAGAIN;
    case -RT_ENOMEM: return -ENOMEM;
    case -RT_ENOSYS: return -ENOSYS;
    case -RT_EBUSY: return -EBUSY;
    case -RT_EIO: return -EIO;
    case -RT_EINTR: return -EINTR;
    case -RT_EINVAL: return -EINVAL;
    case -RT_ERROR:
    case -RT_ETRAP:
    default: return -EIO;
    }
}

static int aic_rf_device_open(struct dfs_fd *fd)
{
    struct aic8800_rf_test_device *device = aic_rf_device_from_fd(fd);

    if (!device)
    {
        return -ENODEV;
    }
    rt_mutex_take(&device->lock, RT_WAITING_FOREVER);
    if (!device->registered || device->unregistering)
    {
        rt_mutex_release(&device->lock);
        return -ENODEV;
    }
    if (device->opened)
    {
        rt_mutex_release(&device->lock);
        return -EBUSY;
    }
    device->opened = RT_TRUE;
    rt_mutex_release(&device->lock);
    return 0;
}

static int aic_rf_device_close(struct dfs_fd *fd)
{
    struct aic8800_rf_test_device *device = aic_rf_device_from_fd(fd);
    rt_bool_t destroy;

    if (!device)
    {
        return -ENODEV;
    }
    rt_mutex_take(&device->lock, RT_WAITING_FOREVER);
    device->opened = RT_FALSE;
    destroy = !device->registered && !device->active_operations &&
              !device->unregistering && !device->destroying;
    if (destroy)
    {
        device->destroying = RT_TRUE;
    }
    rt_mutex_release(&device->lock);
    if (destroy)
    {
        aic_rf_device_destroy(device);
    }
    return 0;
}

static int aic_rf_device_ioctl(struct dfs_fd *fd, int command, void *argument)
{
    struct aic8800_rf_test_device *device = aic_rf_device_from_fd(fd);
    struct aic8800_context *context;
    struct aic8800_rf_command request;
    rt_uint32_t chip_type;
    rt_uint8_t wire_request[AIC_RFTEST_REQUEST_SIZE];
    rt_size_t response_length = 0;
    rt_err_t result;

    if (!device)
    {
        return -ENODEV;
    }
    if (!argument)
    {
        return -EINVAL;
    }
    if (command == AIC8800_RF_IOCTL_GET_CHIP_TYPE ||
        command == AIC8800_RF_IOCTL_GET_PRODUCT_ID)
    {
        rt_mutex_take(&device->lock, RT_WAITING_FOREVER);
        if (!device->registered || device->unregistering || !device->opened ||
            !device->context)
        {
            rt_mutex_release(&device->lock);
            return -ENODEV;
        }
        result = aic_rf_chip_type(device->context, &chip_type);
        if (result == 0 && command == AIC8800_RF_IOCTL_GET_PRODUCT_ID)
        {
            chip_type = device->context->product_id;
        }
        rt_mutex_release(&device->lock);
        if (result != 0)
        {
            return result;
        }
        return lwp_put_to_user(argument, &chip_type, sizeof(chip_type)) ==
               sizeof(chip_type) ? 0 : -EFAULT;
    }
    if (command != AIC8800_RF_IOCTL_COMMAND)
    {
        return -ENOTTY;
    }
    if (lwp_get_from_user(&request, argument, sizeof(request)) !=
        sizeof(request))
    {
        return -EFAULT;
    }
    if (request.argument_length > sizeof(request.arguments) ||
        request.reserved)
    {
        return -EINVAL;
    }

    rt_mutex_take(&device->lock, RT_WAITING_FOREVER);
    if (!device->registered || device->unregistering || !device->opened ||
        !device->context)
    {
        rt_mutex_release(&device->lock);
        return -ENODEV;
    }
    context = device->context;
    result = aic_rf_validate_command(context, &request);
    if (result != 0)
    {
        rt_mutex_release(&device->lock);
        return result;
    }
    device->active_operations++;
    rt_mutex_release(&device->lock);

    rt_memset(wire_request, 0, sizeof(wire_request));
    aic_rf_put_le32(wire_request, request.command);
    aic_rf_put_le32(wire_request + 4, request.argument_length);
    if (request.argument_length)
    {
        rt_memcpy(wire_request + 8, request.arguments,
                  request.argument_length);
    }
    rt_memset(request.result, 0, sizeof(request.result));
    result = aic8800_protocol_command(
        context, AIC_DBG_RFTEST_CMD_REQ, AIC_DBG_RFTEST_CMD_CFM,
        wire_request, sizeof(wire_request), request.result,
        sizeof(request.result), &response_length);
    result = aic_rf_rt_error_to_errno(result);
    if (result == 0)
    {
        request.result_length = response_length;
        if (lwp_put_to_user(argument, &request, sizeof(request)) !=
            sizeof(request))
        {
            result = -EFAULT;
        }
    }

    rt_mutex_take(&device->lock, RT_WAITING_FOREVER);
    device->active_operations--;
    if (!device->active_operations && device->unregistering)
    {
        rt_sem_release(&device->idle);
    }
    rt_mutex_release(&device->lock);
    return result;
}

static const struct dfs_file_ops g_aic_rf_device_fops =
{
    .open = aic_rf_device_open,
    .close = aic_rf_device_close,
    .ioctl = aic_rf_device_ioctl,
};

static rt_err_t aic_rf_device_register_auto(
    struct aic8800_rf_test_device *device)
{
    char name[RT_NAME_MAX];
    unsigned int index;
    rt_err_t result;

    for (index = 0; index < AIC8800_RF_DEVICE_COUNT; index++)
    {
        rt_snprintf(name, sizeof(name), "%s%u", AIC8800_RF_DEVICE_PREFIX,
                    index);
        if (rt_device_find(name))
        {
            continue;
        }
        /* rt_device_register() publishes the object and clears fops. Keep
         * publication and fops installation atomic with respect to threads. */
        rt_enter_critical();
        result = rt_device_register(&device->device, name,
                                    RT_DEVICE_FLAG_RDWR |
                                    RT_DEVICE_FLAG_STANDALONE);
        if (result == RT_EOK)
        {
            device->device.fops = &g_aic_rf_device_fops;
        }
        rt_exit_critical();
        if (result == RT_EOK)
        {
            return RT_EOK;
        }
    }
    return -RT_EFULL;
}

rt_err_t aic8800_rf_test_register(struct aic8800_context *context)
{
    struct aic8800_rf_test_device *device;
    rt_err_t result;

    if (!context || context->rf_test_device)
    {
        return -RT_EINVAL;
    }
    device = rt_calloc(1, sizeof(*device));
    if (!device)
    {
        return -RT_ENOMEM;
    }
    result = rt_mutex_init(&device->lock, "aic-rf", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_free(device);
        return result;
    }
    result = rt_sem_init(&device->idle, "aic-rfi", 0, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&device->lock);
        rt_free(device);
        return result;
    }
    device->context = context;
    device->registered = RT_TRUE;
    device->device.type = RT_Device_Class_Miscellaneous;
    device->device.user_data = device;
    result = aic_rf_device_register_auto(device);
    if (result != RT_EOK)
    {
        device->registered = RT_FALSE;
        aic_rf_device_destroy(device);
        return result;
    }
    context->rf_test_device = device;
    LOG_I("RF test firmware control=/dev/%s", device->device.parent.name);
    return RT_EOK;
}

rt_err_t aic8800_rf_test_unregister(struct aic8800_context *context)
{
    struct aic8800_rf_test_device *device;
    rt_bool_t destroy;
    rt_bool_t wait;
    rt_err_t result;

    if (!context || !context->rf_test_device)
    {
        return context ? RT_EOK : -RT_EINVAL;
    }
    device = context->rf_test_device;
    rt_mutex_take(&device->lock, RT_WAITING_FOREVER);
    device->registered = RT_FALSE;
    device->unregistering = RT_TRUE;
    wait = device->active_operations != 0;
    rt_mutex_release(&device->lock);
    if (wait)
    {
        rt_sem_take(&device->idle, RT_WAITING_FOREVER);
    }
    result = rt_device_unregister(&device->device);
    if (result != RT_EOK)
    {
        rt_mutex_take(&device->lock, RT_WAITING_FOREVER);
        device->registered = RT_TRUE;
        device->unregistering = RT_FALSE;
        rt_mutex_release(&device->lock);
        return result;
    }
    rt_mutex_take(&device->lock, RT_WAITING_FOREVER);
    device->context = RT_NULL;
    device->unregistering = RT_FALSE;
    destroy = !device->opened && !device->destroying;
    if (destroy)
    {
        device->destroying = RT_TRUE;
    }
    rt_mutex_release(&device->lock);
    context->rf_test_device = RT_NULL;
    if (destroy)
    {
        aic_rf_device_destroy(device);
    }
    return RT_EOK;
}
