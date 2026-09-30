
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <stdio.h>
#include <windows.h>
#include "winapi.h"

// QueryPerformanceCounter関数の1秒当たりのカウント数を取得する
static LARGE_INTEGER freq, init;
static LARGE_INTEGER start, end;

void init_performance(void)
{
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&init);
}

void begin_performaince(void)
{
	QueryPerformanceCounter(&start);
}

void end_performaince(void)
{
	QueryPerformanceCounter(&end);
}

void print_performance(void)
{
	// init_performance 未呼び出しでは freq=0 になり得る。ゼロ除算を防ぎ、
	// LTCG(/GL) が freq を compile-time 0 と断定して出す C4723 も抑止する。
	float time = 0.0f;
	if (freq.QuadPart != 0)
	{
		time = (float)(end.QuadPart - start.QuadPart) / freq.QuadPart;
	}
	wprintf(L"time %f[ms]\n", time * 1000);
}

float get_running_time(void)
{
	LARGE_INTEGER current;
	QueryPerformanceCounter(&current);
	// 同上: freq 未初期化(=0)ならゼロ除算になるため 0 を返す。
	if (freq.QuadPart == 0)
	{
		return 0.0f;
	}
	return ((float)(current.QuadPart - init.QuadPart) / freq.QuadPart);
}

uint8_t get_key_status(uint8_t code)
{
	if ((GetKeyState(code) & 0x8000) != 0)
	{
		return 1;
	}
	return 0;
}

void system_wait(float waitTime)
{
	if (waitTime <= 0.0f) {
		_mm_pause();
	} else {
		float time = get_running_time();
		while (time + waitTime > get_running_time()) { _mm_pause(); }
	}
}
