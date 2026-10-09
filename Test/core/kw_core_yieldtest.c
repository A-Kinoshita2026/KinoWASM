/* kw_core_yieldtest.c - exercises the core engine host-yield (suspend/resume)
 * path. Registers an "env"."yield" host function that returns a non-success
 * code (ERR_NEXTFRAME_YIELD), which makes core/kw_core_wasi.c arm a suspend.
 * The driver then resumes the core until it completes.
 *
 * yield_test.wasm exports "run", which calls yield() three times in a loop and
 * sums the results. The host returns host_calls*10 (10 + 20 + 30 = 60), so a
 * correct suspend/resume chain yields result=60, yields=3, host_calls=3.
 *
 * Links KinoWASM.lib only. ASCII-only comments (new file; avoids MSVC C4819
 * without a UTF-8 BOM).
 */
#include "kinowasm.h"
#include "exceptioncode.h"
#include "core/kw_core.h"
#include "store/kw_store.h"
#include "kw_core_mem_backend_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host yield propagation code. Any non-zero, non-(-1) host result code makes
 * the core suspend; the host call's i32 result is carried to the resume. */
#define ERR_NEXTFRAME_YIELD 0x2000

#define ARENA_BYTES (64ull * 1024 * 1024)

/* WASI args/environ accessors. extrafunction.c is not linked, so the minimal
 * WASI fallback in core/kw_core_wasi.c reads these. yield_test needs no args. */
static const char* const s_empty_argv[1] = { NULL };
int                wasi_get_argc(void) { return 0; }
const char* const* wasi_get_argv(void) { return s_empty_argv; }
const char* const* wasi_get_envp(void) { return NULL; }

static int g_host_calls = 0;

/* env.yield : () -> i32. Returns host_calls*10 and suspends the core. */
static kinowasm_result_t yield_host(kinowasm_callinfo_t* call)
{
	g_host_calls++;
	call->rets->data[0].type = TYPE_VAL_I32;
	call->rets->data[0].val.num.i64 = (int64_t)(g_host_calls * 10);
	return (kinowasm_result_t)ERR_NEXTFRAME_YIELD;
}

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

/* Verify the public error code and that a completed/trapped invocation leaves
 * no resumable parents behind. Each test is followed by a fresh yielding call. */
static int check_resume(kinowasm_handle_t store, const char* module, const char* name,
	kinowasm_result_t expected, int expected_yields)
{
	kinowasm_args_t args = { 0 };
	kinowasm_array_init_from(args);
	kinowasm_result_t rc = kinowasm_invoke(store, module, name, &args);
	int yields = 0;
	while(rc == ERR_NEXTFRAME_YIELD && yields < 10) {
		yields++;
		rc = kinowasm_resume(store, &args);
	}
	int ok = rc == expected && yields == expected_yields && !kw_core_is_executing();
	if(expected == RES_SUCCESS)
		ok = ok && args.len == 1 && args.data[0].val.num.i32 == 42;

	printf("%s::%s rc=%u yields=%d -> %s\n", module, name, (unsigned)rc, yields, ok ? "OK" : "FAIL");
	kinowasm_array_term_from(args);
	return !ok;
}

