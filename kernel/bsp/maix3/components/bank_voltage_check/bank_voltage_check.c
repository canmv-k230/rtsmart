/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>

#include "rtthread.h"

#define DBG_TAG "bank_voltage"
// #define DBG_LVL DBG_LOG
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

#ifdef RT_USING_PUFS

#include "drv_fpioa.h"
#include "pufs_rt.h"

#define OTP_BOOT_CFG_ADDR            ((pufs_otp_addr_t)0x0018)
#define OTP_BOOT_CFG_VALID_BIT       (1u << 0)
#define OTP_BOOT_CFG_UART_SEL_SHIFT  1
#define OTP_BOOT_CFG_UART_SEL_MASK   (0xFu << OTP_BOOT_CFG_UART_SEL_SHIFT)
#define OTP_BOOT_CFG_SDIO1_SEL_SHIFT 5
#define OTP_BOOT_CFG_SDIO1_SEL_MASK  (0x3u << OTP_BOOT_CFG_SDIO1_SEL_SHIFT)
#define OTP_BOOT_CFG_OSPI_VOL_BIT    (1u << 7)
#define OTP_BOOT_CFG_UART_VOL_BIT    (1u << 8)
#define OTP_BOOT_CFG_SDIO1_VOL_BIT   (1u << 9)

typedef enum _bank_voltage_group {
    BANK_IO0_1,
    BANK0_IO2_13,
    BANK1_IO14_25,
    BANK2_IO26_37,
    BANK3_IO38_49,
    BANK4_IO50_61,
    BANK5_IO62_63,
    BANK_GROUP_COUNT,
} bank_voltage_group_t;

typedef struct _bank_voltage_desc {
    int         first_pin;
    int         last_pin;
    const char* name;
} bank_voltage_desc_t;

static const bank_voltage_desc_t g_bank_voltage_descs[BANK_GROUP_COUNT] = {
    [BANK_IO0_1] = { 0, 1, "VOL_BANK_IO0_1" },         [BANK0_IO2_13] = { 2, 13, "VOL_BANK0_IO2_13" },
    [BANK1_IO14_25] = { 14, 25, "VOL_BANK1_IO14_25" }, [BANK2_IO26_37] = { 26, 37, "VOL_BANK2_IO26_37" },
    [BANK3_IO38_49] = { 38, 49, "VOL_BANK3_IO38_49" }, [BANK4_IO50_61] = { 50, 61, "VOL_BANK4_IO50_61" },
    [BANK5_IO62_63] = { 62, 63, "VOL_BANK5_IO62_63" },
};

static const bank_voltage_group_t g_uart_sel_bank_map[] = {
    BANK0_IO2_13,  BANK0_IO2_13,  BANK0_IO2_13,  BANK0_IO2_13,  BANK2_IO26_37, BANK2_IO26_37,
    BANK2_IO26_37, BANK3_IO38_49, BANK3_IO38_49, BANK3_IO38_49, BANK3_IO38_49, BANK4_IO50_61,
};

static const bank_voltage_group_t g_sdio1_sel_bank_map[] = {
    BANK2_IO26_37,
    BANK4_IO50_61,
};

static const char* bank_voltage_name(int value) { return (value == BANK_VOL_1V8_MSC) ? "1.8V" : "3.3V"; }

static const char* bank_voltage_group_name(bank_voltage_group_t bank)
{
    if (bank < 0 || bank >= BANK_GROUP_COUNT) {
        return "invalid";
    }

    return g_bank_voltage_descs[bank].name;
}

static inline uint32_t otp_word_from_bytes(const uint8_t word_bytes[4])
{
    return ((uint32_t)word_bytes[0]) | ((uint32_t)word_bytes[1] << 8) | ((uint32_t)word_bytes[2] << 16)
        | ((uint32_t)word_bytes[3] << 24);
}

static int get_bank_voltage_group_state(bank_voltage_group_t bank, int* msc_value, int* is_consistent)
{
    fpioa_iomux_cfg_t cfg;
    int               first_value = -1;
    int               consistent  = 1;

    if (bank < 0 || bank >= BANK_GROUP_COUNT || msc_value == RT_NULL || is_consistent == RT_NULL) {
        return -1;
    }

    for (int pin = g_bank_voltage_descs[bank].first_pin; pin <= g_bank_voltage_descs[bank].last_pin; pin++) {
        if (drv_fpioa_get_pin_cfg(pin, &cfg.u.value) != 0) {
            LOG_E("get pin %d cfg failed while checking %s", pin, g_bank_voltage_descs[bank].name);
            return -1;
        }

        if (first_value < 0) {
            first_value = cfg.u.bit.msc;
            continue;
        }

        if (cfg.u.bit.msc != first_value) {
            consistent = 0;
        }
    }

    *msc_value     = (first_value < 0) ? BANK_VOL_3V3_MSC : first_value;
    *is_consistent = consistent;

    return 0;
}

static int add_bank_voltage_requirement(bank_voltage_group_t bank, int msc_value, const char* source,
                                        int required_msc[BANK_GROUP_COUNT], const char* required_source[BANK_GROUP_COUNT])
{
    if (required_msc[bank] < 0) {
        required_msc[bank]    = msc_value;
        required_source[bank] = source;
        return 0;
    }

    if (required_msc[bank] == msc_value) {
        LOG_D("%s requirement repeats %s from %s", g_bank_voltage_descs[bank].name, bank_voltage_name(msc_value), source);
        return 0;
    }

    LOG_W("otp bank voltage conflict on %s: %s requests %s, %s requests %s", g_bank_voltage_descs[bank].name,
          required_source[bank], bank_voltage_name(required_msc[bank]), source, bank_voltage_name(msc_value));

    return -1;
}

