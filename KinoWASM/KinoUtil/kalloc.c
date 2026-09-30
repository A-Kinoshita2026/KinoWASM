/* kalloc.c — KinoWASM 用アリーナアロケータ (TLSF: Two-Level Segregated Fit)。
 *
 *  旧実装は単一のサイズ昇順フリーリストで、malloc が O(n) の best-fit 走査、free が O(n) の
 *  ソート挿入、かつ free 時の自動 coalescing が無かったため、巨大 WASM のロード/解放 (大量の
 *  確保・解放) で未使用リストが肥大し全体が O(n^2) に悪化していた (ゲーム開始/終了のモタつき)。
 *
 *  本実装は TLSF へ置き換え、malloc/free を保証付き O(1)・境界タグによる O(1) coalescing・
 *  realloc の in-place 成長を実現する。公開 API (kinowasm_mem_*) とアリーナハンドルの意味
 *  (handle == バッファ先頭 == 制御構造体先頭) は完全に維持するため、呼び出し側は無変更。
 *
 *  多アリーナ: 各ブロックヘッダに所属アリーナ (arena back-pointer) を保持し、kinowasm_mem_free は
 *  現在アリーナに依らずブロックから所属を判定する (クロスアリーナ free がこれに依存)。
 *
 *  アラインメント: 既定 16 byte 境界 (payload は常に 16 整列) のため alignof(T)<=16 の確保は
 *  すべて高速パスを通る。page-size 等 16 超の要求のみ memalign の cold path で対応する。
 */

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <stddef.h>
#include <assert.h>
#include "kalloc.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION my_mutex_t;
#define my_mutex_init(m) InitializeCriticalSection(m)
#define my_mutex_lock(m) EnterCriticalSection(m)
#define my_mutex_unlock(m) LeaveCriticalSection(m)
#define my_mutex_destroy(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t my_mutex_t;
#define my_mutex_init(m) do { \
	pthread_mutexattr_t attr; \
	pthread_mutexattr_init(&attr); \
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE); \
	pthread_mutex_init(m, &attr); \
	pthread_mutexattr_destroy(&attr); \
} while(0)
#define my_mutex_lock(m) pthread_mutex_lock(m)
#define my_mutex_unlock(m) pthread_mutex_unlock(m)
#define my_mutex_destroy(m) pthread_mutex_destroy(m)
#endif

/* ───── TLSF パラメータ ───── */
enum {
	ALIGN_LOG2  = 4,                       /* 16 byte 境界 */
	ALIGN_SIZE  = 1 << ALIGN_LOG2,         /* 16 */
	SL_LOG2     = 5,                       /* 第2レベルの分割数 = 32 */
	SL_COUNT    = 1 << SL_LOG2,            /* 32 */
	FL_SHIFT    = SL_LOG2 + ALIGN_LOG2,    /* 9: これ未満は第1レベル 0 (small block) */
	SMALL_BLOCK = 1 << FL_SHIFT,           /* 512 */
	FL_MAX      = 32,                      /* 最大確保 ~4GB を想定 (実アリーナは数十MB) */
	FL_COUNT    = FL_MAX - FL_SHIFT + 1,   /* 24 */
};

/* ブロックヘッダの size 下位ビットをフラグに転用 (size は常に ALIGN_SIZE の倍数 = 下位4bit空き)。 */
#define BH_FREE       ((size_t)1)   /* このブロックが空き */
#define BH_PREV_FREE  ((size_t)2)   /* 直前の物理ブロックが空き */
#define BH_FLAGS      (BH_FREE | BH_PREV_FREE)

/* block_header_t — 1 ブロックのヘッダ。
 *   arena/prev_phys/size は確保中・空き中とも有効。next_free/prev_free は空き時のみ有効で
 *   確保中は payload の一部 (= ユーザ領域に重なる)。payload は (uint8_t*)b + HEADER_SIZE から。
 *   _pad は payload を 16 整列させるための詰め物 (64bit: ヘッダ 32byte / 32bit: 16byte)。 */