int main(void)
{
	kw_core_install_default_mem_backend();

	/* global kalloc arena: kinowasm_init (kw_new_store) allocates from it. */
	uint8_t* arena = (uint8_t*)malloc(ARENA_BYTES);
	if (arena == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	kinowasm_mem_set_info(kinowasm_mem_info_init(arena, ARENA_BYTES));
	kinowasm_mem_info_t root_memory = kinowasm_mem_get_info();

	kinowasm_handle_t S = kinowasm_init();
	if (S == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	void* smem = malloc(ARENA_BYTES);
	if (smem == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	kinowasm_assign_memory(S, smem, ARENA_BYTES);

	/* Register the yielding host import before load so it resolves. */
	kinowasm_extrafunc_t ef = { "env", "yield", yield_host, NULL };
	if (kinowasm_register_extra_func(&ef, 1) != RES_SUCCESS) {
		fprintf(stderr, "register failed\n");
		return 1;
	}

	size_t size = 0;
	uint8_t* wasm = read_file("yield_test.wasm", &size);
	if (wasm == NULL)
		return 1;
	void* mmem = malloc(ARENA_BYTES);
	if (mmem == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	kinowasm_mem_info_t minfo = kinowasm_mem_info_init(mmem, ARENA_BYTES);
	kinowasm_result_t lr = kinowasm_load_module_from_memory(S, wasm, size, "Module", minfo);
	if (lr != RES_SUCCESS) { fprintf(stderr, "load failed err=%u\n", lr); return 1; }

	/* Load hook has built + activated the core instance; resolve "run". */
	int32_t fi = kw_core_lookup_export("run");
	if (fi < 0) { fprintf(stderr, "load failed err=%u\n", (unsigned)RES_ERROR); return 1; }

	int64_t ret = 0;
	int yields = 0;
	int rc = kw_core_invoke((uint32_t)fi, NULL, 0, &ret);
	while (rc == 2) {                 /* 2 = host yield (suspended) */
		yields++;
		printf("[step %d] YIELD (host call #%d)\n", yields, g_host_calls);
		if (yields > 1000) { fprintf(stderr, "resume chain overflow\n"); return 1; }
		rc = kw_core_resume(&ret);
	}
	if (rc != 0) {
		fprintf(stderr, "FAIL: final result code=%u (expected SUCCESS)\n", (unsigned)rc);
		return 1;
	}

	printf("run() = %d  (yields=%d, host_calls=%d)\n", (int)ret, yields, g_host_calls);
	if (!((int)ret == 60 && yields == 3 && g_host_calls == 3)) {
		fprintf(stderr, "FAIL: expected result=60, yields=3, host_calls=3\n");
		return 1;
	}
	printf("PASS: suspend/resume OK\n");

	/* Tail calls to a yielding host still need a resumable completion point. */
	const char* tail_names[] = {
		"tail_host", "tail_indirect_host", "call_tail_host", "call_tail_indirect_host"
	};
	for(size_t i = 0; i < sizeof(tail_names) / sizeof(tail_names[0]); i++) {
		int32_t tail_fi = kw_core_lookup_export(tail_names[i]);
		if(tail_fi < 0)
			return 1;

		g_host_calls = 0;
		ret = 0;
		rc = kw_core_invoke((uint32_t)tail_fi, NULL, 0, &ret);
		if(rc != 2 || kw_core_suspend_code() != ERR_NEXTFRAME_YIELD) {
			fprintf(stderr, "FAIL: %s did not yield\n", tail_names[i]);
			return 1;
		}
		rc = kw_core_resume(&ret);
		int64_t expected = (i < 2) ? 10 : 11;
		if(rc != 0 || ret != expected || g_host_calls != 1) {
			fprintf(stderr, "FAIL: %s rc=%d ret=%lld host_calls=%d\n",
				tail_names[i], rc, (long long)ret, g_host_calls);
			return 1;
		}
	}
	printf("PASS: tail-call suspend/resume OK\n");

	/* Part 2: an exception thrown after a resume must reach the parent
	 * frame's catch (the resume loop re-enters frames individually, so it
	 * has to route g_exc_pending through the frame's call_eh dispatch). */
	size_t esize = 0;
	uint8_t* ewasm = read_file("eh_yield_test.wasm", &esize);
	if (ewasm == NULL)
		return 1;
	void* mmem2 = malloc(ARENA_BYTES);
	if (mmem2 == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	kinowasm_mem_info_t minfo2 = kinowasm_mem_info_init(mmem2, ARENA_BYTES);
	lr = kinowasm_load_module_from_memory(S, ewasm, esize, "ModuleEH", minfo2);
	if (lr != RES_SUCCESS) { fprintf(stderr, "load failed err=%u\n", lr); return 1; }
	int32_t fi2 = kw_core_lookup_export("run_eh");
	if (fi2 < 0) { fprintf(stderr, "load failed err=%u\n", (unsigned)RES_ERROR); return 1; }

	g_host_calls = 0;
	ret = 0;
	int yields2 = 0;
	rc = kw_core_invoke((uint32_t)fi2, NULL, 0, &ret);
	while (rc == 2) {
		yields2++;
		printf("[eh step %d] YIELD (host call #%d)\n", yields2, g_host_calls);
		if (yields2 > 10) { fprintf(stderr, "resume chain overflow\n"); return 1; }
		rc = kw_core_resume(&ret);
	}
	printf("run_eh() rc=%d ret=%d  (yields=%d)\n", rc, (int)ret, yields2);
	if(rc != 0 || (int)ret != 17 || yields2 != 1) {
		fprintf(stderr, "FAIL: expected rc=0 result=17 yields=1\n");
		return 1;
	}

	size_t csize = 0;
	uint8_t* cwasm = read_file("eh_yield_caller.wasm", &csize);
	void* cmem = malloc(ARENA_BYTES);
	if(cwasm == NULL || cmem == NULL)
		return 1;

	lr = kinowasm_load_module_from_memory(S, cwasm, csize, "CallerEH",
		kinowasm_mem_info_init(cmem, ARENA_BYTES));
	if(lr != RES_SUCCESS)
		return 1;

	const char* modules[] = { "ModuleEH", "CallerEH" };
	const char* failures[] = { "nested_trap", "nested_throw", "unmatched_throw" };
	for(size_t m = 0; m < 2; m++) {
		for(size_t i = 0; i < 3; i++) {
			kinowasm_result_t expected = i == 0 ? ERR_TRAP_UNREACHABLE : ERR_WASM_EXCEPTION;
			if(check_resume(S, modules[m], failures[i], expected, 1)
				|| check_resume(S, "ModuleEH", "yield_ok", RES_SUCCESS, 1))
				return 1;
		}
	}
	if(check_resume(S, "ModuleEH", "plain_throw", ERR_WASM_EXCEPTION, 0)
		|| check_resume(S, "ModuleEH", "yield_throw", ERR_WASM_EXCEPTION, 1)
		|| check_resume(S, "ModuleEH", "yield_ref", RES_SUCCESS, 1)
		|| check_resume(S, "ModuleEH", "catch_yield", RES_SUCCESS, 2))
		return 1;

	if(check_resume(S, "CallerEH", "ref_yield", RES_SUCCESS, 1))
		return 1;

	size_t used_before = kinowasm_mem_used_size();
	for(int i = 0; i < 3; i++) {
		if(check_resume(S, "ModuleEH", "large_rethrow_yield", RES_SUCCESS, 1)
			|| check_resume(S, "ModuleEH", "large_exnref_yield", RES_SUCCESS, 1))
			return 1;
	}
	if(kinowasm_mem_used_size() != used_before) {
		fprintf(stderr, "FAIL: exception payload allocation leak\n");
		return 1;
	}

	/* A foreign current arena must not own exception payload allocations. */
	kinowasm_mem_info_t saved_memory = kinowasm_mem_get_info();
	void* small_memory = malloc(4096);
	if(small_memory == NULL)
		return 1;

	kinowasm_mem_set_info(kinowasm_mem_info_init(small_memory, 4096));
	while(kinowasm_mem_malloc(64) != NULL) {
		/* The test owns this entire disposable arena. */
	}
	int32_t large_func = kw_core_lookup_export("large_local");
	int foreign_rc = large_func < 0 ? 1 : kw_core_invoke((uint32_t)large_func, NULL, 0, &ret);
	if(foreign_rc != 0 || ret != 42)
		return 1;

	/* Exhaust the executing store's allocator and verify a recoverable trap. */
	store_t* store = (store_t*)S;
	kinowasm_mem_info_t saved_store_memory = store->storememory;
	store->storememory = kinowasm_mem_get_info();
	int oom_rc = large_func < 0 ? 0 : kw_core_invoke((uint32_t)large_func, NULL, 0, &ret);
	store->storememory = saved_store_memory;
	kinowasm_mem_set_info(saved_memory);
	free(small_memory);
	if(oom_rc != 1 || kw_core_is_executing()
		|| check_resume(S, "ModuleEH", "large_local", RES_SUCCESS, 0))
		return 1;

	/* Teardown must release captured payloads before the host frees the arena. */
	kinowasm_args_t suspended_args = { 0 };
	kinowasm_array_init_from(suspended_args);
	if(kinowasm_invoke(S, "ModuleEH", "large_exnref_yield", &suspended_args) != ERR_NEXTFRAME_YIELD)
		return 1;

	kinowasm_array_term_from(suspended_args);
	kinowasm_term(S);
	free(smem);
	free(mmem);
	free(mmem2);
	free(cmem);
	kinowasm_mem_set_info(root_memory);
	if(kw_core_is_executing())
		return 1;

	S = kinowasm_init();
	smem = malloc(ARENA_BYTES);
	mmem2 = malloc(ARENA_BYTES);
	if(S == NULL || smem == NULL || mmem2 == NULL)
		return 1;

	kinowasm_assign_memory(S, smem, ARENA_BYTES);
	if(kinowasm_load_module_from_memory(S, ewasm, esize, "ModuleEH",
		kinowasm_mem_info_init(mmem2, ARENA_BYTES)) != RES_SUCCESS
		|| check_resume(S, "ModuleEH", "large_local", RES_SUCCESS, 0))
		return 1;

	kinowasm_term(S);
	free(smem);
	free(mmem2);
	free(wasm);
	free(ewasm);
	free(cwasm);
	kinowasm_mem_set_info(root_memory);

	printf("PASS: EH across resume OK\n");
	return 0;
}
