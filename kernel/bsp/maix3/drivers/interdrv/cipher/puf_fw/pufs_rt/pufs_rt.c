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
static size_t rwlck_index_sel(uint32_t idx)
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
 * @param keybits The length in bytes.
 * @return SUCCESS on success, otherwise an error code.
 */
static pufs_status_t otp_range_check(pufs_otp_addr_t addr, uint32_t len)
{
    // word-aligned OTP address check
    if ((addr % WORD_SIZE) != 0)
        return E_ALIGN;
    // OTP boundary check
    if ((len > OTP_LEN) || (addr > (OTP_LEN - len)))
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

static void puf_pgm_ign_ctrl(bool on)
{
    pufs_ptm_cfg_set(PTM_CFG_REG_PGM_IGN_MASK, on);
}

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
/**
 * @brief Get OTP rwlck value
 *
 * @param[in] addr  The address of the rwlck.
 * @return          The rwlck bits.
 */
pufs_otp_lock_t pufs_get_otp_rwlck(pufs_otp_addr_t addr)
{
    pufs_status_t check;

    if ((check = otp_range_check(addr, 4)) != SUCCESS)
        return check;

    // get rwlck
    int idx = addr / WORD_SIZE;
    int group_index = idx % RWLOCK_GROUP_OTP;

    if ((idx = rwlck_index_sel(idx)) == -1)
        return E_INVALID;

    uint32_t lck = (rt_regs->pif[idx] >> (group_index * WORD_SIZE)) & 0xF;

    switch (lck) {
    case PUFRT_VALUE4(0x0): return NA;
    case PUFRT_VALUE4(0xC): return RO;
    case PUFRT_VALUE4(0xF): return RW;
    default: return N_OTP_LOCK_T;
    }
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
    uint32_t* crand = (uint32_t*)rand;

    // Register manipulation
    for (uint32_t i = 0; i < numblks; i++)
        crand[i] = rt_regs->rn;

    return SUCCESS;
}

static const char *lock_state_name(pufs_otp_lock_t lock);

static pufs_status_t pufs_rt_pmp_set(unsigned long perm)
{
    struct sbi_ret ret = sbi_pmp_set(rt_regs_phys, 0x1000, perm);

    if (ret.error == SBI_SUCCESS)
        return SUCCESS;

    LOG_ERROR("SBI PMP set failed: addr=0x%lx len=0x%lx perm=0x%lx error=%ld value=%ld",
              (unsigned long)rt_regs_phys, (unsigned long)0x1000, perm,
              ret.error, ret.value);

    return (ret.error == SBI_ERR_DENIED) ? E_DENY : E_ERROR;
}

static uint32_t otp_pack_program_word(const uint8_t *inbuf, uint32_t offset,
    uint32_t len)
{
    union {
        uint32_t word;
        uint8_t byte[WORD_SIZE];
    } otp_word;

    for (int8_t index = WORD_SIZE - 1; index >= 0; index--)
        otp_word.byte[index] = ((offset + WORD_SIZE - 1 - index) < len) ?
            inbuf[offset + WORD_SIZE - 1 - index] : 0x00;

    return otp_word.word;
}

static void otp_word_to_bytes(uint8_t out[WORD_SIZE], uint32_t word)
{
    out[0] = (word >> 24) & 0xff;
    out[1] = (word >> 16) & 0xff;
    out[2] = (word >> 8) & 0xff;
    out[3] = word & 0xff;
}

/**
 * pufs_read_otp()
 */
pufs_status_t pufs_read_otp(uint8_t* outbuf, uint32_t len, pufs_otp_addr_t addr)
{
    pufs_status_t check;
    uint32_t word, start_index, wlen;

    if ((check = otp_range_check(addr, len)) != SUCCESS)
        return check;

    wlen = len / WORD_SIZE;
    start_index = addr / WORD_SIZE;

    if (wlen > 0) {
        uint32_t* out32 = (uint32_t*)outbuf;
        for (size_t i = 0; i < wlen; ++i)
            out32[i] = be2le(rt_regs->otp[start_index + i]);
    }

    if (len % WORD_SIZE != 0) {
        outbuf += wlen * WORD_SIZE;
        word = be2le(rt_regs->otp[start_index + wlen]);
        rt_memcpy(outbuf, &word, len % WORD_SIZE);
    }

    return SUCCESS;
}
/**
 * pufs_program_otp()
 */
pufs_status_t pufs_program_otp(const uint8_t* inbuf, uint32_t len,
    pufs_otp_addr_t addr)
{
    pufs_status_t check;
    uint32_t start_index = addr / WORD_SIZE;

    if ((check = otp_range_check(addr, len)) != SUCCESS)
        return check;

    // check lock state for entire range
    for (uint32_t off = 0; off < len; off += WORD_SIZE) {
        pufs_otp_lock_t lck = pufs_get_otp_rwlck(addr + off);
        if (lck != RW) {
            LOG_WARN("OTP WRITE DENIED: addr=0x%03x locked (%s)",
                     addr + off, lock_state_name(lck));
            return E_DENY;
        }
    }

    // check for 1->0 bit violations (RT OTP only goes 0->1)
    for (uint32_t i = 0; i < len; i += WORD_SIZE) {
        uint32_t cur = be2le(rt_regs->otp[start_index + (i / WORD_SIZE)]);
        for (uint32_t j = 0; j < WORD_SIZE && (i + j) < len; j++) {
            uint8_t cur_byte = (cur >> (j * 8)) & 0xFF;
            uint8_t new_byte = inbuf[i + j];
            if (cur_byte & ~new_byte) {
                LOG_WARN("OTP WRITE DENIED: addr=0x%03x+%u bit 1->0"
                         " (cur=0x%02x new=0x%02x)",
                         addr, i + j, cur_byte, new_byte);
                return E_DENY;
            }
        }
    }

#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("OTP WRITE DRY-RUN: addr=0x%03x len=%" PRIu32
             " (enable RT_PUFS_OTP_WRITE_ENABLE to commit)", addr, len);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t cur_word = rt_regs->otp[start_index + (i / 4)];
        uint32_t program_word = otp_pack_program_word(inbuf, i, len);
        uint8_t cur_bytes[WORD_SIZE];
        uint8_t write_bytes[WORD_SIZE];

        otp_word_to_bytes(cur_bytes, cur_word);
        otp_word_to_bytes(write_bytes, program_word);

        LOG_WARN("OTP WRITE DRY-RUN WORD: addr=0x%03x cur=[%02x %02x %02x %02x]"
                 " write=[%02x %02x %02x %02x]",
                 addr + i,
                 cur_bytes[0], cur_bytes[1], cur_bytes[2], cur_bytes[3],
                 write_bytes[0], write_bytes[1], write_bytes[2], write_bytes[3]);
    }
    return SUCCESS;
