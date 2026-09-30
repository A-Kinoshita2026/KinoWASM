
#include "kalloc.h"
#include "systemmemory.h"

#define SYSTEM_MEMORY_SIZE 100000000

static kinowasm_mem_info_t system_memory = NULL;
static char system_global[SYSTEM_MEMORY_SIZE];

// システムメモリへ切り替える
void change_system_memory(void)
{
	if (system_memory == NULL)
		system_memory = kinowasm_mem_info_init(system_global, sizeof(system_global));
	
	kinowasm_mem_set_info(system_memory);
}

// システムメモリの掃除
void clean_system_memory(void)
{
	kinowasm_mem_info_t p = kinowasm_mem_get_info();
	kinowasm_mem_set_info(system_memory);
	kinowasm_mem_merge();
	kinowasm_mem_set_info(p);
}
