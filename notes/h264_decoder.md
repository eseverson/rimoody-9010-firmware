<!-- Static analysis of the stock eCos H.264 path (2026-10-31, agent trace; addresses are eCos load addresses). Nothing here has been run on the unit yet. -->
# SPHE8268K hardware H.264 decoder (MFD), reverse-engineered from stock eCos

Status 2026-10-31. Static analysis only, nothing run on the unit. All code addresses are eCos load addresses (0x301040 = start of `ecos.bin`) and refer to `isp-re/ecos.ann.dis` in the earlier session's scratchpad. "Inference" marks conclusions not read directly from code; "open" marks what I could not resolve.

String annotations in `ecos.ann.dis` are **not** shifted by +0x40 (unlike the U-Boot disassembly): `"mfd_if_reset"` at 0x671c8c is referenced from inside `mfd_if_reset` (0x430d5c), and `strings.addr` lists that xref.

Most MFD accesses use a base register (`sethi $rX, #0x9c00c` then `[$rX + #off]`), which the original annotation does not resolve. A grep for `0x9c00d...` misses them, so read the code or track the base register (a throwaway tracker script was used for this pass and then deleted).

## 0. Architecture in one paragraph

The MFD ("multi-format decoder", codecs MPEG-2/4, VC-1, RV, H.264) is a fixed-function decoder with a register file at **0x9C00C000..0x9C00FFFF** (register *n* = 0x9C00C000 + 4n). Software (a JM-reference-style parser) parses NAL headers, SPS, PPS and slice headers itself, but reads the bits through a **hardware bit reader** (0x9C00D400..) that is fed by a **bitstream prefetch DMA ("DPFU")** from a DRAM ring (the "EVB"). For each slice, software appends **register-write commands** to a DRAM ring (the "SPMP" buffer); at picture time it kicks the SPMP engine, which loads those registers (including the slice-data start position in the EVB) and the hardware entropy-decodes and reconstructs the slice. There is no evidence that the SPMP is a programmable core or needs microcode: its input is a plain list of register writes (section 3). The MFD addresses DRAM through a 256-entry **index table ("BG2D", 0x9C00A400)**: the decoder is given small buffer indices (0..0x3C), and the table maps each index to a physical address and a layout ("scan type"). An optional **start-code scanner ("SCS", 0x9C00D640..D72C)** copies caller data into the EVB and stops at start codes.

| Block | Registers | Role | Evidence |
|---|---|---|---|
| MFD core/status | 0x9C00D000..D0B4, DC80..DD00 | control, status, picture end | 0x41325e, 0x4312a2, 0x4147c0 |
| Slice/param regs | 0x9C00D004..D0A4, D180..D1AC, D800..D808 | targets of SPMP commands | 0x43a602 (command IDs) |
| Bit reader | 0x9C00D400 + 4n (read n bits), 0x9C00D480 + 4n (peek n bits) | header parsing | 0x441d84, 0x441da6, 0x43c608 |
| SPMP (command ring) + SPAR (pred-weight parser) | 0x9C00D50C..D570 | register-list engine | 0x4145ac, 0x43a3ca, 0x434a3e |
| SCS start-code scanner | 0x9C00D640..D72C | copy input → EVB, find start codes | 0x4342a0, 0x434958, 0x41f678 |
| DPFU1 | 0x9C00D884..D8A4 | bitstream prefetch for bit reader / slice data | 0x4340cc, 0x43e2cc |
| DPFU2 | 0x9C00D904..D924 | second prefetch unit | 0x41325e, 0x43b352 |
| misc / arbiter-like | 0x9C00DB00..DB7C, DE6C, DF18, DF68, EC4C, EC80..ECA8, EF00..EF44 | init values only | 0x434aac, 0x4135ea |
| BG2D index table | 0x9C00A380..A3B8, 0x9C00A400..A7FC | buffer index → DRAM address/layout | 0x52ec48, 0x52ed0a..0x52edec |
| MOON0 clock/reset | 0x9C000004.. (clk), 0x9C00002C.. (gclk), 0x9C000054.. (reset) | | 0x3131b2, 0x31305a |

## 1. One-time init

### 1.1 Clocks and resets (MOON0, SP7021 layout)

Helpers (all masked writes: bits 31:16 = write mask, bits 15:0 = value; ID → word `id>>4`, bit `id&15`):

| Helper | Register | Action | Address |
|---|---|---|---|
| `0x3131b2(n, id...)` | 0x9C000004 + 4·(id>>4) | clock **enable** (writes `bits | bits<<16`) | 0x313212..0x313234 |
| `0x31325c(n, id...)` | same | clock disable (`bits<<16`) | 0x3132bc.. |
| `0x312fa6(id)` | same | clock disable, single ID | 0x312fbe |
| `0x313000(id)` | 0x9C00002C + 4·(id>>4) | set bit (gated clock enable, inference from SP7021 naming) | 0x313018 |
| `0x31305a(n, id...)` | 0x9C000054 + 4·(id>>4) | reset **assert** (`bits | bits<<16`) | 0x3130ba.. |
| `0x313106(n, id...)` | same | reset **deassert** (`bits<<16`) | 0x313166.. |
| `0x312ef2(id)` / `0x312f4c(id)` | same | single-ID reset assert / deassert | 0x312f0a / 0x312f64 |

MFD hardware reset `0x432dfa`: assert reset for the 12 IDs **0x68, 0x69, 0x6F, 0x71, 0x70, 0x6C, 0x6B, 0x64, 0x66, 0x1F, 0x63, 0x67**, 21 `nop`s, deassert the same 12 (0x432e00..0x432ede). The shell command `codec reset` does the same plus ID 0x21 (0x317b5a..0x317c60). As registers:

- 0x9C000058 bit 15 (ID 0x1F)
- 0x9C00006C bits 3, 4, 6, 7, 8, 9, 11, 12, 15 (IDs 0x63, 0x64, 0x66, 0x67, 0x68, 0x69, 0x6B, 0x6C, 0x6F)
- 0x9C000070 bits 0, 1 (IDs 0x70, 0x71)

