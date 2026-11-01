#!/usr/bin/env python3
"""De-tile a luma (or interleaved CbCr) plane dumped from the Sunplus MFD H.264 decoder and compare with an ffmpeg NV12
reference: mfd_detile.py LUMA.bin REF.nv12 (notes/h264_decoder.md, "Output layout")."""
import sys
W, H, P = 800, 480, 1920
def off(x, y):
    """luma byte offset of pixel (x, y) in the MFD output (64x64 macro-tiles of 8 blocks, 8-byte groups reversed)"""
    stripe, mx = y // 64, x // 64
    xg, par, r = (x // 16) % 4, y & 1, (y % 64) // 2
    o = stripe * P * 64 + mx * 4096 + ((par ^ (xg & 1) ^ (stripe & 1)) * 0x800) + xg * 0x200 + r * 16 + (x % 16)
    return (o & ~7) | (7 - (o & 7))
def detile(raw, w=W, h=H):
    return bytes(raw[off(x, y)] for y in range(h) for x in range(w))
if __name__ == '__main__':
    raw = open(sys.argv[1], 'rb').read(); ref = open(sys.argv[2], 'rb').read()[:W * H]
    out = detile(raw)
    diff = sum(1 for a, b in zip(out, ref) if a != b)
    print('%s: %d of %d luma pixels differ from ffmpeg' % (sys.argv[1].split('/')[-1], diff, W * H))
