/* SPI NOR flash driver: XMC XM25QH128C (JEDEC 20 40 18, 16 MB, 4 KB sectors, 64 KB blocks, 256-byte pages) behind
 * the SoC's flash controller at 0x9C000B00. Everything here follows vendor U-Boot's `sfsp` driver (FlashInit 0xF082E6,
 * WREN 0xF089E4, register read 0xF08A52, DMA program 0xF08B2C, DMA read 0xF08BF6, erase 0xF08CB6, register write
 * 0xF08DBC, JEDEC 0xF09474, protect / unprotect tables 0xF09DE0 / 0xF09F82); notes/nor_flash.md has the register map,
 * which steps are copied, which are additions, and the XIP hazards.
 *
 * XIP: while an erase, a program or a status-register write runs, the window at 0x98000000 returns garbage. The payload
 * runs from DRAM and its only flash readers are explicit 0x98xxxxxx loads in task context (audio.c's microcode loader
 * inside aud_init, net task; plat_ui_install_status, UI task) plus U-Boot commands run by the HU1 CMD op (net task).
 * Interrupt handlers (irq.c: tick, watchdog pet, MCU heartbeat, UART drain) never touch flash. So every controller
 * step here runs in a "window": the scheduler is suspended (interrupts stay on, so the watchdog and the MCU heartbeat
 * keep going), the controller configuration is saved, the step runs to completion including the flash's busy bit,
 * the configuration is restored, and the scheduler resumes. Writes also refuse to run outside the net task, the only
 * task that calls aud_init, so a write and the microcode load can never overlap. */
#include "nor.h"

typedef unsigned int u32;
typedef unsigned char u8;

/* write ranges: free (all 0xFF) in the original dump. 0xE00000 is the NOR boot script. 0xFC0000..0xFFFFFF is the part
 * U-Boot's protect leaves writable for eCos's settings, so 0xFD0000..0xFEFFFF stays out even though it is blank. */
static const struct { u32 lo, hi; } allowed[] = {
    { 0x00C50000u, 0x00C60000u },
    { 0x00C70000u, 0x00E00000u },
    { 0x00E10000u, 0x00FC0000u },
};

