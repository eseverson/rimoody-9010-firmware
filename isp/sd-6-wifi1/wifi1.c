/* Wi-Fi bring-up, round 1. Runs from U-Boot `go` on the SPHE8268K head unit, borrowing the stock U-Boot 2014.04
 * (May 05 2024), gd in $r10. U-Boot is linked at 0xF00040 but relocates itself to the top of the 128 MB RAM (round 6
 * run 2: gp 0x07d76cd8, gd 0x077b2f30); every U-Boot address below is the link address plus `reloc`, taken from the
 * $gp that U-Boot's `go` leaves us (its PC-relative GOT base, link value UB_GP_LINK).
 *
 *   go 0x1800000 hello   CPU/gd report into HELLO_BUF, returns. Touches nothing else.
 *   go 0x1800000 main    hooks U-Boot's console into a RAM log, runs `usb start`, finds the RTL8188FTV, reads its
 *                        efuse, powers the MAC on and uploads + starts the 8051 firmware. The log is saved to the card
 *                        as LOGnn.TXT after every step, so the last file shows how far it got.
 *   go 0x1800000 alive   writes Annn.TXT with the uptime every 20 s for 10 min, to measure what resets the unit.
 *
 * Register sequences follow Linux drivers/net/wireless/realtek/rtl8xxxu (v6.6), rtl8188fu_* and rtl8xxxu_core.c. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

/* ---- stock U-Boot entry points (each checked against its first instruction word before use) ---- */
#define UB_PARSE_STRING_OUTER 0x00f0d7fe  /* parse_string_outer(cmd, FLAG_PARSE_SEMICOLON | FLAG_EXIT_FROM_LOOP) */
#define UB_GET_TIMER          0x00f00c2e  /* get_timer(base), ms */
#define UB_UDELAY             0x00f86a0c
#define UB_USB_GET_DEV_INDEX  0x00f17146
#define UB_USB_CONTROL_MSG    0x00f16b26
#define UB_STDIO_DEVICES      0x00fd9614  /* struct stdio_dev *stdio_devices[3] */
#define UB_GOT_STDIO_DEVICES  0x00fb8660  /* GOT slot holding &stdio_devices (fixed up by relocation) */
#define UB_GP_LINK            0x00fb3d18
#define UB_TEXT_START         0x00f00040
#define UB_TEXT_END           0x00fb8e10
#define RAM_END               0x08000000

static unsigned int reloc;   /* runtime minus link address of U-Boot */
#define UB(a) ((a) + reloc)

#include "ub_sig.h"   /* ub_sig[]: {address, first instruction word} of each entry point, from the flash dump */

#define ub_run(cmd)  ((int (*)(const char *, int))UB(UB_PARSE_STRING_OUTER))(cmd, 3)
#define ub_ms(base)  ((u32 (*)(u32))UB(UB_GET_TIMER))(base)
#define ub_udelay(us) ((void (*)(u32))UB(UB_UDELAY))(us)
#define ub_usb_dev(i) ((u8 *(*)(int))UB(UB_USB_GET_DEV_INDEX))(i)
typedef int (*ctrl_fn)(void *dev, u32 pipe, u8 req, u8 reqtype, u16 value, u16 index, void *data, u16 size,
                       int timeout);
#define ub_ctrl ((ctrl_fn)UB(UB_USB_CONTROL_MSG))

/* struct usb_device field offsets in this build (from usb_new_device / usb_get_descriptor / usb_control_msg) */
#define UDEV_DEVNUM    0x000
#define UDEV_MAXPACKET 0x06c
#define UDEV_DESC      0x140   /* struct usb_device_descriptor; idVendor at +8 */
#define UDEV_STATUS    0x954

struct stdio_dev {
    int flags, ext;
    char name[16];
    int (*start)(void);
    int (*stop)(void);
    void (*putc)(const char c);
    void (*puts)(const char *s);
};

/* ---- memory map ---- */
#define HELLO_BUF ((char *)0x01500000)
#define LOG_BUF   ((char *)0x01a00000)
#define LOG_MAX   0x000f0000

static u32 log_len, log_seq, log_hooked;
static struct stdio_dev *hooked[2];
static void (*orig_putc[2])(const char);
static void (*orig_puts[2])(const char *);

