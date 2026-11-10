/* Panel and display-pipeline bring-up for a boot that skipped U-Boot's logo routine (notes/display_touch.md,
 * "Installer boot: no panel init").
 *
 * On a normal power-on, vendor U-Boot's main_loop calls the logo routine sub_f2089e (0xF0BB36) and then sub_f73628
 * (0xF0BB44) before bootcmd. The stock flash's logo entry is the text "NULL", so the routine never enables OSD0, but it
 * still programs the whole display path, and this firmware relies on that. The SD-card installer's boot runs `ispsp`
 * from main_loop before that point and never comes back, so none of it happens and the panel stays black. Calling
 * U-Boot's routine from the payload would not help either: with the boot mode still saying ISP it reads the panel bin
 * from the update file in RAM, which our installer does not carry. So disp_init() repeats the routine's register
 * writes itself, in U-Boot's order, for the variant this unit's flash carries:
 *
 *   1. sub_f74d1c/sub_f74d68 (0xF208C4..): fields 0 and 1 of block 0x9C1C7000 (module 10 of U-Boot's table 0xFA0DD4):
 *      +0x80 = (bit 20 kept) | 6 << 16 | 0x53, +0x88 = (bit 20 kept) | 3 << 16 | 0x50, each then latched with bit 20.
 *   2. The register lists, run by U-Boot's interpreter loop (0xF20FEC), in table order (0xFB38F0): pinmux 0x9C000208,
 *      the DMIX defaults (24 writes), the OSD0..2 and GPOST0..2 defaults (12 writes), then the panel bin from flash
 *      (partition entry 1, 0x98018028): clocks, pinmux 0x9C000204..234, TCON 0x9C007880..9D4, PWM (OR), output
 *      mapping 0x9C008000..02C, TGEN 0x9C006A80..AB0 and TGEN reset 0x9C006A84 = 1. Each plain write is followed by
 *      sub_f1f7f2's short delay. U-Boot's flag that skips the PWM entries is never set (its setter 0xF205B6 has no
 *      callers), so they are written here too.
 *   3. sub_f1ff8a (panel type 1): pads 54..61 (and 62, 63 when TCON 0x9C007880 bit 15 is set) back to their pinmux
 *      function, GPIO 64..71, 9, 72, 73, 10 driven low, plus 11..16 or 33..38 when 0x9C000094 asks for it.
 *   4. The gamma table (0xF2130C), only if the list turned on TCON 0x9C007900 bit 0 (the stock list does not).
 *   5. sub_f1fa06 for layers 0..2 at 1:1 (GPOST scaler, OSD valid window), the DMIX layer order and TGEN adjust
 *      fields (0xF21518..0xF21674), sub_f1f84e (TGEN timing copied to the second set at 0x9C006B90).
 *   6. sub_f73628: GPIO86 high, DISP_PWM0 pinmux 0x9C000090, PWM0 enable with duty 0x14 (the backlight itself is the
 *      MCU's: `EE <level>`).
 * U-Boot's D-cache flush after step 5 is for its logo frame and is left out; nothing else is.
 *
 * Nothing here calls U-Boot. The panel bin is read through the flash window and checked before the first write:
 * every entry must be a kind U-Boot's loop knows and target a register (0x9C000000..0x9C00FFFF), the TGEN timing must
 * give 1024x600, and the bin must be the single-display, panel-type-1 variant. DISP_HOST builds it for test/disp_host.c
 * with the register accesses going to the test. */
#include "disp.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#ifdef DISP_HOST
u32 host_rd(u32 a);
void host_wr(u32 a, u32 v);
void host_wr16(u32 a, u16 v);
void host_udelay(u32 us);
#define RD(a)      host_rd(a)
#define WR(a, v)   host_wr(a, v)
#define WR16(a, v) host_wr16(a, v)
#define UDELAY(us) host_udelay(us)
#else
#include "clk.h"
#define RD(a)      (*(volatile u32 *)(a))
#define WR(a, v)   (*(volatile u32 *)(a) = (v))
#define WR16(a, v) (*(volatile u16 *)(a) = (v))
#define UDELAY(us) clk_udelay(us)
#endif

#define TGEN 0x9c006a80u
#define TCON 0x9c007880u   /* panel timing controller; +0x80 (0x9C007900) holds the gamma enable */

static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 le16(const u8 *p) { return p[0] | (u32)p[1] << 8; }

/* U-Boot's entry kinds, by address bits [31:21] (0xF21018..0xF21134) */
enum { K_WRITE, K_OR, K_SKIP, K_GAMMA, K_DELAY, K_POLL_SET, K_POLL_CLR };

static int kind(u32 a)
{
    u32 t = a & 0xffe00000u;
    if (t == 0x00800000u)
        return K_SKIP;
    if (t > 0x00800000u)
        return t == 0x02000000u ? K_GAMMA : t == 0x9c400000u ? K_OR : t == 0x01000000u ? K_POLL_CLR : K_WRITE;
    return t == 0 ? K_POLL_SET : t == 0x00200000u ? K_DELAY : K_WRITE;
}

static u32 target(u32 a, int k)   /* the register an entry reads or writes */
{
    if (k == K_OR)
        return a & ~(1u << 22);
    if (k == K_POLL_SET)
        return a | 0x9c000000u;
    return k == K_POLL_CLR ? 0x9c000000u | (a & 0xffffffu) : a;
}

static int reg_ok(u32 r) { return r >= 0x9c000000u && r < 0x9c010000u && !(r & 3); }

int disp_panel_find(const unsigned char *flash, unsigned size, struct disp_panel *p, const char **why)
{
    const u8 *e = flash + DISP_PART_PANEL;
    u32 off = le32(e) & 0x07ffffffu, len = le32(e + 4);
    *why = "ok";
    if (len >> 30) {
        *why = "panel bin is compressed";
        return -1;
    }
    if (len < 0x20 || len > 0x10000 || off >= size || len > size - off) {
        *why = "no panel partition";
        return -1;
    }
    const u8 *b = flash + off;
    u32 idx = le16(b + 2);
    if (12 + 4 * idx > len) {
        *why = "panel bin header";
        return -1;
    }
    u32 po = le32(b + 8 + 4 * idx);
    if (po > len - 16) {
        *why = "panel bin header";
        return -1;
    }
    const u8 *pn = b + po;
    if (pn[0] != 'P' || pn[1] != 'A' || pn[2] != 'N' || pn[3] != 'E' || pn[4] != 'L') {
        *why = "no PANEL tag";
        return -1;
    }
    u32 n = le16(pn + 8);
    if (n == 0 || n > (len - po - 16) / 8) {
        *why = "panel list size";
        return -1;
    }
    const u8 *tail = pn + 12 + 8 * n;
    if (tail[2]) {
        *why = "two-display panel bin (not supported)";
        return -1;
    }
    if (tail[3] != 1) {
        *why = "panel type is not 1 (not supported)";
        return -1;
    }
    u32 ht = 0, vt = 0, ha = 0x400, ve = 0x277, vs = 0x1e;   /* U-Boot's defaults (0xF20D6C) for the last three */
    for (u32 i = 0; i < n; i++) {
        u32 a = le32(pn + 12 + 8 * i), v = le32(pn + 16 + 8 * i);
        int k = kind(a);
        if (k == K_GAMMA ? a - 0x02000000u > 252 : k == K_DELAY ? v > 1000000 : k != K_SKIP && !reg_ok(target(a, k))) {
            *why = "panel list entry outside the known kinds and registers";
            return -1;
        }
        if (k != K_WRITE)
            continue;
        if (a == TGEN + 0x20)
            ht = v & 0xfff;
        else if (a == TGEN + 0x24)
            ha = v & 0xfff;
        else if (a == TGEN + 0x28)
            vt = v & 0xfff;
        else if (a == TGEN + 0x2c)
            ve = v & 0xfff;
        else if (a == TGEN + 0x30)
            vs = v & 0xfff;
    }
    if (!ht || !vt || ha != DISP_WIDTH || ve < vs + 1 || ve - 1 - vs != DISP_HEIGHT) {
        *why = "panel timing is not 1024x600";
        return -1;
    }
    p->ent = pn + 12, p->n = n;
    p->htotal = ht, p->hact = ha, p->vtotal = vt, p->vend = ve, p->vstart = vs;
    return 0;
}

int disp_state(const struct disp_panel *p, struct disp_state *s)
{
    s->cfg = RD(TGEN), s->rst = RD(TGEN + 4);
    s->htotal = RD(TGEN + 0x20) & 0xfff, s->hact = RD(TGEN + 0x24) & 0xfff, s->vtotal = RD(TGEN + 0x28) & 0xfff;
    s->vend = RD(TGEN + 0x2c) & 0xfff, s->vstart = RD(TGEN + 0x30) & 0xfff;
    s->tcon = RD(TCON);
    s->line0 = RD(TGEN + 0x34) & 0xfff;   /* current line (vpp.c waits on it) */
    UDELAY(200);                          /* about 8 lines at 1344 x 635 x 60 Hz */
    s->line1 = RD(TGEN + 0x34) & 0xfff;
    s->moving = s->line0 != s->line1;
    s->timing_ok = p && s->htotal == p->htotal && s->hact == p->hact && s->vtotal == p->vtotal && s->vend == p->vend &&
                   s->vstart == p->vstart;
    return s->timing_ok;
}

/* ---- the sequence ---- */

static u8 gamma_tab[256];   /* U-Boot's table at gp+0x259B0, filled by the list's 0x02xxxxxx entries */

static void settle(void) { UDELAY(1); }   /* sub_f1f7f2: ten rounds of four nops after each write */

static void run_entry(u32 a, u32 v)
{
    int k = kind(a);
    u32 r = target(a, k), i;
    switch (k) {
    case K_WRITE:
        WR(r, v);
        settle();
        break;
    case K_OR:
        WR(r, RD(r) | v);
        settle();
        break;
    case K_GAMMA:
        i = a - 0x02000000u;
        for (u32 b = 0; b < 4; b++)
            gamma_tab[i + b] = (u8)(v >> 8 * b);
        break;
    case K_DELAY:
        UDELAY(v > 1000 ? v / 1000 * 1000 : v);   /* U-Boot: mdelay(v / 1000) above 1000, else udelay(v) */
        break;
    case K_POLL_SET:   /* until all bits of v are set, at most 1002 x 100 us */
        for (i = 0; i < 1002 && (RD(r) & v) != v; i++)
            UDELAY(100);
        break;
    case K_POLL_CLR:   /* until all bits of v are clear */
        for (i = 0; i < 1002 && (RD(r) & v); i++)
            UDELAY(100);
        break;
    default:
        break;
    }
}

/* U-Boot's static lists (0xFB2F0C, 0xFB2E4C, 0xFB2DEC: table entries 0, 1 and 3 at 0xFB38F0) */
static const u32 list_pinmux[][2] = { { 0x9c000208, 0x04600460 } };
static const u32 list_dmix[][2] = {
    { 0x9c006c80, 0x3070 }, { 0x9c006c84, 0x8555 }, { 0x9c006c88, 0x6254 }, { 0x9c006ca0, 0x2000 },
    { 0x9c006cac, 0x10 },   { 0x9c006cb0, 0x80 },   { 0x9c006cb4, 0x80 },   { 0x9c006ce8, 0x2 },
    { 0x9c006d00, 0x3070 }, { 0x9c006d04, 0x8555 }, { 0x9c006d08, 0x6254 }, { 0x9c006d20, 0x2000 },
    { 0x9c006d2c, 0x10 },   { 0x9c006d30, 0x80 },   { 0x9c006d34, 0x80 },   { 0x9c006d68, 0x0 },
    { 0x9c006d80, 0x3070 }, { 0x9c006d84, 0x8555 }, { 0x9c006d88, 0x6254 }, { 0x9c006d8c, 0x2000 },
    { 0x9c006dac, 0x10 },   { 0x9c006db0, 0x80 },   { 0x9c006db4, 0x80 },   { 0x9c006de8, 0x2 },
};
static const u32 list_osd[][2] = {
    { 0x9c006200, 0x437 }, { 0x9c006204, 0 }, { 0x9c006208, 0 }, { 0x9c006280, 0x437 }, { 0x9c006284, 0 },
    { 0x9c006288, 0 },     { 0x9c006300, 0x437 }, { 0x9c006304, 0 }, { 0x9c006308, 0 }, { 0x9c006480, 1 },
    { 0x9c006500, 1 },     { 0x9c006580, 1 },
};