int nor_range_ok(unsigned off, unsigned len)
{
    if (len == 0 || off + len < off)
        return 0;
    for (u32 i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
        if (off >= allowed[i].lo && off + len <= allowed[i].hi)
            return 1;
    return 0;
}

unsigned nor_page_chunk(unsigned off, unsigned len)
{
    u32 room = NOR_PAGE - (off & (NOR_PAGE - 1));
    return len < room ? len : room;
}

/* Winbond-style block protection for a 128 Mbit part (XM25QH128C follows it): SR1 BP2..0 (bits 4..2), TB (5), SEC (6),
 * SR2 CMP (6). U-Boot's protect sets BP = 001, CMP = 1: everything below 0xFC0000. */
int nor_wp_range(unsigned sr1, unsigned sr2, unsigned *lo, unsigned *hi)
{
    u32 bp = sr1 >> 2 & 7, tb = sr1 >> 5 & 1, sec = sr1 >> 6 & 1, cmp = sr2 >> 6 & 1;
    if (sec && bp != 0 && bp != 7)
        return -1;
    u32 size = bp == 0 ? 0 : bp == 7 ? NOR_SIZE : 0x40000u << (bp - 1);
    if (cmp)
        size = NOR_SIZE - size, tb = !tb;
    if (size == 0)
        return *lo = *hi = 0, 0;
    *lo = tb ? 0 : NOR_SIZE - size, *hi = tb ? size : NOR_SIZE;
    return 1;
}

const char *nor_strerror(int e)
{
    switch (e) {
    case 0: return "ok";
    case NOR_EARG: return "outside the allowed range or bad arguments";
    case NOR_ETIME: return "timeout";
    case NOR_EID: return "unexpected JEDEC ID";
    case NOR_EWP: return "protection bits did not change";
    case NOR_EFAULT: return "an earlier step timed out, reboot first";
    case NOR_ETASK: return "not the net task";
    case NOR_ESTATE: return "unexpected controller state";
    case NOR_EVERIFY: return "verify mismatch";
    case NOR_ENOTBLANK: return "target not erased";
    }
    return "?";
}

#ifndef NOR_HOST
#include "clk.h"
#include "crypto.h"
#include "memmap.h"
#include "FreeRTOS.h"
#include "task.h"

#define R(o) (*(volatile u32 *)(0x9c000b00u + (o)))
#define CTRL   0x00   /* [31] busy, [28] DMA program mode, [15:8] command, [6:4] data bytes 0..4, [2] write, [1:0] addr bytes */
#define TIMING 0x04
#define ADDR   0x08   /* flash address */
#define DATA   0x0c   /* PIO data, first byte on the wire in [7:0] */
#define STAT   0x10   /* result of a 0x05 status read */
#define AUTO   0x14   /* [31:24] read command (XIP and DMA reads), [22] prefetch, [21] PIO go, [20] user read command,
                       * [17] DMA go, [15:8] and [0]: U-Boot ORs 0x201 in before a DMA program */
#define CFG0   0x18   /* [15:0] DMA length */
#define CFG1   0x1c   /* line modes and dummy cycles [31:24] for PIO and DMA; 0x150095 = 1-1-1, no dummy */
#define CFG2   0x20   /* line modes for XIP reads (FlashInit: the part's quad read) */
#define DMAADR 0x34   /* DRAM address of a DMA transfer */
#define ISTS   0x48   /* [1] DMA done, write 1 to clear */

#define CTRL_BUSY   0x80000000u
#define CTRL_DMA_WR 0x10000000u
#define CTRL_WR     0x04u
#define CTRL_A3     0x03u
#define AUTO_PIO_GO 0x00200000u
#define AUTO_USR_RD 0x00100000u
#define AUTO_DMA_GO 0x00020000u
#define CFG1_SINGLE 0x00150095u   /* what FlashInit and every `sfsp read` leave here */
#define ISTS_DMA    0x2u
#define DMA_MAX     1024u

static int fault;   /* sticky: a step timed out and the flash may still be busy */
static int suspended;
static u32 s_auto, s_cfg0, s_cfg1;
static u8 dbuf[DMA_MAX] __attribute__((aligned(32)));   /* DMA bounce buffer (reads and page programs) */

static void dc(u32 a, u32 n, int wb, int inval)
{
    for (u32 p = a & ~31u; p < a + n; p += 32) {
        if (wb)
            __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(p) : "memory");
        if (inval)
            __asm__ volatile("cctl %0, L1D_VA_INVAL" ::"r"(p) : "memory");
    }
    __asm__ volatile("msync all" ::: "memory");
}

static int wait_bit(u32 reg, u32 bit, int set, u32 us)
{
    unsigned long long end = clk_us() + us;
    while (!(R(reg) & bit) != !set)
        if (clk_us() > end)
            return NOR_ETIME;
    return 0;
}

/* a window: no other task runs (none can reach the XIP window), interrupts stay on, controller config saved */
static void begin(void)
{
    suspended = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
    if (suspended)
        vTaskSuspendAll();
    wait_bit(CTRL, CTRL_BUSY, 0, 10000);
    s_auto = R(AUTO) & ~(AUTO_PIO_GO | AUTO_DMA_GO), s_cfg0 = R(CFG0), s_cfg1 = R(CFG1);
    R(CFG1) = CFG1_SINGLE;
}

static void end(void)
{
    wait_bit(CTRL, CTRL_BUSY, 0, 10000);
    R(CTRL) &= ~CTRL_DMA_WR;
    R(CFG1) = s_cfg1, R(CFG0) = s_cfg0, R(AUTO) = s_auto;
    if (suspended)
        xTaskResumeAll();
    suspended = 0;
}

