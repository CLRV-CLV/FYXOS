# FYXOS GCC C11 SDK

这是 FYXOS Developer Preview 的配套 **宿主端交叉开发 SDK**。语言编译器直接使用
开源 GCC；`bin/fyx-gcc` 只负责选择 FYXOS freestanding ABI、CRT、链接脚本和
最小运行库。GCC 当前在具有 x86_64 ELF GCC 的 Linux 等宿主上运行，**不是已经在
FYXOS guest 内运行的 GCC**。系统内原有的 fyxcc/FyxJS 仍是受限原生编译器。

## 构建程序

```sh
export PATH="$PWD/bin:$PATH"
fyx-gcc --version
fyx-gcc examples/c11-demo.c -o c11-demo.elf
fyx-gcc examples/hello.c -o hello.elf
```

也可编译多个 C 文件，或先用 `-c` 生成目标文件再链接：

```sh
fyx-gcc -c app.c -o app.o
fyx-gcc app.o -o app.elf
```

在 FYXOS 源码树根目录，可把生成的程序预装进一张新镜像（该步骤需要内核源码，
不是离线修改已有 FYXFS 镜像）：

```sh
make app-image APP=build/my-demo.elf
# 启动这张 build/fyxos.img 后，在 guest 中运行：
run /bin/gcc-c11-demo
```

这里会用自定义 ELF 替换生成镜像中的示例；`make clean && make image` 可恢复默认
C11 示例。当前尚未提供 guest 内 GCC 或独立的镜像文件注入器。

输出为固定基址 `0x400000`、非 PIE、静态 x86_64 ELF，入口采用 SysV AMD64
`main(argc, argv, envp)`；main 返回值就是进程退出码。链接器会按页分离
TEXT/RODATA/DATA/BSS，保持 FYXOS 的 W^X 装载约束。

## 当前目标能力

- 开源 GCC 的完整 C11 前端：指针、数组、结构体、枚举、函数指针、typedef、
  `_Static_assert` 等均可使用；示例覆盖了这些自研 fyxcc 尚不具备的能力。
- `include/fyx.h`：FYXOS syscall、文件、进程和 socket ABI。
- `include/stdio.h`：`puts`、`putchar` 控制台输出。
- `include/string.h`：`strlen`、`strcmp`、`strncmp`、`memcpy`、`memmove`、
  `memset`、`memcmp`。
- 自动链接 CRT、`libfyx.a` 和 GCC 自带的 `libgcc`。

这是 freestanding 目标，不是 Linux/glibc 兼容层：暂时没有 hosted libc、动态
链接、线程本地存储、浮点/SSE 用户上下文、异常或 POSIX 完整 API。不要启用
SSE/AVX，也不要把 Linux ELF 直接当作 FYXOS 程序。可用 `FYX_HOST_CC` 指定另一
个 GCC 可执行文件。
