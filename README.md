# Rimoody 9" 9010 head unit: custom firmware

Reverse engineering and replacement firmware for the **Rimoody 9" single-DIN car stereo, model 9010** (printed on the front bezel), sold as "Rimoody 9'' Single Din Car Stereo Wireless CarPlay Android Auto Detachable". It runs on a Sunplus SPHE8268K SoC with stock eCos firmware. This one is installed in the owner's Miata. The stock firmware stays in NOR flash; a patched U-Boot boot script loads our own bare-metal payload (`firmware`) from the SD card instead. The payload runs FreeRTOS on its own NDS32 port with our own USB, Wi-Fi, SD/FAT, display, touch, audio and H.264 drivers, shows an LVGL UI, runs wireless Android Auto, and is driven from a PC over Wi-Fi with `tools/hu.py`. Linux is not an option: the CPU has an MPU, not a paging MMU (`notes/linux_feasibility.md`).

A one-minute tour of what is built (UI captures from the unit; the video slides use a synthetic test stream):

https://github.com/user-attachments/assets/7f5d5dc7-6e97-40d7-84ee-3d8b79288821

## Install on your head unit

For a Rimoody 9010 that runs its original firmware. This is unofficial firmware: read the risks below first.

You need a microSD card of 32 GB or less, formatted FAT32 (not exFAT), and a PC.

1. Download the release zip (`rimoody-9010-firmware-<build>.zip`) and extract it. Its `INSTALL.txt` has these steps too.
2. Format the card, so it starts empty. Copy `ISP_UPDT.BIN` and `ISPBOOOT.BIN` to it first, then `BOOT.BIN`.
3. Rename `WIFI.TXT.example` to `WIFI.TXT`, fill in your Wi-Fi name and password and a token of your own, and copy it to the card. The current firmware does not start without this file. A network that is out of range is fine: the unit then runs its own "car network" for Android Auto.
4. Put the card in, switch the unit on, and accept the software update it offers.
5. The unit restarts into this firmware, running from the card. The installer deletes its own two files from the card and changes nothing in the unit's flash.
6. Right away, open Settings > Flash and press Set up on "Boot from the SD card, else from flash". It has to finish within about two minutes of accepting the update. After an update, the unit's power controller switches the unit off and on again by itself.
7. After that restart, every power-on starts this firmware from the card. If Set up did not finish in time, the original firmware comes back. Start over from step 2.
8. Recommended: Settings > Flash > Back up, then keep `NORDUMP.BIN` from the card on your PC. It is what a flash programmer needs to repair the unit.
9. Optional: Settings > Flash > Install copies the firmware into flash, so it starts even without the card.

Android Auto needs the unit's own certificate (`AACERT.PEM`, `AAKEY.PEM`). The firmware takes it from the unit's flash at first start, so there is nothing to copy.

Going back to the original firmware:

- Settings > System > Boot stock starts it once, until the next ignition cycle.
- Taking `BOOT.BIN` off the card does the same until you put it back. If you used Install, remove the flash copy first (Settings > Flash > Remove on its row).
- Settings > Flash > Remove on the boot row makes it permanent. From the next power-on only the original firmware starts, until you run the installer again.

