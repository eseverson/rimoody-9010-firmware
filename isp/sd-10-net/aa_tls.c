/* TLS for the Android Auto link (notes/android_auto.md). The head unit is the TLS client: it presents the head-unit
 * certificate, and the TLS records travel inside AA control messages (type 3, "SSL handshake") and then as the
 * encrypted payload of AA frames. So there is no socket: received record bytes are pushed in with aa_tls_feed, and
 * bytes to send collect in an output buffer the AA layer drains with aa_tls_take.
 *
 * Builds for the payload (FreeRTOS heap, our entropy pool) and on the host for test/test_tls.py (AA_TLS_HOST: libc
 * heap, /dev/urandom). One link at a time. */
#include "aa_tls.h"
#include <string.h>
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform.h"
#include "mbedtls/error.h"

#ifdef AA_TLS_HOST
#include <stdio.h>
#include <stdlib.h>
static void *tls_calloc(size_t n, size_t s) { return calloc(n, s); }
static void tls_free(void *p) { free(p); }
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
    (void)data;
    FILE *f = fopen("/dev/urandom", "rb");
    *olen = f ? fread(out, 1, len, f) : 0;
    if (f)
        fclose(f);
    return *olen == len ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}
#else
#include "FreeRTOS.h"
void hu_random(unsigned char *out, int n);   /* net.c entropy pool (eFuse, radio frames, 1 MHz timer jitter) */
static void *tls_calloc(size_t n, size_t s)
{
    if (s && n > (size_t)-1 / s)
        return 0;
    void *p = pvPortMalloc(n * s);
    if (p)
        memset(p, 0, n * s);
    return p;
}
static void tls_free(void *p) { vPortFree(p); }
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen)
{
    (void)data;
    hu_random(out, (int)len);
    *olen = len;
    return 0;
}
#endif

#define IN_MAX  (20 * 1024)
#define OUT_MAX (20 * 1024)

static struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt crt;
    mbedtls_pk_context key;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context ent;
    unsigned char in[IN_MAX], out[OUT_MAX];
    size_t in_r, in_w, out_n;
    int up, inited;
    char err[96];
} t;

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    (void)ctx;
    if (t.out_n + len > OUT_MAX)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(t.out + t.out_n, buf, len);
    t.out_n += len;
    return (int)len;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    size_t have = t.in_w - t.in_r;
    if (!have)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (len > have)
        len = have;
    memcpy(buf, t.in + t.in_r, len);
    t.in_r += len;
    if (t.in_r == t.in_w)
        t.in_r = t.in_w = 0;
    return (int)len;
}

static int fail(const char *what, int e)
{
#ifdef MBEDTLS_ERROR_C
    char m[64];
    mbedtls_strerror(e, m, sizeof m);
#else
    const char *m = "";
#endif
    int k = 0;
    for (const char *s = what; *s && k < 40; s++)
        t.err[k++] = *s;
    t.err[k++] = ':';
    t.err[k++] = ' ';
    static const char hx[] = "0123456789abcdef";
    unsigned v = (unsigned)-e;
    t.err[k++] = '-';
    t.err[k++] = '0';
    t.err[k++] = 'x';
    for (int s = 12; s >= 0; s -= 4)
        t.err[k++] = hx[(v >> s) & 15];
    t.err[k++] = ' ';
    for (const char *s = m; *s && k < (int)sizeof t.err - 1; s++)
        t.err[k++] = *s;
    t.err[k] = 0;
    return e;
}

const char *aa_tls_error(void) { return t.err; }

void aa_tls_free(void)
{
    if (!t.inited)
        return;
    mbedtls_ssl_free(&t.ssl);
    mbedtls_ssl_config_free(&t.conf);
    mbedtls_x509_crt_free(&t.crt);
    mbedtls_pk_free(&t.key);
    mbedtls_ctr_drbg_free(&t.drbg);
    mbedtls_entropy_free(&t.ent);
    t.inited = 0;
}

