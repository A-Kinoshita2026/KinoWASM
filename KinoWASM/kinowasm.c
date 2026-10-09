
#include "kinowasm.h"
#include "internal_macros.h"
#include "KinoUtil/kbuffer.h"
#include "kw_store.h"
#include "core/kw_core.h"   /* core trap 写像用に corert_t / g_rt (kw_core.h で extern 宣言) を取り込む */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ストア(WASM動作)メモリへ切り替える */
#define CHANGE_STORE_MEMORY(S) do { \
	_throwif(ERR_OUTOFMEMORY, (S)->storememory == NULL); \
	kinowasm_mem_set_info(S->storememory); \
} while(0)
#define INIT_MODULE_ARRAY(field) array_init(mod->field)
#define MATCH_TYPE_ARRAY(lhs, rhs) do { \
	if((lhs).len != (rhs).len) \
		return 0; \
	for(uint32_t i = 0; i < (lhs).len; i++) \
		if(array_at((lhs), i) != array_at((rhs), i)) \
			return 0; \
} while(0)
#define APPEND_EXTERNVAL(externvals, value) do { \
	UNUSE uint32_t len = 0; \
	kinowasm_array_append(*(externvals), (value), len); \
} while(0)
/* モジュール読み込み用メモリの確定。modulememory 指定があればそれを使い、NULL なら呼出側が
 * 設定済みのフォールバック (storememory = アリーナ一本化) を上書きせず維持する。 */
#define INIT_MODULE_MEMORY(memory, modulememory) do { \
	if((modulememory) != NULL) \
		(memory) = (modulememory); \
	_throwif(ERR_OUTOFMEMORY, (memory) == NULL); \
} while(0)
/* セキュリティ: memory を持たないモジュール(memaddrs==NULL)では mem を NULL にする。
 * 呼び出し側は mem==NULL を見て no-op する。未初期化 memaddrs[0] による wild deref を防ぐ。 */
#define GET_CURRENT_MEMORY(call, store, mem) \
	store_t* store = (store_t*)(call)->current_store; \
	frame_t* frame = store->stack->frames.data[store->stack->frame_idx]; \
	memoryinstance_t* mem = (frame->module->memaddrs != NULL) \
		? &array_at(store->memorys, frame->module->memaddrs[0]) : NULL
#define COMPARE_STRING(a, b) (strlen((const char*)a) == strlen((const char*)b) && memcmp(a, b, strlen(b)) == 0)

static kinowasm_extrafuncs_t extra_func_table = { 0 };

/* core エンジンのホスト関数呼び出し中だけ非NULL。kinowasm_read/write_memory が
 * store の memory instance でなく core の flat mem を直接読み書きするための橋渡し。 */
uint8_t* g_core_host_mem = NULL;
uint64_t g_core_host_mem_size = 0;
kinowasm_settings_t kinowasm_settings = {
	.default_param_stack_size = 256,
	.default_parser_stack_size = 4096 * 16,
	.default_object_cache_size = 100,
	.extra_func_table = &extra_func_table,
};

/* module_init — module_t の各配列フィールドとカウンタ類を初期化する。
 * デコード前に呼び出し、types/functions/datas などの可変長配列を空状態にし、
 * startidx や各 import カウント、has_start を 0 クリアする。
 * 引数:
 *   mod - 初期化対象のモジュール構造体 */
static void module_init(module_t* mod)
{
	INIT_MODULE_ARRAY(types);
	INIT_MODULE_ARRAY(functions);
	INIT_MODULE_ARRAY(datas);
	INIT_MODULE_ARRAY(imports);
	INIT_MODULE_ARRAY(tables);
	INIT_MODULE_ARRAY(memorys);
	INIT_MODULE_ARRAY(exports);
	INIT_MODULE_ARRAY(globals);
	INIT_MODULE_ARRAY(elements);
	INIT_MODULE_ARRAY(tags);  /* WASM 3.0 exception handling */
	array_init(mod->declared_funcs);
	mod->startidx = 0;
	mod->funcimport_count = 0;
	mod->tableimport_count = 0;
	mod->memoryimport_count = 0;
	mod->globalimport_count = 0;
	mod->tagimport_count = 0;  /* WASM 3.0 exception handling */
	mod->has_start = 0;
}

/* match_functype — 2 つの関数型が一致するか判定する。
 * パラメータ列 (rt1) と戻り値列 (rt2) を要素単位で比較する。
 * 引数:
 *   ft1, ft2 - 比較する関数型
 * 戻り値: 一致なら 1、不一致なら 0 */
static int32_t match_functype(functiontype_t* ft1, functiontype_t* ft2)
{
	MATCH_TYPE_ARRAY(ft1->rt1, ft2->rt1);
	MATCH_TYPE_ARRAY(ft1->rt2, ft2->rt2);
	return 1;
}

/* resolve_tag_typeidx — tag idx を、例外パラメータを記述する関数型の typeidx へ解決する。
 * imported tag は import section を IMPORTDESC_TAG でフィルタした tagidx 番目の宣言型から、
 * local tag は tagimport_count を引いた index で tags[] から取得する
 * (runtime_eh.c の kw_get_tag_typeidx と同じ規則)。tag import の型互換性検証で使う。
 * 引数:
 *   mod    - 解決対象のモジュール
 *   tagidx - import を含む通し番号の tag インデックス
 * 戻り値: 関数型 typeidx、解決不能時は UINT32_MAX */
static uint32_t resolve_tag_typeidx(module_t* mod, uint32_t tagidx)
{
	if(tagidx < mod->tagimport_count) {
		uint32_t imp_count = 0;
		for(size_t i = 0; i < mod->imports.len; i++) {
			import_t* im = &mod->imports.data[i];
			if(im->d.kind == IMPORTDESC_TAG) {
				if(imp_count == tagidx)
					return im->d.tag.typeidx;
				imp_count++;
			}
		}
		return UINT32_MAX;
	}
	uint32_t local_idx = tagidx - mod->tagimport_count;
	if(local_idx >= mod->tags.len)
		return UINT32_MAX;
	return mod->tags.data[local_idx].tagtype.typeidx;
}

/* match_limits — limits (min/max) が import の要求を満たすか判定する。
 * import 側 (l1) の min が要求 (l2) の min 以上で、l2 に max があれば l1 にも
 * max があり l1.max <= l2.max を満たす場合に互換と見なす。
 * 引数:
 *   l1 - 実際に提供される側の limits
 *   l2 - import が要求する側の limits
 * 戻り値: 互換なら 1、非互換なら 0 */
static int32_t match_limits(limits_t* l1, limits_t* l2)
{
	/* index type (memory64/table64) と shared (threads) 属性は import 要求と実インスタンスで
	 * 完全一致していなければならない。min/max だけを比較すると memory64 を要求する import を
	 * memory32 のインスタンスが充足でき、アドレス型 (i64/i32) が食い違って実行時に型混同を
	 * 起こす。shared 属性の不一致も threads セマンティクスを壊す。 */
	if(l1->is_64 != l2->is_64 || l1->shared != l2->shared)
		return 0;
	if(l1->min >= l2->min) {
		if(!l2->has_max)
			return 1;
		else if(l1->has_max && l2->has_max && l1->max <= l2->max)
			return 1;
		else
			return 0;
	}

	return 0;
}

/* match_tabletype — 2 つの table 型が一致するか判定する。
 * reftype が一致し、かつ limits が互換 (match_limits) であれば一致と見なす。
 * 引数:
 *   tt1 - 実際に提供される側の table 型
 *   tt2 - import が要求する側の table 型
 * 戻り値: 互換なら 1、非互換なら 0 */
static int32_t match_tabletype(tabletype_t* tt1, tabletype_t* tt2)
{
	if(tt1->reftype != tt2->reftype)
		return 0;

	return match_limits(&tt1->limits, &tt2->limits);
}

/* match_memtype — 2 つの memory 型が一致するか判定する。
 * memory 型は limits そのものなので match_limits に委譲する。
 * 引数:
 *   mt1 - 実際に提供される側の memory 型
 *   mt2 - import が要求する側の memory 型
 * 戻り値: 互換なら 1、非互換なら 0 */
static int32_t match_memtype(memorytype_t* mt1, memorytype_t* mt2)
{
	return match_limits(mt1, mt2);
}

/* match_globaltype — 2 つの global 型が一致するか判定する。
 * 可変性 (mut) と値型 (valtype) の両方が完全一致する場合のみ互換とする。
 * 引数:
 *   gt1, gt2 - 比較する global 型
 * 戻り値: 一致なら 1、不一致なら 0 */
static int32_t match_globaltype(globaltype_t* gt1, globaltype_t* gt2)
{
	if(gt1->mut != gt2->mut || gt1->valtype != gt2->valtype)
		return 0;

	return 1;
}

/* find_exported_module — ストアに登録済みのモジュールを名前で検索する。
 * moduletable を走査し、登録名が name と一致する moduleinst を返す。
 * 引数:
 *   S    - 検索対象のストア
 *   name - 探すモジュール名 (NUL 終端文字列)
 * 戻り値: 見つかった moduleinst へのポインタ、見つからなければ NULL */
static moduleinst_t* find_exported_module(store_t* S, const char* name)
{
	foreach(mod, moduletable_t, S->moduletable)
		if(COMPARE_STRING(mod->name.data, name))
			return mod->module;

	return NULL;
}

