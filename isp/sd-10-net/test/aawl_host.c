/* Host test for aa_wl.c: plays the phone side of the wireless AA RFCOMM handshake, byte-split, and checks every
 * message the head unit sends against hand-encoded protobuf. */
#include "../aa_wl.c"
#include <stdio.h>
static unsigned char out[4][300];
static int outn[4], nout, fails;
static int snd(const unsigned char *p, int n) { memcpy(out[nout], p, (size_t)n); outn[nout++] = n; return n; }
static void expect(int i, const unsigned char *e, int n, const char *what)
{
    int ok = i < nout && outn[i] == n && !memcmp(out[i], e, (size_t)n);
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        fails++;
        for (int k = 0; i < nout && k < outn[i]; k++) printf("%02x", out[i][k]);
        printf("\n");
    }
}
int main(void)
{
    struct aa_wl_ap ap = { "MiataAA", "secret12", "02:11:22:33:44:55", "192.168.43.1", 5288 };
    aa_wl_rfcomm_connected(&ap, snd);
    /* WifiStartRequest: len 0x11, id 1, {1: "192.168.43.1", 2: 5288} */
    static const unsigned char start[] = { 0, 0x11, 0, 1, 0x0a, 12, '1', '9', '2', '.', '1', '6', '8', '.', '4', '3', '.', '1', 0x10, 0xa8, 0x29 };
    expect(0, start, sizeof start, "WifiStartRequest (ip, port 5288)");
    static const unsigned char req[] = { 0, 0, 0, 2 };   /* WifiInfoRequest, delivered one byte at a time */
    for (int i = 0; i < 4; i++)
        aa_wl_rfcomm_rx(req + i, 1);
    static const unsigned char info[] = { 0, 0x2a, 0, 3, 0x0a, 7, 'M', 'i', 'a', 't', 'a', 'A', 'A', 0x12, 8, 's', 'e', 'c', 'r', 'e', 't', '1', '2',
        0x1a, 17, '0', '2', ':', '1', '1', ':', '2', '2', ':', '3', '3', ':', '4', '4', ':', '5', '5', 0x20, 8, 0x28, 0 };
    expect(1, info, sizeof info, "WifiInfoResponse (ssid, key, bssid, WPA2 personal, static)");
    static const unsigned char st[] = { 0, 2, 0, 7, 0x18, 0, 0, 2, 0, 6, 0x08, 0 };   /* start response status 0, connect status 0 */
    aa_wl_rfcomm_rx(st, sizeof st);
    const struct aa_wl_status *s = aa_wl_status();
    int ok = s->state == AA_WL_PHONE_CONNECTED && s->rx == 3 && !s->errors && !s->unknown;
    printf("%s phone start response + connect status parsed (state %d, rx %u)\n", ok ? "PASS" : "FAIL", s->state, s->rx);
    fails += !ok;
    printf("%d failures\n", fails);
    return fails != 0;
}
