/**
 * @file      pufs_ka.c
 * @brief     PUFsecurity KA API implementation
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
#include "pufs_rt_internal.h"
#include "pufs_ka_internal.h"

struct pufs_ka_regs* ka_regs = NULL;
struct pufs_kwp_regs* kwp_regs = NULL;

/*****************************************************************************
 * Macros
 ****************************************************************************/
#define IV_BLOCK_SIZE 16
#define SK_SIZE 4
#define PK_SIZE 4
#define SS_SIZE 4

/*****************************************************************************
 * Static functions
 ****************************************************************************/
/**
 * @brief check session/secret key slot in KeyArray by key length and registers
 *
 * @param[in] valid    Check the KA register to ensure the key is valid if true.
 * @param[in] slot     The session/secret key slot which the key is stored in.
 * @param[in] keybits  The key length in bits.
 * @return             SUCCESS on success, otherwise an error code.
 */
static pufs_status_t ka_skslot_check(bool valid, pufs_ka_slot_t slot,
    uint32_t keybits)
{
    // check keybits
    switch (slot) {
    case SK128_0:
    case SK128_1:
    case SK128_2:
    case SK128_3:
    case SK128_4:
    case SK128_5:
    case SK128_6:
    case SK128_7:
        if (keybits > 128)
            return E_OVERFLOW;
        break;
    case SK256_0:
    case SK256_1:
    case SK256_2:
    case SK256_3:
        if (keybits > 256)
            return E_OVERFLOW;
        else if ((keybits <= 128) && (keybits != 0))
            return E_UNDERFLOW;
        break;
    case SK512_0:
    case SK512_1:
        if (keybits > 512)
            return E_OVERFLOW;
        else if ((keybits <= 256) && (keybits != 0))
            return E_UNDERFLOW;
        break;
    default:
        return E_INVALID;
    }

    // check registers
    if (valid) {
        uint32_t key_info;
        uint32_t idx;
        uint32_t tagbase;

        switch (slot) {
        case SK128_0:
        case SK128_1:
        case SK128_2:
        case SK128_3:
        case SK128_4:
        case SK128_5:
        case SK128_6:
        case SK128_7:
            idx = slot - SK128_0;
            tagbase = 0x30;
            key_info = ka_regs->sk[idx];
            if (((key_info & SK_KEY_VAILD_MASK) == 0) || (((key_info & SK_KEY_SIZE_MASK) >> 4) != keybits) || (((key_info & SK_KEY_TAG_MASK) >> 16) != (tagbase + idx)))
                return E_INVALID;
            break;
        case SK256_0:
        case SK256_1:
        case SK256_2:
        case SK256_3:
            idx = slot - SK256_0;
            tagbase = 0x50;
            key_info = ka_regs->sk[2 * idx];
            if (((key_info & SK_KEY_VAILD_MASK) == 0) || (((key_info & SK_KEY_SIZE_MASK) >> 4) != keybits) || (((key_info & SK_KEY_TAG_MASK) >> 16) != (tagbase + idx)) || (ka_regs->sk[2 * idx + 1] != ((tagbase + idx) << 16)))
                return E_INVALID;
            break;
        case SK512_0:
        case SK512_1:
            idx = slot - SK512_0;
            tagbase = 0x60;
            key_info = ka_regs->sk[4 * idx];
            if (((key_info & SK_KEY_VAILD_MASK) == 0) || (((key_info & SK_KEY_SIZE_MASK) >> 4) != keybits) || (((key_info & SK_KEY_TAG_MASK) >> 16) != (tagbase + idx)) || (ka_regs->sk[4 * idx + 1] != ((tagbase + idx) << 16)) || (ka_regs->sk[4 * idx + 2] != ((tagbase + idx) << 16)) || (ka_regs->sk[4 * idx + 3] != ((tagbase + idx) << 16)))
                return E_INVALID;
            break;
        default:
            return E_INVALID;
        }
    }

    return SUCCESS;
}
/**
 * @brief check private key slot in KeyArray
 *
 * @param[in] valid    Check the KA register to ensure the key is valid if true.
 * @param[in] slot     The private key slot which the key is stored in.
 * @param[in] keybits  The key length in bits.
 * @return SUCCESS on success, otherwise an error code.
 */
