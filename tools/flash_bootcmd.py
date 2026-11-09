#!/usr/bin/env python3
"""Write the bootcmd patch (tools/patch_uboot.py) into the head unit's NOR over Wi-Fi, through U-Boot's `sfsp`.

  flash_bootcmd.py stage    stage the three new 64 KB blocks in RAM and dry-run the boot script; writes nothing
  flash_bootcmd.py commit   stage, then erase + write + read back + compare; refuses unless U-Boot is < 60 s up (the MCU resets it at ~145 s)

Blocks (flash offsets; `sfsp` takes offsets, 64 KB blocks): 0xE00000 boot script, 0x920000 U-Boot default
environment, 0x8A0000 U-Boot uImage header sums -- written in that order so the window where NOR U-Boot fails xboot's
word-sum check (between the last two) is a couple of seconds. Each block is staged from the live flash (which must
still equal the golden dump), patched with only the differing bytes, and CRC-checked against the target image
before anything is erased. Read-back uses `sfsp read` into a separate buffer, not the cached XIP window.

Recovery if the unit stops booting: CH341A clip + dump1.bin.
"""
import os, re, sys, time, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hu

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, '..', 'dump1.bin')
TARGET = os.path.join(HERE, '..', 'isp', 'bootcmd', 'flash_bootcmd.bin')
SCRIPT = os.path.join(HERE, '..', 'isp', 'bootcmd', 'boot.txt')
BLOCKS = (0xE00000, 0x920000, 0x8A0000)
BLK = 0x10000
STAGE = 0x03000000     # staging buffers, one per block
READBACK = 0x03100000
XIP = 0x98000000


def crc_on_unit(u, addr, n):
    out = u.cmd('crc32 0x%x 0x%x' % (addr, n))
    m = re.search(r'==>\s*([0-9a-f]{8})', out)
    if not m:
        sys.exit('no crc32 result: ' + out)
    return int(m.group(1), 16)


def runs(old, new):
    """Contiguous runs of differing bytes as (offset, bytes)."""
    i, out = 0, []
    while i < len(old):
        if old[i] != new[i]:
            j = i
            while j < len(old) and old[j] != new[j]:
                j += 1
            out.append((i, new[i:j]))
            i = j
        else:
            i += 1
    return out


def stage(u, golden, target):
    for k, b in enumerate(BLOCKS):
        buf = STAGE + k * BLK
        u.cmd('sfsp read 0x%x 0x%x 0x%x' % (buf, b, BLK))
        have = crc_on_unit(u, buf, BLK)
        if have != zlib.crc32(golden[b:b + BLK]) & 0xffffffff:
            sys.exit('flash block 0x%x no longer matches dump1.bin (crc %08x); stopping' % (b, have))
        for off, data in runs(golden[b:b + BLK], target[b:b + BLK]):
            u.write(buf + off, data)
        want = zlib.crc32(target[b:b + BLK]) & 0xffffffff
        have = crc_on_unit(u, buf, BLK)
        if have != want:
            sys.exit('staged block 0x%x crc %08x != %08x; stopping' % (b, have, want))
        print('staged 0x%06x at 0x%x: crc %08x ok' % (b, buf, want), flush=True)


def dry_script(u):
    """Run the boot script from RAM with `go` turned into `echo`, once as written (BOOT.BIN absent: fail path) and
    once loading NET.BIN (success path)."""
    # load to a spare address: 0x1800000 is the payload that is running this
    text = open(SCRIPT).read().replace('go 0x1800000', 'echo DRY go 0x1800000')
    text = text.replace('fatload mmc 0 0x1800000', 'fatload mmc 0 0x3400000')
    assert '0x1800000 /' not in text
    for name, t in (('as written', text), ('with NET.BIN', text.replace('/BOOT.BIN', '/NET.BIN'))):
        sys.path.insert(0, os.path.join(HERE, '..', 'isp'))
        from mkisp import mkscript
        img = mkscript(t, 'dry boot script')
        u.write(0x03080000, img)
        out = u.cmd('source 0x03080000')
        print('--- dry boot script (%s) ---\n%s' % (name, out.strip()), flush=True)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'stage'
    golden, target = open(GOLDEN, 'rb').read(), open(TARGET, 'rb').read()
    ip, hello = hu.listen(1200)
    if not ip:
        sys.exit('no hello')
    print('hello: ' + hello, flush=True)
    u = hu.Unit(ip, os.environ['HU_TOKEN'], timeout=5, retries=3)
    up = int(re.search(r'up=(\d+)', u.ping()).group(1))
    stage(u, golden, target)
    if mode == 'stage':
        dry_script(u)
        print('stage only: nothing written', flush=True)
        return 0
    up = int(re.search(r'up=(\d+)', u.ping()).group(1))
    if up > 60000:
        sys.exit('U-Boot up %d ms: too close to the MCU reset, not writing; run again on a fresh cycle' % up)
    print(u.cmd('sfsp unprotect').strip(), flush=True)
    t0 = time.time()
    for k, b in enumerate(BLOCKS):
        print(u.cmd('sfsp blk_erase 0x%x; sfsp write 0x%x 0x%x 0x%x' % (b, b, STAGE + k * BLK, BLK)).strip(),
              flush=True)
    print('written in %.1f s' % (time.time() - t0), flush=True)
    ok = True
    for k, b in enumerate(BLOCKS):
        u.cmd('mw.l 0x%x 0 0x%x; sfsp read 0x%x 0x%x 0x%x' % (READBACK, BLK // 4, READBACK, b, BLK))
        have, want = crc_on_unit(u, READBACK, BLK), zlib.crc32(target[b:b + BLK]) & 0xffffffff
        print('read back 0x%06x: %08x %s' % (b, have, 'OK' if have == want else 'MISMATCH (want %08x)' % want),
              flush=True)
        ok &= have == want
    print(u.cmd('sfsp protect').strip(), flush=True)
    print('FLASH WRITE %s' % ('VERIFIED' if ok else 'FAILED -- do not power-cycle; re-run commit or use the CH341A'),
          flush=True)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
