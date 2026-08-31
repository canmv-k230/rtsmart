/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
 *
 * The AES-128 key schedule and cipher are adapted from TinyCrypt,
 * Copyright (c) 2017 Intel Corporation. The SHA-1 implementation follows the
 * public-domain Steve Reid implementation. New glue is licensed Apache-2.0.
 */
#include "wlan_offload_crypto.h"

#include <string.h>

struct wlan_offload_sha1
{
    uint32_t state[5];
    uint64_t bytes;
    uint8_t buffer[64];
};

struct wlan_offload_hmac_sha1
{
    struct wlan_offload_sha1 inner;
    struct wlan_offload_sha1 outer;
};

struct wlan_offload_md5
{
    uint32_t state[4];
    uint64_t bytes;
    uint8_t buffer[64];
};

struct wlan_offload_sha256
{
    uint32_t state[8];
    uint64_t bytes;
    uint8_t buffer[64];
};

struct wlan_offload_aes_key
{
    uint32_t words[44];
};

static uint32_t crypto_rotl32(uint32_t value, unsigned int bits)
{
    return (value << bits) | (value >> (32U - bits));
}

static uint32_t crypto_get_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static void crypto_put_be32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static uint32_t crypto_get_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void crypto_put_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

void rt_wlan_offload_crypto_zero(void *data, size_t length)
{
    volatile uint8_t *cursor = data;

    while (length--)
    {
        *cursor++ = 0;
    }
}

