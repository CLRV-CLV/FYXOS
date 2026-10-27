/* Host test driver: links the exact kernel/lib/gzip.c, with libc allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#undef offsetof
#include <fyxos/gzip.h>

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

uint32_t fyx_crc32(const void *data, size_t n)
{
    const unsigned char *p = data;
    uint32_t crc = ~0u;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xedb88320u & -(int)(crc & 1));
    }
    return ~crc;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END) || ftell(f) < 0)
        return 2;
    long size = ftell(f);
    rewind(f);
    unsigned char *input = malloc((size_t)size);
    unsigned char *expected = malloc(16 * 1024 * 1024);
    if (!input || !expected || fread(input, 1, (size_t)size, f) != (size_t)size)
        return 2;
    fclose(f);
    f = fopen(argv[2], "rb");
    if (!f)
        return 2;
    size_t expected_size = fread(expected, 1, 16 * 1024 * 1024, f);
    fclose(f);
    void *output = NULL;
    size_t output_size = 0;
    size_t limit = (size_t)strtoull(argv[3], NULL, 0);
    int rc = gzip_decompress(input, (size_t)size, &output, &output_size, limit);
    int ok = rc == 0 && output_size == expected_size &&
             memcmp(output, expected, output_size) == 0;
    if (!ok)
        fprintf(stderr, "gzip mismatch: rc=%d output=%zu expected=%zu\n",
                rc, output_size, expected_size);
    free(output);
    free(input);
    free(expected);
    return ok ? 0 : 1;
}
