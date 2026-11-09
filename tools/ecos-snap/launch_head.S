/* "HUNE" v1 header at +0x40 so EXEC and the trampoline accept the image (same layout as the payload's head.S) */
    .section .text.head, "ax"
    .global launch_head
launch_head:
    j launch
    .org 0x40
    .ascii "HUNE"
    .word 1
    .asciz "ecos-launch"
    .org 0x80
