// SPDX-License-Identifier: ISC
/*
 * Chip recognition and core control, derived from Linux 6.6 brcmfmac/chip.c.
 * Copyright (c) 2014 Broadcom Corporation.
 */
#include "brcmfmac.h"
#include "tick.h"

#define DBG_TAG "brcmf.chip"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define BRCMF_ENUM_BASE                 0x18000000U
#define BRCMF_CC_EROMPTR                0x00fcU
#define BRCMF_CC_CAPABILITIES           0x0004U
#define BRCMF_CC_CAPABILITIES_EXT       0x00acu
#define BRCMF_CC_PMU_CAPABILITIES       0x0604U
#define BRCMF_CC_CAP_PMU                0x10000000U
#define BRCMF_CC_CAP_EXT_AOB_PRESENT    0x00000040U

#define BRCMF_CID_ID_MASK               0x0000ffffU
#define BRCMF_CID_REV_MASK              0x000f0000U
#define BRCMF_CID_REV_SHIFT             16U
#define BRCMF_CID_TYPE_MASK             0xf0000000U
#define BRCMF_CID_TYPE_SHIFT            28U
#define BRCMF_SOC_AI                    1U

#define DMP_DESC_TYPE_MASK              0x0000000fU
#define DMP_DESC_VALID                  0x00000001U
#define DMP_DESC_COMPONENT              0x00000001U
#define DMP_DESC_MASTER_PORT            0x00000003U
#define DMP_DESC_ADDRESS                0x00000005U
#define DMP_DESC_ADDRSIZE_GT32          0x00000008U
#define DMP_DESC_EOT                    0x0000000fU
#define DMP_COMP_PARTNUM                0x000fff00U
#define DMP_COMP_PARTNUM_SHIFT          8U
#define DMP_COMP_REVISION               0xff000000U
#define DMP_COMP_REVISION_SHIFT         24U
#define DMP_COMP_NUM_SWRAP              0x00f80000U
#define DMP_COMP_NUM_SWRAP_SHIFT        19U
#define DMP_COMP_NUM_MWRAP              0x0007c000U
#define DMP_COMP_NUM_MWRAP_SHIFT        14U
#define DMP_SLAVE_ADDR_BASE             0xfffff000U
#define DMP_SLAVE_TYPE                  0x000000c0U
#define DMP_SLAVE_TYPE_SHIFT            6U
#define DMP_SLAVE_TYPE_SLAVE            0U
#define DMP_SLAVE_TYPE_SWRAP            2U
#define DMP_SLAVE_TYPE_MWRAP            3U
#define DMP_SLAVE_SIZE_TYPE             0x00000030U
#define DMP_SLAVE_SIZE_TYPE_SHIFT       4U
#define DMP_SLAVE_SIZE_4K               0U
#define DMP_SLAVE_SIZE_8K               1U
#define DMP_SLAVE_SIZE_DESC             3U

#define BCMA_IOCTL                      0x0408U
#define BCMA_IOCTL_CLK                  0x0001U
#define BCMA_IOCTL_FGC                  0x0002U
#define BCMA_IOCTL_CPUHALT              0x0020U
#define BCMA_RESET_CTL                  0x0800U
#define BCMA_RESET_CTL_RESET            0x0001U
#define D11_IOCTL_PHYCLOCKEN            0x0004U
#define D11_IOCTL_PHYRESET              0x0008U

#define SOCRAM_COREINFO                 0x0000U
#define SOCRAM_BANKIDX                  0x0010U
#define SOCRAM_BANKINFO                 0x0040U
#define SOCRAM_BANKPDA                  0x0044U
#define SOCRAM_BANKINFO_RETNTRAM        0x00010000U
#define SOCRAM_BANKINFO_SIZE_MASK       0x0000007fU
#define SOCRAM_BANKINFO_SIZE_BASE       8192U
#define SOCRAM_SRNB_MASK                0x000000f0U
#define SOCRAM_SRNB_MASK_EXT            0x00000100U
#define SOCRAM_SRNB_SHIFT               4U
#define SOCRAM_SRBSZ_MASK               0x0000000fU
#define SOCRAM_LSS_MASK                 0x00f00000U
#define SOCRAM_LSS_SHIFT                20U
#define SOCRAM_BANK_SIZE_BASE_SHIFT     14U