static u32 rd32(const void *p) { return *(volatile const u32 *)p; }
static u16 rd16(const void *p) { return *(volatile const u16 *)p; }

static void log_c(char c)
{
    if (log_len < LOG_MAX)
        LOG_BUF[log_len++] = c;
}

static void log_s(const char *s)
{
    while (*s)
        log_c(*s++);
}

/* Console hooks: keep U-Boot's output in the log, then pass it on to the UART as before. */
static void hook_putc0(const char c) { log_c(c); orig_putc[0](c); }
static void hook_puts0(const char *s) { log_s(s); orig_puts[0](s); }
static void hook_putc1(const char c) { log_c(c); orig_putc[1](c); }
static void hook_puts1(const char *s) { log_s(s); orig_puts[1](s); }

/* ---- minimal printf into the log: %s %c %d %u %x %p, with optional 0 and width ---- */
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_arg __builtin_va_arg
#define va_end __builtin_va_end

static char *fmt_num(char *p, u32 v, int base, int width, char pad, int neg)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (neg)
        tmp[n++] = '-';
    while (width-- > n)
        *p++ = pad;
    while (n)
        *p++ = tmp[--n];
    return p;
}

static int vfmt(char *out, const char *f, va_list ap)
{
    char *p = out;
    for (; *f; f++) {
        if (*f != '%') {
            *p++ = *f;
            continue;
        }
        f++;
        char pad = ' ';
        int width = 0;
        if (*f == '0') {
            pad = '0';
            f++;
        }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                *p++ = *s++;
            break;
        }
        case 'c': *p++ = (char)va_arg(ap, int); break;
        case 'd': {
            int v = va_arg(ap, int);
            p = fmt_num(p, v < 0 ? -(u32)v : (u32)v, 10, width, pad, v < 0);
            break;
        }
        case 'u': p = fmt_num(p, va_arg(ap, u32), 10, width, pad, 0); break;
        case 'p': width = 8; pad = '0'; /* fall through */
        case 'x': p = fmt_num(p, va_arg(ap, u32), 16, width, pad, 0); break;
        default: *p++ = *f; break;
        }
    }
    *p = 0;
    return p - out;
}

static char fmtbuf[512];

static void lprintf(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vfmt(fmtbuf, f, ap);
    va_end(ap);
    log_s(fmtbuf);
}

static char cmdbuf[160];

static int run(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vfmt(cmdbuf, f, ap);
    va_end(ap);
    lprintf("=> %s\n", cmdbuf);
    int r = ub_run(cmdbuf);
    lprintf("[rc=%d t=%u]\n", r, ub_ms(0));
    return r;
}

/* Save the whole log so far as a new file; the highest-numbered LOGnn.TXT is the most complete. */
static void flush(const char *why)
{
    lprintf("--- flush %u (%s) t=%u\n", log_seq, why, ub_ms(0));
    u32 len = log_len;
    char cmd[96];
    /* build the command without logging it first, so the file ends at this flush marker */
    char *p = cmd;
    const char *a = "fatwrite $isp_if $isp_dev 1a00000 LOG";
    while (*a)
        *p++ = *a++;
    *p++ = '0' + (log_seq / 10) % 10;
    *p++ = '0' + log_seq % 10;
    a = ".TXT ";
    while (*a)
        *p++ = *a++;
    p = fmt_num(p, len, 16, 0, ' ', 0);
    *p = 0;
    log_seq++;
    ub_run(cmd);
}

static void hexdump(const u8 *b, int n, u32 base)
{
    for (int i = 0; i < n; i += 16) {
        lprintf("%04x:", base + i);
        for (int j = 0; j < 16 && i + j < n; j++)
            lprintf(" %02x", b[i + j]);
        log_c('\n');
    }
}

/* ---- CPU / U-Boot state ---- */
#define MFSR(reg) ({ u32 _v; __asm__ volatile("mfsr %0, $" #reg : "=r"(_v)); _v; })

