/* The head unit's user interface: LVGL v9 drawing ARGB8888 straight into the OSD0 framebuffer, Android Auto video on
 * VPP0 underneath (vpp.c). Design and checklist: docs/superpowers/specs/2026-11-02-head-unit-ui-design.md.
 *
 * Screens: home (the Miata logo, white on black, with a faint dock), Android Auto (transparent over the video; a
 * "waiting for phone" card when there is no session), Settings, Console (the log tail). The volume overlay lives on
 * LVGL's top layer. Front buttons: 1 mute (long press: reboot), 2 home, 3 back (the phone's back key in AA), 4/5
 * volume. Only the UI task touches LVGL; other tasks reach it through ui_pointer / ui_button / ui_aa_session. */
#include "lvgl.h"
#include "ui.h"
#include "clk.h"
#include "memmap.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#define PANEL_W 1024
#define PANEL_H 600
#define VOL_MAX 40   /* eCos's DSP volume index scale (notes/audio.md) */

extern const lv_image_dsc_t ui_logo;

static unsigned int fb;
static volatile int running;
static volatile int ptr_down;
static volatile unsigned int ptr_x, ptr_y;
static QueueHandle_t btn_q;
static volatile int aa_session, aa_session_seen;
static volatile int aa_view;
static int aa_enabled = 1;
static int aa_dpi = 160;   /* screen density Android Auto lays its UI out for: lower = smaller elements, more map */
#define AA_DPI_MIN 100
#define BT_NAME_MAX 20
static char bt_name[BT_NAME_MAX + 1] = "Miata";   /* the Bluetooth name phones see (sent to the AC6965A at boot) */
#define AA_DPI_MAX 240
static int volume = 20, muted, dark = 1;
/* display settings (UI.TXT): backlight in percent by day and with the headlights on; theme 0 light, 1 dark, 2 auto
 * (dark while the headlights are on) */
static int bright_day = 100, bright_night = 40, theme_mode = 1;
static int lights;   /* debounced headlight input */
static int boot_console = 1;
/* equalizer: 10 octave bands (audio.c), -12..+12 dB each; presets are ours, not stock's */
#define EQ_N 10
static signed char eq_db[EQ_N];
static const char *const eq_label[EQ_N] = { "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
static const signed char eq_preset[][EQ_N] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },           /* Flat */
    { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0 },           /* Bass */
    { 5, 4, 2, 0, -1, -1, 0, 2, 4, 5 },         /* Loud: both ends up, for low listening levels */
    { -2, -1, 0, 1, 3, 4, 3, 1, 0, -1 },        /* Voice */
    { 0, 0, 0, 0, 0, 0, 1, 3, 5, 6 },           /* Treble */
};
static const char *const eq_preset_map[] = { "Flat", "Bass", "Loud", "Voice", "Treble", "" };
#define EQ_PRESETS 5   /* UI.TXT boot=1: power up into the text console (Home goes to the home screen) */
static char settings_text[256];
static void settings_save(void);

static lv_obj_t *scr_home, *scr_aa, *scr_settings, *scr_wifi, *scr_btname;
static lv_obj_t *bt_ta, *bt_msg, *bt_label;
static lv_obj_t *wifi_ssid, *wifi_psk, *wifi_kb, *wifi_msg;
static lv_obj_t *aa_card, *status_label, *vol_panel, *vol_bar, *vol_icon, *vol_num;
static lv_timer_t *vol_timer;
static lv_obj_t *dialog;   /* an open confirmation; any navigation closes it */

static void dialog_close(void)
{
    if (dialog)
        lv_msgbox_close(dialog);
    dialog = 0;
}

int ui_running(void) { return running; }
int ui_aa_visible(void) { return aa_view && aa_session; }

void ui_pointer(int action, unsigned x, unsigned y)
{
    if (x >= PANEL_W)
        x = PANEL_W - 1;
    if (y >= PANEL_H)
        y = PANEL_H - 1;
    ptr_x = x, ptr_y = y;
    ptr_down = action != 1;
}

void ui_button(int button, int pressed)
{
    if (btn_q) {
        unsigned char e = (unsigned char)(button << 1 | (pressed != 0));
        xQueueSend(btn_q, &e, 0);
    }
}

void ui_aa_session(int active)
{
    if (aa_session != active)
        aa_session = active, aa_session_seen++;
}

void ui_assert_failed(void)   /* LVGL logged the failed check just before; park the UI task, keep the unit up */
{
    plat_ui_log("ui: LVGL assertion failed, UI stopped\n");
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

static void log_cb(lv_log_level_t level, const char *buf)
{
    (void)level;
    plat_ui_log(buf);
}

/* ---- display and input drivers ---- */

static void dcache_wb_range(unsigned int a, unsigned int n)
{
    for (unsigned int p = a & ~31u; p < a + n; p += 32)
        __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(p) : "memory");
}

/* LVGL renders each dirty area completely in an off-screen buffer; only finished pixels are copied into the
 * framebuffer OSD0 scans, so a scroll never shows a half-drawn row (owner: settings flashed while scrolling) */
