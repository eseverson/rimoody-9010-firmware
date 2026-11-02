#!/usr/bin/env python3
"""Run the stock eCos with a register-snapshot hook and collect what it saw (notes/vpp0_display.md, "eCos register
snapshot"; code in isp/ecos-snap/).

  ecos_snap.py run ECOS.BIN      patch the decompressed eCos image (flash 0x95CF40, LZO; bootsp loads it to 0x301040),
                                 upload it, HOOK.BIN and the launcher's parameters, then EXEC LAUNCH.BIN. eCos boots,
                                 the hook snapshots at vsync calls 120 / 600 / 1500 and resets the unit at 1560 (~26 s).
  ecos_snap.py card ECOS.BIN      write the patched image, ENTRY.BIN and HOOK.BIN to the SD card (power-on path)
  ecos_snap.py arm -              write /ECOSRUN.FLG: the next cold boot runs eCos with the hook (net.c ecos_boot_check)
  ecos_snap.py collect OUT.BIN   after the unit is back on our payload: read the snapshot area, save it, print a summary.
  ecos_snap.py show OUT.BIN      print the summary of a saved snapshot.
  ecos_snap.py card-audio ECOS.BIN   like card, for the audio run: HOOKA.BIN (snapshots of the audio block at 25 / 26 s,
                                 the register sweep at 27 s) and the call loggers of stubs.S patched into the image
  ecos_snap.py show-audio OUT.BIN    decode an audio run: the DSP / AUDIF call log, eCos's audio debug prints, registers
"""
import os, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SNAPDIR = os.path.join(HERE, '..', 'isp', 'ecos-snap')
ECOS_BASE = 0x00301040
HOOK_ADDR, PARAM, SNAP, SNAP_END = 0x02F40000, 0x02F47F00, 0x02F48000, 0x02F88000
NHIST = 2048
# (eCos address, original bytes, patched bytes); instruction bytes are big-endian
PATCHES = [
    (0x64b436, bytes.fromhex('46f00607'), bytes.fromhex('46f00301')),   # clock ISR's call of hal_clock_reset 0x607fe2 ...
    (0x64b43a, bytes.fromhex('58f78fe2'), bytes.fromhex('58f78000')),   # ... -> entry.S + 0 (fixed-rate hook)
    (0x511628, bytes.fromhex('46f0050f'), bytes.fromhex('46f00301')),   # f_005115e8's call of f_0050f7c0 ...
    (0x51162c, bytes.fromhex('58f787c0'), bytes.fromhex('58f78004')),   # ... -> entry.S + 4
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
    print(hu('say', 'eCos snapshot run %s: screen goes to stock eCos, unit resets ITSELF ~3 s after video playback starts (or after 4 min).'
             % time.strftime('%H:%M')))
    print(hu('exec', os.path.join(SNAPDIR, 'LAUNCH.BIN'), '--wait', '3'))
    print('eCos should be booting now; the hook resets the unit ~26 s after its display starts (or the watchdog ~13 s after a hang), then run `collect`.')


AUDIO_SITES = {1: 0x3bbf74, 2: 0x3bafe0, 3: 0x3baf48, 4: 0x3aa8a0, 5: 0x3bb996, 6: 0x3aa50a, 7: 0x3aa184,
               8: 0x3a3f8e, 9: 0x3a4636, 16: 0x5fd67a}
AUDIO_NAMES = {1: 'DSP_SendCmd', 2: 'DownloadCode', 3: 'Download2', 4: 'AUDIF_Set', 5: 'DSP_InitSystem',
               6: 'AUDIF_ResetDsp', 7: 'SetVolume', 8: 'Track_Create', 9: 'Track_Start', 16: 'print'}
STUBS_ADDR = 0x317cb6


def audio_patch(img):
    """stubs.S over the dead `pwm` handler, and `j stub_N; nop16` over each site's 6-byte prologue"""
    stubs = open(os.path.join(SNAPDIR, 'STUBS.BIN'), 'rb').read()
    sym = {l.split()[2]: int(l.split()[0], 16) for l in open(os.path.join(SNAPDIR, 'stubs.sym'))}
    o = STUBS_ADDR - ECOS_BASE
    if img[o:o + 2] != bytes.fromhex('fc48') or len(stubs) > 0x508:
        sys.exit('pwm handler bytes at %#x are %s (want fc48), stubs %d bytes (max 0x508)' % (STUBS_ADDR, img[o:o + 2].hex(), len(stubs)))
    img[o:o + len(stubs)] = stubs
    for i, site in AUDIO_SITES.items():
        so = site - ECOS_BASE
        first = img[so:so + 6]
        ok = (first[0] == 0xfc and first[2:4] == bytes.fromhex('51cf')) or first[:2] == bytes.fromhex('3a1f')
        if not ok:
            sys.exit('site %d %#x starts %s, not a push25/addi or smw prologue' % (i, site, first.hex()))
        d = (sym['stub_%d' % i] - site) >> 1
        img[so:so + 6] = struct.pack('>I', 0x48000000 | (d & 0xFFFFFF)) + bytes.fromhex('9200')


def card(path, audio=False):
    """Write the patched eCos, entry.S and the hook to the SD card for the payload's power-on path (net.c
    ecos_boot_check); `arm` then writes the one-shot flag, and the next cold boot (ACC cycle) runs eCos."""
    img = bytearray(open(path, 'rb').read())
    for a, old, new in PATCHES:
        o = a - ECOS_BASE
        if img[o:o + 4] != old:
            sys.exit('eCos bytes at %#x are %s, expected %s' % (a, img[o:o + 4].hex(), old.hex()))
        img[o:o + 4] = new
    if audio:
        audio_patch(img)
    tmp = os.path.join(SNAPDIR, 'ecos_patched.bin')
    open(tmp, 'wb').write(img)
    for name, src in (('ECOSP.BIN', tmp), ('ENTRY.BIN', os.path.join(SNAPDIR, 'ENTRY.BIN')),
                      ('HOOK.BIN', os.path.join(SNAPDIR, 'HOOKA.BIN' if audio else 'HOOK.BIN'))):
        data = open(src, 'rb').read()
        print(hu('write', '0x600000', src))
        print(hu('--timeout', '60', 'fs', 'write', name, '0x600000', str(len(data))))
        r = hu('--timeout', '60', 'fs', 'crc', name)
        want = '%08x' % (zlib.crc32(data) & 0xFFFFFFFF)
        print(r, 'OK' if want in r else 'MISMATCH (want %s)' % want)
        if want not in r:
            sys.exit(1)


def arm(_):
    flag = os.path.join(SNAPDIR, 'flag.bin')
    open(flag, 'wb').write(b'ECOSRUN\n')
    print(hu('write', '0x600000', flag))
    print(hu('fs', 'write', 'ECOSRUN.FLG', '0x600000', '8'))
    print('armed: the next cold boot (ACC off/on) starts the stock eCos with the hook; the hook resets the unit ~3 s '
          'after video playback starts (or after 4 min), then `collect`.')


def collect(out):
    prm = out + '.param'
    hu('read', hex(PARAM), '20', '--out', prm)
    p = struct.unpack('<5I', open(prm, 'rb').read())
    print('hook crumb %x (1..ff: entered the hook that many times; 100/101: around the first timer read; '
          '200/300/400 + 1: snapshot k started / finished; 500: reset)' % p[4])
    p = p[:4]
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
    h = struct.unpack_from('<21I', data, 0)
    if h[0] != 0x50414E53:
        print('no snapshot header (magic %08x): the hook never ran, or the area was overwritten' % h[0])
        return
    names = ['magic', 'ver', 'calls', 'done', 'first_err_call', 'first_err_flags', 'first_ck_call', 'first_ck',
             'snap0', 'snap1', 'snap2', 'snap3', 'or_flags', 'ck_changes', 'last_flags', 'last_ck', 'ticks', 'calls_disp1', 't0', 't_last', 'trig_tick']
    print(' '.join('%s=%x' % (n, v) for n, v in zip(names, h)))
    print('hook ran for %.2f s of wall clock, %d calls' % (((h[19] - h[18]) & 0xFFFFFFFF) / 1e6, h[2]))
    for name, off, n, step in (('tick', 0x100, min((h[2] + 7) // 8, NHIST), 8),
                               ('disp1 call', 0x100 + 16 * NHIST, min(h[17], 256), 1)):
        prev = None
        for i in range(n):   # print the history where it changes
            e = struct.unpack_from('<4I', data, off + 16 * i)
            if e != prev:
                print('  %s %5d: 7ec %08x 7e8 %04x 788 %02x vscl %04x dmix %04x' % (name, i * step, e[0], e[1], e[2],
                                                                                    e[3] >> 16, e[3] & 0xFFFF))
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


def cstr(a, ecos, flash):
    if 0x98000000 <= a < 0x99000000:
        b, o = flash, a - 0x98000000
    elif ECOS_BASE <= a < ECOS_BASE + len(ecos):
        b, o = ecos, a - ECOS_BASE
    else:
        return None
    e = b.find(b'\0', o, o + 300)
    return b[o:e if e >= 0 else o + 300].decode('latin1')


def fmt_print(fmt, args, ecos, flash):
    out, ai, i = '', 0, 0
    while i < len(fmt):
        c = fmt[i]
        if c != '%':
            out += c
            i += 1
            continue
        j = i + 1
        while j < len(fmt) and fmt[j] in '-+ #0123456789.lhz':
            j += 1
        if j >= len(fmt):
            break
        t = fmt[j]
        if t == '%':
            out += '%'
        else:
            v = args[ai] if ai < len(args) else None
            ai += 1
            if v is None:
                out += '?'
            elif t == 's':
                out += cstr(v, ecos, flash) or '<%x>' % v
            elif t in 'xXp':
                out += '%x' % v
            elif t == 'c':
                out += chr(v & 0xff)
            elif t in 'du i':
                out += str(v - (1 << 32) if v & 0x80000000 else v)
            else:
                out += str(v)
        i = j + 1
    return out


def show_audio(path):
    data = open(path, 'rb').read()
    ecos = open(os.path.join(HERE, '..', 'isp', 'ecos-snap', 'ecos_patched.bin'), 'rb').read()
    flash = open(os.path.join(HERE, '..', 'dump1.bin'), 'rb').read()
    h = struct.unpack_from('<21I', data, 0)
    print('hook: magic %x calls %d done %d snaps %s' % (h[0], h[2], h[3], [hex(x) for x in h[8:11]]))
    lines = []
    for name, off in (('A', 0x20000), ('B', 0x30000)):
        n = struct.unpack_from('<I', data, off)[0]
        print('ring %s: %d calls logged (%d kept)' % (name, n, min(n, 2047)))
        for k in range(min(n, 2047)):
            e = struct.unpack_from('<8I', data, off + 32 + 32 * k)
            lines.append((e[6], name, k, e))
    lines.sort(key=lambda x: (x[0], x[1] == 'B', x[2]))
    out = []
    for tick, ring, k, e in lines:
        i, r0, r1, r2, r3, lp, _, r4 = e
        if i == 16:
            f = cstr(r0, ecos, flash)
            txt = fmt_print(f, [r1, r2, r3, r4], ecos, flash) if f else '<fmt %x>' % r0
            out.append('%7d  print  %s   (from %x)' % (tick, txt.rstrip(), lp - 4))
        else:
            out.append('%7d  %-14s %8x %8x %8x %8x   (from %x)' % (tick, AUDIO_NAMES.get(i, str(i)), r0, r1, r2, r3, lp - 4))
    txt = path + '.calls.txt'
    open(txt, 'w').write('\n'.join(out) + '\n')
    print('%d log lines -> %s' % (len(out), txt))
    for k, off in ((0, 0x10000), (1, 0x14000), (2, 0x18000)):
        regs = blocks(data, off)
        if regs:
            f = path + '.audio%d.txt' % k
            with open(f, 'w') as o:
                for a in sorted(regs):
                    o.write('%08x %08x\n' % (a, regs[a]))
            print('snapshot %d: %d registers -> %s' % (k, len(regs), f))


if __name__ == '__main__':
    cmds = {'run': run, 'card': card, 'card-audio': lambda p: card(p, True), 'arm': arm, 'collect': collect,
            'show': show, 'show-audio': show_audio}
    if len(sys.argv) != 3 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](sys.argv[2])
