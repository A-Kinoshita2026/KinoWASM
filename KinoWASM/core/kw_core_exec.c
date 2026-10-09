/* core_exec.c — register-TOS アーキ実行コア。direct-threaded handler 群 + core_run。
 *
 *  handler ABI: int64_t H(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
 *    pc=自分の最初の operand、sp=フレーム slot 基底、mem=線形メモリ、r0=register-TOS。
 *    NEXT(k): k 個の operand word を消費し、次 handler を musttail。
 *    END/RETURN: plain return で musttail 連鎖を巻き戻し結果を返す。
 *
 *  オペランド規約 (compiler と一致必須):
 *    - 二項 c = a OP b、結果は常に r0。a は slot に正規化済み。b は slot/reg/imm の3変種:
 *        _ss: operands[slot_a, slot_b]   r0 = sp[A] OP sp[B]
 *        _sr: operands[slot_a]          r0 = sp[A] OP r0
 *        _si: operands[slot_a, imm]     r0 = sp[A] OP imm
 *    - 単項: _r (r0 = OP r0)。
 *    - const: r0 = imm。getslot: r0 = sp[s]。setslot: sp[d] = r0。
 *    - r0 は i64。float は bit 再解釈 (union)。
 */
#include "kw_core.h"
#include <math.h>   /* sqrt/ceil/floor/trunc/rint/copysign 等 (float 完備) */
#include "kalloc.h" /* table.grow の realloc */
#include "kw_store.h"  /* store_t (global は store 実体を index 経由で共有: cross-module linking) */

/* clz/ctz/popcount は clang/gcc は __builtin_*、MSVC は <intrin.h> intrinsics。i32.clz/ctz/popcnt
 * ハンドラ用の移植マクロ。呼び元は引数!=0 を保証する (clz/ctz) ので x==0 は扱わない。 */
#if defined(__clang__) || defined(__GNUC__)
#define CORE_CLZ32(x)    __builtin_clz(x)
#define CORE_CTZ32(x)    __builtin_ctz(x)
#define CORE_POPCNT32(x) __builtin_popcount(x)
#define CORE_CLZ64(x)    __builtin_clzll(x)
#define CORE_CTZ64(x)    __builtin_ctzll(x)
#define CORE_POPCNT64(x) __builtin_popcountll(x)
#else
#include <intrin.h>
static inline int core_clz32(uint32_t x)
{
	unsigned long i;
	_BitScanReverse(&i, x);
	return 31 - (int)i;
}
static inline int core_ctz32(uint32_t x)
{
	unsigned long i;
	_BitScanForward(&i, x);
	return (int)i;
}
static inline int core_clz64(uint64_t x)
{
	unsigned long i;
	_BitScanReverse64(&i, x);
	return 63 - (int)i;
}
static inline int core_ctz64(uint64_t x)
{
	unsigned long i;
	_BitScanForward64(&i, x);
	return (int)i;
}
#define CORE_CLZ32(x)    core_clz32(x)
#define CORE_CTZ32(x)    core_ctz32(x)
#define CORE_POPCNT32(x) ((int)__popcnt(x))
#define CORE_CLZ64(x)    core_clz64(x)
#define CORE_CTZ64(x)    core_ctz64(x)
#define CORE_POPCNT64(x) ((int)__popcnt64(x))
#endif

corert_t* g_rt = NULL;

/* ── DEBUG 計装スイッチ。CORE_DEBUG 定義時のみ有効 (本番計測は OFF=高速)。 ── */
/* #define CORE_DEBUG */  /* 有効化すると g_ring_fn/g_cs_fn に呼出履歴を記録し、
                            * segfault/alignfault 時に extrafunction.c がそれを出力する。
                            * 絞り込みは環境変数 KW_RING_SKIP_FROM / KW_RING_EXCLUDE。
                            * per-call コストがあるので通常は OFF。 */
uint32_t g_cur_slots = 0;
#if defined(CORE_DEBUG)
static inline uint32_t CK(uint32_t i)
{
	if(i >= g_cur_slots) {
		fprintf(stderr, "[core] SLOT OOB %u >= %u\n", i, g_cur_slots);
		fflush(stderr);
		abort();
	}
	return i;
}
#else
#define CK(i) (i)
#endif

void core_trap(const char* msg)
{
	g_rt->trapped = 1;
	g_rt->trap_msg = msg;
}

/* r0 (i64) ↔ 各型 再解釈 */
/* i32 結果は符号拡張で r0 へ */
static inline int64_t as_i64(int32_t v)
{
	return (int64_t)v;
}
static inline float r0_f32(int64_t r0)
{
	uint32_t u = (uint32_t)r0;
	float f;
	memcpy(&f, &u, 4);
	return f;
}
static inline double r0_f64(int64_t r0)
{
	uint64_t u = (uint64_t)r0;
	double d;
	memcpy(&d, &u, 8);
	return d;
}
static inline int64_t f32_r0(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return (int64_t)(int32_t)u;
}
static inline int64_t f64_r0(double d)
{
	uint64_t u;
	memcpy(&u, &d, 8);
	return (int64_t)u;
}
/* 算術系の結果格納: NaN は canonical NaN へ正規化する (wasm の nan:canonical/arithmetic は
 * canonical を両方受理)。abs/neg/copysign/reinterpret/load/store 等の bit 保持系は f32_r0/f64_r0
 * (生) を使い NaN payload を温存する。 */
static inline int64_t f32_r0_a(float f)
{
	if(isnan(f))
		return (int64_t)(int32_t)0x7FC00000;

	return f32_r0(f);
}
static inline int64_t f64_r0_a(double d)
{
	if(isnan(d))
		return (int64_t)0x7FF8000000000000ll;

	return f64_r0(d);
}

const coreinstr* g_last_pc = NULL; /* デバッグ: 最後に dispatch した op の operand pc */

#if defined(CORE_PROFILE)
/* op 実行回数プロファイラ (fn-ptr ハッシュ)。CORE_PROFILE 時のみ。計測は重いがカウントは正確。
 * 直前 op との組 (pair) も数え、パーサ emit の冗長パターン (spill 直後の reload 等) を定量化する。
 * 注意: /OPT:ICF で同一コードの op は fn が併合されるため名前は代表 1 つに集約される
 * (例: i32_load と i64_load32_s。この 2 つは意味も同一なので情報は失われない)。 */
#define CORE_PROF_N 1024
coreop_t        g_prof_fn[CORE_PROF_N];
long long     g_prof_ct[CORE_PROF_N]; /* Windows long は 32bit なので 64bit で持つ */
#define CORE_PROF_PAIR_N 8192
coreop_t  g_prof_pair_a[CORE_PROF_PAIR_N];
coreop_t  g_prof_pair_b[CORE_PROF_PAIR_N];
long long g_prof_pair_ct[CORE_PROF_PAIR_N];
long long g_prof_reload_same = 0; /* setslot k → getslot k (r0 に値が残っているのに再ロード = 冗長) */
long long g_prof_reload_diff = 0; /* setslot k → getslot j (別 slot、正当) */
static const coreinstr* g_prof_prev = NULL;
static int64_t H_getslot(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0);
static int64_t H_setslot(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0);
static void prof_hit(const coreinstr* n)
{
	coreop_t fn = n->op;
	unsigned h = (unsigned)((uintptr_t)fn >> 4) & (CORE_PROF_N - 1);
	while(g_prof_fn[h] && g_prof_fn[h] != fn)
		h = (h + 1) & (CORE_PROF_N - 1);

	g_prof_fn[h] = fn;
	g_prof_ct[h]++;
	if(g_prof_prev != NULL) {
		coreop_t pf = g_prof_prev->op;
		unsigned ph = (unsigned)((((uintptr_t)pf >> 4) * 131u) ^ ((uintptr_t)fn >> 4)) & (CORE_PROF_PAIR_N - 1);
		while(g_prof_pair_ct[ph] && !(g_prof_pair_a[ph] == pf && g_prof_pair_b[ph] == fn))
			ph = (ph + 1) & (CORE_PROF_PAIR_N - 1);

		g_prof_pair_a[ph] = pf;
		g_prof_pair_b[ph] = fn;
		g_prof_pair_ct[ph]++;
		if(pf == H_setslot && fn == H_getslot) {
			if(g_prof_prev[1].u32 == n[1].u32)
				g_prof_reload_same++;
			else
				g_prof_reload_diff++;
		}
	}
	g_prof_prev = n;
}
#define NEXT(k) do { \
	const coreinstr* _n = pc + (k); \
	prof_hit(_n); \
	CORE_MUSTTAIL return _n->op(_n + 1, sp, mem, r0); \
} while(0)
#elif defined(CORE_DEBUG)
#define NEXT(k) do { \
	g_last_pc = pc; \
	const coreinstr* _n = pc + (k); \
	CORE_MUSTTAIL return _n->op(_n + 1, sp, mem, r0); \
} while(0)
#else
#define NEXT(k) do { \
	const coreinstr* _n = pc + (k); \
	CORE_MUSTTAIL return _n->op(_n + 1, sp, mem, r0); \
} while(0)
#endif

/* ───────────────────────── データ移動 ───────────────────────── */
static int64_t H_const_i32(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int64_t)pc[0].i32;
	NEXT(1);
}
static int64_t H_const_i64(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = pc[0].i64;
	NEXT(1);
}
static int64_t H_const_f64(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int64_t)pc[0].u64;
	NEXT(1);
}
static int64_t H_getslot(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = sp[CK(pc[0].u32)].i64;
	NEXT(1);
}
static int64_t H_setslot(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	sp[CK(pc[0].u32)].i64 = r0;
	NEXT(1);
}
/* setslot して r0 は次の TOS を保持 (tee/局所最適化用ではなく単純 spill。値は r0 に残す)。 */
static int64_t H_copyslot(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	sp[CK(pc[0].u32)].i64 = sp[CK(pc[1].u32)].i64;
	NEXT(2);
}
/* const を r0 を経由せず直接 slot へ (to_slot が r0 を破壊しないため)。 */
static int64_t H_const_slot_i32(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	sp[CK(pc[0].u32)].i32 = pc[1].i32;
	NEXT(2);
}
static int64_t H_const_slot_i64(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	sp[CK(pc[0].u32)].i64 = pc[1].i64;
	NEXT(2);
}

/* ───────────────────────── 二項 (i32) ───────────────────────── */
/* 整数結果は i32 を符号拡張して r0 へ (上位は未定義扱いだが i32 op は下位 32bit のみ参照)。 */
#define BIN_I32(NAME, EXPR_A_B) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = sp[CK(pc[1].u32)].i32; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = (int32_t)r0; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		NEXT(1); \
	} \
	static int64_t NAME##_si(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = pc[1].i32; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		NEXT(2); \
	} \
	static int64_t NAME##_rs(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = (int32_t)r0; \
		int32_t b = sp[CK(pc[0].u32)].i32; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		NEXT(1); \
	} \
	static int64_t NAME##_ri(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = (int32_t)r0; \
		int32_t b = pc[0].i32; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		NEXT(1); \
	}

/* 比較系専用: 標準比較は BINOP (ss/sr/si) でしか参照されないため _rs/_ri は生成しない
 * (生成すると -Wunused-function。a が r0 側になる比較は compiler が出さない=a を slot 化する)。 */
#define BIN_I32_CMP(NAME, EXPR_A_B) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = sp[CK(pc[1].u32)].i32; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = (int32_t)r0; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		NEXT(1); \
	} \
	static int64_t NAME##_si(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = pc[1].i32; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		NEXT(2); \
	}

/* 結果を slot へ直接書く変種 (binop; local.set/tee x 融合、setslot 消去)。r0 にも結果を残すので
 * local.set (融合後 reg_pos 無効=r0 は次に再ロードされ無害) と local.tee (TOS 継続=r0 に結果が要る)
 * の両方に流用できる。
 *   _st_ss:[a,b,dst]  _st_sr:[a,dst](b=r0)  _st_si:[a,imm,dst]。 */
#define BIN_I32_ST(NAME, EXPR_A_B) \
	static int64_t NAME##_st_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = sp[CK(pc[1].u32)].i32; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		sp[CK(pc[2].u32)].i64 = r0; \
		NEXT(3); \
	} \
	static int64_t NAME##_st_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = (int32_t)r0; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		sp[CK(pc[1].u32)].i64 = r0; \
		NEXT(2); \
	} \
	static int64_t NAME##_st_si(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = pc[1].i32; \
		r0 = as_i64((int32_t)(EXPR_A_B)); \
		sp[CK(pc[2].u32)].i64 = r0; \
		NEXT(3); \
	}
BIN_I32_ST(Hi32_add, (uint32_t)a + (uint32_t)b)	/* 符号付き overflow は UB のため unsigned で行う (shl と対称、以下 add/sub/mul 同様) */
BIN_I32_ST(Hi32_sub, (uint32_t)a - (uint32_t)b)
BIN_I32_ST(Hi32_mul, (uint32_t)a * (uint32_t)b)
BIN_I32_ST(Hi32_and, a & b)
BIN_I32_ST(Hi32_or,  a | b)
BIN_I32_ST(Hi32_xor, a ^ b)
BIN_I32_ST(Hi32_shl, (int32_t)((uint32_t)a << (b & 31)))	/* 符号付き左シフトは UB のため unsigned で行う (shr_u と対称) */
BIN_I32_ST(Hi32_shr_s, a >> (b & 31))
BIN_I32_ST(Hi32_shr_u, (int32_t)((uint32_t)a >> (b & 31)))

BIN_I32(Hi32_add, (uint32_t)a + (uint32_t)b)
BIN_I32(Hi32_sub, (uint32_t)a - (uint32_t)b)
BIN_I32(Hi32_mul, (uint32_t)a * (uint32_t)b)
BIN_I32(Hi32_and, a & b)
BIN_I32(Hi32_or,  a | b)
BIN_I32(Hi32_xor, a ^ b)
BIN_I32(Hi32_shl, (int32_t)((uint32_t)a << (b & 31)))	/* 符号付き左シフトは UB のため unsigned で行う */
BIN_I32(Hi32_shr_s, a >> (b & 31))
BIN_I32(Hi32_shr_u, (int32_t)((uint32_t)a >> (b & 31)))
BIN_I32(Hi32_rotl, (int32_t)(((uint32_t)a << (b&31)) | ((uint32_t)a >> ((0u-(uint32_t)b)&31))))	/* 否定も unsigned (b=INT_MIN の -b は UB) */
BIN_I32(Hi32_rotr, (int32_t)(((uint32_t)a >> (b&31)) | ((uint32_t)a << ((0u-(uint32_t)b)&31))))
/* 比較 (結果 0/1) */
BIN_I32_CMP(Hi32_eq,  a == b)
BIN_I32_CMP(Hi32_ne,  a != b)
BIN_I32_CMP(Hi32_lt_s, a < b)
BIN_I32_CMP(Hi32_lt_u, (uint32_t)a < (uint32_t)b)
BIN_I32_CMP(Hi32_gt_s, a > b)
BIN_I32_CMP(Hi32_gt_u, (uint32_t)a > (uint32_t)b)
BIN_I32_CMP(Hi32_le_s, a <= b)
BIN_I32_CMP(Hi32_le_u, (uint32_t)a <= (uint32_t)b)
BIN_I32_CMP(Hi32_ge_s, a >= b)
BIN_I32_CMP(Hi32_ge_u, (uint32_t)a >= (uint32_t)b)

/* ── compare + br_if 融合ハンドラ ──
 *   compiler が `cmp; br_if` を 1 op に畳む。真なら target へ分岐 (arity-0、r0 はそのまま運ぶ)、
 *   偽なら fall-through。`if` は否定 cmp の同ハンドラを再利用 (then-skip)。
 *   operand: _ss=[slot_a,slot_b,tgt] _sr=[slot_a,tgt](b=r0) _si=[slot_a,imm,tgt] */
