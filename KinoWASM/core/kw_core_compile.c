/* core_compile.c — 生 WASM (スタックマシン) → register-TOS direct-threaded bytecode。
 *
 *  方式:
 *    - compile 時に operand stack をシミュレートし、各値の location を追跡:
 *        L_SLOT (sp[slot] にある / local の別名)、L_REG (r0)、L_CONST (即値、lazy)。
 *      local.get/const は lazy (emit せず location 記録のみ)、消費時に変種を選んで emit。
 *    - register-TOS: 結果は常に r0 (L_REG)。同時に L_REG は最大1個 (古い reg は spill)。
 *    - 分岐は絶対 bytecode アドレスへ解決 (runtime label stack 無し)。forward (block/if end)
 *      は fixup list で end emit 時にパッチ。loop は body 先頭へ後方ジャンプ。
 *    - block/if の結果 (arity<=1) は continuation 時点で r0 に置く規約。
 *
 *  local aliasing: L_SLOT が local x を指す間に local.set/tee x が来たら、先に home slot へ
 *  コピーして退避 (snapshot 意味論を保つ)。
 */
#include "kw_core.h"
#include "kw_core_ops.h"
#include "kalloc.h" /* compile スクラッチ/bytecode を KinoWASM kalloc アリーナから確保する */

/* ───── compile 出力バッファ ───── */
typedef struct {
	coreinstr* code;
	uint32_t   len;
	uint32_t   cap;
} codebuf_t;

static void cb_reserve(codebuf_t* cb, uint32_t n)
{
	if(cb->len + n > cb->cap) {
		uint32_t nc = cb->cap ? cb->cap * 2 : 256; /* 初回 256、以降は倍々。余剰は末尾 trim で回収 */
		while(nc < cb->len + n)
			nc *= 2;

		cb->code = kinowasm_mem_realloc(cb->code, nc * sizeof(coreinstr));
		if(!cb->code)
			core_fatal("OOM code");

		cb->cap = nc;
	}
}
static uint32_t cb_emit_op(codebuf_t* cb, coreop_t fn)
{
	cb_reserve(cb, 1);
	uint32_t i = cb->len;
	cb->code[cb->len++].op = fn;
	return i;
}
static uint32_t cb_emit_u32(codebuf_t* cb, uint32_t v)
{
	cb_reserve(cb, 1);
	uint32_t i = cb->len;
	cb->code[cb->len++].u32 = v;
	return i;
}
static void cb_emit_i32(codebuf_t* cb, int32_t v)
{
	cb_reserve(cb, 1);
	cb->code[cb->len++].i32 = v;
}
static void cb_emit_i64(codebuf_t* cb, int64_t v)
{
	cb_reserve(cb, 1);
	cb->code[cb->len++].i64 = v;
}
/* fixup 対象 */
static uint32_t cb_emit_tgt(codebuf_t* cb)
{
	cb_reserve(cb, 1);
	uint32_t i = cb->len;
	cb->code[cb->len++].tgt = NULL;
	return i;
}

/* ───── compile 時 operand stack ───── */
enum { L_SLOT, L_REG, L_CONST };
typedef struct {
	uint8_t kind;
	uint8_t is64;
	int32_t slot;
	int64_t cval;
} loc_t;

/* ───── 制御フロー枠 ───── */
enum { C_BLOCK, C_LOOP, C_IF, C_TRY };
/* EH: try_table の catch 句。kind 0=catch 1=catch_ref 2=catch_all 3=catch_all_ref。 */
typedef struct {
	uint8_t  kind;
	uint32_t tag;
	uint32_t label;
} corecatch_t;
typedef struct {
	uint8_t  kind;
	uint32_t loop_start;  /* C_LOOP: body 先頭 code index */
	uint32_t* fixups;     /* C_BLOCK/C_IF: パッチ対象 tgt word index 群 */
	uint32_t  nfix;
	uint32_t  cfix;
	int       arity;      /* 結果値数 (multi-value 対応で 0..N) */
	int       in_arity;   /* param 値数 (multi-value 入口、loop の後方ジャンプ運搬数) */
	int       sp_base;    /* 枠の operand base (= params を含む底。end/br の結果はここに置く) */
	uint32_t  else_fixup; /* C_IF: else へのジャンプ word index (else/end で解決)。
	                       * 0xFFFFFFFF = dead code 中の if (分岐 emit せず、patch 不要) */
	int       has_else;
	int       entry_unr;  /* 枠入場時の unreachable 状態。end で復元 (dead block の nest 対応)。 */
	/* C_TRY (try_table): catch 句群 + body 内 call の dispatch 先 fixup。 */
	corecatch_t* catches;
	uint32_t  ncatch;
	uint32_t* disp_fixups;
	uint32_t  ndisp;
	uint32_t  cdisp;
	/* C_TRY (legacy try 0x06): handler 句がインラインに連なる。is_legacy=1。
	 *   in_handler: body 句を抜け handler 句を compile 中 (この try は throw 保護対象外 = innermost_try で skip)。
	 *   nomatch_fixup: 直前 catch 句の no-match br word index (次の catch / 末尾 terminator へ解決)。 */
	uint8_t  is_legacy;
	uint8_t  in_handler;
	uint32_t nomatch_fixup;
} ctrl_t;

/* loop 後方ジャンプ: tgt word に code index(u32) を仮置きし、最後に絶対ポインタ化。 */
typedef struct {
	uint32_t word_idx;
} looprel_t;

typedef struct {
	coremodule_t*  m;
	corefuncdef_t* f;
	codebuf_t cb;
	corereader_t  r;
	loc_t*    st;
	int       cdepth;
	int       cap_st;
	int       reg_pos;      /* r0 を保持する stack index (-1=無) */
	ctrl_t*   ctrl;
	int       cctrl;
	int       cap_ctrl;
	int       ntry;         /* スタック内の C_TRY 枠数 (innermost_try / delegate_target_try の fast path 用) */
	uint32_t  fb;           /* 最初の operand-stack slot (= num_params + num_locals) */
	uint32_t  num_params;
	uint32_t  max_slot;     /* 使用 slot 高水位 */
	int       unreachable;  /* polymorphic stack 中 (br/return/unreachable 後) */
	/* loop 後方ジャンプ fixup */
	looprel_t* loops;
	uint32_t  nloops;
	uint32_t  cloops;
	/* ── compare+br_if 融合追跡 ── */
	uint32_t  last_op_widx; /* emit_binop が出した op word の cb index */
	int       last_mode;    /* 直近 binop の変種 0=ss,1=sr,2=si */
	int       fuse_widx;    /* 融合候補 compare の op word index (-1=無) */
	int       fuse_cmp;     /* 融合候補の compare index (0-9) */
	int       fuse_mode;    /* 融合候補の変種 0/1/2 */
	int       fuse_pos;     /* compare 結果の stack position */
	uint32_t  fuse_cb_end;  /* compare 直後の cb.len (以後 emit があれば無効) */
	/* ── binop; local.set 融合追跡 (結果を slot へ直接書き setslot 消去) ── */
	int       bst_widx;     /* 直前 binop の op word index (-1=無) */
	corebinop_t* bst_tab;   /* 直前 binop の変種表 */
	int       bst_mode;     /* 0=ss,1=sr,2=si (rs/ri は融合外) */
	int       bst_pos;      /* 結果 stack position */
	uint32_t  bst_cb_end;   /* binop 直後の cb.len */
	/* ── load; local.set 融合追跡 (結果を slot へ直接書き setslot 消去)。hot i32 load 4 種のみ ── */
	int       lst_widx;     /* 直前 load の op word index (-1=無/st変種なし) */
	coreop_t  lst_st_fn;    /* その load の st 変種 */
	int       lst_pos;      /* 結果 stack position */
	uint32_t  lst_cb_end;   /* load 直後の cb.len */
} comp_t;

static void rec_loop(comp_t* c, uint32_t word_idx)
{
	if(c->nloops >= c->cloops) {
		c->cloops = c->cloops ? c->cloops * 2 : 16;
		c->loops = kinowasm_mem_realloc(c->loops, c->cloops * sizeof(looprel_t));
		if(!c->loops)
			core_fatal("OOM loops");
	}
	c->loops[c->nloops++].word_idx = word_idx;
}

static void push_loc(comp_t* c, loc_t l)
{
	if(c->cdepth >= c->cap_st) {
		c->cap_st = c->cap_st ? c->cap_st * 2 : 64;
		c->st = kinowasm_mem_realloc(c->st, c->cap_st * sizeof(loc_t));
		if(!c->st)
			core_fatal("OOM st");
	}
	c->st[c->cdepth++] = l;
	uint32_t hs = c->fb + (c->cdepth - 1);
	if(hs + 1 > c->max_slot)
		c->max_slot = hs + 1;
}
static uint32_t home_slot(comp_t* c, int pos)
{
	return c->fb + pos;
}

/* reg を home slot へ退避 */
static void spill_reg(comp_t* c)
{
	if(c->reg_pos < 0)
		return;

	uint32_t hs = home_slot(c, c->reg_pos);
	cb_emit_op(&c->cb, core_setslot);
	cb_emit_u32(&c->cb, hs);
	c->st[c->reg_pos].kind = L_SLOT;
	c->st[c->reg_pos].slot = (int32_t)hs;
	c->reg_pos = -1;
}

/* loc を slot へ正規化し slot index を返す */
static uint32_t to_slot(comp_t* c, int pos)
{
	loc_t* l = &c->st[pos];
	if(l->kind == L_SLOT)
		return (uint32_t)l->slot;

	if(l->kind == L_REG) {
		uint32_t hs = home_slot(c, pos);
		cb_emit_op(&c->cb, core_setslot);
		cb_emit_u32(&c->cb, hs);
		l->kind = L_SLOT;
		l->slot = (int32_t)hs;
		c->reg_pos = -1;
		return hs;
	}
	/* L_CONST → const を直接 slot へ (r0 を破壊しない)。 */
	uint32_t hs = home_slot(c, pos);
	cb_emit_op(&c->cb, l->is64 ? core_const_slot_i64 : core_const_slot_i32);
	cb_emit_u32(&c->cb, hs);
	if(l->is64)
		cb_emit_i64(&c->cb, l->cval);
	else
		cb_emit_i32(&c->cb, (int32_t)l->cval);

	l->kind = L_SLOT;
	l->slot = (int32_t)hs;
	return hs;
}

/* loc を「その position の home slot」へ必ず実体化 (call 引数を連続 slot に並べる用)。
 * lazy local 別名のままだと引数が散在し do_call の連続読みと不一致になるため。 */
static void to_home_slot(comp_t* c, int pos)
{
	uint32_t target = home_slot(c, pos);
	loc_t* l = &c->st[pos];
	if(l->kind == L_SLOT && l->slot == (int32_t)target)
		return;

	if(l->kind == L_SLOT) {
		cb_emit_op(&c->cb, core_copyslot);
		cb_emit_u32(&c->cb, target);
		cb_emit_u32(&c->cb, (uint32_t)l->slot);
	} else if(l->kind == L_REG) {
		cb_emit_op(&c->cb, core_setslot);
		cb_emit_u32(&c->cb, target);
		if(c->reg_pos == pos)
			c->reg_pos = -1;
	} else {
		cb_emit_op(&c->cb, l->is64 ? core_const_slot_i64 : core_const_slot_i32);
		cb_emit_u32(&c->cb, target);
		if(l->is64)
			cb_emit_i64(&c->cb, l->cval);
		else
			cb_emit_i32(&c->cb, (int32_t)l->cval);
	}
	l->kind = L_SLOT;
	l->slot = (int32_t)target;
}

/* 枠 (block/loop/if) に入る前に、外側 operand に残っている「local の lazy alias」を
 * home slot へ実体化して縁を切る。
 *
 * local.get は L_SLOT で local のスロットを直接指す別名として積まれる (lazy alias)。
 * 枠の内側で local.set/tee がその local を書き換えると alias の指す値も変わるため、
 * invalidate_local が set/tee の位置に copyslot を挿して alias を home slot へ移す。
 * ところがその copyslot は「set/tee のある実行経路」にしか置かれない。br で枠を
 * 抜ける経路を通ると copyslot に到達しないまま、alias 先の home slot を読む
 * コードだけが実行され、未初期化スロットを値/アドレスとして使ってしまう。
 *
 * 例 (-O2 の std::vector::push_back で実際に出る形):
 *   (i32.store offset=52 (local.get $0)      ;; ← alias のまま積まれる
 *     (block (result i32)
 *       (if cond (then (br $b <end+112>)))   ;; ← capacity 十分ならこの経路
 *       ... (local.tee $0 ...) ...))         ;; ← 再確保経路にしかない
 * br 経路では copyslot が走らず、store のアドレスが未初期化スロットになる。
 *
 * 対策として枠の入場時に実体化しておく。local スロットは [0, fb)、operand の
 * home slot は [fb, ...) なので slot < fb で local 別名を判別できる。 */
static void materialize_local_aliases(comp_t* c, int upto)
{
	for(int i = 0; i < upto; i++) {
		if(c->st[i].kind == L_SLOT && c->st[i].slot < (int32_t)c->fb)
			to_home_slot(c, i);
	}
}


/* TOS を r0 に確定 (br_if cond / 単項 / 末尾結果用) */
static void to_reg_top(comp_t* c)
{
	if(c->cdepth == 0)
		return;

	int top = c->cdepth - 1;
	loc_t* l = &c->st[top];
	if(l->kind == L_REG)
		return;

	if(c->reg_pos >= 0)
		spill_reg(c);

	if(l->kind == L_SLOT) {
		cb_emit_op(&c->cb, core_getslot);
		cb_emit_u32(&c->cb, (uint32_t)l->slot);
	} else {
		cb_emit_op(&c->cb, l->is64 ? core_const_i64 : core_const_i32);
		if(l->is64)
			cb_emit_i64(&c->cb, l->cval);
		else
			cb_emit_i32(&c->cb, (int32_t)l->cval);
	}
	l->kind = L_REG;
	c->reg_pos = top;
}

/* local x を指す lazy alias を退避 (local.set/tee x 前) */
static void invalidate_local(comp_t* c, uint32_t x)
{
	for(int i = 0; i < c->cdepth; i++) {
		if(c->st[i].kind == L_SLOT && c->st[i].slot == (int32_t)x) {
			uint32_t hs = home_slot(c, i);
			cb_emit_op(&c->cb, core_copyslot);
			cb_emit_u32(&c->cb, hs);
			cb_emit_u32(&c->cb, x);
			c->st[i].slot = (int32_t)hs;
		}
	}
}

