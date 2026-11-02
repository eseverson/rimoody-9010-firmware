# Audio on the SPHE8268K head unit

Status as of 2026-10-31. Static analysis only, no hardware access. Code addresses are eCos load addresses (0x301040 = start of `ecos.bin`); flash-resident strings and tables are at 0x98xxxxxx (XIP, flash offset = address − 0x98000000). Disassembly: `isp-re/ecos.ann.dis` in the earlier session's scratchpad (see memory `head-unit-context`). MCU frames were built with `tools/mcuframe.py`.

## Summary

- **Volume is not an MCU command.** It is a gain in the SoC's audio DSP: a volume index 0..40 (RAM byte 0x6d70bc) maps through a dB table to −103.5 dB (mute) … 0 dB (`0x3aa184`). This fits the earlier capture, where changing the volume put no traffic on the MCU link.
- **The only general audio command to the MCU is system mute, `01 01` / `01 00`** (`0x31dec6`). The MCU almost certainly drives the amplifier's mute line, since this board has no SoC mute GPIO (see below).
- **`d8 <ch> <gain>`** (`SendMcuAudioch` 0x31dfc8, `SendMcuAudioCh_Gain` 0x31e026) is sent only by the Bluetooth-audio ("btap") route switch `0x3855f2`. It selects the AC6965A's audio channel and gain (1..25). It has never shown up in a capture.
- **`ee <level>` is the TFT backlight, not audio** (0x472fbc / 0x473038, `tft_backlight_on/off`; `ee ff` = off). This corrects the earlier guess that `ee` was an audio-path command.
- **SoC audio is a DSP subsystem.** It has a separate audio DSP core whose microcode eCos downloads (`DSP_InitSystem` 0x3bb996, `DownloadCode` 0x3bafe0), an internal ADC with an input mux (radio / AUX / Bluetooth analog in), and DAC/I2S outputs. The registers are at 0x9c001700–0x9c0022ff. No I2C audio processor or codec driver exists in eCos; the only I2C audio-adjacent drivers are for the tuner.
- **Amplifier:** the board is marked `7388`, which is TDA7388-class (the sibling FCC unit used a `YD7388S`). TDA7388 control pins are ST-BY (pin 4) and MUTE (pin 22). eCos never names them. Candidates: MCU-driven mute (`01 xx`), and SoC **GPIO13**, which eCos sets high in `power_on` 200 ms after muting and drives low in `power_off`/standby. That GPIO13 is the amp's ST-BY is medium/low confidence and needs a continuity check.
- **Easiest way to a tone from our own code:** run inside stock eCos and push PCM through its `AUDIF_Track_*` API, the same API Android Auto audio uses (sequence below). Bare-metal audio would mean reproducing the DSP microcode download and setup. That is possible but is a large job.

## SoC → MCU commands (complete list from eCos, audio relevance marked)

Every command goes through `0x31d6d4`, which calls the McuCmd service 0x32c424, then the frame builder 0x521aa8, and out on UART1 (0x9c000980). Frames are `ff aa LEN payload CHK8 CRC16be`.

