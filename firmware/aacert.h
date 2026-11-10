/* The unit's Android Auto certificate and key, taken from the stock firmware in flash (notes/android_auto.md, "Where
 * the certificate lives"). The stock AA module, android_auto_ROM.bin, is a file in a JFFS2 filesystem in NOR; it is an
 * LZMA-alone stream (some sibling modules XOR it with one byte) whose unpacked code carries the head unit's private key
 * and certificate as PEM text. aacert finds the file by walking the flash for JFFS2 nodes (no fixed offsets), puts it
 * together in a work area, unpacks it in bounded steps and stops once a private key and a certificate that pair with it
 * are found (the caller's check callback decides; the payload uses mbedtls_pk_check_pair).
 *
 * Pure code, no hardware: flash is any readable window (the XIP window on the unit, a dump on the host; test/
 * aacert_host.c and tools/aacert.py do the same on the PC). */
#ifndef AACERT_H
#define AACERT_H

#define AACERT_NAME     "android_auto_ROM.bin"
#define AACERT_PEM_MAX  8192u   /* each output buffer, with the trailing newline and NUL */
#define AACERT_MAX_PEMS 8       /* candidate certificates and keys remembered while unpacking */

#define AACERT_ENOMOD  -1   /* no android_auto_ROM.bin in any JFFS2 filesystem in the window */
#define AACERT_ENODATA -2   /* its data nodes are missing, damaged or compressed by JFFS2 */
#define AACERT_EHDR    -3   /* the file is not an LZMA-alone stream with a known size */
#define AACERT_EROOM   -4   /* the file or its unpacked size does not fit the work area */
#define AACERT_EDATA   -5   /* the LZMA stream is damaged or truncated */
#define AACERT_ENOPEM  -6   /* no private key or no certificate in the unpacked module */
#define AACERT_ENOPAIR -7   /* none of its certificates pairs with its key */

/* 0 = the certificate pairs with the key. Both are NUL-terminated PEM; the lengths count the NUL (mbedtls style). */
typedef int (*aacert_check_fn)(void *ctx, const char *cert, unsigned cert_len, const char *key, unsigned key_len);

struct aacert {
    /* set by aacert_open */
    unsigned dirent;      /* flash offset of the file's directory entry */
    unsigned ino;         /* its JFFS2 inode number */
    unsigned nodes;       /* data nodes put together */
    unsigned packed;      /* file size: the LZMA-alone stream */
    unsigned unpacked;    /* unpacked size, from the LZMA header */
    unsigned xor_key;     /* the module's one-byte XOR, 0 for none */
    unsigned first_node;  /* flash offset of the node holding file offset 0 (the LZMA header) */
    unsigned work_used;   /* bytes of the work area in use */
    /* progress, set by aacert_step */
    unsigned decoded;     /* bytes unpacked so far */
    unsigned key_at, cert_at;   /* offsets of the chosen PEM blocks in the unpacked module */
    unsigned key_len, cert_len; /* their lengths in the outputs, with the newline, without the NUL */
    int result;           /* 0 working, 1 pair found, < 0 AACERT_E* */
    /* private */
    unsigned char *pk, *out;    /* the packed file and the unpacked module, in the work area */
    void *dec;                  /* the LZMA decoder state, in the work area */
    unsigned in_pos, scan;
    char *cert_out, *key_out;
    unsigned ncert, nkey, cert_off[AACERT_MAX_PEMS], cert_n[AACERT_MAX_PEMS], key_off[AACERT_MAX_PEMS],
        key_n[AACERT_MAX_PEMS];
};

/* Find the module in flash[0, flash_len), put it together in work and set up the decoder. 0, or AACERT_E*. */
int aacert_open(struct aacert *a, const unsigned char *flash, unsigned flash_len, void *work, unsigned work_len);
/* Unpack at most budget more bytes and look for the key and certificate. 1 = call again, 0 = done (cert_out / key_out
 * hold the PEM text, each ending in a newline and a NUL; AACERT_PEM_MAX bytes each), < 0 = AACERT_E*. */
int aacert_step(struct aacert *a, unsigned budget, char *cert_out, char *key_out, aacert_check_fn check, void *ctx);
const char *aacert_strerror(int e);

#endif
