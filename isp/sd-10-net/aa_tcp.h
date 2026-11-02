/* Wireless Android Auto TCP transport (aa_tcp.c): aa.c over lwIP. */
#ifndef AA_TCP_H
#define AA_TCP_H
struct aa_platform;
/* listen on port; p->send is replaced by the TCP sender; netif_or_null pins the listener to one netif (tests). */
int aa_tcp_listen(const struct aa_platform *p, const void *netif_or_null, int port);
/* connect to a phone running the head unit server (ip as a.b.c.d big-endian, port 5277) */
int aa_tcp_connect(const struct aa_platform *p, unsigned int ip_be, int port);
int aa_tcp_connected(void);
unsigned int aa_tcp_tx_drops(void);
void aa_tcp_poll(void);                 /* net loop: push bytes lwIP had no room for earlier */
unsigned int aa_tcp_pending_max(void);  /* most bytes ever waiting */
#endif
