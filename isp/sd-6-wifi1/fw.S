/* RTL8188FU 8051 firmware (linux-firmware rtlwifi/rtl8188fufw.bin): 32-byte header, then the image. */
    .section .rodata.fw, "a"
    .balign 4
    .global fw_bin
fw_bin:
    .incbin "rtl8188fufw.bin"
    .global fw_bin_end
fw_bin_end:
