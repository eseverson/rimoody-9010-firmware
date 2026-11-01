/* Interrupts, timer tick and exception handling (irq.c, irq.S). */
#ifndef IRQ_H
#define IRQ_H

void irq_init(void);          /* vectors at 0x01F10000, IVB, INTL = 0; interrupts stay off */
int irq_enable(unsigned int hz);   /* timer IRQ 131 at hz (3..1000), GIE on; <0 on bad args */
void irq_disable(void);
void irq_shutdown(void);      /* off + IVB/PSW.INTL as found (before EXEC / returning to U-Boot) */
void irq_check(void);         /* re-assert IVB / GIE if something changed them (counted) */
int irq_is_on(void);
unsigned int irq_save(void);  /* GIE off, returns the old GIE */
void irq_restore(unsigned int flags);
void irq_set_heartbeat(const unsigned char *frame, int len);
void irq_uart_finish(void);   /* with GIE off: complete a heartbeat the ISR is part-way through */   /* sent from the tick every 3 s while on */
int irq_rx_get(unsigned char *b);   /* MCU UART bytes the ISR drained */
int irq_test_trap(void);      /* resumable trap through the vector table; returns 1 if the handler ran */
void irq_crash_now(void);     /* fatal trap: crash record + reset */
int irq_status(char *out);
int irq_crash_report(char *out, char *brief);   /* earlier crash record as text (cleared); 0 if none */
void irq_trap_test(void);
void irq_trap_fatal(void);
extern volatile unsigned int irq_ticks, irq_hb_sent, irq_count[7], irq_uptime_ms;
extern char irq_note[48];   /* e.g. the U-Boot command being run; goes into a crash record */

void irq_watchdog(unsigned int seconds);   /* hang watchdog while the tick is on; 0 = off */
extern volatile unsigned int irq_wd_left, irq_wd_reload;
#define irq_pet() (irq_wd_left = irq_wd_reload)
void irq_hw_watchdog(unsigned int seconds);   /* hardware WDT (0x9C000630), fed by the tick while the hang watchdog is fine; 0 = stop */
unsigned int irq_hw_wdt_left_ms(void);
extern volatile unsigned int irq_hw_wdt_on;
void irq_hw_wdt_test(void);         /* crash record "hwwdt-test", stop feeding: the unit should reset */
int irq_hw_wdt_test_failed(void);   /* still alive: undo the test, returns 1 */
void irq_fail_reset(unsigned int vec);
extern volatile unsigned int *irq_log_len_ptr;   /* set by the payload: its log length goes into crash records */
unsigned int irq_prev_log_len(void);   /* previous run's log length from a valid crash record, else 0 */   /* crash record (102 probation, 103.. failure) and reset */

int plat_fmt(char *out, const char *fmt, ...);

#endif