typedef struct block_header {
	struct kalloc_data*  arena;       /* 所属アリーナ (クロスアリーナ free 用) */
	struct block_header* prev_phys;   /* 直前の物理ブロック */
	size_t               size;        /* payload byte 数 | フラグ */
	size_t               _pad;        /* payload を 16 整列させる詰め物 */
	struct block_header* next_free;   /* 空きリスト前方 (空き時のみ) */
	struct block_header* prev_free;   /* 空きリスト後方 (空き時のみ) */
} block_header_t;

#define HEADER_SIZE  (offsetof(block_header_t, next_free))   /* payload 開始オフセット (= 32 on 64bit) */
#define MIN_PAYLOAD  ((size_t)ALIGN_SIZE)                    /* 空きブロックが next/prev_free を持てる最小 payload */

/* kalloc_data — 1 アリーナの制御構造体。アリーナバッファの先頭に配置 (handle == この先頭)。 */
struct kalloc_data {
	block_header_t* blocks[FL_COUNT][SL_COUNT];  /* 空きリスト頭 [fl][sl] */
	uint32_t        fl_bitmap;                   /* 非空 fl のビット */
	uint32_t        sl_bitmap[FL_COUNT];         /* 各 fl 内の非空 sl のビット */
	uint8_t*        pool_end;                    /* 使用領域終端 (末尾 sentinel の先) */
	size_t          used_bytes;                  /* 確保中 payload 合計 */
	size_t          free_bytes;                  /* 空き payload 合計 */
};

static struct kalloc_data* current_kalloc;
static int is_mtx_inited = 0;
static my_mutex_t alloc_lock;

/* ───── ビット走査 (fls=最上位bit位置 / ffs=最下位bit位置、入力は非0) ───── */
#if defined(_MSC_VER)
static inline int kw_fls_sz(size_t x)
{
	unsigned long i;
#if defined(_WIN64) || defined(_M_X64) || defined(_M_ARM64)
	_BitScanReverse64(&i, (unsigned __int64)x);
#else
	/* 32bit MSVC: size_t は 32bit で x >> 32 は幅超シフト UB (x86 実機では mod 32 され
	 * x >> 0 == x となり全入力で +32 ズレる)。上位語は存在しないので直接走査する。 */
	_BitScanReverse(&i, (unsigned long)x);
#endif
	return (int)i;
}
static inline int kw_ffs_u32(uint32_t x)
{
	unsigned long i;
	_BitScanForward(&i, x);
	return (int)i;
}
#elif defined(__GNUC__) || defined(__clang__)
static inline int kw_fls_sz(size_t x)
{
	return (int)(sizeof(unsigned long long) * 8 - 1) - __builtin_clzll((unsigned long long)x);
}
static inline int kw_ffs_u32(uint32_t x)
{
	return __builtin_ctz(x);
}
#else
static inline int kw_fls_sz(size_t x)
{
	int r = 0;
	while(x >>= 1)
		r++;

	return r;
}
static inline int kw_ffs_u32(uint32_t x)
{
	int r = 0;
	while(!(x & 1)) {
		x >>= 1;
		r++;
	}
	return r;
}
#endif

static inline size_t align_up_sz(size_t x, size_t a)
{
	return (x + (a - 1)) & ~(a - 1);
}
static inline uintptr_t align_up_ptr(uintptr_t x, size_t a)
{
	return (x + (a - 1)) & ~((uintptr_t)a - 1);
}

