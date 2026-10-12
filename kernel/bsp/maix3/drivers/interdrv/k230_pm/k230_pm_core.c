/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <rtthread.h>
#include <rthw.h>
#include "k230_pm.h"
#include "sysctl_boot.h"
#include "sysctl_pwr.h"
#include "sysctl_rst.h"

#define K230_CLK_PATH_MAX 16
#define K230_CLK_REF_MAX  32767
#define K230_PD_REF_MAX   32767
#define K230_PM_DEV_REF_MAX 32767

struct k230_clk_state {
    int refcount;
    int parent_refcount;
    int parent;
    bool boot_pinned;
    bool boot_parent_vote;
    const char *owner;
};

struct k230_pd_state {
    int refcount;
    bool boot_pinned;
    bool emergency_off;
    const char *owner;
};

struct k230_pm_dev_state {
    int refcount;
};

extern volatile sysctl_pwr_s *sysctl_pwr;

static struct rt_mutex k230_clk_mutex;
static struct rt_mutex k230_pd_mutex;
static struct rt_mutex k230_pm_mutex;
static struct k230_clk_state k230_clk_state[SYSCTL_CLK_NODE_MAX];
static struct k230_pd_state k230_pd_state[SYSCTL_PD_MAX];
static struct k230_pm_dev_state k230_pm_dev_state[K230_PM_DEV_ID_MAX];
static bool k230_clk_ready;

static bool k230_clk_is_fixed_root(sysctl_clk_node_e node)
{
    return node == SYSCTL_CLK_OSC24M || node == SYSCTL_CLK_ROOT_TIMERX_PULSE_IN;
}

static int k230_clk_root_pll_index(sysctl_clk_node_e node)
{
    switch (node) {
    case SYSCTL_CLK_PLL0_OCLK:
    case SYSCTL_CLK_PLL0_OCLK_DIV2:
    case SYSCTL_CLK_PLL0_OCLK_DIV3:
    case SYSCTL_CLK_PLL0_OCLK_DIV4:
        return 0;
    case SYSCTL_CLK_PLL1_OCLK:
    case SYSCTL_CLK_PLL1_OCLK_DIV2:
    case SYSCTL_CLK_PLL1_OCLK_DIV3:
    case SYSCTL_CLK_PLL1_OCLK_DIV4:
        return 1;
    case SYSCTL_CLK_PLL2_OCLK:
    case SYSCTL_CLK_PLL2_OCLK_DIV2:
    case SYSCTL_CLK_PLL2_OCLK_DIV3:
    case SYSCTL_CLK_PLL2_OCLK_DIV4:
        return 2;
    case SYSCTL_CLK_PLL3_OCLK:
    case SYSCTL_CLK_PLL3_OCLK_DIV2:
    case SYSCTL_CLK_PLL3_OCLK_DIV3:
    case SYSCTL_CLK_PLL3_OCLK_DIV4:
        return 3;
    default:
        return -1;
    }
}

static bool k230_clk_is_media(sysctl_clk_node_e node)
{
    return sysctl_media_clk_is_node(node);
}

static bool k230_clk_should_boot_pin(const struct k230_clk_desc *desc)
{
    return (desc->flags & (K230_CLK_F_GATEABLE | K230_CLK_F_ALWAYS_ON | K230_CLK_F_BOOT_PINNED_OK)) != 0U;
}

static bool k230_clk_valid(sysctl_clk_node_e node)
{
    return node >= 0 && node < SYSCTL_CLK_NODE_MAX && k230_clk_desc_find(node) != RT_NULL;
}

static int k230_clk_parent(sysctl_clk_node_e node)
{
    const struct k230_clk_desc *desc = k230_clk_desc_find(node);

    if (desc == RT_NULL)
        return K230_CLK_PARENT_NONE;

    if (desc->parent == K230_CLK_PARENT_DYNAMIC) {
        sysctl_clk_node_e parent;

        if (k230_clk_is_media(node))
            parent = sysctl_media_clk_get_leaf_parent(node);
        else
            parent = sysctl_clk_get_leaf_parent(node);

        if (parent >= SYSCTL_CLK_NODE_MAX || parent == SYSCTL_CLK_ROOT_MAX)
            return K230_CLK_PARENT_NONE;
        return parent;
    }

    return desc->parent;
}

static int k230_clk_parent_cached(sysctl_clk_node_e node)
{
    const struct k230_clk_desc *desc = k230_clk_desc_find(node);
    struct k230_clk_state *state;

    if (desc == RT_NULL)
        return K230_CLK_PARENT_NONE;

    state = &k230_clk_state[node];
    if ((desc->flags & K230_CLK_F_DYNAMIC_PARENT) != 0U &&
        state->refcount > 0 && state->parent != K230_CLK_PARENT_NONE)
        return state->parent;

    return k230_clk_parent(node);
}

static bool k230_clk_root_family_active(sysctl_clk_node_e root)
{
    int pll = k230_clk_root_pll_index(root);
    unsigned int i;

    if (pll < 0)
        return false;

    for (i = 0; i < k230_clk_desc_count; i++) {
        sysctl_clk_node_e node = k230_clk_desc[i].node;
        int depth;

        if (k230_clk_state[node].refcount <= 0)
            continue;

        for (depth = 0; depth < K230_CLK_PATH_MAX; depth++) {
            int parent;

            if (k230_clk_root_pll_index(node) == pll)
                return true;

            parent = k230_clk_parent_cached(node);
            if (parent == K230_CLK_PARENT_NONE)
                break;
            if (!k230_clk_valid((sysctl_clk_node_e)parent) &&
                parent >= SYSCTL_CLK_ROOT_MAX)
                break;
            node = (sysctl_clk_node_e)parent;
        }
    }

    return false;
}

