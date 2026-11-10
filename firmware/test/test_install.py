#!/usr/bin/env python3
"""Host test of install.c's planning (stage 1 boot path, stage 2 flash copy) against the original flash dump and
variants of it: a fresh unit, a unit patched by tools/patch_uboot.py (the SD-only script), variants that must be refused,
U-Boot builds with the environment shifted, and the copy region in every state. The fresh-unit result is cross-checked
byte for byte against tools/patch_uboot.py's output with the version 1 script swapped for version 2.

  python3 -I test_install.py ./install_host

Needs dump1.bin (the original 16 MB flash, kept out of git) at the repository root; without it the test is skipped."""
import os, random, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'isp'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from mkisp import FLASH_UBOOT_HDR, check_script, check_stock_uboot, mkscript, xboot_dsum, xboot_hsum, be32  # noqa: E402

HOST = os.path.abspath(sys.argv[1])
DUMP = os.path.join(ROOT, 'dump1.bin')
ENV_OLD = b'bootcmd=bootsp\0bootdelay=0\0baudrate=921600\0\0'
ENV_NEW = b'bootcmd=source 98e00000;bootsp\0bootdelay=0\0'.ljust(44, b'\0')
SCRIPT, COPY, COPY_IMG, COPY_END = 0xE00000, 0xC70000, 0xC70100, 0xE00000
COPY_MAX = COPY_END - COPY_IMG
BODY = FLASH_UBOOT_HDR + 64
fails = 0
tmp = tempfile.mkdtemp(prefix='install_test_')


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print('FAIL', what)


def path(name):
    return os.path.join(tmp, name)


def put(name, data):
    open(path(name), 'wb').write(data)
    return path(name)


def run(*args):
    p = subprocess.run([HOST] + [str(a) for a in args], capture_output=True, text=True)
    kv = dict(l.split('=', 1) for l in p.stdout.splitlines() if '=' in l)
    kv['rc'] = p.returncode
    if p.returncode == 1:
        print(p.stdout, p.stderr)
    return kv


def resum(d, size):
    """Recompute the U-Boot uImage size and both xboot sums, as a different build would carry them."""
    struct.pack_into('>I', d, FLASH_UBOOT_HDR + 12, size)
    struct.pack_into('>I', d, FLASH_UBOOT_HDR + 24, xboot_dsum(bytes(d[BODY:BODY + size])))
    struct.pack_into('>I', d, FLASH_UBOOT_HDR + 4, xboot_hsum(bytes(d[FLASH_UBOOT_HDR:BODY])))
    assert check_stock_uboot(bytes(d[FLASH_UBOOT_HDR:BODY + size])) is None
    return d


def changed_blocks(a, b):
    return [o for o in range(0, len(a), 0x10000) if a[o:o + 0x10000] != b[o:o + 0x10000]]


def boot_ok(name, d, blocks, env_off):
    """Stage 1 must plan exactly these blocks; the result must pass xboot, hold the patched env once, the v2 script, and
    plan nothing on a second run."""
    r = run('boot', put(name, d), path(name + '.out'))
    check(r['rc'] == 0 and r['refuse'] == '', '%s: plan refused: %s' % (name, r.get('refuse')))
    check(r.get('blocks') == ','.join('0x%x' % b for b in blocks), '%s: blocks %s, want %s' % (name, r.get('blocks'),
                                                                                              [hex(b) for b in blocks]))
    check(r.get('env_off') == '0x%x' % env_off, '%s: env at %s' % (name, r.get('env_off')))
    if r['rc']:
        return None
    o = open(path(name + '.out'), 'rb').read()
    size = be32(o, FLASH_UBOOT_HDR + 12)
    check(check_stock_uboot(o[FLASH_UBOOT_HDR:BODY + size]) is None, '%s: result fails the xboot check' % name)
    body = o[BODY:BODY + size]
    check(body.count(ENV_NEW) == 1 and body.count(ENV_OLD) == 0 and o[env_off:env_off + 44] == ENV_NEW,
          '%s: patched env not exactly once at 0x%x' % (name, env_off))
    check(check_script(o[SCRIPT:]) is None and o[SCRIPT + 32:SCRIPT + 50] == b'nor boot script v2',
          '%s: script block' % name)
    check(sorted(changed_blocks(d, o)) == sorted(blocks), '%s: other blocks changed' % name)
    r2 = run('boot', path(name + '.out'))
    check(r2['rc'] == 0 and r2.get('blocks') == '' and r2.get('stage1_done') == '1', '%s: second plan not empty' % name)
    return o