/* one PIO command (U-Boot 0xF08A52 / 0xF08DBC / 0xF08CB6): flags = data bytes << 4 | CTRL_WR | address bytes */
static int pio(u32 cmd, u32 flags, u32 addr, u32 data, u32 *rx)
{
    if (wait_bit(CTRL, CTRL_BUSY, 0, 10000))
        return NOR_ETIME;
    if (flags & 3)
        R(ADDR) = addr;
    if (flags & 0x70)
        R(DATA) = data;
    R(CTRL) = (R(CTRL) & 0xffff0000u) | cmd << 8 | flags;
    R(AUTO) |= AUTO_PIO_GO;
    if (wait_bit(AUTO, AUTO_PIO_GO, 0, 10000))
        return NOR_ETIME;
    if (rx)
        *rx = cmd == 0x05 ? R(STAT) & 0xff : R(DATA);
    return 0;
}

static int wren(void) { return pio(0x06, CTRL_WR, 0, 0, 0); }

static int wait_wip(u32 us)   /* poll 0x05 until WIP clears, as U-Boot does after an erase */
{
    unsigned long long end = clk_us() + us;
    for (;;) {
        u32 s;
        int r = pio(0x05, 1 << 4, 0, 0, &s);
        if (r)
            return fault = 1, r;
        if (!(s & 1))
            return 0;
        if (clk_us() > end)
            return fault = 1, NOR_ETIME;
        clk_udelay(20);
    }
}

static int wr_sr(u32 cmd, u32 v)   /* WREN, 0x01 / 0x31 with one byte, wait (U-Boot 0xF08DBC) */
{
    int r = wren();
    if (!r)
        r = pio(cmd, 1 << 4 | CTRL_WR, 0, v & 0xff, 0);
    return r ? r : wait_wip(200000);
}

/* DMA read of n <= DMA_MAX bytes into dbuf (U-Boot 0xF08BF6, with a plain 1-1-1 command instead of the quad read) */
static int dma_read(u32 off, u32 n, u32 cmd)
{
    dc((u32)dbuf, n, 1, 1);
    if (wait_bit(CTRL, CTRL_BUSY, 0, 10000))
        return NOR_ETIME;
    R(CFG1) = cmd == 0x0b ? CFG1_SINGLE | 8u << 24 : CFG1_SINGLE;   /* 0x0B: 8 dummy cycles (field: inference) */
    R(ADDR) = off;
    R(DATA) = 0;
    R(CTRL) = (R(CTRL) & 0xffff0000u) | CTRL_A3;
    R(CFG0) = (R(CFG0) & 0xffff0000u) | n;
    R(DMAADR) = MM_PHYS(dbuf);
    R(AUTO) = (R(AUTO) & 0x00ffffffu & ~(AUTO_PIO_GO | AUTO_DMA_GO)) | AUTO_USR_RD | cmd << 24;
    R(AUTO) |= AUTO_DMA_GO;
    int r = wait_bit(ISTS, ISTS_DMA, 1, 100000);
    R(ISTS) |= ISTS_DMA;
    if (!r)
        r = wait_bit(CTRL, CTRL_BUSY, 0, 10000);
    dc((u32)dbuf, n, 0, 1);
    return r;
}

/* DMA program of n bytes from dbuf, inside one page (U-Boot 0xF095F8 / 0xF08B2C; the WREN before it and the busy poll
 * after it are additions: U-Boot relies on the controller for both) */
static int dma_program(u32 off, u32 n)
{
    if (!nor_range_ok(off, n) || nor_page_chunk(off, n) != n)
        return NOR_EARG;
    int r = wren();
    if (r || (r = wait_bit(CTRL, CTRL_BUSY, 0, 10000)))
        return r;
    R(CTRL) |= CTRL_DMA_WR;
    if ((r = wait_bit(CTRL, CTRL_BUSY, 0, 10000)))
        return r;
    R(ADDR) = off;
    R(CTRL) = (R(CTRL) & 0xffff0000u) | CTRL_WR | CTRL_A3;
    R(CFG0) = (R(CFG0) & 0xffff0000u) | n;
    R(DMAADR) = MM_PHYS(dbuf);
    R(AUTO) |= 0x201u;
    R(AUTO) |= AUTO_DMA_GO;
    r = wait_bit(ISTS, ISTS_DMA, 1, 100000);
    R(ISTS) |= ISTS_DMA;
    if (wait_bit(CTRL, CTRL_BUSY, 0, 10000) && !r)
        r = NOR_ETIME;
    R(CTRL) &= ~CTRL_DMA_WR;
    if (r)
        return fault = 1, r;
    return wait_wip(20000);   /* tPP is a few ms at most */
}

