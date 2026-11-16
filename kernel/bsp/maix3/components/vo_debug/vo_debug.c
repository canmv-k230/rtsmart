#include <rtthread.h>

#ifdef RT_USING_VO_DEBUG

#include <rthw.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <ioremap.h>
#include <riscv_io.h>
#include "riscv_mmu.h"

#define VO_REG_BASE       0x90840000UL
#define VO_REG_SIZE       0x1000
#define VO_REG_LOAD       0x004
#define VO_DISP_XZONE     0x0c0
#define VO_DISP_YZONE     0x0c4
#define VO_DISP_ENABLE    0x118
#define VO_DISP_SIZE      0x11c
#define VO_BACKGROUND     0x3d0
#define VO_LOAD_READY     ((1U << 0) | (1U << 4))
#define VO_DUMP_CHUNK     (64 * 1024)
#define VO_COMMAND_SIZE   80
#define VO_MAX_ARGS       3
#define VO_ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

enum vo_layer_type {
    VO_VIDEO_ROTATION,
    VO_VIDEO,
    VO_OSD,
};

struct vo_layer {
    const char *name;
    enum vo_layer_type type;
    unsigned int enable_bit;
    unsigned int base;
    unsigned int position;
};

/* Hardware layer names, not the MPP logical layer IDs. Offsets match
 * mpp/kernel/vo/include/{kd_display.h,drv_vo_reg.h}; OSD3..7 are not contiguous
 * with OSD0..2 and have a different register spacing. */
static const struct vo_layer vo_layers[] = {
    { "layer1", VO_VIDEO_ROTATION, 1, 0xa20, 0x0d0 },
    { "layer2", VO_VIDEO,          2, 0x200, 0x0d8 },
    { "layer3", VO_VIDEO,          3, 0x240, 0x0e0 },
    { "osd0",   VO_OSD,            4, 0x280, 0x0e8 },
    { "osd1",   VO_OSD,            5, 0x2c0, 0x0f0 },
    { "osd2",   VO_OSD,            6, 0x300, 0x0f8 },
    { "osd3",   VO_OSD,            7, 0x850, 0x820 },
    { "osd4",   VO_OSD,            8, 0x880, 0x828 },
    { "osd5",   VO_OSD,            9, 0x8b0, 0x830 },
    { "osd6",   VO_OSD,           10, 0x8e0, 0x838 },
    { "osd7",   VO_OSD,           11, 0x910, 0x840 },
};

static const char *const osd_formats[] = {
    "RGB888", "Monochrome8", "RGB565", "ARGB8888", "ARGB4444", "ARGB1555",
};
static const unsigned int osd_bytes_per_pixel[] = { 3, 1, 2, 4, 2, 2 };
static const char *const osd_alpha_modes[] = {
    "fixed", "alpha block", "R channel", "G channel", "B channel", "embedded alpha",
};

struct vo_layer_state {
    rt_uint32_t config;
    rt_uint32_t address[2][2];
    unsigned int width;
    unsigned int height;
    unsigned int stride[2];
    unsigned int x;
    unsigned int y;
    int enabled;
    int dma_enabled;
    int yuv422;
    int gray;
    int format_valid;
};

struct vo_plane {
    rt_uint32_t address;
    size_t size;
    unsigned int row_bytes;
    unsigned int stride;
    unsigned int rows;
};

static rt_uint32_t vo_read(const void *regs, unsigned int offset)
{
    return readl((const volatile rt_uint8_t *)regs + offset);
}

static void vo_write(void *regs, unsigned int offset, rt_uint32_t value)
{
    writel(value, (volatile rt_uint8_t *)regs + offset);
}

static void *vo_map_registers(void)
{
    void *regs = rt_ioremap_nocache((void *)VO_REG_BASE, VO_REG_SIZE);

    if (regs == RT_NULL) {
        rt_kprintf("Cannot map VO registers\n");
    }
    return regs;
}

