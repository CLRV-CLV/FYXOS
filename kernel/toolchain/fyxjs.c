/* FYXOS — FyxJS v1：JavaScript 小子集到原生 x86_64 ELF
 *
 * 前端只做明确的词法转换，随后交给 fyxcc 的表达式/控制流/ELF 后端：
 *   function main -> int main       let -> int
 *   console.log   -> puts           process.exit(x) -> return(x)
 *   ===/!==       -> ==/!=           true/false -> 1/0
 * 字符串与注释逐字保留。没有动态类型、对象、闭包、数组、事件循环或 JS 库。
 */
#include <fyxos/fyxjs.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/vfs.h>

#define JS_SOURCE_CAP 65536u

struct js_parser {
    const char *source;
    size_t length;
    size_t pos;
    char *output;
    size_t output_cap;
    size_t output_len;
    int error;
    struct fyxcc_diagnostic *diagnostic;
};

static bool js_ident_char(char c)
{
    return isalpha(c) || isdigit(c) || c == '_' || c == '$';
}

static void js_fail(struct js_parser *p, const char *message)
{
    if (p->error)
        return;
    p->error = -FYX_EINVAL;
    if (p->diagnostic) {
        p->diagnostic->offset = p->pos;
        strncpy(p->diagnostic->message, message,
                sizeof(p->diagnostic->message) - 1);
        p->diagnostic->message[sizeof(p->diagnostic->message) - 1] = '\0';
    }
}

static void js_put(struct js_parser *p, char c)
{
    if (p->output_len + 1 >= p->output_cap) {
        js_fail(p, "translated source is too large");
        return;
    }
    p->output[p->output_len++] = c;
}

static void js_write(struct js_parser *p, const char *text)
{
    while (*text)
        js_put(p, *text++);
}

static void js_copy(struct js_parser *p, size_t start, size_t end)
{
    while (start < end)
        js_put(p, p->source[start++]);
}

static void js_space(struct js_parser *p)
{
    while (p->pos < p->length && isspace(p->source[p->pos]))
        p->pos++;
}

static bool js_word_at(struct js_parser *p, const char *word)
{
    size_t n = strlen(word);
    if (p->pos + n > p->length ||
        memcmp(p->source + p->pos, word, n) != 0)
        return false;
    if (p->pos + n < p->length && js_ident_char(p->source[p->pos + n]))
        return false;
    p->pos += n;
    return true;
}

static bool js_expect_word(struct js_parser *p, const char *word)
{
    if (js_word_at(p, word))
        return true;
    js_fail(p, "expected function main");
    return false;
}

static bool js_expect(struct js_parser *p, char c)
{
    js_space(p);
    if (p->pos < p->length && p->source[p->pos] == c) {
        p->pos++;
        return true;
    }
    js_fail(p, "unexpected token in function main header");
    return false;
}

static bool js_copy_string(struct js_parser *p)
{
    char quote = p->source[p->pos];
    if (quote != '"') {
        js_fail(p, "only double-quoted strings are supported");
        return false;
    }
    js_put(p, p->source[p->pos++]);
    bool closed = false;
    while (p->pos < p->length) {
        char c = p->source[p->pos++];
        js_put(p, c);
        if (c == '\\' && p->pos < p->length)
            js_put(p, p->source[p->pos++]);
        else if (c == '"') {
            closed = true;
            break;
        }
    }
    if (!closed) {
        js_fail(p, "unterminated string");
        return false;
    }
    return !p->error;
}

static bool js_copy_comment(struct js_parser *p)
{
    if (p->pos + 1 >= p->length || p->source[p->pos] != '/')
        return false;
    char kind = p->source[p->pos + 1];
    if (kind != '/' && kind != '*')
        return false;
    js_put(p, p->source[p->pos++]);
    js_put(p, p->source[p->pos++]);
    if (kind == '/') {
        while (p->pos < p->length) {
            char c = p->source[p->pos++];
            js_put(p, c);
            if (c == '\n')
                break;
        }
        return true;
    }
    bool closed = false;
    while (p->pos < p->length) {
        char c = p->source[p->pos++];
        js_put(p, c);
        if (c == '*' && p->pos < p->length && p->source[p->pos] == '/') {
            js_put(p, p->source[p->pos++]);
            closed = true;
            break;
        }
    }
    if (!closed) {
        js_fail(p, "unterminated block comment");
        return false;
    }
    return true;
}

