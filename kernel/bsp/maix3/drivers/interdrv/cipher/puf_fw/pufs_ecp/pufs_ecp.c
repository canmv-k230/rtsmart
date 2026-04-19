/**
 * @file      pufs_ecp.c
 * @brief     PUFsecurity ECP API implementation
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
#include "pufs_ecp_internal.h"
#include "pufs_ecp_mprog.h"
#include "pufs_ka_internal.h"
#include "pufs_kdf_internal.h"
#include "pufs_ecc_internal.h"

struct pufs_ecp_regs* ecp_regs = NULL;

/*****************************************************************************
 * Macros
 ****************************************************************************/
#define MAXELEN 72
#define ECP_MPMAC_SIZE 16
#define ECP_MPROG_SIZE 256

/*****************************************************************************
 * Static variables
 ****************************************************************************/

static pufs_ecp_version_t ecp_version = ECP_UNSUPPORTED;
static pufs_mp_version_t mp_version = MP_UNSUPPORTED;

struct ecdp_setting {
    pufs_ecp_mprog_curve_st* mprog;
    pufs_ecp_mprog_cmac_st* mpmac;
    pufs_ec_name_t name;
    bool isset;
} ecdp_set;
static const uint8_t rsa_pss_pad1[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

/*****************************************************************************
 * Static functions
 ****************************************************************************/

// 定义一个函数，用于替换memcpy()
static inline void* memcpy4b(void* dest, const void* src, int count)
{
    uint32_t *tmp = (uint32_t*)dest, *s = (uint32_t*)src;
    int c = count / 4;
    while (c--)
        *tmp++ = *s++;

    return dest;
}

static void _write_ecp_operand(uint32_t pos, const uint8_t* op, uint32_t elen)
{
    uint32_t wlen = ((elen + 3) / 4) * 4;
    rvv_memset(pufs_buffer, 0, wlen);
    if (op != NULL)
        reverse(pufs_buffer, op, elen);
    memcpy4b((uint8_t*)ecp_regs->data + wlen * pos, pufs_buffer, wlen);
}
/**
 * @brief Reset the target oeprand to zero
 *
 * @param pos The operand position of the operand to be written in SRAM
 * @param elen The length in bytes of the operand stored in SRAM
 */
static void reset_ecp_operand(uint32_t pos, uint32_t elen)
{
    _write_ecp_operand(pos, NULL, elen);
}
/**
 * @brief Write oeprands into SRAM
 *
 * @param pos The operand position of the operand to be written in SRAM
 * @param op The operand
 * @param elen The length in bytes of the operand stored in SRAM
 */
static void write_ecp_operand(uint32_t pos, const uint8_t* op, uint32_t elen)
{
    _write_ecp_operand(pos, op, elen);
}
/**
 * @brief Wrapper function of _read_ecp_operand() to set false as the
 *        default value of the last parameter if not provided.
 */
#define read_ecp_operand(pos, res, ...) \
    _read_ecp_operand(pos, res, DEF_ARG(__VA_ARGS__, false))
/**
 * @brief Read resulting operands from SRAM
 *
 * @param pos       The result position of the operand to be read from SRAM
 * @param res       The pointer to the space for the resulting operand
 * @param elen      The length in bytes of the resulting operand stored in SRAM
 * @prarm oss_2e2s  Indication of 2e2s_oss read.
 */
static void read_ecp_operand(uint32_t pos, uint8_t* res, uint32_t elen,
    bool oss_2e2s)
{
    uint32_t wlen = ((elen + 3) / 4) * 4;
    uint32_t rlen = wlen;
    if (oss_2e2s)
        rlen = ((elen * 2 + 3) / 4) * 4;

    memcpy4b(pufs_buffer, (uint8_t*)ecp_regs->data + wlen * pos, rlen);
    reverse(res, pufs_buffer, (oss_2e2s ? (elen * 2) : elen));
}
/**
 * @brief Starting ECP and wait until done
 *
 * @return ECCA status register value
 */
static uint32_t pufs_ecc_start(void)
{
    uint32_t ret;
    ecp_regs->ctrl = 0x01;
    do {
        ret = ecp_regs->status;
    } while ((ret & ECP_STATUS_BUSY_MASK) != 0);

    return ret;
}
/**
 * @brief Get ECP version enum based on version number
 *
 * @return ECP version enum
 */
static pufs_ecp_version_t get_ecp_version(uint32_t ecp_ver)
{
    switch (ecp_ver) {
    case 0xecf39303:
        return ECP_ECF39303;
    case 0xecf09303:
        return ECP_ECF09303;
    default:
        return ECP_UNSUPPORTED;
    }
}
/**
 * @brief Get MP version enum based on version number
 *
 * @return ECP version enum
 */
static pufs_mp_version_t get_mp_version(uint32_t mp_ver)
{
    switch (mp_ver) {
    case 0x00000000:
        return MP_00000000;
    case 0x3ee2de76:
        return MP_3EE2DE76;
    default:
        return MP_UNSUPPORTED;
    }
}
/**
 * @brief Initialize RSA field and elen
 *
 * @param[out] field    RSA field.
 * @param[out] elen     RSA element length.
 * @param[in]  rsatype  RSA type.
 * @return              SUCCESS on success, otherwise an error code.
 */
static pufs_status_t pufs_rsa_get_field_elen(pufs_ecp_field_t* field,
    uint32_t* elen,
    pufs_rsa_type_t rsatype)
{
    pufs_ecp_field_t rf;
    uint32_t rl;

    switch (rsatype) {
    case RSA1024:
        rf = P1024;
        rl = 128;
        break;
    case RSA2048:
        rf = P2048;
        rl = 256;
        break;
    case RSA3072:
        rf = P3072;
        rl = 384;
        break;
    case RSA4096:
        rf = P4096;
        rl = 512;
        break;
    default:
        return E_INVALID;
    }

    if (field != NULL)
        *field = rf;
    if (elen != NULL)
        *elen = rl;
    return SUCCESS;
}
/**
 * @brief Bignum comparison of a and b.
 *
 * @param[in]  a    a.
 * @param[in]  b    b.
 * @param[in]  len  a, b length in bytes.
 * @return          An integral value which is greater than, equal to, or less
 *                   then 0 representing a > b, a = b, or a < b.
 */
static int16_t pufs_bn_cmp(const uint8_t* a, const uint8_t* b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (a[i] == b[i])
            continue;
        return ((int16_t)(a[i]) - (int16_t)(b[i]));
    }
    return 0;
}
/**
 * @brief Calculate RSA exponentiation with public key
 *
 * @param[out] msg      Message.
 * @param[in]  sig      RSA signature.
 * @param[in]  rsatype  RSA type.
 * @param[in]  n        RSA parameter n.
 * @param[in]  puk      RSA public key.
 * @return              SUCCESS on success, otherwise an error code.
 */
