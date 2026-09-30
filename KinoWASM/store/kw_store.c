#include "kw_store.h"
/* kw_store.c — store の確保 (runtime_alloc) + instantiate (decode 済 module → store) を担う。
 * 実行 (invoke/start) は core エンジン (KinoWASM/core) が行い、instantiate の const 式は
 * kw_eval_const_expr、active segment は直接適用。 */
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#if defined(LINEAR_MEMORY_DEBUG)
#include <inttypes.h>
#endif

/* hot frame / localpool / value-stack inline helpers (kw_store_frame.h) */
#include "kw_store_frame.h"

/* instance 確保 helper (純 alloc, kw_store_alloc.c) */
#include "kw_store_alloc.h"

/* ───────────────────────── const 式評価器 ─────────────────────────
 * instantiate (global 初期化 / elem 初期化 / active segment offset) の const 式を
 * 実行エンジンに依存せず直接評価する。parser が is_constexpr=1 で
 * 生成する decoded const-expr は許可 op (i32/i64/f32/f64.const, global.get, ref.null,
 * ref.func, v128.const, extended-const の i32/i64 add/sub/mul と その const 融合, copy, end)
 * のみ。const 式は trap しない (検証済み)。結果は out[0] (v128 は out[0..1]) へ。
 * 内部スロットは 0 起点で浅い (実測 ≤2、余裕を見て 64)。 */
#define KW_CONST_SLOTS 64
static kinowasm_result_t kw_eval_const_expr(store_t* S, code_t code, moduleinst_t* mod, kinowasm_val_t* out, int is_v128)
{
	kinowasm_val_t L[KW_CONST_SLOTS];
	instr_t* ip = code;
	#define KW_CE_ADV(n) (ip = (instr_t*)((uint8_t*)ip + (n)))
	#define KW_CE_CK1(s) do { \
		if((s) >= KW_CONST_SLOTS) \
			return ERR_MALFORMED_FUNC; \
	} while(0)
	for(;;) {
		uint16_t op = ip->opcode;
		uint16_t r0 = ip->r0;
		uint16_t r1 = ip->r1;
		uint16_t r2 = ip->r2;
		code_ex_t ex = (code_ex_t)(ip + 1);
		KW_CE_CK1(r0);
		switch(op) {
		case OP_I32_CONST:
			L[r0].num.i32 = ex->c.i32;
			KW_CE_ADV(CODE_WITH_EX_SIZE);
			break;
		case OP_I64_CONST:
			L[r0].num.i64 = ex->c.i64;
			KW_CE_ADV(CODE_WITH_EX_SIZE);
			break;
		case OP_F32_CONST:
			L[r0].num.f32 = ex->c.f32;
			KW_CE_ADV(CODE_WITH_EX_SIZE);
			break;
		case OP_F64_CONST:
			L[r0].num.f64 = ex->c.f64;
			KW_CE_ADV(CODE_WITH_EX_SIZE);
			break;
		case OP_GLOBAL_GET: {
			globaladdr_t a = mod->globaladdrs[ex->val];
			L[r0] = kinowasm_array_at(S->globals, a).val;
			KW_CE_ADV(CODE_WITH_EX_SIZE);
			break;
		}
		case OP_REF_NULL:
			L[r0].ref = REF_NULL;
			KW_CE_ADV(CODE_SIZE);
			break;
		case OP_REF_FUNC:
			L[r0].ref = mod->funcaddrs[ex->x];
			KW_CE_ADV(CODE_WITH_EX_SIZE);
			break;
		case OP_COPY:
			KW_CE_CK1(r1);
			L[r0] = L[r1];
			KW_CE_ADV(CODE_SIZE);
			break;
		case OP_I32_ADD:
			KW_CE_CK1(r1);
			KW_CE_CK1(r2);
			L[r0].num.i32 = L[r1].num.i32 + L[r2].num.i32;
			KW_CE_ADV(CODE_SIZE);
			break;
		case OP_I32_SUB:
			KW_CE_CK1(r1);
			KW_CE_CK1(r2);
			L[r0].num.i32 = L[r1].num.i32 - L[r2].num.i32;
			KW_CE_ADV(CODE_SIZE);
			break;
		case OP_I32_MUL:
			KW_CE_CK1(r1);
			KW_CE_CK1(r2);
			L[r0].num.i32 = L[r1].num.i32 * L[r2].num.i32;
			KW_CE_ADV(CODE_SIZE);
			break;
		case OP_I64_ADD:
			KW_CE_CK1(r1);
			KW_CE_CK1(r2);
			L[r0].num.i64 = L[r1].num.i64 + L[r2].num.i64;
			KW_CE_ADV(CODE_SIZE);
			break;
		case OP_I64_SUB:
			KW_CE_CK1(r1);
			KW_CE_CK1(r2);
			L[r0].num.i64 = L[r1].num.i64 - L[r2].num.i64;
			KW_CE_ADV(CODE_SIZE);
			break;
		case OP_I64_MUL:
			KW_CE_CK1(r1);
			KW_CE_CK1(r2);
			L[r0].num.i64 = L[r1].num.i64 * L[r2].num.i64;
			KW_CE_ADV(CODE_SIZE);
			break;
#if KINOWASM_ENABLE_SIMD
		case OP_0XFD_V128_CONST:
			KW_CE_CK1((uint16_t)(r0 + 1));
			*(kinowasm_v128_t*)&L[r0] = *(kinowasm_v128_t*)ex->v128_ptr;
			KW_CE_ADV(CODE_WITH_EX_SIZE);
			break;
		case OP_GLOBAL_GET_V128: {
			KW_CE_CK1((uint16_t)(r0 + 1));
			globaladdr_t a = mod->globaladdrs[ex->val];
			*(kinowasm_v128_t*)&L[r0] = kinowasm_array_at(S->globals, a).val_v128;
			KW_CE_ADV(CODE_WITH_EX_SIZE);
			break;
		}
		case OP_COPY_V128:
			KW_CE_CK1((uint16_t)(r0 + 1));
			KW_CE_CK1((uint16_t)(r1 + 1));
			*(kinowasm_v128_t*)&L[r0] = *(kinowasm_v128_t*)&L[r1];
			KW_CE_ADV(CODE_SIZE);
			break;
#endif
		case OP_END:
			if(is_v128)
				*(kinowasm_v128_t*)out = *(kinowasm_v128_t*)&L[0];
			else
				out[0] = L[0];

			(void)S;
			return RES_SUCCESS;
		default:
			return ERR_MALFORMED_FUNC;   /* const 式に現れないはずの op */
		}
	}
	#undef KW_CE_ADV
	#undef KW_CE_CK1
}

