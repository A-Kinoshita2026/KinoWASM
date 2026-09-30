#pragma once
/* ============================================================================
 *  store_alloc.h -- instance 確保 helper の宣言 (実体は store_alloc.c)。
 *  型は store.h を先に include しておくこと。
 * ========================================================================== */
funcaddr_t kw_alloc_func(store_t* S, function_t* func, moduleinst_t* moduleinst, funcaddr_t* addr);
tableaddr_t kw_alloc_table(store_t* S, table_t* table, tableaddr_t* addr);
memaddr_t kw_alloc_mem(store_t* S, memory_t* mem, memaddr_t* addr);
kinowasm_result_t kw_alloc_data(store_t* S, data_t* data, dataaddr_t* addr);
