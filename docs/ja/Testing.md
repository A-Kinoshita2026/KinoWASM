# テスト戦略

[English](../Testing.md) | 日本語


KinoRuntime のテストは大きく 11 系統あります:

1. **WASM 公式 testsuite** — WebAssembly 仕様準拠を検証 (passed=45,037 / failed=0、proposals・自作回帰 wast 含む。2026-09-30 のクリーン生成環境)
2. **wasi_test** — WASI snapshot_preview1 実装 (空 argv/environ) の回帰テスト (30 件)
3. **wasi_args_test** — argv / environ の伝搬テスト (12 件)
4. **wasi_fs_test** — preopened directory + read-only file I/O テスト (19 件)
5. **wasi_vfs_test** — `wasi_set_vfs()` で差し替えた in-memory モックが file 系で実際に呼ばれることを検証 (5 件)
6. **wasi_fs_write_test** — Phase 3b の write / pwrite / mkdir / rmdir / unlink / rename テスト (12 件)
7. **wasi_fs_advanced_test** — Phase 3c の `fd_readdir` / `path_link` / `path_symlink` / `path_readlink` + `path_open(O_DIRECTORY)` テスト (9 件)
8. **core / wasi 回帰 wast** — `Test/wasi/` の `core_*` / `wasi_efault_test` 等 (2026-07 のバグ修正の回帰: EH 値運搬 / else・valtype 検証 / heaptype desync / memory64 import / host reexport / WASI EFAULT 等)。wast2json で変換され 1 の testsuite 実行に取り込まれる
9. **core 単体ドライバ** — `Test/core/` の `kw_core_run` / `kw_core_apitest` / `kw_core_multitest` / `kw_core_yieldtest` (suspend/resume) / `kw_core_lifetest` (bridge lifecycle・build 失敗 / 複数 store) / `kw_core_reentrytest` (cross-module 呼出中の host 再入 invoke と suspend/resume)
10. **KinoRuntimePushPopTest** — module push→invoke→pop 100,000 回のリーク回帰 (`Test/pushpop_test.c`)
11. **手動ハーネス** — `KinoRuntimePerfInvokeFunc2.exe` (fibonacci ベンチマーク) / `KinoRuntimePerfLoad.exe` (ロード時間)、KinoUtil 単体の `kutil_microtest`

CoreMark スコアリングは `KinoRuntime.exe` を使う ([Build-and-Test.md](Build-and-Test.md))。

旧ワークスペースの合計45500件には、現在のCMakeでは生成しないJSONが7本（463件分）残っていました。クリーンな公開用コピーでは258本の結果出力、passed=45037 / failed=0を確認しています。件数比較にはクリーンな生成先を使用してください。

## 1. testsuite_runner

### 概要

`Test/testsuite_runner.c` は WebAssembly 公式 testsuite の `.wast` を `wast2json` で変換した **JSON ディレクティブ** を読み、各コマンドを実行して `passed=N failed=M` を出力する (skip があった場合は `passed=N failed=M skipped=K`)。skip は 2 種: WASM 2.0/3.0 の仕様衝突アサーションのハードコード skip_list と、feature 無効 (SIMD OFF / threads) で load できないモジュールに属するコマンド。

### JSON フォーマット

```json
{
  "source_filename": "nop.wast",
  "commands": [
    {"type": "module", "filename": "nop.0.wasm"},
    {"type": "assert_return",
     "action": {"type": "invoke", "field": "as-func-first", "args": []},
     "expected": [{"type": "i32", "value": "1"}]}
  ]
}
```

サポートしているコマンド種別:

| type | 意味 |
|------|------|
| `module` | `.wasm` をロード |
| `register` | 別名でモジュールを登録 |
| `action` | invoke / get を実行 (戻り値検証なし) |
| `assert_return` | invoke して期待値と比較 |
| `assert_return_canonical_nan` | NaN canonical チェック |
| `assert_return_arithmetic_nan` | NaN arithmetic チェック |
| `assert_trap` | invoke して trap (失敗) を期待 |
| `assert_exhaustion` | スタック枯渇等を期待 |
| `assert_malformed` / `assert_invalid` / `assert_unlinkable` / `assert_uninstantiable` | ロード失敗を期待 |

