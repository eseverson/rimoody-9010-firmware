/* Host driver for ap80211.c: writes our AP's frames to a pcap (802.11, linktype 105) for tshark, and parses a set of
 * phone-side frames, printing the result. Used by test_ap80211.py. */
#include "../ap80211.c"
#include <stdio.h>
static void rec(FILE *f, const unsigned char *d, int n)
{
    unsigned int h[4] = { 0, 0, (unsigned)n, (unsigned)n };
    fwrite(h, 4, 4, f);
    fwrite(d, 1, (size_t)n, f);
}
int main(int argc, char **argv)
{
    struct ap_cfg c = { "MiataAA", { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 }, 6 };
    static const unsigned char phone[6] = { 0x3a, 1, 2, 3, 4, 5 };
    unsigned char f[256];
    FILE *o = fopen(argv[1], "wb");
    unsigned int gh[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 105 };
    fwrite(gh, 4, 6, o);
    rec(o, f, ap_beacon(f, &c));
    rec(o, f, ap_probe_resp(f, &c, phone, 1));
    rec(o, f, ap_auth_resp(f, &c, phone, 2, 0));
    rec(o, f, ap_assoc_resp(f, &c, phone, 3, 0, 1, 0));
    rec(o, f, ap_deauth(f, &c, phone, 4, 3));
    fclose(o);
    /* phone frames: wildcard probe, probe for another SSID, auth, assoc with RSN + HT, deauth */
    struct ap_rx r;
    static const unsigned char probe_any[] = { 0x40, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3a, 1, 2, 3, 4, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 1, 1, 0x82 };
    static const unsigned char probe_other[] = { 0x40, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3a, 1, 2, 3, 4, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 4, 'h', 'o', 'm', 'e' };
    static const unsigned char auth[] = { 0xb0, 0, 0, 0, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0x3a, 1, 2, 3, 4, 5, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0, 0, 1, 0, 0, 0 };
    unsigned char assoc[128] = { 0x00, 0, 0, 0, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0x3a, 1, 2, 3, 4, 5, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 0x31, 0x04, 10, 0 };
    int an = 28;
    assoc[an++] = 0, assoc[an++] = 7, memcpy(assoc + an, "MiataAA", 7), an += 7;
    an += ap_rsn_ie(assoc + an);
    assoc[an++] = 45, assoc[an++] = 26, memset(assoc + an, 0, 26), an += 26;
    static const unsigned char deauth[] = { 0xc0, 0, 0, 0, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0x3a, 1, 2, 3, 4, 5, 2, 0x11, 0x22, 0x33, 0x44, 0x55, 0, 0, 3, 0 };
    printf("probe_any %d\n", ap_parse(probe_any, sizeof probe_any, &c, &r));
    printf("probe_other %d\n", ap_parse(probe_other, sizeof probe_other, &c, &r));
    int t = ap_parse(auth, sizeof auth, &c, &r);
    printf("auth %d alg %d\n", t, r.auth_alg);
    t = ap_parse(assoc, an, &c, &r);
    printf("assoc %d rsn %d ht %d\n", t, r.rsn_len, r.ht);
    t = ap_parse(deauth, sizeof deauth, &c, &r);
    printf("deauth %d reason %d\n", t, r.reason);
    return 0;
}
