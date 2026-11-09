<!-- eCos H.264 parser struct offsets mapped to H.264 syntax (agent trace 2026-11-01, see notes/h264_decoder.md section 3.2). -->
# SPMP slice command fields: H.264 meaning (eCos header parsers)

Source: `ecos.ann.dis`, parser state machine 0x43b394..0x43ebc8 (each state stores the next state function at ctx+8), command builder `H264_SpmpProgramStoreReg` 0x43a602. NDS32 note: `slt/slti` are **unsigned**, `slts/sltsi` signed; `lhi/lbi/lb/lh` zero-extend (so every 16-bit command carries the low 16 bits, two's complement for negatives).

Test stream values checked against `firmware/test/h264/{grey,bars}.h264` with `ffmpeg -bsf:v trace_headers`: profile 66, nal_ref_idc 3, nal_unit_type 5, slice_type 7, first_mb 0, frame_num 0, idr_pic_id 0, slice_qp_delta -18, pic_init_qp_minus26 -3, chroma_qp_index_offset -2, no transform_8x8/second offset in PPS (so second = first), num_ref_idx_l0/l1_default_active_minus1 0, entropy 0, constrained_intra_pred 0, deblocking_filter_control_present 1, disable_deblocking_filter_idc 0, offsets 0, max_num_ref_frames 0, 50x30 MBs, frame_mbs_only 1.

## What the structs are

The code is a JM-reference-decoder port (strings "NULL img", "NULL currentSlice", "activate_sps", "UseParameterSet", "init_parsing_dpb").

- **S** = `*(ctx+0x14F620)` = JM `img` (picture/decoder state, current slice header copy).
- **R** = `*(S+0x2C)` = JM `currentSlice` (proven: 0x43e8ea loads it, the "NULL currentSlice" check at 0x43e8fe tests it).
- **Q** = `*(ctx+0x14F628)` = active SPS, an entry of the SPS table ctx+0xAB448 + id·0x838 (`activate_sps` 0x43ca2c stores the pointer at 0x43ca92).
- **P** = `*(ctx+0x14F624)` = active PPS, entry of ctx+0xBBB48 + id·0x7F4 (`activate_pps` 0x43cc8e, store 0x43cce2). Same layout as the parse buffers (SPS at ctx+0x13AF48, PPS at ctx+0x13B780); PicParaSetEnd 0x43bc36 copies field by field.
- NAL header (0x4364e8): ctx byte +0x2A290 = nal_ref_idc (byte>>5), +0x2A28F = nal_unit_type (&0x1F).

### SPS (Q) layout, parser 0x43ce48 → 0x43b394 (scaling) / 0x43bfc8 → 0x43b4ba (poc cycle) / 0x43c136 → VUI 0x43c58e / end 0x43b972

+4 profile_idc · +8/+0xC/+0x10/+0x14 constraint_set0..3 · +0x18 level_idc · +0x19 sps_id · +0x1A chroma_format_idc (default 1 at 0x43cf6a; read at 0x43cfbe for profiles 100/110/122/244) · +0x1C separate_colour_plane · +0x20/+0x21 bit_depth_luma/chroma_minus8 · +0x24 qpprime_y_zero_transform_bypass · +0x28 seq_scaling_matrix_present · +0x14C log2_max_frame_num_minus4 · +0x14D poc_type · +0x14E log2_max_poc_lsb_minus4 · +0x150 delta_pic_order_always_zero · +0x154/+0x158 offsets · +0x15C num_ref_frames_in_poc_cycle · +0x560 max_num_ref_frames (forced to 1 for some stream types via 0x43118e) · +0x564 gaps_allowed · +0x568 pic_width_in_mbs_minus1 (byte, capped at 0x77) · +0x569 pic_height_in_map_units_minus1 · +0x56C frame_mbs_only · +0x570 mb_adaptive_frame_field · +0x574 direct_8x8_inference · +0x578 frame_cropping + 4 offsets at +0x57C.. · +0x584 vui_present.

### PPS (P) layout, parser 0x43d04c → 0x43b6d2 (more_rbsp_data) → 0x43b7b2 / 0x43b8de

+4 pps_id (16 bit) · +6 sps_id · **+8 entropy_coding_mode_flag** (0x43d0ea) · **+0xC transform_8x8_mode_flag** (0x43b768; zeroed at 0x43d27c) · +0x10 pic_scaling_matrix_present · +0x134 bottom_field_pic_order_in_frame_present · **+0x138 num_slice_groups_minus1** (0x43d104) · +0x139 slice_group_map_type (+0x16C/+0x170 change dir / rate_minus1) · +0x7C8/+0x7C9 num_ref_idx_l0/l1_default_active_minus1 · **+0x7CC weighted_pred_flag** (0x43d1a0) · **+0x7D0 weighted_bipred_idc** (u(2), 0x43d1ac) · +0x7D4 pic_init_qp_minus26 · +0x7D8 pic_init_qs_minus26 · **+0x7DC chroma_qp_index_offset** (0x43d236) · **+0x7E0 second_chroma_qp_index_offset** (se at 0x43b958; copied from +0x7DC at 0x43b794 when the PPS has no more data) · +0x7E4 deblocking_filter_control_present · **+0x7E8 constrained_intra_pred_flag** (0x43d272) · +0x7EC redundant_pic_cnt_present.

### Slice header states

0x43e8bc (first_mb, slice_type, pps_id, UseParameterSet) → 0x43d6fa (frame_num .. num_ref_idx) → 0x43d438 (ref_pic_list_reordering) → 0x43d9c6 (SPAR pred_weight_table, only when weighted_pred_flag and P/SP, or weighted_bipred_idc 1 and B: 0x43d6a6..0x43d6c6) → 0x43e58c (dec_ref_pic_marking, when nal_ref_idc ≠ 0) → 0x43da58 (cabac_init_idc .. slice_group_change_cycle, derived sizes) → 0x43de28 SliceHeaderEnd.

## Field map

| # | Reg | Source | Writer | Meaning | Test IDR value |
|---|---|---|---|---|---|
| 3 | D018 | S+0x58 | 0x43d7b4 (0 when frame_mbs_only, 0x43d7a6) | field_pic_flag | 0 |
| 4 | D01C | S+0x5C | 0x43d7be / 0x43d7d4 | bottom_field_flag; D01C = !bottom_field_flag (frame or top field) | 1 |
| 5 | D090 | S+0x14 | 0x43d8cc | direct_spatial_mv_pred_flag (B only, else 0) | 0 |
| 6 | D020 | S+0x30 | 0x43d7ec | MbaffFrameFlag = mb_adaptive_frame_field_flag && !field_pic_flag | 0 |
| 7 | D014 | P+0x7E8 | 0x43d272 | constrained_intra_pred_flag | 0 |
| 8 | D080 | S+0x132C0 | 0x43e93c | nal_ref_idc (byte); D080 = nal_ref_idc ≠ 0 | 1 |
| 9 | D084 | P+0x08 | 0x43d0ea | entropy_coding_mode_flag | 0 |
| 10 | D010 | P+0x7DC, P+0x7E0 | 0x43d236, 0x43b958/0x43b798 | (chroma_qp_index_offset & 0x3F) \| (second_chroma_qp_index_offset & 0x3F) << 6 | 0xFBE |
| 11 | D0A0 | R+0x04 | 0x43db30 | SliceQPY = 26 + pic_init_qp_minus26 + slice_qp_delta (R+0x08 = slice_qp_delta, S+0x08 = same QP) | 5 |
| 12 | D024 | R+0x0C (byte) | 0x43ea80 | slice_type % 5 (0 P, 1 B, 2 I, 3 SP, 4 SI); also S+0x1C | 2 |
| 13 | D008 | S+0x132C8 (byte) | 0x43caea (activate_sps) | PicWidthInMbs = pic_width_in_mbs_minus1 + 1 | 50 (0x32) |
| 14 | D00C | S+0x132CC (16) | 0x43dc34 | PicHeightInMbs = FrameHeightInMbs >> field_pic_flag (FrameHeightInMbs = S+0x132CA = (2 − frame_mbs_only)·PicHeightInMapUnits, 0x43cb10) | 30 (0x1E) |
| 15 | D088 | S+0x00, S+0x132C8 | S+0 at 0x43e2a0 | S+0 = CurrMbAddr = first_mb_in_slice << MbaffFrameFlag. D088 = first MB row: S0 / W (MBAFF: 2·(S0 / 2W)); 0 if W = 0 | 0 |
| 16 | D08C | same | | first MB column: S0 % W (MBAFF: (S0 >> 1) % W) | 0 |
| 17 | D094 | S+0x38 | 0x43d8d6 (default) / 0x43d90c (override) | num_ref_idx_l0_active_minus1. For I slices the override is not read, so S+0x38 = PPS default + 1 | 0 |
| 18 | D098 | S+0x39 | 0x43d8e0 / 0x43d94a, zeroed for non-B at 0x43d9ac | num_ref_idx_l1_active − 1, 16 bit. For I/P slices S+0x39 = 0, so **D098 = 0xFFFF** | 0xFFFF |
| 19 | D0A4 | ctx+0xAA460 \| colocated bits | ctx+0xAA460 at 0x438520 | ctx+0xAA460 = 0x40 if the DPB frame-buffer count (0x6E295C = max_num_ref_frames + 1 + 0x47bf68(), 0x4382d8) > 16, else 0. Colocated part (0x43a916..0x43a970), only when list1[0] (ctx+0x151A8C) is non-null: `fieldsel + 2·col[+0x44] + 4·parity + 8·(col[+0x28]==0) + 16·direct_8x8_inference + 32·frame_mbs_only` | 0 |
| 20 | D180 | P+0x7CC (16) | 0x43d1a0 | weighted_pred_flag | 0 |
| 21 | D184 | P+0x7D0 | 0x43d1ac | weighted_bipred_idc | 0 |
| 22 | D188 | S+0xA8 | 0x43a5ae (SPAR D530) | luma_log2_weight_denom; only written when SPAR runs | 0 (inference: never written before the first weighted slice) |
| 23 | D18C | S+0xA9 | 0x43a5c0 (SPAR D534, only if chroma_format_idc ≠ 0) | chroma_log2_weight_denom | 0 (same) |
| 24 | D800 | R+0x328 | 0x43db8a; 0 when no control at 0x43dbd0 | disable_deblocking_filter_idc | 0 |
| 25 | D804 | R+0x32C | 0x43dba2 (0 if idc = 1) | FilterOffsetA = slice_alpha_c0_offset_div2 << 1 (16-bit two's complement) | 0 |
| 26 | D808 | R+0x330 | 0x43dbb6 (0 if idc = 1) | FilterOffsetB = slice_beta_offset_div2 << 1 | 0 |
| 29 | D09C | S+0x132E8 (byte) | 0x43daf0; 0 at 0x43db0e for CAVLC or I/SI | cabac_init_idc | 0 |
| 30 | D004 | Q+4, Q+0x1A, P+0x0C, P+0x138, S+4 | | bit0 profile_idc > 99; bits 2:1 chroma_format_idc; bit3 transform_8x8_mode_flag; if num_slice_groups_minus1 ≠ 0: bit4 = 1 and bits 15:8 = S+4 = current_slice_nr (0 at picture init 0x4356ce, +1 after each slice commit 0x43e3da) | 0x0002 |
| 31-34 | D190..D19C | S+0x132AC..0x132B8 | SPAR 0x43a5c8..0x43a5f0; reset to 0xFFFFFFFF at 0x4351d4..0x4351f2 (decoder reset 0x4350c2) | SPAR result words D538..D544 (weight-table summary, meaning open) | eCos sends 0xFFFFFFFF ×4 when no weighted slice has run |

Other S fields seen on the way: S+0x08 SliceQPY, +0x0C QSY, +0x10 sp_for_switch_flag, +0x18 num_ref_idx_active_override_flag, +0x1C slice_type, +0x1E/+0x20 width/height in pixels, +0x22/+0x24 chroma width/height, +0x26 structure (0 frame, 1 top, 2 bottom; also R+0x0D), +0x34 nal_ref_idc == 0, +0x3A slice_group_change_cycle, +0x40 redundant_pic_cnt, +0x54 frame_num, +0x60 pic_order_cnt_lsb, +0x64 delta_pic_order_cnt_bottom, +0x74/+0x78 delta_pic_order_cnt[0/1], +0x132BC idr_flag (nal_unit_type == 5), +0x132C2 idr_pic_id, +0x132C4 MaxFrameNum, +0x132C9 PicHeightInMapUnits, +0x132CE PicSizeInMbs, +0x132D0 FrameSizeInMbs, +0x132F5/F6 bit depths. R: +0x0E first_mb_in_slice (clamped to PicSizeInMbs − 1 at 0x43dc8a), +0x18 ref_pic_list_reordering_flag_l0, +0x334 pps_id.

## IDR I-slice: what is needed

- **Item 19 (D0A4)**: eCos emits it for the first slice (setup differs from the previous one). After `init_parsing_dpb` the list arrays are zeroed (0x4384c8..0x4384da, r16 = 0), so list1[0] is null and the colocated part is 0; with ≤ 16 frame buffers D0A4 = 0. Send `0x04290000`.
- **Items 24-26**: needed. idc 0 means the loop filter is **on**; send 0, 0, 0.
- **Items 31-34**: SPAR does not run for I slices, so eCos sends whatever S+0x132AC.. holds: 0xFFFFFFFF after a decoder reset. Matching eCos exactly means four `0x1464..0x1467` pairs with data 0xFFFFFFFF. That the hardware ignores them when weighted_pred_flag = 0 is inference.
- **B-only block**: skipped unless slice_type == B (0x43ac0a branches to 0x43af4e). Not emitted.
- **Item 35**: `0x2040003F` + 64 words. listXsize[0] (ctx+0xAA47C) and listXsize[1] (ctx+0xAA480) are 0, so all 64 words are 0 (32 list-0 entries, then 32 list-1 entries, zero padded at 0x43b22c and 0x43b2b0).
- **Item 36**: `0x2000003F` + 64 words of table 0x72A6D8, which is memset to 0 (0x43b15c..0x43b172, 0x100 bytes) and filled only for list-0 entries, so 64 zero words. (The index mask, 0xF or 0x1F, follows the same "> 16 frame buffers" test as D0A4 bit 6.)

## Test IDR command values (words as written to the ring, before byte swap)

0x04060000 (D018), 0x04070001 (D01C), 0x04240000 (D090), 0x04080000 (D020), 0x04050000 (D014), 0x04200001 (D080), 0x04210000 (D084), 0x04040FBE (D010), 0x04280005 (D0A0), 0x04090002 (D024), 0x04020032 (D008), 0x0403001E (D00C), 0x04220000 (D088), 0x04230000 (D08C), 0x04250000 (D094), 0x0426FFFF (D098), 0x04290000 (D0A4), 0x04600000, 0x04610000, 0x04620000, 0x04630000, 0x60000000, 0x60100000, 0x60200000 (D800..D808), D028/D02C per BG2D indices, 0x04270000 (D09C), 0x04010002 (D004), [0x14640000 0xFFFFFFFF] .. [0x14670000 0xFFFFFFFF], 0x2040003F + 64×0, 0x2000003F + 64×0, 0xF8000000.

## Open / inference

1. Items 31-34: meaning of the SPAR words D538..D544, and whether the hardware reads them for I slices (inference: no). eCos value for the first IDR is 0xFFFFFFFF (proven); 0 is probably also fine but untested.
2. S+0xA8/S+0xA9 initial value: no writer other than SPAR found; 0 is inference (struct presumably zeroed at allocation).
3. D0A4 bit 6 meaning ("more than 16 frame buffers", 5-bit buffer index) is inference from the count it is derived from. The colocated bit layout is a formula only; the per-field meaning (col[+0x28], col[+0x44], col[+0x40]) was not traced.
4. D01C name: computed as !bottom_field_flag; whether the hardware reads it as "top field" or "not bottom" is inference.
5. D098 = 0xFFFF for non-B slices comes straight from the code (S+0x39 − 1, zero-extended); whether the hardware cares for I/P is open.
