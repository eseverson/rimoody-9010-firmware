#!/usr/bin/env python3
"""Run the stock eCos with a register-snapshot hook and collect what it saw (notes/vpp0_display.md, "eCos register
snapshot"; code in isp/ecos-snap/).

  ecos_snap.py run ECOS.BIN      patch the decompressed eCos image (flash 0x95CF40, LZO; bootsp loads it to 0x301040),
                                 upload it, HOOK.BIN and the launcher's parameters, then EXEC LAUNCH.BIN. eCos boots,
                                 the hook snapshots at vsync calls 120 / 600 / 1500 and resets the unit at 1560 (~26 s).
  ecos_snap.py collect OUT.BIN   after the unit is back on our payload: read the snapshot area, save it, print a summary.
  ecos_snap.py show OUT.BIN      print the summary of a saved snapshot.
"""
import os, struct, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SNAPDIR = os.path.join(HERE, '..', 'isp', 'ecos-snap')
ECOS_BASE = 0x00301040
HOOK_ADDR, PARAM, SNAP, SNAP_END = 0x06F00000, 0x06F07F00, 0x06F08000, 0x06F40000
NHIST = 2048
# (eCos address, original bytes, patched bytes); instruction bytes are big-endian
PATCHES = [
    (0x5114ec, bytes.fromhex('46f0050f'), bytes.fromhex('46f00301')),   # sethi $r15, 0x50f -> 0x301
    (0x5114f0, bytes.fromhex('58f78284'), bytes.fromhex('58f78000')),   # ori $r15, $r15, 0x284 -> 0: entry.S at 0x301000
]
ENTRY_ADDR = 0x00301000   # entry.S in the unused uImage header slot (inside eCos's 64 MB MPU segment)


def hu(*args, timeout=600):
    r = subprocess.run([sys.executable, os.path.join(HERE, 'hu.py')] + list(args), capture_output=True, text=True,
                       timeout=timeout)
    return (r.stdout + r.stderr).strip()


def run(path):
    img = bytearray(open(path, 'rb').read())
    if len(img) != 0x3D6038:
        sys.exit('unexpected eCos image size %#x (want 0x3D6038)' % len(img))
    for a, old, new in PATCHES:
        o = a - ECOS_BASE
        if img[o:o + 4] != old:
            sys.exit('eCos bytes at %#x are %s, expected %s' % (a, img[o:o + 4].hex(), old.hex()))
        img[o:o + 4] = new
    crc = zlib.crc32(img) & 0xFFFFFFFF
    tmp = os.path.join(SNAPDIR, 'ecos_patched.bin')
    open(tmp, 'wb').write(img)
    prm = os.path.join(SNAPDIR, 'param.bin')
    open(prm, 'wb').write(struct.pack('<III', 0x434E4C45, len(img), crc))
    print('patched eCos: %d bytes, crc %08x' % (len(img), crc))
    print(hu('write', hex(HOOK_ADDR), os.path.join(SNAPDIR, 'HOOK.BIN')))
    print(hu('write', hex(ENTRY_ADDR), os.path.join(SNAPDIR, 'ENTRY.BIN')))
    print(hu('write', hex(ECOS_BASE), tmp))
    print(hu('write', hex(PARAM), prm))
    back = hu('read', hex(ECOS_BASE), str(len(img)), '--out', tmp + '.rb')
    if open(tmp + '.rb', 'rb').read() != bytes(img):
        sys.exit('read-back of the uploaded image differs (%s); not starting eCos' % back)
    print('read-back ok; EXEC launcher')
    print(hu('exec', os.path.join(SNAPDIR, 'LAUNCH.BIN'), '--wait', '3'))
    print('eCos should be booting now; the hook resets the unit ~26 s after its display starts (or the watchdog ~13 s after a hang), then run `collect`.')


def collect(out):
    prm = out + '.param'
    hu('read', hex(PARAM), '16', '--out', prm)
    p = struct.unpack('<4I', open(prm, 'rb').read())
    print('launcher param: magic %08x len %x crc %08x marker %08x (%s)' % (p + ('launcher reached eCos entry'
          if p[3] == 0x48434E4C else 'launcher did not get to the jump',)))
    data = b''
    for a in range(SNAP, SNAP_END, 0x8000):
        tmp = out + '.part'
        hu('read', hex(a), str(0x8000), '--out', tmp)
        data += open(tmp, 'rb').read()
    os.remove(out + '.part')
    open(out, 'wb').write(data)
    show(out)


def blocks(data, off):
    regs, w = {}, off
    while w + 8 <= len(data):
        a, n = struct.unpack_from('<II', data, w)
        if a == 0:
            break
        if n > 512:
            print('  corrupt block at +%#x' % w)
            break
        for i in range(n):
            regs[a + 4 * i] = struct.unpack_from('<I', data, w + 8 + 4 * i)[0]
        w += 8 + 4 * n
    return regs


def show(path):
    data = open(path, 'rb').read()
    h = struct.unpack_from('<16I', data, 0)
    if h[0] != 0x50414E53:
        print('no snapshot header (magic %08x): the hook never ran, or the area was overwritten' % h[0])
        return
    names = ['magic', 'ver', 'calls', 'done', 'first_err_call', 'first_err_flags', 'first_ck_call', 'first_ck',
             'snap0', 'snap1', 'snap2', 'snap3', 'or_flags', 'ck_changes', 'last_flags', 'last_ck']
    print(' '.join('%s=%x' % (n, v) for n, v in zip(names, h)))
    hist = [struct.unpack_from('<4I', data, 0x100 + 16 * i) for i in range(min(h[2], NHIST))]
    prev = None
    for i, e in enumerate(hist):   # print the history where it changes
        if e != prev:
            print('  call %4d: 7ec %08x 7e8 %04x 788 %02x vscl %04x dmix %04x' % (i, e[0], e[1], e[2], e[3] >> 16,
                                                                                  e[3] & 0xFFFF))
            prev = e
    for k in range(3):
        regs = blocks(data, 0x10000 * (k + 1))
        if not regs:
            continue
        out = path + '.snap%d.txt' % k
        with open(out, 'w') as f:
            for a in sorted(regs):
                f.write('%08x %08x\n' % (a, regs[a]))
        dd = ' '.join('%x=%x' % (a & 0xFFF, regs[a]) for a in sorted(regs) if 0x9C005780 <= a < 0x9C005800 and regs[a])
        print('snap%d (call %d): %d registers -> %s\n  DDFCH %s' % (k, h[8 + k], len(regs), out, dd))


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] not in ('run', 'collect', 'show'):
        sys.exit(__doc__)
    {'run': run, 'collect': collect, 'show': show}[sys.argv[1]](sys.argv[2])
