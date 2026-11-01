/* Host harness for wpa.c, driven line by line by test_wpa.py:
 *   init <pmk> <aa> <spa> <ie> <snonce>   -> "ok"
 *   rx <eapol>                            -> "act <n> protect <p> tx <hex|-> ptk <hex> gtk <id> <hex> rsc <hex>"  */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../crypto.c"
#include "../wpa.c"

void plat_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  [wpa] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int unhex(const char *s, unsigned char *o)
{
    int n = 0;
    unsigned v;
    while (s[0] && s[1] && s[0] != '\n' && sscanf(s, "%2x", &v) == 1)
        o[n++] = (unsigned char)v, s += 2;
    return n;
}

static void hex(const unsigned char *p, int n)
{
    if (!n)
        printf("-");
    for (int i = 0; i < n; i++)
        printf("%02x", p[i]);
}

int main(void)
{
    static struct wpa w;
    static char line[8192], a[5][2048];
    static unsigned char b[5][1024];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "init ", 5)) {
            sscanf(line + 5, "%2047s %2047s %2047s %2047s %2047s", a[0], a[1], a[2], a[3], a[4]);
            int n[5];
            for (int i = 0; i < 5; i++)
                n[i] = unhex(a[i], b[i]);
            wpa_init(&w, b[0], b[1], b[2], b[3], n[3], b[4]);
            printf("ok\n");
        } else if (!strncmp(line, "rx ", 3)) {
            static unsigned char e[2048];
            int n = unhex(line + 3, e);
            int act = wpa_rx(&w, e, n);
            printf("act %d protect %d tx ", act, w.tx_protect);
            hex(w.tx, act & WPA_TX ? w.tx_len : 0);
            printf(" ptk ");
            hex(w.ptk, 48);
            printf(" gtk %d ", w.gtk_id);
            hex(w.gtk, 16);
            printf(" rsc ");
            hex(w.gtk_rsc, 6);
            printf("\n");
        }
        fflush(stdout);
    }
    return 0;
}
