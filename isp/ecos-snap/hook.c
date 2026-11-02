/* Snapshot hook for the stock eCos (notes/vpp0_display.md, "eCos register snapshot").
 *
 * eCos's clock tick (its ISR's call of the empty hal_clock_reset, 0x64b436) and its display-1 vsync dispatcher call
 * (0x511628) go to entry.S at 0x301000, which turns translation off, calls snap_hook(which), then the original target. Every call pets the
 * hardware watchdog the launcher armed (so a hang anywhere resets the unit within ~13 s), records the DDFCH state for
 * that frame, and takes full register snapshots ~2 / 10 / 25 s after its first call. After the last one it writes the D-cache back
 * and lets the watchdog reset the chip; DRAM at 0x02F40000.. survives, the card boots our payload again, and `hu.py read` collects it.
 *
 * Layout at SNAP (0x02F48000): struct hdr; per-frame history at +0x100 (NHIST entries of 4 words); snapshot k at
 * SNAP + 0x10000 * (k + 1), each a list of { addr, nwords, words... } ending with addr = 0. */
typedef unsigned int u32;
#define R(a) (*(volatile u32 *)(a))
#define SNAP 0x02F48000u
#define NHIST 2048u
#define MAGIC 0x50414e53u   /* "SNAP" */

struct hdr {
    u32 magic, ver, calls, done;
    u32 first_err_call, first_err_flags, first_ck_call, first_ck;
    u32 snap_call[4];
    u32 or_flags, max_ck_changes, last_flags, last_ck;
    u32 by_src[2];   /* calls from f_0050f284 (display 0) / f_0050f7c0 (display 1) */
    u32 t0, t_last;  /* 1 MHz counter at the first and the latest call */
    u32 trig_call;   /* tick at which 0x9C005784 b20 (DDFCH index mode: eCos showing a decoded picture) was first seen */
};

#define CRUMB 0x02F47F10u   /* last step reached, written back at once (outside the area the hook zeroes) */
static void crumb(u32 v)
{
    R(CRUMB) = v;
    __asm__ volatile("cctl l1d_wball, alevel\n\tmsync all" ::: "memory");
}

static u32 now_us(void)   /* the 1 MHz counter U-Boot and our payload use (isp/sd-10-net/clk.c) */
{
    R(0x9c0031e8) = 0x1234;
    return (R(0x9c0031e4) & 0xffff) << 16 | (R(0x9c0031e0) & 0xffff);
}

static const u32 sweep_skip[] = { 0x9c000280, 0x9c000500, 0x9c000c00, 0x9c000d80, 0x9c000e80 };   /* bus errors */

/* register blocks for every snapshot: { start, words } */
static const u32 blocks[][2] = {
    { 0x9c005780, 32 },   /* DDFCH */
    { 0x9c005a80, 64 },   /* G181 (commit block) and the matrix at 0x5B34 */
    { 0x9c005d80, 64 },   /* VSCL */
    { 0x9c006080, 32 },   /* VPOST */
    { 0x9c006a80, 32 },   /* TGEN0 */
    { 0x9c006c80, 32 },   /* DMIX0 */
    { 0x9c006f80, 32 },
    { 0x9c007000, 32 },
    { 0x9c009a00, 8 }, { 0x9c009b00, 8 }, { 0x9c009b80, 8 }, { 0x9c009c00, 8 },
    { 0x9c00a380, 32 },   /* BG2D control */
    { 0x9c00a400, 256 },  /* BG2D entry table (index mode resolves the frame indices through it) */
};

static u32 put_block(u32 *o, u32 a, u32 n)
{
    o[0] = a, o[1] = n;
    for (u32 i = 0; i < n; i++)
        o[2 + i] = R(a + 4 * i);
    return n + 2;
}

static void snapshot(int k, int sweep)
{
    struct hdr *h = (struct hdr *)SNAP;
    u32 *o = (u32 *)(SNAP + 0x10000u * (k + 1)), w = 0;
    h->snap_call[k] = h->calls ? h->calls : 1;
    for (u32 b = 0; b < sizeof blocks / sizeof blocks[0]; b++)
        w += put_block(o + w, blocks[b][0], blocks[b][1]);
    if (sweep) {   /* uMCTL2 (registers and port 0), last snapshot only */
        w += put_block(o + w, 0x9c107000, 256);
        w += put_block(o + w, 0x9c107400, 16);
    }
    if (sweep)   /* RGST groups 0..198 (0x9C000000..0x9C00637F), as walked under U-Boot (dumps/rgst_9c000000.bin) */
        for (u32 g = 0x9c000000u; g < 0x9c006380u; g += 0x80) {
            int bad = 0;
            for (u32 i = 0; i < sizeof sweep_skip / sizeof sweep_skip[0]; i++)
                bad |= g == sweep_skip[i];
            if (!bad)
                w += put_block(o + w, g, 32);
        }
    o[w] = 0, o[w + 1] = 0;
}