#define BRIF_CMP(NAME, EXPR_A_B) \
	static int64_t NAME##_brif_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = sp[CK(pc[1].u32)].i32; \
		if(EXPR_A_B) { \
			const coreinstr* t = pc[2].tgt; \
			CORE_MUSTTAIL return t->op(t + 1, sp, mem, r0); \
		} \
		NEXT(3); \
	} \
	static int64_t NAME##_brif_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = (int32_t)r0; \
		if(EXPR_A_B) { \
			const coreinstr* t = pc[1].tgt; \
			CORE_MUSTTAIL return t->op(t + 1, sp, mem, r0); \
		} \
		NEXT(2); \
	} \
	static int64_t NAME##_brif_si(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int32_t a = sp[CK(pc[0].u32)].i32; \
		int32_t b = pc[1].i32; \
		if(EXPR_A_B) { \
			const coreinstr* t = pc[2].tgt; \
			CORE_MUSTTAIL return t->op(t + 1, sp, mem, r0); \
		} \
		NEXT(3); \
	}
BRIF_CMP(Hi32_eq,   a == b)
BRIF_CMP(Hi32_ne,   a != b)
BRIF_CMP(Hi32_lt_s, a < b)
BRIF_CMP(Hi32_le_s, a <= b)
BRIF_CMP(Hi32_gt_s, a > b)
BRIF_CMP(Hi32_ge_s, a >= b)
BRIF_CMP(Hi32_lt_u, (uint32_t)a < (uint32_t)b)
BRIF_CMP(Hi32_le_u, (uint32_t)a <= (uint32_t)b)
BRIF_CMP(Hi32_gt_u, (uint32_t)a > (uint32_t)b)
BRIF_CMP(Hi32_ge_u, (uint32_t)a >= (uint32_t)b)

/* div/rem は trap あり。a/b 両 slot 版のみ (compiler が常に _ss へ落とす)。 */
static int64_t Hi32_div_s_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	int32_t a = sp[pc[0].u32].i32;
	int32_t b = sp[pc[1].u32].i32;
	if(b == 0) {
		core_trap("div0");
		return 0;
	}
	if(a == INT32_MIN && b == -1) {
		core_trap("ovf");
		return 0;
	}
	r0 = as_i64(a / b);
	NEXT(2);
}
static int64_t Hi32_div_u_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t a = (uint32_t)sp[pc[0].u32].i32;
	uint32_t b = (uint32_t)sp[pc[1].u32].i32;
	if(b == 0) {
		core_trap("div0");
		return 0;
	}
	r0 = as_i64((int32_t)(a / b));
	NEXT(2);
}
static int64_t Hi32_rem_s_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	int32_t a = sp[pc[0].u32].i32;
	int32_t b = sp[pc[1].u32].i32;
	if(b == 0) {
		core_trap("div0");
		return 0;
	}
	r0 = as_i64((a == INT32_MIN && b == -1) ? 0 : a % b);
	NEXT(2);
}
static int64_t Hi32_rem_u_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t a = (uint32_t)sp[pc[0].u32].i32;
	uint32_t b = (uint32_t)sp[pc[1].u32].i32;
	if(b == 0) {
		core_trap("div0");
		return 0;
	}
	r0 = as_i64((int32_t)(a % b));
	NEXT(2);
}

/* ───────────────────────── 単項 (i32) ───────────────────────── */
static int64_t Hi32_eqz(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = as_i64((int32_t)r0 == 0);
	NEXT(0);
}
static int64_t Hi32_clz(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t v = (uint32_t)r0;
	int n = v ? CORE_CLZ32(v) : 32;
	r0 = as_i64(n);
	NEXT(0);
}
static int64_t Hi32_ctz(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t v = (uint32_t)r0;
	int n = v ? CORE_CTZ32(v) : 32;
	r0 = as_i64(n);
	NEXT(0);
}
static int64_t Hi32_popcnt(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = as_i64(CORE_POPCNT32((uint32_t)r0));
	NEXT(0);
}
static int64_t Hi32_wrap_i64(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = as_i64((int32_t)r0);
	NEXT(0);
}
static int64_t Hi64_extend_i32_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int64_t)(int32_t)r0;
	NEXT(0);
}
static int64_t Hi64_extend_i32_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int64_t)(uint64_t)(uint32_t)r0;
	NEXT(0);
}

/* ───────────────────────── 二項 (i64) ───────────────────────── */
#define BIN_I64(NAME, EXPR_A_B) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int64_t a = sp[pc[0].u32].i64; \
		int64_t b = sp[pc[1].u32].i64; \
		r0 = (EXPR_A_B); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int64_t a = sp[pc[0].u32].i64; \
		int64_t b = r0; \
		r0 = (EXPR_A_B); \
		NEXT(1); \
	}
BIN_I64(Hi64_add, (int64_t)((uint64_t)a + (uint64_t)b))	/* 符号付き overflow は UB のため unsigned で行う (i32 側と対称) */
BIN_I64(Hi64_mul, (int64_t)((uint64_t)a * (uint64_t)b))
BIN_I64(Hi64_sub, (int64_t)((uint64_t)a - (uint64_t)b))
BIN_I64(Hi64_and, a & b)
BIN_I64(Hi64_or,  a | b)
BIN_I64(Hi64_xor, a ^ b)
BIN_I64(Hi64_shl, (int64_t)((uint64_t)a << (b & 63)))	/* 符号付き左シフトは UB のため unsigned で行う */
BIN_I64(Hi64_shr_u, (int64_t)((uint64_t)a >> (b & 63)))
BIN_I64(Hi64_shr_s, a >> (b & 63))
BIN_I64(Hi64_rotl, (int64_t)(((uint64_t)a << (b&63)) | ((uint64_t)a >> ((0ull-(uint64_t)b)&63))))	/* 否定も unsigned (b=INT64_MIN の -b は UB) */
BIN_I64(Hi64_rotr, (int64_t)(((uint64_t)a >> (b&63)) | ((uint64_t)a << ((0ull-(uint64_t)b)&63))))
/* i64 比較 (結果 i32)。s 版と u 版。 */
#define CMP_I64(NAME, EXPR) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int64_t a = sp[pc[0].u32].i64; \
		int64_t b = sp[pc[1].u32].i64; \
		r0 = as_i64((int32_t)(EXPR)); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		int64_t a = sp[pc[0].u32].i64; \
		int64_t b = r0; \
		r0 = as_i64((int32_t)(EXPR)); \
		NEXT(1); \
	}
CMP_I64(Hi64_eq, a==b)
CMP_I64(Hi64_ne, a!=b)
CMP_I64(Hi64_lt_s, a<b)
CMP_I64(Hi64_gt_s, a>b)
CMP_I64(Hi64_le_s, a<=b)
CMP_I64(Hi64_ge_s, a>=b)
CMP_I64(Hi64_lt_u, (uint64_t)a<(uint64_t)b)
CMP_I64(Hi64_gt_u, (uint64_t)a>(uint64_t)b)
CMP_I64(Hi64_le_u, (uint64_t)a<=(uint64_t)b)
CMP_I64(Hi64_ge_u, (uint64_t)a>=(uint64_t)b)
static int64_t Hi64_eqz(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = as_i64(r0 == 0);
	NEXT(0);
}
static int64_t Hi64_div_s_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	int64_t a = sp[pc[0].u32].i64;
	int64_t b = sp[pc[1].u32].i64;
	if(b == 0) {
		core_trap("div0");
		return 0;
	}
	if(a == INT64_MIN && b == -1) {
		core_trap("ovf");
		return 0;
	}
	r0 = a / b;
	NEXT(2);
}
static int64_t Hi64_div_u_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint64_t a = (uint64_t)sp[pc[0].u32].i64;
	uint64_t b = (uint64_t)sp[pc[1].u32].i64;
	if(b == 0) {
		core_trap("div0");
		return 0;
	}
	r0 = (int64_t)(a / b);
	NEXT(2);
}
static int64_t Hi64_rem_s_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	int64_t a = sp[pc[0].u32].i64;
	int64_t b = sp[pc[1].u32].i64;
	if(b == 0) {
		core_trap("div0");
		return 0;
	}
	r0 = (a == INT64_MIN && b == -1) ? 0 : a % b;
	NEXT(2);
}
static int64_t Hi64_rem_u_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint64_t a = (uint64_t)sp[pc[0].u32].i64;
	uint64_t b = (uint64_t)sp[pc[1].u32].i64;
	if(b == 0) {
		core_trap("div0");
		return 0;
	}
	r0 = (int64_t)(a % b);
	NEXT(2);
}

/* ───────────────────────── 二項 (f64、最小) ───────────────────────── */
/* 算術なので結果 NaN は canonical 正規化 (f64_r0_a)。 */
#define BIN_F64(NAME, OP) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		double a = sp[pc[0].u32].f64; \
		double b = sp[pc[1].u32].f64; \
		r0 = f64_r0_a(a OP b); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		double a = sp[pc[0].u32].f64; \
		double b = r0_f64(r0); \
		r0 = f64_r0_a(a OP b); \
		NEXT(1); \
	}
BIN_F64(Hf64_add, +)
BIN_F64(Hf64_sub, -)
BIN_F64(Hf64_mul, *)
BIN_F64(Hf64_div, /)
static int64_t Hf64_convert_i32_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0((double)(int32_t)r0);
	NEXT(0);
}
static int64_t Hi32_trunc_f64_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	double d = r0_f64(r0);
	if(!(d > -2147483649.0 && d < 2147483648.0)) {
		core_trap("trunc");
		return 0;
	}
	r0 = as_i64((int32_t)d);
	NEXT(0);
}
static int64_t Hi32_trunc_f64_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	double d = r0_f64(r0);
	if(!(d > -1.0 && d < 4294967296.0)) {
		core_trap("trunc");
		return 0;
	}
	r0 = as_i64((int32_t)(uint32_t)d);
	NEXT(0);
}

/* ───────────────────────── memory load/store ───────────────────────── */
/* アドレス = (uint32)r0 + offset。境界チェック後 mem へ直接アクセス (flat)。 */
#define LOAD(NAME, CTYPE, RESULT_EXPR, SZ) \
	static int64_t NAME(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		uint64_t ea = (uint64_t)(uint32_t)r0 + pc[0].u32; \
		if(ea + (SZ) > g_rt->mem_size) { \
			core_trap("oob load"); \
			return 0; \
		} \
		CTYPE v; \
		memcpy(&v, mem + ea, SZ); \
		r0 = (RESULT_EXPR); \
		NEXT(1); \
	}
LOAD(Hi32_load,    int32_t,  as_i64(v), 4)
LOAD(Hi32_load8_s, int8_t,   as_i64((int32_t)v), 1)
LOAD(Hi32_load8_u, uint8_t,  as_i64((int32_t)v), 1)
LOAD(Hi32_load16_s,int16_t,  as_i64((int32_t)v), 2)
LOAD(Hi32_load16_u,uint16_t, as_i64((int32_t)v), 2)
LOAD(Hi64_load,    int64_t,  v, 8)
LOAD(Hf64_load,    int64_t,  v, 8)
LOAD(Hi64_load8_s, int8_t,   (int64_t)v, 1)
LOAD(Hi64_load8_u, uint8_t,  (int64_t)v, 1)
LOAD(Hi64_load16_s,int16_t,  (int64_t)v, 2)
LOAD(Hi64_load16_u,uint16_t, (int64_t)v, 2)
LOAD(Hi64_load32_s,int32_t,  (int64_t)v, 4)
LOAD(Hi64_load32_u,uint32_t, (int64_t)v, 4)
/* load; local.set/tee 融合: 結果を r0 に残しつつ dst slot へも書く (後続 setslot を消去)。operands[offset, dst]。
 * hot な i32 load 4 種のみ生成する。load ハンドラは境界チェックで大きく、全 13 種へ広げると I-cache 圧で
 * 逆に遅くなる (addr slot 直読みで実証済み)。st 版は set/tee 両方に流用 (r0 継続で tee も成立)。 */
#define LOAD_ST(NAME, CTYPE, RESULT_EXPR, SZ) \
	static int64_t NAME(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		uint64_t ea = (uint64_t)(uint32_t)r0 + pc[0].u32; \
		if(ea + (SZ) > g_rt->mem_size) { \
			core_trap("oob load"); \
			return 0; \
		} \
		CTYPE v; \
		memcpy(&v, mem + ea, SZ); \
		r0 = (RESULT_EXPR); \
		sp[CK(pc[1].u32)].i64 = r0; \
		NEXT(2); \
	}
LOAD_ST(Hi32_load_st,    int32_t,  as_i64(v), 4)
LOAD_ST(Hi32_load8_u_st, uint8_t,  as_i64((int32_t)v), 1)
LOAD_ST(Hi32_load16_u_st,uint16_t, as_i64((int32_t)v), 2)
LOAD_ST(Hi32_load16_s_st,int16_t,  as_i64((int32_t)v), 2)
/* store: 値=r0、アドレス=sp[A]。operands[slot_a(addr), offset]。 */
#define STORE(NAME, CTYPE, SZ) \
	static int64_t NAME(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		uint64_t ea = (uint64_t)(uint32_t)sp[CK(pc[0].u32)].i32 + pc[1].u32; \
		if(ea + (SZ) > g_rt->mem_size) { \
			core_trap("oob store"); \
			return 0; \
		} \
		CTYPE v = (CTYPE)r0; \
		memcpy(mem + ea, &v, SZ); \
		NEXT(2); \
	}
STORE(Hi32_store,   int32_t,  4)
STORE(Hi32_store8,  int8_t,   1)
STORE(Hi32_store16, int16_t,  2)
STORE(Hi64_store,   int64_t,  8)
STORE(Hf64_store,   int64_t,  8)
STORE(Hi64_store8,  int8_t,   1)
STORE(Hi64_store16, int16_t,  2)
STORE(Hi64_store32, int32_t,  4)
/* multi-memory / memory64 load/store (memidx>0 または is_64)。kind=wasm opcode。
 * memidx=0 の memory32 は上の高速路を使う (cold)。offset word は u64 (memory64 の 64bit offset)。
 * load: operands[memidx, offset(u64), kind]、addr=r0。store: operands[memidx, addr_slot, offset(u64), kind]、val=r0。 */
static int64_t H_load_memN(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t midx = pc[0].u32;
	uint32_t kind = pc[2].u32;
	uint64_t off = pc[1].u64;
	(void)mem;
	uint8_t* mb = g_rt->mems[midx].base;
	uint64_t msz = g_rt->mems[midx].size;
	/* memory64 はアドレス全 64bit が有効、memory32 は下位 32bit (i32 は符号拡張済みのため切詰め必須)。
	 * a+off は u64 で wrap しうるので分割検査 (msz ≤ 4GB なので以後の ea+SZ は wrap しない)。 */
	uint64_t a = g_rt->mems[midx].is_64 ? (uint64_t)r0 : (uint64_t)(uint32_t)r0;
	if(off > msz || a > msz - off) {
		core_trap("oob load");
		return 0;
	}
	uint64_t ea = a + off;
	#define LDN(SZ, CTYPE, EXPR) do { \
		if(ea + (SZ) > msz) { \
			core_trap("oob load"); \
			return 0; \
		} \
		CTYPE v; \
		memcpy(&v, mb + ea, SZ); \
		r0 = (EXPR); \
	} while(0)
	switch(kind) {
	case 0x28: case 0x2a: LDN(4, int32_t, as_i64(v)); break;
	case 0x2c: LDN(1, int8_t, as_i64((int32_t)v)); break;
	case 0x2d: LDN(1, uint8_t, as_i64((int32_t)v)); break;
	case 0x2e: LDN(2, int16_t, as_i64((int32_t)v)); break;
	case 0x2f: LDN(2, uint16_t, as_i64((int32_t)v)); break;
	case 0x29: case 0x2b: LDN(8, int64_t, v); break;
	case 0x30: LDN(1, int8_t, (int64_t)v); break;
	case 0x31: LDN(1, uint8_t, (int64_t)v); break;
	case 0x32: LDN(2, int16_t, (int64_t)v); break;
	case 0x33: LDN(2, uint16_t, (int64_t)v); break;
	case 0x34: LDN(4, int32_t, (int64_t)v); break;
	case 0x35: LDN(4, uint32_t, (int64_t)v); break;
	}
	#undef LDN
	NEXT(3);
}
static int64_t H_store_memN(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t midx = pc[0].u32;
	uint32_t as = pc[1].u32;
	uint32_t kind = pc[3].u32;
	uint64_t off = pc[2].u64;
	(void)mem;
	uint8_t* mb = g_rt->mems[midx].base;
	uint64_t msz = g_rt->mems[midx].size;
	uint64_t a = g_rt->mems[midx].is_64 ? (uint64_t)sp[CK(as)].i64 : (uint64_t)(uint32_t)sp[CK(as)].i32;
	if(off > msz || a > msz - off) {
		core_trap("oob store");
		return 0;
	}
	uint64_t ea = a + off;
	#define STN(SZ, CTYPE) do { \
		if(ea + (SZ) > msz) { \
			core_trap("oob store"); \
			return 0; \
		} \
		CTYPE v = (CTYPE)r0; \
		memcpy(mb + ea, &v, SZ); \
	} while(0)
	switch(kind) {
	case 0x36: case 0x38: STN(4, int32_t); break;
	case 0x3a: STN(1, int8_t); break;
	case 0x3b: STN(2, int16_t); break;
	case 0x37: case 0x39: STN(8, int64_t); break;
	case 0x3c: STN(1, int8_t); break;
	case 0x3d: STN(2, int16_t); break;
	case 0x3e: STN(4, int32_t); break;
	}
	#undef STN
	NEXT(4);
}

