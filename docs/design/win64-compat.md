# Win64 PE32+ 与最小 Win32 导入纵切片

## 已完成的真实路径

当前分支可以在 FYXOS guest 内执行一个严格限定的 Windows AMD64 程序：

```text
标准 DOS + PE32+ EXE
  → 校验 AMD64/可选头/节表/RVA/文件边界/入口
  → 映射 ImageBase 头页（只读、NX）与各节
  → 解析 Import Directory、Import Lookup Table、IAT
  → kernel32.dll!GetStdHandle / WriteFile / ExitProcess
  → 用户态导入服务桩 → FYXOS Win32 独立分表
  → WIN64_PE_OK / WIN32_IMPORT_OK
  → ExitProcess(24)，Shell 报告退出码 24
```

Shell 复现：

```text
run win64 /bin/win64-demo.exe
```

这不是把 PE 中的指令改成 Linux syscall，也不是由宿主代替执行。测试程序通过
真实 IAT 间接调用，使用 Microsoft x64 的 RCX/RDX/R8/R9、栈参数和 shadow
space 约定；进程在 ring3 运行，导入目标不是内核地址。

## 构建与装载实现

- `compat/win64-demo/hello.S` 使用 GNU as，在单一 `.text` 节内解析本地 RIP
  相对引用，并显式放置标准导入描述符、lookup/address thunk 和 hint/name。
- `tools/mkpe64.py` 写出 DOS、COFF、PE32+ 可选头和节表；产物可由 `objdump`
  读取，机器类型为 AMD64、入口为 `0x401000`、ImageBase 为 `0x400000`。
  当前环境不需要 MinGW，也不再使用会产生错误跨节重定位的 ELF→COFF→PE
  探针路径。
- `kernel/sys/pe_load.c` 最多接受 16 个节和 64 MiB `SizeOfImage`，要求固定
  首选 ImageBase、页对齐节和文件对齐原始数据。入口必须落在可执行节。
- PE 头页以只读/NX 映射；测试程序实际从 ImageBase 读取 MZ/PE 签名。可写
  可执行节仅在 IAT 修补期间临时可写，随后降为 RX；数据节保持 NX。
- 用户栈为 64 KiB。`process_run_wait_abi()` 使用独立 `PROC_ABI_WIN64`，与
  FYX 原生和 Linux syscall 编号完全分表。

## Win32 服务边界

`kernel/sys/pe_load.c` 的用户态服务桩位于独立 RX 页，保存需要保留的寄存器，
把 Win64 参数（包括第 5/6 个栈参数）转换为内部服务号。`kernel/sys/win32.c`
目前只实现：

| API | 当前语义 |
|---|---|
| `GetStdHandle` | 标准输出/错误的 FYXOS 控制台伪句柄 |
| `WriteFile` | 上述控制台句柄的同步写出，并返回实际字节数 |
| `ExitProcess` | 终止当前进程并保留真实退出码 |

未知 DLL、ordinal 或 API 会明确返回 `ENOSYS`。验收包中的
`win64-bad-import.exe` 结构完全合法，只把 `WriteFile` 改为未知 `XriteFile`；
guest 必须拒绝它，释放部分建立的地址空间，并能继续运行正常 EXE。非 PE
输入也会被拒绝，不会落入用户态异常后才算失败。

## 验收

`make test-compat` 的 21 项包含 Win64 宿主结构检查、未知导入负例、真实
IAT/ABI 输出、退出码、跨格式拒绝、原生 ABI 回归，以及完整 BIOS 路径。
干净全量 `make clean && make test` 当前为 **101 PASS / 0 FAIL / RC=0**，
日志是 `build/full-test-win64-deb.log`。

## 明确尚未实现

- 任意 Windows EXE、PE32（32 位）、PIE/ASLR 重定位和任意 DLL 搜索/加载；
- 导入转发、delay-load、绑定导入、TLS 回调、SEH/异常展开；
- Windows CRT、文件/注册表/进程/线程/同步/Winsock/GUI 等完整 Win32；
- 控制台以外的真实 HANDLE 表、异步 OVERLAPPED、GetLastError 完整语义；
- Windows 应用安装器与驱动兼容。

因此准确结论是：**一个可构建、可重复实跑的 Win64 PE32+＋三项 kernel32
导入纵切片已完成；不能外推为“任意 Windows 程序已兼容”。**
