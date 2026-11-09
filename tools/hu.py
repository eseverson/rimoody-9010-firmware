#!/usr/bin/env python3
"""Talk to the head unit's U-Boot-side command server (isp/sd-10-net) over the LAN.

The payload joins the home Wi-Fi, takes a DHCP address, broadcasts `HU1 hello ...` to UDP 5152 every 2 s and serves
UDP 5151. Requests carry the shared token from WIFI.TXT on the card (`--token` or $HU_TOKEN).

  hu.py listen                       wait for the hello broadcast, print it
  hu.py ping
  hu.py cmd 'md.l 0x9c000000 0x20'   run a U-Boot command, print its console output
  hu.py read 0x9e000000 0x8000 -o rom.bin
  hu.py write 0x3000000 payload.bin  upload to RAM (verified by CRC32)
  hu.py run payload.bin [args..]     write to 0x3000000, sync caches, `go 0x3000000 args`
  hu.py log
  hu.py exec NET.BIN                 replace the running payload over Wi-Fi (WRITE to 0x3000000, CRC-checked, EXEC),
                                     then wait for the hello of the new build (bootstraps the pre-EXEC 20261031-1400
                                     build with TRAMP.BIN from the image's directory)
  hu.py mcu                          MCU state: ACC, version, MCU time/date, wall clock, last 20 frames
  hu.py mcu-tx d1 00                 send one MCU frame (payload bytes; ad.., 00, 11 00, e5 03 are refused)
  hu.py fb status|on|off|scale N     the text console on the display (OSD0); N = 1 or 2 for the log tail
  hu.py say TEXT...                  show a line on the console (and in the log)
  hu.py irq status|on [HZ]|off|test|trap
                                     interrupts (off at boot): `test` = resumable trap through the vector table,
                                     `trap` = fatal trap -> crash record at 0x01F00000 + reset (reported by the next run)
  hu.py settime                      set the payload's wall clock (and the MCU's RTC) to this host's local time
  hu.py persist NET.BIN              make it the card's /BOOT.BIN (what power-on runs), read back and CRC-checked
  hu.py fs status|ls|crc NAME|raw LBA  our own SD/FAT driver (sd.c + FatFs); rawtest/write/rm/load also exist

The target IP comes from --ip, $HU_IP, or the first hello heard. Stdlib only.
"""
import argparse, os, re, socket, struct, sys, time, zlib

PORT_CMD, PORT_HELLO = 5151, 5152
OP_PING, OP_CMD, OP_WRITE, OP_READ, OP_SYNC, OP_LOG = 1, 2, 3, 4, 5, 6
OP_MCU_TX, OP_MCU_STATE, OP_SETTIME, OP_EXEC, OP_IRQ, OP_FB, OP_SAY, OP_TOUCH, OP_RTOS, OP_USB, OP_FS, OP_AP, OP_VDEC = 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19
OP_AA, OP_VPP, OP_OSDVID, OP_UI, OP_AUDIO, OP_WIFI = 22, 24, 25, 26, 27, 28
TRAMP_ADDR = 0x02f00000

# Builds without OP_EXEC that `exec` can still replace: CRC32 of their first 4 KB at 0x1800000 -> addresses of their
# hooked[0], orig_putc[0], orig_puts[0] (from nm of that build's net.elf). The trampoline restores the console hook.
PRE_EXEC_BUILDS = {0x9f183f95: ('sd-10-net 20261031-1400', 0x01817610, 0x01817608, 0x01817600)}
CHUNK = 1200
LOAD_ADDR = 0x03000000


def listen(timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('', PORT_HELLO))
    s.settimeout(timeout)
    try:
        while True:
            data, (ip, _) = s.recvfrom(2048)
            if data.startswith(b'HU1 hello'):
                return ip, data.decode(errors='replace').strip()
    except socket.timeout:
        return None, None
    finally:
        s.close()