What works is listed below. Much of the stock feature set is not built yet: no Bluetooth calls or music outside Android Auto, no CarPlay, radio, reverse camera or steering-wheel buttons ([Stock features not built yet](#stock-features-not-built-yet)).

Risks:

- It has been tested on one unit. Units sold as the 9010 may differ. Set up checks the flash layout and refuses anything it does not recognize.
- Set up and Remove rewrite U-Boot's settings in flash. If the power drops during those one to three seconds, the unit no longer starts, and only a flash programmer (for example a CH341A clip) with a backup can repair it. Don't run them while starting the engine, or close to the restart two minutes after the update.
- Install the flash copy only after the card version has run well for a while. Once a copy is in flash, taking the card out no longer gets you back to stock.

## What works today

- Boot: power-on runs the NOR boot script (flash 0xE00000), which loads `/BOOT.BIN` from the card to 0x01800000 and jumps to it; with no `BOOT.BIN` it falls through to stock eCos (`isp/bootcmd/boot.txt`).
- FreeRTOS V11.1.0 with our NDS32 port, own EHCI host and USB enumeration, RTL8188F Wi-Fi driver with WPA2 (CCMP in the chip's hardware for unicast data, software for group traffic), lwIP, own access point mode and DHCP server.
- HU1 control protocol over UDP: live payload replacement (`hu.py exec`), persisting to the card, logs, register access, test ops.
- Robustness: hardware watchdog, crash records reported by the next boot, config handoff and probation across EXEC, failure resets with backoff.
- Own SD driver and FatFs (verified writes, `hu.py fs put`, `hu.py persist`).
- LVGL v9 UI on the OSD0 layer: home screen with dock, settings (Wi-Fi, display brightness and theme, audio EQ, Android Auto), text console view, GT911 touch and the five front touch buttons.
- Audio: bare-metal DSP bring-up, mixer for AA media / guidance / system audio, 10-band EQ.
- Hardware H.264 decoding (MFD) shown full rate through the VPP0 video layer.
- Wireless Android Auto end to end with a real phone: Bluetooth RFCOMM handshake through the AC6965A, Wi-Fi either via the home network or the unit's own "car network" access point, TLS, video, audio and touch, and the microphone for the assistant (front pinhole mic through the DSP's hands-free uplink).
- Bluetooth: name sent at boot, reconnect to the last phone after power-up with `ac 30` (awaiting validation in the car).

AA video throughput: TLS decryption in software (own AES-GCM in `gcm_alt.c`), delayed TCP ACKs, queued USB transmits and background decoding take a 720p stream to 81-87 fps with AES-128-GCM (103-108 fps with ChaCha20-Poly1305), well above the phone's 30-60 fps.

In progress: occasional audio skips during AA (`notes/android_auto.md`, "Stutter hunt"), VPP0 tearing and downscaling, and Wi-Fi join reliability.

## Stock features not built yet

The stock firmware does more than this firmware so far. Booting stock is one tap away (Settings > System > Boot stock) for anything below.

| Stock feature | Status here |
|---|---|
| Bluetooth hands-free calls | Not built. The call frames, call events and audio route are worked out from the stock firmware ([notes/bt_calls.md](notes/bt_calls.md)); in AA the phone shows its own call screen, but call audio has to be routed in. |
| Bluetooth music (A2DP) outside Android Auto | Not built. Same analog path from the Bluetooth chip as calls. |
| Apple CarPlay (wired and wireless) | Not planned yet. Stock ships CarPlay, AirPlay and iAP2 modules; nothing reverse engineered. |
| Wired Android Auto (USB) | Not built; wireless AA only. |
| Other phone mirroring (CarLife, the AutoLink app, Miracast) | Not planned. |
| FM radio (with RDS) | Not built. The tuner sits behind the SoC's hardware I2C and feeds the SoC's audio input; not started. |
| AUX input | Not built. Same audio input mux as the radio, partly mapped ([notes/audio.md](notes/audio.md)). |
| USB and SD media player (music, video) | Not built. |
| Reverse camera | Not built. Stock has a camera option and a video input; not looked at. |
| Steering-wheel buttons | Not built. Stock learns them from two ADC lines ([notes/display_touch.md](notes/display_touch.md)); `tools/adc.py` samples those lines. |
| IR remote, CAN bus, TV/DTV | Factory options in the stock configuration; whether this board has the hardware is unknown. |
| Stock settings (EQ presets, tone, clock, factory menu) | Partly replaced by our own settings (EQ, display, Wi-Fi); the clock and factory menu are not. |

## Hardware

Identifying marks, for matching other units (the same board likely ships under other brands):

- Front bezel: "9010", plus "cmlplay" on the front of the unit.
- Main board silkscreen: `7388-2900-260L V03`, dated `20240504`.
- Stock firmware: eCos on the SPHE8268K, with a touch-panel profile named `TP_9010` among others.

| Part | Role and key facts |
|---|---|
| Sunplus SPHE8268K ("gemini") | AndesCore N13 (AndeStar V3, NDS32), 270 MHz, 8 KiB I/D caches, MPU only, no FPU; 64 MB in-package DRAM (0x04000000..0x07FFFFFF alias low memory, above that hangs the bus); H.264 decoder, audio DSP, multi-layer display (OSD0 + VPP0), two EHCI hosts, SD controller; register map largely SP7021-compatible. Details: [docs/SPHE8268K-datasheet.md](docs/SPHE8268K-datasheet.md), [block diagram](docs/sphe8268k-block-diagram.png). |
| Serial NOR flash (16 MB) | xboot, U-Boot 2014.04 (vendor), stock eCos. Default env patched to `bootcmd=source 98e00000;bootsp` (`tools/patch_uboot.py`, `tools/flash_bootcmd.py`). Full dumps are kept locally (`dump*.bin`, gitignored). |
| Realtek RTL8188FTV | USB Wi-Fi dongle on EHCI1 (0bda:f179); firmware `firmware/rtl8188fufw.bin`. |
| Jieli AC6965A | The "MCU": Bluetooth plus ACC, backlight, mute and RTC, over UART1 at 115200 (`ff aa` frames, `tools/mcuframe.py`). It cuts SoC power if the heartbeat stops. Notes: [notes/audio.md](notes/audio.md), [notes/android_auto.md](notes/android_auto.md). |
| Panel and touch | 1024x600 TTL panel; Goodix GT911 on bit-banged I2C (GPIO90-93), as the stock firmware does it (its hardware I2C masters serve only the FM tuner); the five front buttons are touch areas right of the display. [notes/display_touch.md](notes/display_touch.md). |

## Repository layout

| Path | Contents |
|---|---|
| `firmware/` | The payload (main code). Built into `NET.BIN`. |
| `isp/bootcmd/boot.txt` | The NOR boot script that loads `/BOOT.BIN` (version 1, SD card only). Version 2 (SD card, then a payload copy in NOR, then stock) is built into the payload and installed from Settings > Flash or `hu.py nor install` ([notes/nor_install.md](notes/nor_install.md)). |
| `isp/mkisp.py` | Builds an `ISP_UPDT.BIN` (the stock SD-card update path) from the stock dump plus a U-Boot script: the recovery route if the NOR boot script is lost. The early ISP rounds that used it (probing, NOR dumps, first Wi-Fi, MCU) are in git history before the move to `firmware/`. |
| `tools/ecos-snap/` | Hooks that boot stock eCos and snapshot registers or log calls (`tools/ecos_snap.py`). |
| `tools/` | PC-side tools, see below. |
| `notes/` | Reverse-engineering notes and status logs, see the index below. |
| `docs/` | SoC datasheet (written from our findings) and the UI design spec (`docs/superpowers/specs/`). |
| `miata.svg` | Logo source for `tools/mklogo.py`. |

Important files in `firmware/`:

| File | Purpose |
|---|---|
| `Makefile`, `payload.ld`, `head.S` | Build; raw image linked and entered at 0x01800000. |
| `memmap.h`, `memmap_check.c` | The single DRAM map; compile-time checks for order, overlap and the U-Boot boundary ([notes/memory_map.md](notes/memory_map.md)). |
| `net.c` | Boot, main/net task, HU1 command server, platform ops, MCU and Bluetooth handling. |
| `irq.S`, `irq.c`, `clk.c`, `freertos/port/` | Interrupts, tick, watchdog, time base, FreeRTOS NDS32 port. |
| `ehci.c`, `usbh.c` | EHCI transfers and USB enumeration. |
| `wpa.c`, `crypto.c`, `ip.c`, `wpa_ap.c`, `ap80211.c`, `dhcps.c`, `lwnet.c` | Wi-Fi station (WPA2, CCMP), AP mode, DHCP server, lwIP glue. |
| `sd.c`, `diskio.c`, `fatfs/` | SD driver and FatFs. |
| `aa.c`, `aa_tls.c`, `aa_tcp.c`, `aa_wl.c` | Android Auto protocol, TLS (mbedtls), TCP transport, wireless handshake over RFCOMM. |
| `vdec.c`, `h264p.c` | Hardware H.264 decoder and header parser. |
| `vpp.c`, `osdvid.c` | Video on VPP0 (the goal) and through OSD0 (fallback). |
| `audio.c` | DSP bring-up, mixer, EQ. |
| `ui.c`, `ui_logo.c`, `lv_conf.h`, `console.c` | LVGL UI and the text console. |
| `mcu.c`, `gt9.c` | AC6965A frames, GT911 touch. |
| `tramp.c` | EXEC trampoline, embedded in `NET.BIN`. |
| `freertos/`, `lwip/`, `mbedtls/`, `lvgl/`, `fatfs/` | Vendored, unmodified (FreeRTOS V11.1.0, lwIP 2.2.0, Mbed TLS 3.6.2, LVGL v9.2.2, FatFs R0.15a; see each `VERSION` / `README.vendor`). |
| `test/` | Host tests and H.264 test streams (`test/h264/README.md`). |
| `WIFI.TXT.example` | Card config format (`ssid=`, `psk=`, `token=`, optional `ip=`). |

## Development

Everything below is for building and changing the firmware.

### Building

The payload is cross-compiled with an NDS32 little-endian bare-metal toolchain (`nds32le-elf-`, gcc 14 with newlib). The Makefile looks for it at `$(HOME)/nds32/bin/nds32le-elf-`; point `CROSS` elsewhere if yours lives somewhere else:

```sh
make -C firmware CROSS=/path/to/nds32/bin/nds32le-elf-
```

`make` produces `NET.BIN` (plus `net.dis`, `net.map`, `TRAMP.BIN`); mbedtls and LVGL build into their own `-O2` archives, so the first build takes a while. Every build carries a timestamp build id (`BUILD_ID`), shown by `hu.py ping` and the hello broadcast. Build outputs are gitignored. `make CARD_LOG=0` builds with log files on the SD card off by default (for published builds); Settings > System > "Save logs to the SD card" or `cardlog=` in `UI.TXT` overrides it. The in-memory log behind `hu.py log` and the console is always kept.

Host tests run on the PC with plain gcc, python3 (with the `cryptography` package) and openssl: `make -C firmware test`. They cover crypto, WPA, IP, MCU frames, the console, the H.264 parser, lwIP TCP, the wireless AA handshake, AP mode, DHCP, osdvid de-tiling, TLS and the AA stack over TCP against a scripted phone.

### Making a release

```sh
tools/mkrelease.py firmware/NET.BIN stock-dump.bin    # dist/rimoody-9010-firmware-<BUILD_ID>/ and .zip
```

The second argument is a 16 MB dump of a stock unit's NOR flash. Only its xboot and its U-Boot go into the installer, copied unchanged, and the tool refuses a U-Boot that fails xboot's word-sum check, lacks the verify flag or carries anything but the stock default environment. The installer is the stock SD-card update. eCos finds `ISP_UPDT.BIN` and offers the update. xboot then loads U-Boot from that file, and U-Boot runs the script stored in `ISPBOOOT.BIN`. The script marks the update done, deletes both installer files, and boots `/BOOT.BIN` from the card the way `isp/bootcmd/boot.txt` does. It contains no command that writes the flash. The tool checks that, along with the size limits and U-Boot's script checks, on the files as written. The reasoning is in the docstring of `tools/mkrelease.py`. Build published firmware with `make CARD_LOG=0`. `dist/` is gitignored.

### Running and day-to-day

The card holds `/BOOT.BIN` (the payload) and `/WIFI.TXT`, plus files the payload writes itself (`UI.TXT` settings, `AP.TXT` car network, logs). After power-on the payload joins the home Wi-Fi, broadcasts `HU1 hello` on UDP 5152 and serves UDP 5151. Requests need the token from `WIFI.TXT`:

```sh
export HU_TOKEN=...              # the token= line of WIFI.TXT
tools/hu.py listen               # wait for the hello, shows the unit and its build
tools/hu.py ping
tools/hu.py exec firmware/NET.BIN     # replace the running payload over Wi-Fi, wait for the new build's hello
tools/hu.py persist firmware/NET.BIN  # make it the card's /BOOT.BIN (read back and CRC-checked)
tools/hu.py fs put FILE [NAME]   # write a file to the card, both CRCs checked
tools/hu.py fs ls
tools/hu.py log                  # the run log
tools/hu.py -h                   # everything else: mcu, aa, vdec, vpp, osdvid, ui, audio, rtos, touch, irq, ...
```

The target address comes from `--ip`, `$HU_IP`, or the first hello heard. Typical loop: build, `exec`, check, and only `persist` a build once it has proven itself, since `persist` is what the next power-on runs.

`tools/hu_power.sh state|on|off|cycle [OFF_SECONDS]` switches the unit's bench supply through a Home Assistant switch (equivalent to an ACC cycle), configured by a gitignored `tools/ha.env`.

Other tools, one line each:

- `aa_phone.py`: stands in for the phone in wireless AA (TLS server, streams an H.264 file as AA video).
- `adc.py`: samples the SoC SAR ADC channels over Wi-Fi.
- `card.py`: loads and saves the SD card for ISP rounds; `card.py wifi` writes `WIFI.TXT`.
- `mkrelease.py`: builds the end-user release zip (see "Making a release").
- `ecos_snap.py`: boots stock eCos with a register-snapshot hook (`tools/ecos-snap`).
- `fb.py`: drives the OSD0 framebuffer directly.
- `flash_bootcmd.py`, `flash_script.py`, `patch_uboot.py`, `patch_card_uboot.py`: NOR U-Boot env and boot script patching (done; recovery is a CH341A and the original dump).
- `gt9.py`: early bit-banged GT9xx experiment from the PC (superseded by `gt9.c`; see its warning).
- `mcuframe.py`, `mcu_watch.py`, `mcu_try.py`: build/parse AC6965A frames, stream them live, per-boot MCU experiments.
- `mfd_detile.py`: de-tiles decoder output and compares it with an ffmpeg reference.
- `mklogo.py`: rasterizes `miata.svg` into `ui_logo.c`.
- `tcp_test.py`: lwIP TCP throughput and integrity test.
- `vdec_probe.py`, `vdec_regress.sh`: H.264 decoder register snapshot and regression over the test streams.
- `vpp_fresh.sh`: one VPP0 test per full chip reset.
- `walk.py`: reads an address range over Wi-Fi, surviving bus hangs and resets.

## Notes index

- [android_auto.md](notes/android_auto.md): AA options and decision, Bluetooth/RFCOMM protocol, wireless AA bring-up, car network, stutter hunt, reconnect with `ac 30`. The most current status log.
- [aa_mic.md](notes/aa_mic.md): microphone capture from the DSP's hands-free uplink, the +30 dB preamp boost and the AA mic channel.
- [audio.md](notes/audio.md): audio path overview, MCU commands, amplifier, GPIOs.
- [audio_dsp.md](notes/audio_dsp.md): DSP bring-up and PCM playback from stock eCos.
- [audio_eq.md](notes/audio_eq.md): stock EQ and tone controls, our EQ implementation.
- `audio_trace/`: call and register traces from stock eCos runs.
- [bt_calls.md](notes/bt_calls.md): stock Bluetooth hands-free call frames and audio route (research, not built yet).
- [console.md](notes/console.md): framebuffer text console.
- [display_touch.md](notes/display_touch.md): panel timing, OSD0, GT911 touch, front buttons, display haze notes.
- [h264_decoder.md](notes/h264_decoder.md): the MFD hardware decoder, reverse-engineered from eCos.
- [h264_diff.md](notes/h264_diff.md): eCos decoder setup versus `vdec.c`.
- [h264_fields.md](notes/h264_fields.md): SPMP slice command fields mapped to H.264 syntax.
- [interrupts.md](notes/interrupts.md): interrupt controller, tick, exceptions, crash records.
- [linux_feasibility.md](notes/linux_feasibility.md): why Linux is ruled out (MPU, no MMU).
- [memory_map.md](notes/memory_map.md): 64 MB DRAM map and aliasing (source of truth: `memmap.h`).
- [nor_flash.md](notes/nor_flash.md): the XM25QH128C SPI NOR, its controller, the guarded erase/program driver (`nor.c`, `hu.py nor`) and the free region for a NOR copy of the payload.
- [nor_install.md](notes/nor_install.md): install to flash: the version 2 boot script (SD card, then a payload copy in NOR, then stock), the U-Boot environment patch, the copy's header, every check, power-loss analysis and the on-unit test plan.
- [rtos.md](notes/rtos.md): FreeRTOS port design, milestones, EHCI/USB/SD bring-up, lockup handling.
- [session-state.md](notes/session-state.md): early session log (2026-10-31 to 11-01); outdated, kept for history.
- [usb_host.md](notes/usb_host.md): U-Boot's USB PHY/EHCI init sequence.
- [vpp0_display.md](notes/vpp0_display.md): VPP0 bring-up from first probes to live AA video at 30 fps.
