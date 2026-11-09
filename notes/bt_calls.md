# Bluetooth hands-free calls (HFP)

Status 2026-11-09: static analysis only, nothing was sent to the unit. Sources: stock eCos (`ecos.bin`, load address 0x301040), the NOR dump (flash mapped at 0x98000000), and the decompressed Android Auto module (`android_auto_ROM.bin`). Code addresses are eCos addresses. Labels: **proven** = read directly from code or tables; **inferred** = meaning deduced from behavior, guards, callers or names; **open** = not settled. Companion notes: `audio.md` (MCU frames, mute), `audio_dsp.md` (DSP commands and parameters), `android_auto.md` (RFCOMM, BT_INIT, `ac 10` / `ac 01`, `ac 30`).

## Summary

- The AC6965A runs the whole HFP stack (SCO, AT commands, ring, and almost certainly the microphone). The SoC sends short `ac` commands (dial, answer, hang up, DTMF, mic mute, audio transfer) and receives `ac` events with the call state and the number. No call audio crosses UART1.
- Call audio takes the same analog path as Bluetooth music: chip DAC, SoC ADC1 input 2, DSP media 4 (analog line-in route), DAC, amplifier. During a call stock also switches the DSP front channel-select mode to 2 and sets call volume on the chip with `d8 00 <gain>`, bracketed by MCU mute `01 01` / `01 00`.
- Microphone: stock does nothing on the SoC side for calls, and its command set has no mic gain, AEC or noise-suppression frame. The car mic almost certainly goes into the AC6965A's own MIC pin.
- With Android Auto projecting, stock sends the phone an AA key event that maps to KEYCODE_MEDIA_PAUSE, keeps AA in front (the phone draws the call screen), and switches the analog Bluetooth route on 60 service-loop ticks later.
- For our payload, a call driven from the projected AA screen needs no new `ac` frames: parse the events, switch the SoC audio route, done. Our own answer and hang-up buttons need `ac 0f` and `ac 10`, which need the owner's approval.

## 1. Mechanics (recap, with what was added)

- **Out (proven):** selector 0x3887d0(cmd, arg) reads a class byte at flash 0x984ACE80 + cmd·8 + 4. Class 0 sends nothing. Classes 06/0D/18/19/20/25 send `ac <class> <len> <string>` (0x31dd16: copies at most 30 bytes, stops at the NUL, `len` excludes the NUL). Class 1C sends 6 data bytes (0x31dd76). Every other class sends the two bytes `ac <class>` (0x31dcf0). Direct callers send class 0x22 through 0x31dd16.
- **Bluetooth API table (proven):** 52 function pointers at flash 0x984ACDAC..0x984ACE7C. The UI and other modules call them as `lwi [0x984ac000 + off]`, so the slot offset identifies each use. Slot +0x58 (0x387cda) returns the call state and is the "in a Bluetooth call" test used in 73 places.
- **In (proven):** `bt_parse_receive` 0x387c48 takes the byte after `ac` as the event, looks it up in 32 entries `{u8 event, 3 pad, u32 handler}` at flash 0x984ACC84, and passes the handler a pointer to the bytes after the event. Events 00, 02, 03, 04, 0C and 1C..1F are empty handlers; 0F has no entry.
- **State bytes (proven as storage, meanings inferred from the guards):** 0x71eb14 link state (≥ 3 connected, 0 after `ac 01`); **0x71eb17 call state: 0 idle, 1 outgoing, 2 incoming ringing, 3 active**; 0x71eb16 music (1 playing, 2 paused); 0x71eb19 voice-in-car flag; 0x71eb18 mic-mute flag; 0x71eb7d number (18 bytes); 0x71eb8f phonebook name of that number; 0x71edda voice-assistant active; 0x71eb61 keypad dial buffer.
- **UI notify** 0x386fb6(msg, arg): with a UI callback registered (0x6d7460) it posts message 0x6a to the UI; without one, msg 5 and 7 call `btap_switch_to_call` 0x385806 and msg 0x0a calls `btap_back_from_call` 0x385b0a. Messages seen: 5 call starts (incoming or outgoing), 7 call active, 0x0a call ended, 0x13 mic mute toggled, 0x14 voice moved to phone, 0x19 connected, 0x1a disconnected, 0x22 / 0x24 music paused / playing (proven as code paths).

## 2. Outbound call control (question 1)

Full frames are SoC-side (`tools/mcuframe.py`).

