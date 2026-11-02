/* The head unit's user interface (ui.c): LVGL on the OSD0 layer, Android Auto video on VPP0 underneath.
 * Design: docs/superpowers/specs/2026-11-02-head-unit-ui-design.md. Only the UI task touches LVGL; everything below is
 * safe to call from other tasks (it queues or sets flags). */
#ifndef UI_H
#define UI_H

void ui_start(unsigned int fb_addr);            /* create the UI task (after the scheduler's other tasks) */
int ui_running(void);                           /* the UI owns the OSD0 framebuffer (the text console stays off) */

/* input, from the touch task */
void ui_pointer(int action, unsigned x, unsigned y);   /* 0 down, 1 up, 2 move; panel pixels */
void ui_button(int button, int pressed);               /* front buttons 1..5, top to bottom */

/* Android Auto, from the net task */
void ui_aa_session(int active);                 /* a phone's video started (1) or the session ended (0) */
int ui_aa_visible(void);                        /* the AA view is up: show its video, send it the touches */

/* hooks the UI calls (net.c); run on the UI task, so they must only queue work for the net task */
void plat_ui_mute(int on);
void plat_ui_reboot(void);
void plat_ui_boot_stock(void);
void plat_ui_aa_key(int keycode);               /* 4 BACK */
void plat_ui_aa_enable(int on);
void plat_ui_brightness(int percent);
void plat_ui_video_hide(void);                  /* the AA view went away: VPP0 off */
int plat_ui_status(char *out, int max);         /* one line of network / build status for Settings */
int plat_ui_console(char *out, int max);        /* the log tail for the console screen */

#endif
