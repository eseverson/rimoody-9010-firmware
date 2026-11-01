/* Host test of our lwIP configuration (lwip/lwipopts.h): two netifs wired back to back in memory, a TCP transfer of
 * 8 MB from A to B with 1% frame loss, checked byte for byte; simulated clock. Exit 0 on success. */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int now_ms;
u32_t sys_now(void) { return now_ms; }
unsigned int lwip_rand(void) { return (unsigned)rand(); }
void lwip_assert(const char *m, const char *f, int l) { printf("ASSERT %s %s:%d\n", m, f, l); exit(3); }

#define QN 4096
static struct { int to; int len; unsigned char d[1600]; } q[QN];
static int qh, qt, dropped;
static struct netif na, nb;

static int outs;
static err_t out(struct netif *n, struct pbuf *p)
{
    outs++;
    if ((qt + 1) % QN == qh)
        return ERR_MEM;
    if (rand() % 100 == 0) { dropped++; return ERR_OK; }
    q[qt].to = n == &na ? 1 : 0;
    q[qt].len = pbuf_copy_partial(p, q[qt].d, p->tot_len, 0);
    qt = (qt + 1) % QN;
    return ERR_OK;
}

static err_t nif_init(struct netif *n)
{
    n->linkoutput = out;
    n->output = etharp_output;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

#define TOTAL (8u << 20)
static unsigned int sent, rcvd, bad;

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (!p) { tcp_close(pcb); return ERR_OK; }
    for (struct pbuf *x = p; x; x = x->next)
        for (u16_t i = 0; i < x->len; i++, rcvd++)
            if (((unsigned char *)x->payload)[i] != (unsigned char)(rcvd * 7 + (rcvd >> 11)))
                bad++;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) { tcp_recv(pcb, on_recv); return ERR_OK; }

static struct tcp_pcb *cli;
static void pump_send(void)
{
    static unsigned char chunk[4096];
    while (sent < TOTAL) {
        unsigned int n = tcp_sndbuf(cli);
        if (n > sizeof chunk) n = sizeof chunk;
        if (n > TOTAL - sent) n = TOTAL - sent;
        if (!n) break;
        for (unsigned int i = 0; i < n; i++)
            chunk[i] = (unsigned char)((sent + i) * 7 + ((sent + i) >> 11));
        if (tcp_write(cli, chunk, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        sent += n;
    }
    tcp_output(cli);
}
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) { pump_send(); return ERR_OK; }
static int connected;
static err_t on_conn(void *arg, struct tcp_pcb *pcb, err_t err) { connected = 1; pump_send(); return ERR_OK; }

int main(void)
{
    srand(1);
    lwip_init();
    ip4_addr_t a, b, m, g;
    IP4_ADDR(&a, 10, 0, 0, 1); IP4_ADDR(&b, 10, 0, 0, 2); IP4_ADDR(&m, 255, 255, 255, 0); IP4_ADDR(&g, 0, 0, 0, 0);
    netif_add(&na, &a, &m, &g, 0, nif_init, ethernet_input);
    netif_add(&nb, &b, &m, &g, 0, nif_init, ethernet_input);
    memcpy(na.hwaddr, "\x02\0\0\0\0\1", 6); memcpy(nb.hwaddr, "\x02\0\0\0\0\2", 6);
    netif_set_up(&na); netif_set_up(&nb);
    netif_set_link_up(&na); netif_set_link_up(&nb);
    struct tcp_pcb *l = tcp_new();
    tcp_bind(l, &b, 5288);
    tcp_bind_netif(l, &nb);   /* one stack, two netifs on one subnet: pin each side to its own netif */
    l = tcp_listen(l);
    tcp_accept(l, on_accept);
    cli = tcp_new();
    tcp_bind_netif(cli, &na);
    tcp_sent(cli, on_sent);
    err_t ce = tcp_connect(cli, &b, 5288, on_conn);
    if (ce) { printf("FAIL tcp_connect %d\n", ce); return 1; }
    for (int step = 0; step < 2000000 && rcvd < TOTAL; step++) {
        int n = 0;
        while (qh != qt && n++ < 64) {
            struct netif *to = q[qh].to ? &nb : &na;
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)q[qh].len, PBUF_POOL);
            if (p) { pbuf_take(p, q[qh].d, (u16_t)q[qh].len); if (to->input(p, to) != ERR_OK) pbuf_free(p); }
            qh = (qh + 1) % QN;
        }
        if (qh == qt) now_ms += 1;   /* time passes when the wire is idle */
        sys_check_timeouts();
        if (connected) pump_send();
    }
    printf("%s lwIP TCP: %u of %u bytes received, %u bad, %d frames dropped, %u ms simulated\n",
           rcvd == TOTAL && !bad ? "PASS" : "FAIL", rcvd, TOTAL, bad, dropped, now_ms);
    return rcvd == TOTAL && !bad ? 0 : 1;
}
