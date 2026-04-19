/**
 * @file      pufs_sp38a.c
 * @brief     PUFsecurity SP38A API implementation
 * @copyright 2020 PUFsecurity
 */
/* THIS SOFTWARE IS SUPPLIED BY PUFSECURITY ''AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. TO THE FULLEST
 * EXTENT ALLOWED BY LAW, PUFSECURITY'S TOTAL LIABILITY ON ALL CLAIMS IN
 * ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES,
 * IF ANY, THAT YOU HAVE PAID DIRECTLY TO PUFSECURITY FOR THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pufs_internal.h"
#include "pufs_sp38a_internal.h"
#include "pufs_ka_internal.h"
#include "pufs_dma_internal.h"

struct pufs_sp38a_regs* sp38a_regs = NULL;

/*****************************************************************************
 * Static variables
 ****************************************************************************/

/*****************************************************************************
 * Static functions
 ****************************************************************************/
static bool sp38a_check_sgdma_descriptors(sp38a_op op, pufs_dma_sg_desc_st* descs, uint32_t descs_len)
{
    for (uint32_t i = 0; i < descs_len; i++) {
        switch (op) {
        case SP38A_ECB_CLR:
        case SP38A_CFB_CLR:
        case SP38A_CBC_CLR:
            if (i != descs_len - 1 && descs[i].length % BC_BLOCK_SIZE != 0)
                return false;
            if (i == descs_len - 1 && descs[i].length < 1)
                return false;
            break;
        case SP38A_CBC_CS1:
        case SP38A_CBC_CS2:
        case SP38A_CBC_CS3:
            if (i != descs_len - 1 && descs[i].length % BC_BLOCK_SIZE != 0)
                return false;
            if (i == descs_len - 1 && descs[i].length <= BC_BLOCK_SIZE)
                return false;
            break;
        case SP38A_OFB:
        case SP38A_CTR_32:
        case SP38A_CTR_64:
        case SP38A_CTR_128:
            if (descs[i].length < 1)
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

static pufs_status_t sp38a_get_cfg(pufs_sp38a_ctx* sp38a_ctx, uint32_t* cfg)
{
    switch (sp38a_ctx->cipher) {
    case AES:
        switch (sp38a_ctx->keybits) {
        case 128:
            *cfg = 0x0;
            break;
        case 192:
            *cfg = 0x1;
            break;
        case 256:
            *cfg = 0x2;
            break;
        default:
            return E_FIRMWARE;
        }
        break;
    case SM4:
        switch (sp38a_ctx->keybits) {
        case 128:
            *cfg = 0x3;
            break;
        default:
            return E_FIRMWARE;
        }
        break;
    default:
        return E_FIRMWARE;
    }
    switch (sp38a_ctx->op) {
    case SP38A_ECB_CLR:
        (*cfg) |= 0x0 << 4;
        break;
    case SP38A_CFB_CLR:
        (*cfg) |= 0x1 << 4;
        break;
    case SP38A_OFB:
        (*cfg) |= 0x2 << 4;
        break;
    case SP38A_CBC_CLR:
        (*cfg) |= 0x3 << 4;
        break;
    case SP38A_CBC_CS1:
        (*cfg) |= 0x4 << 4;
        break;
    case SP38A_CBC_CS2:
        (*cfg) |= 0x5 << 4;
        break;
    case SP38A_CBC_CS3:
        (*cfg) |= 0x6 << 4;
        break;
    case SP38A_CTR_32:
        (*cfg) |= 0x7 << 4;
        break;
    case SP38A_CTR_64:
        (*cfg) |= 0x8 << 4;
        break;
    case SP38A_CTR_128:
        (*cfg) |= 0x9 << 4;
        break;
    default:
        return E_FIRMWARE;
    }

    (*cfg) |= (sp38a_ctx->encrypt ? 0x1 : 0x0) << 8;
    return SUCCESS;
}

/**
 * @brief Initialize the internal context for block cipher mode of operation
 *
 * @param[in] op         The mode of operation.
 * @param[in] sp38a_ctx  SP38A context to be initialized.
 * @param[in] cipher     The block cipher algorithm.
 * @param[in] encrypt    True/false for encryption/decryption
 * @param[in] keytype    The type of source which the key is from.
 * @param[in] keyaddr    The pointer to the space in SWKEY or the slot of the
 *                        source which the key is stored in.
 * @param[in] keybits    The key length in bits.
 * @param[in] iv         The initial vector used for CBC/CTR modes.
 * @param[in] option     The additional control of mode for CBC/CTR modes.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38a_ctx_init(sp38a_op op,
    pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    bool encrypt,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    int option)
{
    uint32_t val32;
    pufs_status_t check;
    // abort if sp38a_ctx is occupied
    if (sp38a_ctx->op != SP38A_AVAILABLE)
        return E_BUSY;
    // check keytype
    if ((keytype == PUFKEY) || (keytype == SHARESEC))
        return E_DENY;

    if ((check = crypto_check_sp38a_algo(cipher, keybits)) != SUCCESS)
        return check;

    // check key settings for block cipher
    if ((keytype != SWKEY) && ((check = keyslot_check(true, keytype, (uint32_t)keyaddr, keybits)) != SUCCESS))
        return check;
    // check if the mode of operation is supported
    val32 = sp38a_regs->feature;
    if (((encrypt == true) && ((val32 & SP38A_FEATURE_ENC_MASK) == 0)) || ((encrypt == false) && ((val32 & SP38A_FEATURE_DEC_MASK) == 0)))
        return E_UNSUPPORT;
    switch (op) {
    case SP38A_ECB_CLR:
        if ((val32 & SP38A_FEATURE_ECB_CLR_MASK) == 0)
            return E_UNSUPPORT;
        break;
    case SP38A_CFB_CLR:
        if ((val32 & SP38A_FEATURE_CFB_MASK) == 0)
            return E_UNSUPPORT;
        break;
    case SP38A_OFB:
        if ((val32 & SP38A_FEATURE_OFB_MASK) == 0)
            return E_UNSUPPORT;
        break;
    case SP38A_CBC_CLR:
        if (((option == 0) && ((val32 & SP38A_FEATURE_CBC_CLR_MASK) == 0)) || ((option == 1) && ((val32 & SP38A_FEATURE_CBC_CS1_MASK) == 0)) || ((option == 2) && ((val32 & SP38A_FEATURE_CBC_CS2_MASK) == 0)) || ((option == 3) && ((val32 & SP38A_FEATURE_CBC_CS3_MASK) == 0)))
            return E_UNSUPPORT;
        break;
    case SP38A_CTR_128:
        if ((val32 & SP38A_FEATURE_CTR_MASK) == 0)
            return E_UNSUPPORT;
        break;
    default:
        return E_FIRMWARE;
    }

    // check and set iv if needed
    if (op != SP38A_ECB_CLR) {
        if (iv == NULL)
            return E_INVALID;
        else
            rvv_memcpy(sp38a_ctx->iv, iv, BC_BLOCK_SIZE);
    }

    // initialize for block-cipher mode of operation
    sp38a_ctx->buflen = 0;
    sp38a_ctx->cipher = cipher;
    sp38a_ctx->encrypt = encrypt;
    sp38a_ctx->start = false;
    sp38a_ctx->crypto_io_ctx = NULL;

    // set key
    sp38a_ctx->keybits = keybits;
    sp38a_ctx->keytype = keytype;
    if (keytype != SWKEY)
        sp38a_ctx->keyslot = (uint32_t)keyaddr;
    else
        rvv_memcpy(sp38a_ctx->key, (const void*)keyaddr, b2B(keybits));

    // set mode of operation, and the minimum length of the last input
    if (op == SP38A_ECB_CLR) {
        sp38a_ctx->op = SP38A_ECB_CLR;
        sp38a_ctx->minlen = 1;
    } else if (op == SP38A_CFB_CLR) {
        sp38a_ctx->op = SP38A_CFB_CLR;
        sp38a_ctx->minlen = 1;
    } else if (op == SP38A_OFB) {
        sp38a_ctx->op = SP38A_OFB;
        sp38a_ctx->minlen = 1;
    } else if (op == SP38A_CBC_CLR) {
        if (option == 0) {
            sp38a_ctx->op = SP38A_CBC_CLR;
            sp38a_ctx->minlen = 1;
        } else if (option == 1) {
            sp38a_ctx->op = SP38A_CBC_CS1;
            sp38a_ctx->minlen = 17;
        } else if (option == 2) {
            sp38a_ctx->op = SP38A_CBC_CS2;
            sp38a_ctx->minlen = 17;
        } else if (option == 3) {
            sp38a_ctx->op = SP38A_CBC_CS3;
            sp38a_ctx->minlen = 17;
        } else {
            return E_INVALID;
        }
    } else if (op == SP38A_CTR_128) {
        if (option == 32) {
            sp38a_ctx->op = SP38A_CTR_32;
            sp38a_ctx->minlen = 1;
        } else if (option == 64) {
            sp38a_ctx->op = SP38A_CTR_64;
            sp38a_ctx->minlen = 1;
        } else if (option == 128) {
            sp38a_ctx->op = SP38A_CTR_128;
            sp38a_ctx->minlen = 1;
        } else {
            return E_INVALID;
        }
    } else {
        return E_INVALID;
    }

    return SUCCESS;
}
/**
 * @brief Prepare registers for SP38A operation
 *
 * @param[in] sp38a_ctx  SP38A context.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38a_prepare(pufs_sp38a_ctx* sp38a_ctx)
{
    uint32_t val32;
    pufs_status_t check;

    if (sp38a_ctx->keytype == SWKEY)
        crypto_write_sw_key(sp38a_ctx->key, SW_KEY_MAXLEN);

    dma_write_key_config_0(sp38a_ctx->keytype,
        ALGO_TYPE_SP38A,
        sp38a_ctx->keybits,
        get_key_slot_idx(sp38a_ctx->keytype, sp38a_ctx->keyslot));

    crypto_write_iv(sp38a_ctx->iv, BC_BLOCK_SIZE);

    if ((check = sp38a_get_cfg(sp38a_ctx, &val32)) != SUCCESS)
        return check;

    sp38a_regs->cfg = val32;

    return SUCCESS;
}
/**
 * @brief Post-processing for SP38A operation
 *
 * @param[in] sp38a_ctx  SP38A context.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38a_postproc(pufs_sp38a_ctx* sp38a_ctx)
{
    crypto_read_iv(sp38a_ctx->iv, BC_BLOCK_SIZE);
    return SUCCESS;
}
/**
 * @brief Pass the input into the mode of operation hardware
 *
 * @param[in]  sp38a_ctx  SP38A context.
 * @param[out] outbuf     The pointer to the space where the output is written.
 * @param[out] outlen     The length of the output in bytes.
 * @param[in]  inbuf      The input.
 * @param[in]  inlen      The length of the input in bytes.
 * @param[in]  descs      Input arrays of SGDMA descriptors.
 * @param[in]  descs_len  The length of SGDMA descriptor array.
 * @param[in]  last       True if the input for this operation ends
 * @return                SUCCESS on success, otherwise an error code.
 */
static pufs_status_t ___sp38a_ctx_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* outbuf,
    uint32_t* outlen,
    const uint8_t* inbuf,
    uint32_t inlen,
    bool last)
{
    uint32_t val32;
    pufs_status_t check;

    if (outbuf == NULL || outlen == NULL)
        return E_INVALID;

    // Register manipulation
    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_config_0(false, false, false);
    dma_write_data_block_config(sp38a_ctx->start ? false : true, last, true, true, 0);

    if ((check = sp38a_prepare(sp38a_ctx)) != SUCCESS)
        return check;

    dma_write_rwcfg(outbuf, inbuf, inlen);
    dma_write_start();
    if (dma_wait_done()) {
        LOG_ERROR("pufs dma wait timeout\n");
        return E_ERROR;
    }
    dma_check_busy_status(&val32);

    if (val32 != 0) {
        LOG_ERROR("[ERROR] DMA status 0: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    val32 = sp38a_regs->status;
    if ((val32 & SP38A_STATUS_ERROR_MASK) != 0) {
        LOG_ERROR("[ERROR] SP38A status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    // post-processing
    if (last == false)
        sp38a_postproc(sp38a_ctx);

    dma_read_output(outbuf, inlen);
    *outlen = inlen;

    return SUCCESS;
}
static pufs_status_t __sp38a_ctx_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* outbuf,
    uint32_t* outlen,
    const uint8_t* inbuf,
    uint32_t inlen,
    bool last)
{
    pufs_status_t ret;
    uint32_t len, total_len = 0;

    do {
        len = inlen > CHUNK_MAXLEN ? CHUNK_MAXLEN : inlen;
        ret = ___sp38a_ctx_update(sp38a_ctx, outbuf, outlen, inbuf, len, last);
        if (ret != SUCCESS)
            return ret;
        inbuf += len;
        inlen -= len;
        if (outlen && outbuf) {
            outbuf += *outlen;
            total_len += *outlen;
            *outlen = total_len;
        }
        if (sp38a_ctx->start == false)
            sp38a_ctx->start = true;
    } while (inlen);
    return SUCCESS;
}
/**
 * @brief Check whether two op's are the same type
 *
 * @param[in] opf    sp38a_op of function parameter
 * @param[in] opctx  sp38a_op in sp38a_ctx
 * @return           true if the same type; false otherwise.
 */
static bool check_sp38a_op(sp38a_op opf, sp38a_op opctx)
{
    // ECB, CFB, and OFB
    if (opf == opctx)
        return true;

    // CBC
    if (opf == SP38A_CBC_CLR) {
        switch (opctx) {
        case SP38A_CBC_CLR:
        case SP38A_CBC_CS1:
        case SP38A_CBC_CS2:
        case SP38A_CBC_CS3:
            return true;
        default:
            return false;
        }
    }

    // CTR
    if (opf == SP38A_CTR_128) {
        switch (opctx) {
        case SP38A_CTR_32:
        case SP38A_CTR_64:
        case SP38A_CTR_128:
            return true;
        default:
            return false;
        }
    }

    return false;
}
static pufs_status_t sp38a_ctx_sg_append(sp38a_op op,
    pufs_sp38a_ctx* sp38a_ctx,
    bool encrypt,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    uint32_t sp38a_cfg;
    pufs_status_t check;
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_dsc_attr_st* attr;
    pufs_dma_sg_desc_opts_st opts = { .offset = 0x0, .done_interrupt = false, .done_pause = false };

    if ((check_sp38a_op(op, sp38a_ctx->op) == false) || (sp38a_ctx->encrypt != encrypt))
        return E_UNAVAIL;

    if (sp38a_check_sgdma_descriptors(op, descs, descs_len) == false)
        return E_INVALID;

    if (sp38a_ctx->crypto_io_ctx == NULL) {
        sp38a_ctx->crypto_io_ctx = crypto_new_crypto_io_ctx();

        if (sp38a_ctx->keytype == SWKEY)
            crypto_io_write_sw_key(sp38a_ctx->crypto_io_ctx, sp38a_ctx->key, SW_KEY_MAXLEN);
    }

    crypto_io_write_iv(sp38a_ctx->crypto_io_ctx, sp38a_ctx->iv, BC_BLOCK_SIZE);

    desc = dma_sg_new_read_ctx_descriptor((uintptr_t)sp38a_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    if ((check = sp38a_get_cfg(sp38a_ctx, &sp38a_cfg)) != SUCCESS)
        return check;

    for (uint32_t index = 0; index < descs_len; index++) {
        desc = dma_sg_new_data_descriptor();
        if (desc == NULL)
            return E_FIRMWARE;

        attr = &descs[index].attr;
        opts.head = !sp38a_ctx->start;

        if (index == descs_len - 1 && last)
            opts.tail = last;
        else
            opts.tail = false;

        dma_sg_desc_write_addr(desc, descs[index].write_addr, descs[index].read_addr, descs[index].length);
        dma_sg_desc_write_dsc_config(desc, attr, &opts);
        dma_sg_desc_write_key_config(desc, sp38a_ctx->keytype,
            ALGO_TYPE_SP38A, sp38a_ctx->keybits,
            get_key_slot_idx(sp38a_ctx->keytype, sp38a_ctx->keyslot));
        dma_sg_desc_write_crypto_config(desc, sp38a_cfg, 0x0);

        dma_sg_desc_append_to_list(desc);

        if (!sp38a_ctx->start)
            sp38a_ctx->start = true;
    }

    desc = dma_sg_new_write_ctx_descriptor((uintptr_t)sp38a_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    return SUCCESS;
}
static pufs_status_t sp38a_ctx_sg_done(sp38a_op op,
    pufs_sp38a_ctx* sp38a_ctx,
    bool encrypt)
{
    if ((check_sp38a_op(op, sp38a_ctx->op) == false) || (sp38a_ctx->encrypt != encrypt))
        return E_UNAVAIL;

    crypto_free_crypto_io_ctx(sp38a_ctx->crypto_io_ctx);
    sp38a_ctx->crypto_io_ctx = NULL;
    sp38a_ctx->op = SP38A_AVAILABLE;
    return SUCCESS;
}
/**
 * @brief Handle input and update the buffer for block cipher mode of operation
 *
 * @see sp38a_ctx_init().
 * @see __sp38a_ctx_update().
 * @return SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38a_ctx_update(sp38a_op op,
    pufs_sp38a_ctx* sp38a_ctx,
    bool encrypt,
    uint8_t* outbuf,
    uint32_t* outlen,
    const uint8_t* inbuf,
    uint32_t inlen)
{
    // check sp38a_ctx is owned by this operation (mode of operation/encrypt)
    if ((check_sp38a_op(op, sp38a_ctx->op) == false) || (sp38a_ctx->encrypt != encrypt))
        return E_UNAVAIL;
    // continue if msg is NULL or msglen is zero
    *outlen = 0;
    if ((inbuf == NULL) || (inlen == 0))
        return SUCCESS;

    pufs_status_t check = SUCCESS;
    uint32_t seglen = 0;
    blsegs segs = segment(sp38a_ctx->buff, sp38a_ctx->buflen, inbuf, inlen,
        BC_BLOCK_SIZE, sp38a_ctx->minlen);
    sp38a_ctx->buflen = 0;

    for (uint32_t i = 0; i < segs.nsegs; i++) {
        if (segs.seg[i].process) // process
        {
            if ((check = __sp38a_ctx_update(sp38a_ctx, outbuf + *outlen, &seglen,
                     segs.seg[i].addr, segs.seg[i].len,
                     false))
                != SUCCESS) {
                // release sp38a context
                sp38a_ctx->op = SP38A_AVAILABLE;
                return check;
            }
            *outlen += seglen;
        } else // keep in the internal buffer
        {
            if ((segs.seg[i].addr == sp38a_ctx->buff) && (sp38a_ctx->buflen == 0)) { // skip copy what already in the right place
                sp38a_ctx->buflen += segs.seg[i].len;
            } else // copy into the buffer
            {
                rvv_memcpy(sp38a_ctx->buff + sp38a_ctx->buflen, segs.seg[i].addr, segs.seg[i].len);
                sp38a_ctx->buflen += segs.seg[i].len;
            }
        }
    }

    return SUCCESS;
}
/**
 * @brief Handle the data left in the buffer for block cipher mode of operation
 *
 * @see sp38a_ctx_init().
 * @see __sp38a_ctx_update().
 * @return SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38a_ctx_final(sp38a_op op,
    pufs_sp38a_ctx* sp38a_ctx,
    bool encrypt,
    uint8_t* outbuf,
    uint32_t* outlen)
{
    pufs_status_t check = SUCCESS;

    // check sp38a_ctx is owned by this operation (mode of operation/encrypt)
    if ((check_sp38a_op(op, sp38a_ctx->op) == false) || (sp38a_ctx->encrypt != encrypt))
        return E_UNAVAIL;

    // in final call, it must be minimum-length bytes depending on modes to
    //  pass into the modes of operation module
    if (sp38a_ctx->buflen < sp38a_ctx->minlen)
        check = E_INVALID;
    else
        check = __sp38a_ctx_update(sp38a_ctx, outbuf, outlen, sp38a_ctx->buff,
            sp38a_ctx->buflen, true);

    // release sp38a context
    sp38a_ctx->op = SP38A_AVAILABLE;
    return check;
}

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * pufs_gdle_sp38a_prepare()
 */
pufs_status_t pufs_gdle_sp38a_prepare(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_prepare(sp38a_ctx);
}
/**
 * pufs_gdle_sp38a_postproc()
 */
pufs_status_t pufs_gdle_sp38a_postproc(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_postproc(sp38a_ctx);
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_sp38a_module_init(uint32_t sp38a_offset)
{
    sp38a_regs = (struct pufs_sp38a_regs*)(pufs_context.base_addr + sp38a_offset);
    version_check(SP38A_VERSION, sp38a_regs->version);
    LOG_INFO("%s", "SP38A module is initialized");
}
/**
 * pufs_sp38a_ctx_new()
 */
pufs_sp38a_ctx* pufs_sp38a_ctx_new(void)
{
    pufs_sp38a_ctx* ret;

    ret = malloc(sizeof(pufs_sp38a_ctx));
    if (ret != NULL) {
        ret->op = SP38A_AVAILABLE;
        rvv_memset(ret, 0x0, sizeof(pufs_sp38a_ctx));
    }

    return ret;
}
/**
 * pufs_sp38a_ctx_free()
 */
void pufs_sp38a_ctx_free(pufs_sp38a_ctx* sp38a_ctx)
{
    if (sp38a_ctx != NULL) {
        rvv_memset(sp38a_ctx, 0, sizeof(pufs_sp38a_ctx));
        sp38a_ctx->op = SP38A_AVAILABLE;
    }
    free(sp38a_ctx);
}
/**
 * _pufs_enc_ecb()
 */
pufs_status_t _pufs_enc_ecb_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    return sp38a_ctx_init(SP38A_ECB_CLR, sp38a_ctx, cipher, true, keytype,
        keyaddr, keybits, NULL, 0);
}
pufs_status_t pufs_enc_ecb_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_ECB_CLR, sp38a_ctx, true,
        out, outlen, in, inlen);
}
pufs_status_t pufs_enc_ecb_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_ECB_CLR, sp38a_ctx, true, descs, descs_len, last);
}

pufs_status_t pufs_enc_ecb_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_ECB_CLR, sp38a_ctx, true);
}

pufs_status_t pufs_enc_ecb_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_ECB_CLR, sp38a_ctx, true, out, outlen);
}
pufs_status_t _pufs_enc_ecb(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_enc_ecb_init(&sp38a_ctx, cipher, keytype,
             keyaddr, keybits))
        != SUCCESS)
        return check;
    if ((check = pufs_enc_ecb_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_enc_ecb_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_dec_ecb()
 */
pufs_status_t _pufs_dec_ecb_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    return sp38a_ctx_init(SP38A_ECB_CLR, sp38a_ctx, cipher, false, keytype,
        keyaddr, keybits, NULL, 0);
}
pufs_status_t pufs_dec_ecb_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_ECB_CLR, sp38a_ctx, false,
        out, outlen, in, inlen);
}
pufs_status_t pufs_dec_ecb_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_ECB_CLR, sp38a_ctx, false, descs, descs_len, last);
}
pufs_status_t pufs_dec_ecb_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_ECB_CLR, sp38a_ctx, false);
}
pufs_status_t pufs_dec_ecb_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_ECB_CLR, sp38a_ctx, false, out, outlen);
}
pufs_status_t _pufs_dec_ecb(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_dec_ecb_init(&sp38a_ctx, cipher, keytype,
             keyaddr, keybits))
        != SUCCESS)
        return check;
    if ((check = pufs_dec_ecb_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_dec_ecb_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_enc_cfb()
 */
pufs_status_t _pufs_enc_cfb_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv)
{
    return sp38a_ctx_init(SP38A_CFB_CLR, sp38a_ctx, cipher, true, keytype,
        keyaddr, keybits, iv, 0);
}
pufs_status_t pufs_enc_cfb_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_CFB_CLR, sp38a_ctx, true,
        out, outlen, in, inlen);
}
pufs_status_t pufs_enc_cfb_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_CFB_CLR, sp38a_ctx, true,
        descs, descs_len, last);
}
pufs_status_t pufs_enc_cfb_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_CFB_CLR, sp38a_ctx, true);
}
pufs_status_t pufs_enc_cfb_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_CFB_CLR, sp38a_ctx, true, out, outlen);
}
pufs_status_t _pufs_enc_cfb(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_enc_cfb_init(&sp38a_ctx, cipher, keytype, keyaddr,
             keybits, iv))
        != SUCCESS)
        return check;
    if ((check = pufs_enc_cfb_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_enc_cfb_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_dec_cfb()
 */
pufs_status_t _pufs_dec_cfb_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv)
{
    return sp38a_ctx_init(SP38A_CFB_CLR, sp38a_ctx, cipher, false, keytype,
        keyaddr, keybits, iv, 0);
}
pufs_status_t pufs_dec_cfb_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_CFB_CLR, sp38a_ctx, false,
        out, outlen, in, inlen);
}
pufs_status_t pufs_dec_cfb_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_CFB_CLR, sp38a_ctx, false,
        descs, descs_len, last);
}
pufs_status_t pufs_dec_cfb_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_CFB_CLR, sp38a_ctx, false);
}
pufs_status_t pufs_dec_cfb_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_CFB_CLR, sp38a_ctx, false, out, outlen);
}
pufs_status_t _pufs_dec_cfb(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_dec_cfb_init(&sp38a_ctx, cipher, keytype, keyaddr,
             keybits, iv))
        != SUCCESS)
        return check;
    if ((check = pufs_dec_cfb_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_dec_cfb_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_enc_ofb()
 */
pufs_status_t _pufs_enc_ofb_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv)
{
    return sp38a_ctx_init(SP38A_OFB, sp38a_ctx, cipher, true, keytype,
        keyaddr, keybits, iv, 0);
}
pufs_status_t pufs_enc_ofb_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_OFB, sp38a_ctx, true, out, outlen, in, inlen);
}
pufs_status_t pufs_enc_ofb_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_OFB, sp38a_ctx, true,
        descs, descs_len, last);
}
pufs_status_t pufs_enc_ofb_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_OFB, sp38a_ctx, true);
}
pufs_status_t pufs_enc_ofb_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_OFB, sp38a_ctx, true, out, outlen);
}
pufs_status_t _pufs_enc_ofb(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_enc_ofb_init(&sp38a_ctx, cipher, keytype, keyaddr,
             keybits, iv))
        != SUCCESS)
        return check;
    if ((check = pufs_enc_ofb_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_enc_ofb_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_dec_ofb()
 */
pufs_status_t _pufs_dec_ofb_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv)
{
    return sp38a_ctx_init(SP38A_OFB, sp38a_ctx, cipher, false, keytype,
        keyaddr, keybits, iv, 0);
}
pufs_status_t pufs_dec_ofb_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_OFB, sp38a_ctx, false, out, outlen, in, inlen);
}
pufs_status_t pufs_dec_ofb_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_OFB, sp38a_ctx, false,
        descs, descs_len, last);
}
pufs_status_t pufs_dec_ofb_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_OFB, sp38a_ctx, false);
}
pufs_status_t pufs_dec_ofb_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_OFB, sp38a_ctx, false, out, outlen);
}
pufs_status_t _pufs_dec_ofb(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_dec_ofb_init(&sp38a_ctx, cipher, keytype, keyaddr,
             keybits, iv))
        != SUCCESS)
        return check;
    if ((check = pufs_dec_ofb_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_dec_ofb_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_enc_cbc()
 */
pufs_status_t _pufs_enc_cbc_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    int csmode)
{
    return sp38a_ctx_init(SP38A_CBC_CLR, sp38a_ctx, cipher, true, keytype,
        keyaddr, keybits, iv, csmode);
}
pufs_status_t pufs_enc_cbc_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_CBC_CLR, sp38a_ctx, true,
        out, outlen, in, inlen);
}
pufs_status_t pufs_enc_cbc_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_CBC_CLR, sp38a_ctx, true,
        descs, descs_len, last);
}
pufs_status_t pufs_enc_cbc_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_CBC_CLR, sp38a_ctx, true);
}
pufs_status_t pufs_enc_cbc_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_CBC_CLR, sp38a_ctx, true, out, outlen);
}
pufs_status_t _pufs_enc_cbc(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    int csmode)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_enc_cbc_init(&sp38a_ctx, cipher, keytype, keyaddr,
             keybits, iv, csmode))
        != SUCCESS)
        return check;
    if ((check = pufs_enc_cbc_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_enc_cbc_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_dec_cbc()
 */
pufs_status_t _pufs_dec_cbc_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    int csmode)
{
    return sp38a_ctx_init(SP38A_CBC_CLR, sp38a_ctx, cipher, false, keytype,
        keyaddr, keybits, iv, csmode);
}
pufs_status_t pufs_dec_cbc_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_CBC_CLR, sp38a_ctx, false,
        out, outlen, in, inlen);
}
pufs_status_t pufs_dec_cbc_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_CBC_CLR, sp38a_ctx, false,
        descs, descs_len, last);
}
pufs_status_t pufs_dec_cbc_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_CBC_CLR, sp38a_ctx, false);
}
pufs_status_t pufs_dec_cbc_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_CBC_CLR, sp38a_ctx, false, out, outlen);
}
pufs_status_t _pufs_dec_cbc(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    int csmode)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_dec_cbc_init(&sp38a_ctx, cipher, keytype, keyaddr,
             keybits, iv, csmode))
        != SUCCESS)
        return check;
    if ((check = pufs_dec_cbc_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_dec_cbc_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_enc_ctr()
 */
pufs_status_t _pufs_enc_ctr_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* t1,
    int ctrlen)
{
    return sp38a_ctx_init(SP38A_CTR_128, sp38a_ctx, cipher, true, keytype,
        keyaddr, keybits, t1, ctrlen);
}
pufs_status_t pufs_enc_ctr_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_CTR_128, sp38a_ctx, true,
        out, outlen, in, inlen);
}
pufs_status_t pufs_enc_ctr_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_CTR_128, sp38a_ctx, true,
        descs, descs_len, last);
}
pufs_status_t pufs_enc_ctr_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_CTR_128, sp38a_ctx, true);
}
pufs_status_t pufs_enc_ctr_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_CTR_128, sp38a_ctx, true, out, outlen);
}
pufs_status_t _pufs_enc_ctr(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* t1,
    int ctrlen)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_enc_ctr_init(&sp38a_ctx, cipher, keytype, keyaddr,
             keybits, t1, ctrlen))
        != SUCCESS)
        return check;
    if ((check = pufs_enc_ctr_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_enc_ctr_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_dec_ctr()
 */
pufs_status_t _pufs_dec_ctr_init(pufs_sp38a_ctx* sp38a_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* t1,
    int ctrlen)
{
    return sp38a_ctx_init(SP38A_CTR_128, sp38a_ctx, cipher, false, keytype,
        keyaddr, keybits, t1, ctrlen);
}
pufs_status_t pufs_dec_ctr_update(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38a_ctx_update(SP38A_CTR_128, sp38a_ctx, false,
        out, outlen, in, inlen);
}
pufs_status_t pufs_dec_ctr_sg_append(pufs_sp38a_ctx* sp38a_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38a_ctx_sg_append(SP38A_CTR_128, sp38a_ctx, false,
        descs, descs_len, last);
}
pufs_status_t pufs_dec_ctr_sg_done(pufs_sp38a_ctx* sp38a_ctx)
{
    return sp38a_ctx_sg_done(SP38A_CTR_128, sp38a_ctx, false);
}
pufs_status_t pufs_dec_ctr_final(pufs_sp38a_ctx* sp38a_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38a_ctx_final(SP38A_CTR_128, sp38a_ctx, false, out, outlen);
}
pufs_status_t _pufs_dec_ctr(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* t1,
    int ctrlen)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_dec_ctr_init(&sp38a_ctx, cipher, keytype, keyaddr,
             keybits, t1, ctrlen))
        != SUCCESS)
        return check;
    if ((check = pufs_dec_ctr_update(&sp38a_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_dec_ctr_final(&sp38a_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
