/* Copyright (c) 2025, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <rtthread.h>
#include <rthw.h>
#include <rtdevice.h>
#include <dfs_fs.h>

#include "sysctl_boot.h"

#include <drivers/mmcsd_core.h>
#include <drivers/mtd_nand.h>

#include "sha256_ota.h"

#define DBG_TAG   "ota"
#define DBG_LVL   DBG_INFO
#include <rtdbg.h>

/*
 * TOC 布局（需与 U-Boot 以及 tools/genimage_py/lib/toc.py 保持一致）
 *  - 固定放在启动介质偏移 0x000e0000 处
 *  - 每个 entry 64 字节，总共最多 16 个 entry
 */
#define K230_TOC_OFFSET        0x000e0000
#define K230_TOC_MAX_ENTRIES   16
#define K230_TOC_ENTRY_SIZE    64
/*
 * OTA 槽位版本元数据：
 *  - 为每个槽位（A/B）单独保留一个 512B 块，互不影响
 *  - boot/ota 通过 magic + crc32 校验版本块是否有效
 */
#define OTA_META_MAGIC         0x4f544156u  /* 'OTAV' */

struct ota_partition {
    char        name[32];
    rt_uint64_t offset;
    rt_uint64_t size;
    rt_uint8_t  load;
    rt_uint8_t  boot;
    rt_uint8_t  reserved[14];	/* 对齐到 64 字节，内容目前未使用 */
} __attribute__((packed, aligned(64)));

struct ota_toc {
    rt_uint32_t       entry_count;
    struct ota_partition entries[K230_TOC_MAX_ENTRIES];
};

/*
 * kdimage 头部与分区表格式（需与 tools/genimage_py/image_kd.py /
 * tools/genimage/image-kd.c 保持一致）
 */
#define KDIMG_HDR_MAGIC          0x27CB8F93u
#define KDIMG_PART_MAGIC         0x91DF6DA4u
#define KDIMG_CONTENT_START_OFF  (64u * 1024u)
#define KDIMG_HDR_SIZE           512u

/* 与 tools/genimage_py/image_kd.py 中 fmt '<IIIIIIQII32s32s' 对齐 */
struct kd_img_part {
    rt_uint32_t part_magic;
    rt_uint32_t part_offset;         /* 对齐到 4096 的目标地址（字节） */
    rt_uint32_t part_size;           /* 对齐到 4096 的整体大小 */
    rt_uint32_t part_erase_size;
    rt_uint32_t part_max_size;
    rt_uint32_t reserved;            /* 预留字段 */
    rt_uint64_t part_flag;

    rt_uint32_t part_content_offset; /* 内容在 kdimg 中的偏移 */
    rt_uint32_t part_content_size;   /* 实际内容长度 */
    rt_uint8_t  part_content_sha256[32];

    char        part_name[32];
} __attribute__((packed));

#define KDIMG_PART_ENTRY_SIZE    256u

struct kd_img_hdr {
    rt_uint32_t img_hdr_magic;
    rt_uint32_t img_hdr_crc32;
    rt_uint32_t img_hdr_flag;
    rt_uint32_t img_hdr_version;

    rt_uint32_t part_tbl_num;
    rt_uint32_t part_tbl_crc32;

    char        image_info[32];
    char        chip_info[32];
    char        board_info[64];
} __attribute__((aligned(512)));

struct ota_slot_meta {
    rt_uint32_t magic;
    rt_uint32_t version;
    rt_uint32_t crc32;
    rt_uint32_t reserved;
    rt_uint8_t  padding[512u - 16];
} __attribute__((packed));

/* 统一的 OTA 存储操作函数原型，供其它内核模块使用 */
rt_err_t ota_storage_init(void);
rt_err_t ota_storage_erase(rt_uint64_t offset, rt_uint64_t size);
rt_err_t ota_storage_write(rt_uint64_t offset, const void *buf, rt_size_t len);
rt_err_t ota_storage_read(rt_uint64_t offset, void *buf, rt_size_t len);

struct ota_ioctl_erase_arg {
    rt_uint64_t offset;	/* 以字节为单位 */
    rt_uint64_t size;	/* 以字节为单位 */
};

static sysctl_boot_mode_e g_boot_mode = SYSCTL_BOOT_MAX;
static rt_device_t g_mmc_dev = RT_NULL;
static struct ota_toc g_ota_toc;
static rt_bool_t g_ota_toc_loaded;

struct ota_kd_ctx {
    rt_bool_t probing;        /* 正在探测 kdimg 头/分区表 */
    rt_bool_t header_done;    /* header 已完成并通过 CRC 检查 */
    rt_bool_t table_done;     /* 分区表已完成并通过 CRC 检查 */
    rt_bool_t is_kdimg;       /* 已确认是合法 kdimage */
    rt_bool_t invalid;        /* 确认不是 kdimg 或解析失败，后续写入全部失败 */

    /* A/B 分区选择与版本管理 */
    char       active_slot;       /* 当前活动槽位：'A' 或 'B' */
    char       target_slot;       /* 本次 OTA 目标槽位：'A' 或 'B' */
    rt_uint32_t ver_a;            /* 槽位 A 当前版本（来自 OTA meta 块） */
    rt_uint32_t ver_b;            /* 槽位 B 当前版本（来自 OTA meta 块） */
    rt_uint32_t target_version;   /* OTA 完成后写回到对应槽位 meta 的版本号 */
    rt_bool_t   version_updated;  /* 已经更新过槽位版本块 */

