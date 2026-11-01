/* Run aa.c + aa_tls.c from a PC against a phone running Android Auto's developer "head unit server" (TCP 5277):
 *   aa_net_host PHONE_IP CERT.pem KEY.pem [seconds] [video.h264]
 * Build (in test/): gcc -O1 -DAA_TLS_HOST -I.. -I../mbedtls/include '-DMBEDTLS_CONFIG_FILE="aa_tls_config.h"' -o aa_net_host aa_net_host.c ../aa.c ../aa_tls.c ../mbedtls/library/*.c
 * Same protocol code as the unit (version 1.7, media/speech/system audio and a microphone, which the phone requires);
 * saves the phone's H.264 so the decoder can be checked against ffmpeg on real streams. Certificate and key are read at
 * run time, never built in. */
#include "../aa.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int sock = -1;
static FILE *vf;

static unsigned long long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int send_cb(const unsigned char *p, int n)
{
    int done = 0;
    while (done < n) {
        ssize_t r = send(sock, p + done, (size_t)(n - done), 0);
        if (r <= 0)
            return -1;
        done += (int)r;
    }
    return n;
}

static void video_cb(const unsigned char *p, int n, unsigned long long ts)
{
    (void)ts;
    if (vf)
        fwrite(p, 1, (size_t)n, vf);
}

static void audio_cb(int stream, const unsigned char *p, int n) { (void)stream, (void)p, (void)n; }
static void log_cb(const char *s) { fputs(s, stdout); fflush(stdout); }

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    unsigned char *b = malloc(16384);
    *n = fread(b, 1, 16383, f);
    b[(*n)++] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: aa_net_host PHONE_IP CERT.pem KEY.pem [seconds] [video.h264]\n");
        return 2;
    }
    int secs = argc > 4 ? atoi(argv[4]) : 30;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5277) };
    inet_pton(AF_INET, argv[1], &a.sin_addr);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sock, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 1; }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (argc > 5)
        vf = fopen(argv[5], "wb");
    struct aa_platform p = { 0 };
    p.send = send_cb, p.video = video_cb, p.audio = audio_cb, p.log = log_cb, p.now_us = now_us, p.dpi = 160;
    p.ver_minor = 7, p.audio_mask = 0x87;
    p.cert = slurp(argv[2], &p.cert_len);
    p.key = slurp(argv[3], &p.key_len);
    if (aa_start(&p)) return 1;
    unsigned long long end = now_us() + (unsigned long long)secs * 1000000;
    enum aa_state last = AA_IDLE;
    static const char *const names[] = { "idle", "version", "tls", "authed", "discovery", "running", "closed", "error" };
    unsigned char buf[16384];
    while (now_us() < end) {
        struct pollfd pf = { sock, POLLIN, 0 };
        if (poll(&pf, 1, 50) > 0) {
            ssize_t got = recv(sock, buf, sizeof buf, 0);
            if (got <= 0) { printf("phone closed\n"); break; }
            aa_rx(buf, (int)got);
        }
        const struct aa_status *s = aa_status();
        if (s->state != last) {
            printf("state: %s (version %u.%u, channels open %u)\n", names[s->state], s->version_major, s->version_minor, s->channels_open);
            last = s->state;
            if (s->state == AA_ERROR || s->state == AA_CLOSED)
                break;
        }
    }
    const struct aa_status *s = aa_status();
    printf("done: state %s, rx frames %u msgs %u, tx frames %u, video frames %u (%u bytes), audio frames %u, unknown %u, errors %u\n",
           names[s->state], s->rx_frames, s->rx_msgs, s->tx_frames, s->video_frames, s->video_bytes, s->audio_frames, s->unknown, s->errors);
    if (vf)
        fclose(vf);
    close(sock);
    return 0;
}
