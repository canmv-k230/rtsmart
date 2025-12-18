/*
 * Minimal SHA-256 implementation for OTA use.
 * Based on the standard SHA-256 algorithm (FIPS 180-4).
 */

#include "sha256_ota.h"

#include <string.h>

#define ROTR32(x, n)   (((x) >> (n)) | ((x) << (32u - (n))))
#define SHR(x, n)      ((x) >> (n))

#define CH(x, y, z)    (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)   (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)         (ROTR32((x), 2) ^ ROTR32((x), 13) ^ ROTR32((x), 22))
#define EP1(x)         (ROTR32((x), 6) ^ ROTR32((x), 11) ^ ROTR32((x), 25))
#define SIG0(x)        (ROTR32((x), 7) ^ ROTR32((x), 18) ^ SHR((x), 3))
#define SIG1(x)        (ROTR32((x), 17) ^ ROTR32((x), 19) ^ SHR((x), 10))

static const rt_uint32_t ota_sha256_k[64] =
{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void ota_sha256_transform(ota_sha256_ctx *ctx,
                                 const rt_uint8_t data[64])
{
    rt_uint32_t m[64];
    rt_uint32_t a, b, c, d, e, f, g, h;
    rt_uint32_t t1, t2;
    rt_uint32_t i, j;

    for (i = 0, j = 0; i < 16; ++i, j += 4)
    {
        m[i] = ((rt_uint32_t)data[j] << 24) |
               ((rt_uint32_t)data[j + 1] << 16) |
               ((rt_uint32_t)data[j + 2] << 8) |
               ((rt_uint32_t)data[j + 3]);
    }
    for (; i < 64; ++i)
    {
        m[i] = SIG1(m[i - 2]) + m[i - 7] +
               SIG0(m[i - 15]) + m[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i)
    {
        t1 = h + EP1(e) + CH(e, f, g) + ota_sha256_k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void ota_sha256_init(ota_sha256_ctx *ctx)
{
    rt_memset(ctx, 0, sizeof(*ctx));

    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

void ota_sha256_update(ota_sha256_ctx *ctx,
                       const rt_uint8_t *data,
                       rt_size_t len)
{
    rt_size_t i;

    for (i = 0; i < len; ++i)
    {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;

        if (ctx->datalen == 64u)
        {
            ota_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512u;
            ctx->datalen = 0;
        }
    }
}

void ota_sha256_final(ota_sha256_ctx *ctx,
                      rt_uint8_t out[OTA_SHA256_DIGEST_LENGTH])
{
    rt_uint32_t i;

    i = ctx->datalen;

    /* pad remaining data */
    if (ctx->datalen < 56u)
    {
        ctx->data[i++] = 0x80u;
        while (i < 56u)
            ctx->data[i++] = 0x00u;
    }
    else
    {
        ctx->data[i++] = 0x80u;
        while (i < 64u)
            ctx->data[i++] = 0x00u;
        ota_sha256_transform(ctx, ctx->data);
        rt_memset(ctx->data, 0, 56u);
    }

    /* append total length in bits */
    ctx->bitlen += (rt_uint64_t)ctx->datalen * 8u;

    ctx->data[63] = (rt_uint8_t)(ctx->bitlen);
    ctx->data[62] = (rt_uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (rt_uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (rt_uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (rt_uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (rt_uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (rt_uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (rt_uint8_t)(ctx->bitlen >> 56);

    ota_sha256_transform(ctx, ctx->data);

    /* output digest (big-endian) */
    for (i = 0; i < 4; ++i)
    {
        out[i]      = (rt_uint8_t)((ctx->state[0] >> (24u - i * 8u)) & 0xffu);
        out[i + 4]  = (rt_uint8_t)((ctx->state[1] >> (24u - i * 8u)) & 0xffu);
        out[i + 8]  = (rt_uint8_t)((ctx->state[2] >> (24u - i * 8u)) & 0xffu);
        out[i + 12] = (rt_uint8_t)((ctx->state[3] >> (24u - i * 8u)) & 0xffu);
        out[i + 16] = (rt_uint8_t)((ctx->state[4] >> (24u - i * 8u)) & 0xffu);
        out[i + 20] = (rt_uint8_t)((ctx->state[5] >> (24u - i * 8u)) & 0xffu);
        out[i + 24] = (rt_uint8_t)((ctx->state[6] >> (24u - i * 8u)) & 0xffu);
        out[i + 28] = (rt_uint8_t)((ctx->state[7] >> (24u - i * 8u)) & 0xffu);
    }
}

