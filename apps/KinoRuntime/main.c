#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <inttypes.h>
#include "kw_core_mem_backend_win.h"
#include "systemmemory.h"
#include "kinowasm.h"
#include "features.h"
#include "kalloc.h"
#include "winapi.h"
#include "debug.h"

#if defined(_WINDOWS)
#include <locale.h>
#define WINAPI __stdcall
typedef void* HANDLE;
typedef HANDLE HINSTANCE;
typedef char* LPSTR;
typedef int BOOL;
BOOL WINAPI SetConsoleOutputCP(
	_In_ uint32_t wCodePageID
);
#endif

#define DATA_MEMORY_SIZE 100000000

void register_standard_func(void);
void wasi_set_args(int argc, const char* const* argv);
void wasi_set_environ(const char* const* envp);
int32_t wasi_add_preopen(const char* host_path, const char* wasi_name);
void wasi_clear_preopens(void);
int  wasi_get_exit_status(int32_t* out_code);
void wasi_reset_exit_status(void);
#if defined(ADVENTURE)
void regist_adventure_func(void);
#endif
#if defined(CORE_PROFILE)
void core_prof_dump(void);
#endif

static int assign_store_memory(kinowasm_handle_t handle)
{
	void* mem = malloc(DATA_MEMORY_SIZE);
	if (mem == NULL)
		return 0;

	kinowasm_assign_memory(handle, mem, DATA_MEMORY_SIZE);
	return 1;
}

static kinowasm_mem_info_t allocate_module_memory(void)
{
	void* memory = malloc(DATA_MEMORY_SIZE);
	if (memory == NULL)
		return NULL;
	return kinowasm_mem_info_init(memory, DATA_MEMORY_SIZE);
}

/* wasi-testsuite compatible CLI:
 *   KinoRuntime.exe [--env KEY=VAL]* [--dir HOST::GUEST]* [--version]
 *                   [--] WASM_FILE [wasm_args...]
 *
 * When WASM_FILE is given, runs as a WASI single-shot host: loads the
 * module, calls _start, and propagates proc_exit's code as the host
 * process exit code. Without WASM_FILE, falls back to the legacy mode
 * that loads "Library.wasm" + "Start.wasm" (used by CoreMark etc).
 */

static void print_usage(const char* prog)
{
	fprintf(stderr,
		"Usage: %s [--env KEY=VAL]* [--dir HOST::GUEST]* [--version] [--] WASM_FILE [args...]\n",
		prog);
}

static void print_version(void)
{
	/* The first line is parsed by the wasi-testsuite adapter. */
	printf("kinoruntime 0.1.0\n");
}

static char* xstrdup_local(const char* s)
{
	size_t n = strlen(s) + 1;
	char* r = (char*)malloc(n);
	if (r != NULL)
		memcpy(r, s, n);
	return r;
}

/* Parse a "host::guest" or "path" specifier and call wasi_add_preopen.
 * Returns 0 on success, non-zero on failure. */
static int add_dir_spec(const char* spec)
{
	const char* sep = strstr(spec, "::");
	char* host = NULL;
	const char* guest = NULL;
	int rc = 0;

	if (sep != NULL) {
		size_t hl = (size_t)(sep - spec);
		host = (char*)malloc(hl + 1);
		if (host == NULL)
			return 1;
		memcpy(host, spec, hl);
		host[hl] = '\0';
		guest = sep + 2;
	} else {
		host = xstrdup_local(spec);
		guest = spec;
	}

	if (wasi_add_preopen(host, guest) < 0) {
		fprintf(stderr, "preopen failed: %s\n", spec);
		rc = 1;
	}
	free(host);
	return rc;
}

static int run_wasi_module(const char* wasm_path,
						   int wasm_argc, const char* const* wasm_argv,
						   const char* const* envp,
						   int show_endcode)
{
	change_system_memory();
	register_standard_func();
#if defined(ADVENTURE)
	regist_adventure_func();
#endif

	wasi_reset_exit_status();
	wasi_set_args(wasm_argc, wasm_argv);
	wasi_set_environ(envp);

	kinowasm_handle_t global_store = kinowasm_init();
	if (global_store == NULL) {
		fprintf(stderr, "kinowasm_init failed\n");
		return 1;
	}

	int rc = 0;
	_try{
		_throwif(1, !assign_store_memory(global_store));

		kinowasm_mem_info_t mem_info = allocate_module_memory();
		_throwif(2, mem_info == NULL);

		_throwiferr(kinowasm_load_module(global_store, wasm_path, "Module", mem_info));

		kinowasm_args_t args = { 0 };
		kinowasm_array_init_from(args);
		kinowasm_array_new_from(args, 0);
		_throwiferr(kinowasm_invoke(global_store, "Module", "_start", &args));
		kinowasm_array_term_from(args);
	}
	_catch:
	{
		int32_t exit_code = 0;
		if (wasi_get_exit_status(&exit_code)) {
			rc = (int)exit_code;
		} else if (_result == 0) {
			rc = 0;
		} else {
			if (show_endcode)
				fprintf(stderr, "EndCode: %u\n", _result);
			rc = (int)_result;
		}
	}

	wasi_clear_preopens();
	kinowasm_term(global_store);
	return rc;
}