/* 二項 emit: pop b, pop a, push reg。tab=変種表。 */
static void emit_binop(comp_t* c, corebinop_t* tab)
{
	/* polymorphic: スタック調整のみ */
	if(c->unreachable) {
		if(c->cdepth >= 2)
			c->cdepth--;

		return;
	}
	int bpos = c->cdepth - 1;
	int apos = c->cdepth - 2;
	/* deeper operand a が r0 (register-TOS) で rs/ri 対応 op なら a を spill せず r0 のまま使う。
	 * b は r0 でない (reg は1つ)。b=const→ri、b=slot→rs。setslot 削減の主レバー。 */
	if(c->reg_pos >= 0 && c->reg_pos == apos && tab->rs) {
		loc_t bb = c->st[bpos];
		if(bb.kind == L_CONST && tab->ri && !bb.is64) {
			c->last_op_widx = cb_emit_op(&c->cb, tab->ri);
			cb_emit_i32(&c->cb, (int32_t)bb.cval);
			c->last_mode = 4;
		} else {
			uint32_t bslot = to_slot(c, bpos); /* b→slot (L_SLOT は no-emit、r0=a 不変) */
			c->last_op_widx = cb_emit_op(&c->cb, tab->rs);
			cb_emit_u32(&c->cb, bslot);
			c->last_mode = 3;
		}
		c->cdepth -= 2;
		loc_t res = { L_REG, 0, 0, 0 };
		push_loc(c, res);
		c->reg_pos = c->cdepth - 1;
		c->bst_widx = -1; /* rs/ri は st 融合外 */
		return;
	}
	loc_t b = c->st[bpos];
	uint32_t aslot = to_slot(c, apos); /* a は常に slot 化 (aslot に確保) */
	b = c->st[bpos]; /* to_slot 後に再読込 (安全策) */
	/* b の変種 */
	coreop_t fn;
	int extra_imm = 0;
	int64_t imm = 0;
	uint32_t bslot = 0;
	int b_is_reg = 0;
	if(b.kind == L_REG && tab->sr) {
		fn = tab->sr;
		b_is_reg = 1;
	} else if(b.kind == L_CONST && tab->si && !b.is64) {
		fn = tab->si;
		extra_imm = 1;
		imm = b.cval;
	} else {
		bslot = to_slot(c, bpos);
		fn = tab->ss;
	}
	/* 結果は r0。b が reg なら消費で reg 解放。a が reg だったら to_slot で解放済。
	   どちらも reg でなく deeper reg が残れば spill。 */
	if(b_is_reg)
		c->reg_pos = -1;

	if(c->reg_pos >= 0)
		spill_reg(c);

	c->last_op_widx = cb_emit_op(&c->cb, fn);
	c->last_mode = b_is_reg ? 1 : (extra_imm ? 2 : 0);
	cb_emit_u32(&c->cb, aslot);
	if(fn == tab->ss)
		cb_emit_u32(&c->cb, bslot);
	else if(extra_imm)
		cb_emit_i32(&c->cb, (int32_t)imm);

	/* pop 2, push reg */
	c->cdepth -= 2;
	loc_t res = { L_REG, 0, 0, 0 };
	push_loc(c, res);
	c->reg_pos = c->cdepth - 1;
	/* binop; local.set 融合候補を記録 (st 対応 = 算術、ss/sr/si モードのみ)。 */
	if(tab->st_ss) {
		c->bst_widx = (int)c->last_op_widx;
		c->bst_tab = tab;
		c->bst_mode = c->last_mode;
		c->bst_pos = c->cdepth - 1;
		c->bst_cb_end = c->cb.len;
	} else {
		c->bst_widx = -1;
	}
}

/* compare op → 融合 br_if BINOP テーブル + 否定 index。
 * 順序: 0=eq,1=ne,2=lt_s,3=le_s,4=gt_s,5=ge_s,6=lt_u,7=le_u,8=gt_u,9=ge_u */
static corebinop_t* const g_brif_tab[10] = {
	&core_i32_eq_brif, &core_i32_ne_brif, &core_i32_lt_s_brif, &core_i32_le_s_brif, &core_i32_gt_s_brif,
	&core_i32_ge_s_brif, &core_i32_lt_u_brif, &core_i32_le_u_brif, &core_i32_gt_u_brif, &core_i32_ge_u_brif
};
static const int g_brif_neg[10] = { 1, 0, 5, 4, 3, 2, 9, 8, 7, 6 };

/* compare emit + 融合候補記録。直後が br_if/if なら 1 op に畳める。 */
static void emit_cmp(comp_t* c, corebinop_t* tab, int cmp_idx)
{
	emit_binop(c, tab);
	if(!c->unreachable) {
		c->fuse_widx   = (int)c->last_op_widx;
		c->fuse_cmp    = cmp_idx;
		c->fuse_mode   = c->last_mode;
		c->fuse_pos    = c->cdepth - 1;
		c->fuse_cb_end = c->cb.len;
	}
}

/* 単項 emit (r0 = OP r0) */
static void emit_unop(comp_t* c, coreop_t fn, int res64)
{
	if(c->unreachable)
		return;

	to_reg_top(c);
	cb_emit_op(&c->cb, fn);
	c->st[c->cdepth - 1].is64 = (uint8_t)res64;
}

/* ───── ヘルパ: WASM 読み ───── */
static uint32_t rdu(comp_t* c)
{
	uint32_t r = 0;
	uint32_t s = 0;
	uint8_t b;
	do {
		b = c->r.buf[c->r.pos++];
		r |= (uint32_t)(b & 0x7f) << s;
		s += 7;
	} while(b & 0x80);
	return r;
}
static uint64_t rdu64(comp_t* c)
{
	uint64_t r = 0;
	int s = 0;
	uint8_t b;
	do {
		b = c->r.buf[c->r.pos++];
		r |= (uint64_t)(b & 0x7f) << s;
		s += 7;
	} while(b & 0x80);
	return r;
}
static int32_t rds(comp_t* c)
{
	uint32_t r = 0;
	uint32_t s = 0;
	uint8_t b;
	do {
		b = c->r.buf[c->r.pos++];
		r |= (uint32_t)(b & 0x7f) << s;
		s += 7;
	} while(b & 0x80);
	if(s < 32 && (b & 0x40))
		r |= UINT32_MAX << s;

	return (int32_t)r;
}
static int64_t rds64(comp_t* c)
{
	uint64_t r = 0;
	int s = 0;
	uint8_t b;
	do {
		b = c->r.buf[c->r.pos++];
		r |= (uint64_t)(b & 0x7f) << s;
		s += 7;
	} while(b & 0x80);
	if(s < 64 && (b & 0x40))
		r |= UINT64_MAX << s;

	return (int64_t)r;
}
static uint8_t rdb(comp_t* c)
{
	return c->r.buf[c->r.pos++];
}
static uint64_t rdu64raw(comp_t* c)
{
	uint64_t v = 0;
	for(int i = 0; i < 8; i++)
		v |= (uint64_t)c->r.buf[c->r.pos++] << (8 * i);

	return v;
}
/* memarg を読み offset を返す。multi-memory では align の bit6 (0x40) が立ち memidx LEB が続く。
 * *memidx に解決した memory index を返す (フラグ無しは 0)。multi-memory load/store はこの
 * memidx で mems[] を切り替える。offset は memory64 で u64 LEB になるため 64bit で読む。 */
static uint64_t read_memarg_off(comp_t* c, uint32_t* memidx)
{
	uint32_t align = rdu(c);
	*memidx = (align & 0x40) ? rdu(c) : 0;
	return rdu64(c);
}
/* memidx のメモリが memory64 か (load/store の経路選択)。compile は core_activate 済みの
 * g_rt (build 中の対象インスタンス) を参照する。 */
static int mem_is64(uint32_t memidx)
{
	return g_rt->mems != NULL && memidx < g_rt->num_mems && g_rt->mems[memidx].is_64;
}

/* blocktype 読み → arity (0/1)。負数 (0x40=void) は 0、値型は 1、typeidx は params/results。 */
static int read_blocktype_arity(comp_t* c, int* in_arity)
{
	int64_t bt = rds64(c); /* SLEB */
	/* 0x40 empty */
	if(bt == -64) {
		*in_arity = 0;
		return 0;
	}
	/* 単一値型 */
	if(bt < 0) {
		*in_arity = 0;
		return 1;
	}
	/* typeidx (multi-value blocktype): params/results を返す (multi-value 対応済)。 */
	corefunctype_t* ft = &c->m->types[(uint32_t)bt];
	*in_arity = (int)ft->num_params;
	return (int)ft->num_results;
}

static void push_ctrl(comp_t* c, uint8_t kind, int arity)
{
	if(c->cctrl >= c->cap_ctrl) {
		c->cap_ctrl = c->cap_ctrl ? c->cap_ctrl * 2 : 16;
		c->ctrl = kinowasm_mem_realloc(c->ctrl, c->cap_ctrl * sizeof(ctrl_t));
		if(!c->ctrl)
			core_fatal("OOM ctrl");
	}
	ctrl_t* cf = &c->ctrl[c->cctrl++];
	memset(cf, 0, sizeof(*cf));
	cf->kind = kind;
	cf->arity = arity;
	cf->sp_base = c->cdepth;
	cf->entry_unr = c->unreachable; /* 入場時の dead 状態を記録 (end で復元) */
	if(kind == C_TRY)
		c->ntry++; /* fast path カウンタ: C_TRY push で加算 */
}
/* pop_ctrl — 最内の制御枠を 1 つ pop する。C_TRY を pop するときは ntry を減らし、
 * innermost_try / delegate_target_try の fast path カウンタを維持する。 */
static void pop_ctrl(comp_t* c)
{
	if(c->ctrl[c->cctrl - 1].kind == C_TRY)
		c->ntry--;

	c->cctrl--;
}
static void ctrl_add_fixup(ctrl_t* cf, uint32_t word_idx)
{
	if(cf->nfix >= cf->cfix) {
		cf->cfix = cf->cfix ? cf->cfix * 2 : 4;
		cf->fixups = kinowasm_mem_realloc(cf->fixups, cf->cfix * sizeof(uint32_t));
		if(!cf->fixups)
			core_fatal("OOM fixups");
	}
	cf->fixups[cf->nfix++] = word_idx;
}
/* EH: try_table body 内 call の dispatch 先 fixup を登録 (try の end で dispatch 位置へ patch)。 */
static void ctrl_add_disp(ctrl_t* cf, uint32_t word_idx)
{
	if(cf->ndisp >= cf->cdisp) {
		cf->cdisp = cf->cdisp ? cf->cdisp * 2 : 4;
		cf->disp_fixups = kinowasm_mem_realloc(cf->disp_fixups, cf->cdisp * sizeof(uint32_t));
		if(!cf->disp_fixups)
			core_fatal("OOM disp");
	}
	cf->disp_fixups[cf->ndisp++] = word_idx;
}
/* 最内の「body を compile 中」の C_TRY 枠 index (無ければ -1)。try 内の call/throw は例外時に
 * この try の dispatch へ分岐する。legacy try の handler 句 (in_handler=1) はその try 自身の保護
 * 対象外なので skip し外側の active try を探す (try_table は in_handler 常に 0 で常時 active)。 */
static int innermost_try(comp_t* c)
{
	/* try が無ければ走査不要 (try 外の call/throw は全部これ。最頻出ホットパス) */
	if(c->ntry == 0)
		return -1;

	for(int i = c->cctrl - 1; i >= 0; i--) {
		if(c->ctrl[i].kind == C_TRY && !c->ctrl[i].in_handler)
			return i;
	}
	return -1;
}
/* legacy delegate L の委譲先 try を解決。delegate の label L は try 自身の外側スコープ基準
 * (label 0 = try の直近の囲み枠)。target 枠 ctrl[cctrl-2-L] を起点に外側へ最初の body-phase
 * C_TRY を探す (target が try ならそれ、block 等なら外側の try、無ければ -1=caller 伝播)。 */
static int delegate_target_try(comp_t* c, uint32_t L)
{
	if(c->ntry == 0)
		return -1; /* try が無ければ delegate 先も無い */

	int ti = c->cctrl - 2 - (int)L;
	if(ti > c->cctrl - 2)
		ti = c->cctrl - 2;

	for(int i = ti; i >= 0; i--) {
		if(c->ctrl[i].kind == C_TRY && !c->ctrl[i].in_handler)
			return i;
	}
	return -1;
}

/* tagidx の例外パラメータ数 (= tag の functype の num_params)。 */
static uint32_t tag_nparams(coremodule_t* m, uint32_t tagidx)
{
	if(tagidx >= m->num_tags)
		return 0;

	uint32_t tyi = m->tag_typeidx[tagidx];
	return (tyi < m->num_types) ? m->types[tyi].num_params : 0;
}

/* branch/return/throw_local が枠を非局所脱出する際に pop すべき g_caught 段数
 * (= 脱出する legacy try の handler-phase 枠数)。handler の正常終了は end 側の caught_pop が
 * 釣り合わせるが、非局所脱出はそこを通らないため、脱出点で明示的に pop しないと台帳が
 * リークして以後の rethrow N (lexical 深さ) が別の例外を読む。first_idx 以上の枠が脱出対象
 * (loop への br は枠自身が継続するため target+1、それ以外は target を含む)。 */
static uint32_t count_handler_exits(comp_t* c, int first_idx)
{
	if(c->ntry == 0)
		return 0;

	uint32_t n = 0;
	for(int i = (first_idx < 0 ? 0 : first_idx); i < c->cctrl; i++) {
		if(c->ctrl[i].kind == C_TRY && c->ctrl[i].is_legacy && c->ctrl[i].in_handler)
			n++;
	}
	return n;
}

/* legacy try: 現セクション (body or 直前 handler) を閉じ、dispatch 入口を現在位置へ解決する。
 *   - 正常終了は R 結果を home slot に確定し end へ br (catch 句群を飛ばす)。
 *   - 初回 (in_handler==0): body 内 call_eh / throw_local の disp_fixups を現在位置へ解決し
 *     in_handler=1 に遷移 (以後この try は throw 保護対象外)。
 *   - 2回目以降: 直前 catch 句の no-match br を現在位置 (次の catch 句) へ解決。 */
static void legacy_close_section(comp_t* c, ctrl_t* cf)
{
	int R = cf->arity;
	int closing_handler = cf->in_handler; /* 1=handler を閉じる (正常終了で捕捉例外を pop)、0=body */
	if(!c->unreachable) {
		if(closing_handler)
			cb_emit_op(&c->cb, core_caught_pop); /* handler 正常終了 → 捕捉例外 pop */

		for(int k = 0; k < R; k++)
			to_home_slot(c, c->cdepth - R + k);

		cb_emit_op(&c->cb, core_br);
		uint32_t wi = cb_emit_tgt(&c->cb);
		ctrl_add_fixup(cf, wi);
	}
	if(!cf->in_handler) {
		for(uint32_t k = 0; k < cf->ndisp; k++) {
			c->cb.code[cf->disp_fixups[k]].u32 = c->cb.len;
			rec_loop(c, cf->disp_fixups[k]);
		}
		cf->in_handler = 1;
	} else if(cf->nomatch_fixup != 0xFFFFFFFFu) {
		c->cb.code[cf->nomatch_fixup].u32 = c->cb.len;
		rec_loop(c, cf->nomatch_fixup);
		cf->nomatch_fixup = 0xFFFFFFFFu;
	}
}

/* multi-value: block/loop/if 入場。params (top ia 値) を home slot に確定し operand base に並べる
 * (loop 後方ジャンプ / if 両アームが home slot から読むため)。sp_base は params を含む底。 */
static void enter_block(comp_t* c, uint8_t kind, int oa, int ia)
{
	if(!c->unreachable) {
		/* sp_base より下に live L_REG を残さない。r0 は枠の結果運搬 (br/fall-through) に使われ、
		 * end の追跡リセット (reg_pos=-1) 後は spill 契機が失われて値が孤立するため、入場時に確定する。 */
		if(c->reg_pos >= 0 && c->reg_pos < c->cdepth - ia)
			spill_reg(c);

		/* 外側に残る local 別名を実体化 (materialize_local_aliases のコメント参照)。 */
		materialize_local_aliases(c, c->cdepth - ia);

		for(int k = 0; k < ia; k++)
			to_home_slot(c, c->cdepth - ia + k);
	}
	push_ctrl(c, kind, oa);
	ctrl_t* cf = &c->ctrl[c->cctrl-1];
	cf->in_arity = ia;
	cf->sp_base = c->unreachable ? c->cdepth : (c->cdepth - ia); /* params を含む底 */
}

/* operand [src_base, src_base+n) の n 値を home slot 経由で [dst_base, dst_base+n) の
 * home slot へコピー (loop target / 多値 block target の運搬。block/if の arity-1 は r0 規約で別処理)。 */
static void carry_n_to_base(comp_t* c, int src_base, int dst_base, int n)
{
	for(int k = 0; k < n; k++) {
		uint32_t src = to_slot(c, src_base + k);
		uint32_t dst = home_slot(c, dst_base + k);
		if(src != dst) {
			cb_emit_op(&c->cb, core_copyslot);
			cb_emit_u32(&c->cb, dst);
			cb_emit_u32(&c->cb, src);
		}
	}
}

/* br/br_if: depth 番目の枠へ。loop は loop_start へ後方 (index 仮置き→後で絶対化)、
 * block/if は end へ forward (fixup list)。
 * 値運搬規約: block/param 無し if (forward) の arity-1 は r0 渡し (end 継続が r0 から受ける)。
 * home slot 受け target (loop / try / param 付き if。home_tgt) は arity に関わらず home slot 渡し —
 * これらの受け (loop 本体の params 読み / end 継続) は home slot (sp_base 起点) から読むため、
 * r0 渡しでは届かない。多値 (>=2) は block/if も home slot 渡し。
 * br_if で送値の下に残留 operand がある場合 (cdepth-A > sp_base)、分岐前 carry は非分岐側の
 * 残留 operand の home slot を汚染するため、taken 側 trampoline で運搬する。 */
