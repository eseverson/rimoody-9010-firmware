#!/usr/bin/env python3
"""H.264 decoder bring-up step 1 (notes/h264_decoder.md section 6): read-only snapshot over HU1.

Reads the MOON0 clock / gated-clock / reset registers first (always safe). Only if every clock the MFD path is
believed to need is on and the MFD resets are released does it go on to read the decoder's own registers, because a
read from an unclocked block can stall the bus until a power cycle. --force skips that check.

  vdec_probe.py [--force]      (needs $HU_TOKEN, e.g. `source tools/hu.env`)
"""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

def rd(addr, n):
    with tempfile.NamedTemporaryFile() as f:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'hu.py'), 'read', hex(addr), str(n), '-o', f.name],
                           capture_output=True, text=True, timeout=60)
        d = open(f.name, 'rb').read()
        if len(d) != n:
            sys.exit('read %#x failed: %s' % (addr, (r.stdout + r.stderr).strip()[-200:]))
        return d

def words(addr, n):
    d = rd(addr, 4 * n)
    return list(struct.unpack('<%dI' % n, d))

def show(name, base, vals):
    print(name)
    for i in range(0, len(vals), 4):
        print('  %08x: %s' % (base + 4 * i, ' '.join('%08x' % v for v in vals[i:i + 4])))

# clock IDs the MFD path may need (VC-1/RV enables; H.264 enables nothing itself) and the 12 MFD reset IDs
CLK_IDS = [0x5a, 0x64, 0x66, 0x69, 0x71]
RST_IDS = [0x68, 0x69, 0x6f, 0x71, 0x70, 0x6c, 0x6b, 0x64, 0x66, 0x1f, 0x63, 0x67]

def bit(regs, base, idbase, i):
    return (regs[(idbase - base) // 4 + (i >> 4)] >> (i & 15)) & 1

def main():
    force = '--force' in sys.argv
    clk = words(0x9c000004, 20)    # 0x9C000004..0x9C000050: clock enables + gated clocks
    rst = words(0x9c000054, 9)     # 0x9C000054..0x9C000074: resets
    show('MOON0 clocks 0x9C000004..', 0x9c000004, clk)
    show('MOON0 resets 0x9C000054..', 0x9c000054, rst)
    clk_on = {i: bit(clk, 0x9c000004, 0x9c000004, i) for i in CLK_IDS}
    in_rst = {i: (rst[(i >> 4)] >> (i & 15)) & 1 for i in RST_IDS}
    print('clock IDs on:   ' + ' '.join('%02x=%d' % (i, v) for i, v in clk_on.items()))
    print('reset asserted: ' + ' '.join('%02x=%d' % (i, v) for i, v in in_rst.items()))
    if not force and (not all(clk_on.values()) or any(in_rst.values())):
        print('STOP: an MFD clock is off or a reset is held; reading the decoder could stall the bus. '
              'Enable the clocks / release the resets first, or rerun with --force.')
        return 1
    show('BG2D pitch/swizzle 0x9C00A380..', 0x9c00a380, words(0x9c00a380, 16))
    show('BG2D index table 0x9C00A400 (first 0x40)', 0x9c00a400, words(0x9c00a400, 0x40))
    for a in (0x9c00d000, 0x9c00d04c, 0x9c00d0ac, 0x9c00dc84, 0x9c00d550, 0x9c00d570, 0x9c00d704, 0x9c00d8a0,
              0x9c00d920):
        print('MFD %08x = %08x' % (a, words(a, 1)[0]))
    return 0

if __name__ == '__main__':
    sys.exit(main())
