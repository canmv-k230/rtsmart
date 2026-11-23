/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * PUF Secure Engine - Stateless per-operation kernel driver
 * All intermediate context lives in userspace HAL; kernel holds HW lock
 * only during individual operations.
 */

#include <rtthread.h>

#include <dfs_posix.h>
#include <board.h>
#include <cache.h>
#include <ioremap.h>
#include <lwp_user_mm.h>
#include <rtdbg.h>

#include "rvv_ops.h"

#include "drv_pufs.h"
#include "k230_pm.h"
#include "drv_pufs_internal.h"

#include "pufs_dma.h"
#include "pufs_ecp_internal.h"
#include "pufs_internal.h"
#include "pufs_rt.h"
#include "pufs_rt_internal.h"
#include "pufs_ka.h"
#include "pufs_ka_internal.h"
#include "pufs_crypto.h"
#include "pufs_crypto_internal.h"
#include "pufs_hmac.h"
#include "pufs_hmac_internal.h"
#include "pufs_cmac.h"
#include "pufs_cmac_internal.h"
#include "pufs_kdf.h"
#include "pufs_ecp.h"
#include "pufs_sp38a.h"
#include "pufs_sp38a_internal.h"
#include "pufs_sp38d.h"
#include "pufs_sp38d_internal.h"
#include "pufs_sp38e.h"
#include "pufs_sp38e_internal.h"
#include "pufs_sp38c.h"
#include "pufs_sp38c_internal.h"
#include "pufs_sp90a.h"
#include "pufs_sm2.h"

#define DBG_TAG "PUFS"
#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#define DBG_COLOR

/* HW address offsets */
#define DMA_ADDR_OFFSET         0x000
#define CRYPTO_ADDR_OFFSET      0x100
#define SP38A_ADDR_OFFSET       0x200
#define CMAC_ADDR_OFFSET        0x220
#define SP38C_ADDR_OFFSET       0x240
#define SP38D_ADDR_OFFSET       0x260
#define SP38E_ADDR_OFFSET       0x280
#define KWP_ADDR_OFFSET         0x300
#define CHACHA_ADDR_OFFSET      0x400
#define HMAC_HASH_ADDR_OFFSET   0x800
#define KDF_ADDR_OFFSET         0x900
#define SP90A_ADDR_OFFSET       0xB00
#define KA_ADDR_OFFSET          0xC00
#define PKC_ADDR_OFFSET         0x1000
#define RT_ADDR_OFFSET          0x3000
#define CDE_ADDR_OFFSET         0x4000

/* Reduced device struct — no session state */
struct pufs_device {
    struct rt_device dev;
    void *base;
    struct rt_mutex lock;
};

static struct pufs_device pufs_dev;
/* Set once pufs_device_init() has registered the device; the lock is valid
 * only then (init failures leave it uninitialized or detached). */
static bool pufs_ready;

/* Shared ioctl staging buffers, protected by pufs_dev.lock */
#define PUFS_DMA_BUF_SIZE  65536
static uint8_t *pufs_dma_in;
static uint8_t *pufs_dma_out;

/* ===== Per-operation HW lock helpers ===== */
static inline int pufs_lock_and_recover(void)
{
    rt_err_t ret;

    ret = rt_mutex_take(&pufs_dev.lock, rt_tick_from_millisecond(1000));
    if (ret != RT_EOK)
        return -EBUSY;

    if (rt_otp_guard_keyslots(0, true) != SUCCESS) {
        rt_mutex_release(&pufs_dev.lock);
        return -EIO;
    }
    return 0;
}

static inline int pufs_hw_lock(void)
{
    int ret = pufs_lock_and_recover();
    if (ret != 0)
        return ret;
    bool ecp_idle = pufs_ecp_is_idle();
    if (!pufs_dma_sg_is_idle() || !ecp_idle) {
        rt_mutex_release(&pufs_dev.lock);
        return -EBUSY;
    }
    return 0;
}

static inline void pufs_hw_unlock(void)
{
    rt_mutex_release(&pufs_dev.lock);
}

/*
 * For kernel callers (msh commands, bank voltage check, hwrng), which use
 * RT-Thread error codes: -RT_ENOSYS if the driver failed to initialize,
 * -RT_ETIMEOUT if the lock is not free within 1 s, -RT_EIO if the OTP
 * post-mask guard cannot re-mask after an earlier failure.
 * The ioctl path uses pufs_hw_lock(), which returns POSIX codes.
 */
int pufs_drv_hw_lock(void)
{
    if (!pufs_ready)
        return -RT_ENOSYS;

    int ret = pufs_lock_and_recover();

    if (ret == -EBUSY)
        return -RT_ETIMEOUT;
    if (ret != 0)
        return -RT_EIO;
    /* The kernel-only clients do not use ECP, but can wipe its operands. */
    (void)pufs_ecp_is_idle();
    return 0;
}

void pufs_drv_hw_unlock(void)
{
    pufs_hw_unlock();
}

bool pufs_drv_hw_lock_held(void)
{
    return pufs_dev.lock.owner == rt_thread_self();
}

/* ===== User/kernel data copy helpers ===== */
static int get_from(void *dst, void *src, size_t size)
{
    if (!dst || !src || !size) return 0;

    return lwp_get_from_user_ex(dst, src, size);
}

static int put_to(void *dst, void *src, size_t size)
{
    if (!dst || !src || !size) return 0;

    return lwp_put_to_user_ex(dst, src, size);
}

/* Ioctl copy-in: declare local var, copy from userspace, break on failure */
#define IOCTL_COPY_IN(type, var) \
    type var; \
    if (!args || get_from(&var, args, sizeof(var)) != 0) { ret = -EFAULT; break; }

/* Ioctl copy-back: write result to userspace on success, then clear the
 * kernel copy (contexts and requests can carry software keys). */
#define IOCTL_COPY_BACK(var) \
    do { \
        if (ret == 0 && put_to(args, &var, sizeof(var)) != 0) ret = -EFAULT; \
        rvv_memset(&var, 0, sizeof(var)); \
    } while (0)

/* ===================================================================
 * Context serialization: portable (drv_pufs.h) <-> internal (pufs lib)
 *
 * The _t types in drv_pufs.h use bool for boolean fields, matching the
 * internal types exactly. Pointer fields (crypto_io_ctx, phybuf_list)
 * exist only in the internal types and stay zero from handler rvv_memset.
 * Handlers copy with: rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx))
 * =================================================================== */

_Static_assert(sizeof(pufs_hash_ctx_t) <= sizeof(pufs_hmac_ctx),  "hash ctx size");
_Static_assert(sizeof(pufs_cmac_ctx_t) <= sizeof(pufs_cmac_ctx),  "cmac ctx size");
_Static_assert(sizeof(pufs_sp38a_ctx_t) <= sizeof(pufs_sp38a_ctx), "sp38a ctx size");
_Static_assert(sizeof(pufs_sp38d_ctx_t) <= sizeof(pufs_sp38d_ctx), "sp38d ctx size");
_Static_assert(sizeof(pufs_sp38c_ctx_t) <= sizeof(pufs_sp38c_ctx), "sp38c ctx size");
_Static_assert(sizeof(pufs_sp38e_ctx_t) <= sizeof(pufs_sp38e_ctx), "sp38e ctx size");

/* ===================================================================
 * Context validation
 *
 * Multi-step contexts live in user memory, so on every UPDATE and FINAL the
 * kernel gets back whatever the caller wrote. The library trusts these fields:
 * buflen, minlen and blocklen size copies into the context buffers (here on
 * the kernel stack), and keytype/keyslot/keybits select the key. Accept only
 * values init can produce, as far as memory safety and key policy depend on
 * them; other fields only affect the caller's own output.
 * =================================================================== */

/* segment() keeps fewer than minlen + blocksize bytes buffered, and copies
 * whole blocks into the buffer, so both must fit in @buffsize. */
static bool ctx_buffer_ok(uint32_t buflen, uint32_t minlen, uint32_t blocksize,
                          uint32_t buffsize)
{
    return blocksize != 0 && buffsize % blocksize == 0 &&
           minlen <= buffsize + 1 - blocksize && buflen < minlen + blocksize;
}

/* The same key rule as each mode's init: PUF and shared-secret keys are never
 * usable, a software key fits the context, any other key must pass
 * keyslot_check(). The userspace HAL builds hash/HMAC/CMAC contexts without
 * calling init, so this is the only check those keys get. */
static bool ctx_key_ok(uint32_t keytype, uint32_t keyslot, uint32_t keybits,
                       uint32_t swkey_bits)
{
    if (keytype == PUFKEY || keytype == SHARESEC)
        return false;
    if (keytype == SWKEY)
        return keybits <= swkey_bits;
    return keyslot_check(true, (pufs_key_type_t)keytype, keyslot, keybits) == SUCCESS;
}

static bool hmac_ctx_ok(const pufs_hmac_ctx *c)
{
    return (c->blocklen == 64 || c->blocklen == 128) &&
           ctx_buffer_ok(c->buflen, c->minlen, c->blocklen, sizeof(c->buff)) &&
           ctx_key_ok(c->keytype, c->keyslot, c->keybits, 8 * c->blocklen);
}

static bool cmac_ctx_ok(const pufs_cmac_ctx *c)
{
    return ctx_buffer_ok(c->buflen, c->minlen, CMAC_BLOCK_SIZE, sizeof(c->buff)) &&
           crypto_check_cmac_algo((pufs_cipher_t)c->cipher, c->keybits) == SUCCESS &&
           ctx_key_ok(c->keytype, c->keyslot, c->keybits, 8 * sizeof(c->key));
}

static bool sp38a_ctx_ok(const pufs_sp38a_ctx *c)
{
    return ctx_buffer_ok(c->buflen, c->minlen, PUFS_BC_BLOCK_SIZE, sizeof(c->buff)) &&
           crypto_check_sp38a_algo((pufs_cipher_t)c->cipher, c->keybits) == SUCCESS &&
           ctx_key_ok(c->keytype, c->keyslot, c->keybits, 8 * sizeof(c->key));
}

