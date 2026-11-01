/* Wireless Android Auto: the Bluetooth RFCOMM handshake that hands the phone our access point, and the TCP
 * transport for aa.c (notes/android_auto.md, "Wireless only").
 *
 * RFCOMM (service UUID 4de17a00-52cb-11e6-bdf4-0800200c9a66), each message: length u16be (protobuf bytes only),
 * id u16be, protobuf. As implemented by open wireless-AA projects (unverified against a phone here):
 *   1 WifiStartRequest   HU -> phone  { 1 ip_address string, 2 port int32 }
 *   2 WifiInfoRequest    phone -> HU  {}
 *   3 WifiInfoResponse   HU -> phone  { 1 ssid, 2 key, 3 bssid "aa:bb:..", 4 security_mode (8 = WPA2 personal),
 *                                       5 access_point_type (0 static) }
 *   6 WifiConnectStatus  phone -> HU  { 1 status }
 *   7 WifiStartResponse  phone -> HU  { 1 ip_address, 2 port, 3 status }
 * After that the phone joins the AP and connects to ip:port over TCP; aa.c then runs over that connection. */
#include "aa_wl.h"
#include <string.h>

typedef unsigned char u8;

static const struct aa_wl_ap *ap;
static int (*bt_send)(const u8 *p, int n);
static struct aa_wl_status ws;

static int put_varint(u8 *o, unsigned long long v)
{
    int k = 0;
    while (v >= 0x80)
        o[k++] = (u8)(v | 0x80), v >>= 7;
    o[k++] = (u8)v;
    return k;
}

static int put_str(u8 *o, int field, const char *s)
{
    int k = put_varint(o, (unsigned)field << 3 | 2), n = (int)strlen(s);
    k += put_varint(o + k, (unsigned)n);
    memcpy(o + k, s, (size_t)n);
    return k + n;
}

static int put_u(u8 *o, int field, unsigned long long v)
{
    int k = put_varint(o, (unsigned)field << 3);
    return k + put_varint(o + k, v);
}

static int send_msg(int id, const u8 *pb, int n)
{
    u8 m[300];
    if (n > (int)sizeof m - 4)
        return -1;
    m[0] = (u8)(n >> 8), m[1] = (u8)n, m[2] = (u8)(id >> 8), m[3] = (u8)id;
    memcpy(m + 4, pb, (size_t)n);
    ws.tx++;
    return bt_send(m, n + 4);
}

int aa_wl_rfcomm_connected(const struct aa_wl_ap *a, int (*send)(const unsigned char *p, int n))
{
    ap = a;
    bt_send = send;
    memset(&ws, 0, sizeof ws);
    ws.state = AA_WL_STARTED;
    u8 pb[64];
    int k = put_str(pb, 1, a->ip);
    k += put_u(pb + k, 2, (unsigned)a->port);
    return send_msg(1, pb, k);
}

static u8 rb[512];
static int rn;

static unsigned long long get_u(const u8 *p, int n, int field)
{
    int i = 0;
    while (i < n) {
        unsigned long long key = 0, v = 0;
        for (int s = 0; i < n; s += 7) { u8 c = p[i++]; key |= (unsigned long long)(c & 0x7f) << s; if (!(c & 0x80)) break; }
        for (int s = 0; i < n; s += 7) { u8 c = p[i++]; v |= (unsigned long long)(c & 0x7f) << s; if (!(c & 0x80)) break; }
        if ((key & 7) == 2)
            i += (int)v, v = 0;
        else if ((key & 7) != 0)
            return 0;
        if ((int)(key >> 3) == field && (key & 7) == 0)
            return v;
    }
    return 0;
}

void aa_wl_rfcomm_rx(const unsigned char *p, int n)
{
    while (n > 0) {
        int k = n < (int)sizeof rb - rn ? n : (int)sizeof rb - rn;
        memcpy(rb + rn, p, (size_t)k);
        rn += k, p += k, n -= k;
        while (rn >= 4) {
            int len = rb[0] << 8 | rb[1], id = rb[2] << 8 | rb[3];
            if (len > (int)sizeof rb - 4) {   /* garbage: resync by dropping the buffer */
                rn = 0;
                ws.errors++;
                break;
            }
            if (rn < 4 + len)
                break;
            const u8 *pb = rb + 4;
            ws.rx++;
            if (id == 2) {   /* WifiInfoRequest -> our AP */
                u8 o[256];
                int m = put_str(o, 1, ap->ssid);
                m += put_str(o + m, 2, ap->key);
                m += put_str(o + m, 3, ap->bssid);
                m += put_u(o + m, 4, 8);   /* WPA2 personal */
                m += put_u(o + m, 5, 0);   /* static access point */
                send_msg(3, o, m);
                ws.state = AA_WL_INFO_SENT;
            } else if (id == 7) {
                ws.start_status = (int)get_u(pb, len, 3);
                ws.state = AA_WL_PHONE_STARTED;
            } else if (id == 6) {
                ws.connect_status = (int)get_u(pb, len, 1);
                ws.state = AA_WL_PHONE_CONNECTED;
            } else
                ws.unknown++;
            memmove(rb, rb + 4 + len, (size_t)(rn - 4 - len));
            rn -= 4 + len;
        }
    }
}

const struct aa_wl_status *aa_wl_status(void) { return &ws; }
