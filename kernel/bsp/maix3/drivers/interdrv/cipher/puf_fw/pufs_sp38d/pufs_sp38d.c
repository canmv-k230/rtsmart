/**
 * @file      pufs_sp38d.c
 * @brief     PUFsecurity SP38D API implementation
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
#include <limits.h>
#include "pufs_internal.h"
#include "pufs_crypto_internal.h"
#include "pufs_sp38d_internal.h"
#include "pufs_ka_internal.h"
#include "pufs_dma_internal.h"

struct pufs_sp38d_regs* sp38d_regs = NULL;

/*****************************************************************************
 * Static variables
 ****************************************************************************/

/*****************************************************************************
 * Static functions
 ****************************************************************************/
static pufs_status_t sp38d_get_config(uint32_t* cfg, pufs_sp38d_ctx* ctx, bool gctr, bool reg_in, bool reg_out)
{
    uint32_t val32;
    switch (ctx->cipher) {
    case AES:
        switch (ctx->keybits) {
        case 128:
            val32 = 0x0;
            break;
        case 192:
            val32 = 0x1;
            break;
        case 256:
            val32 = 0x2;
            break;
        default:
            return E_FIRMWARE;
        }
        break;
    case SM4:
        switch (ctx->keybits) {
        case 128:
            val32 = 0x3;
            break;
        default:
            return E_FIRMWARE;
        }
        break;
    default:
        return E_FIRMWARE;
    }
    if (ctx->inbits != ULLONG_MAX)
        val32 |= 0x1 << SP38C_CFG_GHASH_BITS;

    if (gctr)
        val32 |= 0x1 << SP38C_CFG_GCTR_BITS;

    val32 |= (ctx->encrypt ? 0x1 : 0x0) << SP38C_CFG_ENCRYPT_BITS;

    if (reg_in)
        val32 |= 0x1 << SP38C_CFG_REG_IN_BITS;

    if (reg_out)
        val32 |= 0x1 << SP38C_CFG_REG_OUT_BITS;

    *cfg = val32;
    return SUCCESS;
}

static void sp38d_set_crypto_io_ctx(pufs_sp38d_ctx* ctx)
{
    if (ctx->crypto_io_ctx == NULL) {
        ctx->crypto_io_ctx = crypto_new_crypto_io_ctx();

        if (ctx->keytype == SWKEY)
            crypto_io_write_sw_key(ctx->crypto_io_ctx, ctx->key, SW_KEY_MAXLEN);

        crypto_io_write_iv(ctx->crypto_io_ctx, ctx->j0, BC_BLOCK_SIZE);
    }
}

