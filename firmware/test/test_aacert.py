#!/usr/bin/env python3
"""Host test of aacert.c (the payload's certificate extraction) and tools/aacert.py (the PC tool), which must agree.

Synthetic flash images first, built here with throwaway keys (never the unit's): JFFS2 filesystems among random data,
the module XORed and not, its data nodes out of order, an older node and an obsoleted node over the same span, an older
directory entry for another inode, decoy PEM text, a root certificate and a stray key ahead of the pair. Then failure
cases. Then, if dump1.bin (the original 16 MB flash, kept out of git) is at the repository root, the real module: both
must write byte-identical AACERT.PEM / AAKEY.PEM, compared by hash only (no key material is printed).

  python3 -I test_aacert.py ./aacert_host"""
import datetime, hashlib, json, lzma, os, random, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
TOOL = os.path.join(ROOT, 'tools', 'aacert.py')
HOST = os.path.abspath(sys.argv[1])
DUMP = os.path.join(ROOT, 'dump1.bin')
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print('FAIL', what)
    return cond


def jcrc(b):
    return zlib.crc32(b, 0xffffffff) ^ 0xffffffff


def hdr(ntype, tlen):
    return struct.pack('<HHII', 0x1985, ntype, tlen, jcrc(struct.pack('<HHI', 0x1985, ntype | 0x2000, tlen)))


def dirent(pino, ver, ino, name):
    head = hdr(0xE001, 40 + len(name)) + struct.pack('<IIIIBBH', pino, ver, ino, 0, len(name), 8, 0)
    return head + struct.pack('<II', jcrc(head), jcrc(name)) + name


def inode(ino, ver, isize, off, data, obsolete=False):
    head = hdr(0xE002, 68 + len(data)) + struct.pack('<IIIHHIIIIIIIBBH', ino, ver, 0o100644, 0, 0, isize, 0, 0, 0, off,
                                                      len(data), len(data), 0, 0, 0)
    n = bytearray(head + struct.pack('<II', jcrc(data), jcrc(head)) + data)
    if obsolete:
        n[3] &= ~0x20   # the "accurate" bit cleared on flash, as JFFS2 obsoletes a node
    return bytes(n)


class Fs:
    """a JFFS2 filesystem image: 64 KB erase blocks, each starting with a cleanmarker, nodes 4-byte aligned"""
    def __init__(self, blocks):
        self.img = bytearray(b'\xff' * (blocks * 0x10000))
        self.pos = 0
        self.mark()

    def mark(self):
        self.img[self.pos:self.pos + 12] = hdr(0x2003, 12)
        self.pos += 12

    def add(self, node):
        if (self.pos & 0xffff) + len(node) > 0x10000:
            self.pos = (self.pos | 0xffff) + 1
            self.mark()
        self.img[self.pos:self.pos + len(node)] = node
        self.pos = (self.pos + len(node) + 3) & ~3


def certs(rng):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    now = datetime.datetime.now(datetime.timezone.utc)

    def name(o):
        return x509.Name([x509.NameAttribute(NameOID.ORGANIZATION_NAME, o), x509.NameAttribute(NameOID.COUNTRY_NAME, 'US')])

    def cert(subj, iss, pub, signer):
        return (x509.CertificateBuilder().subject_name(subj).issuer_name(iss).public_key(pub)
                .serial_number(rng.getrandbits(64)).not_valid_before(now - datetime.timedelta(days=1))
                .not_valid_after(now + datetime.timedelta(days=3650)).sign(signer, hashes.SHA256())
                .public_bytes(serialization.Encoding.PEM))

    rk, lk, xk = (rsa.generate_private_key(public_exponent=65537, key_size=2048) for _ in range(3))
    root_n, leaf_n = name('Test Automotive Link'), name('Test-Linux-Reference')
    pem = lambda k, f: k.private_bytes(serialization.Encoding.PEM, f, serialization.NoEncryption())   # noqa: E731
    return dict(root=cert(root_n, root_n, rk.public_key(), rk), leaf=cert(leaf_n, root_n, lk.public_key(), rk),
                key8=pem(lk, serialization.PrivateFormat.PKCS8), keyt=pem(lk, serialization.PrivateFormat.TraditionalOpenSSL),
                stray=pem(xk, serialization.PrivateFormat.PKCS8))


def text(rng, n):   # compressible filler that looks like code and strings
    words = [b'AASslWrapper', b'CONTROL_MESSAGE', b'\x00\x00\x00\x00', b'polarssl', b'\x12\x34\x56\x78', b'-', b'----']
    out = bytearray()
    while len(out) < n:
        out += rng.choice(words) + bytes(rng.getrandbits(8) for _ in range(rng.randrange(4)))
    return bytes(out[:n])


