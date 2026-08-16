/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __ESP_HOSTED_WIFI_COUNTRY_H__
#define __ESP_HOSTED_WIFI_COUNTRY_H__

#include <rtthread.h>
#include <wlan_dev.h>

const char *ehf_country_code_from_rt(rt_country_code_t country);
rt_country_code_t ehf_country_code_to_rt(const rt_uint8_t *code,
                                         rt_size_t length);

#endif /* __ESP_HOSTED_WIFI_COUNTRY_H__ */
