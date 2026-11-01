/* H.264 header parsing for the hardware decoder driver (notes/h264_decoder.md): SPS, PPS and slice headers of the
 * baseline / constrained-baseline / main streams Android Auto sends (frame_mbs_only, 4:2:0, 8-bit). Software parses
 * headers; the MFD hardware decodes the slice data, so the slice header result includes where that data starts
 * (bit offset in the NAL payload with emulation-prevention bytes removed, and in the raw NAL bytes).
 * Pure C, no allocation; host-tested by test/test_h264p.py against ffmpeg's trace_headers. */
#include "h264p.h"

typedef unsigned char u8;
typedef unsigned int u32;

struct br {
    const u8 *p;
    u32 n, pos;     /* pos in bits over the RBSP (emulation-prevention bytes skipped) */
    u32 raw_pos;    /* bytes consumed from p including EPBs, for mapping RBSP bits to raw bytes */
    int err;
    u8 cur;         /* current RBSP byte */
    int left;       /* bits left in cur */
    int zeros;      /* consecutive 0x00 bytes seen (EPB detection) */
    u32 epb_before[64]; /* raw offsets of skipped EPBs (for raw slice-data offset) */
    int n_epb;
};

static int next_byte(struct br *b)
{
    for (;;) {
        if (b->raw_pos >= b->n) {
            b->err = 1;
            return 0;
        }
        u8 c = b->p[b->raw_pos++];
        if (b->zeros >= 2 && c == 3) {   /* emulation-prevention byte */
            if (b->n_epb < 64)
                b->epb_before[b->n_epb++] = b->raw_pos - 1;
            b->zeros = 0;
            continue;
        }
        b->zeros = c ? 0 : b->zeros + 1;
        return c;
    }
}

static u32 u1(struct br *b)
{
    if (!b->left) {
        b->cur = (u8)next_byte(b);
        b->left = 8;
    }
    b->left--;
    b->pos++;
    return (b->cur >> b->left) & 1;
}

static u32 un(struct br *b, int n)
{
    u32 v = 0;
    while (n--)
        v = v << 1 | u1(b);
    return v;
}

static u32 ue(struct br *b)
{
    int z = 0;
    while (!u1(b) && !b->err && z < 32)
        z++;
    return z ? ((1u << z) - 1) + un(b, z) : 0;
}

static int se(struct br *b)
{
    u32 k = ue(b);
    return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);
}

static void br_init(struct br *b, const u8 *nal, u32 n)
{
    b->p = nal;
    b->n = n;
    b->pos = b->raw_pos = 0;
    b->err = b->left = b->zeros = b->n_epb = 0;
}

static void skip_scaling_list(struct br *b, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; j++) {
        if (next)
            next = (last + se(b) + 256) % 256;
        last = next ? next : last;
    }
}

int h264_parse_sps(const u8 *nal, u32 n, struct h264_sps *s)
{
    struct br b;
    br_init(&b, nal + 1, n - 1);   /* skip the NAL header byte */
    s->profile_idc = un(&b, 8);
    un(&b, 8);   /* constraint flags */
    s->level_idc = un(&b, 8);
    s->id = ue(&b);
    s->chroma_format_idc = 1;
    if (s->profile_idc == 100 || s->profile_idc == 110 || s->profile_idc == 122 || s->profile_idc == 244 ||
        s->profile_idc == 44 || s->profile_idc == 83 || s->profile_idc == 86 || s->profile_idc == 118 ||
        s->profile_idc == 128) {
        s->chroma_format_idc = ue(&b);
        if (s->chroma_format_idc == 3)
            u1(&b);
        ue(&b);   /* bit depth luma - 8 */
        ue(&b);   /* bit depth chroma - 8 */
        u1(&b);
        if (u1(&b))   /* seq_scaling_matrix_present */
            for (int i = 0; i < 8; i++)
                if (u1(&b))
                    skip_scaling_list(&b, i < 6 ? 16 : 64);
    }
    s->log2_max_frame_num = ue(&b) + 4;
    s->poc_type = ue(&b);
    if (s->poc_type == 0)
        s->log2_max_poc_lsb = ue(&b) + 4;
    else if (s->poc_type == 1) {
        s->delta_pic_order_always_zero = u1(&b);
        se(&b);
        se(&b);
        u32 k = ue(&b);
        for (u32 i = 0; i < k && !b.err; i++)
            se(&b);
    }
    s->num_ref_frames = ue(&b);
    u1(&b);   /* gaps_in_frame_num_allowed */
    s->mb_width = ue(&b) + 1;
    s->mb_height = ue(&b) + 1;   /* map units; frame_mbs_only below */
    s->frame_mbs_only = u1(&b);
    if (!s->frame_mbs_only) {
        u1(&b);   /* mb_adaptive_frame_field */
        s->mb_height *= 2;
    }
    s->direct_8x8_inference = u1(&b);
    s->crop_left = s->crop_right = s->crop_top = s->crop_bottom = 0;
    if (u1(&b)) {
        s->crop_left = ue(&b);
        s->crop_right = ue(&b);
        s->crop_top = ue(&b);
        s->crop_bottom = ue(&b);
    }
    /* VUI is not needed by the decoder */
    s->width = s->mb_width * 16 - 2 * (s->crop_left + s->crop_right);
    s->height = s->mb_height * 16 - 2 * (s->crop_top + s->crop_bottom) * (2 - s->frame_mbs_only);
    return b.err ? -1 : 0;
}

