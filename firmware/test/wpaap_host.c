/* Host test: our WPA2 authenticator (wpa_ap.c) against our supplicant (wpa.c, already verified against published
 * vectors and a real AP): full 4-way handshake, same PTK and GTK on both sides, and a wrong passphrase rejected. */
#include "../crypto.c"
#include "../wpa.c"
#include "../wpa_ap.c"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
void plat_logf(const char *fmt, ...) { (void)fmt; }
static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; }
static int run(const char *ap_pass, const char *sta_pass)
{
    static const cu8 ssid[] = "MiataAA", aa[6] = { 2, 0x11, 0x22, 0x33, 0x44, 0x55 }, spa[6] = { 0x3a, 1, 2, 3, 4, 5 };
    static const cu8 ie[] = { 0x30, 0x14, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
    cu8 pmk_ap[32], pmk_sta[32], anonce[32], snonce[32], gtk[16];
    pbkdf2_sha1((const cu8 *)ap_pass, (int)strlen(ap_pass), ssid, 7, 4096, pmk_ap, 32);
    pbkdf2_sha1((const cu8 *)sta_pass, (int)strlen(sta_pass), ssid, 7, 4096, pmk_sta, 32);
    for (int i = 0; i < 32; i++) anonce[i] = (cu8)(i * 7 + 1), snonce[i] = (cu8)(i * 13 + 5);
    for (int i = 0; i < 16; i++) gtk[i] = (cu8)(0xa0 + i);
    static struct wpa_ap a;
    static struct wpa s;
    wpa_ap_init(&a, pmk_ap, aa, spa, ie, sizeof ie, anonce, gtk, 1);
    wpa_init(&s, pmk_sta, aa, spa, ie, sizeof ie, snonce);
    int r = wpa_ap_start(&a);                 /* 1/4 */
    int rs = wpa_rx(&s, a.tx, a.tx_len);      /* -> 2/4 */
    if (!(rs & WPA_TX)) return -1;
    r = wpa_ap_rx(&a, s.tx, s.tx_len);        /* -> 3/4 or bad MIC */
    if (r & WPA_AP_BADMIC) return -2;
    if (!(r & WPA_AP_TX)) return -3;
    rs = wpa_rx(&s, a.tx, a.tx_len);          /* -> 4/4 */
    if (!(rs & WPA_DONE) || !(rs & WPA_GTK)) return -4;
    r = wpa_ap_rx(&a, s.tx, s.tx_len);
    if (!(r & WPA_AP_DONE)) return -5;
    check(!memcmp(a.ptk, s.ptk, 48), "authenticator and supplicant derive the same PTK");
    check(!memcmp(s.gtk, gtk, 16) && s.gtk_id == 1, "station unwrapped our GTK (key id 1)");
    return 0;
}
int main(void)
{
    int r = run("secret12", "secret12");
    check(r == 0, "4-way handshake completes (1/4 .. 4/4)");
    if (r) printf("  stopped at step %d\n", -r);
    check(run("secret12", "wrongpass") == -2, "wrong passphrase rejected at message 2 (MIC)");
    printf("%d failures\n", fails);
    return fails != 0;
}
