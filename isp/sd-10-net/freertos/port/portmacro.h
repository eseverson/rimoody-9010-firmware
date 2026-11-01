/* FreeRTOS port for the AndesCore N13 (NDS32 V3, IVIC, no FPU, single core) under our payload. See notes/rtos.md.
 *
 * Context = the trap frame irq.S pushes on the task's own stack (34 words: IPC, IPSW, IFC_LP, r2..r27, fp, gp, lp,
 * r0, r1); a task's pxTopOfStack points at it. Switching happens on the way out of any trap/interrupt when
 * port_switch_pending is set (irq.S). portYIELD is a synchronous `trap`, so a yield inside a critical section
 * switches at once; each task's GIE comes back with its IPSW and the nesting count lives in its TCB. */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>

#define portCHAR        char
#define portFLOAT       float
#define portDOUBLE      double
#define portLONG        long
#define portSHORT       short
#define portSTACK_TYPE  uint32_t
#define portBASE_TYPE   long

typedef portSTACK_TYPE StackType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_TYPE_IS_ATOMIC 1

#define portSTACK_GROWTH     (-1)
#define portTICK_PERIOD_MS   ((TickType_t)1000 / configTICK_RATE_HZ)
#define portBYTE_ALIGNMENT   8
#define portNOP()            __asm__ volatile("nop")
#define portMEMORY_BARRIER() __asm__ volatile("" ::: "memory")

/* SWID of the yield trap; irq.c's hu_trap recognizes it, steps past it and requests a switch. */
#define portYIELD_SWID 0x7e58
#define portYIELD()            __asm__ volatile("trap 0x7e58" ::: "memory")
#define portYIELD_WITHIN_API() portYIELD()

extern volatile uint32_t port_switch_pending;
#define portEND_SWITCHING_ISR(x) do { if (x) port_switch_pending = 1; } while (0)
#define portYIELD_FROM_ISR(x)    portEND_SWITCHING_ISR(x)

/* Interrupts never nest here (GIE is off for the whole trap), so the FromISR mask macros stay at their defaults. */
#define portDISABLE_INTERRUPTS() __asm__ volatile("setgie.d\n\tdsb" ::: "memory")
#define portENABLE_INTERRUPTS()  __asm__ volatile("setgie.e\n\tdsb" ::: "memory")

#define portCRITICAL_NESTING_IN_TCB 1
extern void vTaskEnterCritical(void);
extern void vTaskExitCritical(void);
#define portENTER_CRITICAL() vTaskEnterCritical()
#define portEXIT_CRITICAL()  vTaskExitCritical()

#define portTASK_FUNCTION_PROTO(f, p) void f(void *p)
#define portTASK_FUNCTION(f, p)       void f(void *p)

#endif
