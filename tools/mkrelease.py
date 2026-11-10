#!/usr/bin/env python3
"""Build the end-user release: a zip that a stock Rimoody 9010 runs from an SD card through its own update prompt.

  mkrelease.py firmware/NET.BIN dump1.bin [--out dist]

Inputs: a built NET.BIN and a 16 MiB dump of a stock unit's NOR flash. Only two things are taken from the dump, both
copied verbatim: xboot (flash 0x10000) and the U-Boot uImage (flash 0x8A40E0). The U-Boot must pass xboot's check with
its verify flag set (isp/mkisp.py check_stock_uboot) and carry the stock default environment (`bootcmd=bootsp`).

Output: dist/rimoody-9010-firmware-<BUILD_ID>/ and dist/rimoody-9010-firmware-<BUILD_ID>.zip with
  BOOT.BIN                    the firmware (NET.BIN unchanged)
  ISP_UPDT.BIN, ISPBOOOT.BIN  the installer, two identical copies (see below)
  INSTALL.txt                 the steps, for the person at the head unit
  WIFI.TXT.example            the card config
  SHA256SUMS                  sha256sum -c format

How the stock update path runs the installer (reverse engineered, see git history of isp/sd-1-probe/mkisp_sd.py):
  eCos      sees ISP_UPDT.BIN in the card's root and offers an update. Accepting it sends the MCU `e5 01` (park before
            an update), writes "QART" to 0x9C00317C and resets. eCos refuses files over 12 MiB.
  xboot     QART selects SD ISP: it loads U-Boot from ISP_UPDT.BIN (header at file offset 0x10000, body at 0x10040)
            to RAM 0xF00040 and checks xboot's word sums. Its FAT reader is minimal: the file must be contiguous.
  U-Boot    the stock U-Boot from the file (never written anywhere) runs `ispsp all mmc 0`: it reads 0x800 bytes of
            /ISPBOOOT.BIN at file offset 0x100000 to 0x1000040 and runs `source 0x1000060`, i.e. our script.
            The 32-byte "Pentagram_ISP_image" tag before the script is not checked. Script image <= 0x7E0 bytes.
            After `source` returns, ispsp stops only if isp_all_or_update_done is set; otherwise it starts over.

The MCU stays parked for the whole session that follows: it ignores the heartbeat (`d1 00`), `e5 00` and `ad`, and
power-cycles the unit about 145 s after the update started (measured on the ISP rounds; `ispsp update_finish` did not
change it). Our firmware therefore runs only about two minutes after the installer. The script starts it as
`go 0x1800000 main isp`: the `isp` argument tells it that this is the installer's boot, a demo. It writes nothing to
the flash and says so on screen; keeping it is the user's choice (Settings > Flash > Set up, stage 1, after which every
power-on boots the card's BOOT.BIN with the MCU in its normal mode). Otherwise the power cycle brings back the stock
firmware: the script has already deleted the installer files, so eCos does not offer the update again. In this session
Set up is refused from 90 s after boot on, so it cannot run into the power cut.

The installer itself never writes the unit's NOR flash: the script has no flash command (checked below against a list
of U-Boot commands that can write it), and the U-Boot it runs lives in RAM only. The flash is changed only by our
firmware, through the checked install path of Settings > Flash.

Stdlib only. Never executes anything from the dump.
"""
import argparse, hashlib, os, re, shutil, struct, sys, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'isp'))
from mkisp import (FLASH_XBOOT, FLASH_UBOOT_HDR, OFF_UBOOT, OFF_HDR, OFF_STAGE1, STAGE1_MAX, ECOS_MAX_SIZE, be32,  # noqa: E402
                   check_stock_uboot, check_script, mkscript)

ISP_NAMES = ('ISP_UPDT.BIN', 'ISPBOOOT.BIN')   # eCos trigger + xboot's U-Boot source / ispsp's script source
STOCK_ENV = b'bootcmd=bootsp\0bootdelay=0\0baudrate=921600\0\0'   # the 44-byte default environment (install.c)
IMAGE_MAX = 0x18FF00      # largest image the flash copy takes (notes/nor_install.md); MM_PAYLOAD_SZ also holds .bss
ECOS_NAMES = ('MCU_SY260L.BIN', 'LOGO.BIN', 'DKEY.BIN')   # other files stock eCos acts on: never ship these

