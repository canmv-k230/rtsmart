/**
 * @file      pufs_sm2.c
 * @brief     PUFsecurity SM2 API implementation
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
#include <string.h>
#include "pufs_internal.h"
#include "pufs_kdf_internal.h"
#include "pufs_dma_internal.h"
#include "pufs_sm2_internal.h"
#include "pufs_ecp_internal.h"
#include "pufs_hmac_internal.h"
#include "pufs_ecp_regs.h"

/*****************************************************************************
 * Static functions
 ****************************************************************************/
static int get_from(void* dst, void* src, size_t size)
{
    if (!dst || !src || !size)
        return 0;

    rvv_memcpy(dst, src, size);

    return size;
}

static int put_to(void* dst, void* src, size_t size)
{
    if (!dst || !src || !size)
        return 0;

    rvv_memcpy(dst, src, size);

    return size;
}

// KDF function for key exchange
static pufs_status_t pufs_sm2_kex_kdf(uint8_t* key,
    uint32_t keybits,
    pufs_ec_point_st* UxUy,
    pufs_dgst_st* za,
    pufs_dgst_st* zb)
{
    pufs_status_t status;
    pufs_dgst_st md;
    uint32_t counter = 1, temp;
    uint32_t keylen = keybits / 8;
    uint32_t offset = 0;
    uint32_t length = (SM2_EC_FIELD_LENGTH * 4) + 4; // 4 bytes for counter block

    uint32_t i, j;

    rvv_memset(pufs_buffer, 0x0, length);

    rvv_memcpy(pufs_buffer, UxUy->x, UxUy->qlen);
    offset += UxUy->qlen;
    rvv_memcpy(pufs_buffer + offset, UxUy->y, UxUy->qlen);
    offset += UxUy->qlen;
    rvv_memcpy(pufs_buffer + offset, za->dgst, za->dlen);
    offset += za->dlen;
    rvv_memcpy(pufs_buffer + offset, zb->dgst, zb->dlen);
    offset += zb->dlen;

    // The length of digest is 32 bytes.
    for (i = 0, j = (keylen + 31) / 32; i < j; i++) {
        temp = le2be(counter);
        rvv_memcpy(pufs_buffer + offset, &temp, 4);

        if ((status = pufs_hash(&md, pufs_buffer, length, SM3)) != SUCCESS)
            return status;

        if ((i + 1 == j) && (keylen % md.dlen != 0)) {
            put_to(key, md.dgst, keylen % md.dlen);
        } else {
            put_to(key, md.dgst, md.dlen);
            key += md.dlen;
        }
        counter++;
    }

    return SUCCESS;
}

static pufs_status_t pufs_sm2_kex_hash(pufs_dgst_st* s2,
    pufs_dgst_st* s3,
    pufs_ec_point_st* key,
    pufs_dgst_st* za,
    pufs_dgst_st* zb,
    pufs_ec_point_st* tpukl,
    pufs_ec_point_st* tpukr)
{
    pufs_dgst_st partial;
    pufs_status_t status;
    uint32_t length;

    uint32_t offset = 0;

    // step1. SM3_HASH(Ux | Za | Zb | x1 | y1 | x2 | y2)
    length = SM2_EC_FIELD_LENGTH * 7;

    rvv_memset(pufs_buffer, 0x0, length);

    rvv_memcpy(pufs_buffer, key->x, key->qlen);
    offset += key->qlen;
    rvv_memcpy(pufs_buffer + offset, za->dgst, za->dlen);
    offset += za->dlen;
    rvv_memcpy(pufs_buffer + offset, zb->dgst, zb->dlen);
    offset += zb->dlen;
    rvv_memcpy(pufs_buffer + offset, tpukl->x, tpukl->qlen);
    offset += tpukl->qlen;
    rvv_memcpy(pufs_buffer + offset, tpukl->y, tpukl->qlen);
    offset += tpukl->qlen;
    rvv_memcpy(pufs_buffer + offset, tpukr->x, tpukr->qlen);
    offset += tpukr->qlen;
    rvv_memcpy(pufs_buffer + offset, tpukr->y, tpukr->qlen);

    if ((status = pufs_hash(&partial, pufs_buffer, length, SM3)) != SUCCESS)
        return status;

    // step2. SM3_HASH({0x2,0x3}| Uy | dgst from step1)
    length = partial.dlen + SM2_EC_FIELD_LENGTH + 1;
    rvv_memset(pufs_buffer, 0x0, length);
    pufs_buffer[0] = 0x2;

    rvv_memcpy(pufs_buffer + 1, key->y, key->qlen);
    rvv_memcpy(pufs_buffer + 1 + key->qlen, partial.dgst, partial.dlen);

    if ((status = pufs_hash(s2, pufs_buffer, length, SM3)) != SUCCESS)
        return status;

    pufs_buffer[0] = 0x3;

    return pufs_hash(s3, pufs_buffer, length, SM3);
}

