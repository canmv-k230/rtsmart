/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __ESP_HOSTED_PROTOCOL_H__
#define __ESP_HOSTED_PROTOCOL_H__

#include <rtthread.h>

#define EHF_TRANSPORT_HEADER_SIZE 12U
#define EHF_SPI_FRAME_SIZE        1600U
#define EHF_ETHERNET_FRAME_MAX    (EHF_SPI_FRAME_SIZE - EHF_TRANSPORT_HEADER_SIZE)
#define EHF_TRANSPORT_ALIGNMENT   4U

#define EHF_CAP_SDIO              (1U << 0)
#define EHF_CAP_SPI               (1U << 5)
#define EHF_CAP_CHECKSUM          (1U << 7)

static inline rt_uint16_t ehf_get_le16(const void *pointer)
{
    const rt_uint8_t *value = pointer;

    return (rt_uint16_t)value[0] | ((rt_uint16_t)value[1] << 8);
}

static inline rt_uint32_t ehf_get_le32(const void *pointer)
{
    const rt_uint8_t *value = pointer;

    return (rt_uint32_t)value[0] | ((rt_uint32_t)value[1] << 8) |
           ((rt_uint32_t)value[2] << 16) | ((rt_uint32_t)value[3] << 24);
}

static inline void ehf_put_le16(void *pointer, rt_uint16_t value)
{
    rt_uint8_t *destination = pointer;

    destination[0] = value & 0xff;
    destination[1] = value >> 8;
}

static inline void ehf_put_le32(void *pointer, rt_uint32_t value)
{
    rt_uint8_t *destination = pointer;

    destination[0] = value & 0xff;
    destination[1] = (value >> 8) & 0xff;
    destination[2] = (value >> 16) & 0xff;
    destination[3] = value >> 24;
}

static inline rt_uint16_t ehf_checksum(const void *data, rt_size_t length)
{
    const rt_uint8_t *bytes = data;
    rt_uint16_t result = 0;

    while (length--)
    {
        result += *bytes++;
    }
    return result;
}

#endif /* __ESP_HOSTED_PROTOCOL_H__ */
