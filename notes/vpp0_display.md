# VPP0: showing decoded video on the panel

Status 2026-11-01: register sequence extracted from eCos (Haiku pass over `ecos.ann.dis`, 2026-11-01; the index-mode handoff at 0x52a7bc..0x52a820 re-read by hand and confirmed, the rest not yet). Not tried on the unit: every step needs someone looking at the panel, so it waits for the owner. Background: notes/h264_decoder.md section 5 (corrections below).

## Corrections to h264_decoder.md section 5

- The handoff body is entered at 0x52a6d4 (`bnez $r0, 0x52aaa6`, body when r0 == 0) from 0x5272bc in `f_005270de`; chain 0x52925e -> 0x5270de (BG2D rewrite) -> 0x52a6d4.
- `hal_vpp_init` clears bits 0..4 of 0x9C005788 before `|= 0xE0`; 0x9C0057BC is a read-modify-write (bits 3:2 = 1, 5:4 = 1, 1:0 = 0), not "= 0x14"; 0x9C005D84 also clears bits 9 and 12.

## hal_vpp_init (0x529d66..0x52a132), in order

1. Picture-quality / post blocks, purpose not decoded (read-modify-write unless "<-"): 0x9C006080 set b0, clr b1 b2 b3, set b4, clr b5; 0x9C0060A4 clr b0 b1; 0x9C0060A8 <- 0x10; bytes 0x9C0060AC / 0x9C0060AD <- 0x80; 0x9C006F80 <- 1; 0x9C006F84 <- (arg0 < arg1); 0x9C006FC8 <- 0x7F; 0x9C007000 set b0, b12 <- (arg0 < arg1); 0x9C007004 <- 0x3FF; 0x9C009A00 clr b2, set b1 b0; 0x9C009A04 clr b8 b0; 0x9C009B00 set b0, clr b2, set b4; 0x9C009B80 set b0, clr b4, set b5; 0x9C009C00 set b0 b2.
2. Scaler: 0x9C005D80 <- 1; 0x9C005D84 set b4 b6, clr b9 b12.
3. 0x9C005A80 set b0 b1, clr b6, set b8, clr b9..b13. Then 0x9C005B34..0x9C005B50 <- 0x1807, 0x6F0, 0x8FE, 0x4555, 0xA, 0x5014, 0xFF0C, 3 (looks like a YUV->RGB matrix; not decoded).
4. DDFCH: 0x9C005780 <- 1 (enable); 0x9C005784 <- 0x5E000; 0x9C00578C <- 0xD6D6D6; 0x9C005790 <- 0x3DEF.
5. 0x9C0057DC: set b0, set b4, clr b5, clr b8 b9, [17:16] <- 1, clr b24 b28 b29.
6. 0x9C0057BC [1:0] <- 0, [3:2] <- 1, [5:4] <- 1; 0x9C0057E0 <- arg0; 0x9C0057E4 clr b0..b3, set b4, clr b5 b6.
7. 0x9C0057F0 <- 0x80801021: test pattern ON (b0 enable, [5:4] pattern 2 = solid; bytes 1..3 Y/Cb/Cr). 0x80801011 = color bar.
8. 0x9C005788 clr b0..b4, set b5 b6 b7; 0x9C0057B0 / B4 / B8 set b0 and b16.

## Per frame

