/* Call loggers for the audio snapshot run (tools/ecos_snap.py --audio; notes/audio.md "eCos audio trace").
 *
 * Linked at 0x317cb6, over eCos's `pwm` shell-command handler (f_00317cb6, 1288 bytes), which only the shell's
 * command table in flash (0x98068540) points to: dead code at runtime. Each hooked function's first 6 bytes (its
 * push25 + addi $fp prologue, or smw.adm + addi10.sp for the varargs print) are replaced by `j stub_N; nop16`. The
 * stub saves $r16..$r19 and $lp, calls `common` with the hook id in $r16, restores, runs the 6 displaced bytes and
 * jumps back to site + 6. Runs with eCos's translation on: only data stores to 0x02F6xxxx/0x02F7xxxx (inside its
 * 64 MB segment, in the area the loader zeroed).
 *
 * Ring A (0x02F68000, ids < 16: the DSP / AUDIF calls) and ring B (0x02F78000, id 16: eCos's debug print 0x5fd67a,
 * only when called from the audio driver, 0x3a0000..0x3bffff): word 0 counts calls, entries of 32 bytes from +32
 * { id, r0, r1, r2, r3, caller lp, hook tick, r4 }; a full ring keeps its first 2047 entries (the bring-up order). */
#define RINGA 0x02F68000
#define RINGB 0x02F78000
#define NENT 2047

    .section .text
    .global common
common:
    lwi     $r18, [$sp + 16]
    slti    $r15, $r16, 16
    bnez    $r15, 1f
    sethi   $r17, hi20(0x003a0000)
    sub     $r15, $r18, $r17
    sethi   $r17, hi20(0x00020000)
    slt     $r15, $r15, $r17
    beqz    $r15, 9f
    sethi   $r17, hi20(RINGB)
    j       2f
1:  sethi   $r17, hi20(RINGA)
2:  lwi     $r19, [$r17 + 0]
    addi    $r15, $r19, 1
    swi     $r15, [$r17 + 0]
    slti    $r15, $r19, NENT
    beqz    $r15, 9f
    slli    $r19, $r19, 5
    add     $r19, $r19, $r17
    swi     $r16, [$r19 + 32]
    swi     $r0, [$r19 + 36]
    swi     $r1, [$r19 + 40]
    swi     $r2, [$r19 + 44]
    swi     $r3, [$r19 + 48]
    swi     $r18, [$r19 + 52]
    sethi   $r15, hi20(0x02F48000)
    lwi     $r15, [$r15 + 8]
    swi     $r15, [$r19 + 56]
    swi     $r4, [$r19 + 60]
9:  ret     $lp

/* STUB id, site, then the 6 displaced bytes (memory order, as the disassembly prints them) */
.macro STUB id, site, b0, b1, b2, b3, b4, b5
    .global stub_\id
    .balign 2
stub_\id:
    addi    $sp, $sp, -24
    swi     $r16, [$sp + 0]
    swi     $r17, [$sp + 4]
    swi     $r18, [$sp + 8]
    swi     $r19, [$sp + 12]
    swi     $lp, [$sp + 16]
    movi    $r16, \id
    jal     common
    lwi     $lp, [$sp + 16]
    lwi     $r19, [$sp + 12]
    lwi     $r18, [$sp + 8]
    lwi     $r17, [$sp + 4]
    lwi     $r16, [$sp + 0]
    addi    $sp, $sp, 24
    .byte   \b0, \b1, \b2, \b3, \b4, \b5
    j       site_\id + 6   /* site_N: --defsym in the Makefile, so the linker resolves the PC-relative jump */
.endm

    STUB 1,  0x3bbf74, 0xfc, 0x60, 0x51, 0xcf, 0x80, 0x30   /* DSP_SendCmd(cmd, p1, p2, p3) */
    STUB 2,  0x3bafe0, 0xfc, 0x21, 0x51, 0xcf, 0x80, 0x20   /* DownloadCode */
    STUB 3,  0x3baf48, 0xfc, 0x41, 0x51, 0xcf, 0x80, 0x28   /* download (second form) */
    STUB 4,  0x3aa8a0, 0xfc, 0x66, 0x51, 0xcf, 0x80, 0x60   /* AUDIF_Set(id, a, b) */
    STUB 5,  0x3bb996, 0xfc, 0x20, 0x51, 0xcf, 0x80, 0x18   /* DSP_InitSystem */
    STUB 6,  0x3aa50a, 0xfc, 0x28, 0x51, 0xcf, 0x80, 0x58   /* AUDIF_ResetDsp */
    STUB 7,  0x3aa184, 0xfc, 0x00, 0x51, 0xcf, 0x80, 0x10   /* master volume (index) */
    STUB 8,  0x3a3f8e, 0xfc, 0x6d, 0x51, 0xcf, 0x80, 0x98   /* AUDIF_Track_Create_Ext */
    STUB 9,  0x3a4636, 0xfc, 0x20, 0x51, 0xcf, 0x80, 0x18   /* AUDIF_Track_Start */
    STUB 16, 0x5fd67a, 0x3a, 0x1f, 0x94, 0x3c, 0xef, 0xfc   /* debug print (fmt, ...) */
