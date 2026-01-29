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

#include <stdint.h>

/* IC_CON (0x00) */
struct dw_ic_con_t {
    union {
        struct {
            uint32_t MASTER_MODE                  : 1;  /* bit 0 */
            uint32_t SPEED                        : 2;  /* bits 2:1 */
            uint32_t IC_10BITADDR_SLAVE           : 1;  /* bit 3 */
            uint32_t IC_10BITADDR_MASTER          : 1;  /* bit 4 */
            uint32_t IC_RESTART_EN                : 1;  /* bit 5 */
            uint32_t IC_SLAVE_DISABLE             : 1;  /* bit 6 */
            uint32_t STOP_DET_IFADDRESSED         : 1;  /* bit 7 */
            uint32_t TX_EMPTY_CTRL                : 1;  /* bit 8 */
            uint32_t RX_FIFO_FULL_HLD_CTRL        : 1;  /* bit 9 */
            uint32_t STOP_DET_IF_MASTER_ACTIVE    : 1;  /* bit 10 */
            uint32_t BUS_CLEAR_FEATURE_CTRL       : 1;  /* bit 11 */
            uint32_t RSVD_IC_CON_1                : 4;  /* bits 15:12 */
            uint32_t OPTIONAL_SAR_CTRL            : 1;  /* bit 16 */
            uint32_t SMBUS_SLAVE_QUICK_EN         : 1;  /* bit 17 */
            uint32_t SMBUS_ARP_EN                 : 1;  /* bit 18 */
            uint32_t SMBUS_PERSISTENT_SLV_ADDR_EN : 1;  /* bit 19 */
            uint32_t RSVD_IC_CON_2                : 12; /* bits 31:20 */
        } bits;
        uint32_t reg;
    };
};

/* IC_ENABLE (0x6c) */
struct dw_ic_enable_t {
    union {
        struct {
            uint32_t ENABLE                    : 1;  /* bit 0 */
            uint32_t ABORT                     : 1;  /* bit 1 */
            uint32_t TX_CMD_BLOCK              : 1;  /* bit 2 */
            uint32_t SDA_STUCK_RECOVERY_ENABLE : 1;  /* bit 3 */
            uint32_t RSVD_IC_ENABLE_1          : 12; /* bits 15:4 */
            uint32_t SMBUS_CLK_RESET           : 1;  /* bit 16 */
            uint32_t SMBUS_SUSPEND_EN          : 1;  /* bit 17 */
            uint32_t SMBUS_ALERT_EN            : 1;  /* bit 18 */
            uint32_t RSVD_IC_ENABLE_2          : 13; /* bits 31:19 */
        } bits;
        uint32_t reg;
    };
};

/* IC_STATUS (0x70) */
struct dw_ic_status_t {
    union {
        struct {
            uint32_t ACTIVITY                  : 1;  /* bit 0 */
            uint32_t TFNF                      : 1;  /* bit 1 */
            uint32_t TFE                       : 1;  /* bit 2 */
            uint32_t RFNE                      : 1;  /* bit 3 */
            uint32_t RFF                       : 1;  /* bit 4 */
            uint32_t MST_ACTIVITY              : 1;  /* bit 5 */
            uint32_t SLV_ACTIVITY              : 1;  /* bit 6 */
            uint32_t MST_HOLD_TX_FIFO_EMPTY    : 1;  /* bit 7 */
            uint32_t MST_HOLD_RX_FIFO_FULL     : 1;  /* bit 8 */
            uint32_t SLV_HOLD_TX_FIFO_EMPTY    : 1;  /* bit 9 */
            uint32_t SLV_HOLD_RX_FIFO_FULL     : 1;  /* bit 10 */
            uint32_t SDA_STUCK_NOT_RECOVERED   : 1;  /* bit 11 */
            uint32_t RSVD_IC_STATUS_1          : 4;  /* bits 15:12 */
            uint32_t SMBUS_QUICK_CMD_BIT       : 1;  /* bit 16 */
            uint32_t SMBUS_SLAVE_ADDR_VALID    : 1;  /* bit 17 */
            uint32_t SMBUS_SLAVE_ADDR_RESOLVED : 1;  /* bit 18 */
            uint32_t SMBUS_SUSPEND_STATUS      : 1;  /* bit 19 */
            uint32_t SMBUS_ALERT_STATUS        : 1;  /* bit 20 */
            uint32_t RSVD_IC_STATUS_2          : 11; /* bits 31:21 */
        } bits;
        uint32_t reg;
    };
};

