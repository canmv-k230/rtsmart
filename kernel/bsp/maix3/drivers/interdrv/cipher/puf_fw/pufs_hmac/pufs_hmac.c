/**
 * @file      pufs_hmac.c
 * @brief     PUFsecurity HMAC API implementation
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
#include "pufs_hmac_internal.h"
#include "pufs_ka_internal.h"
#include "pufs_dma_internal.h"

struct pufs_hmac_regs* hmac_regs = NULL;

/*****************************************************************************
 * Static variables
 ****************************************************************************/

/*****************************************************************************
 * Static functions
 ****************************************************************************/
static pufs_status_t hmac_get_cfg(pufs_hmac_ctx* hmac_ctx, uint32_t* cfg)
{
    switch (hmac_ctx->hash) {
    case SHA_224:
        *cfg = 0x02;
        break;
    case SHA_256:
        *cfg = 0x03;
        break;
    case SHA_384:
        *cfg = 0x04;
        break;
    case SHA_512:
        *cfg = 0x05;
        break;
    case SHA_512_224:
        *cfg = 0x06;
        break;
    case SHA_512_256:
        *cfg = 0x07;
        break;
    case SM3:
        *cfg = 0x08;
        break;
    default:
        return E_FIRMWARE;
    }

    switch (hmac_ctx->op) {
    case HMAC_HASH:
        // the value is 0x0 if op is hash
        break;
    case HMAC_HMAC:
        *cfg = (0x1 << 8) | *cfg;
        break;
    default:
        return E_FIRMWARE;
        ;
    }
    return SUCCESS;
}

static pufs_status_t hmac_set_dgst_length(pufs_hmac_ctx* hmac_ctx,
    pufs_dgst_st* md)
{
    switch (hmac_ctx->hash) {
    case SHA_224:
    case SHA_512_224:
        md->dlen = 28;
        break;
    case SM3:
    case SHA_256:
    case SHA_512_256:
        md->dlen = 32;
        break;
    case SHA_384:
        md->dlen = 48;
        break;
    case SHA_512:
        md->dlen = 64;
        break;
    default:
        return E_FIRMWARE;
    }
    return SUCCESS;
}

static bool hmac_check_sgdma_descriptors(uint32_t block_size, pufs_dma_sg_desc_st* descs, uint32_t descs_len)
{
    if (descs == NULL || descs_len == 0)
        return false;

    for (uint32_t i = 0; i < descs_len; i++) {
        if (i != descs_len - 1 && descs[i].length % block_size != 0)
            return false;
    }
    return true;
}

/**
 * @brief Initialize the internal context for HMAC
 *
 * @param[in] op        The operation.
 * @param[in] hmac_ctx  HMAC context.
 * @param[in] hash      The hash algorithm.
 * @param[in] keytype   The type of source which the key is from.
 * @param[in] keyaddr   The pointer to the space in SWKEY or the slot of the
 *                       source which the key is stored in.
 * @param[in] keybits   The key length in bits.
 * @return              SUCCESS on success, otherwise an error code.
 */
