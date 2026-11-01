/* See ip.h. HU1 command protocol (little-endian):
 *   request  "HU1" token[32] seq:u32 op:u8 payload
 *   reply    "HU1" seq:u32 (op|0x80):u8 frag:u16 last:u8 data (<= 1200 bytes per datagram)
 *   ops      1 PING, 2 CMD <U-Boot command>, 3 WRITE addr:u32 bytes -> crc32:u32, 4 READ addr:u32 len:u16,
 *            5 SYNC addr:u32 len:u32 (D-cache write-back + I-cache invalidate), 6 LOG (last <= 64 KB of the log),
 *            7.. platform ops (plat_op): 7 MCU_TX, 8 MCU_STATE, 9 SETTIME, 10 EXEC
 * A request with the cached reply's seq gets that reply again instead of running twice; a bad token is dropped. */
#include "ip.h"

int ip_stats[8];
enum { S_RX, S_TX, S_ARP, S_ICMP, S_UDP, S_CMD, S_BADTOKEN, S_RESENT };

static cu8 my_mac[6], tok[32];
static cu32 my_ip, my_mask, my_router, static_ip, dhcp_server, offered_ip, xid;
static cu32 lease_ms, bound_at, dhcp_started, dhcp_last_tx, hello_last;
static int link, bound, dhcp_state, dhcp_tries;   /* bound: 1 DHCP, 2 static */
static cu8 *rbuf;
static cu8 pkt[1600];
static const cu8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

enum { D_IDLE, D_DISCOVER, D_REQUEST, D_BOUND, D_RENEW };

static cu32 be32(const cu8 *p) { return (cu32)p[0] << 24 | (cu32)p[1] << 16 | (cu32)p[2] << 8 | p[3]; }
static int be16(const cu8 *p) { return p[0] << 8 | p[1]; }
static cu32 le32(const cu8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (cu32)p[3] << 24; }
static void put32(cu8 *p, cu32 v) { p[0] = (cu8)(v >> 24), p[1] = (cu8)(v >> 16), p[2] = (cu8)(v >> 8), p[3] = (cu8)v; }
static void put16(cu8 *p, int v) { p[0] = (cu8)(v >> 8), p[1] = (cu8)v; }

