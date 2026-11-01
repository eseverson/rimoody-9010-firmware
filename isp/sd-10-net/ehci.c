/* Our own EHCI host transfers (notes/rtos.md milestone 3), v1: a drop-in for U-Boot's usb_control_msg and
 * usb_bulk_msg on the Wi-Fi dongle's controller, after U-Boot's `usb start` has powered the PHY and enumerated it.
 *
 * Controller: EHCI1 (the dongle's bus 1), capability registers 0x9C104100, operational 0x9C104120 (read live from
 * U-Boot's ehci_ctrl[1] = {hccr, hcor} at link address 0x123B540 + 0x140). HCSPARAMS 0x1101: one port; the dongle is
 * high speed on the root port, so no split transactions. EHCI0 (0x9C102100) is the other bus.
 *
 * v1 runs each transfer the way U-Boot 2014.04 ehci_submit_async does, so it can be interleaved with U-Boot's own
 * transfers while the two are compared: a halted head QH (H = 1) and one transfer QH (DTC = 1, data toggle from the
 * qTDs, the toggle bits shared with U-Boot in struct usb_device), the async schedule enabled only for the transfer
 * and disabled again before the CPU looks at the results. ASYNCLISTADDR is saved and restored around it. The
 * controller never runs while the CPU edits descriptors, so plain cache write-back / invalidate is enough.
 * v2 adds ehci_rx_poll: one bulk IN kept queued on its own async ring (rhead -> rqh -> rhead) with the schedule
 * left running, so the net task polls a qTD token instead of blocking. Any other transfer pauses that ring (ASE off,
 * wait for ASS = 0; the controller keeps the partly filled IN in the QH overlay), runs the v1 way, then resumes it.
 * A finished or stopped IN takes the ring down again (ASYNCLISTADDR back to U-Boot's list), so the descriptors are
 * only ever rewritten with the schedule stopped. ehci_rx_stop must run before U-Boot touches USB or memory is reused.
 *
 * v3 (ehci_rx_poll): a ring of RQN queued bulk INs instead of one, see below.
 *
 * DMA buffers: used in place when cache-line aligned in address and length, otherwise through a bounce buffer, so
 * invalidating never throws away another task's writes that share a line. */
#include "ehci.h"
#include "usbdev.h"
#include "clk.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define REG(a) (*(volatile u32 *)(a))
#define RD32(p) (*(volatile u32 *)(p))

#define HCOR     0x9c104120u
#define USBCMD   (HCOR + 0x00)
#define USBSTS   (HCOR + 0x04)
#define ASYNCLA  (HCOR + 0x18)
#define PORTSC1  (HCOR + 0x44)
#define CMD_ASE  (1u << 5)
#define STS_ASS  (1u << 15)

#define T_BIT    1u          /* terminate */
#define TYPE_QH  (1u << 1)
#define TK_ACTIVE 0x80u
#define TK_HALTED 0x40u
#define TK_DBERR  0x20u
#define TK_BABBLE 0x10u
#define TK_XACT   0x08u
#define PID_OUT 0
#define PID_IN 1
#define PID_SETUP 2

struct qtd { volatile u32 next, alt, token, buf[5]; };                       /* 32 bytes */
struct qh { volatile u32 link, ep1, ep2, cur, ov_next, ov_alt, ov_token, ov_buf[5], pad[4]; };   /* 64 bytes */

#define NQTD 8
static struct qh head __attribute__((aligned(64))), xqh __attribute__((aligned(64)));
static struct qtd qtd[NQTD] __attribute__((aligned(64)));
static u8 setup_pkt[64] __attribute__((aligned(64)));
#define BOUNCE_LEN 20480
static u8 bounce[BOUNCE_LEN] __attribute__((aligned(4096)));
static u32 qtd_len[NQTD];

struct ehci_stats ehci_stats;

/* ---- cache maintenance (line size from DCM_CFG, as net.c's dcache_wb) ---- */
static u32 line(void)
{
    u32 cfg;
    __asm__ volatile("mfsr %0, $dcm_cfg" : "=r"(cfg));
    u32 f = (cfg >> 6) & 7;
    return f ? 8u << (f - 1) : 32;
}

