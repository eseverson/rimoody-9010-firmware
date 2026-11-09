/* 802.11 management frames for our access point (ap80211.c). */
#ifndef AP80211_H
#define AP80211_H
struct ap_cfg {
    const char *ssid;
    unsigned char bssid[6];
    int channel;   /* 1..13 */
};
enum { AP_RX_OTHER, AP_RX_PROBE, AP_RX_AUTH, AP_RX_ASSOC, AP_RX_LEAVE };
struct ap_rx {
    unsigned char sa[6], rsn[64];
    int rsn_len, auth_alg, reassoc, ht, reason;
};
/* each returns the frame length (no FCS); f needs 256 bytes */
int ap_beacon(unsigned char *f, const struct ap_cfg *c);
int ap_probe_resp(unsigned char *f, const struct ap_cfg *c, const unsigned char *to, int seq);
int ap_auth_resp(unsigned char *f, const struct ap_cfg *c, const unsigned char *to, int seq, int status);
int ap_assoc_resp(unsigned char *f, const struct ap_cfg *c, const unsigned char *to, int seq, int status, int aid, int reassoc);
int ap_deauth(unsigned char *f, const struct ap_cfg *c, const unsigned char *to, int seq, int reason);
int ap_rsn_ie(unsigned char *o);   /* our RSN IE (the 4-way handshake's message 3 must carry the same bytes) */
int ap_parse(const unsigned char *f, int n, const struct ap_cfg *c, struct ap_rx *r);
#endif