/* ───────────────────────── 制御フロー ───────────────────────── */
/* 結果値の運搬: arity 1 の分岐は「結果は r0、分岐先 block は r0 から受ける」規約で運搬する。
 * 多値 (arity>=2) は compiler が分岐前に結果 slot 群を target base へコピーする (carry_n_to_base)。 */
static int64_t H_br(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	const coreinstr* t = pc[0].tgt;
	CORE_MUSTTAIL return t->op(t + 1, sp, mem, r0);
}
static int64_t H_br_if(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	/* cond は r0。br_if は cond を pop する。分岐時/非分岐時とも cond は消える。
	 * 分岐後の値スタック top は別 slot から来るため、ここで r0 を復元はしない
	 * (compiler が br_if 前に値を slot に確定させる)。 */
	int32_t c = (int32_t)r0;
	if(c) {
		const coreinstr* t = pc[0].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
	}
	NEXT(1);
}
/* br_if_not: cond==0 のとき分岐 (if の then skip 用)。 */
static int64_t H_br_if_not(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	int32_t c = (int32_t)r0;
	if(!c) {
		const coreinstr* t = pc[0].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
	}
	NEXT(1);
}
/* br_if_v: arity-1 ブロックへの br_if。値=r0、cond=sp[pc[1]]。operands[target, cond_slot]。
 * 分岐時は r0(値) を運び、非分岐時も r0(値) を残す (両経路で block 結果/fall-through 値を保持)。 */
static int64_t H_br_if_v(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	int32_t c = sp[CK(pc[1].u32)].i32;
	if(c) {
		const coreinstr* t = pc[0].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, r0);
	}
	NEXT(2);
}
/* br_table: operands[count, default_tgt, tgt0, tgt1, ...]。index=r0。 */
/* operands[idx_slot, n, default_tgt, case_tgt_0.., case_tgt_{n-1}]。index は slot から読み、
 * r0 は arity-1 の運搬値 (compiler が値を r0 に確定済) として target へ渡す。 */
static int64_t H_br_table(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t idx = (uint32_t)sp[pc[0].u32].i32;
	uint32_t n = pc[1].u32;
	const coreinstr* t = (idx < n) ? pc[3 + idx].tgt : pc[2].tgt;
	CORE_MUSTTAIL return t->op(t + 1, sp, mem, r0);
}
/* return / 関数末尾 end: r0 を結果として plain return (musttail 連鎖を巻き戻す)。 */
static int64_t H_return(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	(void)pc;
	(void)sp;
	(void)mem;
	return r0;
}
/* unreachable: trap する。 */
static int64_t H_unreachable(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	(void)pc;
	(void)sp;
	(void)mem;
	(void)r0;
	core_trap("unreachable");
	return 0;
}

/* multi-value 結果バッファ。callee の ret_multi が書き、caller の mret_get が読む。
 * 直後に読むので nest しても安全 (各 mret_get は対応 call 復帰直後に消費)。
 * CORE_MAX_MRET は kw_core.h で定義 (marshalling 側と共有)。 */
int64_t g_core_mret[CORE_MAX_MRET];
/* 多値 return: R 個の結果を sp[base+k] からグローバル mret バッファへ書き出して plain return。
 * operands[R, base_slot]。戻り値 (r0) は呼出側で無視される。 */
static int64_t H_ret_multi(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t n = pc[0].u32;
	uint32_t base = pc[1].u32;
	(void)mem;
	(void)r0;
	for(uint32_t k = 0; k < n && k < CORE_MAX_MRET; k++)
		g_core_mret[k] = sp[base + k].i64;

	return 0;
}
/* call 復帰直後: mret バッファの R 個を caller の結果 slot [base..) へ取り込む。
 * operands[R, base_slot]。 */
static int64_t H_mret_get(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t n = pc[0].u32;
	uint32_t base = pc[1].u32;
	for(uint32_t k = 0; k < n && k < CORE_MAX_MRET; k++)
		sp[base + k].i64 = g_core_mret[k];

	NEXT(2);
}

/* drop: top を捨てる。compiler が r0/slot を調整するのでここは NEXT のみ。 */
static int64_t H_drop(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	NEXT(0);
}
/* select: cond=r0、operands[slot_a, slot_b]。c? sp[A] : sp[B] → r0。 */
static int64_t H_select(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int32_t)r0 ? sp[CK(pc[0].u32)].i64 : sp[CK(pc[1].u32)].i64;
	NEXT(2);
}

/* global get/set。operands[globalidx]。store の実体スロット (globalinstance.val.num、coreval_t と i64 互換)
 * を index 経由で読み書き (realloc 安全、own/imported とも live 共有、testsuite get とも一致)。inline。 */
static int64_t H_global_get(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	store_t* S = (store_t*)g_rt->store_ref;
	r0 = S->globals.data[g_rt->global_addrs[pc[0].u32]].val.num.i64;
	NEXT(1);
}
static int64_t H_global_set(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	store_t* S = (store_t*)g_rt->store_ref;
	S->globals.data[g_rt->global_addrs[pc[0].u32]].val.num.i64 = r0;
	NEXT(1);
}

/* ═══════════════ suspend / resume (ホスト関数 yield) ═══════════════
 * ホスト関数が非 success (例: MeEngine の ERR_NEXTFRAME_YIELD) を返したとき、core を抜けて
 * 後で kw_core_resume で再開する。起爆は core_call_host (host 非success) のみ = コールドパス。
 * suspend は g_rt->trapped を流用 (EH と同じく既存の `if(trapped)` 分岐で拾う) → ホットパス無影響。
 * 各 call op は trapped 分岐内で「現フレームの再開点 (pc, sp)」を g_resume_chain へ deepest-first で
 * 積む。resume は最深から順に再開し各結果を親の r0 へ送る (kw_core_resume)。 */
/* eh_dispatch: フレームの call が try 内 (call_eh / call_indirect_eh) の場合の catch dispatch 先。
 * resume 中に子フレームから wasm 例外が伝播してきたとき、通常経路の H_call_eh の g_exc_pending
 * 分岐に相当する処理を kw_core_resume が行うために保存する (NULL = 非 EH call、例外は素通し)。
 * saved_caught は call 時点の g_caught_sp (watermark)。dispatch 起動前に巻き戻して子フレーム内の
 * 残骸を清算する (H_call_eh の saved_caught と同じ役割)。 */
/* core_resume_frame_t は kw_core.h に移動 (bridge の再入処理から参照するため) */
extern corefunc_t* g_compiled;   /* def func idx → corefunc_t (定義は kw_core_bridge.c) */
core_resume_frame_t g_resume_chain[1024];
int       g_resume_n = 0;            /* チェーン段数 (0 = 非suspend)。index 0 = 最深。 */
int       g_suspended = 0;           /* host yield 伝播中 (trapped と併用)。 */
int       g_suspend_code = 0;        /* host が返した code (ERR_NEXTFRAME_YIELD 等)。caller へ伝播。 */
int64_t   g_suspend_host_r0 = 0;     /* yield した host 呼び出しの結果 (resume の最深 r0)。 */
int       g_suspend_num_results = 0; /* 元 invoke した entry 関数の結果数 (完走時の marshalling 用)。 */

/* call op の trapped 分岐内から呼ぶ: 現フレームの再開点を積む。チェーン溢れは suspend を
 * 中止し trap 化 (再開不能)。resume_pc = call の継続位置 (NEXT の飛び先)。
 * saved_caught は EH call の watermark (非 EH call は 0 を渡す、eh_dispatch=NULL なら未使用)。
 * noinline: suspend したときしか通らないコールドパスなので、call 系ハンドラ 4 種へ展開させると
 * ホットパスの .text を膨らませるだけになる。 */
CORE_NOINLINE static void core_suspend_push(const coreinstr* resume_pc, coreval_t* sp, const coreinstr* eh_dispatch, int saved_caught)
{
	if(g_resume_n < (int)(sizeof(g_resume_chain)/sizeof(g_resume_chain[0]))) {
		g_resume_chain[g_resume_n].pc = resume_pc;
		g_resume_chain[g_resume_n].sp = sp;
		g_resume_chain[g_resume_n].eh_dispatch = eh_dispatch;
		g_resume_chain[g_resume_n].saved_caught = saved_caught;
		/* 実行中インスタンス。cross-module 呼出の callee 側フレームは呼出元と別インスタンスで、
		 * resume までに g_rt/g_compiled は呼出元へ戻ってしまうためここで控える。 */
		g_resume_chain[g_resume_n].rt = g_rt;
		g_resume_chain[g_resume_n].compiled = g_compiled;
		g_resume_n++;
	} else {
		g_suspended = 0;
		core_trap("resume chain overflow");
	}
}

/* A yielding tail call has no ordinary continuation. Resume at a plain return
 * so the host result reaches the caller (including a top-level tail call). */
CORE_NOINLINE static int64_t core_tail_result(int64_t result, coreval_t* sp)
{
	if(g_suspended) {
		static const coreinstr resume_return = { H_return };
		core_suspend_push(&resume_return, sp, NULL, 0);
	}
	return result;
}

/* ───────────────────────── call / call_indirect ───────────────────────── */
/* 呼出規約: 引数は呼出 op 直前に連続 slot [arg_base .. arg_base+nargs) に確定済み。
 * callee の sp window = &sp[frame_slots] (= caller フレーム末尾)。引数をコピーし core_run。
 * 結果 (単一) を r0 で受け取り継続。operands[funcidx, arg_base, frame_slots, nargs]。 */
/* cross-module 呼出のヘルパ (kw_core_bridge.c)。ファイルスコープで宣言する (ブロックスコープの
 * 関数 extern は MSVC で C4210 → /WX エラーになるため)。 */
void kw_core_import_target(void* target, corert_t** out_rt, corefunc_t** out_compiled);
int  kw_core_resolve_funcaddr(int32_t fa, corert_t** out_rt, corefunc_t** out_compiled, uint32_t* out_def_idx, uint64_t* out_sig);
static int g_depth = 0;
static long g_calls = 0;

/* kw_core_invoke / kw_core_resume の入れ子数。core の実行は必ずこの 2 つから入るので、
 * > 0 なら vstack の主領域は使用中。g_depth / g_resume_n だけでは取りこぼしがあった:
 *  - do_call は import (host 関数) 呼出しで g_depth を増やさずに戻るので、
 *    top-level フレームが直接 host を呼んだ場合は g_depth == 0
 *  - kw_core_resume は最外側フレームを pop してから再開するので、
 *    その間は g_resume_n == 0
 * どちらも "実行中なのに非再入と見なす" 誤判定になる。 */
int g_core_exec_active = 0;

/* kw_core_invoke の再入検出用。core 実行中 (ホスト関数の中から呼ばれた) か、
 * host yield で中断中なら真。どちらの場合も vstack 主領域は使用中なので、
 * 新しい実行をそこから始めてはいけない。 */
int kw_core_is_executing(void)
{
	return (g_core_exec_active > 0 || g_depth > 0 || g_resume_n > 0);
}

/* 診断用。旧実装の再入判定 (g_depth / g_resume_n のみ) の結果を bridge から読む窓。
 * これが 0 なのに実際は実行中、という組み合わせが「旧実装が見抜けなかった再入」。 */
int kw_core_legacy_exec_hint(void)
{
	return (g_depth > 0 || g_resume_n > 0);
}

/* 直近 call の ring buffer (func_idx, 第1引数) — トラップ時に呼出チェーン表示 */
uint32_t g_ring_fn[64];
int64_t g_ring_a0[64];
int g_ring_pos = 0;
/* この index 以上の関数は ring に記録しない (0 = 全記録)。
 * SAFE_HEAP 有効時は Binaryen 生成のヘルパ呼び出しで ring が埋まり、アプリ側の
 * 呼出チェーンが見えなくなる。ヘルパは元の関数群より後ろに追加されるので、
 * name section のエントリ数を閾値にすると除外できる。
 * ホスト側 (extrafunction.c) が環境変数 KW_RING_SKIP_FROM から設定する。 */
uint32_t g_ring_skip_from = 0;
/* ring に記録しない func_idx (最大 8 個)。未使用要素は 0xFFFFFFFF。
 * SAFE_HEAP のチェックが毎回呼ぶ emscripten_get_sbrk_ptr のような
 * 「名前付きだがノイズになる関数」を落とすために使う。
 * ホスト側 (extrafunction.c) が環境変数 KW_RING_EXCLUDE から設定する。 */
uint32_t g_ring_exclude[8] = {
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
	0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
};
/* コールスタック (depth → func_idx, 第1引数) */
uint32_t g_cs_fn[512];
int64_t g_cs_a0[512];
/* 関数別呼出回数 (histogram) */
long g_callcount[256];
/* cross-module 呼出: 対象インスタンス (tgt_rt/tgt_compiled) の def_idx 関数を、引数を callee window へ
 * コピーして実行する。g_rt/g_compiled を一時的に切り替え、trap を呼出元へ伝播する。import 解決呼出
 * (do_call) と共有テーブル call_indirect (H_call_indirect の funcaddr 解決) が共有する。 */
