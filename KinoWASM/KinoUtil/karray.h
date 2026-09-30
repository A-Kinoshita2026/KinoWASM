#pragma once

#include <stdint.h>
#include <stddef.h>
#include "exception.h"

/* kinowasm_array_t — 型消去された動的配列 (要素サイズ可変)。kinowasm_array_* 関数で
 * 操作する。型付きの kinowasm_array(type) と同一レイアウトで相互キャスト可能。 */
typedef struct kinowasm_array_inner {
	void*  data;		/* 要素バッファ (capacity*item_size byte, kalloc 管理) */
	size_t item_size;	/* 1 要素のバイトサイズ */
	size_t capacity;	/* 確保済み要素数 */
	size_t len;		/* 現在の要素数 (len <= capacity) */
} kinowasm_array_t;

/* kinowasm_array(type) — 型付き動的配列を宣言するマクロ。kinowasm_array_t と同一
 * レイアウトなので *_from マクロで kinowasm_array_t* へキャストして共通関数を呼べる。 */
#define kinowasm_array(type) \
struct { \
	type*  data; \
	size_t item_size; \
	size_t capacity; \
	size_t len; \
}

#define kinowasm_array_init_from(ary) kinowasm_array_init((kinowasm_array_t*)&(ary), sizeof(*(ary).data))
#define kinowasm_array_new_from(ary, count) kinowasm_array_new((kinowasm_array_t*)&(ary), sizeof(*(ary).data), count)
#define kinowasm_array_term_from(ary) kinowasm_array_term((kinowasm_array_t*)&(ary))
/* 注: `len == 0` の空配列でも安全に展開できるよう、ポインタ算術を len > 0 でガードする。
 * 旧来の `&((ary).data[(ary).len - 1])` は len=0 で `data[(size_t)-1]` を計算し、
 * ポインタ算術の未定義動作 (Clang 等の最適化で予期せぬ挙動を起こす) を踏んでいた。
 * 現在は条件演算子で空配列を NULL に倒し、ループ条件で必ず弾く。 */
#define kinowasm_array_foreach(target, type, ary) for(type* target = ((ary).len != 0 ? &((ary).data[0]) : NULL); target != NULL && target != &((ary).data[(ary).len]); target++)
/* reverse 版は最後の要素 (data[0]) を処理した直後に target を NULL に切り替えて終了する。
 * `target--` を data の手前まで進めると pointer 算術の未定義動作になるため、明示的に NULL 化。 */
#define kinowasm_array_foreach_reverse(target, type, ary) for(type* target = ((ary).len != 0 ? &((ary).data[(ary).len - 1]) : NULL); target != NULL; target = (target == &((ary).data[0]) ? NULL : target - 1))
#define kinowasm_array_at(ary, idx) (ary.data[idx])
#define kinowasm_array_at_type(ary, type, idx) ((((type*)(ary).data))[idx])
#define kinowasm_array_grow_from(ary, n) kinowasm_array_grow((kinowasm_array_t*)&ary, n)
#define kinowasm_array_append(ary, val, ret) do { \
	if((ary).len == (ary).capacity) { \
		_throwiferr(kinowasm_array_grow_from(ary, 1)); \
	} \
	(ary).data[(ary).len] = val; \
	ret = (uint32_t)(ary).len++; \
} while(0)
#define kinowasm_array_copy_from(dst, src) kinowasm_array_copy((kinowasm_array_t*)&dst, (kinowasm_array_t*)&src)

void kinowasm_array_init(kinowasm_array_t* ary, size_t item_size);
kinowasm_result_t kinowasm_array_new( kinowasm_array_t* ary, size_t item_size, size_t count);
void kinowasm_array_term(kinowasm_array_t* ary);
kinowasm_result_t kinowasm_array_grow(kinowasm_array_t* ary, size_t count);
kinowasm_result_t kinowasm_array_copy(kinowasm_array_t* dst, kinowasm_array_t* src);
