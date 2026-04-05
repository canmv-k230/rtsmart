/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
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

#pragma once

#include <stdint.h>
#include <rtdef.h>

#define K_ADC_MAX_CHANNEL (6)

#define K_ADC_INTER_VREF_0P85 (0x00)
#define K_ADC_INTER_VREF_0P90 (0x01)
#define K_ADC_INTER_VREF_0P95 (0x02)
#define K_ADC_INTER_VREF_1P00 (0x03)

struct k_adc_trim_reg {
    union {
        struct {
            uint32_t enadc : 1;
            uint32_t resv_3_1 : 3;
            uint32_t inter_ref_sel : 1;
            uint32_t resv_7_5 : 3;
            uint32_t vref_sel : 2;
            uint32_t resv_11_10 : 2;
            uint32_t bgtrim : 4;
            uint32_t resv_19_16 : 4;
            uint32_t oscal_en : 1;
            uint32_t resv_23_21 : 3;
            uint32_t oscal_done : 1;
            uint32_t resv_31_25 : 7;
        } bits;
        uint32_t data;
    };
};

struct k_adc_cfg_reg {
    union {
        struct {
            uint32_t in_sel : 3;
            uint32_t resv_3 : 1;
            uint32_t start_of_conv : 1;
            uint32_t resv_7_5 : 3;
            uint32_t busy : 1;
            uint32_t resv_11_9 : 3;
            uint32_t end_of_conv : 1;
            uint32_t resv_15_13 : 3;
            uint32_t outen : 1;
            uint32_t resv_31_17 : 15;
        } bits;
        uint32_t data;
    };
};

struct k_adc_mode_reg {
    union {
        struct {
            uint32_t mode_sel : 2;
            uint32_t resv_3_2 : 2;
            uint32_t dma_pause : 1;
            uint32_t resv_7_5 : 3;
            uint32_t dma1_en : 1;
            uint32_t dma1_in_sel : 3;
            uint32_t dma1_clr : 1;
            uint32_t resv_15_13 : 3;
            uint32_t dma2_in_sel : 3;
            uint32_t dma2_clr : 1;
            uint32_t resv_23_20 : 4;
            uint32_t dma3_in_sel : 3;
            uint32_t dma3_clr : 1;
            uint32_t resv_31_28 : 4;
        } bits;
        uint32_t data;
    };
};

#define K_ADC_THRESHOLD_SEL_HIGH_PASS (0x00)
#define K_ADC_THRESHOLD_SEL_BAND_PASS (0x01)
#define K_ADC_THRESHOLD_SEL_BAND_STOP (0x02)
#define K_ADC_THRESHOLD_SEL_LOW_PASS  (0x03)

struct k_adc_thsd_reg {
    union {
        struct {
            uint32_t threshold_sel : 2;
            uint32_t resv_3_2 : 2;
            uint32_t threshold_low : 12;
            uint32_t threshold_high : 12;
            uint32_t resv_31_28 : 4;
        } bits;
        uint32_t data;
    };
};

struct k_adc_intr_reg {
    union {
        struct {
            uint32_t dma1_intr : 1;
            uint32_t dma2_intr : 1;
            uint32_t dma3_intr : 1;
            uint32_t resv_31_3 : 29;
        } bits;
        uint32_t data;
    };
};

struct k_adc_data_reg {
    union {
        struct {
            uint32_t data : 12;
            uint32_t resv_31_12 : 20;
        } bits;
        uint32_t data;
    };
};

struct k_adc_reg {
    volatile struct k_adc_trim_reg trim; // 0x00: LSADC initializes the self-calibrating control register
    volatile struct k_adc_cfg_reg  cfg; // 0x04: LSADC Data conversion control registe
    volatile struct k_adc_mode_reg mode; // 0x08: LSADC output mode selection control register
    volatile struct k_adc_thsd_reg threshold; // 0x0C: LSADC threshold interrupt control register
    volatile struct k_adc_intr_reg intr; // 0x10: LSADC's DMA error interrupt register
    volatile struct k_adc_data_reg data[6]; // 0x14 - 0x28: Input channel N Digital signal output
    volatile struct k_adc_data_reg dma_data[3]; // 0x2C - 0x34: Continuous sampling channel N digital signal output
};

rt_bool_t k230_adc_acodec_hp_detect_enabled(void);
rt_uint32_t k230_adc_acodec_hp_detect_channel(void);
