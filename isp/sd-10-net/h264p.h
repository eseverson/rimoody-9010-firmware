/* H.264 header parsing for the hardware decoder driver (h264p.c). */
#ifndef H264P_H
#define H264P_H
struct h264_sps {
    unsigned int profile_idc, level_idc, id, chroma_format_idc, log2_max_frame_num, poc_type, log2_max_poc_lsb,
        delta_pic_order_always_zero, num_ref_frames, mb_width, mb_height, frame_mbs_only, direct_8x8_inference,
        crop_left, crop_right, crop_top, crop_bottom, width, height;
};
struct h264_pps {
    unsigned int id, sps_id, entropy_coding_mode, bottom_field_pic_order, num_slice_groups, num_ref_idx_l0_default,
        num_ref_idx_l1_default, weighted_pred, weighted_bipred_idc, deblocking_filter_control_present,
        constrained_intra_pred, redundant_pic_cnt_present, transform_8x8_mode;
    int pic_init_qp, pic_init_qs, chroma_qp_index_offset, second_chroma_qp_index_offset;
};
struct h264_slice {
    unsigned int nal_ref_idc, nal_type, first_mb, slice_type, pps_id, frame_num, idr_pic_id, poc_lsb,
        direct_spatial_mv_pred, num_ref_idx_override, num_ref_idx_l0, num_ref_idx_l1, ref_list_mod, long_term_ref,
        cabac_init_idc, disable_deblocking, weighted, adaptive_marking;
    int delta_poc_bottom, qp, alpha_offset, beta_offset;
    unsigned int data_bit_rbsp, data_bit_raw;   /* first slice-data bit, from the NAL header byte, without / with EPBs */
};
/* nal points at the NAL header byte (after the start code), n = NAL length. 0 ok, <0 error / unsupported. */
int h264_parse_sps(const unsigned char *nal, unsigned int n, struct h264_sps *s);
int h264_parse_pps(const unsigned char *nal, unsigned int n, struct h264_pps *p);
int h264_parse_slice(const unsigned char *nal, unsigned int n, const struct h264_sps *s, const struct h264_pps *p,
                     struct h264_slice *h);
/* Annex-B scan: next NAL after *pos; returns 1 with start (header byte) and len, 0 at the end. */
int h264_next_nal(const unsigned char *p, unsigned int n, unsigned int *pos, unsigned int *start, unsigned int *len);
#endif
