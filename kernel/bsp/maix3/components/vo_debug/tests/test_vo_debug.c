/* Host regression tests: register access and file/mapping failures are mocked. */
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t rt_uint8_t;
typedef uint32_t rt_uint32_t;
typedef uint64_t rt_uint64_t;
typedef uintptr_t rt_ubase_t;
typedef intptr_t rt_base_t;
#define RT_NULL NULL
#define RT_EOK 0
#define RT_ERROR 1
#define RT_ENOMEM 12
#define RT_EINVAL 22
#define PAGE_SIZE 4096UL
#define RT_ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define MSH_CMD_EXPORT(command, desc) static int (*export_##command)(int, char **) = command
/* The alias points to the same function, with its own shell symbol. */
#define MSH_CMD_EXPORT_ALIAS(command, alias, desc) static int (*export_##alias)(int, char **) = command

static uint32_t registers[0x1000 / 4];
static unsigned char ram[2][256 * 1024];
static const uintptr_t ram_base[] = { 0x10000000, 0x20000000 };
static unsigned char output[512 * 1024];
static char log_text[16384];
static size_t output_size, log_size;
static int reg_maps, ram_maps, unmaps, writes, irq_disabled;
static int fail_reg_map, fail_ram_map, fail_open, fail_write, fail_close, removed;

static int rt_kprintf(const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(log_text + log_size, sizeof(log_text) - log_size, format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(log_text) - log_size);
    log_size += length;
    return length;
}

static void *rt_ioremap_nocache(void *address, size_t size)
{
    ++reg_maps;
    assert((uintptr_t)address == 0x90840000 && size == sizeof(registers));
    return fail_reg_map ? NULL : registers;
}

static void *rt_ioremap_wt(void *address, size_t size)
{
    uintptr_t physical = (uintptr_t)address;
    unsigned int i;

    ++ram_maps;
    assert(physical % PAGE_SIZE == 0 && size % PAGE_SIZE == 0);
    if (fail_ram_map == ram_maps) {
        return NULL;
    }
    for (i = 0; i < 2; ++i) {
        if (physical >= ram_base[i] && physical + size <= ram_base[i] + sizeof(ram[i])) {
            return ram[i] + physical - ram_base[i];
        }
    }
    assert(!"Unexpected physical buffer mapping");
    return NULL;
}

static void rt_iounmap(volatile void *address)
{
    assert(address != NULL);
    ++unmaps;
}

static uint32_t readl(const volatile void *address)
{
    uintptr_t offset = (uintptr_t)address - (uintptr_t)registers;
    assert(offset < sizeof(registers) && offset % 4 == 0);
    return registers[offset / 4];
}

static void writel(uint32_t value, volatile void *address)
{
    uintptr_t offset = (uintptr_t)address - (uintptr_t)registers;
    assert(irq_disabled && offset < sizeof(registers) && offset % 4 == 0);
    ++writes;
    registers[offset / 4] = value;
}

static rt_base_t rt_hw_interrupt_disable(void)
{
    assert(!irq_disabled);
    irq_disabled = 1;
    return 0;
}

static void rt_hw_interrupt_enable(rt_base_t level)
{
    assert(level == 0 && irq_disabled);
    irq_disabled = 0;
}

static FILE *test_fopen(const char *name, const char *mode)
{
    assert(strncmp(name, "./dump_", 7) == 0 && strstr(name, ".bin"));
    assert(strcmp(mode, "wb") == 0);
    return fail_open ? NULL : tmpfile();
}

static size_t test_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    assert(file && size == 1 && output_size + count <= sizeof(output));
    if (fail_write) {
        return count - 1;
    }
    memcpy(output + output_size, data, count);
    output_size += count;
    return count;
}

static int test_fclose(FILE *file)
{
    int ret = fclose(file);
    assert(ret == 0);
    return fail_close ? EOF : ret;
}

static int test_remove(const char *filename)
{
    assert(strncmp(filename, "./dump_", 7) == 0);
    ++removed;
    return 0;
}

#define fopen test_fopen
#define fwrite test_fwrite
#define fclose test_fclose
#define remove test_remove
#include "../vo_debug.c"
#undef fopen
#undef fwrite
#undef fclose
#undef remove

#define REG(offset) registers[(offset) / 4]

static void reset(void)
{
    memset(registers, 0, sizeof(registers));
    memset(log_text, 0, sizeof(log_text));
    memset(ram[0], 0x59, sizeof(ram[0]));
    memset(ram[1], 0xa3, sizeof(ram[1]));
    output_size = log_size = 0;
    reg_maps = ram_maps = unmaps = writes = irq_disabled = 0;
    fail_reg_map = fail_ram_map = fail_open = fail_write = fail_close = removed = 0;
}