| Action | cmd → class | Payload | Example frame | Sender (slot, function) | Guard | Callers | Label |
|---|---|---|---|---|---|---|---|
| Dial | 0x14 → 0x0d | `ac 0d <len> <ASCII digits>` (≤ 30) | "123": `ff aa 06 ac 0d 03 31 32 33 a8 36 9a` | +0x44, 0x3886ba | connected, number non-empty, call state 0 | keypad green key 0x368002 / 0x36b61c (dial buffer 0x71eb61), CarLife HFP request type 1 (dial) 0x39fd9e | proven |
| Redial | 7 → 0x0e | `ac 0e` | `ff aa 02 ac 0e 44 c6 22` | +0x48, 0x388456 | connected | green key with an empty dial buffer 0x368010 / 0x36b628 | inferred (redial last number) |
| Answer | 4 → 0x0f | `ac 0f` | `ff aa 02 ac 0f 43 99 cc` | +0x4c, 0x3886f2 | call state 2 | green key while ringing 0x36b632, hardware key 0xa8 0x32f54c, CarLife type 3 0x39fdaa, 0x3694ae, 0x38190a | proven |
| Reject | 5 → 0x10 | `ac 10` | `ff aa 02 ac 10 42 38 7c` | +0x50, 0x388724 | call state 2 | 0x36947c, 0x36bf66, 0x3819a6 | proven |
| Hang up | 6 → 0x10 | `ac 10` | same as reject | +0x54, 0x388756 | call state 1..3 | hardware key 0x8f 0x32f53e, CarLife types 2 and 4 0x39fdb6, 0x36868e, 0x369460, 0x36bf4a, 0x38198a | proven |
| Audio transfer car ↔ phone | 0x0d or 0x0e → 0x17 | `ac 17` (toggle) | `ff aa 02 ac 17 3b 8a e6` | +0x60, 0x38840e (flips 0x71eb19) | connected | in-call button 0x369412 (active call only, debounced), source change during an active call 0x32f168 / 0x32f240 | inferred (strong) |
| Mic mute | 0x0c → 0x1b | `ac 1b` (toggle) | `ff aa 02 ac 1b 37 c7 2a` | +0x64, 0x38859e (flips 0x71eb18, notify 0x13) | connected | in-call button 0x369d44 (600 ms speaker mute first), CarLife types 6/7 (mute / unmute mic) 0x39fdfa, call-end cleanup 0x369e64 | inferred (strong) |
| DTMF | 0x15 → 0x20 | `ac 20 01 <ASCII 0-9 * #>` | "5": `ff aa 04 ac 20 01 35 fa f8 3a` | +0x68, 0x3883dc | call state 3 | in-call keypad 0x368fbc / 0x36a2ca / 0x36bb54, CarLife type 5 (DTMF code) 0x39fdc2 | proven |
| Voice assistant | direct, class 0x22 | `ac 22 01 01` start, `ac 22 00` stop | `ff aa 04 ac 22 01 01 2c df 40` / `ff aa 03 ac 22 00 2f c9 ea` | VoiceView 0x3521da..0x35237e | connected | VoiceView buttons | inferred |
| Call volume | not an `ac` frame | `d8 00 <g>`, g = table 0x9806bb30[vol] (1..25) | vol 20: `ff aa 03 d8 00 0d 18 30 04` | `SendMcuAudioCh_Gain` 0x31e026 | volume mode 5 | volume keys 0x3846f0 → 0x3847ec (mode 5 is chosen when slot +0x58 says a call is up, 0x384714), route setup 0x385724 | proven |

