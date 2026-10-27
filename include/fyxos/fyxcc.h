/* FYXOS — 内置 C 子集原生编译器 */
#ifndef FYXOS_FYXCC_H
#define FYXOS_FYXCC_H

#include <fyxos/types.h>

struct fyxcc_diagnostic {
    size_t offset;
    char message[96];
};

/* 输出为调用者拥有的、可直接写入 FYXFS 的静态 x86_64 ELF64。 */
int fyxcc_compile(const char *source, size_t source_len,
                  void **out_image, size_t *out_size,
                  struct fyxcc_diagnostic *diagnostic);
int fyxcc_compile_file(const char *source_path, const char *output_path,
                       struct fyxcc_diagnostic *diagnostic);

#endif /* FYXOS_FYXCC_H */