#ifdef AUDIO
/* audio snapshot k at SNAP + 0x10000 + 0x4000 k: the audio/DSP block in 128-word pieces */
/* Audio snapshot k at SNAP + 0x10000 + 0x4000 k, 8 registers per clock tick (a long stay in eCos's tick ISR seems to
 * get the unit reset: runs 1 and 2 died part way through at different registers). Progress word at SNAP + 0xF0;
 * crumb = 0x1000000 | address before each read of snapshot 0. Returns 1 when the snapshot is complete. */
#define AUD_START 0x9c001700u
#define AUD_WORDS 768u
static const u32 aud_skip_grp[] = { 0x9c001b00u, 0x9c001b80u, 0x9c001c00u, 0x9c001c80u, 0x9c001d00u, 0x9c001d80u };
static int audio_snapshot_step(int k)
{
    struct hdr *h = (struct hdr *)SNAP;
    u32 *o = (u32 *)(SNAP + 0x10000u + 0x4000u * k);
    u32 p = R(SNAP + 0xF0);
    for (u32 n = 0; n < 8 && p < AUD_WORDS; n++, p++) {
        u32 blk = p / 32, i = p % 32, r = AUD_START + 4 * p;
        if (!i)
            o[34 * blk] = AUD_START + 128 * blk, o[34 * blk + 1] = 32;
        if (!k)
            crumb(0x1000000u | (r & 0xffffu));
        int skip = 0;   /* groups whose read kills the running system (crumb runs 3+): 0x9C001B00 (group 54) */
        for (u32 j = 0; j < sizeof aud_skip_grp / sizeof aud_skip_grp[0]; j++)
            skip |= (r & ~0x7fu) == aud_skip_grp[j];
        o[34 * blk + 2 + i] = skip ? 0xdeadbeefu : R(r);
    }
    R(SNAP + 0xF0) = p;
    if (p < AUD_WORDS)
        return 0;
    o[34 * (AUD_WORDS / 32)] = 0, o[34 * (AUD_WORDS / 32) + 1] = 0;
    h->snap_call[k] = h->calls ? h->calls : 1;
    R(SNAP + 0xF0) = 0;
    return 1;
}

__attribute__((unused)) static void sweep_to(u32 *o)   /* every RGST group 0x9C000000..0x9C00637F that reads without a bus error */
{
    u32 w = 0;
    for (u32 g = 0x9c000000u; g < 0x9c006380u; g += 0x80) {
        int bad = 0;
        for (u32 i = 0; i < sizeof sweep_skip / sizeof sweep_skip[0]; i++)
            bad |= g == sweep_skip[i];
        if (!bad)
            w += put_block(o + w, g, 32);
    }
    o[w] = 0, o[w + 1] = 0;
}
#endif

static void dcache_wb_all(void)
{
    __asm__ volatile("cctl l1d_wball, alevel\n\tmsync all\n\tisb" ::: "memory");
}

static void reset_now(void)
{
    dcache_wb_all();
    R(0x9c000630) = 0x3877;       /* stop */
    R(0x9c000630) = 0xab00;       /* unlock */
    R(0x9c000634) = 0x200;        /* ~0.1 s */
    R(0x9c000630) = 0xab01;       /* lock */
    R(0x9c000630) = 0x7482;       /* clear a latched expiry */
    R(0x9c0000f4) = 0x00120012;   /* route to system reset */
    R(0x9c000630) = 0x4a4b;       /* resume */
    for (;;)
        ;
}

