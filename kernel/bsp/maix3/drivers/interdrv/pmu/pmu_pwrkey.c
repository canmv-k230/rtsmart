#include <stdbool.h>
#include <stdint.h>

#include "pmu_priv.h"

static uint32_t pmu_pwrkey_edge_mode(struct pmu_dev *pmu)
{
    return (pmu_readl(pmu, PMU_INT_DETECT_TYP) >> PMU_KEY_EDGE_OFFSET) &
           PMU_INT_TRIGGER_MASK;
}

static bool pmu_pwrkey_waits_press_edge(uint32_t edge_mode)
{
    return edge_mode == PMU_INT_TRIGGER_TYPE_MASK;
}

static bool pmu_pwrkey_waits_release_edge(uint32_t edge_mode)
{
    return edge_mode ==
           (PMU_INT_TRIGGER_TYPE_MASK | PMU_INT_TRIGGER_EDGE_MASK);
}

static uint32_t pmu_edge_rising(uint32_t offset)
{
    return PMU_INT_TRIGGER_TYPE_MASK << offset;
}

static uint32_t pmu_edge_falling(uint32_t offset)
{
    return (PMU_INT_TRIGGER_TYPE_MASK | PMU_INT_TRIGGER_EDGE_MASK) << offset;
}

static void pmu_pwrkey_set_edge(struct pmu_dev *pmu, bool falling)
{
    uint32_t value;

    value = pmu_readl(pmu, PMU_INT_DETECT_TYP);
    value &= ~(PMU_INT_TRIGGER_MASK << PMU_KEY_EDGE_OFFSET);
    value |= falling ? pmu_edge_falling(PMU_KEY_EDGE_OFFSET) :
        pmu_edge_rising(PMU_KEY_EDGE_OFFSET);
    pmu_writel(pmu, value, PMU_INT_DETECT_TYP);
}

static void pmu_pwrkey_reset_state(struct pmu_dev *pmu)
{
    if (pmu->pwrkey.timer != RT_NULL)
        rt_timer_stop(pmu->pwrkey.timer);

    pmu->pwrkey.pressed = false;
    pmu->pwrkey.long_press_seen = false;
    pmu->pwrkey.release_seen = false;
    pmu->pwrkey.user_notified = false;
}

static bool pmu_pwrkey_shutdown_pending(struct pmu_dev *pmu)
{
    return pmu->pwrkey.release_seen || pmu->notify.ack_pending;
}

static void pmu_pwrkey_init_output_state(struct pmu_dev *pmu)
{
    rt_base_t level;
    uint32_t value;

    level = rt_hw_interrupt_disable();

    value = pmu_readl(pmu, PMU_OUT_EVENT_CTRL);
    value |= PMU_OUT_EVENT_TO_CTRL_0_INT_LOGIC;
    value |= PMU_OUT_EVENT_TO_CTRL_0_SW_REG;
    value &= ~PMU_OUT_EVENT_TO_CTRL_0_INT8;
    pmu_writel(pmu, value, PMU_OUT_EVENT_CTRL);

    value = pmu_readl(pmu, PMU_OUT_LOGIC_CTRL);
    value |= PMU_OUT_LOGIC_TO_CTRL_1_INT_LOGIC;
    value |= PMU_OUT_LOGIC_TO_CTRL_1_SW_REG;
    value &= ~PMU_OUT_LOGIC_TO_CTRL_1_INT8;
    value &= ~PMU_OUT_LOGIC_TO_CTRL_1_LOGIC_AND;
    pmu_writel(pmu, value, PMU_OUT_LOGIC_CTRL);

    value = pmu_readl(pmu, PMU_SYSCTRL_REG);
    value |= PMU_SYSCTRL_TO_CTRL_1_REG_O;
    pmu_writel(pmu, value, PMU_SYSCTRL_REG);

    rt_hw_interrupt_enable(level);
}

static void pmu_pwrkey_on_press(struct pmu_dev *pmu)
{
    if (pmu_pwrkey_shutdown_pending(pmu))
        return;

    pmu->pwrkey.pressed = true;
    pmu->pwrkey.long_press_seen = false;
    pmu->pwrkey.release_seen = false;
    pmu->pwrkey.user_notified = false;
    pmu_pwrkey_set_edge(pmu, true);
    rt_timer_stop(pmu->pwrkey.timer);
    rt_timer_start(pmu->pwrkey.timer);
}

static void pmu_pwrkey_on_release(struct pmu_dev *pmu)
{
    pmu->pwrkey.pressed = false;
    pmu_pwrkey_set_edge(pmu, false);

    if (!pmu->pwrkey.long_press_seen) {
        rt_timer_stop(pmu->pwrkey.timer);
        return;
    }

    pmu->pwrkey.release_seen = true;
    pmu_post_work(pmu, PMU_WORK_KEY_RELEASE);
}

static void pmu_pwrkey_timer_cb(void *parameter)
{
    struct pmu_dev *pmu = parameter;

    if (!pmu->pwrkey.pressed || pmu->pwrkey.long_press_seen)
        return;

    pmu->pwrkey.long_press_seen = true;
    pmu->pwrkey.release_seen = false;
    pmu->pwrkey.user_notified = false;
    pmu_post_work(pmu, PMU_WORK_KEY_LONG);
}

