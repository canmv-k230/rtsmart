/**
 * @file      pufs_dma.c
 * @brief     PUFsecurity DMA API implementation
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
#include <stdlib.h>
#include <string.h>
#include <cache.h>
#include "pufs_internal.h"
#include "pufs_dma_internal.h"
#include "pufs_rt_internal.h"

struct pufs_dma pufs_dma = { .regs = NULL,
    .read_addr = 0x0,
    .read_virt_addr = 0x0,
    .write_addr = 0x0,
    .write_virt_addr = 0x0,
    .buff_size = 0 };

#ifdef DMADIRECT
static uint8_t pufs_dma_write_bounce[CHUNK_MAXLEN] __attribute__((aligned(64)));
static uint8_t pufs_dma_read_bounce[CHUNK_MAXLEN] __attribute__((aligned(64)));
#endif

/*****************************************************************************
 * Macros
 ****************************************************************************/
#define SYS_CLASS_PATH "/sys/class/u-dma-buf/"

/*****************************************************************************
 * Static functions
 ****************************************************************************/
static void init_dma_buffer_v2(uintptr_t write_addr, uintptr_t read_addr, size_t buff_size)
{
#ifndef BAREMETAL
    char buff[30];
    int fd;
    void* mapped_addr;

    if (buff_size == 0) {
        if ((fd = open(SYS_CLASS_PATH "udmabuf0/size", O_RDONLY)) == -1)
            err(1, "Open " SYS_CLASS_PATH "udmabuf0/size failed");
        if (read(fd, buff, 30) == -1)
            err(1, "Read " SYS_CLASS_PATH "udmabuf0/size failed");
        close(fd);
        buff_size = strtol(buff, NULL, 10);
        if ((fd = open(SYS_CLASS_PATH "udmabuf1/size", O_RDONLY)) == -1)
            err(1, "Open " SYS_CLASS_PATH "udmabuf1/size failed");
        if (read(fd, buff, 30) == -1)
            err(1, "Read " SYS_CLASS_PATH "udmabuf1/size failed");
        close(fd);
        if (buff_size > strtoul(buff, NULL, 10))
            buff_size = strtoul(buff, NULL, 10);
        if (debug_api)
            printf("[DEBUG] DMA buffer udmabuf[01] has size 0x%08" PRIxPTR ".\n", buff_size);
    }

    if (read_addr == (size_t)-1) {
        if ((fd = open(SYS_CLASS_PATH "udmabuf0/phys_addr", O_RDONLY)) == -1)
            err(1, "Open " SYS_CLASS_PATH "udmabuf0/phys_addr failed");
        if (read(fd, buff, 30) == -1)
            err(1, "Read " SYS_CLASS_PATH "udmabuf0/phys_addr failed");
        read_addr = strtol(buff, NULL, 16);
        close(fd);
        if (debug_api)
            printf("[DEBUG] udmabuf0 at physical address 0x%08" PRIxPTR ".\n", read_addr);
    }

    if (write_addr == (size_t)-1) {
        if ((fd = open(SYS_CLASS_PATH "udmabuf1/phys_addr", O_RDONLY)) == -1)
            err(1, "Open " SYS_CLASS_PATH "udmabuf1/phys_addr failed");
        if (read(fd, buff, 30) == -1)
            err(1, "Read " SYS_CLASS_PATH "udmabuf1/phys_addr failed");
        write_addr = strtol(buff, NULL, 16);
        close(fd);
        if (debug_api)
            printf("[DEBUG] udmabuf1 at physical address 0x%08" PRIxPTR ".\n", write_addr);
    }

    if ((mapped_addr = mmap(NULL, buff_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, read_addr)) == (void*)-1)
        err(1, "mmap to read_addr 0x%08" PRIxPTR " failed", (uintptr_t)read_addr);
    pufs_dma.read_virt_addr = (uintptr_t)mapped_addr;

    if ((mapped_addr = mmap(NULL, buff_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, write_addr)) == (void*)-1)
        err(1, "mmap to write_addr 0x%08" PRIxPTR " failed", (uintptr_t)write_addr);
    pufs_dma.write_virt_addr = (uintptr_t)mapped_addr;
#else
#ifndef DMADIRECT
    pufs_dma.read_virt_addr = read_addr;
    pufs_dma.write_virt_addr = write_addr;
#endif /* DMADIRECT */
#endif /* BAREMETAL */
    pufs_dma.buff_size = buff_size;
    pufs_dma.read_addr = read_addr;
    pufs_dma.write_addr = write_addr;
}

