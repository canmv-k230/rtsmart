/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_rpc_compat.h"

#include "esp_hosted_transport.h"

void *esp_hosted_rpc_compat_alloc(size_t size)
{
    return rt_malloc(size);
}

void esp_hosted_rpc_compat_free(void *pointer)
{
    rt_free(pointer);
}

size_t esp_hosted_rpc_compat_max_payload(void)
{
    return esp_hosted_transport_max_payload();
}

rt_err_t esp_hosted_rpc_compat_send(uint8_t flags, const void *data,
                                    size_t length)
{
    return esp_hosted_transport_send(ESP_HOSTED_TRANSPORT_IF_SERIAL, flags,
                                     data, length, RT_TRUE,
                                     RT_NULL, RT_NULL);
}
