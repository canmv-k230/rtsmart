/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __ESP_HOSTED_MCU_LOG_H__
#define __ESP_HOSTED_MCU_LOG_H__

#if defined(ESP_HOSTED_MCU_LOG_LEVEL_DEBUG)
#define ESP_HOSTED_MCU_DBG_LVL DBG_LOG
#elif defined(ESP_HOSTED_MCU_LOG_LEVEL_INFO)
#define ESP_HOSTED_MCU_DBG_LVL DBG_INFO
#elif defined(ESP_HOSTED_MCU_LOG_LEVEL_WARNING)
#define ESP_HOSTED_MCU_DBG_LVL DBG_WARNING
#elif defined(ESP_HOSTED_MCU_LOG_LEVEL_ERROR)
#define ESP_HOSTED_MCU_DBG_LVL DBG_ERROR
#else
#define ESP_HOSTED_MCU_DBG_LVL DBG_INFO
#endif

#endif /* __ESP_HOSTED_MCU_LOG_H__ */
