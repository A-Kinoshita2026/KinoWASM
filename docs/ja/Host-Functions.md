# ホスト関数 (extra func)

[English](../Host-Functions.md) | 日本語


WASM モジュールの import (`(import "env" "console" ...)` 等) に対する **ホスト側実装** を登録する仕組みです。実装は `extrafunction.c` (リファレンス実装、WASI 風 host 関数を含む) と `KinoWASM/kinowasm.c` (登録/呼出基盤)。

## 1. 全体像

```
WASM (.wasm)                       KinoRuntime (host)
─────────────                      ──────────────────
(import "env" "Pow" (func ...))    kinowasm_register_extra_func({
                                       { "env", "Pow", cmdpow, NULL }
                                   });

(call $Pow (i32.const 2) (i32.const 8))
        │
        ▼
   core エンジンが引数を slot に確定して call を実行
        │
        ▼ import 関数 → core_call_host (core/kw_core_wasi.c)
   kw_core_invoke_host (kinowasm.c) が extra_func_table を module+name で検索
        │
        ▼ args / rets を kinowasm_args_t に整形
   ホスト関数 cmdpow(callinfo) が呼ばれる
        │
        ▼ rets[0] に結果を書き込み
   結果が core の register-TOS (r0) に戻り WASM 側へ返る
```

## 2. 登録 API

### `kinowasm_extrafunc_t`

```c
typedef struct {
    const char*                  module;    // インポートモジュール名
    const char*                  name;      // 関数名
    kinowasm_extrafuncaddress_t  func;      // 実装関数ポインタ
    void*                        reserved;  // シグネチャ文字列 (import 検証に使用、NULL で検証スキップ)
} kinowasm_extrafunc_t;

typedef kinowasm_result_t (*kinowasm_extrafuncaddress_t)(kinowasm_callinfo_t* call);
```

`reserved` には `extrafunction.c` の慣習として **シグネチャ文字列** を入れる。書式は **1 文字目が戻り値型、2 文字目以降が引数型** (`'v'`=void, `'i'`=i32, `'j'`=i64, `'f'`=f32, `'d'`=f64)。例: `"vi"` = `void(i32)`、`"iii"` = `i32(i32, i32)`。`:` 区切りで複数候補を並べられる (例: `"ii:ij"`)。

この文字列は `validate_function_parameter` (extrafunction.c) が **import 検証に実際に使用する**: `invoke_wasm_module` (§9) が load 後の初回 invoke 時に、import された各 host 関数の WASM 側 functype とシグネチャ文字列を照合し、不一致なら `INVALID_ARGUMENT` で弾く。`reserved = NULL` のエントリは検証をスキップする。

### `kinowasm_register_extra_func`

```c
kinowasm_result_t kinowasm_register_extra_func(
    const kinowasm_extrafunc_t* extra,
    size_t                      size);
```

複数エントリを **一括登録**。複数回呼ぶと追加される (再登録ではない)。`module` + `name` 一致のリンク解決はモジュールロード時 (`kinowasm_load_module`) に行われる。

> 登録は **`kinowasm_init` の前** に行うのが慣習 (`main.c` のパターンに従う)。実装上は load_module の resolve_imports 時点で参照されるため、それまでに揃っていれば良い。

### `kinowasm_clear_extra_func`

```c
void kinowasm_clear_extra_func(void);
```

登録済みテーブルを破棄する。マルチテストや再初期化時に使う。**グローバル状態** のためマルチスレッド利用には別途排他が必要。

## 3. コールバックインタフェース

### `kinowasm_callinfo_t`

```c
typedef struct {
    kinowasm_args_t*  args;          // WASM → ホスト の引数
    kinowasm_args_t*  rets;          // ホスト → WASM の戻り値 (事前確保済み)
    kinowasm_handle_t current_store; // 予約 (現行 core では常に NULL)
} kinowasm_callinfo_t;
```

- `args->data[i].val.num.i32` 等で引数を取得
- `rets->data[i].type` と `rets->data[i].val.*` を **必ずセット** する (橋渡しは型タグを検査せず値をそのまま register-TOS へ戻すため、セット漏れは未初期化値が WASM 側に渡る)
- `current_store` は **現行 core エンジンでは常に NULL** (`kinowasm.c` の callinfo 構築参照)。デリファレンスしないこと。`kinowasm_read_memory` / `kinowasm_write_memory` は callinfo を渡せば実行中 store の flat mem に対して動作する (§6)