static int k230_clk_collect_path(sysctl_clk_node_e node, sysctl_clk_node_e *path,
    int *parents, int *count)
{
    int n = 0;

    if (!k230_clk_valid(node))
        return -K230_PM_EINVAL;

    while (n < K230_CLK_PATH_MAX) {
        int parent;

        if (!k230_clk_valid(node))
            return -K230_PM_EINVAL;

        path[n] = node;
        parent = k230_clk_parent_cached(node);
        parents[n] = parent;
        n++;
        if (parent == K230_CLK_PARENT_NONE)
            break;
        node = (sysctl_clk_node_e)parent;
    }

    if (n == K230_CLK_PATH_MAX)
        return -K230_PM_EINVAL;

    *count = n;
    return 0;
}

static bool k230_clk_hw_set_enabled(const struct k230_clk_desc *desc, bool enable)
{
    if ((desc->flags & K230_CLK_F_GATEABLE) != 0U) {
        /* The PM clock mutex owns this raw transition.  The media wrapper may
         * already be holding its per-leaf mutex while calling k230_pm_get/put.
         */
        sysctl_clk_set_leaf_en(desc->node, enable);
        return true;
    }

    if (desc->node < SYSCTL_CLK_ROOT_MAX && !k230_clk_is_fixed_root(desc->node)) {
        if (enable) {
            if (k230_clk_root_family_active(desc->node)) {
                sysctl_boot_set_root_clk_en(desc->node, true);
                return true;
            }
            return sysctl_boot_start_root_clk(desc->node);
        }
        if (!k230_clk_root_family_active(desc->node))
            return sysctl_boot_set_root_clk_pwroff(desc->node);
    }

    return true;
}

bool k230_clk_hw_enabled(sysctl_clk_node_e node)
{
    if (node >= SYSCTL_CLK_NODE_MAX)
        return false;

    if (k230_clk_is_fixed_root(node))
        return true;

    if (node < SYSCTL_CLK_ROOT_MAX)
        return sysctl_boot_get_root_clk_en(node);

    if (k230_clk_is_media(node))
        return sysctl_media_clk_get_leaf_en(node);

    return sysctl_clk_get_leaf_en(node);
}

uint32_t k230_clk_hw_rate(sysctl_clk_node_e node)
{
    if (node >= SYSCTL_CLK_NODE_MAX)
        return 0;

    if (node < SYSCTL_CLK_ROOT_MAX)
        return sysctl_boot_get_root_clk_freq(node);

    if (k230_clk_is_media(node))
        return sysctl_media_clk_get_leaf_freq(node);

    return sysctl_clk_get_leaf_freq(node);
}

/* The path helpers below are private to clock operations holding
 * k230_clk_mutex. Interrupts may be re-enabled around hardware transitions
 * without exposing partially updated clock state to another thread.
 */
static int k230_clk_acquire_one(sysctl_clk_node_e node, int parent)
{
    const struct k230_clk_desc *desc = k230_clk_desc_find(node);
    struct k230_clk_state *state = &k230_clk_state[node];
    rt_base_t level;

    if (desc == RT_NULL)
        return -K230_PM_EINVAL;

    level = rt_hw_interrupt_disable();
    if (state->refcount >= K230_CLK_REF_MAX) {
        rt_hw_interrupt_enable(level);
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: clk %s refcount overflow\n", desc->name);
#endif
        return -K230_PM_EINVAL;
    }

    if (parent != K230_CLK_PARENT_NONE &&
        state->parent_refcount >= K230_CLK_REF_MAX) {
        rt_hw_interrupt_enable(level);
        return -K230_PM_EINVAL;
    }

    if (state->refcount == 0) {
        rt_hw_interrupt_enable(level);
        if (!k230_clk_hw_set_enabled(desc, true)) {
#ifdef K230_PM_DEBUG
            rt_kprintf("k230_pm: clk %s start timeout\n", desc->name);
#endif
            return -K230_PM_ETIMEOUT;
        }
        level = rt_hw_interrupt_disable();
        state->parent = parent;
    } else if ((desc->flags & K230_CLK_F_DYNAMIC_PARENT) != 0U &&
        state->parent == K230_CLK_PARENT_NONE) {
        state->parent = parent;
    }
    state->refcount++;
    if (parent != K230_CLK_PARENT_NONE)
        state->parent_refcount++;
    rt_hw_interrupt_enable(level);

#ifdef K230_PM_DEBUG
    rt_kprintf("k230_pm: clk_get %s ref=%d\n", desc->name, state->refcount);
#endif
    return 0;
}

static int k230_clk_release_one(sysctl_clk_node_e node)
{
    const struct k230_clk_desc *desc = k230_clk_desc_find(node);
    struct k230_clk_state *state = &k230_clk_state[node];
    bool parent_vote = false;
    bool old_boot_parent_vote;
    bool disable = false;
    int old_parent;
    rt_base_t level;

    if (desc == RT_NULL)
        return -K230_PM_EINVAL;

    level = rt_hw_interrupt_disable();
    if (state->refcount == 0) {
        rt_hw_interrupt_enable(level);
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: clk_put %s underflow\n", desc->name);
#endif
        return -K230_PM_EUNDERFLOW;
    }

    old_parent = state->parent;
    old_boot_parent_vote = state->boot_parent_vote;
    state->refcount--;
    if (state->parent_refcount > 0) {
        state->parent_refcount--;
        parent_vote = true;
    }
    if (state->refcount == 0) {
        disable = (desc->flags & K230_CLK_F_ALWAYS_ON) == 0U;
        state->parent = K230_CLK_PARENT_NONE;
        state->boot_parent_vote = false;
    }
    rt_hw_interrupt_enable(level);

    if (disable && !k230_clk_hw_set_enabled(desc, false)) {
        level = rt_hw_interrupt_disable();
        state->refcount++;
        if (parent_vote)
            state->parent_refcount++;
        state->parent = old_parent;
        state->boot_parent_vote = old_boot_parent_vote;
        state->owner = "poweroff-failed";
        rt_hw_interrupt_enable(level);
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: clk %s power-down timeout\n", desc->name);
#endif
        return -K230_PM_ETIMEOUT;
    }

#ifdef K230_PM_DEBUG
    rt_kprintf("k230_pm: clk_put %s ref=%d\n", desc->name, state->refcount);
#endif
    return 0;
}

