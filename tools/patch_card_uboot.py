#!/usr/bin/env python3
"""Over Wi-Fi, swap the U-Boot inside the card's ISP_UPDT.BIN for the patched one (isp/bootcmd dry run).

Loads /ISP_UPDT.BIN into RAM on the unit, applies the word differences between the card's file and the dry-run file
inside the U-Boot region (0x10000-0xFFFFF), checks the CRC32 in RAM against the expected value, writes the file back,
reloads it and checks again. The stage-1 script region is left as it is.

  patch_card_uboot.py <current ISP_UPDT.BIN> <dry-run ISP_UPDT.BIN>
"""
import os, re, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hu

RAM = 0x03000000


def crc_on_unit(u, n):
    out = u.cmd('crc32 0x%x 0x%x' % (RAM, n))
    m = re.search(r'==>\s*([0-9a-f]{8})', out)
    if not m:
        sys.exit('no crc32 result: ' + out)
    return int(m.group(1), 16)


def main():
    old, new = (open(p, 'rb').read() for p in sys.argv[1:3])
    assert len(old) == len(new)
    patched = bytearray(old)
    words = sorted({i & ~3 for i in range(0x10000, 0x100000) if old[i] != new[i]})
    for w in words:
        patched[w:w + 4] = new[w:w + 4]
    want = zlib.crc32(patched) & 0xffffffff
    print('%d words to patch, expected crc32 %08x' % (len(words), want), flush=True)

    ip, hello = hu.listen(900)
    print('hello: %s' % hello, flush=True)
    u = hu.Unit(ip, os.environ['HU_TOKEN'], timeout=5, retries=3)
    print(u.cmd('fatload mmc 0 0x%x /ISP_UPDT.BIN' % RAM).strip(), flush=True)
    got = crc_on_unit(u, len(old))
    if got != zlib.crc32(old) & 0xffffffff:
        sys.exit('card file is not the expected original (crc %08x), stopping' % got)
    for w in words:
        u.cmd('mw.l 0x%x 0x%08x' % (RAM + w, struct.unpack_from('<I', new, w)[0]))
    got = crc_on_unit(u, len(old))
    if got != want:
        sys.exit('patched RAM crc %08x != %08x, not writing' % (got, want))
    print(u.cmd('fatwrite mmc 0 0x%x ISP_UPDT.BIN 0x%x' % (RAM, len(old))).strip(), flush=True)
    u.cmd('mw.l 0x%x 0 0x%x' % (RAM, len(old) // 4))
    print(u.cmd('fatload mmc 0 0x%x /ISP_UPDT.BIN' % RAM).strip(), flush=True)
    got = crc_on_unit(u, len(old))
    print('card file now crc %08x: %s' % (got, 'PATCHED OK' if got == want else 'MISMATCH'), flush=True)


if __name__ == '__main__':
    main()
