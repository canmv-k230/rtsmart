/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <rtthread.h>

#include "k230_pm.h"
#ifdef K230_PM_DVFS
#include "k230_pm_dvfs.h"
#endif

#define K230_CLK_DUMP_MAX_DEPTH 32

static int k230_parent_node(const struct k230_clk_desc *desc)
{
    sysctl_clk_node_e parent;

    if (desc->parent != K230_CLK_PARENT_DYNAMIC)
        return desc->parent;

    if (sysctl_media_clk_is_node(desc->node))
        parent = sysctl_media_clk_get_leaf_parent(desc->node);
    else
        parent = sysctl_clk_get_leaf_parent(desc->node);

    if (parent >= SYSCTL_CLK_NODE_MAX || parent == SYSCTL_CLK_ROOT_MAX)
        return K230_CLK_PARENT_NONE;

    return parent;
}

static const char *k230_parent_name(const struct k230_clk_desc *desc)
{
    int parent = k230_parent_node(desc);

    if (parent == K230_CLK_PARENT_NONE)
        return "-";

    return k230_clk_name((sysctl_clk_node_e)parent);
}

static const char *k230_clk_gate_state(const struct k230_clk_desc *desc)
{
    if (desc->node >= SYSCTL_CLK_ROOT_MAX &&
        (desc->flags & K230_CLK_F_GATEABLE) == 0U)
        return "n/a";

    return k230_clk_hw_enabled(desc->node) ? "on" : "off";
}

static bool k230_clk_effective_enabled(sysctl_clk_node_e node)
{
    unsigned int depth;

    for (depth = 0; depth < K230_CLK_DUMP_MAX_DEPTH; depth++) {
        const struct k230_clk_desc *desc = k230_clk_desc_find(node);
        int parent;

        if (desc == RT_NULL || !k230_clk_hw_enabled(node))
            return false;

        parent = k230_parent_node(desc);
        if (parent == K230_CLK_PARENT_NONE)
            return true;
        node = (sysctl_clk_node_e)parent;
    }

    return false;
}

static void k230_clk_dump(int argc, char **argv)
{
    unsigned int i;

    (void)argc;
    (void)argv;

    rt_kprintf("%-42s %-42s %-4s %-4s %-4s %-3s %-10s %s\n",
               "clock", "parent", "gate", "eff", "ref", "pin", "rate_hz",
               "flags");
    for (i = 0; i < k230_clk_desc_count; i++) {
        const struct k230_clk_desc *desc = &k230_clk_desc[i];

        rt_kprintf("%-42s %-42s %-4s %-4s %-4d %-3s %-10u %c%c%c%c%c\n",
                   desc->name,
                   k230_parent_name(desc),
                   k230_clk_gate_state(desc),
                   k230_clk_effective_enabled(desc->node) ? "on" : "off",
                   k230_clk_refcount(desc->node),
                   k230_clk_boot_pinned(desc->node) ? "yes" : "no",
                   k230_clk_hw_rate(desc->node),
                   (desc->flags & K230_CLK_F_GATEABLE) ? 'g' : '-',
                   (desc->flags & K230_CLK_F_BOOT_CRITICAL) ? 'b' : '-',
                   (desc->flags & K230_CLK_F_ALWAYS_ON) ? 'a' : '-',
                   (desc->flags & K230_CLK_F_DYNAMIC_PARENT) ? 'd' : '-',
                   (desc->flags & K230_CLK_F_BOOT_PINNED_OK) ? 'r' : '-');
        rt_thread_mdelay(1);
    }
}
MSH_CMD_EXPORT(k230_clk_dump, dump K230 clock resource table);