### testsuite ディレクトリの探索

`testsuite_runner` 起動時に以下を順に試して最初に `.json` を含むディレクトリを使う:

1. `Test\testsuite`
2. `.\Test\testsuite`
3. `..\Test\testsuite`
4. `..\..\Test\testsuite`
5. `..\..\..\Test\testsuite`

`Bin/` をカレントにして実行すると `Bin/Test/testsuite/` がヒットする。引数で個別 `.json` やサブディレクトリも指定可:

```
testsuite_runner.exe Test\testsuite\nop.json
testsuite_runner.exe Test\testsuite\proposals\wasm-3.0
```

### Spectest モジュール

`spectest` というモジュール名で `print` / `print_i32` / `print_f64` 等の no-op 関数を提供する。これは公式 testsuite の多くがインポートしているもの。`testsuite_runner` 内で **WASM バイト列リテラル (`spectest_module[]`)** として埋め込み、起動時に自動ロードする。

### 個別テスト間の状態リセット

各 JSON ファイルの先頭で `reset_loaded_modules` が呼ばれる:

```c
kinowasm_term(handle);                       // 既存 store を破棄
free(store_memory_buffer);                   // store memory 解放
free_tracked_module_memories(&context);      // モジュール memory も全解放
context.handle = kinowasm_init();            // 新規 store
assign_store_memory(&context);               // store memory 再確保
load_spectest_module(&context);              // spectest を再注入
```

これによりテスト間で副作用が漏れない。

### クラッシュ捕捉 (Windows)

`run_command_with_crash_log` が `__try / __except` で AV を捕捉し、`crash <json>:<line> type=<type> code=0x<hex>` を出力してから次のテストへ進む。クラッシュした JSON はそこで打ち切り、次の JSON へ。

## 2. testsuite の準備

### `.gitignore` 配下

`Bin/Test/testsuite/*.json` と `*.wasm` は **`.gitignore` 配下** で git 管理外。実機では公式 testsuite を `wast2json` で変換したものを置く必要がある。

公式 testsuite のレイアウト例:

```
Bin/Test/testsuite/
├── address.wast       (.wast 元ファイル — 任意、参照用)
├── address.json       (wast2json の出力)
├── address.0.wasm     (モジュールごとに連番)
├── address.1.wasm
├── ...
└── proposals/         (wasm-3.0 / threads / wide-arithmetic 等。allowlist で取り込み)
```

### 公式 testsuite の取得

CMake が初回ビルド時に `Materials/wasm-spec/` へ pin commit でクローンし、`wast2json` で変換する。手動で用意する場合:

```bash
git clone https://github.com/WebAssembly/testsuite.git
cd testsuite
for f in *.wast; do
  wast2json "$f" -o "<KinoRuntime>/Bin/Test/testsuite/${f%.wast}.json"
done
```

### 現状 pass しているテスト (記載時点)

トップレベル `.wast` の大半 (address / block / br / call / const / conversions / float 系 / global / i32 / i64 / if / loop / memory 系 / table 系 / traps / type 等) と proposals (wasm-3.0 / wide-arithmetic 等の allowlist 取り込み分)、自作回帰 wast を含む全体で **passed=45,037 / failed=0** (2026-07-22 実測)。

### 未対応

