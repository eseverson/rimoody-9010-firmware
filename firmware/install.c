/* Install to flash: the boot path (stage 1) and a copy of the payload in NOR (stage 2). install.h has the layout,
 * notes/nor_install.md the design, every check and the power-loss analysis.
 *
 * Planning is pure and reads flash only through a callback, so test/install_host.c runs the same code against flash
 * dumps. Nothing here writes flash directly: stage 1 goes through nor_boot_write() (nor.c re-reads each block and
 * checks it against the plan), stage 2 through nor_erase() / nor_program(), which only accept the free region. No U-Boot
 * code runs anywhere in these paths, so U-Boot's `sfsp` state at DRAM 0x800 (overwritten by the decoder ring) is never
 * used. */
#include "install.h"
#include "nor.h"
#include "crypto.h"

typedef unsigned int u32;
typedef unsigned char u8;

#define IH_MAGIC 0x27051956u
#define BLK      0x10000u

/* U-Boot's built-in environment, stock and as stage 1 leaves it (tools/patch_uboot.py ENV_OLD / ENV_NEW, NUL-padded to
 * the same 44 bytes). `baudrate` goes: U-Boot falls back to the compiled 921600, the same value. */
static const u8 env_old[INST_ENV_LEN] = "bootcmd=bootsp\0bootdelay=0\0baudrate=921600\0";
static const u8 env_new[INST_ENV_LEN] = "bootcmd=source 98e00000;bootsp\0bootdelay=0\0";
_Static_assert(sizeof "bootcmd=bootsp\0bootdelay=0\0baudrate=921600\0" == INST_ENV_LEN, "stock env is 44 bytes");

/* The version 2 boot script. Lines 1-4, the card loop and the last line are the SD-only script (isp/bootcmd/
 * boot.txt) unchanged, plus `setenv hu_src sd`. The NOR copy is taken only if the SD card gave no BOOT.BIN, its header
 * word is "HUNC" (programmed last), the CRC of the whole region past the header (image, then 0xFF) matches the header,
 * and the RAM copy has the same CRC. `crc32 addr len dst` stores the CRC as a native word (U-Boot 0xF0C870, `swi`), and
 * the scratch words start at 0 so a store that did not happen cannot match. Any failure leaves hu_ok unset, the script
 * ends, and bootcmd's `;bootsp` starts stock eCos. The region is read and copied whole (0x18FF00 bytes): no length is
 * taken from flash, so the script needs no setexpr and a header that lies about its length cannot steer it. */
static const char script_text[] =
    "mw.l 0x9c0000a4 0x1fe00220\n"
    "uartsp init 1 115200\n"
    "mw.l 0x9c000980 0xff;mw.l 0x9c000980 0xaa;mw.l 0x9c000980 0x02;mw.l 0x9c000980 0xd1;mw.l 0x9c000980 0x00;"
    "mw.l 0x9c000980 0x2d;mw.l 0x9c000980 0xbe;mw.l 0x9c000980 0xba\n"
    "mw.l 0x9c00008c 0x00f00050;mw.l 0x9c000100 0x00fc00fc;mw.l 0x9c00010c 0x00fc00fc\n"
    "setenv isp_if mmc\n"
    "setenv isp_dev 0\n"
    "setenv hu_ok\n"
    "setenv hu_src\n"
    "setenv hu_nor\n"
    "mmc dev 0\n"
    "mmc rescan\n"
    "if fatload mmc 0 0x1800000 /BOOT.BIN; then setenv hu_ok 1; setenv hu_src sd; else mmc dev 1; mmc rescan; "
    "if fatload mmc 1 0x1800000 /BOOT.BIN; then setenv isp_dev 1; setenv hu_ok 1; setenv hu_src sd; fi; fi\n"
    "if test -z \"$hu_ok\"; then mw.l 0x33f0000 0x434e5548 1; mw.l 0x33f0004 0 2; "
    "if cmp.l 0x98c70000 0x33f0000 1; then setenv hu_nor 1; fi; fi\n"
    "if test -n \"$hu_nor\"; then setenv hu_nor; crc32 0x98c70100 0x18ff00 0x33f0004; "
    "if cmp.l 0x33f0004 0x98c70010 1; then setenv hu_nor 1; fi; fi\n"
    "if test -n \"$hu_nor\"; then setenv hu_nor; cp.l 0x98c70100 0x1800000 0x63fc0; crc32 0x1800000 0x18ff00 0x33f0008; "
    "if cmp.l 0x33f0008 0x98c70010 1; then setenv hu_ok 1; setenv hu_src nor; fi; fi\n"
    "if test -n \"$hu_ok\"; then crc32 0x00f00000 0x100000; wdt stop all; go 0x1800000 hello; go 0x1800000 main; fi\n";
