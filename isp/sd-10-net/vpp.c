/* VPP0 video layer bring-up (notes/vpp0_display.md), HU1 op 24, net task only. Every change to the display
 * registers is undone by vpp_restore (the saved blocks written back), automatically after the requested seconds. */
#include "vpp.h"
#include "vdec.h"

typedef unsigned int u32;
#define R(a) (*(volatile u32 *)(a))

static const struct { u32 base, n; } blocks[] = {
    { 0x9c005780, 32 },   /* DDFCH */
    { 0x9c005a80, 1 },
    { 0x9c005d80, 48 },   /* VSCL */
    { 0x9c006c80, 4 },    /* DMIX */
    { 0x9c00a380, 1 },    /* BG2D pitch slots */
    { 0x9c006080, 12 },   /* post blocks hal_vpp_init touches (purpose not decoded) */
    { 0x9c006f80, 19 },
    { 0x9c007000, 2 },
    { 0x9c009a00, 2 },
    { 0x9c009b00, 1 },
    { 0x9c009b80, 1 },
    { 0x9c009c00, 1 },
    { 0x9c005b34, 8 },
    { 0x9c006a80, 1 },    /* TGEN config (pokes) */
    { 0x9c006af4, 1 },    /* TGEN0 source select (probe) */
    { 0x9c006adc, 1 },    /* TGEN0 adjust1 (probe) */
};
static u32 saved[32 + 1 + 48 + 4 + 1 + 12 + 19 + 2 + 2 + 1 + 1 + 1 + 8 + 1 + 1 + 1];
static int have_saved, active;

static void save(void)
{
    if (have_saved)
        return;
    int k = 0;
    for (unsigned b = 0; b < sizeof blocks / sizeof blocks[0]; b++)
        for (u32 i = 0; i < blocks[b].n; i++)
            saved[k++] = R(blocks[b].base + 4 * i);
    have_saved = 1;
}

void vpp_restore(void)
{
    if (!have_saved)
        return;
    int k = 0;
    for (unsigned b = 0; b < sizeof blocks / sizeof blocks[0]; b++)
        for (u32 i = 0; i < blocks[b].n; i++)
            R(blocks[b].base + 4 * i) = saved[k++];
    R(0x9c005a80) |= 1;   /* commit */
    active = 0;
}

int vpp_active(void) { return active; }

static void setbits(u32 a, u32 mask, u32 v) { R(a) = (R(a) & ~mask) | (v & mask); }

/* hal_vpp_init's DDFCH and scaler parts (0x529d66), then 800x480 -> 1024x600 */
static void init_post(void)   /* hal_vpp_init items before the scaler (0x529d7a..0x529f92), arg0 = arg1 = 0 */
{
    setbits(0x9c006080, 0x3f, 1u << 0 | 1u << 4);
    setbits(0x9c0060a4, 3, 0);
    R(0x9c0060a8) = 0x10;
    *(volatile unsigned char *)0x9c0060ac = 0x80;
    *(volatile unsigned char *)0x9c0060ad = 0x80;
    R(0x9c006f80) = 1;
    R(0x9c006f84) = 0;
    R(0x9c006fc8) = 0x7f;
    setbits(0x9c007000, 1u << 0 | 1u << 12, 1u << 0);
    R(0x9c007004) = 0x3ff;
    setbits(0x9c009a00, 7, 3);
    setbits(0x9c009a04, 1u << 8 | 1u, 0);
    setbits(0x9c009b00, 0x15, 0x11);
    setbits(0x9c009b80, 0x31, 0x21);
    setbits(0x9c009c00, 5, 5);
    setbits(0x9c005a80, 0x3f43, 0x103);
    static const u32 m[] = { 0x1807, 0x6f0, 0x8fe, 0x4555, 0xa, 0x5014, 0xff0c, 3 };
    for (int i = 0; i < 8; i++)
        R(0x9c005b34 + 4 * i) = m[i];
}

static u32 fl[8];   /* DDFCH error flags (0x7EC) after each setup stage, for bisecting */
#define FL(i) (fl[i] = R(0x9c0057ec))
static void init_fetch(u32 w, u32 h, u32 pitch)
{
    vdec_bg2d_ensure();   /* DDFCH locks up on its first fetch without the BG2D init (notes/vpp0_display.md, root cause) */
    FL(0);
    init_post();
    FL(1);
    R(0x9c005d80) = 1;
    setbits(0x9c005d84, 1u << 4 | 1u << 6 | 1u << 9 | 1u << 12, 1u << 4 | 1u << 6);
    FL(2);
    R(0x9c005780) = 1;
    R(0x9c005784) = 0x5e000;
    R(0x9c00578c) = 0xd6d6d6;
    R(0x9c005790) = 0x3def;
    setbits(0x9c0057dc, 1u << 0 | 1u << 4 | 1u << 5 | 3u << 8 | 3u << 16 | 1u << 24 | 3u << 28, 1u << 0 | 1u << 4 | 1u << 16);
    setbits(0x9c0057bc, 0x3f, 1u << 2 | 1u << 4);
    setbits(0x9c0057e4, 0x7f, 1u << 4);
    setbits(0x9c005788, 0xff, 0xe0);
    R(0x9c0057b0) |= 0x10001, R(0x9c0057b4) |= 0x10001, R(0x9c0057b8) |= 0x10001;
    R(0x9c0057d0) = pitch;
    R(0x9c0057d8) = 0;
    R(0x9c0057d4) = h << 16 | w;
    FL(3);
    /* scaler: factor ((in - 1) << 22) / (out - 1); filter index 3 / 2 for upscaling (0x529b06) */
    u32 hf = ((w - 1) << 22) / (1024 - 1), vf = ((h - 1) << 22) / (600 - 1);
    R(0x9c005d8c) = w, R(0x9c005d90) = h;            /* input w/h (+0x0C/+0x10) */
    R(0x9c005d94) = 0, R(0x9c005d98) = 0;            /* crop x/y */
    R(0x9c005d9c) = w, R(0x9c005da0) = h;            /* crop w/h */
    R(0x9c005da4) = 1024, R(0x9c005da8) = 600;       /* panel */
    R(0x9c005dac) = 0, R(0x9c005db0) = 0;            /* output x/y */
    R(0x9c005db4) = 1024, R(0x9c005db8) = 600;       /* output w/h */
    R(0x9c005dcc) = hf & 0xffff, R(0x9c005dd0) = (hf >> 16) & 0x1ff;
    R(0x9c005e04) = vf & 0xffff, R(0x9c005e08) = (vf >> 16) & 0x1ff;
    setbits(0x9c005dc8, 0xf0, 3u << 4);
    setbits(0x9c005e00, 0xf0, 2u << 4);
    setbits(0x9c005d84, 0x1f, 1u << 0 | 1u << 1 | (w != 1024) << 2 | (h != 600) << 3 | (h != 600) << 4);
    FL(4);
}

