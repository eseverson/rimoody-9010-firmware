"""h264p.c against ffmpeg's trace_headers on the streams in test/h264: every SPS / PPS / slice-header field we parse,
and the bit position where slice data starts (end of the last header element, NAL header byte included)."""
import re, subprocess, sys, os

FIELDS = {'SPS': ['profile_idc', 'level_idc', 'seq_parameter_set_id', 'log2_max_frame_num_minus4', 'pic_order_cnt_type',
                  'max_num_ref_frames', 'pic_width_in_mbs_minus1', 'pic_height_in_map_units_minus1',
                  'frame_mbs_only_flag', 'direct_8x8_inference_flag'],
          'PPS': ['pic_parameter_set_id', 'seq_parameter_set_id', 'entropy_coding_mode_flag',
                  'num_ref_idx_l0_default_active_minus1', 'pic_init_qp_minus26', 'chroma_qp_index_offset',
                  'deblocking_filter_control_present_flag', 'constrained_intra_pred_flag'],
          'SLICE': ['first_mb_in_slice', 'pic_parameter_set_id', 'frame_num', 'idr_pic_id', 'pic_order_cnt_lsb',
                    'slice_qp_delta', 'disable_deblocking_filter_idc']}
HDR = {'Sequence Parameter Set': 'SPS', 'Picture Parameter Set': 'PPS', 'Slice Header': 'SLICE'}

def trace(path):
    out = subprocess.run(['ffmpeg', '-hide_banner', '-i', path, '-c', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'],
                         capture_output=True, text=True).stderr
    units, cur = [], None
    for line in out.splitlines():
        m = re.search(r'\] (.*)$', line)
        if not m:
            continue
        t = m.group(1).strip()
        if t in HDR:
            cur = {'kind': HDR[t], 'end': 0}
            units.append(cur)
            continue
        m = re.match(r'(\d+)\s+(\S+)\s+([01]+) = (-?\d+)', t)
        if m and cur is not None:
            pos, name, bits, val = int(m.group(1)), m.group(2), m.group(3), int(m.group(4))
            cur.setdefault(name, val)
            if cur['kind'] == 'SLICE':
                cur['end'] = pos + len(bits)
        elif cur is not None and cur['kind'] == 'SLICE' and t.startswith(('NAL unit', 'Slice Data')):
            cur = None
    return units

def ours(exe, path):
    units = []
    for line in subprocess.run([exe, path], capture_output=True, text=True).stdout.splitlines():
        kind, rest = line.split(' ', 1)
        u = {'kind': kind}
        for kv in rest.split():
            k, v = kv.split('=')
            u[k] = int(v) if v.lstrip('-').isdigit() else v
        units.append(u)
    return units

def main():
    exe = os.path.abspath(sys.argv[1])
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'h264')
    fails = 0
    for f in sorted(os.listdir(d)):
        if not f.endswith('.h264'):
            continue
        a, b = trace(os.path.join(d, f)), ours(exe, os.path.join(d, f))
        if [u['kind'] for u in a[:4]] == ['SPS', 'PPS', 'SPS', 'PPS']:
            a = a[2:]   # ffmpeg traces the extradata copy of SPS/PPS first
        bad = 0
        if [u['kind'] for u in a] != [u['kind'] for u in b]:
            print('FAIL %s: unit sequence differs (%d vs %d units)' % (f, len(a), len(b)))
            fails += 1
            continue
        for i, (x, y) in enumerate(zip(a, b)):
            if y.get('r', 0) != 0:
                bad += 1
                print('  %s unit %d: parser returned %d' % (f, i, y['r']))
            for k in FIELDS[x['kind']]:
                if k in x and x[k] != y.get(k):
                    bad += 1
                    if bad < 6:
                        print('  %s unit %d %s %s: ffmpeg %s, ours %s' % (f, i, x['kind'], k, x[k], y.get(k)))
            if x['kind'] == 'SLICE' and x['end'] != y['data_bit_rbsp']:
                bad += 1
                if bad < 6:
                    print('  %s unit %d slice data start: ffmpeg %d, ours %d' % (f, i, x['end'], y['data_bit_rbsp']))
        n_sl = sum(u['kind'] == 'SLICE' for u in a)
        print('%s %s: %d units (%d slices), %d mismatches' % ('PASS' if not bad else 'FAIL', f, len(a), n_sl, bad))
        fails += bool(bad)
    print('%d failures' % fails)
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