int h264_parse_pps(const u8 *nal, u32 n, struct h264_pps *p)
{
    struct br b;
    br_init(&b, nal + 1, n - 1);
    p->id = ue(&b);
    p->sps_id = ue(&b);
    p->entropy_coding_mode = u1(&b);
    p->bottom_field_pic_order = u1(&b);
    p->num_slice_groups = ue(&b) + 1;
    if (p->num_slice_groups > 1)
        return -2;   /* FMO is not used by AA and not supported */
    p->num_ref_idx_l0_default = ue(&b) + 1;
    p->num_ref_idx_l1_default = ue(&b) + 1;
    p->weighted_pred = u1(&b);
    p->weighted_bipred_idc = un(&b, 2);
    p->pic_init_qp = 26 + se(&b);
    p->pic_init_qs = 26 + se(&b);
    p->chroma_qp_index_offset = se(&b);
    p->deblocking_filter_control_present = u1(&b);
    p->constrained_intra_pred = u1(&b);
    p->redundant_pic_cnt_present = u1(&b);
    p->transform_8x8_mode = 0;
    p->second_chroma_qp_index_offset = p->chroma_qp_index_offset;
    return b.err ? -1 : 0;
}

int h264_parse_slice(const u8 *nal, u32 n, const struct h264_sps *s, const struct h264_pps *p, struct h264_slice *h)
{
    struct br b;
    h->nal_ref_idc = (nal[0] >> 5) & 3;
    h->nal_type = nal[0] & 31;
    br_init(&b, nal + 1, n - 1);
    h->first_mb = ue(&b);
    h->slice_type = ue(&b) % 5;   /* 0 P, 1 B, 2 I */
    h->pps_id = ue(&b);
    h->frame_num = un(&b, (int)s->log2_max_frame_num);
    h->idr_pic_id = 0;
    if (h->nal_type == 5)
        h->idr_pic_id = ue(&b);
    h->poc_lsb = 0;
    h->delta_poc_bottom = 0;
    if (s->poc_type == 0) {
        h->poc_lsb = un(&b, (int)s->log2_max_poc_lsb);
        if (p->bottom_field_pic_order)
            h->delta_poc_bottom = se(&b);
    } else if (s->poc_type == 1 && !s->delta_pic_order_always_zero) {
        se(&b);
        if (p->bottom_field_pic_order)
            se(&b);
    }
    if (p->redundant_pic_cnt_present)
        ue(&b);
    h->direct_spatial_mv_pred = 0;
    if (h->slice_type == 1)
        h->direct_spatial_mv_pred = u1(&b);
    h->num_ref_idx_l0 = p->num_ref_idx_l0_default;
    h->num_ref_idx_l1 = p->num_ref_idx_l1_default;
    h->num_ref_idx_override = 0;
    if (h->slice_type == 0 || h->slice_type == 1) {
        if ((h->num_ref_idx_override = u1(&b))) {
            h->num_ref_idx_l0 = ue(&b) + 1;
            if (h->slice_type == 1)
                h->num_ref_idx_l1 = ue(&b) + 1;
        }
    }
    h->ref_list_mod = 0;
    if (h->slice_type != 2) {   /* ref_pic_list_modification: record presence; AA streams use none */
        for (int l = 0; l < (h->slice_type == 1 ? 2 : 1); l++) {
            if (u1(&b)) {
                h->ref_list_mod = 1;
                u32 op;
                while ((op = ue(&b)) != 3 && !b.err)
                    ue(&b);
            }
        }
    }
    h->weighted = 0;
    if ((p->weighted_pred && h->slice_type == 0) || (p->weighted_bipred_idc == 1 && h->slice_type == 1)) {
        /* pred_weight_table: parsed past (AA's baseline streams never carry it; the decoder does not use it yet) */
        h->weighted = 1;
        ue(&b);   /* luma_log2_weight_denom */
        if (s->chroma_format_idc)
            ue(&b);
        for (int l = 0; l < (h->slice_type == 1 ? 2 : 1); l++) {
            u32 nr = l ? h->num_ref_idx_l1 : h->num_ref_idx_l0;
            for (u32 i = 0; i < nr && !b.err; i++) {
                if (u1(&b))
                    se(&b), se(&b);
                if (s->chroma_format_idc && u1(&b))
                    se(&b), se(&b), se(&b), se(&b);
            }
        }
    }
    h->long_term_ref = 0;
    if (h->nal_ref_idc) {   /* dec_ref_pic_marking */
        if (h->nal_type == 5) {
            u1(&b);   /* no_output_of_prior_pics */
            h->long_term_ref = u1(&b);
        } else if (u1(&b)) {   /* adaptive marking */
            u32 op;
            while ((op = ue(&b)) != 0 && !b.err) {
                if (op == 1 || op == 3)
                    ue(&b);
                if (op == 2)
                    ue(&b);
                if (op == 3 || op == 6)
                    ue(&b);
                if (op == 4)
                    ue(&b);
            }
        }
    }
    h->cabac_init_idc = 0;
    if (p->entropy_coding_mode && h->slice_type != 2)
        h->cabac_init_idc = ue(&b);
    h->qp = p->pic_init_qp + se(&b);
    h->disable_deblocking = 0;
    h->alpha_offset = h->beta_offset = 0;
    if (p->deblocking_filter_control_present) {
        h->disable_deblocking = ue(&b);
        if (h->disable_deblocking != 1) {
            h->alpha_offset = 2 * se(&b);
            h->beta_offset = 2 * se(&b);
        }
    }
    if (p->entropy_coding_mode)   /* CABAC slice data starts byte-aligned (cabac_alignment_one_bits) */
        while (b.pos & 7)
            u1(&b);
    h->data_bit_rbsp = b.pos + 8;   /* + the NAL header byte */
    /* raw (with EPBs) position of the first slice-data bit: RBSP byte k maps to raw byte k + EPBs before it */
    u32 rbsp_byte = h->data_bit_rbsp / 8, raw = rbsp_byte;
    for (int i = 0; i < b.n_epb; i++)
        if (b.epb_before[i] + 1 <= raw)   /* epb_before is relative to nal+1 */
            raw++;
    h->data_bit_raw = raw * 8 + h->data_bit_rbsp % 8;
    return b.err ? -1 : 0;
}

int h264_next_nal(const u8 *p, u32 n, u32 *pos, u32 *start, u32 *len)
{
    u32 i = *pos;
    while (i + 3 <= n && !(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
        i++;
    if (i + 3 > n)
        return 0;
    u32 s = i + 3, j = s;
    while (j + 3 <= n && !(p[j] == 0 && p[j + 1] == 0 && (p[j + 2] == 1 || (p[j + 2] == 0 && j + 3 < n && p[j + 3] == 1))))
        j++;
    if (j + 3 > n)
        j = n;
    *start = s;
    *len = j - s;
    *pos = j;
    return 1;
}
