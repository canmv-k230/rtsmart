/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2017-10-10     Tanek        first version
 * 2021-07-07     linzhenxing  add sd card drivers in mmu
 * 2021-07-14     linzhenxing  add emmc
 */

#include <rtthread.h>
#include <rthw.h>
#include <drivers/mmcsd_core.h>
#include <drivers/sdio.h>

#include "board.h"
#include "drv_sdhci.h"
#include "riscv_io.h"
#include <string.h>
#include <ioremap.h>
#include <cache.h>

#include "tick.h"

#ifdef RT_USING_SDIO

#define DBG_TAG "drv_sdhci"
#ifdef RT_SDIO_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_INFO
#endif /* RT_SDIO_DEBUG */
#include <rtdbg.h>

#if defined(RT_USING_SDIO0) || defined(RT_USING_SDIO1)

#define SDHCI_SDMA_ENABLE
#define CACHE_LINESIZE (64)
/* SDHCI requires the SDMA system address to be word aligned. */
#define SDHCI_SDMA_ALIGNMENT (4)
#define SDHCI_SMALL_BOUNCE_SIZE 512U

#define BIT(x) (1 << x)
#define DWC_MSHC_PTR_VENDOR1 0x500
#define MSHC_CTRL_R (DWC_MSHC_PTR_VENDOR1 + 0x08)
#define EMMC_CTRL_R (DWC_MSHC_PTR_VENDOR1 + 0x2c)
#define SDHCI_VENDER_AT_CTRL_REG (DWC_MSHC_PTR_VENDOR1 + 0x40)
#define SDHCI_VENDER_AT_STAT_REG (DWC_MSHC_PTR_VENDOR1 + 0x44)
#define SDHCI_TUNE_AT_EN BIT(0)
#define SDHCI_TUNE_CI_SEL BIT(1)
#define SDHCI_TUNE_SWIN_TH_EN BIT(2)
#define SDHCI_TUNE_RPT_TUNE_ERR BIT(3)
#define SDHCI_TUNE_SW_TUNE_EN BIT(4)
#define SDHCI_TUNE_WIN_EDGE_SEL_MASK (0xf << 8)
#define SDHCI_TUNE_CLK_STOP_EN_MASK BIT(16)
#define SDHCI_TUNE_PRE_CHANGE_DLY_LSB (17)
#define SDHCI_TUNE_PRE_CHANGE_DLY_MASK (0x3 << SDHCI_TUNE_PRE_CHANGE_DLY_LSB)
#define SDHCI_TUNE_POST_CHANGE_DLY_LSB (19)
#define SDHCI_TUNE_POST_CHANGE_DLY_MASK (0x3 << SDHCI_TUNE_POST_CHANGE_DLY_LSB)
#define SDHCI_TUNE_SWIN_TH_VAL_LSB (24)
#define SDHCI_TUNE_SWIN_TH_VAL_MASK (0xff << SDHCI_TUNE_SWIN_TH_VAL_LSB)
#define SDHCI_TUNE_PRE_CHANGE_DLY_VAL (0x1)
#define SDHCI_TUNE_POST_CHANGE_DLY_VAL (0x3)
#define SDHCI_TUNE_SWIN_TH_VAL (0x9)
#define SDHCI_TUNING_LOOP_COUNT 128
#define SDHCI_TUNING_TIMEOUT_MS 50
#define SDHCI_TUNING_TOTAL_TIMEOUT_MS 2000
#define SDHCI_COMMAND_TIMEOUT_MS 1000
#define SDHCI0_BASE_CLOCK 200000000U
#define SDHCI1_BASE_CLOCK 100000000U
#define SDHCI_CARD_MIN_CLOCK 400000U
#define SDHCI0_CARD_MAX_CLOCK 200000000U
#define SDHCI1_CARD_MAX_CLOCK 50000000U

#define DWC_MSHC_MAX_DELAY 255U
#define CARD_IS_EMMC 0
#define EMMC_RST_N 2
#define EMMC_RST_N_OE 3

#define DWC_MSHC_PTR_PHY_REGS 0x300
#define DWC_MSHC_PHY_CNFG (DWC_MSHC_PTR_PHY_REGS + 0x0)
#define PAD_SN_LSB 20
#define PAD_SN_MASK 0xF
#define PAD_SN_DEFAULT ((0x8 & PAD_SN_MASK) << PAD_SN_LSB)
#define PAD_SP_LSB 16
#define PAD_SP_MASK 0xF
#define PAD_SP_DEFAULT ((0x9 & PAD_SP_MASK) << PAD_SP_LSB)
#define PHY_PWRGOOD BIT(1)
#define PHY_RSTN BIT(0)

#define DWC_MSHC_CMDPAD_CNFG (DWC_MSHC_PTR_PHY_REGS + 0x4)
#define DWC_MSHC_DATPAD_CNFG (DWC_MSHC_PTR_PHY_REGS + 0x6)
#define DWC_MSHC_CLKPAD_CNFG (DWC_MSHC_PTR_PHY_REGS + 0x8)
#define DWC_MSHC_STBPAD_CNFG (DWC_MSHC_PTR_PHY_REGS + 0xA)
#define DWC_MSHC_RSTNPAD_CNFG (DWC_MSHC_PTR_PHY_REGS + 0xC)
#define TXSLEW_N_LSB 9
#define TXSLEW_N_MASK 0xF
#define TXSLEW_P_LSB 5
#define TXSLEW_P_MASK 0xF
#define WEAKPULL_EN_LSB 3
#define WEAKPULL_EN_MASK 0x3
#define RXSEL_LSB 0
#define RXSEL_MASK 0x3

#define DWC_MSHC_COMMDL_CNFG (DWC_MSHC_PTR_PHY_REGS + 0x1C)
#define DWC_MSHC_SDCLKDL_CNFG (DWC_MSHC_PTR_PHY_REGS + 0x1D)
#define DWC_MSHC_SDCLKDL_DC (DWC_MSHC_PTR_PHY_REGS + 0x1E)
#define DWC_MSHC_SMPLDL_CNFG (DWC_MSHC_PTR_PHY_REGS + 0x20)
#define DWC_MSHC_ATDL_CNFG (DWC_MSHC_PTR_PHY_REGS + 0x21)

#define DWC_MSHC_PHY_PAD_SD_CLK \
    ((1 << TXSLEW_N_LSB) | (3 << TXSLEW_P_LSB) | (1 << WEAKPULL_EN_LSB) | (2 << RXSEL_LSB))
#define DWC_MSHC_PHY_PAD_SD_DAT \
    ((1 << TXSLEW_N_LSB) | (3 << TXSLEW_P_LSB) | (1 << WEAKPULL_EN_LSB) | (2 << RXSEL_LSB))
#define DWC_MSHC_PHY_PAD_SD_STB \
    ((1 << TXSLEW_N_LSB) | (3 << TXSLEW_P_LSB) | (2 << WEAKPULL_EN_LSB) | (2 << RXSEL_LSB))
#define DWC_MSHC_PHY_PAD_EMMC_CLK \
    ((2 << TXSLEW_N_LSB) | (2 << TXSLEW_P_LSB) | (1 << WEAKPULL_EN_LSB) | (1 << RXSEL_LSB))
#define DWC_MSHC_PHY_PAD_EMMC_DAT \
    ((2 << TXSLEW_N_LSB) | (2 << TXSLEW_P_LSB) | (1 << WEAKPULL_EN_LSB) | (1 << RXSEL_LSB))
#define DWC_MSHC_PHY_PAD_EMMC_STB \
    ((2 << TXSLEW_N_LSB) | (2 << TXSLEW_P_LSB) | (2 << WEAKPULL_EN_LSB) | (1 << RXSEL_LSB))

static struct sdhci_host* sdhci_host0;
static struct sdhci_host* sdhci_host1;

#if defined(SDIO0_BUS_WIDTH_8BIT)
#define SDIO0_BUS_WIDTH_FLAGS (MMCSD_BUSWIDTH_4 | MMCSD_BUSWIDTH_8)
#else
#define SDIO0_BUS_WIDTH_FLAGS MMCSD_BUSWIDTH_4
#endif

static inline void sdhci_writel(struct sdhci_host* host, uint32_t val, int reg)
{
    writel(val, (void*)host->mapbase + reg);
}

static inline void sdhci_writew(struct sdhci_host* host, uint16_t val, int reg)
{
    writew((uint16_t)val, (void*)host->mapbase + reg);
}

static inline void sdhci_writeb(struct sdhci_host* host, uint8_t val, int reg)
{
    writeb((uint8_t)val, (void*)host->mapbase + reg);
}

static inline uint32_t sdhci_readl(struct sdhci_host* host, int reg)
{
    return (uint32_t)readl((void*)host->mapbase + reg);
}

static inline uint16_t sdhci_readw(struct sdhci_host* host, int reg)
{
    return (uint16_t)readw((void*)host->mapbase + reg);
}

static inline uint8_t sdhci_readb(struct sdhci_host* host, int reg)
{
    return (uint8_t)readb((void*)host->mapbase + reg);
}

