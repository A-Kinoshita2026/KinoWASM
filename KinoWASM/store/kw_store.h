#pragma once

#include "module.h"
#include "KinoUtil/kstack.h"
#include "kargs.h"
#include "KinoUtil/exception.h"
#include "exceptioncode.h"

#define OBJECT_CACHE_SIZE kinowasm_settings.default_object_cache_size

typedef enum {
	/* 外部関数ID始点 */
	EXTRA_FUNC_ID = 0x100000,
	/* ページサイズ */
	BASE_PAGE_SIZE = 4096,
	WASM_PAGE_SIZE = BASE_PAGE_SIZE * 16,
} kinowasm_runtime_const_t;

typedef enum {
	STATE_FLAG_SUSPENDED = 0x02,
} store_state_flag_t;

typedef int32_t funcaddr_t;
typedef int32_t tableaddr_t;
typedef int32_t memaddr_t;
typedef int32_t globaladdr_t;
typedef int32_t elemaddr_t;
typedef int32_t dataaddr_t;
typedef int32_t tagaddr_t; /* WASM 3.0 EH: tag instance address (cross-module identity) */

typedef uint64_t eaddr_t;
typedef uint64_t paddr_t;

/* externval_t — import/export で受け渡す外部値。kind が示す種別に応じて
 * store 内インスタンスへの addr (index) を共用体で 1 つだけ保持する。 */
typedef struct {
	uint8_t kind; /* 外部値の種別 (func/table/mem/global/tag) */
	union {       /* kind に対応する store 内アドレス (index) を 1 つだけ使用 */
		funcaddr_t func;   /* 関数インスタンスの addr */
		tableaddr_t table; /* テーブルインスタンスの addr */
		memaddr_t mem;     /* メモリインスタンスの addr */
		globaladdr_t global; /* グローバルインスタンスの addr */
		tagaddr_t tag;     /* WASM 3.0 EH: cross-module tag identity */
	};
} externval_t;
typedef kinowasm_array(externval_t)  externvals_t;

/* exportinst_t — モジュールが公開する 1 つの export。export 名と、その実体を
 * 指す externval_t を組にして保持する。 */
typedef struct {
	string_t    name;  /* export 名 (embedded NUL を含みうる) */
	externval_t value; /* export が指す外部値 (func/table/mem/global/tag) */
} exportinst_t;
typedef kinowasm_array(exportinst_t) exports_t;

/* moduleinst_t — インスタンス化済みモジュール。各 *addrs 配列はモジュール内の
 * local index (typeidx/funcidx/...) を store 内グローバル addr へ写像する索引表で、
 * これにより module ローカル参照を store の実インスタンスへ解決する。 */
typedef struct {
	functiontype_t* types;     /* typeidx → 関数型 (検証/呼出時の型情報) */
	funcaddr_t* funcaddrs;     /* funcidx → store 内 関数インスタンス addr */
	tableaddr_t* tableaddrs;   /* tableidx → store 内 テーブル addr */
	memaddr_t* memaddrs;       /* memidx → store 内 メモリ addr */
	globaladdr_t* globaladdrs; /* globalidx → store 内 グローバル addr */
	elemaddr_t* elemaddrs;     /* elemidx → store 内 要素セグメント addr */
	dataaddr_t* dataaddrs;     /* dataidx → store 内 データセグメント addr */
	tagaddr_t* tagaddrs;       /* WASM 3.0 EH: local tagidx → global tag instance */
	exports_t exports;         /* このモジュールが公開する export 一覧 */
	module_t origin_module;    /* インスタンス化元の静的モジュール定義 */
} moduleinst_t;

/* localpool_t — 関数フレーム用の locals 領域を再利用するためのプール要素。
 * 同サイズの空きプールを単方向リストで繋ぎ、フレーム生成時に貸し出し、
 * 関数終了時に返却することでアロケーションを抑える。 */