### 戻り値

ホスト関数は `kinowasm_result_t` を返す:
- `RES_SUCCESS` (0) → 成功、結果が呼び出し元に戻る
- それ以外 → ランタイムが `_throw` して `kinowasm_invoke` がそのコードで失敗 (§8 のサスペンドもこの経路)

`extrafunction.c` の独自エラーコード:
```c
typedef enum {
    ERR_SEGMENTATION_FAULT = 201,
    ERR_ALIGNMENT_FAULT    = 202,
    INVALID_ARGUMENT       = 203,
} error_code_t;
```

## 4. ボイラープレートを減らすマクロ

`extrafunction.c` は短縮マクロを定義:

```c
typedef kinowasm_callinfo_t* api_call_t;

#define GETPARAM_INT(index)        api_call->args->data[index].val.num.i32
#define GETPARAM_FLOAT(index)      api_call->args->data[index].val.num.f32
#define SETRET_INT(index, value)   do { api_call->rets->data[index].type = TYPE_VAL_I32; api_call->rets->data[index].val.num.i32 = value; } while (0)
#define SETRET_FLOAT(index, value) do { api_call->rets->data[index].type = TYPE_VAL_F32; api_call->rets->data[index].val.num.f32 = value; } while (0)
```

ホスト関数を新規追加するときはこれらをそのまま使うのが手早い。

## 5. 最小例

```c
static kinowasm_result_t cmdpow(api_call_t api_call)
{
    int32_t n1 = GETPARAM_INT(0);
    int32_t n2 = GETPARAM_INT(1);
    int32_t r  = 1;
    for (int i = 0; i < n2; i++)
        r *= n1;

    SETRET_INT(0, r);
    return 0;
}

void register_standard_func(void)
{
    const kinowasm_extrafunc_t table[] = {
        { SYSTEM_FUNCTION, "Pow", cmdpow, (void*)"iii" },
    };
    kinowasm_register_extra_func(table, sizeof(table) / sizeof(table[0]));
}
```

`SYSTEM_FUNCTION` は `kinowasm.h` で `"env"` に定義。emscripten 系 wasm はこの名前で import する。

## 6. WASM メモリへのアクセス

ホスト関数から WASM 側のリニアメモリを読み書きするには [Public-API.md §7](Public-API.md) の関数を使う:

```c
// 文字列を WASM メモリから読んで printf する例 (extrafunction.c の console)
static kinowasm_result_t console(api_call_t api_call)
{
    uint32_t t = (uint32_t)GETPARAM_INT(0);
    // NUL までの長さを数える。範囲外 read は転送されず c が変化しないため、
    // 毎回 0 初期化して範囲外を終端扱いにする (無限ループ回避)。上限 64KB。
    uint32_t count;
    char c = 0;
    for (count = 0; count < 0x10000; count++) {
        c = 0;
        kinowasm_read_memory(api_call, t + count, &c, sizeof(c));
        if (c == '\0')
            break;
    }
    char* str = kinowasm_mem_malloc(count + 1);
    if (str == NULL)
        return 0;                          // best-effort。確保失敗は no-op
    kinowasm_read_memory(api_call, t, str, count);
    str[count] = '\0';
    printf("%s\n", str);
    kinowasm_mem_free(str);
    return 0;
}
```

ポイント:
- `kinowasm_read_memory` / `kinowasm_write_memory` は宣言ページ数 (`num_pages * WASM_PAGE_SIZE`) で範囲検査され、`address + len` が範囲外なら **何も転送せず** `ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS` を返す (部分転送なし = all-or-nothing)。memory を宣言しないモジュールも同様 ([Public-API.md §7](Public-API.md))
- 範囲外 read では転送先が変化しないので、`c` を **毎回 0 初期化** して範囲外バイトを終端扱いにし、上限 (64KB) と併せて無限ループを防ぐ
- WASM の文字列は通常 NUL 終端だが、長さが事前に分からないので 1 byte ずつスキャン
- 読み出しバッファの `kinowasm_mem_malloc` は、invoke 実行中は **store memory バンク** がアクティブなためそこから確保される (invoke 入口の `CHANGE_STORE_MEMORY` 参照)
- 解放を忘れると store memory が断片化・枯渇する (host 関数が頻繁に呼ばれるアプリで顕著)

