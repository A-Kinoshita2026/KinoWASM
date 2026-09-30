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
#include "core/kw_core.h"
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

int main(void)
{
	kw_core_install_default_mem_backend();

	/* global kalloc arena: kinowasm_init (kw_new_store) allocates from it. */
	uint8_t* arena = (uint8_t*)malloc(ARENA_BYTES);
	if (arena == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	kinowasm_mem_set_info(kinowasm_mem_info_init(arena, ARENA_BYTES));

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
	if (rc == 0 && (int)ret == 17 && yields2 == 1) {
		printf("PASS: EH across resume OK\n");
		return 0;
	}
	fprintf(stderr, "FAIL: expected rc=0 result=17 yields=1\n");
	return 1;
}
