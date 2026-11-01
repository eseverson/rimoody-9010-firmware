/* See mcu.h. Commands seen so far (2026-10-31): from the MCU a2 ACC state, a4 "send me the time", a5 version string,
 * a1 h m (its RTC; minutes since power-on while unset, every 60 s), a8 y m d, 00 shutdown request (ACC off). eCos
 * answers a4 with a1 h m s, a8 y m d and, ~30 ms later, 11 01; it answers 00 with e8 01, saves, sends 00 and halts.
 * Never sent here unprompted: ad.., 00, 11 00, e5 03. */
#include "mcu.h"

cu32 mcu_crc16(const cu8 *p, int n)
{
    cu32 c = 0;
    while (n-- > 0) {
        c ^= (cu32)*p++ << 8;
        for (int k = 0; k < 8; k++)
            c = (c & 0x8000) ? ((c << 1) ^ 0x8432) & 0xffff : (c << 1) & 0xffff;
    }
    return c;
}

int mcu_frame(const cu8 *p, int n, cu8 *o)
{
    int s = n;
    o[0] = 0xff, o[1] = 0xaa, o[2] = (cu8)n;
    for (int i = 0; i < n; i++)
        o[3 + i] = p[i], s += p[i];
    o[3 + n] = (cu8)-s;
    cu32 c = mcu_crc16(o, 4 + n);
    o[4 + n] = (cu8)(c >> 8), o[5 + n] = (cu8)c;   /* SoC side: big-endian */
    return 6 + n;
}

void mcu_init(struct mcu *m)
{
    cu8 *p = (cu8 *)m;
    for (unsigned i = 0; i < sizeof(*m); i++)
        p[i] = 0;
}

static void hist_add(struct mcu *m, int dir, const cu8 *p, int n, cu32 now)
{
    struct mcu_hist *h = &m->hist[m->hist_n++ % MCU_HIST];
    h->t = now, h->dir = (cu8)dir, h->len = (cu8)n;
    for (int i = 0; i < n && i < (int)sizeof(h->p); i++)
        h->p[i] = p[i];
}

static int hexs(char *o, const cu8 *p, int n)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        k += plat_fmt(o + k, " %02x", p[i]);
    return k;
}

int mcu_send(struct mcu *m, const cu8 *p, int n, cu32 now)
{
    cu8 f[6 + MCU_MAX_PAYLOAD];
    if (n < 1 || n > MCU_MAX_PAYLOAD)
        return -1;
    int len = mcu_frame(p, n, f);
    plat_mcu_write(f, len);
    m->tx_frames++;
    hist_add(m, 't', p, n, now);
    char t[3 * MCU_MAX_PAYLOAD + 1];
    hexs(t, p, n);
    plat_logf("mcu: t=%u tx%s\n", now, t);
    return 0;
}

int mcu_tx_allowed(const cu8 *p, int n)
{
    if (n < 1 || n > MCU_MAX_PAYLOAD || p[0] == 0xad || (n == 1 && p[0] == 0x00))
        return 0;
    if (n == 2 && ((p[0] == 0x11 && p[1] == 0x00) || (p[0] == 0xe5 && p[1] == 0x03)))
        return 0;
    return 1;
}

/* ---- wall clock: seconds since 2000-01-01 00:00:00 (years 2000-2099) ---- */
static int leap(int y) { return y % 4 == 0; }

static int mdays(int y, int m)
{
    static const cu8 d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m - 1] + (m == 2 && leap(y));
}

static cu32 to_sec(const cu8 v[6])
{
    int y = 2000 + v[0];
    cu32 days = 0;
    for (int yy = 2000; yy < y; yy++)
        days += leap(yy) ? 366 : 365;
    for (int mm = 1; mm < v[1]; mm++)
        days += mdays(y, mm);
    days += v[2] - 1;
    return days * 86400 + v[3] * 3600 + v[4] * 60 + v[5];
}

