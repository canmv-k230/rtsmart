/**
 * @file      pufs_ka_internal.h
 * @brief     PUFsecurity KA internal interface
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

#ifndef __PUFS_KA_INTERNAL_H__
#define __PUFS_KA_INTERNAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "pufs_ka.h"
#include "pufs_ka_regs.h"
#include "pufs_kwp_regs.h"

/*****************************************************************************
 * Macros
 ****************************************************************************/

#define KA_VERSION  0x505001A0
#define KWP_VERSION 0x33384600

/*****************************************************************************
 * Variables
 ****************************************************************************/

extern struct pufs_ka_regs  *ka_regs;
extern struct pufs_kwp_regs *kwp_regs;

pufs_status_t pufs_wrapped_key_size(pufs_key_wrap_t keywrap, uint32_t keybits,
                                  uint32_t *bytes);

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * @brief check if key slot settings is available
 *
 * @param[in] valid    Check the KA register to ensure the key is valid if true.
 * @param[in] keytype  The type of source which the key is from.
 * @param[in] slot     The slot of the source which the key is stored in.
 * @param[in] keybits  The key length in bits.
 * @return SUCCESS on success, otherwise an error code.
 */
#define keyslot_check(valid, keytype, ...) \
    _keyslot_check(valid, keytype, DEF_ARG((uint32_t)__VA_ARGS__, 0))
pufs_status_t _keyslot_check(bool valid, pufs_key_type_t keytype, uint32_t slot,
                             uint32_t keybits);
/**
 * @brief get the key slot index to be set in register interface
 *
 * @param keytype The type of source which the key is from.
 * @param slot The slot of the source which the key is stored in.
 * @param keybits The key length in bits.
 * @return SUCCESS on success, otherwise an error code.
 */
int get_key_slot_idx(pufs_key_type_t keytype, uint32_t keyslot);

/**
 * @brief get the key bit length in the shared secret slot
 *
 * @param[out] keybits  The key length in bits.
 * @param[in]  keyslot  The shared secret keyslot.
 * @return              SUCCESS on success, otherwise an error code.
 */
pufs_status_t pufs_get_ss_keybits(uint32_t* keybits, pufs_ka_slot_t keyslot);

#ifdef __cplusplus
} // closing brace for extern "C"
#endif

#endif /* __PUFS_KA_INTERNAL_H__ */
