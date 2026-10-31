#include <rtthread.h>
#include <rtdevice.h>

#ifdef BSP_USING_SPI_NOR
#include "spinor.h"
#include "sysctl_boot.h"

/* JFFS2 erase blocks; the driver uses 64 KiB block erases for them. */
#define NOR_FS_BLOCK_SIZE (64U * 1024U)

static const struct spinor_partition nor_parts[] = {
#include "spinor_parts.h"
};

static const struct spinor_config nor_config = {
    .bus = "spi0",
    .device = "spinor0",
    .cs = 0x81, /* OSPI CS0 */
    .probe_hz = BSP_SPI_NOR_MAX_HZ < 50000000 ? BSP_SPI_NOR_MAX_HZ : 50000000,
    .max_hz = BSP_SPI_NOR_MAX_HZ,
    .max_lines = BSP_SPI_NOR_MAX_LINES,
};

static int board_spinor_init(void)
{
    struct spinor_flash *flash;
    rt_err_t ret;

    if (g_sysctl_boot_mode != SYSCTL_BOOT_NORFLASH)
        return RT_EOK;

    flash = spinor_attach(&nor_config);
    if (!flash)
        return -RT_EIO;
    ret = spinor_add_partitions(flash, nor_parts, sizeof(nor_parts) / sizeof(nor_parts[0]),
                                NOR_FS_BLOCK_SIZE);
    if (ret != RT_EOK)
        spinor_detach(flash);
    return ret;
}
INIT_COMPONENT_EXPORT(board_spinor_init);
#endif
