/* Android Auto head-unit protocol (notes/android_auto.md), transport-independent: the platform moves raw bulk bytes
 * (USB accessory mode on the unit, libusb in test/aa_host.c) and gets callbacks for video, audio and state.
 *
 * Wire format (as implemented by open head-unit projects such as aasdk; checked against a phone in testing):
 *   frame  = channel u8, flags u8, size u16be, [total u32be if flags & 3 == FIRST], payload[size]
 *   flags  = bits 0-1 fragment (1 first, 2 last, 3 whole message, 0 middle), bit 2 control message on a service
 *            channel, bit 3 encrypted (payload is TLS records)
 *   message = id u16be + body (protobuf, or raw for version / SSL / media)
 * Control channel 0: version request/response (1/2, plain), SSL handshake records (3, plain), auth complete (4, plain),
 * then encrypted: service discovery (5/6), channel open (7/8, on the target channel with the control bit), ping
 * (11/12), navigation focus (13/14), shutdown (15/16), audio focus (18/19).
 * Media channels: setup 0x8000 -> 0x8003, start 0x8001, stop 0x8002, data 0x0000 (u64 timestamp + payload) or 0x0001
 * (codec config, no timestamp), ack 0x8004, video focus 0x8007 -> 0x8008. Input: binding 0x8002 -> 0x8003, events
 * 0x8001. Sensors: start 0x8001 -> 0x8002, events 0x8003. */
#include "aa.h"
#include "aa_tls.h"
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

enum { CH_CONTROL = 0, CH_INPUT = 1, CH_SENSOR = 2, CH_VIDEO = 3, CH_MEDIA_AUDIO = 4, CH_SPEECH_AUDIO = 5,
       CH_SYSTEM_AUDIO = 6, CH_MIC = 7, CH_COUNT = 8 };
enum { F_FIRST = 1, F_LAST = 2, F_WHOLE = 3, F_CONTROL = 4, F_ENC = 8 };

static const struct aa_platform *pf;
static struct aa_status st;
#ifndef AA_MAX_UNACKED
#define AA_MAX_UNACKED 4   /* video/audio frames the phone may send before our ack (1 made it stop-and-wait: 37 ms per frame over Wi-Fi) */
#endif
#ifdef __nds32__
#define AA_BIG __attribute__((section(".bigbss")))   /* payload: outside the 1 MB image window (payload.ld) */
#else
#define AA_BIG
#endif
static u8 tx[0x4000 + 64], msg[0x4000 + 16], plain[0x8000] AA_BIG;

/* ---- protobuf writer (varints, length-delimited fields, nested messages by back-patching) ---- */
struct pb { u8 *p, *end; };
static void pb_byte(struct pb *b, u8 v) { if (b->p < b->end) *b->p++ = v; }
static void pb_varint(struct pb *b, u64 v) { while (v >= 0x80) pb_byte(b, (u8)(v | 0x80)), v >>= 7; pb_byte(b, (u8)v); }
static void pb_u(struct pb *b, int field, u64 v) { pb_varint(b, (u64)field << 3); pb_varint(b, v); }
static void pb_s(struct pb *b, int field, const char *s)
{
    size_t n = strlen(s);
    pb_varint(b, (u64)field << 3 | 2);
    pb_varint(b, n);
    for (size_t i = 0; i < n; i++)
        pb_byte(b, (u8)s[i]);
}
/* nested message: 2 reserved length bytes (enough for < 16 KB), patched by pb_close */
static u8 *pb_open(struct pb *b, int field) { pb_varint(b, (u64)field << 3 | 2); u8 *at = b->p; pb_byte(b, 0); pb_byte(b, 0); return at; }
static void pb_close(struct pb *b, u8 *at)
{
    u32 n = (u32)(b->p - at - 2);
    at[0] = (u8)(n | 0x80);
    at[1] = (u8)(n >> 7);
}

