/* AES-GCM for mbedtls (MBEDTLS_GCM_ALT; NIST SP 800-38D), replacing mbedtls/library/gcm.c for issue #3.
 *
 * mbedtls's GHASH keeps the state in 64-bit integers; on this 32-bit core that cost ~1600 cycles per 16-byte block,
 * twice the AES itself, and the AA video stream decrypts every byte of it. Here GHASH is Shoup's method with an 8-bit
 * table in 32-bit words: per byte, a 4-word shift, one reduction lookup and a 4-word table XOR, all in registers.
 * M[b] = b * H (4 KB per key), R8[r] = the reduction of the byte shifted out (1 KB, shared).
 *
 * The data cache is 8 KB, 2-way: M fills one way, so this pairs with MBEDTLS_AES_FEWER_TABLES (1 KB of AES tables
 * instead of 4). On the unit, per 16 KB record: mbedtls's GCM 8.4 ms, this with full AES tables 5.3-5.8 ms, with the
 * small ones 3.6 ms; a 4-bit GHASH table (256 B) was slower, 9 ms. AA video over AES-128-GCM: 37.9 -> 44.0 fps.
 *
 * Bit order is GCM's: byte 0 bit 7 is x^0, and multiplying by x shifts the block right as a 128-bit big-endian
 * number, folding the bit that falls off back in with 0xE1 << 120. Same API and error codes as mbedtls's gcm.c.
 * Host-tested against Python's `cryptography` (test/test_gcm.py). */
#include <string.h>
#include "mbedtls/gcm.h"
#include "mbedtls/error.h"
#include "mbedtls/platform_util.h"

typedef uint32_t u32;

static u32 R8[256];   /* R8[r] = the reduction of byte r shifted out of a block */

static void mulx(u32 v[4])   /* v = v * x */
{
    u32 lsb = v[3] & 1;
    v[3] = v[3] >> 1 | v[2] << 31;
    v[2] = v[2] >> 1 | v[1] << 31;
    v[1] = v[1] >> 1 | v[0] << 31;
    v[0] = v[0] >> 1 ^ (lsb ? 0xE1000000u : 0);
}

