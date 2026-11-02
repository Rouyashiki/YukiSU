# 集成指导

YukiSU 当前仅支持可加载内核模块（`CONFIG_KSU=m`），不再支持内置 `CONFIG_KSU=y`。

## Hook 方法

**TSR hook:**

- 可加载内核模块 (LKM) 的默认路径。适用于 GKI 2.0 内核（`5.10+`）以及兼容的源码集成内核。
- 需要 `CONFIG_KPROBES=y`、`CONFIG_KRETPROBES=y` 与 `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`。

### 如何使用自定义内核源码树编译 YukiSU LKM

大部分情况下，由 CI 使用 [ddk](https://github.com/Ylarod/ddk) 编译的 LKM 可以在绝大多数设备上加载。但如果您的设备无法加载，您可能需要使用您设备对应的内核源码和构建配置自行构建 LKM。

> **注意：这不是将 YukiSU 集成进内核。**
>
> 请确保您的内核构建树已经使用与设备内核匹配的配置完成构建，并且其中存在 `vmlinux`。
>
> 确保您的环境中拥有可用的 Clang、LLVM 和 Git。
>
```sh
# 指向已经完成配置和构建的内核构建树。
# 如果内核使用 O=out 构建，这里通常应指向 out 目录，而不是源码目录。
export KDIR=<您的内核构建树目录>

export CLANG_PATH=<您的 Clang/LLVM bin 目录>
export PATH=$CLANG_PATH:$PATH

export CROSS_COMPILE=aarch64-linux-gnu-
export ARCH=arm64
export LLVM=1
export LLVM_IAS=1

git clone https://github.com/Rouyashiki/YukiSU
cd YukiSU/kernel

test -f include/uapi/supercall.h

make CONFIG_KSU=m \
    CONFIG_KSU_SUPERKEY=y \
    CONFIG_KSU_YUKIZYGISK=y \
    CC=clang

llvm-strip -d kernelsu.ko
```

编译完成后，生成的 LKM 位于：

```text
YukiSU/kernel/kernelsu.ko
```
