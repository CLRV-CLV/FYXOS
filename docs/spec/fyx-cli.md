# FYX 终端与指令集规范

> 目标：**终端支持所有操作系统的命令**，并提供**一套全新的、更易用的指令集**。
>
> 设计立场：不用背诵历史包袱。老命令照跑（兼容层），新指令更好记、更一致、
> 更具可组合性，且**默认结构化输出**。

---

## 1. 三层结构

```
┌───────────────────────────────────────────────────┐
│  ① FYX 原生指令集    fyxsh 内建，新语法，默认 JSON │
├───────────────────────────────────────────────────┤
│  ② 兼容层            cmd / ps / bash / sh 名字映射 │
├───────────────────────────────────────────────────┤
│  ③ 外部程序          /bin /usr/bin 上的可执行文件  │
└───────────────────────────────────────────────────┘
```

解析顺序：③ → ① → ②。也就是说，若系统里真装了一个 `ls` 程序，就跑它；
否则走 FYX 内建；再否则查兼容表映射到对应的 FYX 指令。

---

## 2. ① FYX 原生指令集

### 2.1 语法总纲

```
动词 [对象] [修饰语...]        # 动词在前，对象紧随
```

统一规则（**无例外**）：

| 规则 | 说明 | 例子 |
|---|---|---|
| 动词是名词化的动作 | `list` `show` `make` `remove` `move` `copy` `set` `run` `find` | `list files` |
| 同类对象用复数 | `files` `disks` `users` `services` `ports` | `list services` |
| 选项统一长名，`--` 前缀，单词用 `-` | 不提供单字母简写 | `--sort=size` |
| 布尔开关 `--x` / `--no-x` | 永远成对 | `--hidden` / `--no-hidden` |
| 输出格式统一 `--out=table\|json\|csv\|plain` | 默认 `table`（交互）/ `json`（管道） | |
| 危险操作必须 `--force` 或交互确认 | 删除/覆写/格式化 | `remove files *.log --force` |
| 时间/大小接受人类单位 | `1h30m` `500M` `2GiB` | `find files --bigger=100M` |
| 一切可组合 | 管道传的是**结构化数据**，不是文本 | `list processes --out=json \| filter --cpu\>10` |

### 2.2 核心指令表

| 指令 | 等价的老命令 | 说明 |
|---|---|---|
| `list files [路径]` | `ls` | `--long --all --sort=name\|size\|time --recursive` |
| `show file <路径>` | `cat`/`head`/`tail` | `--lines=20 --from=start\|end --follow` |
| `find files [--name=…] [--in=内容] [--bigger=…] [--newer=…]` | `find`+`grep` | **内容搜索默认走索引**（`fyx-indexd`），秒级 |
| `make file <路径>` / `make dir <路径>` | `touch`/`mkdir` | `--parents` |
| `copy <源…> <目标>` | `cp` | `--recursive --preserve --no-overwrite` |
| `move <源…> <目标>` | `mv` | |
| `remove <路径…>` | `rm` | `--recursive`；默认进回收站，30 天后清理 |
| `rename <旧> <新>` | `mv`（同目录时） | 语义更清晰 |
| `list disks` | `lsblk`/`df` | 树形，含挂载点、可用空间、健康度 |
| `mount <设备> <挂载点>` | `mount` | `--read-only --snapshot=<名>` |
| `list processes` | `ps`/`tasklist` | `--tree --sort=cpu\|mem` |
| `stop process <pid\|名>` | `kill` | `--graceful`（默认）/`--force` |
| `list services` / `start|stop|restart service <名>` | `systemctl` | |
| `list ports` / `show port <号>` | `netstat`/`ss` | 直接给出**占用进程名** |
| `list packages` / `add package <名>` / `remove package <名>` | `apt`/`dnf`/`winget` | 见 `fyx-package.md` |
| `update system` / `rollback system [--to=<快照>]` | Windows Update / 系统还原 | 见 `design/system-restore.md` |
| `show system` | `uname -a`+`top` 摘要 | CPU、内存、磁盘、启动时长、内核版本 |
| `edit <文件>` | 调起 FYX IDE | 终端内直接开编辑器 |
| `build [目标]` / `run [目标]` / `test [目标]` | `make`/`cargo`/`npm` | 见 `toolchain-interface.md` |
| `undo` / `redo` | —— | **终端级撤销**：文件类操作可回滚 |
| `explain <指令>` | —— | 用自然语言解释任意指令/脚本 |
| `help [指令]` | `man`/`help` | 带**可运行示例** |

### 2.3 结构化管道（关键差异）

老 shell 的管道传文本，于是每个命令都要写一遍解析器。FYX 的管道传
**带类型的记录流**：

```console
fyxsh> list processes --out=json | filter 'cpu > 10' | sort memory --desc | take 5
┌──────┬──────────────┬───────┬──────────┐
│  PID │ 名称         │ CPU%  │ 内存     │
├──────┼──────────────┼───────┼──────────┤
│ 1204 │ fyx-compos   │ 23.1  │ 412 MiB  │
│  882 │ fyx-indexd   │ 15.7  │ 288 MiB  │
└──────┴──────────────┴───────┴──────────┘
```

- 命令声明自己的输出 **schema**（字段名与类型）
- 管道两端 schema 不匹配 → 在**输入时就报错**并给出可选项，而不是运行时吐一堆乱码
- 终端自动做**列宽**、**数字对齐**、**单位换算**、**颜色**

### 2.4 脚本语言 `fyxscript`

传统 shell 脚本的坑（引号、空格、未定义变量、错误不中断）全部规避：

