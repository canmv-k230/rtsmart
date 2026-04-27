#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <rtconfig.h>

#include "pufs_internal.h"
#include "pufs_rt_internal.h"

#include "sbi.h"

struct pufs_rt_cde_regs *rt_cde_regs = NULL;
uintptr_t rt_cde_regs_phys = 0;

static pufs_status_t pufs_rt_cde_pmp_set(unsigned long perm)
{
    struct sbi_ret ret = sbi_pmp_set(rt_cde_regs_phys, sizeof(*rt_cde_regs), perm);

    if (ret.error == SBI_SUCCESS)
        return SUCCESS;

    LOG_ERROR("SBI CDE PMP set failed: addr=0x%lx len=0x%lx perm=0x%lx error=%ld value=%ld",
              (unsigned long)rt_cde_regs_phys, (unsigned long)sizeof(*rt_cde_regs), perm,
              ret.error, ret.value);

    return (ret.error == SBI_ERR_DENIED) ? E_DENY : E_ERROR;
}

static pufs_status_t pufs_rt_pif_pmp_set(unsigned long perm)
{
    struct sbi_ret ret = sbi_pmp_set(rt_regs_phys, 0x1000, perm);

    if (ret.error == SBI_SUCCESS)
        return SUCCESS;

    LOG_ERROR("SBI PIF PMP set failed: addr=0x%lx len=0x%lx perm=0x%lx error=%ld value=%ld",
              (unsigned long)rt_regs_phys, (unsigned long)0x1000, perm,
              ret.error, ret.value);

    return (ret.error == SBI_ERR_DENIED) ? E_DENY : E_ERROR;
}

void pufs_rt_cde_init(uint32_t rt_cde_offset)
{
    rt_cde_regs = (struct pufs_rt_cde_regs *)(pufs_context.base_addr + rt_cde_offset);
    rt_cde_regs_phys = pufs_context.phys_base_addr + rt_cde_offset;
}

static int rt_cde_select_index(uint32_t idx)
{
    uint32_t group = idx / RWLOCK_GROUP_OTP;

    if (group >= PIF_CDE_RWLCK_MAX_GROUP)
        return -1;

    return PIF_CDE_RWLCK_START_INDEX + group; 
}

static uint32_t cde_pack_program_word(const uint8_t *inbuf, uint32_t offset,
    uint32_t len)
{
    uint32_t word = 0;
    uint32_t chunk_len = ((offset + 4) <= len) ? 4 : (len - offset);

    memcpy(&word, inbuf + offset, chunk_len);
    return word;
}

static void cde_word_to_bytes(uint8_t out[4], uint32_t word)
{
    out[0] = word & 0xff;
    out[1] = (word >> 8) & 0xff;
    out[2] = (word >> 16) & 0xff;
    out[3] = (word >> 24) & 0xff;
}

static void cde_fill_program_bytes(uint8_t out[4], const uint8_t *inbuf,
    uint32_t offset, uint32_t len)
{
    uint32_t chunk_len = ((offset + 4) <= len) ? 4 : (len - offset);

    memset(out, 0, sizeof(uint8_t) * 4);
    memcpy(out, inbuf + offset, chunk_len);
}

/**
 * pufs_read_otp()
 */
pufs_status_t pufs_read_cde(uint8_t* outbuf, uint32_t len, uint32_t addr)
{
    uint32_t word, start_index, wlen;

    wlen = len / WORD_SIZE;
    start_index = addr / WORD_SIZE;

    if (wlen > 0) {
        uint32_t* out32 = (uint32_t*)outbuf;
        for (size_t i = 0; i < wlen; ++i)
            out32[i] = rt_cde_regs->otp[start_index + i];
    }

    if (len % WORD_SIZE != 0) {
        outbuf += wlen * WORD_SIZE;
        word = rt_cde_regs->otp[start_index + wlen];
        rt_memcpy(outbuf, &word, len % WORD_SIZE);
    }

    return SUCCESS;
}

/**
 * pufs_program_cde()
 */
pufs_status_t pufs_program_cde(const uint8_t* inbuf, uint32_t len, uint32_t addr)
{
    pufs_status_t check;

    // check lock state for each segment in range
    uint32_t seg_s = addr / PUFS_CDE_SEGMENT;
    uint32_t seg_e = (addr + len + PUFS_CDE_SEGMENT - 1) / PUFS_CDE_SEGMENT;
    for (uint32_t seg = seg_s; seg < seg_e; seg++) {
        pufs_otp_lock_t lck = rt_cde_read_lock(seg * PUFS_CDE_SEGMENT);
        if (lck != RW) {
            LOG_WARN("CDE WRITE DENIED: offset=0x%03x segment locked",
                     seg * PUFS_CDE_SEGMENT);
            return E_DENY;
        }
    }

    // check for 1->0 bit violations (CDE OTP only goes 0->1)
    for (uint32_t index = 0; index < len; index += 4) {
        uint32_t cur = rt_cde_regs->otp[(addr + index) / 4];
        uint32_t new_val = cde_pack_program_word(inbuf, index, len);
        if (cur & ~new_val) {
            LOG_WARN("CDE WRITE DENIED: offset=0x%03x bit 1->0"
                     " (cur=0x%08" PRIx32 " new=0x%08" PRIx32 ")",
                     addr + index, cur, new_val);
            return E_DENY;
        }
    }

#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("CDE WRITE DRY-RUN: offset=0x%03x len=%" PRIu32
             " (enable RT_PUFS_OTP_WRITE_ENABLE to commit)", addr, len);
    for (uint32_t index = 0; index < len; index += 4) {
        uint32_t cur_val = rt_cde_regs->otp[(addr + index) / 4];
        uint8_t cur_bytes[4];
        uint8_t write_bytes[4];

        cde_word_to_bytes(cur_bytes, cur_val);
        cde_fill_program_bytes(write_bytes, inbuf, index, len);

        LOG_WARN("CDE WRITE DRY-RUN WORD: offset=0x%03x cur=[%02x %02x %02x %02x]"
                 " write=[%02x %02x %02x %02x]",
                 addr + index,
                 cur_bytes[0], cur_bytes[1], cur_bytes[2], cur_bytes[3],
                 write_bytes[0], write_bytes[1], write_bytes[2], write_bytes[3]);
    }
    return SUCCESS;
