#!/usr/bin/env python3
"""One MCU experiment per boot cycle of the sd-10-net card, over Wi-Fi.

Waits for the unit's hello, samples the MCU link (GPIO input 0x9C000360 bits 3/4, UART1 status 0x9C000984, the MCU
bytes the payload has logged at 0x01600000), sends the frames of one experiment with `mw.l` to UART1's data register,
samples again, then keeps pinging until the unit stops answering. It logs when the unit went quiet and how long it
took to say hello again: a short gap means the SoC re-entered the card directly (reset inside the SoC, QART kept), a
long one means it went through an eCos boot (the MCU power-cycled it).

Frame bytes are from the eCos analysis (frame builder 0x521aa8: ff aa LEN payload CHK8 CRC16be).

  mcu_try.py e5_00        (see EXPERIMENTS)
"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hu

FRAMES = {
    'd1_00': 'ff aa 02 d1 00 2d be ba',   # heartbeat
    'e5_00': 'ff aa 02 e5 00 19 a8 78',   # guess: clear the e5 01 sent before the update reset
    'e6_00': 'ff aa 02 e6 00 18 70 0e',   # sent by eCos power_on (0x47af0a)
    '11_01': 'ff aa 02 11 01 ec 1f 1e',   # sent after the a4 reply and by power_on
}
EXPERIMENTS = {
    'e5_00': ['e5_00', 'd1_00'],
    'power_on': ['e6_00', '11_01', 'd1_00'],
    'probe': [],
}
# QART is already in 0x9C00317C (stage1); eCos also sets 0x9C003104 bit 5 before its reset (f_0046590a)
REG_KEEP = 0x9c003104


def frame_cmd(name):
    return '; '.join('mw.l 0x9c000980 0x%s' % b for b in FRAMES[name].split())


def sample(u, tag):
    gpio = struct.unpack('<I', u.read(0x9c000360, 4))[0]
    lsr = struct.unpack('<I', u.read(0x9c000984, 4))[0]
    rx = u.read(0x01600000, 1200).split(b'\0')[0].decode(errors='replace').strip()
    print('%s: gpio_in=0x%08x (GPIO3=%d GPIO4=%d) uart1_lsr=0x%x %s | mcu rx: %s' % (
        tag, gpio, gpio >> 3 & 1, gpio >> 4 & 1, lsr, u.ping(), rx or '-'), flush=True)


def main():
    exp = sys.argv[1] if len(sys.argv) > 1 else 'probe'
    token = os.environ['HU_TOKEN']
    ip, hello = hu.listen(900)
    if not ip:
        print('no hello in 900 s', flush=True)
        return 1
    print('hello at %s: %s' % (time.strftime('%T'), hello), flush=True)
    u = hu.Unit(ip, token, timeout=1.5, retries=3)
    sample(u, 'before')
    keep = struct.unpack('<I', u.read(REG_KEEP, 4))[0]
    print(u.cmd('mw.l 0x%x 0x%x; md.l 0x%x 1; md.l 0x9c00317c 1' % (REG_KEEP, keep | 0x20, REG_KEEP)).strip(), flush=True)
    for name in EXPERIMENTS[exp]:
        u.cmd(frame_cmd(name))
        time.sleep(0.3)
        sample(u, 'after %s' % name)
    t_last = time.time()
    while True:
        try:
            u.retries = 1
            p = u.ping()
            t_last = time.time()
            time.sleep(5)
        except TimeoutError:
            break
        if int(time.time()) % 30 < 5:
            sample(u, 'later')
    print('unit quiet from %s (last answer)' % time.strftime('%T', time.localtime(t_last)), flush=True)
    ip, hello = hu.listen(900)
    gap = time.time() - t_last
    print('next hello after %.0f s: %s' % (gap, hello), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
