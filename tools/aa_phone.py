#!/usr/bin/env python3
"""A stand-in for the phone in wireless Android Auto, to test the head unit's AA stack (firmware/aa.c over TCP)
without a phone: connects to the unit's AA port, answers the version request, runs the TLS handshake as the server
(the head unit is the TLS client), asks for service discovery, opens the video channel, sets it up and starts it, then
sends an H.264 Annex-B file as AA video messages, one access unit each, fragmented into 16 KB frames that are each
their own TLS record (as a phone does).

  aa_phone.py UNIT_IP STREAM.h264 [--port 5288] [--pics N] [--fps 30]

The certificate the phone presents is a throwaway one made with openssl at start (the head unit does not check it)."""
import argparse, os, socket, ssl, struct, subprocess, sys, tempfile, time

F_FIRST, F_LAST, F_WHOLE, F_CONTROL, F_ENC = 1, 2, 3, 4, 8
CH_CONTROL, CH_VIDEO = 0, 3


def varint(v):
    out = b''
    while True:
        b = v & 0x7f
        v >>= 7
        out += bytes([b | (0x80 if v else 0)])
        if not v:
            return out


def pb(*fields):   # (field, int) or (field, bytes)
    out = b''
    for f, v in fields:
        if isinstance(v, int):
            out += varint(f << 3) + varint(v)
        else:
            out += varint(f << 3 | 2) + varint(len(v)) + v
    return out


