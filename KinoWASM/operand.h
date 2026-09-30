#pragma once

#include "features.h"

typedef enum {
/* ── 制御フロー ── */
OP_UNREACHABLE = 0x00,
OP_NOP = 0x01,
OP_BLOCK = 0x02,
OP_LOOP = 0x03,
OP_IF = 0x04,
OP_ELSE = 0x05,
/* WASM 3.0 Exception Handling (legacy form): try/catch/throw/rethrow/delegate/catch_all */
OP_TRY = 0x06,
OP_CATCH = 0x07,
OP_THROW = 0x08,
OP_RETHROW = 0x09,
OP_THROW_REF = 0x0A,  /* WASM 3.0 new exception spec */
OP_END = 0x0B,
OP_BR = 0x0C,
OP_BR_IF = 0x0D,
OP_BR_TABLE = 0x0E,
OP_RETURN = 0x0F,
OP_CALL = 0x10,
OP_CALL_INDIRECT = 0x11,
/* WASM 3.0 tail call: フレーム再利用で深い再帰の stack overflow を回避 */
OP_RETURN_CALL = 0x12,
OP_RETURN_CALL_INDIRECT = 0x13,
OP_DELEGATE = 0x18,
OP_CATCH_ALL = 0x19,
/* ── スタック操作 ── */
OP_DROP = 0x1A,
OP_SELECT = 0x1B,
OP_SELECT_T = 0x1C,
/* WASM 3.0 Exception Handling (new spec): try_table 統合形式 */
OP_TRY_TABLE = 0x1F,
/* ── ローカル / グローバル / テーブル ── */
OP_LOCAL_GET = 0x20,
OP_LOCAL_SET = 0x21,
OP_LOCAL_TEE = 0x22,
OP_GLOBAL_GET = 0x23,
OP_GLOBAL_SET = 0x24,
OP_TABLE_GET = 0x25,
OP_TABLE_SET = 0x26,
/* ── ロード ── */
OP_I32_LOAD = 0x28,
OP_I64_LOAD = 0x29,
OP_F32_LOAD = 0x2A,
OP_F64_LOAD = 0x2B,
OP_I32_LOAD8_S = 0x2C,
OP_I32_LOAD8_U = 0x2D,
OP_I32_LOAD16_S = 0x2E,
OP_I32_LOAD16_U = 0x2F,
OP_I64_LOAD8_S = 0x30,
OP_I64_LOAD8_U = 0x31,
OP_I64_LOAD16_S = 0x32,
OP_I64_LOAD16_U = 0x33,
OP_I64_LOAD32_S = 0x34,
OP_I64_LOAD32_U = 0x35,
/* ── ストア ── */
OP_I32_STORE = 0x36,
OP_I64_STORE = 0x37,
OP_F32_STORE = 0x38,
OP_F64_STORE = 0x39,
OP_I32_STORE8 = 0x3A,
OP_I32_STORE16 = 0x3B,
OP_I64_STORE8 = 0x3C,
OP_I64_STORE16 = 0x3D,
OP_I64_STORE32 = 0x3E,
/* ── メモリ ── */
OP_MEMORY_SIZE = 0x3F,
OP_MEMORY_GROW = 0x40,
/* ── 定数 ── */
OP_I32_CONST = 0x41,
OP_I64_CONST = 0x42,
OP_F32_CONST = 0x43,
OP_F64_CONST = 0x44,
/* ── I32 比較 ── */
OP_I32_EQZ = 0x45,
OP_I32_EQ = 0x46,
OP_I32_NE = 0x47,
OP_I32_LT_S = 0x48,
OP_I32_LT_U = 0x49,
OP_I32_GT_S = 0x4A,
OP_I32_GT_U = 0x4B,
OP_I32_LE_S = 0x4C,
OP_I32_LE_U = 0x4D,
OP_I32_GE_S = 0x4E,
OP_I32_GE_U = 0x4F,
/* ── I64 比較 ── */
OP_I64_EQZ = 0x50,
OP_I64_EQ = 0x51,
OP_I64_NE = 0x52,
OP_I64_LT_S = 0x53,
OP_I64_LT_U = 0x54,
OP_I64_GT_S = 0x55,
OP_I64_GT_U = 0x56,
OP_I64_LE_S = 0x57,
OP_I64_LE_U = 0x58,
OP_I64_GE_S = 0x59,
OP_I64_GE_U = 0x5A,
/* ── F32 比較 ── */
OP_F32_EQ = 0x5B,
OP_F32_NE = 0x5C,
OP_F32_LT = 0x5D,
OP_F32_GT = 0x5E,
OP_F32_LE = 0x5F,
OP_F32_GE = 0x60,
/* ── F64 比較 ── */
OP_F64_EQ = 0x61,
OP_F64_NE = 0x62,
OP_F64_LT = 0x63,
OP_F64_GT = 0x64,
OP_F64_LE = 0x65,
OP_F64_GE = 0x66,
/* ── I32 算術 ── */
OP_I32_CLZ = 0x67,
OP_I32_CTZ = 0x68,
OP_I32_POPCNT = 0x69,
OP_I32_ADD = 0x6A,
OP_I32_SUB = 0x6B,
OP_I32_MUL = 0x6C,
OP_I32_DIV_S = 0x6D,
OP_I32_DIV_U = 0x6E,
OP_I32_REM_S = 0x6f,
OP_I32_REM_U = 0x70,
OP_I32_AND = 0x71,
OP_I32_OR = 0x72,
OP_I32_XOR = 0x73,
OP_I32_SHL = 0x74,
OP_I32_SHR_S = 0x75,
OP_I32_SHR_U = 0x76,
OP_I32_ROTL = 0x77,
OP_I32_ROTR = 0x78,
/* ── I64 算術 ── */
OP_I64_CLZ = 0x79,
OP_I64_CTZ = 0x7A,
OP_I64_POPCNT = 0x7B,
OP_I64_ADD = 0x7C,
OP_I64_SUB = 0x7D,
OP_I64_MUL = 0x7E,
OP_I64_DIV_S = 0x7F,
OP_I64_DIV_U = 0x80,
OP_I64_REM_S = 0x81,
OP_I64_REM_U = 0x82,
OP_I64_AND = 0x83,
OP_I64_OR = 0x84,
OP_I64_XOR = 0x85,
OP_I64_SHL = 0x86,
OP_I64_SHR_S = 0x87,
OP_I64_SHR_U = 0x88,
OP_I64_ROTL = 0x89,
OP_I64_ROTR = 0x8A,
/* ── F32 演算 ── */
OP_F32_ABS = 0x8B,
OP_F32_NEG = 0x8C,
OP_F32_CEIL = 0x8D,
OP_F32_FLOOR = 0x8E,
OP_F32_TRUNC = 0x8F,
OP_F32_NEAREST = 0x90,
OP_F32_SQRT = 0x91,
OP_F32_ADD = 0x92,
OP_F32_SUB = 0x93,
OP_F32_MUL = 0x94,
OP_F32_DIV = 0x95,
OP_F32_MIN = 0x96,
OP_F32_MAX = 0x97,
OP_F32_COPYSIGN = 0x98,
/* ── F64 演算 ── */
OP_F64_ABS = 0x99,
OP_F64_NEG = 0x9A,
OP_F64_CEIL = 0x9B,
OP_F64_FLOOR = 0x9C,
OP_F64_TRUNC = 0x9D,
OP_F64_NEAREST = 0x9E,
OP_F64_SQRT = 0x9F,
OP_F64_ADD = 0xA0,
OP_F64_SUB = 0xA1,
OP_F64_MUL = 0xA2,
OP_F64_DIV = 0xA3,
OP_F64_MIN = 0xA4,
OP_F64_MAX = 0xA5,
OP_F64_COPYSIGN = 0xA6,
/* ── 型変換 ── */
OP_I32_WRAP_I64 = 0xA7,
OP_I32_TRUNC_F32_S = 0xA8,
OP_I32_TRUNC_F32_U = 0xA9,
OP_I32_TRUNC_F64_S = 0xAA,
OP_I32_TRUNC_F64_U = 0xAB,
OP_I64_EXTEND_I32_S = 0xAC,
OP_I64_EXTEND_I32_U = 0xAD,
OP_I64_TRUNC_F32_S = 0xAE,
OP_I64_TRUNC_F32_U = 0xAF,
OP_I64_TRUNC_F64_S = 0xB0,
OP_I64_TRUNC_F64_U = 0xB1,
OP_F32_CONVERT_I32_S = 0xB2,
OP_F32_CONVERT_I32_U = 0xB3,
OP_F32_CONVERT_I64_S = 0xB4,
OP_F32_CONVERT_I64_U = 0xB5,
OP_F32_DEMOTE_F64 = 0xB6,
OP_F64_CONVERT_I32_S = 0xB7,
OP_F64_CONVERT_I32_U = 0xB8,
OP_F64_CONVERT_I64_S = 0xB9,
OP_F64_CONVERT_I64_U = 0xBA,
OP_F64_PROMOTE_F32 = 0xBB,
OP_I32_REINTERPRET_F32 = 0xBC,
OP_I64_REINTERPRET_F64 = 0xBD,
OP_F32_REINTERPRET_I32 = 0xBE,
OP_F64_REINTERPRET_I64 = 0xBF,
/* ── 符号拡張 ── */
OP_I32_EXTEND8_S = 0xC0,
OP_I32_EXTEND16_S = 0xC1,
OP_I64_EXTEND8_S = 0xC2,
OP_I64_EXTEND16_S = 0xC3,
OP_I64_EXTEND32_S = 0xC4,
/* ── 参照型 ── */
OP_REF_NULL = 0xD0,
OP_REF_IS_NULL = 0xD1,
OP_REF_FUNC = 0xD2,
OP_0XFC = 0xFC,
OP_0XFD = 0xFD,   /* SIMD prefix (followed by LEB128 sub-opcode) */
OP_0XFE = 0xFE,   /* WASM 3.0 threads/atomics prefix */
/* ── 拡張命令 ── */
OP_COPY = 0x106,
/* v128 専用 2-slot コピー: locals[r0..r0+1] = locals[r1..r1+1] (16 byte) */
OP_COPY_V128 = 0x108,
/* v128 global.get/set: 16 byte をグローバル領域 (val_v128) と locals 間で転送 */
OP_GLOBAL_GET_V128 = 0x109,
OP_GLOBAL_SET_V128 = 0x10A,
/* v128 select: r0 = (cond != 0) ? r1 : r2 (cond は instr_ex->val のレジスタ番号) */
OP_SELECT_V128 = 0x10B,
/* WASM 3.0 Memory64: memory64 module 上の load/store/bulk は parser で以下の
	* 専用 opcode に rewrite される。
	* 元の load/store opcode は ipex->memory.align (16bit) に退避する。 */
OP_MEM64_LOAD = 0x180,
OP_MEM64_STORE = 0x181,
OP_MEM64_BULK = 0x182,
/* WASM 3.0 Exception Handling (legacy try-delegate): parser が OP_TRY を patch して
	* OP_TRY_DELEGATE に書き換える。labelidx は r1 (low 16) + r2 (high 16) に格納。 */
OP_TRY_DELEGATE = 0x184,
OP_0XFC_I32_TRUNC_SAT_F32_S = 0x200,
OP_0XFC_I32_TRUNC_SAT_F32_U = 0x201,
OP_0XFC_I32_TRUNC_SAT_F64_S = 0x202,
OP_0XFC_I32_TRUNC_SAT_F64_U = 0x203,
OP_0XFC_I64_TRUNC_SAT_F32_S = 0x204,
OP_0XFC_I64_TRUNC_SAT_F32_U = 0x205,
OP_0XFC_I64_TRUNC_SAT_F64_S = 0x206,
OP_0XFC_I64_TRUNC_SAT_F64_U = 0x207,
OP_0XFC_MEMORY_INIT = 0x208,
OP_0XFC_DATA_DROP = 0x209,
OP_0XFC_MEMORY_COPY = 0x20A,
OP_0XFC_MEMORY_FILL = 0x20B,
OP_0XFC_TABLE_INIT = 0x20C,
OP_0XFC_ELEM_DROP = 0x20D,
OP_0XFC_TABLE_COPY = 0x20E,
OP_0XFC_TABLE_GROW = 0x20F,
OP_0XFC_TABLE_SIZE = 0x210,
OP_0XFC_TABLE_FILL = 0x211,
/* Wide arithmetic proposal (sub-op 0x13〜0x16 → 0x213〜0x216) */
OP_0XFC_I64_ADD128 = 0x213,
OP_0XFC_I64_SUB128 = 0x214,
OP_0XFC_I64_MUL_WIDE_S = 0x215,
OP_0XFC_I64_MUL_WIDE_U = 0x216,

/* ── SIMD: 0xFD prefix + sub-opcode (synthetic value = 0x300 + sub_op) ──
 * 全 SIMD opcode は uint16_t synthetic 値で dispatch する。 */
OP_0XFD_V128_LOAD            = 0x300,  /* sub=0x00 */
/* load extend: 8 byte をメモリから読み 16 byte v128 に拡張 (8x8 / 16x4 / 32x2 の s/u) */
OP_0XFD_V128_LOAD8X8_S       = 0x301,  /* sub=0x01 */
OP_0XFD_V128_LOAD8X8_U       = 0x302,  /* sub=0x02 */
OP_0XFD_V128_LOAD16X4_S      = 0x303,  /* sub=0x03 */
OP_0XFD_V128_LOAD16X4_U      = 0x304,  /* sub=0x04 */
OP_0XFD_V128_LOAD32X2_S      = 0x305,  /* sub=0x05 */
OP_0XFD_V128_LOAD32X2_U      = 0x306,  /* sub=0x06 */
/* load splat: 1 lane 分を読み全 lane に broadcast */
OP_0XFD_V128_LOAD8_SPLAT     = 0x307,  /* sub=0x07 */
OP_0XFD_V128_LOAD16_SPLAT    = 0x308,
OP_0XFD_V128_LOAD32_SPLAT    = 0x309,
OP_0XFD_V128_LOAD64_SPLAT    = 0x30A,
OP_0XFD_V128_STORE           = 0x30B,  /* sub=0x0B */
OP_0XFD_V128_CONST           = 0x30C,  /* sub=0x0C */
/* load/store lane: 既存 v128 の指定 lane だけメモリと交換 */
OP_0XFD_V128_LOAD8_LANE      = 0x354,  /* sub=0x54 */
OP_0XFD_V128_LOAD16_LANE     = 0x355,
OP_0XFD_V128_LOAD32_LANE     = 0x356,
OP_0XFD_V128_LOAD64_LANE     = 0x357,
OP_0XFD_V128_STORE8_LANE     = 0x358,
OP_0XFD_V128_STORE16_LANE    = 0x359,
OP_0XFD_V128_STORE32_LANE    = 0x35A,
OP_0XFD_V128_STORE64_LANE    = 0x35B,
/* load zero: N byte 読んで上位 zero */
OP_0XFD_V128_LOAD32_ZERO     = 0x35C,  /* sub=0x5C */
OP_0XFD_V128_LOAD64_ZERO     = 0x35D,
OP_0XFD_I8X16_SHUFFLE        = 0x30D,  /* sub=0x0D (16 byte lane indices) */
OP_0XFD_I8X16_SWIZZLE        = 0x30E,  /* sub=0x0E */
/* splat: scalar -> v128 */
OP_0XFD_I8X16_SPLAT          = 0x30F,  /* sub=0x0F */
OP_0XFD_I16X8_SPLAT          = 0x310,  /* sub=0x10 */
OP_0XFD_I32X4_SPLAT          = 0x311,  /* sub=0x11 */
OP_0XFD_I64X2_SPLAT          = 0x312,  /* sub=0x12 */
OP_0XFD_F32X4_SPLAT          = 0x313,  /* sub=0x13 */
OP_0XFD_F64X2_SPLAT          = 0x314,  /* sub=0x14 */
/* extract_lane / replace_lane (lane idx は 1 byte 即値) */
OP_0XFD_I8X16_EXTRACT_LANE_S = 0x315,  /* sub=0x15 */
OP_0XFD_I8X16_EXTRACT_LANE_U = 0x316,  /* sub=0x16 */
OP_0XFD_I8X16_REPLACE_LANE   = 0x317,  /* sub=0x17 */
OP_0XFD_I16X8_EXTRACT_LANE_S = 0x318,  /* sub=0x18 */
OP_0XFD_I16X8_EXTRACT_LANE_U = 0x319,  /* sub=0x19 */
OP_0XFD_I16X8_REPLACE_LANE   = 0x31A,  /* sub=0x1A */
OP_0XFD_I32X4_EXTRACT_LANE   = 0x31B,  /* sub=0x1B */
OP_0XFD_I32X4_REPLACE_LANE   = 0x31C,  /* sub=0x1C */
OP_0XFD_I64X2_EXTRACT_LANE   = 0x31D,  /* sub=0x1D */
OP_0XFD_I64X2_REPLACE_LANE   = 0x31E,  /* sub=0x1E */
OP_0XFD_F32X4_EXTRACT_LANE   = 0x31F,  /* sub=0x1F */
OP_0XFD_F32X4_REPLACE_LANE   = 0x320,  /* sub=0x20 */
OP_0XFD_F64X2_EXTRACT_LANE   = 0x321,  /* sub=0x21 */
OP_0XFD_F64X2_REPLACE_LANE   = 0x322,  /* sub=0x22 */
/* integer / float comparison (lane mask 結果: 真 = 全 bit 1, 偽 = 0) */
OP_0XFD_I8X16_EQ             = 0x323,  /* sub=0x23 */
OP_0XFD_I8X16_NE             = 0x324,
OP_0XFD_I8X16_LT_S           = 0x325,
OP_0XFD_I8X16_LT_U           = 0x326,
OP_0XFD_I8X16_GT_S           = 0x327,
OP_0XFD_I8X16_GT_U           = 0x328,
OP_0XFD_I8X16_LE_S           = 0x329,
OP_0XFD_I8X16_LE_U           = 0x32A,
OP_0XFD_I8X16_GE_S           = 0x32B,
OP_0XFD_I8X16_GE_U           = 0x32C,
OP_0XFD_I16X8_EQ             = 0x32D,
OP_0XFD_I16X8_NE             = 0x32E,
OP_0XFD_I16X8_LT_S           = 0x32F,
OP_0XFD_I16X8_LT_U           = 0x330,
OP_0XFD_I16X8_GT_S           = 0x331,
OP_0XFD_I16X8_GT_U           = 0x332,
OP_0XFD_I16X8_LE_S           = 0x333,
OP_0XFD_I16X8_LE_U           = 0x334,
OP_0XFD_I16X8_GE_S           = 0x335,
OP_0XFD_I16X8_GE_U           = 0x336,
OP_0XFD_I32X4_EQ             = 0x337,
OP_0XFD_I32X4_NE             = 0x338,
OP_0XFD_I32X4_LT_S           = 0x339,
OP_0XFD_I32X4_LT_U           = 0x33A,
OP_0XFD_I32X4_GT_S           = 0x33B,
OP_0XFD_I32X4_GT_U           = 0x33C,
OP_0XFD_I32X4_LE_S           = 0x33D,
OP_0XFD_I32X4_LE_U           = 0x33E,
OP_0XFD_I32X4_GE_S           = 0x33F,
OP_0XFD_I32X4_GE_U           = 0x340,
OP_0XFD_F32X4_EQ             = 0x341,
OP_0XFD_F32X4_NE             = 0x342,
OP_0XFD_F32X4_LT             = 0x343,
OP_0XFD_F32X4_GT             = 0x344,
OP_0XFD_F32X4_LE             = 0x345,
OP_0XFD_F32X4_GE             = 0x346,
OP_0XFD_F64X2_EQ             = 0x347,
OP_0XFD_F64X2_NE             = 0x348,
OP_0XFD_F64X2_LT             = 0x349,
OP_0XFD_F64X2_GT             = 0x34A,
OP_0XFD_F64X2_LE             = 0x34B,
OP_0XFD_F64X2_GE             = 0x34C,
OP_0XFD_I64X2_EQ             = 0x3D6,  /* sub=0xD6 */
OP_0XFD_I64X2_NE             = 0x3D7,
OP_0XFD_I64X2_LT_S           = 0x3D8,
OP_0XFD_I64X2_GT_S           = 0x3D9,
OP_0XFD_I64X2_LE_S           = 0x3DA,
OP_0XFD_I64X2_GE_S           = 0x3DB,
/* bitwise (v128 全 16 byte 単位) */
OP_0XFD_V128_NOT             = 0x34D,  /* sub=0x4D */
OP_0XFD_V128_AND             = 0x34E,  /* sub=0x4E */
OP_0XFD_V128_ANDNOT          = 0x34F,  /* sub=0x4F */
OP_0XFD_V128_OR              = 0x350,  /* sub=0x50 */
OP_0XFD_V128_XOR             = 0x351,  /* sub=0x51 */
OP_0XFD_V128_BITSELECT       = 0x352,  /* sub=0x52 */
/* any_true / all_true / bitmask */
OP_0XFD_V128_ANY_TRUE        = 0x353,  /* sub=0x53 */
OP_0XFD_I8X16_ALL_TRUE       = 0x363,  /* sub=0x63 */
OP_0XFD_I8X16_BITMASK        = 0x364,  /* sub=0x64 */
OP_0XFD_I16X8_ALL_TRUE       = 0x383,  /* sub=0x83 */
OP_0XFD_I16X8_BITMASK        = 0x384,  /* sub=0x84 */
OP_0XFD_I32X4_ALL_TRUE       = 0x3A3,  /* sub=0xA3 */
OP_0XFD_I32X4_BITMASK        = 0x3A4,  /* sub=0xA4 */
OP_0XFD_I64X2_ALL_TRUE       = 0x3C3,  /* sub=0xC3 */
OP_0XFD_I64X2_BITMASK        = 0x3C4,  /* sub=0xC4 */
/* float promote/demote (32 <-> 64) */
OP_0XFD_F32X4_DEMOTE_F64X2_ZERO   = 0x35E,  /* sub=0x5E */
OP_0XFD_F64X2_PROMOTE_LOW_F32X4   = 0x35F,  /* sub=0x5F */
/* float rounding (f32x4 / f64x2): ceil/floor/trunc/nearest */
OP_0XFD_F32X4_CEIL                = 0x367,  /* sub=0x67 */
OP_0XFD_F32X4_FLOOR               = 0x368,
OP_0XFD_F32X4_TRUNC               = 0x369,
OP_0XFD_F32X4_NEAREST             = 0x36A,
OP_0XFD_F64X2_CEIL                = 0x374,  /* sub=0x74 */
OP_0XFD_F64X2_FLOOR               = 0x375,
OP_0XFD_F64X2_TRUNC               = 0x37A,  /* sub=0x7A */
OP_0XFD_F64X2_NEAREST             = 0x394,  /* sub=0x94 */
/* integer/float conversions (trunc_sat / convert) */
OP_0XFD_I32X4_TRUNC_SAT_F32X4_S       = 0x3F8,  /* sub=0xF8 */
OP_0XFD_I32X4_TRUNC_SAT_F32X4_U       = 0x3F9,
OP_0XFD_F32X4_CONVERT_I32X4_S         = 0x3FA,
OP_0XFD_F32X4_CONVERT_I32X4_U         = 0x3FB,
OP_0XFD_I32X4_TRUNC_SAT_F64X2_S_ZERO  = 0x3FC,
OP_0XFD_I32X4_TRUNC_SAT_F64X2_U_ZERO  = 0x3FD,
OP_0XFD_F64X2_CONVERT_LOW_I32X4_S     = 0x3FE,
OP_0XFD_F64X2_CONVERT_LOW_I32X4_U     = 0x3FF,
/* narrow (saturating): 2 v128 (低位/高位) -> v128 (狭い lane に詰める) */
OP_0XFD_I8X16_NARROW_I16X8_S = 0x365,  /* sub=0x65 */
OP_0XFD_I8X16_NARROW_I16X8_U = 0x366,  /* sub=0x66 */
OP_0XFD_I16X8_NARROW_I32X4_S = 0x385,  /* sub=0x85 */
OP_0XFD_I16X8_NARROW_I32X4_U = 0x386,  /* sub=0x86 */
/* extend: v128 (8 -> 16, 16 -> 32, 32 -> 64) signed/unsigned, low/high half */
OP_0XFD_I16X8_EXTEND_LOW_I8X16_S  = 0x387,
OP_0XFD_I16X8_EXTEND_HIGH_I8X16_S = 0x388,
OP_0XFD_I16X8_EXTEND_LOW_I8X16_U  = 0x389,
OP_0XFD_I16X8_EXTEND_HIGH_I8X16_U = 0x38A,
OP_0XFD_I32X4_EXTEND_LOW_I16X8_S  = 0x3A7,
OP_0XFD_I32X4_EXTEND_HIGH_I16X8_S = 0x3A8,
OP_0XFD_I32X4_EXTEND_LOW_I16X8_U  = 0x3A9,
OP_0XFD_I32X4_EXTEND_HIGH_I16X8_U = 0x3AA,
OP_0XFD_I64X2_EXTEND_LOW_I32X4_S  = 0x3C7,
OP_0XFD_I64X2_EXTEND_HIGH_I32X4_S = 0x3C8,
OP_0XFD_I64X2_EXTEND_LOW_I32X4_U  = 0x3C9,
OP_0XFD_I64X2_EXTEND_HIGH_I32X4_U = 0x3CA,
/* integer shifts (v128 lane, i32 count → v128) */
OP_0XFD_I8X16_SHL            = 0x36B,  /* sub=0x6B */
OP_0XFD_I8X16_SHR_S          = 0x36C,  /* sub=0x6C */
OP_0XFD_I8X16_SHR_U          = 0x36D,  /* sub=0x6D */
OP_0XFD_I16X8_SHL            = 0x38B,  /* sub=0x8B */
OP_0XFD_I16X8_SHR_S          = 0x38C,  /* sub=0x8C */
OP_0XFD_I16X8_SHR_U          = 0x38D,  /* sub=0x8D */
OP_0XFD_I32X4_SHL            = 0x3AB,  /* sub=0xAB */
OP_0XFD_I32X4_SHR_S          = 0x3AC,  /* sub=0xAC */
OP_0XFD_I32X4_SHR_U          = 0x3AD,  /* sub=0xAD */
OP_0XFD_I64X2_SHL            = 0x3CB,  /* sub=0xCB */
OP_0XFD_I64X2_SHR_S          = 0x3CC,  /* sub=0xCC */
OP_0XFD_I64X2_SHR_U          = 0x3CD,  /* sub=0xCD */
/* integer arithmetic (per-lane add/sub/mul/neg/abs/min/max/avgr_u) */
OP_0XFD_I8X16_ABS            = 0x360,  /* sub=0x60 */
OP_0XFD_I8X16_NEG            = 0x361,  /* sub=0x61 */
OP_0XFD_I8X16_POPCNT         = 0x362,  /* sub=0x62 */
OP_0XFD_I8X16_ADD            = 0x36E,  /* sub=0x6E */
OP_0XFD_I8X16_SUB            = 0x371,  /* sub=0x71 */
OP_0XFD_I8X16_MIN_S          = 0x376,  /* sub=0x76 */
OP_0XFD_I8X16_MIN_U          = 0x377,  /* sub=0x77 */
OP_0XFD_I8X16_MAX_S          = 0x378,  /* sub=0x78 */
OP_0XFD_I8X16_MAX_U          = 0x379,  /* sub=0x79 */
OP_0XFD_I8X16_AVGR_U         = 0x37B,  /* sub=0x7B */
OP_0XFD_I16X8_ABS            = 0x380,  /* sub=0x80 */
OP_0XFD_I16X8_NEG            = 0x381,  /* sub=0x81 */
OP_0XFD_I16X8_ADD            = 0x38E,  /* sub=0x8E */
OP_0XFD_I16X8_SUB            = 0x391,  /* sub=0x91 */
OP_0XFD_I16X8_MUL            = 0x395,  /* sub=0x95 */
OP_0XFD_I16X8_MIN_S          = 0x396,  /* sub=0x96 */
OP_0XFD_I16X8_MIN_U          = 0x397,  /* sub=0x97 */
OP_0XFD_I16X8_MAX_S          = 0x398,  /* sub=0x98 */
OP_0XFD_I16X8_MAX_U          = 0x399,  /* sub=0x99 */
OP_0XFD_I16X8_AVGR_U         = 0x39B,  /* sub=0x9B */
OP_0XFD_I32X4_ABS            = 0x3A0,  /* sub=0xA0 */
OP_0XFD_I32X4_NEG            = 0x3A1,  /* sub=0xA1 */
OP_0XFD_I32X4_ADD            = 0x3AE,  /* sub=0xAE */
OP_0XFD_I32X4_SUB            = 0x3B1,  /* sub=0xB1 */
OP_0XFD_I32X4_MUL            = 0x3B5,  /* sub=0xB5 */
OP_0XFD_I32X4_MIN_S          = 0x3B6,  /* sub=0xB6 */
OP_0XFD_I32X4_MIN_U          = 0x3B7,  /* sub=0xB7 */
OP_0XFD_I32X4_MAX_S          = 0x3B8,  /* sub=0xB8 */
OP_0XFD_I32X4_MAX_U          = 0x3B9,  /* sub=0xB9 */
OP_0XFD_I64X2_ABS            = 0x3C0,  /* sub=0xC0 */
OP_0XFD_I64X2_NEG            = 0x3C1,  /* sub=0xC1 */
OP_0XFD_I64X2_ADD            = 0x3CE,  /* sub=0xCE */
OP_0XFD_I64X2_SUB            = 0x3D1,  /* sub=0xD1 */
OP_0XFD_I64X2_MUL            = 0x3D5,  /* sub=0xD5 */
/* q15mulr / extadd_pairwise / extmul / dot */
OP_0XFD_I16X8_Q15MULR_SAT_S          = 0x382,  /* sub=0x82 */
OP_0XFD_I16X8_EXTADD_PAIRWISE_I8X16_S = 0x37C,  /* sub=0x7C */
OP_0XFD_I16X8_EXTADD_PAIRWISE_I8X16_U = 0x37D,
OP_0XFD_I32X4_EXTADD_PAIRWISE_I16X8_S = 0x37E,
OP_0XFD_I32X4_EXTADD_PAIRWISE_I16X8_U = 0x37F,
OP_0XFD_I16X8_EXTMUL_LOW_I8X16_S      = 0x39C,  /* sub=0x9C */
OP_0XFD_I16X8_EXTMUL_HIGH_I8X16_S     = 0x39D,
OP_0XFD_I16X8_EXTMUL_LOW_I8X16_U      = 0x39E,
OP_0XFD_I16X8_EXTMUL_HIGH_I8X16_U     = 0x39F,
OP_0XFD_I32X4_DOT_I16X8_S             = 0x3BA,  /* sub=0xBA */
OP_0XFD_I32X4_EXTMUL_LOW_I16X8_S      = 0x3BC,  /* sub=0xBC */
OP_0XFD_I32X4_EXTMUL_HIGH_I16X8_S     = 0x3BD,
OP_0XFD_I32X4_EXTMUL_LOW_I16X8_U      = 0x3BE,
OP_0XFD_I32X4_EXTMUL_HIGH_I16X8_U     = 0x3BF,
OP_0XFD_I64X2_EXTMUL_LOW_I32X4_S      = 0x3DC,  /* sub=0xDC */
OP_0XFD_I64X2_EXTMUL_HIGH_I32X4_S     = 0x3DD,
OP_0XFD_I64X2_EXTMUL_LOW_I32X4_U      = 0x3DE,
OP_0XFD_I64X2_EXTMUL_HIGH_I32X4_U     = 0x3DF,
/* integer saturating add/sub (8/16 bit のみ) */
OP_0XFD_I8X16_ADD_SAT_S      = 0x36F,  /* sub=0x6F */
OP_0XFD_I8X16_ADD_SAT_U      = 0x370,  /* sub=0x70 */
OP_0XFD_I8X16_SUB_SAT_S      = 0x372,  /* sub=0x72 */
OP_0XFD_I8X16_SUB_SAT_U      = 0x373,  /* sub=0x73 */
OP_0XFD_I16X8_ADD_SAT_S      = 0x38F,  /* sub=0x8F */
OP_0XFD_I16X8_ADD_SAT_U      = 0x390,  /* sub=0x90 */
OP_0XFD_I16X8_SUB_SAT_S      = 0x392,  /* sub=0x92 */
OP_0XFD_I16X8_SUB_SAT_U      = 0x393,  /* sub=0x93 */
/* float arithmetic (f32x4 / f64x2) */
OP_0XFD_F32X4_ABS            = 0x3E0,  /* sub=0xE0 */
OP_0XFD_F32X4_NEG            = 0x3E1,
OP_0XFD_F32X4_SQRT           = 0x3E3,
OP_0XFD_F32X4_ADD            = 0x3E4,
OP_0XFD_F32X4_SUB            = 0x3E5,
OP_0XFD_F32X4_MUL            = 0x3E6,
OP_0XFD_F32X4_DIV            = 0x3E7,
OP_0XFD_F32X4_MIN            = 0x3E8,
OP_0XFD_F32X4_MAX            = 0x3E9,
OP_0XFD_F32X4_PMIN           = 0x3EA,
OP_0XFD_F32X4_PMAX           = 0x3EB,
OP_0XFD_F64X2_ABS            = 0x3EC,  /* sub=0xEC */
OP_0XFD_F64X2_NEG            = 0x3ED,
OP_0XFD_F64X2_SQRT           = 0x3EF,
OP_0XFD_F64X2_ADD            = 0x3F0,
OP_0XFD_F64X2_SUB            = 0x3F1,
OP_0XFD_F64X2_MUL            = 0x3F2,
OP_0XFD_F64X2_DIV            = 0x3F3,
OP_0XFD_F64X2_MIN            = 0x3F4,
OP_0XFD_F64X2_MAX            = 0x3F5,
OP_0XFD_F64X2_PMIN           = 0x3F6,
OP_0XFD_F64X2_PMAX           = 0x3F7,
/* ── WASM 3.0 Relaxed SIMD (0xFD prefix, sub-op 0x100..0x113) ──
 * synthetic = 0x300 + sub_op = 0x400..0x413
 * semantics は strict 版に対して NaN handling / -0/+0 区別 / 範囲外動作が implementation-defined */
OP_0XFD_I8X16_RELAXED_SWIZZLE                  = 0x400,
OP_0XFD_I32X4_RELAXED_TRUNC_F32X4_S            = 0x401,
OP_0XFD_I32X4_RELAXED_TRUNC_F32X4_U            = 0x402,
OP_0XFD_I32X4_RELAXED_TRUNC_F64X2_S_ZERO       = 0x403,
OP_0XFD_I32X4_RELAXED_TRUNC_F64X2_U_ZERO       = 0x404,
OP_0XFD_F32X4_RELAXED_MADD                     = 0x405,
OP_0XFD_F32X4_RELAXED_NMADD                    = 0x406,
OP_0XFD_F64X2_RELAXED_MADD                     = 0x407,
OP_0XFD_F64X2_RELAXED_NMADD                    = 0x408,
OP_0XFD_I8X16_RELAXED_LANESELECT               = 0x409,
OP_0XFD_I16X8_RELAXED_LANESELECT               = 0x40A,
OP_0XFD_I32X4_RELAXED_LANESELECT               = 0x40B,
OP_0XFD_I64X2_RELAXED_LANESELECT               = 0x40C,
OP_0XFD_F32X4_RELAXED_MIN                      = 0x40D,
OP_0XFD_F32X4_RELAXED_MAX                      = 0x40E,
OP_0XFD_F64X2_RELAXED_MIN                      = 0x40F,
OP_0XFD_F64X2_RELAXED_MAX                      = 0x410,
OP_0XFD_I16X8_RELAXED_Q15MULR_S                = 0x411,
OP_0XFD_I16X8_RELAXED_DOT_I8X16_I7X16_S        = 0x412,
OP_0XFD_I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S    = 0x413,
/* ── WASM 3.0 Threads & Atomics (0xFE prefix, sub-op 0x00..0x4E) ──
 * synthetic = 0x500 + sub_op
 * シングルスレッド runtime のため atomic 操作は non-atomic 実装で仕様準拠 */
OP_0XFE_MEMORY_ATOMIC_NOTIFY        = 0x500,  /* sub=0x00 */
OP_0XFE_MEMORY_ATOMIC_WAIT32        = 0x501,  /* sub=0x01 */
OP_0XFE_MEMORY_ATOMIC_WAIT64        = 0x502,  /* sub=0x02 */
OP_0XFE_ATOMIC_FENCE                = 0x503,  /* sub=0x03 */
OP_0XFE_I32_ATOMIC_LOAD             = 0x510,  /* sub=0x10 */
OP_0XFE_I64_ATOMIC_LOAD             = 0x511,
OP_0XFE_I32_ATOMIC_LOAD8_U          = 0x512,
OP_0XFE_I32_ATOMIC_LOAD16_U         = 0x513,
OP_0XFE_I64_ATOMIC_LOAD8_U          = 0x514,
OP_0XFE_I64_ATOMIC_LOAD16_U         = 0x515,
OP_0XFE_I64_ATOMIC_LOAD32_U         = 0x516,
OP_0XFE_I32_ATOMIC_STORE            = 0x517,
OP_0XFE_I64_ATOMIC_STORE            = 0x518,
OP_0XFE_I32_ATOMIC_STORE8           = 0x519,
OP_0XFE_I32_ATOMIC_STORE16          = 0x51A,
OP_0XFE_I64_ATOMIC_STORE8           = 0x51B,
OP_0XFE_I64_ATOMIC_STORE16          = 0x51C,
OP_0XFE_I64_ATOMIC_STORE32          = 0x51D,
/* rmw add (0x1E〜0x24) */
OP_0XFE_I32_ATOMIC_RMW_ADD          = 0x51E,
OP_0XFE_I64_ATOMIC_RMW_ADD          = 0x51F,
OP_0XFE_I32_ATOMIC_RMW8_ADD_U       = 0x520,
OP_0XFE_I32_ATOMIC_RMW16_ADD_U      = 0x521,
OP_0XFE_I64_ATOMIC_RMW8_ADD_U       = 0x522,
OP_0XFE_I64_ATOMIC_RMW16_ADD_U      = 0x523,
OP_0XFE_I64_ATOMIC_RMW32_ADD_U      = 0x524,
/* rmw sub (0x25〜0x2B) */
OP_0XFE_I32_ATOMIC_RMW_SUB          = 0x525,
OP_0XFE_I64_ATOMIC_RMW_SUB          = 0x526,
OP_0XFE_I32_ATOMIC_RMW8_SUB_U       = 0x527,
OP_0XFE_I32_ATOMIC_RMW16_SUB_U      = 0x528,
OP_0XFE_I64_ATOMIC_RMW8_SUB_U       = 0x529,
OP_0XFE_I64_ATOMIC_RMW16_SUB_U      = 0x52A,
OP_0XFE_I64_ATOMIC_RMW32_SUB_U      = 0x52B,
/* rmw and (0x2C〜0x32) */
OP_0XFE_I32_ATOMIC_RMW_AND          = 0x52C,
OP_0XFE_I64_ATOMIC_RMW_AND          = 0x52D,
OP_0XFE_I32_ATOMIC_RMW8_AND_U       = 0x52E,
OP_0XFE_I32_ATOMIC_RMW16_AND_U      = 0x52F,
OP_0XFE_I64_ATOMIC_RMW8_AND_U       = 0x530,
OP_0XFE_I64_ATOMIC_RMW16_AND_U      = 0x531,
OP_0XFE_I64_ATOMIC_RMW32_AND_U      = 0x532,
/* rmw or (0x33〜0x39) */
OP_0XFE_I32_ATOMIC_RMW_OR           = 0x533,
OP_0XFE_I64_ATOMIC_RMW_OR           = 0x534,
OP_0XFE_I32_ATOMIC_RMW8_OR_U        = 0x535,
OP_0XFE_I32_ATOMIC_RMW16_OR_U       = 0x536,
OP_0XFE_I64_ATOMIC_RMW8_OR_U        = 0x537,
OP_0XFE_I64_ATOMIC_RMW16_OR_U       = 0x538,
OP_0XFE_I64_ATOMIC_RMW32_OR_U       = 0x539,
/* rmw xor (0x3A〜0x40) */
OP_0XFE_I32_ATOMIC_RMW_XOR          = 0x53A,
OP_0XFE_I64_ATOMIC_RMW_XOR          = 0x53B,
OP_0XFE_I32_ATOMIC_RMW8_XOR_U       = 0x53C,
OP_0XFE_I32_ATOMIC_RMW16_XOR_U      = 0x53D,
OP_0XFE_I64_ATOMIC_RMW8_XOR_U       = 0x53E,
OP_0XFE_I64_ATOMIC_RMW16_XOR_U      = 0x53F,
OP_0XFE_I64_ATOMIC_RMW32_XOR_U      = 0x540,
/* rmw xchg (0x41〜0x47) */
OP_0XFE_I32_ATOMIC_RMW_XCHG         = 0x541,
OP_0XFE_I64_ATOMIC_RMW_XCHG         = 0x542,
OP_0XFE_I32_ATOMIC_RMW8_XCHG_U      = 0x543,
OP_0XFE_I32_ATOMIC_RMW16_XCHG_U     = 0x544,
OP_0XFE_I64_ATOMIC_RMW8_XCHG_U      = 0x545,
OP_0XFE_I64_ATOMIC_RMW16_XCHG_U     = 0x546,
OP_0XFE_I64_ATOMIC_RMW32_XCHG_U     = 0x547,
/* rmw cmpxchg (0x48〜0x4E) */
OP_0XFE_I32_ATOMIC_RMW_CMPXCHG      = 0x548,
OP_0XFE_I64_ATOMIC_RMW_CMPXCHG      = 0x549,
OP_0XFE_I32_ATOMIC_RMW8_CMPXCHG_U   = 0x54A,
OP_0XFE_I32_ATOMIC_RMW16_CMPXCHG_U  = 0x54B,
OP_0XFE_I64_ATOMIC_RMW8_CMPXCHG_U   = 0x54C,
OP_0XFE_I64_ATOMIC_RMW16_CMPXCHG_U  = 0x54D,
OP_0XFE_I64_ATOMIC_RMW32_CMPXCHG_U  = 0x54E,
} kinowasm_opcode_t;
