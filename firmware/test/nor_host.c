/* Host test for nor.c's pure parts: the write-range guard, splitting on page boundaries and the block-protection
 * decode. Builds nor.c with NOR_HOST (no hardware code). Exit status 0 = all passed. */
#define NOR_HOST
#include "../nor.c"
#include <stdio.h>

static int fails;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c))                                                   \
            fails++, printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    } while (0)

static void range_guard(void)
{
    /* inside each range, up to the exact ends */
    CHECK(nor_range_ok(0xC50000, 0x10000));
    CHECK(nor_range_ok(0xC70000, 0xE00000 - 0xC70000));
    CHECK(nor_range_ok(0xDFF000, 0x1000));
    CHECK(nor_range_ok(0xE10000, 0x1000));
    CHECK(nor_range_ok(0xFBF000, 0x1000));          /* the on-unit test sector */
    CHECK(nor_range_ok(0xE10000, 0xFC0000 - 0xE10000));
    CHECK(nor_range_ok(0xC70000, 1));
    /* one byte outside on either side */
    CHECK(!nor_range_ok(0xC4FFFF, 2));
    CHECK(!nor_range_ok(0xC5FFFF, 2));
    CHECK(!nor_range_ok(0xC6FFFF, 2));
    CHECK(!nor_range_ok(0xDFFFFF, 2));              /* into the boot script block */
    CHECK(!nor_range_ok(0xE0FFFF, 2));
    CHECK(!nor_range_ok(0xFBFFFF, 2));
    /* everything that matters stays out */
    CHECK(!nor_range_ok(0x000000, 0x1000));         /* xboot */
    CHECK(!nor_range_ok(0x8A0000, 0x10000));        /* U-Boot header */
    CHECK(!nor_range_ok(0x920000, 0x10000));        /* U-Boot environment */
    CHECK(!nor_range_ok(0xC60000, 0x1000));         /* used block between the two low ranges */
    CHECK(!nor_range_ok(0xE00000, 0x1000));         /* NOR boot script */
    CHECK(!nor_range_ok(0xE00000, 1));
    CHECK(!nor_range_ok(0xFC0000, 0x1000));         /* eCos settings */
    CHECK(!nor_range_ok(0xFD0000, 0x1000));         /* blank, but in the part U-Boot leaves writable for eCos */
    CHECK(!nor_range_ok(0xFF0000, 0x1000));
    CHECK(!nor_range_ok(0x1000000, 0x1000));
    /* spans across a gap, zero length, wrap-around */
    CHECK(!nor_range_ok(0xC50000, 0x30000));
    CHECK(!nor_range_ok(0xDF0000, 0x20000));
    CHECK(!nor_range_ok(0xC70000, 0));
    CHECK(!nor_range_ok(0xC70000, 0xFFFFFFFFu));
    CHECK(!nor_range_ok(0xFFFFF000u, 0x2000));
    CHECK(!nor_range_ok(0xFFFFFFFFu, 1));
    /* every 4 KB sector and 64 KB block of the flash: allowed exactly when inside a range */
    int sectors = 0, blocks = 0;
    for (unsigned a = 0; a < NOR_SIZE; a += NOR_SECTOR) {
        int in = (a >= 0xC50000 && a < 0xC60000) || (a >= 0xC70000 && a < 0xE00000) || (a >= 0xE10000 && a < 0xFC0000);
        CHECK(nor_range_ok(a, NOR_SECTOR) == in);
        sectors += in;
        if (!(a & (NOR_BLOCK - 1))) {
            CHECK(nor_range_ok(a, NOR_BLOCK) == in);
            blocks += in;
        }
    }
    CHECK(sectors == (0x10000 + 0x190000 + 0x1B0000) / NOR_SECTOR);
    CHECK(blocks == 1 + 25 + 27);
}

static void page_split(void)
{
    CHECK(nor_page_chunk(0xC70000, 0x1000) == 0x100);
    CHECK(nor_page_chunk(0xC70000, 0x80) == 0x80);
    CHECK(nor_page_chunk(0xC700F0, 0x1000) == 0x10);
    CHECK(nor_page_chunk(0xC700FF, 5) == 1);
    CHECK(nor_page_chunk(0xC70001, 0xFF) == 0xFF);
    CHECK(nor_page_chunk(0xC70001, 0x100) == 0xFF);
    /* walk a few odd spans the way nor_program does: pieces cover the span exactly, none crosses a page */
    static const unsigned spans[][2] = { { 0xC70000, 0x1000 }, { 0xC70013, 0x1234 }, { 0xFBF0FF, 0xF01 }, { 0xE10080, 1 },
                                         { 0xE100FF, 0x102 } };
    for (unsigned k = 0; k < sizeof spans / sizeof spans[0]; k++) {
        unsigned off = spans[k][0], len = spans[k][1], pieces = 0, total = 0;
        while (len) {
            unsigned n = nor_page_chunk(off, len);
            CHECK(n >= 1 && n <= NOR_PAGE);
            CHECK(off / NOR_PAGE == (off + n - 1) / NOR_PAGE);
            CHECK(n == len || ((off + n) & (NOR_PAGE - 1)) == 0);
            off += n, len -= n, total += n, pieces++;
        }
        CHECK(total == spans[k][1]);
        unsigned first = spans[k][0], last = spans[k][0] + spans[k][1] - 1;
        CHECK(pieces == last / NOR_PAGE - first / NOR_PAGE + 1);
    }
}

static void wp_decode(void)
{
    unsigned lo = 1, hi = 1;
    CHECK(nor_wp_range(0x00, 0x00, &lo, &hi) == 0);
    CHECK(nor_wp_range(0x04, 0x40, &lo, &hi) == 1 && lo == 0 && hi == 0xFC0000);   /* U-Boot's protect */
    CHECK(nor_wp_range(0x04, 0x02, &lo, &hi) == 1 && lo == 0xFC0000 && hi == NOR_SIZE);   /* QE does not matter */
    CHECK(nor_wp_range(0x24, 0x00, &lo, &hi) == 1 && lo == 0 && hi == 0x40000);
    CHECK(nor_wp_range(0x18, 0x00, &lo, &hi) == 1 && lo == 0x800000 && hi == NOR_SIZE);
    CHECK(nor_wp_range(0x1C, 0x00, &lo, &hi) == 1 && lo == 0 && hi == NOR_SIZE);
    CHECK(nor_wp_range(0x1C, 0x40, &lo, &hi) == 0);
    CHECK(nor_wp_range(0x00, 0x40, &lo, &hi) == 1 && lo == 0 && hi == NOR_SIZE);
    CHECK(nor_wp_range(0x80, 0x00, &lo, &hi) == 0);   /* SRP0 alone protects nothing */
    CHECK(nor_wp_range(0x44, 0x00, &lo, &hi) == -1);
    CHECK(nor_wp_range(0x40, 0x00, &lo, &hi) == 0);
}

static void boot_guard(void)
{
    static unsigned char live[NOR_BLOCK], blk[NOR_BLOCK];
    unsigned env = 0x92F09B;   /* where the original dump keeps U-Boot's default environment */
    CHECK(nor_boot_block_ok(NOR_SCRIPT_BLK, env, 44));
    CHECK(nor_boot_block_ok(0x8A0000, env, 44));        /* the uImage header's block */
    CHECK(nor_boot_block_ok(0x920000, env, 44));        /* the environment's block */
    CHECK(!nor_boot_block_ok(0x930000, env, 44));
    CHECK(nor_boot_block_ok(0x930000, 0x92FFF0, 44));   /* an environment across a block boundary: both blocks */
    CHECK(nor_boot_block_ok(0x920000, 0x92FFF0, 44));
    CHECK(!nor_boot_block_ok(0x920001, env, 44));
    CHECK(!nor_boot_block_ok(0x000000, env, 44));       /* xboot */
    CHECK(!nor_boot_block_ok(0xFC0000, env, 44));       /* eCos settings */
    CHECK(!nor_boot_block_ok(0xC70000, 0xC70000, 44));  /* an "environment" outside U-Boot's largest extent */
    CHECK(!nor_boot_block_ok(0x8A0000, NOR_UBOOT_HDR, 44));   /* inside the header, not the body */
    CHECK(!nor_boot_block_ok(0x920000, env, 0) && !nor_boot_block_ok(0x920000, env, 65));
    /* every block of the flash: only the three */
    int n = 0;
    for (unsigned a = 0; a < NOR_SIZE; a += NOR_BLOCK)
        n += nor_boot_block_ok(a, env, 44);
    CHECK(n == 3);
    /* the diff rule: environment bytes and the two sum words only */
    for (unsigned i = 0; i < NOR_BLOCK; i++)
        live[i] = blk[i] = (unsigned char)(i * 7);
    CHECK(nor_boot_diff_ok(0x920000, live, blk, env, 44));
    blk[env - 0x920000] ^= 1, blk[env + 43 - 0x920000] ^= 1;
    CHECK(nor_boot_diff_ok(0x920000, live, blk, env, 44));
    blk[env + 44 - 0x920000] ^= 1;
    CHECK(!nor_boot_diff_ok(0x920000, live, blk, env, 44));
    for (unsigned i = 0; i < NOR_BLOCK; i++)
        blk[i] = live[i];
    unsigned h = NOR_UBOOT_HDR - 0x8A0000;
    blk[h + 4] ^= 1, blk[h + 7] ^= 1, blk[h + 24] ^= 1, blk[h + 27] ^= 1;
    CHECK(nor_boot_diff_ok(0x8A0000, live, blk, env, 44));
    blk[h + 12] ^= 1;   /* the size word: never */
    CHECK(!nor_boot_diff_ok(0x8A0000, live, blk, env, 44));
    blk[h + 12] ^= 1, blk[h + 3] ^= 1;
    CHECK(!nor_boot_diff_ok(0x8A0000, live, blk, env, 44));
    blk[0] = 0x27, blk[1] = 0x05, blk[2] = 0x19, blk[3] = 0x56;   /* the script block: a uImage */
    CHECK(nor_boot_diff_ok(NOR_SCRIPT_BLK, live, blk, env, 44));
    blk[0] = 0xff;
    CHECK(!nor_boot_diff_ok(NOR_SCRIPT_BLK, live, blk, env, 44));
    for (unsigned i = 0; i < NOR_BLOCK; i++)   /* the undo: an erased script block */
        blk[i] = 0xff;
    CHECK(nor_boot_diff_ok(NOR_SCRIPT_BLK, live, blk, env, 44));
    blk[NOR_BLOCK - 1] = 0;
    CHECK(!nor_boot_diff_ok(NOR_SCRIPT_BLK, live, blk, env, 44));
    CHECK(!nor_boot_diff_ok(0x920000, live, blk, env, 44));
    CHECK(!nor_boot_diff_ok(0xC70000, live, live, env, 44));
}

int main(void)
{
    range_guard();
    boot_guard();
    page_split();
    wp_decode();
    CHECK(nor_strerror(NOR_EARG)[0] != '?' && nor_strerror(12345)[0] == '?');
    printf("nor_host: %s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
