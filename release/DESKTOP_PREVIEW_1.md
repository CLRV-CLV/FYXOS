## FYXOS 0.1.0 Desktop Preview 1

这是一次**桌面优先的 BIOS Live 预发布**，不是最终完整操作系统。

### 直接下载

- **FYXOS-VMware-Desktop.iso** — 33 MB 自包含 Live ISO，正常启动自动进入 800×600 图形桌面，不要求先使用 Shell。
- **FYXOS-Desktop.png** — 从客户机实际 framebuffer 导出的桌面截图。
- **SHA256SUMS.txt** — ISO 与截图的 SHA-256。
- **full-test.log** — 本次提交的干净完整回归日志。

### VMware 使用

1. 客户机选择 **Other / Other 64-bit**，至少 512 MiB 内存。
2. 固件选择 **Legacy BIOS**，不要选择 UEFI/Secure Boot。
3. CD/DVD 使用 **IDE 0:0（primary master）**，连接此 ISO，并勾选 Connect at power on。
4. 为体验独立 Live 模式，不要添加 ATA 硬盘。
5. 开机后自动进入桌面：`N` 切换焦点，`T` 切换主题，`R` 重绘；`Q` 或 `Esc` 才进入救援 Shell。

### 本次新增

- BIOS stage2 VBE 800×600×32 DirectColor LFB 枚举、设置与 `boot_info` 交接。
- 内核 VBE NOCACHE 映射，BGA 作为无 VBE 时的回退。
- RAW 与 Live ISO 的正常启动均默认进入桌面。
- GitHub Actions 从源码构建 ISO，验证 ATAPI/ring3/Live RAM overlay，并发布真实 ISO 附件。

### 验证与边界

CI 先执行 `make test-iso`，再执行干净 `make test`；完整回归成功后才创建预发布。framebuffer PNG 与 guest/host 校验和必须一致。

当前验证环境是 Unicorn BIOS/VBE/ATAPI 台架，**尚未在 VMware 本体实测**。仅支持 Legacy BIOS 与 primary-master ATAPI；尚无 UEFI、SATA/AHCI、VMware SVGA II 原生驱动、鼠标、GPU/Vulkan 或 VMware 默认网卡驱动。Live 写入仅进入 RAM overlay，重启后丢失。
