/*
 * ESP-Hosted-FG wire definitions derived from common/include/adapter.h in
 * ESP-Hosted commit 5acd9ba0eaf186cc340b8dc2e7a12993a4162b93.
 *
 * Copyright 2015-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __ESP_HOSTED_FG_PROTOCOL_H__
#define __ESP_HOSTED_FG_PROTOCOL_H__

#include <rtthread.h>

#define EHF_FG_MORE_FRAGMENT  (1U << 0)
#define EHF_FG_SERIAL_PAYLOAD 1500U

enum ehf_fg_interface
{
    EHF_FG_STA_INTERFACE = 0,
    EHF_FG_AP_INTERFACE,
    EHF_FG_SERIAL_INTERFACE,
    EHF_FG_HCI_INTERFACE,
    EHF_FG_PRIVATE_INTERFACE,
    EHF_FG_TEST_INTERFACE,
    EHF_FG_INTERFACE_MAX,
};

enum ehf_fg_private_type
{
    EHF_FG_PRIVATE_EVENT = 0,
    EHF_FG_PRIVATE_COMMAND,
};

enum ehf_fg_private_tag
{
    EHF_FG_PRIVATE_CAPABILITY = 0,
    EHF_FG_PRIVATE_SPI_CLOCK,
    EHF_FG_PRIVATE_CHIP_ID,
    EHF_FG_PRIVATE_RAW_TEST,
    EHF_FG_PRIVATE_FIRMWARE_DATA,
    EHF_FG_PRIVATE_RX_BUFFER_CONFIG,
    EHF_FG_PRIVATE_CUSTOM_STRING,
};

struct ehf_fg_transport_header
{
    rt_uint8_t interface_number;
    rt_uint8_t flags;
    rt_uint8_t length[2];
    rt_uint8_t offset[2];
    rt_uint8_t checksum[2];
    rt_uint8_t sequence[2];
    rt_uint8_t reserved2;
    rt_uint8_t private_type;
} __attribute__((packed));

struct ehf_fg_private_event
{
    rt_uint8_t event_type;
    rt_uint8_t event_length;
    rt_uint8_t data[];
} __attribute__((packed));

#endif /* __ESP_HOSTED_FG_PROTOCOL_H__ */
