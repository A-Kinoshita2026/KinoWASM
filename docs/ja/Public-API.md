# Public API リファレンス

[English](../Public-API.md) | 日本語


`KinoWASM/kinowasm.h` で公開されている C API のリファレンスです。すべての関数は静的ライブラリ `KinoWASM.lib` 経由で利用します。

手動で静的リンクする場合は `KinoWASM.lib` に加えて `KinoUtil.lib` が必要です。また、`wasi_get_argc` / `wasi_get_argv` / `wasi_get_envp` をホストで提供するか、これらを定義する `host/extrafunction.c` をリンクしてください。

## 1. ハンドル型と中核型

### `kinowasm_handle_t`

```c
typedef void* kinowasm_handle_t;
```

WASM ランタイムインスタンスへの不透明ハンドル。実態は `store_t*` (内部型)。`kinowasm_init()` で生成し、`kinowasm_term()` で破棄する。

### `kinowasm_args_t` / `kinowasm_arg_t` (kargs.h)

ホスト ⇔ WASM の引数 / 戻り値を表す動的配列。

```c
typedef enum {
    TYPE_VAL_I32   = 0x7F,
    TYPE_VAL_I64   = 0x7E,
    TYPE_VAL_F32   = 0x7D,
    TYPE_VAL_F64   = 0x7C,
    TYPE_VAL_V128  = 0x7B,   // SIMD 128-bit vector (KINOWASM_ENABLE_SIMD=ON 時)
    TYPE_FUNCREF   = 0x70,
    TYPE_EXTERNREF = 0x6F,
    TYPE_EXNREF    = 0x69,   // WASM 3.0 EH: exception reference (host からは不透明)
} kinowasm_valtype_t;

typedef union {
    int32_t i32;
    int64_t i64;
    float   f32;
    double  f64;
} kinowasm_num_t;

typedef union {
    kinowasm_num_t  num;
    kinowasm_ref_t  ref;
} kinowasm_val_t;

typedef struct {
    uint8_t        type;   // TYPE_VAL_* / TYPE_*REF
    kinowasm_val_t val;
} kinowasm_arg_t;

typedef kinowasm_array(kinowasm_arg_t) kinowasm_args_t;
```

> 💡 `kinowasm_val_t` は 8 byte 固定。v128 値はこの union に収まらないため、内部では
> 別領域 (sidecar) に格納される (`kinowasm_v128_t` = 16 byte union、kargs.h 参照)。
> ホスト ⇔ WASM の引数/戻り値で v128 を直接受け渡す API は未提供。

`kinowasm_args_t` の生成は karray API:

```c
kinowasm_args_t args = { 0 };
kinowasm_array_new_from(args, 2);          // 要素数 2 で確保
args.data[0].type        = TYPE_VAL_I32;
args.data[0].val.num.i32 = 42;
args.data[1].type        = TYPE_VAL_I32;
args.data[1].val.num.i32 = 7;
// ...
kinowasm_array_term_from(args);            // 解放
```

### `kinowasm_callinfo_t`

ホスト関数 (extra func) のコールバックに渡される構造体:

```c
typedef struct {
    kinowasm_args_t*  args;          // WASM 側からの引数
    kinowasm_args_t*  rets;          // ホストが書き込む戻り値
    kinowasm_handle_t current_store; // 呼び出し元 store
} kinowasm_callinfo_t;

typedef kinowasm_result_t (*kinowasm_extrafuncaddress_t)(kinowasm_callinfo_t* call);
```

### `kinowasm_extrafunc_t`

ホスト関数登録テーブルのエントリ:

```c
typedef struct {
    const char*                  module;    // インポートモジュール名 (例: "env", "wasi_snapshot_preview1")
    const char*                  name;      // 関数名
    kinowasm_extrafuncaddress_t  func;      // 実装ポインタ
    void*                        reserved;  // シグネチャ文字列 (NULL 可)
} kinowasm_extrafunc_t;
```

> `reserved` はヘッダ上は予約フィールドだが、`extrafunction.c` の慣習としてシグネチャ文字列 (`"iii"` 等) を格納し、host import の型検証に使用される。書式と検証の仕組みは [Host-Functions.md §2](Host-Functions.md) 参照。

### `kinowasm_mem_info_t` (kalloc.h)