static void cache_op(const volatile void *p, u32 n, int op)
{
    u32 l = line(), a = (u32)p;
    for (u32 x = a & ~(l - 1); x < a + n; x += l) {
        if (op == 0)
            __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(x) : "memory");
        else if (op == 1)
            __asm__ volatile("cctl %0, L1D_VA_INVAL" ::"r"(x) : "memory");
        else   /* the N13 has no L1D_VA_WBINVAL (instruction exception, crash 20261031-194305): WB, then INVAL */
            __asm__ volatile("cctl %0, L1D_VA_WB\n\tcctl %0, L1D_VA_INVAL" ::"r"(x) : "memory");
    }
    __asm__ volatile("msync all" ::: "memory");
}
#define WB(p, n)    cache_op(p, n, 0)
#define INV(p, n)   cache_op(p, n, 1)
#define WBINV(p, n) cache_op(p, n, 2)

static int wait_reg(u32 reg, u32 mask, u32 want, u32 ms)
{
    u32 t0 = clk_ms();
    while ((REG(reg) & mask) != want)
        if (clk_ms() - t0 > ms)
            return -1;
    return 0;
}

static void fill(int i, int pid, int dt, u32 addr, u32 len, int ioc)
{
    struct qtd *q = &qtd[i];
    q->next = T_BIT;
    q->alt = T_BIT;
    q->token = (u32)dt << 31 | len << 16 | (u32)ioc << 15 | 3u << 10 | (u32)pid << 8 | TK_ACTIVE;
    q->buf[0] = addr;
    for (int k = 1; k < 5; k++)
        q->buf[k] = (addr & ~0xfffu) + 0x1000u * k;
    qtd_len[i] = len;
}

static u32 status_of(u32 token)   /* as U-Boot's ehci_submit_async maps the overlay status */
{
    u32 s = token & 0xfc & ~TK_ACTIVE;   /* drop split-state and ping */
    if (!s)
        return 0;
    if (s & TK_BABBLE)
        return USB_ST_BABBLE_DET;
    if (s & TK_DBERR)
        return USB_ST_BUF_ERR;
    if (s == TK_HALTED)
        return USB_ST_STALLED;
    return USB_ST_CRC_ERR;
}

/* One transfer on `pipe` (U-Boot encoding: type 31:30, ep 18:15, dev 14:8, IN bit 7). setup = 8 bytes or 0.
 * Returns the bytes transferred, or -1 with UDEV_STATUS set. */