static inline globaladdr_t kw_alloc_global(store_t* S, global_t* global, moduleinst_t* moduleinst, globaladdr_t* addr)
{
	_try{
		globalinstance_t globalinst;
		memset(&globalinst, 0, sizeof(globalinst));
		globalinst.gt = global->globaltype;
		uint8_t is_v128 = (global->globaltype.valtype == TYPE_VAL_V128);
		uint32_t pool_size = is_v128 ? 2 : 1;
		/* グローバル初期化式を実行するためのダミーフレームを作成 */
		frame_t* F = kw_assign_frame(S);
		_throwif(ERR_OUTOFMEMORY, F == NULL);	/* #13 */
		F->localpool = kw_find_localpool(S, pool_size);
		/* #13: 確保途中の frame を pool へ戻す */
		if(F->localpool == NULL) {
			kw_return_frame(S, F);
			_throw(ERR_OUTOFMEMORY);
		}

		memset(F->localpool->locals, 0, sizeof(kinowasm_val_t) * pool_size);
		F->module = moduleinst;
		F->funcaddrs = moduleinst->funcaddrs;
		F->parent = NULL;
		F->caller_locals = NULL;
		F->root_funcaddr = -1;
		F->arity = is_v128 ? 2 : 1;
		{
			/* push 失敗 (OOM) 時、未 push の frame/localpool を pool へ戻す (#13: リーク防止)。 */
			kinowasm_result_t push_res = kw_push_frame(S->stack, F);
			if(push_res != RES_SUCCESS) {
				kw_return_localpool(S, F->localpool);
				kw_return_frame(S, F);
				_throw(push_res);
			}
		}

		/* 初期化式を const 式評価器で評価する。結果は localpool->locals[0] へ。 */
		_throwiferr(kw_eval_const_expr(S, global->body, moduleinst, F->localpool->locals, is_v128));

		/* 実行結果をスタックから取得 (v128 は 16 byte コピー) */
		if(is_v128)
			globalinst.val_v128 = *(kinowasm_v128_t*)&F->localpool->locals[0];
		else
			globalinst.val = F->localpool->locals[0];
		kw_pop_frame(S->stack, &F);
		kw_return_localpool(S, F->localpool);
		kw_return_frame(S, F);
		kinowasm_array_append(S->globals, globalinst, *addr);
	}
	_catch:
	return _result;
}

