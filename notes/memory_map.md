# Payload memory map (64 MB DRAM)

The unit has **64 MB** of DRAM. CPU addresses 0x04000000..0x07FFFFFF are aliases of 0x00000000..0x03FFFFFF (a write at 0x02F80000 reads back at 0x06F80000), and 0x08000000 up (below the registers at 0x9C000000) hangs the bus (notes/vpp0_display.md). U-Boot was built for 128 MB and runs from the alias: "relocated to 0x07CC3000" is physically 0x03CC3000. DMA masters (H.264 decoder, DDFCH, the audio DSP) are not covered by the alias, so they must be given addresses below 0x04000000.

The source of truth is `firmware/memmap.h`. `memmap_check.c` asserts at compile time that the regions below are in address order, do not overlap and end below U-Boot, and that `configTOTAL_HEAP_SIZE` and `LV_MEM_ADR`/`LV_MEM_SIZE` match their slots. `payload.ld` asserts the payload + .bss, .bigbss, vectors and RTOS heap sizes.

| Physical | Size | What | Owner |
|---|---|---|---|
| 0x00000000 | 2500 KB | H.264 bitstream ring (EVB) | vdec.c |
| 0x00400000 | 1028 KB | SPMP command ring | vdec.c |
| 0x00510000 | 2 KB | SCS start-code info (was 0x00500000, inside the SPMP ring's last 4 KB) | vdec.c |
| 0x00540000, 0x00580000 | 128 KB each | decoder working buffers BG2D 0x3B / 0x3C | vdec.c |
| 0x00600000 | 4 × 0x280000 (to 0x00FFC000) | decoder frame slots: luma, CbCr +0x180000, MVC +0x240000 | vdec.c |
| 0x01000000, 0x01200000 | 0x12C000 each | OSD video buffers (RGB565 1024 × 600) | osdvid.c |
| 0x01340000 | 1 MB | AA certificate extraction: the stock AA module from flash, packed and unpacked (only while AACERT.PEM / AAKEY.PEM are missing) | aacert.c, net.c |
| 0x01500000 | 0x800 | HELLO.TXT report | net.c |
| 0x01600000 | 16 KB used (1 MB slot) | MCU RX hex text | net.c |
| 0x01800000 | to < 0x01A00000 | payload image + .bss | payload.ld |
| 0x01A00000 | 960 KB | run log ring | net.c |
| 0x01B00000 | 256 KB | log bounce buffer for fatwrite | net.c |
| 0x01C00000 | 256 KB | previous run's log tail after a crash | net.c |
| 0x01D00000 | to 0x01F00000 | .bigbss (lwIP heap and pools, AA) | payload.ld |
| 0x01F00000 | | crash record; decoder breadcrumbs at 0x01F0F000 (both survive a reset) | irq.c, vdec.c |
| 0x01F10000 | 0x200 | exception vectors (IVB, 64 KB aligned) | irq.S |
| 0x02000000 | 64 KB + 256 | HU1 reply buffer | net.c |
| 0x02100000 | 4 KB | WIFI.TXT (wiped after parsing) | net.c |
| 0x02200000 | 2 MB | FreeRTOS heap (task stacks) | payload.ld, FreeRTOSConfig.h |
| 0x02400000 | 0x4C4800 | audio DSP window (microcode banks, data memory, PCM rings) | audio.c |
| 0x02A00000 | 1 MB | LVGL heap | lv_conf.h |
| 0x02B00000 | 480 KB | LVGL draw buffer (1024 × 120 × 4) | ui.c |
| 0x02B80000 | 3.5 MB | install to flash: the image being copied (1.6 MB), then the stage 1 plan's blocks | install.c |
| 0x02F00000 | 64 KB | EXEC trampoline, argv at +0xF000 | tramp.ld, net.c |
| 0x03000000 | 4 MB | hu.py upload area (EXEC images ≤ 0x180000, tools/fb.py to 0x032F0080) | tools |
| 0x03400000 | 0x258000 | console / UI framebuffer (1024 × 600 × 4) | net.c, console.c, ui.c |
| 0x03660000, 0x03661000 | 128 bytes each | OSD region headers: framebuffer, video layer | net.c, osdvid.c |
| 0x03700000 | to 0x04000000 | **U-Boot**: stack, malloc area, code | U-Boot |

U-Boot's part, measured at boot (build 20261102-114400, `mem:` line in the log): `gd->start_addr_sp` 0x077B2F20, gd 0x077B2F30, sp at payload entry 0x077B16C0, relocated code 0x07CC3000. Malloc area 0x037B3000..0x03CC3000 (notes/console.md). The boot stack grows down from 0x037B2F20; the payload only uses it before the scheduler starts and for the EXEC trampoline (`port_call_on_stack`). `mem_boot_check()` in net.c logs these values every boot and says `OVERLAP` if the stack (with 256 KB of headroom), gd or the code reach below 0x03700000.

HU1 READ/WRITE (`plat_mem_ok`): refuses 0x08000000..0x9BFFFFFF (bus hang). Alias addresses stay readable and writable (U-Boot's own pointers are 0x07xxxxxx), but writes are checked against the protected regions (payload, log, HU1 reply buffer) by physical address.

The stock eCos snapshot boot (`ecos_boot_check`, tools/ecos-snap) loads ENTRY.BIN to 0x00301000 and HOOK.BIN to 0x02F40000 and hands the machine to eCos; it runs before any of the above is in use and does not come back.