int aa_tls_init(const unsigned char *crt_pem, size_t crt_len, const unsigned char *key_pem, size_t key_len)
{
    aa_tls_free();
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    mbedtls_ssl_init(&t.ssl);
    mbedtls_ssl_config_init(&t.conf);
    mbedtls_x509_crt_init(&t.crt);
    mbedtls_pk_init(&t.key);
    mbedtls_ctr_drbg_init(&t.drbg);
    mbedtls_entropy_init(&t.ent);
    t.inited = 1;
    t.in_r = t.in_w = t.out_n = 0;
    t.up = 0;
    t.err[0] = 0;
    int e;
    /* PEM parsing wants the terminating NUL counted in the length */
    if ((e = mbedtls_x509_crt_parse(&t.crt, crt_pem, crt_len)))
        return fail("certificate", e);
    if ((e = mbedtls_ctr_drbg_seed(&t.drbg, mbedtls_entropy_func, &t.ent, (const unsigned char *)"hu-aa", 5)))
        return fail("drbg seed", e);
    if ((e = mbedtls_pk_parse_key(&t.key, key_pem, key_len, 0, 0, mbedtls_ctr_drbg_random, &t.drbg)))
        return fail("private key", e);
    if ((e = mbedtls_ssl_config_defaults(&t.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)))
        return fail("config", e);
    mbedtls_ssl_conf_authmode(&t.conf, MBEDTLS_SSL_VERIFY_NONE);   /* the phone's certificate is not checked */
    mbedtls_ssl_conf_rng(&t.conf, mbedtls_ctr_drbg_random, &t.drbg);
    mbedtls_ssl_conf_max_tls_version(&t.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_min_tls_version(&t.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    /* Cipher preference: the payload decrypts the whole video stream in software, and AES-256-GCM there runs at ~640
     * KB/s (5 Mbit/s, 7.3 s of a 12 s test stream). ChaCha20-Poly1305 first (fast in plain C), then AES-128-GCM, then
     * mbedtls's default list; the phone picks, so offering more costs nothing if it prefers AES. */
    static int suites[64];
    if (!suites[0]) {
        static const int first[] = { MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
                                     MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 };
        int n = 0;
        for (unsigned i = 0; i < sizeof first / sizeof first[0]; i++)
            suites[n++] = first[i];
        for (const int *d = mbedtls_ssl_list_ciphersuites(); *d && n < 63; d++) {
            int dup = 0;
            for (int i = 0; i < n; i++)
                dup |= suites[i] == *d;
            if (!dup)
                suites[n++] = *d;
        }
        suites[n] = 0;
    }
    mbedtls_ssl_conf_ciphersuites(&t.conf, suites);
    if ((e = mbedtls_ssl_conf_own_cert(&t.conf, &t.crt, &t.key)))
        return fail("own cert", e);
    if ((e = mbedtls_ssl_setup(&t.ssl, &t.conf)))
        return fail("setup", e);
    mbedtls_ssl_set_bio(&t.ssl, 0, bio_send, bio_recv, 0);
    return 0;
}

int aa_tls_feed(const unsigned char *p, size_t n)
{
    if (t.in_w + n > IN_MAX)
        return -1;
    memcpy(t.in + t.in_w, p, n);
    t.in_w += n;
    return 0;
}

size_t aa_tls_take(unsigned char *out, size_t max)
{
    size_t n = t.out_n < max ? t.out_n : max;
    memcpy(out, t.out, n);
    memmove(t.out, t.out + n, t.out_n - n);
    t.out_n -= n;
    return n;
}

int aa_tls_handshake(void)
{
    if (t.up)
        return 0;
    int e = mbedtls_ssl_handshake(&t.ssl);
    if (!e) {
        t.up = 1;
        return 0;
    }
    if (e == MBEDTLS_ERR_SSL_WANT_READ || e == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 1;
    return fail("handshake", e);
}

const char *aa_tls_cipher(void) { return t.up ? mbedtls_ssl_get_ciphersuite(&t.ssl) : "none"; }

int aa_tls_write(const unsigned char *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        int e = mbedtls_ssl_write(&t.ssl, p + done, n - done);
        if (e < 0)
            return fail("write", e);
        done += (size_t)e;
    }
    return (int)done;
}

int aa_tls_read(unsigned char *p, size_t max)
{
    int e = mbedtls_ssl_read(&t.ssl, p, max);
    if (e == MBEDTLS_ERR_SSL_WANT_READ)
        return 0;
    return e < 0 ? fail("read", e) : e;
}
