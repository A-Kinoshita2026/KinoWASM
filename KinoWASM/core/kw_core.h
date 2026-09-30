/* kw_core.h — core (register-TOS アーキ) 実行エンジンの共通型定義。 */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ───── スロット値 (operand stack / locals / globals は全て 8byte slot) ───── */
typedef union {
	int32_t  i32;
	int64_t  i64;
	float    f32;
	double   f64;
	uint64_t u64;
} coreval_t;

/* ───── reader (compiler が関数本体の生バイトを読む) ───── */
typedef struct {
	const uint8_t* buf;
	size_t         pos;
	size_t         end;
} corereader_t;

/* ───── functype (compiler は num_params/num_results のカウントのみ参照) ───── */
typedef struct {
	uint32_t num_params;
	uint32_t num_results;
	const uint8_t* param_types; /* 引数の valtype 列 (rt1.data へのポインタ、NULL 可)。host 橋渡しの型タグ付けに使う */
} corefunctype_t;

/* ───── import ───── */
typedef struct {
	char*    module;
	char*    name;
	uint8_t  kind;     /* 0=func 1=table 2=mem 3=global */
	uint32_t type_idx; /* func: type */
	uint32_t func_idx; /* func: 連番 (imported 関数の index) */
} coreimport_t;

/* ───── 関数 (定義済み) ───── */
typedef struct func corefuncdef_t;
struct func {
	uint32_t       type_idx;
	uint32_t       num_locals; /* 宣言ローカルが占めるスロット数 (引数除く) */
	const uint8_t* code_ptr;   /* 命令本体の生バイト先頭 */
	const uint8_t* code_end;   /* 命令本体の生バイト終端 */
	void*          compiled;   /* コンパイル結果 (corefunc_t*) */
	uint32_t       num_slots;  /* この関数のスロット総数 (locals + operand stack 最大) */
};

/* ───── モジュール (bridge が KinoWASM module から必要分のみ翻訳して埋める) ───── */
typedef struct {
	uint32_t        num_types;
	corefunctype_t* types;
	uint32_t        num_imports;
	coreimport_t*   imports;
	uint32_t        num_imported_funcs;
	uint32_t        num_funcs;
	corefuncdef_t*  funcs;
	uint32_t        num_tags;
	uint32_t*       tag_typeidx; /* EH: tagidx → functype index (param 数算出用) */
} coremodule_t;

/* ───────────────────────── 実行コア (register-TOS アーキ) ───────────────────────── */
/* bytecode word: direct-threaded。handler fn-ptr を bytecode に直埋め (テーブル無)。
 * 各 op は [fn][operand words...] の列。1 word = 8 byte。 */
typedef union coreinstr coreinstr;
/* handler ABI: 全 hot state をレジスタ引数で渡す (Win64 RCX/RDX/R8/R9)。
 *   pc  = 自分の最初の operand word を指す
 *   sp  = 現フレームの slot 基底 (locals + operand scratch、coreval_t 配列)
 *   mem = 線形メモリ base
 *   r0  = register-TOS (i64。float は bit 再解釈)
 * 戻り値 = 関数の結果 (END/return が plain return で musttail 連鎖を巻き戻す)。 */
typedef int64_t (*coreop_t)(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0);
union coreinstr {
	coreop_t op;
	uint32_t u32;
	int32_t  i32;
	int64_t  i64;
	uint64_t u64;
	double   f64;
	const coreinstr* tgt; /* 解決済み分岐先 (label stack 無し) */
	const void* ptr;
};

/* 保証付き tail-call dispatch。clang は __attribute__((musttail))、MSVC は 14.50+ (_MSC_VER>=1950,
 * 例: VS18 の 19.51) で [[msvc::musttail]] を /O2 限定でサポート。clang-cl は __clang__ と _MSC_VER を
 * 両方定義するため clang 分岐を先に置く。いずれも無い場合は空 (NEXT 連鎖が実呼び出しになり
 * スタックを溢れさせるため、その構成では core エンジンを使わない — CMake で gate)。 */
#if defined(__clang__)
#define CORE_MUSTTAIL __attribute__((musttail))
#elif defined(_MSC_VER) && _MSC_VER >= 1950
#define CORE_MUSTTAIL [[msvc::musttail]]
#else
#define CORE_MUSTTAIL
#endif

/* コールドヘルパの inline 抑止。ハンドラへ inline 展開されるとホット .text が膨張し、
 * 実行されないコードでも配置/I-cache が動いて CoreMark が数%落ちる (実測)。 */
#if defined(_MSC_VER) && !defined(__clang__)
#define CORE_NOINLINE __declspec(noinline)
#else
#define CORE_NOINLINE __attribute__((noinline))
#endif