// ZA = H256(ENTLA || id || a || b || Gx || Gy || PUKx || PUKy)
static pufs_status_t pufs_sm2_gen_z(pufs_dgst_st* md,
    const uint8_t* id,
    uint32_t idlen,
    pufs_ec_point_st* puk)
{
    uint8_t n;
    pufs_status_t status;
    pufs_hash_ctx* hash_ctx;
    pufs_ecc_param_st sm2_params = ecc_param[SM2];

    // the length of ENTLA is 2 bytes = 2 ^ 16 bits number,
    // so the max length of idlen is 2 ^ 13 bytes
    if (idlen >= 8192)
        return E_INVALID;

    uint16_t bits = idlen * 8;

    hash_ctx = pufs_hash_ctx_new();

    if ((status = pufs_hash_init(hash_ctx, SM3)) != SUCCESS)
        goto release;

    n = (uint8_t)((bits >> 8) & 0xFF);
    if ((status = pufs_hash_update(hash_ctx, &n, 1)) != SUCCESS)
        goto release;

    n = (uint8_t)(bits & 0xFF);
    if ((status = pufs_hash_update(hash_ctx, &n, 1)) != SUCCESS)
        goto release;

    if (idlen > 0 && (status = pufs_hash_update(hash_ctx, id, idlen)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, sm2_params.a, sm2_params.len)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, sm2_params.b, sm2_params.len)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, sm2_params.px, sm2_params.len)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, sm2_params.py, sm2_params.len)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, puk->x, puk->qlen)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, puk->y, puk->qlen)) != SUCCESS)
        goto release;

    status = pufs_hash_final(hash_ctx, md);

release:
    pufs_hash_ctx_free(hash_ctx);
    if (status != SUCCESS)
        LOG_ERROR("%s", pufs_strstatus(status));

    return status;
}

// generate C3 part of encryption or decryption process using SM3 hash
static pufs_status_t pufs_sm2_encdec_gen_c3(pufs_dgst_st* md,
    pufs_ec_point_st* x2y2,
    const uint8_t* msg,
    uint32_t msg_len)
{
    pufs_status_t status;
    pufs_hash_ctx* hash_ctx;

    hash_ctx = pufs_hash_ctx_new();

    if ((status = pufs_hash_init(hash_ctx, SM3)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, x2y2->x, x2y2->qlen)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, msg, msg_len)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, x2y2->y, x2y2->qlen)) != SUCCESS)
        goto release;

    status = pufs_hash_final(hash_ctx, md);

release:
    pufs_hash_ctx_free(hash_ctx);
    if (status != SUCCESS)
        LOG_ERROR("%s", pufs_strstatus(status));

    return status;
}

// compute SM3_HASH(Za | M)
static pufs_status_t pufs_sm2_sign_m_hash(pufs_dgst_st* md,
    pufs_dgst_st* za,
    const uint8_t* msg,
    uint32_t msg_len)
{
    pufs_status_t status;
    pufs_hash_ctx* hash_ctx;

    hash_ctx = pufs_hash_ctx_new();

    if ((status = pufs_hash_init(hash_ctx, SM3)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, za->dgst, za->dlen)) != SUCCESS)
        goto release;

    if ((status = pufs_hash_update(hash_ctx, msg, msg_len)) != SUCCESS)
        goto release;

    status = pufs_hash_final(hash_ctx, md);

release:
    pufs_hash_ctx_free(hash_ctx);
    if (status != SUCCESS)
        LOG_ERROR("%s", pufs_strstatus(status));

    return status;
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
/**
 * The format of encryption result is 0x4|C1|C3|C2
 */