static pufs_status_t pufs_rsa_verify_calc(uint8_t* msg,
    const uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk)
{
    uint32_t elen = 0, val32 = 0;
    pufs_ecp_field_t field;
    pufs_status_t check;

    if (sig == NULL || n == NULL || puk == 0)
        return E_INVALID;

    if ((check = pufs_rsa_get_field_elen(&field, &elen, rsatype)) != SUCCESS)
        return check;

    if (pufs_bn_cmp(sig, n, elen) >= 0)
        return E_INVALID;

    val32 = field << ECP_ECP_EC_FIELD_BITS;

    ecp_regs->ec = val32;
    ecp_regs->e_short = puk;

    write_ecp_operand(0, n, elen);
    write_ecp_operand(2, sig, elen);

    pufs_rsa_mprog_st* prog = &(rsa_mprog[ecp_version][rsatype]);
    pufs_rsa_mprog_cmac_st* cmac = prog->cmac[mp_version];

    memcpy4b((void*)ecp_regs->mac, cmac->puk, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, prog->func->puk, ECP_MPROG_SIZE);

    val32 = pufs_ecc_start();

    if (val32 & ECP_STATUS_MPROG_MASK)
        return E_ECMPROG;

    if (val32)
        return E_VERFAIL;

    rvv_memset(msg, 0, BUFFER_SIZE);
    read_ecp_operand(2, msg, elen);

    return SUCCESS;
}
/**
 * @brief Bignum subtraction b = n - a. (n > a)
 *
 * @param[out] b    The result b.
 * @param[in]  n    n.
 * @param[in]  a    a.
 * @param[in]  len  n, a, b length in bytes.
 * @return              SUCCESS on success, otherwise an error code.
 */
static pufs_status_t pufs_bn_sub(uint8_t* b,
    const uint8_t* n,
    const uint8_t* a,
    uint32_t len)
{
    if (pufs_bn_cmp(n, a, len) < 0)
        return E_INVALID;

    bool borrow = false;
    for (int32_t i = len - 1; i >= 0; i--) {
        int16_t nn = n[i];
        int16_t aa = a[i];
        if (borrow)
            aa++;

        if (nn >= aa) {
            b[i] = (uint8_t)(nn - aa);
            borrow = false;
        } else {
            b[i] = (uint8_t)(nn + 0x100 - aa);
            borrow = true;
        }
    }
    return SUCCESS;
}
/**
 * @brief Mask generation function used in RSA-PSS.
 *
 * @param[out] mask     Generated mask.
 * @param[in]  masklen  Generated mask length in bytes.
 * @param[in]  hash     Hash algorithm.
 * @param[in]  seed     Seed.
 * @param[in]  seedlen  Seed length in bytes.
 * @return              SUCCESS on success, otherwise an error code.
 */
static pufs_status_t pufs_rsa_pss_mgf(uint8_t* mask,
    uint32_t masklen,
    pufs_hash_t hash,
    const uint8_t* seed,
    uint32_t seedlen)
{
    union {
        uint32_t beint;
        uint8_t str[4];
    } ctr_rep;
    uint32_t curlen = 0;
    pufs_dgst_st md;
    pufs_status_t check;

    pufs_hash_ctx* hash_ctx = pufs_hash_ctx_new();
    if (hash_ctx == NULL)
        return E_UNAVAIL;

    for (uint32_t ctr = 0; curlen < masklen; ctr++) {
        ctr_rep.beint = be2le(ctr);

        if (((check = pufs_hash_init(hash_ctx, hash)) != SUCCESS) || ((check = pufs_hash_update(hash_ctx, seed, seedlen)) != SUCCESS) || ((check = pufs_hash_update(hash_ctx, ctr_rep.str, 4)) != SUCCESS) || ((check = pufs_hash_final(hash_ctx, &md)) != SUCCESS))
            return check;

        uint32_t picklen = masklen - curlen;
        if (picklen > md.dlen)
            picklen = md.dlen;
        rvv_memcpy(mask + curlen, md.dgst, picklen);
        curlen += picklen;
    }

    pufs_hash_ctx_free(hash_ctx);
    return SUCCESS;
}

static pufs_status_t pufs_ecp_set_curve_params_byname(pufs_ec_name_t name, uint32_t ecp_ver)
{
    uint32_t val32, elen;

    elen = ecc_param[name].len;
    write_ecp_operand(0, ecc_param[name].field, elen);
    write_ecp_operand(1, ecc_param[name].a, elen);
    write_ecp_operand(2, ecc_param[name].b, elen);
    write_ecp_operand(3, ecc_param[name].px, elen);
    write_ecp_operand(4, ecc_param[name].py, elen);
    write_ecp_operand(5, ecc_param[name].order, elen);

    switch (ecp_ver) {
    case 0xecf31301:
    case 0xecf31381:
        val32 = ecc_param[name].h << 12 | ecc_param[name].pf << 11 | ecc_param[name].ftype << 8;
        break;
    case 0xecf39303:
    case 0xecf09303:
        val32 = ecc_param[name].h << 16 | ecc_param[name].pf << 15 | ecc_param[name].ftype << 8;
        break;
    default:
        return E_UNSUPPORT;
    }
    ecp_regs->ec = val32;

    return SUCCESS;
}

/**
 * pufs_ecp_set_sm2_curve()
 */
pufs_status_t pufs_ecp_set_sm2_curve(void)
{
    ecdp_set.name = SM2;
    ecdp_set.isset = true;

    return pufs_ecp_set_curve_params_byname(SM2, ecp_regs->version);
}

pufs_status_t pufs_ecp_sm2_enc_oss(pufs_ec_point_st* puk, pufs_ec_point_st* x1y1, pufs_ec_point_st* x2y2)
{
    uint32_t val32;
    uint32_t elen = ecc_param[SM2].len;

    write_ecp_operand(7, puk->x, elen);
    write_ecp_operand(8, puk->y, elen);

    val32 = (get_key_slot_idx(SHARESEC, SHARESEC_0) << 1) << ECP_ECP_KEYSEL_DST_BITS;
    ecp_regs->keysel = val32;

    pufs_sm2_mprog_st* prog = &(sm2_mprog[ecp_version]);
    pufs_sm2_mprog_cmac_st* cmac = prog->cmac[mp_version];

    memcpy4b((void*)ecp_regs->mac, cmac->enc_oss, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, prog->func->enc_oss, ECP_MPROG_SIZE);

    val32 = pufs_ecc_start();

    if (val32 & ECP_STATUS_MPROG_MASK)
        return E_ECMPROG;

    if (val32)
        return E_VERFAIL;

    x1y1->qlen = x2y2->qlen = elen;

    read_ecp_operand(7, x2y2->x, elen);
    read_ecp_operand(8, x2y2->y, elen);
    read_ecp_operand(9, x1y1->x, elen);
    read_ecp_operand(10, x1y1->y, elen);

    return SUCCESS;
}

