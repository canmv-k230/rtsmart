/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_HOSTED_TRANSPORT_SPI_H
#define ESP_HOSTED_TRANSPORT_SPI_H

#include "esp_hosted_transport_internal.h"

#include <drivers/spi.h>

struct eh_spi_bus
{
    struct rt_qspi_device device;
    uint8_t max_data_lines;
    rt_tick_t reset_release_tick;
    rt_bool_t device_attached;
    rt_bool_t reset_performed;
};

struct eh_spi_pin
{
    const char *name;
    int pin;
};

const char *eh_spi_bus_name(void);
rt_err_t eh_spi_validate_pins(const struct eh_spi_pin *pins, size_t count);
rt_err_t eh_spi_init(struct eh_spi_bus *bus, uint8_t data_width,
                     uint8_t max_data_lines, rt_bool_t full_duplex);
void eh_spi_deinit(struct eh_spi_bus *bus);
rt_size_t eh_spi_transfer(struct eh_spi_bus *bus,
                          struct rt_qspi_message *message);
rt_err_t eh_spi_configure_input_irq(int pin, rt_bool_t active_low,
                                    void (*handler)(void *), void *argument);
void eh_spi_deconfigure_input_irq(int pin);
rt_bool_t eh_spi_gpio_active(int pin, rt_bool_t active_low);
int eh_spi_gpio_value(int pin);
void eh_spi_reset_coprocessor(struct eh_spi_bus *bus);
rt_int32_t eh_spi_reset_elapsed_ms(const struct eh_spi_bus *bus);

#endif /* ESP_HOSTED_TRANSPORT_SPI_H */