static void report_cpu(void)
{
    u32 sp, gp, lp, r10;
    __asm__ volatile("mov55 %0, $sp" : "=r"(sp));
    __asm__ volatile("mov55 %0, $gp" : "=r"(gp));
    __asm__ volatile("mov55 %0, $lp" : "=r"(lp));
    __asm__ volatile("mov55 %0, $r10" : "=r"(r10));
    lprintf("sp=%p gp=%p lp=%p gd(r10)=%p\n", sp, gp, lp, r10);
    lprintf("PSW=%p IVB=%p CPU_VER=%p ICM_CFG=%p DCM_CFG=%p\n", MFSR(PSW), MFSR(IVB), MFSR(CPU_VER), MFSR(ICM_CFG),
            MFSR(DCM_CFG));
    lprintf("MMU_CFG=%p MSC_CFG=%p MMU_CTL=%p CACHE_CTL=%p\n", MFSR(MMU_CFG), MFSR(MSC_CFG), MFSR(MMU_CTL),
            MFSR(CACHE_CTL));
    if (r10 >= 0x00100000 && r10 < RAM_END) {
        lprintf("gd[0..0x80]:\n");
        hexdump((const u8 *)r10, 0x80, 0);
    }
}

static int check_uboot(void)
{
    u32 gp;
    __asm__ volatile("mov55 %0, $gp" : "=r"(gp));
    reloc = gp - UB_GP_LINK;
    lprintf("gp=%p -> U-Boot relocated by %p (runs at %p)\n", gp, reloc, UB(UB_TEXT_START));
    u32 gd, gd_reloc;
    __asm__ volatile("mov55 %0, $r10" : "=r"(gd));
    gd_reloc = rd32((const void *)(gd + 0x40));   /* gd->reloc_off; gd+0x2c relocaddr, gd+0x30 ram_size */
    if (reloc != gd_reloc || (UB(UB_TEXT_START) & 0xfff) || UB(UB_TEXT_END) > RAM_END) {
        lprintf("implausible relocation offset (gd->reloc_off=%p)\n", gd_reloc);
        return 1;
    }
    int bad = 0;
    for (unsigned i = 0; i < sizeof(ub_sig) / sizeof(ub_sig[0]); i++) {
        const u16 *p = (const u16 *)UB(ub_sig[i][0]);   /* entry points are only halfword aligned */
        u32 have = rd16(p) | (u32)rd16(p + 1) << 16, want = ub_sig[i][1];
        if (have != want) {
            lprintf("U-Boot mismatch at %p: %p != %p\n", (u32)p, have, want);
            bad++;
        }
    }
    lprintf("U-Boot entry check: %s\n", bad ? "FAILED" : "ok");
    return bad;
}

static void install_hook(void)
{
    struct stdio_dev **devs = (struct stdio_dev **)rd32((const void *)UB(UB_GOT_STDIO_DEVICES));
    lprintf("&stdio_devices=%p (expected %p)\n", (u32)devs, UB(UB_STDIO_DEVICES));
    if ((u32)devs != UB(UB_STDIO_DEVICES)) {
        log_s("GOT does not match, console not hooked\n");
        return;
    }
    for (int i = 0; i < 2; i++) {
        struct stdio_dev *d = devs[i + 1];   /* stdout, stderr */
        lprintf("stdio_devices[%d]=%p", i + 1, (u32)d);
        if ((u32)d < 0x00100000 || (u32)d >= RAM_END) {
            log_s(" (not a RAM pointer, not hooked)\n");
            continue;
        }
        if (i == 1 && d == hooked[0]) {
            log_s(" (same device as stdout)\n");
            continue;
        }
        lprintf(" name=%s putc=%p puts=%p\n", d->name, (u32)d->putc, (u32)d->puts);
        if ((u32)d->putc < UB(UB_TEXT_START) || (u32)d->putc >= UB(UB_TEXT_END) ||
            (u32)d->puts < UB(UB_TEXT_START) || (u32)d->puts >= UB(UB_TEXT_END)) {
            log_s("  not in U-Boot text, not hooked\n");
            continue;
        }
        hooked[i] = d;
        orig_putc[i] = d->putc;
        orig_puts[i] = d->puts;
        d->putc = i ? hook_putc1 : hook_putc0;
        d->puts = i ? hook_puts1 : hook_puts0;
        log_hooked = 1;
    }
}