_Static_assert(INST_COPY_MAX == 0x18ff00u && INST_COPY_MAX / 4 == 0x63fc0u, "script constants");
_Static_assert(INST_SCRATCH == 0x033f0000u && INST_COPY_MAGIC == 0x434e5548u, "script constants");

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_le32(u8 *p, u32 v) { p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24); }
static void put_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }
static void fill(u8 *d, u8 c, u32 n) { while (n--) *d++ = c; }
static void copy(u8 *d, const u8 *s, u32 n) { while (n--) *d++ = *s++; }
static int same(const u8 *a, const u8 *b, u32 n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static u32 blk_base(u32 a) { return a & ~(BLK - 1); }
static u32 first_not(const u8 *p, u8 c, u32 n) { u32 i = 0; while (i < n && p[i] == c) i++; return i; }   /* n: none */

unsigned inst_crc(unsigned crc, const unsigned char *p, unsigned n)
{
    while (n) {   /* crc32_update takes an int */
        u32 k = n > 0x40000000u ? 0x40000000u : n;
        crc = crc32_update(crc, p, (int)k);
        p += k, n -= k;
    }
    return crc;
}

const char *inst_script_text(void) { return script_text; }

unsigned inst_script_image(unsigned char *out)   /* isp/mkisp.py mkscript() with the time stamp 0 */
{
    static const char name[] = INST_SCRIPT_NAME;
    u32 tl = sizeof script_text - 1, size = 8 + tl;
    u8 *pl = out + 64;
    put_be32(pl, tl), put_be32(pl + 4, 0);
    copy(pl + 8, (const u8 *)script_text, tl);
    fill(out, 0, 64);
    put_be32(out, IH_MAGIC);
    put_be32(out + 12, size);
    put_be32(out + 24, inst_crc(0, pl, size));
    out[28] = 5, out[29] = 20, out[30] = 6, out[31] = 0;   /* os, arch NDS32, type script, no compression */
    copy(out + 32, (const u8 *)name, sizeof name - 1);
    put_be32(out + 4, inst_crc(0, out, 64));
    return 64 + size;
}

/* xboot's checks of the U-Boot uImage header (isp/mkisp.py check_stock_uboot): word sums of little-endian words,
 * stored big-endian. Stricter than xboot: the 0x5A "verify" flag must be set, since a header without it proves nothing
 * about the layout. */
static u32 hdr_sum(const u8 *h)
{
    u32 s = 0;
    for (u32 i = 0; i < 64; i += 4)
        s += i == 4 ? 0 : le32(h + i);
    return s;
}

static const char *xboot_hdr(const u8 *h)
{
    if (be32(h) != IH_MAGIC)
        return "no U-Boot uImage at the expected address";
    u32 size = be32(h + 12);
    if (size < 0x1000 || size > NOR_UBOOT_MAX)
        return "U-Boot image size out of range";
    if (h[0x1E] != 0x5A)
        return "U-Boot header has no xboot verify flag";
    if (hdr_sum(h) != be32(h + 4))
        return "U-Boot header word sum does not match";
    return 0;
}

/* our script in the 64 KB script block? OURS: a valid script uImage named "nor boot script" (version 1) or
 * "nor boot script vN", nothing but 0xFF after it. PARTIAL: an interrupted stage 1 write of `want` (page 0 is
 * programmed last, so it is blank, and every other page is blank or equals want's). */
int inst_script_class(const unsigned char *blk, const unsigned char *want, int *ver, unsigned *hcrc)
{
    static const char nm[] = "nor boot script";
    *ver = 0, *hcrc = 0;
    if (first_not(blk, 0xff, BLK) == BLK)
        return INST_SCR_BLANK;
    if (be32(blk) == IH_MAGIC) {
        u8 h[64];
        copy(h, blk, 64);
        put_be32(h + 4, 0);
        u32 size = be32(blk + 12);
        int v = -1;
        if (same(blk + 32, (const u8 *)nm, sizeof nm - 1)) {
            const u8 *t = blk + 32 + sizeof nm - 1;
            u32 rest = 32 - (sizeof nm - 1), k = 0;
            if (t[0] == 0)
                v = 1;
            else if (t[0] == ' ' && t[1] == 'v' && t[2] >= '1' && t[2] <= '9') {
                v = 0;
                for (k = 2; k < rest && t[k] >= '0' && t[k] <= '9' && v < 1000; k++)
                    v = v * 10 + (t[k] - '0');
            }
            for (u32 i = v == 1 ? 0 : k; v >= 0 && i < rest; i++)
                if (t[i])
                    v = -1;
        }
        if (v >= 1 && inst_crc(0, h, 64) == be32(blk + 4) && size >= 9 && size <= BLK - 64 &&
            inst_crc(0, blk + 64, size) == be32(blk + 24) && blk[30] == 6 && be32(blk + 64) != 0 &&
            first_not(blk + 64 + size, 0xff, BLK - 64 - size) == BLK - 64 - size) {
            *ver = v, *hcrc = be32(blk + 4);
            return INST_SCR_OURS;
        }
    }
    if (want && first_not(blk, 0xff, 0x100) == 0x100) {
        for (u32 p = 0x100; p < BLK; p += 0x100)
            if (first_not(blk + p, 0xff, 0x100) != 0x100 && !same(blk + p, want + p, 0x100))
                return INST_SCR_FOREIGN;
        return INST_SCR_PARTIAL;
    }
    return INST_SCR_FOREIGN;
}

/* a reader that returns the flash with the planned blocks in place: the planned result is checked like live flash */
struct overlay {
    inst_rd rd;
    void *ctx;
    const struct inst_boot *b;
};

static int rd_overlay(void *ctx, unsigned off, void *dst, unsigned len)
{
    struct overlay *o = ctx;
    int r = o->rd(o->ctx, off, dst, len);
    if (r)
        return r;
    for (int k = 0; k < o->b->nblk; k++) {
        u32 lo = o->b->blk_off[k], hi = lo + BLK;
        u32 a = off > lo ? off : lo, z = off + len < hi ? off + len : hi;
        if (a < z)
            copy((u8 *)dst + (a - off), o->b->blk[k] + (a - lo), z - a);
    }
    return 0;
}

/* walk the U-Boot body: xboot's data word sum and every place the stock or the patched environment occurs */
struct uwalk {
    u32 dsum, n_old, n_new, at_old, at_new;
};

static int walk_uboot(inst_rd rd, void *ctx, u32 size, u8 *buf, struct uwalk *w)
{
    u32 lo = NOR_UBOOT_HDR + 64, hi = lo + size;
    w->dsum = w->n_old = w->n_new = 0, w->at_old = w->at_new = 0;
    for (u32 c = lo; c < hi; c += BLK) {
        u32 n = hi - c < BLK + INST_ENV_LEN - 1 ? hi - c : BLK + INST_ENV_LEN - 1, own = n < BLK ? n : BLK;
        if (rd(ctx, c, buf, n))
            return -1;
        for (u32 i = 0; i < own; i++)   /* the body starts 4-aligned: byte i of it is byte (c + i) & 3 of a word */
            w->dsum += (u32)buf[i] << 8 * ((c - lo + i) & 3);
        for (u32 i = 0; i < own && i + INST_ENV_LEN <= n; i++) {
            if (buf[i] != 'b')
                continue;
            if (same(buf + i, env_old, INST_ENV_LEN))
                w->n_old++, w->at_old = c + i;
            if (same(buf + i, env_new, INST_ENV_LEN))
                w->n_new++, w->at_new = c + i;
        }
    }
    return 0;
}

int inst_plan_boot(inst_rd rd, void *ctx, unsigned char *work, struct inst_boot *b)
{
    u8 *rb = work + INST_MAX_BLK * BLK, h[64];
    fill((u8 *)b, 0, sizeof *b);
    if (rd(ctx, NOR_UBOOT_HDR, h, 64))
        return b->refuse = "flash read failed", -1;
    const char *e = xboot_hdr(h);
    if (e)
        return b->refuse = e, -1;
    b->body = be32(h + 12), b->hsum = be32(h + 4), b->dsum = be32(h + 24);
    struct uwalk w;
    if (walk_uboot(rd, ctx, b->body, rb, &w))
        return b->refuse = "flash read failed", -1;
    if (w.dsum != b->dsum)
        return b->refuse = "U-Boot body word sum does not match", -1;
    if (w.n_old + w.n_new == 0)
        return b->env_state = INST_ENV_NONE, b->refuse = "U-Boot default environment not found", -1;
    if (w.n_old + w.n_new > 1)
        return b->env_state = INST_ENV_MANY, b->refuse = "U-Boot default environment found more than once", -1;
    b->env_state = w.n_old ? INST_ENV_STOCK : INST_ENV_PATCHED;
    b->env_off = w.n_old ? w.at_old : w.at_new;
    if (!nor_boot_block_ok(blk_base(b->env_off), b->env_off, INST_ENV_LEN))
        return b->refuse = "U-Boot environment outside the blocks stage 1 may write", -1;

    /* the script block: the new one in slot 0, the live one read fresh */
    u8 *ns = work;
    fill(ns, 0xff, BLK);
    inst_script_image(ns);
    if (rd(ctx, NOR_SCRIPT_BLK, rb, BLK))
        return b->refuse = "flash read failed", -1;
    b->scr_state = inst_script_class(rb, ns, &b->scr_ver, &b->scr_crc);
    if (b->scr_state == INST_SCR_FOREIGN)
        return b->refuse = "the boot script block holds something that is not ours", -1;
    if (b->scr_state == INST_SCR_OURS && b->scr_ver > (int)INST_SCRIPT_VER)
        return b->refuse = "a newer boot script is installed; this build would downgrade it", -1;
    int k = 0;
    if (!same(rb, ns, BLK)) {
        b->blk_off[k] = NOR_SCRIPT_BLK, b->blk_live_crc[k] = inst_crc(0, rb, BLK), b->blk[k] = ns;
        b->blk_new_crc[k] = inst_crc(0, ns, BLK);
        if (!nor_boot_diff_ok(NOR_SCRIPT_BLK, rb, ns, b->env_off, INST_ENV_LEN))
            return b->refuse = "internal: new script block rejected", -1;
        k++;
    }

    b->new_hsum = b->hsum, b->new_dsum = b->dsum;
    if (b->env_state == INST_ENV_STOCK) {
        u32 eo = b->env_off, lo = NOR_UBOOT_HDR + 64, hb = blk_base(NOR_UBOOT_HDR);
        if (eo + INST_ENV_LEN > lo + (b->body & ~3u))
            return b->refuse = "U-Boot environment in the body's last partial word", -1;
        u32 d = b->dsum;
        for (u32 i = 0; i < INST_ENV_LEN; i++)
            d += ((u32)env_new[i] - env_old[i]) << 8 * ((eo + i - lo) & 3);
        u8 nh[64];
        copy(nh, h, 64);
        put_be32(nh + 24, d);
        put_be32(nh + 4, hdr_sum(nh));
        b->new_dsum = d, b->new_hsum = be32(nh + 4);
        /* environment block(s) first, the header block last: the order tools/flash_bootcmd.py used */
        u32 cand[3], nc = 0, e1 = blk_base(eo), e2 = blk_base(eo + INST_ENV_LEN - 1);
        if (e1 != hb)
            cand[nc++] = e1;
        if (e2 != hb && e2 != e1)
            cand[nc++] = e2;
        cand[nc++] = hb;
        for (u32 c = 0; c < nc; c++) {
            u32 off = cand[c];
            u8 *nb = work + k * BLK;
            if (rd(ctx, off, nb, BLK))
                return b->refuse = "flash read failed", -1;
            b->blk_off[k] = off, b->blk_live_crc[k] = inst_crc(0, nb, BLK), b->blk[k] = nb;
            for (u32 i = 0; i < INST_ENV_LEN; i++)
                if (eo + i >= off && eo + i < off + BLK)
                    nb[eo + i - off] = env_new[i];
            for (u32 i = 0; i < 64; i++)
                if (NOR_UBOOT_HDR + i >= off && NOR_UBOOT_HDR + i < off + BLK)
                    nb[NOR_UBOOT_HDR + i - off] = nh[i];
            if (rd(ctx, off, rb, BLK))
                return b->refuse = "flash read failed", -1;
            if (!nor_boot_diff_ok(off, rb, nb, eo, INST_ENV_LEN))
                return b->refuse = "internal: a U-Boot block would change outside the environment and sums", -1;
            b->blk_new_crc[k] = inst_crc(0, nb, BLK);
            k++;
        }
        b->nblk = k;
        /* the result, read back through the plan, must pass the same checks as the live flash did */
        struct overlay o = { rd, ctx, b };
        u8 rh[64];
        if (rd_overlay(&o, NOR_UBOOT_HDR, rh, 64) || walk_uboot(rd_overlay, &o, b->body, rb, &w))
            return b->refuse = "flash read failed", -1;
        if (xboot_hdr(rh) || w.dsum != be32(rh + 24) || w.n_old || w.n_new != 1 || w.at_new != eo)
            return b->refuse = "internal: the patched U-Boot would fail the xboot check", -1;
    }
    b->nblk = k;
    return 0;
}

int inst_stage1_done(const struct inst_boot *b)
{
    return !b->refuse && b->env_state == INST_ENV_PATCHED && b->scr_state == INST_SCR_OURS && b->scr_ver >= 2;
}

/* stage 2 */
static int copy_hdr_ours(const u8 *h)
{
    u32 m = le32(h + CH_MAGIC), len = le32(h + CH_LEN);
    if ((m != INST_COPY_MAGIC && m != 0xffffffffu) || le32(h + CH_VERSION) != INST_COPY_VERSION ||
        le32(h + CH_HDR_LEN) != INST_COPY_HDR || len < 0x100 || len > INST_COPY_MAX)
        return 0;
    if (inst_crc(0, h + 4, CH_HCRC - 4) != le32(h + CH_HCRC))
        return 0;
    for (u32 i = 0; i < 56; i++)
        if (!h[CH_BUILD + i])
            return 1;
    return 0;
}

int inst_region(inst_rd rd, void *ctx, unsigned char *buf, struct inst_copy *c)
{
    fill((u8 *)c, 0, sizeof *c);
    u8 h[INST_COPY_HDR];
    u32 crc = 0, bad = 0, ours = 0, tail = 0, any = 0;
    for (u32 k = 0; k < INST_COPY_BLOCKS; k++) {
        u32 off = INST_COPY_OFF + k * BLK;
        if (rd(ctx, off, buf, BLK))
            return c->refuse = "flash read failed", -1;
        if (k == 0) {
            copy(h, buf, INST_COPY_HDR);
            ours = copy_hdr_ours(h);
            tail = INST_COPY_IMG + (ours ? le32(h + CH_LEN) : 0);
        }
        u32 skip = k == 0 ? INST_COPY_HDR : 0;
        crc = inst_crc(crc, buf + skip, BLK - skip);
        if (first_not(buf, 0xff, BLK) != BLK)
            c->erase_mask |= 1u << k, any = 1;
        /* the first byte that a copy described by the header cannot explain: anything non-0xFF from the tail on */
        for (u32 i = 0; !bad && i < BLK; i++)
            if (buf[i] != 0xff && (!ours || off + i >= tail))
                bad = off + i + 1;
    }
    c->span_crc_now = crc;
    if (!any)
        return c->state = INST_REG_BLANK, 0;
    if (!ours || bad) {
        c->first_bad = bad ? bad - 1 : INST_COPY_OFF;
        return c->state = INST_REG_FOREIGN, 0;
    }
    c->old_len = le32(h + CH_LEN), c->old_seq = le32(h + CH_SEQ), c->old_span_crc = le32(h + CH_SPAN_CRC);
    copy((u8 *)c->old_build, h + CH_BUILD, 56);
    c->state = le32(h + CH_MAGIC) == INST_COPY_MAGIC ? INST_REG_COPY : INST_REG_PENDING;
    c->old_ok = c->state == INST_REG_COPY && crc == c->old_span_crc;
    return 0;
}

int inst_image_ok(const unsigned char *img, unsigned len, char *build)
{
    if (len < 0x100 || len > INST_COPY_MAX)
        return 1;
    if (le32(img + 0x40) != 0x454e5548u || le32(img + 0x44) != 1)   /* "HUNE" v1 (head.S) */
        return 2;
    u32 i = 0;
    while (i < 55 && img[0x48 + i] >= 0x20 && img[0x48 + i] < 0x7f)
        build[i] = (char)img[0x48 + i], i++;
    if (i == 0 || img[0x48 + i] != 0)
        return 3;
    for (; i < 56; i++)
        build[i] = 0;
    return 0;
}

int inst_plan_copy(const unsigned char *img, unsigned len, const struct inst_boot *b, struct inst_copy *c)
{
    static const char *const why[] = { "", "image size out of range (0x100 .. 0x18ff00 bytes)",
                                       "image has no HUNE v1 header", "image build id unreadable" };
    c->refuse = 0;
    if (b->refuse)
        return c->refuse = b->refuse, -1;
    if (!inst_stage1_done(b))
        return c->refuse = "stage 1 is not installed: the boot script would never load the copy", -1;
    if (c->state == INST_REG_FOREIGN)
        return c->refuse = "the copy region holds data that is not ours", -1;
    int e = inst_image_ok(img, len, c->build);
    if (e)
        return c->refuse = why[e], -1;
    static const u8 ff[256] = { [0 ... 255] = 0xff };
    u32 crc = inst_crc(0, img, len), span = crc;
    for (u32 left = INST_COPY_MAX - len; left;) {
        u32 n = left < sizeof ff ? left : sizeof ff;
        span = inst_crc(span, ff, n), left -= n;
    }
    if (span == 0)   /* the boot script compares against scratch words that start at 0 */
        return c->refuse = "span CRC is 0", -1;
    c->len = len, c->crc = crc, c->span_crc = span;
    c->seq = c->state == INST_REG_BLANK ? 1 : c->old_seq + 1;
    u8 *h = c->hdr;
    fill(h, 0, INST_COPY_HDR);
    put_le32(h + CH_MAGIC, INST_COPY_MAGIC);
    put_le32(h + CH_VERSION, INST_COPY_VERSION);
    put_le32(h + CH_LEN, len);
    put_le32(h + CH_CRC, crc);
    put_le32(h + CH_SPAN_CRC, span);
    put_le32(h + CH_SEQ, c->seq);
    put_le32(h + CH_HDR_LEN, INST_COPY_HDR);
    copy(h + CH_BUILD, (const u8 *)c->build, 56);
    put_le32(h + CH_HCRC, inst_crc(0, h + 4, CH_HCRC - 4));
    return 0;
}


#ifndef INSTALL_HOST
/* ---- device: runs in the net task ---- */
#include "memmap.h"
#include "clk.h"
#include "irq.h"

extern const char hu_build_id[];
extern u8 __hu_data_start[], __hu_data_end[], __hu_image_end[];   /* payload.ld */

#define IMG  ((u8 *)MM_INSTALL)                   /* the image being installed, INST_COPY_MAX */
#define WORK ((u8 *)(MM_INSTALL + 0x190000u))     /* inst_plan_boot work: new blocks, then the read buffer */
#define RBUF (WORK + INST_MAX_BLK * BLK)          /* 64 KB + 64: reads; nor_boot_write's live copy */
_Static_assert(0x190000u >= INST_COPY_MAX && 0x190000u + INST_WORK_SZ <= MM_INSTALL_SZ, "MM_INSTALL layout");
_Static_assert(INST_MAX_BLK * BLK + BLK + 64 <= INST_WORK_SZ, "work buffer");

static int rd_nor(void *ctx, unsigned off, void *dst, unsigned len)
{
    (void)ctx;
    irq_pet();
    return nor_read(off, dst, len, 0x0b);   /* fast read: what the driver verifies with (notes/nor_flash.md) */
}

static struct inst_boot pb;   /* the last boot plan (status, or the one being written) */
static struct inst_copy pc;   /* the last region scan, or the copy being written */
static int have_status;
static int job;               /* 0 idle, 1 stage 1, 2 stage 2 */
static int jphase, jidx, jdone, jtotal, jres;   /* jres: 0 none yet, 1 ok, -1 failed */
static int s1_resume = -1;    /* stage 1 failed at this step: pb (and its blocks in WORK) is kept for a retry */
static u32 jt0, jms, want_seq;
static char jmsg[160];
static int boot_src;
static volatile int req;
static char lines[2][128];
static volatile int line_cur;

void install_set_boot_src(int src) { boot_src = src; }
void install_request(int stage) { req = stage; }
int install_busy(void) { return job != 0; }

static void text(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i])
        d[i] = s[i], i++;
    d[i] = 0;
}

