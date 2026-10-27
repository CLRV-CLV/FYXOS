/* FYXOS — 小型 C 子集到 x86_64 静态 ELF 的原生编译器
 *
 * 支持：int main、局部整数变量、简单/复合赋值、算术/比较/移位/位/逻辑运算、
 * ++/--、puts/print 字符串、return、if/else、while/do…while/for、?:、
 * break/continue 与嵌套块。
 * 生成代码直接使用
 * FYXOS SYSCALL ABI，
 * 不依赖宿主 libc、GCC 或外部链接器；变量在用户栈上，保持 ELF 段 RX。
 * 这不是完整 C：没有函数调用、指针、数组、结构体、预处理器或 libc。
 */
#include <fyxos/fyxcc.h>
#include <fyxos/elf.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/vfs.h>

#define CC_IMAGE_CAP   65536u
#define CC_DATA_CAP    16384u
#define CC_MAX_FIXUPS  128u
#define CC_MAX_VARS    16u
#define CC_VAR_NAME    24u
#define CC_MAX_DEPTH   32u
#define CC_MAX_CASES   64u
#define CC_MAX_FUNCS   16u
#define CC_MAX_PARAMS  4u
#define CC_MAX_CALLS   128u
#define CC_FRAME_SIZE  ((CC_MAX_VARS + CC_MAX_DEPTH) * 8u)
#define CC_CONTROL_LOOP   1u
#define CC_CONTROL_SWITCH 2u
#define CC_BASE        0x400000ull
#define CC_CODE_OFFSET 128u                 /* EHDR + PHDR，按 8 字节对齐 */

struct cc_fixup {
    size_t disp_pos;
    size_t next_ip;
    size_t data_offset;
};

struct cc_var {
    char name[CC_VAR_NAME];
    u32 offset;                             /* 局部：rbp-offset；参数：rbp+offset */
    bool parameter;
};

struct cc_function {
    char name[CC_VAR_NAME];
    size_t address;
    u32 param_count;
    bool defined;
};

struct cc_call_fixup {
    size_t disp_pos;
    size_t next_ip;
    size_t source_offset;
    u32 function_index;
};

struct cc_control {
    size_t break_chain;                     /* disp32 中暂存下一条链位置 */
    size_t continue_chain;
    size_t continue_target;
    size_t default_target;
    struct cc_var switch_var;               /* 独立的帧内隐藏槽，不占 16 个源码变量 */
    u32 id;                                 /* 全局唯一，避免顺序 switch 复用深度编号 */
    u8 kind;
};

struct cc_case {
    i64 value;
    size_t target;
    u32 owner;
};

struct cc_parser {
    const char *source;
    size_t length;
    size_t pos;
    u8 *image;
    u8 *data;
    size_t code_pos;
    size_t data_len;
    struct cc_fixup fixups[CC_MAX_FIXUPS];
    size_t fixup_count;
    struct cc_var vars[CC_MAX_VARS];
    u32 var_count;
    struct cc_function functions[CC_MAX_FUNCS];
    u32 function_count;
    struct cc_call_fixup calls[CC_MAX_CALLS];
    u32 call_count;
    bool current_is_main;
    u32 depth;
    struct cc_control controls[CC_MAX_DEPTH];
    u32 control_depth;
    struct cc_case cases[CC_MAX_CASES];
    u32 case_count;
    u32 switch_serial;
    int error;
    struct fyxcc_diagnostic *diagnostic;
};

static void cc_fail(struct cc_parser *p, const char *message)
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

static bool identifier_char(char c)
{
    return isalpha(c) || isdigit(c) || c == '_';
}

static void cc_skip(struct cc_parser *p)
{
    for (;;) {
        while (p->pos < p->length && isspace(p->source[p->pos]))
            p->pos++;
        if (p->pos + 1 >= p->length || p->source[p->pos] != '/')
            return;
        if (p->source[p->pos + 1] == '/') {
            p->pos += 2;
            while (p->pos < p->length && p->source[p->pos] != '\n')
                p->pos++;
            continue;
        }
        if (p->source[p->pos + 1] == '*') {
            p->pos += 2;
            bool closed = false;
            while (p->pos + 1 < p->length) {
                if (p->source[p->pos] == '*' && p->source[p->pos + 1] == '/') {
                    p->pos += 2;
                    closed = true;
                    break;
                }
                p->pos++;
            }
            if (!closed) {
                cc_fail(p, "unterminated block comment");
                return;
            }
            continue;
        }
        return;
    }
}

static bool cc_word(struct cc_parser *p, const char *word)
{
    cc_skip(p);
    size_t n = strlen(word);
    if (p->pos + n > p->length || memcmp(p->source + p->pos, word, n) != 0)
        return false;
    if (p->pos + n < p->length && identifier_char(p->source[p->pos + n]))
        return false;
    p->pos += n;
    return true;
}

static bool cc_expect_word(struct cc_parser *p, const char *word)
{
    if (cc_word(p, word))
        return true;
    cc_fail(p, "expected keyword");
    return false;
}

static bool cc_consume(struct cc_parser *p, char c)
{
    cc_skip(p);
    if (p->pos < p->length && p->source[p->pos] == c) {
        p->pos++;
        return true;
    }
    return false;
}

static bool cc_consume_operator(struct cc_parser *p, const char *operator)
{
    cc_skip(p);
    size_t length = strlen(operator);
    if (p->pos + length > p->length ||
        memcmp(p->source + p->pos, operator, length) != 0)
        return false;
    p->pos += length;
    return true;
}

static char cc_assignment_operator(struct cc_parser *p)
{
    cc_skip(p);
    if (p->pos >= p->length)
        return 0;
    char op = p->source[p->pos];
    if (op == '=') {
        if (p->pos + 1 < p->length && p->source[p->pos + 1] == '=')
            return 0;
        p->pos++;
        return op;
    }
    if ((op == '+' || op == '-' || op == '*' || op == '/' || op == '%') &&
        p->pos + 1 < p->length && p->source[p->pos + 1] == '=') {
        p->pos += 2;
        return op;
    }
    return 0;
}

static bool cc_expect(struct cc_parser *p, char c)
{
    if (cc_consume(p, c))
        return true;
    cc_fail(p, "unexpected punctuation");
    return false;
}

static bool cc_identifier(struct cc_parser *p, char out[CC_VAR_NAME])
{
    cc_skip(p);
    if (p->pos >= p->length ||
        (!isalpha(p->source[p->pos]) && p->source[p->pos] != '_'))
        return false;
    size_t n = 0;
    while (p->pos < p->length && identifier_char(p->source[p->pos])) {
        if (n + 1 >= CC_VAR_NAME) {
            cc_fail(p, "identifier is too long");
            return false;
        }
        out[n++] = p->source[p->pos++];
    }
    out[n] = '\0';
    return true;
}

