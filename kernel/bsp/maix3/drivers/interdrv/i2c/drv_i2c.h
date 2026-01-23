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
#ifndef __DW_I2C_H_
#define __DW_I2C_H_

#include <rtdef.h>

#include "drv_i2c_reg.h"

/* I2C register layout */
struct dw_i2c_regs {
    struct dw_ic_con_t              ic_con;           /* 0x00 */
    struct dw_ic_tar_t              ic_tar;           /* 0x04 */
    struct dw_ic_sar_t              ic_sar;           /* 0x08 */
    struct dw_ic_hs_maddr_t         ic_hs_maddr;      /* 0x0c */
    struct dw_ic_data_cmd_t         ic_cmd_data;      /* 0x10 */
    struct dw_ic_ss_scl_hcnt_t      ic_ss_scl_hcnt;   /* 0x14 */
    struct dw_ic_ss_scl_lcnt_t      ic_ss_scl_lcnt;   /* 0x18 */
    struct dw_ic_fs_scl_hcnt_t      ic_fs_scl_hcnt;   /* 0x1c */
    struct dw_ic_fs_scl_lcnt_t      ic_fs_scl_lcnt;   /* 0x20 */
    struct dw_ic_hs_scl_hcnt_t      ic_hs_scl_hcnt;   /* 0x24 */
    struct dw_ic_hs_scl_lcnt_t      ic_hs_scl_lcnt;   /* 0x28 */
    struct dw_ic_intr_t             ic_intr_stat;     /* 0x2c */
    struct dw_ic_intr_t             ic_intr_mask;     /* 0x30 */
    struct dw_ic_intr_t             ic_raw_intr_stat; /* 0x34 */
    struct dw_ic_rx_tl_t            ic_rx_tl;         /* 0x38 */
    struct dw_ic_tx_tl_t            ic_tx_tl;         /* 0x3c */
    struct dw_ic_clr_intr_t         ic_clr_intr;      /* 0x40 */
    struct dw_ic_clr_rx_under_t     ic_clr_rx_under;  /* 0x44 */
    struct dw_ic_clr_rx_over_t      ic_clr_rx_over;   /* 0x48 */
    struct dw_ic_clr_tx_over_t      ic_clr_tx_over;   /* 0x4c */
    struct dw_ic_clr_rd_req_t       ic_clr_rd_req;    /* 0x50 */
    struct dw_ic_clr_tx_abrt_t      ic_clr_tx_abrt;   /* 0x54 */
    struct dw_ic_clr_rx_done_t      ic_clr_rx_done;   /* 0x58 */
    struct dw_ic_clr_activity_t     ic_clr_activity;  /* 0x5c */
    struct dw_ic_clr_stop_det_t     ic_clr_stop_det;  /* 0x60 */
    struct dw_ic_clr_start_det_t    ic_clr_start_det; /* 0x64 */
    struct dw_ic_clr_gen_call_t     ic_clr_gen_call;  /* 0x68 */
    struct dw_ic_enable_t           ic_enable;        /* 0x6c */
    struct dw_ic_status_t           ic_status;        /* 0x70 */
    struct dw_ic_txflr_t            ic_txflr;         /* 0x74 */
    struct dw_ic_rxflr_t            ic_rxflr;         /* 0x78 */
    struct dw_ic_sda_hold_t         ic_sda_hold;      /* 0x7c */
    struct dw_ic_tx_abrt_source_t   ic_tx_abrt_source;/* 0x80 */
    struct dw_ic_slv_data_nack_only_t ic_slv_data_nak_only; /* 0x84 */
    struct dw_ic_dma_cr_t           ic_dma_cr;        /* 0x88 */
    struct dw_ic_dma_tdlr_t         ic_dma_tdlr;      /* 0x8c */
    struct dw_ic_dma_rdlr_t         ic_dma_rdlr;      /* 0x90 */
    struct dw_ic_sda_setup_t        ic_sda_setup;     /* 0x94 */
    struct dw_ic_ack_general_call_t ic_ack_general_call; /* 0x98 */
    struct dw_ic_enable_status_t    ic_enable_status; /* 0x9c */
    struct dw_ic_fs_spklen_t        ic_fs_spklen;     /* 0xa0 */
    struct dw_ic_hs_spklen_t        ic_hs_spklen;     /* 0xa4 */
    struct dw_ic_clr_restart_det_t  ic_clr_restart_det; /* 0xa8 */
    struct dw_ic_scl_stuck_at_low_timeout_t ic_scl_stuck_at_low_timeout; /* 0xac */
    struct dw_ic_sda_stuck_at_low_timeout_t ic_sda_stuck_at_low_timeout; /* 0xb0 */
    struct dw_ic_clr_scl_stuck_det_t ic_clr_scl_stuck_det; /* 0xb4 */
    struct dw_ic_device_id_t        ic_device_id;     /* 0xb8 */
    struct dw_ic_smbus_clk_low_sext_t ic_smbus_clk_low_sext; /* 0xbc */
    struct dw_ic_smbus_clk_low_mext_t ic_smbus_clk_low_mext; /* 0xc0 */
    struct dw_ic_smbus_thigh_max_idle_count_t ic_smbus_thigh_max_idle_count; /* 0xc4 */
    struct dw_ic_smbus_intr_stat_t  ic_smbus_intr_stat; /* 0xc8 */
    struct dw_ic_smbus_intr_mask_t  ic_smbus_intr_mask; /* 0xcc */
    struct dw_ic_smbus_raw_intr_stat_t ic_smbus_raw_intr_stat; /* 0xd0 */
    struct dw_ic_clr_smbus_intr_t   ic_clr_smbus_intr; /* 0xd4 */
    struct dw_ic_optional_sar_t     ic_optional_sar;  /* 0xd8 */
    struct dw_ic_smbus_udid_lsb_t   ic_smbus_udid_lsb; /* 0xdc */
    struct dw_ic_reserve_t          ic_reserve[5];    /* 0xe0 - 0xf0 */
    struct dw_ic_comp_param_1_t     comp_param1;      /* 0xf4 */
    struct dw_ic_comp_version_t     comp_version;     /* 0xf8 */
    struct dw_ic_comp_type_t        comp_type;        /* 0xfc */
};

/* Typical FIFO depth for this controller (can be overridden at runtime) */
#define DW_I2C_DEFAULT_FIFO_DEPTH    16U

/* I2C controller count on this SoC */
#define K230_I2C_MAX_NUM             5U

/* Common frequencies (Hz) */
#define DW_I2C_SPEED_STANDARD        100000U    /* 100 kHz */
#define DW_I2C_SPEED_FAST            400000U    /* 400 kHz */
#define DW_I2C_SPEED_HIGH           3400000U    /* 3.4 MHz */

/* Optional slave EEPROM helper ioctls */
#define I2C_SLAVE_IOCTL_SET_BUFFER_SIZE 0
#define I2C_SLAVE_IOCTL_SET_ADDR        1

#endif /* __DW_I2C_H_ */