static int command(const char *text)
{
    char line[256];
    char *args[VO_MAX_ARGS];
    int count;

    assert(strlen(text) < sizeof(line));
    strcpy(line, text);
    count = vo_split_command(line, args);
    return count < 0 ? count : vo_handle_command(count, args);
}

static void setup_video2(void)
{
    REG(0x118) = 1U << 2;
    REG(0x200) = 1U | (1U << 8);
    REG(0x220) = (3U << 16) | 15U; /* 16 x 4 */
    REG(0x21c) = (3U << 16) | 2U; /* 24-byte padded stride */
    REG(0x214) = 0xdeadbeef;       /* Not a size register. */
    REG(0x204) = ram_base[0] + 3;
    REG(0x208) = ram_base[1] + 5;  /* UV is not contiguous with Y. */
}

static void test_commands_and_register_writes(void)
{
    const char *bad[] = { "query", "dump", "dump osd", "dump xosd0", "dump osd8",
        "dump osd0junk", "dump layer0", "set osd4 -1", "set osd4 2", "set osd4 1junk",
        "set invalid 1", "set osd0 1 extra", "q extra", "" };
    unsigned int i;
    char *args[] = { "vo_debug", "set", "osd7", "1" };

    reset();
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(command(bad[i]) == -RT_EINVAL);
    }
    assert(reg_maps == 0 && writes == 0);
    REG(0x118) = 0xa501005f;
    REG(0x004) = 0x80;
    assert(command(" \tset osd4 1\r\n") == 0);
    assert(REG(0x118) == 0xa501015f && REG(0x004) == 0x91 && writes == 2);
    assert(command("set osd4 1") == 0 && writes == 2);
    assert(command("set osd4 0") == 0 && REG(0x118) == 0xa501005f);
    assert(vo_debug(4, args) == 0 && (REG(0x118) & (1U << 11)));
    assert(export_vo_debug == vo_debug && export_canmv_vo_debug == vo_debug);
    assert(reg_maps == unmaps && !irq_disabled);
    fail_reg_map = 1;
    assert(command("set osd4 1") == -RT_ENOMEM);
    assert(command("q") == -RT_ENOMEM);
    assert(command("dump osd0") == -RT_ENOMEM);
}

static void test_register_decoding(void)
{
    struct vo_layer_state state;
    struct vo_plane planes[2];
    unsigned int count, i;
    const unsigned int osd_base[] = { 0x280, 0x2c0, 0x300, 0x850, 0x880, 0x8b0, 0x8e0, 0x910 };
    const unsigned int osd_pos[] = { 0xe8, 0xf0, 0xf8, 0x820, 0x828, 0x830, 0x838, 0x840 };

    reset();
    for (i = 0; i < 8; ++i) {
        unsigned int base = osd_base[i], pos = osd_pos[i];
        char name[8];
        snprintf(name, sizeof(name), "osd%u", i);
        REG(0x118) |= 1U << (i + 4);
        REG(base) = 0x53; /* ARGB8888, embedded alpha */
        REG(base + 4) = (4U << 16) | 16U;
        REG(base + 0x1c) = (8U << 16) | 8U;
        REG(base + 8) = ram_base[0];
        REG(pos) = (100U << 16) | (10U + i);
        REG(pos + 4) = (200U << 16) | (20U + i);
        vo_read_layer(registers, vo_find_layer(name), &state);
        assert(state.x == 10 + i && state.y == 20 + i);
        assert(state.width == 16 && state.height == 4 && state.stride[0] == 64);
        assert(vo_get_planes(vo_find_layer(name), &state, planes, &count) == 0);
        assert(count == 1 && planes[0].size == 256);
    }
    assert(command("q") == 0);
    assert(strstr(log_text, "format: ARGB8888, alpha: embedded alpha"));
    assert(strstr(log_text, "[osd7]: enabled"));
    REG(0x118) |= 1U << 1;
    REG(0x1c0) = 1;
    REG(0xa20) = 0; /* Config bit 0 is outstanding count, not an enable bit. */
    REG(0xa3c) = 1U << 16;
    REG(0xa24) = (4U << 16) | 16U;
    REG(0xa28) = (24U << 16) | 24U;
    REG(0xa2c) = ram_base[0];
    REG(0xa30) = ram_base[1];
    vo_read_layer(registers, vo_find_layer("layer1"), &state);
    assert(state.enabled && state.dma_enabled);
    assert(vo_get_planes(vo_find_layer("layer1"), &state, planes, &count) == 0);
    assert(count == 2 && planes[0].size == 64 && planes[1].size == 32);
    REG(0xa3c) = 0;
    vo_read_layer(registers, vo_find_layer("layer1"), &state);
    assert(!state.dma_enabled);

    REG(0xa3c) = 1U << 16;
    REG(0xa20) = 1U << 8;
    vo_read_layer(registers, vo_find_layer("layer1"), &state);
    assert(vo_get_planes(vo_find_layer("layer1"), &state, planes, &count) == 0);
    assert(count == 2 && planes[1].size == 64); /* YUV422 has full-height UV. */
    REG(0xa20) = 1U << 10;
    REG(0xa30) = 0;
    vo_read_layer(registers, vo_find_layer("layer1"), &state);
    assert(vo_get_planes(vo_find_layer("layer1"), &state, planes, &count) == 0);
    assert(count == 1); /* Gray does not require a UV buffer. */

    REG(0x118) |= 1U << 3;
    REG(0x240) = 1U | (1U << 8);
    REG(0x260) = (7U << 16) | 31U;
    REG(0x25c) = (7U << 16) | 3U;
    REG(0x244) = ram_base[0];
    REG(0x248) = ram_base[1];
    vo_read_layer(registers, vo_find_layer("layer3"), &state);
    assert(state.width == 32 && state.height == 8 && state.stride[0] == 32);
    assert(vo_get_planes(vo_find_layer("layer3"), &state, planes, &count) == 0);
    assert(count == 2 && planes[0].size == 256 && planes[1].size == 128);

    REG(0x910) = 0x13; /* OSD7 with separate 8-bit alpha plane. */
    REG(0x91c) = ram_base[1];
    REG(0x92c) = (2U << 16) | 8U;
    vo_read_layer(registers, vo_find_layer("osd7"), &state);
    assert(vo_get_planes(vo_find_layer("osd7"), &state, planes, &count) == 0);
    assert(count == 2 && planes[0].size == 256 && planes[1].size == 64);
}

