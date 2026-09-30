# ビルドとテスト

[English](../Build-and-Test.md) | 日本語


ランタイム本体は `KinoWASM/`、CLI の入口は `apps/KinoRuntime/main.c`、共用ホスト実装は `host/` に配置している。ソースの配置にかかわらず、以下の構成・ビルドコマンドはリポジトリのルートで実行する。

## 必要環境

- **Visual Studio 18** — MSVC 14.50 以降 (musttail 必須、下記)。VS 2022 でも **clang-cl を使う場合は** ビルド可
- **CMake 4.2 以降** + **Ninja**: VS 同梱版で OK (`<VS>/Common7/IDE/CommonExtensions/Microsoft/CMake/{CMake,Ninja}/bin`)
- **wabt** (`wast2json`, `wat2wasm`): `Test/wasi/` 配下の wast および公式 spec testsuite を json+wasm に変換する。PATH に置く
- **git**: 初回ビルド時に `Materials/wasm-spec/` へ `WebAssembly/testsuite` を自動 clone するために必要 (オフライン環境では事前に手動 clone)
- **musttail 対応コンパイラ必須**: clang / clang-cl か **MSVC 14.50+ (VS18)**。core エンジンは保証付き tail-call を使うため、非対応コンパイラは CMake が `FATAL_ERROR` で弾く。

## CMakePresets

`CMakePresets.json` の主な構成（clang-cl は PATH から検索するため、使用するLLVM環境を事前に有効にしてください）:

| Preset | Compiler | Build | 用途 |
|--------|----------|-------|------|
| `x64-Debug` | MSVC | Debug | 開発・デバッガ実行 |
| `x64-Release` | MSVC | Release | 本番計測 (既定) |
| `x64-clang-Debug` | clang-cl | Debug | clang での挙動確認 |
| `x64-clang-Release` | clang-cl | Release | clang-cl 性能比較 |
| `x64-Release-pgo-use` | MSVC | Release | PGO 最適化用（開発向け） |

ビルド出力は `out/build/<preset>/` に置かれ、POST_BUILD で `Bin/` にコピーされる (PGO の 2 preset は `out/build/x64-Release-pgo/` を共有)。

MSVC の Debug も musttail の要件を満たすため `/O2 /Ob2` を使用し、競合するランタイムチェック `/RTC1` を無効にする。デバッグ情報と Debug CRT は保持するが、最適化により変数表示やステップ実行がソースと一致しない場合がある。`x64-Debug` は古いツールセットを固定せず、開発者環境の対応 MSVC を使用する。

## ビルド方法

### Visual Studio から

`KinoRuntime` フォルダを開く → CMake が自動構成 → ターゲット (`KinoRuntime` / `testsuite_runner` 等) を選んでビルド。

### コマンドラインから

Visual Studioの対応する開発者コマンドプロンプトで実行します。

```bat
cmake --preset x64-Release
cmake --build out/build/x64-Release -j 1
```

> struct を変えた後やヘッダのみ更新した後は、`.obj` の不整合を避けるため新規ビルドディレクトリ or `--clean-first` が確実。並列ビルドで `Bin/` へのコピーが競合する場合は `-j 1` (逐次) で再実行する。

### 何がビルドされるか

| ターゲット | 出力 | 役割 |
|-----------|------|------|
| `KinoWASM` (静的ライブラリ) | `KinoWASM.lib` | ランタイム本体 |
| `KinoRuntime` (実行可能) | `Bin/KinoRuntime.exe` | `Library.wasm` の `_start` を実行する基本フロントエンド (WASI CLI モードも持つ) |
| `KinoRuntimePerfInvokeFunc2` | `Bin/KinoRuntimePerfInvokeFunc2.exe` | `library.wasm` 内の `fibonacci` をベンチマーク |
| `KinoRuntimePerfLoad` | `Bin/KinoRuntimePerfLoad.exe` | モジュールの decode + instantiate + core build のロード時間を計測 |
| `testsuite_runner` | `Bin/testsuite_runner.exe` | WASM 公式 testsuite + WASI テストを実行 |
| `KinoRuntimePushPopTest` | `out/build/<preset>/KinoRuntimePushPopTest.exe` (Bin へのコピーなし) | module push→invoke→pop 100,000 回のリーク回帰 |
| `kw_core_run` / `kw_core_apitest` / `kw_core_multitest` / `kw_core_yieldtest` / `kw_core_lifetest` | `Bin/kw_core_*.exe` | core エンジン単体ドライバ (kw_core_run は standalone_load 経由、他は公開 API 経由) |
| `kutil_microtest` | `Bin/kutil_microtest.exe` | KinoUtil (kalloc/karray 等) 単体テスト |