int rt_wlan_offload_crypto_equal(const uint8_t *left, const uint8_t *right,
                            size_t length)
{
    uint8_t difference = 0;

    while (length--)
    {
        difference |= *left++ ^ *right++;
    }
    return difference == 0;
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t words[80];
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f;
    uint32_t k;
    uint32_t temporary;
    unsigned int index;

    for (index = 0; index < 16; index++)
    {
        words[index] = crypto_get_be32(block + index * 4U);
    }
    for (; index < 80; index++)
    {
        words[index] = crypto_rotl32(words[index - 3] ^ words[index - 8] ^
                                    words[index - 14] ^ words[index - 16], 1);
    }
    for (index = 0; index < 80; index++)
    {
        if (index < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999U;
        }
        else if (index < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ed9eba1U;
        }
        else if (index < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcU;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xca62c1d6U;
        }
        temporary = crypto_rotl32(a, 5) + f + e + k + words[index];
        e = d;
        d = c;
        c = crypto_rotl32(b, 30);
        b = a;
        a = temporary;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    rt_wlan_offload_crypto_zero(words, sizeof(words));
}

static void sha1_init(struct wlan_offload_sha1 *context)
{
    context->state[0] = 0x67452301U;
    context->state[1] = 0xefcdab89U;
    context->state[2] = 0x98badcfeU;
    context->state[3] = 0x10325476U;
    context->state[4] = 0xc3d2e1f0U;
    context->bytes = 0;
}

static void sha1_update(struct wlan_offload_sha1 *context, const uint8_t *data,
                        size_t length)
{
    size_t used = (size_t)(context->bytes & 63U);
    size_t copy;

    context->bytes += length;
    if (used)
    {
        copy = 64U - used;
        if (copy > length)
        {
            copy = length;
        }
        memcpy(context->buffer + used, data, copy);
        used += copy;
        data += copy;
        length -= copy;
        if (used == 64U)
        {
            sha1_transform(context->state, context->buffer);
        }
    }
    while (length >= 64U)
    {
        sha1_transform(context->state, data);
        data += 64U;
        length -= 64U;
    }
    if (length)
    {
        memcpy(context->buffer, data, length);
    }
}

static void sha1_finish(struct wlan_offload_sha1 *context, uint8_t digest[20])
{
    uint8_t tail[72];
    uint64_t bits = context->bytes * 8U;
    size_t used = (size_t)(context->bytes & 63U);
    size_t padding = used < 56U ? 56U - used : 120U - used;
    unsigned int index;

    memset(tail, 0, padding + 8U);
    tail[0] = 0x80;
    for (index = 0; index < 8; index++)
    {
        tail[padding + index] = (uint8_t)(bits >> (56U - index * 8U));
    }
    sha1_update(context, tail, padding + 8U);
    for (index = 0; index < 5; index++)
    {
        crypto_put_be32(digest + index * 4U, context->state[index]);
    }
    rt_wlan_offload_crypto_zero(tail, sizeof(tail));
    rt_wlan_offload_crypto_zero(context, sizeof(*context));
}

static void hmac_sha1_prepare(struct wlan_offload_hmac_sha1 *prepared,
                              const uint8_t *key, size_t key_length)
{
    struct wlan_offload_sha1 context;
    uint8_t key_block[64];
    size_t index;

    memset(key_block, 0, sizeof(key_block));
    if (key_length > sizeof(key_block))
    {
        sha1_init(&context);
        sha1_update(&context, key, key_length);
        sha1_finish(&context, key_block);
    }
    else
    {
        memcpy(key_block, key, key_length);
    }
    for (index = 0; index < sizeof(key_block); index++)
    {
        key_block[index] ^= 0x36;
    }
    sha1_init(&prepared->inner);
    sha1_update(&prepared->inner, key_block, sizeof(key_block));
    for (index = 0; index < sizeof(key_block); index++)
    {
        key_block[index] ^= 0x36 ^ 0x5c;
    }
    sha1_init(&prepared->outer);
    sha1_update(&prepared->outer, key_block, sizeof(key_block));
    rt_wlan_offload_crypto_zero(key_block, sizeof(key_block));
}

static void hmac_sha1_digest(
    const struct wlan_offload_hmac_sha1 *prepared,
    const uint8_t *data, size_t data_length, uint8_t digest[20])
{
    struct wlan_offload_sha1 inner_context = prepared->inner;
    struct wlan_offload_sha1 outer_context = prepared->outer;
    uint8_t inner[20];

    sha1_update(&inner_context, data, data_length);
    sha1_finish(&inner_context, inner);
    sha1_update(&outer_context, inner, sizeof(inner));
    sha1_finish(&outer_context, digest);
    rt_wlan_offload_crypto_zero(inner, sizeof(inner));
}

void rt_wlan_offload_hmac_sha1(const uint8_t *key, size_t key_length,
                          const uint8_t *data, size_t data_length,
                          uint8_t digest[20])
{
    struct wlan_offload_hmac_sha1 prepared;

    hmac_sha1_prepare(&prepared, key, key_length);
    hmac_sha1_digest(&prepared, data, data_length, digest);
    rt_wlan_offload_crypto_zero(&prepared, sizeof(prepared));
}

#define MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_STEP(fn, a, b, c, d, word, constant, shift) \
    do { \
        (a) += fn((b), (c), (d)) + (word) + (constant); \
        (a) = crypto_rotl32((a), (shift)); \
        (a) += (b); \
    } while (0)

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t x[16];
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    unsigned int index;

    for (index = 0; index < 16; index++)
    {
        x[index] = crypto_get_le32(block + index * 4U);
    }
    MD5_STEP(MD5_F, a, b, c, d, x[0],  0xd76aa478U, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[1],  0xe8c7b756U, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[2],  0x242070dbU, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[3],  0xc1bdceeeU, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[4],  0xf57c0fafU, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[5],  0x4787c62aU, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[6],  0xa8304613U, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[7],  0xfd469501U, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[8],  0x698098d8U, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[9],  0x8b44f7afU, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[10], 0xffff5bb1U, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[11], 0x895cd7beU, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[12], 0x6b901122U, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[13], 0xfd987193U, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[14], 0xa679438eU, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[15], 0x49b40821U, 22);

    MD5_STEP(MD5_G, a, b, c, d, x[1],  0xf61e2562U, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[6],  0xc040b340U, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[11], 0x265e5a51U, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[0],  0xe9b6c7aaU, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[5],  0xd62f105dU, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[10], 0x02441453U, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[15], 0xd8a1e681U, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[4],  0xe7d3fbc8U, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[9],  0x21e1cde6U, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[14], 0xc33707d6U, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[3],  0xf4d50d87U, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[8],  0x455a14edU, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[13], 0xa9e3e905U, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[2],  0xfcefa3f8U, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[7],  0x676f02d9U, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[12], 0x8d2a4c8aU, 20);

    MD5_STEP(MD5_H, a, b, c, d, x[5],  0xfffa3942U, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[8],  0x8771f681U, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[11], 0x6d9d6122U, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[14], 0xfde5380cU, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[1],  0xa4beea44U, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[4],  0x4bdecfa9U, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[7],  0xf6bb4b60U, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[10], 0xbebfbc70U, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[13], 0x289b7ec6U, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[0],  0xeaa127faU, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[3],  0xd4ef3085U, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[6],  0x04881d05U, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[9],  0xd9d4d039U, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[12], 0xe6db99e5U, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[15], 0x1fa27cf8U, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[2],  0xc4ac5665U, 23);

    MD5_STEP(MD5_I, a, b, c, d, x[0],  0xf4292244U, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[7],  0x432aff97U, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[14], 0xab9423a7U, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[5],  0xfc93a039U, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[12], 0x655b59c3U, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[3],  0x8f0ccc92U, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[10], 0xffeff47dU, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[1],  0x85845dd1U, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[8],  0x6fa87e4fU, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[15], 0xfe2ce6e0U, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[6],  0xa3014314U, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[13], 0x4e0811a1U, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[4],  0xf7537e82U, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[11], 0xbd3af235U, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[2],  0x2ad7d2bbU, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[9],  0xeb86d391U, 21);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    rt_wlan_offload_crypto_zero(x, sizeof(x));
}