#define ARMCR4_CAP                      0x0004U
#define ARMCR4_BANKIDX                  0x0040U
#define ARMCR4_BANKINFO                 0x0044U
#define ARMCR4_TCBANB_MASK              0x0000000fU
#define ARMCR4_TCBBNB_MASK              0x000000f0U
#define ARMCR4_TCBBNB_SHIFT             4U
#define ARMCR4_BANK_SIZE_MASK           0x0000007fU
#define ARMCR4_BANK_SIZE_MULT           8192U
#define ARMCR4_BANK_SIZE_1K             0x00000200U

static rt_uint32_t brcmf_chip_read(struct brcmf_context *context,
                                   rt_uint32_t address, rt_err_t *result)
{
    return brcmf_sdio_read32(context, address, result);
}

static rt_err_t brcmf_chip_write(struct brcmf_context *context,
                                 rt_uint32_t address, rt_uint32_t value)
{
    return brcmf_sdio_write32(context, address, value);
}

struct brcmf_core *brcmf_chip_get_core(struct brcmf_chip *chip,
                                       rt_uint16_t id)
{
    rt_size_t index;

    for (index = 0; chip && index < chip->core_count; index++)
    {
        if (chip->cores[index].id == id)
        {
            return &chip->cores[index];
        }
    }
    return RT_NULL;
}

static rt_err_t brcmf_chip_add_core(struct brcmf_chip *chip, rt_uint16_t id,
                                    rt_uint8_t revision, rt_uint32_t base,
                                    rt_uint32_t wrapbase)
{
    struct brcmf_core *core;

    if (chip->core_count >= BRCMF_MAX_CORES)
    {
        LOG_E("core table full while adding 0x%03x", id);
        return -RT_EFULL;
    }
    core = &chip->cores[chip->core_count++];
    core->id = id;
    core->revision = revision;
    core->base = base;
    core->wrapbase = wrapbase;
    return RT_EOK;
}