def module(rng, c, key='key8', with_leaf=True):
    parts = [text(rng, 30000), b'-----BEGIN RSA PRIVATE KEY-----\0-----END RSA PRIVATE KEY-----\0', c['root'], b'\0',
             text(rng, 5000), c['stray'], b'\0', text(rng, 40000), c[key], b'\0', text(rng, 3000)]
    if with_leaf:
        parts += [c['leaf'], b'\0']
    return b''.join(parts + [text(rng, 60000)])


def lzma_alone(data, xor):
    z = bytearray(lzma.compress(data, format=lzma.FORMAT_ALONE, filters=[
        {'id': lzma.FILTER_LZMA1, 'dict_size': 8 << 20, 'lc': 3, 'lp': 0, 'pb': 2}]))
    z[5:13] = struct.pack('<Q', len(data))   # the stock modules carry their size (liblzma writes "unknown")
    return bytes(b ^ xor for b in z)


def image(rng, packed, name=b'android_auto_ROM.bin', corrupt=None):
    """4 MB flash: random 'code', a config filesystem, the module filesystem, 0xFF"""
    flash = bytearray(rng.getrandbits(8) for _ in range(0x100000)) + bytearray(b'\xff' * 0x300000)
    flash[0x2000:0x2004] = b'\x85\x19\x01\xe0'   # a stray magic with a bad CRC on a 4 KB boundary
    cfg = Fs(2)
    cfg.add(dirent(1, 1, 2, b'usercfg.bin'))
    cfg.add(inode(2, 2, 100, 0, bytes(100)))
    flash[0x140000:0x140000 + len(cfg.img)] = cfg.img
    fs = Fs(8)
    fs.add(dirent(1, 1, 2, b'android_auto'))
    fs.add(dirent(2, 2, 9, name))   # older entry: inode 9, a stale file
    fs.add(inode(9, 3, 5000, 0, bytes(rng.getrandbits(8) for _ in range(5000))))
    chunks = [(off, packed[off:off + 4096]) for off in range(0, len(packed), 4096)]
    order = list(range(len(chunks)))
    rng.shuffle(order)
    fs.add(inode(10, 4, len(packed), chunks[3][0], bytes(len(chunks[3][1]))))   # older version of chunk 3: zeros
    fs.add(dirent(2, 20, 10, name))   # the current entry
    ver = 5
    for i in order:
        off, d = chunks[i]
        if corrupt == i:
            d = bytearray(d)
            n = bytearray(inode(10, ver, len(packed), off, bytes(d)))
            n[68 + 10] ^= 1   # data CRC now wrong: the node is skipped and leaves a hole
            fs.add(bytes(n))
        else:
            fs.add(inode(10, ver, len(packed), off, d))
        ver += 1
    fs.add(inode(10, ver + 5, len(packed), chunks[5][0], b'\xaa' * len(chunks[5][1]), obsolete=True))
    flash[0x200000:0x200000 + len(fs.img)] = fs.img
    return bytes(flash)


def run_host(path, out, budget=None):
    r = subprocess.run([HOST, path, out] + ([str(budget)] if budget else []), capture_output=True, text=True)
    facts = dict(l.split('=', 1) for l in r.stdout.splitlines() if '=' in l)
    return r.returncode, facts


def run_tool(path, out, *extra):
    r = subprocess.run([sys.executable, '-I', TOOL, path, '--json', '--out', out] + list(extra), capture_output=True,
                       text=True)
    return r.returncode, (json.loads(r.stdout) if r.returncode == 0 and r.stdout.strip() else {}), r.stderr


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def cert_der_sha(path):
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
    c = x509.load_pem_x509_certificate(open(path, 'rb').read())
    return hashlib.sha256(c.public_bytes(serialization.Encoding.DER)).hexdigest()