static bool js_member(struct js_parser *p, const char *member)
{
    if (p->pos >= p->length || p->source[p->pos] != '.')
        return false;
    size_t saved = p->pos;
    p->pos++;
    if (js_word_at(p, member))
        return true;
    p->pos = saved;
    return false;
}

static void js_translate_body(struct js_parser *p)
{
    while (p->pos < p->length && !p->error) {
        char c = p->source[p->pos];
        if (c == '"' || c == '\'') {
            js_copy_string(p);
            continue;
        }
        if (c == '`') {
            js_fail(p, "template literals are not supported");
            return;
        }
        if (js_copy_comment(p))
            continue;
        if (p->error)
            return;
        if (c == '=' && p->pos + 2 < p->length &&
            p->source[p->pos + 1] == '=' && p->source[p->pos + 2] == '=') {
            js_write(p, "==");
            p->pos += 3;
            continue;
        }
        if (c == '!' && p->pos + 2 < p->length &&
            p->source[p->pos + 1] == '=' && p->source[p->pos + 2] == '=') {
            js_write(p, "!=");
            p->pos += 3;
            continue;
        }
        if (isalpha(c) || c == '_' || c == '$') {
            size_t start = p->pos++;
            while (p->pos < p->length && js_ident_char(p->source[p->pos]))
                p->pos++;
            size_t end = p->pos;
            size_t n = end - start;
            if (n == 7 && memcmp(p->source + start, "console", 7) == 0 &&
                js_member(p, "log")) {
                js_write(p, "puts");
            } else if (n == 7 && memcmp(p->source + start, "process", 7) == 0 &&
                       js_member(p, "exit")) {
                js_write(p, "return");
            } else if (n == 3 && memcmp(p->source + start, "let", 3) == 0) {
                js_write(p, "int");
            } else if (n == 4 && memcmp(p->source + start, "true", 4) == 0) {
                js_put(p, '1');
            } else if (n == 5 && memcmp(p->source + start, "false", 5) == 0) {
                js_put(p, '0');
            } else {
                js_copy(p, start, end);
            }
            continue;
        }
        js_put(p, c);
        p->pos++;
    }
}

static void js_translate(struct js_parser *p)
{
    js_space(p);
    if (!js_expect_word(p, "function"))
        return;
    js_space(p);
    if (!js_expect_word(p, "main") || !js_expect(p, '(') || !js_expect(p, ')'))
        return;
    js_space(p);
    if (p->pos >= p->length || p->source[p->pos] != '{') {
        js_fail(p, "expected function main body");
        return;
    }
    js_write(p, "int main()");
    js_translate_body(p);
}

int fyxjs_compile(const char *source, size_t source_len,
                  void **out_image, size_t *out_size,
                  struct fyxcc_diagnostic *diagnostic)
{
    if (!source || !source_len || !out_image || !out_size)
        return -FYX_EINVAL;
    *out_image = NULL;
    *out_size = 0;
    if (diagnostic)
        memset(diagnostic, 0, sizeof(*diagnostic));
    if (source_len > JS_SOURCE_CAP)
        return -FYX_ERANGE;
    struct js_parser p;
    memset(&p, 0, sizeof(p));
    p.source = source;
    p.length = source_len;
    p.output_cap = source_len + 1;
    p.output = kmalloc(p.output_cap);
    p.diagnostic = diagnostic;
    if (!p.output)
        return -FYX_ENOMEM;
    js_translate(&p);
    int rc;
    if (p.error) {
        rc = p.error;
    } else {
        p.output[p.output_len] = '\0';
        rc = fyxcc_compile(p.output, p.output_len, out_image, out_size, diagnostic);
    }
    kfree(p.output);
    return rc;
}

int fyxjs_compile_file(const char *source_path, const char *output_path,
                       struct fyxcc_diagnostic *diagnostic)
{
    if (diagnostic)
        memset(diagnostic, 0, sizeof(*diagnostic));
    if (!source_path || !output_path)
        return -FYX_EINVAL;
    void *source = NULL, *image = NULL;
    size_t source_size = 0, image_size = 0;
    int rc = vfs_read_file(source_path, &source, &source_size);
    if (rc != 0)
        return rc;
    rc = fyxjs_compile(source, source_size, &image, &image_size, diagnostic);
    kfree(source);
    if (rc != 0)
        return rc;
    rc = vfs_write_file(output_path, image, image_size);
    kfree(image);
    return rc;
}
