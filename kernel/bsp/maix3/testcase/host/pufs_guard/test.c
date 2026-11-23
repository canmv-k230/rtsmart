/* Exercise the production guard, direct/SG submission and ECP start with fake MMIO. */
#include <assert.h>
#include <errno.h>
#include "sbi.h"
#include "pufs_ecp.c"
#include "pufs_dma_internal.h"

#include "drv_pufs.h"
#include "pufs_hmac_internal.h"
#include "pufs_sp90a_internal.h"

static struct pufs_rt_regs rt;
static struct pufs_dma_regs dma;
static struct pufs_ecp_regs ecc;
static struct pufs_rt_cde_regs cde;
/* Same size as the driver's descriptor pool: 32 descriptors of 32 bytes. */
static pufs_dma_sg_internal_desc_st descriptors[32];
static int writable_calls, deny_from, deny_restore, hold_busy, hold_ecc_busy;
static unsigned int cache_ops;
static int deny_cde_restore, emulate_sg_copy;
static uint8_t source[64] __attribute__((aligned(64)));
static uint8_t dest[64] __attribute__((aligned(64)));
static uint32_t observed_mask;

/* Fake physical bases, so the PMP model can tell the RT and CDE windows apart. */
#define RT_PHYS  0x1000u
#define CDE_PHYS 0x2000u
static unsigned long pmp_perm[2];   /* [0] RT window, [1] CDE window */
static int pmp_rw_opens[2];

struct sbi_ret sbi_pmp_set(uintptr_t addr, size_t len, unsigned long perm)
{
    int w = addr == CDE_PHYS;

    if (perm & SBI_PMP_PERM_W) {
        if (++writable_calls >= deny_from && deny_from)
            return (struct sbi_ret){ SBI_ERR_DENIED };
        pmp_rw_opens[w]++;
    } else {
        if (deny_restore || (w && deny_cde_restore))
            return (struct sbi_ret){ SBI_ERR_DENIED };
        if (rt.otp_psmsk[0] != UINT32_MAX)
            observed_mask = rt.otp_psmsk[0];
    }
    pmp_perm[w] = perm;
    return (struct sbi_ret){ SBI_SUCCESS };
}

rt_tick_t rt_tick_get(void)
{
    static rt_tick_t tick;
    if (emulate_sg_copy && dma.start) {
        pufs_dma_sg_internal_desc_st *d = (void *)(uintptr_t)dma.dsc_cfg_3;
        memmove((void *)(uintptr_t)be2le(d->read_addr),
                (void *)(uintptr_t)be2le(d->write_addr), be2le(d->length));
        emulate_sg_copy = 0;
    }
    if (hold_busy && dma.start)
        dma.status_0 = DMA_STATUS_0_BUSY_MASK;
    if (hold_ecc_busy && ecc.ctrl)
        ecc.status = ECP_STATUS_BUSY_MASK;
    return tick++;
}

void rt_hw_cpu_dcache_clean(void *addr, size_t len) { cache_ops++; }

static int lock_released;

/* By default RN is stuck; advance it on a fixed number of reads when asked. */
static rt_device_t registered_hwrng;
static unsigned int fresh_words;

void test_rng_word(void)
{
    if (fresh_words) {
        rt.rn++;
        fresh_words--;
    }
}

rt_err_t rt_device_register(rt_device_t dev, const char *name, rt_uint16_t flags)
{
    (void)flags;
    if (strcmp(name, "hwrng") == 0)
        registered_hwrng = dev;
    return RT_EOK;
}
int pufs_drv_hw_lock(void) { return 0; }
void pufs_drv_hw_unlock(void) {}

#include "drv_pufs_internal.h"

/*
 * /dev/hwrng (drv_hwrng.c). The fake RN register returns the same word until
 * the test changes it, which is exactly what a stuck RNG looks like.
 */
