/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ts_hw.h"

#include <rtthread.h>
#include "board.h"
#include "ioremap.h"
#include <riscv_io.h>

/* sys_ctl ts_controller: TSENW configures the sensor, TSENR holds the
 * conversion latched on each DATA_VALID rising edge. */
#define REG_TSENW 0x000
#define REG_TSENR 0x004

#define TSENW_EN         (1U << 0)
#define TSENW_CONTINUOUS (1U << 1)
#define TSENW_TRIM_POS   2
#define TSENW_TRIM_MASK  (0xFU << TSENW_TRIM_POS)
#define TSENW_TEST_EN    (1U << 6) /* analog test output; must stay 0 */

#define TSENR_DOUT_MASK  0xFFFU
#define TSENR_DOUT_VALID (1U << 12) /* cleared by reading TSENR */

static void *ts_base;

int ts_hw_init(void)
{
    if (!ts_base)
        ts_base = rt_ioremap((void *)TS_BASE_ADDR, TS_IO_SIZE);
    return ts_base ? RT_EOK : -RT_ENOMEM;
}

static uint32_t ts_readl(uint32_t offset)
{
    return readl((uint8_t *)ts_base + offset);
}

static void ts_writel(uint32_t value, uint32_t offset)
{
    writel(value, (uint8_t *)ts_base + offset);
}

void ts_hw_configure(uint8_t trim, bool continuous)
{
    uint32_t ctrl;

    if (!ts_base)
        return;
    ctrl = ts_readl(REG_TSENW) & ~(TSENW_EN | TSENW_TEST_EN);
    ts_writel(ctrl, REG_TSENW);
    ctrl &= ~(TSENW_TRIM_MASK | TSENW_CONTINUOUS);
    ctrl |= ((uint32_t)trim << TSENW_TRIM_POS) & TSENW_TRIM_MASK;
    if (continuous)
        ctrl |= TSENW_CONTINUOUS;
    ts_writel(ctrl, REG_TSENW);
    (void)ts_readl(REG_TSENR);
}

void ts_hw_enable(void)
{
    if (ts_base)
        ts_writel(ts_readl(REG_TSENW) | TSENW_EN, REG_TSENW);
}

void ts_hw_disable(void)
{
    if (ts_base)
        ts_writel(ts_readl(REG_TSENW) & ~TSENW_EN, REG_TSENW);
}

bool ts_hw_enabled(void)
{
    return ts_base && (ts_readl(REG_TSENW) & TSENW_EN);
}

uint32_t ts_hw_ctrl(void)
{
    return ts_base ? ts_readl(REG_TSENW) : 0;
}

/* VALID clears on read, so test it and take DOUT from the same read. */
static bool ts_hw_poll(uint16_t *raw)
{
    uint32_t value;

    if (!ts_base)
        return false;
    value = ts_readl(REG_TSENR);
    if (!(value & TSENR_DOUT_VALID))
        return false;
    *raw = value & TSENR_DOUT_MASK;
    return true;
}

int ts_hw_wait(uint16_t *raw, uint32_t timeout_ms, void (*wait_1ms)(void))
{
    if (!ts_base)
        return -RT_ENOSYS;
    if (!raw || !wait_1ms)
        return -RT_EINVAL;
    for (uint32_t waited = 0;; waited++) {
        if (ts_hw_poll(raw))
            return RT_EOK;
        if (waited == timeout_ms)
            return -RT_ETIMEOUT;
        wait_1ms();
    }
}

int ts_hw_read_single(int32_t *millicelsius, void (*wait_1ms)(void))
{
    uint16_t raw;
    int ret;

    if (!ts_base)
        return -RT_ENOSYS;
    /* Check before enabling, so a bad call leaves the sensor off. */
    if (!millicelsius || !wait_1ms)
        return -RT_EINVAL;
    /* Drop a result latched before this read; single mode then converts
     * once per enable. */
    (void)ts_readl(REG_TSENR);
    ts_hw_enable();
    ret = ts_hw_wait(&raw, TS_HW_FIRST_CONVERSION_MS, wait_1ms);
    ts_hw_disable();
    if (ret == RT_EOK)
        *millicelsius = ts_hw_raw_to_mc(raw);
    return ret;
}

/* Calibration polynomial from the Kendryte SDK. */
int32_t ts_hw_raw_to_mc(uint16_t raw)
{
    double x = raw;
    double c = (((1.01472e-10 * x - 1.10063e-6) * x + 4.36150e-3) * x - 7.10128) * x + 3565.87;

    return (int32_t)(c * 1000.0 + (c < 0 ? -0.5 : 0.5));
}
