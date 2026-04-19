/**
 * @file      pufs_sp38c.c
 * @brief     PUFsecurity SP38C API implementation
 * @copyright 2021 PUFsecurity
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
#include "pufs_ka_internal.h"
#include "pufs_dma_internal.h"
#include "pufs_sp38c_internal.h"

struct pufs_sp38c_regs* sp38c_regs = NULL;

/*****************************************************************************
 * Static variables
 ****************************************************************************/

/*****************************************************************************
 * Static functions
 ****************************************************************************/

static pufs_status_t sp38c_get_config(uint32_t* cfg, pufs_sp38c_ctx* ctx, bool cbcmac, bool reg_in, bool reg_out)
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
        val32 = 0X3;
        break;
    default:
        return E_FIRMWARE;
    }

    if (!cbcmac)
        val32 |= 0x1 << SP38C_CFG_CCM_CTR_BITS;
    else
        val32 |= 0x1 << SP38C_CFG_CCM_CBC_MAC_BITS;

    val32 |= (ctx->encrypt ? 0x1 : 0x0) << SP38C_CFG_ENCRYPT_BITS;

    if (reg_in)
        val32 |= 0x1 << SP38C_CFG_REG_IN_BITS;

    if (reg_out)
        val32 |= 0x1 << SP38C_CFG_REG_OUT_BITS;

    *cfg = val32;
    return SUCCESS;
}

static void sp38c_dec_change_sgdma_descriptors(pufs_dma_sg_desc_st* descs,
    uint32_t descs_len)
{
    for (uint32_t i = 0; i < descs_len; i++) {
        descs[i].write_addr = descs[i].read_addr;
        descs[i].read_addr = 0x0;
    }
}

static void clear_ctx_buffer(pufs_sp38c_ctx* ctx)
{
    rvv_memset(ctx->buff, 0x0, BC_BLOCK_SIZE);
    ctx->buflen = 0x0;
}

/**
 * @brief setup key configuration.
 */
static void sp38c_setup_key(pufs_sp38c_ctx* ctx)
{
    if (ctx->keytype == SWKEY)
        crypto_write_sw_key(ctx->key, SW_KEY_MAXLEN);

    dma_write_key_config_0(ctx->keytype,
        ALGO_TYPE_CCM,
        ctx->keybits,
        get_key_slot_idx(ctx->keytype, ctx->keyslot));
}

