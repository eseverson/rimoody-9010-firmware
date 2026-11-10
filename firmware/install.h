/* Install to flash (notes/nor_install.md).
 *
 * Stage 1, "boot from SD card / flash": the NOR boot script at 0xE00000 becomes version 2 (SD card BOOT.BIN, else the
 * payload copy in NOR if its header and CRC check out, else stock eCos), and on a fresh unit U-Boot's default
 * environment is patched to `bootcmd=source 98e00000;bootsp` with xboot's word sums recomputed (tools/patch_uboot.py).
 * Stage 2, "install this firmware to flash": a copy of the payload with a header at 0xC70000..0xDFFFFF.
 *
 * The planning half is pure (no hardware; test/install_host.c runs it against flash dumps): it reads flash through a
 * callback, decides what each stage would write, and refuses anything it does not recognize. The device half (not
 * built with INSTALL_HOST) runs the plan in the net task, one bounded step per net-loop pass. */
#ifndef INSTALL_H
#define INSTALL_H

#define INST_SCRIPT_VER   2u
#define INST_SCRIPT_NAME  "nor boot script v2"   /* uImage name; "nor boot script" is the SD-only version 1 */
#define INST_ENV_LEN      44u                    /* the default environment span that stage 1 replaces */

/* stage 2: the payload copy */
#define INST_COPY_OFF     0x00C70000u   /* header (one page), image from +0x100 */
#define INST_COPY_END     0x00E00000u   /* first byte past the region (the boot script block) */
#define INST_COPY_HDR     0x100u
#define INST_COPY_IMG     (INST_COPY_OFF + INST_COPY_HDR)
#define INST_COPY_MAX     (INST_COPY_END - INST_COPY_IMG)   /* 0x18FF00: largest image */
#define INST_COPY_MAGIC   0x434e5548u   /* "HUNC", programmed last: the boot script checks it */
#define INST_COPY_VERSION 1u
#define INST_COPY_BLOCKS  ((INST_COPY_END - INST_COPY_OFF) / 0x10000u)   /* 25 */
/* header fields (little-endian u32 unless noted) */
#define CH_MAGIC    0x00   /* "HUNC" = boots; 0xFFFFFFFF = not committed yet; 0 = being removed (zeroed first) */
#define CH_VERSION  0x04
#define CH_LEN      0x08   /* image bytes */
#define CH_CRC      0x0C   /* CRC-32 of the image */
#define CH_SPAN_CRC 0x10   /* CRC-32 of INST_COPY_IMG..INST_COPY_END: the image, then 0xFF; the boot script checks it */
#define CH_SEQ      0x14   /* install counter: 1 on a blank region, the old copy's + 1 otherwise */
#define CH_HDR_LEN  0x18   /* INST_COPY_HDR */
#define CH_BUILD    0x20   /* the image's build id (its HUNE header at +0x48), NUL-terminated, 56 bytes */
#define CH_HCRC     0xFC   /* CRC-32 of bytes 0x04..0xFB: proves a header is ours with or without the magic */

/* the boot script's scratch words (DRAM, free at boot; inside the hu.py upload area while the payload runs) */
#define INST_SCRATCH      0x033F0000u

#define INST_MAX_BLK 4   /* script block, up to two environment blocks, the uImage header block */
#define INST_WORK_SZ 0x60000u   /* inst_plan_boot work buffer: new blocks, then a 64 KB + 64 read buffer */

typedef int (*inst_rd)(void *ctx, unsigned off, void *dst, unsigned len);   /* read flash; 0 = ok */

enum { INST_SCR_BLANK, INST_SCR_OURS, INST_SCR_PARTIAL, INST_SCR_FOREIGN };
enum { INST_ENV_NONE, INST_ENV_STOCK, INST_ENV_PATCHED, INST_ENV_MANY };

