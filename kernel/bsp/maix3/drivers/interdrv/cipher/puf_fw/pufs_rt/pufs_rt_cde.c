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

void pufs_rt_cde_init(uint32_t rt_cde_offset)
{
    rt_cde_regs = (struct pufs_rt_cde_regs *)(pufs_context.base_addr + rt_cde_offset);
    rt_cde_regs_phys = pufs_context.phys_base_addr + rt_cde_offset;
}

pufs_status_t pufs_read_cde(uint8_t* outbuf, uint32_t len, uint32_t addr)
{
    pufs_status_t check;

    if (len && !outbuf)
        return E_INVALID;
    if ((check = rt_otp_guard(OTP_LEN + addr, len, true)) != SUCCESS)
        return check;

    for (uint32_t off = 0; off < len; off += WORD_SIZE) {
        uint32_t word = rt_cde_regs->otp[(addr + off) / WORD_SIZE];

        memcpy(outbuf + off, &word, otp_chunk_len(len - off));
    }

    return rt_otp_guard(OTP_LEN + addr, len, false);
}

/**
 * pufs_program_cde()
 */
pufs_status_t pufs_program_cde(const uint8_t* inbuf, uint32_t len, uint32_t addr)
{
    const struct rt_program_area area = {
        .name = "CDE",
        .otp = rt_cde_regs->otp,
        .size = sizeof(struct pufs_rt_cde_regs),
        .flat_base = OTP_LEN,
        .big_endian = false,
        .lock = rt_cde_read_lock,
        .pmp_set = pufs_rt_cde_pmp_set,
    };

    return rt_program_area(&area, inbuf, len, addr);
}

/* Decode one CDE lock field. CDE has no NA state; that encoding reads as RO. */
static pufs_otp_lock_t rt_cde_lock_decode(uint32_t field)
{
    switch (field) {
    case PUFRT_VALUE4(0x0): return RO;
    case PUFRT_VALUE4(0xC): return RO;
    case PUFRT_VALUE4(0xF): return RW;
    default: return N_OTP_LOCK_T;
    }
}

static const struct rt_lock_area rt_cde_lock_area = {
    .name = "CDE",
    .pif_base = PIF_CDE_RWLCK_START_INDEX,
    .fields = sizeof(struct pufs_rt_cde_regs) / PUFS_CDE_SEGMENT,
    .field_bytes = PUFS_CDE_SEGMENT,
    .flat_base = OTP_LEN,
    .decode = rt_cde_lock_decode,
};

/* Lock every 128-byte CDE segment that [offset, offset + length) touches. */
pufs_status_t rt_cde_write_lock(uint32_t offset, uint32_t length, pufs_otp_lock_t lock)
{
    if (lock != RO && lock != RW)
        return E_INVALID;
    if (length == 0)
        return SUCCESS;
    if (offset >= sizeof(struct pufs_rt_cde_regs) ||
        length > sizeof(struct pufs_rt_cde_regs) - offset)
        return E_OVERFLOW;

    return rt_set_lock_fields(&rt_cde_lock_area, offset / PUFS_CDE_SEGMENT,
                              (offset + length - 1) / PUFS_CDE_SEGMENT, lock);
}

pufs_otp_lock_t rt_cde_read_lock(uint32_t offset)
{
    uint32_t seg = offset / PUFS_CDE_SEGMENT;

    if (offset >= sizeof(struct pufs_rt_cde_regs))
        return N_OTP_LOCK_T;

    return rt_cde_lock_decode((rt_regs->pif[PIF_CDE_RWLCK_START_INDEX +
                               seg / RWLOCK_GROUP_OTP] >>
                               ((seg % RWLOCK_GROUP_OTP) * 4)) & 0xF);
}
