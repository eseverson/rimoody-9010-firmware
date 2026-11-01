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

/* Breadcrumb that survives a reset (DRAM is kept): the step a risky operation is in, written back to memory before
 * the step runs. The next boot reports it (vdec_crumb_report) so a hang off the network still says where it was. */
#define CRUMB ((volatile u32 *)0x01F0F000u)
#define CRUMB_MAGIC 0x56444543u   /* "VDEC" */
static void crumb(u32 step)
{
    CRUMB[0] = CRUMB_MAGIC, CRUMB[1] = step, CRUMB[2] = CRUMB[2] + 1;
    __asm__ volatile("cctl %0, L1D_VA_WB\n\tcctl %1, L1D_VA_WB\n\tmsync all" ::"r"(0x01F0F000u), "r"(0x01F0F020u)
                     : "memory");
}
static void crumb_clear(void) { CRUMB[0] = 0; __asm__ volatile("cctl %0, L1D_VA_WB\n\tmsync all" ::"r"(0x01F0F000u) : "memory"); }

int vdec_crumb_report(char *o, vdec_fmt fmt)   /* at boot: a step left behind by the previous run, then cleared */
{
    if (CRUMB[0] != CRUMB_MAGIC)
        return 0;
    u32 step = CRUMB[1];
    crumb_clear();
    return fmt(o, "vdec: the previous run stopped during step %u (timeouts %x D570 %x D000 %x D04C %x D55C %x DC84 %x D0AC %x)\n",
               step, CRUMB[4], CRUMB[5], CRUMB[6], CRUMB[7], CRUMB[8], CRUMB[9], CRUMB[10]);
}

/* hold the MFD in reset and release it (eCos 0x432dfa): stops a runaway engine */
void vdec_hw_reset(void)
{
    M(0x58) = 0x80008000u, M(0x6c) = 0x9bd89bd8u, M(0x70) = 0x00030003u;
    for (volatile int i = 0; i < 64; i++)
        ;
    M(0x58) = 0x80000000u, M(0x6c) = 0x9bd80000u, M(0x70) = 0x00030000u;
    inited = 0;
}

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

/* ---- step 4: one picture (single-slice I picture) through the SPMP command ring ---- */
#define FB_LUMA   0x04600000u   /* pitch 896 (800 rounded to 128), 512 rows (480 rounded to 64): 0x70000 */
#define FB_CHROMA 0x04670000u   /* interleaved CbCr, 896 x 256 */
#define FB_MVC    0x04700000u
#define WORK_3B   0x04720000u   /* "unique" working buffers BG2D 0x3B / 0x3C (sizes open: 128 KB each) */
#define WORK_3C   0x04740000u
#define IDX_LUMA  0x00
#define IDX_CHROMA 0x14
#define IDX_MVC   0x2a

static u32 ring_n;   /* words written in the SPMP ring */
static void put(u32 w)   /* SPMP words are stored big-endian */
{
    volatile u32 *r = (volatile u32 *)SPMP_ADDR;
    r[ring_n++] = w >> 24 | (w >> 8 & 0xff00) | (w << 8 & 0xff0000) | w << 24;
}
static u32 regno(u32 addr) { return (addr - 0x9c00c000u) >> 2; }
static void w16(u32 addr, u32 v) { put(regno(addr) << 16 | (v & 0xffff)); }
static void w32(u32 addr, u32 v) { put(0x10000000u | regno(addr) << 16); put(v); }

static void cache_inval(u32 a, u32 n)   /* L1D_VA_INVAL over a range (the decoder wrote it behind the cache) */
{
    for (u32 x = a & ~31u; x < a + n; x += 32)
        __asm__ volatile("cctl %0, L1D_VA_INVAL" ::"r"(x) : "memory");
    __asm__ volatile("msync all" ::: "memory");
}

