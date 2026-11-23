/**
 * @file      pufs_rt_internal.h
 * @brief     PUFsecurity PUFrt internal interface
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

#ifndef __PUFS_RT_INTERNAL_H__
#define __PUFS_RT_INTERNAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "pufs_rt.h"
#include "pufs_rt_regs.h"

/*****************************************************************************
 * Macros
 ****************************************************************************/

#define PUFSRT_VERSION 0x504D30D6
#define WORD_SIZE 4 // 4 bytes = 32 bits = an otp block
#define RWLOCK_GROUP_OTP 8
#define MAX_RWLOCK_GROUPS OTP_LEN / WORD_SIZE / RWLOCK_GROUP_OTP  // a group contains 8 otp blocks

static inline uint32_t otp_chunk_len(uint32_t remaining)
{
    return remaining < WORD_SIZE ? remaining : WORD_SIZE;
}

/*****************************************************************************
 * Variables
 ****************************************************************************/

extern struct pufs_rt_regs *rt_regs;
extern uintptr_t rt_regs_phys;

/* CDE OTP area (0x400-0xFFF) */
#define PUFS_CDE_SEGMENT          128
#define PIF_CDE_RWLCK_START_INDEX   4

struct pufs_rt_cde_regs {
    volatile uint32_t otp[768];
};
extern struct pufs_rt_cde_regs *rt_cde_regs;
extern uintptr_t rt_cde_regs_phys;
void pufs_rt_cde_init(uint32_t rt_cde_offset);
pufs_status_t pufs_read_cde(uint8_t* outbuf, uint32_t len, uint32_t addr);
pufs_status_t pufs_program_cde(const uint8_t* inbuf, uint32_t len, uint32_t addr);
pufs_status_t rt_cde_write_lock(uint32_t offset, uint32_t length, pufs_otp_lock_t lock);
pufs_otp_lock_t rt_cde_read_lock(uint32_t offset);
pufs_status_t rt_cde_write_mask(uint32_t offset);

/* Shared by the RT and CDE program paths; flat_addr is only for logging. */
pufs_status_t rt_wait_idle(uint32_t *status);

/* SBI PMP permission for the RT register window (PIF, cfg, masks, RT OTP). */
pufs_status_t pufs_rt_pmp_set(unsigned long perm);
pufs_status_t pufs_rt_cde_pmp_set(unsigned long perm);
pufs_status_t pufs_recover_pmp(void);

/*
 * True when the calling thread holds the PUFS mutex (drv_pufs.c). The guard's
 * state and the RT PMP window are shared, so every guarded access needs it.
 */
bool pufs_drv_hw_lock_held(void);

/* Post-mask guard (pufs_rt.c): mask all OTP at init, open per access. */
pufs_status_t pufs_otp_guard_init(void);
pufs_status_t rt_otp_guard(uint32_t addr, uint32_t len, bool open);
pufs_status_t rt_otp_guard_keyslots(uint32_t slots, bool open);
uint32_t rt_otp_keyslot_mask(uint32_t slot, uint32_t keybits);
void rt_settle_before_remask(pufs_status_t check);
pufs_status_t rt_pgm_ign_enable(void);   /* RT_PUFS_OTP_WRITE_ENABLE builds only */
pufs_status_t rt_program_otp_word(volatile uint32_t *reg, uint32_t word,
    uint32_t flat_addr);

/*
 * One area of 4-bit lock fields in the PIF registers, 8 fields per register:
 * RT has one field per OTP word, CDE one per 128-byte segment.
 */
struct rt_lock_area {
    const char *name;           /* "OTP" or "CDE", for logs */
    uint32_t pif_base;          /* PIF register holding fields 0-7 */
    uint32_t fields;            /* number of lock fields */
    uint32_t field_bytes;       /* OTP bytes covered by one field */
    uint32_t flat_base;         /* flat OTP address of field 0 */
    pufs_otp_lock_t (*decode)(uint32_t field);
};
pufs_status_t rt_set_lock_fields(const struct rt_lock_area *area,
    uint32_t first, uint32_t last, pufs_otp_lock_t lock);

