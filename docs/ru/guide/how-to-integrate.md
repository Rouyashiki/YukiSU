# Руководство по интеграции

В настоящее время YukiSU поддерживает только загружаемые модули ядра (`CONFIG_KSU=m`) и больше не поддерживает встроенный вариант `CONFIG_KSU=y`.

## Методы Hook

**TSR hook:**

- Стандартный вариант для загружаемого модуля ядра (LKM). Подходит для ядер GKI 2.0 (`5.10+`) и совместимых ядер, интегрированных из исходного кода.
- Требуются `CONFIG_KPROBES=y`, `CONFIG_KRETPROBES=y` и `CONFIG_HAVE_SYSCALL_TRACEPOINTS=y`.

### Как собрать YukiSU LKM с использованием собственного дерева исходного кода ядра

В большинстве случаев LKM, собранный CI с помощью [ddk](https://github.com/Ylarod/ddk), может быть загружен на подавляющем большинстве устройств. Однако, если на вашем устройстве модуль не загружается, вам может потребоваться самостоятельно собрать LKM, используя исходный код ядра и конфигурацию сборки, соответствующие вашему устройству.

> **Примечание: это не интеграция YukiSU в ядро.**
>
> Убедитесь, что дерево сборки ядра уже собрано с конфигурацией, соответствующей ядру вашего устройства, и что в нём присутствует `vmlinux`.
>
> Убедитесь, что в вашей среде доступны Clang, LLVM и Git.

```sh
# Укажите уже настроенное и собранное дерево сборки ядра.
# Если ядро собирается с O=out, обычно здесь следует указать каталог out, а не каталог исходного кода.
export KDIR=<каталог дерева сборки ядра>

export CLANG_PATH=<каталог bin вашего Clang/LLVM>
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

После завершения сборки созданный LKM находится здесь:

```text
YukiSU/kernel/kernelsu.ko
```
