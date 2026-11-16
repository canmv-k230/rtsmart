#include <rtthread.h>
#include <rthw.h>
#include <finsh.h>
#include "tick.h"
#include "drv_gpio.h"
#include "drv_fpioa.h"

extern void kd_pin_set_ddr(rt_base_t pin, int value);
extern void kd_pin_set_dr(rt_base_t pin, int value);

static volatile unsigned hits;
static volatile int callback_action, callback_rc, callback_attach_rc;
static int test_pin = 26;
static unsigned checks, failures;

/* Pin-state lock contention between a thread and IRQ callbacks. */
#define CONTEND_PIN 27
static volatile unsigned contend_hits, contend_bad_reads;
static struct rt_timer contend_timer;

static void expect(int condition, const char *name, int value)
{
    checks++;
    if (!condition) {
        failures++;
        rt_kprintf("GPIO_REVIEW FAIL %s value=%d\n", name, value);
    }
}

static void review_callback(void *arg)
{
    int pin = (int)(long)arg;
    hits++;
    if (callback_action == 1) {
        callback_rc = kd_pin_detach_irq(pin);
        callback_attach_rc = kd_pin_attach_irq(pin, GPIO_PE_RISING, review_callback, arg);
    } else if (callback_action == 2) {
        callback_rc = kd_pin_irq_enable(pin, 0);
    } else if (callback_action == 3 && hits == 1) {
        /* Raise a second rising edge while this callback is still running. */
        kd_pin_mode(pin, GPIO_DM_INPUT_PULLDOWN);
        cpu_ticks_delay_us(100);
        kd_pin_mode(pin, GPIO_DM_INPUT_PULLUP);
        cpu_ticks_delay_us(100);
    }
}

static void contend_callback(void *arg)
{
    int pin = (int)(long)arg;
    int value;
    contend_hits++;
    kd_pin_write(CONTEND_PIN, contend_hits & 1);
    value = kd_pin_read(CONTEND_PIN);
    if (value != GPIO_PV_LOW && value != GPIO_PV_HIGH)
        contend_bad_reads++;
    kd_pin_irq_enable(pin, 0);
}

static void contend_tick(void *arg)
{
    /* Hard timer: runs in the tick ISR, at an arbitrary point in the thread. */
    kd_pin_irq_enable((int)(long)arg, 1);
}

static void set_input(int high)
{
    int rc = kd_pin_mode(test_pin, high ? GPIO_DM_INPUT_PULLUP : GPIO_DM_INPUT_PULLDOWN);
    expect(rc == RT_EOK, "input-mode", rc);
    cpu_ticks_delay_us(100);
}

static void edge_case(int mode)
{
    unsigned start;
    int rc;
    callback_action = 0;
    hits = 0;
    set_input(mode == GPIO_PE_FALLING);
    rc = kd_pin_attach_irq(test_pin, mode, review_callback, (void *)(long)test_pin);
    expect(rc == RT_EOK, "edge-attach", rc);
    rc = kd_pin_irq_enable(test_pin, 1);
    expect(rc == RT_EOK, "edge-enable", rc);
    for (int i = 0; i < 16; i++) {
        set_input(0);
        set_input(1);
    }
    if (mode == GPIO_PE_FALLING)
        set_input(0);
    start = hits;
    expect(start == (mode == GPIO_PE_BOTH ? 31 : mode == GPIO_PE_FALLING ? 17 : 16), "edge-count", start);
    rc = kd_pin_detach_irq(test_pin);
    expect(rc == RT_EOK, "edge-detach", rc);
    set_input(0); set_input(1);
    expect(hits == start, "no-callback-after-detach", hits);
    rt_kprintf("GPIO_REVIEW edge mode=%d hits=%u\n", mode, start);
}

