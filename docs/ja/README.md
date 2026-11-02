# YukiSU

<img align='right' src='../YukiSU-mini.svg' width='220px' alt="yukisu logo">

[English](../README.md) | [简体中文](../zh/README.md) | **日本語** | [Türkçe](../tr/README.md) | [Русский](../ru/README.md)

カーネルベースの Android root ソリューションです。[`SukiSU-Ultra`](https://github.com/ShirkNeko/SukiSU-Ultra) からフォークし、不要な部分をいくつか削除し、いくつかの興味深い変更を追加しています。

> **⚠️ 重要なお知らせ**
>
> YukiSU の userspace は **C++ で完全に書き直されました**（以前は Rust ベース）。これは、YukiSU の挙動が他の KernelSU フォークと異なる可能性があることを意味します。問題が発生した場合は、上流プロジェクトではなく、私たちに報告してください。

[![最新正式版](https://img.shields.io/github/v/release/Rouyashiki/YukiSU?label=最新正式版&logo=github)](https://github.com/Rouyashiki/YukiSU/releases/latest)
[![最新テスト版](https://img.shields.io/badge/最新テスト版-nightly.link-39C5BB.svg?logo=github)](https://nightly.link/Rouyashiki/YukiSU/workflows/build-manager/main)
[![グループ](https://img.shields.io/badge/グループ-Telegram-blue.svg?logo=telegram)](https://t.me/manosaba)
[![ライセンス: GPL v2](https://img.shields.io/badge/ライセンス-GPL%20v2-FFA500.svg?logo=gnu)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![ライセンス: GPL v3](https://img.shields.io/badge/ライセンス-GPL%20v3-FFE211.svg?logo=gnu)](https://www.gnu.org/licenses/gpl-3.0.en.html)

## 機能

1. カーネルベースの `su` と root 権限管理
2. Kagami によるモジュールマウントを内蔵し、外部 MetaModule のマウントバックエンドとライフサイクルスクリプトにも対応
3. [App Profile](https://kernelsu.org/zh_CN/guide/app-profile.html) とアプリ単位で制御する non-root 設定
4. 動的マネージャーに対応し、組み込みのパッケージ名/署名パス以外の信頼済みマネージャーを設定可能
5. APatch 方式の SuperKey 認証。コンパイル時のキー、および `ksud` による LKM へのキー注入の両方に対応
6. ADB root、sulog、SELinux hide、モジュールの `init.rc` 注入などの上流機能
7. YukiZygisk を内蔵。カーネルベースの Zygisk 実装で、[Zygisk Next](https://github.com/Dr-TSNG/ZygiskNext) モジュールと完全互換
8. TSR ベースの sucompat/syscall hook 基盤
9. UTS ビュー機能。カーネルを再ビルドせずに `uts_ns` を変更して `uname` を偽装可能
10. Kasumi の非表示ルール管理と設定可能な `su` パスを統合
11. 設定、ログ、複数のプラグインリポジトリに対応する Lua プラグイン
12. ImgPatch によるイメージパッチと ramdisk エディターを備えた Material 3 Expressive マネージャー

## 互換性

- YukiSU は現在、ロード可能カーネルモジュール（`CONFIG_KSU=m`）のみをサポートしており、built-in の `CONFIG_KSU=y` はサポートしていません。
- YukiSU は Android GKI 2.0 デバイス（カーネル 5.10+）の LKM モードをサポートします。GKI 1.0 カーネルおよび non-GKI カーネルはサポートされません。
- YukiSU は `arm64-v8a` デバイスのみをサポートし、マネージャーには Android 12（API 31）以降が必要です。
- YukiZygisk は `arm64-v8a` と `armeabi-v7a` の両 ABI のビルドと注入をサポートします。

## インストール

[`guide/installation.md`](guide/installation.md) を参照してください。

[ワンショットスクリプト](guide/module-script.md)

[モジュールのホットインストール（英語）](../guide/module-hot-install.md)

[モジュール preinit（英語）](../guide/module-preinit.md)

## 統合

[`guide/how-to-integrate.md`](guide/how-to-integrate.md) を参照してください。

## トラブルシューティング

1. マネージャーをアンインストールした後、デバイスが固まりますか？
   _com.sony.playmemories.mobile_ をアンインストールしてください。

## スポンサー

- [Anatdx](https://afd.anatdx.moe)（YukiSU メンテナー）
- [ShirkNeko](https://afdian.com/a/shirkneko)（SukiSU メンテナー）
- [weishu](https://github.com/sponsors/tiann)（KernelSU 作者）

## ライセンス

- “kernel” ディレクトリ内のファイルには [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html) が適用されます。
- 上記以外のすべての部分には [GPL-3.0 or later](https://www.gnu.org/licenses/gpl-3.0.html) が適用されます。

## 謝辞

- [KernelSU](https://github.com/tiann/KernelSU)：上流
- ~~[MKSU](https://github.com/5ec1cff/KernelSU)：Magic Mount~~
- ~~[RKSU](https://github.com/rsuntk/KernelsU)：non-GKI 対応~~
- ~~[KernelPatch](https://github.com/bmax121/KernelPatch)：KernelPatch は APatch のカーネルモジュール実装における重要な部分~~

<details>
<summary>KernelSU の謝辞</summary>

- [Kernel-Assisted Superuser](https://git.zx2c4.com/kernel-assisted-superuser/about/)：KernelSU のアイデアの出典
- [Magisk](https://github.com/topjohnwu/Magisk)：強力な root ツール
- [genuine](https://github.com/brevent/genuine/)：APK v2 署名検証
- [Diamorphine](https://github.com/m0nad/Diamorphine)：一部の rootkit テクニック

</details>