# The installer script. Same pinmux, UART1, MCU frame, SD and card lines as the NOR boot script (isp/bootcmd/boot.txt),
# after the update is marked done and the installer files are gone. Every line, in order:
#   setenv isp_all_or_update_done 1   ispsp stops after `source` instead of starting over (stock end of update)
#   mw.l 0x9c00317c 0 1               clear the QART boot mode: any reset from here on boots normally, not ISP again
#   fatrm ... ISP_UPDT.BIN            eCos must not find it again (it would go back into the updater on every boot);
#                                     first, so a failure further down cannot loop the update
#   fatrm ... ISPBOOOT.BIN            the other copy; harmless if left, removed so the card holds only the firmware
#   wdt stop all                      no SoC watchdog reset while the card is read
#   pinmux, uartsp, d1 00             UART1 up and one heartbeat, as boot.txt (the parked MCU ignores it)
#   ispsp update_finish               the frame stock sends after an update (`ad`); was harmless on the ISP rounds
#   SD pinmux, isp_if/isp_dev, mmc    boot.txt's card setup and its mmc 0, then mmc 1 search for /BOOT.BIN
#   hu_src isp                        shows `hu.py cmd 'printenv hu_src'` that it came in this way (the firmware does
#                                     not read U-Boot's environment at boot)
#   crc32 ...                         evicts the D-cache so the freshly loaded code is what the CPU fetches
#   go hello, go main isp             the firmware's two entry calls, as boot.txt; `isp` marks the demo session (the
#                                     firmware explains how to keep it; an EXEC or a later boot never passes it)
#   reset                             no BOOT.BIN or the firmware returned: boot normally (stock eCos)
SCRIPT = """\
setenv isp_all_or_update_done 1
mw.l 0x9c00317c 0 1
fatrm $isp_if $isp_dev ISP_UPDT.BIN
fatrm $isp_if $isp_dev ISPBOOOT.BIN
wdt stop all
mw.l 0x9c0000a4 0x1fe00220
uartsp init 1 115200
mw.l 0x9c000980 0xff;mw.l 0x9c000980 0xaa;mw.l 0x9c000980 0x02;mw.l 0x9c000980 0xd1;mw.l 0x9c000980 0x00;mw.l 0x9c000980 0x2d;mw.l 0x9c000980 0xbe;mw.l 0x9c000980 0xba
ispsp update_finish
mw.l 0x9c00008c 0x00f00050;mw.l 0x9c000100 0x00fc00fc;mw.l 0x9c00010c 0x00fc00fc
setenv hu_ok
setenv isp_if mmc
setenv isp_dev 0
mmc dev 0
mmc rescan
if fatload mmc 0 0x1800000 /BOOT.BIN; then setenv hu_ok 1; else mmc dev 1; mmc rescan; if fatload mmc 1 0x1800000 /BOOT.BIN; then setenv isp_dev 1; setenv hu_ok 1; fi; fi
if test -n "$hu_ok"; then setenv hu_src isp; crc32 0x00f00000 0x100000; wdt stop all; go 0x1800000 hello; go 0x1800000 main isp; fi
reset
"""

# U-Boot commands that can write the NOR flash or the saved environment, directly or through ispsp's update steps
FLASH_WORDS = re.compile(r'\b(sfsp|sf|saveenv|erase|protect|cp|cp\.[bwl]|mw\.[bwl]\s+0x9[89a-f]|fatwrite|nand|'
                         r'ispsp\s+(?!update_finish\b)\S+|env\s+save|bootsp|bootm|run)\b')
ALLOWED_MW = re.compile(r'^mw\.l 0x9c0[0-9a-f]{5} (0x[0-9a-f]+|[0-9]+)( 1)?$')   # only SoC registers at 0x9C000000..0x9C0FFFFF