struct inst_boot {
    const char *refuse;           /* 0: the plan may be written (nblk may be 0: nothing to do) */
    int undo;                     /* a plan of inst_plan_unboot: back to the stock boot */
    unsigned body, hsum, dsum;    /* U-Boot uImage body size and its word sums as found */
    int env_state;                /* INST_ENV_* */
    unsigned env_off;             /* flash offset of the (stock or patched) environment string */
    int scr_state, scr_ver;       /* script block: INST_SCR_*, and our version found there (1 = SD only) */
    unsigned scr_crc;             /* header CRC of the script found (0 if none) */
    unsigned new_hsum, new_dsum;
    int nblk;                     /* blocks to rewrite, in write order: script, environment, header */
    unsigned blk_off[INST_MAX_BLK], blk_live_crc[INST_MAX_BLK], blk_new_crc[INST_MAX_BLK];
    unsigned char *blk[INST_MAX_BLK];
};

enum { INST_REG_BLANK, INST_REG_COPY, INST_REG_PENDING, INST_REG_FOREIGN };

struct inst_copy {
    const char *refuse;           /* 0: the plan may be written */
    int state;                    /* region: INST_REG_* */
    unsigned first_bad;           /* INST_REG_FOREIGN: first byte that does not fit */
    unsigned old_len, old_seq, old_span_crc, span_crc_now;   /* the copy found; span CRC as it reads now */
    int old_ok;                   /* the copy found would boot: magic, header CRC and span CRC good */
    char old_build[56];
    unsigned erase_mask;          /* bit k: block INST_COPY_OFF + k * 64 KB holds data and needs an erase */
    unsigned len, crc, span_crc, seq;   /* the new copy */
    char build[56];
    unsigned char hdr[INST_COPY_HDR];   /* the new header, magic included */
};

/* pure */
unsigned inst_crc(unsigned crc, const unsigned char *p, unsigned n);
const char *inst_script_text(void);
unsigned inst_script_image(unsigned char *out);   /* the version 2 script as a legacy uImage (<= 4 KB); its length */
int inst_script_class(const unsigned char *blk, const unsigned char *want, int *ver, unsigned *hcrc);
int inst_plan_boot(inst_rd rd, void *ctx, unsigned char *work, struct inst_boot *b);
int inst_plan_unboot(inst_rd rd, void *ctx, unsigned char *work, struct inst_boot *b);   /* undo stage 1 */
int inst_region(inst_rd rd, void *ctx, unsigned char *scratch64k, struct inst_copy *c);
int inst_image_ok(const unsigned char *img, unsigned len, char *build);   /* 0, or why the image is refused */
int inst_plan_copy(const unsigned char *img, unsigned len, const struct inst_boot *b, struct inst_copy *c);
int inst_plan_uncopy(struct inst_copy *c);   /* after inst_region: may the region be erased (ours only) */
int inst_stage1_done(const struct inst_boot *b);   /* environment patched and a valid version >= 2 script */

#ifndef INSTALL_HOST
/* device: net task only */
typedef int (*inst_fmt)(char *, const char *, ...);
int install_op(const unsigned char *p, int n, char *out, inst_fmt fmt);   /* HU1 op 29 sub-ops 9..13 */
void install_poll(void);              /* net loop: one bounded step of a running install */
void install_request(int what);       /* UI: 1 boot path, 2 this firmware (BOOT.BIN), 3 refresh the status,
                                       * 4 remove the boot path, 5 remove the flash copy */
int install_ui(int stage, char *label, int lmax, char *button, int bmax);   /* Settings > Flash row of stage 1 or 2:
                                       * its text and main button text; returns 1 = main button, 2 = Remove, 3 = both */
int install_busy(void);               /* a stage is being written */
int install_line(char *out, int max); /* one line for Settings > System (any task) */
/* the installer boot (notes/nor_install.md "Installer boot"): after clk_ms() passes `at`, stage 1 and its undo refuse
 * to start (a retry of an interrupted plan still runs); 0 = no limit */
void install_s1_deadline(unsigned at);
const char *install_s1_late(void);    /* the refusal reason once the deadline has passed, else 0 (any task) */
int install_progress(int *done, int *total);   /* a job is running: 1 and its step count; else 0 */
int install_last(char *msg, int max); /* the last job: 1 ok, -1 refused or failed, -2 stage 1 failed half way (the plan
                                       * is kept: install_request(1) resumes it), 0 none; its message */
int install_s1_installed(void);       /* the last status read: stage 1 installed, nothing left to write */
int plat_load_file(const char *name, void *dst, unsigned max, unsigned *len);   /* net.c: card file into RAM */
#endif
#endif