/* CCM buffers at most one block itself rather than through segment(). */
static bool sp38c_ctx_ok(const pufs_sp38c_ctx *c)
{
    return c->op == SP38C_CCM && (unsigned)c->stage <= SP38C_TEXT &&
           c->currentlen <= c->inlen && c->buflen <= sizeof(c->buff) &&
           c->taglen >= 4 && c->taglen <= PUFS_BC_BLOCK_SIZE && !(c->taglen & 1) &&
           c->qlen >= 2 && c->qlen <= 8 &&
           crypto_check_ccm_algo((pufs_cipher_t)c->cipher, c->keybits) == SUCCESS &&
           ctx_key_ok(c->keytype, c->keyslot, c->keybits, 8 * sizeof(c->key));
}

static bool sp38d_ctx_ok(const pufs_sp38d_ctx *c)
{
    return ctx_buffer_ok(c->buflen, c->minlen, PUFS_BC_BLOCK_SIZE, sizeof(c->buff)) &&
           crypto_check_sp38d_algo((pufs_cipher_t)c->cipher, c->keybits) == SUCCESS &&
           ctx_key_ok(c->keytype, c->keyslot, c->keybits, 8 * sizeof(c->key));
}

static bool sp38e_ctx_ok(const pufs_sp38e_ctx *c)
{
    return ctx_buffer_ok(c->buflen, c->minlen, PUFS_BC_BLOCK_SIZE, sizeof(c->buff)) &&
           crypto_check_sp38e_algo((pufs_cipher_t)c->cipher, c->keybits) == SUCCESS &&
           ctx_key_ok(c->keytype1, c->keyslot1, c->keybits, 8 * sizeof(c->key1)) &&
           ctx_key_ok(c->keytype2, c->keyslot2, c->keybits, 8 * sizeof(c->key2));
}

/* ===================================================================
 * SP38A init dispatcher (mode -> specific init function)
 * =================================================================== */

typedef pufs_status_t (*sp38a_update_fn)(pufs_sp38a_ctx*, uint8_t*, uint32_t*, const uint8_t*, uint32_t);
typedef pufs_status_t (*sp38a_final_fn)(pufs_sp38a_ctx*, uint8_t*, uint32_t*);

static void sp38a_get_ops(const pufs_sp38a_ctx *ctx, sp38a_update_fn *up, sp38a_final_fn *fin)
{
    sp38a_op op = ctx->op;
    bool enc = ctx->encrypt;

    switch (op) {
    case SP38A_ECB_CLR:
        *up  = enc ? pufs_enc_ecb_update : pufs_dec_ecb_update;
        *fin = enc ? pufs_enc_ecb_final  : pufs_dec_ecb_final;
        break;
    case SP38A_CFB_CLR:
        *up  = enc ? pufs_enc_cfb_update : pufs_dec_cfb_update;
        *fin = enc ? pufs_enc_cfb_final  : pufs_dec_cfb_final;
        break;
    case SP38A_OFB:
        *up  = enc ? pufs_enc_ofb_update : pufs_dec_ofb_update;
        *fin = enc ? pufs_enc_ofb_final  : pufs_dec_ofb_final;
        break;
    case SP38A_CBC_CLR:
    case SP38A_CBC_CS1:
    case SP38A_CBC_CS2:
    case SP38A_CBC_CS3:
        *up  = enc ? pufs_enc_cbc_update : pufs_dec_cbc_update;
        *fin = enc ? pufs_enc_cbc_final  : pufs_dec_cbc_final;
        break;
    case SP38A_CTR_32:
    case SP38A_CTR_64:
    case SP38A_CTR_128:
        *up  = enc ? pufs_enc_ctr_update : pufs_dec_ctr_update;
        *fin = enc ? pufs_enc_ctr_final  : pufs_dec_ctr_final;
        break;
    default:
        *up = NULL; *fin = NULL;
    }
}

static bool cipher_key_addr(pufs_key_type_t keytype, const uint8_t *bytes,
                            const pufs_cipher_slot_t *hwkey, size_t *keyaddr)
{
    if (keytype == SWKEY) {
        *keyaddr = (uintptr_t)bytes;
        return true;
    }

    /* RANDKEY uses the RNG directly and has no slot selector. */
    if (keytype == RANDKEY) {
        *keyaddr = 0;
        return true;
    }

    /* A legacy hardware-key request did not supply a slot. Do not use slot 0. */
    if (hwkey->present != PUFS_CIPHER_SLOT_PRESENT)
        return false;
    *keyaddr = hwkey->slot;
    return true;
}

static int sp38a_do_init(pufs_sp38a_ctx *ctx, const pufs_sp38a_op_t *op)
{
    pufs_cipher_t cipher = (pufs_cipher_t)op->cipher;
    pufs_key_type_t keytype = (pufs_key_type_t)op->keytype;
    size_t keyaddr;
    uint32_t keybits = op->keybits;
    const uint8_t *iv = op->iv;
    uint32_t ivlen = op->ivlen;
    uint8_t mode = op->mode;
    int encrypt = op->encrypt;

    if (!cipher_key_addr(keytype, op->key, &op->hwkey, &keyaddr))
        return -EINVAL;

    if (mode == MODE_ECB) {
        return encrypt ? pufs_enc_ecb_init(ctx, cipher, keytype, keyaddr, keybits)
                       : pufs_dec_ecb_init(ctx, cipher, keytype, keyaddr, keybits);
    } else if (mode == MODE_CFB) {
        return encrypt ? pufs_enc_cfb_init(ctx, cipher, keytype, keyaddr, keybits, (uint8_t*)iv)
                       : pufs_dec_cfb_init(ctx, cipher, keytype, keyaddr, keybits, (uint8_t*)iv);
    } else if (mode == MODE_OFB) {
        return encrypt ? pufs_enc_ofb_init(ctx, cipher, keytype, keyaddr, keybits, (uint8_t*)iv)
                       : pufs_dec_ofb_init(ctx, cipher, keytype, keyaddr, keybits, (uint8_t*)iv);
    } else if (mode >= MODE_CBC && mode <= MODE_CBC_CS3) {
        int cs = mode - MODE_CBC;
        return encrypt ? pufs_enc_cbc_init(ctx, cipher, keytype, keyaddr, keybits, (uint8_t*)iv, cs)
                       : pufs_dec_cbc_init(ctx, cipher, keytype, keyaddr, keybits, (uint8_t*)iv, cs);
    } else if (mode >= MODE_CTR_32 && mode <= MODE_CTR) {
        int ctrlen = mode == MODE_CTR_32 ? 32 : mode == MODE_CTR_64 ? 64 : 128;
        return encrypt ? pufs_enc_ctr_init(ctx, cipher, keytype, keyaddr, keybits, (uint8_t*)iv, ctrlen)
                       : pufs_dec_ctr_init(ctx, cipher, keytype, keyaddr, keybits, (uint8_t*)iv, ctrlen);
    }
    return -EINVAL;
}

/* ===================================================================
 * Streaming operation handlers
 * Pattern: lock HW -> stack ctx -> from_user -> operate -> to_user -> unlock HW
 * =================================================================== */

static int hash_op_handler(pufs_hash_op_t *op)
{
    int ret;
    pufs_hmac_ctx ctx_stack;
    pufs_hmac_ctx *ctx = &ctx_stack;
    void *msg_user;
    void *dgst_user;

    rvv_memset(ctx, 0, sizeof(*ctx));
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
    if (op->op != PUFS_OP_INIT && !hmac_ctx_ok(ctx)) {
        rvv_memset(ctx, 0, sizeof(*ctx));
        pufs_hw_unlock();
        return -EINVAL;
    }
    msg_user = (void *)(uintptr_t)op->msg_phys;
    dgst_user = (void *)(uintptr_t)op->dgst_phys;

    switch (op->op) {
    case PUFS_OP_INIT:
        ret = pufs_hash_init(ctx, (pufs_hash_t)op->hash);
        break;
    case PUFS_OP_UPDATE: {
        if (op->msglen > PUFS_DMA_BUF_SIZE || !msg_user || get_from(pufs_dma_in, msg_user, op->msglen) != 0) {
            ret = -EINVAL;
            break;
        }
        ret = pufs_hash_update(ctx, pufs_dma_in, op->msglen);
        break;
    }
    case PUFS_OP_FINAL: {
        pufs_dgst_st md;
        ret = pufs_hash_final(ctx, &md);
        if (ret == SUCCESS) {
            if (dgst_user && put_to(dgst_user, md.dgst, md.dlen) != 0) {
                ret = -EINVAL;
                break;
            }
            op->dlen = md.dlen;
        }
        break;
    }
    default:
        ret = -EINVAL;
    }

    if (ret == SUCCESS) {
        rvv_memcpy(&op->ctx, ctx, sizeof(op->ctx));
    }

    rvv_memset(ctx, 0, sizeof(*ctx));
    pufs_hw_unlock();
    return (ret > 0) ? -(int)ret : ret;
}