static void emit_branch(comp_t* c, uint32_t depth, int conditional)
{
	ctrl_t* cf = &c->ctrl[c->cctrl - 1 - depth];
	int is_loop = (cf->kind == C_LOOP);
	/* home slot 受けの target: loop (params を home から読む)、try (legacy/try_table とも end が
	 * home から結果を受ける)、param 付き if (else 無し時の素通り経路が home 受けのため、br 時点で
	 * has_else 未確定でも常に home 規約に統一)。block/param 無し if の arity-1 は r0 受け。 */
	int home_tgt = is_loop || cf->kind == C_TRY || (cf->kind == C_IF && cf->in_arity > 0);
	/* loop は params (in_arity)、block/if は results (arity) を target slot へ運ぶ。 */
	int A = is_loop ? cf->in_arity : cf->arity;
	/* handler-phase 枠の非局所脱出で pop すべき g_caught 段数。br_if は taken 時のみ pop する
	 * 必要があるため npop>0 なら trampoline 化する (not-taken 側の台帳を壊さない)。 */
	uint32_t npop = count_handler_exits(c, (c->cctrl - 1 - (int)depth) + (is_loop ? 1 : 0));
	if(conditional) {
		if(A == 1 && !home_tgt) {
			/* arity-1 の block/if への br_if: 値を r0、cond を slot に確定。分岐/非分岐とも r0=値 を保つ。 */
			uint32_t cond_slot = to_slot(c, c->cdepth - 1); /* cond(top) → slot */
			c->cdepth--;                                    /* cond pop */
			to_reg_top(c);                                  /* 値(新top) → r0 */
			if(npop == 0) {
				cb_emit_op(&c->cb, core_br_if_v);
				uint32_t wi = cb_emit_tgt(&c->cb);
				ctrl_add_fixup(cf, wi);
				cb_emit_u32(&c->cb, cond_slot);
				return;
			}
			/* taken 側 trampoline: caught_pop×n → br target (r0 は運搬値のまま素通し)。 */
			cb_emit_op(&c->cb, core_br_if_v);
			uint32_t tw = cb_emit_tgt(&c->cb);
			cb_emit_u32(&c->cb, cond_slot);
			cb_emit_op(&c->cb, core_br);
			uint32_t aw = cb_emit_tgt(&c->cb);      /* not-taken → after */
			c->cb.code[tw].u32 = c->cb.len;
			rec_loop(c, tw);
			for(uint32_t k = 0; k < npop; k++)
				cb_emit_op(&c->cb, core_caught_pop);

			cb_emit_op(&c->cb, core_br);
			{
				uint32_t wi = cb_emit_tgt(&c->cb);
				ctrl_add_fixup(cf, wi);
			}
			c->cb.code[aw].u32 = c->cb.len;
			rec_loop(c, aw);
			return;
		}
		to_reg_top(c);                /* cond を r0 に */
		c->cdepth--;
		c->reg_pos = -1;              /* cond pop */
		if((A >= 1 && c->cdepth - A > cf->sp_base) || (A >= 1 && npop > 0)) {
			/* 残留 operand あり (または handler 脱出 pop あり): 送値を自 home slot に確定し、taken 側
			 * trampoline で pop + target の home slot へ運搬する (分岐前 copyslot だと非分岐側の
			 * 残留 operand を、分岐前 pop だと非分岐側の台帳を壊すため)。 */
			for(int k = 0; k < A; k++)
				to_home_slot(c, c->cdepth - A + k);

			uint32_t src_base = home_slot(c, c->cdepth - A);
			cb_emit_op(&c->cb, core_br_if);
			uint32_t tw = cb_emit_tgt(&c->cb);   /* taken → trampoline */
			cb_emit_op(&c->cb, core_br);
			uint32_t aw = cb_emit_tgt(&c->cb);   /* not-taken → after */
			c->cb.code[tw].u32 = c->cb.len;
			rec_loop(c, tw);
			for(uint32_t k = 0; k < npop; k++)
				cb_emit_op(&c->cb, core_caught_pop);

			for(int k = 0; k < A; k++) {
				uint32_t dst = home_slot(c, cf->sp_base + k);
				if(dst != src_base + k) {
					cb_emit_op(&c->cb, core_copyslot);
					cb_emit_u32(&c->cb, dst);
					cb_emit_u32(&c->cb, src_base + k);
				}
			}
			cb_emit_op(&c->cb, core_br);
			if(is_loop) {
				uint32_t wi = cb_emit_u32(&c->cb, cf->loop_start);
				rec_loop(c, wi);
			} else {
				uint32_t wi = cb_emit_tgt(&c->cb);
				ctrl_add_fixup(cf, wi);
			}
			c->cb.code[aw].u32 = c->cb.len;
			rec_loop(c, aw);
			return;
		}
		/* 残留なし: 分岐前 carry (送値自身の home への実体化なので非分岐側も汚染しない)。 */
		if(A >= 1)
			carry_n_to_base(c, c->cdepth - A, cf->sp_base, A);

		if(npop > 0) {
			/* A==0 で handler 脱出あり: taken 側 trampoline で pop してから分岐。 */
			cb_emit_op(&c->cb, core_br_if);
			uint32_t tw = cb_emit_tgt(&c->cb);
			cb_emit_op(&c->cb, core_br);
			uint32_t aw = cb_emit_tgt(&c->cb);
			c->cb.code[tw].u32 = c->cb.len;
			rec_loop(c, tw);
			for(uint32_t k = 0; k < npop; k++)
				cb_emit_op(&c->cb, core_caught_pop);

			cb_emit_op(&c->cb, core_br);
			if(is_loop) {
				uint32_t wi = cb_emit_u32(&c->cb, cf->loop_start);
				rec_loop(c, wi);
			} else {
				uint32_t wi = cb_emit_tgt(&c->cb);
				ctrl_add_fixup(cf, wi);
			}
			c->cb.code[aw].u32 = c->cb.len;
			rec_loop(c, aw);
			return;
		}
		cb_emit_op(&c->cb, core_br_if);
	} else {
		if(A == 1 && !home_tgt)
			to_reg_top(c);
		else if(A >= 1)
			carry_n_to_base(c, c->cdepth - A, cf->sp_base, A);

		/* handler-phase 枠の非局所脱出: 脱出分の caught_pop (無条件分岐なので直接 emit)。 */
		for(uint32_t k = 0; k < npop; k++)
			cb_emit_op(&c->cb, core_caught_pop);

		cb_emit_op(&c->cb, core_br);
		c->unreachable = 1;
	}
	if(is_loop) {
		uint32_t wi = cb_emit_u32(&c->cb, cf->loop_start);
		rec_loop(c, wi);
	} else {
		uint32_t wi = cb_emit_tgt(&c->cb);
		ctrl_add_fixup(cf, wi);
	}
}

/* compare+br_if/if 融合を試行。直前 compare(c->fuse_*) が TOS かつ未消費なら、その op を
 * 融合 br_if op に書換 + target word 追加 + cond pop。成功で 1。target_fixup は forward 用の
 * fixup 登録先 (br_if=対象 ctrl、if=後で else_fixup へ差し替えるため -1 を渡し呼出側で解決)。
 * negate=1 (if の then-skip) は否定 cmp を使う。tgt_loop_start>=0 なら loop 後方ジャンプ。 */
static int try_fuse_cmp_branch(comp_t* c, ctrl_t* fixup_cf, int negate, int is_loop, uint32_t loop_start)
{
	if(c->fuse_widx < 0)
		return 0;

	if(c->fuse_cb_end != c->cb.len)
		return 0;  /* compare 以後に emit があった */

	if(c->fuse_pos != c->cdepth - 1)
		return 0; /* compare 結果が TOS でない */

	if(c->reg_pos != c->cdepth - 1)
		return 0;  /* 結果が r0 でない */

	int cmp = negate ? g_brif_neg[c->fuse_cmp] : c->fuse_cmp;
	corebinop_t* bt = g_brif_tab[cmp];
	coreop_t fused = (c->fuse_mode == 0) ? bt->ss : (c->fuse_mode == 1) ? bt->sr : bt->si;
	c->cb.code[c->fuse_widx].op = fused; /* compare op を融合 op に */
	if(is_loop) {
		uint32_t wi = cb_emit_u32(&c->cb, loop_start);
		rec_loop(c, wi);
	} else {
		uint32_t wi = cb_emit_tgt(&c->cb);
		ctrl_add_fixup(fixup_cf, wi);
	}
	c->cdepth--;
	c->reg_pos = -1; /* cond pop */
	c->fuse_widx = -1;
	return 1;
}

/* binop; local.set x 融合: 直前 binop の結果を slot x へ直接書く (setslot 消去)。
 * 条件: st 対応 binop が直前で結果 TOS、cb 未変化、x の別名が結果より下の stack に無い
 * (あると x 書込後に古い値期待の読みが壊れる)。 */
static int try_fuse_binop_setslot(comp_t* c, uint32_t x)
{
	if(c->bst_widx < 0)
		return 0;

	if(c->bst_cb_end != c->cb.len)
		return 0;

	if(c->bst_pos != c->cdepth - 1)
		return 0;

	if(c->reg_pos != c->cdepth - 1)
		return 0;

	for(int i = 0; i < c->cdepth - 1; i++) {
		if(c->st[i].kind == L_SLOT && c->st[i].slot == (int32_t)x)
			return 0;
	}
	corebinop_t* t = c->bst_tab;
	coreop_t fused = (c->bst_mode == 0) ? t->st_ss : (c->bst_mode == 1) ? t->st_sr : t->st_si;
	if(!fused)
		return 0;

	c->cb.code[c->bst_widx].op = fused;
	cb_emit_u32(&c->cb, x); /* dst slot を末尾追加 */
	c->cdepth--;
	c->reg_pos = -1;        /* 結果(slot x)を pop */
	c->bst_widx = -1;
	return 1;
}

/* binop; local.tee x 融合: 直前 binop の結果を slot x へ書きつつ TOS(r0) にも残す (setslot 消去)。
 * try_fuse_binop_setslot と同条件だが、tee は値を pop せず継続するので reg_pos=top のまま残す
 * (st_* ハンドラが結果を r0 にも書くので TOS=結果 が成立)。 */
static int try_fuse_binop_tee(comp_t* c, uint32_t x)
{
	if(c->bst_widx < 0)
		return 0;

	if(c->bst_cb_end != c->cb.len)
		return 0;

	if(c->bst_pos != c->cdepth - 1)
		return 0;

	if(c->reg_pos != c->cdepth - 1)
		return 0;

	for(int i = 0; i < c->cdepth - 1; i++) {
		if(c->st[i].kind == L_SLOT && c->st[i].slot == (int32_t)x)
			return 0;
	}
	corebinop_t* t = c->bst_tab;
	coreop_t fused = (c->bst_mode == 0) ? t->st_ss : (c->bst_mode == 1) ? t->st_sr : t->st_si;
	if(!fused)
		return 0;

	c->cb.code[c->bst_widx].op = fused;
	cb_emit_u32(&c->cb, x); /* dst slot を末尾追加 */
	/* tee: pop せず TOS を r0 のまま (結果は st_* が r0 にも書く)。is64 は binop が設定済み。 */
	c->st[c->cdepth - 1].kind = L_REG;
	c->reg_pos = c->cdepth - 1;
	c->bst_widx = -1;
	return 1;
}

/* load; local.set x 融合: 直前 load の結果を slot x へ直接書く (setslot 消去)。
 * try_fuse_binop_setslot と同条件・同ロジック (load の st 版は結果を r0 にも残すので set/tee 兼用)。 */
static int try_fuse_load_setslot(comp_t* c, uint32_t x)
{
	if(c->lst_widx < 0)
		return 0;

	if(c->lst_cb_end != c->cb.len)
		return 0;

	if(c->lst_pos != c->cdepth - 1)
		return 0;

	if(c->reg_pos != c->cdepth - 1)
		return 0;

	for(int i = 0; i < c->cdepth - 1; i++) {
		if(c->st[i].kind == L_SLOT && c->st[i].slot == (int32_t)x)
			return 0;
	}
	c->cb.code[c->lst_widx].op = c->lst_st_fn; /* load op を st 版へ */
	cb_emit_u32(&c->cb, x); /* dst slot を末尾追加 */
	c->cdepth--;
	c->reg_pos = -1;        /* 結果(slot x)を pop */
	c->lst_widx = -1;
	return 1;
}

/* load; local.tee x 融合: 結果を slot x へ書きつつ TOS(r0) にも残す。set と同条件で pop しないだけ。 */
static int try_fuse_load_tee(comp_t* c, uint32_t x)
{
	if(c->lst_widx < 0)
		return 0;

	if(c->lst_cb_end != c->cb.len)
		return 0;

	if(c->lst_pos != c->cdepth - 1)
		return 0;

	if(c->reg_pos != c->cdepth - 1)
		return 0;

	for(int i = 0; i < c->cdepth - 1; i++) {
		if(c->st[i].kind == L_SLOT && c->st[i].slot == (int32_t)x)
			return 0;
	}
	c->cb.code[c->lst_widx].op = c->lst_st_fn;
	cb_emit_u32(&c->cb, x);
	/* tee: pop せず TOS を r0 のまま (st 版が r0 にも結果を書く)。is64 は load が設定済み。 */
	c->reg_pos = c->cdepth - 1;
	c->lst_widx = -1;
	return 1;
}

