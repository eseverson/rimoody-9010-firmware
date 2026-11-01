/* Goodix GT9xx touch controller over bit-banged I2C, as eCos does it (notes/display_touch.md, "Touch controller").
 *
 * Pins (eCos sysio table 0x6C7A64): RST = GPIO90, SDA = GPIO91, SCL = GPIO92, INT = GPIO93. GPIO block registers
 * (SP7021 layout, checked live: OE 0x9C000334 bit 6 drives the backlight GPIO86):
 *   OE  0x9C000334, OUT 0x9C000354: bits[31:16] write-enable mask, bits[15:0] value, bit = pin - 80
 *   IN  0x9C000368: bit = pin - 64
 *   FIRST 0x9C0032EC: bit = pin - 64, 1 = pad owned by the GPIO block (U-Boot leaves 90..93 at 0)
 * Open drain: OUT = 0, a line is pulled low with OE = 1 and released (external pull-up) with OE = 0.
 *
 * An earlier attempt from the desktop (tools/gt9.py, one U-Boot `mw.l` per toggle) "rebooted the unit"; that was the
 * payload's unbounded log formatter overflowing on the ~900-character command packets, not these pins.
 *
 * Nothing here writes to the touch controller except the 0x814E status acknowledge that every GT9xx driver does;
 * the 186-byte configuration is never written. RST and INT are only driven by gt9_reset(). */
#include "gt9.h"
#include "irq.h"

typedef unsigned int u32;
typedef unsigned char u8;
#define REG(a) (*(volatile u32 *)(a))

#define G_OE    0x9c000334u
#define G_OUT   0x9c000354u
#define G_IN    0x9c000368u
#define G_FIRST 0x9c0032ecu
#define P_RST 90
#define P_SDA 91
#define P_SCL 92
#define P_INT 93
#define M(p)  (1u << ((p) - 80))   /* OE/OUT bit */
#define F(p)  (1u << ((p) - 64))   /* IN/FIRST bit */

#include "freertos/port/port_nds32.h"
#define plat_udelay port_udelay   /* runs in the touch task: U-Boot's udelay is for the net task only */
void plat_logf(const char *fmt, ...);

static u32 saved_first, owned;
static u8 addr = 0x5d;
static int busy;   /* gt9_op and gt9_tick never interleave on the bus */
static int ev_state;   /* 0 = not started, 1 = polling, -1 = off (no chip, or turned off) */
char gt9_last[48];
volatile u32 gt9_down_ms;

static void drive(u32 reg, int pin, int v) { REG(reg) = (M(pin) << 16) | (v ? M(pin) : 0); }
static int in(int pin) { return (REG(G_IN) & F(pin)) != 0; }
static void dly(void) { plat_udelay(5); }   /* ~100 kHz; the 100 Hz tick ISR only stretches it */
static void sda(int high) { drive(G_OE, P_SDA, !high); }
static int scl(int high)
{
    drive(G_OE, P_SCL, !high);
    if (!high)
        return 0;
    for (int i = 0; i < 1000; i++) {   /* clock stretching, up to ~1 ms */
        if (in(P_SCL))
            return 0;
        plat_udelay(1);
    }
    return -1;
}

static void take(void)
{
    if (owned)
        return;
    saved_first = REG(G_FIRST);
    drive(G_OE, P_SDA, 0), drive(G_OE, P_SCL, 0);    /* released */
    drive(G_OUT, P_SDA, 0), drive(G_OUT, P_SCL, 0);  /* pull low when OE = 1 */
    REG(G_FIRST) = saved_first | F(P_SDA) | F(P_SCL);
    owned = 1;
    plat_udelay(50);
}

void gt9_release(void)
{
    if (!owned)
        return;
    drive(G_OE, P_SDA, 0), drive(G_OE, P_SCL, 0);
    REG(G_FIRST) = (REG(G_FIRST) & ~(F(P_SDA) | F(P_SCL))) | (saved_first & (F(P_SDA) | F(P_SCL)));
    owned = 0;
}

