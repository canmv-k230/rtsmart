/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef __K230_PM_H__
#define __K230_PM_H__

#include <stdint.h>
#include <stdbool.h>
#include "k230_pm_tables.h"

#include "k230_pm_dev.h"

#define K230_PM_EINVAL      1
#define K230_PM_EUNDERFLOW  2
#define K230_PM_ETIMEOUT    3
#define K230_PM_ECTX        4

const struct k230_pm_dev_desc *k230_pm_dev_desc_find(enum k230_pm_dev_id dev);

int k230_clk_get(sysctl_clk_node_e node);
int k230_clk_put(sysctl_clk_node_e node);
int k230_clk_set_div(sysctl_clk_node_e node, uint32_t numerator,
    uint32_t denominator);
int k230_clk_set_parent_div(sysctl_clk_node_e node, sysctl_clk_node_e parent,
    uint32_t numerator, uint32_t denominator);
int k230_clk_refcount(sysctl_clk_node_e node);
bool k230_clk_boot_pinned(sysctl_clk_node_e node);
bool k230_clk_hw_enabled(sysctl_clk_node_e node);
uint32_t k230_clk_hw_rate(sysctl_clk_node_e node);

int k230_pd_get(sysctl_pwr_domain_e pd);
int k230_pd_put(sysctl_pwr_domain_e pd);
int k230_pd_refcount(sysctl_pwr_domain_e pd);
bool k230_pd_boot_pinned(sysctl_pwr_domain_e pd);
bool k230_pd_emergency_offed(sysctl_pwr_domain_e pd);
bool k230_pd_hw_powered(sysctl_pwr_domain_e pd);

int k230_pm_get(enum k230_pm_dev_id dev);
int k230_pm_put(enum k230_pm_dev_id dev);
int k230_pm_refcount(enum k230_pm_dev_id dev);
int k230_pm_emergency_off(enum k230_pm_dev_id dev);
int k230_pm_release_boot_pins(void);

#endif
