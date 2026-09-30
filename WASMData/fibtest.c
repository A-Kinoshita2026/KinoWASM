/* fibtest.c — フィボナッチベンチマーク (wasm 完結版)。
 *
 * 旧 main.c の FIBTEST (ホスト側で計測して Library.wasm の fibonacci を呼ぶ) を置き換える。
 * 計算・時間計測・結果検証のすべてを wasm 内で行い、ホストは WASI CLI として
 * 実行するだけでよい:
 *
 *   ビルド: build_fibtest.bat (emcc STANDALONE_WASM)
 *   実行:   KinoRuntime.exe fibtest.wasm [n]
 *
 * 時間計測は clock_gettime(CLOCK_MONOTONIC) → WASI clock_time_get に落ちる。
 * 戻り値: 計算結果が期待値と一致すれば 0、不一致なら 1。
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 再帰フィボナッチ (ベンチマーク本体。最適化で畳まれないよう外部可視) */
int fibonacci(int n)
{
	if (n == 0 || n == 1)
		return n;
	return fibonacci(n - 2) + fibonacci(n - 1);
}

/* 既知の期待値 (n=0..46)。47 以降は int32 を溢れるため対象外 */
static long long fib_expect(int n)
{
	long long a = 0, b = 1;
	for (int i = 0; i < n; i++) {
		long long t = a + b;
		a = b;
		b = t;
	}
	return a;
}

int main(int argc, char** argv)
{
	int n = 40;
	if (argc >= 2) {
		n = atoi(argv[1]);
		if (n < 0 || n > 46) {
			fprintf(stderr, "usage: fibtest [n]  (0 <= n <= 46)\n");
			return 1;
		}
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int result = fibonacci(n);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0
	          + (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;

	long long expect = fib_expect(n);
	printf("fibonacci(%d) = %d\n", n, result);
	printf("time: %.2f ms\n", ms);
	if ((long long)result != expect) {
		printf("MISMATCH: expected %lld\n", expect);
		return 1;
	}
	printf("OK\n");
	return 0;
}
