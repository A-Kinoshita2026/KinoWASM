/* kw_core_reentrytest.c - re-entrant invoke across a cross-module call, with
 * and without a host-yield suspend in between.
 *
 * Shape under test (the case kw_core_yieldtest does not cover):
 *
 *   ModA::afun  --(cross-module import)-->  ModB::bfun
 *                                             |
 *                                             +--> env.yhost  (suspends)
 *                                             +--> env.hostcb (host import)
 *                                                    |
 *                                                    +--> kinowasm_invoke(ModC, cfun)
 *
 * While ModB runs, the engine's executing instance is ModB (do_call_cross
 * swapped g_rt / g_compiled), but the bridge's active instance still names
 * ModA. A host callback that re-enters the public API must therefore save and
 * restore the executing pair, not the active instance: ModB's remaining code
 * (call $inner) otherwise resolves against the wrong function table.
 *
 * Cases:
 *   mode 0  cross-module call + host re-entry
 *   mode 1  cross-module call + one suspend/resume cycle (no re-entry)
 *   mode 2  cross-module call + suspend/resume + host re-entry after resume
 *   Re-entry traps in ModB and ModC must retain their public error codes
 *   while the outer call continues, both before and after a resume.
 *
 * Expected in every mode: ModA::afun() == 7 (ModB::$inner). A wrong-instance
 * restore returns 998 (ModA::$poison1) or crashes.
 *
 * Links KinoWASM.lib only. ASCII-only comments (new file; avoids MSVC C4819
 * without a UTF-8 BOM).
 */
#include "kinowasm.h"
#include "exceptioncode.h"
#include "core/kw_core.h"
#include "kw_core_mem_backend_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ARENA_BYTES (64ull * 1024 * 1024)

/* Host yield propagation code (same value kw_core_yieldtest uses): any
 * non-zero, non-(-1) host result makes the core suspend. */
#define ERR_NEXTFRAME_YIELD 0x2000

/* WASI args/environ accessors. extrafunction.c is not linked, so the minimal
 * WASI fallback in core/kw_core_wasi.c reads these. */
static const char* const s_empty_argv[1] = { NULL };
int                wasi_get_argc(void) { return 0; }
const char* const* wasi_get_argv(void) { return s_empty_argv; }
const char* const* wasi_get_envp(void) { return NULL; }

static kinowasm_handle_t g_store = NULL;
static int               g_cb_calls = 0;
static int               g_yield_calls = 0;
static int               g_reentry_value = -1;
static unsigned          g_reentry_rc = 0xFFFFFFFFu;
static const char*       g_reentry_module = "ModC";
static const char*       g_reentry_export = "cfun";

/* env.hostcb : (i32) -> i32. Re-enters the public API to call ModC::cfun while
 * ModB is still on the C stack inside a cross-module call. */
static kinowasm_result_t hostcb(kinowasm_callinfo_t* call)
{
	g_cb_calls++;

	kinowasm_args_t args = { 0 };
	kinowasm_array_init_from(args);
	g_reentry_rc = (unsigned)kinowasm_invoke(g_store, g_reentry_module, g_reentry_export, &args);
	if (g_reentry_rc == RES_SUCCESS && args.len > 0)
		g_reentry_value = args.data[0].val.num.i32;

	kinowasm_array_term_from(args);

	call->rets->data[0].type = TYPE_VAL_I32;
	call->rets->data[0].val.num.i64 = 0;
	return RES_SUCCESS;
}

