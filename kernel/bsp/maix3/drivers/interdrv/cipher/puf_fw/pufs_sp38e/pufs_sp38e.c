/**
 * @file      pufs_sp38e.c
 * @brief     PUFsecurity SP38E API implementation
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
#include "pufs_crypto_internal.h"
#include "pufs_sp38e_internal.h"
#include "pufs_ka_internal.h"
#include "pufs_dma_internal.h"

struct pufs_sp38e_regs* sp38e_regs = NULL;

/*****************************************************************************
 * Static variables
 ****************************************************************************/

/*****************************************************************************
 * Static functions
 ****************************************************************************/
static bool sp38e_check_sgdma_descriptors(pufs_dma_sg_desc_st* descs, uint32_t descs_len, bool last)
{
    if (descs == NULL || descs_len == 0)
        return false;

    for (uint32_t i = 0; i < descs_len; i++) {
        if ((last && descs[i].length < BC_BLOCK_SIZE) || (!last && descs[i].length % BC_BLOCK_SIZE != 0))
            return false;
    }
    return true;
}

static pufs_status_t sp38e_get_config(uint32_t* cfg, pufs_sp38e_ctx* ctx)
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
    switch (ctx->op) {
    case SP38E_TWEAK:
        val32 |= 1 << 2 | (ctx->j & 0x000fffff) << 8;
        break;
    case SP38E_XTS:
        val32 |= (ctx->encrypt ? 0x1 : 0x0) << 3;
        break;
    default:
        return E_FIRMWARE;
    }

    *cfg = val32;

    return SUCCESS;
}

/**
 * @brief Initialize the internal context for block cipher XTS mode
 *
 * @param[in] sp38e_ctx  SP38E context to be initialized.
 * @param[in] encrypt    True/false for encryption/decryption.
 * @param[in] cipher     Block cipher algorithm.
 * @param[in] keytype1   Key1 key type.
 * @param[in] keyaddr1   Key1 key address.
 * @param[in] keybits    Each key length in bits.
 * @param[in] keytype2   Key2 key type.
 * @param[in] keyaddr2   Key2 key address.
 * @param[in] i          Tweak value.
 * @param[in] j          Sequence number of the first input data.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38e_ctx_init(pufs_sp38e_ctx* sp38e_ctx,
    bool encrypt,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype1,
    size_t keyaddr1,
    uint32_t keybits,
    pufs_key_type_t keytype2,
    size_t keyaddr2,
    const uint8_t* i,
    uint32_t j)
{
    pufs_status_t check;
    // abort if sp38e_ctx is occupied
    if (sp38e_ctx->op != SP38E_AVAILABLE)
        return E_BUSY;
    // check keytype
    if ((keytype1 == PUFKEY) || (keytype1 == SHARESEC) || (keytype2 == PUFKEY) || (keytype2 == SHARESEC))
        return E_DENY;
    // check feature with key length
    if ((check = crypto_check_sp38e_algo(cipher, keybits)) != SUCCESS)
        return check;

    // check key settings for block cipher
    if ((keytype1 != SWKEY) && ((check = keyslot_check(true, keytype1, (uint32_t)keyaddr1, keybits)) != SUCCESS))
        return check;
    if ((keytype2 != SWKEY) && ((check = keyslot_check(true, keytype2, (uint32_t)keyaddr2, keybits)) != SUCCESS))
        return check;

    // check i
    if (i == NULL)
        return E_INVALID;

    // initialize for block-cipher XTS mode
    sp38e_ctx->buflen = 0;
    sp38e_ctx->cipher = cipher;
    sp38e_ctx->encrypt = encrypt;
    sp38e_ctx->op = SP38E_TWEAK;
    sp38e_ctx->minlen = 16;
    sp38e_ctx->j = j;
    sp38e_ctx->start = false;
    sp38e_ctx->crypto_io_ctx = NULL;
    rvv_memcpy(sp38e_ctx->i, i, BC_BLOCK_SIZE);

    // set key
    sp38e_ctx->keybits = keybits;
    sp38e_ctx->keytype1 = keytype1;
    sp38e_ctx->keytype2 = keytype2;
    if (keytype1 != SWKEY)
        sp38e_ctx->keyslot1 = (uint32_t)keyaddr1;
    else
        rvv_memcpy(sp38e_ctx->key1, (const void*)keyaddr1, b2B(keybits));
    if (keytype2 != SWKEY)
        sp38e_ctx->keyslot2 = (uint32_t)keyaddr2;
    else
        rvv_memcpy(sp38e_ctx->key2, (const void*)keyaddr2, b2B(keybits));

    return SUCCESS;
}
/**
 * @brief Prepare registers for SP38E operation
 *
 * @param[in] sp38e_ctx  SP38E context.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38e_prepare(pufs_sp38e_ctx* sp38e_ctx)
{
    uint32_t val32;
    pufs_status_t check;

    switch (sp38e_ctx->op) {
    case SP38E_TWEAK:
        if (sp38e_ctx->keytype2 == SWKEY)
            crypto_write_sw_key(sp38e_ctx->key2, SW_KEY_MAXLEN);

        dma_write_key_config_0(sp38e_ctx->keytype2,
            ALGO_TYPE_XTS,
            sp38e_ctx->keybits,
            get_key_slot_idx(sp38e_ctx->keytype2, sp38e_ctx->keyslot2));
        break;
    case SP38E_XTS:
        if (sp38e_ctx->keytype1 == SWKEY)
            crypto_write_sw_key(sp38e_ctx->key1, SW_KEY_MAXLEN);

        dma_write_key_config_0(sp38e_ctx->keytype1,
            ALGO_TYPE_XTS,
            sp38e_ctx->keybits,
            get_key_slot_idx(sp38e_ctx->keytype1, sp38e_ctx->keyslot1));
        break;
    default:
        return E_FIRMWARE;
    }

    if ((check = sp38e_get_config(&val32, sp38e_ctx)) != SUCCESS)
        return check;

    sp38e_regs->cfg = val32;

    crypto_write_iv(sp38e_ctx->i, BC_BLOCK_SIZE);

    return SUCCESS;
}
/**
 * @brief Post-processing for SP38E operation
 *
 * @param[in] sp38e_ctx  SP38E context.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38e_postproc(pufs_sp38e_ctx* sp38e_ctx)
{
    crypto_read_iv(sp38e_ctx->i, BC_BLOCK_SIZE);
    return SUCCESS;
}
/**
 * @brief Starting SP38E and wait until done
 *
 * @return  SUCCESS on success, otherwise an error code.
 */