pufs_status_t pufs_ecp_sm2_dec_oss(pufs_ka_slot_t prkslot, pufs_ec_point_st* x1y1, pufs_ec_point_st* x2y2)
{
    uint32_t val32;
    uint32_t elen = ecc_param[SM2].len;

    write_ecp_operand(7, x1y1->x, elen);
    write_ecp_operand(8, x1y1->y, elen);

    val32 = (get_key_slot_idx(SHARESEC, SHARESEC_0) << 1) << ECP_ECP_KEYSEL_DST_BITS;
    val32 |= (get_key_slot_idx(PRKEY, prkslot) << 1) << 0;
    ecp_regs->keysel = val32;

    pufs_sm2_mprog_st* prog = &(sm2_mprog[ecp_version]);
    pufs_sm2_mprog_cmac_st* cmac = prog->cmac[mp_version];

    memcpy4b((void*)ecp_regs->mac, cmac->dec_oss, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, prog->func->dec_oss, ECP_MPROG_SIZE);

    val32 = pufs_ecc_start();

    if (val32 & ECP_STATUS_MPROG_MASK)
        return E_ECMPROG;

    if (val32)
        return E_VERFAIL;

    x2y2->qlen = elen;

    read_ecp_operand(7, x2y2->x, elen);
    read_ecp_operand(8, x2y2->y, elen);

    return SUCCESS;
}

pufs_status_t pufs_ecp_sm2_sign_dgst(pufs_ecdsa_sig_st* sig,
    pufs_dgst_st* md,
    pufs_key_type_t prktype,
    pufs_ka_slot_t prkslot)
{
    pufs_status_t check;
    uint32_t val32;

    if (ecdp_set.isset == false)
        return E_INVALID;

    if ((prktype != PRKEY) && (prktype != OTPKEY))
        return E_INVALID;

    if ((check = keyslot_check(true, prktype, prkslot, ecc_param[ecdp_set.name].fbits)) != SUCCESS)
        return check;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if (prktype == PRKEY) {
        val32 = (get_key_slot_idx(prktype, prkslot) << 1) << 0;
        ecp_regs->keysel = val32;
    } else // OTPKEY
    {
        val32 = 0x1 << 0;
        ecp_regs->keysel = val32;
        val32 = get_key_slot_idx(prktype, prkslot) << 4;
        ecp_regs->otpkba = val32;
    }

    pufs_sm2_mprog_st* prog = &(sm2_mprog[ecp_version]);
    pufs_sm2_mprog_cmac_st* cmac = prog->cmac[mp_version];

    memcpy4b((void*)ecp_regs->mac, cmac->sign, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, prog->func->sign, ECP_MPROG_SIZE);

    write_ecp_operand(6, md->dgst, md->dlen);

    val32 = pufs_ecc_start();

    if (val32 & ECP_STATUS_MPROG_MASK)
        return E_ECMPROG;

    if (val32)
        return E_VERFAIL;

    sig->qlen = ecc_param[ecdp_set.name].len;
    read_ecp_operand(9, sig->r, sig->qlen);
    read_ecp_operand(10, sig->s, sig->qlen);

    return SUCCESS;
}

pufs_status_t pufs_ecp_sm2_verify_dgst(pufs_ecdsa_sig_st* sig,
    pufs_dgst_st* md,
    pufs_ec_point_st* puk)
{
    uint32_t val32, elen;

    if (ecdp_set.isset == false)
        return E_INVALID;

    elen = ecc_param[ecdp_set.name].len;
    if ((puk->qlen != elen) || (sig->qlen != elen))
        return E_INVALID;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    pufs_sm2_mprog_st* prog = &(sm2_mprog[ecp_version]);
    pufs_sm2_mprog_cmac_st* cmac = prog->cmac[mp_version];

    memcpy4b((void*)ecp_regs->mac, cmac->verify, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, prog->func->verify, ECP_MPROG_SIZE);

    write_ecp_operand(6, md->dgst, md->dlen);
    write_ecp_operand(7, puk->x, elen);
    write_ecp_operand(8, puk->y, elen);
    write_ecp_operand(9, sig->r, elen);
    write_ecp_operand(10, sig->s, elen);

    val32 = pufs_ecc_start();

    if (val32 & ECP_STATUS_MPROG_MASK)
        return E_ECMPROG;

    if (val32)
        return E_VERFAIL;

    return SUCCESS;
}