static const char *scr_name(const struct inst_boot *b)
{
    switch (b->scr_state) {
    case INST_SCR_BLANK: return "blank";
    case INST_SCR_PARTIAL: return "interrupted stage 1 write";
    case INST_SCR_FOREIGN: return "FOREIGN";
    }
    return b->scr_ver == 1 ? "version 1 (SD card only)" : "ours";
}

static void refresh(void)   /* read-only: plan stage 1 (unless a retry holds the plan) and scan the copy region */
{
    if (s1_resume < 0)
        inst_plan_boot(rd_nor, 0, WORK, &pb);
    inst_region(rd_nor, 0, RBUF, &pc);
    have_status = 1;
}

static void set_line(void)
{
    char *l = lines[!line_cur];
    int k;
    if (job)
        k = plat_fmt(l, "Installing the %s: step %d of %d. Keep the power on.", job == 1 ? "boot path" : "firmware copy",
                     jdone + 1, jtotal);
    else if (s1_resume >= 0)
        k = plat_fmt(l, "Boot path install FAILED half way: keep the power on and press Set up again");
    else if (!have_status)
        k = plat_fmt(l, "Checking the flash...");
    else if (pb.refuse)
        k = plat_fmt(l, "Install not possible: %s", pb.refuse);
    else if (!inst_stage1_done(&pb))
        k = plat_fmt(l, pb.env_state == INST_ENV_PATCHED ? "Boot: SD card, then stock (old boot script)"
                                                         : "Boot: stock only (boot path not set up)");
    else if (pc.state == INST_REG_COPY && pc.old_ok)
        k = plat_fmt(l, "Boot: SD card, flash copy (build %s, #%u), stock", pc.old_build, pc.old_seq);
    else
        k = plat_fmt(l, "Boot: SD card, then stock (no flash copy)");
    if (!job && jres)
        plat_fmt(l + k, jres > 0 ? ". Last install ok." : ". Last install failed: %s", jmsg);
    l[127] = 0;
    line_cur = !line_cur;
}