- Fetch geometry (0x52a1f8): 0x9C0057DC b8/b9 = H/2, H/4 predecimation; [17:16] = 1 (2 for the other format); b0 = struct+4; **0x9C0057D0 = pitch** (eCos ALIGN(width,128); ours is 1920, the decoder's BG2D pitch slot); 0x9C0057D8 = origin x [12:0], y [27:16]; 0x9C0057D4 = width [12:0], height [27:16].
- BG2D for display (in 0x5270de): byte 0x9C00A382 = ALIGN128(width)/16 (pitch slot 2), luma and chroma entries get gp = 2 and the picture's scan type; address field = phys >> 10. (We could instead point VPP0 at the decoder's own entries, gp 1, pitch 1920.)
- Handoff, index mode (0x52a7bc, confirmed): 0x9C005784 b28 <- 0, b20 <- 1; 0x9C005798 / 79C / 7A0 <- luma BG2D index, 0x9C0057A4 / 7A8 / 7AC <- chroma index (raw, not shifted). Physical mode instead: b20 <- 0 and `phys >> 10` in the same registers.
- Then: 0x9C005784 b10 <- struct+8, b21, b8 from a mode table (0x6BAEEC + mode*0x60); 0x9C0057DC b4, b5, b28, b29 from the mode (0xB = progressive: b5 = (s+4 > 0), others 0); 0x9C005788 b2 <- 0, b3, b4 <- 1; 0x9C0057BC fields from the table; 0x9C0057B0/B4/B8 set b0 b16; 0x9C005A80 b8, b9, b12 from the mode, **0x9C005A80 b0 <- 1 (commit)**; 0x9C005D84 b9.
- Scaler (0x529b06): factor `((in-1) << 22) / (out-1)`; 800 -> 1024: H = 0x31FC7F -> 0x9C005DCC (low 16) / 0x9C005DD0 (bits 24:16); 480 -> 600: V = 0x332DBA -> 0x9C005E04 / 0x9C005E08; filter index into 0x9C005DC8 [7:4] (3 for H <= 1.0) and 0x9C005E00 [7:4] (2 for V <= 1.0); 0x9C005D84 b0 b1 <- 1, b2 <- (in_w != out_w), b3 b4 <- (in_h != out_h).
- DMIX (0x523ab8): mode fields at 0x9C006C84 bits [2i+1:2i]; eCos sets both VPP0 and OSD0 to mode 0. Under U-Boot VPP0 is in mode 1. Meaning of the modes not decoded.

## Open

- 4:2:0 vs 4:2:2 and linear vs tiled: probably the mode table row (0x9C0057BC fields, 0x9C005784 b8/b10/b21) or the BG2D scan-type bits; not found in the fetch function.
- Whether U-Boot left VPP0 initialized at all (likely not).
- Mode meanings in DMIX; OSD0 alpha: our console is opaque, so video needs OSD0 off or an alpha-0 window.

## Plan (each step needs a look at the panel)

1. RAM-free: DDFCH pattern generator to color bar (0x9C0057F0 = 0x80801011) with hal_vpp_init's DDFCH and scaler parts, VPP0 mode 0 in DMIX, OSD0 off for 20 s, then restore. Proves VPP0 -> DMIX -> panel.
2. Pattern off, index mode on the decoder's slot of a decoded test picture (bars.h264), 800x480 -> 1024x600. Check the picture geometry and colors; try the scan-type and 0x9C0057DC [17:16] variants if it is scrambled.
3. Wire `vdec_feed` to hand each finished slot to VPP0 at vsync (double buffering through the four slots).

## First tries on the unit (2026-11-01, build 20261101-131847, owner watching)

- U-Boot leaves DDFCH partly set (780=1, 784=0xF000, 788=0xE0, 78C=0xD6D6D6), VSCL off, all four BG2D pitch slots 0x78 (1920). MOON0: every clock on except 0x9C000018 bit 11 (ID 0x5B), no resets held.
- DMIX 0x9C006C84 field 0 is VPP0 on L1 (U-Boot: 1 = transparent; SP7021 modes 0 blend, 1 transparent, 2 opaque). Field 1 = OSD0 (mode 0, blend). With field 0 = 2 and OSD0 off the panel goes **solid green** (YUV 0,0,0): the layer reaches the panel but carries no data.
- Still green with: hal_vpp_init's DDFCH, scaler and post blocks (0x9C006080, 0x9C006F80, 0x9C007000, 0x9C009Axx, 0x9C005Bxx); the color-bar pattern (0x80801011); a solid white pattern (0x8080EB21, eCos's format is 0x80801021); the decoder's picture in index mode; 0x9C005788 bit 4 + 0x9C005A80 commit; scaler bypassed (D84 = 0). So DDFCH itself produces nothing.
- eCos (Haiku pass): its per-frame VPP work runs only when its state word is 2, set by `f_005276d8` (callers 0x50d6e2, 0x50e3c2, 0x512f20, 0x513994, 0x513d46, 0x51459e), not decoded yet. Next: decode `f_005276d8` and the unresolved field-end callees (0x5272c2, 0x5272ec, 0x52acd4, 0x52abf0, 0x5268ee), and try clock ID 0x5B. Do not read 0x9C006000..0x9C00A000 blocks over HU1 wholesale: one of them raised a bus-error exception (crash vec7 at 0x18058aa).
