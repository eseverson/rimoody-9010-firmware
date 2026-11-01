#!/usr/bin/env python3
"""Make a flash image whose U-Boot runs a boot script from NOR before booting eCos.

U-Boot's built-in environment is a 44-byte block (flash 0x92F09B: `bootcmd=bootsp`, `bootdelay=0`,
`baudrate=921600`) with no room for a real boot command, and `saveenv` has nowhere to save on this board. So:

  bootcmd=source 98e00000;bootsp     (`baudrate` dropped: U-Boot falls back to the same compiled 921600)
  bootdelay=0

and a script uImage at flash 0xE00000 (XIP 0x98E00000; erased in every dump taken so far). If the script is missing
or corrupt, `source` fails, the `;` carries on and `bootsp` boots eCos exactly as before.

The U-Boot uImage is word-sum checked by xboot (header sum at +4, data sum at +24, flag 0x5A at +0x1E); both are
recomputed and the result is checked with the same re-implementation of xboot that every SD card so far went through
(isp/mkisp.py check_stock_uboot).

  patch_uboot.py ../dump1.bin boot.txt ../flash_bootcmd.bin

Writes the full 16 MiB image; only sectors 0x920000 (U-Boot env) and 0xE00000 (script) differ from the input.
"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'isp'))
from mkisp import FLASH_UBOOT_HDR, check_stock_uboot, check_script, mkscript, xboot_dsum, xboot_hsum, be32

ENV_OLD = b'bootcmd=bootsp\0bootdelay=0\0baudrate=921600\0\0'
ENV_NEW = b'bootcmd=source 98e00000;bootsp\0bootdelay=0\0\0'
SCRIPT_FLASH = 0xE00000
SCRIPT_MAX = 0x10000


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    src, script_txt, out = sys.argv[1:]
    d = bytearray(open(src, 'rb').read())
    assert len(d) == 0x1000000
    size = be32(d, FLASH_UBOOT_HDR + 12)
    body = FLASH_UBOOT_HDR + 64
    assert check_stock_uboot(bytes(d[FLASH_UBOOT_HDR:body + size])) is None, 'input U-Boot fails the xboot check'

    i = d.find(ENV_OLD, body, body + size)
    assert i > 0 and d.find(ENV_OLD, i + 1, body + size) < 0, 'default environment not found exactly once'
    assert len(ENV_NEW) <= len(ENV_OLD)
    d[i:i + len(ENV_OLD)] = ENV_NEW.ljust(len(ENV_OLD), b'\0')

    struct.pack_into('>I', d, FLASH_UBOOT_HDR + 24, xboot_dsum(bytes(d[body:body + size])))
    struct.pack_into('>I', d, FLASH_UBOOT_HDR + 4, xboot_hsum(bytes(d[FLASH_UBOOT_HDR:body])))
    err = check_stock_uboot(bytes(d[FLASH_UBOOT_HDR:body + size]))
    assert err is None, 'patched U-Boot fails the xboot check: ' + err

    assert d[SCRIPT_FLASH:SCRIPT_FLASH + SCRIPT_MAX] == b'\xff' * SCRIPT_MAX, 'script sector not erased in input'
    scr = mkscript(open(script_txt).read(), 'nor boot script')
    assert len(scr) <= SCRIPT_MAX and check_script(scr) is None
    d[SCRIPT_FLASH:SCRIPT_FLASH + len(scr)] = scr

    open(out, 'wb').write(d)
    o = open(src, 'rb').read()
    changed = [s for s in range(0, len(d), 0x10000) if d[s:s + 0x10000] != o[s:s + 0x10000]]
    print('env at flash 0x%x patched; U-Boot sums hdr=0x%08x data=0x%08x pass the xboot check' %
          (i, be32(d, FLASH_UBOOT_HDR + 4), be32(d, FLASH_UBOOT_HDR + 24)))
    print('script %d bytes at flash 0x%x (XIP 0x%x)' % (len(scr), SCRIPT_FLASH, 0x98000000 + SCRIPT_FLASH))
    print('64 KB sectors that differ from %s: %s' % (src, ' '.join('0x%06x' % s for s in changed)))


if __name__ == '__main__':
    main()