static void cc_emit8(struct cc_parser *p, u8 value)
{
    if (p->code_pos >= CC_IMAGE_CAP) {
        cc_fail(p, "generated code is too large");
        return;
    }
    p->image[p->code_pos++] = value;
}

static void cc_emit32(struct cc_parser *p, u32 value)
{
    cc_emit8(p, (u8)value);
    cc_emit8(p, (u8)(value >> 8));
    cc_emit8(p, (u8)(value >> 16));
    cc_emit8(p, (u8)(value >> 24));
}

static void cc_patch32(struct cc_parser *p, size_t pos, u32 value)
{
    if (pos + 4 > p->code_pos || pos + 4 > CC_IMAGE_CAP) {
        cc_fail(p, "invalid generated fixup");
        return;
    }
    p->image[pos + 0] = (u8)value;
    p->image[pos + 1] = (u8)(value >> 8);
    p->image[pos + 2] = (u8)(value >> 16);
    p->image[pos + 3] = (u8)(value >> 24);
}

static void cc_patch_jump(struct cc_parser *p, size_t disp_pos, size_t target)
{
    i64 displacement = (i64)target - (i64)(disp_pos + 4);
    if (displacement < -0x80000000ll || displacement > 0x7FFFFFFFll) {
        cc_fail(p, "branch displacement out of range");
        return;
    }
    cc_patch32(p, disp_pos, (u32)(i32)displacement);
}

static void cc_emit_syscall(struct cc_parser *p, u32 number)
{
    cc_emit8(p, 0xB8);                         /* mov eax, imm32 */
    cc_emit32(p, number);
    cc_emit8(p, 0x0F);
    cc_emit8(p, 0x05);                         /* syscall */
}

static void cc_emit_imm(struct cc_parser *p, i64 value)
{
    if (value < -0x80000000ll || value > 0x7FFFFFFFll) {
        cc_fail(p, "integer literal does not fit int32");
        return;
    }
    cc_emit8(p, 0x48);                         /* mov rax, sign-extended imm32 */
    cc_emit8(p, 0xC7);
    cc_emit8(p, 0xC0);
    cc_emit32(p, (u32)(i32)value);
}

static void cc_emit_imm_rcx(struct cc_parser *p, i64 value)
{
    if (value < -0x80000000ll || value > 0x7FFFFFFFll) {
        cc_fail(p, "integer literal does not fit int32");
        return;
    }
    cc_emit8(p, 0x48);                         /* mov rcx, sign-extended imm32 */
    cc_emit8(p, 0xC7);
    cc_emit8(p, 0xC1);
    cc_emit32(p, (u32)(i32)value);
}

static void cc_emit_call(struct cc_parser *p, struct cc_function *fn)
{
    if (p->call_count >= CC_MAX_CALLS) {
        cc_fail(p, "too many function call sites");
        return;
    }
    struct cc_call_fixup *call = &p->calls[p->call_count++];
    call->source_offset = p->pos;
    call->function_index = (u32)(fn - p->functions);
    cc_emit8(p, 0xE8);                         /* call rel32，尾部统一回填 */
    call->disp_pos = p->code_pos;
    cc_emit32(p, 0);
    call->next_ip = p->code_pos;
}

static bool cc_resolve_calls(struct cc_parser *p)
{
    for (u32 i = 0; i < p->call_count; i++) {
        struct cc_call_fixup *call = &p->calls[i];
        struct cc_function *fn = &p->functions[call->function_index];
        if (!fn->defined) {
            p->pos = call->source_offset;
            cc_fail(p, "undefined function");
            return false;
        }
        i64 displacement = (i64)fn->address - (i64)call->next_ip;
        if (displacement < -0x80000000ll || displacement > 0x7FFFFFFFll) {
            cc_fail(p, "call displacement out of range");
            return false;
        }
        cc_patch32(p, call->disp_pos, (u32)(i32)displacement);
    }
    return !p->error;
}

static void cc_emit_discard_args(struct cc_parser *p, u32 count)
{
    if (!count)
        return;
    cc_emit8(p, 0x48);                         /* add rsp, count * 8 */
    cc_emit8(p, 0x81);
    cc_emit8(p, 0xC4);
    cc_emit32(p, count * 8u);
}

static void cc_emit_prologue(struct cc_parser *p)
{
    cc_emit8(p, 0x55);                         /* push rbp */
    cc_emit8(p, 0x48);                         /* mov rbp, rsp */
    cc_emit8(p, 0x89);
    cc_emit8(p, 0xE5);
    cc_emit8(p, 0x48);                         /* sub rsp, CC_FRAME_SIZE */
    cc_emit8(p, 0x81);
    cc_emit8(p, 0xEC);
    cc_emit32(p, CC_FRAME_SIZE);
}

static struct cc_var *cc_find_var(struct cc_parser *p, const char *name)
{
    for (u32 i = 0; i < p->var_count; i++)
        if (strcmp(p->vars[i].name, name) == 0)
            return &p->vars[i];
    return NULL;
}

static struct cc_var *cc_add_var(struct cc_parser *p, const char *name)
{
    if (cc_find_var(p, name)) {
        cc_fail(p, "duplicate local variable");
        return NULL;
    }
    if (p->var_count >= CC_MAX_VARS) {
        cc_fail(p, "too many local variables");
        return NULL;
    }
    struct cc_var *var = &p->vars[p->var_count++];
    memset(var, 0, sizeof(*var));
    strncpy(var->name, name, sizeof(var->name) - 1);
    var->offset = p->var_count * 8u;
    return var;
}

static struct cc_function *cc_find_function(struct cc_parser *p,
                                             const char *name)
{
    for (u32 i = 0; i < p->function_count; i++)
        if (strcmp(p->functions[i].name, name) == 0)
            return &p->functions[i];
    return NULL;
}

static struct cc_function *cc_declare_function(struct cc_parser *p,
                                                const char *name,
                                                u32 param_count,
                                                bool definition)
{
    struct cc_function *fn = cc_find_function(p, name);
    if (fn) {
        if (fn->param_count != param_count) {
            cc_fail(p, "conflicting function declaration");
            return NULL;
        }
        if (definition && fn->defined) {
            cc_fail(p, "duplicate function definition");
            return NULL;
        }
    } else {
        if (p->function_count >= CC_MAX_FUNCS) {
            cc_fail(p, "too many functions");
            return NULL;
        }
        fn = &p->functions[p->function_count++];
        memset(fn, 0, sizeof(*fn));
        strncpy(fn->name, name, sizeof(fn->name) - 1);
        fn->param_count = param_count;
    }
    if (definition) {
        fn->address = p->code_pos;
        fn->defined = true;                    /* 自身递归立即可见 */
    }
    return fn;
}

static void cc_emit_load_var(struct cc_parser *p, const struct cc_var *var)
{
    i32 displacement = var->parameter ? (i32)var->offset : -(i32)var->offset;
    cc_emit8(p, 0x48);                         /* mov rax, [rbp+disp32] */
    cc_emit8(p, 0x8B);
    cc_emit8(p, 0x85);
    cc_emit32(p, (u32)displacement);
}

