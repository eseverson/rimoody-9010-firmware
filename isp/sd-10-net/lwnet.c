/* lwIP on the 802.11 data path (notes/android_auto.md "Wireless only"), net task only. lwIP takes TCP and ARP;
 * ip.c keeps everything else (HU1 on UDP, ICMP, DHCP client/server) and owns the address, which it hands over through
 * lw_set_addr. Frames arrive as (src, dst, ethertype, payload) like ip_rx and leave through plat_eth_send.
 * Test service: TCP 5300 counts and discards what it receives (reports the bytes and time when the peer closes),
 * TCP 5301 sends LW_SRC_BYTES of a known pattern then closes (test/tcp_test.py checks both). */
#include "lwnet.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "clk.h"
#include <string.h>

/* from ip.h (not included: lwIP defines an ip_init() macro that collides with ours) */
void plat_eth_send(const unsigned char dst[6], int type, const unsigned char *p, int len);
unsigned int plat_rand(void);
void plat_logf(const char *fmt, ...);
typedef u32_t u32;

static struct netif nif;
static int ready;
struct lw_stats lw_stats;

u32_t sys_now(void) { return clk_ms(); }
unsigned int lwip_rand(void) { return plat_rand(); }
void lwip_assert(const char *m, const char *f, int l) { plat_logf("lwip assert: %s %s:%d\n", m, f, l); }

static err_t linkout(struct netif *n, struct pbuf *p)
{
    (void)n;
    static unsigned char f[1600];
    int len = pbuf_copy_partial(p, f, sizeof f, 0);
    if (len < 14)
        return ERR_OK;
    plat_eth_send(f, f[12] << 8 | f[13], f + 14, len - 14);
    lw_stats.tx++;
    return ERR_OK;
}

static err_t nif_init(struct netif *n)
{
    n->linkoutput = linkout;
    n->output = etharp_output;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    n->name[0] = 'w', n->name[1] = 'l';
    return ERR_OK;
}

/* ---- test service ---- */
#define LW_SRC_BYTES (8u << 20)
static u32 sink_bytes, sink_t0, src_sent;

static err_t sink_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg, (void)err;
    if (!p) {
        u32 ms = clk_ms() - sink_t0;
        plat_logf("lwnet: TCP 5300 received %u bytes in %u ms (%u kB/s)\n", sink_bytes, ms, ms ? sink_bytes / ms : 0);
        lw_stats.sink_bytes = sink_bytes, lw_stats.sink_ms = ms;
        tcp_recv(pcb, 0);
        tcp_close(pcb);
        return ERR_OK;
    }
    sink_bytes += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t sink_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg, (void)err;
    sink_bytes = 0, sink_t0 = clk_ms();
    tcp_recv(pcb, sink_recv);
    return ERR_OK;
}

static void src_pump(struct tcp_pcb *pcb)
{
    static unsigned char chunk[1460];
    while (src_sent < LW_SRC_BYTES) {
        u32 n = tcp_sndbuf(pcb);
        if (n > sizeof chunk)
            n = sizeof chunk;
        if (n > LW_SRC_BYTES - src_sent)
            n = LW_SRC_BYTES - src_sent;
        if (!n)
            break;
        for (u32 i = 0; i < n; i++)
            chunk[i] = (unsigned char)((src_sent + i) * 7 + ((src_sent + i) >> 11));
        if (tcp_write(pcb, chunk, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)
            break;
        src_sent += n;
    }
    tcp_output(pcb);
    if (src_sent >= LW_SRC_BYTES && !tcp_sndqueuelen(pcb)) {
        tcp_sent(pcb, 0);
        tcp_close(pcb);
    }
}

static err_t src_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg, (void)len;
    src_pump(pcb);
    return ERR_OK;
}

static err_t src_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg, (void)err;
    src_sent = 0;
    tcp_sent(pcb, src_sent_cb);
    src_pump(pcb);
    return ERR_OK;
}

static void listen_on(int port, tcp_accept_fn fn)
{
    struct tcp_pcb *l = tcp_new();
    if (l && tcp_bind(l, IP_ADDR_ANY, (u16_t)port) == ERR_OK) {
        l = tcp_listen(l);
        tcp_accept(l, fn);
    }
}

void lw_init(const unsigned char mac[6])
{
    lwip_init();
    netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, 0, nif_init, ethernet_input);
    memcpy(nif.hwaddr, mac, 6);
    netif_set_default(&nif);
    netif_set_up(&nif);
    listen_on(5300, sink_accept);
    listen_on(5301, src_accept);
    ready = 1;
}

void lw_set_addr(unsigned int ip, unsigned int mask, unsigned int gw)   /* host byte order, 0 = down */
{
    if (!ready)
        return;
    ip4_addr_t a, m, g;
    ip4_addr_set_u32(&a, lwip_htonl(ip));
    ip4_addr_set_u32(&m, lwip_htonl(mask));
    ip4_addr_set_u32(&g, lwip_htonl(gw));
    netif_set_addr(&nif, &a, &m, &g);
    if (ip)
        netif_set_link_up(&nif);
    else
        netif_set_link_down(&nif);
}

int lw_wants(int type, const unsigned char *p, int len)
{
    if (!ready)
        return 0;
    if (type == 0x0806)
        return 1;
    return type == 0x0800 && len >= 20 && p[9] == 6;   /* TCP */
}

void lw_input(const unsigned char *src, const unsigned char *dst, int type, const unsigned char *p, int len)
{
    if (!ready || len > 1500)
        return;
    struct pbuf *q = pbuf_alloc(PBUF_RAW, (u16_t)(len + 14), PBUF_POOL);
    if (!q) {
        lw_stats.drops++;
        return;
    }
    unsigned char h[14];
    memcpy(h, dst, 6);
    memcpy(h + 6, src, 6);
    h[12] = (unsigned char)(type >> 8), h[13] = (unsigned char)type;
    pbuf_take(q, h, 14);
    pbuf_take_at(q, p, (u16_t)len, 14);
    lw_stats.rx++;
    if (nif.input(q, &nif) != ERR_OK)
        pbuf_free(q);
}

void lw_poll(void)
{
    if (ready)
        sys_check_timeouts();
}
