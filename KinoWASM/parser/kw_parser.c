
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "module.h"
#include "internal_macros.h"
#include "kinowasm.h"
#include "KinoUtil/kbuffer.h"
#include "KinoUtil/kstack.h"
#include "KinoUtil/exception.h"
#include "exceptioncode.h"

/* パーサ補助関数のうち、複数の PARSE_* マクロ経由で多数の case に展開され得る
 * ものは非インライン化してバイナリサイズを抑える。パース (デコード) はモジュールロード時の
 * 一度きりで実行性能に影響しないため、関数呼出し化が安全。 */
#if defined(_MSC_VER) && !defined(__clang__)
#define KW_PARSER_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define KW_PARSER_NOINLINE __attribute__((noinline))
#else
#define KW_PARSER_NOINLINE
#endif

/* ============================================================================
 * kw_parser.c — WASM バイナリ → モジュール構造 (module_t) パーサ + 検証
 *   (生成する instr_t IR は検証・const 式評価の副産物。実行コードは core が
 *    生 bytes から別途コンパイルするため、ここでは実行用バイナリを生成しない。)
 *
 * 【全体の流れ】
 *   kinowasm_parse_module(mod, buf)                   ... エントリ。section を順に読む
 *     - kw_parser_section_order() で section の出現順を検証
 *     - 各 section を kw_parser_type / import / function / table / memory /
 *       global / export / start / element / data / datacount / tag / code が処理
 *     - code section: kw_parser_code -> kw_parser_code_body -> kw_parser_code_body_ex
 *         関数本体を 1 命令ずつ kw_parser_code_instr でデコード + 型検証する
 *         (fusion 等の最適化は行わない。最適化は core コンパイラの責務)。
 *
 * 【命令デコードの構造 (BEGIN_PARSE / PARSE_OP / END_PARSE)】
 *   kw_parser_code_instr() は opcode の巨大 switch。可読性のため case 本体を
 *   カテゴリ別の .inc (parser_ops_base / parser_ops_ext / parser_op_simd) に分割し、以下のマクロで 1 関数に組み立てる:
 *     BEGIN_PARSE   関数頭 + opcode 読取 + const-expr 許可 op 検証 + switch 開始
 *     PARSE_OP(op)  `case op:` (各 .inc 内の handler 先頭)
 *     END_PARSE     default + switch/関数を閉じる
 *   .inc: control / core(変数,テーブル,メモリ,定数) / numeric / fc / simd / threads
 *
 * 【デコード出力の用途】core は raw bytes から自前コンパイルするため、関数本体の命令列は
 *   デコード直後に破棄される (kw_parser_code_body_ex 末尾)。命令列を実際に読むのは const 式
 *   (global init / elem・data offset) を評価する kw_eval_const_expr のみ。
 * ============================================================================ */

/* デコード済み命令 1 個分を本体スクラッチ (kw_parser_code_body_ex が確保) へ確保する。
 * 関数本体の命令列はデコード直後に破棄される (読み手が居ない) ため、先頭スロットを使い回して
 * スクラッチを 1 命令分に抑える。const 式のみ kw_eval_const_expr が後から走査するので
 * 連続ストリームとして積み上げる。 */
#define CODE_ASSIGN() \
	(is_constexpr ? instr_assign(&memory_block, &memory_count, CODE_WITH_EX_SIZE) \
	              : (memory_count = 0, instr_assign(&memory_block, &memory_count, CODE_WITH_EX_SIZE)))
#define read_u8(value, buf) kinowasm_buf_read_u8((value), (buf))
#define read_u7_leb(value, buf) kinowasm_buf_read_u7_leb128((value), (buf))
#define read_u32_leb(value, buf) kinowasm_buf_read_u32_leb128((value), (buf))
#define read_u64_leb(value, buf) kinowasm_buf_read_u_leb128((value), 64, (buf))
/* WASM 3.0 multi-memory 対応: memarg をまとめて読み込むマクロ。
 * align field の bit 6 が立っていれば memidx (u32 leb) が続く仕様。
 * offset は memory64 で u64 LEB になるため 64bit で読み、全幅を offset64_out へ返す。
 * memarg_t.offset (u32) には下位 32bit を格納する — memory64 の load/store は parser で
 * OP_MEM64_* に rewrite され core compiler が raw bytes から u64 offset を再読みするため、
 * 切詰め格納は無害。memory32 で u32 超を弾く検証は呼出側 (memtype 解決後) が行う。 */
#define READ_MEMARG_EX(memarg, offset64_out, memidx_out) do { \
	uint32_t _ma_align; \
	uint32_t _ma_memidx = 0; \
	_throwiferr(read_u32_leb(&_ma_align, param->buf)); \
	if(_ma_align & 0x40u) { \
		_ma_align &= ~0x40u; \
		_throwiferr(read_u32_leb(&_ma_memidx, param->buf)); \
		_throwif(ERR_MALFORMED_FUNC, _ma_memidx >= param->total_memories); \
	} \
	(memidx_out) = _ma_memidx; \
	(memarg).memidx = (uint16_t)_ma_memidx; \
	_throwif(ERR_MALFORMED_ALIGN, _ma_align > UINT16_MAX); \
	(memarg).align = (uint16_t)_ma_align; \
	_throwiferr(read_u64_leb(&(offset64_out), param->buf)); \
	(memarg).offset = (uint32_t)(offset64_out); \
} while(0)
/* 従来形: memory32 前提の命令 (SIMD / atomic 等) 用。u32 超の offset は malformed。 */
#define READ_MEMARG(memarg) do { \
	uint64_t _ma_off64; \
	uint32_t _ma_idx32; \
	READ_MEMARG_EX(memarg, _ma_off64, _ma_idx32); \
	(void)_ma_idx32; \
	_throwif(ERR_MALFORMED_FUNC, _ma_off64 > 0xFFFFFFFFull); \
} while(0)
#define read_s32_leb(value, buf) kinowasm_buf_read_s_32_leb128((value), (buf))
#define read_s64_leb(value, buf) kinowasm_buf_read_s_64_leb128((value), (buf))
#define read_f32(value, buf) kinowasm_buf_read_f32((value), (buf))
#define read_f64(value, buf) kinowasm_buf_read_f64((value), (buf))
#define read_bytes(value, size, buf) kinowasm_buf_read_bytes((value), (size), (buf))
typedef enum {
	MODE_NONE,
	MODE_BLOCK,
	MODE_BLOCK_LOOP,
	MODE_BLOCK_ELSE_LOOP,
} mode_t;

/* WASM 3.0 Exception Handling: try block 内で OP_CATCH/OP_CATCH_ALL を遭遇順に
 * リンクしていくため、parser_stack_t に直前 catch の ip を保持する。 */

typedef enum {
	OP_FLAG_NONE = 0,
	OP_FLAG_CALCULATE = 1,
} operand_flag_t;

typedef enum {
	INSTRFLAG_NONE = 0,
	INSTRFLAG_USE_INSTR_EX = 1,
} instr_flag_t;

typedef struct {
	uint32_t n;
	uint8_t  type;
} local_t;

typedef array(functiontype_t) funcs_array_t;
typedef array(tabletype_t) tables_array_t;

typedef struct {
	int32_t idx;
	int32_t max_idx;
	u8karray_t pool;
	uint8_t polymorphic;
	uint8_t overflowed;	/* #19: pool 容量超過の push を検出。関数デコード後に弾く */
} type_stack_t;

typedef struct {
	kinowasm_stack_t link;
	/* この枠を開いた命令の opcode (OP_BLOCK/OP_LOOP/OP_IF/OP_TRY/OP_TRY_TABLE)。
	 * br の結果型解決 (loop は rt1、それ以外は rt2) や catch/delegate の妥当性検証で参照する。
	 * デコード済み命令列 (IR) は関数本体では破棄されるため、検証に必要な種別はここに持つ。 */
	uint16_t head_opcode;
	mode_t mode;
	int32_t type_stack_idx;
	uint8_t type_stack_polymorphic;
	blocktype_t blocktype;
	/* WASM 3.0 EH: 現在 catch arm を decode 中なら 1 (rethrow validation 用)。
	 * OP_CATCH / OP_CATCH_ALL を見た時点で立ち、try block 終了時に消える。 */
	uint8_t is_catch_arm;
	uint8_t catch_all_seen; /* A legacy catch_all must be the final handler. */
} parser_stack_t;

typedef struct {
	module_t* mod;
	kinowasm_buf_t* buf;
	code_t instr;
	uint8_t instr_flag;
	uint8_t is_constexpr;
	uint8_t expected_result_type;

	/* バリデーション用コンテキスト */
	uint32_t total_locals;
	/* WASM-level local idx → 内部スロットオフセット (v128 は 2 slot 占有)。
	 * localvalues は WASM-level local idx をキーとし、各エントリは元の type
	 * (V128 は 1 entry のまま) を保持する。 */
	u32karray_t* local_offsets;
	uint32_t total_funcs;
	uint32_t total_tables;
	uint32_t total_memories;
	uint32_t total_globals;
	uint32_t total_types;
	uint32_t total_datas;
	uint32_t total_elems;
	
	uint32_t label_depth;
	int32_t type_stack_limit_idx;
	type_stack_t* type_stack;
	u8karray_t* localvalues;
	funcs_array_t* funcs;
	tables_array_t* tables;
	parser_stack_t* root_block;
	parser_stack_t* current_block;
	function_t* current_func;
} parser_param_t;

static kinowasm_result_t kw_parser_resolve_blocktype(functiontype_t* ty, blocktype_t bt, module_t* mod, uint8_t* valtype_buf);

/* get_memtype_by_idx — memidx に対応する memtype を返す (WASM 3.0 multi-memory + memory64)。
 * import memory が前にあり、その後に module 内 memory が続くインデックス空間を辿る。
 * 引数:
 *   mod    - 対象モジュール
 *   memidx - メモリインデックス (import + module 内 memory の連番)
 * 戻り値: 該当 memtype へのポインタ / 不明な memidx には NULL (caller が validate) */
static const memorytype_t* get_memtype_by_idx(module_t* mod, uint32_t memidx)
{
	uint32_t imp_count = 0;
	foreach(im, import_t, mod->imports) {
		if(im->d.kind == IMPORTDESC_MEMORY) {
			if(imp_count == memidx)
				return &im->d.memory;

			imp_count++;
		}
	}
	uint32_t local_idx = memidx - imp_count;
	if(local_idx < mod->memorys.len) {
		return &array_at(mod->memorys, local_idx).memtype;
	}
	return NULL;
}

/* kw_parser_ensure_declared_funcs — declared_funcs ビットマップを遅延初期化する。
 * 未初期化なら (import + module 内 function の総数ぶん) 確保して 0 クリアする。
 * 引数:
 *   mod - 対象モジュール
 * 戻り値: 成功時 RES_SUCCESS / 確保失敗時 ERR_* エラーコード */
static kinowasm_result_t kw_parser_ensure_declared_funcs(module_t* mod)
{
	_try{
		if(mod->declared_funcs.item_size == 0)
			array_init(mod->declared_funcs);
		if(mod->declared_funcs.data == NULL) {
		  uint32_t total_funcs = mod->funcimport_count + (uint32_t)mod->functions.len;
			_throwiferr(array_new(mod->declared_funcs, total_funcs));
			memset(mod->declared_funcs.data, 0, mod->declared_funcs.len);
		}
	}
	_catch:
	return _result;
}

/* kw_parser_mark_declared_func — funcidx を「宣言済み関数」としてマークする。
 * ref.func / export 経由で参照される関数を記録し、後段の参照可否検証に使う。
 * 引数:
 *   mod     - 対象モジュール
 *   funcidx - マークする関数インデックス
 * 戻り値: 成功時 RES_SUCCESS / 範囲外なら ERR_MALFORMED_FUNC 等のエラーコード */
static kinowasm_result_t kw_parser_mark_declared_func(module_t* mod, uint32_t funcidx)
{
	_try{
		_throwiferr(kw_parser_ensure_declared_funcs(mod));
		_throwif(ERR_MALFORMED_FUNC, funcidx >= mod->declared_funcs.len);
		array_at(mod->declared_funcs, funcidx) = 1;
	}
	_catch:
	return _result;
}

/* kw_parser_is_declared_func — funcidx が宣言済み関数としてマーク済みか判定する。
 * 引数:
 *   mod     - 対象モジュール
 *   funcidx - 判定する関数インデックス
 * 戻り値: マーク済みなら非0、未マーク / 未初期化 / 範囲外なら 0 */
static int kw_parser_is_declared_func(module_t* mod, uint32_t funcidx)
{
	if(mod->declared_funcs.data == NULL)
		return 0;
	if(funcidx >= mod->declared_funcs.len)
		return 0;
	return array_at(mod->declared_funcs, funcidx) != 0;
}

/* kw_parser_get_functype — funcidx に対応する関数型 (functiontype_t) を返す。
 * import function を先に、続いて module 内 function を走査するインデックス空間を辿る。
 * 引数:
 *   mod     - 対象モジュール
 *   funcidx - 関数インデックス (import + module 内 function の連番)
 * 戻り値: 該当する functiontype_t へのポインタ / 範囲外なら NULL */
static functiontype_t* kw_parser_get_functype(module_t* mod, uint32_t funcidx)
{
	uint32_t import_func_idx = 0;
	foreach(import, import_t, mod->imports) {
		if(import->d.kind != IMPORTDESC_FUNC)
			continue;
		if(import_func_idx == funcidx)
			return &array_at(mod->types, import->d.functypeidx);
		import_func_idx++;
	}

	funcidx -= mod->funcimport_count;
	if(funcidx >= mod->functions.len)
		return NULL;
	return &array_at(mod->types, array_at(mod->functions, funcidx).typeidx);
}

static functiontype_t* kw_parser_get_tag_functype(module_t* mod, uint32_t tagidx)
{
	if(tagidx < mod->tagimport_count) {
		uint32_t idx = 0;
		foreach(import, import_t, mod->imports) {
			if(import->d.kind != IMPORTDESC_TAG)
				continue;

			if(idx++ == tagidx)
				return &mod->types.data[import->d.tag.typeidx];
		}
	} else {
		tagidx -= mod->tagimport_count;
		if(tagidx < mod->tags.len)
			return &mod->types.data[mod->tags.data[tagidx].tagtype.typeidx];
	}
	return NULL;
}

/* デコード済み命令列 (instr_t 列) は core エンジンでは実行されない (core は raw wasm バイトから
 * 直接コンパイルする)。よって関数本体は型検証の副産物にすぎずデコード後は dead で、const 式
 * (global init / elem・data offset) のみ instantiate で評価される。
 * 本体 (関数本体 / const 式) ごとに入力バイト数から上限を見積った単一連続スクラッチを
 * module アリーナから確保し (kw_parser_code_body_ex)、関数本体はデコード後に解放、const 式は
 * 使用分だけ正確サイズへ memcpy して永続化する。memory_block/count はデコード中の本体
 * スクラッチを指す (decode は単一スレッド・非再入)。 */
