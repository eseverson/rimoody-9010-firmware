/* EXEC trampoline, linked to run at 0x02f00000. Called like a U-Boot `go` target:
 *   tramp(argc, argv) with argv[1] = image address, argv[2] = length (hex), and optionally argv[3..5] = stdio_dev
 *   address, original putc, original puts (to undo a console hook that lives in the image being replaced; the
 *   payload's own EXEC restores the hooks itself and passes only src and len).
 * It checks the image ("HUNE" header v1 at +0x40, 0x100 <= len <= 1.5 MB), returns 2 if it is not acceptable, else copies
 * it to 0x01800000, writes back the D-cache and invalidates the I-cache for it, and calls it like `go 0x1800000 main`
 * with $r10 (U-Boot's gd) and $gp untouched. If that returns, it resets the unit through U-Boot's parse_string_outer
 * (found from $gp, checked against its first instruction word), so the card's BOOT.BIN comes back. */
typedef unsigned int u32;
typedef unsigned short u16;

#define DST           0x01800000u
#define UB_GP_LINK    0x00fb3d18u
#define UB_PARSE_STR  0x00f0d7feu
#define MFSR(reg) ({ u32 _v; __asm__ volatile("mfsr %0, $" #reg : "=r"(_v)); _v; })

static char a0[] = "0x1800000", a1[] = "main";
static char *av[] = { a0, a1, 0 };

static u32 hex(const char *s)
{
    u32 v = 0;
    if (s[0] == '0' && (s[1] | 32) == 'x')
        s += 2;
    for (; *s; s++) {
        int c = *s | 32, d = *s >= '0' && *s <= '9' ? *s - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            break;
        v = v << 4 | (u32)d;
    }
    return v;
}

__attribute__((section(".text.entry"), used))
int tramp(int argc, char *argv[])
{
    if (argc < 3)
        return 1;
    u32 src = hex(argv[1]), len = hex(argv[2]);
    const volatile u32 *s = (const volatile u32 *)src;
    if ((src & 3) || len < 0x100 || len > 0x180000 || s[0x40 / 4] != 0x454e5548 || s[0x44 / 4] != 1)
        return 2;
    if (argc >= 6) {
        u32 dev = hex(argv[3]);
        if (dev) {
            *(volatile u32 *)(dev + 0x20) = hex(argv[4]);
            *(volatile u32 *)(dev + 0x24) = hex(argv[5]);
        }
    }
    volatile u32 *d = (volatile u32 *)DST;
    for (u32 i = 0; i < (len + 3) / 4; i++)
        d[i] = s[i];
    u32 df = (MFSR(DCM_CFG) >> 6) & 7, ifl = (MFSR(ICM_CFG) >> 6) & 7;
    u32 dl = df ? 8u << (df - 1) : 32, il = ifl ? 8u << (ifl - 1) : 32;
    for (u32 p = DST; p < DST + len; p += dl)
        __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(p) : "memory");
    __asm__ volatile("msync all" ::: "memory");
    for (u32 p = DST; p < DST + len; p += il)
        __asm__ volatile("cctl %0, L1I_VA_INVAL" ::"r"(p) : "memory");
    __asm__ volatile("dsb\n\tisb" ::: "memory");
    ((int (*)(int, char **))DST)(2, av);

    u32 gp;
    __asm__ volatile("mov55 %0, $gp" : "=r"(gp));
    const volatile u16 *ps = (const volatile u16 *)(UB_PARSE_STR + gp - UB_GP_LINK);
    if (ps[0] == 0x64fc && ps[1] == 0xd047)
        ((int (*)(const char *, int))ps)("reset", 3);
    for (;;)
        ;
}
