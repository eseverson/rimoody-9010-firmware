#!/usr/bin/env python3
"""Talk to the Goodix GT9xx touch controller over bit-banged I2C, from the desktop, through tools/hu.py U-Boot commands.

WARNING (2026-10-31): the first run rebooted the unit at the first SDA/SCL toggle packet (see notes/display_touch.md,
"Live touch attempt"). Do not run again until it is known what GPIO91/92 are wired to on this board.

Wiring (eCos touch driver 0x516e5a, sysio map 0x6c7a64): RST = GPIO90, SDA = GPIO91, SCL = GPIO92, INT = GPIO93.
I2C address 0x5D (eCos sends 0xBA/0xBB). GPIO registers (eCos helpers 0x5203bc/0x520414/0x520394, 0x5204c0):
  OE  0x9c000334  masked write: bits[31:16] = write-enable mask, bits[15:0] = value, bit = pin-80
  OUT 0x9c000354  same layout
  IN  0x9c000368  bit = pin-64
  GPIO_FIRST 0x9c0032ec  bit = pin-64 (1 = pad owned by the GPIO block); plain read-modify-write register
Open-drain emulation: OUT = 0 for SDA/SCL, then a line is pulled low by OE = 1 and released (external pull-up) by OE = 0.
Only SDA/SCL are switched to GPIO; RST/INT are left alone. GPIO_FIRST is restored on exit.

  gt9.py id                 product id + firmware (0x8140, 6 bytes), config version, x/y resolution, touch count
  gt9.py read REG N         hex dump N bytes from 16-bit register REG
  gt9.py poll [--secs S]    print touch points (0x814E status, 0x8150 records) until S seconds elapse

Slow (each SCL edge is a U-Boot `mw.l` over UDP): about 1-2 s per small register read. Run `source tools/hu.env` first.
"""
import argparse, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hu  # noqa: E402

OE, OUT, IN, FIRST = 0x9C000334, 0x9C000354, 0x9C000368, 0x9C0032EC
SDA_PIN, SCL_PIN = 91, 92
SDA_B, SCL_B = 1 << (SDA_PIN - 80), 1 << (SCL_PIN - 80)
SDA_IN = 1 << (SDA_PIN - 64)
ADDR = 0x5D


class Bus:
    def __init__(self, unit):
        self.u = unit
        self.ops = []      # pending U-Boot commands
        self.reads = []    # callbacks for md results, in order

    def _mw(self, reg, val):
        self.ops.append('mw.l 0x%08x 0x%08x' % (reg, val))

    def sda(self, high):
        self._mw(OE, (SDA_B << 16) | (0 if high else SDA_B))

    def scl(self, high):
        self._mw(OE, (SCL_B << 16) | (0 if high else SCL_B))

    def sample(self, sink):
        self.ops.append('md.l 0x%08x 1' % IN)
        self.reads.append(sink)

    def flush(self):
        """Send queued ops in packets of <= 900 chars / 8 reads; feed md results to sinks in order."""
        sinks = list(self.reads)
        vals = []
        pkt, nreads = [], 0
        def send():
            if not pkt:
                return
            out = self.u.cmd('; '.join(pkt))
            vals.extend(int(m, 16) for m in re.findall(r'^9c000368: ([0-9a-f]{8})', out, re.M))
        for op in self.ops:
            is_read = op.startswith('md')
            if len('; '.join(pkt + [op])) > 900 or (is_read and nreads >= 8):
                send(); pkt, nreads = [], 0
            pkt.append(op)
            nreads += is_read
        send()
        self.ops, self.reads = [], []
        if len(vals) != len(sinks):
            raise IOError('expected %d samples, got %d' % (len(sinks), len(vals)))
        for s, v in zip(sinks, vals):
            s(bool(v & SDA_IN))

    # --- I2C primitives (queued) ---
    def start(self):
        self.sda(True); self.scl(True); self.sda(False); self.scl(False)

    def stop(self):
        self.sda(False); self.scl(True); self.sda(True)

    def write_byte(self, b, acks):
        for i in range(7, -1, -1):
            self.sda(bool(b >> i & 1)); self.scl(True); self.scl(False)
        self.sda(True); self.scl(True); self.sample(lambda v: acks.append(not v)); self.scl(False)

    def read_byte(self, out, last):
        bits = []
        self.sda(True)
        for _ in range(8):
            self.scl(True); self.sample(bits.append); self.scl(False)
        self.sda(last)            # ACK = pull low, NACK on the last byte = release
        self.scl(True); self.scl(False); self.sda(True)
        out.append(bits)

    def read_reg(self, reg, n):
        acks, raw = [], []
        self.start()
        self.write_byte(ADDR << 1, acks); self.write_byte(reg >> 8, acks); self.write_byte(reg & 0xFF, acks)
        self.start()
        self.write_byte(ADDR << 1 | 1, acks)
        for i in range(n):
            self.read_byte(raw, i == n - 1)
        self.stop()
        self.flush()
        if not all(acks):
            raise IOError('NACK from 0x%02x (acks %s)' % (ADDR, acks))
        return bytes(sum(int(b) << (7 - i) for i, b in enumerate(bits)) for bits in raw)

    def write_reg(self, reg, data):
        acks = []
        self.start()
        for b in (ADDR << 1, reg >> 8, reg & 0xFF) + tuple(data):
            self.write_byte(b, acks)
        self.stop()
        self.flush()
        if not all(acks):
            raise IOError('NACK writing 0x%04x (acks %s)' % (reg, acks))


