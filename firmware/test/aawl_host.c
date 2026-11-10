/* Host test for aa_wl.c: replays the phone side of stock eCos's wireless AA RFCOMM handshake (logic-analyzer capture
 * 2026-10-31, Pixel 10 Pro, notes/android_auto.md), byte-split, and checks that every message we send is byte for byte
 * what stock sent with the same parameters. */
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
static void feed(const unsigned char *p, int n, int split)
{
    for (int i = 0; i < n; i += split)
        aa_wl_rfcomm_rx(p + i, n - i < split ? n - i : split);
}
int main(void)
{
    struct aa_wl_ap ap = { "DIRECT-phonelink-d9249c", "88888888", "02:c0:ff:ee:00:01", "192.168.0.1", 8765, 8461480 };
    aa_wl_rfcomm_connected(&ap, snd);
    static const unsigned char ver[] = { 0x00, 0x09, 0x00, 0x04, 0x10, 0x00, 0x18, 0x00, 0x20, 0xa8, 0xb9, 0x84, 0x04 };
    expect(0, ver, sizeof ver, "WifiVersionRequest as stock");
    static const unsigned char vresp[] = { 0x00, 0x16, 0x00, 0x05, 0x08, 0x00, 0x10, 0x00, 0x1a, 0x0e, '5', '7', '2', '1', '1', 'F',
                                           'D', 'C', 'H', '0', '0', '5', 'L', '7', 0x20, 0x00 };
    feed(vresp, sizeof vresp, 1);
    static const unsigned char start[] = { 0x00, 0x10, 0x00, 0x01, 0x0a, 0x0b, '1', '9', '2', '.', '1', '6', '8', '.', '0', '.', '1',
                                           0x10, 0xbd, 0x44 };
    expect(1, start, sizeof start, "WifiStartRequest after the version response, as stock");
    static const unsigned char req[] = { 0x00, 0x00, 0x00, 0x02 };
    feed(req, sizeof req, 1);
    static const unsigned char info[] = { 0x00, 0x38, 0x00, 0x03, 0x0a, 0x17, 'D', 'I', 'R', 'E', 'C', 'T', '-', 'p', 'h', 'o', 'n', 'e',
                                          'l', 'i', 'n', 'k', '-', 'd', '9', '2', '4', '9', 'c', 0x12, 0x08, '8', '8', '8', '8', '8',
                                          '8', '8', '8', 0x1a, 0x11, '0', '2', ':', 'c', '0', ':', 'f', 'f', ':', 'e', 'e', ':', '0',
                                          '0', ':', '0', '1', 0x20, 0x08 };
    expect(2, info, sizeof info, "WifiInfoResponse as stock");
    static const unsigned char st[] = { 0x00, 0x02, 0x00, 0x07, 0x18, 0x00, 0x00, 0x02, 0x00, 0x06, 0x08, 0x00 };
    feed(st, sizeof st, 5);
    const struct aa_wl_status *s = aa_wl_status();
    int ok = s->state == AA_WL_PHONE_CONNECTED && s->rx == 4 && !s->errors && !s->unknown && nout == 3;
    printf("%s phone start response + connect status parsed (state %d, rx %u, sent %d)\n", ok ? "PASS" : "FAIL", s->state, s->rx, nout);
    fails += !ok;
    printf("%d failures\n", fails);
    return fails != 0;
}