/* probe (c == 9): wait ~0.2 s, then the checksums of the three stages: DDFCH 0x7E8 (and its error flags 0x7EC: b5 incomplete
 * frame, b8 luma line buffer empty, b9/b10 burst count error MA0/MA1, [13:12]/[17:16] AXI read response MA0/MA1, 2 = SLVERR,
 * 3 = DECERR, per the Sunplus SP7021 display chapter), the DDFCH AXI debug info 0x7F4, the DDFCH<->downstream handshake
 * 0x7FC (b0 downstream request, b1 DDFCH valid), VSCL 0x9C005E58 (i143 G188.22), DMIX 0x9C006C9C (SP7021 G217.07). */
static int probe_sample(char *o, vpp_fmt fmt, const char *name)
{
    for (volatile int i = 0; i < 4000000; i++)
        ;
    return fmt(o, " [%s ddfch %x/%x axi %x hs %x vscl %x dmix %x]", name, R(0x9c0057e8), R(0x9c0057ec), R(0x9c0057f4),
               R(0x9c0057fc) & 3, R(0x9c005e58), R(0x9c006c9c));
}

/* one summary line per TGEN0 frame for `frames` frames: OR of the DDFCH error flags, last DDFCH / VSCL / DMIX checksums */
static int trace_frames(char *o, vpp_fmt fmt, u32 frames)
{
    u32 pl = R(0x9c006ab4) & 0xfff, frame = 0, orf = 0, it = 0;
    int k = 0;
    while (frame < frames && ++it < 40000000u) {
        u32 ln = R(0x9c006ab4) & 0xfff;
        orf |= R(0x9c0057ec);
        if (ln < pl) {
            frame++;
            k += fmt(o + k, " F%u or %x ck %x v %x d %x;", frame, orf, R(0x9c0057e8), R(0x9c005e58), R(0x9c006c9c));
            orf = 0;
        }
        pl = ln;
    }
    return k;
}

