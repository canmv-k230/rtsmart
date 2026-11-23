/**
 * @file      pufs_rt.c
 * @brief     PUFsecurity PUFrt API implementation
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <rtconfig.h>
#include "rtthread.h"
#include "sbi.h"
#include "pufs_internal.h"
#include "pufs_rt_internal.h"
#include "drv_pufs_internal.h"

struct pufs_rt_regs* rt_regs = NULL;
uintptr_t rt_regs_phys = 0;

#define OTP_CFG_SPI2AXI_ADDR             ((pufs_otp_addr_t)0x0000)
#define OTP_CFG_DISABLE_SPI2AXI_BIT      (1u << 5)
#define OTP_CFG_JTAG_ADDR                ((pufs_otp_addr_t)0x0004)
#define OTP_CFG_JTAG_DISABLE_BIT         (1u << 0)
#define OTP_CFG_BOOT_CTRL_ADDR           ((pufs_otp_addr_t)0x000C)
#define OTP_CFG_FORCE_SECURE_BOOT_BIT    (1u << 0)
#define OTP_CFG_DISABLE_ISP_BIT          (1u << 1)

/*****************************************************************************
 * Static functions
 ****************************************************************************/
/**
 * @brief Select the OTP R/W lock register corresponding to the OTP slot index
 */
static int rwlck_index_sel(uint32_t idx)
{
    uint32_t group = idx / RWLOCK_GROUP_OTP;

    if (group >= MAX_RWLOCK_GROUPS)
        return -1;

    return PIF_RWLCK_START_INDEX + group;
}

static bool check_enable(uint32_t value)
{
    value &= 0xF;
    switch (value) {
    case PUFRT_VALUE4(0x0):
    case PUFRT_VALUE4(0x1):
    case PUFRT_VALUE4(0x2):
    case PUFRT_VALUE4(0x4):
        return true;
    default:
        return false;
    }
}

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * @brief validate PUF slot
 *
 * @param slot The PUF slot which the key is stored in.
 * @return SUCCESS on success, otherwise an error code.
 */
pufs_status_t puf_slot_check(pufs_rt_slot_t slot)
{
    if ((slot < PUFSLOT_1) || (slot > PUFSLOT_3))
        return E_INVALID;

    return SUCCESS;
}
/**
 * @brief validate OTP key slot by key length
 *
 * @param slot The OTP key slot which the key is stored in.
 * @param keybits The key length in bits.
 * @return SUCCESS on success, otherwise an error code.
 */
pufs_status_t otpkey_slot_check(pufs_rt_slot_t slot, uint32_t keybits)
{
    // check slot is in OTPKEY_[0-31], and of size less than 2048
    if ((slot < OTPKEY_0) || (slot > OTPKEY_31))
        return E_INVALID;
    if (keybits > 2047)
        return E_OVERFLOW;
    // produce the alignment factor
    uint32_t align = 1;
    for (uint32_t slotbits = OTP_KEY_BITS; keybits > slotbits; slotbits *= 2, align *= 2)
        ;
    // key slot alignment check
    if (((slot - OTPKEY_0) % align) != 0)
        return E_ALIGN;

    return SUCCESS;
}
/**
 * @brief validate OTP access range test
 *
 * @param addr    The OTP address which the data is stored in.
 * @param len     The length in bytes.
 * @return SUCCESS on success, otherwise an error code.
 */
static pufs_status_t otp_range_check(pufs_otp_addr_t addr, uint32_t len)
{
    // word-aligned OTP address check
    if ((addr % WORD_SIZE) != 0)
        return E_ALIGN;
    // OTP boundary check
    if (addr >= OTP_LEN || len > OTP_LEN - addr)
        return E_OVERFLOW;
    return SUCCESS;
}
/**
 * @brief wait PUFrt busy status
 *
 * @return  PTM status
 */
uint32_t wait_status(void)
{
    while ((rt_regs->status & PTM_STATUS_BUSY_MASK) != 0)
        ;
    return rt_regs->status;
}
#define pufs_ptm_cfg_set(mask, ...) \
    _pufs_ptm_cfg_set(mask, DEF_ARG(__VA_ARGS__, true))
/**
 * @brief Set/unset PTM cfg bit
 *
 * @param[in] mask  The bit.
 * @param[in] on    Turn on/off the bit according to true/false.
 */
static void _pufs_ptm_cfg_set(uint32_t mask, bool on, bool wait)
{
    if (on)
        rt_regs->cfg |= mask;
    else
        rt_regs->cfg = rt_regs->cfg & (~mask);

    if (wait)
        wait_status();
}
/**
 * @brief Continuous random output control
 *
 * @param[in] on  Turn on/off continuous random output when true/false is
 *                specified.
 */
void pufs_rng_cont_ctrl(bool on)
{
    pufs_ptm_cfg_set(PTM_CFG_REG_RNG_CONT_MASK, on);
}
/**
 * @brief Continuous entropy output control
 *
 * @param[in] on  Turn on/off continuous entropy output when true/false is
 *                specified.
 */
void pufs_fre_cont_ctrl(bool on)
{
    pufs_ptm_cfg_set(PTM_CFG_REG_FRE_COUNT_MASK, on);
}
/**
 * @brief PTR/PTC control
 *
 * @param[in] on  Turn on/off PTR/PTC access when true/false is specified.
 */
void pufs_ptr_ptc_ctrl(bool on)
{
    pufs_ptm_cfg_set(PTM_CFG_REG_PTR_PTC_MASK, on);
}

/* Longest wait for the PTM to go idle on the OTP program path. */
#define OTP_IDLE_TIMEOUT_TICKS (RT_TICK_PER_SECOND / 10 + 1)

/*
 * Bounded wait_status() for the OTP program path: a PTM stuck busy returns
 * E_BUSY instead of spinning forever with the PUFS lock held.
 */
pufs_status_t rt_wait_idle(uint32_t *status)
{
    rt_tick_t start;

    if (!((*status = rt_regs->status) & PTM_STATUS_BUSY_MASK))
        return SUCCESS;

    /* Start the clock only once busy is seen, so a delay before the first
     * read cannot eat into the wait. */
    start = rt_tick_get();
    while ((*status = rt_regs->status) & PTM_STATUS_BUSY_MASK) {
        if (rt_tick_get() - start > OTP_IDLE_TIMEOUT_TICKS) {
            /* Re-read: a preemption past the deadline leaves a stale busy. */
            *status = rt_regs->status;
            return (*status & PTM_STATUS_BUSY_MASK) ? E_BUSY : SUCCESS;
        }
    }
    return SUCCESS;
}

#ifdef RT_PUFS_OTP_WRITE_ENABLE
/*
 * Turn on program-ignore before any OTP programming (RT data, CDE data, lock
 * fields): the PTM then skips cells whose program bit is 0 instead of
 * stressing them. Factory program-protect keeps cells already at 1 from being
 * programmed again. The cfg register is in the RT block, so the caller must
 * hold the RT PMP window open.
 */
pufs_status_t rt_pgm_ign_enable(void)
{
    uint32_t status;

    if (rt_wait_idle(&status) != SUCCESS)
        return E_BUSY;
    _pufs_ptm_cfg_set(PTM_CFG_REG_PGM_IGN_MASK, true, false);
    if (rt_wait_idle(&status) != SUCCESS)
        return E_BUSY;
    /* Measured on K230: the bit sticks once set. */
    if (!(rt_regs->cfg & PTM_CFG_REG_PGM_IGN_MASK)) {
        LOG_ERROR("PGM_IGN did not take: cfg=0x%08" PRIx32, rt_regs->cfg);
        return E_ERROR;
    }
    return SUCCESS;
}
#endif