- allowlist 外の `proposals/` (gc / function-references 等)。exception-handling (legacy 含む)・memory64・multi-memory・table64・wide-arithmetic は **取り込み済みで pass**
- threads (0xFE atomics): テストは取り込み済みだが、パーサが常時 `ERR_FEATURE_DISABLED` で拒否するため該当モジュールは **skip** になる (旧 `KINOWASM_ENABLE_THREADS` オプションは撤去済み)
- SIMD (0xFD): 既定 OFF で該当テストは skip。`-DKINOWASM_ENABLE_SIMD=ON` は **パーサ受理のみ** で core は 0xFD 未実装のため、ON にすると SIMD テストが実行されて落ちる (passed=43993 / failed=67 の実測例)。計測前に `Bin\` のバイナリが既定構成であることを確認する

## 3. wasi_test

### 目的

`extrafunction.c` の WASI snapshot_preview1 実装に対する回帰テスト。`random_get` / `clock_time_get` / `clock_res_get` / `fd_prestat_*` および既存スタブ群 (`fd_close` / `fd_fdstat_get` / `fd_seek` / `args_*` / `environ_*`) を網羅。

### 仕組み

`Test/wasi/wasi_test.wast` が WASI 関数を `(import "wasi_snapshot_preview1" ...)` で取り込み、各関数を呼んで戻り値 (errno) や WASM linear memory に書き戻された値を `assert_return` で検証する。

testsuite_runner は起動時に `register_standard_func()` を呼ぶので、`extrafunction.c` の実装がそのまま import 解決される。

### 検証範囲 (30 アサーション、空 argv/environ 状態)

| 関数 | 検証内容 |
|------|---------|
| `random_get` | 成功 errno、`len=0` 動作、2 回呼出で異なる値 (確率的) |
| `clock_time_get` | REALTIME/MONOTONIC 成功、不正 id で EINVAL、REALTIME の上位 32bit 非 0、MONOTONIC 単調非減少 |
| `clock_res_get` | 成功 errno、値 = 1ns、不正 id で EINVAL |
| `fd_prestat_get` / `_dir_name` | 任意 fd で EBADF |
| `fd_close` | 成功 errno (現状の no-op 動作) |
| `fd_fdstat_get` | fd 0/1/2 成功・filetype = CHARACTER_DEVICE、fd 3 で EBADF |
| `fd_seek` | EIO/ESPIPE 返却 |
| `args_*` / `environ_*` | 空状態の挙動: count=0、buf_size=0、errno 0 |

### wasi_fs_test (19 アサーション、preopened directory 配下の read-only file I/O)

`Test/wasi/wasi_fs_test.wast` は Phase 3a で実装した path_open / fd_read / fd_seek / fd_filestat_get 等を検証する。`Bin/Test/testsuite/wasi_fs_test/` に配置され、testsuite_runner が:

```c
wasi_add_preopen("Test/wasi_fs_data", ".");   // fd 3 を割り当て
```

を呼んでから実行する。テストデータは `Test/wasi/wasi_fs_data/` を `Bin/Test/wasi/wasi_fs_data/` に CMake で複製。

| データ | 内容 | サイズ |
|--------|------|--------|
| `hello.txt` | `"Hello, WASI!"` | 12 byte |
| `digits.bin` | `"01234567"` | 8 byte |

検証内容:
- `fd_prestat_get` / `fd_prestat_dir_name` で preopen 列挙
- `path_open` 成功 / fd 番号 / path traversal 拒否 (`../escape` → EINVAL) / 存在しないファイル → EBADF
- `fd_read` で先頭 4 byte = "Hell"、count=4
- `fd_seek` SEEK_END + `fd_tell` でファイルサイズ取得
- `fd_pread` offset=2 から 4 byte = "2345"
- `fd_filestat_get` / `path_filestat_get` でサイズ・filetype
- `fd_fdstat_get` で REGULAR_FILE / DIRECTORY 報告

### wasi_vfs_test (5 アサーション、VFS 差し替えの検証)

`Test/wasi/wasi_vfs_test.wast` は `wasi_set_vfs()` で in-memory モック VFS に差し替えた状態で WASI file 系関数が呼ばれることを検証する。`Bin/Test/testsuite/wasi_vfs_test/` に配置され、testsuite_runner が:

```c
wasi_set_vfs(&mock_vfs);                    /* in-memory モックに差し替え */
wasi_add_preopen("/mock", ".");             /* 実 path "/mock" は実在しない */
```

を行ってから実行する。モックは `vfs_mock.bin` のみ提供、内容 8 byte: `'V','F','S','-','O','K','!',0xAB`。

**検出能力**: `wasi_set_vfs(&mock_vfs)` をコメントアウトすると、libc バックエンドが /mock/vfs_mock.bin を開けず最初のアサーションが失敗する。これにより VFS 差し替えが本当に効いていることが回帰確認できる。

### wasi_fs_write_test (12 アサーション、書き込み系の検証)

`Test/wasi/wasi_fs_write_test.wast` は Phase 3b で追加した書き込み機能を検証する。`Bin/Test/testsuite/wasi_fs_write_test/` に配置され、testsuite_runner が `wasi_add_preopen("Test/wasi_fs_write_scratch", ".")` を行う (CMake が空ディレクトリを自動生成)。

検証内容:
- `path_open` の CREATE+TRUNC+WRITE モード / `fd_write` で 4 byte 書き込み
- `path_filestat_get` で書き込み後のサイズ確認、read-only で再 open して読み戻し
- `fd_pwrite` で先頭 1 byte 上書き
- `path_rename` / `path_create_directory` / `path_remove_directory` / `path_unlink_file`
- 書き込みモードでも path traversal は EINVAL で拒否

テストは冒頭で残骸を片付け idempotent に動作する設計。

### wasi_args_test (12 アサーション、既知 argv/environ)

`Test/wasi/wasi_args_test.wast` は引数渡しの実体動作を検証する。testsuite_runner が以下をセットしてから実行する:

```c
argv    = {"runner_test", "alpha"};                // 2 件、合計 18 byte
environ = {"FOO=bar", "BAZ=qux", NULL};            // 2 件、合計 16 byte
```

検証内容:
- `args_sizes_get` の count=2、buf_size=18 / `args_get` の argv ポインタ配列・文字列内容
- `environ_sizes_get` の count=2、buf_size=16 / `environ_get` のポインタ配列・文字列内容

### 自動生成

`Test/CMakeLists.txt` で各 `Test/wasi/*.wast` を `wast2json` で変換し `Bin/Test/testsuite/` 配下に出力する。`testsuite_runner` の依存ターゲットなのでビルドのたびに自動更新される。

### 実装を変更したら

WASI 関数の挙動を変えたら、対応する `assert_return` も追従する必要があります。新しい WASI 関数を追加したら `*.wast` の import 一覧にも追加し、対応するテスト関数を増やす。

## 4. テストを追加するには

### a. 既存の WASM testsuite 用 .wast を追加するとき

公式リポジトリの該当 `.wast` を `wast2json` で変換するだけ。提案系は `Materials/wasm-spec` の pin commit から allowlist で取り込み、proposals サブディレクトリへ出力する (`Test/CMakeLists.txt` 参照)。

### b. 専用テスト .c を書きたい場合

`Test/perf_invoke_func2.c` のような **WASM ファイルを直接 invoke する C ハーネス** や、`Test/core/` の standalone ドライバのような形で新規作成し、`Test/CMakeLists.txt` に `add_executable` を足す。

## 5. 性能計測用プログラム

`KinoRuntimePerfInvokeFunc2.exe` は `library.wasm` の `fibonacci` を呼び出し、`KinoRuntimePerfLoad.exe` はモジュールのロード時間を計測します。CoreMark 本体と公式 TestSuite は公開リポジトリに同梱しません。

## 6. CTest 統合

生成ツールが利用できる構成では、CTest に全体 `testsuite_runner` と `kw_core_yieldtest` / `kw_core_lifetest` / `kw_core_reentrytest` の4件が登録されます。全体テストの作業ディレクトリは `Bin/` に設定し、失敗件数の出力も検査します。リポジトリのルートから `ctest --test-dir out/build/x64-Release --output-on-failure` で実行できます。テスト入力の生成完了と、全体結果の件数も確認してください。

## 7. 関連ドキュメント

- [Build-and-Test.md](Build-and-Test.md) — ビルド全般、各 `.exe` の使い分け
- [Host-Functions.md](Host-Functions.md) — WASI 実装と VFS 差し替え
- `Test/testsuite_runner.c` — 実装原本