typedef struct localpool {
	struct localpool* next; /* 同プールリスト内の次の空き要素 (末尾は NULL) */
	size_t local_size;      /* この locals 領域の容量 (slot 数) */
	kinowasm_val_t* locals; /* params + locals を格納する slot 配列 */
} localpool_t;

/* frame_t — 関数呼び出し 1 回分の実行フレーム。呼び出し元へ戻るための情報
 * (parent ip・caller_locals・r0)、自身の locals・module 解決情報を保持する。
 * framestack_t により push/pop される。 */
typedef struct {
	code_t parent; /* 呼出元の継続 ip (return 先)。最上位フレームは NULL */
	uint32_t arity; /* 戻り値の個数 (functype rt2.len) */
	funcaddr_t root_funcaddr; /* このフレームが実行中の関数 addr (-1 = 未割当) */
	localpool_t* localpool; /* 借用中の locals プール (終了時に返却) */
	moduleinst_t* module; /* 実行中関数の所属モジュールインスタンス */
	funcaddr_t* funcaddrs; /* module->funcaddrs のキャッシュ (CALL 高速化用) */
	kinowasm_val_t* caller_locals; /* 呼出元の locals (戻り値の書き戻し先となる register 群) */
	uint16_t r0; /* 呼出元で戻り値を受ける先頭 register 番号 */
	uint32_t result_base; /* locals 内で戻り値が始まる slot 位置 (= param_count + local_slot_count) */
} frame_t;

/* framestack_t — コールスタック。フレームポインタの配列 + トップ index で管理する
 * (frame_idx はトップ位置、-1 で空)。 */
typedef struct {
	kinowasm_array(frame_t*) frames; /* コールスタック (フレームポインタの配列) */
	int64_t frame_idx; /* frames のトップ位置 (-1 で空) */
} framestack_t;

/* functioninstance_t — store 内の関数インスタンス。型・所属モジュール・本体コードを
 * 束ねる。WASM 関数なら code が本体を指し、ホスト関数は別途扱われる。 */
typedef struct {
	functiontype_t* type; /* この関数のシグネチャ (params/results) */
	moduleinst_t* module; /* 関数が属するモジュールインスタンス (index 解決用) */
	function_t* code;     /* 関数本体 (デコード済み命令列・locals 情報) */
} functioninstance_t;

/* tableinstance_t — store 内のテーブルインスタンス。要素型と limits を持つ type と、
 * 参照値 (funcref/externref) の動的配列 elem からなる。 */
typedef struct {
	tabletype_t type; /* 要素型と limits (min/max, is_64) */
	kinowasm_array(kinowasm_ref_t) elem; /* テーブル要素 (参照値) の配列 */
} tableinstance_t;

/* memoryinstance_t — store 内の線形メモリインスタンス。実体は OS 仮想メモリの flat バッファ
 * (reserve cap / commit num_pages) で、core 実行エンジンと同一バッファを共有する
 * (kw_alloc_mem が確保し core の rt->mems[].base がそのまま参照、build 時コピー無し)。
 * grow は base を追加 commit する (base は安定)。 */
typedef struct {
	memorytype_t type; /* メモリ型 (limits min/max, is_64) */
	size_t num_pages;  /* 現在 commit 済みの WASM ページ数 (論理サイズ) */
	uint8_t* base;     /* flat 線形メモリ先頭 (NULL=未確保)。core と共有 */
	uint64_t cap;      /* reserve 済みバイト数 (memory.grow の commit 上限) */
} memoryinstance_t;

/* globalinstance_t — store 内のグローバル変数インスタンス。型 gt と現在値を保持し、
 * スカラ/v128 を union で同居させる。 */
typedef struct {
	globaltype_t gt; /* グローバル型 (値型・mutability) */
	/* スカラ global は val (8 byte) を、v128 global は val_v128 (16 byte)
	 * を使う。union で先頭を共有することで val 経由のスカラアクセスを維持。 */
	union {
		kinowasm_val_t val; /* スカラ値 (i32/i64/f32/f64/ref) */
		kinowasm_v128_t val_v128; /* SIMD v128 値 (16 byte) */
	};
} globalinstance_t;