// In SGDMA mode, we use write address as descriptor base address.
static pufs_status_t dma_gen_sg_desc_list(pufs_dma_sg_internal_desc_st* inter_sg_descs,
    uintptr_t inter_sg_descs_raw,
    pufs_dma_sg_desc_st* sg_descs,
    uint32_t descs_length,
    bool set_head,
    bool set_tail)
{
    uint32_t offset = 0, cfg;
    if (inter_sg_descs == NULL || sg_descs == NULL) {
        LOG_ERROR("%s", "descriptor addresses should not be NULL");
        return E_ERROR;
    }
    for (uint32_t index = 0; index < descs_length; index++) {
        inter_sg_descs[index].write_addr = le2be(sg_descs[index].write_addr);
        inter_sg_descs[index].read_addr = le2be(sg_descs[index].read_addr);
        inter_sg_descs[index].length = le2be(sg_descs[index].length);
        inter_sg_descs[index].next = (index + 1 < descs_length) ? le2be(inter_sg_descs_raw + ((index + 1) * sizeof(pufs_dma_sg_internal_desc_st))) : 0x0;

        cfg = 0;
        if (index == 0 && set_head)
            cfg |= 0x1 << DMA_DSC_CFG_4_HEAD_BITS;

        if (index == descs_length - 1) {
            if (set_tail)
                cfg |= 0x1 << DMA_DSC_CFG_4_TAIL_BITS;
            cfg |= 0x1 << DMA_DSC_CFG_4_DN_INTRPT_BITS;
            cfg |= 0x1 << DMA_DSC_CFG_4_DN_PAUSE_BITS;
        }
        cfg |= offset << DMA_DSC_CFG_4_OFFSET_BITS;

        inter_sg_descs[index].cfg = le2be(cfg);

        offset = (offset + sg_descs[index].length) % BC_BLOCK_SIZE;
    }
    return SUCCESS;
}

/*****************************************************************************
 * Internal functions
 ****************************************************************************/
int dma_write_rwcfg(const uint8_t* out, const uint8_t* in, uint32_t len)
{
    pufs_dma.regs->dsc_cfg_2 = len;

#ifndef DMADIRECT
    UNUSED(out);
    if (in != NULL) {
        if (len > pufs_dma.buff_size)
            errx(1, "The input exceeds DMA buffer size 0x%08" PRIxPTR ".\n", pufs_dma.buff_size);
        rvv_memcpy((void*)pufs_dma.write_virt_addr, in, len);
    }

    pufs_dma.regs->dsc_cfg_0 = pufs_dma.write_addr;
    pufs_dma.regs->dsc_cfg_1 = pufs_dma.read_addr;
#else
    pufs_dma.write_addr = (uintptr_t)in;
    pufs_dma.read_addr = (uintptr_t)out;
    pufs_dma.write_virt_addr = 0;
    pufs_dma.read_virt_addr = 0;
    pufs_dma.buff_size = len;
    if (len && in) {
        if (len > CHUNK_MAXLEN) {
            LOG_ERROR("DMA input length too large: %u", len);
            return -EINVAL;
        }
        if ((len & 0x3f) || ((uint64_t)in & 0x3f)) {
            pufs_dma.write_virt_addr = (uintptr_t)pufs_dma_write_bounce;
            rvv_memcpy((void*)pufs_dma.write_virt_addr, in, len);
        } else {
            pufs_dma.write_virt_addr = (uint64_t)in;
        }
        rt_hw_cpu_dcache_clean((void*)pufs_dma.write_virt_addr, len);
    }
    if (len && out) {
        if (len > CHUNK_MAXLEN) {
            LOG_ERROR("DMA output length too large: %u", len);
            return -EINVAL;
        }
        if ((len & 0x3f) || ((uint64_t)out & 0x3f)) {
            pufs_dma.read_virt_addr = (uintptr_t)pufs_dma_read_bounce;
        } else {
            pufs_dma.read_virt_addr = (uint64_t)out;
        }
        rt_hw_cpu_dcache_invalidate((void*)pufs_dma.read_virt_addr, len);
    }
    pufs_dma.regs->dsc_cfg_0 = (uintptr_t)pufs_dma.write_virt_addr;
    pufs_dma.regs->dsc_cfg_1 = (uintptr_t)pufs_dma.read_virt_addr;
#endif /* DMADIRECT */
    return 0;
}

