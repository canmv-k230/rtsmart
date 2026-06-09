/*
 * Private register definitions for the K230 Synopsys DesignWare SSI blocks.
 */

#ifndef DRV_SPI_HW_H__
#define DRV_SPI_HW_H__

#include <stdint.h>

#define DW_SSI_HAS_DMA             2
#define DW_SSI_AXI_BLW             8
#define DW_SSI_TX_FIFO_DEPTH       256
#define DW_SSI_RX_FIFO_DEPTH       256
#define DW_SSI_MAX_XFER_FRAMES     0x10000
#define DW_SSI_DMA_BOUNCE_MAX_SIZE (DW_SSI_MAX_XFER_FRAMES * sizeof(uint32_t))

/* Refill IRQ when TX level <= 1/4 depth so each TXE IRQ refills ~3/4 of the FIFO. */
#define DW_SSI_TX_FIFO_THRESH      (DW_SSI_TX_FIFO_DEPTH / 4)
/* Drain IRQ when RX level > 3/4 depth so each RXF IRQ drains ~3/4 of the FIFO. */
#define DW_SSI_RX_FIFO_THRESH      ((DW_SSI_RX_FIFO_DEPTH * 3) / 4 - 1)

#define DW_SSI_IRQ_BANK_STRIDE     9
#define DW_SSI_IRQ_TXE             0
#define DW_SSI_IRQ_TXO             1
#define DW_SSI_IRQ_RXF             2
#define DW_SSI_IRQ_RXO             3
#define DW_SSI_IRQ_TXU             4
#define DW_SSI_IRQ_RXU             5
#define DW_SSI_IRQ_MST             6
#define DW_SSI_IRQ_DONE            7
#define DW_SSI_IRQ_AXIE            8

#define DW_SSI_IMR_TXE             (1U << 0)
#define DW_SSI_IMR_TXO             (1U << 1)
#define DW_SSI_IMR_RXU             (1U << 2)
#define DW_SSI_IMR_RXO             (1U << 3)
#define DW_SSI_IMR_RXF             (1U << 4)
#define DW_SSI_IMR_MST             (1U << 5)
#define DW_SSI_IMR_XRXO            (1U << 6)
#define DW_SSI_IMR_TXU             (1U << 7)
#define DW_SSI_IMR_AXIE            (1U << 8)
#define DW_SSI_IMR_DONE            (1U << 11)

#define DW_SSI_SR_BUSY             (1U << 0)
#define DW_SSI_SR_TFNF             (1U << 1)
#define DW_SSI_SR_TFE              (1U << 2)
#define DW_SSI_SR_RFNE             (1U << 3)
#define DW_SSI_SR_TX_DONE_MASK     (DW_SSI_SR_BUSY | DW_SSI_SR_TFE)
#define DW_SSI_SR_TX_DONE_VALUE    (DW_SSI_SR_TFE)

#define DW_SSI_DMACR_IDMAE         (1U << 2)
#define DW_SSI_DMACR_ATW_SHIFT     3
#define DW_SSI_DMACR_ADW_SHIFT     5

#define DW_SSI_CTRLR0_TMOD_SHIFT   10
#define DW_SSI_CTRLR0_SPI_FRF_SHIFT 22
#define DW_SSI_CTRLR0_TMOD_MASK    (3U << DW_SSI_CTRLR0_TMOD_SHIFT)
#define DW_SSI_CTRLR0_SPI_FRF_MASK (3U << DW_SSI_CTRLR0_SPI_FRF_SHIFT)

#define DW_SSI_SPI_CTRLR0_TRANS_TYPE_SHIFT 0
#define DW_SSI_SPI_CTRLR0_ADDR_L_SHIFT     2
#define DW_SSI_SPI_CTRLR0_INST_L_SHIFT     8
#define DW_SSI_SPI_CTRLR0_WAIT_CYCLES_SHIFT 11
#define DW_SSI_SPI_CTRLR0_CLK_STRETCH_EN   (1U << 30)
#define DW_SSI_SPI_CTRLR0_ADDR_L(size)     ((size) >> 2 << DW_SSI_SPI_CTRLR0_ADDR_L_SHIFT)
#define DW_SSI_SPI_CTRLR0_INST_L(size)     (((size) ? __builtin_ffs(size) - 2 : 0) << DW_SSI_SPI_CTRLR0_INST_L_SHIFT)
#define DW_SSI_SPI_CTRLR0_WAIT_CYCLES(cycles) ((cycles) << DW_SSI_SPI_CTRLR0_WAIT_CYCLES_SHIFT)

