#!/bin/bash
# Hardware H.264 decoder regression (notes/h264_decoder.md): decode each test stream on the unit, read back the last
# picture and compare it with ffmpeg's decode of the same frame. Needs the unit online and tools/hu.env.
#   tools/vdec_regress.sh [OUTDIR]
set -u
cd "$(dirname "$0")/.." && source tools/hu.env
T=firmware/test/h264
O=${1:-$(mktemp -d)}; mkdir -p "$O"
hu() { python3 tools/hu.py "$@" 2>&1 | grep -v "HU1 hello"; }
hu vdec init >/dev/null
SCAN=${VDEC_SCAN:-1}   # frame layout (vdec.c vdec_scan_type; 1 = eCos/VPP0 layout, the default)
hu op 19 060$SCAN >/dev/null
fail=0
# stream:pictures
for t in noise:1 bars:1 grey:1 ip1:10 ip3:12 slices4_nodeblock:2 slices4:4 base_p:20; do
    f=$T/${t%:*}.h264 n=${t#*:}
    ffmpeg -v error -y -i "$f" -f rawvideo -pix_fmt nv12 "$O/ref.nv12" || { echo "${t%:*}: ffmpeg failed"; fail=1; continue; }
    r=$(hu vdec decode "$f" --pics "$n")
    read -r L C <<< "$(echo "$r" | sed -n 's/.*luma \([0-9a-f]*\) chroma \([0-9a-f]*\).*/\1 \2/p')"
    if [ -z "$L" ]; then echo "${t%:*}: $r"; fail=1; continue; fi
    rm -f "$O/l.bin" "$O/c.bin"
    hu read "0x$L" 0xF0000 -o "$O/l.bin" >/dev/null
    hu read "0x$C" 0x78000 -o "$O/c.bin" >/dev/null
    printf '%-18s %-40s ' "${t%:*} x$n" "$(echo "$r" | grep -o 'mean [0-9]*, max [0-9]*') us"
    python3 tools/mfd_detile.py "$O/l.bin" "$O/ref.nv12" --chroma "$O/c.bin" --frame $((n - 1)) --scan $SCAN | tr '\n' ' '
    [ "${PIPESTATUS[0]}" = 0 ] || fail=1
    echo
done
exit $fail