/* ───── ブロック基本操作 ───── */
static inline size_t block_size(const block_header_t* b)
{
	return b->size & ~BH_FLAGS;
}
static inline void block_set_size(block_header_t* b, size_t s)
{
	b->size = s | (b->size & BH_FLAGS);
}
static inline int block_is_free(const block_header_t* b)
{
	return (b->size & BH_FREE) != 0;
}
static inline int block_is_prev_free(const block_header_t* b)
{
	return (b->size & BH_PREV_FREE) != 0;
}
static inline void block_set_free(block_header_t* b)
{
	b->size |= BH_FREE;
}
static inline void block_set_used(block_header_t* b)
{
	b->size &= ~BH_FREE;
}
static inline void block_set_prev_free(block_header_t* b)
{
	b->size |= BH_PREV_FREE;
}
static inline void block_set_prev_used(block_header_t* b)
{
	b->size &= ~BH_PREV_FREE;
}
static inline void* block_to_ptr(const block_header_t* b)
{
	return (void*)((uint8_t*)b + HEADER_SIZE);
}
static inline block_header_t* block_from_ptr(const void* p)
{
	return (block_header_t*)((uint8_t*)p - HEADER_SIZE);
}
static inline block_header_t* block_next(const block_header_t* b)
{
	return (block_header_t*)((uint8_t*)block_to_ptr(b) + block_size(b));
}

/* ───── サイズ → (fl, sl) マッピング ───── */
static void mapping_insert(size_t size, int* fli, int* sli)
{
	int fl, sl;
	if(size < SMALL_BLOCK) {
		fl = 0;
		sl = (int)(size / (SMALL_BLOCK / SL_COUNT));
	} else {
		fl = kw_fls_sz(size);
		sl = (int)((size >> (fl - SL_LOG2)) - SL_COUNT);
		fl -= (FL_SHIFT - 1);
	}
	/* 巨大要求のクランプ */
	if(fl >= FL_COUNT) {
		fl = FL_COUNT - 1;
		sl = SL_COUNT - 1;
	}
	*fli = fl;
	*sli = sl;
}
/* 確保用: 見つかった最初のブロックが必ず収まるよう size を切り上げてから索引化する。 */
static void mapping_search(size_t size, int* fli, int* sli)
{
	if(size >= SMALL_BLOCK) {
		size_t round = ((size_t)1 << (kw_fls_sz(size) - SL_LOG2)) - 1;
		size += round;
	}
	mapping_insert(size, fli, sli);
}

static block_header_t* find_suitable_block(struct kalloc_data* c, int* fli, int* sli)
{
	int fl = *fli, sl = *sli;
	uint32_t sl_map = c->sl_bitmap[fl] & (0xFFFFFFFFu << sl);
	if(!sl_map) {
		uint32_t fl_map = c->fl_bitmap & ((fl + 1 >= 32) ? 0u : (0xFFFFFFFFu << (fl + 1)));
		if(!fl_map)
			return NULL;   /* Out of memory */
		fl = kw_ffs_u32(fl_map);
		sl_map = c->sl_bitmap[fl];
	}
	sl = kw_ffs_u32(sl_map);
	*fli = fl;
	*sli = sl;
	return c->blocks[fl][sl];
}

/* ───── 空きリスト (bin) 出し入れ ───── */
static void insert_free_block(struct kalloc_data* c, block_header_t* b)
{
	int fl;
	int sl;
	mapping_insert(block_size(b), &fl, &sl);
	block_header_t* head = c->blocks[fl][sl];
	b->next_free = head;
	b->prev_free = NULL;
	if(head)
		head->prev_free = b;

	c->blocks[fl][sl] = b;
	c->fl_bitmap |= (1u << fl);
	c->sl_bitmap[fl] |= (1u << sl);
}
static void remove_free_block(struct kalloc_data* c, block_header_t* b)
{
	int fl;
	int sl;
	mapping_insert(block_size(b), &fl, &sl);
	block_header_t* prev = b->prev_free;
	block_header_t* next = b->next_free;
	if(next)
		next->prev_free = prev;

	if(prev)
		prev->next_free = next;

	if(c->blocks[fl][sl] == b) {
		c->blocks[fl][sl] = next;
		if(!next) {
			c->sl_bitmap[fl] &= ~(1u << sl);
			if(!c->sl_bitmap[fl])
				c->fl_bitmap &= ~(1u << fl);
		}
	}
}