def first_reg(u):
    m = re.search(r'9c0032ec: ([0-9a-f]{8})', u.cmd('md.l 0x%08x 1' % FIRST))
    return int(m.group(1), 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ip', default=os.environ.get('HU_IP'))
    ap.add_argument('--token', default=os.environ.get('HU_TOKEN'))
    sub = ap.add_subparsers(dest='op', required=True)
    sub.add_parser('id')
    p = sub.add_parser('read'); p.add_argument('reg'); p.add_argument('n')
    p = sub.add_parser('poll'); p.add_argument('--secs', type=float, default=30)
    a = ap.parse_args()
    if not a.token:
        sys.exit('no token: source tools/hu.env')
    a.ip = a.ip or hu.listen(10)[0] or sys.exit('no hello from the unit: pass --ip or set HU_IP')
    u = hu.Unit(a.ip, a.token)
    saved = first_reg(u)
    gpio_bits = (1 << (SDA_PIN - 64)) | (1 << (SCL_PIN - 64))
    # lines released (OE = 0) and OUT = 0 before handing the pads to the GPIO block
    u.cmd('mw.l 0x%08x 0x%08x; mw.l 0x%08x 0x%08x; mw.l 0x%08x 0x%08x'
          % (OE, (SDA_B | SCL_B) << 16, OUT, (SDA_B | SCL_B) << 16, FIRST, saved | gpio_bits))
    bus = Bus(u)
    try:
        if a.op == 'id':
            pid = bus.read_reg(0x8140, 6)
            print('product id %r  firmware 0x%02x%02x' % (pid[:4].rstrip(b'\0').decode(errors='replace'), pid[5], pid[4]))
            cfg = bus.read_reg(0x8047, 7)
            print('config version %d  x_max %d  y_max %d  touch_number %d  module_switch1 0x%02x'
                  % (cfg[0], cfg[1] | cfg[2] << 8, cfg[3] | cfg[4] << 8, cfg[5] & 0x0F, cfg[6]))
            st = bus.read_reg(0x814E, 1)[0]
            print('status 0x814E = 0x%02x (ready %d, key %d, points %d)' % (st, st >> 7, st >> 4 & 1, st & 0x0F))
        elif a.op == 'read':
            print(bus.read_reg(int(a.reg, 0), int(a.n, 0)).hex(' '))
        elif a.op == 'poll':
            t_end = time.time() + a.secs
            while time.time() < t_end:
                st = bus.read_reg(0x814E, 1)[0]
                if st & 0x80:
                    n = st & 0x0F
                    recs = bus.read_reg(0x8150, 8 * n) if 0 < n <= 5 else b''
                    pts = [(r[0], r[1] | r[2] << 8, r[3] | r[4] << 8, r[5] | r[6] << 8)
                           for r in (recs[i:i + 8] for i in range(0, len(recs), 8))]
                    key = ' key-bit' if st & 0x10 else ''
                    print('%.1f status 0x%02x%s %s' % (time.time(), st, key,
                          ' '.join('id%d(%d,%d s%d)' % p for p in pts) or '(released)'), flush=True)
                    bus.write_reg(0x814E, [0])
    finally:
        u.cmd('mw.l 0x%08x 0x%08x; mw.l 0x%08x 0x%08x' % (OE, (SDA_B | SCL_B) << 16, FIRST, saved))


if __name__ == '__main__':
    main()