## 7. WASI 風 host 関数

`extrafunction.c` には `wasi_snapshot_preview1` モジュール用の WASI 実装が同梱されています:

| 関数 | シグネチャ | 用途 / 実装状況 |
|------|-----------|----------------|
| `fd_close` | `(i32) -> i32` | fd クローズ (0/1/2 は no-op、preopen は閉じない、通常 file は fclose) |
| `fd_fdstat_get` | `(i32, i32) -> i32` | fd 状態取得 (CHARACTER_DEVICE / DIRECTORY / REGULAR_FILE) |
| `fd_seek` | `(i32, i64, i32, i32) -> i32` | seek (file fd で SEEK_SET/CUR/END、stdio は ESPIPE) |
| `fd_tell` | `(i32, i32) -> i32` | 現在 offset |
| `fd_write` | `(i32, i32, i32, i32) -> i32` | 書き込み (stdout/stderr / file fd) |
| `fd_read` | `(i32, i32, i32, i32) -> i32` | 読み込み (stdin / file fd) |
| `fd_pread` | `(i32, i32, i32, i64, i32) -> i32` | offset 指定 read |
| `fd_pwrite` | `(i32, i32, i32, i64, i32) -> i32` | offset 指定 write |
| `fd_filestat_get` | `(i32, i32) -> i32` | fd の `stat()` (size, atime/mtime/ctime) |
| `fd_prestat_get` | `(i32, i32) -> i32` | preopened FD の列挙 |
| `fd_prestat_dir_name` | `(i32, i32, i32) -> i32` | preopen の WASI 公開名取得 |
| `fd_renumber` | `(i32, i32) -> i32` | fd の付け替え (preopen / stdio は不可) |
| `fd_readdir` | `(i32, i32, i32, i64, i32) -> i32` | directory 列挙 (Phase 3c) |
| `path_open` | `(i32, i32, i32, i32, i32, i64, i64, i32, i32) -> i32` | ファイル open (read / write+CREATE+TRUNC、path traversal 拒否) |
| `path_filestat_get` | `(i32, i32, i32, i32, i32) -> i32` | path の `stat()` |
| `path_create_directory` / `path_remove_directory` / `path_unlink_file` | `(i32, i32, i32) -> i32` | ディレクトリ作成・空ディレクトリ削除・ファイル削除 |
| `path_rename` | `(i32, i32, i32, i32, i32, i32) -> i32` | rename (preopen 配下) |
| `path_link` / `path_symlink` / `path_readlink` | (Phase 3c) | hard link / symbolic link / symlink target 読み取り |
| `environ_sizes_get` / `environ_get` | `(i32, i32) -> i32` | 環境変数のサイズ / 展開 (host の `_environ` を伝播) |
| `args_sizes_get` / `args_get` | `(i32, i32) -> i32` | argv のサイズ / 展開 (KinoRuntime.exe の argv を伝播) |
| `proc_exit` | `(i32)` | プロセス終了 ([Error-Handling.md](Error-Handling.md) §9 の setjmp/longjmp 経由) |
| `clock_time_get` | `(i32, i64, i32) -> i32` | 時刻取得 (REALTIME = wall clock ns / MONOTONIC = QPC ns 精度) |
| `clock_res_get` | `(i32, i32) -> i32` | clock 解像度 (常に 1ns) |
| `random_get` | `(i32, i32) -> i32` | 高品質乱数 (Windows: `rand_s`/RtlGenRandom) |

これらは emscripten / clang `--target=wasm32-wasi` でコンパイルした C/Rust プログラムが起動と単純な stdio に必要とする最低限のセット。ファイルシステムは preopened directory ベースの read/write/ディレクトリ操作まで対応済み (下記 VFS / Phase 3a〜3c 参照)。POSIX 完全互換ではなく、socket 系 / `poll_oneoff` 等は未対応。

### `_initialize` (WASI reactor 規約) の自動呼出し

load 時、モジュールが `_initialize` (型 `() -> ()`) を export していれば、moduletable への登録前に **一度だけ自動実行** される (`kinowasm.c` の load フロー)。Emscripten の standalone (`--no-entry`) ビルドはグローバル C++ コンストラクタ等の初期化コードをここに置くため、未呼出だとグローバル ctor が一度も走らない (2026-07-22 対応)。型が `() -> ()` でない `_initialize` export は reactor 規約外とみなし呼び出さない (後方互換)。trap した場合は start 関数と同様 uninstantiable として load 失敗になる。

