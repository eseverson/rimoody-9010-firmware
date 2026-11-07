# Rimoody 9" 9010 head unit: custom firmware

Reverse engineering and replacement firmware for the **Rimoody 9" single-DIN car stereo, model 9010** (printed on the front bezel), sold as "Rimoody 9'' Single Din Car Stereo Wireless CarPlay Android Auto Detachable". It runs on a Sunplus SPHE8268K SoC with stock eCos firmware. This one is installed in the owner's Miata. The stock firmware stays in NOR flash; a patched U-Boot boot script loads our own bare-metal payload (`isp/sd-10-net`) from the SD card instead. The payload runs FreeRTOS on its own NDS32 port with our own USB, Wi-Fi, SD/FAT, display, touch, audio and H.264 drivers, shows an LVGL UI, runs wireless Android Auto, and is driven from a PC over Wi-Fi with `tools/hu.py`. Linux is not an option: the CPU has an MPU, not a paging MMU (`notes/linux_feasibility.md`).

Working on the unit today:

- Boot: power-on runs the NOR boot script (flash 0xE00000), which loads `/BOOT.BIN` from the card to 0x01800000 and jumps to it; with no `BOOT.BIN` it falls through to stock eCos (`isp/sd-11-bootcmd/boot.txt`).
- FreeRTOS V11.1.0 with our NDS32 port, own EHCI host and USB enumeration, RTL8188F Wi-Fi driver with WPA2 (software CCMP), lwIP, own access point mode and DHCP server.
- HU1 control protocol over UDP: live payload replacement (`hu.py exec`), persisting to the card, logs, register access, test ops.
- Robustness: hardware watchdog, crash records reported by the next boot, config handoff and probation across EXEC, failure resets with backoff.
- Own SD driver and FatFs (verified writes, `hu.py fs put`, `hu.py persist`).
- LVGL v9 UI on the OSD0 layer: home screen with dock, settings (Wi-Fi, display brightness and theme, audio EQ, Android Auto), text console view, GT911 touch and the five front touch buttons.
- Audio: bare-metal DSP bring-up, mixer for AA media / guidance / system audio, 10-band EQ.
- Hardware H.264 decoding (MFD) shown full rate through the VPP0 video layer.
- Wireless Android Auto end to end with a real phone: Bluetooth RFCOMM handshake through the AC6965A, Wi-Fi either via the home network or the unit's own "car network" access point, TLS, video, audio and touch.
- Bluetooth: name sent at boot, reconnect to the last phone after power-up with `ac 30` (awaiting validation in the car).

In progress: occasional audio skips during AA (`notes/android_auto.md`, "Stutter hunt"), VPP0 tearing and downscaling, AES-GCM throughput limiting AA video, and Wi-Fi join reliability.

## Hardware

Identifying marks, for matching other units (the same board likely ships under other brands):

- Front bezel: "9010", plus "cmlplay" on the front of the unit.
- Main board silkscreen: `7388-2900-260L V03`, dated `20240504`.
- Stock firmware: eCos on the SPHE8268K, with a touch-panel profile named `TP_9010` among others.

| Part | Role and key facts |
|---|---|
| Sunplus SPHE8268K ("gemini") | AndesCore N13 (AndeStar V3, NDS32), 270 MHz, 8 KiB I/D caches, MPU only, no FPU; 64 MB in-package DRAM (0x04000000..0x07FFFFFF alias low memory, above that hangs the bus); H.264 decoder, audio DSP, multi-layer display (OSD0 + VPP0), two EHCI hosts, SD controller; register map largely SP7021-compatible. Details: [docs/SPHE8268K-datasheet.md](docs/SPHE8268K-datasheet.md), [block diagram](docs/sphe8268k-block-diagram.png). |
| Serial NOR flash (16 MB) | xboot, U-Boot 2014.04 (vendor), stock eCos. Default env patched to `bootcmd=source 98e00000;bootsp` (`tools/patch_uboot.py`, `tools/flash_bootcmd.py`). Full dumps are kept locally (`dump*.bin`, gitignored). |
| Realtek RTL8188FTV | USB Wi-Fi dongle on EHCI1 (0bda:f179); firmware `isp/sd-10-net/rtl8188fufw.bin`. |
| Jieli AC6965A | The "MCU": Bluetooth plus ACC, backlight, mute and RTC, over UART1 at 115200 (`ff aa` frames, `tools/mcuframe.py`). It cuts SoC power if the heartbeat stops. Notes: [notes/audio.md](notes/audio.md), [notes/android_auto.md](notes/android_auto.md). |
| Panel and touch | 1024x600 TTL panel; Goodix GT911 on bit-banged I2C (GPIO90-93), as the stock firmware does it (its hardware I2C masters serve only the FM tuner); the five front buttons are touch areas right of the display. [notes/display_touch.md](notes/display_touch.md). |

## Repository layout

| Path | Contents |
|---|---|
| `isp/sd-10-net/` | The payload (main code; the name is historical). Built into `NET.BIN`. |
| `isp/sd-1-probe` .. `isp/sd-12-boot`, `isp/0-reset`, `isp/1-probe`, `isp/2-nordump` | Earlier SD-card ISP rounds (U-Boot scripts run through the update path) used before the Wi-Fi loop existed: probing, NOR dumps, first Wi-Fi, MCU, boot command. Mostly history. |
| `isp/sd-11-bootcmd/boot.txt` | The NOR boot script that loads `/BOOT.BIN`. |
| `isp/mkisp.py` | Builds an `ISP_UPDT.BIN` from the stock dump plus a U-Boot script. |
| `isp/ecos-snap/` | Hooks that boot stock eCos and snapshot registers or log calls (`tools/ecos_snap.py`). |
| `tools/` | PC-side tools, see below. |
| `notes/` | Reverse-engineering notes and status logs, see the index below. |
| `docs/` | SoC datasheet (written from our findings) and the UI design spec (`docs/superpowers/specs/`). |
| `miata.svg` | Logo source for `tools/mklogo.py`. |

Important files in `isp/sd-10-net/`:

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

## Building

The payload is cross-compiled with an NDS32 little-endian bare-metal toolchain (`nds32le-elf-`, gcc 14 with newlib). The Makefile looks for it at `$(HOME)/nds32/bin/nds32le-elf-`; point `CROSS` elsewhere if yours lives somewhere else:

```sh
make -C isp/sd-10-net CROSS=/path/to/nds32/bin/nds32le-elf-
```

`make` produces `NET.BIN` (plus `net.dis`, `net.map`, `TRAMP.BIN`); mbedtls and LVGL build into their own `-O2` archives, so the first build takes a while. Every build carries a timestamp build id (`BUILD_ID`), shown by `hu.py ping` and the hello broadcast. Build outputs are gitignored.

Host tests run on the PC with plain gcc, python3 (with the `cryptography` package) and openssl: `make -C isp/sd-10-net test`. They cover crypto, WPA, IP, MCU frames, the console, the H.264 parser, lwIP TCP, the wireless AA handshake, AP mode, DHCP, osdvid de-tiling, TLS and the AA stack over TCP against a scripted phone.

## Running and day-to-day

The card holds `/BOOT.BIN` (the payload) and `/WIFI.TXT`, plus files the payload writes itself (`UI.TXT` settings, `AP.TXT` car network, logs). After power-on the payload joins the home Wi-Fi, broadcasts `HU1 hello` on UDP 5152 and serves UDP 5151. Requests need the token from `WIFI.TXT`:

```sh
export HU_TOKEN=...              # the token= line of WIFI.TXT
tools/hu.py listen               # wait for the hello, shows the unit and its build
tools/hu.py ping
tools/hu.py exec isp/sd-10-net/NET.BIN     # replace the running payload over Wi-Fi, wait for the new build's hello
tools/hu.py persist isp/sd-10-net/NET.BIN  # make it the card's /BOOT.BIN (read back and CRC-checked)
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
- `ecos_snap.py`: boots stock eCos with a register-snapshot hook (`isp/ecos-snap`).
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
- [audio.md](notes/audio.md): audio path overview, MCU commands, amplifier, GPIOs.
- [audio_dsp.md](notes/audio_dsp.md): DSP bring-up and PCM playback from stock eCos.
- [audio_eq.md](notes/audio_eq.md): stock EQ and tone controls, our EQ implementation.
- `audio_trace/`: call and register traces from stock eCos runs.
- [console.md](notes/console.md): framebuffer text console.
- [display_touch.md](notes/display_touch.md): panel timing, OSD0, GT911 touch, front buttons, display haze notes.
- [h264_decoder.md](notes/h264_decoder.md): the MFD hardware decoder, reverse-engineered from eCos.
- [h264_diff.md](notes/h264_diff.md): eCos decoder setup versus `vdec.c`.
- [h264_fields.md](notes/h264_fields.md): SPMP slice command fields mapped to H.264 syntax.
- [interrupts.md](notes/interrupts.md): interrupt controller, tick, exceptions, crash records.
- [linux_feasibility.md](notes/linux_feasibility.md): why Linux is ruled out (MPU, no MMU).
- [memory_map.md](notes/memory_map.md): 64 MB DRAM map and aliasing (source of truth: `memmap.h`).
- [rtos.md](notes/rtos.md): FreeRTOS port design, milestones, EHCI/USB/SD bring-up, lockup handling.
- [session-state.md](notes/session-state.md): early session log (2026-10-31 to 11-01); outdated, kept for history.
- [usb_host.md](notes/usb_host.md): U-Boot's USB PHY/EHCI init sequence.
- [vpp0_display.md](notes/vpp0_display.md): VPP0 bring-up from first probes to live AA video at 30 fps.
