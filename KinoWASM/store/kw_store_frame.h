#pragma once
/* ============================================================================
 *  store_frame.h -- frame / localpool / value-stack の hot inline 補助関数。
 *
 *  OP_CALL や関数 return など hot path から呼ばれるため static inline のまま header
 *  に置き、複数 TU (store.c corpus / runtime_eh.c / runtime_invoke.c) でインライン
 *  展開させる。型と push_val は include 側で store.h を先に取り込むこと。
 * ========================================================================== */

static inline localpool_t* kw_find_localpool(store_t * S, size_t size)
{
	if(size == 0) /* サイズ0はローカル変数がないことを意味するため、ローカルプールを使用せずに直接返す */
		return &S->localpool;

	localpool_t* prev = &S->localpool;
	localpool_t* p = prev->next;
	while(p != NULL) {
		if(size == p->local_size) {
			prev->next = p->next;
			p->next = NULL;
			return p;
		}
		prev = p;
		p = p->next;
	}
	p = (localpool_t*)kinowasm_mem_malloc(sizeof(localpool_t));
	if(p == NULL)
		return NULL;

	p->next = NULL;
	p->local_size = size;
	p->locals = (kinowasm_val_t*)kinowasm_mem_malloc(sizeof(kinowasm_val_t) * size);
	if(p->locals == NULL) {
		kinowasm_mem_free(p);
		return NULL;
	}
	return p;
}

static inline void kw_return_localpool(store_t* S, localpool_t* p)
{
	if(p == &S->localpool)
		return;

	p->next = S->localpool.next;
	S->localpool.next = p;
}

static inline frame_t* kw_assign_frame(store_t* S)
{
	if(S->framepool.len == 0) {
		return kinowasm_mem_malloc(sizeof(frame_t));
	}
	return S->framepool.data[--S->framepool.len];
}

static inline void kw_return_frame(store_t* S, frame_t* frame)
{
	if(S->framepool.len == S->framepool.capacity) {
		kinowasm_mem_free(frame);
		return;
	}
	S->framepool.data[S->framepool.len++] = frame;
}

static inline kinowasm_result_t kw_new_stack(framestack_t** d)
{
	_try{
		framestack_t * stack = *d = (framestack_t*)kinowasm_mem_malloc(sizeof(framestack_t));
		_throwif(ERR_OUTOFMEMORY, stack == NULL);

		*stack = (framestack_t){
			.frame_idx = -1,
		};

		_throwiferr(kinowasm_array_new_from(stack->frames, OBJECT_CACHE_SIZE));
	}
	_catch:
	return _result;
}

static inline kinowasm_result_t kw_push_frame(framestack_t* stack, frame_t* frame)
{
	_try{
		int64_t next_frame_idx = stack->frame_idx + 1;
		if(next_frame_idx == (int32_t)stack->frames.capacity)
			_throwiferr(kinowasm_array_grow_from(stack->frames, OBJECT_CACHE_SIZE));

		stack->frame_idx = next_frame_idx;
		stack->frames.data[stack->frame_idx] = frame;
	}
	_catch:
	return _result;
}

static inline void kw_pop_frame(framestack_t* stack, frame_t** frame)
{
	*frame = (frame_t*)stack->frames.data[stack->frame_idx--];
}
