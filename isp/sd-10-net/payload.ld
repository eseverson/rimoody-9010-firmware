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
    .data : { __hu_data_start = .; *(.data .data.* .sdata .sdata.* .sdata_*) __hu_data_end = .; }
    /* exception/interrupt vectors (irq.S) are stored here in NET.BIN (0x200 reserved) and copied to 0x01F10000 */
    __vec_load = ALIGN(LOADADDR(.data) + SIZEOF(.data), 16);
    .bss ALIGN(__vec_load + 0x200, 64) (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* .sbss_* .scommon* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    /* big buffers outside the 1 MB payload window (lwIP heap and pools, later AA): 0x01D00000 up to the crash record */
    .bigbss 0x01D00000 (NOLOAD) : { KEEP(*(.bigbss)) }
    ASSERT(0x01D00000 + SIZEOF(.bigbss) <= 0x01F00000, ".bigbss runs into the crash record at 0x01F00000")
    /* run address 0x01F10000: IVB.IVBASE has to be 64 KB aligned */
    .hu_vectors 0x01F10000 : AT(__vec_load) { KEEP(*(.hu_vectors)) }
    /* end of NET.BIN (objcopy -R .bss): install.c compares the card's BOOT.BIN with the running image up to here */
    __hu_image_end = __vec_load + SIZEOF(.hu_vectors);
    /* FreeRTOS heap (heap_4, configTOTAL_HEAP_SIZE = 2 MB): task stacks and kernel objects */
    .rtos_heap 0x02200000 (NOLOAD) : { KEEP(*(.rtos_heap)) }
    ASSERT(SIZEOF(.rtos_heap) <= 0x00200000, "rtos heap runs into the audio DSP window at 0x02400000 (memmap.h)")
    ASSERT(__bss_end <= 0x01A00000, "payload + bss run into the log at 0x01A00000")
    ASSERT(SIZEOF(.hu_vectors) <= 0x200, "vector table bigger than its 0x200 slot")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.nds32_e_flags) }
}