#define DRAW_BUF MM_LVGL_DRAW   /* memmap.h: after the LVGL heap */
#define DRAW_ROWS 120
_Static_assert(PANEL_W * DRAW_ROWS * 4 <= MM_LVGL_DRAW_SZ, "LVGL draw buffer outgrows MM_LVGL_DRAW");
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    unsigned int w = (unsigned int)(a->x2 - a->x1 + 1);
    const unsigned int *src = (const unsigned int *)px;
    for (int y = a->y1; y <= a->y2; y++) {
        volatile unsigned int *dst = (volatile unsigned int *)(fb + ((unsigned int)y * PANEL_W + (unsigned int)a->x1) * 4);
        for (unsigned int i = 0; i < w; i++)
            dst[i] = src[i];
        src += w;
        dcache_wb_range((unsigned int)dst, w * 4);
    }
    __asm__ volatile("msync all" ::: "memory");
    lv_display_flush_ready(d);
}

static uint32_t tick_cb(void) { return (uint32_t)(clk_us() / 1000); }

static void pointer_cb(lv_indev_t *i, lv_indev_data_t *d)
{
    (void)i;
    d->point.x = (int32_t)ptr_x, d->point.y = (int32_t)ptr_y;
    d->state = ptr_down && !ui_aa_visible() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---- screens ---- */

static void show_home(void);
static void show_aa(void);
static void show_settings(void);
static void show_console(void);

static lv_obj_t *screen(int black)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    if (black) {
        lv_obj_set_style_bg_color(s, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    }
    return s;
}

static void dock_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which == 0)
        show_aa();
    else
        show_settings();
}

static lv_obj_t *logo(lv_obj_t *parent, lv_opa_t opa)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &ui_logo);
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(img, opa, 0);
    return img;
}

