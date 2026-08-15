/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
 *
 * SAE behavior follows the BSD-licensed hostap implementation. Elliptic-curve
 * arithmetic is provided by the bundled BSD-licensed TinyCrypt P-256 code.
 */
#include "wlan_offload_sae.h"
#include "wlan_offload_crypto.h"

#include <string.h>
#include <tinycrypt/ecc.h>

#define SAE_WORDS 8
#define SAE_POINT_WORDS 16
#define SAE_GROUP 19
#define SAE_MIN_PWE_ITERATIONS 40
#define SAE_MAX_PWE_ITERATIONS 200

static void sae_words_shift_right(uint32_t value[SAE_WORDS], unsigned int bits)
{
    unsigned int iteration;

    while (bits--)
    {
        uint32_t carry = 0;

        for (iteration = SAE_WORDS; iteration-- > 0; )
        {
            uint32_t next = value[iteration] << 31;

            value[iteration] = (value[iteration] >> 1) | carry;
            carry = next;
        }
    }
}

static void sae_mod_exp(uint32_t output[SAE_WORDS],
                        const uint32_t base[SAE_WORDS],
                        const uint32_t exponent[SAE_WORDS],
                        uECC_Curve curve)
{
    uint32_t result[SAE_WORDS] = {1};
    unsigned int bit;

    for (bit = SAE_WORDS * 32U; bit-- > 0; )
    {
        uECC_vli_modMult_fast(result, result, result, curve);
        if (uECC_vli_testBit(exponent, (bitcount_t)bit))
        {
            uECC_vli_modMult_fast(result, result, base, curve);
        }
    }
    memcpy(output, result, sizeof(result));
    rt_wlan_offload_crypto_zero(result, sizeof(result));
}

static int sae_point_multiply(uint32_t output[SAE_POINT_WORDS],
                              const uint32_t point[SAE_POINT_WORDS],
                              const uint32_t scalar[SAE_WORDS],
                              uECC_Curve curve)
{
    uint32_t regular[2][SAE_WORDS];
    uint32_t carry;

    carry = regularize_k(scalar, regular[0], regular[1], curve);
    EccPoint_mult(output, point, regular[!carry], 0,
                  curve->num_n_bits + 1, curve);
    rt_wlan_offload_crypto_zero(regular, sizeof(regular));
    return EccPoint_isZero(output, curve) ? -1 : 0;
}

static int sae_point_add(uint32_t output[SAE_POINT_WORDS],
                         const uint32_t left[SAE_POINT_WORDS],
                         const uint32_t right[SAE_POINT_WORDS],
                         uECC_Curve curve)
{
    uint32_t denominator[SAE_WORDS];
    uint32_t numerator[SAE_WORDS];
    uint32_t slope[SAE_WORDS];
    uint32_t x[SAE_WORDS];
    uint32_t y[SAE_WORDS];
    uint32_t three[SAE_WORDS] = {3};

    if (uECC_vli_cmp_unsafe(left, right, SAE_WORDS) == 0)
    {
        if (uECC_vli_cmp_unsafe(left + SAE_WORDS,
                               right + SAE_WORDS, SAE_WORDS) != 0)
        {
            return -1;
        }
        uECC_vli_modAdd(denominator, left + SAE_WORDS,
                        left + SAE_WORDS, curve->p, SAE_WORDS);
        uECC_vli_modMult_fast(numerator, left, left, curve);
        uECC_vli_modAdd(slope, numerator, numerator,
                        curve->p, SAE_WORDS);
        uECC_vli_modAdd(numerator, slope, numerator,
                        curve->p, SAE_WORDS);
        uECC_vli_modSub(numerator, numerator, three,
                        curve->p, SAE_WORDS);
    }
    else
    {
        uECC_vli_modSub(denominator, right, left, curve->p, SAE_WORDS);
        uECC_vli_modSub(numerator, right + SAE_WORDS, left + SAE_WORDS,
                        curve->p, SAE_WORDS);
    }
    uECC_vli_modInv(denominator, denominator, curve->p, SAE_WORDS);
    uECC_vli_modMult_fast(slope, numerator, denominator, curve);
    uECC_vli_modMult_fast(x, slope, slope, curve);
    uECC_vli_modSub(x, x, left, curve->p, SAE_WORDS);
    uECC_vli_modSub(x, x, right, curve->p, SAE_WORDS);
    uECC_vli_modSub(y, left, x, curve->p, SAE_WORDS);
    uECC_vli_modMult_fast(y, slope, y, curve);
    uECC_vli_modSub(y, y, left + SAE_WORDS, curve->p, SAE_WORDS);
    memcpy(output, x, sizeof(x));
    memcpy(output + SAE_WORDS, y, sizeof(y));
    rt_wlan_offload_crypto_zero(denominator, sizeof(denominator));
    rt_wlan_offload_crypto_zero(numerator, sizeof(numerator));
    rt_wlan_offload_crypto_zero(slope, sizeof(slope));
    rt_wlan_offload_crypto_zero(x, sizeof(x));
    rt_wlan_offload_crypto_zero(y, sizeof(y));
    rt_wlan_offload_crypto_zero(three, sizeof(three));
    return uECC_valid_point(output, curve) == 0 ? 0 : -1;
}

