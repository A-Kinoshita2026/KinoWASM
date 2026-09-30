# Quick Guide — KinoWASM 利用者ガイド

[English](../QuickGuide.md) | 日本語


KinoWASM ライブラリを **自分のアプリに組み込んで使う** ためのチュートリアルです。サンプルコードはコピーしてそのまま動くことを目指しています。

API の網羅的なリファレンスは [Public-API.md](Public-API.md) を参照してください。

---

## 0. このガイドの読み方

シナリオ別 (「何をしたいか」) に並べているので、興味のある節から飛んでください:

| やりたいこと | 節 |
|--------------|-----|
| まず最小限動かしたい | [§1 Hello WASM](#1-hello-wasm) |
| 関数に引数を渡して結果を受け取りたい | [§2 関数を呼ぶ](#2-関数を呼ぶ) |
| C 側の関数を WASM から呼ばせたい (host import) | [§3 ホスト関数を追加](#3-ホスト関数を追加) |
| WASM 側のメモリを読み書きしたい | [§4 メモリにアクセス](#4-メモリにアクセス) |
| 複数モジュールを連携させたい | [§5 複数モジュール](#5-複数モジュール) |
| エラーをハンドリングしたい | [§6 エラー処理](#6-エラー処理) |
| 自分のプロジェクトに組み込みたい | [§7 ビルド統合](#7-ビルド統合) |
| よくある落とし穴 | [§8 よくある落とし穴](#8-よくある落とし穴) |

---

## 1. Hello WASM

次の例は import のない `example.wasm` をロードして `_start` を呼びます。core の WASI フォールバックがリンク時に参照する環境アクセサも定義しています。`host/extrafunction.c` をリンクする場合は、この3関数の定義を省いてください。

```c
#include <stdio.h>
#include <stdlib.h>
#include "kinowasm.h"
#include "systemmemory.h"
#include "kw_core_mem_backend_win.h"

/* Minimal environment accessors required by the core WASI fallback.
 * Omit these when linking host/extrafunction.c, which provides them. */
static const char* const empty_argv[] = { NULL };

int wasi_get_argc(void)
{
	return 0;
}

const char* const* wasi_get_argv(void)
{
	return empty_argv;
}

const char* const* wasi_get_envp(void)
{
	return NULL;
}

int main(void)
{
	const size_t store_size = 100 * 1024 * 1024;
	const size_t module_size = 20 * 1024 * 1024;
	void* store_buffer = malloc(store_size);
	void* module_buffer = malloc(module_size);
	kinowasm_handle_t store = NULL;
	kinowasm_args_t args = { 0 };
	kinowasm_result_t result = RES_ERROR;
	if(store_buffer == NULL || module_buffer == NULL)
		goto cleanup;

	kw_core_install_default_mem_backend();
	change_system_memory();
	store = kinowasm_init();
	if(store == NULL)
		goto cleanup;

	kinowasm_assign_memory(store, store_buffer, store_size);
	kinowasm_mem_info_t module_memory = kinowasm_mem_info_init(module_buffer, module_size);
	if(module_memory == NULL)
		goto cleanup;

	result = kinowasm_load_module(store, "example.wasm", "sample", module_memory);
	if(result != RES_SUCCESS)
		goto cleanup;

	result = kinowasm_array_new_from(args, 0);
	if(result == RES_SUCCESS)
		result = kinowasm_invoke(store, "sample", "_start", &args);

cleanup:
	kinowasm_array_term_from(args);
	if(store != NULL)
		kinowasm_term(store);

	free(module_buffer);
	free(store_buffer);
	if(result != RES_SUCCESS)
		fprintf(stderr, "KinoWASM result: %u\n", (unsigned)result);

	return result == RES_SUCCESS ? 0 : 1;
}
```

### ポイント

- **`kw_core_install_default_mem_backend()` を忘れない**: WASM 線形メモリは OS 仮想メモリ (reserve+commit) で確保され、その実装はホストが注入する。未注入のまま memory を持つモジュールを動かすと `linear memory backend not installed` の fatal になる。リポジトリ内の全ホスト (`main.c` / `testsuite_runner.c` 等) が main 冒頭で呼んでいる
- **メモリは 3 段階に分かれる**:
  - `change_system_memory()` で **system memory バンク** を有効化 (kalloc グローバル状態を初期化)
  - `kinowasm_assign_memory()` で **store memory バンク** を渡す (フレーム / globals 等の永続データ)
  - `kinowasm_load_module(..., mod_info)` で **module memory バンク** を渡す (デコード IR)
- **解放はアプリ側の責任**。`kinowasm_term` は内部だけクリーンアップする。`store_buf` / `mod_buf` は自分で `free()`。
- `kinowasm_array_new_from(args, 0)` は **空配列の生成**。引数なしの関数を呼ぶときに使う。

メモリの割り当ては [Public-API.md §2-4](Public-API.md) を参照してください。

---

## 2. 関数を呼ぶ

### 引数 1 つ + 戻り値 1 つ

```c
/* fib(30) を呼んで結果を表示 */
kinowasm_args_t args = { 0 };
kinowasm_array_new_from(args, 1);
args.data[0].type        = TYPE_VAL_I32;
args.data[0].val.num.i32 = 30;

kinowasm_result_t res = kinowasm_invoke(store, "Mod", "fib", &args);
if (res == RES_SUCCESS) {
    printf("fib(30) = %d\n", args.data[0].val.num.i32);
} else {
    fprintf(stderr, "invoke failed: %u\n", res);
}
kinowasm_array_term_from(args);
```

`args` は **入出力共用**:
- 呼び出し前: 引数を `args.data[i]` に書く (要素数は引数の数)
- 呼び出し後: 戻り値で上書きされる (要素数は戻り値の数)

引数の型と戻り値の型が違う場合でも、配列のサイズはランタイム側で自動調整される (引数 2 個 → 戻り値 1 個など)。

> 💡 **毎フレーム同じ関数を呼ぶ場合** (ゲームの `update` 等): `kinowasm_invoke` は呼び出し毎にモジュール名・関数名を線形検索する。ロード時に `kinowasm_lookup_func(store, "Mod", "update", &handle)` で 1 回だけ解決してハンドルを保持し、毎フレームは `kinowasm_invoke_func(store, handle, &args)` (名前検索なし) で呼ぶと効率的。詳細は [Public-API.md §5](Public-API.md)。

### 複数引数 / 複数戻り値

```c
/* divmod(17, 5) → (商=3, 余り=2) */
kinowasm_args_t args = { 0 };
kinowasm_array_new_from(args, 2);
args.data[0].type        = TYPE_VAL_I32;
args.data[0].val.num.i32 = 17;
args.data[1].type        = TYPE_VAL_I32;
args.data[1].val.num.i32 = 5;

kinowasm_invoke(store, "Mod", "divmod", &args);
/* args は戻り値 2 個になっている */
printf("quot=%d rem=%d\n",
       args.data[0].val.num.i32,
       args.data[1].val.num.i32);
kinowasm_array_term_from(args);
```

### 値型一覧

| `type` 値 | 意味 | アクセス |
|----------|------|----------|
| `TYPE_VAL_I32` | 32 bit 整数 | `val.num.i32` |
| `TYPE_VAL_I64` | 64 bit 整数 | `val.num.i64` |
| `TYPE_VAL_F32` | 32 bit 浮動小数 | `val.num.f32` |
| `TYPE_VAL_F64` | 64 bit 浮動小数 | `val.num.f64` |
| `TYPE_FUNCREF` | 関数参照 | `val.ref` |
| `TYPE_EXTERNREF` | 外部参照 | `val.ref` |
| `TYPE_EXNREF` | 例外参照 (WASM 3.0 EH) | `val.ref` |
| `TYPE_VAL_V128` | 128 bit SIMD 値 | sidecar 格納 (`kargs.h` 参照) |

---

## 3. ホスト関数を追加

WASM 側から `(import "env" "log_int" (func (param i32)))` のように import している関数を C で実装します。

### 最小例

```c
#include "kinowasm.h"

/* ホスト関数の実装。WASM 側から呼ばれる */
static kinowasm_result_t host_log_int(kinowasm_callinfo_t* call)
{
    int32_t v = call->args->data[0].val.num.i32;
    printf("[wasm log] %d\n", v);
    return RES_SUCCESS;
}

/* ホスト関数を一括登録するヘルパー */
void register_my_host_funcs(void)
{
    static const kinowasm_extrafunc_t funcs[] = {
        { "env", "log_int", host_log_int, NULL },
    };
    kinowasm_register_extra_func(funcs, sizeof(funcs) / sizeof(funcs[0]));
}

int main(void)
{
    change_system_memory();
    register_my_host_funcs();              /* ← kinowasm_init より前 */
    kinowasm_handle_t store = kinowasm_init();
    /* ...あとは §1 と同じ... */
}
```

### 引数取得 / 戻り値設定のショートハンド

`extrafunction.c` で使われているショートハンドマクロを真似ると楽 (実物は引数名 `api_call` を参照する。以下は引数名 `call` 版):

```c
#define GETPARAM_INT(idx)         call->args->data[idx].val.num.i32
#define GETPARAM_FLOAT(idx)       call->args->data[idx].val.num.f32
#define SETRET_INT(idx, value)    do { call->rets->data[idx].type = TYPE_VAL_I32; \
                                       call->rets->data[idx].val.num.i32 = (value); } while (0)
#define SETRET_FLOAT(idx, value)  do { call->rets->data[idx].type = TYPE_VAL_F32; \
                                       call->rets->data[idx].val.num.f32 = (value); } while (0)
```

これを使うと `pow(a, b)` の host 実装はこう書けます:

```c
static kinowasm_result_t host_pow(kinowasm_callinfo_t* call)
{
    int32_t base = GETPARAM_INT(0);
    int32_t exp  = GETPARAM_INT(1);
    int32_t r = 1;
    for (int i = 0; i < exp; i++) r *= base;
    SETRET_INT(0, r);
    return RES_SUCCESS;
}
```

戻り値配列 (`call->rets`) の要素数とサイズはランタイム側で事前確保済みなので、`call->rets->data[i]` に書き込むだけで OK。

### 登録ポイント

| やりたいこと | 書き方 |
|--------------|--------|
| 標準ライブラリ風セットを登録 | `extrafunction.c` の `register_standard_func()` を真似る |
| 一部だけ自前で差し替えたい | 自分の登録テーブルだけ `kinowasm_register_extra_func` で追加 (重複登録は最後の勝ち) |
| 既存をクリアしてやり直したい | `kinowasm_clear_extra_func()` を呼ぶ |

詳細は [Host-Functions.md](Host-Functions.md)。

---

## 4. メモリにアクセス

WASM の linear memory (バイト addressable な仮想線形空間) をホスト側から読み書きします。

### WASM が渡してきたポインタ (i32) から文字列を読む

```c
static kinowasm_result_t host_print_str(kinowasm_callinfo_t* call)
{
    int32_t addr = GETPARAM_INT(0);    /* WASM 側のポインタ */

    /* NUL 終端まで長さを測る (範囲外に達したらエラーで打ち切り) */
    char c;
    int32_t len = 0;
    do {
        if (kinowasm_read_memory(call, addr + len, &c, 1) != RES_SUCCESS)
            return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;
        len++;
    } while (c != '\0');

    /* 一括読み込み + 表示 */
    char* buf = kinowasm_mem_malloc(len);
    if (buf == NULL)
        return ERR_OUTOFMEMORY;
    kinowasm_read_memory(call, addr, buf, len);   /* 上で範囲検証済み */
    printf("%s", buf);
    kinowasm_mem_free(buf);

    return RES_SUCCESS;
}
```

### ホストから WASM メモリへ書き込む

```c
static kinowasm_result_t host_get_username(kinowasm_callinfo_t* call)
{
    int32_t buf_addr  = GETPARAM_INT(0);   /* 書き込み先ポインタ */
    int32_t buf_size  = GETPARAM_INT(1);   /* バッファサイズ */
    const char* name  = "Alice";
    size_t len        = strlen(name) + 1;

    if (len > (size_t)buf_size) {
        SETRET_INT(0, -1);                 /* バッファ不足 */
        return RES_SUCCESS;
    }

    if (kinowasm_write_memory(call, buf_addr, name, len) != RES_SUCCESS) {
        SETRET_INT(0, -1);                 /* 範囲外ポインタ (転送されない) */
        return RES_SUCCESS;
    }
    SETRET_INT(0, (int32_t)len);           /* 書き込んだバイト数 */
    return RES_SUCCESS;
}
```

### 注意

- `kinowasm_read_memory` / `kinowasm_write_memory` は **`call` を受け取る**。これは現在実行中のインスタンスのメモリを引くため
- **範囲外アクセスは転送せずエラーを返す**。`address + len` が宣言済みメモリ (`num_pages × 64 KB`) を超える転送、およびメモリを宣言していないモジュールに対する読み書きは、何も転送せず `ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS` を返す (部分転送なし)。WASM 側から渡されたポインタを信用せず、戻り値をチェックすること
- ホスト関数の **外** (例: `main` 内) から WASM メモリを触るのは現状サポートされていない (callinfo が無いため)
- バッファは `kinowasm_mem_malloc` で取り、忘れずに `kinowasm_mem_free` (invoke 実行中は store memory バンクから確保される。[Host-Functions.md §6](Host-Functions.md))

詳細は [Host-Functions.md §6](Host-Functions.md) を参照してください。

---

## 5. 複数モジュール

ライブラリモジュール + メインモジュール構成のような場合、**順番にロード** + **同じ store** で動かします:

```c
kinowasm_handle_t store = kinowasm_init();
kinowasm_assign_memory(store, store_buf, 100 * 1024 * 1024);

/* 1. 先にライブラリをロード (依存される側) */
void* lib_mem = malloc(20 * 1024 * 1024);
kinowasm_load_module(store, "lib.wasm", "lib",
    kinowasm_mem_info_init(lib_mem, 20 * 1024 * 1024));

/* 2. メインモジュールをロード ("lib" を import している) */
void* main_mem = malloc(20 * 1024 * 1024);
kinowasm_load_module(store, "main.wasm", "main",
    kinowasm_mem_info_init(main_mem, 20 * 1024 * 1024));

/* 3. main の _start を呼ぶ */
kinowasm_args_t args = { 0 };
kinowasm_array_new_from(args, 0);
kinowasm_invoke(store, "main", "_start", &args);
kinowasm_array_term_from(args);

/* 後片付け */
kinowasm_term(store);
free(store_buf);
free(lib_mem);
free(main_mem);
```

### モジュール間 import の解決

`main.wasm` が `(import "lib" "add" (func ...))` していれば、ロード時に `store` に登録済みの `"lib"` モジュールから探索される。**先にロードしたほうがエクスポート側**。

### モジュール単位のアンロード

ライブラリ本体 (`kinowasm.h`) に単発の「アンロード」関数は無いが、ホスト層 `extrafunction.c` に **動的モジュール lifecycle API** がある: `create_wasm_module` / `push_wasm_module` / `invoke_wasm_module` / `pop_wasm_module` / `destroy_wasm_module`。push で積んだモジュールを LIFO で pop でき、push→invoke→pop の繰り返しでもリークしない (100,000 回の回帰テスト済み)。詳細は [Host-Functions.md §9](Host-Functions.md)。

LIFO で収まらない解放パターンが必要な場合は、従来どおり **store ごと作り直す** のが安全。

---

## 6. エラー処理

### 戻り値ベース

すべての `kinowasm_*` 関数は `kinowasm_result_t` (`uint32_t`) を返す:

```c
kinowasm_result_t res = kinowasm_invoke(store, "Mod", "fib", &args);
switch (res) {
case RES_SUCCESS:
    /* OK */
    break;
case ERR_TRAP_INTERGER_DIVIDE_BY_ZERO:    /* 120 */
    fprintf(stderr, "division by zero\n");
    break;
case ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS: /* 119 */
    fprintf(stderr, "memory access out of bounds\n");
    break;
default:
    fprintf(stderr, "wasm error: %u\n", res);
    break;
}
```

エラーコード一覧と意味は [Error-Handling.md §5](Error-Handling.md)。

### `_try` / `_catch` を使う書き方

`kinowasm.h` をインクルードすれば `_try`/`_catch`/`_throwiferr` も使えます:

```c
kinowasm_result_t do_work(kinowasm_handle_t store)
{
    _try {
        _throwiferr(kinowasm_load_module(store, "a.wasm", "a", mem_a));
        _throwiferr(kinowasm_load_module(store, "b.wasm", "b", mem_b));

        kinowasm_args_t args = { 0 };
        kinowasm_array_new_from(args, 0);
        _throwiferr(kinowasm_invoke(store, "b", "_start", &args));
        kinowasm_array_term_from(args);
    }
    _catch:
    return _result;
}
```

詳細とパターン集は [Error-Handling.md](Error-Handling.md)。

### トラップが起きたら

WASM 側で `unreachable` 命令を踏んだり、0 除算したり、メモリ範囲外アクセスをしたりすると、`kinowasm_invoke` は対応する `ERR_TRAP_*` を返します。フレームは破棄されないので、エラーをアプリ側で表示してから次の処理に進めます。

---

## 7. ビルド統合

### サブディレクトリとして組み込む

KinoRuntime ディレクトリを丸ごとサブディレクトリに置いて、自分の CMakeLists.txt で:

```cmake
add_subdirectory(third_party/KinoRuntime)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE KinoWASM)
target_include_directories(my_app PRIVATE
    third_party/KinoRuntime/KinoWASM
    third_party/KinoRuntime/KinoWASM/store       # 内部ヘッダ (extrafunction.c 等を流用する場合)
    third_party/KinoRuntime/KinoWASM/KinoUtil
    third_party/KinoRuntime/host
)

# ホスト関数 / システムメモリ実装も拾う場合
target_sources(my_app PRIVATE
    third_party/KinoRuntime/host/systemmemory.c
    third_party/KinoRuntime/host/extrafunction.c   # 標準 WASI 風 host 関数を流用したいとき
    third_party/KinoRuntime/host/winapi.c          # Windows プラットフォーム抽象化
    third_party/KinoRuntime/host/debug.c           # 計時 / DEBUG_LOG
)
```

> `extrafunction.c` の `register_standard_func()` をそのまま使うか、自前の host 関数だけにするかはアプリ次第。`extrafunction.c` は内部で `kw_store.h` を参照するため、これを含める場合は `KinoWASM/store` を include パスに入れる (最小構成で `kinowasm.h` だけ使うなら不要)。

> **線形メモリ backend の注入を忘れずに**: memory を持つモジュールを動かすホストは、main 冒頭で `kw_core_install_default_mem_backend()` (`kw_core_mem_backend_win.h`、`host/` 配下) を呼ぶ (§1 参照)。Windows 以外のプラットフォームでは同ヘッダを雛形に mmap 等で `kw_core_mem_backend_t` を実装して差し替える (ライブラリ側は無変更で済む)。

### ビルド済みの `.lib` をリンクする

すでに Release ビルドした `KinoWASM.lib` をリンクするだけでも OK:

```cmake
add_executable(my_app main.c
    third_party/KinoRuntime/host/systemmemory.c)
target_link_libraries(my_app PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/KinoRuntime/Bin/KinoWASM.lib"
)
target_include_directories(my_app PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/KinoRuntime/KinoWASM"
    "${CMAKE_SOURCE_DIR}/third_party/KinoRuntime/KinoWASM/KinoUtil"
    "${CMAKE_SOURCE_DIR}/third_party/KinoRuntime/host"
)
```

ランタイム本体 (parser / store / core / KinoUtil) はすべて `KinoWASM.lib` に含まれるため、公開 API (`kinowasm.h`) だけ使うなら include は `KinoWASM` と `KinoWASM/KinoUtil` で足ります。

---

## 8. よくある落とし穴

### "kinowasm_init failed" / NULL が返る

`change_system_memory()` を呼んでいない可能性。kalloc は **アクティブなバンクが無い状態** で `malloc` 系を呼ぶと NULL を返します。

```c
change_system_memory();      /* ← これを忘れない */
kinowasm_handle_t store = kinowasm_init();
```

### モジュールロードに失敗 (ERR_OUTOFMEMORY)

モジュールメモリバンクのサイズ不足。WASM のサイズ + デコード IR (元の数倍に膨らむ) を考慮して **20-100 MB ぐらい** 渡しておくのが無難。CoreMark 程度で 20 MB あれば足りる。

### ホスト関数が呼ばれない (ERR_UNKNOWN_IMPORT)

- 登録モジュール名 / 関数名と WASM 側の `(import ...)` の文字列が完全一致しているか確認
- 登録は **`kinowasm_init` の前** に (`extra_func_table` がグローバル状態のため、init 後でも実用上は OK だが慣習として init 前)
- 大文字小文字に注意

### testsuite_runner が `testsuite directory not found`

カレントディレクトリが `Bin/` でない可能性 ([Testing.md](Testing.md))。

### 性能改善のつもりで悪化

性能を比較するときは、同じ入力・ビルド構成・実行環境を使用し、繰り返し計測してください。

### スレッド利用

現状の API は **単一スレッド前提**。`kalloc` 自体は mutex で保護されるが、`current_kalloc` グローバル状態の切替が頻繁に起きるためマルチスレッドで同じ store を共有するとデータ競合が起きやすい。スレッドごとに別 store を作る運用が無難。

---

## 9. 参考リンク

| ドキュメント | 内容 |
|--------------|------|
| [Public-API.md](Public-API.md) | 関数別の網羅リファレンス |
| [Build-and-Test.md](Build-and-Test.md) | 開発環境のビルド方法 |
| [Error-Handling.md](Error-Handling.md) | エラーコードと `_try`/`_catch` |
| [Host-Functions.md](Host-Functions.md) | ホスト関数の詳細 |

ライブラリ提供のサンプル実装:

| ファイル | 何の参考になるか |
|---------|------------------|
| `main.c` | 最小限のフロントエンドの組み立て方 |
| `extrafunction.c` | 充実したホスト関数群 (WASI 風実装含む) |
| `Test/testsuite_runner.c` | 複数モジュール / リセット / WASI 風 spectest 関数の構造 |
| `Test/perf_invoke_func2.c` | 計時付き invoke ハーネス |
