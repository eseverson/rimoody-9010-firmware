/* 64-byte trampoline in the unused uImage header slot of the eCos image (0x301000..0x301040, never read by eCos).
 * Entry +0 (which = 0): eCos's clock ISR 0x64b428 calls the empty hal_clock_reset 0x607fe2 every tick (call at
 * 0x64b436); redirected here it gives a fixed-rate hook. Entry +4 (which = 1): the display-1 vsync dispatcher
 * f_0050f7c0 (call at 0x511628 in the FIELD ISR f_005115e8). eCos runs with its MPU on and segment 0 limited to 64 MB,
 * so snap_hook(which) at 0x02F40000 runs with instruction/data translation off (PSW bits 6/7, saved in $r7 across the
 * call); then the original target is called and pop25 returns to eCos. Keep the 16-bit forms: the slot is 64 bytes. */
    .section .text.entry, "ax"
    .global snap_entry
snap_entry:
    movi55  $r0, #0
    j8      1f
    movi55  $r0, #1
1:  push25  $r8, #0
    mov55   $r6, $r0
    mfsr    $r7, $psw
    bitci   $r15, $r7, #0xc0
    mtsr    $r15, $psw
    isb
    sethi   $r15, hi20(0x02F40000)
    jral5   $r15
    mtsr    $r7, $psw
    bnez38  $r6, 2f
    sethi   $r15, hi20(0x00607fe2)
    ori     $r15, $r15, lo12(0x00607fe2)
    j8      3f
2:  sethi   $r15, hi20(0x0050f7c0)
    ori     $r15, $r15, lo12(0x0050f7c0)
3:  jral5   $r15
    pop25   $r8, #0
