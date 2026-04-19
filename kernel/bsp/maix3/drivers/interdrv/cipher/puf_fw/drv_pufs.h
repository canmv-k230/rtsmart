/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DRV_PUFS__
#define __DRV_PUFS__
#include <stdint.h>
#include <stdbool.h>

/* ===== Constants ===== */
#define PUFS_HMAC_BLOCK_MAXLEN  128
#define PUFS_SW_KEY_MAXLEN      64
#define PUFS_DGST_INT_STATE_LEN 64
#define PUFS_BC_BLOCK_SIZE      16
#define PUFS_DGST_MAX_LEN       64
#define PUFS_CMAC_BLOCK_SIZE    16

/* ===== Operation types for streaming ops ===== */
enum pufs_op_type {
    PUFS_OP_INIT = 0,
    PUFS_OP_UPDATE,
    PUFS_OP_FINAL,
    PUFS_OP_INIT_UPDATE,   /* init + first update in one call */
    PUFS_OP_UPDATE_FINAL,  /* last update + final in one call */
};

/* ===== Ioctl commands: Atomic operations ===== */
#define PUFS_UID_GET        _IOWR('P', 0x00, int)
#define PUFS_OTP_READ       _IOWR('P', 0x01, int)
#define PUFS_OTP_WRITE      _IOWR('P', 0x02, int)
#define PUFS_OTP_LOCK       _IOWR('P', 0x03, int)
#define PUFS_RNG_READ       _IOWR('P', 0x04, int)
#define PUFS_OTP_RWLCK_GET  _IOWR('P', 0x05, int)
#define PUFS_RT_VERSION     _IOWR('P', 0x06, int)
#define PUFS_KEY2OTP        _IOWR('P', 0x07, int)
#define PUFS_ZEROIZE        _IOWR('P', 0x08, int)
#define PUFS_POST_MASK      _IOWR('P', 0x09, int)
#define PUFS_KEY_INOUT      _IOWR('P', 0x10, int)
#define PUFS_KEY_DERIVE     _IOWR('P', 0x11, int)
#define PUFS_ECC_PRK_GEN    _IOWR('P', 0x40, int)
#define PUFS_ECC_PUK_GEN    _IOWR('P', 0x41, int)
#define PUFS_ECC_PUK_VERIFY _IOWR('P', 0x42, int)
#define PUFS_ECC_CDH        _IOWR('P', 0x43, int)
#define PUFS_ECDSA_SIGN     _IOWR('P', 0x45, int)
#define PUFS_ECDSA_VERIFY   _IOWR('P', 0x46, int)
#define PUFS_SM2_SIGN       _IOWR('P', 0x48, int)
#define PUFS_SM2_VERIFY     _IOWR('P', 0x49, int)
#define PUFS_SM2_ENC        _IOWR('P', 0x4A, int)
#define PUFS_SM2_DEC        _IOWR('P', 0x4B, int)
#define PUFS_SM2_KEX        _IOWR('P', 0x4C, int)
#define PUFS_RSA_SIGN       _IOWR('P', 0x4E, int)
#define PUFS_RSA_VERIFY     _IOWR('P', 0x4F, int)

/* ===== Ioctl commands: Streaming operations (context-carrying) ===== */
#define PUFS_HASH_OP        _IOWR('P', 0x60, int)
#define PUFS_HMAC_OP        _IOWR('P', 0x61, int)
#define PUFS_CMAC_OP        _IOWR('P', 0x62, int)
#define PUFS_SP38A_OP       _IOWR('P', 0x63, int)
#define PUFS_SP38D_OP       _IOWR('P', 0x64, int)
#define PUFS_SP38C_OP       _IOWR('P', 0x65, int)
#define PUFS_SP38E_OP       _IOWR('P', 0x66, int)

/* ===== Ioctl commands: DRBG (SP800-90A) ===== */
#define PUFS_DRBG_INIT      _IOWR('P', 0x70, int)
#define PUFS_DRBG_RESEED    _IOWR('P', 0x71, int)
#define PUFS_DRBG_GENERATE  _IOWR('P', 0x72, int)
#define PUFS_DRBG_UNINIT    _IOWR('P', 0x73, int)

