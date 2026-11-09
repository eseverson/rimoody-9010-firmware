/* WPA2-PSK authenticator (wpa_ap.c): the AP side of the 4-way handshake, CCMP, one station. */
#ifndef WPA_AP_H
#define WPA_AP_H
#include "crypto.h"

struct wpa_ap {
    cu8 pmk[32], aa[6], spa[6], anonce[32], snonce[32], ptk[48];   /* PTK: KCK 0-15, KEK 16-31, TK 32-47 */
    cu8 ie[64];        /* our RSN IE, exactly as in the beacon */
    int ie_len;
    cu8 gtk[16];
    int gtk_id;
    cu64 replay;       /* counter of the last message we sent */
    int state, tries;  /* 0 idle, 1 sent 1/4, 3 sent 3/4, 4 done */
    cu8 tx[256];
    int tx_len;
};

#define WPA_AP_TX     1   /* send a->tx (EAPOL, unprotected) */
#define WPA_AP_PTK    2   /* install TK = ptk + 32 for the station */
#define WPA_AP_DONE   4
#define WPA_AP_BADMIC 8   /* message 2 failed the MIC: the station has another passphrase */

void wpa_ap_init(struct wpa_ap *a, const cu8 pmk[32], const cu8 aa[6], const cu8 spa[6], const cu8 *ap_ie, int ie_len,
                 const cu8 anonce[32], const cu8 gtk[16], int gtk_id);
int wpa_ap_start(struct wpa_ap *a);                    /* message 1 into a->tx (also for a retry) */
int wpa_ap_rx(struct wpa_ap *a, const cu8 *eapol, int len);   /* eapol from the 802.1X version byte */
#endif
