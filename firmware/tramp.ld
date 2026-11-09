/* The EXEC trampoline runs from 0x02f00000 (copied there by the payload, or uploaded by hu.py exec --bootstrap). */
OUTPUT_ARCH(nds32)
ENTRY(tramp)

SECTIONS
{
    . = 0x02f00000;
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.* .sdata_*) }
    .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
    ASSERT(SIZEOF(.bss) == 0, "trampoline must not use bss")
    ASSERT(. <= 0x02f0f000, "trampoline overlaps its argument area at 0x02f0f000")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.nds32_e_flags) }
}