INSTALL = """\
{title}
{rule}

Unofficial firmware for the Rimoody 9" single-DIN head unit, model 9010. Use it at your own risk.
{url}

You need: a microSD card of 32 GB or less, a PC, and the unit running its original firmware.

1. Format the card as FAT32 (not exFAT). Start from an empty card: the unit's update loader reads only simple, unfragmented files.

2. Copy ISP_UPDT.BIN and ISPBOOOT.BIN to the card's top folder first, then BOOT.BIN.

3. {wifi}

4. Put the card in the unit and switch it on. The original firmware offers a software update. Accept it.

5. The unit restarts and our firmware starts from the card within a few seconds, as a demo: nothing on the unit has been changed. The installer deletes ISP_UPDT.BIN and ISPBOOOT.BIN from the card.

6. Try it. About two minutes after the update the unit switches itself off and on (its power controller does this after every update) and the original firmware is back. To try again, copy ISP_UPDT.BIN and ISPBOOOT.BIN to the card again.

7. To keep it: during the demo, within the first 90 seconds, go to Settings > Flash > Set up ("Boot from the SD card, else from flash") and confirm. It writes the boot path into the unit's flash, a few seconds; keep the power on and do not start the engine meanwhile, since a power loss while it writes can leave the unit unable to start. From then on every power-on starts this firmware from the card, with no time limit; without the card the original firmware starts. Remove on the same row undoes it.

8. Recommended, once it starts normally: Settings > Flash > Back up. It writes NORDUMP.BIN (16 MB, about two minutes) to the card. Keep a copy on your PC: it is the only way to repair the unit with a flash programmer.

9. Optional: Settings > Flash > Install copies the firmware into the unit's flash, so it starts even without the card. Do this only after it has run well from the card for a while.

Android Auto (wireless) needs the unit's own certificate files, AACERT.PEM and AAKEY.PEM. The firmware takes them from the unit's flash on its first start and keeps them on the card; there is nothing to copy. Keep the card in the unit.

Back to the original firmware:
- Once: Settings > System > Boot stock (until the next ignition cycle).
- Until you put it back: take BOOT.BIN off the card. If you used Install (step 9), first remove the flash copy: Settings > Flash > Remove on the "Install this firmware to flash" row.
- For good: Settings > Flash > Remove on the "Boot from the SD card" row. From the next power-on only the original firmware starts; to come back, run this installer again.

Check the download: sha256sum -c SHA256SUMS
"""

WIFI_REQUIRED = ("Required by this build: rename WIFI.TXT.example to WIFI.TXT (exactly that, not WIFI.TXT.txt), put in "
                 "your Wi-Fi network's name and password, choose your own token, and copy it to the card. Without it "
                 "the firmware does not start. A network that is out of range is fine: after 30 s "
                 "the unit starts its own \"car network\" for Android Auto.")
WIFI_OPTIONAL = ("Optional: WIFI.TXT. Without it the unit runs its own Wi-Fi network, the \"car network\" (its name and "
                 "password are made up at the first start and kept in AP.TXT on the card). Wireless Android Auto uses "
                 "it: the phone gets the password over Bluetooth, there is nothing to type. With WIFI.TXT (rename "
                 "WIFI.TXT.example to WIFI.TXT, not WIFI.TXT.txt, and fill it in) the unit also joins your own Wi-Fi "
                 "when it is in range, and starts the car network 30 s after it is not. Its token= turns on the PC "
                 "tools' commands; without a token the unit accepts none.")

WIFI_EXAMPLE = """\
# Rename this file to WIFI.TXT and copy it to the SD card. Lines starting with # are ignored.
# The file is optional: without it the unit runs only its own car network, for wireless Android Auto.
# ssid=  your Wi-Fi network's name (or a phone hotspot). If it is not in range, the unit starts its own car network.
# psk=   its password, 8 to 63 characters.
# token= a secret of your own, up to 32 characters. Anyone on the same network who knows it can control the unit
#        with the PC tools, so change it. Leave the line out and the unit accepts no PC tool commands.
ssid=YourNetwork
psk=your-wifi-password
token=CHANGE-THIS-SECRET
"""



def sha256(b):
    return hashlib.sha256(b).hexdigest()


def check_net_bin(img):
    """The firmware image: HUNE v1 header at 0x40 with a printable build id (head.S), within the flash copy's limit."""
    if len(img) < 0x100 or img[0x40:0x44] != b'HUNE' or struct.unpack_from('<I', img, 0x44)[0] != 1:
        sys.exit('NET.BIN has no HUNE v1 header at 0x40: not a firmware image')
    bid = img[0x48:0x80].split(b'\0')[0]
    if not re.fullmatch(rb'[0-9A-Za-z._-]{1,55}', bid):
        sys.exit('NET.BIN build id unreadable: %r' % bid)
    if len(img) > IMAGE_MAX:
        sys.exit('NET.BIN is 0x%x bytes; Settings > Flash > Install takes at most 0x%x' % (len(img), IMAGE_MAX))
    return bid.decode()