    rt_uint64_t file_pos;     /* 当前写入到的文件偏移（用于保证顺序写） */
    rt_uint64_t need_end;     /* rtt/rtapp 内容区的最大结束偏移（仅用于调试） */

    rt_uint64_t rtt_written;  /* 已写入 rtt 镜像的字节数 */
    rt_uint64_t rtapp_written;/* 已写入 rtapp 镜像的字节数 */

    /* header: 固定 512B */
    rt_size_t   hdr_have;
    rt_uint8_t  hdr_buf[KDIMG_HDR_SIZE];
    struct kd_img_hdr hdr;

    /* 分区表: KDIMG_PART_ENTRY_SIZE * part_tbl_num */
    rt_size_t   tbl_have;
    rt_size_t   tbl_total;
    rt_uint8_t *tbl_buf;
    rt_uint32_t tbl_num;

    const struct kd_img_part *part_rtt;
    const struct kd_img_part *part_rtapp;
    struct ota_partition *dst_rtt;
    struct ota_partition *dst_rtapp;
};

static struct ota_kd_ctx g_kdctx;

#define OTA_VERIFY_BUF_SIZE  4096u

static rt_err_t ota_toc_load(void);
static struct ota_partition *ota_find_partition(const char *name);

static rt_err_t ota_mmc_open_device(void)
{
    char dev_name[8] = { 0 };
    int mmc_id = -1;

    if (g_boot_mode == SYSCTL_BOOT_EMMC) {
        mmc_id = 0;	/* eMMC -> sd0 */
    } else if (g_boot_mode == SYSCTL_BOOT_SDCARD) {
        mmc_id = 1;	/* SDCard -> sd1 */
    } else {
        return -RT_ERROR;
    }

    rt_snprintf(dev_name, sizeof(dev_name), "sd%d", mmc_id);
    g_mmc_dev = rt_device_find(dev_name);
    if (!g_mmc_dev) {
        LOG_E("ota: cannot find mmc device %s", dev_name);
        return -RT_ERROR;
    }

    rt_err_t ret = rt_device_open(g_mmc_dev, RT_DEVICE_OFLAG_RDWR);
    if ((ret != RT_EOK) && (ret != -RT_EBUSY)) {
        LOG_E("ota: open %s failed: %d", dev_name, ret);
        g_mmc_dev = RT_NULL;
        return ret;
    }

    return RT_EOK;
}

/* 比较两个定长摘要是否一致（常量时间实现，避免早退带来的时间侧信道） */
static rt_bool_t ota_digest_equal(const rt_uint8_t *a,
                                  const rt_uint8_t *b,
                                  rt_size_t len)
{
    rt_uint8_t diff = 0;
    rt_size_t i;

    for (i = 0; i < len; i++)
        diff |= (rt_uint8_t)(a[i] ^ b[i]);

    return (diff == 0) ? RT_TRUE : RT_FALSE;
}

static const rt_uint32_t ota_crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static rt_uint32_t ota_crc32(const void *data, rt_size_t len)
{
    const rt_uint8_t *buf = (const rt_uint8_t *)data;
    rt_uint32_t res = 0xffffffffu;
    rt_size_t i;

    for (i = 0; i < len; i++)
        res = ota_crc32_table[(res ^ buf[i]) & 0xff] ^ (res >> 8);

    return res ^ 0xffffffffu;
}

static rt_uint64_t ota_meta_offset_for_slot(char slot)
{
    static struct ota_partition *meta_part;
    rt_uint64_t span;

    if (!meta_part) {
        meta_part = ota_find_partition("ota_meta");
        if (!meta_part) {
            LOG_E("ota: no 'ota_meta' partition in TOC");
            return 0;
        }
    }

    /* 一个 ota_meta 分区中为 A/B 两个槽位平均划分空间 */
    if (!meta_part->size) {
        LOG_E("ota: ota_meta partition size is zero");
        return 0;
    }
    span = meta_part->size / 2;

    if (slot == 'B')
        return meta_part->offset + span;

    return meta_part->offset;
}

static rt_err_t ota_meta_read_slot(char slot,
                                   struct ota_slot_meta *meta,
                                   rt_bool_t *valid)
{
    rt_uint64_t off;
    rt_err_t ret;
    rt_uint32_t crc;
    rt_uint32_t saved_crc;

    if (valid)
        *valid = RT_FALSE;

    if (!meta)
        return -RT_EINVAL;

    rt_memset(meta, 0, sizeof(*meta));

    off = ota_meta_offset_for_slot(slot);
    if (!off) {
        LOG_E("ota: meta offset for slot %c invalid (no ota_meta)", slot);
        return RT_EOK;
    }
    ret = ota_storage_read(off, meta, sizeof(*meta));
    if (ret != RT_EOK) {
        LOG_E("ota: read meta slot %c failed, ret=%d", slot, ret);
        /* 读取失败视为没有有效版本，不作为致命错误 */
        return RT_EOK;
    }

    if (meta->magic != OTA_META_MAGIC)
        return RT_EOK;

    saved_crc = meta->crc32;
    meta->crc32 = 0;
    crc = ota_crc32(meta, sizeof(*meta));
    meta->crc32 = saved_crc;
    if (crc != meta->crc32) {
        LOG_E("ota: meta slot %c crc mismatch: calc=0x%08x img=0x%08x",
              slot, crc, meta->crc32);
        return RT_EOK;
    }

    if (valid)
        *valid = RT_TRUE;

    return RT_EOK;
}

