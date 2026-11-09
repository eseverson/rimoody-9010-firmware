/* lwIP on the 802.11 data path (lwnet.c): TCP + ARP; ip.c keeps the rest. Net task only. */
#ifndef LWNET_H
#define LWNET_H
struct lw_stats { unsigned int rx, tx, drops, sink_bytes, sink_ms; };
extern struct lw_stats lw_stats;
void lw_init(const unsigned char mac[6]);
void lw_set_addr(unsigned int ip, unsigned int mask, unsigned int gw);   /* host byte order; 0 = link down */
int lw_wants(int type, const unsigned char *p, int len);                 /* TCP or ARP: give it to lw_input */
void lw_input(const unsigned char *src, const unsigned char *dst, int type, const unsigned char *p, int len);
void lw_poll(void);
#endif
