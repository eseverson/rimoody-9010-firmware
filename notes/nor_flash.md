# SPI NOR flash: part, controller, driver

Status 2026-11-09: driver (`firmware/nor.c`), HU1 op 29 and `hu.py nor` written and host-tested. Nothing has run on the unit yet. Sources: vendor U-Boot's `sfsp` driver (disassembly of the U-Boot image in the NOR dump; addresses below are U-Boot load addresses), the flash-part table inside that U-Boot, the CH341A detection of the chip, and the original dump (`dump1.bin`). Labels: **proven** = read from code or tables, **inferred** = a reading that the on-unit test still has to confirm, **memory** = recalled from the part family's datasheets, not checked against the XMC datasheet in this pass.

Goal: let the payload keep a known-good copy of itself in the unused part of the 16 MB NOR. Boot order would later become SD card `BOOT.BIN`, then the NOR copy, then stock eCos. This note covers the driver and its test surface only, not the install feature or the boot-script change.

## The part

- **XMC XM25QH128C, JEDEC ID 20 40 18 (proven).** flashrom on the CH341A clip reported `Found XMC flash chip "XM25QH128C/XM25QH128D" (16384 kB, SPI)`. U-Boot's flash table has the matching entry: name `XM25QH128C`, ID 0x204018, sector 0x1000, block 0x10000, size 0x1000000, flags 0 (table at 0xFB2138).
- U-Boot's per-part tables for it (proven): clocks 0x025A5A32 (`Normal Read 50 MHz`, `Normal Write 90 MHz`, `Fast Read 90 MHz`, `Data IO 2`, as FlashInit prints them); fast read 0xEB, cmd 1 line, address 4, data 4, 6 dummy cycles (used for XIP and for `sfsp read`); protect table 0xFA05C8 and unprotect table 0xFA0578 (below).
- Commands the driver uses (standard for the family; the ones marked U-Boot are what `sfsp` sends to this part): 0x9F JEDEC ID (U-Boot), 0x05 / 0x35 / 0x15 read SR1 / SR2 / SR3 (U-Boot), 0x06 write enable (U-Boot), 0x01 / 0x31 write SR1 / SR2 with one byte (U-Boot), 0x20 4 KB sector erase (U-Boot), 0xD8 64 KB block erase (U-Boot), 0x02 page program (sent by the controller's DMA engine, see below), 0x03 read (ours; U-Boot reads with 0xEB).
- Status bits: SR1 bit 0 WIP (busy), bit 1 WEL, bits 4..2 BP2..0, bit 5 TB, bit 6 SEC, bit 7 SRP0; SR2 bit 1 QE, bit 6 CMP (Winbond layout; U-Boot's tables mask exactly these bits for this part: proven for the masks, memory for the names).
- Timing (memory, the W25Q128-class values XMC matches): page program typ 0.5 ms / max 3 ms, 4 KB erase typ 45 ms / max 400 ms, 64 KB erase typ 0.15 s / max 2 s, status write max 15 ms. The driver's timeouts: 20 ms, 1 s, 4 s and 200 ms.

## Block protection: U-Boot protects at every boot

`FlashInit` (0xF082E6) runs from U-Boot's `main_loop` on every boot, before `bootcmd`, and ends with the protect routine 0xF09DE0 (proven). For this part the protect table is: read SR1; write enable; write SR1 = (SR1 & ~0x7C) | 0x04; read SR2; write enable; write SR2 = (SR2 & ~0x40) | 0x40; then it prints SR3 ("3 check data"). That is BP = 001, TB = 0, SEC = 0, CMP = 1: **everything from 0x000000 to 0xFBFFFF is write-protected, the top 256 KB (0xFC0000..0xFFFFFF) stays writable** for eCos's settings (they live at 0xFC0000 and 0xFF0000 in the dump). The unprotect table clears the same bits (SR1 &= ~0x7C, SR2 &= ~0x40). `tools/flash_bootcmd.py` ran `sfsp unprotect` before and `sfsp protect` after its writes, the same pair.

So every write the payload makes into the free region first has to clear the protection. The driver reads SR1 and SR2, clears BP/TB/SEC and CMP the way U-Boot's unprotect does, checks they cleared, runs the erase or program, then writes back exactly the SR1 and SR2 it found and checks them. A write that fails half way still restores. Each restore is two non-volatile status writes; at four per write operation the status register's endurance is not a concern. SR2's one-time lock bits are never set: the driver only ever writes values it read, minus CMP.

## Controller (0x9C000B00)

FlashInit stores the base 0x9C000B00 at DRAM 0x80C (the `sfsp` driver keeps its state at DRAM 0x800: part table pointer at +0, register base at +0xC) and every `sfsp` routine works through that pointer (proven). The register names follow the upstream SP7021 SPI-NOR controller's register order (spi_ctrl, spi_timing, spi_page_addr, spi_data, spi_status, spi_auto_cfg, spi_cfg0..2, ..., spi_mem_data_addr at 0x34, interrupt mask 0x44, interrupt status 0x48); that order is from memory of the SP7021 sources, and every offset and bit below was read from this U-Boot.

| Offset | Name | Use (U-Boot routine) |
|---|---|---|
| 0x00 | CTRL | [31] busy (every routine polls it before writing CTRL), [29] set by FlashInit 0xF08F06, [28] set around a DMA program (0xF08EAA / 0xF08ED8), [26] address bank for parts over 16 MB, [23:16] and up: clock setup (kept as is: every command writes `(CTRL & 0xFFFF0000) | cmd << 8 | flags`), [15:8] command, [6:4] data bytes 0..4 (jump table at 0xF08A98: 1 → 0x10 ... 4 → 0x40), [2] direction (1 = to the flash), [1:0] address bytes |
| 0x04 | TIMING | [3:1] from the part's clock entry (0xF0901A) |
| 0x08 | ADDR | flash address of an erase, DMA read or DMA program |
| 0x0C | DATA | PIO data, up to 4 bytes, first byte on the wire in [7:0] (JEDEC: 0xF09474 swaps bytes 0 and 2 to get 0x204018) |
| 0x10 | STATUS | result of a 0x05 read (0xF08A52 returns 0x10 for command 5, 0x0C for every other command) |
| 0x14 | AUTO_CFG | [31:24] read command for XIP and DMA reads, [22] prefetch (set by FlashInit on chip id ≥ 0x651), [21] PIO go (self-clearing), [20] use the [31:24] read command, [18] cleared by FlashInit, [17] DMA go; U-Boot ORs 0x201 in before a DMA program (inferred: 0x02 in [15:8] is the page-program command, bit 0 a hardware write enable) |
| 0x18 | CFG0 | [15:0] DMA length; [20] 8-byte data mode for the unique-ID read (0xF094D6) |
| 0x1C | CFG1 | line modes and dummy cycles for PIO / DMA transfers. 0x00150095 = single line command, address and data, no dummy: FlashInit and every `sfsp read` leave this value. Fields (0xF09904): command lines 0x10001 / ..., address lines 1 → 0x40004, 2 → 0x80008, 4 → 0xC000C, data lines 1 → 0x100090, 2 → 0x200020, 4 → 0x300030, dummy cycles in [31:24] |
| 0x20 | CFG2 | line modes for XIP reads (0xF09CA2: the part's quad fast read) |
| 0x24 | DATA64 | unique-ID read data |
| 0x34 | DMA address | DRAM address of a DMA transfer (must be 4-byte aligned: "dma buffer not 4 byte align") |
| 0x44 | interrupt mask | |
| 0x48 | interrupt status | [1] DMA done; U-Boot clears it by writing back the register with bit 1 set |

FlashInit also pulses bit 8 of 0x9C000054 (module reset), clears CTRL[23:20], sets pinmux 0x9C000084 = 0x60002000 and 0x9C000088 = 0x00030001 and ORs 0x01F80008 into 0x9C000114 (proven). U-Boot also contains a second, older routine set that drives 0x9C000B00 with a different layout (0xF04B96 / 0xF04BBA: command in +0x08, busy in +0x08 bit 7, data in +0x1C); `sfsp` does not use it and neither does the driver.

## Sequences

All PIO commands follow 0xF08A52 / 0xF08DBC / 0xF08CB6 (proven): wait for CTRL[31] = 0; write ADDR if the command has an address; write DATA (0 for a read, the value for a write); write CTRL; set AUTO_CFG[21]; wait for it to clear; read STATUS (command 5) or DATA.

U-Boot waits for CTRL[31] and AUTO_CFG[21] with no limit (udelay(40) loops). The first on-unit write test (2026-11-09) showed why: a 0x05 status read issued while a status-register write was still running did not finish within the driver's original 10 ms bound. The controller holds the 0x05 command until the flash is ready, so its result lands in STATUS, not DATA. The first try failed during the protection clear, before any erase was sent, and the restore that followed set SR1 back to 04. The driver now gives every busy and go wait a 5 s bound, which covers a 64 KB erase. `nor status` records which step timed out (wait code, elapsed time, registers) and the slowest PIO seen. That slowest PIO should show the 0x05 after a status write taking several ms.

| Operation | Driver | Source and confidence |
|---|---|---|
| JEDEC ID | 0x9F, 3 data bytes, read | 0xF09474, proven |
| Read SR1 / SR2 / SR3 | 0x05 / 0x35 / 0x15, 1 byte | 0xF08A52 and the protect tables, proven |
| Write enable | 0x06, flags 0x04 | 0xF089E4, proven |
| Write SR1 / SR2 | WREN, then 0x01 / 0x31 with 1 byte, flags 0x14, then poll 0x05 until WIP = 0 | 0xF08DBC, proven |
| Sector / block erase | WREN, then 0x20 / 0xD8, flags 0x07, ADDR = offset, then poll 0x05 every 20 µs until WIP = 0 | 0xF0925C → 0xF08CB6, proven (U-Boot polls every 40 µs) |
| Page program | WREN (addition), set CTRL[28], CTRL flags 0x07, CFG0[15:0] = length, DMA address, AUTO_CFG \|= 0x201, AUTO_CFG[17] = go, wait interrupt status [1], clear it, wait CTRL[31], clear CTRL[28], then poll WIP (addition) | 0xF095F8 → 0xF08B2C, proven for the register writes. U-Boot sends neither a WREN nor a status poll and hands the controller up to 0xFFFC bytes at any alignment, so the controller issues the write enables and page splits itself (inferred). The driver still splits on 256-byte page boundaries and adds the WREN and the WIP poll; both are harmless if the controller already does them |
| Read | CFG1 = 0x150095, ADDR, DATA = 0, CTRL flags 0x03, CFG0[15:0] = length, DMA address, AUTO_CFG[31:24] = 0x03 with [20] set, AUTO_CFG[17] = go, wait interrupt status [1], clear it | 0xF08BF6, proven for the mechanism; U-Boot reads with 0xEB in quad mode instead. The plain 0x03 at the controller's clock (normal read is rated 50 MHz) is inferred to work; `--cmd 0b` (fast read, CFG1 dummy field = 8) is the fallback, and the dummy-field position is inferred |

Two details in U-Boot's code that the driver does not copy: 0xF08BF6 and 0xF08B2C OR the new command into AUTO_CFG without clearing the old one (the source evidently meant to clear the field; it works there because the values match). The driver writes the read command field cleanly, refuses to program if AUTO_CFG[15:8] holds anything other than 0x00 or 0x02, and restores AUTO_CFG, CFG0 and CFG1 after every step, so XIP keeps the configuration FlashInit gave it.

## XIP hazards and how the driver handles them

While an erase, a page program or a status-register write runs, reads through the XIP window at 0x98000000 return garbage (the flash answers only status reads while WIP = 1), and the driver also changes AUTO_CFG and CFG1, which XIP and DMA reads share. The window is also cached in the CPU's D-cache.

Audit of everything that reads the window in this payload:

- **The payload itself runs from DRAM** (linked at 0x01800000; net.map has no section outside DRAM, and the driver's functions sit at 0x0182xxxx). Exception vectors are at 0x01F10000. No code executes from flash.
- **audio.c**: `download_code` / `download_data` copy the DSP microcode out of the window (flash 0x4AF5E8..0x5941xx) and checksum it. Their only caller is `aud_init` (boot via `audio_boot`, and HU1 AUDIO sub-op 1), both on the net task. The master-volume table is a copy in DRAM. Nothing else in audio.c touches flash.
- **install.c** (install to flash, notes/nor_install.md): the whole-region CRC through the window after a stage 2 write, on the net task between driver windows. The Settings status line is a string the net task prepares; the UI task no longer reads the window.
- **Interrupt handlers** (irq.c `isr`: tick, software watchdog, hardware watchdog pet, MCU heartbeat, UART drain) and the crash path: no flash access.
- **HU1**: READ refuses 0x08000000..0x9BFFFFFF, so the window cannot be read over the network. The CMD op runs U-Boot commands (`md`, `crc32`, `source 98e00000`, `sfsp ...` all read flash) on the net task.
- **U-Boot code we call** (`ub_ms`, `ub_udelay`, printf, `run("usb reset")`, `fatwrite`): runs from its relocated copy in DRAM and does not read flash. The driver itself calls no U-Boot code.
- **`sfsp` from the payload is unsafe after video**: its state lives at DRAM 0x800, inside the H.264 bitstream ring (MM_VDEC_EVB at 0). After any decode the register-base pointer at 0x80C is garbage, and `hu.py cmd 'sfsp ...'` (and `tools/flash_bootcmd.py`) would write to wherever it points. Only use `sfsp` on a fresh boot before AA video, or use the driver.

Handling:

- Every controller step (one PIO command, one DMA read of up to 1 KB, one page program, one erase, the protection clear or restore) runs in a window: `vTaskSuspendAll`, save AUTO_CFG / CFG0 / CFG1, set CFG1 = 0x150095, run the step to completion including WIP = 0, restore, `xTaskResumeAll`. No other task can run inside a window, so the UI task cannot reach `plat_ui_install_status` while the flash is busy. Interrupts stay enabled: the tick, the hardware watchdog pet (6 s) and the MCU heartbeat keep running, and none of them touches flash. The longest window is a 64 KB erase (typically 0.15 s, worst case 2 s); a 4 KB erase is typically 45 ms; a page about 1 ms. During a window the net loop stalls too, so AA video and audio hiccup.
- Writes refuse to run outside the net task (`NOR_ETASK`). `aud_init` only runs on the net task, so it can never be in the middle of a microcode copy when a write starts: the two are serialized by the task, and audio.h already documents "net task only".
- After an erase or program the driver invalidates the D-cache lines of the range (`cctl L1D_VA_INVAL`) and reads one word elsewhere in the window, in case the controller's prefetch buffer (AUTO_CFG[22]) still holds old bytes (inferred precaution).
- DMA buffers: a 32-byte-aligned 1 KB bounce buffer in the payload's .bss (DRAM, below 64 MB); written back before a program, written back and invalidated before a read, invalidated after.
- A timeout while the flash may still be busy sets a sticky fault: every later write is refused (`NOR_EFAULT`) until a reboot, and the HU1 op skips its XIP blank check.

## Where writes may go

The driver accepts erase and program only inside these ranges and fails closed for anything else, at every level (`nor_range_ok` in the HU1 op, in `nor_erase` / `nor_program`, and again per erase unit and per page):

| Range | Size | Why |
|---|---|---|
| 0xC50000..0xC5FFFF | 64 KB | blank in the original dump |
| 0xC70000..0xDFFFFF | 1.56 MB | blank in the dump |
| 0xE10000..0xFBFFFF | 1.69 MB | blank; 0xE00000..0xE0FFFF is the NOR boot script |

Outside these ranges only stage 1 of the install may write, and only the boot script block 0xE00000 and the U-Boot blocks holding the environment and the uImage header sums, through `nor_boot_write` (notes/nor_install.md); the general erase and program paths never reach them.

0xFD0000..0xFEFFFF is blank too but stays out: it lies in the top 256 KB that U-Boot's protect deliberately leaves writable, next to eCos's settings at 0xFC0000 and 0xFF0000, so eCos may use it. The free region (3.3 MB) fits the payload (NET.BIN is about 1.1 MB, EXEC accepts up to 1.5 MB) twice over. Erase granularity: `nor_erase` takes 4 KB-aligned spans that lie wholly inside one range, uses 64 KB blocks where a whole aligned block is inside the span and 4 KB sectors elsewhere, so no erase unit can reach outside. Programs must start on blank flash (the HU1 op checks) and are split on 256-byte pages. `test/nor_host.c` checks the guard against every sector and block of the 16 MB, the page split, and the protection decode.

## Driver and test surface

`nor.h`: `nor_jedec`, `nor_status`, `nor_read` (controller DMA, 0x03 or 0x0B), `nor_erase`, `nor_program`; pure helpers `nor_range_ok`, `nor_page_chunk`, `nor_wp_range`. Writes also require the JEDEC ID 0x204018, the net task and no earlier fault.

HU1 op 29 (`hu.py nor`):

| Sub-op | hu.py | What |
|---|---|---|
| 0 | `nor status` | JEDEC ID, SR1..3, decoded protection, CTRL / TIMING / AUTO_CFG / CFG0..2 / interrupt status, the write ranges, the fault flag |
| 1 | `nor compare OFF LEN [--cmd 0b]` | controller read against the XIP window, CRC of both (≤ 1 MB) |
| 2 | `nor read OFF LEN -o FILE` | controller read into the upload area, fetched and CRC-checked |
| 3 | `nor verify OFF FILE` | file uploaded, then controller read and XIP window both compared with it |
| 4 | `nor erase OFF --write` | one 4 KB sector, then a blank check through both paths |
| 5 | `nor erase64 OFF --write` | one 64 KB block, then a blank check |
| 6 | `nor program OFF FILE --write` | up to 64 KB onto blank flash, then verify through both paths |

Write sub-ops carry the magic 0x57524F4E ("NORW"); hu.py adds it only with `--write`.

## On-unit test plan

Read-only first (no owner approval needed beyond running a new build):

1. `hu.py exec NET.BIN`, then `hu.py nor status`. Expect `jedec 204018 XM25QH128C`, SR1 & 0x7C = 0x04, SR2 & 0x40 set, "protected 000000..fbffff", CFG1 00150095, AUTO_CFG[31:24] = 0xEB with [20] set, AUTO_CFG[15:8] 00 or 02, fault 0. A different SR1/SR2 means FlashInit's protect did not take: note it, it does not block the test.
2. `hu.py nor compare 0x0 0x10000`, `hu.py nor compare 0x590000 0x10000` (DSP microcode), `hu.py nor compare 0xE00000 0x10000`: expect "controller = xip". On a mismatch, repeat with `--cmd 0b`; if both mismatch, stop (the read path is wrong, and so would be every verify).
3. `hu.py nor read 0x590000 0x10000 -o m.bin` and compare `m.bin` with the same 64 KB of the original dump (that region has never been written).
4. `hu.py nor compare 0xFBF000 0x1000`: both CRCs should be f154670a, the CRC-32 of 4096 bytes of 0xFF (and of that sector in the original dump).
5. `hu.py ping`, `hu.py audio status`, a UI page change: the unit is unaffected.

One write cycle, on a single 4 KB sector at the top of the free region (needs the owner's OK; no AA session running):

6. `hu.py nor erase 0xFBF000 --write`: expect `ok`, tens of ms, `(BLANK)`. The sector is already blank, but this exercises unprotect, erase and restore.
7. `hu.py nor status`: SR1 and SR2 must be back to the values from step 1, fault 0.
8. `head -c 4096 /dev/urandom > t.bin; hu.py nor program 0xFBF000 t.bin --write`: expect `ok`, a few ms per page, "vs ram: controller MATCH, xip MATCH".
9. `hu.py nor verify 0xFBF000 t.bin` and `hu.py nor compare 0xFBF000 0x1000`: both match. `hu.py nor status`: protection restored.
10. Optional, the page split on hardware: erase again, then program 0x200 bytes at 0xFBF080 (crosses two page boundaries) and verify.
11. `hu.py nor erase 0xFBF000 --write` to leave it blank; power-cycle through the normal boot and check `nor status` and `nor compare 0xFBF000 0x1000` once more (protection as FlashInit sets it, sector still blank).

If anything times out the driver refuses further writes until a reboot; let the unit sit a few seconds (the flash finishes on its own) and reboot. If the protection does not clear (`protection bits did not change`), SRP0 and the WP# pin are the suspects; stop there. Nothing in the free region is read at boot, and the protection bits do not affect reads, so none of these steps can stop the unit from booting. Recovery for anything worse: CH341A and the original dump.

## Open questions

- The controller's SPI clock (CTRL's upper half, TIMING) and whether 0x03 is within its rating at that clock (step 2 answers it).
- The meaning of AUTO_CFG bit 0 and CTRL[28] (the hardware write enable and page split U-Boot relies on).
- Whether the controller's prefetch buffer needs more than one unrelated read to drop old data after a write (step 9 shows it).
- Volatile status writes (0x50) would avoid the non-volatile protection round trip, but U-Boot never uses them on this part.

## On-unit results

- 2026-11-09, build 20261109-121550: `nor status` reads JEDEC 204018 (XM25QH128C), SR1 04 / SR2 42 / SR3 60, protected 000000..fbffff, as expected. Controller reads with 0x03 differ from the XIP window in exactly the first byte of every 1 KB DMA chunk (offsets 0, 0x400, 0x800, 0xC00; checked at 0x000000, 0x3BF000, 0x900000, 0xE00000); with 0x0B fast read all four regions match the XIP window. The driver and `hu.py nor` now default to 0x0B. No erase or program has been run yet.
- 2026-11-09, build 20261109-123123 (after the unbounded-wait fix): erase of the 4 KB sector at 0xFBF000 ok in 34 ms and blank by controller and XIP; protection restored (SR1 04, protected 000000..fbffff); program of 4 KB of random data verified against RAM through the controller and the XIP window, and read back on the PC byte for byte; erased again (blank). The slowest PIO was a 0x05 status read held for 33.7 ms during the erase, which is why the first 10 ms bound timed out. Neighbors 0xFBE000 and 0xFC0000 and samples at 0x0, 0x3BF000 and 0x8A0000 still match the original dump.
- 2026-11-09, build 20261109-124256: `hu.py nor dump` (or Settings > System > Back up) copies the XIP window to NORDUMP.BIN on the card in 8 KB passes of the net loop, then reads the file back and compares CRCs; NORDUMP.TXT records size, JEDEC id, CRC and build. 16 MB in 101 s (~220 KB/s SD writes, ~25 s read-back), HU1 kept answering throughout. Two dumps gave the same CRC (a1226438). It differs from both dump1.bin (ceb2a6c2) and the planned bootcmd image (6095c467), as expected from the boot script and the areas stock eCos writes at runtime; not yet compared region by region.
