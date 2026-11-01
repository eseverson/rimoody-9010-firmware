/* The compiler may emit calls to these for struct copies and initialisers even with -fno-builtin; the payload has no
 * C library. -fno-tree-loop-distribute-patterns keeps gcc from turning these loops back into calls to themselves. */
typedef __SIZE_TYPE__ size_t;

void *memcpy(void *d, const void *s, size_t n)   /* words when both are aligned (TLS records, video frames) */
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (!(((unsigned long)a | (unsigned long)b) & 3)) {
        for (; n >= 16; n -= 16, a += 16, b += 16) {
            unsigned int w0 = ((const unsigned int *)b)[0], w1 = ((const unsigned int *)b)[1];
            unsigned int w2 = ((const unsigned int *)b)[2], w3 = ((const unsigned int *)b)[3];
            ((unsigned int *)a)[0] = w0, ((unsigned int *)a)[1] = w1, ((unsigned int *)a)[2] = w2, ((unsigned int *)a)[3] = w3;
        }
        for (; n >= 4; n -= 4, a += 4, b += 4)
            *(unsigned int *)a = *(const unsigned int *)b;
    }
    while (n--)
        *a++ = *b++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a < b)
        while (n--)
            *a++ = *b++;
    else
        while (n--)
            a[n] = b[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *a = d;
    unsigned int w = (unsigned char)c * 0x01010101u;
    while (n && ((unsigned long)a & 3))
        *a++ = (unsigned char)c, n--;
    for (; n >= 4; n -= 4, a += 4)
        *(unsigned int *)a = w;
    while (n--)
        *a++ = (unsigned char)c;
    return d;
}

int memcmp(const void *x, const void *y, size_t n)
{
    const unsigned char *a = x, *b = y;
    for (; n--; a++, b++)
        if (*a != *b)
            return *a - *b;
    return 0;
}

char *strchr(const char *s, int c)   /* FatFs (invalid-character check on file names) */
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strncmp(const char *a, const char *b, size_t n)   /* lwIP (def.c) */
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

int atoi(const char *s)   /* lwIP (netif_name_to_index) */
{
    int v = 0, neg = 0;
    while (*s == ' ')
        s++;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

int strcmp(const char *a, const char *b)   /* mbedtls (cipher, md, ecp name lookups) */
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strstr(const char *h, const char *n)   /* mbedtls (PEM headers) */
{
    if (!*n)
        return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b)
            a++, b++;
        if (!*b)
            return (char *)h;
    }
    return 0;
}

/* mbedtls's default allocator (aa_tls.c installs the FreeRTOS heap itself; these keep the default pointers linkable) */
void *pvPortMalloc(size_t n);
void vPortFree(void *p);
void *calloc(size_t n, size_t s)
{
    if (s && n > (size_t)-1 / s)
        return 0;
    unsigned char *p = pvPortMalloc(n * s);
    if (p)
        for (size_t i = 0; i < n * s; i++)
            p[i] = 0;
    return p;
}
void free(void *p) { if (p) vPortFree(p); }

/* snprintf for mbedtls (OID / X.509 text, error strings): %d %u %x %X %s %c, '0' / width, 'l' / 'z' ignored */
#include <stdarg.h>
int vsnprintf(char *o, size_t max, const char *f, va_list ap)
{
    size_t k = 0;
#define PUT(c) do { if (k + 1 < max) o[k] = (c); k++; } while (0)
    for (; *f; f++) {
        if (*f != '%') {
            PUT(*f);
            continue;
        }
        f++;
        char pad = ' ';
        int w = 0;
        if (*f == '0')
            pad = '0', f++;
        while (*f >= '0' && *f <= '9')
            w = w * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'z' || *f == 'h')
            f++;
        char buf[24];
        int n = 0, neg = 0;
        const char *s = buf;
        if (*f == 'd' || *f == 'i') {
            long v = va_arg(ap, long);
            unsigned long u = v < 0 ? (neg = 1, 0ul - (unsigned long)v) : (unsigned long)v;
            do buf[n++] = (char)('0' + u % 10); while (u /= 10);
        } else if (*f == 'u' || *f == 'x' || *f == 'X') {
            unsigned long u = va_arg(ap, unsigned long), b = *f == 'u' ? 10 : 16;
            const char *dg = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do buf[n++] = dg[u % b]; while (u /= b);
        } else if (*f == 's') {
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int l = 0;
            while (s[l])
                l++;
            for (; w > l; w--)
                PUT(' ');
            while (*s)
                PUT(*s++);
            continue;
        } else if (*f == 'c') {
            PUT((char)va_arg(ap, int));
            continue;
        } else {
            PUT(*f ? *f : '%');
            if (!*f)
                break;
            continue;
        }
        if (neg && pad == '0') {
            PUT('-');
            w--;
        }
        for (int i = n + neg; i < w; i++)
            PUT(pad);
        if (neg && pad != '0')
            PUT('-');
        while (n)
            PUT(buf[--n]);
    }
    if (max)
        o[k < max ? k : max - 1] = 0;
    return (int)k;
#undef PUT
}
int snprintf(char *o, size_t max, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int r = vsnprintf(o, max, f, ap);
    va_end(ap);
    return r;
}