/* ランタイムインスタンス (cold state、single-thread 前提でグローバル)。
 * hot op (算術/load/store) は触らず、call/global/table/grow のみ参照。 */
typedef struct {
	coremodule_t* mod;
	uint8_t*  mem;      /* 線形メモリ base */
	uint64_t  mem_size; /* 現在のバイト数 */
	uint64_t  mem_cap;  /* 事前確保バイト数 (grow はこの範囲内) */
	uint32_t  mem_pages;
	uint32_t  mem_max_pages;
	/* cross-module linking: global は store の実体 (S->globals[addr]) を index 経由で共有する
	 * (store 配列は module 追加で realloc しうるので pointer 保持は不可)。global.get/set は
	 * store_ref + global_addrs[idx] で実体スロットを得る。own/imported とも live 共有、testsuite (get) とも一致。 */
	void*     store_ref;
	int32_t*  global_addrs;
	uint32_t  num_global_addrs;
	int32_t*  table;      /* call_indirect 用: funcidx を格納 (-1 = null)。tables[0] のミラー (高速路) */
	uint32_t  table_size;
	uint32_t  table_max;  /* table.grow の上限 (宣言 max、無宣言は実用上限) */
	/* multi-table: 宣言された全テーブル。tableidx=0 は上の table/table_size を使う高速路、tableidx>0 は
	 * ここ (tables[tableidx]) を参照。tables[0] も table 0 をミラーし cold op が使う。
	 * global_ref=1: cross-module 共有テーブル。要素は store funcaddr (gfi でなく) を格納し、
	 *   call_indirect は funcaddr→定義インスタンスを解決して呼ぶ (別モジュールが書いた要素も解決可)。
	 *   非共有 (単一モジュール、CoreMark 等) は global_ref=0 で従来の gfi 直呼び (高速)。
	 * owns=1: data を当インスタンスが確保 (free_store で解放)。共有 importer は owns=0 (二重free回避)。 */
	/* is_64=1: table64 (idx operand が i64)。call_indirect は compile が idx64_chk を前置し、
	 * cold table op は実行時にこのフラグで idx/len の切詰め有無を分岐する。 */
	struct coretab {
		int32_t* data;
		uint32_t size;
		uint32_t max;
		uint8_t  global_ref;
		uint8_t  owns;
		uint8_t  is_externref;
		uint8_t  is_64;
		int32_t  store_tableaddr;
	}* tables;
	uint32_t  num_tables;
	/* call_indirect 型チェック用シグネチャ (functype の構造的ハッシュ)。type_sigs[typeidx] (期待型) と
	 * func_sigs[global funcidx] (table 要素の実際型) を比較。一致しなければ trap。 */
	uint64_t* type_sigs;
	uint32_t  num_type_sigs;
	uint64_t* func_sigs;
	uint32_t  num_func_sigs;
	/* cross-module linking: imported func (idx < num_imported_funcs) の解決先。target!=NULL なら
	 * 別 core インスタンスの関数 (do_call が instance 切替で呼ぶ)、NULL なら host (WASI)。 */
	struct coreimpfn {
		void*    target;
		uint32_t target_fidx;
	}* import_funcs;
	uint32_t  num_import_funcs;
	/* passive data segment (memory.init/data.drop 用)。bytes は store の datainstance を指す
	 * (実行中は生存)。dropped で data.drop 済みを表す。 */
	struct {
		const uint8_t* bytes;
		uint32_t       len;
		uint8_t        dropped;
	}* datasegs;
	uint32_t  num_datasegs;
	/* passive elem segment (table.init/elem.drop 用)。funcs は core global func index へ翻訳済み
	 * (-1 = null)。store 由来でなく bridge が確保するので free_store で解放する。 */
	struct {
		const int32_t* funcs;
		uint32_t       len;
		uint8_t        dropped;
	}* elemsegs;
	uint32_t  num_elemsegs;
	/* multi-memory: 宣言された全メモリ。memidx=0 は上の mem/mem_size を使う高速路、memidx>0 は
	 * ここ (mems[memidx]) を参照。mems[0] も memory 0 をミラーし cold op (size/grow/fill/copy/init) が使う。
	 */
	/* owns=1: この base は当インスタンスが確保 (free_store で解放)。owns=0: imported memory で
	 * 定義側インスタンスの base を共有 (二重 free 回避)。store_memaddr=store の memoryinstance index
	 * (memory.grow が num_pages を store へ伝播し、後続モジュールの import 検証を通すため)。 */
	/* is_64=1: memory64 (アドレス operand が i64)。load/store は compile が memN 経路へ落とし、
	 * memN / cold op が実行時にこのフラグでアドレスの u32 切詰め有無を分岐する (memory32 の
	 * hot path は不変)。実メモリ cap は memory64 でも 4GB clamp (kw_store_alloc)。 */
	struct coremem {
		uint8_t* base;
		uint64_t size;
		uint64_t cap;
		uint32_t pages;
		uint8_t  owns;
		uint8_t  is_64;
		int32_t  store_memaddr;
	}* mems;
	uint32_t  num_mems;
	coreval_t* vstack;       /* 値スタック (call ごとに slot window を進める) */
	uint32_t  vstack_slots;
	int       trapped;
	const char* trap_msg;
	/* tables[0].global_ref のミラー (高速路 call_indirect 用)。ホット field のレイアウトを崩さない
	 * よう構造体末尾に置く (cross-module 共有テーブルのみ 1。CoreMark 等の非共有は 0)。 */
	uint8_t   table_global_ref;
	/* EH: local tagidx → global tagaddr (cross-module tag identity)。throw/catch の tag 照合に使う。 */
	int32_t*  tagaddrs;
	uint32_t  num_tagaddrs;
	/* funcref 値変換用: 対応 moduleinst_t (gfi ↔ store funcaddr の相互変換。cold: table ops のみ参照)。
	 * kw_core.h を store 型へ依存させないため void* で保持 (bridge が設定、exec がキャスト)。 */
	void*     inst_ref;
} corert_t;