static const struct vo_layer *vo_find_layer(const char *name)
{
    size_t i;

    for (i = 0; i < VO_ARRAY_SIZE(vo_layers); ++i) {
        if (strcmp(name, vo_layers[i].name) == 0) {
            return &vo_layers[i];
        }
    }
    return RT_NULL;
}

static void vo_read_layer(const void *regs, const struct vo_layer *layer, struct vo_layer_state *state)
{
    unsigned int base = layer->base;
    unsigned int address_offset;
    rt_uint32_t size, stride, control;

    memset(state, 0, sizeof(*state));
    state->enabled = !!(vo_read(regs, VO_DISP_ENABLE) & (1U << layer->enable_bit));
    state->config = vo_read(regs, base);
    state->x = vo_read(regs, layer->position) & 0x1fff;
    state->y = vo_read(regs, layer->position + 4) & 0x1fff;
    state->dma_enabled = 1;
    state->format_valid = 1;

    if (layer->type == VO_VIDEO_ROTATION) {
        control = vo_read(regs, 0x1c0);
        state->dma_enabled = !!(control & 1U) && !!(vo_read(regs, base + 0x1c) & (1U << 16));
        size = vo_read(regs, base + 4);
        stride = vo_read(regs, base + 8);
        state->width = size & 0xfff;
        state->height = (size >> 16) & 0xfff;
        state->stride[0] = stride & 0xffff;
        state->stride[1] = stride >> 16;
        state->yuv422 = !!(state->config & (1U << 8));
        state->gray = !!(state->config & (1U << 10));
        address_offset = base + 0x0c;
    } else if (layer->type == VO_VIDEO) {
        state->dma_enabled = !!(state->config & 1U);
        state->yuv422 = !!(state->config & (1U << 4));
        state->format_valid = state->yuv422 != !!(state->config & (1U << 8));
        size = vo_read(regs, base + 0x20);
        stride = vo_read(regs, base + 0x1c);
        /* Legacy video sizes and eight-byte stride units are stored minus one. */
        state->width = (size & 0x1fff) + 1;
        state->height = ((size >> 16) & 0x1fff) + 1;
        state->stride[0] = ((stride & 0x1fff) + 1) * 8;
        state->stride[1] = state->stride[0];
        address_offset = base + 4;
    } else {
        size = vo_read(regs, base + 4);
        stride = vo_read(regs, base + 0x1c);
        state->width = size & 0xffff;
        state->height = size >> 16;
        state->stride[0] = (stride & 0xffff) * 8;
        state->stride[1] = (stride >> 16) * 8;
        state->format_valid = (state->config & 0xf) < VO_ARRAY_SIZE(osd_formats);
        address_offset = base + 8;
    }

    state->address[0][0] = vo_read(regs, address_offset);
    state->address[0][1] = vo_read(regs, address_offset + 4);
    state->address[1][0] = vo_read(regs, address_offset + 8);
    state->address[1][1] = vo_read(regs, address_offset + 12);
}

static void vo_print_layer(const struct vo_layer *layer, const struct vo_layer_state *state)
{
    unsigned int format = state->config & 0xf;
    unsigned int alpha = (state->config >> 4) & 7;

    rt_kprintf("[%s]: %s\n", layer->name, state->enabled ? "enabled" : "disabled");
    if (!state->enabled) {
        return;
    }

    if (layer->type == VO_OSD) {
        rt_kprintf("format: %s, alpha: %s\n",
                   state->format_valid ? osd_formats[format] : "unknown",
                   alpha < VO_ARRAY_SIZE(osd_alpha_modes) ? osd_alpha_modes[alpha] : "unknown");
    } else {
        rt_kprintf("DMA: %s, format: %s\n", state->dma_enabled ? "enabled" : "disabled",
                   !state->format_valid ? "unknown" : state->gray ? "Y8" : state->yuv422 ? "YUV422" : "YUV420");
        if (layer->type == VO_VIDEO_ROTATION) {
            rt_kprintf("rotate: %u, x mirror: %u, y mirror: %u\n",
                       ((state->config >> 4) & 3) * 90, (state->config >> 6) & 1, (state->config >> 7) & 1);
        }
    }
    rt_kprintf("position (x:y:w:h %u:%u:%u:%u)\n", state->x, state->y, state->width, state->height);
    rt_kprintf("stride (%s %u:%u bytes)\n", layer->type == VO_OSD ? "DATA:ALPHA" : "Y:UV",
               state->stride[0], state->stride[1]);
    rt_kprintf("buffer0 (%s 0x%x:0x%x)\n", layer->type == VO_OSD ? "DATA:ALPHA" : "Y:UV",
               state->address[0][0], state->address[0][1]);
    rt_kprintf("buffer1 (%s 0x%x:0x%x)\n", layer->type == VO_OSD ? "DATA:ALPHA" : "Y:UV",
               state->address[1][0], state->address[1][1]);
}