static void cc_emit_store_var(struct cc_parser *p, const struct cc_var *var)
{
    i32 displacement = var->parameter ? (i32)var->offset : -(i32)var->offset;
    cc_emit8(p, 0x48);                         /* mov [rbp+disp32], rax */
    cc_emit8(p, 0x89);
    cc_emit8(p, 0x85);
    cc_emit32(p, (u32)displacement);
}

static void cc_emit_increment(struct cc_parser *p, bool decrement)
{
    cc_emit8(p, 0x48);                         /* add/sub rax, imm8(1) */
    cc_emit8(p, 0x83);
    cc_emit8(p, decrement ? 0xE8 : 0xC0);
    cc_emit8(p, 1);
}

static void cc_emit_test(struct cc_parser *p)
{
    cc_emit8(p, 0x48);                         /* test rax, rax */
    cc_emit8(p, 0x85);
    cc_emit8(p, 0xC0);
}

static size_t cc_emit_jz(struct cc_parser *p)
{
    cc_emit8(p, 0x0F);
    cc_emit8(p, 0x84);                         /* jz rel32 */
    size_t pos = p->code_pos;
    cc_emit32(p, 0);
    return pos;
}

static size_t cc_emit_jnz(struct cc_parser *p)
{
    cc_emit8(p, 0x0F);
    cc_emit8(p, 0x85);                         /* jnz rel32 */
    size_t pos = p->code_pos;
    cc_emit32(p, 0);
    return pos;
}

static size_t cc_emit_jmp(struct cc_parser *p)
{
    cc_emit8(p, 0xE9);                         /* jmp rel32 */
    size_t pos = p->code_pos;
    cc_emit32(p, 0);
    return pos;
}

static void cc_emit_loop_jump(struct cc_parser *p, size_t *chain)
{
    cc_emit8(p, 0xE9);
    size_t pos = p->code_pos;
    cc_emit32(p, (u32)*chain);                 /* 暂时保存链表，而非位移 */
    *chain = pos;
}

static void cc_emit_condition_chain(struct cc_parser *p, u8 condition,
                                    size_t *chain)
{
    cc_emit8(p, 0x0F);
    cc_emit8(p, condition);                    /* jz/jnz rel32 */
    size_t pos = p->code_pos;
    cc_emit32(p, (u32)*chain);
    *chain = pos;
}

static void cc_patch_chain(struct cc_parser *p, size_t chain,
                           size_t target)
{
    while (chain) {
        if (chain + 4 > p->code_pos) {
            cc_fail(p, "invalid loop fixup chain");
            return;
        }
        u32 next = (u32)p->image[chain] |
                   ((u32)p->image[chain + 1] << 8) |
                   ((u32)p->image[chain + 2] << 16) |
                   ((u32)p->image[chain + 3] << 24);
        cc_patch_jump(p, chain, target);
        if (p->error)
            return;
        chain = next;
    }
}

static bool cc_parse_string(struct cc_parser *p, size_t *offset, size_t *length)
{
    cc_skip(p);
    if (p->pos >= p->length || p->source[p->pos] != '"') {
        cc_fail(p, "expected string literal");
        return false;
    }
    p->pos++;
    size_t start = p->data_len;
    bool closed = false;
    while (p->pos < p->length) {
        char c = p->source[p->pos++];
        if (c == '"') {
            closed = true;
            break;
        }
        if (c == '\n' || c == '\r') {
            cc_fail(p, "newline in string literal");
            return false;
        }
        if (c == '\\') {
            if (p->pos >= p->length) {
                cc_fail(p, "unterminated escape");
                return false;
            }
            char escaped = p->source[p->pos++];
            switch (escaped) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            case '0': c = '\0'; break;
            default:
                cc_fail(p, "unsupported string escape");
                return false;
            }
        }
        if (p->data_len >= CC_DATA_CAP) {
            cc_fail(p, "string data is too large");
            return false;
        }
        p->data[p->data_len++] = (u8)c;
    }
    if (!closed) {
        cc_fail(p, "unterminated string literal");
        return false;
    }
    *offset = start;
    *length = p->data_len - start;
    return true;
}

static bool cc_parse_number(struct cc_parser *p, i64 *out)
{
    cc_skip(p);
    if (p->pos >= p->length || !isdigit(p->source[p->pos])) {
        cc_fail(p, "expected integer");
        return false;
    }
    u32 base = 10;
    if (p->pos + 2 <= p->length && p->source[p->pos] == '0' &&
        (p->source[p->pos + 1] == 'x' || p->source[p->pos + 1] == 'X')) {
        base = 16;
        p->pos += 2;
    }
    u64 value = 0;
    size_t digits = 0;
    while (p->pos < p->length) {
        char c = p->source[p->pos];
        u32 digit;
        if (c >= '0' && c <= '9')
            digit = (u32)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = (u32)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = (u32)(c - 'A' + 10);
        else
            break;
        if (digit >= base)
            break;
        if (value > (~0ull - digit) / base) {
            cc_fail(p, "integer literal overflow");
            return false;
        }
        value = value * base + digit;
        p->pos++;
        digits++;
    }
    if (!digits) {
        cc_fail(p, "invalid integer");
        return false;
    }
    *out = (i64)value;
    return true;
}

static bool cc_parse_expression(struct cc_parser *p);