static int sae_derive_pwe(uint32_t pwe[SAE_POINT_WORDS],
                          const uint8_t own_address[6],
                          const uint8_t peer_address[6],
                          const uint8_t *password, size_t password_length,
                          uECC_Curve curve)
{
    uint8_t address_key[12];
    uint8_t input[RT_WLAN_OFFLOAD_SAE_MAX_PASSWORD_LENGTH + 1U];
    uint8_t seed[32];
    uint8_t candidate_bytes[32];
    uint32_t candidate[SAE_WORDS];
    uint32_t y_squared[SAE_WORDS];
    uint32_t y[SAE_WORDS];
    uint32_t check[SAE_WORDS];
    uint32_t exponent[SAE_WORDS];
    uint32_t zero[SAE_WORDS] = {0};
    uint32_t selected[SAE_POINT_WORDS] = {0};
    unsigned int counter;
    unsigned int found = 0;
    int result = -1;

    if (memcmp(own_address, peer_address, 6) > 0)
    {
        memcpy(address_key, own_address, 6);
        memcpy(address_key + 6, peer_address, 6);
    }
    else
    {
        memcpy(address_key, peer_address, 6);
        memcpy(address_key + 6, own_address, 6);
    }
    memcpy(input, password, password_length);
    memcpy(exponent, curve->p, sizeof(exponent));
    for (counter = 0; counter < SAE_WORDS; counter++)
    {
        exponent[counter]++;
        if (exponent[counter] != 0)
        {
            break;
        }
    }
    sae_words_shift_right(exponent, 2);

    for (counter = 1; counter <= SAE_MAX_PWE_ITERATIONS; counter++)
    {
        uint32_t valid;
        uint32_t select;
        uint32_t mask;
        unsigned int index;

        input[password_length] = (uint8_t)counter;
        rt_wlan_offload_hmac_sha256(address_key, sizeof(address_key), input,
                               password_length + 1U, seed);
        if (rt_wlan_offload_sha256_prf(seed, sizeof(seed),
                                  "SAE Hunting and Pecking",
                                  (const uint8_t *)
                                  "\xff\xff\xff\xff\x00\x00\x00\x01"
                                  "\x00\x00\x00\x00\x00\x00\x00\x00"
                                  "\x00\x00\x00\x00\xff\xff\xff\xff"
                                  "\xff\xff\xff\xff\xff\xff\xff\xff",
                                  32, candidate_bytes,
                                  sizeof(candidate_bytes)) != 0)
        {
            goto exit;
        }
        uECC_vli_bytesToNative(candidate, candidate_bytes, 32);
        valid = uECC_vli_cmp_unsafe(candidate, curve->p, SAE_WORDS) < 0;
        if (!valid)
        {
            memset(candidate, 0, sizeof(candidate));
        }
        curve->x_side(y_squared, candidate, curve);
        sae_mod_exp(y, y_squared, exponent, curve);
        uECC_vli_modMult_fast(check, y, y, curve);
        valid &= uECC_vli_cmp_unsafe(check, y_squared, SAE_WORDS) == 0;
        if (((y[0] & 1U) != (seed[31] & 1U)))
        {
            uECC_vli_modSub(y, zero, y, curve->p, SAE_WORDS);
        }
        select = valid & !found;
        mask = 0U - select;
        for (index = 0; index < SAE_WORDS; index++)
        {
            selected[index] = (selected[index] & ~mask) |
                              (candidate[index] & mask);
            selected[index + SAE_WORDS] =
                (selected[index + SAE_WORDS] & ~mask) | (y[index] & mask);
        }
        found |= valid;
        if (counter >= SAE_MIN_PWE_ITERATIONS && found)
        {
            break;
        }
    }
    if (!found || uECC_valid_point(selected, curve) != 0)
    {
        goto exit;
    }
    memcpy(pwe, selected, sizeof(selected));
    result = 0;

exit:
    rt_wlan_offload_crypto_zero(address_key, sizeof(address_key));
    rt_wlan_offload_crypto_zero(input, sizeof(input));
    rt_wlan_offload_crypto_zero(seed, sizeof(seed));
    rt_wlan_offload_crypto_zero(candidate_bytes, sizeof(candidate_bytes));
    rt_wlan_offload_crypto_zero(candidate, sizeof(candidate));
    rt_wlan_offload_crypto_zero(y_squared, sizeof(y_squared));
    rt_wlan_offload_crypto_zero(y, sizeof(y));
    rt_wlan_offload_crypto_zero(check, sizeof(check));
    rt_wlan_offload_crypto_zero(exponent, sizeof(exponent));
    rt_wlan_offload_crypto_zero(zero, sizeof(zero));
    rt_wlan_offload_crypto_zero(selected, sizeof(selected));
    return result;
}

