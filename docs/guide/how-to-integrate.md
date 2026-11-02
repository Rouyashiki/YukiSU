# Integration Guide

YukiSU currently supports loadable kernel modules only (`CONFIG_KSU=m`) and no longer supports built-in `CONFIG_KSU=y`.

## Hook Methods

**TSR hook:**

- The default path for loadable kernel modules (LKM). Suitable for GKI 2.0 kernels (`5.10+`) and compatible kernels integrated from source.
- Requires `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y`, and `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`.

### How to Build the YukiSU LKM Using a Custom Kernel Source Tree

In most cases, the LKM built by CI using [ddk](https://github.com/Ylarod/ddk) can be loaded on the vast majority of devices. However, if your device cannot load it, you may need to build the LKM yourself using the kernel source and build configuration corresponding to your device.

> **Note: This is not integrating YukiSU into the kernel.**
>
> Make sure your kernel build tree has already been built with a configuration matching your device kernel, and that `vmlinux` exists in it.
>
> Make sure Clang, LLVM, and Git are available in your environment.

```sh
# Point to the kernel build tree that has already been configured and built.
# If the kernel is built with O=out, this should usually point to the out directory rather than the source directory.
export KDIR=<your kernel build tree directory>

export CLANG_PATH=<your Clang/LLVM bin directory>
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

After the build is complete, the generated LKM is located at:

```text
YukiSU/kernel/kernelsu.ko
```
