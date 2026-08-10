#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pmu_priv.h"

#ifdef RT_USING_LWP
#include <lwp_pid.h>
#endif

#include "board.h"
#include "drv_gpio.h"
#include "ioremap.h"
#include "riscv_io.h"

struct pmu_dev g_pmu_dev;

struct pmu_irq_clear_map {
    uint32_t state;
    uint32_t clear;
};

struct pmu_pad_wakeup_cfg {
    uint32_t pad;
    uint32_t io_cfg_reg;
    uint32_t irq_mask;
    uint32_t detect_mask;
    uint32_t type_offset;
    uint32_t debounce_reg;
    uint32_t trigger;
    uint32_t bias;
    uint32_t debounce_ticks;
    const char *source;
};

static void pmu_irq_handler(int vector, void *param);

static const struct pmu_irq_clear_map g_pmu_irq_clear_map[] = {
    { PMU_IRQ_KEY_LONG, PMU_CLR_KEY_LONG },
    { PMU_IRQ_KEY_SHORT, PMU_CLR_KEY_SHORT },
    { PMU_IRQ_KEY_EDGE, PMU_CLR_KEY_EDGE },
    { PMU_IRQ_INT1_0, PMU_CLR_INT1_0 },
    { PMU_IRQ_INT1_1, PMU_CLR_INT1_1 },
    { PMU_IRQ_INT2, PMU_CLR_INT2 },
    { PMU_IRQ_INT3, PMU_CLR_INT3 },
    { PMU_IRQ_INT4, PMU_CLR_INT4 },
    { PMU_IRQ_INT5, PMU_CLR_INT5 },
    { PMU_IRQ_KEY_SHUTDOWN, PMU_CLR_KEY_SHUTDOWN },
};

static const struct pmu_pad_wakeup_cfg g_pmu_pad_wakeup_cfgs[] = {
#ifdef RT_PMU_SHUTDOWN_WAKEUP_PAD65
    { 65U, PMU_IO_CFG_1, PMU_IRQ_INT1_1, PMU_DET_INT1_1, PMU_INT1_EDGE_OFFSET,
      PMU_INT1_LEVEL_DEBOUNCE_VAL, RT_PMU_SHUTDOWN_WAKEUP_PAD65_TRIGGER,
      RT_PMU_SHUTDOWN_WAKEUP_PAD65_BIAS,
      RT_PMU_SHUTDOWN_WAKEUP_PAD65_DEBOUNCE_TICKS,
      RT_PMU_SHUTDOWN_WAKEUP_SOURCE_PAD65 },
#endif
#ifdef RT_PMU_SHUTDOWN_WAKEUP_PAD66
    { 66U, PMU_IO_CFG_2, PMU_IRQ_INT2, PMU_DET_INT2, PMU_INT2_EDGE_OFFSET,
      PMU_INT2_LEVEL_DEBOUNCE_VAL, RT_PMU_SHUTDOWN_WAKEUP_PAD66_TRIGGER,
      RT_PMU_SHUTDOWN_WAKEUP_PAD66_BIAS,
      RT_PMU_SHUTDOWN_WAKEUP_PAD66_DEBOUNCE_TICKS,
      RT_PMU_SHUTDOWN_WAKEUP_SOURCE_PAD66 },
#endif
#ifdef RT_PMU_SHUTDOWN_WAKEUP_PAD67
    { 67U, PMU_IO_CFG_3, PMU_IRQ_INT3, PMU_DET_INT3, PMU_INT3_EDGE_OFFSET,
      PMU_INT3_LEVEL_DEBOUNCE_VAL, RT_PMU_SHUTDOWN_WAKEUP_PAD67_TRIGGER,
      RT_PMU_SHUTDOWN_WAKEUP_PAD67_BIAS,
      RT_PMU_SHUTDOWN_WAKEUP_PAD67_DEBOUNCE_TICKS,
      RT_PMU_SHUTDOWN_WAKEUP_SOURCE_PAD67 },
#endif
#ifdef RT_PMU_SHUTDOWN_WAKEUP_PAD68
    { 68U, PMU_IO_CFG_4, PMU_IRQ_INT4, PMU_DET_INT4, PMU_INT4_EDGE_OFFSET,
      0U, RT_PMU_SHUTDOWN_WAKEUP_PAD68_TRIGGER,
      RT_PMU_SHUTDOWN_WAKEUP_PAD68_BIAS, 0U,
      RT_PMU_SHUTDOWN_WAKEUP_SOURCE_PAD68 },
#endif
#ifdef RT_PMU_SHUTDOWN_WAKEUP_PAD69
    { 69U, PMU_IO_CFG_5, PMU_IRQ_INT5, PMU_DET_INT5, PMU_INT5_EDGE_OFFSET,
      0U, RT_PMU_SHUTDOWN_WAKEUP_PAD69_TRIGGER,
      RT_PMU_SHUTDOWN_WAKEUP_PAD69_BIAS, 0U,
      RT_PMU_SHUTDOWN_WAKEUP_SOURCE_PAD69 },
#endif
};

