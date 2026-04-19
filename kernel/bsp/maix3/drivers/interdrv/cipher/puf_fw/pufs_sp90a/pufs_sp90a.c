/**
 * @file      pufs_sp90a.c
 * @brief     PUFsecurity SP90A API implementation
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
#include <string.h>
#include "pufs_internal.h"
#include "pufs_sp38a_internal.h"
#include "pufs_sp90a_internal.h"
#include "pufs_rt_internal.h"

struct pufs_drbg_regs* drbg_regs = NULL;

/*****************************************************************************
 * Static variables
 ****************************************************************************/
#define INT_ENTROPY_MAXLEN 128
/**
 * @brief internal entropy
 */
static uint8_t intent[INT_ENTROPY_MAXLEN];
static uint32_t intentlen = 0;
/**
 * @brief derivation function output buffer
 */
static uint8_t dfbuf[PRE_SEED_LEN];
/**
 * @brief Block_Cipher_df pre-defined key
 */
static const void* bcdfkey = "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f";

/*****************************************************************************
 * Static functions
 ****************************************************************************/
/**
 * @brief Drive SP90A hardware
 *
 * @param[in] cfg    cfg register setting
 * @param[in] in     Input
 * @param[in] inlen  Length of input in bytes
 * @return           SUCCESS on success, otherwise an error code.
 */

static bool drbg_check_busy_status(uint32_t* status)
{
    uint32_t stat = drbg_regs->status_0;
    bool busy = (stat & SP90A_STATUS_0_BUSY_MASK) != 0;

    if (status != NULL)
        *status = stat;

    return busy;
}

