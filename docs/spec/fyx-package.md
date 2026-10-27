# FYX 包格式规范 `.fyx`

> 目标：比 deb/rpm/msi/apk 更简单、更小、更安全，且天然支持**原子安装、
> 秒级回滚、去重、按需下载**。与 FYXFS 的写时复制能力直接咬合。

---

## 1. 设计原则

1. **单一文件格式**：容器 + 元数据 + 有效载荷 + 签名，一个文件搞定。
2. **内容寻址**：包内每个文件按 BLAKE3 哈希存储，跨包/跨版本天然去重。
3. **可流式解压**：按块压缩（zstd），可只取需要的文件，不必解全包。
4. **默认沙箱**：包声明所需权限，安装后按声明授予；无声明 = 无权限。
5. **可复现构建**：元数据里记录构建输入哈希，任何人可重产出字节相同的包。
6. **原子性**：安装是"建新树 + 换指针"，中断不留残骸；回滚 = 换回旧指针。

---

## 2. 文件整体布局

```
┌──────────────────────────────────┐  0
│  魔数 "FYXPKG\x00\x01"   8 B     │
├──────────────────────────────────┤
│  Header              固定 128 B  │
├──────────────────────────────────┤
│  Section Table   N × 32 B        │
├──────────────────────────────────┤
│  Section 数据（按 4 KiB 对齐）   │
│   ├─ META   (msgpack)            │
│   ├─ INDEX  (内容寻址文件索引)   │
│   ├─ PAYLOAD(压缩块流)           │
│   ├─ HOOKS  (可选，安装脚本)     │
│   └─ SIG    (ed25519 签名)       │
└──────────────────────────────────┘
```

所有多字节字段为**小端**。

### 2.1 Header（128 字节）

| 偏移 | 大小 | 字段 | 说明 |
|---|---|---|---|
| 0 | 8 | `magic` | `FYXPKG\0\x01` |
| 8 | 4 | `format_version` | 当前 `1` |
| 12 | 4 | `header_size` | 128 |
| 16 | 8 | `file_size` | 整个包文件字节数 |
| 24 | 4 | `section_count` | |
| 28 | 4 | `flags` | bit0 有签名 / bit1 有钩子 / bit2 可重定位 |
| 32 | 8 | `payload_offset` | PAYLOAD 段偏移 |
| 40 | 8 | `payload_size` | |
| 48 | 32 | `package_id` | BLAKE3(名称+版本+架构+构建输入) |
| 80 | 16 | `reserved` | 置 0 |
| 96 | 32 | `header_checksum` | BLAKE3(前 96 字节) |

### 2.2 Section Table 项（32 字节）

| 偏移 | 大小 | 字段 |
|---|---|---|
| 0 | 4 | `type` (`META`/`INDX`/`PAYL`/`HOOK`/`SIGN`) |
| 4 | 4 | `compression` （0=无，1=zstd，2=zstd+去重） |
| 8 | 8 | `offset` |
| 16 | 8 | `size`（压缩后） |
| 24 | 8 | `raw_size`（解压后） |

---

## 3. META 段（msgpack 映射）

```yaml
name:        fyx-editor
version:     1.4.2              # 严格 SemVer
epoch:       0                  # 打包修订，版本回退时用
arch:        x86-64             # x86-64 | aarch64 | riscv64 | noarch
summary:     比 VS Code 更强的自研 IDE
description: |
  多行描述……
license:     FYX-1.0
homepage:    https://fyxos.dev/pkg/fyx-editor

# 依赖：支持版本区间与"或"关系
depends:
  - fyx-libc >= 1.0
  - fyx-gfx  >= 0.9, < 2.0
  - ( llvm | gcc )          # 二者之一
provides:    [editor]
conflicts:   [vscode-compat-shim]
replaces:    [fyx-editor-legacy]

# 权限声明（默认全部拒绝）
perm:
  fs:
    read:  [~/.config, /usr/share/fyx-editor]
    write: [~/.config/fyx-editor, ~/.cache/fyx-editor]
  net:
    client: ["*.fyxos.dev:443"]     # 白名单，空 = 无网络
  ipc:   [fyx-compositor]           # 可与之通信的服务名
  device: []                        # 设备节点
  exec:  [subprocess]               # 允许派生子进程

# 构建可复现性
build:
  reproducible: true
  input_hash:   blake3:9f2c…      # 源码树 + 构建脚本 + 工具链版本
  toolchain:    fyx-gcc 14.2.0
  source:       git+https://…@v1.4.2

# 尺寸与校验
installed_size: 18874368
file_count:     412
```

### 3.1 依赖求解

