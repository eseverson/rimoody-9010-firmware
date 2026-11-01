/* SoC <-> MCU link (Jieli AC6965A on UART1, 115200). Frame: ff aa LEN payload CHK8 CRC16, LEN = len(payload),
 * CHK8 = -(LEN + sum(payload)) & 0xff, CRC16 = MSB-first poly 0x8432 init 0 over ff..CHK8. The SoC sends the CRC
 * big-endian, the MCU sends it little-endian (eCos builder 0x521aa8 / parser 0x521932; tools/mcuframe.py).
 * Platform independent; test/mcu_host.c builds it on the host. */
#ifndef MCU_H
#define MCU_H
#include "crypto.h"

#define MCU_MAX_PAYLOAD 64
#define MCU_HIST 20

struct mcu_hist {
    cu32 t;
    cu8 dir, len;   /* dir: 'r' from the MCU, 't' to it */
    cu8 p[24];
};

struct mcu {
    cu8 buf[3 + MCU_MAX_PAYLOAD + 3];
    int n;
    cu32 last_byte;
    cu32 frames, tx_frames, bad, dropped;   /* bad: CHK8/CRC failures; dropped: bytes skipped while resyncing */
    cu8 acc[8];
    int acc_len;                            /* last a2 payload (after the command byte) */
    char version[MCU_MAX_PAYLOAD];          /* a5 string */
    cu8 hm[2], ymd[3];
    int have_time, have_date;               /* the MCU's own a1 (h m) and a8 (y m d) */
    int clock_set;
    cu32 clock_sec, clock_ms;               /* wall clock: seconds since 2000-01-01 at plat ms clock_ms */
    int a4_pending, ready_pending, shutdowns;
    cu32 a4_at, ready_at;
    struct mcu_hist hist[MCU_HIST];
    int hist_n;
};

void mcu_init(struct mcu *m);
void mcu_rx_byte(struct mcu *m, cu8 b, cu32 now);
void mcu_poll(struct mcu *m, cu32 now);   /* deferred replies (a4 handshake, 11 01 after 30 ms) */
int mcu_frame(const cu8 *p, int n, cu8 *out);   /* SoC-side frame (CRC big-endian); returns its length */
int mcu_send(struct mcu *m, const cu8 *p, int n, cu32 now);   /* frame, plat_mcu_write, history */
int mcu_tx_allowed(const cu8 *p, int n);
void mcu_settime(struct mcu *m, const cu8 v[6], cu32 now);   /* y-2000 m d h mi s; also sends a1 + a8 */
int mcu_state(const struct mcu *m, char *out, cu32 now);   /* text, < 3 KB */
cu32 mcu_crc16(const cu8 *p, int n);

/* platform */
void plat_mcu_write(const cu8 *frame, int n);
void plat_mcu_shutdown(void);   /* ACC off: save what matters (called between e8 01 and 00) */
void plat_logf(const char *fmt, ...);
int plat_fmt(char *out, const char *fmt, ...);

#endif
