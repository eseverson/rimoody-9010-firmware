#!/usr/bin/env python3
"""Update only the NOR boot script (flash 0xE00000, run by the patched U-Boot's `source 98e00000`) over Wi-Fi.

  flash_script.py test     cold-boot SD simulation + dry run of the new script from RAM; writes nothing
  flash_script.py commit   test, then erase + write + read back the script block

A broken script is harmless: `source` fails or the script ends, and bootcmd's `;bootsp` boots eCos.

The simulation: on a cold boot xboot never sets up the SD pins (it only does for an SD update: 0x9C00008C =
0x00F00050, 0x9C000100/0x9C00010C |= 0x00FC00FC, xboot 0x9e831cdc), so U-Boot's `mmc` may not see the card. With the
unit running in the update path, clear the SD pin mux, check that a fatload fails, apply the script's pin writes,
check that it works again.
"""
import os, re, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'isp'))
import hu
from mkisp import check_script, mkscript

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'isp', 'sd-11-bootcmd', 'boot.txt')
BLOCK, BLK = 0xE00000, 0x10000
STAGE, READBACK, DRY = 0x03000000, 0x03100000, 0x03080000
PINS = 'mw.l 0x9c00008c 0x00f00050;mw.l 0x9c000100 0x00fc00fc;mw.l 0x9c00010c 0x00fc00fc'


def crc_on_unit(u, addr, n):
    out = u.cmd('crc32 0x%x 0x%x' % (addr, n))
    m = re.search(r'==>\s*([0-9a-f]{8})', out)
    if not m:
        sys.exit('no crc32 result: ' + out)
    return int(m.group(1), 16)


def loads(out):
    return 'bytes read' in out and 'Unable' not in out


def test(u, text):
    print('--- cold-boot SD simulation ---', flush=True)
    base = u.cmd('md.l 0x9c00008c 1').split('\n')[0]
    cleared = u.cmd('mw.l 0x9c00008c 0x00f00000; mmc rescan; fatload mmc 0 0x3400000 /BOOT.BIN')
    fixed = u.cmd(PINS + '; mmc rescan; fatload mmc 0 0x3400000 /BOOT.BIN')
    print('mux before: %s\nmux cleared -> fatload %s\npins set   -> fatload %s' % (
        base, 'ok' if loads(cleared) else 'FAILED', 'ok' if loads(fixed) else 'FAILED'), flush=True)
    if not loads(fixed):
        print(cleared.strip() + '\n' + fixed.strip(), flush=True)
    dry = text.replace('go 0x1800000', 'echo DRY go 0x1800000').replace(' 0x1800000 /BOOT.BIN', ' 0x3400000 /BOOT.BIN')
    assert '0x1800000 /' not in dry
    u.write(DRY, mkscript(dry, 'dry boot script'))
    print('--- dry boot script ---\n' + u.cmd('source 0x%x' % DRY).strip(), flush=True)
    return loads(fixed)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'test'
    text = open(SCRIPT).read()
    img = mkscript(text, 'nor boot script')
    assert check_script(img) is None and len(img) <= BLK
    block = img + b'\xff' * (BLK - len(img))
    want = zlib.crc32(block) & 0xffffffff
    ip, hello = hu.listen(1500)
    if not ip:
        sys.exit('no hello')
    print('hello: ' + hello, flush=True)
    u = hu.Unit(ip, os.environ['HU_TOKEN'], timeout=5, retries=3)
    if not test(u, text):
        sys.exit('SD did not come back with the script\'s pin writes; not flashing this script')
    if mode != 'commit':
        print('test only: nothing written', flush=True)
        return 0
    u.cmd('mw.b 0x%x 0xff 0x%x' % (STAGE, BLK))
    u.write(STAGE, img)
    if crc_on_unit(u, STAGE, BLK) != want:
        sys.exit('staged script block crc mismatch; not writing')
    up = int(re.search(r'up=(\d+)', u.ping()).group(1))
    if up > 100000:
        sys.exit('U-Boot up %d ms: too close to the MCU reset; run again on a fresh cycle' % up)
    u.cmd('sfsp unprotect; sfsp blk_erase 0x%x; sfsp write 0x%x 0x%x 0x%x' % (BLOCK, BLOCK, STAGE, BLK))
    u.cmd('mw.l 0x%x 0 0x%x; sfsp read 0x%x 0x%x 0x%x' % (READBACK, BLK // 4, READBACK, BLOCK, BLK))
    have = crc_on_unit(u, READBACK, BLK)
    u.cmd('sfsp protect')
    print('script block read back %08x: %s' % (have, 'VERIFIED' if have == want else 'MISMATCH (want %08x)' % want),
          flush=True)
    return 0 if have == want else 1


if __name__ == '__main__':
    sys.exit(main())
