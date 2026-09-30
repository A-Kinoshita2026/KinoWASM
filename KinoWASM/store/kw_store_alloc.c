/* ============================================================================
 *  store_alloc.c -- store への instance 確保 (cold, instantiate からのみ呼ばれる)。
 *  純粋な配列 append のみをここに置く。init-expr の評価を伴う kw_alloc_global /
 *  kw_alloc_elem は const 式評価器 (kw_eval_const_expr) を使うため store.c に置く。
 * ========================================================================== */
#include "kw_store.h"
#include "kw_store_alloc.h"
#include "core/kw_core.h"   /* flat 線形メモリの reserve バックエンド (kw_core_mem_reserve) */

funcaddr_t kw_alloc_func(store_t* S, function_t* func, moduleinst_t* moduleinst, funcaddr_t* addr)
{
	_try{
		functioninstance_t funcinst;
		funcinst.type = &moduleinst->types[func->typeidx];
		funcinst.module = moduleinst;
		funcinst.code = func;
		kinowasm_array_append(S->funcs, funcinst, *addr);
	}
	_catch:
	return _result;
}

tableaddr_t kw_alloc_table(store_t* S, table_t* table, tableaddr_t* addr)
{
	_try{
		tableinstance_t tableinst;
		uint32_t n = (uint32_t)table->tabletype.limits.min;
		tableinst.type = table->tabletype;

		_throwiferr(kinowasm_array_new_from(tableinst.elem, n));
		kinowasm_array_foreach(elem, kinowasm_ref_t, tableinst.elem)
			*elem = REF_NULL;

		kinowasm_array_append(S->tables, tableinst, *addr);
	}
	_catch:
	return _result;
}

memaddr_t kw_alloc_mem(store_t* S, memory_t* mem, memaddr_t* addr)
{
	_try{
		memoryinstance_t meminst;
		meminst.type = mem->memtype;
		meminst.num_pages = (size_t)mem->memtype.min;
		/* flat バッファを OS 仮想メモリで reserve (max 上限まで、無宣言は 4GB) + 初期ページのみ
		 * commit。core 実行エンジンはこの base をそのまま使う (build 時コピー無し)。memory64 も
		 * 現状 4GB (CORE_MAX_MEM32_PAGES) で cap clamp。reserve は物理未使用なので大 max でも軽い。 */
		uint64_t cap_pages = mem->memtype.has_max ? mem->memtype.max : 65536ull;
		if(cap_pages < meminst.num_pages) cap_pages = meminst.num_pages;
		if(cap_pages > 65536ull) cap_pages = 65536ull;   /* memory32 仕様上限 = 4GB */
		meminst.cap = cap_pages * 65536ull;
		uint64_t commit = (uint64_t)meminst.num_pages * 65536ull;
		meminst.base = kw_core_mem_reserve(meminst.cap, commit);
		_throwif(ERR_OUTOFMEMORY, meminst.base == NULL);

		kinowasm_array_append(S->memorys, meminst, *addr);
	}
	_catch:
	return _result;
}

kinowasm_result_t kw_alloc_data(store_t* S, data_t* data, dataaddr_t* addr)
{
	_try{
		datainstance_t datainst;
		_throwiferr(kinowasm_array_copy_from(datainst.data, data->init));
		kinowasm_array_append(S->datas, datainst, *addr);
	}
	_catch:
	return _result;
}
