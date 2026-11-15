# hackios-venus

在 **小米 11**（代号 `venus`，高通 SM8350 / 骁龙 888，QCA6490 Wi‑Fi/蓝牙）上运行 iOS 的
**驱动源码与编译产物**。

[English](README.md) | 简体中文

本仓库是从一个更大的、基于 XNU 的移植工作树中**单独抽取出来的驱动部分**。每个已完成的
硬件驱动各占一个文件夹，包含其源码（`src/`）和编译出的 arm64 Mach‑O 目标文件（`obj/`）。
多个驱动共用的平台/传输层放在 `common/`，内核构建脚本放在 `build/`。

> 范围：仅驱动。**不包含**完整 iOS 用户态、合并后的启动内核、引导加载器（bootloader）、
> 设备固件/校准 blob，以及任何私密凭据（见下文「已排除的内容」）。

## 目录结构

| 文件夹 | 驱动 / 组件 | 真机状态 |
|--------|-------------|----------|
| `battery/` | 电池 / 电量计（经 ADSP GLINK） | 真实电量/温度已验证 |
| `wifi/` | QCA6490 Wi‑Fi（`venus_lmd` + ath11k） | WPA2 + DHCP + DNS + HTTP/HTTPS 已验证 |
| `bluetooth/` | QCA6490 蓝牙（H4 / 原生 iOS） | 控制器 + BLE 扫描已验证；配对/音频待验 |
| `audio/` | ADSP + CS35L41 扬声器 / 麦克风 | 物理出声已验证（−24 dB 上限） |
| `display/` | Adreno GPU + DPU / 120 Hz 帧缓冲 | 120 Hz 呈现 已验证 |
| `touch/` | FocalTech / ST FTS 触摸屏 | 触控输入已验证 |
| `brightness/` | 背光持久化（用户态） | 跨重启恢复已验证 |
| `common/` | 共享 QMI / QRTR / ADSP / input / image 层 | — |
| `build/` | XNU 内核构建脚本 | — |

每个文件夹内都有 `README.md`，说明其文件构成及如何接入内核。

## 源码来源

驱动基于苹果 XNU（`xnu-8796.121.2`）与 `hackios-xnu-kernel` 平台层编写，是面向小米 11
的原创移植工作，叠加在 APSL 许可的 XNU 之上。Wi‑Fi 路径还在 FreeBSD LinuxKPI
兼容层（`wifi/lx-vlmd`）上编译 Linux 的 `ath11k` 驱动；相关第三方许可见 `NOTICE`。

## 编译产物

`obj/*.o` 是 XNU 构建实际产出的 arm64 Mach‑O 可重定位目标文件，每个驱动编译单元一个，
即最终被链接进内核的那些对象。为使公开产物不含构建主机路径，**已剥离调试信息**（符号表
与重定位保持完整，对象仍可链接）。`wifi/obj/ath11k-vlmd.o` 是由
`wifi/ath11k/mk-ath11k-vlmd.sh` 产出的单一预链接对象。

这些对象面向 XNU 内核，**不是**可独立加载的模块;它们由 XNU 构建
（`build/xnu-build.sh`）链接进内核镜像。

## 已排除的内容（脱敏）

为便于公开发布，以下内容被刻意排除：

- **私密凭据** — Wi‑Fi SSID/密码文件、`private/` 目录、网络配置。Wi‑Fi 驱动在运行时从
  ramdisk 文件读取凭据且从不记录;本仓库不含任何凭据。
- **设备身份** — 序列号 / MLB / UDID / ECID / MAC 等取值，以及引导加载器的身份伪装。
  不含任何设备标识符。
- **固件与校准 blob** — Wi‑Fi BDF（板级数据，含 MAC/射频校准）、AMSS/M3/BHI 固件镜像，
  以及内嵌这些 blob 的旧实验性 Wi‑Fi track（`redwood_wifi`）。这里包含的 `venus_lmd`
  Wi‑Fi 驱动在运行时从设备加载固件，自身不内嵌任何固件。
- **合并后的启动内核** — 它链接了内嵌 BDF/固件的旧 Wi‑Fi 对象，故不公开;改以各驱动
  独立对象提供。
- **iOS 用户态 / 根文件系统** 及全部研究日志。

唯一保留的二进制 blob 是 `touch/src/redwood_focaltech_fw.h`（触控面板控制器固件），
触控驱动会 `#include` 它，且其中不含设备身份。

## 构建

这里的对象由 Linux/WSL 上的 XNU 交叉构建产出，见 `build/xnu-build.sh` 与
`build/xnu-env.sh`。重新构建需要完整的 XNU 源码树与工具链（不属于这个仅含驱动的仓库）。
Wi‑Fi 射频对象用 `wifi/ath11k/mk-ath11k-vlmd.sh` 重新构建。

## 许可

基础 XNU/平台代码采用 Apple Public Source License 2.0（见 `LICENSE`）。第三方组件
（Linux ath11k、FreeBSD LinuxKPI）保留各自许可，见 `NOTICE`。

## 鸣谢

本工作基于以下开源项目：

- [hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel) — 驱动所
  基于的 XNU 平台层。
- [b1ackm4x/hackios-xnu-kernel](https://github.com/b1ackm4x/hackios-xnu-kernel) —
  lemonade（OnePlus 9）QCA6490 参考驱动，Wi‑Fi（`venus_lmd`）与蓝牙移植自此。
- [Apple XNU](https://github.com/apple-oss-distributions/xnu) — 驱动所面向的内核
  （`xnu-8796.121.2`）。
- [Linux 内核](https://github.com/torvalds/linux) — `ath11k` Wi‑Fi 驱动
  （`drivers/net/wireless/ath/ath11k`），原样使用。
- [FreeBSD](https://github.com/freebsd/freebsd-src) — 用于在 XNU 上编译 `ath11k`
  的 LinuxKPI 兼容头。

感谢这些项目的作者与维护者。