class Unit:
    def __init__(self, ip, token, timeout=2.0, retries=5):
        self.ip, self.timeout, self.retries = ip, timeout, retries
        self.token = token.encode()[:32].ljust(32, b'\0')
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.seq = int(time.time() * 1000) & 0xFFFFFFFF

    def call(self, op, payload=b''):
        """Send one request; collect reply fragments until `last`. Retransmits reuse the seq (server caches)."""
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        req = b'HU1' + self.token + struct.pack('<IB', self.seq, op) + payload
        for attempt in range(self.retries):
            self.sock.sendto(req, (self.ip, PORT_CMD))
            frags, last = {}, None
            try:
                while last is None or len(frags) <= last:
                    data, _ = self.sock.recvfrom(4096)
                    if len(data) < 11 or data[:3] != b'HU1':
                        continue
                    seq, rop, frag, is_last = struct.unpack_from('<IBHB', data, 3)
                    if seq != self.seq or rop != op | 0x80:
                        continue
                    frags[frag] = data[11:]
                    if is_last:
                        last = frag
                return b''.join(frags[i] for i in range(last + 1))
            except socket.timeout:
                if frags:
                    print('warning: incomplete reply (%d fragments), retrying' % len(frags), file=sys.stderr)
        raise TimeoutError('no reply from %s after %d tries' % (self.ip, self.retries))

    def ping(self):
        return self.call(OP_PING).decode(errors='replace')

    def cmd(self, line):
        return self.call(OP_CMD, line.encode() + b'\0').decode(errors='replace')

    def read(self, addr, length):
        out = bytearray()
        while len(out) < length:
            n = min(CHUNK, length - len(out))
            out += self.call(OP_READ, struct.pack('<IH', addr + len(out), n))
        return bytes(out)

    def write(self, addr, data):
        for off in range(0, len(data), CHUNK):
            part = data[off:off + CHUNK]
            r = self.call(OP_WRITE, struct.pack('<I', addr + off) + part)
            if len(r) > 4 and r.startswith(b'ERR'):
                raise IOError('%s (0x%x)' % (r.decode(errors='replace'), addr + off))
            crc = struct.unpack('<I', r[:4])[0]
            if crc != zlib.crc32(part) & 0xFFFFFFFF:
                raise IOError('CRC mismatch writing 0x%x' % (addr + off))

    def sync(self, addr, length):
        self.call(OP_SYNC, struct.pack('<II', addr, length))

    def log(self):
        return self.call(OP_LOG).decode(errors='replace')

    def u32(self, addr):
        return struct.unpack('<I', self.call(OP_READ, struct.pack('<IH', addr, 4)))[0]

    def send_once(self, op, payload=b'', wait=3.0):
        """One request, no retransmission (EXEC / bootstrap `go`: a retransmit could reach the new payload and run
        again). Returns the reply's first fragment or None."""
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        self.sock.sendto(b'HU1' + self.token + struct.pack('<IB', self.seq, op) + payload, (self.ip, PORT_CMD))
        self.sock.settimeout(wait)
        try:
            while True:
                data, _ = self.sock.recvfrom(4096)
                if data[:3] == b'HU1' and struct.unpack_from('<IB', data, 3) == (self.seq, op | 0x80):
                    return data[11:]
        except socket.timeout:
            return None
        finally:
            self.sock.settimeout(self.timeout)


def do_mic(u, a):
    """audio mic start|stop|status|rec SECONDS: the DSP's hands-free uplink (audio.c), AUDIO sub-ops 6..9. rec records
    on the unit into MIC.RAW (and RAM at MM_UPLOAD), polls until it is saved, and with --out fetches it from RAM."""
    sub = {'start': 6, 'stop': 7, 'status': 8, 'rec': 9}.get(a.value)
    if sub is None:
        sys.exit('audio mic start|stop|status|rec SECONDS [--out FILE]')
    if sub != 9:
        print(u.call(OP_AUDIO, bytes([sub])).decode(errors='replace'))
        return 0
    secs = max(1, min(60, int(float(a.db)) if a.db != '0' else 5))
    r = u.call(OP_AUDIO, bytes([9]) + struct.pack('<H', secs)).decode(errors='replace')
    print(r)
    if r.startswith('ERR'):
        return 1
    end = time.time() + secs + 20
    while time.time() < end:
        time.sleep(1.0)
        r = u.call(OP_AUDIO, bytes([8])).decode(errors='replace')
        if 'rec: recording' not in r:
            break
    print(r)
    m = re.search(r'rec: MIC\.RAW (\d+) bytes of \d+ \(save (-?\d+).*copy at (0x[0-9a-f]+)', r)
    if not m:
        print('recording did not finish', file=sys.stderr)
        return 1
    n, save, addr = int(m.group(1)), int(m.group(2)), int(m.group(3), 16)
    if a.out and n:
        data = u.read(addr, n)
        if a.out.lower().endswith('.wav'):
            import wave
            with wave.open(a.out, 'wb') as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(data)
        else:
            with open(a.out, 'wb') as f:
                f.write(data)
        print('saved %d bytes (%.2f s) to %s' % (n, n / 32000, a.out))
    return 0 if save == 0 and n else 1


