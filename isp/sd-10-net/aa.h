/* Android Auto head-unit protocol (aa.c), transport-independent. Single link, one caller thread. */
#ifndef AA_H
#define AA_H
#include <stddef.h>

enum aa_state { AA_IDLE, AA_VERSION, AA_TLS, AA_AUTHED, AA_DISCOVERY, AA_RUNNING, AA_CLOSED, AA_ERROR };

struct aa_platform {
    int (*send)(const unsigned char *p, int n);          /* one frame to the phone's bulk OUT; <0 on error */
    void (*video)(const unsigned char *p, int n, unsigned long long ts_us);   /* H.264 Annex-B access unit */
    void (*audio)(int stream, const unsigned char *p, int n);   /* 0 media 48k stereo, 1 speech, 2 system (16k mono) */
    void (*log)(const char *s);
    unsigned long long (*now_us)(void);
    const unsigned char *cert, *key;                     /* head-unit certificate and key, PEM incl. NUL */
    size_t cert_len, key_len;
    int dpi;
    int ver_minor;    /* protocol minor version we ask for (0 = 1) */
    int hd;           /* 1: 1280 x 720 with margins for a 1024 x 600 panel; 0: 800 x 480 */
    int touch_w, touch_h;   /* touchscreen size we declare (0: the video size) */
    int audio_mask;   /* audio sinks offered: bit 0 media, 1 speech, 2 system; bit 7 = also a microphone source; 0 = none */
};

struct aa_status {
    enum aa_state state;
    unsigned int version_major, version_minor, rx_frames, tx_frames, rx_msgs, channels_open, video_frames, video_bytes,
        audio_frames, unknown, errors;
    unsigned long long decrypt_us, video_us;   /* time spent decrypting records and in the video callback */
    /* per media channel (0 video, 1 media audio, 2 speech, 3 system): frames, the largest gap between two frames and
     * the summed / largest time from our ack to the phone's next frame (about one round trip when it was waiting) */
    unsigned int m_frames[4], m_gap_max_us[4], m_rtt_max_us[4];
    unsigned long long m_rtt_sum_us[4];
    /* the last 64 video frames: arrival (ms since boot), bytes, time in the video callback (us) */
    unsigned int vlog_n, vlog_t[64], vlog_b[64], vlog_dec[64];
};

int aa_start(const struct aa_platform *p);   /* sends the version request */
void aa_rx(const unsigned char *p, int n);   /* bytes from the phone's bulk IN, any chunking */
void aa_closed(void);                        /* the transport went away */
int aa_touch(int action, int x, int y);      /* 0 down, 1 up, 2 move; the phone UI's coordinates (no margins) */
void aa_set_night(int on);                    /* headlights: the night-mode sensor (sent now if AA runs, else at sensor start) */
int aa_send_key(int keycode, int down);          /* 3 HOME, 4 BACK (the keys declared in service discovery) */
const struct aa_status *aa_status(void);
#endif