static void*    memory_block = NULL;     /* 現在デコード中の本体のスクラッチ先頭 */

/* const 式用スクラッチの使い回し。
 * ★const 式 1 本ごとに malloc / free していたが、emscripten の wasm は
 *   data segment が非常に多く (実測 41,061 個)、1 個につき約 2.6MB の
 *   確保と解放が走って data section のデコードだけで 4.4 秒かかっていた。
 *   デコードは単一スレッド・非再入なので 1 本持って使い回せばよい。
 *   モジュール 1 本のデコードが終わったら kw_parser_release_scratch で手放す。 */
static void*    constexpr_scratch = NULL;
static size_t   constexpr_scratch_cap = 0;

/* 命令デコードの型検証で使う、全関数 / 全テーブルの型参照配列。
 * ★中身は module にしか依存しないのに、本体 1 本ごとに作り直していた。
 *   関数 56,700 個 + data segment 41,061 個のモジュールで約 55 億回の
 *   構造体コピーになり、decode だけで 10.7 秒かかっていた
 *   (ゲームの起動 12 秒の主因)。デコードは単一スレッド・非再入なので
 *   static で 1 本持ち、module が変わったときだけ作り直す。 */
static funcs_array_t  cached_func_refs = { 0 };
static tables_array_t cached_table_refs = { 0 };
static module_t*      cached_refs_mod = NULL;
static uint32_t       cached_refs_funcs = 0;
static uint32_t       cached_refs_tables = 0;

/* モジュール 1 本のデコードが終わったところで呼ぶ。 */
void kw_parser_release_scratch(void)
{
	if(constexpr_scratch != NULL) {
		kinowasm_mem_free(constexpr_scratch);
		constexpr_scratch = NULL;
		constexpr_scratch_cap = 0;
	}
	array_term(cached_func_refs);
	array_term(cached_table_refs);
	cached_refs_mod    = NULL;
	cached_refs_funcs  = 0;
	cached_refs_tables = 0;
}
static uint32_t memory_count = 0;        /* スクラッチ内の使用済みバイト数 */
static size_t   memory_capacity = 0;     /* スクラッチの容量 (byte)。instr_assign が overflow を弾く */

/* memory_count を巻き戻す。underflow が起きるとそれは parser のロジックバグであり、
 * クランプして続行しても出力が壊れた状態で WASM 側に渡るだけで安全ではない。
 * 早期検知のため _throwif で例外化する。呼び出し元には _catch: ラベルが必要。
 * ERR_MALFORMED_FUNC を流用 (parser 内部不整合の指標として)。 */
#define ROLLBACK_MEMORY_COUNT(n) \
	do { \
		const uint32_t _roll = (uint32_t)(n); \
		_throwif(ERR_MALFORMED_FUNC, memory_count < _roll); \
		memory_count -= _roll; \
	} while(0)

#define POP_REG(reg, ty) \
	do { \
		reg = (uint16_t)(param->total_locals + type_stack->idx); \
		_throwif(ERR_TYPE_MISMATCH, type_stack_pop(type_stack, param->type_stack_limit_idx, ty)); \
	} while(0)

#define PUSH_REG(reg, ty) \
	do { \
		type_stack_push(type_stack, ty); \
		reg = (uint16_t)(param->total_locals + type_stack->idx); \
	} while(0)

/* v128 (TYPE_VAL_V128) は 16 byte = 隣接する 2 つの scalar slot に格納する。
 * type_stack には V128 marker を 2 連続でプッシュし、slot index は下位側
 * (LSB 寄り) を返す。runtime は *(kinowasm_v128_t*)&locals[r0] で 16 byte
 * 読み書きする (slot は 8 byte align だが unaligned load 想定)。 */
#define PUSH_REG_V128(reg) \
	do { \
		type_stack_push(type_stack, TYPE_VAL_V128); \
		reg = (uint16_t)(param->total_locals + type_stack->idx); \
		type_stack_push(type_stack, TYPE_VAL_V128); \
	} while(0)

#define POP_REG_V128(reg) \
	do { \
		reg = (uint16_t)(param->total_locals + type_stack->idx - 1); \
		_throwif(ERR_TYPE_MISMATCH, type_stack_pop(type_stack, param->type_stack_limit_idx, TYPE_VAL_V128)); \
		_throwif(ERR_TYPE_MISMATCH, type_stack_pop(type_stack, param->type_stack_limit_idx, TYPE_VAL_V128)); \
	} while(0)

/* resulttype の各要素に対して逆順 POP / 順 PUSH を行う頻出パターン。
 * 結果値レジスタは捨てるため UNUSE で警告抑制。 */
#define POP_RESULTTYPE_REVERSE(rt) \
	foreach_reverse(_t, uint8_t, rt) { \
		UNUSE uint16_t _r; \
		POP_REG(_r, *_t); \
	}
#define PUSH_RESULTTYPE(rt) \
	foreach(_t, uint8_t, rt) { \
		UNUSE uint16_t _r; \
		PUSH_REG(_r, *_t); \
	}

/* 制御命令 (unreachable / br / br_table / return) 後のスタック polymorphic 化。
 * 以後の型は不定として扱い、ラベル境界まで型スタックを巻き戻す。 */
#define DROP_STACK_TO_POLYMORPHIC() do { \
	type_stack->polymorphic = 1; \
	while(type_stack->idx > param->type_stack_limit_idx) \
		type_stack_pop(type_stack, param->type_stack_limit_idx, 0); \
} while(0)

/* デコード時の単項命令共通パターン: pop(pop_type) → push(push_type), r0/r1 セット
 * COPY 吸収は DEFINE_NOP_OP 命令(REINTERPRET 等)で r0/r1 を使わないため
 * PARSE_UNARY 内では行わず、個別ハンドラで明示的に呼び出す */
#define PARSE_UNARY(pop_type, push_type) \
	do { \
		uint16_t r1, r0; \
		POP_REG(r1, pop_type); \
		PUSH_REG(r0, push_type); \
		param->instr->r0 = r0; \
		param->instr->r1 = r1; \
	} while(0)

/* デコード時の二項命令共通パターン: pop(pop_type) x2 → push(push_type), r0/r1/r2 セット。 */
#define PARSE_BINARY(pop_type, push_type) \
	do { \
		uint16_t r2, r1, r0; \
		POP_REG(r2, pop_type); \
		POP_REG(r1, pop_type); \
		PUSH_REG(r0, push_type); \
		param->instr->r0 = r0; \
		param->instr->r1 = r1; \
		param->instr->r2 = r2; \
	} while(0)

#define SET_SELECT_INSTR(r0, r1, r2, r3) \
	do { \
		param->instr->r0 = (r0); \
		param->instr->r1 = (r1); \
		param->instr->r2 = (r2); \
		param->instr_flag |= INSTRFLAG_USE_INSTR_EX; \
		instr_ex->val = (r3); \
	} while(0)

/* BLOCK_END — ブロック終端 (end / delegate) の共通後処理。
 * 型スタックをブロック入場時点へ戻して結果型を積み直し、ブロック枠を pop してラベル階層を上げ、
 * 後続命令用のスロットを確保する。 */
#define BLOCK_END() { \
	/* 次の命令（ブロックの後続）を確保 */ \
	code_t block_next = CODE_ASSIGN(); \
	_throwif(ERR_OUTOFMEMORY, !block_next); \
	/* ブロックを戻す */ \
	{ \
		blocktype_t saved_bt = cur->blocktype; \
		int32_t saved_idx = cur->type_stack_idx; \
		type_stack.idx = saved_idx; \
		type_stack.polymorphic = cur->type_stack_polymorphic; \
		functiontype_t bt; \
		uint8_t vt_buf[2]; \
		kw_parser_resolve_blocktype(&bt, saved_bt, mod, vt_buf); \
		foreach(rt, uint8_t, bt.rt2) \
			type_stack_push(&type_stack, *rt); \
	} \
	instr = block_next; \
	instr_ex = (code_ex_t)(instr + 1); \
	kinowasm_mem_free(kinowasm_stack_poptail_entry(&stack, parser_stack_t, link)); \
	/* ラベル階層を1つ上へ */ \
	if(label_depth > 0) \
	label_depth--; \
	cur = kinowasm_stack_tail_entry(&stack, parser_stack_t, link); \
}

#define BEGIN_PARSE \
static kinowasm_result_t kw_parser_code_instr(parser_param_t* param) \
{ \
	_try{ \
	code_ex_t instr_ex = (code_ex_t)(param->instr + 1); \
	type_stack_t* type_stack = param->type_stack; \
	param->instr_flag = INSTRFLAG_NONE; \
	param->instr->opcode = 0; /* 次のコードでは下位ビットしか更新されないため0クリアを行う */ \
	_throwiferr(read_u8((uint8_t*)&param->instr->opcode, param->buf)); \
	if(param->is_constexpr) { \
		switch(param->instr->opcode) { \
		case OP_I32_CONST: \
		case OP_I64_CONST: \
		case OP_F32_CONST: \
		case OP_F64_CONST: \
		case OP_GLOBAL_GET: \
		case OP_REF_NULL: \
		case OP_REF_FUNC: \
		case OP_0XFD: /* SIMD prefix; v128.const のみ const expr で許可されるため後段で sub-op をチェックする */ \
		/* WASM 3.0 extended const expressions: i32/i64 の add/sub/mul を許可 */ \
		case OP_I32_ADD: case OP_I32_SUB: case OP_I32_MUL: \
		case OP_I64_ADD: case OP_I64_SUB: case OP_I64_MUL: \
		case OP_END: \
			break; \
		default: \
			_throw(RES_ERROR); \
		} \
	} \
	/* 読み込みとバリデーションを処理 */ \
	switch(param->instr->opcode) {

#define PARSE_OP(op) case op:

#define END_PARSE \
		default: \
		/* printf("Decode: unsupported opcode: 0x%x\n", param->instr->opcode); */ \
		_throw(RES_ERROR); \
		} \
	} \
	_catch: \
	return _result; \
}

/* instr_assign — 本体スクラッチから assign_size バイトを切り出し 0 クリアして先頭を返す。
 * スクラッチは kw_parser_code_body_ex が入力バイト数から上限を見積って事前確保しているため、
 * 通常は溢れない。単一連続バッファへ bump するだけ。
 * kalloc は user 領域を初期化せず、スクラッチも確保時に全域クリアしない (実使用の数十倍の
 * 保守的見積りのため)。未書き込みの ex/block 領域が偶然読まれても安全なよう、切り出した
 * 分だけここで 0 クリアする (assign_size はほぼ定数 CODE_WITH_EX_SIZE でインライン展開される)。
 * 万一容量を超えた場合は NULL を返し、呼出側が ERR_OUTOFMEMORY として弾く (防御)。
 * 引数:
 *   mem_src     - スクラッチ先頭ポインタへのポインタ (現在は不変)
 *   mem_count   - スクラッチ内使用済みバイト数へのポインタ (更新する)
 *   assign_size - 確保するバイト数
 * 戻り値: 確保した領域の先頭ポインタ / 容量超過時は NULL */
static void* instr_assign(void** mem_src, uint32_t* mem_count, uint32_t assign_size)
{
	if((uint64_t)*mem_count + assign_size > memory_capacity)
		return NULL;
	uint32_t current = *mem_count;
	*mem_count += assign_size;
	void* p = (uint8_t*)(*mem_src) + current;
	memset(p, 0, assign_size);
	return p;
}

/* Type stack functions */
/* type_stack_init — 型検証用スタックを初期化する。
 * idx/max_idx を -1、polymorphic を 0 にし、PARSER_STACK_SIZE 分の pool を確保する。
 * 引数:
 *   stack - 初期化する型スタック
 * 戻り値: 成功時 RES_SUCCESS / pool 確保失敗時 ERR_* エラーコード */
static kinowasm_result_t type_stack_init(type_stack_t* stack)
{
	stack->idx = -1;
	stack->max_idx = -1;
	stack->polymorphic = 0;
	stack->overflowed = 0;
	return array_new(stack->pool, PARSER_STACK_SIZE);
}

/* type_stack_term — 型スタックの pool を解放する。
 * 引数:
 *   stack - 解放する型スタック */
static void type_stack_term(type_stack_t* stack)
{
	 array_term(stack->pool);
}

/* type_stack_push — 型スタックに valtype を 1 つ積む。
 * pool 容量を超える場合は overflowed を立てる (idx は進めない)。max_idx も更新する。
 * #19: 容量超過時に silent drop すると以降の PUSH_REG が同一 slot を返してスロット
 * 別名化/型混同を起こすため、overflowed フラグで検出し関数デコード後に弾く。
 * 引数:
 *   stack - 対象の型スタック
 *   ty    - 積む valtype */
static void type_stack_push(type_stack_t* stack, uint8_t ty)
{
	if(stack->idx + 1 < (int32_t)stack->pool.len) {
		stack->pool.data[++stack->idx] = ty;
		if(stack->idx > stack->max_idx) {
			stack->max_idx = stack->idx;
		}
	} else {
		stack->overflowed = 1;
	}
}

/* type_stack_pop — 型スタックを 1 つ pop し、期待型との不一致を判定する。
 * polymorphic 下で生成された未知型(0)は任意の期待型に適合する。limit_idx 以下
 * (ブロック境界) では polymorphic なら成功、そうでなければ不一致扱い。
 * 引数:
 *   stack     - 対象の型スタック
 *   limit_idx - これ以下は pop 不可とするブロック境界 idx
 *   expect    - 期待する valtype (0 なら型チェックを省略)
 * 戻り値: 成功 (一致) なら 0、型不一致 / 空 pop なら 1 */
static uint8_t type_stack_pop(type_stack_t* stack, int32_t limit_idx, uint8_t expect)
{
	if(stack->idx <= limit_idx)
		return !stack->polymorphic;

	uint8_t ty = stack->pool.data[stack->idx--];
	/* polymorphic下で生成された未知の型(0)は任意の期待型に適合させる */
	if(expect != 0 && ty != 0 && ty != expect)
		return 1;  /* type mismatch */
	return 0;  /* success */
}

/* type_stack_peek — 型スタック最上位の valtype を pop せずに取得する。
 * limit_idx 以下 (空) で polymorphic なら未知型 0 を返す。
 * 引数:
 *   stack     - 対象の型スタック
 *   limit_idx - これ以下を空とみなすブロック境界 idx
 *   t         - 取得した valtype の格納先
 * 戻り値: 成功なら 0、空かつ非 polymorphic なら 1 (エラー) */