static int hmac_op_handler(pufs_hmac_op_t *op)
{
    int ret;
    pufs_hmac_ctx ctx_stack;
    pufs_hmac_ctx *ctx = &ctx_stack;
    void *msg_user;
    void *dgst_user;

    rvv_memset(ctx, 0, sizeof(*ctx));
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
    /* Older HALs initialize a fresh wide-hash HMAC with minlen=128. */
    if (ctx->op == HMAC_HMAC && ctx->blocklen == 128 && ctx->minlen == 128 &&
        ctx->hash >= SHA_384 && ctx->hash <= SHA_512_256 &&
        !ctx->start && !ctx->curlen && !ctx->buflen)
        ctx->minlen = 1;
    if (op->op != PUFS_OP_INIT && !hmac_ctx_ok(ctx)) {
        rvv_memset(ctx, 0, sizeof(*ctx));
        pufs_hw_unlock();
        return -EINVAL;
    }
    msg_user = (void *)(uintptr_t)op->msg_phys;
    dgst_user = (void *)(uintptr_t)op->dgst_phys;

    switch (op->op) {
    case PUFS_OP_INIT:
        /* Init reads (and hashes, if longer than a block) b2B(keybits) bytes
         * of a software key from ctx->key. */
        if (op->keytype == SWKEY && op->keybits > 8 * sizeof(ctx->key)) {
            ret = -EINVAL;
            break;
        }
        ret = pufs_hmac_init(ctx, (pufs_hash_t)op->hash,
                             (pufs_key_type_t)op->keytype,
                             op->keytype == SWKEY ? (size_t)op->ctx.key : op->ctx.keyslot, op->keybits);
        break;
    case PUFS_OP_UPDATE: {
        if (op->msglen > PUFS_DMA_BUF_SIZE || !msg_user || get_from(pufs_dma_in, msg_user, op->msglen) != 0) {
            ret = -EINVAL;
            break;
        }
        ret = pufs_hmac_update(ctx, pufs_dma_in, op->msglen);
        break;
    }
    case PUFS_OP_FINAL: {
        pufs_dgst_st md;
        ret = pufs_hmac_final(ctx, &md);
        if (ret == SUCCESS) {
            if (dgst_user && put_to(dgst_user, md.dgst, md.dlen) != 0) {
                ret = -EINVAL;
                break;
            }
            op->dlen = md.dlen;
        }
        break;
    }
    default:
        ret = -EINVAL;
    }

    if (ret == SUCCESS) {
        rvv_memcpy(&op->ctx, ctx, sizeof(op->ctx));
    }

    rvv_memset(ctx, 0, sizeof(*ctx));
    pufs_hw_unlock();
    return (ret > 0) ? -(int)ret : ret;
}

static int cmac_op_handler(pufs_cmac_op_t *op)
{
    int ret;
    pufs_cmac_ctx ctx_stack;
    pufs_cmac_ctx *ctx = &ctx_stack;
    void *msg_user;
    void *dgst_user;

    rvv_memset(ctx, 0, sizeof(*ctx));
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
    if (op->op != PUFS_OP_INIT && !cmac_ctx_ok(ctx)) {
        rvv_memset(ctx, 0, sizeof(*ctx));
        pufs_hw_unlock();
        return -EINVAL;
    }
    msg_user = (void *)(uintptr_t)op->msg_phys;
    dgst_user = (void *)(uintptr_t)op->dgst_phys;

    switch (op->op) {
    case PUFS_OP_INIT:
        ret = pufs_cmac_init(ctx, (pufs_cipher_t)op->cipher,
                             (pufs_key_type_t)op->keytype,
                             op->keytype == SWKEY ? (size_t)op->ctx.key : op->ctx.keyslot, op->keybits);
        break;
    case PUFS_OP_UPDATE: {
        if (op->msglen > PUFS_DMA_BUF_SIZE || !msg_user || get_from(pufs_dma_in, msg_user, op->msglen) != 0) {
            ret = -EINVAL;
            break;
        }
        ret = pufs_cmac_update(ctx, pufs_dma_in, op->msglen);
        break;
    }
    case PUFS_OP_FINAL: {
        pufs_dgst_st md;
        ret = pufs_cmac_final(ctx, &md);
        if (ret == SUCCESS) {
            if (dgst_user && put_to(dgst_user, md.dgst, md.dlen) != 0) {
                ret = -EINVAL;
                break;
            }
            op->dlen = md.dlen;
        }
        break;
    }
    default:
        ret = -EINVAL;
    }

    if (ret == SUCCESS) {
        rvv_memcpy(&op->ctx, ctx, sizeof(op->ctx));
    }

    rvv_memset(ctx, 0, sizeof(*ctx));
    pufs_hw_unlock();
    return (ret > 0) ? -(int)ret : ret;
}

static int sp38a_op_handler(pufs_sp38a_op_t *op)
{
    int ret;
    pufs_sp38a_ctx ctx_stack;
    pufs_sp38a_ctx *ctx = &ctx_stack;
    void *in_user = (void *)(uintptr_t)op->in_phys;
    void *out_user = (void *)(uintptr_t)op->out_phys;

    rvv_memset(ctx, 0, sizeof(*ctx));
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    switch (op->op) {
    case PUFS_OP_INIT:
        ret = sp38a_do_init(ctx, op);
        break;
    case PUFS_OP_UPDATE: {
        sp38a_update_fn update_fn;
        sp38a_final_fn final_fn;
        uint8_t *inbuf;
        uint8_t *outbuf;

        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38a_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        sp38a_get_ops(ctx, &update_fn, &final_fn);
        if (!update_fn) { ret = -EINVAL; break; }
        if (op->inlen > PUFS_DMA_BUF_SIZE || !in_user || get_from(pufs_dma_in, in_user, op->inlen) != 0) {
            ret = -EINVAL;
            break;
        }
        inbuf = pufs_dma_in;
        outbuf = out_user ? pufs_dma_out : NULL;
        ret = update_fn(ctx, outbuf, &op->outlen, inbuf, op->inlen);
        if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
            ret = -EINVAL;
        break;
    }
    case PUFS_OP_FINAL: {
        sp38a_update_fn update_fn;
        sp38a_final_fn final_fn;
        uint8_t *outbuf;

        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38a_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        sp38a_get_ops(ctx, &update_fn, &final_fn);
        if (!final_fn) { ret = -EINVAL; break; }
        outbuf = out_user ? pufs_dma_out : NULL;
        ret = final_fn(ctx, outbuf, &op->outlen);
        if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
            ret = -EINVAL;
        break;
    }
    default:
        ret = -EINVAL;
    }

    if (ret == SUCCESS) {
        rvv_memcpy(&op->ctx, ctx, sizeof(op->ctx));
    }

    rvv_memset(ctx, 0, sizeof(*ctx));
    pufs_hw_unlock();
    return (ret > 0) ? -(int)ret : ret;
}

static int sp38d_do_init(pufs_sp38d_ctx *ctx, pufs_sp38d_op_t *op)
{
    pufs_cipher_t cipher = (pufs_cipher_t)op->cipher;
    pufs_key_type_t keytype = (pufs_key_type_t)op->keytype;
    size_t keyaddr;

    if (op->ivlen > sizeof(op->iv) ||
        !cipher_key_addr(keytype, op->key, &op->hwkey, &keyaddr))
        return -EINVAL;

    return op->encrypt ? pufs_enc_gcm_init(ctx, cipher, keytype, keyaddr,
                                          op->keybits, op->iv, op->ivlen)
                       : pufs_dec_gcm_init(ctx, cipher, keytype, keyaddr,
                                          op->keybits, op->iv, op->ivlen);
}

static int sp38d_op_handler(pufs_sp38d_op_t *op)
{
    int ret;
    pufs_sp38d_ctx ctx_stack;
    pufs_sp38d_ctx *ctx = &ctx_stack;
    void *in_user = (void *)(uintptr_t)op->in_phys;
    void *out_user = (void *)(uintptr_t)op->out_phys;
    void *tag_user = (void *)(uintptr_t)op->tag_phys;

    rvv_memset(ctx, 0, sizeof(*ctx));
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    switch (op->op) {
    case PUFS_OP_INIT:
        ret = sp38d_do_init(ctx, op);
        break;
    case PUFS_OP_UPDATE: {
        uint8_t *inbuf;
        uint8_t *outbuf;

        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38d_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        if (op->inlen > PUFS_DMA_BUF_SIZE || !in_user || get_from(pufs_dma_in, in_user, op->inlen) != 0) {
            ret = -EINVAL;
            break;
        }
        inbuf = pufs_dma_in;
        outbuf = out_user ? pufs_dma_out : NULL;
        if (ctx->encrypt)
            ret = pufs_enc_gcm_update(ctx, outbuf, &op->outlen, inbuf, op->inlen);
        else
            ret = pufs_dec_gcm_update(ctx, outbuf, &op->outlen, inbuf, op->inlen);
        if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
            ret = -EINVAL;
        break;
    }
    case PUFS_OP_FINAL: {
        uint8_t expected_tag[PUFS_BC_BLOCK_SIZE];
        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38d_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        uint8_t tag[PUFS_BC_BLOCK_SIZE];
        uint8_t *outbuf = out_user ? pufs_dma_out : NULL;
        if (op->taglen > PUFS_BC_BLOCK_SIZE) {
            ret = -EINVAL;
            break;
        }
        if (ctx->encrypt) {
            ret = pufs_enc_gcm_final(ctx, outbuf, &op->outlen, tag, op->taglen);
        } else {
            if (op->taglen > 0 && (!tag_user || get_from(expected_tag, tag_user, op->taglen) != 0)) {
                ret = -EINVAL;
                break;
            }
            ret = pufs_dec_gcm_final_tag(ctx, outbuf, &op->outlen, tag, op->taglen);
            if (ret == SUCCESS && memcmp(tag, expected_tag, op->taglen) != 0)
                ret = E_VERFAIL;
        }
        if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
            ret = -EINVAL;
        if (ret == SUCCESS && ctx->encrypt && op->taglen > 0 && tag_user && put_to(tag_user, tag, op->taglen) != 0)
            ret = -EINVAL;
        break;
    }
    case PUFS_OP_INIT_UPDATE: {
        ret = sp38d_do_init(ctx, op);
        if (ret != SUCCESS) break;
        /* Update */
        op->outlen = 0;
        if (op->inlen > 0) {
            uint8_t *inbuf;
            uint8_t *outbuf;
            if (op->inlen > PUFS_DMA_BUF_SIZE || !in_user || get_from(pufs_dma_in, in_user, op->inlen) != 0) {
                ret = -EINVAL;
                break;
            }
            inbuf = pufs_dma_in;
            outbuf = out_user ? pufs_dma_out : NULL;
            if (ctx->encrypt)
                ret = pufs_enc_gcm_update(ctx, outbuf, &op->outlen, inbuf, op->inlen);
            else
                ret = pufs_dec_gcm_update(ctx, outbuf, &op->outlen, inbuf, op->inlen);
            if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
                ret = -EINVAL;
        }
        break;
    }
    case PUFS_OP_UPDATE_FINAL: {
        uint32_t update_outlen = 0;
        uint32_t final_outlen = 0;
        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38d_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        /* Update */
        if (op->inlen > 0) {
            uint8_t *outbuf;
            if (op->inlen > PUFS_DMA_BUF_SIZE || !in_user || get_from(pufs_dma_in, in_user, op->inlen) != 0) {
                ret = -EINVAL;
                break;
            }
            outbuf = out_user ? pufs_dma_out : NULL;
            if (ctx->encrypt)
                ret = pufs_enc_gcm_update(ctx, outbuf, &update_outlen, pufs_dma_in, op->inlen);
            else
                ret = pufs_dec_gcm_update(ctx, outbuf, &update_outlen, pufs_dma_in, op->inlen);
            if (ret != SUCCESS) break;
            if (out_user && update_outlen > 0 && put_to(out_user, outbuf, update_outlen) != 0) {
                ret = -EINVAL;
                break;
            }
        }
        /* Final */
        {
            uint8_t tag[PUFS_BC_BLOCK_SIZE];
            uint8_t *outbuf = out_user ? pufs_dma_out : NULL;
            if (op->taglen > PUFS_BC_BLOCK_SIZE) {
                ret = -EINVAL;
                break;
            }
            if (ctx->encrypt) {
                ret = pufs_enc_gcm_final(ctx, outbuf, &final_outlen, tag, op->taglen);
            } else {
                uint8_t expected_tag[PUFS_BC_BLOCK_SIZE];
                if (op->taglen > 0 && (!tag_user || get_from(expected_tag, tag_user, op->taglen) != 0)) {
                    ret = -EINVAL;
                    break;
                }
                ret = pufs_dec_gcm_final_tag(ctx, outbuf, &final_outlen, tag, op->taglen);
                if (ret == SUCCESS && memcmp(tag, expected_tag, op->taglen) != 0)
                    ret = E_VERFAIL;
            }
            if (ret == SUCCESS && out_user && final_outlen > 0 &&
                put_to((uint8_t*)out_user + update_outlen, outbuf, final_outlen) != 0)
                ret = -EINVAL;
            if (ret == SUCCESS && ctx->encrypt && op->taglen > 0 && tag_user && put_to(tag_user, tag, op->taglen) != 0)
                ret = -EINVAL;
        }
        op->outlen = update_outlen + final_outlen;
        break;
    }
    default:
        ret = -EINVAL;
    }

    if (ret == SUCCESS || (op->op == PUFS_OP_FINAL) || (op->op == PUFS_OP_UPDATE_FINAL)) {
        rvv_memcpy(&op->ctx, ctx, sizeof(op->ctx));
    }

    rvv_memset(ctx, 0, sizeof(*ctx));
    pufs_hw_unlock();
    return (ret > 0) ? -(int)ret : ret;
}