int vdec_decode(u32 src, u32 len, char *o, vdec_fmt fmt, int kick, u32 skip)
{
    if (!inited)
        return fmt(o, "ERR vdec: init first");
    if (len < 16 || len > 0x100000)
        return fmt(o, "ERR vdec: 16 bytes .. 1 MB");
    const u8 *s = (const u8 *)src;
    /* host-side parse: SPS, PPS, first slice */
    struct h264_sps sps = { 0 };
    struct h264_pps pps = { 0 };
    struct h264_slice sh = { 0 };
    u32 pos = 0, st, nl, slice_at = 0;
    int have = 0;
    while (h264_next_nal(s, len, &pos, &st, &nl)) {
        int t = s[st] & 31;
        if (t == 7)
            h264_parse_sps(s + st, nl, &sps);
        else if (t == 8)
            h264_parse_pps(s + st, nl, &pps);
        else if ((t == 5 || t == 1) && !have) {
            have = !h264_parse_slice(s + st, nl, &sps, &pps, &sh);
            slice_at = st;
        }
    }
    if (!have || sh.slice_type != 2)
        return fmt(o, "ERR vdec: need an I slice (have %d, type %u)", have, sh.slice_type);
    crumb(1);   /* bitstream into the EVB */
    volatile u8 *d = (volatile u8 *)EVB_ADDR;
    for (u32 i = 0; i < len; i++)
        d[i] = s[i];
    plat_sync(EVB_ADDR, len);
    /* read the slice header through the bit reader, then capture the slice-data position (H264_SliceHeaderEnd) */
    crumb(2);   /* slice header through the bit reader */
    int r = dpfu_start(slice_at, len - slice_at);
    for (u32 left = sh.data_bit_rbsp; left; ) {
        u32 k = left > 31 ? 31 : left;
        rd((int)k);
        left -= k;
    }
    u32 save0 = M(0xd894), save1 = M(0xd898);
    M(0xd884) = 0;   /* stop */
    if (skip & 8) {   /* experiment: reset the DPFU before the engine reprograms it */
        poll(0xd8a0, 0xc, 0, 0x1f5 * 16);
        M(0xd8a0) = 1;
        M(0xd8a0) = 0;
    }
    if (skip & 16)    /* experiment: the 0x20 flag every DPFU start value carries */
        save0 |= 0x20;
    crumb(3);   /* frame buffers, BG2D */
    /* frame buffers: fill with a marker so untouched memory shows */
    for (u32 i = 0; i < 0x70000; i += 4)
        *(volatile u32 *)(FB_LUMA + i) = 0x55555555;
    for (u32 i = 0; i < 0x38000; i += 4)
        *(volatile u32 *)(FB_CHROMA + i) = 0x55555555;
    plat_sync(FB_LUMA, 0x70000);
    plat_sync(FB_CHROMA, 0x38000);
    u32 pitch = (sps.width + 127) & ~127u;
    u32 a380 = M(0xa380);   /* "mb size" slots: shared with the display; the decoder takes slot 1 for small widths */
    M(0xa380) = (a380 & ~0xff00u) | (pitch >> 4) << 8;
    bg2d_set(IDX_LUMA, FB_LUMA, 1, 0);
    bg2d_set(IDX_CHROMA, FB_CHROMA, 1, 0);
    bg2d_set(IDX_MVC, FB_MVC, 1, 0);
    bg2d_set(0x3b, WORK_3B, 0, 0);
    bg2d_set(0x3c, WORK_3C, 0, 0);
    /* the slice's command block (notes/h264_fields.md "Test IDR command values") */
    ring_n = 0;
    if (!(skip & 1)) {
        w32(0x9c00d888, save0);
        w32(0x9c00d88c, save1);
    }
    w16(0x9c00d018, 0);                               /* field_pic_flag */
    w16(0x9c00d01c, 1);                               /* !bottom_field_flag */
    w16(0x9c00d090, sh.direct_spatial_mv_pred);
    w16(0x9c00d020, 0);                               /* MbaffFrameFlag */
    w16(0x9c00d014, pps.constrained_intra_pred);
    w16(0x9c00d080, sh.nal_ref_idc != 0);
    w16(0x9c00d084, pps.entropy_coding_mode);
    w16(0x9c00d010, ((u32)pps.chroma_qp_index_offset & 0x3f) | ((u32)pps.second_chroma_qp_index_offset & 0x3f) << 6);
    w16(0x9c00d0a0, (u32)sh.qp);
    w16(0x9c00d024, sh.slice_type);
    w16(0x9c00d008, sps.mb_width);
    w16(0x9c00d00c, sps.mb_height);
    w16(0x9c00d088, sh.first_mb / sps.mb_width);
    w16(0x9c00d08c, sh.first_mb % sps.mb_width);
    w16(0x9c00d094, sh.num_ref_idx_l0 - 1);
    w16(0x9c00d098, 0xffff);                          /* l1 for non-B: S+0x39 - 1 with S+0x39 = 0 */
    w16(0x9c00d0a4, 0);
    w16(0x9c00d180, pps.weighted_pred);
    w16(0x9c00d184, pps.weighted_bipred_idc);
    w16(0x9c00d188, 0);
    w16(0x9c00d18c, 0);
    w16(0x9c00d800, sh.disable_deblocking);
    w16(0x9c00d804, (u32)sh.alpha_offset);
    w16(0x9c00d808, (u32)sh.beta_offset);
    w32(0x9c00d028, 0x3bu << 24 | 0x3cu << 16 | IDX_MVC);
    w16(0x9c00d02c, IDX_LUMA | IDX_CHROMA << 8);
    w16(0x9c00d09c, sh.cabac_init_idc);
    w16(0x9c00d004, (sps.profile_idc > 99) | (sps.chroma_format_idc & 3) << 1 | (pps.transform_8x8_mode & 1) << 3);
    if (!(skip & 4))
        for (u32 a = 0x9c00d190; a <= 0x9c00d19c; a += 4)
            w32(a, 0xffffffff);
    if (!(skip & 2)) {
        put(0x2040003f);                              /* ref lists (empty for I) */
        for (int i = 0; i < 64; i++)
            put(0);
        put(0x2000003f);                              /* inverse map */
        for (int i = 0; i < 64; i++)
            put(0);
    }
    put(0xf8000000);                                  /* end of slice */
    plat_sync(SPMP_ADDR, ring_n * 4);
    if (!kick) {
        crumb_clear();
        return fmt(o, "vdec prepare: slice at %u (header %u bits), DPFU %d save %x/%x; ring %u words; DC84 %x ECA0 %x; "
                      "A380 %x (was %x); idx00 %x idx14 %x idx2a %x",
                   slice_at, sh.data_bit_rbsp, r, save0, save1, ring_n, M(0xdc84), M(0xeca0), M(0xa380), a380,
                   M(0xa400), M(0xa400 + 4 * IDX_CHROMA), M(0xa400 + 4 * IDX_MVC));
    }
    crumb(4);   /* kick */
    /* picture decode (h264_trigger_decode / real_decode_h264_video) */
    u32 pre_dc84 = M(0xdc84), pre_eca0 = M(0xeca0);
    crumb(40);
    M(0xde6c) = 0x18;
    crumb(41);
    M(0xd980) = 1;
    for (u32 a = 0xdb00; a <= 0xdb7c; a += 4)
        M(a) = 0;
    M(0xd980) = 0;
    crumb(42);
    M(0xd0b4) = 0xfe;
    M(0xd034) = 1;
    crumb(43);
    M(0xd924) = EVB_KB << 8 | IDX_EVB;
    M(0xd54c) = SPMP_KB << 8 | IDX_SPMP;
    M(0xd548) = 1;
    M(0xd048) = 1, M(0xd1a4) = 0, M(0xd558) = 0, M(0xd55c) = 0, M(0xd564) = 0;
    M(0xd550) = 0x1f8;
    M(0xd550) = 0;
    M(0xd554) = ring_n << 21 | 0;   /* first slice: length in words, start word 0 */
    M(0xd558) = 1;
    crumb(44);
    M(0xd550) = 0;
    M(0xd550) = 1;
    crumb(45);
    int k1 = poll(0xd550, 1, 1, 20000);
    crumb(46);
    int k2 = k1 ? -1 : poll(0xd570, 1, 1, 200000);
    crumb(47);
    u32 d570 = M(0xd570);
    M(0xd570) = 1;
    M(0xd550) = 0;
    crumb(48);
    int k3 = k2 ? -1 : poll(0xdc84, 1, 1, 200000);
    crumb(49);
    /* results into the breadcrumb first (words 4..11), so a hang in what follows still leaves them */
    CRUMB[4] = (u32)(k1 & 1) | (u32)(k2 & 1) << 1 | (u32)(k3 & 1) << 2, CRUMB[5] = d570;
    crumb(50);
    if (k1 || k2 || k3) {   /* stuck: its status registers stall the bus when read; reset it through MOON0 */
        vdec_hw_reset();
        crumb(54);
        M(0xa380) = a380;
        crumb_clear();
        return fmt(o, "vdec decode (skip %x): TIMEOUT (start %d done %d end %d, D570 %x); decoder reset through MOON0, init again",
                   skip, k1, k2, k3, d570);
    }
    u32 st000 = M(0xd000);
    CRUMB[6] = st000;
    crumb(51);
    u32 st04c = M(0xd04c), st0ac = M(0xd0ac), st55c = M(0xd55c), st558 = M(0xd558), st904 = M(0xd904), stc84 = M(0xdc84);
    CRUMB[7] = st04c, CRUMB[8] = st55c, CRUMB[9] = stc84, CRUMB[10] = st0ac;
    crumb(52);
    if (k1 || k2 || k3)
        soft_reset();   /* the decoder's own reset; asserting its reset lines mid-DMA locked the bus (2026-11-01) */
    crumb(53);
    M(0xa380) = a380;
    crumb(5);   /* reading back */
    cache_inval(FB_LUMA, 0x70000);
    cache_inval(FB_CHROMA, 0x38000);
    /* how much of the luma plane changed, and its first bytes */
    u32 changed = 0, sum = 0;
    for (u32 i = 0; i < 0x70000; i += 4) {
        u32 v = *(volatile u32 *)(FB_LUMA + i);
        changed += v != 0x55555555;
        sum += (v & 0xff) + (v >> 8 & 0xff) + (v >> 16 & 0xff) + (v >> 24);
    }
    vdec_stats.decodes++;
    crumb_clear();
    return fmt(o, "vdec decode (skip %x): slice at %u (header %u bits), DPFU %d save %x/%x; ring %u words; pre DC84 %x ECA0 %x; "
                  "start %s, done %s (D570 %x), picture end %s; D000 %x D04C %x D0AC %x D55C %x D558 %x D904 %x DC84 %x; "
                  "luma words changed %u of %u, mean %u; first %08x %08x",
               skip, slice_at, sh.data_bit_rbsp, r, save0, save1, ring_n, pre_dc84, pre_eca0, k1 ? "TIMEOUT" : "ok",
               k2 ? "TIMEOUT" : "ok", d570, k3 ? "TIMEOUT" : "ok", st000, st04c, st0ac, st55c, st558, st904, stc84,
               changed, 0x70000 / 4, sum / 0x70000, *(volatile u32 *)FB_LUMA, *(volatile u32 *)(FB_LUMA + 4));
}