```fyxscript
#!/usr/bin/env fyxsh

# 变量必须初始化；未定义变量是编译错误
let target = "build/"
let older_than = 7d

# 失败即中断（可用 `try` 显式捕获）
try:
    for file in find files --in=target --older-than=older_than:
        echo "清理 ${file.name}（${file.size}）"
        remove file
catch err:
    echo "跳过：${err.message}"
    continue

# 类型是显式的，管道里的数据有 schema
let big: File[] = find files --bigger=100M | collect
echo "共 ${big.len()} 个大文件"
```

特性：静态检查（`fyxsh --check`）、自动补全的类型信息、内置 `undo` 记录。

---

## 3. ② 兼容层：别的系统的命令怎么跑

### 3.1 机制

`/usr/share/fyx/compat/<flavor>.toml` 描述映射规则，四类处理：

1. **直译**：名字不同语义相同 → 直接映射到 FYX 指令
2. **改写**：拆分/重组参数
3. **拒绝**：语义危险或无法对应 → 给出**明确解释 + 推荐替代**
4. **直通**：若用户装了真实的 `bash`/`coreutils`（作为 `.fyx` 包提供），
   则在**兼容容器**里原样执行

### 3.2 覆盖矩阵

| 来源 | 覆盖 | 例子 |
|---|---|---|
| **Linux (GNU coreutils + util-linux)** | 95%+ | `ls -la` → `list files --long --all`；`grep -r foo .` → `find files --in=foo --recursive`；`df -h` → `list disks`；`ps aux` → `list processes --all`；`kill -9 123` → `stop process 123 --force`；`apt install x` → `add package x`；`systemctl restart n` → `restart service n` |
| **macOS / BSD** | 90%+ | `brew install x` → `add package x`；`say`、`pbcopy/pbpaste`、`open` → 对应 `open file` / 剪贴板指令；BSD 版 `ps`/`netstat` 参数差异单独处理 |
| **Windows CMD** | 90%+ | `dir` → `list files`；`copy`/`move`/`del`/`ren`/`md`/`rd` → 对应指令；`type` → `show file`；`tasklist`/`taskkill` → `list processes`/`stop process`；`ipconfig` → `show network`；`netstat -ano` → `list ports`；`sfc /scannow` → `verify system`；`chkdsk` → `check disk` |
| **PowerShell** | 85%+ | `Get-ChildItem` → `list files`；`Get-Process`/`Stop-Process`；`Get-Service`/`Restart-Service`；`Get-Content`/`Set-Content`；`Where-Object`/`Select-Object` → `filter`/`pick`；支持 `\|` 传对象（与 FYX 的结构化管道天然契合） |
| **Shell 内建与语法** | 常用子集 | `cd`、`pwd`、`export`、`alias`、`source`、`&&`、`\|\|`、`;`、`>`、`>>`、`<`、反引号、`$( )`、`*`/`?` 通配、环境变量展开、作业控制（`&`、`fg`、`bg`、`jobs`） |

### 3.3 不做的事（并明确告知）

- **不模拟** Linux 专有内核接口（`/proc` 的非常规字段、`ioctl` 语义）
  → 提供 **FYXOS 容器**：完整的 Linux ABI 兼容运行时，用于跑需要它的程序
- **不模拟** Win32 的 `regedit`/COM/WMI 全貌 → 提供常用子集的等价指令，
  其余引导到 FYXOS 的原生机制
- 遇到无法映射的命令，输出：

```console
fyxsh> iwconfig
  × 未知命令 "iwconfig"
  FYXOS 里没有这个命令的对应物，因为无线配置由 "fyx-net" 统一管理。
  你可能想要：
    show network            查看当前连接与信号强度
    set wifi --ssid=…       连接无线网络
  或者安装 Linux 兼容运行时以原样运行：add package linux-compat
```

---

## 4. ③ 新指令集的"更易学"体现在哪

| 痛点 | 传统做法 | FYX 做法 |
|---|---|---|
| 记不住 `-ltrh` 这种字母汤 | 背参数 | 全部长名、自解释：`--sort=time --reverse --human` |
| `rm -rf /` 灾难 | 无保护 | 默认进回收站；`--force` 才真删；`undo` 可回退 |
| 管道传文本，格式一变就崩 | 写 `awk`/`sed` | 结构化管道 + schema 校验 |
| 不知道命令有哪些选项 | `man` 长篇 | `help list files` 给分类选项 + 可点运行的示例 |
| 报错看不懂 | `Permission denied` | 指出**哪个权限**、**怎么开**：`需要 fs.write ~/.config/fyx-editor —— 运行 "grant …" 或 "fyx settings"` |
| 单位换算靠心算 | `1024*1024` | 统一接受 `1GiB`/`500M`/`1h30m` |
| 脚本一写就错 | 运行时炸 | `fyxsh --check` 静态检查 + 类型 |

---

## 5. 与当前内核 Shell 的关系

`kernel/shell/shell.c` 当前有 36 条命令：最初 16 条内核诊断命令，加上文件、
挂载、FYXFS 校验、快照/回滚、`dir/type/md/del` 等首批别名，以及阶段 I 的
`ifconfig/ping`。它仍是
**内核态救援 Shell**，不是最终用户终端，也不代表下列兼容规范已完整实现。

后续用户态 `fyxsh` 将接管：
- 内核 Shell 保留，通过 `Alt+F1` 或串口进入（救援用途）
- `fyxsh` 实现上述全部规范，兼容层以 `.fyx` 包分发，可独立更新
