/* First on-target payload: run from U-Boot `go`, write a text report of CPU state to a fixed RAM buffer that the
 * calling script saves to the SD card. Calls no firmware code. */
typedef unsigned int u32;

#define OUT ((char *)0x01500000)

#define MFSR(reg) ({ u32 _v; __asm__ volatile("mfsr %0, $" #reg : "=r"(_v)); _v; })

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static char *hex(char *p, u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        *p++ = "0123456789abcdef"[(v >> i) & 0xf];
    return p;
}

static char *kv(char *p, const char *k, u32 v)
{
    p = put(p, k);
    p = put(p, "=0x");
    p = hex(p, v);
    return put(p, "\n");
}

__attribute__((section(".text.entry"), used))
int payload_entry(int argc, char *const argv[])
{
    char *p = OUT;
    u32 sp, gp, lp;
    __asm__ volatile("mov55 %0, $sp" : "=r"(sp));
    __asm__ volatile("mov55 %0, $gp" : "=r"(gp));
    __asm__ volatile("mov55 %0, $lp" : "=r"(lp));

    p = put(p, "hello from nds32 payload\n");
    p = kv(p, "argc", argc);
    for (int i = 0; i < argc && i < 4; i++) {
        p = put(p, "argv=");
        p = put(p, argv[i]);
        p = put(p, "\n");
    }
    p = kv(p, "entry", (u32)payload_entry);
    p = kv(p, "sp", sp);
    p = kv(p, "gp", gp);
    p = kv(p, "lp", lp);
    p = kv(p, "PSW", MFSR(PSW));
    p = kv(p, "IVB", MFSR(IVB));
    p = kv(p, "CPU_VER", MFSR(CPU_VER));
    p = kv(p, "ICM_CFG", MFSR(ICM_CFG));
    p = kv(p, "DCM_CFG", MFSR(DCM_CFG));
    p = kv(p, "MMU_CFG", MFSR(MMU_CFG));
    p = kv(p, "MSC_CFG", MFSR(MSC_CFG));
    p = kv(p, "MMU_CTL", MFSR(MMU_CTL));
    p = kv(p, "CACHE_CTL", MFSR(CACHE_CTL));
    p = put(p, "end\n");
    while (((u32)p & 0xff) != 0)
        *p++ = ' ';
    return 0;
}