static void start(void) { sda(1); scl(1); dly(); sda(0); dly(); scl(0); dly(); }
static void stop(void) { sda(0); dly(); scl(1); dly(); sda(1); dly(); }

static int wbyte(u8 b)   /* 1 = ACK */
{
    for (int i = 7; i >= 0; i--) {
        sda((b >> i) & 1);
        dly();
        scl(1);
        dly();
        scl(0);
    }
    sda(1);
    dly();
    scl(1);
    dly();
    int ack = !in(P_SDA);
    scl(0);
    dly();
    return ack;
}

static u8 rbyte(int ack)
{
    u8 v = 0;
    sda(1);
    for (int i = 0; i < 8; i++) {
        dly();
        scl(1);
        dly();
        v = (u8)(v << 1 | in(P_SDA));
        scl(0);
    }
    sda(!ack);
    dly();
    scl(1);
    dly();
    scl(0);
    sda(1);
    return v;
}

static void recover(void)   /* a slave left holding SDA low: clock it out */
{
    for (int i = 0; i < 9 && !in(P_SDA); i++) {
        scl(0); dly(); scl(1); dly();
    }
    stop();
}

static int bus_ok(void)
{
    if (in(P_SDA) && in(P_SCL))
        return 1;
    recover();
    return in(P_SDA) && in(P_SCL);
}

static int probe_addr(u8 a)
{
    start();
    int ack = wbyte((u8)(a << 1));
    stop();
    return ack;
}

/* 16-bit register address, MSB first, repeated start, as eCos 0x516990. 0 ok, <0 NACK stage. */
static int rd(unsigned reg, u8 *buf, int n)
{
    int r = 0;
    start();
    if (!wbyte((u8)(addr << 1)))
        r = -1;
    else if (!wbyte((u8)(reg >> 8)) || !wbyte((u8)reg))
        r = -2;
    else {
        start();
        if (!wbyte((u8)(addr << 1 | 1)))
            r = -3;
        else
            for (int i = 0; i < n; i++)
                buf[i] = rbyte(i < n - 1);
    }
    stop();
    return r;
}

static int wr(unsigned reg, const u8 *buf, int n)
{
    int r = 0;
    start();
    if (!wbyte((u8)(addr << 1)))
        r = -1;
    else if (!wbyte((u8)(reg >> 8)) || !wbyte((u8)reg))
        r = -2;
    else
        for (int i = 0; i < n && !r; i++)
            if (!wbyte(buf[i]))
                r = -4;
    stop();
    return r;
}

static int pins(char *o)
{
    u32 in_ = REG(G_IN), oe = REG(G_OE), out = REG(G_OUT), first = REG(G_FIRST);
    return plat_fmt(o, "pins rst/sda/scl/int: in=%u%u%u%u first=%u%u%u%u oe=%u%u%u%u out=%u%u%u%u (owned=%u addr=%x)\n",
                    !!(in_ & F(P_RST)), !!(in_ & F(P_SDA)), !!(in_ & F(P_SCL)), !!(in_ & F(P_INT)),
                    !!(first & F(P_RST)), !!(first & F(P_SDA)), !!(first & F(P_SCL)), !!(first & F(P_INT)),
                    !!(oe & M(P_RST)), !!(oe & M(P_SDA)), !!(oe & M(P_SCL)), !!(oe & M(P_INT)),
                    !!(out & M(P_RST)), !!(out & M(P_SDA)), !!(out & M(P_SCL)), !!(out & M(P_INT)), owned, addr);
}

static int hex(char *o, const u8 *b, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        k += plat_fmt(o + k, "%s%02x", i ? " " : "", b[i]);
    return k;
}

/* eCos 0x516AE4: RST low 1 ms, INT low 10 ms (selects 0x5D at the RST rising edge), RST high 50 ms, INT released.
 * RST/INT are put back to their U-Boot state (not GPIO-owned) afterwards except RST, which stays driven high. */
