/* pushpop_test — push_wasm_module / pop_wasm_module の動的モジュール lifecycle 回帰テスト。
 * rollback_modulepush の解放漏れ (origin_module メタデータ / export 名文字列 / tagaddrs) は
 * push/pop の繰り返しで storememory (kalloc アリーナ) に蓄積し、いずれ push が OOM で失敗
 * する。小さめのアリーナで多数回 push→invoke→pop を回し、リーク蓄積が無いことと pop 後の
 * 再 push / invoke が正常動作する (二重解放・dangling が無い) ことを検証する。 */
#include <stdio.h>
#include <string.h>
#include "kinowasm.h"
#include "systemmemory.h"
#include "kw_core_mem_backend_win.h"

extern void register_standard_func(void);
extern int32_t create_wasm_module(uint32_t module_memory_size, uint32_t script_memory_size);
extern kinowasm_result_t push_wasm_module(int32_t module_id, const char* module_name, const void* module_data, size_t module_data_size);
extern kinowasm_result_t invoke_wasm_module(int32_t module_id, const char* module_name, const char* function_name, kinowasm_args_t* args);
extern kinowasm_result_t pop_wasm_module(int32_t module_id);
extern void destroy_wasm_module(int32_t module_id);

/* (module (type ×8 (func (result i32))) (func (export "ff…f"(32字)) (result i32) (i32.const 42)))
 * type 8 個と長い export 名は 1 iteration あたりのメタデータ確保量 (= リーク時の蓄積量) を
 * 稼ぎ、解放漏れ検出を確実にするため。 */
#define TW_TYPE 0x60, 0x00, 0x01, 0x7f
#define TW_F8 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66
static const uint8_t test_wasm[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x21, 0x08, TW_TYPE, TW_TYPE, TW_TYPE, TW_TYPE, TW_TYPE, TW_TYPE, TW_TYPE, TW_TYPE,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x24, 0x01, 0x20, TW_F8, TW_F8, TW_F8, TW_F8, 0x00, 0x00,
	0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b,
};
static const char* test_func_name = "ffffffffffffffffffffffffffffffff";

#define PUSHPOP_ITERS 100000

int main(void)
{
	kw_core_install_default_mem_backend();
	change_system_memory();
	register_standard_func();

	int32_t id = create_wasm_module(64 * 1024, 48 * 1024 * 1024);
	if(id < 0) {
		printf("FAIL: create_wasm_module\n");
		return 1;
	}

	for(int32_t i = 0; i < PUSHPOP_ITERS; i++) {
		if(push_wasm_module(id, "m", test_wasm, sizeof(test_wasm)) != RES_SUCCESS) {
			printf("FAIL: push at iter %d (leak accumulation?)\n", i);
			return 1;
		}

		kinowasm_args_t args = { 0 };
		kinowasm_array_init_from(args);
		if(invoke_wasm_module(id, "m", test_func_name, &args) != RES_SUCCESS || args.len != 1 || args.data[0].val.num.i32 != 42) {
			printf("FAIL: invoke at iter %d\n", i);
			return 1;
		}

		kinowasm_array_term_from(args);

		if(pop_wasm_module(id) != RES_SUCCESS) {
			printf("FAIL: pop at iter %d\n", i);
			return 1;
		}
	}

	destroy_wasm_module(id);
	printf("PUSHPOP OK (%d iterations)\n", PUSHPOP_ITERS);
	return 0;
}