/* ===== Enums ===== */
typedef enum {
    KT_SWKEY,
    KT_OTPKEY,
    KT_PUFKEY,
    KT_RANDKEY,
    KT_SHARESEC,
    KT_SSKEY,
    KT_PRKEY,
} pufs_keytype_t;

typedef enum {
    KS_SK128_0, KS_SK128_1, KS_SK128_2, KS_SK128_3,
    KS_SK128_4, KS_SK128_5, KS_SK128_6, KS_SK128_7,
    KS_SK256_0, KS_SK256_1, KS_SK256_2, KS_SK256_3,
    KS_SK512_0, KS_SK512_1,
    KS_PRK_0, KS_PRK_1, KS_PRK_2,
    KS_SHARESEC_0,
    KS_PUFSLOT_0 = 0, KS_PUFSLOT_1, KS_PUFSLOT_2, KS_PUFSLOT_3,
    KS_OTPKEY_0, KS_OTPKEY_1, KS_OTPKEY_2, KS_OTPKEY_3,
    KS_OTPKEY_4, KS_OTPKEY_5, KS_OTPKEY_6, KS_OTPKEY_7,
    KS_OTPKEY_8, KS_OTPKEY_9, KS_OTPKEY_10, KS_OTPKEY_11,
    KS_OTPKEY_12, KS_OTPKEY_13, KS_OTPKEY_14, KS_OTPKEY_15,
    KS_OTPKEY_16, KS_OTPKEY_17, KS_OTPKEY_18, KS_OTPKEY_19,
    KS_OTPKEY_20, KS_OTPKEY_21, KS_OTPKEY_22, KS_OTPKEY_23,
    KS_OTPKEY_24, KS_OTPKEY_25, KS_OTPKEY_26, KS_OTPKEY_27,
    KS_OTPKEY_28, KS_OTPKEY_29, KS_OTPKEY_30, KS_OTPKEY_31,
} pufs_keyslot_t;

typedef enum {
    KW_AES_CBC_CS2, KW_AES_KW, KW_AES_KWP, KW_AES_KW_INV, KW_AES_KWP_INV,
} pufs_keywrap_t;

typedef enum {
    KM_IMPORT_PT, KM_IMPORT_WRAP, KM_EXPORT_PT, KM_EXPORT_WRAP, KM_CLEAR,
} pufs_keymode_t;

typedef enum {
    DRBG_AES_CTR, DRBG_HASH, DRBG_HMAC,
} pufs_drbg_type_t;

typedef enum {
    KD_METHOD_PBKDF, KD_METHOD_KBKDF_EXPAND, KD_METHOD_KBKDF_EXTRACT,
    KD_METHOD_KBKDF_EXPAND_EXTRACT, KD_METHOD_SM2,
} pufs_kd_md_t;

typedef enum {
    KD_PRF_HMAC, KD_PRF_HASH, KD_PRF_CMAC,
} pufs_kd_prf_t;

typedef enum {
    HASH_SHA_224, HASH_SHA_256, HASH_SHA_384, HASH_SHA_512,
    HASH_SHA_512_224, HASH_SHA_512_256, HASH_SM3,
} pufs_hashtype_t;

typedef enum {
    ECC_NISTB163, ECC_NISTB233, ECC_NISTB283, ECC_NISTB409, ECC_NISTB571,
    ECC_NISTK163, ECC_NISTK233, ECC_NISTK283, ECC_NISTK409, ECC_NISTK571,
    ECC_NISTP192, ECC_NISTP224, ECC_NISTP256, ECC_NISTP384, ECC_NISTP521,
    ECC_SM2,
} pufs_ecctype_t;

typedef enum {
    RSA_1024, RSA_2048, RSA_3072, RSA_4096,
} pufs_rsatype_t;