/* ---- protobuf reader: find a varint field at the top level of a message ---- */
static int pb_get(const u8 *p, int n, int field, u64 *out)
{
    int i = 0;
    while (i < n) {
        u64 key = 0;
        for (int s = 0; i < n; s += 7) { u8 c = p[i++]; key |= (u64)(c & 0x7f) << s; if (!(c & 0x80)) break; }
        int wt = key & 7, f = (int)(key >> 3);
        u64 v = 0;
        if (wt == 0) {
            for (int s = 0; i < n; s += 7) { u8 c = p[i++]; v |= (u64)(c & 0x7f) << s; if (!(c & 0x80)) break; }
            if (f == field) { *out = v; return 0; }
        } else if (wt == 2) {
            for (int s = 0; i < n; s += 7) { u8 c = p[i++]; v |= (u64)(c & 0x7f) << s; if (!(c & 0x80)) break; }
            i += (int)v;
        } else if (wt == 5) i += 4;
        else if (wt == 1) i += 8;
        else return -1;
    }
    return -1;
}

/* ---- sending ---- */
static void log_s(const char *s) { if (pf->log) pf->log(s); }

static int send_frame(int ch, int flags, const u8 *p, int n)
{
    tx[0] = (u8)ch;
    tx[1] = (u8)flags;
    tx[2] = (u8)(n >> 8);
    tx[3] = (u8)n;
    memcpy(tx + 4, p, (size_t)n);
    st.tx_frames++;
    return pf->send(tx, n + 4);
}

/* one message (id + body), encrypted once TLS is up unless plain is asked for; split into 16 KB frames */
static int send_msg(int ch, int ctrl, int encrypt, u16 id, const u8 *body, int n)
{
    msg[0] = (u8)(id >> 8);
    msg[1] = (u8)id;
    if (n > (int)sizeof msg - 2)
        return -1;
    memcpy(msg + 2, body, (size_t)n);
    n += 2;
    int flags = (ctrl ? F_CONTROL : 0);
    if (!encrypt)
        return send_frame(ch, flags | F_WHOLE, msg, n);
    static u8 rec[0x4000 + 1024];
    if (aa_tls_write(msg, (size_t)n) < 0)
        return -1;
    size_t r = aa_tls_take(rec, sizeof rec);
    return send_frame(ch, flags | F_ENC | F_WHOLE, rec, (int)r);
}

static int send_pb(int ch, int ctrl, u16 id, struct pb *b, u8 *start) { return send_msg(ch, ctrl, 1, id, start, (int)(b->p - start)); }

/* ---- service discovery response: what this head unit offers ---- */
static void add_audio(struct pb *b, int ch, int audio_type, u32 rate, u32 chans)
{
    u8 *c = pb_open(b, 1);
    pb_u(b, 1, (u64)ch);
    u8 *av = pb_open(b, 3);
    pb_u(b, 1, 1);   /* stream type: audio */
    pb_u(b, 2, (u64)audio_type);   /* 1 speech, 2 system, 3 media */
    u8 *cfg = pb_open(b, 3);
    pb_u(b, 1, rate);
    pb_u(b, 2, 16);
    pb_u(b, 3, chans);
    pb_close(b, cfg);
    pb_close(b, av);
    pb_close(b, c);
}

