/* See wpa.h. EAPOL-Key layout (offsets from the 802.1X header): 0 version, 1 type (3 = Key), 2 body length (BE),
 * 4 descriptor type (2 = RSN), 5 key info (BE), 7 key length, 9 replay counter (8), 17 nonce (32), 49 IV (16),
 * 65 RSC (8), 73 reserved (8), 81 MIC (16), 97 key data length (BE), 99 key data. */
#include "wpa.h"

#define KI_VER_MASK  0x0007
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENC_DATA  0x1000

static int be16(const cu8 *p) { return p[0] << 8 | p[1]; }

static cu64 be64(const cu8 *p)
{
    cu64 v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

static int cmp(const cu8 *a, const cu8 *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

static void cpy(cu8 *d, const cu8 *s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

void wpa_init(struct wpa *w, const cu8 pmk[32], const cu8 aa[6], const cu8 spa[6], const cu8 *ie, int ie_len,
              const cu8 snonce[32])
{
    cu8 *p = (cu8 *)w;
    for (unsigned i = 0; i < sizeof(*w); i++)
        p[i] = 0;
    cpy(w->pmk, pmk, 32);
    cpy(w->aa, aa, 6);
    cpy(w->spa, spa, 6);
    cpy(w->ie, ie, ie_len);
    w->ie_len = ie_len;
    cpy(w->snonce, snonce, 32);
    w->gtk_id = -1;
}

static void mic(const cu8 kck[16], const cu8 *frame, int len, cu8 out[16])
{
    cu8 h[20];
    hmac_sha1(kck, 16, frame, len, h);
    cpy(out, h, 16);
}

/* EAPOL-Key frame into w->tx with the MIC filled in (the MIC field is zero while it is computed) */
static void build(struct wpa *w, int ki, const cu8 replay[8], const cu8 *nonce, const cu8 *kd, int kdlen)
{
    cu8 *t = w->tx;
    int len = 99 + kdlen;
    for (int i = 0; i < len; i++)
        t[i] = 0;
    t[0] = 1;   /* 802.1X-2001 */
    t[1] = 3;
    t[2] = (cu8)((len - 4) >> 8), t[3] = (cu8)(len - 4);
    t[4] = 2;
    t[5] = (cu8)(ki >> 8), t[6] = (cu8)ki;
    cpy(t + 9, replay, 8);
    if (nonce)
        cpy(t + 17, nonce, 32);
    t[97] = (cu8)(kdlen >> 8), t[98] = (cu8)kdlen;
    cpy(t + 99, kd, kdlen);
    mic(w->ptk, t, len, t + 81);
    w->tx_len = len;
}

static int check_mic(const struct wpa *w, const cu8 *e, int len)
{
    static cu8 tmp[512];
    if (len > (int)sizeof(tmp))
        return -1;
    cpy(tmp, e, len);
    for (int i = 81; i < 97; i++)
        tmp[i] = 0;
    cu8 m[16];
    mic(w->ptk, tmp, len, m);
    return cmp(m, e + 81, 16) ? -1 : 0;
}

/* Unwrap the key data with the KEK and pick out the GTK KDE (dd len 00-0f-ac 01 keyid/tx 0 GTK). */
static int take_gtk(struct wpa *w, const cu8 *e, int kdlen)
{
    static cu8 kd[256];
    if (kdlen < 24 || kdlen % 8 || kdlen > (int)sizeof(kd) + 8) {
        plat_logf("wpa: bad key data length %d\n", kdlen);
        return -1;
    }
    if (aes_unwrap(w->ptk + 16, kdlen / 8 - 1, e + 99, kd)) {
        plat_logf("wpa: key data unwrap failed\n");
        return -1;
    }
    int n = kdlen - 8;
    for (int i = 0; i + 2 <= n && i + 2 + kd[i + 1] <= n; i += 2 + kd[i + 1]) {
        const cu8 *k = kd + i;
        if (k[0] == 0xdd && k[1] >= 6 && k[2] == 0x00 && k[3] == 0x0f && k[4] == 0xac && k[5] == 0x01) {
            if (k[1] - 6 != 16) {
                plat_logf("wpa: GTK length %d, want 16 (CCMP)\n", k[1] - 6);
                return -1;
            }
            w->gtk_id = k[6] & 3;
            cpy(w->gtk, k + 8, 16);
            cpy(w->gtk_rsc, e + 65, 6);
            return 0;
        }
        if (k[0] == 0xdd && k[1] == 0)
            break;   /* padding */
    }
    plat_logf("wpa: no GTK in the key data\n");
    return -1;
}

int wpa_rx(struct wpa *w, const cu8 *e, int len)
{
    if (len < 99 || e[1] != 3)
        return 0;
    int blen = be16(e + 2);
    if (4 + blen > len || 4 + blen < 99) {
        plat_logf("wpa: truncated EAPOL-Key (%d of %d)\n", len, 4 + blen);
        return 0;
    }
    len = 4 + blen;
    if (e[4] != 2) {
        plat_logf("wpa: descriptor type %d ignored\n", e[4]);
        return 0;
    }
    int ki = be16(e + 5), kdlen = be16(e + 97);
    if (99 + kdlen > len) {
        plat_logf("wpa: key data overruns the frame\n");
        return 0;
    }
    if ((ki & KI_VER_MASK) != 2) {
        plat_logf("wpa: key descriptor version %d not supported (need 2: HMAC-SHA1/AES)\n", ki & KI_VER_MASK);
        return WPA_ERR;
    }
    cu64 replay = be64(e + 9);
    if (w->have_replay && replay <= w->replay) {
        plat_logf("wpa: replayed EAPOL-Key (counter %x) dropped\n", (cu32)replay);
        return 0;
    }
    static const cu8 zero[32];

    if ((ki & KI_PAIRWISE) && (ki & KI_ACK) && !(ki & KI_MIC)) {   /* message 1 */
        cpy(w->anonce, e + 17, 32);
        cu8 data[76];
        int sa = cmp(w->aa, w->spa, 6) < 0, na = cmp(w->anonce, w->snonce, 32) < 0;
        cpy(data, sa ? w->aa : w->spa, 6);
        cpy(data + 6, sa ? w->spa : w->aa, 6);
        cpy(data + 12, na ? w->anonce : w->snonce, 32);
        cpy(data + 44, na ? w->snonce : w->anonce, 32);
        prf_sha1(w->pmk, 32, "Pairwise key expansion", data, 76, w->ptk, 48);
        w->ptk_ok = 1;
        build(w, 2 | KI_PAIRWISE | KI_MIC, e + 9, w->snonce, w->ie, w->ie_len);
        w->tx_protect = 0;
        plat_logf("wpa: msg 1/4 (replay %x), sent 2/4\n", (cu32)replay);
        return WPA_TX;
    }
    if (!w->ptk_ok || !(ki & KI_MIC) || !(ki & KI_ACK)) {
        plat_logf("wpa: unexpected EAPOL-Key, key info %04x\n", ki);
        return 0;
    }
    if (check_mic(w, e, len)) {
        plat_logf("wpa: MIC mismatch (key info %04x) - wrong passphrase?\n", ki);
        return 0;
    }
    w->replay = replay, w->have_replay = 1;

    if (ki & KI_PAIRWISE) {   /* message 3 */
        if (cmp(w->anonce, e + 17, 32)) {
            plat_logf("wpa: msg 3/4 ANonce differs from 1/4, dropped\n");
            return 0;
        }
        if (!(ki & KI_INSTALL) || !(ki & KI_ENC_DATA) || take_gtk(w, e, kdlen))
            return 0;
        int again = w->done;
        build(w, 2 | KI_PAIRWISE | KI_MIC | KI_SECURE, e + 9, 0, zero, 0);
        w->tx_protect = 0;
        w->done = 1;
        plat_logf("wpa: msg 3/4 ok, GTK id %d, sent 4/4%s\n", w->gtk_id, again ? " (retransmission, keys kept)" : "");
        return again ? WPA_TX : WPA_TX | WPA_PTK | WPA_GTK | WPA_DONE;
    }
    /* group key handshake message 1 */
    if (!w->done || !(ki & KI_ENC_DATA) || take_gtk(w, e, kdlen))
        return 0;
    build(w, 2 | KI_MIC | KI_SECURE, e + 9, 0, zero, 0);
    w->tx_protect = 1;
    plat_logf("wpa: group rekey, GTK id %d, sent 2/2\n", w->gtk_id);
    return WPA_TX | WPA_GTK;
}