static pufs_status_t ka_prkslot_check(bool valid, pufs_ka_slot_t slot,
    uint32_t keybits)
{
    // check keybits
    if ((slot < PRK_0) || (slot > PRK_2))
        return E_INVALID;
    if (keybits > 576)
        return E_OVERFLOW;

    // check registers
    if (valid) {
        int idx = slot - PRK_0;
        uint32_t ka_prk = ka_regs->pk[idx];

        if (((ka_prk & PK_KEY_VAILD_MASK) == 0) || (((ka_prk & PK_KEY_SIZE_MASK) >> 4) != keybits))
            return E_INVALID;
    }

    return SUCCESS;
}
/**
 * @brief check shared secret slot in KeyArray
 *
 * @param[in] slot  The shared secret slot which the key is stored in.
 * @return          SUCCESS on success, otherwise an error code.
 */
static pufs_status_t ka_ssslot_check(pufs_ka_slot_t slot)
{
    if ((slot < SHARESEC_0) || (slot > SHARESEC_0))
        return E_INVALID;

    return SUCCESS;
}
/**
 * write SK_FREE register to clear keys stored in specific KA slots
 */
static pufs_status_t clear_ka_slot(pufs_ka_slot_t slot)
{
    uint32_t val32;
    switch (slot) {
    case SK128_0:
    case SK128_1:
    case SK128_2:
    case SK128_3:
    case SK128_4:
    case SK128_5:
    case SK128_6:
    case SK128_7:
        val32 = 0x1 << (slot - SK128_0);
        ka_regs->sk_free = val32;
        return SUCCESS;
    case SK256_0:
    case SK256_1:
    case SK256_2:
    case SK256_3:
        val32 = 0x3 << (2 * (slot - SK256_0));
        ka_regs->sk_free = val32;
        return SUCCESS;
    case SK512_0:
    case SK512_1:
        val32 = 0xf << (4 * (slot - SK512_0));
        ka_regs->sk_free = val32;
        return SUCCESS;
    case PRK_0:
    case PRK_1:
    case PRK_2:
        val32 = 0x1 << (slot - PRK_0);
        ka_regs->pk_free = val32;
        return SUCCESS;
    case SHARESEC_0:
        val32 = 0x1 << (slot - SHARESEC_0);
        ka_regs->ss_free = val32;
        return SUCCESS;
    default:
        return E_INVALID;
    }
}
/**
 * @brief Starting KWP and wait until done
 *
 * @return KWP execution status
 */