static int k230_clk_check_ready(void)
{
    if (!k230_clk_ready) {
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: clock core not initialized\n");
#endif
        return -K230_PM_ECTX;
    }

    if (rt_interrupt_get_nest() != 0U)
        return -K230_PM_ECTX;

    return 0;
}

int k230_clk_get(sysctl_clk_node_e node)
{
    sysctl_clk_node_e path[K230_CLK_PATH_MAX];
    int parents[K230_CLK_PATH_MAX];
    int count;
    int ret;
    int i;

    ret = k230_clk_check_ready();
    if (ret != 0)
        return ret;

    rt_mutex_take(&k230_clk_mutex, RT_WAITING_FOREVER);
    ret = k230_clk_collect_path(node, path, parents, &count);
    if (ret == 0) {
        for (i = count - 1; i >= 0; i--) {
            ret = k230_clk_acquire_one(path[i], parents[i]);
            if (ret != 0) {
                while (++i < count)
                    (void)k230_clk_release_one(path[i]);
                break;
            }
        }
    }
    rt_mutex_release(&k230_clk_mutex);

    return ret;
}

int k230_clk_put(sysctl_clk_node_e node)
{
    sysctl_clk_node_e path[K230_CLK_PATH_MAX];
    int parents[K230_CLK_PATH_MAX];
    int count;
    int ret;
    int i;
    int j;

    ret = k230_clk_check_ready();
    if (ret != 0)
        return ret;

    rt_mutex_take(&k230_clk_mutex, RT_WAITING_FOREVER);
    ret = k230_clk_collect_path(node, path, parents, &count);
    if (ret == 0) {
        for (i = 0; i < count; i++) {
            if (k230_clk_state[path[i]].refcount == 0 ||
                (parents[i] != K230_CLK_PARENT_NONE &&
                 k230_clk_state[path[i]].parent_refcount == 0)) {
#ifdef K230_PM_DEBUG
                rt_kprintf("k230_pm: clk_put %s underflow\n", k230_clk_name(path[i]));
#endif
                ret = -K230_PM_EUNDERFLOW;
                break;
            }
        }
    }

    if (ret == 0) {
        for (i = 0; i < count; i++) {
            ret = k230_clk_release_one(path[i]);
            if (ret != 0) {
                for (j = i - 1; j >= 0; j--)
                    (void)k230_clk_acquire_one(path[j], parents[j]);
                break;
            }
        }
    }
    rt_mutex_release(&k230_clk_mutex);

    return ret;
}

static bool k230_clk_hw_set_parent_div(sysctl_clk_node_e node,
    sysctl_clk_node_e parent, uint32_t numerator, uint32_t denominator)
{
    sysctl_clk_node_e old_parent;
    bool parent_changed = false;

    if (node == SYSCTL_CLK_CPU1_CORE_CLK_ROOT || node == SYSCTL_CLK_AI_CORE_CLK)
        return sysctl_media_clk_set_leaf_parent_div_raw(node, parent, numerator,
            denominator);

    old_parent = sysctl_clk_get_leaf_parent(node);
    if (old_parent != parent) {
        if (!sysctl_clk_set_leaf_parent(node, parent))
            return false;
        parent_changed = true;
    }

    if (sysctl_clk_set_leaf_div(node, numerator, denominator))
        return true;

    if (parent_changed)
        (void)sysctl_clk_set_leaf_parent(node, old_parent);
    return false;
}

int k230_clk_set_div(sysctl_clk_node_e node, uint32_t numerator,
    uint32_t denominator)
{
    const struct k230_clk_desc *desc = k230_clk_desc_find(node);
    int ret;

    ret = k230_clk_check_ready();
    if (ret != 0)
        return ret;
    if (desc == RT_NULL || denominator == 0)
        return -K230_PM_EINVAL;

    rt_mutex_take(&k230_clk_mutex, RT_WAITING_FOREVER);
    ret = sysctl_clk_set_leaf_div(node, numerator, denominator) ?
        0 : -K230_PM_EINVAL;
    rt_mutex_release(&k230_clk_mutex);

    return ret;
}