def image_build(data):
    if len(data) < 0x100 or data[0x40:0x44] != b'HUNE' or struct.unpack_from('<I', data, 0x44)[0] != 1:
        sys.exit('not a payload image with a HUNE v1 header at 0x40')
    return data[0x48:0x80].split(b'\0')[0].decode()


def wait_hello(build, timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('', PORT_HELLO))
    end = time.time() + timeout
    try:
        while time.time() < end:
            s.settimeout(max(0.1, end - time.time()))
            try:
                data, (ip, _) = s.recvfrom(2048)
            except socket.timeout:
                break
            if data.startswith(b'HU1 hello') and b'build=' + build.encode() in data.split():
                return ip, data.decode(errors='replace').strip()
    finally:
        s.close()
    return None, None


def do_exec(u, path, addr, tramp_path, wait, dry=False):
    data = open(path, 'rb').read()
    build = image_build(data)
    pong = u.ping()
    print('running: %s' % pong)
    print('new image: %d bytes, build %s' % (len(data), build))
    if 'build=' in pong:
        if dry:
            print('dry run: would WRITE %d bytes to 0x%x and EXEC' % (len(data), addr))
            return 0
        u.write(addr, data)
        print('uploaded to 0x%x (CRC32 checked per chunk)' % addr)
        r = u.send_once(OP_EXEC, struct.pack('<II', addr, len(data)))
        print('exec: %s' % (r.decode(errors='replace') if r is not None else 'no reply (it may still have started)'))
        if r is not None and not r.startswith(b'ok'):
            return 1
    else:
        known = PRE_EXEC_BUILDS.get(zlib.crc32(u.read(0x01800000, 4096)) & 0xFFFFFFFF)
        if not known:
            sys.exit('the running payload has no EXEC op and is not a known build; deploy via the SD card')
        name, a_hooked, a_putc, a_puts = known
        dev, putc, puts = u.u32(a_hooked), u.u32(a_putc), u.u32(a_puts)
        hook = u.u32(dev + 0x20) if dev else 0
        print('bootstrap from %s: stdio dev 0x%x (putc now 0x%x), original putc 0x%x puts 0x%x' %
              (name, dev, hook, putc, puts))
        if not (0x07000000 <= dev < 0x08000000 and 0x01800000 <= hook < 0x01900000 and
                0x07000000 <= putc < 0x08000000 and 0x07000000 <= puts < 0x08000000):
            sys.exit('console hook state does not look as expected; not touching it')
        tramp = open(tramp_path, 'rb').read()
        if dry:
            print('dry run: would WRITE the image to 0x%x, %d trampoline bytes to 0x%x, SYNC both, then `go 0x%x %x %x '
                  '%x %x %x`' % (addr, len(tramp), TRAMP_ADDR, TRAMP_ADDR, addr, len(data), dev, putc, puts))
            return 0
        u.write(addr, data)
        u.write(TRAMP_ADDR, tramp)
        u.sync(addr, len(data))
        u.sync(TRAMP_ADDR, len(tramp))
        print('uploaded image to 0x%x and trampoline (%d bytes) to 0x%x' % (addr, len(tramp), TRAMP_ADDR))
        line = 'go 0x%x %x %x %x %x %x' % (TRAMP_ADDR, addr, len(data), dev, putc, puts)
        r = u.send_once(OP_CMD, line.encode() + b'\0', wait=2.0)
        print('sent `%s`%s' % (line, '' if r is None else ' -> ' + r.decode(errors='replace')))
    print('waiting up to %d s for the hello of build %s ...' % (wait, build))
    ip, hello = wait_hello(build, wait)
    print(hello if ip else 'no hello from the new build')
    return 0 if ip else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ip', default=os.environ.get('HU_IP'))
    ap.add_argument('--token', default=os.environ.get('HU_TOKEN'))
    ap.add_argument('--timeout', type=float, default=2.0)
    sub = ap.add_subparsers(dest='op', required=True)
    p = sub.add_parser('listen'); p.add_argument('--wait', type=float, default=180)
    sub.add_parser('ping')
    p = sub.add_parser('cmd'); p.add_argument('line', nargs='+')
    p = sub.add_parser('read'); p.add_argument('addr'); p.add_argument('length'); p.add_argument('-o', '--out')
    p = sub.add_parser('write'); p.add_argument('addr'); p.add_argument('file')
    p = sub.add_parser('run'); p.add_argument('file'); p.add_argument('args', nargs='*')
    p.add_argument('--addr', default=hex(LOAD_ADDR))
    sub.add_parser('log')
    p = sub.add_parser('exec'); p.add_argument('file'); p.add_argument('--addr', default=hex(LOAD_ADDR))
    p.add_argument('--tramp', help='TRAMP.BIN (default: next to the image)'); p.add_argument('--wait', type=float,
                                                                                            default=120)
    p.add_argument('--dry-run', action='store_true', help='read-only checks, print the plan')
    sub.add_parser('mcu')
    p = sub.add_parser('mcu-tx'); p.add_argument('bytes', nargs='+', help='payload bytes in hex, e.g. d1 00')
    sub.add_parser('settime')
    p = sub.add_parser('fs', help='our own SD/FAT driver: status, ls [dir], crc NAME, raw LBA, rawtest NAME SRC, write NAME ADDR LEN, rm NAME, load NAME ADDR MAX, put FILE [NAME] (upload, write, check both CRCs against FILE)')
    p.add_argument('what', choices=['status', 'ls', 'crc', 'raw', 'rawtest', 'write', 'rm', 'load', 'put']); p.add_argument('args', nargs='*')
    p = sub.add_parser('ap', help='access-point mode: ap [CHANNEL SSID PSK] (no args: status)')
    p.add_argument('channel', nargs='?', type=int); p.add_argument('ssid', nargs='?'); p.add_argument('psk', nargs='?')
    p = sub.add_parser('wifi', help='station Wi-Fi: status | avoid (skip the current BSS for 10 min and rescan; the unit drops off the network for a few seconds)')
    p.add_argument('what', nargs='?', default='status', choices=['status', 'avoid'])
    p = sub.add_parser('vdec', help='hardware H.264 decoder bring-up: status | init | bitread FILE.h264')
    p.add_argument('what', choices=['status', 'init', 'bitread', 'decode', 'spmp', 'prepare']); p.add_argument('file', nargs='?')
    p.add_argument('--pics', type=int, default=1, help='decode: pictures to decode (0: all)'); p.add_argument('--skip', type=lambda x: int(x, 0), default=0)
    p = sub.add_parser('aa', help='wireless Android Auto: status | start [PORT] | stop | connect PHONE_IP[:PORT] (cert/key from AACERT.PEM / AAKEY.PEM on the card)')
    p.add_argument('what', choices=['status', 'start', 'connect', 'stop', 'tap', 'hold', 'stats', 'vlog']); p.add_argument('target', nargs='?', default='5288'); p.add_argument('extra', nargs='*')
    p = sub.add_parser('vpp', help='VPP0 video layer tests: status | bars [SECS [SLOT]] | show [SECS [SLOT]] | restore | poke ADDR VAL | probe MASK [PATH] (notes/vpp0_display.md round 2; restore afterwards)')
    p.add_argument('what', choices=['status', 'bars', 'show', 'restore', 'poke', 'clock', 'linear', 'gclk', 'bisect', 'probe', 'bitscan', 'fetchtest', 'fresh', 'safe', 'replay', 'frame', 'hide', 'aavideo']); p.add_argument('args', nargs='*')
    p = sub.add_parser('osdvid', help='video through the OSD layer: status | show [SECS] | on | off | fast 0|1 | every MS/10')
    p.add_argument('what', choices=['status', 'show', 'on', 'off', 'fast', 'every']); p.add_argument('secs', nargs='?', type=int, default=0)
    p = sub.add_parser('ui', help='UI tests: tap X Y | button N [HOLD_MS] | drag X1 Y1 X2 Y2 | lights 0|1|2 (forced off/on, input) | shot OUT.png (the OSD0 framebuffer)')
    p.add_argument('what', choices=['tap', 'button', 'drag', 'lights', 'shot']); p.add_argument('args', nargs='*')
    p = sub.add_parser('audio', help='audio DSP (audio.c): status | init | tone HZ (0 off) | volume 0..40 | mute 0|1 | eq BAND DB | mic start|stop|status|rec SECONDS [--out FILE]')
    p.add_argument('what', choices=['status', 'init', 'tone', 'volume', 'mute', 'eq', 'mic']); p.add_argument('value', nargs='?', default='0')
    p.add_argument('db', nargs='?', default='0', help='eq: gain in dB (-12..12, 0.5 steps); mic rec: seconds (1..60)')
    p.add_argument('--out', help='mic rec: also fetch the capture (16 kHz mono s16le) into FILE; .wav gets a WAV header')
    p.add_argument('--peak', type=int, default=256, help='tone: peak level of 32767 (256 = -42 dBFS)')
    p.add_argument('--ms', type=int, default=2000, help='tone: stops by itself after this many ms (max 10000)')
    p = sub.add_parser('op', help='raw platform op: op N [HEXPAYLOAD] (e.g. op 20 = CCMP benchmark)')
    p.add_argument('num', type=int); p.add_argument('payload', nargs='?', default='')
    p = sub.add_parser('persist'); p.add_argument('file'); p.add_argument('--name', default='BOOT.BIN')
    p = sub.add_parser('fb'); p.add_argument('what', choices=['status', 'on', 'off', 'scale'])
    p.add_argument('n', nargs='?', type=int, default=2)
    p = sub.add_parser('say'); p.add_argument('text', nargs='+')
    p = sub.add_parser('irq'); p.add_argument('what', choices=['status', 'on', 'off', 'test', 'trap', 'hwwdt'])
    p.add_argument('hz', nargs='?', type=int, default=100)
    p = sub.add_parser('usb', help='own EHCI driver (builds >= 20261031-195)'); p.add_argument('what', choices=['status', 'compare', 'own', 'uboot']); p.add_argument('count', nargs='?', type=int, default=200)
    sub.add_parser('rtos', help='FreeRTOS task list (builds >= 20261031-1930)')
    p = sub.add_parser('touch', help='GT9xx touch (builds >= 20261031-1825)')
    p.add_argument('what', choices=['pins', 'probe', 'id', 'read', 'poll', 'watch', 'reset', 'events', 'release'])
    p.add_argument('args', nargs='*', help='read: REG16 LEN; watch: SECONDS; events: on|off (payload logs touches, builds >= 1840)')
    a = ap.parse_args()

    if a.op == 'listen':
        ip, hello = listen(a.wait)
        print(hello if ip else 'no hello within %.0f s' % a.wait)
        return 0 if ip else 1
    if not a.token:
        sys.exit('need --token or $HU_TOKEN (the token= line of WIFI.TXT)')
    ip = a.ip
    if not ip:
        ip, hello = listen(30)
        if not ip:
            sys.exit('no --ip and no hello broadcast heard')
        print(hello, file=sys.stderr)
    u = Unit(ip, a.token, a.timeout)

    if a.op == 'ping':
        print(u.ping())
    elif a.op == 'cmd':
        sys.stdout.write(u.cmd(' '.join(a.line)))
    elif a.op == 'read':
        data = u.read(int(a.addr, 0), int(a.length, 0))
        if a.out:
            open(a.out, 'wb').write(data)
            print('%d bytes -> %s' % (len(data), a.out))
        else:
            sys.stdout.buffer.write(data)
    elif a.op == 'write':
        data = open(a.file, 'rb').read()
        u.write(int(a.addr, 0), data)
        print('wrote %d bytes at %s' % (len(data), a.addr))
    elif a.op == 'run':
        addr, data = int(a.addr, 0), open(a.file, 'rb').read()
        u.write(addr, data)
        u.sync(addr, len(data))
        sys.stdout.write(u.cmd('go 0x%x %s' % (addr, ' '.join(a.args))))
    elif a.op == 'log':
        sys.stdout.write(u.log())
    elif a.op == 'exec':
        tramp = a.tramp or os.path.join(os.path.dirname(os.path.abspath(a.file)), 'TRAMP.BIN')
        return do_exec(u, a.file, int(a.addr, 0), tramp, a.wait, a.dry_run)
    elif a.op == 'mcu':
        sys.stdout.write(u.call(OP_MCU_STATE).decode(errors='replace'))
    elif a.op == 'mcu-tx':
        print(u.call(OP_MCU_TX, bytes.fromhex(''.join(a.bytes))).decode(errors='replace'))
    elif a.op == 'fb':
        code = {'off': 0, 'on': 1, 'status': 2, 'scale': 3}[a.what]
        print(u.call(OP_FB, bytes([code]) + (bytes([a.n]) if code == 3 else b'')).decode(errors='replace'))
    elif a.op == 'say':
        print(u.call(OP_SAY, ' '.join(a.text).encode()[:120]).decode(errors='replace'))
    elif a.op == 'irq':
        code = {'off': 0, 'on': 1, 'trap': 2, 'test': 3, 'status': 4, 'hwwdt': 5}[a.what]
        payload = bytes([code]) + (struct.pack('<H', a.hz) if code == 1 else b'')
        if code == 2:   # no retransmit: the unit resets right after replying
            r = u.send_once(OP_IRQ, payload)
            print(r.decode(errors='replace') if r is not None else 'no reply')
        else:
            print(u.call(OP_IRQ, payload).decode(errors='replace'))
    elif a.op == 'settime':
        t = time.localtime()
        print(u.call(OP_SETTIME, bytes([t.tm_year - 2000, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min,
                                        t.tm_sec])).decode(errors='replace'))
    elif a.op == 'usb':
        c = {'status': 0, 'compare': 1, 'own': 2, 'uboot': 3}[a.what]
        print(u.call(OP_USB, struct.pack('<BH', c, a.count)).decode(errors='replace'))
    elif a.op == 'fs' and a.what == 'put':
        if not 1 <= len(a.args) <= 2:
            sys.exit('fs put FILE [NAME]')
        return do_fs_put(u, a.args[0], a.args[1] if len(a.args) > 1 else os.path.basename(a.args[0]).upper())
    elif a.op == 'fs':
        c = {'status': 0, 'ls': 1, 'crc': 2, 'raw': 3, 'rawtest': 4, 'write': 5, 'rm': 6, 'load': 7}[a.what]
        args = a.args
        x = y = 0
        name = ''
        if a.what in ('ls', 'crc', 'rm'):
            name = args[0] if args else ''
        elif a.what == 'raw':
            x = int(args[0], 0)
        elif a.what == 'rawtest':   # name src: 512 bytes already in RAM at src
            name, x = args[0], int(args[1], 0)
        elif a.what in ('write', 'load'):   # name addr len
            name, x, y = args[0], int(args[1], 0), int(args[2], 0)
        r = u.call(OP_FS, struct.pack('<BII', c, x, y) + name.encode()).decode(errors='replace')
        print(r)
        if a.what == 'write' and 'MISMATCH' in r:
            return 1
    elif a.op == 'ap':
        if a.channel is None:
            print(u.call(OP_AP, b'').decode(errors='replace'))
        else:   # the unit leaves its current network; it resets back to station mode after 5 min without a station
            r = u.send_once(OP_AP, bytes([a.channel]) + a.ssid.encode() + b'\0' + a.psk.encode() + b'\0')
            print(r.decode(errors='replace') if r is not None else 'no reply')
    elif a.op == 'wifi':
        if a.what == 'avoid':
            r = u.send_once(OP_WIFI, b'\x01')
            print(r.decode(errors='replace') if r is not None else 'no reply')
        else:
            print(u.call(OP_WIFI, b'').decode(errors='replace'))
    elif a.op == 'vdec':
        if a.what in ('bitread', 'decode', 'prepare'):   # upload the access unit, then run it through the decoder
            data = open(a.file, 'rb').read()
            u.write(LOAD_ADDR, data)
            print(u.call(OP_VDEC, struct.pack('<BIIBB', {'bitread': 2, 'decode': 3, 'prepare': 5}[a.what], LOAD_ADDR, len(data), a.skip, a.pics)).decode(errors='replace'))
        else:
            print(u.call(OP_VDEC, bytes([{'status': 0, 'init': 1, 'spmp': 4}[a.what]])).decode(errors='replace'))
    elif a.op == 'aa':
        if a.what == 'start':
            payload = bytes([1]) + struct.pack('<H', int(a.target))
        elif a.what == 'connect':   # phone running Android Auto's "head unit server" (developer settings), port 5277
            host, _, port = a.target.partition(':')
            payload = bytes([2]) + bytes(int(x) for x in host.split('.')) + struct.pack('<H', int(port or 5277))
            if a.extra:   # VER_MINOR AUDIO_MASK [HD] (audio bit 0 media, 1 speech, 2 system, 0x80 microphone; HD 1 = 1280x720 with margins)
                payload += bytes(int(x, 0) for x in a.extra[:4])   # 4th: 1 = touchscreen declared as the 1024x600 panel
        elif a.what == 'stop':
            payload = bytes([3])
        elif a.what == 'hold':   # aa hold SECONDS
            payload = bytes([5, int(a.target) if a.target != '5288' else 60])
        elif a.what == 'stats':
            payload = bytes([6])
        elif a.what == 'vlog':
            payload = bytes([7])
        elif a.what == 'tap':   # aa tap X Y [HOLD_MS], panel coordinates (1024x600)
            v = [int(a.target)] + [int(e) for e in a.extra]
            payload = bytes([4]) + struct.pack('<HH', v[0], v[1]) + (struct.pack('<H', v[2]) if len(v) > 2 else b'') + bytes(v[3:4])   # [HOLD_MS [MOVES]]
        else:
            payload = bytes([0])
        print(u.call(OP_AA, payload).decode(errors='replace'))
    elif a.op == 'vpp':
        c = {'status': 0, 'bars': 1, 'show': 2, 'restore': 3, 'poke': 4, 'clock': 5, 'linear': 6, 'gclk': 7, 'bisect': 8, 'probe': 9, 'bitscan': 10, 'fetchtest': 11, 'fresh': 12, 'safe': 13, 'replay': 14, 'frame': 15, 'hide': 16, 'aavideo': 17}[a.what]
        if c == 4:
            payload = bytes([4]) + struct.pack('<II', int(a.args[0], 0), int(a.args[1], 0))
        elif c == 10:   # bitscan REG FIRSTBIT COUNT
            payload = bytes([10]) + struct.pack('<IBB', int(a.args[0], 0), int(a.args[1], 0), int(a.args[2], 0))
        elif c == 12:   # fresh FLAGS WHERE PITCH [ADDR:MASK:VAL ...] (vpp.c c == 12; one per full reset)
            payload = bytes([12, int(a.args[0], 0), int(a.args[1], 0)]) + struct.pack('<H', int(a.args[2], 0) if len(a.args) > 2 else 0)
            for pk in a.args[3:]:
                payload += struct.pack('<III', *[int(x, 0) for x in pk.split(':')])
        elif c == 15:   # frame [W H]: the last decoded picture through vpp_show_frame
            payload = bytes([15]) + (struct.pack('<HH', int(a.args[0], 0), int(a.args[1], 0)) if len(a.args) >= 2 else b'')
        elif c == 14:   # replay LISTADDR COUNT (list of <II addr, value> written beforehand)
            payload = bytes([14]) + struct.pack('<IH', int(a.args[0], 0), int(a.args[1], 0))
        elif c == 9:   # probe MASK [PATH]: 16-bit mask (vpp.c lists the bits), path for mask bit 4
            payload = bytes([9]) + struct.pack('<H', int(a.args[0], 0)) + bytes([int(a.args[1], 0) if len(a.args) > 1 else 2])
        else:
            payload = bytes([c] + [int(x, 0) for x in a.args])
        print(u.call(OP_VPP, payload).decode(errors='replace'))
    elif a.op == 'osdvid':
        c = {'status': 0, 'show': 1, 'on': 2, 'off': 3, 'fast': 4, 'every': 5}[a.what]
        print(u.call(OP_OSDVID, bytes([c, a.secs])).decode(errors='replace'))
    elif a.op == 'ui':
        if a.what == 'tap':
            print(u.call(OP_UI, bytes([0]) + struct.pack('<HH', int(a.args[0]), int(a.args[1]))).decode(errors='replace'))
        elif a.what == 'drag':
            print(u.call(OP_UI, bytes([2]) + struct.pack('<HHHH', *[int(v) for v in a.args[:4]])).decode(errors='replace'))
        elif a.what == 'lights':
            print(u.call(OP_UI, bytes([3, int(a.args[0])])).decode(errors='replace'))
        elif a.what == 'button':
            hold = int(a.args[1]) // 100 if len(a.args) > 1 else 1
            print(u.call(OP_UI, bytes([1, int(a.args[0]), max(1, min(255, hold))])).decode(errors='replace'))
        else:   # shot: read the ARGB8888 framebuffer (0x03400000, 1024x600, little-endian words) into a PNG
            from PIL import Image
            raw = u.read(0x03400000, 1024 * 600 * 4)
            Image.frombuffer('RGBA', (1024, 600), raw, 'raw', 'BGRA', 0, 1).convert('RGB').save(a.args[0])
            print('saved', a.args[0])
    elif a.op == 'audio' and a.what == 'mic':
        return do_mic(u, a)
    elif a.op == 'audio':
        c = {'status': 0, 'init': 1, 'tone': 2, 'volume': 3, 'mute': 4, 'eq': 5}[a.what]
        value, db = int(a.value), float(a.db)
        pl = bytes([c]) + (struct.pack('<HHH', value, a.peak, a.ms) if c == 2 else bytes([value]) if c in (3, 4)
                           else struct.pack('<Bb', value, int(round(db * 2))) if c == 5 else b'')
        print(u.call(OP_AUDIO, pl).decode(errors='replace'))
    elif a.op == 'op':
        print(u.call(a.num, bytes.fromhex(a.payload)).decode(errors='replace'))
    elif a.op == 'rtos':
        print(u.call(OP_RTOS, b'').decode(errors='replace'), end='')
    elif a.op == 'touch':
        code = {'pins': 0, 'probe': 1, 'id': 2, 'read': 3, 'poll': 4, 'watch': 4, 'reset': 5, 'events': 6, 'release': 9}[a.what]
        payload = bytes([code])
        if a.what == 'read':
            reg, n = int(a.args[0], 16), int(a.args[1]) if len(a.args) > 1 else 16
            payload += struct.pack('>HB', reg, n)
        if a.what == 'events':
            payload += bytes([0 if a.args and a.args[0] == 'off' else 1])
        if a.what == 'watch':   # the payload's own event poll would take the touches first
            print(u.call(OP_TOUCH, bytes([6, 0])).decode(errors='replace'), end='')
        if a.what != 'watch':
            print(u.call(OP_TOUCH, payload).decode(errors='replace'), end='')
            return 0
        end = time.time() + (float(a.args[0]) if a.args else 30)
        while time.time() < end:   # print only polls that carried a touch or key
            r = u.call(OP_TOUCH, payload).decode(errors='replace')
            if 'ready=1' in r or 'ERR' in r:
                print(time.strftime('%H:%M:%S'), ' '.join(l.strip() for l in r.splitlines() if not l.startswith('pins')),
                      flush=True)
            time.sleep(0.05)
    elif a.op == 'persist':
        return do_persist(u, a.file, a.name)
    return 0