static pufs_status_t pufs_drbg_update(uint32_t cfg,
    const uint8_t* in,
    uint32_t inlen)
{
    if ((in == NULL) && (inlen != 0))
        return E_INVALID;

    drbg_regs->cfg = cfg;

    uint32_t val32;
    rvv_memset(pufs_buffer, 0, PRE_SEED_LEN);
    if (inlen > PRE_SEED_LEN)
        inlen = PRE_SEED_LEN;
    rvv_memcpy(pufs_buffer, in, inlen);

    uint32_t* puf32 = (uint32_t*)pufs_buffer;
    for (int i = 0; i < (PRE_SEED_LEN / 4); ++i)
        drbg_regs->pre_seed[i] = be2le(*(puf32 + i));

    drbg_regs->start = 0x1 << 0;

    while (drbg_check_busy_status(&val32))
        ;

    if (val32 != 0) {
        LOG_ERROR("SP90A status 0: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    return SUCCESS;
}
/**
 * @brief Block_Cipher_df in SP90A
 *
 * @param[out] outlen   Output seed material length in bytes.
 * @param[in]  algo     0/1/2 for AES-128/192/256.
 * @param[in]  entropy  True for using entropy, false otherwise.
 * @param[in]  in1      First input.
 * @param[in]  in1len   First input length in bytes.
 * @param[in]  in2      Second input.
 * @param[in]  in2len   Second input length in bytes.
 * @return              SUCCESS on success, otherwise an error code.
 */
pufs_status_t bc_df(uint32_t* dflen, uint32_t algo, bool entropy,
    const uint8_t* in1, uint32_t in1len, const uint8_t* in2,
    uint32_t in2len)
{
    const uint32_t blocklen = 16;
    union {
        uint8_t uc[PRE_SEED_LEN];
        uint32_t u32[PRE_SEED_LEN / 4];
    } tmp;
    pufs_status_t check;

    uint32_t keybits = 0;
    uint32_t entlen = 0;
    switch (algo) {
    case 0:
        keybits = 128;
        break;
    case 1:
        keybits = 192;
        break;
    case 2:
        keybits = 256;
        break;
    default:
        return E_FIRMWARE;
    }
    if (entropy)
        entlen = intentlen;

    // prepare input: IV + inlen + outlen + input + 0x80 + padding 0's
    *dflen = b2B(keybits) + blocklen;
    rvv_memset(tmp.uc, 0, PRE_SEED_LEN);
    tmp.u32[blocklen / 4] = be2le(entlen + in1len + in2len);
    tmp.u32[blocklen / 4 + 1] = be2le(*dflen);
    tmp.uc[blocklen + 8] = 0x80;
    uint32_t padlen = blocklen - ((8 + entlen + in1len + in2len) % blocklen);
    uint32_t toutlen;

    // generate 1st round K, X via BCC
    pufs_sp38a_ctx sp38a_ctx = { .op = SP38A_AVAILABLE };
    for (uint32_t cnt = 0, curlen = 0; curlen < *dflen; cnt++) {
        uint8_t* out = (dfbuf + cnt * blocklen);
        tmp.u32[0] = be2le(cnt);
        if ((check = pufs_enc_cbc_init(&sp38a_ctx, AES, SWKEY, bcdfkey, keybits,
                 tmp.uc + PRE_SEED_LEN - blocklen,
                 0))
            != SUCCESS)
            return check;
        if ((check = pufs_enc_cbc_update(&sp38a_ctx, out, &toutlen,
                 tmp.uc, blocklen))
            != SUCCESS)
            return check;
        if ((check = pufs_enc_cbc_update(&sp38a_ctx, out, &toutlen,
                 tmp.uc + blocklen, 8))
            != SUCCESS)
            return check;
        for (uint32_t alen = 0; alen < entlen;) {
            uint32_t inlen = entlen - alen;
            if (inlen > blocklen)
                inlen = blocklen;
            if ((check = pufs_enc_cbc_update(&sp38a_ctx, out, &toutlen,
                     intent + alen, inlen))
                != SUCCESS)
                return check;
            alen += inlen;
        }
        for (uint32_t alen = 0; alen < in1len;) {
            uint32_t inlen = in1len - alen;
            if (inlen > blocklen)
                inlen = blocklen;
            if ((check = pufs_enc_cbc_update(&sp38a_ctx, out, &toutlen,
                     in1 + alen, inlen))
                != SUCCESS)
                return check;
            alen += inlen;
        }
        for (uint32_t alen = 0; alen < in2len;) {
            uint32_t inlen = in2len - alen;
            if (inlen > blocklen)
                inlen = blocklen;
            if ((check = pufs_enc_cbc_update(&sp38a_ctx, out, &toutlen,
                     in2 + alen, inlen))
                != SUCCESS)
                return check;
            alen += inlen;
        }
        if ((check = pufs_enc_cbc_update(&sp38a_ctx, out, &toutlen,
                 tmp.uc + blocklen + 8,
                 padlen))
            != SUCCESS)
            return check;
        if ((check = pufs_enc_cbc_final(&sp38a_ctx, out, &toutlen)) != SUCCESS)
            return check;
        if (toutlen != blocklen)
            return E_FIRMWARE;
        curlen += blocklen;
    }

    // Put 1st round K, X into tmp
    rvv_memcpy(tmp.uc, dfbuf, *dflen);

    const uint8_t* key = tmp.uc;
    uint8_t* out = dfbuf;
    if ((pufs_enc_ecb(out, &toutlen, tmp.uc + (keybits / 8),
             blocklen, AES, SWKEY, key, keybits)
            != SUCCESS)
        || (toutlen != blocklen))
        return E_ERROR;
    for (uint32_t i = blocklen; i < *dflen; i += blocklen) {
        out = dfbuf + i;
        if ((pufs_enc_ecb(out, &toutlen, (out - blocklen),
                 blocklen, AES, SWKEY, key, keybits)
                != 0)
            || (toutlen != blocklen))
            return E_ERROR;
    }

    return SUCCESS;
}

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * @brief Check if SP90A hardware is in test mode.
 *
 * @return  True if SP90A is in test mode; false otherwise.
 */
bool pufs_drbg_is_testmode(void)
{
    return ((drbg_regs->status_1 & SP90A_STATUS_1_TEST_MODE_MASK) != 0);
}
/**
 * pufs_drbg_enable_testmode()
 */
void pufs_drbg_enable_testmode(void)
{
    drbg_regs->test_mode = 0x1;
}
/**
 * pufs_drbg_testmode_entropy()
 */
void pufs_drbg_testmode_entropy(const uint8_t* entropy, uint32_t entlen)
{
    intentlen = entlen;
    if (intentlen > INT_ENTROPY_MAXLEN) {
        LOG_WARN("entropy length %" PRIu32 " exceeds maximum %d\n",
            intentlen, INT_ENTROPY_MAXLEN);
        intentlen = INT_ENTROPY_MAXLEN;
    }
    rvv_memcpy(intent, entropy, intentlen);
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_drbg_module_init(uint32_t drbg_offset)
{
    drbg_regs = (struct pufs_drbg_regs*)(pufs_context.base_addr + drbg_offset);
    version_check(DRBG_VERSION, drbg_regs->version);
}

/**
 * pufs_drbg_instantiate()
 */
pufs_status_t pufs_drbg_instantiate(pufs_drbg_t mode,
    uint32_t security,
    bool df,
    const uint8_t* nonce,
    uint32_t noncelen,
    const uint8_t* pstr,
    uint32_t pstrlen)
{
    uint32_t val32;
    int algo = -1;

    // check feature with required security strength
    val32 = drbg_regs->feature;
    switch (mode) {
    case AES_CTR_DRBG:
        if ((val32 & SP90A_FEATURE_CTR_DRBG_MASK) == 0)
            return E_UNSUPPORT;
        if (((val32 & SP90A_FEATURE_AES_128_MASK) != 0) && (security <= 128))
            algo = 0;
        else if (((val32 & SP90A_FEATURE_AES_192_MASK) != 0) && (security <= 192))
            algo = 1;
        else if (((val32 & SP90A_FEATURE_AES_256_MASK) != 0) && (security <= 256))
            algo = 2;
        else
            return E_UNSUPPORT;
        break;
    case HASH_DRBG:
        if ((val32 & SP90A_FEATURE_HASH_DRBG_MASK) == 0)
            return E_UNSUPPORT;
        break;
    case HMAC_DRBG:
        if ((val32 & SP90A_FEATURE_HMAC_DRBG_MASK) == 0)
            return E_UNSUPPORT;
        break;
    default:
        return E_INVALID;
    }
    // abort if DRBG is instantiated
    if ((drbg_regs->status_1 & SP90A_STATUS_1_INSTANT_MASK) != 0)
        return E_BUSY;

    if (algo < 0)
        return E_INVALID;

    // instantiate DRBG
    if (df) {
        uint32_t dflen;
        pufs_status_t check;

        if (pufs_drbg_is_testmode() == false) {
            pufs_rand(intent, INT_ENTROPY_MAXLEN / 4);
            intentlen = INT_ENTROPY_MAXLEN;
        }
        if ((check = bc_df(&dflen, algo, true, nonce,
                 noncelen, pstr, pstrlen))
            != SUCCESS)
            return check;
        return pufs_drbg_update((((uint32_t)algo) << 0 | 0 << 8 | 1 << 24),
            dfbuf, dflen);
    } else {
        if (pstrlen > PRE_SEED_LEN)
            pstrlen = PRE_SEED_LEN;
        if (pufs_drbg_is_testmode() == true) {
            for (uint32_t i = 0; i < pstrlen; i++)
                intent[i] ^= pstr[i];
        } else {
            rvv_memset(intent, 0, PRE_SEED_LEN);
            rvv_memcpy(intent, pstr, pstrlen);
        }
        return pufs_drbg_update((((uint32_t)algo) << 0 | 0 << 8 | 0 << 24),
            intent, PRE_SEED_LEN);
    }
}
/**
 * pufs_drbg_reseed()
 */
pufs_status_t pufs_drbg_reseed(bool df, const uint8_t* adin, uint32_t adinlen)
{
    uint32_t val32, algo;

    // abort if DRBG is not instantiated
    val32 = drbg_regs->status_1;
    if ((val32 & SP90A_STATUS_1_INSTANT_MASK) == 0)
        return E_INVALID;
    algo = (val32 & SP90A_STATUS_1_KEY_LEN_MASK) >> 8;

    // reseed DRBG
    if (df) {
        uint32_t dflen;
        pufs_status_t check;

        if (pufs_drbg_is_testmode() == false) {
            pufs_rand(intent, INT_ENTROPY_MAXLEN / 4);
            intentlen = INT_ENTROPY_MAXLEN;
        }
        if ((check = bc_df(&dflen, algo, true,
                 adin, adinlen, NULL, 0))
            != SUCCESS)
            return check;
        return pufs_drbg_update((((uint32_t)algo) << 0 | 1 << 8 | 1 << 24),
            dfbuf, dflen);
    } else {
        if (adinlen > PRE_SEED_LEN)
            adinlen = PRE_SEED_LEN;
        if (pufs_drbg_is_testmode() == true) {
            for (uint32_t i = 0; i < adinlen; i++)
                intent[i] ^= adin[i];
        } else {
            rvv_memset(intent, 0, PRE_SEED_LEN);
            rvv_memcpy(intent, adin, adinlen);
        }
        return pufs_drbg_update((algo << 0 | 1 << 8 | 0 << 24), intent, PRE_SEED_LEN);
    }
}
/**
 * _pufs_drbg_generate()
 */
pufs_status_t _pufs_drbg_generate(uint8_t* out,
    uint32_t outbits,
    bool pr,
    bool df,
    const uint8_t* adin,
    uint32_t adinlen,
    uint32_t testmode)
{
    pufs_status_t check;
    uint32_t val32, algo;

    // check outbits limit
    if (outbits > 0x80000) // 2^19
        return E_OVERFLOW;
    // abort if DRBG is not instantiated
    val32 = drbg_regs->status_1;
    if ((val32 & SP90A_STATUS_1_INSTANT_MASK) == 0)
        return E_INVALID;
    // check test mode consistency
    if (pufs_drbg_is_testmode() != testmode)
        return E_DENY;

    algo = (val32 & SP90A_STATUS_1_KEY_LEN_MASK) >> 8;
    // check if reseed is needed
    if (((val32 & SP90A_STATUS_1_RESEED_MASK) != 0) || pr) {
        if ((check = pufs_drbg_reseed(df, adin, adinlen)) != SUCCESS)
            return check;
        adinlen = 0;
    }

    uint32_t dflen = 0;
    // pre-generation
    if (adinlen != 0) {
        if (df) {
            if ((check = bc_df(&dflen, algo, false,
                     adin, adinlen, NULL, 0))
                != SUCCESS)
                return check;
            check = pufs_drbg_update((algo << 0 | 2 << 8 | 1 << 24), dfbuf, dflen);
        } else {
            check = pufs_drbg_update((algo << 0 | 2 << 8 | 0 << 24), adin, adinlen);
        }
        if (check != SUCCESS)
            return check;
    }

    // generate random bits
    uint32_t blocklen = 16;
    uint32_t outlen = b2B(outbits);
    uint32_t i = 0;
    uint32_t valid_check = SP90A_STATUS_1_RBITS_VALID_MASK;
    union {
        uint8_t byte[16];
        uint32_t word[4];
    } rbits;
    while (i < outlen) {
        // gen_rbits
        if ((check = pufs_drbg_update((algo << 0 | 3 << 8 | (df ? 1 : 0) << 24),
                 NULL, 0))
            != SUCCESS)
            return check;
        val32 = drbg_regs->status_1;
        if ((val32 & valid_check) != valid_check) {
            LOG_ERROR("SP90A status 1: 0x%08" PRIx32 "\n", val32);
            return E_ERROR;
        }
        // read the result
        // rvv_memcpy(rbits.byte, (void *)drbg_regs->rbits, blocklen);
        for (uint32_t i = 0; i < (blocklen / 4); i++)
            // rbits.word[i] = be2le(rbits.word[i]);
            rbits.word[i] = be2le(drbg_regs->rbits[i]);
        // put into the out buffer
        uint32_t pick = outlen - i;
        if (pick > blocklen)
            pick = blocklen;
        rvv_memcpy(out + i, rbits.byte, pick);
        i += pick;
    }

    // post-generation
    if (df)
        check = pufs_drbg_update((algo << 0 | 4 << 8 | 1 << 24), dfbuf, dflen);
    else
        check = pufs_drbg_update((algo << 0 | 4 << 8 | 0 << 24), adin, adinlen);

    return check;
}
/**
 * pufs_drbg_uninstantiate()
 */
pufs_status_t pufs_drbg_uninstantiate(void)
{
    uint32_t val32, algo;
    // abort if DRBG is not instantiated
    val32 = drbg_regs->status_1;
    if ((val32 & SP90A_STATUS_1_INSTANT_MASK) == 0)
        return E_INVALID;
    algo = (val32 & SP90A_STATUS_1_KEY_LEN_MASK) >> 8;

    // uninstantiate DRBG
    return pufs_drbg_update((algo << 0 | 5 << 8 | 0 << 24), NULL, 0);
}