static void emmc_reg_display(struct sdhci_host* host)
{
    rt_kprintf("SD_MASA_R:%x\n", sdhci_readl(host, SDHCI_DMA_ADDRESS));
    rt_kprintf("BLCOKSIZE_R:%x\n", sdhci_readw(host, SDHCI_BLOCK_SIZE));
    rt_kprintf("BLOCKCOUNT_R:%x\n", sdhci_readw(host, SDHCI_BLOCK_COUNT));
    rt_kprintf("ARGUMENT_R:%x\n", sdhci_readl(host, SDHCI_ARGUMENT));
    rt_kprintf("XFER_MODE_R:%x\n", sdhci_readw(host, SDHCI_TRANSFER_MODE));
    rt_kprintf("CMD_R:%x\n", sdhci_readw(host, SDHCI_COMMAND));
    rt_kprintf("RESP0_R:%x\n", sdhci_readl(host, SDHCI_RESPONSE));
    rt_kprintf("RESP1_R:%x\n", sdhci_readl(host, SDHCI_RESPONSE + 4));
    rt_kprintf("RESP2_R:%x\n", sdhci_readl(host, SDHCI_RESPONSE + 8));
    rt_kprintf("RESP3_R:%x\n", sdhci_readl(host, SDHCI_RESPONSE + 12));
    rt_kprintf("BUF_DATA_R:%x\n", sdhci_readl(host, SDHCI_BUFFER));
    rt_kprintf("PSTATE_REG_R:%x\n", sdhci_readl(host, SDHCI_PRESENT_STATE));
    rt_kprintf("HOST_CTL_R:%x\n", sdhci_readb(host, SDHCI_HOST_CONTROL));
    rt_kprintf("PWR_CTRL_R:%x\n", sdhci_readb(host, SDHCI_POWER_CONTROL));
    rt_kprintf("BGAP_CTRL_R:%x\n", sdhci_readb(host, SDHCI_BLOCK_GAP_CONTROL));
    rt_kprintf("WUP_CTRL_R:%x\n", sdhci_readb(host, SDHCI_WAKE_UP_CONTROL));
    rt_kprintf("CLK_CTRL_R:%x\n", sdhci_readw(host, SDHCI_CLOCK_CONTROL));
    rt_kprintf("TOUT_CTRL_R:%x\n", sdhci_readb(host, SDHCI_TIMEOUT_CONTROL));
    rt_kprintf("SW_RSR_R:%x\n", sdhci_readb(host, SDHCI_SOFTWARE_RESET));
    rt_kprintf("NORMAL_INT_STAT_R:%x\n", sdhci_readw(host, SDHCI_INT_STATUS));
    rt_kprintf("ERROR_INT_STAT_R:%x\n", sdhci_readw(host, SDHCI_INT_STATUS + 2));
    rt_kprintf("NORMAL_INT_STAT_EN_R:%x\n", sdhci_readw(host, SDHCI_INT_ENABLE));
    rt_kprintf("ERROR_INT_STAT_EN_R:%x\n", sdhci_readw(host, SDHCI_INT_ENABLE + 2));
    rt_kprintf("NORNAL_INT_SIGNAL_EN_R:%x\n", sdhci_readw(host, SDHCI_SIGNAL_ENABLE));
    rt_kprintf("ERROR_INT_SIGNAL_EN_R:%x\n", sdhci_readw(host, SDHCI_SIGNAL_ENABLE + 2));
    rt_kprintf("AUTO_CMD_STAT_R:%x\n", sdhci_readw(host, SDHCI_AUTO_CMD_STATUS));
    rt_kprintf("HOST_CTRL2_R:%x\n", sdhci_readw(host, SDHCI_HOST_CONTROL2));
    rt_kprintf("CAPABILITIES1_R:%x\n", sdhci_readl(host, SDHCI_CAPABILITIES));
    rt_kprintf("CAPABILITIES2_R:%x\n", sdhci_readl(host, SDHCI_CAPABILITIES_1));
    rt_kprintf("FORCE_AUTO_CMD_STAT_R:%x\n", sdhci_readw(host, SDHCI_MAX_CURRENT));
    rt_kprintf("FORCE_ERROR_INT_STAT_R:%x\n", sdhci_readw(host, SDHCI_SET_ACMD12_ERROR));
    rt_kprintf("AMDA_ERR_STAT_STAT_R:%x\n", sdhci_readl(host, SDHCI_ADMA_ERROR));
    rt_kprintf("AMDA_SA_LOW_STAT_R:%x\n", sdhci_readl(host, SDHCI_ADMA_ADDRESS));
    rt_kprintf("AMDA_SA_HIGH_STAT_R:%x\n", sdhci_readl(host, SDHCI_ADMA_ADDRESS_HI));
}

static inline void delay_1k(unsigned int uicnt)
{
    int i, j;

    for (i = 0; i < uicnt; i++)
        for (j = 0; j < 1000; j++)
            asm("nop");
}

static void dwcmshc_phy_1_8v_init(struct sdhci_host* host)
{
    sdhci_writew(host, DWC_MSHC_PHY_PAD_EMMC_DAT, DWC_MSHC_CMDPAD_CNFG);
    sdhci_writew(host, DWC_MSHC_PHY_PAD_EMMC_DAT, DWC_MSHC_DATPAD_CNFG);
    sdhci_writew(host, DWC_MSHC_PHY_PAD_EMMC_CLK, DWC_MSHC_CLKPAD_CNFG);
    sdhci_writew(host, DWC_MSHC_PHY_PAD_EMMC_STB, DWC_MSHC_STBPAD_CNFG);
    sdhci_writew(host, DWC_MSHC_PHY_PAD_EMMC_DAT, DWC_MSHC_RSTNPAD_CNFG);
}

static void dwcmshc_phy_3_3v_init(struct sdhci_host* host)
{
    sdhci_writew(host, DWC_MSHC_PHY_PAD_SD_DAT, DWC_MSHC_CMDPAD_CNFG);
    sdhci_writew(host, DWC_MSHC_PHY_PAD_SD_DAT, DWC_MSHC_DATPAD_CNFG);
    sdhci_writew(host, DWC_MSHC_PHY_PAD_SD_CLK, DWC_MSHC_CLKPAD_CNFG);
    sdhci_writew(host, DWC_MSHC_PHY_PAD_SD_STB, DWC_MSHC_STBPAD_CNFG);
    sdhci_writew(host, DWC_MSHC_PHY_PAD_SD_DAT, DWC_MSHC_RSTNPAD_CNFG);
}

static rt_err_t dwcmshc_set_tx_delay(struct sdhci_host* host,
                                     rt_uint32_t delay)
{
    uint16_t clk;
    uint8_t sdclkdl_cnfg;
    uint8_t sdclkdl_dc;

    if (delay > DWC_MSHC_MAX_DELAY) {
        LOG_E("host%d invalid tx delay %u", host->index, delay);
        return -RT_EINVAL;
    }

    clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
    sdhci_writew(host, clk & ~SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);

    if (delay >= 128U) {
        sdclkdl_cnfg = 0x1;
        sdclkdl_dc = delay - 128U;
    } else {
        sdclkdl_cnfg = 0x0;
        sdclkdl_dc = delay;
    }

    sdhci_writeb(host, sdclkdl_cnfg | BIT(4), DWC_MSHC_SDCLKDL_CNFG);
    sdhci_writeb(host, sdclkdl_dc & 0x7f, DWC_MSHC_SDCLKDL_DC);
    sdhci_writeb(host, sdclkdl_cnfg, DWC_MSHC_SDCLKDL_CNFG);
    sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
    host->tx_delay_line = delay;
    cpu_ticks_delay_us(1);

    return RT_EOK;
}

static void dwcmshc_phy_delay_config(struct sdhci_host* host)
{
    sdhci_writeb(host, 1, DWC_MSHC_COMMDL_CNFG);
    (void)dwcmshc_set_tx_delay(host, host->tx_delay_line);
    sdhci_writeb(host, host->rx_delay_line, DWC_MSHC_SMPLDL_CNFG);
}

static int dwcmshc_phy_init(struct sdhci_host* host)
{
    uint32_t reg;
    uint32_t timeout = 15000;
    /* reset phy */
    sdhci_writew(host, 0, DWC_MSHC_PHY_CNFG);

    /* Disable the clock */
    sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

    if (host->io_fixed_1v8) {
        uint32_t data = sdhci_readw(host, SDHCI_HOST_CONTROL2);
        data |= SDHCI_CTRL_VDD_180;
        sdhci_writew(host, data, SDHCI_HOST_CONTROL2);
        dwcmshc_phy_1_8v_init(host);
    } else {
        dwcmshc_phy_3_3v_init(host);
    }

    dwcmshc_phy_delay_config(host);

    /* Wait max 150 ms */
    while (1) {
        reg = sdhci_readl(host, DWC_MSHC_PHY_CNFG);
        if (reg & PHY_PWRGOOD)
            break;
        if (!timeout) {
            return -1;
        }
        timeout--;

        delay_1k(1);
    }

    reg = PAD_SN_DEFAULT | PAD_SP_DEFAULT;
    sdhci_writel(host, reg, DWC_MSHC_PHY_CNFG);

    /* de-assert the phy */
    reg |= PHY_RSTN;
    sdhci_writel(host, reg, DWC_MSHC_PHY_CNFG);

    return 0;
}

static void sdhci_reset(struct sdhci_host* host, uint8_t mask)
{
    unsigned long timeout;

    /* Wait max 100 ms */
    timeout = 100;
    sdhci_writeb(host, mask, SDHCI_SOFTWARE_RESET);
    while (sdhci_readb(host, SDHCI_SOFTWARE_RESET) & mask) {
        if (timeout == 0) {
            LOG_E("%s: Reset 0x%x never completed.\n",
                __func__, (int)mask);
            return;
        }
        timeout--;
        delay_1k(1);
    }
    if (mask == SDHCI_RESET_ALL) {
        if (host->index == 0) {
            uint16_t emmc_ctl = sdhci_readw(host, EMMC_CTRL_R);
            if (host->is_emmc_card)
                emmc_ctl |= (1 << CARD_IS_EMMC);
            else
                emmc_ctl &= ~(1 << CARD_IS_EMMC);
            sdhci_writeb(host, emmc_ctl, EMMC_CTRL_R);
        }
        if (host->have_phy)
            dwcmshc_phy_init(host);
        else
            sdhci_writeb(host, host->mshc_ctrl_r, MSHC_CTRL_R);
    }
}

static uint32_t sdhci_get_present_status_flag(struct sdhci_host* sdhci_host)
{
    return sdhci_readl(sdhci_host, SDHCI_PRESENT_STATE);
}

static uint32_t sdhci_get_int_status_flag(struct sdhci_host* sdhci_host)
{
    return sdhci_readl(sdhci_host, SDHCI_INT_STATUS);
}

static rt_err_t sdhci_wait_bus_idle(struct sdhci_host* host,
    rt_uint32_t inhibit_mask, rt_uint32_t timeout_ms);
