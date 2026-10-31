/* Private to the SPI NOR driver: chip descriptions, flash state and the
 * interfaces between core, QSPI binding and MTD adapter. */
#ifndef K230_SPINOR_CORE_H
#define K230_SPINOR_CORE_H
#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/spi.h>
#include "spinor.h"

#define SPINOR_PAGE_SIZE 256U
#define SPINOR_SECTOR_SIZE 4096U
#define SPINOR_BLOCK_SIZE 65536U
/* RT-Thread QSPI transfer limits honored by the core. */
#define SPINOR_READ_LIMIT 65536U
#define SPINOR_WRITE_LIMIT SPINOR_PAGE_SIZE

/* JEDEC commands and status bits shared by SPI NOR devices. */
#define SPINOR_OP_READ_ID 0x9f
#define SPINOR_OP_READ_STATUS 0x05
#define SPINOR_OP_READ_STATUS2 0x35
#define SPINOR_OP_READ_STATUS3 0x15
#define SPINOR_OP_WRITE_STATUS 0x01
#define SPINOR_OP_WRITE_STATUS2 0x31
#define SPINOR_OP_VOLATILE_SR_WRITE_ENABLE 0x50
#define SPINOR_OP_WRITE_ENABLE 0x06
#define SPINOR_OP_WRITE_DISABLE 0x04
#define SPINOR_OP_ENTER_4B_MODE 0xb7
#define SPINOR_OP_RESET_ENABLE 0x66
#define SPINOR_OP_RESET 0x99
#define SPINOR_STATUS_BUSY 0x01
#define SPINOR_STATUS_WRITE_LATCH 0x02
#define SPINOR_STATUS2_QE 0x02
#define SPINOR_STATUS3_ADS 0x01

/* Capability flags. Every chip supports 1-1-1 fast read (0Bh), 256-byte
 * page program and 4 KiB and 64 KiB erases. */
#define SPINOR_F_DUAL_READ (1U << 0)    /* 1-1-2, 3Bh */
#define SPINOR_F_QUAD_READ (1U << 1)    /* 1-1-4, 6Bh; see qe */
#define SPINOR_F_OCTAL_READ (1U << 2)   /* 1-1-8, 8Bh */
#define SPINOR_F_4B_OPCODES (1U << 3)   /* 0Ch/3Ch/6Ch/7Ch, 12h, 21h, DCh */
#define SPINOR_F_4B_MODE (1U << 4)      /* enter with B7h */
#define SPINOR_F_4B_MODE_WREN (1U << 5) /* B7h needs write enable */
#define SPINOR_F_4B_MODE_ADS (1U << 6)  /* status register 3 bit 0 shows the mode */

/* How the core sets the quad-enable bit for 1-1-4 reads. The driver only
 * writes volatile status bits; QE set in nonvolatile memory is used as is. */
enum spinor_qe {
    SPINOR_QE_NONE,         /* no QE bit: quad reads always available */
    SPINOR_QE_SR2_BIT1_31H, /* status register 2 bit 1, written with 31h */
    SPINOR_QE_SR2_BIT1_01H, /* status register 2 bit 1, written as 01h SR1 SR2 */
    SPINOR_QE_NONVOLATILE,  /* no volatile write: quad only when QE is already set */
};

/* Datasheet maximum times. */
struct spinor_timing {
    rt_uint32_t program_ms;
    rt_uint32_t erase_4k_ms;
    rt_uint32_t erase_64k_ms;
};

/* Data-only chip profile; the core derives every command from it. */
struct spinor_chip {
    const char *name;
    rt_uint8_t id[3];
    rt_uint32_t capacity;
    rt_uint32_t flags;
    rt_uint8_t qe; /* enum spinor_qe */
    const struct spinor_timing *timing;
};

/* spinor_chips.c; ends with an entry whose name is RT_NULL. */
extern const struct spinor_chip spinor_chips[];

struct spinor_erase_type {
    rt_uint32_t size;
    rt_uint32_t timeout_ms;
    rt_uint8_t opcode;
};

struct spinor_mtd;

struct spinor_flash {
    struct rt_qspi_device device;
    struct rt_mutex lock;
    const struct spinor_chip *chip;
    rt_uint32_t jedec_id;
    rt_uint32_t clock_hz;
    struct spinor_erase_type erase[2]; /* 4 KiB, 64 KiB */
    rt_uint8_t address_bytes;
    rt_uint8_t program_opcode;
    rt_uint8_t read_opcode;
    rt_uint8_t data_lines;
    rt_bool_t open;
    rt_bool_t needs_wait;
    struct spinor_mtd *parts;
    rt_size_t part_count;
};

/* One chip-select assertion. Commands and addresses use one data line;
 * receive data can use one, two, four or eight lines. */
struct spinor_transfer {
    rt_uint32_t address;
    rt_uint8_t opcode;
    rt_uint8_t address_bytes;
    rt_uint8_t dummy_cycles;
    rt_uint8_t receive_lines;
    const void *tx;
    void *rx;
    rt_size_t length;
};

/* spinor_core.c: protocol. The QSPI device must be attached and configured
 * for single-line transfers before spinor_core_open(). */
rt_err_t spinor_core_open(struct spinor_flash *flash);
void spinor_core_close(struct spinor_flash *flash);
rt_bool_t spinor_core_has_read_lines(const struct spinor_flash *flash, rt_uint8_t lines);
rt_err_t spinor_core_set_read_lines(struct spinor_flash *flash, rt_uint8_t lines);

/* spinor_qspi.c: RT-Thread QSPI transport. */
rt_err_t spinor_bus_execute(struct spinor_flash *flash, const struct spinor_transfer *op);
rt_err_t spinor_bus_set_lines(struct spinor_flash *flash, rt_uint8_t lines);
void spinor_bus_delay_us(rt_uint32_t us);

/* spinor_mtd.c */
#ifdef RT_USING_MTD_NOR
void spinor_mtd_remove_partitions(struct spinor_flash *flash);
#endif
#endif
