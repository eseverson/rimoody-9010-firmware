/* The compiler may emit calls to these for struct copies and initialisers even with -fno-builtin; the payload has no
 * C library. -fno-tree-loop-distribute-patterns keeps gcc from turning these loops back into calls to themselves. */
typedef __SIZE_TYPE__ size_t;

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
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
