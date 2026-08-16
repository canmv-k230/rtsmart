/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_transport_spi.h"
#include "esp_hosted_mcu_log.h"

#include <drv_fpioa.h>
#include <drv_gpio.h>

#define DBG_TAG "esp_hosted.spi"
#define DBG_LVL ESP_HOSTED_MCU_DBG_LVL
#include <rtdbg.h>

#ifndef ESP_HOSTED_SPI_D2_PIN
#define ESP_HOSTED_SPI_D2_PIN (-1)
#endif
#ifndef ESP_HOSTED_SPI_D3_PIN
#define ESP_HOSTED_SPI_D3_PIN (-1)
#endif
#ifndef ESP_HOSTED_RESET_PULSE_MS
#define ESP_HOSTED_RESET_PULSE_MS 10
#endif
#if defined(ESP_HOSTED_SPI_BUS_SPI1)
#define EH_SPI_BUS_NAME "spi1"
#define EH_SPI_CLK_FUNC QSPI0_CLK
#define EH_SPI_D0_FUNC  QSPI0_D0
#define EH_SPI_D1_FUNC  QSPI0_D1
#define EH_SPI_D2_FUNC  QSPI0_D2
#define EH_SPI_D3_FUNC  QSPI0_D3
#elif defined(ESP_HOSTED_SPI_BUS_SPI2)
#define EH_SPI_BUS_NAME "spi2"
#define EH_SPI_CLK_FUNC QSPI1_CLK
#define EH_SPI_D0_FUNC  QSPI1_D0
#define EH_SPI_D1_FUNC  QSPI1_D1
#define EH_SPI_D2_FUNC  QSPI1_D2
#define EH_SPI_D3_FUNC  QSPI1_D3
#else
#define EH_SPI_BUS_NAME "spi0"
#define EH_SPI_CLK_FUNC OSPI_CLK
#define EH_SPI_D0_FUNC  OSPI_D0
#define EH_SPI_D1_FUNC  OSPI_D1
#define EH_SPI_D2_FUNC  OSPI_D2
#define EH_SPI_D3_FUNC  OSPI_D3
#endif

const char *eh_spi_bus_name(void)
{
    return EH_SPI_BUS_NAME;
}

rt_err_t eh_spi_validate_pins(const struct eh_spi_pin *pins, size_t count)
{
    size_t first;
    size_t second;

    for (first = 0; first < count; first++)
    {
        if (pins[first].pin < 0)
        {
            continue;
        }
        for (second = first + 1; second < count; second++)
        {
            if (pins[first].pin == pins[second].pin)
            {
                LOG_E("GPIO %d assigned to both %s and %s", pins[first].pin,
                      pins[first].name, pins[second].name);
                return -RT_EINVAL;
            }
        }
    }
    return RT_EOK;
}

static rt_err_t eh_spi_configure_pins(uint8_t max_data_lines,
                                      rt_bool_t full_duplex)
{
    const int pins[] = {
        ESP_HOSTED_SPI_D0_PIN,
        ESP_HOSTED_SPI_D1_PIN,
        ESP_HOSTED_SPI_D2_PIN,
        ESP_HOSTED_SPI_D3_PIN,
    };
    const fpioa_func_t functions[] = {
        EH_SPI_D0_FUNC,
        EH_SPI_D1_FUNC,
        EH_SPI_D2_FUNC,
        EH_SPI_D3_FUNC,
    };
    int index;

    if (ESP_HOSTED_SPI_CLK_PIN >= 0 &&
        drv_fpioa_set_pin_func(ESP_HOSTED_SPI_CLK_PIN, EH_SPI_CLK_FUNC) != 0)
    {
        LOG_E("GPIO %d cannot provide %s clock", ESP_HOSTED_SPI_CLK_PIN,
              EH_SPI_BUS_NAME);
        return -RT_EINVAL;
    }
    for (index = 0; index < max_data_lines; index++)
    {
        if (pins[index] >= 0 &&
            drv_fpioa_set_pin_func(pins[index], functions[index]) != 0)
        {
            LOG_E("GPIO %d cannot provide %s D%d", pins[index],
                  EH_SPI_BUS_NAME, index);
            return -RT_EINVAL;
        }
    }

    // if (full_duplex &&
    //     ((pins[0] >= 0 &&
    //       (drv_fpioa_set_pin_oe(pins[0], 1) != 0 ||
    //        drv_fpioa_set_pin_ie(pins[0], 0) != 0)) ||
    //      (pins[1] >= 0 &&
    //       (drv_fpioa_set_pin_oe(pins[1], 0) != 0 ||
    //        drv_fpioa_set_pin_ie(pins[1], 1) != 0))))
    // {
    //     LOG_E("cannot configure full-duplex SPI data direction");
    //     return -RT_ERROR;
    // }
    return RT_EOK;
}

