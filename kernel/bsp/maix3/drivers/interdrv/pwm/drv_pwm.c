/* Copyright (c) 2025, Canaan Bright Sight Co., Ltd
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
#include <stdbool.h>
#include <stdint.h>

#include <rtdef.h>
#include <rtdevice.h>
#include <rtthread.h>

#include "board.h"
#include "ioremap.h"
#include "lwp.h"
#include "lwp_user_mm.h"
#include "riscv_io.h"

#include "drv_pwm.h"
#include "sysctl_clk.h"

#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif

#define DBG_COLOR
#define DBG_TAG "pwm"
#include <rtdbg.h>

#define write32(addr, value) writel(value, (volatile void*)(rt_uint64_t)(addr))
#define read32(addr)         readl((const volatile void*)(rt_uint64_t)(addr))

/* PWM Register Definition */
typedef struct {
    uint32_t scale : 4; /* Clock scale factor */
    uint32_t reserve : 4;
    uint32_t sticky : 1; /* Sticky counter */
    uint32_t zerocmp : 1; /* Zero compare */
    uint32_t deglitch : 1; /* Deglitch enable */
    uint32_t reserve1 : 1;
    uint32_t enalways : 1; /* Always enable */
    uint32_t enoneshot : 1; /* One-shot mode */
    uint32_t reserve2 : 2;
    uint32_t cmp0center : 1; /* Center-aligned for channel 0 */
    uint32_t cmp1center : 1; /* Center-aligned for channel 1 */
    uint32_t cmp2center : 1; /* Center-aligned for channel 2 */
    uint32_t cmp3center : 1; /* Center-aligned for channel 3 */
    uint32_t reserve3 : 4;
    uint32_t cmp0gang : 1; /* Channel 0 gang mode */
    uint32_t cmp1gang : 1; /* Channel 1 gang mode */
    uint32_t cmp2gang : 1; /* Channel 2 gang mode */
    uint32_t cmp3gang : 1; /* Channel 3 gang mode */
    uint32_t cmp0ip : 1; /* Channel 0 interrupt pending */
    uint32_t cmp1ip : 1; /* Channel 1 interrupt pending */
    uint32_t cmp2ip : 1; /* Channel 2 interrupt pending */
    uint32_t cmp3ip : 1; /* Channel 3 interrupt pending */
} pwm_cfg_t;

typedef struct {
    volatile uint32_t pwmcfg; /* 0x00: PWM Configuration */
    volatile uint32_t reserved0;
    volatile uint32_t pwmcount; /* 0x08: PWM Counter */
    volatile uint32_t reserved1;
    volatile uint32_t pwms; /* 0x10: PWM Scaled Counter */
    volatile uint32_t reserved2[3];
    volatile uint32_t pwmcmp0; /* 0x20: PWM Compare 0 */
    volatile uint32_t pwm_chn_pulse[3]; /* 0x24-0x2C: Channel Pulse Width */
} kd_pwm_t;

/* PWM Instance Structure */
struct pwm_inst {
    volatile kd_pwm_t* reg; /* PWM register base */
    uint32_t           period; /* Current period in clock cycles */
    uint32_t           scale; /* Current clock scale factor */
    uint32_t           enable[3]; /* Channel enable status */
    uint32_t           pulses[3]; /* Current pulse width in clock cycles */
};

/* PWM Device Wrapper */
struct pwm_inst_wrap {
    struct rt_device_pwm device; /* RT-Thread PWM device */
    void*                inst_type; /* Instance type identifier */
    struct pwm_inst      pwm[2]; /* Two PWM instances (0 and 1) */
};

static uint32_t _pwm_dev_inst_type;

static struct pwm_inst_wrap pwm_inst_wrap;

static void kd_pwm_write_pulse(struct pwm_inst* inst, int channel,
                               uint32_t period, uint32_t pulse,
                               uint32_t scale);

