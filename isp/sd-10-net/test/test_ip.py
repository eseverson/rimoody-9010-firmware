#!/usr/bin/env python3
"""Drive ip.c (through ip_host) as the LAN would: a DHCP server, the owner's desktop sending ARP, ping and HU1 requests.
Checks every IPv4/UDP/ICMP checksum independently. Usage: test_ip.py path/to/ip_host. Exit status = failures."""
import struct, subprocess, sys, zlib

HOST = sys.argv[1] if len(sys.argv) > 1 else './ip_host'
fails = 0
MAC = bytes.fromhex('0cc11957a25d')
DESK = bytes.fromhex('3c7c3fb00102')
TOKEN = b'sekrit-token'


def check(name, cond):
    global fails
    print(('PASS ' if cond else 'FAIL ') + name)
    fails += not cond


class Dev:
    def __init__(self, static_ip=0):
        self.p = subprocess.Popen([HOST], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.do('init %s %s %08x' % (MAC.hex(), TOKEN.hex(), static_ip))

    def do(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        out = []
        while True:
            l = self.p.stdout.readline().split()
            if l[0] == 'end':
                return out
            out.append(l)

    def rx(self, src, dst, typ, payload):
        return self.do('rx %s %s %04x %s' % (src.hex(), dst.hex(), typ, payload.hex()))


def csum(b):
    if len(b) % 2:
        b += b'\0'
    s = sum(struct.unpack('>%dH' % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


def ipv4(src, dst, proto, l4, ident=1):
    h = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20 + len(l4), ident, 0, 64, proto, 0, src, dst)
    return h[:10] + struct.pack('>H', csum(h)) + h[12:] + l4


def udp(src, dst, sport, dport, data):
    u = struct.pack('>HHHH', sport, dport, 8 + len(data), 0) + data
    c = csum(src + dst + struct.pack('>BBH', 0, 17, len(u)) + u)
    return ipv4(src, dst, 17, u[:6] + struct.pack('>H', c or 0xffff) + u[8:])


def parse_tx(l):
    """-> (dstmac, type, ip_header_ok, src, dst, proto, l4 payload) and checksum verdicts"""
    dst, typ, p = bytes.fromhex(l[1]), int(l[2], 16), bytes.fromhex(l[3])
    if typ != 0x0800:
        return dst, typ, p, None
    ihl = (p[0] & 15) * 4
    tot = struct.unpack('>H', p[2:4])[0]
    ok = csum(p[:ihl]) == 0 and tot == len(p)
    l4 = p[ihl:tot]
    if p[9] == 17:
        ok &= csum(p[12:20] + struct.pack('>BBH', 0, 17, len(l4)) + l4) == 0
        ok &= struct.unpack('>H', l4[4:6])[0] == len(l4)
    elif p[9] == 1:
        ok &= csum(l4) == 0
    return dst, typ, p, ok


IP = lambda s: bytes(int(x) for x in s.split('.'))
dev = Dev()
out = dev.do('up')
dst, typ, p, ok = parse_tx(out[0])
check('DHCP DISCOVER broadcast, checksums', dst == b'\xff' * 6 and typ == 0x0800 and ok and p[12:16] == b'\0' * 4)
d = p[28:]
xid = d[4:8]
check('DISCOVER: op 1, chaddr, cookie, option 53=1', d[0] == 1 and d[28:34] == MAC and d[236:240] == bytes.fromhex(
    '63825363') and d[240:243] == b'\x35\x01\x01' and len(d) >= 300)


def dhcp_reply(mtype, yi, extra=b''):
    b = bytearray(240)
    b[0], b[1], b[2] = 2, 1, 6
    b[4:8] = xid
    b[16:20] = yi
    b[28:34] = MAC
    b[236:240] = bytes.fromhex('63825363')
    opts = bytes([53, 1, mtype, 54, 4]) + IP('192.168.1.1') + bytes([1, 4, 255, 255, 255, 0, 3, 4]) + \
        IP('192.168.1.1') + bytes([51, 4]) + struct.pack('>I', 7200) + extra + b'\xff'
    return udp(IP('192.168.1.1'), IP('255.255.255.255'), 67, 68, bytes(b) + opts)


out = dev.rx(bytes.fromhex('c0c9e3a29916'), b'\xff' * 6, 0x0800, dhcp_reply(2, IP('192.168.1.77')))
dst, typ, p, ok = parse_tx(out[0])
d = p[28:]
check('OFFER -> REQUEST with 50=offered, 54=server', ok and d[240:243] == b'\x35\x01\x03' and
      b'\x32\x04' + IP('192.168.1.77') in d and b'\x36\x04' + IP('192.168.1.1') in d)
out = dev.rx(bytes.fromhex('c0c9e3a29916'), b'\xff' * 6, 0x0800, dhcp_reply(5, IP('192.168.1.77')))
garp = [l for l in out if l[0] == 'tx' and l[2] == '0806']
ev = [l for l in out if l[0] == 'event']
check('ACK -> bound 192.168.1.77/24 router .1', ev and ev[0][1:] == ['c0a8014d', 'ffffff00', 'c0a80101', '1'])
check('gratuitous ARP', garp and bytes.fromhex(garp[0][3])[24:28] == IP('192.168.1.77'))

out = dev.do('time 2500')
out = dev.do('poll')
dst, typ, p, ok = parse_tx(out[0])
txt = p[28:].decode()
check('hello broadcast to 255.255.255.255:5152 from :5151', ok and dst == b'\xff' * 6 and p[16:20] == b'\xff' * 4 and
      p[20:24] == struct.pack('>HH', 5151, 5152))
check('hello text with build id', txt == 'HU1 hello ip=192.168.1.77 mac=0c:c1:19:57:a2:5d up=2500 bssid=c0:c9:e3:a2:99:16 '
      'build=20261031-120000')

arp = struct.pack('>HHBBH', 1, 0x0800, 6, 4, 1) + DESK + IP('192.168.1.2') + b'\0' * 6 + IP('192.168.1.77')
out = dev.rx(DESK, b'\xff' * 6, 0x0806, arp)
r = bytes.fromhex(out[0][3])
check('ARP reply to the desktop', out[0][1] == DESK.hex() and r[6:8] == b'\0\x02' and r[8:14] == MAC and
      r[14:18] == IP('192.168.1.77') and r[18:24] == DESK and r[24:28] == IP('192.168.1.2'))

icmp = bytearray(b'\x08\x00\x00\x00\x12\x34\x00\x01' + bytes(range(56)))
icmp[2:4] = struct.pack('>H', csum(bytes(icmp)))
out = dev.rx(DESK, MAC, 0x0800, ipv4(IP('192.168.1.2'), IP('192.168.1.77'), 1, bytes(icmp)))
dst, typ, p, ok = parse_tx(out[0])
check('ICMP echo reply, checksums', ok and dst == DESK and p[20] == 0 and p[24:] == bytes(icmp[4:]) and
      p[16:20] == IP('192.168.1.2'))


def hu(op, payload=b'', seq=1, token=TOKEN):
    req = b'HU1' + token.ljust(32, b'\0') + struct.pack('<IB', seq, op) + payload
    out = dev.rx(DESK, MAC, 0x0800, udp(IP('192.168.1.2'), IP('192.168.1.77'), 40000, 5151, req))
    frags, good = [], True
    for l in out:
        if l[0] != 'tx':
            continue
        dst, typ, p, ok = parse_tx(l)
        good &= ok and dst == DESK and p[16:20] == IP('192.168.1.2') and p[20:24] == struct.pack('>HH', 5151, 40000)
        d = p[28:]
        good &= d[:3] == b'HU1' and struct.unpack('<I', d[3:7])[0] == seq and d[7] == op | 0x80
        frags.append((struct.unpack('<H', d[8:10])[0], d[10], d[11:]))
    good &= [f[0] for f in frags] == list(range(len(frags))) and all(f[1] == (i == len(frags) - 1)
                                                                      for i, f in enumerate(frags))
    good &= all(len(f[2]) <= 1200 for f in frags)
    return good, len(frags), b''.join(f[2] for f in frags)


g, n, data = hu(1)
check('PING reply, checksums, framing', g and n == 1 and data == b'pong up=2500 hb=4 mcurx=5 rssi=-60 ip=192.168.1.77')
g, n, data = hu(1, seq=2, token=b'wrong')
check('bad token dropped silently', n == 0)
g, n, data = hu(2, b'big', seq=3)
check('CMD big -> 5 fragments, reassembled', g and n == 5 and data.startswith(b'ran:big run#1\n') and len(data) == 5014)
g2, n2, data2 = hu(2, b'big', seq=3)
check('duplicate seq -> cached reply resent, not re-run', g2 and n2 == 5 and data2 == data)
g, n, data = hu(2, b'version', seq=4)
check('next seq runs again', g and data == b'ran:version run#2\n')
blob = bytes(range(256)) * 4 + b'xyz'
g, n, data = hu(3, struct.pack('<I', 0x02000102) + blob, seq=5)
check('WRITE -> CRC32 of the bytes written', g and data == struct.pack('<I', zlib.crc32(blob)))
g, n, data = hu(4, struct.pack('<IH', 0x02000100, 1200), seq=6)
check('READ back 1200 bytes', g and data == (b'\0\0' + blob).ljust(1200, b'\0'))
g, n, data = hu(4, struct.pack('<IH', 0x02000101, 4), seq=7)
check('READ unaligned -> ERR', g and data.startswith(b'ERR'))
g, n, data = hu(3, struct.pack('<I', 0x01800000) + b'x', seq=8)
check('WRITE outside the allowed range -> ERR refused', g and data == b'ERR write refused')
g, n, data = hu(5, struct.pack('<II', 0x02000000, 4096), seq=9)
check('SYNC -> ok', g and data == b'ok')
g, n, data = hu(6, seq=10)
check('LOG -> 64 KB in 55 fragments', g and n == 55 and len(data) == 65536 and data[:3] == bytes([0, 7, 14]))
g, n, data = hu(7, b'\xd1\x00', seq=11)
check('op 7 handed to plat_op', g and data == b'op7:d100')
g, n, data = hu(0x42, seq=12)
check('unknown op -> ERR', g and data.startswith(b'ERR unknown op'))
check('plat_cmd ran exactly twice, sync once', dev.do('stat')[0][1:] == ['runs', '2', 'syncs', '1'])

dev2 = Dev(static_ip=0xc0a80163)
dev2.do('up')
dev2.do('time 5000')
check('no fallback before 10 s', not [l for l in dev2.do('poll') if l[0] == 'event'])
dev2.do('time 10500')
ev = [l for l in dev2.do('poll') if l[0] == 'event']
check('static fallback 192.168.1.99 after 10 s', ev and ev[0][1:] == ['c0a80163', 'ffffff00', 'c0a80101', '2'])
print('%d failures' % fails)
sys.exit(fails)
