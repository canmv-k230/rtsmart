/* Compile the actual candidate driver with public headers and minimal RTOS mocks.
 * Each scenario runs in a fresh process, matching a fresh native boot. */
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "sysctl_clk.h"
#include "sysctl_rst.h"
#include CANDIDATE_SOURCE

static sysctl_rst_t rmu;
volatile sysctl_rst_t *sysctl_rst = &rmu;
static struct dw_i2c_regs regs[5];
const struct rt_i2c_bus_device_ops k230_i2c_bus_ops = {0};
static bool pclk_on = true, core_on = true, reset_return = true;
static int reset_calls, hw_calls, registrations, slave_calls;
static int sequence, reset_sequence, i2c1_init_sequence;
static bool fail_i2c1_init, fail_i2c1_mapping;

void *rt_ioremap(void *base, rt_size_t size)
{
    uintptr_t value = (uintptr_t)base;
    unsigned int index = (unsigned int)((value - 0x91405000UL) / 0x1000UL);
    assert(size == 0x1000 && index < 5);
    if (index == 1 && fail_i2c1_mapping)
        return NULL;
    return &regs[index];
}
void rt_iounmap(void *address) { (void)address; }
void rt_hw_interrupt_mask(int irq) { (void)irq; }
void rt_event_detach(struct rt_event *event) { (void)event; }
void rt_mutex_detach(int *lock) { (void)lock; }
void k230_i2c_ctrl_enable(struct dw_i2c_master *master, rt_bool_t enable)
{ (void)master; (void)enable; }
rt_err_t k230_i2c_config_speed(struct dw_i2c_master *master, rt_uint32_t speed)
{ (void)master; (void)speed; return RT_EOK; }
rt_err_t k230_i2c_slave_register(struct dw_i2c_master *master)
{
    assert(master->is_slave && master->index == 1);
    slave_calls++;
    return RT_EOK;
}
rt_err_t k230_i2c_hw_init(struct dw_i2c_master *master)
{
    assert(!master->is_slave);
    hw_calls++;
    sequence++;
    if (master->index == 1) {
        i2c1_init_sequence = sequence;
        if (fail_i2c1_init)
            return -1;
    }
    return RT_EOK;
}
rt_err_t rt_i2c_bus_device_register(struct rt_i2c_bus_device *bus, const char *name)
{
    (void)bus;
    assert(strcmp(name, "i2c0") == 0 || strcmp(name, "i2c1") == 0);
    sequence++;
    registrations++;
    return RT_EOK;
}
bool sysctl_clk_get_leaf_en(sysctl_clk_node_e clock)
{
    if (clock == SYSCTL_CLK_I2C_1_PCLK_GATE)
        return pclk_on;
    assert(clock == SYSCTL_CLK_I2C_1_CLK);
    return core_on;
}
bool sysctl_reset(sysctl_reset_e target)
{
    assert(target == SYSCTL_RESET_I2C1);
    assert(sysctl_rst && pclk_on && core_on);
    assert(hw_calls == 1); /* i2c0 initialized; i2c1 IRQ is not installed yet. */
    assert(registrations == 1); /* i2c1 is not exposed to clients yet. */
    assert(reset_calls == 0);
    reset_calls++;
    reset_sequence = ++sequence;
    return reset_return;
}

int main(int argc, char **argv)
{
    int expected_reset = 1, repeat = 0;
    assert(argc == 2);
    if (strcmp(argv[1], "rmu_missing") == 0) {
        sysctl_rst = NULL;
        expected_reset = 0;
    } else if (strcmp(argv[1], "pclk_off") == 0) {
        pclk_on = false;
        expected_reset = 0;
    } else if (strcmp(argv[1], "core_off") == 0) {
        core_on = false;
        expected_reset = 0;
    } else if (strcmp(argv[1], "mapping_failed") == 0) {
        fail_i2c1_mapping = true;
        expected_reset = 0;
    } else if (strcmp(argv[1], "reset_api_false") == 0) {
        reset_return = false; /* bool is not an independent reset acknowledgement. */
    } else if (strcmp(argv[1], "init_failed_once") == 0) {
        fail_i2c1_init = true;
        repeat = 1;
    } else if (strcmp(argv[1], "repeated_init") == 0) {
        repeat = 1;
    } else {
        assert(strcmp(argv[1], "normal") == 0);
    }
#if !defined(CONFIG_BOARD_K230_CANMV_V3P0) || defined(RT_USING_SMP) || I2C1_MODE
    expected_reset = 0;
#endif
    assert(rt_hw_i2c_init() == 0);
    assert(reset_calls == expected_reset);
    if (expected_reset)
        assert(reset_sequence < i2c1_init_sequence);
    if (repeat) {
        assert(rt_hw_i2c_init() == 0);
        assert(reset_calls == expected_reset);
    }
    if (I2C1_MODE)
        assert(slave_calls == (fail_i2c1_mapping ? 0 : repeat + 1));
    else
        assert(slave_calls == 0);
    puts("PASS");
    return 0;
}