def check_dump(d):
    """xboot and the stock U-Boot from the dump, with every check xboot and the release depend on."""
    if len(d) != 0x1000000:
        sys.exit('dump must be 16 MiB')
    if d[FLASH_XBOOT:FLASH_XBOOT + 4] != b'XBUT':
        sys.exit('no XBUT at 0x10000')
    if d[0xC400:0xC40D] != b'ISP_UPDT.BIN\0':
        sys.exit('flash 0xC400 holds %r, not "ISP_UPDT.BIN": xboot would look for another file name' % d[0xC400:0xC410])
    xsz = struct.unpack_from('<I', d, FLASH_XBOOT + 8)[0] + 0x20
    if xsz > OFF_UBOOT:
        sys.exit('xboot is 0x%x bytes, more than the 0x%x before U-Boot in the layout' % (xsz, OFF_UBOOT))
    usize = be32(d, FLASH_UBOOT_HDR + 12)
    uimg = d[FLASH_UBOOT_HDR:FLASH_UBOOT_HDR + 64 + usize]
    err = check_stock_uboot(uimg)
    if err:
        sys.exit('U-Boot fails the xboot check: ' + err)
    if uimg[0x1E] != 0x5A:
        sys.exit('U-Boot header has no xboot verify flag: its sums would not be checked, refusing')
    if usize < 0x1000 or OFF_UBOOT + len(uimg) > OFF_HDR:
        sys.exit('U-Boot size 0x%x does not fit the layout' % usize)
    n = uimg.count(STOCK_ENV)
    if n != 1:
        # a patched U-Boot (bootcmd=source 98e00000;bootsp) would run the unit's NOR boot script instead of ours and
        # never delete ISP_UPDT.BIN
        sys.exit('U-Boot default environment is not the stock one (found %d times): use a dump of a stock unit' % n)
    return d[FLASH_XBOOT:FLASH_XBOOT + xsz], uimg


def check_script_text(text):
    """No line of the installer may write the flash or the saved environment."""
    for line in text.splitlines():
        for cmd in (c.strip() for c in re.split(r';|\bthen\b|\belse\b|\bfi\b|\bif\b', line)):
            if not cmd:
                continue
            if cmd.startswith('mw.') and not ALLOWED_MW.match(cmd):
                sys.exit('installer script: %r writes outside the SoC registers' % cmd)
            if FLASH_WORDS.search(cmd) and not cmd.startswith('mw.'):
                sys.exit('installer script: %r can write the flash' % cmd)


def build_isp(xboot, uimg, script_img):
    out = bytearray(OFF_HDR + 0x1000)
    out[0:len(xboot)] = xboot        # not read on the SD path; kept for parity with Sunplus ISPBOOOT.BIN
    out[OFF_UBOOT:OFF_UBOOT + len(uimg)] = uimg
    out[OFF_HDR:OFF_HDR + 32] = b'Pentagram_ISP_image'.ljust(32, b'\0')
    out[OFF_STAGE1:OFF_STAGE1 + len(script_img)] = script_img
    return bytes(out)


def verify_isp(f, uimg, script_img):
    """Emulate the readers on the bytes as written."""
    if len(f) > ECOS_MAX_SIZE:
        sys.exit('ISP file exceeds the 12 MiB limit eCos enforces')
    if check_stock_uboot(f[OFF_UBOOT:]) is not None or f[OFF_UBOOT:OFF_UBOOT + len(uimg)] != uimg:
        sys.exit('internal: xboot would not load the U-Boot from the ISP file')           # xboot, file offset 0x10000
    ram = f[OFF_HDR:OFF_HDR + 0x800]                                                     # ispsp: 0x800 @ 0x100000
    err = check_script(ram[0x20:])                                                       # source $isp_ram_addr+0x20
    if err or ram[0x20:0x20 + len(script_img)] != script_img:
        sys.exit('internal: U-Boot would reject the installer script: %s' % err)


