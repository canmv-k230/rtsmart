/*
 * Simple SHA-256 implementation for OTA verification.
 * The API is intentionally minimal and self-contained.
 */

#ifndef OTA_SHA256_H__
#define OTA_SHA256_H__

#include <rtthread.h>

#define OTA_SHA256_DIGEST_LENGTH 32u

typedef struct
{
    rt_uint8_t  data[64];
    rt_uint32_t datalen;
    rt_uint64_t bitlen;
    rt_uint32_t state[8];
} ota_sha256_ctx;

void ota_sha256_init(ota_sha256_ctx *ctx);
void ota_sha256_update(ota_sha256_ctx *ctx,
                       const rt_uint8_t *data,
                       rt_size_t len);
void ota_sha256_final(ota_sha256_ctx *ctx,
                      rt_uint8_t out[OTA_SHA256_DIGEST_LENGTH]);

#endif /* OTA_SHA256_H__ */

