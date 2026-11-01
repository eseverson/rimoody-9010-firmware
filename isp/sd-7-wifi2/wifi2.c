/* Wi-Fi bring-up, round 2. Runs from U-Boot `go` on the SPHE8268K head unit, borrowing the stock U-Boot 2014.04
 * (May 05 2024), gd in $r10. U-Boot is linked at 0xF00040 but relocates itself to the top of the 128 MB RAM (round 6
 * run 2: gp 0x07d76cd8, gd 0x077b2f30); every U-Boot address below is the link address plus `reloc`, taken from the
 * $gp that U-Boot's `go` leaves us (its PC-relative GOT base, link value UB_GP_LINK).
 *
 *   go 0x1800000 hello   CPU/gd report into HELLO_BUF, returns. Touches nothing else.
 *   go 0x1800000 main    hooks U-Boot's console into a RAM log, runs `usb start`, finds the RTL8188FTV, reads its
 *                        efuse, powers the MAC on, uploads the 8051 firmware (falling back to smaller control-transfer
 *                        blocks if 128 bytes fail), runs the rest of the 8188F init and a passive scan of channels
 *                        1-13 over bulk IN. The log is saved to the card as LOGnn.TXT after every step, so the
 *                        last file shows how far it got.
 *   go 0x1800000 alive   keeps sending the MCU heartbeat and writes Annn.TXT with the uptime every 20 s for 6 min,
 *                        to test whether the missing heartbeat is what resets the unit ~150 s after U-Boot starts.
 *
 * The heartbeat is the frame eCos sends every 4 s on the MCU link (UART1, 0x9C000980, 115200; eCos 0x31d738, frame
 * builder 0x521aa8): ff aa LEN payload CHK8 CRC16be, payload d1 00. stage1.txt sets UART1's pinmux (0x9C0000A4, ids
 * 63/64 as U-Boot's ispsp update_finish does) and runs `uartsp init 1 115200`; hb_tick() sends it every 3 s.
 *
 * Register sequences follow Linux drivers/net/wireless/realtek/rtl8xxxu (v6.6), rtl8188fu_* and rtl8xxxu_core.c;
 * register names come from that driver's rtl8xxxu_regs.h, the init tables from gen_tables.py. Left out on purpose:
 * rtl8188fu_phy_iq_calibrate (RX works without it) and everything TX/association related beyond the init writes. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef signed char s8;

#define BIT(n) (1u << (n))
#define GENMASK(h, l) ((~0u << (l)) & (~0u >> (31 - (h))))
#include "rtl8xxxu_regs.h"

struct reg8val { u16 reg; u8 val; };
struct reg32val { u16 reg; u32 val; };
struct rfregval { u8 reg; u32 val; };
#include "rtl8188f_tables.h"

/* ---- stock U-Boot entry points (each checked against its first instruction word before use) ---- */
#define UB_PARSE_STRING_OUTER 0x00f0d7fe  /* parse_string_outer(cmd, FLAG_PARSE_SEMICOLON | FLAG_EXIT_FROM_LOOP) */
#define UB_GET_TIMER          0x00f00c2e  /* get_timer(base), ms */
#define UB_UDELAY             0x00f86a0c
#define UB_USB_GET_DEV_INDEX  0x00f17146
#define UB_USB_CONTROL_MSG    0x00f16b26
#define UB_USB_BULK_MSG       0x00f16d1c  /* usb_bulk_msg(dev, pipe, data, len, &actual, timeout) */
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
typedef int (*bulk_fn)(void *dev, u32 pipe, void *data, int len, int *actual, int timeout);
#define ub_bulk ((bulk_fn)UB(UB_USB_BULK_MSG))

/* struct usb_device field offsets in this build (from usb_new_device / usb_maxpacket / ehci_submit_async) */
#define UDEV_DEVNUM    0x000
#define UDEV_SPEED     0x008   /* 3 = high speed */
#define UDEV_MAXPACKET 0x06c   /* encoded PACKET_SIZE_*, goes into the pipe's low bits */
#define UDEV_TOGGLE    0x070   /* toggle[0] IN, toggle[1] OUT, one bit per endpoint */
#define UDEV_EPMAX_IN  0x080   /* int epmaxpacketin[16] (EHCI takes the QH max packet from here) */
#define UDEV_EPMAX_OUT 0x0c0   /* int epmaxpacketout[16] */
#define UDEV_DESC      0x140   /* struct usb_device_descriptor; idVendor at +8 */
#define UDEV_STATUS    0x954   /* 0 ok, 2 stall, 4 buf err, 8 babble, 0x20 crc/xact (|2 halted), 0x80000000 timeout */
#define UDEV_ACT_LEN   0x958

/* ---- time budget, in ub_ms(0) (which starts ~3 s after U-Boot itself; the unit resets ~150 s after U-Boot) ---- */
#define EHCI_BULK_MS   5000    /* ehci_submit_async gives up on a bulk TD after 5000 ms, others after 1000 (0xf7e562) */
#define T_SCAN_END     95000   /* no bulk read starts unless it can time out before this */
#define T_INIT_LATEST  60000   /* skip the radio init if we get here this late */
#define DWELL_MS       1500    /* per channel; a channel with no frame at all costs EHCI_BULK_MS instead */

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

/* ---- MCU heartbeat ---- */
#define MCU_UART_DATA ((volatile u32 *)0x9c000980)
#define MCU_UART_LSR  ((volatile u32 *)0x9c000984)   /* bit0: TX ready, bit1: RX byte available */
static const u8 hb_frame[] = { 0xff, 0xaa, 0x02, 0xd1, 0x00, 0x2d, 0xbe, 0xba };
static u32 hb_last, hb_count, hb_stuck;

static void hb_send(void)
{
    for (unsigned i = 0; i < sizeof(hb_frame); i++) {
        int n = 100000;
        while (!(*MCU_UART_LSR & 1) && --n)
            ;
        if (!n)
            hb_stuck++;
        *MCU_UART_DATA = hb_frame[i];
    }
    hb_count++;
}

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
static void hb_tick(void);