int install_line(char *out, int max)
{
    text(out, lines[line_cur], max);
    return 0;
}

static void fail(const char *what, int r)
{
    char d[96];
    text(d, jmsg, sizeof d);   /* a step may have left the details in jmsg */
    plat_fmt(jmsg, "%s: %s (step %d of %d)%s%s", what, r <= -1 && r >= -10 ? nor_strerror(r) : "check failed", jdone + 1,
             jtotal, d[0] ? "; " : "", d);
    if (job == 1)
        s1_resume = jdone;   /* U-Boot may be half written: keep the plan, a retry continues from here */
    job = 0, jres = -1, jms = clk_ms() - jt0;
    refresh();
    set_line();
}

/* the source must be the running payload: same length, same bytes except .data (which changes once it runs) */
static const char *running_check(const u8 *img, u32 len)
{
    const u8 *ram = (const u8 *)MM_PAYLOAD;
    u32 ds = (u32)__hu_data_start - MM_PAYLOAD, de = (u32)__hu_data_end - MM_PAYLOAD;
    if (len != (u32)__hu_image_end - MM_PAYLOAD)
        return "BOOT.BIN is not the running build (length differs)";
    for (u32 i = 0; i < len; i++)
        if ((i < ds || i >= de) && img[i] != ram[i])
            return "BOOT.BIN is not the running build (bytes differ)";
    return 0;
}