int dma_wait_done(void)
{
    int ret = -ETIME;
    uint64_t stop = rt_tick_get() + 1000;

    while (stop > rt_tick_get()) {
        if (dma_check_busy_status(0) == 0) {
            ret = 0;
            break;
        }
    }

    if (pufs_dma.read_virt_addr) {
        rt_hw_cpu_dcache_invalidate((void*)pufs_dma.read_virt_addr, pufs_dma.buff_size);
        if (pufs_dma.read_virt_addr != pufs_dma.read_addr) {
            rvv_memcpy((void*)pufs_dma.read_addr, (void*)pufs_dma.read_virt_addr, pufs_dma.buff_size);
            rt_hw_cpu_dcache_clean((void*)pufs_dma.read_addr, pufs_dma.buff_size);
        }
    }

    return ret;
}

pufs_status_t dma_write_sgcfg(pufs_dma_sg_desc_st* descs, uint32_t descs_len, pufs_dma_sg_desc_opts_st* opts)
{
    bool head = true, tail = true;
    pufs_status_t check;
    pufs_dma_sg_internal_desc_st* inter_descs;
#ifndef DMADIRECT
    inter_descs = (pufs_dma_sg_internal_desc_st*)pufs_dma.write_virt_addr;
#else
    inter_descs = (pufs_dma_sg_internal_desc_st*)pufs_dma.write_addr;
#endif /* DMADIRECT */
    if (opts != NULL) {
        head = opts->head;
        tail = opts->tail;
    }
    if ((check = dma_gen_sg_desc_list(inter_descs, pufs_dma.write_addr, descs, descs_len, head, tail)) != SUCCESS)
        return check;
    pufs_dma.regs->dsc_cfg_3 = pufs_dma.write_addr;

    return SUCCESS;
}

#ifndef DMADIRECT
void clear_dma_read(uint32_t len)
{
    rvv_memset((void*)pufs_dma.read_virt_addr, 0x0, len);
}
#endif /* DMADIRECT */

pufs_status_t pufs_dump_rand_dma(uint8_t* rand, uint32_t len, bool entropy)
{
    uint32_t val32;
    // check feature
    if ((pufs_dma.regs->feature & DMA_FEATURE_RNG_MASK) == 0)
        return E_UNSUPPORT;

    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_config_0(true, false, false);
    dma_write_rwcfg(rand, NULL, len); // config DMA descriptor
    dma_write_key_config_0(0, ALGO_TYPE_NONE, 0, 0);

    dma_write_start();

    if (entropy)
        pufs_fre_cont_ctrl(true);
    else
        pufs_rng_cont_ctrl(true);

    while (dma_check_busy_status(&val32))
        ;

    if (entropy)
        pufs_fre_cont_ctrl(false);
    else
        pufs_rng_cont_ctrl(false);

    if (val32 != 0) {
        LOG_ERROR("DMA status 0: 0x%08" PRIx32 "\n", val32);
        return E_ERROR;
    }

    dma_read_output(rand, len);

    return SUCCESS;
}

#ifndef DMADIRECT
void dma_read_output(uint8_t* addr, uint32_t len)
{
    rvv_memcpy(addr, (void*)pufs_dma.read_virt_addr, len);
}
#endif /* DMADIRECT */

void dma_write_key_config_0(pufs_key_type_t keytype, pufs_algo_type_t algo, uint32_t size, uint32_t slot_index)
{
    uint32_t value = 0;
    value |= slot_index << DMA_KEY_CFG_0_KEY_IDX_BITS;
    value |= size << DMA_KEY_CFG_0_KEY_SIZE_BITS;
    value |= algo << DMA_KEY_CFG_0_KEY_DST_BITS;
    value |= keytype;
    pufs_dma.regs->key_cfg_0 = value;
}

void dma_write_config_0(bool rng_enable, bool sgdma_enable, bool no_cypt)
{
    uint32_t value = 0;
    if (rng_enable)
        value |= 0x1;
    if (sgdma_enable)
        value |= 0x1 << 1;
    if (no_cypt)
        value |= 0x1 << 2;
    pufs_dma.regs->cfg_0 = value;
}

void dma_write_cl_config_0(uint32_t value)
{
    pufs_dma.regs->cl_cfg_0 = value;
}