class Phone:
    def __init__(self, ip, port):
        self.s = socket.create_connection((ip, port), timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b''
        self.tls = None
        self.acks = 0
        self.log = []
        self.ciphers = None

    # ---- frames ----
    def recv_frame(self, timeout=10):
        self.s.settimeout(timeout)
        while True:
            if len(self.buf) >= 4:
                ch, fl, n = self.buf[0], self.buf[1], struct.unpack('>H', self.buf[2:4])[0]
                hdr = 8 if (fl & 3) == F_FIRST else 4
                if len(self.buf) >= hdr + n:
                    pay = self.buf[hdr:hdr + n]
                    self.buf = self.buf[hdr + n:]
                    return ch, fl, pay
            d = self.s.recv(65536)
            if not d:
                raise EOFError('head unit closed the connection')
            self.buf += d

    def recv_msg(self, timeout=10):
        """(ch, flags, msg id, body) of the next whole message, decrypted"""
        parts, ch0 = b'', None
        while True:
            ch, fl, pay = self.recv_frame(timeout)
            if fl & F_ENC:
                self.tin.write(pay)
                pay = b''
                while True:
                    try:
                        pay += self.tls.read(65536)
                    except ssl.SSLWantReadError:
                        break
            parts += pay
            if fl & F_LAST:
                return ch, fl, struct.unpack('>H', parts[:2])[0], parts[2:]

    def send(self, ch, flags, msg, enc):
        """one message (id + body bytes), split into frames of at most 16 KB, each frame its own TLS record"""
        chunks = [msg[i:i + 16384] for i in range(0, len(msg), 16384)] or [b'']
        out = b''
        for i, c in enumerate(chunks):
            fl = flags & F_CONTROL
            fl |= (F_FIRST if i == 0 else 0) | (F_LAST if i == len(chunks) - 1 else 0)
            if enc:
                fl |= F_ENC
                self.tls.write(c)
                c = self.tout.read()
            hdr = struct.pack('>BBH', ch, fl, len(c))
            if (fl & 3) == F_FIRST:
                hdr += struct.pack('>I', len(msg))
            out += hdr + c
        self.s.sendall(out)

    def send_msg(self, ch, mid, body=b'', control=False, enc=True):
        self.send(ch, F_CONTROL if control else 0, struct.pack('>H', mid) + body, enc)

    # ---- session ----
    def handshake(self, certdir, hu_root=None):
        ch, fl, pay = self.recv_frame()
        assert ch == 0 and pay[:2] == b'\x00\x01', 'expected the version request, got ch %d %s' % (ch, pay[:8].hex())
        print('version request: %s' % pay.hex())
        self.send(0, 0, b'\x00\x02' + struct.pack('>HHH', 1, 6, 0), enc=False)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(os.path.join(certdir, 'c.pem'), os.path.join(certdir, 'k.pem'))
        ctx.options &= ~ssl.OP_CIPHER_SERVER_PREFERENCE   # take the head unit's order, as a phone that honors it would
        if self.ciphers:
            ctx.set_ciphers(self.ciphers)
        if hu_root:   # check the head unit's certificate against the root a phone trusts
            ctx.verify_mode = ssl.CERT_REQUIRED
            ctx.load_verify_locations(hu_root)
        else:
            ctx.verify_mode = ssl.CERT_NONE
        self.tin, self.tout = ssl.MemoryBIO(), ssl.MemoryBIO()
        self.tls = ctx.wrap_bio(self.tin, self.tout, server_side=True)
        t0 = time.time()
        while True:
            ch, fl, pay = self.recv_frame()
            assert ch == 0 and pay[:2] == b'\x00\x03', 'expected SSL message 3, got %s' % pay[:4].hex()
            self.tin.write(pay[2:])
            done = False
            try:
                self.tls.do_handshake()
                done = True
            except ssl.SSLWantReadError:
                pass
            out = self.tout.read()
            if out:
                self.send(0, 0, b'\x00\x03' + out, enc=False)
            if done:
                break
        peer = self.tls.getpeercert()
        print('TLS up in %.2f s: %s; head unit certificate %s' % (time.time() - t0, self.tls.cipher(),
              'verified: %s' % dict(x[0] for x in peer['subject']).get('organizationName') if peer else 'not checked'))
        ch, fl, pay = self.recv_frame()
        print('auth complete: %s' % pay.hex())

    def wait_for(self, ch, mid, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            try:
                c, fl, m, body = self.recv_msg(max(0.1, end - time.time()))
            except socket.timeout:
                continue
            if c == ch and m == mid:
                return body
            self.other(c, m, body)
        raise TimeoutError('no message %#x on channel %d' % (mid, ch))

    def other(self, c, m, body):
        if c == CH_VIDEO and m == 0x8004:
            self.acks += 1
        else:
            self.log.append((c, m, body[:16].hex()))

    def drain(self, timeout=0.0):
        try:
            while True:
                c, fl, m, body = self.recv_msg(timeout)
                self.other(c, m, body)
        except (socket.timeout, TimeoutError, BlockingIOError):
            pass


def access_units(data):
    """split Annex-B into messages: the parameter sets first, then one access unit per picture"""
    starts, i = [], 0
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j < 0:
            break
        starts.append(j - 1 if j > 0 and data[j - 1] == 0 else j)
        i = j + 3
    nals = [data[s:(starts[k + 1] if k + 1 < len(starts) else len(data))] for k, s in enumerate(starts)]
    aus, cur, cfg = [], b'', b''
    for n in nals:
        h = n.index(b'\x00\x00\x01') + 3
        t = n[h] & 31
        if t in (7, 8):
            if cur:
                aus.append(cur)
                cur = b''
            cfg += n
            continue
        if t in (1, 5):
            first_mb_zero = n[h + 1] & 0x80   # ue(v) = 0 is the single bit '1'
            if first_mb_zero and cur:
                aus.append(cur)
                cur = b''
            if cfg:
                cur, cfg = cfg, b''
            cur += n
    if cur:
        aus.append(cur)
    return aus


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ip'); ap.add_argument('stream'); ap.add_argument('--port', type=int, default=5288)
    ap.add_argument('--pics', type=int, default=0); ap.add_argument('--fps', type=float, default=30)
    ap.add_argument('--ciphers', help='OpenSSL cipher list the phone side allows, e.g. ECDHE-RSA-AES256-GCM-SHA384')
    ap.add_argument('--loop', type=int, default=1, help='send the stream this many times (long runs)')
    ap.add_argument('--window', type=int, help='override the max unacked frames the head unit asked for (test only)')
    ap.add_argument('--hu-root', help="CA to verify the head unit's certificate with (the Google Automotive Link root)")
    a = ap.parse_args()
    aus = access_units(open(a.stream, 'rb').read())
    if a.pics:
        aus = aus[:a.pics]
    aus = aus * a.loop
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', d + '/k.pem', '-out',
                        d + '/c.pem', '-days', '1', '-subj', '/CN=aa-phone-test'], check=True, capture_output=True)
        p = Phone(a.ip, a.port)
        p.ciphers = a.ciphers
        p.handshake(d, a.hu_root)
    p.send_msg(CH_CONTROL, 5, pb((4, b'aa_phone.py'), (5, b'test')))
    sd = p.wait_for(CH_CONTROL, 6)
    print('service discovery response: %d bytes' % len(sd))
    p.send_msg(CH_VIDEO, 7, pb((1, 0), (2, CH_VIDEO)), control=True)
    print('channel open response: %s' % p.wait_for(CH_VIDEO, 8).hex())
    p.send_msg(CH_VIDEO, 0x8000, pb((1, 3)))   # media setup, type video
    cfg = p.wait_for(CH_VIDEO, 0x8003)
    window = cfg[3] if len(cfg) > 3 and cfg[2] == 0x10 else 1   # field 2 = max_unacked
    print('setup config: %s (max unacked %d)' % (cfg.hex(), window))
    if a.window:
        window = a.window
    p.send_msg(CH_VIDEO, 0x8001, pb((1, 1), (2, 0)))   # start: session 1, configuration 0
    t0, sent, nbytes, lat, inflight = time.time(), 0, 0, [], []
    for k, au in enumerate(aus):
        ts = int((time.time() - t0) * 1e6)
        inflight.append((time.time(), len(au)))
        p.send_msg(CH_VIDEO, 0x0000, struct.pack('>Q', ts) + au)
        sent += 1
        nbytes += len(au)
        while len(inflight) >= window:   # the head unit acks each frame; at most `window` unacked
            try:
                p.wait_for(CH_VIDEO, 0x8004, timeout=2)
                p.acks += 1
                t, n = inflight.pop(0)
                lat.append((time.time() - t, n))
            except TimeoutError:
                print('no ack for frame %d' % k)
                inflight.pop(0)
        due = t0 + (k + 1) / a.fps
        if due > time.time():
            time.sleep(due - time.time())
    dt = time.time() - t0
    p.drain(0.5)
    if lat:
        ls = sorted(x for x, _ in lat)
        big = [x for x, n in lat if n > 60000]
        print('send-to-ack ms: median %.1f, p90 %.1f, max %.1f; frames > 60 KB: %d, median %.1f ms; implied %.1f Mbit/s'
              % (ls[len(ls) // 2] * 1e3, ls[len(ls) * 9 // 10] * 1e3, ls[-1] * 1e3, len(big),
                 (sorted(big)[len(big) // 2] * 1e3) if big else 0, sum(n for _, n in lat) * 8 / sum(x for x, _ in lat) / 1e6))
    if lat and os.environ.get('AA_LAT'):
        for lo, hi in ((0, 2000), (2000, 8000), (8000, 20000), (20000, 60000), (60000, 1 << 30)):
            b = sorted(x for x, n in lat if lo <= n < hi)
            if b:
                print('  %6d..%-6d B: %3d frames, median %.1f ms' % (lo, min(hi, 999999), len(b), b[len(b) // 2] * 1e3))
    print('sent %d access units, %d bytes in %.2f s (%.1f fps, %.2f Mbit/s); %d acks; other messages: %s' %
          (sent, nbytes, dt, sent / dt, nbytes * 8 / dt / 1e6, p.acks, p.log[:8]))


if __name__ == '__main__':
    sys.exit(main())
