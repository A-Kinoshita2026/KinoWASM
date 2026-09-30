#pragma once

#include "KinoUtil/karray.h"
#include "operand.h"
#include <assert.h>

/* blocktype_t — block/if/loop の結果型 (blocktype) を表す共用体。
 * typeidx 形式 (関数型インデックス) と単一 valtype 形式の両方を保持する。 */
typedef union {
	uint32_t typeidx; /* 型インデックス形式の blocktype (func type を参照) */
	uint8_t  valtype; /* 単一値型形式の blocktype (空 or 1 個の valtype) */
} blocktype_t;

/* const_t — *.const 命令の即値を型別に格納する共用体。 */
typedef union {
	int32_t i32; /* i32.const の即値 */
	int64_t i64; /* i64.const の即値 */
	float f32;   /* f32.const の即値 */
	double f64;  /* f64.const の即値 */
} const_t;

/* WASM 3.0 multi-memory: memarg のエンコーディング上 align field の bit 6 が
 * 立っていれば memidx が続く。parser で分離し、ここでは 8byte 維持のため
 * align (実値は 0..15 程度で 8bit で十分) と memidx (multi-memory 用) を
 * 16bit ずつにパックする。 */
/* memarg_t — load/store 命令のメモリ引数 (align + offset + multi-memory の memidx)。 */
typedef struct {
	uint16_t align;  /* アラインメントヒント (実値 0..15 程度) */
	uint16_t memidx; /* 対象メモリのインデックス (multi-memory 用、既定 0) */
	uint32_t offset; /* ベースアドレスへ加算する静的オフセット */
} memarg_t;

/* v128.load{N}_lane / store{N}_lane の即値 (memarg + 1 byte lane idx)。
 * instr_ex の 8 byte union には収まらないため、kinowasm_mem_malloc で
 * 確保した実体へのポインタを instr_ex->v128_ptr に保持する。 */
/* simd_lane_imm_t — v128.load{N}_lane / store{N}_lane の即値 (memarg + lane 番号)。 */
typedef struct {
	memarg_t memory; /* メモリ引数 (align/memidx/offset) */
	uint32_t lane_idx; /* 読み書き対象のレーン番号 */
} simd_lane_imm_t;

typedef kinowasm_array(uint32_t) u32karray_t;
typedef kinowasm_array(uint8_t) u8karray_t;

/* instr_t — デコード後内部命令フォーマット (型検証 + const 式評価で使用)。
 * opcode と最大 3 つのレジスタ (スロット) オペランドを 8 byte に収める。 */
typedef struct instr {
	uint16_t opcode; /* 内部オペコード (パーサが割り当てた命令種別) */
	uint16_t r0; /* 結果(書き込み先)レジスタ/スロット番号 */
	uint16_t r1; /* ソースオペランド 1 のスロット番号 */
	uint16_t r2; /* ソースオペランド 2 のスロット番号 */
} instr_t;

/* instr_ex_t — instr_t と対になる拡張即値スロット。命令ごとに異なる即値を
 * 1 個の 8 byte union で表現する (命令種別によりどのメンバを使うか決まる)。 */
typedef struct instr_ex {
	union {
		/* 2 個の即値を要する命令用 (例: call_indirect の typeidx/tableidx、
		 * select 型付き、table/elem 2 引数 bulk op 等)。 */
		struct {
			uint32_t y;	/* 第 2 即値 */
			uint32_t x;	/* 第 1 即値 */
		};
		u32karray_t* labels; /* br_table の分岐先ラベル配列 */
		memarg_t memory; /* load/store 系のメモリ引数 */
		const_t c; /* *.const の即値 */
		uint32_t val; /* 単一即値 (local/global idx, func idx, 各種 N 等) */
		blocktype_t blocktype; /* block/loop/if の結果型 */
		/* v128.const 即値ポインタ。parser 時に kinowasm_mem_malloc(16) で
		 * 確保した kinowasm_v128_t を指す。pool 化しないことで func/init expr
		 * 双方を同一 op で扱える。 */
		void* v128_ptr;
	};
} instr_ex_t;
