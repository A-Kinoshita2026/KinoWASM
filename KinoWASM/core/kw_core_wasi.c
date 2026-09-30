/* core_wasi.c — CoreMark が使う最小 WASI host 関数。
 *  proc_exit / clock_time_get / fd_write / args_* / fd_seek/close/fdstat_get。
 *  proc_exit は standalone driver (g_exit_jmp_armed=1) では longjmp で _start 呼出元へ脱出、
 *  公開 API 経由ではホスト側へ exit status を記録させ trap で実行を停止する。 */
#include "kw_core.h"
#include <setjmp.h>
#if defined(_WIN32)
#include <windows.h>
#endif

jmp_buf g_exit_jmp;
int     g_exit_jmp_armed = 0; /* 1 = g_exit_jmp が setjmp 済み (standalone driver が main で立てる) */
int     g_exit_code = 0;

/* args/environ アクセサ (testsuite=extrafunction.c / standalone=kw_core_main.c が実体)。 */
extern int wasi_get_argc(void);
extern const char* const* wasi_get_argv(void);
extern const char* const* wasi_get_envp(void);

/* KinoWASM 既存ホスト関数 (extrafunction.c の WASI 等) への橋渡し (kinowasm.c で定義)。
 * testsuite フルランでは extrafunction.c の完全な WASI (preopen/path_open/fd_read 等) が
 * 登録されており、そちらを優先利用する。standalone (kw_core_run) は未リンクで rc=-1 が返り、
 * 下の最小フォールバックが使われる。 */
extern int kw_core_invoke_host(const char* module, const char* name, const int64_t* slot_args, uint32_t nargs,
	const uint8_t* param_types, uint32_t nrets, uint8_t* mem, uint64_t mem_size, int64_t* ret);

#if defined(_WIN32)
#define WASI_EFAULT 21 /* __WASI_EFAULT: 範囲外の wasm ポインタ */
/* wasm 制御ポインタ [a, a+len) が線形メモリ内に収まるか。収まらなければ 0 (= OOB)。
 * a + len は uint64_t で計算するので u32 同士の加算でラップしない。 */
static int wasi_mem_ok(uint32_t a, uint64_t len)
{
	return (uint64_t)a + len <= g_rt->mem_size;
}

static uint32_t ld_u32(uint8_t* mem, uint32_t a)
{
	uint32_t v;
	memcpy(&v, mem + a, 4);
	return v;
}

static void st_u32(uint8_t* mem, uint32_t a, uint32_t v)
{
	memcpy(mem + a, &v, 4);
}

static void st_u64(uint8_t* mem, uint32_t a, uint64_t v)
{
	memcpy(mem + a, &v, 8);
}

static uint64_t now_ns(void)
{
	static LARGE_INTEGER freq = {0};
	static LARGE_INTEGER base = {0};
	if(freq.QuadPart == 0) {
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&base);
	}

	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	/* プログラム開始からの相対 ns。絶対値を小さく保ち、emscripten clock_t(i32) の
	 * 切り捨て/ラップを避ける (since-boot の巨大値だと stop-start が壊れる)。 */
	return (uint64_t)((double)(c.QuadPart - base.QuadPart) * 1e9 / (double)freq.QuadPart);
}
#endif

