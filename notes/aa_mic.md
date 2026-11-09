# Android Auto microphone: capture and the AA mic channel

Status 2026-11-09: implemented, not yet run on the unit. Step 1 (the AA mic channel in `aa.c`, host-tested in `test/aatcp.c`), step 2 (`aud_mic_start/read/stop` in `audio.c`, `hu.py audio mic start|stop|status|rec`) and step 3 (`net.c`: the phone's open/close starts and stops capture, the net loop sends 64 ms messages) are in; the on-unit checks below are next. Two corrections to step 2 found while implementing: the B6/B7 writes go out as single-parameter updates (5/3), and stock sends no 5/2 + 5/4 after the rate-code parameter list. The original research was static analysis of `ecos.bin` (load 0x301040), the flash dump, the unpacked stock AA module and the AC6965A datasheet. Nothing was run on the unit. Code addresses are eCos addresses; `R(x)` / `W(x)` are 0x9C000000 + x. Labels: **proven** = read from code or live register dumps; **inferred** = a reading of the evidence that still needs a test; **memory** = from recollection of the open-source head-unit projects (aasdk / openauto), not checked against a source in this pass.

## Summary

- **The stock firmware captures the AA / CarPlay / CarLife microphone on the SoC, in the audio DSP's hands-free uplink, not on the AC6965A.** Android Auto's middleware hands the AA module a recorder whose PCM comes from `AUDIF_Record` tag `"uplink"`, which runs the DSP's HF_ENC module and reads DSP ring 6 (W 0x18C0 written by the DSP, R 0x18C4 written by the CPU), 16 kHz mono s16 PCM. Proven.
- **Which pin the mic comes in on is not proven.** The DSP has a dedicated "mic" source (route source 3) and the internal ADC0 mux is set to input 3 at boot and never changed by product code, while ADC1 carries the line sources (radio, AUX, AV-in, the AC6965A's analog output). So the mic most likely enters at SoC ADC0 (inferred). Whether a microphone is actually fitted or plugged in on this unit is unknown and needs the owner (see "Owner items").
- **The AC6965A has its own MIC pin** (pin 8, with MIC_BIAS on PA2) and stock never feeds SoC-captured audio to it, so Bluetooth hands-free calls almost certainly use the AC6965A's own mic (inferred). Its audio can reach the SoC only as analog (its DAC into SoC ADC1 input 2); there is no I2S/PCM or UART audio stream between the chips in stock code (proven for the code side).
- **Our payload needs no new `ac` frames for the SoC path.** The plan is: reproduce the stock uplink in `audio.c` (ring 6, HF_ENC), and answer the phone's MicrophoneRequest on channel 7 in `aa.c`, streaming 16 kHz mono PCM with timestamps.

## 1. Is a microphone wired to the SoC?

Evidence that the SoC is designed to capture the mic:

| Finding | Evidence | Label |
|---|---|---|
| Stock AA records the mic through the SoC | `android_auto_mw.c` recorder ops table at 0x6C0340 {create 0x391724/0x391726, start 0x3916CE, read 0x391676, stop+clear 0x39160E, destroy 0x3915B2} is passed to the AA module at 0x3920BE (the module's `setAudioRecorder`, strings `android_auto_stub_setAudioRecorder` and `audioSourceCallbackMicrophoneRequest open:%d ancEnabled:%d ecEnabled:%d maxUnacked:%d` in the unpacked module). Create = `AUDIF_Record_Create("uplink0", "uplink", params)` at 0x3917B8 | proven |
| CarPlay and CarLife do the same | `carplay_audstream_player.c` "CarPlay Audio Record Thread", `AUDIF_Record_Create` at 0x397E0E; "carlife mic" at 0x3A246E. CarPlay turns on three processing flags (record params +0x14/+0x18/+0x1C) only for its stream type 8 (0x397DEA..0x397DF6, likely telephony); AA passes them as 0 | proven |
| Only one capture engine exists, and only these three use it | `AUDIF_Record_Create` 0x3A578E has exactly three callers (0x3917B8, 0x397E0E, 0x3A246E); "already have %s, only support one uplink record" (0x3A5CDC); the only accepted tag is `"uplink"` (0x3A5954) | proven |
| Bluetooth never captures | No `AUDIF_Record` caller in the BT code. `btap_audio_switch` 0x3855F2 only routes the AC6965A's analog output into the SoC: ADC1 input 2 (`0x3AC58A(2)`), DSP ADC-source command `AUDIF_Set(0x52, 4)` = cmd 6/0xC/2 (0x3B433A), MCU `d8` channel/gain | proven |
| The DSP has a "mic" source separate from line-in | Shell `output mic` = `AUDIF_Set(0x24, 1, 3)` (0x3B78A6) routes source 3 to the main output (route function 0x3AE338: RISC 7 = source). AUX uses source 4 (`AUDIF_Set(0x24, 1, 4)`, notes/audio_dsp.md trace step 10). Shell commands `getmicp` (AUDIF_Set 0x4E), `getauxp` (0x4F), `gainadc`, `setnr`, `setecho`, `setduplex`, `setadcsrc` (0x3B5502..0x3B578A) | proven (names), inferred (meaning) |
| ADC0 is parked on input 3, ADC1 is the line mux | Codec init writes 0x1F54 bits 7:4 = 0xF (input 3 for L and R; mux code 0x3B304E: ADC0 field = `src << 6 | src << 4`). Live stock value 0x1F54 = 0x3F044FF0 (notes/audio_trace/ecos_audio_regs_run5.txt). The only product caller of the mux, wrapper 0x3AC58A, always selects ADC1: input 0 radio (0x479168, called from the radio mode 0x330BB8), 1 the empty/no-source mode (0x474BB4), 2 the AC6965A (btap), AUX / AV-in configurable (0x47954A, 0x47441A). Live 0x1FEC = 0x00440000 (ADC1 input 0, radio) | proven |
| So the mic enters at ADC0 | follows from the two rows above | inferred |

The AC6965A side:

- Datasheet (SSOP24, `ac6965a.pdf` in the earlier scratchpad): pin 8 MIC (analog mic input with built-in bias, MIC_BIAS on PA2 next to it), pins 11/12 DACL/DACR, pins 5/6 PA4/PA3 with AMUX0R/AMUX0L (analog inputs). The teardown measured UART on pins 3/4, a ~20 kHz PWM on pin 6 at power-on and an analog signal on pin 5; it does not say where pins 5, 6, 8, 11 and 12 go.
- No digital audio link between the chips in stock code: the AC6965A talks to the SoC only over UART1 (`ff aa` frames). The SoC has an `AUD_BT_IFX` pinmux name and an I2S-in switch (`AUD_Set_I2sIn_Switch` 0x3B2F68, 0x1FFC bits 6/7/9), but that function has no callers and the I2S format is set only from the debug shell. Proven for the code; the PCB was not checked.
- How AC6965A mic audio could reach the SoC: only if the BT chip loops its MIC to its DAC, which then lands on SoC ADC1 input 2 (the btap path). Stock never does this and no command for it is known. Any such command would be an MCU frame we have not identified: **needs owner approval** (and an `ac` frame outside the allowed set would be banned outright).
- Bluetooth hands-free calls: stock routes the call's received audio from the AC6965A into ADC1 input 2 (btap, `callstate` 0x71EB17) and never sends uplink audio to the BT chip, so the HFP uplink must come from the AC6965A's own MIC pin (inferred). The parallel Bluetooth-calls work owns that path.

How stock AA/CarPlay get mic audio for the assistant (answer): the phone's MicrophoneRequest reaches the AA module, which opens the eCos recorder; `AUDIF_Record_Read_Blocked` pulls 16 kHz mono PCM from the DSP's uplink ring and the module sends it to the phone on the AA mic channel. CarPlay (Siri, and CarPlay calls, which travel over CarPlay rather than HFP) uses the same uplink with the DSP's processing flags on.

## 2. The SoC capture path (stock `audio_record.c` / `uplink.c`)

### API (the record counterpart of `audio_track.c`)

| Function | Address | Notes |
|---|---|---|
| `AUDIF_Record_Init` | 0x3A571A | creates the "audio record" mutex |
| `AUDIF_Record_Create(name, tag, params)` | 0x3A578E | tag must be `"uplink"`; one instance only; driver ops 0x6C05F8; driver init `uplink_init` 0x3A9A5E |
| `AUDIF_Record_Start` / `Stop` / `Clear` | 0x3A5E94 / 0x3A6040 / 0x3A61EC | start op 0x3A99FA, stop op 0x3A9996, clear op 0x3A9584 |
| `AUDIF_Record_Read_Blocked(h, buf, len, 1)` | 0x3A6596 | read op `uplink_read` 0x3A9634; "wait block timeout(400ms), buffer is empty" |
| `AUDIF_Record_SetVolume` | 0x3A6392 | |
| `AUDIF_Record_Destory` [sic] | 0x3A6774 | uninit op 0x3A94AC |

Create parameters (0x2C bytes, checked at 0x3A57E0..0x3A5B9C): +0 codec (0 PCM, 6 AAC-LATM, 8 mSBC; `uplink_init` also knows 9 = Opus), +4 rate (PCM: 8000/16000/24000), +8 sample format (must be 1 = S16), +0xC channels (must be 0 = mono), +0x10 "asr (stereo AEC for music)" rate (0, 8000 or 16000), +0x14.. +0x2C processing flags. AA's middleware fills codec 0, rate from its caller, format 1, channels 0, everything else 0 (0x39177E..0x3917B8). The AA module's own strings say its AudioSource format cannot be customized (`Customized AudioSource format is not allowed for now`), i.e. it uses the receiver library's fixed microphone format, which is the 16 kHz mono 16-bit that our service discovery also declares (proven for the string, inferred for the value).

### DSP side: what `uplink_init` does (0x3A9A5E, all proven)

1. `AUDIF_Set(0x43, 0, 0)` → 0x3B2E10(0): stop DSP module 6 (cmd 0/6/2, `0x3BBAEC`), then `DSP_SendCmd(0, 0xA, 0, 0)`; RAM 0x6E0294 = 0. Module 6 is **HF_ENC** in the state word 0x9C0018B0 (bits 27:24).
2. `AUDIF_Set(0x44, asr, 0)` → 0x3B2DBE: RISC param 0xB7 bit 5 = asr (0 for AA), then cmd 5/2.
3. Ring 6 pointers to 0: `AUDIF_Set_DumpPtr` 0x3AA086 → W(0x18C0, 0) (writer 0x3BB50C) and `AUDIF_Set_BarrierPtr` 0x3ABFB8 → W(0x18C4, 0) (writer 0x3BB598).
4. `AUDIF_Set(0x29, flags…)` → 0x3B293C: RISC 0xA1 = (old & 0x180) | flag bits 0..6 | min(n, 30) << 11, then 5/2. With AA's zeros only bit 6 is set (the driver always passes 1 in that slot), so RISC 0xA1 = (old & 0x180) | 0x40.
5. `AUDIF_Set(0x28, rate_code, {0xFF, 0xFF})` → **`AUD_Set_Handsfree_Fs`** 0x3B193C. Rate codes from `uplink_init`: PCM 8 kHz 0, PCM 16 kHz **2**, PCM 24 kHz 3, mSBC 16 kHz 1, AAC 16/24/32 kHz 8/9/0x10, Opus 8/16/24 kHz 5/6/7. The function has two paths, chosen by a magic `0x44535xxx` ("DSP") at the top 6 KB of the DSP area (area base + size − 0x1800; `CheckAudioDRAMSize` 0x3AE13E reserves that block for "AEC parameters"). Nothing in eCos writes that magic (only readers at 0x3B19AA, 0x3B2E7A, 0x3B2EB4), so on our layout the no-magic path applies (inferred). For rate code 2 that path (0x3B214E → 0x3B24C6 → 0x3B2918 → 0x3B2660..0x3B277E) does, in logical RISC parameter values (stored byte-swapped as usual):
   - RISC 0xA5 = 0x233D, 0xA6 = 0x234D, 0xAD = 0x2157, 0xAE = 0x399B, 0xAF = 0x431B, 0xB0 = 0x1B3B (filter or AEC coefficients, meaning unknown)
   - RISC 0xA0 = 16000; App param 0x6E = 16000
   - RISC 0xA2 = 0x148, 0xA3 = 0, 0xA4 = 0x1906, 0xB6 = 0 (taken when the AEC-profile mode RAM 0x6E04B0 is 0..2; it is 0 by default)
   - RISC 0xB7 |= 0x80 (single-parameter setter 0x3BD766)
   - media-6 descriptor (`0x3BDAA0(6, d)`, 16 halfwords; RISC byte 0x120 per notes/audio_dsp.md): d[0] = 0, d[15] = 0x20, rest 0
   - `DSP_CheckCodec(6)` 0x3BBC88: stop module 6 if it is not in init, **no code download for modules 3 and 6** (0x3BBD4C / 0x3BBD50: they run resident code, bank 5 = codec 0x1C loaded at boot), then play (cmd 0/6/3, wait for state 3).
6. Ring address and size: `0x3AC07A(6)` / `0x3AC0D4(6)` → descriptor 0x47 (0x3BCFB8): ring 6 = data + 128 · bswap16(info[+0xE]), size from 0x3BC8A4 (100 KB = 0x19000 per notes/audio_dsp.md), W 0x9C0018C0, R 0x9C0018C4.
7. Frame size `[cfg+0x40]` = 2 bytes · 1 ch · rate/125 = 256 bytes (8 ms) at 16 kHz: the driver hands out whole frames only.

Start (`uplink_start` 0x3A99FA): `AUDIF_Set(0x43, 1, 0)` → play module 6 (0x3BBB70) and `DSP_SendCmd(0, 0xA, 1, 0)` (error text "set asr play failed"; what type 0xA is in the DSP's state command is unknown). Stop (`uplink_stop` 0x3A9996): `AUDIF_Set(0x43, 0, 0)`. Clear and uninit: ring 6 W and R back to 0.

Read (`uplink_read` 0x3A9634, proven): fill = (R(0x18C0) − R(0x18C4)) mod size (0x3BB6CC); returns 0 below one frame; copies `fill`-limited whole frames from the uncached ring with wrap (plain `memcpy`, no byte swap, unlike the playback "alter" ring 10 which swaps to big-endian); then writes the new read offset to 0x18C4 (0x3BB598). So the DSP produces, the CPU consumes and acknowledges by writing 0x18C4: the mirror image of the playback rings.

Format: 16 kHz, mono, signed 16-bit (proven from the parameter checks). Byte order in ring 6 is little-endian by inference: the AA middleware passes the bytes untouched to the module, which sends little-endian PCM to the phone; check with a captured tone.

Rings 7 and 8 (W 0x1898 / R 0x189C and W 0x18E0 / R 0x18E4, 0x16800 bytes each) are not the mic: no record code touches them. In the live stock dump both W pointers sit at 0x16200 and do not move over 2 s, with ENC1 in state 1 (ready) and ENC2 in state 0, so they belong to the idle general-purpose encoders ENC1/ENC2 (inferred). In the same dump HF_ENC is in state 1 and ring 6's pointers are 0, as expected with no phone session.

Mic gain and processing: the analog mic preamp boost is 0x1F58 bits 23:21 / 26:24 (shell `gainop`), a 3-bit code from 0 dB to +40 dB. Boot leaves it at 0 dB; stock CarPlay and CarLife set +30 dB before recording; stock AA never sets it. The shell's `gainadc` is a separate ADC PGA (−24..+6 dB in 1 dB steps). The DSP uplink has a digital gain at RISC 0xB5 (0 dB by default, up to +24 dB) that only CarLife touches, and then only to 0 dB. No mic-bias control exists in eCos. The processing flags (RISC 0xA1 bits 0..6) are presumably echo cancellation / noise reduction enables; stock AA leaves them off, CarPlay telephony turns three on. Details, and the register writes to try, are in section 5.

## 3. The AA protocol side

What `aa.c` does now: service discovery offers channel 7 (`CH_MIC`) as an AV input (ChannelDescriptor field 5: stream type 1 = audio, audio config field 2 = {16000, 16, 1}). The Pixel requires this source ("No audio/mic" otherwise) and accepted it. The channel open request (id 7) on channel 7 is answered like every other channel. **Every later message on channel 7 falls through to `st.unknown++` and gets no answer**, so a voice query today would see the phone's MicrophoneRequest go unanswered (the phone presumably times out or shows the assistant stuck listening; inferred).

The stock module confirms the message names and fields: `<- MicrophoneRequest`, `-> microphoneResponse`, `audioSourceCallbackMicrophoneRequest open:%d ancEnabled:%d ecEnabled:%d maxUnacked:%d`, `VOICE SESSION STATUS CHANGE, status:%d`, plus `KEYCODE_VOICE_ASSIST` in its key table (proven: strings in the unpacked module).

Message flow (ids memory, field names proven by the stock strings, wire numbering memory):

| Direction | Id | Body | Notes |
|---|---|---|---|
| phone → HU | 0x8005 MicrophoneRequest (aasdk `AV_INPUT_OPEN_REQUEST`) | 1 open (bool), 2 anc_enabled, 3 ec_enabled, 4 max_unacked | open = 1 when the assistant starts listening, 0 when it stops |
| HU → phone | 0x8006 MicrophoneResponse (`AV_INPUT_OPEN_RESPONSE`) | 1 int32, 2 uint32 | aasdk names them session / value, the GAL naming is status / session_id; openauto sends 0 and 0 on success, so 0/0 works under either naming. openauto answers an open only after its audio input started, and answers a close with 0 too |
| HU → phone | 0x0000 media with timestamp | u64 big-endian timestamp in microseconds, then raw PCM s16 little-endian mono 16 kHz | same framing as the phone's audio to us; openauto uses the host clock in microseconds |
| phone → HU | 0x8004 MediaAck | 1 session, 2 ack count | the HU should keep at most max_unacked frames in flight |
| phone → HU | 0x8000 setup (possible) | | openauto's audio-input service answers it with 0x8003 {1: status 2 (OK), 2: max_unacked 1, 3: config 0}; answering it harmlessly costs nothing |
| phone → HU (control) | 17 voice session notification | status start / end | informational (memory); stock logs it |

All of these are encrypted messages on channel 7 without the control flag (as the other media messages). Packet size: openauto reads its Qt audio input in fixed chunks of about 2 KB (memory, about 64 ms); 640..2048 bytes (20..64 ms) per message is a reasonable range, and the stock driver's 256-byte frame granularity fits any multiple. Push-to-talk: the phone's own mic button in the AA UI works without any key; a hardware key would send KEYCODE_SEARCH (84) or KEYCODE_VOICE_ASSIST (231) through the input channel and must be listed in the input service's supported keycodes (memory). `aa.c` declares only HOME and BACK today.

Phone calls over wireless AA do not use this channel: they run over Bluetooth HFP between the phone and the AC6965A (memory, consistent with stock having no SoC uplink in the BT code).

## 4. Implementation plan for the payload

No `ac` frames are needed for any step below.

### Step 1: protocol only, host-tested (no unit)

- `aa.h`: add `void (*mic)(int open)` to `struct aa_platform`, and `int aa_send_mic(const unsigned char *pcm, int n)` plus mic counters in `struct aa_status` (opens, frames sent, acks, drops for a full window).
- `aa.c` `on_channel`: a `CH_MIC` branch. 0x8005: parse fields 1 and 4 with `pb_get`; on open set `mic_open = 1`, `mic_unacked = 0`, `mic_window = max_unacked ? max_unacked : 1`, call `pf->mic(1)`, reply 0x8006 {1: 0, 2: 0}; on close call `pf->mic(0)`, reply the same. 0x8004: `mic_unacked -= ack count` (at least 1, floor 0). 0x8000: reply 0x8003 {1: 2, 2: 1, 3: 0}. Reset `mic_open` in `aa_start` / `aa_closed`.
- `aa_send_mic`: if running, open and `mic_unacked < mic_window`, send id 0x0000 with an 8-byte big-endian `now_us()` timestamp plus the PCM; else count a drop and return 0.
- Host test: extend the scripted phone (`test/`, the AA-over-TCP test) to send a MicrophoneRequest, check the response, count timestamped frames, ack them, close. This also guards the window logic.

### Step 2: capture in `audio.c` (unit, needs the unit free; no phone)

- `aud_mic_start(void)`, `aud_mic_stop(void)`, `int aud_mic_read(short *buf, int max_samples)` and a level meter (RMS and peak of the last read) for status. Net task only, like the rest of `audio.c`.
- Start = the stock sequence from section 2 with our helpers: stop module 6 (`send_cmd(0, 6, 2, 0)`, `wait_state(6, 2)`), `send_cmd(0, 0xA, 0, 0)`; RISC 0xB7 bit 5 clear, 5/2; `REG(0x18C0) = REG(0x18C4) = 0`; RISC 0xA1 = (RISC 0xA1 & 0x180) | 0x40, 5/2; the rate-code-2 parameter list (0xA5..0xB0, 0xA0, App 0x6E, 0xA2..0xA4, 0xB6, 0xB7 |= 0x80) then 5/2 and 5/4; media-6 descriptor at RISC byte 0x120; play module 6 (`send_cmd(0, 6, 3, 0)`, `wait_state(6, 3)`); `send_cmd(0, 0xA, 1, 0)`. Ring base = `data + 128 * info(0xE)` (refuse if 0), size 0x19000 (re-read 0x3BC8A4 for ring 6 before coding).
- Read = fill from `REG(0x18C0)` and the CPU's own read offset, `dc_inval` the span (our DSP window is cached), copy whole 256-byte frames with wrap, write the new offset to `REG(0x18C4)`. Keep the DSP→CPU mailbox serviced as elsewhere.
- Stop = `send_cmd(0, 6, 2, 0)` + `send_cmd(0, 0xA, 0, 0)`, pointers to 0.
- HU1: `hu.py audio mic start|stop|status|rec SECONDS` (status shows 0x18B0, 0x18C0/0x18C4 and the level; `rec` writes the raw PCM to the card for listening on the PC).
- Checks, in order: HF_ENC reaches state 3; 0x18C0 advances by 32000 bytes/s (16 kHz mono s16); samples are not all zero; the level reacts to sound in the cabin or on the bench (a test tone from our own speaker is enough: with a working mic it shows up in the capture, and the bench webcam mic is not needed). The last check is also the cheapest answer to section 1's open hardware question.

### Step 3: wire it together

- `net.c`: `pf.mic` starts/stops capture; the net loop pumps `aud_mic_read` into `aa_send_mic` in 1024- or 2048-byte messages while the mic is open (drop the oldest audio rather than queue seconds when the window is full).
- `aa_send_mic` and the capture pump live in the same task as the rest of `aa.c` and `audio.c`, so no locking is needed.
- Later: a voice key (front-panel long press or a steering-wheel input) sending KEYCODE_SEARCH, declared in service discovery.

### Risks

- **No mic fitted or connected, or the mic wired only to the AC6965A.** Then step 2 captures silence. The fallback (BT chip mic looped to its DAC into ADC1 input 2) needs an unknown MCU command: owner approval, and banned if it is an `ac` frame outside the allowed set.
- **Routing interplay:** the route function 0x3AE338 stops HF_DEC and HF_ENC when both are playing; our stream-1 start must not re-route while the mic is open, or capture must restart after it.
- **DSP load:** HF_ENC plus the PCM decoder plus the EQ at once; stock does the same during AA voice, but watch underruns (`hu.py audio status`) during the first tests.
- **Unknowns in the sequence:** the meaning of cmd 0/0xA, RISC 0xA1 bit 6 and the coefficient set; ring 6 byte order; which preamp channel carries the pinhole mic (section 5); whether the top-6 KB "AEC parameters" block must hold anything. Each shows up in the step-2 checks as a stuck state, a stopped 0x18C0, or silent/garbled samples.
- **Echo:** stock AA runs with echo cancellation off and relies on the phone pausing media while listening; nav prompts during a query could leak into the capture.
- **Protocol details from memory** (message ids 0x8005/0x8006, response field meaning, packet size, setup handling): the host test checks our side only; the first real check is the phone (adb `logcat` CAR.*/GH.* tags show rejects).

## 5. Mic gain

Static analysis of `ecos.bin` on 2026-11-09; nothing was run on the unit. Why: the payload's capture puts loud speech at 10 cm from the pinhole at about −54 dBFS RMS over a −70 dBFS floor. The speech is clearly there, but it is 30-40 dB too quiet. Every eCos load/store into the codec block (0x9C001E00..0x9C001FFF) was enumerated from a full disassembly, so the lists of writers below are complete for direct accesses.

### Analog mic preamp boost: 0x1F58 bits 23:21 and 26:24 (shell `gainop`)

- Shell `gainop <dB>` (0x3B9220): `strtod` → float → `0x3ABD20(6, 0xFFFF, &f)` (case 6 of that gain dispatcher, jump table 0x3ABD44) → setter **0x3B3E6E(ch, f)**. Proven.
- The setter maps the float to a 3-bit code by exact comparison. The getter (0x3B4980, table 0x3B49D8) maps codes back. Proven:

| Code | Setter input | Getter output |
|---|---|---|
| 0 | any positive value not listed below | +40 dB |
| 1 | 30.0 | +30 dB |
| 2 | 27.5 | +27.5 dB |
| 3 | 21.0 | +21 dB |
| 4 | 12.0 | +12 dB |
| 5 | 9.0 | +9 dB |
| 6 | 0.0 | 0 dB |
| 7 | any negative value | −1.0 (sentinel; off or mute, inferred) |

- Fields: ch 1 → **0x1F58 bits 23:21**, ch 2 → **bits 26:24**, ch 0xFFFF → both. Write sequence (0x3B3F14..0x3B3F80): `r = R(0x1E18); W(0x1E18, r | 0x40000000); W(0x1F58, new); W(0x1E18, r & ~0x40000000)`. Stock holds 0x1E18 bit 30 set while it changes the boost; the codec init also clears that bit. Its purpose is inferred: probably a mute against the switching click.
- Boot value: codec init writes 0x1F58 = 0x06C60606, so both codes are 6 (0 dB). This matches the live stock dump (notes/audio_trace/ecos_audio_regs_run5.txt). Our `codec_init` writes the same value.
- Who sets it (proven): case 6 has exactly three callers. (1) The CarPlay session entry 0x395286 (`carplay_mw.c`), which the projection-mode switch 0x31C57A runs for device types 4/5: 30.0 on both channels. (2) CarLife's `RecordaudioPlayerStart` 0x3A2354: 30.0 on both channels, before it creates its "carlife mic" uplink record. (3) The shell. `AndroidAutoMain` 0x3908AE (device types 10/11) and the AA recorder ops never call it, and nothing ever sets it back to 0 dB. So stock CarPlay and CarLife record with +30 dB. Stock AA gets whatever the last session left: 0 dB after a boot with only AA, or +30 dB if CarPlay or CarLife ran since boot (inferred from the code paths). Our payload records at 0 dB. That explains the 30-40 dB shortfall (inferred, strong).
- The name ("op" = op-amp) and the 0..+40 dB steps fit a microphone preamp. The ADC PGA below is a separate ±dB trim.

### ADC PGA: shell `gainadc`

- `gainadc <dB> [adc] [ch]` (0x3B9284): dB goes through `strtod` and is truncated to int; adc defaults to 0, ch to 0xFFFF (both). It calls `0x3ABD20(4, {adc, ch, dB})` → **0x3B3B7A(adc, ch, dB)**. Code = 6 − dB for dB in −24..+6 (5-bit field, 1 dB per step). +7 and above clamp to code 0 (+6 dB); below −24 gives code 31. Proven.

| adc | ch 1 | ch 2 |
|---|---|---|
| 0 | 0x1E18 bits 4:0 | 0x1F58 bits 4:0 |
| 1 | 0x1E18 bits 20:16 | 0x1F58 bits 12:8 |
| 2 | 0x1F54 bits 4:0 | 0x1F58 bits 20:16 |

- Boot sets every field to code 6 (0 dB): 0x1E18 `&= 0xFFE0FFE0; |= 0x00060006`, and 0x1F58 = 0x06C60606. 0x1F54 bits 4:0 read 0x10 only because bit 4 belongs to the ADC0 input mux (0x1F54 bits 7:4, `src << 6 | src << 4`). An adc 2 / ch 1 write would clobber the mux, so either that ADC does not exist on this chip or the path is a stock bug (inferred). Do not write it.
- No product code calls case 4; only the shell does. The PGA reaches at most +6 dB, so it can only trim. Which adc number carries the mic is not visible in code.

### Mic bias

- eCos has no mic-bias string ("bias", "vref", "micbias"; the one "ebias" belongs to the Wi-Fi driver), and no `AUDIF_Set` id or shell command touches a bias. Besides the boot codec init (0x3BF564), the post-boot writes, the DAC GPIO bits (0x1E14 bits 31:30), the I2S-in switch (0x3B2FBA: 0x1FFC bits 6/7/9 with 0x1E20/0x1E10/0x1F40), the ADC mux, the PGA and the boost, nothing writes the codec block. 0x1F50 is never written (it reads 0x800 live). Proven.
- So stock has no separate mic-bias step for AA, CarPlay or CarLife. Whatever bias exists comes from the codec init, which our payload repeats verbatim. Init fields of unknown meaning that could include a bias enable: 0x1F54 bits 29:24 = 0x3F, 0x1F54 bits 21:16 = 0x04, 0x1E18 bits 14/30 and 11:10 cleared, 0x1F64 bits 4:0 = 0x1F. The capture shows speech +15-20 dB above the floor in the 50-1000 Hz bands, with strong envelope modulation, which an unbiased electret would hardly give, so bias is not the first suspect (inferred).
- Stock's live 0x1E18 is 0x83060306, and our `codec_init` + `post_boot` should produce the same. A reading of 0x4F064F00 would mean the later init writes did not land, leaving bits 30, 27:26, 14 and 11:10 set and the adc 0 / ch 1 PGA at code 0 (+6 dB). Re-read it before the boost test.

### DSP digital gain on the uplink: RISC 0xB5

- `AUDIF_Record_SetVolume` 0x3A6392 converts a volume to dB (0x3A6924: 100 = 0 dB, 101..124 = +1..+24 dB, below 2 = −103.5 dB, a log curve otherwise). It then calls the uplink driver's op 0x3A9964 (ops table 0x6C05F8, slot 4) → `0x3ABD20(0x10, &dB)` → **0x3B3F8C**, which clamps to +24 dB (anything ≤ −66 dB gives 0) and sets **RISC 0xB5 = (u16)(2048 · 10^(dB/20))** (Q5.11, so 0x0800 = 0 dB), then sends cmd 5/2. Proven.
- Default: param init stores raw 0x0008 at RISC byte 0x16A (0x3BD5AE), i.e. logical 0x0800 = 0 dB. Our `params_init` has the same entry.
- Callers: only CarLife (`SetVolume(100)`, 0 dB). AA and CarPlay never call it, so stock applies no extra digital gain to the uplink. The DSP-side effect on ring 6 is inferred from the driver op, not observed.
- Processing flags (`uplink_init` → 0x3B2938): RISC 0xA1 bits 0..5 = create params +0x14, +0x18, +0x1C, +0x20, +0x24, +0x28; bit 6 = always 1; bits 15:11 = a level `min(n, 30)` that `uplink_init` always passes as 0. CarPlay stream type 8 turns on bits 0..2. Which bit is AEC, NR or AGC is unknown, and none is a fixed gain stage on the CPU side. Trying them is a later experiment, not the gain fix: they change the DSP processing, and AEC without a reference may misbehave.
- Level readout: shell `getmicp` = `AUDIF_Set(0x4E)` → 0x3AF11A reads DSP info halfword 0xE8 (byte 0x1D0 of the info table at data + 0x280, byte-swapped, i.e. `info(0x1D0)` in `audio.c`). `getauxp` reads halfword 0xE9 (0x1D2). This is probably a DSP-measured mic peak or level (inferred from the name), useful as a second meter during the test.

### What to try on the unit

1. **Boost +30 dB on both channels**, the value stock CarPlay and CarLife use in production:

```c
u32 r = REG(0x1e18);
REG(0x1e18) = r | 0x40000000u;                                 /* stock's bracket */
REG(0x1f58) = (REG(0x1f58) & ~0x07e00000u) | 0x01200000u;     /* bits 26:24 = 1, 23:21 = 1: +30 dB */
REG(0x1e18) = r & ~0x40000000u;
```

   From the stock 0x06C60606 this gives 0x01260606. Expect loud speech at 10 cm to land near −24 dBFS RMS. Noise that enters before the preamp rises with it; ADC noise does not.
2. **Other steps** (same bracket; values for both channels, mask 0x07E00000): code 3 (+21 dB) = 0x03600000, code 2 (+27.5) = 0x02400000, code 1 (+30) = 0x01200000, code 0 (+40) = 0x00000000. Any code from 6 down to 0 is safe to try: each is a state the stock shell can set. Back off to code 3 or 2 if speech peaks clip at 10 cm.
3. **Which channel is the mic:** set only bits 23:21 (ch 1: `& ~0x00e00000 | 0x00200000`) or only bits 26:24 (ch 2: `& ~0x07000000 | 0x01000000`) and compare levels. The ring is mono, so this shows which preamp input feeds the capture.
4. **Fine trim, only after the analog boost:** RISC 0xB5 digital gain via `dsp_set(RISC, 0xb5, v); send_cmd(5, 2, 0, 0);` with v = 0x0FF6 (+6 dB), 0x168C (+9), 0x1FD9 (+12), at most 0x7ECB (+24). It raises noise and signal alike. Undo with 0x0800. The ADC PGA adds at most +6 dB more (code 0 in the fields above); leave it alone unless needed.
5. **Undo the boost:** the same bracket with `(REG(0x1f58) & ~0x07e00000u) | 0x06c00000u`, which restores the stock 0x06C60606.

## Owner items

- Is there a microphone: a rear 3.5 mm "MIC" jack or wired mic in the harness, a pinhole mic on the faceplate, and is a mic plugged in on the bench and in the car? A mic that came in the box is typical for these units.
- Optional, unpowered continuity check: mic jack tip to AC6965A pin 8 (MIC) and to the SoC side, to settle where the mic lands.
- Before any on-unit step: the unit must be free (step 2 needs `hu.py` access but no phone). A stock-firmware cross-check is also possible (boot stock eCos with the register-snapshot hook and do one AA voice query, watching 0x18C0 and 0x18B0), but it needs the owner's phone and a stock boot.
- Any MCU frame to route the AC6965A's mic into the SoC: needs owner approval (not planned).