def do_fs_put(u, path, name):
    """A local file onto the card under NAME through our FAT driver (HU1 op 17/5), checked end to end: the unit's RAM copy
    just before the write and the file read back afterwards must both match the local file. Issue #2: once the first
    word of the upload area changed between the upload and the card write, so a plain `write` + `fs write` stored a
    corrupt file without any error."""
    import re
    data = open(path, 'rb').read()
    want = zlib.crc32(data) & 0xffffffff
    u.write(LOAD_ADDR, data)
    r = u.call(OP_FS, struct.pack('<BII', 5, LOAD_ADDR, len(data)) + name.encode()).decode(errors='replace')
    print(r)
    m = re.search(r'ram crc ([0-9a-f]{8})', r)   # older images do not report it
    ram = int(m.group(1), 16) if m else None
    c = u.call(OP_FS, struct.pack('<BII', 2, 0, 0) + name.encode()).decode(errors='replace')
    m = re.search(r': (\d+) bytes, crc ([0-9a-f]{8})', c)
    card_len, card = (int(m.group(1)), int(m.group(2), 16)) if m else (None, None)
    if ram is not None and ram != want:
        verdict = 'FAILED: the RAM copy changed between the upload and the card write (ram %08x)' % ram
    elif card is None or card_len != len(data) or card != want:
        verdict = 'FAILED: the card holds %s bytes, crc %s' % (card_len, '%08x' % card if card is not None else '?')
    else:
        verdict = 'ok'
    print('%s on card: %s (file %d bytes, crc %08x)' % (name, verdict, len(data), want))
    return 0 if verdict == 'ok' else 1