static void run_list(const u32 (*l)[2], u32 n)
{
    for (u32 i = 0; i < n; i++)
        run_entry(l[i][0], l[i][1]);
}

static void setf(u32 a, u32 clr, u32 set) { WR(a, (RD(a) & ~clr) | set); }

/* sub_f74d1c then sub_f74d68 on one field of block 0x9C1C7000 */
static void clk_field(u32 r, u32 hi, u32 lo)
{
    WR(r, (RD(r) & (1u << 20)) | (hi & 0xf) << 16 | lo);
    WR(r + 4, 0);
    u32 a = RD(r), b = RD(r + 4);
    WR(r, a | 1u << 20);
    WR(r + 4, b);
}

/* GPIO block (SP7021 layout): MASTER 0x9C000300, OE 0x9C000320, OUT 0x9C000340, 16 pins per word with a write-enable
 * mask in bits [31:16]; FIRST 0x9C0032E4, 32 pins per word, read-modify-write */
#define GPIO_FIRST(p) (0x9c0032e4u + ((p) >> 5) * 4)
static void gpio_drive(u32 p, u32 v)   /* sub_f75038: OUT, OE, MASTER, FIRST */
{
    u32 w = (p >> 4) * 4, s = p & 15;
    WR(0x9c000340u + w, (0x10000u | v) << s);
    WR(0x9c000320u + w, 0x10001u << s);
    WR(0x9c000300u + w, 0x10001u << s);
    WR(GPIO_FIRST(p), RD(GPIO_FIRST(p)) | 1u << (p & 31));
}

static void gpio_release(u32 p)   /* sub_f750e0: FIRST off, then MASTER, OE and OUT to 0 */
{
    u32 w = (p >> 4) * 4, m = 0x10000u << (p & 15);
    WR(GPIO_FIRST(p), RD(GPIO_FIRST(p)) & ~(1u << (p & 31)));
    WR(0x9c000300u + w, m);
    WR(0x9c000320u + w, m);
    WR(0x9c000340u + w, m);
}

static void panel_pins(void)   /* sub_f1ff8a(1, TCON bit 15) */
{
    static const u8 low[] = { 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 9, 0x48, 0x49, 10 };
    for (u32 p = 0x36; p <= 0x3d; p++)
        gpio_release(p);
    if (RD(TCON) & 0x8000)
        gpio_release(0x3e), gpio_release(0x3f);
    else
        gpio_drive(0x3f, 0), gpio_drive(0x3e, 0);
    for (u32 i = 0; i < sizeof low; i++)
        gpio_drive(low[i], 0);
    u32 m = (RD(0x9c000094) & 6) >> 1;
    for (u32 p = 11; m == 1 && p <= 16; p++)
        gpio_drive(p, 0);
    for (u32 p = 33; m == 2 && p <= 38; p++)
        gpio_drive(p, 0);
}

static void gamma_load(void)   /* 0xF2130C..0xF2137E */
{
    u32 g = TCON + 0x80;
    setf(g, 0x20, 0), setf(g, 0x8000, 0), setf(g, 0xc, 0), setf(g, 0, 0x10), setf(g, 0, 1);
    for (u32 i = 0; i < 256; i++) {
        u32 d = (u32)(int)(signed char)gamma_tab[i] << 4;   /* U-Boot loads the byte signed */
        WR(TCON + 0xec, 2 * i), WR(TCON + 0xf0, d), setf(g, 0, 2);
        WR(TCON + 0xec, 2 * i + 1), WR(TCON + 0xf0, d), setf(g, 0, 2);
    }
    setf(g, 0x10, 0), setf(g, 2, 0);
}

