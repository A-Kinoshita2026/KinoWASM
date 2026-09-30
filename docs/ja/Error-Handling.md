# エラー処理

[English](../Error-Handling.md) | 日本語


KinoWASM の **通常のエラー伝播** は **マクロベースの簡易例外** で統一しています。ここでは `setjmp/longjmp` は使わず、関数内に固定ラベル `_catch:` を置いて `goto` で処理を集約する方式です。

定義は `KinoUtil/exception.h` (~37 行)。

> `setjmp/longjmp` は通常のエラー伝播とは独立した 2 つの経路 (core コンパイル中の致命エラー巻き戻し / standalone driver の WASI `proc_exit` 脱出) でのみ使います。§9 を参照。

## 1. 基本マクロ

```c
typedef uint32_t kinowasm_result_t;
typedef enum {
    RES_SUCCESS = 0,
    RES_ERROR   = 1,
} kinowasm_resultcode_t;

#define _is_error(err)            ((err) > RES_SUCCESS)
#define _try                      kinowasm_result_t _result = RES_SUCCESS;
#define _throw(err)               do { _result = (err); goto _catch; } while (0)
#define _throwif(err, condition)  do { if (condition) { _throw(err); } } while (0)
#define _throwiferr(instr)        do { kinowasm_result_t _errcode = (instr); \
                                       if (_errcode != RES_SUCCESS) _throw(_errcode); } while (0)
```

### `_try`

```c
_try { /* ... */ }
```

実体は **関数頭で `kinowasm_result_t _result = RES_SUCCESS;` を宣言する** だけのマクロ。中括弧は単なる視覚的グルーピングで、構文上は普通のブロック。

### `_catch:`

`_throw*` は `goto _catch;` で飛ぶため、各関数に **`_catch:` ラベルが必要**。慣習的にその直後に `return _result;` を置く。

### `_throw(err)`

`_result = err; goto _catch;` で即座に脱出。`err` には `RES_ERROR`、`ERR_*` 系コード、もしくは別の `_result` を入れる。

### `_throwif(err, condition)`

`condition` が真なら `_throw(err)`。NULL チェック / 範囲チェック / 妥当性検証で多用。

### `_throwiferr(call)`

別の `kinowasm_result_t` を返す関数を呼び、エラーならそのコードでそのまま再 throw。**最も頻繁に使う**マクロ。

## 2. テンプレート

すべての非 void 関数はこのパターンに従う:

```c
kinowasm_result_t do_something(input_t* in)
{
    _try {
        _throwif(ERR_NULLPOINTER, in == NULL);
        _throwif(ERR_OUTOFMEMORY, !some_alloc());

        _throwiferr(load_subresource(in));      // 別の result 関数の呼び出し

        if (in->kind == BAD)
            _throw(ERR_INVALID_FUNC_PARAM);

        // 正常系
    }
    _catch:
    return _result;
}
```

ポイント:
- `_try` は **必ず関数頭** に置く (block scope の関係で)。
- `_catch:` は `_try` 内ではなく **その外側** にある (goto はブロックを飛び越える)。
- 戻り値は `return _result;` で集約。`return ERR_FOO;` を直接書くのは推奨されない (cleanup の一貫性が崩れる)。

## 3. cleanup を伴うパターン

C 言語に try/finally は無いため、解放は `_catch:` の直前 / 直後で行う:

```c
kinowasm_result_t f(void)
{
    foo_t* foo = NULL;
    bar_t* bar = NULL;
    _try {
        foo = make_foo();
        _throwif(ERR_OUTOFMEMORY, foo == NULL);

        bar = make_bar();
        _throwif(ERR_OUTOFMEMORY, bar == NULL);

        _throwiferr(use(foo, bar));
    }
    _catch:
    if (bar != NULL) free_bar(bar);
    if (foo != NULL) free_foo(foo);
    return _result;
}
```

`_catch:` には正常系・異常系のどちらも到達するので、両方で実行されるべき cleanup を置く。

> 配列 / 動的メモリは `kinowasm_array_term`, `kinowasm_mem_free` 等が **NULL を渡しても安全** (no-op) なので、unconditional に呼んで OK。