static bool cc_parse_primary(struct cc_parser *p)
{
    cc_skip(p);
    bool increment = cc_consume_operator(p, "++");
    bool decrement = !increment && cc_consume_operator(p, "--");
    if (increment || decrement) {
        char name[CC_VAR_NAME];
        if (!cc_identifier(p, name)) {
            cc_fail(p, "expected variable after prefix operator");
            return false;
        }
        struct cc_var *var = cc_find_var(p, name);
        if (!var) {
            cc_fail(p, "unknown local variable");
            return false;
        }
        cc_emit_load_var(p, var);
        cc_emit_increment(p, decrement);
        cc_emit_store_var(p, var);
        return !p->error;
    }
    if (cc_consume(p, '!')) {
        if (!cc_parse_primary(p))
            return false;
        cc_emit_test(p);
        cc_emit8(p, 0xB8);                     /* mov eax, 0（保留 ZF） */
        cc_emit32(p, 0);
        cc_emit8(p, 0x0F);                     /* sete al */
        cc_emit8(p, 0x94);
        cc_emit8(p, 0xC0);
        return !p->error;
    }
    if (cc_consume(p, '~')) {
        if (!cc_parse_primary(p))
            return false;
        cc_emit8(p, 0x48);                     /* not rax */
        cc_emit8(p, 0xF7);
        cc_emit8(p, 0xD0);
        return !p->error;
    }
    if (cc_consume(p, '-')) {
        if (!cc_parse_primary(p))
            return false;
        cc_emit8(p, 0x48);                     /* neg rax */
        cc_emit8(p, 0xF7);
        cc_emit8(p, 0xD8);
        return !p->error;
    }
    if (cc_consume(p, '+'))
        return cc_parse_primary(p);
    if (cc_consume(p, '('))
        return cc_parse_expression(p) && cc_expect(p, ')');
    if (p->pos < p->length && isdigit(p->source[p->pos])) {
        i64 value;
        if (!cc_parse_number(p, &value))
            return false;
        cc_emit_imm(p, value);
        return !p->error;
    }
    char name[CC_VAR_NAME];
    if (cc_identifier(p, name)) {
        if (cc_consume(p, '(')) {
            struct cc_function *fn = cc_find_function(p, name);
            if (!fn) {
                cc_fail(p, "unknown function (prototype or definition required)");
                return false;
            }
            u32 arg_count = 0;
            if (!cc_consume(p, ')')) {
                for (;;) {
                    if (arg_count >= CC_MAX_PARAMS) {
                        cc_fail(p, "too many function arguments");
                        return false;
                    }
                    if (!cc_parse_expression(p))
                        return false;
                    cc_emit8(p, 0x50);         /* push 参数，按源码从左到右 */
                    arg_count++;
                    if (cc_consume(p, ')'))
                        break;
                    if (!cc_expect(p, ','))
                        return false;
                }
            }
            if (arg_count != fn->param_count) {
                cc_fail(p, "function argument count mismatch");
                return false;
            }
            cc_emit_call(p, fn);
            cc_emit_discard_args(p, arg_count);
            return !p->error;
        }
        struct cc_var *var = cc_find_var(p, name);
        if (!var) {
            cc_fail(p, "unknown local variable");
            return false;
        }
        cc_emit_load_var(p, var);
        increment = cc_consume_operator(p, "++");
        decrement = !increment && cc_consume_operator(p, "--");
        if (increment || decrement) {
            cc_emit8(p, 0x48);                 /* mov rcx, rax：保存旧值 */
            cc_emit8(p, 0x89);
            cc_emit8(p, 0xC1);
            cc_emit_increment(p, decrement);
            cc_emit_store_var(p, var);
            cc_emit8(p, 0x48);                 /* mov rax, rcx：后缀返回旧值 */
            cc_emit8(p, 0x89);
            cc_emit8(p, 0xC8);
        }
        return !p->error;
    }
    cc_fail(p, "expected expression");
    return false;
}

static void cc_emit_right_operand(struct cc_parser *p)
{
    cc_emit8(p, 0x48);                         /* mov rcx, rax */
    cc_emit8(p, 0x89);
    cc_emit8(p, 0xC1);
    cc_emit8(p, 0x58);                         /* pop rax (left operand) */
}

static bool cc_parse_mul(struct cc_parser *p)
{
    if (!cc_parse_primary(p))
        return false;
    for (;;) {
        cc_skip(p);
        if (p->pos >= p->length ||
            (p->source[p->pos] != '*' && p->source[p->pos] != '/' &&
             p->source[p->pos] != '%'))
            return true;
        char op = p->source[p->pos++];
        cc_emit8(p, 0x50);                     /* push left */
        if (!cc_parse_primary(p))
            return false;
        cc_emit_right_operand(p);
        if (op == '*') {
            cc_emit8(p, 0x48);                 /* imul rax, rcx */
            cc_emit8(p, 0x0F);
            cc_emit8(p, 0xAF);
            cc_emit8(p, 0xC1);
        } else {
            cc_emit8(p, 0x48);                 /* cqo; idiv rcx */
            cc_emit8(p, 0x99);
            cc_emit8(p, 0x48);
            cc_emit8(p, 0xF7);
            cc_emit8(p, 0xF9);
            if (op == '%') {                   /* idiv 的余数在 rdx */
                cc_emit8(p, 0x48);
                cc_emit8(p, 0x89);
                cc_emit8(p, 0xD0);
            }
        }
    }
}

static bool cc_parse_add(struct cc_parser *p)
{
    if (!cc_parse_mul(p))
        return false;
    for (;;) {
        cc_skip(p);
        if (p->pos >= p->length ||
            (p->source[p->pos] != '+' && p->source[p->pos] != '-'))
            return true;
        char op = p->source[p->pos++];
        cc_emit8(p, 0x50);
        if (!cc_parse_mul(p))
            return false;
        cc_emit_right_operand(p);
        cc_emit8(p, 0x48);
        cc_emit8(p, op == '+' ? 0x01 : 0x29);  /* add/sub rax, rcx */
        cc_emit8(p, 0xC8);
    }
}

static u8 cc_relational_operator(struct cc_parser *p)
{
    cc_skip(p);
    if (p->pos + 1 < p->length) {
        char a = p->source[p->pos], b = p->source[p->pos + 1];
        u8 op = 0;
        if (a == '<' && b == '=') op = 0x9E;   /* setle */
        else if (a == '>' && b == '=') op = 0x9D; /* setge */
        else if ((a == '<' && b == '<') || (a == '>' && b == '>'))
            return 0;
        if (op) {
            p->pos += 2;
            return op;
        }
    }
    if (p->pos < p->length && p->source[p->pos] == '<') {
        p->pos++;
        return 0x9C;
    }
    if (p->pos < p->length && p->source[p->pos] == '>') {
        p->pos++;
        return 0x9F;
    }
    return 0;
}

static u8 cc_equality_operator(struct cc_parser *p)
{
    cc_skip(p);
    if (p->pos + 1 >= p->length)
        return 0;
    char a = p->source[p->pos], b = p->source[p->pos + 1];
    if (a == '=' && b == '=') {
        p->pos += 2;
        return 0x94;
    }
    if (a == '!' && b == '=') {
        p->pos += 2;
        return 0x95;
    }
    return 0;
}

static void cc_emit_comparison(struct cc_parser *p, u8 comparison)
{
    cc_emit_right_operand(p);
    cc_emit8(p, 0x48);                         /* cmp rax, rcx */
    cc_emit8(p, 0x39);
    cc_emit8(p, 0xC8);
    cc_emit8(p, 0xB8);                         /* mov eax, 0（不改 flags） */
    cc_emit32(p, 0);
    cc_emit8(p, 0x0F);
    cc_emit8(p, comparison);
    cc_emit8(p, 0xC0);
}

static bool cc_parse_shift(struct cc_parser *p)
{
    if (!cc_parse_add(p))
        return false;
    for (;;) {
        bool left = cc_consume_operator(p, "<<");
        bool right = !left && cc_consume_operator(p, ">>");
        if (!left && !right)
            return !p->error;
        cc_emit8(p, 0x50);
        if (!cc_parse_add(p))
            return false;
        cc_emit_right_operand(p);              /* 移位计数自动使用 CL */
        cc_emit8(p, 0x48);
        cc_emit8(p, 0xD3);
        cc_emit8(p, left ? 0xE0 : 0xF8);       /* shl/sar rax, cl */
    }
}