/**
 * @brief Deep standby control (active low)
 *
 * @param[in] on  Turn off/on deep standby mode when true/false is specified.
 */
void rt_write_pdstb(bool off)
{
    pufs_ptm_cfg_set(PTM_CFG_REG_PDSTB_MASK, off, (off ? true : false));
}
/**
 * @brief Read mode control
 *
 * @param[in] mode  Read mode
 */
void rt_write_read_mode(pufs_read_mode_t mode)
{
    rt_regs->cfg = (rt_regs->cfg & ~(0x3 << 4)) | mode;
}
/* Decode one RT OTP lock field. */
static pufs_otp_lock_t rt_lock_decode(uint32_t field)
{
    switch (field) {
    case PUFRT_VALUE4(0x0): return NA;
    case PUFRT_VALUE4(0xC): return RO;
    case PUFRT_VALUE4(0xF): return RW;
    default: return N_OTP_LOCK_T;
    }
}

/**
 * @brief Get OTP rwlck value
 *
 * @param[in] addr  The address of the rwlck.
 * @return          The rwlck bits.
 */
pufs_otp_lock_t pufs_get_otp_rwlck(pufs_otp_addr_t addr)
{
    if (otp_range_check(addr, WORD_SIZE) != SUCCESS)
        return N_OTP_LOCK_T;

    // get rwlck
    int idx = addr / WORD_SIZE;
    int group_index = idx % RWLOCK_GROUP_OTP;

    if ((idx = rwlck_index_sel(idx)) == -1)
        return N_OTP_LOCK_T;

    return rt_lock_decode((rt_regs->pif[idx] >> (group_index * WORD_SIZE)) & 0xF);
}

void rt_write_enroll(void)
{
    if (rt_check_enrolled())
        return;
    rt_regs->puf_enroll = 0xa7;
    wait_status();
}

bool rt_check_rngclk_enable(void)
{
    return ((rt_regs->cfg >> 1) & 0x1) == 1;
}