struct pmu_dev *pmu_get_dev(void)
{
    return &g_pmu_dev;
}

static volatile rt_uint8_t *pmu_ioremap(volatile rt_uint8_t **base,
                    uintptr_t phys, size_t size,
                    const char *name)
{
    if (*base == RT_NULL) {
        *base = (volatile rt_uint8_t *)rt_ioremap((void *)phys, size);
        if (*base == RT_NULL)
            rt_kprintf("[pmu] ioremap %s failed\n", name);
    }

    return *base;
}

volatile rt_uint8_t *pmu_get_base(struct pmu_dev *pmu)
{
    return pmu_ioremap(&pmu->base, PMU_BASE_ADDR, PMU_IO_SIZE, "PMU");
}

volatile rt_uint8_t *pmu_get_pwr_base(struct pmu_dev *pmu)
{
    return pmu_ioremap(&pmu->pwr_base, PWR_BASE_ADDR, PWR_IO_SIZE, "PWR");
}

volatile rt_uint8_t *pmu_get_rtc_base(struct pmu_dev *pmu)
{
    return pmu_ioremap(&pmu->rtc.base, RTC_BASE_ADDR, RTC_IO_SIZE, "RTC");
}

uint32_t pmu_readl(struct pmu_dev *pmu, uint32_t reg)
{
    return readl(pmu_get_base(pmu) + reg);
}

void pmu_writel(struct pmu_dev *pmu, uint32_t value, uint32_t reg)
{
    writel(value, pmu_get_base(pmu) + reg);
}

void pmu_update_bits_locked(struct pmu_dev *pmu, uint32_t reg, uint32_t mask,
                bool enable)
{
    uint32_t value;

    value = pmu_readl(pmu, reg);
    if (enable)
        value |= mask;
    else
        value &= ~mask;
    pmu_writel(pmu, value, reg);
}

void pmu_set_detect_en_locked(struct pmu_dev *pmu, uint32_t mask, bool enable)
{
    pmu_update_bits_locked(pmu, PMU_INT_DETECT_EN, mask, enable);
}

int pmu_ensure_access(struct pmu_dev *pmu)
{
    volatile rt_uint8_t *base;
    volatile rt_uint8_t *pwr_base;
    uint32_t value;

    base = pmu_get_base(pmu);
    pwr_base = pmu_get_pwr_base(pmu);
    if ((base == RT_NULL) || (pwr_base == RT_NULL))
        return -RT_ERROR;

    value = readl(pwr_base + PMU_PWR_ISO_CTRL_REG);
    if (value & PMU_ISO_ACCESS_MASK)
        writel(value & ~PMU_ISO_ACCESS_MASK,
               pwr_base + PMU_PWR_ISO_CTRL_REG);

    return RT_EOK;
}

int pmu_current_pid(void)
{
#ifdef RT_USING_LWP
    return lwp_getpid();
#else
    return 0;
#endif
}

void pmu_do_shutdown(const char *source)
{
    g_pmu_dev.shutdown_source = source;
    rt_hw_cpu_shutdown();
}

static int pmu_install_irq(struct pmu_dev *pmu)
{
    if (pmu->irq_installed)
        return RT_EOK;

    rt_hw_interrupt_install(K230_PMU_IRQN, pmu_irq_handler,
                RT_NULL, "pmu");
    rt_hw_interrupt_umask(K230_PMU_IRQN);
    pmu->irq_installed = true;

    return RT_EOK;
}

void pmu_post_work(struct pmu_dev *pmu, uint32_t work)
{
    if (pmu->worker.initialized)
        rt_event_send(&pmu->worker.event, work);
}

