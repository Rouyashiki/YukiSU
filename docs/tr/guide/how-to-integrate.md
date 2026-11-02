# Entegrasyon Kılavuzu

YukiSU şu anda yalnızca yüklenebilir çekirdek modülünü (`CONFIG_KSU=m`) destekler ve artık yerleşik `CONFIG_KSU=y` seçeneğini desteklemez.

## Hook Yöntemleri

**TSR hook:**

- Yüklenebilir çekirdek modülü (LKM) için varsayılan yöntemdir. GKI 2.0 çekirdekleri (`5.10+`) ve kaynak koddan entegre edilmiş uyumlu çekirdekler için uygundur.
- `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y` ve `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y` gerektirir.

### Özel Bir Çekirdek Kaynak Ağacı Kullanarak YukiSU LKM Nasıl Derlenir

Çoğu durumda CI tarafından [ddk](https://github.com/Ylarod/ddk) kullanılarak derlenen LKM, cihazların büyük çoğunluğunda yüklenebilir. Ancak cihazınız modülü yükleyemiyorsa, cihazınıza karşılık gelen çekirdek kaynak kodunu ve derleme yapılandırmasını kullanarak LKM'yi kendiniz derlemeniz gerekebilir.

> **Not: Bu işlem YukiSU'yu çekirdeğe entegre etmek değildir.**
>
> Çekirdek derleme ağacınızın, cihazınızdaki çekirdekle eşleşen bir yapılandırmayla önceden derlenmiş olduğundan ve içinde `vmlinux` bulunduğundan emin olun.
>
> Ortamınızda Clang, LLVM ve Git'in kullanılabilir olduğundan emin olun.

```sh
# Önceden yapılandırılmış ve derlenmiş çekirdek derleme ağacını belirtin.
# Çekirdek O=out ile derleniyorsa, burada genellikle kaynak dizini yerine out dizini belirtilmelidir.
export KDIR=<çekirdek derleme ağacı dizininiz>

export CLANG_PATH=<Clang/LLVM bin dizininiz>
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

Derleme tamamlandıktan sonra oluşturulan LKM şu konumda bulunur:

```text
YukiSU/kernel/kernelsu.ko
```
