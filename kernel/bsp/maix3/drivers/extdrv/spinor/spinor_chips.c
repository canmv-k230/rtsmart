/* Supported chips. Each entry cites its datasheet; adding a chip only needs
 * an entry here (see "Adding a chip" in README.md). */
#include "spinor_core.h"

/* ---- Winbond -------------------------------------------------------------
 * QE is status register 2 bit 1 with 31h and 50h; 0Bh/3Bh/6Bh use 8 dummy
 * clocks; ADS is status register 3 bit 0. */

/* W25Q256JW Rev I, section 9.6 p.84: tPP 5 ms, tSE 400 ms, tBE2 2 s. */
static const struct spinor_timing w25q256jw_timing = {
    .program_ms = 5,
    .erase_4k_ms = 400,
    .erase_64k_ms = 2000,
};

/* W25Q512NW Rev F, section 9.6 p.87: tPP 3 ms, tSE 200 ms, tBE2 2 s. */
static const struct spinor_timing w25q512nw_timing = {
    .program_ms = 3,
    .erase_4k_ms = 200,
    .erase_64k_ms = 2000,
};

/* ---- GigaDevice ----------------------------------------------------------
 * GD25LX256E Rev1.6 / GD25LX512ME Rev1.4. The octal parts power up in
 * extended SPI mode (NVCR byte 0 = FFh, section 4.2). They have no
 * 1-1-2/1-1-4 reads and no QE bit; 8Bh/7Ch 1-1-8 reads use a fixed 8 dummy
 * clocks in SPI mode (configuration register notes, p.24-25). The
 * datasheet's 8-dummy row allows 104 MHz. Built-in ECC is disabled for any
 * 8-byte chunk programmed twice (section 6.1), as JFFS2 does when marking
 * nodes obsolete; the data stays correct. */

/* Worst case over the 85/105/125 C grades (GD25LX256E p.74-78). */
static const struct spinor_timing gd25lx_timing = {
    .program_ms = 3,
    .erase_4k_ms = 700,
    .erase_64k_ms = 3000,
};

/* ---- XTX -----------------------------------------------------------------
 * XT25F256B Rev 1.7: device ID table p21, instruction set Table 2 p18-20,
 * status registers section 3, AC characteristics p80-81. 0B 40 19 is the
 * 3.3 V part. */

static const struct spinor_timing xt25f256b_timing = {
    .program_ms = 2,      /* tPP max 1.25 ms */
    .erase_4k_ms = 1500,  /* tSE max */
    .erase_64k_ms = 5000, /* tBE2 max */
};

#define W25Q_FLAGS (SPINOR_F_DUAL_READ | SPINOR_F_QUAD_READ)
#define GD25LX_FLAGS (SPINOR_F_OCTAL_READ | SPINOR_F_4B_OPCODES)

const struct spinor_chip spinor_chips[] = {
    {
        /* EF 60 19 is shared by FW and JW parts. No FW datasheet is
         * available and the 3 V FV lacks 4-byte program and erase commands,
         * so use four-byte mode. 0Bh/3Bh/6Bh run up to 104 MHz. */
        .name = "W25Q256FW/JW",
        .id = {0xef, 0x60, 0x19},
        .capacity = 32U << 20,
        .flags = W25Q_FLAGS | SPINOR_F_4B_MODE | SPINOR_F_4B_MODE_ADS,
        .qe = SPINOR_QE_SR2_BIT1_31H,
        .timing = &w25q256jw_timing,
    },
    {
        /* -IQ/-IN: QE fixed at 1 (section 7.1.9). Dedicated 4-byte
         * commands, Tables 1-4 p.26-29. */
        .name = "W25Q512NW-IQ/IN",
        .id = {0xef, 0x60, 0x20},
        .capacity = 64U << 20,
        .flags = W25Q_FLAGS | SPINOR_F_4B_OPCODES,
        .qe = SPINOR_QE_SR2_BIT1_31H,
        .timing = &w25q512nw_timing,
    },
    {
        /* -IM (DTR variant): QE defaults to 0 and is programmable. */
        .name = "W25Q512NW-IM",
        .id = {0xef, 0x80, 0x20},
        .capacity = 64U << 20,
        .flags = W25Q_FLAGS | SPINOR_F_4B_OPCODES,
        .qe = SPINOR_QE_SR2_BIT1_31H,
        .timing = &w25q512nw_timing,
    },
    {
        .name = "GD25LX256E",
        .id = {0xc8, 0x68, 0x19},
        .capacity = 32U << 20,
        .flags = GD25LX_FLAGS,
        .qe = SPINOR_QE_NONE,
        .timing = &gd25lx_timing,
    },
    {
        .name = "GD25LX512ME",
        .id = {0xc8, 0x68, 0x1a},
        .capacity = 64U << 20,
        .flags = GD25LX_FLAGS,
        .qe = SPINOR_QE_NONE,
        .timing = &gd25lx_timing,
    },
    {
        /* Dedicated 4-byte-address commands keep the chip in its power-on
         * address mode. QE is status register 2 bit 1; 01h takes one byte
         * only, so QE is written with 31h. */
        .name = "XT25F256B",
        .id = {0x0b, 0x40, 0x19},
        .capacity = 32U << 20,
        .flags = SPINOR_F_DUAL_READ | SPINOR_F_QUAD_READ | SPINOR_F_4B_OPCODES,
        .qe = SPINOR_QE_SR2_BIT1_31H,
        .timing = &xt25f256b_timing,
    },
    {RT_NULL},
};
