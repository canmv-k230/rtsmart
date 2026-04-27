#include <rtthread.h>

#include <stdlib.h>
#include <string.h>

#include <dfs_posix.h>

#if defined(RT_USING_MSH)
#include <finsh.h>
#endif

#include "drv_pufs_internal.h"

#include "pufs_common.h"
#include "pufs_hmac.h"
#include "pufs_rt.h"

#if defined(RT_USING_MSH) && defined(RT_PUFS_ENABLE_BUILTIN_CMD)

static void pufs_otp_dump_hex(pufs_otp_addr_t addr, const uint8_t *buf,
                              rt_size_t len)
{
    rt_size_t offset;
    rt_size_t index;

    for (offset = 0; offset < len; offset += 16) {
        rt_size_t line_len = (len - offset > 16) ? 16 : (len - offset);

        rt_kprintf("0x%04x: ", addr + offset);
        for (index = 0; index < line_len; index++) {
            rt_kprintf("%02x ", buf[offset + index]);
        }
        rt_kprintf("\n");
    }
}

static rt_err_t pufs_otp_parse_u32(const char *text, rt_uint32_t *value)
{
    char *end = RT_NULL;
    unsigned long parsed;

    if (text == RT_NULL || value == RT_NULL)
        return -RT_EINVAL;

    parsed = strtoul(text, &end, 0);
    if (end == text || *end != '\0')
        return -RT_EINVAL;

    *value = (rt_uint32_t)parsed;
    return RT_EOK;
}

static const char *pufs_otp_lock_name(pufs_otp_lock_t lock)
{
    switch (lock) {
    case NA:
        return "NA";
    case RO:
        return "RO";
    case RW:
        return "RW";
    default:
        return "UNKNOWN";
    }
}

static void pufs_otp_usage(void)
{
    rt_kprintf("Usage:\n");
    rt_kprintf("  pufs_otp read <addr> <len>\n");
    rt_kprintf("  pufs_otp write <addr> <val0> [val1...]\n");
    rt_kprintf("  pufs_otp lock query <addr>\n");
    rt_kprintf("  pufs_otp lock set <addr> <len> <RO|RW>\n");
    rt_kprintf("Notes:\n");
    rt_kprintf("  addr and len use the flat OTP address space (0x000-0xFFF)\n");
    rt_kprintf("  addr must be 4-byte aligned, len must be a non-zero multiple of 4\n");
}

static int pufs_otp_read_cmd(int argc, char **argv)
{
    int ret;
    rt_uint32_t raw_addr;
    rt_uint32_t raw_len;
    pufs_otp_addr_t addr;
    uint8_t *buf;

    if (argc != 4)
        return -RT_EINVAL;

    ret = pufs_otp_parse_u32(argv[2], &raw_addr);
    if (ret != RT_EOK)
        return ret;

    ret = pufs_otp_parse_u32(argv[3], &raw_len);
    if (ret != RT_EOK)
        return ret;

    if (raw_addr > 0x0FFFu || raw_len == 0 || raw_len > 0x1000u ||
        (raw_addr & 0x3u) != 0 || (raw_len & 0x3u) != 0)
        return -RT_EINVAL;

    addr = (pufs_otp_addr_t)raw_addr;
    if ((rt_uint32_t)addr + raw_len > 0x1000u)
        return -RT_EINVAL;

    buf = rt_malloc(raw_len);
    if (buf == RT_NULL)
        return -RT_ENOMEM;

    ret = pufs_drv_hw_lock();
    if (ret != 0) {
        rt_free(buf);
        return ret;
    }

    ret = pufs_otp_read(buf, raw_len, addr);
    pufs_drv_hw_unlock();

    if (ret == SUCCESS) {
        pufs_otp_dump_hex(addr, buf, raw_len);
        rt_free(buf);
        return 0;
    }

    rt_free(buf);
    return -ret;
}