def zip_date(build_id):
    m = re.match(r'(\d{4})(\d\d)(\d\d)-(\d\d)(\d\d)(\d\d)$', build_id)
    return tuple(int(x) for x in m.groups()) if m else time.localtime()[:6]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('net_bin', help='the built firmware (firmware/NET.BIN)')
    ap.add_argument('dump', help='16 MiB NOR dump of a stock unit (only xboot and U-Boot are used)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'), help='output directory (default: dist/)')
    ap.add_argument('--url', help='project page for INSTALL.txt (default: point to the README)')
    a = ap.parse_args()

    img = open(a.net_bin, 'rb').read()
    build = check_net_bin(img)
    xboot, uimg = check_dump(open(a.dump, 'rb').read())

    check_script_text(SCRIPT)
    script_img = mkscript(SCRIPT, 'release installer', stamp=0)   # stamp 0: the same inputs give the same file
    if len(script_img) > STAGE1_MAX:
        sys.exit('installer script image is %d bytes, max %d' % (len(script_img), STAGE1_MAX))
    if check_script(script_img) is not None:
        sys.exit('internal: installer script image invalid')
    isp = build_isp(xboot, uimg, script_img)
    verify_isp(isp, uimg, script_img)

    # older builds end a boot without WIFI.TXT as "no config (WIFI.TXT)"; current ones run the car network instead
    wifi_required = b'no config (WIFI.TXT)' in img
    url = ('Project page, with what works and what does not: ' + a.url if a.url else
           'What works and what does not: the project README, "Stock features not built yet".')
    title = 'Rimoody 9010 head unit: custom firmware ' + build
    install = INSTALL.format(title=title, rule='=' * len(title), url=url, wifi=WIFI_REQUIRED if wifi_required else WIFI_OPTIONAL)

    files = {'BOOT.BIN': img}
    files.update({n: isp for n in ISP_NAMES})
    files['INSTALL.txt'] = install.replace('\n', '\r\n').encode('ascii')   # CRLF: opened on Windows by most users
    files['WIFI.TXT.example'] = WIFI_EXAMPLE.encode('ascii')
    assert not set(files) & set(ECOS_NAMES)
    files['SHA256SUMS'] = ''.join('%s  %s\n' % (sha256(files[n]), n) for n in sorted(files)).encode()

    name = 'rimoody-9010-firmware-%s' % build
    d = os.path.join(a.out, name)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    for n, b in files.items():
        with open(os.path.join(d, n), 'wb') as f:
            f.write(b)
    zpath = os.path.join(a.out, name + '.zip')
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for n in ('ISP_UPDT.BIN', 'ISPBOOOT.BIN', 'BOOT.BIN', 'WIFI.TXT.example', 'INSTALL.txt', 'SHA256SUMS'):
            zi = zipfile.ZipInfo(n, zip_date(build))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, files[n])

    # read everything back from disk and check it again
    with zipfile.ZipFile(zpath) as z:
        for n in files:
            if z.read(n) != files[n] or open(os.path.join(d, n), 'rb').read() != files[n]:
                sys.exit('read-back mismatch: ' + n)
    back = open(os.path.join(d, 'ISP_UPDT.BIN'), 'rb').read()
    verify_isp(back, uimg, script_img)
    if back != open(os.path.join(d, 'ISPBOOOT.BIN'), 'rb').read():
        sys.exit('read-back: the two ISP files differ')

    print('build %s' % build)
    print('BOOT.BIN      %8d bytes (flash copy limit %d)' % (len(img), IMAGE_MAX))
    print('ISP files     %8d bytes each (eCos limit %d); U-Boot body 0x%x, xboot check ok, verify flag set, stock env'
          % (len(isp), ECOS_MAX_SIZE, len(uimg) - 64))
    print('script image  %8d bytes (limit %d), U-Boot source checks ok, no flash-writing command' % (len(script_img),
                                                                                                   STAGE1_MAX))
    print('WIFI.TXT      %s by this build' % ('REQUIRED' if wifi_required else 'optional'))
    print('wrote %s/ and %s (%d bytes)' % (d, zpath, os.path.getsize(zpath)))


if __name__ == '__main__':
    main()
