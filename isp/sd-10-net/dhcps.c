/* Minimal DHCP server for the wireless Android Auto access point (lwIP raw UDP, net task): hands out a small range
 * after our own address, answers DISCOVER with OFFER and REQUEST with ACK (or NAK for an address it did not offer),
 * no router or DNS (the phone only talks to us), lease 1 day. Leases are remembered by MAC. */
#include "dhcps.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include <string.h>

#define NLEASE 4
static struct udp_pcb *pcb;
static ip4_addr_t me, mask;
static struct { unsigned char mac[6]; int used; } lease[NLEASE];
static unsigned int served;

static int lease_for(const unsigned char *mac)
{
    for (int i = 0; i < NLEASE; i++)
        if (lease[i].used && !memcmp(lease[i].mac, mac, 6))
            return i;
    for (int i = 0; i < NLEASE; i++)
        if (!lease[i].used) {
            memcpy(lease[i].mac, mac, 6);
            lease[i].used = 1;
            return i;
        }
    return -1;
}

static u32_t addr_of(int i) { return lwip_htonl(lwip_ntohl(ip4_addr_get_u32(&me)) + 1 + (u32_t)i); }

static const unsigned char *opt(const unsigned char *o, int n, int code)
{
    for (int i = 0; i + 1 < n && o[i] != 255;) {
        if (o[i] == 0) { i++; continue; }
        if (o[i] == code && i + 2 + o[i + 1] <= n)
            return o + i;
        i += 2 + o[i + 1];
    }
    return 0;
}

static void reply(const unsigned char *req, int type, u32_t yiaddr, const ip_addr_t *to, u16_t port)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 300, PBUF_RAM);
    if (!p)
        return;
    unsigned char *b = p->payload;
    memset(b, 0, 300);
    b[0] = 2, b[1] = 1, b[2] = 6;
    memcpy(b + 4, req + 4, 4);          /* xid */
    memcpy(b + 10, req + 10, 2);        /* flags */
    memcpy(b + 16, &yiaddr, 4);
    memcpy(b + 20, &me, 4);             /* siaddr */
    memcpy(b + 28, req + 28, 16);       /* chaddr */
    b[236] = 99, b[237] = 130, b[238] = 83, b[239] = 99;
    unsigned char *o = b + 240;
    *o++ = 53, *o++ = 1, *o++ = (unsigned char)type;
    *o++ = 54, *o++ = 4, memcpy(o, &me, 4), o += 4;
    if (type != 6) {
        *o++ = 51, *o++ = 4, *o++ = 0, *o++ = 1, *o++ = 0x51, *o++ = 0x80;   /* 86400 s */
        *o++ = 1, *o++ = 4, memcpy(o, &mask, 4), o += 4;
    }
    *o++ = 255;
    udp_sendto(pcb, p, to, port);
    pbuf_free(p);
    served++;
}

static void on_rx(void *arg, struct udp_pcb *u, struct pbuf *p, const ip_addr_t *from, u16_t port)
{
    (void)arg, (void)u, (void)from, (void)port;
    static unsigned char b[600];
    int n = pbuf_copy_partial(p, b, sizeof b, 0);
    pbuf_free(p);
    if (n < 244 || b[0] != 1 || b[236] != 99 || b[237] != 130 || b[238] != 83 || b[239] != 99)
        return;
    const unsigned char *t = opt(b + 240, n - 240, 53);
    if (!t)
        return;
    int i = lease_for(b + 28);
    if (i < 0)
        return;
    u32_t a = addr_of(i);
    ip_addr_t bc;
    ip_addr_set_ip4_u32(&bc, IPADDR_BROADCAST);
    if (t[2] == 1) {   /* DISCOVER -> OFFER */
        reply(b, 2, a, &bc, 68);
    } else if (t[2] == 3) {   /* REQUEST -> ACK if it asks for what we hold for it */
        const unsigned char *r = opt(b + 240, n - 240, 50);
        u32_t want;
        if (r && r[1] == 4)
            memcpy(&want, r + 2, 4);
        else
            memcpy(&want, b + 12, 4);   /* ciaddr (renewing) */
        reply(b, want == a ? 5 : 6, want == a ? a : 0, &bc, 68);
    }
}

int dhcps_start(struct netif *nif, const ip4_addr_t *ip, const ip4_addr_t *netmask)
{
    me = *ip;
    mask = *netmask;
    memset(lease, 0, sizeof lease);
    pcb = udp_new();
    if (!pcb)
        return -1;
    ip_set_option(pcb, SOF_BROADCAST);
    if (nif)
        udp_bind_netif(pcb, nif);   /* replies leave through the AP interface */
    if (udp_bind(pcb, IP_ADDR_ANY, 67) != ERR_OK)
        return -2;
    udp_recv(pcb, on_rx, 0);
    return 0;
}

unsigned int dhcps_served(void) { return served; }
