/* Text console on a 1024x600 ARGB8888 frame (OSD0 on the head unit; a plain array in the host test). Two regions: a
 * status panel (CON_PANEL_ROWS rows at scale 2) and a scrolling tail of the log (scale 1 or 2, long lines wrap).
 * Only rows whose text or colors changed are redrawn; the caller's flush() gets each redrawn pixel range so it can
 * write the D-cache back. Platform independent (test/con_host.c builds it on the host). */
#ifndef CONSOLE_H
#define CONSOLE_H

#define CON_W 1024
#define CON_H 600
#define CON_PANEL_ROWS 6
#define CON_MAX_COLS 128

/* colors: 0xAARRGGBB as little-endian words, alpha must be 0xFF (OSD0 blends per pixel) */
#define CON_WHITE  0xFFFFFFFFu
#define CON_GREY   0xFFC0C0C0u
#define CON_YELLOW 0xFFFFE060u
#define CON_GREEN  0xFF70FF70u
#define CON_RED    0xFFFF6060u
#define CON_CYAN   0xFF60E0FFu
#define CON_PANEL_BG 0xFF102040u
#define CON_LOG_BG   0xFF000000u
#define CON_RULE     0xFF4080C0u

void con_init(unsigned int *fb);   /* clears the frame and forgets what is shown */
void con_panel(int row, const char *text, unsigned int fg);
void con_log_scale(int scale);      /* 1 or 2 */
int con_get_log_scale(void);
/* log tail source: a byte buffer (the payload's log) and its length; the newest lines are shown */
void con_log_source(const char *buf, unsigned int len);
/* redraw what changed; returns the number of text rows redrawn */
int con_render(void (*flush)(unsigned int addr, unsigned int len));

#endif
