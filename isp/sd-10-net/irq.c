/* Interrupts, a timer tick and exception handling for the payload, on top of U-Boot (which never enables GIE, never
 * writes IVB or INT_MASK and leaves the interrupt controller masked; see notes/interrupts.md).
 *
 * CPU: AndesCore N13, IVIC internal vectored mode with 6 inputs (IVB = 0x801 under U-Boot), vectors from irq.S at
 * 0x01F10000. Interrupt controller (SP7021 layout, Linux irq-sp7021-intc.c): 0x9C000780 TYPE (1 = edge), +0x1C
 * POLARITY (1 = low/falling), +0x38 PRIORITY (1 = EXT_INT0), +0x54 MASK (1 = enabled), 7 words each; 0x9C000A80
 * CLEAR (write the bit), +0x1C MASKED_EXT1, +0x38 MASKED_EXT0, +0x7C GROUP. Tick: eCos's system clock timer at
 * 0x9C000640 (ctrl, prescale-1, reload-1, 16-bit down counter), IRQ 131, edge/rising, input clock 27 MHz /
 * (2 * (STC_CFG[7:0] + 1)) = 13.5 MHz (ecos 0x607f5a, 0x608176, 0x64b428).
 *
 * Unexpected exceptions write a crash record at 0x01F00000 and reset like U-Boot's reset_cpu(0) (0xf0aa26). */
#include "irq.h"
#include "crypto.h"
#include "freertos/port/port_nds32.h"

typedef unsigned int u32;
typedef unsigned char u8;

#define MFSR(reg) ({ u32 _v; __asm__ volatile("mfsr %0, $" #reg : "=r"(_v)); _v; })
#define MTSR(v, reg) __asm__ volatile("mtsr %0, $" #reg "\n\tdsb" ::"r"(v) : "memory")
#define GIE_OFF() __asm__ volatile("setgie.d\n\tdsb" ::: "memory")
#define GIE_ON() __asm__ volatile("setgie.e\n\tdsb" ::: "memory")
#define REG(a) (*(volatile u32 *)(a))
#define MSYNC() __asm__ volatile("msync all" ::: "memory")

#define VEC_BASE   0x01F10000u
#define CRASH_ADDR 0x01F00000u
#define CRASH_MAGIC 0x32435548u   /* "HUC2": version 2 (with a note); builds before 20261031-17 used "HUCR" */

#define INTC_TYPE    0x9c000780u
#define INTC_POL     0x9c00079cu
#define INTC_PRI     0x9c0007b8u
#define INTC_MASK    0x9c0007d4u
#define INTC_CLEAR   0x9c000a80u
#define INTC_EXT1    0x9c000a9cu
#define INTC_EXT0    0x9c000ab8u
#define STC_CFG      0x9c00061cu
#define TMR_CTRL     0x9c000640u
#define TMR_PRESCALE 0x9c000644u
#define TMR_RELOAD   0x9c000648u
#define TMR_COUNT    0x9c00064cu
#define TICK_IRQ     131
#define TGRP         (4 * (TICK_IRQ / 32))
#define TBIT         (1u << (TICK_IRQ % 32))
#define UART1_DATA   0x9c000980u
#define UART1_LSR    0x9c000984u

extern const u32 hu_vectors[], hu_vectors_end[], __vec_load[];
extern const char hu_build_id[];
extern u32 __bss_end[];

struct crash {
    u32 magic, version, vec, itype, ipc, ipsw, eva, p_ipc, p_ipsw, p_itype, p_eva, sp, psw, ticks, int_mask;
    u32 regs[31];   /* r0-r27, fp, gp, lp */
    char build[32];
    char note[48];   /* irq_note at the time: the U-Boot command being run, if any */
    u32 uptime_ms;
    u32 stack[16];   /* words at the interrupted $sp */
    u32 log_len;     /* v3: length of the run's log at LOG 0x01a00000 (written back before the reset) */
    u32 crc;
};
volatile u32 *irq_log_len_ptr;   /* the payload's log length, for the crash record */
#define CRASH ((struct crash *)CRASH_ADDR)

