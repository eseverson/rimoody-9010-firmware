/* Host driver for gcm_alt.c (test/test_gcm.py). One case per stdin line:
 *   E KEY IV AAD PT  CHUNK OFF   -> "CT TAG"
 *   D KEY IV AAD CT TAG CHUNK OFF -> "PT" or "AUTHFAIL"
 * Hex fields ("-" = empty). CHUNK 0 uses crypt_and_tag / auth_decrypt in one call; otherwise starts, update_ad and
 * update are fed CHUNK bytes at a time (the partial-block paths). OFF misaligns the input and output buffers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/gcm.h"

static size_t unhex(const char *s, unsigned char *o)
{
    if (!strcmp(s, "-"))
        return 0;
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++)
        sscanf(s + 2 * i, "%2hhx", &o[i]);
    return n;
}

static void hex(const unsigned char *p, size_t n)
{
    if (!n)
        fputs("-", stdout);
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}

int main(void)
{
    static char line[1 << 20], f[7][1 << 18];
    static unsigned char key[32], iv[256], aad[1 << 16], ibuf[(1 << 16) + 8], obuf[(1 << 16) + 8], tag[16], t2[16];
    while (fgets(line, sizeof line, stdin)) {
        char op;
        int chunk = 0, off = 0;
        if (line[0] == 'E') {
            if (sscanf(line, "%c %s %s %s %s %d %d", &op, f[0], f[1], f[2], f[3], &chunk, &off) != 7)
                return 2;
        } else if (sscanf(line, "%c %s %s %s %s %s %d %d", &op, f[0], f[1], f[2], f[3], f[4], &chunk, &off) != 8)
            return 2;
        size_t kl = unhex(f[0], key), ivl = unhex(f[1], iv), al = unhex(f[2], aad);
        unsigned char *in = ibuf + off, *out = obuf + (off ? 4 - off : 0);
        size_t n = unhex(f[3], in), tl = op == 'D' ? unhex(f[4], t2) : 16;
        mbedtls_gcm_context g;
        mbedtls_gcm_init(&g);
        if (mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, (unsigned)kl * 8))
            return 3;
        int mode = op == 'E' ? MBEDTLS_GCM_ENCRYPT : MBEDTLS_GCM_DECRYPT, r;
        if (!chunk) {
            r = op == 'E' ? mbedtls_gcm_crypt_and_tag(&g, mode, n, iv, ivl, aad, al, in, out, 16, tag)
                          : mbedtls_gcm_auth_decrypt(&g, n, iv, ivl, aad, al, t2, tl, in, out);
        } else {
            size_t olen;
            r = mbedtls_gcm_starts(&g, mode, iv, ivl);
            for (size_t i = 0; !r && i < al; i += chunk)
                r = mbedtls_gcm_update_ad(&g, aad + i, al - i < (size_t)chunk ? al - i : (size_t)chunk);
            for (size_t i = 0; !r && i < n; i += chunk) {
                size_t c = n - i < (size_t)chunk ? n - i : (size_t)chunk;
                r = mbedtls_gcm_update(&g, in + i, c, out + i, c, &olen);
                if (!r && olen != c)
                    return 4;
            }
            if (!r)
                r = mbedtls_gcm_finish(&g, NULL, 0, &olen, tag, tl);
            if (!r && op == 'D' && memcmp(tag, t2, tl))
                r = MBEDTLS_ERR_GCM_AUTH_FAILED;
        }
        mbedtls_gcm_free(&g);
        if (r == MBEDTLS_ERR_GCM_AUTH_FAILED) {
            puts("AUTHFAIL");
            continue;
        }
        if (r)
            return 5;
        hex(out, n);
        if (op == 'E')
            putchar(' '), hex(tag, 16);
        putchar('\n');
        fflush(stdout);
    }
    return 0;
}
