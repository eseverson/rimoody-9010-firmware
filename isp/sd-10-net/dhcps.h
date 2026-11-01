/* Minimal DHCP server for the AA access point (dhcps.c). */
#ifndef DHCPS_H
#define DHCPS_H
#include "lwip/ip4_addr.h"
struct netif;
int dhcps_start(struct netif *nif, const ip4_addr_t *ip, const ip4_addr_t *netmask);   /* hands out ip+1 .. ip+4 */
unsigned int dhcps_served(void);
#endif
