#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "kinowasm.h"
#include "systemmemory.h"
#include "winapi.h"
#include "debug.h"
#include "kw_core_mem_backend_win.h"

#define DATA_MEMORY_SIZE 100000000

void register_standard_func(void);

static const char* find_library_wasm_path(void)
{
	static const char* candidates[] = {
		"library.wasm",
		"../WASMData/library.wasm",
		"../../WASMData/library.wasm"
	};

	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		FILE* fp = fopen(candidates[i], "rb");
		if (fp != NULL) {
			fclose(fp);
			return candidates[i];
		}
	}

	return NULL;
}

static double now_seconds(void)
{
	return (double)clock() / (double)CLOCKS_PER_SEC;
}

static int fibonacci_host(int n)
{
	if (n <= 1)
		return n;

	return fibonacci_host(n - 1) + fibonacci_host(n - 2);
}

static kinowasm_result_t invoke_fibonacci(kinowasm_handle_t handle, int n, int* result)
{
	kinowasm_args_t args = { 0 };
	kinowasm_result_t res = kinowasm_array_new_from(args, 1);
	if (res != RES_SUCCESS)
		return res;

	args.data[0].type = TYPE_VAL_I32;
	args.data[0].val.num.i32 = n;

	res = kinowasm_invoke(handle, "library", "fibonacci", &args);
	if (res == RES_SUCCESS) {
		if (args.len != 1 || args.data[0].type != TYPE_VAL_I32) {
			kinowasm_array_term_from(args);
			return RES_ERROR;
		}

		*result = args.data[0].val.num.i32;
	}

	kinowasm_array_term_from(args);
	return res;
}

static int parse_arg_or_default(const char* value, int default_value)
{
	if (value == NULL)
		return default_value;

	return (int)strtol(value, NULL, 10);
}

static int assign_store_memory(kinowasm_handle_t handle)
{
	void* mem = malloc(DATA_MEMORY_SIZE);
	if (mem == NULL)
		return 0;

	kinowasm_assign_memory(handle, mem, DATA_MEMORY_SIZE);
	return 1;
}

static kinowasm_mem_info_t allocate_module_memory(void)
{
	void* memory = malloc(DATA_MEMORY_SIZE);
	if (memory == NULL)
		return NULL;
	return kinowasm_mem_info_init(memory, DATA_MEMORY_SIZE);
}

int main(int argc, char** argv)
{
	kw_core_install_default_mem_backend();
	const int warmup_count = parse_arg_or_default(argc > 1 ? argv[1] : NULL, 1);
	const int sample_count = parse_arg_or_default(argc > 2 ? argv[2] : NULL, 1);
	const int iteration_count = parse_arg_or_default(argc > 3 ? argv[3] : NULL, 1);
	const int fibonacci_n = parse_arg_or_default(argc > 4 ? argv[4] : NULL, 40);
	const int expected = fibonacci_host(fibonacci_n);
	const char* wasm_path;
	kinowasm_handle_t handle;
	kinowasm_result_t res;
	double total_ms = 0.0;
	double min_ms = 0.0;
	double max_ms = 0.0;

	change_system_memory();
	init_performance();
	register_standard_func();

	wasm_path = find_library_wasm_path();
	if (wasm_path == NULL) {
		fprintf(stderr, "library.wasm was not found\n");
		return 1;
	}

	handle = kinowasm_init();
	if (handle == NULL) {
		fprintf(stderr, "kinowasm_init failed\n");
		return 1;
	}

	assign_store_memory(handle);

	res = kinowasm_load_module(handle, wasm_path, "library", allocate_module_memory());
	if (res != RES_SUCCESS) {
		fprintf(stderr, "kinowasm_load_module failed: %d\n", res);
		kinowasm_term(handle);
		return 1;
	}
#if defined(KINOWASM_OPCODE_COUNTER)
	kinowasm_dump_decoded_function(handle, "library", "fibonacci");
#endif

	printf("invoke_func2 manual harness\n");
	printf("wasm=%s warmup=%d samples=%d iterations=%d fibonacci_n=%d\n", wasm_path, warmup_count, sample_count, iteration_count, fibonacci_n);

	for (int sample_index = -warmup_count; sample_index < sample_count; sample_index++) {
#if defined(KINOWASM_OPCODE_COUNTER)
		if (sample_index == 0)
			kinowasm_reset_opcode_counter();
#endif

		double begin = now_seconds();
		int last_result = 0;

		for (int iteration = 0; iteration < iteration_count; iteration++) {
			res = invoke_fibonacci(handle, fibonacci_n, &last_result);
			if (res != RES_SUCCESS) {
				fprintf(stderr, "kinowasm_invoke failed: %d\n", res);
				kinowasm_term(handle);
				return 1;
			}

			if (last_result != expected) {
				fprintf(stderr, "unexpected result: %d (expected %d)\n", last_result, expected);
				kinowasm_term(handle);
				return 1;
			}
		}

		double elapsed_ms = (now_seconds() - begin) * 1000.0;
		if (sample_index < 0) {
			printf("warmup[%d] %.3f ms\n", sample_index + warmup_count + 1, elapsed_ms);
			continue;
		}

		if (sample_index == 0 || elapsed_ms < min_ms)
			min_ms = elapsed_ms;
		if (sample_index == 0 || elapsed_ms > max_ms)
			max_ms = elapsed_ms;
		total_ms += elapsed_ms;

		printf(
			"sample[%d] total=%.3f ms per_invoke=%.3f ms result=%d\n",
			sample_index + 1,
			elapsed_ms,
			elapsed_ms / (double)iteration_count,
			expected
		);
	}

	printf(
		"summary avg=%.3f ms min=%.3f ms max=%.3f ms spread=%.3f ms\n",
		total_ms / (double)sample_count,
		min_ms,
		max_ms,
		max_ms - min_ms
	);
#if defined(KINOWASM_OPCODE_COUNTER)
	kinowasm_dump_opcode_counter();
#endif

	kinowasm_term(handle);
	return 0;
}
