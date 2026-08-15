/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../wlan_offload_sae.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct test_random
{
    unsigned int value;
};

struct vector_random
{
    const unsigned char *data;
    size_t length;
    size_t offset;
};

static int test_random(void *context, unsigned char *data, size_t length)
{
    struct test_random *random = context;
    size_t index;

    for (index = 0; index < length; index++)
    {
        random->value = random->value * 1664525U + 1013904223U;
        data[index] = (unsigned char)(random->value >> 24);
    }
    return 0;
}

static int vector_random(void *context, unsigned char *data, size_t length)
{
    struct vector_random *random = context;

    if (length > random->length - random->offset)
    {
        return -1;
    }
    memcpy(data, random->data + random->offset, length);
    random->offset += length;
    return 0;
}

static void test_ieee_annex_j10(void)
{
    static const unsigned char address_a[6] =
        {0x4d,0x3f,0x2f,0xff,0xe3,0x87};
    static const unsigned char address_b[6] =
        {0xa5,0xd8,0xaa,0x95,0x8e,0x3c};
    static const unsigned char password[] = "mekmitasdigoat";
    static const unsigned char random_and_mask[64] = {
        0x99,0x24,0x65,0xfd,0x3d,0xaa,0x3c,0x60,
        0xaa,0x65,0x65,0xb7,0xf6,0x2a,0x2a,0x7f,
        0x2e,0x12,0xdd,0x12,0xf1,0x98,0xfa,0xf4,
        0xfb,0xed,0x89,0xd7,0xff,0x1a,0xce,0x94,
        0x95,0x07,0xa9,0x0f,0x77,0x7a,0x04,0x4d,
        0x6a,0x08,0x30,0xb9,0x1e,0xa3,0xd5,0xdd,
        0x70,0xbe,0xce,0x44,0xe1,0xac,0xff,0xb8,
        0x69,0x83,0xb5,0xe1,0xbf,0x9f,0xb3,0x22
    };
    static const unsigned char local_commit[98] = {
        0x13,0x00,0x2e,0x2c,0x0f,0x0d,0xb5,0x24,
        0x40,0xad,0x14,0x6d,0x96,0x71,0x14,0xce,
        0x00,0x5c,0xe1,0xea,0xb0,0xaa,0x2c,0x2e,
        0x5c,0x28,0x71,0xb7,0x74,0xf6,0xc2,0x57,
        0x5c,0x65,0xd5,0xad,0x9e,0x00,0x82,0x97,
        0x07,0xaa,0x36,0xba,0x8b,0x85,0x97,0x38,
        0xfc,0x96,0x1d,0x08,0x24,0x35,0x05,0xf4,
        0x7c,0x03,0x53,0x76,0xd7,0xac,0x4b,0xc8,
        0xd7,0xb9,0x50,0x83,0xbf,0x43,0x82,0x7d,
        0x0f,0xc3,0x1e,0xd7,0x78,0xdd,0x36,0x71,
        0xfd,0x21,0xa4,0x6d,0x10,0x91,0xd6,0x4b,
        0x6f,0x9a,0x1e,0x12,0x72,0x62,0x13,0x25,
        0xdb,0xe1
    };
    static const unsigned char peer_commit[98] = {
        0x13,0x00,0x59,0x1b,0x96,0xf3,0x39,0x7f,
        0xb9,0x45,0x10,0x08,0x48,0xe7,0xb5,0x50,
        0x54,0x3b,0x67,0x20,0xd8,0x83,0x37,0xee,
        0x93,0xfc,0x49,0xfd,0x6d,0xf7,0xe0,0x8b,
        0x52,0x23,0xe7,0x1b,0x9b,0xb0,0x48,0xd3,
        0x87,0x3f,0x20,0x55,0x69,0x53,0xa9,0x6c,
        0x91,0x53,0x6f,0xd8,0xee,0x6c,0xa9,0xb4,
        0xa6,0x8a,0x14,0x8b,0x05,0x6a,0x90,0x9b,
        0xe0,0x3e,0x83,0xae,0x20,0x8f,0x60,0xf8,
        0xef,0x55,0x37,0x85,0x80,0x74,0xdb,0x06,
        0x68,0x70,0x32,0x39,0x98,0x62,0x99,0x9b,
        0x51,0x1e,0x0a,0x15,0x52,0xa5,0xfe,0xa3,
        0x17,0xc2
    };
    static const unsigned char expected_kck[32] = {
        0x1e,0x73,0x3f,0x6d,0x9b,0xd5,0x32,0x56,
        0x28,0x73,0x04,0x33,0x88,0x31,0xb0,0x9a,
        0x39,0x40,0x6d,0x12,0x10,0x17,0x07,0x3a,
        0x5c,0x30,0xdb,0x36,0xf3,0x6c,0xb8,0x1a
    };
    static const unsigned char expected_pmk[32] = {
        0x4e,0x4d,0xfa,0xb1,0xa2,0xdd,0x8a,0xc1,
        0xa9,0x17,0x90,0xf9,0x53,0xfa,0xaa,0x45,
        0x2a,0xe5,0xc6,0x87,0x3a,0xb7,0x5b,0x63,
        0x60,0x5b,0xa6,0x63,0xf8,0xa7,0xfe,0x59
    };
    static const unsigned char expected_pmkid[16] = {
        0x87,0x47,0xa6,0x00,0xee,0xa3,0xf9,0xf2,
        0x24,0x75,0xdf,0x58,0xca,0x1e,0x54,0x98
    };
    struct vector_random random = {
        random_and_mask, sizeof(random_and_mask), 0
    };
    struct rt_wlan_offload_sae sae;
    unsigned char commit[RT_WLAN_OFFLOAD_SAE_COMMIT_LENGTH];

    assert(rt_wlan_offload_sae_prepare(&sae, address_a, address_b, password,
                                  sizeof(password) - 1,
                                  vector_random, &random) == 0);
    assert(rt_wlan_offload_sae_write_commit(&sae, commit) == 0);
    assert(memcmp(commit, local_commit, sizeof(commit)) == 0);
    assert(rt_wlan_offload_sae_process_commit(&sae, peer_commit,
                                         sizeof(peer_commit)) == 0);
    assert(memcmp(sae.kck, expected_kck, sizeof(expected_kck)) == 0);
    assert(memcmp(sae.pmk, expected_pmk, sizeof(expected_pmk)) == 0);
    assert(memcmp(sae.pmkid, expected_pmkid, sizeof(expected_pmkid)) == 0);
    rt_wlan_offload_sae_clear(&sae);
}

