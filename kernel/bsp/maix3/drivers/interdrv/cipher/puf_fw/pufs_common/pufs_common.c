/**
 * @file      pufs_common.c
 * @brief     PUFsecurity API implementation
 * @copyright 2021 PUFsecurity
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
#include "pufs_reg_ctrl.h"
#include "pufs_internal.h"

/*****************************************************************************
 * Variables
 ****************************************************************************/

static char* status_msg[] = {
    "",
    "Address alignment mismatch",
    "Space overflow",
    "Size too small",
    "Invalid argument",
    "Resource is occupied",
    "Resource is unavailable",
    "Firmware error",
    "Invalid public key or digital signature",
    "Invalid ECC microprogram",
    "Access denied",
    "Not support",
    "Point at infinity",
    "Unspecific error",
};

struct pufs_context pufs_context = { .base_addr = 0x0, .phys_base_addr = 0x0 };

/*****************************************************************************
 * API functions
 ****************************************************************************/

void pufs_module_init(uintptr_t base_addr, uintptr_t phys_base_addr, size_t size)
{
#ifndef BAREMETAL
    pufs_context.base_addr = (uintptr_t)get_mapped_addr(base_addr, &size);
    pufs_context.phys_base_addr = base_addr;
#else
    pufs_context.base_addr = base_addr;
    pufs_context.phys_base_addr = phys_base_addr;
#endif /* BAREMETAL */
    pufs_context.size = size;
}

void pufs_release(void)
{
#ifndef BAREMETAL
    if (munmap((void*)pufs_context.base_addr, pufs_context.size) == -1)
        err(1, "munmap 0x%08" PRIxPTR " failed", pufs_context.base_addr);
#endif /* BAREMETAL */
}

char* pufs_strstatus(pufs_status_t status)
{
    return status_msg[status];
}