/* find_export_n — モジュールの export を名前 (バイト長指定) で検索する。
 * 名前にバイト長を明示する版。embedded NUL を含む export 名を扱うために使う。
 * read_string() は領域に末尾 NUL を 1 byte 追加して name.len = str_size+1 と
 * 格納するため、比較時は name.len - 1 が実バイト長になる。
 * 引数:
 *   from      - 検索対象のモジュールインスタンス
 *   name      - 探す export 名
 *   name_len  - name の実バイト長
 *   externval - 見つかった export の externval を書き出す先
 * 戻り値: 見つかれば RES_SUCCESS、なければ ERR_UNKNOWN_IMPORT */
static kinowasm_result_t find_export_n(moduleinst_t* from, const char* name, size_t name_len, externval_t* externval)
{
	_try{
		foreach(exportinst, exportinst_t, from->exports) {
			size_t stored_len = exportinst->name.len > 0 ? exportinst->name.len - 1 : 0;
			if(stored_len == name_len &&
				memcmp(exportinst->name.data, name, name_len) == 0) {
				*externval = exportinst->value;
				_throw(RES_SUCCESS);
			}
		}
		_throw(ERR_UNKNOWN_IMPORT);
	}
	_catch:
	return _result;
}

/* resolve_imports — モジュールの全 import を解決し externval 列を構築する。
 * まず extra_func_table (ホスト関数) との名前照合を試み、一致すれば EXTRA_FUNC_ID
 * 起点の funcaddr を割り当て型情報を S->extra_func_type に記録する。一致しなければ
 * 登録済みモジュールの export から解決し、func/table/memory/global/tag の各種別ごとに
 * 型互換性を検査する。解決結果は externvals に import と同順で追加される。
 * 引数:
 *   S          - 対象ストア
 *   module     - import を持つデコード済みモジュール
 *   externvals - 解決結果を格納する externval 配列 (本関数が array_init/grow する)
 * 戻り値: 成功時 RES_SUCCESS、解決失敗や型不一致時は対応する ERR_* */
static kinowasm_result_t resolve_imports(store_t* S, module_t* module, externvals_t* externvals)
{
	_try{
		CHANGE_STORE_MEMORY(S);
		array_init(*externvals);
		_throwiferr(kinowasm_array_grow_from(*externvals, module->imports.len));

		/* extra_func_type は「初回 import の宣言型」を store 寿命で累積する台帳
		 * (rollback/invoke 側の rebuild_extra_func_type と同セマンティクス)。毎ロードで全クリア
		 * すると、ホスト関数を reexport したモジュール経由の import 解決が後段の NULL 検査で
		 * 常に拒否されるため、既存記録を保持し新規スロットのみゼロ初期化する。記録された型は
		 * store アリーナ上 (relocate 済) で moduleinst は teardown/rollback まで生存するため
		 * dangling にならない (rollback 側は rebuild_extra_func_type が台帳を再構築する)。 */
		if(S->extra_func_type.capacity < extra_func_table.len) {
			kinowasm_array_t old = *(kinowasm_array_t*)&S->extra_func_type;
			kinowasm_result_t new_res = array_new(S->extra_func_type, extra_func_table.len);
			if(_is_error(new_res)) {
				*(kinowasm_array_t*)&S->extra_func_type = old;   /* 確保失敗時は旧台帳のまま戻す */
				_throw(new_res);
			}
			memset(S->extra_func_type.data, 0, S->extra_func_type.item_size * S->extra_func_type.len);
			if(old.capacity != 0) {
				memcpy(S->extra_func_type.data, old.data, old.item_size * old.len);
				kinowasm_array_term(&old);
			}
		} else {
			size_t prev_len = S->extra_func_type.len;
			S->extra_func_type.len = extra_func_table.len;
			if(prev_len < extra_func_table.len)
				memset(&array_at(S->extra_func_type, prev_len), 0, S->extra_func_type.item_size * (extra_func_table.len - prev_len));
		}
		foreach(import, import_t, module->imports) {
			if(import->d.kind == IMPORTDESC_FUNC) {
				externval_t ext = { 0 };
				ext.kind = import->d.kind;
				ext.func = 0;
				for(uint32_t i = 0; i < extra_func_table.len; i++) {
					if(COMPARE_STRING(import->module.data, array_at(extra_func_table, i).module) && COMPARE_STRING(import->name.data, array_at(extra_func_table, i).name)) {
						ext.func = EXTRA_FUNC_ID + i;

						/* パラメータ情報を指定する。初回 import の宣言型を記録し、以後の import は
						 * それと構造一致するか検査する (host 関数自体は型情報を持たないため、
						 * モジュール間の宣言一貫性のみ担保できる)。 */
						if(array_at(S->extra_func_type, i) == NULL)
							array_at(S->extra_func_type, i) = &array_at(module->types, import->d.functypeidx);
						else
							_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE,
								!match_functype(array_at(S->extra_func_type, i), &array_at(module->types, import->d.functypeidx)));

						break;
					}
				}
				if(ext.func != 0) {
					APPEND_EXTERNVAL(externvals, ext);
					continue;
				}
			}
			moduleinst_t* from = find_exported_module(S, (const char*)import->module.data);
			if(from == NULL) {
				/* assert_unlinkable test cases legitimately produce this error.
				 * Surface it via the error code; callers print as needed. */
				printf("Error: unresolved import \"%.*s\".\"%.*s\" - no host module named \"%.*s\" is registered\n",
					(int)import->module.len, import->module.data,
					(int)import->name.len, import->name.data,
					(int)import->module.len, import->module.data);
				_throw(ERR_UNKNOWN_IMPORT_SYMBOL);
			}
			externval_t externval = { 0 };
			_throwiferr(find_export_n(from, (const char*)import->name.data, strlen((const char*)import->name.data), &externval));
			_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, import->d.kind != externval.kind);

			switch(import->d.kind) {
			case IMPORTDESC_FUNC: {
				functiontype_t* actual;
				if(externval.func >= EXTRA_FUNC_ID) {
					funcaddr_t extra_func = externval.func - EXTRA_FUNC_ID;
					_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, extra_func >= (funcaddr_t)S->extra_func_type.len);
					actual = array_at(S->extra_func_type, extra_func);
					_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, actual == NULL);
				} else {
					actual = array_at(S->funcs, externval.func).type;
				}
				functiontype_t* expect = &array_at(module->types, import->d.functypeidx);
				_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, !match_functype(actual, expect));
				break;
			}
			case IMPORTDESC_TABLE: {
				/* WASM 仕様: limit 比較では declared min ではなく
				 * 現在の table 要素数 (elem.len) を使う。table.grow 後の
				 * 拡張サイズも考慮する必要があるため。 */
				tableinstance_t* table_inst = &array_at(S->tables, externval.table);
				tabletype_t actual = table_inst->type;
				actual.limits.min = (uint32_t)table_inst->elem.len;
				tabletype_t* expect = &import->d.table;
				_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, !match_tabletype(&actual, expect));
				break;
			}
			case IMPORTDESC_MEMORY: {
				/* table と同様、現在の memory ページ数で比較する。 */
				memoryinstance_t* mem_inst = &array_at(S->memorys, externval.mem);
				memorytype_t actual = mem_inst->type;
				actual.min = (uint32_t)mem_inst->num_pages;
				memorytype_t* expect = &import->d.memory;
				_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, !match_memtype(&actual, expect));
				break;
			}
			case IMPORTDESC_GLOBAL: {
				globaltype_t* actual = &array_at(S->globals, externval.global).gt;
				globaltype_t* expect = &import->d.globaltype;
				_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, !match_globaltype(actual, expect));
				break;
			}
			case IMPORTDESC_TAG: {
				/* WASM 3.0 EH: tag import の型互換性を検証する。source module (from)
				 * が export した tag の global tagaddr を tagaddrs から逆引きして source
				 * 側の local tagidx を求め、その tag の関数型 (例外パラメータ列) を import
				 * 側の宣言型 (import->d.tag.typeidx) と構造的に比較する。これにより不正な型
				 * の tag import を受理し、後段 (throw/catch) で誤捕捉・不正動作するのを防ぐ。 */
				module_t* src_mod = &from->origin_module;
				uint32_t src_tag_count = src_mod->tagimport_count + (uint32_t)src_mod->tags.len;
				uint32_t src_tagidx = UINT32_MAX;
				for(uint32_t t = 0; t < src_tag_count; t++) {
					if(from->tagaddrs != NULL && from->tagaddrs[t] == externval.tag) {
						src_tagidx = t;
						break;
					}
				}
				_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, src_tagidx == UINT32_MAX);

				uint32_t src_typeidx = resolve_tag_typeidx(src_mod, src_tagidx);
				_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, src_typeidx >= src_mod->types.len);
				_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, import->d.tag.typeidx >= module->types.len);
				functiontype_t* actual = &array_at(src_mod->types, src_typeidx);
				functiontype_t* expect = &array_at(module->types, import->d.tag.typeidx);
				_throwif(ERR_INCOMPATIBLE_IMPORT_TYPE, !match_functype(actual, expect));
				break;
			}
			}
			APPEND_EXTERNVAL(externvals, externval);
		}
	}
	_catch:
	return _result;
}

store_t* kw_new_store(void);
void kw_reset_frames(store_t* S);
kinowasm_result_t kw_instantiate(store_t* S, module_t* module, externvals_t* externvals, moduleinst_t** inst);
kinowasm_result_t kw_orphan_moduleinst(store_t* S, moduleinst_t* inst);   /* 失敗 moduleinst を term 解放対象に積む */
kinowasm_result_t kinowasm_parse_module(module_t* mod, kinowasm_buf_t* buf);