static void cpy(cu8 *d, const cu8 *s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static int same(const cu8 *a, const cu8 *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static cu32 sum16(cu32 s, const cu8 *p, int n)
{
    for (int i = 0; i + 1 < n; i += 2)
        s += (cu32)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        s += (cu32)p[n - 1] << 8;
    return s;
}

static int fold(cu32 s)
{
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return ~s & 0xffff;
}

int ip_fmt_addr(char *out, cu32 ip)
{
    return plat_fmt(out, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

cu32 ip_addr(void) { return bound ? my_ip : 0; }

void ip_init(const cu8 mac[6], const cu8 token[32], cu32 sip, cu8 *rb)
{
    cpy(my_mac, mac, 6);
    cpy(tok, token, 32);
    static_ip = sip;
    rbuf = rb;
}

/* IPv4 + UDP around data already placed at pkt + 28 */
static void udp_send(const cu8 dmac[6], cu32 dip, int sport, int dport, int dlen)
{
    static unsigned short id;
    cu8 *ip = pkt, *u = pkt + 20;
    int tot = 28 + dlen;
    ip[0] = 0x45, ip[1] = 0;
    put16(ip + 2, tot);
    put16(ip + 4, ++id);
    ip[6] = 0x40, ip[7] = 0, ip[8] = 64, ip[9] = 17;
    ip[10] = ip[11] = 0;
    put32(ip + 12, bound ? my_ip : 0);
    put32(ip + 16, dip);
    put16(ip + 10, fold(sum16(0, ip, 20)));
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, 8 + dlen);
    u[6] = u[7] = 0;
    cu32 s = sum16(0, ip + 12, 8) + 17 + (cu32)(8 + dlen);
    int c = fold(sum16(s, u, 8 + dlen));
    put16(u + 6, c ? c : 0xffff);
    plat_eth_send(dmac, 0x0800, pkt, tot);
    ip_stats[S_TX]++;
}

/* ---------------- DHCP ---------------- */
static void dhcp_send(int type)
{
    cu8 *d = pkt + 28;
    for (int i = 0; i < 240; i++)
        d[i] = 0;
    d[0] = 1, d[1] = 1, d[2] = 6;
    put32(d + 4, xid);
    put16(d + 8, (int)((plat_ms() - dhcp_started) / 1000));
    if (type == 3 && dhcp_state == D_RENEW)
        put32(d + 12, my_ip);   /* ciaddr */
    else
        d[10] = 0x80;   /* broadcast replies please */
    cpy(d + 28, my_mac, 6);
    put32(d + 236, 0x63825363);
    cu8 *o = d + 240;
    *o++ = 53, *o++ = 1, *o++ = (cu8)type;
    *o++ = 61, *o++ = 7, *o++ = 1;
    cpy(o, my_mac, 6), o += 6;
    if (type == 3 && dhcp_state == D_REQUEST) {
        *o++ = 50, *o++ = 4, put32(o, offered_ip), o += 4;
        *o++ = 54, *o++ = 4, put32(o, dhcp_server), o += 4;
    }
    *o++ = 12, *o++ = 8;
    cpy(o, (const cu8 *)"headunit", 8), o += 8;
    *o++ = 55, *o++ = 4, *o++ = 1, *o++ = 3, *o++ = 51, *o++ = 54;
    *o++ = 255;
    while (o - d < 300)
        *o++ = 0;
    udp_send(bcast, 0xffffffff, 68, 67, (int)(o - d));
    dhcp_last_tx = plat_ms();
    dhcp_tries++;
}

static void dhcp_start(void)
{
    xid = plat_rand();
    dhcp_state = D_DISCOVER;
    dhcp_tries = 0;
    dhcp_started = plat_ms();
    dhcp_send(1);
    plat_logf("dhcp: DISCOVER xid %08x\n", xid);
}

static void bind_addr(cu32 ip, cu32 mask, cu32 router, int how)
{
    my_ip = ip, my_mask = mask, my_router = router, bound = how;
    bound_at = plat_ms();
    char a[16], m[16], r[16];
    ip_fmt_addr(a, ip), ip_fmt_addr(m, mask), ip_fmt_addr(r, router);
    plat_logf("ip: %s mask %s router %s (%s, lease %u s)\n", a, m, r, how == 1 ? "DHCP" : "static", lease_ms / 1000);
    /* gratuitous ARP so neighbours update their caches */
    cu8 *g = pkt;
    put16(g, 1), put16(g + 2, 0x0800), g[4] = 6, g[5] = 4, put16(g + 6, 1);
    cpy(g + 8, my_mac, 6), put32(g + 14, ip);
    for (int i = 18; i < 24; i++)
        g[i] = 0;
    put32(g + 24, ip);
    plat_eth_send(bcast, 0x0806, g, 28);
    plat_ip_event(ip, mask, router, how);
}

static void dhcp_rx(const cu8 *d, int n)
{
    if (n < 240 || d[0] != 2 || be32(d + 4) != xid || !same(d + 28, my_mac, 6) || be32(d + 236) != 0x63825363)
        return;
    int type = 0;
    cu32 server = 0, mask = 0xffffff00, router = 0, lease = 3600;
    for (int i = 240; i < n && d[i] != 255;) {
        if (d[i] == 0) {
            i++;
            continue;
        }
        if (i + 2 > n || i + 2 + d[i + 1] > n)
            break;
        const cu8 *v = d + i + 2;
        int l = d[i + 1];
        if (d[i] == 53 && l >= 1)
            type = v[0];
        else if (d[i] == 54 && l >= 4)
            server = be32(v);
        else if (d[i] == 1 && l >= 4)
            mask = be32(v);
        else if (d[i] == 3 && l >= 4)
            router = be32(v);
        else if (d[i] == 51 && l >= 4)
            lease = be32(v);
        i += 2 + l;
    }
    cu32 yi = be32(d + 16);
    char a[16];
    ip_fmt_addr(a, yi);
    if (type == 2 && dhcp_state == D_DISCOVER) {
        offered_ip = yi, dhcp_server = server;
        dhcp_state = D_REQUEST;
        dhcp_tries = 0;
        plat_logf("dhcp: OFFER %s, REQUEST\n", a);
        dhcp_send(3);
    } else if (type == 5 && (dhcp_state == D_REQUEST || dhcp_state == D_RENEW)) {
        if (lease > 86400)
            lease = 86400;
        if (lease < 60)
            lease = 60;
        lease_ms = lease * 1000;
        if (dhcp_state == D_RENEW && yi == my_ip) {
            bound_at = plat_ms();
            dhcp_state = D_BOUND;
            plat_logf("dhcp: renewed %s for %u s\n", a, lease);
            return;
        }
        dhcp_state = D_BOUND;
        bind_addr(yi, mask, router, 1);
    } else if (type == 6) {
        plat_logf("dhcp: NAK\n");
        if (bound == 1)
            bound = 0, plat_ip_event(0, 0, 0, 0);
        dhcp_start();
    }
}

void ip_link_up(void)
{
    link = 1;
    hello_last = 0;
    if (bound == 2)
        return;   /* static address survives a rejoin */
    if (bound == 1) {
        dhcp_state = D_RENEW;   /* confirm the old lease */
        dhcp_tries = 0;
        dhcp_send(3);
        return;
    }
    dhcp_start();
}

void ip_link_down(void) { link = 0; }

void ip_poll(void)
{
    if (!link)
        return;
    cu32 now = plat_ms();
    if (!bound) {
        if (static_ip && now - dhcp_started > 10000) {
            dhcp_state = D_IDLE;
            lease_ms = 0;
            bind_addr(static_ip, 0xffffff00, (static_ip & 0xffffff00) | 1, 2);
        } else if (now - dhcp_last_tx > 2000) {
            if (dhcp_state == D_REQUEST && dhcp_tries >= 3)
                dhcp_start();
            else
                dhcp_send(dhcp_state == D_REQUEST ? 3 : 1);
        }
    } else if (bound == 1) {
        if (now - bound_at > lease_ms) {
            plat_logf("dhcp: lease expired\n");
            bound = 0;
            plat_ip_event(0, 0, 0, 0);
            dhcp_start();
            return;
        }
        if (now - bound_at > lease_ms / 2 && now - dhcp_last_tx > 10000) {
            dhcp_state = D_RENEW;
            dhcp_send(3);
        }
    }
    if (bound && now - hello_last >= 2000) {
        hello_last = now;
        char *t = (char *)pkt + 28;
        cu8 b[6];
        plat_bssid(b);
        int n = plat_fmt(t, "HU1 hello ip=");
        n += ip_fmt_addr(t + n, my_ip);
        n += plat_fmt(t + n, " mac=%02x:%02x:%02x:%02x:%02x:%02x up=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x", my_mac[0],
                      my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5], now, b[0], b[1], b[2], b[3], b[4], b[5]);
        n += plat_fmt(t + n, " build=%s", plat_build());
        n += plat_hello_extra(t + n);
        udp_send(bcast, 0xffffffff, HU_PORT, HU_HELLO_PORT, n);
    }
}

/* ---------------- HU1 command server ---------------- */
static struct {
    int valid, op, len, port;
    cu32 seq, ip;
    cu8 mac[6];
} cache;

static void send_cached(void)
{
    int frags = cache.len ? (cache.len + HU_MAX_DATA - 1) / HU_MAX_DATA : 1;
    for (int f = 0; f < frags; f++) {
        cu8 *d = pkt + 28;
        int off = f * HU_MAX_DATA, n = cache.len - off;
        if (n > HU_MAX_DATA)
            n = HU_MAX_DATA;
        d[0] = 'H', d[1] = 'U', d[2] = '1';
        d[3] = (cu8)cache.seq, d[4] = (cu8)(cache.seq >> 8), d[5] = (cu8)(cache.seq >> 16), d[6] = (cu8)(cache.seq >> 24);
        d[7] = (cu8)(cache.op | 0x80);
        d[8] = (cu8)f, d[9] = (cu8)(f >> 8);
        d[10] = f == frags - 1;
        cpy(d + 11, rbuf + off, n);
        udp_send(cache.mac, cache.ip, HU_PORT, cache.port, 11 + n);
    }
}

static void cmd_rx(const cu8 *smac, cu32 sip, int sport, const cu8 *d, int n)
{
    if (n < 40 || d[0] != 'H' || d[1] != 'U' || d[2] != '1')
        return;
    if (!same(d + 3, tok, 32)) {
        ip_stats[S_BADTOKEN]++;
        return;
    }
    cu32 seq = le32(d + 35);
    int op = d[39], pn = n - 40;
    const cu8 *p = d + 40;
    cpy(cache.mac, smac, 6);
    cache.ip = sip, cache.port = sport;
    if (cache.valid && cache.seq == seq) {
        ip_stats[S_RESENT]++;
        send_cached();
        return;
    }
    ip_stats[S_CMD]++;
    int len = 0;
    char *t = (char *)rbuf;
    switch (op) {
    case 1:
        len = plat_fmt(t, "pong ");
        len += plat_status(t + len);
        len += plat_fmt(t + len, " ip=");
        len += ip_fmt_addr(t + len, my_ip);
        break;
    case 2: {
        static char c[1024];
        int l = pn < 1023 ? pn : 1023;
        cpy((cu8 *)c, p, l);
        c[l] = 0;
        plat_logf("hu1: cmd '%s'\n", c);
        len = plat_cmd(c, rbuf, HU_MAX_REPLY);
        break;
    }
    case 3: {
        cu32 addr = le32(p);
        int cnt = pn - 4;
        if (pn < 4 || !plat_mem_ok(addr, (cu32)cnt, 1)) {
            len = plat_fmt(t, "ERR write refused");
            break;
        }
        plat_mem_write(addr, p + 4, cnt);
        static cu8 back[1500];
        cu32 a0 = addr & ~3u, span = (addr + (cu32)cnt + 3 - a0) & ~3u;
        plat_mem_read(a0, back, (int)span);
        cu32 crc = crc32_le(back + (addr - a0), cnt);
        rbuf[0] = (cu8)crc, rbuf[1] = (cu8)(crc >> 8), rbuf[2] = (cu8)(crc >> 16), rbuf[3] = (cu8)(crc >> 24);
        len = 4;
        break;
    }
    case 4: {
        cu32 addr = le32(p);
        int cnt = pn >= 6 ? p[4] | p[5] << 8 : 0;
        if (pn < 6 || cnt > HU_MAX_DATA || (addr & 3) || !plat_mem_ok(addr, (cu32)cnt, 0)) {
            len = plat_fmt(t, "ERR read: addr must be 4-aligned, len <= 1200");
            break;
        }
        plat_mem_read(addr, rbuf, (cnt + 3) & ~3);
        len = cnt;
        break;
    }
    case 5: {
        cu32 addr = le32(p), cnt = pn >= 8 ? le32(p + 4) : 0;
        if (pn < 8 || cnt > 0x1000000) {
            len = plat_fmt(t, "ERR sync");
            break;
        }
        plat_sync(addr, cnt);
        len = plat_fmt(t, "ok");
        break;
    }
    case 6:
        len = plat_log_tail(rbuf, HU_MAX_REPLY);
        break;
    default:
        len = plat_op(op, p, pn, rbuf, HU_MAX_REPLY);
        if (len < 0)
            len = plat_fmt(t, "ERR unknown op %d", op);
    }
    cache.valid = 1, cache.seq = seq, cache.op = op, cache.len = len;
    send_cached();
}

/* ---------------- receive ---------------- */
void ip_rx(const cu8 *src, const cu8 *dst, int type, const cu8 *p, int len)
{
    ip_stats[S_RX]++;
    (void)dst;
    if (type == 0x0806) {
        if (len < 28 || be16(p) != 1 || be16(p + 2) != 0x0800 || p[4] != 6 || p[5] != 4)
            return;
        if (be16(p + 6) == 1 && bound && be32(p + 24) == my_ip) {
            cu8 *r = pkt;
            put16(r, 1), put16(r + 2, 0x0800), r[4] = 6, r[5] = 4, put16(r + 6, 2);
            cpy(r + 8, my_mac, 6), put32(r + 14, my_ip);
            cpy(r + 18, p + 8, 10);
            plat_eth_send(p + 8, 0x0806, r, 28);
            ip_stats[S_ARP]++;
        }
        return;
    }
    if (type != 0x0800 || len < 20 || (p[0] >> 4) != 4)
        return;
    int ihl = (p[0] & 15) * 4, tot = be16(p + 2);
    if (ihl < 20 || tot > len || tot < ihl || fold(sum16(0, p, ihl)) != 0)
        return;
    if ((p[6] & 0x3f) || p[7])
        return;   /* fragments: not supported */
    cu32 sip = be32(p + 12), dip = be32(p + 16);
    int ours = bound && (dip == my_ip || dip == (my_ip | ~my_mask)), bc = dip == 0xffffffff;
    const cu8 *l4 = p + ihl;
    int n = tot - ihl;
    if (p[9] == 1 && ours && dip == my_ip && n >= 8 && l4[0] == 8 && l4[1] == 0) {
        if (n > 1400)
            return;
        cu8 *r = pkt;
        cpy(r, p, ihl > 20 ? 20 : ihl);   /* echo reply, options dropped */
        r[0] = 0x45;
        put16(r + 2, 20 + n);
        r[8] = 64;
        put32(r + 12, my_ip), put32(r + 16, sip);
        r[10] = r[11] = 0;
        put16(r + 10, fold(sum16(0, r, 20)));
        cpy(r + 20, l4, n);
        r[20] = 0, r[22] = r[23] = 0;
        put16(r + 22, fold(sum16(0, r + 20, n)));
        plat_eth_send(src, 0x0800, r, 20 + n);
        ip_stats[S_ICMP]++;
        return;
    }
    if (p[9] != 17 || n < 8)
        return;
    int sport = be16(l4), dport = be16(l4 + 2), ulen = be16(l4 + 4);
    if (ulen < 8 || ulen > n)
        return;
    ip_stats[S_UDP]++;
    if (dport == 68 && (bc || ours || !bound || dip == offered_ip))
        dhcp_rx(l4 + 8, ulen - 8);
    else if (dport == HU_PORT && (ours || bc) && bound)
        cmd_rx(src, sip, sport, l4 + 8, ulen - 8);
}
