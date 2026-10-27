/* FYXOS — 自包含 RFC 1951/1952 解码器；不依赖宿主 zlib 或既有内核代码 */
#include <fyxos/gzip.h>
#include <fyxos/fyxfs.h>
#include <fyxos/mm.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>

struct bit_reader {
    const u8 *data;
    size_t size, pos;
    u64 hold;
    u32 bits;
    int error;
};

struct huffman {
    u16 count[16];
    u16 symbol[320];
    u16 symbols;
};

static u16 get16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 get32(const u8 *p)
{
    return (u32)get16(p) | ((u32)get16(p + 2) << 16);
}

static u32 bits_read(struct bit_reader *b, u32 need)
{
    while (b->bits < need) {
        if (b->pos >= b->size) {
            b->error = 1;
            return 0;
        }
        b->hold |= (u64)b->data[b->pos++] << b->bits;
        b->bits += 8;
    }
    u32 value = (u32)(b->hold & (((u64)1 << need) - 1));
    b->hold >>= need;
    b->bits -= need;
    return value;
}

static void bits_align(struct bit_reader *b)
{
    u32 drop = b->bits & 7;
    b->hold >>= drop;
    b->bits -= drop;
}

/* 建立 canonical Huffman 解码表；拒绝过订阅，缺失码在 decode 中报错。 */
static int huffman_build(struct huffman *h, const u8 *lengths, u32 n)
{
    if (n > 288)
        return -FYX_EINVAL;
    memset(h, 0, sizeof(*h));
    for (u32 i = 0; i < n; i++) {
        if (lengths[i] > 15)
            return -FYX_EINVAL;
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;
    h->symbols = n;
    int left = 1;
    for (u32 len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -FYX_EINVAL;
    }
    u16 offsets[16];
    offsets[1] = 0;
    for (u32 len = 1; len < 15; len++)
        offsets[len + 1] = offsets[len] + h->count[len];
    for (u32 sym = 0; sym < n; sym++)
        if (lengths[sym])
            h->symbol[offsets[lengths[sym]]++] = (u16)sym;
    return 0;
}

static int huffman_decode(struct bit_reader *b, const struct huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (u32 len = 1; len <= 15; len++) {
        code |= (int)bits_read(b, 1);
        if (b->error)
            return -1;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_codes(struct bit_reader *b, u8 *out, size_t cap,
                         size_t *written, const struct huffman *lit,
                         const struct huffman *dist)
{
    static const u16 length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
        67,83,99,115,131,163,195,227,258
    };
    static const u8 length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const u16 dist_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,
        513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const u8 dist_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
        9,9,10,10,11,11,12,12,13,13
    };
    size_t pos = *written;
    for (;;) {
        int sym = huffman_decode(b, lit);
        if (sym < 0)
            return -FYX_EINVAL;
        if (sym < 256) {
            if (pos >= cap)
                return -FYX_EINVAL;
            out[pos++] = (u8)sym;
            continue;
        }
        if (sym == 256) {
            *written = pos;
            return 0;
        }
        if (sym < 257 || sym > 285)
            return -FYX_EINVAL;
        u32 li = (u32)sym - 257;
        u32 length = length_base[li] + bits_read(b, length_extra[li]);
        int ds = huffman_decode(b, dist);
        if (b->error || ds < 0 || ds > 29)
            return -FYX_EINVAL;
        u32 distance = dist_base[ds] + bits_read(b, dist_extra[ds]);
        if (b->error || distance > pos || length > cap - pos)
            return -FYX_EINVAL;
        for (u32 i = 0; i < length; i++) {
            out[pos] = out[pos - distance];
            pos++;
        }
    }
}

static int fixed_tables(struct huffman *lit, struct huffman *dist)
{
    u8 lengths[288];
    for (u32 i = 0; i <= 143; i++) lengths[i] = 8;
    for (u32 i = 144; i <= 255; i++) lengths[i] = 9;
    for (u32 i = 256; i <= 279; i++) lengths[i] = 7;
    for (u32 i = 280; i <= 287; i++) lengths[i] = 8;
    int rc = huffman_build(lit, lengths, 288);
    if (rc)
        return rc;
    memset(lengths, 5, 32);
    return huffman_build(dist, lengths, 32);
}

static int dynamic_tables(struct bit_reader *b, struct huffman *lit,
                          struct huffman *dist)
{
    static const u8 order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    u32 nl = bits_read(b, 5) + 257;
    u32 nd = bits_read(b, 5) + 1;
    u32 nc = bits_read(b, 4) + 4;
    if (b->error || nl > 286 || nd > 32)
        return -FYX_EINVAL;
    u8 code_lengths[19] = { 0 };
    for (u32 i = 0; i < nc; i++)
        code_lengths[order[i]] = (u8)bits_read(b, 3);
    struct huffman code;
    int rc = huffman_build(&code, code_lengths, 19);
    if (rc)
        return rc;
    u8 lengths[320] = { 0 };
    u32 total = nl + nd, at = 0;
    while (at < total) {
        int sym = huffman_decode(b, &code);
        if (sym < 0)
            return -FYX_EINVAL;
        if (sym <= 15) {
            lengths[at++] = (u8)sym;
            continue;
        }
        u32 repeat;
        u8 value = 0;
        if (sym == 16) {
            if (!at)
                return -FYX_EINVAL;
            value = lengths[at - 1];
            repeat = bits_read(b, 2) + 3;
        } else if (sym == 17) {
            repeat = bits_read(b, 3) + 3;
        } else if (sym == 18) {
            repeat = bits_read(b, 7) + 11;
        } else {
            return -FYX_EINVAL;
        }
        if (b->error || repeat > total - at)
            return -FYX_EINVAL;
        while (repeat--)
            lengths[at++] = value;
    }
    if (!lengths[256])
        return -FYX_EINVAL;
    rc = huffman_build(lit, lengths, nl);
    if (rc)
        return rc;
    return huffman_build(dist, lengths + nl, nd);
}

static int inflate_deflate(struct bit_reader *b, u8 *out, size_t cap,
                           size_t *written)
{
    size_t pos = 0;
    int final = 0;
    while (!final) {
        final = (int)bits_read(b, 1);
        u32 type = bits_read(b, 2);
        if (b->error || type == 3)
            return -FYX_EINVAL;
        if (type == 0) {
            bits_align(b);
            u32 len = bits_read(b, 16);
            u32 inv = bits_read(b, 16);
            if (b->error || (len ^ 0xffffu) != inv || len > cap - pos)
                return -FYX_EINVAL;
            for (u32 i = 0; i < len; i++) {
                if (b->error)
                    return -FYX_EINVAL;
                out[pos++] = (u8)bits_read(b, 8);
            }
            continue;
        }
        struct huffman lit, dist;
        int rc = type == 1 ? fixed_tables(&lit, &dist) :
                             dynamic_tables(b, &lit, &dist);
        if (rc)
            return rc;
        rc = inflate_codes(b, out, cap, &pos, &lit, &dist);
        if (rc)
            return rc;
    }
    bits_align(b);
    size_t used = b->pos - b->bits / 8;
    if (used != b->size)
        return -FYX_EINVAL; /* 拒绝截断、拼接 member 或尾部垃圾 */
    *written = pos;
    return 0;
}

int gzip_decompress(const void *input, size_t input_size,
                    void **output, size_t *output_size, size_t max_output)
{
    const u8 *in = input;
    if (!in || !output || !output_size || input_size < 18 ||
        in[0] != 0x1f || in[1] != 0x8b || in[2] != 8 || (in[3] & 0xe0))
        return -FYX_EINVAL;
    size_t deflate_end = input_size - 8, pos = 10;
    if (pos > deflate_end)
        return -FYX_EINVAL;
    u8 flags = in[3];
    if (flags & 4) {
        if (pos + 2 > deflate_end)
            return -FYX_EINVAL;
        u32 extra = get16(in + pos);
        pos += 2;
        if (extra > deflate_end - pos)
            return -FYX_EINVAL;
        pos += extra;
    }
    for (u32 mask = 8; mask <= 16; mask <<= 1) {
        if (!(flags & mask))
            continue;
        while (pos < deflate_end && in[pos])
            pos++;
        if (pos >= deflate_end)
            return -FYX_EINVAL;
        pos++;
    }
    if (flags & 2) {
        if (pos + 2 > deflate_end)
            return -FYX_EINVAL;
        if ((u16)fyx_crc32(in, pos) != get16(in + pos))
            return -FYX_EIO;
        pos += 2;
    }
    size_t expected = get32(in + input_size - 4);
    if (expected > max_output)
        return -FYX_ENOMEM;
    u8 *out = kmalloc(expected ? expected : 1);
    if (!out)
        return -FYX_ENOMEM;
    struct bit_reader bits = {
        .data = in, .size = deflate_end, .pos = pos
    };
    size_t written = 0;
    int rc = inflate_deflate(&bits, out, expected, &written);
    if (!rc && (written != expected || get32(in + input_size - 4) != written))
        rc = -FYX_EINVAL;
    if (!rc && fyx_crc32(out, written) != get32(in + input_size - 8))
        rc = -FYX_EIO;
    if (rc) {
        kfree(out);
        return rc;
    }
    *output = out;
    *output_size = written;
    return 0;
}