/* ───── core (register-TOS) 実行エンジン統合フック ─────────────────────
 * core register-TOS direct-threaded エンジンが唯一の実行系。load 後に store から core
 * ランタイムを構築 (kw_core_build_from_store) し、invoke を core へルーティングして
 * 引数/戻り値を marshalling する。musttail 必須 (clang/clang-cl か MSVC 14.50+)。
 * 橋渡し本体は KinoWASM/core/kw_core_bridge.c。 */
extern int     kw_core_build_from_store(store_t* S, moduleinst_t* inst, const uint8_t* wasm, size_t size);
extern int32_t kw_core_select_func(int32_t funcaddr, void* moduleinst);   /* funcaddr+所属moduleinst→インスタンス active 化 + core idx */
extern void    kw_core_save_ctx(kw_core_ctx_t* ctx);    /* 実行コンテキスト (active + g_rt/g_compiled) 退避 */
extern void    kw_core_restore_ctx(const kw_core_ctx_t* ctx); /* 同 復元 */
extern int     kw_core_is_executing(void);              /* core 実行中 (= この invoke は再入) か */
extern int     kw_core_invoke_ex(uint32_t func_idx, const int64_t* args, uint32_t nargs, int64_t* ret, const char** out_trap_msg);
extern int     kw_core_resume(int64_t* ret);             /* suspend 済 core 実行を再開 (0完走/1trap/2再yield) */
extern int     kw_core_suspend_code(void);               /* host yield の伝播 code */
extern void    kw_core_free_store(store_t* S);           /* store 破棄時に core インスタンスを解放 */

/* suspend した core 実行の entry funcaddr (resume 完走時に結果型 rt2 を引くため)。同時 suspend は
 * STATE_FLAG_SUSPENDED で 1 つに制限されるため static で足りる。 */
static funcaddr_t g_core_suspend_funcaddr = 0;

/* core trap 種別 → exceptioncode.h のエラーコード写像 (ホットパス外)。core_trap が g_rt->trap_msg に
 * 入れた文字列を見て具体的な trap コードを返す。invoke / resume / start の trap 返却点で共用する。
 * core 側 (ホットパス) は一切変更せず、bridge/公開 API 層でのみ判定するのが要点。未知/NULL は
 * 従来どおり ERR_TRAP_UNREACHABLE にフォールバック。 */
static kinowasm_result_t kw_core_trap_to_result(const char* msg)
{
	if(msg == NULL)
		return ERR_TRAP_UNREACHABLE;

	if(!strcmp(msg, "undefined element"))
		return ERR_TRAP_UNDEFINED_ELEMENT;

	if(!strcmp(msg, "uninitialized element"))
		return ERR_TRAP_UNINITIALIZED_ELEMENT;

	if(!strcmp(msg, "indirect call type mismatch"))
		return ERR_TRAP_INDIRECT_CALL_TYPE_MISMATCH;

	if(!strcmp(msg, "out of bounds table access"))
		return ERR_TRAP_OUT_OF_BOUNDS_TABLE_ACCESS;

	/* "oob load" / "oob store" / "oob memory.copy" / "oob memory.fill" / "out of bounds memory access" */
	if(!strncmp(msg, "oob", 3) || !strcmp(msg, "out of bounds memory access"))
		return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;

	if(!strcmp(msg, "div0"))
		return ERR_TRAP_INTERGER_DIVIDE_BY_ZERO;

	if(!strcmp(msg, "ovf"))
		return ERR_TRAP_INTERGET_OVERFLOW;

	if(!strcmp(msg, "trunc"))
		return ERR_TRAP_INVALID_CONVERSION_TO_INTERGER;

	if(!strcmp(msg, "wasm exception") || !strcmp(msg, "null exnref"))
		return ERR_WASM_EXCEPTION;

	/* "unreachable" / "runaway/overflow" / "resume chain overflow" / "unimplemented WASI" /
	 * "unknown import" は専用コードが無いため UNREACHABLE 扱い。 */
	return ERR_TRAP_UNREACHABLE;
}

/* kw_core_invoke_marshalled — funcaddr の関数を core エンジンで実行する。
 * 呼出契約: args は入力時に引数列、出力時に戻り値列へ上書きされる。
 * 引数を rt1 型と照合し i64 スロットへ marshalling、結果を rt2 型に従って args へ詰め直す
 * (多値結果は ret_multi/mret_get 経由)。 */
static kinowasm_result_t kw_core_invoke_marshalled(store_t* S, funcaddr_t funcaddr, kinowasm_args_t* args)
{
	/* host 関数の中から呼ばれた再入 invoke では、kw_core_select_func が差し替えた実行コンテキストを
	 * 戻さないと、外側の実行が違うインスタンス (g_rt / g_compiled) を見てしまう。active だけでなく
	 * 実行中の g_rt/g_compiled ごと控えておき (cross-module 呼出中は両者が食い違う)、成功/失敗
	 * どちらの経路でも _catch で復元する。 */
	kw_core_ctx_t prev_ctx;
	kw_core_save_ctx(&prev_ctx);
	int reentrant = kw_core_is_executing();
	_try{
		functioninstance_t* funcinst = &kinowasm_array_at(S->funcs, funcaddr);
		functiontype_t* functype = funcinst->type;
		/* 引数個数・型を rt1 と照合 */
		_throwif(ERR_INVALID_FUNC_PARAM, args->len != functype->rt1.len);
		size_t idx = 0;
		kinowasm_array_foreach(arg, kinowasm_arg_t, *args)
			_throwif(ERR_INVALID_FUNC_PARAM, arg->type != kinowasm_array_at(functype->rt1, idx++));
		/* core は単一/多値の両方に対応 (多値は ret_multi/mret_get 経由)。上限のみガード。 */
		_throwif(ERR_INVALID_FUNC_PARAM, functype->rt2.len > 64);

		/* funcaddr から所属インスタンスを特定し active 化、core global func idx を得る */
		int32_t coreidx = kw_core_select_func((int32_t)funcaddr, funcinst->module);
		_throwif(ERR_INVALID_FUNC_PARAM, coreidx < 0);

		/* 引数 → i64 スロット marshalling。core executor はスロットを型別フィールドで読むため、
		 * i32/f32 は低 32bit に値を置けばよい。i64/f64 はビット列をそのまま渡す。 */
		int64_t slots[64];
		uint32_t nargs = (uint32_t)functype->rt1.len;
		_throwif(ERR_INVALID_FUNC_PARAM, nargs > 64);
		idx = 0;
		kinowasm_array_foreach(arg, kinowasm_arg_t, *args) {
			switch(arg->type) {
			case TYPE_VAL_I32:
				slots[idx] = (int64_t)(uint32_t)arg->val.num.i32;
				break;
			case TYPE_VAL_F32: {
				uint32_t b;
				memcpy(&b, &arg->val.num.f32, sizeof(b));
				slots[idx] = (int64_t)b;
				break;
			}
			default:
				slots[idx] = arg->val.num.i64;
				break;
			}
			idx++;
		}

		/* core 実行。単一は raw r0、多値は g_core_mret 経由で results[] へ詰める。
		 * trap は ERR_TRAP_UNREACHABLE、host yield (suspend) は SUSPENDED + host code 伝播。 */
		int64_t results[64];
		for(uint32_t i = 0; i < 64; i++)
			results[i] = 0;

		const char* trap_msg = NULL;
		int rc = kw_core_invoke_ex((uint32_t)coreidx, slots, nargs, results, &trap_msg);
		if(rc == 2) {
			/* host yield: 再開チェーンは core 側に保存済。store を SUSPENDED にし、再開用 funcaddr を
			 * 控えて host code (ERR_NEXTFRAME_YIELD 等) を caller へ伝播。args は据置 (結果はまだ無い)。 */
			S->state_flags |= STATE_FLAG_SUSPENDED;
			g_core_suspend_funcaddr = funcaddr;
			_throw((kinowasm_result_t)kw_core_suspend_code());
		}
		if(rc)
			_throw(kw_core_trap_to_result(trap_msg));

		/* 戻り値列へ詰め直し。入力 args を解放し rt2.len 分を再確保する。
		 * 各結果のビット列を union へ格納し、型は rt2 で解釈する。 */
		kinowasm_array_term_from(*args);
		_throwiferr(kinowasm_array_new_from(*args, functype->rt2.len));
		for(size_t i = 0; i < functype->rt2.len; i++) {
			kinowasm_arg_t* ret = &args->data[i];
			ret->type = kinowasm_array_at(functype->rt2, i);
			ret->val.num.i64 = results[i];
		}
	}
	_catch:
	if(reentrant)
		kw_core_restore_ctx(&prev_ctx);   /* 再入: 外側が実行していたコンテキストへ戻す */
	return _result;
}

/* kinowasm_init — 新しいストアを確保しランタイムハンドルを返す。
 * kw_new_store でストアを生成し、不透明ハンドルへキャストして返す。
 * 戻り値: 生成したストアを指すハンドル (失敗時は kw_new_store に依存) */
kinowasm_handle_t kinowasm_init(void)
{
	store_t* s = kw_new_store();
	return (kinowasm_handle_t)s;
}

/* kw_free_module_metadata — load 完了後も moduleinst が参照し続ける origin_module のメタデータを
 * 解放する: types とその rt1/rt2 (valtype 列)、imports とその module/name 文字列、tags 配列本体。
 * functions/datas/elements/exports(本体) 等は load 時に kw_free_module_transient が解放済。これらの
 * メタデータは従来どちらの teardown でも解放されずアリーナ一括解放まで残っていたため、push/pop を
 * 繰り返す動的モジュールで storememory に蓄積していた。types/imports/tags は relocate / 直接 decode
 * とも per-module の実体で他モジュールと非共有なので、moduleinst 単位で安全に解放できる。
 * 配列解放 (kinowasm_array_term) はブロックの所属アリーナで解放するため現在アリーナに非依存。 */
