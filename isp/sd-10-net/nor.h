/* SPI NOR flash (XMC XM25QH128C, 16 MB) through the SoC's own flash controller at 0x9C000B00, the controller vendor
 * U-Boot's `sfsp` drives (notes/nor_flash.md). Reads go through the controller, not the XIP window at 0x98000000.
 * Erase and program accept only the free region (nor_range_ok) and fail closed everywhere else. Net task only. */
#ifndef NOR_H
#define NOR_H

#define NOR_SIZE   0x01000000u
#define NOR_SECTOR 0x1000u          /* 4 KB erase (0x20) */
#define NOR_BLOCK  0x10000u         /* 64 KB erase (0xD8) */
#define NOR_PAGE   0x100u           /* page program (0x02) never crosses a page */
#define NOR_XIP    0x98000000u      /* cached read window, address = NOR_XIP + flash offset */
#define NOR_JEDEC  0x204018u        /* XMC XM25QH128C: the part the sequences were taken for; writes refuse any other */
#define NOR_MAGIC  0x57524f4eu      /* "NORW": every HU1 write sub-op carries it */

#define NOR_EARG     -1   /* address outside the allowed range, misaligned, or a bad buffer */
#define NOR_ETIME    -2   /* the controller or the flash did not finish in time */
#define NOR_EID      -3   /* JEDEC ID is not NOR_JEDEC */
#define NOR_EWP      -4   /* the block protection bits did not clear or restore */
#define NOR_EFAULT   -5   /* an earlier step timed out: no writes until a reboot */
#define NOR_ETASK    -6   /* called from a task other than the net task */
#define NOR_ESTATE   -7   /* controller state other than the one the U-Boot sequence was taken from */
#define NOR_EVERIFY  -8   /* read back differs */
#define NOR_ENOTBLANK -9  /* program target is not erased */
#define NOR_ECHANGED  -10 /* a boot block no longer holds what the install plan read */

/* Boot blocks (install.c, stage 1; notes/nor_install.md): outside the free region, the only blocks that may ever be
 * rewritten are the NOR boot script block, the block holding the U-Boot uImage header (its two xboot word sums) and the
 * block(s) holding U-Boot's default environment string, and only through nor_boot_write(). */
#define NOR_UBOOT_HDR  0x008A40E0u   /* xboot loads U-Boot's legacy uImage from here (isp/mkisp.py FLASH_UBOOT_HDR) */
#define NOR_UBOOT_MAX  0x001FFFC0u   /* xboot's largest U-Boot body ("image is too big") */
#define NOR_SCRIPT_BLK 0x00E00000u   /* the NOR boot script (`bootcmd=source 98e00000;bootsp`) */
#define NOR_ENV_MAX    64u           /* longest environment span stage 1 may change */

/* pure helpers (host-tested by test/nor_host.c) */
int nor_range_ok(unsigned off, unsigned len);      /* 1 if [off, off + len) lies inside one allowed write range */
unsigned nor_page_chunk(unsigned off, unsigned len);   /* bytes from off to the next page boundary, at most len */
int nor_wp_range(unsigned sr1, unsigned sr2, unsigned *lo, unsigned *hi);   /* protected [lo, hi): 0 none, 1 range,
                                                                              * -1 not decoded (SEC = 1) */
const char *nor_strerror(int e);
int nor_boot_block_ok(unsigned off, unsigned env_off, unsigned env_len);   /* 1: off is one of the boot blocks above
                                                                            * for an environment at env_off */
int nor_boot_diff_ok(unsigned off, const unsigned char *live, const unsigned char *blk, unsigned env_off,
                     unsigned env_len);   /* 1: the new block changes only the bytes stage 1 may change */

/* the driver */
int nor_jedec(unsigned *id);                        /* 0x9F: manufacturer << 16 | type << 8 | capacity */
int nor_status(unsigned sr[3]);                     /* SR1 (0x05), SR2 (0x35), SR3 (0x15) */
int nor_read(unsigned off, void *dst, unsigned len, unsigned cmd);   /* controller DMA read; cmd 0x03 or 0x0B */
int nor_erase(unsigned off, unsigned len);          /* 4 KB aligned; 64 KB blocks where aligned, sectors elsewhere */
int nor_program(unsigned off, const void *src, unsigned len);   /* split on page boundaries; the caller makes
                                                                 * sure the target is erased (the HU1 op checks) */

int nor_blank(unsigned off, unsigned len, unsigned *bad_c, unsigned *bad_x);   /* non-0xFF bytes: controller, XIP */
int nor_verify(unsigned off, const void *ram, unsigned len, unsigned *bad_c, unsigned *bad_x);   /* mismatches vs RAM */
int nor_boot_write(unsigned off, const void *blk, unsigned live_crc, unsigned env_off, unsigned env_len,
                   void *scratch);   /* rewrite one boot block (64 KB): fresh read of the live block (into scratch, 64 KB)
                                      * must have crc live_crc and differ from blk only where nor_boot_diff_ok allows;
                                      * erase, program (page 0 last), verify through the controller and XIP */

typedef int (*nor_fmt)(char *, const char *, ...);
int nor_op(const unsigned char *p, int n, char *out, nor_fmt fmt);   /* HU1 op 29, see nor.c */
#endif
