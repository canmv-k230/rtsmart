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
#include "drv_i2c.h"
#include "drv_i2c_core.h"

#include "sysctl_clk.h"

#include <riscv_io.h>
#include <rthw.h>
#include <rtthread.h>
#include "tick.h"

#define DBG_TAG "i2c"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>


static unsigned long long dw_div_round_closest_ull(unsigned long long x,
                                                   unsigned long long divisor)
{
    return (x + divisor / 2ULL) / divisor;
}

#define DIV_ROUND_CLOSEST_ULL(x, divisor) \
    dw_div_round_closest_ull((unsigned long long)(x), (unsigned long long)(divisor))

#define DW_STATUS_ACTIVE            (1U << 0)
#define DW_STATUS_MASK              (DW_STATUS_ACTIVE)

/* Internal err_state bits (not HW bits). */
#define DW_I2C_CMD_ERR_TX_ABRT      0x1U
#define DW_I2C_CMD_ERR_ADDR_INVALID 0x2U

static void k230_i2c_clear_all_irqs(struct dw_i2c_master *master);
rt_err_t k230_i2c_config_speed(struct dw_i2c_master *master,
                               rt_uint32_t speed_hz);

static void k230_i2c_config_fifo_size(struct dw_i2c_master *master)
{
    struct dw_ic_comp_param_1_t param;
    rt_uint32_t hw_tx = 0, hw_rx = 0;

    param.reg = readl(&master->regs->comp_param1.reg);

    if (param.reg != 0) {
        hw_tx = param.bits.TX_BUFFER_DEPTH + 1U;
        hw_rx = param.bits.RX_BUFFER_DEPTH + 1U;
    }

    if (hw_tx < 2U || hw_rx < 2U) {
        hw_tx = DW_I2C_DEFAULT_FIFO_DEPTH;
        hw_rx = DW_I2C_DEFAULT_FIFO_DEPTH;
    }

    master->tx_fifo_depth = hw_tx;
    master->rx_fifo_depth = hw_rx;
}

/* Wait until the I2C bus is idle, i.e. IC_STATUS.ACTIVITY is 0. */
static rt_err_t k230_i2c_wait_bus_idle(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_status_t status;
    rt_int32_t timeout = 20; /* ~20ms total, 1ms per step */

    while (timeout-- > 0) {
        status.reg = readl(&regs->ic_status.reg);
        if (status.bits.ACTIVITY == 0U)
            return RT_EOK;

        rt_thread_mdelay(1);
    }

    LOG_W("i2c: timeout waiting for bus ready");
    if (k230_i2c_recover_bus(master) == RT_EOK) {
        status.reg = readl(&regs->ic_status.reg);
        if (status.bits.ACTIVITY == 0U)
            return RT_EOK;
    }
    return -RT_EBUSY;
}

/* Translate TX_ABRT_SOURCE into a local error code. */
static rt_err_t k230_i2c_map_abort(struct dw_i2c_master *master)
{
    struct dw_ic_tx_abrt_source_t src;

    src.reg = master->abrt_src;

    if (src.bits.ABRT_7B_ADDR_NOACK   ||
        src.bits.ABRT_10ADDR1_NOACK   ||
        src.bits.ABRT_10ADDR2_NOACK   ||
        src.bits.ABRT_TXDATA_NOACK    ||
        src.bits.ABRT_GCALL_NOACK)
        return -RT_EIO;

    if (src.bits.ABRT_ARB_LOST)
        return -RT_EBUSY;

    if (src.bits.ABRT_GCALL_READ)
        return -RT_EINVAL;

    return -RT_EIO;
}
/*
 * Fully disable the controller, including "master on hold" situations:
 *  - if MST_ON_HOLD or MASTER_HOLD_TX_FIFO_EMPTY is set, issue an
 *    ABORT and wait for it to clear
 *  - then repeatedly clear ENABLE until ENABLE_STATUS bit0 goes low
 */
