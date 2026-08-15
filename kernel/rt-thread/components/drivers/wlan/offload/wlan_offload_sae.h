/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
 */
#ifndef __RT_WLAN_OFFLOAD_SAE_H__
#define __RT_WLAN_OFFLOAD_SAE_H__

#include <stddef.h>
#include <stdint.h>

#define RT_WLAN_OFFLOAD_SAE_COMMIT_LENGTH 98
#define RT_WLAN_OFFLOAD_SAE_CONFIRM_LENGTH 34
#define RT_WLAN_OFFLOAD_SAE_MAX_PASSWORD_LENGTH 64

typedef int (*rt_wlan_offload_sae_random_t)(void *context, uint8_t *data,
                                       size_t length);

struct rt_wlan_offload_sae
{
    uint32_t pwe[16];
    uint32_t random[8];
    uint32_t own_scalar[8];
    uint32_t own_element[16];
    uint32_t peer_scalar[8];
    uint32_t peer_element[16];
    uint8_t kck[32];
    uint8_t pmk[32];
    uint8_t pmkid[16];
    uint16_t send_confirm;
    uint16_t peer_confirm;
    uint8_t prepared;
    uint8_t committed;
};

int rt_wlan_offload_sae_prepare(struct rt_wlan_offload_sae *sae,
                           const uint8_t own_address[6],
                           const uint8_t peer_address[6],
                           const uint8_t *password, size_t password_length,
                           rt_wlan_offload_sae_random_t random, void *random_context);
int rt_wlan_offload_sae_write_commit(const struct rt_wlan_offload_sae *sae,
                                uint8_t output[RT_WLAN_OFFLOAD_SAE_COMMIT_LENGTH]);
int rt_wlan_offload_sae_process_commit(struct rt_wlan_offload_sae *sae,
                                  const uint8_t *data, size_t length);
int rt_wlan_offload_sae_write_confirm(struct rt_wlan_offload_sae *sae,
                                 uint8_t output[RT_WLAN_OFFLOAD_SAE_CONFIRM_LENGTH]);
int rt_wlan_offload_sae_check_confirm(struct rt_wlan_offload_sae *sae,
                                 const uint8_t *data, size_t length);
void rt_wlan_offload_sae_clear(struct rt_wlan_offload_sae *sae);

#endif /* __RT_WLAN_OFFLOAD_SAE_H__ */