pufs_status_t pufs_ecp_sm2_kekdf(pufs_ec_point_st* key,
    pufs_ec_point_st* tpukl,
    pufs_ec_point_st* tpukr,
    pufs_ec_point_st* pukr,
    pufs_ka_slot_t tprkslotl,
    pufs_ka_slot_t prkslotl)
{
    uint8_t buf[32];
    uint32_t val32, elen;

    if (ecdp_set.isset == false)
        return E_INVALID;

    elen = ecc_param[ecdp_set.name].len;
    if ((tpukl->qlen != elen) || (tpukr->qlen != elen) || (pukr->qlen != elen))
        return E_INVALID;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    pufs_sm2_mprog_st* prog = &(sm2_mprog[ecp_version]);
    pufs_sm2_mprog_cmac_st* cmac = prog->cmac[mp_version];

    memcpy4b((void*)ecp_regs->mac, cmac->kekdf, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, prog->func->kekdf, ECP_MPROG_SIZE);

    val32 = (get_key_slot_idx(SHARESEC, SHARESEC_0) << 1) << ECP_ECP_KEYSEL_DST_BITS;
    val32 |= (get_key_slot_idx(PRKEY, prkslotl) << 1) << ECP_ECP_KEYSEL_SRC_1_BITS;
    val32 |= (get_key_slot_idx(PRKEY, tprkslotl) << 1) << ECP_ECP_KEYSEL_SRC_2_BITS;
    ecp_regs->keysel = val32;

    // the format of T value is T[255] = 1, T[254:128] = X1[126:0], T[127] = 1, T[126:0] = X2[126:0]
    rvv_memset(buf, 0x0, 32);
    rvv_memcpy(buf, tpukr->x + 16, 16);
    buf[0] |= (1 << 7);
    rvv_memcpy(buf + 16, tpukl->x + 16, 16);
    buf[16] |= (1 << 7);

    write_ecp_operand(6, buf, elen);
    write_ecp_operand(7, tpukr->x, elen);
    write_ecp_operand(8, tpukr->y, elen);
    write_ecp_operand(9, pukr->x, elen);
    write_ecp_operand(10, pukr->y, elen);

    val32 = pufs_ecc_start();

    if (val32 & ECP_STATUS_MPROG_MASK)
        return E_ECMPROG;

    if (val32)
        return E_VERFAIL;

    key->qlen = elen;
    read_ecp_operand(9, key->y, elen);
    read_ecp_operand(10, key->x, elen);

    return SUCCESS;
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_pkc_module_init(uintptr_t pkc_offset)
{
    ecp_regs = (struct pufs_ecp_regs*)(pufs_context.base_addr + pkc_offset);
    // TODO: version check
    // there are two versions of PKC module: one is DPA, the other is w/o DPA.
    // so we may need to define the version dynamically.
    ecp_version = get_ecp_version(ecp_regs->version);

    if (ecp_version == ECP_UNSUPPORTED)
        err(1, "current ECP version 0x%x is not supported", (unsigned int)ecp_regs->version);

    mp_version = get_mp_version(ecp_regs->mp_version);

    if (mp_version == MP_UNSUPPORTED)
        err(1, "current ECP_MP version 0x%x is not supported", (unsigned int)ecp_regs->mp_version);
}
/**
 * pufs_rsa_verify()
 */
pufs_status_t pufs_rsa_verify(const uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk,
    const uint8_t* msg)
{
    pufs_status_t check;
    uint8_t em[BUFFER_SIZE];
    uint32_t elen;

    if ((check = pufs_rsa_get_field_elen(NULL, &elen, rsatype)) != SUCCESS)
        return check;
    if ((check = pufs_rsa_verify_calc(em, sig, rsatype, n, puk)) != SUCCESS)
        return check;

    return (memcmp(em, msg, elen) == 0) ? SUCCESS : E_VERFAIL;
}
/**
 * _pufs_rsa_sign()
 */
pufs_status_t _pufs_rsa_sign(uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk,
    const uint8_t* prk,
    const uint8_t* msg,
    const uint8_t* phi)
{
    uint32_t elen = 0, val32 = 0;
    pufs_ecp_field_t field;
    pufs_status_t check;

    if (sig == NULL || n == NULL || puk == 0 || prk == NULL || msg == NULL)
        return E_INVALID;

    if ((check = pufs_rsa_get_field_elen(&field, &elen, rsatype)) != SUCCESS)
        return check;

    if (pufs_bn_cmp(msg, n, elen) >= 0)
        return E_INVALID;

    val32 = field << ECP_ECP_EC_FIELD_BITS;

    ecp_regs->ec = val32;
    ecp_regs->e_short = puk;

    write_ecp_operand(0, n, elen);

    if (phi == NULL)
        reset_ecp_operand(1, elen);
    else
        write_ecp_operand(1, phi, elen);

    write_ecp_operand(2, msg, elen);
    write_ecp_operand(3, prk, elen);

    pufs_rsa_mprog_st* prog = &(rsa_mprog[ecp_version][rsatype]);
    pufs_rsa_mprog_cmac_st* cmac = prog->cmac[mp_version];

    // rvv_memcpy((void *)ecp_regs->mac, cmac->prk, ECP_MPMAC_SIZE);
    //  for(int j=0; j<ECP_MPMAC_SIZE/4; j++)
    //  {
    //      ecp_regs->mac[j] = *((uint32_t *)cmac->prk + j);
    //  }
    //  rvv_memcpy((void *)ecp_regs->program, prog->func->prk, ECP_MPROG_SIZE);
    memcpy4b((void*)ecp_regs->mac, cmac->prk, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, prog->func->prk, ECP_MPROG_SIZE);

    val32 = pufs_ecc_start();

    if (val32 & ECP_STATUS_MPROG_MASK)
        return E_ECMPROG;

    if (val32)
        return E_VERFAIL;

    read_ecp_operand(2, sig, elen);

    return SUCCESS;
}
/**
 * pufs_rsa_x931_verify()
 */
pufs_status_t pufs_rsa_x931_verify(const uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk,
    const uint8_t* msg,
    uint32_t msglen)
{
    pufs_status_t check;
    uint8_t em[BUFFER_SIZE];
    uint32_t elen, i;
    pufs_hash_t hash;
    pufs_dgst_st md;

    if ((check = pufs_rsa_get_field_elen(NULL, &elen, rsatype)) != SUCCESS)
        return check;
    if ((check = pufs_rsa_verify_calc(em, sig, rsatype, n, puk)) != SUCCESS)
        return check;

    if (((em[elen - 1] & 0x0f) != 0x0c) && ((check = pufs_bn_sub(em, n, em, elen)) != SUCCESS))
        return check;

    if (em[elen - 1] != 0xcc)
        return E_VERFAIL;
    switch (em[elen - 2]) {
    case 0x38:
        hash = SHA_224;
        break;
    case 0x34:
        hash = SHA_256;
        break;
    case 0x36:
        hash = SHA_384;
        break;
    case 0x35:
        hash = SHA_512;
        break;
    case 0x39:
        hash = SHA_512_224;
        break;
    case 0x3a:
    case 0x40:
        hash = SHA_512_256;
        break;
    default:
        return E_INVALID;
    }

    if (em[0] != 0x6b)
        return E_VERFAIL;
    for (i = 1; i < elen; i++)
        if (em[i] != 0xbb)
            break;
    if (em[i] != 0xba)
        return E_VERFAIL;
    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;
    if ((i + md.dlen + 3) != elen)
        return E_VERFAIL;
    if (memcmp(em + elen - 2 - md.dlen, md.dgst, md.dlen) != 0)
        return E_VERFAIL;

    return SUCCESS;
}
/**
 * _pufs_rsa_x931_sign()
 */
pufs_status_t _pufs_rsa_x931_sign(uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk,
    const uint8_t* prk,
    pufs_hash_t hash,
    const uint8_t* msg,
    uint32_t msglen,
    const uint8_t* phi)
{
    pufs_status_t check;
    uint8_t em[BUFFER_SIZE];
    uint32_t elen;
    pufs_dgst_st md;

    if ((check = pufs_rsa_get_field_elen(NULL, &elen, rsatype)) != SUCCESS)
        return check;
    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;

    em[0] = 0x6b;
    rvv_memset(em + 1, 0xbb, elen - 4 - md.dlen);
    em[elen - 3 - md.dlen] = 0xba;
    rvv_memcpy(em + elen - 2 - md.dlen, md.dgst, md.dlen);

    switch (hash) {
    case SHA_224:
        em[elen - 2] = 0x38;
        break;
    case SHA_256:
        em[elen - 2] = 0x34;
        break;
    case SHA_384:
        em[elen - 2] = 0x36;
        break;
    case SHA_512:
        em[elen - 2] = 0x35;
        break;
    case SHA_512_224:
        em[elen - 2] = 0x39;
        break;
    case SHA_512_256:
        em[elen - 2] = 0x3a;
        break;
    default:
        return E_INVALID;
    }
    em[elen - 1] = 0xcc;

    if ((check = pufs_rsa_sign(sig, rsatype, n,
             puk, prk, em, phi))
        != SUCCESS)
        return check;

    if ((check = pufs_bn_sub(em, n, sig, elen)) != SUCCESS)
        return check;
    if (pufs_bn_cmp(sig, em, elen) > 0)
        rvv_memcpy(sig, em, elen);

    return SUCCESS;
}
/**
 * pufs_rsa_p1v15_verify()
 */
pufs_status_t pufs_rsa_p1v15_verify(const uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk,
    const uint8_t* msg,
    uint32_t msglen)
{
    pufs_status_t check;
    uint8_t em[BUFFER_SIZE];
    uint32_t elen, i;
    pufs_hash_t hash;
    pufs_dgst_st md;
    uint8_t pret[19] = { 0x30, 0, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
        0x01, 0x65, 0x03, 0x04, 0x02, 0, 0x05, 0x00, 0x04, 0 };

    if ((check = pufs_rsa_get_field_elen(NULL, &elen, rsatype)) != SUCCESS)
        return check;
    if ((check = pufs_rsa_verify_calc(em, sig, rsatype, n, puk)) != SUCCESS)
        return check;

    if ((em[0] != 0x00) || (em[1] != 0x01))
        return E_VERFAIL;
    for (i = 2; i < elen; i++)
        if (em[i] != 0xff)
            break;
    if (em[i++] != 0x00)
        return E_VERFAIL;

    switch (em[i + 14]) {
    case 1:
        hash = SHA_256;
        pret[1] = 0x31;
        pret[14] = 0x01;
        pret[18] = 0x20;
        break;
    case 2:
        hash = SHA_384;
        pret[1] = 0x41;
        pret[14] = 0x02;
        pret[18] = 0x30;
        break;
    case 3:
        hash = SHA_512;
        pret[1] = 0x51;
        pret[14] = 0x03;
        pret[18] = 0x40;
        break;
    case 4:
        hash = SHA_224;
        pret[1] = 0x2d;
        pret[14] = 0x04;
        pret[18] = 0x1c;
        break;
    case 5:
        hash = SHA_512_224;
        pret[1] = 0x2d;
        pret[14] = 0x05;
        pret[18] = 0x1c;
        break;
    case 6:
        hash = SHA_512_256;
        pret[1] = 0x31;
        pret[14] = 0x06;
        pret[18] = 0x20;
        break;
    default:
        return E_INVALID;
    }
    if ((memcmp(em + i, pret, 19) != 0) || ((i + 19 + pret[18]) != elen))
        return E_VERFAIL;

    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;
    if (memcmp(em + i + 19, md.dgst, md.dlen) != 0)
        return E_VERFAIL;

    return SUCCESS;
}
/**
 * _pufs_rsa_p1v15_sign()
 */
pufs_status_t _pufs_rsa_p1v15_sign(uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk,
    const uint8_t* prk,
    pufs_hash_t hash,
    const uint8_t* msg,
    uint32_t msglen,
    const uint8_t* phi)
{
    pufs_status_t check;
    uint8_t em[BUFFER_SIZE];
    uint32_t elen;
    pufs_dgst_st md;
    uint8_t pret[19] = { 0x30, 0, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
        0x01, 0x65, 0x03, 0x04, 0x02, 0, 0x05, 0x00, 0x04, 0 };

    if ((check = pufs_rsa_get_field_elen(NULL, &elen, rsatype)) != SUCCESS)
        return check;
    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;

    em[0] = 0x00;
    em[1] = 0x01;
    rvv_memset(em + 2, 0xff, elen - 22 - md.dlen);
    em[elen - 20 - md.dlen] = 0x00;

    switch (hash) {
    case SHA_224:
        pret[1] = 0x2d;
        pret[14] = 0x04;
        pret[18] = 0x1c;
        break;
    case SHA_256:
        pret[1] = 0x31;
        pret[14] = 0x01;
        pret[18] = 0x20;
        break;
    case SHA_384:
        pret[1] = 0x41;
        pret[14] = 0x02;
        pret[18] = 0x30;
        break;
    case SHA_512:
        pret[1] = 0x51;
        pret[14] = 0x03;
        pret[18] = 0x40;
        break;
    case SHA_512_224:
        pret[1] = 0x2d;
        pret[14] = 0x05;
        pret[18] = 0x1c;
        break;
    case SHA_512_256:
        pret[1] = 0x31;
        pret[14] = 0x06;
        pret[18] = 0x20;
        break;
    default:
        return E_INVALID;
    }
    rvv_memcpy(em + elen - 19 - md.dlen, pret, 19);

    rvv_memcpy(em + elen - md.dlen, md.dgst, md.dlen);

    return pufs_rsa_sign(sig, rsatype, n, puk, prk, em, phi);
}
/**
 * pufs_rsa_pss_verify()
 */
pufs_status_t pufs_rsa_pss_verify(const uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk,
    pufs_hash_t hash,
    const uint8_t* msg,
    uint32_t msglen)
{
    pufs_status_t check;
    uint8_t em[BUFFER_SIZE], mask[BUFFER_SIZE];
    uint32_t elen, i;
    pufs_dgst_st md;

    if ((check = pufs_rsa_get_field_elen(NULL, &elen, rsatype)) != SUCCESS)
        return check;
    if ((check = pufs_rsa_verify_calc(em, sig, rsatype, n, puk)) != SUCCESS)
        return check;

    if (em[elen - 1] != 0xbc)
        return E_VERFAIL;
    if (em[0] & 0x80)
        return E_VERFAIL;

    if ((check = pufs_hash(&md, NULL, 0, hash)) != SUCCESS)
        return check;
    if ((check = pufs_rsa_pss_mgf(mask, elen - md.dlen - 1, hash,
             em + elen - md.dlen - 1, md.dlen))
        != SUCCESS)
        return check;

    for (i = 0; i < elen - md.dlen - 1; i++)
        mask[i] ^= em[i];
    mask[0] &= 0x7f;

    for (i = 0; i < elen - md.dlen - 1; i++)
        if (mask[i] != 0)
            break;
    if (mask[i] != 0x01)
        return E_VERFAIL;
    uint8_t* salt = mask + i + 1;
    uint32_t saltlen = elen - md.dlen - i - 2;
    memmove(mask + md.dlen, salt, saltlen);

    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;
    rvv_memcpy(mask, md.dgst, md.dlen);

    pufs_hash_ctx* hash_ctx = pufs_hash_ctx_new();
    if (hash_ctx == NULL)
        return E_UNAVAIL;
    if (((check = pufs_hash_init(hash_ctx, hash)) != SUCCESS) || ((check = pufs_hash_update(hash_ctx, rsa_pss_pad1, 8)) != SUCCESS) || ((check = pufs_hash_update(hash_ctx, mask, md.dlen + saltlen)) != SUCCESS) || ((check = pufs_hash_final(hash_ctx, &md)) != SUCCESS))
        return check;
    pufs_hash_ctx_free(hash_ctx);

    if (memcmp(em + elen - md.dlen - 1, md.dgst, md.dlen) != 0)
        return E_VERFAIL;

    return SUCCESS;
}
/**
 * _pufs_rsa_pss_sign()
 */
pufs_status_t _pufs_rsa_pss_sign(uint8_t* sig,
    pufs_rsa_type_t rsatype,
    const uint8_t* n,
    uint32_t puk,
    const uint8_t* prk,
    pufs_hash_t hash,
    const uint8_t* msg,
    uint32_t msglen,
    const uint8_t* salt,
    uint32_t saltlen,
    const uint8_t* phi)
{
    pufs_status_t check;
    uint8_t em[BUFFER_SIZE], mask[BUFFER_SIZE];
    uint32_t elen;
    pufs_dgst_st md;

    if ((check = pufs_rsa_get_field_elen(NULL, &elen, rsatype)) != SUCCESS)
        return check;
    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;
    if ((md.dlen + saltlen + 2) > elen)
        return E_INVALID;

    rvv_memcpy(mask, md.dgst, md.dlen);
    rvv_memcpy(mask + md.dlen, salt, saltlen);
    pufs_hash_ctx* hash_ctx = pufs_hash_ctx_new();
    if (hash_ctx == NULL)
        return E_UNAVAIL;
    if (((check = pufs_hash_init(hash_ctx, hash)) != SUCCESS) || ((check = pufs_hash_update(hash_ctx, rsa_pss_pad1, 8)) != SUCCESS) || ((check = pufs_hash_update(hash_ctx, mask, md.dlen + saltlen)) != SUCCESS) || ((check = pufs_hash_final(hash_ctx, &md)) != SUCCESS))
        return check;
    pufs_hash_ctx_free(hash_ctx);

    if ((check = pufs_rsa_pss_mgf(mask, elen - md.dlen - 1, hash,
             md.dgst, md.dlen))
        != SUCCESS)
        return check;

    rvv_memset(em, 0, elen - saltlen - md.dlen - 2);
    em[elen - saltlen - md.dlen - 2] = 0x01;
    rvv_memcpy(em + elen - saltlen - md.dlen - 1, salt, saltlen);
    for (uint32_t i = 0; i < elen - md.dlen - 1; i++)
        em[i] ^= mask[i];
    em[0] &= 0x7f;
    rvv_memcpy(em + elen - md.dlen - 1, md.dgst, md.dlen);
    em[elen - 1] = 0xbc;

    return pufs_rsa_sign(sig, rsatype, n, puk, prk, em, phi);
}
/**
 * pufs_ecp_set_curve_byname()
 */
pufs_status_t pufs_ecp_set_curve_byname(pufs_ec_name_t name)
{
    if (name >= N_ECNAME_T)
        return E_INVALID;
    if ((ecdp_set.name == name) && ecdp_set.isset)
        return SUCCESS;
    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if (name == SM2)
        return pufs_ecp_set_sm2_curve();

    // check if the ECP version and MP version is supported by firmware
    uint32_t ecp_ver, mp_ver;
    ecp_ver = ecp_regs->version;
    mp_ver = ecp_regs->mp_version;

    pufs_ecp_mprog_curve_st* mprog = NULL;
    pufs_ecp_mprog_cmac_st* mpmac = NULL;
    for (int i = 0; ecp_mprog[i] != NULL; i++) { // search the ECP version
        if (ecp_mprog[i]->ecp_version == ecp_ver) {
            for (int j = 0; ecp_mprog[i]->mprog[j] != NULL; j++) { // search the curve name
                if (ecp_mprog[i]->mprog[j]->name == name) {
                    mprog = ecp_mprog[i]->mprog[j];
                    for (int k = 0; mprog->mprog_sum[k] != NULL; k++) { // search the MP version
                        if (mprog->mprog_sum[k]->mp_version == mp_ver) {
                            mpmac = mprog->mprog_sum[k];
                            break;
                        }
                    }
                    break;
                }
            }
            break;
        }
    }

    if ((mprog == NULL) || (mpmac == NULL))
        return E_UNSUPPORT;

    ecdp_set.mprog = mprog;
    ecdp_set.mpmac = mpmac;
    ecdp_set.name = name;
    ecdp_set.isset = true;

    return pufs_ecp_set_curve_params_byname(name, ecp_ver);
}
/**
 * pufs_ecp_gen_sprk()
 */
pufs_status_t pufs_ecp_gen_sprk(pufs_ka_slot_t slot, pufs_key_type_t keytype,
    size_t keyaddr, uint32_t keybits,
    const uint8_t* salt, uint32_t saltlen,
    const uint8_t* info, uint32_t infolen, pufs_hash_t hash)
{
    void *mac, *program;
    pufs_status_t check;
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    // check KA private key slot
    if ((check = keyslot_check(false, PRKEY, slot)) != SUCCESS)
        return check;

    /// Prepare: generate keying material for static prk generation
    uint32_t randlen = (((ecc_param[ecdp_set.name].len + 3) / 4) + 2) * 4;
    if ((check = pufs_kdf(SHARESEC, SHARESEC_0, B2b(randlen), PRF_HMAC, hash,
             false, NULL, 0, 1, keytype, keyaddr, keybits, salt,
             saltlen, info, infolen))
        != SUCCESS)
        return check;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if (ecdp_set.name == SM2) {
        pufs_sm2_mprog_st* prog = &(sm2_mprog[ecp_version]);
        pufs_sm2_mprog_cmac_st* cmac = prog->cmac[mp_version];

        mac = (void*)cmac->prks_gen;
        program = (void*)prog->func->prks_gen;
    } else {
        if ((ecdp_set.mpmac->prks_gen == NULL) || (ecdp_set.mprog->prks_gen == NULL))
            return E_UNSUPPORT;

        mac = (void*)ecdp_set.mpmac->prks_gen;
        program = (void*)ecdp_set.mprog->prks_gen;
    }

    // rvv_memcpy((void *)ecp_regs->mac, mac, ECP_MPMAC_SIZE);
    // rvv_memcpy((void *)ecp_regs->program, program, ECP_MPROG_SIZE);
    memcpy4b((void*)ecp_regs->mac, mac, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, program, ECP_MPROG_SIZE);

    uint32_t val32;
    val32 = (get_key_slot_idx(SHARESEC, SHARESEC_0) << 1) << 0;
    val32 |= (get_key_slot_idx(PRKEY, slot) << 1) << 16;
    ecp_regs->keysel = val32;

    val32 = pufs_ecc_start();

    return (val32 & (0x1 << 1)) ? E_ECMPROG : SUCCESS;
}
/**
 * pufs_ecp_gen_eprk()
 */
pufs_status_t pufs_ecp_gen_eprk(pufs_ka_slot_t slot)
{
    pufs_status_t check;
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    // check KA private key slot
    if ((check = keyslot_check(false, PRKEY, slot)) != SUCCESS)
        return check;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if ((ecdp_set.mpmac->prke_gen == NULL) || (ecdp_set.mprog->prke_gen == NULL))
        return E_UNSUPPORT;

    memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->prke_gen, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->prke_gen, ECP_MPROG_SIZE);

    uint32_t val32;
    val32 = (get_key_slot_idx(PRKEY, slot) << 1) << 16;
    ecp_regs->keysel = val32;

    val32 = pufs_ecc_start();

    return (val32 & (0x1 << 1)) ? E_ECMPROG : SUCCESS;
}
/**
 * pufs_ecp_gen_puk()
 */