static int xfer1(u8 *dev, u32 pipe, const u8 *setup, void *data, u32 len, int timeout_ms)
{
    u32 devnum = (pipe >> 8) & 0x7f, ep = (pipe >> 15) & 0xf, in = (pipe & 0x80) != 0, ctrl = (pipe >> 30) == 2;
    u32 maxp = RD32(dev + (in ? UDEV_EPMAX_IN : UDEV_EPMAX_OUT) + 4 * ep);
    RD32(dev + UDEV_STATUS) = USB_ST_NOT_PROC;
    RD32(dev + UDEV_ACT_LEN) = 0;
    if (RD32(dev + UDEV_SPEED) != 3 || !maxp || maxp > 1024 || (len && !data)) {
        ehci_stats.refused++;
        return -1;
    }
    u32 l = line(), a = (u32)data;
    int direct = !((a | len) & (l - 1));
    if (!direct && len > BOUNCE_LEN) {
        ehci_stats.refused++;
        return -1;
    }
    u32 buf = direct ? a : (u32)bounce;
    if (len) {
        if (!in && !direct)
            for (u32 i = 0; i < len; i++)
                bounce[i] = ((const u8 *)data)[i];
        if (in)
            WBINV((void *)buf, len);   /* nothing dirty may be evicted over the DMA data later */
        else
            WB((void *)buf, len);
    }

    int n = 0, first_data = -1, last_data = -1;
    if (setup) {
        for (int i = 0; i < 8; i++)
            setup_pkt[i] = setup[i];
        WB(setup_pkt, 8);
        fill(n++, PID_SETUP, 0, (u32)setup_pkt, 8, 0);
    }
    u32 tg = ctrl ? 1 : (RD32(dev + UDEV_TOGGLE + 4 * !in) >> ep) & 1;
    for (u32 done = 0; done < len || (!ctrl && !len && n == 0);) {
        if (n >= NQTD - 1) {
            ehci_stats.refused++;
            return -1;
        }
        u32 addr = buf + done, room = 0x5000 - (addr & 0xfff), chunk = len - done;
        if (chunk > room)
            chunk = room / maxp * maxp;
        if (first_data < 0)
            first_data = n;
        last_data = n;
        fill(n++, in ? PID_IN : PID_OUT, (int)tg, addr, chunk, 0);
        tg ^= ((chunk + maxp - 1) / maxp) & 1;
        done += chunk;
        if (!len)
            break;   /* zero-length bulk packet */
    }
    int status_q = -1;
    if (ctrl) {
        status_q = n;
        fill(n++, len && in ? PID_OUT : PID_IN, 1, 0, 0, 0);
    }
    for (int i = 0; i + 1 < n; i++) {
        qtd[i].next = (u32)&qtd[i + 1];
        if (in && i >= first_data && i <= last_data)   /* a short IN packet ends the data stage */
            qtd[i].alt = status_q >= 0 ? (u32)&qtd[status_q] : T_BIT;
    }
    qtd[n - 1].token |= 1u << 15;   /* IOC */

    head.link = (u32)&xqh | TYPE_QH;
    head.ep1 = 1u << 15 | 2u << 12;   /* H, high speed */
    head.ep2 = 1u << 30;
    head.cur = 0;
    head.ov_next = T_BIT;
    head.ov_alt = T_BIT;
    head.ov_token = TK_HALTED;
    xqh.link = (u32)&head | TYPE_QH;
    xqh.ep1 = 8u << 28 | maxp << 16 | 1u << 14 | 2u << 12 | ep << 8 | devnum;   /* RL 8, DTC, HS */
    xqh.ep2 = 1u << 30;   /* mult 1 */
    xqh.cur = 0;
    xqh.ov_next = (u32)&qtd[0];
    xqh.ov_alt = T_BIT;
    xqh.ov_token = 0;
    for (int k = 0; k < 5; k++)
        xqh.ov_buf[k] = 0;
    WB(&head, sizeof head);
    WB(&xqh, sizeof xqh);
    WB(qtd, sizeof qtd);

    if ((REG(USBCMD) & CMD_ASE) || wait_reg(USBSTS, STS_ASS, 0, 100)) {
        ehci_stats.busy++;
        return -1;
    }
    u32 saved = REG(ASYNCLA);
    REG(ASYNCLA) = (u32)&head;
    REG(USBSTS) = REG(USBSTS) & 0x3f;
    REG(USBCMD) = REG(USBCMD) | CMD_ASE;
    int up = wait_reg(USBSTS, STS_ASS, STS_ASS, 100);

    u32 t0 = clk_ms(), fin = 0;
    while (!up) {
        INV(qtd, sizeof qtd);
        fin = 1;
        for (int i = 0; i < n; i++) {
            u32 tk = qtd[i].token;
            if (tk & TK_ACTIVE) {
                fin = 0;
                break;
            }
            if (tk & TK_HALTED)
                break;
            if (in && i >= first_data && i <= last_data && (tk >> 16 & 0x7fff) && status_q < 0)
                break;   /* short bulk IN */
        }
        if (fin || (int)(clk_ms() - t0) > timeout_ms)
            break;
    }
    REG(USBCMD) = REG(USBCMD) & ~CMD_ASE;
    wait_reg(USBSTS, STS_ASS, 0, 100);
    REG(ASYNCLA) = saved;

    INV(&xqh, sizeof xqh);
    INV(qtd, sizeof qtd);
    u32 tok = xqh.ov_token;
    if (up || (tok & TK_ACTIVE)) {
        ehci_stats.timeouts++;
        return -1;   /* UDEV_STATUS stays NOT_PROC, as U-Boot's "timed out on TD" */
    }
    if (!ctrl)
        RD32(dev + UDEV_TOGGLE + 4 * !in) = (RD32(dev + UDEV_TOGGLE + 4 * !in) & ~(1u << ep)) | (tok >> 31) << ep;
    u32 st = status_of(tok), act = 0;
    for (int i = first_data; i >= 0 && i <= last_data; i++) {
        u32 tk = qtd[i].token;
        if (tk & TK_ACTIVE)
            break;
        act += qtd_len[i] - (tk >> 16 & 0x7fff);
        if (tk >> 16 & 0x7fff)
            break;
    }
    if (in && act) {
        INV((void *)buf, act);
        if (!direct)
            for (u32 i = 0; i < act; i++)
                ((u8 *)data)[i] = bounce[i];
    }
    RD32(dev + UDEV_STATUS) = st;
    RD32(dev + UDEV_ACT_LEN) = act;
    if (st) {
        ehci_stats.errors++;
        return -1;
    }
    ehci_stats.ok++;
    return (int)act;
}

