# Metal（独立实验模块）

Standalone, opt-in Metal support for Venus / Adreno 660, ported from the OnePlus 9
RedwoodMetal + AIR/Mesa path. This directory does not replace `display/` or enable
Metal in an existing kernel/runtime. [Validation](validation/RESULT.json).

本目录单独保存已上机验证的桌面 Metal 实现，**不接入现有 `display/` 基线，也不修改默认启动配置**。
运行时前端、AIR 编译后端与 GPU/显示退出修复需要配套使用。

| 目录 | 内容 |
| --- | --- |
| `src/kernel/` | 实验版 RedwoodGPU、Framebuffer；新增调用终止门和显示源队列排空 |
| `src/userland/` | RedwoodMetal 前端、IOSurface drawable、插件描述和发现属性启用工具 |
| `backend/` | 配套 AIR/NIR 后端、Darwin DRM 桥接、Mesa 25.0.7 补丁与构建工具 |
| `obj/` | 实际用于已验收实验内核的两个 arm64 Mach-O 可重定位对象，已去除调试信息 |
| `patches/` | 相对当前显示基线的退出修复，以及 Venus bundle 路径适配 |
| `tests/` | 原生发现/桌面探针、CA 像素测试、独立 Metal 测试源码及主机并发测试 |
| `validation/` | 不含设备身份或原始日志的验收摘要 |
| `scripts/` | 只构建/校验的脚本，产物位于忽略的 `out/` |

## 已验证范围

2026-11-15 独立真机包：**33/33** 启动与桌面检查通过。系统原生发现
`RedwoodMetalDevice`，backboardd 实际完成 Metal 命令；圆角、连续圆角、圆角路径和裁切子层
4 项 CA 检查各 3 帧，像素/颜色差异为零。设置进入前台、自动滑动/Home、Wi-Fi 重连/HTTPS、
蓝牙及壁纸服务通过；最高采样 35℃。正常退出时 GPU 调用和显示源队列排空，未观察到
kernel panic 或 backboardd 退出崩溃，Android 恢复通过。

首轮曾在 GPU 关闭后出现 backboardd SIGSEGV；本目录包含修正后的终止门/排空实现。
原始失败与成功证据留在开发工作区，公共摘要保留失败说明，未将失败改写为通过。

画质、无撕裂、更多应用/MPS、长期运行、休眠以及全应用稳定 120 FPS 尚未验收。
传输签名变化和离屏像素测试不属于面板光学验收。完整内核编译/链接完成，但原构建的
KPI 导出审计仍退出 2，不宣称整套内核构建检查全绿。

## 构建和接入边界

需要完整的 XNU/HackiOS 树、Linux/WSL 交叉工具链、theos iPhoneOS 16.5 SDK，以及自行准备的
iOS 16.5 System/OS/App。这里不包含 Apple SDK、着色器库、固件、凭据、完整内核或 payload。

```sh
bash metal/scripts/build-backend.sh /path/to/full-hackios-tree
bash metal/scripts/build-userland.sh /path/to/full-hackios-tree
python3 metal/tests/gpu-return.py
python3 metal/tests/display-drain.py
python3 metal/scripts/verify.py
```

内核文件依赖当前 `display/src/` 的 MiniIOSurface、VenusDpu/Refresh、翻转等实现及完整 XNU
环境；`obj/*.o` 不能作为独立 kext 加载。它们是 `display/` 对应编译单元的**替代版本**，未来
显式接入时只能选择一份，不能与旧对象重复链接。补丁不会由任何默认脚本自动应用。

启动集成需显式选择本实验内核、配套 runtime，在 launchd 前执行 `metal-enable`，并设置
`metal=1 camtl=1 metal_profile=1`。保留现有 Venus 桥接库及其亮度/触控改动。
完整 stock System 无自定义 bundle 软链接时，启用工具通过发现属性的相对路径定位
`/redwood/lib/RedwoodMetal.bundle`，不改写 System。QuartzCore 使用用户自己的匹配 stock
`default.metallib`；已验收库的哈希记录于摘要，不分发其内容。

`tests/desktop-probe.m` 会等待后打开设置并发送自动手势，仅用于显式实验验收，不能加入日常
启动任务。`tests/venus_compute.ll` 是手写 AIR，未附 Apple 编译器或生成的 `.metallib`。
重新组装测试镜像时仍需检查 loader 前缀和完整 cmdline 预算。

## 来源与完整性

一加 9 参考提交：`020124fde2c5708ef63f03fffd996a19b5b7c8dc`。
来源、原始/导出文件哈希和去调试信息前后对象哈希见 `PROVENANCE.json`；本目录文本统一为 LF。
`MANIFEST.json` 用于校验导出内容。Mesa 不是逐字节可复现构建；哈希识别具体文件，不证明
另一次编译必然生成相同二进制。许可和原作者信息见仓库 `NOTICE`、本目录 `licenses/` 及各文件。
