#!/usr/bin/env python3
"""Stream the MCU frames the payload decodes, live, one line per new frame (polls OP_MCU_STATE).

  mcu_watch.py [--all]     default hides our own heartbeats (tx d1 00)
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hu


def main():
    show_all = '--all' in sys.argv
    u = hu.Unit(os.environ.get('HU_IP') or hu.listen(10)[0], os.environ['HU_TOKEN'], timeout=1.5, retries=2)
    seen = set()
    first = True
    while True:
        try:
            text = u.call(hu.OP_MCU_STATE).decode(errors='replace')
        except TimeoutError:
            print('%s unit not answering' % time.strftime('%T'), flush=True)
            time.sleep(2)
            continue
        for line in text.splitlines():
            parts = line.split()
            if len(parts) < 3 or not parts[0].isdigit() or parts[1] not in ('rx', 'tx'):
                continue
            if line in seen:
                continue
            seen.add(line)
            if first or (not show_all and parts[1] == 'tx' and parts[2:4] == ['d1', '00']):
                continue
            print('%s %s' % (time.strftime('%T'), line), flush=True)
        first = False
        time.sleep(0.3)


if __name__ == '__main__':
    main()