static inline elemaddr_t kw_alloc_elem(store_t* S, element_t* elem, moduleinst_t* moduleinst, elemaddr_t* addr)
{
	elementinstance_t eleminst = { 0 };
	_try{
		_throwiferr(kinowasm_array_new_from(eleminst.elem, elem->init.len));
		/* 要素初期化式を実行するためのダミーフレームを作成 */
		frame_t* F = kw_assign_frame(S);
		_throwif(ERR_OUTOFMEMORY, F == NULL);	/* #13 */
		F->localpool = kw_find_localpool(S, 1); /* 戻り値用 */
		/* #13: 確保途中の frame を pool へ戻す */
		if(F->localpool == NULL) {
			kw_return_frame(S, F);
			_throw(ERR_OUTOFMEMORY);
		}

		memset(F->localpool->locals, 0, sizeof(kinowasm_val_t));
		F->module = moduleinst;
		F->funcaddrs = moduleinst->funcaddrs;
		F->parent = NULL;
		F->caller_locals = NULL;
		F->root_funcaddr = -1;
		F->arity = 1;

		{
			/* push 失敗 (OOM) 時、未 push の frame/localpool を pool へ戻す (#13: リーク防止)。 */
			kinowasm_result_t push_res = kw_push_frame(S->stack, F);
			if(push_res != RES_SUCCESS) {
				kw_return_localpool(S, F->localpool);
				kw_return_frame(S, F);
				_throw(push_res);
			}
		}
		for(uint32_t j = 0; j < elem->init.len; j++) {
			code_t init = kinowasm_array_at(elem->init, j);
			_throwiferr(kw_eval_const_expr(S, init, moduleinst, F->localpool->locals, 0));
			kinowasm_array_at(eleminst.elem, j) = F->localpool->locals[0].ref;
		}
		kw_pop_frame(S->stack, &F);

		kw_return_localpool(S, F->localpool);
		kw_return_frame(S, F);
		kinowasm_array_append(S->elements, eleminst, *addr);
	}
	_catch:
	/* 失敗時は S->elements 未登録 (append は最終手順で失敗しない) の eleminst.elem を誰も
	 * 辿れないため、ここで解放する。成功時は所有権が S->elements 側へ移っており触らない。 */
	if(_result != RES_SUCCESS)
		kinowasm_array_term_from(eleminst.elem);
	return _result;
}

/* kw_orphan_moduleinst — 登録 (moduletable) も成功 instantiate も伴わずに失敗した moduleinst を
 * orphan list へ積み、kinowasm_term でまとめて kw_free_moduleinst させる (個別 sub-alloc のリーク防止)。
 * 追跡用の grow に失敗した場合は諦める (moduleinst はプール上に残り host のプール解放で回収される)。
 * instantiate 失敗 (kw_instantiate の _catch) と、instantiate 成功後の build/start 失敗 (kinowasm.c)
 * の両方から使う。 */
kinowasm_result_t kw_orphan_moduleinst(store_t* S, moduleinst_t* inst)
{
	if(S == NULL || inst == NULL)
		return RES_SUCCESS;
	if(_is_error(kinowasm_array_grow_from(S->orphan_moduleinsts, 1)))
		return ERR_OUTOFMEMORY;
	kinowasm_array_at(S->orphan_moduleinsts, S->orphan_moduleinsts.len) = inst;
	S->orphan_moduleinsts.len++;
	return RES_SUCCESS;
}

/* GROW_STORE_ARRAY — store 配列に module_field.len 個分の空きを確保する (ensure-capacity)。
 * capacity が既に len+count を満たすなら grow しない。動的モジュールの push/pop では pop が len のみ
 * truncate し capacity を残すため、無条件 grow だと push のたびに capacity が module 1 個分ずつ累積し
 * storememory がリークしていた。fresh store (capacity==len) では従来同様、不足分だけ grow する。 */
#define GROW_STORE_ARRAY(store_field, module_field) do { \
	size_t _need = S->store_field.len + (size_t)module->module_field.len; \
	if(_need > S->store_field.capacity) \
		_throwiferr(kinowasm_array_grow_from(S->store_field, _need - S->store_field.capacity)); \
} while(0)

