/* Host test: aa.c over aa_tcp.c over lwIP, with a scripted phone on a second netif. Checks the version request,
 * answers it, and checks that the head unit then starts TLS (a ClientHello inside SSL message 3).
 * Usage: aatcp_host CERT.pem KEY.pem (throwaway pair, made by test_aatcp.sh). */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "../aa.h"
#include "../aa_tcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static unsigned char got[8192];
static int gotn, stage, fails;
static struct tcp_pcb *ph;

static err_t ph_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (!p) return ERR_OK;
    gotn += pbuf_copy_partial(p, got + gotn, p->tot_len, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (stage == 0 && gotn >= 10) {
        static const unsigned char vreq[] = { 0, 3, 0, 6, 0, 1, 0, 1, 0, 1 };
        int ok = !memcmp(got, vreq, 10);
        printf("%s version request over TCP\n", ok ? "PASS" : "FAIL");
        fails += !ok;
        static unsigned char vresp[] = { 0, 3, 0, 8, 0, 2, 0, 1, 0, 1, 0, 0 };
        tcp_write(pcb, vresp, sizeof vresp, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        memmove(got, got + 10, (size_t)(gotn - 10));
        gotn -= 10;
        stage = 1;
    }
    if (stage == 1 && gotn >= 9) {
        int len = got[2] << 8 | got[3];
        int ok = got[0] == 0 && (got[1] & 3) == 3 && got[4] == 0 && got[5] == 3 && got[6] == 0x16 && got[7] == 3 && len > 40;
        printf("%s TLS ClientHello inside SSL message 3 (%d bytes)\n", ok ? "PASS" : "FAIL", len);
        fails += !ok;
        stage = 2;
    }
    return ERR_OK;
}
static err_t ph_conn(void *arg, struct tcp_pcb *pcb, err_t err) { tcp_recv(pcb, ph_recv); return ERR_OK; }

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
    if (aa_tcp_listen(&p, &hu_if, 5288)) { printf("FAIL listen\n"); return 1; }
    ph = tcp_new();
    tcp_bind_netif(ph, &phone_if);
    tcp_connect(ph, &b, 5288, ph_conn);
    for (int i = 0; i < 200000 && stage < 2; i++) {
        while (qh != qt) {
            struct netif *to = q[qh].to ? &hu_if : &phone_if;
            struct pbuf *x = pbuf_alloc(PBUF_RAW, (u16_t)q[qh].len, PBUF_POOL);
            pbuf_take(x, q[qh].d, (u16_t)q[qh].len);
            if (to->input(x, to) != ERR_OK) pbuf_free(x);
            qh = (qh + 1) % QN;
        }
        now_ms++;
        sys_check_timeouts();
    }
    int ok = stage == 2 && aa_status()->state == AA_TLS && aa_tcp_connected();
    printf("%s head unit in TLS state over TCP (state %d)\n", ok ? "PASS" : "FAIL", aa_status()->state);
    fails += !ok;
    printf("%d failures\n", fails);
    return fails != 0;
}
