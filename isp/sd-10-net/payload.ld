/* Raw image loaded and entered at 0x01800000 by U-Boot `go`; entry = first byte. */
OUTPUT_ARCH(nds32)
ENTRY(payload_entry)

SECTIONS
{
    . = 0x01800000;
    .text : {
        KEEP(*(.text.head))
        KEEP(*(.text.entry))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : { *(.data .data.* .sdata .sdata.* .sdata_*) }
    /* exception/interrupt vectors (irq.S) are stored here in NET.BIN (0x200 reserved) and copied to 0x01F10000 */
    __vec_load = ALIGN(LOADADDR(.data) + SIZEOF(.data), 16);
    .bss ALIGN(__vec_load + 0x200, 64) (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* .sbss_* .scommon* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    /* run address 0x01F10000: IVB.IVBASE has to be 64 KB aligned */
    .hu_vectors 0x01F10000 : AT(__vec_load) { KEEP(*(.hu_vectors)) }
    /* big buffers outside the 1 MB payload window (lwIP heap and pools, later AA): 0x01D00000 up to the crash record */
    .bigbss 0x01D00000 (NOLOAD) : { KEEP(*(.bigbss)) }
    ASSERT(0x01D00000 + SIZEOF(.bigbss) <= 0x01F00000, ".bigbss runs into the crash record at 0x01F00000")
    /* FreeRTOS heap (heap_4, configTOTAL_HEAP_SIZE = 2 MB): task stacks and kernel objects */
    .rtos_heap 0x02200000 (NOLOAD) : { KEEP(*(.rtos_heap)) }
    ASSERT(SIZEOF(.rtos_heap) <= 0x00800000, "rtos heap runs into the EXEC trampoline at 0x2f00000")
    ASSERT(__bss_end <= 0x01900000, "payload overlaps 0x1900000")
    ASSERT(SIZEOF(.hu_vectors) <= 0x200, "vector table bigger than its 0x200 slot")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.nds32_e_flags) }
}
