/* Test-only kernel registration for the userspace GPIO IRQ ownership probe. */
#include <rtthread.h>
#include <finsh.h>
#include <string.h>
#include "drv_gpio.h"
#include "drv_fpioa.h"

#define OWNER_PIN 26
extern void kd_pin_set_ddr(rt_base_t pin, int value);
static volatile unsigned owner_hits;
static int owner_active, saved_ddr;
static rt_base_t saved_mode;
static rt_uint32_t saved_iomux;

static void owner_callback(void *arg)
{
    (void)arg;
    owner_hits++;
}

static void gpio_irq_owner_kernel(int argc, char **argv)
{
    if (argc != 2) {
        rt_kprintf("Usage: gpio_irq_owner_kernel start|check|stop\n");
        return;
    }
    if (!strcmp(argv[1], "start")) {
        if (owner_active || drv_fpioa_get_pin_cfg(OWNER_PIN, &saved_iomux)
            || kd_pin_mode_get(OWNER_PIN, &saved_mode)) {
            rt_kprintf("GPIO_OWNER_KERNEL FAIL save-state\n");
            return;
        }
        saved_ddr = kd_pin_get_ddr(OWNER_PIN);
        if (kd_pin_attach_irq(OWNER_PIN, GPIO_PE_RISING, owner_callback, RT_NULL)) {
            rt_kprintf("GPIO_OWNER_KERNEL FAIL attach\n");
            return;
        }
        owner_active = 1;
        owner_hits = 0;
        if (kd_pin_mode(OWNER_PIN, GPIO_DM_INPUT_PULLDOWN) || kd_pin_irq_enable(OWNER_PIN, 1)) {
            rt_kprintf("GPIO_OWNER_KERNEL FAIL enable\n");
            return;
        }
        rt_kprintf("GPIO_OWNER_KERNEL READY pin=%d hits=%u\n", OWNER_PIN, owner_hits);
    } else if (!strcmp(argv[1], "check")) {
        rt_kprintf("GPIO_OWNER_KERNEL %s hits=%u\n",
                   owner_active && owner_hits == 1 ? "PASS" : "FAIL", owner_hits);
    } else if (!strcmp(argv[1], "stop")) {
        if (!owner_active) {
            rt_kprintf("GPIO_OWNER_KERNEL FAIL inactive\n");
            return;
        }
        int rc = kd_pin_detach_irq(OWNER_PIN);
        if (rc == RT_EOK) {
            owner_active = 0;
            kd_pin_mode(OWNER_PIN, saved_mode);
            drv_fpioa_set_pin_cfg(OWNER_PIN, saved_iomux);
            kd_pin_set_ddr(OWNER_PIN, saved_ddr);
        }
        rt_kprintf("GPIO_OWNER_KERNEL %s stop rc=%d\n", rc == RT_EOK ? "PASS" : "FAIL", rc);
    }
}
MSH_CMD_EXPORT(gpio_irq_owner_kernel, GPIO IRQ kernel ownership regression);
