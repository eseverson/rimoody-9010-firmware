/* AES-GCM for mbedtls (MBEDTLS_GCM_ALT), with GHASH done in 32-bit words: mbedtls's own GHASH works on 64-bit
 * integers, which this 32-bit core runs at ~75 cycles per byte (issue #3). Implemented in gcm_alt.c. */
#ifndef GCM_ALT_H
#define GCM_ALT_H

#include <stdint.h>
#include "mbedtls/aes.h"

typedef struct mbedtls_gcm_context {
    mbedtls_aes_context aes;
    uint32_t M[256][4];          /* M[b] = b * H, b a byte read GCM-style (bit 7 = x^0), words big-endian */
    uint32_t y[4];               /* GHASH state */
    unsigned char ctr[16];       /* counter block of the keystream in ectr */
    unsigned char ectr[16];      /* keystream for the current block */
    unsigned char base_ectr[16]; /* E(K, J0), for the tag */
    uint64_t len, add_len;
    int mode;
} mbedtls_gcm_context;

#endif
