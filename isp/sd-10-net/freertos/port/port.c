/* FreeRTOS port for the AndesCore N13 under our payload: the C half. The assembly half (frame save/restore, the
 * switch on the way out of a trap, port_start_first, port_call_on_stack) is in irq.S; the tick and the yield trap
 * are dispatched from irq.c's hu_trap. See portmacro.h and notes/rtos.md. */
#include "FreeRTOS.h"
#include "task.h"
#include "irq.h"
#include "port_nds32.h"
#include "clk.h"

typedef uint32_t u32;

volatile u32 port_switch_pending, port_running, port_boot_sp;
static u32 task_psw, task_gp, task_r10;
static void *jb[5];

extern void *volatile pxCurrentTCB;          /* tasks.c; its first member is pxTopOfStack */
void port_start_first(u32 *frame);           /* irq.S: $sp = frame, then the restore path and iret */
void plat_logf(const char *fmt, ...);

#define FRAME_WORDS 34   /* irq.S: IPC, IPSW, IFC_LP, r2..r27, fp, gp, lp, r0, r1 */

static void task_exit_error(void)
{
    port_assert("task returned", 0);
}

/* Called once from the boot context (U-Boot's stack, before the scheduler). New tasks inherit $gp and $r10
 * (U-Boot's gd) from here, and a PSW with INTL = 0, IFCON = 0 and GIE = 1 for their first iret. */
void port_init(void)
{
    u32 psw;
    __asm__ volatile("mfsr %0, $psw" : "=r"(psw));
    __asm__ volatile("mov55 %0, $gp" : "=r"(task_gp));
    __asm__ volatile("mov55 %0, $r10" : "=r"(task_r10));
    task_psw = (psw & ~(6u | (1u << 14))) | 1u;
}

void port_udelay(u32 us)
{
    clk_udelay(us);
}

StackType_t *pxPortInitialiseStack(StackType_t *top, TaskFunction_t code, void *param)
{
    StackType_t *f = top - FRAME_WORDS;   /* top is 8-aligned (tasks.c) and the frame is 136 bytes */
    for (int i = 0; i < FRAME_WORDS; i++)
        f[i] = 0;
    f[0] = (u32)code;            /* IPC */
    f[1] = task_psw;             /* IPSW */
    f[3 + 10 - 2] = task_r10;    /* r10 */
    f[30] = task_gp;
    f[31] = (u32)task_exit_error;
    f[32] = (u32)param;          /* r0 */
    return f;
}

BaseType_t xPortStartScheduler(void)
{
    if (__builtin_setjmp(jb))
        return pdFALSE;   /* vPortEndScheduler */
    u32 sp;
    __asm__ volatile("mov55 %0, $sp" : "=r"(sp));
    port_boot_sp = (sp - 512) & ~7u;
    portDISABLE_INTERRUPTS();
    if (irq_enable(configTICK_RATE_HZ))
        return pdFALSE;
    portDISABLE_INTERRUPTS();   /* irq_enable turned GIE on; the first task's IPSW turns it back on */
    port_switch_pending = 0;
    port_running = 1;
    port_start_first(*(u32 **)pxCurrentTCB);
    return pdFALSE;   /* not reached */
}

void vPortEndScheduler(void)
{
    portDISABLE_INTERRUPTS();
    port_running = 0;
    irq_disable();
    __builtin_longjmp(jb, 1);
}

/* from the tick ISR (irq.c), GIE off */
void port_tick(void)
{
    if (port_running && xTaskIncrementTick() != pdFALSE)
        port_switch_pending = 1;
}

/* from hu_trap for `trap portYIELD_SWID` */
void port_yield_trap(void)
{
    if (port_running)
        port_switch_pending = 1;
}

void port_assert(const char *what, int line)
{
    portDISABLE_INTERRUPTS();
    plat_logf("rtos: ASSERT %s line %d\n", what, line);
    irq_crash_now();
    for (;;)
        ;
}

void vApplicationStackOverflowHook(TaskHandle_t t, char *name)
{
    plat_logf("rtos: STACK OVERFLOW in task %s\n", name);
    irq_crash_now();
}

void vApplicationMallocFailedHook(void)
{
    port_assert("malloc failed", 0);
}

/* heap_4's memory (configAPPLICATION_ALLOCATED_HEAP): 2 MB at 0x02200000, see payload.ld */
__attribute__((section(".rtos_heap"))) uint8_t ucHeap[configTOTAL_HEAP_SIZE];