typedef enum {
    RSA_BASE, RSA_X931, RSA_P1V15, RSA_PSS,
} pufs_rsamode_t;

typedef enum {
    SM2CT_C1C2C3, SM2CT_C1C3C2,
} pufs_sm2ct_format_t;

typedef enum {
    MAC_HMAC, MAC_CMAC,
} pufs_mac_cipher_t;

typedef enum {
    SK_AES, SK_SM4,
} pufs_skcipher_t;

typedef enum {
    MODE_ECB = 1, MODE_CFB, MODE_OFB,
    MODE_CBC, MODE_CBC_CS1, MODE_CBC_CS2, MODE_CBC_CS3,
    MODE_CTR_32, MODE_CTR_64, MODE_CTR,
    MODE_GCM = 0x10, MODE_CCM, MODE_XTS,
} pufs_skcipher_mode_t;

/* ===== Portable context types for streaming ops ===== */

/* Hash/HMAC context (maps to kernel pufs_hmac_context) */
typedef struct {
    uint8_t buff[PUFS_HMAC_BLOCK_MAXLEN];
    uint8_t key[PUFS_HMAC_BLOCK_MAXLEN];
    uint8_t state[PUFS_DGST_INT_STATE_LEN];
    uint32_t buflen;
    uint32_t keybits;
    uint32_t minlen;
    uint32_t curlen;
    uint32_t keyslot;
    uint32_t blocklen;
    uint32_t keytype;
    uint32_t op;
    uint32_t hash;
    bool start;
} pufs_hash_ctx_t;

/* CMAC context (maps to kernel pufs_cmac_context) */
typedef struct {
    uint8_t buff[PUFS_CMAC_BLOCK_SIZE];
    uint8_t key[PUFS_SW_KEY_MAXLEN];
    uint8_t state[PUFS_DGST_INT_STATE_LEN];
    uint32_t buflen;
    uint32_t keybits;
    uint32_t minlen;
    uint32_t keyslot;
    uint32_t keytype;
    uint32_t op;
    uint32_t cipher;
    bool start;
} pufs_cmac_ctx_t;

/* SP38A context: ECB/CBC/CFB/OFB/CTR (maps to kernel pufs_sp38a_context) */
typedef struct {
    uint8_t buff[2 * PUFS_BC_BLOCK_SIZE];
    uint8_t key[PUFS_SW_KEY_MAXLEN];
    uint8_t iv[PUFS_BC_BLOCK_SIZE];
    uint32_t buflen;
    uint32_t keybits;
    uint32_t minlen;
    uint32_t keyslot;
    uint32_t keytype;
    uint32_t op;
    uint32_t cipher;
    bool encrypt;
    bool start;
} pufs_sp38a_ctx_t;

/* SP38C context: CCM (maps to kernel pufs_sp38c_context) */
typedef struct {
    uint64_t aadlen;
    uint64_t inlen;
    uint64_t currentlen;
    uint8_t buff[PUFS_BC_BLOCK_SIZE];
    uint8_t key[PUFS_SW_KEY_MAXLEN];
    uint8_t ctri[PUFS_BC_BLOCK_SIZE];
    uint8_t cbcmac[PUFS_BC_BLOCK_SIZE];
    uint32_t qlen;
    uint32_t buflen;
    uint32_t keybits;
    uint32_t minlen;
    uint32_t keyslot;
    uint32_t taglen;
    uint32_t keytype;
    uint32_t op;
    uint32_t stage;
    uint32_t cipher;
    bool encrypt;
    bool ctr_start;
    bool cbcmac_start;
} pufs_sp38c_ctx_t;

/* SP38D context: GCM (maps to kernel pufs_sp38d_context) */
typedef struct {
    uint64_t aadbits;
    uint64_t inbits;
    uint8_t buff[PUFS_BC_BLOCK_SIZE];
    uint8_t key[PUFS_SW_KEY_MAXLEN];
    uint8_t j0[PUFS_BC_BLOCK_SIZE];
    uint8_t ghash[PUFS_BC_BLOCK_SIZE];
    uint32_t buflen;
    uint32_t keybits;
    uint32_t minlen;
    uint32_t keyslot;
    uint32_t incj0;
    uint32_t keytype;
    uint32_t op;
    uint32_t stage;
    uint32_t cipher;
    bool encrypt;
    bool start;
} pufs_sp38d_ctx_t;

