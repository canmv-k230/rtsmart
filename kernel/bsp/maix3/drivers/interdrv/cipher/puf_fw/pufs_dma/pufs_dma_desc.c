/**
 * @file      pufs_dma_desc.c
 * @brief     PUFsecurity SGDMA descriptor functions
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
#include "pufs_internal.h"
#include "pufs_dma_internal.h"

/*****************************************************************************
 * Static variables
 ****************************************************************************/
struct dma_sg_mem sg_mem = { .base_addr = 0x0 };
static pufs_dma_sg_internal_desc_st *desc_list_head = NULL, *desc_list_tail = NULL;

/*****************************************************************************
 * Internal Functions
 ****************************************************************************/
static pufs_dma_sg_internal_desc_st* dma_sg_new_decriptor(void)
{
    uint32_t index;
    uintptr_t addr;
    for (index = 0; index < 32 && (sg_mem.desc_bitmap & (1 << index)) != 0; index++)
        ;

    if (index == 32) {
        LOG_ERROR("%s", "failed to new a descriptor. all descriptors are used");
        return NULL;
    }

    addr = VIRT_ADDR(sg_mem.base_addr + (SGDMA_DESCRIPTOR_SIZE * index));
    rvv_memset((void*)addr, 0x0, SGDMA_DESCRIPTOR_SIZE);

    sg_mem.desc_bitmap |= (1 << index);

    return (pufs_dma_sg_internal_desc_st*)(addr);
}

static inline void set_bit(uint32_t* addr, uint32_t offset, bool cfg)
{
    if (cfg)
        *addr |= (1 << offset);
    else
        *addr &= ~(1 << offset);
}

static void dma_sg_desc_write_dsc_pause(pufs_dma_sg_internal_desc_st* desc)
{
    uint32_t cfg = be2le(desc->cfg);
    set_bit(&cfg, DMA_DSC_CFG_4_DN_PAUSE_BITS, true);
    set_bit(&cfg, DMA_DSC_CFG_4_DN_INTRPT_BITS, true);
    desc->cfg = le2be(cfg);
}

void dma_sg_desc_write_addr(pufs_dma_sg_internal_desc_st* desc,
    uintptr_t write_addr,
    uintptr_t read_addr,
    uint32_t length)
{
    desc->write_addr = le2be(write_addr);
    desc->read_addr = le2be(read_addr);
    desc->length = le2be(length);
}

void dma_sg_desc_write_dsc_config(pufs_dma_sg_internal_desc_st* desc,
    pufs_dma_dsc_attr_st* attr,
    pufs_dma_sg_desc_opts_st* opts)
{
    uint32_t cfg = be2le(desc->cfg);

    if (opts) {
        set_bit(&cfg, DMA_DSC_CFG_4_HEAD_BITS, opts->head);
        set_bit(&cfg, DMA_DSC_CFG_4_TAIL_BITS, opts->tail);
        set_bit(&cfg, DMA_DSC_CFG_4_DN_INTRPT_BITS, opts->done_interrupt);
        set_bit(&cfg, DMA_DSC_CFG_4_DN_PAUSE_BITS, opts->done_pause);
        cfg &= ~(0xF << DMA_DSC_CFG_4_OFFSET_BITS);
        // the range of offset value is 0 - 15
        cfg |= (0xF & opts->offset) << DMA_DSC_CFG_4_OFFSET_BITS;
        set_bit(&cfg, DMA_DSC_CFG_4_NO_CRYP_BITS, opts->no_crypto);
    }

    if (attr) {
        set_bit(&cfg, DMA_DSC_CFG_4_FIX_READ_BITS, attr->fix_read_addr);
        set_bit(&cfg, DMA_DSC_CFG_4_FIX_WRITE_BITS, attr->fix_write_addr);
        cfg |= ((attr->read_protect << 8) | attr->write_protect);
    }

    desc->cfg = le2be(cfg);
}

void dma_sg_desc_write_key_config(pufs_dma_sg_internal_desc_st* desc,
    pufs_key_type_t keytype,
    pufs_algo_type_t algo,
    uint32_t size, uint32_t slot_index)
{
    uint32_t cfg = 0;
    cfg |= slot_index << DMA_KEY_CFG_0_KEY_IDX_BITS;
    cfg |= size << DMA_KEY_CFG_0_KEY_SIZE_BITS;
    cfg |= algo << DMA_KEY_CFG_0_KEY_DST_BITS;
    cfg |= keytype;
    desc->key_cfg = le2be(cfg);
}

