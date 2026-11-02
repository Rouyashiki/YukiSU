# 統合ガイド

YukiSU は現在、ロード可能カーネルモジュール（`CONFIG_KSU=m`）のみをサポートしており、built-in の `CONFIG_KSU=y` はサポートしていません。

## Hook 方法

**TSR hook:**

- ロード可能カーネルモジュール（LKM）のデフォルト方式です。GKI 2.0 カーネル（`5.10+`）および互換性のあるソース統合カーネルに適しています。
- `CONFIG_KPROBES=y`、`CONFIG_KRETPROBES=y`、`CONFIG_HAVE_SYSCALL_TRACEPOINTS=y` が必要です。

### カスタムカーネルソースツリーを使用して YukiSU LKM をビルドする方法

ほとんどの場合、CI が [ddk](https://github.com/Ylarod/ddk) を使用してビルドした LKM は、大多数のデバイスでロードできます。ただし、お使いのデバイスでロードできない場合は、そのデバイスに対応するカーネルソースとビルド設定を使用して LKM を自分でビルドする必要がある場合があります。

> **注意：これは YukiSU をカーネルに統合する手順ではありません。**
>
> カーネルのビルドツリーが、デバイスのカーネルと一致する設定でビルド済みであり、その中に `vmlinux` が存在することを確認してください。
>
> 環境内で Clang、LLVM、Git が使用可能であることを確認してください。

```sh
# 設定およびビルド済みのカーネルビルドツリーを指定します。
# カーネルを O=out でビルドしている場合、通常はソースディレクトリではなく out ディレクトリを指定します。
export KDIR=<カーネルビルドツリーのディレクトリ>

export CLANG_PATH=<Clang/LLVM の bin ディレクトリ>
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

ビルド完了後、生成された LKM は次の場所にあります：

```text
YukiSU/kernel/kernelsu.ko
```
