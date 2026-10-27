# FYXOS — 第一条指令 / Developer Preview 发布片

一支可编辑、可复现的原创中文科技发布片。视觉采用极简排版、黑白留白、工程网格与青色光线；产品画面来自 FYXOS 实跑帧缓冲，数据来自版本化发布日志。没有使用其他厂商的商标、广告片段或产品素材。

## 成片规格

- Composition：`FyxLaunch`
- 1920×1080，24 fps，4320 帧，180 秒
- H.264 / 8-bit 4:2:0，立体声 AAC
- 输出：`release/fyxos-developer-preview-launch.mp4`
- 原创合成配乐：`public/audio/fyxos-score.m4a`

## 修改与渲染

需要 Node.js 22+。依赖版本由 `package-lock.json` 固定。

```bash
cd video
npm ci
npm run studio          # Remotion Studio，交互修改与预览
npm run render:smoke    # 前 48 帧，检查浏览器和编码链路
npm run render          # 完整 180 秒 MP4
npm run verify          # 完整解码、帧数、音轨和 SHA-256 验收
```

`browser.mjs` 使用项目内 Chromium，并显式解出 npm 包随附的 NSS/NSPR 兼容库；不依赖系统 Chrome。`stills.mjs` 可审查任意帧：

```bash
npm run stills                       # 12 个章节代表帧
npm run stills -- 250 1350 2580 4080 # 指定帧
```

PNG 输出到 `build/video-stills/`。正式渲染与完整解码检查不会只依赖这些源工程截图。

## 分镜

| 时间 | 章节 | 内容 |
| --- | --- | --- |
| 00:00–00:12 | Cold Open | 从第一条指令与真实运行发问 |
| 00:12–00:26 | Architecture | BIOS、长模式、自研内核、ring3 |
| 00:26–00:44 | Desktop | 真实桌面、32 MiB、38 syscalls、37 commands |
| 00:44–00:58 | Boot Proof | BIOS 启动、网络、GCC 示例与退出码 |
| 00:58–01:15 | GCC C11 SDK | 宿主 GCC、freestanding ELF、C11 语言特性 |
| 01:15–01:33 | Fyx Studio | 真实 Studio、保存/构建/ring3 运行 |
| 01:33–01:50 | App Image | 编译、打包、启动自定义应用 |
| 01:50–02:08 | FYXFS Restore | CoW、快照、回滚、`/home` 保留 |
| 02:08–02:26 | Evidence | 80 PASS / 0 FAIL / RC=0 / 编译诊断 0 |
| 02:26–02:40 | Honest Boundaries | 当前预览版的真实边界 |
| 02:40–02:52 | Release | 镜像、SDK、日志与下载入口 |
| 02:52–03:00 | End Card | FYXOS、版本与项目地址 |

## 素材与事实来源

- `public/images/desktop.png`：来自 `build/full-desktop.png` 的 800×600 实跑帧缓冲。
- `public/images/studio.png`：来自 `build/toolchain-test/studio.png` 的实跑帧缓冲。
- 启动与退出码：`release/boot-test.log`。
- 自定义应用：`release/app-image-test.log`。
- 80 项验收：`release/full-test.log` 中的 80 个 `[PASS]`，无 `[FAIL]`。
- 镜像与 SDK：版本 `v0.1.0-dev1`，校验见 `release/SHA256SUMS.txt`。

视频明确区分宿主 GCC 与 guest 内运行，不把 C/FyxJS 子集、当前 SDK 或 Developer Preview 描述为最终完整生态。

## 原创配乐

`make-score.mjs` 以确定性振荡器、噪声、包络与章节自动化生成 180 秒双声道音轨，不使用采样包或外部音乐。仓库已包含编码后的 M4A，因此直接渲染不必重新合成。修改配乐后执行：

```bash
npm run score
npm run render
```

脚本使用 Remotion 随附的 FFmpeg 将临时 PCM WAV 编码为 AAC，并删除临时 WAV。

## 文件

- `src/Root.jsx`：Composition、章节时间轴、音轨。
- `src/scenes.jsx`：12 个原创章节。
- `src/components.jsx`：排版、网格、终端、截图与动画组件。
- `render.mjs`：完整/冒烟渲染。
- `stills.mjs`：关键帧导出。
- `verify.mjs`：对最终 MP4 做全流解码与音视频验收，写入 `release/video-verification.json`。
- `make-score.mjs`：原创配乐合成器。

中文字体为 Noto Sans SC（SIL Open Font License），通过 npm 包安装；其余画面由本工程绘制或来自上述 FYXOS 实跑截图。
