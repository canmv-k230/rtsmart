/*
* Copyright (c) 2022-2024, ArtInChip Technology Co., Ltd
*
* SPDX-License-Identifier: Apache-2.0
*
* Authors: xuan.wen <xuan.wen@artinchip.com>
*/

#include <rtdevice.h>
#include "spinand_mtd.h"

#define DBG_TAG "spi_nand_mtd"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

static void spinand_dump_buffer(int page, rt_uint8_t *buf, int len,
                                const char *title)
{
#if DBG_LVL >= DBG_LOG
    int i;

    if(!buf || !len) {
        return;
    }

    rt_kprintf("[%s-Page-%d]\n", title, page);

    for (i = 0; i < len; i++) {
        rt_kprintf("%02X ", buf[i]);
        if (i % 16 == 15)
            rt_kprintf("\n");
    }
    rt_kprintf("\n");
#endif
}

static rt_err_t spinand_read_id(struct rt_mtd_nand_device *device)
{
    rt_err_t result = RT_EOK;
    uint32_t id = 0;
    struct aic_spinand *flash = (struct aic_spinand *)device->priv;

    RT_ASSERT(device != RT_NULL);

    result = rt_mutex_take(flash->lock, RT_WAITING_FOREVER);
    RT_ASSERT(result == RT_EOK);

    spinand_read_id_op(flash, (uint8_t *)&id);

    result = rt_mutex_release(flash->lock);
    RT_ASSERT(result == RT_EOK);

    LOG_I("Id: 0x%08x\n", id);

    result = (id != 0x0) ? RT_EOK : -RT_ERROR;
    return result;
}

static rt_err_t spinand_mtd_read(struct rt_mtd_nand_device *device,
                                rt_off_t page, rt_uint8_t *data,
                                rt_uint32_t data_len, rt_uint8_t *spare,
                                rt_uint32_t spare_len)
{
    struct aic_spinand *flash;
    rt_err_t result;

    RT_ASSERT(device != RT_NULL);

    if (page < 0 || page / device->pages_per_block < device->block_start ||
        page / device->pages_per_block > device->block_end) {
        LOG_E("[Error] read page:%d\n", page);
        return -RT_MTD_EIO;
    }

    flash = (struct aic_spinand *)device->priv;

    result = rt_mutex_take(flash->lock, RT_WAITING_FOREVER);
    RT_ASSERT(result == RT_EOK);

    result = spinand_read_page(flash, page, data, data_len, spare, spare_len);

    spinand_dump_buffer(page, (uint8_t *)data, data_len, "READ DATA");
    spinand_dump_buffer(page, (uint8_t *)spare, spare_len, "READ SPARE");

    rt_mutex_release(flash->lock);

    return result;
}

static rt_err_t spinand_mtd_write(struct rt_mtd_nand_device *device,
                                rt_off_t page, const rt_uint8_t *data,
                                rt_uint32_t data_len, const rt_uint8_t *spare,
                                rt_uint32_t spare_len)
{
    struct aic_spinand *flash = (struct aic_spinand *)device->priv;
    RT_ASSERT(device != RT_NULL);
    rt_err_t result = RT_EOK;

    if (page < 0 || page / device->pages_per_block < device->block_start ||
        page / device->pages_per_block > device->block_end) {
        LOG_E("[Error] write page:%d\n", page);
        return -RT_MTD_EIO;
    }

    spinand_dump_buffer(page, (uint8_t *)data, data_len, "WRITE DATA");
    spinand_dump_buffer(page, (uint8_t *)spare, spare_len, "WRITE SPARE");

    result = rt_mutex_take(flash->lock, RT_WAITING_FOREVER);
    RT_ASSERT(result == RT_EOK);

    result = spinand_write_page(flash, page, data, data_len, spare, spare_len);
    rt_mutex_release(flash->lock);
    return result;
}

static rt_err_t spinand_mtd_erase(struct rt_mtd_nand_device *device,
                                rt_uint32_t block)
{
    rt_err_t result = RT_EOK;
    struct aic_spinand *flash = (struct aic_spinand *)device->priv;

    RT_ASSERT(device != RT_NULL);

    if (block < device->block_start || block > device->block_end) {
        LOG_E("[Error] block:%d block_end:%d\n", block, device->block_end);
        return -RT_MTD_EIO;
    }

    LOG_D("erase block: %d\n", block);

    result = rt_mutex_take(flash->lock, RT_WAITING_FOREVER);
    RT_ASSERT(result == RT_EOK);

    result = spinand_block_erase(flash, block);
    if (result != RT_EOK)
        goto exit_spinand_mtd_erase;

    result = RT_EOK;

exit_spinand_mtd_erase:

    rt_mutex_release(flash->lock);

    return result;
}