static bool cc_parse_relational(struct cc_parser *p)
{
    if (!cc_parse_shift(p))
        return false;
    for (;;) {
        u8 comparison = cc_relational_operator(p);
        if (!comparison)
            return !p->error;
        cc_emit8(p, 0x50);
        if (!cc_parse_shift(p))
            return false;
        cc_emit_comparison(p, comparison);
    }
}

static bool cc_parse_equality(struct cc_parser *p)
{
    if (!cc_parse_relational(p))
        return false;
    for (;;) {
        u8 comparison = cc_equality_operator(p);
        if (!comparison)
            return !p->error;
        cc_emit8(p, 0x50);
        if (!cc_parse_relational(p))
            return false;
        cc_emit_comparison(p, comparison);
    }
}

static bool cc_parse_bitand(struct cc_parser *p)
{
    if (!cc_parse_equality(p))
        return false;
    for (;;) {
        cc_skip(p);
        if (p->pos >= p->length || p->source[p->pos] != '&' ||
            (p->pos + 1 < p->length && p->source[p->pos + 1] == '&'))
            return !p->error;
        p->pos++;
        cc_emit8(p, 0x50);
        if (!cc_parse_equality(p))
            return false;
        cc_emit_right_operand(p);
        cc_emit8(p, 0x48);                     /* and rax, rcx */
        cc_emit8(p, 0x21);
        cc_emit8(p, 0xC8);
    }
}

static bool cc_parse_bitxor(struct cc_parser *p)
{
    if (!cc_parse_bitand(p))
        return false;
    for (;;) {
        cc_skip(p);
        if (p->pos >= p->length || p->source[p->pos] != '^')
            return !p->error;
        p->pos++;
        cc_emit8(p, 0x50);
        if (!cc_parse_bitand(p))
            return false;
        cc_emit_right_operand(p);
        cc_emit8(p, 0x48);                     /* xor rax, rcx */
        cc_emit8(p, 0x31);
        cc_emit8(p, 0xC8);
    }
}

static bool cc_parse_bitor(struct cc_parser *p)
{
    if (!cc_parse_bitxor(p))
        return false;
    for (;;) {
        cc_skip(p);
        if (p->pos >= p->length || p->source[p->pos] != '|' ||
            (p->pos + 1 < p->length && p->source[p->pos + 1] == '|'))
            return !p->error;
        p->pos++;
        cc_emit8(p, 0x50);
        if (!cc_parse_bitxor(p))
            return false;
        cc_emit_right_operand(p);
        cc_emit8(p, 0x48);                     /* or rax, rcx */
        cc_emit8(p, 0x09);
        cc_emit8(p, 0xC8);
    }
}

static void cc_emit_boolean(struct cc_parser *p)
{
    cc_emit_test(p);
    cc_emit8(p, 0xB8);                         /* mov eax, 0（不改 flags） */
    cc_emit32(p, 0);
    cc_emit8(p, 0x0F);                         /* setne al */
    cc_emit8(p, 0x95);
    cc_emit8(p, 0xC0);
}

static bool cc_parse_logical_and(struct cc_parser *p)
{
    if (!cc_parse_bitor(p))
        return false;
    size_t false_chain = 0;
    u32 operators = 0;
    while (cc_consume_operator(p, "&&")) {
        cc_emit_test(p);
        cc_emit_condition_chain(p, 0x84, &false_chain); /* 假：跳过所有 RHS */
        operators++;
        if (!cc_parse_bitor(p))
            return false;
    }
    if (!operators)
        return !p->error;
    cc_emit_boolean(p);
    size_t end_jump = cc_emit_jmp(p);
    size_t false_target = p->code_pos;
    cc_emit_imm(p, 0);
    cc_patch_chain(p, false_chain, false_target);
    cc_patch_jump(p, end_jump, p->code_pos);
    return !p->error;
}

static bool cc_parse_logical_or(struct cc_parser *p)
{
    if (!cc_parse_logical_and(p))
        return false;
    size_t true_chain = 0;
    u32 operators = 0;
    while (cc_consume_operator(p, "||")) {
        cc_emit_test(p);
        cc_emit_condition_chain(p, 0x85, &true_chain); /* 真：跳过所有 RHS */
        operators++;
        if (!cc_parse_logical_and(p))
            return false;
    }
    if (!operators)
        return !p->error;
    cc_emit_boolean(p);
    size_t end_jump = cc_emit_jmp(p);
    size_t true_target = p->code_pos;
    cc_emit_imm(p, 1);
    cc_patch_chain(p, true_chain, true_target);
    cc_patch_jump(p, end_jump, p->code_pos);
    return !p->error;
}

static bool cc_parse_expression(struct cc_parser *p)
{
    if (!cc_parse_logical_or(p))
        return false;
    if (!cc_consume(p, '?'))
        return !p->error;
    cc_emit_test(p);
    size_t false_jump = cc_emit_jz(p);
    if (!cc_parse_expression(p) || !cc_expect(p, ':'))
        return false;
    size_t end_jump = cc_emit_jmp(p);
    cc_patch_jump(p, false_jump, p->code_pos);
    if (!cc_parse_expression(p))
        return false;
    cc_patch_jump(p, end_jump, p->code_pos);
    return !p->error;
}

static void cc_emit_print(struct cc_parser *p, size_t data_offset, size_t length)
{
    if (p->fixup_count >= CC_MAX_FIXUPS || length > 0xFFFFFFFFu) {
        cc_fail(p, "too many print statements");
        return;
    }
    cc_emit8(p, 0xB8);                         /* mov eax, SYS_LOG_WRITE */
    cc_emit32(p, 0x1000);
    cc_emit8(p, 0x48);                         /* lea rdi, [rip+disp32] */
    cc_emit8(p, 0x8D);
    cc_emit8(p, 0x3D);
    struct cc_fixup *fixup = &p->fixups[p->fixup_count++];
    fixup->disp_pos = p->code_pos;
    fixup->data_offset = data_offset;
    cc_emit32(p, 0);
    fixup->next_ip = p->code_pos;
    cc_emit8(p, 0xBE);                         /* mov esi, length */
    cc_emit32(p, (u32)length);
    cc_emit8(p, 0x0F);
    cc_emit8(p, 0x05);
}

static void cc_emit_return(struct cc_parser *p)
{
    if (!p->current_is_main) {
        cc_emit8(p, 0x48);                     /* mov rsp, rbp；丢弃局部帧 */
        cc_emit8(p, 0x89);
        cc_emit8(p, 0xEC);
        cc_emit8(p, 0x5D);                     /* pop rbp */
        cc_emit8(p, 0xC3);                     /* ret；返回值留在 rax */
        return;
    }
    cc_emit8(p, 0x89);                         /* mov edi, eax */
    cc_emit8(p, 0xC7);
    cc_emit_syscall(p, 0x1013);                /* SYS_EXIT */
    cc_emit8(p, 0xF4);                         /* hlt if a broken kernel returns */
}