static void gpost_1to1(u32 n)   /* sub_f1fa06(n, {1024, 600, 1024, 600}): no overscan, scale 1.0 */
{
    u32 g = 0x9c006480u + n * 0x80, o = 0x9c006200u + n * 0x80;
    WR(g + 0x30, 0), WR(g + 0x34, 0), WR(g + 0x38, DISP_WIDTH), WR(g + 0x3c, DISP_HEIGHT);
    u32 ow = RD(g + 0x38) & 0xffff, oh = RD(g + 0x3c) & 0xffff, v;
    WR(g + 0x4c, (RD(g + 0x4c) & 0xff0f) | 0x30);
    WR(g + 0x50, 0), WR(g + 0x54, 0x40), WR(g + 0x58, 0), WR(g + 0x5c, 0);
    WR(g + 0x68, (RD(g + 0x68) & 0xff0f) | 0x10);
    WR(g + 0x6c, 0), WR(g + 0x70, 0x40);
    WR(g + 0x10, DISP_WIDTH), WR(g + 0x14, DISP_HEIGHT), WR(g + 0x28, DISP_WIDTH), WR(g + 0x2c, DISP_HEIGHT);
    setf(g + 4, 1, 0);
    v = RD(g + 4), WR(g + 4, ow == DISP_WIDTH && oh == DISP_HEIGHT ? v & ~2u : v | 2u);
    v = RD(g + 4), WR(g + 4, ow == DISP_WIDTH ? v & ~4u : v | 4u);
    v = RD(g + 4), WR(g + 4, oh == DISP_HEIGHT ? v & ~8u : v | 8u);
    setf(g + 4, 0, 0x10);
    WR(o + 0x40, 0), WR(o + 0x44, (DISP_WIDTH + 3) & ~3u), WR(o + 0x48, 0), WR(o + 0x4c, DISP_HEIGHT);
}

void disp_init(const struct disp_panel *p)
{
    clk_field(0x9c1c7080u, 6, 0x53);
    clk_field(0x9c1c7088u, 3, 0x50);

    for (u32 i = 0; i < sizeof gamma_tab; i++)
        gamma_tab[i] = 0;
    run_list(list_pinmux, sizeof list_pinmux / sizeof list_pinmux[0]);
    run_list(list_dmix, sizeof list_dmix / sizeof list_dmix[0]);
    run_list(list_osd, sizeof list_osd / sizeof list_osd[0]);
    for (u32 i = 0; i < p->n; i++)
        run_entry(le32(p->ent + 8 * i), le32(p->ent + 8 * i + 4));

    panel_pins();
    if (RD(TCON + 0x80) & 1)
        gamma_load();

    for (u32 n = 0; n < 3; n++)
        gpost_1to1(n);
    /* DMIX layer order (sub_f1f80c: 0, 3, 4, 5) for both mixers, TGEN adjust fields of both timing sets */
    setf(0x9c006c80, 0x700, 0 << 8), setf(0x9c006c80, 0x7000, 3 << 12);
    setf(0x9c006c88, 0x7, 4), setf(0x9c006c88, 0x70, 5 << 4);
    setf(TGEN + 0x5c, 0x3f00, 0x1000), setf(TGEN + 0x60, 0x3f, 0xa), setf(TGEN + 0x64, 0x3f, 0xe);
    setf(TGEN + 0x64, 0x3f00, 0xc00);
    setf(0x9c006d00, 0x700, 0 << 8), setf(0x9c006d00, 0x7000, 3 << 12);
    setf(0x9c006d08, 0x7, 4), setf(0x9c006d08, 0x70, 5 << 4);
    setf(0x9c006b5c, 0x3f00, 0x1000), setf(0x9c006b60, 0x3f, 0xa), setf(0x9c006b64, 0x3f, 0xe);
    setf(0x9c006b64, 0x3f00, 0xc00);
    setf(0x9c006c84, 0xc, 0);   /* OSD0 (L2) blends with its per-pixel alpha */
    for (u32 i = 0; i < 9; i++)  /* sub_f1f84e */
        WR(0x9c006b90u + 4 * i, RD(TGEN + 0x10 + 4 * i));
    setf(0x9c006bdc, 0x3f00, 0x1700);

    /* sub_f73628 */
    gpio_drive(86, 1);
    WR(0x9c000090, 0x000f0001);   /* pinmux function 0x13 (DISP_PWM0) = 1: group 4, bits [3:0] */
    setf(0x9c007a04, 1, 1);
    WR16(0x9c007a08, 0x14);       /* sub_f74738 stores the duty as a halfword */
    setf(0x9c007a04, 0x100, 0x100);
}