static uint8_t type_stack_peek(type_stack_t* stack, int32_t limit_idx, uint8_t* t)
{
	if(stack->idx <= limit_idx) {
		if(!stack->polymorphic)
			return 1;

		*t = 0;
	} else {
		*t = stack->pool.data[stack->idx];
	}
	return 0;
}

/* kw_parser_get_label_resulttype — labelidx で指すブロックの分岐先 result type を返す。
 * current_block から labelidx 段だけ外側へ辿る。root ブロックは関数の rt2、loop は
 * rt1 (ループ先頭へ戻る)、それ以外のブロックは rt2 を採用する。
 * 引数:
 *   rt          - 取得した result type の格納先
 *   param       - デコードコンテキスト (current_block / root_block 等)
 *   labelidx    - 対象ラベルの相対深さ
 *   valtype_buf - blocktype 解決用に呼出側が確保する 2 byte バッファ
 * 戻り値: 成功時 RES_SUCCESS / 失敗時 ERR_* エラーコード */
static kinowasm_result_t kw_parser_get_label_resulttype(resulttype_t* rt, parser_param_t* param, uint32_t labelidx, uint8_t* valtype_buf)
{
	_try{
		parser_stack_t* target = param->current_block;
		for(uint32_t i = 0; i < labelidx; i++)
			target = kinowasm_stack_entry(target->link.prev, parser_stack_t, link);

		if(target == param->root_block) {
			if(param->current_func != NULL) {
				functiontype_t* ftype = &array_at(param->mod->types, param->current_func->typeidx);
				*rt = ftype->rt2;
			} else {
				rt->len = 0;
				rt->data = NULL;
			}
			return RES_SUCCESS;
		}

		functiontype_t label_ft = { 0 };
		_throwiferr(kw_parser_resolve_blocktype(&label_ft, target->blocktype, param->mod, valtype_buf));
		if(target->head_opcode == OP_LOOP)
			*rt = label_ft.rt1;
		else
			*rt = label_ft.rt2;
	}
	_catch:
	return _result;
}

/* A try_table handler branches with tag parameters and an optional exnref.
 * Validate that tuple independently of the body's operand stack. */
static kinowasm_result_t kw_parser_validate_catch(parser_param_t* param, uint8_t kind, uint32_t tagidx, uint32_t labelidx)
{
	_try{
		resulttype_t label_rt;
		uint8_t vt_buf[2];
		_throwiferr(kw_parser_get_label_resulttype(&label_rt, param, labelidx, vt_buf));
		resulttype_t params = { 0 };
		if(kind < 2) {
			functiontype_t* ft = kw_parser_get_tag_functype(param->mod, tagidx);
			_throwif(ERR_MALFORMED_FUNC, ft == NULL);
			params = ft->rt1;
		}
		int has_ref = kind == 1 || kind == 3;
		_throwif(ERR_TYPE_MISMATCH, label_rt.len != params.len + has_ref);
		for(size_t i = 0; i < params.len; i++)
			_throwif(ERR_TYPE_MISMATCH, label_rt.data[i] != params.data[i]);

		_throwif(ERR_TYPE_MISMATCH, has_ref && label_rt.data[params.len] != TYPE_EXNREF);
	}
	_catch:
	return _result;
}

/* kw_parser_validate_resulttype — base_idx より上の型スタック内容が rt に一致するか検証する。
 * 非 polymorphic 時は個数・型を完全一致で確認。polymorphic 時は明示的に積まれた末尾の
 * 型だけを rt の末尾と照合する (未知型 0 は任意型に適合)。
 * 引数:
 *   type_stack - 検証対象の型スタック
 *   base_idx   - 比較の基準とするブロック境界 idx (これより上を検証)
 *   rt         - 期待する result type
 * 戻り値: 一致時 RES_SUCCESS / 不一致なら ERR_TYPE_MISMATCH */
static kinowasm_result_t kw_parser_validate_resulttype(type_stack_t* type_stack, int32_t base_idx, resulttype_t rt)
{
	_try{
		uint32_t stack_len = (uint32_t)(type_stack->idx - base_idx);
		if(!type_stack->polymorphic) {
			_throwif(ERR_TYPE_MISMATCH, stack_len != rt.len);
			for(uint32_t i = 0; i < rt.len; i++)
				_throwif(ERR_TYPE_MISMATCH, type_stack->pool.data[base_idx + 1 + (int32_t)i] != array_at(rt, i));

			return RES_SUCCESS;
		}

		/* polymorphic時は、明示的に積まれている末尾の型だけを結果型と照合する */
		_throwif(ERR_TYPE_MISMATCH, stack_len > rt.len);
		for(uint32_t i = 0; i < stack_len; i++)
		 _throwif(ERR_TYPE_MISMATCH,
				type_stack->pool.data[base_idx + 1 + (int32_t)i] != 0 &&
				type_stack->pool.data[base_idx + 1 + (int32_t)i] != array_at(rt, rt.len - stack_len + i));
	}
	_catch:
	return _result;
}

/* kw_parser_validate_try_section_end — catch/catch_all/delegate が try の body / 直前 handler を
 * 閉じる際の結果型検証 (OP_ELSE / OP_END 相当: rt2 を期待)。これらの opcode は try 直下のみ valid。
 * 必ず DROP_STACK_TO_POLYMORPHIC の前に呼ぶこと (drop 後は polymorphic 緩和で検証が素通りし、
 * 型不足の body が compile 側へ渡って AV する)。 */
static kinowasm_result_t kw_parser_validate_try_section_end(parser_param_t* param)
{
	_try{
		parser_stack_t* blk = param->current_block;
		_throwif(ERR_MALFORMED_BLOCK, blk == param->root_block || blk->head_opcode != OP_TRY);
		_throwif(ERR_MALFORMED_BLOCK, blk->catch_all_seen);
		functiontype_t bt = { 0 };
		uint8_t vt_buf[2];
		_throwiferr(kw_parser_resolve_blocktype(&bt, blk->blocktype, param->mod, vt_buf));
		_throwiferr(kw_parser_validate_resulttype(param->type_stack, blk->type_stack_idx, bt.rt2));
	}
	_catch:
	return _result;
}

/* kw_parser_get_tabletype — tableidx に対応する tabletype を返す。
 * import table を先に、続いて module 内 table を走査するインデックス空間を辿る。
 * 引数:
 *   mod      - 対象モジュール
 *   tableidx - テーブルインデックス (import + module 内 table の連番)
 * 戻り値: 該当する tabletype_t へのポインタ / 範囲外なら NULL */
static tabletype_t* kw_parser_get_tabletype(module_t* mod, uint32_t tableidx)
{
	if(tableidx < mod->tableimport_count) {
		uint32_t import_tableidx = 0;
		foreach(import, import_t, mod->imports) {
			if(import->d.kind != IMPORTDESC_TABLE)
				continue;

			if(import_tableidx == tableidx)
				return &import->d.table;

			import_tableidx++;
		}
		return NULL;
	}

	tableidx -= mod->tableimport_count;
	if(tableidx >= mod->tables.len)
		return NULL;

	return &mod->tables.data[tableidx].tabletype;
}

/* operand_type_size — load/store 命令がアクセスするメモリ幅 (バイト数) を返す。
 * 引数:
 *   op - load/store 系 opcode
 * 戻り値: アクセス幅 (1/2/4/8)。非対応 opcode は UNREACHABLE */
static uint8_t operand_type_size(uint16_t op)
{
	switch(op) {
	case OP_I32_LOAD:
	case OP_F32_LOAD:
	case OP_I64_LOAD32_S:
	case OP_I64_LOAD32_U:
	case OP_I32_STORE:
	case OP_F32_STORE:
	case OP_I64_STORE32:
		return 4;
	case OP_I64_LOAD:
	case OP_F64_LOAD:
	case OP_I64_STORE:
	case OP_F64_STORE:
		return 8;
	case OP_I32_LOAD8_S:
	case OP_I32_LOAD8_U:
	case OP_I64_LOAD8_S:
	case OP_I64_LOAD8_U:
	case OP_I32_STORE8:
	case OP_I64_STORE8:
		return 1;
	case OP_I32_LOAD16_S:
	case OP_I32_LOAD16_U:
	case OP_I64_LOAD16_S:
	case OP_I64_LOAD16_U:
	case OP_I32_STORE16:
	case OP_I64_STORE16:
		return 2;
	default:
		UNREACHABLE();
	}
}

/* operand_value_type — load/store 命令が扱う値の valtype を返す。
 * 引数:
 *   op - load/store 系 opcode
 * 戻り値: TYPE_VAL_I64 / TYPE_VAL_F32 / TYPE_VAL_F64、それ以外は TYPE_VAL_I32 */
static uint8_t operand_value_type(uint16_t op)
{
	if(op == OP_I64_LOAD || op == OP_I64_STORE ||
		(op >= OP_I64_LOAD8_S && op <= OP_I64_LOAD32_U) ||
		(op >= OP_I64_STORE8 && op <= OP_I64_STORE32))
		return TYPE_VAL_I64;
	if(op == OP_F32_LOAD || op == OP_F32_STORE)
		return TYPE_VAL_F32;
	if(op == OP_F64_LOAD || op == OP_F64_STORE)
		return TYPE_VAL_F64;
	return TYPE_VAL_I32;
}

/* is_valid_utf8 — バイト列が well-formed な UTF-8 か検証する (WASM の name 検証用)。
 * import/export 名や custom section の id 等で使用。RFC 3629 準拠で surrogate /
 * overlong / out-of-range を拒否する。
 * 引数:
 *   data - 検証するバイト列
 *   len  - バイト数
 * 戻り値: valid なら 1、invalid なら 0 */
static int is_valid_utf8(const uint8_t* data, size_t len)
{
	size_t i = 0;
	while(i < len) {
		uint8_t b0 = data[i];
		if(b0 < 0x80) {
			/* 1-byte ASCII */
			i += 1;
			continue;
		}
		uint32_t cp;
		size_t need;
		uint32_t cp_min;
		if((b0 & 0xE0) == 0xC0) {
			/* 2-byte: 110xxxxx 10xxxxxx */
			need = 2;
			cp_min = 0x80;
			cp = b0 & 0x1F;
		} else if((b0 & 0xF0) == 0xE0) {
			/* 3-byte: 1110xxxx 10xxxxxx 10xxxxxx */
			need = 3;
			cp_min = 0x800;
			cp = b0 & 0x0F;
		} else if((b0 & 0xF8) == 0xF0) {
			/* 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
			need = 4;
			cp_min = 0x10000;
			cp = b0 & 0x07;
		} else {
			/* 連続バイト単独 / 5+ byte シーケンス開始は invalid */
			return 0;
		}
		if(i + need > len)
			return 0;
		for(size_t k = 1; k < need; k++) {
			uint8_t bk = data[i + k];
			if((bk & 0xC0) != 0x80)
				return 0;
			cp = (cp << 6) | (bk & 0x3F);
		}
		/* overlong / surrogate / out-of-range の拒否 */
		if(cp < cp_min)
			return 0;
		if(cp >= 0xD800 && cp <= 0xDFFF)
			return 0;
		if(cp > 0x10FFFF)
			return 0;
		i += need;
	}
	return 1;
}

/* read_string — buf から LEB128 長付き文字列を読み取り NUL 終端して格納する。
 * 読み取ったバイト列は UTF-8 妥当性を検証する。
 * 引数:
 *   ary - 格納先文字列 (size+1 byte を確保し末尾に '\0' を置く)
 *   buf - 読み取り元バッファ
 * 戻り値: 成功時 RES_SUCCESS / 確保失敗・不正 UTF-8 等で ERR_* エラーコード */
static kinowasm_result_t read_string(string_t* ary, kinowasm_buf_t* buf)
{
	_try{
		uint32_t str_size;
		_throwiferr(read_u32_leb(&str_size, buf));
		/* str_size+1 の u32 wrap 回避 + 残バッファ超の巨大長を早期に弾く。str_size==0xFFFFFFFF だと
		 * str_size+1 が 0 に wrap し array_new(0) が data=NULL を返し、直後の read_bytes が NULL へ
		 * 書き込んでクラッシュする。残量ガードで str_size を現実的サイズに抑え両方を防ぐ。 */
		_throwif(ERR_UNEXPECTED_END, str_size > buf->len - buf->cur);
		_throwiferr(array_new((*ary), str_size + 1));
		_throwiferr(read_bytes(ary->data, str_size, buf));
		ary->data[str_size] = '\0';
		_throwif(ERR_MALFORMED_UTF8, !is_valid_utf8(ary->data, str_size));
	}
	_catch:
	return _result;
}

/* read_limits — table/memory の limits (flag + min [+ max]) を読み取る。
 * flag のビット解釈 (WASM 3.0 memory64 / threads):
 *   bit 0: has_max / bit 1: shared (threads) / bit 2: is_64 (memory64、memory のみ)
 * is_64 のとき min/max は u64 LEB128、それ以外は u32 LEB128。内部では u64 で保持する。
 * 引数:
 *   limits - 読み取り結果の格納先
 *   buf    - 読み取り元バッファ
 * 戻り値: 成功時 RES_SUCCESS / flag が 0x07 超など不正なら ERR_* エラーコード */
static kinowasm_result_t read_limits(limits_t* limits, kinowasm_buf_t* buf)
{
	_try{
		uint8_t flag;
		_throwiferr(read_u7_leb(&flag, buf));
		_throwif(ERR_INTEGER_TOO_LARGE, flag > 0x07);
		limits->has_max = (flag & 0x01) != 0;
		limits->shared  = (flag & 0x02) != 0;
		limits->is_64   = (flag & 0x04) != 0;
		if(limits->is_64) {
			_throwiferr(read_u64_leb(&limits->min, buf));
			if(limits->has_max)
				_throwiferr(read_u64_leb(&limits->max, buf));
			else
				limits->max = 0;
		} else {
			uint32_t v;
			_throwiferr(read_u32_leb(&v, buf));
			limits->min = v;
			if(limits->has_max) {
				_throwiferr(read_u32_leb(&v, buf));
				limits->max = v;
			} else {
				limits->max = 0;
			}
		}
	}
	_catch:
	return _result;
}

/* read_blocktype — block/loop/if 等の blocktype を読み取る。
 * 0x40 (空) や単一 valtype はそのまま valtype に格納。それ以外は型インデックス符号化
 * (負の LEB128 ではない typeidx) として読み戻し valtype=0xFF + typeidx を設定する。
 * 引数:
 *   blk - 読み取り結果の格納先 (valtype / typeidx)
 *   buf - 読み取り元バッファ
 * 戻り値: 成功時 RES_SUCCESS / 不正な typeidx 等で ERR_MALFORMED_BLOCK 等のエラーコード */