static int sp38c_op_handler(pufs_sp38c_op_t *op)
{
    int ret;
    pufs_sp38c_ctx ctx_stack;
    pufs_sp38c_ctx *ctx = &ctx_stack;
    void *in_user = (void *)(uintptr_t)op->in_phys;
    void *out_user = (void *)(uintptr_t)op->out_phys;
    void *tag_user = (void *)(uintptr_t)op->tag_phys;

    rvv_memset(ctx, 0, sizeof(*ctx));
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    switch (op->op) {
    case PUFS_OP_INIT: {
        pufs_key_type_t keytype = (pufs_key_type_t)op->keytype;
        size_t keyaddr;

        if (!cipher_key_addr(keytype, op->key, &op->hwkey, &keyaddr)) {
            ret = -EINVAL;
            break;
        }
        if (op->encrypt)
            ret = pufs_enc_ccm_init(ctx, (pufs_cipher_t)op->cipher,
                                    keytype, keyaddr, op->keybits,
                                    (uint8_t*)op->nonce, op->noncelen,
                                    op->ccm_aadlen, op->ccm_inlen, op->taglen);
        else
            ret = pufs_dec_ccm_init(ctx, (pufs_cipher_t)op->cipher,
                                    keytype, keyaddr, op->keybits,
                                    (uint8_t*)op->nonce, op->noncelen,
                                    op->ccm_aadlen, op->ccm_inlen, op->taglen);
        break;
    }
    case PUFS_OP_UPDATE: {
        uint8_t *inbuf;
        uint8_t *outbuf;

        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38c_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        if (op->inlen > PUFS_DMA_BUF_SIZE || !in_user || get_from(pufs_dma_in, in_user, op->inlen) != 0) {
            ret = -EINVAL;
            break;
        }
        inbuf = pufs_dma_in;
        outbuf = out_user ? pufs_dma_out : NULL;
        if (ctx->encrypt)
            ret = pufs_enc_ccm_update(ctx, outbuf, &op->outlen, inbuf, op->inlen);
        else
            ret = pufs_dec_ccm_update(ctx, outbuf, &op->outlen, inbuf, op->inlen);
        if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
            ret = -EINVAL;
        break;
    }
    case PUFS_OP_FINAL: {
        uint8_t expected_tag[PUFS_BC_BLOCK_SIZE];
        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38c_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        uint8_t tag[PUFS_BC_BLOCK_SIZE];
        uint8_t *outbuf = out_user ? pufs_dma_out : NULL;
        if (ctx->taglen > PUFS_BC_BLOCK_SIZE) {
            ret = -EINVAL;
            break;
        }
        /* Outside the AAD stage a buffered partial block is processed into
         * outbuf (and read back from it when decrypting): it must exist. */
        if (ctx->stage != SP38C_AAD && ctx->buflen > 0 && !outbuf) {
            ret = -EINVAL;
            break;
        }
        if (ctx->encrypt) {
            ret = pufs_enc_ccm_final(ctx, outbuf, &op->outlen, tag);
        } else {
            if (ctx->taglen > 0 && (!tag_user || get_from(expected_tag, tag_user, ctx->taglen) != 0)) {
                ret = -EINVAL;
                break;
            }
            ret = pufs_dec_ccm_final_tag(ctx, outbuf, &op->outlen, tag);
            if (ret == SUCCESS && memcmp(tag, expected_tag, ctx->taglen) != 0)
                ret = E_VERFAIL;
        }
        if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
            ret = -EINVAL;
        if (ret == SUCCESS && ctx->encrypt && ctx->taglen > 0 && tag_user && put_to(tag_user, tag, ctx->taglen) != 0)
            ret = -EINVAL;
        break;
    }
    default:
        ret = -EINVAL;
    }

    if (ret == SUCCESS || (op->op == PUFS_OP_FINAL)) {
        rvv_memcpy(&op->ctx, ctx, sizeof(op->ctx));
    }

    rvv_memset(ctx, 0, sizeof(*ctx));
    pufs_hw_unlock();
    return (ret > 0) ? -(int)ret : ret;
}

static int sp38e_op_handler(pufs_sp38e_op_t *op)
{
    int ret;
    pufs_sp38e_ctx ctx_stack;
    pufs_sp38e_ctx *ctx = &ctx_stack;
    void *in_user = (void *)(uintptr_t)op->in_phys;
    void *out_user = (void *)(uintptr_t)op->out_phys;

    rvv_memset(ctx, 0, sizeof(*ctx));
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    switch (op->op) {
    case PUFS_OP_INIT: {
        pufs_key_type_t keytype1 = (pufs_key_type_t)op->keytype1;
        pufs_key_type_t keytype2 = (pufs_key_type_t)op->keytype2;
        size_t keyaddr1, keyaddr2;

        if (!cipher_key_addr(keytype1, op->key1, &op->hwkey1, &keyaddr1) ||
            !cipher_key_addr(keytype2, op->key2, &op->hwkey2, &keyaddr2)) {
            ret = -EINVAL;
            break;
        }
        if (op->encrypt)
            ret = pufs_enc_xts_init(ctx, (pufs_cipher_t)op->cipher,
                                    keytype1, keyaddr1, op->keybits,
                                    keytype2, keyaddr2, (uint8_t*)op->iv, 0);
        else
            ret = pufs_dec_xts_init(ctx, (pufs_cipher_t)op->cipher,
                                    keytype1, keyaddr1, op->keybits,
                                    keytype2, keyaddr2, (uint8_t*)op->iv, 0);
        break;
    }
    case PUFS_OP_UPDATE: {
        uint8_t *inbuf;
        uint8_t *outbuf;

        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38e_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        if (op->inlen > PUFS_DMA_BUF_SIZE || !in_user || get_from(pufs_dma_in, in_user, op->inlen) != 0) {
            ret = -EINVAL;
            break;
        }
        inbuf = pufs_dma_in;
        outbuf = out_user ? pufs_dma_out : NULL;
        if (ctx->encrypt)
            ret = pufs_enc_xts_update(ctx, outbuf, &op->outlen, inbuf, op->inlen);
        else
            ret = pufs_dec_xts_update(ctx, outbuf, &op->outlen, inbuf, op->inlen);
        if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
            ret = -EINVAL;
        break;
    }
    case PUFS_OP_FINAL: {
        rvv_memcpy(ctx, &op->ctx, sizeof(op->ctx));
        if (!sp38e_ctx_ok(ctx)) {
            ret = -EINVAL;
            break;
        }
        uint8_t *outbuf = out_user ? pufs_dma_out : NULL;
        if (ctx->encrypt)
            ret = pufs_enc_xts_final(ctx, outbuf, &op->outlen);
        else
            ret = pufs_dec_xts_final(ctx, outbuf, &op->outlen);
        if (ret == SUCCESS && out_user && op->outlen > 0 && put_to(out_user, outbuf, op->outlen) != 0)
            ret = -EINVAL;
        break;
    }
    default:
        ret = -EINVAL;
    }

    if (ret == SUCCESS) {
        rvv_memcpy(&op->ctx, ctx, sizeof(op->ctx));
    }

    rvv_memset(ctx, 0, sizeof(*ctx));
    pufs_hw_unlock();
    return (ret > 0) ? -(int)ret : ret;
}

/* ===================================================================
 * Atomic operation handlers (lock per operation)
 * =================================================================== */

static int uid_get(pufs_uid_get_t *arg)
{
    int ret;
    pufs_uid_st uid;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_get_uid(&uid, arg->slot);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return put_to((void *)(uintptr_t)arg->uid_phys, &uid, sizeof(uid));
}

