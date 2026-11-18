/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 */

#ifndef __DEVICE_FS_H__
#define __DEVICE_FS_H__

#include <rtthread.h>

int devfs_init(void);

/*
 * rt_device_open() for a dfs_file_ops.open callback: returns 0 once the
 * device is open (rt_device_open()'s RT_EOK, or -RT_ENOSYS from the device's
 * open op), otherwise a negative POSIX errno for user space.
 */
int devfs_device_open(rt_device_t device, rt_uint16_t oflag);

#endif
