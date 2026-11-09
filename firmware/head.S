/* First bytes of NET.BIN at 0x01800000: `go 0x1800000 ...` lands on the jump; the header at 0x40 identifies the image
 * for HU1 EXEC and the trampoline: "HUNE", header version 1, build id (NUL-terminated, <= 55 chars). */
    .section .text.head, "ax"
    .global hu_head
hu_head:
    j payload_entry
    .org 0x40
    .ascii "HUNE"
    .word 1
    .global hu_build_id
hu_build_id:
    .asciz BUILD_ID
    .org 0x80