So: assert = `*(0x9C000058) = 0x80008000; *(0x9C00006C) = 0x9BD89BD8; *(0x9C000070) = 0x00030003`; deassert = the same with the low halves 0 (`0x80000000`, `0x9BD80000`, `0x00030000`).

`0x432dfa` is called from `mfd_deinit` (0x433582), from the MFD reset path 0x43358c (via 0x433660) and from the error recovery 0x432f4c. The H.264 init path (`mfd_if_init` → `mfd_init` 0x432fe0 → `h264_create` 0x436ae0) does **not** call it and does **not** touch any clock register.

Clocks: the only MFD-side clock enables are in the VC-1 trigger (0x42d6e6: enable IDs 0x5A, 0x66; 0x42d6fc: disable 0x71; 0x42d710..: reset pulse 0x67) and the RV trigger (0x422e7a: enable 0x5A, 0x66, 0x69, 0x71; disable 0x64). The H.264 path enables nothing, so under eCos the clocks it needs are already on (from boot or from another codec). **Open:** which clock IDs H.264 needs and whether U-Boot leaves them on. Inference: the union of the IDs above. Clock enable IDs 0x5A → 0x9C000018 bit 10; 0x66/0x69 → 0x9C00001C bits 6/9; 0x64 → 0x9C00001C bit 4; 0x71 → 0x9C000020 bit 1. Bring-up should read 0x9C000004..0x9C000024 and 0x9C000054..0x9C000074 first (section 6).

### 1.2 Interrupt

MFD interrupt = controller source **0xA2 (162)** (MJPEG uses 0xA4): `0x608176(0xA2, ...)` configure, `GL_IntrAttach` with the object at 0x6D750C, enable (0x431888..0x431914). The ISR `0x43157c` just masks, acknowledges and unmasks source 0xA2 and returns 3 (call DSR). The H.264 path itself **polls** (SPMP done, picture end); the interrupt only feeds the generic decode-result state machine. A bare-metal driver can poll and leave source 162 masked.

### 1.3 BG2D index table init (display side)

`0x52ec48` (called once from display init 0x50d8b0 → 0x52eb64; "Initializing 2dbg" error string at 0x50d8bc):

1. `0x9C00A39C &= ~0x20`
2. `0x9C00A384..0x9C00A398` = words from 0x6C7EFC: 0x83828100, 0x05048584, 0x00808604, 0x82818000, 0x05858483, 0x00208604
3. `0x9C00A3A0..0x9C00A3B4` = words from 0x6C7EE4: 0x83828100, 0x06050484, 0x00808504, 0x82818000, 0x06058483, 0x00208504
4. `0x9C00A3B8 = 0x0A000A00`
5. `0x9C00A400 + 4i = 0x07000000` for i = 0..255
6. `0x9C00A39C |= 0x2000`, then `|= 0x10`

The 0x9C00A384..A3B4 words look like bit/byte swizzle tables (inference: address-to-tile mapping). Their meaning is open; copy them verbatim.

Table entry format (one word per index at 0x9C00A400 + 4·idx):

| Bits | Field | Writer |
|---|---|---|
| 18:0 | physical address >> 10 (1 KB units) | 0x52edc6 (`entry = (entry & ~0x7FFFF) | (addr<<3 >> 13)`) |
| 20:19 | "gp" (2 bits) | 0x52ed0a |
| 21..26 | "scan type": type 0 → b21=0, b22=1, b23=0, b24=1, b26:25=2; type ≠0 → b21..23=0, b24=1, b26:25=3 | 0x52ed32 |

`0x9C00A380` bytes 0..3 ("mb size" slots 0..3) = `((width + 127) & ~127) >> 4`, i.e. the 128-aligned pitch in 16-byte units (0x52edec). The decoder uses slot 1 for H.264 when picture width < some limit, else slot 0 (0x414140, 0x414358).

Index assignment used by eCos (0x431a70 `mfd_set_bg2d_index`, 0x43232c):

| Index | Buffer |
|---|---|
| 0x00..0x13 | frame buffer *i* luma (0x432528..0x43256a) |
| 0x14..0x27 | frame buffer *i* chroma (0x432592..0x4325d8) |
| 0x28 | EVB bitstream ring (0x431b02..0x431b44) |
| 0x29 | SPMP command ring (0x431b76..0x431bb6) |
| 0x2A.. | H.264 co-located MV ("MVC") buffers (0x432470, 0x431c32) |
| 0x3B, 0x3C | "unique" working buffers (0x431cbe.., 0x431d44; also ctx+0x2A240 = 0x3B, ctx+0x2A23C = 0x3C, 0x436c8a) |

Before each decode the current picture's luma/chroma/MVC entries are rewritten with gp = 1, scan type and address (0x414144..0x414336, section 2).

### 1.4 Working memory (eCos layout table)

`0x43267e` copies a per-codec table (0x672B0C + codec·0x9C; codec 6 = "Streaming H264", used by Android Auto: `f_00391af0` calls `mfd_if_init(6, 0, 0)` at 0x391b94) to 0x7296EC; 0x4321ae allocates entry *i* (address KB at 0x7296EC+16i, size KB at 0x7296F0+16i). For codec 6:

| Entry | Size | Use | Evidence |
|---|---|---|---|
| 0 | 0x9C4 KB (2.44 MB; codec 1: 3 MB) | EVB bitstream ring, BG2D 0x28 | D680/D684 at 0x434958; 0x431b02 |
| 1 | 2 KB | SCS start-code info buffer | D690/D694 at 0x434958 |
| 2 | runtime | MVC buffers, size per frame `(((w+15)/16+3)&~3)·(((h+15)/16+3)&~3)·64` B | 0x4323b4..0x432420 |
| 4 | 0x404 KB | SPMP command ring, BG2D 0x29 | ctx+0x29C68/ctx+0x2A238 at 0x436c3a, 0x434aee |
| 5 | 16 KB | open | |
| 6 | 100 KB | open | |
| 7 | 7 KB | FMO slice-group map (written via `0x40000000 | addr`, so uncached alias, 0x414774) | 0x41470c |
| 8 | 2 KB | open | ctx+0xAA498 |

