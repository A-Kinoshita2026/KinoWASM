# KinoRuntime CLI

[English](README.md) | 日本語


KinoWASM を組み込んで WASM モジュールを実行するアプリケーションです。

- `main.c`: コマンドライン処理と実行の入口
- `adventure.c`: `KINOWASM_ENABLE_ADVENTURE` が有効な構成で使用するホスト処理

共通のホスト実装は `host/`、ランタイム本体は `KinoWASM/` にあります。リポジトリのルートから CMake でビルドしてください。

```bat
cmake --preset x64-Release
cmake --build out/build/x64-Release -j 1
Bin\KinoRuntime.exe path\to\module.wasm
```

詳細は [ビルドとテスト](../../docs/ja/Build-and-Test.md) を参照してください。