static bool cc_parse_statement(struct cc_parser *p);

static bool cc_parse_declaration(struct cc_parser *p)
{
    char name[CC_VAR_NAME];
    if (!cc_identifier(p, name)) {
        cc_fail(p, "expected local variable name");
        return false;
    }
    if (cc_consume(p, '=')) {
        if (!cc_parse_expression(p))
            return false;
    } else {
        cc_emit_imm(p, 0);
    }
    if (!cc_expect(p, ';'))
        return false;
    struct cc_var *var = cc_add_var(p, name);
    if (!var)
        return false;
    cc_emit_store_var(p, var);
    return !p->error;
}

static bool cc_parse_assignment(struct cc_parser *p, bool semicolon)
{
    char name[CC_VAR_NAME];
    if (!cc_identifier(p, name)) {
        cc_fail(p, "expected local variable assignment");
        return false;
    }
    struct cc_var *var = cc_find_var(p, name);
    if (!var) {
        cc_fail(p, "unknown local variable");
        return false;
    }
    char op = cc_assignment_operator(p);
    if (!op) {
        cc_fail(p, "expected assignment operator");
        return false;
    }
    if (op != '=') {
        cc_emit_load_var(p, var);
        cc_emit8(p, 0x50);                     /* push 左值旧值 */
    }
    if (!cc_parse_expression(p))
        return false;
    if (op != '=') {
        cc_emit_right_operand(p);              /* rax=旧值，rcx=右表达式 */
        if (op == '*') {
            cc_emit8(p, 0x48);
            cc_emit8(p, 0x0F);
            cc_emit8(p, 0xAF);
            cc_emit8(p, 0xC1);
        } else if (op == '/' || op == '%') {
            cc_emit8(p, 0x48);
            cc_emit8(p, 0x99);
            cc_emit8(p, 0x48);
            cc_emit8(p, 0xF7);
            cc_emit8(p, 0xF9);
            if (op == '%') {
                cc_emit8(p, 0x48);
                cc_emit8(p, 0x89);
                cc_emit8(p, 0xD0);
            }
        } else {
            cc_emit8(p, 0x48);
            cc_emit8(p, op == '+' ? 0x01 : 0x29);
            cc_emit8(p, 0xC8);
        }
    }
    if (semicolon && !cc_expect(p, ';'))
        return false;
    cc_emit_store_var(p, var);
    return !p->error;
}

static bool cc_parse_assignment_or_expression(struct cc_parser *p,
                                               bool semicolon)
{
    cc_skip(p);
    size_t start = p->pos;
    char name[CC_VAR_NAME];
    bool assignment = false;
    if (cc_identifier(p, name))
        assignment = cc_assignment_operator(p) != 0;
    p->pos = start;
    if (assignment)
        return cc_parse_assignment(p, semicolon);
    if (!cc_parse_expression(p))
        return false;
    if (semicolon && !cc_expect(p, ';'))
        return false;
    return !p->error;
}

static bool cc_parse_block(struct cc_parser *p)
{
    if (!cc_expect(p, '{'))
        return false;
    if (++p->depth > CC_MAX_DEPTH) {
        cc_fail(p, "too many nested blocks");
        return false;
    }
    for (;;) {
        cc_skip(p);
        if (p->error)
            return false;
        if (cc_consume(p, '}')) {
            p->depth--;
            return true;
        }
        if (p->pos >= p->length) {
            cc_fail(p, "unterminated block");
            return false;
        }
        if (!cc_parse_statement(p))
            return false;
    }
}

