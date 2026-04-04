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

#pragma once

#include <rtdevice.h>
#include <rtthread.h>

#include <stdint.h>
#include <sys/ioctl.h>

#ifndef K_IOC_TYPE_FFT
#define K_IOC_TYPE_FFT 'f'
#endif

typedef enum {
    FFT_N64 = 0,
    FFT_N128,
    FFT_N256,
    FFT_N512,
    FFT_N1024,
    FFT_N2048,
    FFT_N4096,
} fft_point_e;

typedef enum {
    FFT_MODE = 0,
    IFFT_MODE,
} k_fft_mode_e;

typedef enum {
    RIRI = 0,
    RRRR,
    RR_II,
} k_fft_input_mode_e;

typedef enum {
    RIRI_OUT = 0,
    RR_II_OUT,
} k_fft_out_mode_e;

typedef union {
    struct {
        volatile fft_point_e        point : 3;
        volatile k_fft_mode_e       mode : 1;
        volatile k_fft_input_mode_e im : 2;
        volatile k_fft_out_mode_e   om : 1;
        volatile uint64_t           fft_intr_mask : 1;
        volatile uint16_t           shift : 12;
        volatile uint32_t           fft_disable_cg : 1;
        volatile uint32_t           reserv : 11;
        volatile uint32_t           time_out : 32;
    } __attribute__((packed));
    volatile uint64_t cfg_value;
} __attribute__((packed)) k_fft_cfg_reg_st;

typedef struct {
    uint32_t point;
    uint32_t mode;
    uint32_t input_mode;
    uint32_t output_mode;
    uint16_t shift;
    uint16_t reserved0;
    uint32_t timeout_ms;
    uint64_t input_phy_addr;
    uint32_t input_len;
    uint32_t reserved1;
    uint64_t output_phy_addr;
    uint32_t output_len;
    uint32_t reserved2;
} k_fft_run_request;

#define KD_IOC_CMD_FFT_RUN _IOW(K_IOC_TYPE_FFT, 204, k_fft_run_request)

int k230_fft_dev_init(void);

/**
 * k230_fft_run - execute an FFT/IFFT request from kernel context
 *
 * Shares the same mutex as the userspace ioctl path, so kernel callers
 * and userspace callers are serialized against each other.
 *
 * The caller must provide physical addresses for input/output buffers
 * and ensure caches are flushed/invalidated as needed.
 *
 * Returns 0 on success, negative errno on failure.
 */
int k230_fft_run(const k_fft_run_request* req);
