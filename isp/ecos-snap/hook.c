/* Snapshot hook for the stock eCos (notes/vpp0_display.md, "eCos register snapshot").
 *
 * eCos's call of its per-vsync display dispatcher f_0050f284 (the sethi/ori pair at 0x5114ec) goes to entry.S at
 * 0x301000, which turns translation off and calls snap_hook, then enters the original dispatcher. Every call pets the
 * hardware watchdog the launcher armed (so a hang anywhere resets the unit within ~13 s), records the DDFCH state for
 * that frame, and takes full register snapshots at three call counts. After the last one it writes the D-cache back
 * and lets the watchdog reset the chip; DRAM at 0x06F00000.. survives, the card boots our payload again, and `hu.py read` collects it.
 *
 * Layout at SNAP (0x06F08000): struct hdr; per-frame history at +0x100 (NHIST entries of 4 words); snapshot k at
 * SNAP + 0x10000 * (k + 1), each a list of { addr, nwords, words... } ending with addr = 0. */
typedef unsigned int u32;
#define R(a) (*(volatile u32 *)(a))
#define SNAP 0x06F08000u
#define NHIST 2048u
#define MAGIC 0x50414e53u   /* "SNAP" */

struct hdr {
    u32 magic, ver, calls, done;
    u32 first_err_call, first_err_flags, first_ck_call, first_ck;
    u32 snap_call[4];
    u32 or_flags, max_ck_changes, last_flags, last_ck;
};

static const u32 sweep_skip[] = { 0x9c000280, 0x9c000500, 0x9c000c00, 0x9c000d80, 0x9c000e80 };   /* bus errors */

/* register blocks for every snapshot: { start, words } */
static const u32 blocks[][2] = {
    { 0x9c005780, 32 },   /* DDFCH */
    { 0x9c005a80, 64 },   /* G181 (commit block) and the matrix at 0x5B34 */
    { 0x9c005d80, 64 },   /* VSCL */
    { 0x9c006080, 32 },   /* VPOST */
    { 0x9c006a80, 32 },   /* TGEN0 */
    { 0x9c006c80, 32 },   /* DMIX0 */
    { 0x9c006f80, 32 },
    { 0x9c007000, 32 },
    { 0x9c009a00, 8 }, { 0x9c009b00, 8 }, { 0x9c009b80, 8 }, { 0x9c009c00, 8 },
    { 0x9c00a380, 32 },   /* BG2D control + first entries */
};

static u32 put_block(u32 *o, u32 a, u32 n)
{
    o[0] = a, o[1] = n;
    for (u32 i = 0; i < n; i++)
        o[2 + i] = R(a + 4 * i);
    return n + 2;
}

static void snapshot(int k, int sweep)
{
    struct hdr *h = (struct hdr *)SNAP;
    u32 *o = (u32 *)(SNAP + 0x10000u * (k + 1)), w = 0;
    h->snap_call[k] = h->calls;
    for (u32 b = 0; b < sizeof blocks / sizeof blocks[0]; b++)
        w += put_block(o + w, blocks[b][0], blocks[b][1]);
    if (sweep) {   /* uMCTL2 (registers and port 0), last snapshot only */
        w += put_block(o + w, 0x9c107000, 256);
        w += put_block(o + w, 0x9c107400, 16);
    }
    if (sweep)   /* RGST groups 0..198 (0x9C000000..0x9C00637F), as walked under U-Boot (dumps/rgst_9c000000.bin) */
        for (u32 g = 0x9c000000u; g < 0x9c006380u; g += 0x80) {
            int bad = 0;
            for (u32 i = 0; i < sizeof sweep_skip / sizeof sweep_skip[0]; i++)
                bad |= g == sweep_skip[i];
            if (!bad)
                w += put_block(o + w, g, 32);
        }
    o[w] = 0, o[w + 1] = 0;
}

static void dcache_wb_all(void)
{
    __asm__ volatile("cctl l1d_wball, alevel\n\tmsync all\n\tisb" ::: "memory");
}

static void reset_now(void)
{
    dcache_wb_all();
    R(0x9c000630) = 0x3877;       /* stop */
    R(0x9c000630) = 0xab00;       /* unlock */
    R(0x9c000634) = 0x200;        /* ~0.1 s */
    R(0x9c000630) = 0xab01;       /* lock */
    R(0x9c000630) = 0x7482;       /* clear a latched expiry */
    R(0x9c0000f4) = 0x00120012;   /* route to system reset */
    R(0x9c000630) = 0x4a4b;       /* resume */
    for (;;)
        ;
}

__attribute__((section(".text.entry"))) void snap_hook(void)
{
    struct hdr *h = (struct hdr *)SNAP;
    if (h->magic != MAGIC || h->ver != 1) {   /* first call: the loader zeroes the area, but be safe */
        u32 *z = (u32 *)SNAP;
        for (u32 i = 0; i < 0x100 / 4 + NHIST * 4; i++)
            z[i] = 0;
        h->magic = MAGIC, h->ver = 1;
    }
    u32 c = h->calls++;
    u32 fl = R(0x9c0057ec), ck = R(0x9c0057e8);
    if (c < NHIST) {
        u32 *e = (u32 *)(SNAP + 0x100) + 4 * c;
        e[0] = fl, e[1] = ck, e[2] = R(0x9c005788), e[3] = R(0x9c005e58) << 16 | (R(0x9c006c9c) & 0xffff);
    }
    if (fl && !h->first_err_call)
        h->first_err_call = c + 1, h->first_err_flags = fl;
    if (ck && !h->first_ck_call)
        h->first_ck_call = c + 1, h->first_ck = ck;
    if (ck != h->last_ck)
        h->max_ck_changes++;
    h->or_flags |= fl, h->last_flags = fl, h->last_ck = ck;
    if (c == 120)
        snapshot(0, 0);   /* ~2 s: right after eCos's display init */
    else if (c == 600)
        snapshot(1, 0);   /* ~10 s */
    else if (c == 1500)
        snapshot(2, 1);   /* ~25 s, with the RGST sweep */
    else if (c == 1560) {
        h->done = 1;
        reset_now();
    }
    if (!(c & 63))
        dcache_wb_all();
    R(0x9c000630) = 0xab00;   /* pet the watchdog (launch.c armed it, ~13 s) */
    R(0x9c000634) = 0xffff;
    R(0x9c000630) = 0xab01;
}