### 線形メモリ読み書き失敗時の扱い

WASI 関数はゲスト指定のアドレスから path 文字列や iovec 配列を `kinowasm_read_memory` で読み、結果を `kinowasm_write_memory` で書き戻す。**読み取りが範囲外 (all-or-nothing で失敗) の場合、未初期化のスタックバッファを安全検査やパス組み立てに渡さず、`__WASI_EFAULT` を返して打ち切る** (`path_open` / `wasi_resolve_path` / `path_symlink` / iovec 読み取り等)。**書き戻しの失敗も同様に検査され、全 WASI 関数で `__WASI_EFAULT` に統一されている** (2026-07-10: 未検査 26 箇所を統一。`fd_readdir` / `path_readlink` の bufused 過大報告も併せて修正)。同様に、ゲスト指定の長さ (`buf_len` 等) を無制限に `malloc` せず `WASI_STAGE_MAX` (16MB) で頭打ちにして単一呼び出しによる巨大確保 DoS を防ぐ。

### VFS (Virtual File System) インタフェース

WASI の `path_open` / `fd_read` / `fd_seek` / `fd_close` / `fd_filestat_get` 等は、内部では libc の `fopen`/`fread`/`fseek`/`fclose`/`stat` を直接呼ばず、**差し替え可能な vtable** を経由して動作します。これにより in-memory FS / 暗号化 FS / 圧縮アーカイブ展開 / WebSocket 経由のリモート FS 等を組み込むことができます。

```c
typedef struct {
    uint64_t size;
    uint64_t atim_ns;
    uint64_t mtim_ns;
    uint64_t ctim_ns;
    uint8_t  filetype;   /* __WASI_FILETYPE_* */
} wasi_vfs_filestat_t;

typedef struct {
    char    name[260];   /* NUL 終端 (255 byte 上限、超過時切り詰め) */
    uint8_t filetype;    /* __WASI_FILETYPE_* */
} wasi_vfs_dirent_t;

typedef struct wasi_vfs {
    void*   (*open)    (const char* host_path, int flags);   /* 失敗時 NULL */
    void    (*close)   (void* handle);
    size_t  (*read)    (void* handle, void* buf, size_t count);
    int64_t (*seek)    (void* handle, int64_t offset, int whence);  /* 失敗時 -1 */
    int64_t (*tell)    (void* handle);
    int     (*filestat)(const char* host_path, wasi_vfs_filestat_t* out);  /* 0=success */

    /* Phase 3b 拡張 (NULL なら ランタイムが ENOSYS を返す) */
    size_t  (*write)   (void* handle, const void* buf, size_t count);
    int     (*unlink)  (const char* host_path);
    int     (*mkdir)   (const char* host_path);
    int     (*rmdir)   (const char* host_path);
    int     (*rename)  (const char* from_path, const char* to_path);

    /* Phase 3c 拡張 (NULL なら ランタイムが ENOSYS を返す) */
    void*   (*opendir)     (const char* host_path);
    void    (*closedir)    (void* dh);
    int     (*readdir_next)(void* dh, wasi_vfs_dirent_t* out);   /* 1=entry, 0=末尾, -1=error */
    int     (*link)        (const char* from_path, const char* to_path);
    int     (*symlink)     (const char* target, const char* link_path);
    int64_t (*readlink)    (const char* host_path, char* buf, size_t buf_len);
    /* readlink: 書き込んだバイト数 (NUL 無し)、-2 = symbolic link でない、-1 = その他 error */
    int     (*realpath)    (const char* host_path, char* out, size_t out_len);
    /* realpath: symlink 解決済み実パスへ正規化 (containment 検査用)。0=success。NULL なら検査スキップ */

    void* user_data;
} wasi_vfs_t;

void wasi_set_vfs(const wasi_vfs_t* vfs);   /* NULL でデフォルト (libc) に戻す */
```

#### 呼び出し規約

