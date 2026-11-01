#!/usr/bin/env python3
"""Load and unload the PROBE SD card for head-unit rounds, the way the unit's loaders need it.

  card.py load <round-dir> [--keep WIFI.TXT ...]   replace the card's files with <round-dir>'s card files
  card.py save <dest-dir>                           copy every file except the round's binaries off the card
  card.py wifi                                      (re)write WIFI.TXT, prompting for the meow password
  card.py check                                     show the card's files, sizes and fragmentation

What `load` guarantees, from what went wrong before:
  * the card is reformatted (FAT32 via udisks, no root needed) every time: U-Boot's `fatwrite` overwriting the same
    file names on every reboot cycle corrupted the FAT (Linux: "request beyond EOF", remounted read-only), and
    U-Boot 2014.04 could not find a file whose entry sat behind many leftover directory entries;
  * every file is written by Linux, one at a time onto the empty filesystem, so each is one contiguous run: xboot's
    minimal FAT reader fell back to the NOR copy of U-Boot after U-Boot's own `fatwrite` had rewritten ISP_UPDT.BIN;
  * each file on the card is byte-identical to its source (sha256), and --keep files are put back unchanged and
    are never printed;
  * the card is only unmounted at the end: `udisksctl power-off` cuts the reader's USB port until it is replugged.

Card files in a round directory: ISP_UPDT.BIN and ISPBOOOT.BIN first (xboot and U-Boot read them first), then any
other *.BIN. WIFI.TXT is kept by default when present. A round without ISP files (just BOOT.BIN) is for the patched
NOR U-Boot, whose bootcmd runs /BOOT.BIN at power-on.
"""
import argparse, getpass, glob, hashlib, os, shutil, subprocess, sys, tempfile

MOUNT = os.environ.get('CARD_MOUNT') or '/run/media/%s/PROBE' % os.environ.get('USER', 'evan')
FIRST = ('ISP_UPDT.BIN', 'ISPBOOOT.BIN')
TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'WIFI.TXT.template')


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def card():
    if not os.path.ismount(MOUNT):
        sys.exit('%s is not mounted (if the reader was powered off, unplug and replug it)' % MOUNT)
    return MOUNT


def device():
    for line in open('/proc/mounts'):
        dev, mnt = line.split()[:2]
        if mnt == MOUNT:
            return dev
    return None


def unmount():
    os.sync()
    dev = device()
    if dev:
        subprocess.run(['udisksctl', 'unmount', '-b', dev], check=True)


def reformat():
    """Unmount, mkfs FAT32 label PROBE through udisks (polkit allows it for removable media), mount again."""
    dev = device()
    subprocess.run(['udisksctl', 'unmount', '-b', dev], check=True, capture_output=True)
    obj = '/org/freedesktop/UDisks2/block_devices/' + os.path.basename(dev)
    subprocess.run(['gdbus', 'call', '--system', '--dest', 'org.freedesktop.UDisks2', '--object-path', obj,
                    '--method', 'org.freedesktop.UDisks2.Block.Format', 'vfat',
                    "{'label': <'PROBE'>, 'update-partition-type': <false>}"], check=True, capture_output=True)
    fsver = subprocess.run(['lsblk', '-no', 'FSVER', dev], capture_output=True, text=True).stdout.strip()
    if fsver != 'FAT32':
        sys.exit('format produced %r, xboot needs FAT32' % fsver)
    subprocess.run(['udisksctl', 'mount', '-b', dev], check=True, capture_output=True)
    if not os.path.ismount(MOUNT):
        sys.exit('card did not remount at %s' % MOUNT)


def check(root):
    st = os.statvfs(root)
    print('  FAT32, %d-byte clusters, %d MB free' % (st.f_bsize, st.f_bavail * st.f_bsize >> 20))
    for name in sorted(os.listdir(root)):
        print('  %-14s %9d' % (name, os.path.getsize(os.path.join(root, name))))
    return True


def load(a):
    root = card()
    src = os.path.abspath(a.dir)
    names = [n for n in FIRST if os.path.exists(os.path.join(src, n))]
    names += sorted(os.path.basename(p) for p in glob.glob(os.path.join(src, '*.BIN')) if os.path.basename(p) not in names)
    if not names:
        sys.exit('%s has no *.BIN card files' % src)
    if names[0] != 'ISP_UPDT.BIN':
        print('note: no ISP_UPDT.BIN -- a boot-from-card round (NOR bootcmd runs /BOOT.BIN), not an update round')
    keep = [k for k in a.keep if os.path.exists(os.path.join(root, k))]
    with tempfile.TemporaryDirectory(dir=os.environ.get('XDG_RUNTIME_DIR')) as tmp:
        saved = {}
        for k in keep:
            shutil.copyfile(os.path.join(root, k), os.path.join(tmp, k))
            saved[k] = sha(os.path.join(tmp, k))
        reformat()
        for n in names:   # one file at a time, synced, so each gets a fresh contiguous run
            shutil.copyfile(os.path.join(src, n), os.path.join(root, n))
            os.sync()
        for k in keep:
            shutil.copyfile(os.path.join(tmp, k), os.path.join(root, k))
            os.sync()
    bad = [n for n in names if sha(os.path.join(root, n)) != sha(os.path.join(src, n))]
    bad += [k for k in keep if sha(os.path.join(root, k)) != saved[k]]
    print('card %s (freshly formatted):' % root)
    check(root)
    if bad:
        sys.exit('MISMATCH after copy: %s (card left mounted)' % ' '.join(bad))
    if 'WIFI.TXT' in a.keep and 'WIFI.TXT' not in keep:
        print('note: no WIFI.TXT on the card; run `card.py wifi` if this round needs Wi-Fi')
    print('verified: %s%s' % (' '.join(names), (' + kept ' + ' '.join(keep)) if keep else ''))
    if not a.no_unmount:
        unmount()
        print('unmounted; the card can go in the unit')


def save(a):
    root = card()
    os.makedirs(a.dest, exist_ok=True)
    got = []
    for n in sorted(os.listdir(root)):
        if n.endswith('.BIN') and n.startswith(('ISP', 'NET', 'WIFI', 'CAP', 'BOOT')) or n == 'WIFI.TXT':
            continue
        shutil.copyfile(os.path.join(root, n), os.path.join(a.dest, n))
        got.append(n)
    print('saved %d files to %s: %s' % (len(got), a.dest, ' '.join(got)))


def wifi(a):
    root = card()
    lines = open(TEMPLATE).read().splitlines()
    pw = getpass.getpass('meow password: ')
    if not 8 <= len(pw) <= 63:
        sys.exit('WPA2 passphrases are 8-63 characters')
    out = '\n'.join(('psk=' + pw) if l.startswith('psk=') else l for l in lines) + '\n'
    with open(os.path.join(root, 'WIFI.TXT'), 'w') as f:
        f.write(out)
    os.sync()
    print('WIFI.TXT written (token from %s)' % os.path.basename(TEMPLATE))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='op', required=True)
    p = sub.add_parser('load'); p.add_argument('dir'); p.add_argument('--keep', nargs='*', default=['WIFI.TXT'])
    p.add_argument('--no-unmount', action='store_true')
    p = sub.add_parser('save'); p.add_argument('dest')
    sub.add_parser('wifi')
    sub.add_parser('check')
    a = ap.parse_args()
    if a.op == 'check':
        print('card %s:' % card())
        return 0 if check(card()) else 1
    {'load': load, 'save': save, 'wifi': wifi}[a.op](a)
    return 0


if __name__ == '__main__':
    sys.exit(main())
