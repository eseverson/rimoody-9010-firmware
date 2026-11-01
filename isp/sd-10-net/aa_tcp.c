/* Wireless Android Auto TCP transport: aa.c over an lwIP raw-API connection (net task only). The phone connects to
 * the ip:port that aa_wl.c handed it over Bluetooth; one session at a time. */
#include "aa_tcp.h"
#include "aa.h"
#include "lwip/tcp.h"
#include <string.h>

static struct tcp_pcb *listener, *conn;
static struct aa_platform plat;
static unsigned int tx_drops;

static int tcp_send_cb(const unsigned char *p, int n)
{
    if (!conn)
        return -1;
    if (tcp_sndbuf(conn) < (unsigned)n || tcp_write(conn, p, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        tx_drops++;
        return -1;
    }
    tcp_output(conn);
    return n;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg, (void)err;
    if (!p) {   /* phone closed */
        tcp_arg(pcb, 0);
        tcp_recv(pcb, 0);
        tcp_close(pcb);
        if (pcb == conn) {
            conn = 0;
            aa_closed();
        }
        return ERR_OK;
    }
    for (struct pbuf *q = p; q; q = q->next)
        aa_rx(q->payload, q->len);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg, (void)err;
    conn = 0;   /* pcb already freed by lwIP */
    aa_closed();
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || conn) {   /* one session at a time */
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    conn = pcb;
    tcp_nagle_disable(pcb);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    aa_start(&plat);   /* version request goes out right away */
    return ERR_OK;
}

int aa_tcp_listen(const struct aa_platform *p, const void *netif_or_null, int port)
{
    plat = *p;
    plat.send = tcp_send_cb;
    listener = tcp_new();
    if (!listener)
        return -1;
    if (netif_or_null)
        tcp_bind_netif(listener, (const struct netif *)netif_or_null);
    if (tcp_bind(listener, IP_ADDR_ANY, (u16_t)port) != ERR_OK)
        return -2;
    listener = tcp_listen(listener);
    tcp_accept(listener, on_accept);
    return 0;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK)
        return err;
    conn = pcb;
    tcp_nagle_disable(pcb);
    tcp_recv(pcb, on_recv);
    aa_start(&plat);
    return ERR_OK;
}

/* Head-unit-server mode (Android Auto developer settings, "Start head unit server"): the phone listens (port 5277)
 * and the head unit connects to it; the protocol on top is the same (we are still the TLS client). */
int aa_tcp_connect(const struct aa_platform *p, unsigned int ip_be, int port)
{
    if (conn)
        return -3;
    plat = *p;
    plat.send = tcp_send_cb;
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb)
        return -1;
    ip_addr_t a;
    IP4_ADDR(&a, ip_be >> 24 & 0xff, ip_be >> 16 & 0xff, ip_be >> 8 & 0xff, ip_be & 0xff);
    tcp_err(pcb, on_err);
    if (tcp_connect(pcb, &a, (u16_t)port, on_connected) != ERR_OK) {
        tcp_abort(pcb);
        return -2;
    }
    return 0;
}

int aa_tcp_connected(void) { return conn != 0; }
unsigned int aa_tcp_tx_drops(void) { return tx_drops; }