- `flags` は `WASI_VFS_OPEN_FLAG_READ` / `_WRITE` / `_CREATE` / `_TRUNC` の bit OR
- `whence` は libc 互換 (`SEEK_SET=0`、`SEEK_CUR=1`、`SEEK_END=2`)
- `read()` の戻り値 0 は EOF。ランタイムは EOF を errno に変換せずバイト数 0 として WASM に返す
- `filestat.filetype` は `__WASI_FILETYPE_REGULAR_FILE=4` または `__WASI_FILETYPE_DIRECTORY=3` を推奨
- 基本 op (open/close/read/seek/tell/filestat) はデフォルト実装が必ず存在する前提。Phase 3b/3c の拡張 op は NULL 可 (その場合ランタイムが `ENOSYS` を返す)

#### デフォルト (libc) 実装

`extrafunction.c` の `default_vfs` は `fopen`/`fread`/`fseek`/`fclose`/`_stat64` のラッパ。`wasi_set_vfs(NULL)` でこれに戻る。

#### in-memory モック VFS の例

`Test/testsuite_runner.c` には差し替え検証用の mock VFS が同梱されており、`wasi_vfs_test/` 実行時に `wasi_set_vfs(&mock_vfs)` で適用される。`wasi_vfs_test.wast` は実ファイルにない `vfs_mock.bin` を open / read / stat することで、モックが実際に呼ばれていることを確認する。

### preopened directory の登録 (Phase 3a)

`path_open` で WASM からアクセス可能なホスト側ディレクトリは、ホスト側から **明示的に preopen** する必要があります (capability-based サンドボックス)。

```c
int32_t wasi_add_preopen(const char* host_path, const char* wasi_name);
void    wasi_clear_preopens(void);
```

- `host_path`: ホストファイルシステム上の実パス (例: `"data"`、`"/var/lib/foo"`)
- `wasi_name`: WASM 側に見える名前 (例: `"."`、`"/"`)
- 戻り値: 成功時 WASI fd 番号 (3 から始まる)、失敗時 `-1`
- 内部で文字列を複製して保持 (呼び出し側のメモリは解放可能)
- 上限: 8 個 (`WASI_MAX_PREOPENS`)

#### path traversal の防御

`path_open` / `path_filestat_get` / `path_create_directory` 等の preopen 配下 path を扱う関数は path 文字列を以下で検証する (`wasi_path_is_safe`):

- 絶対パス (`/foo` / `\\foo`) → 拒否 (`EINVAL`)
- `..` セグメントを含む → 拒否 (`EINVAL`)
- `//` 等の空セグメント → 拒否 (`EINVAL`)
- `.` セグメントは許可
- `:` を含むセグメント (Windows の drive-relative `C:foo` / NTFS Alternate Data Stream `file:stream`) → 拒否 (`EINVAL`)
- **Windows の予約 device 名** (`con` / `prn` / `aux` / `nul` / `com1`〜`com9` / `lpt1`〜`lpt9`、case-insensitive、拡張子付きでも device に解決されるため basename で判定) → 拒否 (`EINVAL`)。`#if defined(_WIN32)` ガード付き

これにより preopened directory の **配下のみ** にアクセスが限定される。

さらに字句検査に加え、**実パス containment 検査** (`wasi_path_contained`) を行う: `path_open` / `wasi_resolve_path` は組み立てた host path を VFS の `realpath` フックで正規化 (symlink/junction を解決) し、その実パスが基点 (preopen / dir fd) の実パス subtree 内に収まることを確認する (RESOLVE_BENEATH 相当)。これにより preopen 内に置かれた **外部を指す既存 symlink** や **symlink ディレクトリ成分** 経由の逸脱を防ぐ。未存在の作成対象 (O_CREAT / mkdir 等) は親ディレクトリで検査する。VFS が `realpath` を提供しない場合 (mock VFS 等) はこの検査をスキップする (自前で安全性を担保すること)。`realpath`→実アクセス間の TOCTOU は残存 (handle ベース検証は将来課題)。

`path_link` / `path_rename` は 2 つの path をすべて `wasi_resolve_path` 経由で検証し、dirfd / sandbox / Windows 予約名 / containment チェックを通る。`path_symlink` は link 名 (new_path) を `wasi_resolve_path` で検証するほか、**symlink の target も `wasi_path_is_safe` で検査** し、絶対パス / `..` を含む target は `EACCES` で拒否する (sandbox 外を指す symlink の作成を防ぐ)。失敗時の errno は `wasi_errno_from_host` で WASI errno にマップされる (未マップは `EIO`)。