static pufs_status_t pufs_sp38e_start(void)
{
    uint32_t val32;
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

    val32 = sp38e_regs->status;
    if ((val32 & SP38E_STATUS_RESP_MASK) != 0) {
        LOG_ERROR("SP38E status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    return SUCCESS;
}
/**
 * @brief Tweak calculation
 *
 * @param[in] sp38e_ctx  SP38E context.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38e_calculate_tweak(pufs_sp38e_ctx* sp38e_ctx)
{
    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_config_0(false, false, false);

    dma_write_rwcfg(NULL, NULL, 0);

    dma_write_data_block_config(true, true, true, true, 0);

    pufs_status_t check;
    if ((check = sp38e_prepare(sp38e_ctx)) != SUCCESS)
        return check;
    if ((check = pufs_sp38e_start()) != SUCCESS)
        return check;
    return sp38e_postproc(sp38e_ctx);
}
/**
 * @brief Pass the input into SP38E hardware
 *
 * @param[in]  sp38e_ctx  SP38E context.
 * @param[out] out        The pointer to the space where the output is written.
 * @param[out] outlen     The length of the output in bytes.
 * @param[in]  in         The input.
 * @param[in]  inlen      The length of the input in bytes.
 * @param[in]  descs      SGDMA descriptors contains input message
 * @param[in]  descs_len  the length of SGDMA descriptors
 * @param[in]  last       True if the input for this operation ends
 * @return                SUCCESS on success, otherwise an error code.
 */
static pufs_status_t ___sp38e_ctx_update(pufs_sp38e_ctx* sp38e_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    bool last)
{
    pufs_status_t check;

    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_config_0(false, false, false);

    dma_write_data_block_config(sp38e_ctx->start ? false : true, last, true, true, 0);

    if ((check = sp38e_prepare(sp38e_ctx)) != SUCCESS)
        return check;

    dma_write_rwcfg(out, in, inlen);
    if ((check = pufs_sp38e_start()) != SUCCESS)
        return check;

    dma_read_output(out, inlen);
    *outlen = inlen;

    return sp38e_postproc(sp38e_ctx);
}
static pufs_status_t __sp38e_ctx_update(pufs_sp38e_ctx* sp38e_ctx,
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
        ret = ___sp38e_ctx_update(sp38e_ctx, out, outlen, in, len, last);
        if (ret != SUCCESS)
            return ret;
        in += len;
        inlen -= len;
        if (outlen && out) {
            out += *outlen;
            total_len += *outlen;
            *outlen = total_len;
        }
        if (sp38e_ctx->start == false)
            sp38e_ctx->start = true;
    } while (inlen);
    return SUCCESS;
}
/**
 * @brief Input data into SP38E
 *
 * @param[in]  sp38e_ctx  SP38E context.
 * @param[in]  encrypt    True/false for encryption/decryption
 * @param[out] out        Output data.
 * @param[out] outlen     Output data length in bytes.
 * @param[in]  in         Input data.
 * @param[in]  inlen      Input data length in bytes.
 * @return                SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38e_ctx_update(pufs_sp38e_ctx* sp38e_ctx,
    bool encrypt,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    // check sp38e_ctx is owned by this operation (mode of operation/encrypt)
    if ((sp38e_ctx->op == SP38E_AVAILABLE) || (sp38e_ctx->encrypt != encrypt))
        return E_UNAVAIL;
    // continue if msg is NULL or msglen is zero
    *outlen = 0;
    if ((in == NULL) || (inlen == 0))
        return SUCCESS;

    pufs_status_t check = SUCCESS;
    if (sp38e_ctx->op == SP38E_TWEAK) {
        if ((check = sp38e_calculate_tweak(sp38e_ctx)) != SUCCESS)
            goto release_sp38e;
        sp38e_ctx->op = SP38E_XTS;
    }

    uint32_t seglen = 0;
    blsegs segs = segment(sp38e_ctx->buff, sp38e_ctx->buflen, in, inlen,
        BC_BLOCK_SIZE, sp38e_ctx->minlen);
    sp38e_ctx->buflen = 0;

    for (uint32_t i = 0; i < segs.nsegs; i++) {
        if (segs.seg[i].process) // process
        {
            if ((check = __sp38e_ctx_update(sp38e_ctx, out + *outlen, &seglen,
                     segs.seg[i].addr, segs.seg[i].len, false))
                != SUCCESS)
                goto release_sp38e;
            *outlen += seglen;
        } else // keep in the internal buffer
        {
            if ((segs.seg[i].addr == sp38e_ctx->buff) && (sp38e_ctx->buflen == 0)) { // skip copy what already in the right place
                sp38e_ctx->buflen += segs.seg[i].len;
            } else // copy into the buffer
            {
                rvv_memcpy(sp38e_ctx->buff + sp38e_ctx->buflen, segs.seg[i].addr, segs.seg[i].len);
                sp38e_ctx->buflen += segs.seg[i].len;
            }
        }
    }

    return SUCCESS;

release_sp38e:
    // release sp38e context
    sp38e_ctx->op = SP38E_AVAILABLE;
    return check;
}

static pufs_status_t sp38e_set_crypto_io_ctx(pufs_sp38e_ctx* ctx)
{
    pufs_status_t check;
    if (ctx->crypto_io_ctx != NULL)
        return SUCCESS;

    ctx->crypto_io_ctx = crypto_new_crypto_io_ctx();

    if ((check = sp38e_calculate_tweak(ctx)) != SUCCESS)
        return check;

    ctx->op = SP38E_XTS;
    if (ctx->keytype1 == SWKEY)
        crypto_io_write_sw_key(ctx->crypto_io_ctx, ctx->key1, SW_KEY_MAXLEN);

    crypto_io_write_iv(ctx->crypto_io_ctx, ctx->i, BC_BLOCK_SIZE);

    return SUCCESS;
}

static pufs_status_t sp38e_ctx_sg_append(pufs_sp38e_ctx* sp38e_ctx,
    bool encrypt,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    uint32_t cfg;
    pufs_status_t check;
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_dsc_attr_st* attr;
    pufs_dma_sg_desc_opts_st opts = { .offset = 0x0, .done_interrupt = false, .done_pause = false };

    if ((sp38e_ctx->op == SP38E_AVAILABLE) || (sp38e_ctx->encrypt != encrypt))
        return E_UNAVAIL;

    if (sp38e_check_sgdma_descriptors(descs, descs_len, last) != true)
        return E_INVALID;

    if ((check = sp38e_set_crypto_io_ctx(sp38e_ctx)) != SUCCESS)
        return check;

    desc = dma_sg_new_read_ctx_descriptor((uintptr_t)sp38e_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    if ((check = sp38e_get_config(&cfg, sp38e_ctx)) != SUCCESS)
        return check;

    for (uint32_t index = 0; index < descs_len; index++) {
        desc = dma_sg_new_data_descriptor();
        if (desc == NULL)
            return E_FIRMWARE;

        attr = &descs[index].attr;
        opts.head = !sp38e_ctx->start;

        if (index == descs_len - 1)
            opts.tail = last;
        else
            opts.tail = false;

        dma_sg_desc_write_addr(desc, descs[index].write_addr, descs[index].read_addr, descs[index].length);
        dma_sg_desc_write_dsc_config(desc, attr, &opts);
        dma_sg_desc_write_key_config(desc, sp38e_ctx->keytype1,
            ALGO_TYPE_XTS, sp38e_ctx->keybits,
            get_key_slot_idx(sp38e_ctx->keytype1, sp38e_ctx->keyslot1));

        dma_sg_desc_write_crypto_config(desc, cfg, 0x0);
        dma_sg_desc_append_to_list(desc);

        if (!sp38e_ctx->start)
            sp38e_ctx->start = true;
    }

    desc = dma_sg_new_write_ctx_descriptor((uintptr_t)sp38e_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    return SUCCESS;
}

static pufs_status_t sp38e_ctx_sg_done(pufs_sp38e_ctx* sp38e_ctx)
{
    crypto_free_crypto_io_ctx(sp38e_ctx->crypto_io_ctx);
    sp38e_ctx->crypto_io_ctx = NULL;
    sp38e_ctx->op = SP38E_AVAILABLE;
    return SUCCESS;
}

/**
 * @brief Finalize current XTS operation mode
 *
 * @param[in]  sp38e_ctx  SP38E context.
 * @param[in]  encrypt    True/false for encryption/decryption
 * @param[out] out        Output data.
 * @param[out] outlen     Output data length in bytes.
 * @return                SUCCESS on success, otherwise an error code.
 */
static pufs_status_t sp38e_ctx_final(pufs_sp38e_ctx* sp38e_ctx,
    bool encrypt,
    uint8_t* out,
    uint32_t* outlen)
{
    pufs_status_t check = SUCCESS;

    // check sp38e_ctx is owned by this operation (mode of operation/encrypt)
    if ((sp38e_ctx->op == SP38E_AVAILABLE) || (sp38e_ctx->encrypt != encrypt))
        return E_UNAVAIL;

    // in final call, it must be minimum-length bytes
    if (sp38e_ctx->buflen < sp38e_ctx->minlen)
        check = E_INVALID;
    else
        check = __sp38e_ctx_update(sp38e_ctx, out, outlen, sp38e_ctx->buff,
            sp38e_ctx->buflen, true);

    // release sp38e context
    sp38e_ctx->op = SP38E_AVAILABLE;
    return check;
}

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * pufs_gdle_xts_prepare()
 */
pufs_status_t pufs_gdle_xts_prepare(pufs_sp38e_ctx* sp38e_ctx)
{
    pufs_status_t check;

    if (sp38e_ctx->op == SP38E_TWEAK) {
        if ((check = sp38e_calculate_tweak(sp38e_ctx)) != SUCCESS)
            return check;
        sp38e_ctx->op = SP38E_XTS;
    }
    return sp38e_prepare(sp38e_ctx);
}
/**
 * pufs_gdle_xts_postproc()
 */
pufs_status_t pufs_gdle_xts_postproc(pufs_sp38e_ctx* sp38e_ctx)
{
    return sp38e_postproc(sp38e_ctx);
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_sp38e_module_init(uint32_t sp38e_offset)
{
    sp38e_regs = (struct pufs_sp38e_regs*)(pufs_context.base_addr + sp38e_offset);
    version_check(SP38E_VERSION, sp38e_regs->version);
    LOG_INFO("%s", "SP38E module is initialized");
}
/**
 * pufs_sp38e_ctx_new()
 */
pufs_sp38e_ctx* pufs_sp38e_ctx_new(void)
{
    pufs_sp38e_ctx* ret;

    ret = malloc(sizeof(pufs_sp38e_ctx));
    if (ret != NULL) {
        ret->op = SP38E_AVAILABLE;
        rvv_memset(ret, 0x0, sizeof(pufs_sp38e_ctx));
    }

    return ret;
}
/**
 * pufs_sp38e_ctx_free()
 */
void pufs_sp38e_ctx_free(pufs_sp38e_ctx* sp38e_ctx)
{
    if (sp38e_ctx != NULL) {
        rvv_memset(sp38e_ctx, 0, sizeof(pufs_sp38e_ctx));
        sp38e_ctx->op = SP38E_AVAILABLE;
    }
    free(sp38e_ctx);
}
/**
 * _pufs_enc_xts()
 */
pufs_status_t _pufs_enc_xts_init(pufs_sp38e_ctx* sp38e_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype1,
    size_t keyaddr1,
    uint32_t keybits,
    pufs_key_type_t keytype2,
    size_t keyaddr2,
    const uint8_t* i,
    uint32_t j)
{
    return sp38e_ctx_init(sp38e_ctx, true, cipher, keytype1, keyaddr1, keybits,
        keytype2, keyaddr2, i, j);
}
pufs_status_t pufs_enc_xts_update(pufs_sp38e_ctx* sp38e_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38e_ctx_update(sp38e_ctx, true, out, outlen, in, inlen);
}

pufs_status_t pufs_enc_xts_sg_append(pufs_sp38e_ctx* sp38e_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38e_ctx_sg_append(sp38e_ctx, true, descs, descs_len, last);
}

pufs_status_t pufs_enc_xts_sg_done(pufs_sp38e_ctx* sp38e_ctx)
{
    return sp38e_ctx_sg_done(sp38e_ctx);
}

pufs_status_t pufs_enc_xts_final(pufs_sp38e_ctx* sp38e_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38e_ctx_final(sp38e_ctx, true, out, outlen);
}
pufs_status_t _pufs_enc_xts(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype1,
    size_t keyaddr1,
    uint32_t keybits,
    pufs_key_type_t keytype2,
    size_t keyaddr2,
    const uint8_t* i,
    uint32_t j)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38e_ctx sp38e_ctx = { .op = SP38E_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_enc_xts_init(&sp38e_ctx, cipher, keytype1,
             keyaddr1, keybits, keytype2,
             keyaddr2, i, j))
        != SUCCESS)
        return check;
    if ((check = pufs_enc_xts_update(&sp38e_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_enc_xts_final(&sp38e_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
/**
 * _pufs_dec_xts()
 */
pufs_status_t _pufs_dec_xts_init(pufs_sp38e_ctx* sp38e_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype1,
    size_t keyaddr1,
    uint32_t keybits,
    pufs_key_type_t keytype2,
    size_t keyaddr2,
    const uint8_t* i,
    uint32_t j)
{
    return sp38e_ctx_init(sp38e_ctx, false, cipher, keytype1, keyaddr1, keybits,
        keytype2, keyaddr2, i, j);
}
pufs_status_t pufs_dec_xts_update(pufs_sp38e_ctx* sp38e_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    return sp38e_ctx_update(sp38e_ctx, false, out, outlen, in, inlen);
}
pufs_status_t pufs_dec_xts_sg_append(pufs_sp38e_ctx* sp38e_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38e_ctx_sg_append(sp38e_ctx, false, descs, descs_len, last);
}

pufs_status_t pufs_dec_xts_sg_done(pufs_sp38e_ctx* sp38e_ctx)
{
    return sp38e_ctx_sg_done(sp38e_ctx);
}

pufs_status_t pufs_dec_xts_final(pufs_sp38e_ctx* sp38e_ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    return sp38e_ctx_final(sp38e_ctx, false, out, outlen);
}
pufs_status_t _pufs_dec_xts(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype1,
    size_t keyaddr1,
    uint32_t keybits,
    pufs_key_type_t keytype2,
    size_t keyaddr2,
    const uint8_t* i,
    uint32_t j)
{
    pufs_status_t check;
    uint32_t toutlen;
    *outlen = 0;
    pufs_sp38e_ctx sp38e_ctx = { .op = SP38E_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_dec_xts_init(&sp38e_ctx, cipher, keytype1,
             keyaddr1, keybits, keytype2,
             keyaddr2, i, j))
        != SUCCESS)
        return check;
    if ((check = pufs_dec_xts_update(&sp38e_ctx,
             out, &toutlen, in, inlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    if ((check = pufs_dec_xts_final(&sp38e_ctx,
             out + *outlen, &toutlen))
        != SUCCESS)
        return check;
    *outlen += toutlen;
    return check;
}
