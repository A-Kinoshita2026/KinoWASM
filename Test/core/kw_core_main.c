/* kw_core_main.c - standalone driver for the core engine (kw_core_run).
 *
 * Loads a wasm file and runs it. With "--invoke NAME" it calls the named
 * 0-arg export and prints the i32/i64 result; otherwise it runs the module's
 * "_start" (WASI command entry, e.g. CoreMark). Links KinoWASM.lib only (no
 * extrafunction.c): the minimal WASI fallback in core/kw_core_wasi.c provides
 * clock_time_get / fd_write / args_* etc.
 *
 * ASCII-only comments: this file is new and avoids non-ASCII so MSVC
 * (CP932 + /WX) does not raise C4819 without a UTF-8 BOM.
 */
#include "core/kw_core.h"
#include "kinowasm.h"               /* public API: init / load / register_extra_func */
#include "kw_core_mem_backend_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* proc_exit landing pad (defined in core/kw_core_wasi.c). */
extern jmp_buf g_exit_jmp;
extern int     g_exit_jmp_armed;
extern int     g_exit_code;

/* WASI args/environ accessors. extrafunction.c is not linked into the
 * standalone driver, so the minimal WASI fallback reads these instead. */
static int                s_argc = 0;
static const char* const* s_argv = NULL;
static const char* const* s_envp = NULL;
int                wasi_get_argc(void) { return s_argc; }
const char* const* wasi_get_argv(void) { return s_argv; }
const char* const* wasi_get_envp(void) { return s_envp; }

/* Read an entire file into a malloc'd buffer. Returns NULL on error. */
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

/* WASI import stub. The standalone driver has no real WASI host functions, so it
 * registers no-op placeholders so decode/instantiate can resolve the imports; the
 * actual WASI behavior comes from the minimal fallback in core/kw_core_wasi.c.
 * wasi_link_stub (defined in core/kw_core_bridge.c, part of the lib) is recognized
 * by kw_core_invoke_host and routed to that fallback. */
extern kinowasm_result_t wasi_link_stub(kinowasm_callinfo_t* call);

#define STANDALONE_ARENA (100ull * 1024 * 1024)   /* sys / store / module arenas (each) */

/* Set up a KinoWASM store and load `wasm` via the public API. The COREMODE load hook
 * (kinowasm.c) builds and activates the core instance. Returns 0 on success, -1 on error.
 * (Previously kw_core_bridge_load in the lib; moved here since only this driver uses it.) */
static int standalone_load(const uint8_t* wasm, size_t size)
{
	/* global kalloc arena: kinowasm_init (kw_new_store) allocates from it. */
	uint8_t* sys = (uint8_t*)malloc(STANDALONE_ARENA);
	if (sys == NULL) { fprintf(stderr, "[core] sys arena OOM\n"); return -1; }
	kinowasm_mem_set_info(kinowasm_mem_info_init(sys, STANDALONE_ARENA));

	kinowasm_handle_t S = kinowasm_init();
	if (S == NULL) { fprintf(stderr, "[core] kinowasm_init failed\n"); return -1; }

	void* smem = malloc(STANDALONE_ARENA);
	if (smem == NULL) { fprintf(stderr, "[core] store mem OOM\n"); return -1; }
	kinowasm_assign_memory(S, smem, STANDALONE_ARENA);

	static const char* const wasi_names[8] = {
		"proc_exit", "clock_time_get", "args_sizes_get", "args_get",
		"fd_seek", "fd_write", "fd_close", "fd_fdstat_get",
	};
	kinowasm_extrafunc_t stubs[8];
	for (int i = 0; i < 8; i++) {
		stubs[i].module = "wasi_snapshot_preview1";
		stubs[i].name = wasi_names[i];
		stubs[i].func = wasi_link_stub;
		stubs[i].reserved = NULL;
	}
	kinowasm_register_extra_func(stubs, 8);

	void* mmem = malloc(STANDALONE_ARENA);
	if (mmem == NULL) { fprintf(stderr, "[core] module mem OOM\n"); return -1; }
	kinowasm_mem_info_t minfo = kinowasm_mem_info_init(mmem, STANDALONE_ARENA);
	kinowasm_result_t lr = kinowasm_load_module_from_memory(S, (void*)wasm, size, "Module", minfo);
	if (lr != RES_SUCCESS) { fprintf(stderr, "[core] load failed err=%u\n", lr); return -1; }
	return 0;   /* COREMODE load hook built + activated the core instance (verified below via lookup). */
}

int main(int argc, char** argv)
{
	const char* path = "Bin/coremark.wasm";   /* default: run CoreMark */
	const char* invoke_name = NULL;

	/* CLI: WASM_FILE [--invoke NAME]
	 * The wasm file is the first positional argument (argv[1]) when it is not
	 * an option; --invoke NAME is scanned independently. */
	if (argc >= 2 && argv[1][0] != '-')
		path = argv[1];
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--invoke") == 0 && i + 1 < argc)
			invoke_name = argv[++i];
	}

	kw_core_install_default_mem_backend();
	s_argc = argc;
	s_argv = (const char* const*)argv;
	s_envp = NULL;

	size_t size = 0;
	uint8_t* wasm = read_file(path, &size);
	if (wasm == NULL)
		return 1;

	/* proc_exit (WASI) longjmps here carrying g_exit_code. */
	if (setjmp(g_exit_jmp) != 0) {
		free(wasm);
		return g_exit_code;
	}
	g_exit_jmp_armed = 1;   /* tell core_call_host the landing pad is valid */

	if (standalone_load(wasm, size) != 0) {
		free(wasm);
		return 1;
	}

	const char* entry = invoke_name ? invoke_name : "_start";
	int32_t fi = kw_core_lookup_export(entry);
	if (fi < 0) {
		fprintf(stderr, "[core] export '%s' not found\n", entry);
		free(wasm);
		return 1;
	}

	int64_t ret = 0;
	int rc = kw_core_invoke((uint32_t)fi, NULL, 0, &ret);
	while (rc == 2)                 /* drain any host yields to completion */
		rc = kw_core_resume(&ret);
	if (rc == 1) {
		fprintf(stderr, "[core] TRAP: %s\n",
			(g_rt && g_rt->trap_msg) ? g_rt->trap_msg : "(unknown)");
		free(wasm);
		return 1;
	}

	if (invoke_name)
		printf("[core] %s -> i64=%lld i32=%d\n",
			invoke_name, (long long)ret, (int)ret);

	free(wasm);
	return 0;
}
