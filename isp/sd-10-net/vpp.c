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
};
static u32 saved[32 + 1 + 48 + 4 + 1 + 12 + 19 + 2 + 2 + 1 + 1 + 1 + 8];
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

static void init_fetch(u32 w, u32 h, u32 pitch)
{
    init_post();
    R(0x9c005d80) = 1;
    setbits(0x9c005d84, 1u << 4 | 1u << 6 | 1u << 9 | 1u << 12, 1u << 4 | 1u << 6);
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
            setbits(0x9c005784, 1u << 28 | 1u << 20, 1u << 20);
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
    return k;
}