static int64_t do_call_cross(corert_t* tgt_rt, corefunc_t* tgt_compiled, uint32_t def_idx,
	coreval_t* sp, coreval_t* arg_src, uint32_t nargs, uint32_t caller_frame_slots)
{
	corefunc_t* callee = &tgt_compiled[def_idx];
	coreval_t* callee_sp = sp + caller_frame_slots;
	if(++g_depth > 2000 || callee_sp + callee->num_slots > tgt_rt->vstack + tgt_rt->vstack_slots) {
		core_trap("runaway/overflow");
		g_depth--;
		return 0;
	}
	g_calls++;
	for(uint32_t i = 0; i < nargs; i++)
		callee_sp[i].i64 = arg_src[i].i64;

	for(uint32_t i = nargs; i < callee->num_slots; i++)
		callee_sp[i].i64 = 0;

	corert_t* srt = g_rt;
	corefunc_t* sc = g_compiled;
	g_rt = tgt_rt;
	g_compiled = tgt_compiled;
	tgt_rt->trapped = 0;   /* 別インスタンスの前回 invoke/assert_trap で残った stale trap をクリア */
	tgt_rt->trap_msg = NULL;
	int64_t r = core_run(callee->entry, callee_sp, tgt_rt->mem);
	int tr = tgt_rt->trapped;
	const char* tmsg = tgt_rt->trap_msg;
	g_rt = srt;
	g_compiled = sc;
	/* trap 伝播 */
	if(tr) {
		g_rt->trapped = 1;
		g_rt->trap_msg = tmsg;
	}
	g_depth--;
	return r;
}
static int64_t do_call(uint32_t func_idx, coreval_t* sp, uint8_t* mem, coreval_t* arg_src, uint32_t nargs, uint32_t caller_frame_slots, int64_t r0_pass)
{
	uint32_t nimp = g_rt->mod->num_imported_funcs;
	if(func_idx < nimp) {
		/* cross-module linking: 別 core インスタンスの関数に解決済なら instance 切替で呼ぶ。
		 * 未解決 (target=NULL) は host (WASI)。 */
		struct coreimpfn* imp = (func_idx < g_rt->num_import_funcs) ? &g_rt->import_funcs[func_idx] : NULL;
		if(imp == NULL || imp->target == NULL)
			return core_call_host(func_idx, arg_src, mem);

		corert_t* tgt_rt;
		corefunc_t* tgt_compiled;
		kw_core_import_target(imp->target, &tgt_rt, &tgt_compiled);
		return do_call_cross(tgt_rt, tgt_compiled, imp->target_fidx, sp, arg_src, nargs, caller_frame_slots);
	}
	uint32_t def = func_idx - nimp;
	corefunc_t* callee = &g_compiled[def];
	coreval_t* callee_sp = sp + caller_frame_slots;
	/* vstack オーバーフロー / 暴走再帰ガード */
	if(++g_depth > 2000 || callee_sp + callee->num_slots > g_rt->vstack + g_rt->vstack_slots) {
		fprintf(stderr, "[core] runaway: func=%u def=%u depth=%d calls=%ld slots=%u\n",
			func_idx, def, g_depth, g_calls, callee->num_slots);
		core_trap("runaway/overflow");
		g_depth--;
		return 0;
	}
	g_calls++;
#if defined(CORE_DEBUG)
	long myid = g_calls;
	if(g_ring_skip_from == 0 || func_idx < g_ring_skip_from) {
		int _ex;
		int _skip = 0;
		for(_ex = 0; _ex < 8; _ex++) {
			if(g_ring_exclude[_ex] == func_idx) { _skip = 1; break; }
		}
		if(!_skip) {
			g_ring_fn[g_ring_pos & 63] = func_idx;
			g_ring_a0[g_ring_pos & 63] = (nargs > 0 ? arg_src[0].i64 : -1);
			g_ring_pos++;
		}
	}
	if(g_depth < 512) {
		g_cs_fn[g_depth] = func_idx;
		g_cs_a0[g_depth] = (nargs > 0 ? arg_src[0].i64 : -1);
	}
	if(func_idx < 256)
		g_callcount[func_idx]++;

	if(myid <= 30)
		fprintf(stderr, "[core] call#%ld ENTER func=%u def=%u nargs=%u\n", myid, func_idx, def, nargs);

	uint32_t saved_slots = g_cur_slots;
	g_cur_slots = callee->num_slots;
#endif
	/* 引数コピー (caller の arg slots → callee の locals 0..nargs)。 */
	for(uint32_t i = 0; i < nargs; i++)
		callee_sp[i].i64 = arg_src[i].i64;

	/* 宣言ローカルを 0 クリア */
	for(uint32_t i = nargs; i < callee->num_slots; i++)
		callee_sp[i].i64 = 0;

	(void)r0_pass;
	(void)def;
	int64_t r = core_run(callee->entry, callee_sp, mem);
#if defined(CORE_DEBUG)
	g_cur_slots = saved_slots;
	if(myid <= 30)
		fprintf(stderr, "[core] call#%ld LEAVE func=%u ret=%lld\n", myid, func_idx, (long long)r);
#endif
	g_depth--;
	return r;
}
static int64_t H_call(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t func_idx = pc[0].u32;
	uint32_t arg_base = pc[1].u32;
	uint32_t frame_slots = pc[2].u32;
	uint32_t nargs = pc[3].u32;
	/* 末尾引数が r0 にある場合に備え、compiler は arg を slot に確定させてから call を出す。 */
	int64_t res = do_call(func_idx, sp, mem, sp + arg_base, nargs, frame_slots, r0);
	if(g_rt->trapped) {
		if(g_suspended)
			core_suspend_push(pc + 4, sp, NULL, 0);

		return 0;
	}
	r0 = res;
	NEXT(4);
}
static int64_t H_call_indirect(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	/* table index = r0。operands[typeidx, arg_base, frame_slots, nargs]。
	 * 範囲外は "undefined element"、null (未初期化=−1) は "uninitialized element" で trap。 */
	uint32_t ti = pc[4].u32;   /* tableidx (multi-table)。0 は高速路 g_rt->table。 */
	int32_t* td = (ti == 0) ? g_rt->table : g_rt->tables[ti].data;
	uint32_t tsz = (ti == 0) ? g_rt->table_size : g_rt->tables[ti].size;
	uint32_t tidx = (uint32_t)r0;
	if(tidx >= tsz) {
		core_trap("undefined element");
		return 0;
	}
	int32_t fi = td[tidx];
	uint8_t gref = (ti == 0) ? g_rt->table_global_ref : g_rt->tables[ti].global_ref;
	if(fi < 0) {
		/* -1=null は trap。-2 以下は非共有テーブルに入った foreign funcref (encoded funcaddr):
		 * funcaddr 解決経路 (gref) で呼ぶ。共有テーブル (funcaddr domain) に -2 以下は入らない。 */
		if(gref || fi == -1) {
			core_trap("uninitialized element");
			return 0;
		}
		gref = 1;
		fi = -2 - fi;
	}
	uint32_t arg_base = pc[1].u32;
	uint32_t frame_slots = pc[2].u32;
	uint32_t nargs = pc[3].u32;
	uint32_t typeidx = pc[0].u32;
	/* cross-module 共有テーブル (global_ref): 要素は store funcaddr。定義インスタンスを解決して
	 * 別モジュールの関数も呼べる。非共有 (CoreMark 等) は gfi 直呼びで高速。
	 * 末尾 NEXT(5) を単一にするため res を一旦受けて最後に dispatch する (MSVC musttail C4737 回避)。 */
	int64_t res;
	if(gref) {
		corert_t* tgt_rt;
		corefunc_t* tgt_compiled;
		uint32_t didx;
		uint64_t sig = 0;
		/* core 未定義 (host 等) */
		if(!kw_core_resolve_funcaddr(fi, &tgt_rt, &tgt_compiled, &didx, &sig)) {
			core_trap("uninitialized element");
			return 0;
		}
		if(typeidx < g_rt->num_type_sigs && sig != 0 && sig != g_rt->type_sigs[typeidx]) {
			core_trap("indirect call type mismatch");
			return 0;
		}
		res = do_call_cross(tgt_rt, tgt_compiled, didx, sp, sp + arg_base, nargs, frame_slots);
	} else {
		/* 型チェック: table 要素の実際型 (func_sigs[fi]) が期待型 (type_sigs[typeidx]) と一致するか。
		 * func_sig=0 (型不明=host/未解決 import) はチェックを省く (誤検知回避)。 */
		if((uint32_t)fi < g_rt->num_func_sigs && typeidx < g_rt->num_type_sigs
			&& g_rt->func_sigs[fi] != 0 && g_rt->func_sigs[fi] != g_rt->type_sigs[typeidx]) {
			core_trap("indirect call type mismatch");
			return 0;
		}
		res = do_call((uint32_t)fi, sp, mem, sp + arg_base, nargs, frame_slots, 0);
	}
	if(g_rt->trapped) {
		if(g_suspended)
			core_suspend_push(pc + 5, sp, NULL, 0);

		return 0;
	}
	r0 = res;
	NEXT(5);
}

/* return_call (tail call): 現フレームを再利用して callee へ musttail。深い tail 再帰でも C スタック/
 * vstack を伸ばさない (count(1000000) 等)。operands[funcidx, arg_base, caller_slots, nargs]。
 * 同モジュール定義関数のみ frame 再利用、import/host は通常 call+結果返却にフォールバック。 */
static int64_t H_return_call(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t fi = pc[0].u32;
	uint32_t arg_base = pc[1].u32;
	uint32_t caller_slots = pc[2].u32;
	uint32_t na = pc[3].u32;
	uint32_t nimp = g_rt->mod->num_imported_funcs;
	if(fi >= nimp) {
		corefunc_t* callee = &g_compiled[fi - nimp];
		if(sp + callee->num_slots > g_rt->vstack + g_rt->vstack_slots) {
			core_trap("runaway/overflow");
			return 0;
		}
		/* 引数を現フレーム先頭へ (overlap可) */
		if(na > 0 && arg_base != 0)
			memmove(sp, sp + arg_base, (size_t)na * sizeof(coreval_t));

		/* 残りローカル 0 クリア */
		for(uint32_t i = na; i < callee->num_slots; i++)
			sp[i].i64 = 0;

		const coreinstr* e = callee->entry;
		CORE_MUSTTAIL return e->op(e + 1, sp, mem, 0); /* sp 据置 = frame 再利用、musttail = C スタック据置 */
	}
	int64_t res = do_call(fi, sp, mem, sp + arg_base, na, caller_slots, 0);   /* import/host は通常呼出し */
	return core_tail_result(res, sp);
}

/* return_call_indirect (tail call): table[r0] を解決し return_call と同様に frame 再利用。
 * operands[typeidx, arg_base, caller_slots, nargs, tableidx]。 */
static int64_t H_return_call_indirect(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t typeidx = pc[0].u32;
	uint32_t arg_base = pc[1].u32;
	uint32_t caller_slots = pc[2].u32;
	uint32_t na = pc[3].u32;
	uint32_t ti = pc[4].u32;
	int32_t* td = (ti == 0) ? g_rt->table : g_rt->tables[ti].data;
	uint32_t tsz = (ti == 0) ? g_rt->table_size : g_rt->tables[ti].size;
	uint32_t tidx = (uint32_t)r0;
	if(tidx >= tsz) {
		core_trap("undefined element");
		return 0;
	}
	int32_t fi = td[tidx];
	uint8_t gref = (ti == 0) ? g_rt->table_global_ref : g_rt->tables[ti].global_ref;
	if(fi < 0) {
		/* -2 以下は foreign funcref (encoded funcaddr) → funcaddr 解決経路 (H_call_indirect と同様)。 */
		if(gref || fi == -1) {
			core_trap("uninitialized element");
			return 0;
		}
		gref = 1;
		fi = -2 - fi;
	}
	if(gref) {
		/* cross-module 共有テーブル: 定義インスタンスを解決して通常 cross-call+結果返却 (frame 再利用せず)。 */
		corert_t* tgt_rt;
		corefunc_t* tgt_compiled;
		uint32_t didx;
		uint64_t sig = 0;
		if(!kw_core_resolve_funcaddr(fi, &tgt_rt, &tgt_compiled, &didx, &sig)) {
			core_trap("uninitialized element");
			return 0;
		}
		if(typeidx < g_rt->num_type_sigs && sig != 0 && sig != g_rt->type_sigs[typeidx]) {
			core_trap("indirect call type mismatch");
			return 0;
		}
		int64_t res = do_call_cross(tgt_rt, tgt_compiled, didx, sp, sp + arg_base, na, caller_slots);
		return core_tail_result(res, sp);
	}
	if((uint32_t)fi < g_rt->num_func_sigs && typeidx < g_rt->num_type_sigs
		&& g_rt->func_sigs[fi] != 0 && g_rt->func_sigs[fi] != g_rt->type_sigs[typeidx]) {
		core_trap("indirect call type mismatch");
		return 0;
	}
	uint32_t nimp = g_rt->mod->num_imported_funcs;
	if((uint32_t)fi >= nimp) {
		corefunc_t* callee = &g_compiled[fi - nimp];
		if(sp + callee->num_slots > g_rt->vstack + g_rt->vstack_slots) {
			core_trap("runaway/overflow");
			return 0;
		}
		if(na > 0 && arg_base != 0)
			memmove(sp, sp + arg_base, (size_t)na * sizeof(coreval_t));

		for(uint32_t i = na; i < callee->num_slots; i++)
			sp[i].i64 = 0;

		const coreinstr* e = callee->entry;
		CORE_MUSTTAIL return e->op(e + 1, sp, mem, 0);
	}
	int64_t res = do_call((uint32_t)fi, sp, mem, sp + arg_base, na, caller_slots, 0);
	return core_tail_result(res, sp);
}

/* ═══════════════ 例外処理 (EH: try_table / throw / catch) ═══════════════
 * 例外伝播 = trapped + g_exc_pending の二重フラグ。throw は g_rt->trapped も立てるので、ホットパスの
 * 既存 `if(trapped) return` がそのまま例外伝播に流用でき CoreMark は無影響 (例外用の追加 check 無し)。
 * catch 判定 (trapped かつ g_exc_pending か) は try 内専用の call op (H_call_eh) と dispatch でのみ行う。 */
int       g_exc_pending = 0; /* 伝播中の例外あり (trapped と併用) */
int32_t   g_exc_tagaddr = 0; /* 例外の global tagaddr (cross-module identity) */
uint32_t  g_exc_nvals = 0;   /* 例外の値の数 */
coreval_t* g_exc_vals = NULL; /* Full tag payload, allocated from the executing store. */
uint32_t  g_exc_capacity = 0;

/* legacy rethrow 用「捕捉済み例外」スタック。catch/catch_all が match した時点で例外 (tagaddr+値) を
 * push し、handler が正常終了する位置 (caught_pop) で pop する。rethrow N は lexical nesting から算出した
 * 深さ J で g_caught[sp-1-J] を再 throw する。構造化制御では各実行経路で push/pop が釣り合う
 * (handler を非局所脱出する経路は例外伝播のみで、その後同一 invoke 内で rethrow が読むことはない)。
 * top-level invoke 毎に g_caught_sp=0 にリセットする (kw_core_invoke)。 */
typedef struct {
	int32_t   tagaddr;
	uint32_t  nvals;
	uint32_t  capacity;
	coreval_t* vals;
} core_caught_t;
core_caught_t g_caught[64];
int           g_caught_sp = 0;

/* exnref (try_table catch_ref/catch_all_ref) 専用ストレージ。legacy rethrow 台帳 (g_caught) とは
 * 物理分離する。exnref は値としてフレームを跨いで生存しうるため、handler 正常終了時の pop も
 * call_eh/resume の watermark 巻き戻しも行わず、invoke 毎リセットのみで管理する。handle=index+1
 * (-1=null、他の参照型と共通)。分離により try_table catch_ref の push が外側 legacy rethrow N の lexical J をズラさない。 */
core_caught_t g_exn[64];
int           g_exn_sp = 0;

/* Payload buffers survive catches and yields. End/reset frees them while their
 * externally supplied arena is still alive; nested invokes retain outer entries. */
void kw_core_clear_exceptions(int caught_base, int exn_base)
{
	kinowasm_mem_free(g_exc_vals);
	g_exc_vals = NULL;
	g_exc_capacity = 0;
	g_exc_nvals = 0;
	g_exc_pending = 0;
	for(int i = caught_base; i < 64; i++) {
		kinowasm_mem_free(g_caught[i].vals);
		memset(&g_caught[i], 0, sizeof(g_caught[i]));
	}
	for(int i = exn_base; i < 64; i++) {
		kinowasm_mem_free(g_exn[i].vals);
		memset(&g_exn[i], 0, sizeof(g_exn[i]));
	}
	g_caught_sp = caught_base;
	g_exn_sp = exn_base;
}

static int exception_reserve(coreval_t** values, uint32_t* capacity, uint32_t count)
{
	if(count <= *capacity)
		return 1;

	if((uint64_t)count <= SIZE_MAX / sizeof(coreval_t)) {
		/* A host callback can leave a different allocation arena selected. */
		kinowasm_mem_info_t saved_memory = kinowasm_mem_get_info();
		kinowasm_mem_set_info(((store_t*)g_rt->store_ref)->storememory);
		coreval_t* data = kinowasm_mem_realloc(*values, (size_t)count * sizeof(coreval_t));
		kinowasm_mem_set_info(saved_memory);
		if(data != NULL) {
			*values = data;
			*capacity = count;
			return 1;
		}
	}
	g_exc_pending = 0;
	core_trap("exception payload allocation failed");
	return 0;
}

static int exception_save(core_caught_t* exception)
{
	if(!exception_reserve(&exception->vals, &exception->capacity, g_exc_nvals))
		return 0;

	exception->tagaddr = g_exc_tagaddr;
	exception->nvals = g_exc_nvals;
	for(uint32_t i = 0; i < g_exc_nvals; i++)
		exception->vals[i] = g_exc_vals[i];

	return 1;
}