def main():
    rng = random.Random(1985)
    c = certs(rng)
    with tempfile.TemporaryDirectory(prefix='aacert_test_') as tmp:
        def path(*p):
            return os.path.join(tmp, *p)

        for label, xor, key in (('xor 0x4d, PKCS#8 key', 0x4D, 'key8'), ('no xor, traditional key', 0, 'keyt')):
            before = fails
            img = image(rng, lzma_alone(module(rng, c, key), xor))
            open(path('flash.bin'), 'wb').write(img)
            for budget in (None, 1000, 1 << 20):
                d = path('c%s' % budget)
                os.mkdir(d)
                rc, f = run_host(path('flash.bin'), d, budget)
                check(rc == 0, '%s, budget %s: aacert_host exit %d (%s)' % (label, budget, rc, f.get('err_text')))
                if rc == 0:
                    check(open(os.path.join(d, 'AACERT.PEM'), 'rb').read() == c['leaf'], label + ': wrong certificate')
                    check(open(os.path.join(d, 'AAKEY.PEM'), 'rb').read() == c[key], label + ': wrong key')
                    check(int(f['xor'], 16) == xor and f['ino'] == '10', label + ': xor %s ino %s' % (f['xor'], f['ino']))
                    check(budget == 1 << 20 or int(f['decoded']) < int(f['unpacked']),
                          label + ': did not stop once the pair was found')
                os.system('rm -rf ' + d)
            rc, f, err = run_tool(path('flash.bin'), path('py'), '--any-issuer', '--force')
            check(rc == 0, '%s: aacert.py exit %d: %s' % (label, rc, err.strip()))
            if rc == 0:
                check(open(path('py', 'AACERT.PEM'), 'rb').read() == c['leaf'], label + ': aacert.py certificate')
                check(open(path('py', 'AAKEY.PEM'), 'rb').read() == c[key], label + ': aacert.py key')
                check(f['issuer_in_module'] is not None, label + ': aacert.py did not verify against the root')
            rc, f, err = run_tool(path('flash.bin'), path('py'), '--force')
            check(rc != 0 and 'issuer organization' in err, label + ': aacert.py accepted a non-Google issuer')
            print('test_aacert: %s %s' % (label, 'ok' if fails == before else 'FAILED'))

        # failures, each with the expected aacert.c error
        before = fails
        packed = lzma_alone(module(rng, c), 0x4D)
        for label, img, want in (
                ('damaged data node', image(rng, packed, corrupt=2), '-2'),
                ('no module', image(rng, packed, name=b'carplay_ROM.bin'), '-1'),
                ('key without its certificate', image(rng, lzma_alone(module(rng, c, with_leaf=False), 0x4D)), '-7'),
                ('not LZMA', image(rng, b'\x01\x02' + packed[2:]), '-3')):
            open(path('bad.bin'), 'wb').write(img)
            os.makedirs(path('bad'), exist_ok=True)
            rc, f = run_host(path('bad.bin'), path('bad'))
            check(rc == 2 and f.get('err') == want, '%s: aacert_host exit %d err %s, want %s' % (label, rc, f.get('err'), want))
            check(not os.listdir(path('bad')), label + ': files written')
            rc, _, _ = run_tool(path('bad.bin'), path('badpy'), '--any-issuer')
            check(rc != 0 and not os.path.exists(path('badpy')), label + ': aacert.py did not refuse')
        print('test_aacert: failure cases %s' % ('ok' if fails == before else 'FAILED'))

        if not os.path.exists(DUMP):
            print('test_aacert: dump1.bin skipped (not at the repository root)')
        else:
            os.mkdir(path('c'), 0o700)
            rc, f = run_host(DUMP, path('c'))
            check(rc == 0, 'dump1.bin: aacert_host exit %d (%s)' % (rc, f.get('err_text')))
            rc2, j, err = run_tool(DUMP, path('py'), '--force')
            check(rc2 == 0, 'dump1.bin: aacert.py exit %d: %s' % (rc2, err.strip()))
            if rc == 0 and rc2 == 0:
                same_c = check(sha(path('c', 'AACERT.PEM')) == sha(path('py', 'AACERT.PEM')), 'dump1.bin: certificates differ')
                same_k = check(sha(path('c', 'AAKEY.PEM')) == sha(path('py', 'AAKEY.PEM')), 'dump1.bin: keys differ')
                check(cert_der_sha(path('c', 'AACERT.PEM')) == j['cert_sha256'], 'dump1.bin: certificate DER hash')
                check(not j['problems'], 'dump1.bin: aacert.py problems %s' % j['problems'])
                if same_c and same_k:
                    print('test_aacert: dump1.bin ok: module entry at 0x%06x, certificate sha256 %s, key hashes match '
                          '(%s KB of %s unpacked, %s steps, %s us on this PC)' % (j['dirent'], j['cert_sha256'],
                          int(f['decoded']) >> 10, int(f['unpacked']) >> 10, f['steps'], f['total_us']))
    print('test_aacert: %s (%d failures)' % ('FAILED' if fails else 'ok', fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
