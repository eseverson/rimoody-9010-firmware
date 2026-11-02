/* eCos register snapshot hook: linked at 0x02F40000 (DRAM there survives a watchdog reset and the next boot) */
OUTPUT_ARCH(nds32)
ENTRY(snap_hook)
SECTIONS
{
    . = 0x02F40000;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) }
    ASSERT(. <= 0x02F47F00, "hook code/data must stay below the snapshot area at 0x02F47F00")
}