static void k230_pd_dump(int argc, char **argv)
{
    unsigned int i;

    (void)argc;
    (void)argv;

    rt_kprintf("%-18s %-24s %-4s %-4s %-3s %-4s %s\n", "domain", "reset", "hw", "ref", "pin", "emer", "flags");
    for (i = 0; i < k230_pd_desc_count; i++) {
        const struct k230_pd_desc *desc = &k230_pd_desc[i];

        rt_kprintf("%-18s %-24s %-4s %-4d %-3s %-4s %c%c\n",
                   desc->name,
                   desc->reset >= 0 ? k230_reset_name((sysctl_reset_e)desc->reset) : "-",
                   k230_pd_hw_powered(desc->pd) ? "on" : "off",
                   k230_pd_refcount(desc->pd),
                   k230_pd_boot_pinned(desc->pd) ? "yes" : "no",
                   k230_pd_emergency_offed(desc->pd) ? "yes" : "no",
                   (desc->flags & K230_PD_F_ALWAYS_ON) ? 'a' : '-',
                   (desc->flags & K230_PD_F_BOOT_PINNED_OK) ? 'r' : '-');
    }
}
MSH_CMD_EXPORT(k230_pd_dump, dump K230 power-domain resource table);

static void k230_pm_dump(int argc, char **argv)
{
    unsigned int i;

    (void)argc;
    (void)argv;

    rt_kprintf("%-10s %-18s %-24s %-8s %s\n", "device", "domain", "reset", "owner", "clocks");
    for (i = 0; i < k230_pm_dev_desc_count; i++) {
        const struct k230_pm_dev_desc *desc = &k230_pm_dev_desc[i];
        unsigned int j;

        rt_kprintf("%-10s %-18s %-24s %-8s ",
                   desc->name,
                   desc->pd >= 0 ? k230_pd_name((sysctl_pwr_domain_e)desc->pd) : "-",
                   desc->reset >= 0 ? k230_reset_name((sysctl_reset_e)desc->reset) : "-",
                   desc->owner);
        for (j = 0; j < desc->clock_count; j++) {
            if (desc->clocks[j] == SYSCTL_CLK_NODE_MAX)
                continue;
            rt_kprintf("%s%s", j ? "," : "", k230_clk_name(desc->clocks[j]));
        }
        rt_kprintf("\n");
    }
}
MSH_CMD_EXPORT(k230_pm_dump, dump K230 device resource bindings);

static int k230_pm_release_boot(int argc, char **argv)
{
    int ret;

    (void)argc;
    (void)argv;

    ret = k230_pm_release_boot_pins();
    if (ret < 0) {
        rt_kprintf("k230_pm_release_boot: failed (%d)\n", ret);
        return -1;
    }

    rt_kprintf("k230_pm_release_boot: released %d boot pins\n", ret);
    return 0;
}
MSH_CMD_EXPORT(k230_pm_release_boot, release runtime-manageable boot clock and power pins);


#ifdef K230_PM_DVFS
static void k230_pm_dvfs_dump_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    k230_pm_dvfs_dump();
}
MSH_CMD_EXPORT_ALIAS(k230_pm_dvfs_dump_cmd, k230_pm_dvfs_dump, dump K230 DVFS policy state);
#endif

static void k230_pm_selftest_expect(bool condition, const char *message, int *failures)
{
    if (condition) {
        rt_kprintf("ok: %s\n", message);
        return;
    }

    rt_kprintf("fail: %s\n", message);
    (*failures)++;
}

static void k230_pm_selftest_topology(int *failures)
{
    const struct k230_clk_desc *desc =
        k230_clk_desc_find(SYSCTL_CLK_HS_HCLK);

    k230_pm_selftest_expect(desc != RT_NULL &&
        desc->parent == SYSCTL_CLK_NOC_HS_SUB_HCLK,
        "normal HS HCLK bypasses high gate", failures);
    k230_pm_selftest_expect(
        sysctl_clk_get_leaf_parent(SYSCTL_CLK_HS_HCLK) ==
            SYSCTL_CLK_NOC_HS_SUB_HCLK,
        "HS HCLK table matches hardware parent", failures);
}