- The CarLife HFP request handler (0x39fd3a, jump table 0x39fd90) is the decoder ring for several of these: its request types 1 dial, 2 terminate, 3 answer, 4 reject, 5 DTMF, 6 mute mic, 7 unmute mic dispatch to slots +0x44, +0x54, +0x4c, +0x54, +0x68, +0x64, +0x64 (proven dispatch; the type names come from the CarLife protocol, inferred).
- Reject and hang up are the same frame, `ac 10`, so the chip decides from its own state.
- Stock keeps a separate saved volume for the call volume type (ReadVolume 0x473f18 keeps own slots for types 1, 5 and 7), applies it with 0x473fd4(5), and sends `d8` on every volume key press during a call (proven).
- Not needed and dangerous: `ac 16` starts the phonebook download **and switches UART1 to 460800** (0x3885ee → 0x521da4(1) → 0x521dc0 path); `ac 15` stops it and returns to 115200 (0x388622 → 0x521da4(0)). `ac 1d` / `ac 1e` / `ac 1f` request call-log lists (0x38847c). A call starting during a download aborts it (0x3876b8).
- Other classes with senders, not call related: `ac 11` connect (0x38837c), `ac 12` disconnect (0x3883a2), `ac 30` reconnect (0x388354), `ac 14` / `ac 13` AA session start / end (section 6), `ac 09` / `ac 0c` / `ac 0b` from slots +0x74 / +0x78 / +0x7c (used by the media UIs and projection modules, probably AVRCP; not mapped), `ac 18 <len> <bdaddr…>` (0x387f68) and `ac 1c <bdaddr 6>` (0x387e60) device-list actions.

## 3. Inbound events (question 1)

Layout is the payload after `ac`. Strings are copied with `strcpy` (0x604d42), so stock relies on a NUL; our parser must also stop at the frame end.

| Event | Layout | Handler | What stock does | Meaning | Label |
|---|---|---|---|---|---|
| 0x07 | `07 <number ASCII> 00` | 0x3878cc | marks connected; call state 2; number to 0x71eb7d; name lookup 0x384cce; call log type 1; notify 5 | **incoming call ringing, caller number** | proven (state 2 is what answer and reject require) |
| 0x08 | `08 <number ASCII> 00` | 0x3877c2 | call state 1; number; call log type 2; notify 5 | **outgoing call** (dialing / alerting) | inferred (strong) |
| 0x09 | `09` | 0x387712 | ignored while the voice assistant is active; aborts a phonebook download; call state 3; notify 7 | **call active** | inferred (strong) |
| 0x0e | `0e <number ASCII> 00` | 0x3872aa | marks connected; call state 3 with the number; call log type 2; notify 7 | call active with number (likely a call placed or answered on the handset) | inferred |
| 0x05 | `05` | 0x3874c0 | voice-in-car flag = 1; marks connected; if call state ≠ 0: state 0, notify 0x0a; music playing → paused | **call ended** / hands-free idle | inferred (no dedicated "hung up" event exists) |
| 0x06 | `06` | 0x38740a | marks connected; music playing (notify 0x24); call state 1..3 → 0, notify 0x0a | music playing; also ends any call state | inferred |
| 0x0d | `0d` | 0x38739a | if the voice-in-car flag is set: clear it, notify 0x14 | voice moved to the handset | inferred |
| 0x16 | `16 <b>` | 0x386190 | 0x71edda = b; `btap_audio_switch(1, 1)` | voice-assistant (SCO) audio active | inferred |
| 0x10 | `10 00 <bdaddr 6, LE> <name> 00` | 0x387002 ("parse_resp_MX") | ignored unless byte 1 is 0; adds the device; marks connected; if already connected and a call is up: call state 0, notify 0x0a | **device (hands-free) connected** | proven |
| 0x01 | `01` | 0x38757e ("parse_resp_IA hfpstate") | link state 0; device list disconnected; notify 0x1a; clears metadata; sets the reconnect flags 0x6de205/0x6de206 | **hands-free disconnected** (does not clear the call state) | proven |
| 0x17 | `17 <len> <phone time ASCII>` | 0x385fb6 | parses YYYY MM DD hh mm ss, sets the clock (`a1` / `a8`) | phone time | proven |
| 0x20 | `20 <b0 < 5> <passkey u32 LE>` | 0x38632c | stores 0x71eea8, passes it to the AA pairing path | pairing passkey | proven |
| 0x0b, 0x11..0x14 | phonebook and call-log entries, end of list | 0x3873c8, 0x3866ac, 0x38666e, 0x386630, 0x3865f2 | only while a download runs (0x71eb5a) | not needed | proven |

Expected sequences (inferred, to be confirmed in a receive-only log): incoming `ac 07 "<number>"` → (answer `ac 0f`) → `ac 09` or `ac 0e` → (hang up `ac 10`) → `ac 05`; outgoing (dial `ac 0d`, or dialed on the phone) → `ac 08 "<number>"` → `ac 09` → `ac 05`. The first-session captures decoded `ac 10`, `ac 18`, `ac 1b`, `ac 2a/2b` and `ac 15`, but no call was captured.

## 4. Call audio path (question 2)