#### Phase 3b で追加された書き込み機能

- `path_open` の `oflags` に `__WASI_OFLAGS_CREAT` (=1) / `__WASI_OFLAGS_TRUNC` (=8) が指定でき、`fs_rights_base` の `__WASI_RIGHT_FD_WRITE` (=64) で書き込みモードを要求
- `fd_write` がファイル fd でも動作 (libc backend は `fwrite`)
- `fd_pwrite` で offset 指定書き込み (handle 位置は呼び出し前の値に復元)
- `path_create_directory` / `path_remove_directory` / `path_unlink_file` / `path_rename` で FS 操作
- VFS インタフェースに `write` / `unlink` / `mkdir` / `rmdir` / `rename` の op が追加された (NULL で `ENOSYS`)

#### Phase 3c で追加されたディレクトリ / リンク機能

- `path_open` の `oflags` に `__WASI_OFLAGS_DIRECTORY` (=2) が指定でき、ディレクトリ用の fd を取得できる (libc backend は handle を確保せず host_path のみ保持)
- `fd_readdir(fd, buf, buf_len, cookie, bufused)` で directory を列挙。dirent layout は 24 byte の header (`d_next:u64, d_ino:u64, d_namlen:u32, d_type:u8 + 3pad`) + name bytes (NUL 無し)。`cookie=0` で先頭から、戻り値の最後の `d_next` を次回 `cookie` として渡せば継続列挙が可能
- `path_link` (hard link)、`path_symlink` (symbolic link)、`path_readlink` (symlink target 読み取り)
- VFS インタフェースに `opendir` / `closedir` / `readdir_next` / `link` / `symlink` / `readlink` / `realpath` の op が追加 (NULL で `ENOSYS` または検査スキップ)
- libc backend での実装メモ:
  - Windows: `FindFirstFileA`/`FindNextFileA` (列挙)、`CreateHardLinkA` (link)、`CreateSymbolicLinkA` + `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` (symlink、Win10 1703+ dev mode 必須)、`DeviceIoControl(FSCTL_GET_REPARSE_POINT)` で reparse buffer を読む (readlink)、`GetFinalPathNameByHandleA` (realpath)
  - POSIX: `opendir`/`readdir`/`closedir`、`link`、`symlink`、`readlink`、`realpath` を直接利用
  - readdir は `.`/`..` をスキップして上位に渡す
- `path_readlink` の戻り値: 通常ファイルに対する呼び出しは `EINVAL` を返す (POSIX 互換)。symbolic link 機能未対応の VFS は `ENOSYS`
- preopen 配下のサンドボックス制約は変わらず、`..` / 絶対パスは引き続き拒否

#### Phase 3a/3b/3c でカバーしないもの

- ファイル ロック / atomic rename / fsync 等の高度な機能
- `path_open` の `__WASI_OFLAGS_EXCL` (排他作成)
- WASI socket 系 / poll_oneoff 等の Phase 4 範囲

### argv / environ の渡し方

ホストプログラム側から WASI モジュールに argv / 環境変数を渡すには、`extrafunction.c` の以下の API を使う:

```c
void wasi_set_args(int argc, const char* const* argv);
void wasi_set_environ(const char* const* envp);  // NULL 終端配列
```

- `KinoRuntime.exe` (`main.c`) は起動時に実プロセスの `argv` / `_environ` を自動でセット
- `testsuite_runner.exe` は `wasi_test` 中は空 `(0, NULL)`、`wasi_args_test` サブディレクトリ実行時のみ既知の値をセットしてから戻す
- 文字列のコピーは行わない (ポインタ保持のみ)。ホスト側が指す string は `wasi_set_*` 呼び出し中・以後の WASM 実行中ずっと有効である必要がある

## 8. リジューム可能なホスト関数

ホスト関数が長時間ブロックする処理 (network I/O, GPU 命令完了待ち等) を持つ場合、中断 / 再開できる。トリガは **ホスト関数が `RES_SUCCESS` 以外を返すこと** だけで、ホスト関数側で store のフラグを操作する必要はない (できない — `current_store` は NULL、§3)。

流れ:

1. ホスト関数は処理が未完了なら **`RES_SUCCESS` 以外の戻り値** (アプリ独自のサスペンド理由コード、`ERR_*` 系等) を返す
2. core が実行を中断して抜け (`core_call_host` がサスペンド状態を記録)、ランタイムが store に `STATE_FLAG_SUSPENDED` を立てる
3. `kinowasm_invoke` は **ホスト関数の戻り値そのもの** をエラーコードとして返すが、store はサスペンド状態でフレーム / IP は保持されている
4. 外部処理が完了したらアプリは `kinowasm_resume(store, args)` を呼ぶ
5. WASM 側は **ホスト関数呼出の直後から** 再開する

> **重要**: ホスト関数が `RES_SUCCESS` を返すと、ランタイムは通常終了とみなす。サスペンドさせたいときは必ず非ゼロの戻り値を返すこと。

```c
// 例: 非同期 I/O で結果を待つ host 関数
static kinowasm_result_t async_read(api_call_t call)
{
    if (operation_pending(call))
        return ASYNC_PENDING;   // RES_SUCCESS 以外を返すだけでサスペンドになる

    SETRET_INT(0, get_result(call));
    return RES_SUCCESS;
}
```

> `extrafunction.c` の同梱 WASI 実装は同期処理のみだが、`Test/core/kw_core_yieldtest.c` が host yield (`ERR_NEXTFRAME_YIELD`) → `kinowasm_resume` の一巡を実演している。

## 8.5 WASM 3.0 tail call (`return_call`) との相互作用

WASM 側が `return_call` / `return_call_indirect` でホスト関数を呼び出すケースもサポート済み。core の `return_call` ハンドラは、呼び先が import (host) 関数の場合は frame 再利用の tail call ではなく **通常の call と同じ経路にフォールバック** し、host の戻り値を現在の関数の結果としてそのまま返す (`core/kw_core_exec.c` の `H_return_call`)。

ホスト関数側は通常呼出と同じインタフェースで動作する (区別は不要)。サスペンド (§8) も同様に動作する。

## 9. 動的モジュール lifecycle API (modulestore)

`extrafunction.c` には、ホストアプリが WASM モジュールを **動的に積み下ろし** するための lifecycle API がある (公開ヘッダではなく extrafunction.c の提供機能):

```c
int32_t create_wasm_module(uint32_t module_memory_size, uint32_t script_memory_size);
kinowasm_result_t push_wasm_module(int32_t module_id, const char* module_name,
                                   const void* module_data, size_t module_data_size);
kinowasm_result_t invoke_wasm_module(int32_t module_id, const char* module_name,
                                     const char* function_name, kinowasm_args_t* args);
kinowasm_result_t pop_wasm_module(int32_t module_id);
void destroy_wasm_module(int32_t module_id);
```

- `create_wasm_module`: `kinowasm_init` + `kinowasm_assign_memory` をまとめて行い、モジュールストア ID を返す (失敗時 -1)
- `push_wasm_module`: モジュールを load して積む。積む前の store 各配列の長さを `push_info` に記録し、load 失敗時はロールバックする
- `invoke_wasm_module`: 関数を呼ぶ。push/pop 後の初回呼出時に `rebuild_extra_func_type` + `validate_function_parameter` で host import のシグネチャ検証 (§2) を再実行する
- `pop_wasm_module`: 最後に push したモジュールを 1 つ下ろす (LIFO)。インスタンスの解放は `kw_free_moduleinst` に一本化されており、push→invoke→pop の繰り返しでもリークしない (回帰テスト `Test/pushpop_test.c` = 100,000 回)
- `destroy_wasm_module`: 残りのモジュールをすべて pop して store ごと破棄

## 10. メモリプール (memorytable)

`extrafunction.c` の前半に `memorytable_t` / `memorypool_t` の typedef があるが、これを使うフリーリスト実装 (WASM ヒープからの一時バッファ切り出し) は現在 **`#if 0` で無効化** されている。`segfault` / `alignfault` host 関数はプールを使わず診断出力のみ行う。host 関数を増やすとき必須ではない。

## 11. 関連ドキュメント

- [Public-API.md](Public-API.md) — `kinowasm_register_extra_func` / `kinowasm_callinfo_t` / `kinowasm_read_memory` 等
- [Error-Handling.md](Error-Handling.md) — host 関数の戻り値とエラー伝播、`proc_exit` の setjmp/longjmp 経路
- `extrafunction.c` — リファレンス実装の原本
