/**
 * @file      pufs_kdf.c
 * @brief     PUFsecurity KDF API implementation
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
#include "pufs_kdf_regs.h"
#include "pufs_kdf_internal.h"
#include "pufs_dma_internal.h"
#include "pufs_hmac_internal.h"
#include "pufs_ka_internal.h"

struct pufs_kdf_regs* kdf_regs = NULL;

/*****************************************************************************
 * Static variables
 ****************************************************************************/
#define MINFOLEN 1024
static uint8_t* minfo;

/*****************************************************************************
 * Static functions
 ****************************************************************************/
/**
 * @brief Check HKDF hash/HMAC support
 *
 * @return SUCCESS if supported, otherwise an error.
 */
static pufs_status_t pufs_kdf_prf_hash_check(pufs_prf_family_t prf,
    pufs_hash_t hash)
{
    pufs_status_t check;
    pufs_hmac_ctx hmac_ctx = { .op = HMAC_AVAILABLE };

    // check by _init function
    switch (prf) {
    case PRF_HASH:
        check = pufs_hash_init(&hmac_ctx, hash);
        break;
    case PRF_HMAC:
        check = pufs_hmac_init(&hmac_ctx, hash, SWKEY, NULL, 0);
        break;
    default:
        check = E_INVALID;
        break;
    }

    return check;
}

/**
 * @brief Check and build KDF counter configuration
 *
 * @return counter configuration legality
 */
static pufs_status_t pufs_kdf_gen_cnt(struct pufs_kdf_cnt_params* params, uint32_t* cnt)
{
    *cnt = 0;
    // Counter mode
    if (!params->feedback && (params->length < 1 || params->length > 4))
        return E_INVALID;

    if (params->feedback && params->length > 4)
        return E_INVALID;

    if (params->length == 0)
        return SUCCESS;

    // Enable counter of interation
    *cnt |= 0x1;

    *cnt |= params->position << KDF_CNT_POS_BITS;

    if (params->position == COUNTER_POS_BEFORE_FIXED || params->position == COUNTER_POS_MID)
        *cnt |= params->order << KDF_CNT_ORD_BITS;

    *cnt |= (params->length - 1) << KDF_CNT_LEN_BITS;

    return SUCCESS;
}

/**
 * @brief Check and build KDF configuration
 *
 * @return KDF configuration legality
 */
static pufs_status_t pufs_kdf_gen_cfg(struct pufs_kdf_cfg_params* params, uint32_t* cfg, uint32_t* ivlen)
{
    uint32_t val32;
    pufs_status_t check;
    *cfg = 0;
    *ivlen = 0;

    switch (params->method) {
    case METHOD_PBKDF:
        *cfg |= 0x0;
        break;
    case METHOD_KBKDF_EXPAND:
        *cfg |= 0x1;
        break;
    case METHOD_KBKDF_EXTRACT:
        *cfg |= 0x2;
        break;
    case METHOD_KBKDF_EXPAND_EXTRACT:
        *cfg |= 0x3;
        break;
    case METHOD_SM2:
        *cfg |= 0x4;
        break;
    default:
        return E_INVALID;
    }

    // 1. check keytype, only SSKEY and SHARESEC are permitted
    switch (params->keytype) {
    case SSKEY:
        *cfg |= 0x0 << KDF_CFG_KEY_TYPE_BITS;
        break;
    case SHARESEC:
        *cfg |= 0x1 << KDF_CFG_KEY_TYPE_BITS;
        break;
    default:
        return E_INVALID;
    }

    // 2. check KA key slot by key length
    if ((check = keyslot_check(false, params->keytype, params->keyslot, params->outbits)) != SUCCESS)
        return check;
    *cfg |= get_key_slot_idx(params->keytype, params->keyslot) << KDF_CFG_KEY_IDX_TO_BITS;

    // 3. check prf
    val32 = kdf_regs->feature;
    switch (params->prf) {
    case PRF_HMAC:
    case PRF_HASH:
        *cfg |= ((params->prf == PRF_HMAC) ? 0x1 : 0x0) << KDF_CFG_PRF_BITS;
        if ((val32 & KDF_FEATURE_HMAC_KDF_MASK) == 0)
            return E_UNSUPPORT;
        switch (params->hash) {
        case SHA_224:
            *ivlen = 28;
            *cfg |= 0x2 << KDF_CFG_VARIANT_BITS;
            break;
        case SHA_256:
            *ivlen = 32;
            *cfg |= 0x3 << KDF_CFG_VARIANT_BITS;
            break;
        case SHA_384:
            *ivlen = 48;
            *cfg |= 0x4 << KDF_CFG_VARIANT_BITS;
            break;
        case SHA_512:
            *ivlen = 64;
            *cfg |= 0x5 << KDF_CFG_VARIANT_BITS;
            break;
        case SHA_512_224:
            *ivlen = 28;
            *cfg |= 0x6 << KDF_CFG_VARIANT_BITS;
            break;
        case SHA_512_256:
            *ivlen = 32;
            *cfg |= 0x7 << KDF_CFG_VARIANT_BITS;
            break;
        case SM3:
            *ivlen = 32;
            *cfg |= 0x8 << KDF_CFG_VARIANT_BITS;
            break;
        default:
            return E_INVALID;
        }
        // iv is only available in feedback mode
        if (!params->feedback)
            *ivlen = 0;
        break;
    case PRF_CMAC:
        *cfg |= 0x2 << KDF_CFG_PRF_BITS;
        if ((val32 & KDF_FEATURE_CMAC_KDF_MASK) == 0)
            return E_UNSUPPORT;
        // TODO: IV length is fixed value for CAVP test vactors.
        // It may be refactored if we need to support different length in the future.
        if (params->feedback)
            *ivlen = 16;
        switch (params->zbits) {
        case 128:
            *cfg |= 0x0 << KDF_CFG_VARIANT_BITS;
            break;
        case 192:
            *cfg |= 0x1 << KDF_CFG_VARIANT_BITS;
            break;
        case 256:
            *cfg |= 0x2 << KDF_CFG_VARIANT_BITS;
            break;
        }
        break;
    default:
        return E_UNSUPPORT;
    }

    if (params->feedback)
        *cfg |= 0x1 << KDF_CFG_FBEN_BITS;

    if (params->feedback && params->iv != NULL)
        *cfg |= 0x1 << KDF_CFG_IVEN_BITS;

    return SUCCESS;
}
/**
 * @brief Check and configure DMA for KDF
 *
 * @return DMA configuration status
 */