void rt_write_rngclk(bool enable)
{
    if (enable)
        rt_regs->cfg |= 0x2;
    else
        rt_regs->cfg &= ~0x2;
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void pufs_rt_module_init(uint32_t rt_offset)
{
    rt_regs = (struct pufs_rt_regs*)(pufs_context.base_addr + rt_offset);
    rt_regs_phys = pufs_context.phys_base_addr + rt_offset;
    version_check(PUFSRT_VERSION, rt_regs->version);
}
/**
 * _pufs_get_uid()
 */
pufs_status_t _pufs_get_uid(pufs_uid_st* uid, pufs_rt_slot_t slot)
{
    uint32_t index, *uid32;
    switch (slot) {
    case PUFSLOT_0:
        index = 0;
        break;
    case PUFSLOT_1:
        index = 8;
        break;
    case PUFSLOT_2:
        index = 16;
        break;
    case PUFSLOT_3:
        index = 24;
        break;
    default:
        return E_INVALID;
    }

    uid32 = (uint32_t*)uid->uid;
    for (size_t i = 0; i < (UIDLEN / 4); ++i)
        uid32[i] = be2le(rt_regs->puf[index + i]);

    return SUCCESS;
}
/**
 * _pufs_rand()
 */
pufs_status_t _pufs_rand(uint8_t* rand, uint32_t numblks)
{
    if (numblks > SIZE_MAX / sizeof(uint32_t))
        return E_OVERFLOW;
    return pufs_rng_fill(rand, (size_t)numblks * sizeof(uint32_t)) == 0 ? SUCCESS : E_ERROR;
}

static const char *lock_state_name(pufs_otp_lock_t lock);

static bool pm_lock_held(const char *who)
{
    if (pufs_drv_hw_lock_held())
        return true;
    LOG_ERROR("OTP POST-MASK: %s called without the PUFS lock", who);
    return false;
}

/* A failed read-only transition must survive the operation that opened it. */
static bool pmp_restore_pending[2];

static pufs_status_t set_pmp_window(unsigned int window, unsigned long perm)
{
    uintptr_t addr = window ? rt_cde_regs_phys : rt_regs_phys;
    size_t len = window ? sizeof(*rt_cde_regs) : 0x1000;
    if (!pm_lock_held(__func__))
        return E_DENY;
    struct sbi_ret ret = sbi_pmp_set(addr, len, perm);
    if (!(perm & SBI_PMP_PERM_W))
        pmp_restore_pending[window] = ret.error != SBI_SUCCESS;
    if (ret.error == SBI_SUCCESS)
        return SUCCESS;
    LOG_ERROR("SBI PMP set failed: addr=0x%lx perm=0x%lx error=%ld",
              (unsigned long)addr, perm, ret.error);
    return ret.error == SBI_ERR_DENIED ? E_DENY : E_ERROR;
}

pufs_status_t pufs_recover_pmp(void)
{
    pufs_status_t result = SUCCESS;
    for (unsigned int w = 0; w < 2; ++w) {
        if (pmp_restore_pending[w]) {
            pufs_status_t st = set_pmp_window(w, SBI_PMP_PERM_R);
            if (st != SUCCESS)
                result = st;
        }
    }
    return result;
}

pufs_status_t pufs_rt_pmp_set(unsigned long perm)
{
    return set_pmp_window(0, perm);
}

pufs_status_t pufs_rt_cde_pmp_set(unsigned long perm)
{
    return set_pmp_window(1, perm);
}

/*****************************************************************************
 * Post-mask guard
 *
 * Every RT OTP group (32 bytes, one OTP key slot) and every CDE segment
 * (128 bytes) stays post-masked except while the driver reads or programs it,
 * or a crypto engine uses an OTP key in it. A masked region is no-access:
 * RT reads return 0xFFFFFFFF, CDE reads 0xDEADDEAD, the crypto engines fail
 * and programming is blocked. So a stray or mistaken access hits a mask
 * instead of a fuse.
 *
 * The mask registers stay unlocked so ranges can be opened and closed again.
 * Once they are locked (before boot by firmware, or by pufs_post_mask()), the
 * guard can no longer change them: it then follows the hardware masks, and
 * only the groups and segments left unmasked stay accessible until reset.
 *
 * All guard state and the RT PMP window belong to the PUFS mutex holder.
 ****************************************************************************/
#define PM_RT_GROUP_BYTES 32u
#define PM_RT_GROUPS      (OTP_LEN / PM_RT_GROUP_BYTES)
#define PM_CDE_SEGS       (sizeof(struct pufs_rt_cde_regs) / PUFS_CDE_SEGMENT)

/*
 * PM_FAILED (also the state before init): every access is denied.
 * PM_DISABLED: guard compiled out; follow the hardware masks.
 * PM_ACTIVE: everything masked except while an access is in progress.
 * PM_LOCKED: mask registers locked; follow the hardware masks.
 */
static enum { PM_FAILED, PM_DISABLED, PM_ACTIVE, PM_LOCKED } pm_state;
static uint32_t pm_fixed[4];   /* Policy masks, never opened by the guard. */
static uint32_t pm_pending[4]; /* Failed cleanup, retried under the PUFS mutex. */
/* Every RT group and CDE segment (unused when the guard is compiled out). */
static const uint32_t pm_all[4] __attribute__((unused)) = {
    UINT32_MAX, UINT32_MAX, UINT32_MAX, (1u << (2 * (PM_CDE_SEGS - 16))) - 1 };

/* Any lck_psmsk bit set: treat the mask registers as read-only. */
static bool pm_hw_locked(void)
{
    return (rt_regs->puf_psmsk & PMK_LCK_PSMSK_MASK) != 0;
}

/* Mask registers: [0..1] RT groups 0-31, [2..3] CDE segments 0-23. */
static volatile uint32_t *pm_reg(int i)
{
    return (i < 2) ? &rt_regs->otp_psmsk[i] : &rt_regs->ptm[i - 2];
}

/* Mask bits (two per group or segment) covering flat range [addr, addr + len). */
static void pm_range_bits(uint32_t addr, uint32_t len, uint32_t bits[4])
{
    memset(bits, 0, 4 * sizeof(uint32_t));
    for (uint32_t a = addr; a < addr + len && a < OTP_TOTAL_LEN; ) {
        if (a < OTP_LEN) {
            uint32_t g = a / PM_RT_GROUP_BYTES;

            bits[g / 16] |= 0x3u << ((g % 16) * 2);
            a = (g + 1) * PM_RT_GROUP_BYTES;
        } else {
            uint32_t s = (a - OTP_LEN) / PUFS_CDE_SEGMENT;

            bits[2 + s / 16] |= 0x3u << ((s % 16) * 2);
            a = OTP_LEN + (s + 1) * PUFS_CDE_SEGMENT;
        }
    }
}

/* Clear (open) or set (close) mask bits and confirm they took. */
static pufs_status_t pm_update(const uint32_t bits[4], bool open)
{
    pufs_status_t check = SUCCESS, restore;

    if (!(bits[0] | bits[1] | bits[2] | bits[3]))
        return SUCCESS;
    if ((restore = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return restore;

    for (int i = 0; i < 4; i++) {
        volatile uint32_t *reg = pm_reg(i);

        if (!bits[i])
            continue;
        *reg = open ? (*reg & ~bits[i]) : (*reg | bits[i]);
        /* Writes are dropped once pufs_post_mask() has locked the registers. */
        if ((*reg & bits[i]) != (open ? 0 : bits[i]))
            check = E_DENY;
    }

    restore = pufs_rt_pmp_set(SBI_PMP_PERM_R);

    if (check != SUCCESS)
        LOG_ERROR("OTP POST-MASK %s DENIED: mask registers locked",
                  open ? "OPEN" : "CLOSE");
    return restore != SUCCESS ? restore : check;
}

static pufs_status_t pm_retry_close(void)
{
    pufs_status_t check = pufs_recover_pmp();
    if (check != SUCCESS)
        return check;
    check = pm_update(pm_pending, false);
    if (check == SUCCESS)
        memset(pm_pending, 0, sizeof(pm_pending));
    return check;
}

static pufs_status_t pm_guard(const uint32_t bits[4], bool open)
{
    pufs_status_t check;

    if (!pm_lock_held(__func__))
        return E_DENY;
    if (open && (check = pm_retry_close()) != SUCCESS)
        return check;
    if (!(bits[0] | bits[1] | bits[2] | bits[3]))
        return SUCCESS;
    if (pm_state == PM_FAILED)
        return E_DENY;
    if (open) {
        for (int i = 0; i < 4; i++) {
            if ((bits[i] & pm_fixed[i]) ||
                (pm_state != PM_ACTIVE && (bits[i] & *pm_reg(i))))
                return E_DENY;
        }
    }
    /* Disabled or locked: the hardware masks decide; nothing to change. */
    if (pm_state != PM_ACTIVE)
        return SUCCESS;

    check = pm_update(bits, open);
    if (check != SUCCESS) {
        /* An open may have changed only some registers. Close the entire
         * request even in that case; never forget an unverified close. */
        for (int i = 0; i < 4; i++)
            pm_pending[i] |= bits[i];
        pufs_status_t close = pm_retry_close();
        if (close != SUCCESS)
            return close;
    }
    return check;
}

/* A policy mask remains in force even when the guard already masked the segment. */
pufs_status_t rt_cde_write_mask(uint32_t offset)
{
    uint32_t bits[4];
    if (offset >= sizeof(struct pufs_rt_cde_regs))
        return E_INVALID;
    if (!pm_lock_held(__func__))
        return E_DENY;
    if (pm_hw_locked())
        return E_DENY;
    pm_range_bits(OTP_LEN + offset, 1, bits);
    pufs_status_t check = pm_update(bits, false);
    if (check == SUCCESS) {
        for (int i = 2; i < 4; i++)
            pm_fixed[i] |= bits[i];
    }
    return check;
}

/* Open or re-mask flat OTP range [addr, addr + len). Caller must not hold the
 * RT PMP window: this opens and closes it. */
pufs_status_t rt_otp_guard(uint32_t addr, uint32_t len, bool open)
{
    uint32_t bits[4];

    if (len == 0)
        return SUCCESS;
    pm_range_bits(addr, len, bits);
    return pm_guard(bits, open);
}

/* Same for OTP key slots: bit n of @slots is OTPKEY_n, which is RT group n. */
pufs_status_t rt_otp_guard_keyslots(uint32_t slots, bool open)
{
    uint32_t bits[4] = { 0 };

    if (!slots)
        return open ? pm_retry_close() : SUCCESS;
    for (uint32_t g = 0; g < PM_RT_GROUPS; g++) {
        if (slots & (1u << g))
            bits[g / 16] |= 0x3u << ((g % 16) * 2);
    }
    return pm_guard(bits, open);
}

/*
 * After a program operation timed out (E_BUSY), give the PTM another bounded
 * wait before the range is masked again, so a slow program is not cut off.
 */
void rt_settle_before_remask(pufs_status_t check)
{
    uint32_t status;

    if (check == E_BUSY)
        rt_wait_idle(&status);
}

/* Slot bitmask for a key of @keybits starting at OTP key slot index @slot. */
uint32_t rt_otp_keyslot_mask(uint32_t slot, uint32_t keybits)
{
    uint32_t n = keybits ? (keybits + OTP_KEY_BITS - 1) / OTP_KEY_BITS : 1, mask = 0;

    for (uint32_t i = 0; i < n && slot + i < PM_RT_GROUPS; i++)
        mask |= 1u << (slot + i);
    return mask;
}

pufs_status_t pufs_otp_guard_init(void)
{
#ifdef RT_PUFS_OTP_POSTMASK_GUARD_DISABLE
    pm_state = PM_DISABLED;
    LOG_WARN("OTP POST-MASK GUARD: disabled by RT_PUFS_OTP_POSTMASK_GUARD_DISABLE");
    return SUCCESS;
#else
    pufs_status_t check;

    if (pm_state == PM_ACTIVE)
        return SUCCESS;
    /*
     * Masks set before boot are policy and are never opened. A normal boot
     * leaves them clear, and the SoC reset behind `reboot` clears them too,
     * so log any that are found: they deny access until power-on reset.
     */
    for (int i = 0; i < 4; i++)
        pm_fixed[i] |= *pm_reg(i) & pm_all[i];
    if (pm_fixed[0] | pm_fixed[1] | pm_fixed[2] | pm_fixed[3])
        LOG_WARN("OTP POST-MASK: masks set before boot stay in force:"
                 " otp_psmsk 0x%08" PRIx32 " 0x%08" PRIx32
                 " cde_psmsk 0x%08" PRIx32 " 0x%08" PRIx32,
                 pm_fixed[0], pm_fixed[1], pm_fixed[2], pm_fixed[3]);

    if (pm_hw_locked()) {
        pm_state = PM_LOCKED;
        LOG_WARN("OTP POST-MASK GUARD: mask registers locked before boot;"
                 " only unmasked OTP stays accessible");
        return SUCCESS;
    }

    check = pm_update(pm_all, false);
    pm_state = (check == SUCCESS) ? PM_ACTIVE : PM_FAILED;
    return check;
#endif
}

void pufs_otp_get_postmask(pufs_otp_postmask_st *st)
{
    st->guard_active = pm_state == PM_ACTIVE;
    st->guard_failed = pm_state == PM_FAILED;
    st->guard_locked = pm_state == PM_LOCKED;
    for (int i = 0; i < 2; i++) {
        st->policy_otp[i] = pm_fixed[i];
        st->policy_cde[i] = pm_fixed[2 + i];
    }
    st->otp_psmsk[0] = rt_regs->otp_psmsk[0];
    st->otp_psmsk[1] = rt_regs->otp_psmsk[1];
    st->cde_psmsk[0] = rt_regs->ptm[0];
    st->cde_psmsk[1] = rt_regs->ptm[1];
    st->puf_psmsk = rt_regs->puf_psmsk;
}

#define PTM_STATUS_FAULT_MASK (PTM_STATUS_ERROR_MASK | \
    PTM_STATUS_WARNING_MASK | PTM_STATUS_WRONG_MASK | \
    PTM_STATUS_FORBID_MASK | PTM_STATUS_FAILCNT_MASK)

/*
 * Program one RT or CDE OTP word and verify it. Bits only go 0->1, so the
 * word must read back exactly as (before | word): every requested bit set and
 * no other bit changed. Logs never include word values: they may be key data.
 */
pufs_status_t rt_program_otp_word(volatile uint32_t *reg, uint32_t word,
    uint32_t flat_addr)
{
    uint32_t status, expect, readback;

    if (rt_wait_idle(&status) != SUCCESS) {
        LOG_ERROR("OTP PROGRAM BUSY: addr=0x%03" PRIx32 " status=0x%08" PRIx32,
                  flat_addr, status);
        return E_BUSY;
    }

    expect = *reg | word;
    *reg = word;

    if (rt_wait_idle(&status) != SUCCESS) {
        LOG_ERROR("OTP PROGRAM TIMEOUT: addr=0x%03" PRIx32 " status=0x%08" PRIx32,
                  flat_addr, status);
        return E_BUSY;
    }

    readback = *reg;
    if (readback != expect) {
        LOG_ERROR("OTP PROGRAM FAILED: addr=0x%03" PRIx32 " status=0x%08" PRIx32 "%s%s",
                  flat_addr, status,
                  (expect & ~readback) ? " (requested bits missing)" : "",
                  (readback & ~expect) ? " (unrequested bits set)" : "");
        return E_ERROR;
    }

    if (status & PTM_STATUS_FAULT_MASK)
        LOG_WARN("OTP PROGRAM STATUS: addr=0x%03" PRIx32 " status=0x%08" PRIx32,
                 flat_addr, status);

    return SUCCESS;
}

/**
 * pufs_read_otp()
 */
pufs_status_t pufs_read_otp(uint8_t* outbuf, uint32_t len, pufs_otp_addr_t addr)
{
    pufs_status_t check;

    if ((check = otp_range_check(addr, len)) != SUCCESS)
        return check;
    if (len && !outbuf)
        return E_INVALID;
    if ((check = rt_otp_guard(addr, len, true)) != SUCCESS)
        return check;

    for (uint32_t off = 0; off < len; off += WORD_SIZE) {
        uint32_t word = be2le(rt_regs->otp[(addr + off) / WORD_SIZE]);

        memcpy(outbuf + off, &word, otp_chunk_len(len - off));
    }

    return rt_otp_guard(addr, len, false);
}
/*
 * Pack up to WORD_SIZE input bytes into a program word for @area. @mask gets
 * the bits the bytes cover; a short final chunk leaves the rest of the word
 * alone.
 */
static uint32_t rt_program_pack(const struct rt_program_area *area,
    const uint8_t *inbuf, uint32_t len, uint32_t *mask)
{
    uint32_t word = 0;

    *mask = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t shift = 8 * (area->big_endian ? WORD_SIZE - 1 - i : i);

        word |= (uint32_t)inbuf[i] << shift;
        *mask |= 0xFFu << shift;
    }
    return word;
}

/* rt_program_area() body; runs with the range opened by the guard. */
static pufs_status_t rt_program_open(const struct rt_program_area *area,
    const uint8_t *inbuf, uint32_t len, uint32_t offset)
{
    uint32_t changed = 0, status, mask, flat = area->flat_base + offset;

    /*
     * A word read while an earlier program is still in flight may already
     * match, which would let a retry report success before it completed.
     */
    if (rt_wait_idle(&status) != SUCCESS) {
        LOG_ERROR("%s WRITE BUSY: addr=0x%03" PRIx32 " status=0x%08" PRIx32,
                  area->name, flat, status);
        return E_BUSY;
    }

    /* Validate every word before making any irreversible changes. */
    for (uint32_t off = 0; off < len; off += WORD_SIZE) {
        pufs_otp_lock_t lock = area->lock(offset + off);
        uint32_t word = rt_program_pack(area, inbuf + off, otp_chunk_len(len - off), &mask);
        uint32_t cur = area->otp[(offset + off) / WORD_SIZE] & mask;

        if (lock != RW) {
            LOG_WARN("%s WRITE DENIED: addr=0x%03" PRIx32 " locked (%s)",
                     area->name, flat + off, lock_state_name(lock));
            return E_DENY;
        }
        if (cur & ~word) {
            LOG_WARN("%s WRITE DENIED: addr=0x%03" PRIx32 " bit 1->0",
                     area->name, flat + off);
            return E_DENY;
        }
        changed += cur != word;
    }

    /* Log counts only: the data may be key material. */
#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("%s WRITE DRY-RUN: addr=0x%03" PRIx32 " len=%" PRIu32
             " (%" PRIu32 " word(s) would change)", area->name, flat, len, changed);
    return SUCCESS;
#else
    pufs_status_t check, restore;

    if (!changed)
        return SUCCESS;

    LOG_WARN("%s WRITE: addr=0x%03" PRIx32 " len=%" PRIu32 " (%" PRIu32 " word(s))",
             area->name, flat, len, changed);

    if ((check = area->pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    /* PGM_IGN lives in the RT block's cfg register: unless that is already the
     * data window, open the RT window only long enough to set it. */
    if (area->pmp_set == pufs_rt_pmp_set) {
        check = rt_pgm_ign_enable();
    } else if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) == SUCCESS) {
        check = rt_pgm_ign_enable();
        restore = pufs_rt_pmp_set(SBI_PMP_PERM_R);
        if (restore != SUCCESS)
            check = restore;
    }
    if (check != SUCCESS)
        LOG_ERROR("%s WRITE ABORTED: addr=0x%03" PRIx32 ", nothing programmed",
                  area->name, flat);

    for (uint32_t off = 0; check == SUCCESS && off < len; off += WORD_SIZE) {
        volatile uint32_t *reg = &area->otp[(offset + off) / WORD_SIZE];
        uint32_t word = rt_program_pack(area, inbuf + off, otp_chunk_len(len - off), &mask);

        if (word & ~*reg)
            check = rt_program_otp_word(reg, word, flat + off);
    }

    restore = area->pmp_set(SBI_PMP_PERM_R);
    return restore != SUCCESS ? restore : check;
#endif
}

