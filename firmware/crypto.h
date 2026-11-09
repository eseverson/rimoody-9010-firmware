/* WPA2-PSK crypto for the head-unit payload: SHA-1, HMAC-SHA1, PBKDF2, the 802.11 PRF, AES-128, RFC 3394 key unwrap,
 * CCMP (AES-CCM, M=8 L=2) and CRC-32. Plain C, no library calls, so the same file builds for the NDS32 payload and for
 * the host test (test_crypto.py drives crypto_host.c). */
#ifndef CRYPTO_H
#define CRYPTO_H

typedef unsigned int cu32;
typedef unsigned char cu8;
typedef unsigned long long cu64;

struct sha1_ctx {
    cu32 h[5];
    cu64 len;
    cu8 buf[64];
    int n;
};

void sha1_init(struct sha1_ctx *c);
void sha1_update(struct sha1_ctx *c, const cu8 *p, int n);
void sha1_final(struct sha1_ctx *c, cu8 out[20]);
void sha1(const cu8 *p, int n, cu8 out[20]);

/* HMAC-SHA1 over the concatenation of up to 4 pieces (NULL/0 for unused ones) */
void hmac_sha1_v(const cu8 *key, int klen, const cu8 *const p[4], const int n[4], cu8 out[20]);
void hmac_sha1(const cu8 *key, int klen, const cu8 *msg, int mlen, cu8 out[20]);

void pbkdf2_sha1(const cu8 *pass, int plen, const cu8 *salt, int slen, int iter, cu8 *out, int outlen);
/* IEEE 802.11 PRF-n: HMAC-SHA1(K, label || 0 || data || i), i = 0, 1, ...; label without its NUL */
void prf_sha1(const cu8 *key, int klen, const char *label, const cu8 *data, int dlen, cu8 *out, int outlen);

struct aes_key {
    cu8 rk[176];
};
void aes_setkey(struct aes_key *k, const cu8 key[16]);
void aes_encrypt(const struct aes_key *k, const cu8 in[16], cu8 out[16]);
void aes_decrypt(const struct aes_key *k, const cu8 in[16], cu8 out[16]);

/* RFC 3394 unwrap with a 128-bit KEK: in has n + 1 64-bit blocks, out gets n. Returns 0 if the IV checks. */
int aes_unwrap(const cu8 kek[16], int n, const cu8 *in, cu8 *out);

/* AES-CCM, M = 8, L = 2, 13-byte nonce (RFC 3610 / CCMP). In place. */
void aes_ccm_encrypt(const cu8 key[16], const cu8 nonce[13], const cu8 *aad, int alen, cu8 *data, int dlen, cu8 mic[8]);
int aes_ccm_decrypt(const cu8 key[16], const cu8 nonce[13], const cu8 *aad, int alen, cu8 *data, int dlen,
                    const cu8 mic[8]);

/* CCMP on an 802.11 MPDU. Encrypt: frame holds hdrlen header bytes, then 8 free bytes for the CCMP header, then
 * plen plaintext bytes; afterwards the payload is ciphertext followed by the 8-byte MIC. Returns the new length.
 * Decrypt: frame of len bytes (no FCS); returns the plaintext length (plaintext at frame + hdrlen + 8) or -1, and the
 * received PN. */
int ccmp_encrypt(const cu8 tk[16], cu8 *frame, int hdrlen, int plen, cu64 pn, int keyid);
int ccmp_decrypt(const cu8 tk[16], cu8 *frame, int len, int hdrlen, cu64 *pn);
int ieee80211_hdrlen(const cu8 *frame);

cu32 crc32_le(const cu8 *p, int n);   /* zlib / IEEE 802.3 CRC-32 */
cu32 crc32_update(cu32 crc, const cu8 *p, int n);   /* continue a crc32_le over more data (start with 0) */

#endif
