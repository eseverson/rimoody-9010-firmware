# Audio EQ and tone controls in the stock eCos (SPHE8268K)

Status 2026-11-02 (second pass: write path, pointer derivation, encoder, slider math and tables re-verified against the disassembly and flash; recipe rewritten against `audio.c`). Static analysis of `ecos.bin` (load 0x301040) and the flash dump, cross-checked against the live call trace `notes/audio_trace/ecos_eq_calls.txt` (owner moving the stock EQ sliders). Companion to `audio_dsp.md` (DSP bring-up, mailbox, parameter areas) and `audio.md`. Code addresses are eCos addresses; 0x98xxxxxx are flash (XIP) addresses. "Inference" marks anything not proven by code or the trace.

## Payload implementation (status)

- **Working on the unit (2026-11-02, build 20261102-112420, persisted).** `audio.c` writes the slot descriptors straight into the id 0x27 area (one aligned 32-bit store per slot, cache write-back) with no mailbox command, as eCos does, and the DSP picks them up. Our own model, not stock's: a 10-band octave EQ at 31, 63, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz, peaking, Q ≈ 1.41 (code 23), the same on channels 0..3, slots 0..9.
- Measured with the C270 webcam microphone (1 kHz tone, stream 1): flat 32.1 dB, 1 kHz band at -12 dB 19.8 dB (-12.3), flat again 32.0 dB; the "Voice" preset (+4 at 1 kHz, +3 at 500 Hz / 2 kHz) +6.1 dB.
- Settings › Audio: presets Flat / Bass / Loud / Voice / Treble (ours) and ten -12..+12 dB sliders; `eq=` line in UI.TXT; HU1 op 27 sub 5 / `hu.py audio eq BAND DB`.

## Summary

- **The EQ is computed by the DSP, not the CPU.** eCos writes a per-channel list of 20 filter descriptors (type, gain in 0.5 dB, Q code, frequency in Hz) into a DSP parameter area. No biquad coefficients are computed on the CPU, and no mailbox command follows the write.
- **The parameter area is the one eCos calls "App"** (`DSP_GetAppParam` 0x3bd80a, pointer RAM 0x6e05a4; `audio_dsp.md` calls it the "id 0x27 area"): `data + 128 * bswap16(info[+0x36])`, 0x500 bytes = 0x280 halfwords, each halfword stored big-endian for the DSP. Naming correction for `audio_dsp.md`: what that note calls "App" (data + 0x180) is eCos' **RiscApp** block (`DSP_GetRiscAppParam` 0x3bd876, pointer 0x6e05a8); master volume, loudness and balance/fader live there and are followed by command 5/4.
- **Per channel: 20 slots × 2 halfwords = 0x28 halfwords.** Channel mask 1/2/4/8/0x10/0x20 selects halfword base 0x00/0x28/0x50/0x78/0xA0/0xC8. Slot k occupies halfwords base + 2k (word0) and base + 2k + 1 (word1).
- **Encoding:** word0 = `(gain_halfdB & 0x7f) << 9 | type << 6 | code6`, word1 = `freq_Hz (15 bits, ≤ 20000) | flag << 15`. Type 3 = peaking (code6 = round(32 / Q), Q 0.5..10); type 4 = low-pass, type 5 = high-pass (code6 = slope and family bits); freq 0 = slot off. Gain is clamped to ±24 half-dB = ±12 dB.
- **The stock UI EQ:** 16 peaking filters (slots 0..15) at fixed 32, 64, 256, 1k, 2k, 3k, 4k, 5k, 6k, 8k, 10k, 12k, 14k, 16k, 18k, 20k Hz, all Q = 2.0, written to the four speaker channels (masks 1, 2, 4, 8; left struct for 1 and 4, right struct for 2 and 8). The UI shows 30 sliders (−8..+8 dB, 1 dB steps); 15 of them sit on filter frequencies and the other 15 are split into the neighboring filters. Slots 16..19 are bass boost, speaker-size/subwoofer filter, LPF and HPF.
- **Tone controls:** bass and treble exist in the UI and its config, but this build never applies them (the apply hook 0x479e72 is an empty function). Balance/fader are RiscApp 0x3E/0x3F (dB codes, then 5/4). Loudness is RiscApp 0x3C (then 5/4). Per-channel delay is RiscApp 0x57..0x5C (`audio_dsp.md`).
- **The DSP also has an unused 10-band-per-channel tone/PEQ block in RiscApp** (halfwords 0..59, frequency and Q by table index, followed by 5/4). Only shell test code writes it.

## 1. Where the EQ data goes (the write path)

### 1.1 Call chain from a slider move

Proven by code and by the trace (callers 0x345240/0x345254/0x345266/0x34527a for slider moves; 0x3453d2..0x34541a for slider 0; 0x345584..0x3455cc for a whole-preset apply):