> ⚠ `_catch:` の直後に `_throw` を含むマクロ (例: 内部で条件次第で throw する `CHANGE_STORE_MEMORY`) を置くと、失敗時に `_catch:` へ戻り続ける **無限ループ** になる。`_catch:` 以降では throw し得ない処理だけを書くこと。

## 4. ネストはできない

`_result` という単一変数を再利用する設計のため、関数内で複数回 `_try` を書くと挙動が壊れる。**1 関数 1 _try** が原則。中で別の関数を呼ぶ場合は当然 OK。

## 5. エラーコード一覧

エラーコードは 2 つの enum に分かれており、どれも `kinowasm_result_t` (uint32_t) として返される。

### システム共通 (`KinoUtil/exception.h`)

| 値 | 名前 | 意味 |
|----|------|------|
| 0 | `RES_SUCCESS` | 成功 |
| 1 | `RES_ERROR` | 一般エラー (詳細不明 / 内部不整合) |
| 10 | `ERR_NULLPOINTER` | NULL 引数 |
| 11 | `ERR_OUTOFMEMORY` | アロケータ枯渇 |
| 12 | `ERR_UNEXPECTED_END` | バッファ末端で予期しない EOF |
| 13 | `ERR_INTEGER_CONVERT_TOO_LONG` | LEB128 が想定 bits を超過 |
| 14 | `ERR_INTEGER_TOO_LARGE` | LEB128 結果が型範囲外 |
| 15 | `ERR_INVALID_ITEMSIZE` | karray の要素サイズ 0 |
| 16 | `ERR_NOT_SUPPORTED` | 未実装機能 |

### KinoWASM 固有 (`KinoWASM/exceptioncode.h`)