void dma_write_data_block_config(bool head, bool tail, bool dn_intrpt, bool dn_pause, uint32_t offset)
{
    uint32_t value = 0;
    if (head)
        value |= 0x1 << DMA_DSC_CFG_4_HEAD_BITS;
    if (tail)
        value |= 0x1 << DMA_DSC_CFG_4_TAIL_BITS;
    if (dn_intrpt)
        value |= 0x1 << DMA_DSC_CFG_4_DN_INTRPT_BITS;
    if (dn_pause)
        value |= 0x1 << DMA_DSC_CFG_4_DN_PAUSE_BITS;
    value |= offset << DMA_DSC_CFG_4_OFFSET_BITS;

    pufs_dma.regs->dsc_cfg_4 = value;
}

void dma_write_data_dsc_config(pufs_dma_sg_desc_opts_st* opts, pufs_dma_dsc_attr_st* attr, bool no_crypto)
{
    uint32_t value = 0;

    if (opts) {
        if (opts->head)
            value |= 0x1 << DMA_DSC_CFG_4_HEAD_BITS;
        if (opts->tail)
            value |= 0x1 << DMA_DSC_CFG_4_TAIL_BITS;
        if (opts->done_interrupt)
            value |= 0x1 << DMA_DSC_CFG_4_DN_INTRPT_BITS;
        if (opts->done_pause)
            value |= 0x1 << DMA_DSC_CFG_4_DN_PAUSE_BITS;
        if (opts->offset)
            value |= opts->offset << DMA_DSC_CFG_4_OFFSET_BITS;
    }
    if (attr) {
        if (no_crypto)
            value |= 0x1 << DMA_DSC_CFG_4_NO_CRYP_BITS;
        if (attr->fix_read_addr)
            value |= 0x1 << DMA_DSC_CFG_4_FIX_READ_BITS;
        if (attr->fix_write_addr)
            value |= 0x1 << DMA_DSC_CFG_4_FIX_WRITE_BITS;
        if (attr->read_protect)
            value |= 0x1 << DMA_DSC_CFG_4_READ_PROT_BITS;
        if (attr->write_protect)
            value |= 0x1 << DMA_DSC_CFG_4_WRITE_PROT_BITS;
    }
    pufs_dma.regs->dsc_cfg_4 = value;
}

void dma_write_start(void)
{
    pufs_dma.regs->start = 0x1;
}

bool dma_check_busy_status(uint32_t* status)
{
    uint32_t stat = pufs_dma.regs->status_0;
    bool busy = (stat & DMA_STATUS_0_BUSY_MASK) != 0;

    if (status != NULL)
        *status = stat;

    return busy;
}

pufs_status_t pufs_dma_read_write(uint8_t* out, uint32_t outlen,
    const uint8_t* data, const uint32_t data_length,
    pufs_dma_dsc_attr_st* attr)
{
    uint32_t status;
    pufs_dma_sg_desc_opts_st opts = { .head = false, .tail = false, .done_interrupt = true, .done_pause = true, .offset = 0x0 };

#ifdef BAREMETAL
    UNUSED(outlen);
#endif

    if (dma_check_busy_status(NULL))
        return E_BUSY;

    dma_write_config_0(false, false, false);
    dma_write_rwcfg(out, data, data_length);
    dma_write_key_config_0(0, ALGO_TYPE_NONE, 0, 0);
    dma_write_data_dsc_config(&opts, attr, true);

    dma_write_start();

    while (dma_check_busy_status(&status))
        ;

    if (status != 0) {
        LOG_ERROR("DMA status 0: 0x%08" PRIx32 "\n", status);
        return E_ERROR;
    }
    dma_read_output(out, outlen);

    return SUCCESS;
}

pufs_status_t pufs_dma_read_write_sg(pufs_dma_sg_desc_st* descs)
{
    pufs_dma_sg_internal_desc_st* desc;
    pufs_dma_sg_desc_opts_st opts = { .head = false, .tail = false, .done_interrupt = true, .done_pause = true, .offset = 0x0, .no_crypto = true };

    desc = dma_sg_new_data_descriptor();
    dma_sg_desc_write_addr(desc, descs->write_addr, descs->read_addr, descs->length);
    dma_sg_desc_write_dsc_config(desc, &descs->attr, &opts);
    dma_sg_desc_write_key_config(desc, 0, ALGO_TYPE_NONE, 0, 0);
    dma_sg_desc_write_crypto_config(desc, 0x0, 0x0);
    dma_sg_desc_append_to_list(desc);

    return pufs_dma_sg_start();
}