static rt_err_t kd_mmc_clock_freq_change(struct sdhci_host* host,
    uint32_t clock);

/* Never returns 0: a zero tick count means "poll" to the RT-Thread IPC calls,
 * which would turn every wait into an instant timeout. */
static rt_tick_t sdhci_ms_to_tick(rt_uint32_t ms)
{
    rt_uint64_t tick = ((rt_uint64_t)ms * RT_TICK_PER_SECOND + 999U) / 1000U;

    if (tick > (rt_uint64_t)RT_TICK_MAX / 2U)
        tick = (rt_uint64_t)RT_TICK_MAX / 2U;

    return tick ? (rt_tick_t)tick : 1;
}

static rt_err_t sdhci_wait_internal_clock(struct sdhci_host* host)
{
    rt_uint32_t timeout = 150000U;

    while (!(sdhci_readw(host, SDHCI_CLOCK_CONTROL) &
             SDHCI_CLOCK_INT_STABLE)) {
        if (!timeout--)
            return -RT_ETIMEOUT;
        cpu_ticks_delay_us(1);
    }
    return RT_EOK;
}

static void sdhci_clear_int_status_flag(struct sdhci_host* sdhci_host, uint32_t mask)
{
    sdhci_writel(sdhci_host, mask, SDHCI_INT_STATUS);
}

static void sdhic_error_recovery(struct sdhci_host* sdhci_host)
{
    /* Error status can clear the inhibit bits before recovery runs. Reset
     * both state machines after a failed transfer, matching the original
     * recovery behavior without resetting successful transfers. */
    sdhci_reset(sdhci_host, SDHCI_RESET_CMD);
    sdhci_reset(sdhci_host, SDHCI_RESET_DATA);
}

static rt_err_t sdhci_receive_command_response(struct sdhci_host* sdhci_host, struct sdhci_command* command)
{
    if (command->responseType == card_response_type_r2) {
        /* CRC is stripped so we need to do some shifting. */
        for (int i = 0; i < 4; i++) {
            command->response[3 - i] = sdhci_readl(sdhci_host, SDHCI_RESPONSE + (3 - i) * 4) << 8;
            if (i != 3)
                command->response[3 - i] |= sdhci_readb(sdhci_host, SDHCI_RESPONSE + (3 - i) * 4 - 1);
        }
    } else {
        command->response[0] = sdhci_readl(sdhci_host, SDHCI_RESPONSE);
    }
    /* check response error flag */
    if ((command->responseErrorFlags != 0U) &&
        ((command->responseType == card_response_type_r1) ||
         (command->responseType == card_response_type_r1b) ||
         (command->responseType == card_response_type_r6) ||
         (command->responseType == card_response_type_r5) ||
         (command->responseType == card_response_type_r5b))) {
        if (((command->responseErrorFlags) & (command->response[0U])) != 0U)
            return -1; // kStatus_USDHC_SendCommandFailed;
    }

    return 0;
}

static void sdhci_send_command(struct sdhci_host* sdhci_host, struct sdhci_command* command)
{
    RT_ASSERT(RT_NULL != command);

    uint32_t cmd_r, xfer_mode;
    struct sdhci_data* sdhci_data = sdhci_host->sdhci_data;

    cmd_r = SDHCI_MAKE_CMD(command->index, command->flags);

    if (sdhci_data != RT_NULL) {
#ifdef SDHCI_SDMA_ENABLE
        uint32_t start_addr;
        if (sdhci_data->rxData) {
            start_addr = (rt_ubase_t)((uint8_t*)sdhci_data->rxData + PV_OFFSET);
            rt_hw_cpu_dcache_invalidate(sdhci_data->rxData, sdhci_data->blockSize * sdhci_data->blockCount);
        } else {
            start_addr = (rt_ubase_t)((uint8_t*)sdhci_data->txData + PV_OFFSET);
            rt_hw_cpu_dcache_clean((void*)(long)start_addr, sdhci_data->blockSize * sdhci_data->blockCount);
        }
        command->flags2 |= sdhci_enable_dma_flag;
        sdhci_host->sdma_start_addr = start_addr;
        sdhci_host->sdma_next_boundary =
            (start_addr & ~(SDHCI_DEFAULT_BOUNDARY_SIZE - 1U)) +
            SDHCI_DEFAULT_BOUNDARY_SIZE;
        sdhci_host->sdma_active = 1;
        sdhci_writel(sdhci_host, start_addr, SDHCI_DMA_ADDRESS);
#endif
        sdhci_writew(sdhci_host,
            SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY_ARG,
                             sdhci_data->blockSize),
            SDHCI_BLOCK_SIZE);
        sdhci_writew(sdhci_host, sdhci_data->blockCount, SDHCI_BLOCK_COUNT);
    }
    xfer_mode = command->flags2 & 0x1ff;
    sdhci_writew(sdhci_host, xfer_mode, SDHCI_TRANSFER_MODE);
    sdhci_writel(sdhci_host, command->argument, SDHCI_ARGUMENT);
    sdhci_writew(sdhci_host, cmd_r, SDHCI_COMMAND);
}

static rt_err_t sdhci_wait_command_done(struct sdhci_host* sdhci_host, struct sdhci_command* command, rt_bool_t executeTuning)
{
    RT_ASSERT(RT_NULL != command);
    rt_err_t err = RT_EOK;
    rt_uint32_t event;
    rt_uint32_t status;
    rt_uint32_t required = SDHCI_INT_RESPONSE;
    rt_uint32_t completed = 0;
    rt_tick_t start;
    rt_tick_t elapsed;
    rt_tick_t timeout_ticks;

    /* tuning cmd do not need to wait command done */
    if (executeTuning)
        return 0;

    /* A response-with-busy command ends when the controller sees DAT[0]
     * released, which it reports as Transfer Complete.  Only wait for it when
     * the command carries no data of its own: with a data phase present the
     * Transfer Complete belongs to that phase, and consuming it here would
     * leave sdhci_transfer_data_blocking() waiting out its whole timeout. */
    if (sdhci_host->sdhci_data == RT_NULL &&
        (command->responseType == card_response_type_r1b ||
         command->responseType == card_response_type_r5b)) {
        required |= SDHCI_INT_DATA_END;
    }

    timeout_ticks = sdhci_ms_to_tick(command->timeoutMs);
    start = rt_tick_get();

    while ((completed & required) != required) {
        elapsed = rt_tick_get() - start;
        if (elapsed >= timeout_ticks) {
            err = -RT_ETIMEOUT;
            break;
        }

        err = rt_event_recv(&sdhci_host->event,
            SDHCI_INT_ERROR | (required & ~completed),
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
            (rt_int32_t)(timeout_ticks - elapsed), &event);
        if (err != RT_EOK)
            break;
        if (event & SDHCI_INT_ERROR) {
            /* Card discovery intentionally tries commands unsupported by the
             * attached card type. Leave policy and user-facing diagnostics to
             * the MMC/SDIO core instead of reporting every negative probe. */
            LOG_D("host%d CMD%u arg 0x%08x command error 0x%04x",
                sdhci_host->index, command->index, command->argument,
                sdhci_host->error_code);
            return -RT_EIO;
        }
        completed |= event;
    }

    if (err != RT_EOK) {
        status = sdhci_get_int_status_flag(sdhci_host);
        LOG_E("host%d CMD%u arg 0x%08x completion timeout "
              "required=0x%08x completed=0x%08x status=0x%08x "
              "int_en=0x%08x sig_en=0x%08x state=0x%08x",
            sdhci_host->index, command->index, command->argument,
            required, completed, status,
            sdhci_readl(sdhci_host, SDHCI_INT_ENABLE),
            sdhci_readl(sdhci_host, SDHCI_SIGNAL_ENABLE),
            sdhci_readl(sdhci_host, SDHCI_PRESENT_STATE));
        return err;
    }

    return sdhci_receive_command_response(sdhci_host, command);
}

static rt_err_t sdhci_transfer_data_blocking(struct sdhci_host* sdhci_host, struct sdhci_data* data)
{
#ifdef SDHCI_SDMA_ENABLE
    rt_err_t err;
    rt_uint32_t event;

    /* SDMA boundary crossings are serviced in sdhci_irq(); this only has to
     * wait for the transfer to end.  The only bits sdhci_irq() forwards are
     * the two waited on here, so one recv decides the transfer. */
    err = rt_event_recv(&sdhci_host->event,
        SDHCI_INT_ERROR | SDHCI_INT_DATA_END,
        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
        sdhci_ms_to_tick(data->timeoutMs), &event);
    if (err != RT_EOK) {
        LOG_E("host%d CMD%u data completion timeout after %u ms",
            sdhci_host->index, sdhci_host->sdhci_command->index,
            (unsigned int)data->timeoutMs);
        return err;
    }
    if (event & SDHCI_INT_ERROR) {
        LOG_E("host%d CMD%u arg 0x%08x data %ux%u error 0x%04x",
            sdhci_host->index, sdhci_host->sdhci_command->index,
            sdhci_host->sdhci_command->argument,
            (unsigned int)data->blockSize,
            (unsigned int)data->blockCount,
            sdhci_host->error_code);
        LOG_E("host%d status=0x%08x dma=0x%08x start=0x%08x "
              "next=0x%08x blocks_left=%u state=0x%08x",
            sdhci_host->index, sdhci_host->error_int_status,
            sdhci_host->error_dma_address,
            sdhci_host->sdma_start_addr,
            sdhci_host->sdma_next_boundary,
            (unsigned int)sdhci_host->error_block_count,
            sdhci_host->error_present_state);
        return -RT_EIO;
    }
    if (data && data->rxData)
        rt_hw_cpu_dcache_invalidate((void*)data->rxData, data->blockSize * data->blockCount);
    return 0;
#else
    uint32_t stat, rdy, mask, timeout, block;

    block = 0;
    timeout = 1000000;
    rdy = SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL;
    mask = SDHCI_DATA_AVAILABLE | SDHCI_SPACE_AVAILABLE;

    while (1) {
        stat = sdhci_get_int_status_flag(sdhci_host);
        if (stat & SDHCI_INT_ERROR) {
            LOG_D("%s: Error detected in status(0x%X)!\n", __func__, stat);
            emmc_reg_display(sdhci_host);
            return -1;
        }
        if (stat & rdy) {
            if (!(sdhci_readl(sdhci_host, SDHCI_PRESENT_STATE) & mask)) {
                continue;
            }
            sdhci_clear_int_status_flag(sdhci_host, rdy);
            if (data->rxData) {
                for (int i = 0; i < data->blockSize / 4; i++)
                    data->rxData[i + block * data->blockSize] = sdhci_readl(sdhci_host, SDHCI_BUFFER);
            } else {
                for (int i = 0; i < data->blockSize / 4; i++)
                    sdhci_writel(sdhci_host, data->txData[i + block * data->blockSize], SDHCI_BUFFER);
            }
            block++;
            if (block >= data->blockCount)
                return 0;
        }
        if (timeout == 0) {
            rt_kprintf("%s: Transfer data timeout\n", __func__);
            return -1;
        }
        timeout--;
        delay_1k(1);
    }
#endif
}

