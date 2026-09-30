/* kw_core_apitest.c - public-API smoke driver for the core engine.
 *
 *   usage: kw_core_apitest WASM FUNC [ARG..]
 *
 * Loads WASM via the public API (kinowasm_load_module_from_memory), invokes
 * the export FUNC with the given arguments, and prints the typed result.
 * Each ARG may carry a type prefix: "i64:N" / "f32:X" / "f64:X"; a bare value
 * is treated as i32. Links KinoWASM.lib only.
 *
 * ASCII-only comments (new file; avoids MSVC C4819 without a UTF-8 BOM).
 */
#include "kinowasm.h"
#include "kw_core_mem_backend_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BYTES (64ull * 1024 * 1024)

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

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: kw_core_apitest WASM FUNC [ARG..]\n");
		return 0;
	}
	const char* path = argv[1];
	const char* func = argv[2];
	int nargs = argc - 3;

	kw_core_install_default_mem_backend();
	kinowasm_mem_set_info(kinowasm_mem_info_init(malloc(ARENA_BYTES), ARENA_BYTES));

	kinowasm_handle_t S = kinowasm_init();
	if (S == NULL) { fprintf(stderr, "kinowasm_init failed\n"); return 1; }
	kinowasm_assign_memory(S, malloc(ARENA_BYTES), ARENA_BYTES);

	size_t size = 0;
	uint8_t* wasm = read_file(path, &size);
	if (wasm == NULL)
		return 1;
	kinowasm_mem_info_t minfo = kinowasm_mem_info_init(malloc(ARENA_BYTES), ARENA_BYTES);
	kinowasm_result_t lr = kinowasm_load_module_from_memory(S, wasm, size, "Module", minfo);
	if (lr != RES_SUCCESS) { fprintf(stderr, "load failed err=%u\n", lr); return 1; }

	/* Build the argument list (also the result list: invoke overwrites it). */
	kinowasm_args_t args = { 0 };
	kinowasm_array_init_from(args);
	if (kinowasm_array_new_from(args, nargs ? (size_t)nargs : 1) != RES_SUCCESS) {
		fprintf(stderr, "args alloc fail\n");
		return 1;
	}
	args.len = (size_t)nargs;
	for (int i = 0; i < nargs; i++) {
		const char* a = argv[3 + i];
		if (strncmp(a, "i64:", 4) == 0) {
			args.data[i].type = TYPE_VAL_I64;
			args.data[i].val.num.i64 = strtoll(a + 4, NULL, 0);
		} else if (strncmp(a, "f32:", 4) == 0) {
			args.data[i].type = TYPE_VAL_F32;
			args.data[i].val.num.f32 = (float)atof(a + 4);
		} else if (strncmp(a, "f64:", 4) == 0) {
			args.data[i].type = TYPE_VAL_F64;
			args.data[i].val.num.f64 = atof(a + 4);
		} else {
			args.data[i].type = TYPE_VAL_I32;
			args.data[i].val.num.i32 = (int32_t)strtol(a, NULL, 0);
		}
	}

	kinowasm_result_t ir = kinowasm_invoke(S, "Module", func, &args);
	if (ir != RES_SUCCESS) { fprintf(stderr, "invoke failed err=%u\n", ir); return 1; }

	if (args.len == 0) {
		printf("%s -> (no result)\n", func);
	} else {
		kinowasm_arg_t* r = &args.data[0];
		printf("%s -> type=0x%02X i32=%d i64=%lld f32=%g f64=%g\n",
			func, r->type, r->val.num.i32, (long long)r->val.num.i64,
			(double)r->val.num.f32, r->val.num.f64);
	}
	kinowasm_array_term_from(args);
	return 0;
}