static pufs_status_t pufs_kdf_cfg_dma(pufs_key_type_t keytype,
    size_t keyaddr,
    uint32_t keybits,
    pufs_kdf_md_t md,
    const uint8_t* salt,
    uint32_t saltlen,
    const uint8_t* info,
    uint32_t infolen,
    uint32_t cutlen,
    uint32_t ctrlen,
    uint8_t* out)
{
    pufs_status_t check;

    // check DMA busy
    if (dma_check_busy_status(NULL))
        return E_BUSY;

    // check key
    if (keytype == SWKEY) {
        if (keybits > 512)
            return E_OVERFLOW;
    } else if (keytype == SHARESEC) {
        if ((check = pufs_get_ss_keybits(&keybits,
                 (pufs_ka_slot_t)keyaddr))
            != SUCCESS)
            return check;
    } else {
        if ((check = keyslot_check(true, keytype, keyaddr, keybits)) != SUCCESS)
            return check;
    }

    // handle salt
    if ((salt != NULL) && (B2b(saltlen) > 512))
        return E_OVERFLOW;
    rvv_memset(pufs_buffer, 0, DGST_INT_STATE_LEN);
    rvv_memcpy(pufs_buffer, salt, saltlen);

    crypto_write_dgst(pufs_buffer, DGST_INT_STATE_LEN);

    // DMA cfg
    dma_write_config_0(false, false, false);

    // DMA descriptor
    dma_write_data_block_config(true, true, true, true, 0);
    minfo = NULL;
    if (cutlen == 0) {
        dma_write_rwcfg(out, info, infolen);
    } else {
        uint32_t minfolen = infolen + ctrlen;
        if (minfolen > MINFOLEN)
            return E_OVERFLOW;
        if ((minfo = malloc(minfolen)) == NULL) {
            err(1, "malloc failed");
            return E_OVERFLOW;
        }
        rvv_memcpy(minfo, info, cutlen);
        rvv_memset(minfo + cutlen, 0, ctrlen);
        rvv_memcpy(minfo + cutlen + ctrlen, info + cutlen, infolen - cutlen);
        dma_write_rwcfg(out, minfo, minfolen);
    }

    // DMA key
    if (keytype == SWKEY) {
        rvv_memset(pufs_buffer, 0, SW_KEY_MAXLEN);
        rvv_memcpy(pufs_buffer, (const void*)keyaddr, b2B(keybits));
        crypto_write_sw_key(pufs_buffer, SW_KEY_MAXLEN);
    }

    dma_write_key_config_0(keytype, (md == METHOD_SM2 ? ALGO_TYPE_SM2ENC : ALGO_TYPE_HKDF), keybits, get_key_slot_idx(keytype, keyaddr));

    return SUCCESS;
}
/**
 * @brief Configure/start KDF and wait until done
 *
 * @return KDF execution status
 */
