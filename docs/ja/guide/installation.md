# インストールガイド

インストール方法については、[KernelSU ドキュメント - インストール](https://kernelsu.org/guide/installation.html) を参照してください。ここでは補足事項のみを説明します。

## ロード可能カーネルモジュール（LKM）によるインストール

[KernelSU ドキュメント - LKM インストール](https://kernelsu.org/guide/installation.html#lkm-installation) を参照してください。

LKM モードには `arm64-v8a` の GKI 2.0 カーネル（5.10+）と、デバイスの KMI に一致するモジュールが必要です。Android のバージョンだけでは互換性を判断できません。マネージャーには Android 12（API 31）以降が必要です。

## カーネルのインストールによる導入

申し訳ありませんが、YukiSU は built-in をサポートしていません。