/*
 * Program [offset, offset + len) of @area: open the range in the post-mask
 * guard, check and program it, and mask it again.
 */
pufs_status_t rt_program_area(const struct rt_program_area *area,
    const uint8_t *inbuf, uint32_t len, uint32_t offset)
{
    pufs_status_t check, close;

    if ((offset % WORD_SIZE) != 0)
        return E_ALIGN;
    if (offset >= area->size || len > area->size - offset)
        return E_OVERFLOW;
    if (len == 0)
        return SUCCESS;
    if (!inbuf)
        return E_INVALID;
    if ((check = rt_otp_guard(area->flat_base + offset, len, true)) != SUCCESS)
        return check;

    check = rt_program_open(area, inbuf, len, offset);
    rt_settle_before_remask(check);
    close = rt_otp_guard(area->flat_base + offset, len, false);
    return close != SUCCESS ? close : check;
}

static pufs_otp_lock_t rt_otp_word_lock(uint32_t offset)
{
    return pufs_get_otp_rwlck(offset);
}

/**
 * pufs_program_otp()
 */
pufs_status_t pufs_program_otp(const uint8_t* inbuf, uint32_t len,
    pufs_otp_addr_t addr)
{
    const struct rt_program_area area = {
        .name = "OTP",
        .otp = rt_regs->otp,
        .size = OTP_LEN,
        .flat_base = 0,
        .big_endian = true,
        .lock = rt_otp_word_lock,
        .pmp_set = pufs_rt_pmp_set,
    };

    return rt_program_area(&area, inbuf, len, addr);
}
/**
 * pufs_lock_otp
 */