- 使用 **SAT 求解**（内置极小的 DPLL 实现，避免引入外部求解器）
- 求解结果写入 `/var/lib/fyx/solver.log`，失败时给出**最短冲突解释**
  （"fyx-editor 需要 llvm，但 fyx-llvm 与已装的 fyx-gcc-13 冲突"）

---

## 4. INDEX 段（内容寻址索引）

```
struct FileEntry {          // 变长，紧凑编码
    u32   path_len;
    char  path[path_len];      // 相对包根，UTF-8，不含前导 '/'
    u8    kind;                // 0 普通文件 1 目录 2 符号链接 3 设备 4 套接字
    u16   mode;                // 权限位
    u32   uid, gid;            // 数值 ID（名字在 META 里映射）
    u64   mtime;
    u64   size;                // 解压后大小
    u8    hash[32];            // BLAKE3(内容)；目录为 0
    u64   payload_offset;      // 在 PAYLOAD 中的偏移
    u32   payload_blocks;      // 占用的块数
    u8    target_len;          // 符号链接目标
    char  target[target_len];
}
```

**去重**：内容相同（哈希相同）的文件，无论同包内还是跨包，只存一份。
安装时若内容块已存在于系统内容库 `/var/lib/fyx/store/<hash[0:2]>/<hash>`，
直接硬链接，不占额外磁盘。

---

## 5. PAYLOAD 段

- 切分为 **128 KiB 逻辑块**，每块独立 zstd 压缩（level 19 打包 / level 3 运行时）
- 块头：`u32 compressed_size | u32 raw_size | u8 checksum[4]`
- 好处：随机读取单文件不需要解压整个包；损坏只影响对应块。

---

## 6. HOOKS 段（可选，受限）

**默认禁止任意脚本**。仅允许声明式钩子（不执行 shell）：

```yaml
hooks:
  pre_install:   { type: systemd-like, unit: fyx-editor-cache.service, action: stop }
  post_install:
    - type: rebuild-cache      # 受控的内置动作
      target: /usr/share/mime
    - type: register-handler
      mimes: [text/plain]
  post_remove:
    - type: purge-dir
      path: ~/.cache/fyx-editor
```

确有需要时可用 `type: script`，但必须：
- 在 META 中显式声明 `perm.exec_script: true`
- 安装时向用户**高亮提示**，且默认在"不可信"档位被拒绝
- 在**沙箱**中执行（无网络、只读根、私有 /tmp）

---

## 7. SIG 段

```
struct Signature {
    u8   algo;          // 1 = ed25519
    u8   key_id[16];
    u64  signed_at;     // Unix 秒
    u8   sig[64];       // 对 Header+SectionTable+META+INDEX+PAYLOAD 的签名
    u8   cert_chain_len;
    u8   cert_chain[];  // 简化的 X.509 子集链
}
```

信任模型：**TOFU + 密钥固定（key pinning）**。仓库首次使用时记录其公钥，
后续变更需在 UI 上显式确认并给出旧/新指纹。

---

## 8. 仓库与索引

- 仓库根目录 `index.fyx`：一个特殊 `.fyx` 包，META 里带 `type: repository`
- 索引内每个包一行紧凑记录：`name|version|arch|package_id|download_size|installed_size|blake3`
- 支持**分片索引**（按首字母分片），客户端只拉需要的分片 → 索引更新流量 KB 级
- 支持**内容分发网络**与 P2P 补充源（可选）

---

## 9. 安装 / 回滚语义

安装 = 三步原子操作（依赖 FYXFS 快照，见 `docs/design/system-restore.md`）：

1. 生成待安装树（在临时子卷中解包，应用文件覆盖规则）
2. 落一个文件系统快照 `pre-<package_id>`
3. 原子地把子卷指针切换到新树；更新 `/var/lib/fyx/db`

**回滚**：把指针切回上一个快照，O(1) 时间。
**卸载**：反向切换；内容库按引用计数回收（无引用后 30 天清理）。

---

## 10. 与现有格式的对照

| 能力 | deb | rpm | msi | apk | **.fyx** |
|---|---|---|---|---|---|
| 原子安装 | 否 | 否 | 部分 | 否 | ✅ |
| 内置快照回滚 | 否 | 否 | 部分 | 否 | ✅ |
| 内容寻址去重 | 否 | 否 | 否 | 否 | ✅ |
| 按需流式解压 | 否 | 否 | 否 | 否 | ✅ |
| 默认权限沙箱 | 否 | 部分 | 否 | 否 | ✅ |
| 可复现构建元数据 | 部分 | 部分 | 否 | 否 | ✅ |
| 签名 | 外部 | 有 | 有 | 有 | ✅ 内置 |