static rt_uint32_t brcmf_chip_dmp_get_desc(struct brcmf_context *context,
                                           rt_uint32_t *address,
                                           rt_uint8_t *type,
                                           rt_err_t *result)
{
    rt_uint32_t value = brcmf_chip_read(context, *address, result);

    *address += 4U;
    if (type)
    {
        *type = value & DMP_DESC_TYPE_MASK;
        if ((*type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
        {
            *type = DMP_DESC_ADDRESS;
        }
    }
    return value;
}

static rt_err_t brcmf_chip_dmp_get_regaddr(struct brcmf_context *context,
                                            rt_uint32_t *erom,
                                            rt_uint32_t *base,
                                            rt_uint32_t *wrap)
{
    rt_uint8_t descriptor;
    rt_uint8_t wrap_type;
    rt_uint32_t value;
    rt_err_t result = RT_EOK;

    *base = 0;
    *wrap = 0;
    value = brcmf_chip_dmp_get_desc(context, erom, &descriptor, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    if (descriptor == DMP_DESC_MASTER_PORT)
    {
        wrap_type = DMP_SLAVE_TYPE_MWRAP;
    }
    else if (descriptor == DMP_DESC_ADDRESS)
    {
        *erom -= 4U;
        wrap_type = DMP_SLAVE_TYPE_SWRAP;
    }
    else
    {
        *erom -= 4U;
        return -RT_EINVAL;
    }

    while (!*base || !*wrap)
    {
        rt_uint32_t size_descriptor;
        rt_uint8_t size_type;
        rt_uint8_t slave_type;

        do
        {
            value = brcmf_chip_dmp_get_desc(context, erom, &descriptor,
                                             &result);
            if (result != RT_EOK)
            {
                return result;
            }
            if (descriptor == DMP_DESC_EOT)
            {
                /* Let the outer EROM scanner consume the end marker. */
                *erom -= 4U;
                return -RT_EIO;
            }
        } while (descriptor != DMP_DESC_ADDRESS &&
                 descriptor != DMP_DESC_COMPONENT);

        if (descriptor == DMP_DESC_COMPONENT)
        {
            *erom -= 4U;
            return RT_EOK;
        }
        if (value & DMP_DESC_ADDRSIZE_GT32)
        {
            (void)brcmf_chip_dmp_get_desc(context, erom, RT_NULL, &result);
        }
        size_type = (value & DMP_SLAVE_SIZE_TYPE) >>
                    DMP_SLAVE_SIZE_TYPE_SHIFT;
        if (size_type == DMP_SLAVE_SIZE_DESC)
        {
            size_descriptor = brcmf_chip_dmp_get_desc(
                context, erom, RT_NULL, &result);
            if (size_descriptor & DMP_DESC_ADDRSIZE_GT32)
            {
                (void)brcmf_chip_dmp_get_desc(context, erom, RT_NULL,
                                               &result);
            }
        }
        if (result != RT_EOK ||
            (size_type != DMP_SLAVE_SIZE_4K &&
             size_type != DMP_SLAVE_SIZE_8K))
        {
            if (result != RT_EOK)
            {
                return result;
            }
            continue;
        }
        slave_type = (value & DMP_SLAVE_TYPE) >> DMP_SLAVE_TYPE_SHIFT;
        if (!*base && slave_type == DMP_SLAVE_TYPE_SLAVE)
        {
            *base = value & DMP_SLAVE_ADDR_BASE;
        }
        if (!*wrap && slave_type == wrap_type)
        {
            *wrap = value & DMP_SLAVE_ADDR_BASE;
        }
    }
    return RT_EOK;
}

static rt_err_t brcmf_chip_scan_erom(struct brcmf_context *context)
{
    struct brcmf_chip *chip = &context->chip;
    rt_uint32_t erom;
    rt_uint8_t type = 0;
    rt_err_t result = RT_EOK;

    erom = brcmf_chip_read(context, chip->enum_base + BRCMF_CC_EROMPTR,
                           &result);
    if (result != RT_EOK || erom == 0xffffffffU)
    {
        return -RT_EIO;
    }
    while (type != DMP_DESC_EOT)
    {
        rt_uint32_t value;
        rt_uint32_t base;
        rt_uint32_t wrap;
        rt_uint16_t id;
        rt_uint8_t revision;
        rt_uint8_t master_wrappers;
        rt_uint8_t slave_wrappers;

        value = brcmf_chip_dmp_get_desc(context, &erom, &type, &result);
        if (result != RT_EOK)
        {
            return result;
        }
        if (!(value & DMP_DESC_VALID) || type != DMP_DESC_COMPONENT)
        {
            continue;
        }
        id = (value & DMP_COMP_PARTNUM) >> DMP_COMP_PARTNUM_SHIFT;
        value = brcmf_chip_dmp_get_desc(context, &erom, &type, &result);
        if (result != RT_EOK ||
            (value & DMP_DESC_TYPE_MASK) != DMP_DESC_COMPONENT)
        {
            return -RT_EIO;
        }
        master_wrappers = (value & DMP_COMP_NUM_MWRAP) >>
                          DMP_COMP_NUM_MWRAP_SHIFT;
        slave_wrappers = (value & DMP_COMP_NUM_SWRAP) >>
                         DMP_COMP_NUM_SWRAP_SHIFT;
        revision = (value & DMP_COMP_REVISION) >> DMP_COMP_REVISION_SHIFT;
        if (!master_wrappers && !slave_wrappers &&
            id != BRCMF_CORE_PMU && id != BRCMF_CORE_GCI)
        {
            continue;
        }
        result = brcmf_chip_dmp_get_regaddr(context, &erom, &base, &wrap);
        if (result != RT_EOK)
        {
            continue;
        }
        result = brcmf_chip_add_core(chip, id, revision, base, wrap);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    return RT_EOK;
}

static rt_bool_t brcmf_chip_core_is_up(struct brcmf_context *context,
                                       struct brcmf_core *core)
{
    rt_err_t result;
    rt_uint32_t io = brcmf_chip_read(context, core->wrapbase + BCMA_IOCTL,
                                     &result);
    rt_uint32_t reset;

    if (result != RT_EOK)
    {
        return RT_FALSE;
    }
    reset = brcmf_chip_read(context, core->wrapbase + BCMA_RESET_CTL,
                            &result);
    return result == RT_EOK &&
           (io & (BCMA_IOCTL_FGC | BCMA_IOCTL_CLK)) == BCMA_IOCTL_CLK &&
           !(reset & BCMA_RESET_CTL_RESET);
}

static rt_err_t brcmf_chip_core_disable(struct brcmf_context *context,
                                        struct brcmf_core *core,
                                        rt_uint32_t pre_reset,
                                        rt_uint32_t reset)
{
    rt_err_t result;
    rt_uint32_t value = brcmf_chip_read(
        context, core->wrapbase + BCMA_RESET_CTL, &result);

    if (result != RT_EOK)
    {
        return result;
    }
    if (!(value & BCMA_RESET_CTL_RESET))
    {
        result = brcmf_chip_write(context, core->wrapbase + BCMA_IOCTL,
                                  pre_reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
        if (result != RT_EOK)
        {
            return result;
        }
        (void)brcmf_chip_read(context, core->wrapbase + BCMA_IOCTL, &result);
        if (result != RT_EOK)
        {
            return result;
        }
        result = brcmf_chip_write(context, core->wrapbase + BCMA_RESET_CTL,
                                  BCMA_RESET_CTL_RESET);
        if (result != RT_EOK)
        {
            return result;
        }
        cpu_ticks_delay_us(20);
    }
    result = brcmf_chip_write(context, core->wrapbase + BCMA_IOCTL,
                              reset | BCMA_IOCTL_FGC | BCMA_IOCTL_CLK);
    if (result != RT_EOK)
    {
        return result;
    }
    (void)brcmf_chip_read(context, core->wrapbase + BCMA_IOCTL, &result);
    return result;
}

static rt_err_t brcmf_chip_core_reset(struct brcmf_context *context,
                                      struct brcmf_core *core,
                                      rt_uint32_t pre_reset,
                                      rt_uint32_t reset,
                                      rt_uint32_t post_reset)
{
    rt_err_t result;
    rt_uint32_t count;

    result = brcmf_chip_core_disable(context, core, pre_reset, reset);
    if (result != RT_EOK)
    {
        return result;
    }
    for (count = 0; count < 50U; count++)
    {
        rt_uint32_t value;

        result = brcmf_chip_write(context, core->wrapbase + BCMA_RESET_CTL, 0);
        if (result != RT_EOK)
        {
            return result;
        }
        value = brcmf_chip_read(context, core->wrapbase + BCMA_RESET_CTL,
                                &result);
        if (result == RT_EOK && !(value & BCMA_RESET_CTL_RESET))
        {
            break;
        }
        cpu_ticks_delay_us(50);
    }
    if (count == 50U)
    {
        return -RT_ETIMEOUT;
    }
    result = brcmf_chip_write(context, core->wrapbase + BCMA_IOCTL,
                              post_reset | BCMA_IOCTL_CLK);
    if (result != RT_EOK)
    {
        return result;
    }
    (void)brcmf_chip_read(context, core->wrapbase + BCMA_IOCTL, &result);
    return result;
}

static rt_uint32_t brcmf_chip_tcm_rambase(struct brcmf_chip *chip)
{
    switch (chip->id)
    {
    case 0x4345:
    case 43454:
        return 0x198000U;
    case 0x4335:
    case 0x4339:
    case 0x4354:
    case 0x4356:
        return 0x180000U;
    case 0x4359:
        return chip->revision < 9U ? 0x180000U : 0x160000U;
    case 0x4373:
        return 0x160000U;
    case 43752:
        return 0x170000U;
    default:
        return 0xffffffffU;
    }
}

static rt_err_t brcmf_chip_get_raminfo(struct brcmf_context *context)
{
    struct brcmf_chip *chip = &context->chip;
    struct brcmf_core *core = brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CR4);
    rt_err_t result = RT_EOK;

    chip->ramsize = 0;
    chip->srsize = 0;
    if (core)
    {
        rt_uint32_t capability = brcmf_chip_read(
            context, core->base + ARMCR4_CAP, &result);
        rt_uint32_t banks;
        rt_uint32_t index;

        if (result != RT_EOK)
        {
            return result;
        }
        banks = (capability & ARMCR4_TCBANB_MASK) +
                ((capability & ARMCR4_TCBBNB_MASK) >> ARMCR4_TCBBNB_SHIFT);
        for (index = 0; index < banks; index++)
        {
            rt_uint32_t info;
            rt_uint32_t multiplier = ARMCR4_BANK_SIZE_MULT;

            result = brcmf_chip_write(context, core->base + ARMCR4_BANKIDX,
                                      index);
            if (result != RT_EOK)
            {
                return result;
            }
            info = brcmf_chip_read(context, core->base + ARMCR4_BANKINFO,
                                   &result);
            if (result != RT_EOK)
            {
                return result;
            }
            if (info & ARMCR4_BANK_SIZE_1K)
            {
                multiplier >>= 3;
            }
            chip->ramsize += ((info & ARMCR4_BANK_SIZE_MASK) + 1U) *
                             multiplier;
        }
        chip->rambase = brcmf_chip_tcm_rambase(chip);
    }
    else if ((core = brcmf_chip_get_core(chip, BRCMF_CORE_SYS_MEM)) != RT_NULL)
    {
        rt_uint32_t info;
        rt_uint32_t banks;
        rt_uint32_t index;

        if (!brcmf_chip_core_is_up(context, core))
        {
            result = brcmf_chip_core_reset(context, core, 0, 0, 0);
            if (result != RT_EOK)
            {
                return result;
            }
        }
        info = brcmf_chip_read(context, core->base + SOCRAM_COREINFO,
                               &result);
        if (result != RT_EOK)
        {
            return result;
        }
        banks = (info & SOCRAM_SRNB_MASK) >> SOCRAM_SRNB_SHIFT;
        for (index = 0; index < banks; index++)
        {
            rt_uint32_t bank_info;

            result = brcmf_chip_write(context, core->base + SOCRAM_BANKIDX,
                                      index);
            if (result != RT_EOK)
            {
                return result;
            }
            bank_info = brcmf_chip_read(
                context, core->base + SOCRAM_BANKINFO, &result);
            if (result != RT_EOK)
            {
                return result;
            }
            chip->ramsize +=
                ((bank_info & SOCRAM_BANKINFO_SIZE_MASK) + 1U) *
                SOCRAM_BANKINFO_SIZE_BASE;
        }
        chip->rambase = brcmf_chip_tcm_rambase(chip);
    }
    else
    {
        rt_uint32_t info;
        rt_uint32_t banks;

        core = brcmf_chip_get_core(chip, BRCMF_CORE_INTERNAL_MEM);
        if (!core)
        {
            return -RT_ENOSYS;
        }
        if (!brcmf_chip_core_is_up(context, core))
        {
            result = brcmf_chip_core_reset(context, core, 0, 0, 0);
            if (result != RT_EOK)
            {
                return result;
            }
        }
        info = brcmf_chip_read(context, core->base + SOCRAM_COREINFO, &result);
        if (result != RT_EOK)
        {
            return result;
        }
        banks = (info & SOCRAM_SRNB_MASK) >> SOCRAM_SRNB_SHIFT;
        if (core->revision <= 7U || core->revision == 12U)
        {
            rt_uint32_t bank_size = info & SOCRAM_SRBSZ_MASK;
            rt_uint32_t last = (info & SOCRAM_LSS_MASK) >> SOCRAM_LSS_SHIFT;

            if (last && banks)
            {
                banks--;
            }
            chip->ramsize = banks <<
                (bank_size + SOCRAM_BANK_SIZE_BASE_SHIFT);
            if (last)
            {
                chip->ramsize += 1U <<
                    (last - 1U + SOCRAM_BANK_SIZE_BASE_SHIFT);
            }
        }
        else
        {
            rt_uint32_t index;

            if (core->revision >= 23U)
            {
                banks = (info & (SOCRAM_SRNB_MASK | SOCRAM_SRNB_MASK_EXT)) >>
                        SOCRAM_SRNB_SHIFT;
            }
            for (index = 0; index < banks; index++)
            {
                rt_uint32_t bank_info;
                rt_uint32_t size;

                result = brcmf_chip_write(
                    context, core->base + SOCRAM_BANKIDX, index);
                if (result != RT_EOK)
                {
                    return result;
                }
                bank_info = brcmf_chip_read(
                    context, core->base + SOCRAM_BANKINFO, &result);
                if (result != RT_EOK)
                {
                    return result;
                }
                size = ((bank_info & SOCRAM_BANKINFO_SIZE_MASK) + 1U) *
                       SOCRAM_BANKINFO_SIZE_BASE;
                chip->ramsize += size;
                if (bank_info & SOCRAM_BANKINFO_RETNTRAM)
                {
                    chip->srsize += size;
                }
            }
        }
        chip->rambase = 0;
    }
    if (!chip->ramsize || chip->rambase == 0xffffffffU ||
        chip->ramsize > 4U * 1024U * 1024U)
    {
        return -RT_EIO;
    }
    return RT_EOK;
}

rt_err_t brcmf_chip_set_passive(struct brcmf_context *context)
{
    struct brcmf_chip *chip = &context->chip;
    struct brcmf_core *cpu = brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CR4);
    struct brcmf_core *d11;
    rt_size_t index;
    rt_err_t result;

    if (cpu)
    {
        result = brcmf_chip_core_reset(context, cpu, BCMA_IOCTL_CPUHALT,
                                       BCMA_IOCTL_CPUHALT,
                                       BCMA_IOCTL_CPUHALT);
        if (result != RT_EOK)
        {
            return result;
        }
        d11 = RT_NULL;
        for (index = 0; index < chip->core_count; index++)
        {
            if (chip->cores[index].id != BRCMF_CORE_80211)
            {
                continue;
            }
            d11 = &chip->cores[index];
            result = brcmf_chip_core_disable(
                context, d11,
                D11_IOCTL_PHYRESET | D11_IOCTL_PHYCLOCKEN,
                D11_IOCTL_PHYCLOCKEN);
            if (result != RT_EOK)
            {
                return result;
            }
        }
        return d11 ? RT_EOK : -RT_ENOSYS;
    }

    cpu = brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CA7);
    if (cpu)
    {
        result = brcmf_chip_core_reset(context, cpu, BCMA_IOCTL_CPUHALT,
                                       BCMA_IOCTL_CPUHALT,
                                       BCMA_IOCTL_CPUHALT);
        d11 = brcmf_chip_get_core(chip, BRCMF_CORE_80211);
        return result == RT_EOK && d11 ? brcmf_chip_core_reset(
            context, d11,
            D11_IOCTL_PHYRESET | D11_IOCTL_PHYCLOCKEN,
            D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN) :
            (result != RT_EOK ? result : -RT_ENOSYS);
    }

    cpu = brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CM3);
    d11 = brcmf_chip_get_core(chip, BRCMF_CORE_80211);
    if (!cpu || !d11)
    {
        return -RT_ENOSYS;
    }
    result = brcmf_chip_core_disable(context, cpu, 0, 0);
    if (result == RT_EOK)
    {
        result = brcmf_chip_core_reset(
            context, d11, D11_IOCTL_PHYRESET | D11_IOCTL_PHYCLOCKEN,
            D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN);
    }
    if (result == RT_EOK)
    {
        struct brcmf_core *memory = brcmf_chip_get_core(
            chip, BRCMF_CORE_INTERNAL_MEM);

        if (!memory)
        {
            return -RT_ENOSYS;
        }
        result = brcmf_chip_core_reset(context, memory, 0, 0, 0);
        if (result == RT_EOK && (chip->id == 43430U || chip->id == 43439U))
        {
            result = brcmf_chip_write(
                context, memory->base + SOCRAM_BANKIDX, 3U);
            if (result == RT_EOK)
            {
                result = brcmf_chip_write(
                    context, memory->base + SOCRAM_BANKPDA, 0U);
            }
        }
    }
    return result;
}

rt_err_t brcmf_chip_set_active(struct brcmf_context *context,
                               rt_uint32_t reset_vector)
{
    struct brcmf_chip *chip = &context->chip;
    struct brcmf_core *cpu = brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CR4);
    struct brcmf_core *sdio = brcmf_chip_get_core(chip, BRCMF_CORE_SDIO_DEV);
    rt_err_t result;

    if (!cpu)
    {
        cpu = brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CA7);
    }
    if (sdio)
    {
        result = brcmf_chip_write(context, sdio->base + 0x20U, 0xffffffffU);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    if (cpu)
    {
        if (reset_vector)
        {
            rt_uint8_t vector[4];

            brcmf_put_le32(vector, reset_vector);
            result = brcmf_sdio_backplane_write(context, 0, vector,
                                                 sizeof(vector));
            if (result != RT_EOK)
            {
                return result;
            }
        }
        return brcmf_chip_core_reset(context, cpu, BCMA_IOCTL_CPUHALT,
                                     0, 0);
    }
    cpu = brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CM3);
    return cpu ? brcmf_chip_core_reset(context, cpu, 0, 0, 0) :
                 -RT_ENOSYS;
}

