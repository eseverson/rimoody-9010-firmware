/* Video through the OSD0 layer (notes/vpp0_display.md: VPP0 does not run yet). Each decoded picture (the MFD's tiled
 * NV12, tools/mfd_detile.py) is converted by the CPU into a linear YUY2 buffer that OSD0 scans (SP7021 OSD region format
 * 4, header word 5 = 0x0004xxxx). A picture at least as large as the 1024 x 600 panel (Android Auto at 1280 x 720 with
 * margins) shows its center 1:1 full screen; a smaller one is centered. Two buffers: the header's data address flips
 * to the finished one. Net task only. */
#include "osdvid.h"
#include "irq.h"

typedef unsigned int u32;
typedef unsigned char u8;
#define R(a) (*(volatile u32 *)(a))

#define OSD0     0x9c006200u
#define HDR      0x03661000u   /* our region header (the console's is at 0x03660000) */
#define BUF0     0x07A00000u   /* up to 1024 x 600 x 2 = 0x12C000 each, free DRAM after the decoder's slots */
#define BUF1     0x07C00000u
#define PANEL_W  1024u
#define PANEL_H  600u
#define PITCH    1920u         /* the decoder's pitch */

static int cur, on;
static u32 frames, last_us, saved_link;

static inline u32 bswap(u32 v) { return v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24; }

/* byte offset of the 16-byte run holding pixels x..x+15 (x % 16 == 0) of row y in the tiled plane */
static inline u32 run(u32 x, u32 y)
{
    u32 st = y / 64, xg = (x / 16) % 4;
    return st * PITCH * 64 + (x / 64) * 4096 + (((y & 1) ^ (xg & 1) ^ (st & 1)) * 0x800) + xg * 0x200 + ((y % 64) / 2) * 16;
}

static u32 out_w = 800, out_h = 480;   /* the region's size, set by the last picture */

static void put_hdr(u32 buf)
{
    volatile u8 *h = (volatile u8 *)HDR;
    u32 W = out_w, H = out_h, row = (PANEL_H - H) / 2, col = (PANEL_W - W) / 2;
    for (int i = 0; i < 128; i++)
        h[i] = 0;
    h[0] = 0x04, h[2] = 0x10;                       /* YUY2 */
    h[4] = (u8)(H >> 8), h[5] = (u8)H, h[6] = (u8)(W >> 8), h[7] = (u8)W;
    h[8] = (u8)(row >> 8), h[9] = (u8)row;          /* display start row */
    h[0xa] = (u8)(col >> 8), h[0xb] = (u8)col;      /* start column */
    h[0x15] = 4, h[0x16] = (u8)(W >> 8), h[0x17] = (u8)W;
    h[0x18] = 0xff, h[0x19] = 0xff, h[0x1a] = 0xff, h[0x1b] = 0xe0;
    h[0x1c] = (u8)(buf >> 24), h[0x1d] = (u8)(buf >> 16), h[0x1e] = (u8)(buf >> 8), h[0x1f] = (u8)buf;
    __asm__ volatile("cctl %0, L1D_VA_WB\n\tcctl %1, L1D_VA_WB\n\tcctl %2, L1D_VA_WB\n\tcctl %3, L1D_VA_WB\n\tmsync all"
                     ::"r"(HDR), "r"(HDR + 32), "r"(HDR + 64), "r"(HDR + 96) : "memory");
}

static void cache_range(u32 a, u32 n, int wb)
{
    for (u32 x = a & ~31u; x < a + n; x += 32) {
        if (wb)
            __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(x) : "memory");
        else
            __asm__ volatile("cctl %0, L1D_VA_INVAL" ::"r"(x) : "memory");
    }
    __asm__ volatile("msync all" ::: "memory");
}

extern unsigned long long clk_us(void);

/* Whole L1D write-back + invalidate (a few hundred lines) instead of ~80000 per-line operations for the source planes;
 * interrupts off in between so an ISR cannot dirty a line that the invalidate would then drop. */
static void dcache_wb_inval_all(void)
{
    u32 f = irq_save();
    __asm__ volatile("cctl l1d_wball, alevel\n\tcctl l1d_invalall\n\tmsync all" ::: "memory");
    irq_restore(f);
}
static void dcache_wb_all(void) { __asm__ volatile("cctl l1d_wball, alevel\n\tmsync all" ::: "memory"); }

static u32 t_in, t_loop, t_out;
int osdvid_fast = 1;

