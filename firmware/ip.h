/* Minimal IPv4 for the head-unit payload: ARP replies, ICMP echo, DHCP client with static fallback, a hello broadcast
 * and the HU1 command server on UDP 5151. Works on Ethernet-style frames (dst/src MAC + ethertype); the platform turns
 * them into 802.11 data frames. Platform independent (test/ip_host.c builds it on the host). */
#ifndef IP_H
#define IP_H
#include "crypto.h"

#define HU_PORT       5151
#define HU_HELLO_PORT 5152
#define HU_MAX_DATA   1200
#define HU_MAX_REPLY  65536

void ip_init(const cu8 mac[6], const cu8 token[32], cu32 static_ip, cu8 *rbuf);   /* rbuf: HU_MAX_REPLY + 256 */
void ip_link_up(void);     /* associated with keys: start DHCP (or keep the address) */
void ip_link_down(void);
void ip_ap_start(cu32 ip);   /* we are the access point: static ip/24, DHCP server for the stations (ip+1..ip+4) */
void ip_rx(const cu8 *src, const cu8 *dst, int type, const cu8 *p, int len);
void ip_poll(void);
cu32 ip_addr(void);
int ip_hu1_on(void);       /* a token is configured (ip_init); without one the command server drops every request */
int ip_fmt_addr(char *out, cu32 ip);
extern int ip_stats[8];   /* rx, tx, arp, icmp, udp, cmd, badtoken, resent */

/* platform */
void plat_eth_send(const cu8 dst[6], int type, const cu8 *p, int len);
cu32 plat_ms(void);
cu32 plat_rand(void);
void plat_logf(const char *fmt, ...);
int plat_fmt(char *out, const char *fmt, ...);   /* sprintf with %s %c %d %u %x, 0/width; returns length */
int plat_cmd(const char *cmd, cu8 *out, int max);   /* run a U-Boot command, output (+ rc line) into out */
int plat_mem_ok(cu32 addr, cu32 n, int write);
void plat_mem_read(cu32 addr, cu8 *out, int n);     /* 32-bit word reads, n a multiple of 4 */
void plat_mem_write(cu32 addr, const cu8 *p, int n);
void plat_sync(cu32 addr, cu32 n);
int plat_log_tail(cu8 *out, int max);
int plat_status(char *out);   /* "up=<ms> hb=<n> mcurx=<n> rssi=<dbm>" */
void plat_bssid(cu8 out[6]);
void plat_ip_event(cu32 ip, cu32 mask, cu32 router, int how);   /* how: 1 DHCP, 2 static, 0 lost */
int plat_op(int op, const cu8 *p, int n, cu8 *out, int max);   /* ops >= 7: reply length, -1 = unknown op */
const char *plat_build(void);
int plat_hello_extra(char *out);   /* appended to the hello broadcast (e.g. " crash=..."), returns its length */   /* build id, e.g. 20261031-153000 */

#endif
