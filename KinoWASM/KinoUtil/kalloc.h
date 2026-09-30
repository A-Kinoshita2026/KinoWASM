#pragma once

#include <stdint.h>
#include <stddef.h>

#define KINOWASM_FORCE_ALIGNED

/* kinowasm_kalloc_const_t — kalloc アロケータの定数。 */
typedef enum {
	MINIMUM_BLOCK_SIZE = 24,	/* 1 ブロックの最小サイズ (ヘッダ込み, byte) */
#if defined(KINOWASM_FORCE_ALIGNED)
	KINOWASM_ALIGNED_SIZE = 8,	/* 既定アライメント (8 byte 強制) */
#else
	KINOWASM_ALIGNED_SIZE = 1,	/* 非強制時は 1 (アライメントなし) */
#endif
} kinowasm_kalloc_const_t;

/* kinowasm_mem_info_t — kalloc ヒープ領域 (アリーナ) を指すハンドル。
 * kinowasm_mem_info_init で領域に紐付け、get/set_info で現在のアリーナを切り替える。 */
typedef uint8_t* kinowasm_mem_info_t;

void kinowasm_mem_init(void);
void kinowasm_mem_cleanup(void);
kinowasm_mem_info_t kinowasm_mem_info_init(void* memory, size_t len);
kinowasm_mem_info_t kinowasm_mem_get_info(void);
void kinowasm_mem_set_info(kinowasm_mem_info_t k);
void* kinowasm_mem_calloc_aligned(size_t num, size_t alignment, size_t size);
void* kinowasm_mem_realloc_aligned(void* src, size_t alignment, size_t new_size);
void* kinowasm_mem_malloc_aligned(size_t alignment, size_t size);
void kinowasm_mem_free(void* p);
uint32_t kinowasm_mem_merge(void);
size_t kinowasm_mem_used_size(void);
size_t kinowasm_mem_unuse_size(void);
size_t kinowasm_mem_total_size(void);

/* kinowasm_mem_malloc — 既定アライメントで size byte を確保する (失敗時 NULL)。 */
static inline void* kinowasm_mem_malloc(size_t size)
{
	return kinowasm_mem_malloc_aligned(KINOWASM_ALIGNED_SIZE, size);
}

/* kinowasm_mem_calloc — 既定アライメントで num*size byte を確保しゼロ初期化する (失敗時 NULL)。 */
static inline void* kinowasm_mem_calloc(size_t num, size_t size)
{
	return kinowasm_mem_calloc_aligned(num, KINOWASM_ALIGNED_SIZE, size);
}

/* kinowasm_mem_realloc — 既定アライメントで src を new_size byte に再確保する (失敗時 NULL)。 */
static inline void* kinowasm_mem_realloc(void* src, size_t new_size)
{
	return kinowasm_mem_realloc_aligned(src, KINOWASM_ALIGNED_SIZE, new_size);
}
