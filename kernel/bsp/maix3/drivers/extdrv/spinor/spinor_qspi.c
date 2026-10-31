/* RT-Thread QSPI binding: transfers, bus configuration, attach/detach and
 * read-mode tuning. */
#include "spinor_core.h"
#include "tick.h"

/* Tuning compares samples spread evenly over the whole chip, including
 * addresses above 16 MiB. */
#define TUNE_SAMPLES 32
#define TUNE_SAMPLE_SIZE 256

rt_err_t spinor_bus_execute(struct spinor_flash *flash, const struct spinor_transfer *op)
{
    struct rt_qspi_message message = {0};
    rt_uint8_t header[1 + 4];
    rt_size_t expected = op->length;
    if (op->address_bytes > 4 || (op->tx && op->rx) || (op->length && !op->tx && !op->rx))
        return -RT_EINVAL;
    message.parent.cs_take = 1;
    message.parent.cs_release = 1;
    message.qspi_data_lines = 1;
    if (!op->length) {
        /* The controller reports an empty data phase as a zero-length (failed)
         * transfer, so send the command and address as the data phase. */
        if (op->dummy_cycles) return -RT_EINVAL;
        header[0] = op->opcode;
        for (rt_uint8_t i = 0; i < op->address_bytes; ++i)
            header[i + 1] = op->address >> (8 * (op->address_bytes - i - 1));
        message.parent.send_buf = header;
        expected = 1 + op->address_bytes;
    } else {
        message.instruction.content = op->opcode;
        message.instruction.size = 8;
        message.instruction.qspi_lines = 1;
        message.address.content = op->address;
        message.address.size = op->address_bytes * 8;
        message.address.qspi_lines = op->address_bytes ? 1 : 0;
        message.dummy_cycles = op->dummy_cycles;
        if (op->rx) {
            if (op->length > SPINOR_READ_LIMIT ||
                (op->receive_lines != 1 && op->receive_lines != 2 && op->receive_lines != 4 &&
                 op->receive_lines != 8))
                return -RT_EINVAL;
            message.qspi_data_lines = op->receive_lines;
            message.parent.recv_buf = op->rx;
        } else {
            if (op->length > SPINOR_WRITE_LIMIT || op->dummy_cycles) return -RT_EINVAL;
            message.parent.send_buf = op->tx;
        }
    }
    message.parent.length = expected;
    rt_set_errno(RT_EOK);
    if (rt_qspi_transfer_message(&flash->device, &message) == expected) return RT_EOK;
    int error = rt_get_errno();
    return error < 0 ? error : -RT_EIO;
}

rt_err_t spinor_bus_set_lines(struct spinor_flash *flash, rt_uint8_t lines)
{
    struct rt_qspi_configuration configuration = flash->device.config;
    configuration.qspi_dl_width = lines;
    configuration.medium_size = flash->chip ? flash->chip->capacity : 0;
    return rt_qspi_configure(&flash->device, &configuration);
}

static rt_err_t set_clock(struct spinor_flash *flash, rt_uint32_t hz)
{
    struct rt_qspi_configuration configuration = flash->device.config;
    rt_err_t error;
    configuration.parent.max_hz = hz;
    error = rt_qspi_configure(&flash->device, &configuration);
    if (!error) flash->clock_hz = hz;
    return error;
}

void spinor_bus_delay_us(rt_uint32_t us)
{
    cpu_ticks_delay_us(us);
}

/* FNV-1a; samples are compared, not stored. */
static rt_uint32_t hash(const rt_uint8_t *data, rt_size_t length)
{
    rt_uint32_t value = 2166136261U;
    while (length--) value = (value ^ *data++) * 16777619U;
    return value;
}

static rt_err_t sample(struct spinor_flash *flash, rt_uint32_t hashes[TUNE_SAMPLES])
{
    rt_uint8_t buffer[TUNE_SAMPLE_SIZE];
    rt_uint32_t stride = flash->chip->capacity / TUNE_SAMPLES;
    for (rt_size_t i = 0; i < TUNE_SAMPLES; ++i) {
        rt_err_t error = spinor_read(flash, i * stride, buffer, sizeof(buffer));
        if (error) return error;
        hashes[i] = hash(buffer, sizeof(buffer));
    }
    return RT_EOK;
}

/* Use the widest read mode the chip and wiring allow at max_hz whose reads
 * match single-line reads at probe_hz; otherwise keep the probe settings. */
static rt_err_t tune(struct spinor_flash *flash, const struct spinor_config *config)
{
    static const rt_uint8_t widths[] = {8, 4, 2, 1};
    rt_uint32_t reference[TUNE_SAMPLES], check[TUNE_SAMPLES];
    rt_err_t error = sample(flash, reference);
    if (error) return error;
    for (rt_size_t i = 0; i < sizeof(widths); ++i) {
        rt_uint8_t lines = widths[i];
        if (lines > config->max_lines || !spinor_core_has_read_lines(flash, lines)) continue;
        if (lines == 1 && config->max_hz == config->probe_hz) return RT_EOK;
        if (spinor_core_set_read_lines(flash, lines) == RT_EOK && set_clock(flash, config->max_hz) == RT_EOK &&
            sample(flash, check) == RT_EOK && rt_memcmp(reference, check, sizeof(check)) == 0)
            return RT_EOK;
        rt_kprintf("SPI NOR: %u-line reads at %u Hz failed verification\n", lines, config->max_hz);
        error = set_clock(flash, config->probe_hz);
        if (!error) error = spinor_core_set_read_lines(flash, 1);
        if (error) return error;
    }
    return RT_EOK;
}

struct spinor_flash *spinor_attach(const struct spinor_config *config)
{
    struct rt_qspi_configuration configuration = {0};
    struct spinor_flash *flash;
    rt_err_t error;
    if (!config || !config->bus || !config->device || !config->probe_hz ||
        config->max_hz < config->probe_hz ||
        (config->max_lines != 1 && config->max_lines != 2 && config->max_lines != 4 && config->max_lines != 8))
        return RT_NULL;
    flash = rt_calloc(1, sizeof(*flash));
    if (!flash) return RT_NULL;
    error = rt_spi_bus_attach_device(&flash->device.parent, config->device, config->bus, RT_NULL);
    if (error) {
        rt_free(flash);
        return RT_NULL;
    }
    configuration.parent.mode = RT_SPI_MODE_0 | RT_SPI_MSB;
    configuration.parent.hard_cs = config->cs;
    configuration.parent.data_width = 8;
    configuration.parent.max_hz = config->probe_hz;
    configuration.qspi_dl_width = 1;
    error = rt_qspi_configure(&flash->device, &configuration);
    if (!error) {
        flash->clock_hz = config->probe_hz;
        error = spinor_core_open(flash);
    }
    if (!error) {
        error = tune(flash, config);
        if (error) spinor_core_close(flash);
    }
    if (error) {
        rt_kprintf("SPI NOR: attach to %s failed (%d)\n", config->bus, error);
        rt_device_unregister(&flash->device.parent.parent);
        rt_free(flash);
        return RT_NULL;
    }
    rt_kprintf("SPI NOR: %s, %u MiB, %u-line reads at up to %u MHz\n", flash->chip->name,
               flash->chip->capacity >> 20, flash->data_lines, flash->clock_hz / 1000000);
    return flash;
}

void spinor_detach(struct spinor_flash *flash)
{
    if (!flash) return;
#ifdef RT_USING_MTD_NOR
    spinor_mtd_remove_partitions(flash);
#endif
    spinor_core_close(flash);
    rt_device_unregister(&flash->device.parent.parent);
    rt_free(flash);
}