static int net_task_only(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return 0;
    const char *n = pcTaskGetName(0);
    return n[0] == 'n' && n[1] == 'e' && n[2] == 't' && !n[3] ? 0 : NOR_ETASK;
}

int nor_jedec(unsigned *id)
{
    u32 v = 0;
    begin();
    int r = pio(0x9f, 3 << 4, 0, 0, &v);
    end();
    *id = (v & 0xff) << 16 | (v & 0xff00) | (v >> 16 & 0xff);   /* as U-Boot 0xF09474 */
    return r;
}

static int sr_read(u32 sr[3])
{
    int r = pio(0x05, 1 << 4, 0, 0, &sr[0]);
    if (!r)
        r = pio(0x35, 1 << 4, 0, 0, &sr[1]);
    if (!r)
        r = pio(0x15, 1 << 4, 0, 0, &sr[2]);
    sr[1] &= 0xff, sr[2] &= 0xff;
    return r;
}

int nor_status(unsigned sr[3])
{
    begin();
    int r = sr_read(sr);
    end();
    return r;
}

int nor_read(unsigned off, void *dst, unsigned len, unsigned cmd)
{
    if (off >= NOR_SIZE || len > NOR_SIZE - off || (cmd != 0x03 && cmd != 0x0b))
        return NOR_EARG;
    u8 *d = dst;
    while (len) {
        u32 n = len < DMA_MAX ? len : DMA_MAX;
        begin();
        int r = dma_read(off, n, cmd);
        end();
        if (r)
            return r;
        for (u32 i = 0; i < n; i++)
            d[i] = dbuf[i];
        d += n, off += n, len -= n;
    }
    return 0;
}

/* writes run with the block protection cleared (U-Boot's unprotect table for this part: SR1 &= ~0x7C, SR2 &= ~0x40)
 * and put back exactly as found afterwards. U-Boot sets BP = 001, CMP = 1 at every boot. */
static u32 wp_sr1, wp_sr2, wp_changed;

static int wp_clear(void)
{
    u32 sr[3];
    begin();
    int r = sr_read(sr);
    wp_sr1 = sr[0], wp_sr2 = sr[1], wp_changed = 0;
    if (!r && (sr[0] & 0x7c))
        wp_changed = 1, r = wr_sr(0x01, sr[0] & ~0x7cu);
    if (!r)
        r = pio(0x35, 1 << 4, 0, 0, &sr[1]);
    if (!r && (sr[1] & 0x40))
        wp_changed = 1, r = wr_sr(0x31, sr[1] & ~0x40u);
    if (!r && !(r = sr_read(sr)) && ((sr[0] & 0x7c) || (sr[1] & 0x40)))
        r = NOR_EWP;
    end();
    return r;
}

static int wp_restore(void)
{
    if (!wp_changed)
        return 0;
    u32 sr[3];
    begin();
    int r = sr_read(sr);
    if (!r && (sr[0] & 0xfc) != (wp_sr1 & 0xfc))
        r = wr_sr(0x01, wp_sr1);
    if (!r)
        r = pio(0x35, 1 << 4, 0, 0, &sr[1]);
    if (!r && (sr[1] & 0xff) != (wp_sr2 & 0xff))
        r = wr_sr(0x31, wp_sr2);
    if (!r && !(r = sr_read(sr)) && ((sr[0] & 0xfc) != (wp_sr1 & 0xfc) || sr[1] != (wp_sr2 & 0xff)))
        r = NOR_EWP;
    end();
    if (!r)
        wp_changed = 0;
    return r;
}

static int write_ok(void)   /* every write: the net task, no earlier fault, the expected part, the expected state */
{
    int r = net_task_only();
    if (r)
        return r;
    if (fault)
        return NOR_EFAULT;
    u32 id;
    if ((r = nor_jedec(&id)))
        return r;
    if (id != NOR_JEDEC)
        return NOR_EID;
    u32 a = R(AUTO) & 0xff00;
    return a == 0 || a == 0x0200 ? 0 : NOR_ESTATE;   /* U-Boot ORs 0x02 into [15:8]; anything else is unknown */
}