/* ---- RTL8188FU over U-Boot's USB stack ---- */
static u8 *wdev;
static u32 errs;
static u8 xbuf[256] __attribute__((aligned(64)));

static u32 pipe_base(u8 *d, int ep) { return (rd32(d + UDEV_DEVNUM) << 8) | (ep << 15) | rd32(d + UDEV_MAXPACKET); }
static u32 ctrl_in(u8 *d) { return (2u << 30) | pipe_base(d, 0) | 0x80; }
static u32 ctrl_out(u8 *d) { return (2u << 30) | pipe_base(d, 0); }

static u32 rtl_read(u16 addr, int len)
{
    int r = ub_ctrl(wdev, ctrl_in(wdev), 0x05, 0xc0, addr, 0, xbuf, len, 500);
    if (r != len) {
        if (errs++ < 20)
            lprintf("  read %04x/%d failed: %d status=%x\n", addr, len, r, rd32(wdev + UDEV_STATUS));
        return 0xdeadbeef;
    }
    u32 v = 0;
    for (int i = len - 1; i >= 0; i--)
        v = (v << 8) | xbuf[i];
    return v;
}

static void rtl_write(u16 addr, u32 val, int len)
{
    if (errs) {
        if (errs++ < 20)
            lprintf("  write %04x skipped after an earlier USB error\n", addr);
        return;
    }
    for (int i = 0; i < len; i++)
        xbuf[i] = val >> (8 * i);
    int r = ub_ctrl(wdev, ctrl_out(wdev), 0x05, 0x40, addr, 0, xbuf, len, 500);
    if (r != len && errs++ < 20)
        lprintf("  write %04x/%d=%x failed: %d status=%x\n", addr, len, val, r, rd32(wdev + UDEV_STATUS));
}

#define rr8(a) ((u8)rtl_read(a, 1))
#define rr16(a) ((u16)rtl_read(a, 2))
#define rr32(a) rtl_read(a, 4)
#define rw8(a, v) rtl_write(a, v, 1)
#define rw16(a, v) rtl_write(a, v, 2)
#define rw32(a, v) rtl_write(a, v, 4)

#define REG_SYS_ISO_CTRL 0x0000
#define REG_SYS_FUNC     0x0002
#define REG_APS_FSMCO    0x0004
#define REG_SYS_CLKR     0x0008
#define REG_9346CR       0x000a
#define REG_RSV_CTRL     0x001c
#define REG_EFUSE_CTRL   0x0030
#define REG_MCU_FW_DL    0x0080
#define REG_EFUSE_ACCESS 0x00cf
#define REG_SYS_CFG      0x00f0
#define REG_CR           0x0100
#define REG_TRXDMA_CTRL  0x010c
#define REG_TRXFF_BNDY   0x0114
#define REG_HMTFR        0x01cc
#define REG_RQPN         0x0200
#define REG_RQPN_NPQ     0x0214
#define REG_FW_START     0x1000

#define BIT(n) (1u << (n))

static int n_out_eps;

/* Read and print the configuration descriptor (interfaces/endpoints). */
static void dump_config(void)
{
    int r = ub_ctrl(wdev, ctrl_in(wdev), 6, 0x80, 0x0200, 0, xbuf, 9, 500);
    if (r != 9) {
        lprintf("config descriptor (9) failed: %d\n", r);
        return;
    }
    int total = xbuf[2] | (xbuf[3] << 8);
    if (total > (int)sizeof(xbuf))
        total = sizeof(xbuf);
    r = ub_ctrl(wdev, ctrl_in(wdev), 6, 0x80, 0x0200, 0, xbuf, total, 500);
    lprintf("config descriptor: %d of %d bytes\n", r, total);
    if (r <= 0)
        return;
    hexdump(xbuf, r, 0);
    for (int i = 0; i + 8 <= r && xbuf[i] >= 2 && i + xbuf[i] <= r;) {
        if (xbuf[i + 1] == 4)
            lprintf("  interface %d alt %d: %d endpoints class %02x/%02x/%02x\n", xbuf[i + 2], xbuf[i + 3],
                    xbuf[i + 4], xbuf[i + 5], xbuf[i + 6], xbuf[i + 7]);
        if (xbuf[i + 1] == 5) {
            static const char *const type[] = { "ctrl", "iso", "bulk", "int" };
            lprintf("  endpoint %02x %s maxpacket %d\n", xbuf[i + 2], type[xbuf[i + 3] & 3],
                    xbuf[i + 4] | (xbuf[i + 5] << 8));
            if ((xbuf[i + 3] & 3) == 2 && !(xbuf[i + 2] & 0x80))
                n_out_eps++;
        }
        i += xbuf[i];
    }
}