void osdvid_frame(u32 luma, u32 chroma, u32 src_w, u32 src_h)
{
    if (!on || !src_w || !src_h)
        return;
    unsigned long long t0 = clk_us();
    u32 W, H, x0, y0;
    if (src_w >= PANEL_W && src_h >= PANEL_H)
        W = PANEL_W, H = PANEL_H, x0 = ((src_w - PANEL_W) / 2) & ~15u, y0 = ((src_h - PANEL_H) / 2) & ~1u;
    else
        W = src_w & ~15u, H = src_h & ~1u, x0 = 0, y0 = 0;
    if (osdvid_fast)
        dcache_wb_inval_all();
    else {
        cache_range(luma, PITCH * ((y0 + H + 63) & ~63u), 0);
        cache_range(chroma, PITCH * ((y0 + H) / 2 + 32), 0);
    }
    unsigned long long t1 = clk_us();
    u32 dst = cur ? BUF0 : BUF1;   /* the one not being shown */
    for (u32 y = 0; y < H; y++) {
        u32 *o = (u32 *)(dst + y * W * 2);
        u32 ly = y0 + y, cy = ly / 2;
        /* offsets of the four 16-pixel runs of a 64-pixel group (run()), for this luma row and its chroma row */
        u32 lst = ly / 64, cst = cy / 64, lp = (ly & 1) ^ (lst & 1), cp = (cy & 1) ^ (cst & 1);
        u32 lrow = luma + lst * PITCH * 64 + ((ly % 64) / 2) * 16, crow = chroma + cst * PITCH * 64 + ((cy % 64) / 2) * 16;
        u32 lo[4], co[4];
        for (u32 g = 0; g < 4; g++)
            lo[g] = ((lp ^ (g & 1)) * 0x800) + g * 0x200, co[g] = ((cp ^ (g & 1)) * 0x800) + g * 0x200;
        for (u32 x = x0; x < x0 + W; x += 16) {
            u32 g = (x / 16) & 3, grp = (x / 64) * 4096;
            const u32 *l = (const u32 *)(lrow + grp + lo[g]);
            const u32 *c = (const u32 *)(crow + grp + co[g]);
            /* each 8-byte group is stored byte-reversed: pixels 0..3 = bswap(word 1), 4..7 = bswap(word 0) */
            u32 ys[4] = { bswap(l[1]), bswap(l[0]), bswap(l[3]), bswap(l[2]) };
            u32 cs[4] = { bswap(c[1]), bswap(c[0]), bswap(c[3]), bswap(c[2]) };   /* U V U V ... */
            for (int k = 0; k < 4; k++) {   /* 4 pixels (2 pairs) per step: Y0 U Y1 V little-endian */
                u32 yy = ys[k], cc = cs[k];
                o[0] = (yy & 0xff) | (cc & 0xff) << 8 | (yy >> 8 & 0xff) << 16 | (cc >> 8 & 0xff) << 24;
                o[1] = (yy >> 16 & 0xff) | (cc >> 16 & 0xff) << 8 | (yy >> 24) << 16 | (cc >> 24) << 24;
                o += 2;
            }
        }
    }
    unsigned long long t2 = clk_us();
    if (osdvid_fast)
        dcache_wb_all();
    else
        cache_range(dst, W * H * 2, 1);
    out_w = W, out_h = H;
    put_hdr(dst);
    cur = !cur;
    frames++;
    unsigned long long t3 = clk_us();
    t_in = (u32)(t1 - t0), t_loop = (u32)(t2 - t1), t_out = (u32)(t3 - t2);
    last_us = (u32)(t3 - t0);
}

void osdvid_times(unsigned int *in, unsigned int *loop, unsigned int *out) { *in = t_in, *loop = t_loop, *out = t_out; }

void osdvid_on(void)
{
    if (on)
        return;
    saved_link = R(OSD0 + 8);
    for (u32 a = BUF0; a < BUF0 + out_w * out_h * 2; a += 4)   /* black */
        R(a) = 0x80108010u;
    cache_range(BUF0, out_w * out_h * 2, 1);
    put_hdr(BUF0);
    cur = 0;
    R(OSD0 + 8) = HDR;
    R(OSD0 + 4) = 1;
    on = 1;
}

void osdvid_off(void)
{
    if (!on)
        return;
    on = 0;
    R(OSD0 + 8) = saved_link;
}

int osdvid_on_now(void) { return on; }
unsigned int osdvid_frames(void) { return frames; }
unsigned int osdvid_last_us(void) { return last_us; }
