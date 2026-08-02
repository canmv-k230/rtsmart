/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_HOSTED_COUNTRY_H
#define ESP_HOSTED_COUNTRY_H

#include <stddef.h>
#include <stdint.h>

#include <rtthread.h>

#include <wlan_dev.h>

const char *eh_country_code_from_rt(rt_country_code_t country);
rt_country_code_t eh_country_code_to_rt(const uint8_t *code, size_t length);

#endif