__attribute__((section(".text.entry"))) void snap_hook(u32 which)
{
    struct hdr *h = (struct hdr *)SNAP;
    if (R(CRUMB) < 0x100)   /* first calls only: once it runs, crumbs are per snapshot step */
        crumb(R(CRUMB) + 1);
    if (h->magic != MAGIC || h->ver != 1) {   /* first call: the loader zeroes the area, but be safe */
        u32 *z = (u32 *)SNAP;
        for (u32 i = 0; i < 0x100 / 4 + NHIST * 4; i++)
            z[i] = 0;
        h->magic = MAGIC, h->ver = 1;
    }
    h->by_src[which & 1]++;
    if (which) {   /* display-1 dispatcher: count it and record the DDFCH state, timing runs off the clock tick */
        u32 i = h->by_src[1] - 1;
        if (i < 256) {
            u32 *e = (u32 *)(SNAP + 0x100 + 16 * NHIST) + 4 * i;
            e[0] = R(0x9c0057ec), e[1] = R(0x9c0057e8), e[2] = R(0x9c005788), e[3] = R(0x9c005e58) << 16 | (R(0x9c006c9c) & 0xffff);
        }
        return;
    }
    u32 c = h->calls++;
    u32 fl = R(0x9c0057ec), ck = R(0x9c0057e8);
    if (!(c & 7) && c / 8 < NHIST) {   /* every 8th tick */
        c /= 8;
        u32 *e = (u32 *)(SNAP + 0x100) + 4 * c;
        e[0] = fl, e[1] = ck, e[2] = R(0x9c005788), e[3] = R(0x9c005e58) << 16 | (R(0x9c006c9c) & 0xffff);
        c = h->calls - 1;
    }
    if (fl && !h->first_err_call)
        h->first_err_call = c + 1, h->first_err_flags = fl;
    if (ck && !h->first_ck_call)
        h->first_ck_call = c + 1, h->first_ck = ck;
    if (ck != h->last_ck)
        h->max_ck_changes++;
    h->or_flags |= fl, h->last_flags = fl, h->last_ck = ck;
    if (c == 0)
        crumb(0x100);
    u32 t = now_us();
    if (c == 0) {
        crumb(0x101);
        h->t0 = t;
    }
    h->t_last = t;
    u32 dt = t - h->t0;   /* wall clock, with call-count fallbacks (the dispatchers do not run every frame) */
#ifdef EQ
    /* EQ run: arm the stubs' logging at 30 s (SNAP + 0xF8), reset at 4 min */
    if (dt >= 30000000u && !R(SNAP + 0xF8u))
        R(SNAP + 0xF8u) = 1;
    if (dt >= 240000000u) {
        h->done = 1;
        crumb(0x500);
        reset_now();
    }
    goto pet;
#endif
#ifdef BT
    {   /* Bluetooth run (stubs.S -DBT logs the frames): find eCos's RFCOMM connect ack `AC 29` in ring A, then let the
         * wireless AA session run 90 s before resetting; 3 min at most. Progress word SNAP + 0xF0, trigger SNAP + 0xF4. */
        u32 n = R(0x02F68000u), k = R(SNAP + 0xF0u);
        for (; k < n && k < 511u; k++) {
            const unsigned char *e = (const unsigned char *)(0x02F68000u + 128u * (k + 1));
            if (*(const u32 *)e == 32 && e[16] == 0xac && e[17] == 0x29 && !R(SNAP + 0xF4u))
                R(SNAP + 0xF4u) = dt ? dt : 1;
        }
        R(SNAP + 0xF0u) = k;
        u32 trig = R(SNAP + 0xF4u);
        if ((trig && dt - trig >= 90000000u) || dt >= 180000000u) {
            h->done = 1;
            crumb(0x500);
            reset_now();
        }
    }
    goto pet;
#endif
#ifdef AUDIO
    /* Audio run (stubs.S logs the DSP / AUDIF calls meanwhile): the audio block 0x9C001700..0x9C0022FF at 25 s and
     * 26 s (moving pointers show up as differences), the full RGST sweep at 27 s, then reset. */
    if (!h->snap_call[0] && dt >= 25000000u) {
        audio_snapshot_step(0);
    } else if (h->snap_call[0] && !h->snap_call[1] && dt >= 27000000u) {
        if (audio_snapshot_step(1))
            crumb(0x2000000u);
    } else if (h->snap_call[1]) {
        h->done = 1;
        crumb(0x500);
        reset_now();
    }
    goto pet;
#endif
    /* Trigger: eCos switches DDFCH to index mode (0x784 b20) when it shows a decoded picture (video playback). Snapshot
     * 0 at 1 s and 1 at 3 s after the trigger, then reset. No trigger within 240 s: snapshot 0 and reset. */
    if (!h->trig_call && (R(0x9c005784) & (1u << 20)))
        h->trig_call = c + 1;
    u32 since = h->trig_call ? c + 1 - h->trig_call : 0;
    if (!h->snap_call[0] && ((h->trig_call && since >= 1000) || dt >= 240000000u || c >= 240000)) {
        crumb(0x200);
        snapshot(0, 0);
        crumb(0x201);
    } else if (h->trig_call && !h->snap_call[1] && since >= 3000) {
        crumb(0x300);
        snapshot(1, 0);
        crumb(0x301);
    } else if (h->snap_call[0] && (!h->trig_call || h->snap_call[1])) {
        h->done = 1;
        crumb(0x500);
        reset_now();
    }
#if defined(AUDIO) || defined(BT) || defined(EQ)
pet:
#endif
    if (c < 8 || !(c & 15))
        dcache_wb_all();
    R(0x9c000630) = 0xab00;   /* pet the watchdog (launch.c armed it, ~13 s) */
    R(0x9c000634) = 0xffff;
    R(0x9c000630) = 0xab01;
}
