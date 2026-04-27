/**
 * @file      pufs_internal.h
 * @brief     PUFsecurity common internal interface
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

#ifndef __PUFS_INTERNAL_H__
#define __PUFS_INTERNAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "pufs_common.h"
#include "pufs_bare.h"
#include "pufs_reg_ctrl.h"
#include "pufs_log.h"

#include "rvv_ops.h"

/*****************************************************************************
 * Macros
 ****************************************************************************/
#define UNUSED(x) (void)(x)
#define CHUNK_MAXLEN 65536

/*****************************************************************************
 * Structures
 ****************************************************************************/
typedef struct {
    bool process;
    const uint8_t* addr;
    uint32_t len;
} segstr;

typedef struct {
    uint32_t nsegs;
    segstr seg[3];
} blsegs;

struct pufs_context {
    uintptr_t base_addr;
    uintptr_t phys_base_addr;
    size_t    size;
};

/*****************************************************************************
 * Variables
 ****************************************************************************/
extern struct pufs_context pufs_context;
#ifndef BUFFER_SIZE
#define BUFFER_SIZE 512 // To ensure PKC works, the minimum buffer size is 512
#endif
extern uint8_t pufs_buffer[BUFFER_SIZE];

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
void version_check(uint32_t version, uint32_t target);
/**
 * @brief Buffer manipulation for blockwise operations
 *
 * @param buf The pointer to the internal buffer.
 * @param buflen The length of data in the internal buffer in bytes.
 * @param in The pointer to the input data.
 * @param inlen The length of input data in bytes.
 * @param blocksize The blocksize in bytes for HW processing.
 * @param minlen The minimum length should be kept in internal buffer.
 * @return segments for blockwise operation or buffering data
 */
blsegs segment(uint8_t* buf, uint32_t buflen, const uint8_t* in, uint32_t inlen,
               uint32_t blocksize, uint32_t minlen);
/**
 * @brief Print byte array as "header(len): 0xXXXXX..."
 *
 * @param header The header
 * @param content The byte array
 * @param len The size of the byte array in bytes.
 */
void pin(const char* header, const uint8_t* content, const int len);

#ifdef __cplusplus
} // closing brace for extern "C"
#endif

#endif /*__PUFS_INTERNAL_H__*/