| 値 | 名前 | 意味 |
|----|------|------|
| 100 | `ERR_FILENOTOPEN` | `.wasm` ファイル open 失敗 |
| 101 | `ERR_HEADERNOTDETECT` | マジックナンバー不一致 (`\0asm`) |
| 102 | `ERR_VERSIONMISMATCH` | WASM version != 1 |
| 103 | `ERR_MAGICNOTDETECT` | 構造体マジック不一致 |
| 104 | `ERR_FUNCTION_COUNT_MISSMATCH` | function / code セクション数不一致 |
| 105 | `ERR_TOO_MANY_LOCALS` | locals 数が制限超過 |
| 106 | `ERR_NOTOPEND` | ブロックが END 前に終了 |
| 107 | `ERR_ZEROBYTEEXPECTED` | 予約バイトが 0 でない |
| 108 | `ERR_DATASECTIONREQUIRED` | datacount で要求された data section が無い |
| 109 | `ERR_UNKNOWN_IMPORT_KIND` | import の kind 値が不正 |
| 110 | `ERR_MALFORMED_MUTABILITY` | global の mut フラグ不正 |
| 111 | `ERR_MALFORMED_REFERENCE_TYPE` | reftype 値が不正 |
| 112 | `ERR_MISMATCH_SECTION_SIZE` | セクションサイズと実消費量が一致せず |
| 113 | `ERR_STACK_OVERFLOW` | 値スタック / フレームスタック満杯 |
| 114 | `ERR_TRAP_UNREACHABLE` | `unreachable` 命令を実行 |
| 115 | `ERR_TRAP_UNDEFINED_ELEMENT` | call_indirect で未定義 element |
| 116 | `ERR_TRAP_UNINITIALIZED_ELEMENT` | element が未初期化 |
| 117 | `ERR_TRAP_INDIRECT_CALL_TYPE_MISMATCH` | call_indirect の型不一致 |
| 118 | `ERR_TRAP_OUT_OF_BOUNDS_TABLE_ACCESS` | table アクセス範囲外 |
| 119 | `ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS` | linear memory 範囲外 |
| 120 | `ERR_TRAP_INTERGER_DIVIDE_BY_ZERO` | 整数 0 除算 |
| 121 | `ERR_TRAP_INTERGET_OVERFLOW` | 整数オーバーフロー (signed div / trunc) |
| 122 | `ERR_TRAP_INVALID_CONVERSION_TO_INTERGER` | NaN → 整数変換 |
| 123 | `ERR_NOTMODULEINIT` | store / module が未初期化 |
| 124 | `ERR_UNKNOWN_IMPORT` | import の名前が見つからない |
| 125 | `ERR_INCOMPATIBLE_IMPORT_TYPE` | import 型不一致 |
| 126 | `ERR_INVALID_FUNC_PARAM` | 関数引数の型 / 値不正 (funcref ハンドル範囲外含む) |
| 127 | `ERR_UNKNOWN_IMPORT_SYMBOL` | import モジュール名が見つからない |
| 128 | `ERR_OUTOFFUNCTION_TABLE` | 関数テーブル外参照 |
| 129 | `ERR_NOT_RESUME` | resume 不可 (suspended でない) |
| 130 | `ERR_FUNCTION_ALREADY_SUSPENDED` | 既に suspended な store に invoke |
| 131 | `ERR_MALFORMED_LOCALVAL` | locals 宣言不正 |
| 132 | `ERR_MALFORMED_BLOCK` | ブロック構造不正 |
| 133 | `ERR_MALFORMED_FUNC` | デコード時の内部不整合 (本体スクラッチの memory_count underflow 等の早期検知) |
| 134 | `ERR_MALFORMED_TABLE` | table 定義不正 |
| 135 | `ERR_MALFORMED_0XFC` | 0xFC プレフィックス命令の引数不正 |
| 136 | `ERR_MALFORMED_GLOBAL` | global 定義不正 |
| 137 | `ERR_MALFORMED_STORE` | store 命令の align/offset 不正 |
| 138 | `ERR_MALFORMED_MEMORY` | memory 定義不正 |
| 139 | `ERR_TYPE_MISMATCH` | 型スタックの型が opcode と不一致 |
| 140 | `ERR_MALFORMED_ALIGN` | load/store の align ヒント不正 |
| 141 | `ERR_MALFORMED_UTF8` | name セクション等の UTF-8 不正 |
| 142 | `ERR_MALFORMED_LANE` | SIMD lane インデックス不正 |
| 143 | `ERR_ATOMIC_WAIT_NON_SHARED` | shared でない memory 上での `atomic.wait` trap |
| 144 | `ERR_TRAP_UNALIGNED_ATOMIC` | atomic load/store/rmw のアラインメント不一致 trap |
| 145 | `ERR_TRAP_UNCAUGHT_EXCEPTION` | catch されなかった例外 (EH) の trap |
| 146 | `ERR_WASM_EXCEPTION` | throw された WASM 例外がどの try/catch にも捕捉されず関数外へ伝播した (core の trap "wasm exception" / "null exnref" の写像) |
| 147 | `ERR_FEATURE_DISABLED` | `features.h` で OFF にされた機能を含むモジュールの load |

### 値の連続性

範囲は手動管理で **歯抜けなし** が原則。新しいコードを追加するときは末尾に追加して `kinowasm_errorcode_t` enum の値を増やす。

## 6. ホスト関数からの戻り

ホスト関数 (`kinowasm_extrafuncaddress_t`) は `kinowasm_result_t` を返す。`RES_SUCCESS` 以外を返すと runtime 側でそのエラーを伝播する:

```c
static kinowasm_result_t my_open(kinowasm_callinfo_t* call)
{
    if (call->args->len < 1)
        return ERR_INVALID_FUNC_PARAM;
    // ...
    return RES_SUCCESS;
}
```

> ホスト関数内では `_try`/`_catch` を使っても問題ない (関数スコープ完結)。WASI 実装 (extrafunction.c) は WASI errno (例: `__WASI_EFAULT`) を戻り値スロットに書き、関数自体は `RES_SUCCESS` を返すスタイルが多い。

## 7. デバッグ Tips

- **Release ビルドでは `DEBUG_LOG` が no-op に潰されている** ため、エラー経路をトレースしたいときは `_throwif` の直前に **`printf` を直接書く** のが無難。`DEBUG_LOG` 経由だと「サイレント死」に見えて原因特定が難しくなる。
- `_result` の値 (10〜147) からエラー箇所を特定するには本ドキュメントの表を参照。`KinoRuntime.exe` は終了時に `EndCode: <数値>` を出力する。
- **パーサの自己整合性違反**: `ERR_MALFORMED_FUNC (133)` で停止する経路は parser の内部不整合チェック (本体スクラッチの `memory_count` underflow 等)。

