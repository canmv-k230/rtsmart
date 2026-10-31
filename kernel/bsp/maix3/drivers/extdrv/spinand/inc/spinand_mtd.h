#ifndef K230_SPINAND_MTD_H
#define K230_SPINAND_MTD_H

#include <drivers/mtd_nand.h>
#include "spinand.h"

/* The caller owns the MTD device and keeps flash alive while registered.
 * Offset/size are bytes, aligned to physical erase blocks. MTD callbacks use
 * absolute flash page/block numbers, as required by RT-Thread/UFFS. */
rt_err_t spinand_mtd_register(struct rt_mtd_nand_device *device, const char *name,
                              struct aic_spinand *flash, rt_uint32_t offset,
                              rt_uint32_t size);
rt_err_t spinand_mtd_unregister(struct rt_mtd_nand_device *device);

#endif