static int exception_restore(const core_caught_t* exception)
{
	if(!exception_reserve(&g_exc_vals, &g_exc_capacity, exception->nvals))
		return 0;

	g_exc_tagaddr = exception->tagaddr;
	g_exc_nvals = exception->nvals;
	for(uint32_t i = 0; i < g_exc_nvals; i++)
		g_exc_vals[i] = exception->vals[i];

	return 1;
}

/* suspend した core 実行を再開する。最深フレーム (chain[0]) から順に再開し、各フレームの結果を
 * 親の r0 へ送る。再 yield 時はチェーンを再伸長して 2 を返す。完走で 0 + ret に最終結果、resume 中
 * trap で 1。ret は呼出側が結果数分用意。実行インスタンス (g_rt/g_compiled/mem) はフレームごとに
 * 復元する (cross-module 呼出の途中で中断するとフレーム間でインスタンスが異なるため)。 */
static int core_resume_body(int64_t* ret);

int kw_core_resume(int64_t* ret)
{
	if(g_resume_n == 0)
		return 1; /* 再開対象なし */

	/* 再開中も core 実行中 = vstack 主領域使用中。最外側フレームの再開中は
	 * g_resume_n も g_depth も 0 になりうるので、ここで明示的に実行中を立てる。 */
	g_core_exec_active++;
	int r = core_resume_body(ret);
	g_core_exec_active--;
	if(r == 1) {
		/* A terminal trap invalidates the remaining parents. Keep the chain
		 * intact for a real yield, including yields from exception handlers. */
		g_resume_n = 0;
		g_suspended = 0;
	}
	if(r != 2)
		kw_core_clear_exceptions(0, 0);

	return r;
}

static int core_resume_body(int64_t* ret)
{
	uint8_t* mem = g_rt->mem;   /* フレーム復元で毎回上書きする (初期値は防御) */
	int64_t r0 = g_suspend_host_r0;
	int exc_from_child = 0; /* 直前 (子) フレームから wasm 例外が伝播中 */
	while(g_resume_n > 0) {
		/* 最深 (index 0) を取り出して前詰めで除去 (再 yield の新フレームが残り親より前に来るように)。 */
		core_resume_frame_t e = g_resume_chain[0];
		int rem = --g_resume_n;
		for(int i = 0; i < rem; i++)
			g_resume_chain[i] = g_resume_chain[i + 1];

		/* このフレームを実行していたインスタンスへ戻す。cross-module 呼出の途中で中断した
		 * チェーンは callee 側と呼出元でインスタンスが異なる (単一モジュールなら全段同値)。 */
		g_rt = e.rt;
		g_compiled = e.compiled;
		mem = e.rt->mem;

		int old_n = g_resume_n;
		/* A resumed frame starts a new execution step. Carry an actual child
		 * exception across modules, never the previous host-yield message. */
		g_suspended = 0;
		g_rt->trapped = exc_from_child;
		g_rt->trap_msg = exc_from_child ? "wasm exception" : NULL;
		if(exc_from_child) {
			/* 子フレームで捕捉されなかった例外: このフレームが try 内 call (call_eh) 由来なら
			 * catch dispatch を起動する (通常経路の H_call_eh の g_exc_pending 分岐に相当)。
			 * 非 EH call のフレームは巻き戻して (再開せず) さらに親へ伝播する。 */
			if(e.eh_dispatch == NULL)
				continue;

			exc_from_child = 0;
			g_caught_sp = e.saved_caught;   /* watermark: 子フレーム内の g_caught 残骸を清算 */
			r0 = e.eh_dispatch->op(e.eh_dispatch + 1, e.sp, mem, 0);
		} else {
			r0 = e.pc->op(e.pc + 1, e.sp, mem, r0);
		}
		if(g_rt->trapped) {
			if(g_suspended) {
				/* 再 yield: 新フレームが [old_n .. g_resume_n) へ append された。最深 (前方) へ
				 * 回転し、残りの親フレーム ([0..old_n)) より前に並べる。 */
				int newc = g_resume_n - old_n;
				if(newc > 0 && old_n > 0) {
					core_resume_frame_t tmp[1024];
					for(int i = 0; i < newc; i++)
						tmp[i] = g_resume_chain[old_n + i];

					for(int i = old_n - 1; i >= 0; i--)
						g_resume_chain[i + newc] = g_resume_chain[i];

					for(int i = 0; i < newc; i++)
						g_resume_chain[i] = tmp[i];
				}
				return 2; /* 再 suspend */
			}
			/* 未捕捉例外: 親フレームへ */
			if(g_exc_pending) {
				exc_from_child = 1;
				continue;
			}
			return 1; /* resume 中の trap */
		}
		/* このフレーム完走: r0 = 結果。次反復で親の r0 として渡す。 */
	}
	if(exc_from_child)
		return 1; /* 全フレームで捕捉されず → 未捕捉例外 (trap 扱いで caller へ) */

	/* 全フレーム完走 = 元 invoke 完了。 */
	if(ret) {
		if(g_suspend_num_results >= 2) {
			/* g_core_mret は CORE_MAX_MRET 要素。書き側 (ret_multi) も同上限でクランプするため、
			 * 読み側も揃える (num_results > 上限は公開 API が弾くが、直接呼出しへの防御)。 */
			for(int i = 0; i < g_suspend_num_results && i < CORE_MAX_MRET; i++)
				ret[i] = g_core_mret[i];
		} else if(g_suspend_num_results == 1) {
			ret[0] = r0;
		}
	}
	return 0;
}

/* host yield の伝播 code (kw_core_invoke/_resume が 2 を返したとき有効)。 */
int kw_core_suspend_code(void)
{
	return g_suspend_code;
}

/* throw: operands[tag_resolve_idx, nvals, val_slot0..N-1]。例外状態を立て、現関数から return で
 * 伝播 (呼出元の trapped check が拾う)。try 内で catch されるかは呼出元/dispatch が判定。 */
static int64_t H_throw(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ridx = pc[0].u32;
	uint32_t nv = pc[1].u32;
	g_exc_tagaddr = (ridx < g_rt->num_tagaddrs) ? g_rt->tagaddrs[ridx] : (int32_t)ridx;
	if(!exception_reserve(&g_exc_vals, &g_exc_capacity, nv))
		return 0;

	for(uint32_t i = 0; i < nv; i++)
		g_exc_vals[i] = sp[pc[2 + i].u32];

	g_exc_nvals = nv;
	g_exc_pending = 1;
	g_rt->trapped = 1;
	if(g_rt->trap_msg == NULL)
		g_rt->trap_msg = "wasm exception";

	return 0;   /* 伝播 (呼出元 trapped check が拾う)。直接 throw も関数を抜けて caller の try へ。 */
}

/* throw_local: 同一関数内の enclosing try (lexical) に直接捕捉される throw。例外状態を立て、現関数を
 * 抜けず dispatch へ musttail で分岐する (br 相当)。operands[tag_resolve_idx, nvals, val_slot0..N-1, target_tgt]。 */
static int64_t H_throw_local(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ridx = pc[0].u32;
	uint32_t nv = pc[1].u32;
	g_exc_tagaddr = (ridx < g_rt->num_tagaddrs) ? g_rt->tagaddrs[ridx] : (int32_t)ridx;
	if(!exception_reserve(&g_exc_vals, &g_exc_capacity, nv))
		return 0;

	for(uint32_t i = 0; i < nv; i++)
		g_exc_vals[i] = sp[pc[2 + i].u32];

	g_exc_nvals = nv;
	g_exc_pending = 1;
	g_rt->trapped = 1;
	if(g_rt->trap_msg == NULL)
		g_rt->trap_msg = "wasm exception";

	const coreinstr* t = pc[2 + nv].tgt; /* dispatch へ (catch chain 先頭) */
	CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
}

/* try 内の call: 例外が上がってきたら (trapped+g_exc_pending) catch dispatch へ musttail、
 * trap (g_exc_pending=0) は通常通り return で伝播。それ以外は通常継続。
 * operands[funcidx, arg_base, frame_slots, nargs, dispatch_tgt]。 */
static int64_t H_call_eh(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t func_idx = pc[0].u32;
	uint32_t arg_base = pc[1].u32;
	uint32_t frame_slots = pc[2].u32;
	uint32_t nargs = pc[3].u32;
	/* watermark: callee 内で handler を非局所脱出 (throw 伝播等) して pop されなかった g_caught
	 * 残骸を、catch 照合前に call 時点の深さへ巻き戻して清算する (rethrow の lexical J と台帳の
	 * 関数跨ぎ整合)。 */
	int saved_caught = g_caught_sp;
	int64_t res = do_call(func_idx, sp, mem, sp + arg_base, nargs, frame_slots, r0);
	if(g_rt->trapped) {
		/* host yield: 再開点 + catch dispatch を積む */
		if(g_suspended) {
			core_suspend_push(pc + 5, sp, pc[4].tgt, saved_caught);
			return 0;
		}
		if(g_exc_pending) {
			g_caught_sp = saved_caught;
			const coreinstr* t = pc[4].tgt;
			CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
		}
		return 0;   /* trap は伝播 */
	}
	r0 = res;
	NEXT(5);
}
static int64_t H_call_indirect_eh(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t typeidx = pc[0].u32;
	uint32_t arg_base = pc[1].u32;
	uint32_t frame_slots = pc[2].u32;
	uint32_t nargs = pc[3].u32;
	uint32_t ti = pc[4].u32;
	int32_t* td = (ti == 0) ? g_rt->table : g_rt->tables[ti].data;
	uint32_t tsz = (ti == 0) ? g_rt->table_size : g_rt->tables[ti].size;
	uint32_t tidx = (uint32_t)r0;
	if(tidx >= tsz) {
		core_trap("undefined element");
		return 0;
	}
	int32_t fi = td[tidx];
	uint8_t gref = (ti == 0) ? g_rt->table_global_ref : g_rt->tables[ti].global_ref;
	if(fi < 0) {
		/* -2 以下は非共有テーブル内の foreign funcref (encoded funcaddr) → funcaddr 解決経路 (H_call_indirect と同様)。 */
		if(gref || fi == -1) {
			core_trap("uninitialized element");
			return 0;
		}
		gref = 1;
		fi = -2 - fi;
	}
	int64_t res;
	int saved_caught = g_caught_sp;   /* watermark (H_call_eh と同様) */
	if(gref) {
		corert_t* tgt_rt;
		corefunc_t* tgt_compiled;
		uint32_t didx;
		uint64_t sig = 0;
		if(!kw_core_resolve_funcaddr(fi, &tgt_rt, &tgt_compiled, &didx, &sig)) {
			core_trap("uninitialized element");
			return 0;
		}
		if(typeidx < g_rt->num_type_sigs && sig != 0 && sig != g_rt->type_sigs[typeidx]) {
			core_trap("indirect call type mismatch");
			return 0;
		}
		res = do_call_cross(tgt_rt, tgt_compiled, didx, sp, sp + arg_base, nargs, frame_slots);
	} else {
		if((uint32_t)fi < g_rt->num_func_sigs && typeidx < g_rt->num_type_sigs
			&& g_rt->func_sigs[fi] != 0 && g_rt->func_sigs[fi] != g_rt->type_sigs[typeidx]) {
			core_trap("indirect call type mismatch");
			return 0;
		}
		res = do_call((uint32_t)fi, sp, mem, sp + arg_base, nargs, frame_slots, 0);
	}
	if(g_rt->trapped) {
		/* host yield: 再開点 + catch dispatch を積む */
		if(g_suspended) {
			core_suspend_push(pc + 6, sp, pc[5].tgt, saved_caught);
			return 0;
		}
		if(g_exc_pending) {
			g_caught_sp = saved_caught;
			const coreinstr* t = pc[5].tgt;
			CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
		}
		return 0;
	}
	r0 = res;
	NEXT(6);
}

/* 捕捉した例外を g_caught (legacy rethrow 台帳) へ退避し index を返す (-1=満杯)。legacy try の
 * catch/catch_all match 時に呼ぶ。exnref を作る catch_ref 系は別配列 g_exn (exn_push) を使う。 */
static inline int caught_push(void)
{
	if(g_caught_sp < (int)(sizeof(g_caught)/sizeof(g_caught[0]))) {
		int idx = g_caught_sp;
		if(!exception_save(&g_caught[idx]))
			return -1;

		g_caught_sp++;
		return idx;
	}
	g_exc_pending = 0;
	core_trap("exception stack overflow");
	return -1;
}
/* 捕捉した例外を exnref 専用ストレージ g_exn へ退避し index を返す (-1=満杯)。catch_ref/catch_all_ref
 * が呼ぶ。g_caught (legacy rethrow 台帳) とは別配列なので legacy rethrow N の J 計算に干渉しない。 */
static inline int exn_push(void)
{
	if(g_exn_sp < (int)(sizeof(g_exn)/sizeof(g_exn[0]))) {
		int idx = g_exn_sp;
		if(!exception_save(&g_exn[idx]))
			return -1;

		g_exn_sp++;
		return idx;
	}
	g_exc_pending = 0;
	core_trap("exception stack overflow");
	return -1;
}
/* exnref handle (index+1、-1=null) から捕捉例外を g_exc_* へ復元。無効な handle は trap。 */
static inline int exn_restore(int64_t handle)
{
	if(handle > 0 && handle <= g_exn_sp)
		return exception_restore(&g_exn[(int)handle - 1]);

	g_exc_pending = 0;
	core_trap("invalid exnref");
	return 0;
}

/* catch dispatch: g_exc_tagaddr が tagaddrs[ridx] と一致なら捕捉。値を dst_base へ置き、捕捉例外を
 * g_caught へ push (rethrow 用) し状態をクリアし target ラベルへ musttail (br 相当)。不一致は NEXT。
 * operands[tag_resolve_idx, nvals, dst_base, target_tgt]。 */
static int64_t H_catch(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ridx = pc[0].u32;
	uint32_t nv = pc[1].u32;
	uint32_t dst = pc[2].u32;
	int32_t want = (ridx < g_rt->num_tagaddrs) ? g_rt->tagaddrs[ridx] : (int32_t)ridx;
	if(g_exc_pending && g_exc_tagaddr == want) {
		for(uint32_t i = 0; i < nv && i < g_exc_nvals; i++)
			sp[dst + i] = g_exc_vals[i];

		if(caught_push() < 0)
			return 0;

		/* 捕捉: 状態クリア */
		g_exc_pending = 0;
		g_rt->trapped = 0;
		/* arity-1 (nv==1) の target ブロックは結果を r0 で受ける規約。legacy handler は slot から
		 * 読む (r0 無視) ので両対応。nv>=2 は slot 配置、nv==0 は値無し。 */
		int64_t pass = (nv >= 1) ? g_exc_vals[0].i64 : 0;
		const coreinstr* t = pc[3].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, pass);
	}
	NEXT(4);
}
/* catch_all: tag 無条件捕捉。値は運ばない (catch_all はラベルへ値無しで分岐)。捕捉例外は rethrow 用に
 * push (元の tag/値を保持)。operands[target_tgt]。 */
static int64_t H_catch_all(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	if(g_exc_pending) {
		if(caught_push() < 0)
			return 0;

		g_exc_pending = 0;
		g_rt->trapped = 0;
		const coreinstr* t = pc[0].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
	}
	NEXT(1);
}
/* try_table 用 catch (no-track): try_table には rethrow (legacy 専用) が無いため g_caught へ
 * push しない。push すると pop 経路が構造上存在せず台帳が汚れ、外側 legacy handler の
 * rethrow N (lexical 深さ) が別の例外を誤読する。exnref を作る catch_ref 系も g_caught でなく
 * 専用の g_exn へ積む (exn_push) ため、同様に legacy rethrow を汚さない。 */