static int otp_read_handler(pufs_otp_rw_t *arg)
{
    int ret;
    uint8_t tmp[OTP_LEN];
    uint32_t len = arg->len;

    if (len > OTP_LEN || !arg->buf)
        return -EINVAL;

    /* Hold off writers so a multi-word read is never half old, half new. */
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_otp_read(tmp, len, arg->addr);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    } else {
        ret = put_to(arg->buf, tmp, len);
    }
    /* OTP words can be key material. */
    rvv_memset(tmp, 0, sizeof(tmp));
    return ret;
}

static int otp_write_handler(pufs_otp_rw_t *arg)
{
    int ret;
    uint8_t tmp[OTP_LEN];
    uint32_t len = arg->len;

    if (len > OTP_LEN || !arg->buf)
        return -EINVAL;

    if (get_from(tmp, arg->buf, len) != 0) {
        ret = -EFAULT;
        goto out;
    }

    ret = pufs_hw_lock();
    if (ret != 0)
        goto out;
    ret = pufs_otp_write(tmp, len, arg->addr);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    }
out:
    /* The data being burned can be a key. */
    rvv_memset(tmp, 0, sizeof(tmp));
    return ret;
}

static int otp_lock_handler(pufs_otp_lock_op_t *arg)
{
    int ret;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_otp_set_lock(arg->addr, arg->len, (pufs_otp_lock_t)arg->lock);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

/* pufs_rng_fill() lives in drv_hwrng.c with /dev/hwrng. */
static int rng_read_handler(pufs_rng_read_t *arg)
{
    uint32_t len = arg->len;
    uint8_t tmp[256];
    uint32_t off = 0;
    int ret = 0;

    if (!arg->buf || len == 0)
        return -EINVAL;

    while (ret == 0 && off < len) {
        uint32_t chunk = len - off;
        if (chunk > sizeof(tmp))
            chunk = sizeof(tmp);
        if ((ret = pufs_rng_fill(tmp, chunk)) == 0 &&
            put_to(arg->buf + off, tmp, chunk) != 0)
            ret = -EFAULT;
        off += chunk;
    }
    /* Do not leave random bytes (possibly key material) on the stack. */
    rvv_memset(tmp, 0, sizeof(tmp));
    return ret;
}

/* The ioctl result carries the query status; arg->lock is valid only on 0. */
static int otp_rwlck_get_handler(pufs_otp_rwlck_get_t *arg)
{
    int ret;
    pufs_otp_lock_t lock = N_OTP_LOCK_T;

    arg->lock = (uint8_t)N_OTP_LOCK_T;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_otp_query_lock(arg->addr, &lock);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    arg->lock = (uint8_t)lock;
    return 0;
}

static int rt_version_handler(pufs_rt_version_t *arg)
{
    int ret;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_rt_version(&arg->version, &arg->features);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int key2otp_handler(pufs_key2otp_t *arg)
{
    int ret;
    uint8_t keytmp[OTP_KEY_LEN * 8]; /* max key: 2047 bits = ~256 bytes */
    uint32_t keybytes = (arg->keybits + 7) >> 3;

    if (!arg->key || keybytes > sizeof(keytmp))
        return -EINVAL;

    if (get_from(keytmp, arg->key, keybytes) != 0) {
        ret = -EFAULT;
        goto out;
    }

    ret = pufs_hw_lock();
    if (ret != 0)
        goto out;
    ret = pufs_program_key2otp(arg->slot, keytmp, arg->keybits,
                               (pufs_otp_lock_t)arg->lock);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    }
out:
    /* The key being burned is secret, on every path. */
    rvv_memset(keytmp, 0, sizeof(keytmp));
    return ret;
}

static int otp_sec_cfg_handler(pufs_otp_security_cfg_t *arg)
{
    int ret;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_otp_apply_security_config(arg->disable_spi2axi != 0,
                                         arg->disable_jtag != 0,
                                         arg->force_secure_boot != 0,
                                         arg->disable_isp != 0);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int otp_sec_lock_handler(pufs_otp_security_cfg_t *arg)
{
    int ret;

    (void)arg;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_otp_lock_security_config_words();
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int otp_sec_state_handler(pufs_otp_security_state_t *arg)
{
    int ret;
    pufs_otp_security_state_st state;

    rvv_memset(&state, 0, sizeof(state));

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_otp_get_security_config_state(&state);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }

    arg->disable_spi2axi = state.disable_spi2axi;
    arg->disable_jtag = state.disable_jtag;
    arg->force_secure_boot = state.force_secure_boot;
    arg->disable_isp = state.disable_isp;
    arg->spi2axi_word_lock = state.spi2axi_word_lock;
    arg->jtag_word_lock = state.jtag_word_lock;
    arg->boot_ctrl_word_lock = state.boot_ctrl_word_lock;
    arg->reserved0 = 0;

    return 0;
}

static int drbg_init_handler(pufs_drbg_init_t *arg)
{
    int ret;
    uint8_t nonce_tmp[64];
    uint8_t pstr_tmp[64];
    uint32_t noncelen = arg->noncelen > sizeof(nonce_tmp) ? sizeof(nonce_tmp) : arg->noncelen;
    uint32_t pstrlen = arg->pstrlen > sizeof(pstr_tmp) ? sizeof(pstr_tmp) : arg->pstrlen;

    if (arg->nonce && noncelen &&
        get_from(nonce_tmp, arg->nonce, noncelen) != 0)
        return -EFAULT;
    if (arg->pstr && pstrlen &&
        get_from(pstr_tmp, arg->pstr, pstrlen) != 0)
        return -EFAULT;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_drbg_instantiate(arg->mode, arg->security, arg->df,
                                arg->nonce ? nonce_tmp : NULL, noncelen,
                                arg->pstr ? pstr_tmp : NULL, pstrlen);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int drbg_reseed_handler(pufs_drbg_reseed_t *arg)
{
    int ret;
    uint8_t adin_tmp[64];
    uint32_t adinlen = arg->adinlen > sizeof(adin_tmp) ? sizeof(adin_tmp) : arg->adinlen;

    if (arg->adin && adinlen &&
        get_from(adin_tmp, arg->adin, adinlen) != 0)
        return -EFAULT;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_drbg_reseed(arg->df,
                           arg->adin ? adin_tmp : NULL, adinlen);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int drbg_generate_handler(pufs_drbg_generate_t *arg)
{
    int ret;
    uint8_t adin_tmp[64];
    uint8_t out_tmp[256];
    uint32_t outbytes = (arg->outbits + 7) >> 3;
    uint32_t adinlen = arg->adinlen > sizeof(adin_tmp) ? sizeof(adin_tmp) : arg->adinlen;

    if (!arg->out || outbytes > sizeof(out_tmp))
        return -EINVAL;

    if (arg->adin && adinlen &&
        get_from(adin_tmp, arg->adin, adinlen) != 0)
        return -EFAULT;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = _pufs_drbg_generate(out_tmp, arg->outbits, arg->pr, arg->df,
                              arg->adin ? adin_tmp : NULL, adinlen, 0);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    } else if (put_to(arg->out, out_tmp, outbytes) != 0) {
        ret = -EFAULT;
    }
    /* Like rng_read_handler(): the output may become key material. */
    rvv_memset(out_tmp, 0, sizeof(out_tmp));
    return ret;
}

static int drbg_uninit_handler(void)
{
    int ret;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;
    ret = pufs_drbg_uninstantiate();
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int key_io(pufs_key_io_t *arg, bool report_status)
{
    int ret;
    uint8_t keytmp[KWP_KEY_MAXLEN];
    uint32_t keybits = arg->keybits;
    uint32_t keybytes = 0;
    bool importing = arg->mode == KM_IMPORT_PT || arg->mode == KM_IMPORT_WRAP;
    bool exporting = arg->mode == KM_EXPORT_PT || arg->mode == KM_EXPORT_WRAP;

    if (keybits > sizeof(keytmp) * 8)
        return -EINVAL;
    if (importing || exporting) {
        if (!arg->keyaddr)
            return -EFAULT;
        if (!keybits)
            return -EINVAL;
        if (arg->mode == KM_IMPORT_WRAP || arg->mode == KM_EXPORT_WRAP) {
            if (pufs_wrapped_key_size(arg->keywrap, keybits, &keybytes) != SUCCESS)
                return -EINVAL;
        } else {
            keybytes = b2B(keybits);
        }
    } else if (arg->mode != KM_CLEAR) {
        return -EINVAL;
    }

    /* Pre-copy key from userspace for import operations */
    if (importing && get_from(keytmp, arg->keyaddr, keybytes) != 0) {
        ret = -EFAULT;
        goto out;
    }

    ret = pufs_hw_lock();
    if (ret != 0)
        goto out;

    if (arg->mode == KM_IMPORT_PT) {
        ret = pufs_import_plaintext_key(arg->keytype, arg->keyslot, keytmp, keybits);
    } else if (arg->mode == KM_IMPORT_WRAP) {
        ret = pufs_import_wrapped_key(arg->keytype, arg->keyslot, keytmp, keybits, arg->kwslot, arg->kwbits, arg->keywrap);
    } else if (arg->mode == KM_EXPORT_PT) {
        ret = pufs_export_plaintext_key(arg->keytype, arg->keyslot, keytmp, keybits);
    } else if (arg->mode == KM_EXPORT_WRAP) {
        ret = pufs_export_wrapped_key(arg->keytype, arg->keyslot, keytmp, keybits, arg->kwslot, arg->kwbits, arg->keywrap);
    } else {
        ret = pufs_clear_key(arg->keytype, arg->keyslot, keybits);
    }

    pufs_hw_unlock();

    /* Copy exported key back to userspace */
    if (ret == SUCCESS && exporting) {
        if (put_to(arg->keyaddr, keytmp, keybytes) != 0)
            ret = -EFAULT;
    }

    if (ret > 0) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        if (!report_status)
            ret = -ret;
    }
out:
    /* Imported or exported key bytes, on every path. */
    rvv_memset(keytmp, 0, sizeof(keytmp));
    return ret;
}

static int key_derive(pufs_key_derive_t *arg)
{
    int ret;
    uint8_t keytmp[SW_KEY_MAXLEN];
    uint8_t ivtmp[BC_BLOCK_SIZE];
    uint8_t salttmp[DGST_INT_STATE_LEN];
    uint32_t keybits = arg->zbits;
    uint32_t saltlen = arg->saltlen;
    uint8_t *keyaddr = arg->zaddr;
    uint8_t *iv = arg->iv;
    uint8_t *salt = arg->salt;

    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;
    saltlen = saltlen > DGST_INT_STATE_LEN ? DGST_INT_STATE_LEN : saltlen;
    if (arg->ztype == SWKEY) {
        if (get_from(keytmp, keyaddr, (keybits + 7) >> 3) != 0) {
            ret = -EFAULT;
            goto out;
        }
        keyaddr = keytmp;
    }
    if (iv) {
        if (get_from(ivtmp, iv, BC_BLOCK_SIZE) != 0) {
            ret = -EFAULT;
            goto out;
        }
        iv = ivtmp;
    }
    if (salt) {
        if (get_from(salttmp, salt, saltlen) != 0) {
            ret = -EFAULT;
            goto out;
        }
        salt = salttmp;
    }

    ret = pufs_hw_lock();
    if (ret != 0)
        goto out;
    ret = pufs_kdf_base(arg->keytype, arg->keyslot, arg->outbits, arg->method,
                        arg->prf, arg->hash, arg->iter, arg->feedback, iv,
                        arg->ctrpos, arg->ctrlen, arg->ztype,
                        (uint64_t)(uintptr_t)keyaddr, keybits, salt, saltlen,
                        arg->info, arg->infolen, arg->out);
    pufs_hw_unlock();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    }
out:
    /* The input key material is secret. */
    rvv_memset(keytmp, 0, sizeof(keytmp));
    return ret;
}

static int ecc_prk_gen(pufs_ecc_prk_gen_t *arg, bool report_status)
{
    int ret;
    uint8_t keytmp[SW_KEY_MAXLEN];
    uint8_t *keyaddr = arg->keyaddr;
    uint32_t keybits = arg->keybits;

    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;

    /* Pre-copy key from userspace before taking HW lock */
    if (!arg->is_ephemeral && arg->keytype == SWKEY) {
        if (get_from(keytmp, keyaddr, (keybits + 7) >> 3) != 0) {
            ret = -EFAULT;
            goto out;
        }
        keyaddr = keytmp;
    }

    ret = pufs_hw_lock();
    if (ret != 0)
        goto out;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS) goto exit;

    if (arg->is_ephemeral) {
        ret = pufs_ecp_gen_eprk(arg->prkslot);
    } else {
        ret = pufs_ecp_gen_sprk(arg->prkslot, arg->keytype, (size_t)keyaddr, keybits,
                                arg->salt, arg->saltlen, arg->info, arg->infolen,
                                arg->hashtype);
    }
exit:
    pufs_hw_unlock();
    if (ret > 0) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        if (!report_status)
            ret = -ret;
    }
out:
    /* The private key is derived from this secret. */
    rvv_memset(keytmp, 0, sizeof(keytmp));
    return ret;
}