カスタムアロケータ "バンク" を指す不透明ハンドル。`kinowasm_mem_info_init(buffer, size)` で初期化。

## 2. ライフサイクル API

### `kinowasm_init`

```c
kinowasm_handle_t kinowasm_init(void);
```

新しい store を確保して返す。失敗時は NULL。**呼び出し前に `change_system_memory()` 等で system memory バンクをアクティブにしておくこと** (kalloc が `current_kalloc` を参照するため)。

### `kinowasm_term`

```c
void kinowasm_term(kinowasm_handle_t s);
```

store の内部リソース (frames / stack / framepool / localpool、moduletable の各 moduleinst、失敗ロードで確保された orphan moduleinst 等) を解放する。NULL 渡し可。

> ⚠ store memory バッファ自体 (`kinowasm_assign_memory` で渡したもの) は呼び出し側が `free()` する。

### `kinowasm_reset_store`

```c
kinowasm_result_t kinowasm_reset_store(kinowasm_handle_t S);
```

実行中フレームを巻き戻し、stack を空にする。**ロード済みモジュールは保持**される。テスト間でクリーンな実行状態に戻したいときに使用。

## 3. メモリバンク管理

### `kinowasm_assign_memory`

```c
void kinowasm_assign_memory(kinowasm_handle_t S, void* memory, size_t size);
```

store memory バンクを設定する。内部で `kinowasm_mem_info_init(memory, size)` を呼んで `S->storememory` に保存。

```c
void* mem = malloc(100 * 1024 * 1024);
kinowasm_assign_memory(handle, mem, 100 * 1024 * 1024);
```

### `kinowasm_get_memory`

```c
void* kinowasm_get_memory(kinowasm_handle_t S);
```

現在の store memory バンクの先頭ポインタ (= `kinowasm_assign_memory` で渡した `memory`) を返す。

## 4. モジュールのロード

### `kinowasm_load_module` / `kinowasm_load_module_from_memory`

```c
kinowasm_result_t kinowasm_load_module(
    kinowasm_handle_t       S,
    const char*             modulefile,
    const char*             modulename,
    kinowasm_mem_info_t     modulememory);

kinowasm_result_t kinowasm_load_module_from_memory(
    kinowasm_handle_t       S,
    void*                   module_data,
    size_t                  module_size,
    const char*             modulename,
    kinowasm_mem_info_t     modulememory);
```

`modulefile` (or `module_data` / `module_size`) で指定した `.wasm` を decode → resolve_imports → instantiate → core コンパイル (+ start 実行) する。start の後、モジュールが `_initialize` (型 `() -> ()`) を export していれば **WASI reactor 規約として一度だけ自動実行** する (Emscripten `--no-entry` ビルドのグローバル ctor 発火。trap は start と同様 uninstantiable 扱いで load 失敗、[Host-Functions.md §7](Host-Functions.md))。

- **`modulename`**: `kinowasm_invoke` でこの名前を指定して呼び出す。他モジュールの import 解決先としても使われる。
- **`modulememory`**: モジュール専用の kalloc バンク。`malloc(N)` した先頭で `kinowasm_mem_info_init(buf, N)` を作って渡す。NULL を渡した場合は store memory が使われる。

> 💡 **モジュールごとに独立したバンク** を渡すのが推奨設計。アンロード相当が `free(buf)` だけで済む。

戻り値 `kinowasm_result_t`: `RES_SUCCESS` (0) / 各種 `ERR_*`。エラーコード一覧は [Error-Handling.md](Error-Handling.md)。

## 5. 関数呼出

### `kinowasm_invoke`

```c
kinowasm_result_t kinowasm_invoke(
    kinowasm_handle_t S,
    const char*       module,
    const char*       funcname,
    kinowasm_args_t*  argument);
```

`module::funcname` をエクスポートテーブルから検索して実行。`argument` は **入出力** で:

- **呼び出し前**: 引数を埋める
- **呼び出し後**: 戻り値で上書きされる

```c
kinowasm_args_t args = { 0 };
kinowasm_array_new_from(args, 1);
args.data[0].type        = TYPE_VAL_I32;
args.data[0].val.num.i32 = 30;

kinowasm_result_t res = kinowasm_invoke(handle, "Lib", "fibonacci", &args);
if (res == RES_SUCCESS) {
    int32_t fib30 = args.data[0].val.num.i32;
}
kinowasm_array_term_from(args);
```