static pufs_status_t _sp38c_ctx_update(pufs_sp38c_ctx* ctx,
    uint8_t* dgst,
    uint8_t* iv,
    const uint8_t* in,
    uint32_t inlen,
    bool last,
    uint8_t* out)
{
    pufs_status_t check;
    uint32_t val32;
    bool start;
    bool cbcmac = dgst != NULL ? true : false;

    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_config_0(false, false, false);

    if (cbcmac) {
        start = ctx->cbcmac_start ? true : false;
        crypto_write_dgst(dgst, BC_BLOCK_SIZE); // the expected length of dgst is a block
    } else {
        start = ctx->ctr_start ? true : false;
        crypto_write_iv(iv, BC_BLOCK_SIZE); // the expected length of iv is a block
        if (ctx->currentlen == ULLONG_MAX)
            sp38c_regs->block_num = 0;
        else
            sp38c_regs->block_num = 1 + (ctx->currentlen / BC_BLOCK_SIZE);
    }

    dma_write_data_block_config(start ? false : true, last, true, true, 0);

    if ((check = sp38c_get_config(&val32, ctx, cbcmac, false, false)) != SUCCESS)
        return check;

    sp38c_regs->cfg = val32;

    dma_write_rwcfg(cbcmac ? NULL : out, in, inlen);
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

    val32 = sp38c_regs->status;
    if ((val32 & SP38C_STATUS_RESP_MASK) != 0) {
        LOG_ERROR("SP38C status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    if (cbcmac && !ctx->cbcmac_start)
        ctx->cbcmac_start = true;

    if (!cbcmac && !ctx->ctr_start)
        ctx->ctr_start = true;

    if (cbcmac) {
        crypto_read_dgest(out, BC_BLOCK_SIZE);
        return SUCCESS;
    }

    dma_read_output(out, inlen);

    return SUCCESS;
}

static pufs_status_t sp38c_ctx_update(pufs_sp38c_ctx* ctx,
    uint8_t* dgst,
    uint8_t* iv,
    const uint8_t* in,
    uint32_t inlen,
    bool last,
    uint8_t* out)
{
    pufs_status_t ret;
    uint32_t len;

    do {
        len = inlen > CHUNK_MAXLEN ? CHUNK_MAXLEN : inlen;
        ret = _sp38c_ctx_update(ctx, dgst, iv, in, len, last, out);
        if (ret != SUCCESS)
            return ret;
        in += len;
        out += len;
        inlen -= len;
    } while (inlen);
    return SUCCESS;
}

/**
 * @brief format the first block B0. See 800-38c: A.2.1 section.
 */
static pufs_status_t formatting_ctrl_info(pufs_sp38c_ctx* ctx,
    const uint8_t* nonce,
    uint32_t noncelen,
    uint8_t* out)
{
    size_t Q;

    // Length requirements from A.1 section
    if (noncelen < 7 || noncelen > 13)
        return E_INVALID;

    if (ctx->taglen < 4 || ctx->taglen > 16 || (ctx->taglen % 2) != 0)
        return E_INVALID;

    rvv_memset(out, 0x0, BC_BLOCK_SIZE);

    // format the flag byte of B0
    out[0] = (ctx->aadlen > 0) ? 0x40 : 0x00;
    out[0] |= ((ctx->taglen - 2) / 2) << 3;
    out[0] |= ctx->qlen - 1;

    rvv_memcpy(out + 1, nonce, noncelen);

    Q = ctx->inlen;
    for (size_t index = 0; index < ctx->qlen; index++, Q >>= 8)
        out[15 - index] = Q & 0xFF;

    if (Q)
        return E_INVALID;

    return SUCCESS;
}

uint32_t pufs_ccm_formatting_aad_header(uint8_t* buf, uint64_t aadlen)
{
    // 0 < a < 2^16 - 2^8
    if (aadlen < 0xff00) {
        buf[0] = (uint8_t)(aadlen >> 8);
        buf[1] = (uint8_t)aadlen;
        return 2;
    }

    if (aadlen >= (uint64_t)(1 << 31)) {
        buf[0] = 0xff;
        buf[1] = 0xff;

        uint8_t* start = (uint8_t*)((uint64_t*)&aadlen);
        for (size_t i = 0; i < 8; i++)
            buf[9 - i] = *(start + i);

        return 10;
    }

    buf[0] = 0xff;
    buf[1] = 0xfe;
    buf[2] = (uint8_t)(aadlen >> 24);
    buf[3] = (uint8_t)(aadlen >> 16);
    buf[4] = (uint8_t)(aadlen >> 8);
    buf[5] = (uint8_t)aadlen;
    return 6;
}

/**
 * @brief format the first block (B1) for Associated Data. See 800-38c: A.2.2 section.
 */
static uint32_t formatting_aad(pufs_sp38c_ctx* ctx,
    const uint8_t* in,
    uint32_t inlen)
{
    uint32_t pos = 0, len = 0;
    rvv_memset(ctx->buff, 0x0, BC_BLOCK_SIZE);

    pos = pufs_ccm_formatting_aad_header(ctx->buff, ctx->aadlen);

    len = (16 - pos) >= inlen ? inlen : 16 - pos;
    rvv_memcpy(ctx->buff + pos, in, len);

    ctx->aadlen -= len;
    ctx->buflen = pos + len;

    return len;
}

/**
 * @brief Y0 = CIPHK(B0). See 6.1 Generation-Encryption Process
 */
static pufs_status_t initialize_cbcmac(pufs_sp38c_ctx* ctx,
    const uint8_t* nonce,
    uint32_t noncelen)
{
    // B0
    if (formatting_ctrl_info(ctx, nonce, noncelen, ctx->buff) != SUCCESS)
        return E_FIRMWARE;

    rvv_memset(ctx->cbcmac, 0x0, BC_BLOCK_SIZE);
    if (sp38c_ctx_update(ctx, ctx->cbcmac, NULL, ctx->buff, BC_BLOCK_SIZE, false, ctx->cbcmac) != SUCCESS)
        return E_FIRMWARE;

    clear_ctx_buffer(ctx);
    return SUCCESS;
}

/**
 * @brief initialize the value of counter block. See A.3 Formatting of the Counter Blocks.
 */
static void initialize_counter(pufs_sp38c_ctx* ctx,
    const uint8_t* nonce,
    uint32_t noncelen)
{
    rvv_memset(ctx->ctri, 0x0, BC_BLOCK_SIZE);
    ctx->ctri[0] |= ctx->qlen - 1;
    rvv_memcpy(ctx->ctri + 1, nonce, noncelen);
}

static pufs_status_t check_sp38c_crypto(pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits)
{
    pufs_status_t check;

    if ((keytype == PUFKEY) || (keytype == SHARESEC))
        return E_DENY;

    if ((check = crypto_check_ccm_algo(cipher, keybits)) != SUCCESS)
        return check;

    if ((keytype != SWKEY) && ((check = keyslot_check(true, keytype, (uint32_t)keyaddr, keybits)) != SUCCESS))
        return check;

    return SUCCESS;
}

static pufs_status_t sp38c_ctx_init(pufs_sp38c_ctx* ctx,
    pufs_cipher_t cipher,
    bool encrypt,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* nonce,
    uint32_t noncelen,
    uint64_t aadlen,
    uint64_t inlen,
    uint32_t taglen)
{
    pufs_status_t ret;

    if (ctx->op != SP38C_AVAILABLE)
        return E_BUSY;

    if ((ret = check_sp38c_crypto(cipher, keytype, keyaddr, keybits)) != SUCCESS)
        return ret;

    ctx->cipher = cipher;
    ctx->encrypt = encrypt;
    ctx->qlen = (BC_BLOCK_SIZE - 1) - noncelen;
    ctx->aadlen = aadlen;
    ctx->inlen = inlen;
    ctx->taglen = taglen;
    ctx->keybits = keybits;
    ctx->keytype = keytype;
    ctx->stage = SP38C_NONE;
    ctx->buflen = 0;
    ctx->currentlen = 0;
    ctx->cbcmac_start = false;
    ctx->ctr_start = false;
    ctx->phybuf_list = NULL;

    rvv_memset(ctx->key, 0x0, SW_KEY_MAXLEN);
    if (keytype == SWKEY)
        rvv_memcpy(ctx->key, (const void*)keyaddr, b2B(keybits));
    else
        ctx->keyslot = (uint32_t)keyaddr;

    sp38c_setup_key(ctx);
    if ((ret = initialize_cbcmac(ctx, nonce, noncelen)) != SUCCESS)
        return ret;

    initialize_counter(ctx, nonce, noncelen);

    ctx->op = SP38C_CCM;
    return SUCCESS;
}

static size_t fill_incomplete_block(pufs_sp38c_ctx* ctx, const uint8_t* start_offset, uint32_t inlen)
{
    size_t len = 0;
    if (ctx->buflen < BC_BLOCK_SIZE) {
        len = (BC_BLOCK_SIZE - ctx->buflen) < inlen ? BC_BLOCK_SIZE - ctx->buflen : inlen;
        rvv_memcpy(ctx->buff + ctx->buflen, start_offset, len);
        ctx->buflen += len;
    }
    return len;
}

static size_t cut_blocks(pufs_sp38c_ctx* ctx, const uint8_t* start_offset, uint32_t inlen)
{
    size_t blocks = inlen / BC_BLOCK_SIZE;
    size_t last_block = inlen % BC_BLOCK_SIZE;

    clear_ctx_buffer(ctx);
    if (last_block) {
        rvv_memcpy(ctx->buff, start_offset + (blocks * BC_BLOCK_SIZE), last_block);
        ctx->buflen = last_block;
    }

    return blocks;
}

static void sp38c_set_crypto_io_ctx(pufs_sp38c_ctx* ctx)
{
    if (ctx->crypto_io_ctx != NULL)
        return;

    ctx->crypto_io_ctx = crypto_new_crypto_io_ctx();

    if (ctx->keytype == SWKEY)
        crypto_io_write_sw_key(ctx->crypto_io_ctx, ctx->key, SW_KEY_MAXLEN);

    crypto_io_write_iv(ctx->crypto_io_ctx, ctx->ctri, BC_BLOCK_SIZE);
    crypto_io_write_dgst(ctx->crypto_io_ctx, ctx->cbcmac, DGST_INT_STATE_LEN);
}

static pufs_status_t sp38c_ctx_sg_append_data(pufs_sp38c_ctx* ctx,
    pufs_ccm_data_t data_type,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    uint32_t cfg,
    bool ctr,
    bool last)
{
    uint32_t block_num;
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_dsc_attr_st* attr;
    pufs_dma_sg_desc_opts_st opts = { .offset = 0x0, .done_interrupt = false, .done_pause = false };

    for (uint32_t index = 0; index < descs_len; index++) {
        desc = dma_sg_new_data_descriptor();
        if (desc == NULL)
            return E_FIRMWARE;

        attr = &descs[index].attr;

        if (data_type == CCM_AAD)
            opts.head = !ctx->cbcmac_start;
        else
            opts.head = !ctx->ctr_start;

        if (data_type != CCM_AAD && index == descs_len - 1)
            opts.tail = last;
        else
            opts.tail = false;

        dma_sg_desc_write_addr(desc, descs[index].write_addr, descs[index].read_addr, descs[index].length);
        dma_sg_desc_write_dsc_config(desc, attr, &opts);
        dma_sg_desc_write_key_config(desc, ctx->keytype,
            ALGO_TYPE_CCM, ctx->keybits,
            get_key_slot_idx(ctx->keytype, ctx->keyslot));

        block_num = !ctr ? 0 : 1 + (ctx->currentlen / BC_BLOCK_SIZE);

        dma_sg_desc_write_crypto_config(desc, cfg, block_num);
        dma_sg_desc_append_to_list(desc);

        if (data_type == CCM_AAD && !ctx->cbcmac_start)
            ctx->cbcmac_start = true;
        if (data_type == CCM_PLAINTEXT && !ctx->ctr_start)
            ctx->ctr_start = true;
        if (data_type == CCM_PLAINTEXT && ctr)
            ctx->currentlen += descs[index].length;
    }

    return SUCCESS;
}

static uintptr_t sp38c_request_phybuf(pufs_sp38c_ctx* ctx,
    uint32_t size,
    uintptr_t writeback_addr)
{
    sp38c_phybuf_record_st* ret;

    ret = (sp38c_phybuf_record_st*)calloc(1, sizeof(sp38c_phybuf_record_st));
    if (!ret)
        return 0;

#ifdef BAREMETAL
    ret->buf_addr = (uintptr_t)calloc(1, size);
#else
    // Fixme: borrow space from dma sg internal descriptor is only a workaround
    ret->buf_addr = PHY_ADDR((uintptr_t)dma_sg_new_data_descriptor());
#endif /* BAREMETAL */
    if (!ret->buf_addr) {
        free(ret);
        return 0;
    }

    ret->size = size;
    ret->writeback_addr = writeback_addr;

    ret->next = ctx->phybuf_list;
    ctx->phybuf_list = ret;

    return ret->buf_addr;
}

/**
 * @brief Reconstruct the descriptor list if neccesary.
 *        Data does NOT fit into a block will be reallocated and assigned a
 *        separate descriptor.
 */
static pufs_dma_sg_desc_st* sp38c_format_descs(pufs_sp38c_ctx* ctx,
    pufs_dma_sg_desc_st* descs,
    uint32_t* descs_len)
{
    uint32_t add_count = 0, unalign_len = 0, align_len = 0;
    pufs_dma_sg_desc_st* ret;

    // Count the number of descriptors containing non-block-aligned data
    for (uint32_t index = 0; index < *descs_len; index++) {
        if (descs[index].length % BC_BLOCK_SIZE != 0)
            add_count++;
    }

    // The list is perfect already
    if (add_count == 0)
        return descs;

    ret = calloc(*descs_len + add_count, sizeof(pufs_dma_sg_desc_st));
    if (ret == NULL)
        return NULL;

    uint32_t ridx = 0;
    for (uint32_t idx = 0; idx < *descs_len; idx += 1) {
        unalign_len = descs[idx].length % BC_BLOCK_SIZE;
        align_len = descs[idx].length - unalign_len;

        // For the block-aligned part, just copy their metadata
        if (descs[idx].length >= BC_BLOCK_SIZE) {
            ret[ridx].write_addr = descs[idx].write_addr;
            ret[ridx].read_addr = descs[idx].read_addr;
            ret[ridx].length = align_len; // trim unaligned part
            ret[ridx].attr = descs[idx].attr;
            ridx++;
        }

        // If no unaligned part need to be handled, ends here
        if (unalign_len == 0)
            continue;

        ret[ridx].write_addr = sp38c_request_phybuf(ctx, BC_BLOCK_SIZE, 0);
        if (!ret[ridx].write_addr)
            return NULL;
        rvv_memcpy((void*)VIRT_ADDR(ret[ridx].write_addr), (void*)DMA_RBUF_VIRT_ADDR(descs[idx].write_addr + align_len),
            unalign_len);

        if (descs[idx].read_addr != 0x0) {
            ret[ridx].read_addr = sp38c_request_phybuf(ctx, BC_BLOCK_SIZE, descs[idx].read_addr + align_len);
            if (!ret[ridx].read_addr)
                return NULL;
        } else {
            ret[ridx].read_addr = 0x0;
        }

        ret[ridx].length = unalign_len;
        ret[ridx].attr = descs[idx].attr;
        ridx++;
    }

    *descs_len = ridx;
    return ret;
}

static pufs_status_t sp38c_ctx_sg_append(pufs_sp38c_ctx* ctx,
    pufs_ccm_data_t data_type,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    uint32_t cfg;
    pufs_status_t check;
    pufs_dma_sg_desc_st* formatted_descs;
    pufs_dma_sg_internal_desc_st* desc;

    formatted_descs = sp38c_format_descs(ctx, descs, &descs_len);
    if (formatted_descs == NULL)
        return E_FIRMWARE;

    sp38c_set_crypto_io_ctx(ctx);

    desc = dma_sg_new_read_ctx_descriptor((uintptr_t)ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    if ((check = sp38c_get_config(&cfg, ctx, data_type == CCM_AAD, false, false)) != SUCCESS)
        return check;

    // sp38c_ctx_sg_append_data(ctx, data_type, descs, descs_len, cfg, data_type == CCM_PLAINTEXT ? true : false, last);
    sp38c_ctx_sg_append_data(ctx, data_type, formatted_descs, descs_len, cfg, data_type == CCM_PLAINTEXT ? true : false, last);

    if (data_type == CCM_PLAINTEXT) {
        // if (!ctx->encrypt) sp38c_dec_change_sgdma_descriptors(descs, descs_len);
        if (!ctx->encrypt)
            sp38c_dec_change_sgdma_descriptors(formatted_descs, descs_len);
        if ((check = sp38c_get_config(&cfg, ctx, true, false, false)) != SUCCESS)
            return check;

        // sp38c_ctx_sg_append_data(ctx, data_type, descs, descs_len, cfg, true, last);
        sp38c_ctx_sg_append_data(ctx, data_type, formatted_descs, descs_len, cfg, true, last);
    }

    desc = dma_sg_new_write_ctx_descriptor((uintptr_t)ctx->crypto_io_ctx);
    dma_sg_desc_append_to_list(desc);

    if (formatted_descs != descs)
        free(formatted_descs);

    return SUCCESS;
}

static pufs_status_t pufs_ccm_aad_update(pufs_sp38c_ctx* ctx,
    const uint8_t* in,
    uint32_t inlen)
{
    pufs_status_t status;
    size_t len = 0;
    uint32_t offset = 0, blocks = 0;

    switch (ctx->stage) {
    case SP38C_NONE:
        // Do nothing if there is no aad data.
        if (ctx->aadlen == 0)
            return SUCCESS;

        if (ctx->buflen == 0) {
            offset = formatting_aad(ctx, in, inlen);
        } else {
            offset = fill_incomplete_block(ctx, in, inlen);
            ctx->aadlen -= offset;
        }
        // Fill B1 block in next iteration if ther are other aad data and B1 is not full.
        if (ctx->buflen != BC_BLOCK_SIZE && ctx->aadlen > 0)
            return SUCCESS;

        if ((status = sp38c_ctx_update(ctx, ctx->cbcmac, NULL, ctx->buff, BC_BLOCK_SIZE, false, ctx->cbcmac)) != SUCCESS)
            return status;
        clear_ctx_buffer(ctx);
        break;
    case SP38C_AAD:
        if (ctx->buflen > 0) {
            offset = fill_incomplete_block(ctx, in, inlen);
            if (ctx->buflen == BC_BLOCK_SIZE && (status = sp38c_ctx_update(ctx, ctx->cbcmac, NULL, ctx->buff, BC_BLOCK_SIZE, false, ctx->cbcmac)) != SUCCESS)
                return status;
        }
        break;
    case SP38C_TEXT:
        return E_INVALID;
    default:
        return E_FIRMWARE;
    }

    len = inlen - offset;
    if (len <= 0) {
        ctx->stage = SP38C_AAD;
        return SUCCESS;
    }

    blocks = cut_blocks(ctx, in + offset, len);
    if (blocks) {
        if ((status = sp38c_ctx_update(ctx, ctx->cbcmac, NULL, in + offset, blocks * BC_BLOCK_SIZE, false, ctx->cbcmac)) != SUCCESS)
            return status;
    }
    ctx->stage = SP38C_AAD;
    return SUCCESS;
}

static pufs_status_t pufs_ccm_aad_final(pufs_sp38c_ctx* ctx)
{
    if (!ctx->buflen)
        return SUCCESS;

    pufs_status_t status = sp38c_ctx_update(ctx, ctx->cbcmac, NULL, ctx->buff, BC_BLOCK_SIZE, false, ctx->cbcmac);
    if (status != SUCCESS)
        return status;

    clear_ctx_buffer(ctx);

    return SUCCESS;
}

static pufs_status_t pufs_ccm_text_update(pufs_sp38c_ctx* ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    pufs_status_t status;
    size_t len = 0;
    uint32_t offset = 0, blocks = 0;
    bool previous_updated = false;
    *outlen = 0;

    switch (ctx->stage) {
    case SP38C_NONE:
        break;
    case SP38C_AAD:
        // update the last block of aad when stage is changed from AAD to TEXT.
        if (ctx->buflen > 0 && (status = pufs_ccm_aad_final(ctx)) != SUCCESS)
            return status;
        break;
    case SP38C_TEXT:
        // fill the previous incompleted block
        if (ctx->buflen > 0) {
            offset = fill_incomplete_block(ctx, in, inlen);
            // update the block if the buff is full and there are other incoming data.
            // if the buff is full and there is no more data, the block will be updated at final step.
            if (inlen - offset > 0 && ctx->buflen >= BC_BLOCK_SIZE) {
                if ((status = sp38c_ctx_update(ctx, NULL, ctx->ctri, ctx->buff, BC_BLOCK_SIZE, false, out)) != SUCCESS)
                    return status;

                uint8_t* cbcmac_src = ctx->encrypt ? ctx->buff : out;
                if ((status = sp38c_ctx_update(ctx, ctx->cbcmac, NULL, cbcmac_src, BC_BLOCK_SIZE, false, ctx->cbcmac)) != SUCCESS)
                    return status;

                clear_ctx_buffer(ctx);
                previous_updated = true;

                ctx->currentlen += BC_BLOCK_SIZE;
            }
        }
        break;
    default:
        return E_FIRMWARE;
    }

    len = inlen - offset;
    if (len <= 0) {
        ctx->stage = SP38C_TEXT;
        return SUCCESS;
    }

    blocks = cut_blocks(ctx, in + offset, len);

    if (blocks) {
        bool last = (ctx->buflen == 0) ? true : false;
        out = previous_updated ? out + BC_BLOCK_SIZE : out;
        if ((status = sp38c_ctx_update(ctx, NULL, ctx->ctri, in + offset, blocks * BC_BLOCK_SIZE, last, out)) != SUCCESS)
            return status;

        uint8_t* cbcmac_src = ctx->encrypt ? (uint8_t*)in + offset : out + offset;
        if ((status = sp38c_ctx_update(ctx, ctx->cbcmac, NULL, cbcmac_src, blocks * BC_BLOCK_SIZE, last, ctx->cbcmac)) != SUCCESS)
            return status;

        ctx->currentlen += blocks * BC_BLOCK_SIZE;
    }

    *outlen = (blocks * BC_BLOCK_SIZE);
    if (previous_updated)
        *outlen += BC_BLOCK_SIZE;

    /*
     * Mbed TLS CCM streaming expects the final update() call to emit all
     * remaining text bytes; finish() only returns the tag. Flush the last
     * partial block here when this call consumed the end of the message.
     */
    if (ctx->buflen > 0 && (ctx->currentlen + ctx->buflen) == ctx->inlen) {
        uint32_t tail_len = ctx->buflen;
        uint8_t *tail_out = out + *outlen;

        if ((status = sp38c_ctx_update(ctx, NULL, ctx->ctri, ctx->buff, tail_len, true, tail_out)) != SUCCESS)
            return status;

        if (!ctx->encrypt) {
            rvv_memset(ctx->buff, 0x0, BC_BLOCK_SIZE);
            rvv_memcpy(ctx->buff, tail_out, tail_len);
        }
        if ((status = sp38c_ctx_update(ctx, ctx->cbcmac, NULL, ctx->buff, BC_BLOCK_SIZE, true, ctx->cbcmac)) != SUCCESS)
            return status;

        ctx->currentlen += tail_len;
        *outlen += tail_len;
        clear_ctx_buffer(ctx);
    }

    ctx->stage = SP38C_TEXT;
    return SUCCESS;
}

static pufs_status_t pufs_ccm_text_final(pufs_sp38c_ctx* ctx,
    uint8_t* out,
    uint32_t* outlen)
{
    pufs_status_t status;
    if (ctx->buflen > 0) {
        if ((status = sp38c_ctx_update(ctx, NULL, ctx->ctri, ctx->buff, ctx->buflen, true, out)) != SUCCESS)
            return status;

        if (!ctx->encrypt) {
            rvv_memset(ctx->buff, 0x0, BC_BLOCK_SIZE);
            rvv_memcpy(ctx->buff, out, ctx->buflen);
        }
        if ((status = sp38c_ctx_update(ctx, ctx->cbcmac, NULL, ctx->buff, BC_BLOCK_SIZE, true, ctx->cbcmac)) != SUCCESS)
            return status;

        ctx->currentlen += ctx->buflen;
    }

    *outlen = ctx->buflen;
    return SUCCESS;
}

/**
 * @brief generate tag value.
 *        Tag = T ⊕ MSBTlen(S0)
 */
void ccm_cfg_print(void)
{
    for (int i = 0; i < sizeof(struct pufs_sp38c_regs) / 4; i++) {
        printf("reg[%d] = 0x%x\n", i, *((uint32_t*)sp38c_regs + i));
    }
}
void dma_print(void)
{
    printf("\nversion=0x%x, interrupt=0x%x, feature=0x%x, status0=0x%x, status1=0x%x, start=0x%x, cfg0=0x%x, cfg1=0x%x, \
            dsc_cfg_0=0x%x, dsc_cfg_1=0x%x, dsc_cfg_2=0x%x, dsc_cfg_3=0x%x, dsc_cfg_4=0x%x, \
            dsc_cur_0=0x%x, dsc_cur_1=0x%x, dsc_cur_2=0x%x, dsc_cur_3=0x%x, dsc_cur_4=0x%x, \
            key_cfg_0=0x%x, cl_cfg_0=0x%x\n",
        pufs_dma.regs->version, pufs_dma.regs->interrupt, pufs_dma.regs->feature, pufs_dma.regs->status_0, pufs_dma.regs->status_1,
        pufs_dma.regs->start, pufs_dma.regs->cfg_0, pufs_dma.regs->cfg_1, pufs_dma.regs->dsc_cfg_0, pufs_dma.regs->dsc_cfg_1,
        pufs_dma.regs->dsc_cfg_2, pufs_dma.regs->dsc_cfg_3, pufs_dma.regs->dsc_cfg_4, pufs_dma.regs->dsc_cur_0, pufs_dma.regs->dsc_cur_1,
        pufs_dma.regs->dsc_cur_2, pufs_dma.regs->dsc_cur_3, pufs_dma.regs->dsc_cur_4, pufs_dma.regs->key_cfg_0, pufs_dma.regs->cl_cfg_0);
}
static pufs_status_t pufs_ccm_tag(pufs_sp38c_ctx* ctx,
    uint8_t* tag,
    bool from_reg)
{
    // ccm_cfg_print();
    pufs_status_t check;
    uint32_t val32;
    // reset counter to get CTR0
    ctx->currentlen = ULLONG_MAX;
    crypto_write_dgst(ctx->cbcmac, BC_BLOCK_SIZE);

    if (!from_reg)
        return sp38c_ctx_update(ctx, NULL, ctx->ctri, ctx->cbcmac, ctx->taglen, true, tag);

    crypto_write_iv(ctx->ctri, BC_BLOCK_SIZE);

    if ((check = sp38c_get_config(&val32, ctx, false, true, true)) != SUCCESS)
        return check;

    sp38c_regs->cfg = val32;
    sp38c_regs->block_num = 0;

    // dma_print();
    dma_write_rwcfg(NULL, NULL, 0);
    dma_write_data_block_config(true, true, true, true, 0);
    dma_write_config_0(false, false, false);
    // dma_print();

    dma_write_start();

    while (dma_check_busy_status(&val32))
        ;

    if (val32 != 0) {
        LOG_ERROR("DMA status 0: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    val32 = sp38c_regs->status;

    if ((val32 & SP38C_STATUS_RESP_MASK) != 0) {
        LOG_ERROR("SP38C status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    // dma_print();
    // printf("tag = 0x%x\n", *(uint32_t*)tag);
    crypto_read_dgest(tag, ctx->taglen);
    // printf("tag = 0x%x\n", *(uint32_t*)tag);

    // ccm_cfg_print();
    // dma_print();
    return SUCCESS;
}

static void sp38c_release_phybuf(pufs_sp38c_ctx* ctx)
{
    sp38c_phybuf_record_st *cur = ctx->phybuf_list, *tmp;

    while (cur != NULL) {
        if (cur->writeback_addr != 0) {
            rvv_memcpy((void*)DMA_RBUF_VIRT_ADDR(cur->writeback_addr), (void*)VIRT_ADDR(cur->buf_addr),
                cur->size);
        }

#ifdef BAREMETAL
        free((void*)cur->buf_addr);
#else
        dma_sg_free_descriptor((pufs_dma_sg_internal_desc_st*)VIRT_ADDR(cur->buf_addr));
#endif /* BAREMETAL */

        tmp = cur;
        cur = cur->next;
        free(tmp);
    }

    ctx->phybuf_list = NULL;
}

static pufs_status_t sp38c_ctx_sg_done(pufs_sp38c_ctx* ctx, uint8_t* tag)
{
    pufs_status_t check;

    sp38c_release_phybuf(ctx);

    crypto_io_read_dgest(ctx->crypto_io_ctx, ctx->cbcmac, BC_BLOCK_SIZE);
    crypto_write_dgst(ctx->cbcmac, BC_BLOCK_SIZE);

    sp38c_setup_key(ctx);
    check = pufs_ccm_tag(ctx, tag, true);

    crypto_free_crypto_io_ctx(ctx->crypto_io_ctx);
    ctx->crypto_io_ctx = NULL;
    ctx->op = SP38C_AVAILABLE;

    return check;
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
/**
 * pufs_sp38c_module_init()
 */
void pufs_sp38c_module_init(uintptr_t sp38c_offset)
{
    sp38c_regs = (struct pufs_sp38c_regs*)(pufs_context.base_addr + sp38c_offset);
    version_check(SP38C_VERSION, sp38c_regs->version);
}
/**
 * pufs_sp38c_ctx_new()
 */
pufs_sp38c_ctx* pufs_sp38c_ctx_new(void)
{
    pufs_sp38c_ctx* ctx;

    ctx = malloc(sizeof(pufs_sp38c_ctx));
    if (ctx != NULL) {
        ctx->op = SP38C_AVAILABLE;
        rvv_memset(ctx, 0x0, sizeof(pufs_sp38c_ctx));
    }

    return ctx;
}
/**
 * pufs_sp38c_ctx_free()
 */
void pufs_sp38c_ctx_free(pufs_sp38c_ctx* ctx)
{
    if (ctx != NULL) {
        rvv_memset(ctx, 0, sizeof(pufs_sp38c_ctx));
        ctx->op = SP38C_AVAILABLE;
    }
    free(ctx);
}
/**
 * _pufs_enc_ccm_init()
 */
pufs_status_t _pufs_enc_ccm_init(pufs_sp38c_ctx* sp38c_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* nonce,
    uint32_t noncelen,
    uint64_t aadlen,
    uint64_t inlen,
    uint32_t taglen)
{
    return sp38c_ctx_init(sp38c_ctx,
        cipher,
        true,
        keytype,
        keyaddr,
        keybits,
        nonce,
        noncelen,
        aadlen,
        inlen,
        taglen);
}
/**
 * pufs_enc_ccm_update
 */
pufs_status_t pufs_enc_ccm_update(pufs_sp38c_ctx* sp38c_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    sp38c_setup_key(sp38c_ctx);

    if (out == NULL)
        return pufs_ccm_aad_update(sp38c_ctx, in, inlen);
    else
        return pufs_ccm_text_update(sp38c_ctx, out, outlen, in, inlen);
}

pufs_status_t pufs_enc_ccm_sg_append(pufs_sp38c_ctx* sp38c_ctx,
    pufs_ccm_data_t data_type,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38c_ctx_sg_append(sp38c_ctx, data_type, descs, descs_len, last);
}

pufs_status_t pufs_enc_ccm_sg_done(pufs_sp38c_ctx* ctx, uint8_t* tag)
{
    return sp38c_ctx_sg_done(ctx, tag);
}

static pufs_status_t ccm_enc_final(pufs_sp38c_ctx* ctx,
    uint8_t* out,
    uint32_t* outlen,
    uint8_t* tag,
    bool from_reg)
{
    pufs_status_t status;
    sp38c_setup_key(ctx);

    if ((status = pufs_ccm_text_final(ctx, out, outlen)) != SUCCESS)
        goto release;

    if ((status = pufs_ccm_tag(ctx, tag, from_reg)) != SUCCESS)
        goto release;

release:
    ctx->op = SP38C_AVAILABLE;
    return status;
}

/**
 * pufs_enc_ccm_final
 */
pufs_status_t pufs_enc_ccm_final(pufs_sp38c_ctx* sp38c_ctx,
    uint8_t* out,
    uint32_t* outlen,
    uint8_t* tag)
{
    return ccm_enc_final(sp38c_ctx, out, outlen, tag, false);
}
/**
 * _pufs_enc_ccm
 */
pufs_status_t _pufs_enc_ccm(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* nonce,
    uint32_t noncelen,
    const uint8_t* aad,
    uint32_t aadlen,
    uint8_t* tag,
    uint32_t taglen)
{
    pufs_status_t status;
    uint32_t len = 0;

    pufs_sp38c_ctx ctx = { .op = SP38C_AVAILABLE };
    if ((status = _pufs_enc_ccm_init(&ctx, cipher, keytype, keyaddr, keybits, nonce, noncelen, aadlen, inlen, taglen)) != SUCCESS)
        return status;

    if ((status = pufs_enc_ccm_update(&ctx, NULL, NULL, aad, aadlen)) != SUCCESS)
        return status;

    if ((status = pufs_enc_ccm_update(&ctx, out, &len, in, inlen)) != SUCCESS)
        return status;

    if ((status = ccm_enc_final(&ctx, out + len, outlen, tag, true)) != SUCCESS)
        return status;

    *outlen += len;
    return SUCCESS;
}
/**
 *
 */
pufs_status_t _pufs_dec_ccm_init(pufs_sp38c_ctx* sp38c_ctx,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* nonce,
    uint32_t noncelen,
    uint64_t aadlen,
    uint64_t inlen,
    uint32_t taglen)
{
    return sp38c_ctx_init(sp38c_ctx,
        cipher,
        false,
        keytype,
        keyaddr,
        keybits,
        nonce,
        noncelen,
        aadlen,
        inlen,
        taglen);
}
/**
 *
 */
pufs_status_t pufs_dec_ccm_update(pufs_sp38c_ctx* sp38c_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen)
{
    sp38c_setup_key(sp38c_ctx);

    if (out == NULL)
        return pufs_ccm_aad_update(sp38c_ctx, in, inlen);
    else
        return pufs_ccm_text_update(sp38c_ctx, out, outlen, in, inlen);

    return E_FIRMWARE;
}
pufs_status_t pufs_dec_ccm_sg_append(pufs_sp38c_ctx* sp38c_ctx,
    pufs_ccm_data_t data_type,
    pufs_dma_sg_desc_st* descs,
    uint32_t descs_len,
    bool last)
{
    return sp38c_ctx_sg_append(sp38c_ctx, data_type, descs, descs_len, last);
}

pufs_status_t pufs_dec_ccm_sg_done(pufs_sp38c_ctx* sp38c_ctx, const uint8_t* tag)
{
    pufs_status_t check;
    rvv_memset(sp38c_ctx->buff, 0x0, BC_BLOCK_SIZE);

    if ((check = sp38c_ctx_sg_done(sp38c_ctx, sp38c_ctx->buff)) != SUCCESS)
        return check;

    if (memcmp(sp38c_ctx->buff, tag, sp38c_ctx->taglen) != 0)
        return E_VERFAIL;

    return SUCCESS;
}

pufs_status_t pufs_dec_ccm_final_tag(pufs_sp38c_ctx* sp38c_ctx,
    uint8_t* out,
    uint32_t* outlen,
    uint8_t* tag)
{
    pufs_status_t status;

    sp38c_setup_key(sp38c_ctx);
    if ((status = pufs_ccm_text_final(sp38c_ctx, out, outlen)) != SUCCESS)
        goto release;

    rvv_memset(tag, 0x0, BC_BLOCK_SIZE);

    if ((status = pufs_ccm_tag(sp38c_ctx, tag, false)) != SUCCESS)
        goto release;

release:
    sp38c_ctx->op = SP38C_AVAILABLE;
    return status;
}
/**
 *
 */
pufs_status_t pufs_dec_ccm_final(pufs_sp38c_ctx* sp38c_ctx,
    uint8_t* out,
    uint32_t* outlen,
    const uint8_t* tag)
{
    uint8_t ttag[16];
    pufs_status_t status = pufs_dec_ccm_final_tag(sp38c_ctx, out, outlen, ttag);

    if (status != SUCCESS)
        return status;

    if (memcmp(ttag, tag, sp38c_ctx->taglen) != 0)
        status = E_VERFAIL;

    return status;
}
/**
 *
 */
pufs_status_t _pufs_dec_ccm(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_cipher_t cipher,
    pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    const uint8_t* nonce,
    int noncelen,
    const uint8_t* aad,
    int aadlen,
    const uint8_t* tag,
    int taglen)
{
    pufs_status_t status;
    uint32_t len = 0;

    pufs_sp38c_ctx ctx = { .op = SP38C_AVAILABLE };
    if ((status = _pufs_dec_ccm_init(&ctx, cipher, keytype, keyaddr, keybits, nonce, noncelen, aadlen, inlen, taglen)) != SUCCESS)
        return status;

    if ((status = pufs_dec_ccm_update(&ctx, NULL, NULL, aad, aadlen)) != SUCCESS)
        return status;

    if ((status = pufs_dec_ccm_update(&ctx, out, &len, in, inlen)) != SUCCESS)
        return status;

    if ((status = pufs_dec_ccm_final(&ctx, out + len, outlen, tag)) != SUCCESS)
        return status;

    *outlen += len;
    return SUCCESS;
}
