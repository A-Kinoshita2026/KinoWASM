
#include <memory.h>
#include "karray.h"
#include "exception.h"
#include "kalloc.h"

void kinowasm_array_init(kinowasm_array_t* ary, size_t item_size)
{
	ary->data = NULL;
	ary->item_size = item_size;
	ary->len = 0;
	ary->capacity = 0;
}

kinowasm_result_t kinowasm_array_new(kinowasm_array_t* ary, size_t item_size, size_t count)
{
	_try{
		if(item_size == 0) {
			kinowasm_array_init(ary, 0);
			_throw(ERR_INVALID_ITEMSIZE);
		} else if(count != 0) {
			/* item_size * count の乗算 overflow を OOM 扱いで弾く。 */
			_throwif(ERR_OUTOFMEMORY, count > SIZE_MAX / item_size);
			ary->data = kinowasm_mem_malloc(item_size * count);
			_throwif(ERR_OUTOFMEMORY, !ary->data);
			ary->item_size = item_size;
			ary->capacity = count;
			ary->len = count;
		} else {
			kinowasm_array_init(ary, item_size);
		}
	}
	_catch:
	return _result;
}

void kinowasm_array_term(kinowasm_array_t* ary)
{
	if(ary == NULL)
		return;

	kinowasm_mem_free(ary->data);
	ary->data = NULL;
	ary->item_size = 0;
	ary->len = 0;
	ary->capacity = 0;
}

kinowasm_result_t kinowasm_array_grow(kinowasm_array_t* ary, size_t count)
{
	_try{
		if(count == 0)
			_throw(RES_SUCCESS);

		/* 加算・乗算 overflow ガード: 悪意ある巨大 count による
		 * realloc 引数 wraparound で小さなバッファが返り、後続 access が OOB に
		 * なるのを防ぐ。OOM として扱う。 */
		_throwif(ERR_OUTOFMEMORY, count > SIZE_MAX - ary->capacity);
		size_t new_capacity = count + ary->capacity;
		_throwif(ERR_OUTOFMEMORY,
			ary->item_size > 0 && new_capacity > SIZE_MAX / ary->item_size);

		void* grow = kinowasm_mem_realloc(ary->data, ary->item_size * new_capacity);
		_throwif(ERR_OUTOFMEMORY, !grow);
		ary->data = grow;
		ary->capacity = new_capacity;
	}
	_catch:
	return _result;
}

kinowasm_result_t kinowasm_array_copy(kinowasm_array_t* dst, kinowasm_array_t* src)
{
	_try{
		_throwiferr(kinowasm_array_new(dst, src->item_size, src->len));
		/* len==0 の空配列は data が NULL (memcpy への NULL はサイズ 0 でも規格上 UB)。 */
		if(src->len != 0)
			memcpy(dst->data, src->data, src->item_size * src->len);
	}
	_catch:
	return _result;
}
