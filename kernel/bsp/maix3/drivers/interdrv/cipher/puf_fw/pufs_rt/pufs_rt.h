/**
 * @file      pufs_rt.h
 * @brief     PUFsecurity PUFrt API interface
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

#ifndef __PUFS_RT_H__
#define __PUFS_RT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "pufs_common.h"

#ifndef DOXYGEN
    // Note: use PUFRT_DEFAULT_ZERO if the default bit of PUF/OTP area is 0
    #define PUFRT_DEFAULT_ZERO 1
    #define PSIOT_012CW01D_B12C 1
#endif

/*****************************************************************************
 * Macros
 ****************************************************************************/
#if defined(PUFRT_DEFAULT_ZERO)
    #define PUFRT_VALUE4(value) (~value & 0xF)
    #define PUFRT_VALUE8(value) (~value & 0xFF)
    #define PUFRT_VALUE32(value) ((uint32_t)~value)
#else
    #define PUFRT_VALUE4(value) (value)
    #define PUFRT_VALUE8(value) (value)
    #define PUFRT_VALUE32(value) (value)
#endif
/**
 * @brief Initialize PUFsrt module
 *
 * @param[in] rt_offset  PUFsrt offset of memory map
 */
void pufs_rt_module_init(uint32_t rt_offset);
/**
 * @brief The size of whole OTP cells in bytes
 */
#define OTP_LEN 1024
/**
 * @brief The size of a OTP key
 */
#define OTP_KEY_BITS 256
#define OTP_KEY_LEN OTP_KEY_BITS / 8
#define OTP_CDE_START 0x400
#define OTP_CDE_USER_START 0xC00 // user can use 1KiB from 0xC00 to 0xFFF, which is divided into 8 segments of 128 bytes each, and each segment can be locked separately
#define OTP_TOTAL_LEN 4096
/**
 * @brief Construct mask bit for pufs_post_mask()
 *
 * @param[in] slot  One of \ref pufs_rt_slot_t elements.
 * @return          The mask bit used in pufs_post_mask().
 */
#define MASK_BIT(slot) (1ULL<<(slot))
/**
 * @brief Check and output test result for testing functions
 *
 * @param[in] text  Description for test item.
 * @param[in] good  Description for succeeded test.
 */
#define CHECKOUT(text, good) \
    do { \
        fprintf(stderr, "  %s ... %s\n", text, ((check == SUCCESS) ? good : "failed")); \
        if (check != SUCCESS) \
            goto cleanup; \
    } while (0)

/*****************************************************************************
 * Enumerations
 ****************************************************************************/
/**
 * @brief OTP lock states
 */
typedef enum {
    NA,  ///< No-Access
    RO,  ///< Read-Only
    RW,  ///< Read-Write
    N_OTP_LOCK_T,
} pufs_otp_lock_t;
/**
 * @brief PUFrt slots
 */
typedef enum {
    // PUF slots
    PUFSLOT_0, ///< PUF slot 0, 256 bits
    PUFSLOT_1, ///< PUF slot 1, 256 bits
    PUFSLOT_2, ///< PUF slot 2, 256 bits
    PUFSLOT_3, ///< PUF slot 3, 256 bits
    // OTP key slots
    OTPKEY_0,  ///< OTP key slot 0, 256 bits, used by BROM
    OTPKEY_1,  ///< OTP key slot 1, 256 bits, used by BROM
    OTPKEY_2,  ///< OTP key slot 2, 256 bits, use for secure boot, store bootloader aes256 key 1
    OTPKEY_3,  ///< OTP key slot 3, 256 bits, use for secure boot, store bootloader aes256 key 2
    OTPKEY_4,  ///< OTP key slot 4, 256 bits, use for secure boot, store bootloader sm4 key 1
    OTPKEY_5,  ///< OTP key slot 5, 256 bits, use for secure boot, store bootloader sm4 key 2
    OTPKEY_6,  ///< OTP key slot 6, 256 bits, use for secure boot, store bootloader rsa pubkey hash
    OTPKEY_7,  ///< OTP key slot 7, 256 bits, use for secure boot, store bootloader sm2 pubkey hash
    OTPKEY_8,  ///< OTP key slot 8, 256 bits, below OTP key slots are for general use, not used by BROM
    OTPKEY_9,  ///< OTP key slot 9, 256 bits
    OTPKEY_10, ///< OTP key slot 10, 256 bits
    OTPKEY_11, ///< OTP key slot 11, 256 bits
    OTPKEY_12, ///< OTP key slot 12, 256 bits
    OTPKEY_13, ///< OTP key slot 13, 256 bits
    OTPKEY_14, ///< OTP key slot 14, 256 bits
    OTPKEY_15, ///< OTP key slot 15, 256 bits
    OTPKEY_16, ///< OTP key slot 16, 256 bits
    OTPKEY_17, ///< OTP key slot 17, 256 bits
    OTPKEY_18, ///< OTP key slot 18, 256 bits
    OTPKEY_19, ///< OTP key slot 19, 256 bits
    OTPKEY_20, ///< OTP key slot 20, 256 bits
    OTPKEY_21, ///< OTP key slot 21, 256 bits
    OTPKEY_22, ///< OTP key slot 22, 256 bits
    OTPKEY_23, ///< OTP key slot 23, 256 bits
    OTPKEY_24, ///< OTP key slot 24, 256 bits
    OTPKEY_25, ///< OTP key slot 25, 256 bits
    OTPKEY_26, ///< OTP key slot 26, 256 bits
    OTPKEY_27, ///< OTP key slot 27, 256 bits
    OTPKEY_28, ///< OTP key slot 28, 256 bits
    OTPKEY_29, ///< OTP key slot 29, 256 bits
    OTPKEY_30, ///< OTP key slot 30, 256 bits
    OTPKEY_31, ///< OTP key slot 31, 256 bits
} pufs_rt_slot_t;