/* env.yhost : (i32) -> i32. Suspends the core once per call site. */
static kinowasm_result_t yhost(kinowasm_callinfo_t* call)
{
	g_yield_calls++;
	call->rets->data[0].type = TYPE_VAL_I32;
	call->rets->data[0].val.num.i64 = 0;
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

static int load_named(kinowasm_handle_t S, const char* path, const char* name)
{
	size_t size = 0;
	uint8_t* wasm = read_file(path, &size);
	if (wasm == NULL)
		return 1;
	void* mmem = malloc(ARENA_BYTES);
	if (mmem == NULL) { fprintf(stderr, "arena OOM\n"); return 1; }
	kinowasm_mem_info_t minfo = kinowasm_mem_info_init(mmem, ARENA_BYTES);
	kinowasm_result_t lr = kinowasm_load_module_from_memory(S, wasm, size, name, minfo);
	if (lr != RES_SUCCESS) {
		fprintf(stderr, "load (%s as %s) failed err=%u\n", path, name, lr);
		return 1;
	}
	return 0;
}

/* Run ModA::afun(mode), or ModB::large_reentry(mode) for payload checks,
 * driving suspend/resume. Returns 0 when the whole case behaved as expected. */
static int run_mode(kinowasm_handle_t S, int mode, int want_yields, int want_cb,
	const char* target_module, const char* target_export, kinowasm_result_t want_rc, int large)
{
	g_cb_calls = 0;
	g_yield_calls = 0;
	g_reentry_value = -1;
	g_reentry_rc = 0xFFFFFFFFu;
	g_reentry_module = target_module;
	g_reentry_export = target_export;

	kinowasm_args_t args = { 0 };
	kinowasm_array_init_from(args);
	if (kinowasm_array_new_from(args, 1) != RES_SUCCESS) {
		fprintf(stderr, "args alloc failed\n");
		return 1;
	}
	args.len = 1;
	args.data[0].type = TYPE_VAL_I32;
	args.data[0].val.num.i32 = mode;

	kinowasm_result_t r = kinowasm_invoke(S, large ? "ModB" : "ModA", large ? "large_reentry" : "afun", &args);
	int resumes = 0;
	while ((unsigned)r == ERR_NEXTFRAME_YIELD) {
		resumes++;
		if (resumes > 10) { fprintf(stderr, "resume loop overflow\n"); break; }
		r = kinowasm_resume(S, &args);
	}
	int value = (r == RES_SUCCESS && args.len > 0) ? args.data[0].val.num.i32 : -1;
	kinowasm_array_term_from(args);

	int ok = (r == RES_SUCCESS)
		&& (value == (large ? 42 : 7))
		&& (resumes == want_yields)
		&& (g_yield_calls == want_yields)
		&& (g_cb_calls == want_cb)
		&& (want_cb == 0 || (g_reentry_rc == want_rc
			&& (want_rc != RES_SUCCESS || g_reentry_value == 123)));

	printf("mode %d: rc=%u value=%d resumes=%d yields=%d cb=%d reentry(%s::%s rc=%u expected=%u value=%d) -> %s\n",
		mode, (unsigned)r, value, resumes, g_yield_calls, g_cb_calls,
		target_module, target_export, g_reentry_rc, want_rc, g_reentry_value, ok ? "OK" : "FAIL");
	return ok ? 0 : 1;
}

int main(void)
{
	kw_core_install_default_mem_backend();

	uint8_t* arena = (uint8_t*)malloc(ARENA_BYTES);
	if (arena == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	kinowasm_mem_set_info(kinowasm_mem_info_init(arena, ARENA_BYTES));

	kinowasm_handle_t S = kinowasm_init();
	if (S == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	g_store = S;
	void* smem = malloc(ARENA_BYTES);
	if (smem == NULL) { fprintf(stderr, "init failed\n"); return 1; }
	kinowasm_assign_memory(S, smem, ARENA_BYTES);

	kinowasm_extrafunc_t ef[2] = {
		{ "env", "hostcb", hostcb, NULL },
		{ "env", "yhost",  yhost,  NULL }
	};
	if (kinowasm_register_extra_func(ef, 2) != RES_SUCCESS) {
		fprintf(stderr, "register failed\n");
		return 1;
	}

	/* ModB first: ModA imports it, and cross-module linking resolves against
	 * instances already built in the same store. */
	if (load_named(S, "reentry_b.wasm", "ModB") != 0) return 1;
	if (load_named(S, "reentry_c.wasm", "ModC") != 0) return 1;
	if (load_named(S, "reentry_a.wasm", "ModA") != 0) return 1;

	int bad = 0;
	bad |= run_mode(S, 0, 0, 1, "ModC", "cfun", RES_SUCCESS, 0);
	bad |= run_mode(S, 1, 1, 0, "ModC", "cfun", RES_SUCCESS, 0);
	bad |= run_mode(S, 2, 1, 1, "ModC", "cfun", RES_SUCCESS, 0);

	const char* target_modules[] = { "ModB", "ModC" };
	const char* trap_exports[] = { "divzero", "oob", "thrower" };
	const kinowasm_result_t trap_codes[] = {
		ERR_TRAP_INTERGER_DIVIDE_BY_ZERO,
		ERR_TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS,
		ERR_WASM_EXCEPTION,
	};
	for(size_t module_idx = 0; module_idx < sizeof(target_modules) / sizeof(target_modules[0]); module_idx++) {
		for(int resume = 0; resume <= 1; resume++) {
			for(size_t trap_idx = 0; trap_idx < sizeof(trap_exports) / sizeof(trap_exports[0]); trap_idx++) {
				bad |= run_mode(S, resume ? 2 : 0, resume, 1,
					target_modules[module_idx], trap_exports[trap_idx], trap_codes[trap_idx], 0);
			}
			/* A later successful call must not inherit the nested trap. */
			bad |= run_mode(S, resume ? 2 : 0, resume, 1, "ModC", "cfun", RES_SUCCESS, 0);
			bad |= run_mode(S, resume ? 2 : 0, resume, 1,
				target_modules[module_idx], "throw_large", ERR_WASM_EXCEPTION, 1);
		}
	}

	printf("%s\n", bad ? "REENTRY FAIL" : "REENTRY OK");
	return bad ? 1 : 0;
}