static int sae_random_scalar(uint32_t scalar[SAE_WORDS], uECC_Curve curve,
                             rt_wlan_offload_sae_random_t random,
                             void *random_context)
{
    uint8_t bytes[32];
    unsigned int attempt;

    for (attempt = 0; attempt < 64; attempt++)
    {
        if (random(random_context, bytes, sizeof(bytes)) != 0)
        {
            return -1;
        }
        uECC_vli_bytesToNative(scalar, bytes, sizeof(bytes));
        if (!uECC_vli_isZero(scalar, SAE_WORDS) &&
            !(scalar[0] == 1U &&
              uECC_vli_isZero(scalar + 1, SAE_WORDS - 1)) &&
            uECC_vli_cmp_unsafe(scalar, curve->n, SAE_WORDS) < 0)
        {
            rt_wlan_offload_crypto_zero(bytes, sizeof(bytes));
            return 0;
        }
    }
    rt_wlan_offload_crypto_zero(bytes, sizeof(bytes));
    return -1;
}

int rt_wlan_offload_sae_prepare(struct rt_wlan_offload_sae *sae,
                           const uint8_t own_address[6],
                           const uint8_t peer_address[6],
                           const uint8_t *password, size_t password_length,
                           rt_wlan_offload_sae_random_t random, void *random_context)
{
    uECC_Curve curve = uECC_secp256r1();
    uint32_t mask[SAE_WORDS];
    uint32_t zero[SAE_WORDS] = {0};
    unsigned int attempt;

    if (!sae || !own_address || !peer_address || !password ||
        !password_length ||
        password_length > RT_WLAN_OFFLOAD_SAE_MAX_PASSWORD_LENGTH || !random)
    {
        return -1;
    }
    rt_wlan_offload_sae_clear(sae);
    if (sae_derive_pwe(sae->pwe, own_address, peer_address,
                       password, password_length, curve) != 0)
    {
        return -1;
    }
    for (attempt = 0; attempt < 64; attempt++)
    {
        if (sae_random_scalar(sae->random, curve, random, random_context) != 0 ||
            sae_random_scalar(mask, curve, random, random_context) != 0)
        {
            goto fail;
        }
        uECC_vli_modAdd(sae->own_scalar, sae->random, mask,
                        curve->n, SAE_WORDS);
        if (!uECC_vli_isZero(sae->own_scalar, SAE_WORDS) &&
            !(sae->own_scalar[0] == 1U &&
              uECC_vli_isZero(sae->own_scalar + 1, SAE_WORDS - 1)))
        {
            break;
        }
    }
    if (attempt == 64 ||
        sae_point_multiply(sae->own_element, sae->pwe, mask, curve) != 0)
    {
        goto fail;
    }
    uECC_vli_modSub(sae->own_element + SAE_WORDS, zero,
                    sae->own_element + SAE_WORDS, curve->p, SAE_WORDS);
    sae->prepared = 1;
    rt_wlan_offload_crypto_zero(mask, sizeof(mask));
    rt_wlan_offload_crypto_zero(zero, sizeof(zero));
    return 0;

fail:
    rt_wlan_offload_crypto_zero(mask, sizeof(mask));
    rt_wlan_offload_crypto_zero(zero, sizeof(zero));
    rt_wlan_offload_sae_clear(sae);
    return -1;
}