/*****************************************************************************
 * Structures
 ****************************************************************************/
/**
 * @brief PUF UID length in bytes.
 */
#define UIDLEN 32
/**
 * @brief PUF UID.
 */
typedef struct {
    uint8_t uid[UIDLEN]; ///< UID container
} pufs_uid_st;
/**
 * @brief OTP addressing type.
 */
typedef uint16_t pufs_otp_addr_t;

/*****************************************************************************
 * API functions
 ****************************************************************************/
/**
 * @brief Wrapper function of _pufs_get_uid() to set PUFSLOT_0 as the default
 *        value of the last parameter if not provided.
 */
#define pufs_get_uid(...) \
    _pufs_get_uid(DEF_ARG(__VA_ARGS__, PUFSLOT_0))
/**
 * @brief Export the unique device identity (256-bit).
 *
 * @param[out] uid   The unique device identity.
 * @param[in]  slot  PUF slot.
 * @return           SUCCESS on success, otherwise an error code.
 *
 * @remark Use the wrapper function pufs_get_uid() for convenience.
 *
 * @note In PUFiot, only PUFSLOT_0 is available for read. Other 3 PUF slots are
 *        reserved for internal use in cryptographic engines.
 */
pufs_status_t _pufs_get_uid(pufs_uid_st* uid, pufs_rt_slot_t slot);
/**
 * @brief Wrapper function of _pufs_rand() to set 1 as the default value of the
 *        last parameter if not provided.
 */
#define pufs_rand(...) \
    _pufs_rand(DEF_ARG(__VA_ARGS__, 1))
/**
 * @brief Read 32-bit random blocks from RNG.
 *
 * @param[out] rand     Output random blocks
 * @param[in]  numblks  Number of blocks to be generated, each block 32 bits.
 * @return              SUCCESS on success, otherwise an error code.
 *
 * @remark Use the wrapper function pufs_rand() for convenience.
 */
pufs_status_t _pufs_rand(uint8_t* rand, uint32_t numblks);
/**
 * @brief Read from OTP with boundary check.
 *
 * @param[out] outbuf  OTP data.
 * @param[in]  len     The length of data in bytes.
 * @param[in]  addr    Sarting address of the read.
 * @return             SUCCESS on success, otherwise an error code.
 *
 * @note \em addr must be aligned to 4 bytes boundary
 */
pufs_status_t pufs_read_otp(uint8_t* outbuf, uint32_t len, pufs_otp_addr_t addr);
/**
 * @brief Write to OTP with boundary check.
 *
 * @param[in] inbuf  The data to be written to OTP.
 * @param[in] len    The length of data in bytes.
 * @param[in] addr   Starting OTP address to be programmed.
 * @return           SUCCESS on success, otherwise an error code.
 *
 * @note \em addr must be aligned to 4 bytes boundary
 */
pufs_status_t pufs_program_otp(const uint8_t* inbuf, uint32_t len,
                               pufs_otp_addr_t addr);