static rt_err_t sdhci_set_transfer_config(struct sdhci_host* sdhci_host, struct sdhci_command* sdhci_command, struct sdhci_data* sdhci_data)
{
    RT_ASSERT(sdhci_command);
    /* Define the flag corresponding to each response type. */
    switch (sdhci_command->responseType) {
    case card_response_type_none:
        break;
    case card_response_type_r1: /* Response 1 */
    case card_response_type_r5: /* Response 5 */
    case card_response_type_r6: /* Response 6 */
    case card_response_type_r7: /* Response 7 */

        sdhci_command->flags |= (sdhci_cmd_resp_short | sdhci_enable_cmd_crc_flag | sdhci_enable_cmd_index_chk_flag);
        break;

    case card_response_type_r1b: /* Response 1 with busy */
    case card_response_type_r5b: /* Response 5 with busy */
        sdhci_command->flags |= (sdhci_cmd_resp_short_busy | sdhci_enable_cmd_crc_flag | sdhci_enable_cmd_index_chk_flag);
        break;

    case card_response_type_r2: /* Response 2 */
        sdhci_command->flags |= (sdhci_cmd_resp_long | sdhci_enable_cmd_crc_flag);
        break;

    case card_response_type_r3: /* Response 3 */
    case card_response_type_r4: /* Response 4 */
        sdhci_command->flags |= (sdhci_cmd_resp_short);
        break;

    default:
        break;
    }

    if (sdhci_command->type == card_command_type_abort) {
        sdhci_command->flags |= sdhci_enable_command_type_abort;
    } else if (sdhci_command->type == card_command_type_resume) {
        sdhci_command->flags |= sdhci_enable_command_type_resume;
    } else if (sdhci_command->type == card_command_type_suspend) {
        sdhci_command->flags |= sdhci_enable_command_type_suspend;
    } else if (sdhci_command->type == card_command_type_normal) {
        sdhci_command->flags |= sdhci_enable_command_type_normal;
    }

    if (sdhci_data) {
        sdhci_command->flags |= sdhci_enable_cmd_data_present_flag;
        sdhci_command->flags2 |= sdhci_enable_block_count_flag;

        if (sdhci_data->rxData) {
            sdhci_command->flags2 |= sdhci_data_read_flag;
        }
        if (sdhci_data->blockCount > 1U) {
            sdhci_command->flags2 |= (sdhci_multiple_block_flag);
            /* auto command 12 */
            if (sdhci_data->enableAutoCommand12) {
                /* Enable Auto command 12. */
                sdhci_command->flags2 |= sdhci_enable_auto_command12_flag;
            }
            /* auto command 23 */
            if (sdhci_data->enableAutoCommand23) {
                sdhci_command->flags2 |= sdhci_enable_auto_command23_flag;
            }
        }
    }
    return 0;
}

static rt_err_t sdhci_transfer_blocking(struct sdhci_host* sdhci_host)
{
    RT_ASSERT(sdhci_host);
    struct sdhci_command* sdhci_command = sdhci_host->sdhci_command;
    struct sdhci_data* sdhci_data = sdhci_host->sdhci_data;
    rt_base_t irq_level;
    uint32_t signal_enable;
    uint32_t inhibit;
    int ret = RT_EOK;

    /* Command Inhibit (DAT) also covers a response-with-busy command holding
     * DAT[0] low, so wait on it for those too and not only for a data phase.
     * An abort is exempt: it exists precisely to break into a stuck transfer.
     * Never let a wedged controller hold the MMC host indefinitely. */
    inhibit = SDHCI_CMD_INHIBIT;
    if (sdhci_data != RT_NULL ||
        sdhci_command->responseType == card_response_type_r1b ||
        sdhci_command->responseType == card_response_type_r5b)
        inhibit |= SDHCI_DATA_INHIBIT;
    if (sdhci_command->type == card_command_type_abort)
        inhibit &= ~SDHCI_DATA_INHIBIT;

    ret = sdhci_wait_bus_idle(sdhci_host, inhibit, sdhci_command->timeoutMs);
    if (ret != RT_EOK) {
        LOG_E("host%d CMD%u bus remained inhibited",
            sdhci_host->index, sdhci_command->index);
        sdhic_error_recovery(sdhci_host);
        return ret;
    }
    sdhci_writel(sdhci_host, SDHCI_INT_ACK_MASK, SDHCI_INT_STATUS);

    ret = sdhci_set_transfer_config(sdhci_host, sdhci_command, sdhci_data);
    if (ret != 0) {
        return ret;
    }
    irq_level = rt_hw_interrupt_disable();
    signal_enable = sdhci_readl(sdhci_host, SDHCI_SIGNAL_ENABLE);
    signal_enable |= SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK;
    sdhci_writel(sdhci_host, signal_enable, SDHCI_SIGNAL_ENABLE);
    rt_hw_interrupt_enable(irq_level);
    rt_event_control(&sdhci_host->event, RT_IPC_CMD_RESET, RT_NULL);
    sdhci_host->error_code = 0;
    sdhci_host->error_int_status = 0;
    sdhci_host->error_dma_address = 0;
    sdhci_host->error_present_state = 0;
    sdhci_host->error_block_count = 0;
    sdhci_send_command(sdhci_host, sdhci_command);
    /* wait command done */
    ret = sdhci_wait_command_done(sdhci_host, sdhci_command, ((sdhci_data == RT_NULL) ? false : sdhci_data->executeTuning));
    /* transfer data */
    if ((sdhci_data != RT_NULL) && (ret == 0)) {
        ret = sdhci_transfer_data_blocking(sdhci_host, sdhci_data);
    }
    sdhci_host->sdma_active = 0;
    irq_level = rt_hw_interrupt_disable();
    signal_enable = sdhci_readl(sdhci_host, SDHCI_SIGNAL_ENABLE);
    signal_enable &= ~(SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK);
    sdhci_writel(sdhci_host, signal_enable, SDHCI_SIGNAL_ENABLE);
    rt_hw_interrupt_enable(irq_level);
    sdhci_writel(sdhci_host, SDHCI_INT_ACK_MASK, SDHCI_INT_STATUS);
    if (ret != RT_EOK)
        sdhic_error_recovery(sdhci_host);
    return ret;
}

static rt_err_t sdhci_init(struct sdhci_host* host)
{
    uint8_t power = SDHCI_POWER_330;
    uint32_t caps;
    uint32_t caps1;
    uint32_t reported_base;
    rt_err_t ret;

    sdhci_reset(host, SDHCI_RESET_ALL);
    caps = sdhci_readl(host, SDHCI_CAPABILITIES);
    caps1 = sdhci_readl(host, SDHCI_CAPABILITIES_1);
    reported_base = ((caps & SDHCI_CLOCK_V3_BASE_MASK) >>
                     SDHCI_CLOCK_BASE_SHIFT) * 1000000U;
    host->clk_mul = (caps1 & SDHCI_CLOCK_MUL_MASK) >>
                    SDHCI_CLOCK_MUL_SHIFT;
    /* Report a mismatch, but keep the configured value: the CAPS base clock
     * field is not authoritative for this core.  U-Boot's snps_sdhci takes the
     * rate from the fixed-clock DT nodes feeding mmc0/mmc1 (200 MHz and
     * 100 MHz) and so never falls back to CAPS, and Linux's dwcmshc does the
     * same through get_max_clock.  Adopting a disagreeing CAPS value would
     * rescale every divider computed from it and silently overclock the card. */
    if (reported_base && reported_base != host->max_clk)
        LOG_W("host%d SDHCI CAPS reports a %u Hz base clock, keeping the "
              "configured %u Hz",
            host->index, reported_base, host->max_clk);
    sdhci_writeb(host, SDHCI_CTRL_HISPD, SDHCI_HOST_CONTROL);
    sdhci_writeb(host, SDHCI_TIMEOUT_MAX, SDHCI_TIMEOUT_CONTROL);
    if (host->io_fixed_1v8)
        power = SDHCI_POWER_180;
    sdhci_writeb(host, SDHCI_POWER_ON | power, SDHCI_POWER_CONTROL);
    sdhci_writew(host, SDHCI_CLOCK_INT_EN, SDHCI_CLOCK_CONTROL);
    ret = sdhci_wait_internal_clock(host);
    if (ret != RT_EOK) {
        LOG_E("host%d initial internal clock never stabilized", host->index);
        return ret;
    }
    sdhci_writel(host, SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK, SDHCI_INT_ENABLE);
    sdhci_writel(host, SDHCI_INT_CARD_INT, SDHCI_SIGNAL_ENABLE);
    return RT_EOK;
}