#define ALLOC_MODULE_ADDRS(field, type, count) do { \
	moduleinst->field = (type*)kinowasm_mem_malloc(sizeof(type) * (count)); \
	_throwif(ERR_OUTOFMEMORY, moduleinst->field == NULL); \
} while(0)

/* kw_new_store — 新しい store (ランタイム全体状態) を確保・初期化する。
 * フレームスタック、各種インスタンス配列 (funcs/tables/memorys/globals/elements/datas
 * /moduletable/extra_func_type)、フレームプール、localpool 等をすべて初期化する。
 * フレームプールは OBJECT_CACHE_SIZE 個ぶん frame_t を事前確保する。
 * 戻り値: 成功時 store_t ポインタ、確保失敗時 NULL */
store_t* kw_new_store(void)
{
	store_t* S = (store_t*)kinowasm_mem_malloc(sizeof(store_t));
	if(S == NULL)
		return NULL;

	/* #5: 先に stack を NULL 化し全インスタンス配列を init しておく。init は alloc を
	 * 伴わず data=NULL にするだけなので、以降のどの失敗経路でも fail: の array_term /
	 * free が安全に呼べる (CLAUDE.md: OOM 経路は部分確保資源を必ず巻き戻す)。 */
	S->stack = NULL;
	kinowasm_array_init_from(S->funcs);
	kinowasm_array_init_from(S->tables);
	kinowasm_array_init_from(S->memorys);
	kinowasm_array_init_from(S->globals);
	kinowasm_array_init_from(S->elements);
	kinowasm_array_init_from(S->datas);
	kinowasm_array_init_from(S->moduletable);
	kinowasm_array_init_from(S->orphan_moduleinsts);
	kinowasm_array_init_from(S->extra_func_type);
	kinowasm_array_init_from(S->framepool);

	S->localpool.locals = NULL;
	S->localpool.local_size = 0;
	S->localpool.next = NULL;
	S->storememory = NULL;
	S->state_flags = 0;

	S->next_tagaddr = 0;

	if(_is_error(kw_new_stack(&S->stack)))
		goto fail;

	if(_is_error(kinowasm_array_new_from(S->framepool, OBJECT_CACHE_SIZE)))
		goto fail;

	/* framepool entry を NULL 初期化してから埋める。途中の malloc 失敗時に fail: 側が
	 * 「非 NULL の entry だけ free」で部分確保分を安全に回収できるようにするため。 */
	memset(S->framepool.data, 0, sizeof(frame_t*) * S->framepool.capacity);

	/* #30: frame pool 事前確保の OOM を検出し store 生成を失敗させる (kalloc_oom_design:
	 * 事前確保 OOM は起動時に失敗させる方針)。未チェックだと NULL frame が pool に残り
	 * 後で kw_assign_frame 経由で wild deref する。 */
	kinowasm_array_foreach(pool, frame_t*, S->framepool) {
		*pool = kinowasm_mem_malloc(sizeof(frame_t));
		if(*pool == NULL)
			goto fail;
	}

	return S;

fail:
	/* #5: 部分確保したリソースを巻き戻す。stack は kinowasm_term と同じ手順で解放
	 * (framestack_t は複合リテラルで frames.data が NULL 初期化されるため、
	 * kw_new_stack が途中失敗していても array_term は安全)。framepool entry は上で
	 * NULL 初期化済みなので非 NULL のものだけ free され、二重 free / wild free しない。 */
	if(S->stack != NULL) {
		kinowasm_array_term_from(S->stack->frames);
		kinowasm_mem_free(S->stack);
	}
	if(S->framepool.data != NULL) {
		for(size_t i = 0; i < S->framepool.capacity; i++)
			kinowasm_mem_free(S->framepool.data[i]);
		kinowasm_array_term_from(S->framepool);
	}
	kinowasm_mem_free(S);
	return NULL;
}

/* kw_reset_frames — フレームスタックを空になるまで全フレームを巻き戻す。
 * 各フレームの localpool を返却しフレーム自体もプールへ戻す。
 * 引数:
 *   S - 対象 store */
void kw_reset_frames(store_t* S)
{
	framestack_t* stack = S->stack;
	while(stack->frame_idx >= 0) {
		frame_t* frame;
		kw_pop_frame(stack, &frame);
		if(frame->localpool != NULL)
			kw_return_localpool(S, frame->localpool);

		kw_return_frame(S, frame);
	}
}