呼び出し中にホスト関数が `RES_SUCCESS` 以外を返した場合、ランタイムが store に `STATE_FLAG_SUSPENDED` を立ててサスペンド状態にし、`kinowasm_invoke` は **そのホスト関数の戻り値** をエラーコードとして返す (再開には `kinowasm_resume`。詳細は [Host-Functions.md §8](Host-Functions.md))。

> ⚠ 未存在のモジュール名 / 関数名を指定した場合は **エラーを返す** (未存在モジュール `ERR_UNKNOWN_IMPORT_SYMBOL` / 未存在関数 `ERR_UNKNOWN_IMPORT` / export が非関数 `RES_ERROR`)。

### `kinowasm_invoke_n` (長さ明示版)

```c
kinowasm_result_t kinowasm_invoke_n(kinowasm_handle_t S, const char* module,
                                    const char* funcname, size_t funcname_len,
                                    kinowasm_args_t* argument);
```

`funcname` が NUL 終端でなくてもよい版 (`funcname_len` バイトを export 名と比較)。embedded NUL を含む export 名を扱うために使う。`module` は通常どおり NUL 終端文字列。

### `kinowasm_lookup_func` / `kinowasm_invoke_func` (解決済みハンドル呼出)

```c
typedef int32_t kinowasm_funcref_t;   // KINOWASM_FUNCREF_INVALID (-1) = 無効

kinowasm_result_t kinowasm_lookup_func(kinowasm_handle_t S, const char* module,
                                       const char* funcname, kinowasm_funcref_t* out);
kinowasm_result_t kinowasm_lookup_func_n(kinowasm_handle_t S, const char* module,
                                         const char* funcname, size_t funcname_len,
                                         kinowasm_funcref_t* out);
kinowasm_result_t kinowasm_invoke_func(kinowasm_handle_t S, kinowasm_funcref_t func,
                                       kinowasm_args_t* argument);
```

`kinowasm_invoke` は呼び出し毎にモジュール名 (O(モジュール数)) と関数名 (O(export数)) を線形検索する。**毎フレーム同じ関数を呼ぶ**ようなケースでは、`kinowasm_lookup_func` でロード時に **1 回だけ解決** してハンドル (`kinowasm_funcref_t`) を取得し、以降は `kinowasm_invoke_func` で **名前検索なし (O(1))** に呼び出す:

```c
kinowasm_funcref_t update_fn;
if (kinowasm_lookup_func(store, "Game", "update", &update_fn) != RES_SUCCESS) { /* error */ }
// 毎フレーム:
res = kinowasm_invoke_func(store, update_fn, &args);   // 文字列比較ゼロ
```

- `kinowasm_invoke` / `kinowasm_invoke_n` は内部で `kinowasm_lookup_func_n` + `kinowasm_invoke_func` を呼ぶ薄いラッパー。
- ハンドルの **有効期間** は解決元モジュールがロードされている間。`kinowasm_reset_store` やモジュール再ロード後は **再取得が必要**。`kinowasm_invoke_func` はハンドルが現在の store の関数範囲外なら `ERR_INVALID_FUNC_PARAM` を返す (別モジュール再ロードで同 index が別関数を指すケースまでは検出しないため、ハンドルは再ロードを跨いで使い回さないこと)。

### `kinowasm_resume`

```c
kinowasm_result_t kinowasm_resume(kinowasm_handle_t S, kinowasm_args_t* argument);
```

サスペンド中の store を再開する。事前に `STATE_FLAG_SUSPENDED` がセットされている必要がある (`ERR_NOT_RESUME` で失敗)。

`argument` には **直前の `kinowasm_invoke` で受け取った args をそのまま** 渡す (戻り値スロットはそのまま使う)。

## 6. ホスト関数 (extra func)

### `kinowasm_register_extra_func`

```c
kinowasm_result_t kinowasm_register_extra_func(
    const kinowasm_extrafunc_t* extra,
    size_t                      size);
```

ホスト関数を一括登録する。`extra[0..size-1]` の各エントリが 1 つの import 関数を表す。

```c
static kinowasm_result_t my_print_i32(kinowasm_callinfo_t* call) {
    int32_t v = call->args->data[0].val.num.i32;
    printf("%d\n", v);
    return RES_SUCCESS;
}

static const kinowasm_extrafunc_t my_funcs[] = {
    { "env", "print_i32", my_print_i32, NULL },
};
kinowasm_register_extra_func(my_funcs, 1);
```

