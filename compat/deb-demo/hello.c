#include <stdio.h>

/* 未修改的 Linux/glibc x86_64 静态测试载荷；不是 FYXOS 原生 ABI 程序。 */
int main(void)
{
    puts("LINUX_GLIBC_OK");
    puts("DEB_PACKAGE_OK");
    return 23;
}