void pmu_pwrkey_handle_long_work(struct pmu_dev *pmu)
{
    rt_err_t ret;

    if (!pmu->pwrkey.long_press_seen)
        return;

    ret = pmu_notify_send(pmu, PMU_EVENT_LONG_PRESS, false);
    if (ret == RT_EOK) {
        pmu->pwrkey.user_notified = true;
        return;
    }

    pmu->pwrkey.user_notified = false;
    rt_kprintf("[pmu] worker: long-press notify unavailable (%d), wait key release then direct shutdown\n",
           ret);
}

void pmu_pwrkey_handle_release_work(struct pmu_dev *pmu)
{
    rt_err_t ret;

    if (!pmu->pwrkey.long_press_seen || !pmu->pwrkey.release_seen)
        return;

    if (!pmu->pwrkey.user_notified) {
        rt_kprintf("[pmu] worker: no userspace listener, direct shutdown on release\n");
        pmu_do_shutdown("power-key-release");
        return;
    }

    ret = pmu_notify_send(pmu, PMU_EVENT_KEY_RELEASE, true);
    if (ret == RT_EOK)
        return;

    rt_kprintf("[pmu] worker: release notify unavailable (%d), direct shutdown\n",
           ret);
    pmu_do_shutdown("power-key-release");
}

void pmu_pwrkey_irq(struct pmu_dev *pmu, uint32_t status)
{
    uint32_t edge_mode;

    if (!pmu->pwrkey.initialized)
        return;

    if ((status & PMU_IRQ_KEY_EDGE) == 0U)
        return;

    edge_mode = pmu_pwrkey_edge_mode(pmu);
    if (pmu_pwrkey_waits_press_edge(edge_mode))
        pmu_pwrkey_on_press(pmu);
    else if (pmu_pwrkey_waits_release_edge(edge_mode))
        pmu_pwrkey_on_release(pmu);
    else if (!pmu->pwrkey.pressed)
        pmu_pwrkey_on_press(pmu);
    else
        pmu_pwrkey_on_release(pmu);
}

static int pmu_configure_pwrkey(struct pmu_dev *pmu)
{
    uint32_t value;

    value = pmu_readl(pmu, PMU_INT0_LONG_PRESS_TRIGGER_VAL);
    if (value != PMU_PWRKEY_LONG_PRESS_TICKS)
        pmu_writel(pmu, PMU_PWRKEY_LONG_PRESS_TICKS,
               PMU_INT0_LONG_PRESS_TRIGGER_VAL);

    value = pmu_readl(pmu, PMU_INT0_LEVEL_DEBOUNCE_VAL);
    if (value != PMU_PWRKEY_DEBOUNCE_TICKS)
        pmu_writel(pmu, PMU_PWRKEY_DEBOUNCE_TICKS,
               PMU_INT0_LEVEL_DEBOUNCE_VAL);

    value = pmu_readl(pmu, PMU_INT0_TO_CTL_REGISTER);
    value &= ~PMU_CPU_IRQ_MASK;
    pmu_writel(pmu, value, PMU_INT0_TO_CTL_REGISTER);

    value = pmu_readl(pmu, PMU_INT1_TO_CTL_REGISTER);
    value &= ~PMU_CPU_IRQ_MASK;
    pmu_writel(pmu, value, PMU_INT1_TO_CTL_REGISTER);

    value = pmu_readl(pmu, PMU_INT0_TO_CPU_REGISTER);
    value &= ~PMU_CPU_IRQ_MASK;
    value |= PMU_IRQ_KEY_EDGE;
    pmu_writel(pmu, value, PMU_INT0_TO_CPU_REGISTER);

    value = pmu_readl(pmu, PMU_INT_DETECT_EN);
    value &= ~PMU_DET_SOURCE_MASK;
    value |= PMU_DET_KEY_EDGE;
    pmu_writel(pmu, value, PMU_INT_DETECT_EN);

    value = PMU_CLR_KEY_LONG;
    value |= PMU_CLR_KEY_SHUTDOWN;
    value |= PMU_CLR_KEY_SHORT;
    value |= PMU_CLR_KEY_EDGE;
    pmu_writel(pmu, value, PMU_INT_DETECT_CLR);
    pmu_pwrkey_set_edge(pmu, false);

    return RT_EOK;
}

int pmu_init_pwrkey(struct pmu_dev *pmu)
{
    int ret;

    if (pmu->pwrkey.initialized)
        return RT_EOK;

    pmu_pwrkey_init_output_state(pmu);

    (void)pmu_init_userdev(pmu);

    if (pmu->pwrkey.timer == RT_NULL) {
        pmu->pwrkey.timer = rt_timer_create(
            "pmupwr", pmu_pwrkey_timer_cb, pmu,
            rt_tick_from_millisecond(PMU_SOFT_SHUTDOWN_MS),
            RT_TIMER_FLAG_ONE_SHOT);
        if (pmu->pwrkey.timer == RT_NULL)
            return -RT_ENOMEM;
    }

    ret = pmu_configure_pwrkey(pmu);
    if (ret != RT_EOK)
        return ret;

    pmu_pwrkey_reset_state(pmu);
    pmu->pwrkey.initialized = true;
    return RT_EOK;
}