static void md5_init(struct wlan_offload_md5 *context)
{
    context->state[0] = 0x67452301U;
    context->state[1] = 0xefcdab89U;
    context->state[2] = 0x98badcfeU;
    context->state[3] = 0x10325476U;
    context->bytes = 0;
}

static void md5_update(struct wlan_offload_md5 *context, const uint8_t *data,
                       size_t length)
{
    size_t used = (size_t)(context->bytes & 63U);
    size_t copy;

    context->bytes += length;
    if (used)
    {
        copy = 64U - used;
        if (copy > length)
        {
            copy = length;
        }
        memcpy(context->buffer + used, data, copy);
        used += copy;
        data += copy;
        length -= copy;
        if (used == 64U)
        {
            md5_transform(context->state, context->buffer);
        }
    }
    while (length >= 64U)
    {
        md5_transform(context->state, data);
        data += 64U;
        length -= 64U;
    }
    if (length)
    {
        memcpy(context->buffer, data, length);
    }
}

static void md5_finish(struct wlan_offload_md5 *context, uint8_t digest[16])
{
    uint8_t tail[72];
    uint64_t bits = context->bytes * 8U;
    size_t used = (size_t)(context->bytes & 63U);
    size_t padding = used < 56U ? 56U - used : 120U - used;
    unsigned int index;

    memset(tail, 0, padding + 8U);
    tail[0] = 0x80;
    for (index = 0; index < 8; index++)
    {
        tail[padding + index] = (uint8_t)(bits >> (index * 8U));
    }
    md5_update(context, tail, padding + 8U);
    for (index = 0; index < 4; index++)
    {
        crypto_put_le32(digest + index * 4U, context->state[index]);
    }
    rt_wlan_offload_crypto_zero(tail, sizeof(tail));
    rt_wlan_offload_crypto_zero(context, sizeof(*context));
}

void rt_wlan_offload_hmac_md5(const uint8_t *key, size_t key_length,
                         const uint8_t *data, size_t data_length,
                         uint8_t digest[16])
{
    struct wlan_offload_md5 context;
    uint8_t key_block[64];
    uint8_t inner[16];
    size_t index;

    memset(key_block, 0, sizeof(key_block));
    if (key_length > sizeof(key_block))
    {
        md5_init(&context);
        md5_update(&context, key, key_length);
        md5_finish(&context, key_block);
    }
    else
    {
        memcpy(key_block, key, key_length);
    }
    for (index = 0; index < sizeof(key_block); index++)
    {
        key_block[index] ^= 0x36;
    }
    md5_init(&context);
    md5_update(&context, key_block, sizeof(key_block));
    md5_update(&context, data, data_length);
    md5_finish(&context, inner);
    for (index = 0; index < sizeof(key_block); index++)
    {
        key_block[index] ^= 0x36 ^ 0x5c;
    }
    md5_init(&context);
    md5_update(&context, key_block, sizeof(key_block));
    md5_update(&context, inner, sizeof(inner));
    md5_finish(&context, digest);
    rt_wlan_offload_crypto_zero(inner, sizeof(inner));
    rt_wlan_offload_crypto_zero(key_block, sizeof(key_block));
}

#undef MD5_STEP
#undef MD5_I
#undef MD5_H
#undef MD5_G
#undef MD5_F

static uint32_t crypto_rotr32(uint32_t value, unsigned int bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,
        0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,
        0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,
        0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,
        0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,
        0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,
        0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,
        0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
        0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };
    uint32_t words[64];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    uint32_t first, second, choice, majority;
    unsigned int index;

    for (index = 0; index < 16; index++)
    {
        words[index] = crypto_get_be32(block + index * 4U);
    }
    for (; index < 64; index++)
    {
        first = crypto_rotr32(words[index - 15], 7) ^
                crypto_rotr32(words[index - 15], 18) ^
                (words[index - 15] >> 3);
        second = crypto_rotr32(words[index - 2], 17) ^
                 crypto_rotr32(words[index - 2], 19) ^
                 (words[index - 2] >> 10);
        words[index] = words[index - 16] + first + words[index - 7] + second;
    }
    for (index = 0; index < 64; index++)
    {
        first = crypto_rotr32(e, 6) ^ crypto_rotr32(e, 11) ^
                crypto_rotr32(e, 25);
        choice = (e & f) ^ (~e & g);
        first += h + choice + constants[index] + words[index];
        second = crypto_rotr32(a, 2) ^ crypto_rotr32(a, 13) ^
                 crypto_rotr32(a, 22);
        majority = (a & b) ^ (a & c) ^ (b & c);
        second += majority;
        h = g; g = f; f = e; e = d + first;
        d = c; c = b; b = a; a = first + second;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    rt_wlan_offload_crypto_zero(words, sizeof(words));
}

