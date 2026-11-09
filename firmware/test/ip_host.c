/* Host harness for ip.c, driven by test_ip.py. Commands (one per line), each answered by zero or more
 * "tx <dstmac> <type> <hex>" / "event ..." lines and then "end":
 *   init <mac> <token-hex> <static-ip-hex>   up   down   time <ms>   poll   rx <srcmac> <dstmac> <type> <hex>
 * Fake platform: 64 KB of "RAM" at 0x02000000, plat_cmd echoes the command ("big" -> 5000 bytes), a run counter. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../crypto.c"
#include "../ip.c"

static cu32 now_ms;
static int cmd_runs, syncs;
static cu8 ram[65536];
static cu8 rb[HU_MAX_REPLY + 256];

static void hex(const cu8 *p, int n)
{
    for (int i = 0; i < n; i++)
        printf("%02x", p[i]);
}

void plat_eth_send(const cu8 dst[6], int type, const cu8 *p, int len)
{
    printf("tx ");
    hex(dst, 6);
    printf(" %04x ", type);
    hex(p, len);
    printf("\n");
}
cu32 plat_ms(void) { return now_ms; }
cu32 plat_rand(void) { return 0x12345678; }
void plat_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  [ip] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
int plat_fmt(char *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(out, fmt, ap);
    va_end(ap);
    return n;
}
int plat_cmd(const char *c, cu8 *out, int max)
{
    cmd_runs++;
    int n = sprintf((char *)out, "ran:%s run#%d\n", c, cmd_runs);
    if (!strcmp(c, "big"))
        for (int i = 0; i < 5000 && n < max; i++)
            out[n++] = (cu8)('a' + i % 26);
    return n;
}
int plat_mem_ok(cu32 a, cu32 n, int w) { (void)w; return a >= 0x02000000 && a + n <= 0x02010000; }
void plat_mem_read(cu32 a, cu8 *out, int n) { memcpy(out, ram + (a - 0x02000000), n); }
void plat_mem_write(cu32 a, const cu8 *p, int n) { memcpy(ram + (a - 0x02000000), p, n); }
void plat_sync(cu32 a, cu32 n) { syncs++; fprintf(stderr, "  [ip] sync %x %u\n", a, n); }
int plat_log_tail(cu8 *out, int max)
{
    for (int i = 0; i < max; i++)
        out[i] = (cu8)(i * 7);
    return max;
}
int plat_status(char *out) { return sprintf(out, "up=%u hb=4 mcurx=5 rssi=-60", now_ms); }
void plat_bssid(cu8 out[6]) { memcpy(out, "\xc0\xc9\xe3\xa2\x99\x16", 6); }
int plat_op(int op, const cu8 *p, int n, cu8 *out, int max)
{
    (void)max;
    if (op != 7)
        return -1;
    int k = sprintf((char *)out, "op7:");
    for (int i = 0; i < n; i++)
        k += sprintf((char *)out + k, "%02x", p[i]);
    return k;
}
const char *plat_build(void) { return "20261031-120000"; }
int plat_hello_extra(char *out) { out[0] = 0; return 0; }
void plat_ip_event(cu32 ip, cu32 mask, cu32 router, int how) { printf("event %08x %08x %08x %d\n", ip, mask, router, how); }

static int unhex(const char *s, cu8 *o)
{
    int n = 0;
    unsigned v;
    while (s[0] && s[1] && s[0] != '\n' && s[0] != ' ' && sscanf(s, "%2x", &v) == 1)
        o[n++] = (cu8)v, s += 2;
    return n;
}

int main(void)
{
    static char line[300000];
    static cu8 a[6], b[6], p[150000], t[32];
    while (fgets(line, sizeof(line), stdin)) {
        char w1[64], w2[64], w3[64];
        if (!strncmp(line, "init ", 5)) {
            sscanf(line + 5, "%63s %63s %63s", w1, w2, w3);
            unhex(w1, a);
            memset(t, 0, 32);
            unhex(w2, t);
            unsigned sip;
            sscanf(w3, "%x", &sip);
            ip_init(a, t, sip, rb);
        } else if (!strncmp(line, "ap ", 3)) {
            unsigned ip;
            sscanf(line + 3, "%x", &ip);
            ip_ap_start(ip);
        } else if (!strncmp(line, "up", 2)) {
            ip_link_up();
        } else if (!strncmp(line, "time ", 5)) {
            sscanf(line + 5, "%u", &now_ms);
        } else if (!strncmp(line, "poll", 4)) {
            ip_poll();
        } else if (!strncmp(line, "stat", 4)) {
            printf("event runs %d syncs %d\n", cmd_runs, syncs);
        } else if (!strncmp(line, "rx ", 3)) {
            unsigned type;
            sscanf(line + 3, "%63s %63s %x", w1, w2, &type);
            unhex(w1, a);
            unhex(w2, b);
            const char *h = line + 3;
            for (int k = 0; k < 3; k++)
                h = strchr(h, ' ') + 1;
            int n = unhex(h, p);
            ip_rx(a, b, (int)type, p, n);
        }
        printf("end\n");
        fflush(stdout);
    }
    return 0;
}