static void sdhci_irq(int vector, void* param)
{
    struct sdhci_host* host = param;
    uint32_t status = sdhci_get_int_status_flag(host);

    if (status & SDHCI_INT_ERROR) {
        host->error_code = (status >> 16) & 0xffff;
        host->error_int_status = status;
        host->error_dma_address = sdhci_readl(host, SDHCI_DMA_ADDRESS);
        host->error_present_state = sdhci_readl(host, SDHCI_PRESENT_STATE);
        host->error_block_count = sdhci_readw(host, SDHCI_BLOCK_COUNT);
    }
    /* SDMA pauses at every SDHCI_DEFAULT_BOUNDARY_SIZE boundary and resumes on
     * the write of the next system address, so feed it here rather than from the
     * waiting thread: the status clear at the end of this handler would let the
     * engine continue from the stale address, so deferring the feed to a thread
     * round-trip is not an option.  The next boundary is derived from the
     * programmed start address because SDHCI_DMA_ADDRESS is not a reliable
     * read-back on this controller.  sdma_active keeps the feed to transfers
     * that are still running: a DMA_END latched after the waiter gave up, or
     * one seen while servicing a card interrupt during PIO tuning, has no
     * boundary left to advance. */
    if (host->sdma_active && !(status & SDHCI_INT_ERROR) &&
        (status & SDHCI_INT_DMA_END)) {
        uint32_t next = host->sdma_next_boundary;

        host->sdma_next_boundary = next + SDHCI_DEFAULT_BOUNDARY_SIZE;
        sdhci_writel(host, next, SDHCI_DMA_ADDRESS);
    }
    if (status & (SDHCI_INT_ERROR | SDHCI_INT_DATA_END |
                  SDHCI_INT_RESPONSE | SDHCI_INT_DATA_AVAIL)) {
        rt_event_send(&host->event,
            status & (SDHCI_INT_ERROR | SDHCI_INT_DATA_END |
                      SDHCI_INT_RESPONSE | SDHCI_INT_DATA_AVAIL));
    }
    if (status & SDHCI_INT_CARD_INT) {
        sdio_irq_wakeup(host->host);
    }
    /* sdio_irq_wakeup() masks the host interrupt before it wakes the SDIO
     * thread. Clear the latched status now; if DAT[1] is still asserted when
     * the thread re-enables it, the controller will report it again. */
    sdhci_clear_int_status_flag(host, status);
}

static rt_int32_t sdhci_execute_tuning_cmd(struct rt_mmcsd_host* mmcsd_host, rt_int32_t opcode);

static void kd_mmc_request(struct rt_mmcsd_host* host, struct rt_mmcsd_req* req)
{
    struct sdhci_host* mmcsd;
    struct rt_mmcsd_cmd* cmd;
    struct rt_mmcsd_data* data;
    rt_err_t error;
    struct sdhci_data sdhci_data = { 0 };
    struct sdhci_command sdhci_command = { 0 };
#ifdef SDHCI_SDMA_ENABLE
    void *allocated_bounce = RT_NULL;
#endif

    RT_ASSERT(host != RT_NULL);
    RT_ASSERT(req != RT_NULL);

    mmcsd = (struct sdhci_host*)host->private_data;
    RT_ASSERT(mmcsd != RT_NULL);

    cmd = req->cmd;
    RT_ASSERT(cmd != RT_NULL);

    LOG_D("\tcmd->cmd_code: %02d, cmd->arg: %08x, cmd->flags: %08x --> ", cmd->cmd_code, cmd->arg, cmd->flags);

    data = cmd->data;

    /* The clock is programmed from kd_set_iocfg(), which the core only calls
     * when the bus configuration changes - never in response to a failed
     * command.  So retry it from here rather than rejecting every request
     * forever; otherwise a single transient failure retires the host. */
    if (mmcsd->clock_error != RT_EOK) {
        mmcsd->clock_error = host->io_cfg.clock ?
            kd_mmc_clock_freq_change(mmcsd, host->io_cfg.clock) : -RT_EIO;
        if (mmcsd->clock_error != RT_EOK) {
            LOG_E("host%d rejecting CMD%u after clock failure: %d",
                mmcsd->index, cmd->cmd_code, mmcsd->clock_error);
            cmd->err = mmcsd->clock_error;
            if (data)
                data->err = mmcsd->clock_error;
            mmcsd_req_complete(host);
            return;
        }
        LOG_W("host%d recovered the %u Hz clock", mmcsd->index,
            host->io_cfg.clock);
    }

    sdhci_command.index = cmd->cmd_code;
    sdhci_command.argument = cmd->arg;

    if (cmd->cmd_code == STOP_TRANSMISSION ||
        (cmd->flags & RESP_MASK) == RESP_R5B)
        sdhci_command.type = card_command_type_abort;
    else
        sdhci_command.type = card_command_type_normal;

    switch (cmd->flags & RESP_MASK) {
    case RESP_NONE:
        sdhci_command.responseType = card_response_type_none;
        break;
    case RESP_R1:
        sdhci_command.responseType = card_response_type_r1;
        break;
    case RESP_R1B:
        sdhci_command.responseType = card_response_type_r1b;
        break;
    case RESP_R2:
        sdhci_command.responseType = card_response_type_r2;
        break;
    case RESP_R3:
        sdhci_command.responseType = card_response_type_r3;
        break;
    case RESP_R4:
        sdhci_command.responseType = card_response_type_r4;
        break;
    case RESP_R6:
        sdhci_command.responseType = card_response_type_r6;
        break;
    case RESP_R7:
        sdhci_command.responseType = card_response_type_r7;
        break;
    case RESP_R5:
        sdhci_command.responseType = card_response_type_r5;
        break;
    case RESP_R5B:
        sdhci_command.responseType = card_response_type_r5b;
        break;
    default:
        RT_ASSERT(RT_NULL);
    }

    sdhci_command.flags = 0;
    sdhci_command.flags2 = 0;
    sdhci_command.responseErrorFlags = 0;
    sdhci_command.timeoutMs = SDHCI_COMMAND_TIMEOUT_MS;
    if ((sdhci_command.responseType == card_response_type_r1b ||
         sdhci_command.responseType == card_response_type_r5b) &&
        cmd->busy_timeout) {
        sdhci_command.timeoutMs = cmd->busy_timeout;
    }
    mmcsd->sdhci_command = &sdhci_command;

    if (data) {
        if (req->stop != RT_NULL)
            sdhci_data.enableAutoCommand12 = true;
        else
            sdhci_data.enableAutoCommand12 = false;

        sdhci_data.enableAutoCommand23 = false;

        sdhci_data.blockSize = data->blksize;
        sdhci_data.blockCount = data->blks;
        {
            /* mmcsd_set_data_timeout() yields the card's worst-case access
             * latency for a SINGLE block - for SDHC reads a flat 100 ms - which
             * is what the hardware data timeout counter in
             * SDHCI_TIMEOUT_CONTROL enforces.  This software timeout is a
             * different thing: a backstop against a lost interrupt or a wedged
             * controller across the WHOLE transfer, so charging one block's
             * latency for a 512-block CMD18 just aborts healthy reads.  Budget
             * the per-block latency once, add the time the blocks need on the
             * bus, and keep the 1 s floor this driver used before the value was
             * taken from the core. */
            rt_uint64_t timeout_ns = data->timeout_ns;
            rt_uint64_t timeout_ms;
            rt_uint64_t bytes;

            if (data->timeout_clks && host->io_cfg.clock)
            {
                timeout_ns += ((rt_uint64_t)data->timeout_clks *
                               1000000000ULL) / host->io_cfg.clock;
            }
            timeout_ms = (timeout_ns + 999999ULL) / 1000000ULL;

            bytes = (rt_uint64_t)sdhci_data.blockSize *
                    sdhci_data.blockCount;
            if (host->io_cfg.clock)
            {
                /* Bits at one bit per clock: pessimistic by the bus width,
                 * which is the right direction for a watchdog. */
                timeout_ms += (bytes * 8000ULL + host->io_cfg.clock - 1ULL) /
                              host->io_cfg.clock;
            }

            if (timeout_ms < SDHCI_COMMAND_TIMEOUT_MS)
                timeout_ms = SDHCI_COMMAND_TIMEOUT_MS;
            sdhci_data.timeoutMs = (rt_uint32_t)timeout_ms;
        }

        if (data->flags == DATA_DIR_WRITE) {
            sdhci_data.txData = data->buf;
            sdhci_data.rxData = RT_NULL;
        } else {
            sdhci_data.rxData = data->buf;
            sdhci_data.txData = RT_NULL;
        }
#ifdef SDHCI_SDMA_ENABLE
        /* SDMA is not cache coherent here.
         *
         * Receive needs the caller's buffer to own whole cache lines: the
         * invalidate before the transfer rounds the start down and covers the
         * trailing partial line, so a neighbour sharing either end would lose
         * whatever it had dirty in cache.  Stage those through an aligned
         * bounce buffer.
         *
         * Transmit needs no such thing.  All it does is write the CPU's dirty
         * lines back before the engine reads RAM, and writing back more than
         * asked for is harmless - the extra lines simply keep the values they
         * already had.  Only the engine's own address alignment matters, so a
         * word-aligned buffer goes straight to DMA.  This is worth the
         * distinction: virtually no Wi-Fi frame is a multiple of 64 bytes, so
         * bouncing transmits cost an aligned allocation plus a full copy on
         * every single frame. */
        uint32_t sz = sdhci_data.blockSize * sdhci_data.blockCount;
        uint32_t pad = 0;
        if (sz & (CACHE_LINESIZE - 1))
            pad = (sz + (CACHE_LINESIZE - 1)) & ~(CACHE_LINESIZE - 1);
        if (sdhci_data.rxData &&
            (((uint64_t)sdhci_data.rxData & (CACHE_LINESIZE - 1)) || pad)) {
            if (sz <= SDHCI_SMALL_BOUNCE_SIZE) {
                sdhci_data.rxData = mmcsd->sdma_bounce;
            } else {
                allocated_bounce = rt_malloc_align(pad ? pad : sz,
                                                    CACHE_LINESIZE);
                sdhci_data.rxData = allocated_bounce;
            }
        } else if (sdhci_data.txData &&
                   ((uint64_t)sdhci_data.txData &
                    (SDHCI_SDMA_ALIGNMENT - 1))) {
            void *tx_bounce;

            if (sz <= SDHCI_SMALL_BOUNCE_SIZE) {
                tx_bounce = mmcsd->sdma_bounce;
            } else {
                allocated_bounce = rt_malloc_align(pad ? pad : sz,
                                                    CACHE_LINESIZE);
                tx_bounce = allocated_bounce;
            }
            if (tx_bounce)
                rt_memcpy(tx_bounce, data->buf, sz);
            sdhci_data.txData = tx_bounce;
        }
        /* Refuse the request rather than handing the DMA engine a null address
         * and copying through it. */
        if ((data->flags == DATA_DIR_WRITE && !sdhci_data.txData) ||
            (data->flags != DATA_DIR_WRITE && !sdhci_data.rxData)) {
            LOG_E("no bounce buffer for a %u byte transfer", (unsigned int)sz);
            cmd->err = -RT_ENOMEM;
            mmcsd->sdhci_data = RT_NULL;
            mmcsd_req_complete(host);
            return;
        }
#endif
        mmcsd->sdhci_data = &sdhci_data;
    } else {
        mmcsd->sdhci_data = RT_NULL;
    }
    error = sdhci_transfer_blocking(mmcsd);
#ifdef SDHCI_SDMA_ENABLE
    if (data && sdhci_data.rxData && sdhci_data.rxData != data->buf) {
        rt_memcpy(data->buf, sdhci_data.rxData,
                  sdhci_data.blockSize * sdhci_data.blockCount);
    }
    if (allocated_bounce)
        rt_free_align(allocated_bounce);
#endif
    if (error != RT_EOK) {
        LOG_D(" ***USDHC_TransferBlocking error: %d*** --> \n", error);
        cmd->err = error;
        if (data)
            data->err = error;
    }

    if ((cmd->flags & RESP_MASK) == RESP_R2) {
        cmd->resp[3] = sdhci_command.response[0];
        cmd->resp[2] = sdhci_command.response[1];
        cmd->resp[1] = sdhci_command.response[2];
        cmd->resp[0] = sdhci_command.response[3];
        LOG_D(" resp 0x%08X 0x%08X 0x%08X 0x%08X\n",
            cmd->resp[0], cmd->resp[1], cmd->resp[2], cmd->resp[3]);
    } else {
        cmd->resp[0] = sdhci_command.response[0];
        LOG_D(" resp 0x%08X\n", cmd->resp[0]);
    }
    mmcsd_req_complete(host);
}

