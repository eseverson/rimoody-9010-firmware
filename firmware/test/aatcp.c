/* Host test: aa.c over aa_tcp.c over lwIP, with a scripted phone on a second netif. Checks the version request,
 * answers it, checks that the head unit then starts TLS (a ClientHello inside SSL message 3), completes the handshake
 * as the phone (the TLS server, mbedtls), then runs service discovery, opens the video and microphone channels and
 * drives the microphone: setup, MicrophoneRequest open with max_unacked 2, timestamped PCM from aa_send_mic with its
 * window, acks, close. Usage: aatcp_host CERT.pem KEY.pem (throwaway pair, made by test_aatcp.sh; the phone uses it too). */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "../aa.h"
#include "../aa_tcp.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int plat_aa_accept_ok(void) { return 1; }

static unsigned int now_ms;
u32_t sys_now(void) { return now_ms; }
unsigned int lwip_rand(void) { return (unsigned)rand(); }
void lwip_assert(const char *m, const char *f, int l) { printf("ASSERT %s %s:%d\n", m, f, l); exit(3); }

#define QN 1024
static struct { int to, len; unsigned char d[1600]; } q[QN];
static int qh, qt;
static struct netif phone_if, hu_if;
static err_t out(struct netif *n, struct pbuf *p)
{
    q[qt].to = n == &phone_if;
    q[qt].len = pbuf_copy_partial(p, q[qt].d, p->tot_len, 0);
    qt = (qt + 1) % QN;
    return ERR_OK;
}
static err_t nif_init(struct netif *n)
{
    n->linkoutput = out, n->output = etharp_output, n->mtu = 1500, n->hwaddr_len = 6;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* ---- the phone's end of TLS: a server, records through memory buffers like aa_tls.c ---- */
static mbedtls_ssl_context pssl;
static mbedtls_ssl_config pconf;
static mbedtls_x509_crt pcrt;
static mbedtls_pk_context pkey;
static mbedtls_ctr_drbg_context pdrbg;
static mbedtls_entropy_context pent;
static unsigned char pin[65536], pout[65536];
static size_t pin_n, pout_n;
static int p_send(void *c, const unsigned char *b, size_t n)
{
    if (pout_n + n > sizeof pout) return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(pout + pout_n, b, n);
    pout_n += n;
    return (int)n;
}
static int p_recv(void *c, unsigned char *b, size_t n)
{
    if (!pin_n) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n > pin_n) n = pin_n;
    memcpy(b, pin, n);
    memmove(pin, pin + n, pin_n - n);
    pin_n -= n;
    return (int)n;
}
static int phone_tls_init(const unsigned char *crt, size_t cn, const unsigned char *key, size_t kn)
{
    mbedtls_platform_set_calloc_free(calloc, free);
    mbedtls_ssl_init(&pssl); mbedtls_ssl_config_init(&pconf); mbedtls_x509_crt_init(&pcrt); mbedtls_pk_init(&pkey);
    mbedtls_ctr_drbg_init(&pdrbg); mbedtls_entropy_init(&pent);
    if (mbedtls_ctr_drbg_seed(&pdrbg, mbedtls_entropy_func, &pent, (const unsigned char *)"phone", 5)) return -1;
    if (mbedtls_x509_crt_parse(&pcrt, crt, cn)) return -2;
    if (mbedtls_pk_parse_key(&pkey, key, kn, 0, 0, mbedtls_ctr_drbg_random, &pdrbg)) return -3;
    if (mbedtls_ssl_config_defaults(&pconf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) return -4;
    mbedtls_ssl_conf_authmode(&pconf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&pconf, mbedtls_ctr_drbg_random, &pdrbg);
    if (mbedtls_ssl_conf_own_cert(&pconf, &pcrt, &pkey)) return -5;
    if (mbedtls_ssl_setup(&pssl, &pconf)) return -6;
    mbedtls_ssl_set_bio(&pssl, 0, p_send, p_recv, 0);
    return 0;
}

/* ---- the phone's frames ---- */
static struct tcp_pcb *ph;
static void ph_frame(int ch, int flags, const unsigned char *p, int n)
{
    unsigned char h[4] = { (unsigned char)ch, (unsigned char)flags, (unsigned char)(n >> 8), (unsigned char)n };
    tcp_write(ph, h, 4, TCP_WRITE_FLAG_COPY);
    tcp_write(ph, p, (u16_t)n, TCP_WRITE_FLAG_COPY);
    tcp_output(ph);
}
static void ph_plain(int id, const unsigned char *body, int n)   /* control channel, not encrypted */
{
    unsigned char m[4096] = { (unsigned char)(id >> 8), (unsigned char)id };
    memcpy(m + 2, body, (size_t)n);
    ph_frame(0, 3, m, n + 2);
}
static void ph_enc(int ch, int ctrl, int id, const unsigned char *body, int n)
{
    unsigned char m[256] = { (unsigned char)(id >> 8), (unsigned char)id };
    memcpy(m + 2, body, (size_t)n);
    if (mbedtls_ssl_write(&pssl, m, (size_t)n + 2) != n + 2) { printf("FAIL phone TLS write\n"); exit(1); }
    ph_frame(ch, 8 | 3 | (ctrl ? 4 : 0), pout, (int)pout_n);
    pout_n = 0;
}

/* ---- what the head unit sent: the handshake is answered here, decrypted messages are logged for main ---- */
static unsigned char got[65536];
static int gotn, stage, authed;
static struct { int ch, flags, id, n; unsigned char b[1024]; } hm[16];
static int hm_n;

static void hu_frame(int ch, int flags, const unsigned char *p, int n)
{
    if (!(flags & 8)) {
        int id = n >= 2 ? p[0] << 8 | p[1] : -1;
        if (stage == 0 && ch == 0 && id == 1) {
            static const unsigned char vreq[] = { 0, 1, 0, 1, 0, 1 };
            check(n == 6 && !memcmp(p, vreq, 6), "version request over TCP");
            static const unsigned char vresp[] = { 0, 1, 0, 1, 0, 0 };
            ph_plain(2, vresp, sizeof vresp);
            stage = 1;
        } else if (ch == 0 && id == 3) {
            if (stage == 1) {
                char t[96];
                snprintf(t, sizeof t, "TLS ClientHello inside SSL message 3 (%d bytes)", n);
                check(p[2] == 0x16 && p[3] == 3 && n > 40, t);
                stage = 2;
            }
            memcpy(pin + pin_n, p + 2, (size_t)n - 2);
            pin_n += (size_t)n - 2;
            int r = mbedtls_ssl_handshake(&pssl);
            if (r && r != MBEDTLS_ERR_SSL_WANT_READ) { printf("FAIL phone TLS handshake -0x%04x\n", -r); exit(1); }
            if (pout_n) ph_plain(3, pout, (int)pout_n), pout_n = 0;
        } else if (ch == 0 && id == 4) {
            check(stage == 2 && mbedtls_ssl_is_handshake_over(&pssl) && n == 4 && p[2] == 8 && p[3] == 0,
                  "TLS handshake done, auth complete (status 0)");
            authed = 1;
        } else {
            printf("FAIL unexpected plain message %d on channel %d\n", id, ch);
            fails++;
        }
        return;
    }
    memcpy(pin + pin_n, p, (size_t)n);
    pin_n += (size_t)n;
    for (;;) {
        unsigned char m[4096];
        int k = mbedtls_ssl_read(&pssl, m, sizeof m);
        if (k == MBEDTLS_ERR_SSL_WANT_READ) break;
        if (k < 2) { printf("FAIL phone TLS read -0x%04x\n", -k); exit(1); }
        if (hm_n == 16) { printf("FAIL message log full\n"); exit(1); }
        hm[hm_n].ch = ch, hm[hm_n].flags = flags, hm[hm_n].id = m[0] << 8 | m[1], hm[hm_n].n = k - 2;
        memcpy(hm[hm_n].b, m + 2, k - 2 < 1024 ? (size_t)k - 2 : 1024);
        hm_n++;
    }
}

static void phone_poll(void)
{
    while (gotn >= 4) {
        int hdr = (got[1] & 3) == 1 ? 8 : 4, size = got[2] << 8 | got[3];
        if (gotn < hdr + size) break;
        hu_frame(got[0], got[1], got + hdr, size);
        memmove(got, got + hdr + size, (size_t)(gotn - hdr - size));
        gotn -= hdr + size;
    }
}

static err_t ph_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (!p) return ERR_OK;
    gotn += pbuf_copy_partial(p, got + gotn, p->tot_len, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static err_t ph_conn(void *arg, struct tcp_pcb *pcb, err_t err) { tcp_recv(pcb, ph_recv); return ERR_OK; }

static void step(void)
{
    while (qh != qt) {
        struct netif *to = q[qh].to ? &hu_if : &phone_if;
        struct pbuf *x = pbuf_alloc(PBUF_RAW, (u16_t)q[qh].len, PBUF_POOL);
        pbuf_take(x, q[qh].d, (u16_t)q[qh].len);
        if (to->input(x, to) != ERR_OK) pbuf_free(x);
        qh = (qh + 1) % QN;
    }
    phone_poll();
    now_ms++;
    sys_check_timeouts();
}
/* run the network until the head unit has sent k decrypted messages (or a while has passed) */
static int wait_msgs(int k)
{
    for (int i = 0; i < 20000 && hm_n < k; i++) step();
    return hm_n >= k;
}

/* ---- microphone platform hooks ---- */
static unsigned long long fake_us = 0x0102030405060708ull;
static unsigned long long now_us(void) { return fake_us; }
static int mic_calls, mic_state = -1;
static void mic_cb(int open) { mic_calls++, mic_state = open; }

/* message i is id on channel ch, a non-control encrypted whole frame, with body want[0..wn) */
static int msg_is(int i, int ch, int id, const unsigned char *want, int wn)
{
    return i < hm_n && hm[i].ch == ch && hm[i].flags == 0x0b && hm[i].id == id && hm[i].n == wn && !memcmp(hm[i].b, want, (size_t)wn);
}
/* message i is a microphone data frame: timestamp ts (u64be) and the PCM pattern starting at seed */
static int mic_frame_is(int i, unsigned long long ts, int seed, int n)
{
    if (i >= hm_n || hm[i].ch != 7 || hm[i].flags != 0x0b || hm[i].id != 0 || hm[i].n != 8 + n) return 0;
    for (int k = 0; k < 8; k++)
        if (hm[i].b[k] != (unsigned char)(ts >> (56 - 8 * k))) return 0;
    for (int k = 0; k < n; k++)
        if (hm[i].b[8 + k] != (unsigned char)(seed + k)) return 0;
    return 1;
}

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b = malloc(16384);
    *n = fread(b, 1, 16383, f);
    b[(*n)++] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    lwip_init();
    ip4_addr_t a, b, m, g;
    IP4_ADDR(&a, 192, 168, 43, 100); IP4_ADDR(&b, 192, 168, 43, 1); IP4_ADDR(&m, 255, 255, 255, 0); IP4_ADDR(&g, 0, 0, 0, 0);
    netif_add(&phone_if, &a, &m, &g, 0, nif_init, ethernet_input);
    netif_add(&hu_if, &b, &m, &g, 0, nif_init, ethernet_input);
    memcpy(phone_if.hwaddr, "\x02\0\0\0\0\1", 6); memcpy(hu_if.hwaddr, "\x02\0\0\0\0\2", 6);
    netif_set_up(&phone_if); netif_set_up(&hu_if); netif_set_link_up(&phone_if); netif_set_link_up(&hu_if);
    struct aa_platform p = { 0 };
    p.cert = slurp(argv[1], &p.cert_len);
    p.key = slurp(argv[2], &p.key_len);
    p.now_us = now_us;
    p.mic = mic_cb;
    p.audio_mask = 0x80;   /* a microphone source, no sinks */
    if (phone_tls_init(p.cert, p.cert_len, p.key, p.key_len)) { printf("FAIL phone TLS init\n"); return 1; }
    if (aa_tcp_listen(&p, &hu_if, 5288)) { printf("FAIL listen\n"); return 1; }
    ph = tcp_new();
    tcp_bind_netif(ph, &phone_if);
    tcp_connect(ph, &b, 5288, ph_conn);
    for (int i = 0; i < 200000 && !authed; i++)
        step();
    char t[128];
    snprintf(t, sizeof t, "head unit authenticated over TCP (state %d)", aa_status()->state);
    check(authed && aa_status()->state == AA_AUTHED && aa_tcp_connected(), t);
    if (!authed) { printf("%d failures\n", fails); return 1; }

    /* service discovery: the microphone is offered as channel 7 with an AV input descriptor (field 5) */
    ph_enc(0, 0, 5, (const unsigned char *)"", 0);
    wait_msgs(1);
    int mic_desc = 0;
    for (int k = 0; hm_n && k + 3 <= hm[0].n; k++)
        mic_desc |= hm[0].b[k] == 0x08 && hm[0].b[k + 1] == 7 && hm[0].b[k + 2] == 0x2a;
    check(hm_n >= 1 && hm[0].ch == 0 && hm[0].id == 6 && mic_desc, "service discovery response offers the microphone on channel 7");

    /* open video (makes the session running) and the microphone */
    static const unsigned char open3[] = { 0x08, 0, 0x10, 3 }, open7[] = { 0x08, 0, 0x10, 7 };
    ph_enc(3, 1, 7, open3, sizeof open3);
    ph_enc(7, 1, 7, open7, sizeof open7);
    wait_msgs(3);
    check(hm_n >= 3 && hm[1].id == 8 && hm[1].ch == 3 && hm[2].id == 8 && hm[2].ch == 7 && (hm[2].flags & 4) &&
          aa_status()->state == AA_RUNNING, "video and microphone channels open, session running");

    static const unsigned char pcm0[1] = { 0 };
    check(aa_send_mic(pcm0, 1) == 0 && aa_status()->mic_drops == 1 && aa_status()->mic_frames == 0, "aa_send_mic refused while the mic is closed");

    /* setup -> 0x8003 {1: 2 ready, 2: 1 in flight, 3: 0} */
    ph_enc(7, 0, 0x8000, (const unsigned char *)"\x08\x01", 2);
    wait_msgs(4);
    static const unsigned char setup_resp[] = { 0x08, 2, 0x10, 1, 0x18, 0 };
    check(msg_is(3, 7, 0x8003, setup_resp, sizeof setup_resp), "mic setup answered 0x8003 {1:2, 2:1, 3:0}");

    /* MicrophoneRequest open, anc 0, ec 0, max_unacked 2 -> 0x8006 {1:0, 2:0} */
    static const unsigned char mic_open[] = { 0x08, 1, 0x10, 0, 0x18, 0, 0x20, 2 }, mic_resp[] = { 0x08, 0, 0x10, 0 };
    ph_enc(7, 0, 0x8005, mic_open, sizeof mic_open);
    wait_msgs(5);
    check(msg_is(4, 7, 0x8006, mic_resp, sizeof mic_resp), "MicrophoneRequest open answered 0x8006 {1:0, 2:0}");
    check(mic_calls == 1 && mic_state == 1 && aa_status()->mic_opens == 1, "platform mic(1) called once");

    /* two frames go out with their timestamps, the third is refused (window 2) */
    unsigned char pcm[640];
    for (int k = 0; k < 640; k++) pcm[k] = (unsigned char)k;
    unsigned long long ts1 = fake_us;
    int r1 = aa_send_mic(pcm, 640);
    fake_us += 20000;
    unsigned long long ts2 = fake_us;
    for (int k = 0; k < 640; k++) pcm[k] = (unsigned char)(k + 1);
    int r2 = aa_send_mic(pcm, 640);
    fake_us += 20000;
    int r3 = aa_send_mic(pcm, 640);
    check(r1 == 640 && r2 == 640 && r3 == 0 && aa_status()->mic_frames == 2 && aa_status()->mic_drops == 2,
          "aa_send_mic sends two frames, refuses a third unacked one");
    wait_msgs(7);
    check(mic_frame_is(5, ts1, 0, 640) && mic_frame_is(6, ts2, 1, 640), "mic frames: id 0, u64be microsecond timestamp, PCM");

    /* the phone acks one frame: one more may go, then the window is full again */
    static const unsigned char ack1[] = { 0x08, 0, 0x10, 1 };
    ph_enc(7, 0, 0x8004, ack1, sizeof ack1);
    for (int i = 0; i < 20000 && aa_status()->mic_acks < 1; i++) step();
    unsigned long long ts3 = fake_us;
    for (int k = 0; k < 640; k++) pcm[k] = (unsigned char)(k + 2);
    int r4 = aa_send_mic(pcm, 640), r5 = aa_send_mic(pcm, 640);
    check(aa_status()->mic_acks == 1 && r4 == 640 && r5 == 0 && aa_status()->mic_drops == 3, "after an ack one more frame goes, then the window is full");
    wait_msgs(8);
    check(mic_frame_is(7, ts3, 2, 640), "mic frame after the ack");

    /* close -> 0x8006, mic(0), and nothing more is sent */
    ph_enc(7, 0, 0x8005, (const unsigned char *)"\x08\x00", 2);
    wait_msgs(9);
    check(msg_is(8, 7, 0x8006, mic_resp, sizeof mic_resp), "MicrophoneRequest close answered 0x8006 {1:0, 2:0}");
    check(mic_calls == 2 && mic_state == 0 && aa_send_mic(pcm, 640) == 0 && aa_status()->mic_drops == 4,
          "platform mic(0) called, aa_send_mic refused after the close");
    snprintf(t, sizeof t, "no unknown messages, no errors (unknown %u, errors %u)", aa_status()->unknown, aa_status()->errors);
    check(aa_status()->unknown == 0 && aa_status()->errors == 0, t);

    /* a session that ends with the mic open stops the capture */
    ph_enc(7, 0, 0x8005, mic_open, sizeof mic_open);
    wait_msgs(10);
    aa_closed();
    check(mic_calls == 4 && mic_state == 0 && aa_status()->mic_opens == 2, "transport close with the mic open calls mic(0)");
    printf("%d failures\n", fails);
    return fails != 0;
}