#define DW_SPI_FRF_STD_SPI         0
#define DW_SPI_FRF_DUAL_SPI        1
#define DW_SPI_FRF_QUAD_SPI        2
#define DW_SPI_FRF_OCT_SPI         3

#define DW_SPI_TMOD_TR             0
#define DW_SPI_TMOD_TO             1
#define DW_SPI_TMOD_RO             2
#define DW_SPI_TMOD_EPROMREAD      3

/* Compatibility aliases while the driver is split in small steps. */
#define SSIC_HAS_DMA               DW_SSI_HAS_DMA
#define SSIC_AXI_BLW               DW_SSI_AXI_BLW
#define SSIC_TX_ABW                DW_SSI_TX_FIFO_DEPTH
#define SSIC_RX_ABW                DW_SSI_RX_FIFO_DEPTH

#define SSI_TXE                    DW_SSI_IRQ_TXE
#define SSI_TXO                    DW_SSI_IRQ_TXO
#define SSI_RXF                    DW_SSI_IRQ_RXF
#define SSI_RXO                    DW_SSI_IRQ_RXO
#define SSI_TXU                    DW_SSI_IRQ_TXU
#define SSI_RXU                    DW_SSI_IRQ_RXU
#define SSI_MST                    DW_SSI_IRQ_MST
#define SSI_DONE                   DW_SSI_IRQ_DONE
#define SSI_AXIE                   DW_SSI_IRQ_AXIE

#define SPI_FRF_STD_SPI            DW_SPI_FRF_STD_SPI
#define SPI_FRF_DUAL_SPI           DW_SPI_FRF_DUAL_SPI
#define SPI_FRF_QUAD_SPI           DW_SPI_FRF_QUAD_SPI
#define SPI_FRF_OCT_SPI            DW_SPI_FRF_OCT_SPI

#define SPI_TMOD_TR                DW_SPI_TMOD_TR
#define SPI_TMOD_TO                DW_SPI_TMOD_TO
#define SPI_TMOD_RO                DW_SPI_TMOD_RO
#define SPI_TMOD_EPROMREAD         DW_SPI_TMOD_EPROMREAD

typedef struct {
    volatile uint32_t ctrlr0;
    volatile uint32_t ctrlr1;
    volatile uint32_t ssienr;
    volatile uint32_t mwcr;
    volatile uint32_t ser;
    volatile uint32_t baudr;
    volatile uint32_t txftlr;
    volatile uint32_t rxftlr;
    volatile uint32_t txflr;
    volatile uint32_t rxflr;
    volatile uint32_t sr;
    volatile uint32_t imr;
    volatile uint32_t isr;
    volatile uint32_t risr;
    volatile uint32_t txeicr;
    volatile uint32_t rxoicr;
    volatile uint32_t rxuicr;
    volatile uint32_t msticr;
    volatile uint32_t icr;
    volatile uint32_t dmacr;
#if DW_SSI_HAS_DMA == 1
    volatile uint32_t dmatdlr;
    volatile uint32_t dmardlr;
#elif DW_SSI_HAS_DMA == 2
    volatile uint32_t axiawlen;
    volatile uint32_t axiarlen;
#else
    uint32_t resv0[2];
#endif
    volatile const uint32_t idr;
    volatile const uint32_t ssic_version_id;
    volatile uint32_t dr[36];
    volatile uint32_t rx_sample_delay;
    volatile uint32_t spi_ctrlr0;
    volatile uint32_t ddr_drive_edge;
    volatile uint32_t xip_mode_bits;
    volatile uint32_t xip_incr_inst;
    volatile uint32_t xip_wrap_inst;
#if SSIC_CONCURRENT_XIP_EN
    volatile uint32_t xip_ctrl;
    volatile uint32_t xip_ser;
    volatile uint32_t xrxoicr;
    volatile uint32_t xip_cnt_time_out;
    uint32_t resv1[1];
    volatile uint32_t spitecr;
#else
    uint32_t resv1[6];
#endif
#if DW_SSI_HAS_DMA == 2
    volatile uint32_t spidr;
    volatile uint32_t spiar;
    volatile uint32_t axiar0;
    volatile uint32_t axiar1;
    volatile uint32_t axiecr;
    volatile uint32_t donecr;
#endif
    uint32_t resv3[2];
#if SSIC_XIP_WRITE_REG_EN
    volatile uint32_t xip_write_incr_inst;
    volatile uint32_t xip_write_wrap_inst;
    volatile uint32_t xip_write_ctrl;
#else
    uint32_t resv4[3];
#endif
} __attribute__((packed, aligned(4))) dw_spi_reg_t;

#endif
