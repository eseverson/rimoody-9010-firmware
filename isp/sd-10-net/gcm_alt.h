/* AES-GCM for mbedtls (MBEDTLS_GCM_ALT): GHASH in 32-bit words and its own AES with a fixed cache layout, for this
 * 32-bit core with an 8 KB 2-way data cache (issue #3). Implemented in gcm_alt.c. */
#ifndef GCM_ALT_H
#define GCM_ALT_H

#include <stdint.h>

typedef struct mbedtls_gcm_context {
    uint32_t M[256][4];          /* M[b] = b * H, b a byte read GCM-style (bit 7 = x^0), words big-endian */
    uint32_t rk[60];             /* AES round keys, big-endian words */
    int nr;                      /* AES rounds: 10, 12 or 14 */
    uint32_t y[4];               /* GHASH state */
    uint32_t ctr[4];             /* counter block of the keystream in ek */
    uint32_t ek[4];              /* keystream for a partial block */
    uint32_t base_ek[4];         /* E(K, J0), for the tag */
    uint64_t len, add_len;
    int mode;
} mbedtls_gcm_context;

#endif
