
#include "kstack.h"

kinowasm_stack_t* kinowasm_stack_tail(kinowasm_stack_t* list)
{
	kinowasm_stack_t* tail = list->prev;
	if(tail == list)
		return NULL;

	return tail;
}