/* bin から外した空きブロック b (free_bytes は減算済) を payload `size` で確保し、余りは分割して
 * 空きへ戻す。b を used 化し payload ポインタを返す。 */
static void* prepare_used_block(struct kalloc_data* c, block_header_t* b, size_t size)
{
	size_t bs = block_size(b);
	if(bs >= size + HEADER_SIZE + MIN_PAYLOAD) {
		/* 分割: b は size、余りは新たな空きブロック rem。 */
		block_set_size(b, size);
		block_header_t* rem = block_next(b);
		rem->arena = c;
		rem->prev_phys = b;
		rem->size = (bs - size - HEADER_SIZE) | BH_FREE;   /* b は直後 used 化 → PREV_FREE は付けない */
		block_header_t* after = block_next(rem);
		after->prev_phys = rem;
		block_set_prev_free(after);                        /* rem が空きなので after の PREV_FREE 立てる */
		insert_free_block(c, rem);
		c->free_bytes += block_size(rem);
	}
	block_set_used(b);
	block_header_t* nb = block_next(b);
	nb->prev_phys = b;
	block_set_prev_used(nb);
	c->used_bytes += block_size(b);
	return block_to_ptr(b);
}

/* ───── 確保 ───── */
static void* alloc_from(struct kalloc_data* c, size_t size /* adjusted */)
{
	int fl;
	int sl;
	mapping_search(size, &fl, &sl);
	block_header_t* b = find_suitable_block(c, &fl, &sl);
	if(!b)
		return NULL;
	remove_free_block(c, b);
	c->free_bytes -= block_size(b);
	return prepare_used_block(c, b, size);
}

/* alignment > ALIGN_SIZE の cold path。十分大きい空きブロックを取り、payload を alignment 境界へ
 * 進めて先頭ギャップを空きブロックとして切り出す。 */
static void* memalign_from(struct kalloc_data* c, size_t alignment, size_t size)
{
	const size_t overhead = alignment + HEADER_SIZE + MIN_PAYLOAD;
	/* overhead の加算が wrap する、または合計が bin 切り上げ安全域 (SIZE_MAX/2、
	 * adjust_request_size と同基準) を超える要求は undersized 確保になるため確保不能とする。 */
	if(overhead < alignment || overhead > SIZE_MAX / 2 || size > SIZE_MAX / 2 - overhead)
		return NULL;
	int fl;
	int sl;
	mapping_search(size + overhead, &fl, &sl);
	block_header_t* b = find_suitable_block(c, &fl, &sl);
	if(!b)
		return NULL;
	remove_free_block(c, b);
	c->free_bytes -= block_size(b);

	uintptr_t p = (uintptr_t)block_to_ptr(b);
	uintptr_t aligned = align_up_ptr(p, alignment);
	size_t gap = (size_t)(aligned - p);
	if(gap != 0) {
		/* 先頭ギャップにヘッダ + 最小 payload が収まるまで境界を進める。 */
		if(gap < HEADER_SIZE + MIN_PAYLOAD) {
			aligned = align_up_ptr(p + (HEADER_SIZE + MIN_PAYLOAD), alignment);
			gap = (size_t)(aligned - p);
		}
		/* b を [先頭空き | tail] に分割。tail のヘッダは aligned - HEADER_SIZE に置く。 */
		size_t lead_payload = gap - HEADER_SIZE;
		size_t bs = block_size(b);
		block_set_size(b, lead_payload);
		block_header_t* tail = block_next(b);   /* = (aligned - HEADER_SIZE) のブロック */
		tail->arena = c;
		tail->prev_phys = b;
		tail->size = (bs - lead_payload - HEADER_SIZE) | BH_FREE | BH_PREV_FREE;
		block_header_t* after = block_next(tail);
		after->prev_phys = tail;
		/* 先頭空き b を bin へ戻す (b は空きのまま)。 */
		insert_free_block(c, b);
		c->free_bytes += block_size(b);
		b = tail;   /* 以後 aligned 先頭の tail を確保対象にする */
	}
	return prepare_used_block(c, b, size);
}