static sysctl_clk_node_e k230_pm_find_zero_ref_gate(void)
{
    static const sysctl_clk_node_e candidates[] = {
        SYSCTL_CLK_UART3_PCLK,
        SYSCTL_CLK_UART3_CLK,
        SYSCTL_CLK_I2C4_PCLK,
        SYSCTL_CLK_I2C4_CLK,
        SYSCTL_CLK_JAMLINK3_PCLK,
        SYSCTL_CLK_JAMLINK3_CO,
        SYSCTL_CLK_PWM_PCLK,
        SYSCTL_CLK_ADC_PCLK,
        SYSCTL_CLK_DECOMP_CLK,
    };
    unsigned int i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const struct k230_clk_desc *desc = k230_clk_desc_find(candidates[i]);

        if (desc == RT_NULL)
            continue;
        if ((desc->flags & K230_CLK_F_GATEABLE) == 0U)
            continue;
        if ((desc->flags & K230_CLK_F_ALWAYS_ON) != 0U)
            continue;
        if (k230_clk_refcount(desc->node) == 0)
            return desc->node;
    }

    return SYSCTL_CLK_NODE_MAX;
}

static void k230_pm_selftest_stress_clock(sysctl_clk_node_e leaf,
                                           sysctl_clk_node_e parent,
                                           int loops,
                                           int *failures)
{
    int parent_initial = k230_clk_refcount(parent);
    int leaf_initial = k230_clk_refcount(leaf);
    int i;
    int ret = 0;

    for (i = 0; i < loops; i++) {
        ret = k230_clk_get(leaf);
        if (ret != 0)
            break;
    }

    k230_pm_selftest_expect(ret == 0, "stress get loop", failures);
    k230_pm_selftest_expect(k230_clk_refcount(leaf) == leaf_initial + loops,
                            "stress leaf refcount", failures);
    k230_pm_selftest_expect(k230_clk_refcount(parent) == parent_initial + loops,
                            "stress parent refcount", failures);

    while (i > 0) {
        i--;
        ret = k230_clk_put(leaf);
        if (ret != 0)
            (*failures)++;
    }

    k230_pm_selftest_expect(k230_clk_refcount(leaf) == leaf_initial,
                            "stress leaf restored", failures);
    k230_pm_selftest_expect(k230_clk_refcount(parent) == parent_initial,
                            "stress parent restored", failures);
}

static void k230_pm_selftest_zero_ref_transition(int *failures)
{
    sysctl_clk_node_e node = k230_pm_find_zero_ref_gate();
    int ret;

    if (node == SYSCTL_CLK_NODE_MAX) {
        rt_kprintf("skip: no zero-ref gate available for transition test\n");
        return;
    }

    ret = k230_clk_get(node);
    k230_pm_selftest_expect(ret == 0, "zero-ref gate get", failures);
    k230_pm_selftest_expect(k230_clk_refcount(node) == 1,
                            "zero-ref gate refcount is one", failures);
    k230_pm_selftest_expect(k230_clk_hw_enabled(node),
                            "zero-ref gate hw enabled", failures);
    ret = k230_clk_put(node);
    k230_pm_selftest_expect(ret == 0, "zero-ref gate put", failures);
    k230_pm_selftest_expect(k230_clk_refcount(node) == 0,
                            "zero-ref gate refcount restored", failures);

    ret = k230_clk_put(node);
    k230_pm_selftest_expect(ret == -K230_PM_EUNDERFLOW,
                            "zero-ref put reports underflow", failures);
    k230_pm_selftest_expect(k230_clk_refcount(node) == 0,
                            "underflow leaves refcount unchanged", failures);
}

