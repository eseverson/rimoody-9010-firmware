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