static int send_service_discovery(void)
{
    static u8 buf[2048];
    struct pb b = { buf, buf + sizeof buf };
    u8 *c, *x, *y;
    /* video: H.264 baseline, 800x480 at 30 fps (the decoder is still to be proven; 480p keeps it light) */
    c = pb_open(&b, 1);
    pb_u(&b, 1, CH_VIDEO);
    x = pb_open(&b, 3);
    pb_u(&b, 1, 3);   /* stream type: video */
    pb_u(&b, 5, 1);   /* available while in call */
    y = pb_open(&b, 4);
    pb_u(&b, 1, pf->hd ? 2 : 1);   /* 1280x720 / 800x480 */
    pb_u(&b, 2, 2);                /* 30 fps */
    pb_u(&b, 3, pf->hd ? 256 : 0); /* margins: the phone lays its UI out in the center 1024 x 600 */
    pb_u(&b, 4, pf->hd ? 120 : 0);
    pb_u(&b, 5, pf->dpi ? pf->dpi : 140);
    pb_close(&b, y);
    pb_close(&b, x);
    pb_close(&b, c);
    if (pf->audio_mask & 1)
        add_audio(&b, CH_MEDIA_AUDIO, 3, 48000, 2);
    if (pf->audio_mask & 2)
        add_audio(&b, CH_SPEECH_AUDIO, 1, 16000, 1);
    if (pf->audio_mask & 4)
        add_audio(&b, CH_SYSTEM_AUDIO, 2, 16000, 1);
    if (pf->audio_mask & 0x80) {   /* microphone: AV input channel, PCM 16 kHz mono */
        c = pb_open(&b, 1);
        pb_u(&b, 1, CH_MIC);
        x = pb_open(&b, 5);
        pb_u(&b, 1, 1);
        y = pb_open(&b, 2);
        pb_u(&b, 1, 16000);
        pb_u(&b, 2, 16);
        pb_u(&b, 3, 1);
        pb_close(&b, y);
        pb_close(&b, x);
        pb_close(&b, c);
    }
    /* input: touchscreen in video coordinates */
    c = pb_open(&b, 1);
    pb_u(&b, 1, CH_INPUT);
    x = pb_open(&b, 4);
    pb_u(&b, 1, 3);    /* keycodes: HOME */
    pb_u(&b, 1, 4);    /* BACK */
    y = pb_open(&b, 2);
    pb_u(&b, 1, pf->touch_w ? (u64)pf->touch_w : pf->hd ? 1280 : 800);   /* touch coordinate space */
    pb_u(&b, 2, pf->touch_h ? (u64)pf->touch_h : pf->hd ? 720 : 480);
    pb_close(&b, y);
    pb_close(&b, x);
    pb_close(&b, c);
    /* sensors: driving status and night mode (required by the phone) */
    c = pb_open(&b, 1);
    pb_u(&b, 1, CH_SENSOR);
    x = pb_open(&b, 2);
    y = pb_open(&b, 1);
    pb_u(&b, 1, 13);   /* driving status */
    pb_close(&b, y);
    y = pb_open(&b, 1);
    pb_u(&b, 1, 10);   /* night data */
    pb_close(&b, y);
    pb_close(&b, x);
    pb_close(&b, c);
    pb_s(&b, 2, "Miata");          /* head unit name */
    pb_s(&b, 3, "MX-5");           /* car model */
    pb_s(&b, 4, "2006");           /* car year */
    pb_s(&b, 5, "0001");           /* car serial */
    pb_u(&b, 6, 1);                /* left-hand drive */
    pb_s(&b, 7, "miata-head-unit");
    pb_s(&b, 8, "SPHE8268K");
    pb_s(&b, 9, "1");
    pb_s(&b, 10, "1.0");
    pb_u(&b, 11, 0);
    return send_pb(CH_CONTROL, 0, 6, &b, buf);
}

/* ---- receiving ---- */
static int ch_open[CH_COUNT];
static u32 session[CH_COUNT];

static void ack_media(int ch, u32 session)
{
    u8 buf[16];
    struct pb b = { buf, buf + sizeof buf };
    pb_u(&b, 1, session);
    pb_u(&b, 2, 1);
    send_pb(ch, 0, 0x8004, &b, buf);
}

static void on_control(const u8 *m, int n)
{
    u16 id = (u16)(m[0] << 8 | m[1]);
    const u8 *body = m + 2;
    int bn = n - 2;
    u8 buf[64];
    struct pb b = { buf, buf + sizeof buf };
    u64 v;
    switch (id) {
    case 5:   /* service discovery request */
        st.state = AA_DISCOVERY;
        log_s("aa: service discovery request\n");
        send_service_discovery();
        break;
    case 11:   /* ping request: echo the timestamp */
        if (!pb_get(body, bn, 1, &v))
            pb_u(&b, 1, v);
        send_pb(CH_CONTROL, 0, 12, &b, buf);
        break;
    case 13:   /* navigation focus request -> projected */
        pb_u(&b, 1, 2);
        send_pb(CH_CONTROL, 0, 14, &b, buf);
        break;
    case 15:   /* shutdown request */
        send_pb(CH_CONTROL, 0, 16, &b, buf);
        st.state = AA_CLOSED;
        log_s("aa: phone asked to shut down\n");
        break;
    case 18: {   /* audio focus request: grant what was asked (1 gain, 2 transient, 3 duck, 4 release) */
        u64 req = 1;
        pb_get(body, bn, 1, &req);
        pb_u(&b, 1, req == 4 ? 3 : req == 2 ? 2 : 1);   /* release -> loss, transient -> transient, else gain */
        send_pb(CH_CONTROL, 0, 19, &b, buf);
        break;
    }
    default:
        st.unknown++;
        break;
    }
}