## 実行ファイルの使い分け

### `KinoRuntime.exe`

- カレントディレクトリの `Library.wasm` をロード→`_start` を実行→終了コードを返す。WASI モジュールは `KinoRuntime.exe <module.wasm> [args...]` で実行。
- CoreMark 等のベンチマーク .wasm を投入する用途で使う。
- スコアは標準出力の `Iterations/Sec` を参照 (高い方が良い)。

### `KinoRuntimePerfInvokeFunc2.exe`

- `library.wasm` の `fibonacci(N)` を呼ぶ計時用ハーネス。

### `KinoRuntimePerfLoad.exe`

- モジュールの decode → instantiate → core build までのロード時間を計測するハーネス。ロード最適化の回帰検出用。

### `testsuite_runner.exe`

- 公式 testsuite (`Bin/Test/testsuite/*.json`) を順次実行し、各テストの `passed=N failed=M` (skip 発生時は `skipped=K` 付き) を報告。
- **現行ベースライン: 全 JSON 合計 passed=45037 / failed=0** (2026-09-30 のクリーン生成環境)。failed が 1 件でも出たら回帰、`passed=` 行が返らない JSON はクラッシュとして扱う。集計時は stderr を分離する (`2>/dev/null`。`[core] runaway:` 診断が `passed=` 行を分断するため)。
- 引数で個別 JSON or サブディレクトリを指定可能:
  ```
  testsuite_runner.exe Test\testsuite\nop.json
  testsuite_runner.exe Test\testsuite\proposals\wasm-3.0
  ```
- **カレントディレクトリは `Bin/`** にすること (testsuite ディレクトリを `Test\testsuite` で探索するため)。`Bin/Test/` 側の古い runner は使わない。

## testsuite の準備

`Bin/Test/testsuite/` 配下の `.json + .wasm` は **`.gitignore` 配下** で git 管理外。CMake が `Materials/wasm-spec/` (WebAssembly/testsuite を pin commit でクローン) と `Test/wasi/*.wast` から自動生成する。simd_* spec テストも変換対象に含まれるが、既定 (SIMD OFF) では実行時に skip される。`-DKINOWASM_ENABLE_SIMD=ON` はパーサ受理のみで core は 0xFD 未実装のため、ON にすると SIMD テストが実行されて落ちる ([Testing.md](Testing.md) §2)。

## トラブルシューティング

| 症状 | 対処 |
|------|------|
| `ml64.exe not found` | VS の Visual Studio Installer から MSVC v143 (x64/x86 build tools) を入れる |
| `wast2json not found` (CMake 警告) | wabt をインストール、または `find_program(WAST2JSON_EXECUTABLE wast2json)` の HINTS にパスを追加 |
| `Materials/wasm-spec/` の clone に失敗 | git の確認、または手動で `git clone https://github.com/WebAssembly/testsuite Materials/wasm-spec` して pin commit に `git checkout --detach` |
| `testsuite directory not found` | カレントディレクトリが `Bin` でない。`cd Bin && testsuite_runner.exe` |
| `Error copying file` (並列ビルド) | `Bin/` へのコピー競合。`-j 1` で逐次再実行 |
| `crash <json>:<line> type=module code=0x...` | 単体 JSON クラッシュ。parser か runtime のバグ。失敗 JSON を個別実行し `_throwif` 直前に `printf` を挿して経路を追う ([Error-Handling.md §7](Error-Handling.md)) |

## 関連ドキュメント

- [Public-API.md](Public-API.md) — C API リファレンス
- [Testing.md](Testing.md) — testsuite_runner とテスト追加方法