extern corert_t* g_rt;

/* 各定義済み関数の compile 結果。corefuncdef_t.compiled が corefunc_t* を指す。 */
typedef struct {
	const coreinstr* entry; /* bytecode 先頭 */
	uint32_t  num_slots;  /* フレームの slot 総数 */
	uint32_t  num_params;
	uint32_t  num_results;
	uint32_t  code_words; /* bytecode word 数 (デバッグ: pc→func 特定用) */
	uint32_t  func_idx;   /* デバッグ用 */
} corefunc_t;

/* core_resume_frame_t — host yield で中断した 1 フレームの再開点。
 * kw_core_exec.c が g_resume_chain に deepest-first で積み、kw_core_resume が再開する。
 * kw_core_bridge.c の再入 invoke 保護からも参照するのでここで公開する。
 * rt/compiled はこのフレームを実行していたインスタンス。cross-module 呼出 (do_call_cross) の
 * 途中で中断すると、C スタックの巻き戻しで g_rt/g_compiled が呼出元へ戻るため、フレーム単位で
 * 控えて resume 時に復元しないと、callee 側フレームを呼出元の関数表・線形メモリで再開してしまう
 * (単一モジュールでは全フレーム同値なので従来と同じ)。 */
typedef struct {
	const coreinstr* pc;
	coreval_t*       sp;
	const coreinstr* eh_dispatch;
	int              saved_caught;
	corert_t*        rt;       /* このフレームの g_rt */
	corefunc_t*      compiled; /* このフレームの g_compiled */
} core_resume_frame_t;

/* 多値結果バッファ g_core_mret のスロット数。関数の結果数 (num_results) はこれ以下でなければ
 * ならない (書き側 ret_multi/mret_get / 読み側 invoke/resume の marshalling がこの上限を共有)。
 * 公開 API (kinowasm_invoke) は rt2.len > CORE_MAX_MRET を弾く。 */
#define CORE_MAX_MRET 64

/* ───── API ───── */
void*       core_alloc(size_t n);
void        core_fatal(const char* msg);
/* ───── 線形メモリ確保バックエンド (プラットフォーム注入) ─────
 * WASM 線形メモリ (flat mem) のみ「予約 (reserve: アドレス空間確保・物理未使用) + コミット
 * (commit: 使用ページのみ物理確保)」で確保する。ライブラリは VirtualAlloc 等を直接呼ばず、
 * ホストが初期化時に注入する関数ポインタ (kw_core_mem_backend_t) 経由で呼ぶ。これにより
 * ライブラリにプラットフォーム依存コードを持たず、移植時はバックエンドを差し替えるだけで済む
 * (Windows=VirtualAlloc、POSIX/iOS=mmap+mprotect 等。WASI FS の差し替えと同方式)。 */