rt_err_t eh_spi_init(struct eh_spi_bus *bus, uint8_t data_width,
                     uint8_t max_data_lines, rt_bool_t full_duplex)
{
    struct rt_qspi_configuration config;
    rt_err_t result;

    if (!bus || (max_data_lines != 1 && max_data_lines != 2 &&
                 max_data_lines != 4))
    {
        return -RT_EINVAL;
    }
    result = eh_spi_configure_pins(full_duplex ? 2 : max_data_lines,
                                   full_duplex);
    if (result != RT_EOK)
    {
        return result;
    }

    if (ESP_HOSTED_SPI_CS_PIN >= 0)
    {
        kd_pin_mode(ESP_HOSTED_SPI_CS_PIN, GPIO_DM_OUTPUT);
        kd_pin_write(ESP_HOSTED_SPI_CS_PIN, GPIO_PV_HIGH);
    }

    rt_memset(bus, 0, sizeof(*bus));
    result = rt_spi_bus_attach_device(&bus->device.parent,
                                      ESP_HOSTED_SPI_DEVICE_NAME,
                                      EH_SPI_BUS_NAME, RT_NULL);
    if (result != RT_EOK)
    {
        LOG_E("cannot attach %s to %s: %d", ESP_HOSTED_SPI_DEVICE_NAME,
              EH_SPI_BUS_NAME, result);
        return result;
    }
    bus->device_attached = RT_TRUE;

    rt_memset(&config, 0, sizeof(config));
    config.parent.mode = RT_SPI_MSB;
    switch (ESP_HOSTED_SPI_MODE)
    {
    case 0: config.parent.mode |= RT_SPI_MODE_0; break;
    case 1: config.parent.mode |= RT_SPI_MODE_1; break;
    case 2: config.parent.mode |= RT_SPI_MODE_2; break;
    default: config.parent.mode |= RT_SPI_MODE_3; break;
    }
    if (ESP_HOSTED_SPI_CS_PIN >= 0)
    {
        config.parent.soft_cs = 0x80 | ESP_HOSTED_SPI_CS_PIN;
    }
    config.parent.data_width = data_width;
    config.parent.max_hz = ESP_HOSTED_SPI_MAX_HZ;
    config.qspi_dl_width = max_data_lines;
    result = rt_qspi_configure(&bus->device, &config);
    if (result != RT_EOK)
    {
        LOG_E("cannot configure %s: %d", EH_SPI_BUS_NAME, result);
        return result;
    }
    bus->max_data_lines = max_data_lines;
    return RT_EOK;
}

void eh_spi_deinit(struct eh_spi_bus *bus)
{
    if (bus && bus->device_attached)
    {
        rt_device_unregister(&bus->device.parent.parent);
        bus->device_attached = RT_FALSE;
    }
}

rt_size_t eh_spi_transfer(struct eh_spi_bus *bus,
                          struct rt_qspi_message *message)
{
    if (!bus || !message)
    {
        return 0;
    }
    message->parent.cs_take = ESP_HOSTED_SPI_CS_PIN >= 0;
    message->parent.cs_release = ESP_HOSTED_SPI_CS_PIN >= 0;
    return rt_qspi_transfer_message(&bus->device, message);
}

rt_err_t eh_spi_configure_input_irq(int pin, rt_bool_t active_low,
                                    void (*handler)(void *), void *argument)
{
    rt_err_t result;

    if (pin < 0)
    {
        return RT_EOK;
    }
    kd_pin_mode(pin, active_low ? GPIO_DM_INPUT_PULLUP
                                : GPIO_DM_INPUT_PULLDOWN);
    result = kd_pin_attach_irq(pin,
                               active_low ? GPIO_PE_FALLING
                                          : GPIO_PE_RISING,
                               handler, argument);
    if (result == RT_EOK)
    {
        kd_pin_irq_enable(pin, RT_TRUE);
    }
    return result;
}

void eh_spi_deconfigure_input_irq(int pin)
{
    if (pin < 0)
    {
        return;
    }
    kd_pin_irq_enable(pin, RT_FALSE);
    kd_pin_detach_irq(pin);
}

rt_bool_t eh_spi_gpio_active(int pin, rt_bool_t active_low)
{
    if (pin < 0)
    {
        return RT_TRUE;
    }
    return kd_pin_read(pin) == (active_low ? GPIO_PV_LOW : GPIO_PV_HIGH);
}

int eh_spi_gpio_value(int pin)
{
    return pin < 0 ? -1 : kd_pin_read(pin);
}

void eh_spi_reset_coprocessor(struct eh_spi_bus *bus)
{
#ifdef ESP_HOSTED_RESET_ACTIVE_LOW
    int reset_value = GPIO_PV_LOW;
#else
    int reset_value = GPIO_PV_HIGH;
#endif
    int run_value = reset_value == GPIO_PV_LOW ? GPIO_PV_HIGH : GPIO_PV_LOW;

    if (!bus)
    {
        return;
    }
    bus->reset_performed = RT_FALSE;
    if (ESP_HOSTED_RESET_PIN < 0)
    {
        return;
    }
    kd_pin_mode(ESP_HOSTED_RESET_PIN, GPIO_DM_OUTPUT);
    kd_pin_write(ESP_HOSTED_RESET_PIN, run_value);
    rt_thread_mdelay(10);
    kd_pin_write(ESP_HOSTED_RESET_PIN, reset_value);
    rt_thread_mdelay(ESP_HOSTED_RESET_PULSE_MS);
    kd_pin_write(ESP_HOSTED_RESET_PIN, run_value);
    bus->reset_release_tick = rt_tick_get();
    bus->reset_performed = RT_TRUE;
    LOG_I("ESP reset released: GPIO=%d pulse=%d ms",
          ESP_HOSTED_RESET_PIN, ESP_HOSTED_RESET_PULSE_MS);
}

rt_int32_t eh_spi_reset_elapsed_ms(const struct eh_spi_bus *bus)
{
    if (!bus || !bus->reset_performed)
    {
        return -1;
    }
    return (rt_int32_t)(((rt_uint64_t)(rt_tick_get() -
                                      bus->reset_release_tick) * 1000U) /
                        RT_TICK_PER_SECOND);
}