static int64_t H_catch_nt(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ridx = pc[0].u32;
	uint32_t nv = pc[1].u32;
	uint32_t dst = pc[2].u32;
	int32_t want = (ridx < g_rt->num_tagaddrs) ? g_rt->tagaddrs[ridx] : (int32_t)ridx;
	if(g_exc_pending && g_exc_tagaddr == want) {
		for(uint32_t i = 0; i < nv && i < g_exc_nvals; i++)
			sp[dst + i] = g_exc_vals[i];

		g_exc_pending = 0;
		g_rt->trapped = 0;
		int64_t pass = (nv >= 1) ? g_exc_vals[0].i64 : 0;
		const coreinstr* t = pc[3].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, pass);
	}
	NEXT(4);
}
static int64_t H_catch_all_nt(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	if(g_exc_pending) {
		g_exc_pending = 0;
		g_rt->trapped = 0;
		const coreinstr* t = pc[0].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
	}
	NEXT(1);
}

/* catch chain 末尾: どの catch にも一致しなかった→例外を再伝播 (trapped/g_exc_pending は立ったまま return)。 */
static int64_t H_rethrow_uncaught(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	(void)pc;
	(void)sp;
	(void)mem;
	return 0;
}

/* caught_pop: handler が正常終了する位置で捕捉例外スタックを 1 段戻す。operands なし。 */
static int64_t H_caught_pop(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	if(g_caught_sp > 0)
		g_caught_sp--;

	NEXT(0);
}

/* rethrow_local: handler 内 rethrow で、同一関数の enclosing try (lexical) が再捕捉する場合。
 * g_caught[sp-1-j] を再 throw 状態にして dispatch へ musttail。operands[j, npop, target_tgt]。
 * npop = dispatch 先より内側の handler 脱出分。j の読みは pop 前の台帳基準なので読み→pop の順
 * (pop は sp を下げるだけで配列は catch push まで無傷)。 */
static int64_t H_rethrow_local(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t j = pc[0].u32;
	uint32_t npop = pc[1].u32;
	int idx = g_caught_sp - 1 - (int)j;
	if(idx >= 0) {
		if(!exception_restore(&g_caught[idx]))
			return 0;
	}
	while(npop-- > 0 && g_caught_sp > 0)
		g_caught_sp--;

	g_exc_pending = 1;
	g_rt->trapped = 1;
	if(g_rt->trap_msg == NULL)
		g_rt->trap_msg = "wasm exception";

	const coreinstr* t = pc[2].tgt;
	CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
}
/* rethrow: 同一関数に再捕捉する try が無く caller へ伝播する場合。operands[j]。 */
static int64_t H_rethrow(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t j = pc[0].u32;
	int idx = g_caught_sp - 1 - (int)j;
	if(idx >= 0) {
		if(!exception_restore(&g_caught[idx]))
			return 0;
	}
	g_exc_pending = 1;
	g_rt->trapped = 1;
	if(g_rt->trap_msg == NULL)
		g_rt->trap_msg = "wasm exception";

	(void)pc;
	(void)sp;
	(void)mem;
	return 0;
}

/* catch_ref: catch と同様に tag 照合 + 値配置。加えて exnref (捕捉例外の handle=g_exn index+1) を
 * 値の直後 (dst+nvals) に置いて target へ分岐。operands[tag_resolve_idx, nvals, dst_base, target_tgt]。 */
static int64_t H_catch_ref(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ridx = pc[0].u32;
	uint32_t nv = pc[1].u32;
	uint32_t dst = pc[2].u32;
	int32_t want = (ridx < g_rt->num_tagaddrs) ? g_rt->tagaddrs[ridx] : (int32_t)ridx;
	if(g_exc_pending && g_exc_tagaddr == want) {
		for(uint32_t i = 0; i < nv && i < g_exc_nvals; i++)
			sp[dst + i] = g_exc_vals[i];

		int idx = exn_push();
		if(idx < 0)
			return 0;

		sp[dst + nv].i64 = (int64_t)(idx + 1); /* positive exnref handle */
		g_exc_pending = 0;
		g_rt->trapped = 0;
		/* arity = nv+1。arity-1 (nv==0) は exnref を r0 で渡す (target が r0 規約のとき)。 */
		int64_t pass = (nv == 0) ? (int64_t)(idx + 1) : g_exc_vals[0].i64;
		const coreinstr* t = pc[3].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, pass);
	}
	NEXT(4);
}
/* catch_all_ref: 無条件捕捉 + exnref を dst へ置いて target へ分岐。operands[dst_base, target_tgt]。 */
static int64_t H_catch_all_ref(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	if(g_exc_pending) {
		uint32_t dst = pc[0].u32;
		int idx = exn_push();
		if(idx < 0)
			return 0;

		sp[dst].i64 = (int64_t)(idx + 1);
		g_exc_pending = 0;
		g_rt->trapped = 0;
		/* arity-1 (exnref のみ): r0 で渡す (target が r0 規約のとき)。 */
		const coreinstr* t = pc[1].tgt;
		CORE_MUSTTAIL return t->op(t + 1, sp, mem, (int64_t)(idx + 1));
	}
	NEXT(2);
}
/* throw_ref_local: exnref を再 throw し同一関数の enclosing try の dispatch へ musttail。
 * operands[exn_slot, target_tgt]。null exnref は trap。 */
static int64_t H_throw_ref_local(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	int64_t h = sp[pc[0].u32].i64;
	/* Handles are positive; -1 is null and 0 remains an invalid handle. */
	if((int32_t)h <= 0) {
		core_trap("null exnref");
		return 0;
	}
	if(!exn_restore(h))
		return 0;

	g_exc_pending = 1;
	g_rt->trapped = 1;
	if(g_rt->trap_msg == NULL)
		g_rt->trap_msg = "wasm exception";

	const coreinstr* t = pc[1].tgt;
	CORE_MUSTTAIL return t->op(t + 1, sp, mem, 0);
}
/* throw_ref: exnref を再 throw し caller へ伝播。operands[exn_slot]。null exnref は trap。 */
static int64_t H_throw_ref(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	int64_t h = sp[pc[0].u32].i64;
	if((int32_t)h <= 0) {
		core_trap("null exnref");
		return 0;
	}
	if(!exn_restore(h))
		return 0;

	g_exc_pending = 1;
	g_rt->trapped = 1;
	if(g_rt->trap_msg == NULL)
		g_rt->trap_msg = "wasm exception";

	(void)sp;
	(void)mem;
	return 0;
}

/* ───── multi-memory: memidx で (base/size/cap/pages) を選ぶ。memidx=0 は高速路フィールド。 ───── */
static inline uint8_t* core_membase(uint32_t mi)
{
	return mi == 0 ? g_rt->mem : g_rt->mems[mi].base;
}
static inline uint64_t core_memsize(uint32_t mi)
{
	return mi == 0 ? g_rt->mem_size : g_rt->mems[mi].size;
}
/* ───── multi-table: tableidx で data/size を選ぶ。tableidx=0 は高速路フィールド。 ───── */
static inline int32_t* core_tabdata(uint32_t ti)
{
	return ti == 0 ? g_rt->table : g_rt->tables[ti].data;
}
static inline uint32_t core_tabsz(uint32_t ti)
{
	return ti == 0 ? g_rt->table_size : g_rt->tables[ti].size;
}

/* ───────────────────────── memory.size / grow ───────────────────────── */
static int64_t H_memory_size(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t mi = pc[0].u32;
	r0 = as_i64((int32_t)(mi == 0 ? g_rt->mem_pages : g_rt->mems[mi].pages));
	NEXT(1);
}
static int64_t H_memory_grow(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t mi = pc[0].u32;
	/* memory64 は delta が i64 (r0 全 64bit)、memory32 は u32。返値 (old pages ≤ 65536 / 失敗 -1) は
	 * i32/i64 で bit 表現が同一 (符号拡張) なので分岐不要。 */
	uint64_t delta = (g_rt->mems && g_rt->mems[mi].is_64) ? (uint64_t)r0 : (uint64_t)(uint32_t)r0;
	uint64_t old = (mi == 0) ? g_rt->mem_pages : g_rt->mems[mi].pages;
	uint64_t cap = (mi == 0) ? g_rt->mem_cap   : g_rt->mems[mi].cap;
	uint8_t* base = (mi == 0) ? g_rt->mem : g_rt->mems[mi].base;
	/* 新規ページを OS にコミット (reserve 済み領域内、base は移動しない)。cap 超過/コミット失敗は -1。
	 * delta が巨大でも wrap しないよう cap/65536 (ページ数) と先に比較する。 */
	uint64_t nw = old + delta;
	if(delta > cap / 65536ULL - old || kw_core_mem_commit(base, old * 65536ULL, (nw - old) * 65536ULL) != 0) {
		r0 = as_i64(-1);
	} else {
		if(mi == 0) {
			g_rt->mem_pages = (uint32_t)nw;
			g_rt->mem_size = nw * 65536ULL;
		} else {
			g_rt->mems[mi].pages = (uint32_t)nw;
			g_rt->mems[mi].size = nw * 65536ULL;
		}
		/* cross-module: store の memoryinstance num_pages を grow 後サイズへ更新し、後続モジュールが
		 * この (共有) メモリを import する際の限界検証 (min ≤ 現サイズ) を通す。flat 内容は importer が
		 * build 時に store から再同期する (imports4 は size のみ検証)。共有 base は cap 上限内で連動。 */
		if(g_rt->mems != NULL) {
			int32_t sma = g_rt->mems[mi].store_memaddr;
			store_t* S = (store_t*)g_rt->store_ref;
			if(sma >= 0 && S != NULL && (size_t)sma < S->memorys.len)
				S->memorys.data[sma].num_pages = (uint32_t)nw;

			/* 共有メモリを import した別インスタンスが grow した場合、owner / 他 sharer の
			 * pages/size も新サイズへ揃える (base は共有のまま、stale size による誤 OOB を防ぐ)。 */
			kw_core_sync_shared_mem_grow(S, sma, (uint32_t)nw);
		}
		r0 = as_i64((int32_t)old);
	}
	NEXT(1);
}

/* ───────────────────────── f64 単項 / 比較 / 変換 ───────────────────────── */
/* fabs: -0.0→+0.0, NaN sign clear。d<0?-d:d は -0.0 を誤る */
static int64_t Hf64_abs(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0(fabs(r0_f64(r0)));
	NEXT(0);
}
static int64_t Hf64_neg(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0(-r0_f64(r0));
	NEXT(0);
}
static int64_t Hf64_convert_i32_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0((double)(uint32_t)r0);
	NEXT(0);
}
static int64_t Hf64_convert_i64_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0((double)(int64_t)r0);
	NEXT(0);
}
static int64_t Hf64_convert_i64_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0((double)(uint64_t)r0);
	NEXT(0);
}
/* reinterpret は r0 が既に bit を保持 → 恒等。compiler は emit せず is64 のみ調整するが、
 * 念のため恒等 handler も用意。 */
static int64_t H_reinterpret(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	NEXT(0);
}
#define FCMP_F64(NAME, OP) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		double a = sp[pc[0].u32].f64; \
		double b = sp[pc[1].u32].f64; \
		r0 = as_i64(a OP b); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		double a = sp[pc[0].u32].f64; \
		double b = r0_f64(r0); \
		r0 = as_i64(a OP b); \
		NEXT(1); \
	}
FCMP_F64(Hf64_eq, ==)
FCMP_F64(Hf64_ne, !=)
FCMP_F64(Hf64_lt, <)
FCMP_F64(Hf64_gt, >)
FCMP_F64(Hf64_le, <=)
FCMP_F64(Hf64_ge, >=)

/* ═══════════════ f32 スカラ全 op + f64 補完 (min/max/round/sqrt/conv) ═══════════════ */
/* wasm min/max: どちらかが NaN なら NaN を返し、±0 は min→-0 / max→+0 を選ぶ (C fmin/fmax と
 * 異なる)。NaN は a+b で伝播 (canonical 厳密化は別途)。 */
static inline double wmin64(double a, double b)
{
	if(isnan(a) || isnan(b))
		return a + b;

	if(a < b)
		return a;

	if(b < a)
		return b;

	return signbit(a) ? a : b;
}
static inline double wmax64(double a, double b)
{
	if(isnan(a) || isnan(b))
		return a + b;

	if(a > b)
		return a;

	if(b > a)
		return b;

	return signbit(a) ? b : a;
}
static inline float wmin32(float a, float b)
{
	if(isnan(a) || isnan(b))
		return a + b;

	if(a < b)
		return a;

	if(b < a)
		return b;

	return signbit(a) ? a : b;
}
static inline float wmax32(float a, float b)
{
	if(isnan(a) || isnan(b))
		return a + b;

	if(a > b)
		return a;

	if(b > a)
		return b;

	return signbit(a) ? b : a;
}

/* f32 二項 (add/sub/mul/div)。f32 値は r0/slot の低 32bit に bit パターンで保持。算術なので
 * 結果 NaN は canonical 正規化 (f32_r0_a)。 */
#define BIN_F32(NAME, OP) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		float a = sp[pc[0].u32].f32; \
		float b = sp[pc[1].u32].f32; \
		r0 = f32_r0_a(a OP b); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		float a = sp[pc[0].u32].f32; \
		float b = r0_f32(r0); \
		r0 = f32_r0_a(a OP b); \
		NEXT(1); \
	}
BIN_F32(Hf32_add, +)
BIN_F32(Hf32_sub, -)
BIN_F32(Hf32_mul, *)
BIN_F32(Hf32_div, /)

/* 関数二項 (min/max/copysign) — f32/f64。RC は結果格納: min/max は canonical 正規化 (f32_r0_a)、
 * copysign は bit 保持 (f32_r0、NaN payload を温存)。 */
#define BIN_F32_FN(NAME, FN, RC) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		float a = sp[pc[0].u32].f32; \
		float b = sp[pc[1].u32].f32; \
		r0 = RC(FN(a, b)); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		float a = sp[pc[0].u32].f32; \
		float b = r0_f32(r0); \
		r0 = RC(FN(a, b)); \
		NEXT(1); \
	}
BIN_F32_FN(Hf32_min, wmin32, f32_r0_a)
BIN_F32_FN(Hf32_max, wmax32, f32_r0_a)
BIN_F32_FN(Hf32_copysign, copysignf, f32_r0)
#define BIN_F64_FN(NAME, FN, RC) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		double a = sp[pc[0].u32].f64; \
		double b = sp[pc[1].u32].f64; \
		r0 = RC(FN(a, b)); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		double a = sp[pc[0].u32].f64; \
		double b = r0_f64(r0); \
		r0 = RC(FN(a, b)); \
		NEXT(1); \
	}
BIN_F64_FN(Hf64_min, wmin64, f64_r0_a)
BIN_F64_FN(Hf64_max, wmax64, f64_r0_a)
BIN_F64_FN(Hf64_copysign, copysign, f64_r0)

/* f32 単項。abs/neg は bit 保持 (f32_r0)、sqrt/round 系は算術なので canonical 正規化 (f32_r0_a)。 */
static int64_t Hf32_abs(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0(fabsf(r0_f32(r0)));
	NEXT(0);
}
static int64_t Hf32_neg(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0(-r0_f32(r0));
	NEXT(0);
}
static int64_t Hf32_sqrt(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0_a(sqrtf(r0_f32(r0)));
	NEXT(0);
}
static int64_t Hf32_ceil(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0_a(ceilf(r0_f32(r0)));
	NEXT(0);
}
static int64_t Hf32_floor(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0_a(floorf(r0_f32(r0)));
	NEXT(0);
}
static int64_t Hf32_trunc(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0_a(truncf(r0_f32(r0)));
	NEXT(0);
}
static int64_t Hf32_nearest(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0_a(rintf(r0_f32(r0)));
	NEXT(0);
}
/* f64 単項補完 (abs/neg は既存) */
static int64_t Hf64_sqrt(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0_a(sqrt(r0_f64(r0)));
	NEXT(0);
}
static int64_t Hf64_ceil(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0_a(ceil(r0_f64(r0)));
	NEXT(0);
}
static int64_t Hf64_floor(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0_a(floor(r0_f64(r0)));
	NEXT(0);
}
static int64_t Hf64_trunc(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0_a(trunc(r0_f64(r0)));
	NEXT(0);
}
static int64_t Hf64_nearest(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0_a(rint(r0_f64(r0)));
	NEXT(0);
}