static bool cc_parse_statement(struct cc_parser *p)
{
    cc_skip(p);
    if (p->pos < p->length && p->source[p->pos] == '{')
        return cc_parse_block(p);
    if (cc_consume(p, ';'))
        return true;
    if (cc_word(p, "int"))
        return cc_parse_declaration(p);
    if (cc_word(p, "return")) {
        if (!cc_parse_expression(p) || !cc_expect(p, ';'))
            return false;
        cc_emit_return(p);
        return !p->error;
    }
    bool is_default = cc_word(p, "default");
    if (is_default || cc_word(p, "case")) {
        if (!p->control_depth ||
            p->controls[p->control_depth - 1].kind != CC_CONTROL_SWITCH) {
            cc_fail(p, "case/default outside switch");
            return false;
        }
        struct cc_control *sw = &p->controls[p->control_depth - 1];
        u32 owner = sw->id;
        if (is_default) {
            if (!cc_expect(p, ':'))
                return false;
            if (sw->default_target) {
                cc_fail(p, "duplicate default label");
                return false;
            }
            sw->default_target = p->code_pos;
            return true;
        }

        bool negative = cc_consume(p, '-');
        if (!negative)
            cc_consume(p, '+');
        i64 value;
        if (!cc_parse_number(p, &value) || !cc_expect(p, ':'))
            return false;
        value = negative ? -value : value;
        for (u32 i = 0; i < p->case_count; i++) {
            if (p->cases[i].owner == owner && p->cases[i].value == value) {
                cc_fail(p, "duplicate case value");
                return false;
            }
        }
        if (p->case_count >= CC_MAX_CASES) {
            cc_fail(p, "too many case labels");
            return false;
        }
        struct cc_case *label = &p->cases[p->case_count++];
        label->value = value;
        label->target = p->code_pos;
        label->owner = owner;
        return true;
    }
    if (cc_word(p, "break")) {
        if (!p->control_depth) {
            cc_fail(p, "break outside loop/switch");
            return false;
        }
        if (!cc_expect(p, ';'))
            return false;
        cc_emit_loop_jump(p, &p->controls[p->control_depth - 1].break_chain);
        return !p->error;
    }
    if (cc_word(p, "continue")) {
        struct cc_control *loop = NULL;
        for (u32 i = p->control_depth; i > 0; i--) {
            if (p->controls[i - 1].kind == CC_CONTROL_LOOP) {
                loop = &p->controls[i - 1];
                break;
            }
        }
        if (!loop) {
            cc_fail(p, "continue outside loop");
            return false;
        }
        if (!cc_expect(p, ';'))
            return false;
        cc_emit_loop_jump(p, &loop->continue_chain);
        return !p->error;
    }
    if (cc_word(p, "puts") || cc_word(p, "print")) {
        if (!cc_expect(p, '('))
            return false;
        size_t offset, length;
        if (!cc_parse_string(p, &offset, &length) ||
            !cc_expect(p, ')') || !cc_expect(p, ';'))
            return false;
        cc_emit_print(p, offset, length);
        return !p->error;
    }
    if (cc_word(p, "if")) {
        if (!cc_expect(p, '(') || !cc_parse_expression(p) || !cc_expect(p, ')'))
            return false;
        cc_emit_test(p);
        size_t false_jump = cc_emit_jz(p);
        if (!cc_parse_statement(p))
            return false;
        if (cc_word(p, "else")) {
            size_t end_jump = cc_emit_jmp(p);
            cc_patch_jump(p, false_jump, p->code_pos);
            if (!cc_parse_statement(p))
                return false;
            cc_patch_jump(p, end_jump, p->code_pos);
        } else {
            cc_patch_jump(p, false_jump, p->code_pos);
        }
        return !p->error;
    }
    if (cc_word(p, "switch")) {
        if (!cc_expect(p, '(') || !cc_parse_expression(p) ||
            !cc_expect(p, ')'))
            return false;
        if (p->control_depth >= CC_MAX_DEPTH) {
            cc_fail(p, "too many nested controls");
            return false;
        }
        struct cc_control *sw = &p->controls[p->control_depth++];
        memset(sw, 0, sizeof(*sw));
        sw->kind = CC_CONTROL_SWITCH;
        sw->switch_var.offset = (CC_MAX_VARS + p->control_depth) * 8u;
        cc_emit_store_var(p, &sw->switch_var);

        size_t dispatch_jump = cc_emit_jmp(p);
        u32 owner = p->switch_serial++;
        sw->id = owner;
        if (!cc_parse_statement(p))
            return false;
        size_t fallthrough_jump = cc_emit_jmp(p);
        cc_patch_jump(p, dispatch_jump, p->code_pos);

        for (u32 i = 0; i < p->case_count; i++) {
            struct cc_case *label = &p->cases[i];
            if (label->owner != owner)
                continue;
            cc_emit_imm_rcx(p, label->value);
            cc_emit_load_var(p, &sw->switch_var);
            cc_emit8(p, 0x48);                 /* cmp rax, rcx */
            cc_emit8(p, 0x39);
            cc_emit8(p, 0xC8);
            size_t match_jump = cc_emit_jz(p);
            cc_patch_jump(p, match_jump, label->target);
        }
        size_t no_match_jump = cc_emit_jmp(p);
        if (sw->default_target)
            cc_patch_jump(p, no_match_jump, sw->default_target);

        size_t end = p->code_pos;
        cc_patch_jump(p, fallthrough_jump, end);
        if (!sw->default_target)
            cc_patch_jump(p, no_match_jump, end);
        cc_patch_chain(p, sw->break_chain, end);
        p->control_depth--;
        return !p->error;
    }
    if (cc_word(p, "do")) {
        size_t body = p->code_pos;
        if (p->control_depth >= CC_MAX_DEPTH) {
            cc_fail(p, "too many nested loops");
            return false;
        }
        struct cc_control *loop = &p->controls[p->control_depth++];
        memset(loop, 0, sizeof(*loop));
        loop->kind = CC_CONTROL_LOOP;
        if (!cc_parse_statement(p))
            return false;
        size_t condition = p->code_pos;
        loop->continue_target = condition;
        if (!cc_expect_word(p, "while") || !cc_expect(p, '(') ||
            !cc_parse_expression(p) || !cc_expect(p, ')') ||
            !cc_expect(p, ';'))
            return false;
        cc_emit_test(p);
        size_t back_jump = cc_emit_jnz(p);
        cc_patch_jump(p, back_jump, body);
        cc_patch_chain(p, loop->continue_chain, condition);
        cc_patch_chain(p, loop->break_chain, p->code_pos);
        p->control_depth--;
        return !p->error;
    }
    if (cc_word(p, "while")) {
        size_t condition = p->code_pos;
        if (!cc_expect(p, '(') || !cc_parse_expression(p) || !cc_expect(p, ')'))
            return false;
        cc_emit_test(p);
        size_t exit_jump = cc_emit_jz(p);
        if (p->control_depth >= CC_MAX_DEPTH) {
            cc_fail(p, "too many nested loops");
            return false;
        }
        struct cc_control *loop = &p->controls[p->control_depth++];
        memset(loop, 0, sizeof(*loop));
        loop->kind = CC_CONTROL_LOOP;
        loop->continue_target = condition;
        if (!cc_parse_statement(p))
            return false;
        size_t back_jump = cc_emit_jmp(p);
        cc_patch_jump(p, back_jump, condition);
        cc_patch_chain(p, loop->continue_chain, loop->continue_target);
        cc_patch_jump(p, exit_jump, p->code_pos);
        cc_patch_chain(p, loop->break_chain, p->code_pos);
        p->control_depth--;
        return !p->error;
    }
    if (cc_word(p, "for")) {
        if (!cc_expect(p, '('))
            return false;
        if (!cc_consume(p, ';')) {
            if (cc_word(p, "int")) {
                if (!cc_parse_declaration(p))
                    return false;
            } else if (!cc_parse_assignment_or_expression(p, true)) {
                return false;
            }
        }

        size_t condition = p->code_pos;
        if (cc_consume(p, ';')) {
            cc_emit_imm(p, 1);               /* for (; ;) 的恒真条件 */
        } else if (!cc_parse_expression(p) || !cc_expect(p, ';')) {
            return false;
        }
        cc_emit_test(p);
        size_t exit_jump = cc_emit_jz(p);
        size_t body_jump = cc_emit_jmp(p);

        size_t iteration = p->code_pos;       /* continue 必须先执行迭代表达式 */
        if (!cc_consume(p, ')')) {
            if (!cc_parse_assignment_or_expression(p, false) ||
                !cc_expect(p, ')'))
                return false;
        }
        size_t condition_jump = cc_emit_jmp(p);
        cc_patch_jump(p, condition_jump, condition);
        cc_patch_jump(p, body_jump, p->code_pos);

        if (p->control_depth >= CC_MAX_DEPTH) {
            cc_fail(p, "too many nested loops");
            return false;
        }
        struct cc_control *loop = &p->controls[p->control_depth++];
        memset(loop, 0, sizeof(*loop));
        loop->kind = CC_CONTROL_LOOP;
        loop->continue_target = iteration;
        if (!cc_parse_statement(p))
            return false;
        size_t iteration_jump = cc_emit_jmp(p);
        cc_patch_jump(p, iteration_jump, iteration);
        cc_patch_chain(p, loop->continue_chain, iteration);
        cc_patch_jump(p, exit_jump, p->code_pos);
        cc_patch_chain(p, loop->break_chain, p->code_pos);
        p->control_depth--;
        return !p->error;
    }
    return cc_parse_assignment_or_expression(p, true);
}

