"""ap80211.c: our AP frames dissected by tshark (field values, no malformed/expert errors), and frame parsing."""
import os, subprocess, sys, tempfile

def main():
    exe = os.path.abspath(sys.argv[1])
    fails = 0
    def check(ok, what):
        nonlocal fails
        print(('PASS ' if ok else 'FAIL ') + what)
        fails += not ok
    with tempfile.TemporaryDirectory() as d:
        pcap = os.path.join(d, 'ap.pcap')
        out = subprocess.run([exe, pcap], capture_output=True, text=True).stdout
        fields = ['frame.number', 'wlan.fc.type_subtype', 'wlan.ssid', 'wlan.ds.current_channel', 'wlan.rsn.akms.type',
                  'wlan.rsn.pcs.type', 'wlan.rsn.gcs.type', 'wlan.ht.capabilities', 'wlan.fixed.aid', 'wlan.fixed.status_code',
                  'wlan.fixed.capabilities.privacy', 'wlan.wfa.ie.wme.subtype', '_ws.malformed', '_ws.expert.severity']
        cmd = ['tshark', '-r', pcap, '-T', 'fields', '-E', 'separator=|'] + sum([['-e', f] for f in fields], [])
        rows = [dict(zip(fields, l.split('|'))) for l in subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()]
        check(len(rows) == 5, 'tshark reads 5 frames')
        b, pr, au, asr, de = (rows + [{}] * 5)[:5]
        ssid = lambda r: bytes.fromhex(r['wlan.ssid']).decode() if all(ch in '0123456789abcdef' for ch in r.get('wlan.ssid', '')) and r.get('wlan.ssid') else r.get('wlan.ssid')
        check(b.get('wlan.fc.type_subtype') in ('0x0008', '0x08', '8') and ssid(b) == 'MiataAA', 'beacon with SSID MiataAA')
        check(b.get('wlan.ds.current_channel') == '6', 'beacon on channel 6')
        check(b.get('wlan.rsn.akms.type') == '2' and b.get('wlan.rsn.pcs.type') == '4' and b.get('wlan.rsn.gcs.type') == '4', 'RSN: PSK, CCMP/CCMP')
        check(b.get('wlan.fixed.capabilities.privacy') in ('1', 'True'), 'privacy bit set')
        check(b.get('wlan.ht.capabilities', '') != '' and b.get('wlan.wfa.ie.wme.subtype') == '1', 'HT capabilities + WMM parameter element')
        check(ssid(pr) == 'MiataAA' and pr.get('wlan.rsn.akms.type') == '2', 'probe response')
        check(au.get('wlan.fixed.status_code') == '0x0000' or au.get('wlan.fixed.status_code') == '0', 'auth response, status 0')
        check(asr.get('wlan.fixed.aid') in ('0x0001', '1', '0xc001') and asr.get('wlan.ht.capabilities', '') != '', 'assoc response AID 1 with HT')
        bad = [r['frame.number'] for r in rows if r.get('_ws.malformed') or 'error' in r.get('_ws.expert.severity', '').lower() or r.get('_ws.expert.severity', '') in ('8388608',)]
        check(not bad, 'no malformed frames or expert errors%s' % (' (frames %s)' % bad if bad else ''))
        exp = ['probe_any 1', 'probe_other 0', 'auth 2 alg 0', 'assoc 3 rsn 22 ht 1', 'deauth 4 reason 3']
        got = out.splitlines()
        for e in exp:
            check(e in got, 'parse: ' + e)
    print('%d failures' % fails)
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