/* import func idx → WASI 関数を名前で判定し実行。args は呼出側 slot、結果(errno)を返す。 */
int64_t core_call_host(uint32_t func_idx, coreval_t* args, uint8_t* mem)
{
	coremodule_t* m = g_rt->mod;
	const char* name = NULL;
	const char* mod_name = NULL;
	for(uint32_t i = 0; i < m->num_imports; i++) {
		if(m->imports[i].kind == 0 && m->imports[i].func_idx == func_idx) {
			name = m->imports[i].name;
			mod_name = m->imports[i].module;
			break;
		}
	}

	if(!name) {
		core_trap("unknown import");
		return 0;
	}

	if(!strcmp(name, "proc_exit")) {
		g_exit_code = args[0].i32;
		if(g_exit_jmp_armed)
			longjmp(g_exit_jmp, 1); /* standalone (kw_core_run 等) の main へ脱出 */

		/* 公開 API (kinowasm_invoke) 経由: g_exit_jmp は未 setjmp なので longjmp できない。
		 * ホスト側 proc_exit (extrafunction.c) があれば exit status を記録させたうえで
		 * trap として実行を停止する。呼出側はエラー戻り後に wasi_get_exit_status() で
		 * 「proc_exit による正常終了」を判別し exit code を採用する (main.c 参照)。
		 * ホスト proc_exit の戻り値 (旧エンジンの trap 合図) は使わず、停止は core_trap が担う。 */
		{
			corefunctype_t* ft = core_func_type(m, func_idx);
			int64_t ret = 0;
			kw_core_invoke_host(mod_name, name, (const int64_t*)args,
				ft ? ft->num_params : 1, ft ? ft->param_types : NULL, 0,
				mem, g_rt->mem_size, &ret);
		}
		core_trap("proc_exit");
		return 0;
	}

	/* KinoWASM 既存ホスト関数があればそちらへ委譲 (testsuite フルランの完全 WASI)。
	 * coreval_t は 8byte union で i64 が先頭オフセットなので int64_t* へ再解釈できる。
	 * proc_exit のみ longjmp 要件があるため上で先に処理した。 */
	{
		corefunctype_t* ft = core_func_type(m, func_idx);
		uint32_t nargs = ft ? ft->num_params : 0;
		uint32_t nrets = ft ? ft->num_results : 0;
		const uint8_t* ptypes = ft ? ft->param_types : NULL; /* 引数 valtype 列 (host 型タグ付け用) */
		int64_t ret = 0;
		int rc = kw_core_invoke_host(mod_name, name, (const int64_t*)args, nargs, ptypes, nrets,
			mem, g_rt->mem_size, &ret);
		if(rc == 0)
			return ret; /* 成功: rets[0]=errno */

		if(rc != -1) {
			/* host が非success (yield 等) を返した → suspend を起爆。trapped を流用して call op の
			 * 既存分岐に拾わせ、再開チェーンを積ませる。code (ERR_NEXTFRAME_YIELD 等) は caller へ伝播。 */
			extern int g_suspended;
			extern int g_suspend_code;
			extern int64_t g_suspend_host_r0;
			g_suspend_code = rc;
			g_suspend_host_r0 = ret; /* yield 時の host 結果 (resume の最深 r0) */
			g_suspended = 1;
			g_rt->trapped = 1;
			if(g_rt->trap_msg == NULL)
				g_rt->trap_msg = "host yield";

			return ret;
		}
		/* rc == -1: 未登録 → 下の最小 WASI フォールバック (standalone 経路) */
	}

	/* ============================================================================
	 *  最小 WASI フォールバック (standalone 専用)。【削除禁止 / extrafunction.c との
	 *  重複は意図的】
	 *
	 *  以下の clock_time_get / random_get / fd_write / args_* / environ_* / fd_* 等は
	 *  extrafunction.c の完全 WASI と機能が重なるが、これは「重複の冗長コード」では
	 *  なく意図した二重実装である。extrafunction.c は KinoWASM.lib に入らず実行ファイル
	 *  ごとにコンパイルされ、Test/core/ のスタンドアロンドライバ (kw_core_run /
	 *  apitest / multitest / yieldtest) は KinoWASM.lib のみをリンクし extrafunction.c を
	 *  リンクしない。そのため上の kw_core_invoke_host が rc=-1 (未登録) を返し、この
	 *  最小フォールバックが CoreMark 等を動かす唯一の WASI 実装になる。
	 *  (testsuite / KinoRuntime.exe は extrafunction.c をリンクするので rc!=-1 で上の
	 *   委譲が使われ、ここには到達しない。)
	 *
	 *  → 「extrafunction.c と重複しているから」という理由でここを消すと standalone
	 *    ドライバが WASI を失って動かなくなる。消す場合は先に該当ドライバへ
	 *    extrafunction.c (+ winapi.c / systemmemory.c / debug.c) をリンクすること。
	 * ========================================================================== */

#if defined(_WIN32)
	if(!strcmp(name, "clock_time_get")) {
		uint32_t clock_id = (uint32_t)args[0].i32;
		uint32_t time_ptr = (uint32_t)args[2].i32;
		/* WASI clock id: 0=realtime 1=monotonic 2=process_cputime 3=thread_cputime。それ以外は EINVAL(28)。 */
		if(clock_id > 3)
			return 28;

		if(!wasi_mem_ok(time_ptr, 8))
			return WASI_EFAULT;

		uint64_t t;
		if(clock_id == 0) {
			/* realtime: epoch(1970) からの絶対 ns (high32 が非0 になる)。 */
			FILETIME ft;
			GetSystemTimePreciseAsFileTime(&ft);
			uint64_t ft100 = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;   /* 100ns since 1601 */
			t = (ft100 - 116444736000000000ull) * 100ull;   /* → ns since 1970 */
		} else {
			t = now_ns(); /* monotonic / cputime: 相対 ns */
		}
		st_u64(mem, time_ptr, t);
		return 0;
	}
	if(!strcmp(name, "random_get")) {
		/* random_get(buf, len): buf を乱数で埋める。len=0 は no-op。連続呼出で異なる値。 */
		uint32_t buf = (uint32_t)args[0].i32;
		uint32_t len = (uint32_t)args[1].i32;
		if(!wasi_mem_ok(buf, len))
			return WASI_EFAULT;

		static uint64_t s = 0x9e3779b97f4a7c15ull;
		for(uint32_t i = 0; i < len; i++) {
			/* xorshift64 */
			s ^= s << 13;
			s ^= s >> 7;
			s ^= s << 17;
			mem[buf + i] = (uint8_t)(s >> 24);
		}
		return 0;
	}
	if(!strcmp(name, "fd_write")) {
		uint32_t fd = (uint32_t)args[0].i32;
		uint32_t iovs = (uint32_t)args[1].i32;
		uint32_t iovs_len = (uint32_t)args[2].i32;
		uint32_t nwr_ptr = (uint32_t)args[3].i32;
		FILE* out = (fd == 2) ? stderr : stdout;
		if(!wasi_mem_ok(nwr_ptr, 4))
			return WASI_EFAULT;

		/* iovec 配列 (各 8 byte: ptr + len) が範囲内か先にまとめて検査。 */
		if(!wasi_mem_ok(iovs, (uint64_t)iovs_len * 8))
			return WASI_EFAULT;

		uint32_t total = 0;
		for(uint32_t i = 0; i < iovs_len; i++) {
			uint32_t p = ld_u32(mem, iovs + i*8);
			uint32_t l = ld_u32(mem, iovs + i*8 + 4);
			if(l) {
				if(!wasi_mem_ok(p, l))
					return WASI_EFAULT;

				fwrite(mem + p, 1, l, out);
			}
			total += l;
		}
		fflush(out);
		st_u32(mem, nwr_ptr, total);
		return 0;
	}
	if(!strcmp(name, "clock_res_get")) {
		/* clock_res_get(clock_id, result_ptr): 分解能(ns)を書く。id>3 は EINVAL。 */
		uint32_t clock_id = (uint32_t)args[0].i32;
		uint32_t res_ptr = (uint32_t)args[1].i32;
		if(clock_id > 3)
			return 28;

		if(!wasi_mem_ok(res_ptr, 8))
			return WASI_EFAULT;

		st_u64(mem, res_ptr, 1ull); /* 1ns 分解能 (wasi-testsuite 期待値) */
		return 0;
	}
	if(!strcmp(name, "args_sizes_get")) {
		int argc = wasi_get_argc();
		const char* const* argv = wasi_get_argv();
		if(!wasi_mem_ok((uint32_t)args[0].i32, 4) || !wasi_mem_ok((uint32_t)args[1].i32, 4))
			return WASI_EFAULT;

		uint32_t bufsz = 0;
		for(int i = 0; i < argc; i++)
			bufsz += (uint32_t)strlen(argv[i]) + 1;

		st_u32(mem, (uint32_t)args[0].i32, (uint32_t)argc);
		st_u32(mem, (uint32_t)args[1].i32, bufsz);
		return 0;
	}
	if(!strcmp(name, "args_get")) {
		/* args_get(argv_ptr, buf_ptr): argv[] に各引数の buf 内アドレスを、buf に NUL 終端文字列を書く。 */
		int argc = wasi_get_argc();
		const char* const* argv = wasi_get_argv();
		uint32_t argv_ptr = (uint32_t)args[0].i32;
		uint32_t buf = (uint32_t)args[1].i32;
		uint32_t pos = buf;
		for(int i = 0; i < argc; i++) {
			uint32_t n = (uint32_t)strlen(argv[i]) + 1;
			if(!wasi_mem_ok(argv_ptr + (uint32_t)i*4, 4) || !wasi_mem_ok(pos, n))
				return WASI_EFAULT;

			st_u32(mem, argv_ptr + (uint32_t)i*4, pos);
			memcpy(mem + pos, argv[i], n);
			pos += n;
		}
		return 0;
	}
	if(!strcmp(name, "fd_seek")) {
		/* stdin/stdout/stderr (fd 0-2) は pipe で seek 不可 → ESPIPE(29)。それ以外は EBADF(8)。 */
		uint32_t fd = (uint32_t)args[0].i32;
		return (fd <= 2) ? 29 : 8;
	}
	if(!strcmp(name, "fd_prestat_get"))
		return 8; /* EBADF: preopen 無し */

	if(!strcmp(name, "fd_prestat_dir_name"))
		return 8; /* EBADF */

	if(!strcmp(name, "environ_sizes_get")) {
		const char* const* env = wasi_get_envp();
		if(!wasi_mem_ok((uint32_t)args[0].i32, 4) || !wasi_mem_ok((uint32_t)args[1].i32, 4))
			return WASI_EFAULT;

		uint32_t cnt = 0;
		uint32_t bufsz = 0;
		if(env) {
			for(int i = 0; env[i]; i++) {
				cnt++;
				bufsz += (uint32_t)strlen(env[i]) + 1;
			}
		}

		st_u32(mem, (uint32_t)args[0].i32, cnt);
		st_u32(mem, (uint32_t)args[1].i32, bufsz);
		return 0;
	}
	if(!strcmp(name, "environ_get")) {
		const char* const* env = wasi_get_envp();
		uint32_t env_ptr = (uint32_t)args[0].i32;
		uint32_t pos = (uint32_t)args[1].i32;
		if(env) {
			for(int i = 0; env[i]; i++) {
				uint32_t n = (uint32_t)strlen(env[i]) + 1;
				if(!wasi_mem_ok(env_ptr + (uint32_t)i*4, 4) || !wasi_mem_ok(pos, n))
					return WASI_EFAULT;

				st_u32(mem, env_ptr + (uint32_t)i*4, pos);
				memcpy(mem + pos, env[i], n);
				pos += n;
			}
		}
		return 0;
	}
	if(!strcmp(name, "sched_yield"))
		return 0;

	/* 標準fdは0、他はEBADF */
	if(!strcmp(name, "fd_close")) {
		uint32_t fd = (uint32_t)args[0].i32;
		return (fd <= 2) ? 0 : 8;
	}
	if(!strcmp(name, "fd_fdstat_get")) {
		/* fdstat: fs_filetype(u8)@0, fs_flags(u16)@2, rights_base(u64)@8, rights_inh(u64)@16 */
		uint32_t fd = (uint32_t)args[0].i32;
		if(fd > 2)
			return 8; /* EBADF */

		uint32_t stat_ptr = (uint32_t)args[1].i32;
		if(!wasi_mem_ok(stat_ptr, 24))
			return WASI_EFAULT;

		memset(mem + stat_ptr, 0, 24);
		mem[stat_ptr] = 2; /* CHARACTER_DEVICE */
		return 0;
	}
	/* spectest の print 系 import は no-op (戻り値なし)。names.wast 等が使用。 */
	if(!strncmp(name, "print", 5))
		return 0;

#endif
	core_trap("unimplemented WASI");
	return 0;
}
