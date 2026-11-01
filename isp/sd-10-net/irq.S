/* Exception / interrupt vectors for the payload (NDS32 V3, IVIC internal vectored mode, 16-byte entries).
 *
 * The table is linked to run at 0x01F10000 (IVB.IVBASE must be 64 KB aligned) and stored in NET.BIN right after
 * .data; irq_init() copies it there and points IVB at it. Entry n pushes $r0/$r1, puts n in $r0 and jumps (PC-relative,
 * within 16 MB) to hu_trap_common in the payload. Vectors: 0 reset/NMI, 1-4 TLB, 5 machine error, 6 debug, 7 general
 * exception, 8 syscall, 9-14 HW0-HW5 (the SP7021-style intc drives HW0/HW1), 15 software interrupt. Same layout as
 * Sunplus eCos (vectors copied to 0x740000, ESZ = 16 bytes, ecos 0x606e7e / 0x606ee6).
 *
 * hu_trap_common saves the complete context on the interrupted stack (r0-r27, fp, gp, lp, IPC, IPSW, IFC_LP; eCos
 * also saves IFC_LP), calls hu_trap(vector, frame) and restores everything, IPC/IPSW included, before iret. hu_trap
 * may edit frame[0] (IPC) to resume after a trap. Frame layout (words): 0 IPC, 1 IPSW, 2 IFC_LP, 3..28 r2..r27,
 * 29 fp, 30 gp, 31 lp, 32 r0, 33 r1; the interrupted $sp is frame + 136. */
    .section .hu_vectors, "ax"
    .balign 16
    .global hu_vectors
hu_vectors:
    .irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    .balign 16
    smw.adm $r0, [$sp], $r1, #0
    movi $r0, \n
    j hu_trap_common
    .endr
    .balign 16
    .global hu_vectors_end
hu_vectors_end:

    .text
    .balign 4
    .global hu_trap_common
hu_trap_common:
    smw.adm $r2, [$sp], $r27, #0xe
    mfsr $r1, $ipc
    mfsr $r2, $ipsw
    mfusr $r3, $ifc_lp
    smw.adm $r1, [$sp], $r3, #0
    mov55 $r1, $sp
    sethi $r15, hi20(hu_trap)
    ori $r15, $r15, lo12(hu_trap)
    jral $lp, $r15
    /* FreeRTOS port: switch tasks on the way out if the tick or a yield asked for it. The frame just built is the
     * outgoing task's saved context; pxCurrentTCB->pxTopOfStack (offset 0) records where it is. */
    sethi $r15, hi20(port_switch_pending)
    ori $r15, $r15, lo12(port_switch_pending)
    lwi $r0, [$r15 + 0]
    beqz $r0, hu_trap_restore
    movi $r0, 0
    swi $r0, [$r15 + 0]
    sethi $r15, hi20(pxCurrentTCB)
    ori $r15, $r15, lo12(pxCurrentTCB)
    lwi $r0, [$r15 + 0]
    swi $sp, [$r0 + 0]
    sethi $r15, hi20(vTaskSwitchContext)
    ori $r15, $r15, lo12(vTaskSwitchContext)
    jral $lp, $r15
    sethi $r15, hi20(pxCurrentTCB)
    ori $r15, $r15, lo12(pxCurrentTCB)
    lwi $r0, [$r15 + 0]
    lwi $sp, [$r0 + 0]
    .global hu_trap_restore
hu_trap_restore:
    lmw.bim $r1, [$sp], $r3, #0
    mtusr $r3, $ifc_lp
    mtsr $r1, $ipc
    mtsr $r2, $ipsw
    lmw.bim $r2, [$sp], $r27, #0xe
    lmw.bim $r0, [$sp], $r1, #0
    iret

/* port_start_first(frame): the first task's prepared frame becomes $sp, then the common restore and iret
 * (GIE comes from the frame's IPSW). Called with GIE off, outside any trap. */
    .global port_start_first
port_start_first:
    mov55 $sp, $r0
    j hu_trap_restore

/* int port_call_on_stack(fn, a0, a1, sp): fn(a0, a1) on stack sp, back on the caller's stack afterwards */
    .global port_call_on_stack
port_call_on_stack:
    smw.adm $r6, [$sp], $r6, #0x2
    mov55 $r6, $sp
    mov55 $sp, $r3
    mov55 $r15, $r0
    mov55 $r0, $r1
    mov55 $r1, $r2
    jral $lp, $r15
    mov55 $sp, $r6
    lmw.bim $r6, [$sp], $r6, #0x2
    ret

/* resumable test trap (SWID 0x7e57, see hu_trap) and the fatal one (SWID 0x0bad) */
    .global irq_trap_test
irq_trap_test:
    trap 0x7e57
    ret5 $lp

    .global irq_trap_fatal
irq_trap_fatal:
    trap 0x0bad
    ret5 $lp
