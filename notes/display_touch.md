# Display and touch on the SPHE8268K head unit

Status as of 2026-10-31. Code addresses: U-Boot at link address (runtime = link + 0x06DC2FC0), eCos at its load address (0x301040 = start of `ecos.bin`). Disassembly: `uboot-re/uboot.ann.dis`, `isp-re/ecos.ann.dis` in the earlier session's scratchpad (see memory `head-unit-context`).

## Summary

- Panel: 1024x600. The OSD0 layer scans an ARGB8888 frame from DRAM, described by a 128-byte big-endian header that OSD0's `link_addr` register points at. **Demonstrated live:** we built a frame at 0x03000000 and a header at 0x032F0000, set OSD0 `link_addr` and `en`, and the OSD's hardware CRC register followed our pixel data (details below). The owner still needs to confirm what the panel shows, which also settles the byte order.
- Backlight: U-Boot turns it on (GPIO86 high plus PWM0 at duty 0x14). eCos switches it with MCU frame `EE <level>` (`EE FF` = off).
- Touch: **Goodix GT911** (ID "911", firmware 0x1060, config version 0x48, 1070x600 coordinate range), I2C address **0x5D**, bit-banged on GPIO91 (SDA) and GPIO92 (SCL), with RST = GPIO90 and INT = GPIO93. Working from our payload (HU1 op 14, `hu.py touch`), see "Live touch (working)". The **five capacitive front buttons are touch areas on the same panel** to the right of the 1024-px display.
- Front-panel keys: no MCU frames are sent for them (parent session's test). eCos has two 5-key SAR-ADC resistor ladders (ADC slots 0 and 1) and GT9xx touch-key/virtual-key handling. Which one the capacitive buttons use is not yet proven.

## Display pipeline (SP7021-style register groups)

The register layout matches Sunplus SP7021's display block (group n = 0x9C000000 + n*0x80). Names come from eCos's `osd` debug dump at 0x526354, which prints every field.

| Block | Base | Evidence |
|---|---|---|
| OSD0 (G196) | 0x9C006200 (OSDn = +n*0x80, n = 0..2) | eCos dump 0x526354 (`---OSD---`), U-Boot 0xF216EE |
| GPOST/GSCALER0 (G201) | 0x9C006480 (+n*0x80) | eCos dump (`---GSCALER---`), U-Boot 0xF1FA06 |
| TGEN (G213) | 0x9C006A80 | U-Boot 0xF21394.. |
| DMIX (G217) | 0x9C006C80 | U-Boot 0xF21568.. |
| PWM (G244) | 0x9C007A00 | U-Boot 0xF74738 |

### OSD0 registers (0x9C006200)

| Offset | Name | Live value (our boot) | Notes |
|---|---|---|---|
| +0x00 | ctrl | 0x437 | matches SP7021 RGB(1<<10) \| latch(1<<5) \| A32B32(1<<4) \| fifo 7 |
| +0x04 | en | 0 (U-Boot left it off) | U-Boot writes 1 at 0xF216FA; eCos 0x5256D4 |
| +0x08 | link_addr | 0 | DRAM address of the region header, `& 0x3FFFFFFF` (eCos 0x525756) |
| +0x20 | req_ctrl | 0x8020 | |
| +0x38 | crc_chksum | 0 when off; per-frame CRC of fetched data when on | readback proof, see below |
| +0x40/+0x44 | hvld_offset / hvld_width | 0 / 0x400 | eCos 0x525790 writes width (aligned to 4) |
| +0x48/+0x4C | vvld_offset / vvld_height | 0 / 0x258 | |
| +0x50 | data_fetch_ctrl | 0 | |
| +0x54 | bist_ctrl | 0 | bit 7 = pattern-generator enable, bits 6+ = type (eCos 0x5257DC, shell `osd ... pattern`) |

### Region header (128 bytes, big-endian fields)

Built identically by U-Boot `sub_f1fe84` and eCos `update_hardware_header` (0x523E48).

| Bytes | Field |
|---|---|
| 0x00 | color format: 0x0E = ARGB8888 (what U-Boot uses for the logo), 0x02 = 8bpp CLUT (byte 0 also gets bit 7) |
| 0x02 | 0x10 (U-Boot) / 0x00 (eCos) |
| 0x04-05 | height (v_size) |
| 0x06-07 | width rounded up to a multiple of 4 (h_size) |
| 0x08-09 / 0x0A-0B | display start row / column |
| 0x15 | 7 |
| 0x16-17 | stride in pixels |
| 0x18-1B | next header, 0xFFFFFFE0 = end of list (written as the LE word 0xE0FFFFFF) |
| 0x1C-1F | pixel data address |

`tools/fb.py` builds this header.

### Timing and mixing (live reads)

- TGEN: htotal 0x540 (1344), hactive 0x400 (1024), vtotal 0x27B (635), active lines 0x1E..0x277 (600). That's 1344x635 at 60 Hz, about a 51.2 MHz pixel clock.
- Resolution struct in low RAM at 0x400: 0x0400 x 0x0258 (U-Boot 0xF20D4E.., default 1024x600 when the panel bin is invalid).
- DMIX 0x9C006C80 = 0x3070, 0x9C006C88 = 0x6254: BG = 7 (pattern gen), L1 = 0 (VPP0), L2 = 3 (OSD0), L3/L4 = 4/5. Layer mode 0x9C006C84 = 0x8551, where L2's 2-bit mode is 0 (blend, uses per-pixel alpha), and U-Boot clears it at 0xF2166E. So OSD0 pixels need alpha 0xFF to be opaque.

### Why the screen was blank in our boot

U-Boot's logo routine 0xF2089E runs from main_loop (0xF0BB36) before bootcmd. It loads the logo entry from the flash table at 0x98018020. That entry (flash 0xB39140) is literally the text `NULL`, so U-Boot prints "logo bin is null" (0xF20F66), sets r13 = -1, and skips the OSD0 `link_addr`/`en` writes at 0xF216EE. Panel and TGEN init still run, so the panel is driven, with no layer enabled.

### Live demonstration (2026-10-31)

`tools/fb.py pattern` writes the 1024x600x4 frame to 0x03000000 and the header to 0x032F0000, flushes the D-cache (hu.py OP_SYNC), and then makes U-Boot's own two writes: `mw.l 0x9c006208 0x032f0000; mw.l 0x9c006204 1`.

OSD0 crc_chksum (0x9C006238) is the hardware proof that OSD0 scans our buffer:

| Frame | CRC |
|---|---|
| OSD0 disabled | 0x0000 |
| test pattern | 0x0978 (same on two boots) |
| solid blue | 0x190D |
| solid red | 0x4D6A |
| en=0, then en=1 again | 0x0000, then 0x0978 |

What the test pattern contains: an 8-px white border. Top half: four bars whose bytes 0 and 3 are both 0xFF, so they are opaque whatever the alpha byte is: memory bytes `FF 00 00 FF`, `FF FF 00 FF`, `FF 00 FF FF`, `FF 80 80 FF`. Bottom half: little-endian ARGB words 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF (intended red, green, blue, white). If the hardware reads ARGB from little-endian words, the top bars are blue, cyan, magenta, lavender and the bottom bars are red, green, blue, white. If it reads big-endian ARGB, the top bars are blue, magenta, cyan, lavender and the bottom red/green bars go transparent (black). **Owner: report the colors left to right, top and bottom.** The pattern is on screen now.

### Backlight

- U-Boot `sub_f73628` (called from main_loop right after the logo routine) does three things: it drives GPIO 0x56 (86) high (OE/OUT 0x9C000334/354 bit 6 = 1 live; GPIO_FIRST 0x9C0032EC bit 22 = 1), it sets the DISP_PWM0 pinmux, and it calls `sub_f74738(0, en=1, duty=0x14, bit8=1)`.
- PWM block 0x9C007A00: +0x04 = per-channel enable bits [3:0] plus bits [11:8], +0x08/+0x0C/+0x10/+0x14 = duty of PWM0..3. Live: 0x9C007A04 = 0xF01, 0x9C007A08 = 0x14.
- eCos `tft_backlight_on/off` (0x47306E / 0x473038) send the MCU frame `EE <level>` through 0x31D928 (off = `EE FF`). The MCU may gate the backlight supply. I did not send these frames because the MCU link belongs to the parent session.

### White haze after an abrupt power cut (2026-10-31)

On the first power-up after the payload hung (frozen console frame for ~15 min) and the owner cut power, the whole screen had a uniform white haze. With OSD0 disabled (mixer black) the haze stayed and faint console text was visible. About 15 minutes later it was gone on its own. DMIX and every console color were checked: opaque and unchanged, so not our pixels.

Not long-on retention: the owner says the unit runs for hours in normal use without this, and the haze was not a ghost image. Working theory: an LCD panel needs a power-off discharge (gate-on/XAO) to drain pixel charge, which the stock shutdown presumably triggers; an abrupt cut after a hang skips it, and the leftover charge shows as haze plus traces of the last frame until it leaks off. Check next time a hang is power-cycled; if it recurs, look for the stock shutdown sequence (MCU `EE FF` backlight off, panel power GPIO) and do the same before any deliberate reset.

eCos disassembly search (Haiku, unverified detail): no boot-time panel bias/VCOM PWM (PWM writes 0x52E8D2.. come only from the `pwm` shell command), `factorycfg.tft_panel_electricity_level` only printed (f_0055D9F4), `[HAL][Panel]` init f_0052D870 is pinmux only. Not checked: 0x52D216's writes at 0x9C0072D0..F0 and the 0x9C007880 block.

An idle screen blank was tried and removed: the owner does not want the display (nav) going dark.

### Drawing from our own code

Pick a frame address (64-byte aligned, outside the payload's areas), write ARGB pixels, flush the D-cache over the frame (`cctl L1D_VA_WB` per line, as in `plat_sync`), build the header, then set `*(u32*)0x9C006208 = hdr` and `*(u32*)0x9C006204 = 1`. For tear-free updates, keep two frames and two headers and flip `link_addr`. OSD0 latches per frame (ctrl bit 5). Next: test whether changing `link_addr` alone flips the buffer, and use bist_ctrl (+0x54) as a RAM-free test.

## Touch controller: Goodix GT9xx

eCos driver block 0x5161D2..0x516F4E, all software I2C on GPIOs:

- Pins: init 0x516E5A maps sysio indices through the table at 0x6C7A64 (8-byte entries `[idx, gpio, pullup, pulldown, ?, first, oe, out]`). SDA = sysio 0x2C → GPIO91. SCL = 0x2D → GPIO92. INT = 0x2E → GPIO93. RST = 0x2B → GPIO90. The GPIO numbers are stored at 0x6D7C54/4C/58/50.
- GPIO registers (helpers 0x520394/0x5203BC/0x520414, mode bit 0x5204C0). OE = 0x9C000300 + (8 + pin/16)*4 and OUT = 0x9C000300 + (0x10 + pin/16)*4; both use masked writes (bits 31:16 are the write mask, bits 15:0 the value, bit = pin%16). IN = 0x9C000300 + (0x18 + pin/32)*4, bit = pin%32. GPIO_FIRST = 0x9C0032E4 + (pin/32)*4, a plain read-modify-write register. For pins 90..93 that gives OE 0x9C000334 bits 10..13, OUT 0x9C000354, IN 0x9C000368 bits 26..29, FIRST 0x9C0032EC bits 26..29.
- Live idle state under U-Boot: IN 0x9C000368 = 0x1F800000, so RST, SDA and SCL read high and INT reads low. FIRST 0x9C0032EC = 0x004003FF, so pins 90..93 are *not* GPIO-owned under U-Boot. eCos `init_gpio_config` (0x5205C2) sets FIRST for every sysio entry.
- I2C address: the device byte is 0xBA/0xBB, so the 7-bit address is 0x5D. Write is 0x5166BC, read is 0x516990 (16-bit register address, MSB first, repeated start, ACK every byte except NACK on the last, 3 retries).
- Reset (0x516AE4): RST low, wait 1 ms; INT low, wait 10 ms; RST high (0x516210), wait 50 ms; INT becomes an input. INT low at the RST rising edge selects 0x5D (Goodix datasheet).
- ID (0x516B5A): read 6 bytes at 0x8140 and print `IC VERSION: %c%c%c_%02x%02x` (product ID such as "911" plus a firmware word).
- Config (0x516BD8): copy 0xBA (186) bytes from the active TP-type struct (0x48D290 selects it per the "TP type" setup item: TP_COMMON, TP_9010, TP_HLX_9, TP_7701, ...) and write it to the chip. That is the GT9xx config block 0x8047..0x8100. It's called from 0x31E0EC, the MCU memory/boot event handler.
- Poll (0x516C74):
  1. Read 2 bytes at **0x814E**; `n = status & 0x0F`.
  2. If 1 <= n <= 5, read `n*8` bytes at **0x8150**. Each 8-byte record is `[track_id, xL, xH, yL, yH, sizeL, sizeH, rsvd]`; a record with track id 0x20 is treated specially.
  3. Write 0x00 to 0x814E to acknowledge.

  Then 0x48C700 calibrates: `x = (raw_x - xo)*100/xc`, `y = (raw_y - yo)*100/yc`, with optional axis swap and 1024-x / 600-y flips. The calibration lives in `/test/tp.bin` (jffs2 `/dev/rom/fis/test`) and is shown by `bIsCalibrated/xo/yo/xc/yc` prints at 0x48C24C.
- Not the hardware I2C master. eCos's I2CM0/I2CM1 driver (IRQ 9 / 0x7B, API 0x51500E/0x5151DE) is used only by the FM tuner drivers (QN8035, TEF668x, SI470x, SI475x).

### Live touch (working, 2026-10-31, build 20261031-182509)

`isp/sd-10-net/gt9.c` drives the bus open-drain from the payload (HU1 op 14; `hu.py touch pins|probe|id|read|poll|watch|reset|release`). No reset was needed: with RST high as U-Boot leaves it, `touch probe` got an ACK at 0x5D (nack at 0x14) and the ID read gave `'911' fw 1060 x 1070 y 600 vendor 03`. The config block (0x8047, 186 bytes) reads version 0x48, X max 0x042E (1070), Y max 0x0258 (600), 5 touches, module switch 0x0D; touch-key registers 0x8093..0x8096 are 0. We never write the config.

Polling (status 0x814E, records from 0x814F, 8 bytes `[id, xL, xH, yL, yH, sizeL, sizeH, rsvd]`, then write 0 to 0x814E) works. Raw coordinates are already in display orientation, no swap or flip needed. Corners as touched by the owner: top-left (45, 12), top-right (979, 48), bottom-left (56, 540), bottom-right (979, 587), center (553, 305).

The front buttons (vertical row, pressed top to bottom) report as ordinary touches at x 1054..1061 with y = **141, 211, 322, 404, 508**. So they are the panel's area past the 1024-px display (X range 1070); the status key bit stays 0. A press is `x >= 1030`, button = nearest of those y values.

The earlier reboot with `tools/gt9.py` was not these pins. The payload's log formatter was unbounded, and the ~900-character UDP command packets that tool sent overflowed `fmtbuf` and overwrote `reloc`, so the next U-Boot call jumped to garbage. Reproduced with RAM-only `mw.l` packets and fixed in build 20261031-182509 (bounded `vfmt`).

## Front-panel keys

- MCU→SoC frames (dispatcher 0x31E704): 0xA0..0xAD and 0xD1 go to system handlers (0x31E478), 0xD9 to CAN-bus (0x321898). Any other first byte is a **key code**, posted as GUI event 0xA000|code through 0x32ADB2 (0x31E510). Codes 0x0A, 0x0B, 0x18 and 0x19 are remapped (to 0x72, 0x73, 0x97, 0x98). The parent saw no frames for the capacitive buttons, so they don't come through here.
- SAR ADC (G94, 0x9C002F00), driver 0x51F238..0x52029A:
  - Registers: +0x04 bit 8 = enable, +0x08 \|= 0x30, +0x2C bits [5:3] = channel and bit 2 = start, +0x34 = data in bits [11:0] (eCos keeps `>>4`, 8 bits).
  - Sampling: a 6-slot round-robin, slot → (channel, pad mode) from flash 0x9889D580: (0,0), (1,1), (2,2), (2,3), (3,4), (3,5). Median of 3 samples per slot.
  - Slots 0 and 1 are 5-key ladders. Values below 0xF7 count as pressed; tables are from the eCos data at 0x6C79E4 / 0x6C7994 and may be overridden by `/test/key.bin`:
    - ch0: 11-48 → 0x37, 49-101 → 0x07, 102-144 → 0x08, 145-189 → 0x1A, 190-231 → 0x0A
    - ch1: 11-48 → 0x10, 49-101 → 0x11, 102-144 → 0x12, 145-189 → 0x14, 190-231 → 0x13
  - Slots 2..5 are steering-wheel-key learning (0x520012): two ADC lines in two pull modes each, matched against 12 learned entries.
  - No U-Boot code touches G94, and I have **not** read it live: its clock state under U-Boot is unknown, and a hung read needs a manual power cycle.
- GT9xx: the status register's key bit and eCos's "touch do virtaul key region" (0x3820E4) mean touch keys are possible on the touch glass.

**Answered:** the capacitive buttons are the GT911 virtual-key area (see "Live touch"). The SAR ADC ladders are presumably for other unit variants or wired keys; not needed.

## Next steps

1. Owner: report the test-pattern colors. That fixes the ARGB byte order for `fb.py` and for our own drawing code.
2. Add a framebuffer and console text renderer to the payload, using the OSD0 path above (`fb.py` shows every step).
3. ~~Touch~~: done, see "Live touch". Next is an interrupt- or tick-driven poll in the payload that turns touches and button presses into events.
4. ~~Keys~~: the front buttons are touch areas; the ADC is not needed for them.
