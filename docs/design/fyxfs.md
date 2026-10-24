# FYXFS v1 与系统还原实现说明

本文描述**当前已经实跑通过的实现**，不是远景规格。长期设计中的 B 树、
内容寻址、压缩、去重、AHCI/NVMe 等仍以 roadmap 为准，不在此冒充完成。

## 1. 当前执行链

```text
ATA IDENTIFY / LBA28 PIO
  → MBR 系统分区（LBA 2048）
  → FYXFS 双超级块 + CoW 根块
  → VFS 路径、目录、打开文件对象
  → 进程 fd（管道与文件统一分派）
  → ring 3 open/read/write/lseek/stat/mkdir/unlink/sync
  → 从持久化 /bin/init 读取 ELF 并 exec
```

首启时内核只自动格式化**两个超级块槽都全零的空白分区**；如果槽中有其他
数据，或看到 FYXFS magic 但校验失败，会报错而不是静默毁掉原卷。初始 `/bin/init` 由内核
携带的安装 blob 写入文件系统，之后的首次启动和 `exec` 都通过 VFS 读盘。

## 2. 磁盘格式

分区内部使用 512 字节扇区、4 KiB 数据块：

| 位置 | 内容 |
|---|---|
| sector 0 | 超级块槽 0 |
| sector 1 | 超级块槽 1 |
| sector 2..15 | 保留 |
| sector 16.. | 4 KiB 块区，块号从 0 开始 |

超级块记录分区范围、`generation`、当前根块和下一个追加块。根块包含扁平
节点表、extents 和快照表。超级块和根块末尾都有 CRC32；文件节点另存全文件
CRC32。`fsck` 检查根块、快照根、父子关系、extent 范围与文件数据校验和。

v1 固定上限为 40 个节点、16 个快照、23 字节单路径分量；目录查找是线性扫描。
数据与根块采用追加分配，暂不回收不可达旧块，达到上限会明确返回 `ENOSPC`。

## 3. 事务与断电语义

一次文件替换按以下顺序执行：

1. 分配并写入新数据块，旧数据保持不变；
2. 写入包含新 extent、长度和 CRC 的新根块；
3. `FLUSH CACHE`；
4. 写另一个超级块槽，以更大的 `generation` 发布新根；
5. 再次 `FLUSH CACHE`，然后才更新内存中的挂载状态。

挂载时分别校验两个超级块，选取最新且根块完整的事务。早期断电只会产生不可达
块；发布点之后断电则能看到完整新事务。Shell 的 `write` 使用单次替换事务，
不是“先 truncate 提交一次、再写内容提交一次”。

`tools/fyxemu.py` 实现 ATA IDENTIFY、读写和 FLUSH，并可在一个普通日志标记后
的第 N 次 ATA 扇区写完成时停止 CPU、保存介质，模拟真实断电。`make test-storage`
覆盖一个数据块的 8 个扇区 + 8 个根块扇区 + 1 个超级块扇区事务的关键写点：

| 断电点 | 重启后的预期 |
|---|---|
| 第 1 次写（数据中） | 旧内容 `original` |
| 第 9 次写（新根开始） | 旧内容 `original` |
| 第 16 次写（根完成、未发布） | 旧内容 `original` |
| 第 17 次写（超级块发布） | 新内容 `changed` |

每种情况都重新启动内核、挂载同一状态镜像，并要求启动检查及二次 `fsck`
均为 0 错误。

## 4. 快照与系统还原

- `snapshot create NAME` 在当前根中追加一条记录，指向之前的不可变根块；
  不复制文件数据，创建为 O(1)。
- `snapshot` 列出 ID、generation、根块和名称。
- `rollback ID` 读取历史根，沿用当前快照表，再作为一次新 CoW 事务发布。
- 默认回滚会先把**当前** `/home` 子树合并回待恢复根，因此系统文件恢复而
  个人文件不变；`rollback ID all` 才恢复包括 `/home` 在内的整卷状态。
- 有打开文件时回滚和卸载返回 `EBUSY`，避免 fd 悄悄指向另一代节点。

这不是“备份文件副本”的演示：回滚后的状态可卸载、重新挂载并跨重启保留。
当前救援 Shell 已能直接执行这些操作；A/B 内核槽、独立救援镜像、认证、
自动保留策略和包管理器联动仍未实现。

## 5. 用户与救援接口

新增系统调用：`open/lseek/stat/mkdir/unlink/sync`；原有 `read/write/close`
现在按 fd 类型分派到管道或 VFS 文件。fork 增加文件对象引用，exec 保留 fd，
exit 关闭文件与管道端点。

救援 Shell 现有 36 条命令（其中 2 条为阶段 I 的 ifconfig/ping），包括：

```text
ls / dir                 cat / type              write / cp
mkdir / md               rm / del                stat
df                       fsck                     sync
mount / unmount          snapshot [create NAME]   rollback ID [all]
```

其中 `dir/type/md/del` 是跨系统命令习惯的首批兼容别名，并不代表完整的
Linux/macOS/Windows 命令兼容层已经完成。

## 6. 可重复验证

```bash
make test           # full-boot + quick + 下列存储集成测试
make test-storage   # 只跑持久化、回滚和 4 个 ATA 写点断电测试
```

状态镜像和完整输出保存在 `build/storage-test/`，构建产物不进入 Git。