static void hwrng_tests(void)
{
    uint8_t buf[64];
    uint32_t word;

    rt.cfg = 0x1;
    pmp_rw_opens[0] = 0;
    deny_from = writable_calls + 1;
    assert(pufs_hwrng_init() == -RT_EIO && !registered_hwrng && !(rt.cfg & 0x2));
    assert(pufs_rng_fill(buf, 1) == -EIO);
    deny_from = 0;

    deny_restore = 1;
    assert(pufs_hwrng_init() == -RT_EIO && !registered_hwrng && (rt.cfg & 2));
    assert(pmp_perm[0] & SBI_PMP_PERM_W);
    assert(pufs_hwrng_init() == -RT_EIO && !registered_hwrng);
    deny_restore = 0;

    /* A stuck startup sample fails reads but leaves the provider available. */
    assert(pufs_hwrng_init() == RT_EOK && registered_hwrng);
    assert((rt.cfg & 0x2) && pmp_rw_opens[0] == 1 && !(pmp_perm[0] & SBI_PMP_PERM_W));
    assert(registered_hwrng->ops->read(registered_hwrng, 0, buf, 1) == (rt_size_t)-EIO);
    assert(buf[0] == 0);
    fresh_words = 1;
    assert(registered_hwrng->ops->read(registered_hwrng, 0, buf, sizeof(word)) == sizeof(word));
    word = rt.rn;
    assert(memcmp(buf, &word, sizeof(word)) == 0);
    assert(pmp_rw_opens[0] == 1);
    assert(registered_hwrng->ops->read(registered_hwrng, 0, buf, 0) == 0);

    /* Tail bytes come from the next word, in memory order. */
    rt.rn = 0x11223343;
    fresh_words = 2;
    memset(buf, 0, sizeof(buf));
    assert(registered_hwrng->ops->read(registered_hwrng, 0, buf, 6) == 6);
    word = 0x11223344;
    assert(memcmp(buf, &word, 4) == 0);
    word++;
    assert(memcmp(buf + 4, &word, 2) == 0 && buf[6] == 0);

    /* Even a one-byte read must not get another copy of the previous word. */
    memset(buf, 0xa5, sizeof(buf));
    assert(pufs_rng_fill(buf, 1) == -EIO && buf[0] == 0 && buf[1] == 0xa5);
    assert(registered_hwrng->ops->read(registered_hwrng, 0, buf, 16) == (rt_size_t)-EIO);

    /* A new word ends the run: reads work again. */
    rt.rn = 0x55667788;
    assert(pufs_rng_fill(buf, 4) == 0);
    word = 0x55667788;
    assert(memcmp(buf, &word, 4) == 0);
    fresh_words = 1;
    assert(pufs_rng_fill(buf, 8) == -EIO);
    for (size_t i = 0; i < 8; i++)
        assert(buf[i] == 0);
    fresh_words = 7;
    assert(registered_hwrng->ops->read(registered_hwrng, 0, buf, 28) == 28);
    puts("hwrng: clock enable, tail bytes, stuck detection and recovery: PASS");
}
#ifdef RT_PUFS_OTP_WRITE_ENABLE
static const int write_build = 1;
#else
static const int write_build = 0;
#endif
bool pufs_drv_hw_lock_held(void) { return !lock_released; }

/* pufs_ka.c is not linked: the ECDH refusal runs before any slot lookup. */
pufs_status_t _keyslot_check(bool valid, pufs_key_type_t keytype, uint32_t slot,
    uint32_t keybits) { return SUCCESS; }
int get_key_slot_idx(pufs_key_type_t keytype, uint32_t keyslot) { return 0; }
void rt_hw_cpu_dcache_invalidate(void *addr, size_t len) { cache_ops++; }

/*
 * RT and CDE programming through the unified API: byte order, refusals, lock
 * fields, PGM_IGN and PMP windows. The fake OTP stores what is written (real
 * cells OR it in), so every programmed word here starts blank or is written
 * as a superset of its current bits.
 */