static void pmu_worker_entry(void *parameter)
{
    struct pmu_dev *pmu = parameter;
    rt_uint32_t work = 0;

    while (1) {
        if (rt_event_recv(&pmu->worker.event,
                  PMU_WORK_KEY_LONG |
                  PMU_WORK_KEY_RELEASE |
                  PMU_WORK_CYCLE_SHUTDOWN,
                  RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                  RT_WAITING_FOREVER,
                  &work) != RT_EOK)
            continue;

        if (work & PMU_WORK_KEY_LONG)
            pmu_pwrkey_handle_long_work(pmu);
        if (work & PMU_WORK_KEY_RELEASE)
            pmu_pwrkey_handle_release_work(pmu);
        if (work & PMU_WORK_CYCLE_SHUTDOWN)
            pmu_cycle_handle_shutdown_work(pmu);
    }
}

int pmu_init_worker(struct pmu_dev *pmu)
{
    rt_err_t ret;

    if (pmu->worker.initialized)
        return RT_EOK;

    ret = rt_event_init(&pmu->worker.event, "pmuwrk", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
        return ret;

    pmu->worker.thread = rt_thread_create("pmu_wkr",
                          pmu_worker_entry,
                          pmu,
                          PMU_WORKER_STACK_SIZE,
                          PMU_WORKER_PRIORITY,
                          PMU_WORKER_TIMESLICE);
    if (pmu->worker.thread == RT_NULL)
        return -RT_ENOMEM;

    rt_thread_startup(pmu->worker.thread);
    pmu->worker.initialized = true;
    return RT_EOK;
}

static uint32_t pmu_irq_clear_mask(uint32_t status)
{
    uint32_t clear = 0U;
    rt_size_t index;

    for (index = 0; index < sizeof(g_pmu_irq_clear_map) /
                   sizeof(g_pmu_irq_clear_map[0]); index++) {
        if (status & g_pmu_irq_clear_map[index].state)
            clear |= g_pmu_irq_clear_map[index].clear;
    }

    return clear;
}

static uint32_t pmu_rtc_tick_fallback(struct pmu_dev *pmu, uint32_t status)
{
    uint32_t route;
    uint32_t detect_en;
    uint32_t int_ctrl;

    if ((status != 0U) || (pmu->rtc.base == RT_NULL))
        return 0U;

    route = pmu_readl(pmu, PMU_INT0_TO_CPU_REGISTER);
    detect_en = pmu_readl(pmu, PMU_INT_DETECT_EN);
    if (((route & PMU_IRQ_RTC_TICK) == 0U) ||
        ((detect_en & PMU_DET_RTC_TICK) == 0U))
        return 0U;

    int_ctrl = readl(pmu->rtc.base + offsetof(k230_rtc_regs_t, int_ctrl));
    if ((int_ctrl & PMU_RTC_TICK_EN_BIT) == 0U)
        return 0U;

    return PMU_IRQ_RTC_TICK;
}

static const struct pmu_pad_wakeup_cfg *pmu_get_shutdown_wakeup_pad(uint32_t pad)
{
    rt_size_t index;

    for (index = 0; index < sizeof(g_pmu_pad_wakeup_cfgs) /
                   sizeof(g_pmu_pad_wakeup_cfgs[0]); index++) {
        if (g_pmu_pad_wakeup_cfgs[index].pad ==
            pad)
            return &g_pmu_pad_wakeup_cfgs[index];
    }
    return RT_NULL;
}

int pmu_get_shutdown_wakeup_level(struct pmu_dev *pmu, uint32_t pad, int *level)
{
    const struct pmu_pad_wakeup_cfg *cfg;

    if ((pmu == RT_NULL) || (level == RT_NULL))
        return -RT_EINVAL;

    cfg = pmu_get_shutdown_wakeup_pad(pad);
    if (cfg == RT_NULL)
        return -RT_ENOSYS;

    /* Keep the configured wakeup PADs in GPIO mode during normal runtime. */
    *level = kd_pin_get_dr(cfg->pad) != 0U ? 1 : 0;
    return RT_EOK;
}

int pmu_get_shutdown_wakeup_source(struct pmu_dev *pmu, char *name,
                                   size_t size)
{
    size_t length;

    if ((pmu == RT_NULL) || (name == RT_NULL) || (size == 0U))
        return -RT_EINVAL;
    if (!pmu->wakeup_source_valid)
        return -RT_ENOSYS;
    if (pmu->wakeup_source_count != 1U)
        return -RT_EBUSY;
    length = strlen(pmu->wakeup_source_name) + 1U;
    if (length > size)
        return -RT_EINVAL;
    memcpy(name, pmu->wakeup_source_name, length);
    return RT_EOK;
}

static void pmu_capture_wakeup_source(struct pmu_dev *pmu)
{
    rt_size_t index;
    uint32_t status;

    pmu->wakeup_source_count = 0U;
    pmu->wakeup_source_name = RT_NULL;
    pmu->wakeup_source_valid = false;
    status = pmu_readl(pmu, PMU_INT_STATE_REG);

    if ((status & (PMU_IRQ_KEY_LONG | PMU_IRQ_KEY_SHUTDOWN)) != 0U) {
        pmu->wakeup_source_count++;
        pmu->wakeup_source_name = "long press key wake up";
        pmu->wakeup_source_valid = true;
    }

    if ((status & (PMU_IRQ_RTC_MASK | PMU_INT_STATE_RTC_INPUT_MASK)) != 0U) {
        pmu->wakeup_source_count++;
        if (!pmu->wakeup_source_valid)
            pmu->wakeup_source_name = "RTC";
        pmu->wakeup_source_valid = true;
    }

    for (index = 0; index < sizeof(g_pmu_pad_wakeup_cfgs) /
                   sizeof(g_pmu_pad_wakeup_cfgs[0]); index++) {
        if ((status & g_pmu_pad_wakeup_cfgs[index].irq_mask) == 0U)
            continue;
        pmu->wakeup_source_count++;
        if (pmu->wakeup_source_valid) {
            rt_kprintf("[pmu] multiple wakeup sources, including PAD%u\n",
                   g_pmu_pad_wakeup_cfgs[index].pad);
            continue;
        }
        pmu->wakeup_source_name = g_pmu_pad_wakeup_cfgs[index].source;
        pmu->wakeup_source_valid = true;
    }
}

static uint32_t pmu_shutdown_wakeup_type_bits(uint32_t trigger)
{
    switch (trigger) {
    case PMU_WAKEUP_TRIGGER_LOW_LEVEL:
        return PMU_INT_TRIGGER_LEVEL_LOW_MASK;
    case PMU_WAKEUP_TRIGGER_RISING_EDGE:
        return PMU_INT_TRIGGER_TYPE_MASK;
    case PMU_WAKEUP_TRIGGER_FALLING_EDGE:
        return PMU_INT_TRIGGER_TYPE_MASK | PMU_INT_TRIGGER_EDGE_MASK;
    case PMU_WAKEUP_TRIGGER_HIGH_LEVEL:
    default:
        return 0U;
    }
}

static uint32_t pmu_shutdown_wakeup_bias_bits(uint32_t bias, bool *update)
{
    *update = true;

    switch (bias) {
    case PMU_WAKEUP_BIAS_PULL_UP:
        return PMU_IO_CFG_PU;
    case PMU_WAKEUP_BIAS_PULL_DOWN:
        return PMU_IO_CFG_PD;
    case PMU_WAKEUP_BIAS_DISABLE:
        return 0U;
    case PMU_WAKEUP_BIAS_KEEP:
    default:
        *update = false;
        return 0U;
    }
}

static void pmu_configure_shutdown_wakeup_bias(
    struct pmu_dev *pmu,
    const struct pmu_pad_wakeup_cfg *cfg)
{
    uint32_t value;
    uint32_t bias;
    bool update;

    bias = pmu_shutdown_wakeup_bias_bits(cfg->bias, &update);
    if (!update)
        return;

    value = pmu_readl(pmu, cfg->io_cfg_reg);
    value &= ~PMU_IO_CFG_PULL_MASK;
    value |= bias;
    pmu_writel(pmu, value, cfg->io_cfg_reg);
}

static void pmu_configure_shutdown_wakeup_pad(
    struct pmu_dev *pmu,
    const struct pmu_pad_wakeup_cfg *cfg)
{
    uint32_t value;

    if (cfg == RT_NULL)
        return;

    pmu_configure_shutdown_wakeup_bias(pmu, cfg);

    value = pmu_readl(pmu, PMU_INT_DETECT_TYP);
    value &= ~(PMU_INT_TRIGGER_MASK << cfg->type_offset);
    value |= pmu_shutdown_wakeup_type_bits(cfg->trigger) << cfg->type_offset;
    pmu_writel(pmu, value, PMU_INT_DETECT_TYP);

    if (cfg->debounce_reg != 0U) {
        pmu_writel(pmu, cfg->debounce_ticks,
               cfg->debounce_reg);
    }
}

static void pmu_set_shutdown_wakeup_pad_mode(
    struct pmu_dev *pmu,
    const struct pmu_pad_wakeup_cfg *cfg,
    bool gpio)
{
    uint32_t value;

    value = pmu_readl(pmu, cfg->io_cfg_reg);
    value &= ~PMU_IO_CFG_IO_SEL_MASK;
    value |= gpio ? PMU_IO_CFG_GPIO_SEL : PMU_IO_CFG_PMU_SEL;

    if (gpio) {
        value |= PMU_IO_CFG_IE;
        value &= ~PMU_IO_CFG_OE;
    }

    pmu_writel(pmu, value, cfg->io_cfg_reg);
}

static void pmu_use_shutdown_wakeup_pads_as_gpio(struct pmu_dev *pmu)
{
    rt_size_t index;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    for (index = 0; index < sizeof(g_pmu_pad_wakeup_cfgs) /
                   sizeof(g_pmu_pad_wakeup_cfgs[0]); index++)
        pmu_set_shutdown_wakeup_pad_mode(pmu,
                         &g_pmu_pad_wakeup_cfgs[index], true);
    rt_hw_interrupt_enable(level);
}

static void pmu_prepare_wakeup_sources(struct pmu_dev *pmu)
{
    rt_size_t index;
    rt_base_t level;
    uint32_t output_mask;
    uint32_t detect_mask;
    uint32_t value;

    output_mask = PMU_IRQ_KEY_LONG;
    detect_mask = PMU_DET_KEY_LONG;
    for (index = 0; index < sizeof(g_pmu_pad_wakeup_cfgs) /
                   sizeof(g_pmu_pad_wakeup_cfgs[0]); index++) {
        output_mask |= g_pmu_pad_wakeup_cfgs[index].irq_mask;
        detect_mask |= g_pmu_pad_wakeup_cfgs[index].detect_mask;
    }

    if (pmu->cycle.shutting_down) {
        output_mask |= PMU_IRQ_RTC_ALARM;
        detect_mask |= PMU_DET_RTC_ALARM;
    }

    level = rt_hw_interrupt_disable();
    for (index = 0; index < sizeof(g_pmu_pad_wakeup_cfgs) /
                   sizeof(g_pmu_pad_wakeup_cfgs[0]); index++)
        pmu_set_shutdown_wakeup_pad_mode(pmu,
                         &g_pmu_pad_wakeup_cfgs[index], false);

    for (index = 0; index < sizeof(g_pmu_pad_wakeup_cfgs) /
                   sizeof(g_pmu_pad_wakeup_cfgs[0]); index++)
        pmu_configure_shutdown_wakeup_pad(pmu, &g_pmu_pad_wakeup_cfgs[index]);

    value = pmu_readl(pmu, PMU_INT0_TO_CTL_REGISTER);
    value &= ~PMU_CPU_IRQ_MASK;
    value |= output_mask;
    pmu_writel(pmu, value, PMU_INT0_TO_CTL_REGISTER);

    value = pmu_readl(pmu, PMU_INT1_TO_CTL_REGISTER);
    value &= ~PMU_CPU_IRQ_MASK;
    value |= output_mask;
    pmu_writel(pmu, value, PMU_INT1_TO_CTL_REGISTER);

    value = pmu_readl(pmu, PMU_INT_DETECT_EN);
    value &= ~PMU_DET_SOURCE_MASK;
    value |= detect_mask;
    pmu_writel(pmu, value, PMU_INT_DETECT_EN);

    rt_hw_interrupt_enable(level);
}

static void pmu_irq_handler(int vector, void *param)
{
    struct pmu_dev *pmu = pmu_get_dev();
    uint32_t raw_status;
    uint32_t cpu_route;
    uint32_t status;
    uint32_t rtc_status;
    uint32_t clear;

    (void)vector;
    (void)param;

    if (pmu_get_base(pmu) == RT_NULL)
        return;

    raw_status = pmu_readl(pmu, PMU_INT_STATE_REG);
    cpu_route = pmu_readl(pmu, PMU_INT0_TO_CPU_REGISTER) & PMU_CPU_IRQ_MASK;
    status = raw_status & cpu_route;
    rtc_status = status & PMU_IRQ_RTC_MASK;
    status &= ~PMU_IRQ_RTC_MASK;

    if (rtc_status == 0U)
        rtc_status = pmu_rtc_tick_fallback(pmu, status);

    if ((status == 0U) && (rtc_status == 0U))
        return;

    clear = pmu_irq_clear_mask(status);
    if (clear != 0U)
        pmu_writel(pmu, clear, PMU_INT_DETECT_CLR);

    if (status & (PMU_IRQ_KEY_LONG | PMU_IRQ_KEY_EDGE))
        pmu_pwrkey_irq(pmu, status & (PMU_IRQ_KEY_LONG |
                          PMU_IRQ_KEY_EDGE));

    if (rtc_status != 0U)
        pmu_rtc_handle_irq(pmu, rtc_status);
}

static void pmu_poweroff(struct pmu_dev *pmu)
{
    rt_base_t level;
    bool cancel_cycle;
    bool prepare_cycle_wakeup;
    const char *source;

    source = pmu->shutdown_source;
    pmu->shutdown_source = RT_NULL;
    if (source != RT_NULL)
        rt_kprintf("[pmu] poweroff: %s\n", source);
    else
        rt_kprintf("[pmu] poweroff\n");

    if (pmu_ensure_access(pmu) != RT_EOK) {
        rt_kprintf("[pmu] poweroff aborted: PMU access is unavailable\n");
        return;
    }

    level = rt_hw_interrupt_disable();
    cancel_cycle = pmu->cycle.active && !pmu->cycle.shutting_down;
    prepare_cycle_wakeup = pmu->cycle.active && pmu->cycle.shutting_down;
    rt_hw_interrupt_enable(level);

    if (cancel_cycle) {
        rt_kprintf("[pmu] cancel pending power cycle for manual shutdown\n");
        pmu_cancel_power_cycle(pmu);
    }
    if (prepare_cycle_wakeup) {
        int ret;

        ret = pmu_cycle_prepare_wakeup_alarm(pmu);
        if (ret != RT_EOK) {
            rt_kprintf("[pmu] poweroff: prepare cycle wake alarm failed (%d)\n",
                   ret);
        }
    }

    pmu_prepare_wakeup_sources(pmu);
    pmu_writel(pmu, PMU_STATUS_NORMAL_PD, PMU_STATUS);
    pmu_writel(pmu, PMU_SYSCTRL_TO_CTRL_1_REG_O |
           PMU_SYSCTRL_TO_CTRL_0_REG_O, PMU_SYSCTRL_REG);
    pmu_writel(pmu, PMU_CLR_ALL, PMU_INT_DETECT_CLR);
    pmu_writel(pmu, 0U, PMU_SYSCTRL_REG);
}

void rt_hw_board_shutdown(void)
{
    pmu_poweroff(pmu_get_dev());
}

static void pmu_shutdown_cmd_entry(void)
{
    pmu_do_shutdown("shell");
}
MSH_CMD_EXPORT_ALIAS(pmu_shutdown_cmd_entry, shutdown, shutdown machine);

int pmu_init(struct pmu_dev *pmu)
{
    int ret;

    if (pmu->initialized)
        return RT_EOK;

    ret = pmu_ensure_access(pmu);
    if (ret != RT_EOK) {
        rt_kprintf("[pmu] init failed: PMU access is unavailable\n");
        return ret;
    }

    pmu_capture_wakeup_source(pmu);

    ret = pmu_install_irq(pmu);
    if (ret != RT_EOK)
        return ret;

    ret = pmu_init_worker(pmu);
    if (ret != RT_EOK)
        return ret;

    ret = pmu_init_pwrkey(pmu);
    if (ret != RT_EOK)
        return ret;

    ret = pmu_init_rtc(pmu);
    if (ret != RT_EOK)
        return ret;

    /* Keep every configured wakeup PAD readable during normal runtime. */
    pmu_use_shutdown_wakeup_pads_as_gpio(pmu);

    pmu->initialized = true;
    return RT_EOK;
}

static int k230_pmu_init(void)
{
    return pmu_init(pmu_get_dev());
}
INIT_PREV_EXPORT(k230_pmu_init);