/* kw_restore_runtime_stack — 関数 invocation 失敗時にスタックを指定基準位置まで巻き戻す。
 * base_frame_idx より上のフレームを順に pop し localpool・フレームを返却する。
 * 引数:
 *   S              - 対象 store
 *   base_frame_idx - 巻き戻し先のフレームインデックス基準 */
static void kw_restore_runtime_stack(store_t* S, int64_t base_frame_idx)
{
	framestack_t* stack = S->stack;
	while(stack->frame_idx > base_frame_idx) {
		frame_t* frame;
		kw_pop_frame(stack, &frame);
		if(frame->localpool != NULL)
			kw_return_localpool(S, frame->localpool);

		kw_return_frame(S, frame);
	}
}

/* -- kw_instantiate のサブフェーズ ------------------------------------
 * いずれも instantiate の _try から _throwiferr で呼ばれる。各自 _try/_catch を持ち、
 * 失敗時は自前の合成バッファのみ解放して error を返す (frame stack の巻き戻しは
 * 呼び元 instantiate の _catch が kw_restore_runtime_stack でまとめて行う)。 */

/* export 列を構築する。各 export desc の kind に応じて moduleinst の addr 配列から対応
 * アドレスを引いて exportinst に格納する。 */
static kinowasm_result_t kw_instantiate_build_exports(module_t* module, moduleinst_t* moduleinst)
{
	_try{
		uint32_t exportidx = 0;
		_throwiferr(kinowasm_array_new_from(moduleinst->exports, module->exports.len));
		kinowasm_array_foreach(exp, export_t, module->exports) {
			exportinst_t* exportinst = &kinowasm_array_at(moduleinst->exports, exportidx);
			exportinst->name = exp->name;
			exportinst->value.kind = exp->exportdesc.kind;

			switch(exp->exportdesc.kind) {
			case IMPORTDESC_FUNC:
				exportinst->value.func = moduleinst->funcaddrs[exp->exportdesc.idx];
				break;
			case IMPORTDESC_TABLE:
				exportinst->value.table = moduleinst->tableaddrs[exp->exportdesc.idx];
				break;
			case IMPORTDESC_MEMORY:
				exportinst->value.mem = moduleinst->memaddrs[exp->exportdesc.idx];
				break;
			case IMPORTDESC_GLOBAL:
				exportinst->value.global = moduleinst->globaladdrs[exp->exportdesc.idx];
				break;
			case IMPORTDESC_TAG:
				/* WASM 3.0 EH: tag export は global tagaddr を渡す。 */
				exportinst->value.tag = moduleinst->tagaddrs[exp->exportdesc.idx];
				break;
			}
			exportidx++;
		}
	}
	_catch:
	return _result;
}

/* active element segment を table へ適用する (table64 対応)。
 * kind 0=active (offset 評価 → 境界検査 → elem 要素を table へコピー)、kind 2=declarative (即 drop)。
 * passive (kind 1) は何もしない (実行時 table.init/elem.drop が core 側で扱う)。 */
static kinowasm_result_t kw_instantiate_init_active_elements(store_t* S, module_t* module, moduleinst_t* moduleinst)
{
	_try{
		for(uint32_t j = 0; j < module->elements.len; j++) {
			element_t* elem = &kinowasm_array_at(module->elements, j);
			elemaddr_t ea = moduleinst->elemaddrs[j];
			elementinstance_t* eleminst = &kinowasm_array_at(S->elements, ea);
			switch(elem->mode.kind) {
			case 0: {  /* active: table.init 相当 */
				tableaddr_t ta = moduleinst->tableaddrs[elem->mode.table];
				tableinstance_t* tab = &kinowasm_array_at(S->tables, ta);
				kinowasm_val_t ov;
				_throwiferr(kw_eval_const_expr(S, elem->mode.offset, moduleinst, &ov, 0));
				/* table64 は i64 offset、それ以外は i32 (上位ゼロ拡張)。 */
				uint64_t d = tab->type.limits.is_64 ? (uint64_t)ov.num.i64 : (uint64_t)(uint32_t)ov.num.i32;
				uint64_t n = (uint64_t)eleminst->elem.len;   /* = elem->init.len */
				uint64_t d_end = d + n;
				_throwif(ERR_TRAP_OUT_OF_BOUNDS_TABLE_ACCESS, d_end < d || d_end > (uint64_t)tab->elem.len);
				for(uint64_t k = 0; k < n; k++)
					kinowasm_array_at(tab->elem, (size_t)(d + k)) = kinowasm_array_at(eleminst->elem, (size_t)k);
				/* active elem は適用後に drop (spec: 後続 table.init/elem.drop は空を見る)。
				 * init ではなく term で確保済み elem.data も解放する。init は data ポインタを捨てるだけで
				 * free しないため、kw_alloc_elem が確保した 1 セグメント分が orphan 化し、動的モジュールの
				 * push/pop で毎サイクルリークしていた (1 active/declarative セグメント = elem.init.len 要素)。
				 * elem は drop 後 len=0 になるので rollback / term の二重 term_from は free(NULL)=no-op で安全。 */
				kinowasm_array_term_from(eleminst->elem);
				break;
			}
			case 2:  /* declarative: 即 drop (要素を空に。term で elem.data も解放しリークを防ぐ) */
				kinowasm_array_term_from(eleminst->elem);
				break;
			}
		}
	}
	_catch:
	return _result;
}

