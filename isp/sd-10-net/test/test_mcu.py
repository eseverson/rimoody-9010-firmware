#!/usr/bin/env python3
"""Check mcu.c (through mcu_host) with the MCU traffic captured on the unit on 2026-10-31 (RX text log at 0x01600000,
read with hu.py) and against tools/mcuframe.py as the independent frame builder/CRC.
Usage: test_mcu.py path/to/mcu_host. Exit status = failures."""
import importlib.util, os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('mcuframe', os.path.join(here, '../../../tools/mcuframe.py'))
mf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mf)

host = subprocess.Popen([sys.argv[1] if len(sys.argv) > 1 else './mcu_host'], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, text=True)
fails = 0


def do(line):
    host.stdin.write(line + '\n')
    host.stdin.flush()
    out = []
    while True:
        l = host.stdout.readline().rstrip('\n')
        if l == 'end':
            return out
        out.append(l)


def check(name, cond):
    global fails
    print(('PASS ' if cond else 'FAIL ') + name)
    fails += not cond


def writes(out):
    return [bytes.fromhex(l[2:]) for l in out if l.startswith('w ')]


def payloads(out):
    return [f[3:3 + f[2]] for f in writes(out)]


def state(t):
    return do('state %d' % t)[0][6:].split('|')


for p in (b'\xd1\x00', b'\xa1\x0f\x12\x00', b'\xa8\x1a\x0a\x1f', b'\x11\x01', bytes(range(40))):
    check('frame builder == mcuframe.frame(%s)' % p[:4].hex(), do('frame ' + p.hex())[0][6:] == mf.frame(p).hex())
check('heartbeat frame ff aa 02 d1 00 2d be ba', do('frame d100')[0][6:] == 'ffaa02d1002dbeba')

# captured: first burst (a leading stray ff; the last a2 frame lost its CHK/CRC to a FIFO overrun), then a1 every 60 s
cap = [(5939, 'ff ff aa 03 a2 00 01 5a f8 f7 ff aa 02 a4 00 5a 5e ce ff aa 0f a5 4f 63 74 20 38 20 32 34 20 31 31 3a 35 '
              '38 1f 08 6f ff aa 03 a1 00 00 5c 46 47 ff aa 04 a8 17 01 01 3b 14 f8 ff aa 03 a2 00 01'),
       (58444, 'ff aa 03 a1 00 01 5b a8 18'), (118451, 'ff aa 03 a1 00 02 5a 94 45')]
out = do('rx %d %s' % (cap[0][0], cap[0][1].replace(' ', '')))
check('no reply before poll', writes(out) == [])
out = do('poll 5940')
check('a4 with the clock unset -> echo the MCU\'s own a1/a8', payloads(out) == [b'\xa1\x00\x00\x00', b'\xa8\x17\x01\x01'])
check('SoC frames use big-endian CRC', writes(out)[0] == mf.frame(b'\xa1\x00\x00\x00'))
check('11 01 not before 30 ms', payloads(do('poll 5960')) == [])
check('11 01 after 30 ms', payloads(do('poll 5970')) == [b'\x11\x01'])
check('11 01 only once', payloads(do('poll 6100')) == [])
s = state(6100)
check('state: acc 00 01, version, mcu time/date', s[0] == 'acc= 00 01' and s[1] == "version='Oct 8 24 11:58'" and
      s[2] == 'mcu_time=00:00 mcu_date=2023-01-01' and s[3] == 'clock=unset')
check('state: 5 frames, no CHK/CRC failure, leading ff skipped', 'rx_frames=5 tx_frames=3 bad=0' in s[4])
for t, h in cap[1:]:
    do('rx %d %s' % (t, h.replace(' ', '')))
s = state(118500)
check('truncated a2 discarded (stale), following a1 frames parsed', 'rx_frames=7' in s[4] and s[2].startswith(
    'mcu_time=00:02'))
check('history lines: rx/tx with timestamps', s[5] == '5939 rx a2 00 01' and s[-2] == '118451 rx a1 00 02')

# garbage, a corrupted CRC, then a good frame in the same burst
good = mf.frame(b'\xa1\x0f\x12')
good_le = good[:-2] + good[-1:] + good[-2:-1]   # the MCU sends its CRC little-endian
bad = bytearray(good_le)
bad[-1] ^= 0x55
do('rx 200000 ' + (b'\x00\x13\xff' + bytes(bad) + b'\xaa' + good_le).hex())
s = state(200000)
check('corrupted frame counted bad, next frame parsed', 'rx_frames=8' in s[4] and 'bad=1' in s[4] and
      s[2].startswith('mcu_time=15:18'))
check('big-endian CRC from the MCU side is rejected', (do('rx 200100 ' + good.hex()), 'bad=2' in state(200100)[4])[1])

out = do('settime 300000 26 12 31 23 59 59')
check('SETTIME sends a1 h m s + a8 y m d', payloads(out) == [b'\xa1\x17\x3b\x3b', b'\xa8\x1a\x0c\x1f'])
do('rx 302000 ' + bytes(mf.frame(b'\xa4\x00')[:-2] + mf.frame(b'\xa4\x00')[-1:] + mf.frame(b'\xa4\x00')[-2:-1]).hex())
out = do('poll 302500')
check('a4 with the clock set -> clock + 2.5 s, across the new year', payloads(out) == [b'\xa1\x00\x00\x01',
                                                                                     b'\xa8\x1b\x01\x01'])
check('11 01 follows', payloads(do('poll 302530')) == [b'\x11\x01'])
check('state shows the clock', state(302530)[3] == 'clock=2027-01-01 00:00:01')

le = lambda p: (lambda f: f[:-2] + f[-1:] + f[-2:-1])(mf.frame(p))
out = do('rx 400000 ' + le(b'\x00').hex())
check('00 shutdown -> e8 01, save, 00', out[:1] == ['w ' + mf.frame(b'\xe8\x01').hex()] and out[1] == 'shutdown' and
      out[2] == 'w ' + mf.frame(b'\x00').hex())
for p, want in ((b'\xd1\x00', 1), (b'\xad\x01', 0), (b'\x00', 0), (b'\x11\x00', 0), (b'\xe5\x03', 0), (b'\x11\x01', 1),
                (b'\xe5\x02', 1)):
    check('MCU_TX %s %s' % (p.hex(), 'allowed' if want else 'refused'), do('allowed ' + p.hex())[0] == 'allowed %d' % want)

host2 = host
host = subprocess.Popen([sys.argv[1] if len(sys.argv) > 1 else './mcu_host'], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, text=True)
do('rx 1000 ' + le(b'\xa4\x00').hex())
check('a4 with nothing known: wait', payloads(do('poll 2500')) == [])
check('a4 with nothing known: after 2 s only 11 01', payloads(do('poll 3100')) == [] and
      payloads(do('poll 3130')) == [b'\x11\x01'])
print('%d failures' % fails)
sys.exit(fails)