pufs_status_t _pufs_sm2_enc(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_ec_point_st puk,
    pufs_sm2_format_t format,
    const uint8_t* k)
{
    // The random number k is auto-generated from hardware.
    // We can't import the k number for EC point computing process in current version.
    UNUSED(k);

    uint8_t tmp;
    uint32_t bits;
    pufs_status_t status;
    pufs_ec_point_st x1y1, x2y2;
    pufs_dgst_st md;

    if ((status = pufs_ecp_set_curve_byname(SM2)) != SUCCESS)
        return status;

    // Using the PKC hardware module to compute (x1, y1) and (x2, y2) EC points.
    // The PKC module will output (x1, y1) and (x2, y2) points, and (x2||y2) will be stored into SHARESEC slot - SHARESEC_0.
    if ((status = pufs_ecp_sm2_enc_oss(&puk, &x1y1, &x2y2)) != SUCCESS) {
        LOG_ERROR("%s", pufs_strstatus(status));
        return status;
    }
    if ((status = pufs_sm2_encdec_gen_c3(&md, &x2y2, in, inlen)) != SUCCESS) {
        LOG_ERROR("%s", pufs_strstatus(status));
        return status;
    }

    tmp = 0x4;
    put_to(out, &tmp, 1);
    put_to(out + 1, x1y1.x, x1y1.qlen);
    put_to(out + 1 + x1y1.qlen, x1y1.y, x1y1.qlen);
    out += (1 + SM2_C1_LENGTH);

    if (format == SM2_C1C3C2)
        out += SM2_C3_LENGTH;

    // The KDF hardware module with SM2 method will compute C2 output.
    // The input data firmware provided are (x2||y2) which is stored in SHARESEC_0 slot and plaintext message.
    bits = B2b(inlen) > 128 ? 128 : B2b(inlen);
    if ((status = pufs_sm2kdf(SSKEY, SK128_0, bits, SHARESEC, SHARESEC_0, B2b(SM2_X2Y2_LENGTH), in, inlen, out)) != SUCCESS) {
        LOG_ERROR("%s", pufs_strstatus(status));
        return status;
    }

    *outlen = inlen + 1 + SM2_C1_LENGTH + SM2_C3_LENGTH;
    dma_read_output(out, inlen);

    if (format == SM2_C1C3C2)
        out -= SM2_C3_LENGTH;
    else
        out += inlen;

    put_to(out, md.dgst, md.dlen);

    return SUCCESS;
}
/**
 *
 */
pufs_status_t pufs_sm2_dec(uint8_t* out,
    uint32_t* outlen,
    const uint8_t* in,
    uint32_t inlen,
    pufs_ka_slot_t prk,
    pufs_sm2_format_t format)
{
    pufs_dgst_st md, md_tmp;
    pufs_status_t status;
    pufs_ec_point_st x1y1, x2y2;
    uint32_t bits;

    // the format of data is 0x4|C1|C3|C2
    // so the length of original message is encrypted length - 1 - C1 - C3;
    int32_t msg_len = inlen - 1 - SM2_C1_LENGTH - SM2_C3_LENGTH;
    uint8_t* data = (uint8_t*)in + 1;

    if (msg_len < 0)
        return E_INVALID;

    x1y1.qlen = SM2_EC_FIELD_LENGTH;
    get_from(x1y1.x, data, SM2_EC_FIELD_LENGTH);
    get_from(x1y1.y, data + SM2_EC_FIELD_LENGTH, SM2_EC_FIELD_LENGTH);

    if ((status = pufs_ecp_set_curve_byname(SM2)) != SUCCESS) {
        LOG_ERROR("%s", pufs_strstatus(status));
        return status;
    }

    if ((status = pufs_ecp_sm2_dec_oss(prk, &x1y1, &x2y2)) != SUCCESS) {
        LOG_ERROR("%s", pufs_strstatus(status));
        return status;
    }

    bits = B2b(msg_len) > 128 ? 128 : B2b(msg_len);
    if ((status = pufs_sm2kdf(SSKEY,
             SK128_0,
             bits,
             SHARESEC,
             SHARESEC_0,
             B2b(SM2_X2Y2_LENGTH),
             (format == SM2_C1C3C2) ? data + SM2_C1_LENGTH + SM2_C3_LENGTH : data + SM2_C1_LENGTH,
             msg_len, out))
        != SUCCESS) {
        LOG_ERROR("%s", pufs_strstatus(status));
        return status;
    }
    dma_read_output(out, msg_len);
    *outlen = msg_len;

    if ((status = pufs_sm2_encdec_gen_c3(&md, &x2y2, out, msg_len)) != SUCCESS)
        return status;

    get_from(md_tmp.dgst, (format == SM2_C1C3C2 ? data + SM2_C1_LENGTH : data + SM2_C1_LENGTH + msg_len), md.dlen);
    if (memcmp(md.dgst, md_tmp.dgst, md.dlen) != 0)
        return E_VERFAIL;

    return SUCCESS;
}
/**
 *
 */