/* active data segment を memory へ適用する (memory64 / multi-memory 対応)。
 * offset 評価 → 境界検査 → datainstance のバイト列を memory へ書き込む。 */
static kinowasm_result_t kw_instantiate_init_active_data(store_t* S, module_t* module, moduleinst_t* moduleinst)
{
	_try{
		for(uint32_t j = 0; j < module->datas.len; j++) {
			data_t* data = &kinowasm_array_at(module->datas, j);
			if(data->mode.kind != DATA_MODE_ACTIVE)
				continue;

			memaddr_t mem_a = moduleinst->memaddrs[data->mode.memory];
			memoryinstance_t* mem_inst = &kinowasm_array_at(S->memorys, mem_a);
			dataaddr_t da = moduleinst->dataaddrs[j];
			datainstance_t* datainst = &kinowasm_array_at(S->datas, da);

			kinowasm_val_t ov;
			_throwiferr(kw_eval_const_expr(S, data->mode.offset, moduleinst, &ov, 0));
			/* memory64 は i64 offset、それ以外は i32 (上位ゼロ拡張)。 */
			uint64_t d = mem_inst->type.is_64 ? (uint64_t)ov.num.i64 : (uint64_t)(uint32_t)ov.num.i32;
			uint64_t n = (uint64_t)datainst->data.len;   /* = data->init.len */
			uint64_t d_end = d + n;
			_throwif(ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS,
				d_end < d || d_end > (uint64_t)mem_inst->num_pages * 65536ull);
			if(n > 0)
				memcpy(mem_inst->base + d, datainst->data.data, (size_t)n);   /* flat へ直書き (commit 済み範囲内) */
		}
	}
	_catch:
	return _result;
}

/* kw_instantiate — モジュールをインスタンス化する。
 * store の各インスタンス配列を必要数だけ拡張し、moduleinst を確保して各種 addrs
 * (func/table/mem/data/global/elem/tag) を割り当てる。import は externvals から
 * 対応するアドレスを継承し、local 定義は alloc_* で新規確保する (tag は store-wide
 * counter で cross-module identity を担保)。続いて exports を構築し、active element
 * segment を synthetic table.init で、active data segment を synthetic memory.init
 * (memory64 対応) で初期化する。start 関数があれば kw_invoke_func で実行する。
 * 引数:
 *   S          - 対象 store
 *   module     - インスタンス化するデコード済みモジュール
 *   externvals - import 解決用の外部値列
 *   inst       - 生成した moduleinst を返す出力ポインタ
 * 戻り値: RES_SUCCESS、または確保失敗・初期化中の trap 等のエラーコード (ERR_*) */
