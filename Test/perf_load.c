/* perf_load.c — wasm モジュールのロード時間計測ハーネス。
 *
 * 使い方: KinoRuntimePerfLoad.exe <file.wasm> [iterations]
 *
 * import section を最小パースして全 import 関数を no-op stub として登録し、
 * kinowasm_load_module_from_memory (decode + instantiate + core build) の
 * 所要時間だけを計測する。ファイル I/O・ホスト関数実体は計測に含めない。
 * start section を持つモジュールでは start 実行もロード時間に含まれる点に注意。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#include "kinowasm.h"
#include "systemmemory.h"
#include "kw_core_mem_backend_win.h"   /* core 線形メモリの VirtualAlloc バックエンド */

#define DATA_MEMORY_SIZE 400000000   /* 数十MB級 wasm の decode/instantiate/core build 用に余裕を確保 */

/* WASI args/environ アクセサ。extrafunction.c を非リンクのため、
 * core/kw_core_wasi.c の最小 WASI フォールバックが読む空実装を提供する。 */
static const char* const s_empty_argv[1] = { NULL };
int                wasi_get_argc(void) { return 0; }
const char* const* wasi_get_argv(void) { return s_empty_argv; }
const char* const* wasi_get_envp(void) { return NULL; }

/* 全 import 関数に割り当てる no-op stub (ロード計測では呼ばれない想定) */
static kinowasm_result_t stub_host_func(kinowasm_callinfo_t* call)
{
	(void)call;
	return RES_SUCCESS;
}

/* 境界チェック付き最小 LEB128 リーダ (計測対象外の前処理専用) */
static uint64_t read_leb(const uint8_t** p, const uint8_t* end)
{
	uint64_t v = 0;
	int shift = 0;
	while (*p < end) {
		uint8_t b = *(*p)++;
		v |= (uint64_t)(b & 0x7F) << shift;
		if (!(b & 0x80))
			break;
		shift += 7;
	}
	return v;
}

/* import section (id=2) を最小パースし、関数 import を全て stub 登録する。
 * 登録した (module, name) 文字列は extra_func_table が参照し続けるため解放しない。 */
static int register_import_stubs(const uint8_t* wasm, size_t size)
{
	const uint8_t* p = wasm + 8;   /* magic + version をスキップ */
	const uint8_t* end = wasm + size;
	while (p < end) {
		uint8_t id = *p++;
		uint64_t sec_size = read_leb(&p, end);
		const uint8_t* sec_end = p + sec_size;
		if (sec_end > end)
			return -1;
		if (id != 2) {
			p = sec_end;
			continue;
		}

		uint64_t count = read_leb(&p, sec_end);
		if (count > 0x10000)
			return -1;
		kinowasm_extrafunc_t* table = (kinowasm_extrafunc_t*)calloc((size_t)count, sizeof(*table));
		if (table == NULL)
			return -1;
		size_t nfunc = 0;
		for (uint64_t i = 0; i < count && p < sec_end; i++) {
			uint64_t mlen = read_leb(&p, sec_end);
			if (p + mlen > sec_end) return -1;
			char* mod = (char*)malloc((size_t)mlen + 1);
			if (mod == NULL) return -1;
			memcpy(mod, p, (size_t)mlen);
			mod[mlen] = '\0';
			p += mlen;

			uint64_t nlen = read_leb(&p, sec_end);
			if (p + nlen > sec_end) return -1;
			char* name = (char*)malloc((size_t)nlen + 1);
			if (name == NULL) return -1;
			memcpy(name, p, (size_t)nlen);
			name[nlen] = '\0';
			p += nlen;

			uint8_t kind = *p++;
			if (kind == 0) {   /* func: typeidx を読み捨てて stub 登録 */
				read_leb(&p, sec_end);
				table[nfunc].module = mod;
				table[nfunc].name = name;
				table[nfunc].func = stub_host_func;
				table[nfunc].reserved = NULL;
				nfunc++;
				continue;
			}
			/* func 以外の import: 形だけ読み飛ばす (table/memory/global/tag) */
			switch (kind) {
			case 1:   /* table: reftype + limits */
				p++;
				[[fallthrough]];
			case 2: {  /* memory: limits */
				uint8_t flags = *p++;
				read_leb(&p, sec_end);            /* min */
				if (flags & 1)
					read_leb(&p, sec_end);         /* max */
				break;
			}
			case 3:   /* global: valtype + mut */
				p += 2;
				break;
			case 4:   /* tag: attribute + typeidx */
				p++;
				read_leb(&p, sec_end);
				break;
			default:
				free(mod);
				free(name);
				free(table);
				return -1;
			}
			free(mod);
			free(name);
		}
		if (nfunc > 0)
			kinowasm_register_extra_func(table, nfunc);
		printf("import stubs: %zu func imports registered\n", nfunc);
		free(table);   /* エントリ本体は register が配列へコピー済み (文字列は参照保持) */
		return 0;
	}
	printf("import stubs: no import section\n");
	return 0;
}

