/* kw_core_bridge.c — KinoWASM 本体のパーサ/instantiate を使って core の flat ランタイムを
 * 構築する橋渡し。
 *
 *  KinoWASM 本体の parser + instantiate (全機能 + 検証) で wasm を load し、実行は core の
 *  register-TOS エンジン (kw_core_compile.c / kw_core_exec.c) で行う。instantiate 済みの
 *  store から globals/table/memory を読み出して core の自前フラット表現へ翻訳する。
 *
 *  生コードバイトだけは KinoWASM が decode 後に破棄するため、code section を最小スキャン
 *  (locate_code) して各関数本体の命令範囲を求め、core compiler へ渡す。
 *
 *  WASI import は instantiate の解決を通すためだけに no-op stub を登録する (実行時の
 *  WASI は core 側 core_call_host が flat mem 上で処理する)。
 */
#include "kw_core.h"
#include "kw_store.h"  /* store_t / moduleinst_t / memoryinstance_t 等 (KinoWASM 内部) */
#include "kinowasm.h"
#include <setjmp.h> /* compile 中 core_fatal の graceful 巻き戻し用 */
#include <string.h> /* 再入 invoke で再開チェーンを退避する memcpy 用 */
#include <stdio.h>  /* 再入 invoke の診断ログ (KW_REENTRY_LOG) */
#include <stdlib.h> /* getenv */

/* 「現在 active」な定義済み関数テーブル (def idx → compile 結果) / ランタイム。core の hot path
 * (exec.c の do_call / g_rt 参照) は単一グローバルを読むため、invoke 直前に該当インスタンスへ
 * 切り替える。複数モジュールはそれぞれ coreinstance_t を持ち、active ポインタを差し替える。
 * exec.c / driver も g_compiled を参照するのでグローバル定義のまま残す。 */
corefunc_t* g_compiled = NULL;

/* coreinstance_t — 1 モジュール分の core 状態をまとめた単位。store/moduleinst のライフサイクルに
 * 紐付き、kinowasm_term (kw_core_free_store) でまとめて解放する。複数モジュール対応の中核。 */
typedef struct coreinstance {
	struct coreinstance* next; /* レジストリ (単方向リスト) */
	store_t*      store;    /* 所属 store (term 時の解放対象判定) */
	moduleinst_t* inst;     /* 対応 moduleinst (funcaddr → core idx 逆引き) */
	coremodule_t  mod;      /* core モジュール (types/imports/funcs) */
	corert_t      rt;       /* core ランタイム (mem/globals/table/vstack) */
	corefunc_t*   compiled; /* def idx → compile 結果 (配列) */
	/* func export (name→idx) */
	struct {
		const char* name;
		uint32_t    func_idx;
	}* exports;
	uint32_t      nexports;
} coreinstance_t;

/* レジストリ (全 live インスタンス) と現在 active のインスタンス。 */
static coreinstance_t* g_core_instances = NULL;
static coreinstance_t* g_core_active    = NULL;

/* 値スタックは store 内の全インスタンス共有 (core はモジュール跨ぎ call を持たず実行が
 * 非リエントラントなため、同時に実行中のインスタンスは 1 つ)。per-instance 16MB を避け
 * store ごとに 1 本だけ確保する。公開 API は複数 store の同時生存を許すため store 単位の
 * リストで管理する (単一グローバル共有だと、先に解放された store の vstack を残存 store の
 * インスタンスが参照し続け use-after-free になる)。エントリ/vstack とも所属 store のアリーナ
 * から確保し、その store の kw_core_free_store で解放する。 */
typedef struct corevstack {
	struct corevstack* next;
	store_t*   store;
	coreval_t* vstack;
} corevstack_t;
static corevstack_t* g_vstacks = NULL;

/* 容量。KinoWASM の store/module メモリは decode+instantiate 用。 */
#define CORE_VSTACK          (2u * 1024 * 1024) /* 共有値スタック 2M slot */
/* 再入 invoke 専用領域。host 関数の中から export を呼ばれたとき (フォーカス変化での
 * SetSuspend 等) にここを使う。主領域 (0..CORE_VSTACK) は実行中/中断中のフレームが
 * 使っているので絶対に触らない。通常実行時の vstack_slots は CORE_VSTACK のままなので、
 * 外側からこの領域へ侵み出すこともない。 */
#define CORE_VSTACK_NESTED   (256u * 1024)      /* 再入専用 256K slot */
#define CORE_MAX_MEM32_PAGES (65536ull)         /* memory32 の仕様上限 (4GB)。reserve はここまで。 */

/* ───── flat 線形メモリ (ホスト注入バックエンド経由) ─────
 * WASM 線形メモリは最大 4GB へ grow しうる。store アリーナから cap 分を事前確保すると (a) max 宣言
 * 4GB で OOM (b) 0 ページ宣言でも cap 分が物理確保され多モジュールでアリーナ枯渇、になる。
 * そこで reserve (アドレス空間予約・物理未使用) + commit (使用ページのみ物理確保) で確保する。
 * base は安定 (commit で移動しない=実行中の mem ポインタが stale にならない)。core の load/store は
 * ea+SZ>size を trap で弾くので未 commit 領域 ([size, cap)) には触れない。
 * 実体 (VirtualAlloc / mmap 等) はライブラリに持たず、ホストが kw_core_set_mem_backend で注入する
 * (移植時はライブラリ非改変でバックエンドを差し替えるだけ。WASI FS の差し替えと同方式)。 */
static const kw_core_mem_backend_t* g_mem_backend = NULL;
void kw_core_set_mem_backend(const kw_core_mem_backend_t* backend)
{
	g_mem_backend = backend;
}

uint8_t* kw_core_mem_reserve(uint64_t reserve_bytes, uint64_t commit_bytes)
{
	if(reserve_bytes == 0)
		reserve_bytes = 65536;

	if(g_mem_backend == NULL || g_mem_backend->reserve == NULL)
		core_fatal("linear memory backend not installed (kw_core_set_mem_backend)");

	return (uint8_t*)g_mem_backend->reserve(reserve_bytes, commit_bytes);
}
/* grow: base の [offset, offset+len) を追加コミット。成功 0 / 失敗 -1。 */
int kw_core_mem_commit(uint8_t* base, uint64_t offset, uint64_t len)
{
	if(len == 0)
		return 0;

	if(g_mem_backend == NULL || g_mem_backend->commit == NULL)
		return -1;

	return g_mem_backend->commit(base, offset, len);
}
void kw_core_mem_release(uint8_t* base)
{
	if(base == NULL || g_mem_backend == NULL || g_mem_backend->release == NULL)
		return;

	g_mem_backend->release(base);
}

/* ───── core エンジン共通ヘルパ ─────
 * compiler (kw_core_compile.c) が core_alloc / core_fatal / core_func_type を参照する。 */

/* build 中 (メタデータ確保 / flat mem / compile) の core_fatal を exit させず
 * kw_core_build_from_store へ巻き戻すための landing pad。公開 API ロード (kinowasm_load_module)
 * 経由ではアリーナ OOM や未対応 opcode 等でプロセスを落とさず load 失敗 (ERR) として返す。
 * g_core_build_active 中のみ longjmp し、それ以外 (実行時の致命エラー) は従来通り exit する。 */
static jmp_buf g_core_build_jmp;
static int     g_core_build_active = 0;

void core_fatal(const char* msg)
{
	fprintf(stderr, "[core fatal] %s\n", msg);
	if(g_core_build_active) {
		g_core_build_active = 0;
		longjmp(g_core_build_jmp, 1);
	}
	exit(70);
}

