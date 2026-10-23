/*
 * FYXOS — 字符串与内存基础函数
 */
#include <fyxos/lib.h>
#include <fyxos/mm.h>

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    u8 v = (u8)c;
    while (n--)
        *d++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const u8 *p = s;
    u8 v = (u8)c;
    for (size_t i = 0; i < n; i++)
        if (p[i] == v)
            return (void *)(p + i);
    return NULL;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)(u8)a[i] - (int)(u8)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n-- && (*d++ = *src++) != '\0')
        ;
    while (n-- > 0)
        *d++ = '\0';
    return dst;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = kmalloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

char *strtok_r(char *s, const char *delim, char **saveptr)
{
    char *start;
    if (s)
        start = s;
    else if (saveptr && *saveptr)
        start = *saveptr;
    else
        return NULL;

    while (*start && strchr(delim, *start))
        start++;
    if (*start == '\0') {
        if (saveptr)
            *saveptr = start;
        return NULL;
    }

    char *end = start;
    while (*end && !strchr(delim, *end))
        end++;
    if (*end) {
        *end = '\0';
        end++;
    }
    if (saveptr)
        *saveptr = end;
    return start;
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isprint(int c)  { return c >= 0x20 && c < 0x7F; }
int toupper(int c)  { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }
int tolower(int c)  { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long val = 0;
    const char *p = s;

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    }

    while (*p) {
        int d;
        if (isdigit(*p))
            d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            break;
        if (d >= base)
            break;
        val = val * (unsigned long)base + (unsigned long)d;
        p++;
    }

    if (end)
        *end = (char *)p;
    return val;
}

long strtol(const char *s, char **end, int base)
{
    int neg = 0;
    while (isspace(*s))
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    unsigned long v = strtoul(s, end, base);
    return neg ? -(long)v : (long)v;
}