static u8 efuse[512];

static int efuse_read8(u16 off, u8 *out)
{
    rw8(REG_EFUSE_CTRL + 1, off & 0xff);
    u8 v = rr8(REG_EFUSE_CTRL + 2);
    rw8(REG_EFUSE_CTRL + 2, (v & 0xfc) | ((off >> 8) & 3));
    v = rr8(REG_EFUSE_CTRL + 3);
    rw8(REG_EFUSE_CTRL + 3, v & 0x7f);
    for (int i = 0; i < 500; i++) {
        u32 r = rr32(REG_EFUSE_CTRL);
        if (r != 0xdeadbeef && (r & BIT(31))) {
            ub_udelay(50);
            *out = rr32(REG_EFUSE_CTRL) & 0xff;
            return 0;
        }
    }
    return -1;
}

static int read_efuse(void)
{
    u16 v16 = rr16(REG_9346CR);
    lprintf("9346CR=%04x (eeprom %s, boot from %s)\n", v16, v16 & BIT(5) ? "present" : "absent",
            v16 & BIT(4) ? "EEPROM" : "EFUSE");
    rw8(REG_EFUSE_ACCESS, 0x69);
    v16 = rr16(REG_SYS_ISO_CTRL);
    if (!(v16 & BIT(15)))
        rw16(REG_SYS_ISO_CTRL, v16 | BIT(15));
    v16 = rr16(REG_SYS_FUNC);
    if (!(v16 & BIT(12)))
        rw16(REG_SYS_FUNC, v16 | BIT(12));
    v16 = rr16(REG_SYS_CLKR);
    if (!(v16 & BIT(5)) || !(v16 & BIT(1)))
        rw16(REG_SYS_CLKR, v16 | BIT(5) | BIT(1));

    for (int i = 0; i < 512; i++)
        efuse[i] = 0xff;
    int ret = 0, phys = 0;
    u8 hdr, ext, lo, hi;
    while (phys < 512) {
        if (efuse_read8(phys++, &hdr)) {
            ret = -1;
            break;
        }
        if (hdr == 0xff)
            break;
        int off, mask;
        if ((hdr & 0x1f) == 0x0f) {
            off = (hdr & 0xe0) >> 5;
            if (efuse_read8(phys++, &ext)) {
                ret = -1;
                break;
            }
            if ((ext & 0x0f) == 0x0f)
                continue;
            off |= (ext & 0xf0) >> 1;
            mask = ext & 0x0f;
        } else {
            off = (hdr >> 4) & 0x0f;
            mask = hdr & 0x0f;
        }
        int map = off * 8;
        for (int w = 0; w < 4; w++, map += 2) {
            if (mask & BIT(w))
                continue;
            if (efuse_read8(phys++, &lo) || efuse_read8(phys++, &hi)) {
                ret = -1;
                break;
            }
            if (map < 511) {
                efuse[map] = lo;
                efuse[map + 1] = hi;
            }
        }
        if (ret)
            break;
    }
    rw8(REG_EFUSE_ACCESS, 0);
    lprintf("efuse: %d physical bytes used, ret=%d\n", phys, ret);
    hexdump(efuse, 512, 0);
    lprintf("efuse id=%04x (want 8129) mac=%02x:%02x:%02x:%02x:%02x:%02x vid/pid=%02x/%02x xtal_k=%02x\n",
            efuse[0] | (efuse[1] << 8), efuse[0xd7], efuse[0xd8], efuse[0xd9], efuse[0xda], efuse[0xdb], efuse[0xdc],
            efuse[0xd0], efuse[0xd2], efuse[0xb9]);
    return ret;
}