int k230_clk_set_parent_div(sysctl_clk_node_e node, sysctl_clk_node_e parent,
    uint32_t numerator, uint32_t denominator)
{
    sysctl_clk_node_e parent_owner = node;
    const struct k230_clk_desc *desc;
    struct k230_clk_state *state;
    sysctl_clk_node_e new_path[K230_CLK_PATH_MAX];
    sysctl_clk_node_e old_path[K230_CLK_PATH_MAX];
    int new_parents[K230_CLK_PATH_MAX];
    int old_parents[K230_CLK_PATH_MAX];
    int new_count = 0;
    int old_count = 0;
    int acquired_votes = 0;
    int release_error = 0;
    int transfer_votes;
    int new_votes;
    int old_parent;
    int ret;
    int vote;
    int i;
    bool establish_boot_parent_vote = false;

    ret = k230_clk_check_ready();
    if (ret != 0)
        return ret;

    /* CPU1_CLK_CFG keeps the mux on cpu1_core_clk_src and the divider on
     * cpu1_core_clk_root.  Parent-vote accounting therefore belongs to the
     * source node even though DVFS changes/readbacks the root node. */
    if (node == SYSCTL_CLK_CPU1_CORE_CLK_ROOT)
        parent_owner = SYSCTL_CLK_CPU1_CORE_CLK_SRC;

    desc = k230_clk_desc_find(parent_owner);
    if (desc == RT_NULL || !k230_clk_valid(parent))
        return -K230_PM_EINVAL;

    /* A fixed-parent clock still supports divider-only DVFS steps. */
    if ((desc->flags & K230_CLK_F_DYNAMIC_PARENT) == 0U) {
        if (sysctl_clk_get_leaf_parent(node) != parent)
            return -K230_PM_EINVAL;
        return k230_clk_set_div(node, numerator, denominator);
    }

    rt_mutex_take(&k230_clk_mutex, RT_WAITING_FOREVER);
    state = &k230_clk_state[parent_owner];
    old_parent = k230_clk_parent_cached(parent_owner);
    transfer_votes = state->parent_refcount;

    if (old_parent != parent) {
        if (transfer_votes > 0 && old_parent == K230_CLK_PARENT_NONE) {
            ret = -K230_PM_EINVAL;
            goto out;
        }

        new_votes = transfer_votes;
        if (new_votes == 0 && state->refcount > 0) {
            new_votes = 1;
            establish_boot_parent_vote = true;
        }

        if (new_votes > 0) {
            ret = k230_clk_collect_path(parent, new_path, new_parents,
                &new_count);
            if (ret != 0)
                goto out;
        }

        if (transfer_votes > 0) {
            ret = k230_clk_collect_path((sysctl_clk_node_e)old_parent,
                old_path, old_parents, &old_count);
            if (ret != 0)
                goto out;

            for (i = 0; i < old_count; i++) {
                struct k230_clk_state *old_state =
                    &k230_clk_state[old_path[i]];

                if (old_state->refcount < transfer_votes ||
                    (old_parents[i] != K230_CLK_PARENT_NONE &&
                     old_state->parent_refcount < transfer_votes)) {
                    ret = -K230_PM_EUNDERFLOW;
                    goto out;
                }
            }
        }

        for (vote = 0; vote < new_votes; vote++) {
            for (i = new_count - 1; i >= 0; i--) {
                ret = k230_clk_acquire_one(new_path[i], new_parents[i]);
                if (ret != 0) {
                    while (++i < new_count)
                        (void)k230_clk_release_one(new_path[i]);
                    goto rollback_new;
                }
            }
            acquired_votes++;
        }
    } else {
        new_votes = 0;
    }

    if (!k230_clk_hw_set_parent_div(node, parent, numerator, denominator)) {
        ret = -K230_PM_EINVAL;
        goto rollback_new;
    }

    if (state->refcount > 0)
        state->parent = parent;
    if (establish_boot_parent_vote) {
        state->parent_refcount++;
        state->boot_parent_vote = true;
    }

    if (old_parent != parent) {
        for (vote = 0; vote < transfer_votes; vote++) {
            for (i = 0; i < old_count; i++) {
                int put_ret = k230_clk_release_one(old_path[i]);

                if (put_ret != 0 && release_error == 0)
                    release_error = put_ret;
            }
        }

    }

    if (release_error != 0) {
        ret = release_error;
        goto out;
    }

    ret = 0;
    goto out;

rollback_new:
    while (acquired_votes > 0) {
        acquired_votes--;
        for (i = 0; i < new_count; i++)
            (void)k230_clk_release_one(new_path[i]);
    }
out:
    rt_mutex_release(&k230_clk_mutex);
    return ret;
}

int k230_clk_refcount(sysctl_clk_node_e node)
{
    int refcount;
    rt_base_t level;

    if (node >= SYSCTL_CLK_NODE_MAX)
        return -K230_PM_EINVAL;

    level = rt_hw_interrupt_disable();
    refcount = k230_clk_state[node].refcount;
    rt_hw_interrupt_enable(level);

    return refcount;
}

bool k230_clk_boot_pinned(sysctl_clk_node_e node)
{
    bool pinned;
    rt_base_t level;

    if (node >= SYSCTL_CLK_NODE_MAX)
        return false;

    level = rt_hw_interrupt_disable();
    pinned = k230_clk_state[node].boot_pinned;
    rt_hw_interrupt_enable(level);

    return pinned;
}


static const struct k230_pd_desc *k230_pd_desc_find(sysctl_pwr_domain_e pd)
{
    unsigned int i;

    for (i = 0; i < k230_pd_desc_count; i++) {
        if (k230_pd_desc[i].pd == pd)
            return &k230_pd_desc[i];
    }

    return RT_NULL;
}

static bool k230_pd_valid(sysctl_pwr_domain_e pd)
{
    return pd >= 0 && pd < SYSCTL_PD_MAX && k230_pd_desc_find(pd) != RT_NULL;
}

static int k230_pd_check_ready(void)
{
    if (!k230_clk_ready) {
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: power-domain core not initialized\n");
#endif
        return -K230_PM_ECTX;
    }

    if (rt_interrupt_get_nest() != 0U)
        return -K230_PM_ECTX;

    return 0;
}

static bool k230_pd_hw_set_lpi(sysctl_pwr_domain_e pd, bool enable)
{
    return sysctl_pwr_set_lpi(pd, enable);
}

static bool k230_pd_hw_go_idle(sysctl_pwr_domain_e pd)
{
    /* The CPU PCU performs its own WFI/NoC-idle sequence after PWR_OFF_REQ.
     * A standalone LPI request can stall while the parked hart is in WFI and
     * prevents the proven power-off request from ever being issued.
     */
    if (pd == SYSCTL_PD_CPU0 || pd == SYSCTL_PD_CPU1)
        return true;

    return k230_pd_hw_set_lpi(pd, false);
}

