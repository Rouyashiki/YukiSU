# YukiSU

<img align='right' src='YukiSU-mini.svg' width='220px' alt="yukisu logo">

**English** | [简体中文](./zh/README.md) | [日本語](./ja/README.md) | [Türkçe](./tr/README.md) | [Русский](./ru/README.md)

A kernel-based root solution for Android devices, forked from [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra), with some useless parts removed and some interesting changes added.

> **⚠️ Important Notice**
>
> YukiSU userspace has been **completely rewritten in C++** (previously based on Rust). This means that YukiSU may behave differently from other KernelSU forks. If you encounter any issues, please report them to us rather than to upstream projects.

[![Latest release](https://img.shields.io/github/v/release/Rouyashiki/YukiSU?label=Latest%20release&logo=github)](https://github.com/Rouyashiki/YukiSU/releases/latest)
[![Latest nightly](https://img.shields.io/badge/Latest%20nightly-nightly.link-39C5BB.svg?logo=github)](https://nightly.link/Rouyashiki/YukiSU/workflows/build-manager/main)
[![Group](https://img.shields.io/badge/Group-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-FFA500.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-FFE211.svg?logo=gnu)](https://www.gnu.org/licenses/gpl-3.0.en.html)

## Features

1. Kernel-based `su` and root permission management
2. Built-in Kagami module mounting, with support for external MetaModule mounting backends and lifecycle scripts
3. [App Profile](https://kernelsu.org/zh_CN/guide/app-profile.html) and per-app non-root configuration
4. Dynamic manager support, allowing trusted managers outside the built-in package name/signature path to be configured
5. APatch-style SuperKey authentication, supporting both compile-time keys and keys injected into the LKM by `ksud`
6. ADB root, sulog, SELinux hide, module `init.rc` injection, and other upstream features
7. Built-in YukiZygisk, a kernel-based Zygisk implementation, fully compatible with [Zygisk Next](https://github.com/Dr-TSNG/ZygiskNext) modules
8. TSR-based sucompat/syscall hook infrastructure
9. UTS view functionality, supporting `uname` spoofing by modifying `uts_ns` without rebuilding the kernel
10. Integrated Kasumi hide-rule management and configurable `su` paths
11. Lua plugins with configuration, logs, and support for multiple plugin repositories
12. Material 3 Expressive manager with ImgPatch image patching and a ramdisk editor

## Compatibility Status

- YukiSU currently supports only loadable kernel modules (`CONFIG_KSU=m`) and no longer supports built-in `CONFIG_KSU=y`.
- YukiSU supports LKM mode on Android GKI 2.0 devices (kernel 5.10+). GKI 1.0 kernels and non-GKI kernels are not supported.
- YukiSU supports only `arm64-v8a` devices; the manager requires Android 12 (API 31) or later.
- YukiZygisk supports building and injection for both the `arm64-v8a` and `armeabi-v7a` ABIs.

## Installation

See [`guide/installation.md`](guide/installation.md)

[One-shot scripts](guide/module-script.md)

Module hot installation: [guide](guide/module-hot-install.md)

[Module preinit](guide/module-preinit.md)

## Integration

See [`guide/how-to-integrate.md`](guide/how-to-integrate.md)

## Troubleshooting

1. Device stuck after uninstalling the manager?
   Please uninstall _com.sony.playmemories.mobile_

## Sponsor

- [Anatdx](https://afd.anatdx.moe) (maintainer of YukiSU)
- [ShirkNeko](https://afdian.com/a/shirkneko) (maintainer of SukiSU)
- [weishu](https://github.com/sponsors/tiann) (author of KernelSU)

## License

- Files under the “kernel” directory are licensed under [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
- All other parts not mentioned above are licensed under [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html).

## Credits

- [KernelSU](https://github.com/tiann/KernelSU): upstream
- ~~[MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount~~
- ~~[RKSU](https://github.com/rsuntk/KernelsU): non-GKI support~~
- ~~[KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch is a key part of APatch's kernel module implementation~~

<details>
<summary>KernelSU credits</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): source of the KernelSU idea
- [Magisk](https://github.com/topjohnwu/Magisk): a powerful root tool
- [genuine](https://github.com/brevent/genuine/): APK v2 signature verification
- [Diamorphine](https://github.com/m0nad/Diamorphine): some rootkit techniques

</details>
