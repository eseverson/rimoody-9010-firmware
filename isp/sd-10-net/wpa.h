/* WPA2-PSK supplicant side of the 4-way and group-key handshakes (IEEE 802.11-2016 12.7.6/12.7.7), CCMP only,
 * EAPOL-Key descriptor version 2 (HMAC-SHA1-128 MIC, AES key wrap). Platform independent; the payload and the host
 * test (wpa_host.c) both build it. */
#ifndef WPA_H
#define WPA_H
#include "crypto.h"

struct wpa {
    cu8 pmk[32], aa[6], spa[6];
    cu8 anonce[32], snonce[32], ptk[48];   /* KCK 0-15, KEK 16-31, TK 32-47 */
    cu8 ie[64];                            /* our RSN IE, exactly as in the association request */
    int ie_len;
    cu64 replay;                           /* last replay counter of a MIC-verified frame */
    int have_replay, ptk_ok, done;
    cu8 gtk[16], gtk_rsc[6];
    int gtk_id;
    cu8 tx[160];                           /* reply EAPOL frame */
    int tx_len, tx_protect;                /* tx_protect: send it CCMP-protected (group handshake) */
};

#define WPA_TX   1   /* send w->tx */
#define WPA_PTK  2   /* install TK = ptk + 32 (after sending w->tx) */
#define WPA_GTK  4   /* install gtk / gtk_id / gtk_rsc */
#define WPA_DONE 8   /* 4-way handshake complete */
#define WPA_ERR  16  /* unsupported AP configuration: give up */

void wpa_init(struct wpa *w, const cu8 pmk[32], const cu8 aa[6], const cu8 spa[6], const cu8 *ie, int ie_len,
              const cu8 snonce[32]);
int wpa_rx(struct wpa *w, const cu8 *eapol, int len);   /* eapol starts at the 802.1X version byte */

void plat_logf(const char *fmt, ...);   /* provided by the platform; %s %c %d %u %x with 0/width only */

#endif