/* elementinstance_t — store 内の要素セグメントインスタンス。table 初期化や
 * table.init/elem.drop の対象となる参照値列を保持する (drop 済みは空)。 */
typedef struct {
	uint8_t type; /* 要素の参照型 (funcref/externref 等) */
	kinowasm_array(kinowasm_ref_t) elem; /* 参照値の配列 (elem.drop で空になる) */
} elementinstance_t;

/* datainstance_t — store 内のデータセグメントインスタンス。memory 初期化や
 * memory.init/data.drop の対象となるバイト列を保持する (drop 済みは空)。 */
typedef struct {
	u8karray_t data; /* セグメントのバイト列 (data.drop で空になる) */
} datainstance_t;

/* moduletable_t — 名前付きでロード済みモジュールを登録するエントリ。名前引きで
 * モジュールインスタンスを解決するための索引で、外部供給されたメモリ領域も保持する。 */
typedef struct {
	string_t name; /* 登録モジュール名 (import/invoke の名前引きキー) */
	moduleinst_t* module; /* 対応するモジュールインスタンス */
	kinowasm_mem_info_t memory; /* このモジュール用に外部から渡されたメモリ領域情報 */
} moduletable_t;

/* store_t — ランタイム全体の状態を集約する最上位構造体 (WASM spec の "store")。
 * 全インスタンス配列 (addr で参照される実体) ・実行スタック・各種プールを一括して
 * 保持する。インスタンス配列の index がそのまま funcaddr_t などの addr 値となる。 */
typedef struct {
	kinowasm_array(functioninstance_t) funcs;   /* 関数インスタンス配列 (funcaddr = index) */
	kinowasm_array(tableinstance_t) tables;     /* テーブルインスタンス配列 (tableaddr = index) */
	kinowasm_array(memoryinstance_t) memorys;   /* メモリインスタンス配列 (memaddr = index) */
	kinowasm_array(globalinstance_t) globals;   /* グローバルインスタンス配列 (globaladdr = index) */
	kinowasm_array(elementinstance_t) elements; /* 要素セグメント配列 (elemaddr = index) */
	kinowasm_array(datainstance_t) datas;       /* データセグメント配列 (dataaddr = index) */
	kinowasm_array(moduletable_t) moduletable;  /* 名前付きロード済みモジュールの登録表 */
	/* 仕様準拠: instantiation が trap しても、それまでに active segment が import 済み
	 * table/memory へ書き込んだ funcref 等は有効であり続ける必要がある (spec store は
	 * monotonic)。よって失敗時も moduleinst/確保物は巻き戻さず、未登録 moduleinst を
	 * ここに積んで teardown でまとめて解放する。 */
	kinowasm_array(moduleinst_t*) orphan_moduleinsts; /* 失敗インスタンス化で確保済みの未登録 moduleinst */
	kinowasm_array(functiontype_t*) extra_func_type;  /* ホスト/外部関数の関数型表 (EXTRA_FUNC_ID 起点) */
	kinowasm_array(frame_t*) framepool; /* 再利用するフレームオブジェクトのプール */
	framestack_t* stack; /* 実行時コールスタック */
	localpool_t localpool; /* locals プールリストの先頭 (size 0 兼ヘッド) */
	kinowasm_mem_info_t storememory; /* store 自身が使う外部供給メモリ領域 */
	uint32_t state_flags; /* 実行状態フラグ (store_state_flag_t の OR) */
	/* WASM 3.0 EH: 各 module の local tag に振る global tagaddr の monotonic counter。
	 * モジュール間で同じ counter を共有することで cross-module identity を担保する。 */
	tagaddr_t next_tagaddr;    /* 次に払い出す global tagaddr (単調増加カウンタ) */
} store_t;