/* drop stale lines of the cached window; then one read elsewhere, in case the controller's prefetch buffer (AUTO bit
 * 22, set by FlashInit) still holds bytes from before the write (inference, harmless) */
static void xip_fresh(u32 off, u32 len)
{
    dc(NOR_XIP + off, len, 0, 1);
    dc(NOR_XIP + 0x10000u, 4, 0, 1);
    (void)*(volatile u32 *)(NOR_XIP + 0x10000u);
}

int nor_erase(unsigned off, unsigned len)
{
    if ((off | len) & (NOR_SECTOR - 1) || !nor_range_ok(off, len))
        return NOR_EARG;
    int r = write_ok();
    if (r)
        return r;
    if ((r = wp_clear())) {
        wp_restore();
        return r;
    }
    while (len && !r) {
        int blk = !(off & (NOR_BLOCK - 1)) && len >= NOR_BLOCK;
        u32 n = blk ? NOR_BLOCK : NOR_SECTOR;
        if (!nor_range_ok(off, n)) {   /* cannot happen: the whole span was checked; fail closed anyway */
            r = NOR_EARG;
            break;
        }
        begin();
        r = wren();
        if (!r)
            r = pio(blk ? 0xd8 : 0x20, CTRL_WR | CTRL_A3, off, 0, 0);
        if (!r)
            r = wait_wip(blk ? 4000000 : 1000000);   /* typical 0.15 s / 45 ms, worst case 2 s / 0.4 s */
        else
            fault = 1;
        end();
        off += n, len -= n;
    }
    int r2 = wp_restore();
    return r ? r : r2;
}

int nor_program(unsigned off, const void *src, unsigned len)
{
    if (!nor_range_ok(off, len))
        return NOR_EARG;
    int r = write_ok();
    if (r)
        return r;
    if ((r = wp_clear())) {
        wp_restore();
        return r;
    }
    const u8 *s = src;
    while (len && !r) {
        u32 n = nor_page_chunk(off, len);
        for (u32 i = 0; i < n; i++)
            dbuf[i] = s[i];
        dc((u32)dbuf, n, 1, 0);
        begin();
        r = dma_program(off, n);
        end();
        s += n, off += n, len -= n;
    }
    int r2 = wp_restore();
    return r ? r : r2;
}

/* ---- HU1 op 29 ----
 * 0 status: JEDEC ID, SR1..3, protected range, controller registers, the write ranges
 * 1 compare  u32 off, u32 len [, u8 cmd]: controller read against the XIP window (len <= 1 MB)
 * 2 read     u32 off, u32 len, u32 dst [, u8 cmd]: controller read into the upload area (MM_UPLOAD)
 * 3 verify   u32 off, u32 src, u32 len: controller read and XIP window against RAM (len <= 1 MB)
 * 4 erase    u32 magic, u32 off: one 4 KB sector, then a blank check (controller and XIP)
 * 5 erase64  u32 magic, u32 off: one 64 KB block, then a blank check
 * 6 program  u32 magic, u32 off, u32 src, u32 len: the target must read blank first; then verify (len <= 64 KB)
 * Writes need NOR_MAGIC, an address range nor_range_ok accepts and the JEDEC ID NOR_JEDEC. */
static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static int ram_ok(u32 a, u32 n) { return a >= 0x00100000u && a < MM_DRAM_END && n <= MM_DRAM_END - a; }
static u8 cbuf[DMA_MAX];

