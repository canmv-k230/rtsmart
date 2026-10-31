/* RT-Thread MTD NOR adapter: one device per partition. Erase blocks may group
 * physical sectors (JFFS2 uses 64 KiB on the EVB). */
#include "spinor_core.h"
#include <drivers/mtd_nor.h>

struct spinor_mtd {
    struct rt_mtd_nor_device device;
    struct spinor_flash *flash;
    rt_uint32_t offset;
    rt_uint32_t size;
};

static rt_bool_t range_valid(const struct spinor_mtd *part, rt_off_t offset, rt_size_t length)
{
    return offset >= 0 && (rt_size_t)offset <= part->size && length <= part->size - (rt_size_t)offset;
}

static rt_err_t mtd_read_id(struct rt_mtd_nor_device *dev)
{
    return ((struct spinor_mtd *)dev)->flash->jedec_id;
}

static rt_size_t mtd_read(struct rt_mtd_nor_device *dev, rt_off_t offset, rt_uint8_t *buffer, rt_uint32_t length)
{
    struct spinor_mtd *part = (struct spinor_mtd *)dev;
    rt_err_t error = range_valid(part, offset, length) ?
        spinor_read(part->flash, part->offset + offset, buffer, length) : -RT_EINVAL;
    if (error) {
        rt_set_errno(error);
        return 0;
    }
    return length;
}

static rt_size_t mtd_write(struct rt_mtd_nor_device *dev, rt_off_t offset, const rt_uint8_t *buffer,
                           rt_uint32_t length)
{
    struct spinor_mtd *part = (struct spinor_mtd *)dev;
    rt_err_t error = range_valid(part, offset, length) ?
        spinor_write(part->flash, part->offset + offset, buffer, length) : -RT_EINVAL;
    if (error) {
        rt_set_errno(error);
        return 0;
    }
    return length;
}

static rt_err_t mtd_erase(struct rt_mtd_nor_device *dev, rt_off_t offset, rt_uint32_t length)
{
    struct spinor_mtd *part = (struct spinor_mtd *)dev;
    if (!range_valid(part, offset, length) || offset % dev->block_size || length % dev->block_size)
        return -RT_EINVAL;
    return spinor_erase(part->flash, part->offset + offset, length);
}

static const struct rt_mtd_nor_driver_ops mtd_ops = {
    .read_id = mtd_read_id,
    .read = mtd_read,
    .write = mtd_write,
    .erase_block = mtd_erase,
};

static rt_bool_t partition_valid(const struct spinor_flash *flash, const struct spinor_partition *parts,
                                 rt_size_t index, rt_uint32_t block_size)
{
    const struct spinor_partition *part = &parts[index];
    if (!part->name || !part->size || part->offset % block_size || part->size % block_size ||
        part->offset > flash->chip->capacity || part->size > flash->chip->capacity - part->offset)
        return RT_FALSE;
    for (rt_size_t i = 0; i < index; ++i)
        if (part->offset < parts[i].offset + parts[i].size && parts[i].offset < part->offset + part->size)
            return RT_FALSE;
    return RT_TRUE;
}

rt_err_t spinor_add_partitions(struct spinor_flash *flash, const struct spinor_partition *parts,
                               rt_size_t count, rt_uint32_t block_size)
{
    struct spinor_mtd *mtds;
    rt_err_t error;
    if (!flash || !flash->open || flash->parts || !parts || !count || !block_size ||
        block_size % SPINOR_SECTOR_SIZE)
        return -RT_EINVAL;
    for (rt_size_t i = 0; i < count; ++i) {
        if (!partition_valid(flash, parts, i, block_size)) {
            rt_kprintf("SPI NOR: partition %s does not fit the flash or overlaps another\n",
                       parts[i].name ? parts[i].name : "?");
            return -RT_EINVAL;
        }
    }
    mtds = rt_calloc(count, sizeof(*mtds));
    if (!mtds) return -RT_ENOMEM;
    for (rt_size_t i = 0; i < count; ++i) {
        mtds[i].flash = flash;
        mtds[i].offset = parts[i].offset;
        mtds[i].size = parts[i].size;
        mtds[i].device.block_size = block_size;
        mtds[i].device.block_end = parts[i].size / block_size;
        mtds[i].device.ops = &mtd_ops;
        error = rt_mtd_nor_register_device(parts[i].name, &mtds[i].device);
        if (error) {
            rt_kprintf("SPI NOR: register %s failed (%d)\n", parts[i].name, error);
            while (i--) rt_device_unregister(&mtds[i].device.parent);
            rt_free(mtds);
            return error;
        }
    }
    flash->parts = mtds;
    flash->part_count = count;
    return RT_EOK;
}

void spinor_mtd_remove_partitions(struct spinor_flash *flash)
{
    for (rt_size_t i = 0; i < flash->part_count; ++i)
        rt_device_unregister(&flash->parts[i].device.parent);
    rt_free(flash->parts);
    flash->parts = RT_NULL;
    flash->part_count = 0;
}
