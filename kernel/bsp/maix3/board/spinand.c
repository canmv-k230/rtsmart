/*
* Copyright (c) 2022-2024, ArtInChip Technology Co., Ltd
*
* SPDX-License-Identifier: Apache-2.0
*
* Authors: xuan.wen <xuan.wen@artinchip.com>
*/

#include <rtthread.h>
#include <rtdevice.h>

#ifdef BSP_USING_SPI_NAND
#include <drivers/spi.h>
#include "spinand_port.h"
#include "spinand_mtd.h"

static const struct {
    const char *name;
    rt_uint32_t offset;
    rt_uint32_t size;
} nand_parts[] = {
#include "spinand_parts.h"
};

static struct rt_mtd_nand_device partitions[sizeof(nand_parts) / sizeof(nand_parts[0])];
static struct rt_qspi_device nand_spi;
static struct aic_spinand nand_flash;

static int board_spinand_init(void)
{
    rt_err_t ret;
    struct rt_qspi_configuration cfg = {
        .parent.mode = RT_SPI_MODE_0 | RT_SPI_MSB,
        .parent.hard_cs = 0x81, /* CS0 */
        .parent.data_width = 8,
        .parent.max_hz = SPI_NAND_SPI_FREQ,
        .qspi_dl_width = SPI_NAND_SPI_BUS_WIDTH,
    };

    ret = rt_spi_bus_attach_device(&nand_spi.parent, "spinand0", SPI_NAND_SPI_DEV, RT_NULL);
    if (ret != RT_EOK)
        return ret;
    ret = rt_qspi_configure(&nand_spi, &cfg);
    if (ret != RT_EOK)
        goto detach;

    ret = spinand_probe(&nand_flash, &nand_spi);
    if (ret != RT_EOK) {
        rt_kprintf("SPI NAND: probe failed (%d)\n", ret);
        goto detach;
    }

    for (rt_size_t i = 0; i < sizeof(nand_parts) / sizeof(nand_parts[0]); i++) {
        ret = spinand_mtd_register(&partitions[i], nand_parts[i].name, &nand_flash,
                                   nand_parts[i].offset, nand_parts[i].size);
        if (ret != RT_EOK) {
            rt_kprintf("SPI NAND: register %s failed (%d)\n", nand_parts[i].name, ret);
            while (i > 0)
                spinand_mtd_unregister(&partitions[--i]);
            spinand_deinit(&nand_flash);
            goto detach;
        }
    }
    rt_kprintf("Found spi nand flash %s\n", nand_flash.info->sz_description);
    return RT_EOK;

detach:
    rt_device_unregister(&nand_spi.parent.parent);
    return ret;
}
INIT_COMPONENT_EXPORT(board_spinand_init);
#endif
