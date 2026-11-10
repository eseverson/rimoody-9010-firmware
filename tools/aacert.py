#!/usr/bin/env python3
"""Take the head unit's own Android Auto certificate and private key out of a flash dump.

The stock firmware keeps them in its Android Auto module, android_auto_ROM.bin, a file in a JFFS2 filesystem in NOR
(notes/android_auto.md, "Where the certificate lives"). This finds the file by walking the dump for JFFS2 nodes (no
fixed offsets), puts it together from its data nodes, removes the module's one-byte XOR if it has one, unpacks the
LZMA-alone stream and picks the private key and the certificate that pairs with it. firmware/aacert.c does the same on
the unit; test/test_aacert.sh checks that both give the same files.

  aacert.py dump1.bin --out DIR              write DIR/AACERT.PEM and DIR/AAKEY.PEM (mode 600)
  aacert.py NORDUMP.BIN                      only report what is there
  aacert.py dump1.bin --compare CERT KEY     do existing files hold the same certificate and key? (prints no key data)

The input is a whole-flash image, file offset = flash offset (dump1.bin, or NORDUMP.BIN from `hu.py nor dump`; the
unit maps it at 0x98000000). Never writes into this repository: the key is the unit's identity and stays out of git.
"""
import argparse, datetime, hashlib, json, lzma, os, re, struct, sys, zlib

NAME = b'android_auto_ROM.bin'
XIP = 0x98000000
J_MAGIC, J_ACCURATE, J_DIRENT, J_INODE = 0x1985, 0x2000, 0xE001, 0xE002
PROBE = 0x1000
PEM_MAX = 8192
AA_ROOT_O = 'Google Automotive Link'


def jcrc(b):   # Linux crc32(0, ...): no pre/post inversion
    return zlib.crc32(b, 0xffffffff) ^ 0xffffffff


def node_at(f, pos):
    if len(f) - pos < 12 or struct.unpack_from('<H', f, pos)[0] != J_MAGIC:
        return 0
    magic, ntype, tlen, hcrc = struct.unpack_from('<HHII', f, pos)
    if jcrc(struct.pack('<HHI', magic, ntype | J_ACCURATE, tlen)) != hcrc or tlen < 12 or tlen > len(f) - pos:
        return 0
    return tlen


def walk(f):
    """(offset, type, length) of every JFFS2 node: probe each 4 KB boundary, follow node lengths, resync by 4 bytes"""
    pos, chain = 0, False
    while len(f) - pos >= 12:
        tlen = node_at(f, pos)
        if tlen:
            yield pos, struct.unpack_from('<H', f, pos + 2)[0], tlen
            chain = True
            pos += (tlen + 3) & ~3
        elif chain and f[pos:pos + 4] != b'\xff\xff\xff\xff':
            pos += 4
        else:
            chain = False
            pos = (pos | (PROBE - 1)) + 1


def find_module(f):
    nodes = [n for n in walk(f) if n[1] & J_ACCURATE]
    best = None
    for pos, t, tlen in nodes:
        if t != J_DIRENT or tlen < 40:
            continue
        pino, ver, ino, mctime, nsize = struct.unpack_from('<IIIIB', f, pos + 12)
        ncrc, namecrc = struct.unpack_from('<II', f, pos + 32)
        name = f[pos + 40:pos + 40 + nsize]
        if name == NAME and jcrc(f[pos:pos + 32]) == ncrc and jcrc(name) == namecrc and (best is None or ver >= best[1]):
            best = (pos, ver, ino)
    if not best or not best[2]:
        sys.exit('no %s in a JFFS2 filesystem in the image' % NAME.decode())
    dirent, _, ino = best
    data, bad = [], 0
    for pos, t, tlen in nodes:
        if t != J_INODE or tlen < 68 or struct.unpack_from('<I', f, pos + 12)[0] != ino:
            continue
        (ver,) = struct.unpack_from('<I', f, pos + 16)
        (isize,) = struct.unpack_from('<I', f, pos + 28)
        off, csize, dsize, compr = struct.unpack_from('<IIIB', f, pos + 44)
        dcrc, ncrc = struct.unpack_from('<II', f, pos + 60)
        body = f[pos + 68:pos + 68 + csize]
        ok = jcrc(f[pos:pos + 60]) == ncrc and tlen - 68 >= csize and (
            (compr == 0 and csize == dsize and jcrc(body) == dcrc) or compr == 1)
        if not ok:
            bad += 1
            continue
        data.append((ver, pos, isize, off, dsize, compr, body))
    if not data:
        sys.exit('%s (inode %d): no good data nodes' % (NAME.decode(), ino))
    data.sort(key=lambda d: d[0])
    size = data[-1][2]
    buf = bytearray(size)
    first = None
    for ver, pos, isize, off, dsize, compr, body in data:
        if off >= size:
            continue
        k = min(dsize, size - off)
        buf[off:off + k] = body[:k] if compr == 0 else bytes(k)
        if off == 0:
            first = pos
    covered = 0
    for d in sorted(data, key=lambda d: d[3]):
        if d[3] > covered:
            break
        covered = max(covered, d[3] + d[4])
    if covered < size:
        sys.exit('%s: no data for file bytes %d.. (a hole)' % (NAME.decode(), covered))
    return dict(dirent=dirent, ino=ino, nodes=len(data), bad_nodes=bad, first_node=first, packed=size), bytes(buf)


