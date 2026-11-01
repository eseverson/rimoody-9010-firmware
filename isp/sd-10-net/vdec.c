/* Hardware H.264 decoder ("MFD") bring-up (notes/h264_decoder.md, section 6), HU1 op 19, net task only.
 * Step 2: one-time init (BG2D index table, H.264 soft reset, register init 1.5 with our own buffers).
 * Step 3: the DPFU / bit-reader path: an Annex-B access unit copied into the bitstream ring (EVB) is read back through
 * the hardware bit reader (0x9C00D400 + 4n reads n bits) and its SPS parsed that way, to compare with h264p.c.
 * Every poll is bounded; a bus stall would be caught by the hardware watchdog (irq.c). */
#include "vdec.h"
#include "h264p.h"

typedef unsigned int u32;
typedef unsigned char u8;
#define M(o) (*(volatile u32 *)(0x9c000000u + (o)))

/* DRAM for the decoder: above everything else we use (payload 0x018, log 0x01A, heap 0x022, uploads 0x030,
 * console framebuffer 0x034-0x0366, trampoline 0x02F) */
#define EVB_ADDR   0x04000000u
#define EVB_KB     2500u          /* 0x9C4 KB, as eCos for codec 6 ("Streaming H264") */
#define SPMP_ADDR  0x04400000u
#define SPMP_KB    1028u          /* 0x404 KB */
#define SCS_ADDR   0x04500000u
#define SCS_KB     2u
#define IDX_EVB    0x28
#define IDX_SPMP   0x29

static int inited;
struct vdec_stats vdec_stats;

static int poll(u32 off, u32 mask, u32 want, u32 tries)
{
    while (tries--)
        if ((M(off) & mask) == want)
            return 0;
    return -1;
}

static void bg2d_set(int idx, u32 phys, int gp, int scan_type)   /* 0x52edc6, 0x52ed0a, 0x52ed32 */
{
    u32 e = M(0xa400 + 4 * idx);
    e = (e & ~0x7ffffu) | (phys >> 10);
    e = (e & ~(3u << 19)) | ((u32)gp & 3) << 19;
    e &= ~(0x3fu << 21);
    e |= scan_type == 0 ? (1u << 22 | 1u << 24 | 2u << 25) : (1u << 24 | 3u << 25);
    M(0xa400 + 4 * idx) = e;
}

static void bg2d_init(void)   /* 0x52ec48, display init's "2dbg" */
{
    static const u32 t1[] = { 0x83828100, 0x05048584, 0x00808604, 0x82818000, 0x05858483, 0x00208604 };
    static const u32 t2[] = { 0x83828100, 0x06050484, 0x00808504, 0x82818000, 0x06058483, 0x00208504 };
    M(0xa39c) &= ~0x20u;
    for (int i = 0; i < 6; i++)
        M(0xa384 + 4 * i) = t1[i], M(0xa3a0 + 4 * i) = t2[i];
    M(0xa3b8) = 0x0a000a00;
    for (int i = 0; i < 256; i++)
        M(0xa400 + 4 * i) = 0x07000000;
    M(0xa39c) |= 0x2000;
    M(0xa39c) |= 0x10;
}

static int soft_reset(void)   /* 0x41325e */
{
    M(0xd000) = 0;
    M(0xd904) = 0;
    M(0xd920) = 1;
    int r = poll(0xd920, 4, 0, 0x3e9 * 16);
    M(0xd904) = 0;
    if (M(0xd0ac)) {
        M(0xd0ac) = 1;
        M(0xd0ac) = 0;
    }
    M(0xd048) = 1;
    M(0xd030) = 0;
    M(0xd038) = 0;
    M(0xd0a8) = 0;
    return r;
}

static int h264_reg_init(void)   /* 0x434aac */
{
    M(0xd0b4) = 0xfe;
    M(0xdf68) = (EVB_KB >> 16) & 3;
    M(0xdf18) = EVB_KB & 0xffff;
    M(0xd980) = 0;
    /* SCS config 0x434958 */
    M(0xd680) = EVB_ADDR >> 10;
    M(0xd684) = EVB_KB;
    M(0xd6c0) = 0x200;
    M(0xd6c8) = 0, M(0xd6c4) = 0, M(0xd6bc) = 0;
    M(0xd640) = 0;
    M(0xd660) = 0;
    M(0xd710) = 1;
    M(0xd714) = 0xffffff;
    for (u32 o = 0xd718; o <= 0xd72c; o += 4)
        M(o) = 0;
    M(0xd70c) = 0, M(0xd68c) = 0, M(0xd688) = 0;
    M(0xd690) = SCS_ADDR >> 10;
    M(0xd694) = SCS_KB;
    /* DPFU1 ring 0x4349e8 */
    M(0xd8a4) = EVB_KB << 8 | IDX_EVB;
    M(0xd924) = EVB_KB << 8 | IDX_EVB;
    if (M(0xd8a0) & 1) {
        M(0xd884) = 0;
        M(0xd8a0) = 1;
        M(0xd8a0) = 0;
    }
    /* SPMP ring 0x434a3e */
    M(0xd50c) = 1, M(0xd548) = 1, M(0xd034) = 0, M(0xd1a4) = 0;
    M(0xd558) = 0, M(0xd55c) = 0, M(0xd564) = 0;
    M(0xd550) = 0x1f8;
    M(0xd550) = 0;
    /* constants 0x434b1e..0x434b7e */
    static const u32 ef[][2] = { { 0xef00, 0xfff }, { 0xef08, 0x1f }, { 0xef0c, 1 }, { 0xef10, 0xff }, { 0xef14, 7 },
                                 { 0xef18, 7 }, { 0xef1c, 7 }, { 0xef20, 0x3ff }, { 0xef24, 7 }, { 0xef28, 0x3f },
                                 { 0xef2c, 0x1f }, { 0xef30, 0x3f }, { 0xef34, 7 }, { 0xef38, 1 }, { 0xef3c, 1 },
                                 { 0xef40, 0x7f }, { 0xef44, 1 }, { 0xef04, 3 } };
    for (unsigned i = 0; i < sizeof ef / sizeof ef[0]; i++)
        M(ef[i][0]) = ef[i][1];
    M(0xec4c) |= 8;
    M(0xd038) = 0;
    bg2d_set(IDX_EVB, EVB_ADDR, 0, 0);
    bg2d_set(IDX_SPMP, SPMP_ADDR, 0, 0);
    return 0;
}

