/* 64-byte trampoline placed in the unused uImage header slot of the eCos image (0x301000..0x301040, never read by
 * eCos). eCos's call of its vsync dispatcher (0x5114ec) is redirected here. eCos runs with its MPU on and segment 0
 * limited to 64 MB, so the hook at 0x06F00000 runs with instruction/data translation off (PSW bits 6/7), then the
 * PSW is restored and the original dispatcher f_0050f284 is entered with the caller's $lp. */
    .section .text.entry, "ax"
    .global snap_entry
snap_entry:
    addi    $sp, $sp, -8
    swi     $lp, [$sp + 0]
    mfsr    $r15, $psw
    swi     $r15, [$sp + 4]
    bitci   $r15, $r15, #0xc0
    mtsr    $r15, $psw
    isb
    sethi   $r15, hi20(0x06F00000)
    jral    $lp, $r15
    lwi     $r15, [$sp + 4]
    mtsr    $r15, $psw
    isb
    lwi     $lp, [$sp + 0]
    addi    $sp, $sp, 8
    sethi   $r15, hi20(0x0050f284)
    ori     $r15, $r15, lo12(0x0050f284)
    jr      $r15