/* PWM Channel Mapping */
#define PWM_CHANNEL_TO_INST(ch)  ((ch) / 3) /* 0-2: instance 0, 3-5: instance 1 */
#define PWM_CHANNEL_TO_SUBCH(ch) ((ch) % 3) /* Sub-channel within instance */

/**
 * @brief Start PWM output on specified channel
 * @param inst PWM instance
 * @param channel Channel number (0-2)
 * @return RT_EOK on success, error code on failure
 */
static int pwm_start(struct pwm_inst* inst, int channel)
{
    if (channel < 0 || channel > 2) {
        LOG_E("Invalid channel %d", channel);
        return -RT_EINVAL;
    }

    uint32_t curr_cfg;
    uint32_t period = inst->period;
    uint32_t scale  = inst->scale;
    uint32_t pulse  = inst->pulses[channel];

    if (period == 0) {
        LOG_E("Channel %d has no configured period", channel);
        return -RT_EINVAL;
    }

    /* Configure PWM scale and period */
    curr_cfg = read32(&inst->reg->pwmcfg);
    write32(&inst->reg->pwmcfg, (curr_cfg & (~0x0F) | scale));
    write32(&inst->reg->pwmcmp0, period >> scale);

    /* Set pulse width */
    kd_pwm_write_pulse(inst, channel, period, pulse, scale);

    /* Enable PWM */
    curr_cfg = read32(&inst->reg->pwmcfg);
    write32(&inst->reg->pwmcfg, curr_cfg | (1 << 12));
    inst->enable[channel] = 1;

    return RT_EOK;
}

/**
 * @brief Stop PWM output on specified channel
 * @param inst PWM instance
 * @param channel Channel number (0-2)
 * @return RT_EOK on success, error code on failure
 */
static int pwm_stop(struct pwm_inst* inst, int channel)
{
    uint32_t curr_cfg;

    if (channel < 0 || channel > 2) {
        LOG_E("Invalid channel %d", channel);
        return -RT_EINVAL;
    }

    inst->enable[channel] = 0;
    write32(&inst->reg->pwm_chn_pulse[channel], UINT32_MAX);

    if (!inst->enable[0] && !inst->enable[1] && !inst->enable[2]) {
        write32(&inst->reg->pwmcount, UINT32_MAX);
        rt_thread_delay(1);

        curr_cfg = read32(&inst->reg->pwmcfg);
        write32(&inst->reg->pwmcfg, curr_cfg & ~(1 << 12));
    }

    return RT_EOK;
}

/**
 * @brief Get current PWM configuration
 * @param inst PWM instance
 * @param channel Channel number (0-2)
 * @param configuration Pointer to configuration structure
 * @return RT_EOK on success, error code on failure
 */
static rt_err_t kd_pwm_get(struct pwm_inst* inst, rt_uint8_t channel, struct rt_pwm_configuration* configuration)
{
    if (!inst || !configuration || channel > 2) {
        return -RT_EINVAL;
    }

    uint64_t pulse, period;
    uint32_t pwm_pclock = sysctl_clk_get_leaf_freq(SYSCTL_CLK_PWM_PCLK_GATE);

    // if (inst->enable[channel]) {
    //     uint32_t scale = read32(&inst->reg->pwmcfg) & 0x0f;
    //     pwm_pclock >>= scale;
    //     period = read32(&inst->reg->pwmcmp0);
    //     pulse  = period - read32(&inst->reg->pwm_chn_pulse[channel]);
    // } else
    {
        period = inst->period;
        pulse  = inst->pulses[channel];
    }

    /* Convert to nanoseconds */
    configuration->period = period ? (uint64_t)period * NSEC_PER_SEC / pwm_pclock : 0;
    configuration->pulse  = pulse ? (uint64_t)pulse * NSEC_PER_SEC / pwm_pclock : 0;

    return RT_EOK;
}

