/* Host test: dhcps.c on one netif, lwIP's DHCP client on another, wired back to back: the client must get
 * 192.168.43.2/24 from 192.168.43.1. */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "../dhcps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned int now_ms;
u32_t sys_now(void) { return now_ms; }
unsigned int lwip_rand(void) { return 1234; }
void lwip_assert(const char *m, const char *f, int l) { printf("ASSERT %s %s:%d\n", m, f, l); exit(3); }
#define QN 256
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
int main(void)
{
    lwip_init();
    ip4_addr_t hu, m, z;
    IP4_ADDR(&hu, 192, 168, 43, 1); IP4_ADDR(&m, 255, 255, 255, 0); IP4_ADDR(&z, 0, 0, 0, 0);
    netif_add(&hu_if, &hu, &m, &z, 0, nif_init, ethernet_input);
    netif_add(&phone_if, &z, &z, &z, 0, nif_init, ethernet_input);
    memcpy(phone_if.hwaddr, "\x3a\1\2\3\4\5", 6); memcpy(hu_if.hwaddr, "\x02\x11\x22\x33\x44\x55", 6);
    netif_set_up(&hu_if); netif_set_up(&phone_if); netif_set_link_up(&hu_if); netif_set_link_up(&phone_if);
    if (dhcps_start(&hu_if, &hu, &m)) { printf("FAIL dhcps_start\n"); return 1; }
    dhcp_start(&phone_if);
    for (int i = 0; i < 20000 && !dhcp_supplied_address(&phone_if); i++) {
        while (qh != qt) {
            struct netif *to = q[qh].to ? &hu_if : &phone_if;
            struct pbuf *x = pbuf_alloc(PBUF_RAW, (u16_t)q[qh].len, PBUF_POOL);
            pbuf_take(x, q[qh].d, (u16_t)q[qh].len);
            if (to->input(x, to) != ERR_OK) pbuf_free(x);
            qh = (qh + 1) % QN;
        }
        now_ms += 10;
        sys_check_timeouts();
    }
    const ip4_addr_t *got = netif_ip4_addr(&phone_if), *gm = netif_ip4_netmask(&phone_if);
    int ok = dhcp_supplied_address(&phone_if) && ip4_addr_get_u32(got) == PP_HTONL(LWIP_MAKEU32(192, 168, 43, 2)) &&
             ip4_addr_get_u32(gm) == PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0));
    printf("%s DHCP: client got %s/%s after %u ms, %u replies served\n", ok ? "PASS" : "FAIL", ip4addr_ntoa(got),
           ok ? "24" : "?", now_ms, dhcps_served());
    return !ok;
}