static bool k230_pd_hw_set_power(sysctl_pwr_domain_e pd, bool enable)
{
    if (enable) {
        /*
         * PWR_UP_REQ only brings the power domain out of POWER-OFF.  The TRM
         * exposes a separate LPI_EXIT_IDLE trigger for reconnecting the NoC
         * data path.  Register accesses may work while that path is still
         * idle, but masters such as ISP DMA will not transfer frame data.
         */
        if (!sysctl_pwr_set_power(pd, true))
            return false;

        /* CPU NoC state is managed by the CPU/PCU wake-up sequence. */
        if (pd == SYSCTL_PD_CPU0 || pd == SYSCTL_PD_CPU1)
            return true;

        if (!k230_pd_hw_set_lpi(pd, true)) {
            /* LPI never left idle, so it is safe to return the domain off. */
            (void)sysctl_pwr_set_power(pd, false);
            return false;
        }
        return true;
    }

    if (!k230_pd_hw_go_idle(pd))
        return false;
    return sysctl_pwr_set_power(pd, false);
}

int k230_pd_get(sysctl_pwr_domain_e pd)
{
    const struct k230_pd_desc *desc = k230_pd_desc_find(pd);
    struct k230_pd_state *state;
    int ret;

    ret = k230_pd_check_ready();
    if (ret != 0)
        return ret;
    if (!k230_pd_valid(pd))
        return -K230_PM_EINVAL;

    state = &k230_pd_state[pd];
    rt_mutex_take(&k230_pd_mutex, RT_WAITING_FOREVER);
    if (state->emergency_off) {
        rt_mutex_release(&k230_pd_mutex);
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: pd_get %s blocked by emergency-off latch\n",
            desc->name);
#endif
        return -K230_PM_ECTX;
    }

    if (state->refcount >= K230_PD_REF_MAX) {
        rt_mutex_release(&k230_pd_mutex);
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: pd %s refcount overflow\n", desc->name);
#endif
        return -K230_PM_EINVAL;
    }

    if (state->refcount == 0 && !k230_pd_hw_set_power(pd, true)) {
        rt_mutex_release(&k230_pd_mutex);
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: pd_get %s power-up failed\n", desc->name);
#endif
        return -K230_PM_ETIMEOUT;
    }

    state->refcount++;
    rt_mutex_release(&k230_pd_mutex);

#ifdef K230_PM_DEBUG
    rt_kprintf("k230_pm: pd_get %s ref=%d\n", desc->name, state->refcount);
#endif
    return 0;
}

static int k230_pd_put_internal(sysctl_pwr_domain_e pd)
{
    const struct k230_pd_desc *desc = k230_pd_desc_find(pd);
    struct k230_pd_state *state;
    int ret;

    ret = k230_pd_check_ready();
    if (ret != 0)
        return ret;
    if (!k230_pd_valid(pd))
        return -K230_PM_EINVAL;

    state = &k230_pd_state[pd];
    rt_mutex_take(&k230_pd_mutex, RT_WAITING_FOREVER);
    if (state->refcount == 0) {
        rt_mutex_release(&k230_pd_mutex);
#ifdef K230_PM_DEBUG
        rt_kprintf("k230_pm: pd_put %s underflow\n", desc->name);
#endif
        return -K230_PM_EUNDERFLOW;
    }

    state->refcount--;
    if (state->refcount == 0 && (desc->flags & K230_PD_F_ALWAYS_ON) == 0U) {
        bool powered_off = k230_pd_hw_set_power(pd, false);

        if (!powered_off) {
            state->refcount++;
            rt_mutex_release(&k230_pd_mutex);
#ifdef K230_PM_DEBUG
            rt_kprintf("k230_pm: pd_put %s power-down failed\n", desc->name);
#endif
            return -K230_PM_ETIMEOUT;
        }
    }
    rt_mutex_release(&k230_pd_mutex);

#ifdef K230_PM_DEBUG
    rt_kprintf("k230_pm: pd_put %s ref=%d\n", desc->name, state->refcount);
#endif
    return 0;
}

int k230_pd_put(sysctl_pwr_domain_e pd)
{
    return k230_pd_put_internal(pd);
}

int k230_pd_refcount(sysctl_pwr_domain_e pd)
{
    int refcount;

    if (pd >= SYSCTL_PD_MAX)
        return -K230_PM_EINVAL;

    rt_mutex_take(&k230_pd_mutex, RT_WAITING_FOREVER);
    refcount = k230_pd_state[pd].refcount;
    rt_mutex_release(&k230_pd_mutex);

    return refcount;
}

bool k230_pd_boot_pinned(sysctl_pwr_domain_e pd)
{
    bool pinned;

    if (pd >= SYSCTL_PD_MAX)
        return false;

    rt_mutex_take(&k230_pd_mutex, RT_WAITING_FOREVER);
    pinned = k230_pd_state[pd].boot_pinned;
    rt_mutex_release(&k230_pd_mutex);

    return pinned;
}

bool k230_pd_emergency_offed(sysctl_pwr_domain_e pd)
{
    bool offed;

    if (pd >= SYSCTL_PD_MAX)
        return false;

    rt_mutex_take(&k230_pd_mutex, RT_WAITING_FOREVER);
    offed = k230_pd_state[pd].emergency_off;
    rt_mutex_release(&k230_pd_mutex);

    return offed;
}

