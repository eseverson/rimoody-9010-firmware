#!/usr/bin/env bash
# One `hu.py vpp fresh` test per full chip reset (a DDFCH AXI error is sticky until reset; notes/vpp0_display.md).
# Usage: tools/vpp_fresh.sh NET.BIN LOG 'label|fresh args[|pre]' ...   (pre: extra hu.py command run before the test, e.g. "vdec init")
set -u
cd "$(dirname "$0")/.."
source tools/hu.env
IMG=$1 LOG=$2; shift 2
IP=${HU_IP:-192.168.1.142}
hu() { python3 tools/hu.py --ip "$IP" "$@"; }
want=$(strings -n 15 "$IMG" | grep -m1 -oE '20[0-9]{6}-[0-9]{6}')
wait_up() {   # wait until the unit answers a ping (up to 120 s)
    for _ in $(seq 60); do hu --timeout 1 ping >/dev/null 2>&1 && return 0; sleep 2; done; return 1
}
for v in "$@"; do
    IFS='|' read -r label args pre <<<"$v"
    echo "=== $label: fresh $args ${pre:+(pre: $pre)} $(date +%T)" | tee -a "$LOG"
    hu irq hwwdt >/dev/null 2>&1; sleep 10
    wait_up || { echo "unit did not come back" | tee -a "$LOG"; exit 1; }
    hu exec "$IMG" 2>&1 | tail -1 | tee -a "$LOG"
    hu ping 2>&1 | grep -q "build=$want" || { echo "not running $want" | tee -a "$LOG"; exit 1; }
    [ -n "${pre:-}" ] && hu $pre 2>&1 | tail -1 | tee -a "$LOG"
    hu --timeout 8 vpp fresh $args 2>&1 | sed 's/; /;\n  /g' | tee -a "$LOG"
done