static u32 be32(const unsigned char *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put_be32(unsigned char *p, u32 v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LOADW(p) __builtin_bswap32(*(const u32 *)(p))
#define STOREW(p, v) (*(u32 *)(p) = __builtin_bswap32(v))
#else
#define LOADW(p) (*(const u32 *)(p))
#define STOREW(p, v) (*(u32 *)(p) = (v))
#endif

/* y = y * H, Horner over the block from its last byte (highest degree) to its first */
static void gmul(const u32 M[256][4], u32 y[4])
{
    const u32 *m;
    u32 z0, z1, z2, z3, r, a = y[0], b = y[1], c = y[2], d = y[3];
#define LOAD(x) (m = M[x], z0 = m[0], z1 = m[1], z2 = m[2], z3 = m[3])
#define STEP(x)                                                                                                     \
    (r = R8[z3 & 0xff], z3 = z3 >> 8 | z2 << 24, z2 = z2 >> 8 | z1 << 24, z1 = z1 >> 8 | z0 << 24, z0 = z0 >> 8 ^ r, \
     m = M[x], z0 ^= m[0], z1 ^= m[1], z2 ^= m[2], z3 ^= m[3])
    LOAD(d & 0xff);
    STEP(d >> 8 & 0xff), STEP(d >> 16 & 0xff), STEP(d >> 24);
    STEP(c & 0xff), STEP(c >> 8 & 0xff), STEP(c >> 16 & 0xff), STEP(c >> 24);
    STEP(b & 0xff), STEP(b >> 8 & 0xff), STEP(b >> 16 & 0xff), STEP(b >> 24);
    STEP(a & 0xff), STEP(a >> 8 & 0xff), STEP(a >> 16 & 0xff), STEP(a >> 24);
#undef LOAD
#undef STEP
    y[0] = z0, y[1] = z1, y[2] = z2, y[3] = z3;
}

static void xor_byte(u32 y[4], unsigned pos, unsigned char v) { y[pos >> 2] ^= (u32)v << (24 - 8 * (pos & 3)); }

static void ghash_block(mbedtls_gcm_context *ctx, const unsigned char *p)
{
    ctx->y[0] ^= be32(p), ctx->y[1] ^= be32(p + 4), ctx->y[2] ^= be32(p + 8), ctx->y[3] ^= be32(p + 12);
    gmul((const u32(*)[4])ctx->M, ctx->y);
}

static void next_keystream(mbedtls_gcm_context *ctx)   /* the software AES never fails once keyed */
{
    put_be32(ctx->ctr + 12, be32(ctx->ctr + 12) + 1);
    (void)!mbedtls_internal_aes_encrypt(&ctx->aes, ctx->ctr, ctx->ectr);
}

void mbedtls_gcm_init(mbedtls_gcm_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    mbedtls_aes_init(&ctx->aes);
}

int mbedtls_gcm_setkey(mbedtls_gcm_context *ctx, mbedtls_cipher_id_t cipher, const unsigned char *key,
                       unsigned int keybits)
{
    if (cipher != MBEDTLS_CIPHER_ID_AES || (keybits != 128 && keybits != 192 && keybits != 256))
        return MBEDTLS_ERR_GCM_BAD_INPUT;
    int ret = mbedtls_aes_setkey_enc(&ctx->aes, key, keybits);
    if (ret)
        return ret;
    if (!R8[1]) {
        for (unsigned r = 0; r < 256; r++) {
            u32 v[4] = { 0, 0, 0, r };
            for (int i = 0; i < 8; i++)
                mulx(v);
            R8[r] = v[0];   /* v[1..3] are 0: the shifted-out byte folds back only into the top 16 bits */
        }
    }
    unsigned char h[16] = { 0 };
    if ((ret = mbedtls_internal_aes_encrypt(&ctx->aes, h, h)) != 0)
        return ret;
    u32 v[4] = { be32(h), be32(h + 4), be32(h + 8), be32(h + 12) };
    for (unsigned i = 0x80; i; i >>= 1) {
        memcpy(ctx->M[i], v, 16);
        mulx(v);
    }
    memset(ctx->M[0], 0, 16);
    for (unsigned i = 2; i < 256; i <<= 1)
        for (unsigned j = 1; j < i; j++)
            for (int k = 0; k < 4; k++)
                ctx->M[i + j][k] = ctx->M[i][k] ^ ctx->M[j][k];
    mbedtls_platform_zeroize(h, sizeof h);
    return 0;
}

int mbedtls_gcm_starts(mbedtls_gcm_context *ctx, int mode, const unsigned char *iv, size_t iv_len)
{
    if (iv_len == 0 || (uint64_t)iv_len >> 61)
        return MBEDTLS_ERR_GCM_BAD_INPUT;
    memset(ctx->y, 0, sizeof ctx->y);
    ctx->len = ctx->add_len = 0;
    ctx->mode = mode;
    if (iv_len == 12) {
        memcpy(ctx->ctr, iv, 12);
        put_be32(ctx->ctr + 12, 1);
    } else {   /* J0 = GHASH(IV, zero-padded, then 0^64 || bit length of IV) */
        unsigned char blk[16];
        size_t n = iv_len;
        for (; n >= 16; n -= 16, iv += 16)
            ghash_block(ctx, iv);
        if (n) {
            memset(blk, 0, 16);
            memcpy(blk, iv, n);
            ghash_block(ctx, blk);
        }
        memset(blk, 0, 8);
        put_be32(blk + 8, (u32)((uint64_t)iv_len >> 29));
        put_be32(blk + 12, (u32)(iv_len << 3));
        ghash_block(ctx, blk);
        for (int k = 0; k < 4; k++)
            put_be32(ctx->ctr + 4 * k, ctx->y[k]);
        memset(ctx->y, 0, sizeof ctx->y);
    }
    return mbedtls_internal_aes_encrypt(&ctx->aes, ctx->ctr, ctx->base_ectr);
}

int mbedtls_gcm_update_ad(mbedtls_gcm_context *ctx, const unsigned char *add, size_t add_len)
{
    if (ctx->len || (uint64_t)add_len >> 61 || ctx->add_len + add_len < ctx->add_len)
        return MBEDTLS_ERR_GCM_BAD_INPUT;
    unsigned pos = (unsigned)(ctx->add_len % 16);
    ctx->add_len += add_len;
    while (add_len) {
        if (pos == 0 && add_len >= 16) {
            ghash_block(ctx, add);
            add += 16, add_len -= 16;
            continue;
        }
        xor_byte(ctx->y, pos, *add++);
        add_len--;
        if (++pos == 16) {
            gmul((const u32(*)[4])ctx->M, ctx->y);
            pos = 0;
        }
    }
    return 0;
}

int mbedtls_gcm_update(mbedtls_gcm_context *ctx, const unsigned char *input, size_t input_length,
                       unsigned char *output, size_t output_size, size_t *output_length)
{
    if (output_size < input_length)
        return MBEDTLS_ERR_GCM_BUFFER_TOO_SMALL;
    *output_length = input_length;
    if (input_length == 0)   /* nothing to close yet: finish() pads a partial AD block when no data follows */
        return 0;
    if (ctx->len + input_length < ctx->len || ctx->len + input_length > 0xFFFFFFFE0ull)
        return MBEDTLS_ERR_GCM_BAD_INPUT;
    if (ctx->len == 0 && ctx->add_len % 16)   /* close the partial AD block */
        gmul((const u32(*)[4])ctx->M, ctx->y);
    const int dec = ctx->mode == MBEDTLS_GCM_DECRYPT;
    unsigned pos = (unsigned)(ctx->len % 16);
    ctx->len += input_length;
    while (input_length && pos) {   /* finish a partial block */
        unsigned char c = *input++ ^ ctx->ectr[pos];
        xor_byte(ctx->y, pos, dec ? (unsigned char)(c ^ ctx->ectr[pos]) : c);
        *output++ = c;
        input_length--;
        if (++pos == 16) {
            gmul((const u32(*)[4])ctx->M, ctx->y);
            pos = 0;
        }
    }
    const int aligned = !(((uintptr_t)input | (uintptr_t)output) & 3);
    u32 *y = ctx->y;
    for (; input_length >= 16; input_length -= 16, input += 16, output += 16) {
        next_keystream(ctx);
        u32 k0 = be32(ctx->ectr), k1 = be32(ctx->ectr + 4), k2 = be32(ctx->ectr + 8), k3 = be32(ctx->ectr + 12);
        u32 i0, i1, i2, i3;
        if (aligned)
            i0 = LOADW(input), i1 = LOADW(input + 4), i2 = LOADW(input + 8), i3 = LOADW(input + 12);
        else
            i0 = be32(input), i1 = be32(input + 4), i2 = be32(input + 8), i3 = be32(input + 12);
        u32 o0 = i0 ^ k0, o1 = i1 ^ k1, o2 = i2 ^ k2, o3 = i3 ^ k3;
        if (dec)
            y[0] ^= i0, y[1] ^= i1, y[2] ^= i2, y[3] ^= i3;
        else
            y[0] ^= o0, y[1] ^= o1, y[2] ^= o2, y[3] ^= o3;
        if (aligned)
            STOREW(output, o0), STOREW(output + 4, o1), STOREW(output + 8, o2), STOREW(output + 12, o3);
        else
            put_be32(output, o0), put_be32(output + 4, o1), put_be32(output + 8, o2), put_be32(output + 12, o3);
        gmul((const u32(*)[4])ctx->M, y);
    }
    if (input_length) {   /* start a partial block; its GHASH multiply waits for more data or finish */
        next_keystream(ctx);
        for (; pos < input_length; pos++) {
            unsigned char c = input[pos] ^ ctx->ectr[pos];
            xor_byte(y, pos, dec ? input[pos] : c);
            output[pos] = c;
        }
    }
    return 0;
}

int mbedtls_gcm_finish(mbedtls_gcm_context *ctx, unsigned char *output, size_t output_size, size_t *output_length,
                       unsigned char *tag, size_t tag_len)
{
    (void)output, (void)output_size;
    *output_length = 0;
    if (tag_len > 16 || tag_len < 4)
        return MBEDTLS_ERR_GCM_BAD_INPUT;
    if ((ctx->len == 0 && ctx->add_len % 16) || ctx->len % 16)
        gmul((const u32(*)[4])ctx->M, ctx->y);
    uint64_t abits = ctx->add_len << 3, cbits = ctx->len << 3;
    ctx->y[0] ^= (u32)(abits >> 32), ctx->y[1] ^= (u32)abits, ctx->y[2] ^= (u32)(cbits >> 32), ctx->y[3] ^= (u32)cbits;
    gmul((const u32(*)[4])ctx->M, ctx->y);
    unsigned char s[16];
    for (int k = 0; k < 4; k++)
        put_be32(s + 4 * k, ctx->y[k]);
    for (size_t i = 0; i < tag_len; i++)
        tag[i] = s[i] ^ ctx->base_ectr[i];
    mbedtls_platform_zeroize(s, sizeof s);
    return 0;
}

int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx, int mode, size_t length, const unsigned char *iv,
                              size_t iv_len, const unsigned char *add, size_t add_len, const unsigned char *input,
                              unsigned char *output, size_t tag_len, unsigned char *tag)
{
    size_t olen;
    int ret;
    if ((ret = mbedtls_gcm_starts(ctx, mode, iv, iv_len)) != 0 || (ret = mbedtls_gcm_update_ad(ctx, add, add_len)) != 0 ||
        (ret = mbedtls_gcm_update(ctx, input, length, output, length, &olen)) != 0)
        return ret;
    return mbedtls_gcm_finish(ctx, NULL, 0, &olen, tag, tag_len);
}

int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *ctx, size_t length, const unsigned char *iv, size_t iv_len,
                             const unsigned char *add, size_t add_len, const unsigned char *tag, size_t tag_len,
                             const unsigned char *input, unsigned char *output)
{
    unsigned char check[16];
    int ret = mbedtls_gcm_crypt_and_tag(ctx, MBEDTLS_GCM_DECRYPT, length, iv, iv_len, add, add_len, input, output,
                                        tag_len, check);
    if (ret)
        return ret;
    unsigned char diff = 0;
    for (size_t i = 0; i < tag_len; i++)
        diff |= tag[i] ^ check[i];
    mbedtls_platform_zeroize(check, sizeof check);
    if (diff) {
        mbedtls_platform_zeroize(output, length);
        return MBEDTLS_ERR_GCM_AUTH_FAILED;
    }
    return 0;
}

void mbedtls_gcm_free(mbedtls_gcm_context *ctx)
{
    if (!ctx)
        return;
    mbedtls_aes_free(&ctx->aes);
    mbedtls_platform_zeroize(ctx, sizeof(*ctx));
}