bool k230_pd_hw_powered(sysctl_pwr_domain_e pd)
{
    volatile uint32_t *state;

    switch (pd) {
    case SYSCTL_PD_CPU0:
        state = &sysctl_pwr->cpu0_pwr_lpi_state;
        break;
    case SYSCTL_PD_CPU1:
        state = &sysctl_pwr->cpu1_pwr_lpi_state;
        break;
    case SYSCTL_PD_AI:
        state = &sysctl_pwr->ai_pwr_lpi_state;
        break;
    case SYSCTL_PD_DISP:
        state = &sysctl_pwr->disp_lpi_state;
        break;
    case SYSCTL_PD_SHRM:
        state = &sysctl_pwr->shrm_pwr_lpi_state;
        break;
    case SYSCTL_PD_VPU:
        state = &sysctl_pwr->vpu_lpi_state;
        break;
    case SYSCTL_PD_MCTL:
        state = &sysctl_pwr->mctl_lpi_state;
        break;
    case SYSCTL_PD_DPU:
        state = &sysctl_pwr->dpu_pwr_lpi_state;
        break;
    case SYSCTL_PD_HI:
        state = &sysctl_pwr->hi_lpi_state;
        break;
    case SYSCTL_PD_LS:
        state = &sysctl_pwr->ls_lpi_state;
        break;
    case SYSCTL_PD_SEC:
        state = &sysctl_pwr->sec_pwr_lpi_state;
        break;
    case SYSCTL_PD_ISP:
        state = &sysctl_pwr->isp_pwr_lpi_state;
        break;
    case SYSCTL_PD_PMU:
        state = &sysctl_pwr->pmu_pwr_lpi_state;
        break;
    default:
        return false;
    }

    /* MCTL_LPI_STAT is a 5-bit state machine; 0x6 is PowerOff. */
    if (pd == SYSCTL_PD_MCTL)
        return ((*state & 0x1fU) != 0x6U);

    /* Only 01 means the domain is definitively powered off. Treat
     * transitional or contradictory states as powered until hardware settles. */
    return ((*state & 0x3U) != 0x1U);
}


/*
 * The block we booted from was brought up by the boot ROM/SPL and must not be
 * hard-reset at runtime: for the SDIO (DWC MSHC) and QSPI controllers a sysctl
 * block reset wipes analog PHY sampling state that software cannot reliably
 * restore, e.g. the SDIO DAT sample path comes up dead on roughly 1 in 7 boots
 * while the command line still works. A block we did not boot from was never
 * touched by the boot stage, so resetting it to a known state is safe.
 */
static bool k230_pm_dev_is_boot_medium(enum k230_pm_dev_id id)
{
    switch (sysctl_boot_get_boot_mode()) {
    case SYSCTL_BOOT_EMMC:      return id == K230_PM_DEV_SDIO0;
    case SYSCTL_BOOT_SDCARD:    return id == K230_PM_DEV_SDIO1;
    case SYSCTL_BOOT_NORFLASH:
    case SYSCTL_BOOT_NANDFLASH: return id == K230_PM_DEV_SPI0;
    default:                    return false;
    }
}

int k230_pm_get(enum k230_pm_dev_id dev)
{
    const struct k230_pm_dev_desc *desc = k230_pm_dev_desc_find(dev);
    struct k230_pm_dev_state *state;
    bool pd_acquired = false;
    unsigned int acquired = 0;
    int ret = 0;
    unsigned int i;

    if (desc == RT_NULL || desc->id < 0 || desc->id >= K230_PM_DEV_ID_MAX)
        return -K230_PM_EINVAL;

    ret = k230_pd_check_ready();
    if (ret != 0)
        return ret;

    state = &k230_pm_dev_state[desc->id];

    rt_mutex_take(&k230_pm_mutex, RT_WAITING_FOREVER);

    if (state->refcount >= K230_PM_DEV_REF_MAX) {
        ret = -K230_PM_EINVAL;
        goto out;
    }

    if (state->refcount > 0) {
        state->refcount++;
        goto out;
    }

    /*
     * The K230 power controller needs the domain's bus/config clock while it
     * performs its LPI and power-state handshakes.  Clock gates live in the
     * always-on sysctl block, so they can and must be opened before PWR_UP_REQ.
     */
    for (i = 0; i < desc->clock_count; i++) {
        if (desc->clocks[i] == SYSCTL_CLK_NODE_MAX)
            continue;
        ret = k230_clk_get(desc->clocks[i]);
        if (ret != 0) {
            rt_kprintf("k230_pm: get %s clock[%u] %s failed: %d\n",
                desc->name, i, k230_clk_name(desc->clocks[i]), ret);
            goto rollback_clks;
        }
        acquired++;
    }

    if (desc->pd >= 0) {
        ret = k230_pd_get((sysctl_pwr_domain_e)desc->pd);
        if (ret != 0) {
            rt_kprintf("k230_pm: get %s power-domain %d failed: %d\n",
                desc->name, desc->pd, ret);
            goto rollback_clks;
        }
        pd_acquired = true;
    }

    if (desc->reset >= 0 && !k230_pm_dev_is_boot_medium(desc->id) &&
        !sysctl_reset((sysctl_reset_e)desc->reset)) {
        rt_kprintf("k230_pm: get %s reset %d timed out\n",
            desc->name, desc->reset);
        ret = -K230_PM_ETIMEOUT;
        goto rollback_pd;
    }

    state->refcount = 1;
    goto out;

rollback_pd:
    if (pd_acquired)
        (void)k230_pd_put((sysctl_pwr_domain_e)desc->pd);
rollback_clks:
    while (acquired > 0) {
        acquired--;
        if (desc->clocks[acquired] != SYSCTL_CLK_NODE_MAX)
            (void)k230_clk_put(desc->clocks[acquired]);
    }
out:
    rt_mutex_release(&k230_pm_mutex);
    return ret;
}

