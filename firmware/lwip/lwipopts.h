/* lwIP configuration for the head unit (notes/android_auto.md, wireless Android Auto): raw API only, called from
 * the net task (NO_SYS: no lwIP threads, the net loop calls sys_check_timeouts), IPv4, TCP sized for an AA video
 * stream of a few Mbit/s, ARP + Ethernet framing on top of the 802.11 data path, DHCP client for station mode
 * (development on the home network); the AP-side DHCP server is our own. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                   1
#define LWIP_SOCKET              0
#define LWIP_NETCONN             0
#define SYS_LIGHTWEIGHT_PROT     0
#define LWIP_IPV4                1
#define LWIP_IPV6                0
#define LWIP_ARP                 1
#define LWIP_ETHERNET            1
#define LWIP_ICMP                1
#define LWIP_UDP                 1
#define LWIP_TCP                 1
#define LWIP_DHCP                1
#define LWIP_AUTOIP              0
#define LWIP_IGMP                0
#define LWIP_DNS                 0
#define LWIP_RAW                 0
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_STATS               0
#define LWIP_TIMERS              1

#define MEM_ALIGNMENT            4
#define MEM_SIZE                 (256 * 1024)
#define MEMP_NUM_PBUF            64
#define MEMP_NUM_TCP_PCB         8
#define MEMP_NUM_TCP_PCB_LISTEN  4
#define MEMP_NUM_TCP_SEG         128
#define MEMP_NUM_UDP_PCB         6
#define PBUF_POOL_SIZE           96
#define PBUF_POOL_BUFSIZE        1600

#define TCP_MSS                  1460
#define TCP_WND                  (32 * TCP_MSS)
#define TCP_SND_BUF              (16 * TCP_MSS)
#define TCP_SND_QUEUELEN         (4 * TCP_SND_BUF / TCP_MSS)
#define LWIP_WND_SCALE           1
#define TCP_RCV_SCALE            2
#define TCP_QUEUE_OOSEQ          1
#define LWIP_TCP_KEEPALIVE       1

#define CHECKSUM_GEN_IP          1
#define CHECKSUM_GEN_UDP         1
#define CHECKSUM_GEN_TCP         1
#define CHECKSUM_CHECK_IP        1
#define CHECKSUM_CHECK_UDP       1
#define CHECKSUM_CHECK_TCP       1

#define LWIP_NOASSERT_ON_ERROR   0
#endif