/* controller read of [off, off + len) against RAM (ram != 0) and against the XIP window; counts and first mismatch */
static int compare(u32 off, u32 len, const u8 *ram, u32 cmd, char *o, nor_fmt fmt)
{
    u32 crc_c = 0, crc_x = 0, bad_c = 0, bad_x = 0, first_c = ~0u, first_x = ~0u;
    xip_fresh(off, len);
    for (u32 done = 0; done < len;) {
        u32 n = len - done < DMA_MAX ? len - done : DMA_MAX;
        int r = nor_read(off + done, cbuf, n, cmd);
        if (r)
            return fmt(o, "ERR read at %06x: %s", off + done, nor_strerror(r));
        const volatile u8 *x = (const volatile u8 *)(NOR_XIP + off + done);
        for (u32 i = 0; i < n; i++) {
            u8 want = ram ? ram[done + i] : x[i];
            if (cbuf[i] != want && bad_c++ == 0)
                first_c = off + done + i;
            if (ram && x[i] != want && bad_x++ == 0)
                first_x = off + done + i;
        }
        crc_c = crc32_update(crc_c, cbuf, (int)n);
        for (u32 i = 0; i < n; i++)
            cbuf[i] = x[i];
        crc_x = crc32_update(crc_x, cbuf, (int)n);
        done += n;
    }
    int k = fmt(o, "%06x+%x: controller crc %08x, xip crc %08x", off, len, crc_c, crc_x);
    if (!ram)
        return k + (bad_c ? fmt(o + k, "; controller vs xip MISMATCH: %u bytes, first at %06x", bad_c, first_c)
                          : fmt(o + k, "; controller = xip"));
    k += fmt(o + k, "; vs ram: controller %s, xip %s", bad_c ? "MISMATCH" : "MATCH", bad_x ? "MISMATCH" : "MATCH");
    if (bad_c || bad_x)
        k += fmt(o + k, " (controller %u bad from %06x, xip %u bad from %06x)", bad_c, first_c, bad_x, first_x);
    return k;
}

/* non-0xFF bytes in [off, off + len) through the controller (*bad_c) and through the XIP window (*bad_x) */
static int blank(u32 off, u32 len, u32 *bad_c, u32 *bad_x)
{
    xip_fresh(off, len);
    *bad_c = *bad_x = 0;
    for (u32 done = 0; done < len;) {
        u32 n = len - done < DMA_MAX ? len - done : DMA_MAX;
        int r = nor_read(off + done, cbuf, n, 0x03);
        if (r)
            return r;
        const volatile u8 *x = (const volatile u8 *)(NOR_XIP + off + done);
        for (u32 i = 0; i < n; i++)
            *bad_c += cbuf[i] != 0xff, *bad_x += x[i] != 0xff;
        done += n;
    }
    return 0;
}

