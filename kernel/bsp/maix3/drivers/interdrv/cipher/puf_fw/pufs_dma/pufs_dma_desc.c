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
#include <stdlib.h>
#include <string.h>
#include "pufs_internal.h"
#include "pufs_dma_internal.h"
#ifdef BAREMETAL
#include <cache.h>
#endif

/*****************************************************************************
 * Static variables
 ****************************************************************************/
struct dma_sg_mem sg_mem = { .base_addr = 0x0 };
static pufs_dma_sg_internal_desc_st *desc_list_head = NULL, *desc_list_tail = NULL;
/*
 * Set when the list being built is incomplete: a descriptor could not be
 * allocated while the list was non-empty, or NULL was appended (the
 * sg_append callers do not all check for NULL; appending it is a no-op).
 * pufs_dma_sg_start() then drops the list instead of submitting it. Cleared
 * when the list is freed. If a caller gives up after appending NULL to an
 * empty list, the next submission is dropped once.
 */
static bool desc_list_failed;
#ifdef BAREMETAL
/* The descriptor pool and all of its target addresses belong to the caller.
 * Submit a private snapshot so a timeout never extends their lifetime. */
#define SG_DESC_COUNT 32
static pufs_dma_sg_internal_desc_st submitted[SG_DESC_COUNT] __attribute__((aligned(64)));
struct sg_range { uintptr_t lo, hi; size_t offset; };
static struct sg_range ranges[SG_DESC_COUNT * 2];
static unsigned int nranges;
static uint8_t *payload;
static void *allocation;
static size_t payload_size;
static bool quarantined;

static void sg_dispose(void)
{
    if (payload_size) {
        rt_hw_cpu_dcache_invalidate(payload, payload_size);
        rvv_memset(payload, 0, payload_size);
        rt_hw_cpu_dcache_clean(payload, payload_size);
    }
    free(allocation);
    allocation = NULL;
    payload = NULL;
    payload_size = 0;
    quarantined = false;
}

static bool dma_sg_reclaim(void)
{
    if (quarantined) {
        if (dma_check_busy_status(NULL))
            return false;
        sg_dispose();
    }
    return true;
}

/* Merge overlaps, including context input/output of different lengths. */
static bool sg_add_range(uintptr_t lo, uint32_t length)
{
    if (!lo || !length)
        return true;
    if (length > UINT32_MAX - lo)
        return false;
    uintptr_t hi = lo + length;
    for (unsigned int i = 0; i < nranges; ) {
        if (hi <= ranges[i].lo || lo >= ranges[i].hi) {
            ++i;
            continue;
        }
        if (ranges[i].lo < lo) lo = ranges[i].lo;
        if (ranges[i].hi > hi) hi = ranges[i].hi;
        ranges[i] = ranges[--nranges];
        i = 0;
    }
    ranges[nranges++] = (struct sg_range){lo, hi, 0};
    return true;
}

static uintptr_t sg_address(uintptr_t addr)
{
    for (unsigned int i = 0; i < nranges; i++)
        if (addr >= ranges[i].lo && addr < ranges[i].hi)
            return (uintptr_t)payload + ranges[i].offset + (addr - ranges[i].lo);
    return 0;
}

static pufs_status_t sg_snapshot(void)
{
    unsigned int n = 0;
    if ((uintptr_t)submitted > UINT32_MAX - sizeof(submitted))
        return E_OVERFLOW;
    nranges = 0;
    for (pufs_dma_sg_internal_desc_st *d = desc_list_head; d;
         d = (void *)(uintptr_t)be2le(d->next)) {
        uint32_t len = be2le(d->length);
        if (n == SG_DESC_COUNT)
            return E_OVERFLOW;
        if (be2le(d->cfg) & ((1u << DMA_DSC_CFG_4_FIX_READ_BITS) |
                             (1u << DMA_DSC_CFG_4_FIX_WRITE_BITS)))
            return E_UNSUPPORT; /* Fixed-address peripheral I/O cannot be staged. */
        if (!sg_add_range(be2le(d->write_addr), len) || !sg_add_range(be2le(d->read_addr), len))
            return E_OVERFLOW;
        submitted[n++] = *d;
    }

    const size_t limit = 2 * CHUNK_MAXLEN;
    size_t total = 0;
    for (unsigned int i = 0; i < nranges; i++) {
        size_t len = ranges[i].hi - ranges[i].lo;
        if (len > limit)
            return E_OVERFLOW;
        size_t padded = (len + 63) & ~(size_t)63;
        if (padded > limit - total)
            return E_OVERFLOW;
        ranges[i].offset = total;
        total += padded;
    }
    if (total) {
        allocation = malloc(total + 63);
        if (!allocation)
            return E_UNAVAIL;
        payload = (uint8_t *)(((uintptr_t)allocation + 63) & ~(uintptr_t)63);
        payload_size = total;
        if ((uintptr_t)payload > UINT32_MAX - total) {
            sg_dispose();
            return E_OVERFLOW;
        }
        rvv_memset(payload, 0, total);
    }
    for (unsigned int i = 0; i < n; i++) {
        uintptr_t src = be2le(submitted[i].write_addr);
        uint32_t len = be2le(submitted[i].length);
        uintptr_t staged = len ? sg_address(src) : 0;
        if (staged)
            rvv_memcpy((void *)staged, (void *)src, len);
        submitted[i].write_addr = le2be(staged);
        submitted[i].read_addr = len ? le2be(sg_address(be2le(submitted[i].read_addr))) : 0;
        submitted[i].next = i + 1 < n ? le2be((uintptr_t)&submitted[i + 1]) : 0;
    }
    if (total)
        rt_hw_cpu_dcache_clean(payload, total);
    rt_hw_cpu_dcache_clean(submitted, n * sizeof(*submitted));
    return SUCCESS;
}