static int bank_voltage_check_init(void)
{
    uint8_t       word_bytes[4] = { 0 };
    uint32_t      otp_word;
    uint32_t      uart_sel;
    uint32_t      sdio1_sel;
    int           required_msc[BANK_GROUP_COUNT];
    const char*   required_source[BANK_GROUP_COUNT];
    pufs_status_t status;

    for (int index = 0; index < BANK_GROUP_COUNT; index++) {
        required_msc[index]    = -1;
        required_source[index] = RT_NULL;
    }

    status = pufs_otp_read(word_bytes, sizeof(word_bytes), OTP_BOOT_CFG_ADDR);
    if (status != SUCCESS) {
        LOG_W("read otp boot cfg 0x%04x failed (%d)", (unsigned int)OTP_BOOT_CFG_ADDR, status);
        return 0;
    }

    otp_word = otp_word_from_bytes(word_bytes);
    LOG_D("otp[0x%04x] raw bytes=%02x %02x %02x %02x raw_word=0x%08x", (unsigned int)OTP_BOOT_CFG_ADDR, word_bytes[0],
          word_bytes[1], word_bytes[2], word_bytes[3], otp_word);

    if ((otp_word & OTP_BOOT_CFG_VALID_BIT) == 0) {
        LOG_I("otp boot cfg 0x%04x is not valid, skip bank voltage check", (unsigned int)OTP_BOOT_CFG_ADDR);
        return 0;
    }

    LOG_D("otp decode: ospi_vol=%s uart_vol=%s sdio1_vol=%s uart_sel=%u sdio1_sel=%u",
          bank_voltage_name((otp_word & OTP_BOOT_CFG_OSPI_VOL_BIT) ? BANK_VOL_1V8_MSC : BANK_VOL_3V3_MSC),
          bank_voltage_name((otp_word & OTP_BOOT_CFG_UART_VOL_BIT) ? BANK_VOL_1V8_MSC : BANK_VOL_3V3_MSC),
          bank_voltage_name((otp_word & OTP_BOOT_CFG_SDIO1_VOL_BIT) ? BANK_VOL_1V8_MSC : BANK_VOL_3V3_MSC),
          (unsigned int)((otp_word & OTP_BOOT_CFG_UART_SEL_MASK) >> OTP_BOOT_CFG_UART_SEL_SHIFT),
          (unsigned int)((otp_word & OTP_BOOT_CFG_SDIO1_SEL_MASK) >> OTP_BOOT_CFG_SDIO1_SEL_SHIFT));

    (void)add_bank_voltage_requirement(BANK1_IO14_25,
                                       (otp_word & OTP_BOOT_CFG_OSPI_VOL_BIT) ? BANK_VOL_1V8_MSC : BANK_VOL_3V3_MSC, "otp.ospi",
                                       required_msc, required_source);

    uart_sel = (otp_word & OTP_BOOT_CFG_UART_SEL_MASK) >> OTP_BOOT_CFG_UART_SEL_SHIFT;
    if (uart_sel < (sizeof(g_uart_sel_bank_map) / sizeof(g_uart_sel_bank_map[0]))) {
        LOG_D("otp uart selection %u -> %s", (unsigned int)uart_sel, bank_voltage_group_name(g_uart_sel_bank_map[uart_sel]));
        (void)add_bank_voltage_requirement(g_uart_sel_bank_map[uart_sel],
                                           (otp_word & OTP_BOOT_CFG_UART_VOL_BIT) ? BANK_VOL_1V8_MSC : BANK_VOL_3V3_MSC,
                                           "otp.uart", required_msc, required_source);
    } else {
        LOG_W("invalid otp uart iomux selection %u", (unsigned int)uart_sel);
    }

    sdio1_sel = (otp_word & OTP_BOOT_CFG_SDIO1_SEL_MASK) >> OTP_BOOT_CFG_SDIO1_SEL_SHIFT;
    if (sdio1_sel < (sizeof(g_sdio1_sel_bank_map) / sizeof(g_sdio1_sel_bank_map[0]))) {
        LOG_D("otp sdio1 selection %u -> %s", (unsigned int)sdio1_sel,
              bank_voltage_group_name(g_sdio1_sel_bank_map[sdio1_sel]));
        (void)add_bank_voltage_requirement(g_sdio1_sel_bank_map[sdio1_sel],
                                           (otp_word & OTP_BOOT_CFG_SDIO1_VOL_BIT) ? BANK_VOL_1V8_MSC : BANK_VOL_3V3_MSC,
                                           "otp.sdio1", required_msc, required_source);
    } else {
        LOG_W("invalid otp sdio1 iomux selection %u", (unsigned int)sdio1_sel);
    }

    for (int bank = 0; bank < BANK_GROUP_COUNT; bank++) {
        int current_msc;
        int is_consistent;

        if (required_msc[bank] < 0) {
            continue;
        }

        if (get_bank_voltage_group_state(bank, &current_msc, &is_consistent) != 0) {
            continue;
        }

        LOG_D("bank %s current=%s%s expected=%s source=%s", g_bank_voltage_descs[bank].name,
              is_consistent ? bank_voltage_name(current_msc) : "mixed", is_consistent ? "" : " state",
              bank_voltage_name(required_msc[bank]), required_source[bank]);

        if ((is_consistent != 0) && (current_msc == required_msc[bank])) {
            continue;
        }

        LOG_W("%s is %s%s, otp expects %s (%s)", g_bank_voltage_descs[bank].name,
              is_consistent ? bank_voltage_name(current_msc) : "mixed", is_consistent ? "" : " state",
              bank_voltage_name(required_msc[bank]), required_source[bank]);
    }

    return 0;
}
INIT_COMPONENT_EXPORT(bank_voltage_check_init);

#endif