static rt_err_t ota_meta_write_slot(char slot, rt_uint32_t new_ver)
{
    struct ota_slot_meta meta;
    rt_uint64_t off;
    rt_err_t ret;

    rt_memset(&meta, 0, sizeof(meta));
    meta.magic = OTA_META_MAGIC;
    meta.version = new_ver;
    meta.reserved = 0;
    meta.crc32 = 0;

    meta.crc32 = ota_crc32(&meta, sizeof(meta));

    off = ota_meta_offset_for_slot(slot);
    if (!off) {
        LOG_E("ota: meta offset for slot %c invalid (no ota_meta)", slot);
        return -RT_ERROR;
    }
    ret = ota_storage_write(off, &meta, sizeof(meta));
    if (ret != RT_EOK) {
        LOG_E("ota: write meta slot %c failed, ret=%d", slot, ret);
        return ret;
    }

    LOG_D("ota: slot %c meta updated, version=0x%x", slot, new_ver);

    return RT_EOK;
}

static void ota_kdctx_reset(void)
{
    if (g_kdctx.tbl_buf) {
        rt_free(g_kdctx.tbl_buf);
        g_kdctx.tbl_buf = RT_NULL;
    }

    rt_memset(&g_kdctx, 0, sizeof(g_kdctx));
    g_kdctx.probing = RT_TRUE;
    g_kdctx.active_slot = 'A';
    g_kdctx.target_slot = 'A';
}

static rt_err_t ota_kd_parse_header(struct ota_kd_ctx *ctx)
{
    struct kd_img_hdr *hdr_view;
    rt_uint32_t saved_crc;
    rt_uint32_t crc;

    hdr_view = (struct kd_img_hdr *)ctx->hdr_buf;

    if (hdr_view->img_hdr_magic != KDIMG_HDR_MAGIC) {
        LOG_E("ota: kdimg magic invalid: 0x%08x", hdr_view->img_hdr_magic);
        return -RT_ERROR;
    }

    saved_crc = hdr_view->img_hdr_crc32;
    hdr_view->img_hdr_crc32 = 0;
    crc = ota_crc32(hdr_view, sizeof(*hdr_view));
    if (crc != saved_crc) {
        LOG_E("ota: kdimg header crc mismatch: calc=0x%08x img=0x%08x",
              crc, saved_crc);
        return -RT_ERROR;
    }

    if (!hdr_view->part_tbl_num) {
        LOG_E("ota: kdimg part_tbl_num is zero");
        return -RT_ERROR;
    }

    ctx->hdr = *hdr_view;
    ctx->tbl_num = hdr_view->part_tbl_num;
    ctx->tbl_total = (rt_size_t)ctx->tbl_num * KDIMG_PART_ENTRY_SIZE;

    ctx->tbl_buf = (rt_uint8_t *)rt_malloc(ctx->tbl_total);
    if (!ctx->tbl_buf) {
        LOG_E("ota: no memory for kdimg part table, size=%lu",
              (unsigned long)ctx->tbl_total);
        return -RT_ENOMEM;
    }

    ctx->tbl_have = 0;
    ctx->header_done = RT_TRUE;

    return RT_EOK;
}