/* stage 2's source into IMG: src/len/crc in RAM (a hu.py upload), or src == 0: the card's BOOT.BIN, which must be the
 * running payload */
static const char *load_source(u32 src, u32 len, u32 crc, u32 *got)
{
    if (src) {
        if (len < 0x100 || len > INST_COPY_MAX || src < 0x00100000u || src >= MM_DRAM_END || len > MM_DRAM_END - src)
            return "source outside DRAM or size out of range (0x100 .. 0x18ff00 bytes)";
        if (src < MM_INSTALL + MM_INSTALL_SZ && src + len > MM_INSTALL)
            return "source overlaps the install buffers";
        if (inst_crc(0, (const u8 *)src, len) != crc)
            return "source CRC does not match (RAM changed after the upload?)";
        copy(IMG, (const u8 *)src, len);
        *got = len;
    } else {
        u32 n = 0;
        if (plat_load_file("BOOT.BIN", IMG, INST_COPY_MAX + 1, &n) || !n)
            return "cannot read BOOT.BIN from the card";
        if (n > INST_COPY_MAX)
            return "BOOT.BIN is larger than the copy region (0x18ff00 bytes)";
        const char *e = running_check(IMG, n);
        if (e)
            return e;
        *got = n, crc = inst_crc(0, IMG, n);
    }
    return inst_crc(0, IMG, *got) == crc ? 0 : "image changed while copying";
}

