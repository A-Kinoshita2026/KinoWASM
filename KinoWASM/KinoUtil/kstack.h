#pragma once

#include <stddef.h>

/* kinowasm_stack_t — 侵入型 (intrusive) 円環双方向リストのノード。利用側の構造体に
 * メンバとして埋め込み、kinowasm_stack_entry でコンテナへ復元する。head 自身も
 * 同型ノードで、空リストは next==prev==自身 (kinowasm_stack_init_entry で初期化)。
 * parser のブロックスタック等で使用する。 */
typedef struct kinowasm_stack {
	struct kinowasm_stack* next;	/* 次ノード (head へ向かう) */
	struct kinowasm_stack* prev;	/* 前ノード (末尾は head->prev) */
} kinowasm_stack_t;

kinowasm_stack_t* kinowasm_stack_tail(kinowasm_stack_t* list);

#define kinowasm_stack_init_entry(stack) do { (stack)->prev = (stack); (stack)->next = (stack); } while(0)
#define kinowasm_stack_entry(ptr, type, member) ((type*)((uint8_t*)(ptr) - offsetof(type, member)))
#define kinowasm_stack_tail_entry(ptr, type, member) kinowasm_stack_entry(kinowasm_stack_tail(ptr), type, member)
#define kinowasm_stack_poptail_entry(ptr, type, member) kinowasm_stack_entry(kinowasm_stack_pop_tail(ptr), type, member)

/* kinowasm_stack_insert — node を prev と next の間に連結する。 */
static inline void kinowasm_stack_insert(kinowasm_stack_t* prev, kinowasm_stack_t* next, kinowasm_stack_t* node)
{
	node->prev = prev;
	node->next = next;
	next->prev = node;
	prev->next = node;
}

/* kinowasm_stack_push_back — list の末尾 (head の直前) に item を追加する。 */
static inline void kinowasm_stack_push_back(kinowasm_stack_t* list, kinowasm_stack_t* item)
{
	kinowasm_stack_insert(list->prev, list, item);
}

/* kinowasm_stack_pop_tail — list の末尾ノードを外して返す。空なら NULL。 */
static inline kinowasm_stack_t* kinowasm_stack_pop_tail(kinowasm_stack_t* list)
{
	kinowasm_stack_t* tail = list->prev;

	if(tail == list)
		return NULL;

	/* Unlink */
	tail->prev->next = tail->next;
	list->prev = tail->prev;

	return tail;
}