Decoder context pointer: `*(u32*)0x6D7898` (ctx). Ring bookkeeping: ctx+0x29C58 SPMP write offset (bytes), ctx+0x29C68 SPMP base KB, ctx+0x2A238 SPMP size KB, ctx+0x2A230 = 0x28 (EVB index), ctx+0x29C6C = 0x29 (SPMP index) (0x436c76..0x436c86).

### 1.5 H.264 register init (`0x434aac`, called once from `h264_create` 0x436cb6)

In order:

1. `D0B4 = 0xFE`
2. `DF68 = (EVB_KB >> 16) & 3`, `DF18 = EVB_KB & 0xFFFF` (EVB_KB = EVB size in KB)
3. `D980 = 0` (codec select; RV writes 5, VC-1 uses other values; 0 = H.264, inference from 0x41fe40 "clear H264")
4. SCS config `0x434958`: `D680 = EVB base (phys>>10)`, `D684 = EVB size KB`, `D6C0 = 0x200`, `D6C8 = D6C4 = D6BC = 0`, `D640 = 0`, `D660 = 0`, `D710 = 1`, `D714 = 0xFFFFFF`, `D718..D72C = 0`, `D70C = D68C = D688 = 0`, `D690 = SCS-info base (phys>>10)`, `D694 = SCS-info size KB`
5. DPFU1 ring `0x4349e8`: `D8A4 = D924 = (EVB_KB << 8) | 0x28`; if `D8A0 & 1`: `D884 = 0; D8A0 = 1; D8A0 = 0`
6. SPMP ring `0x434a3e`: software write offset = 0; `D50C = 1; D548 = 1; D034 = 0; D1A4 = 0; D558 = 0; D55C = 0; D564 = 0; D550 = 0x1F8; D550 = 0`
7. `EF00 = 0xFFF, EF08 = 0x1F, EF0C = 1, EF10 = 0xFF, EF14 = EF18 = EF1C = 7, EF20 = 0x3FF, EF24 = 7, EF28 = 0x3F, EF2C = 0x1F, EF30 = 0x3F, EF34 = 7, EF38 = EF3C = 1, EF40 = 0x7F, EF44 = 1, EF04 = 3` (0x434b1e..0x434b7e; meaning open, looks like arbiter/priority setup)
8. `EC4C |= 8`; `D038 = 0`

(All Dxxx/Exxx above are 0x9C00Dxxx/0x9C00Exxx.)

H.264 soft reset `0x41325e` (used by error recovery and `mfd_if_reset`): `D000 = 0; D904 = 0; D920 = 1`; poll `D920 & 4` clear (0x3E9 tries); `D904 = 0`; if `D0AC != 0`: `D0AC = 1; D0AC = 0`; `D048 = 1; D030 = 0; D038 = 0; D0A8 = 0`.

## 2. Per-frame sequence

### 2.1 Getting the bitstream in

Two ways exist; eCos uses the SCS, a bare-metal driver can skip it (inference: the DPFU only cares about the EVB contents).

**SCS path (eCos, `0x4342a0`)**, input = caller buffer `src` of `len` bytes (any physical address):

1. `D6CC = src >> 10`; `D6D0 = ((len + (src & 0x3FF) + 0x3FF) >> 10) + 1`; `D6C8 = consumed + (src & 0x3FF)`; `D6C4 = len + (src & 0x3FF)` (0x4345e0..0x43460a)
2. `D700 = 0`; `D660 = read_KB << 10`; `D640 = EVB write offset (bytes)`; `D708 = saved state`; `D688 = D68C = 0`; `D70C = saved flag`
3. `D700 = 0x25` (start); poll `D704` until bit 0 = 0 (0x434644..0x434660, no timeout)
4. `D704` bit 1 = stopped at a start code (inference), bit 2 = source consumed, bit 3 = EVB full ("HW SCS out buffer full", 0x43474a). New write offset in `D640`, source offset in `D6C8`, state in `D708`/`D70C`.
5. H.264 start-code pattern registers: `D710/D714 = 0x000001 / 0xFFFFFF` (value/mask); MPEG-4 uses three more pairs at D718..D72C (0x41f678). So the SCS matches 00 00 01.

**DPFU1 (feeds the bit reader), `0x4340cc`:** `pos` = byte offset of the NAL in the EVB, `len` = bytes available:

1. `D884 = 0`; poll `D8A0 & 0xC` = 0 (0x1F5 tries)
2. `D884 = 0; D8A0 = 1; D8A0 = 0` (reset)
3. `D888 = ((pos >> 2) << 6) | 0x20`; `D88C = len` (capped at 0x7FFFFC)
4. `D884 = 2·flag + 1` (start; `flag` = ctx byte +0xAA458, normally 0); poll `D8A0 & 2` (0xC351 tries)
5. skip the sub-word part: read `0x9C00D400 + 4·((pos & 3)·8)`

**Bit reader** (all reads; 0x441d84..0x441e5e, 0x43c608..0x43c6a6):
- `0x9C00D400 + 4n`, n = 1..31: return next n bits, consume them. `0x9C00D400` (n = 0) returns 32 bits.
- `0x9C00D480 + 4n`: peek n bits (0x9C00D484 = peek 1 bit).
- ue(v) = count zeros with `while (!D484) D404;` then read lz+1 bits − 1 (0x441da6).

Emulation-prevention bytes: no code removes 00 00 03 in software, so the bit reader/DPFU presumably drop them (inference; open).

### 2.2 Slice header end (per slice, `H264_SliceHeaderEnd` 0x43de28)

After software has parsed the slice header through the bit reader:

1. Capture the DPFU1 position: `save0 = D894; save1 = D898` (0x43e2cc..0x43e2d4; DPFU2 variant reads D914/D918). These are the slice-data start/length in the D888/D88C format.
2. `D884 = 2·flag` (stop, 0x43e2fa)
3. If profile_idc ≥ 100: scaling lists go into the SPMP ring (`0x43a330`, section 3)
4. Commit the slice `0x43696e`: emit `[0x16220000, save0] [0x16230000, save1]` (= write D888/D88C), then `H264_SpmpProgramStoreReg` (0x43a602), then end marker `0xF8000000` (0x43a2b0); slice descriptor `ctx+0x2A2F8 = (len_words << 21) | start_word` (0x43a2de..0x43a312); slice count `ctx+0x2A2E0 += 1` (0x4369ea).

If the slice uses explicit weighted prediction, the hardware **SPAR** parses `pred_weight_table()` itself: `0x43a3ca` sets `D568 = (SPMP_KB<<8)|0x29`, `D56C = SPMP write offset`, `D51C`, `D520 = num_ref_idx_l0_active−1`, `D524 = l1−1`, `D528/D52C` (PPS weighted flags), `D518 = 2·x`, `D510 = 0; D510 = 1`; polls `D514 & 3 == 3` (0x186A2 tries); then `write_offset += D514 & 0x3FC` (SPAR wrote its words into the ring), reads `D530/D534` (log2 denominators) and `D538..D544`, `D510 = 0`. Not needed for baseline.

### 2.3 Picture decode (`h264_trigger_decode` 0x4147ae → `real_decode_h264_video` 0x4135ea)

1. Require `DC84 & 1` (previous picture ended; else "[Error] Wait picture end!!!", 0x4147c0) and `ECA0 == 0` (0x414822).
2. `DE6C = 0x18` (0x414a34)
3. `D980 = 1`; clear 0x9C00DB00..0x9C00DB7C (`0x41ff1e(0x1B6)`); `D980 = 0` (0x4136a2..0x4136c8)
4. `D0B4 = 0xFE; D034 = 1` (0x4136cc, 0x4136d4)
5. `D924 = (EVB_KB << 8) | 0x28`; `D54C = (SPMP_KB << 8) | 0x29`; `D548 = 1` (0x4136ec..0x413710)
6. BG2D for the picture being decoded, slot k (0x414144..0x414336): `A380` byte 1 = pitch/16 (`0x52ec22(1, width)`); luma index `L = tbl_0x729984[k]`: gp = 1, scan type = pic+0x40 byte, address = luma phys; chroma index `C = tbl_0x729894[k]`: gp = 1, same scan type, address = chroma phys; MVC index: gp = 1, address.
7. SPMP kick (0x4145ac..0x41467a):
   - `D048 = 1; D1A4 = 0; D558 = 0; D55C = 0; D564 = 0`
   - `D550 = 0x1F8; D550 = 0` (reset pulse, inference)
   - `D554 = first-slice descriptor` = `(len_words << 21) | start_word` of the first slice's command block in the SPMP ring (pic+0x7C ← ctx+0x2A2F8, 0x436324/0x4363de)
   - `D558 = 1; D550 = 0; D550 = 1` (start)
   - poll until `D550 & 1` reads 1 (no timeout)
   - poll `D570 & 1` until set, 0x1F5 tries of ~8 nops each, else "spmp processing done timeout" (0x41460c..0x41465a)
   - `D570 = 1` (clear), `D550 = 0`
   - if slice count n ≠ 1: `D558 = n; D550 = 0` (pic+0x74 ← ctx+0x2A2E0)
8. FMO only: `D004 |= 0x20` (0x414698).

So D558 is the number of slices the engine may run and D55C counts finished slices (inference from the error check below).

### 2.4 Completion and status

- Picture end: `DC84 & 1` = 1 (checked before the next trigger, 0x4147c0; also `mpeg24_mc_picture_end` 0x4314d6, where `DC84 bits 12:3` = error MB count for MPEG-2).
- Result check `0x4312fc` → `0x4312a2`: success requires `D55C ≥ D558`, `(D570 >> 6)` ∈ {0, 0xB}, `D0AC >> 8 == 0`, `D000 & 1 == 0`, `D904 & 1 == 0`, `D04C & 1 == 0`, `D04C >> 4 == 1`. Then `D558 = 0` and the SPMP read pointer catches up. `D038 = 0` before.
- eCos's decode timeout is 100 ticks of the 1 ms system tick (0x4309a4, inference that the tick is 1 ms per notes/interrupts.md).

### 2.5 Entropy decoding

Inference, strongly supported: **the hardware does the macroblock layer (CAVLC and CABAC).** Software only reads header syntax through the bit reader (SPS/PPS/VUI/SEI/slice header); after the slice header it hands the hardware the DPFU position of the slice data (commands 0x622/0x623 → D888/D88C) and never reads mb_type or residuals. PPS fields such as the entropy coding mode go to the hardware in command 0x401 (section 3).

## 3. The SPMP command ring

### 3.1 Ring and word format

- Ring: DRAM buffer (codec 6: 0x404 KB), BG2D index 0x29, base KB at ctx+0x29C68, size KB at ctx+0x2A238.
- Append `0x43a21e(ctx, w)`: `*(base_KB<<10 + wp) = bswap32(w)` (`wsbh` + `rotri 16`), `wp = (wp + 4) % size` (ctx+0x29C58). **Words are stored big-endian.**
- Two-word form `0x43a25c(ctx, hdr, data)` = append(hdr), append(data).

Command word (inference from the IDs; every ID that I could cross-check lands on a register software also touches directly):

| Bits 31:28 | Meaning |
|---|---|
| 0 | `reg[bits 27:16] = bits 15:0` (16-bit write) |
| 1 | `reg[bits 27:16] = next word` (32-bit write) |
| 2 | burst: `bits 15:0 + 1` following words to registers starting at `bits 27:16` |
| 0xF (0xF8000000) | end of slice command block |