def unboot_ok(name, installed, original):
    """Undo stage 1 on `installed`: the result must equal `original` with a blank script block, plan nothing more,
    and installing again must give `installed` back (when original had a blank script block)."""
    r = run('unboot', put(name + '.inst', installed), path(name + '.undone'))
    check(r['rc'] == 0 and r['refuse'] == '', '%s: undo refused: %s' % (name, r.get('refuse')))
    if r['rc']:
        return None
    u = open(path(name + '.undone'), 'rb').read()
    want = bytearray(original)
    want[SCRIPT:SCRIPT + 0x10000] = b'\xff' * 0x10000
    check(u == bytes(want), '%s: undo is not the original with a blank script block (blocks %s)' %
          (name, [hex(b) for b in changed_blocks(u, want)]))
    size = be32(u, FLASH_UBOOT_HDR + 12)
    check(check_stock_uboot(u[FLASH_UBOOT_HDR:BODY + size]) is None, '%s: undo fails the xboot check' % name)
    r2 = run('unboot', path(name + '.undone'))
    check(r2['rc'] == 2 and 'nothing to remove' in r2['refuse'], '%s: second undo not refused: %s' % (name, r2))
    r3 = run('boot', path(name + '.undone'), path(name + '.again'))
    check(r3['rc'] == 0, '%s: install after undo refused' % name)
    if original[SCRIPT:SCRIPT + 0x10000] == b'\xff' * 0x10000 and installed[SCRIPT + 32:SCRIPT + 51] == \
            b'nor boot script v2\0' and r3['rc'] == 0:
        check(open(path(name + '.again'), 'rb').read() == bytes(installed), '%s: undo + install does not round-trip' % name)
    return u


def refused(name, d, want, cmd='boot', *extra):
    r = run(cmd, put(name, d), *extra)
    reason = (r.get('copy_refuse') or r.get('refuse', '')) if cmd == 'copy' else r.get('uncopy_refuse', '') \
        if cmd == 'uncopy' else r.get('refuse', '')
    check(r['rc'] == 2 and want in reason, '%s: want refusal "%s", got rc %d "%s"' % (name, want, r['rc'], reason))


def image(n, build, seed):
    """A payload-like image: jump, HUNE v1 header with a build id at +0x40, random rest."""
    rnd = random.Random(seed)
    img = bytearray(rnd.getrandbits(8) for _ in range(n))
    img[0x40:0x48] = b'HUNE' + struct.pack('<I', 1)
    img[0x48:0x48 + 56] = build.encode().ljust(56, b'\0')
    return bytes(img)