/* The inhibit bits clear a few controller clocks after the previous command
 * retires, so spin for the first millisecond to keep the common case cheap.
 * Past that the bus is waiting on the card - an R1b busy signal can legally
 * run for seconds - so sleep instead of burning the CPU at the mmcsd thread's
 * priority, which sits above the Wi-Fi transmit thread. */
#define SDHCI_INHIBIT_SPIN_US 1000U

static rt_err_t sdhci_wait_bus_idle(struct sdhci_host* host,
                                    rt_uint32_t inhibit_mask,
                                    rt_uint32_t timeout_ms)
{
    rt_uint32_t spin = SDHCI_INHIBIT_SPIN_US;
    rt_tick_t timeout_ticks;
    rt_tick_t start;

    if (!inhibit_mask)
        return RT_EOK;

    while (sdhci_get_present_status_flag(host) & inhibit_mask) {
        if (!spin--)
            break;
        cpu_ticks_delay_us(1);
    }
    if (!(sdhci_get_present_status_flag(host) & inhibit_mask))
        return RT_EOK;

    timeout_ticks = sdhci_ms_to_tick(timeout_ms);
    start = rt_tick_get();
    while (sdhci_get_present_status_flag(host) & inhibit_mask) {
        if ((rt_tick_get() - start) >= timeout_ticks)
            return -RT_ETIMEOUT;
        rt_thread_mdelay(1);
    }

    return RT_EOK;
}

static rt_err_t sdhci_config_tuning_engine(struct sdhci_host* host)
{
    uint16_t clk;
    uint32_t val;
    rt_err_t ret;

    clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
    sdhci_writew(host, clk & ~SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);

    sdhci_writeb(host, 0xc, DWC_MSHC_ATDL_CNFG);

    val = sdhci_readl(host, SDHCI_VENDER_AT_CTRL_REG);
    val &= ~(SDHCI_TUNE_CI_SEL | SDHCI_TUNE_RPT_TUNE_ERR |
        SDHCI_TUNE_SW_TUNE_EN | SDHCI_TUNE_WIN_EDGE_SEL_MASK |
        SDHCI_TUNE_PRE_CHANGE_DLY_MASK | SDHCI_TUNE_POST_CHANGE_DLY_MASK |
        SDHCI_TUNE_SWIN_TH_VAL_MASK);
    val |= SDHCI_TUNE_AT_EN | SDHCI_TUNE_SWIN_TH_EN |
        SDHCI_TUNE_CLK_STOP_EN_MASK |
        (SDHCI_TUNE_PRE_CHANGE_DLY_VAL << SDHCI_TUNE_PRE_CHANGE_DLY_LSB) |
        (SDHCI_TUNE_POST_CHANGE_DLY_VAL << SDHCI_TUNE_POST_CHANGE_DLY_LSB) |
        (SDHCI_TUNE_SWIN_TH_VAL << SDHCI_TUNE_SWIN_TH_VAL_LSB);
    sdhci_writel(host, val, SDHCI_VENDER_AT_CTRL_REG);
    sdhci_writel(host, 0, SDHCI_VENDER_AT_STAT_REG);

    sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
    if (clk & SDHCI_CLOCK_INT_EN) {
        ret = sdhci_wait_internal_clock(host);
        if (ret != RT_EOK) {
            LOG_E("host%d tuning clock never stabilized", host->index);
            return ret;
        }
    }
    return RT_EOK;
}

static void sdhci_reset_tuning(struct sdhci_host* host)
{
    uint16_t ctrl2;

    ctrl2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
    ctrl2 &= ~(SDHCI_CTRL_EXEC_TUNING | SDHCI_CTRL_TUNED_CLK);
    sdhci_writew(host, ctrl2, SDHCI_HOST_CONTROL2);
}

static rt_err_t sdhci_send_tuning_cmd(struct sdhci_host* host, rt_int32_t opcode)
{
    rt_err_t err;
    rt_uint32_t event;
    uint16_t block_size;
    uint16_t cmd;
    uint32_t i;
    volatile uint32_t scratch;

    err = sdhci_wait_bus_idle(host,
                               SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT,
                               SDHCI_COMMAND_TIMEOUT_MS);
    if (err)
        return err;

    block_size = (host->host->io_cfg.bus_width == MMCSD_BUS_WIDTH_8) ? 128 : 64;

    rt_event_control(&host->event, RT_IPC_CMD_RESET, RT_NULL);
    sdhci_writel(host, SDHCI_INT_ACK_MASK, SDHCI_INT_STATUS);
    sdhci_writew(host,
        SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY_ARG, block_size),
        SDHCI_BLOCK_SIZE);
    sdhci_writew(host, 1, SDHCI_BLOCK_COUNT);
    sdhci_writew(host, SDHCI_TRNS_READ, SDHCI_TRANSFER_MODE);
    sdhci_writel(host, 0, SDHCI_ARGUMENT);

    cmd = SDHCI_MAKE_CMD(opcode,
        SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA);
    sdhci_writew(host, cmd, SDHCI_COMMAND);

    err = rt_event_recv(&host->event, SDHCI_INT_ERROR | SDHCI_INT_DATA_AVAIL,
        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
        sdhci_ms_to_tick(SDHCI_TUNING_TIMEOUT_MS), &event);
    if (err)
        return err;

    if (event & SDHCI_INT_ERROR)
        return -RT_ERROR;

    for (i = 0; i < block_size / sizeof(uint32_t); i++)
        scratch = sdhci_readl(host, SDHCI_BUFFER);
    (void)scratch;

    return RT_EOK;
}

static rt_int32_t sdhci_execute_tuning_cmd(struct rt_mmcsd_host* mmcsd_host, rt_int32_t opcode)
{
    struct sdhci_host* host;
    uint32_t old_int_enable;
    uint32_t old_signal_enable;
    uint32_t at_stat;
    uint64_t tuning_start_ms;
    uint16_t ctrl2;
    uint32_t i;
    rt_err_t ret;
    rt_bool_t tuning_done;

    RT_ASSERT(mmcsd_host != RT_NULL);
    RT_ASSERT(mmcsd_host->private_data != RT_NULL);

    host = (struct sdhci_host*)mmcsd_host->private_data;

    if (!host->is_emmc_card || mmcsd_host->io_cfg.timing != MMCSD_TIMING_MMC_HS200)
        return RT_EOK;

    if (opcode != SEND_TUNING_BLOCK_HS200)
        return -RT_ERROR;

    old_int_enable = sdhci_readl(host, SDHCI_INT_ENABLE);
    old_signal_enable = sdhci_readl(host, SDHCI_SIGNAL_ENABLE);

    ret = sdhci_config_tuning_engine(host);
    if (ret != RT_EOK)
        return ret;

    sdhci_writel(host, old_int_enable | SDHCI_INT_DATA_AVAIL | SDHCI_INT_CMD_MASK |
        SDHCI_INT_DATA_MASK, SDHCI_INT_ENABLE);
    sdhci_writel(host, old_signal_enable | SDHCI_INT_DATA_AVAIL | SDHCI_INT_ERROR,
        SDHCI_SIGNAL_ENABLE);

    ctrl2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
    ctrl2 &= ~SDHCI_CTRL_TUNED_CLK;
    ctrl2 |= SDHCI_CTRL_EXEC_TUNING;
    sdhci_writew(host, ctrl2, SDHCI_HOST_CONTROL2);

    tuning_start_ms = cpu_ticks_ms();
    ret = -RT_ERROR;
    tuning_done = RT_FALSE;
    for (i = 0; i < SDHCI_TUNING_LOOP_COUNT; i++) {
        if ((cpu_ticks_ms() - tuning_start_ms) >= SDHCI_TUNING_TOTAL_TIMEOUT_MS) {
            ret = -RT_ETIMEOUT;
            break;
        }

        ret = sdhci_send_tuning_cmd(host, opcode);
        if (ret)
            break;

        ctrl2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
        if (!(ctrl2 & SDHCI_CTRL_EXEC_TUNING)) {
            tuning_done = RT_TRUE;
            if (ctrl2 & SDHCI_CTRL_TUNED_CLK)
                ret = RT_EOK;
            else
                ret = -RT_ERROR;
            break;
        }
    }

    ctrl2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
    at_stat = sdhci_readl(host, SDHCI_VENDER_AT_STAT_REG);
    if (!tuning_done && ret == RT_EOK)
        ret = -RT_ETIMEOUT;

    if (ret == RT_EOK) {
        LOG_I("eMMC HS200 tuning ok, loops=%d.", i + 1);
        sdhci_reset(host, SDHCI_RESET_DATA);
    } else {
        sdhci_reset_tuning(host);
        sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
        LOG_W("eMMC HS200 tuning failed, err=%d, loops=%d, host_ctrl2=0x%04x, at_stat=0x%08x, err_stat=0x%04x.",
            ret, i, ctrl2, at_stat, host->error_code);
    }

    sdhci_writel(host, old_int_enable, SDHCI_INT_ENABLE);
    sdhci_writel(host, old_signal_enable, SDHCI_SIGNAL_ENABLE);
    sdhci_writel(host, SDHCI_INT_ACK_MASK, SDHCI_INT_STATUS);

    return ret;
}