static int status_text(char *o, inst_fmt fmt, u32 src, u32 len, u32 crc)
{
    char *s = o;
    u32 id = 0;
    nor_jedec(&id);
    refresh();
    o += fmt(o, "install: flash %06x %s; power-on boot loaded %s; running build %s\n", id,
             id == NOR_JEDEC ? "(XM25QH128C)" : "(UNKNOWN PART: installs refused)",
             boot_src == 2 ? "the NOR copy" : boot_src == 1 ? "the SD card's BOOT.BIN" : "? (no hu_src: version 1 script or EXEC)",
             hu_build_id);
    if (jres)
        o += fmt(o, "last job: %s in %u ms: %s\n", jres > 0 ? "ok" : "FAILED", jms, jmsg);
    if (s1_resume >= 0)
        o += fmt(o, "STAGE 1 INTERRUPTED at step %d of %d: U-Boot may be half written. Do not power off; run `install "
                    "boot --write` again to finish it.\n", s1_resume + 1, pb.nblk);
    o += fmt(o, "u-boot: uImage at %06x, body %x bytes, sums hdr %08x data %08x; env %s at %06x\n", NOR_UBOOT_HDR, pb.body,
             pb.hsum, pb.dsum, pb.env_state == INST_ENV_STOCK ? "stock" : pb.env_state == INST_ENV_PATCHED ? "patched" :
             pb.env_state == INST_ENV_MANY ? "FOUND MORE THAN ONCE" : "not found", pb.env_off);
    o += fmt(o, "script block %06x: %s", NOR_SCRIPT_BLK, scr_name(&pb));
    if (pb.scr_state == INST_SCR_OURS)
        o += fmt(o, ", version %d, header crc %08x", pb.scr_ver, pb.scr_crc);
    o += fmt(o, "\nstage 1 (boot path): ");
    if (pb.refuse)
        o += fmt(o, "REFUSE: %s\n", pb.refuse);
    else if (!pb.nblk)
        o += fmt(o, "nothing to do (installed)\n");
    else {
        o += fmt(o, "would write");
        for (int k = 0; k < pb.nblk; k++)
            o += fmt(o, " %06x (%s)", pb.blk_off[k], pb.blk_off[k] == NOR_SCRIPT_BLK ? "script v2" :
                     pb.blk_off[k] == blk_base(NOR_UBOOT_HDR) ? "uImage sums" : "environment");
        if (pb.env_state == INST_ENV_STOCK)
            o += fmt(o, "; new sums hdr %08x data %08x", pb.new_hsum, pb.new_dsum);
        o += fmt(o, "\n");
    }
    o += fmt(o, "copy region %06x..%06x: ", INST_COPY_OFF, INST_COPY_END - 1);
    if (pc.state == INST_REG_BLANK)
        o += fmt(o, "blank");
    else if (pc.state == INST_REG_FOREIGN)
        o += fmt(o, "FOREIGN data at %06x", pc.first_bad);
    else
        o += fmt(o, "%scopy #%u, build %s, %u bytes, %s", pc.state == INST_REG_COPY ? "" : "interrupted (no magic) ",
                 pc.old_seq, pc.old_build, pc.old_len, pc.old_ok ? "CRC ok: boots" : "does NOT boot (CRC or magic)");
    o += fmt(o, "\nstage 2 (firmware, %s): ", src ? "the uploaded image" : "the card's BOOT.BIN");
    u32 n = 0;
    const char *e = s1_resume >= 0 ? "stage 1 is unfinished" : load_source(src, len, crc, &n);
    if (e)
        return o - s + fmt(o, "REFUSE: %s\n", e);
    struct inst_copy c = pc;
    if (inst_plan_copy(IMG, n, &pb, &c))
        return o - s + fmt(o, "REFUSE: %s\n", c.refuse);
    u32 ne = 0;
    for (u32 k = 0; k < INST_COPY_BLOCKS; k++)
        ne += c.erase_mask >> k & 1;
    return o - s + fmt(o, "would write build %s, %u bytes (max %u) as copy #%u, crc %08x, span crc %08x, erasing %u "
                          "block(s) first\n", c.build, n, INST_COPY_MAX, c.seq, c.crc, c.span_crc, ne);
}

