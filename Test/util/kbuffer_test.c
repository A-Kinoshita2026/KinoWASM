#include "kbuffer.h"
#include <stdio.h>
#include <string.h>

typedef struct {
	const char* name;
	uint64_t cur;
	size_t size;
	int valid;
} buffer_case_t;

static const buffer_case_t cases[] = {
	{ "zero bytes", 0, 0, 1 },
	{ "partial range", 1, 2, 1 },
	{ "exact remaining range", 1, 3, 1 },
	{ "zero bytes at end", 4, 0, 1 },
	{ "read at end", 4, 1, 0 },
	{ "past remaining range", 3, 2, 0 },
	{ "size overflow", 1, SIZE_MAX, 0 },
	{ "cursor past end", 5, 0, 0 },
	{ "cursor overflow", UINT64_MAX, 1, 0 },
};

static unsigned check_case(const buffer_case_t* test, int skip)
{
	uint8_t data[] = { 10, 20, 30, 40 };
	uint8_t dest[] = { 99, 99, 99, 99 };
	uint8_t expected[] = { 99, 99, 99, 99 };
	kinowasm_buf_t buf;
	kinowasm_buf_set(&buf, data, sizeof(data));
	buf.cur = test->cur;
	kinowasm_result_t result = skip ? kinowasm_buf_skip(test->size, &buf)
		: kinowasm_buf_read_bytes(dest, test->size, &buf);
	kinowasm_result_t expected_result = test->valid ? RES_SUCCESS : ERR_UNEXPECTED_END;
	uint64_t expected_cur = test->valid ? test->cur + test->size : test->cur;
	if(test->valid && !skip && test->size != 0)
		memcpy(expected, data + test->cur, test->size);

	/* Rejected reads must leave both the cursor and destination untouched. */
	if(result != expected_result || buf.cur != expected_cur || memcmp(dest, expected, sizeof(dest)) != 0) {
		fprintf(stderr, "FAIL: %s %s result=%u cur=%llu\n", skip ? "skip" : "read",
			test->name, result, (unsigned long long)buf.cur);
		return 1;
	}
	return 0;
}

int main(void)
{
	unsigned failed = 0;
	for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		failed += check_case(&cases[i], 0);
		failed += check_case(&cases[i], 1);
	}

	/* Empty buffers need no backing storage for a zero-length operation. */
	kinowasm_buf_t empty;
	kinowasm_buf_set(&empty, NULL, 0);
	if(kinowasm_buf_read_bytes(NULL, 0, &empty) != RES_SUCCESS || empty.cur != 0)
		failed++;

	if(kinowasm_buf_skip(0, &empty) != RES_SUCCESS || empty.cur != 0)
		failed++;

	printf("buffer cases=%zu failed=%u\n", 2 * (sizeof(cases) / sizeof(cases[0])) + 2, failed);
	return failed != 0;
}
