/* Goodix GT9xx touch over bit-banged I2C on GPIO90..93 (gt9.c). */
#ifndef GT9_H
#define GT9_H
int gt9_op(const unsigned char *p, int n, char *out);   /* HU1 op 14: text reply */
void gt9_release(void);                                  /* give SDA/SCL back to their U-Boot pad owner */
void gt9_tick(unsigned int now_ms);                      /* main loop: poll every 25 ms, report touches and buttons */
/* display touches (not the button strip) as they happen, from the touch task: action 0 down, 1 up, 2 move; panel
 * pixels (1024 x 600). The callee must only queue (it runs on the touch task). */
extern void (*gt9_hook)(int action, unsigned x, unsigned y);
/* front-panel buttons 1..5 (top to bottom), pressed 1 / released 0, from the touch task: the callee must only queue */
extern void (*gt9_button_hook)(int button, int pressed);
extern char gt9_last[48];
extern volatile unsigned int gt9_down_ms;               /* ub_ms of the last touch down, 0 = none yet */                                /* last event, for the console */
#endif
