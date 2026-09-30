/* kutil_microtest.c - KinoUtil (kalloc / karray / kbuffer) microtests and
 * benchmarks. These were formerly compile-time gates (ARRAYTEST1/2/3, COPY_TEST)
 * baked into main.c; moved here so the production entry point stays clean.
 *
 * Usage: kutil_microtest <copytest|arraytest1|arraytest2|arraytest3>
 *
 * Links KinoUtil only (+ winapi.c for the perf timer). ASCII-only comments so
 * MSVC (CP932 + /WX) does not raise C4819 without a UTF-8 BOM.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "kalloc.h"
#include "karray.h"
#include "kbuffer.h"
#include "winapi.h"

#define ARENA_SIZE 100000000
static char data[ARENA_SIZE];

/* Reset the kalloc arena to a fresh state (poison fill + re-init). */
static void set_arena(void)
{
	memset(data, -52, sizeof(data));
	kinowasm_mem_set_info(kinowasm_mem_info_init(data, sizeof(data)));
}

/* COPY_TEST: round-trip a double / int64 through a uint32 pair (verifies the
 * bit layout used by the runtime's split/merge of 64-bit values). */
static int copytest(void)
{
	uint32_t doubledata[2];
	union { int64_t i64; double f64; } box;

	double d1 = 0.000000000000001;
	printf("d1: %1.15lf\n", d1);
	doubledata[0] = *(uint32_t*)&d1;
	doubledata[1] = *((uint32_t*)&d1 + 1);
	*((uint32_t*)&box.i64)     = doubledata[0];
	*((uint32_t*)&box.i64 + 1) = doubledata[1];
	printf("d2: %1.15lf\n", box.f64);

	int64_t i1 = -123456789012345;
	printf("i1: %" PRId64 "\n", i1);
	doubledata[0] = *(uint32_t*)&i1;
	doubledata[1] = *((uint32_t*)&i1 + 1);
	*((uint32_t*)&box.i64)     = doubledata[0];
	*((uint32_t*)&box.i64 + 1) = doubledata[1];
	printf("i2: %" PRId64 "\n", box.i64);
	return 0;
}

/* ARRAYTEST1: read 1,000,000 bytes through the kbuffer reader (throughput). */
static int arraytest1(void)
{
	set_arena();
	kinowasm_array(uint8_t) file_data = { 0 };
	kinowasm_buf_t buf;
	kinowasm_array_new_from(file_data, 1000000);
	kinowasm_buf_set(&buf, file_data.data, file_data.len);
	uint64_t a = 0;
	begin_performaince();
	for (int i = 0; i < 1000000; i++) {
		uint8_t c;
		kinowasm_buf_read_u8(&c, &buf);
		a += c;
	}
	end_performaince();
	print_performance();
	printf("arraytest1: sum=%llu\n", (unsigned long long)a);
	return 0;
}

/* ARRAYTEST2: aligned allocation addresses. */
static int arraytest2(void)
{
	set_arena();
	begin_performaince();
	for (int i = 0; i < 10; i++) {
		int* p = kinowasm_mem_malloc_aligned(256, sizeof(int));
		printf("Addr: %zu\n", (size_t)p);
	}
	end_performaince();
	print_performance();
	return 0;
}

/* ARRAYTEST3: kalloc malloc/calloc/free stress across repeated arena resets. */
static int arraytest3(void)
{
	set_arena();
	int* int1 = (int*)kinowasm_mem_calloc(1, sizeof(int));
	*int1 = 47;
	char* char1 = (char*)kinowasm_mem_malloc(sizeof(char));
	*char1 = 50;
	kinowasm_mem_free(int1);
	int* int2 = (int*)kinowasm_mem_malloc(sizeof(int));
	*int2 = 100;
	kinowasm_mem_free(int2);
	kinowasm_mem_free(char1);

	for (int k = 0; k < 20; k++) {
		set_arena();

		char* char2[100];
		for (int i = 0; i < 100; i++) {
			char2[i] = kinowasm_mem_malloc(sizeof(char));
			*char2[i] = 50;
		}
		for (int i = 0; i < 100; i++)
			kinowasm_mem_free(char2[i]);
		kinowasm_mem_merge();

		begin_performaince();
		for (int i = 0; i < 1000000; i++) {
			int* int3 = kinowasm_mem_calloc(1, sizeof(int));
			*int3 = 47;
		}
		end_performaince();
		print_performance();
	}
	return 0;
}

int main(int argc, char** argv)
{
	init_performance();
	const char* which = (argc >= 2) ? argv[1] : "";
	if (strcmp(which, "copytest") == 0)   return copytest();
	if (strcmp(which, "arraytest1") == 0) return arraytest1();
	if (strcmp(which, "arraytest2") == 0) return arraytest2();
	if (strcmp(which, "arraytest3") == 0) return arraytest3();
	fprintf(stderr, "usage: %s <copytest|arraytest1|arraytest2|arraytest3>\n", argv[0]);
	return 2;
}