/* SP38E context: XTS (maps to kernel pufs_sp38e_context) */
typedef struct {
    uint8_t buff[2 * PUFS_BC_BLOCK_SIZE];
    uint8_t key1[PUFS_SW_KEY_MAXLEN];
    uint8_t key2[PUFS_SW_KEY_MAXLEN];
    uint8_t i[PUFS_BC_BLOCK_SIZE];
    uint32_t buflen;
    uint32_t keybits;
    uint32_t minlen;
    uint32_t keyslot1;
    uint32_t keyslot2;
    uint32_t j;
    uint32_t keytype1;
    uint32_t keytype2;
    uint32_t op;
    uint32_t cipher;
    bool encrypt;
    bool start;
} pufs_sp38e_ctx_t;

/* ===== Streaming operation ioctl structs ===== */

typedef struct {
    uint8_t op;
    uint8_t hash;
    uint8_t _pad[2];
    uint32_t msglen;
    uint64_t msg_phys;
    uint64_t dgst_phys;
    uint32_t dlen;
    pufs_hash_ctx_t ctx;
} pufs_hash_op_t;

typedef struct {
    uint8_t op;
    uint8_t hash;
    uint8_t keytype;
    uint8_t _pad;
    uint32_t keybits;
    uint32_t msglen;
    uint64_t msg_phys;
    uint64_t dgst_phys;
    uint32_t dlen;
    pufs_hash_ctx_t ctx;
} pufs_hmac_op_t;

typedef struct {
    uint8_t op;
    uint8_t cipher;
    uint8_t keytype;
    uint8_t _pad;
    uint32_t keybits;
    uint32_t msglen;
    uint64_t msg_phys;
    uint64_t dgst_phys;
    uint32_t dlen;
    pufs_cmac_ctx_t ctx;
} pufs_cmac_op_t;

typedef struct {
    uint8_t op;
    uint8_t cipher;
    uint8_t mode;
    uint8_t encrypt;
    uint8_t keytype;
    uint8_t _pad[3];
    uint32_t keybits;
    uint8_t key[PUFS_SW_KEY_MAXLEN];
    uint8_t iv[PUFS_BC_BLOCK_SIZE];
    uint32_t ivlen;
    uint32_t inlen;
    uint64_t in_phys;
    uint64_t out_phys;
    uint32_t outlen;
    pufs_sp38a_ctx_t ctx;
} pufs_sp38a_op_t;

typedef struct {
    uint8_t op;
    uint8_t cipher;
    uint8_t encrypt;
    uint8_t keytype;
    uint32_t keybits;
    uint8_t key[PUFS_SW_KEY_MAXLEN];
    uint8_t iv[PUFS_BC_BLOCK_SIZE];
    uint32_t ivlen;
    uint32_t inlen;
    uint64_t in_phys;
    uint64_t out_phys;
    uint32_t outlen;
    uint64_t tag_phys;
    uint32_t taglen;
    pufs_sp38d_ctx_t ctx;
} pufs_sp38d_op_t;

typedef struct {
    uint8_t op;
    uint8_t cipher;
    uint8_t encrypt;
    uint8_t keytype;
    uint32_t keybits;
    uint8_t key[PUFS_SW_KEY_MAXLEN];
    uint8_t nonce[PUFS_BC_BLOCK_SIZE];
    uint32_t noncelen;
    uint64_t ccm_aadlen;
    uint64_t ccm_inlen;
    uint32_t taglen;
    uint32_t inlen;
    uint64_t in_phys;
    uint64_t out_phys;
    uint32_t outlen;
    uint64_t tag_phys;
    pufs_sp38c_ctx_t ctx;
} pufs_sp38c_op_t;