static void test_dump_and_errors(void)
{
    struct vo_layer_state state;
    struct vo_plane planes[2];
    const struct vo_layer *layer = vo_find_layer("layer2");
    unsigned int count;

    reset();
    setup_video2();
    memset(ram, 0xee, sizeof(ram));
    for (unsigned int row = 0; row < 4; ++row) {
        memset(ram[0] + 3 + row * 24, 0x59 + row, 16);
    }
    for (unsigned int row = 0; row < 2; ++row) {
        memset(ram[1] + 5 + row * 24, 0xa3 + row, 16);
    }
    assert(command("dump layer2") == 0);
    assert(output_size == 96 && ram_maps == 6 && unmaps == 7);
    assert(output[0] == 0x59 && output[16] == 0x5a && output[63] == 0x5c);
    assert(output[64] == 0xa3 && output[95] == 0xa4);
    vo_read_layer(registers, layer, &state);
    state.address[0][0] = 0xfffffff0;
    assert(vo_get_planes(layer, &state, planes, &count) == -RT_EINVAL);
    state.address[0][0] = ram_base[0];
    state.stride[0] = 8;
    assert(vo_get_planes(layer, &state, planes, &count) == -RT_EINVAL);
    REG(0x118) = 1U << 3; /* A later layer must not make layer2 appear enabled. */
    assert(command("dump layer2") == -RT_EINVAL);
    REG(0x118) = 1U << 2;
    REG(0x200) |= 1U << 4; /* Mutually exclusive legacy format bits. */
    assert(command("dump layer2") == -RT_EINVAL);

    reset(); setup_video2(); fail_open = 1;
    assert(command("dump layer2") == -RT_ERROR && ram_maps == 0);
    reset(); setup_video2(); fail_ram_map = 2;
    assert(command("dump layer2") == -RT_ENOMEM && removed == 1 && unmaps == 2);
    reset(); setup_video2(); fail_write = 1;
    assert(command("dump layer2") == -RT_ERROR && removed == 1 && unmaps == 2);
    reset(); setup_video2(); fail_close = 1;
    assert(command("dump layer2") == -RT_ERROR && removed == 1);

    reset(); setup_video2();
    REG(0x220) = (1023U << 16) | 127U;
    REG(0x21c) = (1023U << 16) | 15U;
    assert(command("dump layer2") == 0);
    assert(output_size == 128 * 1024 * 3 / 2 && ram_maps == 3 && unmaps == 4);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        char *args[] = { "vo_debug" };
        reset();
        assert(strcmp(argv[1], "--interactive") == 0);
        assert(vo_debug(1, args) == 0);
        assert(reg_maps == 0); /* Malformed/truncated input must not touch VO. */
        puts("Interactive input and EOF tests passed");
        return 0;
    }
    test_commands_and_register_writes();
    test_register_decoding();
    test_dump_and_errors();
    puts("VO debug host regression tests passed");
    return 0;
}
