#pragma once

#include <stdint.h>
#include <stddef.h>
#include "features.h"
#include "kargs.h"
#include "KinoUtil/kalloc.h"

#define SYSTEM_FUNCTION "env"

typedef void* kinowasm_handle_t;

/* kinowasm_callinfo_t — ホスト関数(extra func)呼び出し時にランタイムから渡される呼び出しコンテキスト。 */
typedef struct {
	kinowasm_args_t* args;          /* 入力: WASM 側から渡された引数の配列(型と値が格納済み)。 */
	kinowasm_args_t* rets;          /* 出力: ホスト関数が戻り値を書き込む配列(各要素の type/val を設定する)。 */
	kinowasm_handle_t current_store;/* 呼び出し元ストアのハンドル(メモリ読み書き等で使用)。 */
} kinowasm_callinfo_t;
typedef kinowasm_result_t (*kinowasm_extrafuncaddress_t)(kinowasm_callinfo_t* call);

/* kinowasm_extrafunc_t — ランタイムに登録するホスト関数1件の定義。WASM の import 解決対象になる。 */
typedef struct {
	const char* module;             /* import のモジュール名(例: "env")。WASM 側の import モジュール名と一致させる。 */
	const char* name;               /* import の関数名。WASM 側の import 名と一致させる。 */
	kinowasm_extrafuncaddress_t func;/* 実際に呼び出されるホスト関数へのポインタ。 */
	void* reserved;                 /* シグネチャ文字列(1文字目=戻り値型、以降=引数型。'v'/'i'/'j'/'f'/'d'、':'区切りで複数候補可)。
	                                 * extrafunction.c の validate_function_parameter が import 検証に使用する。NULL で検証スキップ。 */
} kinowasm_extrafunc_t;
typedef kinowasm_array(kinowasm_extrafunc_t) kinowasm_extrafuncs_t;

kinowasm_handle_t kinowasm_init(void);
void kinowasm_term(kinowasm_handle_t s);
void kinowasm_assign_memory(kinowasm_handle_t S, void* memory, size_t size);
void* kinowasm_get_memory(kinowasm_handle_t S);
kinowasm_result_t kinowasm_register_extra_func(const kinowasm_extrafunc_t* extra, size_t size);
void kinowasm_clear_extra_func(void);
void kinowasm_set_stack_size(uint32_t size);
void kinowasm_set_object_cache_size(uint32_t size);
kinowasm_result_t kinowasm_load_module_from_memory(kinowasm_handle_t S, void* module_data, size_t module_size, const char* modulename, kinowasm_mem_info_t modulememory);
kinowasm_result_t kinowasm_load_module(kinowasm_handle_t S, const char* modulefile, const char* modulename, kinowasm_mem_info_t modulememory);
/* kinowasm_funcref_t — 解決済みエクスポート関数ハンドル (内部は funcaddr インデックス)。
 * kinowasm_lookup_func で取得し kinowasm_invoke_func に渡す。KINOWASM_FUNCREF_INVALID は無効。
 * 有効期間: 解決元モジュールがロードされている間。kinowasm_reset_store / 再ロード後は再取得が必要。 */
typedef int32_t kinowasm_funcref_t;
#define KINOWASM_FUNCREF_INVALID (-1)

kinowasm_result_t kinowasm_invoke(kinowasm_handle_t S, const char* module, const char* funcname, kinowasm_args_t* argument);
/* embedded NUL を含む export 名を扱うための長さ明示版。
 * funcname は NUL 終端でなくてもよい (funcname_len バイトを export 名と比較)。
 * module は通常通り NUL 終端文字列。
 * kinowasm_invoke / _n は内部で kinowasm_lookup_func_n + kinowasm_invoke_func を呼ぶ薄いラッパー。 */
kinowasm_result_t kinowasm_invoke_n(kinowasm_handle_t S, const char* module, const char* funcname, size_t funcname_len, kinowasm_args_t* argument);

/* kinowasm_lookup_func / _n — module::funcname を一度だけ解決して関数ハンドルを返す。
 * 毎フレーム呼び出すような用途で、名前検索 (O(モジュール数)+O(export数)) をロード時に
 * 1 回だけ済ませるために使う。未存在モジュールは ERR_UNKNOWN_IMPORT_SYMBOL、未存在関数は
 * ERR_UNKNOWN_IMPORT、export が関数でない場合は RES_ERROR。失敗時 *out は無効値。 */
kinowasm_result_t kinowasm_lookup_func(kinowasm_handle_t S, const char* module, const char* funcname, kinowasm_funcref_t* out);
kinowasm_result_t kinowasm_lookup_func_n(kinowasm_handle_t S, const char* module, const char* funcname, size_t funcname_len, kinowasm_funcref_t* out);
/* 解決済みハンドルを名前検索なし (O(1)) で呼び出す。argument は入出力 (戻り値で上書き)。
 * ハンドルが現在の store の関数範囲外なら ERR_INVALID_FUNC_PARAM。 */
kinowasm_result_t kinowasm_invoke_func(kinowasm_handle_t S, kinowasm_funcref_t func, kinowasm_args_t* argument);
kinowasm_result_t kinowasm_resume(kinowasm_handle_t S, kinowasm_args_t* argument);
kinowasm_result_t kinowasm_reset_store(kinowasm_handle_t S);
/* 戻り値: 全 len バイト転送成功で RES_SUCCESS、memory 未宣言/範囲外は
 * ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS、ページ確保 OOM は ERR_OUTOFMEMORY。
 * void から戻り値追加への変更は後方互換 (戻り値を無視する既存ホスト側はそのまま動作)。 */
kinowasm_result_t kinowasm_read_memory(kinowasm_callinfo_t* call, uint32_t start_address, void* data, size_t len);
kinowasm_result_t kinowasm_write_memory(kinowasm_callinfo_t* call, uint32_t start_address, const void* data, size_t len);