static const char *lock_state_name(pufs_otp_lock_t lock)
{
    switch (lock) {
    case NA: return "NA";
    case RO: return "RO";
    case RW: return "RW";
    default: return "?";
    }
}

/* A field meets a request if it is already in that state, or NA when RO is
 * asked for: NA blocks writes too, and NA -> RO is impossible anyway. */
static bool rt_lock_satisfied(pufs_otp_lock_t have, pufs_otp_lock_t want)
{
    return have == want || (want == RO && have == NA);
}

static uint32_t rt_lock_field(const struct rt_lock_area *area, uint32_t field)
{
    return (rt_regs->pif[area->pif_base + field / RWLOCK_GROUP_OTP] >>
            ((field % RWLOCK_GROUP_OTP) * 4)) & 0xF;
}

/*
 * Set lock fields [first, last] of @area to @lock. Lock fields are OTP too:
 * bits only go 0->1, so a field can only tighten (RW -> RO -> NA). Every field
 * is checked before anything is written, and each PIF register written must
 * read back exactly as (before | new fields).
 */
pufs_status_t rt_set_lock_fields(const struct rt_lock_area *area,
    uint32_t first, uint32_t last, pufs_otp_lock_t lock)
{
    uint32_t lock_val, status, changed = 0;
    uint32_t addr = area->flat_base + first * area->field_bytes;
    uint32_t end = area->flat_base + (last + 1) * area->field_bytes - 1;

    switch (lock) {
    case NA: lock_val = PUFRT_VALUE4(0x0); break;
    case RO: lock_val = PUFRT_VALUE4(0xC); break;
    case RW: lock_val = PUFRT_VALUE4(0xF); break;
    default: return E_INVALID;
    }
    if (first > last || last >= area->fields)
        return E_OVERFLOW;

    if (rt_wait_idle(&status) != SUCCESS) {
        LOG_ERROR("%s LOCK BUSY: addr=0x%03" PRIx32 " status=0x%08" PRIx32,
                  area->name, addr, status);
        return E_BUSY;
    }

    for (uint32_t f = first; f <= last; f++) {
        uint32_t cur = rt_lock_field(area, f);

        if (rt_lock_satisfied(area->decode(cur), lock))
            continue;
        if (cur & ~lock_val) {
            LOG_WARN("%s LOCK DENIED: addr=0x%03" PRIx32 " is %s, cannot become %s",
                     area->name, area->flat_base + f * area->field_bytes,
                     lock_state_name(area->decode(cur)), lock_state_name(lock));
            return E_DENY;
        }
        changed++;
    }

#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("%s LOCK DRY-RUN: addr=0x%03" PRIx32 "-0x%03" PRIx32 " lock=%s"
             " (%" PRIu32 " field(s) would change)",
             area->name, addr, end, lock_state_name(lock), changed);
    return SUCCESS;
#else
    pufs_status_t check, restore, close;

    if (!changed)
        return SUCCESS;

    LOG_WARN("%s LOCK: addr=0x%03" PRIx32 "-0x%03" PRIx32 " lock=%s (%" PRIu32 " field(s))",
             area->name, addr, end, lock_state_name(lock), changed);

    /* Keep the covered OTP data open while its lock fields are programmed. */
    if ((check = rt_otp_guard(addr, end - addr + 1, true)) != SUCCESS)
        return check;
    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS) {
        close = rt_otp_guard(addr, end - addr + 1, false);
        return close != SUCCESS ? close : check;
    }

    if ((check = rt_pgm_ign_enable()) != SUCCESS)
        LOG_ERROR("%s LOCK ABORTED: addr=0x%03" PRIx32 " PTM busy, nothing programmed",
                  area->name, addr);

    for (uint32_t group = first / RWLOCK_GROUP_OTP;
         check == SUCCESS && group <= last / RWLOCK_GROUP_OTP; group++) {
        uint32_t index = area->pif_base + group;
        volatile uint32_t *reg = &rt_regs->pif[index];
        uint32_t before = *reg, next = before;

        for (uint32_t f = group * RWLOCK_GROUP_OTP;
             f < (group + 1) * RWLOCK_GROUP_OTP; f++) {
            uint32_t shift = (f % RWLOCK_GROUP_OTP) * 4;

            if (f >= first && f <= last &&
                !rt_lock_satisfied(area->decode((before >> shift) & 0xF), lock))
                next |= lock_val << shift;
        }
        if (next == before)
            continue;

        *reg = next;
        if (rt_wait_idle(&status) != SUCCESS) {
            LOG_ERROR("%s LOCK TIMEOUT: pif[%" PRIu32 "] status=0x%08" PRIx32,
                      area->name, index, status);
            check = E_BUSY;
        } else if (*reg != next) {
            LOG_ERROR("%s LOCK FAILED: pif[%" PRIu32 "] status=0x%08" PRIx32,
                      area->name, index, status);
            check = E_ERROR;
        }
    }

    restore = pufs_rt_pmp_set(SBI_PMP_PERM_R);
    rt_settle_before_remask(check);
    close = rt_otp_guard(addr, end - addr + 1, false);
    return close != SUCCESS ? close : (restore != SUCCESS ? restore : check);
#endif
}

static const struct rt_lock_area rt_otp_lock_area = {
    .name = "OTP",
    .pif_base = PIF_RWLCK_START_INDEX,
    .fields = OTP_LEN / WORD_SIZE,
    .field_bytes = WORD_SIZE,
    .flat_base = 0,
    .decode = rt_lock_decode,
};