static pufs_status_t pufs_kdf_start(uint32_t cfg, uint32_t cnt, uint32_t outbits, uint32_t kcount,
    const uint8_t* iv, uint32_t ivlen, uint32_t saltlen)
{
    uint32_t val32;

    kdf_regs->cfg = cfg;
    kdf_regs->cnt = cnt;
    kdf_regs->klen = outbits;
    kdf_regs->kcount = kcount;
    kdf_regs->iv_len = ivlen;
    kdf_regs->salt_len = saltlen;

    if (iv != NULL) {
        rvv_memset(pufs_buffer, 0, KDF_IV_LEN);
        rvv_memcpy(pufs_buffer, iv, ivlen);
        for (int i = 0; i < KDF_IV_LEN; i += 4) {
            val32 = be2le(*((uint32_t*)(pufs_buffer + i)));
            *((uint32_t*)(kdf_regs->iv + i)) = val32;
        }
    }

    dma_write_start();
    val32 = dma_wait_done();
    if (minfo != NULL) {
        free(minfo);
        minfo = NULL;
    }
    if (val32) {
        LOG_ERROR("pufs dma wait timeout\n");
        return E_ERROR;
    }
    dma_check_busy_status(&val32);

    if (val32 != 0) {
        LOG_ERROR("DMA status 0: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    val32 = kdf_regs->status;
    if (val32 != 0) {
        LOG_ERROR("KDF status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    return SUCCESS;
}

static pufs_status_t _pufs_kdf_base(
    pufs_key_type_t keytype, pufs_ka_slot_t keyslot, uint32_t outbits,
    pufs_kdf_md_t md, pufs_prf_family_t prf, pufs_hash_t hash, uint32_t iter,
    bool feedback, const uint8_t* iv,
    uint32_t ctrpos, uint32_t ctrlen,
    pufs_key_type_t ztype, size_t zaddr, uint32_t zbits,
    const uint8_t* salt, uint32_t saltlen,
    const uint8_t* info, uint32_t infolen, uint8_t* out)
{
    uint32_t cfg, cnt, ivlen, cutlen = 0, cnt_order = 0;
    pufs_status_t check;
    pufs_kdf_cnt_pos_t cnt_pos;

    // 800-56C - 5.1 Specification of Key-Derivation Procedure
    switch (prf) {
    case PRF_HMAC:
        if ((check = pufs_kdf_prf_hash_check(PRF_HMAC, hash)) != SUCCESS)
            return check;
        break;
    case PRF_CMAC:
    case PRF_HASH:
        break;
    default:
        return E_INVALID;
    }

    struct pufs_kdf_cfg_params cfg_params = {
        .keytype = keytype,
        .keyslot = keyslot,
        .outbits = outbits,
        .prf = prf,
        .method = md,
        .hash = hash,
        .zbits = zbits,
        .feedback = feedback,
        .iv = iv,
    };

    if ((check = pufs_kdf_gen_cfg(&cfg_params, &cfg, &ivlen)) != SUCCESS)
        return check;

    if (feedback) {
        switch (ctrpos) {
        case 0:
        case 1:
        case 2:
            cnt_pos = (pufs_kdf_cnt_pos_t)ctrpos;
            if (cnt_pos == COUNTER_POS_BEFORE_FIXED)
                cnt_order = ivlen;
            break;
        default:
            return E_INVALID;
        }
    } else {
        if (ctrpos > infolen)
            return E_INVALID;

        if (ctrpos == 0) {
            cnt_pos = COUNTER_POS_BEFORE;
        } else if (ctrpos == infolen) {
            cnt_pos = COUNTER_POS_AFTER;
        } else {
            cnt_pos = COUNTER_POS_MID;
            cnt_order = ctrpos;
            cutlen = ctrpos;
        }
    }

    // special case: the counter pos is fixed in PBKDF
    if (md == METHOD_PBKDF)
        cnt_pos = COUNTER_POS_AFTER;

    struct pufs_kdf_cnt_params cnt_params = {
        .feedback = feedback,
        .length = ctrlen,
        .position = cnt_pos,
        .order = cnt_order,
    };

    // the cnt value of SM2 method is fixed in HW, so skip the step.
    if (md == METHOD_SM2)
        cnt = 0x0;
    else if ((check = pufs_kdf_gen_cnt(&cnt_params, &cnt)) != SUCCESS)
        return check;

    // config DMA
    if ((check = pufs_kdf_cfg_dma(ztype, zaddr, zbits, md, salt, saltlen, info,
             infolen, cutlen, ctrlen, out))
        != SUCCESS)
        return check;

    return pufs_kdf_start(cfg, cnt, outbits, iter,
        (feedback ? iv : NULL), ivlen, B2b(saltlen));
}

/*****************************************************************************
 * Internal functions
 ****************************************************************************/

pufs_status_t pufs_sm2kdf(
    pufs_key_type_t keytype,
    pufs_ka_slot_t keyslot,
    uint32_t outbits,
    pufs_key_type_t ztype,
    size_t zaddr,
    uint32_t zbits,
    const uint8_t* info,
    uint32_t infolen,
    uint8_t* out)
{
    return _pufs_kdf_base(
        keytype, keyslot, outbits,
        METHOD_SM2, PRF_HASH, SM3, 1,
        false, NULL,
        0, 0,
        ztype, zaddr, zbits,
        0, 0,
        info, infolen, out);
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
/**
 * pufs_kdf_module_init()
 */
void pufs_kdf_module_init(uintptr_t kdf_offset)
{
    kdf_regs = (struct pufs_kdf_regs*)(pufs_context.base_addr + kdf_offset);
    version_check(KDF_VERSION, kdf_regs->version);
    LOG_INFO("%s", "KDF module is initialized");
}
/**
 * _pufs_kdf()
 */
pufs_status_t _pufs_kdf(
    pufs_key_type_t keytype, pufs_ka_slot_t keyslot, uint32_t outbits,
    pufs_prf_family_t prf, pufs_hash_t hash, bool feedback, const uint8_t* iv,
    uint32_t ctrpos, uint32_t ctrlen, pufs_key_type_t ztype, size_t zaddr,
    uint32_t zbits, const uint8_t* salt, uint32_t saltlen, const uint8_t* info,
    uint32_t infolen)
{
    return _pufs_kdf_base(
        keytype, keyslot, outbits,
        METHOD_KBKDF_EXPAND_EXTRACT, prf, hash, 1,
        feedback, iv,
        ctrpos, ctrlen,
        ztype, zaddr, zbits,
        salt, saltlen,
        info, infolen, NULL);
}
/**
 * _pufs_key_expansion()
 */
pufs_status_t _pufs_key_expansion(
    pufs_key_type_t keytype, pufs_ka_slot_t keyslot, uint32_t outbits, pufs_prf_family_t prf,
    pufs_hash_t hash, bool feedback, const uint8_t* iv, uint32_t ctrpos,
    uint32_t ctrlen, pufs_key_type_t kdktype, size_t kdkaddr, uint32_t kdkbits,
    const uint8_t* info, uint32_t infolen)
{
    return _pufs_kdf_base(
        keytype, keyslot, outbits,
        METHOD_KBKDF_EXPAND, prf, hash, 1,
        feedback, iv,
        ctrpos, ctrlen,
        kdktype, kdkaddr, kdkbits,
        NULL, 0,
        info, infolen, NULL);
}
/**
 * _pufs_pbkdf()
 */
pufs_status_t _pufs_pbkdf(pufs_key_type_t keytype,
    pufs_ka_slot_t keyslot,
    uint32_t outbits,
    pufs_prf_family_t prf,
    pufs_hash_t hash,
    uint32_t iter,
    pufs_key_type_t salttype,
    size_t saltaddr,
    uint32_t saltbits,
    const uint8_t* pass,
    uint32_t passlen)
{
    // It only supports HMAC PRF in this version(0x484B4400).
    if (prf != PRF_HMAC)
        return E_UNSUPPORT;

    if (saltbits == 0 || pass == NULL || passlen == 0)
        return E_INVALID;

    return _pufs_kdf_base(
        keytype, keyslot, outbits,
        METHOD_PBKDF, prf, hash, iter,
        false, NULL,
        0, 4,
        salttype, saltaddr, saltbits,
        pass, passlen,
        NULL, 0, NULL);
}

pufs_status_t pufs_kdf_base(
    pufs_key_type_t keytype, pufs_ka_slot_t keyslot, uint32_t outbits,
    pufs_kdf_md_t md, pufs_prf_family_t prf, pufs_hash_t hash, uint32_t iter,
    bool feedback, const uint8_t* iv,
    uint32_t ctrpos, uint32_t ctrlen,
    pufs_key_type_t ztype, size_t zaddr, uint32_t zbits,
    const uint8_t* salt, uint32_t saltlen,
    const uint8_t* info, uint32_t infolen, uint8_t* out)
{
    return _pufs_kdf_base(
        keytype, keyslot, outbits,
        md, prf, hash, iter,
        feedback, iv,
        ctrpos, ctrlen,
        ztype, zaddr, zbits,
        salt, saltlen,
        info, infolen, out);
}
