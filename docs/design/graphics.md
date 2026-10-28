# 阶段 J：VBE/BGA 软件图形栈与首版桌面

## 边界

本轮已经把图形主链做实并接入救援 Shell，但它是**首版软件桌面**，不是完整
FyxWay/FyxUI 生态，也不是 GPU 驱动。没有使用现成内核、桌面或图片渲染库。

已实现：

- stage2 在 BIOS 阶段通过 VBE `4F00/4F01/4F02` 枚举模式，选择 800×600×32 DirectColor LFB 并填写 `boot_info`；内核以 NOCACHE 映射物理帧缓冲；
- Bochs Graphics Array（BGA）运行时探测、模式设置和线性帧缓冲映射，作为无 VBE 时的回退；
- Live ISO 完成 init 后自动进入桌面，Q/Esc 才进入救援 Shell；
- 800×600、32 bpp、BGRX8888 小端像素格式；
- RAM 背 buffer → 软件光栅化 → LFB present 的双缓冲路径；
- 像素、矩形、边框、圆角矩形、圆、直线和内置 5×7 位图字体；
- 三个有重叠区域的窗口，焦点顺序/z-order、阴影、dock、明暗两套主题；
- `desktop` Shell 命令和 `desktop run` 的 N/T/R/Q 键盘交互；
- Unicorn BGA 端口模型、独立 framebuffer 物理映射、RGB PNG 导出和
  guest/host 帧校验和比对。

## 显示路径

```text
Shell / keyboard
       │
       ▼
desktop.c  窗口状态、焦点、主题、实时读取 net/FYXFS 状态
       │
       ▼
gfx.c      背 buffer 光栅化（clipping、文字、圆角、线条）
       │  469 个连续物理页（1,876 KiB 分配，1,875 KiB 可见像素）
       ▼
present    按 pitch 逐行复制到 HHDM 中的设备 LFB
       │
       ├─ vbe.c  boot_info 中的固件 LFB（完整 BIOS/VMware 优先路径）
       └─ bga.c  0x1ce/0x1cf 端口回退路径，物理地址 0xE0000000
```

BGA 驱动先写 `ID=0xB0C5` 并检查返回值，再设置 XRES/YRES/BPP、虚拟尺寸和
偏移，最后打开 `ENABLE|LFB`。通过 `vmm_map()` 映射 `0xE0000000` 的设备页，
页 flags 包含 `VMM_WRITE|VMM_NOEXEC|VMM_NOCACHE`；这不是把设备内存当普通
RAM，也没有把 LFB 地址硬编码成宿主指针。

VBE 模式查询只在 stage2 的实模式阶段执行；内核不再调用 BIOS。stage2 检查
分辨率、32 bpp、DirectColor 的 R/G/B 掩码位置、pitch 和 LFB 地址，成功后
填写 `boot_info.framebuffer_*`。`vbe.c` 用 `vmm_map()` 以
`VMM_WRITE|VMM_NOEXEC|VMM_NOCACHE` 映射该物理区域；BGA 则保留上述端口
探测路径。两者都没有时，`gfx_init()` 返回错误，文本救援 Shell 继续可用。

## 桌面操作

RAW 与 Live ISO 的正常启动都默认执行 `desktop_run`，因此用户先看到桌面；
按 Q/Esc 后仍可在救援 Shell 手工使用以下命令：

```text
desktop                 启动并呈现一帧（默认暗色、WELCOME 聚焦）
desktop next            循环聚焦下一窗口并重绘
desktop theme           切换暗色/亮色并重绘
desktop info            打印窗口、帧数、背 buffer 和校验和
desktop run             进入键盘交互：N 下一窗口，T 主题，R 重绘，Q/Esc 返回
```

窗口按“非焦点先画、焦点最后画”合成，因此重叠区域能实际验证 z-order。
NETWORK 窗口读取 DHCP/UDP 状态，RESTORE 窗口读取 FYXFS generation/快照
状态。顶栏的 `10:24` 是首版静态示意，不是 RTC 实现。

## 可重复验收

```bash
make image
make test-graphics

# 单独观察并导出最后一帧
.venv/bin/python tools/fyxemu.py --kernel build/fyxos.elf --mode quick \
  --desktop-test --keys 'ntq;desktop info;shutdown' \
  --keys-after '[init] 全部通过' \
  --fb-output build/desktop.png --test --max-seconds 90
```

`make test` 的 full-boot 脚本执行 VBE 模式枚举、`boot_info` 交接和桌面三帧，
quick 脚本则执行 BGA 回退路径；两者都验证焦点/主题、`desktop info` 和 PNG，
分别保存 `build/full-desktop.png`、`build/quick-desktop.png`。`make test-iso`
另验证 Live ISO 自动桌面先于救援 Shell，并导出实际 VBE 帧。
`tools/test_graphics.py` 额外验证：

1. `desktop run` 的 N/T/R/Q 产生四帧并返回 Shell；
2. PNG 是真实 800×600 RGB、scanline/filter 正确且含多种颜色；
3. guest 日志的 FNV-1a 与宿主直接读取 VBE/BGA framebuffer 的结果相同；
4. `--no-gfx` 下返回清晰错误，救援 Shell 仍能正常关机。

台架的 framebuffer 是与 guest RAM 分开的 `mem_map_ptr` 物理区域。PNG 从该
区域读取 BGRX 字节并转换为 RGB，而不是根据桌面状态重新绘制一张图。

## 尚未实现

- UEFI GOP、多显示器、运行时模式切换和热插拔；
- 鼠标、窗口拖动/缩放、合成协议、用户态图形客户端和真正的窗口管理器；
- GPU/ Vulkan 后端、DMA-buf、硬件光标、垂直同步和 120 Hz 承诺；
- 亚像素抗锯齿、完整 Unicode 字体、输入法、剪贴板和桌面搜索；
- 真实时钟、应用生态、IDE 和编译器图形界面。

因此“首版图形主链已验收”不等于“比 Windows 10 完整桌面已完成”，也不等于
最终目标中的兼容性、性能或全套工具链已经交付。