static int pufs_otp_write_cmd(int argc, char **argv)
{
    int ret;
    rt_uint32_t raw_addr;
    rt_uint32_t raw_len;
    pufs_otp_addr_t addr;
    uint32_t *buf;

    if (argc < 4)
        return -RT_EINVAL;

    ret = pufs_otp_parse_u32(argv[2], &raw_addr);
    if (ret != RT_EOK)
        return ret;

    raw_len = (argc - 3) * 4;

    if (raw_addr > 0x0FFFu || raw_len == 0 || raw_len > 0x1000u ||
        (raw_addr & 0x3u) != 0)
        return -RT_EINVAL;

    addr = (pufs_otp_addr_t)raw_addr;
    if ((rt_uint32_t)addr + raw_len > 0x1000u)
        return -RT_EINVAL;

    buf = rt_malloc(raw_len);
    if (buf == RT_NULL)
        return -RT_ENOMEM;

    for (int i = 0; i < argc - 3; i++) {
        ret = pufs_otp_parse_u32(argv[3 + i], &buf[i]);
        if (ret != RT_EOK) {
            rt_free(buf);
            return ret;
        }
    }

    ret = pufs_drv_hw_lock();
    if (ret != 0) {
        rt_free(buf);
        return ret;
    }

    ret = pufs_otp_write((const uint8_t*)buf, raw_len, addr);
    pufs_drv_hw_unlock();

    rt_free(buf);

    if (ret == SUCCESS) {
        rt_kprintf("OTP write success\n");
        return 0;
    }

    return -ret;
}

static int pufs_otp_lock_cmd(int argc, char **argv)
{
    int ret;
    rt_uint32_t raw_addr;
    rt_uint32_t raw_len;
    pufs_otp_addr_t addr;

    if (argc < 4)
        return -RT_EINVAL;

    if (rt_strcmp(argv[2], "query") == 0) {
        if (argc != 4)
            return -RT_EINVAL;

        ret = pufs_otp_parse_u32(argv[3], &raw_addr);
        if (ret != RT_EOK)
            return ret;

        if (raw_addr > 0x0FFFu || (raw_addr & 0x3u) != 0)
            return -RT_EINVAL;

        addr = (pufs_otp_addr_t)raw_addr;

        ret = pufs_drv_hw_lock();
        if (ret != 0) return ret;
        pufs_otp_lock_t lck = pufs_otp_get_lock(addr);
        pufs_drv_hw_unlock();

        rt_kprintf("lock state: %s\n", pufs_otp_lock_name(lck));
        return 0;
    } else if (rt_strcmp(argv[2], "set") == 0) {
        if (argc != 6)
            return -RT_EINVAL;

        ret = pufs_otp_parse_u32(argv[3], &raw_addr);
        if (ret != RT_EOK)
            return ret;

        ret = pufs_otp_parse_u32(argv[4], &raw_len);
        if (ret != RT_EOK)
            return ret;

        if (raw_addr > 0x0FFFu || raw_len == 0 || raw_len > 0x1000u ||
            (raw_addr & 0x3u) != 0 || (raw_len & 0x3u) != 0)
            return -RT_EINVAL;

        addr = (pufs_otp_addr_t)raw_addr;
        if ((rt_uint32_t)addr + raw_len > 0x1000u)
            return -RT_EINVAL;

        pufs_otp_lock_t lck;
        if (rt_strcmp(argv[5], "RO") == 0 || rt_strcmp(argv[5], "ro") == 0)
            lck = RO;
        else if (rt_strcmp(argv[5], "RW") == 0 || rt_strcmp(argv[5], "rw") == 0)
            lck = RW;
        else
            return -RT_EINVAL;

        ret = pufs_drv_hw_lock();
        if (ret != 0) return ret;
        ret = pufs_otp_set_lock(addr, raw_len, lck);
        pufs_drv_hw_unlock();

        if (ret == SUCCESS) {
            rt_kprintf("OTP lock set success\n");
            return 0;
        }

        return -ret;
    }

    return -RT_EINVAL;
}

static void do_pufs_otp(int argc, char **argv)
{
    int ret;

    if (argc < 2) {
        pufs_otp_usage();
        return;
    }

    if (rt_strcmp(argv[1], "read") == 0) {
        ret = pufs_otp_read_cmd(argc, argv);
    } else if (rt_strcmp(argv[1], "write") == 0) {
        ret = pufs_otp_write_cmd(argc, argv);
    } else if (rt_strcmp(argv[1], "lock") == 0) {
        ret = pufs_otp_lock_cmd(argc, argv);
    } else {
        pufs_otp_usage();
        return;
    }

    if (ret == -RT_EINVAL) {
        pufs_otp_usage();
        return;
    }

    if (ret == -RT_ETIMEOUT) {
        rt_kprintf("pufs_otp failed: hardware lock timeout (%d)\n", ret);
        return;
    }

    if (ret == -RT_ENOMEM) {
        rt_kprintf("pufs_otp failed: no memory (%d)\n", ret);
        return;
    }

    if (ret != 0)
        rt_kprintf("pufs_otp failed: %s (%d)\n", pufs_strstatus(-ret), -ret);
}

