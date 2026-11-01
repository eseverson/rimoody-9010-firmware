/* Wi-Fi client, round 10 (sd-10-net). Runs from U-Boot `go` on the SPHE8268K head unit, borrowing the stock U-Boot
 * 2014.04 (May 05 2024), gd in $r10. U-Boot is linked at 0xF00040 but relocates itself to the top of the 128 MB RAM;
 * every U-Boot address below is the link address plus `reloc`, taken from the $gp that U-Boot's `go` leaves us.
 *
 *   go 0x1800000 hello   CPU/gd report into HELLO_BUF, returns. Touches nothing else.
 *   go 0x1800000 main    reads /WIFI.TXT (ssid=, psk=, token=, optional ip=), runs `usb start`, brings the RTL8188FTV
 *                        up as in round 7, scans channels 1/6/11 (then all) for the SSID, joins the strongest BSS
 *                        (open auth, association, WPA2-PSK 4-way handshake, software CCMP), gets an address by DHCP
 *                        (or ip= after 10 s), answers ARP and ping, serves the HU1 command protocol on UDP 5151 and
 *                        broadcasts a hello to UDP 5152 every 2 s. Runs until the unit resets; rejoins after a
 *                        deauth, a failed handshake or 10 s without beacons. Talks to the MCU on UART1 (mcu.c):
 *                        heartbeat, frame parser, the a4 time handshake, the 00 shutdown request.
 *   HU1 EXEC             replaces this payload in place with an image uploaded to RAM (tramp.c at 0x02f00000).
 *
 * Fixed RAM: 0x01500000 HELLO.TXT text, 0x01600000 MCU RX hex, 0x01800000 this payload + bss (< 0x1900000),
 * 0x01a00000 log (960 KB ring), 0x01b00000 fatwrite bounce buffer for the log (256 KB), 0x02000000 HU1 reply buffer (64 KB + 256), 0x02100000 WIFI.TXT (4 KB, wiped after
 * parsing), 0x02f00000 EXEC trampoline (+0xf000 its argv), 0x03000000 where hu.py uploads images. HU1 WRITE refuses
 * the payload, the log and the reply buffer. The image header at +0x40 ("HUNE", 1, build id) comes from head.S.
 *
 * The heartbeat is the frame eCos sends every 4 s on the MCU link (UART1, 0x9C000980, 115200): ff aa 02 d1 00 2d be
 * ba; stage1.txt sets UART1's pinmux and runs `uartsp init 1 115200`; hb_tick() sends it every 3 s.
 *
 * Register sequences follow Linux drivers/net/wireless/realtek/rtl8xxxu (v6.6); register names come from that
 * driver's rtl8xxxu_regs.h, the init tables from gen_tables.py. Crypto (crypto.c), the supplicant (wpa.c) and the IP
 * layer (ip.c) are platform independent and host-tested in test/. Keys stay in software: received frames arrive still
 * encrypted (REG_SECURITY_CFG untouched) and are checked/decrypted here; transmitted frames are encrypted here. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef signed char s8;

#define BIT(n) (1u << (n))
#define GENMASK(h, l) ((~0u << (l)) & (~0u >> (31 - (h))))
#include "rtl8xxxu_regs.h"
#include "crypto.h"
#include "wpa.h"
#include "ip.h"
#include "mcu.h"
#include "irq.h"
#include "gt9.h"
#include "clk.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "freertos/port/port_nds32.h"
#include "console.h"

static char crash_brief[64];
static int pending_crash_save;   /* "vec7/itype..@ipc" of a crash record found at start, for PING */
static int crash_pings;
static unsigned int crash_until;
static void console_poll(void);

extern const char hu_build_id[];
extern const u8 tramp_bin[], tramp_bin_end[];

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
/* Time comes from clk.c (the same 1 MHz counter U-Boot's timer reads), no longer through U-Boot's get_timer/udelay:
 * any task may call these. The names stay so the call sites read as before. */
#define ub_ms(base)   (clk_ms() - (u32)(base))
#define ub_udelay(us) clk_udelay(us)
#define ub_usb_dev(i) ((u8 *(*)(int))UB(UB_USB_GET_DEV_INDEX))(i)
typedef int (*ctrl_fn)(void *dev, u32 pipe, u8 req, u8 reqtype, u16 value, u16 index, void *data, u16 size,
                       int timeout);
static int own_usb;   /* 1: Wi-Fi transfers through ehci.c instead of U-Boot (default after `usb start`; HU1 op 16) */
static int own_enum;  /* 1: usbh.c enumerated the dongle (U-Boot's USB never started): U-Boot transfers impossible */
#define ub_ctrl (own_usb ? (ctrl_fn)ehci_ctrl : (ctrl_fn)UB(UB_USB_CONTROL_MSG))
typedef int (*bulk_fn)(void *dev, u32 pipe, void *data, int len, int *actual, int timeout);
#define ub_bulk (own_usb ? (bulk_fn)ehci_bulk : (bulk_fn)UB(UB_USB_BULK_MSG))

#include "usbdev.h"
#include "ehci.h"
#include "usbh.h"

/* ---- time budget, in ub_ms(0) (which starts ~3 s after U-Boot itself; the unit resets ~150 s after U-Boot) ---- */
#define EHCI_BULK_MS   5000    /* ehci_submit_async gives up on a bulk TD after 5000 ms, others after 1000 (0xf7e562) */

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

static u32 log_len, log_hooked;
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

/* with interrupts on, the tick ISR also writes UART1 (heartbeat): keep each frame whole */
static void uart_write(const u8 *p, int len)
{
    u32 f = irq_save();
    irq_uart_finish();
    for (int i = 0; i < len; i++) {
        int n = 100000;
        while (!(*MCU_UART_LSR & 1) && --n)
            ;
        if (!n)
            hb_stuck++;
        *MCU_UART_DATA = p[i];
    }
    irq_restore(f);
}

static void hb_send(void)
{
    uart_write(hb_frame, sizeof(hb_frame));
    hb_count++;
}

/* The log is a ring of sorts: when full, its newer half moves to the front. */
static void log_c(char c)   /* any task: GIE off around the append */
{
    u32 f = irq_save();
    if (log_len >= LOG_MAX) {
        for (u32 i = 0; i < LOG_MAX / 2; i++)
            LOG_BUF[i] = LOG_BUF[LOG_MAX / 2 + i];
        log_len = LOG_MAX / 2;
    }
    LOG_BUF[log_len++] = c;
    irq_restore(f);
}

/* HU1 CMD: console output of the command being run also goes here */
static u8 *cap_buf;
static int cap_len, cap_max;

static void cap_c(char c)
{
    if (cap_buf && cap_len < cap_max)
        cap_buf[cap_len++] = c;
}

static void cap_s(const char *s)
{
    while (*s)
        cap_c(*s++);
}

static void log_s(const char *s)
{
    while (*s)
        log_c(*s++);
}

/* Console hooks: keep U-Boot's output in the log, then pass it on to the UART as before. */
static void hook_putc0(const char c) { log_c(c); cap_c(c); orig_putc[0](c); }
static void hook_puts0(const char *s) { log_s(s); cap_s(s); orig_puts[0](s); }
static void hook_putc1(const char c) { log_c(c); cap_c(c); orig_putc[1](c); }
static void hook_puts1(const char *s) { log_s(s); cap_s(s); orig_puts[1](s); }

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

/* Bounded: writes at most max-1 characters plus the NUL. An unbounded version let a 900-character HU1 command, logged
 * through the 512-byte fmtbuf, overwrite the variables after it (reloc among them) and crash on the next U-Boot call. */
#define PUT(c) do { if (p < end) *p++ = (c); } while (0)
static int vfmt(char *out, int max, const char *f, va_list ap)
{
    char *p = out, *end = out + max - 1, nb[48], *q;
    for (; *f; f++) {
        if (*f != '%') {
            PUT(*f);
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
        if (width > 32)
            width = 32;
        q = nb;
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            for (; *s; s++)   /* not PUT(*s++): PUT skips its argument once the buffer is full */
                PUT(*s);
            break;
        }
        case 'c': PUT((char)va_arg(ap, int)); break;
        case 'd': {
            int v = va_arg(ap, int);
            q = fmt_num(nb, v < 0 ? -(u32)v : (u32)v, 10, width, pad, v < 0);
            break;
        }
        case 'u': q = fmt_num(nb, va_arg(ap, u32), 10, width, pad, 0); break;
        case 'p': width = 8; pad = '0'; /* fall through */
        case 'x': q = fmt_num(nb, va_arg(ap, u32), 16, width, pad, 0); break;
        case 0: f--; break;
        default: PUT(*f); break;
        }
        for (char *r = nb; r < q; r++)
            PUT(*r);
    }
    *p = 0;
    return p - out;
}
#undef PUT
#define FMT_ANY 0x10000   /* sprintf into a caller buffer of unknown size; callers keep these short */


static int vfmt_n(char *out, const char *f, ...)   /* sprintf */
{
    va_list ap;
    va_start(ap, f);
    int n = vfmt(out, FMT_ANY, f, ap);
    va_end(ap);
    return n;
}

static void lprintf(const char *f, ...)   /* any task: formats on the caller's stack, appends with GIE off */
{
    char b[512];
    va_list ap;
    va_start(ap, f);
    vfmt(b, sizeof b, f, ap);
    va_end(ap);
    u32 fl = irq_save();
    log_s(b);
    irq_restore(fl);
}

static char cmdbuf[160];

static void set_note(const char *s)   /* what U-Boot is doing, for a crash record */
{
    int i = 0;
    for (; s && s[i] && i < 47; i++)
        irq_note[i] = s[i];
    irq_note[i] = 0;
    irq_uptime_ms = ub_ms(0);
}