/* ───── 解放 (境界タグで O(1) coalescing) ───── */

/* release_free_block — 空き (BH_FREE 済)・bin 未登録・自身は used/free 未計上のブロック b を、
 * 前後の物理隣接が空きならマージしてから bin へ挿入する。used_bytes は触らず free_bytes のみ更新
 * (マージで外す隣接分を減算し、最終ブロック分を加算)。free_block と realloc 縮小/成長が共用する。 */
static void release_free_block(struct kalloc_data* c, block_header_t* b)
{
	block_set_free(b);

	/* 直前の物理ブロックが空きならマージ。 */
	if(block_is_prev_free(b)) {
		block_header_t* prev = b->prev_phys;
		remove_free_block(c, prev);
		c->free_bytes -= block_size(prev);
		block_set_size(prev, block_size(prev) + block_size(b) + HEADER_SIZE);
		block_next(prev)->prev_phys = prev;
		b = prev;   /* prev は空きだったので PREV_FREE は元から無い (= 直前は used) */
	}
	/* 直後の物理ブロックが空きならマージ。 */
	block_header_t* nb = block_next(b);
	if(block_is_free(nb)) {
		remove_free_block(c, nb);
		c->free_bytes -= block_size(nb);
		block_set_size(b, block_size(b) + block_size(nb) + HEADER_SIZE);
		block_next(b)->prev_phys = b;
	}
	/* b を空きとして確定。直後ブロックへ PREV_FREE と prev_phys を反映。 */
	block_set_free(b);
	block_header_t* after = block_next(b);
	after->prev_phys = b;
	block_set_prev_free(after);
	insert_free_block(c, b);
	c->free_bytes += block_size(b);
}

static void free_block(struct kalloc_data* c, block_header_t* b)
{
	c->used_bytes -= block_size(b);   /* b は used → 計上を外す */
	release_free_block(c, b);
}

/* resize_used_in_place — used ブロック b (block_size(b) >= adjusted) を payload adjusted へ縮める。
 * 余りが 1 ブロック分以上あれば分割し空きへ戻す (直後が空きなら coalesce)。 */
static void resize_used_in_place(struct kalloc_data* c, block_header_t* b, size_t adjusted)
{
	size_t bs = block_size(b);
	if(bs < adjusted + HEADER_SIZE + MIN_PAYLOAD)
		return;   /* 余り僅少 → そのまま (やや大きめを返す) */
	c->used_bytes -= bs;
	block_set_size(b, adjusted);
	block_header_t* rem = block_next(b);
	rem->arena = c;
	rem->prev_phys = b;
	rem->size = (bs - adjusted - HEADER_SIZE) | BH_FREE;   /* prev (b) は used 維持 */
	block_header_t* after = block_next(rem);
	after->prev_phys = rem;
	block_set_prev_free(after);
	c->used_bytes += adjusted;
	release_free_block(c, rem);   /* rem の直後が空きなら coalesce される */
}

/* ───── 公開 API ───── */
void kinowasm_mem_init(void)
{
	current_kalloc = NULL;
	if(!is_mtx_inited) {
		my_mutex_init(&alloc_lock);
		is_mtx_inited = 1;
	}
}

void kinowasm_mem_cleanup(void)
{
	if(is_mtx_inited) {
		my_mutex_destroy(&alloc_lock);
		is_mtx_inited = 0;
	}
	current_kalloc = NULL;
}