/* f32 比較 (結果 0/1) */
#define FCMP_F32(NAME, OP) \
	static int64_t NAME##_ss(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		float a = sp[pc[0].u32].f32; \
		float b = sp[pc[1].u32].f32; \
		r0 = as_i64(a OP b); \
		NEXT(2); \
	} \
	static int64_t NAME##_sr(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0) \
	{ \
		float a = sp[pc[0].u32].f32; \
		float b = r0_f32(r0); \
		r0 = as_i64(a OP b); \
		NEXT(1); \
	}
FCMP_F32(Hf32_eq, ==)
FCMP_F32(Hf32_ne, !=)
FCMP_F32(Hf32_lt, <)
FCMP_F32(Hf32_gt, >)
FCMP_F32(Hf32_le, <=)
FCMP_F32(Hf32_ge, >=)

/* 変換 (f32 ← int / demote / promote) */
static int64_t Hf32_convert_i32_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0((float)(int32_t)r0);
	NEXT(0);
}
static int64_t Hf32_convert_i32_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0((float)(uint32_t)r0);
	NEXT(0);
}
static int64_t Hf32_convert_i64_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0((float)(int64_t)r0);
	NEXT(0);
}
static int64_t Hf32_convert_i64_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0((float)(uint64_t)r0);
	NEXT(0);
}
static int64_t Hf32_demote_f64(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f32_r0_a((float)r0_f64(r0));
	NEXT(0);
}
static int64_t Hf64_promote_f32(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = f64_r0_a((double)r0_f32(r0));
	NEXT(0);
}
/* trunc float → int (範囲外/NaN は trap)。境界は wasm 仕様の有効域。 */
static int64_t Hi32_trunc_f32_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	float f = r0_f32(r0);
	if(!(f >= -2147483648.0f && f < 2147483648.0f)) {
		core_trap("trunc");
		return 0;
	}
	r0 = as_i64((int32_t)f);
	NEXT(0);
}
static int64_t Hi32_trunc_f32_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	float f = r0_f32(r0);
	if(!(f > -1.0f && f < 4294967296.0f)) {
		core_trap("trunc");
		return 0;
	}
	r0 = as_i64((int32_t)(uint32_t)f);
	NEXT(0);
}
static int64_t Hi64_trunc_f32_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	float f = r0_f32(r0);
	if(!(f >= -9223372036854775808.0f && f < 9223372036854775808.0f)) {
		core_trap("trunc");
		return 0;
	}
	r0 = (int64_t)f;
	NEXT(0);
}
static int64_t Hi64_trunc_f32_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	float f = r0_f32(r0);
	if(!(f > -1.0f && f < 18446744073709551616.0f)) {
		core_trap("trunc");
		return 0;
	}
	r0 = (int64_t)(uint64_t)f;
	NEXT(0);
}
static int64_t Hi64_trunc_f64_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	double d = r0_f64(r0);
	if(!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
		core_trap("trunc");
		return 0;
	}
	r0 = (int64_t)d;
	NEXT(0);
}
static int64_t Hi64_trunc_f64_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	double d = r0_f64(r0);
	if(!(d > -1.0 && d < 18446744073709551616.0)) {
		core_trap("trunc");
		return 0;
	}
	r0 = (int64_t)(uint64_t)d;
	NEXT(0);
}

/* i64 単項 (clz/ctz/popcnt) + sign-extension ops (extend8/16/32_s) */
static int64_t Hi64_clz(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint64_t v = (uint64_t)r0;
	r0 = (int64_t)(v ? CORE_CLZ64(v) : 64);
	NEXT(0);
}
static int64_t Hi64_ctz(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint64_t v = (uint64_t)r0;
	r0 = (int64_t)(v ? CORE_CTZ64(v) : 64);
	NEXT(0);
}
static int64_t Hi64_popcnt(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int64_t)CORE_POPCNT64((uint64_t)r0);
	NEXT(0);
}
static int64_t Hi32_extend8_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = as_i64((int32_t)(int8_t)r0);
	NEXT(0);
}
static int64_t Hi32_extend16_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = as_i64((int32_t)(int16_t)r0);
	NEXT(0);
}
static int64_t Hi64_extend8_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int64_t)(int8_t)r0;
	NEXT(0);
}
static int64_t Hi64_extend16_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int64_t)(int16_t)r0;
	NEXT(0);
}
static int64_t Hi64_extend32_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = (int64_t)(int32_t)r0;
	NEXT(0);
}

/* ═══════════ 0xFC: 飽和変換 (trunc_sat) + bulk memory (copy/fill) ═══════════ */
/* trunc_sat: NaN→0、範囲外は min/max に飽和 (trap しない)。 */
static int64_t Hi32_trunc_sat_f32_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	float f = r0_f32(r0);
	int32_t v;
	if(isnan(f))
		v = 0;
	else if(f < -2147483648.0f)
		v = INT32_MIN;
	else if(f >= 2147483648.0f)
		v = INT32_MAX;
	else
		v = (int32_t)f;

	r0 = as_i64(v);
	NEXT(0);
}
static int64_t Hi32_trunc_sat_f32_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	float f = r0_f32(r0);
	uint32_t v;
	if(isnan(f) || f <= 0.0f)
		v = 0;
	else if(f >= 4294967296.0f)
		v = UINT32_MAX;
	else
		v = (uint32_t)f;

	r0 = as_i64((int32_t)v);
	NEXT(0);
}
static int64_t Hi32_trunc_sat_f64_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	double f = r0_f64(r0);
	int32_t v;
	if(isnan(f))
		v = 0;
	else if(f < -2147483648.0)
		v = INT32_MIN;
	else if(f >= 2147483648.0)
		v = INT32_MAX;
	else
		v = (int32_t)f;

	r0 = as_i64(v);
	NEXT(0);
}
static int64_t Hi32_trunc_sat_f64_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	double f = r0_f64(r0);
	uint32_t v;
	if(isnan(f) || f <= 0.0)
		v = 0;
	else if(f >= 4294967296.0)
		v = UINT32_MAX;
	else
		v = (uint32_t)f;

	r0 = as_i64((int32_t)v);
	NEXT(0);
}
static int64_t Hi64_trunc_sat_f32_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	float f = r0_f32(r0);
	int64_t v;
	if(isnan(f))
		v = 0;
	else if(f < -9223372036854775808.0f)
		v = INT64_MIN;
	else if(f >= 9223372036854775808.0f)
		v = INT64_MAX;
	else
		v = (int64_t)f;

	r0 = v;
	NEXT(0);
}
static int64_t Hi64_trunc_sat_f32_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	float f = r0_f32(r0);
	uint64_t v;
	if(isnan(f) || f <= 0.0f)
		v = 0;
	else if(f >= 18446744073709551616.0f)
		v = UINT64_MAX;
	else
		v = (uint64_t)f;

	r0 = (int64_t)v;
	NEXT(0);
}
static int64_t Hi64_trunc_sat_f64_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	double f = r0_f64(r0);
	int64_t v;
	if(isnan(f))
		v = 0;
	else if(f < -9223372036854775808.0)
		v = INT64_MIN;
	else if(f >= 9223372036854775808.0)
		v = INT64_MAX;
	else
		v = (int64_t)f;

	r0 = v;
	NEXT(0);
}
static int64_t Hi64_trunc_sat_f64_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	double f = r0_f64(r0);
	uint64_t v;
	if(isnan(f) || f <= 0.0)
		v = 0;
	else if(f >= 18446744073709551616.0)
		v = UINT64_MAX;
	else
		v = (uint64_t)f;

	r0 = (int64_t)v;
	NEXT(0);
}

/* memory.copy: operands[dst_slot, src_slot]、len=r0。memory.fill: operands[dst_slot, val_slot]、len=r0。
 * いずれも flat mem 上で境界チェック後 memmove/memset。 */
/* memory64 対応: memidx のアドレス型が i64 なら slot/r0 の全 64bit、i32 なら u32 切詰め
 * (i32 は符号拡張済みのため切詰め必須)。memory.copy の len 型は両メモリの min (spec)。 */
static inline uint64_t core_memaddr_val(uint32_t mi, int64_t v)
{
	return g_rt->mems[mi].is_64 ? (uint64_t)v : (uint64_t)(uint32_t)v;
}
/* memory.copy: operands[dst_slot, src_slot, dst_memidx, src_memidx]、len=r0。異メモリ間コピー可。 */
static int64_t H_memory_copy(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t dmi = pc[2].u32;
	uint32_t smi = pc[3].u32;
	(void)mem;
	uint64_t d = core_memaddr_val(dmi, sp[pc[0].u32].i64);
	uint64_t s = core_memaddr_val(smi, sp[pc[1].u32].i64);
	uint64_t n = (g_rt->mems[dmi].is_64 && g_rt->mems[smi].is_64) ? (uint64_t)r0 : (uint64_t)(uint32_t)r0;
	uint8_t* db = core_membase(dmi);
	uint8_t* sb = core_membase(smi);
	uint64_t dsz = core_memsize(dmi);
	uint64_t ssz = core_memsize(smi);
	if(d > dsz || n > dsz - d || s > ssz || n > ssz - s) {
		core_trap("oob memory.copy");
		return 0;
	}
	if(n)
		memmove(db + d, sb + s, (size_t)n);

	NEXT(4);
}
/* memory.fill: operands[dst_slot, val_slot, memidx]、len=r0。 */
static int64_t H_memory_fill(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t mi = pc[2].u32;
	(void)mem;
	uint64_t d = core_memaddr_val(mi, sp[pc[0].u32].i64);
	uint64_t n = core_memaddr_val(mi, r0);
	int val = (int)(uint8_t)sp[pc[1].u32].i32;
	uint8_t* mb = core_membase(mi);
	uint64_t msz = core_memsize(mi);
	if(d > msz || n > msz - d) {
		core_trap("oob memory.fill");
		return 0;
	}
	if(n)
		memset(mb + d, val, (size_t)n);

	NEXT(3);
}

/* ═══════════ reference / table ops ═══════════ */
/* Operand/local/global funcrefs always use store funcaddr (-1 = null), so
 * calls, returns, exceptions and resume can copy them across modules unchanged.
 * Only private table buffers/element segments use local gfi (or -2-fa for a
 * foreign function). Translate at table value boundaries; keep the private
 * call_indirect fast path. Externrefs are opaque and never translated. */
/* funcaddr domain のテーブルか (funcref かつ共有)。externref 共有テーブルも global_ref=1 が
 * 立ちうる (bridge の共有化経路) が、externref は identity なので変換対象外。 */
static inline uint8_t core_tab_gref(uint32_t ti)
{
	struct coretab* t = &g_rt->tables[ti];
	return (uint8_t)(t->global_ref && !t->is_externref);
}
static inline int core_tab_localref(uint32_t ti)
{
	return !g_rt->tables[ti].is_externref && !g_rt->tables[ti].global_ref;
}
/* gfi domain 値 → store funcaddr (private table の読み出し / 共有テーブルへのコピー用)。
 * noinline: table ハンドラ群への inline 展開による .text 膨張 (配置悪化) を防ぐ。 */
static CORE_NOINLINE int32_t core_ref_to_funcaddr(int32_t v)
{
	if(v == -1)
		return -1;                 /* null */

	if(v <= -2)
		return -2 - v;             /* encoded foreign funcaddr を復元 */

	moduleinst_t* mi = (moduleinst_t*)g_rt->inst_ref;
	if(mi == NULL || mi->funcaddrs == NULL || (uint32_t)v >= g_rt->num_func_sigs)
		return -1;

	return (int32_t)mi->funcaddrs[v];
}
/* store funcaddr → gfi domain 値 (private table への書き込み用)。実行中モジュールに解決できない
 * (別モジュール定義の) funcaddr は -2-fa でエンコードして運ぶ。noinline: 同上。 */
static CORE_NOINLINE int32_t core_funcaddr_to_ref(int32_t fa)
{
	if(fa < 0)
		return -1;                  /* REF_NULL */

	moduleinst_t* mi = (moduleinst_t*)g_rt->inst_ref;
	if(mi != NULL && mi->funcaddrs != NULL) {
		for(uint32_t g = 0; g < g_rt->num_func_sigs; g++) {
			if(mi->funcaddrs[g] == (funcaddr_t)fa)
				return (int32_t)g;
		}
	}
	return -2 - fa;                        /* foreign */
}
static int64_t H_ref_is_null(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = as_i64((int32_t)r0 == -1);
	NEXT(0);
}
/* table64 の call_indirect 前置検査: idx (r0, i64) の上位 32bit が非ゼロなら必ずテーブルサイズ超
 * (実テーブルサイズは u32 範囲) なので trap する。下位 32bit は後続 call_indirect の u32 検査に
 * 委ねる。table64 のときだけ compile が挿入し、table32 の hot path には現れない。 */
static int64_t H_idx64_chk(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	if((uint64_t)r0 >> 32) {
		core_trap("undefined element");
		return 0;
	}
	NEXT(0);
}
/* cross-module 共有テーブル整合: core table への書き込みを store の tableinstance elem へも反映する。
 * importer の build 時 store→共有 buffer 再同期や後発 import による共有化 (owner 昇格) で core 書き込み値が
 * 失われるのを防ぐ。store elem は常に funcaddr domain (externref は identity) なので、
 * 非共有 funcref テーブル (core=gfi domain) は funcaddr へ翻訳して書く。 */
static inline void core_table_store_write(uint32_t ti, uint32_t idx, int32_t val)
{
	if(!g_rt->tables || !g_rt->store_ref)
		return;

	struct coretab* t = &g_rt->tables[ti];
	int32_t sval = (t->is_externref || t->global_ref) ? val : core_ref_to_funcaddr(val);
	int32_t sta = t->store_tableaddr;
	store_t* S = (store_t*)g_rt->store_ref;
	if(sta < 0 || (size_t)sta >= S->tables.len)
		return;

	tableinstance_t* tab = &kinowasm_array_at(S->tables, sta);
	if((size_t)idx < tab->elem.len)
		kinowasm_array_at(tab->elem, idx) = (kinowasm_ref_t)sval;
}
/* table64 対応: テーブルの idx 型が i64 なら slot/r0 の全 64bit、i32 なら u32 切詰め。
 * 実テーブルサイズは u32 範囲なので u64 比較で 2^32 以上の idx は自然に範囲外 trap になる。 */
static inline uint64_t core_tabidx_val(uint32_t ti, int64_t v)
{
	return g_rt->tables[ti].is_64 ? (uint64_t)v : (uint64_t)(uint32_t)v;
}
/* table.get returns a canonical store address, even from a private table. */
static int64_t H_table_get(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ti = pc[0].u32;
	uint64_t idx = core_tabidx_val(ti, r0);
	if(idx >= core_tabsz(ti)) {
		core_trap("out of bounds table access");
		return 0;
	}
	int32_t v = core_tabdata(ti)[idx];
	if(core_tab_localref(ti))
		v = core_ref_to_funcaddr(v);

	r0 = as_i64(v);
	NEXT(1);
}
/* table.size: operands[tableidx]。サイズ ≤ u32 なので i32/i64 (table64) で bit 表現同一、分岐不要。 */
static int64_t H_table_size(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	r0 = as_i64((int32_t)core_tabsz(pc[0].u32));
	NEXT(1);
}
/* table.set translates the canonical value only for a private funcref table. */
static int64_t H_table_set(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ti = pc[1].u32;
	uint64_t idx = core_tabidx_val(ti, sp[pc[0].u32].i64);
	if(idx >= core_tabsz(ti)) {
		core_trap("out of bounds table access");
		return 0;
	}
	int32_t v = (int32_t)r0;
	if(core_tab_localref(ti))
		v = core_funcaddr_to_ref(v);

	core_tabdata(ti)[idx] = v;
	core_table_store_write(ti, (uint32_t)idx, v);
	NEXT(2);
}
/* table.fill: operands[dst_slot, val_slot, tableidx]、len=r0。 */
static int64_t H_table_fill(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ti = pc[2].u32;
	uint64_t d = core_tabidx_val(ti, sp[pc[0].u32].i64);
	uint64_t n = core_tabidx_val(ti, r0);
	int32_t v = (int32_t)sp[pc[1].u32].i32;
	uint64_t tsz = core_tabsz(ti);
	if(d > tsz || n > tsz - d) {
		core_trap("out of bounds table access");
		return 0;
	}
	if(core_tab_localref(ti))
		v = core_funcaddr_to_ref(v);

	int32_t* td = core_tabdata(ti);
	for(uint64_t i = 0; i < n; i++) {
		td[d + i] = v;
		core_table_store_write(ti, (uint32_t)(d + i), v);
	}
	NEXT(3);
}
/* table.copy: operands[dst_slot, src_slot, dst_tableidx, src_tableidx]、len=r0。異テーブル間可。
 * src/dst の domain (共有=funcaddr / 非共有=gfi) が異なる場合は要素ごとに変換。同 domain は
 * memmove (同一共有 buffer を指す 2 テーブルの overlap も安全)。store elem へも伝播する。 */
