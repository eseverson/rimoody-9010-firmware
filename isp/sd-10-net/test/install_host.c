/* Host CLI around install.c's planning for test_install.py: runs the same code the payload runs, against flash dumps.
 *
 *   install_host boot FLASH [OUT]          plan stage 1; OUT = FLASH with the planned blocks written
 *   install_host copy FLASH IMAGE [OUT] [pending]   plan stage 2 (stage 1 plan and region scan first); OUT = the copy
 *                                          written (pending: without the magic word, as an interrupted install leaves it)
 *   install_host region FLASH              classify the copy region
 *   install_host script | scriptimg OUT    the version 2 script text / uImage
 *
 * One `key=value` line per fact on stdout; exit 0 = the plan may be written, 2 = refused, 1 = usage or I/O error. */
#define NOR_HOST
#define INSTALL_HOST
#include "../crypto.c"
#include "../nor.c"
#include "../install.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *flash;

static int rd_file(void *ctx, unsigned off, void *dst, unsigned len)
{
    (void)ctx;
    if (off > NOR_SIZE || len > NOR_SIZE - off)
        return -1;
    memcpy(dst, flash + off, len);
    return 0;
}

static unsigned char *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n > NOR_SIZE ? *n : NOR_SIZE);
    if (fread(b, 1, *n, f) != (size_t)*n)
        *n = -1;
    fclose(f);
    return b;
}

static int save(const char *path, const unsigned char *b, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, n, f) != (size_t)n)
        return -1;
    return fclose(f);
}

static const char *scr[] = { "blank", "ours", "partial", "foreign" };
static const char *env[] = { "none", "stock", "patched", "many" };
static const char *reg[] = { "blank", "copy", "pending", "foreign" };

static void print_boot(const struct inst_boot *b)
{
    printf("refuse=%s\n", b->refuse ? b->refuse : "");
    printf("env=%s\nenv_off=0x%x\nscript=%s\nscript_ver=%d\nbody=0x%x\n", env[b->env_state], b->env_off,
           scr[b->scr_state], b->scr_ver, b->body);
    printf("new_hsum=0x%08x\nnew_dsum=0x%08x\nstage1_done=%d\nblocks=", b->new_hsum, b->new_dsum, inst_stage1_done(b));
    for (int k = 0; k < b->nblk; k++)
        printf("%s0x%x", k ? "," : "", b->blk_off[k]);
    printf("\n");
}

static void print_region(const struct inst_copy *c)
{
    printf("region=%s\nfirst_bad=0x%x\nold_len=%u\nold_seq=%u\nold_ok=%d\nold_build=%s\nerase_mask=0x%x\n",
           reg[c->state], c->first_bad, c->old_len, c->old_seq, c->old_ok, c->old_build, c->erase_mask);
}

int main(int argc, char **argv)
{
    static unsigned char work[INST_WORK_SZ], buf[0x10000];
    if (argc >= 2 && !strcmp(argv[1], "script")) {
        fputs(inst_script_text(), stdout);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "scriptimg")) {
        static unsigned char img[0x10000];
        return save(argv[2], img, inst_script_image(img)) ? 1 : 0;
    }
    if (argc < 3)
        return fprintf(stderr, "usage: see install_host.c\n"), 1;
    long n;
    flash = load(argv[2], &n);
    if (!flash || n != NOR_SIZE)
        return fprintf(stderr, "%s: not a 16 MB flash image\n", argv[2]), 1;
    struct inst_boot b;
    struct inst_copy c;
    if (!strcmp(argv[1], "region")) {
        inst_region(rd_file, 0, buf, &c);
        print_region(&c);
        return 0;
    }
    int r = inst_plan_boot(rd_file, 0, work, &b);
    print_boot(&b);
    if (!strcmp(argv[1], "boot")) {
        if (r)
            return 2;
        for (int k = 0; k < b.nblk; k++) {   /* the device writes these with nor_boot_write: check its rule too */
            if (!nor_boot_diff_ok(b.blk_off[k], flash + b.blk_off[k], b.blk[k], b.env_off, INST_ENV_LEN))
                return printf("diff_rule=FAIL at 0x%x\n", b.blk_off[k]), 1;
            memcpy(flash + b.blk_off[k], b.blk[k], 0x10000);
        }
        if (argc >= 4 && save(argv[3], flash, NOR_SIZE))
            return 1;
        return 0;
    }
    if (!strcmp(argv[1], "copy") && argc >= 4) {
        long il;
        unsigned char *img = load(argv[3], &il);
        if (!img || il < 0)
            return fprintf(stderr, "%s: cannot read\n", argv[3]), 1;
        inst_region(rd_file, 0, buf, &c);
        print_region(&c);
        int rc = inst_plan_copy(img, (unsigned)il, &b, &c);
        printf("copy_refuse=%s\n", c.refuse ? c.refuse : "");
        if (rc)
            return 2;
        printf("seq=%u\nlen=%u\ncrc=0x%08x\nspan_crc=0x%08x\nbuild=%s\n", c.seq, c.len, c.crc, c.span_crc, c.build);
        for (unsigned k = 0; k < INST_COPY_BLOCKS; k++)   /* what the device's steps leave: erase, header, image, magic */
            if (c.erase_mask >> k & 1)
                memset(flash + INST_COPY_OFF + k * 0x10000, 0xff, 0x10000);
        memcpy(flash + INST_COPY_OFF, c.hdr, INST_COPY_HDR);
        if (argc >= 6 && !strcmp(argv[5], "pending"))
            memset(flash + INST_COPY_OFF, 0xff, 4);
        memcpy(flash + INST_COPY_IMG, img, c.len);
        if (argc >= 5 && save(argv[4], flash, NOR_SIZE))
            return 1;
        return 0;
    }
    return fprintf(stderr, "usage: see install_host.c\n"), 1;
}