static rt_err_t ota_kd_parse_part_table(struct ota_kd_ctx *ctx)
{
    rt_uint32_t crc;
    rt_uint32_t i;

    crc = ota_crc32(ctx->tbl_buf, ctx->tbl_total);
    if (crc != ctx->hdr.part_tbl_crc32) {
        LOG_E("ota: kdimg part table crc mismatch: calc=0x%08x img=0x%08x",
              crc, ctx->hdr.part_tbl_crc32);
        return -RT_ERROR;
    }

    ctx->part_rtt = RT_NULL;
    ctx->part_rtapp = RT_NULL;
    ctx->dst_rtt = RT_NULL;
    ctx->dst_rtapp = RT_NULL;
    ctx->ver_a = 0;
    ctx->ver_b = 0;
    ctx->target_version = 0;
    ctx->active_slot = 'A';
    ctx->target_slot = 'A';
    ctx->version_updated = RT_FALSE;

    for (i = 0; i < ctx->tbl_num; i++) {
        struct kd_img_part *p;

        p = (struct kd_img_part *)(ctx->tbl_buf +
                                   (rt_size_t)i * KDIMG_PART_ENTRY_SIZE);

        if (p->part_magic != KDIMG_PART_MAGIC) {
            LOG_E("ota: kdimg part[%u] magic invalid: 0x%08x",
                  i, p->part_magic);
            return -RT_ERROR;
        }

        if (!rt_strncmp(p->part_name, "rtt", sizeof(p->part_name)))
            ctx->part_rtt = p;
        else if (!rt_strncmp(p->part_name, "rtapp", sizeof(p->part_name)))
            ctx->part_rtapp = p;
    }

    if (ctx->part_rtt) {
        struct ota_partition *rtt_a;
        struct ota_partition *rtt_b;
        struct ota_partition *rtapp_a;
        struct ota_partition *rtapp_b;
        struct ota_slot_meta meta_a;
        struct ota_slot_meta meta_b;
        rt_bool_t valid_a = RT_FALSE;
        rt_bool_t valid_b = RT_FALSE;

        rtt_a = ota_find_partition("rtt_a");
        rtt_b = ota_find_partition("rtt_b");
        rtapp_a = ota_find_partition("rtapp_a");
        rtapp_b = ota_find_partition("rtapp_b");

        if (!rtt_a && !rtt_b) {
            LOG_E("ota: no 'rtt_a' or 'rtt_b' partition in TOC");
            return -RT_ERROR;
        }

        ota_meta_read_slot('A', &meta_a, &valid_a);
        ota_meta_read_slot('B', &meta_b, &valid_b);

        ctx->ver_a = valid_a ? meta_a.version : 0;
        ctx->ver_b = valid_b ? meta_b.version : 0;

        if (ctx->ver_a == ctx->ver_b) {
            ctx->active_slot = 'A';
            ctx->target_slot = 'B';
        } else if (ctx->ver_a > ctx->ver_b) {
            ctx->active_slot = 'A';
            ctx->target_slot = 'B';
        } else {
            ctx->active_slot = 'B';
            ctx->target_slot = 'A';
        }

        if ((ctx->ver_a > ctx->ver_b ?
             (ctx->ver_a - ctx->ver_b) :
             (ctx->ver_b - ctx->ver_a)) > 1) {
            LOG_E("ota: slot version gap too large: A=0x%x B=0x%x",
                  ctx->ver_a, ctx->ver_b);
        }

        ctx->target_version =
            (ctx->ver_a > ctx->ver_b ? ctx->ver_a : ctx->ver_b) + 1;

        if (ctx->target_slot == 'A') {
            ctx->dst_rtt = rtt_a;
            ctx->dst_rtapp = rtapp_a;
        } else {
            ctx->dst_rtt = rtt_b;
            ctx->dst_rtapp = rtapp_b;
        }

        if (!ctx->dst_rtt) {
            LOG_E("ota: target slot %c has no rtt partition",
                  ctx->target_slot);
            return -RT_ERROR;
        }

        if (ctx->dst_rtt->size &&
            (rt_uint64_t)ctx->part_rtt->part_content_size >
            ctx->dst_rtt->size) {
            LOG_E("ota: rtt content too large: 0x%x > 0x%lx",
                  ctx->part_rtt->part_content_size,
                  (unsigned long long)ctx->dst_rtt->size);
            return -RT_ERROR;
        }

        if (ctx->part_rtt->part_content_offset < KDIMG_CONTENT_START_OFF) {
            LOG_E("ota: rtt content_offset too small: 0x%x",
                  ctx->part_rtt->part_content_offset);
            return -RT_ERROR;
        }

        LOG_D("ota: OTA target slot=%c, new version=0x%x",
              ctx->target_slot, ctx->target_version);
    }

    if (ctx->part_rtapp) {
        if (!ctx->dst_rtapp) {
            LOG_E("ota: no target rtapp partition in TOC");
            ctx->part_rtapp = RT_NULL;
        } else if (ctx->dst_rtapp->size &&
                   (rt_uint64_t)ctx->part_rtapp->part_content_size >
                   ctx->dst_rtapp->size) {
            LOG_E("ota: rtapp content too large: 0x%x > 0x%lx",
                  ctx->part_rtapp->part_content_size,
                  (unsigned long long)ctx->dst_rtapp->size);
            return -RT_ERROR;
        } else if (ctx->part_rtapp->part_content_offset <
                   KDIMG_CONTENT_START_OFF) {
            LOG_E("ota: rtapp content_offset too small: 0x%x",
                  ctx->part_rtapp->part_content_offset);
            return -RT_ERROR;
        }
    }

    ctx->need_end = 0;
    if (ctx->part_rtt) {
        rt_uint64_t end;

        end = (rt_uint64_t)ctx->part_rtt->part_content_offset +
            ctx->part_rtt->part_content_size;
        if (end > ctx->need_end)
            ctx->need_end = end;

        LOG_D("ota: rtt part: content_off=0x%x size=0x%x",
              ctx->part_rtt->part_content_offset,
              ctx->part_rtt->part_content_size);
    }

    if (ctx->part_rtapp) {
        rt_uint64_t end;

        end = (rt_uint64_t)ctx->part_rtapp->part_content_offset +
            ctx->part_rtapp->part_content_size;
        if (end > ctx->need_end)
            ctx->need_end = end;

        LOG_D("ota: rtapp part: content_off=0x%x size=0x%x",
              ctx->part_rtapp->part_content_offset,
              ctx->part_rtapp->part_content_size);
    }

    ctx->table_done = RT_TRUE;
    ctx->is_kdimg = RT_TRUE;
    ctx->probing = RT_FALSE;

    LOG_D("ota: kdimg hdr ok: parts=%u, part_tbl_crc32=0x%08x",
          ctx->tbl_num, ctx->hdr.part_tbl_crc32);

    return RT_EOK;
}

