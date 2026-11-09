#!/usr/bin/env python3
"""Check crypto.c (through crypto_host) against published test vectors, then against Python's hashlib/hmac/zlib and the
`cryptography` package on random inputs. Usage: test_crypto.py [path/to/crypto_host]. Exit status = number of failures.

Vector sources: FIPS 180 / RFC 3174 (SHA-1), RFC 2202 (HMAC-SHA1), IEEE 802.11 Annex J.3 PRF and J.4 passphrase->PSK
(as carried in hostapd src/crypto/crypto_module_tests.c), RFC 6070 (PBKDF2), FIPS 197 C.1 (AES-128), RFC 3394 4.1
(key unwrap), RFC 3610 packet vector 1 (AES-CCM M=8 L=2, as in Linux crypto/testmgr.h), IEEE 802.11 CCMP test MPDU
(802.11-2012 M.6.4 / 802.11-2016 J.6.4), and the CRC-32 check value."""
import hashlib, hmac, os, random, subprocess, sys, zlib

HOST = sys.argv[1] if len(sys.argv) > 1 else 'crypto_host'
fails = 0
passes = 0


def run(*args):
    return subprocess.run([HOST] + [str(a) for a in args], capture_output=True, text=True, check=True).stdout.split()


def check(name, got, want):
    global fails, passes
    if got.lower() == want.lower():
        passes += 1
        print('PASS', name)
    else:
        fails += 1
        print('FAIL', name, '\n  got ', got, '\n  want', want)


h = bytes.hex
# --- published vectors ---
check('SHA-1 "abc" (FIPS 180)', run('sha1', h(b'abc'))[0], 'a9993e364706816aba3e25717850c26c9cd0d89d')
check('SHA-1 "" (FIPS 180)', run('sha1', '')[0] if run('sha1', '') else '', 'da39a3ee5e6b4b0d3255bfef95601890afd80709')
check('SHA-1 448-bit (FIPS 180)', run('sha1', h(b'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'))[0],
      '84983e441c3bd26ebaae4aa1f95129e5e54670f1')
check('HMAC-SHA1 RFC 2202 #1', run('hmac', '0b' * 20, h(b'Hi There'))[0], 'b617318655057264e28bc0b6fb378c8ef146be00')
check('HMAC-SHA1 RFC 2202 #2', run('hmac', h(b'Jefe'), h(b'what do ya want for nothing?'))[0],
      'effcdf6ae5eb2fa2d27416d5f184df9c259a7c79')
check('HMAC-SHA1 RFC 2202 #6 (80-byte key)', run('hmac', 'aa' * 80,
      h(b'Test Using Larger Than Block-Size Key - Hash Key First'))[0], 'aa4ae5e15272d00e95705637ce8a3b55ed402112')
check('802.11 PRF test 0', run('prf', '0b' * 20, 'prefix', h(b'Hi There'), 64)[0],
      'bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee75df78c3d31e0f889f012120c0862beb'
      '67753e7439ae242edb8373698356cf5a')
check('802.11 PRF test 1', run('prf', h(b'Jefe'), 'prefix', h(b'what do ya want for nothing?'), 64)[0],
      '51f4de5b33f249adf81aeb713a3c20f4fe631446fabdfa58244759ae58ef9009a99abf4eac2ca5fa87e692c440eb4002'
      '3e7babb206d61de7b92f41529092b8fc')
check('802.11 PRF test 2', run('prf', 'aa' * 20, 'prefix', 'dd' * 50, 64)[0],
      'e1ac546ec4cb636f9976487be5c86be17a0252ca5d8d8df12cfb047352524'
      '9ce9dd8d177ead710bc9b590547239107aef7b4abd43d87f0a68f1cbd9e2b6f7607')
for p, s, c, n, want in [
        (b'password', b'salt', 1, 20, '0c60c80f961f0e71f3a9b524af6012062fe037a6'),
        (b'password', b'salt', 2, 20, 'ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957'),
        (b'password', b'salt', 4096, 20, '4b007901b765489abead49d926f721d065a429c1'),
        (b'passwordPASSWORDpassword', b'saltSALTsaltSALTsaltSALTsaltSALTsalt', 4096, 25,
         '3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038'),
        (b'pass\0word', b'sa\0lt', 4096, 16, '56fa6aa75548099dcc37d7f03425e0c3')]:
    check('PBKDF2 RFC 6070 %r/%r c=%d' % (p, s, c), run('pbkdf2', h(p), h(s), c, n)[0], want)
for p, s, want in [
        (b'password', b'IEEE', 'f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e'),
        (b'ThisIsAPassword', b'ThisIsASSID', '0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af'),
        (b'a' * 32, b'Z' * 32, 'becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62')]:
    check('802.11 passphrase->PSK %r' % s, run('pbkdf2', h(p), h(s), 4096, 32)[0], want)