static void from_sec(cu32 s, cu8 v[6])
{
    cu32 days = s / 86400, r = s % 86400;
    int y = 2000, m = 1;
    while (days >= (cu32)(leap(y) ? 366 : 365))
        days -= leap(y) ? 366 : 365, y++;
    while (days >= (cu32)mdays(y, m))
        days -= mdays(y, m), m++;
    v[0] = (cu8)(y - 2000), v[1] = (cu8)m, v[2] = (cu8)(days + 1);
    v[3] = (cu8)(r / 3600), v[4] = (cu8)(r / 60 % 60), v[5] = (cu8)(r % 60);
}

static void clock_now(const struct mcu *m, cu32 now, cu8 v[6])
{
    from_sec(m->clock_sec + (now - m->clock_ms) / 1000, v);
}

static void send_time(struct mcu *m, const cu8 v[6], cu32 now)
{
    cu8 a1[4] = { 0xa1, v[3], v[4], v[5] }, a8[4] = { 0xa8, v[0], v[1], v[2] };
    mcu_send(m, a1, 4, now);
    mcu_send(m, a8, 4, now);
}

void mcu_settime(struct mcu *m, const cu8 v[6], cu32 now)
{
    m->clock_sec = to_sec(v), m->clock_ms = now, m->clock_set = 1;
    plat_logf("mcu: wall clock set to 20%02u-%02u-%02u %02u:%02u:%02u\n", v[0], v[1], v[2], v[3], v[4], v[5]);
    send_time(m, v, now);
}

/* ---- receive ---- */
static void handle(struct mcu *m, const cu8 *p, int n, cu32 now)
{
    char t[3 * MCU_MAX_PAYLOAD + 1];
    hexs(t, p, n);
    plat_logf("mcu: t=%u rx%s\n", now, t);
    hist_add(m, 'r', p, n, now);
    m->frames++;
    switch (p[0]) {
    case 0xa2:
        m->acc_len = n - 1 > (int)sizeof(m->acc) ? (int)sizeof(m->acc) : n - 1;
        for (int i = 0; i < m->acc_len; i++)
            m->acc[i] = p[1 + i];
        break;
    case 0xa5: {
        int k = 0;
        for (int i = 1; i < n && k < (int)sizeof(m->version) - 1; i++)
            m->version[k++] = p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '?';
        m->version[k] = 0;
        break;
    }
    case 0xa1:
        if (n >= 3)
            m->hm[0] = p[1], m->hm[1] = p[2], m->have_time = 1;
        break;
    case 0xa8:
        if (n >= 4)
            m->ymd[0] = p[1], m->ymd[1] = p[2], m->ymd[2] = p[3], m->have_date = 1;
        break;
    case 0xa4:
        m->a4_pending = 1, m->a4_at = now;
        break;
    case 0x00:
        if (n == 1) {
            static const cu8 e8[2] = { 0xe8, 0x01 }, off[1] = { 0x00 };
            plat_logf("mcu: !!!!!!!! SHUTDOWN REQUEST (ACC off) at t=%u - answering e8 01, saving, 00 !!!!!!!!\n", now);
            m->shutdowns++;
            mcu_send(m, e8, 2, now);
            plat_mcu_shutdown();
            mcu_send(m, off, 1, now);
        }
        break;
    }
}

static void shift(struct mcu *m, int k)
{
    for (int i = k; i < m->n; i++)
        m->buf[i - k] = m->buf[i];
    m->n -= k;
}