**Entry (proven).** Notify 5 (`ac 07` / `ac 08`) → `btap_switch_to_call` 0x385806. Outside Android Auto it remembers the current source, then switches to source 8, the Bluetooth phone source (0x32d0f2(8, 8)). That source's enter hook (0x331958 → 0x385c4e) starts a 2 s timed mute and, with a call up, calls `btap_audio_switch(1, 1)`. Notify 0x0a → `btap_back_from_call` 0x385b0a restores the previous source (0x6d7490 / 0x6d748c); the Bluetooth source exit hook (0x331980 → 0x385d2e) calls `btap_audio_switch(0, 0)` and mutes for 1.5 s.

**`btap_audio_switch(1, force)` with a call up (0x3855f2), in order (proven sequence; meanings from audio.md / audio_dsp.md):**

1. `01 01` with a 1000 ms timed mute (0x31ebe0(1, 1000) → 0x31dec6), then 100 ms.
2. 0x3ab510(1): `AUDIF_Set(0x0c, 4)` stop media 4, `AUDIF_Set(0x24, 1, 4)` route media 4 (analog line-in through the ADC, no codec) to the main output, `AUDIF_Set(0x0d, 4)` start. Same sequence as stock's AUX switch.
3. 0x3ac58a(2): `AUDIF_Set(0x39, 1, 2)`: **ADC1 input = source 2** (0x9C001FEC bits 21:20 and 17:16 = 2), cached at 0x6c062d.
4. 0x3abd20(0, 4, &4.0f) → 0x3b36f0: media 4 input gain +4 dB (RISC 0x11, code 2·(dB + 103.5), clamp +24 dB) (inferred field, proven value).
5. 0x473fd4(4): mode-4 volume and an unmute request, held off while the timed mute runs.
6. 30 ms; master volume index forced to 40 = 0 dB through 0x3aa184(40) (the saved index is restored in RAM, not in the DSP); then 1 s.
7. ADC1 source 2 again; `AUDIF_Set(0x52, 4)` → 0x3b433a → **DSP command 6/0x0c/2** (front channel-select mode; mode 0 otherwise, 1 for `AUDIF_Set(0x52, 3)`).
8. 0x473fd4(5): the saved call volume (type 5) is applied and unmute requested.
9. `d8 00 <table[vol]>` (0x31e026). With call state 3, the MCU service loop resends it 10 ticks later (0x6dce25, 0x31ea84).
10. The timed mute expires (0x31ec12; counter = ms / 16) → `SYSTEM_MUTE_OFF` (0x4935a0) → `01 00`.

**`btap_audio_switch(0)` at call end (proven):** `01 01` with a 1200 ms timed mute; 100 ms; `AUDIF_Set(0x52, 0)` → DSP 6/0x0c/0; the ADC source cache is cleared (0x3ac5b0, registers untouched); 0x3ab510(0) → `AUDIF_Set(0x0c, 0)`; `d8 00 19` (`SendMcuAudioch(0)` 0x31dfc8); 0x473fd4(0); later `01 00`.

**For comparison, Bluetooth music** (same function, call state 0): ADC1 source 2, media 4 line-in route, `d8 00 19` (gain 25), +4 dB input gain, no `AUDIF_Set(0x52)` and no master-volume override.

- That the chip's DAC (AC6965A pins 11 DACL / 12 DACR) feeds SoC ADC1 input 2 is **inferred** (stock uses source 2 for every Bluetooth sound; no other path exists in the code). That the `ac` link never carries SCO audio is **proven** (no data-bearing call event).
- `d8` is not an `ac` frame, but `audio.md` lists it under "never send from experiments" (untested; may leave Bluetooth audio misrouted).
- Open: the meaning of DSP front channel-select mode 2 (mono voice to the front pair is a guess); whether the DSP master stays at 0 dB during the call or follows the call volume through the 0x8010 message posted by 0x473dbe (handler not followed).

## 5. Microphone (question 3)

