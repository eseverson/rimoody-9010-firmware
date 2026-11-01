/* See console.h. Font: font8x16.h (gen_font.py, public-domain LatGrkCyr-8x16 from kbd). */
#include "console.h"
#include "font8x16.h"

typedef unsigned int u32;

#define PANEL_SCALE 2
#define PANEL_Y 0
#define RULE_Y (CON_PANEL_ROWS * 16 * PANEL_SCALE)
#define RULE_H 4
#define LOG_Y (RULE_Y + RULE_H + 4)
#define MAX_ROWS 32

struct row {
    int y, scale;
    u32 fg, bg;
    char text[CON_MAX_COLS + 1];
};

static u32 *fb;
static struct row want[MAX_ROWS], shown[MAX_ROWS];
static int log_scale = 2, layout_dirty = 1, n_log_rows;
static const char *log_buf;
static u32 log_len, log_seen = ~0u;

static int cols_for(int scale) { return CON_W / (8 * scale); }

static void copy_text(char *d, const char *s, int max)
{
    int i = 0;
    for (; s && s[i] && i < max; i++)
        d[i] = s[i];
    d[i] = 0;
}

static int same_row(const struct row *a, const struct row *b)
{
    if (a->y != b->y || a->scale != b->scale || a->fg != b->fg || a->bg != b->bg)
        return 0;
    for (int i = 0; i <= CON_MAX_COLS; i++) {
        if (a->text[i] != b->text[i])
            return 0;
        if (!a->text[i])
            return 1;
    }
    return 1;
}

static void fill(int y0, int h, u32 c)
{
    for (int y = y0; y < y0 + h && y < CON_H; y++)
        for (int x = 0; x < CON_W; x++)
            fb[y * CON_W + x] = c;
}

static void draw_row(const struct row *r)
{
    int s = r->scale, cols = cols_for(s), h = 16 * s, end = 0;
    while (end < cols && r->text[end])
        end++;
    for (int gy = 0; gy < 16; gy++) {
        for (int sy = 0; sy < s; sy++) {
            u32 *line = fb + (r->y + gy * s + sy) * CON_W;
            if (r->y + gy * s + sy >= CON_H)
                return;
            for (int c = 0; c < cols; c++) {
                unsigned char ch = c < end ? (unsigned char)r->text[c] : ' ';
                const unsigned char *g = font8x16[ch >= 0x20 && ch < 0x7f ? ch - 0x20 : 95];
                unsigned bits = g[gy];
                u32 *p = line + c * 8 * s;
                for (int b = 0; b < 8; b++) {
                    u32 col = bits & (0x80 >> b) ? r->fg : r->bg;
                    for (int sx = 0; sx < s; sx++)
                        *p++ = col;
                }
            }
        }
    }
    (void)h;
}

void con_init(u32 *frame)
{
    fb = frame;
    fill(0, RULE_Y, CON_PANEL_BG);
    fill(RULE_Y, RULE_H, CON_RULE);
    fill(RULE_Y + RULE_H, CON_H - RULE_Y - RULE_H, CON_LOG_BG);
    for (int i = 0; i < MAX_ROWS; i++)
        shown[i].y = -1;
    for (int i = 0; i < CON_PANEL_ROWS; i++) {
        want[i].y = PANEL_Y + i * 16 * PANEL_SCALE, want[i].scale = PANEL_SCALE;
        want[i].bg = CON_PANEL_BG;
        if (!want[i].fg)
            want[i].fg = CON_WHITE;
    }
    layout_dirty = 1;
    log_seen = ~0u;
}

void con_panel(int row, const char *text, u32 fg)
{
    if (row < 0 || row >= CON_PANEL_ROWS)
        return;
    copy_text(want[row].text, text, cols_for(PANEL_SCALE));
    want[row].fg = fg;
}

void con_log_scale(int scale)
{
    if ((scale == 1 || scale == 2) && scale != log_scale) {
        log_scale = scale;
        layout_dirty = 1;
    }
}

int con_get_log_scale(void) { return log_scale; }

void con_log_source(const char *buf, u32 len) { log_buf = buf, log_len = len; }

/* the newest lines of the log, wrapped at the row width, bottom-aligned */
static void layout_log(void)
{
    int cols = cols_for(log_scale), rows = (CON_H - LOG_Y) / (16 * log_scale);
    if (rows > MAX_ROWS - CON_PANEL_ROWS)
        rows = MAX_ROWS - CON_PANEL_ROWS;
    if (layout_dirty) {   /* old rows beyond the new layout: clear the region once */
        for (int i = CON_PANEL_ROWS; i < MAX_ROWS; i++)
            want[i].y = -1, shown[i].y = -1;
        fill(LOG_Y, CON_H - LOG_Y, CON_LOG_BG);
        n_log_rows = rows;
        layout_dirty = 0;
    }
    static char seg[MAX_ROWS][CON_MAX_COLS + 1];
    int nseg = 0;
    u32 end = log_len;   /* exclusive end of the current line's text */
    if (log_buf && end > 0 && log_buf[end - 1] == '\n')
        end--;
    while (log_buf && nseg < rows) {
        u32 start = end;
        while (start > 0 && log_buf[start - 1] != '\n')
            start--;
        u32 n = end - start;   /* wrapped into pieces of `cols`; the last pieces are the newest */
        int pieces = n ? (int)((n + (u32)cols - 1) / (u32)cols) : 1;
        for (int p = pieces - 1; p >= 0 && nseg < rows; p--) {
            u32 off = start + (u32)p * (u32)cols, m = n - (u32)p * (u32)cols;
            if (m > (u32)cols)
                m = (u32)cols;
            for (u32 i = 0; i < m; i++) {
                char c = log_buf[off + i];
                seg[nseg][i] = c >= 0x20 && c < 0x7f ? c : (c == '\t' ? ' ' : '.');
            }
            seg[nseg][m] = 0;
            nseg++;
        }
        if (start == 0)
            break;
        end = start - 1;   /* the newline before this line ends the previous one */
    }
    for (int i = 0; i < rows; i++) {
        struct row *r = &want[CON_PANEL_ROWS + i];
        r->y = LOG_Y + i * 16 * log_scale, r->scale = log_scale, r->fg = CON_GREY, r->bg = CON_LOG_BG;
        int k = rows - 1 - i;   /* segment index: 0 = newest, at the bottom */
        copy_text(r->text, k < nseg ? seg[k] : "", cols);
        if (r->text[0] == '!' || (r->text[0] == 'j' && r->text[1] == 'o'))
            r->fg = CON_YELLOW;
    }
}

int con_render(void (*flush)(u32 addr, u32 len))
{
    if (!fb)
        return 0;
    if (log_len != log_seen || layout_dirty) {
        layout_log();
        log_seen = log_len;
    }
    int n = 0, total = CON_PANEL_ROWS + n_log_rows;
    for (int i = 0; i < total; i++) {
        if (want[i].y < 0 || same_row(&want[i], &shown[i]))
            continue;
        draw_row(&want[i]);
        shown[i] = want[i];
        if (flush)
            flush((u32)(unsigned long)(fb + want[i].y * CON_W), (u32)(16 * want[i].scale * CON_W * 4));
        n++;
    }
    return n;
}