static int ecc_puk_gen(pufs_ecc_puk_gen_t *arg)
{
    int ret;
    pufs_ec_point_st puk;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS) goto exit;

    ret = pufs_ecp_gen_puk(&puk, arg->prktype, arg->prkslot);
exit:
    pufs_hw_unlock();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    if (put_to(arg->puk, &puk, sizeof(puk)) != 0)
        return -EFAULT;
    return 0;
}

static int ecc_puk_verify(pufs_ecc_puk_verify_t *arg)
{
    int ret;
    pufs_ec_point_st puk;

    if (get_from(&puk, arg->puk, sizeof(puk)) != 0)
        return -EFAULT;
    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS) goto exit;

    ret = pufs_ecp_validate_puk(puk, true);
exit:
    pufs_hw_unlock();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int ecc_cdh(pufs_ecc_cdh_t *arg)
{
    int ret;
    pufs_ec_point_st puk_e, puk_s;
    uint8_t outtmp[QLEN_MAX];
    uint8_t *out;

    if (get_from(&puk_e, arg->puk_e, sizeof(puk_e)) != 0)
        return -EFAULT;
    if (!arg->is_ephemeral && get_from(&puk_s, arg->puk_s, sizeof(puk_s)) != 0)
        return -EFAULT;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS) goto exit;

    out = arg->out ? outtmp : NULL;
    if (arg->is_ephemeral) {
        ret = pufs_ecp_ecccdh_2e(puk_e, arg->prkslot_e, out);
    } else {
        ret = pufs_ecp_ecccdh_2e2s(puk_e, puk_s, arg->prkslot_e,
                                   arg->prktype_s, arg->prkslot_s, out);
    }
exit:
    pufs_hw_unlock();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    if (out)
        put_to(arg->out, out, ecc_param[arg->ecctype].len);
    return 0;
}

static int ecdsa_sign(pufs_ecdsa_sign_t *arg)
{
    int ret;
    pufs_dgst_st md;
    pufs_ecdsa_sig_st sig;
    void *md_user = (void *)(uintptr_t)arg->md_phys;
    void *sig_user = (void *)(uintptr_t)arg->sig_phys;

    md.dlen = arg->mdlen > DLEN_MAX ? DLEN_MAX : arg->mdlen;
    if (!md_user || get_from(md.dgst, md_user, md.dlen) != 0)
        return -EINVAL;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS) goto exit;
    ret = pufs_ecp_ecdsa_sign_dgst(&sig, md, arg->prktype, arg->prkslot);
exit:
    pufs_hw_unlock();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return put_to(sig_user, &sig, sizeof(sig));
}

static int ecdsa_verify(pufs_ecdsa_verify_t *arg)
{
    int ret;
    pufs_dgst_st md;
    pufs_ecdsa_sig_st sig;
    pufs_ec_point_st puk;
    void *md_user = (void *)(uintptr_t)arg->md_phys;
    void *sig_user = (void *)(uintptr_t)arg->sig_phys;
    void *puk_user = (void *)(uintptr_t)arg->puk_phys;

    md.dlen = arg->mdlen > DLEN_MAX ? DLEN_MAX : arg->mdlen;
    if (!md_user || !sig_user || get_from(md.dgst, md_user, md.dlen) != 0 ||
        get_from(&sig, sig_user, sizeof(sig)) != 0)
        return -EINVAL;

    if (arg->otpslot > OTPKEY_31 && (!puk_user || get_from(&puk, puk_user, sizeof(puk)) != 0))
        return -EINVAL;

    int lock_err = pufs_hw_lock();
    if (lock_err != 0) return lock_err;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS) goto exit;
    if (arg->otpslot <= OTPKEY_31) {
        ret = pufs_ecp_ecdsa_verify_dgst_otpkey(sig, md, arg->otpslot);
    } else {
        ret = pufs_ecp_ecdsa_verify_dgst(sig, md, puk);
    }
exit:
    pufs_hw_unlock();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int sm2_sign(pufs_sm2_sign_t *arg)
{
    int ret;
    pufs_ecdsa_sig_st sig;
    void *id_user = (void *)(uintptr_t)arg->id_phys;
    void *msg_user = (void *)(uintptr_t)arg->msg_phys;
    void *sig_user = (void *)(uintptr_t)arg->sig_phys;

    if ((uint64_t)arg->idlen + arg->msglen > PUFS_DMA_BUF_SIZE)
        return -EINVAL;

    if (arg->idlen > 0 && (!id_user || get_from(pufs_dma_in, id_user, arg->idlen) != 0))
        return -EINVAL;
    if (arg->msglen > 0 && (!msg_user || get_from(pufs_dma_in + arg->idlen, msg_user, arg->msglen) != 0))
        return -EINVAL;

    ret = pufs_sm2_sign(&sig, pufs_dma_in + arg->idlen, arg->msglen,
                    pufs_dma_in, arg->idlen,
                    arg->prktype, arg->prkslot);

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return put_to(sig_user, &sig, sizeof(sig));
}

static int sm2_verify(pufs_sm2_verify_t *arg)
{
    int ret;
    pufs_ecdsa_sig_st sig;
    pufs_ec_point_st puk;
    void *sig_user = (void *)(uintptr_t)arg->sig_phys;
    void *puk_user = (void *)(uintptr_t)arg->puk_phys;
    void *id_user = (void *)(uintptr_t)arg->id_phys;
    void *msg_user = (void *)(uintptr_t)arg->msg_phys;

    if ((uint64_t)arg->idlen + arg->msglen > PUFS_DMA_BUF_SIZE)
        return -EINVAL;

    if (!sig_user || !puk_user || get_from(&sig, sig_user, sizeof(sig)) != 0 ||
        get_from(&puk, puk_user, sizeof(puk)) != 0)
        return -EINVAL;

    if (arg->idlen > 0 && (!id_user || get_from(pufs_dma_in, id_user, arg->idlen) != 0))
        return -EINVAL;
    if (arg->msglen > 0 && (!msg_user || get_from(pufs_dma_in + arg->idlen, msg_user, arg->msglen) != 0))
        return -EINVAL;

    ret = pufs_sm2_verify(sig, pufs_dma_in + arg->idlen, arg->msglen, pufs_dma_in, arg->idlen, puk);

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    return 0;
}

static int sm2_enc(pufs_sm2_enc_t *arg)
{
    int ret;
    uint32_t outlen;
    pufs_ec_point_st puk;

    /* Output is inlen + 1 + C1(64) + C3(32) = inlen + 97 */
    if (arg->inlen > PUFS_DMA_BUF_SIZE ||
        arg->inlen + 97 > PUFS_DMA_BUF_SIZE)
        return -EINVAL;

    if (get_from(&puk, arg->puk, sizeof(puk)) != 0)
        return -EFAULT;

    /* Pre-copy input from userspace to kernel buffer */
    if (get_from(pufs_dma_in, arg->in, arg->inlen) != 0)
        return -EFAULT;

    ret = pufs_sm2_enc(pufs_dma_out, &outlen, pufs_dma_in, arg->inlen, puk, arg->format);

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    if (put_to(arg->out, pufs_dma_out, outlen) != 0 ||
        put_to(arg->outlen, &outlen, sizeof(outlen)) != 0)
        return -EFAULT;
    return 0;
}

