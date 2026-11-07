/* AES-GCM for mbedtls (MBEDTLS_GCM_ALT; NIST SP 800-38D, FIPS 197), replacing mbedtls/library/gcm.c for issue #3.
 *
 * GHASH: mbedtls keeps it in 64-bit integers, ~1600 cycles per 16-byte block on this 32-bit core. Here it is Shoup's
 * method with an 8-bit table in 32-bit words: per byte, a 4-word shift, one reduction lookup and a 4-word table XOR,
 * all in registers. M[b] = b * H (4 KB per key, in the context), R8[r] = the reduction of the byte shifted out.
 *
 * AES: its own encrypt-only T-table code instead of mbedtls's, for the cache. The data cache is 8 KB, 2-way, 32-byte
 * lines: M covers every set once, so every other hot table has to avoid the others. With mbedtls's AES tables and a
 * separate R8 the linker decided, and in one build FT0 and R8 shared 27 sets (three hot lines in two ways): the same
 * code ran 3.6 to 6.6 ms per 16 KB record depending on layout. Here the one 1 KB T-table (the S-box is read out of
 * it) and R8 (16-bit) sit together in `hot`, 1.5 KB, so they never collide with each other. The counter, keystream
 * and GHASH state stay in 32-bit words with no per-block function call or byte conversion.
 *
 * Bit order is GCM's: byte 0 bit 7 is x^0, and multiplying by x shifts the block right as a 128-bit big-endian
 * number, folding the bit that falls off back in with 0xE1 << 120. Same API and error codes as mbedtls's gcm.c.
 * Host-tested against Python's `cryptography` (test/test_gcm.py). */
#include <string.h>
#include "mbedtls/gcm.h"
#include "mbedtls/error.h"
#include "mbedtls/platform_util.h"

typedef uint32_t u32;

static struct {
    u32 T[256];           /* T[x] = S[x] * (02, 01, 01, 03), big-endian; byte 1 and byte 2 are S[x] itself */
    uint16_t R8[256];     /* R8[r] << 16 = the reduction of byte r shifted out of a GHASH block */
} hot __attribute__((aligned(32)));

#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void tables_init(void)
{
    if (hot.T[0])
        return;
    unsigned char s[256];
    unsigned p = 1, q = 1;   /* p runs through the powers of 3, q through those of its inverse */
    do {
        p = (p ^ p << 1 ^ (p & 0x80 ? 0x1b : 0)) & 0xff;
        q ^= q << 1, q ^= q << 2, q ^= q << 4, q &= 0xff;
        if (q & 0x80)
            q ^= 0x09;
        unsigned x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4);
        s[p] = (unsigned char)((x ^ 0x63) & 0xff);
    } while (p != 1);
    s[0] = 0x63;
    for (unsigned x = 0; x < 256; x++) {
        u32 v = s[x], v2 = (v << 1 ^ (v & 0x80 ? 0x1b : 0)) & 0xff;
        hot.T[x] = v2 << 24 | v << 16 | v << 8 | (v2 ^ v);
    }
    for (unsigned r = 0; r < 256; r++) {
        u32 v[4] = { 0, 0, 0, r };
        for (int i = 0; i < 8; i++) {   /* v = v * x */
            u32 lsb = v[3] & 1;
            v[3] = v[3] >> 1 | v[2] << 31, v[2] = v[2] >> 1 | v[1] << 31, v[1] = v[1] >> 1 | v[0] << 31;
            v[0] = v[0] >> 1 ^ (lsb ? 0xE1000000u : 0);
        }
        hot.R8[r] = (uint16_t)(v[0] >> 16);   /* v[1..3] and the low half of v[0] are 0 */
    }
}

#define SB(x) (hot.T[x] >> 16 & 0xff)

static void aes_setkey(mbedtls_gcm_context *ctx, const unsigned char *key, unsigned bits)
{
    unsigned nk = bits / 32, n = 4 * (nk + 7);
    u32 *w = ctx->rk, rc = 1;
    for (unsigned i = 0; i < nk; i++)
        w[i] = (u32)key[4 * i] << 24 | (u32)key[4 * i + 1] << 16 | (u32)key[4 * i + 2] << 8 | key[4 * i + 3];
    for (unsigned i = nk; i < n; i++) {
        u32 t = w[i - 1];
        if (i % nk == 0) {
            t = SB(t >> 16 & 0xff) << 24 | SB(t >> 8 & 0xff) << 16 | SB(t & 0xff) << 8 | SB(t >> 24);
            t ^= rc << 24;
            rc = (rc << 1 ^ (rc & 0x80 ? 0x1b : 0)) & 0xff;
        } else if (nk > 6 && i % nk == 4) {
            t = SB(t >> 24) << 24 | SB(t >> 16 & 0xff) << 16 | SB(t >> 8 & 0xff) << 8 | SB(t & 0xff);
        }
        w[i] = w[i - nk] ^ t;
    }
    ctx->nr = (int)nk + 6;
}

