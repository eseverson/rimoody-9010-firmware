#!/usr/bin/env python3
"""Read-only check of a microSD card (or image) for the SPHE8268K SD-ISP ("QART") path.

Usage:  sudo ./sd_check.py /dev/sdX          (whole card, not /dev/sdX1)
        ./sd_check.py card.img

Three readers look at the card, each emulated here:
  1. eCos   needs ISP_UPDT.BIN in the root, non-empty, <= 12 MiB (f_00557b68 scan, f_00475b78, f_00475c66).
  2. xboot  SD ISP (mode 0x43). Its own minimal FAT32 code (fat/fat_boot.c, fat_init 0x9e83225c, fat_read 0x9e831fe0,
            loader 0x9e830cc0) looks for the raw 8.3 name "ISP_UPDTBIN" (name copied from flash 0xC400 because the
            QART path sets the flag at 0x9e839800), loads the U-Boot uImage header at file offset 0x10000 and the
            body at 0x10040, and checks magic / word-sum hcrc / size / word-sum dcrc.
  3. U-Boot "ispsp all mmc 0": U-Boot's FAT driver, partition 1 if LBA 0 is an MBR (else whole device), reads
            0x800 bytes of /ISPBOOOT.BIN at file offset 0x100000 and sources the script uImage at +0x20.
Only reads; never writes.
"""
import os, struct, sys, zlib

SECTOR = 512          # SD block length is forced to 512 (CMD16 at 0x9e8334bc); xboot does all math in 512 B sectors
ECOS_MAX = 0xC00000


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def be32(b, o): return struct.unpack_from('>I', b, o)[0]


class Fail(Exception):
    pass


class Dev:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, os.SEEK_END)
        self.size = self.f.tell()
        self.reads = 0

    def read(self, lba, n=1):
        self.reads += 1
        self.f.seek(lba * SECTOR)
        d = self.f.read(n * SECTOR)
        if len(d) != n * SECTOR:
            raise Fail('read past end of device at LBA %d' % lba)
        return d