static double now_ms(void)
{
#if defined(_WIN32)
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
#else
	return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
#endif
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file.wasm> [iterations]\n", argv[0]);
		return 1;
	}
	const char* path = argv[1];
	int iters = (argc >= 3) ? atoi(argv[2]) : 5;
	if (iters < 1)
		iters = 1;

	/* wasm ファイルを一括読み込み (ファイル I/O を計測から外す) */
	FILE* fp = fopen(path, "rb");
	if (fp == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	long fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (fsize <= 8) {
		fprintf(stderr, "not a wasm file: %s\n", path);
		fclose(fp);
		return 1;
	}
	void* wasm = malloc((size_t)fsize);
	if (wasm == NULL || fread(wasm, 1, (size_t)fsize, fp) != (size_t)fsize) {
		fprintf(stderr, "read failed: %s\n", path);
		fclose(fp);
		return 1;
	}
	fclose(fp);
	printf("module: %s (%.2f MB)\n", path, (double)fsize / 1048576.0);

	kw_core_install_default_mem_backend();
	change_system_memory();
	if (register_import_stubs((const uint8_t*)wasm, (size_t)fsize) != 0) {
		fprintf(stderr, "import section parse failed\n");
		return 1;
	}

	void* store_mem = malloc(DATA_MEMORY_SIZE);
	void* module_mem = malloc(DATA_MEMORY_SIZE);
	if (store_mem == NULL || module_mem == NULL) {
		fprintf(stderr, "host OOM\n");
		return 1;
	}

	double best = 0.0, sum = 0.0;
	for (int it = 0; it < iters; it++) {
		change_system_memory();
		kinowasm_handle_t store = kinowasm_init();
		if (store == NULL) {
			fprintf(stderr, "kinowasm_init failed\n");
			return 1;
		}
		kinowasm_assign_memory(store, store_mem, DATA_MEMORY_SIZE);
		kinowasm_mem_info_t mod_info = kinowasm_mem_info_init(module_mem, DATA_MEMORY_SIZE);
		if (mod_info == NULL) {
			fprintf(stderr, "module memory init failed\n");
			return 1;
		}

		double t0 = now_ms();
		kinowasm_result_t res = kinowasm_load_module_from_memory(store, wasm, (size_t)fsize, "Module", mod_info);
		double t1 = now_ms();
		double ms = t1 - t0;
		printf("load[%d]: %8.2f ms  (result=%d)\n", it, ms, (int)res);
		if (res != RES_SUCCESS) {
			fprintf(stderr, "load failed: %d\n", (int)res);
			return (int)res;
		}
		if (it == 0 || ms < best)
			best = ms;
		sum += ms;
		kinowasm_term(store);
	}
	printf("best=%.2f ms  avg=%.2f ms  (n=%d)\n", best, sum / (double)iters, iters);

	free(store_mem);
	free(module_mem);
	free(wasm);
	return 0;
}