static void on_channel(int ch, int ctrl, const u8 *m, int n)
{
    u16 id = (u16)(m[0] << 8 | m[1]);
    const u8 *body = m + 2;
    int bn = n - 2;
    u8 buf[64];
    struct pb b = { buf, buf + sizeof buf };
    u64 v;
    if (ctrl && id == 7) {   /* channel open request */
        pb_u(&b, 1, 0);
        send_pb(ch, 1, 8, &b, buf);
        if (ch < CH_COUNT)
            ch_open[ch] = 1;
        st.channels_open++;
        if (ch == CH_VIDEO)
            st.state = AA_RUNNING;
        return;
    }
    if (ch == CH_VIDEO || ch == CH_MEDIA_AUDIO || ch == CH_SPEECH_AUDIO || ch == CH_SYSTEM_AUDIO) {
        if (id == 0x8000) {   /* setup -> ready, AA_MAX_UNACKED frames in flight, config 0 */
            pb_u(&b, 1, 2);
            pb_u(&b, 2, AA_MAX_UNACKED);
            pb_u(&b, 3, 0);
            send_pb(ch, 0, 0x8003, &b, buf);
            if (ch == CH_VIDEO) {   /* and take video focus */
                struct pb f = { buf + 32, buf + sizeof buf };
                pb_u(&f, 1, 1);
                pb_u(&f, 2, 0);
                send_pb(ch, 0, 0x8008, &f, buf + 32);
            }
        } else if (id == 0x8001) {   /* start: remember the session for acks */
            if (!pb_get(body, bn, 1, &v))
                session[ch] = (u32)v;
        } else if (id == 0x8007) {   /* video focus request */
            pb_u(&b, 1, 1);
            pb_u(&b, 2, 0);
            send_pb(ch, 0, 0x8008, &b, buf);
        } else if (id == 0x0000 || id == 0x0001) {   /* media data (with / without timestamp) */
            u64 ts = 0;
            int off = 0;
            if (id == 0 && bn >= 8) {
                for (int i = 0; i < 8; i++)
                    ts = ts << 8 | body[i];
                off = 8;
            }
            if (ch == CH_VIDEO) {
                st.video_frames++;
                st.video_bytes += (u32)(bn - off);
                ack_media(ch, session[ch]);   /* before decoding: the phone's next frame is on its way meanwhile */
                if (pf->video) {
                    u64 t0 = pf->now_us ? pf->now_us() : 0;
                    pf->video(body + off, bn - off, ts);
                    if (pf->now_us)
                        st.video_us += pf->now_us() - t0;
                }
            } else {
                st.audio_frames++;
                if (pf->audio)
                    pf->audio(ch == CH_MEDIA_AUDIO ? 0 : ch == CH_SPEECH_AUDIO ? 1 : 2, body + off, bn - off);
                ack_media(ch, session[ch]);
            }
        }
        return;
    }
    if (ch == CH_INPUT && id == 0x8002) {   /* binding request -> ok */
        pb_u(&b, 1, 0);
        send_pb(ch, 0, 0x8003, &b, buf);
        return;
    }
    if (ch == CH_SENSOR && id == 0x8001) {   /* sensor start -> ok, then report parked + day */
        pb_u(&b, 1, 0);
        send_pb(ch, 0, 0x8002, &b, buf);
        struct pb e = { buf, buf + sizeof buf };
        u8 *ds = pb_open(&e, 13);   /* driving status: 0 unrestricted */
        pb_u(&e, 1, 0);
        pb_close(&e, ds);
        send_pb(ch, 0, 0x8003, &e, buf);
        return;
    }
    st.unknown++;
}

