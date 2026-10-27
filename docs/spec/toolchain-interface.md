# FYX 工具链接口规范

> 目标：FYXOS **自带全部语言的编译器**。不重新发明 LLVM/GCC，而是定义一套
> 稳定的接口，把各语言工具链以**统一、可管理、可复现**的方式接进系统。
>
> 当前可运行实现包括 `kernel/toolchain/fyxcc.c` 的小型 C 子集前端，以及
> `kernel/toolchain/fyxjs.c` 的 FyxJS 静态整数子集前端；两者直接生成静态
> x86_64 ELF，共用控制流、表达式和代码生成后端。FyxJS 已支持 function main、
> let、严格比较、console.log/process.exit。它们验证两种源语言的原生编译/装载
> 主链，但不代表完整 C/JavaScript、GCC/LLVM、多语言、自举或完整运行库已经交付。
> FyxJS 边界详见 [`../design/fyxjs.md`](../design/fyxjs.md)。
>
> 当前 C 前端还支持 `while/do…while/for`、`break/continue`、`?:`、局部变量
> 前/后缀 `++/--`、
> `+=`、`-=`、`*=`、`/=`、`%=`、移位、`~`、`&`、`|`、`^`、`!`、`&&`、`||`
> 与嵌套块；表达式遵循 C 的基本优先级，逻辑运算有真实短路和 0/1 归一化，
> `continue` 在 `for` 中先执行迭代表达式，在 `do…while` 中跳到尾部条件；
> `?:` 只求值选中的分支。自定义函数、指针、数组、结构体、预处理器与 libc
> 仍未实现。

---

## 1. 分层模型

```
┌────────────────────────────────────────────────────────┐
│  L4  用户体验层   fyx build / fyx run / IDE 集成       │
├────────────────────────────────────────────────────────┤
│  L3  语言前端     rustc / go / zigc / cpython / node   │
├────────────────────────────────────────────────────────┤
│  L2  公共后端     LLVM（clang 系）· GCC（gcc 系）      │
├────────────────────────────────────────────────────────┤
│  L1  二进制工具   fyx-as / fyx-ld / fyx-ar / fyx-objdump│
├────────────────────────────────────────────────────────┤
│  L0  ABI 与运行库 fyx-libc / libunwind / compiler-rt   │
└────────────────────────────────────────────────────────┘
```

每一层只依赖下一层的**公开接口**，不依赖具体实现。

---

## 2. L0：目标三元组与 ABI

### 2.1 目标三元组

```
x86_64-fyx-fyxos        # 主目标
aarch64-fyx-fyxos
riscv64-fyx-fyxos
```

组件含义：`arch-vendor-os`，其中 ABI 默认为 `fyx`（SysV 兼容，但有自己的
系统调用约定与 TLS 布局）。

### 2.2 与 Linux 的差异（移植时必须改的地方）

| 项目 | FYXOS | 说明 |
|---|---|---|
| 系统调用入口 | `syscall` 指令，编号从 `0x1000` 起 | 见 `include/fyxos/syscall.h` |
| 系统调用返回 | 成功返回 ≥0；错误返回 `-errno`（不设全局 errno） | 简化 libc |
| `errno` | 由 libc 从返回值合成 | 兼容 POSIX 源码 |
| TLS | Variant I（TCB 在前），`fs` 基址 | 与 x86-64 SysV 一致 |
| ELF 解释器 | `/lib/fyx-ld.so` | 自研动态链接器 |
| 默认栈 | 8 MiB，用户态；内核栈 16 KiB → 32 KiB | |
| 地址布局 | 内核高半核 `0xFFFFFFFF80000000`；HHDM `0xFFFF800000000000` | |
| `mmap` 语义 | 支持 `MAP_FYX_LAZY_SNAPSHOT`（CoW 快照映射） | 系统还原用 |

### 2.3 必须提供的运行库

| 库 | 作用 | 来源策略 |
|---|---|---|
| `fyx-libc` | C 标准库 + POSIX | **自研**（参照 musl 的设计，重写实现） |
| `libunwind` | 栈回溯 | 自研（内核 panic 也复用同一份逻辑） |
| `compiler-rt`/`libgcc` | 编译器内建函数 | 随 LLVM/GCC 生成 |
| `libc++`/`libstdc++` | C++ 标准库 | 随 LLVM/GCC 生成 |

---

## 3. L1：二进制工具接口

所有工具遵守统一命令行约定，便于上层无差别调用：

```
fyx-<tool> [--target=<triple>] [--sysroot=<path>] <常规参数>
```

| 工具 | 对应 | 备注 |
|---|---|---|
| `fyx-as` | GNU as | 支持 `.S` 预处理、`.incbin`、`.cfi_*` |
| `fyx-ld` | ld.lld | 支持 `--gc-sections`、`-z separate-code`、LTO 插件 |
| `fyx-ar` / `fyx-ranlib` | ar | 归档，支持确定性模式（时间戳归零） |
| `fyx-objcopy` / `fyx-strip` | objcopy | |
| `fyx-objdump` / `fyx-readelf` / `fyx-nm` | binutils | |
| `fyx-dwp` / `fyx-dsymutil` | 调试信息打包 | |

**自举顺序**（保证不用任何外来二进制即可重建自己）：

1. 用宿主工具链交叉编译 `fyx-binutils` → 得到 `x86_64-fyx-fyxos-ld`
2. 用该 ld + 宿主的 gcc 编译 `fyx-libc`（静态）
3. 用 fyx-libc + 宿主 gcc 编译完整的 `fyx-gcc`（交叉）
4. 用 fyx-gcc 重新编译 fyx-binutils 与 fyx-libc → **完成自举**
5. 之后 LLVM/Clang 用 fyx-gcc 构建