static void kd_pwm_write_pulse(struct pwm_inst* inst, int channel,
                               uint32_t period, uint32_t pulse,
                               uint32_t scale)
{
    if (pulse == 0) { /* duty 0 */
        write32(&inst->reg->pwm_chn_pulse[channel], UINT32_MAX);
    } else if (pulse >= period) { /* duty 100 */
        write32(&inst->reg->pwm_chn_pulse[channel], 0);
    } else { /* duty 0 ~ 100 */
        write32(&inst->reg->pwm_chn_pulse[channel],
                (period - pulse) >> scale);
    }
}

/**
 * @brief Set PWM configuration
 * @param inst PWM instance
 * @param channel Channel number (0-2)
 * @param configuration Pointer to configuration structure
 * @return RT_EOK on success, error code on failure
 */
static int kd_pwm_set(struct pwm_inst* inst, int channel, struct rt_pwm_configuration* configuration)
{
    uint32_t new_pulses[3];
    uint32_t old_period;
    bool period_changed;
    bool any_enabled = false;
    int i;

    if (!inst || !configuration || channel > 2) {
        return -RT_EINVAL;
    }

    uint64_t pulse, period, pwmcmpx_max;
    uint32_t pwm_pclock = sysctl_clk_get_leaf_freq(SYSCTL_CLK_PWM_PCLK_GATE);
    uint32_t pwmscale   = 0;

    /* Convert from nanoseconds to clock cycles */
    pulse  = configuration->pulse ? (uint64_t)configuration->pulse * pwm_pclock / NSEC_PER_SEC : 0;
    period = configuration->period ? (uint64_t)configuration->period * pwm_pclock / NSEC_PER_SEC : 0;

    /* Validate parameters */
    if (period == 0 || pulse > period || period > ((1ULL << (15 + 16)) - 1ULL)) {
        LOG_E("Invalid config for channel %d (pulse=%llu, period=%llu)", channel, pulse, period);
        return -RT_EINVAL;
    }

    if ((0x00 == pulse) && configuration->pulse) {
        pulse = 1;
        LOG_W("Setting pulse width to 1 clock cycle for channel %d", channel);
    }

    /* Calculate appropriate scale factor */
    pwmcmpx_max = (1U << 16) - 1U;
    while ((period >> pwmscale) > pwmcmpx_max) {
        pwmscale++;

        if (pwmscale > 0xF) {
            LOG_E("Period too large for channel %d", channel);
            return -RT_EINVAL;
        }
    }

    old_period = inst->period;
    period_changed = period != old_period;

    /* All three channels share period and scale. Keep sibling duty ratios
     * coherent when one channel changes the instance frequency.
     */
    for (i = 0; i < 3; i++) {
        new_pulses[i] = inst->pulses[i];
        any_enabled = any_enabled || inst->enable[i];

        if (i == channel || !period_changed)
            continue;

        if (old_period == 0 || period == 0) {
            new_pulses[i] = 0;
        } else if (inst->pulses[i] >= old_period) {
            new_pulses[i] = (uint32_t)period;
        } else {
            new_pulses[i] = (uint32_t)(period * inst->pulses[i] /
                                       old_period);
        }
    }
    new_pulses[channel] = (uint32_t)pulse;

    if (period_changed) {
        if (any_enabled) {
            write32(&inst->reg->pwmcfg, (read32(&inst->reg->pwmcfg) & (~0x0F)) | pwmscale);
            write32(&inst->reg->pwmcmp0, period >> pwmscale);

            for (i = 0; i < 3; i++) {
                if (inst->enable[i]) {
                    kd_pwm_write_pulse(inst, i, (uint32_t)period,
                                       new_pulses[i], pwmscale);
                }
            }
        }
    } else if (inst->enable[channel]) {
        kd_pwm_write_pulse(inst, channel, (uint32_t)period,
                           new_pulses[channel], pwmscale);
    }

    /* Store current settings */
    inst->scale  = pwmscale;
    inst->period = (uint32_t)period;
    for (i = 0; i < 3; i++)
        inst->pulses[i] = new_pulses[i];

    return RT_EOK;
}

/**
 * @brief Get PWM channel state
 * @param inst PWM instance
 * @param channel Channel number (0-2)
 * @return RT_EOK on success, error code on failure
 */
