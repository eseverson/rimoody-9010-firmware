/* TRAMP.BIN (tramp.c linked for 0x02f00000), copied there by HU1 EXEC */
    .section .rodata.tramp, "a"
    .balign 4
    .global tramp_bin
tramp_bin:
    .incbin "TRAMP.BIN"
    .global tramp_bin_end
tramp_bin_end:
