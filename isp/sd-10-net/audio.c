/* Audio DSP bring-up and PCM output, reproducing stock eCos (notes/audio_dsp.md has the sources: every step below names
 * the eCos function it copies). Net task only.
 *
 * The DSP runs its microcode out of a DRAM window the CPU assigns: six code banks, then its data/parameter memory.
 * The microcode is read straight out of the flash mapping (0x98000000 + offset), checksummed, and copied in. The DSP
 * reads its shared memory as big-endian halfwords, so every parameter halfword is stored byte-swapped. eCos uses the
 * uncached alias (0x40000000 | addr); here the window is used through the cache and written back / invalidated
 * explicitly, which works whatever the alias does under U-Boot's setup.
 *
 * PCM goes into the "RISC sound" ring (#10): no decoder needed, big-endian s16 stereo at 48 kHz, CPU write pointer
 * 0x9C0018C8, DSP read pointer 0x9C0018CC. NEVER read 0x9C001B00..0x9C001DFF: that kills the system while audio runs
 * (eCos snapshot runs 3..5, 2026-11-02). */
#include "audio.h"
#include "clk.h"
#include "memmap.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define REG(x) (*(volatile u32 *)(0x9c000000u + (x)))

/* DSP window (memmap.h): after the FreeRTOS heap, before the LVGL heap. Same size as eCos'. */
#define AUD_BASE MM_DSP
#define AUD_SIZE MM_DSP_SZ
static const u32 bank_size[6] = { 0x33400, 0x18100, 0x18100, 0xcf00, 0xcf00, 0x14500 };   /* per-class maxima (0x3bfc7e) */
static u32 bank_addr[6], data;   /* data = DSP data memory (params at +0, App params +0x180, DSP info table +0x280) */

/* microcode in flash (0x3bf8a4's table): 0x40-byte big-endian "if" header, code after it, data blob elsewhere */
struct codec { u8 id; u32 hdr, blob; };
static const struct codec codecs[] = { { 0x00, 0x98594184u, 0x98590f04u }, { 0x1d, 0x984b90e8u, 0x984af5e8u },
                                       { 0x1c, 0x984d09d8u, 0x984c5fd8u }, { 0x01, 0x98584480u, 0 } };

/* master volume: eCos's dB table (flash 0x985c7b64, index 0..40) as DSP codes 2 * (dB + 103.5) */
static const u8 vol_code[41] = { 0,   67,  75,  83,  91,  99,  107, 111, 119, 123, 127, 131, 135, 139,
                                 141, 143, 145, 147, 149, 151, 153, 155, 157, 161, 165, 169, 173, 177,
                                 181, 185, 187, 189, 191, 193, 195, 197, 199, 201, 203, 205, 207 };

static aud_logf logf;
static void eq_apply_all(void);
/* PCM mixer state (see "PCM: a small mixer over ring 10" below) */
#define NSRC 4
#define LEAD (48 * 4 * 30)   /* 30 ms */
static u32 rpos, zpos, cur[NSRC], dropped[NSRC], taken[NSRC];
static int ready, chip651, tone_hz;
static u32 tone_phase;
/* the PCM ring in use: stream 1 (the "main" media path, through the PCM decoder: master volume and EQ apply) or, as
 * the fallback, ring 10 ("RISC sound": no decoder, big-endian, but the master volume does not reach it; owner by ear,
 * 2026-11-02) */
static u32 ring, ring_size = 0x16800, ring_w = 0x18c8, ring_r = 0x18cc, ring_be = 1, path;
static u32 cmd_timeouts, msgs, hb_last, hb_stuck, hb_seen, last_evt = 0xffffffffu;