static int poll32(u16 reg, u32 mask, u32 want, int tries, u32 us)
{
    for (int i = 0; i < tries; i++) {
        u32 v = rr32(reg);
        if (v != 0xdeadbeef && (v & mask) == want)
            return 0;
        if (us)
            ub_udelay(us);
    }
    return -1;
}

static int power_on(void)
{
    /* rtl8188f_disabled_to_emu */
    u8 v = rr8(REG_APS_FSMCO + 1);
    rw8(REG_APS_FSMCO + 1, v & ~((BIT(12) | BIT(11)) >> 8));
    v = rr8(0xc4);
    rw8(0xc4, v & ~BIT(4));
    /* rtl8188f_emu_to_active */
    v = rr8(REG_APS_FSMCO + 1);
    rw8(REG_APS_FSMCO + 1, v & ~(BIT(10) >> 8));
    if (poll32(REG_APS_FSMCO, BIT(17), BIT(17), 500, 10)) {
        log_s("power ready (APS_FSMCO bit17) timeout\n");
        return -1;
    }
    v = rr8(REG_APS_FSMCO + 1);
    rw8(REG_APS_FSMCO + 1, v & ~(BIT(15) >> 8));
    v = rr8(REG_APS_FSMCO + 1);
    rw8(REG_APS_FSMCO + 1, v & ~(BIT(11) >> 8));
    v = rr8(REG_APS_FSMCO + 1);
    rw8(REG_APS_FSMCO + 1, v | (BIT(8) >> 8));
    if (poll32(REG_APS_FSMCO, BIT(8), 0, 500, 10)) {
        log_s("MAC enable (APS_FSMCO bit8) timeout\n");
        return -1;
    }
    rw8(0x27, 0x35);
    /* rtl8188fu_power_on */
    rw8(REG_CR, 0);
    u16 cr = rr16(REG_CR);
    cr |= BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(9) | BIT(10);
    rw16(REG_CR, cr);
    lprintf("power on ok: APS_FSMCO=%p CR=%04x\n", rr32(REG_APS_FSMCO), rr16(REG_CR));
    return 0;
}

/* rtl8xxxu_init_queue_reserved_page + init_queue_priority + RX page boundary, 8188F values */
static void init_queues(void)
{
    int hq = 0, nq = 0, lq = 0, count = 0;
    switch (n_out_eps) {
    default:
        lq = 0x02, count++;   /* fall through */
    case 2:
        nq = 0x02, count++;   /* fall through */
    case 1:
        hq = 0x0c, count++;
        break;
    case 0:
        log_s("no bulk OUT endpoints seen, assuming 3\n");
        lq = 0x02, nq = 0x02, hq = 0x0c, count = 3;
        break;
    }
    rw32(REG_RQPN_NPQ, nq);
    u32 pubq = 0xf7 - hq - lq - nq - 1;
    rw32(REG_RQPN, BIT(31) | hq | (lq << 8) | (pubq << 16));

    u16 hiq, mgq, bkq, beq, viq, voq;
    if (count == 1) {
        hiq = mgq = bkq = beq = viq = voq = 3;
    } else if (count == 2) {
        hiq = mgq = viq = voq = 3;
        bkq = beq = 2;
    } else {
        beq = bkq = 1;
        viq = 2;
        voq = mgq = hiq = 3;
    }
    u16 v = rr16(REG_TRXDMA_CTRL) & 7;
    v |= (voq << 4) | (viq << 6) | (beq << 8) | (bkq << 10) | (mgq << 12) | (hiq << 14);
    rw16(REG_TRXDMA_CTRL, v);
    rw16(REG_TRXFF_BNDY + 2, 0x3f7f);
    lprintf("queues: %d out eps, RQPN=%p TRXDMA_CTRL=%04x\n", count, rr32(REG_RQPN), rr16(REG_TRXDMA_CTRL));
}

static void reset_8051(void)
{
    u8 v = rr8(REG_RSV_CTRL + 1);
    rw8(REG_RSV_CTRL + 1, v & ~BIT(0));
    u16 f = rr16(REG_SYS_FUNC) & ~BIT(10);
    rw16(REG_SYS_FUNC, f);
    v = rr8(REG_RSV_CTRL + 1);
    rw8(REG_RSV_CTRL + 1, v | BIT(0));
    rw16(REG_SYS_FUNC, f | BIT(10));
}

