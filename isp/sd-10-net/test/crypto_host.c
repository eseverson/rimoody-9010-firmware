/* Host CLI around crypto.c for test_crypto.py. Arguments and output are hex. Build: cc -O2 -o crypto_host crypto_host.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../crypto.c"

static int unhex(const char *s, unsigned char *out)
{
    int n = 0;
    while (s[0] && s[1]) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (unsigned char)v;
        s += 2;
    }
    return n;
}

static void put(const unsigned char *p, int n)
{
    for (int i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static unsigned char a[70000], b[70000], c[70000], o[70000];

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const char *op = argv[1];
    if (!strcmp(op, "sha1")) {
        int n = unhex(argv[2], a);
        sha1(a, n, o);
        put(o, 20);
    } else if (!strcmp(op, "hmac")) {
        int kn = unhex(argv[2], a), mn = unhex(argv[3], b);
        hmac_sha1(a, kn, b, mn, o);
        put(o, 20);
    } else if (!strcmp(op, "pbkdf2")) {   /* pass-hex salt-hex iter len */
        int pn = unhex(argv[2], a), sn = unhex(argv[3], b), len = atoi(argv[5]);
        pbkdf2_sha1(a, pn, b, sn, atoi(argv[4]), o, len);
        put(o, len);
    } else if (!strcmp(op, "prf")) {   /* key-hex label data-hex len */
        int kn = unhex(argv[2], a), dn = unhex(argv[4], b), len = atoi(argv[5]);
        prf_sha1(a, kn, argv[3], b, dn, o, len);
        put(o, len);
    } else if (!strcmp(op, "aes")) {
        struct aes_key k;
        unhex(argv[2], a);
        unhex(argv[3], b);
        aes_setkey(&k, a);
        aes_encrypt(&k, b, o);
        put(o, 16);
        aes_decrypt(&k, o, c);
        put(c, 16);
    } else if (!strcmp(op, "unwrap")) {
        unhex(argv[2], a);
        int n = unhex(argv[3], b) / 8 - 1;
        int r = aes_unwrap(a, n, b, o);
        if (r)
            printf("FAIL\n");
        else
            put(o, 8 * n);
    } else if (!strcmp(op, "ccm")) {   /* key nonce aad data -> ciphertext||mic */
        unhex(argv[2], a);
        unhex(argv[3], b);
        int an = unhex(argv[4], c), dn = unhex(argv[5], o);
        aes_ccm_encrypt(a, b, c, an, o, dn, o + dn);
        put(o, dn + 8);
        int r = aes_ccm_decrypt(a, b, c, an, o, dn, o + dn);
        printf("%s\n", r ? "DECFAIL" : "DECOK");
    } else if (!strcmp(op, "ccmp")) {   /* tk pn-hex(12) frame-hex hdrlen keyid -> encrypted MPDU, then decrypt check */
        unhex(argv[2], a);
        unsigned long long pn = strtoull(argv[3], 0, 16);
        int fn = unhex(argv[4], b), hl = atoi(argv[5]);
        memcpy(c, b, hl);
        memcpy(c + hl + 8, b + hl, fn - hl);
        int len = ccmp_encrypt(a, c, hl, fn - hl, pn, atoi(argv[6]));
        put(c, len);
        unsigned long long rpn;
        int pl = ccmp_decrypt(a, c, len, ieee80211_hdrlen(c), &rpn);
        printf("%s %llx\n", pl == fn - hl && !memcmp(c + hl + 8, b + hl, pl) ? "DECOK" : "DECFAIL", rpn);
    } else if (!strcmp(op, "crc32")) {
        int n = unhex(argv[2], a);
        printf("%08x\n", crc32_le(a, n));
    } else {
        return 2;
    }
    return 0;
}