static int kd_pwm_get_state(struct pwm_inst* inst, int channel)
{
    if (channel < 0 || channel > 2) {
        LOG_E("Invalid channel %d", channel);
        return -RT_EINVAL;
    }
    return inst->enable[channel] ? RT_EOK : RT_ERROR;
}

/**
 * @brief PWM control function
 * @param device PWM device pointer
 * @param cmd Command to execute
 * @param arg Command argument
 * @return RT_EOK on success, error code on failure
 */
static rt_err_t kd_pwm_control(struct rt_device_pwm* device, int cmd, void* arg)
{
    if (!device || !arg) {
        return -RT_EINVAL;
    }

    struct rt_pwm_configuration config;
    struct pwm_inst_wrap*       wrap = (struct pwm_inst_wrap*)(device->parent.user_data);

    /* Validate instance type */
    if (&_pwm_dev_inst_type != wrap->inst_type) {
        return -RT_EINVAL;
    }

    int pid = lwp_getpid();

    if (0x00 != pid) {
        if (sizeof(struct rt_pwm_configuration) != lwp_get_from_user(&config, arg, sizeof(struct rt_pwm_configuration))) {
            LOG_E("Failed to copy configuration from user space");
            return -RT_ERROR;
        }
    } else {
        rt_memcpy(&config, arg, sizeof(struct rt_pwm_configuration));
    }

    /* Validate channel number */
    if (config.channel < 0 || config.channel > 5) {
        LOG_E("Invalid channel %d", config.channel);
        return -RT_EINVAL;
    }

    /* Map channel to instance and sub-channel */
    uint32_t         inst_idx = PWM_CHANNEL_TO_INST(config.channel);
    uint32_t         sub_ch   = PWM_CHANNEL_TO_SUBCH(config.channel);
    struct pwm_inst* pwm_inst = &wrap->pwm[inst_idx];

    /* Execute command */
    switch (cmd) {
    case PWM_CMD_ENABLE:
    case KD_PWM_CMD_ENABLE:
        return pwm_start(pwm_inst, sub_ch);
    case PWM_CMD_DISABLE:
    case KD_PWM_CMD_DISABLE:
        return pwm_stop(pwm_inst, sub_ch);
    case PWM_CMD_SET:
    case KD_PWM_CMD_SET_CFG:
        return kd_pwm_set(pwm_inst, sub_ch, &config);
    case PWM_CMD_GET:
    case KD_PWM_CMD_GET_CFG:
        if (kd_pwm_get(pwm_inst, sub_ch, &config) != RT_EOK) {
            LOG_E("Failed to get PWM configuration for channel %d", config.channel);
            return -RT_ERROR;
        }
        if (0x00 != pid) {
            if (lwp_put_to_user(arg, &config, sizeof(struct rt_pwm_configuration)) != sizeof(struct rt_pwm_configuration)) {
                LOG_E("Failed to copy configuration to user space");
                return -RT_ERROR;
            }
        } else {
            rt_memcpy(arg, &config, sizeof(struct rt_pwm_configuration));
        }
        return RT_EOK;
    case KD_PWM_CMD_GET_STAT:
        return kd_pwm_get_state(pwm_inst, sub_ch);
    default:
        LOG_W("Unsupported command %d", cmd);
        return -RT_EINVAL;
    }
}

/* PWM Driver Operations */
static struct rt_pwm_ops drv_ops = { .control = kd_pwm_control };

/**
 * @brief Initialize PWM hardware
 * @return RT_EOK on success, error code on failure
 */
