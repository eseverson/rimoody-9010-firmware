/* VPP0 video layer bring-up (notes/vpp0_display.md), HU1 op 24, net task only. Every change to the display
 * registers is undone by vpp_restore (the saved blocks written back), automatically after the requested seconds. */
#include "vpp.h"

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
};
static u32 saved[32 + 1 + 48 + 4 + 1 + 12 + 19 + 2 + 2 + 1 + 1 + 1 + 8 + 1 + 1];
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

/* probe (c == 9): wait ~0.2 s, then the checksums of the three stages: DDFCH 0x7E8 (and its error flags 0x7EC), VSCL
 * 0x9C005E58 (i143 G188.22 vscl0_checksum_result), DMIX 0x9C006C9C (SP7021 G217.07). Nonzero past a stage = data there. */
static int probe_sample(char *o, vpp_fmt fmt, const char *name)
{
    for (volatile int i = 0; i < 4000000; i++)
        ;
    return fmt(o, " [%s ddfch %x/%x vscl %x dmix %x]", name, R(0x9c0057e8), R(0x9c0057ec), R(0x9c005e58), R(0x9c006c9c));
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
            for (int e = 0; e < 2; e++) {
                u32 a2 = 0x9c00a400 + 4 * (e ? chroma_idx : luma_idx);
                R(a2) = (R(a2) & ~(3u << 19)) | 2u << 19;
            }
            setbits(0x9c005784, 1u << 28 | 1u << 20 | 1u << 21 | 1u << 8, 1u << 20 | 1u << 8);
            setbits(0x9c005d84, 1u << 9, 1u << 9);
            setbits(0x9c0057dc, 3u << 16 | 1u, 1u << 16 | 1u);
            setbits(0x9c005a80, 1u << 8 | 1u << 9 | 1u << 12, 1u << 8);
            setbits(0x9c005788, 0x1c, 0x10);
            R(0x9c005798) = luma_idx, R(0x9c00579c) = luma_idx, R(0x9c0057a0) = luma_idx;
            R(0x9c0057a4) = chroma_idx, R(0x9c0057a8) = chroma_idx, R(0x9c0057ac) = chroma_idx;
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
        const u32 w = 800, h = 480, pitch = 896, Y = n >= 5 && p[4] ? 0x03700000u : 0x04e00000u, C = Y + pitch * h;
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
    } else if (c == 9 && n >= 2) {   /* probe (static analysis round 2): where does VPP0's data path break? Samples the DDFCH
                                      * checksum (0x7E8), VSCL checksum (0x9C005E58, i143 G188.22) and DMIX checksum (0x9C006C9C)
                                      * after each step; a nonzero value past a stage means that stage has data. p[1] mask:
                                      *  1 eCos-style init (init_fetch) + DDFCH color bar, VPP0 opaque, OSD0 transparent (DMIX only,
                                      *    the console stays on in software), commit
                                      *  2 TGEN0 source select 0x9C006AF4 field 0 (VPP0) <- 0 (eCos 0x52e5fc, SP7021 tgen_source_sel)
                                      *  4 VSCL path select 0x9C005D84 [13:12] <- p[2] (i143: 0 VPPDMA, 1 OSD0, 2 DDFCH)
                                      *  8 VSCL color bar, 0x9C005D84 b7 (eCos pattern 0xE, i143 0x80)
                                      * 16 DDFCH latch_en off (0x9C005780 = 0) and the BIST register rewritten
                                      * 32 TGEN0 latch mode (0x9C006A80 = 7, SP7021 DRV_TGEN_Init)
                                      * No auto-restore (the reply must beat hu.py's 2 s timeout): `hu.py vpp restore` afterwards. */
        u32 m = p[1], path = n >= 3 ? p[2] & 3 : 2;
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