/*
 * 对已经写入存储介质的目标分区做回读 + SHA256 校验：
 *   - 按 KDIMG 分区表中的 part_content_size 从 dst->offset 开始读取
 *   - 使用 ota_sha256_* 计算摘要，与 part->part_content_sha256 对比
 *   - 校验成功返回 RT_EOK，失败则返回错误码，不更新版本块
 */
static rt_err_t ota_verify_part_sha256(const char *name,
                                       const struct ota_partition *dst,
                                       const struct kd_img_part *part)
{
    ota_sha256_ctx sha;
    rt_uint8_t digest[OTA_SHA256_DIGEST_LENGTH];
    rt_uint8_t *buf;
    rt_uint64_t left;
    rt_uint64_t off;
    rt_err_t ret;

    if (!dst || !part)
        return -RT_EINVAL;

    ota_sha256_init(&sha);

    buf = (rt_uint8_t *)rt_malloc(OTA_VERIFY_BUF_SIZE);
    if (!buf) {
        LOG_E("ota: no memory for verify buffer");
        return -RT_ENOMEM;
    }

    left = part->part_content_size;
    off = dst->offset;

    while (left) {
        rt_size_t chunk = (left > OTA_VERIFY_BUF_SIZE) ?
                          (rt_size_t)OTA_VERIFY_BUF_SIZE :
                          (rt_size_t)left;

        ret = ota_storage_read(off, buf, chunk);
        if (ret != RT_EOK) {
            LOG_E("ota: readback %s failed at off=0x%lx len=0x%lx, ret=%d",
                  name,
                  (unsigned long long)off,
                  (unsigned long long)chunk,
                  ret);
            rt_free(buf);
            return ret;
        }

        ota_sha256_update(&sha, buf, chunk);

        off  += chunk;
        left -= chunk;
    }

    rt_free(buf);

    ota_sha256_final(&sha, digest);

    if (!ota_digest_equal(digest,
                          part->part_content_sha256,
                          OTA_SHA256_DIGEST_LENGTH)) {
        LOG_E("ota: %s sha256 mismatch, will NOT update version", name);
        return -RT_ERROR;
    }

    LOG_D("ota: %s sha256 verify ok", name);

    return RT_EOK;
}

static rt_err_t ota_kd_write_part(const char *name,
                                  struct ota_partition *dst,
                                  const struct kd_img_part *part,
                                  rt_uint64_t start,
                                  rt_uint64_t end,
                                  const rt_uint8_t *buf,
                                  rt_uint64_t *written)
{
    rt_uint64_t region_start;
    rt_uint64_t region_end;
    rt_uint64_t overlap_start;
    rt_uint64_t overlap_end;
    rt_uint64_t len;
    rt_uint64_t buf_off;
    rt_uint64_t part_rel;
    rt_uint64_t flash_off;
    rt_err_t ret;

    if (!dst || !part)
        return RT_EOK;

    region_start = part->part_content_offset;
    region_end = (rt_uint64_t)part->part_content_offset +
        part->part_content_size;

    if (end <= region_start || start >= region_end)
        return RT_EOK;

    overlap_start = (start > region_start) ? start : region_start;
    overlap_end = (end < region_end) ? end : region_end;
    len = overlap_end - overlap_start;

    buf_off = overlap_start - start;
    part_rel = overlap_start - region_start;
    flash_off = dst->offset + part_rel;

    ret = ota_storage_write(flash_off, buf + buf_off, (rt_size_t)len);
    if (ret != RT_EOK) {
        LOG_E("ota: write part '%s' failed at off=0x%lx len=0x%lx, ret=%d",
              name,
              (unsigned long long)flash_off,
              (unsigned long long)len, ret);
        return ret;
    }

    if (written)
        *written += len;

    if (written && *written == part->part_content_size) {
        LOG_D("ota: %s updated: off=0x%lx len=0x%x",
              name,
              (unsigned long long)dst->offset,
              part->part_content_size);
    }

    return RT_EOK;
}