pufs_status_t pufs_lock_otp(pufs_otp_addr_t addr, uint32_t len,
    pufs_otp_lock_t lock)
{
    pufs_status_t check;

    if ((check = otp_range_check(addr, len)) != SUCCESS)
        return check;
    if (lock != NA && lock != RO && lock != RW)
        return E_INVALID;
    if (len == 0)
        return SUCCESS;

    return rt_set_lock_fields(&rt_otp_lock_area, addr / WORD_SIZE,
                              (addr + len - 1) / WORD_SIZE, lock);
}
/**
 * pufs_program_key2otp
 */
pufs_status_t pufs_program_key2otp(pufs_rt_slot_t slot, const uint8_t* key,
    uint32_t keybits, pufs_otp_lock_t lock)
{
    pufs_status_t check;

    /* Lock request must be NA, RO, RW or N_OTP_LOCK_T (skip): check it before
     * anything is burned, so a bad value cannot leave the key unlocked. */
    if ((unsigned)lock > N_OTP_LOCK_T)
        return E_INVALID;

    // OTPKEY_0 is used by BROM (unlocked), reject writes to protect it
    if (slot == OTPKEY_0)
        return E_INVALID;

    // check OTP key slot by key length
    if ((check = otpkey_slot_check(slot, keybits)) != SUCCESS)
        return check;

    // XXX programmed check
    // write key by pufs_program_otp()
    pufs_otp_addr_t addr = (slot - OTPKEY_0) * OTP_KEY_LEN;
    LOG_WARN("KEY2OTP: slot=%u addr=0x%03x keybits=%" PRIu32 " lock=%s",
             slot, addr, keybits, lock_state_name(lock));
    if ((check = pufs_program_otp(key, b2B(keybits), addr)) != SUCCESS)
        return check;

    // set lock state (default N_OTP_LOCK_T = skip locking)
    if (lock < N_OTP_LOCK_T)
        return pufs_lock_otp(addr, b2B(keybits), lock);

    return SUCCESS;
}
/**
 * pufs_zeroize()
 */
pufs_status_t pufs_zeroize(pufs_rt_slot_t slot)
{
    uint32_t val32;

    if (slot == PUFSLOT_0) {
        val32 = 0x4b;
    } else if (slot == PUFSLOT_1) {
        val32 = 0xad;
    } else if (slot == PUFSLOT_2) {
        val32 = 0xd2;
    } else if (slot == PUFSLOT_3) {
        val32 = 0x34;
    } else if ((slot >= OTPKEY_0) && (slot <= OTPKEY_31)) {
        val32 = (slot - OTPKEY_0) + 0x80;
    } else {
        return E_INVALID;
    }

#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("ZEROIZE DRY-RUN: slot=%u"
             " (enable RT_PUFS_OTP_WRITE_ENABLE to commit)", slot);
    return SUCCESS;
#else
    pufs_status_t check, restore, close;
    uint32_t status;
    uint32_t keyslots = (slot >= OTPKEY_0) ? (1u << (slot - OTPKEY_0)) : 0;

    if (rt_wait_idle(&status) != SUCCESS) {
        LOG_ERROR("ZEROIZE BUSY: slot=%u status=0x%08" PRIx32, slot, status);
        return E_BUSY;
    }

    LOG_WARN("ZEROIZE: slot=%u", slot);

    if ((check = rt_otp_guard_keyslots(keyslots, true)) != SUCCESS)
        return check;
    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS) {
        close = rt_otp_guard_keyslots(keyslots, false);
        return close != SUCCESS ? close : check;
    }

    if (slot < OTPKEY_0)
        rt_regs->puf_zeroize = val32;
    else
        rt_regs->otp_zeroize = val32;

    if ((check = rt_wait_idle(&status)) != SUCCESS) {
        LOG_ERROR("ZEROIZE TIMEOUT: slot=%u status=0x%08" PRIx32, slot, status);
    } else if (status & (PTM_STATUS_ERROR_MASK | PTM_STATUS_WARNING_MASK |
                         PTM_STATUS_WRONG_MASK | PTM_STATUS_FORBID_MASK)) {
        LOG_ERROR("ZEROIZE FAILED: slot=%u status=0x%08" PRIx32, slot, status);
        check = E_ERROR;
    }

    restore = pufs_rt_pmp_set(SBI_PMP_PERM_R);
    rt_settle_before_remask(check);
    close = rt_otp_guard_keyslots(keyslots, false);
    return close != SUCCESS ? close : (restore != SUCCESS ? restore : check);
#endif
}
/**
 * pufs_post_mask()
 */
pufs_status_t pufs_post_mask(uint64_t maskslots)
{
#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("POST_MASK DRY-RUN: maskslots=0x%016" PRIx64
             " (enable RT_PUFS_OTP_WRITE_ENABLE to commit)", maskslots);
    return SUCCESS;
#else
    uint32_t otp_psmsk_0, otp_psmsk_1, puf_psmsk, val32_0 = 0, val32_1 = 0;
    uint32_t want[4];

    if (!pm_lock_held(__func__))
        return E_DENY;
    if (pm_state == PM_FAILED) {
        LOG_ERROR("POST_MASK DENIED: post-mask guard failed");
        return E_DENY;
    }

    LOG_WARN("POST_MASK: maskslots=0x%016" PRIx64, maskslots);

    puf_psmsk = (maskslots & 0xf);
    otp_psmsk_0 = ((maskslots >> 4) & 0x0000ffff);
    otp_psmsk_1 = ((maskslots >> 20) & 0x0000ffff);

    for (int i = 0; i < 16; i++) {
        if ((otp_psmsk_0 >> i) & 0x1)
            val32_0 |= (0x3 << (2 * i));
        if ((otp_psmsk_1 >> i) & 0x1)
            val32_1 |= (0x3 << (2 * i));
    }

    uint32_t puf_val = 0;
    for (int i = 0; i < 4; i++) {
        if ((puf_psmsk >> i) & 0x1)
            puf_val |= (0x3 << (2 * i));
    }

    /* Already locked: nothing can change. Succeed only if the requested masks
     * are in force already. */
    if (pm_hw_locked()) {
        pm_state = PM_LOCKED;
        if ((rt_regs->puf_psmsk & puf_val) != puf_val ||
            (rt_regs->otp_psmsk[0] & val32_0) != val32_0 ||
            (rt_regs->otp_psmsk[1] & val32_1) != val32_1) {
            LOG_ERROR("POST_MASK DENIED: mask registers already locked");
            return E_DENY;
        }
        pm_fixed[0] |= val32_0;
        pm_fixed[1] |= val32_1;
        return SUCCESS;
    }

    /* Do not lock the mask registers with an outstanding close. */
    pufs_status_t check = pm_retry_close();
    if (check != SUCCESS)
        return check;

    /*
     * The lock freezes whatever is masked at that moment. With the guard
     * active that is everything, so drop the guard's own masks first and keep
     * exactly the policy masks plus the requested ones; otherwise never drop
     * a mask that is already set. Set CDE policy masks with
     * rt_cde_write_mask() before calling this.
     */
    for (int i = 0; i < 4; i++)
        want[i] = (pm_state == PM_ACTIVE) ? pm_fixed[i] : *pm_reg(i);
    want[0] |= val32_0;
    want[1] |= val32_1;

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    for (int i = 0; i < 4; i++)
        *pm_reg(i) = want[i];

    /* lck_psmsk (bits 31:24) makes every post-mask register read-only:
     * otp_psmsk, puf_psmsk and the CDE masks. */
    rt_regs->puf_psmsk |= PMK_LCK_PSMSK_MASK | puf_val;

    /*
     * Writes to locked mask registers are dropped, so confirm the requested
     * masks and the lock all took; the lock alone is the request when
     * maskslots is 0.
     */
    uint32_t puf_now = rt_regs->puf_psmsk;
    bool applied = (puf_now & PMK_LCK_PSMSK_MASK) == PMK_LCK_PSMSK_MASK &&
                   (puf_now & puf_val) == puf_val;

    for (int i = 0; i < 4; i++)
        applied = applied && (*pm_reg(i) & want[i]) == want[i];

    if (pm_hw_locked()) {
        /* From here on the registers cannot change: follow them. */
        pm_state = PM_LOCKED;
    } else if (pm_state == PM_ACTIVE) {
        /* The lock did not take, so nothing is frozen: put the guard's masks
         * back rather than leave everything open. */
        for (int i = 0; i < 4; i++) {
            *pm_reg(i) |= pm_all[i];
            if ((*pm_reg(i) & pm_all[i]) != pm_all[i])
                pm_state = PM_FAILED;
        }
    }
    if (applied) {
        pm_fixed[0] |= val32_0;
        pm_fixed[1] |= val32_1;
    }

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R)) != SUCCESS)
        return check;

    if (!applied) {
        LOG_ERROR("POST_MASK DENIED: requested masks or lock did not take");
        return E_DENY;
    }

    return SUCCESS;