/* core メタデータ確保 (calloc 相当)。KinoWASM の kalloc アリーナ (現在の current = build 時は
 * store メモリ) から確保し、term/reset でアリーナごと回収される。OOM は core_fatal で graceful
 * 巻き戻し。compile (kw_core_compile.c) からも参照される。 */
void* core_alloc(size_t n)
{
	void* p = kinowasm_mem_calloc(1, n);
	if(!p)
		core_fatal("OOM");

	return p;
}

/* functype の構造的シグネチャ (FNV-1a 64bit)。rt1/rt2 の valtype 列をハッシュ。
 * call_indirect 型チェックで「期待型 vs 実際型」を比較するため、構造的に等価な型は同値になる
 * (跨モジュールでも一致)。 */
static uint64_t functype_sig(functiontype_t* ft)
{
	uint64_t h = 1469598103934665603ull;
	#define FNV_BYTE(b) do { \
		h ^= (uint8_t)(b); \
		h *= 1099511628211ull; \
	} while(0)
	uint32_t n1 = (uint32_t)ft->rt1.len;
	uint32_t n2 = (uint32_t)ft->rt2.len;
	FNV_BYTE(n1);
	FNV_BYTE(n1 >> 8);
	FNV_BYTE(0xAA);
	if(ft->rt1.data) {
		for(uint32_t i = 0; i < n1; i++)
			FNV_BYTE(ft->rt1.data[i]);
	}
	FNV_BYTE(n2);
	FNV_BYTE(n2 >> 8);
	FNV_BYTE(0x55);
	if(ft->rt2.data) {
		for(uint32_t i = 0; i < n2; i++)
			FNV_BYTE(ft->rt2.data[i]);
	}
	#undef FNV_BYTE
	return h;
}

/* global func index → 関数型。import 分は imports から、定義分は funcs[].type_idx から。 */
corefunctype_t* core_func_type(coremodule_t* m, uint32_t func_idx)
{
	if(func_idx < m->num_imported_funcs) {
		for(uint32_t i = 0; i < m->num_imports; i++) {
			if(m->imports[i].kind == 0 && m->imports[i].func_idx == func_idx)
				return &m->types[m->imports[i].type_idx];
		}
		core_fatal("imported func type not found");
	}
	return &m->types[m->funcs[func_idx - m->num_imported_funcs].type_idx];
}

/* WASI link stub — import 解決用の no-op プレースホルダ。standalone driver (Test/core の
 * kw_core_run) は実 WASI を持たないため、parser/instantiate が wasi_snapshot_preview1 の
 * import を解決できるようこのスタブを登録する (登録は driver 側 standalone_load で行う)。
 * 実行時は呼ばれず、実際の WASI 動作は core_call_host の最小フォールバックが担う。
 * kw_core_invoke_host (橋渡し、kinowasm.c) はこのスタブを func ポインタ比較で除外し、
 * 最小フォールバックへ正しく落とす — そのため本体は lib 側に置く (アドレスが安定)。 */
kinowasm_result_t wasi_link_stub(kinowasm_callinfo_t* call)
{
	(void)call;
	return RES_SUCCESS;
}

/* ───── code section locator (生バイトスキャン) ─────
 * 通常は既存 decoder が検証済みの wasm を再スキャンするが、将来の呼出経路の変化や decoder との
 * 仕様差分で不整合なバイト列/size が渡っても OOB read しないよう、全読み取りを end 境界で検査
 * する (load 時のみの cold path なので検査コストは無視できる)。 */
static uint32_t loc_u32(const uint8_t* p, size_t* o, size_t end, int* err)
{
	uint32_t r = 0;
	int s = 0;
	uint8_t b;
	do {
		/* 領域超過 / u32 LEB 最大 5 byte 超過 */
		if(*o >= end || s > 28) {
			*err = 1;
			return 0;
		}
		b = p[(*o)++];
		r |= (uint32_t)(b & 0x7f) << s;
		s += 7;
	} while(b & 0x80);
	return r;
}

/* wasm の code section (id=10) を探し、各関数本体の命令先頭/終端を coremodule の funcs へ
 * 書き込む (local 宣言はスキップ。num_locals は KinoWASM 由来の slot 数を使う)。
 * 不整合 (section/body の境界超過、LEB 過長) は -1 を返す。 */
static int locate_code(const uint8_t* w, size_t n, coremodule_t* m)
{
	if(m->num_funcs == 0)
		return 0;   /* 定義済み関数なし (import/global/memory/table のみ。spectest 等) → code section 不要 */

	int err = 0;
	size_t o = 8;   /* magic(4) + version(4) */
	if(n < 8)
		return -1;

	while(o < n) {
		uint8_t id = w[o++];
		uint32_t sz = loc_u32(w, &o, n, &err);
		if(err || sz > n - o)
			return -1;   /* section が入力を超える */

		size_t sect_end = o + sz;
		if(id == 10) {
			uint32_t cnt = loc_u32(w, &o, sect_end, &err);
			if(err)
				return -1;

			for(uint32_t i = 0; i < cnt && i < m->num_funcs; i++) {
				uint32_t body_sz = loc_u32(w, &o, sect_end, &err);
				if(err || body_sz > sect_end - o)
					return -1;   /* body が section を超える */

				size_t body_end = o + body_sz;
				/* local 宣言群をスキップ → 命令先頭 */
				size_t p = o;
				uint32_t ndecl = loc_u32(w, &p, body_end, &err);
				for(uint32_t d = 0; d < ndecl && !err; d++) {
					loc_u32(w, &p, body_end, &err); /* 繰り返し数 */
					if(!err && p < body_end)
						p++; /* valtype 1 byte */
					else
						err = 1;
				}
				if(err)
					return -1;

				m->funcs[i].code_ptr = w + p;
				m->funcs[i].code_end = w + body_end;
				o = body_end;
			}
			return 0;
		}
		o = sect_end;
	}
	return -1;
}

/* store の instantiate 済み状態 (globals/table/memory) + 生 wasm から core ランタイムを構築。
 * load を済ませた store + moduleinst を受け取り、translate→g_rt→compile→export 記録を行う。
 * kinowasm.c の COREMODE フック (load 完了後) からも呼ばれる。成功で 0、失敗で -1。 */
/* 指定インスタンスを active 化する (hot path が読む g_rt/g_compiled を差し替え)。 */
static void core_activate(coreinstance_t* ni)
{
	g_core_active = ni;
	g_rt        = &ni->rt;
	g_compiled  = ni->compiled;
}

