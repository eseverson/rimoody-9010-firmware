/* Host test for disp.c: the panel bin checks, U-Boot's entry kinds, the order of the sequence and the values it
 * leaves, against a register file in memory. The panel bin is built here; its register writes are the ones the stock
 * flash's bin starts and ends with (notes/display_touch.md). Exit status 0 = all passed. */
#define DISP_HOST
#include "../disp.c"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c))                                                        \
            fails++, printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    } while (0)

/* ---- register file and write log ---- */
#define NREG 4096
#define NLOG 32768
static u32 reg_a[NREG], reg_v[NREG], nreg;
static struct { u32 a, v; int w16; } wlog[NLOG];
static u32 nlog, delay_us;

static u32 *slot(u32 a)
{
    for (u32 i = 0; i < nreg; i++)
        if (reg_a[i] == a)
            return &reg_v[i];
    if (nreg == NREG)
        return 0;
    reg_a[nreg] = a, reg_v[nreg] = 0;
    return &reg_v[nreg++];
}
u32 host_rd(u32 a) { u32 *s = slot(a); return s ? *s : 0; }
void host_wr(u32 a, u32 v)
{
    u32 *s = slot(a);
    if (s)
        *s = v;
    if (nlog < NLOG)
        wlog[nlog].a = a, wlog[nlog].v = v, wlog[nlog].w16 = 0, nlog++;
}
void host_wr16(u32 a, u16 v)
{
    u32 *s = slot(a);
    if (s)
        *s = (*s & 0xffff0000u) | v;
    if (nlog < NLOG)
        wlog[nlog].a = a, wlog[nlog].v = v, wlog[nlog].w16 = 1, nlog++;
}
void host_udelay(u32 us) { delay_us += us; }
static void reset_regs(void) { nreg = nlog = delay_us = 0; }
static void set(u32 a, u32 v) { *slot(a) = v; }

static int find_write(u32 a, u32 v, u32 from)   /* index of the first logged write a = v at or after from, or -1 */
{
    for (u32 i = from; i < nlog; i++)
        if (wlog[i].a == a && wlog[i].v == v)
            return (int)i;
    return -1;
}
static int last_write(u32 a)
{
    for (u32 i = nlog; i-- > 0;)
        if (wlog[i].a == a)
            return (int)i;
    return -1;
}
static u32 writes_to(u32 a)
{
    u32 n = 0;
    for (u32 i = 0; i < nlog; i++)
        n += wlog[i].a == a;
    return n;
}

/* ---- panel bin builder ---- */
#define FLASH_SZ 0x40000u
#define BIN_OFF  0x20000u
static u8 flash[FLASH_SZ];
static u32 ent[256][2], nent;
static u8 tail[4];