/**
 * @brief Initialize the internal context for block cipher GCM mode
 *
 * @param[in] op         GCM operation mode.
 * @param[in] sp38d_ctx  SP38D context to be initialized.
 * @param[in] encrypt    True/false for encryption/decryption
 * @param[in] cipher     Block cipher algorithm.
 * @param[in] keytype    Key type.
 * @param[in] keyaddr    Key address.
 * @param[in] keybits    Key length in bits.
 * @param[in] j0         J_0
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_ctx_init(sp38d_op op,
    pufs_sp38d_ctx* sp38d_ctx,
    bool encrypt,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* j0)
{
    uint32_t val32;
    pufs_status_t check;
    // abort if sp38d_ctx is occupied
    if (sp38d_ctx->op != SP38D_AVAILABLE)
        return E_BUSY;
    // check if op is valid
    switch (op) {
    case SP38D_GHASH:
    case SP38D_GCM:
    case SP38D_GMAC:
        break;
    default:
        return E_INVALID;
    }
    // check keytype
    if ((keytype == PUFKEY) || (keytype == SHARESEC))
        return E_DENY;
    // check feature with key length

    if ((check = crypto_check_sp38d_algo(cipher, keybits)) != SUCCESS)
        return check;

    // check key settings for block cipher
    if ((keytype != SWKEY) && ((check = keyslot_check(true, keytype, (uint32_t)keyaddr, keybits)) != SUCCESS))
        return check;
    // check if the GCM operation mode is supported
    val32 = sp38d_regs->feature;
    if (((val32 & SP38D_FEATURE_GHASH_MASK) == 0) || ((op != SP38D_GHASH) && ((val32 & SP38D_FEATURE_GCTR_MASK) == 0)))
        return E_UNSUPPORT;

    // check and set J_0 if needed
    if (op != SP38D_GHASH) {
        if (j0 == NULL)
            return E_INVALID;
        rvv_memcpy(sp38d_ctx->j0, j0, BC_BLOCK_SIZE);
    }

    // initialize for block-cipher GCM mode
    sp38d_ctx->aadbits = 0;
    sp38d_ctx->inbits = 0;
    sp38d_ctx->buflen = 0;
    sp38d_ctx->cipher = cipher;
    sp38d_ctx->encrypt = encrypt;
    sp38d_ctx->op = op;
    sp38d_ctx->minlen = 1;
    sp38d_ctx->incj0 = 1;
    sp38d_ctx->stage = SP38D_NONE;
    sp38d_ctx->crypto_io_ctx = NULL;

    rvv_memset(sp38d_ctx->ghash, 0, BC_BLOCK_SIZE);

    // set key
    sp38d_ctx->keybits = keybits;
    sp38d_ctx->keytype = keytype;
    if (keytype != SWKEY)
        sp38d_ctx->keyslot = (uint32_t)keyaddr;
    else
        rvv_memcpy(sp38d_ctx->key, (const void*)keyaddr, b2B(keybits));

    return SUCCESS;
}
/**
 * @brief Prepare registers for SP38D operation
 *
 * @param[in] sp38d_ctx  SP38D context.
 * @param[in] inlen      Input length for the preparing operation.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_prepare(pufs_sp38d_ctx* sp38d_ctx,
    const uint8_t* out,
    uint32_t inlen)
{
    pufs_status_t check;
    uint32_t val32;

    if (sp38d_ctx->keytype == SWKEY)
        crypto_write_sw_key(sp38d_ctx->key, SW_KEY_MAXLEN);

    dma_write_key_config_0(sp38d_ctx->keytype,
        ALGO_TYPE_GCM,
        sp38d_ctx->keybits,
        get_key_slot_idx(sp38d_ctx->keytype, sp38d_ctx->keyslot));

    if ((check = sp38d_get_config(&val32, sp38d_ctx, out != NULL, false, false)) != SUCCESS)
        return check;

    sp38d_regs->cfg = val32;

    if (sp38d_ctx->inbits != ULLONG_MAX)
        sp38d_regs->block_num = sp38d_ctx->incj0;
    else
        sp38d_regs->block_num = 0;

    // J_0
    if (out != NULL) {
        crypto_write_iv(sp38d_ctx->j0, BC_BLOCK_SIZE);
        if (sp38d_ctx->inbits != ULLONG_MAX && inlen > 0)
            sp38d_ctx->incj0 += ((inlen - 1 + BC_BLOCK_SIZE) / BC_BLOCK_SIZE);
    }

    // Restore GHASH
    crypto_write_dgst(sp38d_ctx->ghash, BC_BLOCK_SIZE);

    return SUCCESS;
}
/**
 * @brief Post-processing for SP38D operation
 *
 * @param[in] sp38d_ctx  SP38D context.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_postproc(pufs_sp38d_ctx* sp38d_ctx)
{
    crypto_read_dgest(sp38d_ctx->ghash, BC_BLOCK_SIZE);
    return SUCCESS;
}
/**
 * @brief Pass the input into SP38D hardware
 *
 * @param[in]  sp38d_ctx  SP38D context.
 * @param[out] out        The pointer to the space where the output is written.
 * @param[out] outlen     The length of the output in bytes.
 * @param[in]  in         The input.
 * @param[in]  inlen      The length of the input in bytes.
 * @param[in]  last       True if the input for this operation ends
 * @return                SUCCESS on success, otherwise an error code.
 */