static void dc_wb(u32 a, u32 n)
{
    for (u32 p = a & ~31u; p < a + n; p += 32)
        __asm__ volatile("cctl %0, L1D_VA_WB" ::"r"(p) : "memory");
    __asm__ volatile("msync all" ::: "memory");
}
static void dc_inval(u32 a, u32 n)
{
    for (u32 p = a & ~31u; p < a + n; p += 32)
        __asm__ volatile("cctl %0, L1D_VA_INVAL" ::"r"(p) : "memory");
    __asm__ volatile("msync all" ::: "memory");
}
static u16 bswap16(u16 v) { return (u16)(v << 8 | v >> 8); }
static u32 be32(u32 a) { const volatile u8 *p = (const volatile u8 *)a; return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void ms(u32 n) { clk_udelay(1000 * n); }

/* DSP shared memory halfwords: raw stores (eCos's "H(off) = v"), and DSP-view get/set (byte-swapped) */
static void H(u32 area, u32 off, u16 raw) { *(volatile u16 *)(area + off) = raw; dc_wb(area + off, 2); }
static u16 dsp_get(u32 area, u32 idx) { dc_inval(area + 2 * idx, 2); return bswap16(*(volatile u16 *)(area + 2 * idx)); }
static void dsp_set(u32 area, u32 idx, u16 v) { H(area, 2 * idx, bswap16(v)); }
static u16 info(u32 off) { dc_inval(data + 0x280 + off, 2); return bswap16(*(volatile u16 *)(data + 0x280 + off)); }
#define RISC (data)
#define APP (data + 0x180)

static u32 cksum(u32 a, u32 n)   /* 0x3be948: c ^= byte; rotl1 */
{
    u32 c = 0;
    const volatile u8 *p = (const volatile u8 *)a;
    for (u32 i = 0; i < n; i++) {
        c ^= p[i];
        c = c << 1 | c >> 31;
    }
    return c;
}
static const struct codec *codec(int id)
{
    for (u32 i = 0; i < sizeof codecs / sizeof codecs[0]; i++)
        if (codecs[i].id == id)
            return &codecs[i];
    return 0;
}

/* ---- mailbox ---- */

static void service_msgs(void)   /* 0x3be80c: DSP -> CPU message pending when 0x18A8 bit 31 is clear; acknowledge it */
{
    u32 m = REG(0x18a8);
    if (!(m & 0x80000000u)) {
        msgs++;
        if (msgs <= 16 && logf)
            logf("audio: DSP message %08x %08x\n", m, REG(0x18ac));
        REG(0x18a8) = m | 0x80000000u;
    }
}

static int send_cmd(u32 cmd, u32 p1, u32 p2, u32 p3)   /* DSP_SendCmd 0x3bbf74 */
{
    for (int i = 0; (REG(0x1784) & 1) || !(REG(0x1880) & 0x80000000u); i++) {
        if (i >= 1000) {
            cmd_timeouts++;
            if (logf)
                logf("audio: cmd %u/%u/%u busy (1784 %x 1880 %08x)\n", cmd, p1, p2, REG(0x1784), REG(0x1880));
            return -1;
        }
        service_msgs();
        ms(1);
    }
    REG(0x1884) = p3;
    REG(0x1880) = cmd << 24 | p1 << 16 | p2;
    REG(0x1784) |= 1;
    for (int i = 0; i < 1000; i++) {
        if (REG(0x1880) & 0x80000000u)
            return 0;
        service_msgs();
        ms(1);
    }
    cmd_timeouts++;
    if (logf)
        logf("audio: cmd %u/%u/%u/%x not acknowledged (1880 %08x)\n", cmd, p1, p2, p3, REG(0x1880));
    return -1;
}

static int state(int type) { return (int)(REG(0x18b0) >> (4 * type) & (type == 7 ? 3u : 15u)); }
static int wait_state(int type, int st)   /* DSP_WaitState 0x3bb2ec */
{
    for (int i = 0; i < 1000; i++) {
        if (state(type) == st)
            return 0;
        service_msgs();
        if (state(0) == 4) {
            if (logf)
                logf("audio: DSP is dead (18b0 %08x)\n", REG(0x18b0));
            return -2;
        }
        ms(1);
    }
    if (logf)
        logf("audio: wait type %d state %d timed out (18b0 %08x)\n", type, st, REG(0x18b0));
    return -1;
}

static void mailbox_clear(void)   /* 0x3bd262 */
{
    for (int i = 0; i < 31; i++)
        REG(0x1880 + 4 * i) = 0;
    REG(0x1880) = 0x80000000u, REG(0x18a8) = 0x80000000u, REG(0x18f8) = 0x80000000u;
}

/* ---- microcode ---- */

static int download_code(int id, int bank)   /* DownloadCode 0x3bafe0 */
{
    const struct codec *c = codec(id);
    if (!c || be32(c->hdr) >> 16 != 0x6966)
        return -1;
    u32 n = be32(c->hdr + 8);
    if (cksum(c->hdr + 0x40, n) != be32(c->hdr + 0x14) || n > bank_size[bank]) {
        if (logf)
            logf("audio: codec %02x code checksum/size mismatch\n", id);
        return -1;
    }
    if (bank >= 1)
        REG(0x1744 + 4 * bank) &= 0x60ffffffu;
    const volatile u8 *s = (const volatile u8 *)(c->hdr + 0x40);
    u8 *d = (u8 *)bank_addr[bank];
    for (u32 i = 0; i < n; i++)
        d[i] = s[i];
    dc_wb(bank_addr[bank], n);
    return 0;
}

static int download_data(int id, int bank)   /* Download2 0x3baf48: the data blob where the DSP says, then mark the bank */
{
    static const u8 info_off[6] = { 0x12, 0x14, 0x16, 0x1a, 0x1c, 0x18 };
    const struct codec *c = codec(id);
    if (!c)
        return -1;
    if (c->blob) {
        u32 n = be32(c->hdr + 0xc);
        u16 o = info(info_off[bank]);
        if (!o) {
            if (logf)
                logf("audio: codec %02x: DSP has not published the data address for bank %d\n", id, bank);
            return 1;
        }
        if (cksum(c->blob, n) != be32(c->hdr + 0x18)) {
            if (logf)
                logf("audio: codec %02x data checksum mismatch\n", id);
            return -1;
        }
        const volatile u8 *s = (const volatile u8 *)c->blob;
        u8 *d = (u8 *)(data + (u32)o * 128);
        for (u32 i = 0; i < n; i++)
            d[i] = s[i];
        dc_wb(data + (u32)o * 128, n);
    }
    if (bank >= 1)
        REG(0x1744 + 4 * bank) |= 0x80000000u | (u32)id << 24;
    return 0;
}

/* ---- the eCos sequence ---- */

static void codec_init(void)   /* 0x3bf564(1) + 0x3bf50c: analog codec / DAC, in eCos's exact order */
{
    REG(0x1f64) |= 0x80000000u;
    REG(0x1e14) = 0x042bff5f;
    REG(0x1e14) |= 0x03800000;
    REG(0x1e14) &= ~0x1fu;
    REG(0x1e14) |= 0xc0000000u;   /* DAC on */
    REG(0x1f64) |= 0x1f;
    REG(0x1e18) |= 0x80000000u;
    REG(0x1e18) = 0x4f064f00;
    REG(0x1f54) = 0x00004f00;
    REG(0x1f54) |= 0x3f000000;
    REG(0x1e18) &= 0xf3fff3ffu;
    REG(0x1fec) &= 0xff0000ffu;
    REG(0x1fec) = 0x00440000;
    REG(0x1f54) &= ~0xf0u;
    REG(0x1f54) |= 0xf0;
    REG(0x1f54) &= 0xffc0ffffu;
    REG(0x1f54) |= 0x00040000;
    REG(0x1e80) = 0, REG(0x1e84) = 0x7777, REG(0x1e88) = 0x300, REG(0x1e8c) = 0x475, REG(0x1e90) = 0x7e33a9;
    REG(0x1e94) = 0x650200, REG(0x1e98) = 0x650200;
    REG(0x1e9c) = 0x800000, REG(0x1ea0) = 0x800000, REG(0x1ea4) = 0x800000, REG(0x1ea8) = 0x800000;
    REG(0x1eac) = 0x1111, REG(0x1eb0) = 0, REG(0x1eb4) = 0, REG(0x1eb8) = 0, REG(0x1e80) = 0xf;
    REG(0x1e18) &= ~0x40000000u;
    REG(0x1e18) &= ~0x4000u;
    REG(0x1f58) = 0;
    REG(0x1f58) = 0x06c60606;
    REG(0x1e18) &= 0xffe0ffe0u;
    REG(0x1e18) |= 0x00060006;
    REG(0x1f5c) = 0x17171717;
    REG(0x1e08) = 0x4d, REG(0x1f3c) = 0x4d, REG(0x1f40) = 0x5c, REG(0x1e10) = 0x5c, REG(0x1e20) = 0x5c004d;
    REG(0x21a4) = 0x76543210, REG(0x21a8) = 0x98, REG(0x21ac) = 0x76543210, REG(0x21e0) = 0x76543210;
}

static void params_init(void)   /* 0x3bd460, 0x3bd35a, 0x3bc63e: the RISC and App parameter blocks */
{
    u32 P = RISC, A = APP;
    for (u32 i = 0; i < 0x180; i += 4)
        *(volatile u32 *)(P + i) = 0;
    *(volatile u16 *)(P + 0x04) = 0xa75e, *(volatile u16 *)(P + 0x00) = 0x6f8f, *(volatile u16 *)(P + 0x02) = 0x8900;
    *(volatile u16 *)(P + 0x06) = chip651 ? 0x0080 : 0x264f, *(volatile u16 *)(P + 0x08) = chip651 ? 0 : 0x00e7;
    static const u16 pv[][2] = {
        { 0x7e, 0x0100 }, { 0x0e, 0x0100 }, { 0x26, 0x0100 }, { 0x7a, 0x1100 }, { 0x20, 0x2f00 }, { 0x28, 0x0040 },
        { 0x22, 0xcfcf }, { 0x0c, 0x1000 }, { 0x32, 0x80 },   { 0x38, 0x80 },   { 0x3a, 0x80 },   { 0x40, 0x80 },
        { 0x42, 0x80 },   { 0x48, 0x80 },   { 0x4a, 0x80 },   { 0x50, 0x80 },   { 0x52, 0x80 },   { 0x58, 0x80 },
        { 0x5a, 0x80 },   { 0x60, 0x80 },   { 0x62, 0x80 },   { 0x68, 0x80 },   { 0x6a, 0x80 },   { 0x70, 0x80 },
        { 0x72, 0x80 },   { 0x78, 0x80 },   { 0x140, 0x401f }, { 0x144, 0xd703 }, { 0x148, 0x0619 }, { 0x17e, 0xcfcf },
        { 0x172, 0x1000 }, { 0x16a, 0x0008 }, { 0x0a, 0x0100 },
    };
    for (u32 i = 0; i < sizeof pv / sizeof pv[0]; i++)
        *(volatile u16 *)(P + pv[i][0]) = pv[i][1];
    static const u16 bs[][2] = { { 0x16, 0x64 }, { 0x1c, 0x64 }, { 0x1e, 0x5a }, { 0x24, 0x5a }, { 0x12, 0x12c },
                                 { 0x14, 0x12c }, { 0x18, 0x64 }, { 0x1a, 0x64 }, { 0x2e, 0x5a } };
    for (u32 i = 0; i < sizeof bs / sizeof bs[0]; i++)
        *(volatile u16 *)(P + bs[i][0]) = bswap16(bs[i][1]);
    for (u32 i = 0; i < 0x100; i += 4)
        *(volatile u32 *)(A + i) = 0;
    static const u16 t[10] = { 0xb0, 0x123, 0x1b3, 0x203, 0x233, 0x2c3, 0x343, 0x373, 0x393, 0x3b0 };
    for (u32 i = 0; i < 60; i += 10)
        for (u32 j = 0; j < 10; j++)
            *(volatile u16 *)(A + 2 * (i + j)) = bswap16(t[j]);
    static const u16 av[][2] = { { 0x7a, 0xcf00 }, { 0x7c, 0xcfcf }, { 0x7e, 0xcfcf }, { 0x80, 0xcfcf }, { 0xa8, 0x8f31 },
                                 { 0x78, 0x2200 }, { 0xba, 0xb480 }, { 0xbc, 0x3401 }, { 0xbe, 0xb401 }, { 0xc0, 0x3402 },
                                 { 0xc2, 0xb402 }, { 0xc4, 0x3403 }, { 0xc6, 0xa403 }, { 0xd0, 0x0500 } };
    for (u32 i = 0; i < sizeof av / sizeof av[0]; i++)
        *(volatile u16 *)(A + av[i][0]) = av[i][1];
    dc_wb(P, 0x280);
}

static int layout(void)   /* 0x3bd5ce(base, 0) */
{
    REG(0x1798) = 2;
    ms(100);
    for (int i = 0; i < 1000 && (REG(0x1790) >> 1) != 0 && (REG(0x1824) & 0xffff) != 0; i++)
        ms(1);
    REG(0x1780) = 1;
    u32 a = AUD_BASE;
    for (int b = 0; b < 6; b++) {
        bank_addr[b] = a;
        REG(0x1744 + 4 * b) = (a & 0x0fffffffu) >> 8;
        a += bank_size[b];
    }
    data = a;
    REG(0x1764) = data & 0x0fffffffu, REG(0x1ed0) = data & 0x0fffffffu;
    REG(0x17c0) = bank_size[0], REG(0x17c4) = bank_size[1], REG(0x17c8) = bank_size[2];
    REG(0x17cc) = bank_size[3], REG(0x17d0) = bank_size[4], REG(0x17d4) = bank_size[5];
    REG(0x17e0) = AUD_SIZE - (data - AUD_BASE);
    params_init();
    for (u32 i = 0; i < 0x200; i += 4)
        *(volatile u32 *)(data + 0x280 + i) = 0;
    dc_wb(data + 0x280, 0x200);
    mailbox_clear();
    return 0;
}

static int init_system(void)   /* DSP_InitSystem 0x3bb996 */
{
    const struct codec *c0 = codec(0);
    REG(0x1798) = 2;
    ms(100);
    for (int i = 0; i < 1000 && (REG(0x1790) >> 1) != 0 && (REG(0x1824) & 0xffff) != 0; i++)
        ms(1);
    REG(0x1780) = 1;
    if (!(be32(c0->hdr + 2) >> 16 & 1))
        REG(0x1794) |= 2;
    if (download_code(0, 0))
        return -1;
    mailbox_clear();
    REG(0x1780) = chip651 ? 0x10 : 0;   /* release the DSP */
    if (wait_state(0, 1))
        return -2;
    if (state(0) != 2) {   /* idle */
        send_cmd(0, 0, 2, 0);
        if (wait_state(0, 2))
            return -3;
    }
    if (download_data(0, 0) || download_code(0x1d, 3) || download_data(0x1d, 3) || download_code(0x1c, 5) ||
        download_data(0x1c, 5))
        return -4;
    if (state(0) == 2) {   /* run */
        send_cmd(0, 0, 3, 0);
        if (wait_state(0, 3))
            return -5;
    }
    if (dsp_get(RISC, 0) & 0x40) {   /* 0x3bb94e: front output stage, then front play */
        if (state(7) != 1) {
            send_cmd(0, 7, 1, 0);
            wait_state(7, 1);
        }
        send_cmd(3, 8, 1, 0);
        send_cmd(0, 7, 2, 0);
        if (wait_state(7, 2))
            return -6;
    }
    return 0;
}

static void post_boot(void)   /* rest of AUDIF_Init: 48 kHz clocks, codec resets, enable mask, output ports, Swb area */
{
    REG(0x1e14) = (REG(0x1e14) & 0xff8fffffu) | 0x100000;   /* 0x3bf4d2 (RAM 0x71f110 = 0x100000 at boot) */
    REG(0x1f04) = 0x6003;
    REG(0x1f0c) = 0x6883, REG(0x1f10) = 0x6003, REG(0x1f14) = 0x6883;
    REG(0x1f00) = 0x6883;
    REG(0x1f00) |= 0x4000;
    REG(0x0238) = REG(0x0238) & ~0x30u;
    REG(0x1f20) = 0x6001, REG(0x1f1c) = 0x6887;
    REG(0x1f08) = 0x4887;
    REG(0x1f24) = 0x4887;
    REG(0x1f38) = 0x6003, REG(0x1f34) = 0x6883;
    REG(0x0238) = 0xffff1c80u, REG(0x0238) = 0xffff1c84u;   /* 0x3bf800 */
    REG(0x1f64) &= ~0x80000000u;
    REG(0x1f64) |= 0x80000000u;
    REG(0x1e18) &= ~0x80000000u;
    REG(0x1e18) |= 0x80000000u;
    REG(0x1ec0) = 1, REG(0x1ec8) = 0x10000010, REG(0x1ec0) = 0;
    send_cmd(1, 0, 0, 0x1f4fef);
    for (int i = 0; i < 3; i++) {   /* 0x3b135e(0,1), (1,1), (2,0) */
        REG(0x2194) |= 2;
        REG(0x2188) |= 0x8080;
    }
    REG(0x2198) |= 0x210000;
    REG(0x2198) |= 0x420000;
    REG(0x2198) &= ~0x840000u;
    u16 o27 = info(0x36), o28 = info(0x3c);
    if (o27) {   /* 0x3bd174: zero the id 0x27 area */
        for (u32 i = 0; i < 0x500; i += 4)
            *(volatile u32 *)(data + (u32)o27 * 128 + i) = 0;
        dc_wb(data + (u32)o27 * 128, 0x500);
    }
    if (o28) {   /* 0x3bd1a4: the Swb area */
        u32 S = data + (u32)o28 * 128;
        for (u32 i = 0; i < 0x100; i += 4)
            *(volatile u32 *)(S + i) = 0;
        static const u16 sv[][2] = { { 0x00, 0x0700 }, { 0x02, 0x0100 }, { 0x04, 0xffff }, { 0x0a, 0x0002 }, { 0x0c, 0x0a07 },
                                     { 0x0e, 0x0020 }, { 0x14, 0x0800 }, { 0x16, 0x0004 }, { 0x18, 0x9a9a }, { 0x1c, 0x6400 },
                                     { 0x1e, 0x3813 }, { 0x20, 0xe803 }, { 0x22, 0x1027 }, { 0x3c, 0x0008 }, { 0x3e, 0xc800 },
                                     { 0x46, 0x6608 } };
        for (u32 i = 0; i < sizeof sv / sizeof sv[0]; i++)
            *(volatile u16 *)(S + sv[i][0]) = sv[i][1];
        dc_wb(S, 0x100);
    }
    u16 b7 = dsp_get(RISC, 0xb7);   /* bit 9 = 0 (no CPU mixer here), bits 13:12 = 01 */
    dsp_set(RISC, 0xb7, (u16)((b7 & ~0x3200u) | 0x1000u));
}

static void ring_clear(void)
{
    REG(ring_w) = 0, REG(ring_r) = 0, rpos = zpos = 0;
    for (int i = 0; i < NSRC; i++)
        cur[i] = 0;
    if (ring) {
        for (u32 i = 0; i < ring_size; i += 4)
            *(volatile u32 *)(ring + i) = 0;
        dc_wb(ring, ring_size);
    }
}

static int ring10_start(void)   /* "alter" track: AUD_Set_RiscSndFs 48 kHz, ring 10 cleared */
{
    u16 o = info(0x2e);
    ring = o ? data + (u32)o * 128 : 0, ring_size = 0x16800, ring_w = 0x18c8, ring_r = 0x18cc, ring_be = 1, path = 10;
    ring_clear();
    send_cmd(6, 8, 1, 0);
    return ring ? 0 : -1;
}

/* the "main" track as Android Auto uses it in eCos (media_stream.c): stream 1 with the PCM decoder (codec 1 in bank 1),
 * ring 1 (info +4, 0x4B000 bytes, W 0x1888 / R 0x188C), little-endian s16 stereo 48 kHz, routed to the main output */
static int stream1_start(void)
{
    if (dsp_get(RISC, 7) == 1) {   /* AUDIF_Set(0xc, 1): stop media 1 */
        dsp_set(APP, 0x6a, 0);
        send_cmd(5, 4, 0, 0);
    }
    if (state(1) != 0 && state(1) != 2) {
        send_cmd(0, 1, 2, 0);
        wait_state(1, 2);
    }
    dsp_set(RISC, 0x5e, 0);
    send_cmd(5, 2, 0, 0);
    u16 o = info(4);
    if (!o)
        return -1;
    ring = data + (u32)o * 128, ring_size = 0x4b000, ring_w = 0x1888, ring_r = 0x188c, ring_be = 0, path = 1;
    ring_clear();
    static const u16 desc[16] = { 1, 5, 2, 16, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20 };   /* PCM, 2 ch, 16 bit, 48 kHz */
    for (int i = 0; i < 16; i++)
        dsp_set(RISC, 0x50 + i, desc[i]);   /* RISC byte 0xA0: the media-1 codec descriptor */
    if (state(1) != 0 && state(1) != 2) {   /* DSP_CheckCodec(1): stop, load the PCM decoder, play */
        send_cmd(0, 1, 2, 0);
        wait_state(1, 2);
    }
    if (download_code(1, 1) || download_data(1, 1))
        return -2;
    send_cmd(0, 1, 3, 0);
    if (wait_state(1, 3))
        return -3;
    dsp_set(RISC, 0x11, 0xcfcf);   /* AUDIF_Set(0x24, 1, 1): media 1 to the main output, input gain 0 dB */
    dsp_set(RISC, 7, 1);
    dsp_set(RISC, 0x3d, 0x11);
    send_cmd(5, 2, 0, 0);
    dsp_set(APP, 0x6a, 0x800);     /* AUDIF_Set(0xd, 1): start */
    send_cmd(5, 4, 0, 0);
    return 0;
}

int aud_init(aud_logf log)
{
    logf = log;
    ready = 0;
    u32 id = REG(0x0000) & 0xffff;
    chip651 = id == 0x651;
    if (logf)
        logf("audio: chip %x, DSP window %08x + %x\n", id, AUD_BASE, AUD_SIZE);
    REG(0x0200) = (REG(0x0200) & 0x0dffu) | 0x1000u | 0xffff0000u;   /* 0x3ae2c2: DSP clock (432 MHz on a 0x651) */
    REG(0x0214) = (REG(0x0214) & ~0xf0u & 0xffffu) | (chip651 ? 2u : 4u) << 4 | 0xffff0000u;
    codec_init();
    layout();
    int r = init_system();
    if (logf)
        logf("audio: DSP init %d, state %08x, heartbeat %x, banks %08x %08x %08x\n", r, REG(0x18b0), REG(0x18b4),
             REG(0x1744), REG(0x1750), REG(0x1758));
    if (r)
        return r;
    post_boot();
    eq_apply_all();
    int sr = stream1_start();
    if (sr) {
        if (logf)
            logf("audio: stream 1 (PCM decoder) failed (%d, state %08x): RISC sound ring 10 instead\n", sr, REG(0x18b0));
        ring10_start();
    }
    aud_set_mute(1);
    aud_set_volume(0);
    if (logf)
        logf("audio: ready; path %u, ring at %08x, state %08x, %u command timeouts\n", path, ring, REG(0x18b0),
             cmd_timeouts);
    ready = 1;
    return ring ? 0 : -7;
}

int aud_ready(void) { return ready; }

void aud_set_mute(int on) { if (data) send_cmd(6, 7, on ? 1 : 0, 0); }

void aud_set_volume(int index)
{
    if (!data)
        return;
    index = index < 0 ? 0 : index > 40 ? 40 : index;
    u16 v = dsp_get(APP, 0x3d);
    dsp_set(APP, 0x3d, (u16)((v & 0xff00u) | vol_code[index]));
    send_cmd(5, 4, 0, 0);
}

/* ---- equalizer (notes/audio_eq.md): the DSP designs the filters itself from per-slot descriptors in the "App" area
 * (id 0x27, info +0x36), 20 slots per channel, channels 0..3 at halfword offsets 0x00/0x28/0x50/0x78. A slot is two
 * halfwords, stored byte-swapped: w0 = (2·dB & 0x7f) << 9 | type << 6 | code (type 3 = peaking, code = 32/Q),
 * w1 = frequency in Hz (0 = off). eCos sends no mailbox command after these writes. Our bands: a 10-band octave EQ. */
static const u16 eq_hz[AUD_EQ_BANDS] = { 31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
static signed char eq_half[AUD_EQ_BANDS];   /* gain per band in 0.5 dB steps, -24..24 */
#define EQ_Q_CODE 23                          /* 32 / 1.41: one octave wide */

static void eq_slot(int ch, int slot, int type, unsigned hz, int half_db, unsigned code)
{
    u16 o = info(0x36);
    if (!o || ch < 0 || ch > 5 || slot < 0 || slot > 19)
        return;
    static const u8 base[6] = { 0x00, 0x28, 0x50, 0x78, 0xa0, 0xc8 };
    half_db = half_db > 24 ? 24 : half_db < -24 ? -24 : half_db;
    u16 w0 = (u16)(((u32)half_db & 0x7f) << 9 | (u32)type << 6 | (code & 0x3f));
    u16 w1 = (u16)(hz > 20000 ? 20000 : hz);
    u32 a = data + (u32)o * 128 + 2u * (base[ch] + 2u * (u32)slot);
    *(volatile u32 *)a = (u32)bswap16(w1) << 16 | bswap16(w0);   /* both halfwords in one aligned store */
    dc_wb(a, 4);
}

void aud_eq_set(int band, int half_db)
{
    if (band < 0 || band >= AUD_EQ_BANDS)
        return;
    eq_half[band] = (signed char)(half_db > 24 ? 24 : half_db < -24 ? -24 : half_db);
    if (!data)
        return;
    for (int ch = 0; ch < 4; ch++)
        eq_slot(ch, band, 3, eq_hz[band], eq_half[band], EQ_Q_CODE);
}

static void eq_apply_all(void)
{
    for (int b = 0; b < AUD_EQ_BANDS; b++)
        aud_eq_set(b, eq_half[b]);
}

/* ---- PCM: a small mixer over ring 10 ----
 * Positions are monotonic byte counts (offset = pos % size). The DSP's read pointer R is followed as rpos. Every source
 * (0 media, 1 guidance/speech, 2 system sounds, 3 test tone) adds its samples into the ring at its own cursor, so a nav
 * prompt plays over the music; ring bytes are zeroed the first time any source reaches them (zpos), and W = zpos.
 * A source that has been idle restarts LEAD bytes ahead of the DSP. */

static void track_r(void)
{
    u32 r = REG(ring_r) % ring_size, ro = rpos % ring_size;
    rpos += (r + ring_size - ro) % ring_size;
    if ((int)(zpos - rpos) < 0)   /* underrun: the DSP went past everything written */
        zpos = rpos;
}

static void zero_to(u32 end)   /* extend the valid region to end, zero-filled */
{
    while ((int)(end - zpos) > 0) {
        u32 o = zpos % ring_size, k = ring_size - o;
        if (k > end - zpos)
            k = end - zpos;
        u32 *d = (u32 *)(ring + o);
        for (u32 i = 0; i < k / 4; i++)
            d[i] = 0;
        zpos += k;
    }
}

static void add_be(u32 pos, short v)   /* saturating add of one sample at byte position pos (ring byte order) */
{
    u8 *d = (u8 *)(ring + pos % ring_size);
    int hi = ring_be ? 0 : 1, lo = 1 - hi;
    int s = (short)(d[hi] << 8 | d[lo]) + v;
    s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
    d[hi] = (u8)(s >> 8), d[lo] = (u8)s;
}

/* frames of stereo s16 (little-endian, native) from source src into the ring; returns frames taken */
static int mix(int src, const short *lr, int frames, int mono_x3)
{
    if (!ready || !ring || src < 0 || src >= NSRC)
        return 0;
    track_r();
    if ((int)(cur[src] - (rpos + 128)) < 0)   /* idle or fell behind: start just ahead of the DSP */
        cur[src] = rpos + LEAD;
    u32 limit = rpos + ring_size - 128;   /* never write within 128 bytes behind R */
    u32 room = (int)(limit - cur[src]) > 0 ? (limit - cur[src]) / 4 : 0;
    u32 out = mono_x3 ? (u32)frames * 3 : (u32)frames;   /* 16 kHz mono sources are repeated 3x into 48 kHz stereo */
    u32 n = out < room ? out : room;
    u32 start = cur[src];
    zero_to(start + 4 * n);
    for (u32 i = 0; i < n; i++) {
        short l, r;
        if (mono_x3)
            l = r = lr[i / 3];
        else
            l = lr[2 * i], r = lr[2 * i + 1];
        add_be(start + 4 * i, l);
        add_be(start + 4 * i + 2, r);
    }
    for (u32 done = 0; done < 4 * n;) {   /* write back what changed, by contiguous piece */
        u32 o = (start + done) % ring_size, k = ring_size - o;
        if (k > 4 * n - done)
            k = 4 * n - done;
        dc_wb(ring + o, k);
        done += k;
    }
    cur[src] = start + 4 * n;
    taken[src] += n;
    if (n < out)
        dropped[src] += out - n;
    REG(ring_w) = zpos % ring_size;
    return mono_x3 ? (int)(n / 3) : (int)n;
}

int aud_write(const short *lr, int frames) { return mix(0, lr, frames, 0); }
int aud_write_mono16k(int src, const short *m, int samples) { return mix(src, m, samples, 1); }

static u32 ring_free(void)   /* for the tone: room ahead of its own cursor */
{
    track_r();
    u32 c = (int)(cur[3] - rpos) > 0 ? cur[3] : rpos + LEAD;
    u32 limit = rpos + ring_size - 128;
    return (int)(limit - c) > 0 ? limit - c : 0;
}

static int tone_amp = 256;   /* peak, of 32767: 256 = -42 dBFS */
static unsigned long long tone_end;
void aud_tone(int hz) { aud_tone_ex(hz, 256, 2000); }
void aud_tone_ex(int hz, int peak, int ms)   /* quiet by default and time-limited: a test tone must never blast */
{
    tone_hz = hz, tone_phase = 0;
    tone_amp = peak < 1 ? 1 : peak > 8192 ? 8192 : peak;
    tone_end = clk_us() + 1000ull * (unsigned)(ms < 100 ? 100 : ms > 10000 ? 10000 : ms);
}

static short sine(u32 ph)   /* ph: 0..65535 per period; parabolic approximation, peak tone_amp */
{
    int x = (int)(ph & 0xffff) - 32768;            /* -pi..pi */
    int y = x < 0 ? -x : x;
    int v = (int)(((long long)x * (32768 - y)) >> 15);   /* 4x(1-|x|) shape, peak 8192 */
    return (short)(v * tone_amp / 8192);
}

void aud_poll(void)
{
    if (!ready)
        return;
    service_msgs();
    u32 hb = REG(0x18b4) & 0xffff;
    if (hb != hb_last)
        hb_last = hb, hb_stuck = 0, hb_seen++;
    else if (++hb_stuck == 200000 && logf)
        logf("audio: DSP heartbeat stopped at %x (state %08x)\n", hb, REG(0x18b0));
    u32 evt = (u32)info(0x26) << 16 | info(0x28);   /* 0x3be85e: echo DSP params 0x13/0x14 (inference: event ack) */
    if (!(evt & 0x80000000u) && evt != last_evt) {
        last_evt = evt;
        send_cmd(1, 3, 0, evt);
    }
    if (tone_hz && clk_us() >= tone_end)
        tone_hz = 0;
    if (tone_hz && ring) {
        static short buf[2 * 256];
        while (ring_free() >= sizeof buf && ring_free() > ring_size / 2) {   /* keep it about half full */
            for (int i = 0; i < 256; i++) {
                short s = sine(tone_phase >> 16);
                buf[2 * i] = s, buf[2 * i + 1] = s;
                tone_phase += (u32)tone_hz * 89478u;   /* 2^32 / 48000 */
            }
            mix(3, buf, 256, 0);
        }
    }
}

int aud_status(char *o, aud_fmt fmt)
{
    if (!data)
        return fmt(o, "audio: not initialized");
    int k = fmt(o, "eq");
    for (int b = 0; b < AUD_EQ_BANDS; b++)
        k += fmt(o + k, " %u:%d", eq_hz[b], eq_half[b]);
    k += fmt(o + k, "; ");
    o += k;
    return k + fmt(o, "audio: %s state %08x (sys %d front %d) heartbeat %x (%u changes) 1880 %08x 18a8 %08x msgs %u timeouts %u "
                  "path %u ring %08x W %x R %x played %u KB; taken/dropped media %u/%u guide %u/%u tone %u/%u; tone %d Hz vol %02x "
                  "1e14 %08x banks %08x %08x %08x",
               ready ? "ready" : "NOT ready", REG(0x18b0), state(0), state(7), REG(0x18b4), hb_seen, REG(0x1880), REG(0x18a8),
               msgs, cmd_timeouts, path, ring, REG(ring_w), REG(ring_r), rpos >> 10, taken[0], dropped[0], taken[1] + taken[2],
               dropped[1] + dropped[2], taken[3], dropped[3], tone_hz,
               dsp_get(APP, 0x3d) & 0xff, REG(0x1e14), REG(0x1744), REG(0x1750), REG(0x1758));
}