static rt_err_t kd_mmc_clock_freq_change(struct sdhci_host* host, uint32_t clock)
{
    uint32_t div;
    uint32_t encoded_div;
    rt_uint64_t source_clk;
    uint16_t val = 0;
    rt_err_t ret;

    /* This DWC core latches a new divider only after leaving programmable
     * clock mode.  Keep the internal clock running, but gate the card clock
     * and clear the generator select before changing the divider. */
    val = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
    val &= ~(SDHCI_CLOCK_CARD_EN | SDHCI_PROG_CLOCK_MODE);
    sdhci_writew(host, val, SDHCI_CLOCK_CONTROL);
    host->clock_upper_bound = 0;
    if (clock == 0)
        return RT_EOK;

    source_clk = host->max_clk;
    if (host->clk_mul)
        source_clk *= (rt_uint64_t)host->clk_mul + 1U;

    /* K230 needs the programmable-mode select bit even when the capability
     * multiplier is zero.  The multiplier still determines the divider
     * encoding: N + 1 when programmable mode is advertised, otherwise the
     * standard SDHCI 2N encoding used by the K230 U-Boot driver. */
    div = (uint32_t)((source_clk + clock - 1U) / clock);
    if (host->clk_mul) {
        if (div > 1024U)
            div = 1024U;
        encoded_div = div - 1U;
    } else {
        if (div > 1U && (div & 1U))
            div++;
        if (div > SDHCI_MAX_DIV_SPEC_300)
            div = SDHCI_MAX_DIV_SPEC_300;
        encoded_div = div >> 1;
    }
    host->clock_upper_bound = (uint32_t)(source_clk / div);

    val = SDHCI_PROG_CLOCK_MODE;
    val |= (encoded_div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
    val |= ((encoded_div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
        << SDHCI_DIVIDER_HI_SHIFT;
    val |= SDHCI_CLOCK_INT_EN;
    sdhci_writew(host, val, SDHCI_CLOCK_CONTROL);

    ret = sdhci_wait_internal_clock(host);
    if (ret != RT_EOK) {
        LOG_E("host%d internal clock never stabilized", host->index);
        host->clock_upper_bound = 0;
        return ret;
    }
    val |= SDHCI_CLOCK_CARD_EN;
    sdhci_writew(host, val, SDHCI_CLOCK_CONTROL);
    LOG_D("host%d clock source=%llu requested=%u actual=%u mode=%s "
          "div=%u encoded_div=%u control=0x%04x",
        host->index, (unsigned long long)source_clk, clock,
        host->clock_upper_bound, host->clk_mul ? "N+1" : "2N", div,
        encoded_div, sdhci_readw(host, SDHCI_CLOCK_CONTROL));
    return RT_EOK;
}

static void kd_mmc_set_timing(struct sdhci_host* host, uint32_t timing)
{
    uint16_t ctrl2;

    ctrl2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
    ctrl2 &= ~SDHCI_CTRL_UHS_MASK;

    switch (timing) {
    case MMCSD_TIMING_MMC_HS:
    case MMCSD_TIMING_SD_HS:
    case MMCSD_TIMING_UHS_SDR25:
        ctrl2 |= SDHCI_CTRL_UHS_SDR25;
        break;
    case MMCSD_TIMING_UHS_SDR50:
        ctrl2 |= SDHCI_CTRL_UHS_SDR50;
        break;
    case MMCSD_TIMING_MMC_DDR52:
    case MMCSD_TIMING_UHS_DDR50:
        ctrl2 |= SDHCI_CTRL_UHS_DDR50;
        break;
    case MMCSD_TIMING_MMC_HS200:
    case MMCSD_TIMING_UHS_SDR104:
        ctrl2 |= SDHCI_CTRL_UHS_SDR104;
        break;
    case MMCSD_TIMING_MMC_HS400:
        ctrl2 |= SDHCI_CTRL_HS400;
        break;
    case MMCSD_TIMING_LEGACY:
    case MMCSD_TIMING_UHS_SDR12:
    default:
        ctrl2 |= SDHCI_CTRL_UHS_SDR12;
        break;
    }

    if (host->io_fixed_1v8)
        ctrl2 |= SDHCI_CTRL_VDD_180;

    sdhci_writew(host, ctrl2, SDHCI_HOST_CONTROL2);
}

static rt_bool_t kd_mmc_timing_is_high_speed(rt_uint32_t timing)
{
    switch (timing) {
    case MMCSD_TIMING_SD_HS:
    case MMCSD_TIMING_MMC_HS:
    case MMCSD_TIMING_MMC_DDR52:
    case MMCSD_TIMING_MMC_HS200:
    case MMCSD_TIMING_MMC_HS400:
    case MMCSD_TIMING_UHS_SDR25:
    case MMCSD_TIMING_UHS_SDR50:
    case MMCSD_TIMING_UHS_SDR104:
    case MMCSD_TIMING_UHS_DDR50:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static void kd_set_iocfg(struct rt_mmcsd_host* host, struct rt_mmcsd_io_cfg* io_cfg)
{
    struct sdhci_host* mmcsd;
    unsigned int sdhci_clk;
    unsigned int bus_width;
    uint32_t old_clock;
    rt_err_t ret;
    uint8_t ctrl;
    RT_ASSERT(host != RT_NULL);
    RT_ASSERT(host->private_data != RT_NULL);
    RT_ASSERT(io_cfg != RT_NULL);

    mmcsd = (struct sdhci_host*)host->private_data;
    sdhci_clk = io_cfg->clock;
    bus_width = io_cfg->bus_width;

    LOG_D("%s: sdhci_clk=%u, bus_width:%u, timing:%u",
        __func__, sdhci_clk, bus_width, io_cfg->timing);

    old_clock = mmcsd->clock_upper_bound;

    /* Gate the card clock while the bus width, timing and divider change.
     * Requesting 0 Hz cannot fail, so there is nothing to check here. */
    (void)kd_mmc_clock_freq_change(mmcsd, 0);
    mmcsd->clock_error = RT_EOK;

    ctrl = sdhci_readb(mmcsd, SDHCI_HOST_CONTROL);
    ctrl &= ~(SDHCI_CTRL_4BITBUS | SDHCI_CTRL_8BITBUS);
    if (bus_width == 3)
        ctrl |= SDHCI_CTRL_8BITBUS;
    else if (bus_width == 2)
        ctrl |= SDHCI_CTRL_4BITBUS;

    /* io_cfg->timing is not a reliable indicator on its own: init_sd() raises
     * CARD_FLAG_HIGHSPEED and clocks the card to 50 MHz without ever calling
     * mmcsd_set_timing(), so an SD card sits at MMCSD_TIMING_LEGACY while
     * running well past the 25 MHz that default speed allows.  Keep the clock
     * test as the backstop it always was, or the boot card loses high-speed
     * output timing and its reads start failing. */
    if (kd_mmc_timing_is_high_speed(io_cfg->timing) || sdhci_clk > 26000000)
        ctrl |= SDHCI_CTRL_HISPD;
    else
        ctrl &= ~SDHCI_CTRL_HISPD;

    sdhci_writeb(mmcsd, ctrl, SDHCI_HOST_CONTROL);
    kd_mmc_set_timing(mmcsd, io_cfg->timing);
    if (sdhci_clk) {
        ret = kd_mmc_clock_freq_change(mmcsd, sdhci_clk);
        mmcsd->clock_error = ret;
        if (ret != RT_EOK)
            LOG_E("host%d failed to apply %u Hz clock", mmcsd->index,
                sdhci_clk);
        else if (old_clock != mmcsd->clock_upper_bound)
            LOG_D("SDIO%d bus frequency: requested=%u Hz, actual=%u Hz",
                mmcsd->index, sdhci_clk, mmcsd->clock_upper_bound);
    }
}

static void kd_enable_sdio_irq(struct rt_mmcsd_host* mmcsd_host, rt_int32_t en)
{
    struct sdhci_host* host = (struct sdhci_host*)mmcsd_host->private_data;
    rt_base_t irq_level;
    uint32_t val;

    /* A card interrupt only reaches the CPU when it is enabled in both the
     * status-enable and the signal-enable register; sdhci_init() arms only the
     * latter, so enabling one here would leave it permanently masked.  Both are
     * 32-bit registers. */
    irq_level = rt_hw_interrupt_disable();
    val = sdhci_readl(host, SDHCI_INT_ENABLE);
    if (en)
        val |= SDHCI_INT_CARD_INT;
    else
        val &= ~SDHCI_INT_CARD_INT;
    sdhci_writel(host, val, SDHCI_INT_ENABLE);

    val = sdhci_readl(host, SDHCI_SIGNAL_ENABLE);
    if (en)
        val |= SDHCI_INT_CARD_INT;
    else
        val &= ~SDHCI_INT_CARD_INT;
    sdhci_writel(host, val, SDHCI_SIGNAL_ENABLE);
    rt_hw_interrupt_enable(irq_level);
}

static const struct rt_mmcsd_host_ops ops = {
    kd_mmc_request,
    kd_set_iocfg,
    RT_NULL,
    kd_enable_sdio_irq,
    sdhci_execute_tuning_cmd,
};

void kd_sdhci0_reset(int value)
{
    struct sdhci_host* host = sdhci_host0;

    uint16_t emmc_ctl = sdhci_readw(host, EMMC_CTRL_R);
    emmc_ctl |= (1 << EMMC_RST_N_OE);
    if (value)
        emmc_ctl |= (1 << EMMC_RST_N);
    else
        emmc_ctl &= ~(1 << EMMC_RST_N);
    sdhci_writeb(host, emmc_ctl, EMMC_CTRL_R);
}

static struct rt_mmcsd_host *kd_sdhci_get_host(int id)
{
    if (id == 0 && sdhci_host0)
        return sdhci_host0->host;
    if (id == 1 && sdhci_host1)
        return sdhci_host1->host;
    return RT_NULL;
}

void kd_sdhci_change(int id)
{
    struct rt_mmcsd_host *host = kd_sdhci_get_host(id);

    if (host)
        mmcsd_change(host);
    else
        LOG_W("SDIO%d host is not initialized", id);
}

int kd_sdhci_wait_card(int id, int timeout)
{
    struct rt_mmcsd_host *host = kd_sdhci_get_host(id);

    if (!host)
        return -RT_EINVAL;
    return mmcsd_wait_host_ready(host, timeout);
}

#ifdef RT_USING_SDIO0
static rt_err_t kd_sdhci_init_host0(void *hi_sys_virt_addr)
{
    uint32_t val;
    rt_err_t ret;
    struct sdhci_host *sdhci;
    struct rt_mmcsd_host *mmcsd;

    val = readl(hi_sys_virt_addr + 0);
    val |= 1 << 6 | 1 << 4;
    writel(val, hi_sys_virt_addr + 0);

    sdhci = rt_malloc(sizeof(*sdhci));
    if (!sdhci)
        return -RT_ENOMEM;

    rt_memset(sdhci, 0, sizeof(*sdhci));
    sdhci->mapbase = (void*)rt_ioremap((void*)SDEMMC0_BASE, 0x1000);
    if (!sdhci->mapbase) {
        ret = -RT_ENOMEM;
        goto free_sdhci;
    }

    sdhci->index = 0;
    sdhci->have_phy = 1;
    sdhci->mshc_ctrl_r = 0;
    sdhci->rx_delay_line = 0x0d;
#ifdef RT_SDIO0_EMMC
    sdhci->is_emmc_card = 1;
#else
    sdhci->is_emmc_card = 0;
#endif
    sdhci->tx_delay_line = 0xb0;
    /* sdhci_init() resets the controller before applying these settings. */
#ifdef RT_SDIO0_1V8
    sdhci->io_fixed_1v8 = 1;
#else
    sdhci->io_fixed_1v8 = 0;
#endif
    sdhci->max_clk = SDHCI0_BASE_CLOCK;

    ret = sdhci_init(sdhci);
    if (ret != RT_EOK)
        goto unmap_sdhci;

    sdhci->sdma_bounce = rt_malloc_align(SDHCI_SMALL_BOUNCE_SIZE,
                                          CACHE_LINESIZE);
    if (!sdhci->sdma_bounce) {
        ret = -RT_ENOMEM;
        goto unmap_sdhci;
    }

    mmcsd = mmcsd_alloc_host();
    if (!mmcsd) {
        ret = -RT_ENOMEM;
        goto unmap_sdhci;
    }

    mmcsd->ops = &ops;
    mmcsd->freq_min = SDHCI_CARD_MIN_CLOCK;
    mmcsd->freq_max = SDHCI0_CARD_MAX_CLOCK;
    strncpy(mmcsd->name, "sd0", sizeof(mmcsd->name) - 1);
    mmcsd->flags = SDIO0_BUS_WIDTH_FLAGS | MMCSD_MUTBLKWRITE |
                   MMCSD_SUP_HIGHSPEED | MMCSD_SUP_SDIO_IRQ;
    if (sdhci->is_emmc_card) {
        mmcsd->flags |= MMCSD_SUP_NONREMOVABLE;
#if 0 // K230 ddr52 or ddr25 seems not stable.
        if (sdhci->io_fixed_1v8)
            mmcsd->flags |= MMCSD_SUP_DDR_1V8;
        else
            mmcsd->flags |= MMCSD_SUP_DDR_3V3;
#endif

#ifdef RT_SDIO0_HS200
        mmcsd->flags |= MMCSD_SUP_HS200_1V8;
#endif
    }
    mmcsd->valid_ocr = sdhci->io_fixed_1v8 ?
                       VDD_165_195 : VDD_32_33 | VDD_33_34;
    mmcsd->max_seg_size = sdhci->is_emmc_card ?
                          4096U * 512U : 512U * 512U;
    mmcsd->max_dma_segs = 1;
    mmcsd->max_blk_size = 512;
    mmcsd->max_blk_count = 4096;
    mmcsd->private_data = sdhci;
    sdhci->host = mmcsd;

    ret = rt_event_init(&sdhci->event, "sd0_event", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
        goto free_mmcsd;
    rt_hw_interrupt_install(IRQN_SD0, sdhci_irq, sdhci, "sd0");
    rt_hw_interrupt_umask(IRQN_SD0);

    sdhci_host0 = sdhci;
    return RT_EOK;

free_mmcsd:
    mmcsd_free_host(mmcsd);
unmap_sdhci:
    if (sdhci->sdma_bounce)
        rt_free_align(sdhci->sdma_bounce);
    rt_iounmap(sdhci->mapbase);
free_sdhci:
    rt_free(sdhci);
    return ret;
}
#endif

#ifdef RT_USING_SDIO1
static rt_err_t kd_sdhci_init_host1(void *hi_sys_virt_addr)
{
    uint32_t val;
    rt_err_t ret;
    struct sdhci_host *sdhci;
    struct rt_mmcsd_host *mmcsd;

    val = readl(hi_sys_virt_addr + 8);
    val |= 1 << 2 | 1 << 0;
    writel(val, hi_sys_virt_addr + 8);

    sdhci = rt_malloc(sizeof(*sdhci));
    if (!sdhci)
        return -RT_ENOMEM;

    rt_memset(sdhci, 0, sizeof(*sdhci));
    sdhci->mapbase = (void*)rt_ioremap((void*)SDEMMC1_BASE, 0x1000);
    if (!sdhci->mapbase) {
        ret = -RT_ENOMEM;
        goto free_sdhci;
    }

    sdhci->index = 1;
    sdhci->have_phy = 0;
    sdhci->mshc_ctrl_r = 0;
    sdhci->rx_delay_line = 0;
    sdhci->tx_delay_line = 0;
    sdhci->max_clk = SDHCI1_BASE_CLOCK;

    ret = sdhci_init(sdhci);
    if (ret != RT_EOK)
        goto unmap_sdhci;

    sdhci->sdma_bounce = rt_malloc_align(SDHCI_SMALL_BOUNCE_SIZE,
                                          CACHE_LINESIZE);
    if (!sdhci->sdma_bounce) {
        ret = -RT_ENOMEM;
        goto unmap_sdhci;
    }

    mmcsd = mmcsd_alloc_host();
    if (!mmcsd) {
        ret = -RT_ENOMEM;
        goto unmap_sdhci;
    }

    strncpy(mmcsd->name, "sd1", sizeof(mmcsd->name) - 1);
    mmcsd->ops = &ops;
    mmcsd->freq_min = SDHCI_CARD_MIN_CLOCK;
    mmcsd->freq_max = SDHCI1_CARD_MAX_CLOCK;
    mmcsd->valid_ocr = VDD_32_33 | VDD_33_34;
    mmcsd->flags = MMCSD_BUSWIDTH_4 | MMCSD_MUTBLKWRITE |
                   MMCSD_SUP_HIGHSPEED | MMCSD_SUP_SDIO_IRQ;
    mmcsd->max_seg_size = 512U * 512U;
    mmcsd->max_dma_segs = 1;
    mmcsd->max_blk_size = 512;
    mmcsd->max_blk_count = 4096;
    mmcsd->private_data = sdhci;
    sdhci->host = mmcsd;

    ret = rt_event_init(&sdhci->event, "sd1_event", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
        goto free_mmcsd;
    rt_hw_interrupt_install(IRQN_SD1, sdhci_irq, sdhci, "sd1");
    rt_hw_interrupt_umask(IRQN_SD1);

    sdhci_host1 = sdhci;
    return RT_EOK;

free_mmcsd:
    mmcsd_free_host(mmcsd);
unmap_sdhci:
    if (sdhci->sdma_bounce)
        rt_free_align(sdhci->sdma_bounce);
    rt_iounmap(sdhci->mapbase);
free_sdhci:
    rt_free(sdhci);
    return ret;
}
#endif

rt_int32_t kd_sdhci_init(void)
{
    rt_err_t ret = RT_EOK;
    void *hi_sys_virt_addr;

    hi_sys_virt_addr = rt_ioremap((void*)0x91585000, 0x10);
    if (!hi_sys_virt_addr)
        return -RT_ENOMEM;

#ifdef RT_USING_SDIO0
    ret = kd_sdhci_init_host0(hi_sys_virt_addr);
#endif
#ifdef RT_USING_SDIO1
    if (ret == RT_EOK)
        ret = kd_sdhci_init_host1(hi_sys_virt_addr);
#endif

    rt_iounmap(hi_sys_virt_addr);
    return ret;
}
INIT_DEVICE_EXPORT_SEQ(kd_sdhci_init, 200);

#endif /*defined(RT_USING_SDIO0) || defined(RT_USING_SDIO1)*/

#endif /*defined(RT_USING_SDIO)*/