static void k230_i2c_disable_safe(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_intr_t raw_intr;
    struct dw_ic_status_t status;
    struct dw_ic_enable_t en;
    struct dw_ic_enable_status_t en_st;
    rt_int32_t timeout;

    raw_intr.reg = readl(&regs->ic_raw_intr_stat.reg);
    status.reg = readl(&regs->ic_status.reg);
    en.reg = readl(&regs->ic_enable.reg);

    if (raw_intr.bits.MST_ON_HOLD ||
        status.bits.MST_HOLD_TX_FIFO_EMPTY) {
        if (!en.bits.ENABLE) {
            en.bits.ENABLE = 1;
            writel(en.reg, &regs->ic_enable.reg);
            /* wait for ENABLE to latch, ~25us is enough here */
            cpu_ticks_delay_us(25);
        }

        /* request abort of the current transfer */
        en.bits.ABORT = 1;
        writel(en.reg, &regs->ic_enable.reg);

        timeout = 100;
        while (timeout-- > 0) {
            en.reg = readl(&regs->ic_enable.reg);
            if (!en.bits.ABORT)
                break;
            cpu_ticks_delay_us(10);
        }

        if (en.bits.ABORT)
            LOG_W("i2c: timeout while trying to abort current transfer");
    }

    timeout = 100;
    do {
        en.reg = 0;
        writel(en.reg, &regs->ic_enable.reg);
        master->status &= ~DW_STATUS_ACTIVE;
        /*
         * The enable status register may be unimplemented; in that
         * case reading zero will terminate the loop.
         */
        en_st.reg = readl(&regs->ic_enable_status.reg);
        if (!en_st.bits.IC_EN)
            return;

        cpu_ticks_delay_us(25);
    } while (timeout-- > 0);

    LOG_W("i2c: timeout in disabling controller");
}

static rt_err_t k230_i2c_reinit_master(struct dw_i2c_master *master);

__attribute__((weak)) rt_err_t k230_i2c_recover_bus(struct dw_i2c_master *master)
{
    return k230_i2c_reinit_master(master);
}

static void k230_i2c_disable_nowait(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_enable_t en;

    en.reg = 0;
    writel(en.reg, &regs->ic_enable.reg);
    master->status &= ~DW_STATUS_ACTIVE;
}

static rt_bool_t k230_i2c_is_master_active(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_status_t status;
    rt_int32_t timeout = 20; /* ~20ms total, 1.1ms per step */

    status.reg = readl(&regs->ic_status.reg);
    if (status.bits.MST_ACTIVITY == 0U)
        return RT_FALSE;

    while (timeout-- > 0) {
        rt_thread_mdelay(1);
        status.reg = readl(&regs->ic_status.reg);
        if (status.bits.MST_ACTIVITY == 0U)
            return RT_FALSE;
    }

    return RT_TRUE;
}

static rt_err_t k230_i2c_reinit_master(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_con_t con;
    rt_err_t ret;
    rt_uint32_t speed;

    k230_i2c_ctrl_enable(master, RT_FALSE);
    writel(0, &regs->ic_intr_mask.reg);
    k230_i2c_clear_all_irqs(master);
    writel(0, &regs->ic_smbus_intr_mask.reg);

    con.reg = readl(&regs->ic_con.reg);
    con.bits.MASTER_MODE = 1U;
    con.bits.IC_SLAVE_DISABLE = 1U;
    con.bits.IC_RESTART_EN = 1U;
    writel(con.reg, &regs->ic_con.reg);

    speed = master->last_speed ? master->last_speed : DW_I2C_SPEED_FAST;
    ret = k230_i2c_config_speed(master, speed);
    if (ret != RT_EOK)
        return ret;

    writel(0, &regs->ic_rx_tl.reg);
    writel(master->tx_fifo_depth / 2U, &regs->ic_tx_tl.reg);

    return RT_EOK;
}