pufs_status_t pufs_ecp_gen_puk(pufs_ec_point_st* puk, pufs_key_type_t prktype,
    uint32_t prkslot)
{
    pufs_status_t check;
    uint32_t val32;
    void *mac, *program;
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    // check KA private key slot
    if ((prktype != PRKEY) && (prktype != OTPKEY))
        return E_INVALID;
    if ((check = keyslot_check(true, prktype, prkslot, ecc_param[ecdp_set.name].fbits)) != SUCCESS)
        return check;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if (ecdp_set.name == SM2) {
        pufs_sm2_mprog_st* prog = &(sm2_mprog[ecp_version]);
        pufs_sm2_mprog_cmac_st* cmac = prog->cmac[mp_version];

        mac = (void*)cmac->puk_gen;
        program = (void*)prog->func->puk_gen;
    } else {
        if ((ecdp_set.mpmac->puk_gen == NULL) || (ecdp_set.mprog->puk_gen == NULL))
            return E_UNSUPPORT;

        mac = (void*)ecdp_set.mpmac->puk_gen;
        program = (void*)ecdp_set.mprog->puk_gen;
    }

    memcpy4b((void*)ecp_regs->mac, mac, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, program, ECP_MPROG_SIZE);

    if (prktype == PRKEY) {
        val32 = (get_key_slot_idx(prktype, prkslot) << 1) << 0;
        ecp_regs->keysel = val32;
    } else // OTPKEY
    {
        val32 = 0x1 << 0;
        ecp_regs->keysel = val32;
        val32 = get_key_slot_idx(prktype, prkslot) << 4;
        ecp_regs->otpkba = val32;
    }

    val32 = pufs_ecc_start();

    if (val32 & (0x1 << 1))
        return E_ECMPROG;

    // puk x, y read from SRAM
    puk->qlen = ecc_param[ecdp_set.name].len;
    read_ecp_operand(9, puk->x, puk->qlen);
    read_ecp_operand(10, puk->y, puk->qlen);

    return SUCCESS;
}
/**
 * _pufs_ecp_validate_puk()
 */
