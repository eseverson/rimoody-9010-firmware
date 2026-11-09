/* 802.11 management frames for our access point (wireless Android Auto, notes/android_auto.md): beacon, probe
 * response, open-system authentication response and association response, plus parsing of what a phone sends.
 * 2.4 GHz, 802.11b/g/n (HT20, MCS 0-7, short GI), WPA2-PSK / CCMP, WMM (phones expect it for 11n). Frames are built
 * without FCS (the hardware appends it); the beacon timestamp is filled by the hardware. Pure C, host-tested with
 * tshark (test/test_ap80211.py). */
#include "ap80211.h"
#include <string.h>

typedef unsigned char u8;

static int ie(u8 *o, int id, const u8 *d, int n)
{
    o[0] = (u8)id, o[1] = (u8)n;
    memcpy(o + 2, d, (size_t)n);
    return 2 + n;
}

/* RSN IE: version 1, group CCMP, pairwise CCMP, AKM PSK, no capabilities */
static const u8 rsn[] = { 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };

int ap_rsn_ie(u8 *o) { return ie(o, 48, rsn, sizeof rsn); }

static int body_ies(u8 *o, const struct ap_cfg *c, int beacon, int assoc)
{
    int k = 0;
    if (!assoc)
        k += ie(o + k, 0, (const u8 *)c->ssid, (int)strlen(c->ssid));
    static const u8 rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };   /* 1 2 5.5 11 basic, 6 9 12 18 */
    k += ie(o + k, 1, rates, sizeof rates);
    if (!assoc) {
        u8 ch = (u8)c->channel;
        k += ie(o + k, 3, &ch, 1);
    }
    if (beacon) {
        static const u8 tim[] = { 0, 1, 0, 0 };   /* DTIM count 0, period 1, no buffered traffic */
        k += ie(o + k, 5, tim, sizeof tim);
    }
    if (!assoc) {
        static const u8 erp[] = { 0 };
        k += ie(o + k, 42, erp, 1);
    }
    static const u8 xrates[] = { 0x30, 0x48, 0x60, 0x6c };   /* 24 36 48 54 */
    k += ie(o + k, 50, xrates, sizeof xrates);
    if (!assoc)
        k += ap_rsn_ie(o + k);
    /* HT capabilities: HT20, SM power save disabled, short GI 20, max A-MSDU 3839; A-MPDU factor 64 KB, 8 us
     * spacing; MCS 0-7 */
    u8 htc[26] = { 0 };
    htc[0] = 0x2c, htc[1] = 0x01;   /* cap info 0x012c: SMPS disabled (3<<2), SGI20, Rx STBC 1 */
    htc[2] = 0x1b;                  /* A-MPDU: max 64 KB (3), min spacing 8 us (6<<2) */
    htc[3] = 0xff;                  /* MCS 0-7 */
    k += ie(o + k, 45, htc, sizeof htc);
    u8 hto[22] = { 0 };
    hto[0] = (u8)c->channel;        /* primary channel, no secondary, HT20 */
    k += ie(o + k, 61, hto, sizeof hto);
    /* WMM parameter element (Microsoft OUI 00:50:f2, type 2, subtype 1, version 1) with the default EDCA set */
    static const u8 wmm[] = { 0x00, 0x50, 0xf2, 2, 1, 1, 0x00, 0x00,
                              0x03, 0xa4, 0x00, 0x00,    /* AC_BE: AIFSN 3, ECWmin 4, ECWmax 10 */
                              0x27, 0xa4, 0x00, 0x00,    /* AC_BK: AIFSN 7 */
                              0x42, 0x43, 0x5e, 0x00,    /* AC_VI: AIFSN 2, ECW 3/4, TXOP 94 */
                              0x62, 0x32, 0x2f, 0x00 };  /* AC_VO: AIFSN 2, ECW 2/3, TXOP 47 */
    k += ie(o + k, 221, wmm, sizeof wmm);
    return k;
}