static int run(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vfmt(cmdbuf, sizeof cmdbuf, f, ap);
    va_end(ap);
    lprintf("=> %s\n", cmdbuf);
    set_note(cmdbuf);
    if (cmdbuf[0] == 'u' && cmdbuf[1] == 's' && cmdbuf[2] == 'b')
        ehci_rx_stop();   /* U-Boot's USB code expects its own async list */
    int r = ub_run(cmdbuf);
    set_note(0);
    lprintf("[rc=%d t=%u]\n", r, ub_ms(0));
    return r;
}

/* Card writes are rare on purpose: repeated same-name fatwrites across boots corrupted the card's FAT before, and the
 * log is on the screen and behind `hu.py log`. flush() only marks the log; save_log() writes the newest <= 256 KB to a
 * new file, copied first into a 64-byte aligned bounce buffer (U-Boot's SD driver moves data by halfword PIO,
 * 0xf7707a: an odd source address faulted, and the log ring may move while fatwrite reads it). Used for a crash report
 * and for the MCU's shutdown request only. */
#define LOG_BOUNCE ((char *)0x01b00000)   /* 0x40000 bytes, after the log (0x01a00000-0x01af0000) */
static void hb_tick(void);

static void flush(const char *why)
{
    hb_tick();
    lprintf("--- %s t=%u\n", why, ub_ms(0));
}