int kw_core_build_from_store(store_t* S, moduleinst_t* inst, const uint8_t* wasm, size_t size)
{
	module_t* km = &inst->origin_module;

	/* インスタンス確保 (store アリーナから。term/reset でアリーナごと回収される)。
	 * build 中の current アリーナ = store メモリ (load 直後で CHANGE_STORE_MEMORY 済)。 */
	coreinstance_t* ni = (coreinstance_t*)kinowasm_mem_calloc(1, sizeof(coreinstance_t));
	if(ni == NULL) {
		fprintf(stderr, "[core bridge] instance OOM\n");
		return -1;
	}

	ni->store = S;
	ni->inst  = inst;

	/* build 中のアリーナ OOM / 未対応 opcode (compile 内 core_fatal) をここへ巻き戻し、
	 * load 失敗 (-1) を返す (公開 API 経由でもプロセスを落とさない)。ni と途中確保は store
	 * アリーナ内なので term/reset で回収され、leak しない (登録前なので free_store 対象外)。
	 * compile は active 状態 (g_rt の is_64 参照等) を要するため core_activate(ni) をコンパイル前に
	 * 呼ぶ。失敗時にレジストリ未登録の ni が active に残ると、後続の kw_core_lookup_export /
	 * kw_core_invoke が未完成の状態 (空 exports / NULL・途中 compiled) を参照するため、build 前の
	 * active へ復帰する。
	 * 注: prev_active は setjmp 前に設定し以後変更しない (longjmp 後も値が保証される)。 */
	coreinstance_t* volatile prev_active = g_core_active;
	if(setjmp(g_core_build_jmp)) {
		fprintf(stderr, "[core bridge] build failed (OOM or unsupported op) - load rejected\n");
		if(prev_active != NULL) {
			core_activate((coreinstance_t*)prev_active);
		} else {
			g_core_active = NULL;
			g_rt = NULL;
			g_compiled = NULL;
		}
		return -1;
	}
	g_core_build_active = 1;

	/* 3. coremodule 構築 (compiler が必要とする最小: types カウント / imports / funcs) */
	coremodule_t* m = &ni->mod;

	m->num_types = (uint32_t)km->types.len;
	m->types = core_alloc(sizeof(corefunctype_t) * (m->num_types ? m->num_types : 1));
	for(uint32_t i = 0; i < m->num_types; i++) {
		m->types[i].num_params  = (uint32_t)km->types.data[i].rt1.len;
		m->types[i].num_results = (uint32_t)km->types.data[i].rt2.len;
		/* 引数 valtype 列。host 橋渡し (kw_core_invoke_host) が引数を正しい型でタグ付けするのに使う。
		 * imports[].name 等と同様 km (= inst->origin_module) の生存期間に依存するポインタ参照。 */
		m->types[i].param_types = (km->types.data[i].rt1.len > 0) ? km->types.data[i].rt1.data : NULL;
	}

	m->num_imports = (uint32_t)km->imports.len;
	m->imports = core_alloc(sizeof(coreimport_t) * (m->num_imports ? m->num_imports : 1));
	uint32_t fidx = 0;
	for(uint32_t i = 0; i < m->num_imports; i++) {
		import_t* im = &km->imports.data[i];
		m->imports[i].module = (char*)im->module.data; /* NUL 終端 (read_string) */
		m->imports[i].name   = (char*)im->name.data;
		m->imports[i].kind   = im->d.kind;             /* IMPORTDESC_FUNC=0 が core kind 0 と一致 */
		if(im->d.kind == IMPORTDESC_FUNC) {
			m->imports[i].type_idx = im->d.functypeidx;
			m->imports[i].func_idx = fidx++;
		}
	}
	m->num_imported_funcs = km->funcimport_count;

	m->num_funcs = (uint32_t)km->functions.len;
	m->funcs = core_alloc(sizeof(corefuncdef_t) * (m->num_funcs ? m->num_funcs : 1));
	for(uint32_t i = 0; i < m->num_funcs; i++) {
		m->funcs[i].type_idx   = km->functions.data[i].typeidx;
		m->funcs[i].num_locals = km->functions.data[i].local_slot_count;
		m->funcs[i].compiled   = NULL;
	}
	if(locate_code(wasm, size, m) != 0)
		core_fatal("code section not found or malformed");

	/* EH: tagidx → functype index。imported tag は imports[].d.tagtypeidx、local tag は km->tags[]。
	 * compiler が throw/catch の値数 (= functype.num_params) を算出するのに使う。 */
	{
		uint32_t ntag = km->tagimport_count + (uint32_t)km->tags.len;
		m->num_tags = ntag;
		m->tag_typeidx = core_alloc(sizeof(uint32_t) * (ntag ? ntag : 1));
		uint32_t ti2 = 0;
		for(uint32_t i = 0; i < m->num_imports; i++) {
			if(km->imports.data[i].d.kind == IMPORTDESC_TAG && ti2 < ntag)
				m->tag_typeidx[ti2++] = km->imports.data[i].d.tag.typeidx;
		}
		for(uint32_t i = 0; i < (uint32_t)km->tags.len && ti2 < ntag; i++)
			m->tag_typeidx[ti2++] = km->tags.data[i].tagtype.typeidx;
	}

	/* 4. corert 構築 (store から flat 表現へ翻訳) */
	corert_t* rt = &ni->rt;
	rt->mod = m;

	/* memory: kw_alloc_mem (store/kw_store_alloc.c) が宣言 max (無宣言は仕様上限 4GB) を上限に flat mem を
	 * OS 仮想メモリで reserve し初期ページのみ commit 済み。ここでは store のフラットバッファを
	 * 直接参照する借用ビューを作るだけでコピーは発生しない。grow は H_memory_grow が追加 commit する。
	 * reserve は物理未使用なので 0 ページ宣言や 4GB max でもアリーナを圧迫しない。
	 * base は安定なので実行中の mem ポインタが stale にならない。 */
	{
		uint32_t nmem = km->memoryimport_count + (uint32_t)km->memorys.len;
		rt->num_mems = nmem;
		if(nmem > 0) {
			rt->mems = core_alloc(sizeof(*rt->mems) * nmem);
			for(uint32_t mi = 0; mi < nmem; mi++) {
				memaddr_t ma = inst->memaddrs[mi];
				memoryinstance_t* meminst = &S->memorys.data[ma];
				uint32_t pages = (uint32_t)meminst->num_pages;
				/* 同じ memaddr を共有する import は同じ store memory (同じ base) に解決される。
				 * フラットバッファは store 側が所有し kw_core_free_store が一度だけ解放する
				 * (rt->mems は owns=0 の借用ビュー)。 */
				rt->mems[mi].base = meminst->base;
				rt->mems[mi].size = (uint64_t)pages * 65536ull;
				rt->mems[mi].cap  = meminst->cap;
				rt->mems[mi].pages = pages;
				rt->mems[mi].owns = 0;
				rt->mems[mi].is_64 = meminst->type.is_64;
				rt->mems[mi].store_memaddr = (int32_t)ma;
			}
			/* memory 0 の高速路フィールドは mems[0] をミラー。 */
			rt->mem = rt->mems[0].base;
			rt->mem_size = rt->mems[0].size;
			rt->mem_cap = rt->mems[0].cap;
			rt->mem_pages = rt->mems[0].pages;
		}
	}

	/* globals: 値配列のコピーは持たず、store の実体 (S->globals[addr]) を index 経由で共有する
	 * (global.get/set は store_ref + global_addrs[idx] で実体スロットへアクセスする)。ここでは
	 * import 分を含む通し個数 ng のみ算出する。 */
	uint32_t ng = km->globalimport_count + (uint32_t)km->globals.len;
	/* cross-module linking: global を store の実体へ index 経由で結ぶ (realloc 安全)。
	 * own/imported とも同一 store スロットを共有し、mutable global の import 共有も testsuite (get) も live。 */
	rt->store_ref = S;
	rt->inst_ref = inst;   /* funcref 値変換 (gfi ↔ store funcaddr) 用の backref */
	rt->num_global_addrs = ng;
	rt->global_addrs = core_alloc(sizeof(int32_t) * (ng ? ng : 1));
	for(uint32_t i = 0; i < ng; i++)
		rt->global_addrs[i] = (int32_t)inst->globaladdrs[i];

	/* EH: local tagidx → global tagaddr (cross-module tag identity)。throw/catch の照合に使う。 */
	{
		uint32_t ntag = km->tagimport_count + (uint32_t)km->tags.len;
		rt->num_tagaddrs = ntag;
		rt->tagaddrs = core_alloc(sizeof(int32_t) * (ntag ? ntag : 1));
		for(uint32_t i = 0; i < ntag; i++)
			rt->tagaddrs[i] = (inst->tagaddrs != NULL) ? (int32_t)inst->tagaddrs[i] : (int32_t)i;
	}

	/* multi-table: 宣言された全テーブルを個別確保。funcref を core の global func index へ逆引き
	 * (funcaddr → idx)、externref 非 null は表現不可で -1。tableidx=0 は高速路 (table/table_size) にミラー。 */
	{
		uint32_t ntab = km->tableimport_count + (uint32_t)km->tables.len;
		rt->num_tables = ntab;
		if(ntab > 0) {
			uint32_t total_funcs = km->funcimport_count + (uint32_t)km->functions.len;
			rt->tables = core_alloc(sizeof(*rt->tables) * ntab);
			for(uint32_t ti = 0; ti < ntab; ti++) {
				tableaddr_t ta = inst->tableaddrs[ti];
				tableinstance_t* tab = &S->tables.data[ta];
				uint32_t tsize = (uint32_t)tab->elem.len;
				uint32_t tmax = tab->type.limits.has_max ? (uint32_t)tab->type.limits.max : 0x10000000u;
				uint8_t is_ext = (tab->type.reftype == TYPE_EXTERNREF) ? 1 : 0;
				/* cross-module 共有テーブル: 同 store の既存インスタンスが同一 store tableaddr の
				 * data を確保済なら共有する。共有テーブルは別モジュールが要素を書き込むため、要素を
				 * store funcaddr (global) で表現する (gfi はモジュールローカルで解決不能)。owner も
				 * global_ref へ変換し、両者が funcaddr 解決経路 (call_indirect) を使う。 */
				int32_t* shared = NULL;
				struct coretab* owner_tab = NULL;
				uint8_t* owner_tgr = NULL;
				for(coreinstance_t* it = g_core_instances; it != NULL && shared == NULL; it = it->next) {
					if(it->store != S || it->inst == NULL || it->inst->tableaddrs == NULL)
						continue;

					for(uint32_t tj = 0; tj < it->rt.num_tables; tj++) {
						if(it->inst->tableaddrs[tj] == ta && it->rt.tables[tj].data != NULL) {
							shared = it->rt.tables[tj].data;
							owner_tab = &it->rt.tables[tj];
							if(tj == 0)
								owner_tgr = &it->rt.table_global_ref;

							break;
						}
					}
				}
				if(shared != NULL) {
					/* store の要素 (funcaddr) を共有 buffer へ再同期。owner の gfi も funcaddr へ上書き。 */
					for(uint32_t k = 0; k < tsize; k++) {
						kinowasm_ref_t fa = tab->elem.data[k];
						shared[k] = (fa == REF_NULL) ? -1 : (int32_t)fa;
					}
					owner_tab->global_ref = 1;
					if(owner_tgr)
						*owner_tgr = 1;

					rt->tables[ti].data = shared;
					rt->tables[ti].size = tsize;
					rt->tables[ti].max = tmax;
					rt->tables[ti].global_ref = 1;
					rt->tables[ti].owns = 0;
					rt->tables[ti].store_tableaddr = (int32_t)ta;
					rt->tables[ti].is_externref = is_ext;
					rt->tables[ti].is_64 = tab->type.limits.is_64;
				} else {
					int32_t* data = core_alloc(sizeof(int32_t) * (tsize ? tsize : 1));
					/* externref は要素=ref 値をそのまま格納。funcref は通常 gfi (モジュールローカル) だが、
					 * 要素に**別モジュールの funcref** (imported global 経由等で local func 空間に無い funcaddr)
					 * があれば gfi 化不能なので、store funcaddr を直接格納し global_ref 化 (call_indirect が
					 * funcaddr 解決で別モジュールへ)。単一モジュール (CoreMark 等) は gfi 高速路を維持。 */
					int use_funcaddr = is_ext;
					if(!use_funcaddr) {
						for(uint32_t k = 0; k < tsize; k++) {
							kinowasm_ref_t fa = tab->elem.data[k];
							if(fa == REF_NULL)
								continue;

							int found = 0;
							for(uint32_t g = 0; g < total_funcs; g++) {
								if(inst->funcaddrs[g] == fa) {
									found = 1;
									break;
								}
							}
							/* cross-module funcref 検出 */
							if(!found) {
								use_funcaddr = 1;
								break;
							}
						}
					}
					for(uint32_t k = 0; k < tsize; k++) {
						kinowasm_ref_t fa = tab->elem.data[k];
						if(use_funcaddr) {
							data[k] = (fa == REF_NULL) ? -1 : (int32_t)fa;   /* funcaddr / externref 値を直接 */
						} else {
							int32_t gfi = -1;
							if(fa != REF_NULL) {
								for(uint32_t g = 0; g < total_funcs; g++) {
									if(inst->funcaddrs[g] == fa) {
										gfi = (int32_t)g;
										break;
									}
								}
							}
							data[k] = gfi;
						}
					}
					rt->tables[ti].data = data;
					rt->tables[ti].size = tsize;
					rt->tables[ti].max = tmax;
					/* externref は call_indirect されないので global_ref 不要 (funcaddr 解決を回避)。
					 * funcref で cross-module 要素ありのときのみ global_ref。 */
					rt->tables[ti].global_ref = (use_funcaddr && !is_ext) ? 1 : 0;
					rt->tables[ti].owns = 1;
					rt->tables[ti].store_tableaddr = (int32_t)ta;
					rt->tables[ti].is_externref = is_ext;
					rt->tables[ti].is_64 = tab->type.limits.is_64;
				}
			}
			/* table 0 の高速路フィールドは tables[0] をミラー。 */
			rt->table = rt->tables[0].data;
			rt->table_size = rt->tables[0].size;
			rt->table_max = rt->tables[0].max;
			rt->table_global_ref = rt->tables[0].global_ref;
		}
	}

	/* passive data segments: store の datainstance を core から参照 (memory.init/data.drop)。
	 * bytes は store の kalloc 領域を指す (実行中は生存)。active 段は instantiate 後 len=0。 */
	{
		uint32_t nd = (uint32_t)km->datas.len;
		rt->num_datasegs = nd;
		if(nd > 0) {
			rt->datasegs = core_alloc(sizeof(*rt->datasegs) * nd);
			for(uint32_t i = 0; i < nd; i++) {
				dataaddr_t da = inst->dataaddrs[i];
				datainstance_t* di = &S->datas.data[da];
				rt->datasegs[i].bytes = di->data.data;
				rt->datasegs[i].len = (uint32_t)di->data.len;
				rt->datasegs[i].dropped = 0;
			}
		}
	}

	/* passive elem segments: store の elementinstance を core global func index へ翻訳 (table.init/
	 * elem.drop)。active/declarative 段は instantiate 後 elem 空 (len=0)。funcs は bridge 確保。 */
	{
		uint32_t ne = (uint32_t)km->elements.len;
		rt->num_elemsegs = ne;
		if(ne > 0) {
			uint32_t total_funcs = km->funcimport_count + (uint32_t)km->functions.len;
			rt->elemsegs = core_alloc(sizeof(*rt->elemsegs) * ne);
			for(uint32_t i = 0; i < ne; i++) {
				elemaddr_t ea = inst->elemaddrs[i];
				elementinstance_t* el = &S->elements.data[ea];
				uint32_t len = (uint32_t)el->elem.len;
				rt->elemsegs[i].len = len;
				rt->elemsegs[i].dropped = 0;
				rt->elemsegs[i].funcs = NULL;
				if(len > 0) {
					int32_t* fs = core_alloc(sizeof(int32_t) * len);
					for(uint32_t k = 0; k < len; k++) {
						kinowasm_ref_t fa = el->elem.data[k];
						int32_t gfi = -1;
						if(fa != REF_NULL) {
							for(uint32_t g = 0; g < total_funcs; g++) {
								if(inst->funcaddrs[g] == fa) {
									gfi = (int32_t)g;
									break;
								}
							}
							/* 別モジュール定義 (imported global 経由等) の funcref は gfi 化不能:
							 * -2-fa でエンコードして保持 (call_indirect が funcaddr 解決で呼ぶ)。 */
							if(gfi < 0)
								gfi = -2 - (int32_t)fa;
						}
						fs[k] = gfi;
					}
					rt->elemsegs[i].funcs = fs;
				}
			}
		}
	}

	/* call_indirect 型チェック用シグネチャ。type_sigs[typeidx] = 各型の構造的ハッシュ。
	 * func_sigs[global funcidx] = 各関数の実際型のハッシュ (store の functioninstance.type から)。 */
	{
		uint32_t nt = m->num_types;
		rt->num_type_sigs = nt;
		if(nt > 0) {
			rt->type_sigs = core_alloc(sizeof(uint64_t) * nt);
			for(uint32_t i = 0; i < nt; i++)
				rt->type_sigs[i] = functype_sig(&km->types.data[i]);
		}
		uint32_t nf = km->funcimport_count + (uint32_t)km->functions.len;
		rt->num_func_sigs = nf;
		if(nf > 0 && inst->funcaddrs != NULL) {
			rt->func_sigs = core_alloc(sizeof(uint64_t) * nf);
			for(uint32_t g = 0; g < nf; g++) {
				funcaddr_t fa = inst->funcaddrs[g];
				if(fa < 0 || (size_t)fa >= S->funcs.len) {
					rt->func_sigs[g] = 0;
					continue;
				}
				functioninstance_t* fi = &S->funcs.data[fa];
				rt->func_sigs[g] = fi->type ? functype_sig(fi->type) : 0;
			}
		} else {
			rt->num_func_sigs = 0;
		}
	}

	/* cross-module linking: imported func を別 core インスタンス (同 store の定義済み関数) へ解決。
	 * 見つかれば target/target_fidx を設定 (do_call が instance 切替で呼ぶ)、無ければ host。 */
	{
		/* 防御: funcaddrs 未確保なら nimp=0 とし、num_import_funcs と import_funcs (未確保=NULL) を
		 * 整合させる (do_call の import_funcs[idx] への NULL+offset deref を防ぐ。通常は
		 * funcimport_count>0 のとき funcaddrs は必ず確保済なので未到達)。 */
		uint32_t nimp = (inst->funcaddrs != NULL) ? km->funcimport_count : 0;
		rt->num_import_funcs = nimp;
		if(nimp > 0) {
			rt->import_funcs = core_alloc(sizeof(*rt->import_funcs) * nimp);
			for(uint32_t g = 0; g < nimp; g++) {
				rt->import_funcs[g].target = NULL;
				rt->import_funcs[g].target_fidx = 0;
				funcaddr_t fa = inst->funcaddrs[g];
				for(coreinstance_t* it = g_core_instances; it != NULL; it = it->next) {
					if(it->store != S)
						continue;

					moduleinst_t* mi2 = it->inst;
					if(!mi2 || !mi2->funcaddrs)
						continue;

					/* 関数数は core モジュール it->mod から取得 (origin_module.functions は load 完了時に解放)。 */
					uint32_t nimp2 = it->mod.num_imported_funcs;
					uint32_t tot2 = nimp2 + it->mod.num_funcs;
					for(uint32_t g2 = nimp2; g2 < tot2; g2++) {
						if(mi2->funcaddrs[g2] == fa) {
							rt->import_funcs[g].target = it;
							rt->import_funcs[g].target_fidx = g2 - nimp2;
							break;
						}
					}
					if(rt->import_funcs[g].target)
						break;
				}
			}
		}
	}

	/* 値スタックは store 内共有で 1 本だけ確保する (その store のアリーナから)。この store の
	 * エントリを検索し、無ければ確保してリストへ登録する (登録は vstack 確保成功後 = build 失敗
	 * 時にリストを汚さない)。解放は kw_core_free_store が store 単位で行う。 */
	{
		corevstack_t* vs = NULL;
		for(corevstack_t* it = g_vstacks; it != NULL; it = it->next) {
			if(it->store == S) {
				vs = it;
				break;
			}
		}
		if(vs == NULL) {
			vs = (corevstack_t*)core_alloc(sizeof(corevstack_t));
			vs->vstack = (coreval_t*)kinowasm_mem_malloc(sizeof(coreval_t) * (CORE_VSTACK + CORE_VSTACK_NESTED));
			if(vs->vstack == NULL)
				core_fatal("vstack OOM");

			vs->store = S;
			vs->next = g_vstacks;
			g_vstacks = vs;
		}
		rt->vstack = vs->vstack;
		rt->vstack_slots = CORE_VSTACK;
	}

	/* このインスタンスを active 化してからコンパイル/実行に入る。 */
	core_activate(ni);

	/* 5. 全関数コンパイル。未対応 opcode 等で compiler が core_fatal を呼ぶと build_jmp へ巻き戻り
	 * load 失敗 (-1) を返す (上の setjmp で握る)。 */
	ni->compiled = core_alloc(sizeof(corefunc_t) * (m->num_funcs ? m->num_funcs : 1));
	g_compiled = ni->compiled;   /* exec (do_call) が読む active テーブルを更新 */
	for(uint32_t i = 0; i < m->num_funcs; i++) {
		core_compile_func(m, i);
		ni->compiled[i] = *(corefunc_t*)m->funcs[i].compiled;
	}

	/* 6. func export を記録 (name → global func idx)。invoke はこの表で解決する。 */
	uint32_t nfexp = 0;
	for(uint32_t i = 0; i < km->exports.len; i++) {
		if(km->exports.data[i].exportdesc.kind == 0)
			nfexp++;
	}
	ni->exports = core_alloc(sizeof(*ni->exports) * (nfexp ? nfexp : 1));
	ni->nexports = 0;
	for(uint32_t i = 0; i < km->exports.len; i++) {
		export_t* e = &km->exports.data[i];
		if(e->exportdesc.kind != 0)
			continue; /* func export のみ */

		size_t nlen = e->name.len > 0 ? e->name.len - 1 : 0; /* read_string は末尾 NUL を含む */
		char* nm = core_alloc(nlen + 1);
		memcpy(nm, e->name.data, nlen);
		nm[nlen] = 0;
		ni->exports[ni->nexports].name = nm;
		ni->exports[ni->nexports].func_idx = e->exportdesc.idx;
		ni->nexports++;
	}

	/* build 成功。graceful 巻き戻し対象から外す。 */
	g_core_build_active = 0;

	/* レジストリへ登録 (これ以降 free_store の対象)。 */
	ni->next = g_core_instances;
	g_core_instances = ni;
	return 0;
}