static void program_tests(void)
{
#ifdef RT_PUFS_OTP_WRITE_ENABLE
    const int burn = 1;
#else
    const int burn = 0;
#endif
    const uint8_t rt_bytes[8] = { 1, 2, 3, 4, 0x10, 0x20, 0x30, 0x40 };
    const uint8_t cde_bytes[4] = { 1, 2, 3, 4 };
    const uint8_t half[2] = { 0xaa, 0xbb }, zero[4] = { 0 }, low[4] = { 0, 0, 0, 1 };
    uint32_t masks[4] = { rt.otp_psmsk[0], rt.otp_psmsk[1], rt.ptm[0], rt.ptm[1] };
    volatile uint32_t *cde_word = &cde.otp[(0xff8 - OTP_LEN) / 4];

    /* Zeroed lock fields decode as RW (PUFRT_DEFAULT_ZERO). */
    memset((void *)rt.otp, 0, sizeof(rt.otp));
    memset((void *)cde.otp, 0, sizeof(cde.otp));
    memset((void *)rt.pif, 0, sizeof(rt.pif));
    rt.cfg = 0;
    pmp_rw_opens[0] = pmp_rw_opens[1] = 0;

    /* Nothing to change: no PMP window and no PGM_IGN. */
    assert(pufs_otp_write(zero, 4, 0x3f8) == SUCCESS);
    assert(pufs_otp_write(zero, 4, 0xff8) == SUCCESS);
    assert(rt.cfg == 0 && pmp_rw_opens[1] == 0);

    /* RT packs byte 0 into the high byte; CDE stores bytes in memory order. */
    assert(pufs_otp_write(rt_bytes, 8, 0x3f8) == SUCCESS);
    assert(rt.otp[254] == (burn ? 0x01020304u : 0));
    assert(rt.otp[255] == (burn ? 0x10203040u : 0));
    assert(!!(rt.cfg & PTM_CFG_REG_PGM_IGN_MASK) == burn);
    assert(pufs_otp_write(half, 2, 0x3f0) == SUCCESS);
    assert(rt.otp[252] == (burn ? 0xaabb0000u : 0));
    rt.cfg = 0;
    assert(pufs_otp_write(cde_bytes, 4, 0xff8) == SUCCESS);
    assert(*cde_word == (burn ? 0x04030201u : 0));
    assert(!!(rt.cfg & PTM_CFG_REG_PGM_IGN_MASK) == burn);
    assert(pufs_otp_write(half, 2, 0xff0) == SUCCESS);
    assert(cde.otp[(0xff0 - OTP_LEN) / 4] == (burn ? 0x0000bbaau : 0));
    assert(pmp_rw_opens[1] == (burn ? 2 : 0));

    if (burn) {
        uint8_t scratch;
        deny_cde_restore = 1;
        assert(pufs_otp_write(low, 4, 0xfe0) == E_DENY);
        assert(pmp_perm[1] & SBI_PMP_PERM_W);
        assert(pufs_otp_read(&scratch, 1, 32) == E_DENY);
        deny_cde_restore = 0;
        assert(pufs_otp_read(&scratch, 1, 32) == SUCCESS);
        assert(!(pmp_perm[1] & SBI_PMP_PERM_W));
    }

    /* A bit 1->0 is refused in both areas, before anything is written. */
    rt.otp[253] = 0x80000000u;
    cde.otp[(0xfe8 - OTP_LEN) / 4] = 0x80u;
    assert(pufs_otp_write(low, 4, 0x3f4) == E_DENY && rt.otp[253] == 0x80000000u);
    assert(pufs_otp_write(low, 4, 0xfe8) == E_DENY &&
           cde.otp[(0xfe8 - OTP_LEN) / 4] == 0x80u);

    /* Locked RT word 0x3e8 (field 250) and CDE segment 22 (0xf00-0xf7f). */
    rt.pif[PIF_RWLCK_START_INDEX + 31] = 0x3u << 8;
    rt.pif[PIF_CDE_RWLCK_START_INDEX + 2] = 0x3u << 24;
    assert(pufs_otp_write(low, 4, 0x3e8) == E_DENY && rt.otp[250] == 0);
    assert(pufs_otp_write(low, 4, 0xf00) == E_DENY && cde.otp[(0xf00 - OTP_LEN) / 4] == 0);
    /* A write that spans a locked word programs none of the range. */
    assert(pufs_otp_write(rt_bytes, 8, 0x3e4) == E_DENY && rt.otp[249] == 0);
    /* The BROM patch area is never written. */
    assert(pufs_otp_write(low, 4, 0x800) == E_DENY);

    /* Lock fields only tighten: RW -> RO sets the RO encoding in place. */
    assert(pufs_otp_set_lock(0x3fc, 4, RO) == SUCCESS);
    assert(rt.pif[PIF_RWLCK_START_INDEX + 31] == ((0x3u << 8) | (burn ? 0x3u << 28 : 0)));
    assert(pufs_otp_set_lock(0xf80, 4, RO) == SUCCESS);
    assert(rt.pif[PIF_CDE_RWLCK_START_INDEX + 2] == ((0x3u << 24) | (burn ? 0x3u << 28 : 0)));

    /* Every window is read-only again and the guard re-masked everything. */
    assert(!(pmp_perm[0] & SBI_PMP_PERM_W) && !(pmp_perm[1] & SBI_PMP_PERM_W));
    assert(rt.otp_psmsk[0] == masks[0] && rt.otp_psmsk[1] == masks[1] &&
           rt.ptm[0] == masks[2] && rt.ptm[1] == masks[3]);
    memset((void *)rt.pif, 0, sizeof(rt.pif));
    puts(burn ? "RT/CDE programming: PASS" : "RT/CDE programming dry-run: PASS");
}

#define PUFS_DMA_BUF_SIZE 65536
static uint8_t pufs_dma_in[PUFS_DMA_BUF_SIZE];
#define pufs_hw_lock pufs_drv_hw_lock
#define pufs_hw_unlock pufs_drv_hw_unlock
static int get_from(void *dst, void *src, size_t n) { memcpy(dst, src, n); return 0; }
#define put_to get_from
#include "hmac_handler.inc"