int rt_wlan_offload_sae_write_commit(const struct rt_wlan_offload_sae *sae,
                                uint8_t output[RT_WLAN_OFFLOAD_SAE_COMMIT_LENGTH])
{
    if (!sae || !sae->prepared || !output)
    {
        return -1;
    }
    output[0] = SAE_GROUP;
    output[1] = 0;
    uECC_vli_nativeToBytes(output + 2, 32, sae->own_scalar);
    uECC_vli_nativeToBytes(output + 34, 32, sae->own_element);
    uECC_vli_nativeToBytes(output + 66, 32,
                           sae->own_element + SAE_WORDS);
    return 0;
}

int rt_wlan_offload_sae_process_commit(struct rt_wlan_offload_sae *sae,
                                  const uint8_t *data, size_t length)
{
    uECC_Curve curve = uECC_secp256r1();
    uint32_t intermediate[SAE_POINT_WORDS];
    uint32_t shared[SAE_POINT_WORDS];
    uint32_t scalar_sum[SAE_WORDS];
    uint8_t k[32];
    uint8_t zero[32] = {0};
    uint8_t keyseed[32];
    uint8_t keys[64];

    if (!sae || !sae->prepared || !data ||
        length < RT_WLAN_OFFLOAD_SAE_COMMIT_LENGTH || data[0] != SAE_GROUP ||
        data[1] != 0)
    {
        return -1;
    }
    uECC_vli_bytesToNative(sae->peer_scalar, data + 2, 32);
    uECC_vli_bytesToNative(sae->peer_element, data + 34, 32);
    uECC_vli_bytesToNative(sae->peer_element + SAE_WORDS, data + 66, 32);
    if (uECC_vli_isZero(sae->peer_scalar, SAE_WORDS) ||
        (sae->peer_scalar[0] == 1U &&
         uECC_vli_isZero(sae->peer_scalar + 1, SAE_WORDS - 1)) ||
        uECC_vli_cmp_unsafe(sae->peer_scalar, curve->n, SAE_WORDS) >= 0 ||
        uECC_valid_point(sae->peer_element, curve) != 0 ||
        (uECC_vli_cmp_unsafe(sae->peer_scalar, sae->own_scalar,
                            SAE_WORDS) == 0 &&
         memcmp(sae->peer_element, sae->own_element,
                sizeof(sae->own_element)) == 0))
    {
        return -1;
    }
    if (sae_point_multiply(intermediate, sae->pwe,
                           sae->peer_scalar, curve) != 0 ||
        sae_point_add(intermediate, intermediate,
                      sae->peer_element, curve) != 0 ||
        sae_point_multiply(shared, intermediate,
                           sae->random, curve) != 0)
    {
        goto fail;
    }
    uECC_vli_nativeToBytes(k, sizeof(k), shared);
    rt_wlan_offload_hmac_sha256(zero, sizeof(zero), k, sizeof(k), keyseed);
    uECC_vli_modAdd(scalar_sum, sae->own_scalar, sae->peer_scalar,
                    curve->n, SAE_WORDS);
    uECC_vli_nativeToBytes(k, sizeof(k), scalar_sum);
    if (rt_wlan_offload_sha256_prf(keyseed, sizeof(keyseed), "SAE KCK and PMK",
                              k, sizeof(k), keys, sizeof(keys)) != 0)
    {
        goto fail;
    }
    memcpy(sae->kck, keys, 32);
    memcpy(sae->pmk, keys + 32, 32);
    memcpy(sae->pmkid, k, sizeof(sae->pmkid));
    sae->committed = 1;
    rt_wlan_offload_crypto_zero(intermediate, sizeof(intermediate));
    rt_wlan_offload_crypto_zero(shared, sizeof(shared));
    rt_wlan_offload_crypto_zero(scalar_sum, sizeof(scalar_sum));
    rt_wlan_offload_crypto_zero(k, sizeof(k));
    rt_wlan_offload_crypto_zero(zero, sizeof(zero));
    rt_wlan_offload_crypto_zero(keyseed, sizeof(keyseed));
    rt_wlan_offload_crypto_zero(keys, sizeof(keys));
    return 0;

fail:
    rt_wlan_offload_crypto_zero(intermediate, sizeof(intermediate));
    rt_wlan_offload_crypto_zero(shared, sizeof(shared));
    rt_wlan_offload_crypto_zero(scalar_sum, sizeof(scalar_sum));
    rt_wlan_offload_crypto_zero(k, sizeof(k));
    rt_wlan_offload_crypto_zero(keyseed, sizeof(keyseed));
    rt_wlan_offload_crypto_zero(keys, sizeof(keys));
    return -1;
}

