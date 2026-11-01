/* Host harness for console.c, driven by test_console.py: commands panel <row> <fg hex> <text>, log <text> (appends a
 * line), scale <n>, render -> "rows <n> flushes <bytes>", dump <file> (raw 1024x600 little-endian ARGB words). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../console.c"

static unsigned int frame[CON_W * CON_H];
static char logbuf[1 << 20];
static unsigned int loglen, flushed;

static void flush(unsigned int addr, unsigned int len) { (void)addr; flushed += len; }

int main(void)
{
    static char line[4096];
    con_init(frame);
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "panel ", 6)) {
            int row;
            unsigned fg;
            int off = 0;
            sscanf(line + 6, "%d %x %n", &row, &fg, &off);
            con_panel(row, line + 6 + off, fg);
        } else if (!strncmp(line, "log ", 4)) {
            int n = sprintf(logbuf + loglen, "%s\n", line + 4);
            loglen += n;
            con_log_source(logbuf, loglen);
        } else if (!strncmp(line, "scale ", 6)) {
            con_log_scale(atoi(line + 6));
        } else if (!strcmp(line, "render")) {
            flushed = 0;
            int n = con_render(flush);
            printf("rows %d flushes %u\n", n, flushed);
        } else if (!strncmp(line, "dump ", 5)) {
            FILE *f = fopen(line + 5, "wb");
            fwrite(frame, 4, CON_W * CON_H, f);
            fclose(f);
        }
        printf("end\n");
        fflush(stdout);
    }
    return 0;
}