/* IC_ENABLE_STATUS (0x9c) */
struct dw_ic_enable_status_t {
    union {
        struct {
            uint32_t IC_EN                   : 1;  /* bit 0 */
            uint32_t SLV_DISABLED_WHILE_BUSY : 1;  /* bit 1 */
            uint32_t SLV_RX_DATA_LOST        : 1;  /* bit 2 */
            uint32_t RSVD_IC_ENABLE_STATUS   : 29; /* bits 31:3 */
        } bits;
        uint32_t reg;
    };
};

/* IC_DATA_CMD (0x10) */
struct dw_ic_data_cmd_t {
    union {
        struct {
            uint32_t DAT              : 8;  /* bits 7:0  */
            uint32_t CMD              : 1;  /* bit  8    */
            uint32_t STOP             : 1;  /* bit  9    */
            uint32_t RESTART          : 1;  /* bit 10    */
            uint32_t FIRST_DATA_BYTE  : 1;  /* bit 11    */
            uint32_t RSVD_IC_DATA_CMD : 20; /* bits 31:12 */
        } bits;
        uint32_t reg;
    };
};

/* Simple 32‑bit wrapper for registers where individual fields are not used
 * in the current driver. Field names follow databook naming. */

/* IC_TAR (0x04) */
struct dw_ic_tar_t {
    union {
        struct {
            uint32_t IC_TAR             : 10; /* bits 9:0   */
            uint32_t GC_OR_START        : 1;  /* bit  10    */
            uint32_t SPECIAL            : 1;  /* bit  11    */
            uint32_t IC_10BITADDR_MASTER: 1;  /* bit  12    */
            uint32_t RSVD_IC_TAR        : 19; /* bits 31:13 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SAR (0x08) */
struct dw_ic_sar_t {
    union {
        struct {
            uint32_t IC_SAR      : 10; /* bits 9:0   */
            uint32_t RSVD_IC_SAR : 22; /* bits 31:10 */
        } bits;
        uint32_t reg;
    };
};

/* IC_HS_MADDR (0x0c) */
struct dw_ic_hs_maddr_t {
    union {
        struct {
            uint32_t IC_HS_MAR      : 3;  /* bits 2:0  */
            uint32_t RSVD_IC_HS_MAR : 29; /* bits 31:3 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SS_SCL_HCNT (0x14) */
struct dw_ic_ss_scl_hcnt_t {
    union {
        struct {
            uint32_t IC_SS_SCL_HCNT      : 16; /* bits 15:0  */
            uint32_t RSVD_IC_SS_SCL_HCNT : 16; /* bits 31:16 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SS_SCL_LCNT (0x18) */
struct dw_ic_ss_scl_lcnt_t {
    union {
        struct {
            uint32_t IC_SS_SCL_LCNT      : 16; /* bits 15:0  */
            uint32_t RSVD_IC_SS_SCL_LCNT : 16; /* bits 31:16 */
        } bits;
        uint32_t reg;
    };
};

/* IC_FS_SCL_HCNT (0x1c) */
struct dw_ic_fs_scl_hcnt_t {
    union {
        struct {
            uint32_t IC_FS_SCL_HCNT      : 16; /* bits 15:0  */
            uint32_t RSVD_IC_FS_SCL_HCNT : 16; /* bits 31:16 */
        } bits;
        uint32_t reg;
    };
};

/* IC_FS_SCL_LCNT (0x20) */
struct dw_ic_fs_scl_lcnt_t {
    union {
        struct {
            uint32_t IC_FS_SCL_LCNT      : 16; /* bits 15:0  */
            uint32_t RSVD_IC_FS_SCL_LCNT : 16; /* bits 31:16 */
        } bits;
        uint32_t reg;
    };
};

/* IC_HS_SCL_HCNT (0x24) */
struct dw_ic_hs_scl_hcnt_t {
    union {
        struct {
            uint32_t IC_HS_SCL_HCNT      : 16; /* bits 15:0  */
            uint32_t RSVD_IC_HS_SCL_HCNT : 16; /* bits 31:16 */
        } bits;
        uint32_t reg;
    };
};

/* IC_HS_SCL_LCNT (0x28) */
struct dw_ic_hs_scl_lcnt_t {
    union {
        struct {
            uint32_t IC_HS_SCL_LCNT      : 16; /* bits 15:0  */
            uint32_t RSVD_IC_HS_SCL_LCNT : 16; /* bits 31:16 */
        } bits;
        uint32_t reg;
    };
};

/* IC_INTR_STAT / IC_INTR_MASK / IC_RAW_INTR_STAT (0x2c/0x30/0x34) */
struct dw_ic_intr_t {
    union {
        struct {
            uint32_t RX_UNDER    : 1;  /* bit 0 */
            uint32_t RX_OVER     : 1;  /* bit 1 */
            uint32_t RX_FULL     : 1;  /* bit 2 */
            uint32_t TX_OVER     : 1;  /* bit 3 */
            uint32_t TX_EMPTY    : 1;  /* bit 4 */
            uint32_t RD_REQ      : 1;  /* bit 5 */
            uint32_t TX_ABRT     : 1;  /* bit 6 */
            uint32_t RX_DONE     : 1;  /* bit 7 */
            uint32_t ACTIVITY    : 1;  /* bit 8 */
            uint32_t STOP_DET    : 1;  /* bit 9 */
            uint32_t START_DET   : 1;  /* bit 10 */
            uint32_t GEN_CALL    : 1;  /* bit 11 */
            uint32_t RESTART_DET : 1;  /* bit 12 */
            uint32_t MST_ON_HOLD : 1;  /* bit 13 */
            uint32_t RSVD        : 18; /* bits 31:14 */
        } bits;
        uint32_t reg;
    };
};

/* IC_RX_TL (0x38) */
struct dw_ic_rx_tl_t {
    union {
        struct {
            uint32_t RX_TL         : 8;  /* bits 7:0   */
            uint32_t RSVD_IC_RX_TL : 24; /* bits 31:8  */
        } bits;
        uint32_t reg;
    };
};

/* IC_TX_TL (0x3c) */
struct dw_ic_tx_tl_t {
    union {
        struct {
            uint32_t TX_TL         : 8;  /* bits 7:0   */
            uint32_t RSVD_IC_TX_TL : 24; /* bits 31:8  */
        } bits;
        uint32_t reg;
    };
};

/* Clear interrupt registers (0x40 ~ 0x68) */
struct dw_ic_clr_intr_t {
    union {
        struct {
            uint32_t CLR_INTR         : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_INTR : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_rx_under_t {
    union {
        struct {
            uint32_t CLR_RX_UNDER             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_RX_UNDER     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_rx_over_t {
    union {
        struct {
            uint32_t CLR_RX_OVER             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_RX_OVER     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_tx_over_t {
    union {
        struct {
            uint32_t CLR_TX_OVER             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_TX_OVER     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_rd_req_t {
    union {
        struct {
            uint32_t CLR_RD_REQ             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_RD_REQ     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_tx_abrt_t {
    union {
        struct {
            uint32_t CLR_TX_ABRT             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_TX_ABRT     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_rx_done_t {
    union {
        struct {
            uint32_t CLR_RX_DONE             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_RX_DONE     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_activity_t {
    union {
        struct {
            uint32_t CLR_ACTIVITY             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_ACTIVITY     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_stop_det_t {
    union {
        struct {
            uint32_t CLR_STOP_DET             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_STOP_DET     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_start_det_t {
    union {
        struct {
            uint32_t CLR_START_DET             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_START_DET     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_clr_gen_call_t {
    union {
        struct {
            uint32_t CLR_GEN_CALL             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_GEN_CALL     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

/* IC_TXFLR (0x74) */
struct dw_ic_txflr_t {
    union {
        struct {
            uint32_t TXFLR      : 8;  /* bits 7:0   */
            uint32_t RSVD_TXFLR : 24; /* bits 31:8  */
        } bits;
        uint32_t reg;
    };
};

/* IC_RXFLR (0x78) */
struct dw_ic_rxflr_t {
    union {
        struct {
            uint32_t RXFLR      : 8;  /* bits 7:0   */
            uint32_t RSVD_RXFLR : 24; /* bits 31:8  */
        } bits;
        uint32_t reg;
    };
};

/* IC_SDA_HOLD (0x7c) */
struct dw_ic_sda_hold_t {
    union {
        struct {
            uint32_t IC_SDA_TX_HOLD   : 16; /* bits 15:0  */
            uint32_t IC_SDA_RX_HOLD   : 8;  /* bits 23:16 */
            uint32_t RSVD_IC_SDA_HOLD : 8;  /* bits 31:24 */
        } bits;
        uint32_t reg;
    };
};

/* IC_TX_ABRT_SOURCE (0x80)
 *
 * Abort source bits follow the controller databook encoding.
 * Only single-bit flags are modelled here; multi-bit fields are kept
 * reserved.
 */
struct dw_ic_tx_abrt_source_t {
    union {
        struct {
            uint32_t ABRT_7B_ADDR_NOACK       : 1;  /* bit 0  */
            uint32_t ABRT_10ADDR1_NOACK       : 1;  /* bit 1  */
            uint32_t ABRT_10ADDR2_NOACK       : 1;  /* bit 2  */
            uint32_t ABRT_TXDATA_NOACK        : 1;  /* bit 3  */
            uint32_t ABRT_GCALL_NOACK         : 1;  /* bit 4  */
            uint32_t ABRT_GCALL_READ          : 1;  /* bit 5  */
            uint32_t ABRT_HS_ACKDET           : 1;  /* bit 6  */
            uint32_t ABRT_SBYTE_ACKDET        : 1;  /* bit 7  */
            uint32_t ABRT_SBYTE_NORSTRT       : 1;  /* bit 8  */
            uint32_t ABRT_10B_RD_NORSTRT      : 1;  /* bit 9  */
            uint32_t ABRT_MASTER_DIS          : 1;  /* bit 10 */
            uint32_t ABRT_ARB_LOST            : 1;  /* bit 11 */
            uint32_t ABRT_SLVFLUSH_TXFIFO     : 1;  /* bit 12 */
            uint32_t ABRT_SLV_ARBLOST         : 1;  /* bit 13 */
            uint32_t ABRT_SLV_RD_INTX         : 1;  /* bit 14 */
            uint32_t RSVD_IC_TX_ABRT_SOURCE   : 17; /* bits 31:15 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SLV_DATA_NACK_ONLY (0x84) */
struct dw_ic_slv_data_nack_only_t {
    union {
        struct {
            uint32_t IC_SLV_DATA_NACK_ONLY   : 1;  /* bit 0      */
            uint32_t RSVD_IC_SLV_DATA_NACK_ONLY : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

/* IC_DMA_CR / IC_DMA_TDLR / IC_DMA_RDLR (0x88/0x8c/0x90) */
struct dw_ic_dma_cr_t {
    union {
        struct {
            uint32_t RDMAE               : 1;  /* bit 0: Receive DMA enable */
            uint32_t TDMAE               : 1;  /* bit 1: Transmit DMA enable */
            uint32_t RSVD_IC_DMA_CR_2_31 : 30; /* bits 31:2 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_dma_tdlr_t {
    union {
        struct {
            uint32_t DMATDL            : 8;  /* bits 7:0  */
            uint32_t RSVD_IC_DMA_TDLR : 24; /* bits 31:8 */
        } bits;
        uint32_t reg;
    };
};

struct dw_ic_dma_rdlr_t {
    union {
        struct {
            uint32_t DMARDL            : 8;  /* bits 7:0  */
            uint32_t RSVD_IC_DMA_RDLR : 24; /* bits 31:8 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SDA_SETUP (0x94) */
struct dw_ic_sda_setup_t {
    union {
        struct {
            uint32_t IC_SDA_SETUP      : 8;  /* bits 7:0  */
            uint32_t RSVD_IC_SDA_SETUP : 24; /* bits 31:8 */
        } bits;
        uint32_t reg;
    };
};

/* IC_ACK_GENERAL_CALL (0x98) */
struct dw_ic_ack_general_call_t {
    union {
        struct {
            uint32_t ACK_GEN_CALL              : 1;  /* bit 0      */
            uint32_t RSVD_IC_ACK_GENERAL_CALL : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

/* IC_FS_SPKLEN (0xa0) */
struct dw_ic_fs_spklen_t {
    union {
        struct {
            uint32_t IC_FS_SPKLEN      : 8;  /* bits 7:0  */
            uint32_t RSVD_IC_FS_SPKLEN : 24; /* bits 31:8 */
        } bits;
        uint32_t reg;
    };
};

/* IC_HS_SPKLEN (0xa4) */
struct dw_ic_hs_spklen_t {
    union {
        struct {
            uint32_t IC_HS_SPKLEN      : 8;  /* bits 7:0  */
            uint32_t RSVD_IC_HS_SPKLEN : 24; /* bits 31:8 */
        } bits;
        uint32_t reg;
    };
};

/* IC_CLR_RESTART_DET (0xa8) */
struct dw_ic_clr_restart_det_t {
    union {
        struct {
            uint32_t CLR_RESTART_DET             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_RESTART_DET     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SCL_STUCK_AT_LOW_TIMEOUT (0xac) */
struct dw_ic_scl_stuck_at_low_timeout_t {
    union {
        struct {
            uint32_t IC_SCL_STUCK_AT_LOW_TIMEOUT : 32;
        } bits;
        uint32_t reg;
    };
};

/* IC_SDA_STUCK_AT_LOW_TIMEOUT (0xb0) */
struct dw_ic_sda_stuck_at_low_timeout_t {
    union {
        struct {
            uint32_t IC_SDA_STUCK_AT_LOW_TIMEOUT : 32;
        } bits;
        uint32_t reg;
    };
};

/* IC_CLR_SCL_STUCK_DET (0xb4) */
struct dw_ic_clr_scl_stuck_det_t {
    union {
        struct {
            uint32_t CLR_SCL_STUCK_DET             : 1;  /* bit 0      */
            uint32_t RSVD_IC_CLR_SCL_STUCK_DET     : 31; /* bits 31:1 */
        } bits;
        uint32_t reg;
    };
};

/* IC_DEVICE_ID (0xb8) */
struct dw_ic_device_id_t {
    union {
        struct {
            uint32_t DEVICE_ID         : 24; /* bits 23:0  */
            uint32_t RSVD_IC_DEVICE_ID : 8;  /* bits 31:24 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SMBUS_CLK_LOW_SEXT (0xbc) */
struct dw_ic_smbus_clk_low_sext_t {
    union {
        struct {
            uint32_t SMBUS_CLK_LOW_SEXT_TIMEOUT : 32; /* bits 31:0 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SMBUS_CLK_LOW_MEXT (0xc0) */
struct dw_ic_smbus_clk_low_mext_t {
    union {
        struct {
            uint32_t SMBUS_CLK_LOW_MEXT_TIMEOUT : 32; /* bits 31:0 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SMBUS_THIGH_MAX_IDLE_COUNT (0xc4) */
struct dw_ic_smbus_thigh_max_idle_count_t {
    union {
        struct {
            uint32_t SMBUS_THIGH_MAX_BUS_IDLE_CNT      : 16; /* bits 15:0  */
            uint32_t RSVD_SMBUS_THIGH_MAX_BUS_IDLE_CNT : 16; /* bits 31:16 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SMBUS_INTR_STAT (0xc8) */
struct dw_ic_smbus_intr_stat_t {
    union {
        struct {
            uint32_t R_SLV_CLOCK_EXTND_TIMEOUT  : 1;  /* bit 0  */
            uint32_t R_MST_CLOCK_EXTND_TIMEOUT  : 1;  /* bit 1  */
            uint32_t R_QUICK_CMD_DET            : 1;  /* bit 2  */
            uint32_t R_HOST_NOTIFY_MST_DET      : 1;  /* bit 3  */
            uint32_t R_ARP_PREPARE_CMD_DET      : 1;  /* bit 4  */
            uint32_t R_ARP_RST_CMD_DET          : 1;  /* bit 5  */
            uint32_t R_ARP_GET_UDID_CMD_DET     : 1;  /* bit 6  */
            uint32_t R_ARP_ASSGN_ADDR_CMD_DET   : 1;  /* bit 7  */
            uint32_t R_SLV_RX_PEC_NACK          : 1;  /* bit 8  */
            uint32_t R_SMBUS_SUSPEND_DET        : 1;  /* bit 9  */
            uint32_t R_SMBUS_ALERT_DET          : 1;  /* bit 10 */
            uint32_t RSVD_IC_SMBUS_INTR_STAT    : 21; /* bits 31:11 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SMBUS_INTR_MASK (0xcc) */
struct dw_ic_smbus_intr_mask_t {
    union {
        struct {
            uint32_t M_SLV_CLOCK_EXTND_TIMEOUT  : 1;  /* bit 0  */
            uint32_t M_MST_CLOCK_EXTND_TIMEOUT  : 1;  /* bit 1  */
            uint32_t M_QUICK_CMD_DET            : 1;  /* bit 2  */
            uint32_t M_HOST_NOTIFY_MST_DET      : 1;  /* bit 3  */
            uint32_t M_ARP_PREPARE_CMD_DET      : 1;  /* bit 4  */
            uint32_t M_ARP_RST_CMD_DET          : 1;  /* bit 5  */
            uint32_t M_ARP_GET_UDID_CMD_DET     : 1;  /* bit 6  */
            uint32_t M_ARP_ASSGN_ADDR_CMD_DET   : 1;  /* bit 7  */
            uint32_t M_SLV_RX_PEC_NACK          : 1;  /* bit 8  */
            uint32_t M_SMBUS_SUSPEND_DET        : 1;  /* bit 9  */
            uint32_t M_SMBUS_ALERT_DET          : 1;  /* bit 10 */
            uint32_t RSVD_IC_SMBUS_INTR_MASK    : 21; /* bits 31:11 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SMBUS_RAW_INTR_STAT (0xd0) */
struct dw_ic_smbus_raw_intr_stat_t {
    union {
        struct {
            uint32_t SLV_CLOCK_EXTND_TIMEOUT      : 1;  /* bit 0  */
            uint32_t MST_CLOCK_EXTND_TIMEOUT      : 1;  /* bit 1  */
            uint32_t QUICK_CMD_DET                : 1;  /* bit 2  */
            uint32_t HOST_NOTIFY_MST_DET          : 1;  /* bit 3  */
            uint32_t ARP_PREPARE_CMD_DET          : 1;  /* bit 4  */
            uint32_t ARP_RST_CMD_DET              : 1;  /* bit 5  */
            uint32_t ARP_GET_UDID_CMD_DET         : 1;  /* bit 6  */
            uint32_t ARP_ASSGN_ADDR_CMD_DET       : 1;  /* bit 7  */
            uint32_t SLV_RX_PEC_NACK              : 1;  /* bit 8  */
            uint32_t SMBUS_SUSPEND_DET            : 1;  /* bit 9  */
            uint32_t SMBUS_ALERT_DET              : 1;  /* bit 10 */
            uint32_t RSVD_IC_SMBUS_RAW_INTR_STAT  : 21; /* bits 31:11 */
        } bits;
        uint32_t reg;
    };
};

/* IC_CLR_SMBUS_INTR (0xd4) */
struct dw_ic_clr_smbus_intr_t {
    union {
        struct {
            uint32_t CLR_SLV_CLOCK_EXTND_TIMEOUT  : 1;  /* bit 0  */
            uint32_t CLR_MST_CLOCK_EXTND_TIMEOUT  : 1;  /* bit 1  */
            uint32_t CLR_QUICK_CMD_DET            : 1;  /* bit 2  */
            uint32_t CLR_HOST_NOTIFY_MST_DET      : 1;  /* bit 3  */
            uint32_t CLR_ARP_PREPARE_CMD_DET      : 1;  /* bit 4  */
            uint32_t CLR_ARP_RST_CMD_DET          : 1;  /* bit 5  */
            uint32_t CLR_ARP_GET_UDID_CMD_DET     : 1;  /* bit 6  */
            uint32_t CLR_ARP_ASSGN_ADDR_CMD_DET   : 1;  /* bit 7  */
            uint32_t CLR_SLV_RX_PEC_NACK          : 1;  /* bit 8  */
            uint32_t CLR_SMBUS_SUSPEND_DET        : 1;  /* bit 9  */
            uint32_t CLR_SMBUS_ALERT_DET          : 1;  /* bit 10 */
            uint32_t RSVD_IC_CLR_SMBUS_INTR       : 21; /* bits 31:11 */
        } bits;
        uint32_t reg;
    };
};

/* IC_OPTIONAL_SAR (0xd8) */
struct dw_ic_optional_sar_t {
    union {
        struct {
            uint32_t OPTIONAL_SAR          : 7;  /* bits 6:0  */
            uint32_t RSVD_IC_OPTIONAL_SAR  : 25; /* bits 31:7 */
        } bits;
        uint32_t reg;
    };
};

/* IC_SMBUS_UDID_LSB / WORD1 / WORD2 / WORD3 (0xdc / 0xe0 / 0xe4 / 0xe8) */
struct dw_ic_smbus_udid_lsb_t {
    union {
        struct {
            uint32_t IC_SMBUS_UDID_LSB : 32;
        } bits;
        uint32_t reg;
    };
};

/* Reserved space for non-existent registers 0xe0 - 0xf0.
 * These locations are not implemented by this DW_apb_i2c instance.
 */
struct dw_ic_reserve_t {
    union {
        struct {
            uint32_t RSVD_IC_SPACE : 32;
        } bits;
        uint32_t reg;
    };
};

/* IC_COMP_PARAM_1 (0xf4) */
struct dw_ic_comp_param_1_t {
    union {
        struct {
            uint32_t APB_DATA_WIDTH       : 2;  /* bits 1:0  */
            uint32_t MAX_SPEED_MODE       : 2;  /* bits 3:2  */
            uint32_t HC_COUNT_VALUES      : 1;  /* bit  4    */
            uint32_t INTR_IO              : 1;  /* bit  5    */
            uint32_t HAS_DMA              : 1;  /* bit  6    */
            uint32_t ADD_ENCODED_PARAMS   : 1;  /* bit  7    */
            uint32_t RX_BUFFER_DEPTH      : 8;  /* bits 15:8 */
            uint32_t TX_BUFFER_DEPTH      : 8;  /* bits 23:16 */
            uint32_t RSVD_IC_COMP_PARAM_1 : 8;  /* bits 31:24 */
        } bits;
        uint32_t reg;
    };
};

/* IC_COMP_VERSION (0xf8) */
struct dw_ic_comp_version_t {
    union {
        struct {
            uint32_t IC_COMP_VERSION : 32;
        } bits;
        uint32_t reg;
    };
};

/* IC_COMP_TYPE (0xfc) */
struct dw_ic_comp_type_t {
    union {
        struct {
            uint32_t IC_COMP_TYPE : 32;
        } bits;
        uint32_t reg;
    };
};