Register *r* = 0x9C00C000 + 4r. Cross-checks: 0x622/0x623 → D888/D88C (DPFU1 start/length, values captured from D894/D898); 0x642/0x643 → D908/D90C (DPFU2, 0x41325e writes the same pair); 0x464..0x467 → D190..D19C, which carry the SPAR results read from D538..D544 (0x43a5c4..0x43a5f4, 0x43aba2..0x43ac06), and D180..D1AC is the block `0x432d46` zeroes at init; 0x401 → D004, whose bit 5 software also sets for FMO (0x4146a4); burst 0xA00 → 0x9C00E800 carries 224 = 64+64+6·16 scaling-list entries (0x43a330).

Ring descriptor registers: `D54C = (size_KB << 8) | index` (same format as `D8A4/D924` for the EVB). Start of a picture's commands: `D554 = (len_words << 21) | start_word`.

### 3.2 Commands for one slice, in emission order

Source structs: `S` = slice (ctx+0x14F620), `P` = PPS-like struct (ctx+0x14F624), `Q` = SPS-like struct (ctx+0x14F628), `R` = `S+0x2C` pointer (open: which struct). Field meanings are mine unless the value transformation pins them down.

| # | Word(s) | Register | Value | Address |
|---|---|---|---|---|
| 1 | 0x1622 + data | D888 | slice-data DPFU start (= D894 at header end) | 0x43b338 |
| 2 | 0x1623 + data | D88C | slice-data length (= D898) | 0x43b34c |
| 3 | 0x0406 | D018 | S+0x58 (16 bit) | 0x43a69a |
| 4 | 0x0407 | D01C | 1 if S+0x5C == 0 else 0 | 0x43a6b4 |
| 5 | 0x0424 | D090 | S+0x14 | 0x43a6ce |
| 6 | 0x0408 | D020 | S+0x30 (field/structure flag, inference) | 0x43a6e6 |
| 7 | 0x0405 | D014 | P+0x7E8 | 0x43a6fe |
| 8 | 0x0420 | D080 | S+0x132C0 != 0 | 0x43a716 |
| 9 | 0x0421 | D084 | P+0x08 | 0x43a734 |
| 10 | 0x0404 | D010 | (P+0x7DC & 0x3F) \| (P+0x7E0 & 0x3F) << 6 (two signed 6-bit values; chroma_qp_index_offset and second_chroma_qp_index_offset, inference) | 0x43a74c |
| 11 | 0x0428 | D0A0 | R+0x04 | 0x43a770 |
| 12 | 0x0409 | D024 | R+0x0C (byte) | 0x43a78c |
| 13 | 0x0402 | D008 | S+0x132C8 (byte) | 0x43a7a6 |
| 14 | 0x0403 | D00C | S+0x132CC (16 bit) | 0x43a7be |
| 15 | 0x0422 | D088 | 0 if S+0x132C8 == 0, else a quotient of S+0x00 by it (open) | 0x43a7da.. |
| 16 | 0x0423 | D08C | same family (open) | 0x43a842.. |
| 17 | 0x0425 | D094 | S+0x38 − 1 = num_ref_idx_l0_active_minus1 | 0x43a85c |
| 18 | 0x0426 | D098 | S+0x39 − 1 = num_ref_idx_l1_active_minus1 | 0x43a878 |
| 19 | 0x0429 | D0A4 | ctx+0xAA460 \| co-located/direct info (computed from list-1 POCs; open) — skipped when the ref setup is unchanged | 0x43a9c2 |
| 20 | 0x0460 | D180 | P+0x7CC (weighted_pred_flag, inference) | 0x43a9e6 |
| 21 | 0x0461 | D184 | P+0x7D0 (weighted_bipred_idc, inference; value 2 selects implicit weights at 0x43ac5c) | 0x43a9fe |
| 22 | 0x0462 | D188 | S+0xA8 (luma_log2_weight_denom from SPAR D530) | 0x43aa16 |
| 23 | 0x0463 | D18C | S+0xA9 (chroma denom from D534) | 0x43aa2e |
| 24 | 0x6000 | D800 | R+0x328 | 0x43aa46 |
| 25 | 0x6010 | D804 | R+0x32C | 0x43aa5e |
| 26 | 0x6020 | D808 | R+0x330 | 0x43aa76 |
| 27 | 0x140A + data | D028 | `0x3B<<24 \| 0x3C<<16 \| mvc_idx[cur] \| (mvc_idx[list1[0]] << 8 if B)` | 0x43aa98..0x43aaf8 |
| 28 | 0x040B | D02C | `luma_idx[cur] \| chroma_idx[cur] << 8` (current picture's BG2D indices) | 0x43ab00..0x43ab34 |
| 29 | 0x0427 | D09C | S+0x132E8 (byte) | 0x43ab42 |
| 30 | 0x0401 | D004 | `(Q.profile_idc > 99) \| Q+0x1A << 1 \| P+0x0C << 3 \| (P+0x138 ? S+4 << 8 \| 0x10 : 0)` (P+0x0C is probably entropy_coding_mode_flag, open) | 0x43ab5e..0x43ab9e |
| 31..34 | 0x1464..0x1467 + data | D190..D19C | S+0x132AC..0x132B8 (SPAR outputs) | 0x43aba2..0x43ac06 |
| B only | 0x21800065 or 0x21E00005 burst + entries; 0x1236/0x1237/0x1238 + data; 0x21E6004F/0x21E6001F burst | 0x9C00C600.., 0x9C00C8D8.., 0x9C00C798.. | implicit weights, direct-mode tables (not needed for baseline) | 0x43ac58..0x43b15a |
| 35 | 0x2040003F + 64 words | 0x9C00C100..C1FC | ref lists: list 0 then list 1, 32 entries each, entry = `luma_idx \| chroma_idx << 8` (+0x80 each when the ref struct byte 0 == 2, inference: field/long-term), zero-padded | 0x43afae..0x43b2c8 (P/I path 0x43b1c8..) |
| 36 | 0x2000003F + 64 words | 0x9C00C000..C0FC | inverse map buffer index → list position (table 0x72A6D8, built 0x43b15c..0x43b1c6) | 0x43b2ca..0x43b2fc |
| 37 | 0xF8000000 | — | end of slice block | 0x43a2b0 |

Items 35/36 are skipped when the reference setup equals the previous slice's (0x43af4e..0x43afa2). For an IDR I-slice both lists are empty, so item 35 is 64 zero words (plus the inverse map).

Optional, emitted earlier for the same slice: SPAR pred-weight words (hardware-written), and for High profile the scaling lists: `0x2A0000DF` then 224 words, one per byte of the 6 4x4 lists and 2 8x8 lists (0x43a330).

## 4. Frame buffers

- Allocated by the display frame-buffer manager (`0x50a8fc`, called through 0x50dfe0 from `mfd_set_frame_buffer` 0x43222e with format 2), info read back with 0x50e00c: luma address = info+0x18, chroma = info+0x1C (0x432600..0x432610 store them >>10 in tables 0x7298E4 (luma) and 0x729830 (chroma)).
- Format 2 geometry (0x50a996..0x50a9d8, 0x50aa1c..0x50aa2c):
  - pitch = `(width + 127) & ~127` bytes, same for luma and chroma
  - luma lines = `((ceil(h/16) + 3) & ~3) · 16` (64-line aligned)
  - chroma lines = `(((ceil(h/16) + 1) / 2 + 3) & ~3) · 16`
  - each plane rounded up to 1 KB; chroma follows luma directly
  - 800x480: pitch 896, luma 896x512 = 448 KB, chroma 896x256 = 224 KB, 672 KB per frame
- Chroma has half the luma lines at the same pitch, so it is 4:2:0 with interleaved CbCr (NV12-like). Inference.
- Tiling: the 128-byte pitch, 64-line alignment and the BG2D "scan type" bits suggest a block/tiled layout, not raster. **Open**, and the most important thing to settle in bring-up (read back a decoded plane and compare with a reference decode).
- Count: `frame_cnt = 0x47bf68() + num_ref_frames + 1` (0x6E295C, 0x4382ca..0x4382d8; num_ref_frames = Q+0x560 inference), at most 20 (indices 0x00..0x13). MVC buffers: num_ref_frames + 1 (0x6E2960).
- Fixed-size path: streaming types 1, 2, 3, 5, 8, 9 always allocate 6 buffers of 1088x1920 (0x4381f8). Android Auto sets streaming type 0xA (0x391bac), so it takes the SPS size.
- Registers for current/reference pictures: no address registers. The current picture is named by BG2D indices (command 0x40B → D02C), references by the 64-word list burst (0x040..), co-located MVs by 0x40A → D028; addresses and layout live in the BG2D table.

## 5. Display

The earlier guess of 0x9C150xxx is wrong. Video goes through VPP0: the data fetch block DDFCH at **0x9C005780**, the scaler VSCL at **0x9C005D80**, then the mixer DMIX at 0x9C006C80. A sub-pass traced this; I re-checked the address writes at 0x52a75e..0x52a7ba and the pitch write at 0x52a3c4 myself. The rest is the sub-pass's reading and was not independently re-verified.

### 5.1 Frame handoff

The field-end ISR `drv_vpp_field_end_early_isr` (0x528b14) → 0x52925e → `0x5270de(vpp, frame)` → `0x52a6d8` writes the addresses.

**Index mode.** Taken when the frame's "use BG2D" flag is set (struct +0xC in 0x52a6d8):
- The VPP rewrites the frame's BG2D entries with gp = 2, the scan type, the address, and "mb size" slot 2 = width (0x527166..0x52726e).
- It sets 0x9C005784 bit 20 and writes the raw indices to the address registers (0x52a7bc..).

**Physical mode.** Otherwise:
- 0x9C005784 bits 28 and 20 are cleared.
- Luma `phys >> 10` goes to **0x9C005798, 0x9C00579C, 0x9C0057A0** and chroma `phys >> 10` to **0x9C0057A4, 0x9C0057A8, 0x9C0057AC** (0x52a76a..0x52a7ba). These are three slots per plane, presumably frame/field (inference).

**Per-frame flags:**
- 0x9C0057DC bits 4, 5, 28, 29 (field flags; mode 0xB = progressive, 2 = interlaced)
- 0x9C005788 bit 4 set, bit 3 = field
- 0x9C005A80 |= 1 (0x52aa1e)

Inference: because the decoder writes frames through BG2D with a scan type, index mode with the decoder's own indices is the mode that handles the layout correctly. Physical mode only works if the layout turns out to be linear.

### 5.2 Fetch geometry (`0x52a1f8`)

- Pitch: 0x9C0057D0 = ALIGN(width, 128) (0x52a3c4).
- Fetch offset: 0x9C0057D8, x in [12:0], y in [27:16].
- Fetch size: 0x9C0057D4, w in [12:0], h in [27:16].
- Pre-decimation: 0x9C0057DC bit 8 = H/2, bit 9 = H/4; bits 17:16, 1 = normal, 2 = V/2.

Open: which bit selects 4:2:0 vs 4:2:2, and linear vs tiled.

### 5.3 Scaler (VSCL 0x9C005D80, `0x529b06`, writes at 0x52a4ce..0x52a516)

- Geometry: +0x0C/+0x10 input w/h, +0x14/+0x18 crop x/y, +0x1C/+0x20 crop w/h, +0x24/+0x28 panel w/h, +0x2C/+0x30 output x/y, +0x34/+0x38 output w/h.
- Factor = `((in − 1) << 22) / (out − 1)`. For 800x480 → 1024x600: H = 0x31FC7F, V = 0x332DBA.
  - Horizontal: low 16 bits → 0x9C005DCC, bits 24:16 → 0x9C005DD0, 0x9C005DC8[7:4] = 3 when the factor is ≤ 1.0.
  - Vertical: 0x9C005E04, 0x9C005E08, 0x9C005E00[7:4] = 2 when the factor is ≤ 1.0.
- 0x9C005D84: bits 0 and 1 always set; bit 2 = H scaling; bits 3 and 4 = V scaling (0x52a536).

### 5.4 Mixing with the OSD0 console

- DMIX slot source: slot 0 = 0x9C006C80[10:8], slot 1 = [14:12], slots 2..5 = nibbles of 0x9C006C88.
- Slot mode: 0x9C006C84 bits [2i+1:2i] (0x523ab8).
- eCos puts VPP0 and OSD0 both in mode 0 (0x50c1de, 0x50c26e). Under U-Boot slot 0 (VPP0) is in mode 1 (live 0x8551); by SP7021 analogy that means transparent (inference).
- With OSD0 in mode 0 (per-pixel alpha), video shows wherever the console's pixels have alpha < 0xFF (inference), or with OSD0 disabled. So the console and the video can coexist by clearing a window in the ARGB frame to alpha 0.

### 5.5 Init and test pattern

- `hal_vpp_init` 0x529d66:
  - 0x9C005780 = 1, 0x9C005784 = 0x5E000, 0x9C00578C = 0xD6D6D6, 0x9C005790 = 0x3DEF
  - 0x9C0057DC: bit 0, bit 4, [17:16] = 1
  - 0x9C0057BC = 0x14; 0x9C005788 |= 0xE0; 0x9C0057B0/B4/B8 |= 0x10001
  - 0x9C005D80 = 1; 0x9C005D84 |= 0x50
- The DDFCH pattern generator is left **on** at init: 0x9C0057F0 = 0x80801021, solid black (0x52a0a2). Bit 0 = enable, bits 5:4 = border / colorbar / solid / solid + border, bytes 1..3 = Y/Cb/Cr (`0x52b7aa`). Clear bit 0 for real frames.
- 0x9C0057F0 = 0x80801011 (color bar) is a RAM-free test of the VPP0 → DMIX → panel path.

### 5.6 Latch

Addresses are written only from the field-end ISR, which suggests vsync-latched registers. 0x9C005A80 |= 1 is the only commit-like write. Open whether anything else is needed.

Untouched blocks: 0x9C005A80, 0x9C006080 and 0x9C009Axx (picture quality) are written by the VPP code but not decoded.

## 6. Bring-up plan

Each step only reads and writes physical addresses, so it fits the payload's HU1 ops. Stop at the first step that fails.

1. **Snapshot.** Read and log MOON0 0x9C000004..0x9C000050 (clock/gclk) and 0x9C000054..0x9C000074 (reset), 0x9C00A380..0x9C00A3BC, the first 0x40 BG2D entries, and the MFD registers D000, D04C, D0AC, DC84, D550, D570, D704, D8A0, D920. A hang here means the MFD clock is off: power-cycle, then enable clock IDs 0x5A, 0x66, 0x69, 0x71, 0x64 (0x9C000018 bit 10, 0x9C00001C bits 4/6/9, 0x9C000020 bit 1, masked writes) and retry.
2. **Reset and init.** Pulse the 12 MFD reset IDs (section 1.1). Run the BG2D init (1.3) only if step 1 shows the table uninitialized (all zero). Run the H.264 init (1.5) with EVB = 2.5 MB and SPMP ring = 1 MB at fixed DRAM addresses outside the payload. Program BG2D 0x28 (EVB) and 0x29 (SPMP): `entry = (entry & ~0x7FFFF) | (phys >> 10)`, gp = 0.
3. **Bit-reader test (no decode).** Copy one Annex-B IDR access unit (SPS+PPS+IDR slice from a known 800x480 baseline stream, generated on the host with x264 `--profile baseline --slices 1`, about 30 KB) into the EVB at offset 0 with the CPU, write back the D-cache. Program DPFU1 (2.1, pos = 0, len = AU size), read 32 bits at D400 and expect `00 00 00 01`, then read the SPS fields with D404/D420/ue and compare with what the host parser says. This proves the clock, the EVB index, the DPFU and the read format.
4. **One IDR decode.** Parse SPS/PPS/slice header in C through the bit reader (or on the host, with the bit position known). At slice-header end read D894/D898. Build the slice's command block in the SPMP ring (section 3.2, items 1..37, big-endian) using values from the host parser; allocate one 896x(512+256) frame buffer, program BG2D 0x00 (luma) and 0x14 (chroma) with gp = 1 and scan type 0, and the MVC index 0x2A. Kick per 2.3, poll D570, then poll DC84 bit 0 and check the 2.4 status conditions.
5. **Read back.** Dump the luma plane (448 KB) and compare with the host's reference decode (ffmpeg `-f rawvideo -pix_fmt nv12`) both as raster and under candidate tilings (16x16 MB, 128x32/64 blocks). The matching layout settles the "scan type" question. A flat-colour IDR (all-grey 800x480) makes a first pass easy: any layout gives a uniform plane.
6. **Display.**
   - First prove the plane with no RAM involved: set 0x9C0057F0 = 0x80801011 (color bar), 0x9C006C84 bits 1:0 = 0, and OSD0 off or alpha 0.
   - Then program fetch and scaler for 800x480 → 1024x600 (5.2, 5.3), point VPP0 at the decoded frame and clear the pattern bit. Try index mode with the decoder's BG2D indices first (5.1), physical mode second.
   - Fallback: de-tile with the CPU into the OSD0 ARGB frame.

Shortcut worth trying before step 4: values for the variable registers could be recorded from eCos itself if a stock-boot session can dump DRAM (SPMP ring at its allocated address). Open whether that is possible on this unit.

## 7. Corrections to the earlier quick map

- `0x9C00D550 = 0x1F8` is followed immediately by `= 0`: a reset pulse, not the doorbell. Start = `D550 = 1`, wait until D550 bit 0 reads 1, then poll D570 bit 0 (0x4145c6..0x41463c).
- `D558` is the slice count the engine may run (1, then n), not a generic argument; `D554` is the first slice's command descriptor.
- The 0x9C00D640..D72C block is the start-code scanner (SCS) and its EVB/source pointers, not a decoder config block. 0x4158a2 is another user (open: which).
- `0x42d674` (luma/chroma scan type) belongs to the VC-1 trigger (`vc1_trigger_decode`), not H.264; H.264 does the same in `real_decode_h264_video` 0x414144..0x414336.
- The SPMP is a register-list engine, not a programmable core (inference: commands decode to register writes; no download loop into MFD space in 0x413000..0x445000).

## 8. Open questions

1. Which MOON0 clocks the H.264 path needs and their state after U-Boot.
2. Frame buffer tiling and the exact meaning of BG2D scan type and "gp"; meaning of the 0x9C00A384..A3B4 swizzle words.
3. Meaning of several command values (items 3..9, 11..16, 19, 24..26, 29, 30) beyond the source struct offset; the parser's struct layouts (S, P, Q, R) are not mapped to syntax elements.
4. D888 low bits (0x20) and D894/D898 format details; whether the DPFU removes emulation-prevention bytes.
5. The EF00..EF44, DE6C, D0B4, DB00.. block semantics (copied as constants).
6. Display:
   - Index vs physical mode for MFD frames.
   - Which DDFCH bit selects 4:2:0.
   - Whether DMIX mode 1 hides VPP0.
   - The 0x9C005784 init bits and the 0x9C005A80 / 0x9C006080 blocks.
   - Whether an explicit latch is needed outside the field-end ISR.

## Bring-up log
- 2026-11-01, step 1 (`tools/vdec_probe.py`, build 20261101-021107 under U-Boot's setup): MOON0 0x9C000004..0x9C000020 read 0xFFFF in the low halves (all clock IDs the MFD path may need are on: 5A, 64, 66, 69, 71); every MFD reset ID released. MFD registers readable without a bus stall: D000 0, D04C 4, D0AC 0, DC84 1 (picture end), D550 1, D570 0, D704 4, D8A0 0, D920 0 (idle). BG2D 0x9C00A380.. holds pitch/swizzle-looking constants (78787878, 83828100, ...); the index table 0x9C00A400 is random (uninitialized SRAM), so step 2 must program it.
- 2026-11-01, steps 2 and 3 (`hu.py vdec init`, `hu.py vdec bitread FILE`, build 20261101-025237): BG2D table initialized (0x52ec48 sequence), H.264 soft reset completed (D920 bit 2 cleared), register init 1.5 with our rings (EVB 0x04000000 / 2500 KB = BG2D 0x28 -> 0x05410000, SPMP 0x04400000 = BG2D 0x29 -> 0x05411000, SCS info 0x04500000); the unit and display kept running. An access unit copied into the EVB by the CPU (D-cache written back) and read through DPFU1 + the bit reader gives `00000001`, NAL header 0x67 and SPS fields (profile, constraints, level, sps_id, log2_max_frame_num, poc_type) identical to h264p.c, for the baseline test stream and the CABAC main-profile stream. So clocks, EVB index, DPFU start (D888 = (pos>>2)<<6 | 0x20, D88C = len, D884 = 1) and the read format (D400 + 4n = n bits; D400 = 32 bits; D484 = peek 1) are proven. Next: step 4, one IDR decode through the SPMP command ring.
- 2026-11-01 03:12, step 4 attempt (build 20261101-031056, `hu.py vdec decode grey.h264`): **the unit went dark** (no HU1 reply, no ping for 30+ min), and the hardware watchdog did not bring it back, so the MCU presumably cut power; needs an ignition/power cycle. What the attempt did: fill the frame buffers (0x04600000.., marker 0x55), set BG2D 0x00/0x14/0x2A/0x3B/0x3C (gp 1 / 0, scan type 0), **overwrite 0x9C00A380 bytes 0 and 1 (display-shared "mb size" slots; were 0x78) with 0x38**, read the slice header through the bit reader, write the 37-command block (verified word values, D800.. as 0x0600xxxx), then the 2.3 kick sequence with bounded polls. Unknown which step hung. Suspects: the decoder's DMA running away (wrong layout / addresses) and stalling the bus beyond what a watchdog reset recovers (if the MFD keeps running across the reset, U-Boot's reload could be corrupted), or the A380 change upsetting the display DMA.
  Next attempt, in smaller steps, each its own HU1 call: (a) SPMP engine only: a block of harmless register writes + end marker, kick, and check the registers took the values (no decode); (b) leave A380 alone if possible (try slot values only after reading what the display uses); (c) decode with an immediate MFD reset (assert the 12 MFD reset IDs, section 1.1) if "done" does not come within a short bound, so a runaway engine is stopped by us rather than by a chip reset; (d) record each step in a RAM breadcrumb that survives a reset (like the crash record), so the next boot reports how far it got.
- 2026-11-01, step 4 bisect (builds 034738..103617): the SPMP engine alone (harmless register writes + end marker) starts, completes (D570 0xB, D55C 1) and loads the registers; the picture-only pre-kick writes (DE6C, D980/DB00 clear, D0B4, D034, D924) change nothing. The full slice block, and every variant that **includes the slice-data DPFU pair (0x622/0x623 -> D888/D88C, values captured from D894/D898 = 0x282/0x477)**, never acknowledges the start (D550 bit 0 stays 0, D570 = 0x40); afterwards a read of D000 stalls the bus (watchdog reset). Leaving the pair out, the engine completes and decodes nothing. Resetting the MFD through MOON0 after such a timeout, without reading its status registers, recovers it in place. Breadcrumbs at 0x01F0F000 (`vdec_crumb_report` at boot) gave each of these steps. Open: the DPFU state the engine expects when it reprograms D888 (we stop it with D884 = 0 after the header; reset? the 0x20 flag absent from the captured value?); next experiments are skip bits 8 (DPFU reset after capture) and 16 (OR 0x20 into the position).
