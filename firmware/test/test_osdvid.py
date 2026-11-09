"""osdvid.c's de-tiler against ffmpeg: each NV12 test picture is tiled into the decoder's layout (tools/mfd_detile.py,
checked bit-exact against the hardware in both scan types), converted by osdvid_host, and compared with the same window
of the NV12 picture as YUY2 (Y0 U Y1 V, each line taking chroma line y / 2). 800x480 shows whole; 1280x720 shows its 1024x600 center (osdvid_frame)."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import mfd_detile

HOST = sys.argv[1]
P = mfd_detile.P

def tile(plane, w, h):
    out = bytearray(((h + 63) // 64) * 64 * P)
    for y in range(h):
        for x in range(w):
            out[mfd_detile.off(x, y)] = plane[y * w + x]
    return bytes(out)

def testsrc(w, h):   # one NV12 picture of ffmpeg's testsrc2
    return subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=%dx%d:rate=1' % (w, h),
                           '-frames:v', '1', '-pix_fmt', 'nv12', '-f', 'rawvideo', '-'], capture_output=True, check=True).stdout

fail = 0
with tempfile.TemporaryDirectory() as d:
    for w, h in ((800, 480), (1280, 720)):
        if w >= 1024 and h >= 600:
            W, H, x0, y0 = 1024, 600, ((w - 1024) // 2) & ~15, ((h - 600) // 2) & ~1
        else:
            W, H, x0, y0 = w & ~15, h & ~1, 0, 0
        nv12 = testsrc(w, h)
        ref = bytearray(W * H * 2)
        for y in range(H):
            ref[y * W * 2:(y + 1) * W * 2:2] = nv12[(y0 + y) * w + x0:][:W]
            ref[y * W * 2 + 1:(y + 1) * W * 2:2] = nv12[w * h + ((y0 + y) // 2) * w + x0:][:W]   # U V U V ...
        for scan in (0, 1):
            mfd_detile.SCAN = scan
            open(d + '/l', 'wb').write(tile(nv12[:w * h], w, h))
            open(d + '/c', 'wb').write(tile(nv12[w * h:], w, h // 2))
            subprocess.run([HOST, d + '/l', d + '/c', str(x0), str(y0), str(W), str(H), str(scan), d + '/o'], check=True)
            out = open(d + '/o', 'rb').read()
            bad = [i for i in range(len(out)) if out[i] != ref[i]]
            print('%dx%d scan %d: %s' % (w, h, scan, 'ok' if not bad else '%d of %d bytes differ (first at %d)' % (len(bad), len(out), bad[0])))
            fail |= bool(bad)
sys.exit(fail)