/* export 名 → global func index。active インスタンスの表を引く (standalone 単一モジュール用)。
 * 見つからなければ -1。 */
int32_t kw_core_lookup_export(const char* name)
{
	if(g_core_active == NULL)
		return -1;

	for(uint32_t i = 0; i < g_core_active->nexports; i++) {
		if(strcmp(g_core_active->exports[i].name, name) == 0)
			return (int32_t)g_core_active->exports[i].func_idx;
	}
	return -1;
}

/* store funcaddr から所属インスタンスを特定し active 化、その core global func index を返す。
 * funcaddr は store グローバルに一意。定義済み関数 (funcimport_count 以降) のみを対象に走査
 * することで、import を再 export した funcaddr ではなく定義元モジュールへ解決する。
 * 見つからなければ -1。kinowasm.c の COREMODE invoke フックが funcref を解決するのに使う。 */
void kw_core_import_target(void* target, corert_t** out_rt, corefunc_t** out_compiled)
{
	coreinstance_t* it = (coreinstance_t*)target;
	*out_rt = &it->rt;
	*out_compiled = it->compiled;
}

/* cross-module 共有テーブル用: store funcaddr fa を定義する core インスタンスを探し、呼出に
 * 必要な (rt, compiled, def_idx) と型シグネチャ (call_indirect 型チェック用) を返す。
 * 見つかれば 1、未定義 (host 関数等で core インスタンスに無い) は 0。共有テーブルの
 * call_indirect のみが使う cold path なので線形探索で十分 (CoreMark の非共有経路は通らない)。 */