typedef struct {
    uint8_t op;
    uint8_t cipher;
    uint8_t encrypt;
    uint8_t keytype1;
    uint8_t keytype2;
    uint8_t _pad[3];
    uint32_t keybits;
    uint8_t key1[PUFS_SW_KEY_MAXLEN];
    uint8_t key2[PUFS_SW_KEY_MAXLEN];
    uint8_t iv[PUFS_BC_BLOCK_SIZE];
    uint32_t ivlen;
    uint32_t inlen;
    uint64_t in_phys;
    uint64_t out_phys;
    uint32_t outlen;
    pufs_sp38e_ctx_t ctx;
} pufs_sp38e_op_t;

/* ===== Atomic operation structs (unchanged from original) ===== */

typedef struct {
    uint8_t uid[32];
} pufs_uid_t;

typedef struct {
    uint8_t slot;
    uint64_t uid_phys;
} pufs_uid_get_t;

typedef struct {
    uint16_t addr;
    uint32_t len;
    uint8_t* buf;
} pufs_otp_rw_t;

typedef struct {
    uint16_t addr;
    uint32_t len;
    uint8_t  lock;    /* pufs_otp_lock_t: 0=NA, 1=RO, 2=RW */
} pufs_otp_lock_op_t;

typedef struct {
    uint32_t len;
    uint8_t* buf;
} pufs_rng_read_t;

typedef struct {
    uint16_t addr;
    uint8_t  lock;    /* output: pufs_otp_lock_t */
} pufs_otp_rwlck_get_t;

typedef struct {
    uint32_t version;
    uint32_t features;
} pufs_rt_version_t;

typedef struct {
    uint8_t  slot;    /* pufs_rt_slot_t: OTPKEY_0..31 */
    uint32_t keybits;
    uint8_t* key;
    uint8_t  lock;    /* pufs_otp_lock_t: NA/RO/RW, or N_OTP_LOCK_T to skip */
} pufs_key2otp_t;

typedef struct {
    uint8_t  slot;    /* pufs_rt_slot_t: PUFSLOT_0..3 */
} pufs_zeroize_t;

typedef struct {
    uint64_t maskslots;
} pufs_post_mask_t;

typedef struct {
    uint8_t  mode;    /* pufs_drbg_type_t */
    uint32_t security;
    uint8_t  df;
    uint32_t noncelen;
    uint32_t pstrlen;
    uint8_t* nonce;
    uint8_t* pstr;
} pufs_drbg_init_t;

typedef struct {
    uint8_t  df;
    uint32_t adinlen;
    uint8_t* adin;
} pufs_drbg_reseed_t;

typedef struct {
    uint32_t outbits;
    uint8_t  pr;
    uint8_t  df;
    uint32_t adinlen;
    uint8_t* adin;
    uint8_t* out;
} pufs_drbg_generate_t;

typedef struct {
    uint8_t mode;
    uint8_t keytype;
    uint8_t keyslot;
    uint8_t* keyaddr;
    uint32_t keybits;
    uint8_t keywrap;
    uint8_t kwslot;
    uint32_t kwbits;
} pufs_key_io_t;

typedef struct {
    uint8_t keytype;
    uint8_t keyslot;
    uint8_t method;
    uint8_t prf;
    uint8_t hash;
    uint8_t feedback;
    uint8_t ztype;
    uint32_t outbits;
    uint32_t iter;
    uint32_t ctrpos;
    uint32_t ctrlen;
    uint32_t zbits;
    uint32_t saltlen;
    uint32_t infolen;
    uint8_t* iv;
    uint8_t* zaddr;
    uint8_t* salt;
    uint8_t* info;
    uint8_t* out;
} pufs_key_derive_t;

typedef struct {
    uint8_t ecctype;
    uint8_t is_ephemeral;
    uint8_t prkslot;
    uint8_t keytype;
    uint8_t hashtype;
    uint32_t keybits;
    uint32_t saltlen;
    uint32_t infolen;
    uint8_t* keyaddr;
    uint8_t* salt;
    uint8_t* info;
} pufs_ecc_prk_gen_t;