> 戻り値の書き込みは `call->rets->data[i]` に対して行う。`rets` の要素数とサイズは事前にランタイム側で確保済み。

### `kinowasm_clear_extra_func`

```c
void kinowasm_clear_extra_func(void);
```

登録済み extra func テーブルを破棄する。

> グローバル状態のためマルチスレッド利用には別途排他が必要。

## 7. ホストからのメモリアクセス

WASM のリニアメモリへのホスト側アクセス用 (主に host 関数内で使う):

### `kinowasm_read_memory`

```c
kinowasm_result_t kinowasm_read_memory(
    kinowasm_callinfo_t* call,
    uint32_t             start_address,
    void*                data,
    size_t               len);
```

WASM linear memory の `[start_address, start_address+len)` をホストの `data` バッファにコピーする。core エンジンのホスト関数呼び出し中は実行中インスタンスの flat メモリを直接読む。

戻り値: 全 `len` バイトの転送に成功すると `RES_SUCCESS`。`[start_address, start_address+len)` が宣言済みメモリ範囲 (`num_pages * 64KiB`) を超える場合、または対象モジュールがメモリを宣言していない場合は **転送を行わず** `ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS` を返す (部分コピーはされない = all-or-nothing)。

### `kinowasm_write_memory`

```c
kinowasm_result_t kinowasm_write_memory(
    kinowasm_callinfo_t* call,
    uint32_t             start_address,
    const void*          data,
    size_t               len);
```

逆方向。`data` を WASM メモリへ書き込む。戻り値の規約は `kinowasm_read_memory` と同じ (範囲外・メモリ未宣言は転送せず `ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS`、部分書き込みなし)。

> 💡 両関数とも元は `void` 戻りだった。`kinowasm_result_t` 返却への変更は後方互換で、戻り値を無視する既存のホスト関数はそのまま動作する。新規コードではエラーチェックを推奨 (未初期化バッファを長さ・パスとして使う脆弱性の防止になる)。

## 8. 設定 API

### `kinowasm_set_stack_size`

```c
void kinowasm_set_stack_size(uint32_t size);
```

パーサ用スタックの初期サイズを設定。既定 `4096 * 16` (= 65536)。**`kinowasm_init` 前に呼ぶこと**。

### `kinowasm_set_object_cache_size`

```c
void kinowasm_set_object_cache_size(uint32_t size);
```

framepool / localpool 等のオブジェクト再利用キャッシュ容量。既定 `100`。

## 9. 最小限の使用例

```c
#include "kinowasm.h"
#include "systemmemory.h"

int main(void) {
    change_system_memory();                              // システムメモリバンクを有効化
    register_standard_func();                            // host 関数登録 (extrafunction.c が提供)

    kinowasm_handle_t store = kinowasm_init();

    void* store_mem = malloc(100 * 1024 * 1024);
    kinowasm_assign_memory(store, store_mem, 100 * 1024 * 1024);

    void* module_mem = malloc(20 * 1024 * 1024);
    kinowasm_mem_info_t mod_info = kinowasm_mem_info_init(module_mem, 20 * 1024 * 1024);

    kinowasm_result_t res = kinowasm_load_module(
        store, "Library.wasm", "Lib", mod_info);
    if (res != RES_SUCCESS) { /* error */ }

    kinowasm_args_t args = { 0 };
    kinowasm_array_new_from(args, 1);
    args.data[0].type        = TYPE_VAL_I32;
    args.data[0].val.num.i32 = 30;

    res = kinowasm_invoke(store, "Lib", "fibonacci", &args);
    if (res == RES_SUCCESS) {
        printf("fib(30) = %d\n", args.data[0].val.num.i32);
    }

    kinowasm_array_term_from(args);
    kinowasm_term(store);
    free(store_mem);
    free(module_mem);
    return 0;
}
```

## 10. 関連ドキュメント

- [Error-Handling.md](Error-Handling.md) — `_try` / `_catch` / エラーコード一覧
- [Host-Functions.md](Host-Functions.md) — ホスト関数 (extra func / WASI) の登録
- [Build-and-Test.md](Build-and-Test.md) — ライブラリのビルドと試験