---

## 4. L2/L3：语言工具链注册接口

任何编译器只要安装时提供一个 **工具链描述文件** 即可被系统识别：

`/usr/share/fyx/toolchains/<name>.toml`

```toml
[toolchain]
name        = "llvm"
version     = "20.1.0"
triple      = "x86_64-fyx-fyxos"
languages   = ["c", "c++", "objc", "fortran", "rust(backend)", "swift(backend)"]
sysroot     = "/usr/lib/fyx/sysroot/llvm-20"

[bin]
cc    = "clang"
cxx   = "clang++"
as    = "fyx-as"
ld    = "fyx-ld"
ar    = "fyx-ar"

[capabilities]
lto         = { thin = true, full = true }
sanitizers  = ["address", "undefined", "thread", "memory"]
debug       = { dwarf = 5, split = true }
optimization = ["O0","O1","O2","O3","Os","Oz"]
deterministic = true          # 相同输入产出字节相同

[flags]
common  = ["-fno-plt", "-fPIE"]
sysroot = "--sysroot={sysroot}"

[env]
CC  = "clang --sysroot=/usr/lib/fyx/sysroot/llvm-20"
CXX = "clang++ --sysroot=/usr/lib/fyx/sysroot/llvm-20"
```

系统读取所有描述文件，生成 `fyx toolchain list / use / default`：

```console
$ fyx toolchain list
  名称        版本      语言                        状态
  gcc         14.2.0    c,c++,fortran,ada,go        默认
  llvm        20.1.0    c,c++,objc,fortran          可用
  rustc       1.85.0    rust                        可用
  go          1.24     go                          可用
  zig         0.14      c,c++,zig                   可用
```

### 4.1 确定性构建要求

工具链声明 `deterministic = true` 后，系统在打包（`.fyx`）时校验：
- 归档时间戳归零、文件顺序按路径排序
- 不嵌入构建路径（`-ffile-prefix-map`）
- 不嵌入构建时间（`SOURCE_DATE_EPOCH`）
- 不嵌入主机名/用户名

校验失败 → 包标记为不可复现，仓库侧拒绝收录。

---

## 5. L4：统一构建接口 `fyx build`

`fyx build` 是**描述式**的，不绑定具体构建系统：

`fyx.toml`（工程根）

```toml
[project]
name    = "fyx-editor"
version = "1.4.2"
license = "FYX-1.0"

[target.editor]
kind       = "executable"
sources    = ["src/**/*.rs"]
toolchain  = "rustc"
features   = ["lsp"]

[target.editor.depends]
fyx-gfx    = ">=0.9"
```

`fyx build` 内部：
1. 解析依赖图 → 求解（复用包管理器）
2. 为每个 target 生成**与构建系统无关**的编译命令列表
3. 分发到 `fyx-buildd`（并行、缓存、沙箱化）
4. 产出统一布局的 `out/`：`bin/ lib/ share/ include/ debug/`

**构建缓存**：内容寻址；相同输入（源码哈希 + 工具链哈希 + 标志）直接复用
产物，跨工程跨用户共享 `/var/cache/fyx/build`。

**远程执行/缓存**（可选）：遵循 Remote Execution API 的精简子集。

---

## 6. 语言支持清单（阶段 K 的目标）

| 语言 | 前端 | 策略 | 目标阶段 |
|---|---|---|---|
| C / C++ | GCC + Clang | 移植 | K1 |
| Rust | rustc（LLVM 后端） | 移植 | K2 |
| Go | gc（自举，需先有 Go 或 gcc-go） | 移植 | K2 |
| Zig | zig（自带后端） | 移植 | K2 |
| Python | CPython | 移植 | K3 |
| JavaScript/TS | 自研 `fyx-js`（先解释器，后 JIT） | **自研** | K3 |
| Java/Kotlin | 接入 Graal/OpenJDK | 移植 | K4 |
| C# | 接入 .NET 运行时 | 移植 | K4 |
| Swift | swiftc（LLVM） | 移植 | K4 |
| Haskell / OCaml / Lisp | GHC / OCaml / SBCL | 移植 | K4 |
| 科学计算 | Julia / R / Fortran | 移植 | K4 |
| Shell | 自研 `fyxsh`（兼容 bash/zsh/fish 常用语法，见 fyx-cli.md） | **自研** | K1 |

---

## 7. 移植检查清单（以 GCC 为例）

1. 新增 `config/fyxos.h`、`x86-64/fyxos.h` 目标宏
2. 实现 `libgcc` 的 FYXOS 分支（栈展开、内建函数）
3. `libstdc++` 需要：`syscall` 编号映射、`/proc` 替代（FYXOS 提供 `procfs`）
4. 动态链接器：自研 `fyx-ld.so`（不复用 glibc 的）
5. 测试：以 FYXOS 的 `fyxemu` 台架跑 GCC 的 `make check` 子集
6. 打包为 `.fyx`，标记 `deterministic = true`

---

## 8. 与内核的接口边界

工具链**只**依赖内核的公开 ABI：

- 当前已定义 38 个系统调用（`include/fyxos/syscall.h`，编号 0x1000 起；后续工具链所需接口仍需补齐）
- `procfs`/`sysfs` 提供的只读信息
- `.fyx` 包格式
- 文件权限与能力（capability）模型

内核不提供 Linux 兼容层；兼容性由 `fyx-libc` 在**源码级**保证（POSIX 完整），
并对常见的 Linux 专有行为给出明确文档与替代方案。