/*****************************************************************************
 * API functions
 ****************************************************************************/
void _pufs_dma_module_init(uintptr_t dma_offset, pufs_dma_attr_st* dma_attr)
{
    uintptr_t write_addr = -1, read_addr = -1;
    size_t buff_size = 0;
    pufs_dma.regs = (struct pufs_dma_regs*)(pufs_context.base_addr + dma_offset);
    version_check(DMA_VERSION, pufs_dma.regs->version);

    if (dma_attr != NULL) {
        write_addr = dma_attr->write_addr;
        read_addr = dma_attr->read_addr;
        buff_size = dma_attr->buff_size;
    }
    init_dma_buffer_v2(write_addr, read_addr, buff_size);
}

void pufs_dma_prepare_sg_descs_offset(pufs_dma_sg_desc_st* descs,
    uint32_t* descs_len,
    const uint32_t max_descs_len,
    const char* msg,
    const uint32_t msglen,
    uint32_t block_size,
    bool set_read,
    uint32_t start_offset)
{
    pufs_dma_dsc_attr_st attr = { .fix_read_addr = 0x0, .fix_write_addr = 0x0, .read_protect = 0x0, .write_protect = 0x0 };
    uintptr_t addr, start_w = pufs_dma.read_addr + start_offset, start_r = pufs_dma.read_addr + 512;
    uint32_t len = msglen;
    *descs_len = 0;
    if (block_size == 0)
        block_size = 2 * BC_BLOCK_SIZE;
#ifndef DMADIRECT
    addr = pufs_dma.read_virt_addr;
#else
    addr = pufs_dma.read_addr;
#endif /* DMADIRECT */
    rvv_memcpy((void*)(addr + start_offset), msg, msglen);

    if (len == 0) {
        descs[0].length = 0;
        descs[0].write_addr = start_w;
        descs[0].read_addr = (set_read == true) ? start_r : 0x0;
        descs[0].attr = attr;

        *descs_len = 1;
        return;
    }

    for (uint32_t index = 0; index < max_descs_len && len > 0; index++) {
        descs[index].attr = attr;
        descs[index].write_addr = start_w;
        descs[index].read_addr = (set_read == true) ? start_r : 0x0;

        if (len >= block_size)
            descs[index].length = block_size;
        else
            descs[index].length = len % block_size;

        len = (len > block_size) ? len - block_size : 0;
        start_w += block_size;
        start_r += block_size;
        *descs_len += 1;
    }
}

void pufs_dma_set_dsc_attr(pufs_dma_dsc_attr_st* attr)
{
    uint32_t value = 0;
    if (attr->fix_read_addr)
        value |= 1 << DMA_DSC_CFG_4_FIX_READ_BITS;
    if (attr->fix_write_addr)
        value |= 1 << DMA_DSC_CFG_4_FIX_WRITE_BITS;
    value |= attr->read_protect << DMA_DSC_CFG_4_READ_PROT_BITS;
    value |= attr->write_protect << DMA_DSC_CFG_4_WRITE_PROT_BITS;

    pufs_dma.regs->dsc_cfg_4 |= value;
}

// helper function for testing.
void pufs_dma_prepare_sg_descs(pufs_dma_sg_desc_st* descs,
    uint32_t* descs_len,
    const uint32_t max_descs_len,
    const char* msg,
    const uint32_t msglen,
    uint32_t block_size,
    bool set_read)
{
    pufs_dma_prepare_sg_descs_offset(descs, descs_len, max_descs_len, msg, msglen, block_size, set_read, 0);
}

void pufs_dma_read_output(uint8_t* addr, uint32_t len)
{
#ifndef DMADIRECT
    rvv_memcpy(addr, (void*)(pufs_dma.read_virt_addr + 512), len);
#else
    rvv_memcpy(addr, (void*)(pufs_dma.read_addr + 512), len);
#endif /* DMADIRECT */
}

void pufs_dma_module_release(void)
{
#ifndef BAREMETAL
    munmap((void*)pufs_dma.read_virt_addr, pufs_dma.buff_size);
    munmap((void*)pufs_dma.write_virt_addr, pufs_dma.buff_size);
#endif /* BAREMETAL */
}