static void build_home(void)
{
    scr_home = screen(1);
    lv_obj_align(logo(scr_home, LV_OPA_COVER), LV_ALIGN_CENTER, 0, -40);
    /* dock A: faint round buttons along the bottom, brighter while pressed */
    lv_obj_t *dock = lv_obj_create(scr_home);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(dock, 12, 0);
    lv_obj_set_style_pad_column(dock, 32, 0);
    lv_obj_set_style_radius(dock, 44, 0);
    lv_obj_set_style_bg_color(dock, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_10, 0);
    lv_obj_set_style_border_width(dock, 0, 0);
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -24);
    static const char *const icons[] = { LV_SYMBOL_GPS, LV_SYMBOL_SETTINGS };   /* the log view lives in Settings > System */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(dock);
        lv_obj_set_size(b, 72, 72);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, icons[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_opa(l, LV_OPA_60, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, dock_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void build_aa(void)
{
    scr_aa = screen(0);
    lv_obj_set_style_bg_opa(scr_aa, LV_OPA_TRANSP, 0);   /* the video on VPP0 shows through */
    aa_card = lv_obj_create(scr_aa);   /* covers the panel while there is no phone */
    lv_obj_remove_flag(aa_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(aa_card, PANEL_W, PANEL_H);
    lv_obj_set_style_radius(aa_card, 0, 0);
    lv_obj_set_style_border_width(aa_card, 0, 0);
    lv_obj_set_style_bg_color(aa_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(aa_card, LV_OPA_COVER, 0);
    lv_obj_align(logo(aa_card, LV_OPA_30), LV_ALIGN_CENTER, 0, -60);
    lv_obj_t *t = lv_label_create(aa_card);
    lv_label_set_text(t, "Android Auto\nwaiting for the phone");
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_style_text_opa(t, LV_OPA_70, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 90);
}

static void aa_switch_cb(lv_event_t *e)
{
    aa_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    plat_ui_aa_enable(aa_enabled);
    settings_save();
}

/* the phone reads the density when it connects, so a change applies from the next connection */
static void aa_dpi_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e)) * 10;
    lv_label_set_text_fmt(label, "%d dpi", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && v != aa_dpi) {
        aa_dpi = v;
        plat_ui_aa_dpi(aa_dpi);
        settings_save();
    }
}

/* ---- settings file and display policy ---- */

static int kv_int(const char *t, const char *key, int dflt, int lo, int hi)   /* "key=123" at a line start */
{
    for (const char *p = t; *p;) {
        int i = 0;
        while (key[i] && p[i] == key[i])
            i++;
        if (!key[i] && p[i] == '=') {
            int v = 0, any = 0;
            for (p += i + 1; *p >= '0' && *p <= '9'; p++)
                v = v * 10 + (*p - '0'), any = 1;
            return !any ? dflt : v < lo ? lo : v > hi ? hi : v;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return dflt;
}

static void kv_list(const char *t, const char *key, signed char *out, int n, int lo, int hi)   /* "key=1,-2,3" */
{
    for (const char *p = t; *p;) {
        int i = 0;
        while (key[i] && p[i] == key[i])
            i++;
        if (!key[i] && p[i] == '=') {
            p += i + 1;
            for (int k = 0; k < n && *p && *p != '\n'; k++) {
                int neg = *p == '-', v = 0;
                if (neg)
                    p++;
                while (*p >= '0' && *p <= '9')
                    v = v * 10 + (*p++ - '0');
                v = neg ? -v : v;
                out[k] = (signed char)(v < lo ? lo : v > hi ? hi : v);
                if (*p == ',')
                    p++;
            }
            return;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
}

static void kv_str(const char *t, const char *key, char *out, int max)   /* "key=text" up to the end of the line */
{
    for (const char *p = t; *p;) {
        int i = 0;
        while (key[i] && p[i] == key[i])
            i++;
        if (!key[i] && p[i] == '=') {
            p += i + 1;
            int k = 0;
            while (k < max && p[k] >= 0x20 && p[k] < 0x7f)
                out[k] = p[k], k++;
            if (k)
                out[k] = 0;
            return;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
}

static void settings_load(void)
{
    bright_day = kv_int(settings_text, "day", bright_day, 0, 100);
    bright_night = kv_int(settings_text, "night", bright_night, 0, 100);
    theme_mode = kv_int(settings_text, "theme", theme_mode, 0, 2);
    aa_enabled = kv_int(settings_text, "aa", aa_enabled, 0, 1);
    aa_dpi = kv_int(settings_text, "aadpi", aa_dpi, AA_DPI_MIN, AA_DPI_MAX) / 10 * 10;
    kv_str(settings_text, "btname", bt_name, BT_NAME_MAX);
    volume = kv_int(settings_text, "vol", volume, 0, VOL_MAX);
    boot_console = kv_int(settings_text, "boot", boot_console, 0, 1);
    kv_list(settings_text, "eq", eq_db, EQ_N, -12, 12);
    plat_ui_aa_enable(aa_enabled);
    plat_ui_aa_dpi(aa_dpi);
    plat_ui_bt_name(bt_name);
}

static void settings_save(void)
{
    char t[256];
    int k = lv_snprintf(t, sizeof t, "day=%d\nnight=%d\ntheme=%d\naa=%d\naadpi=%d\nbtname=%s\nvol=%d\nboot=%d\neq=",
                        bright_day, bright_night, theme_mode, aa_enabled, aa_dpi, bt_name, volume, boot_console);
    for (int i = 0; i < EQ_N; i++)
        k += lv_snprintf(t + k, sizeof t - (unsigned)k, "%d%s", eq_db[i], i < EQ_N - 1 ? "," : "\n");
    plat_ui_settings_save(t);
}

static void apply_backlight(void) { plat_ui_brightness(lights ? bright_night : bright_day); }

static void apply_theme(void);
static void theme_update(void)   /* dark follows the mode (and the headlights in auto) */
{
    int want = theme_mode == 2 ? lights : theme_mode;
    if (want != dark) {
        dark = want;
        apply_theme();
    }
}

/* ---- colours: explicit per mode (the owner found light mode too low in contrast with translucent text) ---- */

static lv_style_t st_scr, st_row, st_section, st_nav, st_nav_sel, st_title;

static void style_colors(void)
{
    lv_color_t scr = dark ? lv_color_hex(0x15171a) : lv_color_hex(0xd9dde3);
    lv_color_t row_bg = dark ? lv_color_hex(0x272a30) : lv_color_hex(0xffffff);
    lv_color_t border = dark ? lv_color_hex(0x3b3f47) : lv_color_hex(0xaab2bd);
    lv_color_t text = dark ? lv_color_hex(0xf2f2f2) : lv_color_hex(0x101214);
    lv_color_t muted = dark ? lv_color_hex(0xa7adb5) : lv_color_hex(0x3f4650);
    lv_style_set_bg_color(&st_scr, scr);
    lv_style_set_bg_opa(&st_scr, LV_OPA_COVER);
    lv_style_set_text_color(&st_scr, text);
    lv_style_set_bg_color(&st_row, row_bg);
    lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
    lv_style_set_border_color(&st_row, border);
    lv_style_set_border_width(&st_row, 1);
    lv_style_set_radius(&st_row, 10);
    lv_style_set_text_color(&st_row, text);
    lv_style_set_text_color(&st_section, muted);
    lv_style_set_text_font(&st_section, &lv_font_montserrat_14);
    lv_style_set_text_color(&st_title, text);
    lv_style_set_text_font(&st_title, &lv_font_montserrat_28);
    lv_style_set_bg_opa(&st_nav, LV_OPA_TRANSP);
    lv_style_set_shadow_width(&st_nav, 0);
    lv_style_set_text_color(&st_nav, text);
    lv_style_set_radius(&st_nav, 10);
    lv_style_set_bg_color(&st_nav_sel, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_bg_opa(&st_nav_sel, LV_OPA_COVER);
    lv_style_set_text_color(&st_nav_sel, lv_color_white());
}

static void apply_theme(void)
{
    static int inited;
    if (!inited) {
        lv_style_init(&st_scr), lv_style_init(&st_row), lv_style_init(&st_section);
        lv_style_init(&st_nav), lv_style_init(&st_nav_sel), lv_style_init(&st_title);
        inited = 1;
    }
    lv_display_t *d = lv_display_get_default();
    lv_theme_t *t = lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_GREY), dark,
                                          &lv_font_montserrat_20);
    lv_display_set_theme(d, t);
    style_colors();
    /* the bottom layer is created like a screen and picks up the theme's opaque screen colour; it must stay
     * transparent so a transparent screen (the AA view) shows the video on VPP0 */
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_TRANSP, 0);
    lv_obj_report_style_change(0);
}

/* brightness sliders: user data 0 day, 1 headlights on. Dragging previews that level on the backlight; letting go
 * stores it and goes back to the level that applies now. */
static void bright_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        plat_ui_brightness(v);
        return;
    }
    if (which)
        bright_night = v;
    else
        bright_day = v;
    apply_backlight();
    settings_save();
}

static void theme_cb(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (i > 2)
        return;
    theme_mode = (int)i;
    theme_update();
    settings_save();
}

/* the headlight input, every 200 ms; a change counts after 5 equal readings in a row (eCos does the same) */
static void lights_timer(lv_timer_t *t)
{
    (void)t;
    static int last = -1, same;
    int v = plat_ui_lights() ? 1 : 0;
    same = v == last ? same + 1 : 1;
    last = v;
    if (same == 5 && v != lights) {
        lights = v;
        apply_backlight();
        plat_ui_night(lights);
        theme_update();
    }
}

/* ---- console mode: the original text console (console.c, drawn by net.c's console task) takes the panel; the UI
 * stops drawing until Home, Back or a tap ---- */

static volatile int console_on;
int ui_console_active(void) { return console_on; }

static void show_console(void)
{
    dialog_close();
    console_on = 1;
}

static void leave_console(void)
{
    console_on = 0;
    lv_obj_invalidate(lv_screen_active());   /* the console drew over everything */
    lv_obj_invalidate(lv_layer_top());
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    dialog_close();
}

static void confirm_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    dialog_close();
    if (which == 0)
        plat_ui_reboot();
    else if (which == 1)
        plat_ui_boot_stock();
}

static void sys_cb(lv_event_t *e)   /* restart / stock firmware: ask first, a stray tap must not restart the unit */
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which == 2) {
        show_console();
        return;
    }
    dialog_close();
    lv_obj_t *mb = dialog = lv_msgbox_create(0);
    lv_obj_set_width(mb, 560);
    lv_msgbox_add_title(mb, which == 0 ? "Restart the head unit?" : "Start the original firmware?");
    lv_msgbox_add_text(mb, which == 0 ? "It comes back in about 15 seconds."
                                      : "It runs until the next ignition cycle, then this app starts again.");
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, which == 0 ? "Restart" : "Start it");
    lv_obj_add_event_cb(ok, confirm_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
    lv_obj_t *no = lv_msgbox_add_footer_button(mb, "Cancel");
    lv_obj_add_event_cb(no, cancel_cb, LV_EVENT_CLICKED, 0);
}

