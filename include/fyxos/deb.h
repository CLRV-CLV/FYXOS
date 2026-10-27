/* FYXOS — 标准未压缩 amd64 DEB 安装纵切片 */
#ifndef FYXOS_DEB_H
#define FYXOS_DEB_H

#include <fyxos/types.h>

int deb_install(const char *package_path);
int deb_install_image(const void *image, size_t size, const char *source_name);

#endif /* FYXOS_DEB_H */