/* one block, big-endian words in and out */
static inline __attribute__((always_inline)) void aes_enc(const mbedtls_gcm_context *ctx, const u32 in[4], u32 out[4])
{
    const u32 *rk = ctx->rk, *T = hot.T;
    u32 s0 = in[0] ^ rk[0], s1 = in[1] ^ rk[1], s2 = in[2] ^ rk[2], s3 = in[3] ^ rk[3], t0, t1, t2, t3;
#define ROUND(a, b, c, d, k)                                                                                   \
    (T[a >> 24] ^ ROR(T[b >> 16 & 0xff], 8) ^ ROR(T[c >> 8 & 0xff], 16) ^ ROR(T[d & 0xff], 24) ^ (k))
    for (int r = 1; r < ctx->nr; r++) {
        rk += 4;
        t0 = ROUND(s0, s1, s2, s3, rk[0]);
        t1 = ROUND(s1, s2, s3, s0, rk[1]);
        t2 = ROUND(s2, s3, s0, s1, rk[2]);
        t3 = ROUND(s3, s0, s1, s2, rk[3]);
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
#undef ROUND
    rk += 4;
#define LAST(a, b, c, d, k)                                                                                    \
    ((T[a >> 24] << 8 & 0xff000000u) ^ (T[b >> 16 & 0xff] & 0x00ff0000u) ^ (T[c >> 8 & 0xff] & 0x0000ff00u) ^ \
     (T[d & 0xff] >> 16 & 0xffu) ^ (k))
    out[0] = LAST(s0, s1, s2, s3, rk[0]);
    out[1] = LAST(s1, s2, s3, s0, rk[1]);
    out[2] = LAST(s2, s3, s0, s1, rk[2]);
    out[3] = LAST(s3, s0, s1, s2, rk[3]);
#undef LAST
}

static u32 be32(const unsigned char *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put_be32(unsigned char *p, u32 v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static unsigned char word_byte(const u32 w[4], unsigned pos) { return (unsigned char)(w[pos >> 2] >> (24 - 8 * (pos & 3))); }
static void xor_byte(u32 y[4], unsigned pos, unsigned char v) { y[pos >> 2] ^= (u32)v << (24 - 8 * (pos & 3)); }

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LOADW(p) __builtin_bswap32(*(const u32 *)(p))
#define STOREW(p, v) (*(u32 *)(p) = __builtin_bswap32(v))
#else
#define LOADW(p) (*(const u32 *)(p))
#define STOREW(p, v) (*(u32 *)(p) = (v))
#endif

/* y = y * H, Horner over the block from its last byte (highest degree) to its first */
static inline __attribute__((always_inline)) void gmul(const u32 M[256][4], u32 y[4])
{
    const u32 *m;
    const uint16_t *R8 = hot.R8;
    u32 z0, z1, z2, z3, r, a = y[0], b = y[1], c = y[2], d = y[3];
#define LOAD(x) (m = M[x], z0 = m[0], z1 = m[1], z2 = m[2], z3 = m[3])
#define STEP(x)                                                                                             \
    (r = (u32)R8[z3 & 0xff] << 16, z3 = z3 >> 8 | z2 << 24, z2 = z2 >> 8 | z1 << 24, z1 = z1 >> 8 | z0 << 24, \
     z0 = z0 >> 8 ^ r, m = M[x], z0 ^= m[0], z1 ^= m[1], z2 ^= m[2], z3 ^= m[3])
    LOAD(d & 0xff);
    STEP(d >> 8 & 0xff), STEP(d >> 16 & 0xff), STEP(d >> 24);
    STEP(c & 0xff), STEP(c >> 8 & 0xff), STEP(c >> 16 & 0xff), STEP(c >> 24);
    STEP(b & 0xff), STEP(b >> 8 & 0xff), STEP(b >> 16 & 0xff), STEP(b >> 24);
    STEP(a & 0xff), STEP(a >> 8 & 0xff), STEP(a >> 16 & 0xff), STEP(a >> 24);
#undef LOAD
#undef STEP
    y[0] = z0, y[1] = z1, y[2] = z2, y[3] = z3;
}

static void ghash_mul(mbedtls_gcm_context *ctx) { gmul((const u32(*)[4])ctx->M, ctx->y); }

static void ghash_block(mbedtls_gcm_context *ctx, const unsigned char *p)
{
    ctx->y[0] ^= be32(p), ctx->y[1] ^= be32(p + 4), ctx->y[2] ^= be32(p + 8), ctx->y[3] ^= be32(p + 12);
    ghash_mul(ctx);
}

void mbedtls_gcm_init(mbedtls_gcm_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }

int mbedtls_gcm_setkey(mbedtls_gcm_context *ctx, mbedtls_cipher_id_t cipher, const unsigned char *key,
                       unsigned int keybits)
{
    if (cipher != MBEDTLS_CIPHER_ID_AES || (keybits != 128 && keybits != 192 && keybits != 256))
        return MBEDTLS_ERR_GCM_BAD_INPUT;
    tables_init();
    aes_setkey(ctx, key, keybits);
    u32 zero[4] = { 0 }, v[4];
    aes_enc(ctx, zero, v);   /* H */
    for (unsigned i = 0x80; i; i >>= 1) {
        memcpy(ctx->M[i], v, 16);
        u32 lsb = v[3] & 1;   /* v = v * x */
        v[3] = v[3] >> 1 | v[2] << 31, v[2] = v[2] >> 1 | v[1] << 31, v[1] = v[1] >> 1 | v[0] << 31;
        v[0] = v[0] >> 1 ^ (lsb ? 0xE1000000u : 0);
    }
    memset(ctx->M[0], 0, 16);
    for (unsigned i = 2; i < 256; i <<= 1)
        for (unsigned j = 1; j < i; j++)
            for (int k = 0; k < 4; k++)
                ctx->M[i + j][k] = ctx->M[i][k] ^ ctx->M[j][k];
    mbedtls_platform_zeroize(v, sizeof v);
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
        ctx->ctr[0] = be32(iv), ctx->ctr[1] = be32(iv + 4), ctx->ctr[2] = be32(iv + 8), ctx->ctr[3] = 1;
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
        memcpy(ctx->ctr, ctx->y, 16);
        memset(ctx->y, 0, sizeof ctx->y);
    }
    aes_enc(ctx, ctx->ctr, ctx->base_ek);
    return 0;
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
            ghash_mul(ctx);
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
        ghash_mul(ctx);
    const int dec = ctx->mode == MBEDTLS_GCM_DECRYPT;
    unsigned pos = (unsigned)(ctx->len % 16);
    ctx->len += input_length;
    while (input_length && pos) {   /* finish a partial block */
        unsigned char in = *input++, c = in ^ word_byte(ctx->ek, pos);
        xor_byte(ctx->y, pos, dec ? in : c);
        *output++ = c;
        input_length--;
        if (++pos == 16) {
            ghash_mul(ctx);
            pos = 0;
        }
    }
    const int aligned = !(((uintptr_t)input | (uintptr_t)output) & 3);
    const u32(*M)[4] = (const u32(*)[4])ctx->M;
    u32 y[4] = { ctx->y[0], ctx->y[1], ctx->y[2], ctx->y[3] }, k[4];
    for (; input_length >= 16; input_length -= 16, input += 16, output += 16) {
        ctx->ctr[3]++;
        aes_enc(ctx, ctx->ctr, k);
        u32 i0, i1, i2, i3;
        if (aligned)
            i0 = LOADW(input), i1 = LOADW(input + 4), i2 = LOADW(input + 8), i3 = LOADW(input + 12);
        else
            i0 = be32(input), i1 = be32(input + 4), i2 = be32(input + 8), i3 = be32(input + 12);
        u32 o0 = i0 ^ k[0], o1 = i1 ^ k[1], o2 = i2 ^ k[2], o3 = i3 ^ k[3];
        if (dec)
            y[0] ^= i0, y[1] ^= i1, y[2] ^= i2, y[3] ^= i3;
        else
            y[0] ^= o0, y[1] ^= o1, y[2] ^= o2, y[3] ^= o3;
        if (aligned)
            STOREW(output, o0), STOREW(output + 4, o1), STOREW(output + 8, o2), STOREW(output + 12, o3);
        else
            put_be32(output, o0), put_be32(output + 4, o1), put_be32(output + 8, o2), put_be32(output + 12, o3);
        gmul(M, y);
    }
    memcpy(ctx->y, y, 16);
    if (input_length) {   /* start a partial block; its GHASH multiply waits for more data or finish */
        ctx->ctr[3]++;
        aes_enc(ctx, ctx->ctr, ctx->ek);
        for (; pos < input_length; pos++) {
            unsigned char c = input[pos] ^ word_byte(ctx->ek, pos);
            xor_byte(ctx->y, pos, dec ? input[pos] : c);
            output[pos] = c;
        }
    }
    mbedtls_platform_zeroize(k, sizeof k);
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
        ghash_mul(ctx);
    uint64_t abits = ctx->add_len << 3, cbits = ctx->len << 3;
    ctx->y[0] ^= (u32)(abits >> 32), ctx->y[1] ^= (u32)abits, ctx->y[2] ^= (u32)(cbits >> 32), ctx->y[3] ^= (u32)cbits;
    ghash_mul(ctx);
    for (size_t i = 0; i < tag_len; i++)
        tag[i] = word_byte(ctx->y, (unsigned)i) ^ word_byte(ctx->base_ek, (unsigned)i);
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
    if (ctx)
        mbedtls_platform_zeroize(ctx, sizeof(*ctx));
}
