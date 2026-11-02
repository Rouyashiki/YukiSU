# 安装指导

您可以前往 [KernelSU 文档 - 安装](https://kernelsu.org/guide/installation.html) 获取有关如何安装的参考，这里只是额外的说明。

## 通过加载可加载内核模块 (LKM) 进行安装

请参阅 [KernelSU 文档 - LKM 安装](https://kernelsu.org/guide/installation.html#lkm-installation)

LKM 模式要求 `arm64-v8a` GKI 2.0 内核（5.10+），并使用与设备 KMI 匹配的模块。不能仅凭 Android 版本判断兼容性；管理器要求 Android 12（API 31）或更高版本。

## 通过安装内核进行安装

抱歉，YukiSU 不支持 built-in。

## 修改启动配置

安装页的开发者选项可以禁用模块 preinit、自定义 init.rc，或将功能强制开启、关闭。强制值在刷入镜像并重启后生效，运行中的功能设置无法修改；选择“不锁定”并重新修补、刷入可解除锁定。

选择镜像文件时，可以启用“仅更新 ramdisk 配置”，保留镜像中的 init、内核模块、SuperKey 和 kernel。命令行对应：

```sh
ksud boot-patch --boot init_boot.img --no-install --no-preinit --force-feature su_compat=0 --out .
```

明确指定镜像时，此模式不需要 KMI 或新的 LKM。它保留已有的 bundled 标记、UTS 参数及其他未涉及的配置；`--allow-shell`、`--no-custom-rc`、`--no-preinit`、`--force-feature`、`--block-module` 和 ADB 调试设置按本次参数重新设置，省略即清除相应设置。`--uts-config` 只在指定时更新 UTS 配置。

`--no-install` 仅修改 ramdisk，不修改 ImgPatch 内嵌配置，也不能与替换 init、LKM、kernel 或 SuperKey 的参数组合。上述命令只输出镜像文件，不会刷写分区。