static int vo_query_status(void)
{
    void *regs = vo_map_registers();
    struct vo_layer_state state;
    rt_uint32_t xzone, yzone, size;
    size_t i;

    if (regs == RT_NULL) {
        return -RT_ENOMEM;
    }
    xzone = vo_read(regs, VO_DISP_XZONE);
    yzone = vo_read(regs, VO_DISP_YZONE);
    size = vo_read(regs, VO_DISP_SIZE);
    rt_kprintf("============= vo_status =========================\n");
    rt_kprintf("VO_DISP_ZONE(x: %u~%u, y: %u~%u)\n", xzone & 0x1fff, (xzone >> 16) & 0x1fff,
               yzone & 0x1fff, (yzone >> 16) & 0x1fff);
    rt_kprintf("VO_DISP_SIZE(h: %u, v: %u)\n", size & 0x1fff, (size >> 16) & 0x1fff);
    rt_kprintf("VO_DISP_BACKGROUND(YUV: 0x%x)\n", vo_read(regs, VO_BACKGROUND) & 0xffffff);
    for (i = 0; i < VO_ARRAY_SIZE(vo_layers); ++i) {
        vo_read_layer(regs, &vo_layers[i], &state);
        vo_print_layer(&vo_layers[i], &state);
    }
    rt_iounmap(regs);
    return RT_EOK;
}

static int vo_get_planes(const struct vo_layer *layer, const struct vo_layer_state *state,
                         struct vo_plane planes[2], unsigned int *count)
{
    unsigned int row_bytes = state->width;
    unsigned int rows[2] = { state->height, 0 };
    unsigned int i;

    if (!state->enabled || !state->dma_enabled) {
        rt_kprintf("%s is not enabled\n", layer->name);
        return -RT_EINVAL;
    }
    if (!state->format_valid || !state->width || !state->height) {
        rt_kprintf("%s has an invalid format or size\n", layer->name);
        return -RT_EINVAL;
    }
    *count = 1;
    if (layer->type == VO_OSD) {
        unsigned int alpha = (state->config >> 4) & 7;

        if (alpha >= VO_ARRAY_SIZE(osd_alpha_modes)) {
            return -RT_EINVAL;
        }
        row_bytes *= osd_bytes_per_pixel[state->config & 0xf];
        if (alpha == 1) {
            rows[1] = state->height;
            *count = 2;
        }
    } else if (!state->gray) {
        if ((state->width & 1) || (!state->yuv422 && (state->height & 1))) {
            rt_kprintf("%s has an invalid YUV size\n", layer->name);
            return -RT_EINVAL;
        }
        rows[1] = state->yuv422 ? state->height : state->height / 2;
        *count = 2;
    }

    for (i = 0; i < *count; ++i) {
        unsigned int min_stride = i == 0 ? row_bytes : state->width;
        rt_uint64_t span = (rt_uint64_t)state->stride[i] * (rows[i] - 1) + min_stride;

        planes[i].address = state->address[0][i];
        /* VO buffer addresses are 32-bit physical addresses. Check before
         * converting sizes or mapping, including the end of the last plane. */
        if (state->stride[i] < min_stride || !planes[i].address ||
            (rt_uint64_t)planes[i].address + span > (1ULL << 32)) {
            rt_kprintf("%s has an invalid buffer address or stride\n", layer->name);
            return -RT_EINVAL;
        }
        planes[i].row_bytes = min_stride;
        planes[i].stride = state->stride[i];
        planes[i].rows = rows[i];
        planes[i].size = (size_t)min_stride * rows[i];
    }
    return RT_EOK;
}

