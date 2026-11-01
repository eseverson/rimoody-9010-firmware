Test streams for the H.264 decoder bring-up (notes/h264_decoder.md): 800x480 baseline (CAVLC), one IDR frame, one slice, SPS + PPS + IDR, no SEI. `*.nv12` is ffmpeg's decode of each (reference output, 800x480 NV12).

Regenerate: `ffmpeg -f lavfi -i "color=c=gray:size=800x480:rate=30" -frames:v 1 -c:v libx264 -profile:v baseline -level 3.1 -x264-params "slices=1:keyint=1:cabac=0:bframes=0:ref=1:repeat-headers=1" -pix_fmt yuv420p -bsf:v filter_units=remove_types=6 -f h264 grey.h264` (bars: `smptebars=size=800x480:rate=30`).

Multi-picture streams (testsrc2, 800x480 baseline CAVLC, x264 with repeat-headers, SEI removed; `tools/vdec_regress.sh` decodes each on the unit and compares the last picture with ffmpeg):

- `ip1.h264`: 10 pictures, IDR + 9 P, one slice, one reference (`slices=1:keyint=60:ref=1`).
- `ip3.h264`: 12 pictures, three references (`ref=3`).
- `slices4.h264`: 4 pictures, 4 slices each, constant QP (`slices=4:qp=26:aq-mode=0`).
- `slices4_nodeblock.h264`: 2 pictures, 4 slices, deblocking off (`no-deblock=1`).
- `base_p.h264`: 20 pictures, 4 slices, two references, per-slice QPs, an IDR at picture 10.

