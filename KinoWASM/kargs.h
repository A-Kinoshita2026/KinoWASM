#pragma once

#include "KinoUtil/karray.h"

/* kinowasm_valtype_t — WASM 値型。値は binary spec の type byte と一致する。 */
typedef enum {
	TYPE_VAL_I32 = 0x7F,
	TYPE_VAL_I64 = 0x7E,
	TYPE_VAL_F32 = 0x7D,
	TYPE_VAL_F64 = 0x7C,
	TYPE_VAL_V128 = 0x7B, /* SIMD 128-bit vector (sidecar 領域に格納) */
	TYPE_FUNCREF = 0x70,
	TYPE_EXTERNREF = 0x6F,
	TYPE_EXNREF = 0x69, /* WASM 3.0 EH: exception reference (host-opaque) */
} kinowasm_valtype_t;

/* kinowasm_ref_t — 参照型の値。funcaddr 等のインデックスを格納し、-1 (REF_NULL) で null。 */
typedef int32_t kinowasm_ref_t;

/* 参照 null 値 (REF_NULL) と externval の種別。REF_NULL は kinowasm_ref_t の null。
 * EXTERN_* は export/import descriptor の種別を表す。 */
typedef enum {
	REF_NULL = -1,
	EXTERN_FUNC = 0,
	EXTERN_TABLE= 1,
	EXTERN_MEM = 2,
	EXTERN_GLOBAL = 3,
} kinowasm_extern_val_t;

/* 128-bit SIMD vector value。WASM SIMD は little-endian 固定で、
 * 各 lane shape (i8x16 / i16x8 / i32x4 / i64x2 / f32x4 / f64x2) を
 * 同じ 16 byte 領域として扱う。
 * kinowasm_val_t 本体は 8 byte に保ち、v128 値は frame の sidecar
 * (kinowasm_v128_t* v128_locals) に格納する。スカラ性能を維持するための設計。 */
typedef union {
	uint8_t  u8[16];
	int8_t   i8[16];
	uint16_t u16[8];
	int16_t  i16[8];
	uint32_t u32[4];
	int32_t  i32[4];
	uint64_t u64[2];
	int64_t  i64[2];
	float    f32[4];
	double   f64[2];
} kinowasm_v128_t;

/* kinowasm_num_t — 数値 (i32/i64/f32/f64) を表す 8 byte union。同一ビット列を型ごとに解釈する。 */
typedef union {
	int32_t i32; /* i32 として解釈 */
	int64_t i64; /* i64 として解釈 */
	float f32;  /* f32 として解釈 */
	double f64; /* f64 として解釈 */
} kinowasm_num_t;

/* kinowasm_val_t — 値スタック/ローカルの 1 スロット値 (8 byte)。数値または参照を保持する。
 * v128 はここに収まらず frame の sidecar (kinowasm_v128_t* v128_locals) 側に格納する。 */
typedef union {
	kinowasm_num_t num; /* 数値 (i32/i64/f32/f64) */
	kinowasm_ref_t ref; /* 参照 (funcref/externref/exnref) */
} kinowasm_val_t;

/* kinowasm_arg_t — 型タグ付きの値。ホスト⇔wasm 間の引数/戻り値の受け渡しに使う。 */
typedef struct {
	uint8_t type; /* 値型 (kinowasm_valtype_t の値) */
	kinowasm_val_t val;	/* 値本体 */
} kinowasm_arg_t;

/* kinowasm_args_t — kinowasm_arg_t の可変長配列 (invoke の引数列 / 戻り値列)。 */
typedef kinowasm_array(kinowasm_arg_t) kinowasm_args_t;