static rt_err_t ota_kd_stream_write(struct ota_kd_ctx *ctx,
                                    const rt_uint8_t *buf,
                                    rt_size_t size)
{
    rt_uint64_t pos;
    const rt_uint8_t *p;
    rt_size_t left;
    rt_err_t ret;

    if (!size)
        return RT_EOK;

    if (ctx->invalid)
        return -RT_EINVAL;

    pos  = ctx->file_pos;
    p    = buf;
    left = size;

    /* 1. 累积 header（0~KDIMG_HDR_SIZE）*/
    if (!ctx->header_done && pos < KDIMG_HDR_SIZE) {
        rt_uint64_t need;
        rt_size_t copy_len;

        if (ctx->hdr_have < KDIMG_HDR_SIZE)
            need = KDIMG_HDR_SIZE - ctx->hdr_have;
        else
            need = 0;

        if (need && left) {
            copy_len = (left > (rt_size_t)need) ? (rt_size_t)need : left;

            rt_memcpy(ctx->hdr_buf + ctx->hdr_have, p, copy_len);
            ctx->hdr_have += copy_len;
            p   += copy_len;
            left -= copy_len;
            pos  += copy_len;
        }

        if (!ctx->header_done && ctx->hdr_have >= KDIMG_HDR_SIZE) {
            ret = ota_kd_parse_header(ctx);
            if (ret != RT_EOK) {
                ctx->invalid = RT_TRUE;
                return ret;
            }
        }
    }

    /* 2. 累积分区表（紧跟 header 之后，大小为 tbl_total） */
    if (ctx->header_done && !ctx->table_done && left) {
        rt_uint64_t need;
        rt_size_t copy_len;

        if (ctx->tbl_have < ctx->tbl_total)
            need = ctx->tbl_total - ctx->tbl_have;
        else
            need = 0;

        if (need && left) {
            copy_len = (left > (rt_size_t)need) ? (rt_size_t)need : left;

            rt_memcpy(ctx->tbl_buf + ctx->tbl_have, p, copy_len);
            ctx->tbl_have += copy_len;
            p   += copy_len;
            left -= copy_len;
            pos  += copy_len;
        }

        if (!ctx->table_done && ctx->tbl_have >= ctx->tbl_total) {
            ret = ota_kd_parse_part_table(ctx);
            if (ret != RT_EOK) {
                ctx->invalid = RT_TRUE;
                return ret;
            }
        }
    }

    /* 3. 还在探测 header/table 阶段，不对 flash 做任何写入 */
    if (!ctx->is_kdimg)
        return RT_EOK;

    /* 4. 剩余部分（若有）属于内容区，按 rtt/rtapp 分区表流式写入 */
    if (left) {
        rt_uint64_t start = pos;
        rt_uint64_t end   = pos + left;

        if (ctx->part_rtt && ctx->dst_rtt) {
            ret = ota_kd_write_part("rtt", ctx->dst_rtt, ctx->part_rtt,
                                    start, end, p, &ctx->rtt_written);
            if (ret != RT_EOK)
                return ret;
        }

        if (ctx->part_rtapp && ctx->dst_rtapp) {
            ret = ota_kd_write_part("rtapp", ctx->dst_rtapp,
                                    ctx->part_rtapp,
                                    start, end, p, &ctx->rtapp_written);
            if (ret != RT_EOK)
                return ret;
        }
    }

    /*
     * 当本次 OTA 目标槽位的 rtt/rtapp 都写完后，更新对应槽位的版本块。
     * 这里允许 rtapp 可选：若 kdimage 中没有 rtapp，则仅以 rtt 完成为准。
     */
    if (ctx->is_kdimg && !ctx->version_updated && ctx->part_rtt &&
        ctx->dst_rtt &&
        ctx->rtt_written == ctx->part_rtt->part_content_size) {
        if (!ctx->part_rtapp || !ctx->dst_rtapp ||
            ctx->rtapp_written == ctx->part_rtapp->part_content_size) {
            /* 先对 rtt/rtapp 做回读 + SHA256 校验，确保写入真实可靠 */
            if (ctx->part_rtt && ctx->dst_rtt) {
                ret = ota_verify_part_sha256("rtt",
                                             ctx->dst_rtt,
                                             ctx->part_rtt);
                if (ret != RT_EOK)
                    return ret;
            }

            if (ctx->part_rtapp && ctx->dst_rtapp) {
                ret = ota_verify_part_sha256("rtapp",
                                             ctx->dst_rtapp,
                                             ctx->part_rtapp);
                if (ret != RT_EOK)
                    return ret;
            }

            ret = ota_meta_write_slot(ctx->target_slot,
                                      ctx->target_version);
            if (ret == RT_EOK)
                ctx->version_updated = RT_TRUE;
            else
                return ret;
        }
    }

    return RT_EOK;
}

/*
 * MMC/SD 读写：支持任意字节长度
 *   - 若 offset 和 len 都是 512B 对齐，则直接整扇区读写（不做 read‑modify‑write）
 *   - 对于头尾不对齐的部分，仅对首尾扇区做一次 read‑modify‑write
 *   - 中间的整扇区区间直接读写用户 buffer，避免额外拷贝
 */
