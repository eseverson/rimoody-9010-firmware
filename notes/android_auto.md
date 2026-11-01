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

## Plan for C

1. H.264 decoder: map `h264_vid.c` and the decoder register block from the eCos disassembly (bitstream feed, frame buffers, output format, how frames reach the display layer). Biggest unknown; decides feasibility.
2. Phone USB port: which controller (EHCI0 at 0x9C102100 is the candidate), generalize usbh.c, AOA switch (control requests 51/52/53).
3. TLS: mbedTLS client with client-certificate auth; NDS32 bignum speed check (RSA-2048 / ECDHE handshake time at 270 MHz).
4. AA protocol: version exchange, TLS over the control channel, service discovery, video/audio/input channels.
5. Audio out.

## Progress

- TLS: Mbed TLS 3.6.2 vendored (`isp/sd-10-net/mbedtls/`, config `aa_tls_config.h`); every library file cross-compiles for NDS32, about 127 KB of code before gc-sections. Not linked yet.