1. Touch handler `0x35909c(slider 0..9, y)`. If the current preset (RAM 0x6dd5fc) is not 6 (User), it first copies the current preset's row into User (`0x345420(6, cur)`), restores the edited preset's row from flash (`0x34545a`) and switches to User (`0x35904c(0x13)`, 0x13 − 13 = 6, which reapplies the whole preset).
2. Slider value: `step = ((bottom − y) << 4) / height` = 0..16; `0x3454c8(preset, page*10 + slider, 2*step)` stores `row[idx] = step − 8` (signed byte, −8..+8 dB). Page = RAM byte 0x6dda08 (0..2), so the 30 sliders show 10 per page. (`0x34549c` is the reverse: returns `(row[idx] + 8) * 2`.)
3. `0x345356(preset, idx)`: idx 0 → slot 0 gain = row[0] directly (float store at 0x71f2b0 / 0x71f4e0) and the four id 0x14 calls; odd idx → `0x345100(preset, idx)`; even idx ≥ 2 → `0x345100(preset, idx − 1)` and `0x345100(preset, idx + 1)`.
4. `0x345100(preset, k)` (k odd) computes the slot gain from three slider values (formula in section 2.2), clamps it to [−8.0, +8.0] dB (floats 0xC1000000 / 0x41000000, 0x3451e2..0x345218), stores it as a float into slot `(k+1)/2` of both RAM structs (`0x71f2a8 + 0x1c*b + 8` and `+0x230`), then calls the effect dispatcher four times: `0x3ab6b8(0x14, 1, 0x71f2a8)`, `(0x14, 2, 0x71f4d8)`, `(0x14, 4, 0x71f2a8)`, `(0x14, 8, 0x71f4d8)`. The trace's r3 (0x7f..0x82 or 0) is a stale register, not an argument: the dispatcher only uses r0..r2.
5. `0x3ab6b8` (33-entry jump table at 0x3ab6dc, case 0x14 at 0x3ab910) calls `0x3af614(mask, struct)`.
6. `0x3af614` encodes all 20 slots of the struct into a local 0x28-halfword buffer (section 1.3) and copies it with `0x3bd296(base, buf)`: `for i in 0..0x27: area[base + i] = bswap16(buf[i])`, where `area = *(u16 **)0x6e05a4` and `base = 0x676ab0[mask − 2]` (byte table: mask 1 → 0, 2 → 0x28, 4 → 0x50, 8 → 0x78, 0x10 → 0xA0, 0x20 → 0xC8; anything else → 0). With mask 0xFFFF the same buffer goes to all six bases (slots of type 6 are refilled per channel from that channel's current values first).
7. **Nothing else.** `0x3af614` returns without calling `DSP_SendCmd`, and neither do the callers. This matches the trace: the setters 0x3bd2ca/0x3bd312/0x3bd336 and `DSP_SendCmd` were never hit during EQ changes, because `0x3bd296` is a separate block-copy routine the hook did not cover. Inference: the DSP system code re-reads this area on its own (every block, or on change). There is no flag, checksum or counter in the area for it to key on.

Re-verified 2026-11-02 (second pass), instruction by instruction:

- Dispatcher `0x3ab6b8`: `slti r15, r0, #0x21` (ids 0..0x20), `r6 = r1` (mask), `r1 = r2` (struct), jump table 0x3ab6dc; case 0x14 at 0x3ab910 is `r0 = r6; call 0x3af614; j 0x3ab9e8` (common return). No lock, no command, r3 unused.
- `0x3af614` calls only `memset` 0x3016e8, the float helpers (0x606aee, 0x30365c, 0x302f40, 0x302ea8, 0x302b34, 0x305a94, 0x303bb8, 0x6062a2, 0x3057d8), the App getter `0x3bd80a` (type-6 slots) and the block copy `0x3bd296`. Its only stores outside its stack frame are the ones inside `0x3bd296`.
- `0x3bd296(base, buf)`: `if (base >= 0x259) return 4; p = *(u16 **)0x6e05a4 + base; for (i = 0; i < 0x28; i++) p[i] = wsbh(buf[i]); return 0;` (0x3bd29c..0x3bd2c4). Plain halfword stores, no register access, no barrier.
- Trace numbers (`audio_trace/ecos_eq_calls.txt`): 2043 dispatcher calls from tick 94529 to 131874 (about 37 s of slider and preset changes; 462 bursts from 0x345100, 43 from slider 0, 6 whole-preset applies), with `DSP_SendCmd` and the RiscApp setter hooked and silent throughout. So the stock EQ changes with no notification at all, as far as the CPU is concerned.
- Quirk (harmless for us): the encoder's local buffer is 0x50 bytes but it clears only 0x28 bytes (`0x3016e8(buf, 0, 0x28)` at 0x3af634), so with exactly one extra crossover section the copy writes stack garbage into slot 19.

### 1.2 Where the area is in DSP memory

- Proof of the pointer: `0x3bd174` calls `0x3bca04(0x27)` (area lookup, jump table 0x3bca24; case 0x27 at 0x3bcca2 loads `info = *(0x6e059c)`, `hw = info[+0x36]`, then 0x3bd134..0x3bd16c returns `hw ? *(u32 *)0x6e05b0 + 128 * bswap16(hw) : 0`), stores the result to 0x6e05a4 and zeroes 0x500 bytes. RAM 0x6e05b0 and 0x6e05ac both hold the data base (0x3bd69e, 0x3bd6a8) and 0x6e05a8 holds data + 0x180 (0x3bd6b0). The base is an uncached alias (`0x3bd5ce` ORs 0x40000000), so eCos needs no cache maintenance; our `audio.c` accesses DSP memory cached and must `dc_wb` after the write.
- `area = data + 128 * bswap16(*(u16 *)(data + 0x280 + 0x36))`, size 0x500 bytes (0x280 halfwords; setter bound check 0x281 in 0x3bd2ca, 0x259 in 0x3bd296). `data` = DSP data base (0x9C001764, live 0x0136BD00); the info table at data + 0x280 is written by the DSP during boot (`audio_dsp.md` section 2). eCos stores the pointer in RAM 0x6e05a4 and zeroes the whole area right after boot (`0x3bd174`).
- Halfword i lives at byte `area + 2*i` and is stored big-endian (eCos does `wsbh` then a little-endian store).
- EQ layout inside the area, per channel c with base B(c) ∈ {0x00, 0x28, 0x50, 0x78, 0xA0, 0xC8}: slot k word0 at halfword `B(c) + 2k`, word1 at `B(c) + 2k + 1`, k = 0..19. Halfwords 0xF0..0x27F are not used by the EQ.
- Channel identity (inference): masks 1/2/4/8 are front-left, front-right, rear-left, rear-right (left struct goes to 1 and 4, right struct to 2 and 8; balance code treats the low byte as left), 0x10 and 0x20 are two extra outputs (0x20 gets a 70 Hz low-pass when "subwoofer on", section 4.3). The same masks map to delay params RiscApp 0x57..0x5C.

### 1.3 Slot struct (CPU side) and encoding (`0x3af614`)

RAM structs: left 0x71f2a8, right 0x71f4d8 (= left + 0x230), each 20 slots × 0x1C bytes:

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | s32 | filter type: 0 = off/none (inference), 1, 2 (not used by eCos; inference: low/high shelf), 3 = peaking, 4 = low-pass, 5 = high-pass, 6 = keep the slot's current DSP value. ≥ 7 is an error |
| +0x04 | u16 | frequency in Hz (`& 0x7fff`, then `min(f, 20000)`); 0 = slot off (the getter 0x3af934 skips it) |
| +0x08 | float | gain in dB |
| +0x0C | float | Q (type 3 only), valid 0.5..10 |
| +0x10 | s32 | types 4/5: filter family bit (inference: 0 = Butterworth, 1 = Linkwitz-Riley or similar) |
| +0x14 | s32 | types 4/5: slope 0..3 = 6/12/18/24 dB/oct (the crossover setter 0x3afdc6 maps 6/12/18/24 to 0/1/2/3) |
| +0x18 | s32 | types 0/4/5: 1 sets word1 bit 15 (0x3afdc6 sets it for phase = 180°, so inference: phase invert) |

Encoding, exact (0x3af67a..0x3af7aa):

```c
gain_hd = (int)ldexpf(gain_dB, 1);          // 0x606aee = scalbnf, 0x30365c = float->int, truncates toward 0
gain_hd = clamp(gain_hd, -24, +24);         // ±12 dB in 0.5 dB steps
if (type == 3) {                             // peaking
    if (Q < 0.5f)       code = 0x3f;
    else if (Q > 10.0f) code = 3;            // round(32/10)
    else { code = (int)floor(32.0 / Q + 0.5); if (code == 0x40) code = 0x3f; }
} else if (type == 4 || type == 5) {
    code = (family << 3) | slope;            // +0x10, +0x14
} else code = 0;
flag = (type == 0 || type == 4 || type == 5) && s18 == 1;
word0 = (u16)(gain_hd << 9 | type << 6 | code);
word1 = (u16)(min(freq & 0x7fff, 20000) | flag << 15);
```

Types 4/5 with slope ≥ 2 (two biquads, inference) also write a second section, `word0 | 4` plus the same word1, into slot 18 then 19 (at most two), and then stop encoding at slot 18. This only happens for slots below 18; for slots 18/19 the extra section is refused and the function returns −8, but the slot itself is still written. The DSP decodes Q as `32 / code` (the getter 0x3af934 computes exactly that, clamped to [0.5, 10]), so for a peaking filter code = 32/Q: Q 2.0 → 16, Q 10 → 3, Q 0.5 → 63.

Examples (DSP-view halfword values; write them big-endian):

| Filter | word0 | word1 |
|---|---|---|
| peaking 1000 Hz, +3 dB, Q 2 | 0x0CD0 | 0x03E8 |
| peaking 1000 Hz, −8 dB, Q 2 | 0xE0D0 | 0x03E8 |
| peaking 32 Hz, 0 dB, Q 2 (stock default slot 0) | 0x00D0 | 0x0020 |
| bass boost, 150 Hz, +15 dB → clamped +12 dB, Q 10 | 0x30C3 | 0x0096 |
| LPF slot as written by `0x47a544` (off, freq 0) | 0xD302 | 0x8000 |
| HPF slot as written by `0x47a544` (off, freq 0) | 0xD34B | 0x0000 |

## 2. The stock UI EQ model

### 2.1 Filter bank (boot defaults, `0x3ac5d2`, called from `AUDIF_Init` 0x3b1618)

Both RAM structs, slots 0..15: type 3, Q 2.0 (0x40000000), gain 0, +0x10/+0x14/+0x18 = 0. Frequencies (only writer of +0x04 for these slots in the whole image):

| Slot | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Hz | 32 | 64 | 256 | 1000 | 2000 | 3000 | 4000 | 5000 | 6000 | 8000 | 10000 | 12000 | 14000 | 16000 | 18000 | 20000 |

Slots 16..19: type 6 (keep) at this point. `0x3ac5d2` applies masks 1 and 2 only; the later boot steps and every preset apply write masks 1, 2, 4, 8.

### 2.2 Thirty sliders onto sixteen filters

Slider labels (pointer table 0x980bbec4): 32, 64, 128, 256, 512, 1K, 1.5k, 2k, 2.5k, 3k, 3.5k, 4k, 4.5k, 5k, 5.5k, 6k, 7k, 8k, 9k, 10k, 11k, 12k, 13k, 14k, 15k, 16k, 17k, 18k, 19k, 20k (s0..s29). Odd sliders and s0 sit on a filter; even sliders are interpolation points that feed both neighbors through lookup tables of 17 floats indexed by `s + 8`:

- T1 (0x98018780) = 0.5·s: −4.0 … +4.0 in 0.5 steps.
- T2 (0x980187c4) = s: −8 … +8.
- T3 (0x98018808): −7, −6.5, −5.5, −5, −4, −3, −2, −1, 0, 1, 2, 3, 4, 5, 5.5, 6.5, 7.
- T4 (0x9801884c): −5, −4.5, −4, −3.5, −2.5, −2, −1, −0.5, 0, 0.5, 1, 2, 2.5, 3.5, 4, 4.5, 5.

Slot gain (dB) before the ±8 clamp (`0x345100`, table selection at 0x34518a..0x345308, additions by `__addsf3` 0x3027cc):

| Slot (Hz) | Gain |
|---|---|
| 0 (32) | s0 (no clamp, set directly by 0x3453a0 / 0x345554) |
| 1 (64) | s1 + s2 |
| 2 (256) | s3 + s2 + s4 |
| 3 (1000) | s5 + s4 + T3(s6) |
| 4 (2000) | s7 + T3(s6) + T4(s8) |
| 5 (3000) | s9 + T4(s8) + 0.5·s10 |
| 6..14 (4k, 5k, 6k, 8k, 10k, 12k, 14k, 16k, 18k) | s(2b−1) + 0.5·s(2b−2) + 0.5·s(2b) |
| 15 (20000) | s29 + 0.5·s28 |

The result goes through the encoder above (0.5 dB resolution, ±12 dB DSP limit; the UI path never exceeds ±8 dB). For our own firmware the owner's presets can skip the 30-slider layer entirely and set the 16 slot gains (or any frequencies and Q) directly.

### 2.3 Presets (low priority)

Seven rows of 30 signed bytes (one per slider, dB) in RAM 0x6dd528 (row r at 0x6dd528 + 30·r), defaults copied from flash 0x980706a8 (210 bytes) by `0x34532e`. Current preset index: RAM 0x6dd5fc (0..6, setter 0x34530a, getter 0x34531e). Names (string IDs at 0x98070698, English string table 0x983e1a50): 0 Standard, 1 Classic, 2 Rock, 3 Pop, 4 Jazz, 5 Electric, 6 User. Standard and User default to all zeros. Flash rows (s0..s29):

- 1 Classic: 0 0 0 0 0 −1 0 −1 0 −1 0 −1 −2 −1 0 −1 −2 −2 0 −2 −2 −2 0 −2 −2 −2 −2 −2 −2 −3
- 2 Rock: 3 3 0 0 0 −2 0 0 0 1 0 1 2 1 2 2 2 2 2 2 2 3 2 3 2 4 2 3 4 4
- 3 Pop: −1 −1 0 0 0 3 2 1 0 0 0 0 0 −1 0 −1 0 −1 0 −1 0 −1 0 −1 0 −1 0 −1 0 −1
- 4 Jazz: 6 4 2 2 1 0 1 1 0 0 0 −1 0 −1 −2 −2 0 0 0 0 0 0 0 1 0 1 0 1 0 1
- 5 Electric: −2 −2 0 −1 −1 −3 −1 −2 −2 −1 −2 −1 −2 0 0 −1 0 1 0 1 0 1 2 1 2 2 2 2 2 3

Whole-preset apply `0x3454ee`: for idx 0..29 the same per-slider logic as `0x345356` (so most slots are written several times; harmless). Selecting a preset from the UI: `0x35904c(button)` → `0x34530a(button − 13)` → redraw → `0x3454ee`.

## 3. Other effects that share the slot list

| Slot | Use | Writer | Values |
|---|---|---|---|
| 16 | bass boost ("VB", `set_nFc_value` / `set_vb_gain`) | init `0x479fae` (type 3, freq 0 = off, gain 6.0, Q 10.0, masks 1/2/4/8); `0x479cca(freq)` sets +0x04; `0x479d56(gain_int)` sets +0x08; toggle `0x47a0d6` | settings: freq RAM 0x6e7f2c (default 150 Hz, `0x47a14a`), gain 0x6e7f28 (default 15 dB, which the encoder clamps to 12 dB), on/off 0x6e7f30. Off = freq 0 and gain 0 |
| 17 | speaker size / subwoofer | `0x465e46`: front speakers "small" = HPF (type 5) 80 Hz, slope 1 (12 dB/oct) on masks 1 and 2; "large" = HPF with freq 0 (off). `0x465d18` subwoofer on/off: LPF (type 4) 70 Hz, slope 1 on mask 0x20, plus output-enable bit 5 via `AUDIF_Set(0x49/0x35)`. `0x465f18`: same HPF for mask 0x10 | |
| 18 | low-pass, all six channels (mask 0xFFFF) | `0x47a6a2` (toggle 0x47a74c), `0x47a7c8`, `0x479e7a` | type 4, slope 2, family 0, flag 1, gain −11.5 dB; cutoff RAM 0x6e7f40 (default 20000, `0x47a1a4`), on/off 0x6e7f3c; off = freq 0. `0x479e7a` uses 800 Hz when byte 0x6e7f25 == 1, else 20000 |
| 19 | high-pass, all six channels | `0x47a5bc` (toggle 0x47a666), `0x47a7c8` | type 5, slope 3, family 1, gain −11.5 dB; cutoff RAM 0x6e7f44 (default 20 Hz), on/off 0x6e7f3d; off = freq 0 |
| 10/11 | crossover (shell only) | `0x3afdc6` (dispatcher id 0x0B): writes slot 10 of each channel with type 4 or 5, slope from 6/12/18/24 dB/oct, frequency from the 64-entry table below, phase flag, and a second section in slot 11 for slopes ≥ 18 dB/oct. No UI caller, and it would overwrite EQ slots 10/11 | |

## 4. Tone controls outside the slot list (RiscApp block, always followed by 5/4)

All of these use `0x3bd336` (RiscApp setter: `*(u16 *)(data + 0x180 + 2*idx) = bswap16(v)`) and then `DSP_SendCmd(5, 4, 0, 0)`.

### 4.1 Balance and fader: RiscApp 0x3E and 0x3F

`0x3aa422(a, b)` (checks −20 ≤ a, b ≤ 20) → `0x3aa396(a, b)`; the UI calls it as `0x479e4c`: `a = −fade` (RAM s8 0x6e7f21), `b = balance` (s8 0x6e7f20). Exact computation:

```c
L = b > 0 ? b : 0;  R = b > 0 ? 0 : -b;           // balance: b > 0 attenuates the low byte
if (a > 0) { L2 = L + a; R2 = R + a; }            // a > 0 attenuates the 0x3F pair
else       { L2 = L; R2 = R; L -= a; R -= a; }    // a <= 0 attenuates the 0x3E pair by -a
RiscApp[0x3E] = tbl[min(R,20)] << 8 | tbl[min(L,20)];
RiscApp[0x3F] = tbl[min(R2,20)] << 8 | tbl[min(L2,20)];
DSP_SendCmd(5, 4, 0, 0);
```

`tbl` = 0x67669c, 21 gain codes (dB = code/2 − 103.5, same code as master volume): 207, 204, 200, 195, 190, 185, 180, 175, 170, 165, 160, 150, 140, 130, 120, 110, 100, 90, 70, 50, 30 = 0, −1.5, −3.5, −6, −8.5, −11, −13.5, −16, −18.5, −21, −23.5, −28.5, −33.5, −38.5, −43.5, −48.5, −53.5, −58.5, −68.5, −78.5, −88.5 dB. Boot default 0xCFCF in both (`0x3bd35a`). The UI labels balance > 0 as "R" and fader > 0 as "R" (rear), fader < 0 as "F" (`0x34c800`), so (inference) 0x3E = front pair, 0x3F = rear pair, low byte = left, high byte = right. UI range ±20 for balance/fader (`0x34c96c`, max in RAM 0x6dd810). Note the previous description of 0x3E/0x3F as loudness in `audio_dsp.md` is wrong; they are balance/fader.

### 4.2 Loudness: RiscApp 0x3C

Bits (writers 0x3aa29c, 0x3aa2e6, 0x3aa34c, id 8 0x3afc58, id 0x16 0x3afd08, volume 0x3b3810): bits 1:0 = center frequency select 400 / 800 / 2400 Hz → 0 / 1 / 2; bit 2 = a second switch (id 8 field 1; meaning unknown); bit 3 = loudness on (`0x3aa29c(on)`); bit 5 = set by `0x3afd08` (inference: "table mode": the volume setter only refreshes bits 12:8 when `(0x3C & 0x30) == 0x20`); bits 12:8 = boost level. `0x3afd08` copies a 0x68-byte per-volume-code level table to RAM 0x6e0430 and picks `table[0x67 − min(vol_code, 0xCE)/2]`; the UI fills every entry with RAM byte 0x6e7f37 (default 12; `0x47a468` / `0x479ef6`), so the level does not track volume in practice. UI state: on/off 0x6e7f26, frequency choice 0x6e7f38 (0/1/2 → 400/800/2400, `0x47a434`). Boot trace value 0x0C20 = level 12, table mode, off, 400 Hz.

### 4.3 Bass and treble

The audio-settings page keeps balance, fade, bass and treble in RAM s8 0x6dd814 / 0x6dd813 / 0x6dd812 / 0x6dd811 (printed by `0x34c6d2`: "balanceValue", "fadeValue", "bassValue", "trebleValue"; range ±16 for bass/treble). On change, `0x34c89a` stores them into the effect settings (0x6e7f20 balance, 0x6e7f21 fade, 0x6e7f22 bass, 0x6e7f24 treble) and then calls `0x479e4c` (balance/fade apply) for balance/fade, but only the empty function `0x479e72` (`push25; pop25`) for bass/treble. **Bass and treble have no audible effect in this firmware build.**

### 4.4 Unused DSP tone/PEQ block (RiscApp 0..59)

RiscApp halfwords 10c .. 10c+9 per channel c (mask 1/2/4/8/0x10/0x20 → c = 0..5) hold a second, table-driven filter set: word = `gain_halfdB (6-bit signed) << 10 | freq_idx << 4 | q_idx`. Writers: id 2 (`0x3af3ec`) for the 8 middle words 10c+1..10c+8, id 6 (`0x3afa78`) for words 10c and 10c+9 (inference: bass and treble shelves); both send 5/4. Boot defaults (`0x3bd35a`): 0x00B0, 0x0123, 0x01B3, 0x0203, 0x0233, 0x02C3, 0x0343, 0x0373, 0x0393, 0x03B0, all gains 0. Frequency index table 0x676c04 (64 × u32, Hz): 0, 20, 22, 25, 28, 32, 36, 40, 45, 50, 57, 63, 71, 80, 90, 101, 113, 127, 143, 160, 180, 202, 226, 254, 285, 320, 359, 403, 453, 508, 570, 640, 718, 806, 905, 1016, 1140, 1280, 1437, 1613, 1810, 2032, 2281, 2560, 2874, 3225, 3620, 4064, 4561, 5120, 5747, 6451, 7241, 8127, 9123, 10240, 11494, 12902, 14482, 16255, 18246, 20480. Q table 0x676d04 (floats): 0.5, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 7, 8, 9, 10. So the defaults are 63 Hz and 18.2 kHz for the end words and 127, 359, 640, 905, 2874, 7241, 10240, 12902 Hz at Q 2 for the middle ones. Only the shell (`0x3b52ec`) calls ids 2/6, so whether the DSP actually runs this block is untested. The slot list (section 1) is the proven path.

### 4.5 Effect dispatcher ids touched here (`0x3ab6b8`, table 0x3ab6dc)

| Id | Target | Notes |
|---|---|---|
| 0x02 / 0x06 | `0x3af3e8` / `0x3afa78` | RiscApp tone/PEQ block (section 4.4), 5/4 |
| 0x08 / 0x16 | `0x3afc58` / `0x3afd04` | loudness RiscApp 0x3C (section 4.2) |
| 0x0B | `0x3afdc6` | crossover into slots 10/11 (shell only) |
| 0x0E | `0x3aff88` | per-channel delay |
| 0x12 / 0x13 | `0x3af5a0` | **surround, not EQ**: r1 = 1 sets RiscApp 0x55 bit 15, 0 clears it, 2 writes RiscApp 0x55 = struct u16[1]; no 5/4. Only caller is the shell's `surr` command (0x3b54f6). Boot value 0 (`0x3bd35a`, A.H(0xaa) = 0), so stock runs with it off; it is not an EQ enable |
| 0x14 | `0x3af614` | slot-list write (section 1) |
| 0x15 | `0x3af934` | slot-list read back into a struct (shell only) |

## 5. Where eCos keeps the settings

- EQ rows and preset: RAM 0x6dd528 (7 × 30 s8) and 0x6dd5fc. Saved by `0x3455d4` through the settings store (`0x32bbfc(handle 0x55e35e(), buf, 0xDC)`) as `[u32 ~sum(bytes 4..0xDB)][u32 preset][210 bytes rows][pad]`; loaded and checksum-checked by `0x345682` ("EQ check sum error", falls back to flash defaults and preset 0). `0x3457ae` = load + apply, called from the power-on path 0x3372ec; `0x345764` = reset to defaults + apply + save.
- Filter state actually sent: RAM structs 0x71f2a8 / 0x71f4d8 (section 1.3).
- Effect settings block RAM 0x6e7f20: +0 balance, +1 fade, +2 bass, +4 treble, +5 LPF-800 switch, +6 loudness on, +8 VB gain, +0xC VB freq, +0x10 VB on, +0x11 a toggle, +0x12 listening position (0..5, `0x47a2f6`, sets per-channel delays of 0 or 10 ms through id 0x0E), +0x13..+0x16 per-channel delays (0..20 ms, masks 1/2/4/8), +0x17 loudness level, +0x18 loudness frequency choice, +0x1C LPF on, +0x1D HPF on, +0x20 LPF cutoff, +0x24 HPF cutoff, +0x28 "restore effects" flag (`0x47a858` reapplies VB, loudness, LPF/HPF when set).
- Boot order of the DSP-side defaults: `AUDIF_Init` → `0x3ac5d2` (filter bank, masks 1/2) → `0x34532e` (preset rows from flash) → `0x479fae` (slot 16) → `0x47a544` (slots 18/19 off, all channels). The user's preset is applied later at power-on (`0x3457ae`).

## 6. Reproduce in our firmware

Preconditions: DSP running as in `audio_dsp.md`, `data` and the info table at data + 0x280 known. Written against the helpers already in `isp/sd-10-net/audio.c` (`data`, `info()`, `dc_wb()`), which access DSP memory through the cache (eCos uses the uncached alias instead).

```c
// eCos "App" area (id 0x27): 0x280 halfwords, stored byte-swapped (DSP is big-endian)
static u32 eq_area(void) { u16 o = info(0x36); return o ? data + 128u * o : 0; }   // 0x3bca04(0x27)
static const u8 eq_base[6] = { 0x00, 0x28, 0x50, 0x78, 0xA0, 0xC8 };               // masks 1, 2, 4, 8, 0x10, 0x20 (0x676ab0)

enum { EQ_OFF = 0, EQ_PEAK = 3, EQ_LPF = 4, EQ_HPF = 5 };

// one filter slot; for EQ_LPF/EQ_HPF, code = family << 3 | slope (slope 0..3 = 6/12/18/24 dB/oct, inference)
static void eq_slot(int ch, int slot, int type, unsigned hz, float gain_db, float q, unsigned code, int flag)
{
    u32 A = eq_area();
    if (!A || ch < 0 || ch > 5 || slot < 0 || slot > 19) return;
    int g = (int)(gain_db * 2.0f);                     // ldexpf(x, 1) then truncate toward 0 (0x3af69e..0x3af6aa)
    if (g > 24) g = 24;
    if (g < -24) g = -24;                              // +-12 dB in 0.5 dB steps (0x3af6be..0x3af6d6)
    if (type == EQ_PEAK) {                             // 0x3af6d8..0x3af75a
        if (q < 0.5f) code = 0x3f;
        else if (q > 10.0f) code = 3;
        else { code = (unsigned)(32.0f / q + 0.5f); if (code == 0x40) code = 0x3f; }
    } else if (type != EQ_LPF && type != EQ_HPF) code = 0;
    if (hz > 20000) hz = 20000;
    u16 w0 = (u16)(((u32)g & 0x7f) << 9 | (u32)type << 6 | (code & 0x3f));
    u16 w1 = (u16)(hz | (flag && (type == EQ_OFF || type == EQ_LPF || type == EQ_HPF) ? 0x8000u : 0));
    u32 a = A + 2u * (eq_base[ch] + 2u * slot);        // 4-byte aligned: write both halfwords in one store
    *(volatile u32 *)a = (u32)bswap16(w1) << 16 | bswap16(w0);
    dc_wb(a, 4);                                       // eCos: nothing else, no mailbox command
}

// Stock-equivalent bank: 16 peaking filters at Q 2 on the four speaker channels
static const u16 eq_hz[16] = { 32, 64, 256, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 14000, 16000, 18000, 20000 };
void eq_apply(const float gain_db[16])                 // one gain per filter, +-12 dB, 0.5 dB steps
{
    for (int ch = 0; ch < 4; ch++)
        for (int b = 0; b < 16; b++) eq_slot(ch, b, EQ_PEAK, eq_hz[b], gain_db[b], 2.0f, 0, 0);
    // slots 16..19 stay 0 (type 0, freq 0 = off): what eCos has with bass boost, LPF and HPF off
}
```

- The 32-bit store is our choice, not eCos' (it writes two halfwords): it keeps the DSP from ever seeing a new word0 with an old word1. Byte offset `2 * (base + 2k)` is a multiple of 4 and the area is 128-byte aligned, so the store is aligned.
- Equivalent dB values in the encoded form, for reference: word0 = `(u16)(2*dB) << 9 | 3 << 6 | 32/Q`, word1 = Hz. Because the DSP does the filter design from (type, gain, Q, f), **we never compute biquad coefficients**. The DSP's internal filter math (RBJ cookbook or otherwise) and its coefficient format are not visible from the CPU side; only the descriptor format above is.
- Path caveat (inference): `audio.c` notes that master volume and EQ apply to stream 1 (the PCM decoder path) and not to ring 10 ("RISC sound"). The slot list is presumably processed in the same post-decoder chain, so EQ tests should use stream 1.
- **No mailbox command** is sent by eCos after these writes. If a first test shows no audible change, the open question below applies; the cheap experiments are `DSP_SendCmd(5, 4, 0, 0)` and `DSP_SendCmd(5, 2, 0, 0)` after the write (both harmless: they only tell the DSP that the RiscApp/RISC blocks changed).
- Zero the whole area once after the DSP boots, as eCos does (`0x3bd174`), so unused slots read as off. `audio.c` already does this (the `o27` block after DSP start).
- Bass boost as in stock: `eq_slot(ch, 16, 3, 150, 12.0f, 10.0f, 0, 0)` for ch 0..3; off = freq 0.
- LPF / HPF on all six channels as in stock: `eq_slot(ch, 18, 4, cutoff, -11.5f, 0, 2, 1)` and `eq_slot(ch, 19, 5, cutoff, -11.5f, 0, (1 << 3) | 3, 0)`; off = freq 0.
- Balance/fader: `RiscApp[0x3E]`, `RiscApp[0x3F]` per section 4.1, then 5/4. Loudness: `RiscApp[0x3C]` per section 4.2, then 5/4. Delay: RiscApp 0x57..0x5C = `fs * ms / 1000` samples (0..20 ms), then 5/4.

## 7. Open questions

- **How the DSP picks up slot-list changes.** No command, flag or counter is written (re-verified: the whole write path is plain halfword stores, and 37 s of live EQ changes produced zero `DSP_SendCmd` calls); inference: the system microcode re-reads the area continuously or on change. Untested on our firmware. Suggested first live check, on stream 1 with a test tone: set a 1 kHz tone, write slot 3 of channels 0..3 to −12 dB (word0 0xD0D0, word1 0x03E8) and listen; if nothing changes, retry with `send_cmd(5, 4, 0, 0)` and then `send_cmd(5, 2, 0, 0)` after the write. The answer could also sit in the codec 0x00 microcode, which nobody has disassembled (DSP ISA unknown).
- Whether some stock-only state (set by a later boot step we do not replicate) gates the slot-list processing. The only effect-enable bit found (RiscApp 0x55 bit 15, dispatcher ids 0x12/0x13) is surround and is off on stock, so no EQ enable is known; a silent first test would point here.
- Types 1 and 2 (no eCos writer) and type 0 with the flag bit: inferred as shelves/off, unconfirmed. The gain field's meaning for types 4/5 (eCos always passes −11.5 dB) is unknown.
- The family bit (code bit 3) and slope coding for types 4/5 are inferred from the crossover setter's 6/12/18/24 dB/oct mapping.
- Channel identity of masks 1/2/4/8/0x10/0x20 is inferred (FL, FR, RL, RR, extra, subwoofer).
- Whether the RiscApp tone/PEQ block (section 4.4) is processed by the DSP at all.