int kw_core_resolve_funcaddr(int32_t fa, corert_t** out_rt, corefunc_t** out_compiled,
	uint32_t* out_def_idx, uint64_t* out_sig)
{
	/* funcaddr は store 内でのみ一意 (store の S->funcs への index)。複数 store 同時生存時に
	 * 別 store の同値 funcaddr を先に拾わないよう、実行中 store (g_rt->store_ref) に限定して探す
	 * (sync_shared_* と同じ絞り込み)。呼出元 (H_call_indirect 系) では g_rt が実行中インスタンス。
	 * 単一 store の cross-module 共有テーブルは全モジュールが同一 store なので影響しない。 */
	void* cur_store = (g_rt != NULL) ? g_rt->store_ref : NULL;
	for(coreinstance_t* it = g_core_instances; it != NULL; it = it->next) {
		if(cur_store != NULL && it->store != cur_store)
			continue;

		moduleinst_t* mi = it->inst;
		if(mi == NULL || mi->funcaddrs == NULL)
			continue;

		/* 関数数は core モジュール it->mod から取得 (origin_module.functions は load 完了時に解放)。 */
		uint32_t nimp = it->mod.num_imported_funcs;
		uint32_t total = nimp + it->mod.num_funcs;
		for(uint32_t g = nimp; g < total; g++) {   /* 定義済み関数のみ (g>=nimp) */
			if(mi->funcaddrs[g] == (funcaddr_t)fa) {
				*out_rt = &it->rt;
				*out_compiled = it->compiled;
				*out_def_idx = g - nimp;
				if(out_sig != NULL) {
					store_t* S = it->store;
					functioninstance_t* fi = ((size_t)fa < S->funcs.len) ? &S->funcs.data[fa] : NULL;
					*out_sig = (fi && fi->type) ? functype_sig(fi->type) : 0;
				}
				return 1;
			}
		}
	}
	return 0;
}

