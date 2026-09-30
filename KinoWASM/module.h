#pragma once

#include "instr.h"
#include "KinoUtil/karray.h"
#include "KinoUtil/kalloc.h"
#include "kargs.h"

/* スタックサイズ */
#define PARSER_STACK_SIZE kinowasm_settings.default_parser_stack_size
/* デコード済み命令 1 個の最大サイズ。命令本体 (instr_t) + 即値スロット (instr_ex_t)。
 * ブロック構造 (旧 instr_block_t) は core が raw bytes から再構築するため保持しない。 */
#define CODE_SIZE         (sizeof(instr_t))
#define CODE_WITH_EX_SIZE (CODE_SIZE + sizeof(instr_ex_t))

#if defined(__clang__) || defined(__GNUC__)
/* Clang, GCC用 */
#define UNREACHABLE() __builtin_unreachable()
#define UNUSE [[maybe_unused]]
#elif defined(_MSC_VER)
/* MSVC専用 */
#define UNREACHABLE() __assume(0)
#define UNUSE
#else
/* その他（標準的なC99） */
#include <assert.h>
#define UNREACHABLE() assert(0)
#define UNUSE
#endif

typedef enum {
	IMPORTDESC_FUNC = 0,
	IMPORTDESC_TABLE  = 1,
	IMPORTDESC_MEMORY = 2,
	IMPORTDESC_GLOBAL = 3,
	IMPORTDESC_TAG = 4,    /* WASM 3.0 exception handling */
	KIND_ACTIVE = 0,
	KIND_PASSIVE = 1,
	KIND_DECLARATIVE = 2,
	DATA_MODE_PASSIVE = 0,
	DATA_MODE_ACTIVE = 1,
} importdesc_kind_t;

typedef u8karray_t  string_t;
typedef u8karray_t  resulttype_t;
typedef instr_t*    code_t;
typedef instr_ex_t* code_ex_t;

/* kinowasm_settings_t — ランタイム全体の動作を決める起動時設定。 */
typedef struct {
	uint32_t default_param_stack_size;   /* 実行時オペランドスタックのバイトサイズ */
	uint32_t default_parser_stack_size; /* デコード時に使う型スタック等のバイトサイズ */
	uint32_t default_object_cache_size;  /* オブジェクトキャッシュの初期サイズ */
	void*    extra_func_table;           /* ホスト関数 (WASI 等) のテーブルへのポインタ */
} kinowasm_settings_t;

extern kinowasm_settings_t kinowasm_settings;

/* limits_t — memory / table の容量制限 (limits)。WASM 仕様の resizable limits。
 * memorytype_t としても使われ、メモリ型 = limits そのものを表す。 */
typedef struct {
	uint64_t min;      /* 最小サイズ (memory は page 数 / table は要素数)。WASM 3.0 memory64: i64 アドレス対応のため u64 化 */
	uint64_t max;      /* 最大サイズ。has_max が 0 のときは無効 */
	uint8_t has_max;   /* max が指定されているか (limits flag bit 0) */
	uint8_t is_64;     /* 64bit アドレス空間か。WASM 3.0 memory64: limits flag bit 2 */
	uint8_t shared;    /* 共有メモリ (threads) か。WASM threads: limits flag bit 1 */
} limits_t;
typedef limits_t memorytype_t;

/* tabletype_t — table 型。要素の参照型と容量制限を保持する。 */
typedef struct {
	uint8_t reftype;   /* 要素の参照型 (funcref / externref) */
	limits_t limits;   /* 要素数の最小/最大制限 */
} tabletype_t;

/* functiontype_t — 関数型 (関数シグネチャ)。引数列と戻り値列の result type。 */
typedef struct {
	resulttype_t rt1;  /* 引数の result type (パラメータの valtype 列) */
	resulttype_t rt2;  /* 戻り値の result type (結果の valtype 列) */
} functiontype_t;

/* function_t — 定義済み関数 1 件。型インデックス・デコード済みコード・ローカル情報を保持する
 * (import 関数は含まず、code section で本体を持つ関数のみ)。 */
typedef struct {
	uint32_t typeidx;          /* この関数の型インデックス (mod->types への参照) */
	code_t body;               /* デコード済み命令列の先頭 */
	u8karray_t localvalues;    /* ローカル変数の valtype 列 (引数を除く宣言ローカル) */
	/* localvalues は WASM-level local 1 件につき 1 entry (元の type byte)。
	 * v128 は 2 internal slot を占有するため、ランタイムの result_base 計算では
	 * local_slot_count (= 内部スロット総数) を使う。 */
	uint32_t local_slot_count; /* 引数を除くローカルが占める内部スロット総数 (v128 は 2 換算) */
} function_t;

/* datamode_t — data segment の配置モード。active なら配置先メモリと offset 式を持つ。 */
typedef struct {
	uint8_t kind;      /* DATA_MODE_ACTIVE / DATA_MODE_PASSIVE */
	uint32_t memory;   /* 配置先メモリインデックス (active 時、multi-memory 対応) */
	code_t offset;     /* 配置先オフセットを与える定数式 (active 時のみ有効) */
} datamode_t;

/* data_t — data section の 1 セグメント。初期化バイト列と配置モードを保持する。 */
typedef struct {
	datamode_t mode;   /* active / passive と配置先情報 */
	u8karray_t init;   /* メモリへ書き込む初期化バイト列 */
} data_t;

/* globaltype_t — global 変数の型。値型と可変性を保持する。 */
typedef struct {
	uint8_t valtype;   /* 値型 (i32/i64/f32/f64/v128/参照型) */
	uint8_t mut;       /* mutability (0 = const / 1 = var) */
} globaltype_t;