extern const u8 fw_bin[], fw_bin_end[];

static int write_block(u16 addr, const u8 *src, int len)
{
    static u8 blk[128] __attribute__((aligned(64)));
    for (int i = 0; i < len; i++)
        blk[i] = src[i];
    int r = ub_ctrl(wdev, ctrl_out(wdev), 0x05, 0x40, addr, 0, blk, len, 500);
    if (r != len) {
        lprintf("  fw block %04x/%d failed: %d\n", addr, len, r);
        return -1;
    }
    return 0;
}

static int firmware(void)
{
    const u8 *fw = fw_bin + 32;
    int size = fw_bin_end - fw_bin - 32;
    lprintf("firmware: signature %04x version %d size %d\n", fw_bin[0] | (fw_bin[1] << 8), fw_bin[4] | (fw_bin[5] << 8),
            size);

    /* rtl8xxxu_download_firmware */
    u8 v = rr8(REG_SYS_FUNC + 1);
    rw8(REG_SYS_FUNC + 1, v | 4);
    u16 f = rr16(REG_SYS_FUNC);
    rw16(REG_SYS_FUNC, f | BIT(10));
    v = rr8(REG_MCU_FW_DL);
    lprintf("MCU_FW_DL before=%p\n", rr32(REG_MCU_FW_DL));
    if (v & BIT(7)) {
        log_s("firmware already running, resetting the 8051\n");
        rw8(REG_MCU_FW_DL, 0);
        reset_8051();
    }
    v = rr8(REG_MCU_FW_DL);
    rw8(REG_MCU_FW_DL, v | BIT(0));
    u32 v32 = rr32(REG_MCU_FW_DL);
    rw32(REG_MCU_FW_DL, v32 & ~BIT(19));
    v = rr8(REG_MCU_FW_DL);
    rw8(REG_MCU_FW_DL, v | BIT(2));

    u32 t0 = ub_ms(0);
    int ret = 0;
    for (int page = 0; page * 4096 < size && !ret; page++) {
        v = rr8(REG_MCU_FW_DL + 2) & 0xf8;
        rw8(REG_MCU_FW_DL + 2, v | page);
        int n = size - page * 4096;
        if (n > 4096)
            n = 4096;
        for (int off = 0; off < n && !ret; off += 128)
            ret = write_block(REG_FW_START + off, fw + page * 4096 + off, n - off < 128 ? n - off : 128);
    }
    u16 dl = rr16(REG_MCU_FW_DL);
    rw16(REG_MCU_FW_DL, dl & ~BIT(0));
    lprintf("download %s in %u ms, MCU_FW_DL=%p\n", ret ? "FAILED" : "done", ub_ms(t0), rr32(REG_MCU_FW_DL));
    if (ret)
        return ret;

    /* rtl8xxxu_start_firmware */
    if (poll32(REG_MCU_FW_DL, BIT(2), BIT(2), 1000, 0)) {
        lprintf("checksum report timeout, MCU_FW_DL=%p\n", rr32(REG_MCU_FW_DL));
        return -1;
    }
    v32 = rr32(REG_MCU_FW_DL);
    rw32(REG_MCU_FW_DL, (v32 | BIT(1)) & ~BIT(6));
    reset_8051();
    t0 = ub_ms(0);
    if (poll32(REG_MCU_FW_DL, BIT(6), BIT(6), 1000, 100)) {
        lprintf("firmware did not start, MCU_FW_DL=%p\n", rr32(REG_MCU_FW_DL));
        return -1;
    }
    rw8(REG_HMTFR, 0x0f);
    lprintf("FIRMWARE RUNNING after %u ms, MCU_FW_DL=%p\n", ub_ms(t0), rr32(REG_MCU_FW_DL));
    return 0;
}

static void dump_regs(const char *when)
{
    static u8 regs[256];
    for (int a = 0; a < 256; a += 4) {
        u32 v = rr32(a);
        regs[a] = v, regs[a + 1] = v >> 8, regs[a + 2] = v >> 16, regs[a + 3] = v >> 24;
    }
    lprintf("registers 0x00-0xff %s:\n", when);
    hexdump(regs, 256, 0);
}