#else
    LOG_WARN("CDE WRITE: offset=0x%03x len=%" PRIu32, addr, len);

    if ((check = pufs_rt_cde_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    for (uint32_t index = 0; index < len; index += 4) {
        uint32_t new_val = cde_pack_program_word(inbuf, index, len);

        if (new_val == 0) continue;
        if (rt_cde_regs->otp[(addr + index) / 4] == new_val) continue;
        rt_cde_regs->otp[(addr + index) / 4] = new_val;
    }

    if ((check = pufs_rt_cde_pmp_set(SBI_PMP_PERM_R)) != SUCCESS)
        return check;

    return SUCCESS;
#endif
}

pufs_status_t rt_cde_write_lock(uint32_t offset, uint32_t length, pufs_otp_lock_t lock)
{
    uint32_t lock_val = 0, end = 0, start = 0, val32 = 0, rwlock_index, shift, mask = 0;
    pufs_status_t check;

    switch (lock)
    {
    case RO:
        lock_val = PUFRT_VALUE4(0xC);
        break;
    case RW:
        lock_val = PUFRT_VALUE4(0xF);
        break;
    default:
        return E_INVALID;
    }

#ifndef RT_PUFS_OTP_WRITE_ENABLE
    LOG_WARN("CDE LOCK DRY-RUN: offset=0x%03x len=%" PRIu32 " lock=%s"
             " (enable RT_PUFS_OTP_WRITE_ENABLE to commit)",
             offset, length, lock == RO ? "RO" : "RW");
    return SUCCESS;
#else
    LOG_WARN("CDE LOCK: offset=0x%03x len=%" PRIu32 " lock=%s",
             offset, length, lock == RO ? "RO" : "RW");

    if ((check = pufs_rt_pif_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    end = (length + (PUFS_CDE_SEGMENT - 1)) / PUFS_CDE_SEGMENT;
    start = offset / PUFS_CDE_SEGMENT;

    for (uint32_t i = 0; i < end; i++)
    {
        int idx = start + i;
        rwlock_index = rt_cde_select_index(idx);

        shift = (idx % RWLOCK_GROUP_OTP) * 4;
        val32 |= lock_val << shift;
        mask |= 0xF << shift;

        if (shift == 28 || i == end - 1)
        {
            val32 |= (rt_regs->pif[rwlock_index] & (~mask));
            rt_regs->pif[rwlock_index] = val32;

            val32 = 0;
            mask = 0;
        }
    }

    if ((check = pufs_rt_pif_pmp_set(SBI_PMP_PERM_R)) != SUCCESS)
        return check;

    return SUCCESS;
#endif
}

pufs_otp_lock_t rt_cde_read_lock(uint32_t offset)
{
    int idx;
    uint32_t group_index, lck;

    idx = offset / PUFS_CDE_SEGMENT;
    group_index = idx % RWLOCK_GROUP_OTP;

    if ((idx = rt_cde_select_index(idx)) == -1)
        return N_OTP_LOCK_T;

    lck = (rt_regs->pif[idx] >> (group_index * 4)) & 0xF;

    switch (lck)
    {
    case PUFRT_VALUE4(0x0):
        return RO;
    case PUFRT_VALUE4(0xC):
        return RO;
    case PUFRT_VALUE4(0xF):
        return RW;
    default:
        return N_OTP_LOCK_T;
    }
    return N_OTP_LOCK_T;
}

// mask 1K bits(128 bytes) segment starting from offset input
pufs_status_t rt_cde_write_mask(uint32_t offset)
{
    uint32_t index, group, reg, group_index;
    pufs_status_t check;
    index = offset / PUFS_CDE_SEGMENT;
    group = index / 16; // 2 bit for each code segment mask of 32 bit register
    group_index = index % 16;

    if (group > 1)
        return E_INVALID;

    if ((check = pufs_rt_pif_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W)) != SUCCESS)
        return check;

    reg = rt_regs->ptm[group];
    reg |= 0x3 << (group_index * 2);
    rt_regs->ptm[group] = reg;

    if ((check = pufs_rt_pif_pmp_set(SBI_PMP_PERM_R)) != SUCCESS)
        return check;

    return SUCCESS;
}
