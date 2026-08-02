/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RT_ESP_HOSTED_RPC_COMPAT_H
#define RT_ESP_HOSTED_RPC_COMPAT_H

#include <rtthread.h>

/* Platform boundary used by the upstream-shaped RPC core. RPC framing and
 * protobuf handling stay under host/drivers/rpc; only allocation and serial
 * transport submission are supplied by the RT-Smart port. */
void *esp_hosted_rpc_compat_alloc(size_t size);
void esp_hosted_rpc_compat_free(void *pointer);
size_t esp_hosted_rpc_compat_max_payload(void);
rt_err_t esp_hosted_rpc_compat_send(uint8_t flags, const void *data,
                                    size_t length);

#endif /* RT_ESP_HOSTED_RPC_COMPAT_H */