/* Compute SCL HCNT/LCNT from timing parameters (ns). */
static void k230_i2c_calc_scl_ticks(rt_uint32_t ic_clk_hz,
                                  rt_uint32_t t_high_ns,
                                  rt_uint32_t t_low_ns,
                                  rt_uint32_t fall_high_ns,
                                  rt_uint32_t fall_low_ns,
                                  rt_uint32_t *hcnt,
                                  rt_uint32_t *lcnt)
{
    unsigned long long num;
    const unsigned long long den = 1000000000ULL; /* 1e9, Hz * ns → cycles */
    rt_uint32_t h, l;

    num = (unsigned long long)ic_clk_hz *
          (unsigned long long)(t_high_ns + fall_high_ns);
    h = (rt_uint32_t)DIV_ROUND_CLOSEST_ULL(num, den);
    h = (h > 3U) ? (h - 3U) : 1U;

    num = (unsigned long long)ic_clk_hz *
          (unsigned long long)(t_low_ns + fall_low_ns);
    l = (rt_uint32_t)DIV_ROUND_CLOSEST_ULL(num, den);
    l = (l > 1U) ? (l - 1U) : 1U;

    *hcnt = h;
    *lcnt = l;
}

rt_err_t k230_i2c_config_speed(struct dw_i2c_master *master,
                               rt_uint32_t speed_hz)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    rt_uint32_t clock;
    rt_uint32_t hcnt = 0, lcnt = 0;
    rt_uint32_t t_high_ns, t_low_ns;
    rt_uint32_t spklen_cnt;
    rt_uint32_t add_h = 0, add_l = 0, over = 0;
    unsigned long long target = 0;
    unsigned long long total = 0;
    unsigned long long diff = 0;
    struct dw_ic_con_t con;
    struct dw_ic_enable_t enable;

    if (!speed_hz)
        return -RT_EINVAL;

    clock = sysctl_clk_get_leaf_freq(SYSCTL_CLK_I2C_0_CLK +
                                     master->index);
    if (!clock)
        return -RT_ERROR;

    enable.reg = readl(&regs->ic_enable.reg);
    if (enable.bits.ENABLE) {
        return -RT_EBUSY;
    }

    con.reg = readl(&regs->ic_con.reg);

    spklen_cnt = (rt_uint32_t)DIV_ROUND_CLOSEST_ULL(
        (unsigned long long)clock * 10ULL, 1000000000ULL);

    if (spklen_cnt == 0U)
        spklen_cnt = 1U;
    if (spklen_cnt > 0xffU)
        spklen_cnt = 0xffU;

    if (speed_hz <= DW_I2C_SPEED_STANDARD) {
        t_high_ns = 4000U;
        t_low_ns  = 4700U;
        con.bits.SPEED = 1U; /* STANDARD */
    } else if (speed_hz <= DW_I2C_SPEED_FAST) {
        t_high_ns = 600U;
        t_low_ns  = 1300U;
        con.bits.SPEED = 2U; /* FAST */
    } else if (speed_hz <= DW_I2C_SPEED_HIGH) {
        t_high_ns = 60U;
        t_low_ns  = 160U;
        con.bits.SPEED = 3U; /* HIGH */
    } else {
        return -RT_EINVAL;
    }

#if 0
    /* old style (kept as backup) */
    {
        rt_uint32_t period;
        rt_uint32_t fall_cnt = spklen_cnt;

        period = clock / speed_hz;
        period = period - spklen_cnt - 7U - 1U - fall_cnt;
        lcnt = period / 2U;
        hcnt = period - lcnt;
    }
#else
    k230_i2c_calc_scl_ticks(clock,
                            t_high_ns,
                            t_low_ns,
                            300U,
                            300U,
                            &hcnt,
                            &lcnt);

    if (hcnt < (spklen_cnt + 5U))
        hcnt = spklen_cnt + 5U;
    if (lcnt < (spklen_cnt + 7U))
        lcnt = spklen_cnt + 7U;

    target = ((unsigned long long)clock + speed_hz - 1ULL) /
             (unsigned long long)speed_hz;
    total = (unsigned long long)(hcnt + 3U) +
            (unsigned long long)(lcnt + 1U);
    if (total < target) {
        diff = target - total;
        add_h = (rt_uint32_t)(diff / 2ULL);
        add_l = (rt_uint32_t)(diff - add_h);

        hcnt += add_h;
        lcnt += add_l;

        if (hcnt > 0xffffU) {
            over = hcnt - 0xffffU;
            hcnt = 0xffffU;
            lcnt += over;
        }
        if (lcnt > 0xffffU)
            lcnt = 0xffffU;
    }
