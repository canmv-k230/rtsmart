/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"
#include "usbh_msc.h"

#include "rtthread.h"
#include <dfs_fs.h>

#define DBG_TAG "usb.msc"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define DEV_FORMAT "sd%c"

#ifndef CONFIG_USB_DFS_MOUNT_POINT
#define CONFIG_USB_DFS_MOUNT_POINT "/udisk"
#endif

static rt_err_t rt_udisk_init(rt_device_t dev)
{
    return RT_EOK;
}

static rt_size_t rt_udisk_read(rt_device_t dev, rt_off_t pos, void *buffer,
                             rt_size_t size)
{
    struct usbh_msc *msc_class = (struct usbh_msc *)dev->user_data;
    int ret;

    bool bounce = ((uintptr_t)buffer & (CONFIG_USB_ALIGN_SIZE - 1)) != 0;
    uint8_t *dma_buf = buffer;

    if (bounce) {
        dma_buf = rt_malloc_align(size * msc_class->blocksize,
                                  CONFIG_USB_ALIGN_SIZE);
        if (!dma_buf) {
            rt_kprintf("msc get align buf failed\n");
            return 0;
        }
    }

    ret = usbh_msc_scsi_read10(msc_class, pos, dma_buf, size);
    if (ret < 0) {
        rt_kprintf("usb mass_storage read failed\n");
    } else if (bounce) {
        rt_memcpy(buffer, dma_buf, size * msc_class->blocksize);
    }
    if (bounce) {
        rt_free_align(dma_buf);
    }

    return ret < 0 ? 0 : size;
}

static rt_size_t rt_udisk_write(rt_device_t dev, rt_off_t pos, const void *buffer,
                              rt_size_t size)
{
    struct usbh_msc *msc_class = (struct usbh_msc *)dev->user_data;
    int ret;

    bool bounce = ((uintptr_t)buffer & (CONFIG_USB_ALIGN_SIZE - 1)) != 0;
    uint8_t *dma_buf = (uint8_t *)buffer;

    if (bounce) {
        dma_buf = rt_malloc_align(size * msc_class->blocksize,
                                  CONFIG_USB_ALIGN_SIZE);
        if (!dma_buf) {
            rt_kprintf("msc get align buf failed\n");
            return 0;
        }
        rt_memcpy(dma_buf, buffer, size * msc_class->blocksize);
    }

    ret = usbh_msc_scsi_write10(msc_class, pos, dma_buf, size);
    if (ret < 0) {
        rt_kprintf("usb mass_storage write failed\n");
    }
    if (bounce) {
        rt_free_align(dma_buf);
    }

    return ret < 0 ? 0 : size;
}

static rt_err_t rt_udisk_control(rt_device_t dev, int cmd, void *args)
{
    /* check parameter */
    RT_ASSERT(dev != RT_NULL);
    struct usbh_msc *msc_class = (struct usbh_msc *)dev->user_data;

    if (cmd == RT_DEVICE_CTRL_BLK_GETGEOME) {
        struct rt_device_blk_geometry *geometry;

        geometry = (struct rt_device_blk_geometry *)args;
        if (geometry == RT_NULL)
            return -RT_ERROR;

        geometry->bytes_per_sector = msc_class->blocksize;
        geometry->block_size = msc_class->blocksize;
        geometry->sector_count = msc_class->blocknum;
    }

    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops udisk_device_ops = {
    rt_udisk_init,
    RT_NULL,
    RT_NULL,
    rt_udisk_read,
    rt_udisk_write,
    rt_udisk_control
};
#endif

int udisk_init(struct usbh_msc *msc_class)
{
    rt_err_t ret = 0;
    uint8_t *sector;
    struct dfs_partition part0;
    struct rt_device *dev;
    char name[CONFIG_USBHOST_DEV_NAMELEN];
    char mount_point[CONFIG_USBHOST_DEV_NAMELEN];

    if (msc_class->blocksize < 512)
        return -RT_EINVAL;

    dev = rt_calloc(1, sizeof(struct rt_device));
    if (!dev)
        return -RT_ENOMEM;

    sector = rt_malloc_align(msc_class->blocksize, CONFIG_USB_ALIGN_SIZE);
    if (!sector) {
        rt_free(dev);
        return -RT_ENOMEM;
    }

    snprintf(name, CONFIG_USBHOST_DEV_NAMELEN, DEV_FORMAT, msc_class->sdchar);
    snprintf(mount_point, CONFIG_USBHOST_DEV_NAMELEN, CONFIG_USB_DFS_MOUNT_POINT, msc_class->sdchar);

    ret = usbh_msc_scsi_read10(msc_class, 0, sector, 1);
    if (ret != RT_EOK) {
        LOG_E("USB storage partition read failed: %d", ret);
        rt_free_align(sector);
        rt_free(dev);
        return ret;
    }

    if (dfs_filesystem_get_partition(&part0, sector, 0) == RT_EOK) {
        LOG_D("partition 0: type=%d offset=0x%x size=0x%x",
              part0.type, part0.offset, part0.size);
    }
    rt_free_align(sector);

    dev->type = RT_Device_Class_Block;
#ifdef RT_USING_DEVICE_OPS
    dev->ops = &udisk_device_ops;
#else
    dev->init = rt_udisk_init;
    dev->read = rt_udisk_read;
    dev->write = rt_udisk_write;
    dev->control = rt_udisk_control;
#endif
    dev->user_data = msc_class;

    ret = rt_device_register(dev, name, RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_REMOVABLE | RT_DEVICE_FLAG_STANDALONE);
    if (ret) {
        LOG_E("failed to register USB storage /dev/%s: %d", name, ret);
        rt_free(dev);
        goto out;
    }

    ret = dfs_mount(name, mount_point, "elm", 0, 0);
    if (ret == 0) {
        LOG_I("USB storage attached as /dev/%s, mounted at %s", name,
              mount_point);
    } else {
        LOG_W("USB storage attached as /dev/%s, mount at %s failed: %d",
              name, mount_point, ret);
    }

out:
    return ret;
}

void usbh_msc_run(struct usbh_msc *msc_class)
{
    udisk_init(msc_class);
}

void usbh_msc_stop(struct usbh_msc *msc_class)
{
    struct rt_device *dev;

    char name[CONFIG_USBHOST_DEV_NAMELEN];
    char mount_point[CONFIG_USBHOST_DEV_NAMELEN];

    snprintf(name, CONFIG_USBHOST_DEV_NAMELEN, DEV_FORMAT, msc_class->sdchar);
    snprintf(mount_point, CONFIG_USBHOST_DEV_NAMELEN, CONFIG_USB_DFS_MOUNT_POINT, msc_class->sdchar);

    dfs_unmount(mount_point);
    dev = rt_device_find(name);
    if (dev) {
        rt_device_unregister(dev);
        rt_free(dev);
        LOG_I("USB storage detached from /dev/%s", name);
    }
}