pufs_status_t _pufs_ecp_validate_puk(pufs_ec_point_st puk, bool full)
{
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    uint32_t elen = ecc_param[ecdp_set.name].len;
    if (puk.qlen != elen)
        return E_INVALID;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if (full) {
        if ((ecdp_set.mpmac->pukv_f == NULL) || (ecdp_set.mprog->pukv_f == NULL))
            return E_UNSUPPORT;

        memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->pukv_f, ECP_MPMAC_SIZE);
        memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->pukv_f, ECP_MPROG_SIZE);
    } else {
        if ((ecdp_set.mpmac->pukv_p == NULL) || (ecdp_set.mprog->pukv_p == NULL))
            return E_UNSUPPORT;

        // rvv_memcpy((void *)ecp_regs->mac, ecdp_set.mpmac->pukv_p, ECP_MPMAC_SIZE);
        // rvv_memcpy((void *)ecp_regs->program, ecdp_set.mprog->pukv_p, ECP_MPROG_SIZE);
        memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->pukv_p, ECP_MPMAC_SIZE);
        memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->pukv_p, ECP_MPROG_SIZE);
    }

    write_ecp_operand(9, puk.x, elen);
    write_ecp_operand(10, puk.y, elen);

    uint32_t val32;
    val32 = pufs_ecc_start();

    if (val32 & (0x1 << 1))
        return E_ECMPROG;
    else if (val32 & (0x1 << 3))
        return E_VERFAIL;
    else
        return SUCCESS;
}
/**
 * _pufs_ecp_ecccdh_2e()
 */