static void abi_tests(void)
{
    static struct pufs_hmac_regs hm = {.feature = UINT32_MAX};
    static struct pufs_drbg_regs drbg = {.feature = SP90A_FEATURE_CTR_DRBG_MASK | SP90A_FEATURE_AES_128_MASK};
    uint8_t key[20], msg[] = "Hi There";
    hmac_regs = &hm;
    pufs_hmac_op_t op = {.op = PUFS_OP_INIT, .hash = SHA_256, .keytype = SWKEY, .keybits = 160};
    memset(key, 0x0b, sizeof(key));
    memcpy(op.ctx.key, key, sizeof(key));
    assert(hmac_op_handler(&op) == 0 && !memcmp(key, op.ctx.key, sizeof(key)));
    op.ctx.hash = SHA_512;
    op.ctx.blocklen = op.ctx.minlen = 128;
    op.op = PUFS_OP_UPDATE;
    op.msglen = sizeof(msg) - 1;
    op.msg_phys = (uintptr_t)msg;
    assert(hmac_op_handler(&op) == 0 && op.ctx.minlen == 1);
    assert(op.ctx.buflen == op.msglen && !memcmp(op.ctx.buff, msg, op.msglen));

    /* Before RNG startup, neither production derivation may issue a command. */
    drbg_regs = &drbg;
    assert(pufs_drbg_instantiate(AES_CTR_DRBG, 128, true, NULL, 0, NULL, 0) == E_ERROR && !drbg.start);
    drbg.status_1 = SP90A_STATUS_1_INSTANT_MASK;
    assert(pufs_drbg_reseed(true, NULL, 0) == E_ERROR && !drbg.start);
    puts("raw HMAC key, legacy SHA-512 context, missing DRBG entropy: PASS");
}