static int64_t H_table_copy(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t dti = pc[2].u32;
	uint32_t sti = pc[3].u32;
	uint64_t d = core_tabidx_val(dti, sp[pc[0].u32].i64);
	uint64_t s = core_tabidx_val(sti, sp[pc[1].u32].i64);
	uint64_t n = (g_rt->tables[dti].is_64 && g_rt->tables[sti].is_64) ? (uint64_t)r0 : (uint64_t)(uint32_t)r0;
	uint64_t dsz = core_tabsz(dti);
	uint64_t ssz = core_tabsz(sti);
	if(d > dsz || n > dsz - d || s > ssz || n > ssz - s) {
		core_trap("out of bounds table access");
		return 0;
	}
	if(n) {
		int32_t* dd = core_tabdata(dti);
		const int32_t* sd = core_tabdata(sti);
		uint8_t dg = core_tab_gref(dti);
		uint8_t sg = core_tab_gref(sti);
		if(dg == sg) {
			memmove(&dd[d], &sd[s], (size_t)n * sizeof(int32_t));
		} else if(sg) {
			for(uint64_t i = 0; i < n; i++)
				dd[d + i] = core_funcaddr_to_ref(sd[s + i]);
		} else {
			for(uint64_t i = 0; i < n; i++)
				dd[d + i] = core_ref_to_funcaddr(sd[s + i]);
		}
		for(uint64_t i = 0; i < n; i++)
			core_table_store_write(dti, (uint32_t)(d + i), dd[d + i]);
	}
	NEXT(4);
}
/* table.grow: operands[init_slot, tableidx]、delta=r0。old size を返す (≤u32 なので i32/i64 で
 * bit 表現同一、返値分岐不要)。max 超過/OOM は -1。table64 の delta は i64 (r0 全 64bit)。 */
static int64_t H_table_grow(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t ti = pc[1].u32;
	uint64_t delta = core_tabidx_val(ti, r0);
	int32_t initv = (int32_t)sp[pc[0].u32].i32;
	uint32_t old = (ti == 0) ? g_rt->table_size : g_rt->tables[ti].size;
	uint32_t tmax = (ti == 0) ? g_rt->table_max  : g_rt->tables[ti].max;
	/* funcref テーブルの store elem は funcaddr domain。共有 (global_ref) は core buffer も funcaddr。 */
	int32_t store_initv = initv;
	if(core_tab_localref(ti))
		initv = core_funcaddr_to_ref(initv);

	/* delta > tmax の先行検査で old+delta の u64 wrap (巨大 delta の table64) を防ぐ。 */
	if(delta > tmax || (uint64_t)old + delta > tmax) {
		r0 = as_i64(-1);
		NEXT(2);
	}
	uint64_t nw = (uint64_t)old + delta;
	/* 32bit size_t 環境向けガード: (size_t)nw*sizeof(int32_t) の wrap を realloc 前に弾く
	 * (64bit では nw<=tmax<=UINT32_MAX 相当なので通常到達しない)。 */
	if(nw > (uint64_t)(SIZE_MAX / sizeof(int32_t))) {
		r0 = as_i64(-1);
		NEXT(2);
	}
	int32_t* nt = (int32_t*)kinowasm_mem_realloc(core_tabdata(ti), (size_t)(nw ? nw : 1) * sizeof(int32_t));
	if(!nt) {
		r0 = as_i64(-1);
		NEXT(2);
	}
	for(uint32_t i = old; i < (uint32_t)nw; i++)
		nt[i] = initv;

	if(ti == 0) {
		g_rt->table = nt;
		g_rt->table_size = (uint32_t)nw;
		g_rt->tables[0].data = nt;
		g_rt->tables[0].size = (uint32_t)nw;
	} else {
		g_rt->tables[ti].data = nt;
		g_rt->tables[ti].size = (uint32_t)nw;
	}
	/* cross-module: store の tableinstance elem を grow 後サイズへ伸ばし、後続モジュールが
	 * このテーブルを import する際にサイズと新要素の store funcaddr を引き継げるようにする。 */
	if(g_rt->store_ref && g_rt->tables) {
		int32_t sta = g_rt->tables[ti].store_tableaddr;
		store_t* S = (store_t*)g_rt->store_ref;
		/* cross-module 共有テーブル: realloc で data が移動した可能性があるため、同一 store_tableaddr を
		 * 共有する全インスタンスの data/size を新ポインタ (nt) へ揃える (UAF / 二重 free / リーク防止)。
		 * 線形メモリ grow の kw_core_sync_shared_mem_grow と対称。 */
		kw_core_sync_shared_table_grow(g_rt->store_ref, sta, nt, (uint32_t)nw);
		if(sta >= 0 && (size_t)sta < S->tables.len) {
			tableinstance_t* tab = &kinowasm_array_at(S->tables, sta);
			size_t oldlen = tab->elem.len;
			if((uint64_t)nw > oldlen && !_is_error(kinowasm_array_grow_from(tab->elem, (size_t)((uint64_t)nw - oldlen)))) {
				kinowasm_ref_t rv = (store_initv < 0) ? REF_NULL : (kinowasm_ref_t)store_initv;   /* funcaddr domain (externref は identity) */
				for(size_t i = oldlen; i < (size_t)nw; i++)
					kinowasm_array_at(tab->elem, i) = rv;

				tab->elem.len = (size_t)nw;
			}
		}
	}
	r0 = as_i64((int32_t)old);
	NEXT(2);
}

/* memory.init: operands[dataidx, dst_slot, src_slot, memidx]、len=r0。passive data seg → memory。
 * dst はメモリのアドレス型 (memory64 は i64)、src/len は data segment 側なので常に i32。 */
static int64_t H_memory_init(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t dataidx = pc[0].u32;
	uint32_t s = (uint32_t)sp[pc[2].u32].i32;
	uint32_t n = (uint32_t)r0;
	(void)mem;
	uint32_t mi = pc[3].u32;
	uint8_t* mb = core_membase(mi);
	uint64_t d = core_memaddr_val(mi, sp[pc[1].u32].i64);
	uint64_t msz = core_memsize(mi);
	uint32_t slen = g_rt->datasegs[dataidx].dropped ? 0 : g_rt->datasegs[dataidx].len;
	if((uint64_t)s + n > slen || d > msz || (uint64_t)n > msz - d) {
		core_trap("out of bounds memory access");
		return 0;
	}
	if(n)
		memcpy(mb + d, g_rt->datasegs[dataidx].bytes + s, (size_t)n);

	NEXT(4);
}
/* data.drop: passive data seg を drop 済みに。 */
static int64_t H_data_drop(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	g_rt->datasegs[pc[0].u32].dropped = 1;
	NEXT(1);
}
/* table.init: operands[elemidx, dst_slot, src_slot, tableidx]、len=r0。passive elem seg → table。
 * dst はテーブルの idx 型 (table64 は i64)、src/len は elem segment 側なので常に i32。 */
static int64_t H_table_init(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t elemidx = pc[0].u32;
	uint32_t s = (uint32_t)sp[pc[2].u32].i32;
	uint32_t n = (uint32_t)r0;
	uint32_t ti = pc[3].u32;
	uint64_t d = core_tabidx_val(ti, sp[pc[1].u32].i64);
	uint64_t tsz = core_tabsz(ti);
	uint32_t slen = g_rt->elemsegs[elemidx].dropped ? 0 : g_rt->elemsegs[elemidx].len;
	if((uint64_t)s + n > slen || d > tsz || (uint64_t)n > tsz - d) {
		core_trap("out of bounds table access");
		return 0;
	}
	uint8_t gref = core_tab_gref(ti);   /* elemseg は gfi domain。共有テーブルへは funcaddr へ変換 */
	int32_t* td = core_tabdata(ti);
	for(uint32_t i = 0; i < n; i++) {
		int32_t v = g_rt->elemsegs[elemidx].funcs[s + i];
		if(gref)
			v = core_ref_to_funcaddr(v);

		td[d + i] = v;
		core_table_store_write(ti, (uint32_t)(d + i), v);
	}
	NEXT(4);
}
/* elem.drop: passive elem seg を drop 済みに。 */
static int64_t H_elem_drop(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	g_rt->elemsegs[pc[0].u32].dropped = 1;
	NEXT(1);
}

/* ───── wide arithmetic (i64.add128/sub128/mul_wide_s/u) ─────
 * 入力と出力 slot は同一 base に揃えてあるので全入力を読んでから書く。operands[base]。 */
static int64_t H_i64_add128(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t b = pc[0].u32;
	(void)mem;
	(void)r0;
	uint64_t lo_a = (uint64_t)sp[b].i64;
	uint64_t hi_a = (uint64_t)sp[b + 1].i64;
	uint64_t lo_b = (uint64_t)sp[b + 2].i64;
	uint64_t hi_b = (uint64_t)sp[b + 3].i64;
	uint64_t lo = lo_a + lo_b;
	uint64_t carry = (lo < lo_a) ? 1u : 0u;
	uint64_t hi = hi_a + hi_b + carry;
	sp[b].i64 = (int64_t)lo;
	sp[b + 1].i64 = (int64_t)hi;
	NEXT(1);
}
static int64_t H_i64_sub128(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t b = pc[0].u32;
	(void)mem;
	(void)r0;
	uint64_t lo_a = (uint64_t)sp[b].i64;
	uint64_t hi_a = (uint64_t)sp[b + 1].i64;
	uint64_t lo_b = (uint64_t)sp[b + 2].i64;
	uint64_t hi_b = (uint64_t)sp[b + 3].i64;
	uint64_t lo = lo_a - lo_b;
	uint64_t borrow = (lo_a < lo_b) ? 1u : 0u;
	uint64_t hi = hi_a - hi_b - borrow;
	sp[b].i64 = (int64_t)lo;
	sp[b + 1].i64 = (int64_t)hi;
	NEXT(1);
}
/* 128bit 乗算は 32bit limb 4 部分積で portable に (MSVC/clang 共通)。 */
static int64_t H_i64_mul_wide_s(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t base = pc[0].u32;
	(void)mem;
	(void)r0;
	int64_t a = sp[base].i64;
	int64_t b = sp[base + 1].i64;
	uint64_t ua = (uint64_t)a;
	uint64_t ub = (uint64_t)b;
	uint64_t a_lo = ua & 0xFFFFFFFFu;
	uint64_t a_hi = ua >> 32;
	uint64_t b_lo = ub & 0xFFFFFFFFu;
	uint64_t b_hi = ub >> 32;
	uint64_t ll = a_lo * b_lo;
	uint64_t lh = a_lo * b_hi;
	uint64_t hl = a_hi * b_lo;
	uint64_t hh = a_hi * b_hi;
	uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
	uint64_t lo = (ll & 0xFFFFFFFFu) | (mid << 32);
	uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
	/* 符号付き補正: 負のオペランドぶんを上位から引く。 */
	if(a < 0)
		hi -= ub;

	if(b < 0)
		hi -= ua;

	sp[base].i64 = (int64_t)lo;
	sp[base + 1].i64 = (int64_t)hi;
	NEXT(1);
}
static int64_t H_i64_mul_wide_u(const coreinstr* pc, coreval_t* sp, uint8_t* mem, int64_t r0)
{
	uint32_t base = pc[0].u32;
	(void)mem;
	(void)r0;
	uint64_t ua = (uint64_t)sp[base].i64;
	uint64_t ub = (uint64_t)sp[base + 1].i64;
	uint64_t a_lo = ua & 0xFFFFFFFFu;
	uint64_t a_hi = ua >> 32;
	uint64_t b_lo = ub & 0xFFFFFFFFu;
	uint64_t b_hi = ub >> 32;
	uint64_t ll = a_lo * b_lo;
	uint64_t lh = a_lo * b_hi;
	uint64_t hl = a_hi * b_lo;
	uint64_t hh = a_hi * b_hi;
	uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
	uint64_t lo = (ll & 0xFFFFFFFFu) | (mid << 32);
	uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
	sp[base].i64 = (int64_t)lo;
	sp[base + 1].i64 = (int64_t)hi;
	NEXT(1);
}

/* ───────────────────────── core_run ───────────────────────── */
int64_t core_run(const coreinstr* entry, coreval_t* sp, uint8_t* mem)
{
	return entry->op(entry + 1, sp, mem, 0);
}

/* compiler が参照する handler 公開 (op 種別 → 各変種 fn-ptr)。
 * core_ops.h を CORE_DEFINE_OPS 付きで include すると non-static グローバルとして定義される。
 * 上で定義した static handler を参照するのでこの位置 (末尾) で include する。 */
#define CORE_DEFINE_OPS
#include "kw_core_ops.h"
#undef CORE_DEFINE_OPS

/* handler fn-ptr → op 名 (デバッグ逆アセンブル用)。 */
const char* core_opname(coreop_t fn)
{
#define CORE_NAME_OPS
#include "kw_core_ops.h"
#undef CORE_NAME_OPS
	if(fn == H_br_if_not)
		return "br_if_not";

	if(fn == H_br_if_v)
		return "br_if_v";

	return "?";
}

#if defined(CORE_PROFILE)
/* プロファイル集計を回数降順で stderr に出す。実行末尾に main から呼ぶ。 */
void core_prof_dump(void)
{
	extern coreop_t g_prof_fn[];
	extern long long g_prof_ct[];
	extern coreop_t g_prof_pair_a[];
	extern coreop_t g_prof_pair_b[];
	extern long long g_prof_pair_ct[];
	extern long long g_prof_reload_same;
	extern long long g_prof_reload_diff;
	long long total = 0;
	for(int i = 0; i < CORE_PROF_N; i++)
		total += g_prof_ct[i];

	fprintf(stderr, "[core] === opcode profile (total=%lld) ===\n", total);
	for(int rank = 0; rank < 48; rank++) {
		int best = -1;
		long long bestc = 0;
		for(int i = 0; i < CORE_PROF_N; i++) {
			if(g_prof_ct[i] > bestc) {
				bestc = g_prof_ct[i];
				best = i;
			}
		}
		if(best < 0)
			break;

		fprintf(stderr, "  %-18s %14lld  (%5.2f%%)\n", core_opname(g_prof_fn[best]), bestc, 100.0 * bestc / total);
		g_prof_ct[best] = 0;
	}
	fprintf(stderr, "[core] === opcode pair profile (top 48) ===\n");
	for(int rank = 0; rank < 48; rank++) {
		int best = -1;
		long long bestc = 0;
		for(int i = 0; i < CORE_PROF_PAIR_N; i++) {
			if(g_prof_pair_ct[i] > bestc) {
				bestc = g_prof_pair_ct[i];
				best = i;
			}
		}
		if(best < 0)
			break;

		fprintf(stderr, "  %-18s -> %-18s %14lld  (%5.2f%%)\n",
			core_opname(g_prof_pair_a[best]), core_opname(g_prof_pair_b[best]),
			bestc, 100.0 * bestc / total);
		g_prof_pair_ct[best] = 0;
	}
	fprintf(stderr, "[core] setslot->getslot same-slot (redundant reload) = %lld\n", g_prof_reload_same);
	fprintf(stderr, "[core] setslot->getslot diff-slot (legitimate)       = %lld\n", g_prof_reload_diff);
}
#endif