static void kw_free_module_metadata(module_t* m)
{
	kinowasm_array_foreach(ft, functiontype_t, m->types) {
		kinowasm_array_term_from(ft->rt1);
		kinowasm_array_term_from(ft->rt2);
	}
	kinowasm_array_term_from(m->types);

	kinowasm_array_foreach(im, import_t, m->imports) {
		kinowasm_array_term_from(im->module);
		kinowasm_array_term_from(im->name);
	}
	kinowasm_array_term_from(m->imports);

	kinowasm_array_term_from(m->tags);   /* tag_t は付随確保なし。配列本体のみ */
}

/* kw_free_moduleinst — moduleinst が確保した address 配列群・exports・保持メタデータを解放し本体を
 * free する。NULL は no-op。登録モジュール (moduletable) と失敗インスタンス化 (orphan_moduleinsts) の
 * 双方の teardown、および動的モジュール pop (rollback_modulepush) で共用する。 */
void kw_free_moduleinst(moduleinst_t* mi)
{
	if(mi == NULL)
		return;
	/* 保持メタデータ (types+rt1/rt2 / imports+文字列 / tags)。 */
	kw_free_module_metadata(&mi->origin_module);
	/* export 名文字列 (exportinst.name は relocate/直接 decode の名前を浅く参照。実体の唯一の所有者)
	 * を解放してから exports 配列本体を解放する。 */
	kinowasm_array_foreach(ex, exportinst_t, mi->exports)
		kinowasm_array_term_from(ex->name);
	/* kw_instantiate が ALLOC_MODULE_ADDRS / kinowasm_mem_malloc で確保した
	 * address 配列群。NULL は kinowasm_mem_free が許容。 */
	kinowasm_mem_free(mi->funcaddrs);
	kinowasm_mem_free(mi->tableaddrs);
	kinowasm_mem_free(mi->memaddrs);
	kinowasm_mem_free(mi->globaladdrs);
	kinowasm_mem_free(mi->elemaddrs);
	kinowasm_mem_free(mi->dataaddrs);
	kinowasm_mem_free(mi->tagaddrs);
	array_term(mi->exports);
	kinowasm_mem_free(mi);
}

/* kinowasm_term — ストアと内部で確保した全リソースを解放する。
 * フレームをリセットしたうえでスタック、フレームプール、ローカルプール連鎖、
 * moduletable 内の各 moduleinst と、失敗インスタンス化で確保された orphan
 * moduleinst を順に解放し、最後にストア本体を解放する。
 * NULL ハンドルは無視する。module ごとの memory は外部所有のため解放しない。
 * 引数:
 *   s - 破棄するランタイムハンドル (NULL 可) */
void kinowasm_term(kinowasm_handle_t s)
{
	store_t* store = (store_t*)s;
	if(store == NULL)
		return;

	/* core 統合: この store に紐付く core インスタンス (flat mem / compile 結果等) を
	 * 解放する。testsuite はテストファイルごとに term→init で store を作り直すため、ここで
	 * 回収しないと per-load の flat mem が蓄積して OOM する。 */
	kw_core_free_store(store);

	kw_reset_frames(store);

	if(store->stack != NULL) {
		array_term(store->stack->frames);
		kinowasm_mem_free(store->stack);
	}

	if(store->framepool.data != NULL) {
		for(size_t i = 0; i < store->framepool.capacity; i++) {
			if(store->framepool.data[i] != NULL)
				kinowasm_mem_free(store->framepool.data[i]);
		}
		array_term(store->framepool);
	}

	for(localpool_t* pool = store->localpool.next; pool != NULL;) {
		localpool_t* next = pool->next;
		kinowasm_mem_free(pool->locals);
		kinowasm_mem_free(pool);
		pool = next;
	}

	if(store->moduletable.data != NULL) {
		for(size_t i = 0; i < store->moduletable.len; i++) {
			moduletable_t* mod = &store->moduletable.data[i];
			/* name buffer は array_new で確保。data=NULL でも array_term は安全。 */
			array_term(mod->name);
			/* moduletable は name -> moduleinst の写像で、WAST の (register "alias") 等により
			 * 複数エントリが同一 moduleinst を指しうる。moduleinst は store が一意に所有するため
			 * teardown では一度だけ解放し、以降の別名エントリは NULL 化して二重解放を防ぐ。 */
			moduleinst_t* mi = mod->module;
			if(mi != NULL) {
				kw_free_moduleinst(mi);
				for(size_t j = i; j < store->moduletable.len; j++)
					if(store->moduletable.data[j].module == mi)
						store->moduletable.data[j].module = NULL;
			}
			/* mod->memory は CLAUDE.md 方針で外部所有。runtime 側で free しない。 */
		}
		array_term(store->moduletable);
	}

	/* 失敗インスタンス化で確保された未登録 moduleinst を解放 (instantiate _catch で追跡)。
	 * 仕様準拠のため失敗時も巻き戻さず保持したぶんをここでまとめて回収する。 */
	if(store->orphan_moduleinsts.data != NULL) {
		for(size_t i = 0; i < store->orphan_moduleinsts.len; i++)
			kw_free_moduleinst(store->orphan_moduleinsts.data[i]);
		array_term(store->orphan_moduleinsts);
	}

	kinowasm_mem_free(store);
}

/* kinowasm_assign_memory — ストア (WASM 実行用) メモリを外部バッファで割り当てる。
 * 渡されたバッファを kinowasm_mem_info_init で管理情報化し store->storememory に設定する。
 * 引数:
 *   S      - 対象ハンドル
 *   memory - ストアが使用する外部確保済みメモリ領域
 *   size   - memory のバイトサイズ */
void kinowasm_assign_memory(kinowasm_handle_t S, void* memory, size_t size)
{
	store_t* store = (store_t*)S;
	store->storememory = kinowasm_mem_info_init(memory, size);
}

/* kinowasm_get_memory — ストアに割り当て済みのストアメモリ管理情報を取得する。
 * 引数:
 *   S - 対象ハンドル
 * 戻り値: store->storememory (kinowasm_mem_info_t)。未割り当てなら NULL */
void* kinowasm_get_memory(kinowasm_handle_t S)
{
	store_t* store = (store_t*)S;
	return store->storememory;
}

/* kinowasm_register_extra_func — ホスト関数群をグローバルな extra_func_table に登録する。
 * 初回呼出時にテーブルを初期化し、配列を size 分拡張してから extra をまとめてコピーする。
 * WASI 等のホスト関数を import 解決対象として追加するために使う。
 * 引数:
 *   extra - 登録するホスト関数定義の配列
 *   size  - extra の要素数
 * 戻り値: 成功時 RES_SUCCESS、引数不正で RES_ERROR、確保失敗で ERR_OUTOFMEMORY */
kinowasm_result_t kinowasm_register_extra_func(const kinowasm_extrafunc_t* extra, size_t size)
{
	_try{
		if(extra == NULL || size == 0)
			_throw(RES_ERROR);

		if(extra_func_table.item_size == 0)
			array_init(extra_func_table);

		size_t table_start = extra_func_table.len;
		if(kinowasm_array_grow_from(extra_func_table, size) == 0) {
			memcpy(&extra_func_table.data[table_start], extra, sizeof(kinowasm_extrafunc_t) * size);
			extra_func_table.len += size;
		} else {
			_throw(ERR_OUTOFMEMORY);
		}
	}
	_catch:
	return _result;
}

/* kinowasm_clear_extra_func — 登録済みホスト関数テーブルを解放する。
 * 初期化済み (item_size != 0) の場合のみ extra_func_table を array_term する。 */
void kinowasm_clear_extra_func(void)
{
	if(extra_func_table.item_size != 0)
		array_term(extra_func_table);
}

/* kinowasm_set_stack_size — パーサ用スタックサイズの既定値を設定する。
 * 引数:
 *   size - 設定する default_parser_stack_size (バイト) */
void kinowasm_set_stack_size(uint32_t size)
{
	kinowasm_settings.default_parser_stack_size = size;
}

/* kinowasm_set_object_cache_size — オブジェクトキャッシュサイズの既定値を設定する。
 * 引数:
 *   size - 設定する default_object_cache_size */
void kinowasm_set_object_cache_size(uint32_t size)
{
	kinowasm_settings.default_object_cache_size = size;
}

/* kinowasm_load_module_from_memory — メモリ上の WASM バイナリをロードして実体化する。
 * modulememory をモジュール読み込み用メモリとして切り替え、デコード → import 解決 →
 * instantiate を順に実行する。成功後はストアメモリへ戻し、モジュール名・moduleinst・
 * 使用メモリを moduletable に登録する。デコードや実体化に使った一時資源は適宜解放し、
 * 失敗時は _catch でストアメモリへ復帰し externvals を巻き戻す。
 * 引数:
 *   S            - 対象ハンドル
 *   module_data  - WASM バイナリ先頭ポインタ
 *   module_size  - バイナリのバイトサイズ
 *   modulename   - ストアに登録するモジュール名
 *   modulememory - モジュール読み込みに使う外部メモリ管理情報
 * 戻り値: 成功時 RES_SUCCESS、各段階の失敗に応じた ERR_* */
