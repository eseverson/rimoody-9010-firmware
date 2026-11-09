#!/usr/bin/env python3
"""Build an ISP_UPDT.BIN for the SPHE8268K ("gemini") head unit from the stock flash dump plus a U-Boot script.

Layout (offsets in the file):
  0x000000  stock xboot (copied from flash 0x10000, XBUT header) -- never read in the PART/USB path, kept for parity
            with Sunplus ISPBOOOT.BIN; --no-xboot leaves this zero
  0x010000  stock U-Boot legacy uImage (64-byte header + body), copied verbatim from flash 0x8A40E0.
            xboot loads the header from file offset 0x10000 and the body from 0x10040 to RAM 0xF00040.
  0x100000  32-byte "Pentagram_ISP_image" signature (not checked by this firmware, cosmetic)
  0x100020  legacy uImage, type=script, comp=none (standard crc32 header + data). U-Boot `ispsp update usb N` reads
            0x800 bytes from file offset 0x100000 into RAM 0x1000040 and runs `source 0x1000060`. Header + script
            must fit in 0x7E0 bytes.
  0x110000  optional stage-2 script uImage (any size) that the stage-1 script can fatload + source.

Stdlib only. Never executes anything from the dump.
"""
import argparse, struct, sys, time, zlib

FLASH_XBOOT = 0x10000
FLASH_UBOOT_HDR = 0x8A40E0
OFF_UBOOT = 0x10000
OFF_HDR = 0x100000
OFF_STAGE1 = 0x100020
OFF_STAGE2 = 0x110000
STAGE1_MAX = 0x800 - 0x20
ECOS_MAX_SIZE = 0xC00000      # eCos rejects files larger than 12 MiB (f_00475c66)
UBOOT_MAX_BODY = 0x1FFFC0     # xboot "image is too big"
IH_MAGIC = 0x27051956
IH_TYPE_SCRIPT = 6
IH_ARCH_NDS32 = 20            # not checked by `source`, but correct for this SoC

# Stage-1: proof of execution with no writes to the head unit's own flash.
DEFAULT_STAGE1 = """\
mw.l 0x9c00317c 0 1
setenv isp_all_or_update_done 1
fatwrite $isp_if $isp_dev 0x98000000 NORDUMP.BIN 0x1000000
env export -t 0x1200000
fatwrite $isp_if $isp_dev 0x1200000 UENV.TXT $filesize
fatrm $isp_if $isp_dev ISP_UPDT.BIN
reset
"""


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def xboot_hsum(hdr):
    h = bytearray(hdr[:64])
    h[4:8] = b'\0\0\0\0'
    return sum(struct.unpack('<16I', h)) & 0xFFFFFFFF


