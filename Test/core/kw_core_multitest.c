/* kw_core_multitest.c - multi-module driver for the core engine.
 *
 *   usage: kw_core_multitest WASM_A NAME_A WASM_B NAME_B [ITERS]
 *
 * Loads two wasm modules into one store under distinct names, then verifies
 * that each keeps an isolated linear memory and that repeated invocations of
 * its exported "f" (i32 -> i32) stay stable across module switches. Both
 * modules are exercised ITERS times (default 1000) as a stress check.
 *
 * Links KinoWASM.lib only. ASCII-only comments (new file; avoids MSVC C4819
 * without a UTF-8 BOM).
 */
#include "kinowasm.h"
#include "core/kw_core.h"
#include "kw_core_mem_backend_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ARENA_BYTES (128ull * 1024 * 1024)

/* WASI args/environ accessors. extrafunction.c is not linked, so the minimal
 * WASI fallback in core/kw_core_wasi.c reads these. */
static const char* const s_empty_argv[1] = { NULL };
int                wasi_get_argc(void) { return 0; }
const char* const* wasi_get_argv(void) { return s_empty_argv; }
const char* const* wasi_get_envp(void) { return NULL; }

static uint8_t* read_file(const char* path, size_t* out_size)
{
	FILE* f = fopen(path, "rb");
	if (f == NULL) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); fprintf(stderr, "read fail\n"); return NULL; }
	uint8_t* buf = (uint8_t*)malloc((size_t)n);
	if (buf == NULL) { fclose(f); fprintf(stderr, "read fail\n"); return NULL; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f); free(buf); fprintf(stderr, "read fail\n"); return NULL;
	}
	fclose(f);
	*out_size = (size_t)n;
	return buf;
}

/* Load one module from file under the given store name. Returns 0 on success. */
static int load_named(kinowasm_handle_t S, const char* path, const char* name)
{
	size_t size = 0;
	uint8_t* wasm = read_file(path, &size);
	if (wasm == NULL)
		return 1;
	kinowasm_mem_info_t minfo = kinowasm_mem_info_init(malloc(ARENA_BYTES), ARENA_BYTES);
	kinowasm_result_t lr = kinowasm_load_module_from_memory(S, wasm, size, name, minfo);
	if (lr != RES_SUCCESS) {
		fprintf(stderr, "load (%s) failed err=%u\n", path, lr);
		return 1;
	}
	return 0;
}

/* Invoke <name>::f(arg) -> i32. On success stores the result and the active
 * module's linear-memory base. Returns 0 on success. */
static int invoke_f(kinowasm_handle_t S, const char* name, int32_t arg,
					int32_t* out_res, uintptr_t* out_mem)
{
	kinowasm_args_t args = { 0 };
	kinowasm_array_init_from(args);
	if (kinowasm_array_new_from(args, 1) != RES_SUCCESS)
		return 1;
	args.len = 1;
	args.data[0].type = TYPE_VAL_I32;
	args.data[0].val.num.i32 = arg;
	kinowasm_result_t r = kinowasm_invoke(S, name, "f", &args);
	if (r != RES_SUCCESS) {
		fprintf(stderr, "invoke %s::%s failed err=%u\n", name, "f", r);
		kinowasm_array_term_from(args);
		return 1;
	}
	if (out_res) *out_res = (args.len > 0) ? args.data[0].val.num.i32 : 0;
	if (out_mem) *out_mem = (uintptr_t)(g_rt ? g_rt->mem : NULL);
	kinowasm_array_term_from(args);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: kw_core_multitest WASM_A NAME_A WASM_B NAME_B [ITERS]\n");
		return 0;
	}
	const char* wasm_a = argv[1];
	const char* name_a = argv[2];
	const char* wasm_b = argv[3];
	const char* name_b = argv[4];
	int iters = (argc > 5) ? atoi(argv[5]) : 1000;
	if (iters < 1) iters = 1;

	kw_core_install_default_mem_backend();
	kinowasm_mem_set_info(kinowasm_mem_info_init(malloc(ARENA_BYTES), ARENA_BYTES));

	kinowasm_handle_t S = kinowasm_init();
	if (S == NULL) { fprintf(stderr, "kinowasm_init failed\n"); return 1; }
	kinowasm_assign_memory(S, malloc(ARENA_BYTES), ARENA_BYTES);

	if (load_named(S, wasm_a, name_a) != 0) return 1;
	if (load_named(S, wasm_b, name_b) != 0) return 1;

	/* Run f(1) on A, then B, then A again to confirm memory isolation and that
	 * A's linear-memory base is unchanged after switching to B and back. */
	int32_t res_a = 0, res_b = 0, res_a2 = 0;
	uintptr_t mem_a = 0, mem_b = 0, mem_a2 = 0;
	if (invoke_f(S, name_a, 1, &res_a, &mem_a) != 0) return 1;
	if (invoke_f(S, name_b, 1, &res_b, &mem_b) != 0) return 1;
	if (invoke_f(S, name_a, 1, &res_a2, &mem_a2) != 0) return 1;

	printf("%s mem=0x%X  %s mem=0x%X  %s mem(re)=0x%X\n",
		name_a, (unsigned)mem_a, name_b, (unsigned)mem_b, name_a, (unsigned)mem_a2);
	printf("%s::f(1)=%d  %s::f(1)=%d\n", name_a, res_a, name_b, res_b);

	/* Stress: alternate the two modules and require stable results. */
	int stress_ok = 1;
	for (int i = 0; i < iters; i++) {
		int32_t a = 0, b = 0;
		if (invoke_f(S, name_a, 1, &a, NULL) != 0 ||
			invoke_f(S, name_b, 1, &b, NULL) != 0) {
			stress_ok = 0;
			break;
		}
		if (a != res_a || b != res_b) {
			stress_ok = 0;
			break;
		}
	}
	printf("stress: %d iterations %s\n", iters, stress_ok ? "OK" : "FAIL");

	int ok = stress_ok
		&& (mem_a == mem_a2)        /* A's memory base is stable */
		&& (mem_a != mem_b)         /* A and B are isolated */
		&& (res_a == res_a2);       /* A's result is stable */
	printf("%s\n", ok ? "MULTI-MODULE OK" : "MULTI-MODULE FAIL");
	return ok ? 0 : 1;
}
