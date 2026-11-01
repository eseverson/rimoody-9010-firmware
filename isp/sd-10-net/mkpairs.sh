#!/bin/sh
# Build the two SD-card variants from NET.BIN (built on cachy) and the stage-1 scripts:
#   fin/    runs `ispsp update_finish` after `uartsp init 1 115200`, before `go ... main`
#   nofin/  does not
#   loop/   like nofin, but leaves ISP_UPDT.BIN on the card and sets the update register to QART, so when the unit
#           resets (the MCU does it ~150 s after U-Boot starts) xboot boots straight back into this card
# Each directory gets ISP_UPDT.BIN + ISPBOOOT.BIN + NET.BIN; the owner adds WIFI.TXT (see WIFI.TXT.example).
set -e
cd "$(dirname "$0")"
for v in fin nofin loop; do
    mkdir -p $v
    python3 -I ../sd-1-probe/mkisp_sd.py ../../dump1.bin $v --script stage1_$v.txt
    cp NET.BIN $v/
done
cmp fin/NET.BIN NET.BIN && cmp nofin/NET.BIN NET.BIN && cmp loop/NET.BIN NET.BIN