static inline kinowasm_result_t read_blocktype(blocktype_t* blk, kinowasm_buf_t* buf)
{
	_try{
		uint8_t valtype;
		_throwiferr(read_u8(&valtype, buf));
		blk->typeidx = 0;
		switch(valtype) {
		case 0x40:
		case TYPE_VAL_I32:
		case TYPE_VAL_I64:
		case TYPE_VAL_F32:
		case TYPE_VAL_F64:
		case TYPE_FUNCREF:
		case TYPE_EXTERNREF:
		case TYPE_EXNREF:    /* WASM 3.0 EH */
			blk->valtype = valtype;
			break;
#if KINOWASM_ENABLE_SIMD
		case TYPE_VAL_V128:
			blk->valtype = valtype;
			break;
#else
		case TYPE_VAL_V128:
			/* SIMD 無効: v128 blocktype を明示拒否する。default の typeidx 経路に
			 * 落とすと 0x7B を型インデックスと誤読し generic error になり、他の v128
			 * gate (global/func/local) と挙動が食い違う。FEATURE_DISABLED で揃える。 */
			_throw(ERR_FEATURE_DISABLED);
#endif
		default:
			buf->cur--;
			blk->valtype = 0xFF;
			{
				int64_t typeidx;
				_throwiferr(read_s64_leb(&typeidx, buf));
				_throwif(ERR_MALFORMED_BLOCK, typeidx < 0 || typeidx > UINT32_MAX);
				blk->typeidx = (uint32_t)typeidx;
			}
			break;
		}
	}
	_catch:
	return _result;
}

/* kw_parser_is_valtype — b が有効な valtype (数値/ベクトル/参照型) かを判定する。
 * 0x00 は type_stack_pop の「任意適合」marker と衝突するため、不正バイトをそのまま
 * 型として受理すると以後の型検査を全面すり抜ける。型バイトを読む全箇所 (functype/
 * globaltype/local 宣言/select_t) で必ず弾く (SIMD gate は従来通り呼出側で
 * ERR_FEATURE_DISABLED として区別する)。 */
static int kw_parser_is_valtype(uint8_t b)
{
	switch(b) {
	case TYPE_VAL_I32:
	case TYPE_VAL_I64:
	case TYPE_VAL_F32:
	case TYPE_VAL_F64:
	case TYPE_VAL_V128:
	case TYPE_FUNCREF:
	case TYPE_EXTERNREF:
	case TYPE_EXNREF:
		return 1;
	}
	return 0;
}

static int kw_parser_is_reftype(uint8_t type)
{
	return type == TYPE_FUNCREF || type == TYPE_EXTERNREF || type == TYPE_EXNREF;
}

/* kw_parser_tabletype — table 型 (reftype + limits) を読み取る。
 * reftype は funcref / externref のいずれかでなければならない。
 * 引数:
 *   tt  - 読み取り結果の格納先
 *   buf - 読み取り元バッファ
 * 戻り値: 成功時 RES_SUCCESS / 不正な reftype で ERR_MALFORMED_REFERENCE_TYPE 等のエラーコード */
static kinowasm_result_t kw_parser_tabletype(tabletype_t* tt, kinowasm_buf_t* buf)
{
	_try{
		_throwiferr(read_u7_leb(&tt->reftype, buf));
		_throwif(ERR_MALFORMED_REFERENCE_TYPE, tt->reftype != TYPE_FUNCREF && tt->reftype != TYPE_EXTERNREF);
		_throwiferr(read_limits(&tt->limits, buf));
		_throwif(ERR_MALFORMED_TABLE, tt->limits.has_max && tt->limits.max < tt->limits.min);
	}
	_catch:
	return _result;
}

/* kw_parser_globaltype — global 型 (valtype + mutability) を読み取る。
 * SIMD 無効ビルドでは V128 を拒否、mut は 0/1 のみ許可する。
 * 引数:
 *   gt  - 読み取り結果の格納先
 *   buf - 読み取り元バッファ
 * 戻り値: 成功時 RES_SUCCESS / 不正な mut 等で ERR_MALFORMED_MUTABILITY 等のエラーコード */
static kinowasm_result_t kw_parser_globaltype(globaltype_t* gt, kinowasm_buf_t* buf)
{
	_try{
		_throwiferr(read_u8(&gt->valtype, buf));
		_throwif(ERR_MALFORMED_GLOBAL, !kw_parser_is_valtype(gt->valtype));
#if !KINOWASM_ENABLE_SIMD
		_throwif(ERR_FEATURE_DISABLED, gt->valtype == TYPE_VAL_V128);
#endif
		_throwiferr(read_u8(&gt->mut, buf));
		_throwif(ERR_MALFORMED_MUTABILITY, gt->mut != 0 && gt->mut != 1);
	}
	_catch:
	return _result;
}

/* kw_parser_read_resulttype_expanded — result type (valtype 列) を読み取り内部表現へ展開する。
 * WASM では valtype 1 個 = 値 1 個だが、内部表現では V128 は隣接 2 slot を占有するため
 * V128 を 2 entry に展開して格納する (type_stack 比較 / param 渡しが自然に動く)。
 * 引数:
 *   rt  - 展開後の result type の格納先 (V128 を 2 entry に展開)
 *   buf - 読み取り元バッファ
 * 戻り値: 成功時 RES_SUCCESS / 長さ超過や確保失敗で ERR_* エラーコード */
static kinowasm_result_t kw_parser_read_resulttype_expanded(resulttype_t* rt, kinowasm_buf_t* buf)
{
	_try{
		uint32_t spec_len;
		_throwiferr(read_u32_leb(&spec_len, buf));
		/* 1 pass: 各 valtype を読み取りバッファに溜める */
		uint8_t tmp[256];
		uint32_t expanded_len = 0;
		_throwif(ERR_MALFORMED_FUNC, spec_len > sizeof(tmp));
		for(uint32_t i = 0; i < spec_len; i++) {
			uint8_t vt;
			_throwiferr(read_u8(&vt, buf));
			_throwif(ERR_MALFORMED_FUNC, !kw_parser_is_valtype(vt));
#if !KINOWASM_ENABLE_SIMD
			_throwif(ERR_FEATURE_DISABLED, vt == TYPE_VAL_V128);
#endif
			tmp[i] = vt;
			expanded_len += (vt == TYPE_VAL_V128) ? 2 : 1;
		}
		_throwiferr(array_new((*rt), expanded_len));
		uint32_t out = 0;
		for(uint32_t i = 0; i < spec_len; i++) {
			rt->data[out++] = tmp[i];
			if(tmp[i] == TYPE_VAL_V128)
				rt->data[out++] = TYPE_VAL_V128;
		}
	}
	_catch:
	return _result;
}

/* kw_parser_type — Type section をパースし mod->types に関数型一覧を構築する。
 * 各エントリは magic 0x60 + 引数 result type (rt1) + 戻り値 result type (rt2)。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / magic 不一致や section サイズ不整合で ERR_* エラーコード */
