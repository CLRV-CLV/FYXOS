# FYXGit v1 实现与边界

FYXGit 是 FYXOS 的原生版本控制纵切片，不是把宿主 `git`  binary 伪装成系统
功能，也不宣称兼容 Git 的 SHA-1、index、pack 或远程协议。

## 已实现的命令

```text
git init [/absolute/root]
git [-C /absolute/root] add <file>
git [-C /absolute/root] commit -m <message>
git [-C /absolute/root] status
git [-C /absolute/root] log
git [-C /absolute/root] diff [--cached] [file]
git [-C /absolute/root] restore [--staged] <file>
```

未指定 `-C` 时根目录为 `/home/user`。相对文件路径相对于仓库根；绝对路径必须
位于该根内，拒绝 `..` 和 `.fyxgit` 自身。

- `add` 读取工作区文件，计算 CRC32，将内容追加到仓库内容池并更新暂存区。
- `commit` 保存完整暂存树、父提交 ID 和消息；ID 是 FYXGit v1 的 FNV-1a
  内容/父链摘要，不是 Git SHA-1。
- `status` 分别报告 staged/worktree 的 A/M/D，以及仓库根的直接未跟踪文件。
- `diff` 对工作区与暂存区比较；`--cached` 对暂存区与 HEAD 比较。文本输出
  公共前后缀之后的实际 `-`/`+` 行，二进制则明确报告。
- `restore` 从暂存区（无暂存项时从 HEAD）恢复工作区；`--staged` 只把暂存区
  重置到 HEAD，不改工作区。

## 持久化格式

仓库根下的 `.fyxgit` 是一个固定结构的 FYXGit v1 数据库，包含：

1. magic/version、仓库根、HEAD、提交数、暂存区；
2. 最多 8 个提交，每个提交最多 8 个文件及父 ID/消息；
3. 64 KiB 内容池，单个 blob 最大 4 KiB；
4. 全数据库 CRC32。

每次元数据修改通过 `vfs_write_file()` 原子替换整个数据库。加载时检查结构
长度、根路径、固定字符串终止、所有 blob 边界/CRC、HEAD 和数据库 CRC，损坏
不会被当作空仓库自动覆盖。

## 容量与尚未实现

v1 最多 8 个提交、8 个跟踪文件；内容池只追加，达到上限会明确返回 ENOSPC。
未跟踪扫描只覆盖仓库根的直接文件，已跟踪的子目录路径可以正常 diff/restore。

尚未实现：真实 Git 文件格式和 SHA-1/SHA-256、分支/tag、合并/rebase、rm/mv、
clone/fetch/push、SSH/HTTP、包压缩、签名、权限/换行转换和递归未跟踪扫描。
这些不能被当作完整 git 已交付。

## 验证

```bash
make test-git
```

测试在独立 `/home/user/repo` 中执行 init/add/commit/status/log/diff/restore，
再把同一 ATA 镜像重新启动，验证提交父链、clean 状态和恢复内容仍然存在。
长脚本还覆盖 4 KiB 键盘环形缓冲，防止大事务期间粘贴数据被静默丢弃。
