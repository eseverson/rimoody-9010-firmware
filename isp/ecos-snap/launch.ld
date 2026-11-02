/* eCos launcher, started by the payload's HU1 EXEC (copied to 0x01800000 by the trampoline) */
OUTPUT_ARCH(nds32)
ENTRY(launch_head)
SECTIONS
{
    . = 0x01800000;
    .text : { KEEP(*(.text.head)) KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) }
}