int nor_op(const unsigned char *p, int n, char *out, nor_fmt fmt)
{
    char *o = out;
    int c = n ? p[0] : 0;
    u32 a1 = n >= 5 ? le32(p + 1) : 0, a2 = n >= 9 ? le32(p + 5) : 0, a3 = n >= 13 ? le32(p + 9) : 0;
    u32 a4 = n >= 17 ? le32(p + 13) : 0;
    if (c == 0) {
        u32 id = 0, sr[3] = { 0, 0, 0 }, lo, hi;
        int r = nor_jedec(&id);
        int r2 = nor_status(sr);
        o += fmt(o, "nor: jedec %06x%s (%s), sr1 %02x sr2 %02x sr3 %02x (%s)", id, id == NOR_JEDEC ? " XM25QH128C" : "",
                 nor_strerror(r), sr[0], sr[1], sr[2], nor_strerror(r2));
        int w = nor_wp_range(sr[0], sr[1], &lo, &hi);
        o += w < 0 ? fmt(o, ", protection not decoded (SEC=1)") : w ? fmt(o, ", protected %06x..%06x", lo, hi - 1)
                                                                    : fmt(o, ", unprotected");
        o += fmt(o, "; ctrl %08x timing %08x auto %08x cfg0 %08x cfg1 %08x cfg2 %08x ists %08x", R(CTRL), R(TIMING),
                 R(AUTO), R(CFG0), R(CFG1), R(CFG2), R(ISTS));
        o += fmt(o, "; writable");
        for (u32 i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
            o += fmt(o, " %06x..%06x", allowed[i].lo, allowed[i].hi - 1);
        return o - out + fmt(o, "; fault %d", fault);
    }
    if (c == 1 || c == 2) {   /* compare / read: off, len [, dst], cmd */
        u32 cmd = c == 1 ? (n >= 10 ? p[9] : 0x03) : (n >= 14 ? p[13] : 0x03);
        if (a1 >= NOR_SIZE || a2 == 0 || a2 > NOR_SIZE - a1 || (cmd != 0x03 && cmd != 0x0b))
            return fmt(o, "ERR nor: off < 16 MB, 0 < len, off + len <= 16 MB, cmd 03 or 0b");
        if (c == 1) {
            if (a2 > 0x100000u)
                return fmt(o, "ERR nor compare: len <= 1 MB");
            return compare(a1, a2, 0, cmd, o, fmt);
        }
        if (a3 < MM_UPLOAD || a2 > MM_UPLOAD_SZ || a3 - MM_UPLOAD > MM_UPLOAD_SZ - a2)
            return fmt(o, "ERR nor read: dst inside the upload area %08x + %x", MM_UPLOAD, MM_UPLOAD_SZ);
        int r = nor_read(a1, (void *)a3, a2, cmd);
        return fmt(o, "nor read %06x+%x -> %08x: %s, crc %08x", a1, a2, a3, nor_strerror(r),
                   r ? 0 : crc32_le((const u8 *)a3, (int)a2));
    }
    if (c == 3) {   /* verify off, src, len */
        if (a1 >= NOR_SIZE || a3 == 0 || a3 > NOR_SIZE - a1 || a3 > 0x100000u || !ram_ok(a2, a3))
            return fmt(o, "ERR nor verify: flash range, len <= 1 MB, src in DRAM");
        return compare(a1, a3, (const u8 *)a2, 0x03, o, fmt);
    }
    if (c >= 4 && c <= 6) {
        if (n < 9 || a1 != NOR_MAGIC)
            return fmt(o, "ERR nor: write sub-ops need the magic %08x", NOR_MAGIC);
        u32 off = a2;
        if (c == 4 || c == 5) {
            u32 len = c == 4 ? NOR_SECTOR : NOR_BLOCK;
            if ((off & (len - 1)) || !nor_range_ok(off, len))
                return fmt(o, "ERR nor erase %06x: not %u-aligned inside the allowed range", off, len);
            unsigned long long t = clk_us();
            int r = nor_erase(off, len);
            o += fmt(o, "nor erase %06x+%x: %s, %u ms", off, len, nor_strerror(r), (u32)((clk_us() - t) / 1000));
            u32 bc, bx;
            if (r == 0 || r == NOR_EWP) {   /* the erase itself finished: look at the result */
                int b = blank(off, len, &bc, &bx);
                o += b ? fmt(o, "; blank check: %s", nor_strerror(b))
                       : fmt(o, "; blank check: controller %u, xip %u non-FF bytes%s", bc, bx, bc || bx ? "" : " (BLANK)");
            }
            return o - out;
        }
        u32 src = a3, len = a4;
        if (n < 17 || !nor_range_ok(off, len) || len > NOR_BLOCK || !ram_ok(src, len))
            return fmt(o, "ERR nor program %06x+%x: inside the allowed range, len <= 64 KB, src in DRAM", off, len);
        u32 bc, bx;
        int b = blank(off, len, &bc, &bx);
        if (b || bc || bx)
            return fmt(o, "ERR nor program %06x+%x: target not blank (read %s, controller %u, xip %u non-FF bytes)", off,
                       len, nor_strerror(b), bc, bx);
        unsigned long long t = clk_us();
        int r = nor_program(off, (const u8 *)src, len);
        o += fmt(o, "nor program %06x+%x from %08x: %s, %u ms; ", off, len, src, nor_strerror(r),
                 (u32)((clk_us() - t) / 1000));
        if (r && r != NOR_EWP && r != NOR_EVERIFY)
            return o - out;
        return o - out + compare(off, len, (const u8 *)src, 0x03, o, fmt);
    }
    return fmt(o, "ERR nor sub-op: 0 status, 1 compare, 2 read, 3 verify, 4 erase, 5 erase64, 6 program");
}
#endif
