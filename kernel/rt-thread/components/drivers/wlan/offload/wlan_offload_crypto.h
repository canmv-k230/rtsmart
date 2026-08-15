/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_WLAN_OFFLOAD_CRYPTO_H__
#define __RT_WLAN_OFFLOAD_CRYPTO_H__

#include <stddef.h>
#include <stdint.h>

void rt_wlan_offload_hmac_sha1(const uint8_t *key, size_t key_length,
                          const uint8_t *data, size_t data_length,
                          uint8_t digest[20]);
void rt_wlan_offload_hmac_md5(const uint8_t *key, size_t key_length,
                         const uint8_t *data, size_t data_length,
                         uint8_t digest[16]);
void rt_wlan_offload_hmac_sha256(const uint8_t *key, size_t key_length,
                            const uint8_t *data, size_t data_length,
                            uint8_t digest[32]);
int rt_wlan_offload_sha256_prf(const uint8_t *key, size_t key_length,
                          const char *label, const uint8_t *data,
                          size_t data_length, uint8_t *output,
                          size_t output_length);
int rt_wlan_offload_pbkdf2_sha1(const uint8_t *passphrase, size_t passphrase_length,
                           const uint8_t *ssid, size_t ssid_length,
                           uint8_t pmk[32]);
void rt_wlan_offload_wpa_prf(const uint8_t *key, size_t key_length,
                        const char *label, const uint8_t *data,
                        size_t data_length, uint8_t *output,
                        size_t output_length);
int rt_wlan_offload_aes_unwrap(const uint8_t kek[16], const uint8_t *input,
                          size_t input_length, uint8_t *output,
                          size_t *output_length);
int rt_wlan_offload_aes_wrap(const uint8_t kek[16], const uint8_t *input,
                        size_t input_length, uint8_t *output,
                        size_t *output_length);
int rt_wlan_offload_rc4_skip(const uint8_t *key, size_t key_length,
                        size_t skip, uint8_t *data, size_t data_length);
int rt_wlan_offload_aes_cmac(const uint8_t key[16], const uint8_t *data,
                        size_t data_length, uint8_t mic[16]);
int rt_wlan_offload_crypto_equal(const uint8_t *left, const uint8_t *right,
                            size_t length);
void rt_wlan_offload_crypto_zero(void *data, size_t length);

#endif /* __RT_WLAN_OFFLOAD_CRYPTO_H__ */