static void gpio_irq_review(int argc, char **argv)
{
    rt_uint32_t saved_iomux;
    rt_base_t saved_mode;
    unsigned start;
    int saved_ddr = kd_pin_get_ddr(test_pin);
    int rc;
    (void)argc; (void)argv;
    checks = failures = hits = 0;
    if (drv_fpioa_get_pin_cfg(test_pin, &saved_iomux) || kd_pin_mode_get(test_pin, &saved_mode)) {
        rt_kprintf("GPIO_REVIEW FAIL save-pin\n");
        return;
    }
    rt_kprintf("GPIO_REVIEW BEGIN build=%s %s pin=%d smp=0\n", __DATE__, __TIME__, test_pin);
    rc = kd_pin_attach_irq(-1, GPIO_PE_RISING, review_callback, 0);
    expect(rc == -RT_EINVAL, "invalid-negative-pin", rc);
    rc = kd_pin_attach_irq(64, GPIO_PE_RISING, review_callback, 0);
    expect(rc == -RT_EINVAL, "invalid-high-pin", rc);
    rc = kd_pin_attach_irq(test_pin, GPIO_PE_RISING, 0, 0);
    expect(rc == -RT_EINVAL, "null-handler", rc);
    rc = kd_pin_attach_irq(test_pin, 99, review_callback, 0);
    expect(rc == -RT_EINVAL, "invalid-mode", rc);

    edge_case(GPIO_PE_RISING);
    edge_case(GPIO_PE_FALLING);
    edge_case(GPIO_PE_BOTH);

    callback_action = 0; hits = 0;
    for (int i = 0; i < 1000; i++) {
        set_input(0);
        rc = kd_pin_attach_irq(test_pin, GPIO_PE_RISING, review_callback, (void *)(long)test_pin);
        expect(rc == RT_EOK, "stress-attach", rc);
        rc = kd_pin_irq_enable(test_pin, 1);
        expect(rc == RT_EOK, "stress-enable", rc);
        start = hits;
        set_input(1);
        expect(hits == start + 1, "stress-interrupt", hits - start);
        rc = kd_pin_detach_irq(test_pin);
        expect(rc == RT_EOK, "stress-detach", rc);
        set_input(0); set_input(1);
        expect(hits == start + 1, "stress-no-late-callback", hits - start);
    }
    rt_kprintf("GPIO_REVIEW stress cycles=1000 hits=%u\n", hits);
    callback_action = 0; hits = 0;
    set_input(0);
    expect(kd_pin_attach_irq(test_pin, GPIO_PE_RISING, review_callback, (void *)(long)test_pin) == RT_EOK, "duplicate-initial-attach", 0);
    expect(kd_pin_irq_enable(test_pin, 1) == RT_EOK, "duplicate-enable", 0);
    rc = kd_pin_attach_irq(test_pin, GPIO_PE_RISING, review_callback, (void *)(long)test_pin);
    expect(rc == -RT_EBUSY, "duplicate-rejected", rc);
    set_input(1);
    expect(hits == 1, "duplicate-keeps-original-enabled", hits);
    kd_pin_detach_irq(test_pin);

    callback_action = 2;
    for (int mode = GPIO_PE_HIGH; mode <= GPIO_PE_LOW; mode++) {
        rt_uint32_t level_iomux;
        rt_base_t level_mode;
        test_pin = mode == GPIO_PE_HIGH ? 27 : 28;
        drv_fpioa_get_pin_cfg(test_pin, &level_iomux);
        kd_pin_mode_get(test_pin, &level_mode);
        int level_ddr = kd_pin_get_ddr(test_pin);
        int active = mode == GPIO_PE_HIGH;
        hits = 0;
        set_input(!active);
        rc = kd_pin_attach_irq(test_pin, mode, review_callback, (void *)(long)test_pin);
        expect(rc == RT_EOK, "level-attach", rc);
        kd_pin_irq_enable(test_pin, 1);
        set_input(active);
        expect(hits == 1 && callback_rc == RT_EOK, "level-disable-in-callback", hits);
        set_input(!active);
        expect(kd_pin_irq_enable(test_pin, 1) == RT_EOK, "level-reenable", 0);
        set_input(active);
        expect(hits == 2, "level-reenable-delivers-again", hits);
        kd_pin_detach_irq(test_pin);
        kd_pin_mode(test_pin, level_mode);
        drv_fpioa_set_pin_cfg(test_pin, level_iomux);
        kd_pin_set_ddr(test_pin, level_ddr);
    }
    test_pin = 26;
    callback_action = 1; hits = 0;
    set_input(0);
    expect(kd_pin_attach_irq(test_pin, GPIO_PE_RISING, review_callback, (void *)(long)test_pin) == RT_EOK, "self-attach", 0);
    kd_pin_irq_enable(test_pin, 1);
    set_input(1);
    expect(hits == 1 && callback_rc == RT_EOK, "self-detach", callback_rc);
    expect(callback_attach_rc == -RT_EBUSY, "self-reattach-busy-in-callback", callback_attach_rc);
    set_input(0); set_input(1);
    expect(hits == 1, "self-remains-detached", hits);
    callback_action = 0;
    set_input(0);
    kd_pin_attach_irq(test_pin, GPIO_PE_RISING, review_callback, (void *)(long)test_pin);
    kd_pin_irq_enable(test_pin, 1);
    start = hits;
    set_input(1);
    expect(hits == start + 1, "reattach-after-self-detach", hits - start);

    kd_pin_detach_irq(test_pin);
    for (int i = 0; i < 100; i++) {
        callback_action = 1;
        set_input(0);
        expect(kd_pin_attach_irq(test_pin, GPIO_PE_RISING, review_callback, (void *)(long)test_pin) == RT_EOK,
               "self-stress-attach", i);
        kd_pin_irq_enable(test_pin, 1);
        start = hits;
        set_input(1);
        expect(hits == start + 1 && callback_rc == RT_EOK, "self-stress-detach", i);
        expect(callback_attach_rc == -RT_EBUSY, "self-stress-attach-busy-in-callback", callback_attach_rc);
        callback_action = 0;
        set_input(0);
        expect(kd_pin_attach_irq(test_pin, GPIO_PE_RISING, review_callback, (void *)(long)test_pin) == RT_EOK,
               "self-stress-reuse", i);
        kd_pin_irq_enable(test_pin, 1);
        start = hits;
        set_input(1);
        expect(hits == start + 1, "self-stress-new-irq", hits - start);
        kd_pin_detach_irq(test_pin);
    }
    rt_kprintf("GPIO_REVIEW self-detach-reuse cycles=100\n");

    /* An edge raised during the callback must interrupt again after it. */
    callback_action = 3; hits = 0;
    set_input(0);
    expect(kd_pin_attach_irq(test_pin, GPIO_PE_RISING, review_callback, (void *)(long)test_pin) == RT_EOK,
           "edge-in-callback-attach", 0);
    kd_pin_irq_enable(test_pin, 1);
    set_input(1);
    expect(hits == 2, "edge-in-callback-not-lost", hits);
    kd_pin_detach_irq(test_pin);
    callback_action = 0;
    rt_kprintf("GPIO_REVIEW edge-in-callback hits=%u\n", hits);

    /* A thread hammers kd_pin_write/read on CONTEND_PIN while IRQ callbacks
     * fired from the tick use the same pin. Open-drain mode makes every write
     * and read also update the pinmux, covering the fpioa lock as well. A
     * lock that leaves interrupts enabled hangs the board here. */
    {
        rt_uint32_t contend_iomux;
        rt_base_t contend_mode;
        int contend_ddr = kd_pin_get_ddr(CONTEND_PIN);
        unsigned writes = 0;
        rt_uint64_t end;
        drv_fpioa_get_pin_cfg(CONTEND_PIN, &contend_iomux);
        kd_pin_mode_get(CONTEND_PIN, &contend_mode);
        expect(kd_pin_mode(CONTEND_PIN, GPIO_DM_OUTPUT_OD) == RT_EOK, "contend-output", 0);
        contend_hits = contend_bad_reads = 0;
        set_input(1);
        expect(kd_pin_attach_irq(test_pin, GPIO_PE_HIGH, contend_callback, (void *)(long)test_pin) == RT_EOK,
               "contend-attach", 0);
        rt_timer_init(&contend_timer, "gpio_ct", contend_tick, (void *)(long)test_pin, 1,
                      RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_HARD_TIMER);
        rt_timer_start(&contend_timer);
        end = cpu_ticks_ms() + 3000;
        while (cpu_ticks_ms() < end) {
            kd_pin_write(CONTEND_PIN, writes & 1);
            if (kd_pin_read(CONTEND_PIN) < 0)
                contend_bad_reads++;
            writes++;
        }
        rt_timer_stop(&contend_timer);
        rt_timer_detach(&contend_timer);
        kd_pin_detach_irq(test_pin);
        expect(contend_hits >= RT_TICK_PER_SECOND, "contend-callbacks", contend_hits);
        expect(contend_bad_reads == 0, "contend-reads", contend_bad_reads);
        kd_pin_set_dr(CONTEND_PIN, 0);
        kd_pin_mode(CONTEND_PIN, contend_mode);
        drv_fpioa_set_pin_cfg(CONTEND_PIN, contend_iomux);
        kd_pin_set_ddr(CONTEND_PIN, contend_ddr);
        rt_kprintf("GPIO_REVIEW contend writes=%u callbacks=%u\n", writes, contend_hits);
    }
    kd_pin_mode(test_pin, saved_mode);
    drv_fpioa_set_pin_cfg(test_pin, saved_iomux);
    kd_pin_set_ddr(test_pin, saved_ddr);
    rt_kprintf("GPIO_REVIEW END checks=%u failures=%u\n", checks, failures);
}
MSH_CMD_EXPORT(gpio_irq_review, GPIO IRQ review hardware regression);