static void pufs_otp_sec_usage(void)
{
    rt_kprintf("Usage:\n");
    rt_kprintf("  pufs_otp_sec query\n");
    rt_kprintf("  pufs_otp_sec write <spi2axi|jtag|secure_boot|isp|all> [more items...]\n");
    rt_kprintf("  pufs_otp_sec write all\n");
    rt_kprintf("  pufs_otp_sec lock\n");
    rt_kprintf("Notes:\n");
    rt_kprintf("  write only sets OTP bits from 0 to 1; it cannot clear bits\n");
    rt_kprintf("  lock sets config words 0x0000, 0x0004 and 0x000C to RO\n");
}

static int pufs_otp_sec_query(void)
{
    int ret;
    pufs_otp_security_state_st state;

    ret = pufs_drv_hw_lock();
    if (ret != 0)
        return ret;

    ret = pufs_otp_get_security_config_state(&state);
    pufs_drv_hw_unlock();

    if (ret != SUCCESS)
        return -ret;

    rt_kprintf("disable_spi2axi:    %u\n", state.disable_spi2axi);
    rt_kprintf("disable_jtag:      %u\n", state.disable_jtag);
    rt_kprintf("force_secure_boot: %u\n", state.force_secure_boot);
    rt_kprintf("disable_isp:       %u\n", state.disable_isp);
    rt_kprintf("spi2axi_word_lock: %s (%u)\n",
               pufs_otp_lock_name(state.spi2axi_word_lock),
               state.spi2axi_word_lock);
    rt_kprintf("jtag_word_lock:    %s (%u)\n",
               pufs_otp_lock_name(state.jtag_word_lock),
               state.jtag_word_lock);
    rt_kprintf("boot_ctrl_lock:    %s (%u)\n",
               pufs_otp_lock_name(state.boot_ctrl_word_lock),
               state.boot_ctrl_word_lock);

    return 0;
}

static int pufs_otp_sec_write(int argc, char **argv)
{
    int ret;
    rt_bool_t disable_spi2axi = RT_FALSE;
    rt_bool_t disable_jtag = RT_FALSE;
    rt_bool_t force_secure_boot = RT_FALSE;
    rt_bool_t disable_isp = RT_FALSE;
    int index;

    if (argc < 3)
        return -RT_EINVAL;

    if (rt_strcmp(argv[2], "all") == 0) {
        if (argc != 3)
            return -RT_EINVAL;

        disable_spi2axi = RT_TRUE;
        disable_jtag = RT_TRUE;
        force_secure_boot = RT_TRUE;
        disable_isp = RT_TRUE;
    } else {
        for (index = 2; index < argc; index++) {
            if (rt_strcmp(argv[index], "spi2axi") == 0) {
                disable_spi2axi = RT_TRUE;
            } else if (rt_strcmp(argv[index], "jtag") == 0) {
                disable_jtag = RT_TRUE;
            } else if (rt_strcmp(argv[index], "secure_boot") == 0) {
                force_secure_boot = RT_TRUE;
            } else if (rt_strcmp(argv[index], "isp") == 0) {
                disable_isp = RT_TRUE;
            } else {
                return -RT_EINVAL;
            }
        }
    }

    ret = pufs_drv_hw_lock();
    if (ret != 0)
        return ret;

    ret = pufs_otp_apply_security_config(disable_spi2axi != RT_FALSE,
                                         disable_jtag != RT_FALSE,
                                         force_secure_boot != RT_FALSE,
                                         disable_isp != RT_FALSE);
    pufs_drv_hw_unlock();

    if (ret != SUCCESS)
        return -ret;

    rt_kprintf("security config programmed\n");
    return 0;
}

