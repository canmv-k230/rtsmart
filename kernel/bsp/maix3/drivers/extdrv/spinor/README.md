# Native SPI NOR

A native RT-Smart driver for the boot SPI NOR flash. Chips are described by
data-only profiles; one generic core derives every command from them.

| File | Responsibility |
| --- | --- |
| `inc/spinor.h` | Public API: attach/detach, info, read/write/erase, MTD partitions |
| `spinor_core.h` | Private: chip description, flash state, internal interfaces |
| `spinor_core.c` | Reset, identification, addressing, read modes, quad enable, serialized reads/programs/erases |
| `spinor_qspi.c` | RT-Thread QSPI transfers and bus configuration, attach/detach, read-mode tuning |
| `spinor_mtd.c` | MTD NOR partitions |
| `spinor_chips.c` | Chip profiles with datasheet citations |
| `board/spinor.c` (outside this directory) | Bus, clock and wiring limits and the partition table |

## Usage

```c
static const struct spinor_partition parts[] = {
    { "nor0", 16 << 20, 4 << 20 },
};
static const struct spinor_config config = {
    .bus = "spi0", .device = "spinor0", .cs = 0x81,
    .probe_hz = 50000000, .max_hz = 100000000, .max_lines = 4,
};

struct spinor_flash *flash = spinor_attach(&config);
if (flash && spinor_add_partitions(flash, parts, 1, 64 * 1024) != RT_EOK)
    spinor_detach(flash);
```

`spinor_attach()` attaches the SPI device, then:

1. Issues the JEDEC software reset (66h, 99h) in single-line SPI and polls
   BUSY for up to 50 ms in case the chip needs longer (for example after a
   reset during an erase).
2. Reads the three-byte JEDEC ID (9Fh) and looks it up in `spinor_chips.c`.
   Unknown IDs are rejected.
3. Chooses addressing: three-byte for 16 MiB or less; otherwise the
   dedicated four-byte-address commands when the profile has them, else
   four-byte mode (B7h).
4. Tunes reads. It hashes 32 samples spread over the whole chip with
   single-line reads at `probe_hz`, then tries the widest mode the profile
   and `max_lines` allow at `max_hz`, narrowing until the samples match. If
   none matches, it keeps single-line reads at `probe_hz`.

`spinor_add_partitions()` checks every partition (alignment to the block
size, fit, no overlap) before registering any, and registers each as an MTD
NOR device. All calls return `rt_err_t`; the MTD adapter converts to the
byte counts RT-Thread expects.

## Supported chips

| Chip | ID | Size | Reads | Addressing | Quad enable |
| --- | --- | --- | --- | --- | --- |
| W25Q256FW/JW | EF 60 19 | 32 MiB | 1-1-1/2/4 | B7h mode, ADS checked | SR2 bit 1, volatile via 50h + 31h |
| W25Q512NW-IQ/IN | EF 60 20 | 64 MiB | 1-1-1/2/4 | 4-byte opcodes | fixed at 1 |
| W25Q512NW-IM | EF 80 20 | 64 MiB | 1-1-1/2/4 | 4-byte opcodes | SR2 bit 1, volatile via 50h + 31h |
| GD25LX256E | C8 68 19 | 32 MiB | 1-1-1/8 | 4-byte opcodes | none |
| GD25LX512ME | C8 68 1A | 64 MiB | 1-1-1/8 | 4-byte opcodes | none |
| XT25F256B | 0B 40 19 | 32 MiB | 1-1-1/2/4 | 4-byte opcodes | SR2 bit 1, volatile via 50h + 31h |

Only the W25Q256 has been run on hardware. The others are datasheet-only
until a board with that chip is tested.

## Adding a chip

Add an entry to `spinor_chips.c`: the ID, capacity, capability flags
(`SPINOR_F_*`), the quad-enable method and a timing record of datasheet
maximum program/erase times, with a datasheet citation. Every chip must
support 0Bh fast read, 256-byte page program with 02h (12h with four-byte
opcodes), and 4 KiB (20h/21h) and 64 KiB (D8h/DCh) erases. Check the chip
against the limits below; a chip that breaks one needs a new flag or method
in the core, not chip-specific code.

## Performance

- Reads use the widest verified mode at the board clock limit.
- An erase uses 64 KiB block erases wherever the range is 64 KiB aligned
  and 4 KiB sector erases elsewhere. The EVB's JFFS2 partitions use 64 KiB
  erase blocks, so each block is one block-erase command.
- Page programs are polled without sleeping for up to 1 ms; erases sleep
  between status polls.
- Writes split at page boundaries; reads at 64 KiB per transfer.

## Concurrency and verification

All I/O shares one flash mutex. Successful programs and erases wait for
completion before releasing it, so ordinary reads need no status poll. An
uncertain mutation forces a bounded readiness check before the next
operation. Programs are read back, which detects ignored commands and
protected locations; erases are not, because JFFS2 checks every erased
block. A failed write or erase may have changed part of its range. The
driver does not unlock protected areas or change nonvolatile status bits.
`spinor_detach()` waits for an operation in progress, but all users must be
gone first.

## Limits for other chips

- **Reset.** A chip left in QPI, octal, DTR or continuous-read (XIP) mode
  ignores the single-line reset and fails identification. No boot stage
  enables those modes.
- **Identification.** Exactly three JEDEC ID bytes, matched exactly. No
  SFDP, so parts that share an ID (W25Q256FW/JW) get the conservative
  common profile, and manufacturer IDs with continuation codes (7Fh) are not
  handled.
- **Status and completion.** BUSY is status register 1 bit 0 and WEL bit 1.
  Flag status registers and program/erase error bits (GD25LX, XT25F256B) are
  not read; failures are detected by program readback and JFFS2's erase
  check.
- **Commands and addresses** always use one line, with 1, 2, 4 or 8 data
  lines (1-1-x); every multi-line read uses 8 dummy clocks. No 1-2-2/1-4-4
  (BBh/EBh) or 1-8-8 reads, QPI, octal I/O or DTR. The K230 controller needs
  single-line dummy clocks in multiples of 8 up to 32; multi-line reads
  accept 0-31.
- **Four-byte mode** assumes B7h, optionally after write enable, with the
  mode shown in status register 3 bit 0. Chips that show it elsewhere
  (XT25F256B uses status register 2 bit 0) must use dedicated four-byte
  opcodes. Power-up four-byte mode (ADP=1) is not supported on chips of
  16 MiB or less.
- **Quad enable** is status register 2 bit 1, written as volatile status
  through 50h then 31h or 01h. Chips without a volatile write get quad reads
  only when QE is already set.
- **Erases** are 4 KiB and 64 KiB only. No 32 KiB erases, no erase/program
  suspend, so reads wait for an erase to finish.
- **Protection.** No unlock. Chips that power up locked fail every program
  with `-RT_EIO`.
- **Size and dies.** One chip select, one die, capacity below 4 GiB.
- **Transport.** The K230 controller's limits apply: 64 KiB per read, 256
  bytes per write, 8-bit data width.

There are no automated tests; board boot, reboot and read/write checks are
needed for each change.
