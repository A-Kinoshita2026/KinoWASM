# ホスト側の参考実装

[English](README.md) | 日本語


KinoWASM 本体は `../KinoWASM/` にあります。このディレクトリは CLI とテストで共用するホスト側の実装です。組み込み先のOSや権限・メモリ管理方針に応じて流用または置き換えてください。

| ファイル | 役割 |
|---|---|
| `extrafunction.c` | WASI 等のホスト関数 |
| `kw_core_mem_backend_win.h` | Windows の線形メモリバックエンド |
| `winapi.c` / `winapi.h` | Windows 側のラッパ |
| `systemmemory.c` / `systemmemory.h` | ホストのメモリバンク管理 |
| `debug.c` / `debug.h` | 計時・デバッグ出力 |
| `debug_opcode_names.inc` | デバッグ用命令名。`tools/gen_opcode_names.py` で生成 |

利用方法は [QuickGuide](../docs/ja/QuickGuide.md)、ホスト関数の仕様は [Host-Functions](../docs/ja/Host-Functions.md) を参照してください。
