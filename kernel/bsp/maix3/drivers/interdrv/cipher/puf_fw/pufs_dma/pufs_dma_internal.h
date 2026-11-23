/**
 * @file      pufs_dma_internal.h
 * @brief     PUFsecurity DMA internal interface
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

#ifndef __PUFS_DMA_INTERNAL_H__
#define __PUFS_DMA_INTERNAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "pufs_dma.h"
#include "pufs_ka.h"
#include "pufs_dma_regs.h"

#define DMA_VERSION 0x505000A1

#define DEFAULT_DESC_MEM_SIZE 2048
#define SGDMA_DESCRIPTOR_SIZE   32

/*****************************************************************************
 * Macros
 ****************************************************************************/
#undef DMADIRECT
#ifdef BAREMETAL
#ifndef DMABUFFER
#define DMADIRECT
#endif /*DMABUFFER */
#endif /* BAREMETAL */

/*****************************************************************************
 * Structs
 ****************************************************************************/
struct dma_sg_mem
{
    uintptr_t base_addr;
    uintptr_t virt_base_addr;

    uint32_t desc_bitmap;
};

struct dma_desc_cfg
{
    uintptr_t base_addr;
    uintptr_t virt_addr;
    size_t    size;
    pufs_dma_dsc_attr_st attr;
};

struct pufs_dma
{
    struct pufs_dma_regs *regs;

    uintptr_t             read_addr;
    uintptr_t             read_virt_addr;

    uintptr_t             write_addr;
    uintptr_t             write_virt_addr;

    size_t                buff_size;
};

typedef struct dma_sg_internal_desc
{
    uint32_t write_addr;
    uint32_t read_addr;
    uint32_t length;
    uint32_t next;
    uint32_t cfg;
    uint32_t key_cfg;
    uint32_t cypt_cfg[2];
} pufs_dma_sg_internal_desc_st;

typedef struct dma_sg_desc_opts
{
    bool    head;
    bool    tail;
    bool    done_interrupt;
    bool    done_pause;
    bool    no_crypto;
    uint8_t offset;
}pufs_dma_sg_desc_opts_st;


/*****************************************************************************
 * Variables
 ****************************************************************************/
extern struct dma_sg_mem sg_mem;
extern struct pufs_dma pufs_dma;

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
#ifndef BAREMETAL
    #define VIRT_ADDR(addr) ((addr - sg_mem.base_addr) + sg_mem.virt_base_addr)
    #define PHY_ADDR(addr) ((addr - sg_mem.virt_base_addr) + sg_mem.base_addr)
    #define DMA_RBUF_VIRT_ADDR(addr) ((addr - pufs_dma.read_addr) + pufs_dma.read_virt_addr)
#else
     #define VIRT_ADDR(addr) (addr)
     #define PHY_ADDR(addr) (addr)
     #define DMA_RBUF_VIRT_ADDR(addr) (addr)
#endif

#ifndef DMADIRECT
void clear_dma_read(uint32_t len);
void dma_read_output(uint8_t* addr, uint32_t len);
#else
#define clear_dma_read(...)
#define dma_read_output(...)
#endif /* DMADIRECT */

pufs_status_t dma_write_rwcfg(const uint8_t *out, const uint8_t *in, uint32_t len);
pufs_status_t dma_write_sgcfg(pufs_dma_sg_desc_st *descs, uint32_t descs_len, pufs_dma_sg_desc_opts_st *opts);
void dma_write_key_config_0(pufs_key_type_t keytype, pufs_algo_type_t algo, uint32_t size, uint32_t slot_index);
uint32_t dma_key_slots(uint32_t cfg);
void dma_write_cl_config_0(uint32_t value);
void dma_write_config_0(bool rng_enable, bool sgdma_enable, bool no_cypt);
void dma_write_data_block_config(bool head, bool tail, bool dn_intrpt, bool dn_pause, uint32_t offset);
void dma_write_data_dsc_config(pufs_dma_sg_desc_opts_st *opts, pufs_dma_dsc_attr_st *attr, bool no_crypto);
pufs_status_t dma_run(void);
pufs_status_t dma_run_sg(uint32_t slots, bool *timed_out);

/*
 * key_cfg_0 encoding. Each field is masked to its width, so an out-of-range
 * size or slot cannot spill into the key source bits and select a different
 * key than the one validated (the guard opens slots decoded from this value).
 */
static inline uint32_t dma_key_cfg(pufs_key_type_t keytype, pufs_algo_type_t algo,
    uint32_t size, uint32_t slot_index)
{
    return ((slot_index << DMA_KEY_CFG_0_KEY_IDX_BITS) & DMA_KEY_CFG_0_KEY_IDX_MASK) |
           ((size << DMA_KEY_CFG_0_KEY_SIZE_BITS) & DMA_KEY_CFG_0_KEY_SIZE_MASK) |
           (((uint32_t)algo << DMA_KEY_CFG_0_KEY_DST_BITS) & DMA_KEY_CFG_0_KEY_DST_MASK) |
           ((uint32_t)keytype & DMA_KEY_CFG_0_KEY_SRC_MASK);
}
bool dma_check_busy_status(uint32_t *status);

pufs_dma_sg_internal_desc_st *dma_sg_new_read_ctx_descriptor(uintptr_t crypto_ctx);
pufs_dma_sg_internal_desc_st *dma_sg_new_data_descriptor(void);
pufs_dma_sg_internal_desc_st *dma_sg_new_write_ctx_descriptor(uintptr_t crypto_ctx);
void dma_sg_free_descriptor(pufs_dma_sg_internal_desc_st *desc);
void dma_sg_free_all_descriptor(void);
void dma_sg_desc_append_to_list(pufs_dma_sg_internal_desc_st *desc);

void dma_sg_desc_write_addr(pufs_dma_sg_internal_desc_st *desc,
                            uintptr_t write_addr,
                            uintptr_t read_addr,
                            uint32_t length);
void dma_sg_desc_write_dsc_config(pufs_dma_sg_internal_desc_st *desc,
                                  pufs_dma_dsc_attr_st *attr,
                                  pufs_dma_sg_desc_opts_st *opts);
void dma_sg_desc_write_key_config(pufs_dma_sg_internal_desc_st *desc,
                                  pufs_key_type_t keytype, pufs_algo_type_t algo,
                                  uint32_t size, uint32_t slot_index);
void dma_sg_desc_write_crypto_config(pufs_dma_sg_internal_desc_st *desc, uint32_t cfg0, uint32_t cfg1);

#ifdef __cplusplus
} // closing brace for extern "C"
#endif

#endif /*__PUFS_DMA_INTERNAL_H__*/