/* a complete message arrived on a channel (decrypted if needed) */
static void on_message(int ch, int flags, const u8 *m, int n)
{
    if (n < 2)
        return;
    st.rx_msgs++;
    if (ch == CH_CONTROL && !(flags & F_CONTROL))
        on_control(m, n);
    else
        on_channel(ch, flags & F_CONTROL, m, n);
}

static void tls_progress(void)
{
    int r = aa_tls_handshake();
    static u8 rec[0x4000];
    size_t k;
    while ((k = aa_tls_take(rec, sizeof rec))) {   /* handshake records go back as plain message 3 */
        send_msg(CH_CONTROL, 0, 0, 3, rec, (int)k);
    }
    if (r < 0) {
        st.state = AA_ERROR;
        log_s("aa: TLS handshake failed: ");
        log_s(aa_tls_error());
        log_s("\n");
    } else if (r == 0 && st.state == AA_TLS) {
        u8 buf[8];
        struct pb b = { buf, buf + sizeof buf };
        pb_u(&b, 1, 0);   /* auth complete, status OK (plain) */
        send_msg(CH_CONTROL, 0, 0, 4, buf, (int)(b.p - buf));
        st.state = AA_AUTHED;
        log_s("aa: TLS up (");
        log_s(aa_tls_cipher());
        log_s("), auth complete sent\n");
    }
}

/* a complete frame payload for channel ch (already reassembled) */
static void on_payload(int ch, int flags, const u8 *p, int n)
{
    if (!(flags & F_ENC)) {
        if (ch == CH_CONTROL && n >= 2 && p[0] == 0 && p[1] == 2) {   /* version response */
            st.version_major = n >= 4 ? p[2] << 8 | p[3] : 0;
            st.version_minor = n >= 6 ? p[4] << 8 | p[5] : 0;
            int status = n >= 8 ? p[6] << 8 | p[7] : -1;
            if (status) {
                st.state = AA_ERROR;
                log_s("aa: version mismatch\n");
                return;
            }
            st.state = AA_TLS;
            tls_progress();   /* sends the ClientHello */
            return;
        }
        if (ch == CH_CONTROL && n >= 2 && p[0] == 0 && p[1] == 3) {   /* SSL handshake records from the phone */
            aa_tls_feed(p + 2, (size_t)(n - 2));
            tls_progress();
            return;
        }
        on_message(ch, flags, p, n);
        return;
    }
    if (aa_tls_feed(p, (size_t)n)) {
        st.errors++;
        return;
    }
    for (;;) {
        u64 t0 = pf->now_us ? pf->now_us() : 0;
        int k = aa_tls_read(plain, sizeof plain);
        if (pf->now_us)
            st.decrypt_us += pf->now_us() - t0;
        if (k <= 0) {
            if (k < 0)
                st.errors++;
            break;
        }
        on_message(ch, flags, plain, k);
    }
}

/* append one frame's plaintext (decrypted if F_ENC) to dst[at..max); the new length, or -1 (message dropped) */
static int frame_plain(int flags, const u8 *p, int n, u8 *dst, int at, int max)
{
    if (!(flags & F_ENC)) {
        if (at + n > max)
            return st.errors++, -1;
        memcpy(dst + at, p, (size_t)n);
        return at + n;
    }
    if (aa_tls_feed(p, (size_t)n))
        return st.errors++, -1;
    u64 t0 = pf->now_us ? pf->now_us() : 0;
    for (;;) {
        if (pf->now_us)
            st.decrypt_us += pf->now_us() - t0, t0 = pf->now_us();
        int k = aa_tls_read(dst + at, (size_t)(max - at));
        if (k < 0)
            return st.errors++, -1;
        if (k == 0)
            return at;
        at += k;
        if (at == max)
            return st.errors++, -1;   /* no room left: the rest would not fit */
    }
}