typedef struct {
	void* (*reserve)(uint64_t reserve_bytes, uint64_t commit_bytes); /* base 返却 (失敗 NULL)。commit_bytes 分は確保済で返す。 */
	int   (*commit)(void* base, uint64_t offset, uint64_t len); /* 追加コミット。成功 0 / 失敗 -1。 */
	void  (*release)(void* base); /* 解放。 */
} kw_core_mem_backend_t;
void     kw_core_set_mem_backend(const kw_core_mem_backend_t* backend);
/* 上記バックエンドを呼ぶライブラリ内ラッパ。bridge/exec が flat mem 確保・grow・解放で使う。 */
uint8_t* kw_core_mem_reserve(uint64_t reserve_bytes, uint64_t commit_bytes);
int      kw_core_mem_commit(uint8_t* base, uint64_t offset, uint64_t len);
void     kw_core_mem_release(uint8_t* base);
corefunctype_t* core_func_type(coremodule_t* m, uint32_t func_idx);

/* exec: 関数を slot window sp で完走させ結果を返す (単一結果は r0、0 結果は 0)。 */
int64_t  core_run(const coreinstr* entry, coreval_t* sp, uint8_t* mem);
void     core_trap(const char* msg);

/* compile: 定義済み関数 func_idx を register-TOS direct-threaded bytecode へコンパイル。 */
void     core_compile_func(coremodule_t* m, uint32_t def_func_idx);

/* WASI host 関数呼出 (imported func index で分岐)。args は呼び元 slot、結果を r0 で返す。 */
int64_t  core_call_host(uint32_t import_func_idx, coreval_t* args, uint8_t* mem);

/* export 名 → global func index (-1=無)。active インスタンスの表を引く。 */
int32_t  kw_core_lookup_export(const char* name);
/* store funcaddr から所属インスタンスを特定し active 化、その core global func index を返す
 * (-1=無)。複数モジュール対応。 */
int32_t  kw_core_select_func(int32_t funcaddr, void* store);
/* kw_core_ctx_t — 再入 invoke が退避する実行コンテキスト。active インスタンス (bridge の登録上の
 * 「現在」) と、実際に実行中の (g_rt, g_compiled) ペアを一組で持つ。cross-module 呼出
 * (do_call_cross) は g_rt / g_compiled だけを差し替えて active を据え置くため、active だけでは
 * 実行中インスタンスを表せない (それで復元すると、cross-call 先の続きを呼出元の関数表で実行して
 * しまう)。三つ組で退避・復元して初めて外側の実行が元の状態で継続できる。 */
typedef struct {
	void*       active;   /* g_core_active (coreinstance_t*) */
	corert_t*   rt;       /* g_rt */
	corefunc_t* compiled; /* g_compiled */
} kw_core_ctx_t;
/* 再入 invoke 用。kw_core_select_func は対象を active 化するが元へ戻さないので、host 関数の中から
 * export を呼ぶ経路では呼出側が明示的に復元する。 */
void     kw_core_save_ctx(kw_core_ctx_t* ctx);
void     kw_core_restore_ctx(const kw_core_ctx_t* ctx);
/* core 実行中 (host 関数の中 / host yield で中断中) か。再入 invoke の判定に使う。 */
int      kw_core_is_executing(void);
/* cross-module linking: import_funcs[].target (coreinstance) から、その rt と compiled 配列を取り出す
 * (do_call が instance 切替で別モジュールの関数を呼ぶため)。 */
void     kw_core_import_target(void* target, corert_t** out_rt, corefunc_t** out_compiled);
/* 共有メモリ grow を同一 store_memaddr の全 core インスタンスへ伝播 (pages/size を新サイズへ)。 */
void     kw_core_sync_shared_mem_grow(void* store_ref, int32_t store_memaddr, uint32_t new_pages);
/* 共有テーブル grow を同一 store_tableaddr の全 core インスタンスへ伝播 (data ポインタ/size を新値へ)。
 * realloc で data が移動するため、ポインタ非同期による UAF / 二重 free / リークを防ぐ。 */
void     kw_core_sync_shared_table_grow(void* store_ref, int32_t store_tableaddr, int32_t* new_data, uint32_t new_size);
/* core 関数を引数付きで呼び単一結果を *ret (raw r0) へ。正常 0 / trap 1 / host yield (suspend) 2。
 * active インスタンス (kw_core_select_func で選択済) に対して実行する。suspend 時は再開チェーンを
 * 保存し、kw_core_resume で継続する。 */
int      kw_core_invoke(uint32_t func_idx, const int64_t* args, uint32_t nargs, int64_t* ret);
/* suspend 済 core 実行を再開する。完走 0 (+ret に最終結果) / resume 中 trap 1 / 再 yield 2。 */
int      kw_core_resume(int64_t* ret);
/* host yield の伝播 code (例: ERR_NEXTFRAME_YIELD)。kw_core_invoke/_resume が 2 を返したとき有効。 */
int      kw_core_suspend_code(void);
