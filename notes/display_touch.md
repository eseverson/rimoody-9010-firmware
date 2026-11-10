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
- eCos `tft_backlight_on/off` (0x47306E / 0x473038) send the MCU frame `EE <level>` through 0x31D928 (off = `EE FF`). The PWM0 duty at 0x9C007A08 has no visible effect; the MCU owns the backlight.
- **`EE <level>` verified on the unit (2026-11-02, camera with fixed exposure):** the level is an inverted duty. 0x00 is brightest; it dims roughly linearly through 0xC0, 0xE1 is dim but lit, and 0xF0..0xFF is off. The level is `table[idx]`, where the table is 33 bytes at flash 0x9806BF9C (`e1 dc d7 d2 cd c8 c3 be b9 b2 ab a4 9d 96 8f 88 81 7a 74 6e 68 62 5c 56 50 46 3c 32 28 1e 14 0a 00`) and idx is the user brightness 0..32 (config +0x10, getter 0x32136C, setter 0x321390).
- **Headlight (illumination) input: GPIO26, active low** (eCos sysio 0x6B, pin-table entry `6b 1a 00 00 01 01 00 02`: an input). Under U-Boot it reads from IN 0x9C000360 bit 26 without setting GPIO_FIRST; with nothing connected it reads 1 (lights off). eCos samples it in 0x320998, and 0x320A82 debounces it (two equal samples in a row, then a 5-poll holdoff; struct 0x71E5CA). A change calls 0x3213DC(night): night halves the brightness index, day doubles it (capped at 32). The same handler can drive GPIO38, but on this image it never follows the lights; see "Front-button lights (key lights)" below.
- Payload (build 20261102-012033): the Settings › Display day and headlight sliders (percent → eCos table) send `EE`, the UI debounces GPIO26 every 200 ms, Auto theme goes dark with the lights, and Android Auto gets the night-mode sensor (SensorBatch field 10). `hu.py ui lights 0|1|2` forces the input off/on or releases it. **Still to check in the car:** that GPIO26 reads 0 with the lights on. If it stays at 1, set GPIO_FIRST 0x9C0032E4 bit 26 as eCos does.

### Installer boot: no panel init

On a normal power-on, main_loop runs the logo routine 0xF2089E and then `sub_f73628` before bootcmd. The SD-card installer's boot never gets there: xboot starts U-Boot in ISP mode (boot mode word 0x9E839408 = 0x41, 0x43, 0x45 or 0xFF), and main_loop runs `ispsp` at 0xF0BB28, which sources our script and jumps into the firmware without returning. Clocks, TCON, TGEN, mixer, GPOST and panel pins stay at their reset values: the MCU's backlight is on, there is no picture. Calling the routine from the payload would not help: with the boot mode still ISP it reads the panel bin from the update file in RAM, and our installer file carries only xboot and U-Boot.

What the routine does with this unit's flash (logo entry `NULL`; panel bin at flash 0xB38C20 through partition entry 1 at 0x98018028: 155 entries, one display, panel type 1), in order:

1. Fields 0 and 1 of block 0x9C1C7000 (`sub_f74d1c`/`sub_f74d68`): `+0x80 = 6 << 16 | 0x53` and `+0x88 = 3 << 16 | 0x50`, each latched with bit 20.
2. Register lists through an interpreter loop (0xF20FEC) that takes the entry kind from address bits [31:21]: plain write (followed by a short delay), OR into the register (0x9C4xxxxx), delay (0x002xxxxx), poll until bits set (0x000xxxxx) or clear (0x010xxxxx), gamma byte (0x020xxxxx), skip (0x008xxxxx). The lists: U-Boot's pinmux word 0x9C000208, its mixer defaults, its OSD0..2 and GPOST0..2 defaults, then the panel bin (MOON clocks, pinmux 0x9C000204..234, TCON 0x9C007880..9D4, PWM, output mapping 0x9C008000..02C, TGEN timing, TGEN reset 0x9C006A84 = 1).
3. `sub_f1ff8a`, panel pins: GPIO 54..63 back to their pinmux function, GPIO 64..73, 9 and 10 driven low.
4. Gamma table, only if TCON 0x9C007900 bit 0 is set (not on this panel).
5. GPOST0..2 at scale 1.0 and the OSD valid windows (`sub_f1fa06`), mixer layer order, TGEN adjust fields, TGEN timing copied to the second set at 0x9C006B90 (`sub_f1f84e`).
6. `sub_f73628`: GPIO86 high, DISP_PWM0 pinmux 0x9C000090 = 0x000F0001, PWM0 enable with duty 0x14.

