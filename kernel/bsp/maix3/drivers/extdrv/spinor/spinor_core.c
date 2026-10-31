/* SPI NOR protocol: identification, addressing, read modes, quad enable and
 * serialized reads, programs and erases. Chip differences come only from
 * the profiles in spinor_chips.c. */
#include "spinor_core.h"

/* After a software reset, wait tRST (30 us on W25Q256), then poll BUSY in
 * case the chip needs longer, e.g. after a reset during an erase. */
#define RESET_US 50
#define RESET_TIMEOUT_MS 50
/* Page programs usually finish within a millisecond: poll them without
 * sleeping for up to PROGRAM_SPIN_US before falling back to 1 ms sleeps. */
#define POLL_US 20
#define PROGRAM_SPIN_US 1000
/* Dummy clocks of every supported 1-1-x read command. */
#define READ_DUMMY 8

/* Read commands by data lines: three-byte (or 4-byte mode) and dedicated
 * four-byte-address opcodes. */
static const struct {
    rt_uint8_t lines;
    rt_uint32_t flag;
    rt_uint8_t opcode;
    rt_uint8_t opcode_4b;
} read_commands[] = {
    {1, 0, 0x0b, 0x0c},
    {2, SPINOR_F_DUAL_READ, 0x3b, 0x3c},
    {4, SPINOR_F_QUAD_READ, 0x6b, 0x6c},
    {8, SPINOR_F_OCTAL_READ, 0x8b, 0x7c},
};

static rt_err_t command(struct spinor_flash *flash, rt_uint8_t opcode)
{
    struct spinor_transfer op = {.opcode = opcode};
    return spinor_bus_execute(flash, &op);
}

static rt_err_t read_register(struct spinor_flash *flash, rt_uint8_t opcode, rt_uint8_t *value)
{
    struct spinor_transfer op = {.opcode = opcode, .receive_lines = 1, .rx = value, .length = 1};
    return spinor_bus_execute(flash, &op);
}

static rt_err_t write_register(struct spinor_flash *flash, rt_uint8_t opcode, const rt_uint8_t *value,
                               rt_size_t length)
{
    struct spinor_transfer op = {.opcode = opcode, .tx = value, .length = length};
    return spinor_bus_execute(flash, &op);
}

static rt_err_t ready(struct spinor_flash *flash, rt_uint32_t timeout_ms, rt_uint32_t spin_us)
{
    rt_tick_t began = rt_tick_get();
    rt_tick_t allowance = rt_tick_from_millisecond(timeout_ms);
    rt_uint8_t status;
    rt_err_t error;
    flash->needs_wait = RT_TRUE;
    do {
        error = read_register(flash, SPINOR_OP_READ_STATUS, &status);
        if (error) return error;
        if ((status & SPINOR_STATUS_BUSY) == 0) {
            flash->needs_wait = RT_FALSE;
            return RT_EOK;
        }
        if ((rt_tick_t)(rt_tick_get() - began) >= allowance) break;
        if (spin_us >= POLL_US) {
            spinor_bus_delay_us(POLL_US);
            spin_us -= POLL_US;
        } else {
            rt_thread_mdelay(1);
        }
    } while (1);
    return -RT_ETIMEOUT;
}

/* Take the flash lock. An uncertain earlier mutation may still be running:
 * wait for it for up to the longest erase time. */
static rt_err_t begin_io(struct spinor_flash *flash)
{
    rt_err_t error = rt_mutex_take(&flash->lock, RT_WAITING_FOREVER);
    if (error) return error;
    if (!flash->open)
        error = -RT_EIO;
    else if (flash->needs_wait)
        error = ready(flash, flash->erase[1].timeout_ms, 0);
    if (error) rt_mutex_release(&flash->lock);
    return error;
}

static rt_bool_t contains(const struct spinor_flash *flash, rt_uint32_t offset, rt_size_t length)
{
    if (!flash || !flash->open || offset > flash->chip->capacity) return RT_FALSE;
    return length <= flash->chip->capacity - offset;
}

static rt_bool_t uses_4b_opcodes(const struct spinor_flash *flash)
{
    return flash->address_bytes == 4 && (flash->chip->flags & SPINOR_F_4B_OPCODES);
}