pufs_status_t _pufs_ecp_ecccdh_2e(pufs_ec_point_st puk,
    pufs_ka_slot_t prkslot,
    uint8_t* ss)
{
    pufs_status_t check;
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    // check KA private key slot
    if ((check = keyslot_check(true, PRKEY, prkslot, ecc_param[ecdp_set.name].fbits)) != SUCCESS)
        return check;
    uint32_t elen = ecc_param[ecdp_set.name].len;
    if (puk.qlen != elen)
        return E_INVALID;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if (ss == NULL) {
        if ((ecdp_set.mpmac->eccdh_2e == NULL) || (ecdp_set.mprog->eccdh_2e == NULL))
            return E_UNSUPPORT;

        memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->eccdh_2e, ECP_MPMAC_SIZE);
        memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->eccdh_2e, ECP_MPROG_SIZE);
    } else {
        if ((ecdp_set.mpmac->eccdh_2e_oss == NULL) || (ecdp_set.mprog->eccdh_2e_oss == NULL))
            return E_UNSUPPORT;

        memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->eccdh_2e_oss, ECP_MPMAC_SIZE);
        memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->eccdh_2e_oss, ECP_MPROG_SIZE);
    }

    uint32_t val32;
    val32 = (get_key_slot_idx(SHARESEC, SHARESEC_0) << 1) << 16;
    val32 |= (get_key_slot_idx(PRKEY, prkslot) << 1) << 0;
    ecp_regs->keysel = val32;

    write_ecp_operand(6, puk.x, elen);
    write_ecp_operand(7, puk.y, elen);

    val32 = pufs_ecc_start();

    if (val32 & (0x1 << 1))
        return E_ECMPROG;

    if (ss != NULL)
        read_ecp_operand(9, ss, elen);

    return SUCCESS;
}
/**
 * _pufs_ecp_ecccdh_2e2s()
 */
pufs_status_t _pufs_ecp_ecccdh_2e2s(pufs_ec_point_st puk_e,
    pufs_ec_point_st puk_s,
    pufs_ka_slot_t prkslot_e,
    pufs_key_type_t prktype_s,
    uint32_t prkslot_s,
    uint8_t* ss)
{
    pufs_status_t check;
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    // check KA private key slot for ephemeral private key
    if ((check = keyslot_check(true, PRKEY, prkslot_e, ecc_param[ecdp_set.name].fbits)) != SUCCESS)
        return check;
    // check KA private key slot for static private key
    if ((prktype_s != PRKEY) && (prktype_s != OTPKEY))
        return E_INVALID;
    if ((check = keyslot_check(true, prktype_s, prkslot_s, ecc_param[ecdp_set.name].fbits)) != SUCCESS)
        return check;
    uint32_t elen = ecc_param[ecdp_set.name].len;
    if ((puk_s.qlen != elen) || (puk_e.qlen != elen))
        return E_INVALID;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if (ss == NULL) {
        if ((ecdp_set.mpmac->eccdh_2s2e == NULL) || (ecdp_set.mprog->eccdh_2s2e == NULL))
            return E_UNSUPPORT;

        memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->eccdh_2s2e, ECP_MPMAC_SIZE);
        memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->eccdh_2s2e, ECP_MPROG_SIZE);
    } else {
        if ((ecdp_set.mpmac->eccdh_2s2e_oss == NULL) || (ecdp_set.mprog->eccdh_2s2e_oss == NULL))
            return E_UNSUPPORT;

        memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->eccdh_2s2e_oss, ECP_MPMAC_SIZE);
        memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->eccdh_2s2e_oss, ECP_MPROG_SIZE);
    }

    uint32_t val32;
    val32 = (get_key_slot_idx(SHARESEC, SHARESEC_0) << 1) << 16;
    val32 |= (get_key_slot_idx(PRKEY, prkslot_e) << 1) << 8;
    if (prktype_s == PRKEY) {
        val32 |= (get_key_slot_idx(prktype_s, prkslot_s) << 1) << 0;
        ecp_regs->keysel = val32;
    } else // OTPKEY
    {
        val32 |= 0x1 << 0;
        ecp_regs->keysel = val32;
        val32 = get_key_slot_idx(prktype_s, prkslot_s) << 4;
        ecp_regs->otpkba = val32;
    }

    write_ecp_operand(6, puk_s.x, elen);
    write_ecp_operand(7, puk_s.y, elen);
    write_ecp_operand(8, puk_e.x, elen);
    write_ecp_operand(9, puk_e.y, elen);

    val32 = pufs_ecc_start();

    if (val32 & (0x1 << 1))
        return E_ECMPROG;

    if (ss != NULL) {
        read_ecp_operand(9, ss, elen, true);
    }

    return SUCCESS;
}
/**
 * pufs_ecp_ecdsa_verify_dgst()
 */
