# KinoRuntime

[English](README.md) | 日本語


KinoRuntime は C で実装された WebAssembly ランタイムです。組み込み用の静的ライブラリ **KinoWASM** と、実行・テスト・性能計測用のプログラムを含みます。

実行エンジンは `KinoWASM/core/` の register-TOS / direct-threaded アーキテクチャを採用し、保証付き末尾呼び出し（musttail）を使用します。

## ソースの配置

| ディレクトリ | 役割 |
|---|---|
| `KinoWASM/` | このプロジェクトの本体。組み込み用のランタイムライブラリ |
| `apps/KinoRuntime/` | KinoWASM を使用する CLI アプリケーション |
| `host/` | CLI・テストで共用するホスト実装。WASI 関数、Windows メモリバックエンド、メモリ・デバッグ補助 |
| `Test/` | 独自テスト、公式テストのランナー、性能計測ハーネス |
| `WASMData/` | WASM 側の独自サンプルと生成手順 |
| `tools/`、`scripts/` | 開発・ビルド用の補助ツール |
| `docs/` | 利用ガイド・API・ビルドとテスト手順 |

ライブラリの利用にはまず `KinoWASM/kinowasm.h` と利用者ガイドを参照してください。`host/` は利用側で用意する処理の参考実装です。

## はじめに

- アプリケーションへの組み込み: [利用者ガイド](docs/ja/QuickGuide.md)
- ビルドと実行: [ビルドとテスト](docs/ja/Build-and-Test.md)
- API: [公開 C API](docs/ja/Public-API.md)
- その他の資料: [ドキュメント一覧](docs/ja/README.md)

## ビルド環境

現在のルートビルド手順は Windows 向けです。CMake 4.2 以降、Ninja、および musttail 対応の clang-cl または MSVC 14.50 以降が必要です。テストデータの生成には wabt の `wast2json` と Git を使用します。

対応する Visual Studio の開発者コマンドプロンプトで、リポジトリのルートから実行します。

```bat
cmake --preset x64-Release
cmake --build out/build/x64-Release -j 1
```

生成した実行ファイルは `Bin/` にコピーされます。WASI モジュールの実行例:

```bat
Bin\KinoRuntime.exe path\to\module.wasm
```

全体の testsuite は `Bin/` をカレントディレクトリとして実行します。

```bat
cd Bin
testsuite_runner.exe
```

テストデータの取得・生成条件と結果の判定は [Testing.md](docs/ja/Testing.md) を参照してください。複数のビルド構成が同じ `Bin/` を使うため、実行前に最後にビルドした構成を確認してください。

## 対応範囲と制約

- WASI の対応範囲は [Host-Functions.md](docs/ja/Host-Functions.md) を参照してください。完全な WASI / POSIX 互換を表明するものではありません。
- SIMD は既定で無効です。有効化オプションはありますが、core の SIMD 実行は未実装です。
- メモリバックエンド等のホスト実装が必要です。組み込み例は利用者ガイドを参照してください。
- 信頼できないモジュールの実行に関する安全性の保証や、セキュリティ監査済みという表明はしていません。ホスト関数に渡す権限も含めて用途に応じた検証が必要です。

## ライセンスと公開準備

[MIT License](LICENSE) で提供します。Copyright (c) 2026 A.Kinoshita.

別途取得する第三者テスト群やツールチェーンのライブラリには、それぞれのライセンスが適用されます。