static int hdr(u8 *f, int fc, const u8 *da, const u8 *bssid, int seq)
{
    f[0] = (u8)fc, f[1] = 0, f[2] = 0, f[3] = 0;
    memcpy(f + 4, da, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[22] = (u8)(seq << 4), f[23] = (u8)(seq >> 4);
    return 24;
}

static int fixed(u8 *f, int beacon_like)
{
    int k = 0;
    if (beacon_like) {
        memset(f, 0, 8);   /* timestamp: the hardware fills it */
        f[8] = 100, f[9] = 0;   /* beacon interval 100 TU */
        k = 10;
    }
    f[k] = 0x31, f[k + 1] = 0x04;   /* capabilities: ESS, privacy, short preamble, short slot */
    return k + 2;
}

int ap_beacon(u8 *f, const struct ap_cfg *c)
{
    static const u8 bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    int k = hdr(f, 0x80, bc, c->bssid, 0);
    k += fixed(f + k, 1);
    return k + body_ies(f + k, c, 1, 0);
}

int ap_probe_resp(u8 *f, const struct ap_cfg *c, const u8 *to, int seq)
{
    int k = hdr(f, 0x50, to, c->bssid, seq);
    k += fixed(f + k, 1);
    return k + body_ies(f + k, c, 0, 0);
}

int ap_auth_resp(u8 *f, const struct ap_cfg *c, const u8 *to, int seq, int status)
{
    int k = hdr(f, 0xb0, to, c->bssid, seq);
    f[k++] = 0, f[k++] = 0;   /* open system */
    f[k++] = 2, f[k++] = 0;   /* transaction 2 */
    f[k++] = (u8)status, f[k++] = (u8)(status >> 8);
    return k;
}

int ap_assoc_resp(u8 *f, const struct ap_cfg *c, const u8 *to, int seq, int status, int aid, int reassoc)
{
    int k = hdr(f, reassoc ? 0x30 : 0x10, to, c->bssid, seq);
    k += fixed(f + k, 0);
    f[k++] = (u8)status, f[k++] = (u8)(status >> 8);
    f[k++] = (u8)aid, f[k++] = (u8)((aid >> 8) | 0xc0);
    return k + body_ies(f + k, c, 0, 1);
}

int ap_deauth(u8 *f, const struct ap_cfg *c, const u8 *to, int seq, int reason)
{
    int k = hdr(f, 0xc0, to, c->bssid, seq);
    f[k++] = (u8)reason, f[k++] = (u8)(reason >> 8);
    return k;
}

static const u8 *find_ie(const u8 *p, int n, int id)
{
    for (int i = 0; i + 2 <= n && i + 2 + p[i + 1] <= n; i += 2 + p[i + 1])
        if (p[i] == id)
            return p + i;
    return 0;
}

int ap_parse(const u8 *f, int n, const struct ap_cfg *c, struct ap_rx *r)
{
    memset(r, 0, sizeof *r);
    if (n < 24 || (f[0] & 0x0c) != 0)   /* management frames only */
        return AP_RX_OTHER;
    memcpy(r->sa, f + 10, 6);
    int st = f[0] >> 4;
    int to_us = !memcmp(f + 4, c->bssid, 6) || (f[4] & 1);
    if (!to_us)
        return AP_RX_OTHER;
    const u8 *b = f + 24;
    int bn = n - 24;
    switch (st) {
    case 4: {   /* probe request: wildcard or our SSID */
        const u8 *s = find_ie(b, bn, 0);
        if (!s)
            return AP_RX_OTHER;
        if (s[1] && (s[1] != strlen(c->ssid) || memcmp(s + 2, c->ssid, s[1])))
            return AP_RX_OTHER;
        return AP_RX_PROBE;
    }
    case 11:   /* authentication */
        if (bn < 6 || memcmp(f + 16, c->bssid, 6))
            return AP_RX_OTHER;
        r->auth_alg = b[0] | b[1] << 8;
        return b[2] == 1 && b[3] == 0 ? AP_RX_AUTH : AP_RX_OTHER;
    case 0:
    case 2: {   /* (re)association request */
        int off = st == 2 ? 10 : 4;
        if (bn < off || memcmp(f + 16, c->bssid, 6))
            return AP_RX_OTHER;
        r->reassoc = st == 2;
        const u8 *s = find_ie(b + off, bn - off, 0);
        if (!s || s[1] != strlen(c->ssid) || memcmp(s + 2, c->ssid, s[1]))
            return AP_RX_OTHER;
        const u8 *rs = find_ie(b + off, bn - off, 48);
        if (rs && rs[1] + 2 <= (int)sizeof r->rsn) {
            memcpy(r->rsn, rs, (size_t)rs[1] + 2);
            r->rsn_len = rs[1] + 2;
        }
        r->ht = find_ie(b + off, bn - off, 45) != 0;
        return AP_RX_ASSOC;
    }
    case 10:
    case 12:   /* disassociation / deauthentication */
        r->reason = bn >= 2 ? b[0] | b[1] << 8 : 0;
        return AP_RX_LEAVE;
    }
    return AP_RX_OTHER;
}