/* ---- v3: a ring of queued bulk INs ---- */
/* RQN transfers of RQ_LEN bytes stay queued on their own async ring (rhead -> rqh -> rhead) with the schedule left
 * running, so the controller keeps receiving into RAM while the CPU is busy elsewhere (with one queued IN, a few ms of
 * decrypting and decoding let the dongle's RX FIFO overflow: TCP retransmits, 3 Mbit/s). The dongle sends one frame
 * per bulk IN (RX aggregation off), so each transfer ends with a short packet; every qTD's alternate pointer equals its
 * next pointer, so short or full, the controller moves on round the ring. A consumed qTD is re-armed at once: the
 * controller, parked on an inactive next qTD, picks it up on a later pass (the dummy-qTD scheme Linux uses). The data
 * toggle lives in the QH overlay (DTC = 0). */
#define RQN 32
#define RQ_LEN 4096
struct rq_slot { struct qtd q; u32 pad[8]; };   /* 64 bytes: a qTD never shares a cache line with another */
static struct qh rhead __attribute__((aligned(64))), rqh __attribute__((aligned(64)));
static struct rq_slot rq[RQN] __attribute__((aligned(64)));
#ifdef __nds32__
static u8 rqbuf[RQN][RQ_LEN] __attribute__((aligned(4096), section(".bigbss")));
#else
static u8 rqbuf[RQN][RQ_LEN] __attribute__((aligned(4096)));
#endif
static int rx_armed, rq_head;
static u8 *rx_dev;
static u32 rx_pipe, rx_ep, ub_async;

static int sched_stop(void)
{
    REG(USBCMD) = REG(USBCMD) & ~CMD_ASE;
    return wait_reg(USBSTS, STS_ASS, 0, 100);
}

static int sched_start(void)
{
    REG(USBSTS) = REG(USBSTS) & 0x3f;
    REG(USBCMD) = REG(USBCMD) | CMD_ASE;
    return wait_reg(USBSTS, STS_ASS, STS_ASS, 100);
}

/* Ring stopped: save the endpoint's toggle from the overlay (once the controller has fetched a qTD) and hand
 * ASYNCLISTADDR back. Transfers received but not yet polled are dropped. */
static void rx_down(void)
{
    INV(&rqh, sizeof rqh);
    if (rqh.cur)
        RD32(rx_dev + UDEV_TOGGLE) = (RD32(rx_dev + UDEV_TOGGLE) & ~(1u << rx_ep)) | (rqh.ov_token >> 31) << rx_ep;
    REG(ASYNCLA) = ub_async;
    rx_armed = 0;
}

void ehci_rx_stop(void)   /* drops a partly received transfer */
{
    if (!rx_armed)
        return;
    sched_stop();
    rx_down();
}

static void rq_fill(int i)
{
    struct qtd *q = &rq[i].q;
    q->buf[0] = (u32)rqbuf[i];
    for (int k = 1; k < 5; k++)
        q->buf[k] = 0;
    q->token = (u32)RQ_LEN << 16 | 3u << 10 | (u32)PID_IN << 8 | TK_ACTIVE;
}