static rt_err_t ota_mmc_rw(rt_bool_t is_write,
                           rt_uint64_t offset,
                           void *buffer,
                           rt_size_t len)
{
    const rt_uint32_t SECTOR = SECTOR_SIZE;
    rt_uint8_t tmp[SECTOR];
    rt_uint8_t *buf = buffer;
    rt_uint64_t cur_off;
    rt_uint64_t lba;
    rt_size_t sectors;
    rt_size_t done;

    if (!g_mmc_dev)
        return -RT_ERROR;

    if (!len)
        return RT_EOK;

    cur_off = offset;

    /* 完全对齐的简单路径：直接整扇区访问 */
    if ((offset % SECTOR) == 0 && (len % SECTOR) == 0) {
        lba = offset / SECTOR;
        sectors = len / SECTOR;

        if (!sectors)
            return RT_EOK;

        if (!is_write)
            done = rt_device_read(g_mmc_dev, (rt_off_t)lba, buf, sectors);
        else
            done = rt_device_write(g_mmc_dev, (rt_off_t)lba, buf, sectors);

        if (done != sectors) {
            LOG_E("ota: mmc %s failed, expect %u, done %d",
                  is_write ? "write" : "read",
                  (unsigned int)sectors, (int)done);
            return -RT_ERROR;
        }

        return RT_EOK;
    }

    /*
     * 下面处理未对齐的情况：
     *   1) 头部非对齐扇区（若有）：read‑modify‑write
     *   2) 中间整扇区：直接读写
     *   3) 尾部非对齐扇区（若有）：read‑modify‑write
     */

    /* 1) 处理头部非对齐扇区 */
    if (cur_off % SECTOR) {
        rt_uint32_t head_off = cur_off % SECTOR;
        rt_uint32_t head_len = SECTOR - head_off;

        if (head_len > len)
            head_len = len;

        lba = cur_off / SECTOR;

        /* 读出整个扇区 */
        done = rt_device_read(g_mmc_dev, (rt_off_t)lba, tmp, 1);
        if (done != 1) {
            LOG_E("ota: mmc read head sector failed, done=%d", (int)done);
            return -RT_ERROR;
        }

        if (!is_write) {
            rt_memcpy(buf, tmp + head_off, head_len);
        } else {
            rt_memcpy(tmp + head_off, buf, head_len);
            done = rt_device_write(g_mmc_dev, (rt_off_t)lba, tmp, 1);
            if (done != 1) {
                LOG_E("ota: mmc write head sector failed, done=%d", (int)done);
                return -RT_ERROR;
            }
        }

        cur_off += head_len;
        buf     += head_len;
        len     -= head_len;
    }

    /* 2) 处理中间整扇区 */
    if (len >= SECTOR) {
        sectors = len / SECTOR;
        lba = cur_off / SECTOR;

        if (!is_write)
            done = rt_device_read(g_mmc_dev, (rt_off_t)lba, buf, sectors);
        else
            done = rt_device_write(g_mmc_dev, (rt_off_t)lba, buf, sectors);

        if (done != sectors) {
            LOG_E("ota: mmc %s body failed, expect %u, done %d",
                  is_write ? "write" : "read",
                  (unsigned int)sectors, (int)done);
            return -RT_ERROR;
        }

        cur_off += (rt_uint64_t)sectors * SECTOR;
        buf     += (rt_uint64_t)sectors * SECTOR;
        len     -= (rt_uint64_t)sectors * SECTOR;
    }

    /* 3) 处理尾部非对齐扇区 */
    if (len) {
        lba = cur_off / SECTOR;

        done = rt_device_read(g_mmc_dev, (rt_off_t)lba, tmp, 1);
        if (done != 1) {
            LOG_E("ota: mmc read tail sector failed, done=%d", (int)done);
            return -RT_ERROR;
        }

        if (!is_write) {
            rt_memcpy(buf, tmp, len);
        } else {
            rt_memcpy(tmp, buf, len);
            done = rt_device_write(g_mmc_dev, (rt_off_t)lba, tmp, 1);
            if (done != 1) {
                LOG_E("ota: mmc write tail sector failed, done=%d", (int)done);
                return -RT_ERROR;
            }
        }
    }

    return RT_EOK;
}

static rt_err_t ota_mmc_erase(rt_uint64_t offset, rt_uint64_t size)
{
    (void)offset;
    (void)size;
    return RT_EOK;
}

static rt_err_t ota_nand_rw(rt_bool_t is_write,
                            rt_uint64_t offset,
                            void *buffer,
                            rt_size_t len)
{
    (void)is_write;
    (void)offset;
    (void)buffer;
    (void)len;
    return -RT_ENOSYS;
}

static rt_err_t ota_nand_erase(rt_uint64_t offset, rt_uint64_t size)
{
    (void)offset;
    (void)size;
    return -RT_ENOSYS;
}

static rt_err_t ota_toc_load(void)
{
    rt_err_t ret;
    rt_size_t i;

    if (g_ota_toc_loaded)
        return RT_EOK;

    if (g_boot_mode == SYSCTL_BOOT_MAX) {
        ret = ota_storage_init();
        if (ret != RT_EOK)
            return ret;
    }

    rt_memset(&g_ota_toc, 0, sizeof(g_ota_toc));

    ret = ota_storage_read(K230_TOC_OFFSET,
                           g_ota_toc.entries,
                           sizeof(g_ota_toc.entries));
    if (ret != RT_EOK) {
        LOG_E("ota: read TOC failed, ret=%d", ret);
        return ret;
    }

    g_ota_toc.entry_count = 0;

    for (i = 0; i < K230_TOC_MAX_ENTRIES; i++) {
        if (g_ota_toc.entries[i].name[0] == '\0')
            break;

        g_ota_toc.entry_count++;
    }

    if (!g_ota_toc.entry_count) {
        LOG_E("ota: TOC is empty");
        return -RT_ERROR;
    }

    g_ota_toc_loaded = RT_TRUE;

    LOG_D("ota: TOC loaded, entries=%u", g_ota_toc.entry_count);

    return RT_EOK;
}

static struct ota_partition *ota_find_partition(const char *name)
{
    rt_size_t i;

    if (!name)
        return RT_NULL;

    if (ota_toc_load() != RT_EOK)
        return RT_NULL;

    for (i = 0; i < g_ota_toc.entry_count; i++) {
        struct ota_partition *e = &g_ota_toc.entries[i];

        if (rt_strncmp(e->name, name, sizeof(e->name)) == 0)
            return e;
    }