static lv_obj_t *row(lv_obj_t *page, const char *text)
{
    lv_obj_t *r = lv_obj_create(page);
    lv_obj_add_style(r, &st_row, 0);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 72, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);   /* long texts wrap instead of running under the control */
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_style_pad_right(l, 16, 0);
    return r;
}

static void wifi_ta_cb(lv_event_t *e)   /* the keyboard types into whichever field was tapped */
{
    lv_keyboard_set_textarea(wifi_kb, lv_event_get_target(e));
}

static void wifi_kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_CANCEL) {
        lv_screen_load(scr_settings);
    } else if (c == LV_EVENT_READY) {   /* the keyboard's check key: connect */
        const char *ss = lv_textarea_get_text(wifi_ssid), *pk = lv_textarea_get_text(wifi_psk);
        int sl = 0, pl = 0;
        while (ss[sl])
            sl++;
        while (pk[pl])
            pl++;
        if (!sl || sl > 32 || pl < 8 || pl > 63) {
            lv_label_set_text(wifi_msg, "Network name 1-32 characters, password 8-63.");
            return;
        }
        lv_label_set_text(wifi_msg, "Saving to WIFI.TXT and restarting...");
        lv_refr_now(0);
        plat_ui_wifi_set(ss, pk);
    }
}

static void wifi_open_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(wifi_psk, "");
    lv_label_set_text(wifi_msg, "Tap a field, type, then the check key to connect (restarts the unit).");
    lv_keyboard_set_textarea(wifi_kb, wifi_ssid);
    lv_screen_load(scr_wifi);
}