static int rx_arm(u8 *dev, u32 pipe)
{
    u32 devnum = (pipe >> 8) & 0x7f, ep = (pipe >> 15) & 0xf;
    u32 maxp = RD32(dev + UDEV_EPMAX_IN + 4 * ep);
    if (RD32(dev + UDEV_SPEED) != 3 || !maxp || maxp > 1024 || (pipe >> 30) != 3 || !(pipe & 0x80) ||
        (REG(USBCMD) & CMD_ASE)) {
        ehci_stats.refused++;
        return -1;
    }
    INV(rqbuf, sizeof rqbuf);
    for (int i = 0; i < RQN; i++) {
        u32 nx = (u32)&rq[(i + 1) % RQN].q;
        rq[i].q.next = nx;
        rq[i].q.alt = nx;
        rq_fill(i);
    }
    rq_head = 0;
    u32 tg = (RD32(dev + UDEV_TOGGLE) >> ep) & 1;
    rhead.link = (u32)&rqh | TYPE_QH;
    rhead.ep1 = 1u << 15 | 2u << 12;
    rhead.ep2 = 1u << 30;
    rhead.cur = 0;
    rhead.ov_next = T_BIT;
    rhead.ov_alt = T_BIT;
    rhead.ov_token = TK_HALTED;
    rqh.link = (u32)&rhead | TYPE_QH;
    rqh.ep1 = 8u << 28 | maxp << 16 | 2u << 12 | ep << 8 | devnum;   /* DTC = 0: toggle in the overlay */
    rqh.ep2 = 1u << 30;
    rqh.cur = 0;
    rqh.ov_next = (u32)&rq[0].q;
    rqh.ov_alt = T_BIT;
    rqh.ov_token = tg << 31;
    for (int k = 0; k < 5; k++)
        rqh.ov_buf[k] = 0;
    WB(&rhead, sizeof rhead);
    WB(&rqh, sizeof rqh);
    WB(rq, sizeof rq);
    rx_dev = dev, rx_pipe = pipe, rx_ep = ep;
    ub_async = REG(ASYNCLA);
    REG(ASYNCLA) = (u32)&rhead;
    rx_armed = 1;
    if (sched_start()) {
        ehci_stats.busy++;
        ehci_rx_stop();
        return -1;
    }
    return 0;
}

int ehci_rx_poll(void *dev, u32 pipe, void *data, int len, int *actual)
{
    *actual = 0;
    if (rx_armed && (rx_dev != dev || rx_pipe != pipe))
        ehci_rx_stop();
    if (!rx_armed)
        return rx_arm(dev, pipe) ? -1 : -2;
    struct qtd *q = &rq[rq_head].q;
    INV(q, sizeof *q);
    u32 tk = q->token;
    if (tk & TK_ACTIVE)
        return -2;
    u32 st = status_of(tk), act = RQ_LEN - (tk >> 16 & 0x7fff);
    RD32((u8 *)dev + UDEV_STATUS) = st;
    if (st || (tk & TK_HALTED)) {   /* the QH halted: take the ring down; the next poll arms a fresh one */
        ehci_stats.errors++;
        ehci_rx_stop();
        return -1;
    }
    if (act > (u32)len)
        act = (u32)len;
    if (act) {
        INV(rqbuf[rq_head], act);
        const u8 *src = rqbuf[rq_head];
        u8 *dst = data;
        for (u32 i = 0; i < act; i++)
            dst[i] = src[i];
    }
    rq_fill(rq_head);   /* back into the ring */
    WB(q, sizeof *q);
    rq_head = (rq_head + 1) % RQN;
    RD32((u8 *)dev + UDEV_ACT_LEN) = act;
    *actual = (int)act;
    ehci_stats.ok++;
    ehci_stats.rx++;
    return 0;
}

/* Any other transfer: pause the queued IN around it (or end it, for a blocking read of the same endpoint). */
static int xfer(u8 *dev, u32 pipe, const u8 *setup, void *data, u32 len, int timeout_ms)
{
    int paused = 0;
    if (rx_armed) {
        if (dev == rx_dev && (pipe & 0x80) && (pipe >> 30) == 3 && ((pipe >> 15) & 0xf) == rx_ep)
            ehci_rx_stop();
        else if (sched_stop()) {
            ehci_stats.busy++;
            return -1;
        } else
            paused = 1;
    }
    int r = xfer1(dev, pipe, setup, data, len, timeout_ms);
    if (paused) {
        REG(ASYNCLA) = (u32)&rhead;
        if (sched_start()) {
            ehci_stats.busy++;
            ehci_rx_stop();
        }
    }
    return r;
}

int ehci_ctrl(void *dev, u32 pipe, u8 req, u8 reqtype, u16 value, u16 index, void *data, u16 size, int timeout)
{
    u8 s[8] = { reqtype, req, (u8)value, (u8)(value >> 8), (u8)index, (u8)(index >> 8), (u8)size, (u8)(size >> 8) };
    return xfer(dev, pipe, s, data, size, timeout);
}

int ehci_bulk(void *dev, u32 pipe, void *data, int len, int *actual, int timeout)
{
    int r = xfer(dev, pipe, 0, data, (u32)len, timeout);
    *actual = r < 0 ? (int)RD32((u8 *)dev + UDEV_ACT_LEN) : r;
    return r < 0 ? -1 : 0;
}

u32 ehci_portsc(void) { return REG(PORTSC1); }