/* Set QE in the volatile status register; keep every other bit. */
static rt_err_t enable_quad(struct spinor_flash *flash)
{
    rt_uint8_t status[2];
    rt_err_t error;
    if (flash->chip->qe == SPINOR_QE_NONE) return RT_EOK;
    error = read_register(flash, SPINOR_OP_READ_STATUS2, &status[1]);
    if (error || (status[1] & SPINOR_STATUS2_QE)) return error;
    if (flash->chip->qe == SPINOR_QE_NONVOLATILE) return -RT_ENOSYS;
    if (flash->chip->qe == SPINOR_QE_SR2_BIT1_01H) {
        error = read_register(flash, SPINOR_OP_READ_STATUS, &status[0]);
        if (error) return error;
    }
    status[1] |= SPINOR_STATUS2_QE;
    /* Clear the ordinary write latch; only change the volatile QE value. */
    error = command(flash, SPINOR_OP_WRITE_DISABLE);
    if (!error) error = command(flash, SPINOR_OP_VOLATILE_SR_WRITE_ENABLE);
    if (error) return error;
    if (flash->chip->qe == SPINOR_QE_SR2_BIT1_31H)
        error = write_register(flash, SPINOR_OP_WRITE_STATUS2, &status[1], 1);
    else
        error = write_register(flash, SPINOR_OP_WRITE_STATUS, status, 2);
    if (error) return error;
    spinor_bus_delay_us(1);
    error = read_register(flash, SPINOR_OP_READ_STATUS2, &status[0]);
    if (error) return error;
    return status[0] & SPINOR_STATUS2_QE ? RT_EOK : -RT_EIO;
}

rt_bool_t spinor_core_has_read_lines(const struct spinor_flash *flash, rt_uint8_t lines)
{
    for (rt_size_t i = 0; i < sizeof(read_commands) / sizeof(read_commands[0]); ++i)
        if (read_commands[i].lines == lines)
            return !read_commands[i].flag || (flash->chip->flags & read_commands[i].flag);
    return RT_FALSE;
}

/* Call with the flash lock held, or before the flash is shared. */
static rt_err_t choose_read(struct spinor_flash *flash, rt_uint8_t lines)
{
    rt_size_t i;
    rt_err_t error;
    if (!spinor_core_has_read_lines(flash, lines)) return -RT_ENOSYS;
    for (i = 0; read_commands[i].lines != lines; ++i)
        ;
    if (lines == 4) {
        error = enable_quad(flash);
        if (error) return error;
    }
    error = spinor_bus_set_lines(flash, lines);
    if (error) return error;
    flash->read_opcode = uses_4b_opcodes(flash) ? read_commands[i].opcode_4b : read_commands[i].opcode;
    flash->data_lines = lines;
    return RT_EOK;
}

rt_err_t spinor_core_set_read_lines(struct spinor_flash *flash, rt_uint8_t lines)
{
    rt_err_t error = begin_io(flash);
    if (error) return error;
    error = choose_read(flash, lines);
    rt_mutex_release(&flash->lock);
    return error;
}

/* Choose three-byte addressing, dedicated four-byte opcodes, or four-byte
 * mode. */
static rt_err_t setup_addressing(struct spinor_flash *flash)
{
    const struct spinor_chip *chip = flash->chip;
    rt_uint8_t status;
    rt_err_t error;
    if (chip->capacity <= 0x1000000U) {
        flash->address_bytes = 3;
        return RT_EOK;
    }
    flash->address_bytes = 4;
    if (chip->flags & SPINOR_F_4B_OPCODES) return RT_EOK;
    if (!(chip->flags & SPINOR_F_4B_MODE)) return -RT_ENOSYS;
    if (chip->flags & SPINOR_F_4B_MODE_WREN) {
        error = command(flash, SPINOR_OP_WRITE_ENABLE);
        if (error) return error;
    }
    error = command(flash, SPINOR_OP_ENTER_4B_MODE);
    if (error || !(chip->flags & SPINOR_F_4B_MODE_ADS)) return error;
    error = read_register(flash, SPINOR_OP_READ_STATUS3, &status);
    if (error) return error;
    return status & SPINOR_STATUS3_ADS ? RT_EOK : -RT_EIO;
}