#endif
}
/**
 * pufs_rt_version()
 */
pufs_status_t pufs_rt_version(uint32_t* version, uint32_t* features)
{
    *version = rt_regs->version;
    *features = rt_regs->feature;
    return SUCCESS;
}

bool rt_check_enrolled(void)
{
    return check_enable(rt_regs->pif[0] >> PIF_00_ENROLL_BITS);
}

bool rt_check_tmlck_enable(void)
{
    return check_enable(rt_regs->pif[0] >> PIF_00_TMLCK_BITS);
}

bool rt_check_puflck_enable(void)
{
    return check_enable(rt_regs->pif[0] >> PIF_00_PUFLCK_BITS);
}

bool rt_check_otplck_enable(void)
{
    return check_enable(rt_regs->pif[0] >> PIF_00_OTPLCK_BITS);
}

bool rt_check_shfwen_enable(void)
{
    return check_enable(rt_regs->pif[0] >> PIF_00_SHFWEN_BITS);
}

bool rt_check_shfren_enable(void)
{
    return check_enable(rt_regs->pif[0] >> PIF_00_SHFREN_BITS);
}

bool rt_check_pgmprt_enable(void)
{
    return check_enable(rt_regs->pif[0] >> PIF_00_PGMPRT_BITS);
}

pufs_status_t rt_write_set_flag(pufs_ptm_flag_t flag, uint32_t* status)
{
    uint32_t res;
    if (rt_check_tmlck_enable())
        return E_INVALID;

    rt_regs->set_flag = flag;
    res = wait_status();
    if (status != NULL)
        *status = res;

    return SUCCESS;
}

/*****************************************************************************
 * Unified OTP wrappers (flat 0-4095 address space)
 *
 * Dispatches to RT (0x000-0x3FF) or CDE (0x400-0xFFF) drivers.
 * BROM patch area (0x400-0xBFF) is write-protected.
 ****************************************************************************/
static pufs_status_t otp_unified_range_check(pufs_otp_addr_t addr, uint32_t len)
{
    if ((addr % WORD_SIZE) != 0)
        return E_ALIGN;
    if (addr >= OTP_TOTAL_LEN || len > OTP_TOTAL_LEN - addr)
        return E_OVERFLOW;
    if (addr < OTP_LEN && len > OTP_LEN - addr)
        return E_INVALID; /* cross-boundary RT/CDE not allowed */
    return SUCCESS;
}

/*
 * Preserve the deployed raw byte contract of the leaf PUFS RT/CDE drivers.
 *
 * Refuse to read while a program is in flight: a word written by a timed-out
 * program may already read back as requested, and a caller that skips work
 * when the data "already matches" would then report success too early.
 */
pufs_status_t pufs_otp_read(uint8_t* outbuf, uint32_t len, pufs_otp_addr_t addr)
{
    pufs_status_t check;
    uint32_t status;

    if ((check = otp_unified_range_check(addr, len)) != SUCCESS)
        return check;
    if (rt_wait_idle(&status) != SUCCESS) {
        LOG_ERROR("OTP READ BUSY: addr=0x%03x status=0x%08" PRIx32, addr, status);
        return E_BUSY;
    }
    if (addr < OTP_LEN)
        return pufs_read_otp(outbuf, len, addr);
    return pufs_read_cde(outbuf, len, addr - OTP_LEN);
}

/* Preserve the deployed raw byte contract of the leaf PUFS RT/CDE drivers. */
pufs_status_t pufs_otp_write(const uint8_t* inbuf, uint32_t len,
    pufs_otp_addr_t addr)
{
    pufs_status_t check;
    if ((check = otp_unified_range_check(addr, len)) != SUCCESS)
        return check;
    /* protect BROM patch area (0x400-0xBFF) */
    if (addr >= OTP_CDE_START && addr < OTP_CDE_USER_START) {
        LOG_WARN("OTP WRITE DENIED: addr=0x%03x in BROM patch area", addr);
        return E_DENY;
    }
    if (addr < OTP_LEN)
        return pufs_program_otp(inbuf, len, addr);
    return pufs_program_cde(inbuf, len, addr - OTP_LEN);
}

pufs_status_t pufs_otp_query_lock(pufs_otp_addr_t addr, pufs_otp_lock_t *lock)
{
    uint32_t status;

    if (!lock)
        return E_INVALID;
    *lock = N_OTP_LOCK_T;

    /* RT locks are per word (aligned); CDE locks cover a 128-byte segment. */
    if (addr >= OTP_TOTAL_LEN)
        return E_OVERFLOW;
    if (addr < OTP_LEN && (addr % WORD_SIZE) != 0)
        return E_ALIGN;

    /* Same rule as pufs_otp_read(): no lock state while a program is in flight. */
    if (rt_wait_idle(&status) != SUCCESS) {
        LOG_ERROR("OTP LOCK QUERY BUSY: addr=0x%03x status=0x%08" PRIx32, addr, status);
        return E_BUSY;
    }

    *lock = (addr < OTP_LEN) ? pufs_get_otp_rwlck(addr)
                             : rt_cde_read_lock(addr - OTP_LEN);
    return SUCCESS;
}

pufs_otp_lock_t pufs_otp_get_lock(pufs_otp_addr_t addr)
{
    pufs_otp_lock_t lock;

    return (pufs_otp_query_lock(addr, &lock) == SUCCESS) ? lock : N_OTP_LOCK_T;
}

pufs_status_t pufs_otp_set_lock(pufs_otp_addr_t addr, uint32_t len,
    pufs_otp_lock_t lock)
{
    pufs_status_t check;
    if ((check = otp_unified_range_check(addr, len)) != SUCCESS)
        return check;
    if (addr < OTP_LEN)
        return pufs_lock_otp(addr, len, lock);
    if (lock == NA)
        return E_INVALID; /* CDE has no NA state */
    return rt_cde_write_lock(addr - OTP_LEN, len, lock);
}

