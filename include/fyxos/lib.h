/*
 * FYXOS — 内核基础库（freestanding，不依赖任何 libc）
 */
#ifndef FYXOS_LIB_H
#define FYXOS_LIB_H

#include <fyxos/types.h>

/* ------------------------------- string --------------------------------- */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strdup(const char *s);          /* 使用内核堆 */
char  *strtok_r(char *s, const char *delim, char **saveptr);
int    isspace(int c);
int    isdigit(int c);
int    isalpha(int c);
int    isprint(int c);
int    toupper(int c);
int    tolower(int c);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

/* ------------------------------- printf --------------------------------- */
int  vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...);
int  kprintf(const char *fmt, ...);          /* 输出到控制台 */
int  ksnprintf(char *buf, size_t size, const char *fmt, ...);
void kputs(const char *s);
void puthex(u64 v);
void putdec(u64 v);

#endif /* FYXOS_LIB_H */
