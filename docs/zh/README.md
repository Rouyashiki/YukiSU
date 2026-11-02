# YukiSU

<img align='right' src='../YukiSU-mini.svg' width='220px' alt="yukisu logo">

[English](../README.md) | **简体中文** | [日本語](../ja/README.md) | [Türkçe](../tr/README.md) | [Русский](../ru/README.md)

一个基于内核的 Android root 方案，从 [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) 分叉而来，去掉了一些无用的部分，增加了一些有趣的变更。

> **⚠️ 重要提示**
>
> YukiSU userspace 已**完全用 C++ 重写**（原先基于 Rust）。这意味着 YukiSU 的行为可能与其他 KernelSU 分支有所不同。若遇到问题，请向我们反馈，而非上游项目。

[![最新正式版](https://img.shields.io/github/v/release/Rouyashiki/YukiSU?label=最新正式版&logo=github)](https://github.com/Rouyashiki/YukiSU/releases/latest)
[![最新测试版](https://img.shields.io/badge/最新测试版-nightly.link-39C5BB.svg?logo=github)](https://nightly.link/Rouyashiki/YukiSU/workflows/build-manager/main)
[![群组](https://img.shields.io/badge/群组-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![协议: GPL v2](https://img.shields.io/badge/许可证-GPL%20v2-FFA500.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![协议: GPL v3](https://img.shields.io/badge/许可证-GPL%20v3-FFE211.svg?logo=gnu)](https://www.gnu.org/licenses/gpl-3.0.en.html)

## 特性

1. 基于内核的 `su` 与 root 权限管理
2. 内置 Kagami 模块挂载，支持外部 MetaModule 挂载后端与生命周期脚本
3. [App Profile](https://kernelsu.org/zh_CN/guide/app-profile.html) 与按应用控制的非 root 配置
4. 动态管理器支持，可配置除内置包名/签名路径之外的受信任管理器
5. APatch 风格的 SuperKey 认证，支持编译期密钥，也支持由 `ksud` 注入 LKM
6. ADB root、sulog、SELinux hide、模块 `init.rc` 注入等上游特性
7. 内置 YukiZygisk，基于内核的 Zygisk 实现，全面兼容 [Zygisk Next](https://github.com/Dr-TSNG/ZygiskNext) 模块
8. 基于 TSR 的 sucompat/syscall hook 基础设施
9. UTS 视图功能，支持通过修改 `uts_ns` 来进行 uname 伪装，无需编译内核
10. 集成 Kasumi 隐藏规则管理与可配置的 `su` 路径
11. Lua 插件，支持配置、日志与多个插件仓库
12. Material 3 Expressive 管理器，提供 ImgPatch 镜像修补与 ramdisk 编辑器

## 兼容状态

- YukiSU 当前仅支持可加载内核模块（`CONFIG_KSU=m`），不再支持内置 `CONFIG_KSU=y`。
- YukiSU 支持 Android GKI 2.0 设备（内核 5.10+）的 LKM 模式。GKI 1.0 内核与 non-GKI 内核不被支持。
- YukiSU 仅支持 `arm64-v8a` 设备；管理器要求 Android 12（API 31）或更高版本。
- YukiZygisk 支持 `arm64-v8a` 与 `armeabi-v7a` 两种 ABI 的构建与注入。

## 安装

参见 [`guide/installation.md`](guide/installation.md)

[一次性脚本](guide/module-script.md)

模块热安装：[使用指南](guide/module-hot-install.md)

[模块 preinit](guide/module-preinit.md)

## 集成

参见 [`guide/how-to-integrate.md`](guide/how-to-integrate.md)

## 故障排除

1. 卸载管理器后设备卡住？
   请卸载 _com.sony.playmemories.mobile_

## 赞助

- [Anatdx](https://afd.anatdx.moe)（YukiSU 维护者）
- [ShirkNeko](https://afdian.com/a/shirkneko)（SukiSU 维护者）
- [weishu](https://github.com/sponsors/tiann)（KernelSU 作者）

## 许可证

- “kernel” 目录下文件采用 [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)。
- 上述以外的其余部分采用 [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html)。

## 鸣谢

- [KernelSU](https://github.com/tiann/KernelSU)：上游
- ~~[MKSU](https://github.com/5ec1cff/KernelSU)：Magic Mount~~
- ~~[RKSU](https://github.com/rsuntk/KernelsU)：non-GKI 支持~~
- ~~[KernelPatch](https://github.com/bmax121/KernelPatch)：KernelPatch 为 APatch 内核模块实现的关键部分~~

<details>
<summary>KernelSU 鸣谢</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/)：KernelSU 创意来源
- [Magisk](https://github.com/topjohnwu/Magisk)：强大的 root 工具
- [genuine](https://github.com/brevent/genuine/)：APK v2 签名校验
- [Diamorphine](https://github.com/m0nad/Diamorphine)：部分 rootkit 技巧

</details>
