/* kw_core_lifetest.c - lifecycle regression driver for the core engine.
 *
 * Covers two lifecycle regressions in kw_core_bridge.c:
 *
 *   1) Two stores alive at once: each store must own its shared value stack.
 *      A single global vstack owned by the first store caused a use-after-free
 *      in store B once store A was terminated. Verified structurally (B's
 *      active vstack must lie inside B's own arena) and behaviorally (invoke
 *      still returns the right value after A's arena is freed and poisoned).
 *
 *   2) A failed core build (unsupported opcode reached via longjmp) must
 *      restore the previously active instance. Otherwise the standalone
 *      lookup/invoke path (kw_core_lookup_export / kw_core_invoke) keeps
 *      pointing at the unregistered half-built instance.
 *      The failure must happen AFTER the new instance is activated, i.e. in
 *      core_compile_func - an earlier failure (e.g. a missing code section)
 *      would leave the active instance untouched and test nothing. Since the
 *      parser now rejects every opcode the core cannot execute, no loadable
 *      module can reach that point; the raw image is therefore handed to
 *      kw_core_build_from_store directly, bypassing the parser.
 *
 *   usage: kw_core_lifetest [OK_WASM]
 *          (default: life_ok.wasm in the CWD, i.e. Bin/)
 *
 * Links KinoWASM.lib only. ASCII-only comments (new file; avoids MSVC C4819
 * without a UTF-8 BOM).
 */
#include "kinowasm.h"
#include "core/kw_core.h"
#include "store/kw_store.h"
#include "kw_core_mem_backend_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ARENA_BYTES (64ull * 1024 * 1024)

/* Core bridge entry point (declared in kinowasm.c, no public header). Test 2
 * calls it directly to reach the failed-build path without the parser. */
extern int kw_core_build_from_store(store_t* S, moduleinst_t* inst, const uint8_t* wasm, size_t size);

/* Raw module image whose single code body uses the 0xFE (atomics) prefix, which
 * the core compiler does not implement. Only the code section is read here
 * (locate_code); every other piece of module metadata comes from the already
 * instantiated moduleinst, so no other section is needed.
 *   magic+version
 *   section 10 (code), size 6: count 1, body size 4, 0 local decls, 0xFE 0x10, end */
static const uint8_t s_bad_image[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x0a, 0x06, 0x01, 0x04, 0x00, 0xfe, 0x10, 0x0b,
};

/* Look up a registered module instance by name. */
static moduleinst_t* find_moduleinst(store_t* S, const char* name)
{
	for (size_t i = 0; i < S->moduletable.len; i++) {
		moduletable_t* mt = &kinowasm_array_at(S->moduletable, i);
		if (mt->name.data != NULL && strcmp((const char*)mt->name.data, name) == 0)
			return mt->module;
	}
	return NULL;
}

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

/* Load one module image into the store under the given name. */
static kinowasm_result_t load_named(kinowasm_handle_t S, const uint8_t* wasm, size_t size,
									const char* name)
{
	void* mmem = malloc(ARENA_BYTES);
	if (mmem == NULL) { fprintf(stderr, "module arena OOM\n"); return RES_ERROR; }
	kinowasm_mem_info_t minfo = kinowasm_mem_info_init(mmem, ARENA_BYTES);
	return kinowasm_load_module_from_memory(S, (void*)wasm, size, name, minfo);
}

/* Invoke <mod>::f(arg) -> i32 through the public API. Returns 0 on success. */
static int invoke_f(kinowasm_handle_t S, const char* mod, int32_t arg, int32_t* out_res)
{
	kinowasm_args_t args = { 0 };
	kinowasm_array_init_from(args);
	if (kinowasm_array_new_from(args, 1) != RES_SUCCESS)
		return 1;
	args.len = 1;
	args.data[0].type = TYPE_VAL_I32;
	args.data[0].val.num.i32 = arg;
	kinowasm_result_t r = kinowasm_invoke(S, mod, "f", &args);
	if (r != RES_SUCCESS) {
		fprintf(stderr, "invoke %s::f failed err=%u\n", mod, r);
		kinowasm_array_term_from(args);
		return 1;
	}
	if (out_res) *out_res = (args.len > 0) ? args.data[0].val.num.i32 : 0;
	kinowasm_array_term_from(args);
	return 0;
}

