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
};

struct aa_status {
    enum aa_state state;
    unsigned int version_major, version_minor, rx_frames, tx_frames, rx_msgs, channels_open, video_frames, video_bytes,
        audio_frames, unknown, errors;
};

int aa_start(const struct aa_platform *p);   /* sends the version request */
void aa_rx(const unsigned char *p, int n);   /* bytes from the phone's bulk IN, any chunking */
int aa_touch(int action, int x, int y);      /* 0 down, 1 up, 2 move; video coordinates (800x480) */
const struct aa_status *aa_status(void);
#endif
