/*
 * FYXOS — PS/2 键盘驱动（中断驱动 + 环形缓冲）
 * ---------------------------------------------------------------------------
 * 使用扫描码集 1（Set 1），US 布局。按下/松开事件都会进 IRQ1，
 * 只有“按下”且能映射成字符时才入缓冲。
 */
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/sync.h>

#define KB_DATA_PORT   0x60
#define KB_STATUS_PORT 0x64

#define SC_EXTENDED  0xE0
#define SC_RELEASE   0x80

/* 扫描码集 1 -> ASCII（未按下 Shift / 未开启 CapsLock） */
static const char keymap_lower[128] = {
      0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p','[',']','\n',
      0, /* LCtrl */
    'a','s','d','f','g','h','j','k','l',';','\'', '`',
      0, /* LShift */ '\\',
    'z','x','c','v','b','n','m',',','.','/',
      0, /* RShift */ '*',
      0, /* LAlt */ ' ',
      0, /* CapsLock */
      0,0,0,0,0,0,0,0,0,0, /* F1..F10 */
      0, /* NumLock */ 0, /* ScrollLock */
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char keymap_upper[128] = {
      0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t', 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
      0,
    'A','S','D','F','G','H','J','K','L',':','"', '~',
      0, '|',
    'Z','X','C','V','B','N','M','<','>','?',
      0, '*',
      0, ' ',
      0,
      0,0,0,0,0,0,0,0,0,0,
      0, 0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

struct keybuf {
    char buf[KEYBUF_SIZE];
    volatile u32 head;
    volatile u32 tail;
    volatile u32 count;
    volatile u32 dropped;
};

static struct keybuf g_keybuf;
static volatile u64 g_irq_count;
static volatile u64 g_scancode_count;
static bool g_shift;
static bool g_capslock;
static bool g_extended;
static bool g_ready;

static void keybuf_push(char c)
{
    if (g_keybuf.count >= KEYBUF_SIZE) {
        g_keybuf.dropped++;
        return;
    }
    g_keybuf.buf[g_keybuf.head] = c;
    g_keybuf.head = (g_keybuf.head + 1) % KEYBUF_SIZE;
    g_keybuf.count++;
}

static int keybuf_pop(char *out)
{
    if (g_keybuf.count == 0)
        return 0;
    *out = g_keybuf.buf[g_keybuf.tail];
    g_keybuf.tail = (g_keybuf.tail + 1) % KEYBUF_SIZE;
    g_keybuf.count--;
    return 1;
}

static void handle_scancode(u8 sc)
{
    if (sc == SC_EXTENDED) {
        g_extended = true;
        return;
    }

    bool released = !!(sc & SC_RELEASE);
    u8 code = sc & ~SC_RELEASE;

    if (released) {
        if (code == 0x2A || code == 0x36)      /* Shift */
            g_shift = false;
        g_extended = false;
        return;
    }

    switch (code) {
    case 0x2A:                                  /* LShift */
    case 0x36:                                  /* RShift */
        g_shift = true;
        return;
    case 0x3A:                                  /* CapsLock */
        g_capslock = !g_capslock;
        return;
    default:
        break;
    }

    if (g_extended) {
        g_extended = false;
        return;                                 /* 方向键等暂不处理 */
    }

    if (code >= 128)
        return;

    bool upper = g_shift ^ g_capslock;
    char c = upper ? keymap_upper[code] : keymap_lower[code];
    if (c)
        keybuf_push(c);
}

static void keyboard_handler(void *arg)
{
    UNUSED(arg);
    u8 sc = inb(KB_DATA_PORT);
    g_irq_count++;
    g_scancode_count++;
    handle_scancode(sc);
    irq_eoi(IRQ_KEYBOARD);
}

void keyboard_init(void)
{
    memset(&g_keybuf, 0, sizeof(g_keybuf));
    g_shift = false;
    g_capslock = false;
    g_extended = false;

    if (irq_register(IRQ_KEYBOARD, keyboard_handler, "ps2-keyboard", NULL) == 0) {
        pic_unmask(IRQ_KEYBOARD);
        g_ready = true;
    }
}

int keyboard_has_char(void)
{
    return g_keybuf.count != 0;
}

int keyboard_poll(char *out)
{
    if (!out)
        return 0;
    u64 flags = irq_save_disable();
    int r = keybuf_pop(out);
    irq_restore(flags);
    return r;
}

char keyboard_getchar(void)
{
    char c = 0;
    while (!keyboard_poll(&c)) {
        /* 等待中断把字符送进来；中断关闭时也会轮询一次数据端口兜底 */
        if (inb(KB_STATUS_PORT) & 0x01)
            handle_scancode(inb(KB_DATA_PORT));
        cpu_pause();
    }
    return c;
}

u64 keyboard_irq_count(void)
{
    return g_irq_count;
}