static int sm2_dec(pufs_sm2_dec_t *arg)
{
    int ret;
    uint32_t outlen;

    if (arg->inlen > PUFS_DMA_BUF_SIZE)
        return -EINVAL;

    /* Pre-copy input from userspace to kernel buffer */
    if (get_from(pufs_dma_in, arg->in, arg->inlen) != 0)
        return -EFAULT;

    ret = pufs_sm2_dec(pufs_dma_out, &outlen, pufs_dma_in, arg->inlen, arg->prkslot,
                       arg->format);

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    if (put_to(arg->out, pufs_dma_out, outlen) != 0 ||
        put_to(arg->outlen, &outlen, sizeof(outlen)) != 0)
        return -EFAULT;
    return 0;
}

static int sm2_kex(pufs_sm2_kex_t *arg)
{
    int ret;
    pufs_dgst_st s2, s3;
    pufs_ec_point_st pukr, tpukr;
    uint32_t keylen = arg->keybits / 8;

    if ((uint64_t)arg->idllen + arg->idrlen > PUFS_DMA_BUF_SIZE ||
        keylen > PUFS_DMA_BUF_SIZE)
        return -EINVAL;

    if (get_from(&pukr, arg->pukr, sizeof(pukr)) != 0 ||
        get_from(&tpukr, arg->tpukr, sizeof(tpukr)) != 0)
        return -EFAULT;

    /* Pre-copy idl and idr from userspace to kernel buffer */
    if (get_from(pufs_dma_in, arg->idl, arg->idllen) != 0 ||
        get_from(pufs_dma_in + arg->idllen, arg->idr, arg->idrlen) != 0)
        return -EFAULT;

    ret = pufs_sm2_kex(&s2, &s3, pufs_dma_out, arg->keybits,
                       pufs_dma_in, arg->idllen,
                       pufs_dma_in + arg->idllen, arg->idrlen,
                       arg->prkslotl, arg->tprkslotl,
                       pukr, tpukr, arg->init);

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }
    if (put_to(arg->key, pufs_dma_out, keylen) != 0 ||
        put_to(arg->dlen2, &s2.dlen, sizeof(s2.dlen)) != 0 ||
        put_to(arg->dgst2, s2.dgst, s2.dlen) != 0 ||
        put_to(arg->dlen3, &s3.dlen, sizeof(s3.dlen)) != 0 ||
        put_to(arg->dgst3, s3.dgst, s3.dlen) != 0)
        return -EFAULT;
    return 0;
}

static int rsa_sign(pufs_rsa_sign_t *arg)
{
    int ret;
    uint8_t type = arg->rsatype;
    uint8_t mode = arg->rsamode;
    uint32_t elen = (type + 1) * 128;
    uint32_t msg_input_len = (mode == RSA_BASE) ? elen : arg->msglen;
    uint32_t prk_off = elen;
    uint32_t msg_off = elen * 2;
    size_t salt_off = (size_t)msg_off + msg_input_len;
    void *n_user = (void *)(uintptr_t)arg->n_phys;
    void *prk_user = (void *)(uintptr_t)arg->prk_phys;
    void *msg_user = (void *)(uintptr_t)arg->msg_phys;
    void *salt_user = (void *)(uintptr_t)arg->salt_phys;
    void *sig_user = (void *)(uintptr_t)arg->sig_phys;

    if ((uint64_t)salt_off + arg->saltlen > PUFS_DMA_BUF_SIZE || !n_user || !prk_user || !sig_user)
        return -EINVAL;
    if (get_from(pufs_dma_in, n_user, elen) != 0 || get_from(pufs_dma_in + prk_off, prk_user, elen) != 0 ||
        (msg_input_len > 0 && (!msg_user || get_from(pufs_dma_in + msg_off, msg_user, msg_input_len) != 0)) ||
        (arg->saltlen > 0 && (!salt_user || get_from(pufs_dma_in + salt_off, salt_user, arg->saltlen) != 0))) {
        rvv_memset(pufs_dma_in + prk_off, 0, elen);
        return -EINVAL;
    }


    if (mode == RSA_BASE) {
        ret = pufs_rsa_sign(pufs_dma_out, type, pufs_dma_in, arg->puk,
                            pufs_dma_in + prk_off, pufs_dma_in + msg_off);
    } else if (mode == RSA_X931) {
        ret = pufs_rsa_x931_sign(pufs_dma_out, type, pufs_dma_in, arg->puk,
                                 pufs_dma_in + prk_off, arg->hashtype,
                                 pufs_dma_in + msg_off, arg->msglen);
    } else if (mode == RSA_P1V15) {
        ret = pufs_rsa_p1v15_sign(pufs_dma_out, type, pufs_dma_in, arg->puk,
                                  pufs_dma_in + prk_off, arg->hashtype,
                                  pufs_dma_in + msg_off, arg->msglen);
    } else if (mode == RSA_PSS) {
        ret = pufs_rsa_pss_sign(pufs_dma_out, type, pufs_dma_in, arg->puk,
                                pufs_dma_in + prk_off, arg->hashtype,
                                pufs_dma_in + msg_off, arg->msglen,
                                pufs_dma_in + salt_off, arg->saltlen);
    } else {
        ret = -EINVAL;
    }

    /* The private exponent must not stay in the shared staging buffer. */
    rvv_memset(pufs_dma_in + prk_off, 0, elen);
    if (ret > 0) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    }
    if (ret == 0 && put_to(sig_user, pufs_dma_out, elen) != 0)
        ret = -EINVAL;
    return ret;
}

static int rsa_verify(pufs_rsa_verify_t *arg)
{
    int ret;
    uint8_t type = arg->rsatype;
    uint8_t mode = arg->rsamode;
    uint32_t elen = (type + 1) * 128;
    uint32_t msg_input_len = (mode == RSA_BASE) ? elen : arg->msglen;
    uint32_t msg_off = elen;
    size_t sig_off = (size_t)elen + msg_input_len;
    void *n_user = (void *)(uintptr_t)arg->n_phys;
    void *msg_user = (void *)(uintptr_t)arg->msg_phys;
    void *sig_user = (void *)(uintptr_t)arg->sig_phys;

    if ((uint64_t)sig_off + elen > PUFS_DMA_BUF_SIZE || !n_user || !sig_user)
        return -EINVAL;
    if (get_from(pufs_dma_in, n_user, elen) != 0 ||
        (msg_input_len > 0 && (!msg_user || get_from(pufs_dma_in + msg_off, msg_user, msg_input_len) != 0)) ||
        get_from(pufs_dma_in + sig_off, sig_user, elen) != 0)
        return -EINVAL;


    if (mode == RSA_BASE) {
        ret = pufs_rsa_verify(pufs_dma_in + sig_off, type, pufs_dma_in, arg->puk,
                              pufs_dma_in + msg_off);
    } else if (mode == RSA_X931) {
        ret = pufs_rsa_x931_verify(pufs_dma_in + sig_off, type, pufs_dma_in,
                                   arg->puk, pufs_dma_in + msg_off, arg->msglen);
    } else if (mode == RSA_P1V15) {
        ret = pufs_rsa_p1v15_verify(pufs_dma_in + sig_off, type, pufs_dma_in,
                                    arg->puk, pufs_dma_in + msg_off, arg->msglen);
    } else if (mode == RSA_PSS) {
        ret = pufs_rsa_pss_verify(pufs_dma_in + sig_off, type, pufs_dma_in,
                                  arg->puk, arg->hashtype,
                                  pufs_dma_in + msg_off, arg->msglen);
    } else {
        ret = -EINVAL;
    }

    if (ret > 0) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    }
    return ret;
}

/* ===================================================================
 * Device ioctl handler
 * =================================================================== */