What the firmware does now (`firmware/disp.c`, called from `payload_entry` right after the "net main" line, before the MCU link, the decoder and the UI): it reads and checks the panel bin through the flash window, logs the timing generator's state, and on the installer boot, or on any boot whose TGEN lacks the panel's timing, repeats steps 1 to 6 itself. No U-Boot code runs. The check compares TGEN 0x9C006AA0..AB0 (htotal 0x540, hactive 0x400, vtotal 0x27B, lines 0x1E..0x277) with the panel bin, and the log also says whether the line counter 0x9C006AB4 advances. A normal boot logs `disp: TGEN ... panel timing set` and writes nothing. Run on the host against the flash dump, the sequence leaves the live normal-boot values (OSD0 ctrl 0x437 and its 0x400 x 0x258 window, mixer 0x3070 / 0x8551 / 0x6254, PWM 0xF01 / 0x14). Not yet confirmed on the unit: look for `disp: panel init (155 entries, installer boot) ... ok` in the installer boot's log, and a picture.

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

`firmware/gt9.c` drives the bus open-drain from the payload (HU1 op 14; `hu.py touch pins|probe|id|read|poll|watch|reset|release`). No reset was needed: with RST high as U-Boot leaves it, `touch probe` got an ACK at 0x5D (nack at 0x14) and the ID read gave `'911' fw 1060 x 1070 y 600 vendor 03`. The config block (0x8047, 186 bytes) reads version 0x48, X max 0x042E (1070), Y max 0x0258 (600), 5 touches, module switch 0x0D; touch-key registers 0x8093..0x8096 are 0. We never write the config.

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

## Front-button lights (key lights)

Static analysis of the stock eCos image (2026-11-09), nothing run on the unit. Summary: stock lights the buttons with **SoC GPIOs, not MCU frames, PWM, or the GT911**. GPIO20, GPIO21 and GPIO22 are on/off color lines (seven fixed colors plus a color cycle, picked on a Settings page), and GPIO38 is held high whenever the unit is on. The headlight input does **not** change the button lights on this image: the factory switch that would make GPIO38 follow the headlights is missing from its menus. That these pins really reach the bezel LEDs is inferred (the eCos build is generic across boards), so the owner checks below decide it.

### What stock does