/* ---- frame reassembly from the raw bulk byte stream ---- */
static u8 rxbuf[0x4000 * 2 + 64] AA_BIG;
static int rxn;
#ifndef AA_VIDEO_MSG_MAX
#define AA_VIDEO_MSG_MAX 0x60000   /* largest multi-frame message (an IDR frame at 800x480 is well under this) */
#endif
static u8 big_video[AA_VIDEO_MSG_MAX] AA_BIG, big_other[CH_COUNT][0x4000] AA_BIG;
static int bign[CH_COUNT];
static u8 *bigbuf(int ch) { return ch == CH_VIDEO ? big_video : big_other[ch]; }
static int bigmax(int ch) { return ch == CH_VIDEO ? (int)sizeof big_video : (int)sizeof big_other[0]; }

void aa_rx(const u8 *p, int n)
{
    while (n > 0) {
        int k = n < (int)sizeof rxbuf - rxn ? n : (int)sizeof rxbuf - rxn;
        memcpy(rxbuf + rxn, p, (size_t)k);
        rxn += k, p += k, n -= k;
        for (;;) {
            if (rxn < 4)
                break;
            int ch = rxbuf[0], flags = rxbuf[1], size = rxbuf[2] << 8 | rxbuf[3];
            int hdr = (flags & 3) == F_FIRST ? 8 : 4;
            if (rxn < hdr + size)
                break;
            const u8 *pay = rxbuf + hdr;
            st.rx_frames++;
            if (ch >= CH_COUNT) {
                st.errors++;
            } else if ((flags & 3) == F_WHOLE) {
                on_payload(ch, flags, pay, size);
            } else {
                /* a message in several frames: each frame of an encrypted one is its own TLS record (at most 16 KB of
                 * plaintext), so decrypt frame by frame and assemble the plaintext */
                if ((flags & 3) == F_FIRST)
                    bign[ch] = 0;
                if (bign[ch] >= 0)
                    bign[ch] = frame_plain(flags, pay, size, bigbuf(ch), bign[ch], bigmax(ch));
                if ((flags & 3) == F_LAST) {
                    if (bign[ch] >= 2)
                        on_message(ch, flags, bigbuf(ch), bign[ch]);
                    bign[ch] = 0;
                }
            }
            memmove(rxbuf, rxbuf + hdr + size, (size_t)(rxn - hdr - size));
            rxn -= hdr + size;
        }
    }
}

int aa_start(const struct aa_platform *p)
{
    pf = p;
    memset(&st, 0, sizeof st);
    memset(ch_open, 0, sizeof ch_open);
    memset(session, 0, sizeof session);
    memset(bign, 0, sizeof bign);
    rxn = 0;
    if (aa_tls_init(p->cert, p->cert_len, p->key, p->key_len)) {
        log_s("aa: TLS init failed: ");
        log_s(aa_tls_error());
        log_s("\n");
        st.state = AA_ERROR;
        return -1;
    }
    st.state = AA_VERSION;
    u8 ver[4] = { 0, 1, 0, (u8)(p->ver_minor ? p->ver_minor : 1) };   /* protocol 1.x */
    return send_msg(CH_CONTROL, 0, 0, 1, ver, 4);
}

void aa_closed(void) { st.state = AA_CLOSED; }

int aa_touch(int action, int x, int y)
{
    if (st.state != AA_RUNNING)
        return -1;
    static u8 buf[64];
    struct pb b = { buf, buf + sizeof buf };
    pb_u(&b, 1, pf->now_us ? pf->now_us() * 1000ull : 0);   /* timestamp, ns */
    u8 *te = pb_open(&b, 3);   /* touch event */
    u8 *pt = pb_open(&b, 1);   /* pointer */
    pb_u(&b, 1, (u64)x);
    pb_u(&b, 2, (u64)y);
    pb_u(&b, 3, 0);            /* pointer id */
    pb_close(&b, pt);
    pb_u(&b, 2, 0);            /* action index */
    pb_u(&b, 3, (u64)action);  /* 0 down, 1 up, 2 move */
    pb_close(&b, te);
    return send_pb(CH_INPUT, 0, 0x8001, &b, buf);
}

const struct aa_status *aa_status(void) { return &st; }
