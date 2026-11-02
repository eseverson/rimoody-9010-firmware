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
static int volume = 20, muted, dark = 1;

static lv_obj_t *scr_home, *scr_aa, *scr_settings, *scr_console;
static lv_obj_t *aa_card, *console_label, *status_label, *vol_panel, *vol_bar, *vol_icon, *vol_num;
static lv_timer_t *vol_timer;

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

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
    for (int y = a->y1; y <= a->y2; y++)
        dcache_wb_range(fb + ((unsigned int)y * PANEL_W + (unsigned int)a->x1) * 4, (unsigned int)(a->x2 - a->x1 + 1) * 4);
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
    else if (which == 1)
        lv_screen_load(scr_settings);
    else
        lv_screen_load(scr_console);
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
    static const char *const icons[] = { LV_SYMBOL_GPS, LV_SYMBOL_SETTINGS, LV_SYMBOL_LIST };
    for (int i = 0; i < 3; i++) {
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
}

static void bright_cb(lv_event_t *e) { plat_ui_brightness(lv_slider_get_value(lv_event_get_target(e))); }

static void apply_theme(void)
{
    lv_display_t *d = lv_display_get_default();
    lv_theme_t *t = lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_GREY), dark,
                                          &lv_font_montserrat_20);
    lv_display_set_theme(d, t);
    /* the bottom layer is created like a screen and picks up the theme's opaque screen colour; it must stay
     * transparent so a transparent screen (the AA view) shows the video on VPP0 */
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_TRANSP, 0);
}

static void dark_cb(lv_event_t *e)
{
    dark = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    apply_theme();
}

static void sys_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which == 0)
        plat_ui_reboot();
    else if (which == 1)
        plat_ui_boot_stock();
}

static lv_obj_t *row(lv_obj_t *list, const char *text)
{
    lv_obj_t *r = lv_obj_create(list);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, text);
    return r;
}

static void section(lv_obj_t *list, const char *text)
{
    lv_obj_t *l = lv_label_create(list);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(l, LV_OPA_60, 0);
    lv_obj_set_style_pad_top(l, 12, 0);
}

static void build_settings(void)
{
    scr_settings = screen(0);
    lv_obj_t *title = lv_label_create(scr_settings);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 32, 20);
    lv_obj_t *list = lv_obj_create(scr_settings);
    lv_obj_set_size(list, PANEL_W - 64, PANEL_H - 90);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);

    section(list, "NETWORK");
    lv_obj_t *r = row(list, "Wi-Fi");
    status_label = lv_label_create(r);
    lv_label_set_text(status_label, "");
    lv_obj_t *n = lv_label_create(list);
    lv_label_set_text(n, "To change network: edit WIFI.TXT on the SD card (on-screen entry comes next).");
    lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(n, LV_OPA_60, 0);

    section(list, "ANDROID AUTO");
    r = row(list, "Enabled");
    lv_obj_t *sw = lv_switch_create(r);
    if (aa_enabled)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, aa_switch_cb, LV_EVENT_VALUE_CHANGED, 0);

    section(list, "DISPLAY");
    r = row(list, "Brightness");
    lv_obj_t *sl = lv_slider_create(r);
    lv_obj_set_width(sl, 360);
    lv_slider_set_range(sl, 10, 100);
    lv_slider_set_value(sl, 80, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_RELEASED, 0);
    r = row(list, "Dark mode");
    sw = lv_switch_create(r);
    if (dark)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, dark_cb, LV_EVENT_VALUE_CHANGED, 0);

    section(list, "AUDIO");
    row(list, "Equalizer: after audio output is built");

    section(list, "SYSTEM");
    r = row(list, "Restart the head unit");
    lv_obj_t *b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Reboot");
    lv_obj_add_event_cb(b, sys_cb, LV_EVENT_CLICKED, (void *)0);
    r = row(list, "Start the original firmware once");
    b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Boot stock");
    lv_obj_add_event_cb(b, sys_cb, LV_EVENT_CLICKED, (void *)1);
    r = row(list, "Install: starts from the SD card (boot script in NOR)");
    b = lv_button_create(r);
    lv_label_set_text(lv_label_create(b), "Install to flash");
    lv_obj_add_state(b, LV_STATE_DISABLED);   /* stub: writing the app into NOR permanently is not built yet */
}

static void console_timer(lv_timer_t *t)
{
    (void)t;
    static char buf[2400];
    if (lv_screen_active() == scr_console) {
        plat_ui_console(buf, sizeof buf);
        lv_label_set_text(console_label, buf);
    }
    if (lv_screen_active() == scr_settings) {
        plat_ui_status(buf, 200);
        lv_label_set_text(status_label, buf);
    }
}

static void build_console(void)
{
    scr_console = screen(1);
    console_label = lv_label_create(scr_console);
    lv_obj_set_width(console_label, PANEL_W - 32);
    lv_obj_set_style_text_font(console_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(console_label, lv_color_hex(0xc0c0c0), 0);
    lv_obj_align(console_label, LV_ALIGN_BOTTOM_LEFT, 16, -8);
    lv_label_set_text(console_label, "");
}

/* ---- volume overlay on the top layer: over every screen, including the video ---- */

static void vol_hide(lv_timer_t *t)
{
    lv_obj_add_flag(vol_panel, LV_OBJ_FLAG_HIDDEN);
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
    if (aa_view) {
        aa_view = 0;
        plat_ui_video_hide();
    }
    lv_screen_load(scr_home);
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
        else if (lv_screen_active() != scr_home)
            show_home();
    } else if (b == 4 || b == 5) {
        vol_dir = pressed ? (b == 4 ? 1 : -1) : 0;
        if (pressed) {
            volume += vol_dir;
            volume = volume < 0 ? 0 : volume > VOL_MAX ? VOL_MAX : volume;
            muted = 0;
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
    lv_display_set_buffers(d, (void *)fb, 0, PANEL_W * PANEL_H * 4, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush_cb);
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, pointer_cb);
    apply_theme();
    build_home();
    build_aa();
    build_settings();
    build_console();
    build_volume();
    lv_timer_create(console_timer, 500, 0);
    lv_screen_load(scr_home);
    running = 1;
    int seen = 0;
    for (;;) {
        unsigned char e;
        while (xQueueReceive(btn_q, &e, 0) == pdTRUE)
            on_button(e >> 1, e & 1);
        poll_held();
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

void ui_start(unsigned int fb_addr)
{
    fb = fb_addr;
    btn_q = xQueueCreate(16, 1);
    xTaskCreate(ui_task, "ui", 8192, 0, 2, 0);
}