/* instantiation の結果が trap (= active elem/data segment / start 関数の trap) か判定する。
 * これらは moduleinst が完全に確保された後に起き、import 済み shared table/memory への部分的な
 * 副作用が残る。link/decode/alloc 失敗 (RES 124+ や OOM) は副作用を残さないので対象外。 */
static inline int kw_is_trap_result(kinowasm_result_t r)
{
	return (r >= ERR_TRAP_UNREACHABLE && r <= ERR_TRAP_INVALID_CONVERSION_TO_INTERGER)
		|| r == ERR_TRAP_UNCAUGHT_EXCEPTION || r == ERR_WASM_EXCEPTION;
}

/* kw_reloc_str_to_store — module アリーナの文字列 (read_string 由来、末尾 NUL 含む) を
 * current アリーナ (store) へディープコピーし、元の string_t を store コピーへ張り替える。 */
static kinowasm_result_t kw_reloc_str_to_store(string_t* s)
{
	_try{
		if(s->len != 0 && s->data != NULL) {
			string_t cp = { 0 };
			_throwiferr(kinowasm_array_copy_from(cp, *s));
			kinowasm_mem_free(s->data);   /* 旧 .data (module アリーナ) を解放してから store コピーへ張り替える */
			*s = cp;
		}
	}
	_catch:
	return _result;
}

/* kw_relocate_module_metadata_to_store — module アリーナに確保され load 完了後も参照される
 * メタデータを store アリーナへ移す。core 移行後、moduleinst / origin_module / core ランタイム /
 * cross-module 解決の各経路が「module アリーナを指す浅い別名」を保持している:
 *   A: moduleinst->types (= module->types.data + 各 rt1/rt2) … invoke marshalling / func import 型検査
 *   B: exportinst->name  (= export->name 浅いコピー)        … invoke 関数解決 / 他モジュール import 解決
 *   C: core m->imports[].name/.module (= import 名 alias)   … invoke の host 呼出名判定
 *   D: origin_module.types/tags (cross-module TAG import 型検査)
 *   E: S->extra_func_type[i] (= &module->types[...] host import 型検査)
 * これらは全て module_t 自身の .data 由来なので、ここで types/imports/export名/tags を store へ
 * ディープコピーして module_t の .data を張り替えれば、下流 (resolve_imports/instantiate/origin_module/
 * build) は全て store コピーを参照する。結果、load 完了後に module アリーナ (バッファ) を解放できる。
 * 呼び出し: decode 後・resolve_imports 前 (CHANGE_STORE_MEMORY 済で store が current の状態)。 */
static kinowasm_result_t kw_relocate_module_metadata_to_store(module_t* m)
{
	_try{
		/* types: 配列本体を store へ → 各 rt1/rt2 (valtype バイト列) を store へ。旧配列は解放。 */
		if(m->types.len != 0) {
			void* old = m->types.data;
			size_t n = m->types.len;
			_throwiferr(kinowasm_array_new_from(m->types, n));
			memcpy(m->types.data, old, sizeof(*m->types.data) * n);
			kinowasm_array_foreach(ft, functiontype_t, m->types) {
				_throwiferr(kw_reloc_str_to_store(&ft->rt1));   /* 旧 rt1/rt2 は kw_reloc_str_to_store が解放 */
				_throwiferr(kw_reloc_str_to_store(&ft->rt2));
			}
			kinowasm_mem_free(old);   /* 旧 types 配列本体 (module アリーナ) を解放 */
		}
		/* imports: 配列本体を store へ → 各 module/name 文字列を store へ (C/D)。旧配列は解放。 */
		if(m->imports.len != 0) {
			void* old = m->imports.data;
			size_t n = m->imports.len;
			_throwiferr(kinowasm_array_new_from(m->imports, n));
			memcpy(m->imports.data, old, sizeof(*m->imports.data) * n);
			kinowasm_array_foreach(im, import_t, m->imports) {
				_throwiferr(kw_reloc_str_to_store(&im->module));   /* 旧 module/name 文字列は kw_reloc_str_to_store が解放 */
				_throwiferr(kw_reloc_str_to_store(&im->name));
			}
			kinowasm_mem_free(old);   /* 旧 imports 配列本体 (module アリーナ) を解放 */
		}
		/* tags: 配列本体を store へ (D の cross-module tag 型検査が origin_module.tags をスキャン)。旧配列は解放。 */
		if(m->tags.len != 0) {
			void* old = m->tags.data;
			size_t n = m->tags.len;
			_throwiferr(kinowasm_array_new_from(m->tags, n));
			memcpy(m->tags.data, old, sizeof(*m->tags.data) * n);
			kinowasm_mem_free(old);   /* 旧 tags 配列本体 (module アリーナ) を解放 */
		}
		/* exports: name 文字列のみ store へ (B)。export 配列本体は instantiate でしか読まれず、
		 * exportinst->name が name struct を値コピーで捕捉するので本体の再配置は不要。 */
		kinowasm_array_foreach(ex, export_t, m->exports)
			_throwiferr(kw_reloc_str_to_store(&ex->name));
	}
	_catch:
	return _result;
}

/* kw_free_module_transient — load 完了後に不要になるデコード済み module_t の配列群を module アリーナ
 * から個別解放する。load 後も参照される types/imports/tags/export 名は kw_relocate_module_metadata_to_store
 * で store アリーナへ移済みなので解放しない (origin_module が参照)。関数本体の instr_t 列は
 * kw_parser_code_body_ex がデコード後に解放済 (func->body=NULL)、const 式は永続ブロックへ memcpy 退避
 * 済なので、ここでは kalloc 配列 (localvalues / init / 各 section 配列本体) のみ解放する。const 式の
 * 永続ブロックは module アリーナの wholesale 解放で回収される。
 * 注: store の funcinst.code は m->functions[i] を指すが core エンジンでは load 後に読まれない (型は
 * moduleinst->types = store 側を参照)。exports 本体を解放しても name 文字列は store 側 (relocate 済) で残る。 */
static void kw_free_module_transient(module_t* m)
{
	kinowasm_array_foreach(fn, function_t, m->functions)
		kinowasm_array_term_from(fn->localvalues);
	kinowasm_array_term_from(m->functions);

	kinowasm_array_foreach(dt, data_t, m->datas)
		kinowasm_array_term_from(dt->init);
	kinowasm_array_term_from(m->datas);

	kinowasm_array_foreach(el, element_t, m->elements)
		kinowasm_array_term_from(el->init);
	kinowasm_array_term_from(m->elements);

	kinowasm_array_term_from(m->tables);
	kinowasm_array_term_from(m->memorys);
	kinowasm_array_term_from(m->globals);
	kinowasm_array_term_from(m->exports);   /* name 文字列は store 側 (relocate 済) なので配列本体のみ解放 */
	kinowasm_array_term_from(m->declared_funcs);
}

/* register_loaded_module — ロード完了した moduleinst を名前付きで store の moduletable へ登録する。
 * 動的モジュール (push/pop) で capacity が累積しないよう、空きがあれば grow しない ensure-capacity。
 * grow を怠ると array_at(.., len) が one-past-capacity の OOB write になるため OOM を必ず検査する。 */
static kinowasm_result_t register_loaded_module(store_t* store, moduleinst_t* moduleinst,
	const module_t* m, const char* modulename, kinowasm_mem_info_t mem)
{
	_try{
		if(store->moduletable.len == store->moduletable.capacity)
			_throwiferr(kinowasm_array_grow_from(store->moduletable, 1));
		moduletable_t* mt = &array_at(store->moduletable, store->moduletable.len);
		size_t name_len = strlen(modulename);
		_throwiferr(array_new(mt->name, name_len + 1));
		memcpy(mt->name.data, modulename, name_len);
		array_at(mt->name, name_len) = '\0';
		moduleinst->origin_module = *m;
		mt->module = moduleinst;
		mt->memory = mem;
		store->moduletable.len++;
	}
	_catch:
	return _result;
}