int vdec_init(char *o, vdec_fmt fmt)
{
    u32 e0 = M(0xa400), e1 = M(0xa404);
    int fresh = e0 != 0x07000000 || e1 != 0x07000000;
    if (fresh)
        bg2d_init();
    int r = soft_reset();
    h264_reg_init();
    inited = 1;
    return fmt(o, "vdec: init done (BG2D table %s, soft reset %s); EVB %x (%u KB, idx %x = %x), SPMP %x (idx %x = %x); "
                  "D000 %x D04C %x DC84 %x D550 %x D8A0 %x",
               fresh ? "initialized" : "kept", r ? "TIMED OUT" : "ok", EVB_ADDR, EVB_KB, IDX_EVB, M(0xa400 + 4 * IDX_EVB),
               SPMP_ADDR, IDX_SPMP, M(0xa400 + 4 * IDX_SPMP), M(0xd000), M(0xd04c), M(0xdc84), M(0xd550), M(0xd8a0));
}

/* ---- bit reader ---- */
static u32 rd(int n) { return M(0xd400 + 4 * (u32)n); }   /* n = 1..31: next n bits; 0: 32 bits */
static u32 peek1(void) { return M(0xd484); }
static u32 ue(void)
{
    int z = 0;
    while (!peek1() && z < 32)
        rd(1), z++;
    return rd(z + 1) - 1;   /* the leading 1 and z bits: value = 2^z - 1 + bits, read as (z+1)-bit number - 1 */
}

static int dpfu_start(u32 pos, u32 len)   /* 0x4340cc */
{
    M(0xd884) = 0;
    int r1 = poll(0xd8a0, 0xc, 0, 0x1f5 * 16);
    M(0xd884) = 0;
    M(0xd8a0) = 1;
    M(0xd8a0) = 0;
    M(0xd888) = (pos >> 2) << 6 | 0x20;
    M(0xd88c) = len > 0x7ffffc ? 0x7ffffc : len;
    M(0xd884) = 1;
    int r2 = poll(0xd8a0, 2, 2, 0xc351 * 4);
    if (pos & 3)
        rd((int)(pos & 3) * 8);
    return r1 ? -1 : r2 ? -2 : 0;
}

extern void plat_sync(u32 addr, u32 n);   /* D-cache write-back (+ I-cache invalidate) over a range */

int vdec_bitread(u32 src, u32 len, char *o, vdec_fmt fmt)
{
    if (!inited)
        return fmt(o, "ERR vdec: init first");
    if (len < 16 || len > 0x100000)
        return fmt(o, "ERR vdec: 16 bytes .. 1 MB");
    volatile u8 *d = (volatile u8 *)EVB_ADDR;
    const u8 *s = (const u8 *)src;
    for (u32 i = 0; i < len; i++)
        d[i] = s[i];
    plat_sync(EVB_ADDR, len);
    /* the host parser on the same bytes, for comparison */
    u32 pos = 0, st, nl;
    struct h264_sps sps = { 0 };
    int have = 0;
    while (h264_next_nal(s, len, &pos, &st, &nl))
        if ((s[st] & 31) == 7) {
            have = !h264_parse_sps(s + st, nl, &sps);
            break;
        }
    int r = dpfu_start(0, len);
    u32 sc = rd(0), hdr = rd(8), prof = rd(8), cons = rd(8), lvl = rd(8), id = ue(), lmfn = ue(), poc = ue();
    int k = fmt(o, "vdec: dpfu %d; start code %08x, nal hdr %02x; profile %u constraints %02x level %u sps_id %u "
                   "log2_max_frame_num-4 %u poc_type %u",
                r, sc, hdr, prof, cons, lvl, id, lmfn, poc);
    if (have)
        k += fmt(o + k, "; host parser: profile %u level %u sps_id %u log2_max_frame_num-4 %u poc_type %u -> %s",
                 sps.profile_idc, sps.level_idc, sps.id, sps.log2_max_frame_num - 4, sps.poc_type,
                 sc == 1 && (hdr & 31) == 7 && prof == sps.profile_idc && lvl == sps.level_idc && id == sps.id &&
                         lmfn == sps.log2_max_frame_num - 4 && poc == sps.poc_type
                     ? "MATCH"
                     : "MISMATCH");
    vdec_stats.bitreads++;
    return k;
}

int vdec_status(char *o, vdec_fmt fmt)
{
    return fmt(o, "vdec: %s; D000 %x D04C %x D0AC %x DC84 %x D550 %x D570 %x D704 %x D8A0 %x D920 %x; idx28 %x idx29 %x",
               inited ? "inited" : "not inited", M(0xd000), M(0xd04c), M(0xd0ac), M(0xdc84), M(0xd550), M(0xd570),
               M(0xd704), M(0xd8a0), M(0xd920), M(0xa400 + 4 * IDX_EVB), M(0xa400 + 4 * IDX_SPMP));
}
