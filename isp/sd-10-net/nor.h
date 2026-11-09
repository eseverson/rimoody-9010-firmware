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

/* pure helpers (host-tested by test/nor_host.c) */
int nor_range_ok(unsigned off, unsigned len);      /* 1 if [off, off + len) lies inside one allowed write range */
unsigned nor_page_chunk(unsigned off, unsigned len);   /* bytes from off to the next page boundary, at most len */
int nor_wp_range(unsigned sr1, unsigned sr2, unsigned *lo, unsigned *hi);   /* protected [lo, hi): 0 none, 1 range,
                                                                              * -1 not decoded (SEC = 1) */
const char *nor_strerror(int e);

/* the driver */
int nor_jedec(unsigned *id);                        /* 0x9F: manufacturer << 16 | type << 8 | capacity */
int nor_status(unsigned sr[3]);                     /* SR1 (0x05), SR2 (0x35), SR3 (0x15) */
int nor_read(unsigned off, void *dst, unsigned len, unsigned cmd);   /* controller DMA read; cmd 0x03 or 0x0B */
int nor_erase(unsigned off, unsigned len);          /* 4 KB aligned; 64 KB blocks where aligned, sectors elsewhere */
int nor_program(unsigned off, const void *src, unsigned len);   /* split on page boundaries; the caller makes
                                                                 * sure the target is erased (the HU1 op checks) */

typedef int (*nor_fmt)(char *, const char *, ...);
int nor_op(const unsigned char *p, int n, char *out, nor_fmt fmt);   /* HU1 op 29, see nor.c */
#endif
