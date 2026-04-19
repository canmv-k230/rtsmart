/**
 * @file      pufs_cmac.c
 * @brief     PUFsecurity CMAC API implementation
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
#include "pufs_cmac_internal.h"
#include "pufs_ka_internal.h"
#include "pufs_dma_internal.h"

struct pufs_cmac_regs* cmac_regs = NULL;

/*****************************************************************************
 * Static variables
 ****************************************************************************/

/*****************************************************************************
 * Static functions
 ****************************************************************************/
static bool cmac_check_sgdma_descriptors(pufs_dma_sg_desc_st* descs, uint32_t descs_len)
{
    if (descs == NULL || descs_len == 0)
        return false;

    for (uint32_t i = 0; i < descs_len; i++) {
        if (i != descs_len - 1 && descs[i].length % CMAC_BLOCK_SIZE != 0)
            return false;
    }
    return true;
}

static pufs_status_t cmac_get_cfg(pufs_cmac_ctx* cmac_ctx, uint32_t* cfg)
{
    switch (cmac_ctx->cipher) {
    case AES:
        switch (cmac_ctx->keybits) {
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
        switch (cmac_ctx->keybits) {
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
    return SUCCESS;
}

/**
 * @brief Initialize the internal context for CMAC
 *
 * @param[in] op        The operation.
 * @param[in] cmac_ctx  CMAC context to be initialized.
 * @param[in] cipher    The block cipher algorithm.
 * @param[in] keytype   The type of source which the key is from.
 * @param[in] keyaddr   The pointer to the space in SWKEY or the slot of the
 *                       source which the key is stored in.
 * @param[in] keybits   The key length in bits.
 * @return              SUCCESS on success, otherwise an error code.
 */
static pufs_status_t cmac_ctx_init(cmac_op op,
    pufs_cmac_ctx* cmac_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    pufs_status_t check;
    // abort if cmac_ctx is occupied
    if (cmac_ctx->op != CMAC_AVAILABLE)
        return E_BUSY;
    // check if op is CMAC_CMAC
    if (op != CMAC_CMAC)
        return E_INVALID;
    // check keytype
    if ((keytype == PUFKEY) || (keytype == SHARESEC))
        return E_DENY;
    // check feature with key length

    if ((check = crypto_check_cmac_algo(cipher, keybits)) != SUCCESS)
        return check;

    // check key settings for CMAC
    if ((keytype != SWKEY) && ((check = keyslot_check(true, keytype, (uint32_t)keyaddr, keybits)) != SUCCESS))
        return check;

    // initialize for CMAC
    cmac_ctx->buflen = 0;
    cmac_ctx->keybits = keybits;
    cmac_ctx->minlen = 1;
    cmac_ctx->keytype = keytype;
    cmac_ctx->op = op;
    cmac_ctx->cipher = cipher;
    cmac_ctx->start = false;
    cmac_ctx->crypto_io_ctx = NULL;

    if (keytype != SWKEY)
        cmac_ctx->keyslot = (uint32_t)keyaddr;
    else
        rvv_memcpy(cmac_ctx->key, (const void*)keyaddr, b2B(keybits));

    return SUCCESS;
}
/**
 * @brief Pass the input into the CMAC hardware
 *
 * @param[in] cmac_ctx  CMAC context to be initialized.
 * @param[in] md        The pointer to the space which the digest is written to.
 * @param[in] msg       The message.
 * @param[in] msglen    The length of the message in bytes.
 * @param[in] last      True if the input for this operation ends
 * @return              SUCCESS on success, otherwise an error code.
 */
static pufs_status_t ___cmac_ctx_update(pufs_cmac_ctx* cmac_ctx,
    pufs_dgst_st* md,
    const uint8_t* msg,
    uint32_t msglen,
    bool last)
{
    uint32_t val32;
    pufs_status_t check;

    if (last && (md == NULL))
        return E_INVALID;

    // Register manipulation
    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_data_block_config(cmac_ctx->start ? false : true, last, true, true, 0);
    dma_write_config_0(false, false, false);

    if (cmac_ctx->keytype == SWKEY)
        crypto_write_sw_key(cmac_ctx->key, SW_KEY_MAXLEN);

    dma_write_key_config_0(cmac_ctx->keytype,
        ALGO_TYPE_CMAC, cmac_ctx->keybits,
        get_key_slot_idx(cmac_ctx->keytype, cmac_ctx->keyslot));

    if (cmac_ctx->start)
        crypto_write_dgst(cmac_ctx->state, DGST_INT_STATE_LEN);

    if ((check = cmac_get_cfg(cmac_ctx, &val32)) != SUCCESS)
        return check;

    cmac_regs->cfg = val32;

    dma_write_rwcfg(NULL, msg, msglen);
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

    val32 = cmac_regs->status;

    if (val32 != 0) {
        LOG_ERROR("CMAC status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    if (last)
        crypto_read_dgest(md->dgst, DGST_INT_STATE_LEN);
    else
        crypto_read_dgest(cmac_ctx->state, DGST_INT_STATE_LEN);

    if (last) {
        switch (cmac_ctx->cipher) {
        case AES:
            md->dlen = 16;
            break;
        default:
            return E_FIRMWARE;
        }
    }

    return SUCCESS;
}
static pufs_status_t __cmac_ctx_update(pufs_cmac_ctx* cmac_ctx,
    pufs_dgst_st* md,
    const uint8_t* msg,
    uint32_t msglen,
    bool last)
{
    pufs_status_t ret;
    uint32_t len;

    do {
        len = msglen > CHUNK_MAXLEN ? CHUNK_MAXLEN : msglen;
        ret = ___cmac_ctx_update(cmac_ctx, md, msg, len, last);
        if (ret != SUCCESS)
            return ret;
        msg += len;
        msglen -= len;
        if (cmac_ctx->start == false)
            cmac_ctx->start = true;
    } while (msglen);
    return SUCCESS;
}
/**
 * @brief Handle input and update the buffer for CMAC
 *
 * @see cmac_ctx_init().
 * @see __cmac_ctx_update().
 * @return SUCCESS on success, otherwise an error code.
 */
static pufs_status_t cmac_ctx_update(cmac_op op,
    pufs_cmac_ctx* cmac_ctx,
    const uint8_t* msg,
    uint32_t msglen)
{
    // check cmac_ctx is owned by CMAC
    if (cmac_ctx->op != op)
        return E_UNAVAIL;
    // continue if msg is NULL or msglen is zero
    if ((msg == NULL) || (msglen == 0))
        return SUCCESS;

    pufs_status_t check = SUCCESS;
    blsegs segs = segment(cmac_ctx->buff, cmac_ctx->buflen, msg, msglen,
        CMAC_BLOCK_SIZE, cmac_ctx->minlen);
    cmac_ctx->buflen = 0;

    for (uint32_t i = 0; i < segs.nsegs; i++) {
        if (segs.seg[i].process) // process
        {
            if ((check = __cmac_ctx_update(cmac_ctx, NULL, segs.seg[i].addr,
                     segs.seg[i].len, false))
                != SUCCESS) {
                // release cmac context
                cmac_ctx->op = CMAC_AVAILABLE;
                return check;
            }
        } else // keep in the internal buffer
        {
            if ((segs.seg[i].addr == cmac_ctx->buff) && (cmac_ctx->buflen == 0)) { // skip copy what already in the right place
                cmac_ctx->buflen += segs.seg[i].len;
            } else // copy into the buffer
            {
                rvv_memcpy(cmac_ctx->buff + cmac_ctx->buflen, segs.seg[i].addr, segs.seg[i].len);
                cmac_ctx->buflen += segs.seg[i].len;
            }
        }
    }

    return SUCCESS;
}
static pufs_status_t cmac_ctx_sg_append(cmac_op op,
    pufs_cmac_ctx* cmac_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    uint32_t cmac_cfg;
    pufs_status_t check;
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_dsc_attr_st* attr;
    pufs_dma_sg_desc_opts_st opts = { .offset = 0x0, .done_interrupt = false, .done_pause = false };

    if (cmac_ctx->op != op)
        return E_UNAVAIL;

    if (cmac_check_sgdma_descriptors(descs, descs_len) != true)
        return E_INVALID;

    // initialize crypto_io_ctx
    if (cmac_ctx->crypto_io_ctx == NULL) {
        cmac_ctx->crypto_io_ctx = crypto_new_crypto_io_ctx();

        if (cmac_ctx->keytype == SWKEY)
            crypto_io_write_sw_key(cmac_ctx->crypto_io_ctx, cmac_ctx->key, SW_KEY_MAXLEN);
    }

    desc = dma_sg_new_read_ctx_descriptor((uintptr_t)cmac_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    if ((check = cmac_get_cfg(cmac_ctx, &cmac_cfg)) != SUCCESS)
        return check;

    for (uint32_t index = 0; index < descs_len; index++) {
        desc = dma_sg_new_data_descriptor();
        if (desc == NULL)
            return E_FIRMWARE;

        attr = &descs[index].attr;
        opts.head = !cmac_ctx->start;

        if (index == descs_len - 1 && last)
            opts.tail = last;
        else
            opts.tail = false;

        dma_sg_desc_write_addr(desc, descs[index].write_addr, descs[index].read_addr, descs[index].length);
        dma_sg_desc_write_dsc_config(desc, attr, &opts);
        dma_sg_desc_write_key_config(desc, cmac_ctx->keytype,
            ALGO_TYPE_CMAC, cmac_ctx->keybits,
            get_key_slot_idx(cmac_ctx->keytype, cmac_ctx->keyslot));
        dma_sg_desc_write_crypto_config(desc, cmac_cfg, 0x0);

        dma_sg_desc_append_to_list(desc);

        if (!cmac_ctx->start)
            cmac_ctx->start = true;
    }

    desc = dma_sg_new_write_ctx_descriptor((uintptr_t)cmac_ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    return SUCCESS;
}

static pufs_status_t cmac_ctx_sg_done(pufs_cmac_ctx* cmac_ctx,
    pufs_dgst_st* md)
{
    switch (cmac_ctx->cipher) {
    case AES:
        md->dlen = 16;
        break;
    default:
        return E_FIRMWARE;
    }

    crypto_io_read_dgest(cmac_ctx->crypto_io_ctx, md->dgst, md->dlen);

    crypto_free_crypto_io_ctx(cmac_ctx->crypto_io_ctx);
    cmac_ctx->crypto_io_ctx = NULL;
    cmac_ctx->op = CMAC_AVAILABLE;
    return SUCCESS;
}

/**
 * @brief Handle the data left in the buffer for CMAC
 *
 * @see cmac_ctx_init().
 * @see __cmac_ctx_update().
 * @return SUCCESS on success, otherwise an error code.
 */
static pufs_status_t cmac_ctx_final(cmac_op op,
    pufs_cmac_ctx* cmac_ctx,
    pufs_dgst_st* md)
{
    pufs_status_t check = SUCCESS;

    // check cmac_ctx is owned by CMAC
    if (cmac_ctx->op != op)
        return E_UNAVAIL;

    // in final call, it must be some data to pass into the CMAC module
    if (cmac_ctx->start && (cmac_ctx->buflen == 0))
        check = E_FIRMWARE;
    else
        check = __cmac_ctx_update(cmac_ctx, md, cmac_ctx->buff,
            cmac_ctx->buflen, true);

    cmac_ctx->op = CMAC_AVAILABLE;
    return check;
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_cmac_module_init(uint32_t cmac_offset)
{
    cmac_regs = (struct pufs_cmac_regs*)(pufs_context.base_addr + cmac_offset);
    version_check(CMAC_VERSION, cmac_regs->version);
    LOG_INFO("%s", "CMAC module is initialized");
}
/**
 * pufs_cmac_ctx_new()
 */
pufs_cmac_ctx* pufs_cmac_ctx_new(void)
{
    pufs_cmac_ctx* ret;

    ret = malloc(sizeof(pufs_cmac_ctx));
    if (ret != NULL) {
        ret->op = CMAC_AVAILABLE;
        rvv_memset(ret, 0x0, sizeof(pufs_cmac_ctx));
    }

    return ret;
}
/**
 * pufs_cmac_ctx_free()
 */
void pufs_cmac_ctx_free(pufs_cmac_ctx* cmac_ctx)
{
    if (cmac_ctx != NULL) {
        rvv_memset(cmac_ctx, 0, sizeof(pufs_cmac_ctx));
        cmac_ctx->op = CMAC_AVAILABLE;
    }
    free(cmac_ctx);
}
/**
 * _pufs_cmac_init()
 */
pufs_status_t _pufs_cmac_init(pufs_cmac_ctx* cmac_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    return cmac_ctx_init(CMAC_CMAC, cmac_ctx, cipher, keytype, keyaddr, keybits);
}
/**
 * pufs_cmac_upadte()
 */
pufs_status_t pufs_cmac_update(pufs_cmac_ctx* cmac_ctx,
    const uint8_t* msg,
    uint32_t msglen)
{
    return cmac_ctx_update(CMAC_CMAC, cmac_ctx, msg, msglen);
}
/**
 * pufs_cmac_final()
 */
pufs_status_t pufs_cmac_final(pufs_cmac_ctx* cmac_ctx, pufs_dgst_st* md)
{
    return cmac_ctx_final(CMAC_CMAC, cmac_ctx, md);
}

pufs_status_t pufs_cmac_sg_append(pufs_cmac_ctx* cmac_ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return cmac_ctx_sg_append(CMAC_CMAC, cmac_ctx, descs, descs_len, last);
}

pufs_status_t pufs_cmac_sg_done(pufs_cmac_ctx* cmac_ctx, pufs_dgst_st* md)
{
    return cmac_ctx_sg_done(cmac_ctx, md);
}

/**
 * _pufs_cmac()
 */
pufs_status_t _pufs_cmac(pufs_dgst_st* md,
    const uint8_t* msg,
    uint32_t msglen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    pufs_status_t check;
    pufs_cmac_ctx cmac_ctx = { .op = CMAC_AVAILABLE };

    // Call I-U-F model
    if ((check = _pufs_cmac_init(&cmac_ctx, cipher, keytype, keyaddr, keybits)) != SUCCESS)
        return check;
    if ((check = pufs_cmac_update(&cmac_ctx, msg, msglen)) != SUCCESS)
        return check;
    return pufs_cmac_final(&cmac_ctx, md);
}