rt_err_t brcmf_chip_attach(struct brcmf_context *context)
{
    struct brcmf_chip *chip = &context->chip;
    rt_uint32_t chip_id;
    rt_uint32_t soc_type;
    struct brcmf_core *cc;
    struct brcmf_core *pmu;
    rt_err_t result = RT_EOK;

    rt_memset(chip, 0, sizeof(*chip));
    chip->enum_base = BRCMF_ENUM_BASE;
    chip_id = brcmf_chip_read(context, BRCMF_ENUM_BASE, &result);
    if (result != RT_EOK || chip_id == 0xffffffffU)
    {
        return -RT_EIO;
    }
    chip->id = chip_id & BRCMF_CID_ID_MASK;
    chip->revision = (chip_id & BRCMF_CID_REV_MASK) >> BRCMF_CID_REV_SHIFT;
    if (chip->id == 0x4335U && chip->revision >= 2U &&
        (context->function1->product == 0x4335U ||
         context->function1->product == 0x4339U))
    {
        chip->id = 0x4339U;
    }
    soc_type = (chip_id & BRCMF_CID_TYPE_MASK) >> BRCMF_CID_TYPE_SHIFT;
    if (soc_type != BRCMF_SOC_AI)
    {
        LOG_E("unsupported Sonics backplane on chip %u", chip->id);
        return -RT_ENOSYS;
    }
    result = brcmf_chip_scan_erom(context);
    if (result != RT_EOK ||
        !brcmf_chip_get_core(chip, BRCMF_CORE_CHIPCOMMON) ||
        !brcmf_chip_get_core(chip, BRCMF_CORE_SDIO_DEV) ||
        (!brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CM3) &&
         !brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CR4) &&
         !brcmf_chip_get_core(chip, BRCMF_CORE_ARM_CA7)))
    {
        return result == RT_EOK ? -RT_ENOSYS : result;
    }
    result = brcmf_chip_set_passive(context);
    if (result != RT_EOK)
    {
        return result;
    }
    result = brcmf_chip_get_raminfo(context);
    if (result != RT_EOK)
    {
        return result;
    }
    cc = brcmf_chip_get_core(chip, BRCMF_CORE_CHIPCOMMON);
    chip->cc_caps = brcmf_chip_read(
        context, cc->base + BRCMF_CC_CAPABILITIES, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    chip->cc_caps_ext = brcmf_chip_read(
        context, cc->base + BRCMF_CC_CAPABILITIES_EXT, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    pmu = cc;
    if (cc->revision >= 35U &&
        (chip->cc_caps_ext & BRCMF_CC_CAP_EXT_AOB_PRESENT))
    {
        struct brcmf_core *aob_pmu = brcmf_chip_get_core(
            chip, BRCMF_CORE_PMU);

        if (aob_pmu)
        {
            pmu = aob_pmu;
        }
    }
    if ((chip->cc_caps & BRCMF_CC_CAP_PMU) && result == RT_EOK)
    {
        chip->pmu_caps = brcmf_chip_read(
            context, pmu->base + BRCMF_CC_PMU_CAPABILITIES, &result);
        chip->pmu_revision = chip->pmu_caps & 0xffU;
    }
    LOG_I("chip %u rev %u, RAM 0x%08x+0x%x, %u cores",
          chip->id, chip->revision, chip->rambase, chip->ramsize,
          chip->core_count);
    return result;
}