static void wifi(void)
{
    log_s("\n# USB devices\n");
    for (int i = 0; i < 32; i++) {
        u8 *d = ub_usb_dev(i);
        if (!d)
            continue;
        const u8 *desc = d + UDEV_DESC;
        lprintf("dev[%d] @%p devnum=%d id=%04x:%04x class=%02x maxpacket0=%d\n", i, (u32)d, rd32(d + UDEV_DEVNUM),
                rd16(desc + 8), rd16(desc + 10), desc[4], desc[7]);
        if (rd16(desc + 8) == 0x0bda && !wdev)
            wdev = d;
    }
    if (!wdev) {
        log_s("no Realtek (0bda) device on U-Boot's USB bus\n");
        return;
    }
    lprintf("using Realtek device @%p\n", (u32)wdev);
    dump_config();
    flush("enumerated");

    u32 cfg = rr32(REG_SYS_CFG);
    lprintf("SYS_CFG=%p chip cut %d, vendor bits %x, test chip %d\n", cfg, (cfg >> 12) & 0xf, (cfg >> 18) & 3,
            !!(cfg & BIT(23)));
    dump_regs("at start");
    flush("registers");

    if (errs)
        goto out;
    read_efuse();
    flush("efuse");

    if (errs || power_on())
        goto out;
    init_queues();
    flush("powered");

    if (!errs && firmware() == 0)
        dump_regs("with firmware running");
out:
    lprintf("usb errors: %u\n", errs);
}

static void hello(int argc, char *const argv[])
{
    log_len = 0;
    lprintf("hello from wifi1 payload, argc=%d", argc);
    for (int i = 0; i < argc; i++)
        lprintf(" [%s]", argv[i]);
    log_c('\n');
    report_cpu();
    check_uboot();
    lprintf("end\n");
    for (u32 i = 0; i < 0x800; i++)   /* HELLO.TXT is the first 0x800 bytes */
        HELLO_BUF[i] = i < log_len ? LOG_BUF[i] : ' ';
}

static void alive(void)
{
    u32 t0 = ub_ms(0);
    for (int n = 0; n < 30; n++) {
        while (ub_ms(t0) < (u32)n * 20000)
            ub_udelay(1000);
        char *p = HELLO_BUF;
        const char *a = "alive ms=";
        while (*a)
            *p++ = *a++;
        p = fmt_num(p, ub_ms(0), 10, 0, ' ', 0);
        *p++ = '\n';
        char name[48];
        char *q = name;
        a = "fatwrite $isp_if $isp_dev 1500000 A";
        while (*a)
            *q++ = *a++;
        q = fmt_num(q, n, 10, 3, '0', 0);
        a = ".TXT ";
        while (*a)
            *q++ = *a++;
        q = fmt_num(q, p - HELLO_BUF, 16, 0, ' ', 0);
        *q = 0;
        ub_run(name);
    }
}

extern u32 __bss_start[], __bss_end[];

__attribute__((section(".text.entry"), used))
int payload_entry(int argc, char *const argv[])
{
    const char *cmd = argc > 1 ? argv[1] : "";
    if (cmd[0] == 'h') {
        for (u32 *p = __bss_start; p < __bss_end; p++)
            *p = 0;
        hello(argc, argv);
        return 0;
    }
    if (cmd[0] == 'a') {
        if (check_uboot())
            return 2;
        alive();
        return 0;
    }
    if (cmd[0] != 'm')
        return 1;

    for (u32 *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    log_s("wifi1 main\n");
    report_cpu();
    if (check_uboot())
        return 2;   /* log stays in RAM at 0x1a00000; calling U-Boot now is unsafe */
    lprintf("t=%u ms since boot\n", ub_ms(0));
    install_hook();
    flush("hooked");

    log_s("\n# U-Boot USB\n");
    run("usb start");
    flush("usb start");
    run("usb tree");
    run("usb info");
    flush("usb info");

    wifi();
    flush("done");
    for (int i = 0; i < 2; i++) {
        if (hooked[i]) {
            hooked[i]->putc = orig_putc[i];
            hooked[i]->puts = orig_puts[i];
        }
    }
    return 0;
}