static int vo_write_span(FILE *file, rt_ubase_t address, size_t remaining)
{
    while (remaining) {
        size_t length = remaining > VO_DUMP_CHUNK ? VO_DUMP_CHUNK : remaining;
        size_t offset = address & (PAGE_SIZE - 1);
        /* Frame buffers are RAM, so use normal uncached memory, not MMIO
         * attributes. Map bounded, page-aligned spans and unmap the same base. */
        void *mapping = rt_ioremap_wt((void *)(address - offset), RT_ALIGN(offset + length, PAGE_SIZE));
        size_t written;

        if (mapping == RT_NULL) {
            rt_kprintf("Cannot map VO buffer at 0x%lx\n", (unsigned long)address);
            return -RT_ENOMEM;
        }
        written = fwrite((const rt_uint8_t *)mapping + offset, 1, length, file);
        rt_iounmap(mapping);
        if (written != length) {
            rt_kprintf("Failed to write VO buffer\n");
            return -RT_ERROR;
        }
        address += length;
        remaining -= length;
    }
    return RT_EOK;
}

static int vo_write_plane(FILE *file, const struct vo_plane *plane)
{
    unsigned int row;
    int ret;

    if (plane->stride == plane->row_bytes) {
        return vo_write_span(file, plane->address, plane->size);
    }
    /* Strip row padding and avoid reading past the active part of the last
     * row: a valid allocation need not include trailing stride padding. */
    for (row = 0; row < plane->rows; ++row) {
        ret = vo_write_span(file, (rt_ubase_t)plane->address + (size_t)row * plane->stride, plane->row_bytes);
        if (ret != RT_EOK) {
            return ret;
        }
    }
    return RT_EOK;
}

static int vo_dump_buffer(const struct vo_layer *layer)
{
    struct vo_layer_state state;
    struct vo_plane planes[2];
    unsigned int count, i;
    void *regs = vo_map_registers();
    char filename[32];
    FILE *file;
    int ret;
    rt_base_t level;

    if (regs == RT_NULL) {
        return -RT_ENOMEM;
    }
    level = rt_hw_interrupt_disable();
    vo_read_layer(regs, layer, &state);
    rt_hw_interrupt_enable(level);
    rt_iounmap(regs);
    ret = vo_get_planes(layer, &state, planes, &count);
    if (ret != RT_EOK) {
        return ret;
    }
    snprintf(filename, sizeof(filename), "./dump_%s.bin", layer->name);
    file = fopen(filename, "wb");
    if (file == RT_NULL) {
        rt_kprintf("Cannot open %s\n", filename);
        return -RT_ERROR;
    }
    for (i = 0; i < count; ++i) {
        rt_kprintf("buffer0 plane%u: address=0x%x, size=%lu\n", i, planes[i].address,
                   (unsigned long)planes[i].size);
        ret = vo_write_plane(file, &planes[i]);
        if (ret != RT_EOK) {
            break;
        }
    }
    if (fclose(file) != 0) {
        rt_kprintf("Failed to close %s\n", filename);
        ret = -RT_ERROR;
    }
    if (ret != RT_EOK) {
        remove(filename);
        return ret;
    }
    rt_kprintf("Saved buffer0 to %s\n", filename);
    return RT_EOK;
}