static void sg_copy_output(void)
{
    if (!payload_size)
        return;
    rt_hw_cpu_dcache_invalidate(payload, payload_size);
    for (pufs_dma_sg_internal_desc_st *d = desc_list_head; d;
         d = (void *)(uintptr_t)be2le(d->next)) {
        uintptr_t dst = be2le(d->read_addr);
        uint32_t len = be2le(d->length);
        if (dst && len)
            rvv_memcpy((void *)dst, (void *)sg_address(dst), len);
    }
}
#else
static bool dma_sg_reclaim(void) { return true; }
#endif

/*****************************************************************************
 * Internal Functions
 ****************************************************************************/
static pufs_dma_sg_internal_desc_st* dma_sg_new_decriptor(void)
{
    uint32_t index;
    uintptr_t addr;

    if (!dma_sg_reclaim()) {
        desc_list_failed |= desc_list_head != NULL;
        return NULL;
    }

    for (index = 0; index < 32 && (sg_mem.desc_bitmap & (1u << index)) != 0; index++)
        ;

    if (index == 32) {
        LOG_ERROR("%s", "failed to new a descriptor. all descriptors are used");
        desc_list_failed |= desc_list_head != NULL;
        return NULL;
    }

    addr = VIRT_ADDR(sg_mem.base_addr + (SGDMA_DESCRIPTOR_SIZE * index));
    rvv_memset((void*)addr, 0x0, SGDMA_DESCRIPTOR_SIZE);

    sg_mem.desc_bitmap |= (1u << index);

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
    desc->key_cfg = le2be(dma_key_cfg(keytype, algo, size, slot_index));
}

void dma_sg_desc_write_crypto_config(pufs_dma_sg_internal_desc_st* desc, uint32_t cfg0, uint32_t cfg1)
{
    desc->cypt_cfg[0] = le2be(cfg0);
    desc->cypt_cfg[1] = le2be(cfg1);
}

void dma_sg_desc_append_to_list(pufs_dma_sg_internal_desc_st* desc)
{
    if (desc == NULL) {
        desc_list_failed = true;
        return;
    }
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
    if (!dma_sg_reclaim())
        return;
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
    sg_mem.desc_bitmap &= ~(1u << index);
}

static void dma_sg_free_list(pufs_dma_sg_internal_desc_st *head)
{
    while (head != NULL) {
        pufs_dma_sg_internal_desc_st *next = head->next ?
            (pufs_dma_sg_internal_desc_st *)(uintptr_t)VIRT_ADDR(be2le(head->next)) : NULL;
        dma_sg_free_descriptor(head);
        head = next;
    }
}

void dma_sg_free_all_descriptor(void)
{
    dma_sg_free_list(desc_list_head);
    desc_list_head = desc_list_tail = NULL;
    desc_list_failed = false;
}

pufs_status_t pufs_dma_sg_start(void)
{
    if (!dma_sg_reclaim())
        return E_BUSY;

    if (desc_list_failed) {
        dma_sg_free_all_descriptor();
        return E_UNAVAIL;
    }

    if (desc_list_head == NULL) {
        LOG_DEBUG("%s", "there is no descriptor");
        return SUCCESS;
    }

#ifndef BAREMETAL
    /* No private bus mapping: a bounded submission could outlive the caller. */
    dma_sg_free_all_descriptor();
    return E_UNSUPPORT;
#else
    uint32_t slots = 0;
    bool timed_out;
    /* Do not touch the descriptor registers while an earlier (timed-out) run
     * still owns the engine. Nothing was submitted: drop the list, or the next
     * operation would append to it and submit stale descriptors. */
    if (dma_check_busy_status(NULL)) {
        dma_sg_free_all_descriptor();
        return E_BUSY;
    }
    /* Authorize only the keys in the submitted descriptor list. */
    for (pufs_dma_sg_internal_desc_st *d = desc_list_head; d;
         d = d->next ? (pufs_dma_sg_internal_desc_st *)(uintptr_t)VIRT_ADDR(be2le(d->next)) : NULL)
        slots |= dma_key_slots(be2le(d->key_cfg));

    dma_write_config_0(false, true, false);
    pufs_dma.regs->dsc_cfg_2 = 0x20;

    // mark the last descriptor with done_pause flag
    dma_sg_desc_write_dsc_pause(desc_list_tail);
    pufs_status_t check = sg_snapshot();
    if (check != SUCCESS) {
        dma_sg_free_all_descriptor();
        return check;
    }
    pufs_dma.regs->dsc_cfg_3 = (uintptr_t)submitted;
    check = dma_run_sg(slots, &timed_out);
    if (timed_out)
        quarantined = true;
    else {
        if (check == SUCCESS)
            sg_copy_output();
        sg_dispose();
    }
    dma_sg_free_all_descriptor();
    return check;
#endif
}

bool pufs_dma_sg_is_idle(void)
{
    return !dma_check_busy_status(NULL) && dma_sg_reclaim();
}

void pufs_dma_sg_release(void)
{
    if (!dma_sg_reclaim())
        return;
#ifndef BAREMETAL
    munmap((void*)sg_mem.virt_base_addr, DEFAULT_DESC_MEM_SIZE);
#endif
}
