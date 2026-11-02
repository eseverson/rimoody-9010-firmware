# Audio DSP bring-up and PCM playback (SPHE8268K, stock eCos)

Status 2026-11-02. Static analysis of `ecos.bin` (load 0x301040) and the flash dump, checked against a live call trace of the stock eCos boot (`audio1.bin.calls.txt`) and a live register dump of 0x9C001700..0x9C001CFF (`audio1.bin.audio0.txt`, about 25 s after boot, radio/AUX source, volume index 24). Companion to `audio.md` (MCU commands, amp, GPIOs). Code addresses are eCos addresses. "Inference" marks anything not proven by code or the live data.

Conventions used below: `R(x)` / `W(x, v)` are 32-bit MMIO accesses at `0x9C000000 + x`. `delay_ticks(n)` is eCos `0x3bb16c` (thread sleep once the scheduler runs, busy wait on the eCos tick counter before that; audio init runs before the scheduler). The 1000-tick timeouts are logged as 1 s timeouts, so a tick is about 1 ms (inference). `bswap16` is NDS32 `wsbh`. The DSP sees its shared-memory halfwords big-endian, so eCos byte-swaps every parameter halfword it writes.

## Payload implementation (status)

- `isp/sd-10-net/audio.c` follows sections 3..6 and 9 ("alter" ring 10): DSP window at 0x02400000 (0x4C4800 bytes, between the FreeRTOS heap and the LVGL heap), microcode checksummed and copied from the flash mapping, every DSP-memory access through the cache with explicit write-back/invalidate. A small mixer adds Android Auto media (48 kHz stereo), guidance/speech and system sounds (16 kHz mono, repeated 3x) into ring 10 at per-source cursors. HU1 op 27 / `hu.py audio status|init|tone HZ|volume N|mute 0|1`. UI volume and mute follow once it is up; the volume is saved in UI.TXT.
- **Runs on the unit (2026-11-02, build 20261102-025238, persisted):** from a cold boot the DSP comes up first try (state 0x21011113 = sys run, front play, the same as stock; banks 3/5 loaded; 0x1E14 = 0xC79BFF40 as in eCos; ring 10 address published by the DSP), with 0 mailbox timeouts. With the test tone the read pointer 0x18CC advances 192 KB/s = 48 kHz stereo s16, no drops. A warm re-init (exec over a running DSP) works too.
- Boot order (net.c `audio_boot`, as eCos power_on): MCU `01 01`, 200 ms, GPIO13 high (was low and not GPIO-owned under U-Boot; probably the 7388's ST-BY), 100 ms, DSP, MCU `01 00` unless the UI is muted.
- **Sound verified by ear (2026-11-02, build 20261102-111611, one home-theater speaker on front right):** the first path, RISC-sound ring 10, plays but the master volume (App 0x3D) does not reach it (a -42 dBFS tone sounded the same at volume index 10 and 2). Output now goes through **stream 1 with the PCM decoder** (codec 1 in bank 1, ring 1 at info +4, 0x4B000 bytes, W 0x1888 / R 0x188C, little-endian), as eCos's "main" track for Android Auto: state 0x21011133 (stream 1 play); the same tone is silent at index 10 and heard at index 34, so the master volume works there. Ring 10 stays as the fallback if stream 1 fails to start. Test tones default to -42 dBFS and stop after 2 s (`hu.py audio tone HZ [--peak N] [--ms N]`): the owner found a -12 dBFS tone too loud on this speaker.
- **Not verified:** Android Auto audio end to end (needs the phone).

## Summary

- **Microcode is plain data in flash.** A codec table (`0x3bf8a4`) points at 15 blobs with 0x40-byte big-endian `if` headers; code and data are copied with `memcpy`, no compression. Header checksums (`rotl1(c ^ byte)`) verify for every blob. Boot loads codec 0x00 (system, bank 0), 0x1d (bank 3) and 0x1c (bank 5). Extraction recipe in section 1.
- **The DSP runs out of a CPU-assigned DRAM window** (live: 0x012DA400, 0x4C4800 bytes, from eCos' memory-region table). Six code banks plus a data area; their addresses go into 0x9C001744..0x1758 (addr >> 8, plus loaded bit 31 and codec id 28:24), 0x9C001764 / 0x1ED0 (data base), sizes into 0x9C0017C0..0x17E0. All CPU accesses use the uncached alias 0x40000000 | phys.
- **Boot handshake:** code for bank 0 → release the DSP (0x9C001780 = 0x10) → wait for "sys ready" in the state word 0x9C0018B0 → idle (cmd 0/0/2) → data blobs (their destinations are published by the DSP in an info table at data + 0x280) → other banks → run (cmd 0/0/3) → front output play (3/8/1, 0/7/2).
- **Mailbox:** 0x9C001880/0x1884 + doorbell 0x9C001784 bit 0, ack = 0x1880 bit 31. DSP→CPU messages in 0x9C0018A8/0x18AC must be acknowledged (bit 31). Parameters live in byte-swapped halfword arrays in DSP memory; cmd 5/2 and 5/4 tell the DSP that the RISC or App parameter block changed.
- **Output:** the boot code programs the internal codec/DAC (0x9C001E08..0x1F64, DAC on = 0x1E14 bits 31:30) and never touches the external I2S block, so the speakers are almost certainly on the internal DAC (inference). 48 kHz clocking is `0x3aee20(4)`. Master unmute is cmd 6/7/0; master volume is App param 0x3D low byte = 2·(dB + 103.5), then cmd 5/4.
- **PCM:** every PCM path is a DRAM ring in the DSP data area with a CPU write-pointer register and a DSP read-pointer register in 0x9C001888..0x18E4; the CPU signals data just by advancing the write pointer. Android Auto uses the "main" track = DSP stream 1 with the PCM decoder (codec 1 into bank 1), ring #1 (W 0x1888, R 0x188C). A simpler "RISC sound" ring #10 (W 0x18C8, R 0x18CC, 0x16800 bytes, big-endian s16, rate by cmd 6/8/1) needs no decoder.

## 1. Microcode store (flash, uncompressed, checksummed)

The DSP microcode lives in flash and eCos reads it through the XIP window (0x98000000 + flash offset). Nothing is compressed or encrypted: `DownloadCode` and `Download2` are plain `memcpy` (0x301728) from flash into DRAM.

`0x3bf8a4` (first call of `0x3bf842`, "pDspCode") builds a 32-entry table of codec descriptors: pointer table at RAM 0x6e1f8c, descriptors of 0x4c bytes at 0x6e160c + 0x4c·id. For each present id it stores two flash pointers, descriptor +0x40 = header, +0x48 = data blob (0 = none), then parses the header into the descriptor and sets +0x44 = header + 0x40 (code start). The header is 0x40 bytes, all fields big-endian:

| Header offset | Size | Meaning |
|---|---|---|
| +0x00 | 2 | magic `if` (0x69 0x66) |
| +0x02 | 2 | flags. Bit 1 = has a data blob. Bit 0 (codec 0 only matters) = if clear, `DSP_InitSystem` sets 0x9C001794 bit 1. Bit 2 = take the code size from +0x2c instead of +0x08 (no codec uses it) |
| +0x04 | 4 | byte 0 = codec id, bytes 1..3 = build date YY MM DD (inference from values like `1d 21 04 07`) |
| +0x08 | 4 | code size in bytes (the exact `memcpy` length) |
| +0x0c | 4 | data blob size in bytes |
| +0x10, +0x12 | 2+2 | two resource numbers (inference: MIPS/memory budgets; eCos only takes maxima of +0x12 per class) |
| +0x14 | 4 | checksum of the code bytes |
| +0x18 | 4 | checksum of the data bytes |
| +0x28 | 4 | 0x6d4 for all but codec 2 (0x434); returned by `0x3bf888` (inference: interface version) |

The checksum (`0x3be948`, used by `DSP_CheckCodec`): `c = 0; for each byte b: c ^= b; c = rotl32(c, 1)`. Every code and data blob below verifies against its header, so the table is complete and correct. The data blob always sits immediately before its header (data + size == header).

| Codec id | Header (XIP) | Code size | Data blob (XIP) | Data size | Date |
|---|---|---|---|---|---|
| **0x00 (system)** | 0x98594184 | 0x333f0 | 0x98590f04 | 0x3280 | 23-03-29 |
| 0x01 | 0x98584480 | 0xca44 | none | 0 | 20-05-20 |
| 0x02 | 0x9857912c | 0xb312 | 0x9857322c | 0x5f00 | 18-05-24 |
| 0x06 | 0x985669e8 | 0xc802 | 0x98560268 | 0x6780 | 19-03-12 |
| 0x08 | 0x985481d8 | 0x1804e | 0x9853c4d8 | 0xbd00 | 18-04-16 |
| 0x09 | 0x9852f918 | 0xcb7e | 0x98528698 | 0x7280 | 18-03-22 |
| 0x0a | 0x9851e238 | 0xa420 | 0x985149b8 | 0x9880 | 18-04-16 |
| 0x0d | 0x98512590 | 0x23e8 | none | 0 | 22-11-15 |
| 0x13 | 0x98510788 | 0x1dc8 | 0x98510208 | 0x580 | 20-08-18 |
| 0x16 | 0x984ffb50 | 0x10676 | 0x984f5fd0 | 0x9b80 | 20-01-20 |
| 0x18 | 0x984f23ec | 0x3ba4 | 0x984f1b6c | 0x880 | 17-08-25 |
| 0x1b | 0x984e5530 | 0x31a6 | 0x984e4eb0 | 0x680 | 17-08-25 |
| **0x1c** | 0x984d09d8 | 0x14498 | 0x984c5fd8 | 0xaa00 | 18-08-23 |
| **0x1d** | 0x984b90e8 | 0xceae | 0x984af5e8 | 0x9b00 | 21-04-07 |
| 0x1e | 0x984f0098 | 0x1a92 | 0x984e8718 | 0x7980 | 22-09-05 |

Ids 3, 4, 5, 7, 11, 12, 14..18, 20, 21, 23, 25, 26, 31 have no code in this build. Boot loads only 0x00 (bank 0), 0x1d (bank 3) and 0x1c (bank 5). The trace values 0x6773f0 / 0x67748c in the `DownloadCode` / `Download2` argument columns are stale registers (string pointers), not table addresses. Codec names are not in the image; from the bank classes below, 0x1b/0x1d belong to the "bank 3/4" class and 0x1c is the only "bank 5" codec (inference: hands-free/post-processing helpers, not stream decoders).

Extraction script (verified; writes `dsp_XX_code.bin` / `dsp_XX_data.bin`):

```python
import struct, sys
f = open('dump1.bin', 'rb').read()          # 16 MB flash dump
T = {0x00: (0x594184, 0x590f04), 0x1c: (0x4d09d8, 0x4c5fd8), 0x1d: (0x4b90e8, 0x4af5e8)}  # flash offsets (add others from the table)
def ck(b):
    c = 0
    for x in b: c ^= x; c = ((c << 1) | (c >> 31)) & 0xffffffff
    return c
for cid, (h, d) in T.items():
    assert f[h:h+2] == b'if'
    csz, dsz, cck, dck = struct.unpack('>II', f[h+8:h+16]) + struct.unpack('>II', f[h+0x14:h+0x1c])
    code, data = f[h+0x40:h+0x40+csz], f[d:d+dsz]
    assert ck(code) == cck and ck(data) == dck
    open(f'dsp_{cid:02x}_code.bin', 'wb').write(code); open(f'dsp_{cid:02x}_data.bin', 'wb').write(data)
```

## 2. DSP DRAM window and memory layout

The DSP executes from and works in a DRAM window the CPU carves out. eCos gets it from its memory-region table (`0x492cc4(1)`, fields in KB: A = +4 base, B = +8 size) in `AUDIF_Init` 0x3b13ac:

- DSP area base `0x6e04dc = A << 10`, DSP area size `0x6e04d8 = (B − 190) << 10`.
- The last 190 KB go to the CPU mixer: `0x3a7a50((A+B−190)<<10, 0x25800)` and `0x3a74e0((A+B−40)<<10, 0xa000)` (see the PCM section).
- Live values: base 0x012DA400, size 0x4C4800, so A = 0x4B69 KB, B = 0x13D0 KB, mixer buffers at 0x0179EC00 (150 KB) and 0x017C4400 (40 KB), window ends at 0x017CE400.

`0x3bd5ce(base, keep_params)` (called with `keep_params = 0` from `AUDIF_Init`, 1 from `AUDIF_ResetDsp`) computes the layout. If `base <= 0x3FFFFFF` it first converts it to the uncached alias `0x40000000 | base` (`0x314680`), so every CPU access to DSP memory, including the microcode copies, is uncached and needs no cache maintenance. Bank sizes are the per-class maxima over all codecs in the table, each rounded as `(size & ~0xff) + 0x100` (0x3bfc7e):

| Bank | Who goes there | Size (this build) | Address (live) | Register (addr>>8, 28-bit) |
|---|---|---|---|---|
| 0 | codec 0 (system) | 0x33400 | 0x012DA400 | 0x1744 |
| 1 | stream decoders (ids 0x01, 0x02, 0x06..0x0a, 0x0d, 0x13, 0x14, 0x16, 0x18, 0x1e) | 0x18100 | 0x0130D800 | 0x1748 |
| 2 | same class | 0x18100 | 0x01325900 | 0x174C |
| 3 | ids 0x1b, 0x1d | 0xCF00 | 0x0133DA00 | 0x1750 |
| 4 | ids 0x1b, 0x1d | 0xCF00 | 0x0134A900 | 0x1754 |
| 5 | id 0x1c | 0x14500 | 0x01357800 | 0x1758 |
| data | DSP data/param memory | rest of window | 0x0136BD00 | 0x1764 and 0x1ED0 (byte address) |

Register writes in `0x3bd5ce` (all proven against the live dump):

```c
for (b = 0; b < 6; b++) W(0x1744 + 4*b, (bank[b] & 0x0fffffff) >> 8);   // bank[] are uncached aliases, so the top nibble is stripped
W(0x1764, data & 0x0fffffff);  W(0x1ed0, data & 0x0fffffff);
W(0x17c0, size0); W(0x17c4, size1); W(0x17c8, size1); W(0x17cc, size3); W(0x17d0, size3); W(0x17d4, size5);
W(0x17e0, area_size - (data - bank0));   // live 0x432F00 = data memory size
```

The bank registers later get bit 31 (loaded) and bits 28:24 (codec id) from `Download2` (live: 0x1750 = 0x9D0133DA = codec 0x1D in bank 3, 0x1758 = 0x9C013578 = codec 0x1C in bank 5). 0x1744 also reads back with bit 31 set although no eCos code writes it (inference: set by the DSP or hardware when bank 0 boots).

Data memory map (relative to `data`, held uncached in RAM 0x6e05b0/0x6e05ac):

| Offset | Size | eCos name / RAM pointer | Content |
|---|---|---|---|
| +0x000 | 0x180 | RISC params, 0x6e05ac (get 0x3bd79e, set 0x3bd312, 0xC0 halfwords) | CPU-written configuration, initialized by `0x3bd460` |
| +0x180 | 0x100 | App params, 0x6e05a8 (get 0x3bd80a, set 0x3bd336, 0x80 halfwords) | initialized by `0x3bd35a` |
| +0x280 | 0x200 | DSP info table, 0x6e059c (read 0x3bd94e) | zeroed by the CPU (`0x3bd23e`), then written by the DSP system code (inference, nothing on the CPU side writes it). Halfwords +0x12..+0x1c give the data-blob location of each bank, +0x20/+0x32/+0x36/+0x3c further areas, all as `bswap16(hw) * 128` bytes from `data` |
| info[+0x36]·128 | 0x500 | id 0x27 area, 0x6e05a4 | zeroed after boot (`0x3bd174`) |
| info[+0x3c]·128 | 0x100 | id 0x28 area, 0x6e05a0 | initialized after boot (`0x3bd1a4`) |

## 3. Clock and analog setup before the DSP (AUDIF_Init 0x3b13ac)

`AUDIF_Init` runs once from the system init thread 0x306286, before the scheduler starts. In order:

1. Memory window and mixer buffers (section 2).
2. **DSP clock** `0x3ae2c2(MHz)`: MHz = 0x134 (308) if chip id `R(0x0000) == 0x650`, 0x1B0 (432) if 0x651. The live 0x9C001780 = 0x10 says this unit is a 0x651 (see step 4 of `DSP_InitSystem`), so 432 MHz, select code 2 (308 → 4, 360 → 3, 540 → 1, else 2). MOON writes with the upper 16 bits as write-enable mask:
   - `W(0x0200, (R(0x0200) & 0x0DFF) | 0x1000 | 0xFFFF0000)` (clears bits 9 and 15:13, sets bit 12)
   - `W(0x0214, (R(0x0214) & ~0xF0) | sel << 4 | 0xFFFF0000)`
3. **Analog codec / DAC init** `0x3bf564(1)` (exact order):

```c
W(0x1f64, R(0x1f64) | 0x80000000);
W(0x1e14, 0x042bff5f);
W(0x1e14, R(0x1e14) | 0x03800000);
W(0x1e14, R(0x1e14) & ~0x1f);
W(0x1e14, R(0x1e14) | 0xc0000000);       // DAC on, same bits as AUDIF_Set(0x58) / shell "dacon"
W(0x1f64, R(0x1f64) | 0x1f);
W(0x1e18, R(0x1e18) | 0x80000000);
W(0x1e18, 0x4f064f00);
W(0x1f54, 0x00004f00);
W(0x1f54, R(0x1f54) | 0x3f000000);
W(0x1e18, R(0x1e18) & 0xf3fff3ff);
W(0x1fec, R(0x1fec) & 0xff0000ff);
W(0x1fec, 0x00440000);
W(0x1f54, R(0x1f54) & ~0xf0);  W(0x1f54, R(0x1f54) | 0xf0);
W(0x1f54, R(0x1f54) & 0xffc0ffff);
W(0x1f54, R(0x1f54) | 0x00040000);
W(0x1e80, 0);  W(0x1e84, 0x7777);  W(0x1e88, 0x300);  W(0x1e8c, 0x475);  W(0x1e90, 0x7e33a9);
W(0x1e94, 0x650200);  W(0x1e98, 0x650200);
W(0x1e9c, 0x800000);  W(0x1ea0, 0x800000);  W(0x1ea4, 0x800000);  W(0x1ea8, 0x800000);
W(0x1eac, 0x1111);  W(0x1eb0, 0);  W(0x1eb4, 0);  W(0x1eb8, 0);  W(0x1e80, 0xf);
W(0x1e18, R(0x1e18) & ~0x40000000);
W(0x1e18, R(0x1e18) & ~0x4000);
W(0x1f58, 0);  W(0x1f58, 0x06c60606);
W(0x1e18, R(0x1e18) & 0xffe0ffe0);
W(0x1e18, R(0x1e18) | 0x00060006);
W(0x1f5c, 0x17171717);
// 0x3bf50c:
W(0x1e08, 0x4d);  W(0x1f3c, 0x4d);  W(0x1f40, 0x5c);  W(0x1e10, 0x5c);  W(0x1e20, 0x5c004d);
W(0x21a4, 0x76543210);  W(0x21a8, 0x98);  W(0x21ac, 0x76543210);  W(0x21e0, 0x76543210);
```

   The meaning of most fields is unknown. 0x1f54 bits 7:4 and 0x1fec bits 21:16 are the ADC input mux fields from `audio.md`; 0x21a4/0x21ac/0x21e0 = 0x76543210 look like identity channel maps (inference).
4. RAM-only state init (`0x3accac`, `0x3acdee`, `0x3ace3e`, `0x3acb2c`; no MMIO).
5. `0x3bd5ce(base, 0)`: layout plus parameter init (section 4), then `DSP_InitSystem` (section 5), then the post-boot steps in section 6.

The audio sample clock is set later by `0x3aee20(4)` → `0x3bf760(mask = 4)` (48 kHz, see section 6). eCos does not touch MOON clock-gate or reset registers for audio besides 0x0200/0x0214/0x0238; whatever U-Boot leaves enabled is assumed (open question for bare metal: if the block is gated after our own U-Boot path, reads stall the bus, as 0x9C001D00..0x1EFF did in the live dump).

## 4. `0x3bd5ce(base, keep_params)` in full

```c
W(0x1798, 2);                       // inference: request DSP stop / reset
delay_ticks(100);
while ((R(0x1790) >> 1) != 0 && (R(0x1824) & 0xffff) != 0) ;   // wait until idle (live: 0x1790 = 0x10, 0x1824 = 0)
W(0x1780, 1);                       // hold the DSP (bit 0)
if (base <= 0x3ffffff) base |= 0x40000000;   // uncached alias
build_codec_table();                // 0x3bf8a4, section 1
compute bank/data addresses; write 0x1744..0x1758, 0x1764, 0x1ed0, 0x17c0..0x17d4, 0x17e0 (section 2)
if (!keep_params) init_risc_and_app_params();   // 0x3bd460 (calls 0x3bd35a and 0x3bc63e)
memset(data + 0x280, 0, 0x200);     // 0x3bd23e: DSP info table
mailbox_clear();                    // 0x3bd262
write_bank_sizes_to_params();       // 0x3bc63e again
```

`mailbox_clear` (0x3bd262): `for (i = 0; i < 31; i++) W(0x1880 + 4*i, 0); W(0x1880, 0x80000000); W(0x18a8, 0x80000000); W(0x18f8, 0x80000000);`

Parameter init, exact CPU halfword stores (`H(off) = v` is a little-endian 16-bit store at that byte offset; the DSP reads it big-endian, so for example `H(0) = 0x6f8f` is the DSP value 0x8f6f). `P` = data + 0 (RISC params), `A` = data + 0x180 (App params):

```c
// 0x3bd460
memset(P, 0, 0x180);
P.H(0x04)=0xa75e; P.H(0x00)=0x6f8f; P.H(0x02)=0x8900;
if (chip == 0x650) { P.H(0x06)=0x264f; P.H(0x08)=0x00e7; } else { P.H(0x06)=0x0080; P.H(0x08)=0; }
P.H(0x7e)=0x0100; P.H(0x0e)=0x0100; P.H(0x26)=0x0100; P.H(0x7a)=0x1100; P.H(0x20)=0x2f00; P.H(0x28)=0x0040;
P.H(0x22)=0xcfcf; P.H(0x0c)=0x1000; P.H(0x7c)=0;
P.H(0x32)=0x80; P.H(0x34)=0; P.H(0x36)=0; P.H(0x38)=0x80; P.H(0x3a)=0x80; P.H(0x3c)=0; P.H(0x3e)=0;
P.H(0x40)=0x80; P.H(0x42)=0x80; P.H(0x44)=0; P.H(0x46)=0; P.H(0x48)=0x80; P.H(0x4a)=0x80; P.H(0x4c)=0; P.H(0x4e)=0;
P.H(0x50)=0x80; P.H(0x52)=0x80; P.H(0x54)=0; P.H(0x56)=0; P.H(0x58)=0x80; P.H(0x5a)=0x80; P.H(0x5c)=0; P.H(0x5e)=0;
P.H(0x60)=0x80; P.H(0x62)=0x80; P.H(0x68)=0x80; P.H(0x6a)=0x80; P.H(0x70)=0x80; P.H(0x72)=0x80; P.H(0x78)=0x80;
P.H(0x140)=0x401f; P.H(0x64)=0; P.H(0x66)=0; P.H(0x6c)=0; P.H(0x6e)=0; P.H(0x74)=0; P.H(0x76)=0;
P.H(0x144)=0xd703; P.H(0x146)=0; P.H(0x16c)=0; P.H(0x148)=0x0619; P.H(0x17e)=0xcfcf; P.H(0x172)=0x1000; P.H(0x16a)=0x0008;
// 0x3bd35a
memset(A, 0, 0x100);
static const u16 t[10] = {0xb0,0x123,0x1b3,0x203,0x233,0x2c3,0x343,0x373,0x393,0x3b0};
for (i = 0; i < 60; i += 10) for (j = 0; j < 10; j++) A.H(2*(i+j)) = bswap16(t[j]);
A.H(0x7a)=0xcf00; A.H(0x7c)=0xcfcf; A.H(0x7e)=0xcfcf; A.H(0x80)=0xcfcf; A.H(0xa8)=0x8f31; A.H(0xaa)=0; A.H(0x78)=0x2200;
A.H(0xba)=0xb480; A.H(0xbc)=0x3401; A.H(0xbe)=0xb401; A.H(0xc0)=0x3402; A.H(0xc2)=0xb402; A.H(0xc4)=0x3403; A.H(0xc6)=0xa403;
A.H(0xd0)=0x0500;
// 0x3bc63e (values from the codec-table summary at RAM 0x6e15dc, constants in this build)
P.H(0x16)=bswap16(0x64); P.H(0x1c)=bswap16(0x64); P.H(0x1e)=bswap16(0x5a); P.H(0x24)=bswap16(0x5a);
P.H(0x0a)=0x0100; P.H(0x12)=bswap16(0x12c); P.H(0x14)=bswap16(0x12c); P.H(0x18)=bswap16(0x64); P.H(0x1a)=bswap16(0x64);
P.H(0x2e)=bswap16(0x5a);
```

(Inference: 0x12c/0x64/0x5a are MHz/MIPS budgets: 300, 100, 90.) Since chip 0x651 takes the `else` branch, P+0x06 = 0x0080 and P+0x08 = 0 on this unit.

## 5. `DSP_InitSystem` 0x3bb996 and the download functions

```c
int DSP_InitSystem(void)
{
    desc0 = codec_desc(0);                               // 0x3bf842
    W(0x1798, 2);
    delay_ticks(100);
    while ((R(0x1790) >> 1) != 0 && (R(0x1824) & 0xffff) != 0) ;
    W(0x1780, 1);                                        // DSP held
    if (!(desc0->flags & 1)) W(0x1794, R(0x1794) | 2);   // flags = 0x0002 here, so bit 1 is set (live 0x1794 = 3)
    DownloadCode(0x00, 0);                               // bank 0 = system code, no data yet
    mailbox_clear();                                     // 0x3bd262
    W(0x1780, chip == 0x651 ? 0x10 : 0);                 // release: bit 0 cleared (live 0x10)
    DSP_WaitState(SYS, READY);                           // 0x3bb2ec(0, 1), 1000 ticks max
    DSP_SetSystemIdle();                                 // 0x3bb71a: if sys state != IDLE, cmd(0,0,2) then wait sys == IDLE
    Download2(0x00, 0);                                  // system data blob
    DownloadCode(0x1d, 3); Download2(0x1d, 3);
    DownloadCode(0x1c, 5); Download2(0x1c, 5);
    DSP_SetSystemRun();                                  // 0x3bb7f6: if sys state == IDLE, cmd(0,0,3) then wait sys == RUN
    0x3bb94e();                                          // if RISC param 0 (DSP view 0x8f6f) bit 6: front (type 7) to STOP if needed (cmd 0,7,1 + wait),
                                                         // cmd(3,8,1), then front PLAY (cmd 0,7,2 + wait type 7 == 2)
    DSP_GetInfo(0x1f);                                   // 0x3bb202, read only, result unused
    return 0;
}

int DownloadCode(int id, int bank)                       // 0x3bafe0
{
    d = codec_desc(id); if (!d) return 0x1b;
    dst = bank_addr[bank];                               // 0x3bca04(2*bank): RAM 0x6e05c8 - 4*bank
    if (bank >= 1) W(0x1744 + 4*bank, R(0x1744 + 4*bank) & 0x60ffffff);  // clear loaded bit 31 and codec id 28:24
    memcpy(dst, d->code /* header+0x40 */, d->code_size /* header +0x08, exact */);
    return 0;
}

int Download2(int id, int bank)                          // 0x3baf48: data blob, then mark bank loaded
{
    d = codec_desc(id); if (!d) return 0x1b;
    if (d->data) {
        static const u8 info_off[6] = {0x12, 0x14, 0x16, 0x1a, 0x1c, 0x18};   // bank 0..5 (ids 0x1e,0x1f,0x20,0x22,0x23,0x21)
        u16 o = bswap16(*(u16 *)(data + 0x280 + info_off[bank]));
        if (o == 0) return 1;                            // DSP has not published the address: bank NOT marked loaded
        memcpy(data + o * 128, d->data, d->data_size);
    }
    if (bank >= 1) W(0x1744 + 4*bank, R(0x1744 + 4*bank) | 0x80000000 | id << 24);   // 0x3baec4
    return 0;
}
```

Order matters: the data-blob destinations come from the DSP info table at data+0x280, which only the running system code can fill, so code for bank 0 goes in first, the DSP is released and reaches READY, and only then are the data blobs copied.

**State word 0x9C0018B0** (written by the DSP; `0x3bb284(type)` returns nibble `type`, type 7 is bits 29:28): names from the tables at 0x677468 / 0x6774a4 / 0x67748c.

| Nibble | Type | States |
|---|---|---|
| 3:0 | 0 sys | 0 init, 1 ready, 2 idle, 3 run, 4 dead |
| 7:4 | 1 stream1 | 0 init, 1 ready, 2 stop, 3 play, 4 ff, 5 pause |
| 11:8 | 2 stream2 | same |
| 15:12 | 3 HF_DEC | |
| 19:16 | 4 ENC1 | |
| 23:20 | 5 ENC2 | |
| 27:24 | 6 HF_ENC | |
| 29:28 | 7 front output ("FONRT" [sic] in the type-name table) | 0 init, 1 stop, 2 play, 3 pause |

Live value 0x21011123: sys = 3 (run), stream1 = 2 (stop), stream2 = 1, HF_DEC = 1, ENC1 = 1, ENC2 = 0, HF_ENC = 1, front = 2 (play). The media/stream ids used by the command layer (1..6) are these same nibble indices.

`DSP_WaitState(type, state)` (0x3bb2ec): loop up to 1000 ticks: if `nibble(type) == state` return 0; service the DSP→CPU mailbox (`0x3be80c`, below); `delay_ticks(1)`; if sys nibble == 4, log "system is in MAIN_STATE_DEAD" and fail with 0xf.

**Command mailbox** `DSP_SendCmd(cmd, p1, p2, p3)` (0x3bbf74): refuse if `R(0x1784) & 1` ("wait previous interrupt done") or if `R(0x1880)` bit 31 is clear ("wait previous command done"); then `W(0x1884, p3); W(0x1880, cmd << 24 | p1 << 16 | p2); W(0x1784, R(0x1784) | 1);` and poll until 0x1880 bit 31 is set again (the DSP's acknowledgment), servicing the DSP→CPU mailbox and sleeping 1 tick per iteration, 1000 ticks max. Live 0x1880 = 0x85040000 = the last command (5, 4, 0) with bit 31 = done.

**DSP→CPU mailbox** (`0x3be80c`): if `R(0x18a8)` bit 31 is clear, the DSP has posted a message: bits 29:28 = 0 means the payload is the low 16 bits; 2 means an extended message with `R(0x18ac)`, handled by `0x3be2a8`. The CPU acknowledges with `W(0x18a8, R(0x18a8) | 0x80000000)`. Bare-metal code must keep servicing this in every wait loop, as eCos does (inference: otherwise the DSP may stall waiting for the CPU).

Other 0x9C0017xx control bits seen: 0x1794 bit 0 is pulsed low then high by the shell commands `dumpinfo` and `pc` (0x3b624e, 0x3b9592) and ten times by the DSP watchdog 0x3b0874 before it calls `AUDIF_ResetDsp` (inference: a DSP reset/kick line). The watchdog watches the DSP heartbeat 0x9C0018B4 (low 16 bits, live 0xDA31) and resets the DSP if it does not change for 11 polls; a bare-metal port should check the same heartbeat. `AUDIF_ResetDsp` 0x3aa50a (not called in a normal boot) clears 0x1794 bit 1, then runs `0x3bd5ce(base, 1)`, `DSP_InitSystem` and `0x3aee20(4)`.

## 6. After `DSP_InitSystem` (rest of AUDIF_Init 0x3b14a6..0x3b163c)

```c
0x3ae13e(area_size);                 // size sanity check only
DSP_GetInfo(0); DSP_GetInfo(1);      // reads, results unused
// 0x3aee20(4) -> sample clock, 4 = mask bit 2 = 48 kHz (inference: index k&3 = {32k, 44.1k, 48k} x 2^(k>>2))
W(0x1e14, (R(0x1e14) & 0xff8fffff) | (((*(u32*)0x71f110 >> 20) & 7) ? 0x100000 : 0));  // 0x3bf4d2; RAM 0x71f110 = 0x100000 at boot, so bit 20 is set
W(0x1f04, 0x6003);
W(0x1f0c, 0x6883); W(0x1f10, 0x6003); W(0x1f14, 0x6883);
W(0x1f00, 0x6883); W(0x1f00, R(0x1f00) | 0x4000);
W(0x0238, R(0x0238) & ~0x30);        // 0x3bf714: 48k family 00, 44.1k family 0x10 (written without a mask half, possibly a no-op on a masked MOON register)
W(0x1f20, 0x6001); W(0x1f1c, 0x6887);   // 0x3bf3c8
W(0x1f08, 0x4887);                      // 0x3bf3fa
W(0x1f24, 0x4887);                      // 0x3bf430
W(0x1f38, 0x6003); W(0x1f34, 0x6883);   // 0x3bf464
// 0x3bf800: MOON masked writes
W(0x0238, 0xffff1c80); W(0x0238, 0xffff1c84);
// 0x3bf496: codec reset pulses
W(0x1f64, R(0x1f64) & ~0x80000000); W(0x1f64, R(0x1f64) | 0x80000000);
W(0x1e18, R(0x1e18) & ~0x80000000); W(0x1e18, R(0x1e18) | 0x80000000);
// audio time counter (0x1ec4 is read by the CarPlay/AirPlay players)
W(0x1ec0, 1); W(0x1ec8, 0x10000010); W(0x1ec0, 0);
DSP_SendCmd(1, 0, 0, 0x1f4fef);      // enable mask, kept in RAM 0x71f108; bit meanings unknown
// 0x3b135e(0,1), (1,1), (2,0): output-port enable (inference)
for each call: W(0x2194, R(0x2194) | 2); W(0x2188, R(0x2188) | 0x8080);
W(0x2198, R(0x2198) | 0x210000); W(0x2198, R(0x2198) | 0x420000); W(0x2198, R(0x2198) & ~0x840000);
// 0x3bd174: zero the id 0x27 "App" area (0x500 bytes); 0x3bd1a4: init the id 0x28 "Swb" area (0x100 bytes):
S = data + 128 * bswap16(info[+0x3c]); memset(S, 0, 0x100);
S.H(0x00)=0x0700; S.H(0x02)=0x0100; S.H(0x04)=0xffff; S.H(0x0a)=0x0002; S.H(0x0c)=0x0a07; S.H(0x0e)=0x0020;
S.H(0x14)=0x0800; S.H(0x16)=0x0004; S.H(0x18)=0x9a9a; S.H(0x1c)=0x6400; S.H(0x1e)=0x3813; S.H(0x20)=0xe803;
S.H(0x22)=0x1027; S.H(0x3c)=0x0008; S.H(0x3e)=0xc800; S.H(0x46)=0x6608;
// RISC param 0xb7 (byte 0x16e): bit 9 = (u16 at (area_base + area_size - 0x1686) != 0); then bits 13:12 = 01.
// Done through DSP_GetRiscParam / DSP_SetRiscParam, no command sent.
0x3ac5d2(); 0x34532e(); 0x479fae(); 0x47a544();   // EQ/effect defaults and config copy (many 5/2, 5/4 commands later)
```

The table lookups behind the clock values (12 entries each, index = bit number of the rate mask): 0x677c84 (0x1f00), 0x677c1c (0x1f0c/0x1f14), 0x677c50 (0x1f1c), 0x677be8 (0x1f08, `& 0xc7ff | 0x800`), 0x677bb4 (0x1f24, same), 0x677b80 (0x1f34). Index 2 (48 kHz) and index 1 (44.1 kHz) use the same dividers; the PLL family bit in 0x9C000238 differs.

## 7. Command reference and the boot trace, decoded

Command word: `cmd << 24 | p1 << 16 | p2` in 0x1880, `p3` in 0x1884.

| cmd | Form | Meaning | Proof |
|---|---|---|---|
| 0 | 0/type/state | set the state of module `type` (the 0x18B0 nibble index), then the caller waits with `DSP_WaitState`. 0/0/2 idle, 0/0/3 run; 0/m/2 stop codec m, 0/m/3 play, 0/m/5 pause, 0/m/1; 0/7/1 front stop, 0/7/2 front play | 0x3bb71a, 0x3bb7f6, 0x3bbaec, 0x3bbb70, 0x3bbdf2, 0x3bbe78, 0x3bb890, 0x3bb8c2 |
| 1 | 1/0/0/mask | module enable mask (RAM copy 0x71f108); boot 0x1f4fef; start of media needs bits 0x1f0103 (0x3ae7e0) | 0x3b1546 |
| 1 | 1/2/0/n | ring-buffer writer | 0x3b1336 |
| 1 | 1/3/0/x | echo `info[0x13] << 16 | info[0x14]` back to the DSP when bit 31 clear (inference: event acknowledgment), polled from 0x3b4a18 | 0x3be85e |
| 3 | 3/{2,3,4}/v | per-media configuration for media 1..3 | 0x3bb21a |
| 3 | 3/0xa/0/step | decoder ramp, step = 0x8000 / (ms·fs/1000) | 0x3b41ee |
| 3 | 3/8/1 | sent before front play, only if RISC param 0 bit 6 (inference: enable front output stage) | 0x3bb94e |
| 5 | 5/2 | RISC parameter block changed (every site follows a 0x3bd312 write) | 70+ sites |
| 5 | 5/3/idx/val | one RISC parameter changed | 0x3bd76e |
| 5 | 5/4 | App parameter block changed (every site follows a 0x3bd336 write) | |
| 6 | 6/3/x, 6/4/x | mute flags for two paths (`AUD_Set_Mute` 0x3b4370, `Demute` 0x3b440a) | |
| 6 | 6/7/1, 6/7/0 | master mute / unmute (shell `vol 1` / `vol 2|3`) | 0x3b42fe, 0x3b431c |
| 6 | 6/8/{1,2,3} | RISC sound rate 48 / 24 / 16 kHz (`AUD_Set_RiscSndFs`) | 0x3b449e |
| 6 | 6/0xc | front channel-select mode | |
| 6 | 6/0xd/0/bits | mixer doorbell: -1 at init, `1 << (slot+1)` per aux track write (`AUDIF_Set` 0x60) | 0x3b09f8, 0x3a885a |
| 7 | 7/… | stream/bitstream control | 0x3b0dda..0x3b125a |

Parameter areas (all halfwords, CPU stores `bswap16(v)`):

| Area | Location | Count | Setter / getter |
|---|---|---|---|
| RISC | data + 0 | 0xC0 | 0x3bd312 / 0x3bd79e |
| App ("RiscApp") | data + 0x180 | 0x80 | 0x3bd336 / 0x3bd80a |
| id 0x27 area | info-table offset | 0x280 | 0x3bd2ca |
| Swb (id 0x28) | info-table offset | 0x80 | 0x3bd2ee |
| DSP info (DSP-written) | data + 0x280 | | 0x3bd94e |

Useful parameters: RISC 7 = media routed to the main output; RISC 0x11 = that source's input gain, two bytes of `2·(dB + 103.5)` (default 0xCFCF = 0 dB); RISC 0x3d = 0x11 when routing; RISC byte offsets 0xA0/0xC0/0x100/0xE0/0x80/0x120 = 16-halfword codec descriptor of media 1..6 (`0x3bdaa0`); App 0x3d low byte = master volume code; App 0x6a = 0x800 when the routed media starts, 0 when it stops (inference: fade/ramp enable); App 0x3c/0x3e/0x3f loudness; App 0x57..0x5c per-channel delay 0..20 ms.

Trace, in order (tick 0 = early boot):

1. `DownloadCode(0,0)`, 0/0/2 (idle), `Download2(0,0)`, codecs 0x1d→bank 3 and 0x1c→bank 5, 0/0/3 (run): `DSP_InitSystem`, section 5.
2. 3/8/1 then 0/7/2: front output enable and front play (live 0x18B0 bits 29:28 = 2 confirms).
3. 1/0/0/0x1f4fef: initial enable mask (section 6).
4. `AUDIF_Set(0xc, 1)` = stop media 1: `0x3b42b0(1, 0)`: since RISC 7 == 1, App 0x6a = 0 and 5/4; then `DSP_SetCodecStop(1)` = 0/1/2 (0x3bbaf2); then RISC 0x5e (media-1 descriptor p14; media 2 → 0x6e, media 3 → 0x8e, 0x3ae89c) = 0 and 5/2.
5. `AUDIF_Set(2, 1, &0x71f144)`: configure the media-1 codec from the WAVE-format-tag struct (0x3b03d0, then `0x3aee20(4)`); no codec download appears in the trace at this point (see open questions).
6. `AUDIF_Set(0x24, 1, 1)` = route media 1 to the main output (0x3ae338): RISC 0x11 = gain of source 1 (table 0x71f12c), RISC 7 = 1, RISC 0x3d = 0x11, 5/2.
7. Tick 297: `AUDIF_Set(0x61)` / `AUDIF_Set(0x60, -1)` → 6/0xd/0/-1: aux mixer init (section 9). 1/3/0/7 then 1/3/0/0: DSP event acknowledgments.
8. Ticks 1142..1152, 5/4 bursts: loudness (0x3aa41c, 0x3aa2de), App 0x3c (0x3afdb4), four per-channel delays (0x3b006c).
9. Tick 2058: 6/7/0 master unmute, then `SetVolume(24)` → App 0x3d = 0xA5 (−21 dB), 5/4.
10. Tick 8928..9131, switch to AUX: `AUDIF_Set(0x39, 1, 0)` (ADC 1 input 0), `0xc(4)` stop media 4, `0x24(1, 4)` route media 4 (analog line-in through the ADC; no codec), `0xd(4)` start (App 0x6a = 0x800, 5/4), 5/2 from 0x3b3802 = source input gain.

Volume table correction for `audio.md`: the float table at flash 0x5c7b64 gives index 20 = −27 dB (not −25), index 24 = −21 dB, index 40 = 0 dB.

## 8. Output path to the speakers

- **Internal DAC, not I2S (inference, strong).** Boot programs only the internal codec registers (0x1E08..0x1EB8, 0x1F00..0x1F64, 0x1FEC). The external I2S block 0x9C002280..0x2294 (`AUDIF_Set` 0x56/0x57) and 0x9C00219C (`output spdif` / `output reset`) are written only by shell commands (0x3b915c, 0x3b9172, 0x3b685c, 0x3b7884, 0x3b7904). Effects and delays are set per channel with masks 1/2/4/8 and six delay channels, which fits four analog outputs into the 4-channel 7388 amp. Pinmux was not checked.
- **DAC enable:** 0x9C001E14 bits 31:30 = 11, written by `0x3bf564` at boot. `AUDIF_Set(0x58, x)` (0x3b4ec2) sets them for x = 0 and clears them otherwise; only the shell (`dacon` / `dacoff`) calls it. What these "DAC GPIO" bits drive (amp standby or mute?) is unknown.
- **Master volume** `0x3aa184(index)` → `0x3b3810`: dB from the table, clamped to [−103.5, +24]; `code = 2·(dB + 103.5)`; App 0x3d = (old & 0xff00) | code; if dB ≤ 0 and (App 0x3c & 0x30) == 0x20, App 0x3c bits 12:8 = loudness table byte (0x6e0497 − code/2); then 5/4. 0 dB = 207 (0xCF).
- **Master mute:** 6/7/1 mute, 6/7/0 unmute. The system MCU mute (`01 00`, `audio.md`) is separate and also needed.
- **ADC mux** (`AUDIF_Set(0x39, adc, src)` → 0x3b304e): adc 0 = 0x1F54 bits 7:4 = `src << 2 | src`; adc 1 = 0x1FEC bits 21:20 and 17:16 = src.

## 9. PCM playback

### Common mechanism: DSP rings with pointer registers

- **Ring address:** `data + 128 * bswap16(info[off(n)])` (getter 0x3bc720; refuses while sys state < 2). `off(n)`: 1 → +4, 2 → +6, 3 → +8, 4 → +0xa, 5 → +0xc, 6 → +0xe, 7 → +0x10, 8 → +0x30, 10 → +0x2e, 11 → +0x1ec, 12 → +0x1ea, 13 → +0x24 (jump table 0x3bc798).
- **Ring size** (0x3bc8a4): rings 1/2 = 0x4B000 (300 KB), 3/4/6 = 100 KB, 7/8/10 = 0x16800 (90 KB), from the constants at 0x6e15ee..0x6e1608 (section 4) in KB.
- **Pointer registers** (byte offsets into the ring; setters 0x3bb50c (write) / 0x3bb598 (read/"barrier")):

| Ring | Write ptr (CPU) | Read ptr (DSP) |
|---|---|---|
| 1 | 0x9C001888 | 0x9C00188C |
| 2 | 0x9C001890 | 0x9C001894 |
| 3 | 0x9C0018B8 | 0x9C0018BC |
| 4 | 0x9C0018A0 | 0x9C0018A4 |
| 5 | 0x9C0018D0 | 0x9C0018D4 |
| 6 | 0x9C0018C0 | 0x9C0018C4 |
| 7 | 0x9C001898 | 0x9C00189C |
| 8 | 0x9C0018E0 | 0x9C0018E4 |
| 10 | 0x9C0018C8 | 0x9C0018CC |

- fill = `(W − R) mod size` (0x3bb6cc), free = size − fill (0x3bb6f4); drivers keep 128 bytes of slack. The CPU writes R only to reset it to 0 on clear. **New data is signaled by writing the new W; there is no doorbell per write.** Copies go through the uncached alias (0x3a7704, 0x3a9412).

### Track API

`Track_Create_Ext` picks a driver by tag (0x3a41b8): "main" → `media_stream.c` (ops 0x6c057c, init 0x3a74fc), "alter" → `risc_sound.c` (ops 0x6c05c4, init 0x3a931e), "auxiliary" → `mixer_stream.c` (ops 0x6c05b0, init 0x3a8cb4). `Write_Blocked` 0x3a5188 calls the driver's write and retries every 8 ms until 400 ms.

**"main" (Android Auto, stream 1):**
1. `AUDIF_Set(0xc, 1)`: stop media 1 (trace step 4 above).
2. Ring 1: base = uncached(ring 1), size 0x4B000; W = R = 0 (0x3abeb6, 0x3abfb8).
3. `AUDIF_Set(2, 1, &fmt)` → 0x3ad7a4: zero 0x1888/0x188C; descriptor at RISC byte 0xA0: p0 = 1 (codec id PCM), p1 = 5 (PCM sub-format), p2 = channels, p3 = bits, p4 = rate code (48 kHz → 0x10, 0x3ace74), p15 = 0x20 (16 halfwords, each `bswap16`). Then `DSP_CheckCodec` 0x3bbc80(1): unless stream 1 is in init, stop it, `DownloadCode(1, 1)` + `Download2(1, 1)` (codec 1 = PCM decoder, 0xCA44 bytes, no data blob), then 0/1/3 and wait for stream-1 state 3 (play).
4. `AUDIF_Set(0x24, 1, 1)`: route media 1 (RISC 0x11, 7, 0x3d, then 5/2).
5. Start = `AUDIF_Set(0xd, 1)`: 1/0/0/mask if 0x71f108 lacks 0x1f0103 (already has it), play 0/1/3, App 0x6a = 0x800, 5/4.
6. Write (0x3a703e): copy little-endian s16 stereo into the ring with wrap, then write W (held back until a prefill threshold after a clear).

**"alter" (RISC sound, no decoder):** accepts ≤ 2 ch, 16 bit; byte-swaps 2-ch little-endian samples to big-endian (0x3a9e64); clears ring 10 pointers; rate by `AUD_Set_RiscSndFs` = 6/8/1 for 48 kHz; ring 10 is 0x16800 bytes; start/stop only flip a software flag (no DSP command); write updates W in 0x9C0018C8. Inference: the system code drains ring 10 continuously. Whether it is audible without extra routing is open.

**"auxiliary" mixer (up to 5 tracks):** pool from `0x3a7a50(base, 0x25800)`, 5 × 0x7880; work area = ring 11 (info +0x1ec), 0x100-byte header (+4 = 8, +8 = 0x100, +0xc/+0x10/+0x14 = 0x80000000, +0x18 = 0x240000, raw LE words), then 6/0xd/0/−1. Per slot k: track buffer 0x7800 bytes from the pool, descriptor at work + 0x100 + k·0x200 (BE32 fields: +2 buffer offset from work base, +6 write pointer, +0xa size, +0xe channels, +0x10 rate code, +0x82 read pointer); doorbell 6/0xd/0/`1 << (k+1)`. The 0xA000 region from `0x3a74e0` serves only media stream 2 (descriptor at ring-12 base + 0x280).

## 10. Bare-metal recipe (untested; composed from the above)

```c
// Preconditions: MCU heartbeat running and system mute held (audio.md); U-Boot clocks as left for eCos.
u32 base = 0x012DA400, size = 0x4C4800;          // eCos' window; any 256-byte aligned DRAM below 0x4000000 that
                                                  // our payload leaves alone should do (inference)
// 1. clocks and codec
W(0x0200, (R(0x0200) & 0x0dff) | 0x1000 | 0xffff0000);
W(0x0214, (R(0x0214) & ~0xf0) | (2 << 4) | 0xffff0000);   // 432 MHz on a 0x651 chip
codec_init();                                     // section 3 step 3 (0x3bf564 + 0x3bf50c)
// 2. layout and parameters: 0x3bd5ce(base, 0)     (section 4)
// 3. DSP_InitSystem()                              (section 5; extract codecs 0x00, 0x1d, 0x1c with section 1)
// 4. post-boot: section 6 (48 kHz clocks, 0x238, reset pulses, 0x1ec0, cmd 1/0/0/0x1f4fef, 0x2188..0x2198, Swb area, RISC 0xb7)
// 5. output: 6/7/0 (unmute); App[0x3d] = (App[0x3d] & 0xff00) | 207 (0 dB, or lower), DSP_SendCmd(5,4,0,0)
// 6a. Shortest test, RISC sound ring 10:
DSP_SendCmd(6, 8, 1, 0);                          // 48 kHz
u8 *ring = (u8 *)(0x40000000 | (data + 128 * bswap16(info16(0x2e))));
W(0x18c8, 0); W(0x18cc, 0); w = 0;
loop: n = min(len, 0x16800 - ((w - R(0x18cc) + 0x16800) % 0x16800) - 128);
      copy n bytes of BIG-ENDIAN s16 stereo to ring[w] with wrap; w = (w + n) % 0x16800; W(0x18c8, w);
// 6b. What Android Auto does (stream 1 + PCM decoder): section 9 "main" steps 1..6, ring 1, W 0x1888, R 0x188C.
// Throughout: service 0x18a8 (ack bit 31) in every wait loop; watch heartbeat 0x18b4; finally send MCU unmute 01 00.
```

## 11. Live registers tied to code

| Register | Live | Meaning |
|---|---|---|
| 0x1744..0x1758 | 0x80012DA4, 0x000130D8, 0x00013259, 0x9D0133DA, 0x000134A9, 0x9C013578 | bank addresses >> 8; bit 31 + codec id for loaded banks (bank 3 = 0x1D, bank 5 = 0x1C); banks 1, 2, 4 empty (section 2) |
| 0x1764 | 0x0136BD00 | data base (also 0x1ED0) |
| 0x17C0..0x17D4 | 0x33400, 0x18100, 0x18100, 0xCF00, 0xCF00, 0x14500 | bank sizes |
| 0x17E0 | 0x00432F00 | data memory size = area size − code banks |
| 0x1780 | 0x10 | DSP released; bit 4 = chip 0x651 |
| 0x1784 | 0 | doorbell idle |
| 0x1790, 0x1824 | 0x10, 0 | read-only busy poll (exits because 0x1824 = 0) |
| 0x1794 | 3 | bit 1 set by `DSP_InitSystem`, bit 0 reset/kick line (section 5) |
| 0x1798 | 0 | written 2 at init, reads 0 (self-clearing strobe, inference) |
| 0x1880 | 0x85040000 | last command 5/4/0, bit 31 = acknowledged |
| 0x18A8, 0x18F8 | 0x80000000 | DSP→CPU mailbox empty / acked; 0x18F8 initialized the same way, use unknown |
| 0x18B0 | 0x21011123 | state word (section 5) |
| 0x18B4 | 0x0000DA31 | DSP heartbeat watched by 0x3b0874 |
| 0x1898, 0x18E0 | 0x16200 | write pointers of rings 7 and 8 (90 KB each); inference: DSP-written capture/encoder outputs of the active AUX/radio path |
| 0x1700..0x1738, 0x178C (0xABCDDEAD), 0x179C..0x17A8, 0x1800..0x186C, 0x18F0, 0x1900..0x1AF4 | various | not referenced by the eCos audio driver: DSP-side or hardware status. 0x1718 = 0x01335A00 and 0x18F0 = 0x01315600 point into the bank 2 and bank 1 code areas. 0x1728 is only dummy-read as a delay (0x3be90e) |

## 12. Open questions

- Is the DSP info table at data + 0x280 really written by the DSP at boot? Nothing on the CPU writes it and all data-blob destinations depend on it. A live read of data + 0x280 (0x0136BF80 on the stock layout) would confirm it and give the ring offsets.
- Stream 1 is in "stop" live, but the trace shows no codec-1 download. Either the hook missed it or the boot `AUDIF_Set(2, 1, …)` format struct does not select PCM. Track_Create_Ext was not exercised in this boot (no phone audio).
- Is RISC sound (ring 10) audible without extra routing or a volume parameter?
- Clock gates: eCos touches only MOON 0x0200/0x0214/0x0238 for audio. If our boot path leaves the audio block gated, MMIO reads stall the bus (the live dump already stalls in 0x9C001D00..0x1EFF). Which MOON clock-enable bits cover 0x9C0017xx..0x9C0022xx is unknown.
- Meanings of the 1/0/0 enable-mask bits (0x1f4fef), of 3/8/1, of most codec register fields, and what the 0x1E14 bits 31:30 "DAC GPIO" pins drive.
- 0x9C000238 is written both with and without a mask half; whether the unmasked write in `0x3bf714` has any effect.
- Board pinmux of the `AUD_EXT_DAC` pins (would settle internal DAC vs I2S for good).
- Codec names for ids 0x1c / 0x1d (loaded at boot) are not in the image.