pufs_status_t pufs_ecp_ecdsa_verify_dgst(pufs_ecdsa_sig_st sig, pufs_dgst_st md,
    pufs_ec_point_st puk)
{
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    uint32_t elen = ecc_param[ecdp_set.name].len;
    if ((puk.qlen != elen) || (sig.qlen != elen))
        return E_INVALID;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if ((ecdp_set.mpmac->ecdsa_v == NULL) || (ecdp_set.mprog->ecdsa_v == NULL))
        return E_UNSUPPORT;

    memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->ecdsa_v, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->ecdsa_v, ECP_MPROG_SIZE);

    // handle message digest
    uint8_t z[MAXELEN];
    get_z(ecdp_set.name, z, md.dgst, md.dlen);
    write_ecp_operand(6, z, elen);
    write_ecp_operand(7, puk.x, elen);
    write_ecp_operand(8, puk.y, elen);
    write_ecp_operand(9, sig.r, elen);
    write_ecp_operand(10, sig.s, elen);

    uint32_t val32;
    val32 = pufs_ecc_start();

    if (val32 & (0x1 << 1))
        return E_ECMPROG;
    else if (val32 & (0x1 << 2))
        return E_VERFAIL;
    else
        return SUCCESS;
}
/**
 * pufs_ecp_ecdsa_verify_msg()
 */
pufs_status_t pufs_ecp_ecdsa_verify_msg(pufs_ecdsa_sig_st sig,
    const uint8_t* msg, uint32_t msglen,
    pufs_hash_t hash, pufs_ec_point_st puk)
{
    pufs_status_t check;
    // Step 1: calculate the digest
    pufs_dgst_st md;
    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;
    // Step 2: verify the digest
    return pufs_ecp_ecdsa_verify_dgst(sig, md, puk);
}
/**
 * pufs_ecp_ecdsa_verify_dgst_otpkey()
 */
pufs_status_t pufs_ecp_ecdsa_verify_dgst_otpkey(
    pufs_ecdsa_sig_st sig, pufs_dgst_st md, pufs_rt_slot_t puk)
{
    pufs_status_t check;
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    if ((check = keyslot_check(true, OTPKEY, puk, ecc_param[ecdp_set.name].fbits)) != SUCCESS)
        return check;
    uint32_t elen = ecc_param[ecdp_set.name].len;
    if (sig.qlen != elen)
        return E_INVALID;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if ((ecdp_set.mpmac->ecdsa_v_otpk == NULL) || (ecdp_set.mprog->ecdsa_v_otpk == NULL))
        return E_UNSUPPORT;

    // rvv_memcpy((void *)ecp_regs->mac, ecdp_set.mpmac->ecdsa_v_otpk, ECP_MPMAC_SIZE);
    // rvv_memcpy((void *)ecp_regs->program, ecdp_set.mprog->ecdsa_v_otpk, ECP_MPROG_SIZE);
    memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->ecdsa_v_otpk, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->ecdsa_v_otpk, ECP_MPROG_SIZE);

    uint8_t z[MAXELEN];
    get_z(ecdp_set.name, z, md.dgst, md.dlen);
    write_ecp_operand(6, z, elen);
    write_ecp_operand(9, sig.r, elen);
    write_ecp_operand(10, sig.s, elen);

    uint32_t val32;
    val32 = get_key_slot_idx(OTPKEY, puk) << 4;
    ecp_regs->otpkba = val32;

    val32 = pufs_ecc_start();

    if (val32 & (0x1 << 1))
        return E_ECMPROG;
    else if (val32 & (0x1 << 2))
        return E_VERFAIL;
    else
        return SUCCESS;
}
/**
 * pufs_ecp_ecdsa_verify_msg_otpkey()
 */
pufs_status_t pufs_ecp_ecdsa_verify_msg_otpkey(
    pufs_ecdsa_sig_st sig, const uint8_t* msg, uint32_t msglen,
    pufs_hash_t hash, pufs_rt_slot_t puk)
{
    pufs_status_t check;
    // Step 1: calculate the digest
    pufs_dgst_st md;
    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;
    // Step 2: verify the digest
    return pufs_ecp_ecdsa_verify_dgst_otpkey(sig, md, puk);
}
/**
 * _pufs_ecp_ecdsa_sign_dgst()
 */
pufs_status_t _pufs_ecp_ecdsa_sign_dgst(pufs_ecdsa_sig_st* sig, pufs_dgst_st md,
    pufs_key_type_t prktype,
    uint32_t prkslot, const uint8_t* k)
{
    pufs_status_t check;
    // check if ECDP is set
    if (ecdp_set.isset == false)
        return E_INVALID;
    // check KA private key slot
    if ((prktype != PRKEY) && (prktype != OTPKEY))
        return E_INVALID;
    if ((check = keyslot_check(true, prktype, prkslot, ecc_param[ecdp_set.name].fbits)) != SUCCESS)
        return check;

    if (ecp_regs->status & ECP_STATUS_BUSY_MASK)
        return E_BUSY;

    if ((ecdp_set.mpmac->ecdsa_s == NULL) || (ecdp_set.mprog->ecdsa_s == NULL))
        return E_UNSUPPORT;

    memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->ecdsa_s, ECP_MPMAC_SIZE);
    memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->ecdsa_s, ECP_MPROG_SIZE);

    uint32_t val32;
    if (prktype == PRKEY) {
        val32 = (get_key_slot_idx(prktype, prkslot) << 1) << 0;
        ecp_regs->keysel = val32;
    } else // OTPKEY
    {
        val32 = 0x1 << 0;
        ecp_regs->keysel = val32;
        val32 = get_key_slot_idx(prktype, prkslot) << 4;
        ecp_regs->otpkba = val32;
    }

    // handle message digest
    uint8_t z[MAXELEN];
    uint32_t elen = ecc_param[ecdp_set.name].len;
    get_z(ecdp_set.name, z, md.dgst, md.dlen);
    write_ecp_operand(6, z, elen);
    if (k != NULL) {
        if ((ecdp_set.mpmac->ecdsa_s_ik == NULL) || (ecdp_set.mprog->ecdsa_s_ik == NULL))
            return E_UNSUPPORT;

        memcpy4b((void*)ecp_regs->mac, ecdp_set.mpmac->ecdsa_s_ik, ECP_MPMAC_SIZE);
        memcpy4b((void*)ecp_regs->program, ecdp_set.mprog->ecdsa_s_ik, ECP_MPROG_SIZE);

        write_ecp_operand(7, k, elen);
    }

    val32 = pufs_ecc_start();

    if (val32 & (0x1 << 1))
        return E_ECMPROG;

    // sig r, s read from SRAM
    sig->qlen = ecc_param[ecdp_set.name].len;
    read_ecp_operand(9, sig->r, sig->qlen);
    read_ecp_operand(10, sig->s, sig->qlen);

    return SUCCESS;
}
/**
 * _pufs_ecp_ecdsa_sign_msg()
 */
pufs_status_t _pufs_ecp_ecdsa_sign_msg(
    pufs_ecdsa_sig_st* sig, const uint8_t* msg, uint32_t msglen,
    pufs_hash_t hash, pufs_key_type_t prktype, uint32_t prkslot,
    const uint8_t* k)
{
    pufs_status_t check;
    // Step 1: calculate the digest
    pufs_dgst_st md;
    if ((check = pufs_hash(&md, msg, msglen, hash)) != SUCCESS)
        return check;
    // Step 2: sign the digest
    return pufs_ecp_ecdsa_sign_dgst(sig, md, prktype, prkslot, k);
}
