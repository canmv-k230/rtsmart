/* Exercise the ioctl and KWP register copies with fake MMIO. */
#include <assert.h>
#include <errno.h>
#include <string.h>

#include "libc_ioctl.h"
#include "drv_pufs.h"
#include "pufs_crypto_regs.h"
#include "pufs_ecp.h"
#include "pufs_internal.h"
#include "pufs_ka_internal.h"

static unsigned int locks;
static size_t copied_in, copied_out;
static int lock_error;

static int pufs_hw_lock(void) { locks++; return lock_error; }
static void pufs_hw_unlock(void) {}
#define LOG_D(...) ((void)0)

static int get_from(void *dst, void *src, size_t n)
{
    assert(src);
    copied_in = n;
    memcpy(dst, src, n);
    return 0;
}

static int put_to(void *dst, void *src, size_t n)
{
    assert(dst);
    copied_out = n;
    memcpy(dst, src, n);
    return 0;
}

pufs_status_t pufs_ecp_set_curve_byname(pufs_ec_name_t name)
{
    assert(name == NISTP384);
    return SUCCESS;
}

pufs_status_t pufs_ecp_gen_eprk(pufs_ka_slot_t slot)
{
    assert(slot == PRK_1);
    return E_ECMPROG;
}

pufs_status_t _pufs_ecp_gen_sprk(pufs_ka_slot_t slot, pufs_key_type_t type,
                                 size_t keyaddr, uint32_t keybits, const uint8_t *salt,
                                 uint32_t saltlen, const uint8_t *info, uint32_t infolen,
                                 pufs_hash_t hash)
{
    assert(0);
    return E_ERROR;
}

#include "key_io.inc"
#include "status_copies.inc"

static int status_ioctl(int cmd, void *args)
{
    int ret;

    switch (cmd) {
#include "status_cases.inc"
    default:
        return -EINVAL;
    }
    return ret;
}

int main(void)
{
    struct pufs_ka_regs ka = { 0 };
    struct pufs_kwp_regs kwp = { 0 };
    uint8_t blob[KWP_KEY_MAXLEN + 1];
    pufs_key_io_t op = {
        .keytype = PRKEY, .keyslot = PRK_0, .keybits = 576,
        .kwslot = SK128_0, .kwbits = 128,
    };
    ka_regs = &ka;
    kwp_regs = &kwp;
    ka.sk[0] = (0x30u << 16) | (128u << 4) | SK_KEY_VAILD_MASK;
    kwp.feature = KWP_FEATURE_AES_KW_E_MASK | KWP_FEATURE_AES_KW_D_MASK |
                  KWP_FEATURE_AES_KWP_E_MASK | KWP_FEATURE_AES_KWP_D_MASK |
                  KWP_FEATURE_AES_CBC_CS2_E_MASK;

    for (op.mode = KM_IMPORT_PT; op.mode <= KM_EXPORT_WRAP; op.mode++) {
        assert(key_io(&op, false) == -EFAULT);
        assert(key_io(&op, true) == -EFAULT);
    }
    assert(locks == 0);

    op.mode = KM_IMPORT_WRAP;
    op.keyaddr = blob;
    op.keywrap = AES_KW;
    op.keybits = UINT32_MAX;
    assert(key_io(&op, false) == -EINVAL && locks == 0);
    op.keybits = 576;
    memset(blob, 0x4a, sizeof(blob));
    blob[KWP_KEY_MAXLEN - 1] = 0x7f;
    assert(key_io(&op, false) == 0 && copied_in == KWP_KEY_MAXLEN);
    assert(kwp.key[KWP_KEY_MAXLEN / 4 - 1] == 0x4a4a4a7f);

    op.mode = KM_EXPORT_WRAP;
    ka.pk[0] = (576u << 4) | PK_KEY_VAILD_MASK;
    memset(blob, 0, sizeof(blob));
    assert(key_io(&op, true) == 0 && copied_out == KWP_KEY_MAXLEN);
    assert(blob[KWP_KEY_MAXLEN - 1] == 0x7f && blob[KWP_KEY_MAXLEN] == 0);

    op.mode = KM_IMPORT_WRAP;
    op.keywrap = AES_KWP_INV;
    op.keybits = 520;
    memset(blob, 0x3b, sizeof(blob));
    assert(key_io(&op, true) == 0 && copied_in == KWP_KEY_MAXLEN);
    op.mode = KM_EXPORT_WRAP;
    ka.pk[0] = (520u << 4) | PK_KEY_VAILD_MASK;
    memset(blob, 0, sizeof(blob));
    assert(key_io(&op, true) == 0 && copied_out == KWP_KEY_MAXLEN);
    assert(blob[KWP_KEY_MAXLEN - 1] == 0x3b && blob[KWP_KEY_MAXLEN] == 0);

    op.keywrap = AES_CBC_CS2;
    assert(key_io(&op, false) == 0 && copied_out == 65 && blob[64] == 0x3b);
    kwp.feature = 0;
    copied_out = 0;
    assert(status_ioctl(PUFS_KEY_INOUT, &op) == -E_UNSUPPORT && copied_out == 0);
    assert(status_ioctl(PUFS_KEY_INOUT_STATUS, &op) == E_UNSUPPORT && copied_out == 0);
    op.mode = KM_EXPORT_PT;
    assert(key_io(&op, false) == 0 && copied_out == 65 && blob[64] == 0x3b);
    puts("key I/O: NULL buffers refused, complete wrapped and partial-word copies: PASS");

    pufs_ecc_prk_gen_t ecc = { .ecctype = NISTP384, .is_ephemeral = 1, .prkslot = PRK_1 };
    assert(status_ioctl(PUFS_ECC_PRK_GEN, &ecc) == -E_ECMPROG);
    assert(status_ioctl(PUFS_ECC_PRK_GEN_STATUS, &ecc) == E_ECMPROG);
    assert(status_ioctl(PUFS_KEY_INOUT_STATUS, NULL) == -EFAULT);
    lock_error = -EBUSY;
    assert(status_ioctl(PUFS_ECC_PRK_GEN_STATUS, &ecc) == -EBUSY);
    assert(status_ioctl(PUFS_KEY_INOUT_STATUS, &op) == -EBUSY);
    puts("ECC keygen and key I/O: opt-in positive hardware status, negative OS errors: PASS");
    return 0;
}
