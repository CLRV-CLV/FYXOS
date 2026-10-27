/* FYXOS — FyxJS 子集原生编译器（共用 fyxcc x86_64 后端） */
#ifndef FYXOS_FYXJS_H
#define FYXOS_FYXJS_H

#include <fyxos/fyxcc.h>

int fyxjs_compile(const char *source, size_t source_len,
                  void **out_image, size_t *out_size,
                  struct fyxcc_diagnostic *diagnostic);
int fyxjs_compile_file(const char *source_path, const char *output_path,
                       struct fyxcc_diagnostic *diagnostic);

#endif /* FYXOS_FYXJS_H */