static pufs_status_t pufs_kwp_start(void)
{
    uint32_t val32;
    // ka_cfg_print();
    // kwp_cfg_print();
    kwp_regs->start = 0x1;
    // ka_cfg_print();
    // kwp_cfg_print();
    while (((val32 = kwp_regs->status) & KWP_STATUS_BUSY_MASK) != 0)
        ;

    if (val32 & (0x1 << 2))
        return E_DENY;
    else if (val32 & (0x1 << 3))
        return E_OVERFLOW;
    else if (val32 & (0x1 << 4))
        return E_UNDERFLOW;
    else if (val32 & (0x1 << 5))
        return E_VERFAIL;
    else if (val32 != 0) {
        LOG_ERROR("KWP status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    return SUCCESS;
}

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * _keyslot_check()
 */
pufs_status_t _keyslot_check(bool valid, pufs_key_type_t keytype, uint32_t slot,
    uint32_t keybits)
{
    switch (keytype) {
    case SWKEY:
        return E_INVALID;
    case OTPKEY:
        return otpkey_slot_check((pufs_rt_slot_t)slot, keybits);
    case PUFKEY:
        return puf_slot_check((pufs_rt_slot_t)slot);
    case RANDKEY:
        return (keybits > 2047) ? E_INVALID : SUCCESS;
    case SHARESEC:
        return ka_ssslot_check((pufs_ka_slot_t)slot);
    case SSKEY:
        return ka_skslot_check(valid, (pufs_ka_slot_t)slot, keybits);
    case PRKEY:
        return ka_prkslot_check(valid, (pufs_ka_slot_t)slot, keybits);
    default:
        return E_INVALID;
    }
}
/**
 * get_key_slot_idx()
 */
int get_key_slot_idx(pufs_key_type_t keytype, uint32_t keyslot)
{
    switch (keytype) {
    case SWKEY:
        return 0;
    case OTPKEY:
        switch ((pufs_rt_slot_t)keyslot) {
        case OTPKEY_0:
        case OTPKEY_1:
        case OTPKEY_2:
        case OTPKEY_3:
        case OTPKEY_4:
        case OTPKEY_5:
        case OTPKEY_6:
        case OTPKEY_7:
        case OTPKEY_8:
        case OTPKEY_9:
        case OTPKEY_10:
        case OTPKEY_11:
        case OTPKEY_12:
        case OTPKEY_13:
        case OTPKEY_14:
        case OTPKEY_15:
        case OTPKEY_16:
        case OTPKEY_17:
        case OTPKEY_18:
        case OTPKEY_19:
        case OTPKEY_20:
        case OTPKEY_21:
        case OTPKEY_22:
        case OTPKEY_23:
        case OTPKEY_24:
        case OTPKEY_25:
        case OTPKEY_26:
        case OTPKEY_27:
        case OTPKEY_28:
        case OTPKEY_29:
        case OTPKEY_30:
        case OTPKEY_31:
            return (keyslot - OTPKEY_0);
        default:
            return -1;
        }
    case PUFKEY:
        switch ((pufs_rt_slot_t)keyslot) {
        case PUFSLOT_1:
        case PUFSLOT_2:
        case PUFSLOT_3:
            return (keyslot - PUFSLOT_1 + 1);
        default:
            return -1;
        }
    case RANDKEY:
        return 0;
    case SHARESEC:
        switch ((pufs_ka_slot_t)keyslot) {
        case SHARESEC_0:
            return (keyslot - SHARESEC_0);
        default:
            return -1;
        }
    case SSKEY:
        switch ((pufs_ka_slot_t)keyslot) {
        case SK128_0:
        case SK128_1:
        case SK128_2:
        case SK128_3:
        case SK128_4:
        case SK128_5:
        case SK128_6:
        case SK128_7:
            return (keyslot - SK128_0);
        case SK256_0:
        case SK256_1:
        case SK256_2:
        case SK256_3:
            return ((keyslot - SK256_0) * 2);
        case SK512_0:
        case SK512_1:
            return ((keyslot - SK512_0) * 4);
        default:
            return -1;
        }
        break;
    case PRKEY:
        switch ((pufs_ka_slot_t)keyslot) {
        case PRK_0:
        case PRK_1:
        case PRK_2:
            return (keyslot - PRK_0);
        default:
            return -1;
        }
    default:
        return -1;
    }
}
/**
 * pufs_get_ss_keybits()
 */
pufs_status_t pufs_get_ss_keybits(uint32_t* keybits, pufs_ka_slot_t keyslot)
{
    pufs_status_t check;
    if ((check = ka_ssslot_check(keyslot)) != SUCCESS)
        return check;

    if ((ka_regs->ss & SS_KEY_VAILD_MASK) == 0)
        return E_INVALID;

    *keybits = (ka_regs->ss & SS_KEY_SIZE_MASK) >> 4;
    return SUCCESS;
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_ka_module_init(uint32_t ka_offset)
{
    ka_regs = (struct pufs_ka_regs*)(pufs_context.base_addr + ka_offset);
    version_check(KA_VERSION, ka_regs->version);
}

void pufs_kwp_module_init(uint32_t kwp_offset)
{
    kwp_regs = (struct pufs_kwp_regs*)(pufs_context.base_addr + kwp_offset);
    version_check(KWP_VERSION, kwp_regs->version);
}
/**
 * pufs_import_plaintext_key()
 */
void kwp_cfg_print(void)
{
    /*
    for(int i=0; i<sizeof(struct pufs_kwp_regs)/4; i++)
    {
        printf("reg[%d] = 0x%x\n", i, *((uint32_t *)kwp_regs+i));
    }
    */
    printf("\nversion=0x%x, interrupt=0x%x, feature=0x%x, status=0x%x, start=0x%x, cfg=0x%x\n",
        kwp_regs->version, kwp_regs->interrupt, kwp_regs->feature, kwp_regs->status, kwp_regs->start,
        kwp_regs->cfg);
    for (int i = 0; i < 4; i++) {
        printf("iv[%d] = 0x%x\n", i, kwp_regs->iv[i]);
    }
    for (int i = 0; i < 20; i++) {
        printf("key[%d] = 0x%x\n", i, kwp_regs->key[i]);
    }
}
void ka_cfg_print(void)
{
    for (int i = 0; i < sizeof(struct pufs_ka_regs) / 4; i++) {
        printf("reg[%d] = 0x%x\n", i, *((uint32_t*)ka_regs + i));
    }
}
pufs_status_t pufs_import_plaintext_key(pufs_key_type_t keytype,
    pufs_ka_slot_t slot, const uint8_t* key,
    uint32_t keybits)
{
    // kwp_cfg_print();
    // ka_cfg_print();
    pufs_status_t check;
    // keytype MUST be either SSKEY or PRKEY
    if ((keytype != SSKEY) && (keytype != PRKEY))
        return E_INVALID;
    // check KA key slot by key length
    if ((check = keyslot_check(false, keytype, slot, keybits)) != SUCCESS)
        return check;

    // Register manipulation
    if (kwp_regs->status & KWP_STATUS_BUSY_MASK)
        return E_BUSY;

    uint32_t val32;

    val32 = 0x0 << 0 | keybits << 8;
    switch (keytype) {
    case SSKEY:
        val32 |= 0x0 << 19;
        break;
    case PRKEY:
        val32 |= 0x1 << 19;
        break;
    default:
        return E_FIRMWARE;
    }
    val32 |= get_key_slot_idx(keytype, slot) << 20;
    kwp_regs->cfg = val32;

    rvv_memset(pufs_buffer, 0, KWP_KEY_MAXLEN);
    rvv_memcpy(pufs_buffer, key, b2B(keybits));

    uint32_t* buf = (uint32_t*)pufs_buffer;
    for (int i = 0; i < KWP_KEY_MAXLEN / 4; ++i)
        kwp_regs->key[i] = be2le(buf[i]);

    // ka_cfg_print();
    //  kwp_cfg_print();
    return pufs_kwp_start();
}
/**
 * @brief Export a plaintext key from Key Array.
 *
 * @param[in] keytype  Key type.
 * @param[in] slot     Key slot.
 * @param[in] key      Key.
 * @param[in] keybits  Key length in bits.
 * @return             SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_export_plaintext_key(
    pufs_key_type_t keytype, pufs_ka_slot_t slot, uint8_t* key,
    uint32_t keybits)
{
    pufs_status_t check;
    // keytype MUST be either SSKEY or PRKEY
    if ((keytype != SSKEY) && (keytype != PRKEY))
        return E_INVALID;
    // check KA key slot by key length
    if ((check = keyslot_check(true, keytype, slot, keybits)) != SUCCESS)
        return check;

    // Register manipulation
    if (kwp_regs->status & KWP_STATUS_BUSY_MASK)
        return E_BUSY;

    uint32_t val32;

    val32 = 0x1 << 0 | keybits << 8;
    switch (keytype) {
    case SSKEY:
        val32 |= 0x0 << 19;
        break;
    case PRKEY:
        val32 |= 0x1 << 19;
        break;
    default:
        return E_FIRMWARE;
    }
    val32 |= get_key_slot_idx(keytype, slot) << 20;
    kwp_regs->cfg = val32;

    if ((check = pufs_kwp_start()) != SUCCESS)
        return check;

    uint32_t* key32 = (uint32_t*)key;
    for (size_t i = 0; i < b2B(keybits) / 4; ++i)
        key32[i] = be2le(kwp_regs->key[i]);

    return SUCCESS;
}
/**
 * @see _pufs_import_wrapped_key().
 */
pufs_status_t _pufs_import_wrapped_key(
    pufs_key_type_t keytype, pufs_ka_slot_t slot, const uint8_t* key,
    uint32_t keybits, pufs_ka_slot_t kekslot, uint32_t kekbits,
    pufs_key_wrap_t keywrap, const uint8_t* iv)
{
    pufs_status_t check;
    // keytype MUST be either SSKEY or PRKEY
    if ((keytype != SSKEY) && (keytype != PRKEY))
        return E_INVALID;
    // check KA key slot by imported key length
    if ((check = keyslot_check(false, keytype, slot, keybits)) != SUCCESS)
        return check;
    // check KA session key slot by KEK length
    if ((check = keyslot_check(true, SSKEY, kekslot, kekbits)) != SUCCESS)
        return check;
    // check if the keywrap algorithm is supported
    if (keywrap >= N_KEYWRAP_T)
        return E_INVALID;

    if (kwp_regs->status & KWP_STATUS_BUSY_MASK)
        return E_BUSY;

    uint32_t val32, feature;
    uint32_t inkeylen = b2B(keybits);
    feature = kwp_regs->feature;

    val32 = 0x2 << 0;
    if ((keywrap == AES_KW_INV) || (keywrap == AES_KWP_INV))
        val32 |= 0x1 << 2;
    switch (keywrap) {
    case AES_CBC_CS2:
        if ((feature & KWP_FEATURE_AES_CBC_CS2_D_MASK) == 0)
            return E_UNSUPPORT;
        val32 |= 0x0 << 4;
        break;
    case AES_KW:
    case AES_KW_INV:
        if ((feature & KWP_FEATURE_AES_KW_D_MASK) == 0)
            return E_UNSUPPORT;
        if ((keybits < 128) || ((keybits % 64) != 0))
            return E_INVALID;
        val32 |= 0x2 << 4;
        inkeylen += 8;
        break;
    case AES_KWP:
    case AES_KWP_INV:
        if ((feature & KWP_FEATURE_AES_KWP_D_MASK) == 0)
            return E_UNSUPPORT;
        if ((keybits % 8) != 0)
            return E_INVALID;
        val32 |= 0x3 << 4;
        inkeylen = 8 * ((inkeylen + 15) / 8);
        break;
    default:
        return E_FIRMWARE;
    }
    val32 |= keybits << 8;
    switch (keytype) {
    case SSKEY:
        val32 |= 0x0 << 19;
        break;
    case PRKEY:
        val32 |= 0x1 << 19;
        break;
    default:
        return E_FIRMWARE;
    }
    val32 |= get_key_slot_idx(keytype, slot) << 20;
    val32 |= get_key_slot_idx(SSKEY, kekslot) << 24;
    switch (kekbits) {
    case 128:
        val32 |= 0x0 << 28;
        break;
    case 192:
        val32 |= 0x1 << 28;
        break;
    case 256:
        val32 |= 0x2 << 28;
        break;
    default:
        return E_FIRMWARE;
    }
    kwp_regs->cfg = val32;

    if (iv != NULL) {
        uint32_t* iv32 = (uint32_t*)iv;
        for (int i = 0; i < IV_BLOCK_SIZE / 4; ++i)
            kwp_regs->iv[i] = be2le(iv32[i]);
    }

    rvv_memset(pufs_buffer, 0, KWP_KEY_MAXLEN);
    rvv_memcpy(pufs_buffer, key, inkeylen);

    uint32_t* buf = (uint32_t*)pufs_buffer;
    for (int i = 0; i < KWP_KEY_MAXLEN / 4; ++i)
        kwp_regs->key[i] = be2le(buf[i]);

    check = pufs_kwp_start();
    if (check == E_VERFAIL) {
        if ((check = pufs_clear_key(keytype, slot, keybits)) != SUCCESS)
            return check;
        return E_VERFAIL;
    }

    return check;
}
/**
 * _pufs_export_wrapped_key()
 */
pufs_status_t _pufs_export_wrapped_key(
    pufs_key_type_t keytype, pufs_ka_slot_t slot, uint8_t* key,
    uint32_t keybits, pufs_ka_slot_t kekslot, uint32_t kekbits,
    pufs_key_wrap_t keywrap, const uint8_t* iv)
{
    pufs_status_t check;
    // keytype MUST be either SSKEY or PRKEY
    if ((keytype != SSKEY) && (keytype != PRKEY))
        return E_INVALID;
    // check KA key slot by exported key length
    if ((check = keyslot_check(true, keytype, slot, keybits)) != SUCCESS)
        return check;
    // check KA session key slot by KEK length
    if ((check = keyslot_check(true, SSKEY, kekslot, kekbits)) != SUCCESS)
        return check;
    // check if the keywrap algorithm is supported
    if (keywrap >= N_KEYWRAP_T)
        return E_INVALID;

    if (kwp_regs->status & KWP_STATUS_BUSY_MASK)
        return E_BUSY;

    uint32_t val32, feature;
    uint32_t outkeylen = b2B(keybits);
    feature = kwp_regs->feature;

    val32 = 0x3 << 0;
    if ((keywrap == AES_KW_INV) || (keywrap == AES_KWP_INV))
        val32 |= 0x1 << 2;
    switch (keywrap) {
    case AES_CBC_CS2:
        if ((feature & KWP_FEATURE_AES_CBC_CS2_E_MASK) == 0)
            return E_UNSUPPORT;
        val32 |= 0x0 << 4;
        break;
    case AES_KW:
    case AES_KW_INV:
        if ((feature & KWP_FEATURE_AES_KW_E_MASK) == 0)
            return E_UNSUPPORT;
        if ((keybits < 128) || ((keybits % 64) != 0))
            return E_INVALID;
        val32 |= 0x2 << 4;
        outkeylen += 8;
        break;
    case AES_KWP:
    case AES_KWP_INV:
        if ((feature & KWP_FEATURE_AES_KWP_E_MASK) == 0)
            return E_UNSUPPORT;
        if ((keybits % 8) != 0)
            return E_INVALID;
        val32 |= 0x3 << 4;
        outkeylen = 8 * ((outkeylen + 15) / 8);
        break;
    default:
        return E_FIRMWARE;
    }
    val32 |= keybits << 8;
    switch (keytype) {
    case SSKEY:
        val32 |= 0x0 << 19;
        break;
    case PRKEY:
        val32 |= 0x1 << 19;
        break;
    default:
        return E_FIRMWARE;
    }
    val32 |= get_key_slot_idx(keytype, slot) << 20;
    val32 |= get_key_slot_idx(SSKEY, kekslot) << 24;
    switch (kekbits) {
    case 128:
        val32 |= 0x0 << 28;
        break;
    case 192:
        val32 |= 0x1 << 28;
        break;
    case 256:
        val32 |= 0x2 << 28;
        break;
    default:
        return E_FIRMWARE;
    }

    kwp_regs->cfg = val32;

    if (iv != NULL) {
        uint32_t* iv32 = (uint32_t*)iv;
        for (int i = 0; i < IV_BLOCK_SIZE / 4; ++i)
            kwp_regs->iv[i] = be2le(iv32[i]);
    }

    if ((check = pufs_kwp_start()) != SUCCESS)
        return check;

    uint32_t* key32 = (uint32_t*)key;
    for (size_t i = 0; i < outkeylen / 4; ++i)
        key32[i] = be2le(kwp_regs->key[i]);

    return SUCCESS;
}
/**
 * pufs_clear_key()
 */
pufs_status_t pufs_clear_key(pufs_key_type_t keytype, pufs_ka_slot_t slot,
    uint32_t keybits)
{
    pufs_status_t check;
    // keytype MUST be one of SSKEY, PRKEY, or SHARESEC
    if ((keytype != SSKEY) && (keytype != PRKEY) && (keytype != SHARESEC))
        return E_INVALID;
    // check KA key slot by key length
    if ((check = keyslot_check(false, keytype, slot, keybits)) != SUCCESS)
        return check;

    return clear_ka_slot(slot);
}