kinowasm_mem_info_t kinowasm_mem_info_init(void* memory, size_t len)
{
	if(memory == NULL)
		return NULL;

	if(!is_mtx_inited)
		kinowasm_mem_init();

	/* レイアウト: [制御構造体][big free block][末尾 sentinel(size 0, used)]。
	 * 先頭ブロックは payload が 16 整列するよう絶対アドレスで切り上げる。 */
	uintptr_t base = (uintptr_t)memory;
	uintptr_t ctrl_end = base + sizeof(struct kalloc_data);
	uintptr_t first = align_up_ptr(ctrl_end, ALIGN_SIZE);
	uintptr_t end = base + len;

	/* big block ヘッダ + payload + sentinel ヘッダ が収まるか。 */
	if(end < first + 2 * HEADER_SIZE + MIN_PAYLOAD)
		return NULL;

	size_t big_payload = (size_t)(end - first - 2 * HEADER_SIZE);
	big_payload &= ~(size_t)(ALIGN_SIZE - 1);   /* ALIGN 倍数へ切り下げ */
	if(big_payload < MIN_PAYLOAD)
		return NULL;

	struct kalloc_data* c = (struct kalloc_data*)memory;
	memset(c, 0, sizeof(*c));

	block_header_t* big = (block_header_t*)first;
	big->arena = c;
	big->prev_phys = NULL;
	big->size = big_payload | BH_FREE;          /* 直前無し → PREV_FREE 無し */

	block_header_t* sentinel = block_next(big);  /* first + HEADER_SIZE + big_payload */
	sentinel->arena = c;
	sentinel->prev_phys = big;
	sentinel->size = 0 | BH_PREV_FREE;           /* size 0, used, 直前 (big) は空き */

	c->pool_end = (uint8_t*)sentinel + HEADER_SIZE;
	c->used_bytes = 0;
	c->free_bytes = big_payload;
	insert_free_block(c, big);

	return (kinowasm_mem_info_t)c;
}

kinowasm_mem_info_t kinowasm_mem_get_info(void)
{
	return (kinowasm_mem_info_t)current_kalloc;
}

void kinowasm_mem_set_info(kinowasm_mem_info_t k)
{
	current_kalloc = (struct kalloc_data*)k;
}

static size_t adjust_request_size(size_t size)
{
	if(size == 0)
		return MIN_PAYLOAD;
	/* 上限ガード: (1) align_up_sz の切り上げ (size + ALIGN_SIZE-1)、(2) mapping_search の
	 * bin 切り上げ (size += 2^(fls-5)-1)、(3) prepare_used_block の分割判定 (size + HEADER_SIZE
	 * + MIN_PAYLOAD) のいずれかが wrap すると、巨大要求が小ブロックとして「成功」しヒープ破壊に
	 * なる。size <= SIZE_MAX/2 なら (1)(2)(3) とも wrap しない (size + 2^(bits-6) < 2^bits) ため、
	 * それを超える要求は確保不能として 0 を返し、呼び出し側に NULL 返却させる。 */
	if(size > SIZE_MAX / 2)
		return 0;
	size_t s = align_up_sz(size, ALIGN_SIZE);
	if(s < MIN_PAYLOAD)
		s = MIN_PAYLOAD;
	return s;
}

void* kinowasm_mem_malloc_aligned(size_t alignment, size_t size)
{
	my_mutex_lock(&alloc_lock);
	void* p = NULL;
	if(current_kalloc != NULL) {
		size_t adjusted = adjust_request_size(size);
		if(adjusted != 0) {   /* 0 = 切り上げ overflow (確保不能)。p は NULL のまま。 */
			if(alignment <= ALIGN_SIZE)
				p = alloc_from(current_kalloc, adjusted);
			else
				p = memalign_from(current_kalloc, alignment, adjusted);
		}
	}
#if defined(KINOWASM_ALLOC_DEBUG)
	if(p != NULL)
		memset(p, 0xCD, size);
#endif
	my_mutex_unlock(&alloc_lock);
	return p;
}

void* kinowasm_mem_calloc_aligned(size_t num, size_t alignment, size_t size)
{
	/* num*size のオーバーフローを検出 (小さい確保を返して呼び出し側が誤認するのを回避)。 */
	if(num != 0 && size > SIZE_MAX / num)
		return NULL;
	const size_t real_size = num * size;
	void* p = kinowasm_mem_malloc_aligned(alignment, real_size);
	if(p != NULL)
		memset(p, 0, real_size);
	return p;
}