int rt_hw_pwm_init(void)
{
    /* Check if already registered */
    if (rt_device_find("pwm")) {
        LOG_E("PWM device already registered");
        return -RT_ERROR;
    }

    /* Map PWM registers */
    void* reg = rt_ioremap((void*)PWM_BASE_ADDR, PWM_IO_SIZE);
    if (!reg) {
        LOG_E("PWM ioremap failed");
        return -RT_ERROR;
    }

    /* Initialize PWM wrapper */
    rt_memset(&pwm_inst_wrap, 0, sizeof(pwm_inst_wrap));
    pwm_inst_wrap.inst_type = &_pwm_dev_inst_type;

    /* Initialize PWM instance 0 (channels 0-2) */
    pwm_inst_wrap.pwm[0].reg = (kd_pwm_t*)((char*)reg + 0x00);
    write32(&pwm_inst_wrap.pwm[0].reg->pwmcfg, (1 << 9) | (1 << 10)); /* deglitch and enalways and cmpxcenter */

    /* Initialize PWM instance 1 (channels 3-5) */
    pwm_inst_wrap.pwm[1].reg = (kd_pwm_t*)((char*)reg + 0x40);
    write32(&pwm_inst_wrap.pwm[1].reg->pwmcfg, (1 << 9) | (1 << 10)); /* deglitch and enalways and cmpxcenter */

    /* Register PWM device */
    pwm_inst_wrap.device.ops = &drv_ops;
    if (rt_device_pwm_register(&pwm_inst_wrap.device, "pwm", &drv_ops, &pwm_inst_wrap) != RT_EOK) {
        LOG_E("PWM device registration failed");
        rt_iounmap(reg);
        return -RT_ERROR;
    }

    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_pwm_init);

#if defined(RT_USING_MSH) && defined(RT_PWM_ENABLE_BUILTIN_CMD)
#include <finsh.h>
#include <msh.h>

static void do_pwm(int argc, char** argv)
{
    if (argc < 3) {
        rt_kprintf("Usage:\n  pwm enable <channel>\n  pwm disable <channel>\n"
                   "  pwm set <channel> <period_ns> <pulse_ns>\n  pwm get <channel>\n"
                   "Channels: 0-5\n");
        return;
    }

    struct rt_pwm_configuration config  = { .channel = atoi(argv[2]) };
    rt_device_t                 pwm_dev = rt_device_find("pwm");

    if (!pwm_dev) {
        rt_kprintf("PWM device not found\n");
        return;
    }

    const char* cmd = argv[1];
    if (!rt_strcmp(cmd, "enable")) {
        rt_kprintf(rt_device_control(pwm_dev, PWM_CMD_ENABLE, &config) == RT_EOK ? "Enabled PWM channel %d\n"
                                                                                 : "Failed to enable channel %d\n",
                   config.channel);
    } else if (!rt_strcmp(cmd, "disable")) {
        rt_kprintf(rt_device_control(pwm_dev, PWM_CMD_DISABLE, &config) == RT_EOK ? "Disabled PWM channel %d\n"
                                                                                  : "Failed to disable channel %d\n",
                   config.channel);
    } else if (!rt_strcmp(cmd, "set") && argc == 5) {
        config.period = atoi(argv[3]);
        config.pulse  = atoi(argv[4]);
        if (rt_device_control(pwm_dev, PWM_CMD_SET, &config) == RT_EOK) {
            rt_kprintf("Set channel %d: period=%dns, pulse=%dns (%.1f%%)\n", config.channel, config.period, config.pulse,
                       (float)config.pulse * 100 / config.period);
        } else {
            rt_kprintf("Failed to set PWM parameters\n");
        }
    } else if (!rt_strcmp(cmd, "get")) {
        if (rt_device_control(pwm_dev, PWM_CMD_GET, &config) == RT_EOK) {
            rt_kprintf("Channel %d: period=%dns, pulse=%dns (%.1f%%)\n", config.channel, config.period, config.pulse,
                       (float)config.pulse * 100 / config.period);
        } else {
            rt_kprintf("Failed to get PWM parameters\n");
        }
    } else {
        rt_kprintf("Invalid command\n");
    }
}

MSH_CMD_EXPORT_ALIAS(do_pwm, pwm,
                     "PWM command:\n"
                     "  enable <channel>\n"
                     "  disable <channel>\n"
                     "  set <channel> <period_ns> <pulse_ns>\n"
                     "  get <channel>\n"
                     "Channels: 0-5");
#endif
