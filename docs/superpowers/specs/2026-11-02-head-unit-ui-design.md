# Head unit UI: design and working plan

Status 2026-11-02: owner described the experience and handed the rest to me ("we can iterate, start working on your own"). Calls marked *(my call)* are mine, open to change. Progress is tracked in the checklist at the end; this document is the durable record.

## Intent

The unit lives in Android Auto: the owner uses maps and audio control there almost all the time. Everything else is a calm, minimal shell around it: the Miata logo, a way out to a few settings, and the front buttons doing what their labels say.

## The experience (owner's words unless marked)

- **Boot / home:** the Miata logo (`miata.svg`), inverted to white on black. It stays as the home screen, with a **faint dock** of round buttons along the bottom that brightens when touched (mockup option A).
- **Android Auto:** the main state. *(my call)* When a phone session starts the unit switches to it on its own; when the phone leaves it goes back home. AA video is full screen through VPP0 (centre 1024x600 of the 1280x720 stream, 1:1).
- **Front capacitive buttons** (top to bottom):
  1. power symbol: short = mute toggle; long = reboot
  2. Home: always back to the logo home screen (out of AA)
  3. Back: in AA, the phone's back key; in our screens, back
  4. volume up, 5. volume down, with a volume overlay
- **Settings** (owner's list):
  - Wi-Fi: connect to a specified network
  - Android Auto: disable switch
  - Display: brightness, light/dark mode
  - Equalizer: maybe (placeholder until audio exists)
  - Commands: boot to the stock firmware, reboot
  - Install: the NOR boot-script patch (what we did by hand) plus a stubbed "write the app to flash permanently", for someone who starts from the SD card and later decides to commit
  - Lower priority: background image selection, etc.
- **Volume overlay:** translucent, over everything including AA. *(my call)* Level 0..40 (eCos's DSP volume index scale), auto-hides after ~1.5 s.

## Non-goals (now)

- Audio output (SoC audio DSP microcode, DAC/I2S): **later, its own project** (owner). Volume/mute are state + overlay only until then.
- eCos's other features (radio, CarPlay, AirPlay mirroring, Bluetooth calls/music, reverse camera, media player, AUX, CAN): **hold off** (owner).
- Animated / 60 fps UI: the CPU's memory writes are slow (37 ms per 768 KB in osdvid); keep screens mostly static and redraw only what changes *(my call)*.

## How it gets made *(my call; owner doesn't mind)*

- **LVGL v9.2.2**, vendored into `isp/sd-10-net/lvgl/` (src only) and built into its own archive like mbedtls.
- **Display:** LVGL renders ARGB8888 directly into the OSD0 framebuffer (0x03400000, 1024x600x4, region header 0x03660000, as `fb_enable`), `LV_DISPLAY_RENDER_MODE_DIRECT`; the flush callback writes back the D-cache for the dirty area. OSD0 is the top layer over VPP0; transparent/translucent pixels show the AA video underneath (per-pixel alpha in the OSD region; to be verified on hardware).
- **Tasks:** a `ui` FreeRTOS task runs `lv_timer_handler`; it replaces the console's drawing while the UI is on. The text console stays available as a debug screen.
- **Input:** the GT911 touch task feeds a pointer input device (panel area x < 1024); presses in the button strip (x 1054..1061, y 141/211/322/404/508) become button events handled by the UI (short/long press). While AA is shown, panel touches keep going to the phone.
- **Assets:** `tools/mklogo.py` rasterizes `miata.svg` (ImageMagick) to an inverted A8/ARGB image as a C array at build time.
- **Backends:** Wi-Fi config writes `WIFI.TXT` on the card and reconnects; "boot stock firmware" sets a one-shot card flag that the payload's early boot path turns into U-Boot's `bootsp ecos nor`; reboot = clean dongle power-off then reset; brightness through the backlight PWM (0x9C007A08) or the MCU `EE <level>` frame (to verify); install status reads the NOR boot script at 0x98E00000.

## Left to the builder

Exact layouts, colors and icon set; Wi-Fi network list vs manual entry first; where the console screen lives (dock "more" or Settings).

## Checklist

- [x] 1. LVGL builds in the payload; screens on the panel through OSD0 (build 20261102-001836; framebuffer screenshots via `hu.py ui shot`, input injection via `hu.py ui tap/button`). Real-finger touch still to confirm with the owner.
- [x] 2. Home: inverted logo + faint dock (AA, Settings, Console); front buttons handled (mute short / reboot long, home, back, vol ± with repeat)
- [x] 3. AA loop (build 20261102-002618, tools/aa_phone.py stream): auto-enter on the first decoded picture, Home leaves (VPP0 off), the dock's AA button returns, stream end goes home; OSD0 blends over VPP0 with per-pixel alpha (LVGL's bottom layer had to be made transparent: it picks up the theme's opaque screen colour). Back -> AA key 4 sent but not verified without a real phone.
- [x] 4. Volume overlay (visual + state; mute sends MCU 01 01/01 00)
- [~] 5. Settings (build 20261102-003431, persisted): Wi-Fi status + "Change" screen (SSID/password fields, on-screen keyboard; the check key rewrites WIFI.TXT's ssid=/psk=, keeps token=/ip=, restarts; connect not exercised on the unit), Android Auto switch, brightness slider (**not wired**: the PWM duty had no visible effect; the MCU `EE <level>` frame needs the owner present to try), dark mode switch (theme; the home screen stays black), equalizer placeholder, Reboot (verified, also the long press on button 1), Boot stock (one-shot ECOSSTOCK.FLG -> U-Boot `bootsp ecos nor`; **not exercised**: it leaves the unit in eCos until an ACC cycle), install status (reads the NOR boot script + bootcmd, verified "Installed") and the "Install to flash" stub.
- [x] 6. Console as a debug screen (dock, third button)

## Open items for the owner

- Real-finger touch and the front buttons by hand (all screens were driven by `hu.py ui tap/button/drag` so far).
- Android Auto with the phone: touch, the Back button (AA key 4), auto-enter/leave.
- Backlight: try the MCU `EE <level>` frame with the owner watching (`EE FF` is off).
- Boot stock: try once (then ACC cycle back).
- Later: audio output (then volume/mute/EQ become real), a Wi-Fi network list from a scan, background image.
