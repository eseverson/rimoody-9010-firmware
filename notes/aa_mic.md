# Android Auto microphone: capture and the AA mic channel

Status 2026-11-09: research only, static analysis of `ecos.bin` (load 0x301040), the flash dump, the unpacked stock AA module and the AC6965A datasheet. Nothing was run on the unit. Code addresses are eCos addresses; `R(x)` / `W(x)` are 0x9C000000 + x. Labels: **proven** = read from code or live register dumps; **inferred** = a reading of the evidence that still needs a test; **memory** = from recollection of the open-source head-unit projects (aasdk / openauto), not checked against a source in this pass.

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

Mic gain and processing: `gainadc` and `getmicp` exist in the shell; which register holds the ADC0 gain is unknown (0x1F54 bits 15:8 = 0x4F, 0x1E18 = 0x4F064F00 are candidates). The processing flags (RISC 0xA1 bits 0..6) are presumably echo cancellation / noise reduction enables; stock AA leaves them off, CarPlay telephony turns three on.

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
- **Unknowns in the sequence:** the meaning of cmd 0/0xA, RISC 0xA1 bit 6 and the coefficient set; ring 6 byte order; the ADC0 gain register; whether the top-6 KB "AEC parameters" block must hold anything. Each shows up in the step-2 checks as a stuck state, a stopped 0x18C0, or silent/garbled samples.
- **Echo:** stock AA runs with echo cancellation off and relies on the phone pausing media while listening; nav prompts during a query could leak into the capture.
- **Protocol details from memory** (message ids 0x8005/0x8006, response field meaning, packet size, setup handling): the host test checks our side only; the first real check is the phone (adb `logcat` CAR.*/GH.* tags show rejects).

## Owner items

- Is there a microphone: a rear 3.5 mm "MIC" jack or wired mic in the harness, a pinhole mic on the faceplate, and is a mic plugged in on the bench and in the car? A mic that came in the box is typical for these units.
- Optional, unpowered continuity check: mic jack tip to AC6965A pin 8 (MIC) and to the SoC side, to settle where the mic lands.
- Before any on-unit step: the unit must be free (step 2 needs `hu.py` access but no phone). A stock-firmware cross-check is also possible (boot stock eCos with the register-snapshot hook and do one AA voice query, watching 0x18C0 and 0x18B0), but it needs the owner's phone and a stock boot.
- Any MCU frame to route the AC6965A's mic into the SoC: needs owner approval (not planned).