#define QLEN_MAX 72
typedef struct {
    uint32_t qlen;
    uint8_t x[QLEN_MAX];
    uint8_t y[QLEN_MAX];
} pufs_ecc_puk_t;

typedef struct {
    uint8_t ecctype;
    uint8_t prktype;
    uint8_t prkslot;
    pufs_ecc_puk_t* puk;
} pufs_ecc_puk_gen_t;

typedef struct {
    uint8_t ecctype;
    pufs_ecc_puk_t* puk;
} pufs_ecc_puk_verify_t;

typedef struct {
    uint8_t ecctype;
    uint8_t is_ephemeral;
    uint8_t prkslot_e;
    uint8_t prktype_s;
    uint8_t prkslot_s;
    pufs_ecc_puk_t* puk_e;
    pufs_ecc_puk_t* puk_s;
    uint8_t* out;
} pufs_ecc_cdh_t;

#define NLEN_MAX 72
typedef struct {
    uint32_t qlen;
    uint8_t r[NLEN_MAX];
    uint8_t s[NLEN_MAX];
} pufs_ecdsa_sig_t;

typedef struct {
    uint8_t ecctype;
    uint8_t prktype;
    uint8_t prkslot;
    uint32_t mdlen;
    uint64_t md_phys;
    uint64_t sig_phys;
} pufs_ecdsa_sign_t;

typedef struct {
    uint8_t ecctype;
    uint32_t mdlen;
    union {
        uint64_t puk_phys;
        uint64_t otpslot;
    };
    uint64_t md_phys;
    uint64_t sig_phys;
} pufs_ecdsa_verify_t;

typedef struct {
    uint8_t prktype;
    uint8_t prkslot;
    uint32_t idlen;
    uint32_t msglen;
    uint64_t id_phys;
    uint64_t msg_phys;
    uint64_t sig_phys;
} pufs_sm2_sign_t;

typedef struct {
    uint32_t idlen;
    uint32_t msglen;
    uint64_t puk_phys;
    uint64_t id_phys;
    uint64_t msg_phys;
    uint64_t sig_phys;
} pufs_sm2_verify_t;

typedef struct {
    uint8_t format;
    uint32_t inlen;
    uint8_t* in;
    uint8_t* out;
    uint32_t* outlen;
    pufs_ecc_puk_t* puk;
} pufs_sm2_enc_t;

typedef struct {
    uint8_t format;
    uint8_t prkslot;
    uint32_t inlen;
    uint8_t* in;
    uint8_t* out;
    uint32_t* outlen;
} pufs_sm2_dec_t;

typedef struct {
    uint8_t init;
    uint8_t prkslotl;
    uint8_t tprkslotl;
    uint32_t idllen;
    uint32_t idrlen;
    uint32_t keybits;
    pufs_ecc_puk_t* pukr;
    pufs_ecc_puk_t* tpukr;
    uint8_t* idl;
    uint8_t* idr;
    uint8_t* key;
    uint8_t* dgst2;
    uint32_t* dlen2;
    uint8_t* dgst3;
    uint32_t* dlen3;
} pufs_sm2_kex_t;

typedef struct {
    uint8_t rsamode;
    uint8_t rsatype;
    uint8_t hashtype;
    uint32_t puk;
    uint32_t msglen;
    uint32_t saltlen;
    uint64_t sig_phys;
    uint64_t n_phys;
    uint64_t prk_phys;
    uint64_t msg_phys;
    uint64_t salt_phys;
} pufs_rsa_sign_t;

typedef struct {
    uint8_t rsamode;
    uint8_t rsatype;
    uint8_t hashtype;
    uint32_t puk;
    uint32_t msglen;
    uint64_t sig_phys;
    uint64_t n_phys;
    uint64_t msg_phys;
} pufs_rsa_verify_t;

#endif /* __DRV_PUFS__ */