    return RT_NULL;
}

rt_err_t ota_storage_init(void)
{
    g_boot_mode = sysctl_boot_get_boot_mode();
    LOG_D("ota: boot mode = %d", g_boot_mode);

    switch (g_boot_mode) {
    case SYSCTL_BOOT_EMMC:
    case SYSCTL_BOOT_SDCARD:
        return ota_mmc_open_device();

    case SYSCTL_BOOT_NANDFLASH:
        LOG_E("ota: NAND boot mode not implemented yet");
        return -RT_ENOSYS;

    case SYSCTL_BOOT_NORFLASH:
        LOG_E("ota: NOR boot mode not implemented yet");
        return -RT_ENOSYS;

    default:
        LOG_E("ota: invalid boot mode %d", g_boot_mode);
        return -RT_ERROR;
    }
}

rt_err_t ota_storage_read(rt_uint64_t offset, void *buf, rt_size_t len)
{
    switch (g_boot_mode) {
    case SYSCTL_BOOT_EMMC:
    case SYSCTL_BOOT_SDCARD:
        return ota_mmc_rw(RT_FALSE, offset, buf, len);
    case SYSCTL_BOOT_NANDFLASH:
        return ota_nand_rw(RT_FALSE, offset, buf, len);
    case SYSCTL_BOOT_NORFLASH:
        return -RT_ENOSYS;
    default:
        return -RT_ERROR;
    }
}

rt_err_t ota_storage_write(rt_uint64_t offset, const void *buf, rt_size_t len)
{
    void *non_const = (void *)buf;

    switch (g_boot_mode) {
    case SYSCTL_BOOT_EMMC:
    case SYSCTL_BOOT_SDCARD:
        return ota_mmc_rw(RT_TRUE, offset, non_const, len);
    case SYSCTL_BOOT_NANDFLASH:
        return ota_nand_rw(RT_TRUE, offset, non_const, len);
    case SYSCTL_BOOT_NORFLASH:
        return -RT_ENOSYS;
    default:
        return -RT_ERROR;
    }
}

rt_err_t ota_storage_erase(rt_uint64_t offset, rt_uint64_t size)
{
    switch (g_boot_mode) {
    case SYSCTL_BOOT_EMMC:
    case SYSCTL_BOOT_SDCARD:
        return ota_mmc_erase(offset, size);
    case SYSCTL_BOOT_NANDFLASH:
        return ota_nand_erase(offset, size);
    case SYSCTL_BOOT_NORFLASH:
        return -RT_ENOSYS;
    default:
        return -RT_ERROR;
    }
}

static rt_err_t ota_dev_init(rt_device_t dev)
{
    (void)dev;

    ota_kdctx_reset();

    return ota_storage_init();
}

static rt_err_t ota_dev_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;

    ota_kdctx_reset();

    return RT_EOK;
}

static rt_err_t ota_dev_close(rt_device_t dev)
{
    (void)dev;

    ota_kdctx_reset();

    return RT_EOK;
}

static rt_size_t ota_dev_read(rt_device_t dev, rt_off_t pos,
                              void *buffer, rt_size_t size)
{
    (void)dev;
    (void)pos;
    (void)buffer;
    (void)size;

    return 0;
}

static rt_size_t ota_dev_write(rt_device_t dev, rt_off_t pos,
                               const void *buffer, rt_size_t size)
{
    rt_err_t ret;

    (void)dev;

    if (!buffer || !size)
        return -1;

    if (g_kdctx.invalid)
        return -1;

    /* 目前仅支持从 offset=0 开始的顺序写入 */
    if ((rt_uint64_t)pos != g_kdctx.file_pos) {
        LOG_E("ota: non-sequential write, pos=0x%lx expected=0x%lx",
              (unsigned long)pos,
              (unsigned long)g_kdctx.file_pos);
        g_kdctx.invalid = RT_TRUE;
        return -1;
    }

    ret = ota_kd_stream_write(&g_kdctx,
                              (const rt_uint8_t *)buffer, size);
    if (ret != RT_EOK) {
        LOG_E("ota: kdimage stream write failed, ret=%d", ret);
        g_kdctx.invalid = RT_TRUE;
        return -1;
    }

    g_kdctx.file_pos += size;

    return size;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops ota_dev_ops = {
    ota_dev_init,
    ota_dev_open,
    ota_dev_close,
    ota_dev_read,
    ota_dev_write,
    RT_NULL
};
#endif

static struct rt_device g_ota_device;

static int rt_hw_ota_register(void)
{
    rt_err_t ret;
    struct rt_device *device = &g_ota_device;

    rt_memset(device, 0, sizeof(*device));

    device->type = RT_Device_Class_Char;
    device->rx_indicate = RT_NULL;
    device->tx_complete = RT_NULL;

#ifdef RT_USING_DEVICE_OPS
    device->ops = &ota_dev_ops;
#else
    device->init = ota_dev_init;
    device->open = ota_dev_open;
    device->close = ota_dev_close;
    device->read = ota_dev_read;
    device->write = ota_dev_write;
    device->control = RT_NULL;
#endif

    ret = rt_device_register(device, "ota", RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK) {
        LOG_E("ota: device_register failed: %d", ret);
        return ret;
    }

    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_ota_register);