int main(int argc, char* argv[])
{
	kw_core_install_default_mem_backend();
#if defined(_WINDOWS)
	setlocale(LC_CTYPE, ".utf8");
	/*
	SetConsoleOutputCP(65001u);
	int oldMode = _setmode(_fileno(stdout), _O_BINARY);
	 */
#endif

	init_performance();

	/* ---- CLI parsing ---- */
	const char** env_list = NULL;
	size_t       env_len  = 0;
	const char*  wasm_file = NULL;
	int          first_wasm_arg = -1;
	int          parse_ok = 1;

	int i = 1;
	while (i < argc) {
		const char* a = argv[i];
		if (strcmp(a, "--") == 0) {
			i++;
			if (i < argc) {
				wasm_file = argv[i];
				first_wasm_arg = i;
			}
			break;
		} else if (strcmp(a, "--version") == 0) {
			print_version();
			free((void*)env_list);
			return 0;
		} else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
			print_usage(argv[0]);
			free((void*)env_list);
			return 0;
		} else if (strcmp(a, "--env") == 0) {
			if (i + 1 >= argc) { parse_ok = 0; break; }
			const char* kv = argv[++i];
			env_list = (const char**)realloc((void*)env_list, sizeof(char*) * (env_len + 2));
			if (env_list == NULL) { parse_ok = 0; break; }
			env_list[env_len++] = kv;
			env_list[env_len]   = NULL;
		} else if (strncmp(a, "--env=", 6) == 0) {
			const char* kv = a + 6;
			env_list = (const char**)realloc((void*)env_list, sizeof(char*) * (env_len + 2));
			if (env_list == NULL) { parse_ok = 0; break; }
			env_list[env_len++] = kv;
			env_list[env_len]   = NULL;
		} else if (strcmp(a, "--dir") == 0) {
			if (i + 1 >= argc) { parse_ok = 0; break; }
			if (add_dir_spec(argv[++i]) != 0) { parse_ok = 0; break; }
		} else if (strncmp(a, "--dir=", 6) == 0) {
			if (add_dir_spec(a + 6) != 0) { parse_ok = 0; break; }
		} else if (a[0] == '-' && a[1] != '\0') {
			fprintf(stderr, "Unknown option: %s\n", a);
			parse_ok = 0;
			break;
		} else {
			wasm_file = a;
			first_wasm_arg = i;
			break;
		}
		i++;
	}

	if (!parse_ok) {
		print_usage(argv[0]);
		free((void*)env_list);
		wasi_clear_preopens();
		return 2;
	}

	/* ---- WASI single-shot mode ---- */
	if (wasm_file != NULL) {
		int wasm_argc = (argc - first_wasm_arg);
		const char** wasm_argv = (const char**)malloc(sizeof(char*) * (size_t)wasm_argc);
		if (wasm_argv == NULL) {
			free((void*)env_list);
			wasi_clear_preopens();
			return 1;
		}
		for (int k = 0; k < wasm_argc; k++)
			wasm_argv[k] = argv[first_wasm_arg + k];

		int rc = run_wasi_module(wasm_file, wasm_argc, wasm_argv, env_list,
								 /*show_endcode=*/0);

		free(wasm_argv);
		free((void*)env_list);
		return rc;
	}

	free((void*)env_list);

	/* ---- Legacy mode (Library.wasm + Start.wasm) ---- */
	change_system_memory();
	register_standard_func();
#if defined(ADVENTURE)
	regist_adventure_func();
#endif

	wasi_set_args(argc, (const char* const*)argv);
#if defined(_WIN32)
	wasi_set_environ((const char* const*)_environ);
#else
	extern char** environ;
	wasi_set_environ((const char* const*)environ);
#endif

	kinowasm_handle_t global_store = kinowasm_init();

	_try{
		assign_store_memory(global_store);

		_throwiferr(kinowasm_load_module(global_store, "Library.wasm", "Lib", allocate_module_memory()));
		kinowasm_args_t args = { 0 };
		kinowasm_array_init_from(args);
		/* フィボナッチベンチマークは wasm 完結版 (WASMData/fibtest.c → fibtest.wasm) へ移行。
		 * 実行: KinoRuntime.exe fibtest.wasm [n] (計測・検証とも wasm 内で行う)。 */
#if defined(TWOPARAM)
		_throwiferr(kinowasm_load_module(global_store, "Start.wasm", "Start", NULL));
		kinowasm_array_new_from(args, 2);
		args.data[0].type = TYPE_VAL_I32;
		args.data[0].val.num.i32 = 30;
		args.data[1].type = TYPE_VAL_I32;
		args.data[1].val.num.i32 = 4;
		begin_performaince();
		_throwiferr(kinowasm_invoke(global_store, "Start", "Main", &args));
		end_performaince();
		print_performance();
		printf("Ret: %d\n", args.data[0].val.num.i32);
		kinowasm_array_term_from(args);
#else
		_throwiferr(kinowasm_load_module(global_store, "Start.wasm", "Start", allocate_module_memory()));

#if defined(KINOWASM_OPCODE_COUNTER)
		kinowasm_dump_decoded_function(global_store, "Start", "_start");
#endif
		kinowasm_array_new_from(args, 0);
		_throwiferr(kinowasm_invoke(global_store, "Start", "_start", &args));
		kinowasm_array_term_from(args);
#endif
#if defined(CORE_PROFILE)
		/* core 動的 opcode プロファイル (KINOWASM_CORE_PROFILE=ON 時のみ)。stderr へ出力。 */
		core_prof_dump();
#endif
#if defined(KINOWASM_OPCODE_COUNTER)
		kinowasm_dump_opcode_counter();
		// ヘッドレスでも回収できるよう CSV へも書き出す (CWD 直下)
		kinowasm_write_opcode_profile_csv("opcode_profile.csv");
#endif
	}
	_catch:
	printf("EndCode: %d\n", _result);
#if defined(_WINDOWS)
	if (_result != 0) {
		int c = getchar();
	}
#endif
	return _result;
}
