/* Host driver for h264p.c: prints the parsed fields of every SPS / PPS / slice in an Annex-B file (test_h264p.py). */
#include "../h264p.c"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    static unsigned char d[4 << 20];
    if (argc < 2)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    unsigned int n = (unsigned)fread(d, 1, sizeof d, f), pos = 0, s0, len;
    struct h264_sps sps = { 0 };
    struct h264_pps pps = { 0 };
    while (h264_next_nal(d, n, &pos, &s0, &len)) {
        int t = d[s0] & 31, r;
        if (t == 7) {
            r = h264_parse_sps(d + s0, len, &sps);
            printf("SPS r=%d profile_idc=%u level_idc=%u seq_parameter_set_id=%u log2_max_frame_num_minus4=%u pic_order_cnt_type=%u log2_max_pic_order_cnt_lsb_minus4=%u max_num_ref_frames=%u pic_width_in_mbs_minus1=%u pic_height_in_map_units_minus1=%u frame_mbs_only_flag=%u direct_8x8_inference_flag=%u width=%u height=%u\n",
                   r, sps.profile_idc, sps.level_idc, sps.id, sps.log2_max_frame_num - 4, sps.poc_type, sps.log2_max_poc_lsb - 4, sps.num_ref_frames, sps.mb_width - 1, sps.mb_height - 1, sps.frame_mbs_only, sps.direct_8x8_inference, sps.width, sps.height);
        } else if (t == 8) {
            r = h264_parse_pps(d + s0, len, &pps);
            printf("PPS r=%d pic_parameter_set_id=%u seq_parameter_set_id=%u entropy_coding_mode_flag=%u num_ref_idx_l0_default_active_minus1=%u pic_init_qp_minus26=%d chroma_qp_index_offset=%d deblocking_filter_control_present_flag=%u constrained_intra_pred_flag=%u\n",
                   r, pps.id, pps.sps_id, pps.entropy_coding_mode, pps.num_ref_idx_l0_default - 1, pps.pic_init_qp - 26, pps.chroma_qp_index_offset, pps.deblocking_filter_control_present, pps.constrained_intra_pred);
        } else if (t == 1 || t == 5) {
            struct h264_slice h;
            r = h264_parse_slice(d + s0, len, &sps, &pps, &h);
            printf("SLICE r=%d first_mb_in_slice=%u slice_type=%u pic_parameter_set_id=%u frame_num=%u idr_pic_id=%u pic_order_cnt_lsb=%u slice_qp_delta=%d disable_deblocking_filter_idc=%u data_bit_rbsp=%u data_bit_raw=%u nal_len=%u\n",
                   r, h.first_mb, h.slice_type, h.pps_id, h.frame_num, h.idr_pic_id, h.poc_lsb, h.qp - pps.pic_init_qp, h.disable_deblocking, h.data_bit_rbsp, h.data_bit_raw, len);
        }
    }
    return 0;
}