kinowasm_result_t kinowasm_load_module_from_memory(kinowasm_handle_t S, void* module_data, size_t module_size, const char* modulename, kinowasm_mem_info_t modulememory)
{
	store_t* store = NULL;
	kinowasm_mem_info_t mem = NULL;
	externvals_t externvals = { 0 };
	int externvals_created = 0;
	_try{
		store = (store_t*)S;
		_throwif(ERR_NOTMODULEINIT, store == NULL);
		_throwif(ERR_FILENOTOPEN, module_data == NULL || module_size == 0);

		if(modulememory == NULL)
			mem = store->storememory;

		/* モジュール読み込み用メモリを確保し切り替える */
		INIT_MODULE_MEMORY(mem, modulememory);

		kinowasm_mem_set_info(mem);
		kinowasm_buf_t file_buffer;
		kinowasm_buf_set(&file_buffer, module_data, module_size);
		module_t m;
		module_init(&m);
		/* デコード(ここではまだ実体がない) */
		_throwiferr(kinowasm_parse_module(&m, &file_buffer));
		/* ストア(WASM実行用)へ切り替える */
		CHANGE_STORE_MEMORY(store);
		/* load 後も参照されるメタデータ (types/import・export名/tags) を module アリーナから
		 * store アリーナへ移し、moduleinst を自己完結化する (これ以降 module アリーナは解放可能)。
		 * アリーナ一本化 (modulememory == storememory) の場合は decode 済みのまま store にあるので
		 * relocate 不要 (コピー+旧解放の無駄を省く)。 */
		if((kinowasm_mem_info_t)mem != store->storememory)
			_throwiferr(kw_relocate_module_metadata_to_store(&m));
		/* インポート情報を解決する。resolve_imports は内部で externvals を確保するため、
		 * 途中失敗 (unknown import 等) でも _catch で term させるようフラグは呼び出し前に立てる
		 * (externvals は {0} 初期化済みなので未確保での term も no-op で安全)。 */
		externvals_created = 1;
		_throwiferr(resolve_imports(store, &m, &externvals));
		moduleinst_t* moduleinst;
		/* モジュールを実体化する */
		/* 仕様: instantiation が active elem/data segment や start で trap しても、それまでに
		 * import 済み shared table/memory へ適用された副作用は残り、確保済み関数も呼出可能。
		 * core エンジンは store を snapshot するため、失敗 moduleinst (kw_instantiate が orphan list
		 * 末尾へ退避済) の core instance を構築して owner の共有資源へ副作用を再同期 + 関数を登録
		 * してから trap を伝播する。 */
		{
			kinowasm_result_t inst_res = kw_instantiate(store, &m, &externvals, &moduleinst);
			if(inst_res != RES_SUCCESS && (store->state_flags & STATE_FLAG_SUSPENDED) == 0
			    && kw_is_trap_result(inst_res) && store->orphan_moduleinsts.len > 0) {
				moduleinst_t* failed = kinowasm_array_at(store->orphan_moduleinsts, store->orphan_moduleinsts.len - 1);
				if(failed != NULL)
					/* origin_module は kw_instantiate の _catch (orphan 積み時) で設定済み。 */
					(void)kw_core_build_from_store(store, failed, (const uint8_t*)module_data, module_size);
			}
			_throwiferr(inst_res);
		}
		/* ファイルデータやモジュール用メモリは必要ないので解放 */
		array_term(externvals);
		externvals_created = 0;

		/* core 統合: instantiate 済み store + 生 wasm から core flat ランタイムを構築し (build)、
		 * start 関数を core エンジンで実行する。start が trap した場合は
		 * uninstantiable: moduletable 登録前に throw する (core instance + 副作用は残る = spec 準拠)。
		 * module_data は呼出側が所有し有効。core compiler は生コードを coreinstr へ翻訳して自己完結する。 */
		moduleinst->origin_module = m;   /* build が参照 (登録は後段)。 */

		/* デコード出力に build / start は依存しない: 関数本体は kw_parser_code_body_ex が
		 * 本体ごとにスクラッチを確保→デコード後に即解放済み (core は生 wasm から compile するため
		 * func->body は dead)、const 式は同関数が永続ブロックへ memcpy 退避し instantiate (上の :836)
		 * で評価済。 */
		if(kw_core_build_from_store(store, moduleinst, (const uint8_t*)module_data, module_size) != 0) {
			(void)kw_orphan_moduleinst(store, moduleinst);   /* 未登録 moduleinst を term 解放対象に (リーク防止) */
			_throw(RES_ERROR);
		}
		if(m.has_start && m.startidx >= m.funcimport_count) {
			/* 定義済み start を build 済みインスタンス (build が active 化済) で実行。trap で uninstantiable。 */
			const char* trap_msg = NULL;
			if(kw_core_invoke_ex(m.startidx, NULL, 0, NULL, &trap_msg) != 0) {
				(void)kw_orphan_moduleinst(store, moduleinst);   /* 未登録 moduleinst を term 解放対象に (リーク防止) */
				_throw(kw_core_trap_to_result(trap_msg));   /* start 関数の trap → 具体コードへ写像 (uninstantiable) */
			}
		}

		/* WASI reactor 規約: モジュールが "_initialize" (型 () -> ()) を export していれば、
		 * 他の export を呼び出す前に一度だけ実行する。Emscripten の standalone (--no-entry)
		 * ビルドはグローバル C++ コンストラクタ等の初期化コードをここに置くため、未呼出だと
		 * グローバル ctor が一度も走らず、namespace スコープの非トリビアル定数が空のまま
		 * 参照される (2026-07-22 修正)。型が () -> () でない export は reactor 規約外と
		 * みなし呼び出さない (既存モジュールの後方互換)。trap は start と同様
		 * uninstantiable として扱う。 */
		{
			externval_t initval;
			if(find_export_n(moduleinst, "_initialize", sizeof("_initialize") - 1, &initval) == RES_SUCCESS
			    && initval.kind == IMPORTDESC_FUNC
			    && (size_t)initval.func < store->funcs.len) {
				functioninstance_t* initfunc = &kinowasm_array_at(store->funcs, initval.func);
				if(initfunc->type != NULL && initfunc->type->rt1.len == 0 && initfunc->type->rt2.len == 0) {
					kinowasm_args_t noargs = { 0 };
					kinowasm_result_t init_res = kw_core_invoke_marshalled(store, initval.func, &noargs);
					if(init_res != RES_SUCCESS) {
						(void)kw_orphan_moduleinst(store, moduleinst);   /* 未登録 moduleinst を term 解放対象に (リーク防止) */
						_throw(init_res);
					}
				}
			}
		}

		/* ロード完了したモジュールを名前付きで moduletable へ登録する。登録失敗 (OOM) 時は
		 * moduletable に載らないため、build/start 失敗と同様 orphan へ積んで term の解放対象にする
		 * (register_loaded_module は成功時のみ len++ するので orphan との二重解放はない)。 */
		{
			kinowasm_result_t reg_res = register_loaded_module(store, moduleinst, &m, modulename, mem);
			if(reg_res != RES_SUCCESS) {
				(void)kw_orphan_moduleinst(store, moduleinst);
				_throw(reg_res);
			}
		}

		/* デコードに使った module アリーナ (modulememory) の一時データを個別解放して空に戻す。
		 * 永続データは全て store アリーナへ移済み (relocate / instantiate / core build)。
		 * origin_module に対して解放し、解放済み配列を空化して dangling を残さない
		 * (types/imports/tags は store 側を指すので保持される)。 */
		kw_free_module_transient(&moduleinst->origin_module);
	}
	_catch:
	/* CHANGE_STORE_MEMORY は内部に _throwif (goto _catch) を含むため _catch 内で使うと
	 * storememory==NULL のとき自分自身へ戻る無限ループ (ハング) になる。復帰は throw しない
	 * best-effort の set のみ行う。 */
	if(store != NULL && store->storememory != NULL)
		kinowasm_mem_set_info(store->storememory);

	if(externvals_created)
		array_term(externvals);

	return _result;
}

/* kinowasm_load_module — ファイルパスから WASM バイナリを読み込み実体化する。
 * modulememory に切り替えてファイルを読み込み、バイト列を kinowasm_load_module_from_memory
 * に渡してロードする。fopen/fread に失敗した場合や途中エラー時は _catch で fp を閉じ
 * 読み込みバッファを解放する。
 * 引数:
 *   S            - 対象ハンドル
 *   modulefile   - 読み込む WASM ファイルのパス
 *   modulename   - ストアに登録するモジュール名
 *   modulememory - モジュール読み込みに使う外部メモリ管理情報
 * 戻り値: 成功時 RES_SUCCESS、ファイル未オープンで ERR_FILENOTOPEN 等の ERR_* */
kinowasm_result_t kinowasm_load_module(kinowasm_handle_t S, const char* modulefile, const char* modulename, kinowasm_mem_info_t modulememory)
{
	store_t* store = NULL;
	kinowasm_mem_info_t mem = NULL;
	u8karray_t file_data = { 0 };
	FILE* fp = NULL;
	_try{
		store = (store_t*)S;
		_throwif(ERR_NOTMODULEINIT, store == NULL);

		if(modulememory == NULL)
			mem = store->storememory;

		/* モジュール読み込み用メモリを確保し切り替える */
		INIT_MODULE_MEMORY(mem, modulememory);

		kinowasm_mem_set_info(mem);
		fp = fopen(modulefile, "rb");
		_throwif(ERR_FILENOTOPEN, fp == NULL);
		fseek(fp, 0, SEEK_END);
		_throwiferr(array_new(file_data, ftell(fp)));
		fseek(fp, 0, SEEK_SET);
		/* #28: short read で未初期化のバッファ末尾をモジュールとしてデコードしないよう、
		 * fread の戻り値が要求長と一致することを確認する (file_data は malloc で非ゼロ初期化)。 */
		size_t read_len = fread(file_data.data, sizeof(uint8_t), file_data.len, fp);
		_throwif(ERR_FILENOTOPEN, read_len != file_data.len);
		fclose(fp);
		fp = NULL;
		_throwiferr(kinowasm_load_module_from_memory(store, file_data.data, file_data.len, modulename, mem));
		array_term(file_data);
	}
	_catch:
	if(fp != NULL)
		fclose(fp);

	if(file_data.data != NULL)
		array_term(file_data);

	return _result;
}

/* kinowasm_lookup_func_n — module::funcname を一度だけ解決して関数ハンドルを返す。
 * moduletable から一致モジュールを探し、funcname の export を find_export_n で解決して
 * その funcaddr を funcref として返す。毎フレーム呼び出す用途で名前検索 (O(モジュール数)
 * +O(export数)) をロード時 1 回に抑えるための前段。embedded NUL を含む関数名に対応する。
 * 引数:
 *   S            - 対象ハンドル
 *   module       - 関数を持つモジュール名 (NUL 終端)
 *   funcname     - 解決する export 関数名
 *   funcname_len - funcname の実バイト長
 *   out          - 解決した関数ハンドルを書き出す先 (失敗時 KINOWASM_FUNCREF_INVALID)
 * 戻り値: 成功時 RES_SUCCESS、未存在モジュール ERR_UNKNOWN_IMPORT_SYMBOL、
 *         未存在関数 ERR_UNKNOWN_IMPORT、export が非関数 RES_ERROR 等 */
