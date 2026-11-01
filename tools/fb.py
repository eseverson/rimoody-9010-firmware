#!/usr/bin/env python3
"""Drive the head unit's OSD0 layer (1024x600 ARGB8888 framebuffer in DRAM) through tools/hu.py.

See notes/display_touch.md for the register map and evidence. Only register writes that stock U-Boot itself makes are
used: OSD0 link_addr (0x9c006208) and OSD0 en (0x9c006204), as at U-Boot link address 0xf216ee..0xf216fa.

  fb.py status                     OSD0 ctrl/en/link/size/crc + TGEN/DMIX words
  fb.py pattern                    upload the color-bar test frame, then enable OSD0
  fb.py fill RRGGBB                upload a solid frame, then enable OSD0
  fb.py raw FILE                   upload FILE (1024*600*4 bytes, ARGB8888 as little-endian words), enable OSD0
  fb.py off                        OSD0 en = 0 (link_addr left alone)

Run `source tools/hu.env` first (token). --ip as for hu.py.
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hu  # noqa: E402

W, H = 1024, 600
FB_ADDR = 0x03000000      # U-Boot's own logo buffer for format 0xE (0xf20e3c); free RAM in our payload's map
HDR_ADDR = 0x032F0000     # 128-byte OSD header, 128-aligned, just past the 0x258000-byte frame
OSD0 = 0x9C006200
FMT_ARGB8888 = 0x0E


def header(fb_addr, w=W, h=H, fmt=FMT_ARGB8888):
    """OSD region header as U-Boot sub_f1fe84 / eCos update_hardware_header (0x523e48) build it (big-endian)."""
    b = bytearray(128)
    b[0] = fmt | (0x80 if fmt == 2 else 0)      # bit 7 = CLUT for 8bpp
    b[2] = 0x10                                  # U-Boot sets 0x10, eCos 0x00
    struct.pack_into('>HH', b, 4, h, (w + 3) & ~3)   # v_size, h_size
    struct.pack_into('>HH', b, 8, 0, 0)              # disp_start_row, disp_start_col
    b[0x15] = 7
    struct.pack_into('>H', b, 0x16, w)               # data width (stride, pixels)
    struct.pack_into('>I', b, 0x18, 0xFFFFFFE0)      # link_next: end of list
    struct.pack_into('>I', b, 0x1C, fb_addr)         # data_start
    return bytes(b)


def px(b0, b1, b2, b3):
    return bytes((b0, b1, b2, b3))


def test_pattern():
    """Top half: bars whose bytes 0 and 3 are both 0xFF, so alpha is opaque whichever byte the OSD takes as alpha.
    Bottom half: pure red/green/blue/white as little-endian ARGB words (0xAARRGGBB). 8-px white border."""
    top = [px(0xFF, 0x00, 0x00, 0xFF), px(0xFF, 0xFF, 0x00, 0xFF), px(0xFF, 0x00, 0xFF, 0xFF), px(0xFF, 0x80, 0x80, 0xFF)]
    bot = [struct.pack('<I', v) for v in (0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF)]
    white = px(0xFF, 0xFF, 0xFF, 0xFF)
    bw = W // 4
    rows = []
    for half in (top, bot):
        line = bytearray()
        for x in range(W):
            line += white if x < 8 or x >= W - 8 else half[min(x // bw, 3)]
        rows.append(bytes(line))
    border = white * W
    out = bytearray()
    for y in range(H):
        out += border if y < 8 or y >= H - 8 else rows[0 if y < H // 2 else 1]
    return bytes(out)


def upload_and_enable(u, frame):
    assert len(frame) == W * H * 4
    print('uploading %d bytes to 0x%08x ...' % (len(frame), FB_ADDR), flush=True)
    u.write(FB_ADDR, frame)
    u.write(HDR_ADDR, header(FB_ADDR))
    u.sync(FB_ADDR, len(frame))
    u.sync(HDR_ADDR, 128)
    print(u.cmd('mw.l 0x%08x 0x%08x; mw.l 0x%08x 1' % (OSD0 + 8, HDR_ADDR, OSD0 + 4)))


def status(u):
    regs = [(OSD0 + o, n) for o, n in ((0, 'osd0 ctrl'), (4, 'osd0 en'), (8, 'osd0 link_addr'), (0x20, 'osd0 req_ctrl'),
                                        (0x38, 'osd0 crc_chksum'), (0x44, 'osd0 hvld_width'), (0x4c, 'osd0 vvld_height'),
                                        (0x54, 'osd0 bist_ctrl'))]
    regs += [(0x9C006A80, 'tgen 0x00'), (0x9C006AA0, 'tgen htotal'), (0x9C006AA4, 'tgen hactive'),
             (0x9C006AA8, 'tgen vtotal'), (0x9C006AAC, 'tgen vact_end'), (0x9C006AB0, 'tgen vact_start'),
             (0x9C006C80, 'dmix layer cfg0'), (0x9C006C84, 'dmix layer mode'), (0x9C006C88, 'dmix layer cfg1')]
    out = u.cmd(' '.join('md.l 0x%08x 1;' % a for a, _ in regs))
    vals = {}
    for line in out.splitlines():
        if ':' in line and line[:8].isalnum():
            a, v = line.split(':', 1)
            vals[int(a, 16)] = v.split()[0]
    for a, n in regs:
        print('%-18s 0x%08x = %s' % (n, a, vals.get(a, '?')))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ip', default=os.environ.get('HU_IP', '192.168.1.138'))
    ap.add_argument('--token', default=os.environ.get('HU_TOKEN'))
    sub = ap.add_subparsers(dest='op', required=True)
    sub.add_parser('status'); sub.add_parser('pattern'); sub.add_parser('off')
    p = sub.add_parser('fill'); p.add_argument('rgb')
    p = sub.add_parser('raw'); p.add_argument('file')
    a = ap.parse_args()
    if not a.token:
        sys.exit('no token: source tools/hu.env')
    u = hu.Unit(a.ip, a.token)
    if a.op == 'status':
        status(u)
    elif a.op == 'pattern':
        upload_and_enable(u, test_pattern())
    elif a.op == 'fill':
        v = 0xFF000000 | int(a.rgb, 16)
        upload_and_enable(u, struct.pack('<I', v) * (W * H))
    elif a.op == 'raw':
        upload_and_enable(u, open(a.file, 'rb').read())
    elif a.op == 'off':
        print(u.cmd('mw.l 0x%08x 0' % (OSD0 + 4)))


if __name__ == '__main__':
    main()