static void test_password_lengths(void)
{
    static const unsigned char address_a[6] = {0x02,0,0,0,0,1};
    static const unsigned char address_b[6] = {0x02,0,0,0,0,2};
    unsigned char password[RT_WLAN_OFFLOAD_SAE_MAX_PASSWORD_LENGTH + 1U];
    struct test_random random = {3};
    struct rt_wlan_offload_sae sae;

    memset(password, 'p', sizeof(password));
    assert(rt_wlan_offload_sae_prepare(&sae, address_a, address_b, password,
                                      0, test_random, &random) != 0);
    assert(rt_wlan_offload_sae_prepare(&sae, address_a, address_b, password,
                                      1, test_random, &random) == 0);
    rt_wlan_offload_sae_clear(&sae);
    assert(rt_wlan_offload_sae_prepare(
               &sae, address_a, address_b, password,
               RT_WLAN_OFFLOAD_SAE_MAX_PASSWORD_LENGTH,
               test_random, &random) == 0);
    rt_wlan_offload_sae_clear(&sae);
    assert(rt_wlan_offload_sae_prepare(
               &sae, address_a, address_b, password, sizeof(password),
               test_random, &random) != 0);
}

int main(void)
{
    static const unsigned char address_a[6] = {0x02,0,0,0,0,1};
    static const unsigned char address_b[6] = {0x02,0,0,0,0,2};
    static const unsigned char password[] = "correct horse battery staple";
    struct test_random random_a = {1};
    struct test_random random_b = {2};
    struct rt_wlan_offload_sae a;
    struct rt_wlan_offload_sae b;
    unsigned char commit_a[RT_WLAN_OFFLOAD_SAE_COMMIT_LENGTH];
    unsigned char commit_b[RT_WLAN_OFFLOAD_SAE_COMMIT_LENGTH];
    unsigned char confirm_a[RT_WLAN_OFFLOAD_SAE_CONFIRM_LENGTH];
    unsigned char confirm_b[RT_WLAN_OFFLOAD_SAE_CONFIRM_LENGTH];

    test_ieee_annex_j10();
    test_password_lengths();
    assert(rt_wlan_offload_sae_prepare(&a, address_a, address_b, password,
                                  sizeof(password) - 1,
                                  test_random, &random_a) == 0);
    assert(rt_wlan_offload_sae_prepare(&b, address_b, address_a, password,
                                  sizeof(password) - 1,
                                  test_random, &random_b) == 0);
    assert(rt_wlan_offload_sae_write_commit(&a, commit_a) == 0);
    assert(rt_wlan_offload_sae_write_commit(&b, commit_b) == 0);
    assert(rt_wlan_offload_sae_process_commit(&a, commit_b, sizeof(commit_b)) == 0);
    assert(rt_wlan_offload_sae_process_commit(&b, commit_a, sizeof(commit_a)) == 0);
    assert(memcmp(a.pmk, b.pmk, sizeof(a.pmk)) == 0);
    assert(memcmp(a.pmkid, b.pmkid, sizeof(a.pmkid)) == 0);
    assert(rt_wlan_offload_sae_write_confirm(&a, confirm_a) == 0);
    assert(rt_wlan_offload_sae_write_confirm(&b, confirm_b) == 0);
    assert(rt_wlan_offload_sae_check_confirm(&a, confirm_b,
                                        sizeof(confirm_b)) == 0);
    assert(rt_wlan_offload_sae_check_confirm(&b, confirm_a,
                                        sizeof(confirm_a)) == 0);
    rt_wlan_offload_sae_clear(&a);
    rt_wlan_offload_sae_clear(&b);
    puts("wlan_offload SAE tests passed");
    return 0;
}