volatile u32 irq_ticks, irq_hb_sent, irq_count[7], irq_spurious, irq_rx_drop, irq_test_hits, irq_ivb_fix, irq_gie_fix;
volatile u32 irq_guard;
char irq_note[48];   /* 1: tick would not clear, 2: interrupts with nothing pending; the ISR masked HW0/HW1 */
static u32 spur_streak, stuck_streak;
volatile u32 irq_uptime_ms;   /* set by the main loop (ub_ms), copied into a crash record */
static volatile u32 rx_w, rx_r;
static volatile u8 rxring[256];
static u32 orig_ivb, orig_psw, orig_int_mask, our_ivb, hz_now, hb_every, hb_cnt, last_itype, installed;
static volatile int on;
static const u8 *hb_frame;
static int hb_len;
static volatile int hb_pos = -1;   /* next heartbeat byte to send; -1 = none pending */

static void dcache_wb(u32 a, u32 n)
{
    u32 f = (MFSR(DCM_CFG) >> 6) & 7, l = f ? 8u << (f - 1) : 32;
    for (u32 p = a & ~(l - 1); p < a + n; p += l)
        __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(p) : "memory");
    MSYNC();
}

static void icache_inval(u32 a, u32 n)
{
    u32 f = (MFSR(ICM_CFG) >> 6) & 7, l = f ? 8u << (f - 1) : 32;
    for (u32 p = a & ~(l - 1); p < a + n; p += l)
        __asm__ volatile("cctl %0, L1I_VA_INVAL" ::"r"(p) : "memory");
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

u32 irq_save(void)
{
    u32 psw = MFSR(PSW);
    GIE_OFF();
    return psw & 1;
}

void irq_restore(u32 flags)
{
    if (flags)
        GIE_ON();
}

int irq_is_on(void) { return on; }

void irq_set_heartbeat(const unsigned char *frame, int len) { hb_frame = frame, hb_len = len; }

static void uart_put(const u8 *p, int n)
{
    for (int i = 0; i < n; i++) {
        int k = 20000;
        while (!(REG(UART1_LSR) & 1) && --k)
            ;
        REG(UART1_DATA) = p[i];
    }
}

/* send pending heartbeat bytes only while the TX FIFO has room: the ISR never waits on the UART */
static void hb_pump(void)
{
    while (hb_pos >= 0 && hb_pos < hb_len && (REG(UART1_LSR) & 1))
        REG(UART1_DATA) = hb_frame[hb_pos++];
    if (hb_pos >= hb_len)
        hb_pos = -1;
}

/* called with GIE off before another frame goes out: finish a heartbeat the ISR has started */
void irq_uart_finish(void)
{
    if (hb_pos > 0) {
        uart_put(hb_frame + hb_pos, hb_len - hb_pos);
        hb_pos = -1;
    } else {
        hb_pos = -1;   /* not started yet: drop it, the next one is 3 s away */
    }
}

static void uart_drain(void)
{
    for (int k = 0; k < 64 && (REG(UART1_LSR) & 2); k++) {
        u8 b = (u8)REG(UART1_DATA);
        if (rx_w - rx_r >= sizeof(rxring))
            irq_rx_drop++;
        else
            rxring[rx_w++ % sizeof(rxring)] = b;
    }
}

int irq_rx_get(unsigned char *b)
{
    if (rx_r == rx_w)
        return 0;
    *b = rxring[rx_r % sizeof(rxring)];
    rx_r++;
    return 1;
}

/* storm guard: mask our two CPU inputs and stop the timer; IPSW still has GIE set, but nothing can interrupt */
static void guard_off(u32 why)
{
    u32 m = MFSR(INT_MASK) & ~3u;
    MTSR(m, INT_MASK);
    REG(INTC_MASK + TGRP) &= ~TBIT;
    REG(TMR_CTRL) = 0;
    irq_guard = why;
    on = 0;
}

/* ---- the interrupt service routine: only our timer, our state and UART1 ---- */
/* Hang watchdog: the main thread pets it (irq_pet() from hb_tick, which runs on every register access, delay and loop
 * pass); if it goes irq_wd_seconds without a pet, the tick ISR writes a crash record with vec = WD_VEC and the
 * interrupted PC, then resets. A hung payload otherwise sits with the heartbeat still running until a power cycle. */
#define WD_VEC 100
volatile u32 irq_wd_left, irq_wd_reload;
static u32 wd_seconds;
static void crash(u32 vec, u32 *frame);

void irq_watchdog(u32 seconds)
{
    wd_seconds = seconds;
    irq_wd_reload = seconds * hz_now;
    irq_wd_left = irq_wd_reload;
}

/* Hardware watchdog (SP7021-style WDT at 0x9C000630, as in Linux sunplus_wdt.c and U-Boot 0xf00946): a 20-bit
 * down-counter (measured ~80 kHz here, Linux assumes 90 kHz: 6 s set = ~6.7 s); CNT holds bits 19:4. The tick ISR reloads it, but only while the software hang
 * watchdog above is healthy, so it fires when the tick itself stops (a hang with GIE off, a CPU stalled on the bus,
 * or the payload gone) - the cases the tick-based watchdog cannot see. It must fire before the MCU cuts power for the
 * missing heartbeat (~9 s), which nothing can recover from short of an ignition cycle. */
#define WDT_CTRL 0x9c000630u
#define WDT_CNT  0x9c000634u
#define HWWDT_TEST_VEC 101
static u32 hw_wdt_count, hw_wdt_starve;
volatile u32 irq_hw_wdt_on;

void irq_hw_watchdog(u32 seconds)
{
    REG(WDT_CTRL) = 0x3877;   /* stop */
    irq_hw_wdt_on = 0;
    if (!seconds)
        return;
    if (seconds > 11)
        seconds = 11;
    hw_wdt_count = seconds * 90000u >> 4;
    hw_wdt_starve = 0;
    REG(WDT_CTRL) = 0xab00;   /* unlock */
    REG(WDT_CNT) = hw_wdt_count;
    REG(WDT_CTRL) = 0xab01;   /* lock */
    REG(WDT_CTRL) = 0x7482;   /* clear a latched expiry: one left from an earlier unrouted run resets the unit the
                                 moment routing is enabled (seen 2026-11-01) */
    REG(0x9c0000f4) = 0x00120012;   /* route the watchdog to system reset (bits 1 and 4, masked write), as U-Boot's
                                       reset 0xf00920 does first; at boot both are clear and expiry does nothing */
    REG(WDT_CTRL) = 0x4a4b;   /* resume */
    irq_hw_wdt_on = 1;
}

static void hw_wdt_pet(void)
{
    REG(WDT_CTRL) = 0xab00;
    REG(WDT_CNT) = hw_wdt_count;
    REG(WDT_CTRL) = 0xab01;
}

u32 irq_hw_wdt_left_ms(void) { return irq_hw_wdt_on ? ((REG(WDT_CNT) & 0xffff) << 4) / 90 : 0; }

static void isr(u32 vec, u32 *frame)
{
    irq_count[vec - 9]++;
    if ((REG(INTC_EXT0 + TGRP) | REG(INTC_EXT1 + TGRP)) & TBIT) {
        REG(INTC_CLEAR + TGRP) = TBIT;   /* eCos acknowledge (0x608150): write the bit, then msync */
        MSYNC();
        spur_streak = 0;
        if ((REG(INTC_EXT0 + TGRP) | REG(INTC_EXT1 + TGRP)) & TBIT) {
            if (++stuck_streak > 1000)
                guard_off(1);
        } else {
            stuck_streak = 0;
        }
        irq_ticks++;
        port_tick();
        if (irq_wd_reload && --irq_wd_left == 0)
            crash(WD_VEC, frame);
        if (irq_hw_wdt_on && !hw_wdt_starve && !(irq_ticks & 7))
            hw_wdt_pet();
        if (hb_frame && ++hb_cnt >= hb_every) {
            hb_cnt = 0;
            if (hb_pos < 0) {
                hb_pos = 0;
                irq_hb_sent++;
            }
        }
        hb_pump();
    } else {
        irq_spurious++;
        if (++spur_streak > 1000)
            guard_off(2);
    }
    uart_drain();
}

/* ---- crash path ---- */
static void reset_unit(void)
{
    REG(0x9c003104) |= 0x20;   /* U-Boot reset_cpu(0), 0xf0aa26 */
    REG(0x9c00317c) = 0;
    for (volatile int i = 0; i < 200000; i++)
        ;
    REG(0x9c000054) = 0x10001;
    for (;;)
        ;
}

static void crash_record(u32 vec, u32 *frame);
static void crash(u32 vec, u32 *frame)
{
    crash_record(vec, frame);
    REG(TMR_CTRL) = 0;
    reset_unit();
}

/* Proof that the hardware watchdog resets the unit: leave a crash record saying so (reported by the next boot as
 * "hwwdt-test") and stop petting. If the unit is still running seconds later, irq_hw_wdt_test_failed() clears it. */
void irq_hw_wdt_test(void)
{
    static u32 fake[34];
    crash_record(HWWDT_TEST_VEC, fake);
    hw_wdt_starve = 1;
}

int irq_hw_wdt_test_failed(void)
{
    if (!hw_wdt_starve)
        return 0;
    hw_wdt_starve = 0;
    REG(WDT_CTRL) = 0x7482;   /* it expired without resetting: clear the latch before anything enables routing */
    ((struct crash *)CRASH)->magic = 0;
    dcache_wb(CRASH_ADDR, sizeof(struct crash));
    return 1;
}

static void crash_record(u32 vec, u32 *frame)
{
    struct crash *c = CRASH;
    u8 *p = (u8 *)c;
    for (unsigned i = 0; i < sizeof(*c); i++)
        p[i] = 0;
    c->magic = CRASH_MAGIC, c->version = 3, c->vec = vec;
    c->log_len = irq_log_len_ptr ? *irq_log_len_ptr : 0;
    if (c->log_len && c->log_len <= 0x000f0000)
        dcache_wb(0x01a00000u, c->log_len);   /* the next boot saves this run's log to the card */
    c->itype = MFSR(ITYPE), c->ipc = frame[0], c->ipsw = frame[1], c->eva = MFSR(EVA);
    c->p_ipc = MFSR(P_IPC), c->p_ipsw = MFSR(P_IPSW), c->p_itype = MFSR(P_ITYPE), c->p_eva = MFSR(P_EVA);
    c->sp = (u32)frame + 136, c->psw = MFSR(PSW), c->ticks = irq_ticks, c->int_mask = MFSR(INT_MASK);
    c->regs[0] = frame[32], c->regs[1] = frame[33];
    for (int i = 2; i <= 27; i++)
        c->regs[i] = frame[i + 1];
    c->regs[28] = frame[29], c->regs[29] = frame[30], c->regs[30] = frame[31];
    for (int i = 0; i < 31 && hu_build_id[i]; i++)
        c->build[i] = hu_build_id[i];
    for (int i = 0; i < 47 && irq_note[i]; i++)
        c->note[i] = irq_note[i];
    c->uptime_ms = irq_uptime_ms;
    if (c->sp >= 0x00100000 && c->sp < 0x08000000 - 64)
        for (int i = 0; i < 16; i++)
            c->stack[i] = ((volatile u32 *)c->sp)[i];
    c->crc = crc32_le(p, (int)((u8 *)&c->crc - p));
    dcache_wb(CRASH_ADDR, sizeof(*c));
}

void hu_trap(u32 vec, u32 *frame);
void hu_trap(u32 vec, u32 *frame)
{
    if (vec >= 9 && vec <= 15) {
        isr(vec, frame);
        return;
    }
    if (vec == 7) {
        u32 it = MFSR(ITYPE);
        if (((it >> 16) & 0x7fff) == 0x7e58) {   /* FreeRTOS portYIELD: resume after the trap, switch on the way out */
            frame[0] += 4;
            port_yield_trap();
            return;
        }
        if (((it >> 16) & 0x7fff) == 0x7e57) {   /* irq_trap_test: resume after the trap instruction */
            last_itype = it;
            irq_test_hits++;
            frame[0] += 4;
            return;
        }
    }
    crash(vec, frame);
}

/* ---- setup ---- */
void irq_init(void)
{
    u32 n = (u32)hu_vectors_end - (u32)hu_vectors;
    const u32 *s = __vec_load;
    volatile u32 *d = (volatile u32 *)VEC_BASE;
    for (u32 i = 0; i < n / 4; i++)
        d[i] = s[i];
    dcache_wb(VEC_BASE, n);
    icache_inval(VEC_BASE, n);
    orig_ivb = MFSR(IVB), orig_psw = MFSR(PSW), orig_int_mask = MFSR(INT_MASK);
    /* eCos HAL (0x606e7e): INTL = 0, IVB = base | ESZ 16 bytes | low 13 bits; INT_CTRL = 0 if IVB bit 0;
     * INT_MASK2 = 0, INT_PEND2 = ~0 if IVIC_VER != 0 */
    u32 psw = orig_psw & ~6u;
    MTSR(psw, PSW);
    our_ivb = (orig_ivb & 0x1fff) | (1u << 14) | VEC_BASE;
    MTSR(our_ivb, IVB);
    __asm__ volatile("isb" ::: "memory");
    if (orig_ivb & 1) {
        u32 z = 0;
        MTSR(z, INT_CTRL);
    }
    if (orig_ivb & 0x1800) {
        u32 z = 0, m = ~0u;
        MTSR(z, INT_MASK2);
        MTSR(m, INT_PEND2);
    }
    installed = 1;
}

int irq_enable(u32 hz)
{
    if (!installed || hz < 3 || hz > 1000)
        return -1;
    u32 rclk = 27000000u / (2 * ((REG(STC_CFG) & 0xff) + 1)), d = 100, reload = rclk / d / hz;
    if (reload < 2 || reload > 0x10000)
        return -2;
    GIE_OFF();
    REG(TMR_CTRL) = 0;   /* eCos hal_clock_initialize (0x607f5a) */
    REG(TMR_PRESCALE) = d - 1;
    REG(TMR_RELOAD) = reload - 1;
    REG(TMR_COUNT) = reload - 1;
    REG(INTC_TYPE + TGRP) |= TBIT;   /* edge (0x608176 with r1 = 0) */
    REG(INTC_POL + TGRP) &= ~TBIT;   /* rising (r2 = 1) */
    REG(INTC_PRI + TGRP) |= TBIT;    /* EXT_INT0, as eCos/Linux init all sources */
    REG(INTC_CLEAR + TGRP) = TBIT;
    MSYNC();
    REG(INTC_MASK + TGRP) |= TBIT;   /* unmask (0x60812a) */
    u32 m = MFSR(INT_MASK) | 3;      /* HW0 + HW1, as eCos (0x608088) */
    MTSR(m, INT_MASK);
    hz_now = hz, irq_wd_reload = irq_wd_left = wd_seconds * hz, hb_every = 3 * hz, hb_cnt = 0, irq_guard = 0, spur_streak = stuck_streak = 0;
    REG(TMR_CTRL) = 0xf;
    on = 1;
    GIE_ON();
    return 0;
}

void irq_disable(void)
{
    GIE_OFF();
    on = 0;
    irq_hw_watchdog(0);   /* nothing pets it without the tick; callers that want it re-arm it (EXEC, failure exits) */
    if (!installed)
        return;
    REG(TMR_CTRL) = 0;
    REG(INTC_MASK + TGRP) &= ~TBIT;
    REG(INTC_CLEAR + TGRP) = TBIT;
    MSYNC();
    u32 m = (MFSR(INT_MASK) & ~3u) | (orig_int_mask & 3);
    MTSR(m, INT_MASK);
}

void irq_shutdown(void)
{
    irq_disable();
    if (!installed)
        return;
    MTSR(orig_ivb, IVB);
    u32 psw = (MFSR(PSW) & ~6u) | (orig_psw & 6);
    MTSR(psw, PSW);
    installed = 0;
}

/* called from the main loop: put back what someone else may have changed (U-Boot never does; counted) */
void irq_check(void)
{
    if (!installed)
        return;
    if (MFSR(IVB) != our_ivb) {
        irq_ivb_fix++;
        MTSR(our_ivb, IVB);
    }
    if (on && !(MFSR(PSW) & 1)) {
        irq_gie_fix++;
        GIE_ON();
    }
}

int irq_test_trap(void)
{
    u32 before = irq_test_hits;
    irq_trap_test();
    return irq_test_hits - before;
}

int irq_status(char *o)
{
    return plat_fmt(o, "irq=%s guard=%u hz=%u ticks=%u hw=%u/%u/%u/%u/%u/%u/%u spurious=%u hb_isr=%u rx_drop=%u traps=%u "
                       "itype=%x ivb=%x(orig %x, fixed %u) psw=%x(orig %x) int_mask=%x int_pend=%x gie_fix=%u "
                       "tmr=%x/%x/%x cnt=%x intc_mask4=%x pend4=%x/%x hwwdt=%s left=%u ms",
                    on ? "on" : "off", irq_guard, hz_now, irq_ticks, irq_count[0], irq_count[1], irq_count[2], irq_count[3],
                    irq_count[4], irq_count[5], irq_count[6], irq_spurious, irq_hb_sent, irq_rx_drop, irq_test_hits,
                    last_itype, MFSR(IVB), orig_ivb, irq_ivb_fix, MFSR(PSW), orig_psw, MFSR(INT_MASK),
                    MFSR(INT_PEND), irq_gie_fix, REG(TMR_CTRL), REG(TMR_PRESCALE), REG(TMR_RELOAD), REG(TMR_COUNT),
                    REG(INTC_MASK + TGRP), REG(INTC_EXT0 + TGRP), REG(INTC_EXT1 + TGRP),
                    irq_hw_wdt_on ? (hw_wdt_starve ? "starved" : "on") : "off", irq_hw_wdt_left_ms());
}

/* a crash record left by an earlier run: text into o (0 if none), then cleared */
u32 irq_prev_log_len(void)   /* log length kept in a valid crash record, before anything logs over it */
{
    const struct crash *c = CRASH;
    if (c->magic != CRASH_MAGIC || c->version != 3 ||
        c->crc != crc32_le((const u8 *)c, (int)((const u8 *)&c->crc - (const u8 *)c)))
        return 0;
    return c->log_len <= 0x000f0000 ? c->log_len : 0;
}

int irq_crash_report(char *o, char *brief)
{
    struct crash *c = CRASH;
    brief[0] = 0;
    if (c->magic != CRASH_MAGIC)
        return 0;
    int ok = c->version == 3 && c->crc == crc32_le((const u8 *)c, (int)((const u8 *)&c->crc - (const u8 *)c));
    char b[33], note[49];
    for (int i = 0; i < 32; i++)
        b[i] = c->build[i] >= 0x20 && c->build[i] < 0x7f ? c->build[i] : 0;
    b[32] = 0;
    for (int i = 0; i < 48; i++)
        note[i] = c->note[i] >= 0x20 && c->note[i] < 0x7f ? c->note[i] : 0;
    note[48] = 0;
    int k = plat_fmt(o, "CRASH RECORD%s: vec=%u itype=%x ipc=%x ipsw=%x eva=%x p_ipc=%x p_ipsw=%x p_itype=%x "
                        "p_eva=%x sp=%x psw=%x int_mask=%x ticks=%u up=%u build=%s\n  note: '%s'\n",
                     ok ? "" : " (CRC MISMATCH)", c->vec, c->itype, c->ipc, c->ipsw, c->eva, c->p_ipc, c->p_ipsw,
                     c->p_itype, c->p_eva, c->sp, c->psw, c->int_mask, c->ticks, c->uptime_ms, b, note);
    static const char *const names[31] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
                                           "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20",
                                           "r21", "r22", "r23", "r24", "r25", "r26", "r27", "fp", "gp", "lp" };
    for (int i = 0; i < 31; i++)
        k += plat_fmt(o + k, "%s%s=%08x%s", i % 8 ? " " : "  ", names[i], c->regs[i],
                      i % 8 == 7 || i == 30 ? "\n" : "");
    k += plat_fmt(o + k, "  stack:");
    for (int i = 0; i < 16; i++)
        k += plat_fmt(o + k, " %08x", c->stack[i]);
    k += plat_fmt(o + k, "\n");
    if (c->vec == WD_VEC)
        plat_fmt(brief, "hang@%x%s", c->ipc, ok ? "" : "?");
    else if (c->vec == HWWDT_TEST_VEC)
        plat_fmt(brief, "hwwdt-test%s", ok ? "" : "?");
    else if (c->vec >= 102 && c->vec <= 110)
        plat_fmt(brief, "%s%s", c->vec == 102 ? "probation" : "fail", ok ? "" : "?");
    else
        plat_fmt(brief, "vec%u/itype%x@%x%s", c->vec, c->itype, c->ipc, ok ? "" : "?");
    c->magic = 0;
    dcache_wb(CRASH_ADDR, sizeof(*c));
    return k;
}

void irq_crash_now(void) { irq_trap_fatal(); }

/* Planned reset with a crash record (vec 102 probation, 103.. other failures), from task context. */
void irq_fail_reset(unsigned int vec)
{
    static u32 fake[34];
    GIE_OFF();
    crash_record(vec, fake);
    REG(TMR_CTRL) = 0;
    reset_unit();
}