void core_compile_func(coremodule_t* m, uint32_t def_idx)
{
	corefuncdef_t* f = &m->funcs[def_idx];
	corefunctype_t* ft = &m->types[f->type_idx];
	/* 多値結果 (num_results>=2): 暗黙の関数 block が R 結果を home slot に確定し、ret_multi で
	 * グローバル mret バッファへ書き出す (call 側は mret_get で取り込む)。単一/0 は従来の r0 規約。 */
	comp_t C;
	memset(&C, 0, sizeof(C));
	C.m = m;
	C.f = f;
	C.reg_pos = -1;
	C.fuse_widx = -1;
	C.bst_widx = -1;
	C.lst_widx = -1;
	C.num_params = ft->num_params;
	C.fb = ft->num_params + f->num_locals;
	C.max_slot = C.fb;
	C.r.buf = f->code_ptr;
	C.r.pos = 0;
	C.r.end = (size_t)(f->code_end - f->code_ptr);

	/* codebuf を本体バイト数から概算で初期確保し、倍々 realloc の中間コピー回数を削減する
	 * (余剰は末尾 trim で回収)。1 命令あたり概ね 1 word 前後なので body 長を初期 cap の目安にする。 */
	if(C.r.end > 0)
		cb_reserve(&C.cb, (uint32_t)(C.r.end < 256 ? 256 : C.r.end));

	/* 関数本体は暗黙の block (arity = num_results)。 */
	push_ctrl(&C, C_BLOCK, (int)ft->num_results);

	while(C.r.pos < C.r.end) {
		uint8_t op = rdb(&C);
		/* 制御フロー境界 (block/loop/if/else/end = 0x02-0x0b) を跨ぐ binop;local.set 融合は
		   br が end へ運ぶ値を fused st op が無視するため壊れる。境界で bst/lst を無効化。 */
		if(op >= 0x02 && op <= 0x0b) {
			C.bst_widx = -1;
			C.lst_widx = -1;
		}

		switch(op) {
		/* unreachable: trap する。 */
		case 0x00:
			if(!C.unreachable)
				cb_emit_op(&C.cb, core_unreachable);

			C.unreachable = 1;
			break;
		case 0x01: /* nop */
			break;
		case 0x02: {
			int ia;
			int oa = read_blocktype_arity(&C, &ia);
			enter_block(&C, C_BLOCK, oa, ia);
			break;
		}
		case 0x03: {
			int ia;
			int oa = read_blocktype_arity(&C, &ia);
			enter_block(&C, C_LOOP, oa, ia);
			C.ctrl[C.cctrl - 1].loop_start = C.cb.len;
			break;
		}
		/* if: cond=top。cond==0 で else/end へ (br_if_not)。 */
		case 0x04: {
			int ia;
			int oa = read_blocktype_arity(&C, &ia);
			uint32_t wi;
			/* dead code 中の if: cond は stack に無く (空)、本体も dead。分岐を emit せず nesting のみ
			 * 追跡する (else_fixup=無効印)。これを怠ると to_reg_top が C.st[-1] を触り crash する。 */
			if(C.unreachable) {
				push_ctrl(&C, C_IF, oa);
				C.ctrl[C.cctrl - 1].else_fixup = 0xFFFFFFFFu;
				C.ctrl[C.cctrl - 1].in_arity = ia;
				break;
			}
			/* 外側に残る local 別名を実体化 (materialize_local_aliases のコメント参照)。
			 * 必ず分岐命令より前に置くこと。分岐の後ろに置くと else 側へ飛んだ経路で
			 * copyslot が実行されず、同じ不具合を作り込む。cond (TOS) は to_reg_top で
			 * r0 へ移すので対象外。 */
			materialize_local_aliases(&C, C.cdepth - 1);
			if(ia > 0) {
				/* if with params: cond を r0 へ確定し pop、ia 個の param を home slot ([cdepth-ia..)) へ
				 * 確定してから分岐。実行時は片アームのみ走るので param slot は他アームでも保たれる。
				 * 融合は使わず単純化 (param 確定が compare の後に挟まるため)。 */
				to_reg_top(&C);
				C.cdepth--;
				C.reg_pos = -1; /* cond → r0, pop */
				for(int k = 0; k < ia; k++)
					to_home_slot(&C, C.cdepth - ia + k);

				cb_emit_op(&C.cb, core_br_if_not);
				wi = cb_emit_tgt(&C.cb);
				push_ctrl(&C, C_IF, oa);
				C.ctrl[C.cctrl - 1].else_fixup = wi;
				C.ctrl[C.cctrl - 1].in_arity = ia;
				C.ctrl[C.cctrl - 1].sp_base = C.cdepth - ia;
				break;
			}
			/* compare 直前なら否定 cmp の融合 brif で then-skip を 1 op 化 (cond!=cmp で else へ)。 */
			if(!C.unreachable && C.fuse_widx >= 0 && C.fuse_cb_end == C.cb.len
				&& C.fuse_pos == C.cdepth - 1 && C.reg_pos == C.cdepth - 1) {
				corebinop_t* bt = g_brif_tab[g_brif_neg[C.fuse_cmp]];
				coreop_t fused = (C.fuse_mode == 0) ? bt->ss : (C.fuse_mode == 1) ? bt->sr : bt->si;
				C.cb.code[C.fuse_widx].op = fused;
				wi = cb_emit_tgt(&C.cb);
				C.cdepth--;
				C.reg_pos = -1;
				C.fuse_widx = -1;
			} else {
				to_reg_top(&C);
				C.cdepth--;
				C.reg_pos = -1;
				cb_emit_op(&C.cb, core_br_if_not);
				wi = cb_emit_tgt(&C.cb);
			}
			push_ctrl(&C, C_IF, oa);
			C.ctrl[C.cctrl - 1].else_fixup = wi;
			C.ctrl[C.cctrl - 1].in_arity = ia;
			break;
		}
		case 0x05: { /* else */
			ctrl_t* cf = &C.ctrl[C.cctrl - 1];
			/* dead code 中の if (else_fixup 無効印): else アームも dead。分岐 emit / fixup patch せず、
			 * nesting だけ更新し unreachable を維持する。 */
			if(cf->else_fixup == 0xFFFFFFFFu) {
				cf->has_else = 1;
				C.cdepth = cf->sp_base;
				C.reg_pos = -1;
				break;
			}
			/* then アームの結果を継続位置へ確定: arity 1 → r0 (param 付き if は home slot 規約に統一)、
			 * arity>=2 → home slot ([sp_base, +R))。end の home_res / emit_branch の home_tgt と同一規約。 */
			if(!C.unreachable) {
				if(cf->arity == 1 && cf->in_arity == 0) {
					to_reg_top(&C);
				} else if(cf->arity >= 1) {
					for(int k = 0; k < cf->arity; k++)
						to_home_slot(&C, C.cdepth - cf->arity + k);
				}
			}
			cb_emit_op(&C.cb, core_br);
			uint32_t wi = cb_emit_tgt(&C.cb);
			ctrl_add_fixup(cf, wi);
			/* else_fixup を else 先頭(=現在地 index)へ。realloc 耐性のため index 仮置き+最終解決。 */
			C.cb.code[cf->else_fixup].u32 = C.cb.len;
			rec_loop(&C, cf->else_fixup);
			cf->has_else = 1;
			C.cdepth = cf->sp_base;
			C.reg_pos = -1;
			C.unreachable = 0;
			/* if with params: else アームに param を再公開 (home slot は then 入場時に確定済、
			 * 実行時 then 未走行なら値は保持)。L_SLOT で operand stack を再構築。 */
			for(int k = 0; k < cf->in_arity; k++) {
				loc_t p = { L_SLOT, 0, (int32_t)home_slot(&C, cf->sp_base + k), 0 };
				push_loc(&C, p);
			}
			break;
		}
		/* throw tagidx。同一関数内 active try があれば dispatch へ分岐 (throw_local)、
		 * 無ければ現関数を抜けて伝播 (core_throw)。enclosing try (caller) は call_eh が捕捉。 */
		case 0x08: {
			uint32_t tagidx = rdu(&C);
			if(!C.unreachable) {
				uint32_t nv = tag_nparams(m, tagidx);
				if(C.reg_pos >= 0 && C.reg_pos < (int)(C.cdepth - (int)nv))
					spill_reg(&C);

				for(int i = (int)(C.cdepth - (int)nv); i < C.cdepth; i++) {
					if(i >= 0)
						to_home_slot(&C, i);
				}
				int it = innermost_try(&C);
				if(it >= 0) {
					/* 同一関数 try 内: 状態を立てて dispatch へ musttail (br 相当)。
					 * dispatch 先 try より内側の handler を脱出する分の caught_pop
					 * (caller 伝播側は call_eh の watermark が清算するため不要)。 */
					uint32_t np = count_handler_exits(&C, it + 1);
					for(uint32_t k = 0; k < np; k++)
						cb_emit_op(&C.cb, core_caught_pop);

					cb_emit_op(&C.cb, core_throw_local);
					cb_emit_u32(&C.cb, tagidx);
					cb_emit_u32(&C.cb, nv);
					for(uint32_t k = 0; k < nv; k++)
						cb_emit_u32(&C.cb, home_slot(&C, C.cdepth - (int)nv + (int)k));

					uint32_t wi = cb_emit_tgt(&C.cb);
					ctrl_add_disp(&C.ctrl[it], wi);
				} else {
					cb_emit_op(&C.cb, core_throw);
					cb_emit_u32(&C.cb, tagidx);
					cb_emit_u32(&C.cb, nv);
					for(uint32_t k = 0; k < nv; k++)
						cb_emit_u32(&C.cb, home_slot(&C, C.cdepth - (int)nv + (int)k));
				}
				C.cdepth -= (int)nv;
				C.reg_pos = -1;
				C.unreachable = 1;
			}
			break;
		}
		/* throw_ref: stack top の exnref を再 throw。null は trap。 */
		case 0x0A: {
			if(!C.unreachable) {
				uint32_t exn_slot = to_slot(&C, C.cdepth - 1);
				C.cdepth--;
				C.reg_pos = -1;
				int it = innermost_try(&C);
				if(it >= 0) {
					/* 脱出 handler 分の caught_pop (throw_local と同様)。caught_pop は legacy 台帳
					 * g_caught を下げるだけで、exnref の読み (exn_restore) は別配列 g_exn を読む
					 * ため影響を受けない。 */
					uint32_t np = count_handler_exits(&C, it + 1);
					for(uint32_t k = 0; k < np; k++)
						cb_emit_op(&C.cb, core_caught_pop);

					cb_emit_op(&C.cb, core_throw_ref_local);
					cb_emit_u32(&C.cb, exn_slot);
					uint32_t wi = cb_emit_tgt(&C.cb);
					ctrl_add_disp(&C.ctrl[it], wi);
				} else {
					cb_emit_op(&C.cb, core_throw_ref);
					cb_emit_u32(&C.cb, exn_slot);
				}
				C.unreachable = 1;
			}
			break;
		}
		/* legacy try blocktype: handler 句 (catch/catch_all/delegate) が end まで連なる。 */
		case 0x06: {
			int ia;
			int oa = read_blocktype_arity(&C, &ia);
			enter_block(&C, C_TRY, oa, ia);
			C.ctrl[C.cctrl - 1].is_legacy = 1;
			C.ctrl[C.cctrl - 1].in_handler = 0;
			C.ctrl[C.cctrl - 1].nomatch_fixup = 0xFFFFFFFFu;
			break;
		}
		/* legacy catch tagidx: body/直前 handler を閉じ、tag 照合 dispatch + handler 開始。 */
		case 0x07: {
			uint32_t tag = rdu(&C);
			ctrl_t* cf = &C.ctrl[C.cctrl - 1];
			legacy_close_section(&C, cf);
			uint32_t nv = tag_nparams(m, tag);
			uint32_t dst = home_slot(&C, cf->sp_base);
			cb_emit_op(&C.cb, core_catch);
			cb_emit_u32(&C.cb, tag);
			cb_emit_u32(&C.cb, nv);
			cb_emit_u32(&C.cb, dst);
			uint32_t tgt_wi = cb_emit_tgt(&C.cb);           /* match → handler 先頭 */
			cb_emit_op(&C.cb, core_br);
			uint32_t nm_wi = cb_emit_tgt(&C.cb);            /* no-match → 次 dispatch */
			cf->nomatch_fixup = nm_wi;
			C.cb.code[tgt_wi].u32 = C.cb.len;
			rec_loop(&C, tgt_wi);                           /* handler 先頭を解決 */
			/* handler 入場: 例外値が home slot [sp_base..sp_base+nv) に置かれた状態で operand stack 再構築。 */
			C.cdepth = cf->sp_base;
			C.reg_pos = -1;
			C.unreachable = 0;
			for(uint32_t k = 0; k < nv; k++) {
				loc_t p = { L_SLOT, 0, (int32_t)home_slot(&C, cf->sp_base + (int)k), 0 };
				push_loc(&C, p);
			}
			break;
		}
		/* legacy catch_all: body/直前 handler を閉じ、無条件捕捉 handler 開始 (値運搬なし)。 */
		case 0x19: {
			ctrl_t* cf = &C.ctrl[C.cctrl - 1];
			legacy_close_section(&C, cf);
			cb_emit_op(&C.cb, core_catch_all);
			uint32_t tgt_wi = cb_emit_tgt(&C.cb);
			cf->nomatch_fixup = 0xFFFFFFFFu; /* catch_all は常に match。後続 catch 句は無い */
			C.cb.code[tgt_wi].u32 = C.cb.len;
			rec_loop(&C, tgt_wi);
			C.cdepth = cf->sp_base;
			C.reg_pos = -1;
			C.unreachable = 0;
			break;
		}
		/* legacy rethrow N: label N の catch ブロックが捕捉した例外を再 throw。 */
		case 0x09: {
			uint32_t N = rdu(&C);
			if(!C.unreachable) {
				/* g_caught 深さ J = target より内側の handler-phase try 数 (rethrow が読む stack offset)。 */
				int ti = C.cctrl - 1 - (int)N;
				uint32_t J = 0;
				for(int i = (ti < 0 ? 0 : ti + 1); i < C.cctrl; i++) {
					if(C.ctrl[i].kind == C_TRY && C.ctrl[i].in_handler)
						J++;
				}
				int it = innermost_try(&C);
				if(it >= 0) {
					/* 同一関数 try が再捕捉: dispatch へ musttail。
					 * npop = dispatch 先より内側の handler 脱出分。J (lexical 深さ) の読みは pop 前の
					 * 台帳基準なので、H_rethrow_local が読んでから pop する (operand 渡し)。
					 * caller 伝播 (core_rethrow) は呼出元 call_eh の watermark が清算するため不要。 */
					cb_emit_op(&C.cb, core_rethrow_local);
					cb_emit_u32(&C.cb, J);
					cb_emit_u32(&C.cb, count_handler_exits(&C, it + 1));
					uint32_t wi = cb_emit_tgt(&C.cb);
					ctrl_add_disp(&C.ctrl[it], wi);
				} else {
					/* caller へ伝播 */
					cb_emit_op(&C.cb, core_rethrow);
					cb_emit_u32(&C.cb, J);
				}
				C.reg_pos = -1;
				C.unreachable = 1;
			}
			break;
		}
		/* legacy delegate L: try を閉じ body の例外を label L の外側 active try へ委譲。 */
		case 0x18: {
			uint32_t L = rdu(&C);
			ctrl_t* cf = &C.ctrl[C.cctrl - 1];
			int R = cf->arity;
			if(!C.unreachable) {
				for(int k = 0; k < R; k++)
					to_home_slot(&C, C.cdepth - R + k);
			}
			int tgt = delegate_target_try(&C, L);
			if(tgt >= 0) {
				/* 外側 try が捕捉: body の disp_fixups をその try の disp へ移譲 (後で dispatch に解決)。 */
				uint32_t np = count_handler_exits(&C, tgt + 1);
				if(np == 0) {
					for(uint32_t k = 0; k < cf->ndisp; k++)
						ctrl_add_disp(&C.ctrl[tgt], cf->disp_fixups[k]);
				} else {
					/* 委譲先との間に handler-phase 枠が挟まる (catch 内の try...delegate): 例外は
					 * それらを脱出するため、caught_pop trampoline を経由して委譲先 dispatch へ飛ばす。
					 * 正常経路は trampoline を踏まないよう skip する。 */
					uint32_t skip2 = 0xFFFFFFFFu;
					if(!C.unreachable) {
						cb_emit_op(&C.cb, core_br);
						skip2 = cb_emit_tgt(&C.cb);
						ctrl_add_fixup(cf, skip2);
					}
					uint32_t tramp = C.cb.len;
					for(uint32_t k = 0; k < cf->ndisp; k++) {
						C.cb.code[cf->disp_fixups[k]].u32 = tramp;
						rec_loop(&C, cf->disp_fixups[k]);
					}
					for(uint32_t k = 0; k < np; k++)
						cb_emit_op(&C.cb, core_caught_pop);

					cb_emit_op(&C.cb, core_br);
					uint32_t wi3 = cb_emit_tgt(&C.cb);
					ctrl_add_disp(&C.ctrl[tgt], wi3);
				}
			} else {
				/* caller へ伝播: 正常路を飛ばし terminator(rethrow_uncaught) を置いて disp を解決。 */
				uint32_t skip_wi = 0xFFFFFFFFu;
				if(!C.unreachable) {
					cb_emit_op(&C.cb, core_br);
					skip_wi = cb_emit_tgt(&C.cb);
					ctrl_add_fixup(cf, skip_wi);
				}
				for(uint32_t k = 0; k < cf->ndisp; k++) {
					C.cb.code[cf->disp_fixups[k]].u32 = C.cb.len;
					rec_loop(&C, cf->disp_fixups[k]);
				}
				cb_emit_op(&C.cb, core_rethrow_uncaught);
			}
			for(uint32_t k = 0; k < cf->nfix; k++) {
				C.cb.code[cf->fixups[k]].u32 = C.cb.len;
				rec_loop(&C, cf->fixups[k]);
			}
			C.cdepth = cf->sp_base;
			C.reg_pos = -1;
			for(int k = 0; k < R; k++) {
				loc_t res = { L_SLOT, 0, (int32_t)home_slot(&C, cf->sp_base + k), 0 };
				push_loc(&C, res);
			}
			kinowasm_mem_free(cf->fixups);
			kinowasm_mem_free(cf->disp_fixups);
			pop_ctrl(&C);
			C.unreachable = cf->entry_unr;
			if(C.cctrl == 0) {
				if(R >= 2) {
					cb_emit_op(&C.cb, core_ret_multi);
					cb_emit_u32(&C.cb, (uint32_t)R);
					cb_emit_u32(&C.cb, C.fb);
				} else {
					cb_emit_op(&C.cb, core_ret);
				}
			}
			break;
		}
		/* try_table blocktype vec(catch)。catch 句を枠に記録し end で dispatch を emit。 */
		case 0x1F: {
			int ia;
			int oa = read_blocktype_arity(&C, &ia);
			uint32_t nc = rdu(&C);
			corecatch_t* catches = nc ? (corecatch_t*)kinowasm_mem_malloc(sizeof(corecatch_t) * nc) : NULL;
			if(nc && !catches)
				core_fatal("OOM catches");

			for(uint32_t k = 0; k < nc; k++) {
				uint8_t ck = C.r.buf[C.r.pos++];
				catches[k].kind = ck;
				if(ck == 0x00 || ck == 0x01) {
					catches[k].tag = rdu(&C);
					catches[k].label = rdu(&C);
				} else {
					catches[k].tag = 0;
					catches[k].label = rdu(&C);
				}
			}
			enter_block(&C, C_TRY, oa, ia);
			C.ctrl[C.cctrl - 1].catches = catches;
			C.ctrl[C.cctrl - 1].ncatch = nc;
			break;
		}
		case 0x0b: { /* end */
			ctrl_t* cf = &C.ctrl[C.cctrl - 1];
			int R = cf->arity;
			if(cf->kind == C_TRY && cf->is_legacy) {
				/* legacy try end: 末尾 dispatch terminator + end ラベル。
				 * 最終セクション (catchless try の body、または最終 handler) を閉じる。 */
				if(!C.unreachable) {
					if(cf->in_handler)
						cb_emit_op(&C.cb, core_caught_pop); /* 最終 handler 正常終了 → 捕捉例外 pop */

					for(int k = 0; k < R; k++)
						to_home_slot(&C, C.cdepth - R + k);

					cb_emit_op(&C.cb, core_br);
					uint32_t wi = cb_emit_tgt(&C.cb);
					ctrl_add_fixup(cf, wi);
				}
				/* dispatch 入口を terminator 位置へ解決。 */
				if(!cf->in_handler) {
					for(uint32_t k = 0; k < cf->ndisp; k++) {
						C.cb.code[cf->disp_fixups[k]].u32 = C.cb.len;
						rec_loop(&C, cf->disp_fixups[k]);
					}
				} else if(cf->nomatch_fixup != 0xFFFFFFFFu) {
					C.cb.code[cf->nomatch_fixup].u32 = C.cb.len;
					rec_loop(&C, cf->nomatch_fixup);
				}
				cf->in_handler = 1; /* 以後 innermost_try が自分を skip (terminator の外側探索用) */
				/* terminator: 同一関数の外側 active try があればその dispatch へ再伝播、無ければ caller へ return。
				 * 再伝播は [ot+1..) の handler-phase 枠を脱出するため caught_pop で釣り合わせる。
				 * cf 自身は直前で in_handler=1 に設定済みだが、terminator 到達 = 全 catch no-match で
				 * caught は積んでいないため 1 段除外する。caller 伝播 (rethrow_uncaught) は呼出元
				 * call_eh の watermark が清算するため pop 不要。 */
				int ot = innermost_try(&C);
				if(ot >= 0) {
					uint32_t np = count_handler_exits(&C, ot + 1);
					if(np > 0)
						np--;

					for(uint32_t k = 0; k < np; k++)
						cb_emit_op(&C.cb, core_caught_pop);

					cb_emit_op(&C.cb, core_br);
					uint32_t wi2 = cb_emit_tgt(&C.cb);
					ctrl_add_disp(&C.ctrl[ot], wi2);
				} else {
					cb_emit_op(&C.cb, core_rethrow_uncaught);
				}
				/* end ラベル: skip-br 群 (各セクション正常終了) + br depth (try ブロック宛) を現在位置へ。 */
				for(uint32_t k = 0; k < cf->nfix; k++) {
					C.cb.code[cf->fixups[k]].u32 = C.cb.len;
					rec_loop(&C, cf->fixups[k]);
				}
				C.cdepth = cf->sp_base;
				C.reg_pos = -1;
				for(int k = 0; k < R; k++) {
					loc_t res = { L_SLOT, 0, (int32_t)home_slot(&C, cf->sp_base + k), 0 };
					push_loc(&C, res);
				}
				kinowasm_mem_free(cf->fixups);
				kinowasm_mem_free(cf->disp_fixups);
				pop_ctrl(&C);
				C.unreachable = cf->entry_unr;
				if(C.cctrl == 0) {
					if(R >= 2) {
						cb_emit_op(&C.cb, core_ret_multi);
						cb_emit_u32(&C.cb, (uint32_t)R);
						cb_emit_u32(&C.cb, C.fb);
					} else {
						cb_emit_op(&C.cb, core_ret);
					}
				}
				break;
			}
			if(cf->kind == C_TRY) {
				/* try_table end: 正常終了は dispatch を飛ばし、例外は catch 句で照合。 */
				if(!C.unreachable) {
					for(int k = 0; k < R; k++)
						to_home_slot(&C, C.cdepth - R + k);
				}
				uint32_t skip_wi = 0xFFFFFFFFu;
				if(!C.unreachable) {
					cb_emit_op(&C.cb, core_br);
					skip_wi = cb_emit_tgt(&C.cb);
				}
				/* dispatch 開始: body 内 call_eh の dispatch fixup を現在位置へ。 */
				for(uint32_t k = 0; k < cf->ndisp; k++) {
					C.cb.code[cf->disp_fixups[k]].u32 = C.cb.len;
					rec_loop(&C, cf->disp_fixups[k]);
				}
				/* catch 句: tag 一致なら値運搬+target ラベルへ分岐。kind 0x01=catch_ref / 0x03=catch_all_ref は
				 * exnref を値の直後に追加配置 (target ブロックの arity は values+1)。 */
				/* catch/catch_all は no-track 変種 (g_caught へ push しない): try_table に rethrow は
				 * 無く、push すると pop 経路が構造上無いため台帳が汚れる。exnref を作る ref 系
				 * (0x01/0x03) は handle が g_caught index 参照のため push 版を使う。 */
				for(uint32_t k = 0; k < cf->ncatch; k++) {
					corecatch_t* cc = &cf->catches[k];
					ctrl_t* tcf = &C.ctrl[C.cctrl - 2 - (int)cc->label];
					int is_loop = (tcf->kind == C_LOOP);
					if(cc->kind == 0x00) {
						uint32_t nv = tag_nparams(m, cc->tag);
						cb_emit_op(&C.cb, core_catch_nt);
						cb_emit_u32(&C.cb, cc->tag);
						cb_emit_u32(&C.cb, nv);
						cb_emit_u32(&C.cb, home_slot(&C, tcf->sp_base));
					} else if(cc->kind == 0x01) {
						uint32_t nv = tag_nparams(m, cc->tag);
						cb_emit_op(&C.cb, core_catch_ref);
						cb_emit_u32(&C.cb, cc->tag);
						cb_emit_u32(&C.cb, nv);
						cb_emit_u32(&C.cb, home_slot(&C, tcf->sp_base));
					} else if(cc->kind == 0x03) {
						/* catch_all_ref: exnref を tcf->sp_base へ置く */
						cb_emit_op(&C.cb, core_catch_all_ref);
						cb_emit_u32(&C.cb, home_slot(&C, tcf->sp_base));
					} else {
						/* 0x02 catch_all */
						cb_emit_op(&C.cb, core_catch_all_nt);
					}
					/* match 時の分岐先 (loop は後方ジャンプ、それ以外は end へ forward fixup)。 */
					if(is_loop) {
						uint32_t wi = cb_emit_u32(&C.cb, tcf->loop_start);
						rec_loop(&C, wi);
					} else {
						uint32_t wi = cb_emit_tgt(&C.cb);
						ctrl_add_fixup(tcf, wi);
					}
				}
				cb_emit_op(&C.cb, core_rethrow_uncaught); /* どの catch にも一致せず → 再伝播 (return) */
				/* block_end: block fixups (br to try) + 正常終了 skip-br を現在位置へ。 */
				for(uint32_t k = 0; k < cf->nfix; k++) {
					C.cb.code[cf->fixups[k]].u32 = C.cb.len;
					rec_loop(&C, cf->fixups[k]);
				}
				if(skip_wi != 0xFFFFFFFFu) {
					C.cb.code[skip_wi].u32 = C.cb.len;
					rec_loop(&C, skip_wi);
				}
				C.cdepth = cf->sp_base;
				C.reg_pos = -1;
				for(int k = 0; k < R; k++) {
					loc_t res = { L_SLOT, 0, (int32_t)home_slot(&C, cf->sp_base + k), 0 };
					push_loc(&C, res);
				}
				kinowasm_mem_free(cf->fixups);
				kinowasm_mem_free(cf->catches);
				kinowasm_mem_free(cf->disp_fixups);
				pop_ctrl(&C);
				C.unreachable = cf->entry_unr;
				if(C.cctrl == 0) {
					if(R >= 2) {
						cb_emit_op(&C.cb, core_ret_multi);
						cb_emit_u32(&C.cb, (uint32_t)R);
						cb_emit_u32(&C.cb, C.fb);
					} else {
						cb_emit_op(&C.cb, core_ret);
					}
				}
				break;
			}
			/* if-with-params: else 無し時は cond==0 で param が結果へ素通りし home slot に居る。br は
			   has_else 未確定の時点で emit されるため、param 付き if は has_else に関わらず home slot 規約
			   (emit_branch の home_tgt と同一)。それ以外は arity1=r0 / arity>=2=home slot。 */
			int home_res = (cf->kind == C_IF && cf->in_arity > 0) || R >= 2;
			/* fall-through 結果を継続位置へ確定。 */
			if(!C.unreachable) {
				if(R == 1 && !home_res) {
					to_reg_top(&C);
				} else if(R >= 1 && home_res) {
					for(int k = 0; k < R; k++)
						to_home_slot(&C, C.cdepth - R + k);
				}
			}
			/* else 無しの if は else_fixup を end へ。dead code の if (無効印) は分岐未 emit なので patch しない。 */
			if(cf->kind == C_IF && !cf->has_else && cf->else_fixup != 0xFFFFFFFFu) {
				C.cb.code[cf->else_fixup].u32 = C.cb.len;
				rec_loop(&C, cf->else_fixup);
			}
			/* block/if の forward fixup (br 群) を end 位置(index)へ。最終に一括ポインタ化。 */
			for(uint32_t k = 0; k < cf->nfix; k++) {
				C.cb.code[cf->fixups[k]].u32 = C.cb.len;
				rec_loop(&C, cf->fixups[k]);
			}
			/* operand stack を block base + arity にリセット。 */
			C.cdepth = cf->sp_base;
			C.reg_pos = -1;
			if(R == 1 && !home_res) {
				loc_t res = { L_REG, 0, 0, 0 };
				push_loc(&C, res);
				C.reg_pos = C.cdepth - 1;
			} else if(R >= 1 && home_res) {
				for(int k = 0; k < R; k++) {
					loc_t res = { L_SLOT, 0, (int32_t)home_slot(&C, cf->sp_base + k), 0 };
					push_loc(&C, res);
				}
			}
			kinowasm_mem_free(cf->fixups);
			pop_ctrl(&C);
			/* 入場時 dead だった block は継続も dead。単一 unreachable フラグが nest しない問題を
			   ここで吸収する (到達可能 block は entry_unr=0 で従来通り reachable 継続)。 */
			C.unreachable = cf->entry_unr;
			/* 関数末尾: 多値は ret_multi (結果は home slot [fb..fb+R))、それ以外 ret。 */
			if(C.cctrl == 0) {
				if(R >= 2) {
					cb_emit_op(&C.cb, core_ret_multi);
					cb_emit_u32(&C.cb, (uint32_t)R);
					cb_emit_u32(&C.cb, C.fb);
				} else {
					cb_emit_op(&C.cb, core_ret);
				}
			}
			break;
		}
		case 0x0c: {
			uint32_t d = rdu(&C);
			if(!C.unreachable)
				emit_branch(&C, d, 0);

			break;
		}
		case 0x0d: {
			uint32_t d = rdu(&C);
			if(!C.unreachable) {
				ctrl_t* cf = &C.ctrl[C.cctrl - 1 - d];
				int is_loop = (cf->kind == C_LOOP);
				int tgt_arity = is_loop ? cf->in_arity : cf->arity;
				/* compare 直前なら 1 op 融合 (arity-0 のみ。arity>=1 は値運搬要で br_if_v / carry 経路。
				 * handler-phase 枠を脱出する br_if は caught_pop trampoline が要るため融合しない)。 */
				uint32_t np_bi = count_handler_exits(&C, (C.cctrl - 1 - (int)d) + (is_loop ? 1 : 0));
				if(!(tgt_arity == 0 && np_bi == 0 && try_fuse_cmp_branch(&C, cf, 0, is_loop, cf->loop_start)))
					emit_branch(&C, d, 1);
			}
			break;
		}
		case 0x0e: { /* br_table */
			uint32_t n = rdu(&C);
			/* WASM 順: n labels then default。bytecode: [n][default][l0..ln-1]。
			 * immediate (n labels + default) は dead code でも読み進めて reader を同期させる。 */
			uint32_t small[256];
			uint32_t* labels = (n <= 256) ? small : (uint32_t*)kinowasm_mem_malloc(sizeof(uint32_t) * (n ? n : 1));
			if(!labels)
				core_fatal("OOM br_table labels");

			for(uint32_t k = 0; k < n; k++)
				labels[k] = rdu(&C);

			uint32_t dft = rdu(&C);
			if(!C.unreachable) {
				/* index(top) を slot に確定 (r0 を空ける)。全 target は同一 arity (wasm 検証)。
				 * arity-1 なら値(index の下) を r0 に確定して target へ運搬 (br_if_v と同方式)。 */
				uint32_t idx_slot = to_slot(&C, C.cdepth - 1);
				C.cdepth--;
				C.reg_pos = -1;
				ctrl_t* dcf = &C.ctrl[C.cctrl - 1 - dft];
				int A = (dcf->kind == C_LOOP) ? dcf->in_arity : dcf->arity;
				/* arity-1 で home slot 受け target (loop / try / param 付き if) が混在するか
				 * (これらの受けは home slot 渡しで r0 運搬では届かない)。emit_branch の home_tgt と同一規約。 */
				#define IS_HOME_TGT(cfp) ((cfp)->kind == C_LOOP || (cfp)->kind == C_TRY || ((cfp)->kind == C_IF && (cfp)->in_arity > 0))
				/* target ごとの handler 脱出 pop 数 (emit_branch の npop と同一規約)。 */
				#define BRT_NPOP(depth) count_handler_exits(&C, (C.cctrl - 1 - (int)(depth)) + ((C.ctrl[C.cctrl - 1 - (depth)].kind == C_LOOP) ? 1 : 0))
				int loop_tgt = 0;
				if(A == 1) {
					if(IS_HOME_TGT(dcf))
						loop_tgt = 1;

					for(uint32_t k = 0; k < n && !loop_tgt; k++) {
						if(IS_HOME_TGT(&C.ctrl[C.cctrl - 1 - labels[k]]))
							loop_tgt = 1;
					}
				}
				int any_pop = BRT_NPOP(dft) > 0;
				for(uint32_t k = 0; k < n && !any_pop; k++) {
					if(BRT_NPOP(labels[k]) > 0)
						any_pop = 1;
				}
				if(A <= 1 && !loop_tgt && !any_pop) {
					/* arity-0/1 (block/if target のみ): 値(index の下)を r0 に確定して運搬 (br_if_v と同方式)。 */
					if(A == 1)
						to_reg_top(&C);

					cb_emit_op(&C.cb, core_br_table);
					cb_emit_u32(&C.cb, idx_slot);
					cb_emit_u32(&C.cb, n);
					/* 分岐先 word を出す helper: loop なら index 仮置き+rec_loop、それ以外 fixup。 */
					#define EMIT_BR_TGT(depth) do { \
						ctrl_t* tcfp = &C.ctrl[C.cctrl - 1 - (depth)]; \
						if(tcfp->kind == C_LOOP) { \
							uint32_t twi = cb_emit_u32(&C.cb, tcfp->loop_start); \
							rec_loop(&C, twi); \
						} else { \
							uint32_t twi = cb_emit_tgt(&C.cb); \
							ctrl_add_fixup(tcfp, twi); \
						} \
					} while(0)
					EMIT_BR_TGT(dft);
					for(uint32_t k = 0; k < n; k++)
						EMIT_BR_TGT(labels[k]);

					#undef EMIT_BR_TGT
				} else if(A <= 1) {
					/* arity-0/1 + (home slot 受け target 混在 or handler 脱出 pop あり): target 種別ごとの
					 * trampoline で pop + 運搬する (A==1: home 受け target=copyslot、block=getslot で r0 へ。
					 * A==0 は pop のみ)。 */
					uint32_t src = 0;
					if(A == 1) {
						to_home_slot(&C, C.cdepth - 1);
						src = home_slot(&C, C.cdepth - 1);
					}
					cb_emit_op(&C.cb, core_br_table);
					cb_emit_u32(&C.cb, idx_slot);
					cb_emit_u32(&C.cb, n);
					uint32_t disp = C.cb.len; /* dispatch word placeholder (default + n labels) */
					for(uint32_t k = 0; k <= n; k++)
						cb_emit_u32(&C.cb, 0);

					#define EMIT_TRAMP1(depth, dword) do { \
						ctrl_t* tcfp = &C.ctrl[C.cctrl - 1 - (depth)]; \
						C.cb.code[(dword)].u32 = C.cb.len; \
						rec_loop(&C, (dword)); \
						uint32_t tnp = BRT_NPOP(depth); \
						for(uint32_t tp = 0; tp < tnp; tp++) \
							cb_emit_op(&C.cb, core_caught_pop); \
						if(A == 1) { \
							if(IS_HOME_TGT(tcfp)) { \
								uint32_t tdst = home_slot(&C, tcfp->sp_base); \
								if(tdst != src) { \
									cb_emit_op(&C.cb, core_copyslot); \
									cb_emit_u32(&C.cb, tdst); \
									cb_emit_u32(&C.cb, src); \
								} \
							} else { \
								cb_emit_op(&C.cb, core_getslot); \
								cb_emit_u32(&C.cb, src); \
							} \
						} \
						cb_emit_op(&C.cb, core_br); \
						if(tcfp->kind == C_LOOP) { \
							uint32_t twi = cb_emit_u32(&C.cb, tcfp->loop_start); \
							rec_loop(&C, twi); \
						} else { \
							uint32_t twi = cb_emit_tgt(&C.cb); \
							ctrl_add_fixup(tcfp, twi); \
						} \
					} while(0)
					EMIT_TRAMP1(dft, disp);
					for(uint32_t k = 0; k < n; k++)
						EMIT_TRAMP1(labels[k], disp + 1 + k);

					#undef EMIT_TRAMP1
				} else {
					/* 多値 (A>=2): A 個の結果を source home slot に確定し、各 target ごとの trampoline
					 * (copyslot で target の home slot へ運搬 → br) へ分岐。target ごとに sp_base が
					 * 異なるため一括運搬できず trampoline が要る。 */
					for(int k = 0; k < A; k++)
						to_home_slot(&C, C.cdepth - A + k);

					uint32_t src_base = home_slot(&C, C.cdepth - A);
					cb_emit_op(&C.cb, core_br_table);
					cb_emit_u32(&C.cb, idx_slot);
					cb_emit_u32(&C.cb, n);
					uint32_t disp = C.cb.len; /* dispatch word placeholder (default + n labels) */
					for(uint32_t k = 0; k <= n; k++)
						cb_emit_u32(&C.cb, 0);

					/* trampoline: dispatch word を trampoline 先頭 index に設定し rec_loop 登録。 */
					#define EMIT_TRAMP(depth, dword) do { \
						ctrl_t* tcfp = &C.ctrl[C.cctrl - 1 - (depth)]; \
						C.cb.code[(dword)].u32 = C.cb.len; \
						rec_loop(&C, (dword)); \
						uint32_t tnp = BRT_NPOP(depth); \
						for(uint32_t tp = 0; tp < tnp; tp++) \
							cb_emit_op(&C.cb, core_caught_pop); \
						for(int tk = 0; tk < A; tk++) { \
							uint32_t tdst = home_slot(&C, tcfp->sp_base + tk); \
							if(tdst != src_base + tk) { \
								cb_emit_op(&C.cb, core_copyslot); \
								cb_emit_u32(&C.cb, tdst); \
								cb_emit_u32(&C.cb, src_base + tk); \
							} \
						} \
						cb_emit_op(&C.cb, core_br); \
						if(tcfp->kind == C_LOOP) { \
							uint32_t twi = cb_emit_u32(&C.cb, tcfp->loop_start); \
							rec_loop(&C, twi); \
						} else { \
							uint32_t twi = cb_emit_tgt(&C.cb); \
							ctrl_add_fixup(tcfp, twi); \
						} \
					} while(0)
					EMIT_TRAMP(dft, disp);
					for(uint32_t k = 0; k < n; k++)
						EMIT_TRAMP(labels[k], disp + 1 + k);

					#undef EMIT_TRAMP
				}
				#undef BRT_NPOP
				#undef IS_HOME_TGT
			}
			if(labels != small)
				kinowasm_mem_free(labels);

			C.unreachable = 1;
			break;
		}
		case 0x0f: /* return */
			if(!C.unreachable) {
				int R = (int)ft->num_results;
				/* handler 内からの return: 脱出する全 handler 分の caught_pop を先に emit
				 * (関数を抜けても g_caught は invoke 単位のグローバル台帳のため、caller の
				 * rethrow が残骸を跨いで誤読しないよう釣り合わせる)。 */
				uint32_t np = count_handler_exits(&C, 0);
				for(uint32_t k = 0; k < np; k++)
					cb_emit_op(&C.cb, core_caught_pop);

				if(R >= 2) {
					/* top R を home slot へ強制実体化し ret_multi で書き出す (local 別名も home へ)。 */
					for(int k = 0; k < R; k++)
						to_home_slot(&C, C.cdepth - R + k);

					cb_emit_op(&C.cb, core_ret_multi);
					cb_emit_u32(&C.cb, (uint32_t)R);
					cb_emit_u32(&C.cb, home_slot(&C, C.cdepth - R));
				} else {
					if(R == 1)
						to_reg_top(&C);

					cb_emit_op(&C.cb, core_ret);
				}
				C.unreachable = 1;
			}
			break;
		case 0x10: { /* call */
			uint32_t fi = rdu(&C);
			if(!C.unreachable) {
				corefunctype_t* cft = core_func_type(m, fi);
				uint32_t na = cft->num_params;   /* 引数を連続 home slot に確定 */
				int nr = (int)cft->num_results;
				/* call は r0 を破壊するので、引数の下に居る live reg(L_REG=r0) を先に spill。 */
				if(C.reg_pos >= 0 && C.reg_pos < (int)(C.cdepth - na))
					spill_reg(&C);

				for(int i = (int)(C.cdepth - na); i < C.cdepth; i++) {
					if(i >= 0)
						to_home_slot(&C, i);
				}
				uint32_t arg_base = (C.cdepth >= (int)na) ? home_slot(&C, C.cdepth - na) : C.fb;
				int it = innermost_try(&C);
				if(it >= 0) {
					cb_emit_op(&C.cb, core_call_eh);
					cb_emit_u32(&C.cb, fi);
					cb_emit_u32(&C.cb, arg_base);
					cb_emit_u32(&C.cb, C.max_slot);
					cb_emit_u32(&C.cb, na);
					uint32_t wi = cb_emit_tgt(&C.cb);
					ctrl_add_disp(&C.ctrl[it], wi);
				} else {
					cb_emit_op(&C.cb, core_call);
					cb_emit_u32(&C.cb, fi);
					cb_emit_u32(&C.cb, arg_base);
					cb_emit_u32(&C.cb, C.max_slot);
					cb_emit_u32(&C.cb, na);
				}
				C.cdepth -= na;
				C.reg_pos = -1;
				if(nr == 1) {
					loc_t res = { L_REG, 0, 0, 0 };
					push_loc(&C, res);
					C.reg_pos = C.cdepth - 1;
				} else if(nr >= 2) {
					/* 多値: mret バッファを結果 slot [cdepth..) へ取り込み L_SLOT で push。 */
					cb_emit_op(&C.cb, core_mret_get);
					cb_emit_u32(&C.cb, (uint32_t)nr);
					cb_emit_u32(&C.cb, home_slot(&C, C.cdepth));
					for(int k = 0; k < nr; k++) {
						loc_t res = { L_SLOT, 0, (int32_t)home_slot(&C, C.cdepth), 0 };
						push_loc(&C, res);
					}
				}
			}
			break;
		}
		case 0x11: { /* call_indirect */
			uint32_t ti = rdu(&C);
			uint32_t tblidx = rdu(&C); /* table idx */
			if(!C.unreachable) {
				corefunctype_t* cft = &m->types[ti];
				uint32_t na = cft->num_params;
				int nr = (int)cft->num_results;
				to_reg_top(&C);
				C.cdepth--;
				C.reg_pos = -1; /* table index pop → r0 */
				/* table64: idx (i64) の上位 32bit を前置 op で検査し、call_indirect 本体は u32 経路のまま。 */
				if(g_rt->tables && tblidx < g_rt->num_tables && g_rt->tables[tblidx].is_64)
					cb_emit_op(&C.cb, core_idx64_chk);

				for(int i = (int)(C.cdepth - na); i < C.cdepth; i++) {
					if(i >= 0)
						to_home_slot(&C, i);
				}
				uint32_t arg_base = (C.cdepth >= (int)na) ? home_slot(&C, C.cdepth - na) : C.fb;
				int it = innermost_try(&C);
				if(it >= 0) {
					cb_emit_op(&C.cb, core_call_indirect_eh);
					cb_emit_u32(&C.cb, ti);
					cb_emit_u32(&C.cb, arg_base);
					cb_emit_u32(&C.cb, C.max_slot);
					cb_emit_u32(&C.cb, na);
					cb_emit_u32(&C.cb, tblidx);
					uint32_t wi = cb_emit_tgt(&C.cb);
					ctrl_add_disp(&C.ctrl[it], wi);
				} else {
					cb_emit_op(&C.cb, core_call_indirect);
					cb_emit_u32(&C.cb, ti);
					cb_emit_u32(&C.cb, arg_base);
					cb_emit_u32(&C.cb, C.max_slot);
					cb_emit_u32(&C.cb, na);
					cb_emit_u32(&C.cb, tblidx);
				}
				C.cdepth -= na;
				if(nr == 1) {
					loc_t res = { L_REG, 0, 0, 0 };
					push_loc(&C, res);
					C.reg_pos = C.cdepth - 1;
				} else if(nr >= 2) {
					/* 多値: mret バッファを結果 slot [cdepth..) へ取り込み L_SLOT で push。 */
					cb_emit_op(&C.cb, core_mret_get);
					cb_emit_u32(&C.cb, (uint32_t)nr);
					cb_emit_u32(&C.cb, home_slot(&C, C.cdepth));
					for(int k = 0; k < nr; k++) {
						loc_t res = { L_SLOT, 0, (int32_t)home_slot(&C, C.cdepth), 0 };
						push_loc(&C, res);
					}
				}
			}
			break;
		}
		/* return_call (tail call): call して結果(r0/g_core_mret)をそのまま現関数の結果に。 */
		case 0x12: {
			uint32_t fi = rdu(&C);
			if(!C.unreachable) {
				corefunctype_t* cft = core_func_type(m, fi);
				uint32_t na = cft->num_params;
				/* handler 内からの tail call = 関数脱出: return と同様に全 handler 分を pop。 */
				uint32_t np = count_handler_exits(&C, 0);
				for(uint32_t k = 0; k < np; k++)
					cb_emit_op(&C.cb, core_caught_pop);

				if(C.reg_pos >= 0 && C.reg_pos < (int)(C.cdepth - na))
					spill_reg(&C);

				for(int i = (int)(C.cdepth - na); i < C.cdepth; i++) {
					if(i >= 0)
						to_home_slot(&C, i);
				}
				uint32_t arg_base = (C.cdepth >= (int)na) ? home_slot(&C, C.cdepth - na) : C.fb;
				/* 単一 tail-call op: callee へ frame 再利用で musttail (深い tail 再帰でも C スタック据置)。
				 * 多値も callee の ret_multi が g_core_mret に書くので、現関数の結果としてそのまま返る。 */
				cb_emit_op(&C.cb, core_return_call);
				cb_emit_u32(&C.cb, fi);
				cb_emit_u32(&C.cb, arg_base);
				cb_emit_u32(&C.cb, C.max_slot);
				cb_emit_u32(&C.cb, na);
				C.cdepth -= na;
				C.reg_pos = -1;
				C.unreachable = 1;
			}
			break;
		}
		case 0x13: { /* return_call_indirect (tail call) */
			uint32_t ti = rdu(&C);
			uint32_t tblidx = rdu(&C); /* tableidx */
			if(!C.unreachable) {
				corefunctype_t* cft = &m->types[ti];
				uint32_t na = cft->num_params;
				uint32_t np = count_handler_exits(&C, 0);   /* return と同様に全 handler 分を pop */
				for(uint32_t k = 0; k < np; k++)
					cb_emit_op(&C.cb, core_caught_pop);

				to_reg_top(&C);
				C.cdepth--;
				C.reg_pos = -1; /* table index → r0 */
				if(g_rt->tables && tblidx < g_rt->num_tables && g_rt->tables[tblidx].is_64)
					cb_emit_op(&C.cb, core_idx64_chk);

				for(int i = (int)(C.cdepth - na); i < C.cdepth; i++) {
					if(i >= 0)
						to_home_slot(&C, i);
				}
				uint32_t arg_base = (C.cdepth >= (int)na) ? home_slot(&C, C.cdepth - na) : C.fb;
				cb_emit_op(&C.cb, core_return_call_indirect);
				cb_emit_u32(&C.cb, ti);
				cb_emit_u32(&C.cb, arg_base);
				cb_emit_u32(&C.cb, C.max_slot);
				cb_emit_u32(&C.cb, na);
				cb_emit_u32(&C.cb, tblidx);
				C.cdepth -= na;
				C.reg_pos = -1;
				C.unreachable = 1;
			}
			break;
		}
		case 0x1a: /* drop */
			if(!C.unreachable) {
				if(C.reg_pos == C.cdepth - 1)
					C.reg_pos = -1;

				C.cdepth--;
			}
			break;
		case 0x1b: /* select */
			if(!C.unreachable) {
				to_reg_top(&C); /* cond=r0 */
				C.cdepth--;
				C.reg_pos = -1;
				int bpos = C.cdepth - 1;
				int apos = C.cdepth - 2;
				uint32_t bs = to_slot(&C, bpos);
				uint32_t as = to_slot(&C, apos);
				cb_emit_op(&C.cb, core_select);
				cb_emit_u32(&C.cb, as);
				cb_emit_u32(&C.cb, bs);
				C.cdepth -= 2;
				loc_t res = { L_REG, 0, 0, 0 };
				push_loc(&C, res);
				C.reg_pos = C.cdepth - 1;
			}
			break;
		case 0x1c: /* select_t (typed select): 型 vec を読み捨て select と同一動作。 */
			{
				uint32_t tn = rdu(&C);
				for(uint32_t k = 0; k < tn; k++)
					rdb(&C);
			}
			if(!C.unreachable) {
				to_reg_top(&C);
				C.cdepth--;
				C.reg_pos = -1;
				int bpos = C.cdepth - 1;
				int apos = C.cdepth - 2;
				uint32_t bs = to_slot(&C, bpos);
				uint32_t as = to_slot(&C, apos);
				cb_emit_op(&C.cb, core_select);
				cb_emit_u32(&C.cb, as);
				cb_emit_u32(&C.cb, bs);
				C.cdepth -= 2;
				loc_t res = { L_REG, 0, 0, 0 };
				push_loc(&C, res);
				C.reg_pos = C.cdepth - 1;
			}
			break;
		/* reference types: funcref/externref は int32 (funcidx、-1=null)。 */
		case 0xd0: { /* ref.null → -1 */
			rds64(&C); /* reftype/heaptype (SLEB) */
			if(!C.unreachable) {
				loc_t l = { L_CONST, 0, 0, -1 };
				push_loc(&C, l);
			}
			break;
		}
		case 0xd1: /* ref.is_null */
			emit_unop(&C, core_ref_is_null, 0);
			break;
		case 0xd2: { /* ref.func → funcidx */
			uint32_t fi = rdu(&C);
			if(!C.unreachable) {
				loc_t l = { L_CONST, 0, 0, (int32_t)fi };
				push_loc(&C, l);
			}
			break;
		}
		/* table get/set (単一 table 前提、tableidx は読み捨て)。 */
		case 0x25: { /* table.get: idx(r0)→ref(r0) */
			uint32_t ti = rdu(&C);
			if(!C.unreachable) {
				to_reg_top(&C);
				cb_emit_op(&C.cb, core_table_get);
				cb_emit_u32(&C.cb, ti);
				C.st[C.cdepth - 1].is64 = 0;
			}
			break;
		}
		case 0x26: { /* table.set: [idx,val] */
			uint32_t ti = rdu(&C);
			if(!C.unreachable) {
				to_reg_top(&C); /* val=r0 */
				int ip = C.cdepth - 2;
				uint32_t is = to_slot(&C, ip);
				cb_emit_op(&C.cb, core_table_set);
				cb_emit_u32(&C.cb, is);
				cb_emit_u32(&C.cb, ti);
				C.cdepth -= 2;
				C.reg_pos = -1;
			}
			break;
		}
		case 0x20: { /* local.get: lazy alias */
			uint32_t x = rdu(&C);
			if(!C.unreachable) {
				loc_t l = { L_SLOT, 0, (int32_t)x, 0 };
				push_loc(&C, l);
			}
			break;
		}
		case 0x21: {
			uint32_t x = rdu(&C);
			if(!C.unreachable) {
				int top = C.cdepth - 1;
				/* local.set 融合 (deeper reg を温存=不要 spill 回避):
				 *   ① binop 直前 → 結果を slot x へ直接 (st)。
				 *   ①' load 直前 → 結果を slot x へ直接 (load st)。
				 *   ② TOS=const → const を slot x へ直接 (const_slot)。
				 *   ③ TOS=local 別名 → copyslot(x, y)。
				 *   ④ それ以外 (L_REG) → 従来の setslot。 */
				if(try_fuse_binop_setslot(&C, x)) {
					/* ① 融合済み: 追加 emit 不要 */
				} else if(try_fuse_load_setslot(&C, x)) {
					/* ①' 融合済み: 追加 emit 不要 */
				} else if(C.st[top].kind == L_CONST) {
					loc_t cc = C.st[top];
					invalidate_local(&C, x);
					cb_emit_op(&C.cb, cc.is64 ? core_const_slot_i64 : core_const_slot_i32);
					cb_emit_u32(&C.cb, x);
					if(cc.is64)
						cb_emit_i64(&C.cb, cc.cval);
					else
						cb_emit_i32(&C.cb, (int32_t)cc.cval);

					C.cdepth--;
				} else if(C.st[top].kind == L_SLOT) {
					invalidate_local(&C, x);
					uint32_t ys = (uint32_t)C.st[C.cdepth - 1].slot;
					if(ys != x) {
						cb_emit_op(&C.cb, core_copyslot);
						cb_emit_u32(&C.cb, x);
						cb_emit_u32(&C.cb, ys);
					}
					C.cdepth--;
				} else {
					to_reg_top(&C);
					invalidate_local(&C, x);
					cb_emit_op(&C.cb, core_setslot);
					cb_emit_u32(&C.cb, x);
					C.cdepth--;
					C.reg_pos = -1;
				}
			}
			break;
		}
		case 0x22: { /* local.tee */
			uint32_t x = rdu(&C);
			if(!C.unreachable) {
				/* local.tee 融合: 直前 binop/load の結果を slot x へ直接書き r0 にも残す (setslot 消去)。
				 * 融合不可は従来どおり to_reg_top + setslot。 */
				if(!try_fuse_binop_tee(&C, x) && !try_fuse_load_tee(&C, x)) {
					to_reg_top(&C);
					invalidate_local(&C, x);
					cb_emit_op(&C.cb, core_setslot);
					cb_emit_u32(&C.cb, x);
				}
			}
			break;
		}
		case 0x23: {
			uint32_t x = rdu(&C);
			if(!C.unreachable) {
				if(C.reg_pos >= 0)
					spill_reg(&C);

				cb_emit_op(&C.cb, core_global_get);
				cb_emit_u32(&C.cb, x);
				loc_t l = { L_REG, 0, 0, 0 };
				push_loc(&C, l);
				C.reg_pos = C.cdepth - 1;
			}
			break;
		}
		case 0x24: {
			uint32_t x = rdu(&C);
			if(!C.unreachable) {
				to_reg_top(&C);
				cb_emit_op(&C.cb, core_global_set);
				cb_emit_u32(&C.cb, x);
				C.cdepth--;
				C.reg_pos = -1;
			}
			break;
		}
		/* loads: [align][offset]。addr=top(r0)。 */
		/* memory64 (mem_is64) は memN 経路 (u64 offset + is_64 アドレス対応)。memidx=0 の memory32 で
		 * offset が u32 に収まるときのみ高速路 (hot path は不変)。 */
		/* STFN != NULL の load は結果 slot 直書き版 (i32_load_st 等) を持ち、後続 local.set/tee と
		 * 融合できる。高速路 (memory32・offset≤u32) のときだけ lst 状態を記録する。 */
		#define LOADC(FN, IS64, STFN) do { \
			uint32_t midx; \
			uint64_t off = read_memarg_off(&C, &midx); \
			if(!C.unreachable) { \
				to_reg_top(&C); \
				if(midx == 0 && !mem_is64(0) && off <= 0xFFFFFFFFull) { \
					uint32_t _lwi = cb_emit_op(&C.cb, FN); \
					cb_emit_u32(&C.cb, (uint32_t)off); \
					if((STFN) != NULL) { \
						C.lst_widx = (int)_lwi; \
						C.lst_st_fn = (STFN); \
						C.lst_pos = C.cdepth - 1; \
						C.lst_cb_end = C.cb.len; \
					} else { \
						C.lst_widx = -1; \
					} \
				} else { \
					cb_emit_op(&C.cb, core_load_memN); \
					cb_emit_u32(&C.cb, midx); \
					cb_emit_i64(&C.cb, (int64_t)off); \
					cb_emit_u32(&C.cb, op); \
					C.lst_widx = -1; \
				} \
				C.st[C.cdepth - 1].is64 = IS64; \
			} \
		} while(0)
		case 0x28: LOADC(core_i32_load,0,core_i32_load_st); break;
		case 0x2a: LOADC(core_i32_load,0,core_i32_load_st); break; /* f32.load = i32.load (同一 4byte move) */
		case 0x2c: LOADC(core_i32_load8_s,0,NULL); break;
		case 0x2d: LOADC(core_i32_load8_u,0,core_i32_load8_u_st); break;
		case 0x2e: LOADC(core_i32_load16_s,0,core_i32_load16_s_st); break;
		case 0x2f: LOADC(core_i32_load16_u,0,core_i32_load16_u_st); break;
		case 0x29: LOADC(core_i64_load,1,NULL); break;
		case 0x2b: LOADC(core_f64_load,1,NULL); break;
		case 0x30: LOADC(core_i64_load8_s,1,NULL); break;
		case 0x31: LOADC(core_i64_load8_u,1,NULL); break;
		case 0x32: LOADC(core_i64_load16_s,1,NULL); break;
		case 0x33: LOADC(core_i64_load16_u,1,NULL); break;
		case 0x34: LOADC(core_i64_load32_s,1,NULL); break;
		case 0x35: LOADC(core_i64_load32_u,1,NULL); break;
		/* stores: [align][offset]。value=top(r0)、addr=2nd(slot)。 */
		#define STOREC(FN) do { \
			uint32_t midx; \
			uint64_t off = read_memarg_off(&C, &midx); \
			if(!C.unreachable) { \
				to_reg_top(&C); \
				int ap = C.cdepth - 2; \
				uint32_t as = to_slot(&C, ap); \
				if(midx == 0 && !mem_is64(0) && off <= 0xFFFFFFFFull) { \
					cb_emit_op(&C.cb, FN); \
					cb_emit_u32(&C.cb, as); \
					cb_emit_u32(&C.cb, (uint32_t)off); \
				} else { \
					cb_emit_op(&C.cb, core_store_memN); \
					cb_emit_u32(&C.cb, midx); \
					cb_emit_u32(&C.cb, as); \
					cb_emit_i64(&C.cb, (int64_t)off); \
					cb_emit_u32(&C.cb, op); \
				} \
				C.cdepth -= 2; \
				C.reg_pos = -1; \
			} \
		} while(0)
		case 0x36: STOREC(core_i32_store); break;
		case 0x38: STOREC(core_i32_store); break; /* f32.store = i32.store (同一 4byte move) */
		case 0x3a: STOREC(core_i32_store8); break;
		case 0x3b: STOREC(core_i32_store16); break;
		case 0x37: STOREC(core_i64_store); break;
		case 0x39: STOREC(core_f64_store); break;
		case 0x3c: STOREC(core_i64_store8); break;
		case 0x3d: STOREC(core_i64_store16); break;
		case 0x3e: STOREC(core_i64_store32); break;
		case 0x41: {
			int32_t v = rds(&C);
			if(!C.unreachable) {
				loc_t l = { L_CONST, 0, 0, v };
				push_loc(&C, l);
			}
			break;
		}
		case 0x42: {
			int64_t v = rds64(&C);
			if(!C.unreachable) {
				loc_t l = { L_CONST, 1, 0, v };
				push_loc(&C, l);
			}
			break;
		}
		case 0x43: {
			uint32_t raw = 0;
			for(int i = 0; i < 4; i++)
				raw |= (uint32_t)rdb(&C) << (8 * i);

			if(!C.unreachable) {
				loc_t l = { L_CONST, 0, 0, (int32_t)raw };
				push_loc(&C, l);
			}
			break;
		}
		case 0x44: {
			uint64_t raw = rdu64raw(&C);
			if(!C.unreachable) {
				loc_t l = { L_CONST, 1, 0, (int64_t)raw };
				push_loc(&C, l);
			}
			break;
		}
		/* 単項 */
		case 0x45: emit_unop(&C,core_i32_eqz,0); break;
		case 0x67: emit_unop(&C,core_i32_clz,0); break;
		case 0x68: emit_unop(&C,core_i32_ctz,0); break;
		case 0x69: emit_unop(&C,core_i32_popcnt,0); break;
		case 0xa7: emit_unop(&C,core_i32_wrap_i64,0); break;
		case 0xac: emit_unop(&C,core_i64_extend_i32_s,1); break;
		case 0xad: emit_unop(&C,core_i64_extend_i32_u,1); break;
		case 0xb7: emit_unop(&C,core_f64_convert_i32_s,1); break;
		case 0xaa: emit_unop(&C,core_i32_trunc_f64_s,0); break;
		case 0xab: emit_unop(&C,core_i32_trunc_f64_u,0); break;
		/* float 単項補完 (f32 全部 + f64 ceil/floor/trunc/nearest/sqrt) */
		case 0x8b: emit_unop(&C,core_f32_abs,0); break;
		case 0x8c: emit_unop(&C,core_f32_neg,0); break;
		case 0x8d: emit_unop(&C,core_f32_ceil,0); break;
		case 0x8e: emit_unop(&C,core_f32_floor,0); break;
		case 0x8f: emit_unop(&C,core_f32_trunc,0); break;
		case 0x90: emit_unop(&C,core_f32_nearest,0); break;
		case 0x91: emit_unop(&C,core_f32_sqrt,0); break;
		case 0x9b: emit_unop(&C,core_f64_ceil,1); break;
		case 0x9c: emit_unop(&C,core_f64_floor,1); break;
		case 0x9d: emit_unop(&C,core_f64_trunc,1); break;
		case 0x9e: emit_unop(&C,core_f64_nearest,1); break;
		case 0x9f: emit_unop(&C,core_f64_sqrt,1); break;
		/* 変換補完 (trunc f32→int / i64 trunc f64 / f32 convert / demote / promote) */
		case 0xa8: emit_unop(&C,core_i32_trunc_f32_s,0); break;
		case 0xa9: emit_unop(&C,core_i32_trunc_f32_u,0); break;
		case 0xae: emit_unop(&C,core_i64_trunc_f32_s,1); break;
		case 0xaf: emit_unop(&C,core_i64_trunc_f32_u,1); break;
		case 0xb0: emit_unop(&C,core_i64_trunc_f64_s,1); break;
		case 0xb1: emit_unop(&C,core_i64_trunc_f64_u,1); break;
		case 0xb2: emit_unop(&C,core_f32_convert_i32_s,0); break;
		case 0xb3: emit_unop(&C,core_f32_convert_i32_u,0); break;
		case 0xb4: emit_unop(&C,core_f32_convert_i64_s,0); break;
		case 0xb5: emit_unop(&C,core_f32_convert_i64_u,0); break;
		case 0xb6: emit_unop(&C,core_f32_demote_f64,0); break;
		case 0xbb: emit_unop(&C,core_f64_promote_f32,1); break;
		/* i32 二項 */
		case 0x6a: emit_binop(&C,&core_i32_add); break;
		case 0x6b: emit_binop(&C,&core_i32_sub); break;
		case 0x6c: emit_binop(&C,&core_i32_mul); break;
		case 0x6d: emit_binop(&C,&core_i32_div_s_tab); break;  /* div/rem: 専用 */
		case 0x6e: emit_binop(&C,&core_i32_div_u_tab); break;
		case 0x6f: emit_binop(&C,&core_i32_rem_s_tab); break;
		case 0x70: emit_binop(&C,&core_i32_rem_u_tab); break;
		case 0x71: emit_binop(&C,&core_i32_and); break;
		case 0x72: emit_binop(&C,&core_i32_or); break;
		case 0x73: emit_binop(&C,&core_i32_xor); break;
		case 0x74: emit_binop(&C,&core_i32_shl); break;
		case 0x75: emit_binop(&C,&core_i32_shr_s); break;
		case 0x76: emit_binop(&C,&core_i32_shr_u); break;
		case 0x77: emit_binop(&C,&core_i32_rotl); break;
		case 0x78: emit_binop(&C,&core_i32_rotr); break;
		case 0x46: emit_cmp(&C,&core_i32_eq,  0); break;
		case 0x47: emit_cmp(&C,&core_i32_ne,  1); break;
		case 0x48: emit_cmp(&C,&core_i32_lt_s,2); break;
		case 0x49: emit_cmp(&C,&core_i32_lt_u,6); break;
		case 0x4a: emit_cmp(&C,&core_i32_gt_s,4); break;
		case 0x4b: emit_cmp(&C,&core_i32_gt_u,8); break;
		case 0x4c: emit_cmp(&C,&core_i32_le_s,3); break;
		case 0x4d: emit_cmp(&C,&core_i32_le_u,7); break;
		case 0x4e: emit_cmp(&C,&core_i32_ge_s,5); break;
		case 0x4f: emit_cmp(&C,&core_i32_ge_u,9); break;
		/* i64 比較 / eqz */
		case 0x50: emit_unop(&C,core_i64_eqz,0); break;
		case 0x79: emit_unop(&C,core_i64_clz,1); break;
		case 0x7a: emit_unop(&C,core_i64_ctz,1); break;
		case 0x7b: emit_unop(&C,core_i64_popcnt,1); break;
		case 0xc0: emit_unop(&C,core_i32_extend8_s,0); break;
		case 0xc1: emit_unop(&C,core_i32_extend16_s,0); break;
		case 0xc2: emit_unop(&C,core_i64_extend8_s,1); break;
		case 0xc3: emit_unop(&C,core_i64_extend16_s,1); break;
		case 0xc4: emit_unop(&C,core_i64_extend32_s,1); break;
		case 0x51: emit_binop(&C,&core_i64_eq); break;
		case 0x52: emit_binop(&C,&core_i64_ne); break;
		case 0x53: emit_binop(&C,&core_i64_lt_s); break;
		case 0x54: emit_binop(&C,&core_i64_lt_u); break;
		case 0x55: emit_binop(&C,&core_i64_gt_s); break;
		case 0x56: emit_binop(&C,&core_i64_gt_u); break;
		case 0x57: emit_binop(&C,&core_i64_le_s); break;
		case 0x58: emit_binop(&C,&core_i64_le_u); break;
		case 0x59: emit_binop(&C,&core_i64_ge_s); break;
		case 0x5a: emit_binop(&C,&core_i64_ge_u); break;
		case 0x7f: emit_binop(&C,&core_i64_div_s_tab); break;
		case 0x80: emit_binop(&C,&core_i64_div_u_tab); break;
		case 0x81: emit_binop(&C,&core_i64_rem_s_tab); break;
		case 0x82: emit_binop(&C,&core_i64_rem_u_tab); break;
		/* i64 二項 (最小) */
		case 0x7c: emit_binop(&C,&core_i64_add); break;
		case 0x7d: emit_binop(&C,&core_i64_sub); break;
		case 0x7e: emit_binop(&C,&core_i64_mul); break;
		case 0x83: emit_binop(&C,&core_i64_and); break;
		case 0x84: emit_binop(&C,&core_i64_or); break;
		case 0x85: emit_binop(&C,&core_i64_xor); break;
		case 0x86: emit_binop(&C,&core_i64_shl); break;
		case 0x87: emit_binop(&C,&core_i64_shr_s); break;
		case 0x88: emit_binop(&C,&core_i64_shr_u); break;
		case 0x89: emit_binop(&C,&core_i64_rotl); break;
		case 0x8a: emit_binop(&C,&core_i64_rotr); break;
		/* f64 二項 (最小) */
		case 0xa0: emit_binop(&C,&core_f64_add); break;
		case 0xa1: emit_binop(&C,&core_f64_sub); break;
		case 0xa2: emit_binop(&C,&core_f64_mul); break;
		case 0xa3: emit_binop(&C,&core_f64_div); break;
		case 0x61: emit_binop(&C,&core_f64_eq); break;
		case 0x62: emit_binop(&C,&core_f64_ne); break;
		case 0x63: emit_binop(&C,&core_f64_lt); break;
		case 0x64: emit_binop(&C,&core_f64_gt); break;
		case 0x65: emit_binop(&C,&core_f64_le); break;
		case 0x66: emit_binop(&C,&core_f64_ge); break;
		/* f64 二項補完 (min/max/copysign) */
		case 0xa4: emit_binop(&C,&core_f64_min); break;
		case 0xa5: emit_binop(&C,&core_f64_max); break;
		case 0xa6: emit_binop(&C,&core_f64_copysign); break;
		/* f32 二項 (add/sub/mul/div/min/max/copysign) + 比較 */
		case 0x92: emit_binop(&C,&core_f32_add); break;
		case 0x93: emit_binop(&C,&core_f32_sub); break;
		case 0x94: emit_binop(&C,&core_f32_mul); break;
		case 0x95: emit_binop(&C,&core_f32_div); break;
		case 0x96: emit_binop(&C,&core_f32_min); break;
		case 0x97: emit_binop(&C,&core_f32_max); break;
		case 0x98: emit_binop(&C,&core_f32_copysign); break;
		case 0x5b: emit_binop(&C,&core_f32_eq); break;
		case 0x5c: emit_binop(&C,&core_f32_ne); break;
		case 0x5d: emit_binop(&C,&core_f32_lt); break;
		case 0x5e: emit_binop(&C,&core_f32_gt); break;
		case 0x5f: emit_binop(&C,&core_f32_le); break;
		case 0x60: emit_binop(&C,&core_f32_ge); break;
		case 0x99: emit_unop(&C,core_f64_abs,1); break;
		case 0x9a: emit_unop(&C,core_f64_neg,1); break;
		case 0xb8: emit_unop(&C,core_f64_convert_i32_u,1); break;
		case 0xb9: emit_unop(&C,core_f64_convert_i64_s,1); break;
		case 0xba: emit_unop(&C,core_f64_convert_i64_u,1); break;
		/* reinterpret は恒等 (r0 が bit 保持)。emit せず is64 のみ調整。 */
		case 0xbc: /* i32.reinterpret_f32 */
			if(!C.unreachable)
				C.st[C.cdepth - 1].is64 = 0;

			break;
		case 0xbd: /* i64.reinterpret_f64 */
			if(!C.unreachable)
				C.st[C.cdepth - 1].is64 = 1;

			break;
		case 0xbe: /* f32.reinterpret_i32 */
			if(!C.unreachable)
				C.st[C.cdepth - 1].is64 = 0;

			break;
		case 0xbf: /* f64.reinterpret_i64 */
			if(!C.unreachable)
				C.st[C.cdepth - 1].is64 = 1;

			break;
		/* memory.size / grow (memidx を operand に渡す) */
		case 0x3f: {
			uint32_t mi = rdu(&C);
			if(!C.unreachable) {
				if(C.reg_pos >= 0)
					spill_reg(&C);

				cb_emit_op(&C.cb, core_memory_size);
				cb_emit_u32(&C.cb, mi);
				loc_t l = { L_REG, 0, 0, 0 };
				push_loc(&C, l);
				C.reg_pos = C.cdepth - 1;
			}
			break;
		}
		case 0x40: {
			uint32_t mi = rdu(&C);
			if(!C.unreachable) {
				to_reg_top(&C);
				cb_emit_op(&C.cb, core_memory_grow);
				cb_emit_u32(&C.cb, mi);
			}
			break;
		}
		case 0xfc: {
			uint32_t sub = rdu(&C);
			switch(sub) {
			/* memory.init (0x08): [dataidx, memidx]。stack[dst,src,len]、len=top。 */
			case 0x08: {
				uint32_t di = rdu(&C);
				uint32_t mi = rdu(&C); /* memidx */
				if(!C.unreachable) {
					to_reg_top(&C);
					C.cdepth--;
					C.reg_pos = -1;
					int spos = C.cdepth - 1;
					int dpos = C.cdepth - 2;
					uint32_t ss = to_slot(&C, spos);
					uint32_t ds = to_slot(&C, dpos);
					cb_emit_op(&C.cb, core_memory_init);
					cb_emit_u32(&C.cb, di);
					cb_emit_u32(&C.cb, ds);
					cb_emit_u32(&C.cb, ss);
					cb_emit_u32(&C.cb, mi);
					C.cdepth -= 2;
					C.reg_pos = -1;
				}
				break;
			}
			/* data.drop (0x09): [dataidx]。 */
			case 0x09: {
				uint32_t di = rdu(&C);
				if(!C.unreachable) {
					cb_emit_op(&C.cb, core_data_drop);
					cb_emit_u32(&C.cb, di);
				}
				break;
			}
			/* table.init (0x0c): [elemidx, tableidx]。stack[dst,src,len]、len=top。 */
			case 0x0c: {
				uint32_t ei = rdu(&C);
				uint32_t tblidx = rdu(&C); /* tableidx */
				if(!C.unreachable) {
					to_reg_top(&C);
					C.cdepth--;
					C.reg_pos = -1;
					int spos = C.cdepth - 1;
					int dpos = C.cdepth - 2;
					uint32_t ss = to_slot(&C, spos);
					uint32_t ds = to_slot(&C, dpos);
					cb_emit_op(&C.cb, core_table_init);
					cb_emit_u32(&C.cb, ei);
					cb_emit_u32(&C.cb, ds);
					cb_emit_u32(&C.cb, ss);
					cb_emit_u32(&C.cb, tblidx);
					C.cdepth -= 2;
					C.reg_pos = -1;
				}
				break;
			}
			/* elem.drop (0x0d): [elemidx]。 */
			case 0x0d: {
				uint32_t ei = rdu(&C);
				if(!C.unreachable) {
					cb_emit_op(&C.cb, core_elem_drop);
					cb_emit_u32(&C.cb, ei);
				}
				break;
			}
			/* wide arithmetic (0x13 add128/0x14 sub128): 4 入力 i64 → 2 出力。in/out 同一 base。 */
			case 0x13:
			case 0x14: {
				coreop_t wop = (sub == 0x13) ? core_i64_add128 : core_i64_sub128;
				if(!C.unreachable) {
					for(int k = 0; k < 4; k++)
						to_home_slot(&C, C.cdepth - 4 + k);

					uint32_t base = home_slot(&C, C.cdepth - 4);
					cb_emit_op(&C.cb, wop);
					cb_emit_u32(&C.cb, base);
					C.cdepth -= 4;
					/* Wide handlers preserve r0; keep a live register below the inputs. */
					for(int k = 0; k < 2; k++) {
						loc_t r = { L_SLOT, 1, (int32_t)home_slot(&C, C.cdepth), 0 };
						push_loc(&C, r);
					}
				}
				break;
			}
			/* wide mul (0x15 mul_wide_s/0x16 _u): 2 入力 i64 → 2 出力。in/out 同一 base。 */
			case 0x15:
			case 0x16: {
				coreop_t wop = (sub == 0x15) ? core_i64_mul_wide_s : core_i64_mul_wide_u;
				if(!C.unreachable) {
					for(int k = 0; k < 2; k++)
						to_home_slot(&C, C.cdepth - 2 + k);

					uint32_t base = home_slot(&C, C.cdepth - 2);
					cb_emit_op(&C.cb, wop);
					cb_emit_u32(&C.cb, base);
					C.cdepth -= 2;
					/* Input materialization already clears a consumed register. */
					for(int k = 0; k < 2; k++) {
						loc_t r = { L_SLOT, 1, (int32_t)home_slot(&C, C.cdepth), 0 };
						push_loc(&C, r);
					}
				}
				break;
			}
			/* 飽和変換 (0x00-0x07) — 単項 */
			case 0x00: emit_unop(&C,core_i32_trunc_sat_f32_s,0); break;
			case 0x01: emit_unop(&C,core_i32_trunc_sat_f32_u,0); break;
			case 0x02: emit_unop(&C,core_i32_trunc_sat_f64_s,0); break;
			case 0x03: emit_unop(&C,core_i32_trunc_sat_f64_u,0); break;
			case 0x04: emit_unop(&C,core_i64_trunc_sat_f32_s,1); break;
			case 0x05: emit_unop(&C,core_i64_trunc_sat_f32_u,1); break;
			case 0x06: emit_unop(&C,core_i64_trunc_sat_f64_s,1); break;
			case 0x07: emit_unop(&C,core_i64_trunc_sat_f64_u,1); break;
			/* memory.copy (0x0a): stack[dst,src,len]、len=top。operands[dst_slot,src_slot]。 */
			case 0x0a: {
				uint32_t dmi = rdu(&C);
				uint32_t smi = rdu(&C); /* dst/src memidx */
				if(!C.unreachable) {
					to_reg_top(&C);
					C.cdepth--;
					C.reg_pos = -1;
					int spos = C.cdepth - 1;
					int dpos = C.cdepth - 2;
					uint32_t ss = to_slot(&C, spos);
					uint32_t ds = to_slot(&C, dpos);
					cb_emit_op(&C.cb, core_memory_copy);
					cb_emit_u32(&C.cb, ds);
					cb_emit_u32(&C.cb, ss);
					cb_emit_u32(&C.cb, dmi);
					cb_emit_u32(&C.cb, smi);
					C.cdepth -= 2;
					C.reg_pos = -1;
				}
				break;
			}
			/* memory.fill (0x0b): stack[dst,val,len]、len=top。operands[dst_slot,val_slot]。 */
			case 0x0b: {
				uint32_t mi = rdu(&C); /* memidx */
				if(!C.unreachable) {
					to_reg_top(&C);
					C.cdepth--;
					C.reg_pos = -1;
					int vpos = C.cdepth - 1;
					int dpos = C.cdepth - 2;
					uint32_t vs = to_slot(&C, vpos);
					uint32_t ds = to_slot(&C, dpos);
					cb_emit_op(&C.cb, core_memory_fill);
					cb_emit_u32(&C.cb, ds);
					cb_emit_u32(&C.cb, vs);
					cb_emit_u32(&C.cb, mi);
					C.cdepth -= 2;
					C.reg_pos = -1;
				}
				break;
			}
			/* table.copy (0x0e): stack[dst,src,len]。operands[dst_slot,src_slot,dst_tblidx,src_tblidx]。 */
			case 0x0e: {
				uint32_t dti = rdu(&C);
				uint32_t sti = rdu(&C); /* dst/src tableidx */
				if(!C.unreachable) {
					to_reg_top(&C);
					C.cdepth--;
					C.reg_pos = -1;
					int spos = C.cdepth - 1;
					int dpos = C.cdepth - 2;
					uint32_t ss = to_slot(&C, spos);
					uint32_t ds = to_slot(&C, dpos);
					cb_emit_op(&C.cb, core_table_copy);
					cb_emit_u32(&C.cb, ds);
					cb_emit_u32(&C.cb, ss);
					cb_emit_u32(&C.cb, dti);
					cb_emit_u32(&C.cb, sti);
					C.cdepth -= 2;
					C.reg_pos = -1;
				}
				break;
			}
			/* table.size (0x10): operands[tableidx] → r0=size。 */
			case 0x10: {
				uint32_t tblidx = rdu(&C);
				if(!C.unreachable) {
					if(C.reg_pos >= 0)
						spill_reg(&C);

					cb_emit_op(&C.cb, core_table_size);
					cb_emit_u32(&C.cb, tblidx);
					loc_t l = { L_REG, 0, 0, 0 };
					push_loc(&C, l);
					C.reg_pos = C.cdepth - 1;
				}
				break;
			}
			/* table.grow (0x0f): stack[init, delta]、delta=top。operands[init_slot, tableidx]、結果 old→r0。 */
			case 0x0f: {
				uint32_t tblidx = rdu(&C);
				if(!C.unreachable) {
					to_reg_top(&C); /* delta=r0 */
					int ip = C.cdepth - 2;
					uint32_t is = to_slot(&C, ip);
					cb_emit_op(&C.cb, core_table_grow);
					cb_emit_u32(&C.cb, is);
					cb_emit_u32(&C.cb, tblidx);
					C.cdepth -= 2;
					C.reg_pos = -1;
					loc_t l = { L_REG, 0, 0, 0 };
					push_loc(&C, l);
					C.reg_pos = C.cdepth - 1;
				}
				break;
			}
			/* table.fill (0x11): stack[dst,val,len]。operands[dst_slot,val_slot,tableidx]。 */
			case 0x11: {
				uint32_t tblidx = rdu(&C);
				if(!C.unreachable) {
					to_reg_top(&C);
					C.cdepth--;
					C.reg_pos = -1;
					int vpos = C.cdepth - 1;
					int dpos = C.cdepth - 2;
					uint32_t vs = to_slot(&C, vpos);
					uint32_t ds = to_slot(&C, dpos);
					cb_emit_op(&C.cb, core_table_fill);
					cb_emit_u32(&C.cb, ds);
					cb_emit_u32(&C.cb, vs);
					cb_emit_u32(&C.cb, tblidx);
					C.cdepth -= 2;
					C.reg_pos = -1;
				}
				break;
			}
			default:
				fprintf(stderr, "[core compile] unsupported 0xFC sub-op 0x%02x in func %u\n", sub, def_idx);
				core_fatal("unsupported 0xFC op");
			}
			break;
		}
		default:
			fprintf(stderr, "[core compile] unsupported opcode 0x%02x in func %u\n", op, def_idx);
			core_fatal("unsupported op");
		}
	}

	/* trim: codebuf を実使用分 (len) へ縮小し、倍々確保の余剰 (cap-len、最大約2倍) をアリーナへ返す。
	 * これにより巨大モジュール (関数多数) でも compiled の総量がアリーナに収まりやすくなる。
	 * 必ずポインタ解決 (下のループ) より前に行う: realloc でバッファが移動しても、ジャンプ先は
	 * まだ u32 index で保持されており、解決は移動後の C.cb.code に対して行うため安全。 */
	if(C.cb.len > 0 && C.cb.cap > C.cb.len) {
		coreinstr* trimmed = (coreinstr*)kinowasm_mem_realloc(C.cb.code, (size_t)C.cb.len * sizeof(coreinstr));
		if(trimmed != NULL) {
			C.cb.code = trimmed;
			C.cb.cap = C.cb.len;
		}
	}

	/* loop 後方ジャンプ word (u32 index 保存) を絶対ポインタへ解決 */
	for(uint32_t i = 0; i < C.nloops; i++) {
		uint32_t wi = C.loops[i].word_idx;
		uint32_t ci = C.cb.code[wi].u32;
		C.cb.code[wi].tgt = &C.cb.code[ci];
	}
	kinowasm_mem_free(C.loops);

	/* 結果格納 */
	corefunc_t* out = (corefunc_t*)core_alloc(sizeof(corefunc_t));
	out->entry = C.cb.code;
	out->num_slots = C.max_slot;
	out->num_params = ft->num_params;
	out->num_results = ft->num_results;
	out->code_words = C.cb.len;
	out->func_idx = def_idx + m->num_imported_funcs;
	f->compiled = out;
	f->num_slots = C.max_slot;
	kinowasm_mem_free(C.st);
	kinowasm_mem_free(C.ctrl);
}