def xboot_dsum(body):
    n = len(body) & ~3
    s = sum(struct.unpack('<%dI' % (n // 4), body[:n])) if n else 0
    tail = body[n:]
    t = int.from_bytes(tail, 'little') if tail else 0
    return (s + t) & 0xFFFFFFFF


def check_stock_uboot(img):
    """Re-implement xboot's checks (xboot 0x9e830d6c..0x9e830e0c, 0x9e831f08, 0x9e831f5c)."""
    if be32(img, 0) != IH_MAGIC:
        return 'bad magic'
    size = be32(img, 12)
    if size > UBOOT_MAX_BODY:
        return 'image is too big'
    if img[0x1E] != 0x5A:
        return None  # xboot prints "skip verify: not support" and accepts
    if xboot_hsum(img) != be32(img, 4):
        return 'bad hcrc (word sum)'
    if xboot_dsum(img[64:64 + size]) != be32(img, 24):
        return 'corrupted image (word sum)'
    return None


def mkscript(text, name, stamp=None):
    """Legacy script uImage; stamp = header time (default now; install.c's built-in script uses 0)."""
    data = text.encode('ascii')
    if not data.endswith(b'\n'):
        data += b'\n'
    payload = struct.pack('>II', len(data), 0) + data
    hdr = bytearray(struct.pack('>7I4B32s', IH_MAGIC, 0, int(time.time()) if stamp is None else stamp, len(payload), 0, 0,
                                zlib.crc32(payload) & 0xFFFFFFFF, 5, IH_ARCH_NDS32, IH_TYPE_SCRIPT, 0,
                                name.encode()[:31]))
    struct.pack_into('>I', hdr, 4, zlib.crc32(bytes(hdr)) & 0xFFFFFFFF)
    return bytes(hdr) + payload


def check_script(img):
    """Re-implement U-Boot 2014.04 image_source() checks (sub_00f1153a)."""
    if be32(img, 0) != IH_MAGIC:
        return 'Wrong image format'
    h = bytearray(img[:64]); h[4:8] = b'\0\0\0\0'
    if zlib.crc32(bytes(h)) & 0xFFFFFFFF != be32(img, 4):
        return 'Bad header crc'
    size = be32(img, 12)
    if zlib.crc32(img[64:64 + size]) & 0xFFFFFFFF != be32(img, 24):
        return 'Bad data crc'
    if img[0x1E] != IH_TYPE_SCRIPT:
        return 'Bad image type'
    if be32(img, 64) == 0:
        return 'Empty Script'
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', help='16 MiB SPI NOR dump')
    ap.add_argument('out', help='output file, e.g. ISP_UPDT.BIN')
    ap.add_argument('--script', help='stage-1 U-Boot script text file (default: dump-flash-to-stick payload)')
    ap.add_argument('--stage2', help='optional stage-2 script text, stored at 0x110000; load it from stage-1 with: '
                    'fatload $isp_if $isp_dev 0x1100000 $isp_update_file_name <len> 0x110000 && source 0x1100000')
    ap.add_argument('--no-xboot', action='store_true', help='leave 0x0-0xFFFF zero')
    a = ap.parse_args()

    d = open(a.dump, 'rb').read()
    if len(d) != 0x1000000:
        sys.exit('dump must be 16 MiB')
    if d[FLASH_XBOOT:FLASH_XBOOT + 4] != b'XBUT':
        sys.exit('no XBUT at 0x10000')
    if d[0xC400:0xC40D] != b'ISP_UPDT.BIN\0':
        print('warning: flash 0xC400 does not hold "ISP_UPDT.BIN"; xboot/U-Boot will look for %r'
              % d[0xC400:0xC410])

    usize = be32(d, FLASH_UBOOT_HDR + 12)
    uimg = d[FLASH_UBOOT_HDR:FLASH_UBOOT_HDR + 64 + usize]
    err = check_stock_uboot(uimg)
    if err:
        sys.exit('stock U-Boot fails xboot check: ' + err)
    if OFF_UBOOT + len(uimg) > OFF_HDR:
        sys.exit('U-Boot too large for layout')

    s1 = mkscript(open(a.script).read() if a.script else DEFAULT_STAGE1, 'isp stage1')
    if len(s1) > STAGE1_MAX:
        sys.exit('stage-1 script image is %d bytes, max %d (use --stage2)' % (len(s1), STAGE1_MAX))
    assert check_script(s1) is None

    out = bytearray(OFF_HDR + 0x1000)
    if not a.no_xboot:
        xsz = struct.unpack_from('<I', d, FLASH_XBOOT + 8)[0] + 0x20
        out[0:xsz] = d[FLASH_XBOOT:FLASH_XBOOT + xsz]
    out[OFF_UBOOT:OFF_UBOOT + len(uimg)] = uimg
    out[OFF_HDR:OFF_HDR + 32] = b'Pentagram_ISP_image'.ljust(32, b'\0')
    out[OFF_STAGE1:OFF_STAGE1 + len(s1)] = s1
    if a.stage2:
        s2 = mkscript(open(a.stage2).read(), 'isp stage2')
        out = out.ljust(OFF_STAGE2, b'\0')[:OFF_STAGE2] + s2 + b'\0' * ((-len(s2)) % 512)
        print('stage-2 at 0x%x, %d bytes (use 0x%x as fatload length)' % (OFF_STAGE2, len(s2), len(s2)))
    if len(out) > ECOS_MAX_SIZE:
        sys.exit('file exceeds the 12 MiB limit eCos enforces')
    open(a.out, 'wb').write(out)

    # self-check: emulate the readers
    f = bytes(out)
    assert check_stock_uboot(f[OFF_UBOOT:]) is None
    ram = f[OFF_HDR:OFF_HDR + 0x800]          # ispsp: fatload ... 0x800 0x100000 -> 0x1000040
    assert check_script(ram[0x20:]) is None   # source $isp_ram_addr+0x20
    print('wrote %s: %d bytes; U-Boot body 0x%x; stage-1 image %d/%d bytes' % (a.out, len(f), usize, len(s1),
                                                                                STAGE1_MAX))


if __name__ == '__main__':
    main()