#endif

    if (con.bits.SPEED == 3U) {
        writel(spklen_cnt, &regs->ic_hs_spklen.reg);
        writel(hcnt, &regs->ic_hs_scl_hcnt.reg);
        writel(lcnt, &regs->ic_hs_scl_lcnt.reg);
    } else {
        writel(spklen_cnt, &regs->ic_fs_spklen.reg);
        if (con.bits.SPEED == 1U) {
            writel(hcnt, &regs->ic_ss_scl_hcnt.reg);
            writel(lcnt, &regs->ic_ss_scl_lcnt.reg);
        } else {
            writel(hcnt, &regs->ic_fs_scl_hcnt.reg);
            writel(lcnt, &regs->ic_fs_scl_lcnt.reg);
        }
    }

    writel(0, &regs->ic_sda_hold.reg);
    writel(con.reg, &regs->ic_con.reg);

    master->last_speed = speed_hz;

    return RT_EOK;
}

void k230_i2c_ctrl_enable(struct dw_i2c_master *master, rt_bool_t enable)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_enable_t en;

    if (enable) {
        master->status |= DW_STATUS_ACTIVE;
        en.reg = 0;
        en.bits.ENABLE = 1;
        writel(en.reg, &regs->ic_enable.reg);
    } else {
        k230_i2c_disable_safe(master);
    }
}

static void k230_i2c_clear_all_irqs(struct dw_i2c_master *master)
{
    (void)readl(&master->regs->ic_clr_intr.reg);
}

static rt_uint32_t k230_i2c_read_clear_irqs(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    rt_uint32_t stat = readl(&regs->ic_intr_stat.reg);
    rt_uint32_t dummy;
    struct dw_ic_intr_t intr;

    intr.reg = stat;

    if (intr.bits.RX_UNDER)
        dummy = readl(&regs->ic_clr_rx_under.reg);
    if (intr.bits.RX_OVER)
        dummy = readl(&regs->ic_clr_rx_over.reg);
    if (intr.bits.TX_OVER)
        dummy = readl(&regs->ic_clr_tx_over.reg);
    if (intr.bits.RD_REQ)
        dummy = readl(&regs->ic_clr_rd_req.reg);
    if (intr.bits.TX_ABRT) {
        /* Preserve abort source before clearing it */
        master->abrt_src = readl(&regs->ic_tx_abrt_source.reg);
        dummy = readl(&regs->ic_clr_tx_abrt.reg);
    }
    if (intr.bits.RX_DONE)
        dummy = readl(&regs->ic_clr_rx_done.reg);
    if (intr.bits.ACTIVITY)
        dummy = readl(&regs->ic_clr_activity.reg);
    if (intr.bits.STOP_DET &&
        ((master->rx_pending == 0) ||
         intr.bits.RX_FULL))
        dummy = readl(&regs->ic_clr_stop_det.reg);
    if (intr.bits.START_DET)
        dummy = readl(&regs->ic_clr_start_det.reg);
    if (intr.bits.GEN_CALL)
        dummy = readl(&regs->ic_clr_gen_call.reg);

    return stat;
}

