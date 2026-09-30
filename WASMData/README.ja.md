# WASM サンプル

[English](README.md) | 日本語


このディレクトリには KinoRuntime 用に作成したサンプルのソースとビルド用バッチを置きます。CoreMark と公式 TestSuite 本体は含みません。

## 既存バイナリの確認結果

| ファイル | 対応するソース・確認内容 | 公開時の扱い |
|---|---|---|
| `library.wasm` | `Library.c` の `ConsoleTest`、`SubTest`、`fibonacci` と対応。`env.Test1`、`env.Console` を import | 独自サンプルとして保持 |
| `start.wasm` | `System/Main.cpp`、`Adv.cpp` と対応。`Main` を export、`adventure` のホスト関数を import | 独自サンプルとして保持 |
| `fibtest.wasm` | `fibtest.c` の検証・表示文字列と対応。標準入出力等のツールチェーン側ライブラリがリンクされる | 生成物は除外し、ソースとビルド手順を公開 |

関数・import・文字列とビルドオプションからの確認であり、同じツールチェーンによる再ビルドでのバイト一致を確認したものではありません。`library.wasm` と `start.wasm` のビルドは `-nostdlib` を指定しています。

## 再生成

Emscripten の `emcc` / `em++` を PATH に設定し、このディレクトリをカレントディレクトリにして実行します。

```bat
build_fibtest.bat
build_adv.bat
build_library.bat
```

- `build_fibtest.bat`: このディレクトリの `fibtest.wasm` を生成します。Git の除外対象です。
- `build_adv.bat`: このディレクトリの `start.wasm` を生成します。
- `build_library.bat`: このディレクトリの `library.wasm` を生成します。

`fibtest.c` を再生成した場合も、標準ライブラリを含む生成物の配布には使用ツールチェーンのライセンス確認が必要です。公開リポジトリでは生成物を同梱しません。

CoreMark は利用者が別途用意し、[性能計測手順](../docs/ja/Build-and-Test.md)に従って実行します。公式 TestSuite は CMake の取得・生成手順で `Materials/` と `Bin/` に配置され、どちらも Git の除外対象です。
