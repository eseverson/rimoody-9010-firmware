/* Wireless Android Auto Bluetooth handshake (aa_wl.c). */
#ifndef AA_WL_H
#define AA_WL_H
struct aa_wl_ap {
    const char *ssid, *key, *bssid;   /* bssid as "aa:bb:cc:dd:ee:ff" */
    const char *ip;                   /* our address on the AP, e.g. "192.168.43.1" */
    int port;                         /* TCP port aa.c listens on */
};
enum aa_wl_state { AA_WL_IDLE, AA_WL_STARTED, AA_WL_INFO_SENT, AA_WL_PHONE_STARTED, AA_WL_PHONE_CONNECTED };
struct aa_wl_status { enum aa_wl_state state; int start_status, connect_status; unsigned int rx, tx, unknown, errors; };

/* RFCOMM channel to the phone is open: sends WifiStartRequest (ip, port). */
int aa_wl_rfcomm_connected(const struct aa_wl_ap *ap, int (*send)(const unsigned char *p, int n));
void aa_wl_rfcomm_rx(const unsigned char *p, int n);   /* bytes from the phone over RFCOMM */
const struct aa_wl_status *aa_wl_status(void);
#endif