static void sha256_init(struct wlan_offload_sha256 *context)
{
    static const uint32_t initial[8] = {
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U
    };

    memcpy(context->state, initial, sizeof(initial));
    context->bytes = 0;
}

static void sha256_update(struct wlan_offload_sha256 *context, const uint8_t *data,
                          size_t length)
{
    size_t used = (size_t)(context->bytes & 63U);
    size_t copy;

    context->bytes += length;
    if (used)
    {
        copy = 64U - used;
        if (copy > length)
        {
            copy = length;
        }
        memcpy(context->buffer + used, data, copy);
        used += copy;
        data += copy;
        length -= copy;
        if (used == 64U)
        {
            sha256_transform(context->state, context->buffer);
        }
    }
    while (length >= 64U)
    {
        sha256_transform(context->state, data);
        data += 64U;
        length -= 64U;
    }
    if (length)
    {
        memcpy(context->buffer, data, length);
    }
}

static void sha256_finish(struct wlan_offload_sha256 *context, uint8_t digest[32])
{
    uint8_t tail[72];
    uint64_t bits = context->bytes * 8U;
    size_t used = (size_t)(context->bytes & 63U);
    size_t padding = used < 56U ? 56U - used : 120U - used;
    unsigned int index;

    memset(tail, 0, padding + 8U);
    tail[0] = 0x80;
    for (index = 0; index < 8; index++)
    {
        tail[padding + index] = (uint8_t)(bits >> (56U - index * 8U));
    }
    sha256_update(context, tail, padding + 8U);
    for (index = 0; index < 8; index++)
    {
        crypto_put_be32(digest + index * 4U, context->state[index]);
    }
    rt_wlan_offload_crypto_zero(tail, sizeof(tail));
    rt_wlan_offload_crypto_zero(context, sizeof(*context));
}

void rt_wlan_offload_hmac_sha256(const uint8_t *key, size_t key_length,
                            const uint8_t *data, size_t data_length,
                            uint8_t digest[32])
{
    struct wlan_offload_sha256 context;
    uint8_t key_block[64];
    uint8_t inner[32];
    size_t index;

    memset(key_block, 0, sizeof(key_block));
    if (key_length > sizeof(key_block))
    {
        sha256_init(&context);
        sha256_update(&context, key, key_length);
        sha256_finish(&context, key_block);
    }
    else
    {
        memcpy(key_block, key, key_length);
    }
    for (index = 0; index < sizeof(key_block); index++)
    {
        key_block[index] ^= 0x36;
    }
    sha256_init(&context);
    sha256_update(&context, key_block, sizeof(key_block));
    sha256_update(&context, data, data_length);
    sha256_finish(&context, inner);
    for (index = 0; index < sizeof(key_block); index++)
    {
        key_block[index] ^= 0x36 ^ 0x5c;
    }
    sha256_init(&context);
    sha256_update(&context, key_block, sizeof(key_block));
    sha256_update(&context, inner, sizeof(inner));
    sha256_finish(&context, digest);
    rt_wlan_offload_crypto_zero(inner, sizeof(inner));
    rt_wlan_offload_crypto_zero(key_block, sizeof(key_block));
}

int rt_wlan_offload_sha256_prf(const uint8_t *key, size_t key_length,
                          const char *label, const uint8_t *data,
                          size_t data_length, uint8_t *output,
                          size_t output_length)
{
    uint8_t input[256];
    uint8_t digest[32];
    uint16_t counter = 1;
    uint16_t bits;
    size_t label_length;
    size_t input_length;
    size_t generated = 0;

    if (!key || !label || !output || output_length > 8191U)
    {
        return -1;
    }
    label_length = strlen(label);
    input_length = 2U + label_length + data_length + 2U;
    if (input_length > sizeof(input))
    {
        return -1;
    }
    bits = (uint16_t)(output_length * 8U);
    memcpy(input + 2, label, label_length);
    if (data_length)
    {
        memcpy(input + 2U + label_length, data, data_length);
    }
    input[input_length - 2U] = (uint8_t)bits;
    input[input_length - 1U] = (uint8_t)(bits >> 8);
    while (generated < output_length)
    {
        size_t copy = output_length - generated;

        input[0] = (uint8_t)counter;
        input[1] = (uint8_t)(counter >> 8);
        rt_wlan_offload_hmac_sha256(key, key_length, input, input_length, digest);
        if (copy > sizeof(digest))
        {
            copy = sizeof(digest);
        }
        memcpy(output + generated, digest, copy);
        generated += copy;
        counter++;
    }
    rt_wlan_offload_crypto_zero(digest, sizeof(digest));
    rt_wlan_offload_crypto_zero(input, sizeof(input));
    return 0;
}