kinowasm_result_t kinowasm_lookup_func_n(kinowasm_handle_t S, const char* module, const char* funcname, size_t funcname_len, kinowasm_funcref_t* out)
{
	_try{
		store_t* store = (store_t*)S;
		if(out != NULL)
			*out = KINOWASM_FUNCREF_INVALID;
		_throwif(ERR_NOTMODULEINIT, store == NULL || out == NULL);
		CHANGE_STORE_MEMORY(store);
		_throwif(ERR_NOTMODULEINIT, store->moduletable.len == 0);
		foreach(mod, moduletable_t, store->moduletable) {
			if(strcmp(module, (const char*)mod->name.data) == 0) {
				externval_t externval;
				_throwiferr(find_export_n(mod->module, funcname, funcname_len, &externval));
				_throwif(RES_ERROR, externval.kind != IMPORTDESC_FUNC);
				*out = (kinowasm_funcref_t)externval.func;
				_throw(RES_SUCCESS);  /* 解決完了。残りのモジュールは探さない */
			}
		}
		/* どのモジュール名にも一致しなかった */
		_throw(ERR_UNKNOWN_IMPORT_SYMBOL);
	}
	_catch:
	return _result;
}

/* kinowasm_lookup_func — kinowasm_lookup_func_n の NUL 終端名ラッパー。 */
kinowasm_result_t kinowasm_lookup_func(kinowasm_handle_t S, const char* module, const char* funcname, kinowasm_funcref_t* out)
{
	return kinowasm_lookup_func_n(S, module, funcname, strlen(funcname), out);
}

/* kinowasm_invoke_func — 解決済み関数ハンドルを名前検索なし (O(1)) で呼び出す。
 * サスペンド中でないことと、ハンドルが現在の store の関数範囲内であることを確認し、
 * ストアメモリへ切り替えて core エンジンで実行する。
 * 引数:
 *   S        - 対象ハンドル
 *   func     - kinowasm_lookup_func で取得した関数ハンドル
 *   argument - 呼出引数 (兼戻り値受け取り)
 * 戻り値: 成功時 RES_SUCCESS、未初期化/サスペンド中/不正ハンドル等で ERR_* */
kinowasm_result_t kinowasm_invoke_func(kinowasm_handle_t S, kinowasm_funcref_t func, kinowasm_args_t* argument)
{
	_try{
		store_t* store = (store_t*)S;
		_throwif(ERR_NOTMODULEINIT, store == NULL);
		_throwif(ERR_FUNCTION_ALREADY_SUSPENDED, (store->state_flags & STATE_FLAG_SUSPENDED));
		/* stale / 不正ハンドルによる wild 呼び出しを防ぐ範囲検査 (host func id も弾く)。 */
		_throwif(ERR_INVALID_FUNC_PARAM, func < 0 || (size_t)func >= store->funcs.len);
		CHANGE_STORE_MEMORY(store);
		/* core register-TOS エンジンで実行する。 */
		_throwiferr(kw_core_invoke_marshalled(store, (funcaddr_t)func, argument));
	}
	_catch:
	return _result;
}

/* kinowasm_invoke_n — モジュールの export 関数を名前 (バイト長指定) で呼び出す。
 * kinowasm_lookup_func_n で解決し kinowasm_invoke_func で実行する薄いラッパー。
 * embedded NUL を含む関数名に対応する。
 * 引数:
 *   S            - 対象ハンドル
 *   module       - 呼び出す関数を持つモジュール名
 *   funcname     - 呼び出す export 関数名
 *   funcname_len - funcname の実バイト長
 *   argument     - 呼出引数 (兼戻り値受け取り)
 * 戻り値: 成功時 RES_SUCCESS、未初期化/サスペンド中/解決失敗等で ERR_* */
kinowasm_result_t kinowasm_invoke_n(kinowasm_handle_t S, const char* module, const char* funcname, size_t funcname_len, kinowasm_args_t* argument)
{
	_try{
		kinowasm_funcref_t func;
		_throwiferr(kinowasm_lookup_func_n(S, module, funcname, funcname_len, &func));
		_throwiferr(kinowasm_invoke_func(S, func, argument));
	}
	_catch:
	{
		/* 診断: KW_REENTRY_LOG=1 で host からの export 呼び出しを 1 行出力する。
		 * exec は呼び出し時点で core が実行中だったか (= この invoke は再入)。 */
		static int s_log = -1;
		if(s_log < 0) {
			const char* ev = getenv("KW_REENTRY_LOG");
			s_log = (ev != NULL && *ev != '0') ? 1 : 0;
		}
		if(s_log) {
			fprintf(stderr, "[kw] invoke %s::%.*s exec=%d rc=%u\n",
				module ? module : "?", (int)funcname_len, funcname,
				kw_core_is_executing(), (unsigned)_result);
			fflush(stderr);
		}
	}
	return _result;
}

/* kinowasm_invoke — モジュールの export 関数を NUL 終端名で呼び出す。
 * strlen で長さを求め kinowasm_invoke_n に委譲する簡易ラッパー。
 * 引数:
 *   S        - 対象ハンドル
 *   module   - 呼び出す関数を持つモジュール名
 *   funcname - 呼び出す export 関数名 (NUL 終端)
 *   argument - 呼出引数 (兼戻り値受け取り)
 * 戻り値: kinowasm_invoke_n の結果 (RES_SUCCESS / ERR_*) */
kinowasm_result_t kinowasm_invoke(kinowasm_handle_t S, const char* module, const char* funcname, kinowasm_args_t* argument)
{
	return kinowasm_invoke_n(S, module, funcname, strlen(funcname), argument);
}

/* kinowasm_resume — サスペンド中の実行を再開する。
 * SUSPENDED 状態であることを確認しフラグを解除、ストアメモリへ切り替え、
 * kw_core_resume で core エンジンの実行を続行する。
 * ホスト関数呼出で中断した処理の継続に使う。
 * 引数:
 *   S        - 対象ハンドル
 *   argument - 再開時に受け渡す引数 (兼戻り値受け取り)
 * 戻り値: 成功時 RES_SUCCESS、未初期化や非サスペンド時は ERR_NOTMODULEINIT / ERR_NOT_RESUME */
kinowasm_result_t kinowasm_resume(kinowasm_handle_t S, kinowasm_args_t* argument)
{
	_try{
		store_t* store = (store_t*)S;
		_throwif(ERR_NOTMODULEINIT, store == NULL);
		_throwif(ERR_NOT_RESUME, !(store->state_flags & STATE_FLAG_SUSPENDED));
		CHANGE_STORE_MEMORY(store);
		/* ここでサスペンド状態を解除する (再 yield 時は下で再設定) */
		store->state_flags &= ~STATE_FLAG_SUSPENDED;

		/* 再開対象 entry 関数の型 (結果列の marshalling 用) を控える。 */
		functioninstance_t* funcinst = &kinowasm_array_at(store->funcs, g_core_suspend_funcaddr);
		functiontype_t* functype = funcinst->type;

		int64_t results[64];
		for(uint32_t i = 0; i < 64; i++)
			results[i] = 0;

		int rc = kw_core_resume(results);
		if(rc == 2) {
			/* 再 yield: 再び SUSPENDED にして host code を伝播。argument は据置。 */
			store->state_flags |= STATE_FLAG_SUSPENDED;
			_throw((kinowasm_result_t)kw_core_suspend_code());
		}
		if(rc)
			_throw(kw_core_trap_to_result(g_rt != NULL ? g_rt->trap_msg : NULL));

		/* 完走: 最終結果を argument へ詰め直す (invoke 成功時と同手順)。 */
		kinowasm_array_term_from(*argument);
		_throwiferr(kinowasm_array_new_from(*argument, functype->rt2.len));
		for(size_t i = 0; i < functype->rt2.len; i++) {
			kinowasm_arg_t* ret = &argument->data[i];
			ret->type = kinowasm_array_at(functype->rt2, i);
			ret->val.num.i64 = results[i];
		}
	}
	_catch:
	return _result;
}

/* kinowasm_reset_store — ストアの実行状態 (スタック) を初期状態に戻す。
 * ストアメモリへ切り替えてフレームをリセットし、frame インデックスを
 * -1 (空) に戻す。モジュール登録やメモリ確保はそのまま保持する。
 * 引数:
 *   S - 対象ハンドル
 * 戻り値: 成功時 RES_SUCCESS、未初期化時は ERR_NOTMODULEINIT */
kinowasm_result_t kinowasm_reset_store(kinowasm_handle_t S)
{
	_try{
		store_t* store = (store_t*)S;
		_throwif(ERR_NOTMODULEINIT, store == NULL);
		CHANGE_STORE_MEMORY(store);
		kw_reset_frames(store);
		store->stack->frame_idx = -1;
	}
	_catch:
	return _result;
}

/* kinowasm_read_memory — 現在実行中フレームの線形メモリから読み出す。
 * call から現在フレームの memory 0 を取得し、address から len バイトを data へコピーする。
 * ホスト関数が WASM 側メモリを参照するために使う。
 * 引数:
 *   call    - 現在の呼出情報 (実行中ストア・フレームを保持)
 *   address - 読み出し開始アドレス (線形メモリ内オフセット)
 *   data    - 読み出し先バッファ
 *   len     - 読み出すバイト数
 * 戻り値: 全 len バイト読み出し成功で RES_SUCCESS、memory 未宣言/範囲外は
 *   ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS、ページ確保 OOM は ERR_OUTOFMEMORY。
 *   void→戻り値追加は後方互換のため、戻り値を無視する既存呼び出し側は従来どおり動作する。 */