static int pufs_otp_sec_lock(void)
{
    int ret;

    ret = pufs_drv_hw_lock();
    if (ret != 0)
        return ret;

    ret = pufs_otp_lock_security_config_words();
    pufs_drv_hw_unlock();

    if (ret != SUCCESS)
        return -ret;

    rt_kprintf("security config words locked RO\n");
    return 0;
}

static void do_pufs_otp_sec(int argc, char **argv)
{
    int ret;

    if (argc < 2) {
        pufs_otp_sec_usage();
        return;
    }

    if (rt_strcmp(argv[1], "query") == 0) {
        ret = pufs_otp_sec_query();
    } else if (rt_strcmp(argv[1], "write") == 0) {
        ret = pufs_otp_sec_write(argc, argv);
    } else if (rt_strcmp(argv[1], "lock") == 0) {
        if (argc != 2) {
            pufs_otp_sec_usage();
            return;
        }
        ret = pufs_otp_sec_lock();
    } else {
        pufs_otp_sec_usage();
        return;
    }

    if (ret == -RT_EINVAL) {
        pufs_otp_sec_usage();
        return;
    }

    if (ret != 0) {
        if (ret == -RT_ETIMEOUT) {
            rt_kprintf("pufs_otp_sec failed: hardware lock timeout (%d)\n", ret);
        } else {
            rt_kprintf("pufs_otp_sec failed: %s (%d)\n",
                       pufs_strstatus(-ret),
                       -ret);
        }
    }
}

MSH_CMD_EXPORT_ALIAS(do_pufs_otp_sec, pufs_otp_sec,
                     "PUFS OTP security config command:\n"
                     "  query\n"
                     "  write <jtag|secure_boot|isp|all> [more items...]\n"
                     "  write all\n"
                     "  lock");

MSH_CMD_EXPORT_ALIAS(do_pufs_otp, pufs_otp,
                     "PUFS OTP command:\n"
                     "  read <addr> <len>\n"
                     "  write <addr> <val0> [val1...]\n"
                     "  lock query <addr>\n"
                     "  lock set <addr> <len> <RO|RW>");

#endif

#if defined(RT_USING_MSH) && defined(RT_USING_PUFS_FILE_HASH)

#define PUFS_FILE_HASH_CHUNK_SIZE 4096

static pufs_hash_t pufs_file_hash_default_alg(void)
{
    return SHA_256;
}

static const char *pufs_file_hash_name(pufs_hash_t hash)
{
    switch (hash) {
    case SHA_224:
        return "sha224";
    case SHA_256:
        return "sha256";
    case SHA_384:
        return "sha384";
    case SHA_512:
        return "sha512";
    case SHA_512_224:
        return "sha512_224";
    case SHA_512_256:
        return "sha512_256";
    case SM3:
        return "sm3";
    default:
        return "unknown";
    }
}

static rt_err_t pufs_file_hash_parse_alg(const char *text, pufs_hash_t *hash)
{
    if (text == RT_NULL || hash == RT_NULL)
        return -RT_EINVAL;

    if (rt_strcmp(text, "sha224") == 0) {
        *hash = SHA_224;
    } else if (rt_strcmp(text, "sha256") == 0) {
        *hash = SHA_256;
    } else if (rt_strcmp(text, "sha384") == 0) {
        *hash = SHA_384;
    } else if (rt_strcmp(text, "sha512") == 0) {
        *hash = SHA_512;
    } else if (rt_strcmp(text, "sha512_224") == 0) {
        *hash = SHA_512_224;
    } else if (rt_strcmp(text, "sha512_256") == 0) {
        *hash = SHA_512_256;
    } else if (rt_strcmp(text, "sm3") == 0) {
        *hash = SM3;
    } else {
        return -RT_EINVAL;
    }

    return RT_EOK;
}

static void pufs_file_hash_usage(const char *cmd_name, rt_bool_t fixed_sha256)
{
    rt_kprintf("Usage:\n");
    if (cmd_name == RT_NULL)
        cmd_name = "pufs_file_hash";

    if (fixed_sha256) {
        rt_kprintf("  %s <path>\n", cmd_name);
        rt_kprintf("Notes:\n");
        rt_kprintf("  this alias always calculates sha256\n");
    } else {
        rt_kprintf("  %s <path> [sha224|sha256|sha384|sha512|sha512_224|sha512_256|sm3]\n",
                   cmd_name);
        rt_kprintf("Notes:\n");
        rt_kprintf("  default algorithm is sha256\n");
    }
}