static bool cc_parse_function(struct cc_parser *p, bool *saw_main,
                              size_t *main_address)
{
    if (!cc_expect_word(p, "int"))
        return false;
    char name[CC_VAR_NAME];
    if (!cc_identifier(p, name)) {
        cc_fail(p, "expected function name");
        return false;
    }
    if (!cc_expect(p, '('))
        return false;

    char param_names[CC_MAX_PARAMS][CC_VAR_NAME];
    bool param_named[CC_MAX_PARAMS];
    u32 param_count = 0;
    if (cc_word(p, "void")) {
        if (!cc_expect(p, ')'))
            return false;
    } else if (!cc_consume(p, ')')) {
        for (;;) {
            if (param_count >= CC_MAX_PARAMS) {
                cc_fail(p, "too many function parameters");
                return false;
            }
            if (!cc_expect_word(p, "int"))
                return false;
            param_named[param_count] =
                cc_identifier(p, param_names[param_count]);
            if (!param_named[param_count])
                param_names[param_count][0] = '\0'; /* 原型允许 int f(int, int) */
            param_count++;
            if (cc_consume(p, ')'))
                break;
            if (!cc_expect(p, ','))
                return false;
        }
    }

    bool is_main = strcmp(name, "main") == 0;
    if (is_main && param_count) {
        cc_fail(p, "main parameters are not supported");
        return false;
    }
    bool prototype = cc_consume(p, ';');
    if (!prototype) {
        for (u32 i = 0; i < param_count; i++) {
            if (!param_named[i]) {
                cc_fail(p, "parameter name required in function definition");
                return false;
            }
        }
    }
    struct cc_function *fn =
        cc_declare_function(p, name, param_count, !prototype);
    if (!fn)
        return false;
    if (prototype)
        return !p->error;

    p->var_count = 0;
    p->depth = 0;
    p->control_depth = 0;
    p->current_is_main = is_main;
    cc_emit_prologue(p);

    for (u32 i = 0; i < param_count; i++) {
        struct cc_var *var = cc_add_var(p, param_names[i]);
        if (!var)
            return false;
        var->parameter = true;
        var->offset = 16u + (param_count - 1u - i) * 8u;
    }
    if (!cc_parse_block(p))
        return false;
    cc_emit_imm(p, 0);                        /* int 函数 fall-through 返回 0 */
    cc_emit_return(p);
    if (is_main) {
        *saw_main = true;
        *main_address = fn->address;
    }
    return !p->error;
}

static bool cc_parse_program(struct cc_parser *p)
{
    size_t entry_jump = cc_emit_jmp(p);       /* ELF 入口跳到 main，而非首个 helper */
    bool saw_main = false;
    size_t main_address = 0;
    for (;;) {
        cc_skip(p);
        if (p->pos == p->length)
            break;
        if (!cc_parse_function(p, &saw_main, &main_address))
            return false;
    }
    if (!saw_main) {
        cc_fail(p, "missing main function");
        return false;
    }
    if (!cc_resolve_calls(p))
        return false;
    cc_patch_jump(p, entry_jump, main_address);
    return !p->error;
}

static void cc_make_elf(struct cc_parser *p, size_t image_size)
{
    struct elf64_ehdr *eh = (struct elf64_ehdr *)p->image;
    struct elf64_phdr *ph = (struct elf64_phdr *)(p->image + sizeof(*eh));
    memset(eh, 0, sizeof(*eh));
    eh->e_ident[0] = 0x7F;
    eh->e_ident[1] = 'E'; eh->e_ident[2] = 'L'; eh->e_ident[3] = 'F';
    eh->e_ident[4] = ELFCLASS64;
    eh->e_ident[5] = ELFDATA2LSB;
    eh->e_ident[6] = 1;
    eh->e_type = ET_EXEC;
    eh->e_machine = EM_X86_64;
    eh->e_version = 1;
    eh->e_entry = CC_BASE + CC_CODE_OFFSET;
    eh->e_phoff = sizeof(*eh);
    eh->e_ehsize = sizeof(*eh);
    eh->e_phentsize = sizeof(*ph);
    eh->e_phnum = 1;
    eh->e_shentsize = sizeof(struct elf64_shdr);
    memset(ph, 0, sizeof(*ph));
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_X;
    ph->p_offset = 0;
    ph->p_vaddr = CC_BASE;
    ph->p_paddr = CC_BASE;
    ph->p_filesz = image_size;
    ph->p_memsz = image_size;
    ph->p_align = PAGE_SIZE;
}

int fyxcc_compile(const char *source, size_t source_len,
                  void **out_image, size_t *out_size,
                  struct fyxcc_diagnostic *diagnostic)
{
    if (!out_image || !out_size || !source || !source_len)
        return -FYX_EINVAL;
    *out_image = NULL;
    *out_size = 0;
    if (diagnostic)
        memset(diagnostic, 0, sizeof(*diagnostic));
    if (source_len > CC_IMAGE_CAP)
        return -FYX_ERANGE;

    /* 解析器包含控制/函数/标签表；堆分配避免占用 16 KiB 内核线程栈的大半。 */
    struct cc_parser *p = kzalloc(sizeof(*p));
    if (!p)
        return -FYX_ENOMEM;
    p->source = source;
    p->length = source_len;
    p->code_pos = CC_CODE_OFFSET;
    p->image = kzalloc(CC_IMAGE_CAP);
    p->data = kmalloc(CC_DATA_CAP);
    p->diagnostic = diagnostic;
    if (!p->image || !p->data) {
        kfree(p->image);
        kfree(p->data);
        kfree(p);
        return -FYX_ENOMEM;
    }

    if (cc_parse_program(p) && !p->error) {
        size_t data_start = p->code_pos;
        if (data_start + p->data_len > CC_IMAGE_CAP) {
            cc_fail(p, "ELF image is too large");
        } else {
            memcpy(p->image + data_start, p->data, p->data_len);
            size_t image_size = data_start + p->data_len;
            for (size_t i = 0; i < p->fixup_count; i++) {
                struct cc_fixup *f = &p->fixups[i];
                i64 target = (i64)(CC_BASE + data_start + f->data_offset);
                i64 next = (i64)(CC_BASE + f->next_ip);
                i64 displacement = target - next;
                if (displacement < -0x80000000ll || displacement > 0x7FFFFFFFll) {
                    cc_fail(p, "RIP displacement out of range");
                    break;
                }
                cc_patch32(p, f->disp_pos, (u32)(i32)displacement);
            }
            if (!p->error) {
                cc_make_elf(p, image_size);
                *out_image = p->image;
                *out_size = image_size;
                kfree(p->data);
                kfree(p);
                return FYX_OK;
            }
        }
    }
    int error = p->error ? p->error : -FYX_EINVAL;
    kfree(p->image);
    kfree(p->data);
    kfree(p);
    return error;
}

int fyxcc_compile_file(const char *source_path, const char *output_path,
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
    rc = fyxcc_compile(source, source_size, &image, &image_size, diagnostic);
    kfree(source);
    if (rc != 0)
        return rc;
    rc = vfs_write_file(output_path, image, image_size);
    kfree(image);
    return rc;
}