#else
    LOG_WARN("OTP WRITE: addr=0x%03x len=%" PRIu32, addr, len);

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    puf_pgm_ign_ctrl(true);

    // program
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t program_word = otp_pack_program_word(inbuf, i, len);

        if (program_word == 0x0)
            continue;
        if (rt_regs->otp[start_index + (i / 4)] == program_word)
            continue;

        rt_regs->otp[start_index + (i / 4)] = program_word;
    }

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R)) != SUCCESS)
        return check;

    return SUCCESS;
#endif
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

pufs_status_t pufs_lock_otp(pufs_otp_addr_t addr, uint32_t len,
    pufs_otp_lock_t lock)
{
    pufs_status_t check;
    uint32_t lock_val = 0, shift = 0, start = 0, end = 0, mask = 0, val32 = 0, rwlock_index;

    if ((check = otp_range_check(addr, len)) != SUCCESS)
        return check;

    switch (lock) {
    case NA:
        lock_val = PUFRT_VALUE4(0x0);
        break;
    case RO:
        lock_val = PUFRT_VALUE4(0xC);
        break;
    case RW:
        lock_val = PUFRT_VALUE4(0xF);
        break;
    default:
        return E_INVALID;
    }

#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("OTP LOCK DRY-RUN: addr=0x%03x len=%" PRIu32 " lock=%s"
             " (enable RT_PUFS_OTP_WRITE_ENABLE to commit)",
             addr, len, lock_state_name(lock));
    return SUCCESS;