int rt_wlan_offload_pbkdf2_sha1(const uint8_t *passphrase, size_t passphrase_length,
                           const uint8_t *ssid, size_t ssid_length,
                           uint8_t pmk[32])
{
    struct wlan_offload_hmac_sha1 prepared;
    uint8_t salt[36];
    uint8_t digest[20];
    uint8_t accumulator[20];
    size_t generated = 0;
    unsigned int block;
    unsigned int iteration;
    size_t copy;
    size_t index;

    if (!passphrase || !passphrase_length || !ssid || ssid_length > 32 || !pmk)
    {
        return -1;
    }
    hmac_sha1_prepare(&prepared, passphrase, passphrase_length);
    memcpy(salt, ssid, ssid_length);
    for (block = 1; generated < 32; block++)
    {
        crypto_put_be32(salt + ssid_length, block);
        hmac_sha1_digest(&prepared, salt, ssid_length + 4U, digest);
        memcpy(accumulator, digest, sizeof(accumulator));
        for (iteration = 1; iteration < 4096; iteration++)
        {
            hmac_sha1_digest(&prepared, digest, sizeof(digest), digest);
            for (index = 0; index < sizeof(accumulator); index++)
            {
                accumulator[index] ^= digest[index];
            }
        }
        copy = 32U - generated;
        if (copy > sizeof(accumulator))
        {
            copy = sizeof(accumulator);
        }
        memcpy(pmk + generated, accumulator, copy);
        generated += copy;
    }
    rt_wlan_offload_crypto_zero(salt, sizeof(salt));
    rt_wlan_offload_crypto_zero(digest, sizeof(digest));
    rt_wlan_offload_crypto_zero(accumulator, sizeof(accumulator));
    rt_wlan_offload_crypto_zero(&prepared, sizeof(prepared));
    return 0;
}

void rt_wlan_offload_wpa_prf(const uint8_t *key, size_t key_length,
                        const char *label, const uint8_t *data,
                        size_t data_length, uint8_t *output,
                        size_t output_length)
{
    uint8_t input[128];
    uint8_t digest[20];
    size_t label_length = strlen(label);
    size_t generated = 0;
    size_t copy;
    uint8_t counter = 0;

    if (label_length + data_length + 2U > sizeof(input))
    {
        return;
    }
    memcpy(input, label, label_length);
    input[label_length] = 0;
    memcpy(input + label_length + 1U, data, data_length);
    while (generated < output_length)
    {
        input[label_length + 1U + data_length] = counter++;
        rt_wlan_offload_hmac_sha1(key, key_length, input,
                             label_length + data_length + 2U, digest);
        copy = output_length - generated;
        if (copy > sizeof(digest))
        {
            copy = sizeof(digest);
        }
        memcpy(output + generated, digest, copy);
        generated += copy;
    }
    rt_wlan_offload_crypto_zero(input, sizeof(input));
    rt_wlan_offload_crypto_zero(digest, sizeof(digest));
}

/* TinyCrypt AES inverse cipher, reduced to the AES-128 ECB primitive. */
static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t aes_inverse_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static uint32_t aes_rotate_word(uint32_t value)
{
    return (value >> 24) | (value << 8);
}

static uint32_t aes_substitute_word(uint32_t value)
{
    return ((uint32_t)aes_sbox[(value >> 24) & 0xffU] << 24) |
           ((uint32_t)aes_sbox[(value >> 16) & 0xffU] << 16) |
           ((uint32_t)aes_sbox[(value >> 8) & 0xffU] << 8) |
           aes_sbox[value & 0xffU];
}

static void aes_set_key(struct wlan_offload_aes_key *schedule, const uint8_t key[16])
{
    static const uint32_t round_constant[11] = {
        0,0x01000000U,0x02000000U,0x04000000U,0x08000000U,0x10000000U,
        0x20000000U,0x40000000U,0x80000000U,0x1b000000U,0x36000000U
    };
    uint32_t temporary;
    unsigned int index;

    for (index = 0; index < 4; index++)
    {
        schedule->words[index] = crypto_get_be32(key + index * 4U);
    }
    for (; index < 44; index++)
    {
        temporary = schedule->words[index - 1];
        if ((index & 3U) == 0)
        {
            temporary = aes_substitute_word(aes_rotate_word(temporary)) ^
                        round_constant[index / 4U];
        }
        schedule->words[index] = schedule->words[index - 4] ^ temporary;
    }
}