pufs_status_t pufs_sm2_kex(pufs_dgst_st* s2,
    pufs_dgst_st* s3,
    uint8_t* key,
    uint32_t keybits,
    const uint8_t* idl,
    uint32_t idllen,
    const uint8_t* idr,
    uint32_t idrlen,
    pufs_ka_slot_t prkslotl,
    pufs_ka_slot_t tprkslotl,
    pufs_ec_point_st pukr,
    pufs_ec_point_st tpukr,
    bool init)
{
    pufs_status_t status;
    pufs_dgst_st za, zb;
    pufs_ec_point_st pukl, tpukl, UxUy, *pza = &pukr, *pzb = &pukr;

    if ((status = pufs_ecp_set_curve_byname(SM2)) != SUCCESS || (status = pufs_ecp_gen_puk(&pukl, PRKEY, prkslotl)) != SUCCESS || (status = pufs_ecp_gen_puk(&tpukl, PRKEY, tprkslotl)) != SUCCESS || (status = pufs_ecp_sm2_kekdf(&UxUy, &tpukl, &tpukr, &pukr, tprkslotl, prkslotl)) != SUCCESS) {
        LOG_ERROR("generate point: %s", pufs_strstatus(status));
        return status;
    }

    if (init)
        pza = &pukl;
    else
        pzb = &pukl;

    if ((status = pufs_sm2_gen_z(&za, idl, idllen, pza)) != SUCCESS || (status = pufs_sm2_gen_z(&zb, idr, idrlen, pzb)) != SUCCESS || (status = pufs_sm2_kex_kdf(key, keybits, &UxUy, &za, &zb)) != SUCCESS) {
        LOG_ERROR("generate secret: %s", pufs_strstatus(status));
        return status;
    }

    if (init)
        status = pufs_sm2_kex_hash(s2, s3, &UxUy, &za, &zb, &tpukl, &tpukr);
    else
        status = pufs_sm2_kex_hash(s2, s3, &UxUy, &za, &zb, &tpukr, &tpukl);

    if (status != SUCCESS)
        LOG_ERROR("hash secret: %s", pufs_strstatus(status));

    return status;
}
/**
 * pufs_sm2_verify
 */
pufs_status_t pufs_sm2_verify(pufs_ecdsa_sig_st sig,
    const uint8_t* msg,
    uint32_t msglen,
    const uint8_t* id,
    uint32_t idlen,
    pufs_ec_point_st puk)
{
    pufs_status_t status;
    pufs_dgst_st za, md;

    if ((status = pufs_ecp_set_curve_byname(SM2)) != SUCCESS || (status = pufs_sm2_gen_z(&za, id, idlen, &puk)) != SUCCESS || (status = pufs_sm2_sign_m_hash(&md, &za, msg, msglen)) != SUCCESS || (status = pufs_ecp_sm2_verify_dgst(&sig, &md, &puk)) != SUCCESS) {
        LOG_ERROR("%s", pufs_strstatus(status));
    }
    return status;
}
/**
 * _pufs_sm2_sign
 */
pufs_status_t _pufs_sm2_sign(pufs_ecdsa_sig_st* sig,
    const uint8_t* msg,
    uint32_t msglen,
    const uint8_t* id,
    uint32_t idlen,
    pufs_key_type_t prktype,
    uint32_t prkslot,
    const uint8_t* k)
{
    // The random number k is auto-generated from hardware.
    // We can't import the k number for EC point computing process in current version.
    UNUSED(k);

    pufs_status_t status;
    pufs_ec_point_st puk;
    pufs_dgst_st za, md;

    if ((status = pufs_ecp_set_curve_byname(SM2)) != SUCCESS || (status = pufs_ecp_gen_puk(&puk, prktype, prkslot)) != SUCCESS || (status = pufs_sm2_gen_z(&za, id, idlen, &puk)) != SUCCESS || (status = pufs_sm2_sign_m_hash(&md, &za, msg, msglen)) != SUCCESS || (status = pufs_ecp_sm2_sign_dgst(sig, &md, prktype, prkslot)) != SUCCESS) {
        LOG_ERROR("%s", pufs_strstatus(status));
    }
    return status;
}