/*
 * One OTP data area for the program path: RT (one lock field per word, byte 0
 * packed into the high byte) or CDE (one lock field per 128-byte segment,
 * bytes in memory order). Offsets are relative to the area.
 */
struct rt_program_area {
    const char *name;                         /* "OTP" or "CDE", for logs */
    volatile uint32_t *otp;                   /* data word 0 */
    uint32_t size;                            /* bytes */
    uint32_t flat_base;                       /* flat OTP address of word 0 */
    bool big_endian;                          /* RT word packing */
    pufs_otp_lock_t (*lock)(uint32_t offset); /* lock state of a word */
    pufs_status_t (*pmp_set)(unsigned long perm); /* data register window */
};
pufs_status_t rt_program_area(const struct rt_program_area *area,
    const uint8_t *inbuf, uint32_t len, uint32_t offset);

/*****************************************************************************
 * Enumerations
 ****************************************************************************/
/**
 * @brief read mode
 */
typedef enum {
    RD,             ///< Read
    OFF_MG_RD,      ///< Off-margin read
    ON_MG_RD,       ///< On-margin read
    EXT_OFF_MG_RD,  ///< Extended off-margin read
    N_READ_MODE_T,
} pufs_read_mode_t;

typedef enum {
    PGMPRT_FLAG = 0xb6,
    TMLCK_FLAG = 0x71,
    OTPLCK_FLAG = 0x65,
    PUFLCK_FLAG = 0x2c,
    SHFREN_FLAG = 0x99,
    SHFWEN_FLAG = 0xc2,
    SET_SHFREN_FLAG = 0xe3,
    SET_SHFWEN_FLAG = 0x8e,
} pufs_ptm_flag_t;

typedef enum {
    PUFORG_0 = 0x12,
    PUFORG_1 = 0x13,
    PA_REP_0 = 0x38,
    PA_REP_1 = 0x39,
} pufs_ptm_pin_t;

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * @brief validate PUF slot
 *
 * @param slot The PUF slot which the key is stored in.
 * @return SUCCESS on success, otherwise an error code.
 */
pufs_status_t puf_slot_check(pufs_rt_slot_t slot);
/**
 * @brief validate OTP key slot by key length
 *
 * @param slot The OTP key slot which the key is stored in.
 * @param keybits The key length in bits.
 * @return SUCCESS on success, otherwise an error code.
 */
pufs_status_t otpkey_slot_check(pufs_rt_slot_t slot, uint32_t keybits);
/**
 * @brief wait PUFrt busy status
 *
 * @return  PTM status
 */
uint32_t wait_status(void);
/**
 * @brief Continuous random output control
 *
 * @param[in] on  Turn on/off continuous random output when true/false is
 *                specified.
 */
void pufs_rng_cont_ctrl(bool on);
/**
 * @brief Continuous entropy output control
 *
 * @param[in] on  Turn on/off continuous random output when true/false is
 *                specified.
 */
void pufs_fre_cont_ctrl(bool on);
/**
 * @brief PTR/PTC control
 *
 * @param[in] on  Turn on/off PTR/PTC access when true/false is specified.
 */
void pufs_ptr_ptc_ctrl(bool on);
/**
 * @brief Deep standby control
 *
 * @param[in] on  Turn on/off deep-standby mode when true/false is specified.
 */
void rt_write_pdstb(bool on);
/**
 * @brief Read mode control
 *
 * @param[in] mode  Read mode
 */
void rt_write_read_mode(pufs_read_mode_t mode);

void rt_write_enroll(void);

void rt_write_rngclk(bool enable);

pufs_status_t rt_write_set_flag(pufs_ptm_flag_t flag, uint32_t *status);

bool rt_check_enrolled(void);
bool rt_check_shfwen_enable(void);
bool rt_check_shfren_enable(void);
bool rt_check_pgmprt_enable(void);
bool rt_check_tmlck_enable(void);
bool rt_check_puflck_enable(void);
bool rt_check_otplck_enable(void);
bool rt_check_rngclk_enable(void);

#ifdef __cplusplus
} // closing brace for extern "C"
#endif

#endif /* __PUFS_RT_INTERNAL_H__ */