| Payload | Sender (eCos) | Meaning | Audio? | Triggered by |
|---|---|---|---|---|
| `01 01` / `01 00` | `0x31dec6` (system mute, deduplicated through RAM 0x6bd275), `0x31de98` (raw send, no checks) | system mute on / off | **yes** | Mute: power_off 0x47acbc, MCU shutdown request 0x31e510, standby path 0x337116, mute key (`SYSTEM_MUTE_ON` 0x49354a), source changes, tuner retune, EQ/setting writes 0x34c1bc..0x34c2cc, reversing/park mute. Unmute: `0x473fd4` (the source audio-ready path) sends `01 00` 40 ms after a source starts, but only if volume > 0, user mute (0x6d70e7) is off and 0x71e5b8 == 0. power_on 0x47af0a sends a raw `01 01`, waits 200 ms, then enables GPIO13 |
| `d8 <ch> 19` | `SendMcuAudioch` 0x31dfc8 | select MCU audio channel (`ch` cached at 0x6d70ef), gain fixed at 0x19 | yes (BT route) | `btap_audio_switch(0)` 0x3855f2, 0x385c4e |
| `d8 <ch> <g>` | `SendMcuAudioCh_Gain` 0x31e026 | channel plus gain, `g = table[vol]`, table at 0x9806bb30: vol 0..40 → `01 01 02 02 … 18 18 19 19 19` (1..25) | yes (BT route) | `btap_audio_switch(1)`, EQ/setting writes 0x34c2ec / 0x34c3ca / 0x34c5b0, 0x3846f0, and a delayed resend from the MCU service loop 0x31e948 (counter 0x6dce25) |
| `d1 xx` | 0x31d738 (`d1 00`), 0x31d760 (`d1 <status>`, rate-limited to 2 s) | heartbeat | no | MCU service loop. **Required**, or the MCU cuts power about 9 s after boot |
| `11 01` / `11 00` | 0x31d98a | system up / standby | indirectly (`11 00` is part of shutdown) | after the a4 handshake (0x31e2a6), power_on, power_off |
| `e6 00` | 0x31db54 | unknown, sent only once, in power_on | unknown | power_on 0x47af0a |
| `e8 01` | 0x31d88a | shutdown acknowledge | no (follows a mute) | 0x31e510 |
| `00` | 0x31d718 | final "SoC down", after which the SoC spins forever | **unsafe** | end of 0x31e510 |
| `ee <lvl>` | 0x31d928 | TFT backlight (`ff` = off) | no | 0x472fbc, 0x473038 |
| `ed <0xff−n>` | 0x31d8b0 (table 0x9806bb5c) | unknown setting; default n = 0x73 from config byte +0x54 | not apparently | `MNSystemInit` 0x31f7f0, 0x31fa24, 0x32f4cc, 0x34d70e |
| `da <idx> <len> <data>` | 0x31de42 | sends two config bytes (struct offsets +0x5a/+0x5b) | not apparently | 0x31e0ec (handshake handler) |
| `a1 hh mm ss`, `a8 …` | 0x31d9ae, 0x31d9f8 | set time / date | no | 0x31da46 |
| `ac xx`, `ac 00 len str`, `ac a0 …` | 0x31dcf0, 0x31dd16, 0x31dd76 / 0x31ddb6 / 0x31ddf8 | Bluetooth command / passthrough (`ac 14` seen at AA exit) | Bluetooth only | BT dispatcher 0x3887d0 and phonelink code |
| `e5 xx` | 0x31dcca | `e5 01` park, `e5 03` MCU bootloader | **unsafe** | MCU upgrade 0x47539c / 0x476270 |
| `ad` | 0x31db32 | reboot request | **unsafe** | 0x32f4cc, 0x33adda, 0x4759aa |

There are no SoC→MCU commands for loudness, EQ, bass, treble, balance or fader. Those live in the DSP: the shell has `seteq`, `geteq`, `loud`, `gain`, `smute`, `PFgain`, DRC and others, with handlers at 0x3b52ec…0x3ba800. Separately, when config byte 0x6d7091 is non-zero, the EQ setters send raw 6-byte frames on a second UART handle (RAM 0x703d04, sender 0x521e4e): `51 ac 00 00 01 ck`, `51 11 ab 00 vv ck`, `51 10 ab 00 vv ck`, with `ck = ~sum`. This looks like an optional external audio module and probably isn't fitted on this unit.

### Full frames

| Purpose | Payload | Frame |
|---|---|---|
| **unmute** | `01 00` | `ff aa 02 01 00 fd e5 b2` |
| mute | `01 01` | `ff aa 02 01 01 fc aa 94` |
| "mid volume" (only meaningful on the BT route: vol index 20 → MCU gain 0x0d) | `d8 00 0d` | `ff aa 03 d8 00 0d 18 30 04` |
| BT route default (ch 0, gain 25) | `d8 00 19` | `ff aa 03 d8 00 19 0c e6 50` |
| heartbeat | `d1 00` | `ff aa 02 d1 00 2d be ba` |
| system up | `11 01` | `ff aa 02 11 01 ec 1f 1e` |
| power_on extra | `e6 00` | `ff aa 02 e6 00 18 70 0e` |

The real "mid volume" on this unit is SoC-side: index 20 = −27 dB (dB table at flash 0x5c7b64: −103.5, −70, −66, −62, −58, −54, −50, −48, −44, −42, −40, −38 … −1, 0 for indices 0..40), applied by `0x3aa184`, which clamps to [−103.5, +24] dB and passes the value to the DSP through 0x3b3810.

## SoC audio block

