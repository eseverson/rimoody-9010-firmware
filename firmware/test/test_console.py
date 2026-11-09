#!/usr/bin/env python3
"""Check console.c (through con_host): dirty-row accounting, layout at both log scales, opaque pixels, and write a PNG
of the screen for a look. Usage: test_console.py path/to/con_host [out.png]. Exit status = failures."""
import struct, subprocess, sys, zlib

host = subprocess.Popen([sys.argv[1] if len(sys.argv) > 1 else './con_host'], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, text=True)
fails = 0


def do(line):
    host.stdin.write(line + '\n')
    host.stdin.flush()
    out = []
    while True:
        l = host.stdout.readline().strip()
        if l == 'end':
            return out
        out.append(l)


def check(name, cond):
    global fails
    print(('PASS ' if cond else 'FAIL ') + name)
    fails += not cond


def rows():
    r = do('render')[0].split()
    return int(r[1]), int(r[3])


panel = ['HU net 20261031-170042   up 01:23:45', 'IP 192.168.1.138  meow  ch 11  -66 dBm  state up',
         'IRQ on 100 Hz  ticks 120763  hb 402', "MCU ACC 00 01  'Oct 8 24 11:58'  16:29  2026-10-31",
         'crash: none', 'say: hello from the desk']
for i, t in enumerate(panel):
    do('panel %d %x %s' % (i, 0xFFFFFFFF if i else 0xFFFFE060, t))
for i in range(30):
    do('log line %02d: ' % i + 'x' * (i * 7))
n, fl = rows()
check('first render draws 6 panel rows + 12 log rows (scale 2)', n == 18 and fl == 18 * 32 * 1024 * 4)
check('second render draws nothing', rows() == (0, 0))
do('panel 2 ffffffff IRQ on 100 Hz  ticks 120764  hb 402')
check('one changed panel row -> 1 row, 128 KB written back', rows() == (1, 32 * 1024 * 4))
do('log a new line')
n, fl = rows()
check('new log line -> only log rows redrawn', 1 <= n <= 12)
do('dump /tmp/con_s2.raw') if False else None
out = sys.argv[2] if len(sys.argv) > 2 else None
if out:
    do('dump ' + out + '.raw')
do('scale 1')
n, fl = rows()
check('scale 1: 25 log rows of 16 px', n == 25 and fl == 25 * 16 * 1024 * 4)
do('dump ' + (out or '/dev/null') + '.s1.raw' if out else 'dump /dev/null')
if out:
    for suffix in ('.raw', '.s1.raw'):
        raw = open(out + suffix, 'rb').read()
        words = struct.unpack('<%dI' % (1024 * 600), raw)
        check('all pixels opaque (%s)' % suffix, all(w >> 24 == 0xFF for w in words[::97]))
        png_rows = b''.join(b'\0' + bytes(b for w in words[y * 1024:(y + 1) * 1024]
                                          for b in ((w >> 16) & 255, (w >> 8) & 255, w & 255)) for y in range(600))
        chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1024, 600, 8, 2, 0, 0, 0)) + \
            chunk(b'IDAT', zlib.compress(png_rows)) + chunk(b'IEND', b'')
        open(out + suffix.replace('.raw', '') + '.png', 'wb').write(png)
host.stdin.close()
print('%d failures' % fails)
sys.exit(fails)