static rt_err_t spinand_mtd_block_isbad(struct rt_mtd_nand_device *device,
                                        rt_uint32_t block)
{
    struct aic_spinand *flash = (struct aic_spinand *)device->priv;
    rt_err_t result = RT_EOK;

    RT_ASSERT(device != RT_NULL);

    if (block < device->block_start || block > device->block_end) {
        LOG_E("[Error] block:%d\n", block);
        return -RT_MTD_EIO;
    }

    pr_debug("check block status: %d\n", block);

    result = rt_mutex_take(flash->lock, RT_WAITING_FOREVER);
    RT_ASSERT(result == RT_EOK);

    result = spinand_block_isbad(flash, block);

    rt_mutex_release(flash->lock);

    return result;
}

static rt_err_t spinand_mtd_block_markbad(struct rt_mtd_nand_device *device,
                                        rt_uint32_t block)
{
    rt_err_t result = RT_EOK;
    struct aic_spinand *flash = (struct aic_spinand *)device->priv;

    RT_ASSERT(device != RT_NULL);

    if (block < device->block_start || block > device->block_end) {
        LOG_E("[Error] block:%d\n", block);
        return -RT_MTD_EIO;
    }

    LOG_I("mark bad block: %d\n", block);

    result = rt_mutex_take(flash->lock, RT_WAITING_FOREVER);
    RT_ASSERT(result == RT_EOK);

    /* Erase block after checking it is bad or not. */
    if (spinand_block_isbad(flash, block) != 0) {
        LOG_W("Block %d is bad.\n", block);
        result = RT_EOK;
    } else {
        result = spinand_block_markbad(flash, block);
    }

    rt_mutex_release(flash->lock);

    return result;
}

static rt_err_t spinand_map_user(struct rt_mtd_nand_device *device,
                                        rt_uint8_t *oobbuf, rt_uint8_t *buf,
                                        rt_base_t start, rt_base_t nbytes)
{
    rt_err_t result = RT_EOK;
    struct aic_spinand *flash = (struct aic_spinand *)device->priv;

    RT_ASSERT(device != RT_NULL);

    result = spinand_ooblayout_map_user(flash, oobbuf, buf, start, nbytes);

    return result;
}

static rt_err_t spinand_unmap_user(struct rt_mtd_nand_device *device,
                                        rt_uint8_t *dst, rt_uint8_t *src,
                                        rt_base_t start, rt_base_t nbytes)
{
    rt_err_t result = RT_EOK;
    struct aic_spinand *flash = (struct aic_spinand *)device->priv;

    RT_ASSERT(device != RT_NULL);

    result = spinand_ooblayout_unmap_user(flash, dst, src, start, nbytes);

    return result;
}

static const struct rt_mtd_nand_driver_ops spinand_ops = {
    .read_id = spinand_read_id,
    .read_page = spinand_mtd_read,
    .write_page = spinand_mtd_write,
    .erase_block = spinand_mtd_erase,
    .check_block = spinand_mtd_block_isbad,
    .mark_badblock = spinand_mtd_block_markbad,
    .map_user = spinand_map_user,
    .unmap_user = spinand_unmap_user,
};

rt_err_t spinand_mtd_register(struct rt_mtd_nand_device *device, const char *name,
                              struct aic_spinand *flash, rt_uint32_t offset,
                              rt_uint32_t size)
{
    rt_uint32_t block_size;
    uint64_t capacity;

    if (!device || !name || !flash || !flash->IsInited || !flash->info)
        return -RT_EINVAL;

    block_size = (rt_uint32_t)flash->info->page_size * flash->info->pages_per_eraseblock;
    capacity = (uint64_t)block_size * flash->info->block_per_lun;
    if (!block_size || !size || offset % block_size || size % block_size ||
        (uint64_t)offset + size > capacity)
        return -RT_EINVAL;

    rt_memset(device, 0, sizeof(*device));
    device->page_size = flash->info->page_size;
    device->pages_per_block = flash->info->pages_per_eraseblock;
    device->oob_size = flash->info->oob_size;
    device->oob_free = 32;
    device->plane_num = flash->info->planes_per_lun;
    /* RT-Thread/UFFS uses absolute page/block numbers and an inclusive end. */
    device->block_start = offset / block_size;
    device->block_total = size / block_size;
    device->block_end = device->block_start + device->block_total - 1;
    device->ops = &spinand_ops;
    device->priv = flash;

    return rt_mtd_nand_register_device(name, device);
}

rt_err_t spinand_mtd_unregister(struct rt_mtd_nand_device *device)
{
    if (!device)
        return -RT_EINVAL;
    return rt_device_unregister(&device->parent);
}
