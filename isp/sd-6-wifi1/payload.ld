/* Raw image loaded and entered at 0x01800000 by U-Boot `go`; entry = first byte. */
OUTPUT_ARCH(nds32)
ENTRY(payload_entry)

SECTIONS
{
    . = 0x01800000;
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.* .sdata_*) }
    . = ALIGN(64);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* .sbss_* .scommon* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    ASSERT(__bss_end <= 0x01900000, "payload overlaps 0x1900000")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.nds32_e_flags) }
}