static void build_wifi(void)
{
    scr_wifi = screen(0);
    lv_obj_add_style(scr_wifi, &st_scr, 0);
    lv_obj_t *t = lv_label_create(scr_wifi);
    lv_label_set_text(t, "Wi-Fi network");
    lv_obj_add_style(t, &st_title, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 32, 16);
    wifi_ssid = lv_textarea_create(scr_wifi);
    lv_textarea_set_one_line(wifi_ssid, true);
    lv_textarea_set_max_length(wifi_ssid, 32);
    lv_textarea_set_placeholder_text(wifi_ssid, "Network name (SSID)");
    lv_obj_set_width(wifi_ssid, 460);
    lv_obj_align(wifi_ssid, LV_ALIGN_TOP_LEFT, 32, 66);
    lv_obj_add_event_cb(wifi_ssid, wifi_ta_cb, LV_EVENT_FOCUSED, 0);
    wifi_psk = lv_textarea_create(scr_wifi);
    lv_textarea_set_one_line(wifi_psk, true);
    lv_textarea_set_password_mode(wifi_psk, true);
    lv_textarea_set_max_length(wifi_psk, 63);
    lv_textarea_set_placeholder_text(wifi_psk, "Password");
    lv_obj_set_width(wifi_psk, 460);
    lv_obj_align(wifi_psk, LV_ALIGN_TOP_RIGHT, -32, 66);
    lv_obj_add_event_cb(wifi_psk, wifi_ta_cb, LV_EVENT_FOCUSED, 0);
    wifi_msg = lv_label_create(scr_wifi);
    lv_obj_add_style(wifi_msg, &st_section, 0);
    lv_obj_align(wifi_msg, LV_ALIGN_TOP_LEFT, 32, 124);
    wifi_kb = lv_keyboard_create(scr_wifi);
    lv_obj_set_size(wifi_kb, PANEL_W, 400);
    lv_obj_align(wifi_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(wifi_kb, wifi_kb_cb, LV_EVENT_ALL, 0);
}

/* Bluetooth name: 1..BT_NAME_MAX printable characters; the AC6965A takes it at once (phones may show the old name
 * until they look again) */
static void bt_kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_CANCEL) {
        lv_screen_load(scr_settings);
    } else if (c == LV_EVENT_READY) {
        const char *s = lv_textarea_get_text(bt_ta);
        int n = 0, ok = 1;
        for (; s[n]; n++)
            ok &= s[n] >= 0x20 && s[n] < 0x7f;
        while (n && s[n - 1] == ' ')
            n--;
        if (!n || n > BT_NAME_MAX || !ok || s[0] == ' ') {
            lv_label_set_text(bt_msg, "1-20 letters, digits or symbols.");
            return;
        }
        for (int i = 0; i < n; i++)
            bt_name[i] = s[i];
        bt_name[n] = 0;
        lv_label_set_text(bt_label, bt_name);
        plat_ui_bt_name(bt_name);
        settings_save();
        lv_screen_load(scr_settings);
    }
}

static void bt_open_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(bt_ta, bt_name);
    lv_label_set_text(bt_msg, "");
    lv_screen_load(scr_btname);
}

static void build_btname(void)
{
    scr_btname = screen(0);
    lv_obj_add_style(scr_btname, &st_scr, 0);
    lv_obj_t *t = lv_label_create(scr_btname);
    lv_label_set_text(t, "Bluetooth name");
    lv_obj_add_style(t, &st_title, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 32, 16);
    bt_ta = lv_textarea_create(scr_btname);
    lv_textarea_set_one_line(bt_ta, true);
    lv_textarea_set_max_length(bt_ta, BT_NAME_MAX);
    lv_obj_set_width(bt_ta, 460);
    lv_obj_align(bt_ta, LV_ALIGN_TOP_LEFT, 32, 66);
    bt_msg = lv_label_create(scr_btname);
    lv_obj_add_style(bt_msg, &st_section, 0);
    lv_obj_align(bt_msg, LV_ALIGN_TOP_LEFT, 32, 124);
    lv_obj_t *kb = lv_keyboard_create(scr_btname);
    lv_obj_set_size(kb, PANEL_W, 400);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, bt_ta);
    lv_obj_add_event_cb(kb, bt_kb_cb, LV_EVENT_ALL, 0);
}

/* ---- settings: categories down the left, one page each that fits without scrolling (scrolling a full-height list
 * is choppy on this CPU, and a car screen should not need it) ---- */

#define NPAGES 5
static lv_obj_t *pages[NPAGES], *navs[NPAGES];