def unpack(packed):
    key = packed[1]   # LZMA-alone: props, dict size u32 (low byte 0), size u64; the XOR shows in byte 1
    raw = bytes(b ^ key for b in packed) if key else packed
    props, dic, size = struct.unpack_from('<BIQ', raw)
    if props >= 225 or dic < 4096 or size >> 32 or not size:
        sys.exit('the module is not an LZMA-alone stream with a known size (props %#x, dict %#x, size %#x)' %
                 (props, dic, size))
    d = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
    out = d.decompress(raw, max_length=size)
    if len(out) != size:
        sys.exit('LZMA: %d of %d bytes' % (len(out), size))
    return key, props, dic, out


PEM_RE = re.compile(rb'-----BEGIN ([A-Z ]{1,40})-----([\r\n][A-Za-z0-9+/=\r\n]*\n)-----END \1-----')
KEY_LABELS = (b'PRIVATE KEY', b'RSA PRIVATE KEY', b'EC PRIVATE KEY')


def pem_blocks(m):
    """(offset, label, text) of every PEM block the firmware's scanner accepts (aacert.c pem_at), in order"""
    for x in PEM_RE.finditer(m):
        text = x.group(0)
        if len(text) + 2 <= PEM_MAX and len(re.sub(rb'[\r\n]', b'', x.group(2))) >= 64:
            yield x.start(), x.group(1), text


def pick(m):
    """the firmware's rule: the first key + certificate pair in unpacking order"""
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
    certs, keys = [], []

    def pairs(c, k):
        try:
            cert = x509.load_pem_x509_certificate(c[2] + b'\n')
            key = serialization.load_pem_private_key(k[2] + b'\n', None)
        except Exception:   # noqa: BLE001 - anything unparsable just does not pair
            return False
        return cert.public_key().public_numbers() == key.public_key().public_numbers()

    for blk in pem_blocks(m):
        if blk[1] == b'CERTIFICATE' and len(certs) < 8:
            for k in keys:
                if pairs(blk, k):
                    return blk, k, certs + [blk]
            certs.append(blk)
        elif blk[1] in KEY_LABELS and len(keys) < 8:
            for c in certs:
                if pairs(c, blk):
                    return c, blk, certs
            keys.append(blk)
    sys.exit('no private key that pairs with a certificate in the module (%d certificates, %d keys)' %
             (len(certs), len(keys)))


def org(name):
    from cryptography.x509.oid import NameOID
    v = name.get_attributes_for_oid(NameOID.ORGANIZATION_NAME)
    return v[0].value if v else ''


def check_cert(cert_pem, key_pem, others, any_issuer):
    """sanity checks; returns (facts, problems)"""
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
    cert = x509.load_pem_x509_certificate(cert_pem)
    key = serialization.load_pem_private_key(key_pem, None)
    probs = []
    if cert.public_key().public_numbers() != key.public_key().public_numbers():
        probs.append('the key does not belong to the certificate')
    if cert.subject == cert.issuer:
        probs.append('self-signed: a root, not a head-unit certificate')
    if not org(cert.subject):
        probs.append('no organization in the subject')
    if org(cert.issuer) != AA_ROOT_O and not any_issuer:
        probs.append('issuer organization is %r, not %r' % (org(cert.issuer), AA_ROOT_O))
    now = datetime.datetime.now(datetime.timezone.utc)
    if not cert.not_valid_before_utc <= now <= cert.not_valid_after_utc:
        probs.append('not valid now (%s .. %s)' % (cert.not_valid_before_utc, cert.not_valid_after_utc))
    signed_by = None
    for o in others:
        ic = x509.load_pem_x509_certificate(o[2] + b'\n')
        if ic.subject == cert.issuer and ic.subject != cert.subject:
            try:
                cert.verify_directly_issued_by(ic)
                signed_by = ic.subject.rfc4514_string()
            except Exception as e:   # noqa: BLE001 - any failure is a finding
                probs.append('issuer certificate in the module does not verify it: %s' % e)
    der = cert.public_bytes(serialization.Encoding.DER)
    spki = cert.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    pub = key.public_key()
    facts = dict(subject=cert.subject.rfc4514_string(), issuer=cert.issuer.rfc4514_string(),
                 not_before=str(cert.not_valid_before_utc), not_after=str(cert.not_valid_after_utc),
                 key_type='RSA' if 'RSA' in type(pub).__name__ else type(pub).__name__.strip('_'),
                 key_bits=getattr(pub, 'key_size', 0),
                 cert_sha256=hashlib.sha256(der).hexdigest(), pubkey_sha256=hashlib.sha256(spki).hexdigest(),
                 issuer_in_module=signed_by)
    return facts, probs


def inside_repo(path):
    repo = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    p = os.path.realpath(path)
    return os.path.exists(os.path.join(repo, '.git')) and (p == repo or p.startswith(repo + os.sep))