/* SPMP engine alone: a command block of 16-bit and 32-bit writes to the weight-denominator / SPAR result registers
 * (harmless outside a decode) and the end marker; kick it like a picture and read the registers back. */
int vdec_spmp_test(char *o, vdec_fmt fmt, u32 extras)
{
    if (!inited)
        return fmt(o, "ERR vdec: init first");
    crumb(10);
    ring_n = 0;
    w16(0x9c00d188, 0x0005);
    w16(0x9c00d18c, 0x0003);
    w32(0x9c00d190, 0x12345678);
    w32(0x9c00d194, 0x9abcdef0);
    put(0xf8000000);
    plat_sync(SPMP_ADDR, ring_n * 4);
    M(0xd188) = 0, M(0xd18c) = 0, M(0xd190) = 0, M(0xd194) = 0;
    /* the picture-only pre-kick writes of real_decode_h264_video, by bit, to find the one that stops the engine */
    crumb(12);
    if (extras & 1)
        M(0xde6c) = 0x18;
    if (extras & 2) {
        M(0xd980) = 1;
        for (u32 a2 = 0xdb00; a2 <= 0xdb7c; a2 += 4)
            M(a2) = 0;
        M(0xd980) = 0;
    }
    if (extras & 4)
        M(0xd0b4) = 0xfe;
    if (extras & 8)
        M(0xd034) = 1;
    if (extras & 16)
        M(0xd924) = EVB_KB << 8 | IDX_EVB;
    crumb(13);
    M(0xd54c) = SPMP_KB << 8 | IDX_SPMP;
    M(0xd548) = 1;
    M(0xd048) = 1, M(0xd1a4) = 0, M(0xd558) = 0, M(0xd55c) = 0, M(0xd564) = 0;
    M(0xd550) = 0x1f8;
    M(0xd550) = 0;
    M(0xd554) = ring_n << 21 | 0;
    M(0xd558) = 1;
    M(0xd550) = 0;
    M(0xd550) = 1;
    crumb(11);
    int k1 = poll(0xd550, 1, 1, 20000);
    int k2 = k1 ? -1 : poll(0xd570, 1, 1, 200000);
    u32 d570 = M(0xd570);
    M(0xd570) = 1;
    M(0xd550) = 0;
    u32 a = M(0xd188), b = M(0xd18c), c = M(0xd190), d = M(0xd194), n55c = M(0xd55c);
    if (k1 || k2)
        soft_reset();
    crumb_clear();
    int ok = a == 5 && b == 3 && c == 0x12345678 && d == 0x9abcdef0;
    return fmt(o, "vdec spmp (extras %x): start %s, done %s (D570 %x, D55C %x); D188 %x D18C %x D190 %x D194 %x -> %s",
               extras, k1 ? "TIMEOUT" : "ok", k2 ? "TIMEOUT" : "ok", d570, n55c, a, b, c, d, ok ? "REGISTERS LOADED" : "not loaded");
}

int vdec_status(char *o, vdec_fmt fmt)
{
    return fmt(o, "vdec: %s; D000 %x D04C %x D0AC %x DC84 %x D550 %x D570 %x D704 %x D8A0 %x D920 %x; idx28 %x idx29 %x",
               inited ? "inited" : "not inited", M(0xd000), M(0xd04c), M(0xd0ac), M(0xdc84), M(0xd550), M(0xd570),
               M(0xd704), M(0xd8a0), M(0xd920), M(0xa400 + 4 * IDX_EVB), M(0xa400 + 4 * IDX_SPMP));
}