/* tagtype_t — exception tag の型。
 * WASM 3.0 Exception Handling: tag は exception type を関数型 (params のみ、results 空)
 * で記述する。attribute は将来の拡張用 (現状 0 = exception)。 */
typedef struct {
	uint8_t  attribute; /* tag 種別 (現状 0 = exception) */
	uint32_t typeidx;   /* 例外パラメータを記述する関数型インデックス */
} tagtype_t;

/* importdesc_t — import の対象記述。kind に応じて union のいずれか 1 つが有効。 */
typedef struct {
	uint8_t kind;            /* IMPORTDESC_FUNC/TABLE/MEMORY/GLOBAL/TAG のいずれか */
	union {
		uint32_t functypeidx; /* kind=FUNC: 関数型インデックス */
		tabletype_t table;    /* kind=TABLE: table 型 */
		memorytype_t memory;  /* kind=MEMORY: memory 型 */
		globaltype_t globaltype; /* kind=GLOBAL: global 型 */
		tagtype_t tag;  /* WASM 3.0 exception handling: tag import */
	};
} importdesc_t;

/* import_t — import section の 1 エントリ。モジュール名・要素名と対象記述。 */
typedef struct {
	string_t module;   /* import 元モジュール名 */
	string_t name;     /* import する要素名 */
	importdesc_t d;    /* import 対象の種別と型情報 */
} import_t;

/* memory_t — memory section で定義されるメモリ 1 件。 */
typedef struct {
	memorytype_t memtype;  /* メモリ型 (limits) */
} memory_t;

/* table_t — table section で定義されるテーブル 1 件。 */
typedef struct {
	tabletype_t tabletype; /* table 型 (reftype + limits) */
} table_t;

/* global_t — global section で定義されるグローバル変数 1 件。型と初期化式を保持する。 */
typedef struct {
	globaltype_t globaltype; /* 値型と可変性 */
	code_t body;             /* 初期値を与える定数式 (デコード済み) */
} global_t;

/* exportdesc_t — export の対象記述。kind が示す名前空間内のインデックスを指す。 */
typedef struct {
	uint8_t kind;      /* 対象種別 (func/table/memory/global)。importdesc_kind_t に準拠 */
	uint32_t idx;      /* 対象のインデックス (import を含む通し番号) */
} exportdesc_t;

/* export_t — export section の 1 エントリ。公開名と対象記述。 */
typedef struct {
	string_t name;          /* 外部へ公開する名前 */
	exportdesc_t exportdesc; /* 公開する対象の種別とインデックス */
} export_t;

/* elementmode_t — element segment の配置モード。active なら対象 table と offset 式を持つ。 */
typedef struct {
	uint8_t kind;      /* KIND_ACTIVE / KIND_PASSIVE / KIND_DECLARATIVE */
	uint32_t table;    /* 配置先 table インデックス (active 時) */
	code_t offset;     /* 配置先オフセットを与える定数式 (active 時のみ有効) */
} elementmode_t;

/* element_t — element section の 1 セグメント。table を初期化する参照要素の集合。 */
typedef struct {
	uint8_t type;      /* 要素の参照型 (funcref / externref)。table.init パース時 (kw_parser_ops_ext.inc) の table reftype 一致検証でも参照される */
	elementmode_t mode; /* active / passive / declarative と配置先情報 */
	kinowasm_array(code_t) init; /* 各要素の初期値を与える定数式の配列 */
} element_t;

/* tag_t — tag section で定義される exception tag 1 件。 */
typedef struct {
	tagtype_t tagtype; /* tag の型 (attribute + 関数型インデックス) */
} tag_t;

/* module_t — WASM バイナリをデコードした結果のモジュール全体。
 * 各 section の内部表現 (型/関数/データ/import/table/memory/export/global/element/tag)
 * と、import 数や start 関数などのメタ情報を保持する。 */
typedef struct {
	kinowasm_array(functiontype_t) types;    /* type section: 関数型の一覧 */
	kinowasm_array(function_t) functions;    /* function + code section: 定義関数の一覧 (import 関数は含まない) */
	kinowasm_array(data_t) datas;            /* data section: データセグメント一覧 */
	kinowasm_array(import_t) imports;        /* import section: import エントリ一覧 */
	kinowasm_array(table_t) tables;          /* table section: 定義テーブル一覧 (import 分は含まない) */
	kinowasm_array(memory_t) memorys;        /* memory section: 定義メモリ一覧 (import 分は含まない) */
	kinowasm_array(export_t) exports;        /* export section: export エントリ一覧 */
	kinowasm_array(global_t) globals;        /* global section: 定義グローバル一覧 (import 分は含まない) */
	kinowasm_array(element_t) elements;      /* element section: エレメントセグメント一覧 */
	kinowasm_array(tag_t) tags;              /* tag section: 定義 tag 一覧 (import 分は含まない) */
	u8karray_t declared_funcs;               /* declarative element で宣言済みの funcidx ビットマップ (遅延初期化) */
	uint32_t startidx;                       /* start 関数の funcidx (has_start が 1 のとき有効) */
	uint32_t funcimport_count;               /* import された関数の数 (functions より前に並ぶ通し番号の境界) */
	uint32_t tableimport_count;              /* import されたテーブルの数 */
	uint32_t memoryimport_count;             /* import されたメモリの数 */
	uint32_t globalimport_count;             /* import されたグローバルの数 (globals より前に並ぶ境界) */
	uint32_t tagimport_count;                /* import された tag の数 */
	uint8_t  has_start;                      /* start section が存在し startidx が有効か */
} module_t;
