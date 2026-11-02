/* Boot the stock eCos with the snapshot hook (notes/vpp0_display.md, "eCos register snapshot").
 *
 * Before EXEC, tools/ecos_snap.py has written: the decompressed eCos image (identical to the LZO image bootsp loads from
 * flash 0x95CF40), with the hook patches applied, at 0x00301040; HOOK.BIN at 0x06F00000; and a parameter block at
 * PARAM. This replaces what U-Boot's `bootsp ecos nor` does after loading (sub_00f1d1c8 / sub_00f1cc02): SetBootDev
 * pinmux, D-cache write-back + invalidate, then jral 0x301040 with r0 = r1 = r2 = 0. It also undoes what our payload
 * left running: the hardware watchdog EXEC armed (eCos does not feed it) and the EHCI controller. Returns (the
 * trampoline then resets through U-Boot) if the image CRC or the patches do not check out. */
typedef unsigned int u32;
#define R(a) (*(volatile u32 *)(a))
#define ECOS   0x00301040u
#define PARAM  0x06F07F00u   /* { magic "ELNC", len, crc32 } */
#define SNAP   0x06F08000u
#define HCOR   0x9c104120u

static u32 crc32(const unsigned char *p, u32 n)
{
    u32 c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c >> 1 ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

int launch(int argc, char **argv)
{
    (void)argc, (void)argv;
    const volatile u32 *prm = (const volatile u32 *)PARAM;
    if (prm[0] != 0x434e4c45u)
        return 3;
    __asm__ volatile("setgie.d\n\tdsb" ::: "memory");
    if (crc32((const unsigned char *)ECOS, prm[1]) != prm[2])
        return 4;
    /* patches present: call site 0x5114ec -> entry.S at 0x301000 (big-endian instruction bytes), entry and hook loaded */
    const volatile unsigned char *b = (const volatile unsigned char *)0x5114ec;
    if (b[0] != 0x46 || b[1] != 0xf0 || b[2] != 0x03 || b[3] != 0x01)
        return 5;
    if (R(0x06F00000) == 0 || R(0x06F00000) == ~0u || R(0x00301000) == 0 || R(0x00301000) == ~0u)
        return 6;
    /* hardware watchdog ~13 s, routed to system reset: the hook pets it every vsync, so if eCos never gets there or
     * hangs, the unit resets on its own and the next boot shows how far it got (the snapshot header) */
    R(0x9c000630) = 0x3877;
    R(0x9c000630) = 0xab00;
    R(0x9c000634) = 0xffff;
    R(0x9c000630) = 0xab01;
    R(0x9c000630) = 0x7482;
    R(0x9c0000f4) = 0x00120012;
    R(0x9c000630) = 0x4a4b;
    R(HCOR) &= ~1u;                  /* EHCI run/stop = 0 */
    for (u32 i = 0; i < 1000000 && !(R(HCOR + 4) & (1u << 12)); i++)
        ;
    R(HCOR) |= 2u;                   /* HCRESET */
    for (u32 i = 0; i < 1000000 && (R(HCOR) & 2u); i++)
        ;
    for (u32 *z = (u32 *)SNAP; z < (u32 *)0x06F40000u; z++)   /* the hook starts from a zero header */
        *z = 0;
    R(PARAM + 12) = 0x48434e4cu;   /* "LNCH": the launcher got this far (collect reports it) */
    R(0x9c000084) = 0x60000000u | 1u << 13;   /* SetBootDev(SPI_NOR, 1) (U-Boot 0xf0aa62) */
    R(0x9c000088) = 0x00030000u | 1u;
    u32 im;
    __asm__ volatile("mfsr %0, $int_mask" : "=r"(im));
    im &= ~0xffffu;
    __asm__ volatile("mtsr %0, $int_mask\n\tdsb" ::"r"(im));
    __asm__ volatile("cctl l1d_wball, alevel\n\tmsync all\n\tcctl l1d_invalall\n\tdsb" ::: "memory");
    for (u32 p = ECOS & ~31u; p < ECOS + prm[1]; p += 16)   /* I-cache: no invalidate-all op; by address, 16 B steps */
        __asm__ volatile("cctl %0, L1I_VA_INVAL" ::"r"(p) : "memory");
    for (u32 p = 0x00301000u; p < 0x00301040u; p += 16)
        __asm__ volatile("cctl %0, L1I_VA_INVAL" ::"r"(p) : "memory");
    for (u32 p = 0x06F00000u; p < 0x06F08000u; p += 16)
        __asm__ volatile("cctl %0, L1I_VA_INVAL" ::"r"(p) : "memory");
    __asm__ volatile("dsb\n\tisb" ::: "memory");
    ((void (*)(int, int, int))ECOS)(0, 0, 0);
    for (;;)
        ;
}