int vpp_op(const unsigned char *p, int n, char *o, vpp_fmt fmt, u32 luma_idx, u32 chroma_idx)
{
    int c = n ? p[0] : 0;
    if (c == 1 || c == 2) {   /* 1: color bar from the DDFCH pattern generator; 2: the decoder's picture (index mode) */
        save();
        init_fetch(800, 480, 1920);
        if (c == 1) {
            R(0x9c0057f0) = 0x80801011;
        } else {
            R(0x9c0057f0) &= ~1u;
            /* eCos's display path (0x5270de): the frame's BG2D entries get gp = 2 (the decoder writes gp = 1), progressive
             * scan bits; then the mode-0xB handoff bits: 0x784 b8 (index mode), D84 b9, DC[17:16] = 1, A80 b8 */
            /* p[4] variant: 0 index mode with eCos's gp-2 rewrite of the frame's BG2D entries, 1 index mode on the
             * decoder's own entries (gp 1), 2 physical addresses of the slot (tiled data: scrambled, but shows the fetch) */
            int var = n >= 5 ? p[4] : 0;
            for (int e = 0; e < 2 && var == 0; e++) {
                u32 a2 = 0x9c00a400 + 4 * (e ? chroma_idx : luma_idx);
                R(a2) = (R(a2) & ~(3u << 19)) | 2u << 19;
            }
            if (var == 2)
                setbits(0x9c005784, 1u << 28 | 1u << 20 | 1u << 21 | 1u << 8, 0);
            else
                setbits(0x9c005784, 1u << 28 | 1u << 20 | 1u << 21 | 1u << 8, 1u << 20 | 1u << 8);
            setbits(0x9c005d84, 1u << 9, 0);   /* mode 0xB case 0x52a744: r5 = 0 -> D84 b9 = 0 (round 3 correction) */
            /* config_0: b4 "il c" 0 = frame, b5 "fid c" 0 = top (Sunplus doc); the 0xB case clears both, hal_vpp_init had set b4 */
            setbits(0x9c0057dc, 3u << 16 | 1u << 5 | 1u << 4 | 1u, 1u << 16 | 1u);
            setbits(0x9c005a80, 1u << 8 | 1u << 9 | 1u << 12, 1u << 8);
            setbits(0x9c005788, 0x1c, 0x10);
            u32 ly = luma_idx, lc = chroma_idx;
            if (var == 2)   /* the slot's physical addresses from its BG2D entries (address field = phys >> 10) */
                ly = R(0x9c00a400 + 4 * luma_idx) & 0x7ffff, lc = R(0x9c00a400 + 4 * chroma_idx) & 0x7ffff;
            R(0x9c005798) = ly, R(0x9c00579c) = ly, R(0x9c0057a0) = ly;
            R(0x9c0057a4) = lc, R(0x9c0057a8) = lc, R(0x9c0057ac) = lc;
        }
        /* DMIX layer mode, 2 bits per layer at field (layer - 1): 0 blend (plane alpha), 1 transparent (U-Boot's
         * setting for VPP0 on L1), 2 opaque, as the SP7021 driver; default field 0, mode 2 */
        u32 fld = n >= 3 ? p[2] : 0, mode = n >= 4 ? p[3] : 2;
        setbits(0x9c006c84, 3u << (2 * fld), mode << (2 * fld));
        R(0x9c005a80) |= 1;   /* commit */
        active = 1;
    } else if (c == 3) {
        vpp_restore();
    } else if (c == 6) {   /* linear NV12 color bars at 0x04E00000, DDFCH as the SP7021 driver (ddfch_setting) */
        const u32 w = 800, h = 480, pitch = 896, Y = n >= 5 && p[4] ? 0x03700000u : 0x00e00000u, C = Y + pitch * h;
        static const unsigned char bars[8][3] = { { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
                                                  { 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 } };
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                *(volatile unsigned char *)(Y + y * pitch + x) = bars[x / 100][0];
        for (u32 y = 0; y < h / 2; y++)
            for (u32 x = 0; x < w; x += 2) {
                *(volatile unsigned char *)(C + y * pitch + x) = bars[x / 100][1];
                *(volatile unsigned char *)(C + y * pitch + x + 1) = bars[x / 100][2];
            }
        for (u32 a2 = Y & ~31u; a2 < C + pitch * h / 2; a2 += 32)
            __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(a2) : "memory");
        __asm__ volatile("msync all" ::: "memory");
        save();
        init_fetch(w, h, pitch);
        R(0x9c005780) = 1;                       /* latch_en */
        R(0x9c005784) = n >= 3 && p[2] ? 0x5e000 : 0;   /* mode_option: 0 = NV12 (SP7021); eCos 0x5E000 */
        R(0x9c005788) = n >= 4 && p[3] ? p[3] : 0xd0;    /* ddfch_enable */
        R(0x9c005798) = R(0x9c00579c) = R(0x9c0057a0) = Y >> 10;
        R(0x9c0057a4) = R(0x9c0057a8) = R(0x9c0057ac) = C >> 10;
        R(0x9c0057d0) = pitch;
        R(0x9c0057d8) = 0;
        R(0x9c0057dc) = 0x10000;                 /* config_0 */
        R(0x9c0057f0) = 0x80801002;              /* bist off */
        R(0x9c0057d4) = h << 16 | w;
        FL(5);
        setbits(0x9c006c84, 3u, 2u);             /* VPP0 opaque */
        R(0x9c005a80) |= 1;
        FL(6);
        active = 1;
        u32 c0 = R(0x9c0057e8), e0 = R(0x9c0057ec);
        for (volatile int i = 0; i < 3000000; i++)
            ;
        FL(7);
        fmt(o, "flags by stage: start %x post %x vscl-on %x ddfch %x scaler %x sp7021-ddfch %x mix+commit %x later %x; ",
            fl[0], fl[1], fl[2], fl[3], fl[4], fl[5], fl[6], fl[7]);
        int k0 = 0;
        while (o[k0])
            k0++;
        return k0 + fmt(o + k0, "vpp linear: checksum %x -> %x, error flags %x -> %x, 7f4 %x 7f8 %x 7fc %x, 7bc %x 7c0 %x", c0,
                   R(0x9c0057e8), e0, R(0x9c0057ec), R(0x9c0057f4), R(0x9c0057f8), R(0x9c0057fc), R(0x9c0057bc), R(0x9c0057c0));
    } else if (c == 11) {   /* fetch test (after a full reboot: a DDFCH AXI error is sticky): eCos init, then valid physical
                              * addresses in all three slots BEFORE fetch enable, then fetch on, commit, and samples at
                              * ~50 ms, ~0.5 s and ~2 s. p[1] bit 0: BIST off (real data); p[2]: 0 decoder slot 0 (luma
                              * 0x04600000, CbCr +0x180000), 1 the same with CbCr = luma (one master port's region only).
                              * No restore: `vpp restore` afterwards. */
        int real = n >= 2 && (p[1] & 1), where = n >= 3 ? p[2] : 0;
        u32 Y = where == 2 ? 0x03700000u : 0x00600000u, C = where == 1 ? Y : where == 2 ? 0x03800000u : Y + 0x180000u;
        int full = n >= 4 && p[3];   /* p[3]: 1 = 1024 x 600, no scaling */
        save();
        init_fetch(full ? 1024 : 800, full ? 600 : 480, 1920);
        setbits(0x9c005784, 1u << 28 | 1u << 20, 0);   /* physical addresses */
        R(0x9c005798) = R(0x9c00579c) = R(0x9c0057a0) = Y >> 10;
        R(0x9c0057a4) = R(0x9c0057a8) = R(0x9c0057ac) = C >> 10;
        R(0x9c0057f0) = real ? 0x80801002 : 0x80801011;
        setbits(0x9c006c84, 0xfu, 2u | 1u << 2);   /* VPP0 opaque, OSD0 transparent */
        int k = fmt(o, "fetch test (%s, CbCr %x): before enable flags %x;", real ? "slot 0 data" : "BIST bars", C, R(0x9c0057ec));
        setbits(0x9c005788, 0x1c, 0x10);
        R(0x9c005a80) |= 1;
        static const int waits[] = { 1000000, 9000000, 30000000 };
        for (int i = 0; i < 3; i++) {
            for (volatile int j = 0; j < waits[i]; j++)
                ;
            k += fmt(o + k, " [t%d ddfch %x/%x axi %x hs %x vscl %x dmix %x]", i, R(0x9c0057e8), R(0x9c0057ec), R(0x9c0057f4),
                     R(0x9c0057fc) & 3, R(0x9c005e58), R(0x9c006c9c));
        }
        return k;
    } else if (c == 12 && n >= 5) {   /* fresh test, one per full reset (a DDFCH AXI error is sticky). Refuses to run when the
                                        * error flags are already set. p[1] flags: b0 real data (BIST off), b1 frame mode (0x7DC
                                        * b4 = b5 = 0, one-frame BIST), b2 index mode (BG2D entries 0x40/0x41, gp 2, need `vdec
                                        * init` first), b3 no fetch enable (BIST alone), b4 direct mode (0x780 = 0), b5 1024x600
                                        * unscaled, b6 fetch enable before the addresses. p[2] where: 0 Y 0x04600000 / C +1.5 MB,
                                        * 1 C = Y, 2 Y 0x03700000 / C 0x03800000. p[3..4] 0x7D0 (0 = 1920). Then pokes of 12 bytes
                                        * (addr, mask, value, LE), applied before the fetch enable. After the commit, a trace of
                                        * TGEN0 line / frame against every change of the DDFCH flags and checksum, VSCL and DMIX
                                        * checksums, 0x7F4 and 0x7FC, for 6 frames. No restore. */
        u32 fl = p[1], where = p[2] & 3, act = p[2] & 0xf0, pitch = p[3] | (u32)p[4] << 8;
        if (R(0x9c0057ec))
            return fmt(o, "ERR fresh: DDFCH flags already %x, reset first (hu.py irq hwwdt)", R(0x9c0057ec));
        int full = fl & 32;
        u32 w = full ? 1024 : 800, h = full ? 600 : 480;
        /* where 3: below 64 MB in eCos's image area (unused by the payload). The unit has 64 MB of DRAM; addresses from
         * 0x04000000 up alias the low 64 MB for the CPU (2026-11-01), so 0x04600000 is not real memory for an AXI master. */
        u32 Y = where == 3 ? 0x00400000u : where == 2 ? 0x03700000u : 0x00600000u,
            C = where == 3 ? 0x00500000u : where == 1 ? Y : where == 2 ? 0x03800000u : Y + 0x180000u;
        save();
        init_fetch(w, h, pitch ? pitch : 1920);
        if (fl & 2)
            setbits(0x9c0057dc, 3u << 4, 0);
        if (fl & 16)
            R(0x9c005780) = 0;
        if (fl & 64)
            setbits(0x9c005788, 0x1c, 0x10);
        u32 ly = Y >> 10, lc = C >> 10;
        if (fl & 4) {   /* eCos's display entries: address >> 10, gp 2 (pitch slot 2), progressive scan bits */
            R(0x9c00a400 + 4 * 0x40) = ly | 2u << 19 | 1u << 22 | 1u << 24 | 2u << 25;
            R(0x9c00a400 + 4 * 0x41) = lc | 2u << 19 | 1u << 22 | 1u << 24 | 2u << 25;
            setbits(0x9c005784, 1u << 28 | 1u << 20 | 1u << 21 | 1u << 8, 1u << 20 | 1u << 8);
            ly = 0x40, lc = 0x41;
        } else {
            setbits(0x9c005784, 1u << 28 | 1u << 20, 0);
        }
        R(0x9c005798) = R(0x9c00579c) = R(0x9c0057a0) = ly;
        R(0x9c0057a4) = R(0x9c0057a8) = R(0x9c0057ac) = lc;
        R(0x9c0057f0) = (fl & 1) ? 0x80801002 : (fl & 2) ? 0x80801013 : 0x80801011;
        setbits(0x9c006c84, 0xfu, 2u | 1u << 2);   /* VPP0 opaque, OSD0 transparent */
        int np = 0;
        for (int i = 5; i + 12 <= n && np < 16; i += 12, np++) {
            u32 a = p[i] | (u32)p[i + 1] << 8 | (u32)p[i + 2] << 16 | (u32)p[i + 3] << 24;
            u32 m = p[i + 4] | (u32)p[i + 5] << 8 | (u32)p[i + 6] << 16 | (u32)p[i + 7] << 24;
            u32 v = p[i + 8] | (u32)p[i + 9] << 8 | (u32)p[i + 10] << 16 | (u32)p[i + 11] << 24;
            if ((a & 3) || !((a >= 0x9c000000u && a < 0x9c010000u) || (a >= 0x9c107000u && a < 0x9c107400u)))
                return fmt(o, "ERR fresh: poke %x outside 0x9C000000..0x9C00FFFF / uMCTL2 0x9C107000..0x9C1073FF", a);
            setbits(a, m, v);
        }
        int k = fmt(o, "fresh fl %x Y %x C %x %ux%u pitch %u pokes %d: 784 %x 788 %x 7dc %x 7f0 %x, before flags %x;", fl, Y, C, w,
                    h, R(0x9c0057d0), np, R(0x9c005784), R(0x9c005788), R(0x9c0057dc), R(0x9c0057f0), R(0x9c0057ec));
        if (!(fl & 8) && !(fl & 64))
            setbits(0x9c005788, 0x1c, 0x10);
        R(0x9c005a80) |= 1;
        /* trace: an event whenever a watched value changes or TGEN0's line counter wraps (new frame) */
        u32 prev[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u }, pl = R(0x9c006ab4) & 0xfff, frame = 0, it = 0;
        int ev = 0;
        int um = act & 0x80;   /* uMCTL2 POISONSTAT (b16: a read was answered with an error), PSTAT, POISONCFG (opt-in) */
        u32 ps0 = um ? R(0x9c107370) : 0;
        if (um)
            k += fmt(o + k, " umctl poisonstat %x pstat %x poisoncfg %x;", ps0, R(0x9c1073fc), R(0x9c10736c));
        k += fmt(o + k, " start line %u;", pl);
        u32 orf = 0, nchg = 0, nfr = (fl & 128) ? 12 : 6;   /* b7: summary, one line per frame (OR of flags, changes seen) */
        while (frame < nfr && ++it < 40000000u && ev < 48) {
            u32 ln = R(0x9c006ab4) & 0xfff;
            u32 cur[6] = { R(0x9c0057ec), R(0x9c0057e8), R(0x9c005e58), R(0x9c006c9c), R(0x9c0057f4), R(0x9c0057fc) & 3 };
            int wrap = ln < pl, chg = 0;
            pl = ln;
            if (wrap && ++frame == 3 && (act & 0x70)) {   /* frame 3 starts: try to restart after the first-fetch error */
                if (act & 0x10)
                    R(0x9c0057ec) = 0x00011720;   /* W1C b5 b8 b9 b10; response fields [13:12]/[17:16] clear with 1 */
                if (act & 0x20) {   /* eCos's stop/start (0x52774c / 0x5276d8): VSCL and the 6F80/7000 block off, commit, on */
                    R(0x9c005d80) = 0, setbits(0x9c007000, 1, 0), R(0x9c006f80) = 0, R(0x9c005a80) |= 1;
                    for (volatile int j = 0; j < 20000; j++)
                        ;
                    R(0x9c005d80) = 1, setbits(0x9c007000, 1, 1), R(0x9c006f80) = 1, R(0x9c005a80) |= 1;
                }
                if (act & 0x40) {   /* fetch enable off for a frame's worth of polling, clear, back on */
                    setbits(0x9c005788, 0x10, 0), R(0x9c005a80) |= 1;
                    for (u32 l0 = R(0x9c006ab4) & 0xfff, j = 0; j < 5000000 && (R(0x9c006ab4) & 0xfff) >= l0; j++)
                        ;
                    R(0x9c0057ec) = 0x00011720;
                    setbits(0x9c005788, 0x10, 0x10), R(0x9c005a80) |= 1;
                }
                k += fmt(o + k, " [act %x at F3: flags now %x]", act, R(0x9c0057ec));
            }
            for (int i = 0; i < 6; i++)
                chg |= cur[i] != prev[i], prev[i] = cur[i];
            orf |= cur[0], nchg += chg;
            if ((fl & 128) && wrap) {
                k += fmt(o + k, " F%u or %x chg %u ck %x v %x d %x;", frame, orf, nchg, cur[1], cur[2], cur[3]);
                orf = 0, nchg = 0, ev++;
            } else if (!(fl & 128) && (chg || wrap)) {
                k += fmt(o + k, " %s%u.%u fl %x ck %x v %x d %x ax %x hs %x;", wrap ? "F" : "", frame, ln, cur[0], cur[1], cur[2], cur[3],
                         cur[4], cur[5]);
                ev++;
            }
        }
        k += fmt(o + k, " end it %u frames %u", it, frame);
        if (um)
            k += fmt(o + k, "; umctl poisonstat %x -> %x pstat %x", ps0, R(0x9c107370), R(0x9c1073fc));
        return k;
    } else if (c == 13) {   /* safe: fetch off, BIST off, VPP0 transparent in DMIX, commit (run before a reset) */
        setbits(0x9c005788, 0x1c, 0);
        R(0x9c0057f0) &= ~1u;
        setbits(0x9c006c84, 3, 1);
        R(0x9c005a80) |= 1;
        return fmt(o, "vpp safe: 788 %x 7f0 %x c84 %x flags %x", R(0x9c005788), R(0x9c0057f0), R(0x9c006c84), R(0x9c0057ec));
    } else if (c == 14 && n >= 7) {   /* replay (one per full reset): p[1..4] address of a list written beforehand with
                                        * hu.py write, p[5..6] entry count; each entry { addr, value } is written in order,
                                        * then a 12-frame summary trace. tools/ecos_snap.py builds the list from a register
                                        * snapshot taken under the stock eCos. No restore. */
        u32 la = p[1] | (u32)p[2] << 8 | (u32)p[3] << 16 | (u32)p[4] << 24, cnt = p[5] | (u32)p[6] << 8;
        if (R(0x9c0057ec))
            return fmt(o, "ERR replay: DDFCH flags already %x, reset first (hu.py irq hwwdt)", R(0x9c0057ec));
        if ((la & 3) || la < 0x00300000u || la + 8 * cnt > 0x00700000u || cnt > 2048)
            return fmt(o, "ERR replay: list must lie in 0x00300000..0x00700000, <= 2048 entries");
        save();
        const volatile u32 *l = (const volatile u32 *)la;
        u32 bad = 0;
        for (u32 i = 0; i < cnt; i++) {
            u32 a = l[2 * i], v = l[2 * i + 1];
            if ((a & 3) || a < 0x9c000000u || a >= 0x9c010000u) {
                bad++;
                continue;
            }
            R(a) = v;
        }
        int k = fmt(o, "replay %u writes (%u refused): 784 %x 788 %x 7dc %x 7f0 %x a80 %x d84 %x c84 %x, flags %x;", cnt, bad,
                    R(0x9c005784), R(0x9c005788), R(0x9c0057dc), R(0x9c0057f0), R(0x9c005a80), R(0x9c005d84), R(0x9c006c84),
                    R(0x9c0057ec));
        R(0x9c005a80) |= 1;
        return k + trace_frames(o + k, fmt, 12);
    } else if (c == 10 && n >= 7) {   /* bit scan: p[1..4] register (LE), p[5] first bit, p[6] count. After the probe's step 1
                                        * (eCos init, DDFCH color bar, fetch on, VPP0 opaque), flip each bit alone for ~3 frames
                                        * and report every bit after which a stage shows data. `vpp restore` afterwards. */
        u32 reg = p[1] | (u32)p[2] << 8 | (u32)p[3] << 16 | (u32)p[4] << 24, b0 = p[5], nb = p[6];
        int ok = (reg >= 0x9c005780 && reg < 0x9c005800) || (reg >= 0x9c005a80 && reg < 0x9c005b80) ||
                 (reg >= 0x9c005d80 && reg < 0x9c005e80) || (reg >= 0x9c006a80 && reg < 0x9c006b00) ||
                 (reg >= 0x9c006c80 && reg < 0x9c006d00) || (reg >= 0x9c006080 && reg < 0x9c006100);
        if (!ok || (reg & 3) || b0 + nb > 32)
            return fmt(o, "ERR bitscan: register outside the scanned blocks, or bits past 31");
        save();
        init_fetch(800, 480, 1920);
        R(0x9c0057f0) = 0x80801011;
        setbits(0x9c005788, 0x1c, 0x10);
        setbits(0x9c006c84, 0xfu, 2u | 1u << 2);
        R(0x9c005a80) |= 1;
        for (volatile int i = 0; i < 1000000; i++)
            ;
        int k = fmt(o, "bitscan %x bits %u..%u: base %x, idle ddfch %x axi %x hs %x vscl %x;", reg, b0, b0 + nb - 1, R(reg),
                    R(0x9c0057e8), R(0x9c0057f4), R(0x9c0057fc) & 3, R(0x9c005e58));
        int hits = 0;
        for (u32 b = b0; b < b0 + nb; b++) {
            u32 v = R(reg);
            R(reg) = v ^ (1u << b);
            R(0x9c005a80) |= 1;
            for (volatile int i = 0; i < 1000000; i++)
                ;
            u32 d = R(0x9c0057e8), ax = R(0x9c0057f4), hs = R(0x9c0057fc) & 3, vs = R(0x9c005e58), dm = R(0x9c006c9c);
            R(reg) = v;
            R(0x9c005a80) |= 1;
            if (d || ax || hs || vs)
                k += fmt(o + k, " b%u: ddfch %x axi %x hs %x vscl %x dmix %x;", b, d, ax, hs, vs, dm), hits++;
        }
        return k + fmt(o + k, " %d hits", hits);
    } else if (c == 9 && n >= 2) {   /* probe (static analysis rounds 2 and 3): where does VPP0's data path break? Samples the DDFCH
                                      * checksum (0x7E8), VSCL checksum (0x9C005E58, i143 G188.22) and DMIX checksum (0x9C006C9C)
                                      * after each step; a nonzero value past a stage means that stage has data. p[1..2] mask (LE16), p[3] path:
                                      *  1 eCos-style init (init_fetch) + DDFCH color bar, VPP0 opaque, OSD0 transparent (DMIX only,
                                      *    the console stays on in software), commit
                                      *  2 TGEN0 source select 0x9C006AF4 field 0 (VPP0) <- 0 (eCos 0x52e5fc, SP7021 tgen_source_sel)
                                      *  4 VSCL path select 0x9C005D84 [13:12] <- p[3] (i143: 0 VPPDMA, 1 OSD0, 2 DDFCH)
                                      *  8 VSCL color bar, 0x9C005D84 b7 (eCos pattern 0xE, i143 0x80)
                                      * 16 DDFCH latch_en off (0x9C005780 = 0) and the BIST register rewritten
                                      * 32 TGEN0 latch mode (0x9C006A80 = 7, SP7021 DRV_TGEN_Init)
                                      * Round 3 (the scaler's own pattern reaches the mixer, nothing enters the scaler):
                                      * 64 commit 0x9C005A80 b0 2000 times about one line apart (3 frames: every phase of the frame)
                                      * 128 handoff written inside vblank: wait for TGEN0's line counter (+0x34 [11:0]) to pass the
                                      *     field end line (+0x2C), then BIST on, fetch enable, commit
                                      * 256 physical fetch, BIST off, from the OSD video buffer 0x07A00000 (a region OSD0 reads fine)
                                      * 512 physical fetch, BIST off, from the decoder's slot 0 at 0x04600000 (tiled, pitch 1920)
                                      * 1024 mode_option 0x784 = 0xF000 (the value found under U-Boot) then 0x5F000
                                      * 2048 TGEN0 adjust1 0x9C006ADC = 0x100D (i143 DRV_TGEN_Init: [5:0] = 0xD added to U-Boot's 0x10)
                                      * 4096 both DDFCH AXI masters on MA0: 0x784 = 0x1E000 (SP7021 reset value; eCos 0x5E000 = chroma on MA1)
                                      * 8192 both on MA1: 0x784 = 0x7E000
                                      * 16384 DDFCH internal clocks free-running: 0x7C0 = 0x79 (doc: 0 = gated for power saving)
                                      * 32768 frame mode as eCos's progressive handoff: 0x7DC b4 (il c: field) and b5 (fid c) cleared,
                                      *     0x788 b3 = 0 b4 = 1, D84 b9 = 0, BIST 0x80801013 (bit 1: one frame, as SP7021's color bar)
                                      * No auto-restore (the reply must beat hu.py's 2 s timeout): `hu.py vpp restore` afterwards. */
        u32 m = p[1] | (n >= 3 ? (u32)p[2] << 8 : 0), path = n >= 4 ? p[3] & 3 : 2;   /* mask is 16 bits (LE) */
        save();
        int k = fmt(o, "vpp probe %x: before AF4=%x D84=%x C80=%x C84=%x;", m, R(0x9c006af4), R(0x9c005d84), R(0x9c006c80),
                    R(0x9c006c84));
#define SAMPLE(name) (k += probe_sample(o + k, fmt, name))
        if (m & 1) {
            init_fetch(800, 480, 1920);
            R(0x9c0057f0) = 0x80801011;
            setbits(0x9c005788, 0x1c, 0x10);             /* fetch enable (i143 DRV_DDFCH_on 0xD0; eCos per frame) */
            setbits(0x9c006c84, 0xfu, 2u | 1u << 2);   /* L1 (VPP0) opaque, L2 (OSD0) transparent */
            R(0x9c005a80) |= 1;
            SAMPLE("init");
        }
        if (m & 2) {
            setbits(0x9c006af4, 3u, 0);
            R(0x9c005a80) |= 1;
            SAMPLE("tgsrc");
        }
        if (m & 4) {
            setbits(0x9c005d84, 3u << 12, path << 12);
            R(0x9c005a80) |= 1;
            SAMPLE("path");
        }
        if (m & 8) {
            R(0x9c005d84) |= 1u << 7;
            R(0x9c005a80) |= 1;
            SAMPLE("vsclbar");
        }
        if (m & 16) {
            R(0x9c005780) = 0;
            R(0x9c0057f0) = 0x80801011;
            R(0x9c005a80) |= 1;
            SAMPLE("nolatch");
        }
        if (m & 32) {
            R(0x9c006a80) = 7;
            SAMPLE("tgen7");
        }
        if (m & 64) {
            for (int j = 0; j < 2000; j++) {
                R(0x9c005a80) |= 1;
                for (volatile int i = 0; i < 500; i++)
                    ;
            }
            SAMPLE("commits");
        }
        if (m & 128) {
            u32 fe = R(0x9c006aac) & 0xfff, ln = 0, tries = 0;
            do
                ln = R(0x9c006ab4) & 0xfff;
            while (ln < fe && ++tries < 2000000);
            R(0x9c0057f0) = 0x80801011;
            R(0x9c005788) |= 1u << 4;
            R(0x9c005a80) |= 1;
            k += fmt(o + k, " [vbl fe=%x line=%x tries=%u]", fe, ln, tries);
            SAMPLE("vbl");
        }
        for (int t = 0; t < 2; t++) {
            if (!(m & (256u << t)))
                continue;
            u32 Y = t ? 0x00600000u : 0x01000000u, C = Y + 0x100000;
            R(0x9c0057f0) = 0x80801002;              /* BIST off (SP7021 value) */
            setbits(0x9c005784, 1u << 28 | 1u << 20, 0);
            R(0x9c005798) = R(0x9c00579c) = R(0x9c0057a0) = Y >> 10;
            R(0x9c0057a4) = R(0x9c0057a8) = R(0x9c0057ac) = C >> 10;
            R(0x9c0057d0) = 1920;
            R(0x9c0057d4) = 480u << 16 | 800;
            R(0x9c005788) |= 1u << 4;
            R(0x9c005a80) |= 1;
            SAMPLE(t ? "memdpb" : "memosd");
        }
        if (m & 1024) {
            R(0x9c005784) = 0xf000;
            R(0x9c005a80) |= 1;
            SAMPLE("mo_f000");
            R(0x9c005784) = 0x5f000;
            R(0x9c005a80) |= 1;
            SAMPLE("mo_5f000");
        }
        if (m & 2048) {
            R(0x9c006adc) = 0x100d;
            R(0x9c005a80) |= 1;
            SAMPLE("adj");
        }
        if (m & 4096) {
            R(0x9c005784) = 0x1e000;
            R(0x9c005a80) |= 1;
            SAMPLE("ma0");
        }
        if (m & 8192) {
            R(0x9c005784) = 0x7e000;
            R(0x9c005a80) |= 1;
            SAMPLE("ma1");
        }
        if (m & 16384) {
            R(0x9c0057c0) = 0x79;
            R(0x9c005a80) |= 1;
            SAMPLE("freerun");
        }
        if (m & 32768) {
            setbits(0x9c0057dc, 1u << 5 | 1u << 4, 0);
            setbits(0x9c005788, 0x18, 0x10);
            setbits(0x9c005d84, 1u << 9, 0);
            R(0x9c0057f0) = 0x80801013;
            R(0x9c005a80) |= 1;
            SAMPLE("frame");
        }
#undef SAMPLE
        active = 1;
        return k + fmt(o + k, " after AF4=%x D84=%x 780=%x", R(0x9c006af4), R(0x9c005d84), R(0x9c005780));
    } else if (c == 8 && n >= 2) {   /* bisect: apply setup parts by mask, wait ~0.3 s, report the error flags */
        u32 m = p[1], f0 = R(0x9c0057ec);
        save();
        if (m & 1)
            init_post();
        if (m & 2) {
            R(0x9c005d80) = 1;
            setbits(0x9c005d84, 1u << 4 | 1u << 6 | 1u << 9 | 1u << 12, 1u << 4 | 1u << 6);
        }
        if (m & 4) {   /* the whole init_fetch (post, scaler, DDFCH, geometry) */
            init_fetch(800, 480, 1920);
        }
        if (m & 8) {   /* index mode on the decoder's last picture with gp 2 */
            for (int e = 0; e < 2; e++) {
                u32 a2 = 0x9c00a400 + 4 * (e ? chroma_idx : luma_idx);
                R(a2) = (R(a2) & ~(3u << 19)) | 2u << 19;
            }
            R(0x9c0057f0) &= ~1u;
            setbits(0x9c005784, 1u << 28 | 1u << 20 | 1u << 21 | 1u << 8, 1u << 20 | 1u << 8);
            R(0x9c005798) = R(0x9c00579c) = R(0x9c0057a0) = luma_idx;
            R(0x9c0057a4) = R(0x9c0057a8) = R(0x9c0057ac) = chroma_idx;
        }
        if (m & 16)
            setbits(0x9c006c84, 3u, 2u);
        if (m & 32)
            R(0x9c005a80) |= 1;
        if (m & 64)
            R(0x9c005788) = 0;   /* DDFCH enable off */
        active = 1;
        u32 f1 = R(0x9c0057ec);
        for (volatile int i = 0; i < 6000000; i++)
            ;
        return fmt(o, "vpp bisect mask %x: flags %x -> %x -> %x; checksum %x", m, f0, f1, R(0x9c0057ec), R(0x9c0057e8));
    } else if (c == 7 && n >= 2) {   /* MOON0 gated clock by ID (0x9C00002C + 4 * (id >> 4), eCos 0x313000) */
        u32 id = p[1], a2 = 0x9c00002c + 4 * (id >> 4);
        if (id >= 0x80)
            return fmt(o, "ERR vpp gclk: id < 0x80");
        R(a2) = 0x10001u << (id & 15);
        return fmt(o, "vpp: gated clock %x set, reg %x = %x", id, a2, R(a2));
    } else if (c == 5 && n >= 2) {   /* MOON0 clock enable by ID (0x9C000004 + 4 * (id >> 4), bit | bit << 16) */
        u32 id = p[1];
        if (id >= 0x80)
            return fmt(o, "ERR vpp clock: id < 0x80");
        R(0x9c000004 + 4 * (id >> 4)) = (1u << (id & 15)) | (1u << ((id & 15) + 16));
        return fmt(o, "vpp: clock %x on, reg %x = %x", id, 0x9c000004 + 4 * (id >> 4), R(0x9c000004 + 4 * (id >> 4)));
    } else if (c == 4 && n >= 9) {   /* poke: u32 address, u32 value, only within the display blocks */
        u32 a = (u32)p[1] | (u32)p[2] << 8 | (u32)p[3] << 16 | (u32)p[4] << 24;
        u32 v = (u32)p[5] | (u32)p[6] << 8 | (u32)p[7] << 16 | (u32)p[8] << 24;
        if ((a & 3) || a < 0x9c005700 || a >= 0x9c006d00)
            return fmt(o, "ERR vpp poke: 0x9c005700..0x9c006cff only");
        save();
        R(a) = v;
        active = 1;
    }
    int k = fmt(o, "vpp: %s; DDFCH", active ? "ACTIVE (auto-restore pending)" : "idle");
    for (u32 a = 0x9c005780; a < 0x9c005800; a += 4)
        if (R(a))
            k += fmt(o + k, " %x=%x", a & 0xfff, R(a));
    k += fmt(o + k, "; A80=%x VSCL D80=%x D84=%x; DMIX C80=%x C84=%x C88=%x; A380=%x", R(0x9c005a80), R(0x9c005d80),
             R(0x9c005d84), R(0x9c006c80), R(0x9c006c84), R(0x9c006c88), R(0x9c00a380));
    k += fmt(o + k, "; DMIX alpha %x %x %x chksum %x pix_en %x; TGEN", R(0x9c006c8c), R(0x9c006c90), R(0x9c006c94),
             R(0x9c006c9c), R(0x9c006ce8));
    for (u32 a = 0x9c006a80; a < 0x9c006b00; a += 4)   /* G213 TGEN (SP7021 layout) */
        if (R(a))
            k += fmt(o + k, " %x=%x", a & 0xfff, R(a));
    return k;
}