static uint8_t aes_double(uint8_t value)
{
    return (uint8_t)((value << 1) ^ ((value >> 7) * 0x1bU));
}

static uint8_t aes_multiply(uint8_t value, uint8_t factor)
{
    uint8_t result = 0;

    while (factor)
    {
        if (factor & 1U)
        {
            result ^= value;
        }
        value = aes_double(value);
        factor >>= 1;
    }
    return result;
}

static void aes_add_round_key(uint8_t state[16], const uint32_t *words)
{
    unsigned int index;

    for (index = 0; index < 4; index++)
    {
        state[index * 4U] ^= (uint8_t)(words[index] >> 24);
        state[index * 4U + 1U] ^= (uint8_t)(words[index] >> 16);
        state[index * 4U + 2U] ^= (uint8_t)(words[index] >> 8);
        state[index * 4U + 3U] ^= (uint8_t)words[index];
    }
}

static void aes_inverse_shift_rows(uint8_t state[16])
{
    uint8_t copy[16];

    memcpy(copy, state, sizeof(copy));
    state[0]=copy[0]; state[1]=copy[13]; state[2]=copy[10]; state[3]=copy[7];
    state[4]=copy[4]; state[5]=copy[1]; state[6]=copy[14]; state[7]=copy[11];
    state[8]=copy[8]; state[9]=copy[5]; state[10]=copy[2]; state[11]=copy[15];
    state[12]=copy[12]; state[13]=copy[9]; state[14]=copy[6]; state[15]=copy[3];
}

static void aes_inverse_mix_columns(uint8_t state[16])
{
    uint8_t copy[16];
    unsigned int column;

    memcpy(copy, state, sizeof(copy));
    for (column = 0; column < 4; column++)
    {
        const uint8_t *in = copy + column * 4U;
        uint8_t *out = state + column * 4U;

        out[0] = aes_multiply(in[0],14) ^ aes_multiply(in[1],11) ^
                 aes_multiply(in[2],13) ^ aes_multiply(in[3],9);
        out[1] = aes_multiply(in[0],9) ^ aes_multiply(in[1],14) ^
                 aes_multiply(in[2],11) ^ aes_multiply(in[3],13);
        out[2] = aes_multiply(in[0],13) ^ aes_multiply(in[1],9) ^
                 aes_multiply(in[2],14) ^ aes_multiply(in[3],11);
        out[3] = aes_multiply(in[0],11) ^ aes_multiply(in[1],13) ^
                 aes_multiply(in[2],9) ^ aes_multiply(in[3],14);
    }
    rt_wlan_offload_crypto_zero(copy, sizeof(copy));
}

static void aes_decrypt_block(const struct wlan_offload_aes_key *schedule,
                              const uint8_t input[16], uint8_t output[16])
{
    uint8_t state[16];
    unsigned int round;
    unsigned int index;

    memcpy(state, input, sizeof(state));
    aes_add_round_key(state, schedule->words + 40);
    for (round = 9; round > 0; round--)
    {
        aes_inverse_shift_rows(state);
        for (index = 0; index < sizeof(state); index++)
        {
            state[index] = aes_inverse_sbox[state[index]];
        }
        aes_add_round_key(state, schedule->words + round * 4U);
        aes_inverse_mix_columns(state);
    }
    aes_inverse_shift_rows(state);
    for (index = 0; index < sizeof(state); index++)
    {
        state[index] = aes_inverse_sbox[state[index]];
    }
    aes_add_round_key(state, schedule->words);
    memcpy(output, state, sizeof(state));
    rt_wlan_offload_crypto_zero(state, sizeof(state));
}

static void aes_shift_rows(uint8_t state[16])
{
    uint8_t copy[16];

    memcpy(copy, state, sizeof(copy));
    state[0]=copy[0]; state[1]=copy[5]; state[2]=copy[10]; state[3]=copy[15];
    state[4]=copy[4]; state[5]=copy[9]; state[6]=copy[14]; state[7]=copy[3];
    state[8]=copy[8]; state[9]=copy[13]; state[10]=copy[2]; state[11]=copy[7];
    state[12]=copy[12]; state[13]=copy[1]; state[14]=copy[6]; state[15]=copy[11];
}

