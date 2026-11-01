#!/usr/bin/env python3
"""Play a WPA2-PSK authenticator (hostapd's side of IEEE 802.11 12.7.6/12.7.7) against wpa.c through wpa_host, with
the AP's crypto done independently in Python (hashlib/hmac + cryptography's AES key wrap).
Usage: test_wpa.py path/to/wpa_host. Exit status = number of failures."""
import hashlib, hmac, os, struct, subprocess, sys
from cryptography.hazmat.primitives.keywrap import aes_key_wrap

host = subprocess.Popen([sys.argv[1] if len(sys.argv) > 1 else './wpa_host'], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, text=True)
fails = 0


def cmd(line):
    host.stdin.write(line + '\n')
    host.stdin.flush()
    return host.stdout.readline().split()


def check(name, cond):
    global fails
    print(('PASS ' if cond else 'FAIL ') + name)
    fails += not cond


def prf(k, label, data, n):
    out = b''
    for i in range((n + 19) // 20):
        out += hmac.new(k, label + b'\0' + data + bytes([i]), hashlib.sha1).digest()
    return out[:n]


def eapol(ki, replay, nonce, kd, kck=None, rsc=b'\0' * 8, ver=2):
    body = struct.pack('>BHH', 2, ki, 16) + struct.pack('>Q', replay) + nonce + b'\0' * 16 + rsc + b'\0' * 8 + \
        b'\0' * 16 + struct.pack('>H', len(kd)) + kd
    f = bytearray(struct.pack('>BBH', ver, 3, len(body)) + body)
    if kck:
        f[81:97] = hmac.new(kck, bytes(f), hashlib.sha1).digest()[:16]
    return bytes(f)


def parse(r):
    v = lambda k, i=1: r[r.index(k) + i]
    hx = lambda x: bytes.fromhex(x) if x != '-' else b''
    return int(v('act')), int(v('protect')), hx(v('tx')), hx(v('ptk')), int(v('gtk')), hx(v('gtk', 2)), hx(v('rsc'))


def mic_ok(f, kck):
    g = bytearray(f)
    g[81:97] = b'\0' * 16
    return hmac.new(kck, bytes(g), hashlib.sha1).digest()[:16] == f[81:97]


pmk = hashlib.pbkdf2_hmac('sha1', b'correct horse battery', b'meow', 4096, 32)
aa, spa = bytes.fromhex('c0c9e3a29916'), bytes.fromhex('0cc11957a25d')
ie = bytes.fromhex('30140100000fac040100000fac040100000fac020000')
anonce, snonce = os.urandom(32), os.urandom(32)
check('init', cmd('init %s %s %s %s %s' % (pmk.hex(), aa.hex(), spa.hex(), ie.hex(), snonce.hex()))[0] == 'ok')
ptk = prf(pmk, b'Pairwise key expansion', min(aa, spa) + max(aa, spa) + min(anonce, snonce) + max(anonce, snonce), 48)
kck, kek, tk = ptk[:16], ptk[16:32], ptk[32:48]

# message 1 (with a PMKID KDE, which the supplicant must ignore)
act, prot, tx, dptk, *_ = parse(cmd('rx ' + eapol(0x008a, 1, anonce, bytes.fromhex('dd14000fac04') + os.urandom(16)).hex()))
check('msg 1/4 -> reply', act == 1 and prot == 0)
check('PTK matches the authenticator', dptk == ptk)
ki, = struct.unpack('>H', tx[5:7])
check('msg 2/4 key info 0x010a', ki == 0x010a)
check('msg 2/4 replay counter 1', struct.unpack('>Q', tx[9:17])[0] == 1)
check('msg 2/4 SNonce', tx[17:49] == snonce)
check('msg 2/4 key data = our RSN IE', tx[99:] == ie and struct.unpack('>H', tx[97:99])[0] == len(ie))
check('msg 2/4 MIC', mic_ok(tx, kck))

gtk = os.urandom(16)
kd = ie + bytes.fromhex('dd16000fac010100') + gtk
kd += b'\xdd' + b'\0' * (-len(kd) % 8 - 1) if len(kd) % 8 else b''
rsc = bytes.fromhex('1122334455660000')
m3 = eapol(0x13ca, 2, anonce, aes_key_wrap(kek, kd), kck, rsc)
bad = bytearray(m3)
bad[90] ^= 1
act, *_ = parse(cmd('rx ' + bytes(bad).hex()))
check('msg 3/4 with a bad MIC ignored', act == 0)
act, prot, tx, dptk, gid, dgtk, drsc = parse(cmd('rx ' + m3.hex()))
check('msg 3/4 -> send 4/4, install PTK+GTK, done', act == 1 | 2 | 4 | 8 and prot == 0)
check('msg 4/4 key info 0x030a, replay 2, MIC', struct.unpack('>H', tx[5:7])[0] == 0x030a and
      struct.unpack('>Q', tx[9:17])[0] == 2 and mic_ok(tx, kck) and len(tx) == 99)
check('TK', dptk[32:] == tk)
check('GTK id 1 and key', gid == 1 and dgtk == gtk)
check('GTK RSC', drsc == rsc[:6])
act, *_ = parse(cmd('rx ' + m3.hex()))
check('replayed msg 3/4 (same counter) ignored', act == 0)
act, prot, tx, *_ = parse(cmd('rx ' + eapol(0x13ca, 3, anonce, aes_key_wrap(kek, kd), kck, rsc).hex()))
check('retransmitted msg 3/4 (new counter) -> 4/4 again, no key reinstall', act == 1)

gtk2 = os.urandom(16)
kd2 = bytes.fromhex('dd16000fac010200') + gtk2
g1 = eapol(0x1382, 4, b'\0' * 32, aes_key_wrap(kek, kd2), kck, bytes.fromhex('0700000000000000'))
act, prot, tx, dptk, gid, dgtk, drsc = parse(cmd('rx ' + g1.hex()))
check('group 1/2 -> send 2/2 protected, install GTK', act == 1 | 4 and prot == 1)
check('group 2/2 key info 0x0302, replay 4, MIC', struct.unpack('>H', tx[5:7])[0] == 0x0302 and
      struct.unpack('>Q', tx[9:17])[0] == 4 and mic_ok(tx, kck))
check('new GTK id 2', gid == 2 and dgtk == gtk2 and drsc == bytes.fromhex('070000000000'))
act, *_ = parse(cmd('rx ' + g1.hex()))
check('replayed group 1/2 ignored', act == 0)
act, *_ = parse(cmd('rx ' + eapol(0x0089, 9, anonce, b'').hex()))
check('TKIP descriptor (version 1) -> error', act == 16)
host.stdin.close()
print('%d failures' % fails)
sys.exit(fails)