static void put32(u8 *p, u32 v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
static void add(u32 a, u32 v) { ent[nent][0] = a, ent[nent][1] = v, nent++; }

static void base_list(void)   /* the stock bin's first entries, its TCON and PWM samples, its TGEN block */
{
    nent = 0;
    add(0x9c000018, 0x08000000), add(0x9c000094, 0x001f0019), add(0x9c000204, 0xf0001000);
    add(0x9c007880, 0x0000810e), add(0x9c007900, 0x00008020);
    add(0x9c407a04, 0x00000f01), add(0x9c407a08, 0x00000014);
    add(0x9c008000, 0x00003200);
    add(0x9c006a80, 3), add(0x9c006a90, 0x00130001), add(0x9c006aa0, 0x540), add(0x9c006aa4, 0x400);
    add(0x9c006aa8, 0x27b), add(0x9c006aac, 0x277), add(0x9c006ab0, 0x1e), add(0x9c006a84, 1);
    add(0x02000000, 0x83020100);   /* gamma bytes: stored, applied only with TCON+0x80 bit 0 */
    tail[0] = 0, tail[1] = 0, tail[2] = 0, tail[3] = 1;
}

static void build(u32 part_size_flags)
{
    memset(flash, 0xff, sizeof flash);
    u8 *b = flash + BIN_OFF, *pn = b + 12;
    u32 len = 12 + 12 + 8 * nent + 16;
    put32(b, 1), put32(b + 4, len), put32(b + 8, 12);   /* one section, at +12 */
    memcpy(pn, "PANEL\0\0\0", 8);
    put32(pn + 8, nent);
    for (u32 i = 0; i < nent; i++)
        put32(pn + 12 + 8 * i, ent[i][0]), put32(pn + 16 + 8 * i, ent[i][1]);
    memcpy(pn + 12 + 8 * nent, tail, 4);
    memcpy(pn + 16 + 8 * nent, "ratio\0\0\0", 8);
    put32(flash + DISP_PART_PANEL, 0x98000000u + BIN_OFF);
    put32(flash + DISP_PART_PANEL + 4, len | part_size_flags);
}

static int find(struct disp_panel *p, const char **why)
{
    build(0);
    return disp_panel_find(flash, FLASH_SZ, p, why);
}

static void parse(void)
{
    struct disp_panel p;
    const char *why;
    base_list();
    CHECK(find(&p, &why) == 0);
    CHECK(p.n == nent && p.htotal == 0x540 && p.hact == 0x400 && p.vtotal == 0x27b && p.vend == 0x277 &&
          p.vstart == 0x1e);
    CHECK(p.ent == flash + BIN_OFF + 24);

    /* refused before anything is written */
    base_list(), build(1u << 30);
    CHECK(disp_panel_find(flash, FLASH_SZ, &p, &why) == -1 && strstr(why, "compressed"));
    base_list(), tail[3] = 2;
    CHECK(find(&p, &why) == -1 && strstr(why, "type"));
    base_list(), tail[2] = 1;
    CHECK(find(&p, &why) == -1 && strstr(why, "two-display"));
    base_list(), add(0x80000000, 1);
    CHECK(find(&p, &why) == -1 && strstr(why, "entry"));
    base_list(), add(0x9c010000, 1);   /* just past the register window */
    CHECK(find(&p, &why) == -1);
    base_list(), add(0x9c000002, 1);   /* unaligned */
    CHECK(find(&p, &why) == -1);
    base_list(), add(0x02000100, 0);   /* gamma byte outside the 256-byte table */
    CHECK(find(&p, &why) == -1);
    base_list(), add(0x00200000, 2000000);   /* a 2 s delay */
    CHECK(find(&p, &why) == -1);
    base_list(), add(0x00800000, 0xdeadbeef);   /* U-Boot's "skip" kind: allowed, does nothing */
    CHECK(find(&p, &why) == 0);
    base_list(), add(0x9c006aa4, 800);
    CHECK(find(&p, &why) == -1 && strstr(why, "1024x600"));
    base_list(), add(0x9c006aac, 0x1e + 1 + 480);
    CHECK(find(&p, &why) == -1);
    base_list();
    ent[0][0] = 0x50414e45;   /* garbage in place of the list's first address is still checked */
    CHECK(find(&p, &why) == -1);
    base_list(), build(0);
    flash[BIN_OFF + 12] = 'X';
    CHECK(disp_panel_find(flash, FLASH_SZ, &p, &why) == -1 && strstr(why, "PANEL"));
    base_list(), build(0);
    put32(flash + BIN_OFF + 12 + 8, 0x7fff);   /* count far past the partition */
    CHECK(disp_panel_find(flash, FLASH_SZ, &p, &why) == -1);
    base_list(), build(0);
    put32(flash + DISP_PART_PANEL, 0x98000000u + FLASH_SZ - 0x10);   /* partition runs off the flash */
    CHECK(disp_panel_find(flash, FLASH_SZ, &p, &why) == -1);
}

static void sequence(void)
{
    struct disp_panel p;
    struct disp_state st;
    const char *why;
    base_list();
    CHECK(find(&p, &why) == 0);
    reset_regs();
    set(0x9c007a04, 0x10);                       /* OR entries keep other bits */
    set(0x9c0032e8, 0xc0000000u | 1u << 22);     /* GPIO62, 63, 54 owned by GPIO before: released (TCON bit 15) */
    set(0x9c1c7080, 0x00100000u | 0x12345);      /* bit 20 survives the field write */
    CHECK(disp_state(&p, &st) == 0 && !st.timing_ok);

    reset_regs();
    set(0x9c007a04, 0x10), set(0x9c0032e8, 0xc0000000u | 1u << 22), set(0x9c1c7080, 0x00100000u | 0x12345);
    disp_init(&p);

    /* 1. block 0x9C1C7000 first */
    CHECK(wlog[0].a == 0x9c1c7080 && wlog[0].v == 0x00160053);
    CHECK(wlog[1].a == 0x9c1c7084 && wlog[1].v == 0);
    CHECK(wlog[2].a == 0x9c1c7080 && wlog[2].v == 0x00160053);
    CHECK(wlog[4].a == 0x9c1c7088 && wlog[4].v == 0x00030050);
    CHECK(wlog[6].a == 0x9c1c7088 && wlog[6].v == 0x00130050);
    /* 2. the static lists, then the panel bin */
    CHECK(wlog[8].a == 0x9c000208 && wlog[8].v == 0x04600460);
    int dm = find_write(0x9c006c84, 0x8555, 0), osd = find_write(0x9c006580, 1, 0);
    int pan = find_write(0x9c000018, 0x08000000, 0);
    CHECK(dm > 8 && osd > dm && pan > osd);
    CHECK(find_write(0x9c007a04, 0xf11, (u32)pan) > pan);   /* 0x9C407A04 entry: OR into 0x9C007A04 */
    CHECK(host_rd(0x9c006aa0) == 0x540 && host_rd(0x9c006a84) == 1);
    /* 3. panel pins: 54..61 released, 62/63 released (TCON bit 15), 64..73, 9, 10 low */
    int pins = find_write(0x9c00030c, 0x00400000, (u32)pan);   /* GPIO54 MASTER off */
    CHECK(pins > pan && (host_rd(0x9c0032e8) & (1u << 22)) == 0);
    CHECK((host_rd(0x9c0032e8) & 0xc0000000u) == 0);
    CHECK(find_write(0x9c000340, 0x02000000, (u32)pins) > pins);   /* GPIO9 OUT = 0 */
    CHECK(find_write(0x9c000340, 0x04000000, (u32)pins) > pins);   /* GPIO10 */
    CHECK(find_write(0x9c000350, 0x00010000, (u32)pins) > pins);   /* GPIO64 */
    CHECK((host_rd(0x9c0032e4) & (1u << 9 | 1u << 10)) == (1u << 9 | 1u << 10));
    CHECK(find_write(0x9c000340, 0x08000000, 0) < 0);   /* GPIO11..16 only when 0x9C000094 asks */
    /* 4. no gamma without TCON+0x80 bit 0 */
    CHECK(writes_to(0x9c00796c) == 0);
    /* 5. GPOST0..2, OSD windows, mixer, TGEN second set */
    CHECK(host_rd(0x9c0064b8) == 1024 && host_rd(0x9c0064bc) == 600 && host_rd(0x9c006484) == 0x10);
    CHECK(host_rd(0x9c0065b8) == 1024 && host_rd(0x9c0064d4) == 0x40 && host_rd(0x9c0064f0) == 0x40);
    CHECK(host_rd(0x9c0064cc) == 0x30 && host_rd(0x9c0064e8) == 0x10);
    CHECK(host_rd(0x9c006244) == 1024 && host_rd(0x9c00624c) == 600 && host_rd(0x9c0062c4) == 1024);
    CHECK(host_rd(0x9c006204) == 0 && host_rd(0x9c006200) == 0x437);   /* OSD0 stays off */
    CHECK(host_rd(0x9c006c80) == 0x3070 && host_rd(0x9c006c88) == 0x6254);   /* the live values */
    CHECK(host_rd(0x9c006c84) == 0x8551);
    CHECK(host_rd(0x9c006adc) == 0x1000 && host_rd(0x9c006ae0) == 0xa && host_rd(0x9c006ae4) == 0xc0e);
    CHECK(host_rd(0x9c006b90) == 0x00130001 && host_rd(0x9c006ba0) == 0x540 && host_rd(0x9c006bb0) == 0x1e);
    CHECK(host_rd(0x9c006bdc) == 0x1700);
    /* 6. sub_f73628 last: GPIO86 high, DISP_PWM0 pinmux, PWM0 */
    int bl = find_write(0x9c000354, 0x00400040, 0);
    CHECK(bl > last_write(0x9c006bdc));
    CHECK(host_rd(0x9c000334) == 0x00400040 && host_rd(0x9c000314) == 0x00400040);
    CHECK(host_rd(0x9c0032ec) & (1u << 22));
    CHECK(host_rd(0x9c000090) == 0x000f0001);
    int d = last_write(0x9c007a08);
    CHECK(d > bl && wlog[d].w16 && wlog[d].v == 0x14);
    CHECK(host_rd(0x9c007a04) == 0xf11 && last_write(0x9c007a04) == (int)nlog - 1);

    CHECK(disp_state(&p, &st) == 1 && st.timing_ok);
    CHECK(disp_state(0, &st) == 0);
}

static void kinds(void)
{
    struct disp_panel p;
    const char *why;
    base_list();
    CHECK(find(&p, &why) == 0);
    reset_regs();
    disp_init(&p);
    u32 base = delay_us;

    base_list();
    add(0x00200000, 5000);   /* 5 ms */
    add(0x00200000, 300);    /* 300 us */
    add(0x00000c00, 0x3);    /* wait for 0x9C000C00 bits 0..1 set: already set */
    add(0x01000c04, 0x1);    /* wait for 0x9C000C04 bit 0 clear: never, so 1002 x 100 us */
    add(0x00800000, 0x1234); /* skip */
    CHECK(find(&p, &why) == 0);
    reset_regs();
    set(0x9c000c00, 0x7), set(0x9c000c04, 0x1);
    disp_init(&p);
    CHECK(delay_us - base == 5000 + 300 + 1002 * 100);
    CHECK(writes_to(0x9c000c00) == 0 && writes_to(0x9c000c04) == 0);

    /* gamma: TCON+0x80 bit 0 set by the list loads 256 x 2 entries, bytes sign-extended as U-Boot loads them */
    base_list();
    ent[4][1] = 0x8021;
    CHECK(find(&p, &why) == 0);
    reset_regs();
    disp_init(&p);
    CHECK(writes_to(0x9c00796c) == 512 && writes_to(0x9c007970) == 512);
    CHECK(find_write(0x9c00796c, 7, 0) >= 0 && find_write(0x9c007970, 0xfffff830u, 0) >= 0);   /* byte 3 = 0x83 */
    CHECK(find_write(0x9c007970, 0x20, 0) >= 0);                                              /* byte 2 = 0x02 */
    CHECK((host_rd(0x9c007900) & 0x803f) == 0x1);
}

int main(void)
{
    parse();
    sequence();
    kinds();
    if (fails)
        printf("disp_host: %d failed\n", fails);
    else
        printf("disp_host: ok\n");
    return fails != 0;
}
