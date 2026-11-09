OUTPUT_ARCH(nds32)
ENTRY(snap_entry)
SECTIONS
{
    . = 0x00301000;
    .text : { KEEP(*(.text.entry)) }
    ASSERT(. <= 0x00301040, "the trampoline must fit the 64-byte uImage header slot")
}