static void aes_mix_columns(uint8_t state[16])
{
    uint8_t copy[16];
    unsigned int column;

    memcpy(copy, state, sizeof(copy));
    for (column = 0; column < 4; column++)
    {
        const uint8_t *in = copy + column * 4U;
        uint8_t *out = state + column * 4U;

        out[0] = aes_multiply(in[0], 2) ^ aes_multiply(in[1], 3) ^
                 in[2] ^ in[3];
        out[1] = in[0] ^ aes_multiply(in[1], 2) ^
                 aes_multiply(in[2], 3) ^ in[3];
        out[2] = in[0] ^ in[1] ^ aes_multiply(in[2], 2) ^
                 aes_multiply(in[3], 3);
        out[3] = aes_multiply(in[0], 3) ^ in[1] ^ in[2] ^
                 aes_multiply(in[3], 2);
    }
    rt_wlan_offload_crypto_zero(copy, sizeof(copy));
}

static void aes_encrypt_block(const struct wlan_offload_aes_key *schedule,
                              const uint8_t input[16], uint8_t output[16])
{
    uint8_t state[16];
    unsigned int round;
    unsigned int index;

    memcpy(state, input, sizeof(state));
    aes_add_round_key(state, schedule->words);
    for (round = 1; round < 10; round++)
    {
        for (index = 0; index < sizeof(state); index++)
        {
            state[index] = aes_sbox[state[index]];
        }
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, schedule->words + round * 4U);
    }
    for (index = 0; index < sizeof(state); index++)
    {
        state[index] = aes_sbox[state[index]];
    }
    aes_shift_rows(state);
    aes_add_round_key(state, schedule->words + 40);
    memcpy(output, state, sizeof(state));
    rt_wlan_offload_crypto_zero(state, sizeof(state));
}

static void aes_cmac_double(uint8_t value[16])
{
    uint8_t carry = value[0] >> 7;
    unsigned int index;

    for (index = 0; index < 15; index++)
    {
        value[index] = (uint8_t)((value[index] << 1) |
                                 (value[index + 1] >> 7));
    }
    value[15] <<= 1;
    if (carry)
    {
        value[15] ^= 0x87;
    }
}

int rt_wlan_offload_aes_cmac(const uint8_t key[16], const uint8_t *data,
                        size_t data_length, uint8_t mic[16])
{
    struct wlan_offload_aes_key schedule;
    uint8_t state[16] = {0};
    uint8_t subkey[16] = {0};
    uint8_t block[16];
    size_t blocks;
    size_t index;
    size_t position;
    int complete;

    if (!key || (!data && data_length) || !mic)
    {
        return -1;
    }
    aes_set_key(&schedule, key);
    aes_encrypt_block(&schedule, subkey, subkey);
    aes_cmac_double(subkey);
    complete = data_length != 0 && (data_length & 15U) == 0;
    blocks = (data_length + 15U) / 16U;
    if (!blocks)
    {
        blocks = 1;
    }
    for (position = 0; position + 1U < blocks; position++)
    {
        for (index = 0; index < sizeof(block); index++)
        {
            block[index] = state[index] ^ data[position * 16U + index];
        }
        aes_encrypt_block(&schedule, block, state);
    }
    if (!complete)
    {
        aes_cmac_double(subkey);
    }
    memset(block, 0, sizeof(block));
    index = data_length - (blocks - 1U) * 16U;
    if (index)
    {
        memcpy(block, data + (blocks - 1U) * 16U, index);
    }
    if (!complete)
    {
        block[index] = 0x80;
    }
    for (index = 0; index < sizeof(block); index++)
    {
        block[index] ^= state[index] ^ subkey[index];
    }
    aes_encrypt_block(&schedule, block, mic);
    rt_wlan_offload_crypto_zero(&schedule, sizeof(schedule));
    rt_wlan_offload_crypto_zero(state, sizeof(state));
    rt_wlan_offload_crypto_zero(subkey, sizeof(subkey));
    rt_wlan_offload_crypto_zero(block, sizeof(block));
    return 0;
}

