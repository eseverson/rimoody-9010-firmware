/* Host CLI around aacert.c for test_aacert.py: the payload's extraction code against a flash image on the PC.
 *
 *   aacert_host FLASH OUTDIR [BUDGET]   write OUTDIR/AACERT.PEM and OUTDIR/AAKEY.PEM; BUDGET = bytes unpacked per step
 *                                       (default 32768, as the payload)
 *
 * One `key=value` line per fact on stdout, never key material; exit 0 = written, 2 = extraction failed (err=),
 * 1 = usage or I/O error. The pair check is the payload's own (aa_tls_check_pair, mbedtls). */
#include "../aacert.h"
#include "../aa_tls.h"
#include "../memmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static char subject[200];
static int checks;
static int check(void *ctx, const char *cert, unsigned cl, const char *key, unsigned kl)
{
    (void)ctx;
    checks++;
    return aa_tls_check_pair((const unsigned char *)cert, cl, (const unsigned char *)key, kl, subject, sizeof subject);
}

static int save(const char *dir, const char *name, const char *p, unsigned n, int mode)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f))
        return -1;
    return chmod(path, mode);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return fprintf(stderr, "usage: aacert_host FLASH OUTDIR [BUDGET]\n"), 1;
    unsigned budget = argc > 3 ? (unsigned)strtoul(argv[3], 0, 0) : 32768;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return perror(argv[1]), 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *flash = malloc(n);
    if (!flash || fread(flash, 1, n, f) != (size_t)n)
        return fprintf(stderr, "read %s failed\n", argv[1]), 1;
    fclose(f);
    void *work = malloc(MM_AACERT_SZ);
    static char cert[AACERT_PEM_MAX], key[AACERT_PEM_MAX];
    static struct aacert a;

    double t0 = now_us();
    int r = aacert_open(&a, flash, (unsigned)n, work, MM_AACERT_SZ);
    double t1 = now_us(), worst = 0;
    int steps = 0;
    while (!r) {
        double s0 = now_us();
        r = aacert_step(&a, budget, cert, key, check, 0);
        double s = now_us() - s0;
        worst = s > worst ? s : worst;
        steps++;
        if (r == 0)
            break;
        if (r == 1)
            r = 0;
    }
    double t2 = now_us();
    printf("dirent=0x%x\nino=%u\nnodes=%u\nfirst_node=0x%x\npacked=%u\nunpacked=%u\nxor=0x%02x\nwork_used=%u\n", a.dirent,
           a.ino, a.nodes, a.first_node, a.packed, a.unpacked, a.xor_key, a.work_used);
    printf("decoded=%u\nsteps=%d\nchecks=%d\nopen_us=%.0f\nmax_step_us=%.0f\ntotal_us=%.0f\n", a.decoded, steps, checks,
           t1 - t0, worst, t2 - t0);
    if (r < 0)
        return printf("err=%d\nerr_text=%s\n", r, aacert_strerror(r)), 2;
    printf("key_at=0x%x\ncert_at=0x%x\nkey_len=%u\ncert_len=%u\nsubject=%s\n", a.key_at, a.cert_at, a.key_len,
           a.cert_len, subject);
    if (save(argv[2], "AACERT.PEM", cert, a.cert_len, 0644) || save(argv[2], "AAKEY.PEM", key, a.key_len, 0600))
        return fprintf(stderr, "write to %s failed\n", argv[2]), 1;
    return 0;
}
