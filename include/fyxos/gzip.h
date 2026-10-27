/* FYXOS — gzip/DEFLATE 解码（严格边界、CRC32 与 ISIZE 校验） */
#ifndef FYXOS_GZIP_H
#define FYXOS_GZIP_H

#include <fyxos/types.h>

int gzip_decompress(const void *input, size_t input_size,
                    void **output, size_t *output_size, size_t max_output);

#endif /* FYXOS_GZIP_H */
