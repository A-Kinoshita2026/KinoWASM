# KinoWASM ドキュメント

[English](../README.md) | 日本語


KinoWASM の利用・組み込みと、KinoRuntime のビルド・テストに必要な資料をまとめています。概要は [ルート README](../../README.ja.md) を参照してください。

## 読み始める順序

1. [QuickGuide](QuickGuide.md): ライブラリを組み込むための利用ガイド
2. [Build-and-Test](Build-and-Test.md): ビルド環境、実行方法、テストの準備
3. [Public-API](Public-API.md): 公開 C API のリファレンス

## ドキュメント一覧

| 文書 | 内容 |
|---|---|
| [QuickGuide.md](QuickGuide.md) | 初期化、メモリ割り当て、ロード、呼び出し、組み込み例 |
| [Public-API.md](Public-API.md) | 公開 C API の型・関数・呼び出し契約 |
| [Host-Functions.md](Host-Functions.md) | ホスト関数の登録、WASI の対応範囲、再開・モジュール管理 |
| [Error-Handling.md](Error-Handling.md) | エラーコードとエラー処理 |
| [Build-and-Test.md](Build-and-Test.md) | CMake によるビルドとプログラムの実行 |
| [Testing.md](Testing.md) | テストの準備、結果の判定、テスト追加方法 |
| [benchmark.md](benchmark.md) | CoreMarkと再帰フィボナッチによるCLIランタイムの暫定比較 |

## 文書の扱い

公開用のソースコピーは [Publishing.md](Publishing.md) の手順で作成します。

文書とコードに違いがある場合は、現在のヘッダ・実装を確認してください。内部の最適化記録や旧版資料は公開文書に含めません。