- **Nothing on the SoC side for calls (proven for the code read):** `btap_audio_switch`, `btap_switch_to_call`, `btap_back_from_call`, the Bluetooth source hooks and every event handler touch only the ADC1 input mux, the media 4 playback route, DSP mode 6/0x0c, volume and MCU mute. No record ("uplink") track, no ADC capture, no I2S or DAC feed toward the chip, no mic gain.
- **No mic gain, AEC or noise-suppression command (proven to the level of static callers):** every selector class that has a sender is accounted for in section 2. Classes 0x02, 0x06, 0x07, 0x08, 0x0a, 0x1a and 0x25 have no caller at all; slots +0xb0 (`ac 23`), +0xb4 (`ac 24`) and +0xd0 (`ac 04`) are never referenced. The SoC DSP has an AEC parameter area (0x3ae23a) and "stereo aec for music" (0x3a5b80), which belong to the SoC uplink tracks, not to Bluetooth calls.
- **Physical wiring (inferred, strong):** the AC6965A has a MIC input (pin 8) with MIC_BIAS on PA2 (datasheet pin table), the unit ships an external microphone for its rear MIC jack, and stock does no SoC-side mic work during calls. So the jack goes to the AC6965A and any AEC/NS runs in the Jieli firmware. Confirm with an unpowered continuity check from the MIC jack tip to AC6965A pin 8.
- The SoC does record a microphone for projection voice (CarLife "carlife mic" record track 0x3a23f2 → 0x3a578e, 16 kHz mono; AA "uplink" track in `android_auto_mw.c` 0x391798). Where that signal enters the SoC is the subject of `notes/aa_mic.md`; one possibility consistent with this note is the chip passing its mic through its DAC into ADC input 2. Open.

## 6. During Android Auto (question 4)

- **Session bracket (proven code, inferred meaning):** AA start (`AndroidAutoMain` 0x3908ae, 0x390bd6) sends `ac 14` (slot +0xa0 0x38852c, after a 600 ms timed mute) when Bluetooth is connected and sets 0x6de4b4; AA exit (0x38fbbe, 0x38fcd4) sends `ac 13` (slot +0x9c 0x388564). Probably tells the chip a projection session owns media. Our payload has never sent either and wireless AA works. `audio.md` says `ac 14` was seen at AA exit; the code sends it at start (open).
- **Device type** 0x31c454 (RAM 0x6dce1c) is 0x0a or 0x0b during an AA session (set by 0x38f5b6, 0x0b when 0x391d26() == 2) (proven values, inferred: wired / wireless).
- **Call start during AA** (`btap_switch_to_call` 0x385806, device type 0x0a / 0x0b; proven):
  1. If a call is already being handled (0x6d715a), nothing.
  2. Normal case, AA media over the AA link (0x6d7114 = 0): 0x38f622 sends AA key 0x100 down and then up through the AA middleware (0x392264 → module). The module's translation (android_auto module code at 0x5b20) maps 0x100 to **KEYCODE_MEDIA_PAUSE (127)**; its other entries are 1 HOME, 2 BACK, 4 SEARCH, 8 CALL, 0x10 ENDCALL, 0x20 MEDIA_NEXT, 0x40 MEDIA_PLAY, 0x80 MEDIA_PLAY_PAUSE, 0x200 MEDIA_PREVIOUS. Then the countdown 0x6d7131 = 60; when the MCU service loop runs it out (0x31eb1a..0x31eb48) and the call is still up, `btap_audio_switch(1, 1)` (section 4).
  3. If an AA session had the Bluetooth analog route up (0x6d7114 = 1, set by 0x390e9a), `btap_audio_switch(1, 1)` at once.
  4. In-call flag 0x6d715a = 1; if the current source is not AA (0x0a), switch to it (0x32d0f2(0x0a, 0x0a)), so the phone's projected call screen is in front. The SoC draws no call UI of its own in this case.
- **No explicit stop of AA audio tracks.** Quiet comes from the pause key, the phone's own audio focus during a call, and the timed `01 01`. Routing media 4 also replaces the routed media in RISC 7, so the AA PCM stream on media 1 stops reaching the output while the call route is up (inferred from the `AUDIF_Set(0x24)` decode in `audio_dsp.md`).
- **Call end during AA** (0x385b0a): `btap_audio_switch(0, 0)`, clears 0x6d715a and the countdown. No play key is sent; resuming media is left to the phone.
- The AA focus callback (0x38ffe4..0x390014) tears the Bluetooth analog route down (0x3ac5b0, 0x3ab510(0)) when AA audio channels start while 0x6d7114 is set.
- With AA projecting, the phone shows its own answer / decline / hang-up controls and receives touches over AA, then drives HFP itself. So stock's `ac` call-control frames are only needed for its own phone UI and hardware keys.

## 7. Plan for our payload (question 5)