#else
    LOG_WARN("OTP LOCK: addr=0x%03x len=%" PRIu32 " lock=%s",
             addr, len, lock_state_name(lock));

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    end = (len + 3) / 4;
    start = addr / WORD_SIZE;

    for (uint32_t i = 0; i < end; i++) {
        int idx = start + i;
        rwlock_index = rwlck_index_sel(idx);

        shift = (idx % RWLOCK_GROUP_OTP) * 4;
        val32 |= lock_val << shift;
        mask |= 0xF << shift;

        if (shift == 28 || i == end - 1) {
            val32 |= (rt_regs->pif[rwlock_index] & (~mask));
            rt_regs->pif[rwlock_index] = val32;

            val32 = 0;
            mask = 0;
        }
    }

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R)) != SUCCESS)
        return check;

    return SUCCESS;
#endif
}
/**
 * pufs_program_key2otp
 */
pufs_status_t pufs_program_key2otp(pufs_rt_slot_t slot, const uint8_t* key,
    uint32_t keybits, pufs_otp_lock_t lock)
{
    pufs_status_t check;

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
    LOG_WARN("ZEROIZE: slot=%u", slot);

    pufs_status_t check;

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    if (slot < OTPKEY_0)
        rt_regs->puf_zeroize = val32;
    else
        rt_regs->otp_zeroize = val32;

    val32 = wait_status();

    if ((val32 & 0x0000001e) != 0) {
        check = pufs_rt_pmp_set(SBI_PMP_PERM_R);
        if (check != SUCCESS)
            return check;

        LOG_ERROR("PUFRT status: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R)) != SUCCESS)
        return check;

    return SUCCESS;
#endif
}
/**
 * pufs_post_mask()
 */
pufs_status_t pufs_post_mask(uint64_t maskslots)
{
    uint32_t otp_psmsk_0, otp_psmsk_1, puf_psmsk, val32_0 = 0, val32_1 = 0;

#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("POST_MASK DRY-RUN: maskslots=0x%016" PRIx64
             " (enable RT_PUFS_OTP_WRITE_ENABLE to commit)", maskslots);
    return SUCCESS;
#else
    LOG_WARN("POST_MASK: maskslots=0x%016" PRIx64, maskslots);

    pufs_status_t check;

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    puf_psmsk = (maskslots & 0xf);
    otp_psmsk_0 = ((maskslots >> 4) & 0x0000ffff);
    otp_psmsk_1 = ((maskslots >> 20) & 0x0000ffff);

    for (int i = 0; i < 16; i++) {
        if ((otp_psmsk_0 >> i) & 0x1)
            val32_0 |= (0x3 << (2 * i));
        if ((otp_psmsk_1 >> i) & 0x1)
            val32_1 |= (0x3 << (2 * i));
    }

    rt_regs->otp_psmsk[0] = val32_0;
    rt_regs->otp_psmsk[1] = val32_1;

    // also enable post-masking
    val32_0 = PMK_LCK_PSMSK_MASK;
    for (int i = 0; i < 4; i++) {
        if ((puf_psmsk >> i) & 0x1)
            val32_0 |= (0x3 << (2 * i));
    }

    rt_regs->puf_psmsk = val32_0;

    if ((check = pufs_rt_pmp_set(SBI_PMP_PERM_R)) != SUCCESS)
        return check;

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
    if (addr + len > OTP_TOTAL_LEN)
        return E_OVERFLOW;
    if (addr < OTP_LEN && (addr + len) > OTP_LEN)
        return E_INVALID; /* cross-boundary RT/CDE not allowed */
    return SUCCESS;
}

/* Preserve the deployed raw byte contract of the leaf PUFS RT/CDE drivers. */
pufs_status_t pufs_otp_read(uint8_t* outbuf, uint32_t len, pufs_otp_addr_t addr)
{
    pufs_status_t check;
    if ((check = otp_unified_range_check(addr, len)) != SUCCESS)
        return check;
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
    /* protect BROM patch area (0x400-0xAFF) */
    if (addr >= OTP_CDE_START && addr < OTP_CDE_USER_START) {
        LOG_WARN("OTP WRITE DENIED: addr=0x%03x in BROM patch area", addr);
        return E_DENY;
    }
    if (addr < OTP_LEN)
        return pufs_program_otp(inbuf, len, addr);
    return pufs_program_cde(inbuf, len, addr - OTP_LEN);
}

pufs_otp_lock_t pufs_otp_get_lock(pufs_otp_addr_t addr)
{
    if (addr < OTP_LEN)
        return pufs_get_otp_rwlck(addr);
    if (addr >= OTP_TOTAL_LEN)
        return N_OTP_LOCK_T;
    return rt_cde_read_lock(addr - OTP_LEN);
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

static pufs_status_t otp_update_config_word(pufs_otp_addr_t addr,
    uint32_t logical_set_bits)
{
    pufs_status_t check;
    uint8_t word_bytes[WORD_SIZE] = {0};
    uint32_t raw_word;
    uint32_t updated_raw_word;

    if ((check = pufs_otp_read(word_bytes, sizeof(word_bytes), addr)) != SUCCESS)
        return check;

    raw_word = otp_config_word_from_bytes(addr, word_bytes);

    /* Config words are updated by directly setting the documented bit from
     * 0 -> 1 while preserving the rest of the word.
     */
    updated_raw_word = raw_word | logical_set_bits;

    if (updated_raw_word != raw_word)
    {
        otp_config_word_to_bytes(addr, word_bytes, updated_raw_word);
        check = pufs_otp_write(word_bytes, sizeof(word_bytes), addr);
        if (check != SUCCESS)
            return check;
    }

    return SUCCESS;
}

pufs_status_t pufs_otp_apply_security_config(bool disable_spi2axi,
    bool disable_jtag,
    bool force_secure_boot, bool disable_isp)
{
    pufs_status_t check;
    uint32_t boot_ctrl_bits = 0;

    if (disable_spi2axi)
    {
        check = otp_update_config_word(OTP_CFG_SPI2AXI_ADDR,
                                       OTP_CFG_DISABLE_SPI2AXI_BIT | 7 << 8);
        if (check != SUCCESS)
            return check;
    }

    if (disable_jtag)
    {
        check = otp_update_config_word(OTP_CFG_JTAG_ADDR,
                                       OTP_CFG_JTAG_DISABLE_BIT);
        if (check != SUCCESS)
            return check;
    }

    if (force_secure_boot)
        boot_ctrl_bits |= OTP_CFG_FORCE_SECURE_BOOT_BIT;

    if (disable_isp)
        boot_ctrl_bits |= OTP_CFG_DISABLE_ISP_BIT;

    if (boot_ctrl_bits != 0)
        return otp_update_config_word(OTP_CFG_BOOT_CTRL_ADDR,
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
    uint8_t word_bytes[WORD_SIZE];
    uint32_t raw_word;

    if (state == NULL)
        return E_INVALID;

    memset(state, 0, sizeof(*state));

    check = pufs_otp_read(word_bytes, sizeof(word_bytes), OTP_CFG_SPI2AXI_ADDR);
    if (check != SUCCESS)
        return check;

    raw_word = otp_config_word_from_bytes(OTP_CFG_SPI2AXI_ADDR, word_bytes);
    state->disable_spi2axi = (raw_word & OTP_CFG_DISABLE_SPI2AXI_BIT) ? 1 : 0;
    state->spi2axi_word_lock = pufs_otp_get_lock(OTP_CFG_SPI2AXI_ADDR);

    check = pufs_otp_read(word_bytes, sizeof(word_bytes), OTP_CFG_JTAG_ADDR);
    if (check != SUCCESS)
        return check;

    raw_word = otp_config_word_from_bytes(OTP_CFG_JTAG_ADDR, word_bytes);
    state->disable_jtag = (raw_word & OTP_CFG_JTAG_DISABLE_BIT) ? 1 : 0;
    state->jtag_word_lock = pufs_otp_get_lock(OTP_CFG_JTAG_ADDR);

    check = pufs_otp_read(word_bytes, sizeof(word_bytes), OTP_CFG_BOOT_CTRL_ADDR);
    if (check != SUCCESS)
        return check;

    raw_word = otp_config_word_from_bytes(OTP_CFG_BOOT_CTRL_ADDR, word_bytes);
    state->force_secure_boot =
        (raw_word & OTP_CFG_FORCE_SECURE_BOOT_BIT) ? 1 : 0;
    state->disable_isp = (raw_word & OTP_CFG_DISABLE_ISP_BIT) ? 1 : 0;
    state->boot_ctrl_word_lock = pufs_otp_get_lock(OTP_CFG_BOOT_CTRL_ADDR);

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
