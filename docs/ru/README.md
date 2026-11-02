# YukiSU

<img align='right' src='../YukiSU-mini.svg' width='220px' alt="yukisu logo">

[English](../README.md) | [简体中文](../zh/README.md) | [日本語](../ja/README.md) | [Türkçe](../tr/README.md) | **Русский**

Решение для получения root-доступа на Android-устройствах на уровне ядра, созданное как форк [`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra): некоторые бесполезные части удалены, а несколько интересных изменений добавлены.

> **⚠️ Важное уведомление**
>
> Userspace YukiSU был **полностью переписан на C++** (ранее он был основан на Rust). Это означает, что поведение YukiSU может отличаться от других форков KernelSU. Если вы столкнулись с проблемой, сообщите о ней нам, а не вышестоящим проектам.

[![Последний стабильный релиз](https://img.shields.io/github/v/release/Rouyashiki/YukiSU?label=Последний%20стабильный%20релиз&logo=github)](https://github.com/Rouyashiki/YukiSU/releases/latest)
[![Последняя тестовая версия](https://img.shields.io/badge/Последняя%20тестовая%20версия-nightly.link-39C5BB.svg?logo=github)](https://nightly.link/Rouyashiki/YukiSU/workflows/build-manager/main)
[![Группа](https://img.shields.io/badge/Группа-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![Лицензия: GPL v2](https://img.shields.io/badge/Лицензия-GPL%20v2-FFA500.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Лицензия: GPL v3](https://img.shields.io/badge/Лицензия-GPL%20v3-FFE211.svg?logo=gnu)](https://www.gnu.org/licenses/gpl-3.0.en.html)

## Возможности

1. Управление `su` и root-правами на уровне ядра
2. Встроенное монтирование модулей через Kagami с поддержкой внешних механизмов монтирования MetaModule и скриптов жизненного цикла
3. [App Profile](https://kernelsu.org/zh_CN/guide/app-profile.html) и управляемые отдельно для каждого приложения non-root настройки
4. Поддержка динамического менеджера с возможностью настраивать доверенные менеджеры помимо встроенного пути по имени пакета/подписи
5. Аутентификация SuperKey в стиле APatch с поддержкой как ключей, заданных во время компиляции, так и ключей, внедряемых в LKM через `ksud`
6. ADB root, sulog, SELinux hide, внедрение `init.rc` модулей и другие возможности вышестоящего проекта
7. Встроенный YukiZygisk — реализация Zygisk на уровне ядра, полностью совместимая с модулями [Zygisk Next](https://github.com/Dr-TSNG/ZygiskNext)
8. Инфраструктура sucompat/syscall hook на основе TSR
9. Функция UTS view, позволяющая подменять `uname` изменением `uts_ns` без повторной сборки ядра
10. Интеграция управления правилами скрытия Kasumi и настраиваемые пути `su`
11. Плагины Lua с настройками, журналами и поддержкой нескольких репозиториев плагинов
12. Менеджер Material 3 Expressive с патчингом образов через ImgPatch и редактором ramdisk

## Совместимость

- В настоящее время YukiSU поддерживает только загружаемый модуль ядра (`CONFIG_KSU=m`) и больше не поддерживает встроенный вариант `CONFIG_KSU=y`.
- YukiSU поддерживает режим LKM на устройствах Android GKI 2.0 (ядро 5.10+). Ядра GKI 1.0 и non-GKI не поддерживаются.
- YukiSU поддерживает только устройства `arm64-v8a`; для менеджера требуется Android 12 (API 31) или новее.
- YukiZygisk поддерживает сборку и внедрение для обеих ABI: `arm64-v8a` и `armeabi-v7a`.

## Установка

См. [`guide/installation.md`](guide/installation.md)

[Одноразовые скрипты](guide/module-script.md)

[Горячая установка модулей (на английском)](../guide/module-hot-install.md)

[Preinit модулей (на английском)](../guide/module-preinit.md)

## Интеграция

См. [`guide/how-to-integrate.md`](guide/how-to-integrate.md)

## Устранение неполадок

1. Устройство зависает после удаления менеджера?
   Удалите _com.sony.playmemories.mobile_

## Поддержать проект

- [Anatdx](https://afd.anatdx.moe) (сопровождающий YukiSU)
- [ShirkNeko](https://afdian.com/a/shirkneko) (сопровождающий SukiSU)
- [weishu](https://github.com/sponsors/tiann) (автор KernelSU)

## Лицензия

- Файлы в каталоге “kernel” распространяются по лицензии [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
- Все остальные части, не указанные выше, распространяются по лицензии [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html).

## Благодарности

- [KernelSU](https://github.com/tiann/KernelSU): вышестоящий проект
- ~~[MKSU](https://github.com/5ec1cff/KernelSU): Magic Mount~~
- ~~[RKSU](https://github.com/rsuntk/KernelsU): поддержка non-GKI~~
- ~~[KernelPatch](https://github.com/bmax121/KernelPatch): KernelPatch является ключевой частью реализации модуля ядра APatch~~

<details>
<summary>Благодарности KernelSU</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/): источник идеи KernelSU
- [Magisk](https://github.com/topjohnwu/Magisk): мощный root-инструмент
- [genuine](https://github.com/brevent/genuine/): проверка подписи APK v2
- [Diamorphine](https://github.com/m0nad/Diamorphine): некоторые техники rootkit

</details>
