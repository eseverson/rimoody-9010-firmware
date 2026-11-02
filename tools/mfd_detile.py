#!/usr/bin/env python3
"""De-tile planes dumped from the Sunplus MFD H.264 decoder and compare with an ffmpeg NV12 reference
(notes/h264_decoder.md, "Output layout").

  mfd_detile.py LUMA.bin REF.nv12 [--chroma CHROMA.bin] [--frame N]

REF.nv12 holds one or more NV12 frames (800x480 unless --size WxH) (ffmpeg -f rawvideo -pix_fmt nv12); --frame picks which one."""
import argparse
W, H, P = 800, 480, 1920
def off(x, y):
    """byte offset of (x, y) in the MFD output (64x64 macro-tiles of 8 blocks, 8-byte groups reversed); the CbCr plane
    uses the same layout with x in bytes (Cb, Cr interleaved) and half the rows"""
    stripe, mx = y // 64, x // 64
    xg, par, r = (x // 16) % 4, y & 1, (y % 64) // 2
    o = stripe * P * 64 + mx * 4096 + ((par ^ (xg & 1) ^ (stripe & 1)) * 0x800) + xg * 0x200 + r * 16 + (x % 16)
    return (o & ~7) | (7 - (o & 7))
def detile(raw, w=W, h=H):
    return bytes(raw[off(x, y)] for y in range(h) for x in range(w))
def compare(name, out, ref, w):
    bad = [i for i, (a, b) in enumerate(zip(out, ref)) if a != b]
    first = ' (first at x=%d y=%d: %d vs %d)' % (bad[0] % w, bad[0] // w, out[bad[0]], ref[bad[0]]) if bad else ''
    print('%s: %d of %d differ from ffmpeg%s' % (name, len(bad), len(ref), first))
    return not bad
if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('luma'); ap.add_argument('ref'); ap.add_argument('--chroma'); ap.add_argument('--frame', type=int, default=0)
    ap.add_argument('--size', default='%dx%d' % (W, H))
    a = ap.parse_args()
    W, H = map(int, a.size.split('x'))
    fsz = W * H * 3 // 2
    ref = open(a.ref, 'rb').read()[a.frame * fsz:(a.frame + 1) * fsz]
    ok = compare('luma', detile(open(a.luma, 'rb').read(), W, H), ref[:W * H], W)
    if a.chroma:
        ok &= compare('chroma', detile(open(a.chroma, 'rb').read(), W, H // 2), ref[W * H:], W)
    raise SystemExit(0 if ok else 1)