static int vo_set_layer(const struct vo_layer *layer, int enable)
{
    void *regs = vo_map_registers();
    rt_uint32_t value, mask = 1U << layer->enable_bit;
    rt_base_t level;

    if (regs == RT_NULL) {
        return -RT_ENOMEM;
    }
    /* Keep the read/modify/write and load together against the VO ISR on this
     * core. This command changes the mixer bit only, not the MPP driver state. */
    level = rt_hw_interrupt_disable();
    value = vo_read(regs, VO_DISP_ENABLE);
    if (!!(value & mask) != enable) {
        vo_write(regs, VO_DISP_ENABLE, enable ? value | mask : value & ~mask);
        vo_write(regs, VO_REG_LOAD, vo_read(regs, VO_REG_LOAD) | VO_LOAD_READY);
    }
    rt_hw_interrupt_enable(level);
    rt_iounmap(regs);
    rt_kprintf("%s: %s\n", layer->name, enable ? "enabled" : "disabled");
    return RT_EOK;
}

static void vo_print_help(void)
{
    rt_kprintf("vo_debug [q | set <layer> <0|1> | dump <layer> | help]\n");
    rt_kprintf("  Layers: layer1~3, osd0~7 (hardware numbering)\n");
    rt_kprintf("  q                  Query VO registers\n");
    rt_kprintf("  set <layer> <0|1>  Change the mixer enable bit\n");
    rt_kprintf("  dump <layer>       Save buffer0 planes to ./dump_<layer>.bin\n");
    rt_kprintf("  exit               Leave interactive mode\n");
}

static int vo_handle_command(int argc, char **argv)
{
    const struct vo_layer *layer;

    if (argc == 1) {
        if (strcmp(argv[0], "q") == 0) {
            return vo_query_status();
        }
        if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "-h") == 0) {
            vo_print_help();
            return RT_EOK;
        }
    }
    if ((argc == 2 && strcmp(argv[0], "dump") == 0) ||
        (argc == 3 && strcmp(argv[0], "set") == 0)) {
        layer = vo_find_layer(argv[1]);
        if (layer == RT_NULL) {
            rt_kprintf("Unknown layer: %s\n", argv[1]);
            return -RT_EINVAL;
        }
        if (argc == 2) {
            return vo_dump_buffer(layer);
        }
        if (strcmp(argv[2], "0") == 0 || strcmp(argv[2], "1") == 0) {
            return vo_set_layer(layer, argv[2][0] == '1');
        }
    }
    vo_print_help();
    return -RT_EINVAL;
}

static int vo_split_command(char *command, char **argv)
{
    int argc = 0;

    while (*command) {
        while (isspace((unsigned char)*command)) {
            ++command;
        }
        if (!*command) {
            break;
        }
        if (argc == VO_MAX_ARGS) {
            return -RT_EINVAL;
        }
        argv[argc++] = command;
        while (*command && !isspace((unsigned char)*command)) {
            ++command;
        }
        if (*command) {
            *command++ = '\0';
        }
    }
    return argc;
}

int vo_debug(int argc, char **argv)
{
    char command[VO_COMMAND_SIZE];
    char *args[VO_MAX_ARGS];
    int count, ch;

    if (argc > 1) {
        return vo_handle_command(argc - 1, argv + 1);
    }
    vo_print_help();
    while (1) {
        rt_kprintf("vo> ");
        if (fgets(command, sizeof(command), stdin) == RT_NULL) {
            return ferror(stdin) ? -RT_ERROR : RT_EOK;
        }
        if (strchr(command, '\n') == RT_NULL && strlen(command) == sizeof(command) - 1) {
            while ((ch = getchar()) != '\n' && ch != EOF) {
            }
            rt_kprintf("Command too long\n");
            continue;
        }
        count = vo_split_command(command, args);
        if (count == 0) {
            continue;
        }
        if (count < 0) {
            vo_print_help();
            continue;
        }
        if (count == 1 && strcmp(args[0], "exit") == 0) {
            return RT_EOK;
        }
        vo_handle_command(count, args);
    }
}
MSH_CMD_EXPORT(vo_debug, Debug VO registers and dump layer buffers);
MSH_CMD_EXPORT_ALIAS(vo_debug, canmv_vo_debug, Alias for vo_debug);

#endif /* RT_USING_VO_DEBUG */