static int start_boot(char *o, inst_fmt fmt)
{
    if (s1_resume >= 0) {   /* finish the interrupted plan; nor_boot_write accepts blocks it already (partly) wrote */
        job = 1, jdone = s1_resume, jtotal = pb.nblk, jres = 0, jt0 = clk_ms(), jmsg[0] = 0;
        s1_resume = -1;
        set_line();
        return fmt(o, "install boot: resuming the interrupted plan at step %d of %d", jdone + 1, jtotal);
    }
    refresh();
    if (pb.refuse)
        return fmt(o, "ERR install boot: %s", pb.refuse);
    if (!pb.nblk)
        return fmt(o, "install boot: nothing to do, the version 2 script and the patched environment are installed");
    job = 1, jdone = 0, jtotal = pb.nblk, jres = 0, jt0 = clk_ms(), jmsg[0] = 0;
    set_line();
    return fmt(o, "install boot: started, %d block(s); poll with `hu.py nor install progress`", pb.nblk);
}

static int start_copy(u32 src, u32 len, u32 crc, char *o, inst_fmt fmt)
{
    if (s1_resume >= 0)
        return fmt(o, "ERR install firmware: stage 1 is unfinished, run install boot first");
    refresh();
    u32 n = 0;
    const char *e = load_source(src, len, crc, &n);
    if (e)
        return fmt(o, "ERR install firmware: %s", e);
    if (inst_plan_copy(IMG, n, &pb, &pc))
        return fmt(o, "ERR install firmware: %s", pc.refuse);
    jtotal = 0;
    for (u32 k = 0; k < INST_COPY_BLOCKS; k++)
        jtotal += pc.erase_mask >> k & 1;
    jtotal += 1 + (int)((n + BLK - 1) / BLK) + 2;   /* header, image chunks, whole-region check, magic */
    job = 2, jphase = 0, jidx = INST_COPY_BLOCKS - 1, jdone = 0, jres = 0, jt0 = clk_ms(), jmsg[0] = 0;
    want_seq = pc.seq;
    set_line();
    return fmt(o, "install firmware: started, build %s, %u bytes as copy #%u, %d steps; poll with `hu.py nor install "
                  "progress`", pc.build, n, pc.seq, jtotal);
}

static void finish(int stage)
{
    job = 0, jms = clk_ms() - jt0;
    refresh();   /* re-read everything from flash: the result must look installed from scratch */
    int ok = stage == 1 ? inst_stage1_done(&pb) && !pb.nblk
                        : pc.state == INST_REG_COPY && pc.old_ok && pc.old_seq == want_seq;
    jres = ok ? 1 : -1;
    plat_fmt(jmsg, "stage %d written and verified; read back from flash: %s", stage, ok ? "installed" : "NOT as planned");
    set_line();
}

static void step_boot(void)
{
    u32 off = pb.blk_off[jdone];
    int r = nor_boot_write(off, pb.blk[jdone], pb.blk_live_crc[jdone], pb.env_off, INST_ENV_LEN, RBUF);
    if (r) {
        fail(off == NOR_SCRIPT_BLK ? "boot script block" : "U-Boot block", r);
        return;
    }
    if (++jdone == jtotal)
        finish(1);
    else
        set_line();
}

/* stage 2, one step per call: erase the blocks that hold data, last block first (the header block goes last, so an
 * interrupted erase still leaves a header that marks the region as ours); the header without its magic; the image in
 * 64 KB chunks, each checked blank before and verified after; a whole-region CRC through the controller and through the
 * XIP window, as the boot script reads it; then the magic word, the one thing the boot script trusts. */
