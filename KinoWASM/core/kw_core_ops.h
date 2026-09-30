/* core_ops.h — handler fn-ptr の公開テーブル。
 *   core_exec.c: #define CORE_DEFINE_OPS してから include → non-static グローバル定義
 *     (上で定義した static handler を代入)。
 *   core_compile.c: そのまま include → extern 宣言のみ。
 * binop は (ss, sr, si) 3 変種を構造体で保持。div/rem 等 ss のみのものは sr/si=NULL。 */
/* 複数 include 可 (define / extern / name の 3 モード)。pragma once は付けない。 */
#ifndef CORE_BINOP_T_DEFINED
#define CORE_BINOP_T_DEFINED
#include "kw_core.h"
/* ss/sr/si: a=slot。rs/ri: a=r0 (spill 回避、算術のみ)。st_*: 結果を slot へ直接 (binop;local.set 融合)。 */
typedef struct {
	coreop_t ss;
	coreop_t sr;
	coreop_t si;
	coreop_t rs;
	coreop_t ri;
	coreop_t st_ss;
	coreop_t st_sr;
	coreop_t st_si;
} corebinop_t;
#endif

#if defined(CORE_NAME_OPS)
/* マクロ定義内は継続行 (\) で 1 行 1 命令まで。空行は継続を切るため入れられない。 */
#define BINOP(name, a, b, c) \
	if(fn == core_##name.ss) return #name; \
	if((b) && fn == core_##name.sr) return #name; \
	if((c) && fn == core_##name.si) return #name;
#define BINOPR(name, a, b, c, d, e, f, g, h) \
	BINOP(name, a, b, c) \
	if((d) && fn == core_##name.rs) return #name; \
	if((e) && fn == core_##name.ri) return #name; \
	if((f) && fn == core_##name.st_ss) return #name; \
	if((g) && fn == core_##name.st_sr) return #name; \
	if((h) && fn == core_##name.st_si) return #name;
#define OP(name, h)          if(fn == core_##name) return #name;
#elif defined(CORE_DEFINE_OPS)
#define BINOP(name, a, b, c) corebinop_t core_##name = { a, b, c, NULL, NULL, NULL, NULL, NULL };
#define BINOPR(name, a, b, c, d, e, f, g, h) corebinop_t core_##name = { a, b, c, d, e, f, g, h };
#define OP(name, h)          coreop_t   core_##name = h;
#else
#define BINOP(name, a, b, c) extern corebinop_t core_##name;
#define BINOPR(name, a, b, c, d, e, f, g, h) extern corebinop_t core_##name;
#define OP(name, h)          extern coreop_t   core_##name;
#endif

/* ── scalar / データ移動 ── */
OP(const_i32, H_const_i32)
OP(const_i64, H_const_i64)
OP(const_f64, H_const_f64)
OP(getslot,   H_getslot)
OP(setslot,   H_setslot)
OP(copyslot,  H_copyslot)
OP(const_slot_i32, H_const_slot_i32)
OP(const_slot_i64, H_const_slot_i64)
OP(drop,      H_drop)
OP(select,    H_select)
OP(global_get,H_global_get)
OP(global_set,H_global_set)
OP(br,        H_br)
OP(br_if,     H_br_if)
OP(br_if_v,   H_br_if_v)
OP(br_if_not, H_br_if_not)
OP(br_table,  H_br_table)
OP(ret,       H_return)
OP(unreachable, H_unreachable)
OP(ret_multi, H_ret_multi)
OP(mret_get,  H_mret_get)
OP(call,      H_call)
OP(call_indirect, H_call_indirect)
OP(return_call, H_return_call)
OP(return_call_indirect, H_return_call_indirect)
OP(throw, H_throw)
OP(throw_local, H_throw_local)
OP(call_eh, H_call_eh)
OP(call_indirect_eh, H_call_indirect_eh)
OP(catch, H_catch)
OP(catch_all, H_catch_all)
OP(catch_nt, H_catch_nt)
OP(catch_all_nt, H_catch_all_nt)
OP(rethrow_uncaught, H_rethrow_uncaught)
OP(caught_pop, H_caught_pop)
OP(rethrow_local, H_rethrow_local)
OP(rethrow, H_rethrow)
OP(catch_ref, H_catch_ref)
OP(catch_all_ref, H_catch_all_ref)
OP(throw_ref_local, H_throw_ref_local)
OP(throw_ref, H_throw_ref)

/* ── 単項 / 変換 ── */
OP(i32_eqz,   Hi32_eqz)
OP(i32_clz,   Hi32_clz)
OP(i32_ctz,   Hi32_ctz)
OP(i32_popcnt,Hi32_popcnt)
OP(i32_wrap_i64, Hi32_wrap_i64)
OP(i64_extend_i32_s, Hi64_extend_i32_s)
OP(i64_extend_i32_u, Hi64_extend_i32_u)
OP(i64_eqz, Hi64_eqz)
OP(i64_clz, Hi64_clz)
OP(i64_ctz, Hi64_ctz)
OP(i64_popcnt, Hi64_popcnt)
OP(i32_extend8_s, Hi32_extend8_s)
OP(i32_extend16_s, Hi32_extend16_s)
OP(i64_extend8_s, Hi64_extend8_s)
OP(i64_extend16_s, Hi64_extend16_s)
OP(i64_extend32_s, Hi64_extend32_s)
OP(f64_convert_i32_s, Hf64_convert_i32_s)
OP(f64_convert_i32_u, Hf64_convert_i32_u)
OP(f64_convert_i64_s, Hf64_convert_i64_s)
OP(f64_convert_i64_u, Hf64_convert_i64_u)
OP(i32_trunc_f64_s, Hi32_trunc_f64_s)
OP(i32_trunc_f64_u, Hi32_trunc_f64_u)
OP(f64_abs, Hf64_abs)
OP(f64_neg, Hf64_neg)
/* float 補完 (f64 単項/変換) */
OP(f64_sqrt, Hf64_sqrt)
OP(f64_ceil, Hf64_ceil)
OP(f64_floor, Hf64_floor)
OP(f64_trunc, Hf64_trunc)
OP(f64_nearest, Hf64_nearest)
OP(f64_promote_f32, Hf64_promote_f32)
OP(i64_trunc_f64_s, Hi64_trunc_f64_s)
OP(i64_trunc_f64_u, Hi64_trunc_f64_u)
/* f32 単項/変換 */
OP(f32_abs, Hf32_abs)
OP(f32_neg, Hf32_neg)
OP(f32_sqrt, Hf32_sqrt)
OP(f32_ceil, Hf32_ceil)
OP(f32_floor, Hf32_floor)
OP(f32_trunc, Hf32_trunc)
OP(f32_nearest, Hf32_nearest)
OP(f32_convert_i32_s, Hf32_convert_i32_s)
OP(f32_convert_i32_u, Hf32_convert_i32_u)
OP(f32_convert_i64_s, Hf32_convert_i64_s)
OP(f32_convert_i64_u, Hf32_convert_i64_u)
OP(f32_demote_f64, Hf32_demote_f64)
OP(i32_trunc_f32_s, Hi32_trunc_f32_s)
OP(i32_trunc_f32_u, Hi32_trunc_f32_u)
OP(i64_trunc_f32_s, Hi64_trunc_f32_s)
OP(i64_trunc_f32_u, Hi64_trunc_f32_u)
OP(reinterpret, H_reinterpret)
OP(memory_size, H_memory_size)
OP(memory_grow, H_memory_grow)
/* 0xFC: 飽和変換 + bulk memory */
OP(i32_trunc_sat_f32_s, Hi32_trunc_sat_f32_s)
OP(i32_trunc_sat_f32_u, Hi32_trunc_sat_f32_u)
OP(i32_trunc_sat_f64_s, Hi32_trunc_sat_f64_s)
OP(i32_trunc_sat_f64_u, Hi32_trunc_sat_f64_u)
OP(i64_trunc_sat_f32_s, Hi64_trunc_sat_f32_s)
OP(i64_trunc_sat_f32_u, Hi64_trunc_sat_f32_u)
OP(i64_trunc_sat_f64_s, Hi64_trunc_sat_f64_s)
OP(i64_trunc_sat_f64_u, Hi64_trunc_sat_f64_u)
OP(memory_copy, H_memory_copy)
OP(memory_fill, H_memory_fill)
/* reference / table ops */
OP(ref_is_null, H_ref_is_null)
OP(table_get,   H_table_get)
OP(table_set,   H_table_set)
OP(table_size,  H_table_size)
OP(table_fill,  H_table_fill)
OP(table_copy,  H_table_copy)
OP(table_grow,  H_table_grow)
OP(memory_init, H_memory_init)
OP(data_drop,   H_data_drop)
OP(table_init,  H_table_init)
OP(elem_drop,   H_elem_drop)
OP(load_memN,     H_load_memN)
OP(store_memN,    H_store_memN)
OP(idx64_chk,     H_idx64_chk)
OP(i64_add128,    H_i64_add128)
OP(i64_sub128,    H_i64_sub128)
OP(i64_mul_wide_s,H_i64_mul_wide_s)
OP(i64_mul_wide_u,H_i64_mul_wide_u)

/* ── load / store ── */
OP(i32_load,    Hi32_load)
OP(i32_load8_s, Hi32_load8_s)
OP(i32_load8_u, Hi32_load8_u)
OP(i32_load16_s,Hi32_load16_s)
OP(i32_load16_u,Hi32_load16_u)
OP(i64_load,    Hi64_load)
OP(f64_load,    Hf64_load)
OP(i64_load8_s, Hi64_load8_s)
OP(i64_load8_u, Hi64_load8_u)
OP(i64_load16_s,Hi64_load16_s)
OP(i64_load16_u,Hi64_load16_u)
OP(i64_load32_s,Hi64_load32_s)
OP(i64_load32_u,Hi64_load32_u)
/* load; local.set/tee 融合版 (hot i32 load 4 種のみ)。 */
OP(i32_load_st,    Hi32_load_st)
OP(i32_load8_u_st, Hi32_load8_u_st)
OP(i32_load16_u_st,Hi32_load16_u_st)
OP(i32_load16_s_st,Hi32_load16_s_st)
OP(i32_store,   Hi32_store)
OP(i32_store8,  Hi32_store8)
OP(i32_store16, Hi32_store16)
OP(i64_store,   Hi64_store)
OP(f64_store,   Hf64_store)
OP(i64_store8,  Hi64_store8)
OP(i64_store16, Hi64_store16)
OP(i64_store32, Hi64_store32)

/* ── div/rem (ss のみ。sr/si=NULL → compiler が ss にフォールバック) ── */
BINOP(i32_div_s_tab, Hi32_div_s_ss, NULL, NULL)
BINOP(i32_div_u_tab, Hi32_div_u_ss, NULL, NULL)
BINOP(i32_rem_s_tab, Hi32_rem_s_ss, NULL, NULL)
BINOP(i32_rem_u_tab, Hi32_rem_u_ss, NULL, NULL)

/* ── i32 二項 (算術は ss/sr/si/rs/ri + st_*(結果→slot)) ── */
BINOPR(i32_add, Hi32_add_ss, Hi32_add_sr, Hi32_add_si, Hi32_add_rs, Hi32_add_ri, Hi32_add_st_ss, Hi32_add_st_sr, Hi32_add_st_si)
BINOPR(i32_sub, Hi32_sub_ss, Hi32_sub_sr, Hi32_sub_si, Hi32_sub_rs, Hi32_sub_ri, Hi32_sub_st_ss, Hi32_sub_st_sr, Hi32_sub_st_si)
BINOPR(i32_mul, Hi32_mul_ss, Hi32_mul_sr, Hi32_mul_si, Hi32_mul_rs, Hi32_mul_ri, Hi32_mul_st_ss, Hi32_mul_st_sr, Hi32_mul_st_si)
BINOPR(i32_and, Hi32_and_ss, Hi32_and_sr, Hi32_and_si, Hi32_and_rs, Hi32_and_ri, Hi32_and_st_ss, Hi32_and_st_sr, Hi32_and_st_si)
BINOPR(i32_or,  Hi32_or_ss,  Hi32_or_sr,  Hi32_or_si,  Hi32_or_rs,  Hi32_or_ri,  Hi32_or_st_ss,  Hi32_or_st_sr,  Hi32_or_st_si)
BINOPR(i32_xor, Hi32_xor_ss, Hi32_xor_sr, Hi32_xor_si, Hi32_xor_rs, Hi32_xor_ri, Hi32_xor_st_ss, Hi32_xor_st_sr, Hi32_xor_st_si)
BINOPR(i32_shl, Hi32_shl_ss, Hi32_shl_sr, Hi32_shl_si, Hi32_shl_rs, Hi32_shl_ri, Hi32_shl_st_ss, Hi32_shl_st_sr, Hi32_shl_st_si)
BINOPR(i32_shr_s, Hi32_shr_s_ss, Hi32_shr_s_sr, Hi32_shr_s_si, Hi32_shr_s_rs, Hi32_shr_s_ri, Hi32_shr_s_st_ss, Hi32_shr_s_st_sr, Hi32_shr_s_st_si)
BINOPR(i32_shr_u, Hi32_shr_u_ss, Hi32_shr_u_sr, Hi32_shr_u_si, Hi32_shr_u_rs, Hi32_shr_u_ri, Hi32_shr_u_st_ss, Hi32_shr_u_st_sr, Hi32_shr_u_st_si)
BINOPR(i32_rotl, Hi32_rotl_ss, Hi32_rotl_sr, Hi32_rotl_si, Hi32_rotl_rs, Hi32_rotl_ri, NULL, NULL, NULL)
BINOPR(i32_rotr, Hi32_rotr_ss, Hi32_rotr_sr, Hi32_rotr_si, Hi32_rotr_rs, Hi32_rotr_ri, NULL, NULL, NULL)
BINOP(i32_eq,  Hi32_eq_ss,  Hi32_eq_sr,  Hi32_eq_si)
BINOP(i32_ne,  Hi32_ne_ss,  Hi32_ne_sr,  Hi32_ne_si)
BINOP(i32_lt_s, Hi32_lt_s_ss, Hi32_lt_s_sr, Hi32_lt_s_si)
BINOP(i32_lt_u, Hi32_lt_u_ss, Hi32_lt_u_sr, Hi32_lt_u_si)
BINOP(i32_gt_s, Hi32_gt_s_ss, Hi32_gt_s_sr, Hi32_gt_s_si)
BINOP(i32_gt_u, Hi32_gt_u_ss, Hi32_gt_u_sr, Hi32_gt_u_si)
BINOP(i32_le_s, Hi32_le_s_ss, Hi32_le_s_sr, Hi32_le_s_si)
BINOP(i32_le_u, Hi32_le_u_ss, Hi32_le_u_sr, Hi32_le_u_si)
BINOP(i32_ge_s, Hi32_ge_s_ss, Hi32_ge_s_sr, Hi32_ge_s_si)
BINOP(i32_ge_u, Hi32_ge_u_ss, Hi32_ge_u_sr, Hi32_ge_u_si)

/* ── compare+br_if 融合 (ss/sr/si)。compiler が `cmp; br_if`/`cmp; if` を 1 op 化 ── */
BINOP(i32_eq_brif,   Hi32_eq_brif_ss,   Hi32_eq_brif_sr,   Hi32_eq_brif_si)
BINOP(i32_ne_brif,   Hi32_ne_brif_ss,   Hi32_ne_brif_sr,   Hi32_ne_brif_si)
BINOP(i32_lt_s_brif, Hi32_lt_s_brif_ss, Hi32_lt_s_brif_sr, Hi32_lt_s_brif_si)
BINOP(i32_le_s_brif, Hi32_le_s_brif_ss, Hi32_le_s_brif_sr, Hi32_le_s_brif_si)
BINOP(i32_gt_s_brif, Hi32_gt_s_brif_ss, Hi32_gt_s_brif_sr, Hi32_gt_s_brif_si)
BINOP(i32_ge_s_brif, Hi32_ge_s_brif_ss, Hi32_ge_s_brif_sr, Hi32_ge_s_brif_si)
BINOP(i32_lt_u_brif, Hi32_lt_u_brif_ss, Hi32_lt_u_brif_sr, Hi32_lt_u_brif_si)
BINOP(i32_le_u_brif, Hi32_le_u_brif_ss, Hi32_le_u_brif_sr, Hi32_le_u_brif_si)
BINOP(i32_gt_u_brif, Hi32_gt_u_brif_ss, Hi32_gt_u_brif_sr, Hi32_gt_u_brif_si)
BINOP(i32_ge_u_brif, Hi32_ge_u_brif_ss, Hi32_ge_u_brif_sr, Hi32_ge_u_brif_si)

/* ── i64 二項 (ss/sr) ── */
BINOP(i64_add, Hi64_add_ss, Hi64_add_sr, NULL)
BINOP(i64_sub, Hi64_sub_ss, Hi64_sub_sr, NULL)
BINOP(i64_mul, Hi64_mul_ss, Hi64_mul_sr, NULL)
BINOP(i64_and, Hi64_and_ss, Hi64_and_sr, NULL)
BINOP(i64_or,  Hi64_or_ss,  Hi64_or_sr,  NULL)
BINOP(i64_xor, Hi64_xor_ss, Hi64_xor_sr, NULL)
BINOP(i64_shl, Hi64_shl_ss, Hi64_shl_sr, NULL)
BINOP(i64_shr_s, Hi64_shr_s_ss, Hi64_shr_s_sr, NULL)
BINOP(i64_shr_u, Hi64_shr_u_ss, Hi64_shr_u_sr, NULL)
BINOP(i64_rotl, Hi64_rotl_ss, Hi64_rotl_sr, NULL)
BINOP(i64_rotr, Hi64_rotr_ss, Hi64_rotr_sr, NULL)
BINOP(i64_eq, Hi64_eq_ss, Hi64_eq_sr, NULL)
BINOP(i64_ne, Hi64_ne_ss, Hi64_ne_sr, NULL)
BINOP(i64_lt_s, Hi64_lt_s_ss, Hi64_lt_s_sr, NULL)
BINOP(i64_gt_s, Hi64_gt_s_ss, Hi64_gt_s_sr, NULL)
BINOP(i64_le_s, Hi64_le_s_ss, Hi64_le_s_sr, NULL)
BINOP(i64_ge_s, Hi64_ge_s_ss, Hi64_ge_s_sr, NULL)
BINOP(i64_lt_u, Hi64_lt_u_ss, Hi64_lt_u_sr, NULL)
BINOP(i64_gt_u, Hi64_gt_u_ss, Hi64_gt_u_sr, NULL)
BINOP(i64_le_u, Hi64_le_u_ss, Hi64_le_u_sr, NULL)
BINOP(i64_ge_u, Hi64_ge_u_ss, Hi64_ge_u_sr, NULL)
BINOP(i64_div_s_tab, Hi64_div_s_ss, NULL, NULL)
BINOP(i64_div_u_tab, Hi64_div_u_ss, NULL, NULL)
BINOP(i64_rem_s_tab, Hi64_rem_s_ss, NULL, NULL)
BINOP(i64_rem_u_tab, Hi64_rem_u_ss, NULL, NULL)

/* ── f64 二項 (ss/sr) ── */
BINOP(f64_add, Hf64_add_ss, Hf64_add_sr, NULL)
BINOP(f64_sub, Hf64_sub_ss, Hf64_sub_sr, NULL)
BINOP(f64_mul, Hf64_mul_ss, Hf64_mul_sr, NULL)
BINOP(f64_div, Hf64_div_ss, Hf64_div_sr, NULL)
BINOP(f64_eq, Hf64_eq_ss, Hf64_eq_sr, NULL)
BINOP(f64_ne, Hf64_ne_ss, Hf64_ne_sr, NULL)
BINOP(f64_lt, Hf64_lt_ss, Hf64_lt_sr, NULL)
BINOP(f64_gt, Hf64_gt_ss, Hf64_gt_sr, NULL)
BINOP(f64_le, Hf64_le_ss, Hf64_le_sr, NULL)
BINOP(f64_ge, Hf64_ge_ss, Hf64_ge_sr, NULL)
/* f64 二項補完 (min/max/copysign) */
BINOP(f64_min, Hf64_min_ss, Hf64_min_sr, NULL)
BINOP(f64_max, Hf64_max_ss, Hf64_max_sr, NULL)
BINOP(f64_copysign, Hf64_copysign_ss, Hf64_copysign_sr, NULL)

/* ── f32 二項 (ss/sr) ── */
BINOP(f32_add, Hf32_add_ss, Hf32_add_sr, NULL)
BINOP(f32_sub, Hf32_sub_ss, Hf32_sub_sr, NULL)
BINOP(f32_mul, Hf32_mul_ss, Hf32_mul_sr, NULL)
BINOP(f32_div, Hf32_div_ss, Hf32_div_sr, NULL)
BINOP(f32_min, Hf32_min_ss, Hf32_min_sr, NULL)
BINOP(f32_max, Hf32_max_ss, Hf32_max_sr, NULL)
BINOP(f32_copysign, Hf32_copysign_ss, Hf32_copysign_sr, NULL)
BINOP(f32_eq, Hf32_eq_ss, Hf32_eq_sr, NULL)
BINOP(f32_ne, Hf32_ne_ss, Hf32_ne_sr, NULL)
BINOP(f32_lt, Hf32_lt_ss, Hf32_lt_sr, NULL)
BINOP(f32_gt, Hf32_gt_ss, Hf32_gt_sr, NULL)
BINOP(f32_le, Hf32_le_ss, Hf32_le_sr, NULL)
BINOP(f32_ge, Hf32_ge_ss, Hf32_ge_sr, NULL)

#undef BINOP
#undef BINOPR
#undef OP
