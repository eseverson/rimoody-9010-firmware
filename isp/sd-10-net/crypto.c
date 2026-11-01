/* See crypto.h. Written from the specs (FIPS 180-1, RFC 2104, RFC 2898, IEEE 802.11-2016 12.7.1.2 / 12.5.3, FIPS 197,
 * RFC 3394, RFC 3610); verified on the host by test_crypto.py against published vectors and Python's own crypto. */
#include "crypto.h"

/* ---------------- SHA-1 ---------------- */
static cu32 rol(cu32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(cu32 h[5], const cu8 *p)
{
    cu32 w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++)
        w[i] = (cu32)p[4 * i] << 24 | (cu32)p[4 * i + 1] << 16 | (cu32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        cu32 f, k;
        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5a827999;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else
            f = b ^ c ^ d, k = 0xca62c1d6;
        cu32 t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

void sha1_init(struct sha1_ctx *c)
{
    c->h[0] = 0x67452301, c->h[1] = 0xefcdab89, c->h[2] = 0x98badcfe, c->h[3] = 0x10325476, c->h[4] = 0xc3d2e1f0;
    c->len = 0;
    c->n = 0;
}

void sha1_update(struct sha1_ctx *c, const cu8 *p, int n)
{
    c->len += (cu64)n;
    while (n > 0) {
        if (c->n == 0 && n >= 64) {
            sha1_block(c->h, p);
            p += 64, n -= 64;
            continue;
        }
        c->buf[c->n++] = *p++;
        n--;
        if (c->n == 64) {
            sha1_block(c->h, c->buf);
            c->n = 0;
        }
    }
}

void sha1_final(struct sha1_ctx *c, cu8 out[20])
{
    cu64 bits = c->len * 8;
    cu8 pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56)
        sha1_update(c, &pad, 1);
    cu8 l[8];
    for (int i = 0; i < 8; i++)
        l[i] = (cu8)(bits >> (56 - 8 * i));
    sha1_update(c, l, 8);
    for (int i = 0; i < 20; i++)
        out[i] = (cu8)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void sha1(const cu8 *p, int n, cu8 out[20])
{
    struct sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, p, n);
    sha1_final(&c, out);
}

/* ---------------- HMAC-SHA1, PBKDF2, PRF ---------------- */
struct hmac_ctx {
    struct sha1_ctx in, out;
};

static void hmac_start(struct hmac_ctx *h, const cu8 *key, int klen)
{
    cu8 k[64], kh[20];
    if (klen > 64) {
        sha1(key, klen, kh);
        key = kh, klen = 20;
    }
    for (int i = 0; i < 64; i++)
        k[i] = (i < klen ? key[i] : 0) ^ 0x36;
    sha1_init(&h->in);
    sha1_update(&h->in, k, 64);
    for (int i = 0; i < 64; i++)
        k[i] ^= 0x36 ^ 0x5c;
    sha1_init(&h->out);
    sha1_update(&h->out, k, 64);
}

/* MAC of the pieces, from a prepared key schedule (the contexts are copied, so it can be reused) */
static void hmac_run(const struct hmac_ctx *h, const cu8 *const p[4], const int n[4], cu8 out[20])
{
    struct sha1_ctx c = h->in;
    cu8 ih[20];
    for (int i = 0; i < 4; i++)
        if (p[i] && n[i] > 0)
            sha1_update(&c, p[i], n[i]);
    sha1_final(&c, ih);
    c = h->out;
    sha1_update(&c, ih, 20);
    sha1_final(&c, out);
}

void hmac_sha1_v(const cu8 *key, int klen, const cu8 *const p[4], const int n[4], cu8 out[20])
{
    struct hmac_ctx h;
    hmac_start(&h, key, klen);
    hmac_run(&h, p, n, out);
}

void hmac_sha1(const cu8 *key, int klen, const cu8 *msg, int mlen, cu8 out[20])
{
    const cu8 *p[4] = { msg, 0, 0, 0 };
    const int n[4] = { mlen, 0, 0, 0 };
    hmac_sha1_v(key, klen, p, n, out);
}

void pbkdf2_sha1(const cu8 *pass, int plen, const cu8 *salt, int slen, int iter, cu8 *out, int outlen)
{
    struct hmac_ctx h;
    hmac_start(&h, pass, plen);
    for (cu32 blk = 1; outlen > 0; blk++) {
        cu8 cnt[4] = { (cu8)(blk >> 24), (cu8)(blk >> 16), (cu8)(blk >> 8), (cu8)blk };
        cu8 u[20], t[20];
        const cu8 *p[4] = { salt, cnt, 0, 0 };
        int n[4] = { slen, 4, 0, 0 };
        hmac_run(&h, p, n, u);
        for (int i = 0; i < 20; i++)
            t[i] = u[i];
        p[1] = 0, n[1] = 0;
        for (int j = 1; j < iter; j++) {
            p[0] = u, n[0] = 20;
            hmac_run(&h, p, n, u);
            for (int i = 0; i < 20; i++)
                t[i] ^= u[i];
        }
        for (int i = 0; i < 20 && outlen > 0; i++, outlen--)
            *out++ = t[i];
    }
}

void prf_sha1(const cu8 *key, int klen, const char *label, const cu8 *data, int dlen, cu8 *out, int outlen)
{
    struct hmac_ctx h;
    hmac_start(&h, key, klen);
    int llen = 0;
    while (label[llen])
        llen++;
    cu8 zero = 0, cnt = 0;
    for (; outlen > 0; cnt++) {
        const cu8 *p[4] = { (const cu8 *)label, &zero, data, &cnt };
        const int n[4] = { llen, 1, dlen, 1 };
        cu8 h20[20];
        hmac_run(&h, p, n, h20);
        for (int i = 0; i < 20 && outlen > 0; i++, outlen--)
            *out++ = h20[i];
    }
}

/* ---------------- AES-128 (tables computed at first use) ---------------- */
static cu8 sbox[256], isbox[256];
static int aes_ready;

static cu8 rotl8(cu8 x, int n) { return (cu8)((x << n) | (x >> (8 - n))); }
static cu8 xtime(cu8 a) { return (cu8)((a << 1) ^ (a & 0x80 ? 0x1b : 0)); }

static cu8 gmul(cu8 a, cu8 b)
{
    cu8 r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

static void aes_tables(void)
{
    cu8 p = 1, q = 1;
    do {
        p = (cu8)(p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0));   /* p *= 3 */
        q ^= (cu8)(q << 1);                               /* q /= 3 */
        q ^= (cu8)(q << 2);
        q ^= (cu8)(q << 4);
        if (q & 0x80)
            q ^= 0x09;
        sbox[p] = (cu8)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++)
        isbox[sbox[i]] = (cu8)i;
    aes_ready = 1;
}

void aes_setkey(struct aes_key *k, const cu8 key[16])
{
    if (!aes_ready)
        aes_tables();
    cu8 *w = k->rk, rcon = 1;
    for (int i = 0; i < 16; i++)
        w[i] = key[i];
    for (int i = 16; i < 176; i += 4) {
        cu8 t0 = w[i - 4], t1 = w[i - 3], t2 = w[i - 2], t3 = w[i - 1];
        if (i % 16 == 0) {
            cu8 x = t0;
            t0 = sbox[t1] ^ rcon, t1 = sbox[t2], t2 = sbox[t3], t3 = sbox[x];
            rcon = xtime(rcon);
        }
        w[i] = w[i - 16] ^ t0, w[i + 1] = w[i - 15] ^ t1, w[i + 2] = w[i - 14] ^ t2, w[i + 3] = w[i - 13] ^ t3;
    }
}

void aes_encrypt(const struct aes_key *k, const cu8 in[16], cu8 out[16])
{
    cu8 s[16], t[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ k->rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int c = 0; c < 4; c++)   /* SubBytes + ShiftRows */
            for (int row = 0; row < 4; row++)
                t[row + 4 * c] = sbox[s[row + 4 * ((c + row) & 3)]];
        if (r < 10) {
            for (int c = 0; c < 4; c++) {   /* MixColumns */
                cu8 a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3;
                s[4 * c + 1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3;
                s[4 * c + 2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3;
                s[4 * c + 3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3);
            }
        } else {
            for (int i = 0; i < 16; i++)
                s[i] = t[i];
        }
        for (int i = 0; i < 16; i++)
            s[i] ^= k->rk[16 * r + i];
    }
    for (int i = 0; i < 16; i++)
        out[i] = s[i];
}

void aes_decrypt(const struct aes_key *k, const cu8 in[16], cu8 out[16])
{
    cu8 s[16], t[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ k->rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        for (int c = 0; c < 4; c++)   /* InvShiftRows + InvSubBytes */
            for (int row = 0; row < 4; row++)
                t[row + 4 * c] = isbox[s[row + 4 * ((c - row + 4) & 3)]];
        for (int i = 0; i < 16; i++)
            t[i] ^= k->rk[16 * r + i];
        if (r > 0) {
            for (int c = 0; c < 4; c++) {   /* InvMixColumns */
                cu8 a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                s[4 * c + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                s[4 * c + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                s[4 * c + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        } else {
            for (int i = 0; i < 16; i++)
                s[i] = t[i];
        }
    }
    for (int i = 0; i < 16; i++)
        out[i] = s[i];
}

int aes_unwrap(const cu8 kek[16], int n, const cu8 *in, cu8 *out)
{
    struct aes_key k;
    aes_setkey(&k, kek);
    cu8 a[8], b[16];
    for (int i = 0; i < 8; i++)
        a[i] = in[i];
    for (int i = 0; i < 8 * n; i++)
        out[i] = in[8 + i];
    for (int j = 5; j >= 0; j--) {
        for (int i = n; i >= 1; i--) {
            cu64 t = (cu64)n * j + i;
            for (int x = 0; x < 8; x++)
                b[x] = a[x] ^ (cu8)(t >> (56 - 8 * x));
            for (int x = 0; x < 8; x++)
                b[8 + x] = out[8 * (i - 1) + x];
            aes_decrypt(&k, b, b);
            for (int x = 0; x < 8; x++)
                a[x] = b[x], out[8 * (i - 1) + x] = b[8 + x];
        }
    }
    for (int i = 0; i < 8; i++)
        if (a[i] != 0xa6)
            return -1;
    return 0;
}

/* ---------------- AES-CCM (M = 8, L = 2) ---------------- */
static void ccm_mac(const struct aes_key *k, const cu8 nonce[13], const cu8 *aad, int alen, const cu8 *data, int dlen,
                    cu8 x[16])
{
    cu8 b[16];
    b[0] = (alen ? 0x40 : 0) | ((8 - 2) / 2) << 3 | (2 - 1);
    for (int i = 0; i < 13; i++)
        b[1 + i] = nonce[i];
    b[14] = (cu8)(dlen >> 8), b[15] = (cu8)dlen;
    aes_encrypt(k, b, x);
    if (alen) {   /* 2-byte length, then the AAD, zero padded to whole blocks */
        cu8 blk[16];
        int pos = 2, i = 0;
        blk[0] = (cu8)(alen >> 8), blk[1] = (cu8)alen;
        for (;;) {
            while (pos < 16 && i < alen)
                blk[pos++] = aad[i++];
            while (pos < 16)
                blk[pos++] = 0;
            for (int j = 0; j < 16; j++)
                x[j] ^= blk[j];
            aes_encrypt(k, x, x);
            if (i >= alen)
                break;
            pos = 0;
        }
    }
    for (int i = 0; i < dlen; i += 16) {
        for (int j = 0; j < 16 && i + j < dlen; j++)
            x[j] ^= data[i + j];
        aes_encrypt(k, x, x);
    }
}

static void ccm_ctr(const struct aes_key *k, const cu8 nonce[13], cu8 *data, int dlen, cu8 s0[16])
{
    cu8 a[16], s[16];
    a[0] = 2 - 1;
    for (int i = 0; i < 13; i++)
        a[1 + i] = nonce[i];
    a[14] = a[15] = 0;
    aes_encrypt(k, a, s0);
    for (int i = 0, ctr = 1; i < dlen; i += 16, ctr++) {
        a[14] = (cu8)(ctr >> 8), a[15] = (cu8)ctr;
        aes_encrypt(k, a, s);
        for (int j = 0; j < 16 && i + j < dlen; j++)
            data[i + j] ^= s[j];
    }
}

void aes_ccm_encrypt(const cu8 key[16], const cu8 nonce[13], const cu8 *aad, int alen, cu8 *data, int dlen, cu8 mic[8])
{
    struct aes_key k;
    cu8 x[16], s0[16];
    aes_setkey(&k, key);
    ccm_mac(&k, nonce, aad, alen, data, dlen, x);
    ccm_ctr(&k, nonce, data, dlen, s0);
    for (int i = 0; i < 8; i++)
        mic[i] = x[i] ^ s0[i];
}

int aes_ccm_decrypt(const cu8 key[16], const cu8 nonce[13], const cu8 *aad, int alen, cu8 *data, int dlen,
                    const cu8 mic[8])
{
    struct aes_key k;
    cu8 x[16], s0[16];
    aes_setkey(&k, key);
    ccm_ctr(&k, nonce, data, dlen, s0);
    ccm_mac(&k, nonce, aad, alen, data, dlen, x);
    int bad = 0;
    for (int i = 0; i < 8; i++)
        bad |= mic[i] ^ x[i] ^ s0[i];
    return bad ? -1 : 0;
}

/* ---------------- CCMP ---------------- */
int ieee80211_hdrlen(const cu8 *f)
{
    int type = (f[0] >> 2) & 3, len = 24;
    if (type == 2) {
        if ((f[1] & 3) == 3)
            len += 6;   /* A4 */
        if (f[0] & 0x80) {
            len += 2;   /* QoS control */
            if (f[1] & 0x80)
                len += 4;   /* HT control */
        }
    } else if (type == 0 && (f[1] & 0x80)) {
        len += 4;
    }
    return len;
}

/* AAD and nonce, IEEE 802.11-2016 12.5.3.3.3/4 (no PMF on data, no SPP A-MSDU) */
static int ccmp_aad_nonce(const cu8 *f, int hdrlen, const cu8 pn[6], cu8 aad[32], cu8 nonce[13])
{
    int type = (f[0] >> 2) & 3, qos = type == 2 && (f[0] & 0x80), n = 0;
    aad[n++] = type == 2 ? (f[0] & 0x8f) : f[0];   /* data: subtype bits 4-6 masked */
    aad[n++] = (f[1] & ~(0x08 | 0x10 | 0x20) & (qos ? 0x7f : 0xff)) | 0x40;
    for (int i = 4; i < 22; i++)
        aad[n++] = f[i];   /* A1 A2 A3 */
    aad[n++] = f[22] & 0x0f;   /* fragment number only */
    aad[n++] = 0;
    int pos = 24;
    if (type == 2 && (f[1] & 3) == 3) {
        for (int i = 0; i < 6; i++)
            aad[n++] = f[pos + i];
        pos += 6;
    }
    cu8 prio = 0;
    if (qos) {
        prio = f[pos] & 0x0f;
        aad[n++] = f[pos] & 0x0f;
        aad[n++] = 0;
    }
    (void)hdrlen;
    nonce[0] = prio | (type == 0 ? 0x10 : 0);
    for (int i = 0; i < 6; i++)
        nonce[1 + i] = f[10 + i];   /* A2 */
    for (int i = 0; i < 6; i++)
        nonce[7 + i] = pn[5 - i];   /* PN5 first */
    return n;
}

int ccmp_encrypt(const cu8 tk[16], cu8 *f, int hdrlen, int plen, cu64 pn, int keyid)
{
    cu8 p[6], aad[32], nonce[13];
    for (int i = 0; i < 6; i++)
        p[i] = (cu8)(pn >> (8 * i));   /* p[0] = PN0 */
    f[1] |= 0x40;
    cu8 *h = f + hdrlen;
    h[0] = p[0], h[1] = p[1], h[2] = 0, h[3] = (cu8)(0x20 | (keyid << 6)), h[4] = p[2], h[5] = p[3], h[6] = p[4],
    h[7] = p[5];
    int alen = ccmp_aad_nonce(f, hdrlen, p, aad, nonce);
    aes_ccm_encrypt(tk, nonce, aad, alen, h + 8, plen, h + 8 + plen);
    return hdrlen + 8 + plen + 8;
}

int ccmp_decrypt(const cu8 tk[16], cu8 *f, int len, int hdrlen, cu64 *pn)
{
    int plen = len - hdrlen - 16;
    if (plen < 0)
        return -1;
    const cu8 *h = f + hdrlen;
    if (!(h[3] & 0x20))
        return -1;   /* ExtIV must be set */
    cu8 p[6] = { h[0], h[1], h[4], h[5], h[6], h[7] }, aad[32], nonce[13];
    *pn = 0;
    for (int i = 5; i >= 0; i--)
        *pn = (*pn << 8) | p[i];
    int alen = ccmp_aad_nonce(f, hdrlen, p, aad, nonce);
    if (aes_ccm_decrypt(tk, nonce, aad, alen, f + hdrlen + 8, plen, f + hdrlen + 8 + plen))
        return -1;
    return plen;
}

/* ---------------- CRC-32 ---------------- */
cu32 crc32_le(const cu8 *p, int n)
{
    cu32 c = 0xffffffff;
    while (n-- > 0) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320 & -(c & 1));
    }
    return ~c;
}