static kinowasm_result_t kw_parser_type(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t type_size;
		_throwiferr(read_u32_leb(&type_size, buf));
		/* 宣言数が残バッファを超える場合は確保前に弾く (read_string と同様の巨大確保 DoS 防止。
		 * 各エントリは最低 1 byte 消費するため、残量超の宣言数は必ず途中で尽きて失敗する)。
		 * 以降の各 section も同じガードを置く。 */
		_throwif(ERR_UNEXPECTED_END, type_size > buf->len - buf->cur);
		_throwiferr(array_new(mod->types, type_size));
		foreach(item, functiontype_t, mod->types) {
			uint8_t magic;
			_throwiferr(read_u7_leb(&magic, buf));
			_throwif(ERR_MAGICNOTDETECT, magic != 0x60);
			_throwiferr(kw_parser_read_resulttype_expanded(&item->rt1, buf));
			_throwiferr(kw_parser_read_resulttype_expanded(&item->rt2, buf));
		}
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_import — Import section をパースし mod->imports を構築する。
 * func / table / memory / global / tag (WASM 3.0 EH) の各 import を読み取り、種別ごとの
 * 妥当性検証 (型 idx 範囲、memory limits、shared は max 必須 等) と import 数のカウントを行う。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / 不正な import 種別や範囲外 idx で ERR_* エラーコード */
static kinowasm_result_t kw_parser_import(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t import_size;
		_throwiferr(read_u32_leb(&import_size, buf));
		_throwif(ERR_UNEXPECTED_END, import_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		_throwiferr(array_new(mod->imports, import_size));
		foreach(import, import_t, mod->imports) {
			_throwiferr(read_string(&import->module, buf));
			_throwiferr(read_string(&import->name, buf));
			_throwiferr(read_u8(&import->d.kind, buf));
			switch(import->d.kind){
			case IMPORTDESC_FUNC:
				_throwiferr(read_u32_leb(&import->d.functypeidx, buf));
				_throwif(ERR_MALFORMED_FUNC, import->d.functypeidx >= mod->types.len);
				mod->funcimport_count++;
				break;
			case IMPORTDESC_TABLE:
				_throwiferr(kw_parser_tabletype(&import->d.table, buf));
				mod->tableimport_count++;
				break;
			case IMPORTDESC_MEMORY:
				_throwiferr(read_limits(&import->d.memory, buf));
				_throwif(ERR_MALFORMED_MEMORY, import->d.memory.min > 65536);
				if(import->d.memory.has_max) {
					_throwif(ERR_MALFORMED_MEMORY, import->d.memory.max > 65536);
					_throwif(ERR_MALFORMED_MEMORY, import->d.memory.max < import->d.memory.min);
				}
				/* shared memory は max 必須 (WASM threads spec)。 */
				_throwif(ERR_MALFORMED_MEMORY,
					import->d.memory.shared && !import->d.memory.has_max);
				mod->memoryimport_count++;
				/* WASM 3.0 multi-memory: 複数 memory import 可。runtime 側 memaddrs[]
				 * は実行時に十分なサイズを確保する必要がある (現状 1 固定の場合は
				 * 別途調整)。 */
				break;
			case IMPORTDESC_GLOBAL:
				_throwiferr(kw_parser_globaltype(&import->d.globaltype, buf));
				mod->globalimport_count++;
				break;
			case IMPORTDESC_TAG: {
				/* WASM 3.0 exception handling: tag import { attribute, typeidx } */
				_throwiferr(read_u8(&import->d.tag.attribute, buf));
				_throwif(ERR_MALFORMED_FUNC, import->d.tag.attribute != 0);
				_throwiferr(read_u32_leb(&import->d.tag.typeidx, buf));
				_throwif(ERR_MALFORMED_FUNC, import->d.tag.typeidx >= mod->types.len);
				_throwif(ERR_TYPE_MISMATCH, mod->types.data[import->d.tag.typeidx].rt2.len != 0);
				mod->tagimport_count++;
				break;
			}
			default:
				_throw(ERR_UNKNOWN_IMPORT_KIND);
			}
		}
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;

}

/* kw_parser_function — Function section をパースし各 module 関数の typeidx を読み取る。
 * 関数本体は Code section で別途デコードされ、ここでは型インデックスのみ登録する。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / 範囲外 typeidx や section サイズ不整合で ERR_* エラーコード */
static kinowasm_result_t kw_parser_function(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t type_size;
		_throwiferr(read_u32_leb(&type_size, buf));
		_throwif(ERR_UNEXPECTED_END, type_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		_throwiferr(array_new(mod->functions, type_size));
		foreach(func, function_t, mod->functions) {
			_throwiferr(read_u32_leb(&func->typeidx, buf));
			_throwif(ERR_MALFORMED_FUNC, func->typeidx >= mod->types.len);
		}

		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_resolve_blocktype — blocktype を引数/戻り値 result type (functiontype_t) に解決する。
 * 0x40 は空、単一 valtype は rt2 に 1 (V128 は 2) entry、型インデックス指定は mod->types を参照。
 * 引数:
 *   ty          - 解決結果の格納先 (rt1/rt2)
 *   bt          - 解決する blocktype
 *   mod         - 型インデックス参照用モジュール
 *   valtype_buf - 単一 valtype を指す結果用に呼出側が 2 byte 確保するバッファ
 * 戻り値: 成功時 RES_SUCCESS / 範囲外 typeidx で ERR_MALFORMED_BLOCK */
static kinowasm_result_t kw_parser_resolve_blocktype(functiontype_t* ty, blocktype_t bt, module_t* mod, uint8_t* valtype_buf)
{
	_try{
		switch(bt.valtype) {
		case 0x40:
			ty->rt1.len = 0;
			ty->rt1.data = NULL;
			ty->rt2.len = 0;
			ty->rt2.data = NULL;
			break;
#if KINOWASM_ENABLE_SIMD
		case TYPE_VAL_V128:
			ty->rt1.len = 0;
			ty->rt1.data = NULL;
			valtype_buf[0] = TYPE_VAL_V128;
			valtype_buf[1] = TYPE_VAL_V128;
			ty->rt2.len = 2;
			ty->rt2.data = valtype_buf;
			break;
#endif
		case TYPE_VAL_I32:
		case TYPE_VAL_I64:
		case TYPE_VAL_F32:
		case TYPE_VAL_F64:
		case TYPE_FUNCREF:
		case TYPE_EXTERNREF:
		case TYPE_EXNREF:    /* WASM 3.0 EH */
			ty->rt1.len = 0;
			ty->rt1.data = NULL;
			valtype_buf[0] = bt.valtype;
			ty->rt2.len = 1;
			ty->rt2.data = valtype_buf;
			break;
		default:
			if(bt.typeidx < mod->types.len) {
				*ty = array_at(mod->types, bt.typeidx);
			} else {
				_throw(ERR_MALFORMED_BLOCK);
			}
			break;
		}
	}
	_catch:
	return _result;
}

/* kw_parser_get_globaltype — globalidx に対応する globaltype を返す。
 * import global を先に、続いて module 内 global を走査するインデックス空間を辿る。
 * 引数:
 *   mod       - 対象モジュール
 *   globalidx - global インデックス (import + module 内 global の連番)
 * 戻り値: 該当する globaltype_t へのポインタ / 範囲外なら NULL */
static globaltype_t* kw_parser_get_globaltype(module_t* mod, uint32_t globalidx)
{
	if(globalidx < mod->globalimport_count) {
		uint32_t import_globalidx = 0;
		foreach(import, import_t, mod->imports) {
			if(import->d.kind != IMPORTDESC_GLOBAL)
				continue;
			if(import_globalidx == globalidx)
				return &import->d.globaltype;
			import_globalidx++;
		}
		return NULL;
	}

	globalidx -= mod->globalimport_count;
	if(globalidx >= mod->globals.len)
		return NULL;
	return &mod->globals.data[globalidx].globaltype;
}

/* kw_parser_is_constexpr_global — globalidx が const 式から参照可能な global か判定する。
 * const 式で参照できるのは「インポートされた immutable global」のみ。
 * 引数:
 *   mod       - 対象モジュール
 *   globalidx - 判定する global インデックス
 * 戻り値: 参照可 (import かつ immutable) なら 1、それ以外は 0 */
static int kw_parser_is_constexpr_global(module_t* mod, uint32_t globalidx)
{
	if(globalidx >= mod->globalimport_count)
		return 0;
	globaltype_t* g = kw_parser_get_globaltype(mod, globalidx);
	return g != NULL && g->mut == 0;
}

BEGIN_PARSE
/* 命令デコード handler はカテゴリ別 .inc に分割 (可読性のため)。
 * BEGIN_PARSE が開いた switch の case 群をここで構成する。 */
#include "kw_parser_ops_base.inc" /* control + core + numeric (基本命令) */
#include "kw_parser_ops_ext.inc"  /* fc (0xFC bulk/ref/table) + threads (0xFE atomics) */
#include "kw_parser_op_simd.inc"  /* simd (0xFD)。将来の SIMD 整理単位として独立保持 */
END_PARSE

/* kw_parser_build_local_layout — 関数のローカル変数レイアウトを構築する。
 * パラメータ (ftype->rt1) と宣言ローカル (func->localvalues) を WASM-level local idx 順に
 * localvalues[] (各 local の型) と local_offsets[] (内部スロット開始位置) へ展開する。
 * v128 は 1 WASM-local だが内部 2 slot を占めるため offset を 2 進める (rt1 側は
 * kw_parser_read_resulttype_expanded で既に V128 が 2 entry に展開済みなので再集約する)。
 * *total_locals に内部スロット総数 (v128=2 換算) を返す。失敗時は呼び出し側が配列を term する。 */
static kinowasm_result_t kw_parser_build_local_layout(functiontype_t* ftype, function_t* func, uint32_t param_count,
	u8karray_t* localvalues, u32karray_t* local_offsets, uint32_t* total_locals)
{
	_try{
		/* rt1 は V128 を 2 entry に展開済みなので、WASM-level の local 数を数え直す。 */
		uint32_t wasm_param_count = 0;
		for(uint32_t i = 0; i < param_count; )
			i += (array_at(ftype->rt1, i) == TYPE_VAL_V128) ? (wasm_param_count++, 2) : (wasm_param_count++, 1);

		uint32_t wasm_local_count = wasm_param_count + (func != NULL ? (uint32_t)func->localvalues.len : 0);
		_throwiferr(array_new(*localvalues, wasm_local_count));
		_throwiferr(array_new(*local_offsets, wasm_local_count));

		uint32_t wasm_idx = 0;
		uint32_t internal_off = 0;
		/* パラメータを展開 (V128 ペアを 1 entry に再集約しつつ offset は 2 進める)。 */
		for(uint32_t i = 0; i < param_count; ) {
			uint8_t param_type = array_at(ftype->rt1, i);
			(*localvalues).data[wasm_idx] = param_type;
			(*local_offsets).data[wasm_idx] = internal_off;
			wasm_idx++;
			uint32_t slots = (param_type == TYPE_VAL_V128) ? 2 : 1;
			internal_off += slots;
			i += slots;
		}
		/* 宣言ローカルを続けて展開。 */
		if(func != NULL) {
			foreach(local, uint8_t, func->localvalues) {
				(*localvalues).data[wasm_idx] = *local;
				(*local_offsets).data[wasm_idx] = internal_off;
				wasm_idx++;
				internal_off += (*local == TYPE_VAL_V128) ? 2 : 1;
			}
		}
		*total_locals = internal_off;
	}
	_catch:
	return _result;
}

/* kw_parser_build_func_type_refs — 全関数 (import + 定義) の関数型を global func index 順に
 * funcs[] へ集める。命令デコード中の call / call_indirect の型検証で参照する。 */
static kinowasm_result_t kw_parser_build_func_type_refs(module_t* mod, uint32_t total_funcs, funcs_array_t* funcs)
{
	_try{
		_throwiferr(array_new(*funcs, total_funcs));
		int32_t funcidx = 0;
		foreach(import, import_t, mod->imports)
			if(import->d.kind == IMPORTDESC_FUNC)
				(*funcs).data[funcidx++] = array_at(mod->types, import->d.functypeidx);
		foreach(modfunc, function_t, mod->functions)
			(*funcs).data[funcidx++] = array_at(mod->types, modfunc->typeidx);
	}
	_catch:
	return _result;
}

/* kw_parser_build_table_type_refs — 全テーブル (import + 定義) の table 型を tableidx 順に
 * tables[] へ集める。命令デコード中の table.* 系命令の型検証で参照する。 */
static kinowasm_result_t kw_parser_build_table_type_refs(module_t* mod, uint32_t total_tables, tables_array_t* tables)
{
	_try{
		_throwiferr(array_new(*tables, total_tables));
		int32_t tableidx = 0;
		foreach(import, import_t, mod->imports)
			if(import->d.kind == IMPORTDESC_TABLE)
				(*tables).data[tableidx++] = import->d.table;
		foreach(modtable, table_t, mod->tables)
			(*tables).data[tableidx++] = modtable->tabletype;
	}
	_catch:
	return _result;
}

/* kw_parser_code_body_ex — 関数本体 / const 式を 1 命令ずつデコードし instr_t 列を構築する中核。
 * func==NULL & is_constexpr=1 で global init / elem・data offset の const 式にも使う。
 *
 * メインループ (while !eof) の 1 反復:
 *   1. kw_parser_code_instr() で 1 命令を decode + 型検証 (instr_flag に EX/BLOCK 使用有無をセット)。
 *   2. 未使用の instr_ex 領域を ROLLBACK_MEMORY_COUNT で巻き戻し節約。
 *   3. block 開始 (block/loop/if/try) なら block stack を push。
 *   4. OP_END で関数末尾を検出してループ脱出。
 * 命令列はこの本体専用の連続スクラッチ (入口で確保) へ CODE_ASSIGN / instr_assign で bump する。
 * 引数:
 *   mod                  - 対象モジュール (型 / import 情報の参照に使う)
 *   buf                  - 関数本体 / const 式のバイト列バッファ
 *   body                 - 構築した先頭命令ポインタの格納先
 *   func                 - 関数本体なら対象 function_t、const 式なら NULL
 *   is_constexpr         - const 式デコードなら 1
 *   expected_result_type - const 式が残すべき値の valtype (関数本体では未使用)
 * 戻り値: 成功時 RES_SUCCESS / 型不一致・OOM・末尾が OP_END でない等で ERR_* エラーコード */
static kinowasm_result_t kw_parser_code_body_ex(module_t* mod, kinowasm_buf_t* buf, code_t* body, function_t* func, uint8_t is_constexpr, uint8_t expected_result_type)
{
	type_stack_t type_stack = { 0 };
	u8karray_t localvalues;
	array_init(localvalues);
	u32karray_t local_offsets;
	array_init(local_offsets);
	void* body_scratch = NULL;   /* この本体専用の連続スクラッチ (関数本体=後で破棄 / const 式=memcpy 退避) */
	_try{
		kinowasm_stack_t stack;
		kinowasm_stack_init_entry(&stack);

		parser_stack_t root = {0};
		root.mode = MODE_NONE;
		root.type_stack_idx = -1; /* Root stack index */
		root.type_stack_polymorphic = 0;
		kinowasm_stack_push_back(&stack, &root.link);

		parser_stack_t* cur = &root;

		/* この本体用のスクラッチを確保する。
		 *  関数本体: 命令列は誰も読まないので CODE_ASSIGN が先頭スロットを使い回す。1 命令分で足りる。
		 *  const 式: kw_eval_const_expr が後から走査するので連続ストリームを積む。命令数 <= 入力バイト数、
		 *    1 命令の出力 <= CODE_WITH_EX_SIZE なので bytes*CODE_WITH_EX_SIZE で十分。const 式は微小なので
		 *    入力上限を 64KB にクランプし巨大 section での過大確保を防ぐ。 */
		{
			uint64_t cap = CODE_WITH_EX_SIZE;
			if(is_constexpr) {
				uint64_t avail = buf->len - buf->cur;
				if(avail > 65536u)
					avail = 65536u;
				cap = (avail + 2u) * (uint64_t)CODE_WITH_EX_SIZE;
				_throwif(ERR_OUTOFMEMORY, cap > 0x7FFFFFFFu);   /* 1 本体で 2GB 超の出力は非現実的 */
			}
			if(is_constexpr) {
				/* 使い回し。足りなければ取り直す。 */
				if(constexpr_scratch_cap < (size_t)cap) {
					kw_parser_release_scratch();
					constexpr_scratch = kinowasm_mem_malloc((size_t)cap);
					_throwif(ERR_OUTOFMEMORY, constexpr_scratch == NULL);
					constexpr_scratch_cap = (size_t)cap;
				}
				body_scratch = constexpr_scratch;
			} else {
				body_scratch = kinowasm_mem_malloc((size_t)cap);
			}
			_throwif(ERR_OUTOFMEMORY, body_scratch == NULL);
			/* 全域の 0 クリアはしない。0 保証は instr_assign が切り出した分だけ行う。 */
			memory_block = body_scratch;
			memory_count = 0;
			memory_capacity = (size_t)cap;
		}

		/* 最初の命令を確保 */
		code_t instr = CODE_ASSIGN();
		_throwif(ERR_OUTOFMEMORY, !instr);
		*body = instr;

		code_ex_t instr_ex = (code_ex_t)(instr + 1);

		/* label depth for BR checks (root counts as depth 1) */
		uint32_t label_depth = 1;
		uint32_t max_label_depth = 1;

		/* スタック型チェック用 */
		_throwiferr(type_stack_init(&type_stack));

		/* ここで「関数の総ローカル数」を計算して kw_parser_code_body に渡す
		 * 総ローカル数 = パラメータ数 (func type rt1.len) + local_count */
		functiontype_t* ftype = NULL;
		uint32_t param_count = 0;
		uint32_t total_locals = 0;
		if(func != NULL) {
		  _throwif(ERR_MALFORMED_FUNC, func->typeidx >= mod->types.len);
			ftype = &array_at(mod->types, func->typeidx);
			param_count = (uint32_t)ftype->rt1.len;
		}

		/* パラメータ + 宣言ローカルを WASM-level idx 順に展開し localvalues / local_offsets を構築。
		 * total_locals は内部スロット総数 (v128=2 換算)。 */
		_throwiferr(kw_parser_build_local_layout(ftype, func, param_count, &localvalues, &local_offsets, &total_locals));

		/* Prepare validation counts and data needed for processing */
		uint32_t total_funcs = (uint32_t)(mod->funcimport_count + mod->functions.len);
		uint32_t total_tables = (uint32_t)(mod->tableimport_count + mod->tables.len);

		/* 命令デコードの型検証用に、全関数/全テーブルの型を index 順の参照配列へ集める。
		 * ★module ごとに 1 回だけ。本体 1 本ごとに作ると、大きいモジュールで
		 *   デコード時間が関数数の 2 乗で伸びる。 */
		if(cached_refs_mod != mod || cached_refs_funcs != total_funcs
		   || cached_refs_tables != total_tables) {
			array_term(cached_func_refs);
			array_term(cached_table_refs);
			_throwiferr(kw_parser_build_func_type_refs(mod, total_funcs, &cached_func_refs));
			_throwiferr(kw_parser_build_table_type_refs(mod, total_tables, &cached_table_refs));
			cached_refs_mod    = mod;
			cached_refs_funcs  = total_funcs;
			cached_refs_tables = total_tables;
		}

		parser_param_t param = {
			.mod = mod,
			.buf = buf,
			.instr_flag = INSTRFLAG_NONE,
			.is_constexpr = is_constexpr,
			.expected_result_type = expected_result_type,

			/* バリデーション用コンテキスト設定 */
			.total_locals = total_locals,
			.total_funcs = total_funcs,
			.total_tables = total_tables,
			.total_memories = (uint32_t)(mod->memoryimport_count + mod->memorys.len),
			.total_globals = (uint32_t)(mod->globalimport_count + mod->globals.len),
			.total_types = (uint32_t)mod->types.len,
			.total_datas = (uint32_t)mod->datas.len,
			.total_elems = (uint32_t)mod->elements.len,

			.label_depth = label_depth, /* 初期値 */
			.type_stack_limit_idx = root.type_stack_idx,
			.type_stack = &type_stack,
			.localvalues = &localvalues,
			.local_offsets = &local_offsets,
			.funcs = &cached_func_refs,
			.tables = &cached_table_refs,
			.root_block = &root,
			.current_block = &root,
			.current_func = func,
		};

		while(!kinowasm_buf_is_eof(buf)) {
			/* 現在のパラメータをセット */
			param.instr = instr;
			param.label_depth = label_depth;
			param.type_stack_limit_idx = cur->type_stack_idx;
			param.current_block = cur;

			/* 1命令デコードとバリデーションを同時に行う */
			_throwiferr(kw_parser_code_instr(&param));

			if(!(param.instr_flag & INSTRFLAG_USE_INSTR_EX)) {
				/* instrex_tの領域を使用していないのでカウンターを巻き戻してメモリを節約する */
				ROLLBACK_MEMORY_COUNT(sizeof(instr_ex_t));
			}
			cur = kinowasm_stack_tail_entry(&stack, parser_stack_t, link);

			/* ブロック開始 */
			if(instr->opcode == OP_BLOCK ||
				instr->opcode == OP_LOOP ||
				instr->opcode == OP_IF
				|| instr->opcode == OP_TRY      /* WASM 3.0 EH (legacy try) */
				|| instr->opcode == OP_TRY_TABLE /* WASM 3.0 EH try_table */
				)
			{
				parser_stack_t* new_stack = kinowasm_mem_malloc(sizeof(parser_stack_t));
				_throwif(ERR_OUTOFMEMORY, !new_stack);
				kinowasm_stack_push_back(&stack, &new_stack->link);
				new_stack->head_opcode = instr->opcode;
				new_stack->mode = MODE_BLOCK;
				/* ブロック開始時のタイプスタックの状態を保存（パラメータをポップした後） */
				new_stack->type_stack_idx = type_stack.idx;
				new_stack->type_stack_polymorphic = type_stack.polymorphic;
				new_stack->blocktype = instr_ex->blocktype;
				new_stack->is_catch_arm = 0;
				new_stack->catch_all_seen = 0;
				cur = new_stack;
				/* ラベル階層を進める */
				label_depth++;
				if(label_depth > max_label_depth)
					max_label_depth = label_depth;
				/* ブロックのスタックを積む */
				{
					functiontype_t bt = { 0 };
					uint8_t vt_buf[2];
					_throwiferr(kw_parser_resolve_blocktype(&bt, instr_ex->blocktype, mod, vt_buf));
					foreach(pt, uint8_t, bt.rt1) {
						type_stack_push(&type_stack, *pt);
					}
					/* ポリモーフィックフラグはリセット */
					type_stack.polymorphic = 0;
				}
			}

			/* ブロック状態遷移 */
			if(cur->mode == MODE_BLOCK) {
				/* body 先頭の命令スロットを確保して遷移する。 */
				cur->mode = MODE_BLOCK_LOOP;
				instr = CODE_ASSIGN();
				_throwif(ERR_OUTOFMEMORY, !instr);
				instr_ex = (code_ex_t)(instr + 1);
				continue;
			} else if(cur->mode == MODE_BLOCK_LOOP) {
				/* WASM 3.0 EH: try ブロック内で OP_CATCH / OP_CATCH_ALL を遭遇したら、
				 * catch chain に link し、type stack を try 開始時点まで巻き戻して
				 * tag のパラメータ型を push する。OP_CATCH 自体は通常命令として
				 * linear stream に残り、自然到達時の skip-to-end 動作で消費される。 */
				if((cur->head_opcode == OP_TRY) &&
					(instr->opcode == OP_CATCH || instr->opcode == OP_CATCH_ALL)) {
					/* body / 直前 handler の結果型検証は PARSE_OP(OP_CATCH/OP_CATCH_ALL) 側で
					 * DROP_STACK_TO_POLYMORPHIC の前に行う (DROP 後の検証は常に緩和され素通りする)。 */
					/* WASM 3.0 EH: catch arm 突入。OP_RETHROW validation 用に
					 * is_catch_arm を立てる (rethrow L は L 番目の surrounding
					 * label が catch arm を指す場合のみ valid)。 */
					cur->is_catch_arm = 1;
					cur->catch_all_seen = instr->opcode == OP_CATCH_ALL;
					/* type stack を try 開始時点 (rt1 push 前) に reset */
					type_stack.idx = cur->type_stack_idx;
					type_stack.polymorphic = cur->type_stack_polymorphic;
					/* OP_CATCH の場合、tag のパラメータ型を push */
					if(instr->opcode == OP_CATCH) {
						uint32_t tagidx = (uint32_t)instr->r1 | ((uint32_t)instr->r2 << 16);
						uint32_t typeidx = UINT32_MAX;
						if(tagidx < mod->tagimport_count) {
							uint32_t imp_count = 0;
							foreach(im, import_t, mod->imports) {
								if(im->d.kind == IMPORTDESC_TAG) {
									if(imp_count == tagidx) {
										typeidx = im->d.tag.typeidx;
										break;
									}
									imp_count++;
								}
							}
						} else {
							uint32_t local_idx = tagidx - mod->tagimport_count;
							if(local_idx < mod->tags.len)
								typeidx = mod->tags.data[local_idx].tagtype.typeidx;
						}
						_throwif(ERR_MALFORMED_FUNC, typeidx >= mod->types.len);
						functiontype_t* tag_ft = &mod->types.data[typeidx];
						foreach(pt, uint8_t, tag_ft->rt1) {
							type_stack_push(&type_stack, *pt);
						}
					}
					instr = CODE_ASSIGN();
					_throwif(ERR_OUTOFMEMORY, !instr);
					instr_ex = (code_ex_t)(instr + 1);
					continue;
				}
				if(instr->opcode == OP_DELEGATE) {
					/* legacy try-delegate: OP_TRY をパッチして OP_TRY_DELEGATE に変換し、
					 * labelidx を OP_TRY の r1/r2 に格納して BLOCK_END する。
					 * delegate は OP_END と同様にブロック末尾とみなす。 */
					_throwif(ERR_TYPE_MISMATCH, cur->head_opcode != OP_TRY);
					cur->head_opcode = OP_TRY_DELEGATE;
					/* type stack の検証 (OP_END 相当) は PARSE_OP(OP_DELEGATE) 側で drop 前に実施済み。 */
					BLOCK_END();
					continue;
				}
				if(instr->opcode != OP_END && instr->opcode != OP_ELSE) {
					/* 同じブロック内で次の命令へ */
					instr = CODE_ASSIGN();
					_throwif(ERR_OUTOFMEMORY, !instr);
					instr_ex = (code_ex_t)(instr + 1);
					continue;
				} else if(instr->opcode == OP_ELSE) {
					/* else は if ブロックの中でのみ有効。block/loop/try 内の else を通すと
					 * core compiler が if 専用の else_fixup (block では 0) を使って bytecode
					 * 先頭を破壊し AV する。ここで一元的に弾く。 */
					_throwif(ERR_MALFORMED_BLOCK, cur->head_opcode != OP_IF);
					functiontype_t bt = { 0 };
					uint8_t vt_buf[2];
					_throwiferr(kw_parser_resolve_blocktype(&bt, cur->blocktype, mod, vt_buf));
					_throwiferr(kw_parser_validate_resulttype(&type_stack, cur->type_stack_idx, bt.rt2));
					cur->mode = MODE_BLOCK_ELSE_LOOP;
					/* ELSEブロックへ行くために状態をリセット */
					type_stack.idx = cur->type_stack_idx;
					type_stack.polymorphic = 0;
					/* ブロックのスタックを積む */
					{
						functiontype_t else_bt = { 0 };
						uint8_t else_vt_buf[2];
						_throwiferr(kw_parser_resolve_blocktype(&else_bt, cur->blocktype, mod, else_vt_buf));
						foreach(pt, uint8_t, else_bt.rt1) {
							type_stack_push(&type_stack, *pt);
						}
					}
					/* else 部分の先頭命令スロットを確保 */
					instr = CODE_ASSIGN();
					_throwif(ERR_OUTOFMEMORY, !instr);
					instr_ex = (code_ex_t)(instr + 1);
					continue;
				} else { /* OP_END */
					functiontype_t end_bt = { 0 };
					uint8_t end_vt_buf[2];
					_throwiferr(kw_parser_resolve_blocktype(&end_bt, cur->blocktype, mod, end_vt_buf));
					_throwiferr(kw_parser_validate_resulttype(&type_stack, cur->type_stack_idx, end_bt.rt2));
					if(cur->head_opcode == OP_IF) {
					  _throwif(ERR_TYPE_MISMATCH, end_bt.rt1.len != end_bt.rt2.len);
						for(uint32_t i = 0; i < end_bt.rt1.len; i++)
							_throwif(ERR_TYPE_MISMATCH, array_at(end_bt.rt1, i) != array_at(end_bt.rt2, i));
					}
					BLOCK_END();
					continue;
				}
			} else if(cur->mode == MODE_BLOCK_ELSE_LOOP) {
				/* else アーム内の 2 個目の else は不正 (if の else は 1 個のみ)。放置すると
				 * 通常命令として無視され、core が解決済み else_fixup を再パッチして制御フローを
				 * 壊す。ネストした内側 if の else は別フレームで処理されるためここには来ない。 */
				_throwif(ERR_MALFORMED_BLOCK, instr->opcode == OP_ELSE);
				if(instr->opcode != OP_END) {
					/* 同じブロック内で次の命令へ */
					instr = CODE_ASSIGN();
					_throwif(ERR_OUTOFMEMORY, !instr);
					instr_ex = (code_ex_t)(instr + 1);
					continue;
				} else { /* OP_END */
					functiontype_t else_end_bt = { 0 };
					uint8_t else_end_vt_buf[2];
					_throwiferr(kw_parser_resolve_blocktype(&else_end_bt, cur->blocktype, mod, else_end_vt_buf));
					_throwiferr(kw_parser_validate_resulttype(&type_stack, cur->type_stack_idx, else_end_bt.rt2));
					BLOCK_END();
					continue;
				}
			}

			/* トップレベル（関数ボディ / グローバル初期化式など） */
			if(cur->mode == MODE_NONE) {
				_throwif(ERR_MALFORMED_BLOCK, instr->opcode == OP_ELSE);
				if(instr->opcode == OP_END)
					break;

				instr = CODE_ASSIGN();
				_throwif(ERR_OUTOFMEMORY, !instr);
				instr_ex = (code_ex_t)(instr + 1);
			}
		}
		_throwif(ERR_NOTOPEND, instr->opcode != OP_END);
		if(is_constexpr) {
			if(expected_result_type == TYPE_VAL_V128) {
				/* V128 は 2 entry に展開されているため idx==1 を期待。 */
				_throwif(ERR_TYPE_MISMATCH, type_stack.idx != 1);
				_throwif(ERR_TYPE_MISMATCH, type_stack.pool.data[0] != TYPE_VAL_V128);
				_throwif(ERR_TYPE_MISMATCH, type_stack.pool.data[1] != TYPE_VAL_V128);
			} else {
				_throwif(ERR_TYPE_MISMATCH, type_stack.idx != 0);
				_throwif(ERR_TYPE_MISMATCH, type_stack.pool.data[0] != expected_result_type);
			}
		}
		if(func != NULL) {
			_throwiferr(kw_parser_validate_resulttype(&type_stack, -1, ftype->rt2));
			/* #19: オペランドスタックが pool 容量 (65536) を超えた関数を弾く。
			 * 超過すると type_stack_push が drop され slot 別名化するため。 */
			_throwif(ERR_TOO_MANY_LOCALS, type_stack.overflowed);
			/* cap 検証専用なので function_t メンバではなくローカル変数で持つ。 */
			uint32_t max_register_count = total_locals + type_stack.max_idx + 1;
			/* セキュリティ: instr_t の r0/r1/r2 と local slot は uint16_t。スロット番号は
			 * (uint16_t)(total_locals + idx) で計算されるため、必要スロット総数
			 * (max_register_count) が 16bit 空間 (最大インデックス 65535 = count 0x10000) を
			 * 超えると番号が wrap し別スロットと別名化して型混同を起こす。ここで一元的に弾く。 */
			_throwif(ERR_TOO_MANY_LOCALS, max_register_count > 0x10000);
			/* #25: br_table の default ラベルは instr_t.r2 (uint16_t) に格納される。
			 * ブロックネスト深度が 16bit 空間 (65536) を超えると default_label が
			 * truncate され誤ったラベルへ分岐するため、ここで一元的に弾く
			 * (空ブロックは register slot を消費せず上の cap では捕捉できない)。 */
			_throwif(ERR_TOO_MANY_LOCALS, max_label_depth > 0x10000);
			/* total_locals は param + local の内部スロット総数。
			 * runtime の result_base 計算で使う local_slot_count は
			 * その内 local 部分のみ (= total_locals - param_count_internal)。 */
			func->local_slot_count = total_locals - param_count;
		}

		/* スクラッチの後始末。
		 *  const 式: 命令列は自己完結 (内部ポインタを持たない) ため、
		 *    使用分だけ正確サイズの永続ブロックへ memcpy して退避する (instantiate が
		 *    global init / elem・data offset を評価するまで生存させる)。
		 *  関数本体: core が raw wasm バイトから実行するため二度と読まれない (dead)。normal
		 *    build ではスクラッチを解放し body を NULL 化する。KINOWASM_OPCODE_COUNTER
		 *    (decoded dump) のみ func->body を後で walk するためスクラッチを保持する。 */
		if(is_constexpr) {
			size_t used = memory_count ? memory_count : CODE_SIZE;
			void* persist = kinowasm_mem_malloc(used);
			_throwif(ERR_OUTOFMEMORY, persist == NULL);
			memcpy(persist, body_scratch, used);
			*body = (code_t)persist;
			/* ★使い回しなので解放しない。中身は上で退避済み。 */
			body_scratch = NULL;
		}
#if !defined(KINOWASM_OPCODE_COUNTER)
		else {
			kinowasm_mem_free(body_scratch);
			body_scratch = NULL;
			*body = NULL;   /* func->body は core では未使用。dangling を残さない */
		}
#endif
	}
	_catch:
	/* エラー時のみスクラッチを解放 (中途半端なデコードは破棄)。成功時は上で解放済 (const 式 /
	 * normal の関数本体) か、KINOWASM_OPCODE_COUNTER の func->body として意図的に保持している。 */
	/* 使い回しのスクラッチはここで解放しない (次の const 式で使う)。 */
	if(body_scratch != NULL && body_scratch != constexpr_scratch && _is_error(_result))
		kinowasm_mem_free(body_scratch);
	array_term(localvalues);
	array_term(local_offsets);
	type_stack_term(&type_stack);
	return _result;
}

/* kw_parser_code_body — 関数本体をデコードする kw_parser_code_body_ex の薄いラッパ。
 * is_constexpr=0 / expected_result_type=0 固定で呼び出す。
 * 引数:
 *   mod  - 対象モジュール
 *   buf  - 関数本体のバイト列バッファ
 *   body - 構築した先頭命令ポインタの格納先
 *   func - 対象 function_t
 * 戻り値: kw_parser_code_body_ex の結果 (成功時 RES_SUCCESS / 失敗時 ERR_*) */
static kinowasm_result_t kw_parser_code_body(module_t* mod, kinowasm_buf_t* buf, code_t* body, function_t* func)
{
	return kw_parser_code_body_ex(mod, buf, body, func, 0, 0);
}

/* kw_parser_code — Code section をパースし各 module 関数の局所変数と本体をデコードする。
 * 関数数が Function section と一致するか検証し、各エントリのローカル宣言を展開した上で
 * kw_parser_code_body を呼んで本体命令列を構築する。
 * 引数:
 *   mod - 対象モジュール (本体を func->body に格納)
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / 関数数不一致・ローカル過多・section サイズ不整合で ERR_* */
static kinowasm_result_t kw_parser_code(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t func_count;
		_throwiferr(read_u32_leb(&func_count, buf));
		_throwif(ERR_FUNCTION_COUNT_MISSMATCH, mod->functions.len != func_count);
		foreach(func, function_t, mod->functions) {
			uint32_t code_size;
			kinowasm_buf_t code_buf;
			array(local_t) local_vals = { 0 };
			uint32_t local_size;
			_throwiferr(read_u32_leb(&code_size, buf));
			/* セキュリティ: code_size は attacker 制御。残バッファ長を超える値だと
			 * code_buf 経由で元ファイルバッファ外を OOB read してしまうため弾く。
			 * (buf->cur <= buf->len は section 取り込み時に保証済み) */
			_throwif(ERR_MISMATCH_SECTION_SIZE, code_size > buf->len - buf->cur);
			kinowasm_buf_set(&code_buf, buf->data + buf->cur, code_size);
			buf->cur += code_size;
			_throwiferr(read_u32_leb(&local_size, &code_buf));
			/* 残量超の local 宣言群数は確保前に弾く (各エントリは最低 2 byte 消費)。 */
			_throwif(ERR_UNEXPECTED_END, local_size > code_buf.len - code_buf.cur);
			_throwiferr(array_new(local_vals, local_size));
			uint64_t local_count = 0;
			foreach(local, local_t, local_vals) {
				_throwiferr(read_u32_leb(&local->n, &code_buf));
				_throwiferr(read_u8(&local->type, &code_buf));
				_throwif(ERR_MALFORMED_LOCALVAL, !kw_parser_is_valtype(local->type));
#if !KINOWASM_ENABLE_SIMD
				_throwif(ERR_FEATURE_DISABLED, local->type == TYPE_VAL_V128);
#endif
				local_count += local->n;
				/* #21: array_new(localvalues) の前に 16bit レジスタ空間 (0x10000) で
				 * 早期に弾く。これより大きいモジュールは後段の max_register_count cap でも
				 * どのみち拒否されるため、巨大確保 (最大 ~4GB) を未然に防ぐ。 */
				_throwif(ERR_TOO_MANY_LOCALS, local_count > 0x10000);
			}
			_throwiferr(array_new(func->localvalues, local_count));
			uint32_t num = 0;
			foreach(local, local_t, local_vals)
				for(uint32_t i = 0; i < local->n; i++)
					array_at(func->localvalues, num++) = local->type;

			_throwiferr(kw_parser_code_body(mod, &code_buf, &func->body, func));
			/* セキュリティ: 関数本体は終端 OP_END で閉じ、code entry はそこで尽きるはず。
			 * 余剰バイトが残っていれば malformed。これを弾かないと core (生バイトから再
			 * コンパイル) が宣言サイズ全体を走査して関数論理末尾 (cctrl==0) を越えて読み、
			 * end ハンドラが C.ctrl[-1] を OOB 参照する (parser↔core の信頼境界を維持)。 */
			_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(&code_buf));
			array_term(local_vals);
		}
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_table — Table section をパースし mod->tables を構築する。
 * 各 table の tabletype を読み取り、limits の min/max 整合性を検証する。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / max < min など不正で ERR_MALFORMED_TABLE 等のエラーコード */
static kinowasm_result_t kw_parser_table(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t table_size;
		_throwiferr(read_u32_leb(&table_size, buf));
		_throwif(ERR_UNEXPECTED_END, table_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		_throwiferr(array_new(mod->tables, table_size));
		foreach(table, table_t, mod->tables)
			_throwiferr(kw_parser_tabletype(&table->tabletype, buf));

		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_memory — Memory section をパースし mod->memorys を構築する。
 * WASM 3.0 multi-memory により複数 memory 宣言を許可。各 memory の min/max (最大 65536 ページ)
 * と shared なら max 必須の制約を検証する。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / ページ数超過や shared+max 不在で ERR_MALFORMED_MEMORY 等 */
static kinowasm_result_t kw_parser_memory(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t memory_size;
		_throwiferr(read_u32_leb(&memory_size, buf));
		_throwif(ERR_UNEXPECTED_END, memory_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		/* WASM 3.0 multi-memory: 複数 memory 宣言を許可 (上限なし)。 */
		_throwiferr(array_new(mod->memorys, memory_size));
		foreach(mem, memory_t, mod->memorys) {
			_throwiferr(read_limits(&mem->memtype, buf));
			/* メモリサイズの制限チェック（最大65536ページ） */
			_throwif(ERR_MALFORMED_MEMORY, mem->memtype.min > 65536);
			if(mem->memtype.has_max) {
				_throwif(ERR_MALFORMED_MEMORY, mem->memtype.max > 65536);
				_throwif(ERR_MALFORMED_MEMORY, mem->memtype.max < mem->memtype.min);
			}
			/* WASM threads spec: shared memory は max 必須。
			 * shared なのに max 不在は invalid module として reject する。 */
			_throwif(ERR_MALFORMED_MEMORY,
				mem->memtype.shared && !mem->memtype.has_max);
		}
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_global — Global section をパースし mod->globals を構築する。
 * 各 global の globaltype を読み取り、初期化式 (const 式) を kw_parser_code_body_ex で
 * デコードする (func=NULL, is_constexpr=1, 期待型=global の valtype)。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / 初期化式の型不一致や section サイズ不整合で ERR_* */
static kinowasm_result_t kw_parser_global(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t global_size;
		_throwiferr(read_u32_leb(&global_size, buf));
		_throwif(ERR_UNEXPECTED_END, global_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		_throwiferr(array_new(mod->globals, global_size));
		foreach(global, global_t, mod->globals) {
			_throwiferr(kw_parser_globaltype(&global->globaltype, buf));
			/* グローバル初期化式は関数ではないので func_local_count = 0 */
			_throwiferr(kw_parser_code_body_ex(mod, buf, &global->body, NULL, 1, global->globaltype.valtype));
		}
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_export — Export section をパースし mod->exports を構築する。
 * 各 export の名前 (重複不可) と種別 (func/table/memory/global/tag)・対象 idx の範囲を検証し、
 * func export は宣言済み関数としてマークする。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / 名前重複や範囲外 idx で RES_ERROR / ERR_MALFORMED_* */
static kinowasm_result_t kw_parser_export(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t export_size;
		_throwiferr(read_u32_leb(&export_size, buf));
		_throwif(ERR_UNEXPECTED_END, export_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		_throwiferr(array_new(mod->exports, export_size));
		foreach(export, export_t, mod->exports) {
			_throwiferr(read_string(&export->name, buf));
			for(export_t* prev = mod->exports.data; prev != export; prev++) {
				_throwif(RES_ERROR, prev->name.len == export->name.len && memcmp(prev->name.data, export->name.data, export->name.len) == 0);
			}
			_throwiferr(read_u8(&export->exportdesc.kind, buf));
			_throwiferr(read_u32_leb(&export->exportdesc.idx, buf));
			
			/* エクスポート対象のインデックス範囲検証 */
			switch(export->exportdesc.kind) {
			case IMPORTDESC_FUNC:
				_throwif(ERR_MALFORMED_FUNC, export->exportdesc.idx >= (uint32_t)(mod->funcimport_count + mod->functions.len));
				_throwiferr(kw_parser_mark_declared_func(mod, export->exportdesc.idx));
				break;
			case IMPORTDESC_TABLE:
				_throwif(ERR_MALFORMED_TABLE, export->exportdesc.idx >= (uint32_t)(mod->tableimport_count + mod->tables.len));
				break;
			case IMPORTDESC_MEMORY:
				_throwif(ERR_MALFORMED_MEMORY, export->exportdesc.idx >= (uint32_t)(mod->memoryimport_count + mod->memorys.len));
				break;
			case IMPORTDESC_GLOBAL:
				_throwif(ERR_MALFORMED_GLOBAL, export->exportdesc.idx >= (uint32_t)(mod->globalimport_count + mod->globals.len));
				break;
			case IMPORTDESC_TAG:
				/* WASM 3.0 EH: tag export idx 範囲検証 (import + local 全 tag)。 */
				_throwif(ERR_MALFORMED_FUNC, export->exportdesc.idx >= (uint32_t)(mod->tagimport_count + mod->tags.len));
				break;
			default:
				_throw(ERR_UNKNOWN_IMPORT_KIND);
			}
		}
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_element_read_funcref_inits — Element section の funcidx 列を ref.func 初期化式に展開する。
 * 各 funcidx を読み、範囲検証して宣言済みマークした上で「OP_REF_FUNC; OP_END」の小命令列を
 * 1 件ずつ malloc して element->init に格納する (funcref 短縮形 init の共通処理)。
 * 引数:
 *   mod     - 対象モジュール
 *   buf     - 読み取り元バッファ
 *   element - init 列を格納する element
 * 戻り値: 成功時 RES_SUCCESS / 範囲外 funcidx や OOM で ERR_* エラーコード */
static kinowasm_result_t kw_parser_element_read_funcref_inits(module_t* mod, kinowasm_buf_t* buf, element_t* element)
{
	_try{
		uint32_t init_size;
		_throwiferr(read_u32_leb(&init_size, buf));
		_throwif(ERR_UNEXPECTED_END, init_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		_throwiferr(array_new(element->init, init_size));

		foreach(body, code_t, element->init) {
			uint32_t x;
			_throwiferr(read_u32_leb(&x, buf));
			/* funcref 短縮形の init (OP_REF_FUNC; OP_END) は右サイズの単発 malloc。内部ポインタを
			 * 持たず instantiate (elem 評価) で読まれるまで生存し、module アリーナの wholesale 解放で
			 * 回収される。 */
			code_t init = kinowasm_mem_malloc(CODE_WITH_EX_SIZE + CODE_SIZE);
			if(init == NULL)
				_throw(ERR_OUTOFMEMORY);

			code_ex_t init_ex = (code_ex_t)(init + 1);
			code_t init_end = (code_t)(init_ex + 1);

			*init = (instr_t){
				.opcode = OP_REF_FUNC,
				.r0 = 0,
			};
			init_ex->x = x;
			_throwif(ERR_MALFORMED_FUNC, init_ex->x >= (uint32_t)(mod->funcimport_count + mod->functions.len));
			_throwiferr(kw_parser_mark_declared_func(mod, init_ex->x));
			*init_end = (instr_t){
				.opcode = OP_END,
			};

			*body = init;
		}
	}
	_catch:
	return _result;
}

/* kw_parser_element — Element section をパースし mod->elements を構築する。
 * 先頭の kind (0〜7) ごとに active/passive/declarative の別、対象 table、offset 式 (table64 では i64)、
 * reftype、init 列 (funcidx 短縮形 or const 式) を読み分けて検証する。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / 未対応 kind や型不一致・範囲外 table で RES_ERROR / ERR_* */
static kinowasm_result_t kw_parser_element(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t element_size;
		_throwiferr(read_u32_leb(&element_size, buf));
		_throwif(ERR_UNEXPECTED_END, element_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		_throwiferr(array_new(mod->elements, element_size));
		foreach(element, element_t, mod->elements) {
			uint32_t kind;
			_throwiferr(read_u32_leb(&kind, buf));
			switch(kind) {
			case 0: {
				/* Function reference */
				tabletype_t* table_type;
				element->type = TYPE_FUNCREF;
				element->mode.kind = 0;
				element->mode.table = 0;
				_throwif(ERR_MALFORMED_TABLE, (uint32_t)(mod->tableimport_count + mod->tables.len) == 0);
				table_type = kw_parser_get_tabletype(mod, element->mode.table);
				_throwif(ERR_MALFORMED_TABLE, table_type == NULL);
				_throwif(ERR_TYPE_MISMATCH, table_type->reftype != element->type);
				/* table64: active elem の offset 式は i64 */
				uint8_t off_type0 = table_type->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
				_throwiferr(kw_parser_code_body_ex(mod, buf, &element->mode.offset, NULL, 1, off_type0));
				_throwiferr(kw_parser_element_read_funcref_inits(mod, buf, element));
				break;
			}
			case 2: {
				_throwiferr(read_u32_leb(&element->mode.table, buf));
				_throwif(ERR_MALFORMED_TABLE, element->mode.table >= (uint32_t)(mod->tableimport_count + mod->tables.len));
				tabletype_t* table_type2 = kw_parser_get_tabletype(mod, element->mode.table);
				_throwif(ERR_MALFORMED_TABLE, table_type2 == NULL);
				uint8_t off_type2 = table_type2->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
				_throwiferr(kw_parser_code_body_ex(mod, buf, &element->mode.offset, NULL, 1, off_type2));
				/* fallthrough into case 1/3 for the rest */
			}
			case 1:
			case 3: {
			  tabletype_t* table_type = NULL;
				if(kind == 1)
					element->mode.kind = KIND_PASSIVE;
				else if(kind == 2)
					element->mode.kind = KIND_ACTIVE;
				else
					element->mode.kind = KIND_DECLARATIVE;

				uint8_t et;
				_throwiferr(read_u8(&et, buf));
				_throwif(RES_ERROR, et != 0);
				element->type = TYPE_FUNCREF;
				if(kind == 2) {
					table_type = kw_parser_get_tabletype(mod, element->mode.table);
					_throwif(ERR_MALFORMED_TABLE, table_type == NULL);
					_throwif(ERR_TYPE_MISMATCH, table_type->reftype != element->type);
				}

				_throwiferr(kw_parser_element_read_funcref_inits(mod, buf, element));
				break;
			}
			case 4: {
				tabletype_t* table_type;
				element->mode.kind = KIND_ACTIVE;
				element->type = TYPE_FUNCREF;
				element->mode.table = 0;
				_throwif(ERR_MALFORMED_TABLE, (uint32_t)(mod->tableimport_count + mod->tables.len) == 0);
				table_type = kw_parser_get_tabletype(mod, element->mode.table);
				_throwif(ERR_MALFORMED_TABLE, table_type == NULL);
				_throwif(ERR_TYPE_MISMATCH, table_type->reftype != element->type);
				uint8_t off_type4 = table_type->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
				_throwiferr(kw_parser_code_body_ex(mod, buf, &element->mode.offset, NULL, 1, off_type4));
				uint32_t init_size;
				_throwiferr(read_u32_leb(&init_size, buf));
				_throwif(ERR_UNEXPECTED_END, init_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
				_throwiferr(array_new(element->init, init_size));
				foreach(body, code_t, element->init)
					_throwiferr(kw_parser_code_body_ex(mod, buf, body, NULL, 1, element->type));
				break;
			}
			case 5: {
				element->mode.kind = KIND_PASSIVE;
				_throwiferr(read_u8(&element->type, buf));
				_throwif(ERR_MALFORMED_REFERENCE_TYPE, element->type != TYPE_FUNCREF && element->type != TYPE_EXTERNREF);
				uint32_t init_size;
				_throwiferr(read_u32_leb(&init_size, buf));
				_throwif(ERR_UNEXPECTED_END, init_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
				_throwiferr(array_new(element->init, init_size));
				foreach(body, code_t, element->init)
					_throwiferr(kw_parser_code_body_ex(mod, buf, body, NULL, 1, element->type));
				break;
			}
			case 6: {
				tabletype_t* table_type;
				element->mode.kind = KIND_ACTIVE;
				_throwiferr(read_u32_leb(&element->mode.table, buf));
				_throwif(ERR_MALFORMED_TABLE, element->mode.table >= (uint32_t)(mod->tableimport_count + mod->tables.len));
				table_type = kw_parser_get_tabletype(mod, element->mode.table);
				_throwif(ERR_MALFORMED_TABLE, table_type == NULL);
				uint8_t off_type6 = table_type->limits.is_64 ? TYPE_VAL_I64 : TYPE_VAL_I32;
				_throwiferr(kw_parser_code_body_ex(mod, buf, &element->mode.offset, NULL, 1, off_type6));
				_throwiferr(read_u8(&element->type, buf));
				_throwif(ERR_MALFORMED_REFERENCE_TYPE, element->type != TYPE_FUNCREF && element->type != TYPE_EXTERNREF);
				table_type = kw_parser_get_tabletype(mod, element->mode.table);
				_throwif(ERR_MALFORMED_TABLE, table_type == NULL);
				_throwif(ERR_TYPE_MISMATCH, table_type->reftype != element->type);
				uint32_t init_size;
				_throwiferr(read_u32_leb(&init_size, buf));
				_throwif(ERR_UNEXPECTED_END, init_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
				_throwiferr(array_new(element->init, init_size));
				foreach(body, code_t, element->init)
					_throwiferr(kw_parser_code_body_ex(mod, buf, body, NULL, 1, element->type));
				break;
			}
			case 7: {
				element->mode.kind = KIND_DECLARATIVE;
				_throwiferr(read_u8(&element->type, buf));
				_throwif(ERR_MALFORMED_REFERENCE_TYPE, element->type != TYPE_FUNCREF && element->type != TYPE_EXTERNREF);
				uint32_t init_size;
				_throwiferr(read_u32_leb(&init_size, buf));
				_throwif(ERR_UNEXPECTED_END, init_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
				_throwiferr(array_new(element->init, init_size));
				foreach(body, code_t, element->init)
					_throwiferr(kw_parser_code_body_ex(mod, buf, body, NULL, 1, element->type));
				break;
			}
			default:
				printf("unsupported element: %x", kind);
				_throw(RES_ERROR);
			}
		}

		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_start — Start section をパースし start 関数インデックスを設定する。
 * start 関数は引数・戻り値ともに空でなければならない。
 * 引数:
 *   mod - 結果を格納するモジュール (has_start / startidx を設定)
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / 範囲外 idx や非空シグネチャで ERR_MALFORMED_FUNC / ERR_TYPE_MISMATCH */
static kinowasm_result_t kw_parser_start(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		mod->has_start = 1;
		_throwiferr(read_u32_leb(&mod->startidx, buf));
		_throwif(ERR_MALFORMED_FUNC, mod->startidx >= mod->funcimport_count + mod->functions.len);
		functiontype_t* start_type = kw_parser_get_functype(mod, mod->startidx);
		_throwif(ERR_TYPE_MISMATCH, start_type == NULL || start_type->rt1.len != 0 || start_type->rt2.len != 0);
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_data — Data section をパースし mod->datas を構築する。
 * Data count section で先行確保済みなら個数一致を検証。各 data の kind (active/passive)、
 * 対象 memory と offset 式 (memory64 では i64)、バイト列を読み取る。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / メモリ不在や個数/section サイズ不整合で ERR_* エラーコード */
static kinowasm_result_t kw_parser_data(module_t* mod, kinowasm_buf_t* buf, int has_datacount)
{
	_try{
		uint32_t data_size;
		_throwiferr(read_u32_leb(&data_size, buf));
		if(has_datacount)
			_throwif(ERR_MISMATCH_SECTION_SIZE, data_size != mod->datas.len);
		else {
			_throwif(ERR_UNEXPECTED_END, data_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
			_throwiferr(array_new(mod->datas, data_size));
		}

		uint32_t kind;
		foreach(data, data_t, mod->datas) {
			_throwiferr(read_u32_leb(&kind, buf));
			switch(kind) {
			case 0:
				data->mode.kind = DATA_MODE_ACTIVE;
				data->mode.memory = 0;
				_throwif(ERR_MALFORMED_MEMORY, (mod->memoryimport_count + mod->memorys.len) == 0);
				{
					/* memory64: data offset init expr は i64。 */
					const memorytype_t* dmt = get_memtype_by_idx(mod, 0);
					uint8_t off_type = (dmt && dmt->is_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
					_throwiferr(kw_parser_code_body_ex(mod, buf, &data->mode.offset, NULL, 1, off_type));
				}
				break;
			case 1:
				data->mode.kind = DATA_MODE_PASSIVE;
				break;
			case 2:
				data->mode.kind = DATA_MODE_ACTIVE;
				_throwiferr(read_u32_leb(&data->mode.memory, buf));
				_throwif(ERR_MALFORMED_MEMORY, data->mode.memory >= (uint32_t)(mod->memoryimport_count + mod->memorys.len));
				{
					const memorytype_t* dmt = get_memtype_by_idx(mod, data->mode.memory);
					uint8_t off_type = (dmt && dmt->is_64) ? TYPE_VAL_I64 : TYPE_VAL_I32;
					_throwiferr(kw_parser_code_body_ex(mod, buf, &data->mode.offset, NULL, 1, off_type));
				}
				break;
			default:
				/* WASM 仕様で data segment の flag は 0/1/2 のみ。未知 flag を弾かないと
				 * data->mode (kind/memory/offset) が未初期化のまま instantiate に渡り、
				 * 未初期化 index/ポインタ参照になる (element section の default と対称)。 */
				_throw(RES_ERROR);
			}
			uint32_t init_size;
			_throwiferr(read_u32_leb(&init_size, buf));
			_throwiferr(array_new(data->init, init_size));
			/* ★まとめて写す。1 バイトずつ read_u8 を回すと境界検査が毎回入る。 */
			if(init_size != 0)
				_throwiferr(kinowasm_buf_read_bytes(&array_at(data->init, 0), init_size, buf));
		}
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_datacount — Data count section をパースし data 個数ぶんの mod->datas を先行確保する。
 * Code section より前に出現し、memory.init / data.drop の data idx 検証に使われる。
 * 引数:
 *   mod            - 結果を格納するモジュール
 *   buf            - section 内容のバッファ
 *   file_remaining - datacount section より後のファイル残量 (byte)
 * 戻り値: 成功時 RES_SUCCESS / 確保失敗や section サイズ不整合で ERR_* エラーコード */
static kinowasm_result_t kw_parser_datacount(module_t* mod, kinowasm_buf_t* buf, uint64_t file_remaining)
{
	_try{
		uint32_t data_size;
		_throwiferr(read_u32_leb(&data_size, buf));
		/* 宣言数は後続 data section の要素数 (各 data は最低 1 byte 消費)。ファイル残量超の
		 * 宣言は data section で必ず不整合になるため、確保前に弾く (巨大確保 DoS 防止)。 */
		_throwif(ERR_UNEXPECTED_END, data_size > file_remaining);
		_throwiferr(array_new(mod->datas, data_size));
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_tag — Tag section (id=13) をパースし mod->tags を構築する (WASM 3.0 exception handling)。
 * 各 tag は { attribute (u8), typeidx (u32 LEB) }。attribute は将来用で現状 0 (exception) のみ許可。
 * typeidx は登録済関数型を参照し、その results (rt2) は空でなければならない。
 * 引数:
 *   mod - 結果を格納するモジュール
 *   buf - section 内容のバッファ
 * 戻り値: 成功時 RES_SUCCESS / 非0 attribute や非空 results で ERR_MALFORMED_FUNC / ERR_TYPE_MISMATCH */
static kinowasm_result_t kw_parser_tag(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t tag_size;
		_throwiferr(read_u32_leb(&tag_size, buf));
		_throwif(ERR_UNEXPECTED_END, tag_size > buf->len - buf->cur);	/* 残量超の宣言数は確保前に弾く */
		_throwiferr(array_new(mod->tags, tag_size));
		for(uint32_t i = 0; i < tag_size; i++) {
			tag_t* tag = &array_at(mod->tags, i);
			_throwiferr(read_u8(&tag->tagtype.attribute, buf));
			_throwif(ERR_MALFORMED_FUNC, tag->tagtype.attribute != 0);
			_throwiferr(read_u32_leb(&tag->tagtype.typeidx, buf));
			_throwif(ERR_MALFORMED_FUNC, tag->tagtype.typeidx >= mod->types.len);
			/* 仕様で tag の関数型 results は空でなければならない */
			functiontype_t* ft = &array_at(mod->types, tag->tagtype.typeidx);
			_throwif(ERR_TYPE_MISMATCH, ft->rt2.len != 0);
		}
		_throwif(ERR_MISMATCH_SECTION_SIZE, !kinowasm_buf_is_eof(buf));
	}
	_catch:
	return _result;
}

/* kw_parser_section_order — section id を出現順序の連番に写像する。
 * 出現順検証 (前 section より大きい連番か) に使う。Tag section (id=13) は Memory の後
 * Global の前に位置づけられる (WASM 3.0 exception handling)。
 * 引数:
 *   section_id - WASM section id
 * 戻り値: 正規化した順序番号 (1 起点)。未知 / 無効な id は 0 */
static uint8_t kw_parser_section_order(uint8_t section_id)
{
	switch(section_id) {
	case 1:  return 1;  /* Type */
	case 2:  return 2;  /* Import */
	case 3:  return 3;  /* Function */
	case 4:  return 4;  /* Table */
	case 5:  return 5;  /* Memory */
	case 13: return 6;  /* Tag (WASM 3.0 exception handling) */
	case 6:  return 7;  /* Global */
	case 7:  return 8;  /* Export */
	case 8:  return 9;  /* Start */
	case 9:  return 10; /* Element */
	case 12: return 11; /* Data count */
	case 10: return 12; /* Code */
	case 11: return 13; /* Data */
	default: return 0;
	}
}

/* kinowasm_parse_module — WASM バイナリ全体をデコードしモジュールを構築するエントリポイント。
 * magic (\0asm) とバージョン (1) を検証した後、各 section を出現順検証しながら種別ごとの
 * kw_parser_* に振り分けて処理する (命令列スクラッチは本体ごとに kw_parser_code_body_ex が確保)。
 * 引数:
 *   mod - デコード結果を格納するモジュール
 *   buf - WASM バイナリ全体のバッファ
 * 戻り値: 成功時 RES_SUCCESS / ヘッダ不正・順序違反・関数数不一致など各 section の ERR_* */
kinowasm_result_t kinowasm_parse_module(module_t* mod, kinowasm_buf_t* buf)
{
	_try{
		uint32_t header;
		uint32_t version;
		uint8_t section_id;
		uint8_t last_section_order = 0;
		uint32_t section_size;
		int has_code_section = 0;   /* code section (id=10) を処理したか。関数本体の欠落検出用 */
		int has_data_section = 0;   /* data section (id=11) を処理したか。datacount との整合検証用 */
		int has_datacount = 0;      /* datacount section (id=12) を処理したか。datacount との整合検証用 */
		_throwiferr(read_bytes(&header, sizeof(header), buf));
		_throwif(ERR_HEADERNOTDETECT, header != 0x6d736100);
		_throwiferr(read_bytes(&version, sizeof(version), buf));
		_throwif(ERR_VERSIONMISMATCH, version != 0x00000001);
		/* 命令列スクラッチは本体 (関数本体 / const 式) ごとに kw_parser_code_body_ex が確保する。 */
		while(!kinowasm_buf_is_eof(buf)) {
			_throwiferr(read_u8(&section_id, buf));
			_throwiferr(read_u32_leb(&section_size, buf));
			_throwif(ERR_MISMATCH_SECTION_SIZE, buf->cur + section_size > buf->len);
			if(section_id != 0) {
				uint8_t section_order;
				section_order = kw_parser_section_order(section_id);
				_throwif(RES_ERROR, section_order == 0);
				_throwif(RES_ERROR, section_order <= last_section_order);
				last_section_order = section_order;
			}
			kinowasm_buf_t decode_buf;
			kinowasm_buf_set(&decode_buf, (uint8_t*)buf->data + buf->cur, section_size);
			buf->cur += section_size;
			switch(section_id) {
			case 0: {
				/* Custom Section: name は valid UTF-8 でなければならない (WASM 仕様)。
				 * 内容自体は無視するが、name の妥当性は検証する。 */
				string_t name = { 0 };
				_throwiferr(read_string(&name, &decode_buf));
				array_term(name);
				/* 中身は使わないので読み飛ばす (1 バイトずつ回さない)。 */
				_throwiferr(kinowasm_buf_skip(
					(size_t)(decode_buf.len - decode_buf.cur), &decode_buf));
				break;
			}
			case 1:
				/* Type section */
				_throwiferr(kw_parser_type(mod, &decode_buf));
				break;
			case 2:
				/* Import section */
				_throwiferr(kw_parser_import(mod, &decode_buf));
				break;
			case 3:
				/* Function section */
				_throwiferr(kw_parser_function(mod, &decode_buf));
				break;
			case 4:
				/* Table section */
				_throwiferr(kw_parser_table(mod, &decode_buf));
				break;
			case 5:
				/* Memory section */
				_throwiferr(kw_parser_memory(mod, &decode_buf));
				break;
			case 6:
				/* Global section */
				_throwiferr(kw_parser_global(mod, &decode_buf));
				break;
			case 7:
				/* Export section */
				_throwiferr(kw_parser_export(mod, &decode_buf));
				break;
			case 8:
				/* Start section */
				_throwiferr(kw_parser_start(mod, &decode_buf));
				break;
			case 9:
				/* Element section */
				_throwiferr(kw_parser_element(mod, &decode_buf));
				break;
			case 10:
				/* Code section */
				_throwiferr(kw_parser_code(mod, &decode_buf));
				has_code_section = 1;
				break;
			case 11:
				/* Data section */
				_throwiferr(kw_parser_data(mod, &decode_buf, has_datacount));
				has_data_section = 1;
				break;
			case 12:
				/* Data count section */
				_throwiferr(kw_parser_datacount(mod, &decode_buf, buf->len - buf->cur));
				has_datacount = 1;
				break;
			case 13:
				/* WASM 3.0 exception handling: Tag section */
				_throwiferr(kw_parser_tag(mod, &decode_buf));
				break;
			default:
				_throw(RES_ERROR);
			}
		}
		/* function section があるのに code section が無い (本体欠落) モジュールを弾く。
		 * code section ありなら kw_parser_code 内で func_count==functions.len を検証済み。 */
		_throwif(ERR_FUNCTION_COUNT_MISSMATCH, mod->functions.len != 0 && !has_code_section);
		/* datacount section が data segment 数を宣言したのに data section が無い (N>0) モジュールを弾く。
		 * datacount は mod->datas を N 個先行確保するが、data section が来ないと各 data_t が
		 * 未初期化のまま instantiate (kw_alloc_data) に渡り garbage ポインタからの memcpy になる。
		 * (data section があれば kw_parser_data 内で datacount との個数一致を検証済み。) */
		_throwif(ERR_DATASECTIONREQUIRED, has_datacount && !has_data_section && mod->datas.len != 0);
	}
	_catch:
	kw_parser_release_scratch();
	return _result;
}