Order: receive-only first, SoC-only audio second, new `ac` frames last and one approval at a time. Each step is a short check with the owner placing or receiving one call; no soaks.

**Step 1: parse and show call state (no new frames).** `net.c` `plat_mcu_bt`: handle 0x07, 0x08, 0x09, 0x0e, 0x05, 0x06, 0x0d, 0x16 and 0x01 next to the existing 0x10 / 0x18..0x1b. Keep a call state (0 idle, 1 outgoing, 2 ringing, 3 active) and the number (bounded by the frame length, printable ASCII only), log transitions, expose them in the HU1 status, and show a banner in the UI. Check: the log for one incoming and one outgoing call; this also settles the inbound sequences in section 3.

**Step 2: call audio route (SoC only).** `audio.c`: `aud_bt_route(on)` as a port of section 4, without `d8`: MCU `01 01` (already used for mute); ADC1 input 2 (0x9C001FEC bits 21:20 and 17:16); media 4 line-in route (stop media 4, route media 4 to the main output with RISC 7 / 0x11 / 0x3d as `audio_dsp.md` section 7 decodes for media 1, App 0x6a = 0x800, 5/2 and 5/4, start), input gain +4 dB, DSP 6/0x0c/2, master volume from our own saved call volume; then `01 00`. Off: DSP 6/0x0c/0, route media 1 (AA PCM) back, `01 00` after a short mute. `net.c` turns it on at call state 1/2/3 (ringing, so an in-band ring tone is heard) and off at 0. Call volume in this step is our DSP master volume only. Check: incoming call answered on the phone or the AA screen, voice heard in the car; the far end hears the car mic if it is wired to the chip.

**Step 3: Android Auto during calls (no `ac` frames; AA protocol only).** `aa.c`: on call start send KEYCODE_MEDIA_PAUSE (127) down and up with `aa_send_key` (the input service now advertises only HOME, so add 127, and CALL / ENDCALL if wanted); stop mixing AA media into the mixer while the Bluetooth route is up and keep guidance muted; keep the AA video in front. `ac 14` / `ac 13` are not needed. Check: music pauses at the ring, resumes after the call (phone's decision).

**Step 4: our own call controls (needs owner approval, frame by frame).** `mcu.c` / `net.c` helpers plus UI buttons and, later, steering-wheel keys. With AA projecting none of these is needed.

| Frame | Purpose | Status |
|---|---|---|
| `ac 0f` | answer | **needs owner approval** |
| `ac 10` | reject / hang up | **needs owner approval** |
| `ac 0d <len> <digits>` | dial | **needs owner approval** |
| `ac 0e` | redial | **needs owner approval** |
| `ac 20 01 <digit>` | DTMF | **needs owner approval** |
| `ac 1b` | mic mute toggle | **needs owner approval** |
| `ac 17` | audio transfer car ↔ phone | **needs owner approval** |
| `ac 22 01 01` / `ac 22 00` | phone voice assistant | **needs owner approval** |
| `d8 00 <g>` | call volume on the chip (not an `ac` frame; `audio.md` lists it as untested) | **needs owner approval** |
| `01 01` / `01 00` | MCU mute around route changes | already used by the payload |
| `ac 19`, `ac 30`, `ac 29 04`, `ac 2a` / `ac 2b` | name, reconnect, RFCOMM | already allowed |

Never: `ac 16` and `ac 15` (phonebook download; they switch UART1 between 460800 and 115200 through 0x521da4 / 0x521dc0), `ac 1d` / `ac 1e` / `ac 1f` (call-log lists, only meaningful with the phonebook), any MCU upgrade frame (`e5`, `MCU_SY260L.BIN`), and the unexplained device-list frames `ac 18 <len> <bdaddr>` and `ac 1c <bdaddr 6>`.

## Open questions

- Continuity: rear MIC jack tip to AC6965A pin 8; chip pins 11/12 to the SoC ADC input that `AUDIF_Set(0x39, 1, 2)` selects.
- The real inbound sequences for an incoming, outgoing, rejected and handset-answered call (step 1 log); whether `ac 0e` or `ac 09` follows an answer.
- Whether the phone puts the ring tone in-band over SCO, or the chip plays its own ring through its DAC.
- Meaning of DSP 6/0x0c/2 and whether stock's DSP master stays at 0 dB during a call.
- What `ac 14` / `ac 13` change in the chip during AA, and the AA versus capture discrepancy about when `ac 14` is sent.