/**
 * @brief Set OTP lock state
 *
 * @param[in] addr  Starting OTP address lock state to be set.
 * @param[in] len   The length of OTP data in bytes.
 * @param[in] lock  The lock state.
 * @return          SUCCESS on success, otherwise an error code.
 *
 * @note \em addr must be aligned to 4 bytes boundary
 */
pufs_status_t pufs_lock_otp(pufs_otp_addr_t addr, uint32_t len,
                            pufs_otp_lock_t lock);
/**
 * @brief Import a cleartext key into OTP with boundary check.
 *
 * @param[in] slot     OTP key slot.
 * @param[in] key      The plaintext key to be imported.
 * @param[in] keybits  Key length in bits. (max key bit length: 2047)
 * @param[in] lock     Lock state after programming. Use N_OTP_LOCK_T to skip
 *                     locking, NA to set No-Access, RO for Read-Only.
 * @return             SUCCESS on success, otherwise an error code.
 *
 * @note Each OTP key slot is 256-bit. For a key of length \f$b\f$ bits, the
 *       slot number \em n (starting with 0) MUST be a multiple of \f$2^k\f$
 *       where \f$k\f$ is the smallest integer such that \f$b \le 256 \cdot
 *       2^k\f$. For example, a 384-bit key can be programmed in OTPKEY_0,
 *       OTPKEY_2, and so forth.
 */
pufs_status_t pufs_program_key2otp(pufs_rt_slot_t slot, const uint8_t* key,
                                   uint32_t keybits, pufs_otp_lock_t lock);
/**
 * @brief Zeroize PUF slot
 *
 * @param[in] slot  PUF slot.
 * @return          SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_zeroize(pufs_rt_slot_t slot);
/**
 * @brief PUFrt post masking
 *
 * @param[in] maskslots  The bitmap of \ref pufs_rt_slot_t slots to be masked.
 * @return               SUCCESS on success, otherwise an error code.
 *
 * @note \em maskslots is constructed by bit-wise or of \ref MASK_BIT outputs
 *       of the PUF slots/OTP key slots. For example, if PUFSLOT_1 and OTPKEY_2
 *       is designed to be masked, the input is \n
 *        MASK_BIT(PUFSLOT_1) | MASK_BIT(OTPKEY_2)
 */
pufs_status_t pufs_post_mask(uint64_t maskslots);
/**
 * @brief Read version and features register value
 *
 * @param[out] version   Version register value
 * @param[out] features  Features register value
 * @return               SUCCESS.
 */
pufs_status_t pufs_rt_version(uint32_t* version, uint32_t* features);
/**
 * @brief Get OTP rwlck value
 *
 * @param[in] addr  The address of the rwlck.
 * @return          The rwlck bits.
 */
pufs_otp_lock_t pufs_get_otp_rwlck(pufs_otp_addr_t addr);
/**
 * @brief Unified OTP read (flat 0-4095 address space)
 *
 * Dispatches to RT (0x000-0x3FF) or CDE (0x400-0xFFF).
 *
 * @param[out] outbuf  OTP data.
 * @param[in]  len     The length of data in bytes.
 * @param[in]  addr    Starting address (0-4095).
 * @return             SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_otp_read(uint8_t* outbuf, uint32_t len, pufs_otp_addr_t addr);
/**
 * @brief Unified OTP write (flat 0-4095 address space)
 *
 * Dispatches to RT (0x000-0x3FF) or CDE (0x400-0xFFF).
 * BROM patch area (0x400-0xAFF) is write-protected.
 *
 * @param[in] inbuf  The data to be written.
 * @param[in] len    The length of data in bytes.
 * @param[in] addr   Starting address (0-4095).
 * @return           SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_otp_write(const uint8_t* inbuf, uint32_t len,
                             pufs_otp_addr_t addr);
/**
 * @brief Unified OTP lock get (flat 0-4095 address space)
 *
 * @param[in] addr  The address (0-4095).
 * @return          The lock state.
 */
pufs_otp_lock_t pufs_otp_get_lock(pufs_otp_addr_t addr);
/**
 * @brief Unified OTP lock set (flat 0-4095 address space)
 *
 * @param[in] addr  Starting address (0-4095).
 * @param[in] len   Length in bytes.
 * @param[in] lock  The lock state.
 * @return          SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_otp_set_lock(pufs_otp_addr_t addr, uint32_t len,
                                pufs_otp_lock_t lock);

#ifdef PSIOT_012CW01D_B12C

void pufs_rt_cde_init(uint32_t rt_cde_offset);

#endif

#ifdef __cplusplus
} // closing brace for extern "C"
#endif

#endif /* __PUFS_RT_H__ */
