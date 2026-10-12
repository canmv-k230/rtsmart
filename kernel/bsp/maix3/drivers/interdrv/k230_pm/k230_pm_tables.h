/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __K230_PM_TABLES_H__
#define __K230_PM_TABLES_H__

#include <stdint.h>
#include <stdbool.h>
#include "sysctl_clk.h"
#include "sysctl_pwr.h"
#include "sysctl_rst.h"

#define K230_CLK_PARENT_NONE    (-1)
#define K230_CLK_PARENT_DYNAMIC (-2)
#define K230_PD_NONE            (-1)
#define K230_RESET_NONE         (-1)
#define K230_PM_MAX_DEV_CLKS    16

#define K230_DVFS_CAP_AUTO    (1U << 0)
#define K230_DVFS_CAP_THERMAL (1U << 1)

struct k230_dvfs_step {
    int32_t freq;
    int32_t volt;
    sysctl_clk_node_e leaf;
    sysctl_clk_node_e parent;
    uint16_t div_num;
    uint16_t div_den;
};

struct k230_dvfs_domain_desc {
    const char *name;
    int16_t pm_dev;
    const char *regulator_name;
    const struct k230_dvfs_step *steps;
    uint8_t step_count;
    uint8_t caps;
};

enum k230_clk_flags {
    K230_CLK_F_GATEABLE       = 1U << 0,
    K230_CLK_F_BOOT_CRITICAL  = 1U << 1,
    K230_CLK_F_ALWAYS_ON      = 1U << 2,
    K230_CLK_F_DYNAMIC_PARENT = 1U << 3,
    K230_CLK_F_BOOT_PINNED_OK = 1U << 4,
};

enum k230_pd_flags {
    K230_PD_F_ALWAYS_ON = 1U << 0,
    K230_PD_F_BOOT_PINNED_OK = 1U << 1,
};

struct k230_clk_desc {
    sysctl_clk_node_e node;
    const char *name;
    int16_t parent;
    uint8_t flags;
};

struct k230_pd_desc {
    sysctl_pwr_domain_e pd;
    const char *name;
    int16_t reset;
    uint8_t flags;
};

struct k230_pm_dev_desc {
    int id;
    const char *name;
    int16_t pd;
    int16_t reset;
    const sysctl_clk_node_e clocks[K230_PM_MAX_DEV_CLKS];
    uint8_t clock_count;
    const char *owner;
};

extern const struct k230_clk_desc k230_clk_desc[];
extern const unsigned int k230_clk_desc_count;
extern const struct k230_pd_desc k230_pd_desc[];
extern const unsigned int k230_pd_desc_count;
extern const struct k230_pm_dev_desc k230_pm_dev_desc[];
extern const unsigned int k230_pm_dev_desc_count;
extern const struct k230_dvfs_domain_desc k230_dvfs_domain_desc[];
extern const unsigned int k230_dvfs_domain_desc_count;

const struct k230_clk_desc *k230_clk_desc_find(sysctl_clk_node_e node);
const char *k230_clk_name(sysctl_clk_node_e node);
const char *k230_pd_name(sysctl_pwr_domain_e pd);
const char *k230_reset_name(sysctl_reset_e reset);

#endif
