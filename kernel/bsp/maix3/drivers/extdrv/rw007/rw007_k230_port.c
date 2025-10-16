#include <rtthread.h>
#include <rtdevice.h>
#include <drv_gpio.h>
#include <drivers/spi.h>
#include <board.h>
#include <spi_wifi_rw007.h>

#define DBG_TAG "spi.rw007"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

extern void spi_wifi_isr(int vector);
static void rw007_gpio_init(void)
{
    // /* Configure IO */
    kd_pin_mode(RW007_RST_PIN, GPIO_DM_OUTPUT);
    kd_pin_mode(RW007_INT_BUSY_PIN, GPIO_DM_INPUT_PULLDOWN);

    /* Reset rw007 and config mode */
    kd_pin_write(RW007_RST_PIN, GPIO_PV_LOW);

    rt_thread_delay(rt_tick_from_millisecond(100));
    kd_pin_write(RW007_RST_PIN, GPIO_PV_HIGH);

    /* Wait rw007 ready(exit busy stat) */
    while(!kd_pin_read(RW007_INT_BUSY_PIN))
    {
        rt_thread_delay(5);
    }

    rt_thread_delay(rt_tick_from_millisecond(200));
    kd_pin_mode(RW007_INT_BUSY_PIN, PIN_MODE_INPUT_PULLUP);
}

int wifi_spi_device_init(void)
{
    char sn_version[32];
    rt_err_t ret = 0;
    struct rt_qspi_device *wspi = (struct rt_qspi_device*)rt_malloc(sizeof(struct rt_qspi_device));
    if (wspi == RT_NULL) {
        LOG_E("Failed to malloc memory for qspi device.\n");
        return -RT_ENOMEM;
    }

    ret = rt_spi_bus_attach_device((struct rt_spi_device*)wspi, "wspi", RW007_SPI_BUS_NAME, NULL);
    if (ret != RT_EOK) {
        LOG_E("attach spi0 failed!\n");
        return ret;
    }

    rw007_gpio_init();
    rt_hw_wifi_init("wspi");

    rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION);
    rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP);

    rw007_sn_get(sn_version);
    rt_kprintf("\nrw007  sn: [%s]\n", sn_version);
    rw007_version_get(sn_version);
    rt_kprintf("rw007 ver: [%s]\n\n", sn_version);

    return 0;
}
INIT_APP_EXPORT(wifi_spi_device_init);


static void int_wifi_irq(void * p)
{
    ((void)p);
    spi_wifi_isr(0);
}

void spi_wifi_hw_init(void)
{
    kd_pin_attach_irq(RW007_INT_BUSY_PIN, PIN_IRQ_MODE_FALLING, int_wifi_irq, 0);
    kd_pin_irq_enable(RW007_INT_BUSY_PIN, RT_TRUE);
}