def script_checks():
    text = subprocess.run([HOST, 'script'], capture_output=True, text=True).stdout
    subprocess.run([HOST, 'scriptimg', path('s.img')], check=True)
    img = open(path('s.img'), 'rb').read()
    check(img == mkscript(text, 'nor boot script v2', stamp=0), 'C script image != mkisp.mkscript(stamp=0)')
    check(check_script(img) is None, 'script image fails U-Boot image_source checks')
    v1 = open(os.path.join(ROOT, 'isp', 'bootcmd', 'boot.txt')).read().splitlines()
    lines = text.splitlines()
    check(lines[:4] == v1[:4] and lines[-1] == v1[-1], 'v2 script changed the SD-only lines 1-4 or the go line')
    card = [l for l in lines if l.startswith('if fatload')]
    check(len(card) == 1 and card[0].replace(' setenv hu_src sd;', '') == v1[-2], 'card loop changed: %s' % card)
    for want in ('cmp.l 0x98c70000 0x33f0000 1', 'crc32 0x98c70100 0x18ff00 0x33f0004', 'cmp.l 0x33f0004 0x98c70010 1',
                 'cp.l 0x98c70100 0x1800000 0x63fc0', 'mw.l 0x33f0000 0x434e5548 1'):
        check(want in text, 'script lacks %r' % want)
    check(COPY_MAX == 0x18ff00 and COPY_MAX // 4 == 0x63fc0, 'region constants')
    return text


def boot_script_says(f):
    """What the version 2 script decides about the copy, done in Python: magic, then CRC of the whole span."""
    return f[COPY:COPY + 4] == b'HUNC' and zlib.crc32(f[COPY_IMG:COPY_END]) == struct.unpack_from('<I', f, COPY + 0x10)[0]


def main():
    if not os.path.exists(DUMP):
        print('test_install: skipped (no dump1.bin at the repository root)')
        return 0
    dump = bytearray(open(DUMP, 'rb').read())
    text = script_checks()
    eo = dump.find(ENV_OLD)

    # fresh unit, and the cross-check against tools/patch_uboot.py
    fresh = boot_ok('fresh', dump, [SCRIPT, 0x920000, 0x8A0000], eo)
    import patch_uboot
    v1txt = os.path.join(ROOT, 'isp', 'bootcmd', 'boot.txt')
    sys.argv = ['patch_uboot.py', DUMP, v1txt, path('pu.bin')]
    import contextlib, io
    with contextlib.redirect_stdout(io.StringIO()):
        patch_uboot.main()
    pu = bytearray(open(path('pu.bin'), 'rb').read())
    swapped = bytearray(pu)
    swapped[SCRIPT:SCRIPT + 0x10000] = mkscript(text, 'nor boot script v2', stamp=0).ljust(0x10000, b'\xff')
    check(fresh is not None and fresh == bytes(swapped), 'fresh plan != patch_uboot.py output with the v2 script')
    if fresh is not None:
        check(fresh[0x920000:0x930000] == pu[0x920000:0x930000] and fresh[0x8A0000:0x8B0000] == pu[0x8A0000:0x8B0000],
              'env block or header sums differ from patch_uboot.py')

    # this unit: patch_uboot.py with the SD-only script installed -> only the script block
    o = boot_ok('patched-v1', pu, [SCRIPT], eo)
    check(o is not None and changed_blocks(pu, o) == [SCRIPT], 'patched-v1 touched more than the script block')

    # interrupted stage 1 script write (page 0 blank, some pages missing) is accepted and finished
    part = bytearray(pu)
    part[SCRIPT:SCRIPT + 0x10000] = fresh[SCRIPT:SCRIPT + 0x10000]
    part[SCRIPT:SCRIPT + 0x100] = b'\xff' * 0x100
    part[SCRIPT + 0x300:SCRIPT + 0x400] = b'\xff' * 0x100
    r = run('boot', put('partial', part))
    check(r['rc'] == 0 and r.get('script') == 'partial' and r.get('blocks') == '0x%x' % SCRIPT, 'partial script: %s' % r)

    # refusals
    d = bytearray(dump); d[eo:eo + 7] = b'bootcmX'
    refused('env-missing', resum(d, be32(d, FLASH_UBOOT_HDR + 12)), 'not found')
    d = bytearray(dump); d[eo + 0x200:eo + 0x200 + 44] = ENV_OLD
    refused('env-twice', resum(d, be32(d, FLASH_UBOOT_HDR + 12)), 'more than once')
    d = bytearray(dump); d[eo + 0x200:eo + 0x200 + 44] = ENV_NEW
    refused('env-stock-and-patched', resum(d, be32(d, FLASH_UBOOT_HDR + 12)), 'more than once')
    d = bytearray(dump); d[SCRIPT + 0x1000:SCRIPT + 0x1010] = os.urandom(16)
    refused('script-foreign', d, 'not ours')
    d = bytearray(dump); d[SCRIPT:SCRIPT + 0x10000] = mkscript('echo hi\n', 'isp stage1').ljust(0x10000, b'\xff')
    refused('script-other-name', d, 'not ours')
    d = bytearray(pu); d[SCRIPT + 0x2000] = 0x00
    refused('script-v1-plus-junk', d, 'not ours')
    d = bytearray(dump); d[SCRIPT:SCRIPT + 0x10000] = mkscript(text, 'nor boot script v3').ljust(0x10000, b'\xff')
    refused('script-newer', d, 'newer boot script')
    d = bytearray(dump); d[BODY + 0x1000] ^= 0x40
    refused('uimage-body-sum', d, 'body word sum')
    d = bytearray(dump); d[FLASH_UBOOT_HDR + 8] ^= 1
    refused('uimage-header-sum', d, 'header word sum')
    d = bytearray(dump); d[FLASH_UBOOT_HDR + 0x1E] = 0
    refused('uimage-no-flag', d, 'verify flag')
    d = bytearray(dump); d[FLASH_UBOOT_HDR] = 0
    refused('uimage-missing', d, 'no U-Boot uImage')

    # other U-Boot builds: the environment shifted (into the next block, across a block boundary, into the header block)
    size = be32(dump, FLASH_UBOOT_HDR + 12)
    body = bytes(dump[BODY:BODY + size])
    rnd = random.Random(1)
    for name, delta, blocks in (('shift-next-block', 0x10123, [SCRIPT, 0x930000, 0x8A0000]),
                                ('shift-straddle', 0x92FFF0 - eo, [SCRIPT, 0x920000, 0x930000, 0x8A0000]),
                                ('shift-header-block', -0x86000, [SCRIPT, 0x8A0000])):
        i = eo - BODY
        nb = (body[:i] + bytes(rnd.getrandbits(8) for _ in range(delta)) + body[i:]) if delta > 0 else \
             (body[:i + delta] + body[i:])
        d = bytearray(dump)
        d[BODY:BODY + len(nb)] = nb
        d = resum(d, len(nb))
        o = boot_ok(name, d, blocks, eo + delta)
        if o is not None:
            unboot_ok(name + '-undo', o, d)

    # stage 1 undo: the fresh install and this unit's state (patch_uboot.py, version 1 script) both go back to dump1
    unboot_ok('undo-fresh', fresh, dump)
    u = unboot_ok('undo-v1', pu, dump)
    check(u is not None and u[0x920000:0x930000] == dump[0x920000:0x930000] and
          u[0x8A0000:0x8B0000] == dump[0x8A0000:0x8B0000] and u == bytes(dump), 'undo-v1: not dump1 byte for byte')
    r = run('unboot', path('undo-v1.inst'))
    check(r.get('blocks') == '0x%x,0x920000,0x8a0000' % SCRIPT, 'undo blocks: %s' % r.get('blocks'))
    refused('undo-stock', dump, 'nothing to remove', 'unboot')
    d = bytearray(pu); d[SCRIPT:SCRIPT + 0x10000] = b'\xff' * 0x10000
    refused('undo-no-script', d, 'does not hold our script', 'unboot')
    d = bytearray(pu); d[SCRIPT + 0x1000] = 0x12
    refused('undo-foreign-script', d, 'does not hold our script', 'unboot')
    d = bytearray(pu); d[SCRIPT:SCRIPT + 0x10000] = mkscript('echo hi\n', 'isp stage1').ljust(0x10000, b'\xff')
    refused('undo-other-script', d, 'does not hold our script', 'unboot')
    d = bytearray(pu); d[SCRIPT:SCRIPT + 0x10000] = mkscript(text, 'nor boot script v3').ljust(0x10000, b'\xff')
    refused('undo-newer-script', d, 'does not hold our script', 'unboot')
    d = bytearray(pu); d[BODY + 0x1000] ^= 0x40
    refused('undo-body-sum', d, 'body word sum', 'unboot')
    d = bytearray(pu); d[FLASH_UBOOT_HDR + 8] ^= 1
    refused('undo-header-sum', d, 'header word sum', 'unboot')
    d = bytearray(pu); d[eo + 0x200:eo + 0x200 + 44] = ENV_NEW
    refused('undo-env-twice', resum(d, be32(d, FLASH_UBOOT_HDR + 12)), 'more than once', 'unboot')
    d = bytearray(pu); d[eo + 0x200:eo + 0x200 + 44] = ENV_OLD
    refused('undo-env-both-forms', resum(d, be32(d, FLASH_UBOOT_HDR + 12)), 'more than once', 'unboot')
    d = bytearray(pu); d[eo + 8:eo + 14] = b'SOURCE'
    refused('undo-env-other', resum(d, be32(d, FLASH_UBOOT_HDR + 12)), 'not found', 'unboot')

    # stage 2
    img = image(0x11EE80, '20261109-120000', 1)
    big = image(COPY_MAX + 1, 'too-big', 2)
    refused('copy-fresh-unit', dump, 'stage 1 is not installed', 'copy', put('img.bin', img))
    refused('copy-v1-script', pu, 'stage 1 is not installed', 'copy', path('img.bin'))
    r = run('copy', put('s1.bin', fresh), path('img.bin'), path('c1.bin'))
    check(r['rc'] == 0 and r.get('seq') == '1' and r.get('erase_mask') == '0x0', 'copy on blank region: %s' % r)
    c1 = open(path('c1.bin'), 'rb').read()
    h = c1[COPY:COPY + 0x100]
    span = zlib.crc32(img + b'\xff' * (COPY_MAX - len(img)))
    check(h[:4] == b'HUNC' and struct.unpack_from('<6I', h, 4) == (1, len(img), zlib.crc32(img), span, 1, 0x100),
          'copy header fields')
    check(struct.unpack_from('<I', h, 0xFC)[0] == zlib.crc32(h[4:0xFC]), 'copy header CRC')
    check(h[0x20:0x2F] == b'20261109-120000', 'copy header build id')
    check(boot_script_says(c1), 'the boot script would not take the copy')
    check(changed_blocks(fresh, c1) == list(range(COPY, COPY + 0x120000, 0x10000)), 'copy touched other blocks')
    r = run('region', path('c1.bin'))
    check(r.get('region') == 'copy' and r.get('old_ok') == '1' and r.get('old_seq') == '1', 'region of c1: %s' % r)
    # reinstall over our own copy: seq 2, erases the blocks that hold data
    img2 = image(0x100000, '20261110-080000', 3)
    r = run('copy', path('c1.bin'), put('img2.bin', img2), path('c2.bin'))
    check(r['rc'] == 0 and r.get('seq') == '2' and r.get('erase_mask') == '0x%x' % ((1 << 18) - 1), 'reinstall: %s' % r)
    c2 = open(path('c2.bin'), 'rb').read()
    check(boot_script_says(c2) and c2[COPY_IMG + len(img2):COPY_END] == b'\xff' * (COPY_END - COPY_IMG - len(img2)),
          'reinstall result')
    # interrupted install (header without magic): ours, not bootable, may be reinstalled
    r = run('copy', path('s1.bin'), path('img.bin'), path('pend.bin'), 'pending')
    pend = open(path('pend.bin'), 'rb').read()
    check(not boot_script_says(pend), 'the boot script would take a copy without its magic')
    r = run('region', path('pend.bin'))
    check(r.get('region') == 'pending' and r.get('old_ok') == '0', 'pending region: %s' % r)
    r = run('copy', path('pend.bin'), path('img2.bin'))
    check(r['rc'] == 0 and r.get('seq') == '2', 'install over a pending copy: %s' % r)
    # interrupted erase: header block still there, a later block already blank -> ours, does not boot
    d = bytearray(c1); d[COPY + 0x50000:COPY + 0x60000] = b'\xff' * 0x10000
    r = run('region', put('half-erased.bin', d))
    check(r.get('region') == 'copy' and r.get('old_ok') == '0' and not boot_script_says(d), 'half erased: %s' % r)
    # refusals
    d = bytearray(fresh); d[0xD00000] = 0x12
    refused('copy-foreign-data', d, 'not ours', 'copy', path('img.bin'))
    d = bytearray(c1); d[COPY_IMG + len(img) + 0x10] = 0
    refused('copy-junk-past-length', d, 'not ours', 'copy', path('img2.bin'))
    d = bytearray(c1); d[COPY + 0x30] ^= 1
    refused('copy-header-damaged', d, 'not ours', 'copy', path('img2.bin'))
    refused('copy-too-big', fresh, 'size out of range', 'copy', put('big.bin', big))
    r = run('copy', path('s1.bin'), put('max.bin', image(COPY_MAX, 'max', 4)), path('cmax.bin'))
    check(r['rc'] == 0 and boot_script_says(open(path('cmax.bin'), 'rb').read()), 'largest image: %s' % r)
    noh = bytearray(img); noh[0x40:0x44] = b'XXXX'
    refused('copy-no-hune', fresh, 'HUNE', 'copy', put('noh.bin', noh))

    # stage 2 undo: zero the magic, erase the blocks with data; only our own copy
    for name, src in (('uncopy-c1', 'c1.bin'), ('uncopy-c2', 'c2.bin'), ('uncopy-pending', 'pend.bin'),
                      ('uncopy-max', 'cmax.bin')):
        f = open(path(src), 'rb').read()
        r = run('uncopy', path(src), path(name + '.out'))
        check(r['rc'] == 0, '%s: refused: %s' % (name, r.get('uncopy_refuse')))
        o = open(path(name + '.out'), 'rb').read()
        check(o[COPY:COPY_END] == b'\xff' * (COPY_END - COPY) and o[:COPY] == f[:COPY] and o[COPY_END:] == f[COPY_END:],
              '%s: region not blank or other bytes changed' % name)
        check(run('region', path(name + '.out')).get('region') == 'blank', '%s: region not blank' % name)
    d = bytearray(c1); d[COPY:COPY + 4] = b'\0\0\0\0'   # interrupted removal: magic zeroed, nothing erased yet
    r = run('region', put('half-removed.bin', d))
    check(r.get('region') == 'pending' and r.get('old_ok') == '0' and not boot_script_says(d), 'half removed: %s' % r)
    check(run('uncopy', path('half-removed.bin'))['rc'] == 0, 'uncopy of a half-removed copy refused')
    d[COPY + 0x50000:COPY + 0x60000] = b'\xff' * 0x10000   # ... and one block erased
    check(run('uncopy', put('half-removed2.bin', d))['rc'] == 0, 'uncopy of a half-erased copy refused')
    check(run('copy', path('half-removed.bin'), path('img2.bin'))['rc'] == 0, 'install over a half-removed copy refused')
    refused('uncopy-blank', fresh, 'nothing to remove', 'uncopy')
    d = bytearray(fresh); d[0xD00000] = 0x12
    refused('uncopy-foreign', d, 'not ours', 'uncopy')
    d = bytearray(c1); d[COPY_IMG + len(img) + 0x10] = 0
    refused('uncopy-junk-past-length', d, 'not ours', 'uncopy')

    print('test_install: %s (%d failures)' % ('FAILED' if fails else 'ok', fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