int k230_pm_put(enum k230_pm_dev_id dev)
{
    const struct k230_pm_dev_desc *desc = k230_pm_dev_desc_find(dev);
    struct k230_pm_dev_state *state;
    sysctl_clk_node_e released[K230_PM_MAX_DEV_CLKS];
    bool pd_released = false;
    unsigned int released_count = 0;
    int ret = 0;
    unsigned int i;

    if (desc == RT_NULL || desc->id < 0 || desc->id >= K230_PM_DEV_ID_MAX)
        return -K230_PM_EINVAL;

    ret = k230_pd_check_ready();
    if (ret != 0)
        return ret;

    state = &k230_pm_dev_state[desc->id];

    rt_mutex_take(&k230_pm_mutex, RT_WAITING_FOREVER);

    if (state->refcount == 0) {
        ret = -K230_PM_EUNDERFLOW;
        goto out;
    }

    if (state->refcount > 1) {
        state->refcount--;
        goto out;
    }

    /*
     * Keep the bus/config clocks running through the final LPI and power-off
     * acknowledgement.  Once the domain is confirmed off, release its clocks
     * in reverse order.
     */
    if (desc->pd >= 0) {
        ret = k230_pd_put((sysctl_pwr_domain_e)desc->pd);
        if (ret != 0)
            goto out;
        pd_released = true;
    }

    for (i = desc->clock_count; i > 0; i--) {
        sysctl_clk_node_e clk = desc->clocks[i - 1];

        if (clk == SYSCTL_CLK_NODE_MAX)
            continue;
        ret = k230_clk_put(clk);
        if (ret != 0)
            goto rollback_clks;
        released[released_count++] = clk;
    }

    state->refcount = 0;
    goto out;

rollback_clks:
    while (released_count > 0) {
        released_count--;
        (void)k230_clk_get(released[released_count]);
    }
    if (pd_released)
        (void)k230_pd_get((sysctl_pwr_domain_e)desc->pd);
out:
    rt_mutex_release(&k230_pm_mutex);
    return ret;
}

int k230_pm_refcount(enum k230_pm_dev_id dev)
{
    const struct k230_pm_dev_desc *desc = k230_pm_dev_desc_find(dev);
    int refcount;

    if (desc == RT_NULL || desc->id < 0 || desc->id >= K230_PM_DEV_ID_MAX)
        return -K230_PM_EINVAL;

    rt_mutex_take(&k230_pm_mutex, RT_WAITING_FOREVER);
    refcount = k230_pm_dev_state[desc->id].refcount;
    rt_mutex_release(&k230_pm_mutex);

    return refcount;
}

int k230_pm_emergency_off(enum k230_pm_dev_id dev)
{
    const struct k230_pm_dev_desc *desc = k230_pm_dev_desc_find(dev);
    struct k230_pd_state *state;
    int ret;

    if (desc == RT_NULL || desc->pd < 0)
        return -K230_PM_EINVAL;

    ret = k230_pd_check_ready();
    if (ret != 0)
        return ret;

    rt_mutex_take(&k230_pm_mutex, RT_WAITING_FOREVER);
    rt_mutex_take(&k230_pd_mutex, RT_WAITING_FOREVER);

    state = &k230_pd_state[desc->pd];
    state->emergency_off = true;
    state->owner = "emergency";
    ret = k230_pd_hw_set_power((sysctl_pwr_domain_e)desc->pd, false) ? 0 :
        -K230_PM_ETIMEOUT;

    rt_mutex_release(&k230_pd_mutex);
    rt_mutex_release(&k230_pm_mutex);

    rt_kprintf("k230_pm: emergency off %s ret=%d ref=%d\n", desc->name, ret,
        state->refcount);
    return ret;
}
static int k230_clk_release_boot_pin_one(sysctl_clk_node_e node)
{
    const struct k230_clk_desc *desc = k230_clk_desc_find(node);
    struct k230_clk_state *state;
    sysctl_clk_node_e parent_path[K230_CLK_PATH_MAX];
    int parent_parents[K230_CLK_PATH_MAX];
    int parent_count = 0;
    int old_parent;
    bool disable = false;
    bool parent_vote;
    int ret;
    int i;
    int j;
    rt_base_t level;

    if (desc == RT_NULL)
        return -K230_PM_EINVAL;

    state = &k230_clk_state[node];
    level = rt_hw_interrupt_disable();
    if (!state->boot_pinned || state->refcount == 0) {
        rt_hw_interrupt_enable(level);
        return 0;
    }

    old_parent = state->parent;
    parent_vote = state->boot_parent_vote && state->parent_refcount > 0 &&
        old_parent != K230_CLK_PARENT_NONE;
    rt_hw_interrupt_enable(level);

    if (parent_vote) {
        ret = k230_clk_collect_path((sysctl_clk_node_e)old_parent,
            parent_path, parent_parents, &parent_count);
        if (ret != 0)
            return ret;
    }

    level = rt_hw_interrupt_disable();
    state->boot_pinned = false;
    state->boot_parent_vote = false;
    state->refcount--;
    if (parent_vote)
        state->parent_refcount--;
    if (state->refcount == 0) {
        disable = (desc->flags & K230_CLK_F_ALWAYS_ON) == 0U;
        state->parent = K230_CLK_PARENT_NONE;
    }
    rt_hw_interrupt_enable(level);

    if (disable && !k230_clk_hw_set_enabled(desc, false)) {
        level = rt_hw_interrupt_disable();
        state->boot_pinned = true;
        state->boot_parent_vote = parent_vote;
        state->refcount++;
        if (parent_vote)
            state->parent_refcount++;
        state->parent = old_parent;
        rt_hw_interrupt_enable(level);
        return -K230_PM_ETIMEOUT;
    }

    if (parent_vote) {
        for (i = 0; i < parent_count; i++) {
            ret = k230_clk_release_one(parent_path[i]);
            if (ret != 0) {
                for (j = i - 1; j >= 0; j--)
                    (void)k230_clk_acquire_one(parent_path[j],
                        parent_parents[j]);
                if (disable)
                    (void)k230_clk_hw_set_enabled(desc, true);
                level = rt_hw_interrupt_disable();
                state->boot_pinned = true;
                state->boot_parent_vote = true;
                state->refcount++;
                state->parent_refcount++;
                state->parent = old_parent;
                rt_hw_interrupt_enable(level);
                return ret;
            }
        }
    }

#ifdef K230_PM_DEBUG
    rt_kprintf("k230_pm: release boot clk %s ref=%d\n", desc->name, state->refcount);
#endif
    return 1;
}