- **DSP mailbox:** `DSP_SendCmd(cmd, p1, p2, p3)` at 0x3bbf74 writes `cmd<<24 | p1<<16 | p2` to **0x9c001880** and `p3` to **0x9c001884**, raises bit 0 of **0x9c001784**, then polls 0x9c001880 bit 31 for the acknowledgment, with a 1 s timeout (`DSP_SendCmd TimeOut`). DSP mute is cmd 6/sub 3 and demute is cmd 6/sub 4 (`AUD_Set_Mute` 0x3b43xx, `AUD_Set_Demute` 0x3b440a). `AUD_Set_RiscSndFs` 0x3b449e accepts 48000 / 24000 / 16000 (cmd 6/8).
- **DSP bring-up:** `AUDIF_ResetDsp` 0x3aa50a calls `DSP_InitSystem` 0x3bb996. That downloads microcode banks 0, 3 and 5 with `DownloadCode` 0x3bafe0, then `DSP_SetSystemRun` 0x3bb7f6. 0x9c001794 is toggled around the reset. The microcode comes from tables inside the eCos image (`dsp_code.c` / `DSP_CheckCodec`).
- **Control dispatcher:** `AUDIF_Set(id, a, b)` at 0x3aa8a0 is a 0x65-entry jump table (table at 0x3aa8c4). IDs seen: 0x0c/0x0d (path setup, used by 0x3ab510), 0x24 (input select; the shell's `output mic` = 0x24(1,3)), 0x39 (ADC source mux, 0x3b304e), 0x52 (Bluetooth route), 0x56/0x57 (I2S format), 0x58 (DAC enable, 0x3b4ec2), 0x5a (0x9c001e14 bits 22:20).
- **DAC enable ("ID_CMD_SET_DAC_GPIO", shell `dacon` / `dacoff`):** **0x9c001e14 bits 31:30** = `11` for on, `00` for off (0x3b4ec2).
- **ADC input mux** (`AUDIF_Set(0x39, adc, src)` → 0x3b304e, wrapper 0x3ac58a): ADC0 = 0x9c001f54 bits 7:4 (L and R fields, `src` 0..3); ADC1 = 0x9c001fec bits 21:20 / 17:16. Sources used: 2 = Bluetooth/AC6965A (btap 0x3855f2), 0 = from 0x479168 (probably the tuner), 1 = from 0x474bb4 (unidentified), AUX = a configurable value (`_AUXIN_CallBack` 0x47954a). This puts the analog sources (tuner `ZT0811` in the GBT-7042 can, AUX, the AC6965A DAC) into the **SoC ADC**, with mixing and volume in the DSP.
- **Internal ADC / I2S-in switch** (`AUD_Set_I2sIn_Switch` 0x3b2f68): bits 6/7/9 of 0x9c001ffc plus 0x9c001e20 / 0x9c001e10 / 0x9c001f40.
- **External I2S** (`AUDIF_Set(0x56,…)` → 0x3b4bde): 0x9c002280 / 0x9c002288 / 0x9c00228c / 0x9c002294. The formats `AUD_Set_I2s_Format` accepts are master/slave, 16/24/32-bit, LRCK polarity, left/right justify and 1-bit delay (strings at 0x676874…). U-Boot's pinmux name table lists `pmAUD_EXT_DAC_IFX0..3`, `pmAUD_EXT_ADC_IFX0` and `pmAUD_BT_IFX`, and the UI has an "I2S Out" option. Whether this board actually uses external I2S is unknown.
- **Other audio register groups touched by the driver:** 0x9c001728–0x9c001798 (DSP control), 0x9c001880–0x9c0018f8 (mailbox and parameters), 0x9c001e08–0x9c001ec8, 0x9c001f04–0x9c001ffc, and 0x9c002094–0x9c00219c (0x9c00219c is written 0x204 or 0x2040 by the shell's `output spdif` / `output reset`).
- **PCM API** (`audio_track.c`), used by Android Auto / CarPlay (`vautolink_mw.c` 0x38dfb4 / 0x38df48 / 0x38debc):
  - `AUDIF_Track_Create_Ext` 0x3a3f8e(`"main0"`, `"main"`, &params, 0x18), with params: 0x30 bytes zeroed, then +8 = 2 (channels), +0xc = 48000, +0x10 = 16 (bits).
  - `AUDIF_Track_SetVolume` 0x3a4b68(h, 100) and `AUDIF_Track_Start` 0x3a4636(h).
  - `AUDIF_Track_Write_Blocked` 0x3a5188(h, buf, len, 1), which returns the number of bytes written.
  - `Stop` 0x3a47f4, `Destroy` 0x3a557a, `SetVolumeDB` 0x3a4d6c.
  - The driver only supports 2ch / s16 / PCM (string at 0x6761cc). `AUDIF_Track_Init` 0x3a3ef0 is called from the Audio Thread setup 0x3064b4.
- **U-Boot:** no audio initialization found (no audio strings besides the pinmux names).

## SoC GPIOs around audio and power

GPIO registers use masked writes (upper 16 bits = write-enable for the lower 16): OE at 0x9c000320 + 4·(n/16) (`gpio_e_set` 0x520414), OUT at 0x9c000340 + 4·(n/16) (`gpio_set_output` 0x5203bc), IN at 0x9c000360 + 4·(n/32). eCos looks pins up by "sysio" index in a static table at 0x6c7a64 (53 × 8 bytes: sysio, pin, pad, pad, M, F, OE, OUT; applied at boot by 0x5205c2).

| sysio → GPIO | Helper | Boot state | Who toggles it | Guess |
|---|---|---|---|---|
| 0x69 → **GPIO13** | 0x31d7c2 | output low | power_on: high, after `01 01` + 200 ms. power_off: low if 0x71e5a6. Standby path 0x337116: low | amplifier ST-BY or audio power (medium/low) |
| 0x37 → GPIO38 | 0x31d848 | output low | power_on: high if 0x6dce67 == 0. power_off / MCU shutdown: low. Also 0x320a82 (a debounced input) | panel or camera power (low) |
| 0x31 → none (pin 0x5e, "invalid") | inside 0x31dec6 | n/a | the system-mute GPIO, gated by config 0x6d7104 | **not fitted on this board**, so mute is MCU-only |
| 0x12 → GPIO12 | 0x31db7a | output high | USB VBUS reset (`mcu_Start USB vBus Reset`) | USB port 0 VBUS |
| 0x34 → GPIO36, 0x68 → GPIO43 | 0x31dc46, 0x31dc88 | output low | video-input modules 0x5147a4 / 0x51b060; MCU upgrade 0x475076 and 0x510c72 | not audio |

Reference sequences:

- **power_on** (0x47af0a):
  1. `11 01` (first time only), then `e6 00`, then 30 ms.
  2. GPIO38 high (if 0x6dce67 == 0), then 30 ms.
  3. Backlight level, then 0x50ee36(0,1).
  4. `01 01` (if `*0x984ace04()`), then 200 ms.
  5. **GPIO13 high**, then 100 ms.
  6. Later, once a source is running: `01 00` from 0x473fd4.
- **power_off** (0x47acbc):
  1. `01 01`, then backlight off (`ee ff`), then 100 ms.
  2. Backlight 0xff, then 20 ms.
  3. GPIO38 low.
  4. GPIO13 low (conditional), then `11 00`, then `d1 00`.

## Minimal play-a-tone sequence

### A. Inside stock eCos (recommended first)

1. **Prove the analog chain first, no code.** Put a 1 kHz WAV/MP3 on a USB stick and play it from the stock USB player. High confidence this works and it rules out the amp/speaker side.
2. **Own PCM through eCos** from a patched eCos or an injected task (medium confidence):
   1. Make sure the master volume index is above 0 (call `0x3aa184(vol)`, or leave it as the UI set it).
   2. Create a track with `AUDIF_Track_Create_Ext(0x3a3f8e)`, using 48 kHz / 2 ch / 16-bit params as at 0x38dfb4.
   3. `SetVolume(h,100)`, then `Start(h)`.
   4. Loop `Write_Blocked(h, sine, len, 1)`.
   5. If the MCU still has the system muted (for example the active source never unmuted), call `0x31dec6(0)`, which sends `01 00`.

   The DSP, DAC, mixer and amp power are already up because stock power_on ran. Open question: whether a track plays while a non-phonelink source owns the DSP.

### B. Bare metal from the SD payload (no eCos)

Order matters for pop and safety. Confidence drops at each step.

1. **MCU link alive** (high): keep sending `d1 00` every ≤ 2 s from the start. The MCU cuts power at about 8.9 s if the SoC is silent. Optionally send `11 01` and `e6 00` as power_on does (`tools/mcu_try.py power_on`). Whether these matter for audio is unknown.
2. **Hold mute** (high that it's harmless): send `01 01` = `ff aa 02 01 01 fc aa 94`.
3. **Amp power** (low/medium): drive GPIO13 high. `mw.l 0x9c000320 0x20002000` sets OE, then `mw.l 0x9c000340 0x20002000` drives it high (first confirm GPIO13's M/F bits match the table entry `69 0d 00 00 01 01 01 00`, which U-Boot may not have applied). Wait 100–200 ms.
4. **Audio subsystem** (low; most of the work): reproduce `AUDIF_ResetDsp` 0x3aa50a → `DSP_InitSystem` 0x3bb996, including the microcode download from the eCos image. Then turn the DAC on (0x9c001e14 |= 0xc0000000), set master gain through the 0x3b3810 path, and feed PCM through the track/mixer buffers (`mixer_stream.c` 0x3a7xxx–0x3a8xxx). A shortcut worth checking: the PCM handshake between the CPU and the DSP ("RISC sound", `AUD_Set_RiscSndFs`) may let a small subset of the DSP setup suffice.
5. **Unmute** (high for the MCU part): send `01 00` = `ff aa 02 01 00 fd e5 b2`.

### Unsafe or disruptive (never send from experiments)

- `ad`: reboot.
- `e5 03`: MCU bootloader. `e5 01`: park before an update.
- SoC→MCU `00`: final power-down handshake.
- `e8 01`: the shutdown acknowledgment. The MCU proceeds to power down.
- `11 00`: standby.
- Any `ac` frames: Bluetooth passthrough. Some paths switch UART1 to 460800 (0x521dc0), which drops the link at 115200.
- The MCU-upgrade path (`MCU_SY260L.BIN`, `UPG_MCU_CMD`).
- Stopping the heartbeat: power is cut after about 9 s.
- `d8` frames: they change the AC6965A's audio routing. That is probably harmless, but it is untested and may leave Bluetooth audio misrouted until the next power cycle.

## Open questions and checks

- Which pins drive the 7388's ST-BY (pin 4) and MUTE (pin 22)? Check continuity, unpowered, from those pins to SoC GPIO13 and to the AC6965A pins. That settles whether GPIO13 is the amp enable and whether `01 xx` is the amp mute.
- Where does the SoC's analog output go: straight to the 7388, or through the AC6965A (earlier capture: AC6965A pin 5 = AMUX0R carries an analog signal)? Scope the AC6965A pin 5 and the 7388 inputs while playing USB media.
- What does `e6 00` mean (sent only by power_on)? Which source owns ADC mux inputs 0, 1 and AUX on this board?
- Is config byte 0x6d7091 (the external `51 …` audio-module protocol) zero on this unit? Is 0x6d7104 zero? Read both live from RAM (bytes 0x006d7091, 0x006d7104) during an eCos boot if a read path is ever available.
- Why did the source-switch capture show no `d8`, even with Bluetooth audio? Most likely btap needs [0x71eb17] / phonelink state, which wasn't met.

## PCM playback path for AA audio (Haiku pass over ecos.ann.dis, 2026-11-01; addresses read, meanings partly inferred, not yet tried)

- **PCM does not go to a FIFO or DAC register: the CPU writes a DRAM track buffer and an audio DSP core consumes it.** Playback needs the DSP microcode downloaded (`AUDIF_ResetDsp` 0x3aa50a -> `DSP_InitSystem` 0x3bb996 -> download 0x3bafe0 / 0x3baf48 with IDs 0x1d / 0x1c) and mailbox commands.
- Audio/DSP MMIO 0x9C001700..0x9C0022FF. Reset path: 0x9C001794 &= ~2 (hold), buffer layout 0x3bd5ce (4 KB-scaled bases into 0x9C001744..0x9C001764 and 0x9C001ED0, raw values 0x9C0017C0..0x9C0017D4, 0x9C0017E0), 0x9C001798 = 2, 0x9C001794 |= 2, 0x9C001780 = 1 then 0x10 or 0 by chip ID (0x9C000000 = 0x650 / 0x651).
- Mailbox (`DSP_SendCmd` 0x3bbf74): p3 -> 0x9C001884, `cmd << 24 | p1 << 16 | p2` -> 0x9C001880, doorbell 0x9C001784 bit 0, wait for 0x9C001880 bit 31 (1 s). Clear routine 0x3bd262 zeroes 0x9C001880..+0x7C and sets 0x9C001880 / 0x9C0018A8 / 0x9C0018F8 to 0x80000000.
- Sample rate (`AUD_Set_RiscSndFs` 0x3b449e): command 6, p1 = 8, p2 = 1 (48 kHz), 2 (24 kHz), 3 (16 kHz). The track API accepts only "2ch/s16/pcm/pcm_be" (string), so 16 kHz mono speech likely goes out as stereo.
- Track API: `AUDIF_Track_Init` 0x3a3ef0, `Create_Ext` 0x3a3f8e ("AudioTrack-DEC2"), `Start` 0x3a4636, `Stop` 0x3a47f4, `Write_Blocked` 0x3a5188 (400 ms timeout on "buffer is full"), `SetVolume` 0x3a4b68. The DSP's consume pointer register was not found.
- Output: an internal DAC path (enable 0x3b4ec2: 0x9C001E14 bits 31:30) and an I2S path (0x3b4bde: 0x9C002280/88/8C) both exist; which one this board uses is open. Mute is an MCU command (`01 xx` payload via 0x31d6d4), consistent with the sections above.
- Next: find where eCos keeps the DSP code image (pDspCode, IDs 0x1c/0x1d) in the eCos binary, then try a 1 kHz tone (needs someone listening).