static void setup_commands(struct spinor_flash *flash)
{
    const struct spinor_timing *timing = flash->chip->timing;
    rt_bool_t four_byte = uses_4b_opcodes(flash);
    flash->program_opcode = four_byte ? 0x12 : 0x02;
    flash->erase[0] = (struct spinor_erase_type){SPINOR_SECTOR_SIZE, timing->erase_4k_ms, four_byte ? 0x21 : 0x20};
    flash->erase[1] = (struct spinor_erase_type){SPINOR_BLOCK_SIZE, timing->erase_64k_ms, four_byte ? 0xdc : 0xd8};
}

static const struct spinor_chip *find_chip(const rt_uint8_t identity[3])
{
    for (const struct spinor_chip *chip = spinor_chips; chip->name; ++chip)
        if (rt_memcmp(identity, chip->id, 3) == 0) return chip;
    return RT_NULL;
}

static rt_bool_t chip_valid(const struct spinor_chip *chip)
{
    const struct spinor_timing *timing = chip->timing;
    if (!timing || !timing->program_ms || !timing->erase_4k_ms || !timing->erase_64k_ms) return RT_FALSE;
    if (chip->qe > SPINOR_QE_NONVOLATILE) return RT_FALSE;
    return chip->capacity >= SPINOR_BLOCK_SIZE && chip->capacity % SPINOR_BLOCK_SIZE == 0;
}

rt_err_t spinor_core_open(struct spinor_flash *flash)
{
    rt_uint8_t identity[3];
    rt_err_t error;
    struct spinor_transfer identify = {
        .opcode = SPINOR_OP_READ_ID, .receive_lines = 1, .rx = identity, .length = sizeof(identity),
    };
    error = rt_mutex_init(&flash->lock, "spinor", RT_IPC_FLAG_PRIO);
    if (error) return error;
    /* Return the chip to its power-on state before identifying it; an
     * earlier boot stage may have changed its modes. A chip in QPI, octal or
     * continuous-read mode ignores this single-line reset and then fails
     * identification; no boot stage enables those modes. */
    error = command(flash, SPINOR_OP_RESET_ENABLE);
    if (!error) error = command(flash, SPINOR_OP_RESET);
    if (error) goto failed;
    spinor_bus_delay_us(RESET_US);
    error = ready(flash, RESET_TIMEOUT_MS, 0);
    if (!error) error = spinor_bus_execute(flash, &identify);
    if (error) goto failed;
    flash->jedec_id = (rt_uint32_t)identity[0] << 16 | (rt_uint32_t)identity[1] << 8 | identity[2];
    flash->chip = find_chip(identity);
    if (!flash->chip) {
        rt_kprintf("SPI NOR: unsupported JEDEC ID %06x\n", flash->jedec_id);
        error = -RT_ENOSYS;
        goto failed;
    }
    error = -RT_EINVAL;
    if (!chip_valid(flash->chip)) goto failed;
    error = setup_addressing(flash);
    if (error) goto failed;
    setup_commands(flash);
    error = ready(flash, flash->erase[1].timeout_ms, 0);
    if (!error) error = choose_read(flash, 1);
    if (error) goto failed;
    flash->open = RT_TRUE;
    return RT_EOK;
failed:
    rt_mutex_detach(&flash->lock);
    flash->chip = RT_NULL;
    return error;
}

void spinor_core_close(struct spinor_flash *flash)
{
    if (!flash->open) return;
    /* Let an operation already holding the lock finish first. */
    rt_mutex_take(&flash->lock, RT_WAITING_FOREVER);
    flash->open = RT_FALSE;
    rt_mutex_release(&flash->lock);
    rt_mutex_detach(&flash->lock);
}

void spinor_get_info(const struct spinor_flash *flash, struct spinor_info *info)
{
    if (!flash || !info) return;
    info->name = flash->chip->name;
    info->jedec_id = flash->jedec_id;
    info->capacity = flash->chip->capacity;
    info->page_size = SPINOR_PAGE_SIZE;
    info->erase_size = SPINOR_SECTOR_SIZE;
    info->clock_hz = flash->clock_hz;
    info->read_lines = flash->data_lines;
}

static rt_err_t read_fragment(struct spinor_flash *flash, rt_uint32_t offset, void *buffer, rt_size_t length)
{
    struct spinor_transfer op = {
        .opcode = flash->read_opcode, .address = offset, .address_bytes = flash->address_bytes,
        .dummy_cycles = READ_DUMMY, .receive_lines = flash->data_lines, .rx = buffer, .length = length,
    };
    return spinor_bus_execute(flash, &op);
}

