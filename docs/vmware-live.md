# FYXOS BIOS Live ISO 与 VMware 试用

## 成品与构建

```sh
make iso
# 输出：build/fyxos.iso
```

生成器只使用 Python 标准库，不需要 mkisofs、genisoimage 或 xorriso。ISO 包含标准 ISO9660 文件树、El Torito x86 BIOS hard-disk-emulation 启动项、`FYXOS.IMG;1` 和本说明。BIOS 先运行自研 stage1/stage2；进入长模式后，自研 ATAPI 驱动直接读取光盘内的系统分区，因此 **Live 试用不需要另加虚拟硬盘**。

## VMware 设置

1. 新建虚拟机，客户机类型选择 **Other / Other 64-bit**；1 个 vCPU、512 MiB 或更多内存即可。
2. 固件选择 **Legacy BIOS**，不要选择 UEFI/Secure Boot。本 ISO 没有 UEFI 启动项。
3. 使用 **IDE CD/DVD，连接为 IDE 0:0（primary master）**，选择 **Use ISO image file / 使用 ISO 映像文件** 并指向 `build/fyxos.iso`，勾选 **Connect at power on**。当前内核只有 legacy ATA 主通道主设备 ATAPI 驱动，SATA/AHCI、IDE 从盘和其他通道尚未实现。
4. 为体验独立 Live 模式，新建时不要添加硬盘，或删除向导默认创建的硬盘。当前块设备初始化优先使用 ATA 硬盘；连接空白 IDE 硬盘可能让它代替光盘成为系统盘。
5. 开机并从 CD/DVD 启动（必要时在 VMware Boot Menu 中选择 CD-ROM）。看到 FYXOS 内核自检、ring3 init 和 `fyxos>` 提示符后，输入 `help`。
6. 可试 `df`、`run /bin/gcc-c11-demo`、`deb /packages/fyx-deb-demo.deb` 等命令；输入 `shutdown` 关机。`desktop` 仅在检测到 Bochs BGA 时可用，VMware SVGA 尚未适配；VMware 默认 e1000/vmxnet 网卡也尚未适配，不要把模拟器中的 RTL8139 网络验收外推到 VMware。

## Live 写入边界

光盘只读。FYXFS 的创建、安装、快照和修改会写入 **RAM overlay**，本次运行及 `unmount;mount` 后仍可使用，但重启或关机后全部丢失，ISO 文件不会被改写。这不是持久安装器，也不表示已经支持 ATAPI 光盘刻写。

## 验证

```sh
make test-iso
```

该目标严格校验 ISO9660/El Torito 描述符和负向损坏输入，并在 Unicorn 台架中执行两次完整 BIOS→ATAPI→ring3 启动：第一次验证 Live 写入、重挂读回和 fsck，第二次验证文件随重启消失且原始 ISO SHA256 不变。

沙箱没有 VMware，因此这里实际完成的是 ISO 结构校验和模拟器硬件模型实跑；不把它表述为“已在 VMware 实测”。若在特定 VMware 版本遇到固件兼容问题，请保留启动画面和串口日志。