int main(void)
{
    uint8_t out = 0xa5;
    rt_regs = &rt;
    rt_cde_regs = &cde;
    rt_regs_phys = RT_PHYS;
    rt_cde_regs_phys = CDE_PHYS;
    pufs_dma.regs = &dma;
    ecp_regs = &ecc;
    abi_tests();
    rt.otp_psmsk[0] = 1;  /* A partially set firmware mask still denies access. */
#ifdef RT_PUFS_OTP_POSTMASK_GUARD_DISABLE
    assert(pufs_otp_guard_init() == SUCCESS);
    assert(pufs_otp_read(&out, 1, 0) == E_DENY && out == 0xa5);
    assert(pufs_otp_read(&out, 1, 32) == SUCCESS && out == 0);
    assert(rt_cde_write_mask(128) == SUCCESS);
    assert(pufs_otp_read(&out, 1, OTP_LEN + 128) == E_DENY);
    program_tests();
    hwrng_tests();
    puts("disabled guard: PASS");
#else
    /* Registers locked before boot: follow the masks, leave them unchanged. */
    rt.puf_psmsk = PMK_LCK_PSMSK_MASK;
    assert(pufs_otp_guard_init() == SUCCESS);
    assert(pufs_otp_read(&out, 1, 0) == E_DENY && out == 0xa5);
    assert(pufs_otp_read(&out, 1, 32) == SUCCESS && out == 0);
    assert(rt.otp_psmsk[0] == 1 && rt.otp_psmsk[1] == 0 && rt.ptm[0] == 0);
    pufs_otp_postmask_st st;
    pufs_otp_get_postmask(&st);
    assert(st.guard_locked && !st.guard_active && st.policy_otp[0] == 1);
    /* Locked registers are never written: requests either already hold or fail. */
    int opens = pmp_rw_opens[0];
    assert(rt_cde_write_mask(256) == E_DENY && rt.ptm[0] == 0);
    assert(pufs_post_mask(0) == SUCCESS);
    assert(pufs_post_mask(MASK_BIT(OTPKEY_1)) == (write_build ? E_DENY : SUCCESS));
    assert(pmp_rw_opens[0] == opens && rt.otp_psmsk[0] == 1);
    pufs_otp_get_postmask(&st);
    assert(st.policy_cde[0] == 0 && st.policy_otp[0] == 1);
    out = 0xa5;
    rt.puf_psmsk = 0;
    deny_restore = 1;
    assert(pufs_otp_guard_init() == E_DENY);
    assert(pufs_otp_read(&out, 1, OTP_LEN) == E_DENY && out == 0xa5);
    /* A failed guard never locks anything in. */
    assert(pufs_post_mask(MASK_BIT(OTPKEY_5)) == (write_build ? E_DENY : SUCCESS));
    assert(!(rt.puf_psmsk & PMK_LCK_PSMSK_MASK));
    /* Model the next cold boot. */
    deny_restore = 0;
    rt.otp_psmsk[0] = 1;
    rt.otp_psmsk[1] = rt.ptm[0] = rt.ptm[1] = 0;
    assert(pufs_otp_guard_init() == SUCCESS);
    assert(pufs_otp_read(&out, 1, 0) == E_DENY && out == 0xa5);
    lock_released = 1;
    assert(pufs_otp_read(&out, 1, 32) == E_DENY && out == 0xa5);
    assert(rt_cde_write_mask(128) == E_DENY);
    assert(rt.otp_psmsk[0] == UINT32_MAX && rt.ptm[0] == UINT32_MAX);
    /* No guard involved (PUF slot), but the RT window still needs the lock. */
    assert(pufs_zeroize(PUFSLOT_1) == (write_build ? E_DENY : SUCCESS) && rt.puf_zeroize == 0);
    lock_released = 0;
    assert(rt_cde_write_mask(128) == SUCCESS);
    assert(rt_otp_guard(OTP_LEN + 124, 8, true) == E_DENY);
    assert(rt.ptm[0] == UINT32_MAX);

    /* The engine must not start for a policy-masked key. */
    dma_write_key_config_0(OTPKEY, 0, 256, 0);
    assert(dma_run() == E_DENY && !dma.start);
    uint32_t status;
    ecp_select_otp_key(0, 256);
    assert(pufs_ecc_start(&status) == E_DENY && !ecc.ctrl);

    /* Refuse the PMP write for close and keep refusing subsequent retries. */
    dma_write_key_config_0(OTPKEY, 0, 256, 2);
    deny_from = writable_calls + 2;
    assert(dma_run() == E_DENY && dma.start);
    assert((rt.otp_psmsk[0] & 0x30) == 0);
    ecc.ctrl = 0;
    ecp_select_otp_key(3, 256);
    assert(pufs_ecc_start(&status) == E_DENY && !ecc.ctrl);
    deny_from = 0;
    assert(pufs_otp_read(&out, 1, 32) == SUCCESS);
    assert(rt.otp_psmsk[0] == UINT32_MAX);
    ecp_select_otp_key(3, 256);
    deny_from = writable_calls + 2;
    assert(pufs_ecc_start(&status) == E_DENY && ecc.ctrl);
    deny_from = 0;

    /* A complete direct run between SG queueing and submission must use
     * only its own key; the SG run must still open both descriptor slots. */
    pufs_dma_sg_init((uintptr_t)descriptors);
    for (int i = 4; i <= 5; i++) {
        pufs_dma_sg_internal_desc_st *d = dma_sg_new_data_descriptor();
        assert(d);
        dma_sg_desc_write_key_config(d, OTPKEY, 0, 256, i);
        dma_sg_desc_append_to_list(d);
    }
    assert(dma_run() == SUCCESS);
    assert(observed_mask == (UINT32_MAX & ~0x30u));
    assert(pufs_dma_sg_start() == SUCCESS);
    assert(observed_mask == (UINT32_MAX & ~0xf00u));
    assert(rt.otp_psmsk[0] == UINT32_MAX);

    /* An oversized key size is masked to its field: it cannot spill into the
     * slot index and select (and open) a different OTP key slot. */
    dma_write_key_config_0(OTPKEY, 0, 1u << 16, 2);
    assert(((dma.key_cfg_0 & DMA_KEY_CFG_0_KEY_IDX_MASK) >> DMA_KEY_CFG_0_KEY_IDX_BITS) == 2);
    assert(dma_run() == SUCCESS && observed_mask == (UINT32_MAX & ~0x30u));

    /* DMA is still writing after timeout. Neither scrub nor reuse is safe. */
    assert(dma_write_rwcfg(&out, NULL, 1) == SUCCESS);
    uint8_t *bounce = (uint8_t *)pufs_dma.read_virt_addr;
    *bounce = 0xa5;
    out = 0x33;
    cache_ops = 0;
    hold_busy = 1;
    assert(dma_run() == E_BUSY);
    assert(cache_ops == 0 && *bounce == 0xa5 && out == 0x33);
    assert(dma_write_rwcfg(NULL, NULL, 0) == E_BUSY);
    *bounce = 0x77; /* Late device write. */
    hold_busy = 0;
    dma.status_0 = 0;
    assert(dma_write_rwcfg(NULL, NULL, 0) == SUCCESS);
    assert(*bounce == 0 && out == 0x33 && cache_ops == 2);

    source[0] = 0x56;
    dest[0] = 0x34;
    assert(dma_write_rwcfg(dest, source, sizeof(source)) == SUCCESS);
    assert(dma.dsc_cfg_0 != (uintptr_t)source && dma.dsc_cfg_1 != (uintptr_t)dest);
    uint8_t *write_bounce = (uint8_t *)(uintptr_t)dma.dsc_cfg_0;
    hold_busy = 1;
    assert(dma_run() == E_BUSY);
    source[0] = 0;
    assert(*(uint8_t *)(uintptr_t)dma.dsc_cfg_0 == 0x56);
    *(uint8_t *)(uintptr_t)dma.dsc_cfg_1 = 0x99;
    hold_busy = 0;
    dma.status_0 = 0;
    assert(dma_write_rwcfg(NULL, NULL, 0) == SUCCESS && dest[0] == 0x34);
    /* The timed-out input is scrubbed once the engine is idle. */
    assert(write_bounce[0] == 0);

    /* A completed run scrubs its input from the write bounce. */
    source[0] = 0x6b;
    assert(dma_write_rwcfg(NULL, source, sizeof(source)) == SUCCESS);
    assert((uint8_t *)(uintptr_t)dma.dsc_cfg_0 == write_bounce && write_bounce[0] == 0x6b);
    assert(dma_run() == SUCCESS && write_bounce[0] == 0);

    /* So does a run the guard refuses before it starts (policy-masked key)... */
    source[0] = 0x6c;
    assert(dma_write_rwcfg(NULL, source, sizeof(source)) == SUCCESS && write_bounce[0] == 0x6c);
    dma_write_key_config_0(OTPKEY, 0, 256, 0);
    dma.start = 0;
    assert(dma_run() == E_DENY && !dma.start && write_bounce[0] == 0);
    dma_write_key_config_0(SWKEY, ALGO_TYPE_NONE, 0, 0);
    /* ...and one refused for a busy engine, once the engine is idle. */
    source[0] = 0x6d;
    assert(dma_write_rwcfg(NULL, source, sizeof(source)) == SUCCESS && write_bounce[0] == 0x6d);
    dma.status_0 = DMA_STATUS_0_BUSY_MASK;
    assert(dma_run() == E_BUSY && !dma.start && write_bounce[0] == 0x6d);
    dma.status_0 = 0;
    assert(!dma_check_busy_status(NULL) && write_bounce[0] == 0);
    source[0] = 0;

    /* An SG run refused for a busy engine must not leave its descriptors
     * queued for the next operation to submit. */
    pufs_dma_sg_internal_desc_st *stale = dma_sg_new_data_descriptor();
    assert(stale);
    dma_sg_desc_append_to_list(stale);
    dma.status_0 = DMA_STATUS_0_BUSY_MASK;
    assert(pufs_dma_sg_start() == E_BUSY);
    dma.status_0 = 0;
    dma.start = 0;
    assert(pufs_dma_sg_start() == SUCCESS && !dma.start);

    /* Only the engine-owned snapshot survives a timeout. */
    pufs_dma_sg_internal_desc_st *submitted = dma_sg_new_data_descriptor();
    assert(submitted);
    dma_sg_desc_write_key_config(submitted, OTPKEY, 0, 256, 4);
    source[0] = 0x78;
    dma_sg_desc_write_addr(submitted, (uintptr_t)source, (uintptr_t)dest, sizeof(dest));
    dma_sg_desc_append_to_list(submitted);
    hold_busy = 1;
    assert(pufs_dma_sg_start() == E_BUSY && dma.start);
    assert(rt.otp_psmsk[0] == UINT32_MAX && !sg_mem.desc_bitmap);
    pufs_dma_sg_internal_desc_st *live = (void *)(uintptr_t)dma.dsc_cfg_3;
    assert(live != submitted);
    memset(submitted, 0, sizeof(*submitted));
    source[0] = 0;
    assert(*(uint8_t *)(uintptr_t)be2le(live->write_addr) == 0x78);
    *(uint8_t *)(uintptr_t)be2le(live->read_addr) = 0xab;
    assert(dest[0] == 0x34);
    assert(!pufs_dma_sg_is_idle());
    dma_sg_free_all_descriptor();
    assert(!dma_sg_new_data_descriptor() && !sg_mem.desc_bitmap);
    hold_busy = 0;
    dma.status_0 = dma.start = 0;
    assert(pufs_dma_sg_is_idle());
    assert(pufs_dma_sg_start() == SUCCESS && !sg_mem.desc_bitmap && !dma.start);

    submitted = dma_sg_new_data_descriptor();
    assert(submitted && dest[0] == 0x34);
    source[0] = 0x59;
    dma_sg_desc_write_addr(submitted, (uintptr_t)source, (uintptr_t)(source + 1), 20);
    dma_sg_desc_append_to_list(submitted);
    emulate_sg_copy = 1;
    assert(pufs_dma_sg_start() == SUCCESS && !emulate_sg_copy && source[1] == 0x59);

    /* Non-overlapping ranges can exceed the staging limit only after padding. */
    pufs_dma_sg_internal_desc_st *large = dma_sg_new_data_descriptor();
    pufs_dma_sg_internal_desc_st *small = dma_sg_new_data_descriptor();
    assert(large && small);
    /* These fake DMA addresses must not be accessed after a budget failure. */
    dma_sg_desc_write_addr(large, 0x100000, 0, 2 * CHUNK_MAXLEN - 1);
    dma_sg_desc_write_addr(small, 0x200000, 0, 1);
    dma_sg_desc_append_to_list(large);
    dma_sg_desc_append_to_list(small);
    dma.start = 0;
    assert(pufs_dma_sg_start() == E_OVERFLOW && !dma.start && !sg_mem.desc_bitmap);

    /* A list with a NULL appended (unchecked allocation) is dropped. */
    pufs_dma_sg_internal_desc_st *first = dma_sg_new_data_descriptor();
    assert(first);
    dma_sg_desc_append_to_list(first);
    dma_sg_desc_append_to_list(NULL);
    dma.start = 0;
    assert(pufs_dma_sg_start() == E_UNAVAIL && !dma.start && !sg_mem.desc_bitmap);
    /* An allocation failure on an empty list (a probe) leaves the next
     * submission alone. */
    pufs_dma_sg_internal_desc_st *held[32];
    for (int i = 0; i < 32; i++)
        assert((held[i] = dma_sg_new_data_descriptor()) != NULL);
    assert(!dma_sg_new_data_descriptor());
    for (int i = 0; i < 32; i++)
        dma_sg_free_descriptor(held[i]);
    submitted = dma_sg_new_data_descriptor();
    assert(submitted);
    source[0] = 0x5b;
    dma_sg_desc_write_addr(submitted, (uintptr_t)source, (uintptr_t)(source + 1), 20);
    dma_sg_desc_append_to_list(submitted);
    emulate_sg_copy = 1;
    assert(pufs_dma_sg_start() == SUCCESS && !emulate_sg_copy && source[1] == 0x5b);
    /* A list whose allocation ran out mid-build is dropped. */
    for (int i = 0; i < 32; i++) {
        pufs_dma_sg_internal_desc_st *d = dma_sg_new_data_descriptor();
        assert(d);
        dma_sg_desc_write_addr(d, (uintptr_t)source, (uintptr_t)(source + 1), 1);
        dma_sg_desc_append_to_list(d);
    }
    assert(!dma_sg_new_data_descriptor());
    dma.start = 0;
    assert(pufs_dma_sg_start() == E_UNAVAIL && !dma.start && !sg_mem.desc_bitmap);
    submitted = dma_sg_new_data_descriptor();
    assert(submitted);
    source[0] = 0x5a;
    dma_sg_desc_write_addr(submitted, (uintptr_t)source, (uintptr_t)(source + 1), 20);
    dma_sg_desc_append_to_list(submitted);
    emulate_sg_copy = 1;
    assert(pufs_dma_sg_start() == SUCCESS && !emulate_sg_copy && source[1] == 0x5a);

    /* ECC timeout re-masks the slot and does not start another operation. */
    ecp_select_otp_key(3, 256);
    hold_ecc_busy = 1;
    assert(pufs_ecc_start(&status) == E_BUSY && ecc.ctrl);
    assert(rt.otp_psmsk[0] == UINT32_MAX);
    ecc.ctrl = 0;
    uint32_t old_ec = ecc.ec;
    assert(pufs_ecp_set_sm2_curve() == E_BUSY && ecc.ec == old_ec);
    assert(pufs_ecc_start(&status) == E_BUSY && !ecc.ctrl);
    hold_ecc_busy = 0;
    ecc.status = 0;
    assert(pufs_ecc_start(&status) == SUCCESS && ecc.ctrl);

    /* Both RSA paths overwrite the ECP curve registers. Reload the same
     * curve on the next request; a cached name alone is not sufficient. */
    uint8_t n[128] = {0x80}, operand[128] = {0};
    uint8_t rsa_out[BUFFER_SIZE];
    ecc.version = 0xecf39303;
    ecc.mp_version = 0;
    ecp_version = ECP_ECF39303;
    mp_version = MP_00000000;
    assert(pufs_ecp_set_curve_byname(NISTP256) == SUCCESS);
    uint32_t curve_ec = ecc.ec, curve_first_word = ecc.data[0];
    assert(_pufs_rsa_sign(rsa_out, RSA1024, n, 65537, operand, operand, NULL) == SUCCESS);
    assert(!ecdp_set.isset);
    assert(pufs_ecp_set_curve_byname(NISTP256) == SUCCESS);
    assert(ecc.ec == curve_ec && ecc.data[0] == curve_first_word);
    assert(pufs_rsa_verify_calc(rsa_out, operand, RSA1024, n, 65537) == SUCCESS);
    assert(!ecdp_set.isset);
    assert(pufs_ecp_set_curve_byname(NISTP256) == SUCCESS);
    assert(ecc.ec == curve_ec && ecc.data[0] == curve_first_word);

    /* Private operands are wiped at completion or the first idle check after a timeout. */
    uint8_t private_exp[128], phi[128];
    memset(private_exp, 0x5a, sizeof(private_exp));
    memset(phi, 0x3c, sizeof(phi));
    assert(_pufs_rsa_sign(rsa_out, RSA1024, n, 65537, private_exp, operand, phi) == SUCCESS);
    for (size_t i = 0; i < 32; i++)
        assert(ecc.data[32 + i] == 0 && ecc.data[96 + i] == 0);

    hold_ecc_busy = 1;
    ecc.ctrl = 0;
    assert(_pufs_rsa_sign(rsa_out, RSA1024, n, 65537, private_exp, operand, phi) == E_BUSY);
    assert(memcmp(pufs_buffer, operand, sizeof(operand)) == 0);
    assert(!pufs_ecp_is_idle());
    for (size_t i = 0; i < 32; i++)
        assert(ecc.data[32 + i] == 0x3c3c3c3c && ecc.data[96 + i] == 0x5a5a5a5a);

    hold_ecc_busy = 0;
    ecc.status = 0;
    ecc.data[31] = ecc.data[64] = ecc.data[95] = ecc.data[128] = 0xfeedface;
    assert(pufs_ecp_is_idle());
    for (size_t i = 0; i < 32; i++)
        assert(ecc.data[32 + i] == 0 && ecc.data[96 + i] == 0);
    assert(ecc.data[31] == 0xfeedface && ecc.data[64] == 0xfeedface);
    assert(ecc.data[95] == 0xfeedface && ecc.data[128] == 0xfeedface);
    ecc.data[96] = 1;
    assert(pufs_ecp_is_idle() && ecc.data[96] == 1);

    /* P-256 ECDH with an OTP private key is refused on the affected revision. */
    pufs_ec_point_st point = { .qlen = ecc_param[NISTP256].len };
    ecdp_set.isset = true;
    ecdp_set.name = NISTP256;
    ecp_version = ECP_ECF39303;
    mp_version = MP_00000000;
    ecc.ctrl = 0;
    assert(_pufs_ecp_ecccdh_2e2s(point, point, PRK_0, OTPKEY, OTPKEY_10, NULL) == E_UNSUPPORT);
    assert(!ecc.ctrl);
    puts("guard, ECC, direct/SG DMA and timeout recovery: PASS");
    program_tests();
    hwrng_tests();

    /* Policy so far: the boot-time bit in group 0 and CDE segment 1. */
    pufs_otp_get_postmask(&st);
    assert(st.guard_active && st.policy_otp[0] == 1 && st.policy_cde[0] == 0xc);
    lock_released = 1;
    assert(pufs_post_mask(MASK_BIT(OTPKEY_3)) == (
#ifdef RT_PUFS_OTP_WRITE_ENABLE
        E_DENY
#else
        SUCCESS
#endif
    ));
    lock_released = 0;
    assert(rt.otp_psmsk[0] == UINT32_MAX && !(rt.puf_psmsk & PMK_LCK_PSMSK_MASK));
    assert(pufs_post_mask(MASK_BIT(OTPKEY_3)) == SUCCESS);
#ifdef RT_PUFS_OTP_WRITE_ENABLE
    /* Only OTPKEY_3 (group 3) and the policy masks remain, now locked. */
    assert(rt.otp_psmsk[0] == (0x1u | 0xc0u) && rt.otp_psmsk[1] == 0);
    assert(rt.ptm[0] == 0xc && rt.ptm[1] == 0);
    assert((rt.puf_psmsk & PMK_LCK_PSMSK_MASK) == PMK_LCK_PSMSK_MASK);
    pufs_otp_get_postmask(&st);
    assert(st.guard_locked && st.policy_otp[0] == 0xc1);
    out = 0xa5;
    assert(pufs_otp_read(&out, 1, 96) == E_DENY && out == 0xa5);
    assert(pufs_otp_read(&out, 1, OTP_LEN + 128) == E_DENY);
    assert(pufs_otp_read(&out, 1, 32) == SUCCESS && out == 0);
    assert(pufs_otp_read(&out, 1, OTP_LEN) == SUCCESS);
    assert(rt.otp_psmsk[0] == 0xc1 && rt.ptm[0] == 0xc);
    puts("post-mask lockdown: PASS");
#else
    assert(rt.otp_psmsk[0] == UINT32_MAX && !(rt.puf_psmsk & PMK_LCK_PSMSK_MASK));
    puts("post-mask lockdown dry-run: PASS");
#endif
#endif
    return 0;
}