static int pufs_file_hash_run(const char *path, pufs_hash_t hash)
{
    int fd;
    int ret;
    ssize_t read_len;
    uint8_t *buf;
    pufs_hash_ctx *ctx;
    pufs_dgst_st md;

    if (path == RT_NULL)
        return -RT_EINVAL;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;

    buf = rt_malloc(PUFS_FILE_HASH_CHUNK_SIZE);
    if (buf == RT_NULL) {
        close(fd);
        return -RT_ENOMEM;
    }

    ctx = pufs_hash_ctx_new();
    if (ctx == RT_NULL) {
        rt_free(buf);
        close(fd);
        return -RT_ENOMEM;
    }

    memset(&md, 0, sizeof(md));

    ret = pufs_drv_hw_lock();
    if (ret != 0)
        goto cleanup;

    ret = pufs_hash_init(ctx, hash);
    pufs_drv_hw_unlock();
    if (ret != SUCCESS) {
        ret = -ret;
        goto cleanup;
    }

    while ((read_len = read(fd, buf, PUFS_FILE_HASH_CHUNK_SIZE)) > 0) {
        ret = pufs_drv_hw_lock();
        if (ret != 0)
            goto cleanup;

        ret = pufs_hash_update(ctx, buf, (uint32_t)read_len);
        pufs_drv_hw_unlock();
        if (ret != SUCCESS) {
            ret = -ret;
            goto cleanup;
        }
    }

    if (read_len < 0) {
        ret = (int)read_len;
        goto cleanup;
    }

    ret = pufs_drv_hw_lock();
    if (ret != 0)
        goto cleanup;

    ret = pufs_hash_final(ctx, &md);
    pufs_drv_hw_unlock();
    if (ret != SUCCESS) {
        ret = -ret;
        goto cleanup;
    }

    for (uint32_t index = 0; index < md.dlen; index++)
        rt_kprintf("%02x", md.dgst[index]);
    rt_kprintf("  %s (%s)\n", path, pufs_file_hash_name(hash));
    ret = 0;

cleanup:
    pufs_hash_ctx_free(ctx);
    rt_free(buf);
    close(fd);
    return ret;
}

static void do_pufs_file_hash(int argc, char **argv)
{
    int ret;
    pufs_hash_t hash = pufs_file_hash_default_alg();
    const char *cmd_name = (argc > 0 && argv[0] != RT_NULL) ? argv[0] : "pufs_file_hash";
    rt_bool_t fixed_sha256 = (rt_strcmp(cmd_name, "sha256") == 0) ? RT_TRUE : RT_FALSE;

    if (fixed_sha256) {
        if (argc != 2) {
            pufs_file_hash_usage(cmd_name, fixed_sha256);
            return;
        }
        hash = SHA_256;
    } else {
        if (argc < 2 || argc > 3) {
            pufs_file_hash_usage(cmd_name, fixed_sha256);
            return;
        }

        if (argc == 3) {
            ret = pufs_file_hash_parse_alg(argv[2], &hash);
            if (ret != RT_EOK) {
                pufs_file_hash_usage(cmd_name, fixed_sha256);
                return;
            }
        }
    }

    ret = pufs_file_hash_run(argv[1], hash);
    if (ret == -RT_ETIMEOUT) {
        rt_kprintf("%s failed: hardware lock timeout (%d)\n", cmd_name, ret);
        return;
    }
    if (ret == -RT_ENOMEM) {
        rt_kprintf("%s failed: no memory (%d)\n", cmd_name, ret);
        return;
    }
    if (ret < 0) {
        rt_kprintf("%s failed: %d\n", cmd_name, ret);
    }
}

MSH_CMD_EXPORT_ALIAS(do_pufs_file_hash, sha256,
                     "PUFS sha256 command:\n"
                     "  <path>");

MSH_CMD_EXPORT_ALIAS(do_pufs_file_hash, pufs_file_hash,
                     "PUFS file hash command (compatibility):\n"
                     "  <path> [sha224|sha256|sha384|sha512|sha512_224|sha512_256|sm3]");

#endif
