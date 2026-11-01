/* Host harness for mcu.c, driven by test_mcu.py. Commands, each answered by "w <frame hex>" / "shutdown" lines and
 * then "end": rx <ms> <hex>, poll <ms>, settime <ms> y m d h mi s, send <ms> <hex>, allowed <hex>, state <ms>,
 * frame <hex>. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../crypto.h"
#include "../mcu.c"

void plat_mcu_write(const cu8 *f, int n)
{
    printf("w ");
    for (int i = 0; i < n; i++)
        printf("%02x", f[i]);
    printf("\n");
}
void plat_mcu_shutdown(void) { printf("shutdown\n"); }
void plat_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  [mcu] ");
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

static int unhex(const char *s, cu8 *o)
{
    int n = 0;
    unsigned v;
    while (*s == ' ')
        s++;
    while (s[0] && s[1] && s[0] != '\n' && sscanf(s, "%2x", &v) == 1)
        o[n++] = (cu8)v, s += 2;
    return n;
}

int main(void)
{
    static struct mcu m;
    static char line[65536], out[8192];
    static cu8 b[8192];
    mcu_init(&m);
    while (fgets(line, sizeof(line), stdin)) {
        char op[16];
        unsigned t = 0;
        int off = 0;
        sscanf(line, "%15s %n", op, &off);
        if (!strcmp(op, "rx")) {
            int o2 = 0;
            sscanf(line + off, "%u %n", &t, &o2);
            int n = unhex(line + off + o2, b);
            for (int i = 0; i < n; i++)
                mcu_rx_byte(&m, b[i], t);
        } else if (!strcmp(op, "poll")) {
            sscanf(line + off, "%u", &t);
            mcu_poll(&m, t);
        } else if (!strcmp(op, "settime")) {
            unsigned v[6];
            sscanf(line + off, "%u %u %u %u %u %u %u", &t, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            cu8 c[6];
            for (int i = 0; i < 6; i++)
                c[i] = (cu8)v[i];
            mcu_settime(&m, c, t);
        } else if (!strcmp(op, "send")) {
            int o2 = 0;
            sscanf(line + off, "%u %n", &t, &o2);
            int n = unhex(line + off + o2, b);
            mcu_send(&m, b, n, t);
        } else if (!strcmp(op, "allowed")) {
            int n = unhex(line + off, b);
            printf("allowed %d\n", mcu_tx_allowed(b, n));
        } else if (!strcmp(op, "state")) {
            sscanf(line + off, "%u", &t);
            mcu_state(&m, out, t);
            for (char *p = out; *p; p++)
                if (*p == '\n')
                    *p = '|';
            printf("state %s\n", out);
        } else if (!strcmp(op, "frame")) {
            int n = unhex(line + off, b);
            cu8 f[256];
            int k = mcu_frame(b, n, f);
            printf("frame ");
            for (int i = 0; i < k; i++)
                printf("%02x", f[i]);
            printf("\n");
        }
        printf("end\n");
        fflush(stdout);
    }
    return 0;
}