int main(int argc, char** argv)
{
	const char* ok_path = (argc > 1) ? argv[1] : "life_ok.wasm";

	kw_core_install_default_mem_backend();
	kinowasm_mem_set_info(kinowasm_mem_info_init(malloc(ARENA_BYTES), ARENA_BYTES));

	size_t ok_size = 0;
	uint8_t* ok_wasm = read_file(ok_path, &ok_size);
	if (ok_wasm == NULL)
		return 1;

	/* ---- Test 1: two stores alive at once; destroying A must not break B ---- */
	kinowasm_handle_t SA = kinowasm_init();
	kinowasm_handle_t SB = kinowasm_init();
	if (SA == NULL || SB == NULL) { fprintf(stderr, "kinowasm_init failed\n"); return 1; }
	uint8_t* arena_a = (uint8_t*)malloc(ARENA_BYTES);
	uint8_t* arena_b = (uint8_t*)malloc(ARENA_BYTES);
	if (arena_a == NULL || arena_b == NULL) { fprintf(stderr, "store arena OOM\n"); return 1; }
	kinowasm_assign_memory(SA, arena_a, ARENA_BYTES);
	kinowasm_assign_memory(SB, arena_b, ARENA_BYTES);

	if (load_named(SA, ok_wasm, ok_size, "M") != RES_SUCCESS) { fprintf(stderr, "load A failed\n"); return 1; }
	if (load_named(SB, ok_wasm, ok_size, "M") != RES_SUCCESS) { fprintf(stderr, "load B failed\n"); return 1; }

	int32_t res = 0;
	if (invoke_f(SB, "M", 1, &res) != 0 || res != 42) {
		fprintf(stderr, "B pre-invoke failed res=%d\n", res);
		return 1;
	}
	/* Structural check: after invoking B, the active vstack must lie inside B's
	 * own arena. With the old single global vstack it lived in A's arena. */
	uint8_t* vs = (uint8_t*)g_rt->vstack;
	int t1_ok = (vs >= arena_b && vs < arena_b + ARENA_BYTES);
	printf("vstack-in-own-store: %s\n", t1_ok ? "OK" : "FAIL");

	/* Behavioral check: terminate A and poison its arena (the buffer is host
	 * owned). If B still borrowed A's vstack this invoke would compute on
	 * freed-and-poisoned memory. */
	kinowasm_term(SA);
	memset(arena_a, 0xDD, ARENA_BYTES);
	if (invoke_f(SB, "M", 1, &res) != 0 || res != 42) {
		fprintf(stderr, "B post-term invoke failed res=%d\n", res);
		t1_ok = 0;
	}
	printf("two-store lifetime: %s\n", t1_ok ? "OK" : "FAIL");

	/* ---- Test 2: a failed build must restore the previous active instance ---- */
	int32_t fi_before = kw_core_lookup_export("f");   /* B::M is active after the invoke above */
	moduleinst_t* inst_b = find_moduleinst((store_t*)SB, "M");
	if (inst_b == NULL) { fprintf(stderr, "moduleinst M not found\n"); return 1; }
	/* origin_module.functions is emptied once the load completes
	 * (kw_free_module_transient), so the bridge would see zero defined functions
	 * and succeed without ever compiling. Point it at a single stub entry that
	 * matches the one code body in s_bad_image (type 0 = life_ok's (i32)->i32,
	 * no declared locals) and restore it afterwards. */
	function_t stub_fn;
	memset(&stub_fn, 0, sizeof(stub_fn));
	stub_fn.typeidx = 0;
	module_t saved_mod = inst_b->origin_module;
	inst_b->origin_module.functions.data = &stub_fn;
	inst_b->origin_module.functions.len = 1;
	inst_b->origin_module.functions.capacity = 1;
	/* Fails inside core_compile_func (unsupported 0xFE), i.e. after the new
	 * instance has been activated - the exact path that must restore prev_active. */
	int rejected = (kw_core_build_from_store((store_t*)SB, inst_b, s_bad_image, sizeof(s_bad_image)) != 0);
	inst_b->origin_module = saved_mod;
	printf("bad-build rejected: %s\n", rejected ? "OK" : "FAIL");

	/* The standalone lookup/invoke path must still reach the old module. */
	int32_t fi_after = kw_core_lookup_export("f");
	int t2_ok = rejected && fi_before >= 0 && fi_after == fi_before;
	if (t2_ok) {
		int64_t slot_args[1] = { 1 };
		int64_t ret = 0;
		if (kw_core_invoke((uint32_t)fi_after, slot_args, 1, &ret) != 0 || (int32_t)ret != 42)
			t2_ok = 0;
	}
	printf("active-restore after failed build: %s\n", t2_ok ? "OK" : "FAIL");

	int all_ok = t1_ok && t2_ok;
	printf("%s\n", all_ok ? "LIFETEST OK" : "LIFETEST FAIL");
	kinowasm_term(SB);
	free(ok_wasm);
	return all_ok ? 0 : 1;
}