void mcu_rx_byte(struct mcu *m, cu8 b, cu32 now)
{
    if (m->n && now - m->last_byte > 200) {   /* a partial frame went stale (bytes lost): start over */
        m->dropped += (cu32)m->n;
        m->n = 0;
    }
    m->last_byte = now;
    if (m->n >= (int)sizeof(m->buf)) {
        m->dropped++;
        shift(m, 1);
    }
    m->buf[m->n++] = b;
    for (;;) {
        if (!m->n)
            return;
        if (m->buf[0] != 0xff || (m->n >= 2 && m->buf[1] != 0xaa)) {
            m->dropped++;
            shift(m, 1);
            continue;
        }
        if (m->n < 3)
            return;
        int len = m->buf[2];
        if (!len || len > MCU_MAX_PAYLOAD) {
            m->bad++;
            shift(m, 1);
            continue;
        }
        int total = 6 + len;
        if (m->n < total)
            return;
        int s = 0;
        for (int i = 2; i < 4 + len; i++)
            s += m->buf[i];
        cu32 crc = m->buf[4 + len] | (cu32)m->buf[5 + len] << 8;   /* MCU side: little-endian */
        if ((s & 0xff) == 0 && crc == mcu_crc16(m->buf, 4 + len)) {
            cu8 p[MCU_MAX_PAYLOAD];
            for (int i = 0; i < len; i++)
                p[i] = m->buf[3 + i];
            shift(m, total);
            handle(m, p, len, now);
        } else {
            m->bad++;
            shift(m, 1);
        }
    }
}

void mcu_poll(struct mcu *m, cu32 now)
{
    if (m->a4_pending) {
        cu8 v[6];
        if (m->clock_set) {
            clock_now(m, now, v);
            plat_logf("mcu: time request, sending the wall clock\n");
        } else if (m->have_time && m->have_date) {
            v[0] = m->ymd[0], v[1] = m->ymd[1], v[2] = m->ymd[2], v[3] = m->hm[0], v[4] = m->hm[1], v[5] = 0;
            plat_logf("mcu: time request, clock unset: echoing the MCU's own time/date\n");
        } else if (now - m->a4_at > 2000) {
            plat_logf("mcu: time request, no time known: only 11 01\n");
            m->a4_pending = 0, m->ready_pending = 1, m->ready_at = now + 30;
            return;
        } else {
            return;
        }
        send_time(m, v, now);
        m->a4_pending = 0, m->ready_pending = 1, m->ready_at = now + 30;
    }
    if (m->ready_pending && (int)(now - m->ready_at) >= 0) {
        static const cu8 r[2] = { 0x11, 0x01 };
        m->ready_pending = 0;
        mcu_send(m, r, 2, now);
    }
}

int mcu_state(const struct mcu *m, char *o, cu32 now)
{
    int k = plat_fmt(o, "acc=");
    k += m->acc_len ? hexs(o + k, m->acc, m->acc_len) : plat_fmt(o + k, "?");
    k += plat_fmt(o + k, "\nversion='%s'\nmcu_time=", m->version);
    k += m->have_time ? plat_fmt(o + k, "%02u:%02u", m->hm[0], m->hm[1]) : plat_fmt(o + k, "?");
    k += plat_fmt(o + k, " mcu_date=");
    k += m->have_date ? plat_fmt(o + k, "20%02u-%02u-%02u", m->ymd[0], m->ymd[1], m->ymd[2]) : plat_fmt(o + k, "?");
    k += plat_fmt(o + k, "\nclock=");
    if (m->clock_set) {
        cu8 v[6];
        clock_now(m, now, v);
        k += plat_fmt(o + k, "20%02u-%02u-%02u %02u:%02u:%02u", v[0], v[1], v[2], v[3], v[4], v[5]);
    } else {
        k += plat_fmt(o + k, "unset");
    }
    k += plat_fmt(o + k, "\nrx_frames=%u tx_frames=%u bad=%u dropped=%u shutdowns=%d now=%u\n", m->frames,
                  m->tx_frames, m->bad, m->dropped, m->shutdowns, now);
    int first = m->hist_n > MCU_HIST ? m->hist_n - MCU_HIST : 0;
    for (int i = first; i < m->hist_n; i++) {
        const struct mcu_hist *h = &m->hist[i % MCU_HIST];
        k += plat_fmt(o + k, "%u %s", h->t, h->dir == 'r' ? "rx" : "tx");
        k += hexs(o + k, h->p, h->len < (int)sizeof(h->p) ? h->len : (int)sizeof(h->p));
        k += plat_fmt(o + k, "\n");
    }
    return k;
}
