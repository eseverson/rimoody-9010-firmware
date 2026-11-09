/* WPA2-PSK authenticator: the access-point side of the 4-way handshake (IEEE 802.11-2016 12.7.6), CCMP, EAPOL-Key
 * descriptor version 2, for the wireless Android Auto access point (notes/android_auto.md). One station at a time.
 * Platform independent; host-tested against our own supplicant (wpa.c) in test/wpaap_host.c. */
#include "wpa_ap.h"

#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENC_DATA  0x1000

static void ap_cpy(cu8 *d, const cu8 *s, int n) { while (n-- > 0) *d++ = *s++; }
static int ap_cmp(const cu8 *a, const cu8 *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* RFC 3394 wrap with a 128-bit KEK: n 64-bit blocks in, n + 1 out */
static void aes_wrap(const cu8 kek[16], int n, const cu8 *in, cu8 *out)
{
    struct aes_key k;
    aes_setkey(&k, kek);
    cu8 a[8], b[16];
    for (int i = 0; i < 8; i++)
        a[i] = 0xa6;
    ap_cpy(out + 8, in, 8 * n);
    for (int j = 0; j <= 5; j++)
        for (int i = 1; i <= n; i++) {
            ap_cpy(b, a, 8);
            ap_cpy(b + 8, out + 8 * i, 8);
            aes_encrypt(&k, b, b);
            cu32 t = (cu32)(n * j + i);
            ap_cpy(a, b, 8);
            a[7] ^= (cu8)t, a[6] ^= (cu8)(t >> 8), a[5] ^= (cu8)(t >> 16), a[4] ^= (cu8)(t >> 24);
            ap_cpy(out + 8 * i, b + 8, 8);
        }
    ap_cpy(out, a, 8);
}

static void ap_mic(const cu8 kck[16], const cu8 *f, int len, cu8 out[16])
{
    cu8 h[20];
    hmac_sha1(kck, 16, f, len, h);
    ap_cpy(out, h, 16);
}

static void put_replay(cu8 *p, cu64 r)
{
    for (int i = 7; i >= 0; i--)
        p[i] = (cu8)r, r >>= 8;
}

/* EAPOL-Key frame into a->tx; MIC computed when ki has KI_MIC */
static void ap_build(struct wpa_ap *a, int ki, const cu8 *nonce, const cu8 *kd, int kdlen, const cu8 rsc[6])
{
    cu8 *t = a->tx;
    int len = 99 + kdlen;
    for (int i = 0; i < len; i++)
        t[i] = 0;
    t[0] = 2;   /* 802.1X-2004 */
    t[1] = 3;
    t[2] = (cu8)((len - 4) >> 8), t[3] = (cu8)(len - 4);
    t[4] = 2;
    t[5] = (cu8)(ki >> 8), t[6] = (cu8)ki;
    t[8] = 16;   /* key length (CCMP) */
    put_replay(t + 9, ++a->replay);
    if (nonce)
        ap_cpy(t + 17, nonce, 32);
    if (rsc)
        ap_cpy(t + 81 - 16, rsc, 6);   /* key RSC at offset 65 */
    t[97] = (cu8)(kdlen >> 8), t[98] = (cu8)kdlen;
    ap_cpy(t + 99, kd, kdlen);
    if (ki & KI_MIC)
        ap_mic(a->ptk, t, len, t + 81);
    a->tx_len = len;
}

void wpa_ap_init(struct wpa_ap *a, const cu8 pmk[32], const cu8 aa[6], const cu8 spa[6], const cu8 *ap_ie, int ie_len,
                 const cu8 anonce[32], const cu8 gtk[16], int gtk_id)
{
    cu8 *p = (cu8 *)a;
    for (unsigned i = 0; i < sizeof(*a); i++)
        p[i] = 0;
    ap_cpy(a->pmk, pmk, 32);
    ap_cpy(a->aa, aa, 6);
    ap_cpy(a->spa, spa, 6);
    ap_cpy(a->ie, ap_ie, ie_len);
    a->ie_len = ie_len;
    ap_cpy(a->anonce, anonce, 32);
    ap_cpy(a->gtk, gtk, 16);
    a->gtk_id = gtk_id;
}

int wpa_ap_start(struct wpa_ap *a)   /* message 1 */
{
    ap_build(a, 2 | KI_PAIRWISE | KI_ACK, a->anonce, 0, 0, 0);
    a->state = 1;
    a->tries++;
    return WPA_AP_TX;
}

int wpa_ap_rx(struct wpa_ap *a, const cu8 *e, int len)
{
    if (len < 99 || e[1] != 3 || e[4] != 2)
        return 0;
    int blen = e[2] << 8 | e[3];
    if (4 + blen > len || 4 + blen < 99)
        return 0;
    len = 4 + blen;
    int ki = e[5] << 8 | e[6], kdlen = e[97] << 8 | e[98];
    if (99 + kdlen > len || !(ki & KI_MIC) || (ki & KI_ACK))
        return 0;
    cu64 r = 0;
    for (int i = 0; i < 8; i++)
        r = r << 8 | e[9 + i];
    if (r != a->replay)   /* must echo the counter of our last message */
        return 0;
    static cu8 tmp[512];
    if (len > (int)sizeof tmp)
        return 0;

    if (a->state == 1 && (ki & KI_PAIRWISE) && !(ki & KI_SECURE)) {   /* message 2: derive PTK, check MIC */
        ap_cpy(a->snonce, e + 17, 32);
        cu8 data[76];
        int sa = ap_cmp(a->aa, a->spa, 6) < 0, na = ap_cmp(a->anonce, a->snonce, 32) < 0;
        ap_cpy(data, sa ? a->aa : a->spa, 6);
        ap_cpy(data + 6, sa ? a->spa : a->aa, 6);
        ap_cpy(data + 12, na ? a->anonce : a->snonce, 32);
        ap_cpy(data + 44, na ? a->snonce : a->anonce, 32);
        prf_sha1(a->pmk, 32, "Pairwise key expansion", data, 76, a->ptk, 48);
        ap_cpy(tmp, e, len);
        for (int i = 81; i < 97; i++)
            tmp[i] = 0;
        cu8 m[16];
        ap_mic(a->ptk, tmp, len, m);
        if (ap_cmp(m, e + 81, 16)) {
            a->state = 0;
            return WPA_AP_BADMIC;   /* wrong passphrase on the station */
        }
        /* message 3: our RSN IE + the GTK KDE, padded and wrapped with the KEK */
        cu8 kd[128];
        int n = 0;
        ap_cpy(kd, a->ie, a->ie_len);
        n = a->ie_len;
        kd[n++] = 0xdd, kd[n++] = 22, kd[n++] = 0x00, kd[n++] = 0x0f, kd[n++] = 0xac, kd[n++] = 0x01;
        kd[n++] = (cu8)(a->gtk_id & 3), kd[n++] = 0;
        ap_cpy(kd + n, a->gtk, 16);
        n += 16;
        if (n % 8 || n < 16) {
            kd[n++] = 0xdd;
            while (n % 8 || n < 16)
                kd[n++] = 0;
        }
        cu8 wrapped[136];
        aes_wrap(a->ptk + 16, n / 8, kd, wrapped);
        static const cu8 rsc0[6];
        ap_build(a, 2 | KI_PAIRWISE | KI_INSTALL | KI_ACK | KI_MIC | KI_SECURE | KI_ENC_DATA, a->anonce, wrapped, n + 8, rsc0);
        a->state = 3;
        return WPA_AP_TX;
    }
    if (a->state == 3 && (ki & KI_PAIRWISE) && (ki & KI_SECURE)) {   /* message 4 */
        ap_cpy(tmp, e, len);
        for (int i = 81; i < 97; i++)
            tmp[i] = 0;
        cu8 m[16];
        ap_mic(a->ptk, tmp, len, m);
        if (ap_cmp(m, e + 81, 16))
            return 0;
        a->state = 4;
        return WPA_AP_PTK | WPA_AP_DONE;
    }
    return 0;
}