static void sae_confirm(const struct rt_wlan_offload_sae *sae, uint16_t counter,
                        int peer_first, uint8_t output[32])
{
    uint8_t input[194];
    const uint32_t *scalar1 = peer_first ? sae->peer_scalar : sae->own_scalar;
    const uint32_t *scalar2 = peer_first ? sae->own_scalar : sae->peer_scalar;
    const uint32_t *element1 = peer_first ? sae->peer_element : sae->own_element;
    const uint32_t *element2 = peer_first ? sae->own_element : sae->peer_element;

    input[0] = (uint8_t)counter;
    input[1] = (uint8_t)(counter >> 8);
    uECC_vli_nativeToBytes(input + 2, 32, scalar1);
    uECC_vli_nativeToBytes(input + 34, 32, element1);
    uECC_vli_nativeToBytes(input + 66, 32, element1 + SAE_WORDS);
    uECC_vli_nativeToBytes(input + 98, 32, scalar2);
    uECC_vli_nativeToBytes(input + 130, 32, element2);
    uECC_vli_nativeToBytes(input + 162, 32, element2 + SAE_WORDS);
    rt_wlan_offload_hmac_sha256(sae->kck, sizeof(sae->kck),
                           input, sizeof(input), output);
    rt_wlan_offload_crypto_zero(input, sizeof(input));
}

int rt_wlan_offload_sae_write_confirm(struct rt_wlan_offload_sae *sae,
                                 uint8_t output[RT_WLAN_OFFLOAD_SAE_CONFIRM_LENGTH])
{
    if (!sae || !sae->committed || !output)
    {
        return -1;
    }
    if (sae->send_confirm != 0xffffU)
    {
        sae->send_confirm++;
    }
    output[0] = (uint8_t)sae->send_confirm;
    output[1] = (uint8_t)(sae->send_confirm >> 8);
    sae_confirm(sae, sae->send_confirm, 0, output + 2);
    return 0;
}

int rt_wlan_offload_sae_check_confirm(struct rt_wlan_offload_sae *sae,
                                 const uint8_t *data, size_t length)
{
    uint8_t expected[32];
    uint16_t counter;
    int valid;

    if (!sae || !sae->committed || !data ||
        length < RT_WLAN_OFFLOAD_SAE_CONFIRM_LENGTH)
    {
        return -1;
    }
    counter = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (counter <= sae->peer_confirm)
    {
        return -1;
    }
    sae_confirm(sae, counter, 1, expected);
    valid = rt_wlan_offload_crypto_equal(expected, data + 2, sizeof(expected));
    rt_wlan_offload_crypto_zero(expected, sizeof(expected));
    if (!valid)
    {
        return -1;
    }
    sae->peer_confirm = counter;
    return 0;
}

void rt_wlan_offload_sae_clear(struct rt_wlan_offload_sae *sae)
{
    if (sae)
    {
        rt_wlan_offload_crypto_zero(sae, sizeof(*sae));
    }
}