## 8. OOM 安全性の設計意図

KinoWASM はランタイム稼働中の動的メモリを **`kinowasm_mem_malloc` (kalloc) のプール**から確保し、ホスト OS の `malloc`/`VirtualAlloc` を踏まないようにしている。プール自体は起動時にホストから一括で渡される (`kinowasm_assign_memory`)。

**意図:**
- ホスト OS のメモリ枯渇でランタイムが終了処理すらできなくなる事態を防ぐ。プール枯渇は `kinowasm_mem_malloc` が `NULL` を返すだけなので、`_throwif(ERR_OUTOFMEMORY, p == NULL)` で確実に巻き戻せる。
- OS `malloc` の挙動 (Linux の OOM killer 等) に依存せず、組み込み環境でも決定論的に動作する。

**実装上の前提:**
- `kinowasm_mem_free(NULL)` は no-op。`kinowasm_array_term` も `data == NULL` で safe。
- 複数段階の確保を行う関数では、各段で `_throw` した場合に直前の段が `_catch:` で巻き戻ることを保証する。特に、確保途中の frame / localpool は `kw_return_frame` / `kw_return_localpool` でプールへ戻す (返し忘れるとプールが徐々に枯渇する)。
- インスタンス化に失敗した moduleinst は破棄せず **orphan list** (`kw_orphan_moduleinst`) に積み、`kinowasm_term` でまとめて `kw_free_moduleinst` する。部分確保された address 配列や exports 名も NULL 許容 free で安全に解放される (WASM store は monotonic で、失敗しても副作用適用済みの funcref 等は生存させる必要があるため即時破棄しない)。

**監査時の注目点:**
- 部分確保資源 (open fd, push 済み frame, malloc 済み buffer) を `_catch:` で必ず巻き戻すか
- OOM 経路で残る partial-init オブジェクトを `kinowasm_term` が安全に解放できるか
- 将来 OOM 注入テスト (`kinowasm_mem_malloc` を N 回目で NULL を返すモード) を入れて全経路を踏破する余地

## 9. setjmp/longjmp を使う例外的経路

上記のマクロ例外は通常のエラー伝播用で `setjmp/longjmp` を使わない。ただし **通常のエラー伝播とは独立した 2 つの経路** でのみ setjmp/longjmp を使う:

1. **core コンパイル中の致命エラー巻き戻し** (`KinoWASM/core/kw_core_bridge.c`)
   - `kw_core_build_from_store` が各関数を bytecode にコンパイルする最中に、未対応構造等の致命エラー (`core_fatal`) を検出したら `longjmp(g_core_build_jmp, 1)` する。
   - build 開始時に `setjmp(g_core_build_jmp)` した地点へ戻り、load を失敗 (-1) として graceful に返す。`g_core_build_active` 中のみ longjmp し、実行時の致命エラーは従来どおり `exit` する。
   - 深いコンパイルスタックを一気に巻き戻すためで、`kinowasm_result_t` の逐次伝播では扱いにくい経路をカバーする。

2. **standalone driver の WASI `proc_exit` 脱出** (`KinoWASM/core/kw_core_wasi.c` + `Test/core/kw_core_main.c`)
   - WASI `proc_exit(code)` は WASM 実行を即時終了させる。standalone driver (`kw_core_run` 等) は `main` で `setjmp(g_exit_jmp)` して `g_exit_jmp_armed = 1` を立てる。
   - `proc_exit` は `g_exit_jmp_armed` が立っていれば `longjmp(g_exit_jmp, 1)` で `_start` の呼出元 (`main`) へ脱出する。
   - **公開 API (`kinowasm_invoke`) 経由では `g_exit_jmp` は setjmp されておらず**、longjmp しない (この場合は trap 相当として通常の機構で扱う)。未 setjmp のまま longjmp すると AV になるため、`g_exit_jmp_armed` ガードは必須。

## 10. 関連ドキュメント

- [Public-API.md](Public-API.md) — 公開 API の戻り値
- `KinoUtil/exception.h` / `KinoWASM/exceptioncode.h` — マクロとエラーコードの定義原本
