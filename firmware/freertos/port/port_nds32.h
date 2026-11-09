/* Payload-side API of the NDS32 FreeRTOS port (port.c, irq.S). */
#ifndef PORT_NDS32_H
#define PORT_NDS32_H
#include <stdint.h>
extern volatile uint32_t port_switch_pending, port_running, port_boot_sp;
void port_init(void);                   /* boot context, before any xTaskCreate */
void port_udelay(uint32_t us);          /* busy wait usable from any task (clk.c) */
void port_tick(void);                   /* tick ISR */
void port_yield_trap(void);             /* hu_trap, yield SWID */
void port_assert(const char *what, int line);
/* call fn(a0, a1) on another stack and return its result (EXEC runs the trampoline on the boot stack) */
int port_call_on_stack(int (*fn)(int, char **), int a0, char **a1, uint32_t sp);
#endif