static void save_log(const char *name)
{
    hb_tick();
    lprintf("--- saving the log as %s t=%u\n", name, ub_ms(0));
    u32 len = log_len, start = len > 0x40000 ? len - 0x40000 : 0;
    len -= start;
    for (u32 i = 0; i < len; i++)
        LOG_BOUNCE[i] = LOG_BUF[start + i];
    char cmd[96];
    char *p = cmd;
    const char *a = "fatwrite $isp_if $isp_dev 1b00000 ";
    while (*a)
        *p++ = *a++;
    while (*name)
        *p++ = *name++;
    *p++ = ' ';
    p = fmt_num(p, len, 16, 0, ' ', 0);
    *p = 0;
    set_note(cmd);
    ub_run(cmd);
    set_note(0);
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

static void rx_poll(void);

/* called from every register access, mdelay and flush: also drains the MCU UART so its FIFO does not overrun */
static void hb_tick(void)
{
    irq_pet();
    rx_poll();
    if (irq_is_on())
        return;   /* the timer ISR sends the heartbeat */
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

static int n_out_eps, bulk_in_ep, out_ep[4];

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
                if (!(d[2] & 0x80)) {
                    if (n_out_eps < 4)
                        out_ep[n_out_eps] = d[2] & 0x0f;
                    n_out_eps++;
                }
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


/* ======================================================================================================== */
/* ---- receive buffer, signal, SSID escaping ---- */
#define RXBUF_LEN 16384
static u8 rxbuf[RXBUF_LEN] __attribute__((aligned(4096)));   /* page aligned: one qTD, 4 buffer pointers */

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 be16(const u8 *p) { return p[0] << 8 | p[1]; }

static void cpy(void *d, const void *s, int n)
{
    for (int i = 0; i < n; i++)
        ((u8 *)d)[i] = ((const u8 *)s)[i];
}

static int same(const u8 *a, const u8 *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

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

static char *fmt_mac(char *o, const u8 *m)
{
    for (int i = 0; i < 6; i++) {
        o = fmt_num(o, m[i], 16, 2, '0', 0);
        if (i < 5)
            *o++ = ':';
    }
    *o = 0;
    return o;
}

/* Bytes the MCU sends us: raw hex text with a ms timestamp per burst at RX_TXT (as before), and through the frame
 * parser in mcu.c, which logs one decoded line per frame and answers a4 / 00. */
#define RX_TXT ((char *)0x01600000)
#define RX_MAX 0x4000
static u32 rx_len, rx_bytes, rx_last;
static struct mcu mcu_st;

static void rx_poll(void)
{
    static int busy;
    if (busy)
        return;   /* mcu.c can log and flush, and flush calls back in here */
    busy = 1;
    for (int k = 0; k < 256; k++) {
        u8 b;
        if (!irq_rx_get(&b)) {   /* bytes the tick ISR drained first; the UART itself only while IRQs are off */
            if (irq_is_on() || !(*MCU_UART_LSR & 2))
                break;
            b = *MCU_UART_DATA;
        }
        u32 now = ub_ms(0);
        rx_bytes++;
        if (rx_len + 24 <= RX_MAX) {
            if (rx_bytes == 1 || now - rx_last > 50) {   /* new burst: newline + timestamp */
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
        }
        rx_last = now;
        mcu_rx_byte(&mcu_st, b, now);
    }
    mcu_poll(&mcu_st, ub_ms(0));
    busy = 0;
}

/* ---- randomness for the SNonce and the DHCP xid: SHA-1 over a pool fed with timer values, the chip's TSF and the
 * PHY status / TSF fields of received frames ---- */
static u8 rng_pool[20];
static u32 rng_ctr;

static void rng_mix(const u8 *p, int n)
{
    struct sha1_ctx c;
    u32 t[2] = { ub_ms(0), rng_ctr++ };
    sha1_init(&c);
    sha1_update(&c, rng_pool, 20);
    sha1_update(&c, (const u8 *)t, 8);
    sha1_update(&c, p, n);
    sha1_final(&c, rng_pool);
}

static void rng_bytes(u8 *out, int n)
{
    while (n > 0) {
        u8 t[8];
        u32 tsf = rr32(REG_TSFTR), tsf_hi = rr32(REG_TSFTR + 4);
        cpy(t, &tsf, 4), cpy(t + 4, &tsf_hi, 4);
        rng_mix(t, 8);
        for (int i = 0; i < 16 && n > 0; i++, n--)
            *out++ = rng_pool[i];
        rng_mix((const u8 *)"out", 3);
    }
}

/* ---- configuration from /WIFI.TXT (never logged, wiped from RAM after parsing) ---- */
#define WIFI_TXT ((char *)0x02100000)
static u8 cfg_ssid[33], cfg_pmk[32], cfg_token[32];
static int cfg_ssid_len;
static u32 cfg_ip;

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

static int load_config(void)
{
    for (int i = 0; i < 4096; i++)
        WIFI_TXT[i] = 0;
    /* the stage-1 loads that work use no size argument; U-Boot's own ispsp code names files with a leading slash */
    if (run("fatload $isp_if $isp_dev 2100000 WIFI.TXT") && run("fatload $isp_if $isp_dev 2100000 /WIFI.TXT") &&
        run("fatload $isp_if $isp_dev 2100000 /WIFI.TXT fff")) {
        log_s("config: cannot read /WIFI.TXT (lines ssid=, psk=, token=, optional ip=)\n");
        return -1;
    }
    WIFI_TXT[4095] = 0;
    static char psk[65];
    int psk_len = 0, tok_len = 0, bad = 0;
    for (char *l = WIFI_TXT; *l;) {
        char *e = l;
        while (*e && *e != '\n')
            e++;
        char *end = e;
        while (end > l && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
            end--;
        int n = end - l;
        if (n > 5 && same((const u8 *)l, (const u8 *)"ssid=", 5) && n - 5 <= 32) {
            cfg_ssid_len = n - 5;
            cpy(cfg_ssid, l + 5, cfg_ssid_len);
        } else if (n > 4 && same((const u8 *)l, (const u8 *)"psk=", 4) && n - 4 <= 64) {
            psk_len = n - 4;
            cpy(psk, l + 4, psk_len);
        } else if (n > 6 && same((const u8 *)l, (const u8 *)"token=", 6) && n - 6 <= 32) {
            tok_len = n - 6;
            cpy(cfg_token, l + 6, tok_len);
        } else if (n > 3 && same((const u8 *)l, (const u8 *)"ip=", 3)) {
            u32 ip = 0, part = 0, dots = 0;
            for (char *p = l + 3; p < end; p++) {
                if (*p == '.')
                    ip = ip << 8 | part, part = 0, dots++;
                else if (*p >= '0' && *p <= '9')
                    part = part * 10 + (*p - '0');
                else
                    dots = 9;
            }
            if (dots == 3 && part < 256)
                cfg_ip = ip << 8 | part;
            else
                bad = 1;
        }
        l = *e ? e + 1 : e;
    }
    for (int i = 0; i < 4096; i++)
        WIFI_TXT[i] = 0;
    int hex = psk_len == 64;
    for (int i = 0; hex && i < 64; i++)
        hex = hexval(psk[i]) >= 0;
    static char es[32 * 4 + 1];
    esc_ssid(es, cfg_ssid, cfg_ssid_len);
    lprintf("config: ssid '%s' (%d bytes), psk %s (%d chars), token %d chars, static ip %s%s\n", es, cfg_ssid_len,
            hex ? "hex PMK" : "passphrase", psk_len, tok_len, cfg_ip ? "set" : "none", bad ? " (ip= unparsable)" : "");
    if (!cfg_ssid_len || !tok_len || (!hex && (psk_len < 8 || psk_len > 63))) {
        log_s("config: need ssid=, token= and psk= (8-63 chars or 64 hex digits)\n");
        return -1;
    }
    u32 t0 = ub_ms(0);
    if (hex)
        for (int i = 0; i < 32; i++)
            cfg_pmk[i] = hexval(psk[2 * i]) << 4 | hexval(psk[2 * i + 1]);
    else
        pbkdf2_sha1((const u8 *)psk, psk_len, cfg_ssid, cfg_ssid_len, 4096, cfg_pmk, 32);
    for (int i = 0; i < 65; i++)
        psk[i] = 0;
    lprintf("config: PMK ready in %u ms\n", ub_ms(t0));
    return 0;
}

/* ---- transmit: TX descriptor (rtl8xxxu_txdesc40, rtl8xxxu_fill_txdesc_v2) + bulk OUT ---- */
#define TXDESC_BROADMULTICAST       BIT(0)
#define TXDESC_LAST_SEGMENT         BIT(2)
#define TXDESC_FIRST_SEGMENT        BIT(3)
#define TXDESC_OWN                  BIT(7)
#define TXDESC_QUEUE_SHIFT          8
#define TXDESC_QUEUE_BE             0x0
#define TXDESC_QUEUE_MGNT           0x12
#define TXDESC40_AGG_BREAK          BIT(16)
#define TXDESC40_USE_DRIVER_RATE    BIT(8)
#define TXDESC40_RETRY_LIMIT_ENABLE BIT(17)
#define TXDESC40_RETRY_LIMIT_SHIFT  18
#define TXDESC40_SEQ_SHIFT          12
#define DESC_RATE_1M                0x00
#define DESC_RATE_12M               0x06
#define TXDESC_SIZE                 40

static u8 txbuf[2048] __attribute__((aligned(64)));
static u16 tx_seq;
static u32 tx_count, tx_err, tx_pad;

static void put32le(u8 *p, u32 v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

/* U-Boot's usb_sndbulkpipe: (PIPE_BULK=3) << 30 | devnum << 8 | ep << 15 | maxpacketsize, no 0x80 (usb_storage builds
 * exactly this for its OUT endpoint at 0xf18344) */
static int usb_tx(int ep, int len)
{
    u32 pipe = (3u << 30) | pipe_base(wdev, ep);
    for (int i = 0; i < 3; i++) {
        int actual = 0;
        int r = ub_bulk(wdev, pipe, txbuf, len, &actual, 100);
        if (r >= 0 && actual == len) {
            tx_count++;
            return 0;
        }
        if (tx_err++ < 20)
            lprintf("  bulk OUT ep%d len %d: %d status=%x actual=%d\n", ep, len, r, rd32(wdev + UDEV_STATUS), actual);
    }
    return -1;
}

/* the 802.11 frame is at txbuf + TXDESC_SIZE; mgmt frames go at 1 Mb/s on the MGNT queue (first OUT endpoint, the
 * high queue), data at a fixed 12 Mb/s on BE (second OUT endpoint, the normal queue), as rtl8xxxu maps them */
static int tx_frame(int flen, int mgmt)
{
    u8 *d = txbuf, *f = txbuf + TXDESC_SIZE;
    for (int i = 0; i < TXDESC_SIZE; i++)
        d[i] = 0;
    d[0] = flen, d[1] = flen >> 8, d[2] = TXDESC_SIZE;
    const u8 *da = (f[1] & 1) ? f + 16 : f + 4;   /* ieee80211_get_DA: A3 for ToDS */
    d[3] = TXDESC_OWN | TXDESC_FIRST_SEGMENT | TXDESC_LAST_SEGMENT | (da[0] & 1 ? TXDESC_BROADMULTICAST : 0);
    put32le(d + 4, (mgmt ? TXDESC_QUEUE_MGNT : TXDESC_QUEUE_BE) << TXDESC_QUEUE_SHIFT);
    put32le(d + 8, TXDESC40_AGG_BREAK);
    put32le(d + 12, TXDESC40_USE_DRIVER_RATE);
    put32le(d + 16, (mgmt ? DESC_RATE_1M : DESC_RATE_12M) | TXDESC40_RETRY_LIMIT_ENABLE |
                        (u32)(mgmt ? 6 : 12) << TXDESC40_RETRY_LIMIT_SHIFT);
    put32le(d + 36, (u32)((f[22] | f[23] << 8) >> 4) << TXDESC40_SEQ_SHIFT);
    u16 cs = 0;   /* rtl8xxxu_calc_tx_desc_csum: XOR of the first 16 halfwords, csum field (28) zero */
    for (int i = 0; i < 32; i += 2)
        cs ^= d[i] | d[i + 1] << 8;
    d[28] = cs, d[29] = cs >> 8;
    int ep = n_out_eps >= 2 ? out_ep[mgmt ? 0 : 1] : out_ep[0];
    return usb_tx(ep, TXDESC_SIZE + flen);
}

/* ---- association state ---- */
struct cand {
    u8 addr[6], ch, ds;
    s8 rssi;
    u16 cap, n;
    u8 rates[12], xrates[12], rsn[64];   /* IEs as received, header included */
};
#define MAX_CAND 8
static struct cand cands[MAX_CAND], bss;
static int n_cands;

enum { J_SCAN, J_WAIT, J_AUTH, J_ASSOC, J_4WAY, J_UP, J_ABORT };
static const char *const jnames[] = { "scan", "wait", "auth", "assoc", "4-way", "up", "abort" };
static int jstate, scanning, cur_ch, aid, j_tries, j_fail, h2c_mbox;
static u32 j_t, last_beacon, retry_at;
static s8 rssi_now;
static u8 my_mac[6];
static struct wpa wpa;
static u8 tk[16], gtk[4][16];
static int tk_ok, gtk_ok[4], keys_ok;
static cu64 tx_pn, rx_pn_tk, rx_pn_gtk[4];
static u32 rx_frames, rx_data, dec_fail, replays, beacons;
static const u8 our_rsn[] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
                              0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };   /* RSN v1, CCMP/CCMP, PSK, no PMF */

static u8 *mgmt_hdr(int fc0, const u8 *a1)
{
    u8 *f = txbuf + TXDESC_SIZE;
    f[0] = fc0, f[1] = 0, f[2] = f[3] = 0;
    cpy(f + 4, a1, 6);
    cpy(f + 10, my_mac, 6);
    cpy(f + 16, bss.addr, 6);
    f[22] = tx_seq << 4, f[23] = tx_seq >> 4;
    tx_seq = (tx_seq + 1) & 0xfff;
    return f + 24;
}

static void send_auth(void)
{
    u8 *b = mgmt_hdr(0xb0, bss.addr);
    b[0] = b[1] = 0, b[2] = 1, b[3] = 0, b[4] = b[5] = 0;   /* open system, seq 1, status 0 */
    int r = tx_frame(30, 1);
    lprintf("join: auth request %d sent (usb %d) t=%u\n", j_tries, r, ub_ms(0));
}

static void send_assoc(void)
{
    u8 *b = mgmt_hdr(0x00, bss.addr), *p = b;
    u16 cap = 0x0001 | 0x0010 | (bss.cap & (0x0020 | 0x0400));   /* ESS, privacy, + AP's short preamble/slot */
    *p++ = cap, *p++ = cap >> 8, *p++ = 10, *p++ = 0;              /* listen interval 10 */
    *p++ = 0, *p++ = cfg_ssid_len;
    cpy(p, cfg_ssid, cfg_ssid_len), p += cfg_ssid_len;
    if (bss.rates[1])
        cpy(p, bss.rates, 2 + bss.rates[1]), p += 2 + bss.rates[1];
    if (bss.xrates[1])
        cpy(p, bss.xrates, 2 + bss.xrates[1]), p += 2 + bss.xrates[1];
    cpy(p, our_rsn, sizeof(our_rsn)), p += sizeof(our_rsn);
    int r = tx_frame(24 + (p - b), 1);
    lprintf("join: assoc request %d sent, %d bytes (usb %d) t=%u\n", j_tries, 24 + (int)(p - b), r, ub_ms(0));
}

static void send_deauth(int reason)
{
    u8 *b = mgmt_hdr(0xc0, bss.addr);
    b[0] = reason, b[1] = 0;
    tx_frame(26, 1);
}

/* 802.11 data frame to the AP (ToDS) carrying an Ethernet-type payload; CCMP-protected when asked and keyed */
static int tx_data(const u8 *dst, int type, const u8 *p, int len, int protect)
{
    if (len > 1500)
        return -1;
    u8 *f = txbuf + TXDESC_SIZE;
    f[0] = 0x08, f[1] = 0x01, f[2] = f[3] = 0;
    cpy(f + 4, bss.addr, 6);
    cpy(f + 10, my_mac, 6);
    cpy(f + 16, dst, 6);
    f[22] = tx_seq << 4, f[23] = tx_seq >> 4;
    tx_seq = (tx_seq + 1) & 0xfff;
    protect = protect && tk_ok;
    u8 *body = f + 24 + (protect ? 8 : 0);
    static const u8 snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    cpy(body, snap, 6);
    body[6] = type >> 8, body[7] = type;
    cpy(body + 8, p, len);
    int plen = 8 + len;
    /* keep the USB transfer off a multiple of 512 (no zero-length packet from U-Boot's EHCI): IPv4 tolerates a pad */
    if (type == 0x0800 && (TXDESC_SIZE + 24 + plen + (protect ? 16 : 0)) % 512 == 0)
        body[plen++] = 0, tx_pad++;
    int flen = protect ? ccmp_encrypt(tk, f, 24, plen, ++tx_pn, 0) : 24 + plen;
    return tx_frame(flen, 0);
}

/* ---- MLME helpers: rtl8xxxu_set_bssid, the BSS_CHANGED_ASSOC path of rtl8xxxu_bss_info_changed ---- */
static void set_bssid(const u8 *b)
{
    for (int i = 0; i < 6; i++)
        rw8(REG_BSSID + i, b[i]);
}

static void h2c_media_status(int connect)   /* rtl8xxxu_gen2_report_connect(macid 0, role AP) */
{
    int i;
    for (i = 0; i < 100 && (rr8(REG_HMTFR) & BIT(h2c_mbox)); i++)
        ;
    if (i == 100) {
        log_s("h2c: mailbox busy\n");
        return;
    }
    rw32(REG_HMBOX_0 + 4 * h2c_mbox, 0x01 | (u32)((connect ? 1 : 0) | (2 << 4)) << 8);
    h2c_mbox = (h2c_mbox + 1) & 3;
}

static void set_rcr(int station)
{
    u32 v = RCR_ACCEPT_PHYS_MATCH | RCR_ACCEPT_MCAST | RCR_ACCEPT_BCAST | RCR_ACCEPT_MGMT_FRAME | RCR_HTC_LOC_CTRL |
            RCR_APPEND_PHYSTAT | RCR_APPEND_ICV | RCR_APPEND_MIC;
    if (station)   /* configure_filter without FIF_BCN_PRBRESP_PROMISC: our BSS only */
        v |= RCR_CHECK_BSSID_MATCH | RCR_CHECK_BSSID_BEACON;
    rw32(REG_RCR, v);
}

static void keys_clear(void)
{
    tk_ok = keys_ok = 0;
    for (int i = 0; i < 4; i++)
        gtk_ok[i] = 0;
    for (int i = 0; i < 16; i++)
        tk[i] = 0;
}

static void set_state(int s)
{
    lprintf("join: %s -> %s t=%u\n", jnames[jstate], jnames[s], ub_ms(0));
    jstate = s;
}

static void join_fail(const char *why)
{
    lprintf("join: FAILED in %s: %s (failure %d)\n", jnames[jstate], why, j_fail + 1);
    if (jstate >= J_ASSOC) {
        send_deauth(3);
        h2c_media_status(0);
        u8 v8 = rr8(REG_BEACON_CTRL);
        rw8(REG_BEACON_CTRL, v8 | BEACON_DISABLE_TSF_UPDATE);
    }
    keys_clear();
    ip_link_down();
    j_fail++;
    retry_at = ub_ms(0) + 1000;
    set_state(j_fail % 3 == 0 ? J_SCAN : J_WAIT);
    if (j_fail <= 5)
        flush("join failed");
}

/* RSN IE of the AP: CCMP group and pairwise, PSK AKM, PMF not required */
static int rsn_ok(const u8 *ie)
{
    static const u8 oui[3] = { 0x00, 0x0f, 0xac };
    int l = ie[1];
    const u8 *p = ie + 2, *end = ie + 2 + l;
    lprintf("bss: RSN IE");
    for (int i = 0; i < 2 + l; i++)
        lprintf(" %02x", ie[i]);
    log_c('\n');
    if (l < 2 || p[0] != 1 || p[1] != 0) {
        log_s("bss: RSN version is not 1\n");
        return 0;
    }
    p += 2;
    if (end - p < 4 || !same(p, oui, 3) || p[3] != 4) {
        lprintf("bss: group cipher %02x-%02x-%02x:%d is not CCMP; not supported\n", p[0], p[1], p[2], p[3]);
        return 0;
    }
    p += 4;
    int n = end - p >= 2 ? p[0] | p[1] << 8 : 0, ok = 0;
    p += 2;
    for (int i = 0; i < n && p + 4 <= end; i++, p += 4)
        ok |= same(p, oui, 3) && p[3] == 4;
    if (!ok) {
        log_s("bss: no CCMP pairwise cipher (TKIP only?); not supported\n");
        return 0;
    }
    n = end - p >= 2 ? p[0] | p[1] << 8 : 0, ok = 0;
    p += 2;
    for (int i = 0; i < n && p + 4 <= end; i++, p += 4)
        ok |= same(p, oui, 3) && p[3] == 2;
    if (!ok) {
        log_s("bss: no PSK AKM (SAE or 802.1X only?); not supported\n");
        return 0;
    }
    int caps = end - p >= 2 ? p[0] | p[1] << 8 : 0;
    if (caps & 0x40) {
        log_s("bss: management frame protection required; not supported\n");
        return 0;
    }
    lprintf("bss: RSN ok: CCMP/CCMP, PSK, RSN capabilities %04x (MFP %s)\n", caps, caps & 0x80 ? "capable" : "off");
    return 1;
}

/* ---- frame receive ---- */
static void scan_frame(const u8 *f, int len, int rssi)
{
    if (len < 36 || (f[0] != 0x80 && f[0] != 0x50))
        return;
    const u8 *ssid = 0, *rates = 0, *xrates = 0, *rsn = 0;
    int ssid_len = -1, ds = 0;
    for (int i = 36; i + 2 <= len && i + 2 + f[i + 1] <= len; i += 2 + f[i + 1]) {
        if (f[i] == 0 && ssid_len < 0)
            ssid = f + i + 2, ssid_len = f[i + 1];
        else if (f[i] == 1 && !rates)
            rates = f + i;
        else if (f[i] == 50 && !xrates)
            xrates = f + i;
        else if (f[i] == 48 && !rsn)
            rsn = f + i;
        else if (f[i] == 3 && f[i + 1] == 1)
            ds = f[i + 2];
    }
    if (ssid_len != cfg_ssid_len || !same(ssid, cfg_ssid, ssid_len))
        return;
    struct cand *c = 0;
    for (int i = 0; i < n_cands && !c; i++)
        if (same(cands[i].addr, f + 16, 6))
            c = &cands[i];
    if (!c) {
        if (n_cands >= MAX_CAND)
            return;
        c = &cands[n_cands++];
        cpy(c->addr, f + 16, 6);
        c->rssi = -128;
        char m[20];
        fmt_mac(m, f + 16);
        lprintf("scan: %s on ch %d (DS %d) %d dBm%s\n", m, cur_ch, ds, rssi, rsn ? "" : " (no RSN IE)");
    }
    c->n++;
    c->ch = cur_ch, c->ds = ds ? ds : cur_ch;
    if (rssi > c->rssi)
        c->rssi = rssi;
    c->cap = f[34] | f[35] << 8;
    c->rates[1] = c->xrates[1] = c->rsn[1] = 0;
    if (rates && rates[1] <= 10)
        cpy(c->rates, rates, 2 + rates[1]);
    if (xrates && xrates[1] <= 10)
        cpy(c->xrates, xrates, 2 + xrates[1]);
    if (rsn && rsn[1] <= 62)
        cpy(c->rsn, rsn, 2 + rsn[1]);
}

static void eapol_in(const u8 *p, int n)
{
    if (jstate < J_4WAY)
        return;
    int act = wpa_rx(&wpa, p, n);
    if (act & WPA_ERR) {
        set_state(J_ABORT);
        return;
    }
    if (act & WPA_TX)
        tx_data(bss.addr, 0x888e, wpa.tx, wpa.tx_len, wpa.tx_protect);
    if (act & WPA_PTK) {
        cpy(tk, wpa.ptk + 32, 16);
        tk_ok = 1, tx_pn = 0, rx_pn_tk = 0;
    }
    if (act & WPA_GTK) {
        int id = wpa.gtk_id;
        cpy(gtk[id], wpa.gtk, 16);
        cu64 rsc = 0;
        for (int i = 5; i >= 0; i--)
            rsc = rsc << 8 | wpa.gtk_rsc[i];
        rx_pn_gtk[id] = rsc ? rsc - 1 : 0;
        gtk_ok[id] = 1;
    }
    if (act & WPA_DONE) {
        keys_ok = 1;
        j_fail = 0;
        set_state(J_UP);
        flush("keys installed");
        ip_link_up();
    }
}

static void data_frame(u8 *f, int len)
{
    if ((f[1] & 3) != 2 || !same(f + 10, bss.addr, 6) || (f[0] & 0x40))
        return;   /* not FromDS from our AP, or a null-data subtype */
    int hl = ieee80211_hdrlen(f);
    const u8 *da = f + 4, *sa = f + 16;
    if (len < hl + 8 || same(sa, my_mac, 6))
        return;   /* too short, or our own broadcast relayed back by the AP */
    u8 *body;
    int n, prot = f[1] & 0x40;
    if (prot) {
        int keyid = f[hl + 3] >> 6, mc = da[0] & 1;
        if (mc ? !gtk_ok[keyid] : !tk_ok)
            return;
        cu64 pn;
        n = ccmp_decrypt(mc ? gtk[keyid] : tk, f, len, hl, &pn);
        if (n < 0) {
            if (dec_fail++ < 10)
                lprintf("rx: CCMP MIC failure (%s key %d, %d bytes)\n", mc ? "group" : "pairwise", keyid, len);
            return;
        }
        cu64 *last = mc ? &rx_pn_gtk[keyid] : &rx_pn_tk;
        if (pn <= *last) {
            replays++;
            return;
        }
        *last = pn;
        body = f + hl + 8;
    } else {
        body = f + hl, n = len - hl;
    }
    static const u8 snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
    if (n < 8 || !same(body, snap, 6))
        return;
    int type = be16(body + 6);
    rx_data++;
    if (type == 0x888e)
        eapol_in(body + 8, n - 8);
    else if (prot && keys_ok && (type == 0x0800 || type == 0x0806))
        ip_rx(sa, da, type, body + 8, n - 8);
}

static void mgmt_frame(const u8 *f, int len, int rssi)
{
    int sub = f[0] >> 4;
    if (scanning) {
        scan_frame(f, len, rssi);
        return;
    }
    if (!same(f + 16, bss.addr, 6))
        return;
    if (sub == 8) {
        last_beacon = ub_ms(0), beacons++;
        if (rssi)
            rssi_now = rssi_now ? (rssi_now * 7 + rssi) / 8 : rssi;
        return;
    }
    if (!same(f + 4, my_mac, 6) && !(f[4] & 1))
        return;
    if (len < 26)
        return;
    const u8 *b = f + 24;
    if (sub == 11 && jstate == J_AUTH && len >= 30) {   /* authentication */
        int alg = b[0] | b[1] << 8, seq = b[2] | b[3] << 8, st = b[4] | b[5] << 8;
        lprintf("join: auth response alg %d seq %d status %d\n", alg, seq, st);
        if (alg == 0 && seq == 2 && st == 0) {
            set_state(J_ASSOC);
            j_tries = 1, j_t = ub_ms(0);
            send_assoc();
        } else {
            join_fail("authentication refused");
        }
    } else if ((sub == 1 || sub == 3) && jstate == J_ASSOC && len >= 30) {   /* (re)association response */
        int st = b[2] | b[3] << 8;
        aid = (b[4] | b[5] << 8) & 0x3fff;
        lprintf("join: assoc response status %d aid %d cap %04x\n", st, aid, b[0] | b[1] << 8);
        if (st) {
            join_fail("association refused");
            return;
        }
        /* rtl8xxxu_bss_info_changed(BSS_CHANGED_ASSOC, assoc) */
        u8 v8 = rr8(REG_MSR);
        rw8(REG_MSR, (v8 & ~MSR_LINKTYPE_MASK) | MSR_LINKTYPE_STATION);
        rw8(REG_BCN_MAX_ERR, 0xff);
        v8 = rr8(REG_FWHW_TXQ_CTRL + 2);   /* rtl8xxxu_stop_tx_beacon */
        rw8(REG_FWHW_TXQ_CTRL + 2, v8 & ~BIT(6));
        rw8(REG_TBTT_PROHIBIT + 1, 0x64);
        v8 = rr8(REG_TBTT_PROHIBIT + 2);
        rw8(REG_TBTT_PROHIBIT + 2, v8 & ~BIT(0));
        rw16(REG_BCN_PSR_RPT, 0xc000 | aid);
        h2c_media_status(1);
        u8 snonce[32];
        rng_bytes(snonce, 32);
        wpa_init(&wpa, cfg_pmk, bss.addr, my_mac, our_rsn, sizeof(our_rsn), snonce);
        set_state(J_4WAY);
        j_t = ub_ms(0);
    } else if (sub == 12 || sub == 10) {   /* deauthentication / disassociation */
        lprintf("join: %s from the AP, reason %d\n", sub == 12 ? "deauth" : "disassoc", b[0] | b[1] << 8);
        if (jstate >= J_AUTH && jstate <= J_UP)
            join_fail(sub == 12 ? "deauthenticated" : "disassociated");
    }
}

static int dumps;

/* rtl8xxxu_parse_rxdesc24 over one bulk IN transfer, which may hold several packets */
static void parse_rx(int n)
{
    int off = 0;
    while (off + 24 <= n) {
        u8 *d = rxbuf + off;
        u32 d0 = le32(d), d2 = le32(d + 8), d3 = le32(d + 12);
        int pktlen = d0 & 0x3fff, drv = ((d0 >> 16) & 0xf) * 8, shift = (d0 >> 24) & 3, hdr = 24 + drv + shift;
        if (hdr + pktlen > n - off)
            break;
        u8 *f = d + hdr;
        const u8 *phy = (d0 & BIT(26)) && drv >= 8 ? d + 24 : 0;
        if (dumps < 2 && !scanning) {
            dumps++;
            lprintf("rx sample: pktlen %d drvinfo %d fc %02x%02x\n", pktlen, drv, f[0], f[1]);
        }
        if (!(d2 & BIT(28)) && !(d0 & (BIT(14) | BIT(15))) && pktlen >= 24) {   /* not C2H, no CRC/ICV error */
            rx_frames++;
            int type = (f[0] >> 2) & 3;
            if (phy && (rx_frames & 15) == 1)
                rng_mix(d, 24 + 8);
            if (type == 0)
                mgmt_frame(f, pktlen, phy ? signal_dbm(phy, d3 & 0x7f) : 0);
            else if (type == 2 && !scanning)
                data_frame(f, pktlen);
        }
        off += (hdr + pktlen + 7) & ~7;
    }
}

static int bulk_fail;

/* One bulk IN read. Returns 0 data, -2 nothing (at once with ehci.c, after EHCI_BULK_MS with U-Boot), -1 error. */
static int rx_once(void)
{
    static u32 pipe;
    if (!pipe)
        pipe = bulk_in(wdev, bulk_in_ep ? bulk_in_ep : 1);
    int actual = 0, r;
    u32 t = ub_ms(0);
    if (own_usb) {   /* ehci.c keeps one bulk IN queued: polling never blocks */
        r = ehci_rx_poll(wdev, pipe, rxbuf, RXBUF_LEN, &actual);
        if (r == -2)
            return -2;
    } else
        r = ub_bulk(wdev, pipe, rxbuf, RXBUF_LEN, &actual, 100);
    if (r < 0) {
        if (!own_usb && ub_ms(t) >= EHCI_BULK_MS - 1000)
            return -2;
        if (bulk_fail++ < 20)
            lprintf("rx: bulk IN error %d, status=%x\n", r, rd32(wdev + UDEV_STATUS));
        return -1;
    }
    bulk_fail = 0;
    if (actual > RXBUF_LEN)
        actual = RXBUF_LEN;
    if (actual >= 24)
        parse_rx(actual);
    return 0;
}

/* ---- scan for the configured SSID ---- */
static void scan_channel(int ch, u32 dwell)
{
    set_tx_power(ch);
    config_channel(ch);
    cur_ch = ch;
    u32 t0 = ub_ms(0);
    while (ub_ms(t0) < dwell && !errs) {
        hb_tick();
        if (rx_once() == -2 && !own_usb)   /* U-Boot's -2 meant 5 s of silence; ehci.c's means nothing yet */
            break;
    }
}

/* returns 0 with bss chosen, -1 nothing found, -2 found but unsupported */
static int scan_and_choose(void)
{
    static const u8 quick[] = { 1, 6, 11 };
    n_cands = 0;
    keys_clear();
    ip_link_down();
    set_rcr(0);
    u8 v8 = rr8(REG_BEACON_CTRL);   /* rtl8xxxu_sw_scan_start */
    rw8(REG_BEACON_CTRL, v8 | BEACON_DISABLE_TSF_UPDATE);
    scanning = 1;
    u32 t0 = ub_ms(0);
    for (unsigned i = 0; i < sizeof(quick); i++)
        scan_channel(quick[i], 700);
    if (!n_cands)
        for (int ch = 1; ch <= 13; ch++)
            scan_channel(ch, 700);
    scanning = 0;
    v8 = rr8(REG_BEACON_CTRL);   /* rtl8xxxu_sw_scan_complete */
    rw8(REG_BEACON_CTRL, v8 & ~BEACON_DISABLE_TSF_UPDATE);
    lprintf("scan: %d BSS for the SSID in %u ms\n", n_cands, ub_ms(t0));
    if (!n_cands)
        return -1;
    int best = 0;
    for (int i = 1; i < n_cands; i++)
        if (cands[i].rssi > cands[best].rssi)
            best = i;
    cpy(&bss, &cands[best], sizeof(bss));
    char m[20];
    fmt_mac(m, bss.addr);
    lprintf("bss: chose %s ch %d %d dBm cap %04x, rates %d+%d\n", m, bss.ds, bss.rssi, bss.cap, bss.rates[1],
            bss.xrates[1]);
    if (!bss.rsn[1]) {
        log_s("bss: no RSN IE (open or WPA1 network); not supported\n");
        return -2;
    }
    return rsn_ok(bss.rsn) ? 0 : -2;
}

static void join_start(void)
{
    set_tx_power(bss.ds);
    config_channel(bss.ds);
    cur_ch = bss.ds;
    set_bssid(bss.addr);
    set_rcr(1);
    keys_clear();
    last_beacon = ub_ms(0);
    set_state(J_AUTH);
    j_tries = 1, j_t = ub_ms(0);
    send_auth();
}

static void mlme_tick(void)
{
    u32 now = ub_ms(0);
    if ((jstate == J_AUTH || jstate == J_ASSOC) && now - j_t > 400) {
        if (j_tries >= 5) {
            join_fail(jstate == J_AUTH ? "no auth response" : "no assoc response");
            return;
        }
        j_tries++, j_t = now;
        if (jstate == J_AUTH)
            send_auth();
        else
            send_assoc();
    }
    if (jstate == J_4WAY && now - j_t > 5000)
        join_fail("4-way handshake timed out");
    else if (jstate >= J_AUTH && jstate <= J_UP && now - last_beacon > 10000)
        join_fail("beacon loss");
}

/* ---- platform for ip.c and wpa.c ---- */
#define RBUF ((u8 *)0x02000000)   /* HU1 reply buffer, HU_MAX_REPLY + 256 */
extern u32 __bss_start[], __bss_end[];
void plat_logf(const char *fmt, ...)   /* any task: its own buffer, not the net task's fmtbuf */
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, sizeof b, fmt, ap);
    va_end(ap);
    u32 f = irq_save();
    log_s(b);
    irq_restore(f);
}

int plat_fmt(char *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfmt(out, FMT_ANY, fmt, ap);
    va_end(ap);
    return n;
}

void plat_eth_send(const cu8 dst[6], int type, const cu8 *p, int len)
{
    if (keys_ok)
        tx_data(dst, type, p, len, 1);
}

cu32 plat_ms(void) { return ub_ms(0); }

cu32 plat_rand(void)
{
    u8 r[4];
    rng_bytes(r, 4);
    return le32(r);
}

void plat_udelay(u32 us) { ub_udelay(us); }

int plat_cmd(const char *c, cu8 *out, int max)
{
    cap_buf = out, cap_len = 0, cap_max = max - 32;
    irq_pet();   /* each remote command gets the full watchdog period */
    set_note(c);
    int rc = ub_run(c);
    set_note(0);
    cap_buf = 0;
    return cap_len + vfmt_n((char *)out + cap_len, "[rc=%d]\n", rc);
}

int plat_mem_ok(cu32 a, cu32 n, int write)
{
    if (a + n < a)
        return 0;
    if (!write)
        return 1;
    static const u32 keep[][2] = { { 0x01800000, 0 }, { 0x01a00000, 0x01a00000 + LOG_MAX },
                                   { 0x02000000, 0x02000000 + HU_MAX_REPLY + 256 } };
    for (unsigned i = 0; i < 3; i++) {
        u32 lo = keep[i][0], hi = i ? keep[i][1] : (u32)__bss_end;
        if (a < hi && a + n > lo)
            return 0;
    }
    return 1;
}

void plat_mem_read(cu32 a, cu8 *out, int n)
{
    for (int i = 0; i < n; i += 4)
        put32le(out + i, *(volatile u32 *)(a + i));
}

void plat_mem_write(cu32 a, const cu8 *p, int n)
{
    for (int i = 0; i < n; i++)
        *(volatile u8 *)(a + i) = p[i];
}

/* D-cache write-back and I-cache invalidate per line, the way U-Boot's flush_dcache_range (0xf0a390) and
 * invalidate_icache_range (0xf0a3b6) size their lines */
void plat_sync(cu32 a, cu32 n)
{
    u32 df = (MFSR(DCM_CFG) >> 6) & 7, ifl = (MFSR(ICM_CFG) >> 6) & 7;
    u32 dl = df ? 8u << (df - 1) : 32, il = ifl ? 8u << (ifl - 1) : 32;
    for (u32 p = a & ~(dl - 1); p < a + n; p += dl)
        __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(p) : "memory");
    __asm__ volatile("msync all" ::: "memory");
    for (u32 p = a & ~(il - 1); p < a + n; p += il)
        __asm__ volatile("cctl %0, L1I_VA_INVAL" ::"r"(p) : "memory");
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

int plat_log_tail(cu8 *out, int max)
{
    u32 n = log_len < (u32)max ? log_len : (u32)max;
    cpy(out, LOG_BUF + log_len - n, n);
    return n;
}

/* a crash record found at start is reported in the first 3 PINGs and the hellos of the first 2 minutes, then dropped */
static const char *crash_live(void)
{
    if (!crash_brief[0] || crash_pings >= 3 || (int)(ub_ms(0) - crash_until) > 0)
        return 0;
    return crash_brief;
}

int plat_status(char *out)
{
    const char *c = crash_live();
    if (c)
        crash_pings++;
    return vfmt_n(out, "up=%u hb=%u mcurx=%u rssi=%d build=%s irq=%s ticks=%u irqs=%u crash=%s", ub_ms(0),
                  hb_count + irq_hb_sent, rx_bytes, rssi_now, hu_build_id, irq_is_on() ? "on" : "off", irq_ticks,
                  irq_count[0] + irq_count[1] + irq_count[2] + irq_count[3] + irq_count[4] + irq_count[5] + irq_count[6],
                  c ? c : "none");
}

int plat_hello_extra(char *out)
{
    const char *c = crash_live();
    return c ? vfmt_n(out, " crash=%s", c) : 0;
}

void plat_bssid(cu8 out[6]) { cpy(out, bss.addr, 6); }


void plat_ip_event(cu32 ip, cu32 mask, cu32 router, int how)
{
    if (!how) {
        log_s("ip: address lost\n");
        return;
    }
    char *p = HELLO_BUF;
    p += vfmt_n(p, "ip=");
    p += ip_fmt_addr(p, ip);
    p += vfmt_n(p, "\nmask=");
    p += ip_fmt_addr(p, mask);
    p += vfmt_n(p, "\nrouter=");
    p += ip_fmt_addr(p, router);
    p += vfmt_n(p, "\nvia=%s\nmac=", how == 1 ? "dhcp" : "static");
    p = fmt_mac(p, my_mac);
    p += vfmt_n(p, "\nbssid=");
    p = fmt_mac(p, bss.addr);
    p += vfmt_n(p, "\nch=%d\nrssi=%d\nt=%u\nport=%d\n", cur_ch, rssi_now, ub_ms(0), HU_PORT);
    *p = 0;
    log_s(HELLO_BUF);
    flush("ip");
}

/* ---- framebuffer console on OSD0 (console.c; notes/console.md) ---- */
#define FB_ADDR 0x03400000u   /* 1024 x 600 x 4 = 0x258000 bytes, up to 0x03658000 */
#define FB_HDR  0x03660000u   /* 128-byte OSD region header */
#define OSD0    0x9c006200u
#define IOREG(a) (*(volatile u32 *)(a))
static int fb_on_flag, fb_renders, fb_rows;
static u32 fb_last;
static char say_text[CON_MAX_COLS + 1];

static void dcache_wb(u32 a, u32 n)
{
    u32 f = (MFSR(DCM_CFG) >> 6) & 7, l = f ? 8u << (f - 1) : 32;
    for (u32 p = a & ~(l - 1); p < a + n; p += l)
        __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(p) : "memory");
    __asm__ volatile("msync all" ::: "memory");
}

static void fb_flush(unsigned int a, unsigned int n) { dcache_wb(a, n); }

static void put16be(u8 *p, u32 v) { p[0] = v >> 8, p[1] = v; }

/* frame + header as U-Boot sub_f1fe84 / tools/fb.py build them, then U-Boot's own two OSD0 writes (0xf216ee) */
static void fb_enable(void)
{
    con_init((unsigned int *)FB_ADDR);
    u8 *h = (u8 *)FB_HDR;
    for (int i = 0; i < 128; i++)
        h[i] = 0;
    h[0] = 0x0e;   /* ARGB8888 */
    h[2] = 0x10;
    put16be(h + 4, CON_H), put16be(h + 6, CON_W), put16be(h + 0x16, CON_W);
    h[0x15] = 7;
    h[0x18] = 0xff, h[0x19] = 0xff, h[0x1a] = 0xff, h[0x1b] = 0xe0;   /* end of the region list */
    h[0x1c] = (u8)(FB_ADDR >> 24), h[0x1d] = (u8)(FB_ADDR >> 16), h[0x1e] = (u8)(FB_ADDR >> 8), h[0x1f] = (u8)FB_ADDR;
    dcache_wb(FB_ADDR, CON_W * CON_H * 4);
    dcache_wb(FB_HDR, 128);
    IOREG(OSD0 + 8) = FB_HDR;
    IOREG(OSD0 + 4) = 1;
    fb_on_flag = 1, fb_last = 0;
}

static void fb_disable(void)
{
    IOREG(OSD0 + 4) = 0;
    fb_on_flag = 0;
}

/* The console task owns drawing; the framebuffer on/off/scale commands (net task) take the same lock. */
static SemaphoreHandle_t con_lock;
static void con_lock_take(void) { if (con_lock && port_running) xSemaphoreTake(con_lock, portMAX_DELAY); }
static void con_lock_give(void) { if (con_lock && port_running) xSemaphoreGive(con_lock); }

/* status panel + log tail, about twice a second, from the main loop and hb_tick (never from the ISR) */
static void console_poll(void)
{
    static int busy;
    u32 now = ub_ms(0);
    if (!fb_on_flag || busy || (fb_last && now - fb_last < 500))
        return;
    busy = 1;
    fb_last = now;
    static char t[320];
    char ip[16], es[32 * 4 + 1];
    u32 s = now / 1000;
    vfmt_n(t, "HU net %s   up %u:%02u:%02u", hu_build_id, s / 3600, s / 60 % 60, s % 60);
    con_panel(0, t, CON_YELLOW);
    ip_fmt_addr(ip, ip_addr());
    esc_ssid(es, cfg_ssid, cfg_ssid_len);
    vfmt_n(t, "IP %s  %s  ch %d  %d dBm  %s", ip, es, cur_ch, rssi_now, jnames[jstate]);
    con_panel(1, t, jstate == J_UP && ip_addr() ? CON_GREEN : CON_WHITE);
    vfmt_n(t, "IRQ %s  ticks %u  hb %u  usb err %u  tx %u", irq_is_on() ? "on" : "off", irq_ticks,
           hb_count + irq_hb_sent, errs, tx_count);
    con_panel(2, t, CON_WHITE);
    int k = vfmt_n(t, "MCU ACC");
    for (int i = 0; i < mcu_st.acc_len; i++)
        k += vfmt_n(t + k, " %02x", mcu_st.acc[i]);
    if (!mcu_st.acc_len)
        k += vfmt_n(t + k, " ?");
    k += vfmt_n(t + k, "  '%s'", mcu_st.version);
    if (mcu_st.have_time)
        k += vfmt_n(t + k, "  %02u:%02u", mcu_st.hm[0], mcu_st.hm[1]);
    if (mcu_st.have_date)
        vfmt_n(t + k, "  20%02u-%02u-%02u", mcu_st.ymd[0], mcu_st.ymd[1], mcu_st.ymd[2]);
    con_panel(3, t, CON_WHITE);
    if (crash_brief[0])
        vfmt_n(t, "last crash: %s", crash_brief);
    else
        vfmt_n(t, "crash: none   %s", gt9_last);
    con_panel(4, t, crash_brief[0] ? CON_RED : CON_WHITE);
    if (say_text[0])
        con_panel(5, say_text, CON_CYAN);
    else
        con_panel(5, "HU1 on udp 5151", CON_GREY);
    con_log_source(LOG_BUF, log_len);
    fb_rows += con_render(fb_flush);
    fb_renders++;
    busy = 0;
}

static int fb_status(char *t)
{
    return vfmt_n(t, "fb=%s frame=%x hdr=%x log_scale=%d renders=%d rows=%d osd0 ctrl=%x en=%x link=%x crc=%x",
                  fb_on_flag ? "on" : "off", FB_ADDR, FB_HDR, con_get_log_scale(), fb_renders, fb_rows,
                  IOREG(OSD0), IOREG(OSD0 + 4), IOREG(OSD0 + 8), IOREG(OSD0 + 0x38));
}

/* ---- MCU, wall clock, EXEC ---- */
void plat_mcu_write(const cu8 *f, int n) { uart_write(f, n); }

/* ACC off: one log file with a fresh name (S + 7 hex digits from the uptime and the log length) */
void plat_mcu_shutdown(void)
{
    char name[16];
    vfmt_n(name, "S%07x.TXT", (ub_ms(0) ^ (log_len << 12)) & 0xfffffff);
    save_log(name);
}

const char *plat_build(void) { return hu_build_id; }

#define TRAMP_ADDR 0x02f00000u
static u32 exec_src, exec_len;
static int exec_pending, fatal_pending;
static u32 deferred_at;   /* EXEC / fatal trap wait this long after their HU1 reply so it leaves the radio first */

static int rtos_status(char *o)
{
    static TaskStatus_t st[12];
    static const char *const sn[] = { "run", "ready", "blocked", "suspended", "deleted", "?" };
    UBaseType_t n = uxTaskGetSystemState(st, 12, 0);
    int k = vfmt_n(o, "tick %u  tasks %u  heap free %u (min %u) of %u\n", (u32)xTaskGetTickCount(), (u32)n,
                   (u32)xPortGetFreeHeapSize(), (u32)xPortGetMinimumEverFreeHeapSize(), (u32)configTOTAL_HEAP_SIZE);
    for (UBaseType_t i = 0; i < n; i++)
        k += vfmt_n(o + k, "  %s: prio %u, %s, stack free %u words\n", st[i].pcTaskName, (u32)st[i].uxCurrentPriority,
                    sn[st[i].eCurrentState < 5 ? st[i].eCurrentState : 5], (u32)st[i].usStackHighWaterMark);
    return k;
}

static int usb_op(int c, int count, char *o)
{
    if (!wdev)
        return vfmt_n(o, "ERR no Wi-Fi device yet");
    if ((c == 1 || c == 3) && own_enum)
        return vfmt_n(o, "ERR the dongle was enumerated by usbh.c; U-Boot's USB is not started");
    if (c)
        ehci_rx_stop();   /* compare and uboot call U-Boot's USB code; rx_once re-queues */
    if (c == 2 || c == 3) {
        own_usb = c == 2;
        lprintf("usb: Wi-Fi transfers now through %s\n", own_usb ? "ehci.c" : "U-Boot");
    }
    int k = 0;
    if (c == 1) {   /* the same vendor read (REG_SYS_CFG, 4 bytes) through both, alternating */
        static u8 b1[64] __attribute__((aligned(64)));
        u32 bad = 0, fail_ub = 0, fail_own = 0, t_ub = 0, t_own = 0, v_ub = 0, v_own = 0;
        for (int i = 0; i < count; i++) {
            u32 t0 = clk_ms();
            int r = ((ctrl_fn)UB(UB_USB_CONTROL_MSG))(wdev, ctrl_in(wdev), 0x05, 0xc0, REG_SYS_CFG, 0, b1, 4, 500);
            t_ub += clk_ms() - t0;
            v_ub = r == 4 ? rd32(b1) : (fail_ub++, 0);
            t0 = clk_ms();
            r = ehci_ctrl(wdev, ctrl_in(wdev), 0x05, 0xc0, REG_SYS_CFG, 0, b1, 4, 500);
            t_own += clk_ms() - t0;
            v_own = r == 4 ? rd32(b1) : (fail_own++, 0);
            if (v_ub != v_own)
                bad++;
            irq_pet();
        }
        k = vfmt_n(o, "usb: compare %d reads: u-boot fail %u (%u ms), ehci.c fail %u (%u ms), mismatch %u, value %x\n",
                   count, fail_ub, t_ub, fail_own, t_own, bad, v_own);
        lprintf("%s", o);
    }
    return k + vfmt_n(o + k, "usb: driver %s, ehci.c ok %u (rx %u) err %u timeout %u busy %u refused %u, portsc %x, status %x",
                  own_usb ? "ehci.c" : "u-boot", ehci_stats.ok, ehci_stats.rx, ehci_stats.errors, ehci_stats.timeouts,
                  ehci_stats.busy, ehci_stats.refused, ehci_portsc(), rd32(wdev + UDEV_STATUS));
}

int plat_op(int op, const cu8 *p, int n, cu8 *out, int max)
{
    char *t = (char *)out;
    u32 now = ub_ms(0);
    switch (op) {
    case 7:   /* MCU_TX: raw payload -> frame */
        if (!mcu_tx_allowed(p, n))
            return vfmt_n(t, "ERR refused: payload must be 1-64 bytes and not ad.., 00, 11 00 or e5 03");
        mcu_send(&mcu_st, p, n, now);
        return vfmt_n(t, "ok");
    case 8:   /* MCU_STATE */
        return mcu_state(&mcu_st, t, now);
    case 9: {   /* SETTIME y-2000 m d h mi s */
        if (n < 6 || p[0] > 99 || p[1] < 1 || p[1] > 12 || p[2] < 1 || p[2] > 31 || p[3] > 23 || p[4] > 59 || p[5] > 59)
            return vfmt_n(t, "ERR settime: y-2000 m d h mi s");
        mcu_settime(&mcu_st, p, now);
        return vfmt_n(t, "ok 20%02u-%02u-%02u %02u:%02u:%02u", p[0], p[1], p[2], p[3], p[4], p[5]);
    }
    case 12: {   /* FB: 0 off, 1 on, 2 status, 3 log scale <u8 1|2> */
        int c = n ? p[0] : 2;
        con_lock_take();
        if (c == 0)
            fb_disable();
        else if (c == 1 && !fb_on_flag)
            fb_enable();
        else if (c == 3 && n >= 2)
            con_log_scale(p[1]);
        con_lock_give();
        fb_last = 0;   /* the console task redraws within 50 ms */
        return fb_status(t);
    }
    case 13: {   /* SAY: a line for the screen (panel row 5) and the log */
        int l = n < CON_MAX_COLS - 5 ? n : CON_MAX_COLS - 5, k = vfmt_n(say_text, "say: ");
        for (int i = 0; i < l; i++)
            say_text[k + i] = p[i] >= 0x20 && p[i] < 0x7f ? p[i] : ' ';
        say_text[k + l] = 0;
        lprintf("%s\n", say_text);
        fb_last = 0;
        return vfmt_n(t, "ok");
    }
    case 11: {   /* IRQ_CTL: 0 off, 1 on [u16 hz], 2 fatal trap (after this reply), 3 resumable trap, 4 status */
        int k = 0, c = n ? p[0] : 4;
        if (c == 0) {
            irq_disable();
            log_s("irq: off\n");
        } else if (c == 1) {
            u32 hz = n >= 3 ? (u32)(p[1] | p[2] << 8) : 100;
            int r = irq_enable(hz);
            lprintf("irq: on at %u Hz -> %d, t=%u\n", hz, r, now);
            if (r)
                k = vfmt_n(t, "ERR irq on %d (hz 3..1000)\n", r);
        } else if (c == 2) {
            fatal_pending = 1, deferred_at = ub_ms(0) + 200;
            k = vfmt_n(t, "fatal trap after this reply: crash record, then reset\n");
        } else if (c == 3) {
            int hit = irq_test_trap();
            lprintf("irq: test trap -> handler %s\n", hit ? "ran, resumed" : "DID NOT RUN");
            k = vfmt_n(t, "test trap: %s\n", hit ? "handled and resumed" : "handler did not run");
        }
        return k + irq_status(t + k);
    }
    case 14:   /* TOUCH: gt9.c */
        return gt9_op(p, n, t);
    case 15:   /* RTOS: task list */
        return rtos_status(t);
    case 16:   /* USB: 0 status, 1 compare U-Boot vs ehci.c on N register reads, 2 own driver on, 3 off */
        return usb_op(n ? p[0] : 0, n >= 3 ? p[1] | p[2] << 8 : 200, t);
    case 10: {   /* EXEC src len: checked here, run from net_loop after this reply has gone out */
        u32 src = n >= 8 ? le32(p) : 0, len = n >= 8 ? le32(p + 4) : 0;
        const u32 *img = (const u32 *)src;
        if (n < 8 || (src & 3) || src < 0x00100000 || len < 0x100 || len > 0x100000 || src + len > 0x07000000 ||
            (src < (u32)__bss_end && src + len > 0x01800000) || (src < TRAMP_ADDR + 0x10000 && src + len > TRAMP_ADDR))
            return vfmt_n(t, "ERR exec: src 4-aligned, 0x100 <= len <= 0x100000, not over the payload/trampoline");
        if (img[0x40 / 4] != 0x454e5548 || img[0x44 / 4] != 1)
            return vfmt_n(t, "ERR exec: no HUNE v1 header at src+0x40");
        char id[56];
        const char *b = (const char *)(src + 0x48);
        int k = 0;
        while (k < 55 && b[k] >= 0x20 && b[k] < 0x7f)
            id[k] = b[k], k++;
        id[k] = 0;
        exec_src = src, exec_len = len, exec_pending = 1, deferred_at = ub_ms(0) + 200;
        return vfmt_n(t, "ok exec %u bytes from %x, build %s (running %s)", len, src, id, hu_build_id);
    }
    }
    (void)max;
    return -1;
}

/* Replace this payload: undo the console hooks, leave the BSS, copy the trampoline to TRAMP_ADDR and call it. It only
 * comes back if it refuses the image. */
static void do_exec(void)
{
    exec_pending = 0;
    lprintf("exec: %u bytes from %p, t=%u\n", exec_len, exec_src, ub_ms(0));
    irq_shutdown();   /* the vectors and the ISR are in the image about to be replaced */
    gt9_release();
    if (jstate >= J_AUTH && jstate <= J_UP) {
        send_deauth(3);
        h2c_media_status(0);
    }
    ehci_rx_stop();   /* no DMA into rxbuf once the next image is copied in */
    flush("exec");
    for (int i = 0; i < 2; i++)
        if (hooked[i])
            hooked[i]->putc = orig_putc[i], hooked[i]->puts = orig_puts[i];
    u32 tlen = tramp_bin_end - tramp_bin;
    cpy((void *)TRAMP_ADDR, tramp_bin, tlen);
    plat_sync(TRAMP_ADDR, tlen);
    char *a = (char *)(TRAMP_ADDR + 0xf000), **argv = (char **)(TRAMP_ADDR + 0xf100);
    argv[0] = a;
    a += vfmt_n(a, "tramp") + 1;
    argv[1] = a;
    a += vfmt_n(a, "%x", exec_src) + 1;
    argv[2] = a;
    vfmt_n(a, "%x", exec_len);
    argv[3] = 0;
    int (*tramp)(int, char **) = (int (*)(int, char **))TRAMP_ADDR;
    /* under the scheduler this runs on a task stack inside the RTOS heap, which the next image's heap reuses */
    int r = port_running ? port_call_on_stack(tramp, 3, argv, port_boot_sp) : tramp(3, argv);
    install_hook();
    irq_init();
    if (port_running)
        irq_enable(configTICK_RATE_HZ);
    lprintf("exec: the trampoline refused the image (%d)\n", r);
    if (jstate >= J_AUTH && jstate <= J_UP)
        join_fail("exec refused");
}

/* ---- the radio bring-up (round 7) and the network loop ---- */
static int wifi_init(void)
{
    log_s("\n# USB devices\n");
    for (int i = 0; i < 32 && !wdev; i++) {
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
        return -1;
    }
    lprintf("using Realtek device @%p\n", (u32)wdev);
    dump_config();
    u32 cfg = rr32(REG_SYS_CFG);
    lprintf("SYS_CFG=%p chip cut %d\n", cfg, (cfg >> 12) & 0xf);
    if (errs || read_efuse())
        return -1;
    cpy(my_mac, efuse + 0xd7, 6);
    rng_mix(efuse, 512);
    if (power_on())
        return -1;
    init_queues();
    if (errs || firmware())
        return -1;
    flush("firmware");
    if (init_rest())
        return -1;
    radio_on();
    flush("radio on");
    return errs ? -1 : 0;
}

static void status_line(void)
{
    char ip[16], m[20];
    ip_fmt_addr(ip, ip_addr());
    fmt_mac(m, bss.addr);
    lprintf("status t=%u state=%s bss=%s ch=%d rssi=%d beacons=%u rx=%u data=%u tx=%u txerr=%u pad=%u decfail=%u "
            "replay=%u ip=%s ipstats=%u/%u/%u/%u/%u/%u/%u/%u hb=%u usberr=%u\n",
            ub_ms(0), jnames[jstate], m, cur_ch, rssi_now, beacons, rx_frames, rx_data, tx_count, tx_err, tx_pad,
            dec_fail, replays, ip, ip_stats[0], ip_stats[1], ip_stats[2], ip_stats[3], ip_stats[4], ip_stats[5],
            ip_stats[6], ip_stats[7], hb_count, errs);
}

static void net_loop(void)
{
    u32 last_status = ub_ms(0), last_flush_len = log_len;
    jstate = J_SCAN;
    for (;;) {
        hb_tick();
        rx_poll();
        if (errs || jstate == J_ABORT || bulk_fail > 50) {
            lprintf("net: stopping (usb errors %u, bulk failures %d, state %s)\n", errs, bulk_fail, jnames[jstate]);
            return;
        }
        if (jstate == J_SCAN) {
            int r = scan_and_choose();
            if (r == -2) {
                set_state(J_ABORT);
                continue;
            }
            static int scans;
            if (r == 0 || scans++ < 2)
                flush("scanned");
            if (r == 0)
                join_start();
            else
                retry_at = ub_ms(0) + 3000, set_state(J_WAIT);
            continue;
        }
        if (jstate == J_WAIT && (int)(ub_ms(0) - retry_at) >= 0) {
            if (n_cands)
                join_start();
            else
                set_state(J_SCAN);
            continue;
        }
        rx_once();
        mlme_tick();
        ip_poll();
        irq_check();
        if (exec_pending && (int)(ub_ms(0) - deferred_at) >= 0)
            do_exec();
        if (fatal_pending && (int)(ub_ms(0) - deferred_at) >= 0) {
            log_s("irq: fatal test trap now\n");
            flush("fatal trap");
            irq_crash_now();
        }
        u32 now = ub_ms(0);
        if (now - last_status >= 30000) {
            last_status = now;
            status_line();
            if (log_len != last_flush_len) {
                last_flush_len = log_len;
                flush("periodic");
            }
        }
    }
}

static void hello(int argc, char *const argv[])
{
    log_len = 0;
    lprintf("hello from net payload build %s, argc=%d", hu_build_id, argc);
    for (int i = 0; i < argc; i++)
        lprintf(" [%s]", argv[i]);
    log_c('\n');
    report_cpu();
    check_uboot();
    lprintf("end\n");
    for (u32 i = 0; i < 0x800; i++)   /* HELLO.TXT is the first 0x800 bytes */
        HELLO_BUF[i] = i < log_len ? LOG_BUF[i] : ' ';
}

extern uint8_t ucHeap[];

/* Screen: status panel and log tail, redrawn every 500 ms or within 50 ms of a `fb_last = 0` kick. No U-Boot calls. */
static void console_task(void *arg)
{
    for (;;) {
        con_lock_take();
        console_poll();
        con_lock_give();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* The touch controller, every 30 ms. No U-Boot calls in here (port_udelay, not udelay). */
static void touch_task(void *arg)
{
    for (;;) {
        gt9_tick(xTaskGetTickCount() * portTICK_PERIOD_MS);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* Everything that calls U-Boot (USB for Wi-Fi, mmc/fat, commands, get_timer/udelay) stays in this one task: the old
 * main loop, unchanged. It never blocks, so it runs whenever no higher-priority task is ready. */
#define NET_STACK_WORDS (64 * 1024)
static void net_task(void *arg)
{
    lprintf("rtos: scheduler running, tick %u Hz (irq.c timer); net task\n", configTICK_RATE_HZ);
    irq_watchdog(90);   /* a hang now reboots (crash=hang@pc) instead of needing a power cycle */
    flush("hooked");
    if (pending_crash_save) {   /* one file per crash, named after the report's CRC */
        char name[16];
        vfmt_n(name, "C%07x.TXT", crc32_le((const u8 *)LOG_BUF, (int)log_len) & 0xfffffff);
        save_log(name);
    }
    if (load_config()) {
        flush("no config");
        vTaskEndScheduler();
    }

    log_s("\n# USB (usbh.c)\n");
    u32 t0 = ub_ms(0);
    wdev = usbh_start();
    if (wdev) {
        own_enum = 1;
        lprintf("usb: enumerated %04x:%04x in %u ms, portsc %x, otp %x, %u endpoints\n", rd16(wdev + UDEV_DESC + 8),
                rd16(wdev + UDEV_DESC + 10), ub_ms(t0), usbh_info.portsc, usbh_info.otp, usbh_info.n_eps);
    } else {
        lprintf("usb: own enumeration failed at step %u (portsc %x); falling back to U-Boot\n", usbh_info.step,
                usbh_info.portsc);
        log_s("\n# U-Boot USB\n");
        run("usb reset");   /* not "start": after an EXEC U-Boot thinks USB is already started and would skip it */
    }
    flush("usb start");
    own_usb = 1;   /* from here on every Wi-Fi transfer goes through ehci.c; U-Boot only enumerated (hu.py usb uboot reverts) */

    if (wifi_init() == 0) {
        ip_init(my_mac, cfg_token, cfg_ip, RBUF);
        net_loop();
    }
    ehci_rx_stop();
    vTaskEndScheduler();
}

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
    if (cmd[0] != 'm')
        return 1;

    for (u32 *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    lprintf("net main, build %s\n", hu_build_id);
    mcu_init(&mcu_st);
    report_cpu();
    if (check_uboot())
        return 2;   /* log stays in RAM at 0x1a00000; calling U-Boot now is unsafe */
    lprintf("t=%u ms since boot\n", ub_ms(0));
    irq_init();   /* exceptions now land in irq.c (crash record + reset) */
    irq_set_heartbeat(hb_frame, sizeof(hb_frame));
    hb_tick();
    install_hook();
#ifndef BOOT_FB
#define BOOT_FB 1
#endif
    if (BOOT_FB)
        fb_enable();   /* text console on OSD0 from here on */
    {
        static char rep[1024];
        if (irq_crash_report(rep, crash_brief)) {
            crash_until = ub_ms(0) + 120000;
            log_s("\n!!!!!!!! previous run crashed !!!!!!!!\n");
            log_s(rep);
            pending_crash_save = 1;   /* saved to the card below, once */
        }
    }
    port_init();
    lprintf("rtos: FreeRTOS %s, tick %u Hz, heap %u KB at %p\n", tskKERNEL_VERSION_NUMBER, configTICK_RATE_HZ,
            configTOTAL_HEAP_SIZE / 1024, ucHeap);
    if (xTaskCreate(net_task, "net", NET_STACK_WORDS, 0, 1, 0) != pdPASS ||
        xTaskCreate(touch_task, "touch", 2048, 0, 3, 0) != pdPASS ||
        xTaskCreate(console_task, "console", 4096, 0, 2, 0) != pdPASS || !(con_lock = xSemaphoreCreateMutex())) {
        flush("rtos: task create failed");
        return 4;
    }
    vTaskStartScheduler();   /* returns when net_task ends the scheduler */
    lprintf("rtos: scheduler ended\n");
    status_line();
    lprintf("usb errors: %u, heartbeats sent: %u, t=%u\n", errs, hb_count, ub_ms(0));
    irq_shutdown();
    flush("done");
    for (int i = 0; i < 2; i++) {
        if (hooked[i]) {
            hooked[i]->putc = orig_putc[i];
            hooked[i]->puts = orig_puts[i];
        }
    }
    return 0;
}
