#!/usr/bin/env python3
"""Read an address range from the head unit over Wi-Fi, surviving bus hangs and resets.

Reads CHUNK bytes at a time into <out>.bin (unreadable chunks stay zero) and keeps progress in <out>.json. A read that
gets no reply is taken to have hung the bus: that chunk is recorded as bad and skipped, then the walker waits for the
unit to come back (the MCU resets it ~150 s after U-Boot starts; with the sd-10-net `loop` card it boots straight back
into the network payload) and carries on. Run it again with the same arguments to resume.

  walk.py 0x9c000000 0x8000 ../dumps/rgst_9c000000 [--chunk 128]
"""
import argparse, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hu


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('start'); ap.add_argument('length'); ap.add_argument('out')
    ap.add_argument('--chunk', type=lambda s: int(s, 0), default=128)
    ap.add_argument('--wait', type=float, default=400, help='seconds to wait for the unit after a hang')
    a = ap.parse_args()
    start, length, chunk = int(a.start, 0), int(a.length, 0), a.chunk
    token = os.environ.get('HU_TOKEN') or sys.exit('need $HU_TOKEN (source tools/hu.env)')

    st_path, bin_path = a.out + '.json', a.out + '.bin'
    st = json.load(open(st_path)) if os.path.exists(st_path) else {'next': start, 'bad': [], 'good': 0}
    if not os.path.exists(bin_path):
        open(bin_path, 'wb').write(bytes(length))
    f = open(bin_path, 'r+b')

    ip = os.environ.get('HU_IP')
    while st['next'] < start + length:
        if not ip:
            ip, hello = hu.listen(a.wait)
            if not ip:
                print('unit did not come back within %.0f s; stopping at 0x%x' % (a.wait, st['next']), flush=True)
                return 1
            print('back: %s' % hello, flush=True)
        u = hu.Unit(ip, token, timeout=1.5, retries=2)
        try:
            while st['next'] < start + length:
                addr = st['next']
                data = u.read(addr, min(chunk, start + length - addr))
                f.seek(addr - start); f.write(data); f.flush()
                st['next'] = addr + len(data); st['good'] += len(data)
                json.dump(st, open(st_path, 'w'))
        except TimeoutError:
            st['bad'].append(st['next'])
            print('hang at 0x%x (chunk 0x%x), skipping' % (st['next'], chunk), flush=True)
            st['next'] += chunk
            json.dump(st, open(st_path, 'w'))
            ip = None   # wait for the next hello
            time.sleep(20)   # let any stale hello broadcasts from the hung boot pass
    print('done: %d bytes read, bad chunks: %s' % (st['good'], ' '.join('0x%x' % b for b in st['bad'])), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