void* kinowasm_mem_realloc_aligned(void* src, size_t alignment, size_t new_size)
{
	if(src == NULL)
		return kinowasm_mem_malloc_aligned(alignment, new_size);

	my_mutex_lock(&alloc_lock);
	block_header_t* b = block_from_ptr(src);
	struct kalloc_data* c = b->arena;
	const size_t cursize = block_size(b);
	const size_t adjusted = adjust_request_size(new_size);
	if(adjusted == 0) {   /* 切り上げ overflow: 拡張不能。src は保持したまま NULL を返す。 */
		my_mutex_unlock(&alloc_lock);
		return NULL;
	}

	/* in-place 可能か判定。alignment 拘束は src が既に満たしている場合のみ維持できる。 */
	int aligned_ok = (alignment <= ALIGN_SIZE) || (((uintptr_t)src % alignment) == 0);

	if(aligned_ok && adjusted <= cursize) {
		/* 縮小 (または同サイズ): 余りを分割して返すだけ。 */
		resize_used_in_place(c, b, adjusted);
		my_mutex_unlock(&alloc_lock);
		return src;
	}

	if(aligned_ok) {
		/* 直後ブロックが空きで吸収すれば足りるなら in-place 成長 (コピー回避)。 */
		block_header_t* nb = block_next(b);
		if(block_is_free(nb) && cursize + HEADER_SIZE + block_size(nb) >= adjusted) {
			size_t nbs = block_size(nb);
			remove_free_block(c, nb);
			c->free_bytes -= nbs;
			block_set_size(b, cursize + HEADER_SIZE + nbs);   /* b を used のまま拡大 */
			block_header_t* after = block_next(b);
			after->prev_phys = b;
			block_set_prev_used(after);
			c->used_bytes += HEADER_SIZE + nbs;                /* 旧ヘッダ + 旧 nb payload が used 化 */
			resize_used_in_place(c, b, adjusted);             /* 余りは分割して空きへ */
			my_mutex_unlock(&alloc_lock);
			return src;
		}
	}

	/* フォールバック: src と同じアリーナから確保 → コピー → 旧領域解放。OOM 時は src を保持。 */
	void* p;
	if(alignment <= ALIGN_SIZE)
		p = alloc_from(c, adjusted);
	else
		p = memalign_from(c, alignment, adjusted);
	if(p != NULL) {
		memcpy(p, src, cursize < new_size ? cursize : new_size);
		free_block(c, b);
	}
	my_mutex_unlock(&alloc_lock);
	return p;
}

void kinowasm_mem_free(void* p)
{
	if(p == NULL)
		return;
	my_mutex_lock(&alloc_lock);
	block_header_t* b = block_from_ptr(p);
#if defined(KINOWASM_ALLOC_DEBUG)
	memset(p, 0xDD, block_size(b));
#endif
	free_block(b->arena, b);
	my_mutex_unlock(&alloc_lock);
}

/* TLSF は free 時に自動 coalescing するため明示マージは不要。互換のため成功 (0) を返す no-op。 */
uint32_t kinowasm_mem_merge(void)
{
	return 0;
}

size_t kinowasm_mem_used_size(void)
{
	my_mutex_lock(&alloc_lock);
	size_t r = current_kalloc ? current_kalloc->used_bytes : 0;
	my_mutex_unlock(&alloc_lock);
	return r;
}

size_t kinowasm_mem_unuse_size(void)
{
	my_mutex_lock(&alloc_lock);
	size_t r = current_kalloc ? current_kalloc->free_bytes : 0;
	my_mutex_unlock(&alloc_lock);
	return r;
}

size_t kinowasm_mem_total_size(void)
{
	my_mutex_lock(&alloc_lock);
	size_t r = current_kalloc ? (current_kalloc->used_bytes + current_kalloc->free_bytes) : 0;
	my_mutex_unlock(&alloc_lock);
	return r;
}
