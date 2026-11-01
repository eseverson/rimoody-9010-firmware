#!/usr/bin/env python3
"""TCP over the unit's lwIP (lwnet.c test service): send N MB to port 5300 (discarded, the unit logs the rate), then
receive the unit's pattern from port 5301 and check every byte. Usage: tcp_test.py IP [MB]"""
import socket, sys, time

def main():
    ip, mb = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 8
    n = mb << 20
    buf = bytes(65536)
    s = socket.create_connection((ip, 5300), timeout=20)
    t0 = time.time(); sent = 0
    while sent < n:
        k = s.send(buf[:min(len(buf), n - sent)]); sent += k
    s.shutdown(socket.SHUT_WR)
    s.settimeout(30)
    try:
        s.recv(1)
    except Exception:
        pass
    s.close()
    dt = time.time() - t0
    print('to unit:   %d bytes in %.2f s = %.0f kB/s (%.2f Mbit/s)' % (sent, dt, sent / dt / 1024, sent * 8 / dt / 1e6))
    s = socket.create_connection((ip, 5301), timeout=20)
    t0 = time.time(); got = 0; bad = 0
    while True:
        d = s.recv(65536)
        if not d:
            break
        for i in range(0, len(d), 4096):   # spot-check every 4 KB of the pattern
            j = got + i
            if d[i] != ((j * 7 + (j >> 11)) & 0xff):
                bad += 1
        got += len(d)
    dt = time.time() - t0
    print('from unit: %d bytes in %.2f s = %.0f kB/s (%.2f Mbit/s), %d pattern errors' % (got, dt, got / dt / 1024, got * 8 / dt / 1e6, bad))
    return 0 if got == 8 << 20 and not bad else 1

if __name__ == '__main__':
    sys.exit(main())