/* 共有メモリ grow をストア横断で全 core インスタンスへ伝播する。importer が import 済み共有
 * メモリを grow すると、その base は安定 (reserve/commit) だが pages/size は grower の view しか
 * 更新されない。owner / 他 sharer の mems[].pages/size が古いままだと、それらの load/store が
 * grow 後領域を OOB と誤判定する (linking.wast: $Pm が $Mm.mem を grow → $Mm.load が stale size)。
 * 同一 store_memaddr を共有する全インスタンスの pages/size を新サイズへ揃える (base は共有のまま)。 */
void kw_core_sync_shared_mem_grow(void* store_ref, int32_t store_memaddr, uint32_t new_pages)
{
	if(store_memaddr < 0)
		return;

	uint64_t nsize = (uint64_t)new_pages * 65536ull;
	for(coreinstance_t* it = g_core_instances; it != NULL; it = it->next) {
		if(it->store != store_ref)
			continue;

		for(uint32_t mj = 0; mj < it->rt.num_mems; mj++) {
			if(it->rt.mems[mj].store_memaddr == store_memaddr) {
				it->rt.mems[mj].pages = new_pages;
				it->rt.mems[mj].size = nsize;
				if(mj == 0) {
					it->rt.mem_pages = new_pages;
					it->rt.mem_size = nsize;
				}
			}
		}
	}
}

/* table.grow は realloc で data バッファを移動しうるが、grower の view (g_rt->tables[ti]) しか
 * data/size を更新しない。同一 store_tableaddr を共有する他インスタンスの coretab.data が
 * 解放済み領域を指すと、call_indirect/table.get で use-after-free、teardown で旧ポインタの
 * 二重 free / 新バッファのリークになる。全 sharer の data/size を新値へ揃える (owns は据置で
 * owner が一度だけ free する不変条件を維持)。線形メモリの kw_core_sync_shared_mem_grow と対称。 */
void kw_core_sync_shared_table_grow(void* store_ref, int32_t store_tableaddr, int32_t* new_data, uint32_t new_size)
{
	if(store_tableaddr < 0)
		return;

	for(coreinstance_t* it = g_core_instances; it != NULL; it = it->next) {
		if(it->store != store_ref)
			continue;

		for(uint32_t tj = 0; tj < it->rt.num_tables; tj++) {
			if(it->rt.tables[tj].store_tableaddr == store_tableaddr) {
				it->rt.tables[tj].data = new_data;
				it->rt.tables[tj].size = new_size;
				if(tj == 0) {
					it->rt.table = new_data;
					it->rt.table_size = new_size;
				}
			}
		}
	}
}

int32_t kw_core_select_func(int32_t funcaddr, void* moduleinst)
{
	/* 所属 moduleinst (functioninstance.module) で対象 core インスタンスを一意特定する。
	 * funcaddr だけのマッチは、複数モジュールが同一 store funcaddr 空間で衝突しうる
	 * (spectest と被テストモジュールが共に funcaddr 0 を持つ等) ため使えない。 */
	for(coreinstance_t* it = g_core_instances; it != NULL; it = it->next) {
		moduleinst_t* mi = it->inst;
		if(mi == NULL || mi->funcaddrs == NULL)
			continue;

		if(moduleinst != NULL && mi != (moduleinst_t*)moduleinst)
			continue;

		/* 関数数は core モジュール it->mod から取得する (origin_module.functions は load 完了時に
		 * kw_free_module_transient で解放されるため参照しない。num_imported_funcs/num_funcs は build 時に
		 * origin_module の funcimport_count/functions.len から設定済 = 同値)。funcaddrs は moduleinst 側 (生存)。 */
		uint32_t nimp = it->mod.num_imported_funcs;
		uint32_t total = nimp + it->mod.num_funcs;
		for(uint32_t g = nimp; g < total; g++) {
			if(mi->funcaddrs[g] == (funcaddr_t)funcaddr) {
				core_activate(it);
				return (int32_t)g;
			}
		}
	}
	return -1;
}

/* free_one_core_instance — core インスタンス 1 件の per-instance 確保を解放する (リストからの unlink は
 * 呼出側が済ませておくこと)。線形メモリ base は owns のときのみ release (store 共有は owns=0 で触らない)。
 * 共有 vstack は store 単位なのでここでは解放しない。kw_core_free_store / kw_core_free_instance が共用。 */
