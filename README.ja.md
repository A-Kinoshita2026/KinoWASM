# KinoRuntime

[English](README.md) | 日本語


KinoRuntime は C で実装された WebAssembly ランタイムです。組み込み用の静的ライブラリ **KinoWASM** と、実行・テスト・性能計測用のプログラムを含みます。

実行エンジンは `KinoWASM/core/` の register-TOS / direct-threaded アーキテクチャを採用し、保証付き末尾呼び出し（musttail）を使用します。

## 家庭用コンソール対応と採用実績

KinoWASM は Nintendo Switch、Nintendo Switch 2、PlayStation 5、Xbox 向けのビルド実績があり、複数の製品で使用されています。

コンソール向けの組み込み実装は非公開で管理しており、各プラットフォームの SDK とホスト実装が必要です。この公開リポジトリでは Windows x64 向けのビルドを提供し、コンソール SDK、プラットフォーム固有の組み込み実装、コンソール向けビルド手順は含みません。

## WebAssembly の主要機能の対応状況

✅ 実装済み · 🟡 部分対応または検証上の制約あり · ❌ 実行エンジン未対応。既定の core エンジンについての表で、仕様への完全準拠を認定するものではありません。

| 機能・命令群 | 状態 | 対応範囲 |
|---|:---:|---|
| 基本スカラー命令 | ✅ | `i32`、`i64`、`f32`、`f64` の演算・比較・変換・ビット再解釈 |
| 制御フロー・関数呼び出し | ✅ | block、loop、分岐、直接・間接呼び出し、ローカル・グローバル変数 |
| 線形メモリ | ✅ | load/store、`memory.size`、`memory.grow` |
| Mutable globals | ✅ | 可変グローバル変数の import/export |
| 符号拡張命令 | ✅ | `i32.extend8_s` / `extend16_s`、`i64.extend8_s` / `extend16_s` / `extend32_s` |
| 非トラップ浮動小数点→整数変換 | ✅ | `i32.trunc_sat_*`、`i64.trunc_sat_*` |
| Multi-value | ✅ | 関数の複数戻り値、block/loop の引数 |
| Bulk memory・テーブル操作 | ✅ | memory/table の init・copy・fill、data/element drop、table grow・size |
| Reference types | ✅ | 基本的な `funcref` / `externref`、`ref.null`、`ref.is_null`、`ref.func`、テーブル操作 |
| Tail calls | ✅ | `return_call`、`return_call_indirect` |
| Extended constant expressions | ✅ | 定数式内の整数 `add`、`sub`、`mul` |
| Multi-memory | ✅ | メモリ番号を指定する操作 |
| Memory64 / Table64 | 🟡 | 実装と選択されたテストあり。一部の境界値検証は未対応 |
| 例外処理 | 🟡 | 従来の `try`/`catch` と新しい `try_table` / `throw_ref` を実装。公式テストの網羅性には変換ツールの構文対応による制約あり |
| Wide arithmetic | ✅ | `i64.add128`、`i64.sub128`、`i64.mul_wide_s`、`i64.mul_wide_u` |
| SIMD / Relaxed SIMD | ❌ | core の実行未対応。SIMD のパーサ設定を有効にしても実行できません |
| Threads / atomics | ❌ | atomic 命令は拒否。WebAssembly のスレッド実行は未対応 |
| GC / typed function references | ❌ | GC の型・操作、`call_ref` / `return_call_ref` は未対応 |

WASI は別のホストインターフェースで、部分対応です。[Host-Functions.md](docs/ja/Host-Functions.md) を参照してください。テスト範囲とスキップ条件は [Testing.md](docs/ja/Testing.md) に記載しています。新しい機能の一部に対応していることは、WebAssembly 3.0 全体への対応を意味しません。

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