static void k230_pm_selftest_parent_gate(int *failures)
{
    const sysctl_clk_node_e child = SYSCTL_CLK_CSI0_CFG_CLK;
    const sysctl_clk_node_e parent = SYSCTL_CLK_ISP_CFG_CLK;
    const int loops = 100;
    int child_initial = k230_clk_refcount(child);
    int parent_initial = k230_clk_refcount(parent);
    int held = 0;
    int i;
    int ret = 0;

    if (child_initial != 0 || parent_initial != 0) {
        rt_kprintf("fail: parent-gate clocks busy child=%d parent=%d\n",
            child_initial, parent_initial);
        (*failures)++;
        return;
    }

    k230_pm_selftest_expect(!k230_clk_boot_pinned(child) &&
        !k230_clk_boot_pinned(parent),
        "parent-gate boot pins released", failures);
    k230_pm_selftest_expect(!k230_clk_hw_enabled(child) &&
        !k230_clk_hw_enabled(parent),
        "parent-gate clocks initially off", failures);

    for (i = 0; i < loops; i++) {
        ret = k230_clk_get(child);
        if (ret != 0)
            break;
        held++;
        if (k230_clk_refcount(child) != 1 ||
            k230_clk_refcount(parent) != 1 ||
            !k230_clk_hw_enabled(child) || !k230_clk_hw_enabled(parent)) {
            ret = -K230_PM_ECTX;
            break;
        }

        ret = k230_clk_get(child);
        if (ret != 0)
            break;
        held++;
        if (k230_clk_refcount(child) != 2 ||
            k230_clk_refcount(parent) != 2) {
            ret = -K230_PM_ECTX;
            break;
        }

        ret = k230_clk_put(child);
        if (ret != 0)
            break;
        held--;
        if (k230_clk_refcount(child) != 1 ||
            k230_clk_refcount(parent) != 1 ||
            !k230_clk_hw_enabled(child) || !k230_clk_hw_enabled(parent)) {
            ret = -K230_PM_ECTX;
            break;
        }

        ret = k230_clk_put(child);
        if (ret != 0)
            break;
        held--;
        if (k230_clk_refcount(child) != 0 ||
            k230_clk_refcount(parent) != 0 ||
            k230_clk_hw_enabled(child) || k230_clk_hw_enabled(parent)) {
            ret = -K230_PM_ECTX;
            break;
        }
    }

    while (held > 0) {
        if (k230_clk_put(child) != 0)
            ret = -K230_PM_ECTX;
        held--;
    }

    k230_pm_selftest_expect(ret == 0 && i == loops,
        "parent-gate 100 nested start/stop cycles", failures);
    k230_pm_selftest_expect(k230_clk_refcount(child) == child_initial &&
        k230_clk_refcount(parent) == parent_initial &&
        !k230_clk_hw_enabled(child) && !k230_clk_hw_enabled(parent),
        "parent-gate final vote powers clocks down", failures);
}

static void k230_pm_selftest_simulated_rollback(int *failures)
{
    const struct k230_pm_dev_desc *desc = k230_pm_dev_desc_find(K230_PM_DEV_UART3);
    int before[K230_PM_MAX_DEV_CLKS];
    unsigned int acquired = 0;
    unsigned int i;
    int ret = 0;

    if (desc == RT_NULL || desc->clock_count < 2) {
        rt_kprintf("skip: no multi-clock device for rollback test\n");
        return;
    }

    for (i = 0; i < desc->clock_count; i++)
        before[i] = k230_clk_refcount(desc->clocks[i]);

    for (i = 0; i < desc->clock_count; i++) {
        ret = k230_clk_get(desc->clocks[i]);
        if (ret != 0)
            break;
        acquired++;
        if (i == 0)
            break;
    }

    k230_pm_selftest_expect(ret == 0 && acquired == 1,
                            "rollback setup acquired first resource", failures);

    while (acquired > 0) {
        acquired--;
        (void)k230_clk_put(desc->clocks[acquired]);
    }

    for (i = 0; i < desc->clock_count; i++) {
        k230_pm_selftest_expect(k230_clk_refcount(desc->clocks[i]) == before[i],
                                "rollback restores clock refcount", failures);
    }
}