out = run('aes', '000102030405060708090a0b0c0d0e0f', '00112233445566778899aabbccddeeff')
check('AES-128 FIPS 197 C.1 encrypt', out[0], '69c4e0d86a7b0430d8cdb78070b4c55a')
check('AES-128 FIPS 197 C.1 decrypt', out[1], '00112233445566778899aabbccddeeff')
check('RFC 3394 4.1 unwrap', run('unwrap', '000102030405060708090a0b0c0d0e0f',
                                 '1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5')[0], '00112233445566778899aabbccddeeff')
check('RFC 3394 4.1 unwrap, corrupted', run('unwrap', '000102030405060708090a0b0c0d0e0f',
                                            '1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe4')[0], 'FAIL')
out = run('ccm', 'c0c1c2c3c4c5c6c7c8c9cacbcccdcecf', '00000003020100a0a1a2a3a4a5', '0001020304050607',
          '08090a0b0c0d0e0f101112131415161718191a1b1c1d1e')
check('AES-CCM RFC 3610 packet vector 1', out[0], '588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0')
check('AES-CCM RFC 3610 packet vector 1 decrypt', out[1], 'DECOK')
hdr = '0848c32c0fd2e128a57c5030f1844408abaea5b8fcba8033'
out = run('ccmp', 'c97c1f67ce371185514a8a19f2bdd52f', 'b5039776e70c', hdr + 'f8ba1a55d02f85ae967bb62fb6cda8eb7e78a050',
          24, 0)
enc = hdr + '0ce7002076970' + '3b5' + 'f3d0a2fe9a3dbf2342a643e43246e80c3c04d019' + '7845ce0b16f97623'
check('CCMP 802.11 test MPDU encrypt', out[0], enc)
check('CCMP 802.11 test MPDU decrypt', out[1], 'DECOK')
check('CCMP 802.11 test MPDU PN', out[2], 'b5039776e70c')
check('CRC-32 of the CCMP test MPDU = its FCS 1d 99 f0 66', run('crc32', enc)[0], '66f0991d')
check('CRC-32 check value "123456789"', run('crc32', h(b'123456789'))[0], 'cbf43926')

# --- random cross-checks against independent implementations ---
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.keywrap import aes_key_wrap

rnd = random.Random(1)
rb = lambda n: bytes(rnd.getrandbits(8) for _ in range(n))
ok = 0
for i in range(40):
    m, k = rb(rnd.randrange(0, 300)), rb(rnd.randrange(1, 100))
    ok += run('sha1', h(m))[0] == hashlib.sha1(m).hexdigest() if m else 1
    ok += run('hmac', h(k), h(m) if m else '')[0] == hmac.new(k, m, hashlib.sha1).hexdigest()
    ok += run('crc32', h(m))[0] == '%08x' % zlib.crc32(m) if m else 1
    p, s = rb(rnd.randrange(8, 64)), rb(rnd.randrange(1, 33))
    ok += run('pbkdf2', h(p), h(s), 50, 32)[0] == hashlib.pbkdf2_hmac('sha1', p, s, 50, 32).hex()
    data = rb(76)
    want = b''.join(hmac.new(k, b'Pairwise key expansion\0' + data + bytes([j]), hashlib.sha1).digest()
                    for j in range(3))[:48]
    ok += run('prf', h(k), 'Pairwise key expansion', h(data), 48)[0] == want.hex()
    key, blk = rb(16), rb(16)
    e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    ok += run('aes', h(key), h(blk))[0] == (e.update(blk) + e.finalize()).hex()
    kd = rb(8 * rnd.randrange(2, 8))
    ok += run('unwrap', h(key), aes_key_wrap(key, kd).hex())[0] == kd.hex()
    nonce, aad, pt = rb(13), rb(rnd.randrange(0, 40)), rb(rnd.randrange(1, 200))
    out = run('ccm', h(key), h(nonce), h(aad), h(pt))
    ok += out[0] == AESCCM(key, tag_length=8).encrypt(nonce, pt, aad).hex() and out[1] == 'DECOK'
    # QoS data from DS with retry/more-data bits set: round trip through ccmp_encrypt/ccmp_decrypt
    qhdr = bytes([0x88, 0x2a]) + rb(22) + bytes([rnd.randrange(16), 0])
    out = run('ccmp', h(key), '%012x' % rnd.getrandbits(48), h(qhdr + pt), 26, rnd.randrange(4))
    ok += out[1] == 'DECOK'
print('random cross-checks: %d/%d agree' % (ok, 40 * 9))
fails += 40 * 9 - ok
print('%d published vectors passed, %d failures in total' % (passes, fails))
sys.exit(fails)
