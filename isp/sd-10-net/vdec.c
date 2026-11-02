/* Hardware H.264 decoder ("MFD") bring-up (notes/h264_decoder.md, section 6), HU1 op 19, net task only.
 * Step 2: one-time init (BG2D index table, H.264 soft reset, register init 1.5 with our own buffers).
 * Step 3: the DPFU / bit-reader path: an Annex-B access unit copied into the bitstream ring (EVB) is read back through
 * the hardware bit reader (0x9C00D400 + 4n reads n bits) and its SPS parsed that way, to compare with h264p.c.
 * Every poll is bounded; a bus stall would be caught by the hardware watchdog (irq.c). */
#include "vdec.h"
#include "h264p.h"
#include "clk.h"
#include <string.h>

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

static int dpfu_flag;   /* eCos ctx+0xAA458: D884 = 2*flag + 1 to start, 2*flag at the slice header end */
static int dpfu_start(u32 pos, u32 len)   /* 0x4340cc */
{
    M(0xd884) = 0;
    int r1 = poll(0xd8a0, 0xc, 0, 0x1f5 * 16);
    M(0xd884) = 0;
    M(0xd8a0) = 1;
    M(0xd8a0) = 0;
    M(0xd888) = (pos >> 2) << 6 | 0x20;
    M(0xd88c) = len > 0x7ffffc ? 0x7ffffc : len;
    M(0xd884) = 2u * (u32)dpfu_flag + 1;
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

/* ---- steps 4 and 5: pictures (one slice each, I and P) through the SPMP command ring ---- */
/* The decoder lays pictures out with the pitch in BG2D "mb size" slot 0 (0x9C00A380 byte 0 = 0x78: 1920 bytes, set
 * by the display init and shared with it), in 64-row stripes of 4 KB macro-tiles 64 pixels wide (notes/h264_decoder.md
 * "Output layout"). So the buffers use that pitch: 1920 x 512 luma, 1920 x 256 interleaved CbCr.
 * Four frame slots (DRAM 0x04600000..0x04DA0000; 0x06000000 is in use by something else): luma at +0, CbCr at
 * +0x100000, co-located motion vectors ("MVC", 0x1A000 for 800x480) at +0x180000. BG2D indices per slot k: luma k,
 * chroma 0x14 + k, MVC 0x2A + k, as eCos's tables. */
#define FB_PITCH    1920u
#define FB_LUMA_N   0xF0000u
#define FB_CHROMA_N 0x78000u
#define WORK_3B     0x047A0000u   /* "unique" working buffers BG2D 0x3B / 0x3C (sizes open: 128 KB each), after slot 0's MVC */
#define WORK_3C     0x047C0000u
#define NSLOT       4
static const u32 slot_base[NSLOT] = { 0x04600000u, 0x04800000u, 0x04A00000u, 0x04C00000u };
#define SLOT_LUMA(k)   (slot_base[k])
#define SLOT_CHROMA(k) (slot_base[k] + 0x100000u)
#define SLOT_MVC(k)    (slot_base[k] + 0x180000u)
#define IDX_LUMA(k)    (k)
#define IDX_CHROMA(k)  (0x14 + (k))
#define IDX_MVC(k)     (0x2a + (k))

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

static u32 tiled(u32 x, u32 y)   /* byte offset of luma pixel (x, y) in the decoder's layout (tools/mfd_detile.py) */
{
    u32 st = y / 64, xg = (x / 16) % 4;
    u32 o = st * FB_PITCH * 64 + (x / 64) * 4096 + (((y & 1) ^ (xg & 1) ^ (st & 1)) * 0x800) + xg * 0x200 +
            ((y % 64) / 2) * 16 + (x % 16);
    return (o & ~7u) | (7 - (o & 7));
}

/* short-term reference frames (sliding window; no long-term, no adaptive marking: refused below) */
static struct { int used; u32 frame_num; } dpb[NSLOT];

struct pic_result { u32 nal_bad, t_us, spins, d570, timeouts, save0, save1, hdr_bits; int dpfu; };

struct vslice { const u8 *nal; u32 len; struct h264_slice sh; };
#define MAX_SLICES 68   /* one per macroblock row at 1088 lines */

/* one picture: its slice NALs (each from its header byte) into the EVB, each slice header through the bit reader and a
 * command block per slice, the kick, and the wait for the last pixel. kick 0 stops before the kick (the ring is left
 * for inspection). */
static int decode_pic(const struct vslice *sl, int ns, const struct h264_sps *sps, const struct h264_pps *pps, int cur,
                      const int *l0, int n0, int kick, u32 skip, struct pic_result *pr)
{
    crumb(1);   /* bitstream into the EVB, emulation-prevention bytes removed (00 00 03 -> 00 00): the hardware does not
                   strip them (a noise frame desynced at once); eCos's start-code scanner presumably does on its copy */
    volatile u8 *d = (volatile u8 *)EVB_ADDR;
    u32 wo = 0, at[MAX_SLICES], dend[MAX_SLICES], blk[MAX_SLICES + 1];
    for (int j = 0; j < ns; j++) {
        const u8 *s = sl[j].nal;
        u32 len = sl[j].len, zeros = 0;
        at[j] = wo;
        for (u32 i = 0; i < len; i++) {
            u8 c = s[i];
            if (!(skip & 128) && zeros >= 2 && c == 3 && i + 1 < len && s[i + 1] <= 3) {
                zeros = 0;
                continue;
            }
            zeros = c ? 0 : zeros + 1;
            d[wo++] = c;
        }
        dend[j] = wo;
        for (u32 i = 0; i < 64; i++)   /* zero tail: the prefetcher reads whole words past the last byte (without it
                                          the last 4x4 block of a noise frame came out wrong) */
            d[wo++] = 0;
        while (wo & 3)   /* the DPFU start is a word address */
            d[wo++] = 0;
    }
    plat_sync(EVB_ADDR, wo);
    crumb(3);   /* BG2D, the completion marker */
    /* completion: the status bits come back long before the macroblocks are done, so wait for the last pixel (bottom
     * right) to leave a 0x55 marker. Only that byte is marked; if the picture's own value there is 0x55 the wait runs
     * out (reported, harmless). */
    volatile u8 *lastpx = (volatile u8 *)(SLOT_LUMA(cur) + tiled(sps->width - 1, sps->height - 1));
    *lastpx = 0x55;
    __asm__ volatile("cctl %0, L1D_VA_WB\n\tmsync all" ::"r"((u32)lastpx & ~31u) : "memory");
    bg2d_set(IDX_LUMA(cur), SLOT_LUMA(cur), 1, 0);
    bg2d_set(IDX_CHROMA(cur), SLOT_CHROMA(cur), 1, 0);
    bg2d_set(IDX_MVC(cur), SLOT_MVC(cur), 1, 0);
    bg2d_set(0x3b, WORK_3B, 0, 1);   /* working buffers keep the init layout bits ("scan type != 0"), gp 0 (0x431a70) */
    bg2d_set(0x3c, WORK_3C, 0, 1);
    /* the slice's command block (notes/h264_decoder.md section 3.2) */
    ring_n = 0;
    for (int j = 0; j < ns; j++) {
        blk[j] = ring_n;
        const struct h264_slice *sh = &sl[j].sh;
        int nl0 = sh->slice_type == 0 ? (n0 < (int)sh->num_ref_idx_l0 ? n0 : (int)sh->num_ref_idx_l0) : 0;
        /* the slice header through the bit reader, then the slice-data position (H264_SliceHeaderEnd) */
        crumb(2);
        dpfu_flag = 0;
        /* the length is the slice's own bytes: counting the zero tail too, the decoder ran past the end of a slice
         * into the next one's macroblocks (multi-slice pictures desynced at each slice's first row with neighbors) */
        int dr = dpfu_start(at[j], dend[j] - at[j]);
        if (rd(8) != sl[j].nal[0])   /* the NAL header byte: checks the DPFU landed on the slice */
            pr->nal_bad++;
        for (u32 left = sh->data_bit_rbsp - 8; left; ) {
            u32 k = left > 31 ? 31 : left;
            rd((int)k);
            left -= k;
        }
        u32 save0 = M(0xd894), save1 = M(0xd898);
        M(0xd884) = 2u * (u32)dpfu_flag;   /* "stop" (0), or with flag 1 presumably hand over to the hardware */
        if (j == 0)
            pr->save0 = save0, pr->save1 = save1, pr->hdr_bits = sh->data_bit_rbsp, pr->dpfu = dr;
        if (!(skip & 1)) {   /* slice-data position: read from DPFU1 (D894/D898), handed to DPFU2 (D908/D90C), as eCos
                                0x43b302 does with ctx+0xAA45A = 1 (set by its decoder reset 0x4350c2); skip bit 64 = DPFU1 */
            w32((skip & 64) ? 0x9c00d888 : 0x9c00d908, save0);
            w32((skip & 64) ? 0x9c00d88c : 0x9c00d90c, save1);
        }
        w16(0x9c00d018, 0);                               /* field_pic_flag */
        w16(0x9c00d01c, 1);                               /* !bottom_field_flag */
        w16(0x9c00d090, sh->direct_spatial_mv_pred);
        w16(0x9c00d020, 0);                               /* MbaffFrameFlag */
        w16(0x9c00d014, pps->constrained_intra_pred);
        w16(0x9c00d080, sh->nal_ref_idc != 0);
        w16(0x9c00d084, pps->entropy_coding_mode);
        w16(0x9c00d010, ((u32)pps->chroma_qp_index_offset & 0x3f) | ((u32)pps->second_chroma_qp_index_offset & 0x3f) << 6);
        w16(0x9c00d0a0, (u32)sh->qp);
        w16(0x9c00d024, sh->slice_type);
        w16(0x9c00d008, sps->mb_width);
        w16(0x9c00d00c, sps->mb_height);
        {   /* first macroblock: row, column (experiments: skip 16 swaps them, 32 sends the address in D088) */
            u32 r0 = sh->first_mb / sps->mb_width, c0 = sh->first_mb % sps->mb_width;
            w16(0x9c00d088, r0);
            w16(0x9c00d08c, c0);

        }
        w16(0x9c00d094, sh->num_ref_idx_l0 - 1);
        w16(0x9c00d098, 0xffff);                          /* l1 for non-B: S+0x39 - 1 with S+0x39 = 0 */
        w16(0x9c00d0a4, 0);                               /* <= 16 frame buffers, no list 1 */
        w16(0x9c00d180, pps->weighted_pred);
        w16(0x9c00d184, pps->weighted_bipred_idc);
        w16(0x9c00d188, 0);
        w16(0x9c00d18c, 0);
        w16(0x9c00d800, sh->disable_deblocking);
        w16(0x9c00d804, (u32)sh->alpha_offset);
        w16(0x9c00d808, (u32)sh->beta_offset);
        w32(0x9c00d028, 0x3bu << 24 | 0x3cu << 16 | (u32)IDX_MVC(cur));
        w16(0x9c00d02c, (u32)(IDX_LUMA(cur) | IDX_CHROMA(cur) << 8));
        w16(0x9c00d09c, sh->cabac_init_idc);
        w16(0x9c00d004, (sps->profile_idc > 99) | (sps->chroma_format_idc & 3) << 1 | (pps->transform_8x8_mode & 1) << 3);
        if (!(skip & 4))
            for (u32 a = 0x9c00d190; a <= 0x9c00d19c; a += 4)
                w32(a, 0xffffffff);
        if (!(skip & 2)) {
            /* item 35: list 0 then list 1, 32 entries each, entry = luma index | chroma index << 8 (0x43b1c8..) */
            put(0x2040003f);
            for (int i = 0; i < 64; i++)
                put(i < nl0 ? (u32)(IDX_LUMA(l0[i]) | IDX_CHROMA(l0[i]) << 8) : 0);
            /* item 36: buffer index -> list position (0x43b15c..0x43b1c6: entry 2 * luma index + a field bit, the
             * list scanned from the end so the first position wins). For frames the bit's meaning is open, so both
             * entries of a frame get its position. */
            u32 inv[64] = { 0 };
            for (int i = nl0 - 1; i >= 0; i--)
                inv[2 * IDX_LUMA(l0[i])] = inv[2 * IDX_LUMA(l0[i]) + 1] = (u32)i;
            put(0x2000003f);
            for (int i = 0; i < 64; i++)
                put(inv[i]);
        }
        put(0xf8000000);                                  /* end of slice */
    }
    blk[ns] = ring_n;
    plat_sync(SPMP_ADDR, ring_n * 4);
    if (!kick)
        return 0;
    crumb(4);   /* kick: picture decode (h264_trigger_decode / real_decode_h264_video) */
    M(0xde6c) = 0x18;
    M(0xd980) = 1;
    for (u32 a = 0xdb00; a <= 0xdb7c; a += 4)
        M(a) = 0;
    M(0xd980) = 0;
    M(0xd0b4) = 0xfe;
    M(0xd034) = 1;
    M(0xd924) = EVB_KB << 8 | IDX_EVB;
    M(0xd54c) = SPMP_KB << 8 | IDX_SPMP;
    M(0xd548) = 1;
    unsigned long long t_kick = clk_us();
    M(0xd048) = 1, M(0xd1a4) = 0, M(0xd558) = 0, M(0xd55c) = 0, M(0xd564) = 0;
    M(0xd550) = 0x1f8;
    M(0xd550) = 0;
    /* the first slice's block: length in words, start word (0x43a2de); the engine finds the following blocks after it */
    M(0xd554) = blk[1] << 21 | 0;
    M(0xd558) = 1;
    M(0xd550) = 0;
    M(0xd550) = 1;
    crumb(45);
    /* start acknowledged (D550 bit 0; eCos waits on it with no timeout). It can be missed: once in ~15000 pictures it
     * never read 1 while the block had already completed (D570 0x2CB), so done (D570 bit 0) counts as acknowledged */
    int k1 = -1;
    for (u32 n = 0; n < 3000000; n++)
        if (M(0xd550) & 1) {
            k1 = 0;
            break;
        } else if (M(0xd570) & 1) {
            k1 = 0;
            vdec_stats.ack_missed++;   /* usual: 78% of 18000 pictures; harmless */
            break;
        }
    int k2 = k1 ? -1 : poll(0xd570, 1, 1, 3000000);
    crumb(47);
    pr->d570 = M(0xd570);
    M(0xd570) = 1;
    M(0xd550) = 0;
    if (ns != 1) {   /* the engine runs the following slices' blocks itself (0x41466a) */
        M(0xd558) = (u32)ns;
        M(0xd550) = 0;
    }
    int k3 = k2 ? -1 : poll(0xdc84, 1, 1, 3000000);
    crumb(48);
    u32 spins = 0;
    for (; spins < 2000000; spins++) {
        __asm__ volatile("cctl %0, L1D_VA_INVAL\n\tmsync all" ::"r"((u32)lastpx & ~31u) : "memory");
        if (*lastpx != 0x55)
            break;
    }
    pr->t_us = (u32)(clk_us() - t_kick);
    pr->spins = spins;
    pr->timeouts = (u32)(k1 & 1) | (u32)(k2 & 1) << 1 | (u32)(k3 & 1) << 2;
    CRUMB[4] = pr->timeouts, CRUMB[5] = pr->d570;
    crumb(49);
    if (pr->timeouts) {   /* stuck: its status registers stall the bus when read; reset it through MOON0 */
        vdec_hw_reset();
        return -1;
    }
    return 0;
}

static int frame_num_wrap(u32 fn, u32 cur_fn, u32 max_fn) { return fn > cur_fn ? (int)fn - (int)max_fn : (int)fn; }

/* Stream state, kept across access units (vdec_feed): parameter sets, the DPB, the last picture. */
static struct h264_sps sps;
static struct h264_pps pps;
static int last_slot = -1;

void vdec_stream_reset(void)
{
    memset(&sps, 0, sizeof sps);
    memset(&pps, 0, sizeof pps);
    for (int i = 0; i < NSLOT; i++)
        dpb[i].used = 0;
    last_slot = -1;
}

struct feed_out { u32 pics, t_sum, t_max; int err; };

/* Decode the NAL units in [s, s + len): SPS/PPS update the parameters; each slice starting at macroblock 0, with the
 * slices after it, is one picture. maxpics 0: all. Text (per-picture times, errors) goes to o; returns its length. */
static int feed(const u8 *s, u32 len, u32 maxpics, int kick, u32 skip, char *o, vdec_fmt fmt, struct feed_out *fo)
{
    u32 pos = 0, st, nl, pics = 0;
    int k = 0;
    fo->err = 1;
    u32 a380 = M(0xa380);   /* left alone: slot 0 (1920) is what the decoder uses, see FB_PITCH */
    while (h264_next_nal(s, len, &pos, &st, &nl)) {
        int t = s[st] & 31;
        if (t == 7) {
            if (h264_parse_sps(s + st, nl, &sps))
                return k + fmt(o + k, " ERR SPS");
            continue;
        }
        if (t == 8) {
            if (h264_parse_pps(s + st, nl, &pps))
                return k + fmt(o + k, " ERR PPS");
            continue;
        }
        if (t != 5 && t != 1)
            continue;
        /* a picture: this slice and the ones after it that do not start at macroblock 0 */
        static struct vslice sl[MAX_SLICES];
        int ns = 0, is_p = 0;
        u32 evb_need = 0;
        for (u32 p2 = pos, st2 = st, nl2 = nl;;) {
            struct h264_slice h2 = { 0 };
            if (!sps.width || h264_parse_slice(s + st2, nl2, &sps, &pps, &h2))
                return k + fmt(o + k, " ERR picture %u slice %d unparsed", pics, ns);
            if (ns && !h2.first_mb)
                break;
            if (!ns && h2.first_mb)
                return k + fmt(o + k, " ERR picture %u starts at macroblock %u", pics, h2.first_mb);
            if (h2.slice_type != 0 && h2.slice_type != 2)
                return k + fmt(o + k, " ERR picture %u: slice type %u (only I and P)", pics, h2.slice_type);
            if (h2.long_term_ref || h2.rplm_n > H264_MAXOPS || h2.mmco_n > H264_MAXOPS)
                return k + fmt(o + k, " ERR picture %u: long-term marking or too many ref ops (not yet)", pics);
            for (u32 i = 0; i < h2.rplm_n; i++)
                if (h2.rplm_idc[i] > 1)
                    return k + fmt(o + k, " ERR picture %u: long-term list modification (not yet)", pics);
            for (u32 i = 0; i < h2.mmco_n; i++)
                if (h2.mmco_op[i] != 1)
                    return k + fmt(o + k, " ERR picture %u: marking op %u (only 1 so far)", pics, h2.mmco_op[i]);
            if (ns && (h2.rplm_n != sl[0].sh.rplm_n || memcmp(h2.rplm_idc, sl[0].sh.rplm_idc, sizeof h2.rplm_idc) ||
                       memcmp(h2.rplm_val, sl[0].sh.rplm_val, sizeof h2.rplm_val)))
                return k + fmt(o + k, " ERR picture %u: slices with different list modifications (not yet)", pics);
            if (ns == MAX_SLICES || (evb_need += nl2 + 68) > EVB_KB * 1024)
                return k + fmt(o + k, " ERR picture %u: too many slices or bytes", pics);
            sl[ns].nal = s + st2, sl[ns].len = nl2, sl[ns].sh = h2;
            is_p |= h2.slice_type == 0;
            ns++;
            pos = p2;
            if (!h264_next_nal(s, len, &p2, &st2, &nl2) || ((s[st2] & 31) != 1 && (s[st2] & 31) != 5))
                break;
        }
        const struct h264_slice sh = sl[0].sh;
        if (sps.width > 960 || sps.height > 512)
            return k + fmt(o + k, " ERR %ux%u is larger than the slots", sps.width, sps.height);
        if (t == 5)
            for (int i = 0; i < NSLOT; i++)
                dpb[i].used = 0;
        /* list 0: short-term frames by descending FrameNumWrap (8.2.4.2.1) */
        u32 max_fn = 1u << sps.log2_max_frame_num;
        int l0[NSLOT], n0 = 0;
        for (int i = 0; i < NSLOT; i++)
            if (dpb[i].used) {
                int w = frame_num_wrap(dpb[i].frame_num, sh.frame_num, max_fn), j = n0++;
                for (; j > 0 && frame_num_wrap(dpb[l0[j - 1]].frame_num, sh.frame_num, max_fn) < w; j--)
                    l0[j] = l0[j - 1];
                l0[j] = i;
            }
        if (sh.rplm_n) {   /* 8.2.4.3.1, short-term only: each op puts the frame it names at the next index */
            int n = (int)sh.num_ref_idx_l0 < NSLOT ? (int)sh.num_ref_idx_l0 : NSLOT, lst[NSLOT + 1], ri = 0;
            for (int i = 0; i <= n; i++)
                lst[i] = i < n0 ? l0[i] : -1;
            int pred = (int)sh.frame_num;
            for (u32 i = 0; i < sh.rplm_n; i++) {
                int d = (int)sh.rplm_val[i] + 1, nw = sh.rplm_idc[i] == 0 ? pred - d : pred + d;
                if (nw < 0)
                    nw += (int)max_fn;
                else if (nw >= (int)max_fn)
                    nw -= (int)max_fn;
                pred = nw;
                int pn = nw > (int)sh.frame_num ? nw - (int)max_fn : nw, f = -1;
                for (int j = 0; j < NSLOT; j++)
                    if (dpb[j].used && frame_num_wrap(dpb[j].frame_num, sh.frame_num, max_fn) == pn)
                        f = j;
                if (f < 0 || ri >= n)
                    return k + fmt(o + k, " ERR picture %u: list modification names a missing frame (%d)", pics, pn);
                for (int c = n; c > ri; c--)
                    lst[c] = lst[c - 1];
                lst[ri++] = f;
                int ni = ri;
                for (int c = ri; c <= n; c++)
                    if (lst[c] != f)
                        lst[ni++] = lst[c];
            }
            for (n0 = 0; n0 < n && lst[n0] >= 0; n0++)
                l0[n0] = lst[n0];
        }
        if (is_p && !n0)
            return k + fmt(o + k, " ERR picture %u: P without a reference", pics);
        int cur = 0;
        while (cur < NSLOT && dpb[cur].used)
            cur++;
        if (cur == NSLOT)
            return k + fmt(o + k, " ERR no free slot");
        struct pic_result pr = { 0 };
        int r = decode_pic(sl, ns, &sps, &pps, cur, l0, n0, kick, skip, &pr);
        M(0xa380) = a380;
        if (!kick) {
            crumb_clear();
            return k + fmt(o + k, " prepared picture %u (header %u bits, DPFU %d, save %x/%x, ring %u words, slot %d, %d refs)",
                           pics, pr.hdr_bits, pr.dpfu, pr.save0, pr.save1, ring_n, cur, n0);
        }
        if (r) {
            crumb_clear();
            return k + fmt(o + k, " picture %u TIMEOUT (start/done/end bits %x, D570 %x); decoder reset through MOON0, init again",
                           pics, pr.timeouts, pr.d570);
        }
        if (pics < 24)
            k += fmt(o + k, " %c%u/s%d/%u", is_p ? 'P' : 'I', sh.frame_num, cur, pr.t_us);
        if (ns > 1 && pics < 24)
            k += fmt(o + k, "x%d", ns);
        if (pr.spins >= 2000000)
            k += fmt(o + k, "(marker)");
        if (pr.nal_bad)
            k += fmt(o + k, "(%u slices misplaced in the EVB)", pr.nal_bad);
        fo->t_sum += pr.t_us;
        if (pr.t_us > fo->t_max)
            fo->t_max = pr.t_us;
        if (sh.nal_ref_idc && sh.adaptive_marking) {   /* 8.2.5.4, op 1 only: unmark the short-term frame named */
            for (u32 i = 0; i < sh.mmco_n; i++) {
                int pn = (int)sh.frame_num - (int)(sh.mmco_val[i] + 1);
                for (int j = 0; j < NSLOT; j++)
                    if (dpb[j].used && frame_num_wrap(dpb[j].frame_num, sh.frame_num, max_fn) == pn)
                        dpb[j].used = 0;
            }
            dpb[cur].used = 1, dpb[cur].frame_num = sh.frame_num;
        } else if (sh.nal_ref_idc) {   /* sliding window */
            u32 nmax = sps.num_ref_frames ? sps.num_ref_frames : 1, nused = 0;
            for (int i = 0; i < NSLOT; i++)
                nused += dpb[i].used;
            if (nmax > NSLOT - 1)
                nmax = NSLOT - 1;
            while (nused >= nmax) {
                int old = -1;
                for (int i = 0; i < NSLOT; i++)
                    if (dpb[i].used && (old < 0 || frame_num_wrap(dpb[i].frame_num, sh.frame_num, max_fn) <
                                                       frame_num_wrap(dpb[old].frame_num, sh.frame_num, max_fn)))
                        old = i;
                dpb[old].used = 0;
                nused--;
            }
            dpb[cur].used = 1, dpb[cur].frame_num = sh.frame_num;
        }
        last_slot = cur;
        pics++;
        fo->pics = pics;
        vdec_stats.decodes++;
        vdec_stats.last_us = pr.t_us;
        if (maxpics && pics >= maxpics)
            break;
    }
    crumb_clear();
    fo->err = 0;
    return k;
}

/* op 19/3: decode the uploaded stream from a fresh state; the last picture stays in its slot for reading back (the reply
 * names the addresses). */
int vdec_decode(u32 src, u32 len, char *o, vdec_fmt fmt, int kick, u32 skip, u32 maxpics)
{
    if (!inited)
        return fmt(o, "ERR vdec: init first");
    if (len < 16 || len > 0x400000)
        return fmt(o, "ERR vdec: 16 bytes .. 4 MB");
    vdec_stream_reset();
    struct feed_out fo = { 0 };
    int k = fmt(o, "vdec decode (skip %x):", skip);
    k += feed((const u8 *)src, len, maxpics, kick, skip, o + k, fmt, &fo);
    if (fo.err || !kick)
        return k;
    if (!fo.pics)
        return k + fmt(o + k, " ERR no picture");
    cache_inval(SLOT_LUMA(last_slot), FB_LUMA_N);
    cache_inval(SLOT_CHROMA(last_slot), FB_CHROMA_N);
    return k + fmt(o + k, "; %u pictures (us each, mean %u, max %u); last in slot %d: luma %x chroma %x", fo.pics,
                   fo.t_sum / fo.pics, fo.t_max, last_slot, SLOT_LUMA(last_slot), SLOT_CHROMA(last_slot));
}

/* One access unit of a running stream (Android Auto video): decoded at once, state kept. 0 ok; on error the text is in
 * o and the stream restarts at the next IDR. */
int vdec_feed(const unsigned char *p, unsigned int n, char *o, vdec_fmt fmt)
{
    if (!inited) {   /* after a reset through MOON0 (a stuck picture): init again, carry on from the next IDR */
        char m[200];
        vdec_init(m, fmt);
        vdec_stats.reinits++;
        for (int i = 0; i < NSLOT; i++)
            dpb[i].used = 0;
    }
    struct feed_out fo = { 0 };
    feed(p, n, 0, 1, 0, o, fmt, &fo);
    if (fo.err) {
        vdec_stats.errors++;
        for (int i = 0; i < NSLOT; i++)   /* references are gone: P pictures fail until the next IDR */
            dpb[i].used = 0;
        return -1;
    }
    return 0;
}

int vdec_last_slot(unsigned int *luma, unsigned int *chroma)
{
    if (last_slot < 0)
        return -1;
    *luma = SLOT_LUMA(last_slot), *chroma = SLOT_CHROMA(last_slot);
    return last_slot;
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
