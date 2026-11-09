/* Host test for aa_tls.c: TLS handshake + echo with `openssl s_server` over TCP, the socket only moving the bytes
 * aa_tls_take / aa_tls_feed hand over (as the AA control channel will). Usage: tls_host PORT CRT KEY */
#include "../aa_tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>

static unsigned char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    static unsigned char b[2][8192];
    static int k;
    unsigned char *o = b[k++];
    *n = fread(o, 1, 8191, f);
    o[(*n)++] = 0;
    fclose(f);
    return o;
}

static void pump(int s)
{
    unsigned char b[20000];
    size_t n;
    while ((n = aa_tls_take(b, sizeof b)))
        if (write(s, b, n) != (ssize_t)n) { perror("write"); exit(2); }
    struct pollfd p = { s, POLLIN, 0 };
    if (poll(&p, 1, 100) > 0) {
        ssize_t r = read(s, b, sizeof b);
        if (r <= 0) { fprintf(stderr, "server closed\n"); exit(3); }
        aa_tls_feed(b, (size_t)r);
    }
}

int main(int argc, char **argv)
{
    size_t cn, kn;
    unsigned char *crt = slurp(argv[2], &cn), *key = slurp(argv[3], &kn);
    if (aa_tls_init(crt, cn, key, kn)) { printf("FAIL init: %s\n", aa_tls_error()); return 1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[1])) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 1; }
    int r, steps = 0;
    while ((r = aa_tls_handshake()) == 1 && steps++ < 200)
        pump(s);
    if (r) { printf("FAIL handshake: %s\n", r < 0 ? aa_tls_error() : "timeout"); return 1; }
    pump(s);
    printf("PASS handshake, cipher %s\n", aa_tls_cipher());
    const char *msg = "hello head unit\n";
    if (aa_tls_write((const unsigned char *)msg, strlen(msg)) < 0) { printf("FAIL write: %s\n", aa_tls_error()); return 1; }
    unsigned char got[256];
    int k = 0;
    for (int i = 0; i < 50 && k < (int)strlen(msg); i++) {
        pump(s);
        r = aa_tls_read(got + k, sizeof got - 1 - k);
        if (r < 0) { printf("FAIL read: %s\n", aa_tls_error()); return 1; }
        k += r;
    }
    got[k] = 0;
    printf("%s echo: %s", k ? "PASS" : "FAIL", k ? (char *)got : "(nothing)\n");
    return k ? 0 : 1;
}
