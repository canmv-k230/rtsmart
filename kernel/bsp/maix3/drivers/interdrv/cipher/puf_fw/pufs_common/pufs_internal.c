/**
 * @file      pufs_internal.c
 * @brief     PUFsecurity common API implementation
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
#ifndef __USE_POSIX199309
#define __USE_POSIX199309
#endif /* __USE_POSIX199309 */
#include <time.h>
#include "pufs_internal.h"

/*****************************************************************************
 * Variables
 ****************************************************************************/
uint8_t pufs_buffer[BUFFER_SIZE];

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
/**
 * segment()
 */
blsegs segment(uint8_t* buf, uint32_t buflen, const uint8_t* in,
    uint32_t inlen, uint32_t blocksize, uint32_t minlen)
{
    blsegs ret = { .nsegs = 0 };

    // calculate total number of blocks to be processed
    uint32_t nprocblocks = 0;
    if ((buflen + inlen) >= (minlen + blocksize))
        nprocblocks = (buflen + inlen - minlen) / blocksize;

    // no available block for processing, keep input in the internal buffer.
    if (nprocblocks == 0) {
        ret.seg[ret.nsegs++] = (segstr) { false, buf, buflen };
        ret.seg[ret.nsegs++] = (segstr) { false, in, inlen };
        return ret;
    }

    const uint8_t* start = in;
    // some blocks are ready for processing,
    // using bytes in the internal buffer first
    if (buflen != 0) {
        // if all data in the internal buffer will be processed
        if (nprocblocks * blocksize >= buflen) {
            // fill buffer if not a complete block
            uint32_t proclen = blocksize;
            nprocblocks--;
            while (proclen < buflen) {
                proclen += blocksize;
                nprocblocks--;
            }
            rvv_memcpy(buf + buflen, start, proclen - buflen);
            ret.seg[ret.nsegs++] = (segstr) { true, buf, proclen };
            start += (proclen - buflen);
            inlen -= (proclen - buflen);
        } else // some data will be remained in the internal buffer
        {
            ret.seg[ret.nsegs++] = (segstr) { true, buf,
                nprocblocks * blocksize };
            ret.seg[ret.nsegs++] = (segstr) { false,
                buf + nprocblocks * blocksize,
                buflen - nprocblocks * blocksize };
            nprocblocks = 0;
        }
    }
    // deal with input data
    if (nprocblocks > 0) {
        ret.seg[ret.nsegs++] = (segstr) { true, start, nprocblocks * blocksize };
    }
    ret.seg[ret.nsegs++] = (segstr) { false, start + nprocblocks * blocksize,
        inlen - nprocblocks * blocksize };
    return ret;
}
/**
 * pin()
 */
void pin(const char* header, const uint8_t* content, const int len)
{
    printf("%s(%u) = 0x", header, len);
    for (int i = 0; i < len; i++)
        printf("%02x", *(content + i));
    printf("\n");
}
/**
 * version_check()
 */
void version_check(uint32_t version, uint32_t target)
{
    if (version != target)
        err(1, "current version 0x%x is not matched to supported version 0x%x", (unsigned int)target, (unsigned int)version);
}
