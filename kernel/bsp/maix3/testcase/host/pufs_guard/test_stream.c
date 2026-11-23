/* Exercise production CCM buffering with observable CTR output and MAC input. */
#include <assert.h>
#include "pufs_sp38c.c"

static struct pufs_sp38c_regs regs;
static const uint8_t *src;
static uint8_t *dst, authenticated[128];
static size_t length, used;

bool dma_check_busy_status(uint32_t *status) { return false; }
void dma_write_config_0(bool rng, bool sg, bool nocrypto) {}
void dma_write_data_block_config(bool h, bool t, bool irq, bool pause, uint32_t off) {}
pufs_status_t crypto_write_iv(uint8_t *iv, size_t n) { return SUCCESS; }
pufs_status_t crypto_write_dgst(uint8_t *d, size_t n) { return SUCCESS; }
void crypto_read_dgest(uint8_t *out, size_t n) { memset(out, 0, n); }

pufs_status_t dma_write_rwcfg(const uint8_t *out, const uint8_t *in, uint32_t n)
{
    src = in;
    dst = (uint8_t *)out;
    length = n;
    return SUCCESS;
}

pufs_status_t dma_run(void)
{
    if (dst) {
        for (size_t i = 0; i < length; i++)
            dst[i] = src[i] ^ 0x80;
    } else {
        assert(used + length <= sizeof(authenticated));
        memcpy(authenticated + used, src, length);
        used += length;
    }
    return SUCCESS;
}

static void check_split(bool encrypt, unsigned size, unsigned split)
{
    uint8_t input[51], out[52], mac[64] = {0};
    pufs_sp38c_ctx ctx = {.cipher = AES, .keybits = 128, .encrypt = encrypt, .inlen = size};
    uint32_t n, m;
    used = 0;
    memset(out, 0x55, sizeof(out));
    for (unsigned i = 0; i < size; i++) {
        input[i] = i ^ (encrypt ? 0 : 0x80);
        mac[i] = i;
    }
    assert(pufs_ccm_text_update(&ctx, out, &n, input, split) == SUCCESS && n <= size);
    assert(pufs_ccm_text_update(&ctx, out + n, &m, input + split, size - split) == SUCCESS);
    assert(n + m == size && ctx.buflen == 0 && out[size] == 0x55);
    for (unsigned i = 0; i < size; i++)
        assert(out[i] == (uint8_t)(input[i] ^ 0x80));
    assert(used == ((size + 15) & ~15u) && memcmp(authenticated, mac, used) == 0);
}

int main(void)
{
    sp38c_regs = &regs;
    for (unsigned size = 1; size <= 51; size++)
        for (unsigned split = 0; split <= size; split++) {
            check_split(false, size, split);
            check_split(true, size, split);
        }
    uint8_t aad[46], expected[48] = {0, sizeof(aad)};
    for (unsigned i = 0; i < sizeof(aad); i++)
        expected[2 + i] = aad[i] = i;
    for (unsigned split = 1; split <= sizeof(aad); split++) {
        pufs_sp38c_ctx ctx = {.cipher = AES, .keybits = 128, .aadlen = sizeof(aad)};
        uint32_t n = 1;
        used = 0;
        assert(pufs_ccm_aad_update(&ctx, aad, split) == SUCCESS);
        assert(pufs_ccm_aad_update(&ctx, aad + split, sizeof(aad) - split) == SUCCESS);
        assert(pufs_ccm_text_final(&ctx, NULL, &n) == SUCCESS && n == 0);
        assert(used == sizeof(expected) && memcmp(authenticated, expected, used) == 0);
    }
    /* Before the declared AAD is complete (stage NONE), text and final are
     * refused: the buffer holds a partial B1 block, not text. */
    for (unsigned given = 0; given < sizeof(aad); given++) {
        pufs_sp38c_ctx ctx = {.cipher = AES, .keybits = 128, .aadlen = sizeof(aad), .inlen = 16};
        uint8_t text[16] = {0}, out[32];
        uint32_t n;
        used = 0;
        if (given)
            assert(pufs_ccm_aad_update(&ctx, aad, given) == SUCCESS);
        if (ctx.stage != SP38C_NONE)
            continue;
        pufs_sp38c_ctx copy = ctx;
        assert(pufs_ccm_text_update(&copy, out, &n, text, sizeof(text)) == E_INVALID);
        copy = ctx;
        assert(pufs_ccm_text_final(&copy, out, &n) == E_INVALID);
        copy = ctx;
        assert(pufs_ccm_text_final(&copy, NULL, &n) == E_INVALID);
        assert(used == 0);
    }
    /* Past the first block, AAD is still counted: too little or too much is
     * refused, and so is AAD when none was declared. */
    {
        pufs_sp38c_ctx ctx = {.cipher = AES, .keybits = 128, .aadlen = 20, .inlen = 16};
        uint8_t text[16] = {0}, out[32];
        uint32_t n;
        used = 0;
        assert(pufs_ccm_aad_update(&ctx, aad, 16) == SUCCESS && ctx.stage == SP38C_AAD);
        pufs_sp38c_ctx copy = ctx;
        assert(pufs_ccm_text_update(&copy, out, &n, text, sizeof(text)) == E_INVALID);
        copy = ctx;
        assert(pufs_ccm_text_final(&copy, out, &n) == E_INVALID);
        copy = ctx;
        assert(pufs_ccm_aad_update(&copy, aad + 16, 5) == E_INVALID);
        assert(pufs_ccm_aad_update(&ctx, aad + 16, 4) == SUCCESS && ctx.aadlen == 0);
        assert(pufs_ccm_text_update(&ctx, out, &n, text, sizeof(text)) == SUCCESS);
    }
    {
        pufs_sp38c_ctx ctx = {.cipher = AES, .keybits = 128, .aadlen = 0, .inlen = 16};
        assert(pufs_ccm_aad_update(&ctx, aad, 1) == E_INVALID);
        assert(pufs_ccm_aad_update(&ctx, aad, 0) == SUCCESS);
    }
    /* A forged stage-NONE context with a buffered byte and no AAD declared. */
    {
        pufs_sp38c_ctx ctx = {.cipher = AES, .keybits = 128, .inlen = 16, .buflen = 1};
        uint8_t out[32];
        uint32_t n;
        assert(pufs_ccm_text_final(&ctx, out, &n) == E_INVALID);
    }
    /* A buffered partial text block needs an output buffer at final. */
    {
        pufs_sp38c_ctx ctx = {.cipher = AES, .keybits = 128, .inlen = 20};
        uint8_t text[20] = {0}, out[32];
        uint32_t n;
        assert(pufs_ccm_text_update(&ctx, out, &n, text, 5) == SUCCESS);
        assert(ctx.stage == SP38C_TEXT && ctx.buflen == 5);
        assert(pufs_ccm_text_final(&ctx, NULL, &n) == E_INVALID);
        assert(pufs_ccm_text_final(&ctx, out, &n) == SUCCESS && n == 5);
    }
    puts("CCM split encryption, decryption, and authenticated input: PASS");
    puts("CCM incomplete AAD and NULL final output refused: PASS");
    return 0;
}