rt_err_t spinor_read(struct spinor_flash *flash, rt_uint32_t offset, void *buffer, rt_size_t length)
{
    rt_err_t error = RT_EOK;
    if (!contains(flash, offset, length) || (!buffer && length)) return -RT_EINVAL;
    if (!length) return RT_EOK;
    error = begin_io(flash);
    if (error) return error;
    for (rt_size_t done = 0; done != length && !error;) {
        rt_size_t count = length - done;
        if (count > SPINOR_READ_LIMIT) count = SPINOR_READ_LIMIT;
        error = read_fragment(flash, offset + done, (rt_uint8_t *)buffer + done, count);
        done += count;
    }
    rt_mutex_release(&flash->lock);
    return error;
}

/* The caller holds the flash mutex throughout command and busy wait. */
static rt_err_t change(struct spinor_flash *flash, struct spinor_transfer *op,
                       rt_uint32_t timeout_ms, rt_uint32_t spin_us)
{
    rt_uint8_t status;
    rt_err_t error = command(flash, SPINOR_OP_WRITE_ENABLE);
    if (error) return error;
    error = read_register(flash, SPINOR_OP_READ_STATUS, &status);
    if (error) return error;
    if (!(status & SPINOR_STATUS_WRITE_LATCH)) return -RT_EIO;
    flash->needs_wait = RT_TRUE;
    error = spinor_bus_execute(flash, op);
    return error ? error : ready(flash, timeout_ms, spin_us);
}

/* Readback catches programs the chip ignored, e.g. in protected areas. */
static rt_err_t verify(struct spinor_flash *flash, rt_uint32_t offset,
                       const rt_uint8_t *expected, rt_size_t length)
{
    rt_uint8_t buffer[SPINOR_WRITE_LIMIT];
    rt_err_t error = read_fragment(flash, offset, buffer, length);
    if (error) return error;
    return rt_memcmp(buffer, expected, length) ? -RT_EIO : RT_EOK;
}

rt_err_t spinor_write(struct spinor_flash *flash, rt_uint32_t offset, const void *buffer, rt_size_t length)
{
    const rt_uint8_t *bytes = buffer;
    rt_err_t error;
    if (!contains(flash, offset, length) || (!buffer && length)) return -RT_EINVAL;
    if (!length) return RT_EOK;
    error = begin_io(flash);
    if (error) return error;
    for (rt_size_t done = 0; done != length && !error;) {
        rt_uint32_t address = offset + done;
        rt_size_t count = SPINOR_PAGE_SIZE - address % SPINOR_PAGE_SIZE;
        if (count > length - done) count = length - done;
        struct spinor_transfer op = {
            .opcode = flash->program_opcode, .address = address,
            .address_bytes = flash->address_bytes, .tx = bytes + done, .length = count,
        };
        error = change(flash, &op, flash->chip->timing->program_ms, PROGRAM_SPIN_US);
        if (!error) error = verify(flash, address, bytes + done, count);
        done += count;
    }
    rt_mutex_release(&flash->lock);
    return error;
}

/* Each step uses a 64 KiB block erase where the range is aligned and long
 * enough, else a 4 KiB sector erase. Erased contents are not read back:
 * JFFS2 checks every erased block. */
rt_err_t spinor_erase(struct spinor_flash *flash, rt_uint32_t offset, rt_size_t length)
{
    rt_err_t error;
    if (!contains(flash, offset, length)) return -RT_EINVAL;
    if (offset % SPINOR_SECTOR_SIZE || length % SPINOR_SECTOR_SIZE) return -RT_EINVAL;
    if (!length) return RT_EOK;
    error = begin_io(flash);
    if (error) return error;
    while (length && !error) {
        const struct spinor_erase_type *erase = &flash->erase[1];
        if (offset % erase->size || length < erase->size) erase = &flash->erase[0];
        struct spinor_transfer op = {
            .opcode = erase->opcode, .address = offset, .address_bytes = flash->address_bytes,
        };
        error = change(flash, &op, erase->timeout_ms, 0);
        offset += erase->size;
        length -= erase->size;
    }
    rt_mutex_release(&flash->lock);
    return error;
}