static pufs_status_t ___sp38d_ctx_update(pufs_sp38d_ctx* sp38d_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    bool last)
{
    uint32_t val32;
    pufs_status_t check;
    // Register manipulation
    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_config_0(false, false, false);
    dma_write_data_block_config(sp38d_ctx->start ? false : true, last, true, true, 0);

    if ((check = sp38d_prepare(sp38d_ctx, out, inlen)) != SUCCESS)
        return check;

    dma_write_rwcfg(out, in, inlen);
    dma_write_start();
    if (dma_wait_done()) {
        LOG_ERROR("pufs dma wait timeout\n");
        return E_ERROR;
    }
    dma_check_busy_status(&val32);

    if (val32 != 0) {
        LOG_ERROR("DMA status 0: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    val32 = sp38d_regs->status;
    if ((val32 & SP38D_STATUS_RESP_MASK) != 0) {
        LOG_ERROR("SP38D status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    if ((check = sp38d_postproc(sp38d_ctx)) != SUCCESS)
        return check;

    if (out != NULL) // output
    {
        dma_read_output(out, inlen);
        *outlen = inlen;
    }

    return SUCCESS;
}

static pufs_status_t __sp38d_ctx_update(pufs_sp38d_ctx* sp38d_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    bool last)
{
    pufs_status_t ret;
    uint32_t len, total_len = 0;

    do {
        len = inlen > CHUNK_MAXLEN ? CHUNK_MAXLEN : inlen;
        ret = ___sp38d_ctx_update(sp38d_ctx, out, outlen, in, len, last);
        if (ret != SUCCESS)
            return ret;
        in += len;
        inlen -= len;
        if (outlen && out) {
            out += *outlen;
            total_len += *outlen;
            *outlen = total_len;
        }
        if (sp38d_ctx->start == false)
            sp38d_ctx->start = true;
    } while (inlen);
    return SUCCESS;
}
/**
 * @brief Input data into SP38D
 *
 * @param[in]  op         GCM operation mode.
 * @param[in]  sp38d_ctx  SP38D context.
 * @param[in]  encrypt    True/false for encryption/decryption
 * @param[out] out        Output data.
 * @param[out] outlen     Output data length in bytes.
 * @param[in]  in         Input data.
 * @param[in]  inlen      Input data length in bytes.
 * @return                SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_ctx_update(sp38d_op op,
    pufs_sp38d_ctx* sp38d_ctx,
    bool encrypt,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    // check sp38d_ctx is owned by this operation (GCM mode)
    if ((sp38d_ctx->op != op) || (sp38d_ctx->encrypt != encrypt))
        return E_UNAVAIL;
    // continue if msg is NULL or msglen is zero
    if ((in == NULL) || (inlen == 0)) {
        if (outlen != NULL)
            *outlen = 0;
        return SUCCESS;
    }

    switch (sp38d_ctx->stage) {
    case SP38D_NONE:
        break;
    case SP38D_AAD:
        sp38d_ctx->aadbits += (((uint64_t)inlen) << 3);
        break;
    case SP38D_TEXT:
        sp38d_ctx->inbits += (((uint64_t)inlen) << 3);
        break;
    default:
        return E_FIRMWARE;
    }

    blsegs segs = segment(sp38d_ctx->buff, sp38d_ctx->buflen, in, inlen,
        BC_BLOCK_SIZE, sp38d_ctx->minlen);
    sp38d_ctx->buflen = 0;

    uint32_t seglen = 0;
    pufs_status_t check = SUCCESS;
    if (sp38d_ctx->stage == SP38D_TEXT)
        *outlen = 0;
    for (uint32_t i = 0; i < segs.nsegs; i++) {
        if (segs.seg[i].process) // process
        {
            if (sp38d_ctx->stage == SP38D_TEXT)
                check = __sp38d_ctx_update(sp38d_ctx, out + *outlen, &seglen,
                    segs.seg[i].addr, segs.seg[i].len,
                    false);
            else
                check = __sp38d_ctx_update(sp38d_ctx,
                    NULL, NULL, segs.seg[i].addr,
                    segs.seg[i].len, false);
            if (check != SUCCESS) {
                // release sp38d context
                sp38d_ctx->op = SP38D_AVAILABLE;
                return check;
            }
            if (sp38d_ctx->stage == SP38D_TEXT)
                *outlen += seglen;
        } else // keep in the internal buffer
        {
            if ((segs.seg[i].addr == sp38d_ctx->buff) && (sp38d_ctx->buflen == 0)) { // skip copy what already in the right place
                sp38d_ctx->buflen += segs.seg[i].len;
            } else // copy into the buffer
            {
                rvv_memcpy(sp38d_ctx->buff + sp38d_ctx->buflen, segs.seg[i].addr, segs.seg[i].len);
                sp38d_ctx->buflen += segs.seg[i].len;
            }
        }
    }

    return SUCCESS;
}
static pufs_status_t sp38d_ctx_sg_append(sp38d_op op,
    pufs_sp38d_ctx* sp38d_ctx,
    bool encrypt,
    pufs_gcm_input_data_t data_type,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    uint32_t cfg;
    pufs_status_t check;
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_dsc_attr_st* attr;
    pufs_dma_sg_desc_opts_st opts = { .offset = 0x0, .done_interrupt = false, .done_pause = false };

    if ((sp38d_ctx->op != op) || (sp38d_ctx->encrypt != encrypt))
        return E_UNAVAIL;

    sp38d_set_crypto_io_ctx(sp38d_ctx);

    desc = dma_sg_new_read_ctx_descriptor((uintptr_t)sp38d_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    if ((check = sp38d_get_config(&cfg, sp38d_ctx, data_type == GCM_PLAINTEXT, false, false)) != SUCCESS)
        return check;

    for (uint32_t index = 0; index < descs_len; index++) {
        desc = dma_sg_new_data_descriptor();
        if (desc == NULL)
            return E_FIRMWARE;

        attr = &descs[index].attr;
        opts.head = !sp38d_ctx->start;

        if (index == descs_len - 1 && last)
            opts.tail = last;
        else
            opts.tail = false;

        dma_sg_desc_write_addr(desc, descs[index].write_addr, descs[index].read_addr, descs[index].length);
        dma_sg_desc_write_dsc_config(desc, attr, &opts);
        dma_sg_desc_write_key_config(desc, sp38d_ctx->keytype,
            ALGO_TYPE_GCM, sp38d_ctx->keybits,
            get_key_slot_idx(sp38d_ctx->keytype, sp38d_ctx->keyslot));

        dma_sg_desc_write_crypto_config(desc, cfg, sp38d_ctx->incj0);
        dma_sg_desc_append_to_list(desc);

        if (!sp38d_ctx->start)
            sp38d_ctx->start = true;

        switch (data_type) {
        case GCM_AAD:
            sp38d_ctx->aadbits += descs[index].length << 3;
            break;
        case GCM_PLAINTEXT:
            sp38d_ctx->inbits += descs[index].length << 3;
            sp38d_ctx->incj0 = 1 + ((sp38d_ctx->inbits >> 3) / BC_BLOCK_SIZE);
            break;
        default:
            return E_FIRMWARE;
        }
    }

    // reset start bit for incoming TEXT data.
    if (data_type == GCM_AAD && last)
        sp38d_ctx->start = false;

    desc = dma_sg_new_write_ctx_descriptor((uintptr_t)sp38d_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    return SUCCESS;
}

static pufs_status_t sp38d_tag(pufs_sp38d_ctx* sp38d_ctx, uint8_t* tag, uint32_t taglen, bool from_reg)
{
    uint32_t val32, tmplen = 0;
    pufs_status_t check;
    union {
        uint8_t uc[BC_BLOCK_SIZE];
        uint32_t u32[BC_BLOCK_SIZE / 4];
    } tmp;

    if (sp38d_ctx->op == SP38D_GHASH) {
        rvv_memcpy(tag, sp38d_ctx->ghash, taglen);
        return SUCCESS;
    }

    // len(A) || len(C)
    tmp.u32[0] = be2le((uint32_t)(sp38d_ctx->aadbits >> 32));
    tmp.u32[1] = be2le((uint32_t)(sp38d_ctx->aadbits));
    tmp.u32[2] = be2le((uint32_t)(sp38d_ctx->inbits >> 32));
    tmp.u32[3] = be2le((uint32_t)sp38d_ctx->inbits);
    if ((check = __sp38d_ctx_update(sp38d_ctx, NULL, NULL, tmp.uc,
             BC_BLOCK_SIZE, true))
        != SUCCESS)
        return check;

    // last GCTR
    sp38d_ctx->inbits = ULLONG_MAX;

    if (!from_reg) {
        if (((check = __sp38d_ctx_update(sp38d_ctx,
                  tmp.uc, &tmplen, sp38d_ctx->ghash,
                  BC_BLOCK_SIZE, true))
                != SUCCESS)
            || (tmplen != BC_BLOCK_SIZE))
            return E_FIRMWARE;

        rvv_memcpy(tag, tmp.uc, taglen);
        return SUCCESS;
    }

    crypto_write_iv(sp38d_ctx->j0, BC_BLOCK_SIZE);

    if (sp38d_ctx->keytype == SWKEY)
        crypto_write_sw_key(sp38d_ctx->key, SW_KEY_MAXLEN);

    if ((check = sp38d_get_config(&val32, sp38d_ctx, true, true, true)) != SUCCESS)
        return check;

    sp38d_regs->cfg = val32;
    sp38d_regs->block_num = 0;

    dma_write_data_block_config(true, true, true, true, 0);
    dma_write_rwcfg(NULL, NULL, 0);
    dma_write_config_0(false, false, false);
    dma_write_key_config_0(sp38d_ctx->keytype,
        ALGO_TYPE_GCM,
        sp38d_ctx->keybits,
        get_key_slot_idx(sp38d_ctx->keytype, sp38d_ctx->keyslot));

    dma_write_start();

    while (dma_check_busy_status(&val32))
        ;

    if (val32 != 0) {
        LOG_ERROR("DMA status 0: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    val32 = sp38d_regs->status;
    if ((val32 & SP38D_STATUS_RESP_MASK) != 0) {
        LOG_ERROR("SP38D status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }
    crypto_read_dgest(tag, taglen);

    return SUCCESS;
}

static pufs_status_t sp38d_ctx_sg_done(pufs_sp38d_ctx* sp38d_ctx, uint8_t* tag, uint32_t taglen)
{
    pufs_status_t check;

    if (sp38d_ctx->crypto_io_ctx)
        crypto_io_read_dgest(sp38d_ctx->crypto_io_ctx, sp38d_ctx->ghash, BC_BLOCK_SIZE);

    check = sp38d_tag(sp38d_ctx, tag, taglen, true);

    crypto_free_crypto_io_ctx(sp38d_ctx->crypto_io_ctx);
    sp38d_ctx->crypto_io_ctx = NULL;
    sp38d_ctx->op = SP38D_AVAILABLE;
    return check;
}

/**
 * @brief Finalize current GCM operation mode
 *
 * @param[in]  op         GCM operation mode.
 * @param[in]  sp38d_ctx  SP38D context.
 * @param[in]  encrypt    True/false for encryption/decryption
 * @param[out] out        Output data.
 * @param[out] outlen     Output data length in bytes.
 * @param[out] tag        Output tag.
 * @param[in]  taglen     Specified output tag length in bytes.
 * @return                SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_ctx_final(sp38d_op op,
    pufs_sp38d_ctx* sp38d_ctx,
    bool encrypt,
    uint8_t* out,
    uint32_t* outlen,
    uint8_t* tag,
    uint32_t taglen,
    bool from_reg)
{
    pufs_status_t check = SUCCESS;

    if (outlen != NULL)
        *outlen = 0;
    // check sp38d_ctx is owned by this operation (GCM mode)
    if ((sp38d_ctx->op != op) || (sp38d_ctx->encrypt != encrypt))
        return E_UNAVAIL;
    if (sp38d_ctx->buflen != 0) {
        if ((check = __sp38d_ctx_update(sp38d_ctx, out, outlen, sp38d_ctx->buff,
                 sp38d_ctx->buflen, true))
            != SUCCESS)
            goto release_sp38d;
    }

    check = sp38d_tag(sp38d_ctx, tag, taglen, from_reg);

release_sp38d:
    // release sp38d context
    sp38d_ctx->op = SP38D_AVAILABLE;

    return check;
}
/**
 * @brief Initialize GCM context for GHASH operation
 *
 * @param[in] sp38d_ctx  SP38D context to be initialized.
 * @param[in] cipher     Block cipher algorithm.
 * @param[in] keytype    Key type.
 * @param[in] keyaddr    Key address.
 * @param[in] keybits    Key length in bits.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_ghash_init(pufs_sp38d_ctx* sp38d_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    return sp38d_ctx_init(SP38D_GHASH, sp38d_ctx, false, cipher,
        keytype, keyaddr, keybits, NULL);
}
/**
 * @brief Input data into GHASH
 *
 * @param[in] sp38d_ctx  SP38D context.
 * @param[in] in         Input data.
 * @param[in] inlen      Input data length in bytes.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_ghash_update(pufs_sp38d_ctx* sp38d_ctx,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38d_ctx_update(SP38D_GHASH, sp38d_ctx, false,
        NULL, NULL, in, inlen);
}
/**
 * @brief Extract GHASH
 *
 * @param[in]  sp38d_ctx  SP38D context.
 * @param[out] out        GHASH value.
 * @param[in]  outlen     GHASH value length in bytes.
 * @return                SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_ghash_final(pufs_sp38d_ctx* sp38d_ctx,
    uint8_t* out,
    uint32_t outlen)
{
    return sp38d_ctx_final(SP38D_GHASH, sp38d_ctx, false,
        NULL, NULL, out, outlen, false);
}
/**
 * @brief Build J_0 for GCM and GMAC operation
 *
 * @param[out] j0       J_0.
 * @param[in]  cipher   Block cipher algorithm.
 * @param[in]  keytype  Key type.
 * @param[in]  keyaddr  Key address.
 * @param[in]  keybits  Key length in bits.
 * @param[in]  iv       Initial vector.
 * @param[in]  ivlen    Initial vector length in bytes.
 * @return              SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_build_j0(uint8_t* j0,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    uint32_t ivlen)
{
    uint8_t tmp[BC_BLOCK_SIZE];
    uint64_t ivbits = ivlen << 3;
    pufs_status_t check;

    if ((iv == NULL) || (ivlen == 0))
        return E_INVALID;

    if (ivlen == 12) {
        rvv_memcpy(j0, iv, ivlen);
        rvv_memset(j0 + ivlen, 0, 3);
        *(j0 + 15) = 1;
        return SUCCESS;
    }

    pufs_sp38d_ctx sp38d_ctx = { .op = SP38D_AVAILABLE };
    if ((check = sp38d_ghash_init(&sp38d_ctx, cipher, keytype,
             keyaddr, keybits))
        != SUCCESS)
        return check;
    if ((check = sp38d_ghash_update(&sp38d_ctx, iv, ivlen)) != SUCCESS)
        return check;
    rvv_memset(tmp, 0, BC_BLOCK_SIZE);
    if ((ivlen % BC_BLOCK_SIZE) != 0) {
        uint32_t padlen = BC_BLOCK_SIZE - (ivlen % BC_BLOCK_SIZE);
        if ((check = sp38d_ghash_update(&sp38d_ctx, tmp, padlen)) != SUCCESS)
            return check;
    }
    *((uint32_t*)(tmp + 8)) = be2le((uint32_t)(ivbits >> 32));
    *((uint32_t*)(tmp + 12)) = be2le((uint32_t)ivbits);
    if ((check = sp38d_ghash_update(&sp38d_ctx, tmp, BC_BLOCK_SIZE)) != SUCCESS)
        return check;

    return sp38d_ghash_final(&sp38d_ctx, j0, BC_BLOCK_SIZE);
}
/**
 * @brief Step forward SP38D stage
 *
 * @param[in] sp38d_ctx  SP38D context.
 * @param[in] stage      Next SP38D stage.
 * @return             SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38d_step_stage(pufs_sp38d_ctx* sp38d_ctx,
    sp38d_stage stage)
{
    switch (sp38d_ctx->stage) {
    case SP38D_NONE:
        break;
    case SP38D_AAD:
        switch (stage) {
        case SP38D_AAD:
            return SUCCESS;
        case SP38D_TEXT:
            if (sp38d_ctx->buflen != 0) { // clear AAD and start input
                pufs_status_t check;
                check = __sp38d_ctx_update(sp38d_ctx,
                    NULL, NULL, sp38d_ctx->buff,
                    sp38d_ctx->buflen, true);
                if (check != SUCCESS)
                    return check;
                sp38d_ctx->buflen = 0;
            }
            break;
        default:
            return E_FIRMWARE;
        }
        break;
    case SP38D_TEXT:
        if (stage != SP38D_TEXT)
            return E_INVALID;
        break;
    default:
        return E_FIRMWARE;
    }

    if (sp38d_ctx->stage != stage) {
        sp38d_ctx->start = false;
        sp38d_ctx->stage = stage;
    }
    return SUCCESS;
}
/**
 * @brief Initialize GCM context for GCM operation
 *
 * @param[in] sp38d_ctx  SP38D context.
 * @param[in] cipher     Block cipher algorithm.
 * @param[in] keytype    Key type.
 * @param[in] keyaddr    Key address.
 * @param[in] keybits    Key length in bits.
 * @param[in] iv         Initial vector.
 * @param[in] ivlen      Initial vector length in bytes.
 * @param[in] encrypt    True/false for encryption/decryption.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t pufs_gcm_init(pufs_sp38d_ctx* sp38d_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    uint32_t ivlen,
    bool encrypt)
{
    uint8_t j0[BC_BLOCK_SIZE];
    pufs_status_t check;
    if ((check = sp38d_build_j0(j0, cipher, keytype, keyaddr,
             keybits, iv, ivlen))
        != SUCCESS)
        return check;
    return sp38d_ctx_init(SP38D_GCM, sp38d_ctx, encrypt, cipher,
        keytype, keyaddr, keybits, j0);
}

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * pufs_gdle_gcm_prepare()
 */
pufs_status_t pufs_gdle_gcm_prepare(pufs_sp38d_ctx* sp38d_ctx,
    const uint8_t* out,
    uint32_t inlen)
{
    if (out == NULL)
        return E_INVALID;
    pufs_status_t check = sp38d_step_stage(sp38d_ctx, SP38D_TEXT);
    if (check != SUCCESS)
        return check;
    sp38d_ctx->inbits += (((uint64_t)inlen) << 3);
    return sp38d_prepare(sp38d_ctx, out, inlen);
}
/**
 * pufs_gdle_gcm_postproc()
 */
pufs_status_t pufs_gdle_gcm_postproc(pufs_sp38d_ctx* sp38d_ctx)
{
    return sp38d_postproc(sp38d_ctx);
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_sp38d_module_init(uint32_t sp38d_offset)
{
    sp38d_regs = (struct pufs_sp38d_regs*)(pufs_context.base_addr + sp38d_offset);
    version_check(SP38D_VERSION, sp38d_regs->version);
    LOG_INFO("%s", "SP38D module is initialized");
}
/**
 * pufs_sp38d_ctx_new()
 */
pufs_sp38d_ctx* pufs_sp38d_ctx_new(void)
{
    pufs_sp38d_ctx* ret;

    ret = malloc(sizeof(pufs_sp38d_ctx));
    if (ret != NULL) {
        ret->op = SP38D_AVAILABLE;
        rvv_memset(ret, 0x0, sizeof(pufs_sp38d_ctx));
    }

    return ret;
}
/**
 * pufs_sp38d_ctx_free()
 */
void pufs_sp38d_ctx_free(pufs_sp38d_ctx* sp38d_ctx)
{
    if (sp38d_ctx != NULL) {
        rvv_memset(sp38d_ctx, 0, sizeof(pufs_sp38d_ctx));
        sp38d_ctx->op = SP38D_AVAILABLE;
    }
    free(sp38d_ctx);
}
/**
 * _pufs_enc_gcm()
 */
pufs_status_t _pufs_enc_gcm_init(pufs_sp38d_ctx* sp38d_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    uint32_t ivlen)
{
    return pufs_gcm_init(sp38d_ctx, cipher, keytype, keyaddr, keybits,
        iv, ivlen, true);
}
pufs_status_t pufs_enc_gcm_update(pufs_sp38d_ctx* sp38d_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    pufs_status_t check = sp38d_step_stage(sp38d_ctx, (out == NULL) ? SP38D_AAD : SP38D_TEXT);
    if (check != SUCCESS)
        return check;
    return sp38d_ctx_update(SP38D_GCM, sp38d_ctx, true, out, outlen, in, inlen);
}

pufs_status_t pufs_enc_gcm_sg_append(pufs_sp38d_ctx* sp38d_ctx,
    pufs_gcm_input_data_t data_type,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38d_ctx_sg_append(SP38D_GCM, sp38d_ctx, true, data_type, descs, descs_len, last);
}

pufs_status_t pufs_enc_gcm_sg_done(pufs_sp38d_ctx* sp38d_ctx, uint8_t* tag, uint32_t taglen)
{
    return sp38d_ctx_sg_done(sp38d_ctx, tag, taglen);
}

pufs_status_t pufs_enc_gcm_final(pufs_sp38d_ctx* sp38d_ctx,
    uint8_t* out,
    uint32_t* outlen,
    uint8_t* tag,
    uint32_t taglen)
{
    return sp38d_ctx_final(SP38D_GCM, sp38d_ctx, true,
        out, outlen, tag, taglen, false);
}
pufs_status_t _pufs_enc_gcm(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    uint32_t ivlen,
    const uint8_t* aad,
    uint32_t aadlen,
    uint8_t* tag,
    uint32_t taglen)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38d_ctx sp38d_ctx = { .op = SP38D_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_enc_gcm_init(&sp38d_ctx, cipher, keytype, keyaddr,
             keybits, iv, ivlen))
        != SUCCESS)
        return check;
    if ((check = pufs_enc_gcm_update(&sp38d_ctx, NULL, NULL,
             aad, aadlen))
        != SUCCESS)
        return check;
    if ((check = pufs_enc_gcm_update(&sp38d_ctx, out, &toutlen,
             in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;

    if ((check = sp38d_ctx_final(SP38D_GCM, &sp38d_ctx, true, out + *outlen, &toutlen,
             tag, taglen, true))
        != SUCCESS)
        return check;

    *outlen += toutlen;
    return check;
}
/**
 * pufs_dec_gcm()
 */
pufs_status_t _pufs_dec_gcm_init(pufs_sp38d_ctx* sp38d_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    uint32_t ivlen)
{
    return pufs_gcm_init(sp38d_ctx, cipher, keytype, keyaddr, keybits,
        iv, ivlen, false);
}
pufs_status_t pufs_dec_gcm_update(pufs_sp38d_ctx* sp38d_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    pufs_status_t check = sp38d_step_stage(sp38d_ctx, (out == NULL) ? SP38D_AAD : SP38D_TEXT);
    if (check != SUCCESS)
        return check;
    return sp38d_ctx_update(SP38D_GCM, sp38d_ctx, false,
        out, outlen, in, inlen);
}
pufs_status_t pufs_dec_gcm_sg_append(pufs_sp38d_ctx* sp38d_ctx,
    pufs_gcm_input_data_t data_type,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38d_ctx_sg_append(SP38D_GCM, sp38d_ctx, false, data_type, descs, descs_len, last);
}

pufs_status_t pufs_dec_gcm_final_tag(pufs_sp38d_ctx* sp38d_ctx,
    uint8_t* out,
    uint32_t* outlen,
    uint8_t* tag,
    uint32_t taglen)
{
    return sp38d_ctx_final(SP38D_GCM, sp38d_ctx, false,
        out, outlen, tag, taglen, false);
}

pufs_status_t pufs_dec_gcm_sg_done(pufs_sp38d_ctx* sp38d_ctx, const uint8_t* tag, uint32_t taglen)
{
    uint8_t newtag[BC_BLOCK_SIZE];
    pufs_status_t check;

    if ((check = sp38d_ctx_sg_done(sp38d_ctx, newtag, taglen)) != SUCCESS)
        return check;

    return ((memcmp(tag, newtag, taglen) == 0) ? SUCCESS : E_VERFAIL);
}

pufs_status_t pufs_dec_gcm_final(pufs_sp38d_ctx* sp38d_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* tag,
    uint32_t taglen)
{
    uint8_t newtag[BC_BLOCK_SIZE];
    pufs_status_t check;
    if ((check = pufs_dec_gcm_final_tag(sp38d_ctx, out, outlen,
             newtag, taglen))
        != SUCCESS)
        return check;
    return ((memcmp(tag, newtag, taglen) == 0) ? SUCCESS : E_VERFAIL);
}
pufs_status_t _pufs_dec_gcm(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* iv,
    int ivlen,
    const uint8_t* aad,
    int aadlen,
    const uint8_t* tag,
    int taglen)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38d_ctx sp38d_ctx = { .op = SP38D_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_dec_gcm_init(&sp38d_ctx, cipher, keytype, keyaddr,
             keybits, iv, ivlen))
        != SUCCESS)
        return check;
    if ((check = pufs_dec_gcm_update(&sp38d_ctx, NULL, NULL,
             aad, aadlen))
        != SUCCESS)
        return check;
    if ((check = pufs_dec_gcm_update(&sp38d_ctx, out, &toutlen,
             in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;

    if ((check = pufs_dec_gcm_final(&sp38d_ctx, out + *outlen, &toutlen,
             tag, taglen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