def compare(cert_pem, key_pem, cert_path, key_path):
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
    enc = serialization.Encoding.DER
    a = x509.load_pem_x509_certificate(cert_pem).public_bytes(enc)
    b = x509.load_pem_x509_certificate(open(cert_path, 'rb').read()).public_bytes(enc)
    fmt = (enc, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    ka = serialization.load_pem_private_key(key_pem, None).private_bytes(*fmt)
    kb = serialization.load_pem_private_key(open(key_path, 'rb').read(), None).private_bytes(*fmt)
    return hashlib.sha256(a).digest() == hashlib.sha256(b).digest(), hashlib.sha256(ka).digest() == hashlib.sha256(kb).digest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='whole-flash image (dump1.bin, NORDUMP.BIN)')
    ap.add_argument('--out', help='directory for AACERT.PEM and AAKEY.PEM (outside this repository)')
    ap.add_argument('--force', action='store_true', help='replace existing files in --out')
    ap.add_argument('--compare', nargs=2, metavar=('CERT', 'KEY'), help='compare with existing files (by hash)')
    ap.add_argument('--any-issuer', action='store_true', help='accept an issuer other than ' + AA_ROOT_O)
    ap.add_argument('--json', action='store_true', help='facts as one JSON object (no key data)')
    a = ap.parse_args()

    f = open(a.image, 'rb').read()
    mod, packed = find_module(f)
    key, props, dic, m = unpack(packed)
    cert_blk, key_blk, _ = pick(m)
    certs = [b for b in pem_blocks(m) if b[1] == b'CERTIFICATE']
    cert_pem, key_pem = cert_blk[2] + b'\n', key_blk[2] + b'\n'   # as the firmware saves them
    others = [c for c in certs if c[0] != cert_blk[0]]
    facts, probs = check_cert(cert_pem, key_pem, others, a.any_issuer)
    facts.update(mod, xor=key, lzma_props=props, lzma_dict=dic, unpacked=len(m), key_at=key_blk[0],
                 cert_at=cert_blk[0], certificates=len(certs), cert_file_sha256=hashlib.sha256(cert_pem).hexdigest(),
                 problems=probs)
    if not a.json:
        print('module    %s at flash %#08x (XIP %#010x): inode %d, %d data nodes%s, first at %#08x' % (
            NAME.decode(), mod['dirent'], XIP + mod['dirent'], mod['ino'], mod['nodes'],
            ' (%d bad, skipped)' % mod['bad_nodes'] if mod['bad_nodes'] else '', mod['first_node']))
        print('packed    %d bytes, XOR %#04x, LZMA-alone props %#04x dict %d KB, %d bytes unpacked' % (
            mod['packed'], key, props, dic >> 10, len(m)))
        print('found     private key at +%#x (%s), certificate at +%#x (%d certificates in the module)' % (
            key_blk[0], key_blk[1].decode(), cert_blk[0], len(certs)))
        print('subject   %s' % facts['subject'])
        print('issuer    %s%s' % (facts['issuer'], ', verified with the issuer certificate in the module'
                                  if facts['issuer_in_module'] else ''))
        print('valid     %s .. %s, %s %d' % (facts['not_before'], facts['not_after'], facts['key_type'],
                                             facts['key_bits']))
        print('sha256    certificate (DER) %s' % facts['cert_sha256'])
        print('key       pairs with the certificate' if not any('belong' in p for p in probs) else 'key       DOES NOT PAIR')
    for p in probs:
        print('PROBLEM   %s' % p, file=sys.stderr)

    rc = 1 if probs else 0
    if a.compare:
        try:
            same_c, same_k = compare(cert_pem, key_pem, *a.compare)
        except (OSError, ValueError) as e:
            sys.exit('compare: cannot read %s / %s: %s' % (a.compare[0], a.compare[1], type(e).__name__))
        facts.update(compare_cert=same_c, compare_key=same_k)
        if not a.json:
            print('compare   certificate %s, key %s' % ('same' if same_c else 'DIFFERENT', 'same' if same_k else 'DIFFERENT'))
        rc = rc or (0 if same_c and same_k else 1)
    if a.out and not probs:
        if inside_repo(a.out):
            sys.exit('refusing to write the key inside the repository: pick a directory outside it')
        os.makedirs(a.out, exist_ok=True)
        for name, data, mode in (('AACERT.PEM', cert_pem, 0o644), ('AAKEY.PEM', key_pem, 0o600)):
            p = os.path.join(a.out, name)
            if os.path.exists(p) and not a.force:
                sys.exit('%s exists (use --force to replace it)' % p)
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
            with os.fdopen(fd, 'wb') as fo:
                fo.write(data)
            os.chmod(p, mode)
        if not a.json:
            print('wrote     %s (%d bytes), %s (%d bytes)' % (os.path.join(a.out, 'AACERT.PEM'), len(cert_pem),
                                                            os.path.join(a.out, 'AAKEY.PEM'), len(key_pem)))
    if a.json:
        print(json.dumps(facts))
    sys.exit(rc)


if __name__ == '__main__':
    main()