static void show_page(int i)
{
    for (int k = 0; k < NPAGES; k++) {
        if (k == i) {
            lv_obj_remove_flag(pages[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(navs[k], LV_STATE_CHECKED);
        } else {
            lv_obj_add_flag(pages[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_state(navs[k], LV_STATE_CHECKED);
        }
    }
}

static void nav_cb(lv_event_t *e) { show_page((int)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *page(lv_obj_t *parent, int i, const char *name, lv_obj_t *nav)
{
    lv_obj_t *b = navs[i] = lv_button_create(nav);
    lv_obj_add_style(b, &st_nav, 0);
    lv_obj_add_style(b, &st_nav_sel, LV_STATE_CHECKED);
    lv_obj_set_size(b, LV_PCT(100), 64);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *p = pages[i] = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, PANEL_W - 300, PANEL_H - 40);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, 280, 20);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/* ---- equalizer page: presets, then one vertical slider per band ---- */

static lv_obj_t *eq_slider[EQ_N], *eq_val[EQ_N], *eq_presets;

static void eq_show(int b)
{
    lv_label_set_text_fmt(eq_val[b], "%+d", eq_db[b]);
    if (!eq_db[b])
        lv_label_set_text(eq_val[b], "0");
}

static void eq_preset_mark(void)   /* highlight the preset the bands match, if any */
{
    int which = -1;
    for (int pr = 0; pr < EQ_PRESETS && which < 0; pr++) {
        int same = 1;
        for (int b = 0; b < EQ_N; b++)
            same &= eq_db[b] == eq_preset[pr][b];
        if (same)
            which = pr;
    }
    lv_buttonmatrix_clear_button_ctrl_all(eq_presets, LV_BUTTONMATRIX_CTRL_CHECKED);
    if (which >= 0)
        lv_buttonmatrix_set_button_ctrl(eq_presets, (uint32_t)which, LV_BUTTONMATRIX_CTRL_CHECKED);
}

static void eq_slider_cb(lv_event_t *e)
{
    int b = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        eq_db[b] = (signed char)lv_slider_get_value(lv_event_get_target(e));
        eq_show(b);
        plat_ui_eq(b, eq_db[b]);
    } else {
        eq_preset_mark();
        settings_save();
    }
}

static void eq_preset_cb(lv_event_t *e)
{
    uint32_t pr = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (pr >= EQ_PRESETS)
        return;
    for (int b = 0; b < EQ_N; b++) {
        eq_db[b] = eq_preset[pr][b];
        lv_slider_set_value(eq_slider[b], eq_db[b], LV_ANIM_OFF);
        eq_show(b);
        plat_ui_eq(b, eq_db[b]);
    }
    eq_preset_mark();
    settings_save();
}

static void build_eq(lv_obj_t *p)
{
    lv_obj_t *r = row(p, "Equalizer");
    eq_presets = lv_buttonmatrix_create(r);
    lv_buttonmatrix_set_map(eq_presets, eq_preset_map);
    lv_buttonmatrix_set_button_ctrl_all(eq_presets, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(eq_presets, true);
    lv_obj_set_size(eq_presets, 470, 60);
    lv_obj_set_style_pad_all(eq_presets, 4, 0);
    lv_obj_add_event_cb(eq_presets, eq_preset_cb, LV_EVENT_VALUE_CHANGED, 0);
    lv_obj_t *bands = lv_obj_create(p);
    lv_obj_add_style(bands, &st_row, 0);
    lv_obj_remove_flag(bands, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bands, LV_PCT(100), 420);
    lv_obj_set_flex_flow(bands, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bands, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(bands, 10, 0);
    for (int b = 0; b < EQ_N; b++) {
        lv_obj_t *col = lv_obj_create(bands);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 60, LV_PCT(100));
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        eq_val[b] = lv_label_create(col);
        lv_obj_add_style(eq_val[b], &st_section, 0);
        lv_obj_t *sl = eq_slider[b] = lv_slider_create(col);
        lv_obj_set_size(sl, 14, 300);   /* taller than wide: vertical */
        lv_slider_set_range(sl, -12, 12);
        lv_slider_set_mode(sl, LV_SLIDER_MODE_SYMMETRICAL);   /* the bar grows from 0 dB, up or down */
        lv_slider_set_value(sl, eq_db[b], LV_ANIM_OFF);
        lv_obj_set_ext_click_area(sl, 20);   /* easier to grab with a finger in a car */
        lv_obj_add_event_cb(sl, eq_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)b);
        lv_obj_add_event_cb(sl, eq_slider_cb, LV_EVENT_RELEASED, (void *)(intptr_t)b);
        lv_obj_t *l = lv_label_create(col);
        lv_label_set_text(l, eq_label[b]);
        lv_obj_add_style(l, &st_section, 0);
        eq_show(b);
    }
    eq_preset_mark();
}

static void build_settings(void)
{
    scr_settings = screen(0);
    lv_obj_add_style(scr_settings, &st_scr, 0);
    lv_obj_t *nav = lv_obj_create(scr_settings);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, 240, PANEL_H - 40);
    lv_obj_align(nav, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nav, 6, 0);
    lv_obj_remove_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(nav);
    lv_label_set_text(title, "Settings");
    lv_obj_add_style(title, &st_title, 0);
    lv_obj_set_style_pad_bottom(title, 14, 0);

    lv_obj_t *p = page(scr_settings, 0, "Network", nav);
    lv_obj_t *r = row(p, "Wi-Fi");
    status_label = lv_label_create(r);
    lv_label_set_text(status_label, "");
    lv_obj_set_style_pad_right(status_label, 16, 0);
    lv_obj_t *wb = lv_button_create(r);
    lv_label_set_text(lv_label_create(wb), "Change");
    lv_obj_add_event_cb(wb, wifi_open_cb, LV_EVENT_CLICKED, 0);

    p = page(scr_settings, 1, "Android Auto", nav);
    r = row(p, "Enabled");
    lv_obj_t *sw = lv_switch_create(r);
    if (aa_enabled)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, aa_switch_cb, LV_EVENT_VALUE_CHANGED, 0);
    r = row(p, "Bluetooth name");
    bt_label = lv_label_create(r);
    lv_label_set_text(bt_label, bt_name);
    lv_obj_set_style_pad_right(bt_label, 16, 0);
    lv_obj_t *bb = lv_button_create(r);
    lv_label_set_text(lv_label_create(bb), "Change");
    lv_obj_add_event_cb(bb, bt_open_cb, LV_EVENT_CLICKED, 0);
    r = row(p, "Interface size");
    lv_obj_t *dl = lv_label_create(r);
    lv_obj_t *ds = lv_slider_create(r);
    lv_obj_move_to_index(dl, -1);   /* slider, then its value */
    lv_obj_set_width(ds, 280);
    lv_obj_set_style_margin_right(ds, 16, 0);
    lv_obj_set_ext_click_area(ds, 20);
    lv_slider_set_range(ds, AA_DPI_MIN / 10, AA_DPI_MAX / 10);
    lv_slider_set_value(ds, aa_dpi / 10, LV_ANIM_OFF);
    lv_label_set_text_fmt(dl, "%d dpi", aa_dpi);
    lv_obj_set_width(dl, 90);
    lv_obj_add_event_cb(ds, aa_dpi_cb, LV_EVENT_VALUE_CHANGED, dl);
    lv_obj_add_event_cb(ds, aa_dpi_cb, LV_EVENT_RELEASED, dl);

    p = page(scr_settings, 2, "Display", nav);
    static const char *const bright_names[] = { "Brightness", "With headlights on" };
    for (int i = 0; i < 2; i++) {
        r = row(p, bright_names[i]);
        lv_obj_t *sl = lv_slider_create(r);
        lv_obj_set_width(sl, 320);
        lv_obj_set_style_margin_right(sl, 12, 0);
        lv_slider_set_range(sl, 0, 100);
        lv_slider_set_value(sl, i ? bright_night : bright_day, LV_ANIM_OFF);
        lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
    }
    r = row(p, "Theme");
    static const char *const theme_map[] = { "Light", "Dark", "Auto", "" };
    lv_obj_t *bm = lv_buttonmatrix_create(r);
    lv_buttonmatrix_set_map(bm, theme_map);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_buttonmatrix_set_button_ctrl(bm, (uint32_t)theme_mode, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, 330, 64);
    lv_obj_set_style_pad_all(bm, 4, 0);
    lv_obj_add_event_cb(bm, theme_cb, LV_EVENT_VALUE_CHANGED, 0);

    p = page(scr_settings, 3, "Audio", nav);
    build_eq(p);

    p = page(scr_settings, 4, "System", nav);
    r = row(p, "Restart the head unit");
    lv_obj_t *b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Reboot");
    lv_obj_add_event_cb(b, sys_cb, LV_EVENT_CLICKED, (void *)0);
    r = row(p, "Log console");
    b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Open");
    lv_obj_add_event_cb(b, sys_cb, LV_EVENT_CLICKED, (void *)2);
    r = row(p, "Start the original firmware once");
    b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Boot stock");
    lv_obj_add_event_cb(b, sys_cb, LV_EVENT_CLICKED, (void *)1);
    static char inst[120];
    plat_ui_install_status(inst, sizeof inst);
    r = row(p, inst);
    b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Install to flash");
    lv_obj_add_state(b, LV_STATE_DISABLED);   /* stub: writing the app into NOR permanently is not built yet */
    show_page(0);
}

static void status_timer(lv_timer_t *t)
{
    (void)t;
    static char buf[200];
    if (lv_screen_active() == scr_settings) {
        plat_ui_status(buf, sizeof buf);
        lv_label_set_text(status_label, buf);
    }
}

/* ---- volume overlay on the top layer: over every screen, including the video ---- */

static void vol_hide(lv_timer_t *t)
{
    lv_obj_add_flag(vol_panel, LV_OBJ_FLAG_HIDDEN);
    settings_save();   /* the volume, once the presses are over (not one SD write per step) */
    lv_timer_pause(t);   /* kept for the next press (a repeat count would make LVGL delete it) */
}

static void build_volume(void)
{
    vol_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(vol_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(vol_panel, 460, 76);
    lv_obj_align(vol_panel, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_radius(vol_panel, 38, 0);
    lv_obj_set_style_border_width(vol_panel, 0, 0);
    lv_obj_set_style_bg_color(vol_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vol_panel, LV_OPA_60, 0);
    vol_icon = lv_label_create(vol_panel);
    lv_obj_set_style_text_color(vol_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(vol_icon, &lv_font_montserrat_28, 0);
    lv_obj_align(vol_icon, LV_ALIGN_LEFT_MID, 8, 0);
    vol_bar = lv_bar_create(vol_panel);
    lv_bar_set_range(vol_bar, 0, VOL_MAX);
    lv_obj_set_size(vol_bar, 290, 12);
    lv_obj_align(vol_bar, LV_ALIGN_CENTER, 6, 0);
    lv_obj_set_style_bg_color(vol_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vol_bar, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vol_bar, lv_color_white(), LV_PART_INDICATOR);
    vol_num = lv_label_create(vol_panel);
    lv_obj_set_style_text_color(vol_num, lv_color_white(), 0);
    lv_obj_align(vol_num, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_flag(vol_panel, LV_OBJ_FLAG_HIDDEN);
    vol_timer = lv_timer_create(vol_hide, 1500, 0);
    lv_timer_set_auto_delete(vol_timer, false);
    lv_timer_pause(vol_timer);
}

static void vol_show(void)
{
    plat_ui_volume(volume);
    lv_label_set_text(vol_icon, muted || !volume ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    lv_bar_set_value(vol_bar, muted ? 0 : volume, LV_ANIM_OFF);
    lv_label_set_text_fmt(vol_num, "%d", muted ? 0 : volume);
    lv_obj_remove_flag(vol_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(vol_timer);
    lv_timer_resume(vol_timer);
}

/* ---- navigation ---- */

static void show_home(void)
{
    dialog_close();
    if (aa_view) {
        aa_view = 0;
        plat_ui_video_hide();
    }
    lv_screen_load(scr_home);
}

static void show_settings(void)
{
    dialog_close();
    show_page(0);   /* always opens on the first page */
    lv_screen_load(scr_settings);
}

static void show_aa(void)
{
    if (!aa_enabled)
        return;
    aa_view = 1;
    if (aa_session)
        lv_obj_add_flag(aa_card, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(aa_card, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(scr_aa);
}

static unsigned int btn1_at;
static int btn1_fired, vol_dir;
static unsigned int vol_at;

static void on_button(int b, int pressed)
{
    unsigned int now = tick_cb();
    if (console_on && pressed && (b == 2 || b == 3)) {   /* Home / Back leave the text console */
        leave_console();
        if (b == 2)
            show_home();
        return;
    }
    if (b == 1) {
        if (pressed) {
            btn1_at = now, btn1_fired = 0;
        } else if (!btn1_fired) {   /* short press: mute toggle */
            muted = !muted;
            plat_ui_mute(muted);
            vol_show();
        }
        if (!pressed)
            btn1_at = 0;
    } else if (b == 2 && pressed) {
        show_home();
    } else if (b == 3 && pressed) {
        if (ui_aa_visible())
            plat_ui_aa_key(4);
        else if (lv_screen_active() == scr_wifi || lv_screen_active() == scr_btname)
            lv_screen_load(scr_settings);   /* keeps its scroll position: coming back from a sub-screen */
        else if (lv_screen_active() != scr_home)
            show_home();
    } else if (b == 4 || b == 5) {
        vol_dir = pressed ? (b == 4 ? 1 : -1) : 0;
        if (pressed) {
            volume += vol_dir;
            volume = volume < 0 ? 0 : volume > VOL_MAX ? VOL_MAX : volume;
            if (muted) {   /* a volume press ends a mute */
                muted = 0;
                plat_ui_mute(0);
            }
            vol_show();
            vol_at = now + 450;   /* held: repeat after a pause */
        }
    }
}

static void poll_held(void)
{
    unsigned int now = tick_cb();
    if (btn1_at && !btn1_fired && now - btn1_at >= 1500) {   /* long press: reboot */
        btn1_fired = 1;
        lv_obj_t *m = lv_label_create(lv_layer_top());
        lv_label_set_text(m, "Rebooting...");
        lv_obj_set_style_text_font(m, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(m, lv_color_white(), 0);
        lv_obj_center(m);
        lv_refr_now(0);
        plat_ui_reboot();
    }
    if (vol_dir && (int)(now - vol_at) >= 0) {
        volume += vol_dir;
        volume = volume < 0 ? 0 : volume > VOL_MAX ? VOL_MAX : volume;
        vol_show();
        vol_at = now + 120;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    lv_init();
    lv_log_register_print_cb(log_cb);
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(d, (void *)DRAW_BUF, 0, PANEL_W * DRAW_ROWS * 4, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, pointer_cb);
    settings_load();
    lights = plat_ui_lights() ? 1 : 0;   /* the state at power-on counts as is */
    dark = theme_mode == 2 ? lights : theme_mode;
    apply_backlight();
    plat_ui_night(lights);
    plat_ui_volume(volume);
    for (int b = 0; b < EQ_N; b++)
        plat_ui_eq(b, eq_db[b]);
    apply_theme();
    build_home();
    build_aa();
    build_settings();
    build_wifi();
    build_btname();
    build_volume();
    lv_timer_create(status_timer, 500, 0);
    lv_timer_create(lights_timer, 200, 0);
    lv_screen_load(scr_home);
    if (boot_console)
        show_console();
    running = 1;
    int seen = 0;
    for (;;) {
        unsigned char e;
        while (xQueueReceive(btn_q, &e, 0) == pdTRUE)
            on_button(e >> 1, e & 1);
        poll_held();
        if (console_on) {   /* the text console owns the panel: no LVGL drawing; a tap (on release) comes back */
            static int was_down;
            if (ptr_down)
                was_down = 1;
            else if (was_down)
                was_down = 0, leave_console();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (seen != aa_session_seen) {   /* a phone came or went */
            seen = aa_session_seen;
            if (aa_session && aa_enabled)
                show_aa();
            else if (!aa_session && aa_view)
                show_home();
        }
        uint32_t wait = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(wait < 10 ? 10 : wait > 30 ? 30 : wait));
    }
}

void ui_start(unsigned int fb_addr, const char *settings)
{
    fb = fb_addr;
    int i = 0;
    for (; settings && settings[i] && i < (int)sizeof settings_text - 1; i++)
        settings_text[i] = settings[i];
    settings_text[i] = 0;
    btn_q = xQueueCreate(16, 1);
    xTaskCreate(ui_task, "ui", 8192, 0, 2, 0);
}