kinowasm_result_t kinowasm_read_memory(kinowasm_callinfo_t* call, uint32_t address, void* data, size_t len)
{
	if(g_core_host_mem != NULL) {   /* core エンジン: flat mem を直接読む */
		if(len > g_core_host_mem_size || (uint64_t)address > g_core_host_mem_size - len)
			return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;
		memcpy(data, g_core_host_mem + address, len);
		return RES_SUCCESS;
	}
	/* セキュリティ: memory を持たないモジュールの core 経由ホスト呼び出しは g_core_host_mem==NULL
	 * かつ current_store==NULL で届く (kw_core_invoke_host)。store 経路の frame 参照前に弾く。 */
	if(call == NULL || call->current_store == NULL)
		return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;
	GET_CURRENT_MEMORY(call, store, mem);
	if(mem == NULL)
		return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;	/* セキュリティ #2: memory 未宣言モジュールは転送不可 */
	/* セキュリティ #4: WASI 経由は VM の load/store 境界検査を経ないため、ここで
	 * num_pages 範囲を検査する。範囲外は転送しない (線形メモリ分離の維持)。 */
	uint64_t mem_size = (uint64_t)mem->num_pages * WASM_PAGE_SIZE;
	if(len > mem_size || (uint64_t)address > mem_size - len)
		return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;
	memcpy(data, mem->base + address, len);   /* flat 線形メモリを直接読む */
	return RES_SUCCESS;
}

/* kinowasm_write_memory — 現在実行中フレームの線形メモリへ書き込む。
 * call から現在フレームの memory 0 を取得し、data から len バイトを address へ書き込む。
 * ホスト関数が WASM 側メモリへ結果を返すために使う。
 * 引数:
 *   call    - 現在の呼出情報 (実行中ストア・フレームを保持)
 *   address - 書き込み開始アドレス (線形メモリ内オフセット)
 *   data    - 書き込むデータの先頭ポインタ
 *   len     - 書き込むバイト数
 * 戻り値: 全 len バイト書き込み成功で RES_SUCCESS、memory 未宣言/範囲外は
 *   ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS、ページ確保 OOM は ERR_OUTOFMEMORY。
 *   void→戻り値追加は後方互換のため、戻り値を無視する既存呼び出し側は従来どおり動作する。 */
kinowasm_result_t kinowasm_write_memory(kinowasm_callinfo_t* call, uint32_t address, const void* data, size_t len)
{
	if(g_core_host_mem != NULL) {   /* core エンジン: flat mem を直接書く */
		if(len > g_core_host_mem_size || (uint64_t)address > g_core_host_mem_size - len)
			return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;
		memcpy(g_core_host_mem + address, data, len);
		return RES_SUCCESS;
	}
	/* セキュリティ: memory を持たないモジュールの core 経由ホスト呼び出しは g_core_host_mem==NULL
	 * かつ current_store==NULL で届く (kw_core_invoke_host)。store 経路の frame 参照前に弾く。 */
	if(call == NULL || call->current_store == NULL)
		return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;
	GET_CURRENT_MEMORY(call, store, mem);
	if(mem == NULL)
		return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;	/* セキュリティ #2: memory 未宣言モジュールは転送不可 */
	/* セキュリティ #4: WASI 経由は VM の load/store 境界検査を経ないため、ここで
	 * num_pages 範囲を検査する。範囲外は転送しない (線形メモリ分離の維持)。 */
	uint64_t mem_size = (uint64_t)mem->num_pages * WASM_PAGE_SIZE;
	if(len > mem_size || (uint64_t)address > mem_size - len)
		return ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS;
	memcpy(mem->base + address, data, len);   /* flat 線形メモリへ直接書く */
	return RES_SUCCESS;
}

/* kw_core_invoke_host — core エンジン (kw_core_wasi.c) から KinoWASM 既存ホスト関数
 *   (extrafunction.c の WASI 実装等) を呼び出す橋渡し。
 *   name で extra_func_table を検索 → slot 引数を kinowasm_args_t へ詰めて呼ぶ。
 *   param_types は functype.rt1 の valtype 列 (呼出側が渡す)。各引数を宣言型で正しくタグ付け
 *   するのに使う (NULL なら従来どおり全引数 I64 とみなす)。ExtraFunc 側の引数型検証を通す。
 *   ホスト関数のメモリアクセスは g_core_host_mem に向く (read/write_memory を上で分岐済)。
 *   import 解決用の wasi_link_stub (standalone の no-op プレースホルダ) はスキップし、
 *   呼出側の最小 WASI フォールバックへ落とす。
 *   戻り値: 0=成功(*ret に rets[0]=errno)、2=ホストが trap、-1=未登録/スタブ。 */
extern kinowasm_result_t wasi_link_stub(kinowasm_callinfo_t* call);
int kw_core_invoke_host(const char* module, const char* name, const int64_t* slot_args, uint32_t nargs,
	const uint8_t* param_types, uint32_t nrets, uint8_t* mem, uint64_t mem_size, int64_t* ret)
{
	/* import 解決は (module, name) で行う。同名異モジュール (例: "env"."Wait" のシステム wait "vf" と
	 * "__internal"."Wait" の ADV Wait "vi") を取り違えると別関数を呼んでしまうため、まず module+name の
	 * 完全一致を優先する。見つからなければ従来どおり name のみで一致させ、module 未指定の旧経路との
	 * 後方互換を保つ。 */
	kinowasm_extrafuncaddress_t func = NULL;
	int stub_hit = 0;
	if(module != NULL) {
		for(size_t i = 0; i < extra_func_table.len; i++) {
			if(strcmp(array_at(extra_func_table, i).name, name) != 0)
				continue;

			const char* em = array_at(extra_func_table, i).module;
			if(em == NULL || strcmp(em, module) != 0)
				continue;

			kinowasm_extrafuncaddress_t f = array_at(extra_func_table, i).func;
			/* 解決スタブは無視→最小 WASI へ */
			if(f == wasi_link_stub) {
				stub_hit = 1;
				break;
			}
			func = f;
			break;
		}
	}
	if(func == NULL && !stub_hit) {
		for(size_t i = 0; i < extra_func_table.len; i++) {
			if(strcmp(array_at(extra_func_table, i).name, name) != 0)
				continue;

			kinowasm_extrafuncaddress_t f = array_at(extra_func_table, i).func;
			if(f == wasi_link_stub)
				break;   /* 解決スタブは無視→最小 WASI へ */

			func = f;
			break;
		}
	}
	if(func == NULL)
		return -1;   /* 未登録/スタブ → 呼出側で最小 WASI フォールバック */

	/* 引数列 / 戻り値列を確保。
	 * セキュリティ: ホスト関数は固定インデックスで引数を読む (例: path_open は data[8] まで)。
	 * ゲストが import を本来より少ない引数数で宣言すると arglist が短くなり、ホスト関数が
	 * 宣言数を超えた slot を読んで未初期化/隣接ヒープへの OOB read になる。ホスト関数が
	 * 参照しうる最大引数数 (host_min_args) まで確保し、宣言を超える分は 0 で埋める。 */
	const uint32_t host_min_args = 16;   /* 現状の最大は path_open の 9 引数。余裕を見て 16。 */
	kinowasm_args_t arglist = { 0 }, retlist = { 0 };
	uint32_t alloc_args = (nargs > host_min_args) ? nargs : host_min_args;
	if(kinowasm_array_new_from(arglist, alloc_args) != RES_SUCCESS)
		return -1;
	for(uint32_t i = 0; i < nargs; i++) {
		/* 引数の型は呼出側 (core) が functype.rt1 から渡す valtype 列で決める。これが無い場合のみ
		 * 従来どおり I64 とみなす。値ビットは union なので i32/f32 等も低位ビットがそのまま読める。 */
		array_at(arglist, i).type = (param_types != NULL) ? param_types[i] : TYPE_VAL_I64;
		array_at(arglist, i).val.num.i64 = slot_args[i];
	}
	/* 宣言数を超える padding slot を 0 で埋める (過小アリティ import からの OOB/未初期化 read 防止)。 */
	for(uint32_t i = nargs; i < alloc_args; i++) {
		array_at(arglist, i).type = TYPE_VAL_I32;
		array_at(arglist, i).val.num.i64 = 0;
	}
	arglist.len = nargs;
	if(kinowasm_array_new_from(retlist, nrets ? nrets : 1) != RES_SUCCESS) {
		kinowasm_array_term_from(arglist);
		return -1;
	}
	retlist.len = nrets;

	/* ホスト関数のメモリ参照を core flat mem に向ける (read/write_memory が分岐)。 */
	uint8_t* saved_mem = g_core_host_mem;
	uint64_t saved_size = g_core_host_mem_size;
	g_core_host_mem = mem;
	g_core_host_mem_size = mem_size;
	kinowasm_callinfo_t call = { &arglist, &retlist, NULL };
	kinowasm_result_t r = func(&call);
	g_core_host_mem = saved_mem;
	g_core_host_mem_size = saved_size;

	if(ret != NULL && nrets > 0)
		*ret = array_at(retlist, 0).val.num.i64;
	kinowasm_array_term_from(arglist);
	kinowasm_array_term_from(retlist);
	/* 0=成功。非success はホストの code をそのまま返す (例: ERR_NEXTFRAME_YIELD=0x2000)。
	 * core 側 (core_call_host) が suspend を起爆し、その code を caller へ伝播する。 */
	return (r == RES_SUCCESS) ? 0 : (int)r;
}
