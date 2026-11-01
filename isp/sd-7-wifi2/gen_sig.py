#!/usr/bin/env python3
"""Emit ub_sig.h: the first instruction word of each U-Boot entry point wifi2.c calls, taken from the flash dump.

The stock U-Boot uImage sits at flash 0x8A40E0; xboot copies its body (file offset +0x40) to RAM 0xF00040 and U-Boot
runs there without relocating. wifi2.c compares these words with RAM before calling anything."""
import re, struct, sys

FLASH_UBOOT_BODY = 0x8A40E0 + 0x40
RAM_BASE = 0xF00040

dump, src, out = sys.argv[1:4]
d = open(dump, 'rb').read()
names = ['UB_PARSE_STRING_OUTER', 'UB_GET_TIMER', 'UB_UDELAY', 'UB_USB_GET_DEV_INDEX', 'UB_USB_CONTROL_MSG',
         'UB_USB_BULK_MSG']
defs = dict(re.findall(r'#define (UB_\w+)\s+(0x[0-9a-fA-F]+)', open(src).read()))
rows = []
for n in names:
    a = int(defs[n], 16)
    w = struct.unpack_from('<I', d, FLASH_UBOOT_BODY + a - RAM_BASE)[0]
    rows.append('    { 0x%08x, 0x%08x },   /* %s */' % (a, w, n))
open(out, 'w').write('static const u32 ub_sig[][2] = {\n%s\n};\n' % '\n'.join(rows))
