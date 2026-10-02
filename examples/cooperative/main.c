#include "cooperative_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int run_frames(void)
{
	for(int32_t frame = 1; frame <= 4; frame++) {
		int32_t status = kino_example_tick();
		printf("frame=%d status=%d progress=%d\n", frame, status, kino_example_progress());
		int32_t expected_progress = frame < 4 ? frame * 10 : 30;
		if(kino_example_progress() != expected_progress || status != (frame < 4 ? 1 : 0))
			return 0;

	}
	return kino_example_result() == 60 && kino_example_tick() == 0;
}

int main(int argc, char** argv)
{
	if(argc != 2)
		return 1;

	FILE* file = fopen(argv[1], "rb");
	if(file == NULL)
		return 1;

	if(fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return 1;
	}
	long length = ftell(file);
	if(length <= 0 || (unsigned long)length > UINT32_MAX || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return 1;
	}
	uint8_t* bytes = malloc((size_t)length);
	if(bytes == NULL) {
		fclose(file);
		return 1;
	}
	size_t count = fread(bytes, 1, (size_t)length, file);
	fclose(file);
	int ok = count == (size_t)length;
	if(ok)
		ok = kino_example_open(bytes, (uint32_t)length) == 0 && run_frames();

	kino_example_close();
	/* Restart after normal completion, then cancel while suspended. */
	if(ok)
		ok = kino_example_open(bytes, (uint32_t)length) == 0 && kino_example_tick() == 1;

	kino_example_close();
	if(ok)
		ok = kino_example_open(bytes, (uint32_t)length) == 0 && run_frames();

	kino_example_close();
	free(bytes);
	if(!ok) {
		fprintf(stderr, "FAIL: cooperative example, runtime error=%u\n", kino_example_error());
		return 1;
	}
	puts("PASS: 4 ticks, 3 yields, result=60; reopen and cancellation OK");
	return 0;
}