static void free_one_core_instance(coreinstance_t* it)
{
	/* compile 出力: 各 func の bytecode (entry) と corefunc_t (mod.funcs[i].compiled)。
	 * compiled[i].entry と ((corefunc_t*)mod.funcs[i].compiled)->entry は同一なので entry は 1 度だけ free。 */
	if(it->compiled != NULL) {
		for(uint32_t i = 0; i < it->mod.num_funcs; i++)
			kinowasm_mem_free((void*)it->compiled[i].entry); /* entry は const 型だが所有権は bridge */

		kinowasm_mem_free(it->compiled);
	}
	if(it->mod.funcs != NULL) {
		for(uint32_t i = 0; i < it->mod.num_funcs; i++)
			kinowasm_mem_free(it->mod.funcs[i].compiled); /* corefunc_t struct (entry は上で解放済) */
	}
	kinowasm_mem_free(it->mod.types);
	kinowasm_mem_free(it->mod.imports);
	kinowasm_mem_free(it->mod.funcs);
	kinowasm_mem_free(it->mod.tag_typeidx);
	kinowasm_mem_free(it->rt.tagaddrs);
	for(uint32_t i = 0; i < it->rt.num_mems; i++) {
		if(it->rt.mems[i].owns)
			kw_core_mem_release(it->rt.mems[i].base); /* flat 領域は VirtualFree (owns のみ=二重free回避。store 共有は owns=0 で触らない) */
	}
	kinowasm_mem_free(it->rt.mems);
	kinowasm_mem_free(it->rt.global_addrs);
	for(uint32_t i = 0; i < it->rt.num_tables; i++) {
		if(it->rt.tables[i].owns)
			kinowasm_mem_free(it->rt.tables[i].data); /* owns のみ。共有 importer は解放しない */
	}
	kinowasm_mem_free(it->rt.tables);
	kinowasm_mem_free(it->rt.type_sigs);
	kinowasm_mem_free(it->rt.func_sigs);
	kinowasm_mem_free(it->rt.import_funcs);
	kinowasm_mem_free(it->rt.datasegs); /* bytes は store 所有なので free しない、配列のみ */
	for(uint32_t i = 0; i < it->rt.num_elemsegs; i++)
		kinowasm_mem_free((void*)it->rt.elemsegs[i].funcs);   /* funcs は bridge 確保 */

	kinowasm_mem_free(it->rt.elemsegs);
	/* rt.vstack は store 内共有なので per-instance では解放しない (store 単位で解放) */
	for(uint32_t i = 0; i < it->nexports; i++)
		kinowasm_mem_free((void*)it->exports[i].name);

	kinowasm_mem_free(it->exports);
	if(g_core_active == it) {
		g_core_active = NULL;
		g_rt = NULL;
		g_compiled = NULL;
	}
	kinowasm_mem_free(it);
}

/* 指定 store に属する全 core インスタンスを解放する。kinowasm_term から呼ばれ、テストファイル
 * ごとの store 破棄に伴って per-load の flat mem 等を回収する (蓄積による OOM 防止)。
 * 全確保が store アリーナ由来なので kinowasm_mem_free (アリーナ非依存) で返す。この store の
 * 共有 vstack もここで解放する。 */
void kw_core_free_store(store_t* S)
{
	coreinstance_t** pp = &g_core_instances;
	while(*pp != NULL) {
		coreinstance_t* it = *pp;
		if(it->store != S) {
			pp = &it->next;
			continue;
		}
		*pp = it->next; /* リストから外す */
		free_one_core_instance(it);
	}

	/* flat 線形メモリは store 所有 (kw_alloc_mem が reserve)。全 core インスタンス解放後に
	 * S->memorys を一度だけ走査して release する。import 共有でも store memory は memaddr ごとに
	 * 一意なので二重 free にならない (rt->mems は owns=0 の借用ビューで解放しない)。 */
	for(size_t _mi = 0; _mi < S->memorys.len; _mi++) {
		if(S->memorys.data[_mi].base != NULL) {
			kw_core_mem_release(S->memorys.data[_mi].base);
			S->memorys.data[_mi].base = NULL;
		}
	}

	/* この store の共有 vstack エントリを解放する (他 store のエントリには触らない)。 */
	{
		corevstack_t** vp = &g_vstacks;
		while(*vp != NULL) {
			corevstack_t* vs = *vp;
			if(vs->store != S) {
				vp = &vs->next;
				continue;
			}
			*vp = vs->next;
			kinowasm_mem_free(vs->vstack);
			kinowasm_mem_free(vs);
		}
	}
}

/* kw_core_free_instance — 指定 (store, moduleinst) の core インスタンス 1 件だけを解放する。
 * 動的モジュールの pop (rollback_modulepush) が、push で構築した core インスタンス (compiled
 * bytecode / types / rt 等) を回収するのに使う。これを呼ばないと push/pop 再利用で storememory に
 * 蓄積する。線形メモリ base は store 所有 (owns=0) なので触らず、rollback 側の free_mem_instance が
 * store->memorys を解放する (二重 free 回避)。moduleinst 本体は呼出側 (rollback) が解放する。 */
void kw_core_free_instance(store_t* S, moduleinst_t* inst)
{
	coreinstance_t** pp = &g_core_instances;
	while(*pp != NULL) {
		coreinstance_t* it = *pp;
		if(it->store == S && it->inst == inst) {
			*pp = it->next; /* リストから外す */
			/* 共有 table の owner (owns=1) を先に解放すると、同じ data を借用する残存インスタンスの
			 * coretab.data が dangling になる (現行の pop は LIFO で owner より importer が先に消える
			 * ため通常は起きないが、解放順序に依存しない防御)。残存 sharer がいれば所有権を移譲して
			 * から解放する (owner は常に 1 つ、free_one_core_instance は owns=1 のみ data を free)。 */
			for(uint32_t ti = 0; ti < it->rt.num_tables; ti++) {
				if(!it->rt.tables[ti].owns)
					continue;

				int moved = 0;
				for(coreinstance_t* other = g_core_instances; other != NULL && !moved; other = other->next) {
					if(other->store != S)
						continue;

					for(uint32_t tj = 0; tj < other->rt.num_tables && !moved; tj++) {
						if(other->rt.tables[tj].data == it->rt.tables[ti].data) {
							other->rt.tables[tj].owns = 1;
							it->rt.tables[ti].owns = 0;
							moved = 1;
						}
					}
				}
			}
			free_one_core_instance(it);
			return;
		}
		pp = &it->next;
	}
}

/* 再入判定 (kw_core_is_executing) の旧実装との差分を見る診断窓 (kw_core_exec.c)。関数の宣言は
 * ファイルスコープに置く (ブロックスコープの extern は MSVC が C4210 を出し /WX でビルドできない)。 */
int kw_core_legacy_exec_hint(void);

/* 実行コンテキスト (active + 実行中の g_rt/g_compiled) を退避/復元する。kw_core_select_func は
 * 対象を active 化するが元へ戻さないので、host 関数の中から export を呼ばれると、戻った先の実行が
 * 違うインスタンスを見てしまう。再入経路 (kinowasm.c) がこれで元に戻す。
 * active ではなく g_rt/g_compiled を直に退避するのは、cross-module 呼出 (do_call_cross) が
 * この 2 つだけを差し替えて active を据え置くため。active から復元すると cross-call 先の続きを
 * 呼出元の関数表で実行してしまう。 */
void kw_core_save_ctx(kw_core_ctx_t* ctx)
{
	ctx->active   = (void*)g_core_active;
	ctx->rt       = g_rt;
	ctx->compiled = g_compiled;
}
void kw_core_restore_ctx(const kw_core_ctx_t* ctx)
{
	g_core_active = (coreinstance_t*)ctx->active;
	g_rt          = ctx->rt;
	g_compiled    = ctx->compiled;
}

/* この vstack を共有する全インスタンスの上限を差し替える。vstack は store 内共有なので、
 * 再入実行中の cross-module call も同じ上限を見なければ誤オーバーフロー判定になる。 */
static void core_set_vstack_bound(const coreval_t* vstack, uint32_t slots)
{
	for(coreinstance_t* it = g_core_instances; it != NULL; it = it->next) {
		if(it->rt.vstack == vstack)
			it->rt.vstack_slots = slots;
	}
}

/* core 関数を引数付きで呼び、単一結果を *ret (raw r0、呼出側が型解釈) へ返す。
 * args は nargs 個の slot 値 (i64)。trap 時は 1、正常時は 0。 */
