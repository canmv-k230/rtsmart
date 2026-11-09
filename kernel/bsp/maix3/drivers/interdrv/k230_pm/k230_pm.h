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

/* Thermal admission state is lock-free and checked before reference fast paths.
 * The NONE backend ignores accelerator blocks and enforces clock caps only. */
bool k230_pm_shutdown_latch(unsigned int reason);
unsigned int k230_pm_shutdown_reason(void);
/* True only when the configured thermal backend makes a latched reason
 * terminal. NONE retains protection while allowing the system to run. */
bool k230_pm_shutdown_terminal(void);
void k230_pm_thermal_block_accelerators(bool block);
bool k230_pm_thermal_accelerators_blocked(void);
void k230_pm_thermal_set_limits(uint32_t cpu_hz, uint32_t kpu_hz, uint32_t vpu_hz);
uint32_t k230_pm_thermal_limit(sysctl_clk_node_e node);
/* Without force, a domain still referenced by clients or boot owners stays on. */
int k230_pm_thermal_off(enum k230_pm_dev_id dev, bool force);
/* Terminal shutdown only: interrupts disabled, register access, no mutexes.
 * Powers off every runtime-switchable domain; returns a mask (1 << pd) of
 * domains whose handshake failed and that are still powered. */
unsigned int k230_pm_thermal_quiesce_raw(void);
/* Enter the terminal thermal board hook without the generic device-console
 * diagnostic. This function does not return. */
void k230_pm_thermal_shutdown_enter(void);

#endif