static pufs_status_t hmac_ctx_init(hmac_op op,
    pufs_hmac_ctx* hmac_ctx,
    pufs_hash_t hash,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    pufs_dgst_st stkey;
    pufs_status_t check;
    // abort if hmac_ctx is occupied
    if (hmac_ctx->op != HMAC_AVAILABLE)
        return E_BUSY;
    // check if the hash algorithm is supported
    if (hash >= N_HASH_T)
        return E_INVALID;
    // check keytype
    if ((keytype == PUFKEY) || (keytype == SHARESEC))
        return E_DENY;
    // check if the operation is supported
    uint32_t val32 = hmac_regs->feature;
    if ((op == HMAC_HMAC) && ((val32 & HMAC_HASH_FEATURE_HMAC_MASK) == 0))
        return E_UNSUPPORT;
    switch (hash) {
    case SHA_224:
    case SHA_256:
        if ((val32 & HMAC_HASH_FEATURE_SHA2_MASK) == 0)
            return E_UNSUPPORT;
        break;
    case SHA_384:
    case SHA_512:
    case SHA_512_224:
    case SHA_512_256:
        if ((val32 & HMAC_HASH_FEATURE_SHA2_512_MASK) == 0)
            return E_UNSUPPORT;
        break;
    case SM3:
        if ((val32 & HMAC_HASH_FEATURE_SM3_MASK) == 0)
            return E_UNSUPPORT;
        break;
    default:
        return E_INVALID;
    }
    // check key settings for HMAC
    switch (hash) {
    case SHA_224:
    case SHA_256:
    case SM3:
        hmac_ctx->blocklen = 64;
        break;
    case SHA_384:
    case SHA_512:
    case SHA_512_224:
    case SHA_512_256:
        hmac_ctx->blocklen = 128;
        break;
    default:
        return E_FIRMWARE;
    }
    if (op == HMAC_HMAC) {
        if ((keytype != SWKEY) && ((check = keyslot_check(true, keytype, (uint32_t)keyaddr, keybits)) != SUCCESS))
            return check;
        if ((keytype == SWKEY) && (keybits > B2b(hmac_ctx->blocklen))) {
            if ((check = pufs_hash(&stkey, (const uint8_t*)keyaddr,
                     b2B(keybits), hash))
                != SUCCESS)
                return check;
            keybits = B2b(stkey.dlen);
            keyaddr = (size_t)stkey.dgst;
        }
    }

    // initialize for hash
    hmac_ctx->buflen = 0;
    hmac_ctx->keybits = keybits;
    hmac_ctx->minlen = 1;
    hmac_ctx->keytype = keytype;
    hmac_ctx->curlen = 0;
    hmac_ctx->op = op;
    hmac_ctx->hash = hash;
    hmac_ctx->start = false;

    if (keytype != SWKEY)
        hmac_ctx->keyslot = (uint32_t)keyaddr;
    else {
        rvv_memset(hmac_ctx->key, 0, HMAC_BLOCK_MAXLEN);
        rvv_memcpy(hmac_ctx->key, (const void*)keyaddr, b2B(keybits));
    }

    return SUCCESS;
}

static pufs_status_t __hmac_ctx_final(pufs_hmac_ctx* hmac_ctx,
    pufs_dgst_st* md)
{
    if (md == NULL)
        return E_INVALID;

    crypto_read_dgest(md->dgst, DGST_INT_STATE_LEN);
    return hmac_set_dgst_length(hmac_ctx, md);
}

/**
 * @brief Pass the input into the HMAC hardware
 *
 * @param[in]  hmac_ctx  HMAC context.
 * @param[out] md        The pointer to the space which the digest is written to.
 * @param[in]  msg       The message.
 * @param[in]  msglen    The length of the message in bytes.
 * @param[in]  last      True if the input for this operation ends.
 * @return               SUCCESS on success, otherwise an error code.
 */