static void k230_i2c_fill_tx_fifo(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_con_t con;
    rt_uint32_t tx_space, rx_space;
    struct dw_ic_intr_t mask;
    rt_uint16_t addr = master->msgs[0].addr;

    con.reg = readl(&regs->ic_con.reg);

    for (; master->msg_wr_idx < master->msgs_num; master->msg_wr_idx++) {
        struct rt_i2c_msg *msg = &master->msgs[master->msg_wr_idx];
        rt_bool_t is_last_msg;
        rt_uint32_t j = master->buf_wr_idx;

        if (msg->addr != addr) {
            LOG_E("invalid msg address\n");
            master->err_state |= DW_I2C_CMD_ERR_ADDR_INVALID;
            break;
        }

        is_last_msg = (master->msg_wr_idx == (master->msgs_num - 1U)) ? RT_TRUE : RT_FALSE;;
        tx_space = master->tx_fifo_depth - readl(&regs->ic_txflr.reg);
        rx_space = master->rx_fifo_depth - readl(&regs->ic_rxflr.reg);

        if (msg->flags & RT_I2C_RD) {
            master->lst_rd_idx = (rt_int32_t)master->msg_wr_idx;
            /* read message is restriced by rx_space and tx_space */
            for (; j < msg->len && tx_space && rx_space; j++, tx_space--) {
                struct dw_ic_data_cmd_t data;

                /* Avoid RX FIFO overrun: do not queue more reads than depth */
                if (master->rx_pending >= master->rx_fifo_depth)
                    break;

                data.reg = 0;
                /* STOP on the last byte of the last message */
                if (is_last_msg && (j == (msg->len - 1U))) {
                    data.bits.STOP = 1;
                }

                data.bits.CMD = 1; /* READ */
                writel(data.reg, &regs->ic_cmd_data.reg);
                master->rx_pending++;
                rx_space--;
            }
        } else {
            rt_uint8_t *buf = &msg->buf[j];

            /* write message is not restriced by rx_space */
            for (; j < msg->len && tx_space; j++, tx_space--) {
                struct dw_ic_data_cmd_t data;

                data.reg = 0;
                if (is_last_msg && (j == (msg->len - 1U))) {
                    data.bits.STOP = 1;
                }

                data.bits.DAT = *buf++;
                /* CMD defaults to 0 => WRITE */
                writel(data.reg, &regs->ic_cmd_data.reg);
            }
        }

        master->buf_wr_idx = j;
        if (master->buf_wr_idx == msg->len) {
            master->buf_wr_idx = 0;
        } else {
            break;
        }
    }

    /* Update interrupt mask based on current transfer state */
    mask.reg = 0;
    mask.bits.RX_FULL  = 1;
    mask.bits.TX_ABRT  = 1;
    mask.bits.STOP_DET = 1;
    mask.bits.TX_EMPTY = 1;

    if (master->msg_wr_idx == master->msgs_num)
        mask.bits.TX_EMPTY = 0;

    if (master->err_state)
        mask.reg = 0;

    writel(mask.reg, &regs->ic_intr_mask.reg);
}

static void k230_i2c_drain_rx_fifo(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    rt_uint32_t available = readl(&regs->ic_rxflr.reg);

    while ((master->msg_rd_idx <= master->msg_wr_idx) &&
           (master->msg_rd_idx < master->msgs_num) &&
           available) {
        struct rt_i2c_msg *msg =
            &master->msgs[master->msg_rd_idx];

        if (!(msg->flags & RT_I2C_RD)) {
            master->msg_rd_idx++;
            continue;
        }

        rt_uint32_t j = master->buf_rd_idx;
        rt_uint8_t *buf = &msg->buf[j];

        for (; j < msg->len && available; j++, available--) {
            *buf++ = (rt_uint8_t)readl(&regs->ic_cmd_data.reg);
            if (master->rx_pending)
                master->rx_pending--;
        }

        master->buf_rd_idx = j;
        if (master->buf_rd_idx == msg->len) {
            master->buf_rd_idx = 0;
            master->msg_rd_idx++;
        }
    }
}

static void k230_i2c_service_irq(struct dw_i2c_master *master,
                                    rt_uint32_t stat)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_intr_t intr;

    intr.reg = stat;

    if (intr.bits.TX_ABRT) {
        /* Mark TX abort; final error is decided in k230_i2c_do_xfer() */
        master->err_state |= DW_I2C_CMD_ERR_TX_ABRT;
        master->status &= ~DW_STATUS_MASK;
        master->rx_pending = 0;
        /* Stop any further interrupts from this controller */
        writel(0, &regs->ic_intr_mask.reg);
        goto tx_aborted;
    }

    if (intr.bits.RX_FULL)
        k230_i2c_drain_rx_fifo(master);

    if (intr.bits.TX_EMPTY)
        k230_i2c_fill_tx_fifo(master);

tx_aborted:
    /*
     * Complete the transfer when either TX aborted or STOP detected
     * (or a message error is raised) and all expected RX bytes have
     * been drained (rx_pending == 0).
     */
    if (((intr.bits.TX_ABRT || intr.bits.STOP_DET) || master->err_state) &&
        (master->rx_pending == 0))
        rt_event_send(&master->event, 1);
}

