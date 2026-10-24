/* FYXOS — 小型 C 子集到 x86_64 静态 ELF 的原生编译器
 *
 * 支持：int main、局部整数变量/赋值、四则运算、比较、puts/print 字符串、
 * return、if/else、while 与嵌套块。生成代码直接使用 FYXOS SYSCALL ABI，
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
#define CC_FRAME_SIZE  (CC_MAX_VARS * 8u)
#define CC_MAX_DEPTH   32u
#define CC_BASE        0x400000ull
#define CC_CODE_OFFSET 128u                 /* EHDR + PHDR，按 8 字节对齐 */

struct cc_fixup {
    size_t disp_pos;
    size_t next_ip;
    size_t data_offset;
};

struct cc_var {
    char name[CC_VAR_NAME];
    u32 offset;                             /* rbp - offset */
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
    u32 depth;
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

static void cc_emit_load_var(struct cc_parser *p, const struct cc_var *var)
{
    cc_emit8(p, 0x48);                         /* mov rax, [rbp-disp32] */
    cc_emit8(p, 0x8B);
    cc_emit8(p, 0x85);
    cc_emit32(p, (u32)(-(i32)var->offset));
}

static void cc_emit_store_var(struct cc_parser *p, const struct cc_var *var)
{
    cc_emit8(p, 0x48);                         /* mov [rbp-disp32], rax */
    cc_emit8(p, 0x89);
    cc_emit8(p, 0x85);
    cc_emit32(p, (u32)(-(i32)var->offset));
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

static size_t cc_emit_jmp(struct cc_parser *p)
{
    cc_emit8(p, 0xE9);                         /* jmp rel32 */
    size_t pos = p->code_pos;
    cc_emit32(p, 0);
    return pos;
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
        struct cc_var *var = cc_find_var(p, name);
        if (!var) {
            cc_fail(p, "unknown local variable");
            return false;
        }
        cc_emit_load_var(p, var);
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
            (p->source[p->pos] != '*' && p->source[p->pos] != '/'))
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

static u8 cc_comparison(struct cc_parser *p)
{
    cc_skip(p);
    if (p->pos + 1 < p->length) {
        char a = p->source[p->pos], b = p->source[p->pos + 1];
        u8 op = 0;
        if (a == '=' && b == '=') op = 0x94;   /* sete */
        else if (a == '!' && b == '=') op = 0x95; /* setne */
        else if (a == '<' && b == '=') op = 0x9E; /* setle */
        else if (a == '>' && b == '=') op = 0x9D; /* setge */
        if (op) {
            p->pos += 2;
            return op;
        }
    }
    if (p->pos < p->length && p->source[p->pos] == '<') {
        p->pos++;
        return 0x9C;                            /* setl */
    }
    if (p->pos < p->length && p->source[p->pos] == '>') {
        p->pos++;
        return 0x9F;                            /* setg */
    }
    return 0;
}

static bool cc_parse_expression(struct cc_parser *p)
{
    if (!cc_parse_add(p))
        return false;
    for (;;) {
        u8 comparison = cc_comparison(p);
        if (!comparison)
            return !p->error;
        cc_emit8(p, 0x50);
        if (!cc_parse_add(p))
            return false;
        cc_emit_right_operand(p);
        cc_emit8(p, 0x48);                     /* cmp rax, rcx */
        cc_emit8(p, 0x39);
        cc_emit8(p, 0xC8);
        cc_emit8(p, 0xB8);                     /* mov eax, 0（不改 flags） */
        cc_emit32(p, 0);
        cc_emit8(p, 0x0F);
        cc_emit8(p, comparison);
        cc_emit8(p, 0xC0);
    }
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
    cc_emit8(p, 0x89);                         /* mov edi, eax */
    cc_emit8(p, 0xC7);
    cc_emit_syscall(p, 0x1013);                /* SYS_EXIT */
    cc_emit8(p, 0xF4);                         /* hlt if a broken kernel returns */
}

static bool cc_parse_statement(struct cc_parser *p);

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
    if (cc_word(p, "int")) {
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
    if (cc_word(p, "return")) {
        if (!cc_parse_expression(p) || !cc_expect(p, ';'))
            return false;
        cc_emit_return(p);
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
    if (cc_word(p, "while")) {
        size_t condition = p->code_pos;
        if (!cc_expect(p, '(') || !cc_parse_expression(p) || !cc_expect(p, ')'))
            return false;
        cc_emit_test(p);
        size_t exit_jump = cc_emit_jz(p);
        if (!cc_parse_statement(p))
            return false;
        size_t back_jump = cc_emit_jmp(p);
        cc_patch_jump(p, back_jump, condition);
        cc_patch_jump(p, exit_jump, p->code_pos);
        return !p->error;
    }
    char name[CC_VAR_NAME];
    if (cc_identifier(p, name)) {
        struct cc_var *var = cc_find_var(p, name);
        if (!var) {
            cc_fail(p, "unknown local variable");
            return false;
        }
        if (!cc_expect(p, '=') || !cc_parse_expression(p) || !cc_expect(p, ';'))
            return false;
        cc_emit_store_var(p, var);
        return !p->error;
    }
    cc_fail(p, "expected declaration/if/while/puts/return/assignment");
    return false;
}

static bool cc_parse_program(struct cc_parser *p)
{
    if (!cc_expect_word(p, "int") || !cc_expect_word(p, "main") ||
        !cc_expect(p, '('))
        return false;
    cc_word(p, "void");                       /* 可选的 void 参数 */
    if (!cc_expect(p, ')'))
        return false;
    cc_emit_prologue(p);
    if (!cc_parse_block(p))
        return false;
    cc_skip(p);
    if (p->pos != p->length) {
        cc_fail(p, "trailing tokens after main");
        return false;
    }
    /* 所有控制流路径都有合法的 fall-through return 0；显式 return 不会到达这里。 */
    cc_emit_imm(p, 0);
    cc_emit_return(p);
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

    struct cc_parser p;
    memset(&p, 0, sizeof(p));
    p.source = source;
    p.length = source_len;
    p.code_pos = CC_CODE_OFFSET;
    p.image = kzalloc(CC_IMAGE_CAP);
    p.data = kmalloc(CC_DATA_CAP);
    p.diagnostic = diagnostic;
    if (!p.image || !p.data) {
        kfree(p.image);
        kfree(p.data);
        return -FYX_ENOMEM;
    }

    if (cc_parse_program(&p) && !p.error) {
        size_t data_start = p.code_pos;
        if (data_start + p.data_len > CC_IMAGE_CAP) {
            cc_fail(&p, "ELF image is too large");
        } else {
            memcpy(p.image + data_start, p.data, p.data_len);
            size_t image_size = data_start + p.data_len;
            for (size_t i = 0; i < p.fixup_count; i++) {
                struct cc_fixup *f = &p.fixups[i];
                i64 target = (i64)(CC_BASE + data_start + f->data_offset);
                i64 next = (i64)(CC_BASE + f->next_ip);
                i64 displacement = target - next;
                if (displacement < -0x80000000ll || displacement > 0x7FFFFFFFll) {
                    cc_fail(&p, "RIP displacement out of range");
                    break;
                }
                cc_patch32(&p, f->disp_pos, (u32)(i32)displacement);
            }
            if (!p.error) {
                cc_make_elf(&p, image_size);
                *out_image = p.image;
                *out_size = image_size;
                kfree(p.data);
                return FYX_OK;
            }
        }
    }
    int error = p.error ? p.error : -FYX_EINVAL;
    kfree(p.image);
    kfree(p.data);
    return error;
}

int fyxcc_compile_file(const char *source_path, const char *output_path,
                       struct fyxcc_diagnostic *diagnostic)
{
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
