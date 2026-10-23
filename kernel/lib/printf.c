/*
 * FYXOS — 格式化输出（freestanding，不依赖 libc）
 *
 * 支持：%s %c %d %i %u %x %X %o %p %%
 * 修饰符：长度 l / ll / h / z；宽度数字；'0' 补零；'-' 左对齐；'#' 加前缀
 */
#include <fyxos/lib.h>
#include <fyxos/kernel.h>

#define FLAG_LEFT    0x01u
#define FLAG_ZERO    0x02u
#define FLAG_PLUS    0x04u
#define FLAG_SPACE   0x08u
#define FLAG_ALT     0x10u
#define FLAG_UPPER   0x20u

enum len_mod { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z };

struct fmt_out {
    char  *buf;
    size_t size;
    size_t pos;
    int    (*emit)(char c, void *ctx);
    void  *ctx;
};

static void emit_char(struct fmt_out *o, char c)
{
    if (o->emit) {
        o->emit(c, o->ctx);
        o->pos++;
        return;
    }
    if (o->buf && o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void emit_pad(struct fmt_out *o, char c, int n)
{
    while (n-- > 0)
        emit_char(o, c);
}

static void emit_number(struct fmt_out *o, u64 value, int base,
                        int width, int flags)
{
    static const char digits_l[] = "0123456789abcdef";
    static const char digits_u[] = "0123456789ABCDEF";
    const char *digits = (flags & FLAG_UPPER) ? digits_u : digits_l;
    char tmp[32];
    int  n = 0;

    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value) {
            tmp[n++] = digits[value % (u64)base];
            value /= (u64)base;
        }
    }

    int prefix = 0;
    if (flags & FLAG_ALT) {
        if (base == 16)
            prefix = 2;
        else if (base == 8)
            prefix = 1;
    }
    if (flags & FLAG_PLUS)
        prefix += 1;

    int len = n + prefix;
    int pad = (width > len) ? width - len : 0;

    if (!(flags & FLAG_LEFT) && (flags & FLAG_ZERO)) {
        if (flags & FLAG_ALT) {
            emit_char(o, '0');
            if (base == 16)
                emit_char(o, (flags & FLAG_UPPER) ? 'X' : 'x');
        } else if (flags & FLAG_PLUS) {
            emit_char(o, '+');
        }
        emit_pad(o, '0', pad);
    } else {
        if (!(flags & FLAG_LEFT))
            emit_pad(o, ' ', pad);
        if (flags & FLAG_ALT) {
            emit_char(o, '0');
            if (base == 16)
                emit_char(o, (flags & FLAG_UPPER) ? 'X' : 'x');
        } else if (flags & FLAG_PLUS) {
            emit_char(o, '+');
        }
    }

    while (n-- > 0)
        emit_char(o, tmp[n]);

    if (flags & FLAG_LEFT)
        emit_pad(o, ' ', pad);
}

static void emit_signed(struct fmt_out *o, i64 value, int base,
                        int width, int flags)
{
    u64 mag;
    if (value < 0) {
        emit_char(o, '-');
        mag = (u64)(-(value + 1)) + 1u;   /* 避免对 INT64_MIN 取负溢出 */
        if (width > 0)
            width--;
    } else {
        mag = (u64)value;
    }
    emit_number(o, mag, base, width, flags & ~FLAG_PLUS);
}

int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
    struct fmt_out o = { buf, size, 0, NULL, NULL };
    const char *p = fmt;

    while (*p) {
        if (*p != '%') {
            emit_char(&o, *p++);
            continue;
        }
        p++;

        int flags = 0;
        for (;;) {
            if (*p == '-')       { flags |= FLAG_LEFT;  p++; }
            else if (*p == '0')  { flags |= FLAG_ZERO;  p++; }
            else if (*p == '+')  { flags |= FLAG_PLUS;  p++; }
            else if (*p == ' ')  { flags |= FLAG_SPACE; p++; }
            else if (*p == '#')  { flags |= FLAG_ALT;   p++; }
            else break;
        }

        int width = 0;
        while (isdigit(*p))
            width = width * 10 + (*p++ - '0');

        enum len_mod lm = LEN_NONE;
        if (*p == 'h') {
            lm = LEN_H;
            p++;
            if (*p == 'h') { lm = LEN_HH; p++; }
        } else if (*p == 'l') {
            lm = LEN_L;
            p++;
            if (*p == 'l') { lm = LEN_LL; p++; }
        } else if (*p == 'z') {
            lm = LEN_Z;
            p++;
        }

        char conv = *p++;
        switch (conv) {
        case '\0':
            emit_char(&o, '%');
            p--;
            break;
        case '%':
            emit_char(&o, '%');
            break;
        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            if (flags & FLAG_LEFT)
                emit_char(&o, c);
            emit_pad(&o, ' ', width - 1);
            if (!(flags & FLAG_LEFT))
                emit_char(&o, c);
            break;
        }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)strlen(s);
            int pad = (width > len) ? width - len : 0;
            if (!(flags & FLAG_LEFT))
                emit_pad(&o, ' ', pad);
            while (*s)
                emit_char(&o, *s++);
            if (flags & FLAG_LEFT)
                emit_pad(&o, ' ', pad);
            break;
        }
        case 'p':
            flags |= FLAG_ALT;
            emit_number(&o, (u64)(uintptr_t)__builtin_va_arg(ap, void *),
                        16, width, flags);
            break;
        case 'd':
        case 'i': {
            i64 v;
            if (lm == LEN_L)       v = __builtin_va_arg(ap, long);
            else if (lm == LEN_LL) v = __builtin_va_arg(ap, long long);
            else                   v = __builtin_va_arg(ap, int);
            emit_signed(&o, v, 10, width, flags);
            break;
        }
        case 'u': {
            u64 v;
            if (lm == LEN_L)       v = __builtin_va_arg(ap, unsigned long);
            else if (lm == LEN_LL) v = __builtin_va_arg(ap, unsigned long long);
            else                   v = __builtin_va_arg(ap, unsigned int);
            emit_number(&o, v, 10, width, flags);
            break;
        }
        case 'x':
        case 'X': {
            u64 v;
            if (lm == LEN_L)       v = __builtin_va_arg(ap, unsigned long);
            else if (lm == LEN_LL) v = __builtin_va_arg(ap, unsigned long long);
            else                   v = __builtin_va_arg(ap, unsigned int);
            if (conv == 'X')
                flags |= FLAG_UPPER;
            emit_number(&o, v, 16, width, flags);
            break;
        }
        case 'o': {
            u64 v = __builtin_va_arg(ap, unsigned int);
            emit_number(&o, v, 8, width, flags);
            break;
        }
        default:
            emit_char(&o, '%');
            emit_char(&o, conv);
            break;
        }
    }

    if (o.buf && o.size)
        o.buf[(o.pos < o.size - 1) ? o.pos : o.size - 1] = '\0';
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int kprintf(const char *fmt, ...)
{
    char tmp[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(tmp) - 1)
            n = (int)sizeof(tmp) - 1;
        console_write(tmp, (size_t)n);
    }
    return n;
}

void kputs(const char *s)
{
    console_write(s, strlen(s));
}

void puthex(u64 v)
{
    char buf[32];
    ksnprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    kputs(buf);
}

void putdec(u64 v)
{
    char buf[32];
    ksnprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    kputs(buf);
}
