/* Wireless Android Auto TCP transport (aa_tcp.c): aa.c over lwIP. */
#ifndef AA_TCP_H
#define AA_TCP_H
struct aa_platform;
/* listen on port; p->send is replaced by the TCP sender; netif_or_null pins the listener to one netif (tests). */
int aa_tcp_listen(const struct aa_platform *p, const void *netif_or_null, int port);
int aa_tcp_connected(void);
unsigned int aa_tcp_tx_drops(void);
#endif