# ---------------------------------------------------------------------------------------------- xboot emulation
def xboot_fat_init(dev, name11):
    b = dev.read(0)
    part = 0
    # 0x9e832380..0x9e8323e2: 55 AA, (entry1.boot & 0x7f) == 0, entry1.type != 0  -> MBR, use entry 1 only
    if b[0x1fe] == 0x55 and b[0x1ff] == 0xAA and (b[0x1be] & 0x7f) == 0 and b[0x1c2] != 0:
        part = u32(b, 0x1c6)
        print('  MBR: partition entry 1 type=%#04x boot=%#04x start LBA %d ("part@")' % (b[0x1c2], b[0x1be], part))
        b = dev.read(part)
    else:
        print('  no MBR partition 1 -> LBA 0 treated as the FAT32 boot sector ("no mbr")')
    if b[0x52:0x5a] != b'FAT32   ':                              # 0x9e832398..0x9e832436
        raise Fail('"Not fat32": bytes 0x52..0x59 of LBA %d are %r (xboot only reads FAT32)' % (part, b[0x52:0x5a]))
    bps, spc, rsv, nfat = u16(b, 0x0b), b[0x0d], u16(b, 0x0e), b[0x10]
    fatsz, rootclus = u32(b, 0x24), u32(b, 0x2c)
    print('  BPB: bps=%d spc=%d (cluster %d B) rsv=%d nfat=%d fatsz=%d rootclus=%d'
          % (bps, spc, bps * spc, rsv, nfat, fatsz, rootclus))
    if bps != SECTOR:
        raise Fail('bytes/sector %d != 512 (SD reads are 512-byte blocks; xboot sector buffer is 512 B)' % bps)
    if spc == 0 or spc & (spc - 1):
        raise Fail('sectors/cluster %d is not a power of two' % spc)
    if rootclus != 2:
        print('  WARN: RootClus=%d != 2: xboot computes cluster LBA as data_start+(clus-RootClus)*spc, '
              'so every cluster address is off by %d clusters' % (rootclus, rootclus - 2))
    l2 = bps.bit_length() - 1
    fat_start = part + rsv
    data0 = fat_start + nfat * fatsz   # xboot: first sector of cluster RootClus (correct only when RootClus == 2)

    def clus_lba(c): return data0 + (c - rootclus) * spc

    def next_clus(c):
        fs = dev.read(fat_start + (c >> (l2 - 2)))
        return u32(fs, 4 * (c & (bps // 4 - 1)))   # no 0x0FFFFFFF mask, like xboot

    c = rootclus
    hits = []
    seen = 0
    while not (c > 0x0ffffff7 or c >= 0x80000000):
        seen += 1
        if seen > 65536:
            raise Fail('root directory chain does not terminate')
        for s in range(spc):
            sec = dev.read(clus_lba(c) + s)
            for o in range(0, bps, 32):
                if sec[o:o + 11] == name11:          # exact raw 8.3 compare, no attribute or end-marker check
                    hits.append((u16(sec, o + 0x14) << 16 | u16(sec, o + 0x1a), u32(sec, o + 0x1c), sec[o + 11]))
                    break
            if hits:
                break
        if hits:
            break
        c = next_clus(c)
    if not hits:
        raise Fail('"no file": %r not found in the root directory (raw 8.3 compare)' % name11)
    fc, size, attr = hits[0]
    print('  found %r: first cluster %d, size %d (%#x), attr %#04x' % (name11, fc, size, size, attr))
    return dict(bps=bps, spc=spc, first=fc, size=size), clus_lba, next_clus


def xboot_fat_read(dev, ctx, clus_lba, next_clus, off, length):
    bps, spc = ctx['bps'], ctx['spc']
    c = ctx['first']
    s = off // bps
    while s >= spc:
        if c > 0x0ffffff7:
            raise Fail('cluster chain ended while seeking to offset %#x' % off)
        c = next_clus(c)
        s -= spc
    out = bytearray()
    boff = off % bps
    while len(out) < length:
        sec = dev.read(clus_lba(c) + s)              # SD: one 512-byte block per command (0x9e832148, r14 = 1)
        take = min(bps - boff, length - len(out))
        out += sec[boff:boff + take]
        boff = 0
        s += 1
        if s == spc:
            s = 0
            c = next_clus(c)
    return bytes(out)


def check_xboot(dev):
    print('[xboot] SD ISP, mode 0x43, file name from flash 0xC400')
    ctx, cl, nx = xboot_fat_init(dev, b'ISP_UPDTBIN')
    hdr = xboot_fat_read(dev, ctx, cl, nx, 0x10000, 0x40)
    if be32(hdr, 0) != 0x27051956:
        raise Fail('"bad mgaic": header at file offset 0x10000 is %s' % hdr[:8].hex())
    h = bytearray(hdr); h[4:8] = b'\0' * 4
    if hdr[0x1e] == 0x5a and (sum(struct.unpack('<16I', h)) & 0xffffffff) != be32(hdr, 4):
        raise Fail('"bad hcrc"')
    size = be32(hdr, 12)
    if size > 0x1fffc0:
        raise Fail('"image is too big"')
    body = xboot_fat_read(dev, ctx, cl, nx, 0x10040, size)
    if hdr[0x1e] == 0x5a:
        n = size // 4
        sm = sum(struct.unpack('<%dI' % n, body[:4 * n]))
        t = int.from_bytes(body[4 * n:], 'little')
        if (sm + t) & 0xffffffff != be32(hdr, 0x18):
            raise Fail('"corrupted image" (dcrc)')
    print('  OK: U-Boot %r, %d-byte body loads and verifies (%d sector reads)'
          % (hdr[32:64].split(b'\0')[0].decode(errors='replace'), size, dev.reads))


# ------------------------------------------------------------------------------ correct FAT reader (eCos/U-Boot)
class Fat:
    def __init__(self, dev, start):
        self.dev, self.start = dev, start
        b = dev.read(start)
        if b[0x1fe:0x200] != b'\x55\xaa':
            raise Fail('no boot-sector signature at LBA %d' % start)
        self.bps, self.spc, self.rsv, self.nfat = u16(b, 0x0b), b[0x0d], u16(b, 0x0e), b[0x10]
        if self.bps != SECTOR:
            raise Fail('bytes/sector %d != 512' % self.bps)
        rootent, fatsz16 = u16(b, 0x11), u16(b, 0x16)
        self.fat32 = fatsz16 == 0
        self.fatsz = u32(b, 0x24) if self.fat32 else fatsz16
        self.fat_start = start + self.rsv
        rootsecs = (rootent * 32 + SECTOR - 1) // SECTOR
        self.root_start = self.fat_start + self.nfat * self.fatsz
        self.data0 = self.root_start + (0 if self.fat32 else rootsecs)
        self.rootclus = u32(b, 0x2c) if self.fat32 else 0
        self.rootsecs = rootsecs
        tot = u16(b, 0x13) or u32(b, 0x20)
        self.nclus = (tot - (self.data0 - start)) // self.spc
        self.kind = 'FAT32' if self.fat32 else ('FAT16' if self.nclus >= 4085 else 'FAT12')

    def nxt(self, c):
        if self.fat32:
            o = c * 4
            v = u32(self.dev.read(self.fat_start + o // SECTOR), o % SECTOR) & 0x0FFFFFFF
            return v if v < 0x0FFFFFF8 else None
        o = c * 2
        v = u16(self.dev.read(self.fat_start + o // SECTOR), o % SECTOR)
        return v if v < 0xFFF8 else None

    def chain(self, c):
        out = []
        while c is not None and c >= 2:
            out.append(c)
            if len(out) > self.nclus + 2:
                raise Fail('cluster chain loop')
            c = self.nxt(c)
        return out

    def lba(self, c):
        return self.data0 + (c - 2) * self.spc

    def root_entries(self):
        secs = []
        if self.fat32:
            for c in self.chain(self.rootclus):
                secs += [self.lba(c) + i for i in range(self.spc)]
        else:
            secs = [self.root_start + i for i in range(self.rootsecs)]
        lfn = {}
        for s in secs:
            sec = self.dev.read(s)
            for o in range(0, SECTOR, 32):
                e = sec[o:o + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xE5:
                    lfn = {}
                    continue
                if e[11] == 0x0F:
                    seq = e[0] & 0x1F
                    lfn[seq] = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16-le', 'replace')
                    continue
                short = e[0:8].rstrip(b' ').decode('ascii', 'replace')
                ext = e[8:11].rstrip(b' ').decode('ascii', 'replace')
                long = ''.join(lfn[k] for k in sorted(lfn)).split('\0')[0] if lfn else None
                lfn = {}
                yield dict(raw=e[0:11], short=short + ('.' + ext if ext else ''), long=long, attr=e[11],
                           first=(u16(e, 0x14) << 16 | u16(e, 0x1a)) if self.fat32 else u16(e, 0x1a),
                           size=u32(e, 0x1c))

    def find(self, name):
        for e in self.root_entries():
            if e['attr'] & 0x18:
                continue
            if e['short'].upper() == name.upper() or (e['long'] or '').upper() == name.upper():
                return e
        return None

    def read(self, e, off, length):
        cl = self.chain(e['first'])
        csz = self.spc * SECTOR
        out = bytearray()
        while length > 0:
            ci, co = divmod(off, csz)
            if ci >= len(cl):
                raise Fail('read beyond cluster chain')
            sec = self.dev.read(self.lba(cl[ci]) + co // SECTOR)
            take = min(SECTOR - co % SECTOR, length)
            out += sec[co % SECTOR:co % SECTOR + take]
            off += take
            length -= take
        return bytes(out)


def uboot_part_start(dev):
    """U-Boot 2014.04 part_dos test_block_type() + get_device_and_partition("0"): MBR -> partition 1."""
    b = dev.read(0)
    if b[0x1fe:0x200] != b'\x55\xaa':
        return 0, 'no 55AA at LBA 0 -> whole device'
    for slot in range(3):
        bi = b[0x1be + 16 * slot]
        if bi not in (0, 0x80):
            if slot == 0 and (b[0x36:0x39] == b'FAT' or b[0x52:0x57] == b'FAT32'):
                return 0, 'LBA 0 is a FAT boot sector (superfloppy) -> whole device'
            return None, 'LBA 0 is neither MBR nor FAT boot sector'
    if b[0x1c2] == 0:
        # MBR accepted but partition 1 empty -> "** Invalid partition 1 **"; ispsp then retries with isp_dev 0:0
        return 0, 'MBR with empty partition 1 -> ispsp falls back to "0:0" (whole device)'
    return u32(b, 0x1c6), 'MBR partition 1 (type %#04x) at LBA %d' % (b[0x1c2], u32(b, 0x1c6))


def check_ecos_uboot(dev):
    start, why = uboot_part_start(dev)
    if start is None:
        raise Fail('U-Boot: ' + why)
    print('[eCos/U-Boot] filesystem: %s' % why)
    fs = Fat(dev, start)
    print('  %s, %d clusters of %d B' % (fs.kind, fs.nclus, fs.spc * SECTOR))
    names = [e['long'] or e['short'] for e in fs.root_entries() if not e['attr'] & 0x08]
    print('  root: %s' % ', '.join(names))

    e = fs.find('ISP_UPDT.BIN')
    print('[eCos] trigger')
    if not e:
        raise Fail('ISP_UPDT.BIN not in the root directory: eCos will not offer the upgrade')
    if e['size'] == 0 or e['size'] > ECOS_MAX:
        raise Fail('ISP_UPDT.BIN size %d: eCos needs 0 < size <= 12 MiB' % e['size'])
    print('  OK: ISP_UPDT.BIN, %d bytes' % e['size'])
    if e['raw'] != b'ISP_UPDTBIN':
        raise Fail('ISP_UPDT.BIN short-name entry is %r, xboot compares the raw 8.3 name "ISP_UPDTBIN"' % e['raw'])

    print('[U-Boot] ispsp all mmc 0: fatload ... /ISPBOOOT.BIN 0x800 0x100000; source $isp_ram_addr+0x20')
    b = fs.find('ISPBOOOT.BIN')
    if not b:
        raise Fail('/ISPBOOOT.BIN missing: U-Boot retries 6x, then writes "PART" and resets into USB ISP')
    if b['size'] < 0x100800:
        raise Fail('ISPBOOOT.BIN is %d bytes; needs >= 0x100800 so the 0x800-byte read at 0x100000 succeeds' % b['size'])
    blk = fs.read(b, 0x100000, 0x800)
    img = blk[0x20:]
    if be32(img, 0) != 0x27051956:
        raise Fail('no uImage at ISPBOOOT.BIN+0x100020 ("Wrong image format")')
    h = bytearray(img[:64]); h[4:8] = b'\0' * 4
    if zlib.crc32(bytes(h)) & 0xffffffff != be32(img, 4):
        raise Fail('script uImage: Bad header crc')
    size = be32(img, 12)
    if 64 + size > len(img):
        raise Fail('script uImage larger than the 0x7E0 bytes ispsp loads')
    if zlib.crc32(img[64:64 + size]) & 0xffffffff != be32(img, 24):
        raise Fail('script uImage: Bad data crc')
    if img[0x1e] != 6:
        raise Fail('script uImage: Bad image type %d' % img[0x1e])
    slen = be32(img, 64)
    print('  OK: script uImage %r, script:' % img[32:64].split(b'\0')[0].decode(errors='replace'))
    for line in img[72:72 + slen].decode('ascii', 'replace').splitlines():
        print('      | ' + line)
    a = fs.read(e, 0, e['size'])
    bb = fs.read(b, 0, b['size'])
    if a[0x10000:0x100800] != bb[0x10000:0x100800]:
        print('  NOTE: ISP_UPDT.BIN and ISPBOOOT.BIN differ (fine if intended: U-Boot comes from ISP_UPDT.BIN, '
              'the script from ISPBOOOT.BIN)')
    for extra in ('UENV.TXT',):
        x = fs.find(extra)
        if x:
            print('  NOTE: %s already present (%d bytes); fatwrite will overwrite it' % (extra, x['size']))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    if path.startswith('/dev/') and path[-1].isdigit() and not path.startswith('/dev/loop'):
        print('NOTE: %s looks like a partition; pass the whole card (e.g. /dev/sdX, /dev/mmcblk0)' % path)
    dev = Dev(path)
    gib = dev.size / 2**30
    print('device: %s, %.2f GiB (%d sectors)' % (path, gib, dev.size // SECTOR))
    if dev.size > 32 * 2**30 + 2**28:
        print('NOTE: > 32 GiB (SDXC). xboot treats it like SDHC; untested on this unit, a <= 32 GiB SDHC card is safer')
    ok = True
    for fn in (check_ecos_uboot, check_xboot):
        try:
            fn(dev)
        except Fail as ex:
            print('  FAIL: %s' % ex)
            ok = False
    print('PASS: all three readers accept this card' if ok else 'FAIL: do not use this card as-is')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
