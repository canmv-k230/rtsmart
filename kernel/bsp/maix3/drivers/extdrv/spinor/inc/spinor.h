#ifndef K230_SPINOR_H
#define K230_SPINOR_H
#include <rtthread.h>

struct spinor_flash;

struct spinor_config {
    const char *bus;      /* SPI bus, e.g. "spi0" */
    const char *device;   /* name for the attached SPI device */
    rt_uint8_t cs;        /* hard_cs value for the bus driver */
    rt_uint32_t probe_hz; /* clock for identification and reference reads */
    rt_uint32_t max_hz;   /* board clock limit for reads */
    rt_uint8_t max_lines; /* data lines wired: 1, 2, 4 or 8 */
};

struct spinor_info {
    const char *name;
    rt_uint32_t jedec_id;
    rt_uint32_t capacity;
    rt_uint32_t page_size;
    rt_uint32_t erase_size; /* smallest erase */
    rt_uint32_t clock_hz;   /* requested read clock; the controller rounds down */
    rt_uint8_t read_lines;
};

struct spinor_partition {
    const char *name;
    rt_uint32_t offset;
    rt_uint32_t size;
};

/* Attach the flash, identify it and choose its read mode: the widest mode
 * the chip and wiring allow at max_hz whose reads match single-line reads
 * at probe_hz, else single-line reads at probe_hz. Returns RT_NULL on
 * failure. */
struct spinor_flash *spinor_attach(const struct spinor_config *config);
/* Remove partitions, detach the SPI device and free the flash. No users may
 * remain. */
void spinor_detach(struct spinor_flash *flash);
void spinor_get_info(const struct spinor_flash *flash, struct spinor_info *info);

/* Byte offsets and lengths. A failed write or erase may have changed part of
 * the range. Erase ranges must be aligned to info.erase_size. */
rt_err_t spinor_read(struct spinor_flash *flash, rt_uint32_t offset, void *buffer, rt_size_t length);
rt_err_t spinor_write(struct spinor_flash *flash, rt_uint32_t offset, const void *buffer, rt_size_t length);
rt_err_t spinor_erase(struct spinor_flash *flash, rt_uint32_t offset, rt_size_t length);

#ifdef RT_USING_MTD_NOR
/* Register each partition as an MTD NOR device whose erase blocks are
 * block_size bytes (a multiple of info.erase_size). Every partition is
 * checked before any is registered. Call at most once per flash. */
rt_err_t spinor_add_partitions(struct spinor_flash *flash, const struct spinor_partition *parts,
                               rt_size_t count, rt_uint32_t block_size);
#endif
#endif