#define FAIL(w, r) do { fail(w, r); return; } while (0)
static void step_copy(void)
{
    u32 bc, bx;
    int r;
    if (jphase == 0) {
        while (jidx >= 0 && !(pc.erase_mask >> jidx & 1))
            jidx--;
        if (jidx >= 0) {
            u32 off = INST_COPY_OFF + (u32)jidx * BLK;
            if ((r = nor_erase(off, BLK)) || (r = nor_blank(off, BLK, &bc, &bx)))
                FAIL("erase", r);
            if (bc || bx)
                FAIL("erase (not blank afterwards)", NOR_EVERIFY);
            jidx--, jdone++;
            set_line();
            return;
        }
        jphase = 1;
    }
    if (jphase == 1) {   /* everything blank now: the header without the magic */
        if ((r = nor_blank(INST_COPY_OFF, INST_COPY_END - INST_COPY_OFF, &bc, &bx)))
            FAIL("blank check", r);
        if (bc || bx)
            FAIL("region not blank before programming", NOR_ENOTBLANK);
        if ((r = nor_program(INST_COPY_OFF + 4, pc.hdr + 4, INST_COPY_HDR - 4)) ||
            (r = nor_verify(INST_COPY_OFF + 4, pc.hdr + 4, INST_COPY_HDR - 4, &bc, &bx)))
            FAIL("header", r);
        if (bc || bx)
            FAIL("header verify", NOR_EVERIFY);
        jphase = 2, jidx = 0, jdone++;
        set_line();
        return;
    }
    if (jphase == 2) {
        u32 at = (u32)jidx * BLK, n = pc.len - at < BLK ? pc.len - at : BLK;
        if ((r = nor_blank(INST_COPY_IMG + at, n, &bc, &bx)))
            FAIL("blank check", r);
        if (bc || bx)
            FAIL("image area not blank", NOR_ENOTBLANK);
        if ((r = nor_program(INST_COPY_IMG + at, IMG + at, n)) || (r = nor_verify(INST_COPY_IMG + at, IMG + at, n, &bc, &bx)))
            FAIL("image", r);
        if (bc || bx)
            FAIL("image verify", NOR_EVERIFY);
        jdone++;
        if (at + n >= pc.len)
            jphase = 3;
        else
            jidx++;
        set_line();
        return;
    }
    if (jphase == 3) {
        u32 cc = 0, cx = 0;
        for (u32 at = INST_COPY_IMG; at < INST_COPY_END; at += BLK) {
            u32 n = INST_COPY_END - at < BLK ? INST_COPY_END - at : BLK;
            if ((r = nor_read(at, RBUF, n, 0x0b)))
                FAIL("whole-region read", r);
            cc = inst_crc(cc, RBUF, n);
        }
        nor_blank(INST_COPY_IMG, 4, &bc, &bx);   /* the driver's prefetch-buffer flush before the window reads */
        for (u32 at = INST_COPY_IMG; at < INST_COPY_END; at += BLK) {
            u32 n = INST_COPY_END - at < BLK ? INST_COPY_END - at : BLK;
            for (u32 a = (NOR_XIP + at) & ~31u; a < NOR_XIP + at + n; a += 32)
                __asm__ volatile("cctl %0, L1D_VA_INVAL" ::"r"(a) : "memory");
            const volatile u8 *x = (const volatile u8 *)(NOR_XIP + at);
            for (u32 i = 0; i < n; i++)
                RBUF[i] = x[i];
            cx = inst_crc(cx, RBUF, n);
            irq_pet();
        }
        if (cc != pc.span_crc || cx != pc.span_crc) {
            plat_fmt(jmsg, "span crc controller %08x, xip %08x, want %08x", cc, cx, pc.span_crc);
            FAIL("whole-region CRC", NOR_EVERIFY);
        }
        jphase = 4, jdone++;
        set_line();
        return;
    }
    if ((r = nor_program(INST_COPY_OFF, pc.hdr, 4)) || (r = nor_verify(INST_COPY_OFF, pc.hdr, INST_COPY_HDR, &bc, &bx)))
        FAIL("magic", r);
    if (bc || bx)
        FAIL("magic verify", NOR_EVERIFY);
    jdone++;
    finish(2);
}

void install_poll(void)
{
    static int first = 1;
    if (job == 1) {
        step_boot();
        return;
    }
    if (job == 2) {
        step_copy();
        return;
    }
    int r = req;
    if (first && !r && clk_ms() > 20000)   /* the Settings status line, once after boot */
        r = 3;
    if (!r)
        return;
    req = 0, first = 0;
    char t[200];
    if (r == 1)
        start_boot(t, plat_fmt);
    else if (r == 2)
        start_copy(0, 0, 0, t, plat_fmt);
    else
        refresh();
    if (!job && r != 3 && t[0] == 'E') {   /* refused before it started */
        jres = -1, jms = 0;
        text(jmsg, t + 4, sizeof jmsg);
    }
    set_line();
}

/* HU1 op 29:
 *  9 status [u32 src, u32 len, u32 crc]   read-only: what is installed and what each stage would do; stage 2's dry run
 *                                          uses the image at src (a hu.py upload) or, without src, the card's BOOT.BIN
 * 10 boot     u32 magic                   start stage 1 (or finish an interrupted one)
 * 11 firmware u32 magic [, src, len, crc] start stage 2 (without src: the card's BOOT.BIN, which must be the running build)
 * 12 progress                              the running job or the last result
 * 13 script                                the version 2 script text, its uImage size and CRC (for the dry run) */
int install_op(const unsigned char *p, int n, char *out, inst_fmt fmt)
{
    int c = n ? p[0] : 12;
    u32 a1 = n >= 5 ? le32(p + 1) : 0, a2 = n >= 9 ? le32(p + 5) : 0, a3 = n >= 13 ? le32(p + 9) : 0;
    u32 a4 = n >= 17 ? le32(p + 13) : 0;
    if (c == 12) {
        if (job)
            return fmt(out, "install: stage %d running, step %d of %d, %u ms", job, jdone + 1, jtotal, clk_ms() - jt0);
        return fmt(out, "install: idle; last: %s (%u ms) %s", jres > 0 ? "ok" : jres < 0 ? "FAILED" : "none", jms, jmsg);
    }
    if (c == 13) {
        u8 *img = WORK + INST_WORK_SZ - 0x2000;   /* past the read buffer; the script image is under 2 KB */
        if (job)
            return fmt(out, "ERR install: busy");
        u32 l = inst_script_image(img);
        int k = fmt(out, "script %s, uImage %u bytes, crc %08x\n", INST_SCRIPT_NAME, l, inst_crc(0, img, l));
        return k + fmt(out + k, "%s", script_text);
    }
    if (job)
        return fmt(out, "ERR install: busy (stage %d, step %d of %d)", job, jdone + 1, jtotal);
    if (c == 9)
        return status_text(out, fmt, n >= 13 ? a1 : 0, a2, a3);
    if (c == 10 || c == 11) {
        if (n < 5 || a1 != NOR_MAGIC)
            return fmt(out, "ERR install: write sub-ops need the magic %08x", NOR_MAGIC);
        int k = c == 10 ? start_boot(out, fmt) : start_copy(n >= 17 ? a2 : 0, a3, a4, out, fmt);
        set_line();
        return k;
    }
    return fmt(out, "ERR install sub-op: 9 status, 10 boot, 11 firmware, 12 progress, 13 script");
}
#endif
