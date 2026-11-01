#!/usr/bin/env python3
"""Build and parse SoC<->MCU frames (Jieli AC6965A link, UART1 115200).

Frame: ff aa LEN payload CHK8 CRC16, LEN = len(payload), CHK8 = -(LEN + sum(payload)) & 0xff, CRC16 = MSB-first
poly 0x8432 init 0 over ff..CHK8 (eCos builder 0x521aa8). The SoC sends the CRC big-endian; the MCU sends it
little-endian (eCos parser 0x521932).

  mcuframe.py d1 00          -> ff aa 02 d1 00 2d be ba
  mcuframe.py --mw d1 00     -> the same as U-Boot `mw.l 0x9c000980 ..` commands
"""
import sys


def crc16(data, poly=0x8432):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ poly) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c


def frame(payload):
    body = bytes([0xff, 0xaa, len(payload)]) + bytes(payload)
    body += bytes([(-(len(payload) + sum(payload))) & 0xff])
    c = crc16(body)
    return body + bytes([c >> 8, c & 0xff])


def mw(payload):
    return ';'.join('mw.l 0x9c000980 0x%02x' % b for b in frame(payload))


if __name__ == '__main__':
    args = sys.argv[1:]
    as_mw = args[:1] == ['--mw']
    p = [int(x, 16) for x in args[as_mw:]]
    print(mw(p) if as_mw else frame(p).hex(' '))
