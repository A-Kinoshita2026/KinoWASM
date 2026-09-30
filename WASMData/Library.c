
#include "System/ScriptSystem.h"

#define SIZE 3
void SubTest(int* a, int* b);

int ConsoleTest(int a, int b)
{
	int Array[SIZE];
	Test1(a);
	Test1(b);
	Array[0] = a;
	Array[1] = b;
	Array[2] = 10;
	//int c = a * 2 + b * 2;
	int c = 0;
	Console(u"LLLLL");
	SubTest(&c, Array);
	return c;
}

void SubTest(int* a, int* b)
{
	for(int i = 0; i < SIZE; i++)
		*a = *a + *b++;
}

// フィボナッチ数列を再帰的な方法で計算する関数
int fibonacci(int n) {
	if (n == 0 || n == 1) {
		return n;
	} else {
		return fibonacci(n - 2) + fibonacci(n - 1);
	}
}
