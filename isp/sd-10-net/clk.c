/* Our own time base, replacing U-Boot's get_timer/udelay for our code (notes/rtos.md, milestone 3).
 *
 * The counter is the one U-Boot's timer.c uses (0xF00A94 / 0xF00B2C / init 0xF00B92): group 0x9C003180, a 32-bit
 * up-counter at 1 MHz (prescaler 0x9C00318C = 270 MHz / 1 MHz - 1 = 0x10D, measured: 6020868 counts in 6.02 s).
 * Writing 0x1234 to +0x68 latches it; +0x60 / +0x64 then hold the low / high 16 bits. The live halves at +0x00 / +0x04
 * are not read here (no carry-safe pair read).
 *
 * U-Boot's get_timer zeroes the counter (+0x00, +0x04, +0x08) once it reaches 4294 s and carries the time in gd. So a
 * reading lower than the previous one means "U-Boot reset it": the 64-bit clock adds the previous reading as its base
 * (it loses at most the time since that previous reading, which is small because something reads every few ms).
 *
 * Safe from any task: the latch and both reads happen with GIE off. Preemption happens only at a tick, so the chance
 * of landing between U-Boot's own latch and its two reads (and handing it a mixed pair) is negligible. */
#include "clk.h"
#include "irq.h"

typedef unsigned int u32;
typedef unsigned long long u64;
#define REG(a) (*(volatile u32 *)(a))

#define CNT_LATCH 0x9c0031e8u
#define CNT_LO    0x9c0031e0u
#define CNT_HI    0x9c0031e4u

static u32 last;
static u64 base;

u64 clk_us(void)
{
    u32 f = irq_save();
    REG(CNT_LATCH) = 0x1234;
    u32 raw = (REG(CNT_HI) & 0xffff) << 16 | (REG(CNT_LO) & 0xffff);
    if (raw < last)
        base += last;
    last = raw;
    u64 t = base + raw;
    irq_restore(f);
    return t;
}

u32 clk_ms(void)
{
    return (u32)(clk_us() / 1000u);
}

void clk_udelay(u32 us)
{
    u64 end = clk_us() + us;
    while (clk_us() < end)
        ;
}
