/* Console capture for U-Boot scripts on the SPHE8268K head unit. Runs from `go` under the stock U-Boot 2014.04, which
 * relocates itself (link 0xF00040, runtime offset from the $gp that `go` leaves us; gd in $r10).
 *
 *   go 0x1800000 hook        start copying everything U-Boot prints into a RAM log (hook stays installed)
 *   go 0x1800000 save NAME   write the log so far to the card as NAME, then start a new log
 *   go 0x1800000 walk ADDR LEN CHUNK NAME
 *                            copy ADDR..ADDR+LEN (hex) with 32-bit reads, one CHUNK at a time, writing the first chunk
 *                            to NAME and appending the rest; if a read hangs the bus, NAME's length shows where
 *
 * The dump script around it uses plain U-Boot commands; this only makes their output (md, help, printenv, mmcinfo...)
 * visible without a UART. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define UB_PARSE_STRING_OUTER 0x00f0d7fe  /* parse_string_outer(cmd, FLAG_PARSE_SEMICOLON | FLAG_EXIT_FROM_LOOP) */
#define UB_GET_TIMER          0x00f00c2e  /* get_timer(base), ms */
#define UB_STDIO_DEVICES      0x00fd9614  /* struct stdio_dev *stdio_devices[3] */
#define UB_GOT_STDIO_DEVICES  0x00fb8660  /* GOT slot holding &stdio_devices (fixed up by relocation) */
#define UB_GP_LINK            0x00fb3d18
#define UB_TEXT_START         0x00f00040
#define UB_TEXT_END           0x00fb8e10
#define RAM_END               0x08000000

#include "ub_sig.h"   /* ub_sig[]: {address, first instruction word} of each entry point, from the flash dump */

static u32 reloc;     /* runtime minus link address of U-Boot */
#define UB(a) ((a) + reloc)
#define ub_run(cmd) ((int (*)(const char *, int))UB(UB_PARSE_STRING_OUTER))(cmd, 3)
#define ub_ms(base) ((u32 (*)(u32))UB(UB_GET_TIMER))(base)

struct stdio_dev {
    int flags, ext;
    char name[16];
    int (*start)(void);
    int (*stop)(void);
    void (*putc)(const char c);
    void (*puts)(const char *s);
};

#define LOG_BUF ((char *)0x01a00000)
#define LOG_MAX 0x00400000

static u32 log_len;
static struct stdio_dev *hooked;
static void (*orig_putc)(const char);
static void (*orig_puts)(const char *);

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

static void hook_putc(const char c) { log_c(c); orig_putc(c); }
static void hook_puts(const char *s) { log_s(s); orig_puts(s); }

static char *put_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_hex(char *p, u32 v)
{
    int i = 28;
    while (i > 0 && !((v >> i) & 0xf))
        i -= 4;
    for (; i >= 0; i -= 4)
        *p++ = "0123456789abcdef"[(v >> i) & 0xf];
    return p;
}

static char *put_dec(char *p, u32 v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

static void log_hex(const char *k, u32 v)
{
    char b[48], *p = put_s(b, k);
    p = put_s(p, "=0x");
    p = put_hex(p, v);
    *p++ = '\n';
    *p = 0;
    log_s(b);
}

static int check_uboot(void)
{
    u32 gp, gd;
    __asm__ volatile("mov55 %0, $gp" : "=r"(gp));
    __asm__ volatile("mov55 %0, $r10" : "=r"(gd));
    reloc = gp - UB_GP_LINK;
    if (gd < 0x00100000 || gd >= RAM_END || reloc != rd32((const void *)(gd + 0x40)) ||
        (UB(UB_TEXT_START) & 0xfff) || UB(UB_TEXT_END) > RAM_END)
        return 1;
    for (unsigned i = 0; i < sizeof(ub_sig) / sizeof(ub_sig[0]); i++) {
        const u16 *p = (const u16 *)UB(ub_sig[i][0]);   /* entry points are only halfword aligned */
        if ((rd16(p) | (u32)rd16(p + 1) << 16) != ub_sig[i][1])
            return 1;
    }
    return 0;
}

static int hook(void)
{
    struct stdio_dev **devs = (struct stdio_dev **)rd32((const void *)UB(UB_GOT_STDIO_DEVICES));
    if ((u32)devs != UB(UB_STDIO_DEVICES))
        return 3;
    struct stdio_dev *d = devs[1];   /* stdout; stderr is the same "serial" device */
    if ((u32)d < 0x00100000 || (u32)d >= RAM_END || (u32)d->putc < UB(UB_TEXT_START) ||
        (u32)d->putc >= UB(UB_TEXT_END) || (u32)d->puts < UB(UB_TEXT_START) || (u32)d->puts >= UB(UB_TEXT_END))
        return 4;
    hooked = d;
    orig_putc = d->putc;
    orig_puts = d->puts;
    d->putc = hook_putc;
    d->puts = hook_puts;
    log_hex("capture started, U-Boot reloc", reloc);
    log_hex("ms", ub_ms(0));
    return 0;
}

static void save(const char *name)
{
    log_hex("save ms", ub_ms(0));
    char cmd[96], *p = put_s(cmd, "fatwrite $isp_if $isp_dev 1a00000 ");
    p = put_s(p, name);
    *p++ = ' ';
    p = put_hex(p, log_len);
    *p = 0;
    ub_run(cmd);
    log_len = 0;
    char b[40];
    p = put_s(b, "new log after ");
    p = put_s(p, name);
    p = put_s(p, " ms=");
    p = put_dec(p, ub_ms(0));
    *p++ = '\n';
    *p = 0;
    log_s(b);
}

#define WALK_BUF ((volatile u32 *)0x02000000)

static u32 parse_hex(const char *s)
{
    u32 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++) {
        char c = *s;
        v = v << 4 | (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

static void walk(u32 addr, u32 len, u32 chunk, const char *name)
{
    log_hex("walk", addr);
    log_hex("ms", ub_ms(0));
    for (u32 off = 0; off < len; off += chunk) {
        for (u32 i = 0; i < chunk; i += 4)
            WALK_BUF[i / 4] = *(volatile const u32 *)(addr + off + i);
        char cmd[96], *p = put_s(cmd, off ? "fatappend" : "fatwrite");
        p = put_s(p, " $isp_if $isp_dev 2000000 ");
        p = put_s(p, name);
        *p++ = ' ';
        p = put_hex(p, chunk);
        *p = 0;
        if (ub_run(cmd)) {
            log_hex("write failed at offset", off);
            break;
        }
    }
    log_hex("walk done ms", ub_ms(0));
}

extern u32 __bss_start[], __bss_end[];

__attribute__((section(".text.entry"), used))
int payload_entry(int argc, char *const argv[])
{
    const char *cmd = argc > 1 ? argv[1] : "";
    if (cmd[0] == 'h') {
        for (u32 *p = __bss_start; p < __bss_end; p++)
            *p = 0;
        if (check_uboot())
            return 2;
        return hook();
    }
    if (cmd[0] == 's' && argc > 2 && hooked) {
        if (check_uboot())
            return 2;
        save(argv[2]);
        return 0;
    }
    if (cmd[0] == 'w' && argc > 5 && hooked) {
        if (check_uboot())
            return 2;
        u32 addr = parse_hex(argv[2]), len = parse_hex(argv[3]), chunk = parse_hex(argv[4]);
        if ((addr | len | chunk) & 3 || !chunk || chunk > 0x100000)
            return 5;
        walk(addr, len, chunk, argv[5]);
        return 0;
    }
    return 1;
}