void dma_sg_desc_write_crypto_config(pufs_dma_sg_internal_desc_st* desc, uint32_t cfg0, uint32_t cfg1)
{
    desc->cypt_cfg[0] = le2be(cfg0);
    desc->cypt_cfg[1] = le2be(cfg1);
}

void dma_sg_desc_append_to_list(pufs_dma_sg_internal_desc_st* desc)
{
    if (desc_list_head == NULL) {
        desc_list_head = desc;
        desc_list_tail = desc;
        return;
    }
    desc_list_tail->next = le2be(PHY_ADDR((uintptr_t)desc));
    desc_list_tail = desc;
}

void pufs_dma_sg_init(uintptr_t base_addr)
{
    sg_mem.base_addr = base_addr;
#ifndef BAREMETAL
    uint32_t size = DEFAULT_DESC_MEM_SIZE; // default 2K size
    sg_mem.virt_base_addr = (uintptr_t)get_mapped_addr(base_addr, &size);
#endif
    sg_mem.desc_bitmap = 0x0;
}

pufs_dma_sg_internal_desc_st* dma_sg_new_read_ctx_descriptor(uintptr_t crypto_ctx_addr)
{
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_sg_desc_opts_st opts = { .done_interrupt = false,
        .done_pause = false,
        .head = true,
        .tail = true,
        .offset = 0x0 };

    if ((desc = dma_sg_new_decriptor()) == NULL)
        return NULL;

    desc->length = le2be(144); // 16-byte iv + 64-byte swkey + 64-byte dgst
    desc->write_addr = le2be(PHY_ADDR(crypto_ctx_addr));

    dma_sg_desc_write_dsc_config(desc, NULL, &opts);
    dma_sg_desc_write_key_config(desc, 0, ALGO_TYPE_CYPT_REG_IO, 0, 0);
    dma_sg_desc_write_crypto_config(desc, 0x1, 0x0);

    return desc;
}

pufs_dma_sg_internal_desc_st* dma_sg_new_data_descriptor(void)
{
    return dma_sg_new_decriptor();
}

pufs_dma_sg_internal_desc_st* dma_sg_new_write_ctx_descriptor(uintptr_t crypto_ctx_addr)
{
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_sg_desc_opts_st opts = { .done_interrupt = false,
        .done_pause = false,
        .head = true,
        .tail = true,
        .offset = 0x0 };

    if ((desc = dma_sg_new_decriptor()) == NULL)
        return NULL;

    desc->length = le2be(80); // 16-byte iv + 64-byte dgst
    desc->read_addr = le2be(PHY_ADDR(crypto_ctx_addr));

    dma_sg_desc_write_dsc_config(desc, NULL, &opts);
    dma_sg_desc_write_key_config(desc, 0, ALGO_TYPE_CYPT_REG_IO, 0, 0);

    return desc;
}

void dma_sg_free_descriptor(pufs_dma_sg_internal_desc_st* desc)
{
    uint32_t index;
    index = (PHY_ADDR(((uintptr_t)desc)) - sg_mem.base_addr) / SGDMA_DESCRIPTOR_SIZE;
    sg_mem.desc_bitmap &= ~(1 << index);
}

void dma_sg_free_all_descriptor(void)
{
    while (desc_list_head != NULL) {
        dma_sg_free_descriptor(desc_list_head);
        if (desc_list_head->next == 0x0)
            desc_list_head = NULL;
        else
            desc_list_head = (pufs_dma_sg_internal_desc_st*)(uint64_t)(VIRT_ADDR(be2le(desc_list_head->next)));
    }
}

pufs_status_t pufs_dma_sg_start(void)
{
    uint32_t status;
    pufs_status_t check = SUCCESS;

    if (desc_list_head == NULL) {
        LOG_DEBUG("%s", "there is no descriptor");
        return SUCCESS;
    }

    dma_write_config_0(false, true, false);
    pufs_dma.regs->dsc_cfg_2 = 0x20;

    // mark the last descriptor with done_pause flag
    dma_sg_desc_write_dsc_pause(desc_list_tail);
    pufs_dma.regs->dsc_cfg_3 = (PHY_ADDR((uintptr_t)desc_list_head));

    dma_write_start();
    while (dma_check_busy_status(&status))
        ;

    if (status != 0) {
        LOG_ERROR("[ERROR] DMA status 0: 0x%08" PRIx32 "\n", status);
        check = E_ERROR;
    }

    dma_sg_free_all_descriptor();

    return check;
}

void pufs_dma_sg_release(void)
{
#ifndef BAREMETAL
    munmap((void*)sg_mem.virt_base_addr, DEFAULT_DESC_MEM_SIZE);
#endif
}