static pufs_status_t ___hmac_ctx_update(pufs_hmac_ctx* hmac_ctx,
    pufs_dgst_st* md,
    const uint8_t* msg,
    uint32_t msglen,
    bool last)
{
    uint32_t val32;
    pufs_status_t check;

    if (last && (md == NULL))
        return E_INVALID;

    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_config_0(false, false, false);
    dma_write_data_block_config(hmac_ctx->start ? false : true, last, true, true, 0);

    if (hmac_ctx->op == HMAC_HMAC && hmac_ctx->keytype == SWKEY) {
        crypto_write_sw_key(hmac_ctx->key, SW_KEY_MAXLEN);
        for (int i = 0; i + SW_KEY_MAXLEN < HMAC_BLOCK_MAXLEN; i += 4) {
            val32 = be2le(*((uint32_t*)(hmac_ctx->key + SW_KEY_MAXLEN + i)));
            *((uint32_t*)(hmac_regs->sw_key + i)) = val32;
        }
    }

    dma_write_key_config_0(hmac_ctx->keytype,
        ALGO_TYPE_HMAC,
        (hmac_ctx->keybits < 512) ? hmac_ctx->keybits : 512,
        get_key_slot_idx(hmac_ctx->keytype, hmac_ctx->keyslot));

    if (hmac_ctx->start)
        crypto_write_dgst(hmac_ctx->state, DGST_INT_STATE_LEN);

    if ((check = hmac_get_cfg(hmac_ctx, &val32)) != SUCCESS)
        return check;

    hmac_regs->cfg = val32;
    hmac_regs->plen = hmac_ctx->curlen;

    dma_write_rwcfg(NULL, msg, msglen);
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

    val32 = hmac_regs->status;
    if (val32 != 0) {
        LOG_ERROR("[ERROR] HMAC status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    if (!last) {
        crypto_read_dgest(hmac_ctx->state, DGST_INT_STATE_LEN);
        hmac_ctx->curlen = hmac_regs->alen;
    }

    return SUCCESS;
}

static pufs_status_t __hmac_ctx_update(pufs_hmac_ctx* hmac_ctx,
    pufs_dgst_st* md,
    const uint8_t* msg,
    uint32_t msglen,
    bool last)
{
    pufs_status_t ret;
    uint32_t len;

    do {
        len = msglen > CHUNK_MAXLEN ? CHUNK_MAXLEN : msglen;
        ret = ___hmac_ctx_update(hmac_ctx, md, msg, len, last);
        if (ret != SUCCESS)
            return ret;
        msg += len;
        msglen -= len;
        if (hmac_ctx->start == false)
            hmac_ctx->start = true;
    } while (msglen);
    return SUCCESS;
}

/**
 * @brief Handle input and update the buffer for HMAC
 *
 * @see hmac_ctx_init().
 * @see __hmac_ctx_update().
 * @return SUCCESS on success, otherwise an error code.
 */
static pufs_status_t hmac_ctx_update(hmac_op op,
    pufs_hmac_ctx* hmac_ctx,
    const uint8_t* msg,
    uint32_t msglen)
{
    pufs_status_t check;
    // check hmac_ctx is owned by this operation (hash or HMAC)
    if (hmac_ctx->op != op)
        return E_UNAVAIL;
    // continue if msg is NULL or msglen is zero
    if ((msg == NULL) || (msglen == 0))
        return SUCCESS;

    blsegs segs = segment(hmac_ctx->buff, hmac_ctx->buflen, msg, msglen,
        hmac_ctx->blocklen, hmac_ctx->minlen);
    hmac_ctx->buflen = 0;

    for (uint32_t i = 0; i < segs.nsegs; i++) {
        if (segs.seg[i].process) // process
        {
            if ((check = __hmac_ctx_update(hmac_ctx, NULL, segs.seg[i].addr,
                     segs.seg[i].len, false))
                != SUCCESS) {
                // release hmac context
                hmac_ctx->op = HMAC_AVAILABLE;
                return check;
            }
        } else // keep in the internal buffer
        {
            if ((segs.seg[i].addr == hmac_ctx->buff) && (hmac_ctx->buflen == 0)) { // skip copy what already in the right place
                hmac_ctx->buflen += segs.seg[i].len;
            } else // copy into the buffer
            {
                rvv_memcpy(hmac_ctx->buff + hmac_ctx->buflen, segs.seg[i].addr, segs.seg[i].len);
                hmac_ctx->buflen += segs.seg[i].len;
            }
        }
    }

    return SUCCESS;
}

static pufs_status_t hmac_ctx_sg_append(hmac_op op,
    pufs_hmac_ctx* hmac_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    pufs_status_t check;
    uint32_t value32, key_size = 0, plen = 0;
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_dsc_attr_st* attr;
    pufs_dma_sg_desc_opts_st opts = { .offset = 0x0, .done_interrupt = false, .done_pause = false };

    if (hmac_ctx->op != op)
        return E_UNAVAIL;

    if (hmac_check_sgdma_descriptors(hmac_ctx->blocklen, descs, descs_len) != true)
        return E_INVALID;

    // initialize crypto_io_ctx
    if (hmac_ctx->crypto_io_ctx == NULL) {
        // TODO: for linux user-space env
        hmac_ctx->crypto_io_ctx = crypto_new_crypto_io_ctx();

        if (op == HMAC_HMAC && hmac_ctx->keytype == SWKEY)
            crypto_io_write_sw_key(hmac_ctx->crypto_io_ctx, hmac_ctx->key, SW_KEY_MAXLEN);
    }

    if (hmac_ctx->op == HMAC_HMAC) {
        switch (hmac_ctx->hash) {
        case SHA_224:
        case SHA_256:
        case SM3:
            key_size = 64;
            break;
        case SHA_384:
        case SHA_512:
        case SHA_512_224:
        case SHA_512_256:
            key_size = 128;
            break;
        default:
            return E_INVALID;
        }
    }

    desc = dma_sg_new_read_ctx_descriptor((uintptr_t)hmac_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    if ((check = hmac_get_cfg(hmac_ctx, &value32)) != SUCCESS)
        return check;

    for (uint32_t index = 0; index < descs_len; index++) {
        desc = dma_sg_new_data_descriptor();
        if (desc == NULL)
            return E_FIRMWARE;

        attr = &descs[index].attr;
        opts.head = !hmac_ctx->start;

        if (index == descs_len - 1 && last)
            opts.tail = last;
        else
            opts.tail = false;

        dma_sg_desc_write_addr(desc, descs[index].write_addr, descs[index].read_addr, descs[index].length);
        dma_sg_desc_write_dsc_config(desc, attr, &opts);
        dma_sg_desc_write_key_config(desc, hmac_ctx->keytype,
            ALGO_TYPE_HMAC, (hmac_ctx->keybits < 512) ? hmac_ctx->keybits : 512,
            get_key_slot_idx(hmac_ctx->keytype, hmac_ctx->keyslot));
        plen = hmac_ctx->curlen > 0 ? (hmac_ctx->curlen + key_size) : 0;
        dma_sg_desc_write_crypto_config(desc, value32, plen);

        dma_sg_desc_append_to_list(desc);

        hmac_ctx->curlen += descs[index].length;
        if (!hmac_ctx->start)
            hmac_ctx->start = true;
    }

    desc = dma_sg_new_write_ctx_descriptor((uintptr_t)hmac_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    return SUCCESS;
}

static pufs_status_t hmac_ctx_sg_done(pufs_hash_ctx* hmac_ctx,
    pufs_dgst_st* md)
{
    pufs_status_t check;
    if ((check = hmac_set_dgst_length(hmac_ctx, md)) != SUCCESS)
        return check;

    crypto_io_read_dgest(hmac_ctx->crypto_io_ctx, md->dgst, md->dlen);

    crypto_free_crypto_io_ctx(hmac_ctx->crypto_io_ctx);
    hmac_ctx->crypto_io_ctx = NULL;
    hmac_ctx->op = HMAC_AVAILABLE;
    return SUCCESS;
}

/**
 * @brief Handle the data left in the buffer for HMAC
 *
 * @see hmac_ctx_init().
 * @see __hmac_ctx_update().
 * @return SUCCESS on success, otherwise an error code.
 */
pufs_status_t hmac_ctx_final(hmac_op op,
    pufs_hmac_ctx* hmac_ctx,
    pufs_dgst_st* md)
{
    pufs_status_t check = SUCCESS;

    // check hmac_ctx is owned by this operation (hash, HMAC, or KDF)
    if (hmac_ctx->op != op)
        return E_UNAVAIL;

    // in final call, it must be some data to pass into the hash module
    if (hmac_ctx->start && (hmac_ctx->buflen == 0))
        check = E_INVALID;
    else
        check = __hmac_ctx_update(hmac_ctx, md, hmac_ctx->buff,
            hmac_ctx->buflen, true);
    if (check != SUCCESS)
        goto done;

    check = __hmac_ctx_final(hmac_ctx, md);

done:
    hmac_ctx->op = HMAC_AVAILABLE;
    return check;
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_hmac_module_init(uint32_t hmac_offset)
{
    hmac_regs = (struct pufs_hmac_regs*)(pufs_context.base_addr + hmac_offset);
    version_check(HMAC_VERSION, hmac_regs->version);
}

/**
 * pufs_hmac_ctx_new()
 */
pufs_hmac_ctx* pufs_hmac_ctx_new(void)
{
    pufs_hmac_ctx* ret;

    ret = malloc(sizeof(pufs_hmac_ctx));
    if (ret != NULL) {
        ret->op = HMAC_AVAILABLE;
        rvv_memset(ret, 0x0, sizeof(pufs_hmac_ctx));
    }

    return ret;
}

/**
 * pufs_hmac_ctx_free()
 */
void pufs_hmac_ctx_free(pufs_hmac_ctx* hmac_ctx)
{
    if (hmac_ctx != NULL) {
        rvv_memset(hmac_ctx, 0, sizeof(pufs_hmac_ctx));
        hmac_ctx->op = HMAC_AVAILABLE;
    }
    free(hmac_ctx);
}

/**
 * pufs_hash_init()
 */
pufs_status_t pufs_hash_init(pufs_hash_ctx* hash_ctx, pufs_hash_t hash)
{
    return hmac_ctx_init(HMAC_HASH, hash_ctx, hash, 0, 0, 0);
}

/**
 * pufs_hash_update()
 */
pufs_status_t pufs_hash_update(pufs_hash_ctx* hash_ctx,
    const uint8_t* msg,
    uint32_t msglen)
{
    return hmac_ctx_update(HMAC_HASH, hash_ctx, msg, msglen);
}

pufs_status_t pufs_hash_sg_append(pufs_hash_ctx* hash_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return hmac_ctx_sg_append(HMAC_HASH, hash_ctx, descs, descs_len, last);
}

pufs_status_t pufs_hash_sg_done(pufs_hash_ctx* hash_ctx,
    pufs_dgst_st* md)
{
    return hmac_ctx_sg_done(hash_ctx, md);
}

/**
 * pufs_hash_final()
 */
pufs_status_t pufs_hash_final(pufs_hash_ctx* hash_ctx, pufs_dgst_st* md)
{
    return hmac_ctx_final(HMAC_HASH, hash_ctx, md);
}

/**
 * pufs_hash()
 */
pufs_status_t pufs_hash(pufs_dgst_st* md,
    const uint8_t* msg,
    uint32_t msglen,
    pufs_hash_t hash)
{
    pufs_status_t check;
    pufs_hash_ctx hash_ctx = { .op = HMAC_AVAILABLE };

    // Call I-U-F model
    if ((check = pufs_hash_init(&hash_ctx, hash)) != SUCCESS)
        return check;
    if ((check = pufs_hash_update(&hash_ctx, msg, msglen)) != SUCCESS)
        return check;
    return pufs_hash_final(&hash_ctx, md);
}

/**
 * _pufs_hmac_init()
 */
pufs_status_t _pufs_hmac_init(pufs_hmac_ctx* hmac_ctx,
    pufs_hash_t hash,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    return hmac_ctx_init(HMAC_HMAC, hmac_ctx, hash, keytype, keyaddr, keybits);
}

/**
 * pufs_hmac_update()
 */
pufs_status_t pufs_hmac_update(pufs_hmac_ctx* hmac_ctx,
    const uint8_t* msg,
    uint32_t msglen)
{
    return hmac_ctx_update(HMAC_HMAC, hmac_ctx, msg, msglen);
}

pufs_status_t pufs_hmac_sg_append(pufs_hmac_ctx* hmac_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return hmac_ctx_sg_append(HMAC_HMAC, hmac_ctx, descs, descs_len, last);
}

pufs_status_t pufs_hmac_sg_done(pufs_hmac_ctx* hmac_ctx,
    pufs_dgst_st* md)
{
    return hmac_ctx_sg_done(hmac_ctx, md);
}

/**
 * pufs_hmac_final()
 */
pufs_status_t pufs_hmac_final(pufs_hmac_ctx* hmac_ctx, pufs_dgst_st* md)
{
    return hmac_ctx_final(HMAC_HMAC, hmac_ctx, md);
}

/**
 * _pufs_hmac()
 */
pufs_status_t _pufs_hmac(pufs_dgst_st* md,
    const uint8_t* msg,
    uint32_t msglen,
    pufs_hash_t hash,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    pufs_status_t check;
    pufs_hmac_ctx hmac_ctx = { .op = HMAC_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_hmac_init(&hmac_ctx, hash,
             keytype, keyaddr, keybits))
        != SUCCESS)
        return check;
    if ((check = pufs_hmac_update(&hmac_ctx, msg, msglen)) != SUCCESS)
        return check;
    return pufs_hmac_final(&hmac_ctx, md);
}