- **Pins** (sysio pin table 0x6C7A64, entries `[sysio, gpio, pu, pd, M, F, OE, OUT]`): sysio 0x6C → GPIO20 `6c 14 00 00 01 01 01 00`, 0x6D → GPIO21 `6d 15 ...`, 0x6E → GPIO22 `6e 16 ...`, 0x37 → GPIO38 `37 26 00 00 01 01 01 00`. All four are GPIO-owned outputs, low at boot. `init_gpio_config` (0x5205C2) applies each entry in the order OUT (0x5203BC), OE (0x520414), M (0x52046C, masked register 0x9C000300 + 4·(pin/16)), FIRST (0x5204C0, read-modify-write 0x9C0032E4 + 4·(pin/32)).
- **Key-light engine** (0x32047E resolves the three pins; 0x320506, called from the MCU thread's setup 0x31EC68, drives all three low). The command entry is 0x31D804(v), which builds a 2-byte command and passes it to **0x320660, a local function (not the MCU sender 0x31D928)**:
  - v 1..7 → `02 <v-1>`: static color v-1 (0x3205AC).
  - v 8 → `03 02`: cycle through the seven colors, one step every 2·20 = 40 passes of the MCU receive loop 0x31E948.
  - v 0xFF → `04 00`: off. The enable flag 0x6DCE4C is cleared and GPIO20/21/22 are driven low.
- **Output**: on every pass of the MCU receive loop, 0x3207D8 writes each pin from its channel byte (0x71E57E → GPIO20, 0x71E57F → GPIO21, 0x71E580 → GPIO22): 1 if the byte is nonzero, else 0. The bytes only ever take 0 or 0x3F, so this is on/off with **no dimming**.
- **Color table** (0x3205AC, index → GPIO20/21/22): 0 → 1 0 0, 1 → 0 1 0, 2 → 1 1 0, 3 → 0 0 1, 4 → 1 0 1, 5 → 0 1 1, 6 → 1 1 1. If the lines are R, G, B (inferred), that is red, green, yellow, blue, magenta, cyan, white.
- **Setting**: Settings › System › "Key Lights" (string id 0x1FA; System menu table entry at 0x6BDC6E `fa 01 00 10 ff 00`, a page link) opens `keylightview` (0x38326E, handler 0x382EA4): eight buttons in a 4×2 grid. Button k (0..7) stores k in config byte 0x6DCE28+0x15 and calls 0x31D804(k+1), so k = 7 is the cycle. A second page handler (0x33DAE6) does the same. No `setupcfg`/`factorycfg` field is printed for it; it lives in the system config block that `/test/usercfg.bin` restores, which we have not decoded.
- **power_on** (0x47AF0A): GPIO38 high (see below), 30 ms, then 0x31D804(config+0x15 + 1), so the saved color comes back. **power_off** (0x47ACBC): 0x31D804(0xFF) (keys off), 20 ms, then GPIO38 low. The MCU shutdown frame `00` (handler 0x31E510) also drives GPIO38 low.
- **GPIO38 and the headlights.** The lights-change handler 0x320A82 and the init 0x320BD4 compute GPIO38 = `!detect || lights`: with the lights on it goes high (if powered, 0x71E59F); with them off it goes low only if `detect` is set. `detect` is 0x6DCE67, written only by 0x320F72 from config+1 bit 3, and only if setup item 0x2FD exists in the factory Option menu (0x34F51A searches the table at 0x6BE50C). String 0x2FD is **"Headlight detection"**, and that item is in neither the Option table nor the System table of this image (no 6-byte menu entry with id 0x2FD anywhere in the data). So `detect` = 0, and GPIO38 is high whenever the unit is on, whatever the headlights do. The `00 10 ff` System entries and the factory Option list are static data; no code was found that adds items to them at runtime.
- **Everything else the lights change does** (0x320A82 → 0x3213DC): TFT backlight only (index halved or doubled, `EE table[idx]` via 0x321390 → 0x472FDC → 0x31D928), GUI message 0x68, and the phone-projection night flag (0x3926B4 in `android_auto_mw.c` for sources 0x0A/0x0B, 0x395BD6 for sources 4/5). Nothing for the keys.
- **Ruled out:**
  - MCU frames: none in the key-light path, and none of the stock boot frames (see notes/android_auto.md, "the boot frames") is a key-light command.
  - PWM: the only other PWM user near this code is 0x3203F6 (PWM1, duty 0x80, period 0x190, a few ticks), called from the key handlers and gated by the Beep setting (0x6D7108): the key beep.
  - GT911: only the config block (0x8047) and the status ack (0x814E) are written; its touch-key registers are 0 and the GT911 has no LED outputs.
  - I2C: the hardware I2C masters serve only the tuner drivers.
  - LED driver chips: no LED-driver strings in the image.

### Hardware or MCU paths

Two other possibilities stay open: the button LEDs could be wired to the illumination line, or the AC6965A could drive them by itself. The firmware does not settle either. Against them: stock has a working color page and drives these pins at power-on and power-off, which only makes sense if the LEDs hang off the SoC on at least some boards. The owner's checks below decide it.

### What our firmware should do

Masked registers (bits 31:16 = write mask, 15:0 = value). For GPIO20..22 the bits are 4..6 of the second 16-pin group; for GPIO38 it is bit 6 of the third.

| Pin | M | OE | OUT on / off | FIRST (RMW) |
|---|---|---|---|---|
| GPIO20..22 | 0x9C000304 ← 0x00700070 | 0x9C000324 ← 0x00700070 | 0x9C000344 ← `0x0070xxxx`, value bits per color (white 0x00700070, off 0x00700000) | 0x9C0032E4 \|= 0x00700000 |
| GPIO38 | 0x9C000308 ← 0x00400040 | 0x9C000328 ← 0x00400040 | 0x9C000348 ← 0x00400040 / 0x00400000 | 0x9C0032E8 \|= 0x00000040 |

1. At startup, in the eCos order (OUT, then OE, M, FIRST): GPIO38 high, then GPIO20/21/22 to a color. Single colors: GPIO20 `0x00700010`, GPIO21 `0x00700020`, GPIO22 `0x00700040`. Confidence: register layout proven (same helpers as GPIO13 and the GT911 pins); that it lights the buttons inferred, medium. Driving GPIO38 high matches what stock does whenever it is on, so it is the safe direction even if GPIO38 is something other than the LED supply.
2. Before a deliberate reset or power-down: GPIO20/21/22 low, then GPIO38 low (stock power_off order).
3. Headlights: stock does nothing to the keys. Optional, beyond stock: offer "Headlight detection" ourselves (GPIO38 = lights) or switch to a dimmer color at night. Real dimming needs software PWM on these GPIOs; untested.
4. No MCU frames are needed.

### Owner checks (stock boot, no SD card)

1. Daytime, headlights off: are the buttons lit, and in what color? Stock predicts lit.
2. Settings › System › Key Lights: does the page exist, and does a color change the buttons? Note which button is selected; that is the color to copy (first row is presumably red, green, yellow, blue; second row magenta, cyan, white, cycle).
3. Headlights on: do the buttons change at all? Stock code predicts no change. If they brighten, dim or switch on, the LEDs are on the illumination line in hardware (or the MCU), not under eCos control.
4. Under our firmware today: are the buttons dark? A read-only peek of 0x9C000304/324/344, 0x9C000308/328/348 and 0x9C0032E4/E8 shows what U-Boot leaves on these pins.

## Next steps

1. Owner: report the test-pattern colors. That fixes the ARGB byte order for `fb.py` and for our own drawing code.
2. Add a framebuffer and console text renderer to the payload, using the OSD0 path above (`fb.py` shows every step).
3. ~~Touch~~: done, see "Live touch". Next is an interrupt- or tick-driven poll in the payload that turns touches and button presses into events.
4. ~~Keys~~: the front buttons are touch areas; the ADC is not needed for them.

## Second haze event (2026-11-01)
After the ~4 h dead period that followed the SD test build (SoC hung or powered off by the MCU, 2026-10-31 ~21:00 until the owner's power cycle after 01:00), a bigger and thicker white haze appeared around the screen border and stayed through another >10 s power cycle. Both haze events followed a dead/hung SoC; never normal use or long on-time. Working theory: with the SoC dead (no valid frames) the panel's own rails keep a DC voltage on the liquid crystal, and ionic impurities migrate toward the seal at the edges (border haze). Expected to fade over hours of normal AC drive. Prevention: the hardware watchdog (notes/rtos.md "Lockups") now resets a dead SoC within ~7 s. Discriminating check: boot stock eCos (card out); same haze there = stored in the panel, not our drive.

On the unit 2026-11-09 (build 20261109-181333): GPIO38 high plus GPIO20/21/22 lights the bezel button icons; the webcam shows GPIO20 alone red, GPIO21 green, GPIO22 blue, all three white. The firmware drives them from Settings > Display > "Button lights" (Off, On, Headlights: only while the headlight input is on) and "Button light color" (stock's seven colors), saved as `keylight=` / `keycolor=` in UI.TXT, default On and white; they go off (colors, then GPIO38) on the MCU's shutdown request.
