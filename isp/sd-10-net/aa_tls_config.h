/* Mbed TLS configuration for the Android Auto link (MBEDTLS_CONFIG_FILE): TLS 1.2 with the head unit's certificate
 * (RSA-2048), ECDHE-RSA / RSA key exchange, AES-GCM and AES-CBC, SHA-1/256/384, X.509 + PEM parsing. No sockets,
 * files, threads or clock: TLS records travel inside the AA control channel through our own BIO callbacks, memory
 * comes from the FreeRTOS heap, randomness from CTR_DRBG seeded by mbedtls_hardware_poll (our entropy pool). */
#ifndef AA_TLS_CONFIG_H
#define AA_TLS_CONFIG_H

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_DEPRECATED_REMOVED
#define MBEDTLS_X509_REMOVE_INFO

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_AES_FEWER_TABLES   /* 1 KB of tables, not 4: gcm_alt.c's 4 KB GHASH table needs the cache room */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_GCM_C
#define MBEDTLS_GCM_ALT   /* gcm_alt.c: GHASH in 32-bit words, ~2x mbedtls's 64-bit one on this core (issue #3) */
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#ifdef AA_TLS_HOST   /* the host tests run a TLS server as the phone */
#define MBEDTLS_SSL_SRV_C
#endif
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_MAX_CONTENT_LEN 16384

#endif