static rt_err_t pufs_control(rt_device_t dev, int cmd, void *args)
{
    int ret;

    (void)dev;

    switch (cmd) {
    /* --- Streaming operations --- */
    case PUFS_HASH_OP: {
        IOCTL_COPY_IN(pufs_hash_op_t, op);
        ret = hash_op_handler(&op);
        IOCTL_COPY_BACK(op);
        break;
    }
    case PUFS_HMAC_OP: {
        IOCTL_COPY_IN(pufs_hmac_op_t, op);
        ret = hmac_op_handler(&op);
        IOCTL_COPY_BACK(op);
        break;
    }
    case PUFS_CMAC_OP: {
        IOCTL_COPY_IN(pufs_cmac_op_t, op);
        ret = cmac_op_handler(&op);
        IOCTL_COPY_BACK(op);
        break;
    }
    case PUFS_SP38A_OP: {
        IOCTL_COPY_IN(pufs_sp38a_op_t, op);
        ret = sp38a_op_handler(&op);
        IOCTL_COPY_BACK(op);
        break;
    }
    case PUFS_SP38D_OP: {
        IOCTL_COPY_IN(pufs_sp38d_op_t, op);
        ret = sp38d_op_handler(&op);
        IOCTL_COPY_BACK(op);
        break;
    }
    case PUFS_SP38C_OP: {
        IOCTL_COPY_IN(pufs_sp38c_op_t, op);
        ret = sp38c_op_handler(&op);
        IOCTL_COPY_BACK(op);
        break;
    }
    case PUFS_SP38E_OP: {
        IOCTL_COPY_IN(pufs_sp38e_op_t, op);
        ret = sp38e_op_handler(&op);
        IOCTL_COPY_BACK(op);
        break;
    }
    case PUFS_CIPHER_CAPS: {
        uint32_t caps = PUFS_CIPHER_CAP_HW_SLOT;
        ret = 0;
        IOCTL_COPY_BACK(caps);
        break;
    }
    /* --- Atomic operations --- */
    case PUFS_UID_GET: {
        IOCTL_COPY_IN(pufs_uid_get_t, arg);
        ret = uid_get(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_OTP_READ: {
        IOCTL_COPY_IN(pufs_otp_rw_t, arg);
        ret = otp_read_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_OTP_WRITE: {
        IOCTL_COPY_IN(pufs_otp_rw_t, arg);
        ret = otp_write_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_OTP_LOCK: {
        IOCTL_COPY_IN(pufs_otp_lock_op_t, arg);
        ret = otp_lock_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_RNG_READ: {
        IOCTL_COPY_IN(pufs_rng_read_t, arg);
        ret = rng_read_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_OTP_RWLCK_GET: {
        IOCTL_COPY_IN(pufs_otp_rwlck_get_t, arg);
        ret = otp_rwlck_get_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_RT_VERSION: {
        IOCTL_COPY_IN(pufs_rt_version_t, arg);
        ret = rt_version_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_KEY2OTP: {
        IOCTL_COPY_IN(pufs_key2otp_t, arg);
        ret = key2otp_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_OTP_SEC_CFG: {
        IOCTL_COPY_IN(pufs_otp_security_cfg_t, arg);
        ret = otp_sec_cfg_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_OTP_SEC_LOCK: {
        IOCTL_COPY_IN(pufs_otp_security_cfg_t, arg);
        ret = otp_sec_lock_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_OTP_SEC_STATE: {
        IOCTL_COPY_IN(pufs_otp_security_state_t, arg);
        ret = otp_sec_state_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_KEY_INOUT:
    case PUFS_KEY_INOUT_STATUS: {
        IOCTL_COPY_IN(pufs_key_io_t, arg);
        /* Match the int-sized command, as the switch does. */
        ret = key_io(&arg, cmd == (int)PUFS_KEY_INOUT_STATUS);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_KEY_DERIVE: {
        IOCTL_COPY_IN(pufs_key_derive_t, arg);
        ret = key_derive(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_ECC_PRK_GEN:
    case PUFS_ECC_PRK_GEN_STATUS: {
        IOCTL_COPY_IN(pufs_ecc_prk_gen_t, arg);
        ret = ecc_prk_gen(&arg, cmd == (int)PUFS_ECC_PRK_GEN_STATUS);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_ECC_PUK_GEN: {
        IOCTL_COPY_IN(pufs_ecc_puk_gen_t, arg);
        ret = ecc_puk_gen(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_ECC_PUK_VERIFY: {
        IOCTL_COPY_IN(pufs_ecc_puk_verify_t, arg);
        ret = ecc_puk_verify(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_ECC_CDH: {
        IOCTL_COPY_IN(pufs_ecc_cdh_t, arg);
        ret = ecc_cdh(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_ECDSA_SIGN: {
        IOCTL_COPY_IN(pufs_ecdsa_sign_t, arg);
        ret = ecdsa_sign(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_ECDSA_VERIFY: {
        IOCTL_COPY_IN(pufs_ecdsa_verify_t, arg);
        ret = ecdsa_verify(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_SM2_SIGN: {
        IOCTL_COPY_IN(pufs_sm2_sign_t, arg);
        int lock_err = pufs_hw_lock();
        if (lock_err != 0) return lock_err;
        ret = sm2_sign(&arg);
        pufs_hw_unlock();
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_SM2_VERIFY: {
        IOCTL_COPY_IN(pufs_sm2_verify_t, arg);
        int lock_err = pufs_hw_lock();
        if (lock_err != 0) return lock_err;
        ret = sm2_verify(&arg);
        pufs_hw_unlock();
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_SM2_ENC: {
        IOCTL_COPY_IN(pufs_sm2_enc_t, arg);
        int lock_err = pufs_hw_lock();
        if (lock_err != 0) return lock_err;
        ret = sm2_enc(&arg);
        pufs_hw_unlock();
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_SM2_DEC: {
        IOCTL_COPY_IN(pufs_sm2_dec_t, arg);
        int lock_err = pufs_hw_lock();
        if (lock_err != 0) return lock_err;
        ret = sm2_dec(&arg);
        pufs_hw_unlock();
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_SM2_KEX: {
        IOCTL_COPY_IN(pufs_sm2_kex_t, arg);
        int lock_err = pufs_hw_lock();
        if (lock_err != 0) return lock_err;
        ret = sm2_kex(&arg);
        pufs_hw_unlock();
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_RSA_SIGN: {
        IOCTL_COPY_IN(pufs_rsa_sign_t, arg);
        int lock_err = pufs_hw_lock();
        if (lock_err != 0) return lock_err;
        ret = rsa_sign(&arg);
        pufs_hw_unlock();
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_RSA_VERIFY: {
        IOCTL_COPY_IN(pufs_rsa_verify_t, arg);
        int lock_err = pufs_hw_lock();
        if (lock_err != 0) return lock_err;
        ret = rsa_verify(&arg);
        pufs_hw_unlock();
        IOCTL_COPY_BACK(arg);
        break;
    }
    /* --- DRBG (SP800-90A) --- */
    case PUFS_DRBG_INIT: {
        IOCTL_COPY_IN(pufs_drbg_init_t, arg);
        ret = drbg_init_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_DRBG_RESEED: {
        IOCTL_COPY_IN(pufs_drbg_reseed_t, arg);
        ret = drbg_reseed_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_DRBG_GENERATE: {
        IOCTL_COPY_IN(pufs_drbg_generate_t, arg);
        ret = drbg_generate_handler(&arg);
        IOCTL_COPY_BACK(arg);
        break;
    }
    case PUFS_DRBG_UNINIT: {
        ret = drbg_uninit_handler();
        break;
    }
    default:
        ret = -EINVAL;
    }

    return ret;
}

/* ===================================================================
 * Device lifecycle — stateless, nothing to clean up
 * =================================================================== */

static rt_err_t pufs_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev; (void)oflag;
    return RT_EOK;
}

static rt_err_t pufs_close(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

static const struct rt_device_ops pufs_ops = {
    RT_NULL,
    pufs_open,
    pufs_close,
    RT_NULL,
    RT_NULL,
    pufs_control,
};

/* ===================================================================
 * Device initialization
 * =================================================================== */

int pufs_device_init(void)
{
    int ret;
    rt_err_t lock_ret;

    /* PUFS/HWRNG is a low-power, always-on service. Keep SEC clock resources
     * held for the lifetime of the registered driver. */
    ret = k230_pm_get(K230_PM_DEV_SEC);
    if (ret != 0) {
        LOG_E("failed to enable SEC clocks ret=%d", ret);
        return ret;
    }

    pufs_dev.base = rt_ioremap((void *)SECURITY_BASE_ADDR, SECURITY_IO_SIZE);
    if (!pufs_dev.base) {
        LOG_E("failed to ioremap security base\n");
        (void)k230_pm_put(K230_PM_DEV_SEC);
        return -RT_ENOMEM;
    }

    lock_ret = rt_mutex_init(&pufs_dev.lock, "pufs_lock", RT_IPC_FLAG_PRIO);
    if (lock_ret != RT_EOK) {
        LOG_E("failed to initialize pufs lock\n");
        ret = lock_ret;
        goto unmap_base;
    }

    /* Pre-allocate shared ioctl staging buffers */
    pufs_dma_in = rt_malloc_align(PUFS_DMA_BUF_SIZE, 64);
    /* CCM's final update also flushes up to one buffered block on top of a
     * full PUFS_DMA_BUF_SIZE input. */
    pufs_dma_out = rt_malloc_align(PUFS_DMA_BUF_SIZE + 64, 64);
    if (!pufs_dma_in || !pufs_dma_out) {
        LOG_E("failed to allocate DMA buffers\n");
        if (pufs_dma_in) rt_free_align(pufs_dma_in);
        if (pufs_dma_out) rt_free_align(pufs_dma_out);
        pufs_dma_in = pufs_dma_out = NULL;
        ret = -RT_ENOMEM;
        goto detach_lock;
    }

    pufs_module_init((uintptr_t)pufs_dev.base, SECURITY_BASE_ADDR, SECURITY_IO_SIZE);
    pufs_dma_module_init(DMA_ADDR_OFFSET, NULL);
    pufs_rt_module_init(RT_ADDR_OFFSET);
    pufs_ka_module_init(KA_ADDR_OFFSET);
    pufs_kwp_module_init(KWP_ADDR_OFFSET);
    pufs_crypto_module_init(CRYPTO_ADDR_OFFSET);
    pufs_hmac_module_init(HMAC_HASH_ADDR_OFFSET);
    pufs_cmac_module_init(CMAC_ADDR_OFFSET);
    pufs_kdf_module_init(KDF_ADDR_OFFSET);
    pufs_pkc_module_init(PKC_ADDR_OFFSET);
    pufs_sp38a_module_init(SP38A_ADDR_OFFSET);
    pufs_sp38c_module_init(SP38C_ADDR_OFFSET);
    pufs_sp38d_module_init(SP38D_ADDR_OFFSET);
    pufs_sp38e_module_init(SP38E_ADDR_OFFSET);
    pufs_drbg_module_init(SP90A_ADDR_OFFSET);
    pufs_rt_cde_init(CDE_ADDR_OFFSET);

    /* Post-mask all OTP and CDE; the driver opens only what each access needs.
     * The guard requires the PUFS mutex, even this early. */
    ret = pufs_lock_and_recover();
    if (ret != 0) {
        LOG_E("OTP post-mask guard initialization failed: %s\n",
              ret == -EBUSY ? "PUFS lock timeout" : "OTP re-mask failed");
    } else {
        if (pufs_otp_guard_init() != SUCCESS)
            LOG_E("OTP post-mask guard initialization failed: OTP access denied\n");
        pufs_hw_unlock();
    }

    pufs_dev.dev.ops = &pufs_ops;
    ret = rt_device_register(&pufs_dev.dev, "pufs", RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK) {
        LOG_E("failed to register pufs device\n");
        goto free_dma;
    }
    pufs_ready = true;

    /* PUFS remains usable when another random provider owns "hwrng". */
    (void)pufs_hwrng_init();

    return RT_EOK;

free_dma:
    rt_free_align(pufs_dma_in);
    rt_free_align(pufs_dma_out);
    pufs_dma_in = pufs_dma_out = NULL;
detach_lock:
    rt_mutex_detach(&pufs_dev.lock);
unmap_base:
    rt_iounmap(pufs_dev.base);
    pufs_dev.base = RT_NULL;
    (void)k230_pm_put(K230_PM_DEV_SEC);
    return ret;
}
INIT_DEVICE_EXPORT(pufs_device_init);