static int reset(char *o)
{
    u32 first = REG(G_FIRST);
    drive(G_OUT, P_RST, 0), drive(G_OUT, P_INT, 0);
    drive(G_OE, P_RST, 1);
    REG(G_FIRST) = first | F(P_RST) | F(P_INT);
    plat_udelay(1000);
    drive(G_OE, P_INT, 1);
    plat_udelay(10000);
    drive(G_OUT, P_RST, 1);
    plat_udelay(50000);
    drive(G_OE, P_INT, 0);
    REG(G_FIRST) = REG(G_FIRST) & ~F(P_INT);
    addr = 0x5d;
    plat_udelay(50000);
    return plat_fmt(o, "reset done (RST high, INT released)\n");
}

static int op(const u8 *p, int n, char *o);

static int claim(void)   /* the touch task and the net task (HU1 op 14) share the bus */
{
    u32 f = irq_save();
    int ok = !busy;
    busy = 1;
    irq_restore(f);
    return ok;
}

int gt9_op(const u8 *p, int n, char *o)
{
    if (n && p[0] == 6) {
        ev_state = n > 1 && p[1] ? 0 : -1;
        return plat_fmt(o, "touch events %s\n", ev_state ? "off" : "on");
    }
    if (!claim())
        return plat_fmt(o, "ERR touch busy\n");
    int k = op(p, n, o);
    busy = 0;
    return k;
}

/* Front buttons: a vertical row of touch areas past the 1024-px display (panel X range 1070), centers measured live. */
static const unsigned short btn_y[5] = {141, 211, 322, 404, 508};

static int button(unsigned x, unsigned y)
{
    if (x < 1030)
        return 0;
    int best = 0;
    for (int i = 1; i < 5; i++)
        if ((y > btn_y[i] ? y - btn_y[i] : btn_y[i] - y) < (y > btn_y[best] ? y - btn_y[best] : btn_y[best] - y))
            best = i;
    return best + 1;
}

/* Status 0x814E, first record from 0x814F, acknowledge; logs touch down/up and button presses (not every move). */
void (*gt9_hook)(int action, unsigned x, unsigned y);

void gt9_tick(u32 now)
{
    static u32 last;
    static int down, down_btn;
    static unsigned lx, ly;
    if (ev_state < 0 || now - last < 25 || !claim())
        return;
    last = now;
    if (!ev_state) {
        take();
        int ok = bus_ok() && probe_addr(0x5d);
        ev_state = ok ? 1 : -1;
        plat_logf("touch: events %s\n", ok ? "on (gt911 at 0x5d)" : "off (no ack at 0x5d)");
        if (!ok) {
            busy = 0;
            return;
        }
        addr = 0x5d;
    }
    u8 b[12];
    if (rd(0x814e, b, 10) == 0 && (b[0] & 0x80)) {
        int np = b[0] & 0x0f;
        unsigned x = b[2] | b[3] << 8, y = b[4] | b[5] << 8;
        if (np && !down) {
            gt9_down_ms = now ? now : 1;
            int btn = button(x, y);
            down_btn = btn;
            if (!btn && gt9_hook)
                gt9_hook(0, x, y);
            if (btn)
                plat_fmt(gt9_last, "button %d", btn);
            else
                plat_fmt(gt9_last, "touch %u,%u", x, y);
            plat_logf("touch: down %u %u%s%s\n", x, y, btn ? " " : "", btn ? gt9_last : "");
        } else if (!np && down) {
            plat_logf("touch: up %u %u\n", lx, ly);
            if (!down_btn && gt9_hook)
                gt9_hook(1, lx, ly);
        } else if (np && down && !down_btn && (x != lx || y != ly) && gt9_hook) {
            gt9_hook(2, x, y);
        }
        if (np)
            lx = x, ly = y;
        down = np != 0;
        u8 z = 0;
        wr(0x814e, &z, 1);
    }
    busy = 0;
}

