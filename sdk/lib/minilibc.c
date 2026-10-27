#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <fyx.h>

size_t strlen(const char *text)
{
    size_t length = 0;
    while (text[length])
        length++;
    return length;
}

int strcmp(const char *left, const char *right)
{
    size_t i = 0;
    while (left[i] && left[i] == right[i])
        i++;
    return (int)(unsigned char)left[i] - (int)(unsigned char)right[i];
}

int strncmp(const char *left, const char *right, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a != b)
            return (int)a - (int)b;
        if (!a)
            return 0;
    }
    return 0;
}

void *memcpy(void *destination, const void *source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; i++)
        out[i] = in[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    if (out < in) {
        for (size_t i = 0; i < count; i++)
            out[i] = in[i];
    } else if (out > in) {
        for (size_t i = count; i > 0; i--)
            out[i - 1] = in[i - 1];
    }
    return destination;
}

void *memset(void *destination, int value, size_t count)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < count; i++)
        out[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < count; i++) {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

int putchar(int ch)
{
    char value = (char)ch;
    return sys_write(&value, 1) == 1 ? (int)(unsigned char)value : -1;
}

int puts(const char *text)
{
    size_t length = strlen(text);
    if (sys_write(text, length) != (long)length)
        return -1;
    return putchar('\n') == '\n' ? 0 : -1;
}
