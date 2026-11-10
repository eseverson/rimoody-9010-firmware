#!/usr/bin/env python3
"""Sample the SoC's SAR ADC (G94, 0x9C002F00) over Wi-Fi the way eCos's key driver does (0x51F2B2..0x51F3F8):
stop (+0x2C &= ~4), select channel (+0x2C bits 5:3), start (+0x2C |= 4), read +0x34 bits 11:0.
eCos's front-panel key ladders are channels 0 and 1 (keys below 0xF7 of the 8-bit value).

  adc.py once            one sample of channels 0-3
  adc.py watch [ch..]    print a line whenever a channel's 8-bit value changes (default 0 1)
"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hu

BASE = 0x9c002f00


def sample(u, ch):
    ctl = struct.unpack('<I', u.read(BASE + 0x2c, 4))[0]
    ctl &= ~4
    sel = (ctl & ~0x38) | (ch << 3)
    u.cmd('mw.l 0x%x 0x%x; mw.l 0x%x 0x%x; mw.l 0x%x 0x%x' % (BASE + 0x2c, ctl, BASE + 0x2c, sel, BASE + 0x2c,
                                                             sel | 4))
    time.sleep(0.01)
    raw = struct.unpack('<I', u.read(BASE + 0x34, 4))[0] & 0xfff
    return raw, raw >> 4


def main():
    u = hu.Unit(os.environ.get('HU_IP') or hu.listen(10)[0], os.environ['HU_TOKEN'], timeout=2, retries=2)
    if sys.argv[1:2] == ['watch']:
        chans = [int(c) for c in sys.argv[2:]] or [0, 1]
        last = {}
        while True:
            for ch in chans:
                raw, v = sample(u, ch)
                if ch not in last or abs(last[ch] - v) > 3:
                    print('%s ch%d = 0x%02x (raw 0x%03x)' % (time.strftime('%T'), ch, v, raw), flush=True)
                    last[ch] = v
    else:
        for ch in range(4):
            raw, v = sample(u, ch)
            print('ch%d raw 0x%03x -> 0x%02x' % (ch, raw, v))


if __name__ == '__main__':
    main()