kinowasm_result_t kw_instantiate(store_t* S, module_t* module, externvals_t* externvals, moduleinst_t** inst)
{
	/* 失敗時のスタック巻き戻し基準 (#11) のため _catch から参照できる関数スコープに置く。 */
	int64_t base_frame_idx = -1;
	/* 合成コードバッファ (table_init/memory_init) は各サブフェーズ helper のローカルに
	 * 移動済み。trap 時の解放も helper の _catch が行うため、ここでは持たない。 */
	_try{
		base_frame_idx = S->stack->frame_idx;
		/* moduleinst 確保前に throw されても _catch で誤って解放しないよう NULL 初期化 */
		*inst = NULL;
		GROW_STORE_ARRAY(funcs, functions);
		GROW_STORE_ARRAY(tables, tables);
		GROW_STORE_ARRAY(memorys, memorys);
		GROW_STORE_ARRAY(globals, globals);
		GROW_STORE_ARRAY(elements, elements);
		GROW_STORE_ARRAY(datas, datas);

		_throwif(ERR_OUTOFFUNCTION_TABLE, S->funcs.capacity >= EXTRA_FUNC_ID);

		moduleinst_t* moduleinst = *inst = (moduleinst_t*)kinowasm_mem_malloc(sizeof(moduleinst_t));
		_throwif(ERR_OUTOFMEMORY, moduleinst == NULL);	/* #9 */
		/* #10: addr 配列群を NULL 初期化。ALLOC_MODULE_ADDRS が途中で失敗しても
		 * _catch が未確保フィールドを誤って free しないようにする。 */
		memset(moduleinst, 0, sizeof(moduleinst_t));
		moduleinst->types = module->types.data;
		ALLOC_MODULE_ADDRS(funcaddrs, funcaddr_t, module->funcimport_count + module->functions.len);
		ALLOC_MODULE_ADDRS(tableaddrs, tableaddr_t, module->tableimport_count + module->tables.len);
		{
			/* WASM 3.0 multi-memory: 複数 memory の場合は import + local 全数を確保。
			 * セキュリティ: memory を一切持たないモジュールでは memaddrs を確保せず NULL の
			 * ままにする。これにより UPDATE_MEM / GET_CURRENT_MEMORY 等の `memaddrs != NULL`
			 * ガードが機能し、未初期化 memaddrs[0] を store 添字に使う wild deref を防ぐ。 */
			uint32_t total_memories = module->memoryimport_count + (uint32_t)module->memorys.len;
			if(total_memories == 0) {
				moduleinst->memaddrs = NULL;
			} else {
				moduleinst->memaddrs = (memaddr_t*)kinowasm_mem_malloc(sizeof(memaddr_t) * total_memories);
				_throwif(ERR_OUTOFMEMORY, moduleinst->memaddrs == NULL);
			}
		}
		ALLOC_MODULE_ADDRS(dataaddrs, dataaddr_t, module->datas.len);
		ALLOC_MODULE_ADDRS(globaladdrs, globaladdr_t, module->globalimport_count + module->globals.len);
		ALLOC_MODULE_ADDRS(elemaddrs, elemaddr_t, module->elements.len);
		/* WASM 3.0 EH: tagaddrs (cross-module identity)。 */
		{
			uint32_t total_tags = module->tagimport_count + (uint32_t)module->tags.len;
			if(total_tags > 0) {
				ALLOC_MODULE_ADDRS(tagaddrs, tagaddr_t, total_tags);
			} else {
				moduleinst->tagaddrs = NULL;
			}
		}

		uint32_t funcidx = 0;
		uint32_t tableidx = 0;
		uint32_t memidx = 0;
		uint32_t globalidx = 0;
		uint32_t elemidx = 0;
		uint32_t dataidx = 0;
		uint32_t tagidx = 0;

		size_t i = 0;
		kinowasm_array_foreach(import, import_t, module->imports) {
			externval_t* externval = &kinowasm_array_at_type(*externvals, externval_t, i++);
			switch(import->d.kind) {
			case IMPORTDESC_FUNC:
				moduleinst->funcaddrs[funcidx++] = externval->func;
				break;
			case IMPORTDESC_TABLE:
				moduleinst->tableaddrs[tableidx++] = externval->table;
				break;
			case IMPORTDESC_MEMORY:
				moduleinst->memaddrs[memidx++] = externval->mem;
				break;
			case IMPORTDESC_GLOBAL:
				moduleinst->globaladdrs[globalidx++] = externval->global;
				break;
			case IMPORTDESC_TAG:
				/* WASM 3.0 EH: tag import は source module の tagaddr をそのまま継承。
				 * これにより同じ tag インスタンスを catch できる。 */
				moduleinst->tagaddrs[tagidx++] = externval->tag;
				break;
			}
		}
		kinowasm_array_foreach(func, function_t, module->functions) {
			_throwiferr(kw_alloc_func(S, func, moduleinst, &moduleinst->funcaddrs[funcidx]));
			funcidx++;
		}
		kinowasm_array_foreach(table, table_t, module->tables) {
			_throwiferr(kw_alloc_table(S, table, &moduleinst->tableaddrs[tableidx]));
			tableidx++;
		}
		kinowasm_array_foreach(mem, memory_t, module->memorys) {
			_throwiferr(kw_alloc_mem(S, mem, &moduleinst->memaddrs[memidx]));
			memidx++;
		}
		kinowasm_array_foreach(data, data_t, module->datas) {
			_throwiferr(kw_alloc_data(S, data, &moduleinst->dataaddrs[dataidx]));
			dataidx++;
		}
		/* WASM 3.0 EH: local tag に新規 global tagaddr を割り当てる。
		 * cross-module identity は次の通り担保される:
		 *   - import: 上の loop で source module の tagaddr を継承
		 *   - local : ここで store-wide counter から新規 ID を発行
		 * これで同じ tag インスタンスは re-import 経由でも tagaddr が一致し、
		 * 別モジュールの local tag 同士は (たまたま local idx が同じでも) 不一致になる。 */
		for(uint32_t li = 0; li < (uint32_t)module->tags.len; li++) {
			moduleinst->tagaddrs[tagidx++] = S->next_tagaddr++;
		}
		frame_t* F = kw_assign_frame(S);
		_throwif(ERR_OUTOFMEMORY, F == NULL);	/* #13 */
		F->localpool = NULL;
		F->module = moduleinst;
		F->funcaddrs = moduleinst->funcaddrs;
		F->parent = NULL;
		{
			/* push 失敗 (OOM) 時、未 push の frame はスタック上にないため _catch の
			 * kw_restore_runtime_stack では回収されない。pool へ戻す (#13: リーク防止)。 */
			kinowasm_result_t push_res = kw_push_frame(S->stack, F);
			if(push_res != RES_SUCCESS) {
				kw_return_frame(S, F);
				_throw(push_res);
			}
		}

		kinowasm_array_foreach(global, global_t, module->globals) {
			_throwiferr(kw_alloc_global(S, global, moduleinst, moduleinst->globaladdrs + globalidx));
			globalidx++;
		}
		kinowasm_array_foreach(elem, element_t, module->elements) {
			_throwiferr(kw_alloc_elem(S, elem, moduleinst, moduleinst->elemaddrs + elemidx));
			elemidx++;
		}
		_throwiferr(kw_instantiate_build_exports(module, moduleinst));

		_throwiferr(kw_instantiate_init_active_elements(S, module, moduleinst));

		_throwiferr(kw_instantiate_init_active_data(S, module, moduleinst));

		/* start 関数は core エンジンが build 後に実行する (kinowasm.c の load フック)。 */

		kw_pop_frame(S->stack, &F);
		kw_return_frame(S, F);
	}
	_catch:
	/* 成功時は _result==RES_SUCCESS で fall-through するため何もしない。suspend 中
	 * (start 関数がホスト呼出しで中断) は実行継続のため state を触らない。
	 *
	 * 失敗時は frame stack のみ巻き戻す (#11)。store 配列・moduleinst・各 sub-alloc は
	 * 仕様準拠のため *保持* する: instantiation が trap しても、それまでに active
	 * element/data segment が import 済み table/memory へ書き込んだ funcref 等は有効で
	 * あり続ける必要がある (spec の store は monotonic で、確保物は外部から参照されうる)。
	 * 巻き戻すと、その funcref が指す func/moduleinst が消えて dangling になる
	 * (multi-memory linking テストで顕在化)。未登録となった moduleinst は orphan list に
	 * 積み、kinowasm_term でまとめて解放する。store 配列の各エントリと sub-alloc は
	 * 他の store 内容と同様、host のプール解放時に回収される。 */
	if(_result != RES_SUCCESS && (S->state_flags & STATE_FLAG_SUSPENDED) == 0) {
		kw_restore_runtime_stack(S, base_frame_idx);
		/* 合成バッファ (table_init/memory_init) の解放は各サブフェーズ helper の _catch 側。 */
		if(*inst != NULL) {
			/* 失敗 moduleinst の origin_module は未設定 (memset 0) のままなので、term 時の
			 * kw_free_module_metadata が relocate 済み types/imports/tags を解放できるよう
			 * ここで結ぶ (trap 時の core build もこれを参照する)。 */
			(*inst)->origin_module = *module;
			/* teardown 用に追跡 (kw_orphan_moduleinst)。grow 失敗時は諦める (host のプール解放で回収)。 */
			(void)kw_orphan_moduleinst(S, *inst);
			*inst = NULL;
		}
	}
	return _result;
}
