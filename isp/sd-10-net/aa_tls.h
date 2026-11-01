/* TLS for the Android Auto link (aa_tls.c): we are the TLS client, records go through buffers, not a socket. */
#ifndef AA_TLS_H
#define AA_TLS_H
#include <stddef.h>
int aa_tls_init(const unsigned char *crt_pem, size_t crt_len, const unsigned char *key_pem, size_t key_len); /* lens incl. NUL */
int aa_tls_feed(const unsigned char *p, size_t n);   /* record bytes received from the phone; -1 if the buffer is full */
size_t aa_tls_take(unsigned char *out, size_t max);  /* record bytes to send to the phone */
int aa_tls_handshake(void);                          /* 0 done, 1 needs more bytes, <0 error (aa_tls_error) */
int aa_tls_write(const unsigned char *p, size_t n);  /* encrypt; the records land in the output buffer */
int aa_tls_read(unsigned char *p, size_t max);       /* decrypt what was fed; 0 = nothing complete yet */
const char *aa_tls_cipher(void);
const char *aa_tls_error(void);
void aa_tls_free(void);
#endif