static void k230_i2c_irq_handler(int irq, void *param)
{
    struct dw_i2c_master *master = param;
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_enable_t en;
    struct dw_ic_intr_t raw_intr;
    rt_uint32_t raw_val, stat;

    /*
     *  - ignore interrupts when controller is disabled;
     *  - ignore pure ACTIVITY interrupts.
     */
    en.reg = readl(&regs->ic_enable.reg);
    raw_val = readl(&regs->ic_raw_intr_stat.reg);
    raw_intr.reg = raw_val;

    if (!en.bits.ENABLE)
        return;

    /* Ignore pure ACTIVITY interrupts. */
    raw_intr.bits.ACTIVITY = 0;
    if (raw_intr.reg == 0U)
        return;

    /* Filter out spurious 0xFFFFFFFF patterns from misbehaving controllers */
    if (raw_val == 0xffffffffU)
        return;

    stat = k230_i2c_read_clear_irqs(master);

    /*
     * If the driver doesn't see the controller as active, treat this
     * as an unexpected interrupt and shut the source up by disabling
     * all interrupts.
     */
    if ((master->status & DW_STATUS_ACTIVE) == 0U) {
        writel(0, &regs->ic_intr_mask.reg);
        return;
    }

    k230_i2c_service_irq(master, stat);
}

rt_err_t k230_i2c_hw_init(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    struct dw_ic_con_t con;

    k230_i2c_ctrl_enable(master, RT_FALSE);
    writel(0, &regs->ic_intr_mask.reg);
    k230_i2c_clear_all_irqs(master);

    /* Mask SMBus-related interrupts by clearing IC_SMBUS_INTR_MASK (0xcc). */
    writel(0, &regs->ic_smbus_intr_mask.reg);

    /*
     * Configure as I2C master, 7-bit addr, fast mode, restart enabled.
     * Do not set STOP_DET_IFADDRESSED or TX_EMPTY_CTRL here; only
     * MASTER_MODE / SLAVE_DISABLE / RESTART_EN / SPEED bits are used.
     * Speed mode and SCL timings are refined later in k230_i2c_config_speed().
     */
    con.reg = 0;
    con.bits.MASTER_MODE      = 1;
    con.bits.IC_SLAVE_DISABLE = 1;
    con.bits.IC_RESTART_EN    = 1;
    con.bits.SPEED            = 2U; /* FAST as initial mode */
    writel(con.reg, &regs->ic_con.reg);

    k230_i2c_config_fifo_size(master);

    writel(0, &regs->ic_rx_tl.reg);
    writel(master->tx_fifo_depth / 2U, &regs->ic_tx_tl.reg);

    k230_i2c_config_speed(master, DW_I2C_SPEED_FAST);

    rt_event_init(&master->event, "i2c_evt", RT_IPC_FLAG_PRIO);

    rt_hw_interrupt_install(master->irq, k230_i2c_irq_handler,
                            master, master->name);
    rt_hw_interrupt_umask(master->irq);

    return RT_EOK;
}