int rt_wlan_offload_aes_wrap(const uint8_t kek[16], const uint8_t *input,
                        size_t input_length, uint8_t *output,
                        size_t *output_length)
{
    struct wlan_offload_aes_key schedule;
    uint8_t accumulator[8] = {
        0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6
    };
    uint8_t block[16];
    size_t count;
    size_t index;
    unsigned int round;
    uint64_t counter;

    if (!kek || !input || !output || !output_length || input_length < 16U ||
        (input_length & 7U) || *output_length < input_length + 8U)
    {
        return -1;
    }
    count = input_length / 8U;
    memcpy(output + 8U, input, input_length);
    aes_set_key(&schedule, kek);
    for (round = 0; round < 6U; round++)
    {
        for (index = 1U; index <= count; index++)
        {
            memcpy(block, accumulator, 8);
            memcpy(block + 8, output + index * 8U, 8);
            aes_encrypt_block(&schedule, block, block);
            counter = (uint64_t)count * round + index;
            memcpy(accumulator, block, 8);
            accumulator[7] ^= (uint8_t)counter;
            accumulator[6] ^= (uint8_t)(counter >> 8);
            accumulator[5] ^= (uint8_t)(counter >> 16);
            accumulator[4] ^= (uint8_t)(counter >> 24);
            accumulator[3] ^= (uint8_t)(counter >> 32);
            accumulator[2] ^= (uint8_t)(counter >> 40);
            accumulator[1] ^= (uint8_t)(counter >> 48);
            accumulator[0] ^= (uint8_t)(counter >> 56);
            memcpy(output + index * 8U, block + 8, 8);
        }
    }
    memcpy(output, accumulator, sizeof(accumulator));
    *output_length = input_length + 8U;
    rt_wlan_offload_crypto_zero(&schedule, sizeof(schedule));
    rt_wlan_offload_crypto_zero(accumulator, sizeof(accumulator));
    rt_wlan_offload_crypto_zero(block, sizeof(block));
    return 0;
}

int rt_wlan_offload_aes_unwrap(const uint8_t kek[16], const uint8_t *input,
                          size_t input_length, uint8_t *output,
                          size_t *output_length)
{
    static const uint8_t initial_value[8] = {
        0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6
    };
    struct wlan_offload_aes_key schedule;
    uint8_t accumulator[8];
    uint8_t block[16];
    size_t count;
    size_t index;
    unsigned int round;
    uint64_t counter;

    if (!kek || !input || !output || !output_length || input_length < 24U ||
        (input_length & 7U))
    {
        return -1;
    }
    count = input_length / 8U - 1U;
    memcpy(accumulator, input, sizeof(accumulator));
    memcpy(output, input + 8U, input_length - 8U);
    aes_set_key(&schedule, kek);
    for (round = 6; round-- > 0; )
    {
        for (index = count; index > 0; index--)
        {
            counter = (uint64_t)count * round + index;
            memcpy(block, accumulator, 8);
            block[7] ^= (uint8_t)counter;
            block[6] ^= (uint8_t)(counter >> 8);
            block[5] ^= (uint8_t)(counter >> 16);
            block[4] ^= (uint8_t)(counter >> 24);
            block[3] ^= (uint8_t)(counter >> 32);
            block[2] ^= (uint8_t)(counter >> 40);
            block[1] ^= (uint8_t)(counter >> 48);
            block[0] ^= (uint8_t)(counter >> 56);
            memcpy(block + 8, output + (index - 1U) * 8U, 8);
            aes_decrypt_block(&schedule, block, block);
            memcpy(accumulator, block, 8);
            memcpy(output + (index - 1U) * 8U, block + 8, 8);
        }
    }
    *output_length = input_length - 8U;
    index = rt_wlan_offload_crypto_equal(accumulator, initial_value, 8) ? 0U : 1U;
    rt_wlan_offload_crypto_zero(&schedule, sizeof(schedule));
    rt_wlan_offload_crypto_zero(accumulator, sizeof(accumulator));
    rt_wlan_offload_crypto_zero(block, sizeof(block));
    if (index)
    {
        rt_wlan_offload_crypto_zero(output, *output_length);
        *output_length = 0;
        return -1;
    }
    return 0;
}

int rt_wlan_offload_rc4_skip(const uint8_t *key, size_t key_length,
                        size_t skip, uint8_t *data, size_t data_length)
{
    uint8_t state[256];
    uint8_t temporary;
    uint8_t x = 0;
    uint8_t y = 0;
    size_t index;

    if (!key || !key_length || !data)
    {
        return -1;
    }
    for (index = 0; index < sizeof(state); index++)
    {
        state[index] = (uint8_t)index;
    }
    for (index = 0; index < sizeof(state); index++)
    {
        y = (uint8_t)(y + state[index] + key[index % key_length]);
        temporary = state[index];
        state[index] = state[y];
        state[y] = temporary;
    }
    y = 0;
    for (index = 0; index < skip + data_length; index++)
    {
        x = (uint8_t)(x + 1U);
        y = (uint8_t)(y + state[x]);
        temporary = state[x];
        state[x] = state[y];
        state[y] = temporary;
        if (index >= skip)
        {
            data[index - skip] ^=
                state[(uint8_t)(state[x] + state[y])];
        }
    }
    rt_wlan_offload_crypto_zero(state, sizeof(state));
    return 0;
}