static void flush(const char *why)
{
    hb_tick();
    if (log_seq > 99)
        return;
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

static void hb_tick(void)
{
    u32 now = ub_ms(0);
    if (now - hb_last >= 3000) {
        hb_last = now;
        hb_send();
    }
}

static void mdelay(u32 ms)
{
    while (ms--) {
        ub_udelay(1000);
        if (!(ms & 255))
            hb_tick();
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

/* U-Boot's usb_*pipe(): type << 30 | devnum << 8 | endpoint << 15 | dev->maxpacketsize, | 0x80 for IN. The bulk IN
 * form (0xc0000080 | ...) is what usb_storage builds at 0xf1840e before calling usb_bulk_msg. */
static u32 pipe_base(u8 *d, int ep) { return (rd32(d + UDEV_DEVNUM) << 8) | (ep << 15) | rd32(d + UDEV_MAXPACKET); }
static u32 ctrl_in(u8 *d) { return (2u << 30) | pipe_base(d, 0) | 0x80; }
static u32 ctrl_out(u8 *d) { return (2u << 30) | pipe_base(d, 0); }
static u32 bulk_in(u8 *d, int ep) { return (3u << 30) | pipe_base(d, ep) | 0x80; }

static u32 rtl_read(u16 addr, int len)
{
    if (errs >= 8) {   /* the device has stopped answering: don't spend up to 1 s (EHCI timeout) per read */
        errs++;
        return 0xdeadbeef;
    }
    hb_tick();
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
    hb_tick();
    for (int i = 0; i < len; i++)
        xbuf[i] = val >> (8 * i);
    int r = ub_ctrl(wdev, ctrl_out(wdev), 0x05, 0x40, addr, 0, xbuf, len, 500);
    if (r != len && errs++ < 20)
        lprintf("  write %04x/%d=%x failed: %d status=%x\n", addr, len, val, r, rd32(wdev + UDEV_STATUS));
}

/* A vendor control transfer that never counts as an error (probes only). */
static int ctrl_raw(int in, u16 addr, u8 *buf, int len)
{
    int r = ub_ctrl(wdev, in ? ctrl_in(wdev) : ctrl_out(wdev), 0x05, in ? 0xc0 : 0x40, addr, 0, buf, len, 500);
    lprintf("  probe %s %04x/%d: %d status=%x act_len=%d\n", in ? "read" : "write", addr, len, r,
            rd32(wdev + UDEV_STATUS), rd32(wdev + UDEV_ACT_LEN));
    return r;
}

#define rr8(a) ((u8)rtl_read(a, 1))
#define rr16(a) ((u16)rtl_read(a, 2))
#define rr32(a) rtl_read(a, 4)
#define rw8(a, v) rtl_write(a, v, 1)
#define rw16(a, v) rtl_write(a, v, 2)
#define rw32(a, v) rtl_write(a, v, 4)

static int n_out_eps, bulk_in_ep;

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
    /* every descriptor that lies completely inside the r bytes received */
    for (int i = 0; i + 2 <= r && xbuf[i] >= 2 && i + xbuf[i] <= r; i += xbuf[i]) {
        const u8 *d = xbuf + i;
        if (d[1] == 4 && d[0] >= 9)
            lprintf("  interface %d alt %d: %d endpoints class %02x/%02x/%02x\n", d[2], d[3], d[4], d[5], d[6], d[7]);
        if (d[1] == 5 && d[0] >= 7) {
            static const char *const type[] = { "ctrl", "iso", "bulk", "int" };
            lprintf("  endpoint %02x %s maxpacket %d\n", d[2], type[d[3] & 3], d[4] | (d[5] << 8));
            if ((d[3] & 3) == 2) {
                if (!(d[2] & 0x80))
                    n_out_eps++;
                else if (!bulk_in_ep)
                    bulk_in_ep = d[2] & 0x0f;
            }
        }
    }
    lprintf("bulk OUT endpoints: %d, bulk IN endpoint: %d\n", n_out_eps, bulk_in_ep);
}

static u8 efuse[512];

static int efuse_read8(u16 off, u8 *out)
{
    rw8(REG_EFUSE_CTRL + 1, off & 0xff);
    u8 v = rr8(REG_EFUSE_CTRL + 2);
    rw8(REG_EFUSE_CTRL + 2, (v & 0xfc) | ((off >> 8) & 3));
    v = rr8(REG_EFUSE_CTRL + 3);
    rw8(REG_EFUSE_CTRL + 3, v & 0x7f);
    for (int i = 0; i < 500 && !errs; i++) {
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
    for (int i = 0; i < tries && !errs; i++) {
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
    u16 f = rr16(REG_SYS_FUNC) & ~SYS_FUNC_CPU_ENABLE;
    rw16(REG_SYS_FUNC, f);
    v = rr8(REG_RSV_CTRL + 1);
    rw8(REG_RSV_CTRL + 1, v | BIT(0));
    rw16(REG_SYS_FUNC, f | SYS_FUNC_CPU_ENABLE);
}

/* ---- firmware ---- */
extern const u8 fw_bin[], fw_bin_end[];
static int fw_bs;   /* control-transfer block size the download finally worked with */

static int fw_write_block(u16 addr, const u8 *src, int len)
{
    static u8 blk[128] __attribute__((aligned(64)));
    for (int i = 0; i < len; i++)
        blk[i] = src[i];
    int r = ub_ctrl(wdev, ctrl_out(wdev), 0x05, 0x40, addr, 0, blk, len, 500);
    if (r != len) {
        lprintf("  fw block %04x/%d failed: %d status=%x act_len=%d\n", addr, len, r, rd32(wdev + UDEV_STATUS),
                rd32(wdev + UDEV_ACT_LEN));
        return -1;
    }
    return 0;
}

/* With download enabled and page 0 selected: does a 4-byte write to the firmware window read back? */
static void fw_probe(const u8 *fw)
{
    static u8 pb[64] __attribute__((aligned(64)));
    u8 v = rr8(REG_MCU_FW_DL + 2) & 0xf8;
    rw8(REG_MCU_FW_DL + 2, v);
    for (int i = 0; i < 4; i++)
        pb[i] = fw[i];
    int w = ctrl_raw(0, REG_FW_START_ADDRESS, pb, 4);
    for (int i = 0; i < 4; i++)
        pb[i] = 0x5a;
    int r = ctrl_raw(1, REG_FW_START_ADDRESS, pb, 4);
    lprintf("fw window probe: wrote %02x %02x %02x %02x (rc %d), read back %02x %02x %02x %02x (rc %d) -> %s\n",
            fw[0], fw[1], fw[2], fw[3], w, pb[0], pb[1], pb[2], pb[3], r,
            w == 4 && r == 4 && pb[0] == fw[0] && pb[1] == fw[1] && pb[2] == fw[2] && pb[3] == fw[3] ? "MATCH"
                                                                                                     : "no match");
}

/* rtl8xxxu_download_firmware, with the control-transfer block size as a parameter */
static int fw_download(int bs, int probe)
{
    const u8 *fw = fw_bin + 32;
    int size = fw_bin_end - fw_bin - 32;

    u8 v = rr8(REG_SYS_FUNC + 1);
    rw8(REG_SYS_FUNC + 1, v | 4);
    u16 f = rr16(REG_SYS_FUNC);
    rw16(REG_SYS_FUNC, f | SYS_FUNC_CPU_ENABLE);
    v = rr8(REG_MCU_FW_DL);
    lprintf("MCU_FW_DL before=%p\n", rr32(REG_MCU_FW_DL));
    if (v & MCU_FW_RAM_SEL) {
        log_s("firmware already running, resetting the 8051\n");
        rw8(REG_MCU_FW_DL, 0);
        reset_8051();
    }
    v = rr8(REG_MCU_FW_DL);
    rw8(REG_MCU_FW_DL, v | MCU_FW_DL_ENABLE);
    u32 v32 = rr32(REG_MCU_FW_DL);
    rw32(REG_MCU_FW_DL, v32 & ~BIT(19));
    if (probe && !errs)
        fw_probe(fw);   /* before the checksum reset, so the probe write cannot spoil the checksum */
    v = rr8(REG_MCU_FW_DL);
    rw8(REG_MCU_FW_DL, v | MCU_FW_DL_CSUM_REPORT);

    u32 t0 = ub_ms(0);
    int ret = 0;
    for (int page = 0; page * 4096 < size && !ret && !errs; page++) {
        v = rr8(REG_MCU_FW_DL + 2) & 0xf8;
        rw8(REG_MCU_FW_DL + 2, v | page);
        int n = size - page * 4096;
        if (n > 4096)
            n = 4096;
        for (int off = 0; off < n && !ret; off += bs)
            ret = fw_write_block(REG_FW_START_ADDRESS + off, fw + page * 4096 + off, n - off < bs ? n - off : bs);
    }
    if (errs)
        ret = -1;
    u16 dl = rr16(REG_MCU_FW_DL);
    rw16(REG_MCU_FW_DL, dl & ~MCU_FW_DL_ENABLE);
    lprintf("download (%d-byte blocks) %s in %u ms, MCU_FW_DL=%p\n", bs, ret ? "FAILED" : "done", ub_ms(t0),
            rr32(REG_MCU_FW_DL));
    return ret;
}

/* rtl8xxxu_start_firmware */
static int fw_start(void)
{
    if (poll32(REG_MCU_FW_DL, MCU_FW_DL_CSUM_REPORT, MCU_FW_DL_CSUM_REPORT, 1000, 0)) {
        lprintf("checksum report timeout, MCU_FW_DL=%p\n", rr32(REG_MCU_FW_DL));
        return -1;
    }
    u32 v32 = rr32(REG_MCU_FW_DL);
    rw32(REG_MCU_FW_DL, (v32 | MCU_FW_DL_READY) & ~MCU_WINT_INIT_READY);
    reset_8051();
    u32 t0 = ub_ms(0);
    if (poll32(REG_MCU_FW_DL, MCU_WINT_INIT_READY, MCU_WINT_INIT_READY, 1000, 100)) {
        lprintf("firmware did not start, MCU_FW_DL=%p\n", rr32(REG_MCU_FW_DL));
        return -1;
    }
    rw8(REG_HMTFR, 0x0f);
    lprintf("firmware started after %u ms, MCU_FW_DL=%p\n", ub_ms(t0), rr32(REG_MCU_FW_DL));
    return 0;
}

static int firmware(void)
{
    lprintf("firmware: signature %04x version %d size %d\n", fw_bin[0] | (fw_bin[1] << 8), fw_bin[4] | (fw_bin[5] << 8),
            (int)(fw_bin_end - fw_bin - 32));
    lprintf("usb_device: speed %d, ep0 maxpacket in/out %d/%d, ep%d IN maxpacket %d, toggles %x/%x\n",
            rd32(wdev + UDEV_SPEED), rd32(wdev + UDEV_EPMAX_IN), rd32(wdev + UDEV_EPMAX_OUT), bulk_in_ep,
            rd32(wdev + UDEV_EPMAX_IN + 4 * bulk_in_ep), rd32(wdev + UDEV_TOGGLE), rd32(wdev + UDEV_TOGGLE + 4));
    log_s("(usb_device status: 2 stall, 4 data buffer error, 8 babble, 0x20 crc/xact error, |2 if halted, "
          "0x80000000 not processed = EHCI timeout)\n");
    static const u8 sizes[] = { 128, 64, 32, 16, 8, 4 };
    for (unsigned i = 0; i < sizeof(sizes) && !errs; i++) {
        lprintf("# firmware download, %d-byte control writes\n", sizes[i]);
        if (fw_download(sizes[i], i == 0))
            continue;
        if (fw_start() == 0) {
            fw_bs = sizes[i];
            lprintf("FIRMWARE RUNNING (downloaded with %d-byte blocks)\n", fw_bs);
            return 0;
        }
    }
    log_s("firmware: every block size failed\n");
    return -1;
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

/* ---- RF registers through the LSSI/HSSI 3-wire bridge (path A only; the 8188F is 1T1R) ---- */
static u32 read_rfreg(u8 reg)   /* rtl8xxxu_read_rfreg */
{
    u32 hssia = rr32(REG_FPGA0_XA_HSSI_PARM2);
    u32 v = hssia;
    v &= ~FPGA0_HSSI_PARM2_ADDR_MASK;
    v |= (u32)reg << FPGA0_HSSI_PARM2_ADDR_SHIFT;
    v |= FPGA0_HSSI_PARM2_EDGE_READ;
    hssia &= ~FPGA0_HSSI_PARM2_EDGE_READ;
    rw32(REG_FPGA0_XA_HSSI_PARM2, hssia);
    ub_udelay(10);
    rw32(REG_FPGA0_XA_HSSI_PARM2, v);
    ub_udelay(100);
    hssia |= FPGA0_HSSI_PARM2_EDGE_READ;
    rw32(REG_FPGA0_XA_HSSI_PARM2, hssia);
    ub_udelay(10);
    v = rr32(REG_FPGA0_XA_HSSI_PARM1);
    if (v & FPGA0_HSSI_PARM1_PI)
        v = rr32(REG_HSPI_XA_READBACK);
    else
        v = rr32(REG_FPGA0_XA_LSSI_READBACK);
    return v & 0xfffff;
}

static void write_rfreg(u8 reg, u32 data)   /* rtl8xxxu_write_rfreg */
{
    data &= FPGA0_LSSI_PARM_DATA_MASK;
    rw32(REG_FPGA0_XA_LSSI_PARM, ((u32)reg << FPGA0_LSSI_PARM_ADDR_SHIFT) | data);
    ub_udelay(1);
}

static void rw32_mask(u16 reg, u32 mask, u32 val)   /* rtl8xxxu_write32_mask */
{
    int shift = 0;
    while (!(mask & BIT(shift)))
        shift++;
    u32 v = rr32(reg);
    rw32(reg, (v & ~mask) | ((val << shift) & mask));
}

/* ---- the rest of rtl8xxxu_init_device for the 8188F, in Linux order ---- */
static int chip_cut;
#define PPG_BB_GAIN_2G_TXA_OFFSET_8188F 0xee   /* physical efuse byte, from rtl8xxxu_8188f.c */

static void antenna_selection(void)   /* rtl8723bu_phy_init_antenna_selection */
{
    u32 v = rr32(REG_PAD_CTRL1);
    rw32(REG_PAD_CTRL1, v & ~(BIT(20) | BIT(24)));
    v = rr32(REG_GPIO_MUXCFG);
    rw32(REG_GPIO_MUXCFG, v & ~BIT(4));
    v = rr32(REG_GPIO_MUXCFG);
    rw32(REG_GPIO_MUXCFG, v | BIT(3));
    v = rr32(REG_LEDCFG0);
    rw32(REG_LEDCFG0, v | BIT(24));
    v = rr32(REG_LEDCFG0);
    rw32(REG_LEDCFG0, v & ~BIT(23));
    v = rr32(REG_RFE_BUFFER);
    rw32(REG_RFE_BUFFER, v | BIT(0) | BIT(1));
    v = rr32(REG_RFE_CTRL_ANTA_SRC);
    rw32(REG_RFE_CTRL_ANTA_SRC, (v & 0xffffff00) | 0x77);
    v = rr32(REG_PWR_DATA);
    rw32(REG_PWR_DATA, v | PWR_DATA_EEPRPAD_RFE_CTRL_EN);
}

static void init_mac(void)   /* rtl8xxxu_init_mac with rtl8188f_mac_init_table */
{
    for (const struct reg8val *t = rtl8188f_mac_init_table; t->reg != 0xffff && !errs; t++)
        rw8(t->reg, t->val);
}

static void init_phy_regs(const struct reg32val *t)   /* rtl8xxxu_init_phy_regs */
{
    for (; t->reg != 0xffff && !errs; t++) {
        rw32(t->reg, t->val);
        ub_udelay(1);
    }
}

static void init_phy_bb(void)   /* rtl8188fu_init_phy_bb + the generic tail of rtl8xxxu_init_phy_bb */
{
    u16 v16 = rr16(REG_SYS_FUNC);
    rw16(REG_SYS_FUNC, v16 | SYS_FUNC_BB_GLB_RSTN | SYS_FUNC_BBRSTB | SYS_FUNC_DIO_RF);
    rw8(REG_RF_CTRL, RF_ENABLE | RF_RSTB | RF_SDMRSTB);
    ub_udelay(20);
    write_rfreg(RF6052_REG_IQADJ_G1, 0x780);
    rw8(REG_SYS_FUNC, SYS_FUNC_BB_GLB_RSTN | SYS_FUNC_BBRSTB | SYS_FUNC_USBA | SYS_FUNC_USBD);
    init_phy_regs(rtl8188fu_phy_init_table);
    init_phy_regs(rtl8188f_agc_table);

    /* rtl8188f_set_crystal_cap(default_crystal_cap); Linux skips it when the cap equals its initial 0 */
    u32 cap = efuse[0xb9] & 0x3f;
    if (cap) {
        u32 v = rr32(REG_AFE_XTAL_CTRL);
        v &= ~(GENMASK(22, 17) | GENMASK(16, 11));
        v |= (cap << 17) | (cap << 11);
        rw32(REG_AFE_XTAL_CTRL, v);
    }
    lprintf("phy bb: %d + %d table writes, crystal cap %02x, AFE_XTAL_CTRL=%p\n",
            (int)(sizeof(rtl8188fu_phy_init_table) / sizeof(rtl8188fu_phy_init_table[0])) - 1,
            (int)(sizeof(rtl8188f_agc_table) / sizeof(rtl8188f_agc_table[0])) - 1, cap, rr32(REG_AFE_XTAL_CTRL));
}

static void init_phy_rf(void)   /* rtl8188fu_init_phy_rf -> rtl8xxxu_init_phy_rf(table, RF_A) */
{
    const struct rfregval *t = chip_cut == 1 ? rtl8188fu_cut_b_radioa_init_table : rtl8188fu_radioa_init_table;
    u16 rfenv = rr16(REG_FPGA0_XA_RF_SW_CTRL) & FPGA0_RF_RFENV;
    u32 v = rr32(REG_FPGA0_XA_RF_INT_OE);
    rw32(REG_FPGA0_XA_RF_INT_OE, v | BIT(20));
    ub_udelay(1);
    v = rr32(REG_FPGA0_XA_RF_INT_OE);
    rw32(REG_FPGA0_XA_RF_INT_OE, v | BIT(4));
    ub_udelay(1);
    v = rr32(REG_FPGA0_XA_HSSI_PARM2);
    rw32(REG_FPGA0_XA_HSSI_PARM2, v & ~FPGA0_HSSI_3WIRE_ADDR_LEN);
    ub_udelay(1);
    v = rr32(REG_FPGA0_XA_HSSI_PARM2);
    rw32(REG_FPGA0_XA_HSSI_PARM2, v & ~FPGA0_HSSI_3WIRE_DATA_LEN);
    ub_udelay(1);
    int n = 0;
    for (; !(t->reg == 0xff && t->val == 0xffffffff) && !errs; t++) {   /* rtl8xxxu_init_rf_regs */
        switch (t->reg) {
        case 0xfe: mdelay(50); continue;
        case 0xfd: mdelay(5); continue;
        case 0xfc: mdelay(1); continue;
        case 0xfb: ub_udelay(50); continue;
        case 0xfa: ub_udelay(5); continue;
        case 0xf9: ub_udelay(1); continue;
        }
        write_rfreg(t->reg, t->val);
        ub_udelay(1);
        n++;
    }
    u16 v16 = rr16(REG_FPGA0_XA_RF_SW_CTRL);
    rw16(REG_FPGA0_XA_RF_SW_CTRL, (v16 & ~FPGA0_RF_RFENV) | rfenv);
    lprintf("phy rf: %s table, %d RF writes; RF00=%05x RF18=%05x RF42=%05x\n", chip_cut == 1 ? "cut B" : "default",
            n, read_rfreg(0x00), read_rfreg(RF6052_REG_MODE_AG), read_rfreg(RF6052_REG_T_METER_8723B));
}

static int auto_llt(void)   /* rtl8xxxu_auto_llt_table */
{
    u32 v = rr32(REG_AUTO_LLT);
    rw32(REG_AUTO_LLT, v | AUTO_LLT_INIT_LLT);
    for (int i = 500; i && !errs; i--) {
        v = rr32(REG_AUTO_LLT);
        if (!(v & AUTO_LLT_INIT_LLT))
            return 0;
        ub_udelay(3);
    }
    lprintf("LLT table init failed, AUTO_LLT=%p\n", v);
    return -1;
}

static void set_spec_sifs(u16 cck, u16 ofdm)
{
    rw16(REG_SPEC_SIFS, ((cck << SPEC_SIFS_CCK_SHIFT) & SPEC_SIFS_CCK_MASK) |
                            ((ofdm << SPEC_SIFS_OFDM_SHIFT) & SPEC_SIFS_OFDM_MASK));
}

static void set_retry(u16 short_retry, u16 long_retry)
{
    rw16(REG_RETRY_LIMIT, ((short_retry << RETRY_LIMIT_SHORT_SHIFT) & RETRY_LIMIT_SHORT_MASK) |
                              ((long_retry << RETRY_LIMIT_LONG_SHIFT) & RETRY_LIMIT_LONG_MASK));
}

static void init_burst(void)   /* rtl8xxxu_init_burst */
{
    u8 v = rr8(REG_RXDMA_PRO_8723B);
    v = (v & ~RXDMA_PRO_DMA_BURST_SIZE) | (1 << 4);
    v = (v & ~RXDMA_PRO_DMA_BURST_CNT) | (3 << 2);
    rw8(REG_RXDMA_PRO_8723B, v | RXDMA_PRO_DMA_MODE);
    v = rr8(REG_HT_SINGLE_AMPDU_8723B);
    rw8(REG_HT_SINGLE_AMPDU_8723B, v | HT_SINGLE_AMPDU_ENABLE);
    rw16(REG_MAX_AGGR_NUM, 0x0c14);
    rw8(REG_AMPDU_MAX_TIME_8723B, 0x70);
    rw32(REG_AGGLEN_LMT, 0xffffffff);
    rw8(REG_RX_PKT_LIMIT, 0x18);
    rw8(REG_PIFS, 0x00);
    rw8(REG_FWHW_TXQ_CTRL, FWHW_TXQ_CTRL_AMPDU_RETRY);   /* 8188F */
    rw32(REG_FAST_EDCA_CTRL, 0x03086666);
    rw8(REG_USTIME_TSF_8723B, 0x28);
    rw8(REG_USTIME_EDCA, 0x28);
    v = rr8(REG_RSV_CTRL);
    rw8(REG_RSV_CTRL, v | RSV_CTRL_WLOCK_1C | RSV_CTRL_DIS_PRST);
}

/* rtl8188fu_init_aggregation: also what we want, since USB RX aggregation ends up off (one frame per bulk IN) */
static void init_aggregation(void)
{
    u32 v = rr32(REG_DWBCN0_CTRL_8188F);
    rw32(REG_DWBCN0_CTRL_8188F, (v & ~(0xf << 4)) | (6 << 4));
    rw8(REG_DWBCN1_CTRL_8723B, 6 << 1);
    u8 agg_ctrl = rr8(REG_TRXDMA_CTRL) & ~TRXDMA_CTRL_RXDMA_AGG_EN;
    u32 agg_rx = rr32(REG_RXDMA_AGG_PG_TH) & ~RXDMA_USB_AGG_ENABLE & ~0xff0f;
    u8 rxdma_mode = rr8(REG_RXDMA_PRO_8723B) & ~BIT(1);
    rw8(REG_TRXDMA_CTRL, agg_ctrl);
    rw32(REG_RXDMA_AGG_PG_TH, agg_rx);
    rw8(REG_RXDMA_PRO_8723B, rxdma_mode);
}

static void init_statistics(void)   /* rtl8188fu_init_statistics */
{
    rw16(REG_NHM_TIMER_8723B + 2, 0xc350);
    rw16(REG_NHM_TH9_TH10_8723B + 2, 0xffff);
    rw32(REG_NHM_TH3_TO_TH0_8723B, 0xffffff50);
    rw32(REG_NHM_TH7_TO_TH4_8723B, 0xffffffff);
    u32 v = rr32(REG_FPGA0_IQK);
    rw32(REG_FPGA0_IQK, v | 0xff);
    v = rr32(REG_NHM_TH9_TH10_8723B);
    rw32(REG_NHM_TH9_TH10_8723B, (v & ~(BIT(8) | BIT(9) | BIT(10))) | BIT(8));
    v = rr32(REG_OFDM0_FA_RSTC);
    rw32(REG_OFDM0_FA_RSTC, v | BIT(7));
}

/* rtl8188f_set_tx_power(channel, ht40 = false), power indexes from the efuse (struct rtl8188fu_efuse at 0x10) */
static void set_tx_power(int ch)
{
    int group = ch < 3 ? 0 : ch < 6 ? 1 : ch < 9 ? 2 : ch < 12 ? 3 : 4;
    int cck_group = ch == 14 ? 5 : group;
    u32 cck = efuse[0x10 + cck_group];
    u8 ht40 = efuse[0x16 + group];
    int ofdm_diff = (s8)(efuse[0x1b] << 4) >> 4;   /* ht20_ofdm_1s_diff.a, signed 4 bits */
    int ht20_diff = (s8)efuse[0x1b] >> 4;          /* .b */

    u32 v = rr32(REG_TX_AGC_A_CCK1_MCS32);
    rw32(REG_TX_AGC_A_CCK1_MCS32, (v & 0xffff00ff) | (cck << 8));
    v = rr32(REG_TX_AGC_B_CCK11_A_CCK2_11);
    rw32(REG_TX_AGC_B_CCK11_A_CCK2_11, (v & 0xff) | (cck << 8) | (cck << 16) | (cck << 24));
    u32 b = (u8)(ht40 + ofdm_diff);
    u32 ofdm = b | b << 8 | b << 16 | b << 24;
    rw32(REG_TX_AGC_A_RATE18_06, ofdm);
    rw32(REG_TX_AGC_A_RATE54_24, ofdm);
    b = (u8)(ht40 + ht20_diff);
    u32 mcs = b | b << 8 | b << 16 | b << 24;
    rw32(REG_TX_AGC_A_MCS03_MCS00, mcs);
    rw32(REG_TX_AGC_A_MCS07_MCS04, mcs);
    rw32(REG_TX_AGC_A_MCS11_MCS08, mcs);
    rw32(REG_TX_AGC_A_MCS15_MCS12, mcs);
}

static void lc_calibrate(void)   /* rtl8188f_phy_lc_calibrate */
{
    u32 lstf = rr32(REG_OFDM1_LSTF);
    if (lstf & OFDM_LSTF_MASK)
        rw32(REG_OFDM1_LSTF, lstf & ~OFDM_LSTF_MASK);
    else
        rw8(REG_TXPAUSE, 0xff);
    u32 rf_amode = read_rfreg(RF6052_REG_MODE_AG);
    write_rfreg(RF6052_REG_MODE_AG, rf_amode | 0x08000);
    int i;
    for (i = 0; i < 100 && !errs; i++) {
        if ((read_rfreg(RF6052_REG_MODE_AG) & 0x08000) == 0)
            break;
        mdelay(10);
    }
    write_rfreg(RF6052_REG_MODE_AG, rf_amode);
    if (lstf & OFDM_LSTF_MASK)
        rw32(REG_OFDM1_LSTF, lstf);
    else
        rw8(REG_TXPAUSE, 0x00);
    lprintf("LC calibration %s after %d polls (RF18 %05x)\n", i < 100 ? "done" : "TIMED OUT", i, rf_amode);
}

static int init_rest(void)
{
    u32 t0 = ub_ms(0);
    chip_cut = (rr32(REG_SYS_CFG) & SYS_CFG_CHIP_VERSION_MASK) >> 12;

    antenna_selection();
    init_mac();
    lprintf("antenna selection + MAC table (%d writes) done, usb errors %u\n",
            (int)(sizeof(rtl8188f_mac_init_table) / sizeof(rtl8188f_mac_init_table[0])) - 1, errs);
    init_phy_bb();
    init_phy_rf();
    flush("mac bb rf");
    if (errs)
        return -1;

    rw32(REG_FPGA0_TX_INFO, 0x00000003);   /* RFSW control */
    u32 v = FPGA0_RF_TRSW | FPGA0_RF_TRSWB | FPGA0_RF_ANTSW | FPGA0_RF_ANTSWB |
            ((FPGA0_RF_ANTSW | FPGA0_RF_ANTSWB) << FPGA0_RF_BD_CTRL_SHIFT) | FPGA0_RF_PAPE |
            (FPGA0_RF_PAPE << FPGA0_RF_BD_CTRL_SHIFT);   /* no_pape is 0 for the 8188F */
    rw32(REG_FPGA0_XAB_RF_SW_CTRL, v);
    rw32(REG_FPGA0_XA_RF_INT_OE, 0x66f60210);

    /* !macpower (needs_full_init): TX buffer boundary = TX_TOTAL_PAGE_NUM_8188F + 1 */
    u8 b = 0xf7 + 1;
    rw8(REG_TXPKTBUF_BCNQ_BDNY, b);
    rw8(REG_TXPKTBUF_MGQ_BDNY, b);
    rw8(REG_TXPKTBUF_WMAC_LBK_BF_HD, b);
    rw8(REG_TRXFF_BNDY, b);
    rw8(REG_TDECTRL + 1, b);
    rw8(REG_PBP, (PBP_PAGE_SIZE_256 << PBP_PAGE_SIZE_RX_SHIFT) | (PBP_PAGE_SIZE_256 << PBP_PAGE_SIZE_TX_SHIFT));
    if (auto_llt())
        return -1;

    /* rtl8188f_usb_quirks */
    u16 v16 = rr16(REG_CR);
    rw16(REG_CR, v16 | CR_MAC_TX_ENABLE | CR_MAC_RX_ENABLE);
    v = rr32(REG_TXDMA_OFFSET_CHK);
    rw32(REG_TXDMA_OFFSET_CHK, v | TXDMA_OFFSET_DROP_DATA_EN);

    /* has_tx_report */
    u8 v8 = rr8(REG_TX_REPORT_CTRL);
    rw8(REG_TX_REPORT_CTRL, v8 | TX_REPORT_CTRL_TIMER_ENABLE);
    rw8(REG_TX_REPORT_CTRL + 1, 0x02);
    rw16(REG_TX_REPORT_TIME, 0xcdf0);
    v8 = rr8(0xa3);
    rw8(0xa3, v8 & 0xf8);

    rw8(REG_RX_DRVINFO_SZ, 4);   /* 4 * 8 bytes of PHY status in front of every frame */
    rw32(REG_HISR0, 0xffffffff);
    rw32(REG_HISR1, 0xffffffff);

    for (int i = 0; i < 6; i++)   /* rtl8xxxu_set_mac */
        rw8(REG_MACID + i, efuse[0xd7 + i]);
    v8 = rr8(REG_MSR);            /* rtl8xxxu_set_linktype(STATION) */
    rw8(REG_MSR, (v8 & ~MSR_LINKTYPE_MASK) | MSR_LINKTYPE_STATION);

    v = RCR_ACCEPT_PHYS_MATCH | RCR_ACCEPT_MCAST | RCR_ACCEPT_BCAST | RCR_ACCEPT_MGMT_FRAME | RCR_HTC_LOC_CTRL |
        RCR_APPEND_PHYSTAT | RCR_APPEND_ICV | RCR_APPEND_MIC;
    rw32(REG_RCR, v);
    rw16(REG_RXFLTMAP2, 0xffff);   /* init_reg_rxfltmap */
    rw16(REG_RXFLTMAP1, 0x400);
    rw16(REG_RXFLTMAP0, 0xffff);

    v = rr32(REG_RESPONSE_RATE_SET);
    rw32(REG_RESPONSE_RATE_SET, (v & ~RESPONSE_RATE_BITMAP_ALL) | RESPONSE_RATE_RRSR_CCK_ONLY_1M);
    set_spec_sifs(0x10, 0x10);
    set_retry(0x30, 0x30);
    set_spec_sifs(0x0a, 0x10);
    rw16(REG_MAC_SPEC_SIFS, 0x100a);
    rw16(REG_SIFS_CCK, 0x100a);
    rw16(REG_SIFS_OFDM, 0x100a);
    rw32(REG_EDCA_BE_PARAM, 0x005ea42b);
    rw32(REG_EDCA_BK_PARAM, 0x0000a44f);
    rw32(REG_EDCA_VI_PARAM, 0x005ea324);
    rw32(REG_EDCA_VO_PARAM, 0x002fa226);
    rw32(REG_DARFRC, 0x00000000);
    rw32(REG_DARFRC + 4, 0x10080404);
    rw32(REG_RARFRC, 0x04030201);
    rw32(REG_RARFRC + 4, 0x08070605);
    v8 = rr8(REG_FWHW_TXQ_CTRL);
    rw8(REG_FWHW_TXQ_CTRL, v8 | FWHW_TXQ_CTRL_AMPDU_RETRY);
    rw8(REG_ACKTO, 0x40);
    rw16(REG_BEACON_CTRL, BEACON_DISABLE_TSF_UPDATE | (BEACON_DISABLE_TSF_UPDATE << 8));
    rw16(REG_TBTT_PROHIBIT, 0x6404);
    rw8(REG_BEACON_DMA_TIME, BEACON_DMA_ATIME_INT_TIME);   /* (no DRIVER_EARLY_INT on the 8188F) */
    rw16(REG_BEACON_TCFG, 0x660F);
    init_burst();
    init_aggregation();
    rw16(REG_PKT_VO_VI_LIFE_TIME, 0x0400);
    rw16(REG_PKT_BE_BK_LIFE_TIME, 0x0400);
    v = rr32(REG_FPGA0_RF_MODE);
    rw32(REG_FPGA0_RF_MODE, v | FPGA_RF_MODE_CCK | FPGA_RF_MODE_OFDM);
    rw32(REG_CAM_CMD, CAM_CMD_POLLING | BIT(30));
    set_tx_power(1);
    rw8(REG_HWSEQ_CTRL, 0xff);
    rw32(REG_BAR_MODE_CTRL, 0x0201ffff);
    init_statistics();
    v8 = rr8(REG_GPIO_MUXCFG);
    rw8(REG_GPIO_MUXCFG, v8 & ~GPIO_MUXCFG_IO_SEL_ENBT);
    rw8(REG_CCK_PD_THRESH, CCK_PD_TYPE1_LV1_TH);
    lprintf("MAC registers done: RCR=%p RXDMA_AGG_PG_TH=%p RXDMA_PRO=%02x TRXDMA_CTRL=%04x, usb errors %u\n",
            rr32(REG_RCR), rr32(REG_RXDMA_AGG_PG_TH), rr8(REG_RXDMA_PRO_8723B), rr16(REG_TRXDMA_CTRL), errs);
    flush("mac regs");
    if (errs)
        return -1;

    lc_calibrate();
    /* rtl8188fu_phy_iq_calibrate skipped (RX works without it, sensitivity is a little worse) */
    v = read_rfreg(RF6052_REG_T_METER_8723B);   /* gen2_thermal_meter */
    write_rfreg(RF6052_REG_T_METER_8723B, v | 0x30000);
    rw8(REG_NAV_UPPER, (30000 + NAV_UPPER_UNIT - 1) / NAV_UPPER_UNIT);
    v = rr32(REG_FWHW_TXQ_CTRL);
    rw32(REG_FWHW_TXQ_CTRL, v | FWHW_TXQ_CTRL_XMIT_MGMT_ACK);
    lprintf("init_device done in %u ms: thermal RF42 %05x, 0xa9c=%p, usb errors %u\n", ub_ms(t0),
            read_rfreg(RF6052_REG_T_METER_8723B), rr32(0xa9c), errs);
    return errs ? -1 : 0;
}

/* mac80211 order: rtl8xxxu_start (enable_rf + RX filter + IGI), add_interface(STATION), configure_filter,
 * sw_scan_start */
static void radio_on(void)
{
    /* rtl8188f_enable_rf */
    u8 pg = 0xff;
    efuse_read8(PPG_BB_GAIN_2G_TXA_OFFSET_8188F, &pg);
    if (pg != 0xff) {
        int gain = pg & 0x0f;
        if (gain == 0x0f)
            gain = 0;
        else if (gain & 1)
            gain = gain >> 1;
        else
            gain = -(gain >> 1);
        u32 val8 = gain < 0 ? -gain : gain;
        if (gain > 0)
            val8 |= BIT(5);
        u32 v = read_rfreg(RF6052_REG_UNKNOWN_55);
        write_rfreg(RF6052_REG_UNKNOWN_55, (v & ~0xfc000) | (val8 << 14));
    }
    rw8(REG_RF_CTRL, RF_ENABLE | RF_RSTB | RF_SDMRSTB);
    u32 v = rr32(REG_OFDM0_TRX_PATH_ENABLE);
    v &= ~(OFDM_RF_PATH_RX_MASK | OFDM_RF_PATH_TX_MASK);
    rw32(REG_OFDM0_TRX_PATH_ENABLE, v | OFDM_RF_PATH_RX_A | OFDM_RF_PATH_TX_A);
    rw8(REG_TXPAUSE, 0x00);

    /* rtl8xxxu_start tail */
    rw16(REG_RXFLTMAP2, 0xffff);
    rw16(REG_RXFLTMAP0, 0xffff);
    rw32_mask(REG_OFDM0_XA_AGC_CORE1, OFDM0_X_AGC_CORE1_IGI_MASK, 0x1e);

    /* rtl8xxxu_add_interface(STATION): rtl8xxxu_stop_tx_beacon, beacon function on (link type and MAC address
     * are already set by init) */
    u8 v8 = rr8(REG_FWHW_TXQ_CTRL + 2);
    rw8(REG_FWHW_TXQ_CTRL + 2, v8 & ~BIT(6));
    rw8(REG_TBTT_PROHIBIT + 1, 0x64);
    v8 = rr8(REG_TBTT_PROHIBIT + 2);
    rw8(REG_TBTT_PROHIBIT + 2, v8 & ~BIT(0));
    v8 = rr8(REG_BEACON_CTRL);
    rw8(REG_BEACON_CTRL, v8 | BEACON_ATIM | BEACON_FUNCTION_ENABLE | BEACON_DISABLE_TSF_UPDATE);

    /* configure_filter with FIF_BCN_PRBRESP_PROMISC (what mac80211 sets while scanning): beacons/probe responses
     * from any BSSID; no FCS-fail, control, other-BSS or PS-poll frames */
    v = rr32(REG_RCR);
    v &= ~(RCR_ACCEPT_CRC32 | RCR_CHECK_BSSID_BEACON | RCR_CHECK_BSSID_MATCH | RCR_ACCEPT_CTRL_FRAME |
           RCR_ACCEPT_AP | RCR_ACCEPT_PM);
    rw32(REG_RCR, v);
    v8 = rr8(REG_BEACON_CTRL);   /* rtl8xxxu_sw_scan_start */
    rw8(REG_BEACON_CTRL, v8 | BEACON_DISABLE_TSF_UPDATE);
    lprintf("radio on: efuse[0xee]=%02x RF_CTRL=%02x TRX_PATH=%p IGI=%p RCR=%p RXFLTMAP0/1/2=%04x/%04x/%04x\n", pg,
            rr8(REG_RF_CTRL), rr32(REG_OFDM0_TRX_PATH_ENABLE), rr32(REG_OFDM0_XA_AGC_CORE1), rr32(REG_RCR),
            rr16(REG_RXFLTMAP0), rr16(REG_RXFLTMAP1), rr16(REG_RXFLTMAP2));
}

/* ---- channel switch: rtl8188fu_config_channel for 20 MHz ---- */
static void spur_calibration(int ch)   /* rtl8188f_spur_calibration */
{
    static const u32 freq[15] = { [5] = 0xFCCD, [6] = 0xFC4D, [7] = 0xFFCD, [8] = 0xFF4D, [11] = 0xFDCD,
                                  [13] = 0xFCCD, [14] = 0xFF9A };
    static const u32 d40[15] = { [5] = 0x06000000, [6] = 0x00000600, [13] = 0x06000000 };
    static const u32 d44[15] = { [11] = 0x04000000 };
    static const u32 d4c[15] = { [7] = 0x06000000, [8] = 0x00000380, [14] = 0x00180000 };

    u32 v = rr32(REG_OFDM0_RX_D_SYNC_PATH);
    rw32(REG_OFDM0_RX_D_SYNC_PATH, v | GENMASK(28, 24));
    v = rr32(REG_OFDM0_RX_D_SYNC_PATH);
    rw32(REG_OFDM0_RX_D_SYNC_PATH, v | BIT(9));   /* notch filter */

    if (ch <= 14 && freq[ch]) {
        u32 reg948 = rr32(REG_S0S1_PATH_SWITCH);
        int hw_ctrl = !!(reg948 & BIT(6)), s1 = 0;
        if (hw_ctrl)
            s1 = (rr32(REG_FPGA0_XB_RF_INT_OE) & GENMASK(5, 3)) == BIT(3);
        else
            s1 = !(reg948 & BIT(9));
        if (s1) {
            u32 gain = rr32(REG_OFDM0_XA_AGC_CORE1);
            v = rr32(REG_FPGA0_RF_MODE);
            rw32(REG_FPGA0_RF_MODE, v & ~FPGA_RF_MODE_CCK);
            rw32(REG_OFDM0_XA_AGC_CORE1, (gain & ~OFDM0_X_AGC_CORE1_IGI_MASK) | 0x30);
            rw32(REG_FPGA0_ANALOG4, 0xccf000c0);   /* 3-wire off */
            rw32(REG_FPGA0_PSD_FUNC, freq[ch]);
            rw32(REG_FPGA0_PSD_FUNC, 0x400000 | freq[ch]);
            mdelay(30);
            u32 psd = rr32(REG_FPGA0_PSD_REPORT);
            rw32(REG_FPGA0_PSD_FUNC, freq[ch]);
            rw32(REG_FPGA0_ANALOG4, 0xccc000c0);   /* 3-wire on */
            v = rr32(REG_FPGA0_RF_MODE);
            rw32(REG_FPGA0_RF_MODE, v | FPGA_RF_MODE_CCK);
            rw32(REG_OFDM0_XA_AGC_CORE1, gain);
            lprintf("  spur check ch %d: PSD %u%s\n", ch, psd, psd >= 0x16 ? " -> notch" : "");
            if (psd >= 0x16) {
                rw32(REG_OFDM1_CSI_FIX_MASK1, d40[ch]);
                rw32(REG_OFDM1_CSI_FIX_MASK2, d44[ch]);
                rw32(0xd48, 0x0);
                rw32(0xd4c, d4c[ch]);
                v = rr32(REG_OFDM1_CFO_TRACKING);
                rw32(REG_OFDM1_CFO_TRACKING, v | BIT(28));   /* CSI mask on */
                return;
            }
        }
    }
    v = rr32(REG_OFDM1_CFO_TRACKING);
    rw32(REG_OFDM1_CFO_TRACKING, v & ~BIT(28));
}

static void config_channel(int ch)
{
    u32 v = read_rfreg(RF6052_REG_MODE_AG);
    write_rfreg(RF6052_REG_MODE_AG, (v & ~MODE_AG_CHANNEL_MASK) | ch);
    spur_calibration(ch);
    v = rr32(REG_FPGA0_RF_MODE);
    rw32(REG_FPGA0_RF_MODE, v & ~FPGA_RF_MODE);   /* 20 MHz */
    v = rr32(REG_FPGA1_RF_MODE);
    rw32(REG_FPGA1_RF_MODE, v & ~FPGA_RF_MODE);
    v = rr32(REG_FPGA0_RF_MODE);
    rw32(REG_FPGA0_RF_MODE, v | GENMASK(10, 8));   /* RXADC clk */
    v = rr32(REG_FPGA0_RF_MODE);
    rw32(REG_FPGA0_RF_MODE, (v | BIT(14) | BIT(12)) & ~BIT(13));   /* TXDAC clk */
    v = rr32(REG_OFDM0_TX_PSDO_NOISE_WEIGHT);
    rw32(REG_OFDM0_TX_PSDO_NOISE_WEIGHT, v & ~GENMASK(31, 30));   /* small BW */
    v = rr32(REG_OFDM0_TX_PSDO_NOISE_WEIGHT);
    rw32(REG_OFDM0_TX_PSDO_NOISE_WEIGHT, (v & ~BIT(29)) | BIT(28));   /* adc buffer clk */
    v = rr32(REG_OFDM0_XA_RX_AFE);
    rw32(REG_OFDM0_XA_RX_AFE, (v & ~BIT(29)) | BIT(28));
    v = rr32(REG_OFDM_RX_DFIR);
    rw32(REG_OFDM_RX_DFIR, v & ~BIT(19));
    v = rr32(REG_OFDM_RX_DFIR);
    rw32(REG_OFDM_RX_DFIR, (v & ~GENMASK(23, 20)) | BIT(21) | BIT(20));
    write_rfreg(RF6052_REG_MODE_AG, ch | MODE_AG_BW_20MHZ_8723B);   /* RF TRX_BW */
    write_rfreg(RF6052_REG_RXG_MIX_SWBW, 0x00065);   /* filter BW & RC corner (ACPR) */
    write_rfreg(RF6052_REG_RX_BB2, 0x0);
    write_rfreg(RF6052_REG_GAIN_CCA, 0x00140);   /* RC corner */
    write_rfreg(RF6052_REG_RX_G2, 0x01c6c);
}

/* ---- passive scan over bulk IN ---- */
#define RXBUF_LEN 16384
static u8 rxbuf[RXBUF_LEN] __attribute__((aligned(4096)));   /* page aligned: one qTD, 4 buffer pointers */

struct chstat {
    u32 reads, empty, timeouts, errors, bytes, frames, crc, c2h, trunc, runts, bcn, ms;
    u32 type[4];   /* mgmt, ctrl, data, ext */
};
static struct chstat chs[15];

struct bss {
    u8 addr[6];
    u8 ch, ds, ssid_len, subtype;
    s8 rssi;
    u16 n;
    char ssid[32 * 4 + 1];   /* escaped */
};
#define MAX_BSS 64
static struct bss bsses[MAX_BSS];
static int n_bss, bss_dropped, dumps, bulk_errs;

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* rtl8723au_rx_parse_phystats: rtl8188f_cck_rssi for CCK rates, pwdb_all for OFDM */
static int signal_dbm(const u8 *phy, u32 rxmcs)
{
    if (rxmcs < 4) {   /* DESC_RATE_6M */
        int lna = phy[5] >> 5, vga = phy[5] & 0x1f;
        switch (lna) {
        case 7: return vga <= 27 ? -100 + 2 * (27 - vga) : -100;
        case 5: return -74 + 2 * (21 - vga);
        case 3: return -60 + 2 * (20 - vga);
        case 1: return -44 + 2 * (19 - vga);
        default: return 0;
        }
    }
    return (phy[4] >> 1) - 110;
}

/* SSID for the log: printable ASCII as is, everything else (and \\ and ') as \\xHH; up to 4 * 32 + 1 bytes */
static void esc_ssid(char *o, const u8 *s, int n)
{
    int nonzero = 0;
    for (int i = 0; i < n; i++)
        nonzero |= s[i];
    if (!nonzero) {
        for (const char *h = "<hidden>"; *h;)
            *o++ = *h++;
        *o = 0;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (s[i] >= 0x20 && s[i] < 0x7f && s[i] != '\\' && s[i] != '\'') {
            *o++ = s[i];
        } else {
            *o++ = '\\';
            *o++ = 'x';
            o = fmt_num(o, s[i], 16, 2, '0', 0);
        }
    }
    *o = 0;
}

static void handle_frame(int ch, const u8 *f, int len, int err, const u8 *phy, u32 rxmcs)
{
    struct chstat *c = &chs[ch];
    c->frames++;
    if (len < 10) {
        c->runts++;
        return;
    }
    int type = (f[0] >> 2) & 3, sub = (f[0] >> 4) & 15;
    c->type[type]++;
    if (err) {
        c->crc++;
        return;
    }
    if (type != 0 || (sub != 8 && sub != 5) || len < 36)
        return;
    c->bcn++;
    const u8 *ssid = 0;
    int ssid_len = 0, ds = 0;
    for (int i = 36; i + 2 <= len && i + 2 + f[i + 1] <= len; i += 2 + f[i + 1]) {
        if (f[i] == 0 && !ssid) {
            ssid = f + i + 2;
            ssid_len = f[i + 1] > 32 ? 32 : f[i + 1];
        } else if (f[i] == 3 && f[i + 1] == 1) {
            ds = f[i + 2];
        }
    }
    int rssi = phy ? signal_dbm(phy, rxmcs) : 0;
    const u8 *a = f + 16;   /* addr3 = BSSID */
    struct bss *b = 0;
    for (int i = 0; i < n_bss && !b; i++)
        if (bsses[i].addr[0] == a[0] && bsses[i].addr[1] == a[1] && bsses[i].addr[2] == a[2] &&
            bsses[i].addr[3] == a[3] && bsses[i].addr[4] == a[4] && bsses[i].addr[5] == a[5])
            b = &bsses[i];
    if (b) {
        b->n++;
        if (rssi > b->rssi)
            b->rssi = rssi;
        return;
    }
    if (n_bss >= MAX_BSS) {
        bss_dropped++;
        return;
    }
    b = &bsses[n_bss++];
    for (int i = 0; i < 6; i++)
        b->addr[i] = a[i];
    b->ch = ch, b->ds = ds, b->rssi = rssi, b->n = 1, b->subtype = sub, b->ssid_len = ssid_len;
    esc_ssid(b->ssid, ssid ? ssid : f, ssid ? ssid_len : 0);
    lprintf("  NEW ch%02d %02x:%02x:%02x:%02x:%02x:%02x DS=%d %d dBm rate %02x %s '%s'\n", ch, a[0], a[1], a[2], a[3],
            a[4], a[5], ds, rssi, rxmcs, sub == 8 ? "beacon" : "probe-resp", b->ssid);
}

/* rtl8xxxu_parse_rxdesc24 over one bulk IN transfer, which may hold several packets */
static void parse_rx(int ch, int n)
{
    int off = 0;
    while (off + 24 <= n) {
        const u8 *d = rxbuf + off;
        u32 d0 = le32(d), d2 = le32(d + 8), d3 = le32(d + 12);
        int pktlen = d0 & 0x3fff, drv = ((d0 >> 16) & 0xf) * 8, shift = (d0 >> 24) & 3;
        int hdr = 24 + drv + shift;
        if (dumps < 4) {
            dumps++;
            lprintf("  rx sample (transfer %d bytes, offset %d): pktlen %d drvinfo %d shift %d phy %d crc %d icv %d "
                    "rpt_sel %d rxmcs %02x\n", n, off, pktlen, drv, shift, !!(d0 & BIT(26)), !!(d0 & BIT(14)),
                    !!(d0 & BIT(15)), !!(d2 & BIT(28)), d3 & 0x7f);
            hexdump(d, n - off < 96 ? n - off : 96, off);
        }
        if (hdr + pktlen > n - off) {
            chs[ch].trunc++;
            break;
        }
        if (d2 & BIT(28))   /* rpt_sel: C2H report from the firmware, not a frame */
            chs[ch].c2h++;
        else
            handle_frame(ch, d + hdr, pktlen, (d0 >> 14) & 3, (d0 & BIT(26)) && drv >= 8 ? d + 24 : 0, d3 & 0x7f);
        off += (hdr + pktlen + 7) & ~7;
    }
}

/* Dwell on one channel. Returns -1 when the time budget is used up. */
static int scan_channel(int ch, u32 pipe)
{
    struct chstat *c = &chs[ch];
    u32 t0 = ub_ms(0);
    set_tx_power(ch);
    config_channel(ch);
    u32 cfg = ub_ms(t0), t1 = ub_ms(0);
    int late = 0;
    while (ub_ms(t1) < DWELL_MS && !errs) {
        if (ub_ms(0) + EHCI_BULK_MS + 500 > T_SCAN_END) {
            late = 1;
            break;
        }
        hb_tick();
        int actual = 0;
        u32 tr = ub_ms(0);
        int r = ub_bulk(wdev, pipe, rxbuf, RXBUF_LEN, &actual, 100);
        u32 dt = ub_ms(tr);
        c->reads++;
        if (r < 0) {
            if (dt >= EHCI_BULK_MS - 1000) {   /* nothing at all for 5 s: this channel is quiet */
                c->timeouts++;
                break;
            }
            c->errors++, bulk_errs++;
            lprintf("  bulk IN error %d after %u ms, status=%x act_len=%d actual=%d\n", r, dt,
                    rd32(wdev + UDEV_STATUS), rd32(wdev + UDEV_ACT_LEN), actual);
            if (c->errors >= 3 || bulk_errs >= 10)
                break;
            continue;
        }
        if (actual > RXBUF_LEN)
            actual = RXBUF_LEN;
        c->bytes += actual;
        if (actual >= 24)
            parse_rx(ch, actual);
        else
            c->empty++;
    }
    c->ms = ub_ms(t0);
    lprintf("ch%02d: %u ms (switch %u): %u reads, %u frames (mgmt %u ctrl %u data %u), %u beacon/probe-resp, "
            "%u crc/icv err, %u c2h, %u short, %u empty, %u truncated, %u timeouts, %u errors, %u bytes, %d BSS\n",
            ch, c->ms, cfg, c->reads, c->frames, c->type[0], c->type[1], c->type[2], c->bcn, c->crc, c->c2h, c->runts,
            c->empty, c->trunc, c->timeouts, c->errors, c->bytes, n_bss);
    return late ? -1 : 0;
}

static void scan(void)
{
    if (!bulk_in_ep) {
        log_s("no bulk IN endpoint from the descriptor, using 1\n");
        bulk_in_ep = 1;
    }
    u32 pipe = bulk_in(wdev, bulk_in_ep);
    lprintf("\n# passive scan, bulk IN ep %d pipe %p, %u ms dwell, rx buffer %p/%d, until t=%u\n", bulk_in_ep, pipe,
            DWELL_MS, (u32)rxbuf, RXBUF_LEN, T_SCAN_END);
    int ch;
    for (ch = 1; ch <= 13 && !errs && bulk_errs < 10; ch++) {
        if (scan_channel(ch, pipe)) {
            lprintf("time budget reached at t=%u, stopping before channel %d is done\n", ub_ms(0), ch);
            break;
        }
        if (ch % 3 == 0)
            flush("scan");
    }
    lprintf("\n# scan summary (t=%u, %d BSS, %d not stored, usb errors %u, bulk errors %d)\n", ub_ms(0), n_bss,
            bss_dropped, errs, bulk_errs);
    for (int i = 0; i < n_bss; i++) {
        struct bss *b = &bsses[i];
        lprintf("BSS %02x:%02x:%02x:%02x:%02x:%02x first ch%02d DS=%d best %d dBm seen %u x %s '%s'\n", b->addr[0],
                b->addr[1], b->addr[2], b->addr[3], b->addr[4], b->addr[5], b->ch, b->ds, b->rssi, b->n,
                b->subtype == 8 ? "beacon" : "probe-resp", b->ssid);
    }
    flush("scan done");
}

/* Diagnostic, after everything else: do multi-packet control IN transfers work (vendor read of 64/128 bytes)? */
static void diag_ctrl_in(void)
{
    static u8 big[128] __attribute__((aligned(64)));
    static const u8 lens[] = { 4, 64, 65, 128 };
    log_s("\n# control IN size probe (vendor read at 0x0000)\n");
    for (unsigned i = 0; i < sizeof(lens); i++) {
        for (int j = 0; j < 128; j++)
            big[j] = 0xa5;
        if (ctrl_raw(1, 0x0000, big, lens[i]) > 0)
            hexdump(big, lens[i] < 32 ? lens[i] : 32, 0);
    }
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

    if (errs || firmware())
        goto out;
    flush("firmware");

    if (ub_ms(0) > T_INIT_LATEST) {
        lprintf("t=%u is too late for the radio init\n", ub_ms(0));
        goto out;
    }
    if (init_rest())
        goto out;
    radio_on();
    flush("radio on");
    if (errs)
        goto out;
    scan();
    if (!errs && ub_ms(0) < T_SCAN_END)
        diag_ctrl_in();
out:
    lprintf("usb errors: %u, t=%u\n", errs, ub_ms(0));
}

static void hello(int argc, char *const argv[])
{
    log_len = 0;
    lprintf("hello from wifi2 payload, argc=%d", argc);
    for (int i = 0; i < argc; i++)
        lprintf(" [%s]", argv[i]);
    log_c('\n');
    report_cpu();
    check_uboot();
    lprintf("end\n");
    for (u32 i = 0; i < 0x800; i++)   /* HELLO.TXT is the first 0x800 bytes */
        HELLO_BUF[i] = i < log_len ? LOG_BUF[i] : ' ';
}

/* Bytes the MCU sends us, as hex text with a ms timestamp per burst (kept across Annn.TXT files). */
#define RX_TXT ((char *)0x01600000)
#define RX_MAX 0x4000
static u32 rx_len, rx_bytes, rx_last;

static void rx_poll(void)
{
    for (int k = 0; k < 64 && (*MCU_UART_LSR & 2); k++) {
        u8 b = *MCU_UART_DATA;
        u32 now = ub_ms(0);
        if (rx_len + 24 > RX_MAX)
            continue;
        if (!rx_bytes || now - rx_last > 50) {   /* new burst: newline + timestamp */
            char *p = RX_TXT + rx_len;
            *p++ = '\n';
            p = fmt_num(p, now, 10, 0, ' ', 0);
            *p++ = ':';
            rx_len = p - RX_TXT;
        }
        char *p = RX_TXT + rx_len;
        *p++ = ' ';
        *p++ = "0123456789abcdef"[b >> 4];
        *p++ = "0123456789abcdef"[b & 15];
        rx_len = p - RX_TXT;
        rx_bytes++;
        rx_last = now;
    }
}

/* Heartbeat loop: Annn.TXT every 20 s for 6 min, each with the uptime and everything the MCU has sent so far. If the
 * last file is well past ~150 s of U-Boot uptime, whatever stage1 did before this stopped the reset. */
static void alive(void)
{
    u32 t0 = ub_ms(0);
    rx_len = rx_bytes = 0;
    for (int n = 0; n < 18; n++) {
        while (ub_ms(t0) < (u32)n * 20000) {
            hb_tick();
            rx_poll();
            ub_udelay(200);
        }
        char *p = HELLO_BUF, name[56], *q = name;
        const char *a = "alive ms=";
        while (*a)
            *p++ = *a++;
        p = fmt_num(p, ub_ms(0), 10, 0, ' ', 0);
        a = " heartbeats=";
        while (*a)
            *p++ = *a++;
        p = fmt_num(p, hb_count, 10, 0, ' ', 0);
        a = " tx_stuck=";
        while (*a)
            *p++ = *a++;
        p = fmt_num(p, hb_stuck, 10, 0, ' ', 0);
        a = " mcu_rx_bytes=";
        while (*a)
            *p++ = *a++;
        p = fmt_num(p, rx_bytes, 10, 0, ' ', 0);
        for (u32 i = 0; i < rx_len; i++)
            *p++ = RX_TXT[i];
        *p++ = '\n';
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
    log_s("wifi2 main\n");
    report_cpu();
    if (check_uboot())
        return 2;   /* log stays in RAM at 0x1a00000; calling U-Boot now is unsafe */
    lprintf("t=%u ms since boot\n", ub_ms(0));
    hb_tick();
    install_hook();
    flush("hooked");

    log_s("\n# U-Boot USB\n");
    run("usb start");
    flush("usb start");
    run("usb tree");
    run("usb info");
    flush("usb info");

    wifi();
    lprintf("heartbeats sent: %u, tx stuck: %u\n", hb_count, hb_stuck);
    flush("done");
    for (int i = 0; i < 2; i++) {
        if (hooked[i]) {
            hooked[i]->putc = orig_putc[i];
            hooked[i]->puts = orig_puts[i];
        }
    }
    return 0;
}