/*
 * Raw pufs_otp_read()/write() data keeps the existing PUFS byte contract.
 * For the first two RT config words (0x0000-0x0007), the leaf RT path has
 * already swapped the PUFS big-endian hardware word into the host buffer, so
 * the logical config word needs one more byte-order reversal here. Later
 * config words use the raw buffer bytes directly.
 */
static bool otp_config_word_needs_byte_reverse(pufs_otp_addr_t addr)
{
    return addr < 0x0008;
}

static uint32_t otp_config_word_from_bytes(pufs_otp_addr_t addr,
    const uint8_t word_bytes[WORD_SIZE])
{
    uint32_t word;

    if (otp_config_word_needs_byte_reverse(addr)) {
        return ((uint32_t)word_bytes[0] << 24) |
               ((uint32_t)word_bytes[1] << 16) |
               ((uint32_t)word_bytes[2] << 8) |
               ((uint32_t)word_bytes[3]);
    }

    memcpy(&word, word_bytes, sizeof(word));
    return word;
}

static void otp_config_word_to_bytes(pufs_otp_addr_t addr,
    uint8_t word_bytes[WORD_SIZE], uint32_t word)
{
    if (otp_config_word_needs_byte_reverse(addr)) {
        word_bytes[0] = (word >> 24) & 0xff;
        word_bytes[1] = (word >> 16) & 0xff;
        word_bytes[2] = (word >> 8) & 0xff;
        word_bytes[3] = word & 0xff;
        return;
    }

    memcpy(word_bytes, &word, sizeof(word));
}

/* Config words 0x0000-0x000F, all in RT group 0: one guarded read covers them. */
#define OTP_CFG_LEN 16

static pufs_status_t otp_read_config_words(uint8_t cfg[OTP_CFG_LEN])
{
    /* Returns E_BUSY while a program is in flight, so a retry cannot skip the
     * write on a word whose earlier program has not finished. */
    return pufs_otp_read(cfg, OTP_CFG_LEN, 0);
}

static pufs_status_t otp_update_config_word(const uint8_t cfg[OTP_CFG_LEN],
    pufs_otp_addr_t addr, uint32_t logical_set_bits)
{
    uint8_t word_bytes[WORD_SIZE];
    uint32_t raw_word = otp_config_word_from_bytes(addr, cfg + addr);

    /* Config words are updated by directly setting the documented bit from
     * 0 -> 1 while preserving the rest of the word.
     */
    if ((raw_word | logical_set_bits) == raw_word)
        return SUCCESS;

    otp_config_word_to_bytes(addr, word_bytes, raw_word | logical_set_bits);
    return pufs_otp_write(word_bytes, sizeof(word_bytes), addr);
}

pufs_status_t pufs_otp_apply_security_config(bool disable_spi2axi,
    bool disable_jtag,
    bool force_secure_boot, bool disable_isp)
{
    pufs_status_t check;
    uint32_t boot_ctrl_bits = 0;
    uint8_t cfg[OTP_CFG_LEN];

    if (force_secure_boot)
        boot_ctrl_bits |= OTP_CFG_FORCE_SECURE_BOOT_BIT;

    if (disable_isp)
        boot_ctrl_bits |= OTP_CFG_DISABLE_ISP_BIT;

    if (!disable_spi2axi && !disable_jtag && boot_ctrl_bits == 0)
        return SUCCESS;

    /* Each word is programmed separately; one word's program does not change
     * the others, so a single read up front is enough. */
    if ((check = otp_read_config_words(cfg)) != SUCCESS)
        return check;

    if (disable_spi2axi)
    {
        check = otp_update_config_word(cfg, OTP_CFG_SPI2AXI_ADDR,
                                       OTP_CFG_DISABLE_SPI2AXI_BIT | 7 << 8);
        if (check != SUCCESS)
            return check;
    }

    if (disable_jtag)
    {
        check = otp_update_config_word(cfg, OTP_CFG_JTAG_ADDR,
                                       OTP_CFG_JTAG_DISABLE_BIT);
        if (check != SUCCESS)
            return check;
    }

    if (boot_ctrl_bits != 0)
        return otp_update_config_word(cfg, OTP_CFG_BOOT_CTRL_ADDR,
                                      boot_ctrl_bits);

    return SUCCESS;
}

pufs_status_t pufs_otp_disable_spi2axi(void)
{
    return pufs_otp_apply_security_config(true, false, false, false);
}

pufs_status_t pufs_otp_disable_jtag(void)
{
    return pufs_otp_apply_security_config(false, true, false, false);
}

pufs_status_t pufs_otp_force_secure_boot(void)
{
    return pufs_otp_apply_security_config(false, false, true, false);
}

pufs_status_t pufs_otp_disable_isp(void)
{
    return pufs_otp_apply_security_config(false, false, false, true);
}

pufs_status_t pufs_otp_get_security_config_state(
    pufs_otp_security_state_st *state)
{
    pufs_status_t check;
    uint8_t cfg[OTP_CFG_LEN];
    uint32_t raw_word;

    if (state == NULL)
        return E_INVALID;

    memset(state, 0, sizeof(*state));

    if ((check = otp_read_config_words(cfg)) != SUCCESS)
        return check;

    raw_word = otp_config_word_from_bytes(OTP_CFG_SPI2AXI_ADDR, cfg + OTP_CFG_SPI2AXI_ADDR);
    state->disable_spi2axi = (raw_word & OTP_CFG_DISABLE_SPI2AXI_BIT) ? 1 : 0;
    if ((check = pufs_otp_query_lock(OTP_CFG_SPI2AXI_ADDR, &state->spi2axi_word_lock)) != SUCCESS)
        return check;

    raw_word = otp_config_word_from_bytes(OTP_CFG_JTAG_ADDR, cfg + OTP_CFG_JTAG_ADDR);
    state->disable_jtag = (raw_word & OTP_CFG_JTAG_DISABLE_BIT) ? 1 : 0;
    if ((check = pufs_otp_query_lock(OTP_CFG_JTAG_ADDR, &state->jtag_word_lock)) != SUCCESS)
        return check;

    raw_word = otp_config_word_from_bytes(OTP_CFG_BOOT_CTRL_ADDR, cfg + OTP_CFG_BOOT_CTRL_ADDR);
    state->force_secure_boot =
        (raw_word & OTP_CFG_FORCE_SECURE_BOOT_BIT) ? 1 : 0;
    state->disable_isp = (raw_word & OTP_CFG_DISABLE_ISP_BIT) ? 1 : 0;
    if ((check = pufs_otp_query_lock(OTP_CFG_BOOT_CTRL_ADDR, &state->boot_ctrl_word_lock)) != SUCCESS)
        return check;

    return SUCCESS;
}

pufs_status_t pufs_otp_lock_security_config_words(void)
{
    pufs_status_t check;

    check = pufs_otp_set_lock(OTP_CFG_SPI2AXI_ADDR, WORD_SIZE, RO);
    if (check != SUCCESS)
        return check;

    check = pufs_otp_set_lock(OTP_CFG_JTAG_ADDR, WORD_SIZE, RO);
    if (check != SUCCESS)
        return check;

    return pufs_otp_set_lock(OTP_CFG_BOOT_CTRL_ADDR, WORD_SIZE, RO);
}
