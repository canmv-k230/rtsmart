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
#include "drv_adc.h"

#include "ioremap.h"
#include "rtdef.h"
#include "rtdevice.h"
#include "rtthread.h"

#include "board.h"
#include "iopoll.h"
#include "riscv_io.h"
#include "tick.h"

#include "sysctl_clk.h"
#include "sysctl_rst.h"

#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif

#define DBG_COLOR
#define DBG_TAG "drv_adc"
#include <rtdbg.h>

struct adc_inst {
    struct rt_adc_device device;
    struct rt_mutex      mutex;

    struct k_adc_reg* reg; // ADC_BASE_ADDR

    int inited;
};

static struct adc_inst k230_adc_inst;

rt_bool_t k230_adc_acodec_hp_detect_enabled(void)
{
#ifdef RT_ADC_ENABLE_ACODEC_HP_DETECT
    return RT_TRUE;
#else
    return RT_FALSE;
#endif
}

rt_uint32_t k230_adc_acodec_hp_detect_channel(void)
{
#ifdef RT_ADC_ACODEC_HP_DETECT_CHANNEL
    return RT_ADC_ACODEC_HP_DETECT_CHANNEL;
#else
    return 0;
#endif
}

static int k230_adc_init(void)
{
    int ret;

    struct k_adc_trim_reg trim;
    struct k_adc_mode_reg mode;

    if (NULL == k230_adc_inst.reg) {
        LOG_E("invalid inst");
        return -1;
    }

    rt_mutex_take(&k230_adc_inst.mutex, RT_WAITING_FOREVER);

#if 0
    sysctl_clk_set_leaf_en(SYSCTL_CLK_ADC_PCLK_GATE, 0);
    cpu_ticks_delay_us(1);
    sysctl_clk_set_leaf_en(SYSCTL_CLK_ADC_PCLK_GATE, 1);
    cpu_ticks_delay_us(1);

    // enable adc
    trim.data       = readl(&k230_adc_inst.reg->trim);
    trim.bits.enadc = 1;
    writel(trim.data, &k230_adc_inst.reg->trim);

    // reset adc, wait 100us
    sysctl_reset(SYSCTL_RESET_ADC);
    sysctl_reset(SYSCTL_RESET_ADC_APB);
    cpu_ticks_delay_us(100);
#endif

    // disable adc
    trim.data       = readl(&k230_adc_inst.reg->trim);
    trim.bits.enadc = 0;
    writel(trim.data, &k230_adc_inst.reg->trim);

    // enable adc
    trim.data       = readl(&k230_adc_inst.reg->trim);
    trim.bits.enadc = 1;
    writel(trim.data, &k230_adc_inst.reg->trim);

    // enable calibration
    trim.data          = readl(&k230_adc_inst.reg->trim);
    trim.bits.enadc    = 1;
    trim.bits.oscal_en = 1;
    writel(trim.data, &k230_adc_inst.reg->trim);

    // wait calibration done
#if 1
    cpu_ticks_delay_us(200);
#else
    ret = readl_poll_timeout(&k230_adc_inst.reg->trim, trim.data, (trim.bits.oscal_done), 1, 200);
    if (0x00 != ret) {
        LOG_E("ADC OS Cal failed: timeout");
        // rt_mutex_release(&k230_adc_inst.mutex);

        // return ret;
    }
#endif

    // disable calibration
    trim.data          = readl(&k230_adc_inst.reg->trim);
    trim.bits.oscal_en = 0;
    writel(trim.data, &k230_adc_inst.reg->trim);

    // set mode, one shot mode.
    mode.data          = readl(&k230_adc_inst.reg->mode);
    mode.bits.mode_sel = 0x00;
    writel(mode.data, &k230_adc_inst.reg->mode);

    rt_mutex_release(&k230_adc_inst.mutex);

    return 0;
}

static rt_err_t k230_adc_read(rt_uint32_t channel, rt_uint32_t* value)
{
    int ret;

    struct k_adc_cfg_reg  cfg;
    struct k_adc_data_reg data;

    if (value) {
        *value = 0;
    }

    if (K_ADC_MAX_CHANNEL <= channel) {
        return -1;
    }

    if (0x00 == k230_adc_inst.inited) {
        return -1;
    }

    rt_mutex_take(&k230_adc_inst.mutex, RT_WAITING_FOREVER);

    ret = readl_poll_timeout(&k230_adc_inst.reg->cfg, cfg.data, (0x00 == cfg.bits.busy), 1, 200);
    if (0x00 != ret) {
        LOG_E("Wait adc done timeout 1, %d", ret);
        rt_mutex_release(&k230_adc_inst.mutex);

        return ret;
    }

    cfg.data               = 0;
    cfg.bits.start_of_conv = 1;
    cfg.bits.in_sel        = channel;
    writel(cfg.data, &k230_adc_inst.reg->cfg);

    uint64_t start = cpu_ticks_us();

    ret = readl_poll_timeout(&k230_adc_inst.reg->cfg, cfg.data, cfg.bits.outen && cfg.bits.end_of_conv, 1, 200);
    if (0x00 != ret) {
        LOG_E("Wait adc done timeout 2, %d", ret);
        rt_mutex_release(&k230_adc_inst.mutex);

        return ret;
    }

    data.data = readl(&k230_adc_inst.reg->data[channel]);
    if (value) {
        *value = data.bits.data;
    }

    rt_mutex_release(&k230_adc_inst.mutex);

    return RT_EOK;
}

static rt_err_t k230_adc_enabled(struct rt_adc_device* device, rt_uint32_t channel, rt_bool_t enabled)
{
    (void)device;
    (void)channel;
    (void)enabled;

    // we use oneshot mode to read adc, so no need to enable or disable.

    return RT_EOK;
}

static rt_err_t k230_adc_convert(struct rt_adc_device* device, rt_uint32_t channel, rt_uint32_t* value)
{
    (void)device;

    return k230_adc_read(channel, value);
}

static struct rt_adc_ops k230_adc_ops = {
    .enabled = k230_adc_enabled,
    .convert = k230_adc_convert,
};

int k230_adc_dev_init(void)
{
    if (0x00 != k230_adc_inst.inited) {
        return 0;
    }

    rt_memset(&k230_adc_inst, 0x00, sizeof(k230_adc_inst));

    if (RT_EOK != rt_mutex_init(&k230_adc_inst.mutex, "k230_adc", RT_IPC_FLAG_FIFO)) {
        LOG_E("register adc device failed");

        goto _failed_init_mutex;
    }

    k230_adc_inst.reg = rt_ioremap_nocache((void*)ADC_BASE_ADDR, ADC_IO_SIZE);
    if (!k230_adc_inst.reg) {
        LOG_E("ioremap failed");

        goto _failed_remap;
    }

    if (0x00 != k230_adc_init()) {
        LOG_E("init failed");

        goto _failed_init;
    }

    if (RT_EOK != rt_hw_adc_register(&k230_adc_inst.device, "adc", &k230_adc_ops, NULL)) {
        LOG_E("Register adc failed");

        goto _failed_reg_device;
    }

    k230_adc_inst.inited = 1;

    return 0;

_failed_reg_device:
    LOG_W("TODO: deinit adc driver");
_failed_init:
    if (k230_adc_inst.reg) {
        rt_iounmap(k230_adc_inst.reg);
        k230_adc_inst.reg = NULL;
    }
_failed_remap:
    rt_mutex_detach(&k230_adc_inst.mutex);
_failed_init_mutex:

    return -1;
}
INIT_DEVICE_EXPORT(k230_adc_dev_init);