int kw_core_invoke(uint32_t func_idx, const int64_t* args, uint32_t nargs, int64_t* ret)
{
	extern uint32_t g_cur_slots;
	uint32_t nimp = g_rt->mod->num_imported_funcs;
	if(func_idx < nimp)
		return 1; /* host (import) 関数の直接 invoke は未対応 */

	uint32_t def = func_idx - nimp;
	corefunc_t* f = &g_compiled[def];

	extern int g_exc_pending;
	extern int g_caught_sp;
	extern int g_exn_sp;
	extern int g_suspended;
	extern int g_resume_n;
	extern core_resume_frame_t g_resume_chain[];
	extern int g_suspend_num_results;
	extern int g_suspend_code;
	extern int64_t g_suspend_host_r0;
	extern int32_t g_exc_tagaddr;
	extern uint32_t g_exc_nvals;
	extern coreval_t g_exc_vals[];
	extern int g_core_exec_active;

	/* ── 再入 invoke の保護 ──────────────────────────────────────────────
	 * core は本来「同時に実行中のインスタンスは 1 つ」前提の非リエントラント設計だが、
	 * host 関数の中から export を呼ばれる経路が実在する (フォーカス変化で host が
	 * SetSuspend を呼ぶ / host yield で中断中に呼ばれる)。素通しすると
	 *   - vstack を先頭から使い直し、実行中フレームのローカルを上書きする
	 *   - g_resume_n = 0 で中断側の再開チェーンを捨てる
	 *   - EH / suspend グローバルを踏み潰す
	 * のいずれかで再開後の実行が壊れる。ここでは
	 *   - vstack 末尾に固定確保した再入専用領域で実行する (主領域には触らない)
	 *   - 共有グローバルを退避して復元する
	 * の 2 段で隔離する。 */
	static core_resume_frame_t s_saved_chain[1024];
	static int s_nested_depth = 0;

	int nested = kw_core_is_executing();

	/* 診断: KW_REENTRY_LOG=1 で再入 invoke を 1 行出力する。top-level 印が付いたものは
	 * g_depth / g_resume_n がどちらも 0 のまま起きた再入 = 旧判定では見抜けなかったケース。
	 * -Og / -O2 で top-level フレームが host import を直接呼ぶ形になるとこれになり、
	 * 素通しされて vstack 先頭 (= top-level フレームのローカル) を上書きしていた。 */
	{
		static int s_reentry_log = -1;
		if(s_reentry_log < 0) {
			const char* ev = getenv("KW_REENTRY_LOG");
			s_reentry_log = (ev != NULL && *ev != '0') ? 1 : 0;
		}
		if(s_reentry_log && nested) {
			fprintf(stderr, "[core] reentrant invoke func=%u nest_depth=%d%s\n",
				func_idx, s_nested_depth,
				kw_core_legacy_exec_hint() ? "" : " (top-level frame)");
			fflush(stderr);
		}
	}

	coreval_t* vsbase = g_rt->vstack;
	coreval_t* base = vsbase;

	int saved_trapped = 0;
	const char* saved_trap_msg = NULL;
	int saved_resume_n = g_resume_n;
	int saved_suspended = g_suspended;
	int saved_num_results = g_suspend_num_results;
	int saved_suspend_code = g_suspend_code;
	int64_t saved_host_r0 = g_suspend_host_r0;
	int saved_caught_sp = g_caught_sp;
	int saved_exn_sp = g_exn_sp;
	int saved_exc_pending = g_exc_pending;
	int32_t saved_exc_tagaddr = g_exc_tagaddr;
	uint32_t saved_exc_nvals = g_exc_nvals;
	coreval_t saved_exc_vals[16];
	uint32_t saved_cur_slots = g_cur_slots;

	if(nested) {
		if(s_nested_depth > 0)
			return 1;   /* 二重の再入は未対応 (退避先が 1 段しかない) */
		if(saved_resume_n > (int)(sizeof(s_saved_chain) / sizeof(s_saved_chain[0])))
			return 1;
		if(f->num_slots > CORE_VSTACK_NESTED)
			return 1;   /* entry フレームが再入専用領域に入らない */

		s_nested_depth++;
		saved_trapped = g_rt->trapped;
		saved_trap_msg = g_rt->trap_msg;
		memcpy(saved_exc_vals, g_exc_vals, sizeof(saved_exc_vals));
		memcpy(s_saved_chain, g_resume_chain, sizeof(core_resume_frame_t) * (size_t)saved_resume_n);
		base = vsbase + CORE_VSTACK;
		core_set_vstack_bound(vsbase, CORE_VSTACK + CORE_VSTACK_NESTED);
	}

	g_rt->trapped = 0;
	g_rt->trap_msg = NULL; /* 前回 invoke の trap 種別を持ち越すと throw 系 (NULL 時のみ設定) の写像が化ける */
	/* EH / suspend 状態を top-level invoke ごとにリセット (前回 invoke の残骸を一掃)。
	 * 再入時は外側の捕捉済み台帳を残したまま上へ積む (watermark 方式なので、再入側の
	 * rethrow 深さは自分の push だけで解決する)。 */
	g_exc_pending = 0;
	if(!nested) {
		g_caught_sp = 0;
		g_exn_sp = 0;
	}
	g_suspended = 0;
	g_resume_n = 0;
	g_suspend_num_results = (int)f->num_results; /* 完走時の結果 marshalling 用 (resume が使う) */
	g_cur_slots = f->num_slots;
	for(uint32_t i = 0; i < f->num_slots; i++)
		base[i].i64 = (i < nargs) ? args[i] : 0;

	g_core_exec_active++;
	int64_t r = core_run(f->entry, base, g_rt->mem);
	g_core_exec_active--;

	if(nested) {
		/* 再入した実行が trap / yield しても、外側の実行はそのまま続行させる。特に
		 * g_rt->trapped を残すと、host 関数から戻った直後の call op の trapped チェックが
		 * 外側の実行を巻き戻してしまうので必ず元に戻す。 */
		int nested_failed = g_rt->trapped;
		core_set_vstack_bound(vsbase, CORE_VSTACK);
		g_resume_n = saved_resume_n;
		g_suspended = saved_suspended;
		g_suspend_num_results = saved_num_results;
		g_suspend_code = saved_suspend_code;
		g_suspend_host_r0 = saved_host_r0;
		g_caught_sp = saved_caught_sp;
		g_exn_sp = saved_exn_sp;
		g_exc_pending = saved_exc_pending;
		g_exc_tagaddr = saved_exc_tagaddr;
		g_exc_nvals = saved_exc_nvals;
		memcpy(g_exc_vals, saved_exc_vals, sizeof(saved_exc_vals));
		g_cur_slots = saved_cur_slots;
		g_rt->trapped = saved_trapped;
		g_rt->trap_msg = saved_trap_msg;
		memcpy(g_resume_chain, s_saved_chain, sizeof(core_resume_frame_t) * (size_t)saved_resume_n);
		s_nested_depth--;
		if(nested_failed)
			return 1;   /* 再入側の trap / yield は呼出側へ失敗として返す */
	} else if(g_rt->trapped) {
		return g_suspended ? 2 : 1;   /* 2 = host yield (suspend、再開チェーン保存済) */
	}

	/* 結果を ret[] へ。多値 (num_results>=2) は ret_multi が書いた g_core_mret から、
	 * 単一は raw r0 から取り込む。ret は num_results 個ぶんの領域を呼出側が用意する。 */
	if(ret) {
		if(f->num_results >= 2) {
			extern int64_t g_core_mret[];
			/* g_core_mret は CORE_MAX_MRET 要素。書き側 (ret_multi) も同上限でクランプするため、
			 * 読み側も揃える (num_results > 上限は公開 API が弾くが、直接呼出しへの防御)。 */
			for(uint32_t i = 0; i < f->num_results && i < CORE_MAX_MRET; i++)
				ret[i] = g_core_mret[i];
		} else if(f->num_results == 1) {
			ret[0] = r;
		}
	}
	return 0;
}
