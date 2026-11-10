#!/usr/bin/env python3
"""Checks tools/mkrelease.py's installer script and its checker without a flash dump: the script starts the firmware
with the installer-boot marker, passes the flash-command check, builds a script image U-Boot accepts within the size
limit, and the checker still refuses what could write the flash. Exit status = failures."""
import os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import mkrelease as mr  # noqa: E402

fails = 0


def check(name, cond):
    global fails
    print(('PASS ' if cond else 'FAIL ') + name)
    fails += not cond


def refused(text):
    try:
        mr.check_script_text(text)
    except SystemExit:
        return True
    return False


lines = mr.SCRIPT.splitlines()
check('the release script passes the flash-command check', not refused(mr.SCRIPT))
go = [l for l in lines if 'go 0x1800000' in l]
check('one line starts the firmware', len(go) == 1)
check('hello first, then main with the installer marker',
      go and 'go 0x1800000 hello; go 0x1800000 main isp; fi' in go[0])
check('the marker appears nowhere else', mr.SCRIPT.count('main isp') == 1)
check('hu_src isp is still set before the go lines', go and 'setenv hu_src isp' in go[0] and
      go[0].index('setenv hu_src isp') < go[0].index('go 0x1800000 hello'))
first_go = lines.index(go[0]) if go else -1
check('the installer files are deleted before the firmware starts',
      all(any(('fatrm $isp_if $isp_dev ' + n) == l for l in lines[:first_go]) for n in mr.ISP_NAMES))
check('QART cleared and the update marked done before anything else',
      lines[:2] == ['setenv isp_all_or_update_done 1', 'mw.l 0x9c00317c 0 1'])
check('ends in reset', lines[-1] == 'reset')

img = mr.mkscript(mr.SCRIPT, 'release installer', stamp=0)
check('script image within the limit (%d <= %d bytes)' % (len(img), mr.STAGE1_MAX), len(img) <= mr.STAGE1_MAX)
check('U-Boot source accepts the script image', mr.check_script(img) is None)
check('the image is reproducible', img == mr.mkscript(mr.SCRIPT, 'release installer', stamp=0))

for bad in ('mw.l 0x01000000 1', 'mw.l 0x98e00000 0', 'sf erase 0 10000', 'saveenv', 'run bootcmd', 'env save',
            'ispsp all mmc 0', 'cp.b 0x1800000 0x98000000 100', 'fatwrite mmc 0 0x1800000 X 10',
            'if true; then saveenv; fi'):
    check('checker refuses %r' % bad, refused(bad))
check('checker allows ispsp update_finish', not refused('ispsp update_finish'))

# a firmware image without the old "no config (WIFI.TXT)" stop: WIFI.TXT is optional and INSTALL.txt says so
fake = bytearray(0x200)
fake[0x40:0x44] = b'HUNE'
struct.pack_into('<I', fake, 0x44, 1)
fake[0x48:0x48 + 15] = b'20261109-120000'
check('check_net_bin reads the build id', mr.check_net_bin(bytes(fake)) == '20261109-120000')
net_c = open(os.path.join(ROOT, 'firmware', 'net.c')).read()
check('net.c no longer stops without WIFI.TXT (mkrelease then writes "optional")', 'no config (WIFI.TXT)' not in net_c)
inst = mr.INSTALL.format(title='t', rule='=', url='u', wifi=mr.WIFI_OPTIONAL)
check('INSTALL.txt: WIFI.TXT optional, car network and token explained',
      'Optional: WIFI.TXT' in inst and 'car network' in inst and 'AP.TXT' in inst and 'token' in inst)
check('INSTALL.txt: the boot path is set up by itself', 'by itself' in inst and 'Installing the boot path' in inst)
check('INSTALL.txt is ASCII (written CRLF for Windows)', inst.isascii())
check('WIFI.TXT.example says the file is optional', 'optional' in mr.WIFI_EXAMPLE)

print('%d failures' % fails)
sys.exit(fails)