static void k230_pm_selftest_device_roundtrip(int *failures)
{
    const struct k230_pm_dev_desc *desc = k230_pm_dev_desc_find(K230_PM_DEV_UART3);
    int before[K230_PM_MAX_DEV_CLKS];
    int device_before;
    int clock_delta;
    unsigned int i;
    int ret;

    if (desc == RT_NULL) {
        k230_pm_selftest_expect(false, "device descriptor exists", failures);
        return;
    }

    for (i = 0; i < desc->clock_count; i++)
        before[i] = k230_clk_refcount(desc->clocks[i]);
    device_before = k230_pm_refcount(K230_PM_DEV_UART3);
    clock_delta = device_before == 0 ? 1 : 0;

    ret = k230_pm_get(K230_PM_DEV_UART3);
    k230_pm_selftest_expect(ret == 0, "device get", failures);
    k230_pm_selftest_expect(
        k230_pm_refcount(K230_PM_DEV_UART3) == device_before + 1,
        "device get increments device refcount", failures);
    for (i = 0; i < desc->clock_count; i++) {
        k230_pm_selftest_expect(
            k230_clk_refcount(desc->clocks[i]) == before[i] + clock_delta,
            "device get updates clock vote", failures);
    }

    ret = k230_pm_put(K230_PM_DEV_UART3);
    k230_pm_selftest_expect(ret == 0, "device put", failures);
    k230_pm_selftest_expect(
        k230_pm_refcount(K230_PM_DEV_UART3) == device_before,
        "device put restores device refcount", failures);
    for (i = 0; i < desc->clock_count; i++) {
        k230_pm_selftest_expect(k230_clk_refcount(desc->clocks[i]) == before[i],
                                "device put restores clock", failures);
    }
}

static int k230_pm_selftest(int argc, char **argv)
{
    int failures = 0;
    int parent_initial;
    int leaf_initial;
    int ret;

    (void)argc;
    (void)argv;

    parent_initial = k230_clk_refcount(SYSCTL_CLK_LS_PCLK);
    leaf_initial = k230_clk_refcount(SYSCTL_CLK_UART3_PCLK);

    ret = k230_clk_get(SYSCTL_CLK_UART3_PCLK);
    k230_pm_selftest_expect(ret == 0, "first uart3 pclk get", &failures);
    ret = k230_clk_get(SYSCTL_CLK_UART3_PCLK);
    k230_pm_selftest_expect(ret == 0, "second uart3 pclk get", &failures);
    k230_pm_selftest_expect(k230_clk_refcount(SYSCTL_CLK_UART3_PCLK) == leaf_initial + 2,
                            "leaf refcount increments", &failures);
    k230_pm_selftest_expect(k230_clk_refcount(SYSCTL_CLK_LS_PCLK) == parent_initial + 2,
                            "parent refcount propagates", &failures);

    ret = k230_clk_put(SYSCTL_CLK_UART3_PCLK);
    k230_pm_selftest_expect(ret == 0, "first uart3 pclk put", &failures);
    k230_pm_selftest_expect(k230_clk_refcount(SYSCTL_CLK_UART3_PCLK) == leaf_initial + 1,
                            "leaf remains referenced after one put", &failures);
    k230_pm_selftest_expect(k230_clk_refcount(SYSCTL_CLK_LS_PCLK) == parent_initial + 1,
                            "parent remains referenced after one put", &failures);

    ret = k230_clk_put(SYSCTL_CLK_UART3_PCLK);
    k230_pm_selftest_expect(ret == 0, "second uart3 pclk put", &failures);
    k230_pm_selftest_expect(k230_clk_refcount(SYSCTL_CLK_UART3_PCLK) == leaf_initial,
                            "leaf refcount restored", &failures);
    k230_pm_selftest_expect(k230_clk_refcount(SYSCTL_CLK_LS_PCLK) == parent_initial,
                            "parent refcount restored", &failures);

    k230_pm_selftest_topology(&failures);
    k230_pm_selftest_stress_clock(SYSCTL_CLK_UART3_PCLK,
                                  SYSCTL_CLK_LS_PCLK, 64, &failures);
    k230_pm_selftest_zero_ref_transition(&failures);
    k230_pm_selftest_parent_gate(&failures);
    k230_pm_selftest_simulated_rollback(&failures);
    k230_pm_selftest_device_roundtrip(&failures);

    if (failures == 0)
        rt_kprintf("k230_pm_selftest: PASS\n");
    else
        rt_kprintf("k230_pm_selftest: FAIL (%d)\n", failures);

    return failures == 0 ? 0 : -1;
}
MSH_CMD_EXPORT(k230_pm_selftest, run K230 PM clock selftest);