static rt_err_t k230_i2c_do_xfer(struct dw_i2c_master *master)
{
    volatile struct dw_i2c_regs *regs = master->regs;
    rt_uint32_t val = 0;
    struct dw_ic_con_t con;
    struct dw_ic_tar_t tar;
    struct dw_ic_intr_t mask;
    rt_err_t ret;
    rt_bool_t tx_done;
    rt_bool_t rx_done;
    /* Wait until the bus is not busy before starting a new transfer */
    ret = k230_i2c_wait_bus_idle(master);
    if (ret != RT_EOK)
        return ret;

    master->msg_wr_idx = 0;
    master->buf_wr_idx = 0;
    master->msg_rd_idx = 0;
    master->buf_rd_idx = 0;
    master->rx_pending = 0;
    master->abrt_src = 0;
    master->err_state = 0;
    master->lst_rd_idx = -1;

    /* Disable the controller before programming it */
    k230_i2c_ctrl_enable(master, RT_FALSE);

    /* Program IC_CON.10BITADDR_MASTER when RT_I2C_ADDR_10BIT is set. */
    con.reg = readl(&regs->ic_con.reg);
    con.bits.IC_10BITADDR_MASTER = (master->msgs[0].flags & RT_I2C_ADDR_10BIT) ? 1U : 0U;
    writel(con.reg, &regs->ic_con.reg);

    /* Program target address (all messages must share same address) */
    tar.reg = 0;
    tar.bits.IC_TAR = master->msgs[0].addr & 0x3ffU;
    if (master->msgs[0].flags & RT_I2C_ADDR_10BIT)
        tar.bits.IC_10BITADDR_MASTER = 1;
    val = tar.reg;
    writel(tar.reg, &regs->ic_tar.reg);

    /* Enforce disabled interrupts before enabling the controller */
    writel(0, &regs->ic_intr_mask.reg);

    k230_i2c_ctrl_enable(master, RT_TRUE);

    /* Dummy read to avoid the register getting stuck on some controllers */
    (void)readl(&regs->ic_enable_status.reg);

    k230_i2c_clear_all_irqs(master);
    mask.reg = 0;
    mask.bits.RX_FULL  = 1;
    mask.bits.TX_ABRT  = 1;
    mask.bits.STOP_DET = 1;
    mask.bits.TX_EMPTY = 1;
    writel(mask.reg, &regs->ic_intr_mask.reg);

    ret = rt_event_recv(&master->event,
                        1,
                        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                        master->bus.timeout ? master->bus.timeout : RT_TICK_PER_SECOND,
                        RT_NULL);

    if (ret != RT_EOK) {
        k230_i2c_ctrl_enable(master, RT_FALSE);
        if (ret == -RT_ETIMEOUT) {
            LOG_E("i2c: xfer timeout, tar=0x%02X, bus %s, maybe dts is incorrect.", val, master->name);
            (void)k230_i2c_recover_bus(master);
        } else {
            LOG_E("i2c: xfer error %d", ret);
        }
        return ret;
    }

    if (k230_i2c_is_master_active(master))
        LOG_E("i2c: controller active");

    k230_i2c_disable_nowait(master);

    if (master->err_state & DW_I2C_CMD_ERR_ADDR_INVALID)
        return -RT_EINVAL;

    if (master->err_state & DW_I2C_CMD_ERR_TX_ABRT) {
        rt_err_t err = -RT_EIO;

        err = k230_i2c_map_abort(master);

        if (master->abrt_src)
            LOG_D("i2c: xfer abort, source=0x%08x",
                  master->abrt_src);
        return err;
    }

    tx_done = (master->msg_wr_idx == master->msgs_num) &&
        (master->buf_wr_idx == 0U);
    rx_done = ((rt_int32_t)master->msg_rd_idx > master->lst_rd_idx) &&
        (master->buf_rd_idx == 0U);

    if (!tx_done || !rx_done) {
        LOG_E("i2c: transfer terminated early");
        return -RT_EIO;
    }

    return RT_EOK;
}

static rt_size_t k230_i2c_bus_xfer(struct rt_i2c_bus_device *bus,
                                    struct rt_i2c_msg msgs[],
                                    rt_uint32_t num)
{
    struct dw_i2c_master *master = (struct dw_i2c_master *)bus;
    rt_err_t ret;

    if (!num || !msgs)
        return -RT_EINVAL;

    master->msgs = msgs;
    master->msgs_num = num;

    ret = k230_i2c_do_xfer(master);
    if (ret == RT_EOK)
        return num;

    return ret;
}

static rt_err_t k230_i2c_bus_ctrl(struct rt_i2c_bus_device *bus,
                                   rt_uint32_t cmd,
                                   rt_uint32_t arg)
{
    struct dw_i2c_master *master = (struct dw_i2c_master *)bus;

    switch (cmd) {
    case RT_I2C_DEV_CTRL_CLK:
        return k230_i2c_config_speed(master, arg);
    default:
        return -RT_EINVAL;
    }
}

const struct rt_i2c_bus_device_ops k230_i2c_bus_ops = {
    .master_xfer     = k230_i2c_bus_xfer,
    .slave_xfer      = RT_NULL,
    .i2c_bus_control = k230_i2c_bus_ctrl,
};
