Test streams for the H.264 decoder bring-up (notes/h264_decoder.md): 800x480 baseline (CAVLC), one IDR frame, one slice, SPS + PPS + IDR, no SEI. `*.nv12` is ffmpeg's decode of each (reference output, 800x480 NV12).

Regenerate: `ffmpeg -f lavfi -i "color=c=gray:size=800x480:rate=30" -frames:v 1 -c:v libx264 -profile:v baseline -level 3.1 -x264-params "slices=1:keyint=1:cabac=0:bframes=0:ref=1:repeat-headers=1" -pix_fmt yuv420p -bsf:v filter_units=remove_types=6 -f h264 grey.h264` (bars: `smptebars=size=800x480:rate=30`).