def do_persist(u, path, name):
    """Make an image the one the NOR boot script loads at power-on: fatrm + fresh fatwrite of /BOOT.BIN on the card,
    then read it back and compare CRC32. U-Boot only (xboot never reads BOOT.BIN), one write, no overwrite in place."""
    import re
    img = open(path, 'rb').read()
    u.write(LOAD_ADDR, img)
    want = zlib.crc32(img) & 0xffffffff
    r = u.call(OP_FS, struct.pack('<BII', 8, LOAD_ADDR, len(img)) + name.encode()).decode(errors='replace')
    if 'unknown op' not in r and 'sub-op' not in r:   # our own FAT driver (write PERSIST.TMP, verify, rename)
        m = re.search(r'crc ([0-9a-f]{8})', r)
        ok = 'persisted' in r and bool(m) and int(m.group(1), 16) == want
        print(r)
        print('%s on card: %s (crc %s, image %08x)' % (name, 'persisted' if ok else 'FAILED', m.group(1) if m else '?', want))
        return 0 if ok else 1
    print(u.cmd('fatrm mmc 0 %s; fatwrite mmc 0 0x%x %s 0x%x' % (name, LOAD_ADDR, name, len(img))).strip())
    check = LOAD_ADDR + 0x200000
    u.cmd('mw.l 0x%x 0 0x%x' % (check, (len(img) + 3) // 4))
    out = u.cmd('fatload mmc 0 0x%x /%s; crc32 0x%x 0x%x' % (check, name, check, len(img)))
    m = re.search(r'==>\s*([0-9a-f]{8})', out)
    ok = bool(m) and int(m.group(1), 16) == want
    print('%s on card: %s (crc %s, image %08x)' % (name, 'persisted' if ok else 'MISMATCH', m.group(1) if m else '?',
                                                   want))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
