#!/usr/bin/env python3
"""Check gcm_alt.c (through gcm_host) against the NIST GCM spec vectors and against the `cryptography` package on
random inputs: AES-128/192/256, 12-byte and other IV lengths, empty and partial-block AAD and data, chunked updates,
misaligned buffers, and tag rejection. Usage: test_gcm.py [path/to/gcm_host]. Exit status = number of failures."""
import os, random, subprocess, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HOST = sys.argv[1] if len(sys.argv) > 1 else './gcm_host'
h = lambda b: b.hex() if b else '-'


def ref_encrypt(key, iv, aad, pt):
    e = Cipher(algorithms.AES(key), modes.GCM(iv)).encryptor()
    e.authenticate_additional_data(aad)
    ct = e.update(pt) + e.finalize()
    return ct, e.tag


# NIST "The Galois/Counter Mode of Operation" test cases 2, 3, 4 and 6 (AES-128)
K = bytes.fromhex('feffe9928665731c6d6a8f9467308308')
P = bytes.fromhex('d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525'
                  'b16aedf5aa0de657ba637b39')
A = bytes.fromhex('feedfacedeadbeeffeedfacedeadbeefabaddad2')
nist = [
    (bytes(16), bytes(12), b'', bytes(16), '0388dace60b6a392f328c2b971b2fe78', 'ab6e47d42cec13bdf53a67b21257bddf'),
    (K, bytes.fromhex('cafebabefacedbaddecaf888'), b'', P + bytes.fromhex('b1e6'), None, None),
    (K, bytes.fromhex('cafebabefacedbaddecaf888'), A, P, '42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca1'
     '2e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091', '5bc94fbc3221a5db94fae95ae7121a47'),
    (K, bytes.fromhex('9313225df88406e555909c5aff5269aa6a7a9538534f7da1e4c303d2a318a728c3c0c95156809539fcf0e2429a6b5254'
                      '16aedbf5a0de6a57a637b39b'), A, P, '8ce24998625615b603a033aca13fb894be9112a5c3a211a8ba262a3cca7e2c'
     'a701e4a9a4fba43c90ccdcb281d48c7c6fd62875d2aca417034c34aee5', '619cc5aefffe0bfa462af43c1699d050'),
]
cases, want = [], []
for key, iv, aad, pt, ct, tag in nist:
    if ct is None:
        ct, tag = (x.hex() for x in ref_encrypt(key, iv, aad, pt))
    else:
        assert ref_encrypt(key, iv, aad, pt) == (bytes.fromhex(ct), bytes.fromhex(tag))   # the vectors themselves
    for chunk, off in ((0, 0), (1, 1), (7, 2), (16, 3)):
        cases.append('E %s %s %s %s %d %d' % (h(key), h(iv), h(aad), h(pt), chunk, off))
        want.append('%s %s' % (ct, tag))

rnd = random.Random(1)
for i in range(400):
    key = os.urandom(rnd.choice((16, 24, 32)))
    iv = os.urandom(12 if i % 3 else rnd.choice((8, 16, 17, 60, 128)))
    aad = os.urandom(rnd.choice((0, 0, 5, 13, 16, 31, 64)))
    pt = os.urandom(rnd.choice((0, 1, 15, 16, 17, 100, 1024, 16384)) if i % 5 else rnd.randrange(0, 3000))
    chunk, off = rnd.choice((0, 0, 1, 5, 16, 33)), rnd.randrange(4)
    if len(pt) > 2000 and chunk == 1:
        chunk = 17
    ct, tag = ref_encrypt(key, iv, aad, pt)
    cases.append('E %s %s %s %s %d %d' % (h(key), h(iv), h(aad), h(pt), chunk, off))
    want.append('%s %s' % (h(ct), tag.hex()))
    cases.append('D %s %s %s %s %s %d %d' % (h(key), h(iv), h(aad), h(ct), tag.hex(), chunk, off))
    want.append(h(pt))
    if i % 4 == 0:   # a flipped tag or ciphertext bit must be rejected
        bad_tag = bytes([tag[0] ^ 1]) + tag[1:]
        cases.append('D %s %s %s %s %s %d %d' % (h(key), h(iv), h(aad), h(ct), bad_tag.hex(), chunk, off))
        want.append('AUTHFAIL')
        if ct:
            bad_ct = bytes([ct[-1] ^ 0x80]) + b'' if len(ct) == 1 else ct[:-1] + bytes([ct[-1] ^ 0x80])
            cases.append('D %s %s %s %s %s %d %d' % (h(key), h(iv), h(aad), h(bad_ct), tag.hex(), chunk, off))
            want.append('AUTHFAIL')
    if i % 7 == 0:   # truncated tags (TLS uses 16; the API allows 4..16)
        tl = rnd.choice((4, 8, 12))
        cases.append('D %s %s %s %s %s %d %d' % (h(key), h(iv), h(aad), h(ct), tag[:tl].hex(), chunk, off))
        want.append(h(pt))

out = subprocess.run([HOST], input='\n'.join(cases) + '\n', capture_output=True, text=True, check=True).stdout.split('\n')
fails = 0
for k, (c, w) in enumerate(zip(cases, want)):
    if out[k] != w:
        fails += 1
        if fails <= 5:
            print('FAIL', c[:120], '\n  got ', out[k][:120], '\n  want', w[:120])
print('%s gcm_alt: %d cases, %d failures' % ('PASS' if not fails else 'FAIL', len(cases), fails))
sys.exit(fails)
