# Android Auto on the head unit

Status 2026-10-31: research; decision made (below). Sources: the 16 MB NOR dump (`dump1.bin`), eCos strings, the module filesystem below.

## What the stock firmware has

- **A real Android Auto implementation**, wired and wireless, as a loadable module: `android_auto_ROM.bin` (module_android_auto 2.1.0, version.json 2.1.159a, dated 2024-07-08). Decompressed it is 312848 bytes of NDS32 code with PolarSSL (built from `/home/lingyu.yan/module_core/external/polarssl`), the AA control protocol (`CONTROL_MESSAGE_TYPE__MESSAGE_ENCAPSULATED_SSL`, `AASslWrapper`), and an embedded PEM certificate and private key (the head unit's Android Auto identity).
- **Sibling modules** in the same filesystem: `airplay`, `carplay` (+ `iap2`), `carlife`, `autolink` (third-party mirroring app `com.link.autolink.pro`), `vautolink`, `wfd` (Miracast).
- **Hardware H.264 decode** driven by eCos (`h264/h264_vid.c`: `h264_create`, `h264_trigger_decode`, `H264_SpmpProgramStoreReg`, SPS/PPS/slice parsing in software, decode in hardware). Phone projection video goes through it.
- **USB accessory (AOA) host side** in eCos (`aoa_init_accessory`, `aoa_bulk_*`, "AOA::starting device in accessory mode").
- **Audio**: the AUDIF track API used by AA/CarPlay audio (notes/audio.md).

## Where it lives in flash

- Two JFFS2 filesystems (magic 0x1985, no compression at the JFFS2 level): 0xB40000 (config: `0.bin`, `0_v.bin`, `ir.bin`, `key.bin`, `tp.bin`, `usercfg.bin`) and **0xB60000 (projection modules)**, one directory per module with `<name>_ROM.bin` + `version.json`.
- Each `_ROM.bin` is LZMA-alone (props 0x5D, dict 8 MB). All but `android_auto` are XORed with one byte (airplay/carplay/wfd 0x4D, autolink/carlife 0x1D, iap2 0xCD, vautolink 0x6D).
- Extraction: a small JFFS2 parser (jefferson stops on repeated node versions), then de-XOR + `lzma.decompress(FORMAT_ALONE)`. Extracted copies are kept out of git (scratchpad), since they are vendor code and contain the AA private key.

## Can our firmware host the vendor module?

No, not in practice. Inside the module every `sethi/ori` call (3795 calls, 603 targets) stays within the module; calls into eCos are PC-relative `jal`, so the module is linked for one load address next to one exact eCos build. Hosting it means providing eCos's functions at eCos's addresses, i.e. running eCos.

## Options

| | What | For | Against |
|---|---|---|---|
| A | Our firmware for everything else; reboot into stock eCos for Android Auto | Works today (vendor AA, licensed cert, HW decode, audio) | Seconds of switch time; two systems; our UI is gone while AA runs |
| B | Run our code inside stock eCos (as a module like the `_ROM`s, or a task) instead of replacing it | AA, audio, video, radio keep working; our UI/apps on top | Not the "full replacement"; tied to closed eCos |
| C | Own Android Auto in our firmware | Fully ours, one system | Needs, all ours: USB host on the phone port + AOA, TLS (mbedTLS), the AA protocol (protobuf channels: control, video, audio, input, sensors), the **H.264 hardware decoder reverse-engineered** from eCos, audio output (DSP microcode, notes/audio.md), and a head-unit certificate |

The certificate for C: Google's phone app only talks to head units presenting a certificate signed by Google. The only one available is this unit's own (embedded in its AA module). Using it in our firmware on this same unit is the owner's call.

## Decision (owner, 2026-10-31)

**C: our own Android Auto stack, using this unit's certificate.** The embedded certificate is Google's reference head-unit certificate (subject O=Google-Linux-Reference, issuer O=Google Automotive Link, RSA-2048, sha256WithRSA, valid 2014-08-12 to 2044-08-11), with the Google Automotive Link root (2014-06-06 to 2044-06-05) and one private key. The key and certificate stay out of git and out of the payload image: the payload will load them from the SD card at runtime.

## Wireless only (owner, 2026-10-31)

USB Android Auto is not wanted, only wireless. How wireless AA works: the phone pairs with the head unit over Bluetooth, finds the AA wireless RFCOMM service (UUID 4de17a00-52cb-11e6-bdf4-0800200c9a66), and over RFCOMM receives the head unit's Wi-Fi access point (SSID, password, BSSID, security) and an IP/port; it joins the AP and opens a TCP connection, and from there the AA protocol is the same as over USB (same framing, TLS, channels).

The stock firmware does exactly this on this hardware: hostapd with the RTL8188F in AP mode (2.4 GHz; the chip has no 5 GHz) and an RFCOMM interface to the Bluetooth chip (`RFCOMM_IF_sendData`; the AA module's `AABluetoothEndpoint` / `AAWifiDiscovery` / `setWiFiInfo` / `setWiFiIPAndPort`). So 2.4 GHz wireless AA is accepted by phones, at least the ones the vendor tested.

Consequences for the plan: the USB phone port and AOA work is dropped (`test/aa_host.c` stays only as a protocol harness); `aa.c` is transport-independent and stays. New pieces:
1. **Wi-Fi AP mode** on our RTL8188F driver: beacons, probe/auth/assoc responses, a WPA2-PSK authenticator (the 4-way handshake from the AP side; wpa.c has the supplicant side), and a DHCP server for one client. Reference: Linux rtl8xxxu gained AP mode for the 8188F.
2. **TCP**: lwIP (raw API, in the net task) on a netif over our 802.11 data path, replacing ip.c's hand-rolled UDP/ARP/ICMP; HU1 moves onto it.
3. **Bluetooth RFCOMM** through the Jieli AC6965A: how eCos registers the AA service and moves RFCOMM data over the chip's link (being mapped). The Jieli's own firmware does the Bluetooth stack; we only get what its UART protocol exposes.
4. Decoder and audio as before.

## Plan for C (USB parts superseded by the wireless section above)

1. H.264 decoder: map `h264_vid.c` and the decoder register block from the eCos disassembly (bitstream feed, frame buffers, output format, how frames reach the display layer). Biggest unknown; decides feasibility.
2. Phone USB port: which controller (EHCI0 at 0x9C102100 is the candidate), generalize usbh.c, AOA switch (control requests 51/52/53).
3. TLS: mbedTLS client with client-certificate auth; NDS32 bignum speed check (RSA-2048 / ECDHE handshake time at 270 MHz).
4. AA protocol: version exchange, TLS over the control channel, service discovery, video/audio/input channels.
5. Audio out.

## Progress

- TLS: Mbed TLS 3.6.2 vendored (`isp/sd-10-net/mbedtls/`, config `aa_tls_config.h`); every library file cross-compiles for NDS32, about 127 KB of code before gc-sections. Not linked yet.

## eCos maps (Haiku passes over ecos.ann.dis, 2026-10-31; addresses are eCos load addresses; not yet checked by hand)

### Phone USB / AOA
- AOA switch (0x444E9A): GET_PROTOCOL (0xC0, 51, len 2, 500 ms; version <= 0 aborts), SEND_STRING (0x40, 52, wIndex 0..5, string incl. NUL), START (0x40, 53, **wValue 1**). Strings: "Sunplus, Inc." / "DVD" / "Autolink" / "2.0" / "http://support.autolink.top/Autolink.apk" / "0000000012345678" (an alternate set can come from a pointer table at 0x6E298C). No request 58 (audio) here.
- After re-enumeration: VID 0x18D1, PID 0x2D00..0x2D05 means accessory mode (0x444D7E); a device already in that range is not switched again.
- Port gate: "accessory not support on port[%d], accessory port[%d]" compares the port with a global (0x6E2BC8); which EHCI the phone port is stays open: check PORTSC of EHCI0 (0x9C102164) with a phone plugged in.
- Apple devices (VID 0x05AC) take a host-mode branch with a role switch (0x4447EE); not needed for AA.
- `usb_vbus_ctrl(port, on)` 0x45A390 through a registered function pointer; GPIO unknown.

### H.264 decoder ("MFD", multi-format decoder, with an "SPMP" sub-processor)
- Entry: `mfd_if_init` 0x430E38, `mfd_if_reset` 0x430D44, `mfd_if_deinit` 0x430F42; `h264_create` 0x436AE0; per-codec dispatch 0x432F9A; `h264_trigger_decode` 0x4147AE; `real_decode_h264_video` 0x4135EA (4.3 KB).
- Registers 0x9C00C000..0x9C00Exxx. Status 0x9C00DC84 bit 0 ("wait picture end"). Doorbell: 0x9C00D550 = 0x1F8, wait bit 0; poll 0x9C00D570 bit 0 (done, ~500 tries, "spmp processing done timeout"); then 0x9C00D570 = 1, 0x9C00D550 = 0; 0x9C00D558 carries an argument. Config block 0x9C00D640..0x9C00D72C (written at 0x4158A8).
- Software parses SPS/PPS/slice headers (0x43B3C4..0x43C5C0, 0x43DE28). `H264_SpmpProgramStoreReg` 0x43A602 builds a command ring of byte-swapped words in the decoder context (register IDs like 0x04060000, 0x04200000..0x04300000); how the ring reaches the hardware is open.
- Frame buffers: luma/chroma address/scan-type programming in 0x42D674; allocation 0x43815C / 0x4330E4. Display likely through the 0x9C150xxx block via the "AV broker" 0x3CF114. Output format unknown.
- No microcode download found yet; whether the SPMP runs loaded code is open.

## Next steps
1. TLS glue `aa_tls.c` + host handshake test against `openssl s_server` (no phone needed).
2. Phone port: with the unit up and a phone plugged in, read both EHCI PORTSCs; generalize usbh.c to that controller; implement the AOA switch and bulk endpoints.
3. AA protocol framing + version exchange + TLS over the control channel, testable with the phone once 2 works.
4. Decoder: hand-trace `real_decode_h264_video` and `H264_SpmpProgramStoreReg` for one IDR frame; find how the command ring is submitted and whether the SPMP needs code.
- TLS glue `aa_tls.c` (client, records through buffers): host test against `openssl s_server` passes (`test/test_tls.sh`, part of `make test`): ECDHE-RSA-AES256-GCM-SHA384, client certificate seen by the server. The unit's head-unit certificate and key (kept in the session scratchpad, mode 600, not in git) load, and the first handshake step gives a 124-byte TLS 1.2 ClientHello.
- AA protocol core `aa.c` / `aa.h` (framing, version, TLS on the control channel, service discovery, channel open, ping, focus, media setup/ack, sensors, touch) and `test/aa_host.c` (libusb: AOA switch with the "Android" / "Android Auto" strings, bulk loop, H.264 saved to a file). Builds; **next test: an Android phone on the PC**: `test/aa_host any CERT KEY 60 out.h264`. The protobuf field numbers come from open head-unit implementations and are unverified until a phone accepts them.
- Decoder: traced (notes/h264_decoder.md). No microcode: the "SPMP" is a DMA engine that executes register-write lists from a DRAM ring; software parses SPS/PPS/slice headers through a hardware bit reader, hardware does entropy decode and reconstruction; frames are addressed through the BG2D index table (0x9C00A400); display through VPP0 (0x9C005780) with a scaler to 1024x600. Bring-up inputs ready: `test/h264/{grey,bars}.h264` (800x480 baseline IDR, ffmpeg reference NV12 regenerated locally) and `tools/vdec_probe.py` (step 1, read-only, refuses to touch the MFD block unless its clocks are on and resets released).
- H.264 header parser `h264p.c` (SPS/PPS/slice header + slice-data bit offset) matches ffmpeg trace_headers on every field over 4 test streams (baseline multi-slice P, CABAC main with weighted prediction); in `make test`.

## Waiting on hardware (2026-10-31 evening)
- Unit offline since the SD test build (needs a power cycle).
- Decoder bring-up: `source tools/hu.env && tools/vdec_probe.py` (step 1, read-only), then steps 2..6 of notes/h264_decoder.md section 6.
- AA protocol: an Android phone on the PC: `cd isp/sd-10-net/test && gcc ... aa_host` (build line in the aa_host.c header comment) then `./aa_host any <cert> <key> 60 out.h264`.
- Phone port: phone plugged into the unit, read PORTSC of EHCI0 (0x9C102164) and EHCI1.

## Bluetooth RFCOMM through the AC6965A (Haiku map of eCos, 2026-10-31, unverified)
- RFCOMM runs over the same MCU link as `AC`-prefixed frames (built by 0x31DDF8, sent through 0x31D6D4 / McuCmd). **`ac` is on the banned-frame list: nothing sends an AC frame without the owner's explicit go-ahead.**
- Out: `RFCOMM_IF_sendData` 0x388918 splits payloads into 50-byte chunks, `AC 2a <len> <handle> <data>` for all but the last, `AC 2b <len> <handle> <data>` for the last (50 ms between chunks).
- In: chunks `[len][data]` collected by 0x385EA2 (`RFCOMM_DATA_PARSER_IND`) / 0x385EF2 (`END`) into a 256-byte ring; channel state 2 (AA, set beside "wifi AndroidAuto on") hands them to the AA middleware. The inbound frame type that carries them is not identified yet.
- Connect event (0x3866EC `RFCOMM_CONNECT`): rfc_info +0x0A handle (u32 LE), +0x12 the 16-byte service UUID; the AA UUID constant sits at 0x6C021C. No SoC-to-chip frame carrying the UUID was found: the Jieli firmware probably advertises the AA service itself. After connect eCos sends `AC 29 04 <handle4>`.
- Stock AA TCP endpoint: IP 192.168.0.1, port 8765 (0x3927C8).
- Other BT commands go through a selector (0x3887D0) using a flash table near 0x984ACE80; its layout is not decoded yet (the bytes there are 8-byte index/value pairs).
- Next: watch the MCU link (receive only) while a phone pairs and starts wireless AA under stock firmware, if the stock firmware can be booted with our UART tap; otherwise decode the inbound dispatch.

## Wireless pieces built (2026-10-31, all host-tested, in `make test`)
- `aa_wl.c`: RFCOMM handshake (WifiStartRequest / WifiInfoResponse, phone replies), byte-exact against hand-encoded protobuf.
- `aa_tcp.c`: aa.c over lwIP TCP; scripted phone gets the version request and a TLS ClientHello.
- lwIP 2.2.0 (`lwip/`, `lwipopts.h`): 8 MB transfer with 1% loss intact.
- `wpa_ap.c`: WPA2-PSK authenticator; full 4-way handshake against our supplicant, wrong passphrase rejected.
- `dhcps.c`: DHCP server; lwIP's client gets 192.168.43.2/24.
- `ap80211.c`: beacon / probe response / auth / assoc response (b/g/n HT20, WPA2-CCMP RSN, WMM) dissected by tshark without errors, plus parsing of phone frames.
- RTL8188F AP mode recipe from Linux rtl8xxxu (v6.12): MSR link type 3 (AP) at 0x102; BEACON_CTRL 0x550 = DIS_TSF_UPDATE | BCN_FUNC_EN | MBSSID | TX_BCN_RPT after a TSF0 reset (0x553); ATIMWND 0x55A = 0x0C; BCN_INTERVAL 0x554 = 100; TBTT_PROHIBIT 0x540 = 0x6404 then +1 = 0x80 and +2 &= 0xF0 when beaconing; FWHW_TXQ_CTRL 0x420 +2 |= EN_BCNQ_DL>>16; beacon sent as a normal TX frame on queue 0x10 after clearing BCN_VALID (TDECTRL 0x208 bit 16, write 1) and SW_BCN_SEL (0x228 bit 20), then wait BCN_VALID; RCR without CHECK_BSSID_MATCH / CHECK_BSSID_BEACON; H2C media status connect for the broadcast MACID; keys through the CAM as in station mode.
- Next on hardware: AP mode in net.c (radio + these modules), lwIP netif over the 802.11 data path, then BT RFCOMM (needs the owner's go-ahead for `AC` frames).

## AP mode on the unit (2026-11-01, build 20261101-023732, not yet tested with a station)
- `hu.py ap CH SSID PSK` (HU1 op 18) switches the unit from station to access point: MSR AP, BSSID = our MAC, BCN0 on, beacon uploaded on the BEACON queue (endpoint 0, HW sequence), RCR without BSSID filtering, broadcast MACID connected; open auth, association, our WPA2 authenticator (EAPOL retries), software CCMP (pairwise for the station, GTK id 1 for broadcast), static 192.168.43.1 with a DHCP server in ip.c (offer/ack/nak tested on the host). `hu.py ap` alone: status.
- Safety: if no station keys up and sends a HU1 command within 5 min, the unit saves its log as A<id>.TXT and resets back to station mode (card build). Every planned reset (probation, failure, AP deadline) now saves the run's log first (F/A<id>.TXT), and crash records (v3) carry the log length so the next boot also keeps the crashed run's log (P<id>.TXT).
- This PC has no Wi-Fi interface: the first AP test needs a phone (or laptop) joining the AP.
- **First AP test passed** (2026-11-01, build 20261101-023732, owner's phone): beacon accepted (BCN_VALID), phone found MiataTest on channel 6, open auth, association (HT), our WPA2 4-way handshake completed first time, DHCP offer + ack 192.168.43.2; the phone left ~20 s later with reason 8 (leaving BSS: no internet, or the owner); the 5-minute deadline then saved A9A55773.TXT and reset to station mode. Next: lwIP on this link (TCP for AA, HU1 over it), then the Bluetooth RFCOMM path.