static int op(const u8 *p, int n, char *o)
{
    int c = n ? p[0] : 0, k = 0;
    if (c == 9) {
        ev_state = -1;
        gt9_release();
        return pins(o);
    }
    if (c == 0)
        return pins(o);
    take();
    k += pins(o + k);
    if (!bus_ok())
        return k + plat_fmt(o + k, "ERR bus stuck low after recovery (no pull-ups, or the pads are not these lines)\n");
    if (c == 5)
        k += reset(o + k);
    if (c == 1 || c == 5) {
        int a5d = probe_addr(0x5d), a14 = probe_addr(0x14);
        if (!a5d && a14)
            addr = 0x14;
        k += plat_fmt(o + k, "probe: 0x5d %s, 0x14 %s\n", a5d ? "ACK" : "nack", a14 ? "ACK" : "nack");
        if (!a5d && !a14)
            return k;
    }
    u8 b[200];
    if (c == 1 || c == 2 || c == 5) {   /* product id, firmware, x/y resolution, vendor */
        int r = rd(0x8140, b, 11);
        if (r)
            return k + plat_fmt(o + k, "ERR id read %d\n", r);
        return k + plat_fmt(o + k, "id '%c%c%c%c' fw %02x%02x x %u y %u vendor %02x\n", b[0] >= 0x20 ? b[0] : '?',
                            b[1] >= 0x20 ? b[1] : '?', b[2] >= 0x20 ? b[2] : '?', b[3] >= 0x20 ? b[3] : ' ', b[5],
                            b[4], b[6] | b[7] << 8, b[8] | b[9] << 8, b[10]);
    }
    if (c == 3 && n >= 4) {   /* read <reg16 be> <len> */
        unsigned reg = p[1] << 8 | p[2], len = p[3] > sizeof b ? sizeof b : p[3];
        int r = rd(reg, b, (int)len);
        if (r)
            return k + plat_fmt(o + k, "ERR read %x %d\n", reg, r);
        k += plat_fmt(o + k, "%04x:", reg);
        for (unsigned i = 0; i < len; i += 16) {
            k += plat_fmt(o + k, i ? "\n     " : " ");
            k += hex(o + k, b + i, len - i < 16 ? (int)(len - i) : 16);
        }
        return k + plat_fmt(o + k, "\n");
    }
    if (c == 4) {   /* poll, as Linux goodix.c: status 0x814E then 8-byte records from 0x814F, then acknowledge */
        int r = rd(0x814e, b, 1);
        if (r)
            return k + plat_fmt(o + k, "ERR status read %d\n", r);
        u8 st = b[0];
        int np = st & 0x0f;
        k += plat_fmt(o + k, "status %02x ready=%u touches=%u key=%u int=%u\n", st, st >> 7, np, (st >> 4) & 1,
                      in(P_INT));
        if (!(st & 0x80))
            return k;
        if (np > 5)
            np = 5;
        if (!rd(0x814e, b, 2 + np * 8)) {   /* + the byte after the records: the key bits on touch-key panels */
            for (int i = 0; i < np; i++) {
                const u8 *q = b + 1 + i * 8;
                k += plat_fmt(o + k, "  id %u x %u y %u size %u\n", q[0], q[1] | q[2] << 8, q[3] | q[4] << 8,
                              q[5] | q[6] << 8);
            }
            k += plat_fmt(o + k, "  raw ");
            k += hex(o + k, b, 2 + np * 8);
            k += plat_fmt(o + k, "\n");
        }
        u8 z = 0;
        r = wr(0x814e, &z, 1);
        if (r)
            k += plat_fmt(o + k, "ERR ack write %d\n", r);
        return k;
    }
    return k + plat_fmt(o + k, "ERR touch: 0 pins, 1 probe+id, 2 id, 3 read reg16 len, 4 poll, 5 reset+probe, 6 events 0/1, 9 release\n");
}