static int k230_clk_power_domain(sysctl_clk_node_e node)
{
    unsigned int i;
    unsigned int j;

    switch (node) {
    case SYSCTL_CLK_CPU0_CORE_CLK_ROOT:
    case SYSCTL_CLK_CPU0_CORE_CLK:
    case SYSCTL_CLK_CPU0_PLICLK:
    case SYSCTL_CLK_CPU0_ACLK:
    case SYSCTL_CLK_TDI_PCLK:
    case SYSCTL_CLK_CPU0_PCLK:
        return SYSCTL_PD_CPU0;
    default:
        break;
    }

    for (i = 0; i < k230_pm_dev_desc_count; i++) {
        const struct k230_pm_dev_desc *dev = &k230_pm_dev_desc[i];

        if (dev->pd < 0)
            continue;
        for (j = 0; j < dev->clock_count; j++) {
            if (dev->clocks[j] == node)
                return dev->pd;
        }
    }

    return K230_PD_NONE;
}

static int k230_pd_release_boot_pin_one(sysctl_pwr_domain_e pd)
{
    const struct k230_pd_desc *desc = k230_pd_desc_find(pd);
    struct k230_pd_state *state;

    if (desc == RT_NULL)
        return -K230_PM_EINVAL;

    state = &k230_pd_state[pd];
    if (!state->boot_pinned || state->refcount == 0)
        return 0;

    /* Boot-owned domains are idle and still clocked. Let the PCU perform its
     * integrated idle/power-off sequence; standalone CPU0 and DISP LPI
     * requests have both failed during the boot handoff on real hardware.
     */
    if (state->refcount == 1 && (desc->flags & K230_PD_F_ALWAYS_ON) == 0U) {
        if (!sysctl_pwr_set_power(pd, false))
            return -K230_PM_ETIMEOUT;
    }

    state->boot_pinned = false;
    state->refcount--;

#ifdef K230_PM_DEBUG
    rt_kprintf("k230_pm: release boot pd %s ref=%d\n", desc->name, state->refcount);
#endif
    return 1;
}

int k230_pm_release_boot_pins(void)
{
    uint32_t failed_pds = 0;
    unsigned int i;
    int released = 0;
    int first_error = 0;
    int ret;

    ret = k230_clk_check_ready();
    if (ret != 0)
        return ret;

    rt_mutex_take(&k230_pm_mutex, RT_WAITING_FOREVER);

    /* SPL leaves boot domains running. Retire every boot vote independently
     * so one failed domain cannot block CPU0 or another media domain.
     */
    rt_mutex_take(&k230_pd_mutex, RT_WAITING_FOREVER);
    for (i = 0; i < k230_pd_desc_count; i++) {
        const struct k230_pd_desc *desc = &k230_pd_desc[i];

        if ((desc->flags & K230_PD_F_BOOT_PINNED_OK) == 0U)
            continue;

        ret = k230_pd_release_boot_pin_one(desc->pd);
        if (ret < 0) {
            failed_pds |= 1U << (unsigned int)desc->pd;
            if (first_error == 0)
                first_error = ret;
            rt_kprintf("k230_pm: boot pd release failed: %s (%d)\n",
                desc->name, ret);
            continue;
        }
        released += ret;
    }
    rt_mutex_release(&k230_pd_mutex);

    rt_mutex_take(&k230_clk_mutex, RT_WAITING_FOREVER);
    /* Descriptors are parent-first; release children before their sources. */
    for (i = k230_clk_desc_count; i > 0; i--) {
        const struct k230_clk_desc *desc = &k230_clk_desc[i - 1];
        int pd;

        if ((desc->flags & K230_CLK_F_BOOT_PINNED_OK) == 0U)
            continue;

        pd = k230_clk_power_domain(desc->node);
        if (pd >= 0 && (failed_pds & (1U << (unsigned int)pd)) != 0U)
            continue;

        ret = k230_clk_release_boot_pin_one(desc->node);
        if (ret < 0) {
            if (first_error == 0)
                first_error = ret;
            break;
        }
        released += ret;
    }
    rt_mutex_release(&k230_clk_mutex);

    rt_mutex_release(&k230_pm_mutex);
    return first_error != 0 ? first_error : released;
}

#ifdef K230_PM_AGGRESSIVE_DEFAULT
static int k230_pm_aggressive_late_init(void)
{
    int ret = k230_pm_release_boot_pins();

    return ret < 0 ? ret : 0;
}
INIT_APP_EXPORT(k230_pm_aggressive_late_init);
#endif

static int k230_pm_boot_init(void)
{
    unsigned int i;

    rt_mutex_init(&k230_clk_mutex, "k230clk", RT_IPC_FLAG_PRIO);
    rt_mutex_init(&k230_pd_mutex, "k230pd", RT_IPC_FLAG_PRIO);
    rt_mutex_init(&k230_pm_mutex, "k230pm", RT_IPC_FLAG_PRIO);

    for (i = 0; i < SYSCTL_CLK_NODE_MAX; i++)
        k230_clk_state[i].parent = K230_CLK_PARENT_NONE;

    for (i = 0; i < k230_clk_desc_count; i++) {
        const struct k230_clk_desc *desc = &k230_clk_desc[i];
        struct k230_clk_state *state = &k230_clk_state[desc->node];

        if (k230_clk_should_boot_pin(desc) && k230_clk_hw_enabled(desc->node)) {
            state->refcount = 1;
            state->boot_pinned = true;
            state->owner = "boot";
        }
    }

    for (i = 0; i < k230_pd_desc_count; i++) {
        const struct k230_pd_desc *desc = &k230_pd_desc[i];
        struct k230_pd_state *state = &k230_pd_state[desc->pd];

        if (k230_pd_hw_powered(desc->pd)) {
            state->refcount = 1;
            state->boot_pinned = true;
            state->owner = "boot";
        }
    }

    k230_clk_ready = true;
    return 0;
}
INIT_PREV_EXPORT(k230_pm_boot_init);
